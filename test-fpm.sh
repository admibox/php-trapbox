#!/bin/bash
#
# Test trapbox under php-fpm with OPcache and JIT across PHP versions
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/test-config.sh"
STATUS=0

for VERSION in "${PHP_VERSIONS[@]}"; do
    case "$VERSION" in
        7.*) JITS="none" ;;
        *) JITS="none tracing 1205" ;;
    esac

    if OUTPUT=$("${RUNTIME[@]}" run --rm --entrypoint sh -v "$SCRIPT_DIR:/ext" -w /ext -e JITS="$JITS" "$IMAGE_REPOSITORY:$VERSION-fpm$IMAGE_TAG_SUFFIX" -c '
        make clean >/dev/null 2>&1
        phpize --clean >/dev/null 2>&1
        phpize >/dev/null 2>&1 && ./configure >/dev/null 2>&1 && make >/dev/null 2>&1 || { echo "FAIL: build"; exit 1; }
        STATUS=0
        for JIT in $JITS; do
            for MODE in off bypass block trap; do
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
