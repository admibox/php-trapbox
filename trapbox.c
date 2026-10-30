#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_trapbox.h"
#include "SAPI.h"
#include "zend_smart_str.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/time.h>

/* For compatibility with older PHP versions */
#ifndef ZEND_PARSE_PARAMETERS_NONE
#define ZEND_PARSE_PARAMETERS_NONE() \
  ZEND_PARSE_PARAMETERS_START(0, 0)  \
  ZEND_PARSE_PARAMETERS_END()
#endif

/* ── ini settings ─────────────────────────────────────────────── */

PHP_INI_BEGIN()
  STD_PHP_INI_BOOLEAN("trapbox.enabled", "1", PHP_INI_SYSTEM, OnUpdateBool, enabled, zend_trapbox_globals, trapbox_globals)
  STD_PHP_INI_ENTRY("trapbox.log_path", "/var/log/trapbox/events.jsonl", PHP_INI_SYSTEM, OnUpdateString, log_path, zend_trapbox_globals, trapbox_globals)
  STD_PHP_INI_ENTRY("trapbox.exec", "bypass", PHP_INI_SYSTEM, OnUpdateString, mode_exec, zend_trapbox_globals, trapbox_globals)
  STD_PHP_INI_ENTRY("trapbox.eval", "bypass", PHP_INI_SYSTEM, OnUpdateString, mode_eval, zend_trapbox_globals, trapbox_globals)
  STD_PHP_INI_ENTRY("trapbox.include", "bypass", PHP_INI_SYSTEM, OnUpdateString, mode_include, zend_trapbox_globals, trapbox_globals)
  STD_PHP_INI_ENTRY("trapbox.file_write", "bypass", PHP_INI_SYSTEM, OnUpdateString, mode_file_write, zend_trapbox_globals, trapbox_globals)
  STD_PHP_INI_ENTRY("trapbox.secret_read", "bypass", PHP_INI_SYSTEM, OnUpdateString, mode_secret_read, zend_trapbox_globals, trapbox_globals)
  STD_PHP_INI_ENTRY("trapbox.unserialize", "bypass", PHP_INI_SYSTEM, OnUpdateString, mode_unserialize, zend_trapbox_globals, trapbox_globals)
  STD_PHP_INI_ENTRY("trapbox.errors", "bypass", PHP_INI_SYSTEM, OnUpdateString, mode_errors, zend_trapbox_globals, trapbox_globals)
  STD_PHP_INI_ENTRY("trapbox.file_write_extensions", "php,phtml,inc,htaccess,user.ini,js,html", PHP_INI_SYSTEM, OnUpdateString, file_write_extensions, zend_trapbox_globals, trapbox_globals)
  STD_PHP_INI_BOOLEAN("trapbox.file_write_any_path", "0", PHP_INI_SYSTEM, OnUpdateBool, file_write_any_path, zend_trapbox_globals, trapbox_globals)
  STD_PHP_INI_ENTRY("trapbox.secret_paths", "env.php,.env,wp-config.php,/etc/passwd,/etc/shadow", PHP_INI_SYSTEM, OnUpdateString, secret_paths, zend_trapbox_globals, trapbox_globals)
  STD_PHP_INI_ENTRY("trapbox.max_arg_size", "65536", PHP_INI_SYSTEM, OnUpdateLong, max_arg_size, zend_trapbox_globals, trapbox_globals)
  STD_PHP_INI_ENTRY("trapbox.max_body_size", "1048576", PHP_INI_SYSTEM, OnUpdateLong, max_body_size, zend_trapbox_globals, trapbox_globals)
PHP_INI_END()

/* ── globals ──────────────────────────────────────────────────── */

ZEND_DECLARE_MODULE_GLOBALS(trapbox)

static void php_trapbox_init_globals(zend_trapbox_globals *g)
{
  memset(g, 0, sizeof(*g));
}

/* ── mode parsing ─────────────────────────────────────────────── */

enum trapbox_mode {
  TRAPBOX_MODE_OFF = 0,
  TRAPBOX_MODE_BYPASS,
  TRAPBOX_MODE_BLOCK,
  TRAPBOX_MODE_TRAP
};

