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
  STD_PHP_INI_ENTRY("trapbox.exec", "block", PHP_INI_SYSTEM, OnUpdateString, mode_exec, zend_trapbox_globals, trapbox_globals)
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
  if (!str) return TRAPBOX_MODE_OFF;
  if (strcmp(str, "bypass") == 0) return TRAPBOX_MODE_BYPASS;
  if (strcmp(str, "block") == 0) return TRAPBOX_MODE_BLOCK;
  if (strcmp(str, "trap") == 0) return TRAPBOX_MODE_TRAP;
  if (strcmp(str, "off") == 0) return TRAPBOX_MODE_OFF;
  return TRAPBOX_MODE_OFF;
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
        if (c < 0x20) {
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

  /* Client IP: CF-Connecting-IP > X-Forwarded-For > REMOTE_ADDR */
  zval *val;
  if ((val = zend_hash_str_find(TRAPBOX_G(server_ht), "HTTP_CF_CONNECTING_IP", sizeof("HTTP_CF_CONNECTING_IP") - 1)) && Z_TYPE_P(val) == IS_STRING) {
    TRAPBOX_G(client_ip) = Z_STRVAL_P(val);
  } else if ((val = zend_hash_str_find(TRAPBOX_G(server_ht), "HTTP_X_FORWARDED_FOR", sizeof("HTTP_X_FORWARDED_FOR") - 1)) && Z_TYPE_P(val) == IS_STRING) {
    TRAPBOX_G(client_ip) = Z_STRVAL_P(val);
  } else if ((val = zend_hash_str_find(TRAPBOX_G(server_ht), "REMOTE_ADDR", sizeof("REMOTE_ADDR") - 1)) && Z_TYPE_P(val) == IS_STRING) {
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
  if (TRAPBOX_G(request_method)) {
    trapbox_json_key_str(&buf, "method", TRAPBOX_G(request_method), strlen(TRAPBOX_G(request_method)), 0);
    smart_str_appendc(&buf, ',');
  }
  if (TRAPBOX_G(request_uri)) {
    trapbox_json_key_str(&buf, "uri", TRAPBOX_G(request_uri), strlen(TRAPBOX_G(request_uri)), 0);
    smart_str_appendc(&buf, ',');
  }
  if (TRAPBOX_G(client_ip)) {
    trapbox_json_key_str(&buf, "ip", TRAPBOX_G(client_ip), strlen(TRAPBOX_G(client_ip)), 0);
  }
  if (TRAPBOX_G(request_body)) {
    smart_str_appendc(&buf, ',');
    trapbox_json_key_str(&buf, "body", ZSTR_VAL(TRAPBOX_G(request_body)), ZSTR_LEN(TRAPBOX_G(request_body)), TRAPBOX_G(max_body_size));
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

/* ── module lifecycle ─────────────────────────────────────────── */

static const zend_function_entry trapbox_functions[] = {
    PHP_FE_END
};

PHP_MINIT_FUNCTION(trapbox)
{
  ZEND_INIT_MODULE_GLOBALS(trapbox, php_trapbox_init_globals, NULL);
  REGISTER_INI_ENTRIES();
  return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(trapbox)
{
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
