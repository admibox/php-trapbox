# Finding security holes, with the origin

A design note for turning trapbox from a general-purpose function interceptor
into one specific instrument: a trap that catches an intrusion in the act and
records enough context to name the file that let it in.

Written 2026-10-30, out of a real break-in on a production Magento 2.3.5
store (ideavinos.com). Nothing here is implemented yet.

## The case this comes from

On 29 October 2026 a store was broken into. What the logs give us:

```
13:52:04-06  4 POST /graphql/ from one address that appears nowhere else
13:52:05     curl downloads 1,690 bytes; a php -r runs inside php-fpm
             PHP Warning: file_get_contents(app/etc/env.php): No such file or directory
                                                     in Command line code on line 1
13:52:37     PHP Warning: fopen(.../page/js/require_js.phtml):
                          Read-only file system in Command line code(1) : eval()'d code on line 68
13:54:10     PHP Warning: file_put_contents(.../page/js/require_js.phtml):
                          Read-only file system in Command line code(1) : eval()'d code on line 26
13:55:44     a card skimmer is written into core_config_data
```

Everything that matters is missing from that record:

- **What the request contained.** The GraphQL body was never logged, so the
  vulnerability is still unknown and still open on every other site running
  the same modules.
- **What the second stage did.** The 1,690 bytes curl fetched are gone. We
  know the skimmer it eventually planted, not what else the payload could do.
- **Which code allowed it.** No backtrace, so no file and no line. The
  warnings name the attacker's *target*, never the vulnerable *caller*.

Note the shape of the attack, because it drives the whole design: it tried to
write code twice, was stopped both times by a read-only file system, and only
then fell back to the database. **The read-only mount defeated the attack and
told us nothing about who was behind it.** Prevention without attribution
leaves you exactly here: safe this time, blind for the next one.

## What the current design misses

The original idea was to intercept writes to code files. That is one row of
a larger table, and this attack succeeded through a different row.

| family | examples | a function? | where to hook |
|---|---|---|---|
| run a command | `exec` `shell_exec` `system` `passthru` `popen` `proc_open` | yes | function table |
| evaluate code | `eval` | **no** | `zend_compile_string` |
| include code | `include`/`require`, `phar://`, `data://` | **no** | `zend_compile_file` |
| write code | `file_put_contents` `fopen`+`fwrite` `rename` `copy` `symlink` | yes | function table, or a stream wrapper |
| deserialize | `unserialize`, gadget chains, `phar://` | yes / wrapper | function table + wrapper |
| **write to the database** | `PDO::exec` `PDOStatement::execute` | **methods** | the class's function table |
| read secrets | `file_get_contents('app/etc/env.php')` | yes | wrapper |
| send data out | `curl_exec` `fsockopen` | yes | function table |

Backticks are not a separate opcode in 7.3; they compile to a `shell_exec`
call, so that row covers them.

The attacker did not pick the database out of cunning. It was the only row
left once the others were closed.

## eval is not a function

Verified on 7.3: `function_exists('eval')` is `false`, same as `include`. The
compiler emits opcode `ZEND_INCLUDE_OR_EVAL` (73, `zend_vm_opcodes.h:150`).
There is no entry in `EG(function_table)`, which is what trapbox manipulates,
so there is nothing to replace.

The hooks that do exist, all present in 7.3:

```c
zend_compile.h:721  extern ZEND_API zend_op_array *(*zend_compile_file)(zend_file_handle *, int);
zend_compile.h:722  extern ZEND_API zend_op_array *(*zend_compile_string)(zval *source_string, char *filename);
zend_execute.h:325  ZEND_API int zend_set_user_opcode_handler(zend_uchar opcode, user_opcode_handler_t);
```

`zend_compile_string` is the right one: a global pointer, swapped in MINIT and
restored in MSHUTDOWN, that receives the eval'd source in full before it is
compiled. OPcache does the same thing to `zend_compile_file`, so the pattern
is well travelled. trapbox already uses the third hook for `ZEND_EXIT`.

```c
static zend_op_array *(*prev_compile_string)(zval *source, char *filename);

static zend_op_array *trapbox_compile_string(zval *source, char *filename)
{
    trapbox_record("eval", Z_STRVAL_P(source), Z_STRLEN_P(source));
    return prev_compile_string(source, filename);
}
```

This hook is **safer than the closure mechanism trapbox already has**: two C
function pointers at module scope, no request-scoped zvals held in tables, no
`zend_create_closure` per call, no user PHP invoked from a delicate place. If
the handler writes to a file in C and returns, the failure surface is close to
nothing.

Three warnings:

