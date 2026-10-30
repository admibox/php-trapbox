#!/bin/bash
#
# Test trapbox under php-fpm with OPcache and JIT across PHP versions
#

if nerdctl info &>/dev/null; then
    RUNTIME="nerdctl"
elif sudo nerdctl info &>/dev/null; then
    RUNTIME="sudo nerdctl"
elif command -v docker &>/dev/null; then
    RUNTIME="docker"
else
    echo "Error: No container runtime found (nerdctl or docker)"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
STATUS=0

for VERSION in ${VERSIONS:-7.3 7.4 8.0 8.1 8.2 8.3 8.4 8.5}; do
    case "$VERSION" in
        7.*) JITS="none" ;;
        *) JITS="none tracing 1205" ;;
    esac

    if OUTPUT=$($RUNTIME run --rm -v "$SCRIPT_DIR:/ext" -w /ext -e JITS="$JITS" "php:$VERSION-fpm" sh -c '
        make clean >/dev/null 2>&1
        phpize --clean >/dev/null 2>&1
        phpize >/dev/null 2>&1 && ./configure >/dev/null 2>&1 && make >/dev/null 2>&1 || { echo "FAIL: build"; exit 1; }
        STATUS=0
        for JIT in $JITS; do
            for MODE in bypass block; do
                php -n -dphar.readonly=0 e2e/fpm_check.php $MODE $JIT 2>&1 || STATUS=1
            done
        done
        exit $STATUS
    ' 2>&1); then
        echo "PHP $VERSION: PASSED"
    else
        echo "PHP $VERSION: FAILED"
        echo "$OUTPUT" | grep -v -E 'sha256|resolv|elapsed|downloading|waiting|manifest|^$' | tail -30
        STATUS=1
    fi
done

exit $STATUS
