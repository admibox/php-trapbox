#!/bin/bash
#
# Test trapbox extension across multiple PHP versions using Docker
#

set -e

# Auto-detect container runtime
if command -v docker &>/dev/null; then
    RUNTIME="docker"
elif command -v nerdctl &>/dev/null; then
    # Check if nerdctl works without sudo (rootless) or needs sudo
    if nerdctl info &>/dev/null 2>&1; then
        RUNTIME="nerdctl"
    elif sudo nerdctl info &>/dev/null 2>&1; then
        RUNTIME="sudo nerdctl"
    else
        echo "Error: nerdctl found but not functional (rootless or with sudo)"
        exit 1
    fi
else
    echo "Error: No container runtime found (docker or nerdctl)"
    exit 1
fi
echo "Using container runtime: $RUNTIME"

VERSIONS=("7.4" "8.0" "8.1" "8.2" "8.3" "8.4" "8.5")
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
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

for VERSION in "${VERSIONS[@]}"; do
    echo -e "\n${YELLOW}Testing PHP $VERSION...${NC}"

    if $RUNTIME run --rm -v "$SCRIPT_DIR:/ext" -w /ext "php:$VERSION-cli" sh -c '
        make clean >/dev/null 2>&1
        phpize --clean >/dev/null 2>&1
        phpize >/dev/null 2>&1 && \
        ./configure >/dev/null 2>&1 && \
        make >/dev/null 2>&1 && \
        php -dextension=modules/trapbox.so test.php >/dev/null 2>&1
    ' 2>/dev/null; then
        echo -e "${GREEN}PHP $VERSION: PASSED${NC}"
        PASSED+=("$VERSION")
    else
        echo -e "${RED}PHP $VERSION: FAILED${NC}"
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
