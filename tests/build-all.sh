#!/bin/sh
# Build all five GtkHx test-rig container images by invoking the shared
# build.sh once per container.
#
# Trackers are built before servers purely for readable output order;
# image builds are independent, so the order has no functional effect.
#
# Any extra args are forwarded to every build, e.g.:
#   ./build-all.sh --no-cache
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Tracker images first, then the servers that register with them, then
# the SOCKS proxy the socks Tier 3 test routes through.
for c in argus hxtrackd mhxd janus socks; do
    echo "============================================================"
    echo "  building $c  (gtkhx-$c)"
    echo "============================================================"
    "$DIR/build.sh" "$c" "$@"
done

echo
echo "All five images built: gtkhx-argus, gtkhx-hxtrackd, gtkhx-mhxd, gtkhx-janus, gtkhx-socks"
