# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

php-trapbox is a PHP extension that enables runtime function interception using closures. It allows replacing any PHP function with a custom handler that receives the original function as a callable plus all arguments. The extension includes stealth features to hide itself from PHP introspection and a seal mechanism to prevent further modifications.

## Build Commands

```bash
# Full build (clean rebuild)
make clean && phpize && ./configure && make

# Quick rebuild after code changes
make

# Install to PHP extension directory
make install
```

## Running Tests

```bash
# Run main test (build + test)
./test.sh

# Test across all PHP versions (7.4, 8.0-8.5) using Docker
./test-versions.sh

# Run tests manually
php -dextension=modules/trapbox.so test.php
php -dextension=modules/trapbox.so test2.php
```

## Architecture

### Core Data Structures

- `replaced_functions` (HashTable): Maps function names to original `zend_function` pointers
- `closures` (HashTable): Maps function names to replacement closure zvals
- `intercepted_function` struct: Wraps original function with replacement handler
- `internal_call_context` (thread-local int): Tracks nested calls to prevent infinite recursion

### Function Interception Flow

1. `trapbox_intercept()` stores original function and closure in hash tables
2. Creates `intercepted_function` struct pointing to original function
3. Replaces function handler with `ZEND_FN(replacement_function)`
4. On call, `replacement_function` checks `internal_call_context`:
   - If > 0 (nested/internal call): calls original directly
   - If 0 (normal call): wraps original as closure, calls user's handler with `($original, ...$args)`

### Module Lifecycle

- **MINIT**: Initialize hash tables, register exit opcode handler
- **MSHUTDOWN**: Destroy hash tables
- **RINIT/RSHUTDOWN**: Per-request setup/cleanup (minimal)

### PHP API

```php
trapbox_intercept(string $function, Closure $handler)
// Handler: function($original, ...$args) { return $original(...$args); }

trapbox_set_exit_handler(Closure $handler)
// Handler: function($status) { /* handle exit */ }
// PHP < 8.4: Handler called, then script stops (opcode limitation)
// PHP 8.4+: Handler called, script continues (exit is a function)

trapbox_seal()
// Removes trapbox_intercept and trapbox_seal from function table
```

### PHP Version Compatibility

The extension supports PHP 7.4 through 8.5. Version-conditional compilation is used via `#if PHP_VERSION_ID` guards:
- **PHP < 8.4**: `exit` is an opcode — intercepted via `zend_set_user_opcode_handler(ZEND_EXIT, ...)`
- **PHP >= 8.4**: `exit` is a regular function — intercepted via the same `trapbox_intercept` mechanism

### Key Constraints

- `trapbox_intercept` cannot intercept itself (explicitly blocked)
- A function can only be intercepted once (re-interception returns a warning)
- After `trapbox_seal()`, no further interceptions are possible
- `__thread` is used for `internal_call_context` — the extension assumes thread-local storage support

### Source Files

All extension logic is in `trapbox.c` (single file). `php_trapbox.h` only contains the module entry extern and version define.

### Stealth Features

- `phpinfo()` output is empty (MINFO callback is NULL in module entry)
- Module version is NULL in module entry
- `seal()` removes trapbox functions from PHP's function table