static enum trapbox_mode trapbox_parse_mode(const char *str)
{
  /* The ini parser turns an unquoted `off` into an empty string */
  if (!str || !*str || strcmp(str, "off") == 0) return TRAPBOX_MODE_OFF;
  if (strcmp(str, "bypass") == 0) return TRAPBOX_MODE_BYPASS;
  if (strcmp(str, "block") == 0) return TRAPBOX_MODE_BLOCK;
  if (strcmp(str, "trap") == 0) return TRAPBOX_MODE_TRAP;
  /* A typo must not silently disable monitoring */
  return TRAPBOX_MODE_BYPASS;
}

static const char *trapbox_mode_name(enum trapbox_mode mode)
{
  switch (mode) {
    case TRAPBOX_MODE_BYPASS: return "bypass";
    case TRAPBOX_MODE_BLOCK: return "block";
    case TRAPBOX_MODE_TRAP: return "trap";
    default: return "off";
  }
}

static void trapbox_check_mode(const char *setting, const char *value)
{
  if (value && *value && strcmp(value, "off") != 0 && strcmp(value, "bypass") != 0
      && strcmp(value, "block") != 0 && strcmp(value, "trap") != 0) {
    zend_error(E_WARNING, "trapbox: invalid value '%s' for %s, using bypass", value, setting);
  }
}

/* ── correlation id ───────────────────────────────────────────── */

#define TRAPBOX_RID_LEN 12

static void trapbox_generate_rid(char *buf)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);

  unsigned long seed = (unsigned long)tv.tv_sec ^ ((unsigned long)tv.tv_usec << 16)
                       ^ (unsigned long)getpid();

  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < TRAPBOX_RID_LEN; i++) {
    seed = seed * 6364136223846793005UL + 1442695040888963407UL;
    buf[i] = hex[(seed >> 32) & 0xf];
  }
  buf[TRAPBOX_RID_LEN] = '\0';
}

/* ── json helpers ─────────────────────────────────────────────── */

static void trapbox_json_append_escaped(smart_str *buf, const char *str, size_t len, size_t max_len)
{
  if (max_len > 0 && len > max_len) {
    len = max_len;
  }
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)str[i];
    switch (c) {
      case '"':  smart_str_appendl(buf, "\\\"", 2); break;
      case '\\': smart_str_appendl(buf, "\\\\", 2); break;
      case '\n': smart_str_appendl(buf, "\\n", 2); break;
      case '\r': smart_str_appendl(buf, "\\r", 2); break;
      case '\t': smart_str_appendl(buf, "\\t", 2); break;
      default:
        /* Bytes >= 0x80 map to U+00XX so binary payloads stay valid JSON and recoverable */
        if (c < 0x20 || c >= 0x7f) {
          char esc[7];
          snprintf(esc, sizeof(esc), "\\u%04x", c);
          smart_str_appendl(buf, esc, 6);
        } else {
          smart_str_appendc(buf, c);
        }
    }
  }
}

static void trapbox_json_key_str(smart_str *buf, const char *key, const char *val, size_t val_len, size_t max_len)
{
  smart_str_appendc(buf, '"');
  smart_str_appends(buf, key);
  smart_str_appendl(buf, "\":\"", 3);
  trapbox_json_append_escaped(buf, val, val_len, max_len);
  smart_str_appendc(buf, '"');
}

static void trapbox_json_key_long(smart_str *buf, const char *key, zend_long val)
{
  char num[21];
  int num_len = snprintf(num, sizeof(num), ZEND_LONG_FMT, val);
  smart_str_appendc(buf, '"');
  smart_str_appends(buf, key);
  smart_str_appendl(buf, "\":", 2);
  smart_str_appendl(buf, num, num_len);
}

/* ── request context (lazy capture) ───────────────────────────── */

