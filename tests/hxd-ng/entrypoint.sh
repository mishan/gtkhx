#!/bin/sh
# Start hxd-ng for the Tier 3 rig, advertising the host's own addresses as
# the voice SFU's ICE candidates.
#
# hxd-ng is ICE-lite: it offers only the addresses it is told to advertise
# and never makes connectivity checks of its own. The test clients run on
# the same host (the whole rig is on host networking), but libnice never
# gathers a loopback candidate, so a 127.0.0.1 advertisement is one no
# check ever reaches. The host's real interface addresses are — the same
# kind Janus offers — and on host networking the container sees exactly
# those. They differ per machine, so they are read here at every start
# rather than written into the image.
set -eu

CONF=/etc/hxd-ng/hxd-ng.toml
RUN=/run/hxd-ng/hxd-ng.toml

# hxd-ng takes one advertised address per family with a wildcard bind,
# so this is the host's primary IPv4 address: the first non-loopback one
# `hostname -I` lists, which is the default route's in practice.
addr=
for a in $(hostname -I 2>/dev/null); do
    case $a in
        *:* | 127.*) continue ;; # IPv4 only; the tests speak v4
    esac
    addr=$a
    break
done
# Nothing but loopback: advertise it anyway, so the server starts and the
# failure shows up as ICE not connecting rather than as a missing server.
[ -n "$addr" ] || addr=127.0.0.1
echo "hxd-ng entrypoint: advertising voice on $addr:5524" >&2

mkdir -p "$(dirname "$RUN")"
sed "s|^advertise = .*|advertise = [\"$addr:5524\"]|" "$CONF" >"$RUN"
exec hxd --config "$RUN" "$@"
