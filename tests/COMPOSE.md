# Multi-server test rig (docker-compose)

One command brings up both Hotline servers and both trackers, with the
servers registered against both trackers, so the tracker-listing and
registration paths can be exercised end-to-end without standing up four
containers by hand.

## What's in the rig

| Service    | Role             | Host ports                          | Connect as |
|------------|------------------|-------------------------------------|------------|
| `mhxd`     | Hotline server   | 5500 (HTLS), 5501 (HTXF)            | `localhost:5500` |
| `janus`    | Hotline server   | 5510/5511, 5610/5611 (TLS), 5514/udp | `localhost:5510` |
| `argus`    | Tracker (v1/2/3) | 5498 (HTRK), 6498 (TLS), 5499/udp   | tracker host `localhost:5498` |
| `hxtrackd` | Tracker (v1)     | 5598→5498 (HTRK), 5599→5499/udp     | tracker host `localhost:5598` |

Host ports match the per-container READMEs and the Tier 3 matrix
(`tests/integration/server_matrix.c`, `tracker_matrix.c`), so a target
is reachable at the same host port whether it was launched standalone
or through this rig.

## Scripts

```sh
cd tests

./build-all.sh            # build all four images (forwards args, e.g. --no-cache)
./run.sh                  # rebuild + tear down + restart the whole rig
./run.sh --no-cache       # same, forcing a clean rebuild

# per-container builds (same canonical gtkhx-<name> tag compose uses)
./janus/build.sh
./argus/build.sh
./mhxd/build.sh
./hxtrackd/build.sh
```

`run.sh` runs `build-all.sh`, then `docker compose down --remove-orphans`,
then `docker compose up -d`, and prints the resulting `ps`. It auto-detects
the `docker compose` plugin and falls back to legacy `docker-compose`.

Direct compose use also works:

```sh
docker compose -f tests/docker-compose.yml up -d --build
docker compose -f tests/docker-compose.yml logs -f
docker compose -f tests/docker-compose.yml down
```

## How registration works

All four containers share a user-defined bridge network, so each
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

The default rig uses a bridge network rather than host networking
because the two trackers both listen on 5498/5499 and can't coexist on
the host's port table. The cost is Janus's WebRTC **voice** path
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
