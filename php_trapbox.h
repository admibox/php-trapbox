/* trapbox extension for PHP */

#ifndef PHP_TRAPBOX_H
# define PHP_TRAPBOX_H

extern zend_module_entry trapbox_module_entry;
# define phpext_trapbox_ptr &trapbox_module_entry

# define PHP_TRAPBOX_VERSION "2.0.0-dev"

ZEND_BEGIN_MODULE_GLOBALS(trapbox)
  /* ini settings */
  zend_bool enabled;
  char *log_path;
  char *mode_exec;
  char *mode_eval;
  char *mode_include;
  char *mode_file_write;
  char *mode_secret_read;
  char *mode_unserialize;
  char *mode_errors;
  char *file_write_extensions;
  zend_bool file_write_any_path;
  char *secret_paths;
  char *canary_env_path;
  zend_long max_arg_size;
  zend_long max_body_size;

  /* PHP 7 callback cleanup and fallback deduplication, request-local. */
  void *proxy_abandoned;
  zend_string *proxy_pending_message;
  zend_string *proxy_pending_file;
  int proxy_pending_type;
  uint32_t proxy_pending_line;

  /* per-request state */
  char rid[13];
  int request_captured;
  int in_request;
  int in_error;
  const char *request_method;
  const char *request_uri;
  const char *client_ip;
  HashTable *server_ht;
  zend_string *request_body;
ZEND_END_MODULE_GLOBALS(trapbox)

ZEND_EXTERN_MODULE_GLOBALS(trapbox)
# define TRAPBOX_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(trapbox, v)

/* Event writer — called by each intercept category */
void trapbox_log_event(const char *category, const char *mode, const char *function,
                       const char *const *arg_strs, const size_t *arg_lens, int arg_count);

# if defined(ZTS) && defined(COMPILE_DL_TRAPBOX)
ZEND_TSRMLS_CACHE_EXTERN()
# endif

#endif	/* PHP_TRAPBOX_H */
