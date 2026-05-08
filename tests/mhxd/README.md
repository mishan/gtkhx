# mhxd test server

A Docker container that builds the latest [mhxd](https://github.com/kangsterizer/mhxd)
from upstream and runs it as a Hotline server we can point GtkHx at.

## Why

GtkHx needs a controlled, repeatable Hotline server target for integration
testing — connect, log in, fetch user list, exchange chat messages, hit the
file/news endpoints, disconnect. `hlserver.com` and Badmoon are useful for
"does it work in the wild" checks but they're third-party and flaky to
script against. mhxd is the canonical reference codebase (same family
GtkHx's protocol stack came from), open-source, and runs locally.

## Build

From the repo root:

```sh
docker build -t gtkhx-mhxd tests/mhxd
```

The build pulls mhxd's `master` branch fresh on every run (no pinned
commit), so this same Dockerfile gives us whatever upstream mhxd
offers today. Build takes a couple of minutes — autotools regen +
the full tree compile.

## Run

```sh
docker run --rm -p 5500:5500 -p 5501:5501 gtkhx-mhxd
```

That's the foreground mode — Ctrl+C kills the container. Logs go
to stdout. Add `-d` for detached mode if you want it living in the
background.

GtkHx connects with:

```
Server:  localhost:5500
Login:   guest
Pass:    (empty)
```

The shipped `run/hxd/` skeleton from mhxd has `guest` and `admin`
accounts pre-configured. The `admin` password is whatever mhxd's
default seed sets — check `run/hxd/accounts/admin/access` and
related files inside the container if you need elevated testing.

## Ports

| Port | Protocol | Purpose                          |
|------|----------|----------------------------------|
| 5498 | TCP      | HTRK tracker (only used by hxtrackd, not hxd) |
| 5500 | TCP      | HTLS — main client connection    |
| 5501 | TCP      | HTXF — file transfer subchannel  |

5498 is exposed by the image but only useful if you swap the
container's CMD to launch `hxtrackd` instead of `hxd`.

## Iterate

If you want to test a different branch / fork / patched mhxd, edit
the `git clone` line in the Dockerfile to point elsewhere. Or build
locally and `docker build --build-arg ...` if we add an arg later.

If mhxd's autotools graph fails on a parallel build (it has in the
past), the Dockerfile already falls back to a single-job rebuild via
`make -j$(nproc) || make`.

## Connecting GtkHx

In the running app:

1. Toolbar → Connect (or Ctrl+K).
2. Server: `localhost`, Port: `5500`.
3. Login / Password as above.
4. Click Connect.

`GTKHX_DEBUG=proto ./build/src/gtkhx` shows the wire conversation
in the terminal — useful for diagnosing whatever protocol-level
quirk you're chasing against this controlled mhxd target.
