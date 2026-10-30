#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_trapbox.h"

/* For compatibility with older PHP versions */
#ifndef ZEND_PARSE_PARAMETERS_NONE
#define ZEND_PARSE_PARAMETERS_NONE() \
  ZEND_PARSE_PARAMETERS_START(0, 0)  \
  ZEND_PARSE_PARAMETERS_END()
#endif

static const zend_function_entry trapbox_functions[] = {
    PHP_FE_END
};

PHP_MINIT_FUNCTION(trapbox)
{
  return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(trapbox)
{
  return SUCCESS;
}

PHP_RINIT_FUNCTION(trapbox)
{
#if defined(ZTS) && defined(COMPILE_DL_TRAPBOX)
  ZEND_TSRMLS_CACHE_UPDATE();
#endif
  return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(trapbox)
{
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
