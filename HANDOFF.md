# trapbox v2 — handoff

Written 2026-10-30. The session driving this rewrite was cut off by a model
safety classifier mid-task (false positive on legitimate defensive security
work). This file is the context to resume with another tool (Kimi, Codex, …).

Read `CLAUDE.md` first for build/test mechanics, then this file for where the
work stands and what comes next. The full design lives in the plan doc:
https://claude.ai/artifact/Kb9CCqtKc8ntrSQgEyCyBi

## Current integration status — 2026-10-30

The PHP 7 error-handler wrapper is now integrated in `trapbox.c`, controlled by
existing `trapbox.enabled` and `trapbox.errors` only. No new build flag or runtime
INI option. The previous extra 168-byte bailout leak has been fixed; current
validation and evidence are recorded in the final integration section below.
Earlier sections retain the chronological experiments and superseded limitations.

## What trapbox v2 is

A PHP C extension for intrusion detection. It intercepts dangerous operations
and writes one JSON line per event to `trapbox.log_path`, with correlation id,
request context and a backtrace. Per-category modes: `block` (prevent), `trap`
(fake success, keep the payload), `bypass` (allow + log), `off`. Every category
defaults to `bypass` so installing the extension never changes app behavior.
Config is entirely `php.ini` (`PHP_INI_SYSTEM`); there is no PHP-facing API.

Born from a real Magento 2.3.5 break-in (ideavinos.com, prod runs PHP 7.3): the
read-only mount stopped the attack but left no attribution — no request body,
no backtrace, no origin. trapbox exists to name the file that let it in.

## State: phases 0–6 done, committed, pushed, green on PHP 7.3–8.5

At the initial handoff, the working tree was clean and phases 0–6 were on
`origin/main` (github.com:admibox/php-trapbox). The follow-up changes and
experiments described below are local and uncommitted.

- **Phase 0** skeleton — v1 API (`trapbox_intercept`/`seal`/exit handler),
  closures, stealth all removed.
- **Phase 1** event infra — 14 ini settings, correlation id (RINIT), lazy
  request capture (method/uri/ip/headers/body), backtrace walker, JSONL writer
  (`open`+`flock`+`write`), ZTS-safe globals.
- **Phase 2** exec family — `exec system shell_exec passthru popen proc_open
  pcntl_exec`. `pcntl_exec` untested (not in the CLI image).
- **Phase 3** eval — `zend_compile_string` hook, 3 signature variants. block
  returns NULL (eval → false), trap compiles `return;` (eval → null).
- **Phase 4** include — `zend_compile_file` hook, installed in first RINIT (not
  MINIT) so OPcache can't sit in front and hide cached `phar://`. Fires on any
  wrapper except `file://`.
- **Phase 5** file_write / secret_read — per-function handlers (hot path, O(1),
  no alloc on non-match). Writes match by extension, reads by substring. trap
  for reads swaps in the canary path; trap for `fopen`-write returns a memory
  stream keeping `orig_path` so the attacker's later `fwrite` is logged.
- **Phase 6** unserialize + errors — unserialize fires only on payloads with
  objects (`O:`/`C:`, NUL-safe memory search) and `allowed_classes !== false`.
  Errors via `zend_observer_error_register` (8.0+) / `zend_error_cb` (7.x),
  filtered to eval'd-code origins and I/O failures naming a secret/code path.

Everything is one file: `trapbox.c` (~700 lines), header `php_trapbox.h`.

## Tests — all green 7.3–8.5, re-run before trusting any change

- `./test-versions.sh` — builds in containers (`sudo nerdctl`), runs
  `test_*.php` via `test.php`, each category in off/block/trap, plus
  `e2e/request_check.php` (real request through `php -S`).
- `./test-fpm.sh` — 100 requests through one persistent php-fpm worker with
  OPcache, and JIT (tracing + function) on PHP 8. This suite exists because the
  v1 per-request function-table swap SIGSEGV'd under JIT; it fails if OPcache or
  JIT isn't actually active, so it can't pass vacuously.

Both must stay green. `test-fpm.sh` is the guard for any change to hooking.

## Backtrace arguments + truncation marker implemented (2026-10-30)

