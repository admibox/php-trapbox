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
# Test all supported PHP branches (7.3, 7.4, 8.0-8.5) via containers
./test-versions.sh

# Run tests manually (requires extension built for this PHP version)
php -dextension=modules/trapbox.so test.php
```

Tests use `nerdctl`, with `sudo` when required. Both suites share
`test-config.sh`: PHP 7.3, 7.4, and 8.0–8.5, matching
`~/dev/php-docker-images/versions.php`. The default images are `ecommpro/php`,
so tests exercise the patched PHP builds and enabled extensions used by that
project. Build tools must be available in the selected images.

```bash
./test-versions.sh
./test-fpm.sh
# Select branches or compare with official PHP images:
VERSIONS="7.3 8.5" ./test-versions.sh
IMAGE_REPOSITORY=php ./test-fpm.sh
# Use a dated candidate tag, for example 8.5-fpm-20261027-all-r1:
IMAGE_TAG_SUFFIX=-20261027-all-r1 ./test-fpm.sh
```

Run the suites sequentially: they rebuild `modules/trapbox.so` in the shared
working directory. The CLI suite also runs `e2e/include_check.php`, which checks
all four include/require constructs and all modes, with URL inclusion enabled
and disabled. `require` and `require_once` in block mode fail with a fatal error
on PHP 7 and a catchable `Error` on PHP 8, matching native dependency failures.
Plain `include` in block mode still returns `false`; trap mode returns `1`.

The FPM suite exercises all four modes on one persistent worker, with OPcache
and both tracing and function JIT on PHP 8. It verifies deserialization side
effects, `allowed_classes=false`, scalar deserialization, warnings with and
without a user error handler, per-request category counts, and worker reuse
after an uncaught fatal error or a blocked `require`. Handled warnings are
observed on PHP 7 through the callback wrapper and on PHP 8 through its observer.

`test.php` is the runner — it includes all `test_*.php` files. Each test prints `OK:` or `FAIL:` lines. Add new tests as `test_<name>.php`.

`e2e/request_check.php` drives a real HTTP request through `php -S`, because CLI has no request context. Never put PHP files in `tests/`: `phpize --clean` deletes `tests/*.php`.

The PHP 7 handler has additional compatibility runners. They now build isolated
copies of the integrated root source, despite the historical directory name:

```bash
python3 experiments/php7-error-handler/fpm-memory.py
python3 experiments/php7-error-handler/fpm-residency.py
experiments/php7-error-handler/run-zts.sh
python3 experiments/php7-error-handler/zts-threads.py --cold
```

The FPM memory runner needs the images built by `run-memcheck.sh`. It compares
native/prototype under Valgrind with OPcache, exit/fatal handlers and subsequent
requests in the same worker. Native PHP shutdown losses with Zend MM disabled remain; the old extra
168-byte wrapper leak is fixed by releasing abandoned engine references during
RSHUTDOWN. Normal and bailout FPM losses must now match native bytes/blocks
exactly, and the audit rejects all abandoned wrapper-owned allocations.
The residency runner measures idle worker RSS/private memory and PHP heap with
normal Zend MM, without worker recycling. Two native/prototype pairs per PHP
version each execute 50,000 handler exits after warmup; allocator-disabled
positive controls verify that sampling detects growth. All eight normal-allocator
runs stayed flat in RSS/private memory. This is evidence for that workload,
not a general leak-free guarantee. Allocator-disabled cleanup is tested
separately under Valgrind.
The ZTS runners use official PHP 7.3/7.4 ZTS images. The embedded harness runs
eight actual interpreter threads; `--cold` tests concurrent first requests.
See [VALIDATION.md](VALIDATION.md) for validation evidence and exact gates.
Remaining interception limitations are documented below.

## Architecture

Single source file: `trapbox.c`. Header: `php_trapbox.h`.

### Hook mechanisms

| Category | Hook |
|---|---|
| exec family | Function table replacement in `EG(function_table)` |
| eval | `zend_compile_string` (global fn pointer swap) |
| include/require | `zend_compile_file` (global fn pointer swap, chains after OPcache) |
| File writes/reads | Function table replacement + argument filtering |
| unserialize | Function table replacement |
| Errors/warnings | Callback wrapper plus `zend_error_cb` (7.x) / `zend_observer_error_register` (8.0+) |

### Event argument format

Backtrace frames include `args` in positional order, including extra positional
arguments. These are the current frame values, which may differ from the values
originally passed if the function changes its parameters. Strings use
`trapbox.max_arg_size`; integers, booleans, and null keep their JSON types.
Floats use `{"type":"float","value":"..."}` to preserve non-finite values.
Arrays, objects, and resources use type summaries, without traversing contents
or invoking user code. Unset argument slots use `{"type":"unavailable"}`.
Extra named variadic arguments are not captured.

Truncated strings end with `...[truncated]`, after the configured number of
source bytes. This applies to event arguments, frame arguments, headers, and
request bodies. The marker is additional to the byte limit and is not escaped
when it occurs literally in input, so it is an indicator rather than an
unambiguous machine-readable flag. Body capture reads one extra byte to detect
truncation; a body exactly at the limit has no marker.

### Current limitations

- All modes default to `bypass`: installing the extension must not change application behavior.
- `trapbox.exec = trap` currently behaves like `block` (returns `false`). Fake output is pending, see the open questions in the design plan.

- `file_write` and `secret_read` filter by the literal path: `/etc//passwd` evades `/etc/passwd`, and a symlink evades both. Writes and reads through `SplFileObject`, `fprintf`, `stream_copy_to_stream` and similar are not hooked. `file_put_contents` data given as an array or stream is not logged.

- On PHP 7, a callback wrapper observes handled warnings because the native error callback misses them. It is installed automatically when `trapbox.enabled` and `trapbox.errors` are active, with no additional build/runtime option. It preserves native callback/stack behavior but adds an internal backtrace frame. Abandoned references from `exit()` are released after user shutdown callbacks and destructors, in RSHUTDOWN. PHP 8 keeps its native error observer.
- `unserialize` is not covered through `phar://` metadata: any file function on a phar can trigger deserialization without an event.

### Version compatibility

PHP 7.3 through 8.5. Version-conditional compilation via `#if PHP_VERSION_ID` for:
- `zend_compile_string` signature (changes at 8.0 and 8.2)
- `zend_error_cb` vs `zend_observer_error_register` (changes at 8.0, signature again at 8.1)

### Module lifecycle

- **MINIT**: Register ini settings, patch function handlers in place, swap `zend_compile_string`
- **MSHUTDOWN**: Restore compile hooks
- **RINIT**: Generate correlation ID, initialize per-request state. The first RINIT of each process swaps `zend_compile_file`, because OPcache hooks it after MINIT and would otherwise hide cached `phar://` includes
- **RSHUTDOWN**: Release per-request state

Function handlers are patched **once in MINIT and never restored**. Swapping function table entries per request (the v1 approach) crashes php-fpm under JIT: compiled code keeps a pointer to the freed entry. All mode settings are `PHP_INI_SYSTEM`, so a process-lifetime decision is correct. `test-fpm.sh` exists to catch this class of bug; run it for any change to hooking.

In ZTS, a TSRM mutex protects the complete first-RINIT compile-hook check and
installation. Without it, concurrent first requests can save the hook as its own
predecessor and loop indefinitely. Allocate/free the mutex in MINIT/MSHUTDOWN.
ZTS correlation IDs use an atomic sequence with process/startup salt: time and
PID alone collide when threads start requests in the same microsecond.
