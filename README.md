# php-trapbox

PHP extension that records selected operations and suspicious errors as JSONL.

## Error observation

The existing `php.ini` settings control error observation:

```ini
extension=trapbox.so
trapbox.enabled=1
trapbox.errors=bypass
```

These are the default enabled/error settings. On PHP 7.3 and 7.4, trapbox wraps
application error handlers so handled warnings are observed too. On PHP 8,
trapbox uses the engine's error observer. `trapbox.errors=off` disables error
observation; `trapbox.enabled=0` disables trapbox. No separate build flag or
runtime setting is needed for the PHP 7 wrapper.

Settings are `PHP_INI_SYSTEM`: reload or restart PHP-FPM after changing them. PHP scripts
cannot change them with `ini_set()`. Errors are only observed, including when
the configured error mode is `block` or `trap`.

The PHP 7 wrapper preserves callback returns, exceptions and native handler
stack operations, but adds an internal frame to handler backtraces. References
abandoned by `exit()` are released at request shutdown. Tested PHP 7 shutdown
cases have no additional lost bytes or blocks compared with native PHP under
Valgrind, including with `USE_ZEND_ALLOC=0`; native PHP itself still has losses
in that configuration. This is not a general leak-free guarantee.

See [CLAUDE.md](CLAUDE.md) for build/test commands and known limitations.
See [VALIDATION.md](VALIDATION.md) for the PHP matrix, memory and concurrency
checks, and local Magento deployment and browser validation results.