Changes are local, not committed or pushed. `trapbox_append_backtrace()` now
captures positional arguments, including extra arguments in their relocated
Zend slots. References are dereferenced, strings obey `max_arg_size`, and
containers/resources are type summaries. Floats are typed strings (including
INF/NAN); unset slots are marked unavailable. No object methods are called.
Extra named variadic arguments are not captured. See CLAUDE.md for the format.

Truncated strings carry a trailing `...[truncated]` marker. Request body
capture reads one extra byte so exact-limit bodies are not marked truncated.
The marker can also occur literally in input; it is not a separate metadata flag.

Coverage added in `test_backtrace.php`, `e2e/request_check.php`, and the FPM
suite: references, variadics, extra arguments, recursive arrays, objects,
resources, non-finite floats, constructors, generators, unset arguments,
truncation boundaries, and argument assertions on repeated FPM/JIT requests.

Validation: `./test-versions.sh` and `./test-fpm.sh` passed on PHP 7.3–8.5.
The final constructor/generator/unset and HTTP boundary cases were also checked
separately on PHP 7.3 after their addition during the CLI matrix run.

The initial working tree contained many untracked build artifacts and this
handoff file, despite the older clean-tree statement above. They were preserved.

## Follow-up: FPM coverage and required includes (2026-10-30)

Local changes now also cover the omissions recovered from the Claude session:

- Blocked `require` / `require_once` previously returned false and continued.
  They now fail like native required dependencies: fatal on PHP 7, catchable
  Error on PHP 8. Include still returns false; trap still returns 1.
- `e2e/include_check.php` tests all modes and include/require constructs,
  including `allow_url_include=0` for non-once data-wrapper includes.
- FPM explicitly tests unserialize side effects, restricted/scalar payloads,
  handled and unhandled warnings, fatal requests, and worker reuse afterward.
  It now runs off/bypass/block/trap, with tracing/function JIT on PHP 8.
- `test-config.sh` shares the version matrix matching
  `~/dev/php-docker-images/versions.php` and defaults to `ecommpro/php` images.
  Both runners support VERSIONS, IMAGE_REPOSITORY, and IMAGE_TAG_SUFFIX.
- The FastCGI test client now sends SCRIPT_NAME as well as REQUEST_URI, so FPM
  exposes the request URI to the event capture.

Validation of this follow-up: both complete suites passed against local
`ecommpro/php` CLI/FPM images on PHP 7.3, 7.4, and 8.0–8.5. The FPM matrix
covers 80 mode/JIT configurations, with 100 normal requests per configuration
and one additional fatal request in off/bypass/block. Shell syntax and
`git diff --check` also pass. Changes remain uncommitted.

Other recovered items remain open: secret reads after rename; performance
under FPM, real applications, and event logging; a possible ZTS race during
first-RINIT include hook installation; log rotation documentation; reconciling
the original plan's global trapbox.mode and event result field with the code.

## Exploration: wrapping PHP 7 error handlers (2026-10-30)

The prototype lives in `experiments/php7-error-handler/` and is NOT integrated
into production `trapbox.c`. Run `experiments/php7-error-handler/run.sh`; it
copies the current extension into a fresh `/tmp/trapbox-handler-prototype.*`
directory, injects `proxy.inc`, and builds with ecommpro/php 7.3 and 7.4 FPM
images. Production build artifacts are not changed.

The native set_error_handler still validates callbacks and maintains its own
stack. Its active callable is wrapped; its return value is unwrapped to retain
original callback identity. Native restore_error_handler restores the wrappers
without needing another hook. Observation happens before delegation, while
return values, exceptions and native fallback processing are preserved. A
request-local fingerprint suppresses duplicate observation when a false return
sends the original error to zend_error_cb. Wrappers expose their callback to GC.

### Scope compatibility resolved in the prototype

A class-scoped __invoke initially prevented valid private callbacks from
running. The replacement uses the object's get_closure handler to expose an
embedded internal function with no class scope, bound object or called scope.
Zend's native callback resolution therefore finds the original error context.
Callbacks are not pre-resolved or granted additional visibility. A failed
lookup without an exception returns false so the engine performs the same
fallback processing it would perform without the proxy.

Each wrapper owns its internal function descriptor, and the engine's
orig_user_error_handler zval keeps it alive during dispatch. The class is final;
empty objects created or deserialized by user code cannot expose that function
through get_closure/Closure::fromCallable. No global execute hook, manual
access-check implementation, or mutation of EG(current_execute_data) is used.

### Validation