static void trapbox_capture_request(void)
{
  if (TRAPBOX_G(request_captured)) return;
  TRAPBOX_G(request_captured) = 1;

  TRAPBOX_G(request_method) = SG(request_info).request_method;
  TRAPBOX_G(request_uri) = SG(request_info).request_uri;

  /* Read headers from $_SERVER */
  zval *server = NULL;
  zend_is_auto_global_str("_SERVER", sizeof("_SERVER") - 1);
  server = &PG(http_globals)[TRACK_VARS_SERVER];
  if (!server || Z_TYPE_P(server) != IS_ARRAY) return;

  TRAPBOX_G(server_ht) = Z_ARRVAL_P(server);

  /* Forwarding headers are attacker-controlled, so they are logged as headers, never as the ip */
  zval *val;
  if ((val = zend_hash_str_find(TRAPBOX_G(server_ht), "REMOTE_ADDR", sizeof("REMOTE_ADDR") - 1)) && Z_TYPE_P(val) == IS_STRING) {
    TRAPBOX_G(client_ip) = Z_STRVAL_P(val);
  }

  /* Request body from php://input */
  php_stream *stream = php_stream_open_wrapper("php://input", "rb", REPORT_ERRORS, NULL);
  if (stream) {
    zend_string *body = php_stream_copy_to_mem(stream, TRAPBOX_G(max_body_size), 0);
    php_stream_close(stream);
    if (body) {
      TRAPBOX_G(request_body) = body;
    }
  }
}

/* ── backtrace capture ────────────────────────────────────────── */

static void trapbox_append_backtrace(smart_str *buf)
{
  zend_execute_data *frame = EG(current_execute_data);
  int first = 1;

  smart_str_appendl(buf, "\"backtrace\":[", 13);

  while (frame) {
    if (!frame->func || !ZEND_USER_CODE(frame->func->type)) {
      frame = frame->prev_execute_data;
      continue;
    }
    if (!first) smart_str_appendc(buf, ',');
    first = 0;

    smart_str_appendl(buf, "{", 1);

    const char *filename = ZSTR_VAL(frame->func->op_array.filename);
    trapbox_json_key_str(buf, "file", filename, strlen(filename), 0);
    smart_str_appendc(buf, ',');

    trapbox_json_key_long(buf, "line", frame->opline ? frame->opline->lineno : 0);
    smart_str_appendc(buf, ',');

    const char *fn = frame->func->common.function_name
      ? ZSTR_VAL(frame->func->common.function_name) : "{main}";
    trapbox_json_key_str(buf, "function", fn, strlen(fn), 0);

    smart_str_appendc(buf, '}');
    frame = frame->prev_execute_data;
  }

  smart_str_appendc(buf, ']');
}

/* ── event writer ─────────────────────────────────────────────── */

static void trapbox_request_field(smart_str *buf, int *first, const char *key, const char *val, size_t len, size_t max_len)
{
  if (!*first) smart_str_appendc(buf, ',');
  *first = 0;
  trapbox_json_key_str(buf, key, val, len, max_len);
}

static void trapbox_append_headers(smart_str *buf, int *first)
{
  zend_string *key;
  zval *val;
  int first_header = 1;

  if (!TRAPBOX_G(server_ht)) return;

  if (!*first) smart_str_appendc(buf, ',');
  *first = 0;
  smart_str_appendl(buf, "\"headers\":{", 11);

  ZEND_HASH_FOREACH_STR_KEY_VAL(TRAPBOX_G(server_ht), key, val) {
    if (!key || ZSTR_LEN(key) <= 5 || strncmp(ZSTR_VAL(key), "HTTP_", 5) != 0 || Z_TYPE_P(val) != IS_STRING) continue;
    /* Credentials of legitimate users do not belong in a forensic log */
    if (strcmp(ZSTR_VAL(key), "HTTP_COOKIE") == 0 || strcmp(ZSTR_VAL(key), "HTTP_AUTHORIZATION") == 0) continue;

    if (!first_header) smart_str_appendc(buf, ',');
    first_header = 0;

    smart_str_appendc(buf, '"');
    for (size_t i = 5; i < ZSTR_LEN(key); i++) {
      char c = ZSTR_VAL(key)[i];
      smart_str_appendc(buf, c == '_' ? '-' : (char)tolower((unsigned char)c));
    }
    smart_str_appendl(buf, "\":\"", 3);
    trapbox_json_append_escaped(buf, Z_STRVAL_P(val), Z_STRLEN_P(val), TRAPBOX_G(max_arg_size));
    smart_str_appendc(buf, '"');
  } ZEND_HASH_FOREACH_END();

  smart_str_appendc(buf, '}');
}

