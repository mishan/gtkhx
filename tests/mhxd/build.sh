#!/bin/sh
# Build the mhxd test-server image.
#
# Tag matches the `image:` in tests/docker-compose.yml and README.md.
#
# Extra docker-build args pass through, e.g.:
#   ./build.sh --no-cache
#   ./build.sh --build-arg MHXD_REV=<sha>
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

exec docker build -t gtkhx-mhxd "$@" "$DIR"
