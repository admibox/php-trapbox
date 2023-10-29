#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "php_trapbox.h"
#include "zend_closures.h"

HashTable replaced_functions;
HashTable closures;

typedef struct _intercepted_function
{
  zend_function func;
  zend_function *original_func;
  zend_string *function_name;
} intercepted_function;

/* For compatibility with older PHP versions */
#ifndef ZEND_PARSE_PARAMETERS_NONE
#define ZEND_PARSE_PARAMETERS_NONE() \
  ZEND_PARSE_PARAMETERS_START(0, 0)  \
  ZEND_PARSE_PARAMETERS_END()
#endif

ZEND_BEGIN_ARG_INFO(arginfo_trapbox_intercept, 0)
ZEND_ARG_INFO(0, function_name)
ZEND_ARG_INFO(0, closure)
ZEND_END_ARG_INFO()

ZEND_FUNCTION(replacement_function)
{
  zval retval;
  ZVAL_UNDEF(&retval);
  char *error = NULL;

  uint32_t num_args = ZEND_NUM_ARGS();
  zval *args = emalloc(num_args * sizeof(zval));
  zval original_func_zval;

  if (zend_get_parameters_array_ex(num_args, args) == FAILURE)
  {
    efree(args);
    RETURN_STRING("Failed to get arguments");
  }

  intercepted_function *replacement = (intercepted_function *)execute_data->func;
  zend_function *original_func = replacement->original_func;

  // Create an anonymous function (closure) that wraps around the original function
  zval original_closure;
  object_init_ex(&original_closure, zend_ce_closure);
  zend_create_closure(&original_closure, original_func, NULL, NULL, NULL);

  // Create a callable array [closure]
  ZVAL_COPY(&original_func_zval, &original_closure);
  add_next_index_zval(&original_func_zval, &original_closure);

  zval *closure = zend_hash_find(&closures, replacement->function_name);

  zend_fcall_info fci = empty_fcall_info;
  zend_fcall_info_cache fci_cache = empty_fcall_info_cache;

  zend_fcall_info_init(closure, 0, &fci, &fci_cache, NULL, &error);

  fci.retval = &retval;
  fci.param_count = num_args + 1; // Adding 1 for the original function callable
  fci.params = safe_emalloc((num_args + 1), sizeof(zval), 0);
  ZVAL_COPY_VALUE(&fci.params[0], &original_func_zval);
  for (uint32_t i = 0; i < num_args; ++i)
  {
    ZVAL_COPY_VALUE(&fci.params[i + 1], &args[i]);
  }
  fci.size = sizeof(fci);

  if (zend_call_function(&fci, &fci_cache) != SUCCESS)
  {
    php_printf("DEBUG: zend_call_function failed\n");
    RETURN_STRING("Closure call failed");
  }

  zval_ptr_dtor(&original_func_zval);
  efree(args);
  efree(fci.params);

  RETURN_ZVAL(&retval, 0, 0);
}

// Function to intercept and replace another function
PHP_FUNCTION(trapbox_intercept)
{
  char *function_name;
  size_t function_name_len;
  zval *closure;
  zend_string *function_name_str;
  zval *original_function_zval;

  if (zend_parse_parameters(ZEND_NUM_ARGS(), "sz", &function_name, &function_name_len, &closure) == FAILURE)
  {
    return;
  }

  function_name_str = zend_string_init(function_name, function_name_len, 0);

  if ((original_function_zval = zend_hash_find(EG(function_table), function_name_str)) == NULL)
  {
    php_error_docref(NULL, E_WARNING, "Function %s() does not exist", ZSTR_VAL(function_name_str));
    zend_string_release(function_name_str);
    return;
  }

  Z_ADDREF_P(closure);
  zend_hash_add(&closures, function_name_str, closure);

  // Store the original function in the HashTable
  zend_hash_add_ptr(&replaced_functions, function_name_str, Z_PTR_P(original_function_zval));

  intercepted_function *replacement = emalloc(sizeof(intercepted_function));

  zend_function *original_func_copy = emalloc(sizeof(zend_function));
  memcpy(original_func_copy, Z_PTR_P(original_function_zval), sizeof(zend_function));

  // Assign original_func to original_func_copy
  replacement->original_func = original_func_copy;

  memcpy(&replacement->func, Z_PTR_P(original_function_zval), sizeof(zend_function));
  replacement->function_name = zend_string_copy(function_name_str);
  replacement->func.internal_function.handler = ZEND_FN(replacement_function);

  if (zend_hash_update_ptr(EG(function_table), function_name_str, replacement) == NULL)
  {
    php_error_docref(NULL, E_WARNING, "Unable to replace function %s()", ZSTR_VAL(function_name_str));
    efree(replacement);
  }

  zend_string_release(function_name_str);
}

PHP_RINIT_FUNCTION(trapbox)
{
#if defined(ZTS) && defined(COMPILE_DL_TRAPBOX)
  ZEND_TSRMLS_CACHE_UPDATE();
#endif

  return SUCCESS;
}

PHP_MINFO_FUNCTION(trapbox)
{
  php_info_print_table_start();
  php_info_print_table_header(2, "trapbox support", "enabled");
  php_info_print_table_end();
}

static const zend_function_entry trapbox_functions[] = {
    PHP_FE(trapbox_intercept, arginfo_trapbox_intercept)
        PHP_FE_END};

PHP_MSHUTDOWN_FUNCTION(trapbox)
{
  intercepted_function *func;

  ZEND_HASH_FOREACH_PTR(&closures, func)
  {
    efree(func->original_func);
    zend_string_release(func->function_name);
    efree(func);
  }
  ZEND_HASH_FOREACH_END();

  zend_hash_destroy(&replaced_functions);
  return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(trapbox)
{
  return SUCCESS;
}

PHP_MINIT_FUNCTION(trapbox)
{
  zend_hash_init(&replaced_functions, 8, NULL, NULL, 1);
  zend_hash_init(&closures, 8, NULL, ZVAL_PTR_DTOR, 1);

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
    PHP_MINFO(trapbox),
    PHP_TRAPBOX_VERSION,
    STANDARD_MODULE_PROPERTIES
  };

#ifdef COMPILE_DL_TRAPBOX
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(trapbox)
#endif
