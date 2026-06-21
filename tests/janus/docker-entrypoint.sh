#!/bin/sh
# tests/janus/docker-entrypoint.sh — optionally enable tracker
# registration from an env var, then exec the Janus server.
#
# Standalone behaviour is unchanged: with TRACKERS unset the config
# file's own EnableTrackerRegistration value is honoured as-is (the
# checked-in conf/config.yaml ships it `false`). This keeps the
# documented `docker run --network=host gtkhx-janus` path identical to
# before this entrypoint existed.
#
# The docker-compose rig sets:
#
#   TRACKERS="argus:5499,hxtrackd:5499"
#
# which flips EnableTrackerRegistration to true and rewrites the
# `Trackers:` YAML list so Janus's registration heartbeats reach the
# Argus + hxtrackd tracker containers by their compose service names.
#
# Janus's basic `Trackers:` list (host:port[:password]) emits the
# classic HTRK UDP registration that both a v1-only tracker (hxtrackd)
# and a v1/v2/v3 tracker (Argus) accept; v2/v3-specific registration
# would go through the separate `TrackerConfig:` block, which we leave
# empty. Registration also requires Name + Description to be set, which
# the checked-in config already provides.
set -eu

CONF=/opt/janus/Server/config.yaml

if [ -n "${TRACKERS:-}" ]; then
	# Enable registration.
	sed -i \
		's|^EnableTrackerRegistration:.*|EnableTrackerRegistration: true|' \
		"$CONF"

	# Drop the upstream default single entry so we don't also try to
	# register against the public hltracker.com from inside the test
	# rig.
	sed -i '/^[[:space:]]*-[[:space:]]*hltracker\.com:5499$/d' "$CONF"

	# Insert each requested tracker right after the `Trackers:` key.
	# Each insert lands immediately after the anchor, so the final
	# on-disk order is the reverse of the env order — irrelevant for
	# registration. Two-space indentation matches the upstream style
	# and parses cleanly (verified with a YAML loader).
	for t in $(echo "$TRACKERS" | tr ',' ' '); do
		[ -n "$t" ] || continue
		sed -i "/^Trackers:/a\\  - ${t}" "$CONF"
	done

	echo "docker-entrypoint: tracker registration enabled -> $TRACKERS"
else
	echo "docker-entrypoint: TRACKERS unset; using config.yaml as-is"
fi

exec /opt/janus/janus "$@"
