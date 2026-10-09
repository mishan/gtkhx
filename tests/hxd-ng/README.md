# hxd-ng test server

[hxd-ng](https://github.com/mishan/hxd-ng) from the image its CI
publishes to GHCR (`ghcr.io/mishan/hxd-ng`), configured for GtkHx's Tier
3 rig. It is here for one reason: it implements the video extension
(`docs/capabilities-video.md` in its tree), so every video test runs
against it as well as Janus. It also serves voice — ICE-lite, on str0m —
which the video media tests use as a matter of course.

## Build

```sh
docker build -t gtkhx-hxd-ng tests/hxd-ng
# or: tests/build.sh hxd-ng
```

Nothing compiles: the Dockerfile starts `FROM` the published image,
pinned by its `sha-<commit>` tag and digest, and adds only `conf/` and
`entrypoint.sh`. hxd-ng's CI publishes an image for every
commit on its main, so bumping the pin means picking the new commit's
tag, resolving its digest (`docker buildx imagetools inspect
ghcr.io/mishan/hxd-ng:sha-<commit>`), and updating the `FROM` line.
Bump it on purpose, with the video and voice tests run against the new
one.

The published image runs as an unprivileged user (uid 10001), which owns
`/var/lib/hxd-ng` and `/run/hxd-ng`, where the entrypoint writes its
config.

## Run

```sh
docker run -d --network host --name gtkhx-hxd-ng gtkhx-hxd-ng
```

Host networking, like the rest of the rig, on ports clear of mhxd and
Janus:

| Port | |
|---|---|
| 5520/tcp | Hotline |
| 5521/tcp | File transfers and the banner (HTXF) |
| 5620/tcp | Hotline over TLS, with a self-signed certificate made on first start |
| 5621/tcp | HTXF over TLS |
| 5524/udp | Voice and video media |

## Configuration

`conf/hxd-ng.toml` turns on voice and video with the spec's default
ceilings — one screen-share slot a room, so the second-sharer refusal is
testable. It also serves a file-mode banner and a files area holding
`test.txt`, `test_folder/` and an `Uploads/` folder, for the HOPE, TLS,
transfer and folder transfer tests. `conf/accounts/` has
two accounts:

- `guest` (the harness's login): voice, `video_chat` and `screen_share`.
- `novideo` / `novideo`: voice but neither video bit, for the access
  refusal tests.

The voice section's `advertise` line is rewritten at every start by
`entrypoint.sh` to the host's own addresses. hxd-ng is ICE-lite and
offers only what it is told; libnice never gathers a loopback candidate,
so advertising `127.0.0.1` leaves the client's checks with nowhere to go.
