#!/usr/bin/env python3
# Diagnostic: confirm the Hotline servers in the compose rig actually
# registered with a v1 tracker.
#
# Queries a tracker's HTRK v1 listing (the same wire shape
# tests/integration/test_tracker_v1.c drives) and prints every record's
# name, port and user count. Exits 0 if at least --min-records records
# came back, else 1. Intended to run as a `continue-on-error` CI step:
# it surfaces whether mhxd / Janus registration landed without gating
# the merge on a timing-sensitive, newly-introduced behaviour.
#
# Why v1 only: both mhxd and Janus register over the classic HTRK UDP
# path, which a v1 tracker (hxtrackd) re-serves verbatim in its v1
# listing — so a v1 query is the simplest end-to-end proof the
# registration datagram was received and stored. (Argus also receives
# the registrations; verifying its v3 listing is left to the richer
# Tier 3 test_tracker_v3 binary.)
#
# Usage:
#   check-tracker-registration.py --host 127.0.0.1 --port 5498 \
#       --min-records 2 [--timeout-secs 30]
#
# The hxtrackd container is seeded with one fixture record ("hxtrackd
# test server"); with mhxd (and ideally Janus) registering, the listing
# should grow to >= 2. The script polls until --timeout-secs elapses.

import argparse
import socket
import struct
import sys
import time

HTRK_V1_MAGIC = b"HTRK\x00\x01"


def fetch_listing(host, port, recv_timeout=3.0):
    """Connect, send the v1 magic, return the list of (name, port,
    nusers) records. Raises on connect / short-read failures."""
    s = socket.create_connection((host, port), timeout=recv_timeout)
    s.settimeout(recv_timeout)
    try:
        s.sendall(HTRK_V1_MAGIC)
        hdr = recv_exact(s, 14)
        # nservers is a u16 BE at offset [10..11] of the v1 reply header
        # (matches the pure parser tests/integration/test_tracker_v1.c
        # reads).
        nservers = struct.unpack(">H", hdr[10:12])[0]

        records = []
        for _ in range(nservers):
            # Read the 8-byte head first and test the padding sentinel
            # BEFORE consuming the rest of the fixed prefix. The v1
            # stream can carry all-zero 8-byte padding slots that must be
            # skipped without eating the next record's bytes — exactly
            # how tests/integration/test_tracker_v1.c and
            # src/network.c's reader handle it. (padding == first byte 0,
            # per tracker_record_is_padding.) Reading 11 unconditionally
            # would desync the stream by 3 bytes on the first padding
            # slot and make the poll fail spuriously.
            head = recv_exact(s, 8)
            if head[0] == 0:
                continue                     # padding slot: 8 bytes only
            rest = recv_exact(s, 3)          # reserved(2) + name_len(1)
            fixed = head + rest
            # 11-byte fixed record: [0..3] addr, [4..5] port (BE),
            # [6..7] nusers (BE), [8..9] unused, [10] name_len —
            # matches parse_tracker_record_fixed in the Rust/C parsers.
            srv_port = struct.unpack(">H", fixed[4:6])[0]
            nusers = struct.unpack(">H", fixed[6:8])[0]
            name_len = fixed[10]
            name = recv_exact(s, name_len).decode("latin-1") if name_len else ""
            (dlen,) = struct.unpack(">B", recv_exact(s, 1))
            if dlen:
                recv_exact(s, dlen)          # skip description
            records.append((name, srv_port, nusers))
        return records
    finally:
        s.close()


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("short read from tracker")
        buf += chunk
    return buf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5498)
    ap.add_argument("--min-records", type=int, default=2)
    ap.add_argument("--timeout-secs", type=int, default=30)
    args = ap.parse_args()

    deadline = time.monotonic() + args.timeout_secs
    last = []
    while time.monotonic() < deadline:
        try:
            last = fetch_listing(args.host, args.port)
        except (OSError, EOFError) as e:
            print(f"[check] query failed ({e}); retrying", flush=True)
            time.sleep(1)
            continue
        if len(last) >= args.min_records:
            break
        time.sleep(1)

    print(f"[check] {args.host}:{args.port} returned {len(last)} record(s):",
          flush=True)
    for name, port, nusers in last:
        print(f"          name={name!r} port={port} users={nusers}",
              flush=True)

    if len(last) >= args.min_records:
        print(f"[check] OK (>= {args.min_records})", flush=True)
        return 0
    print(f"[check] FAIL: expected >= {args.min_records} record(s)",
          file=sys.stderr, flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
