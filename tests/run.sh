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
# Afterwards, tail logs with:
#   docker compose -f tests/docker-compose.yml logs -f
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
COMPOSE_FILE="$DIR/docker-compose.yml"

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

echo ">> Rebuilding images"
"$DIR/build-all.sh" "$@"

echo
echo ">> Tearing down any running rig"
# --remove-orphans cleans up containers from earlier compose revisions
# (e.g. a service that was renamed). Safe no-op when nothing is running.
$COMPOSE -f "$COMPOSE_FILE" down --remove-orphans

echo
echo ">> Starting the rig"
$COMPOSE -f "$COMPOSE_FILE" up -d

echo
$COMPOSE -f "$COMPOSE_FILE" ps
echo
echo "Rig is up. Servers: mhxd localhost:5500, Janus localhost:5510."
echo "Trackers: Argus localhost:5498, hxtrackd localhost:5598."
echo "Follow logs: $COMPOSE -f tests/docker-compose.yml logs -f"
