#!/bin/sh
# Rebuild every image, tear down whatever's currently running, and
# bring the whole rig back up fresh.
#
# Steps:
#   1. build-all.sh           — rebuild all five images
#   2. docker compose down    — stop + remove the currently running rig
#   3. docker compose up -d   — start the freshly-built rig detached
#
# Extra args are forwarded to build-all.sh (and thus each build.sh),
# e.g. force a clean rebuild:
#   ./run.sh --no-cache
#
# The whole rig runs on host networking (see docker-compose.yml), so
# there's no bridge/override to juggle. Requires Docker Compose v2 (the
# compose file uses the top-level `name:` key). The script prints the
# exact `logs -f` command afterwards.
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
COMPOSE_FILE="$DIR/docker-compose.yml"

# These compose files require Docker Compose v2: the file uses the
# top-level `name:` key, which Compose v1 (`docker-compose` 1.x) chokes
# on. Detect v2 explicitly and refuse to limp along on v1.
#
# Prefer the `docker compose` plugin; accept the standalone
# `docker-compose` binary only if it self-reports v2+.
if docker compose version >/dev/null 2>&1; then
	COMPOSE="docker compose"
elif docker-compose version >/dev/null 2>&1; then
	COMPOSE="docker-compose"
else
	echo "run.sh: Docker Compose v2 is required but was not found." >&2
	echo "  Install the 'docker compose' plugin." >&2
	exit 1
fi

# Parse MAJOR from the selected CLI and enforce the v2 floor.
ver=$($COMPOSE version --short 2>/dev/null | sed 's/^[vV]//')
major=${ver%%.*}
case "$major" in '' | *[!0-9]*) major=0 ;; esac
if [ "$major" -lt 2 ]; then
	echo "run.sh: Docker Compose v2+ required, but '$COMPOSE' reports $ver." >&2
	echo "  This compose file uses v2-only features (top-level name:)." >&2
	exit 1
fi

# Invoke compose with the file path quoted so a repo path containing
# spaces survives. $COMPOSE stays unquoted on purpose: it's the CLI
# word(s), either "docker compose" or the single token "docker-compose",
# never a path.
dc() {
	$COMPOSE -f "$COMPOSE_FILE" "$@"
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
echo "Trackers: hxtrackd localhost:5498, Argus localhost:5698."
# Quote the path so a spaced repo path stays copy-pasteable.
echo "Follow logs: $COMPOSE -f \"$COMPOSE_FILE\" logs -f"