The final run passed on PHP 7.3 and 7.4 using ecommpro/php:

- Differential CLI probes preserve true/null/false returns, exceptions, masks,
  restore/null, invalid registration, nested warnings, callback replacement,
  context references, invokable objects, public methods, destruction and GC.
- Visibility tests preserve private/protected access inside permitted scopes,
  reject calls from unrelated/global scopes, preserve fallback error details,
  and cover __call, __callStatic, inherited late static binding, self/parent/static,
  bound closures and named functions. Valid private callbacks now run once,
  matching native PHP, rather than being skipped by the initial prototype.
- The basic probe records 18 warnings once each. The visibility probe records
  its 15 warnings once each, including failed callback resolution and magic
  callbacks returning false.
- 10,000 registrations/restorations have bounded retained memory. Both CLI
  probes also pass with USE_ZEND_ALLOC=0.
- Separate processes compare exit(17) and an uncaught error inside a handler,
  including exit status, shutdown callbacks and destructor output, using both
  allocators. They match native PHP.
- The copied FPM suite covers all four modes and 100 normal requests each:
  800 normal requests across both versions, plus fatal recovery. Every normal
  request exercises private/protected/magic callbacks and rejected private
  access, with exact per-request event counts. A wrapped callback remains
  active at request shutdown to exercise engine cleanup.

The private-method incompatibility is resolved in these tests. The added
`trapbox_error_proxy` frame remains visible to application backtraces; full
transparency is not claimed. Integration, ZTS and broader compatibility review
remain outstanding. Production PHP 7 still has the documented observation gap
until the prototype is integrated. No commit or push has been made.

## Generated differential tests and Memcheck (2026-10-30)

The user authorized stronger differential and memory testing. This remains
isolated in `experiments/php7-error-handler/`; production trapbox is unchanged.

New reproducible entry points:

- `run.sh`: builds the temporary prototype and runs deterministic probes,
  generated differential sequences, and the FPM suite. Defaults to 100 seeds
  with 100 operations each; SEQUENCE_COUNT and SEQUENCE_LENGTH override these.
- `run-memcheck.sh`: builds local test-only images from ecommpro/php 7.3/7.4
  with Valgrind, then runs 1,000 seeds of 128 operations plus memory checks.
  Requires nerdctl/sudo just like the existing suites; no images are published.
- `differential.php --replay <sequence.json>` replays a fixture in the temporary
  build container. Generated failures save the seed, original/reduced sequence,
  native/prototype stdout, stderr, exit status and event log. Reduction preserves
  the failure class. `run.sh` retains artifacts under its printed temporary
  build directory, including per-version archives.

Sequences cover handler registration, masks, restore/null/invalid callbacks,
reusing and manually invoking previous callbacks, repeated identical warnings,
error_reporting, caught exceptions, nested warnings and callback replacement
inside a running handler. Every sequence compares application output and
error_get_last against native PHP, then checks the expected event order and
count. No differential mismatches were found across seeds 1–1000 on each of
PHP 7.3 and 7.4: 2,000 sequences, 256,000 generated top-level operations.

Memcheck found a real prototype bug: exit() abandoned the extra heap array of
copied callback arguments (80 direct bytes in the captured 7.3 report). The
proxy now borrows Zend's call-frame arguments; call_user_function forwards
with no_separation=1. No extra array/refcount ownership needs C cleanup after
bailout. Existing termination.php reproduces the case. audit-bailout.php rejects
abandoned dispatch allocations; it was verified to reject the pre-fix report
and accept the post-fix report.

Memory evidence and limits:

- Normal-return probes, caught exceptions, visibility cases and eight selected
  generated sequences pass on both versions with zero unsuppressed errors and
  zero definite/indirect/possible lost bytes. Both native and prototype runs
  are checked, using USE_ZEND_ALLOC=0. No suppression file is used.
- Production PHP binaries lack PHP's HAVE_VALGRIND string-comparison adapter.
  Their padded assembly string comparison causes undefined-value reports even
  without trapbox. valgrind-string.c supplies the equivalent exact-length
  comparison only under Valgrind, following Zend/zend_string.c's upstream
  wrapper design. It does not suppress invalid memory accesses.
- exit and uncaught-fatal cases already leak in native PHP 7 when Zend MM is
  disabled. Their full leak reports are retained; those cases gate on invalid
  accesses and the explicit abandoned-dispatch-allocation check, not zero
  leaks. The engine-held proxy object also remains a lifetime limitation on
  bailout. Do not report these cases as leak-free.
