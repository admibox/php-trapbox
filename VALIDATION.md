# Validation record

Validation of implementation commit `ab47d39368b5718d7f33446b5bb251c0031f577b`.
For build commands, test runners, architecture, and known interception limits,
see [CLAUDE.md](CLAUDE.md). For configuration, see [README.md](README.md).

Results below describe the tested versions and workloads, not a guarantee for
all applications. Absolute paths identify evidence on the validation machine;
artifacts under `/tmp` are temporary and are not distributed with this repository.

## Integrated-source validation — 2026-10-30

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

## Local Ideavinos deployment — 2026-10-31

Commit `ab47d39368b5718d7f33446b5bb251c0031f577b` is installed and active in
`~/docker/ideavinos.com`, reachable at `https://ideavinos.localhost/`.
All categories explicitly use `bypass`. No production host or remote repository
was changed. This is the actual application FPM pool, not the earlier sidecar.

Build provenance:

- Source/header/config were extracted directly from the commit with `git show`.
- Exact base image: `admibox/php:7.3-fpm`, image ID
  `sha256:2f1361a2d77885881bd67ee76e5607bce0c2394106eb48c3b024ef06da214779`.
- A local immutable-purpose alias `trapbox-build-base:ideavinos-2f1361a2` was
  created because this nerdctl version does not accept a `sha256:` image ID as
  a run reference. Its image ID was checked against the original image.
- PHP is 7.3.33 NTS, built March 18, 2022. The PHP executable hash was compared
  between build image and running application container; it matches.
- Installed module SHA-256:
  `c5838dd327a8b6fa08da8afbeebe71be52d0bcdab0db1f3107a9577e312773f4`.

Persistent local files (existing bind mounts; no compose/image replacement):

- Module: `/home/manel/docker/ideavinos.com/etc/php/trapbox/ab47d39/trapbox.so`.
- Active INI: `/home/manel/docker/ideavinos.com/etc/php-fpm/conf.d/zz-trapbox.ini`.
- Event log: `/home/manel/other/ideavinos.com/var/log/trapbox-local-ab47d39.jsonl`
  (mode 0600, worker UID 1000).
- `build.json`, `deployment.json`, `DEPLOYMENT.txt`, `rollback.py` and verification
  fixtures live beside the installed module. Build logs/source snapshots:
  `/tmp/trapbox-ideavinos-deploy.974578mt/`.

Validation:

1. Exact-image activation and callback-visibility probes passed before install.
2. `php-fpm -t` accepted the INI. The actual FPM workers reported the expected
   module mapped in memory, matching module hash, OPcache enabled, all modes
   bypass and unchanged Magento exception/callback identity behavior.
3. Home, product and login remain HTTP 200; missing page remains 404. Their
   normalized HTML matches the no-extension baseline. Search remains the known
   500 from Elasticsearch's missing `tm_value` mapping, also identical.
4. Five controlled Magento-handled warnings were logged exactly once with five
   distinct IDs. Final validation also captured ten application file-write
   events. Full event payloads were not copied into Git.
5. Rollback was actually tested: INI removed from the scan directory, FPM
   reloaded, real worker confirmed the extension absent, home returned 200.
   The extension was then re-enabled and the full verification repeated.

The first activation used a container restart, which changed its network
address while Nginx retained the old resolution of `php-fpm`. This caused a
transient 502 even though direct named-service FPM tests passed. Automatic
rollback disabled trapbox; reloading Nginx restored the web. **Final activation
and rollback use SIGUSR2 on the FPM master**, preserving the listening socket,
container and network. No container IP was placed in configuration or commands.
If the container itself is recreated/restarted in this legacy stack, Nginx's
upstream name resolution must also be refreshed.

Repeat the private FastCGI check from the existing CLI container:

```sh
sudo nerdctl exec ideavinos-php-cli-1 php -n /usr/local/share/php/trapbox/ab47d39/client.php
```

Disable this local build and verify recovery (tested):

```sh
python3 /home/manel/docker/ideavinos.com/etc/php/trapbox/ab47d39/rollback.py
```

Both verification scripts are outside the public document root; no public PHP
test endpoint was added. The binary is specific to the recorded PHP image/ABI;
rebuild it for a different PHP runtime. Final state: active, observe-only,
local FPM only.

### Browser smoke check (2026-10-31)

An isolated headless Chrome session loaded home, the Envinate product and customer
login with HTTP 200 and completed DOM rendering. No credentials or orders were
submitted. Browser evidence: `/tmp/trapbox-browser-results.json`.

Observed frontend issues: four missing payment icons (404), Doofinder options
blocked by CORS for the localhost origin, and a home-page JavaScript exception
reading `scrollHeight` from undefined. At this stage, these browser issues had
not been compared with an extension-disabled baseline; the comparison below
subsequently reproduced them without trapbox.
Catalog search still returns the previously established Elasticsearch `tm_value`
mapping error (500).

During the browsing interval, eight file-write events were observed, all bypass.
A subsequent private FPM probe confirmed the expected module hash, all modes
bypass, and preserved Magento exception handling/callback identity. This was a
public-page smoke check, not an authenticated or checkout flow validation.

### Browser comparison with extension unloaded (2026-10-31)

Repeated the four public routes in fresh isolated Chrome profiles, first with
trapbox removed from FPM and then restored. Verified each FPM state through the
private FastCGI probe. Used master SIGUSR2 reloads, preserving the container and
network; restoration was protected by a finally block.

At 1365x900, both runs produced identical captured DOM summaries, HTTP status/URL
sets, and console errors: home/product/login 200, search 500, four payment icons
404, and Doofinder CORS rejection. The home scrollHeight exception was absent in
both wide-viewport runs. A second paired home test with Chrome's original default
viewport reproduced the identical scrollHeight exception both without and with
trapbox. Thus all previously observed browser issues reproduce without trapbox;
no extension-induced difference was detected in this smoke check.

Evidence: `/tmp/trapbox-browser-ab.963qdlj5/` and
`/tmp/trapbox-browser-ab-small.qci26zf1/` (JSON results and browser logs).
Final state: original INI restored, module loaded, all categories bypass.

### Local guest cart smoke check (2026-10-31)

With trapbox active in bypass, used an isolated Chrome guest session and the
storefront controls to add Envinate Taganan Tinto 2025 (product 10615), open the
cart, update quantity from one to two, and empty the cart. The item subtotal
changed from EUR 29.50 to EUR 59.00. Both the cart page and minicart reflected the
changes. Reloading the empty cart confirmed it remained empty. No checkout was
started, credentials supplied, or order placed.

Observed local document/XHR/fetch responses during these actions were successful,
with no JavaScript exceptions captured. The trapbox log interval contained eight
file-write events, all bypass, and no error events. Interval counts can include
concurrent browsing; they are not an assertion about per-cart event counts.
Temporary evidence: `/tmp/trapbox-cart-{add,open,update,empty,reload,final}.log`
and before/after state files under the same prefix. No application code or
extension configuration changed. The isolated test cart was left empty.
## Validation scope

Local validation is complete for installation and rollback, public-page
comparison with and without the extension, and the guest cart flow. No
regressions were detected in these checks. Authenticated flows, checkout
completion, and production deployment were not validated. The local deployment
remains active with every category set to bypass.
