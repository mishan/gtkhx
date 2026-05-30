#!/usr/bin/env python3
# Seed-registration helper for the hxtrackd test container.
#
# hxtrackd's default behaviour is "empty server list until someone
# registers via UDP". The bundled `init_servers()` code path that
# would populate test entries is wrapped in `#if 0` in upstream
# tracker.c (mhxd commit history shows it was disabled when the
# code was committed). For a Tier 3 target we want at least one
# server in every listing so the client-side test can assert
# records flowed end-to-end, not just "the v1 fallback handshake
# completed."
#
# Strategy: spam a single registration datagram every few seconds
# for the lifetime of the container. The container's hxtrackd.conf
# bumps `tracker.interval` to 86400 (24 hours), so one successful
# datagram is technically enough — but the loop is belt-and-
# suspenders for the case where hxtrackd starts up after this
# script (race during container boot).
#
# Registration packet shape (htrk_udp_rcv in mhxd/src/hxtrackd/
# tracker.c):
#
#   [0..1]   version  u16 BE  (any non-zero — hxtrackd doesn't gate)
#   [2..3]   port     u16 BE  (the HTLS port of the registering server)
#   [4..5]   nusers   u16 BE  (current concurrent users)
#   [6..7]   reserved (zero)
#   [8..11]  id       u32 BE  (server's HTRK id; uniquely names
#                              the registration on hxtrackd's side)
#   [12]     nlen     u8       (server name length, capped 31)
#   [13..]   name     bytes
#   [13+nlen]   dlen  u8       (description length)
#   [14+nlen..] desc   bytes
#
# The advertised IPv4 address comes from the UDP source IP, NOT
# the packet — hxtrackd reads `saddr->SIN_ADDR.S_ADDR`. So this
# registration shows up in listings as the container's internal
# IP (whatever Docker assigned to the bridge interface). Tests
# assert on name+port+nusers, not on the IP, which keeps them
# deterministic across container restarts.

import socket
import struct
import sys
import time

# Seed entry. These come out in the v1 listing payload as the
# Pascal-string name + description fields.
SERVER_NAME = b"hxtrackd test server"
# ASCII-only — Python bytes literals reject non-ASCII chars, and
# keeping the fixture ASCII also dodges the MacRoman -> UTF-8
# transcoding hx_tracker_server_new_v1 does on incoming v1
# records (which would mangle UTF-8 multibyte bytes if we used
# them here).
SERVER_DESC = b"Tier 3 fixture -- pinned by tests/hxtrackd"
SERVER_PORT = 5500
SERVER_USERS = 4
SERVER_ID = 0x12345678
# The first 2 bytes of an HTRK registration are the REGISTRATION
# protocol version (0x0001 for v1, 0x5801 for the "X1" extended
# v2), NOT the HTLS protocol version of the server doing the
# registration. mhxd's hxtrackd/tracker.c::tracker_udp_ready_read
# dispatches on this byte and silently drops anything that isn't
# 0x0001 or 0x5801. A test-fixture seed using 0x00b2 (Hotline
# 1.7.8) here would silently no-op — the registration never lands,
# the listing comes back empty, and the Tier 3 test fails with
# "no records" looking like a probe-fallback bug.
HTRK_REG_VERSION = 0x0001

TRACKER_HOST = "127.0.0.1"
TRACKER_UDP_PORT = 5499
HEARTBEAT_SECS = 60   # well under the 24h expiry


def build_registration():
    """Pack the htrk registration datagram (v1)."""
    hdr = struct.pack(
        ">HHHHI",
        HTRK_REG_VERSION,
        SERVER_PORT,
        SERVER_USERS,
        0,             # reserved
        SERVER_ID,
    )
    nlen = min(len(SERVER_NAME), 31)
    dlen = min(len(SERVER_DESC), 255)
    return (
        hdr
        + bytes([nlen])
        + SERVER_NAME[:nlen]
        + bytes([dlen])
        + SERVER_DESC[:dlen]
    )


def main():
    pkt = build_registration()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sent = 0
    while True:
        try:
            s.sendto(pkt, (TRACKER_HOST, TRACKER_UDP_PORT))
            sent += 1
            # First-five loud, then every 60th to avoid log spam in
            # `docker logs` once we know the tracker is up.
            if sent <= 5 or sent % 60 == 0:
                print(
                    f"[seed-tracker] sent registration #{sent} "
                    f"({len(pkt)} bytes)",
                    flush=True,
                )
        except OSError as e:
            print(f"[seed-tracker] sendto failed: {e}", file=sys.stderr,
                  flush=True)
        time.sleep(HEARTBEAT_SECS)


if __name__ == "__main__":
    main()
