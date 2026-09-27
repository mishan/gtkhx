# hlservd test server

[hlservd](https://github.com/mishan/hotline-docker/tree/main/images/hlservd)
is the Hotline Server 1.9.5 as a headless POSIX daemon, ported from the
Hotsprings 2003 GPL release by the author of Underline Hotline. It is the
rig's stand-in for the original 1.9 server: the Windows and Mac 1.9
servers need Wine or an emulator, and this doesn't.

## Build and run

```sh
docker build -t gtkhx-hlservd tests/hlservd
# or: tests/build.sh hlservd
docker run -d --network host --name gtkhx-hlservd gtkhx-hlservd
```

The base image comes from hotline-docker (`ghcr.io/mishan/hlservd`), which
pins the upstream binary by sha256.

| Port | |
|---|---|
| 5530/tcp | Hotline |
| 5531/tcp | File transfers |
| 5532, 5533/tcp | HTTP tunnel |

## Configuration

`conf/Settings.ini` moves the ports clear of the other servers and sets
`TrustLoopback = true`: the server bans an address that opens more than
about ten connections in thirty seconds, and the test suites open far more
than that from 127.0.0.1. `files/` seeds the root with a fixture file.

## Accounts

The server makes them on its first start: `guest`, and `admin`, which logs
in with no password from the machine running the server. Host networking
makes the tests' connections local, so the end-to-end suite uses `admin`
as is.

## What it doesn't do

Per its own README, the Linux build keeps no classic Mac file data (type
and creator codes, Finder comments, resource forks), so files list with
empty type codes.
