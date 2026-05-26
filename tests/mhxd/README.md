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
accounts pre-configured. Both have blank passwords.

## Ports

| Port | Protocol | Purpose                          |
|------|----------|----------------------------------|
| 5498 | TCP      | HTRK tracker (only used by hxtrackd, not hxd) |
| 5500 | TCP      | HTLS — main client connection    |
| 5501 | TCP      | HTXF — file transfer subchannel  |

5498 is exposed by the image but only useful if you swap the
container's CMD to launch `hxtrackd` instead of `hxd`.

## Banner configuration

Hotline servers advertise a per-connection banner image after the
client agrees to the server agreement. Two modes on the wire:

- **URL mode** — server tells the client a URL to fetch the image
  from over HTTP. Default.
- **File mode** — server holds the image bytes and ships them over
  the HTXF subchannel (port 5501) after the client sends
  `HTLC_HDR_DOWNLOAD_BANNER`.

The entrypoint script reads three environment variables at startup
and patches `hxd.conf`'s `banner` block accordingly:

| Var           | Values                            | Default                                                    |
|---------------|-----------------------------------|------------------------------------------------------------|
| `BANNER_MODE` | `URL`, `GIFf`, `JPEG`             | `URL`                                                      |
| `BANNER_URL`  | any URL (URL mode only)           | `https://placehold.co/468x60/png?text=GtkHx+Test+Banner`   |
| `BANNER_FILE` | path inside the container         | `/opt/mhxd/run/banner.jpg` (JPEG) / `banner.gif` (GIFf)    |

The image is fetched from placehold.co at build time and baked
into the image (`/opt/mhxd/run/banner.jpg` and `.gif`), so HTXF
mode works without runtime network access.

Examples:

```sh
# Default: URL mode, placehold.co fixture
docker run --rm -p 5500:5500 -p 5501:5501 gtkhx-mhxd

# File mode, baked-in JPEG
docker run --rm -p 5500:5500 -p 5501:5501 \
    -e BANNER_MODE=JPEG gtkhx-mhxd

# File mode, GIF, bring your own banner
docker run --rm -p 5500:5500 -p 5501:5501 \
    -e BANNER_MODE=GIFf \
    -e BANNER_FILE=/opt/mhxd/run/custom.gif \
    -v $PWD/my_banner.gif:/opt/mhxd/run/custom.gif:ro \
    gtkhx-mhxd
```

## Layout

```
tests/mhxd/
├── Dockerfile              build recipe
├── docker-entrypoint.sh    runtime banner-block patcher
├── README.md               this file
├── conf/
│   ├── hxd.conf            server config (ident=0, version=185, nospam=no)
│   └── accounts/
│       └── guest/
│           └── UserData    binary blob with the access bits we need
└── patches/
    └── folder-xfer-size.patch  fix for an upstream copy/paste typo in
                                folder transfers
```

The Dockerfile pulls fresh mhxd master, applies the patch, then
overlays `conf/` onto the upstream `run/hxd/` skeleton. This is
deliberately layered so:

- Configuration changes are one-file edits to `conf/hxd.conf`
  (no sed pipelines).
- The binary `UserData` blob is regeneratable by running the
  bytewise edits inline (see comments in the Dockerfile's earlier
  history), but it's checked in so it's auditable in git.
- The upstream-source bugfix is a real patch file we can submit
  upstream when convenient.

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
