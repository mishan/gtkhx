# Multi-server test rig (docker-compose)

One command brings up both Hotline servers, both trackers, and a SOCKS5
proxy — all on host networking — with the servers registered against the
trackers, so the tracker-listing, registration, and SOCKS-connect paths
can all be exercised end-to-end without standing up five containers by
hand.

## What's in the rig

| Service    | Role             | Host ports                          | Connect as |
|------------|------------------|-------------------------------------|------------|
| `mhxd`     | Hotline server   | 5500 (HTLS), 5501 (HTXF)            | `localhost:5500` |
| `janus`    | Hotline server   | 5510/5511, 5610/5611 (TLS), 5514/udp | `localhost:5510` |
| `hxtrackd` | Tracker (v1)     | 5498 (HTRK), 5499/udp               | tracker host `localhost:5498` |
| `argus`    | Tracker (v1/2/3) | 5698 (HTRK), 6498 (TLS), 5699/udp   | tracker host `localhost:5698` |
| `socks`    | SOCKS5 proxy     | 1080                                | `socks5://localhost:1080` |

Every container runs on **host networking** (`network_mode: host`), so
each binds those ports on the host directly — no `-p` publishing, no
user-defined bridge. Host ports match the Tier 3 matrices
(`tests/integration/server_matrix.c`,
`tests/integration/tracker_matrix.c`).

The one wrinkle host networking creates — two processes can't share a
port — is why the trackers land where they do: hxtrackd's listen ports
are hardcoded compile-time constants (`HTRK_TCPPORT`/`HTRK_UDPPORT` =
5498/5499) that can't move, so the config-driven Argus is shifted up to
5698/5699 (+ TLS 6498) to keep out of its way.

## Scripts

```sh
cd tests

./build-all.sh            # build all five images (forwards args, e.g. --no-cache)
./run.sh                  # rebuild + tear down + restart the whole rig
./run.sh --no-cache       # same, forcing a clean rebuild

# single-container build (same canonical gtkhx-<name> tag compose uses)
./build.sh janus
./build.sh argus --no-cache
./build.sh mhxd --build-arg MHXD_REV=<sha>
./build.sh socks          # builds gtkhx-socks from tests/socks-proxy/
```

`run.sh` runs `build-all.sh`, then `docker compose down --remove-orphans`,
then `docker compose up -d`, and prints the resulting `ps`. It requires
**Docker Compose v2** (the compose file uses the top-level `name:` key);
it prefers the `docker compose` plugin, accepts a standalone
`docker-compose` only if it reports v2+, and exits with a clear message
on Compose v1.

Direct compose use also works:

```sh
docker compose -f tests/docker-compose.yml up -d
docker compose -f tests/docker-compose.yml logs -f
docker compose -f tests/docker-compose.yml down
```

## Why host networking everywhere

An earlier revision ran the Hotline core on a user-defined bridge (for
service-name DNS) with only Janus on host networking. That bridge adds a
routable host interface (e.g. `172.18.0.1`), and the host-networked
WebRTC **voice** test's ICE agent enumerates it as a dead-end candidate
— which broke the voice Tier 3 tests (peer never reaches CONNECTED).

Running every container on host networking removes the extra interface,
so ICE behaves exactly as it did under the old per-container CI. The cost
is that the trackers can't share a port (hence the Argus 5698/5699
shift), and there's no bridge DNS — so registration targets are plain
`127.0.0.1:<port>` loopback rather than service names.

Requirement: a **Linux host**. Host networking on Docker Desktop for
macOS/Windows is a limited beta with different loopback semantics.

## How registration works

Registration is driven by a `TRACKERS` env var the compose file sets on
each server — no image is hard-wired to the rig, so standalone
`docker run` of any container is unchanged.

- **mhxd** — the entrypoint patches `hxd.conf`'s `trackers` directive
  from `TRACKERS` (default `127.0.0.1` when unset). The rig sets
  `TRACKERS="127.0.0.1"`. hxd only ever registers on the fixed HTRK port
  5499, which on host loopback is **hxtrackd**. (It has no per-tracker
  port, so it can't reach Argus's 5699 — Janus covers that.)
- **Janus** — the entrypoint flips `EnableTrackerRegistration: true` and
  rewrites the `Trackers:` YAML list from `TRACKERS`. The rig sets
  `TRACKERS="127.0.0.1:5499,127.0.0.1:5699"`, so Janus registers with
  **both** hxtrackd (5499) and Argus (5699).

So hxtrackd receives mhxd + Janus (+ its own seeded fixture), and Argus
receives Janus. Both send the classic HTRK UDP registration.

Verify quickly:

```sh
docker compose -f tests/docker-compose.yml logs hxtrackd | grep -i regist
python3 tests/ci/check-tracker-registration.py --port 5498 --min-records 2
```

The advertised IP for each registration is the UDP source address (both
trackers read it off the packet's source, not the payload), so it isn't
deterministic — assert on name/port/users, not IP, as the tracker tests
do.

## The SOCKS proxy

The `socks` service (microsocks) backs the integration suite's
`test_integration_socks`, which routes GtkHx's production connect path
through the proxy to mhxd. The test asks the proxy to CONNECT to
`GTKHX_TEST_HOST:GTKHX_TEST_PORT` (`127.0.0.1:5500`), and that
`127.0.0.1` has to mean the host loopback where mhxd listens — which is
exactly what host networking gives it (the same reason everything else in
the rig is host-net). It binds the host's `1080` directly, matching the
standalone `docker run --network host gtkhx-socks`.