void trapbox_log_event(const char *category, const char *mode, const char *function,
                       const char *const *arg_strs, const size_t *arg_lens, int arg_count)
{
  if (!TRAPBOX_G(enabled)) return;

  const char *log_path = TRAPBOX_G(log_path);
  if (!log_path || !*log_path) return;

  trapbox_capture_request();

  smart_str buf = {0};
  smart_str_appendc(&buf, '{');

  /* ts */
  struct timeval tv;
  gettimeofday(&tv, NULL);
  char ts[40];
  struct tm tm_buf;
  gmtime_r(&tv.tv_sec, &tm_buf);
  int ts_len = strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_buf);
  ts_len += snprintf(ts + ts_len, sizeof(ts) - ts_len, ".%03dZ", (int)(tv.tv_usec / 1000));
  trapbox_json_key_str(&buf, "ts", ts, ts_len, 0);
  smart_str_appendc(&buf, ',');

  /* rid */
  trapbox_json_key_str(&buf, "rid", TRAPBOX_G(rid), TRAPBOX_RID_LEN, 0);
  smart_str_appendc(&buf, ',');

  /* category, mode, function */
  trapbox_json_key_str(&buf, "category", category, strlen(category), 0);
  smart_str_appendc(&buf, ',');
  trapbox_json_key_str(&buf, "mode", mode, strlen(mode), 0);
  smart_str_appendc(&buf, ',');
  trapbox_json_key_str(&buf, "function", function, strlen(function), 0);
  smart_str_appendc(&buf, ',');

  /* args */
  smart_str_appendl(&buf, "\"args\":[", 8);
  for (int i = 0; i < arg_count; i++) {
    if (i > 0) smart_str_appendc(&buf, ',');
    smart_str_appendc(&buf, '"');
    trapbox_json_append_escaped(&buf, arg_strs[i], arg_lens[i], TRAPBOX_G(max_arg_size));
    smart_str_appendc(&buf, '"');
  }
  smart_str_appendl(&buf, "],", 2);

  /* request */
  smart_str_appendl(&buf, "\"request\":{", 11);
  int first_field = 1;
  if (TRAPBOX_G(request_method)) {
    trapbox_request_field(&buf, &first_field, "method", TRAPBOX_G(request_method), strlen(TRAPBOX_G(request_method)), 0);
  }
  if (TRAPBOX_G(request_uri)) {
    trapbox_request_field(&buf, &first_field, "uri", TRAPBOX_G(request_uri), strlen(TRAPBOX_G(request_uri)), 0);
  }
  if (TRAPBOX_G(client_ip)) {
    trapbox_request_field(&buf, &first_field, "ip", TRAPBOX_G(client_ip), strlen(TRAPBOX_G(client_ip)), 0);
  }
  trapbox_append_headers(&buf, &first_field);
  if (TRAPBOX_G(request_body)) {
    trapbox_request_field(&buf, &first_field, "body", ZSTR_VAL(TRAPBOX_G(request_body)), ZSTR_LEN(TRAPBOX_G(request_body)), TRAPBOX_G(max_body_size));
  }
  smart_str_appendl(&buf, "},", 2);

  /* backtrace */
  trapbox_append_backtrace(&buf);

  smart_str_appendc(&buf, '}');
  smart_str_appendc(&buf, '\n');
  smart_str_0(&buf);

  /* Write with flock */
  int fd = open(log_path, O_WRONLY | O_APPEND | O_CREAT, 0640);
  if (fd >= 0) {
    flock(fd, LOCK_EX);
    write(fd, ZSTR_VAL(buf.s), ZSTR_LEN(buf.s));
    flock(fd, LOCK_UN);
    close(fd);
  }

  smart_str_free(&buf);
}

/* ── function table interception ──────────────────────────────── */

/*
 * Handlers are patched once in MINIT and never restored. A per-request swap of
 * function table entries crashes under JIT: compiled code outlives the request
 * and keeps the pointer to the freed entry.
 */
typedef struct _trapbox_hook {
  const char *name;
  zif_handler original;
} trapbox_hook;

static trapbox_hook trapbox_exec_hooks[] = {
  {"exec", NULL}, {"system", NULL}, {"shell_exec", NULL}, {"passthru", NULL},
  {"popen", NULL}, {"proc_open", NULL}, {"pcntl_exec", NULL}, {NULL, NULL}
};

