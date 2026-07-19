#!/bin/sh
# Rebuild every image, tear down whatever's currently running, and
# bring the whole rig back up fresh.
#
# Steps:
#   1. build-all.sh           — rebuild all four images
#   2. docker compose down    — stop + remove the currently running rig
#   3. docker compose up -d   — start the freshly-built rig detached
#
# Extra args are forwarded to build-all.sh (and thus each build.sh),
# e.g. force a clean rebuild:
#   ./run.sh --no-cache
#
# Set JANUS_HOST_NET=1 to layer in the host-networking override so
# Janus's WebRTC voice path works (Linux host only — see
# docker-compose.janus-host.yml):
#   JANUS_HOST_NET=1 ./run.sh
#
# Afterwards, tail logs with:
#   docker compose -f tests/docker-compose.yml logs -f
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
COMPOSE_FILE="$DIR/docker-compose.yml"
OVERRIDE_FILE="$DIR/docker-compose.janus-host.yml"

# Optional host-networking override for Janus (enables voice). The down,
# up, and ps calls must all see the same -f set so compose tears down and
# brings up the identical project definition.
USE_OVERRIDE=0
if [ "${JANUS_HOST_NET:-0}" = "1" ]; then
	USE_OVERRIDE=1
	echo ">> Janus host-networking override enabled (voice path active)"
fi

# Pick the available Compose CLI: prefer the `docker compose` plugin,
# fall back to the legacy standalone `docker-compose` binary.
if docker compose version >/dev/null 2>&1; then
	COMPOSE="docker compose"
elif command -v docker-compose >/dev/null 2>&1; then
	COMPOSE="docker-compose"
else
	echo "run.sh: neither 'docker compose' nor 'docker-compose' is available" >&2
	exit 1
fi

# Invoke compose with the right -f set, keeping every file path quoted so
# a repo path containing spaces survives. $COMPOSE stays unquoted on
# purpose: it's the CLI word(s), either "docker compose" or the single
# token "docker-compose", never a path.
dc() {
	if [ "$USE_OVERRIDE" -eq 1 ]; then
		$COMPOSE -f "$COMPOSE_FILE" -f "$OVERRIDE_FILE" "$@"
	else
		$COMPOSE -f "$COMPOSE_FILE" "$@"
	fi
}

echo ">> Rebuilding images"
"$DIR/build-all.sh" "$@"

echo
echo ">> Tearing down any running rig"
# --remove-orphans cleans up containers from earlier compose revisions
# (e.g. a service that was renamed). Safe no-op when nothing is running.
dc down --remove-orphans

echo
echo ">> Starting the rig"
dc up -d

echo
dc ps
echo
echo "Rig is up. Servers: mhxd localhost:5500, Janus localhost:5510."
echo "Trackers: Argus localhost:5498, hxtrackd localhost:5598."
# Print the exact logs command for the rig actually brought up, paths
# quoted so a spaced path is copy-pasteable.
if [ "$USE_OVERRIDE" -eq 1 ]; then
	echo "Follow logs: $COMPOSE -f \"$COMPOSE_FILE\" -f \"$OVERRIDE_FILE\" logs -f"
else
	echo "Follow logs: $COMPOSE -f \"$COMPOSE_FILE\" logs -f"
fi
