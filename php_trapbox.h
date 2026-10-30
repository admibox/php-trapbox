/* trapbox extension for PHP */

#ifndef PHP_TRAPBOX_H
# define PHP_TRAPBOX_H

extern zend_module_entry trapbox_module_entry;
# define phpext_trapbox_ptr &trapbox_module_entry

# define PHP_TRAPBOX_VERSION "2.0.0-dev"

# if defined(ZTS) && defined(COMPILE_DL_TRAPBOX)
ZEND_TSRMLS_CACHE_EXTERN()
# endif

#endif	/* PHP_TRAPBOX_H */
