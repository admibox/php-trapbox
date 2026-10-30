#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/test-config.sh"
WORK="$(mktemp -d /tmp/trapbox-handler-prototype.XXXXXX)"
python3 "$ROOT/experiments/php7-error-handler/prepare.py" "$ROOT" "$WORK"
cp "$ROOT/experiments/php7-error-handler/"*.php "$WORK/"
if [ -d "$ROOT/experiments/php7-error-handler/regressions" ]; then
    cp -r "$ROOT/experiments/php7-error-handler/regressions" "$WORK/"
fi
cp "$ROOT/experiments/php7-error-handler/memory.sh" "$ROOT/experiments/php7-error-handler/valgrind-string.c" "$WORK/"
echo "Prototype build directory: $WORK"
for VERSION in ${PROXY_VERSIONS:-7.3 7.4}; do
    "${RUNTIME[@]}" run --rm --entrypoint sh -v "$WORK:/ext" -w /ext -e MEMCHECK="${MEMCHECK:-0}" -e PHP_BRANCH="$VERSION" -e SEQUENCE_COUNT="${SEQUENCE_COUNT:-100}" -e SEQUENCE_LENGTH="${SEQUENCE_LENGTH:-100}" "$IMAGE_REPOSITORY:$VERSION-fpm$IMAGE_TAG_SUFFIX" -c '
        mkdir -p "artifacts/$PHP_BRANCH"
        save_artifacts() {
            cp build.log *.txt "artifacts/$PHP_BRANCH/" 2>/dev/null
            for REPORT in differential-results memory-results memory-cases; do
                [ ! -d "$REPORT" ] || cp -r "$REPORT" "artifacts/$PHP_BRANCH/"
            done
        }
        trap save_artifacts EXIT
        make clean >/dev/null 2>&1 || true
        phpize --clean >/dev/null 2>&1 || true
        phpize >build.log 2>&1 && ./configure >>build.log 2>&1 && make >>build.log 2>&1 || { cat build.log; exit 1; }
        FLAGS="-dtrapbox.log_path=/tmp/trapbox-proxy-events.jsonl -dtrapbox.exec=off -dtrapbox.eval=off -dtrapbox.include=off -dtrapbox.file_write=off -dtrapbox.secret_read=off -dtrapbox.unserialize=off -dtrapbox.errors=bypass"
        php -n -dextension=modules/trapbox.so $FLAGS activation.php || exit 1
        php -n -dextension=modules/trapbox.so $FLAGS -dtrapbox.errors=off activation.php || exit 1
        php -n -dextension=modules/trapbox.so $FLAGS -dtrapbox.enabled=0 activation.php || exit 1
        for PROBE in probe visibility; do
            php "$PROBE.php" >"$PROBE-baseline.txt" 2>&1 || { cat "$PROBE-baseline.txt"; exit 1; }
            php -dextension=modules/trapbox.so $FLAGS "$PROBE.php" >"$PROBE-prototype.txt" 2>&1 || { cat "$PROBE-prototype.txt"; exit 1; }
            grep "^RESULT:" "$PROBE-baseline.txt" >baseline-result.txt
            grep "^RESULT:" "$PROBE-prototype.txt" >prototype-result.txt
            diff -u baseline-result.txt prototype-result.txt || exit 1
            USE_ZEND_ALLOC=0 php -dextension=modules/trapbox.so $FLAGS "$PROBE.php" >allocator.txt 2>&1 || { cat allocator.txt; exit 1; }
            echo "OK: $PROBE native/prototype comparison and system allocator"
        done
        for CASE in exit fatal; do
            if [ "$CASE" = exit ]; then EXPECTED=17; else EXPECTED=255; fi
            php -ddisplay_errors=0 -dlog_errors=0 termination.php "$CASE" >termination-baseline.txt 2>&1
            CODE=$?
            [ "$CODE" -eq "$EXPECTED" ] || { cat termination-baseline.txt; exit 1; }
            for ALLOCATOR in 1 0; do
                USE_ZEND_ALLOC=$ALLOCATOR php -dextension=modules/trapbox.so $FLAGS -ddisplay_errors=0 -dlog_errors=0 termination.php "$CASE" >termination-prototype.txt 2>&1
                CODE=$?
                [ "$CODE" -eq "$EXPECTED" ] || { cat termination-prototype.txt; exit 1; }
                diff -u termination-baseline.txt termination-prototype.txt || exit 1
            done
            echo "OK: handler $CASE status, shutdown and destructor effects"
        done
        php -n differential.php "$SEQUENCE_COUNT" "$SEQUENCE_LENGTH" || exit 1
        if [ "$MEMCHECK" = 1 ]; then sh memory.sh || exit 1; fi
        cat probe-prototype.txt
        for MODE in off bypass block trap; do
            php -n -dphar.readonly=0 e2e/fpm_check.php "$MODE" none || exit 1
        done
    '
    echo "PHP $VERSION: scope and handler compatibility checks passed"
done
