#!/bin/sh
# Run inside the dedicated Memcheck image, after building the temporary extension.
set -u
mkdir -p memory-results
cc -shared -fPIC -g -O1 $(php-config --includes) valgrind-string.c -o valgrind-string.so || exit 1
export USE_ZEND_ALLOC=0
export ZEND_DONT_UNLOAD_MODULES=1
export LD_PRELOAD=/ext/valgrind-string.so
FLAGS="-n -ddisplay_errors=0 -dlog_errors=0 -derror_reporting=32767"
PROXY="-dextension=/ext/modules/trapbox.so -dtrapbox.log_path=/tmp/trapbox-proxy-events.jsonl -dtrapbox.exec=off -dtrapbox.eval=off -dtrapbox.include=off -dtrapbox.file_write=off -dtrapbox.secret_read=off -dtrapbox.unserialize=off -dtrapbox.errors=bypass"
run_case() {
    LABEL=$1
    EXPECTED=$2
    shift 2
    LEAK_ERRORS=definite,indirect,possible
    if [ "$EXPECTED" -ne 0 ]; then
        # Native PHP 7 itself abandons allocations on exit/fatal with Zend MM off.
        # Keep full leak reports, but gate this case on invalid accesses only.
        # Normal-return and caught-exception cases still fail on any lost memory.
        LEAK_ERRORS=none
    fi
    for MODE in native prototype; do
        EXTRA=""
        if [ "$MODE" = prototype ]; then EXTRA=$PROXY; fi
        valgrind --tool=memcheck --error-exitcode=99 --leak-check=full \
            --show-leak-kinds=definite,indirect,possible \
            --errors-for-leak-kinds="$LEAK_ERRORS" --track-origins=yes \
            --log-file="memory-results/$LABEL-$MODE.log" \
            php $FLAGS $EXTRA "$@" >"memory-results/$LABEL-$MODE.txt" 2>&1
        CODE=$?
        if [ "$CODE" -ne "$EXPECTED" ]; then
            echo "FAIL: Memcheck $LABEL $MODE: exit=$CODE expected=$EXPECTED"
            cat "memory-results/$LABEL-$MODE.txt" "memory-results/$LABEL-$MODE.log"
            return 1
        fi
        if ! grep -q 'ERROR SUMMARY: 0 errors' "memory-results/$LABEL-$MODE.log"; then
            echo "FAIL: missing clean Memcheck summary for $LABEL $MODE"
            return 1
        fi
    done
    if [ "$EXPECTED" -ne 0 ]; then
        php -n audit-bailout.php "memory-results/$LABEL-prototype.log" || return 1
        echo "LIMITATION: $LABEL has native PHP bailout leaks; full reports retained, invalid-access checks passed"
    else
        echo "OK: Memcheck $LABEL, native and prototype, no suppressed errors"
    fi
}
run_case probe 0 probe.php || exit 1
run_case visibility 0 visibility.php || exit 1
run_case exit 17 termination.php exit || exit 1
run_case fatal 255 termination.php fatal || exit 1
for CASE in nested replace shutdown destructor; do
    run_case "bailout-$CASE" 17 bailout-cases.php "$CASE" || exit 1
    diff -u "memory-results/bailout-$CASE-native.txt" "memory-results/bailout-$CASE-prototype.txt" || exit 1
done
for CASE in memory-cases/*.json regressions/*.json; do
    [ -f "$CASE" ] || continue
    run_case "$(basename "$CASE" .json)" 0 sequence.php "$CASE" || exit 1
done
