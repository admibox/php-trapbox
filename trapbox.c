#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "php_trapbox.h"
#include "zend_closures.h"

HashTable replaced_functions;
HashTable closures;

static zval exit_handler;
static int exit_handler_set = 0;
static int sealed = 0;
#if PHP_VERSION_ID >= 80400
static int exit_intercepted_84 = 0;  // Track if exit was intercepted on PHP 8.4+
#endif

// Thread-local storage for call context
__thread int internal_call_context = 0;

typedef struct _intercepted_function
{
  zend_function func;
  zend_function *original_func;
  // zend_string *function_name;
} intercepted_function;

/* For compatibility with older PHP versions */
#ifndef ZEND_PARSE_PARAMETERS_NONE
#define ZEND_PARSE_PARAMETERS_NONE() \
  ZEND_PARSE_PARAMETERS_START(0, 0)  \
  ZEND_PARSE_PARAMETERS_END()
#endif

ZEND_BEGIN_ARG_INFO(arginfo_trapbox_set_exit_handler, 0)
    ZEND_ARG_CALLABLE_INFO(0, handler, 0) // Expect a callable as the argument
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_trapbox_intercept, 0)
ZEND_ARG_INFO(0, function_name)
ZEND_ARG_INFO(0, closure)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_trapbox_seal, 0)
ZEND_END_ARG_INFO()




// PHP < 8.4: exit is an opcode, intercept via opcode handler
// PHP >= 8.4: exit is a function, intercept via trapbox_intercept
#if PHP_VERSION_ID < 80400
int trapbox_exit_handler(zend_execute_data *execute_data) {
    zval retval;
    zval args[1];
    zval *status = NULL;
    const zend_op *opline = execute_data->opline;

    // Get the exit status from the opcode operand
    if (opline->op1_type == IS_CONST) {
        status = RT_CONSTANT(opline, opline->op1);
    } else if (opline->op1_type != IS_UNUSED) {
        status = EX_VAR(opline->op1.var);
    }

    if (exit_handler_set && Z_TYPE(exit_handler) == IS_OBJECT) {
        ZVAL_NULL(&retval);

        if (status) {
            ZVAL_COPY(&args[0], status);
        } else {
            ZVAL_LONG(&args[0], 0);
        }

        if (call_user_function(EG(function_table), NULL, &exit_handler, &retval, 1, args) == FAILURE) {
            php_printf("[Trapbox] Failed to invoke exit handler\n");
        }

        zval_ptr_dtor(&args[0]);
        zval_ptr_dtor(&retval);
    } else {
        php_printf("[Trapbox] No handler set for exit\n");
    }

    // Prevent actual exit
    return ZEND_USER_OPCODE_RETURN;
}
#endif




// Utility functions to manage context
void enter_internal_call() {
    internal_call_context++;
}

void exit_internal_call() {
    if (internal_call_context > 0) {
        internal_call_context--;
    }
}

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

  zend_string *original_function_name = execute_data->func->common.function_name;

#if PHP_VERSION_ID >= 80400
  // Special handling for exit() when using trapbox_set_exit_handler on PHP 8.4+
  if (exit_handler_set &&
      ZSTR_LEN(original_function_name) == 4 &&
      memcmp(ZSTR_VAL(original_function_name), "exit", 4) == 0) {

      zval handler_retval;
      zval handler_args[1];

      ZVAL_NULL(&handler_retval);
      if (num_args > 0) {
          ZVAL_COPY(&handler_args[0], &args[0]);
      } else {
          ZVAL_LONG(&handler_args[0], 0);
      }

      call_user_function(EG(function_table), NULL, &exit_handler, &handler_retval, 1, handler_args);

      zval_ptr_dtor(&handler_args[0]);
      efree(args);

      RETURN_ZVAL(&handler_retval, 0, 1);
  }
#endif

  intercepted_function *replacement = (intercepted_function *)execute_data->func;
  zend_function *original_func = replacement->original_func;

  zend_fcall_info fci = empty_fcall_info;
  zend_fcall_info_cache fci_cache = empty_fcall_info_cache;

    if (internal_call_context > 0) {
        // Directly call the original function

      fci.size = sizeof(fci);
      fci.retval = &retval;
      fci.param_count = num_args;
      fci.params = args;
      fci_cache.function_handler = original_func;
      fci_cache.called_scope = original_func->common.scope;
      fci_cache.object = NULL;

      if (zend_call_function(&fci, &fci_cache) == FAILURE) {
          php_printf("Failed to call original function\n");
          RETURN_FALSE;
      }

    } else {
      // Create an anonymous function (closure) that wraps around the original function
      zval original_closure;
      object_init_ex(&original_closure, zend_ce_closure);
      zend_create_closure(&original_closure, original_func, NULL, NULL, NULL);

      // Create a callable array [closure]
      ZVAL_COPY(&original_func_zval, &original_closure);
      // add_next_index_zval(&original_func_zval, &original_closure);

      zval *closure = zend_hash_find(&closures, original_function_name);

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

      enter_internal_call();
      if (zend_call_function(&fci, &fci_cache) != SUCCESS)
      {
        exit_internal_call();
        php_printf("DEBUG: zend_call_function failed\n");
        RETURN_STRING("Closure call failed");
      }
      exit_internal_call();

      zval_ptr_dtor(&original_func_zval);
  }

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

  if (sealed) {
      php_error_docref(NULL, E_WARNING, "No further interceptions allowed");
      return;
  }

  if (zend_parse_parameters(ZEND_NUM_ARGS(), "sz", &function_name, &function_name_len, &closure) == FAILURE)
  {
    return;
  }

  function_name_str = zend_string_init(function_name, function_name_len, 0);

  if (strcmp(function_name, "trapbox_intercept") == 0) {
      php_error_docref(NULL, E_WARNING, "trapbox_intercept cannot be intercepted");
      return;
  }

  if (zend_hash_exists(&replaced_functions, function_name_str)) {
      php_error_docref(NULL, E_WARNING, "Function %s() is already intercepted", ZSTR_VAL(function_name_str));
      return;
  }

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
  replacement->func.common.function_name = zend_string_copy(function_name_str);
  replacement->func.internal_function.handler = ZEND_FN(replacement_function);

  zend_hash_del(EG(function_table), function_name_str);

  if (zend_hash_add_ptr(EG(function_table), function_name_str, replacement) == NULL)
  {
    php_error_docref(NULL, E_WARNING, "Unable to replace function %s()", ZSTR_VAL(function_name_str));
    efree(replacement);
  }

  zend_string_release(function_name_str);
}

