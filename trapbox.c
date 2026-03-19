#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "php_trapbox.h"
#include "zend_closures.h"

/*
 * Per-request state: these are reset in RINIT and cleaned up in RSHUTDOWN.
 * The hash tables use persistent=0 (request-scoped allocation).
 */
static HashTable *replaced_functions = NULL;  // name -> original zend_function* (the original pointer from the function table)
static HashTable *closures = NULL;            // name -> zval (the user-provided closure)
static HashTable *replacements = NULL;        // name -> intercepted_function* (our replacement structs, for cleanup)

static zval exit_handler;
static int exit_handler_set = 0;
static int sealed = 0;
#if PHP_VERSION_ID >= 80400
static int exit_intercepted_84 = 0;
#endif

// Thread-local storage for call context (prevents infinite recursion)
__thread int internal_call_context = 0;

typedef struct _intercepted_function
{
  zend_function func;
  zend_function *original_func;
} intercepted_function;

/* For compatibility with older PHP versions */
#ifndef ZEND_PARSE_PARAMETERS_NONE
#define ZEND_PARSE_PARAMETERS_NONE() \
  ZEND_PARSE_PARAMETERS_START(0, 0)  \
  ZEND_PARSE_PARAMETERS_END()
#endif

ZEND_BEGIN_ARG_INFO(arginfo_trapbox_set_exit_handler, 0)
    ZEND_ARG_CALLABLE_INFO(0, handler, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_trapbox_intercept, 0)
ZEND_ARG_INFO(0, function_name)
ZEND_ARG_INFO(0, closure)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_trapbox_seal, 0)
ZEND_END_ARG_INFO()


// PHP < 8.4: exit is an opcode, intercept via opcode handler
#if PHP_VERSION_ID < 80400
int trapbox_exit_handler(zend_execute_data *execute_data) {
    zval retval;
    zval args[1];
    zval *status = NULL;
    const zend_op *opline = execute_data->opline;

    if (opline->op1_type == IS_CONST) {
        status = RT_CONSTANT(opline, opline->op1);
    } else if (opline->op1_type != IS_UNUSED) {
        status = EX_VAR(opline->op1.var);
    }

    if (!exit_handler_set || Z_TYPE(exit_handler) != IS_OBJECT) {
        // No handler set: let PHP handle exit normally
        return ZEND_USER_OPCODE_DISPATCH;
    }

    ZVAL_NULL(&retval);

    if (status) {
        ZVAL_COPY(&args[0], status);
    } else {
        ZVAL_LONG(&args[0], 0);
    }

    if (call_user_function(EG(function_table), NULL, &exit_handler, &retval, 1, args) == FAILURE) {
        zval_ptr_dtor(&args[0]);
        zval_ptr_dtor(&retval);
        return ZEND_USER_OPCODE_DISPATCH;
    }

    zval_ptr_dtor(&args[0]);
    zval_ptr_dtor(&retval);

    return ZEND_USER_OPCODE_RETURN;
}
#endif


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
  uint32_t i;

  uint32_t num_args = ZEND_NUM_ARGS();
  zval *args = NULL;
  if (num_args > 0) {
    args = safe_emalloc(num_args, sizeof(zval), 0);
    if (zend_get_parameters_array_ex(num_args, args) == FAILURE)
    {
      efree(args);
      RETURN_FALSE;
    }
    // zend_get_parameters_array_ex uses ZVAL_COPY_VALUE (no refcount increment).
    // Addref each arg so we own our copies.
    for (i = 0; i < num_args; i++) {
      Z_TRY_ADDREF(args[i]);
    }
  }

  zend_string *original_function_name = execute_data->func->common.function_name;

#if PHP_VERSION_ID >= 80400
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
      for (i = 0; i < num_args; i++) {
        zval_ptr_dtor(&args[i]);
      }
      if (args) efree(args);

      RETURN_ZVAL(&handler_retval, 0, 1);
  }