static zif_handler trapbox_original_handler(const trapbox_hook *hooks, const char *name)
{
  for (const trapbox_hook *hook = hooks; hook->name; hook++) {
    if (strcmp(hook->name, name) == 0) return hook->original;
  }
  return NULL;
}

static zend_string *trapbox_zstr_arg(zval *arg)
{
  if (Z_TYPE_P(arg) == IS_STRING) {
    return Z_STR_P(arg);
  }
  return NULL;
}

ZEND_FUNCTION(trapbox_exec_handler)
{
  enum trapbox_mode mode = trapbox_parse_mode(TRAPBOX_G(mode_exec));
  const char *fn_name = ZSTR_VAL(execute_data->func->common.function_name);
  zif_handler original = trapbox_original_handler(trapbox_exec_hooks, fn_name);

  /* Collect first argument as the command string for logging */
  const char *arg_strs[1] = { "" };
  size_t arg_lens[1] = { 0 };
  int arg_count = 0;

  uint32_t num_args = ZEND_NUM_ARGS();
  if (num_args > 0) {
    zval *args = safe_emalloc(num_args, sizeof(zval), 0);
    if (zend_get_parameters_array_ex(num_args, args) == SUCCESS) {
      zend_string *cmd = trapbox_zstr_arg(&args[0]);
      if (cmd) {
        arg_strs[0] = ZSTR_VAL(cmd);
        arg_lens[0] = ZSTR_LEN(cmd);
        arg_count = 1;
      }
    }
    efree(args);
  }

  trapbox_log_event("exec", trapbox_mode_name(mode), fn_name, arg_strs, arg_lens, arg_count);

  if (mode == TRAPBOX_MODE_BYPASS) {
    original(INTERNAL_FUNCTION_PARAM_PASSTHRU);
    return;
  }

  /* block or trap: return false */
  RETURN_FALSE;
}

/* ── eval interception (zend_compile_string) ──────────────────── */

#if PHP_VERSION_ID >= 80200
typedef zend_op_array *(*trapbox_compile_string_t)(zend_string *source, const char *filename, zend_compile_position position);
#elif PHP_VERSION_ID >= 80000
typedef zend_op_array *(*trapbox_compile_string_t)(zend_string *source, const char *filename);
#else
typedef zend_op_array *(*trapbox_compile_string_t)(zval *source, char *filename);
#endif

static trapbox_compile_string_t trapbox_prev_compile_string = NULL;

/* Logs the eval and returns the mode. The filename names the construct: eval()'d code, assert code, runtime-created function */
static enum trapbox_mode trapbox_eval_mode(const char *source, size_t source_len, const char *filename)
{
  enum trapbox_mode mode = trapbox_parse_mode(TRAPBOX_G(mode_eval));
  const char *arg_strs[2] = { source, filename ? filename : "" };
  size_t arg_lens[2] = { source_len, filename ? strlen(filename) : 0 };
  trapbox_log_event("eval", trapbox_mode_name(mode), "eval", arg_strs, arg_lens, 2);
  return mode;
}

/* block returns NULL without an exception: the engine then evaluates eval() to false, like a failed compile.
   trap compiles `return;` instead of the payload: eval() returns null, as if the code ran and returned nothing.
   An empty source would not do: compile_string returns NULL for it, which is block. */
#define TRAPBOX_TRAP_SOURCE "return;"

