#!/bin/bash
#
# Test trapbox extension across multiple PHP versions using nerdctl
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/test-config.sh"
echo "Using container runtime: ${RUNTIME[*]}"

FAILED=()
PASSED=()

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=========================================="
echo "Testing trapbox extension"
echo "=========================================="

# Clean up stale build files
rm -f "$SCRIPT_DIR/trapbox.dep" 2>/dev/null

for VERSION in "${PHP_VERSIONS[@]}"; do
    echo -e "\n${YELLOW}Testing PHP $VERSION...${NC}"

    if OUTPUT=$("${RUNTIME[@]}" run --rm --entrypoint sh -v "$SCRIPT_DIR:/ext" -w /ext "$IMAGE_REPOSITORY:$VERSION-cli$IMAGE_TAG_SUFFIX" -c '
        make clean >/dev/null 2>&1
        phpize --clean >/dev/null 2>&1
        phpize >/dev/null 2>&1 && \
        ./configure >/dev/null 2>&1 && \
        make >/dev/null 2>&1 || { echo "FAIL: build"; exit 1; }
        LOG=/tmp/trapbox-test.jsonl
        FLAGS="-dextension=modules/trapbox.so -dtrapbox.log_path=$LOG -dallow_url_include=1 -dphar.readonly=0 -dtrapbox.canary_env_path=/tmp/trapbox-canary.txt"
        php $FLAGS test.php 2>&1 || exit 1
        for MODE in off block trap; do
            for CATEGORY in exec eval include file_write secret_read unserialize errors; do
                RESULT=$(php $FLAGS -dtrapbox.$CATEGORY=$MODE test_$CATEGORY.php 2>&1)
                CODE=$?
                echo "$RESULT"
                [ $CODE -eq 0 ] || { echo "FAIL: exit code $CODE in $CATEGORY mode $MODE"; exit 1; }
                case "$RESULT" in *FAIL*) exit 1;; esac
            done
        done
        php e2e/request_check.php 2>&1 || exit 1
        php -dphar.readonly=0 e2e/include_check.php 2>&1 || exit 1
    ' 2>&1); then
        echo -e "${GREEN}PHP $VERSION: PASSED${NC}"
        PASSED+=("$VERSION")
    else
        echo -e "${RED}PHP $VERSION: FAILED${NC}"
        echo "$OUTPUT" | tail -25
        FAILED+=("$VERSION")
    fi

    # Clean dep file between versions
    rm -f "$SCRIPT_DIR/trapbox.dep" 2>/dev/null
done

echo ""
echo "=========================================="
echo "Results"
echo "=========================================="
echo -e "${GREEN}Passed: ${PASSED[*]:-none}${NC}"
echo -e "${RED}Failed: ${FAILED[*]:-none}${NC}"

if [ ${#FAILED[@]} -gt 0 ]; then
    exit 1
fi
