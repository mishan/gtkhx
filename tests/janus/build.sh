#!/bin/sh
# Build the Janus test-server image.
#
# Tag matches the `image:` in tests/docker-compose.yml and the
# standalone instructions in README.md, so the same image is reused
# whether you build it here, via build-all.sh, or through compose.
#
# Pass extra docker-build args through, e.g.:
#   ./build.sh --no-cache
#   ./build.sh --build-arg JANUS_SHA256=...
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

exec docker build -t gtkhx-janus "$@" "$DIR"
