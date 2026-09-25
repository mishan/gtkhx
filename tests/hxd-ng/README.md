# hxd-ng test server

[hxd-ng](https://github.com/mishan/hxd-ng) built from source at a pinned
revision, configured for GtkHx's Tier 3 rig. It is here for one reason:
it is the only Hotline server that implements the video extension
(`docs/capabilities-video.md` in its tree), so every video test runs
against it. It also serves voice — ICE-lite, on str0m — which the video
media tests use as a matter of course.

## Build

```sh
docker build -t gtkhx-hxd-ng tests/hxd-ng
# or: tests/build.sh hxd-ng
```

A full release build of `hxd` (a few minutes; cargo's registry and target
directories are BuildKit cache mounts, so a rebuild is quicker). The
revision is the `HXD_NG_REV` build argument at the top of the Dockerfile.
Bump it on purpose, with the video and voice tests run against the new
one.

## Run

```sh
docker run -d --network host --name gtkhx-hxd-ng gtkhx-hxd-ng
```

Host networking, like the rest of the rig, on ports clear of mhxd and
Janus:

| Port | |
|---|---|
| 5520/tcp | Hotline |
| 5524/udp | Voice and video media |

## Configuration

`conf/hxd-ng.toml` turns on voice and video with the spec's default
ceilings — one screen-share slot a room, so the second-sharer refusal is
testable. `conf/accounts/` has two accounts:

- `guest` (the harness's login): voice, `video_chat` and `screen_share`.
- `novideo` / `novideo`: voice but neither video bit, for the access
  refusal tests.

The voice section's `advertise` line is rewritten at every start by
`entrypoint.sh` to the host's own addresses. hxd-ng is ICE-lite and
offers only what it is told; libnice never gathers a loopback candidate,
so advertising `127.0.0.1` leaves the client's checks with nowhere to go.