#endif

  intercepted_function *replacement = (intercepted_function *)execute_data->func;
  zend_function *original_func = replacement->original_func;

  zend_fcall_info fci = empty_fcall_info;
  zend_fcall_info_cache fci_cache = empty_fcall_info_cache;

  if (internal_call_context > 0) {
      // Direct call to original function (re-entrant call from within the closure)
      fci.size = sizeof(fci);
      fci.retval = &retval;
      fci.param_count = num_args;
      fci.params = args;
      fci_cache.function_handler = original_func;
      fci_cache.called_scope = original_func->common.scope;
      fci_cache.object = NULL;

      if (zend_call_function(&fci, &fci_cache) == FAILURE) {
          for (i = 0; i < num_args; i++) {
            zval_ptr_dtor(&args[i]);
          }
          if (args) efree(args);
          RETURN_FALSE;
      }

  } else {
      // Create a closure wrapping the original function
      zval original_closure;
      zend_create_closure(&original_closure, original_func, NULL, NULL, NULL);

      zval *closure = zend_hash_find(closures, original_function_name);
      if (!closure) {
          zval_ptr_dtor(&original_closure);
          for (i = 0; i < num_args; i++) {
            zval_ptr_dtor(&args[i]);
          }
          if (args) efree(args);
          RETURN_FALSE;
      }

      zend_fcall_info_init(closure, 0, &fci, &fci_cache, NULL, &error);

      fci.retval = &retval;
      fci.param_count = num_args + 1;
      fci.params = safe_emalloc((num_args + 1), sizeof(zval), 0);

      // First param: closure wrapping the original function
      ZVAL_COPY(&fci.params[0], &original_closure);

      // Remaining params
      for (i = 0; i < num_args; i++)
      {
        ZVAL_COPY(&fci.params[i + 1], &args[i]);
      }
      fci.size = sizeof(fci);

      enter_internal_call();
      if (zend_call_function(&fci, &fci_cache) != SUCCESS)
      {
        exit_internal_call();
        for (i = 0; i < fci.param_count; i++) {
          zval_ptr_dtor(&fci.params[i]);
        }
        efree(fci.params);
        zval_ptr_dtor(&original_closure);
        for (i = 0; i < num_args; i++) {
          zval_ptr_dtor(&args[i]);
        }
        if (args) efree(args);
        RETURN_FALSE;
      }
      exit_internal_call();

      // Clean up fci.params
      for (i = 0; i < fci.param_count; i++) {
        zval_ptr_dtor(&fci.params[i]);
      }
      efree(fci.params);

      zval_ptr_dtor(&original_closure);
  }

  // Release our refs on args
  for (i = 0; i < num_args; i++) {
    zval_ptr_dtor(&args[i]);
  }
  if (args) efree(args);

  if (Z_TYPE(retval) == IS_UNDEF) {
    RETURN_NULL();
  }

  RETURN_ZVAL(&retval, 0, 1);
}

/*
 * Internal helper to intercept a function.
 * Allocates the intercepted_function struct and modifies the function table.
 */
static int trapbox_do_intercept(zend_string *function_name_str, zval *closure) {
  zval *original_function_zval;

  if ((original_function_zval = zend_hash_find(EG(function_table), function_name_str)) == NULL) {
    return FAILURE;
  }

  // Store closure (addref so it survives)
  Z_ADDREF_P(closure);
  zend_hash_add(closures, function_name_str, closure);

  // Store original function pointer for restoration in RSHUTDOWN
  zend_function *original_func_ptr = Z_PTR_P(original_function_zval);
  zend_hash_add_ptr(replaced_functions, function_name_str, original_func_ptr);

  // Create our replacement struct
  intercepted_function *replacement = emalloc(sizeof(intercepted_function));

  // Copy the original function definition so we can call it later
  zend_function *original_func_copy = emalloc(sizeof(zend_function));
  memcpy(original_func_copy, original_func_ptr, sizeof(zend_function));
  replacement->original_func = original_func_copy;

  // Set up the replacement function entry
  memcpy(&replacement->func, original_func_ptr, sizeof(zend_function));
  replacement->func.common.function_name = zend_string_copy(function_name_str);
  replacement->func.internal_function.handler = ZEND_FN(replacement_function);

  // Track for cleanup
  zend_hash_add_ptr(replacements, function_name_str, replacement);

  // Swap in the function table by directly overwriting the pointer.
  // We must NOT use zend_hash_del + zend_hash_add because the function table
  // destructor (ZEND_FUNCTION_DTOR) would corrupt the original function entry.
  zval *ft_entry = zend_hash_find(EG(function_table), function_name_str);
  if (ft_entry) {
    Z_PTR_P(ft_entry) = replacement;
  }

  return SUCCESS;
}

