#!/bin/bash
# Build + test in container. Use commit-based caching: first run installs deps,
# subsequent runs are fast (incremental compile).
set -euo pipefail
DIR=$(cd "$(dirname "$0")/.." && pwd)
IMG="pyroscope-php-test:local"

# Build cached image (once)
if ! docker image inspect "$IMG" &>/dev/null; then
  echo "--- Building test image (one-time) ---"
  docker build -t "$IMG" -f- "$DIR" <<'DOCKERFILE'
FROM hub.tal.com/astra/library-php:8.3.30-bookworm-cli-swoole6
RUN apt-get update -qq && apt-get install -y -qq autoconf gcc make libc6-dev libcurl4-openssl-dev >/dev/null 2>&1
DOCKERFILE
  echo "--- Image ready ---"
fi

# Build + test
docker run --rm -v "$DIR:/app" -w /app "$IMG" bash -c '
set -euo pipefail
cd /app/extension
phpize --clean 2>/dev/null || true
phpize >/dev/null 2>&1 && ./configure --enable-pyroscope-php >/dev/null 2>&1 && make -j$(nproc) >/dev/null 2>&1 && make install >/dev/null 2>&1
echo "extension=pyroscope_php.so" > /usr/local/etc/php/conf.d/pyroscope_php.ini
php /app/tests/rigorous_test.php
'