#if PHP_VERSION_ID >= 80200
static zend_op_array *trapbox_compile_string(zend_string *source, const char *filename, zend_compile_position position)
{
  enum trapbox_mode mode = trapbox_eval_mode(ZSTR_VAL(source), ZSTR_LEN(source), filename);
  if (mode == TRAPBOX_MODE_BLOCK) return NULL;
  if (mode == TRAPBOX_MODE_TRAP) {
    zend_string *trap = zend_string_init(TRAPBOX_TRAP_SOURCE, sizeof(TRAPBOX_TRAP_SOURCE) - 1, 0);
    zend_op_array *op_array = trapbox_prev_compile_string(trap, filename, position);
    zend_string_release(trap);
    return op_array;
  }
  return trapbox_prev_compile_string(source, filename, position);
}
#elif PHP_VERSION_ID >= 80000
static zend_op_array *trapbox_compile_string(zend_string *source, const char *filename)
{
  enum trapbox_mode mode = trapbox_eval_mode(ZSTR_VAL(source), ZSTR_LEN(source), filename);
  if (mode == TRAPBOX_MODE_BLOCK) return NULL;
  if (mode == TRAPBOX_MODE_TRAP) {
    zend_string *trap = zend_string_init(TRAPBOX_TRAP_SOURCE, sizeof(TRAPBOX_TRAP_SOURCE) - 1, 0);
    zend_op_array *op_array = trapbox_prev_compile_string(trap, filename);
    zend_string_release(trap);
    return op_array;
  }
  return trapbox_prev_compile_string(source, filename);
}
#else
static zend_op_array *trapbox_compile_string(zval *source, char *filename)
{
  if (Z_TYPE_P(source) != IS_STRING) return trapbox_prev_compile_string(source, filename);
  enum trapbox_mode mode = trapbox_eval_mode(Z_STRVAL_P(source), Z_STRLEN_P(source), filename);
  if (mode == TRAPBOX_MODE_BLOCK) return NULL;
  if (mode == TRAPBOX_MODE_TRAP) {
    zval trap;
    ZVAL_STRINGL(&trap, TRAPBOX_TRAP_SOURCE, sizeof(TRAPBOX_TRAP_SOURCE) - 1);
    zend_op_array *op_array = trapbox_prev_compile_string(&trap, filename);
    zval_ptr_dtor(&trap);
    return op_array;
  }
  return trapbox_prev_compile_string(source, filename);
}
#endif

/* ── include interception (zend_compile_file) ─────────────────── */

static zend_op_array *(*trapbox_prev_compile_file)(zend_file_handle *file_handle, int type) = NULL;

/* Any stream wrapper except file://, so wrappers we did not think of are still caught */
static int trapbox_is_wrapper_path(const char *path)
{
  const char *p = path;
  if (strncasecmp(path, "data:", 5) == 0) return 1;
  while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.') p++;
  if (p == path || strncmp(p, "://", 3) != 0) return 0;
  return !((p - path) == 4 && strncasecmp(path, "file", 4) == 0);
}

static zend_op_array *trapbox_compile_trap(const char *filename)
{
  zend_op_array *op_array;
#if PHP_VERSION_ID >= 80000
  zend_string *trap = zend_string_init("return 1;", sizeof("return 1;") - 1, 0);
# if PHP_VERSION_ID >= 80200
  op_array = trapbox_prev_compile_string(trap, filename, ZEND_COMPILE_POSITION_AFTER_OPEN_TAG);
# else
  op_array = trapbox_prev_compile_string(trap, filename);
# endif
  zend_string_release(trap);
#else
  zval trap;
  ZVAL_STRINGL(&trap, "return 1;", sizeof("return 1;") - 1);
  op_array = trapbox_prev_compile_string(&trap, (char *)filename);
  zval_ptr_dtor(&trap);
#endif
  return op_array;
}

static zend_op_array *trapbox_compile_file(zend_file_handle *file_handle, int type)
{
#if PHP_VERSION_ID >= 80100
  const char *filename = file_handle->filename ? ZSTR_VAL(file_handle->filename) : NULL;
#else
  const char *filename = file_handle->filename;
#endif

  if (!filename || !trapbox_is_wrapper_path(filename)) {
    return trapbox_prev_compile_file(file_handle, type);
  }

  enum trapbox_mode mode = trapbox_parse_mode(TRAPBOX_G(mode_include));
  const char *arg_strs[1] = { filename };
  size_t arg_lens[1] = { strlen(filename) };
  trapbox_log_event("include", trapbox_mode_name(mode), "include", arg_strs, arg_lens, 1);

  /* include evaluates to false on block. require does not halt: the caller sees false too */
  if (mode == TRAPBOX_MODE_BLOCK) return NULL;
  /* trap mimics a successful include of a file with no return statement */
  if (mode == TRAPBOX_MODE_TRAP) return trapbox_compile_trap(filename);
  return trapbox_prev_compile_file(file_handle, type);
}

