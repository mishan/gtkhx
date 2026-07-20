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
# Requires Docker Compose v2 (the compose files use v2-only features;
# JANUS_HOST_NET=1 additionally needs v2.24+ for the !reset tag). The
# script prints the exact `logs -f` command for the rig it brought up.
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

# These compose files require Docker Compose v2: the base file uses the
# top-level `name:` key, and the Janus host override uses the `!reset`
# tag (added in v2.24). Compose v1 (`docker-compose` 1.x) chokes on both
# with confusing schema errors, so we detect v2 explicitly and refuse to
# limp along on v1.
#
# Prefer the `docker compose` plugin; accept the standalone
# `docker-compose` binary only if it self-reports v2+.
if docker compose version >/dev/null 2>&1; then
	COMPOSE="docker compose"
elif docker-compose version >/dev/null 2>&1; then
	COMPOSE="docker-compose"
else
	echo "run.sh: Docker Compose v2 is required but was not found." >&2
	echo "  Install the 'docker compose' plugin (v2.24+ recommended)." >&2
	exit 1
fi

# Parse MAJOR.MINOR from the selected CLI and enforce the version floor.
ver=$($COMPOSE version --short 2>/dev/null | sed 's/^[vV]//')
major=${ver%%.*}
minrest=${ver#*.}
minor=${minrest%%.*}
case "$major" in '' | *[!0-9]*) major=0 ;; esac
case "$minor" in '' | *[!0-9]*) minor=0 ;; esac

if [ "$major" -lt 2 ]; then
	echo "run.sh: Docker Compose v2+ required, but '$COMPOSE' reports $ver." >&2
	echo "  These compose files use v2-only features (top-level name:)." >&2
	exit 1
fi

# The Janus host override uses the !reset tag, added in Compose v2.24.
if [ "$USE_OVERRIDE" -eq 1 ] && [ "$major" -eq 2 ] && [ "$minor" -lt 24 ]; then
	echo "run.sh: JANUS_HOST_NET=1 needs Docker Compose >= 2.24 for the" >&2
	echo "  !reset tag in docker-compose.janus-host.yml ('$COMPOSE' is $ver)." >&2
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
