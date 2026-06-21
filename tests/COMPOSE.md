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

The rig uses a bridge network rather than host networking because the
two trackers both listen on 5498/5499 and can't coexist on the host's
port table. The cost is Janus's WebRTC **voice** path (5514/udp): ICE
negotiation against 127.0.0.1 needs `--network=host` (see
`tests/janus/README.md`). For voice manual-testing, keep using the
standalone host-net invocation:

```sh
docker run --rm --network=host gtkhx-janus
```

Everything else — chat, PM, news, files, banner, HOPE/AEAD, TLS, and
tracker listing + registration — works over the bridge.