PHP_FUNCTION(trapbox_intercept)
{
  char *function_name;
  size_t function_name_len;
  zval *closure;
  zend_string *function_name_str;

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
      zend_string_release(function_name_str);
      return;
  }

  if (zend_hash_exists(replaced_functions, function_name_str)) {
      php_error_docref(NULL, E_WARNING, "Function %s() is already intercepted", ZSTR_VAL(function_name_str));
      zend_string_release(function_name_str);
      return;
  }

  if (trapbox_do_intercept(function_name_str, closure) == FAILURE) {
      php_error_docref(NULL, E_WARNING, "Function %s() does not exist", ZSTR_VAL(function_name_str));
  }

  zend_string_release(function_name_str);
}

PHP_FUNCTION(trapbox_seal)
{
  ZEND_PARSE_PARAMETERS_NONE();
  sealed = 1;
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
    if (!exit_intercepted_84) {
        zend_string *function_name_str = zend_string_init("exit", 4, 0);

        if (zend_hash_find(EG(function_table), function_name_str) != NULL) {
            // For exit on PHP 8.4+, we need a dummy closure (never actually called via closure path)
            zval dummy_closure;
            ZVAL_NULL(&dummy_closure);

            trapbox_do_intercept(function_name_str, &dummy_closure);
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

  // Initialize per-request hash tables
  ALLOC_HASHTABLE(replaced_functions);
  zend_hash_init(replaced_functions, 8, NULL, NULL, 0);

  ALLOC_HASHTABLE(closures);
  zend_hash_init(closures, 8, NULL, ZVAL_PTR_DTOR, 0);

  ALLOC_HASHTABLE(replacements);
  zend_hash_init(replacements, 8, NULL, NULL, 0);

  // Reset per-request state
  sealed = 0;
  exit_handler_set = 0;
  ZVAL_UNDEF(&exit_handler);
  internal_call_context = 0;
#if PHP_VERSION_ID >= 80400
  exit_intercepted_84 = 0;
#endif

  return SUCCESS;
}

PHP_MINFO_FUNCTION(trapbox)
{
  php_info_print_table_start();
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
  return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(trapbox)
{
  zend_string *key;
  zend_function *original_func;

  if (replaced_functions) {
    // Restore original functions in the function table.
    // Use zend_hash_update_ptr to overwrite the pointer directly,
    // avoiding the function table destructor which could corrupt things.
    ZEND_HASH_FOREACH_STR_KEY_PTR(replaced_functions, key, original_func) {
      if (key) {
        zval *existing = zend_hash_find(EG(function_table), key);
        if (existing) {
          // Directly overwrite the pointer in the existing zval
          Z_PTR_P(existing) = original_func;
        }
      }
    } ZEND_HASH_FOREACH_END();

    zend_hash_destroy(replaced_functions);
    FREE_HASHTABLE(replaced_functions);
    replaced_functions = NULL;
  }

  if (replacements) {
    intercepted_function *repl;
    ZEND_HASH_FOREACH_PTR(replacements, repl) {
      if (repl) {
        // Free the function name string we copied
        if (repl->func.common.function_name) {
          zend_string_release(repl->func.common.function_name);
        }
        // Free the original function copy
        if (repl->original_func) {
          efree(repl->original_func);
        }
        efree(repl);
      }
    } ZEND_HASH_FOREACH_END();

    zend_hash_destroy(replacements);
    FREE_HASHTABLE(replacements);
    replacements = NULL;
  }

  if (closures) {
    zend_hash_destroy(closures);
    FREE_HASHTABLE(closures);
    closures = NULL;
  }

  if (exit_handler_set) {
    zval_ptr_dtor(&exit_handler);
    exit_handler_set = 0;
    ZVAL_UNDEF(&exit_handler);
  }

  sealed = 0;
#if PHP_VERSION_ID >= 80400
  exit_intercepted_84 = 0;
#endif

  return SUCCESS;
}

PHP_MINIT_FUNCTION(trapbox)
{
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