- The post-fix FPM suite passes all four modes on both versions: 800 normal
  requests plus fatal recovery. FPM was not run under Valgrind in this pass.

Final evidence directories in this session:
- PHP 7.3: /tmp/trapbox-handler-prototype.7BifwL (memory-results and generated cases)
- PHP 7.4: /tmp/trapbox-handler-prototype.nxJkXw
- Pre-fix loss report: /tmp/trapbox-handler-prototype.QkDaP5/exit-prototype-investigation.log

The extra backtrace frame, production integration and ZTS remain outstanding.
Generated coverage and Memcheck improve evidence; they are not a proof of full
compatibility with arbitrary application callbacks.

## Then: remaining plan items

- **Phase 7** — README, example `trapbox.ini` per stack (Magento/WP/Laravel),
  and `trapbox-tail` (the log-reader that does alerting; the extension only
  ever writes, never makes network calls — that separation is deliberate).
- **exec trap** still behaves like block (returns false). Fake output is an open
  question: static `uid=33(www-data)` is detectable, `popen`/`proc_open` need a
  real resource. Documented as pending in the plan.
- **file_write→include correlation** — deferred from phase 4. Flag an include of
  a file written earlier in the same request (the normal-path webshell case
  that phase 4's wrapper filter misses). Cross-request is a harder, separate
  problem.
- **`trapbox.app_root`** — undecided. Would let include-filtering flag paths
  outside the app root, not just wrappers.

## Known gaps (also in CLAUDE.md / plan)

- **PHP 7 handler transparency:** handled warnings are now observed by the
  integrated wrapper. Callback returns, exceptions and handler-stack behavior
  are tested against native PHP, but an extra internal stack frame is visible.
- **Path matching is literal:** `/etc//passwd` and symlinks evade `secret_paths`.
- **Not hooked:** `SplFileObject`, `fprintf`, `stream_copy_to_stream`; `phar://`
  metadata deserialization (→ no unserialize event); normal-path includes.
- **Default write extensions omit** `phar php5 php7 pht`.
- **ZTS:** PHP 7.3/7.4 now have single-thread and eight-thread coverage, including
  concurrent first requests. Two core ZTS defects were reproduced and fixed
  (request-ID collisions and compile-hook installation race). PHP 8 ZTS, other
  threaded SAPIs and race-detector instrumentation remain untested; see below.
- **`move_uploaded_file`** hooked but untested.

## Ground rules that shaped the code (don't regress these)

- Function handlers patched ONCE in MINIT, never restored per-request (the JIT
  SIGSEGV lesson). All mode ini is `PHP_INI_SYSTEM`, so process-lifetime is fine.
- The extension only writes a log. No network, no alerting, no credentials in
  it. Alerting belongs in a separate log-reader (keeps php-fpm workers safe).
- `ip` is always `REMOTE_ADDR`; `X-Forwarded-For`/`CF-Connecting-IP` are
  attacker-controlled and logged only as headers.
- `Cookie` and `Authorization` headers are never logged (legit users' creds).
  NOTE: the request body IS logged and can contain passwords/card data — a
  known, accepted tradeoff, worth a word in the README.
- An invalid mode value falls back to `bypass` with a warning, never to `off` —
  a typo must not silently disable monitoring.
- Tests assert real behavior (did the command run? did the file appear? is the
  phar actually cached?), not just "a line was logged." Keep that bar.

## Local Magento validation — 2026-10-30

The user offered `~/docker/ideavinos.com` as the first real application candidate.
The code is mounted from `/home/manel/other/ideavinos.com`; Magento dependency
is 2.3.5. Its running `ideavinos-php-fpm-1` uses `admibox/php:7.3-fpm`, PHP
7.3.33 built March 18, 2022 (NTS), different from the newer matrix images.

Added `experiments/php7-error-handler/local-magento.py` and `magento-probe.php`.
Run from the repository:

```sh
python3 experiments/php7-error-handler/local-magento.py
```

The runner compiles the isolated prototype with the application's exact image,
then starts native/prototype temporary FPM containers sharing the application's
mounts and network by container name. It uses a private Unix FastCGI socket,
one worker, the mounted PHP configuration, application prepend, and OPcache.
All trapbox categories are in `bypass`. It does not change the app's source,
compose, INI, or running FPM; ordinary GETs can write sessions/cache/logs.
Temporary containers are stopped and removed. This is server-side compatibility
coverage, not a browser/JavaScript or full checkout test.

Final passing artifacts: `/tmp/trapbox-local-magento.ed8yqro9/`.
36 FastCGI requests: 18 native and 18 prototype, three iterations of:

- Homepage: 200.
- Product `/vino-envinate-taganan-tinto.html`: 200.
- Customer login page: 200 (no credentials submitted).
- Search `?q=rioja`: preexisting 500, missing Elasticsearch `tm_value` mapping.
  Independently confirmed through `https://ideavinos.localhost/` before running
  the prototype. The search failure is identical; successful search is untested.
- Missing page: 404.
- Internal fixture loading the real Magento bootstrap/autoloader and ErrorHandler:
  warning converted to the same Exception, suppressed warning returning false,
  callback identity/set/restore preserved, false-return fallback preserved.

Assertions: same statuses, titles and full HTML after narrowly normalizing form
keys, remember-me IDs and captcha timestamps; exact fixture JSON; one stable
worker per mode and OPcache active. Nine expected warning events (including
suppressed/handled warnings), no duplicates. Total prototype events: 39 (30
file writes, nine warnings). Both FPM logs show no crash markers. The original
storefront still returns 200 and does not load trapbox after the run.

This supports compatibility with the tested local Magento paths, not production
readiness. No authenticated actions, cart changes, checkout/payments, admin, cron,
queue consumers, browser scripts, cold-cache matrix or long-running load tested.
FPM Valgrind and PHP 7 ZTS were subsequently tested (see the next section).
The bailout limitations and extra observable proxy stack frame still apply. No integration into
production `trapbox.c` and no production deployment performed.

## FPM Memcheck and concurrent ZTS — follow-up on 2026-10-30

Reproducible commands (all runners use isolated temporary builds):

```sh
python3 experiments/php7-error-handler/fpm-memory.py
experiments/php7-error-handler/run-zts.sh
python3 experiments/php7-error-handler/zts-threads.py
python3 experiments/php7-error-handler/zts-threads.py --cold
```

### FPM under Valgrind

`fpm-memory.py` uses the existing `trapbox-proxy-memcheck:7.3-fpm` and `7.4-fpm`
images, one persistent worker, OPcache, `USE_ZEND_ALLOC=0`, and the same exact-byte
Zend string comparison adapter used by the earlier CLI Memcheck tests. It runs
native and prototype separately: 20 normal/caught-exception requests and 50
requests alternating normal, handler exit(17), normal, handler fatal, and caught
exception. Total: 280 requests across both versions and implementations.

Checks: same worker PID, clean handler/error state at each next request,
shutdown/destructor/response equality, correct continuation behavior, one warning
event and a unique request ID per request, clean Memcheck access-error summaries
in both master and worker. Native FPM skips the fixture's destructor after the
uncaught fatal; the prototype matches it. Graceful shutdown has a 60-second FPM
control timeout so Memcheck cleanup is not interrupted by signal escalation.

Final passing reports: `/tmp/trapbox-fpm-memory.k8m0pjon/`.
Console: `/tmp/trapbox-fpm-memory-final.log`.

**Not leak-free:** both native FPM master and worker have shutdown losses with
Zend MM disabled, including in the normal case. Normal prototype byte/block
loss summaries match native exactly. In the bailout suite, the prototype adds
exactly 1,680 lost bytes in ten blocks per PHP version: one 168-byte engine-held
proxy object for each exit(). The previously fixed dispatch argument-vector leak
has not returned. The full leak reports are retained, not suppressed.

`check_bailout_loss_delta()` additionally requires the total increase over native
to equal only the directly allocated proxy objects shown by the report, one per
exit, and identical master losses. It was applied to both saved final reports;
a synthetic extra 80-byte loss was correctly rejected. `audit-bailout.php` also
rejects other abandoned allocations owned by proxy dispatch. These checks do not
turn the known wrapper leak into an acceptable production guarantee. At this stage, default-allocator RSS had not been measured. The subsequent
residency test below now covers that specific repeated-exit scenario.

### ZTS compatibility and actual concurrent requests

Official `php:7.3-zts` / `php:7.4-zts` (7.3.33 / 7.4.33) are separate builds from
the patched NTS images in the main matrix. `run-zts.sh` verifies PHP_ZTS, callback
identity/visibility probes, system allocator behavior and 1,000 generated
sequences of 128 operations per version. Final post-fix artifacts:
`/tmp/trapbox-zts.b5Ja9H/`; console `/tmp/trapbox-zts-after-mutex.log`.

`zts-threads.c` links the images' embedded PHP library. Eight OS threads execute
100 requests each in one process, including false-return, throw, exit and fatal
handlers. The fixture verifies per-request handler isolation, eval/include
results, shutdown results and native/prototype equality. Event checks require
2,400 events and exactly 800 isolated request IDs per prototype run. These are
actual concurrent interpreter threads, not separate CLI processes or FPM workers.

Two defects in core `trapbox.c` were reproduced and fixed:

1. The time/PID-only request ID generator gave just 595 IDs for 800 concurrent
   requests, merging events from unrelated threads. Evidence:
   `/tmp/trapbox-zts-threads.d6tq_pk1/7.3/events.jsonl`.
   ZTS now uses an atomic sequence, an odd 48-bit permutation, startup-time salt
   and PID. IDs are unique within a process until the 48-bit sequence wraps;
   they remain correlation identifiers, not cryptographic tokens. NTS retains
   the existing generator.
2. Concurrent first RINIT calls could save `trapbox_compile_file` as its own
   predecessor, causing an infinite loop in all workers. The cold-start native
   case passed while the prototype completed zero requests and used eight CPU
   cores. GDB confirmed equal predecessor/hook addresses; evidence:
   `/tmp/trapbox-zts-cold-backtrace.txt` and
   `/tmp/trapbox-zts-threads.ria_zr3h/7.3/`. The failed test container was stopped.
   A ZTS-only TSRM mutex now protects the entire first-RINIT check/install.
   It is allocated in MINIT and freed in MSHUTDOWN. Hook installation still
   occurs once after OPcache startup, never restored per request.

Passing warm-start threaded artifacts (after ID fix):
`/tmp/trapbox-zts-threads.cbzvuhtx/`.
Passing cold-start artifacts (after both fixes), both PHP versions:
`/tmp/trapbox-zts-threads.w5vsvril/`.
Console: `/tmp/trapbox-zts-cold-fixed.log`.
The cold mode initializes modules on the main thread but starts the first
requests concurrently in the workers, so it exercises the previously missed
installation race. Both modes use embedded PHP without OPcache; FPM separately
covers OPcache. No Helgrind/ThreadSanitizer claim or PHP 8 ZTS coverage.

After both core fixes, `./test-fpm.sh` passed the complete NTS PHP 7.3–8.5 matrix
(80 configurations, OPcache and PHP 8 JIT modes). Console:
`/tmp/trapbox-fpm-zts-fix-regression.log`. The post-fix ZTS differential run also
passed both versions. The PHP 7 handler wrapper itself remains isolated under
`experiments/`; the two ZTS core fixes are local changes in `trapbox.c`.
No commit, push, production integration of the wrapper, or deployment performed.

## Normal-allocator FPM residency — 2026-10-30

The user asked whether the 168-byte wrapper loss measured with Zend MM disabled
also accumulates with normal PHP memory management. Added a standalone runner
and a read-only health fixture:

```sh
python3 experiments/php7-error-handler/fpm-residency.py
```

Uses patched `ecommpro/php:7.3-fpm` and `7.4-fpm`, isolated prototype builds,
OPcache, `USE_ZEND_ALLOC=1`, one persistent worker, `pm.max_requests=0`, and only
error observation enabled. No Valgrind, preload adapter, forced GC, cache clearing,
worker recycling or application changes. Reuses the existing handler fixture,
including a captured object, exit(17), shutdown callback and destructor checks.

For each version: two native/prototype pairs, reversing order for the second
pair. Each worker warms up with 100 normal requests and 1,000 handler exits, then
runs ten batches of 5,000 exits followed by 25 normal recovery requests. After
each batch, a health request checks handler/error state, allocator setting,
loaded extension and cached OPcache target. An external process reads the idle
worker's `/proc/<pid>/smaps_rollup` (RSS, PSS, private dirty memory, anonymous
memory and swap); PHP separately reports used/reserved heap. All readings require
no swap and the same worker PID. Raw samples and response digests are retained.

Acceptance bounds were set before running: end-minus-start growth at most 1 MiB
and second-half range at most 256 KiB for RSS, private dirty memory, used PHP heap
and reserved PHP heap. **Actual results were stronger:** all eight long runs had
zero RSS/private-dirty/anonymous growth and zero RSS/private-dirty tail range;
reserved PHP heap stayed at 2 MiB. Used heap decreased by 1,208 bytes after the
first health script compilation and then stayed stable. Different processes had
different baseline RSS; the conclusion concerns growth, not zero extension cost.

| PHP | Native RSS growth, repetitions 1 / 2 | Prototype RSS growth, repetitions 1 / 2 |
| --- | --- | --- |
| 7.3 | 0 / 0 bytes | 0 / 0 bytes |
| 7.4 | 0 / 0 bytes | 0 / 0 bytes |

400,000 measured handler exits with Zend MM enabled (50,000 per worker), plus
warmup and recovery requests. Exact response digests match native/prototype for
each pair. Every prototype warning has exactly one event and a distinct request
ID; no worker replacements, crash markers or response failures.

A short **positive control** repeats 1,000 measured exits with `USE_ZEND_ALLOC=0`
for native/prototype on both versions, requiring private-dirty growth over 1 MiB:

| PHP | Native RSS growth | Prototype RSS growth |
| --- | --- | --- |
| 7.3 | 4,386,816 bytes | 5,169,152 bytes |
| 7.4 | 4,444,160 bytes | 5,246,976 bytes |

The positive controls confirm that sampling detects retained memory; RSS includes
allocator overhead and native PHP losses, so these deltas are not a measurement
of the 168-byte object itself. The earlier Memcheck attribution remains valid.
Total across long runs and controls: 416,600 fixture requests plus 132 health
requests. These checks do not prove the absence of every leak, long-duration
workload stability, or application-specific memory bounds. They do establish
that the known bailout wrapper does not cause observable accumulation in this
normal-allocator scenario over the measured 50,000 exits per worker.

Final artifacts: `/tmp/trapbox-fpm-residency.y0778oto/`, including `run.json`,
aggregate `summary.json`, and per-worker `summary.json`, `samples.json`, raw
smaps, FPM logs, response digest and event logs. Console:
`/tmp/trapbox-fpm-residency-run.log`. Runner exited 0 and all test containers were
removed. No production extension changes, integration or deployment in this step.

The earlier separate opt-in proposal was not adopted. The final integration
below uses existing settings only and fixes the additional wrapper bailout loss.

## PHP 7 wrapper integrated, without new configuration — 2026-10-30

The wrapper is now part of root `trapbox.c`, not injected by the experimental
build script. It is installed at MINIT on PHP 7.3/7.4 when existing
`trapbox.enabled=1` and `trapbox.errors` is not `off`. Default settings already
activate it. PHP 8 retains its native observer. There is **no new runtime INI
option and no new build flag**. These existing settings are PHP_INI_SYSTEM,
read at process startup; changes require restarting FPM. `errors=off` disables
observation/wrapping, and `enabled=0` disables the extension as before.

`experiments/php7-error-handler/proxy.inc` was incorporated into `trapbox.c` and
removed as a duplicate source. `prepare.py` now only copies the actual integrated
source/header/config plus test fixtures; it no longer injects an implementation.
Historical runner labels "prototype" mean "with the extension" in current runs.
Main CLI/FPM tests now require handled warning events on PHP 7 too. README and
CLAUDE.md describe the configuration and observable extra handler stack frame.

### Abandoned-reference cleanup

The 168-byte extra loss is fixed rather than bypassed or hidden by configuration.
PHP 7 `zend_error()` moves the current error handler into a C-local zval while
calling it; `exit()` skips that local's restoration/destruction. The wrapper now
catches engine bailout only to record the abandoned reference in request-local
storage, then immediately rethrows the bailout. It does not change PHP exception
handling, exit status, response flow or active error-handler stack.

RSHUTDOWN releases those references after user shutdown functions/destructors,
when PHP execution can no longer resume the abandoned frames that contain
embedded proxy function descriptors. The list is intrusive (no new allocation
on bailout), thread-local, and counts references separately from objects.
Ordinary callback returns/exceptions do not enter this cleanup path.

New `bailout-cases.php` covers nested handlers, replacement inside a handler,
exit during a shutdown callback, and exit during an object destructor. All are
compared against native PHP with status 17, matching output and Valgrind access
checks. `audit-bailout.php` no longer exempts proxy allocations. The strict audit
was also run against the old leak report and correctly rejected it; evidence:
`/tmp/trapbox-integrated-negative-audit.txt`.

### Final validation of the integrated source

All commands exited 0:

- `./test-versions.sh`: CLI matrix PHP 7.3–8.5, all modes and existing E2E checks.
  Console `/tmp/trapbox-integrated-cli.log`.
- `./test-fpm.sh`: all 80 NTS configurations PHP 7.3–8.5, OPcache and PHP 8 JIT.
  Console `/tmp/trapbox-integrated-fpm.log`.
- Integrated PHP 7 compatibility/Memcheck runner: both versions, 1,000 sequences
  of 128 operations per version, visibility/identity probes, normal/system
  allocators, exit/fatal plus four new bailout cases, FPM modes and memory cases.
  Artifacts `/tmp/trapbox-handler-prototype.TuoOJz/artifacts/{7.3,7.4}/`;
  console `/tmp/trapbox-integrated-memory.log`.
- `fpm-memory.py`: both PHP versions, native/extension, 280 requests with stable
  workers and OPcache under Valgrind. **Normal and bailout lost-byte/block
  summaries now match native FPM exactly in both master and worker.** No wrapper
  allowance, no invalid accesses, no proxy-owned leaked allocations. Native PHP
  losses remain; this is not a general leak-free claim. Artifacts
  `/tmp/trapbox-fpm-memory.x4r3x5rj/`; console
  `/tmp/trapbox-integrated-fpm-memory.log`.
- `zts-threads.py --cold`: 800 requests on eight actual threads per implementation
  and version, concurrent first RINIT, handler isolation and 2,400 extension
  events with 800 unique IDs. Artifacts `/tmp/trapbox-zts-threads.myfx525l/`;
  console `/tmp/trapbox-integrated-zts.log`.
- `fpm-residency.py`: repeats the full 400,000 measured normal-allocator exits,
  native/extension, two pairs per version. All eight long runs again have zero
  post-warmup RSS/private-dirty growth. Allocator-disabled positive controls
  still detect native memory retention. Artifacts
  `/tmp/trapbox-fpm-residency.cj00tub2/`; console
  `/tmp/trapbox-integrated-residency.log`.
- `local-magento.py`: actual Ideavinos image and application, same 36 requests,
  normalized HTML/handler behavior and nine expected warning events. The known
  Elasticsearch search failure is still present in both baseline and extension.
  Artifacts `/tmp/trapbox-local-magento.myj4ovnb/`; console
  `/tmp/trapbox-integrated-magento.log`.
- `activation.php`: existing defaults, `errors=off`, and `enabled=0`, on both
  PHP 7 versions. Checks callback identity, invocation, event count and whether
  the internal wrapper class is installed. Added to `run.sh`; also executed
  separately against the final FPM Memcheck builds, all six checks passed.

Remaining compatibility distinction: the wrapper adds an internal handler stack
frame. Other preexisting scope limitations (literal paths, unhooked APIs, etc.)
are unchanged. No new INI entries, build flags or allocator-specific fallback.
The integration, supporting fixes and tests are recorded together in Git.
No push or production deployment has been performed.

### Commit review

The commit includes the earlier argument/truncation capture and required-include
corrections, the shared PHP image matrix, the integrated PHP 7 wrapper and its
cleanup, ZTS fixes, the test harnesses and documentation. Generated phpize/make
files, shared libraries, logs, local histories and source backup copies are
excluded. No implementation changes were needed during the final diff review.

Root `trapbox.c` and `php_trapbox.h` were compared byte-for-byte against the final
FPM Memcheck, Magento and ZTS build snapshots; all match. Python/shell syntax and
Git whitespace checks passed. The previous complete test results therefore
apply to the committed implementation without another redundant matrix run.
Suggested next step: prepare the versioned build and local deployment check.

## Environment

- No PHP on the host. Everything runs in containers via `sudo nerdctl` (rootless
  containerd isn't set up, so plain `nerdctl` fails — the scripts auto-detect).
- `tests/` is forbidden for PHP files: `phpize --clean` deletes `tests/*.php`.
  End-to-end helpers live in `e2e/`.
