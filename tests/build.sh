#!/bin/sh
# Build one of the test-rig container images.
#
# Usage:
#   ./build.sh <container> [extra docker-build args...]
#
#   <container>   one of: argus, hxtrackd, mhxd, janus, socks
#
# The image is tagged gtkhx-<container>, matching the `image:` keys in
# docker-compose.yml and the standalone instructions in each container's
# README.md — so the same image is reused whether you build it here, via
# build-all.sh, through compose, or in CI.
#
# The build context is tests/<container>, except `socks`, whose directory
# is tests/socks-proxy/ (the image is still gtkhx-socks, matching the
# standalone `docker build -t gtkhx-socks tests/socks-proxy`).
#
# Extra args pass straight through to `docker build`, e.g.:
#   ./build.sh janus --no-cache
#   ./build.sh mhxd  --build-arg MHXD_REV=<sha>
#   ./build.sh argus --build-arg ARGUS_SHA256=<sha>
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ "$#" -lt 1 ]; then
	echo "usage: $0 <container> [docker build args...]" >&2
	echo "  container: argus | hxtrackd | mhxd | janus | socks" >&2
	exit 64
fi

container=$1
shift

# Always pass the short build name (the one that becomes the gtkhx-<name>
# tag compose expects), NOT a directory name. For SOCKS that's `socks`,
# whose context dir is socks-proxy/ — passing `socks-proxy` would produce
# a gtkhx-socks-proxy tag that doesn't match docker-compose.yml. Reject
# the `-proxy` directory form up front so the mismatch can't happen.
case "$container" in
*-proxy)
	echo "$0: pass the short build name, not the directory (e.g. use" >&2
	echo "  'socks', not '$container' — the socks image is gtkhx-socks)." >&2
	exit 64
	;;
esac

# Resolve the build-context directory. Most images live in tests/<name>;
# the SOCKS proxy's directory is socks-proxy/ while its image stays
# gtkhx-socks, so map the `socks` short name to socks-proxy/.
ctxdir="$container"
if [ ! -d "$DIR/$ctxdir" ] && [ -d "$DIR/$ctxdir-proxy" ]; then
	ctxdir="$container-proxy"
fi

if [ ! -f "$DIR/$ctxdir/Dockerfile" ]; then
	echo "$0: unknown container '$container' (no $ctxdir/Dockerfile)" >&2
	echo "  expected one of: argus, hxtrackd, mhxd, janus, socks" >&2
	exit 64
fi

exec docker build -t "gtkhx-$container" "$@" "$DIR/$ctxdir"
