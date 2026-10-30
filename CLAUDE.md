# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

trapbox is a PHP C extension for intrusion detection. It intercepts dangerous PHP functions (`exec`, `eval`, `include`, file writes to code paths, secret file reads, `unserialize`) and logs events with full backtrace, request context, and correlation ID. Three modes per category: **block** (prevent), **trap** (fake success, capture payload), **bypass** (allow, observe).

Design plan: https://claude.ai/artifact/Kb9CCqtKc8ntrSQgEyCyBi

## Build Commands

```bash
# Full build (clean rebuild)
make clean && phpize && ./configure && make

# Quick rebuild after code changes
make
```

## Running Tests

```bash
# Test across all PHP versions (7.4, 8.0-8.5) via containers
./test-versions.sh

# Run tests manually (requires extension built for this PHP version)
php -dextension=modules/trapbox.so test.php
```

Tests use `sudo nerdctl` (not docker). `test-versions.sh` auto-detects the container runtime.

`test.php` is the runner — it includes all `test_*.php` files. Each test prints `OK:` or `FAIL:` lines. Add new tests as `test_<name>.php`.

`e2e/request_check.php` drives a real HTTP request through `php -S`, because CLI has no request context. Never put PHP files in `tests/`: `phpize --clean` deletes `tests/*.php`.

## Architecture

Single source file: `trapbox.c`. Header: `php_trapbox.h`.

### Hook mechanisms (planned, see design plan)

| Category | Hook |
|---|---|
| exec family | Function table replacement in `EG(function_table)` |
| eval | `zend_compile_string` (global fn pointer swap) |
| include/require | `zend_compile_file` (global fn pointer swap, chains after OPcache) |
| File writes/reads | Function table replacement + argument filtering |
| unserialize | Function table replacement |
| Errors/warnings | `zend_error_cb` (7.x) / `zend_register_error_notify_callback` (8.0+) |

### Current limitations

- All modes default to `bypass`: installing the extension must not change application behavior.
- `trapbox.exec = trap` currently behaves like `block` (returns `false`). Fake output is pending, see the open questions in the design plan.

### Version compatibility

PHP 7.3 through 8.5. Version-conditional compilation via `#if PHP_VERSION_ID` for:
- `zend_compile_string` signature (changes at 8.0 and 8.2)
- `zend_error_cb` vs `zend_register_error_notify_callback` (changes at 8.0)

### Module lifecycle

- **MINIT**: Register ini settings, patch function handlers in place, swap `zend_compile_string`
- **MSHUTDOWN**: Restore compile hooks
- **RINIT**: Generate correlation ID, initialize per-request state. The first RINIT of each process swaps `zend_compile_file`, because OPcache hooks it after MINIT and would otherwise hide cached `phar://` includes
- **RSHUTDOWN**: Release per-request state

Function handlers are patched **once in MINIT and never restored**. Swapping function table entries per request (the v1 approach) crashes php-fpm under JIT: compiled code keeps a pointer to the freed entry. All mode settings are `PHP_INI_SYSTEM`, so a process-lifetime decision is correct. `test-fpm.sh` exists to catch this class of bug; run it for any change to hooking.
