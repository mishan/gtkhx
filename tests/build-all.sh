#!/bin/sh
# Build all four GtkHx test-rig container images by invoking each
# container's own build.sh in turn.
#
# Trackers are built before servers purely for readable output order;
# image builds are independent, so the order has no functional effect.
#
# Any extra args are forwarded to every per-container build, e.g.:
#   ./build-all.sh --no-cache
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Tracker images first, then the servers that register with them.
for c in argus hxtrackd mhxd janus; do
	echo "============================================================"
	echo "  building $c  (gtkhx-$c)"
	echo "============================================================"
	"$DIR/$c/build.sh" "$@"
done

echo
echo "All four images built: gtkhx-argus, gtkhx-hxtrackd, gtkhx-mhxd, gtkhx-janus"