PHP_FUNCTION(trapbox_seal)
{
  ZEND_PARSE_PARAMETERS_NONE(); // No parameters are expected
  sealed = 1;


  zend_string *name_trapbox_intercept = zend_string_init("trapbox_intercept", strlen("trapbox_intercept"), 0);
  zend_string *name_trapbox_seal = zend_string_init("trapbox_seal", strlen("trapbox_seal"), 0);

  if (zend_hash_exists(EG(function_table), name_trapbox_intercept)) {
    zend_hash_del(EG(function_table), name_trapbox_intercept);
  }

  if (zend_hash_exists(EG(function_table), name_trapbox_seal)) {
    zend_hash_del(EG(function_table), name_trapbox_seal);
  }

  zend_string_release(name_trapbox_intercept);
  zend_string_release(name_trapbox_seal);

  RETURN_TRUE;
}


PHP_FUNCTION(trapbox_set_exit_handler) {
    zval *handler;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "z", &handler) == FAILURE) {
        RETURN_FALSE;
    }

    if (!zend_is_callable(handler, 0, NULL)) {
        php_error_docref(NULL, E_WARNING, "Provided argument is not callable");
        RETURN_FALSE;
    }

    if (exit_handler_set) {
        zval_ptr_dtor(&exit_handler);
    }

    ZVAL_COPY(&exit_handler, handler);
    exit_handler_set = 1;

#if PHP_VERSION_ID >= 80400
    // On PHP 8.4+, exit() is a regular function - intercept it
    if (!exit_intercepted_84) {
        zend_string *function_name_str = zend_string_init("exit", 4, 0);
        zval *original_function_zval;

        if ((original_function_zval = zend_hash_find(EG(function_table), function_name_str)) != NULL) {
            // Store original function
            zend_hash_add_ptr(&replaced_functions, function_name_str, Z_PTR_P(original_function_zval));

            // Create replacement
            intercepted_function *replacement = emalloc(sizeof(intercepted_function));
            zend_function *original_func_copy = emalloc(sizeof(zend_function));
            memcpy(original_func_copy, Z_PTR_P(original_function_zval), sizeof(zend_function));
            replacement->original_func = original_func_copy;
            memcpy(&replacement->func, Z_PTR_P(original_function_zval), sizeof(zend_function));
            replacement->func.common.function_name = zend_string_copy(function_name_str);
            replacement->func.internal_function.handler = ZEND_FN(replacement_function);

            // Replace in function table
            zend_hash_del(EG(function_table), function_name_str);
            zend_hash_add_ptr(EG(function_table), function_name_str, replacement);

            exit_intercepted_84 = 1;
        }

        zend_string_release(function_name_str);
    }
#endif

    RETURN_TRUE;
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
  // Stealth
  //php_info_print_table_header(2, "trapbox support", "enabled");
  php_info_print_table_end();
}

static const zend_function_entry trapbox_functions[] = {
    PHP_FE(trapbox_intercept, arginfo_trapbox_intercept)
    PHP_FE(trapbox_seal, arginfo_trapbox_seal)
    PHP_FE(trapbox_set_exit_handler, arginfo_trapbox_set_exit_handler)
    PHP_FE_END
};

PHP_MSHUTDOWN_FUNCTION(trapbox)
{
  intercepted_function *func;

  ZEND_HASH_FOREACH_PTR(&replaced_functions, func)
  {
    //efree(func->original_func);
    //efree(func);
  }
  ZEND_HASH_FOREACH_END();

  zend_hash_destroy(&replaced_functions);
  zend_hash_destroy(&closures);
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
#if PHP_VERSION_ID < 80400
  zend_set_user_opcode_handler(ZEND_EXIT, trapbox_exit_handler);
#endif

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

  NULL, //PHP_MINFO(trapbox),
  NULL, //PHP_TRAPBOX_VERSION,
  STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_TRAPBOX
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(trapbox)
#endif
