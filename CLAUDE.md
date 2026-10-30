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

### Version compatibility

PHP 7.4 through 8.5. Version-conditional compilation via `#if PHP_VERSION_ID` for:
- `zend_compile_string` signature (changes at 8.0 and 8.2)
- `zend_error_cb` vs `zend_register_error_notify_callback` (changes at 8.0)

### Module lifecycle

- **MINIT**: Register ini settings, swap compile hooks
- **MSHUTDOWN**: Restore compile hooks
- **RINIT**: Generate correlation ID, initialize per-request state
- **RSHUTDOWN**: Restore function table, clean up per-request state
