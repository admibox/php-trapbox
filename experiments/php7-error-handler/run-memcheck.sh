#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/test-config.sh"
for VERSION in ${PROXY_VERSIONS:-7.3 7.4}; do
    "${RUNTIME[@]}" build \
        --build-arg "PHP_IMAGE=$IMAGE_REPOSITORY:$VERSION-fpm$IMAGE_TAG_SUFFIX" \
        -f "$ROOT/experiments/php7-error-handler/Dockerfile.memcheck" \
        -t "trapbox-proxy-memcheck:$VERSION-fpm" \
        "$ROOT/experiments/php7-error-handler"
done
IMAGE_REPOSITORY=trapbox-proxy-memcheck IMAGE_TAG_SUFFIX= MEMCHECK=1 \
    SEQUENCE_COUNT="${SEQUENCE_COUNT:-1000}" SEQUENCE_LENGTH="${SEQUENCE_LENGTH:-128}" \
    "$ROOT/experiments/php7-error-handler/run.sh"
