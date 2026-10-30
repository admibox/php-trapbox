#!/bin/bash
# Shared matrix: matches ~/dev/php-docker-images/versions.php.
read -r -a PHP_VERSIONS <<< "${VERSIONS:-7.3 7.4 8.0 8.1 8.2 8.3 8.4 8.5}"
IMAGE_REPOSITORY="${IMAGE_REPOSITORY:-ecommpro/php}"
IMAGE_TAG_SUFFIX="${IMAGE_TAG_SUFFIX:-}"

if nerdctl info &>/dev/null; then
    RUNTIME=(nerdctl)
elif sudo nerdctl info &>/dev/null; then
    RUNTIME=(sudo nerdctl)
else
    echo "Error: nerdctl is not available (rootless or with sudo)"
    exit 1
fi
