#!/bin/sh
# Build the Argus test-tracker image.
#
# Tag matches the `image:` in tests/docker-compose.yml and README.md.
#
# Extra docker-build args pass through, e.g.:
#   ./build.sh --no-cache
#   ./build.sh --build-arg ARGUS_SHA256=...
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

exec docker build -t gtkhx-argus "$@" "$DIR"