- **The signature changes between versions.** 7.3 is as above; by 8.2 it is
  `(zend_string *source, const char *filename, zend_compile_position)`.
  One extension for all the images means `#if PHP_VERSION_ID`.
- **Chaining with OPcache.** It swaps `zend_compile_file` too. Saving the
  previous pointer and calling it works because OPcache plays by the same
  rules, but load order matters.
- **`zend_compile_file` does not see what is already cached.** An `include` of
  a file OPcache already holds never reaches it. Good enough to catch a
  freshly written webshell being included - it is new, so it is not cached -
  useless for auditing includes in general.

## The trap: "fine, you wrote it"

The point is not to block. It is to grant, convincingly, and keep everything.

| attempt | what to return | what to keep |
|---|---|---|
| write a `.php`/`.phtml` | `strlen($data)` | the content, in quarantine - the whole webshell |
| `eval` | an op_array compiling `return;` | the source, which is the second stage |
| `exec` | plausible output (`uid=33(www-data)...`) | the command line |
| read `app/etc/env.php` | **a fake env.php with canary credentials** | the fact that it was read |
| write to `core_config_data` | success | the value, then revert |

The canary deserves its own line: if a password that exists only inside a fake
`env.php` ever shows up trying to reach MySQL, you know it was stolen, when,
and through which request. That detects the *next* stage, not this one.

The cost of lying is real. An attacker who notices turns destructive or
changes technique. This is the choice underneath the whole design - diagnose
or defend - and finding the hole means choosing to diagnose.

## What "the suspect's context" has to contain

This is the part that turns a log line into an origin. Any event, from any
layer, carries:

- a **correlation id** per request, so five events from one intrusion read as one;
- method, URI, headers, and **the raw request body in full** - the GraphQL
  payload we do not have;
- the real client address (`CF-Connecting-IP` where Cloudflare is in front);
- **a backtrace with arguments**: file and line for every frame. This is
  literally the origin. It names the resolver that allowed it;
- what was attempted, what was returned instead, and the payload itself.

Without the backtrace this is a smoke alarm. With it, it is a finger pointing
at the guilty file.

## What trapbox should not try to cover

Two things that are cheaper and safer somewhere else, and knowing this is what
keeps the scope honest:

**Anything the application can already see.** The write that succeeded in this
attack went through `Magento\Framework\App\Config\Storage\WriterInterface::save`.
A thirty-line Magento plugin - no extension, no core dump risk, deployed like
any commit - would have captured the full `debug_backtrace()` and the request
body at the exact moment. Where the application sees it, the application is
the better place.

**Whatever failed loudly.** An attack that fails makes noise, always in the
same way: paths that do not exist, permission denied, read-only file systems.
A `set_error_handler` that dumps request, body and backtrace when a warning
touches a code path or `env.php` would have named the guilty file here, in
pure PHP, at zero cost while nothing is failing. In C the equivalent is
`zend_error_cb`, which also catches the fatals the PHP handler never sees.

trapbox earns its place on what neither of those can reach: `exec`, `eval`,
the `include` of a `phar://`. That is a smaller remit than "intercept any
function", and it is the one worth building.

## Verified on 2026-10-30

- trapbox **compiles clean on PHP 7.3.33**, no warnings, despite the README
  claiming 7.4+. Production runs 7.3, so this matters.
- Interception works there: tested against `exec` `shell_exec` `system`
  `passthru` `popen` `proc_open`. It returns the full command, a backtrace
  with file and line for each frame, and can block by not calling the
  original.
- The lifecycle is sounder than `CLAUDE.md` claims. That file says the hash
  tables live from MINIT to MSHUTDOWN; the code allocates them in RINIT
  (line 383) and restores the original function pointers in RSHUTDOWN (line
  429). There is no cross-request use-after-free. **`CLAUDE.md` should be
  corrected.**

## Risks worth respecting

- It rewrites the process-global function table and builds a closure on every
  intercepted call. Fine for functions that are called almost never - `exec`
  and friends are rare in Magento - dangerous for `fopen`, called thousands of
  times per page.
- `trapbox_set_exit_handler` stores a zval in a module-scope static. Under
  php-fpm, where processes persist, that one does look like a use-after-free
  waiting to happen. Worth a second read before anything ships.
- None of this belongs on a production store four weeks before Black Friday
  without load testing somewhere else first.

## Where to start

1. `zend_compile_string`, for the eval source. Lowest risk, highest value,
   and nothing else can reach it.
2. The `exec` family through the existing closure mechanism, limited to that
   list and no other function.
3. The event format above, shared with whatever the application layer writes,
   so a Magento plugin today and trapbox tomorrow produce the same records.
