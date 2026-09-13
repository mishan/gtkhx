# Mobius — Tier 3 test target

[Mobius](https://github.com/jhalter/mobius) is the most widely deployed
modern Hotline server: Classic Macs Hotline, MacSecret, VesperNet's
public listing and Greg Gant's hotline.semihosted.xyz all run it. It is
also the server GtkHx users are most likely to be on, which makes it the
one where an interop bug costs the most and surfaces the slowest — via a
user report, weeks later.

## What's different about this container

mhxd and Janus are built (or unpacked) by images we publish ourselves
from `hotline-docker`, and their Dockerfiles here are thin overlays on
those. Mobius publishes its own server image, so there is no base layer
to build first — `tests/mobius/Dockerfile` is the whole thing.

The upstream image is `FROM scratch`. There is no shell, no package
manager and nothing to `RUN`; the only way to configure it is to `COPY`
a file the server reads at startup. That rules out the build-time tricks
the other two use (Janus generates a TLS cert with `openssl`, mhxd pulls
banner fixtures with `curl`), which is why this container has no TLS
listener and no banner of its own — it serves the banner the upstream
image ships.

| | |
|---|---|
| Image | `gtkhx-mobius` |
| Base | `ghcr.io/jhalter/mobius-hotline-server:${MOBIUS_TAG}` |
| Pin | `MOBIUS_TAG=v0.23.1` (build arg) |
| HTLS port | 5520 |
| HTXF port | 5521 (Mobius derives it as bind + 1) |
| Account | `guest`, no password |

Upstream publishes one image tag per git tag, plus `edge` for master.
There is no floating `latest`, so the pin can't drift; bump `MOBIUS_TAG`
deliberately and run the suite, per the rig's pin policy:

```sh
tests/build.sh mobius --build-arg MOBIUS_TAG=v0.24.0
```

## Config overlay

Four files land on top of the upstream config tree at
`/usr/local/var/mobius/config`. Everything else — `Agreement.txt`,
`banner.jpg`, `Files/`, `Users/admin.yaml` — is upstream's.

- `conf/config.yaml` — names the server, turns tracker registration off,
  and empties `NewsFeeds` so opening a category can't reach the network
  mid-test.
- `conf/Users/guest.yaml` — upstream's guest with the five `News*` write
  bits turned on. The harness logs in as guest with no password, which
  is the only login shape that authenticates across all three rig
  servers; Mobius's `admin` has the full bitmap but a real password, and
  a password login is a different code path from the one every other
  Tier 3 test drives.
- `conf/ThreadedNews.yaml` — seeded with one category holding one
  article plus one folder, shaped like mhxd's `run/hxd/newsdir/` and
  using the same `irasshaimase` category name, so the seeded assertions
  read the same content whichever server is the default target.

Mobius rewrites `ThreadedNews.yaml` in place whenever a client mutates
the tree, so a long-lived container drifts from the fixture the same way
the mhxd one does. Rebuild it when a seeded assertion starts failing in
isolation.

## Why tracker registration is off

mhxd and Janus already cover registration from two directions — mhxd
reaches only hxtrackd (hxd registers on the fixed HTRK port 5499),
Janus reaches both hxtrackd and Argus. The tracker Tier 3 tests assert
on what those two publish. A third registrant would change those
listings without exercising a path the other two don't.

## Known interop finding

One thing this target found the moment the news tests were pointed at
it. It's recorded here because it explains an assertion elsewhere.

**`MAKENEWSDIR` sent as a single path panics the server.** Before GtkHx
sent `HTLC_HDR_MAKENEWSDIR` as `NEWSPATH` (the parent) plus `FILE_NAME`
(the new folder), it folded the new name into the path and sent that
alone. mhxd answers such a request with a plain ENOENT task error.
Mobius walks the path in `getCatByPath`, gets a zero-value entry for the
component that doesn't exist yet, and assigns into its nil `SubCats`
map:

```
level=ERROR msg=PANIC err="assignment to entry in nil map"
  mobius.(*ThreadedNewsYAML).CreateGrouping   threaded_news.go:37
  mobius.HandleNewNewsFldr                    handlers_news.go:137
```

`dontPanic` recovers, so the connection survives — but **no reply is
ever sent** and the folder is never created. A client that waits on the
reply waits forever. The Tier 3 news tests assert that a reply always
arrives, which is what pins this.