static void trapbox_install_hooks(trapbox_hook *hooks, zif_handler handler)
{
  for (trapbox_hook *hook = hooks; hook->name; hook++) {
    zend_function *func = zend_hash_str_find_ptr(CG(function_table), hook->name, strlen(hook->name));
    if (func && func->type == ZEND_INTERNAL_FUNCTION) {
      hook->original = func->internal_function.handler;
      func->internal_function.handler = handler;
    }
  }
}

/* ── module lifecycle ─────────────────────────────────────────── */

static const zend_function_entry trapbox_functions[] = {
    PHP_FE_END
};

PHP_MINIT_FUNCTION(trapbox)
{
  ZEND_INIT_MODULE_GLOBALS(trapbox, php_trapbox_init_globals, NULL);
  REGISTER_INI_ENTRIES();
  trapbox_check_mode("trapbox.exec", TRAPBOX_G(mode_exec));
  trapbox_check_mode("trapbox.eval", TRAPBOX_G(mode_eval));
  trapbox_check_mode("trapbox.include", TRAPBOX_G(mode_include));
  trapbox_check_mode("trapbox.file_write", TRAPBOX_G(mode_file_write));
  trapbox_check_mode("trapbox.secret_read", TRAPBOX_G(mode_secret_read));
  trapbox_check_mode("trapbox.unserialize", TRAPBOX_G(mode_unserialize));
  trapbox_check_mode("trapbox.errors", TRAPBOX_G(mode_errors));

  /* These settings are PHP_INI_SYSTEM, so the decision holds for the whole process */
  if (TRAPBOX_G(enabled) && trapbox_parse_mode(TRAPBOX_G(mode_exec)) != TRAPBOX_MODE_OFF) {
    trapbox_install_hooks(trapbox_exec_hooks, ZEND_FN(trapbox_exec_handler));
  }
  trapbox_prev_compile_string = zend_compile_string;
  if (TRAPBOX_G(enabled) && trapbox_parse_mode(TRAPBOX_G(mode_eval)) != TRAPBOX_MODE_OFF) {
    zend_compile_string = trapbox_compile_string;
  }
  return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(trapbox)
{
  if (zend_compile_string == trapbox_compile_string) {
    zend_compile_string = trapbox_prev_compile_string;
  }
  if (zend_compile_file == trapbox_compile_file) {
    zend_compile_file = trapbox_prev_compile_file;
  }
  UNREGISTER_INI_ENTRIES();
  return SUCCESS;
}

PHP_RINIT_FUNCTION(trapbox)
{
#if defined(ZTS) && defined(COMPILE_DL_TRAPBOX)
  ZEND_TSRMLS_CACHE_UPDATE();
#endif
  trapbox_generate_rid(TRAPBOX_G(rid));
  TRAPBOX_G(request_captured) = 0;
  TRAPBOX_G(request_method) = NULL;
  TRAPBOX_G(request_uri) = NULL;
  TRAPBOX_G(client_ip) = NULL;
  TRAPBOX_G(server_ht) = NULL;
  TRAPBOX_G(request_body) = NULL;

  /* Installed here rather than in MINIT: OPcache hooks after MINIT and would sit in front,
     hiding cached phar:// includes. Runs once per process. */
  if (!trapbox_prev_compile_file && TRAPBOX_G(enabled)
      && trapbox_parse_mode(TRAPBOX_G(mode_include)) != TRAPBOX_MODE_OFF) {
    trapbox_prev_compile_file = zend_compile_file;
    zend_compile_file = trapbox_compile_file;
  }
  return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(trapbox)
{
  if (TRAPBOX_G(request_body)) {
    zend_string_release(TRAPBOX_G(request_body));
    TRAPBOX_G(request_body) = NULL;
  }
  return SUCCESS;
}

zend_module_entry trapbox_module_entry = {
  STANDARD_MODULE_HEADER,
  "trapbox",
  trapbox_functions,
  PHP_MINIT(trapbox),
  PHP_MSHUTDOWN(trapbox),
  PHP_RINIT(trapbox),
  PHP_RSHUTDOWN(trapbox),
  NULL,
  PHP_TRAPBOX_VERSION,
  STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_TRAPBOX
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(trapbox)
#endif
