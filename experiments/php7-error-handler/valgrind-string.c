/* Test-only equivalent of PHP 7's HAVE_VALGRIND string comparison wrapper.
 * The production PHP binary uses word-sized assembly reads into string padding,
 * which Memcheck reports as undefined branches even without trapbox. PHP's own
 * Valgrind build replaces that path with an exact-length byte comparison:
 * https://github.com/php/php-src/blob/PHP-7.3/Zend/zend_string.c
 * This adapter enables that behavior for the existing ecommpro binaries, without
 * suppressing memory errors or changing the production images. */
#include "php.h"
#include <valgrind/valgrind.h>

ZEND_API zend_bool ZEND_FASTCALL I_WRAP_SONAME_FNNAME_ZU(NONE,zend_string_equal_val)(zend_string *left, zend_string *right)
{
    return memcmp(ZSTR_VAL(left), ZSTR_VAL(right), ZSTR_LEN(left)) == 0;
}
