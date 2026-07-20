# Multi-server test rig (docker-compose)

One command brings up both Hotline servers, both trackers, and a SOCKS5
proxy, with the servers registered against both trackers, so the
tracker-listing, registration, and SOCKS-connect paths can all be
exercised end-to-end without standing up five containers by hand.

## What's in the rig

| Service    | Role             | Host ports                          | Connect as |
|------------|------------------|-------------------------------------|------------|
| `mhxd`     | Hotline server   | 5500 (HTLS), 5501 (HTXF)            | `localhost:5500` |
| `janus`    | Hotline server   | 5510/5511, 5610/5611 (TLS), 5514/udp | `localhost:5510` |
| `argus`    | Tracker (v1/2/3) | 5498 (HTRK), 6498 (TLS), 5499/udp   | tracker host `localhost:5498` |
| `hxtrackd` | Tracker (v1)     | 5598→5498 (HTRK), 5599→5499/udp     | tracker host `localhost:5598` |
| `socks`    | SOCKS5 proxy     | 1080 (host networking)              | `socks5://localhost:1080` |

`socks` (microsocks) backs `tests/integration/test_integration_socks.c`,
which routes the production connect path through the proxy to mhxd. It
runs on **host networking**, not the bridge — see the SOCKS section
below for why.

Host ports match the per-container READMEs and the Tier 3 matrix
(`tests/integration/server_matrix.c`,
`tests/integration/tracker_matrix.c`), so a target is reachable at the
same host port whether it was launched standalone or through this rig.

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
**Docker Compose v2** — these files use v2-only features (top-level
`name:`; the Janus host override uses the `!reset` tag, which needs
**v2.24+**). `run.sh` prefers the `docker compose` plugin, accepts a
standalone `docker-compose` only if it reports v2+, and exits with a
clear message on Compose v1.

Direct compose use also works:

```sh
docker compose -f tests/docker-compose.yml up -d --build
docker compose -f tests/docker-compose.yml logs -f
docker compose -f tests/docker-compose.yml down
```

## How registration works

The four Hotline containers share a user-defined bridge network, so each
resolves the others by service name. Registration is driven entirely by
a `TRACKERS` env var the compose file sets on each server — no image is
hard-wired to the rig, so standalone `docker run` of any container is
unchanged.

- **mhxd** — the entrypoint patches `hxd.conf`'s `trackers` directive
  from `TRACKERS` (default `127.0.0.1` when unset). The rig sets
  `TRACKERS="argus, hxtrackd"`. `tracker_register yes` is already on.
- **Janus** — the entrypoint flips `EnableTrackerRegistration: true` and
  rewrites the `Trackers:` YAML list from `TRACKERS` (a no-op when
  unset). The rig sets `TRACKERS="argus:5499,hxtrackd:5499"`.

Both servers send the classic HTRK UDP registration on port 5499, which
Argus (v1/v2/v3) and hxtrackd (v1-only) both accept. After the rig is up,
a tracker-listing fetch against either tracker returns the registered
servers (alongside Argus's three pinned `promoted_servers` fixtures).

Verify quickly:

```sh
docker compose -f tests/docker-compose.yml logs argus    | grep -i regist
docker compose -f tests/docker-compose.yml logs hxtrackd | grep -i regist
```

The advertised IP for each registration is the registering container's
bridge IP (both trackers read the UDP source address, not the packet),
so it isn't deterministic across restarts — assert on name/port/users,
not IP, exactly as the existing tracker tests do.

## Networking trade-off (voice)

The four Hotline containers run on a bridge network rather than host
networking because the two trackers both listen on 5498/5499 and can't
coexist on the host's port table. (The `socks` proxy is the one base
service on host networking — see its section below — but it's auxiliary,
not part of the Hotline core.) The cost is Janus's WebRTC **voice** path
(5514/udp): ICE negotiation against 127.0.0.1 needs host networking
(see `tests/janus/README.md`). Everything else — chat, PM, news, files,
banner, HOPE/AEAD, TLS, and tracker listing + registration — works over
the bridge.

### Enabling voice: host-networking override for Janus

The trackers stay on the bridge, but **Janus alone** can run with host
networking via an override file. Bring the rig up with voice enabled:

```sh
JANUS_HOST_NET=1 ./run.sh
# or directly:
docker compose -f docker-compose.yml -f docker-compose.janus-host.yml up -d
```

With the override, Janus joins the host network namespace: its listeners
(including 5514/udp) bind the host directly so ICE works, and since a
host-net container can't also be on the `hotline` bridge, it registers
with the trackers through their host-published ports on loopback —
`127.0.0.1:5499` (Argus) and `127.0.0.1:5599` (hxtrackd) — instead of by
service name. mhxd is unaffected and keeps registering by service name.

Requirements: a **Linux host** (host networking on Docker Desktop for
macOS/Windows is a limited beta with different loopback semantics) and
**Docker Compose v2.24+** (for the `!reset` tag the override uses). Full
rationale is in `docker-compose.janus-host.yml`.

The standalone host-net invocation also still works for one-off voice
testing without the rest of the rig:

```sh
docker run --rm --network=host gtkhx-janus
```

## The SOCKS proxy (also host networking)

The `socks` service (microsocks) is always part of the rig — the
integration suite's `test_integration_socks` routes GtkHx's production
connect path through it to mhxd. Like Janus's voice path it runs on
**host networking**, but for a different reason: the test asks the proxy
to CONNECT to `GTKHX_TEST_HOST:GTKHX_TEST_PORT` (`127.0.0.1:5500`), and
that `127.0.0.1` has to mean the host loopback where mhxd publishes 5500
— not the proxy container's own loopback. So the proxy must share the
host network namespace; a bridge proxy would dial its own 127.0.0.1 and
fail. It binds the host's `1080` directly (no `ports:` mapping, since
host-net services can't publish ports), exactly matching the standalone
`docker run --network host gtkhx-socks`.

Because it's host-net, the same **Linux-host** caveat as the Janus voice
override applies (Docker Desktop's host networking is a limited beta). It
does not need the `!reset` override, though — `socks` is a first-class
service in the base `docker-compose.yml`, so a plain `./run.sh` (or
`docker compose -f docker-compose.yml up`) brings it up.
