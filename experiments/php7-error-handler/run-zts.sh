#!/bin/bash
# Official PHP 7 ZTS builds are a separate compatibility target from patched NTS images.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/test-config.sh"
WORK="$(mktemp -d /tmp/trapbox-zts.XXXXXX)"
echo "ZTS artifacts: $WORK"
for VERSION in ${PROXY_VERSIONS:-7.3 7.4}; do
    BUILD="$WORK/$VERSION"
    python3 "$ROOT/experiments/php7-error-handler/prepare.py" "$ROOT" "$BUILD"
    cp "$ROOT/experiments/php7-error-handler/"*.php "$BUILD/"
    "${RUNTIME[@]}" run --rm --entrypoint sh -v "$BUILD:/ext" -w /ext \
        -e SEQUENCE_COUNT="${SEQUENCE_COUNT:-1000}" -e SEQUENCE_LENGTH="${SEQUENCE_LENGTH:-128}" \
        "php:$VERSION-zts" -c '
        set -eu
        php -v >version.txt
        php -r "if (!PHP_ZTS) exit(1);"
        phpize >build.log 2>&1 && ./configure >>build.log 2>&1 && make -j2 >>build.log 2>&1
        FLAGS="-dextension=/ext/modules/trapbox.so -dtrapbox.log_path=/tmp/trapbox-proxy-events.jsonl -dtrapbox.exec=off -dtrapbox.eval=off -dtrapbox.include=off -dtrapbox.file_write=off -dtrapbox.secret_read=off -dtrapbox.unserialize=off -dtrapbox.errors=bypass"
        for PROBE in probe visibility; do
            php "$PROBE.php" >"$PROBE-native.txt" 2>&1
            php $FLAGS "$PROBE.php" >"$PROBE-prototype.txt" 2>&1
            grep "^RESULT:" "$PROBE-native.txt" >native-result.txt
            grep "^RESULT:" "$PROBE-prototype.txt" >prototype-result.txt
            diff -u native-result.txt prototype-result.txt
            USE_ZEND_ALLOC=0 php $FLAGS "$PROBE.php" >"$PROBE-malloc.txt" 2>&1
            echo "PASS ZTS: $PROBE, native/prototype and system allocator"
        done
        php -n differential.php "$SEQUENCE_COUNT" "$SEQUENCE_LENGTH" >differential.txt 2>&1
        tail -1 differential.txt
    '
    echo "PASS PHP $VERSION ZTS: single-thread compatibility"
done
