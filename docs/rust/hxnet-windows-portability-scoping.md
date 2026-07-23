# hxnet Windows portability — raw-fd removal scoping

**Goal:** make the `hxnet` crate compile and run on Windows by removing the
raw-file-descriptor FFI surface it still carries. Track: `claude/hxnet-windows-scope`
(this doc), implementation on a follow-up branch.

**Decision (locked in with Misha):** *Option B — remove the raw-fd FFI entries
entirely*, not merely `#[cfg(unix)]`-gate them. No raw fd crosses the FFI on any
platform when we're done.

---

## TL;DR — the problem is smaller than it looks

The headline "hxnet still works with raw FDs" is true only of **test-support
code**. The production client is already fd-free.

Every `hxnet_*` entry the running binary calls takes a host/port (and connects
*inside* Rust via `connect::resolve_and_connect`) or an opaque handle — never an
fd. Confirmed by grepping every `hxnet_*` call site in `src/*.c` (excluding
tests and `extern` declarations):

| Path | Production FFI entry | fd? |
|---|---|---|
| Control channel | `hxnet_connection_open_plaintext` / `_open_hope` / `_open_plaintext_tls` | no — host/port |
| File transfer (HTXF) | `hxnet_htxf_connect` | no — host/port |
| Tracker | `hxnet_tracker_fetch_open` | no — host/port |
| Banner (URL + file) | `hxnet_banner_fetch_open`, `hxnet_htxf_connect` | no — host/port |
| Frames / handles | `hxnet_connection_send_frame` / `_destroy`, `hxnet_frame_free`, `hxnet_htxf_read/write/close/abort*`, `hxnet_hope_aead_*` | no — opaque |

This is the Phase G / delete-old-connect end state: the whole connect lifecycle
(DNS, TCP, magic, LOGIN, HOPE, TLS-from-byte-zero) lives in Rust. C hands over
strings, not sockets. There is no `dup()` / `socket()` / `g_socket_get_fd()`
handoff feeding hxnet anywhere in the production tree.

So the raw-fd surface that blocks a Windows build is confined to (a) two
**unconditional** `use std::os::unix::io` imports and (b) four **fd-adopting FFI
functions kept solely for tests**.

---

## Inventory of the raw-fd surface

### The two unconditional imports (the actual compile blocker)

```
rust/crates/hxnet/src/ffi.rs:71    use std::os::unix::io::FromRawFd;
rust/crates/hxnet/src/htxf.rs:345  use std::os::unix::io::FromRawFd;
```

`std::os::unix` does not exist on Windows, so these fail `cargo build` for a
`*-pc-windows-*` target before anything else is even reached. They exist only to
support the fd-adopting functions below.

### The fd-adopting FFI functions (all test-only in production)

| Symbol | File:line | Adopts | Called by |
|---|---|---|---|
| `hxnet_connection_spawn_fd` | `ffi.rs:281` | `TcpStream::from_raw_fd` | `tests/unit/test_hxnet_ffi.c` (AF_UNIX socketpair) |
| `hxnet_connection_spawn_fd_with_callback` | `ffi.rs:779` | same | `tests/unit/test_hxnet_ffi.c` |
| `hxnet_connection_spawn_fd_with_transforms_and_callback` | `ffi.rs:1213` | same | **dead** — only an `extern` decl in `src/hxnet_bridge.c`, no call site |
| `hxnet_htxf_open` | `htxf.rs:682` | same | Tier-3 integration tests (see below) |

`hxnet_htxf_connect` (`htxf.rs:774`) is a **strict superset** of
`hxnet_htxf_open`: same `tls` / `preamble` / `hope_aead` / `xfer_ref` /
`verify_cert` parameters, plus it does the connect (and optional SOCKS tunnel)
in-process. Any `hxnet_htxf_open(fd, …)` call can become
`hxnet_htxf_connect(host, port, NULL, 0, …)` by pointing it at the address the
test already connected to.

### Test code that constructs fds

Rust unit tests inside `htxf.rs` (`#[cfg(test)] mod tests`) that call
`into_raw_fd` on a loopback `TcpStream` to feed `hxnet_htxf_open`:

```
htxf.rs  ffi_open_write_read_close_over_socket   (~line 1183, into_raw_fd ~1246)
htxf.rs  ffi_abort_unblocks_parked_read          (~line 1290, into_raw_fd ~1310)
```

C tests that hand an fd to `hxnet_htxf_open`, all sourcing it from
`integration_connect_xfer()` (a raw `connect()` to the server's xfer port —
`srv->host` / `srv->xfer_port` are right there):

```
tests/integration/test_file_get.c
tests/integration/test_file_put.c
tests/integration/test_folder_roundtrip.c
tests/integration/test_hope_chacha20_banner.c
tests/integration/test_real_tls_banner.c
tests/unit/test_htxf_cancel.c
```

C tests that hand an fd to `hxnet_connection_spawn_fd*`:

```
tests/unit/test_hxnet_ffi.c   (socketpair — validates the fd-adopt path itself)
```

Note `c_int` used for return codes and status constants (`HXNET_RECV_*`,
`HXNET_SEND_*`, poll results, etc.) is **portable** and stays. Only
`std::os::unix::io` and the `fd:`-typed socket parameters are the problem.

### Everything else is already portable

`connect.rs` / `lifecycle.rs` / `tls.rs` / `tracker*.rs` / `banner_http.rs` use
`tokio::net` + std, no unix-isms. The blocking HTXF worker uses
`TcpStream::into_std()` + `set_read_timeout` — both portable. No `socketpair`,
`AF_UNIX`, `mio`, `socket2`, or `nix` in `src/`.

---

## Why Option B (remove) over Option A (cfg-gate)

Option A (`#[cfg(unix)]` on the imports + the four functions + the C tests that
call them) is the smaller diff and would unblock the Windows compile. Rejected
because:

- It leaves the raw-fd surface alive on Unix, so "move away from raw FDs" is only
  half-done and a future contributor can still reach for the fd path.
- It creates a `cfg`-split test matrix (some tests exist only on Unix), which is
  exactly the kind of platform skew we want to avoid while `ports.yml` is young.

Option B is achievable precisely *because production already proved the
connect-based path*. The fd entries are a convenience for injecting a
pre-connected socket into a test — and both `hxnet_htxf_connect` (loopback
connect) and `Connection::spawn` / `spawn_boxed` (in-memory `tokio::io::duplex`,
already used throughout `tests/duplex.rs`) give tests a fd-free way to do the
same thing.

---

## Work items

Ordered so the crate compiles on Windows early, then the test suites are made
green on Unix.

1. **Migrate the C integration tests to `hxnet_htxf_connect`.**
   Replace each `int xfd = integration_connect_xfer(); … hxnet_htxf_open(xfd,
   tls, host, …)` with `hxnet_htxf_connect(srv->host, len, srv->xfer_port, NULL,
   0, tls, …)`. Files: `test_file_get.c`, `test_file_put.c`,
   `test_folder_roundtrip.c`, `test_hope_chacha20_banner.c`,
   `test_real_tls_banner.c`, `test_htxf_cancel.c`. Consider a single
   `integration_htxf_connect(ref, tls, …)` helper in `integration_harness.c` so
   the connect+preamble shape lives in one place (mirrors how
   `integration_connect_xfer` already centralizes the raw connect).

2. **Migrate / retire `tests/unit/test_hxnet_ffi.c`.**
   Its socketpair cases exist to validate `hxnet_connection_spawn_fd*` — which
   we're deleting. Either point them at a loopback `hxnet_connection_open_tcp`
   (with a tiny in-test listener), or drop them in favour of the existing Rust
   `tests/duplex.rs` coverage of the same actor-loop behaviour. The FFI
   ABI-pin checks in this file for `HxnetTransformConfig` should move to a
   surviving entry or become a standalone `_Static_assert` test.

3. **Migrate the `htxf.rs` `#[cfg(test)]` fd cases.**
   `ffi_open_write_read_close_over_socket` and `ffi_abort_unblocks_parked_read`
   switch from `into_raw_fd` + `hxnet_htxf_open` to either a loopback
   `hxnet_htxf_connect` or an in-memory duplex against the safe `HtxfChannel`
   API. The abort test just needs a parked read to unblock — a duplex pipe with
   no writer reproduces that without a socket.

4. **Delete the raw-fd FFI surface.**
   Remove `hxnet_htxf_open`, `hxnet_connection_spawn_fd`,
   `_spawn_fd_with_callback`, `_spawn_fd_with_transforms_and_callback`, and the
   two `use std::os::unix::io::FromRawFd` imports. Remove the now-dead `extern`
   declaration of `hxnet_connection_spawn_fd_with_transforms_and_callback` in
   `src/hxnet_bridge.c` (and any stale externs in `test_hxnet_ffi.c`). This is
   the step that turns the `ports.yml` `windows-app` `ninja` step green for
   hxnet.

5. **Update `tests/meson.build`.**
   Drop the socketpair/`hxnet_htxf_open` commentary and any test registration
   that no longer applies; confirm the migrated tests still register on the
   right tiers.

6. **Confirm on `ports.yml`.**
   The validation vehicle already exists — `windows-app` (MSYS2 UCRT64 full
   meson build, `-Dtests=false`) is what compiles hxnet on Windows today and
   whose failure this work fixes. After the change, fire `ports.yml` manually
   (`gh workflow run ports.yml`) and confirm `windows-app` + `macos-app` reach
   the hxnet compile clean. No new CI job is needed for this pass.

---

## Interaction with `ports.yml` (already on `main`)

`ports.yml` (manual `workflow_dispatch` for now) already anticipates this work
and documents it as a known blocker:

> *"hxnet — needs glib/pkg-config, so it builds only in these app jobs (not the
> bare tripwire); its unix RawFd FFI seams may still block the Windows build."*

Two consequences for scope:

- **hxnet is validated by the full-app jobs, not the bare `rust-portability`
  tripwire.** The tripwire (`cargo test` of `PORTABLE_CRATES` on
  windows/macos-latest, no GTK) deliberately excludes hxnet because it pulls
  `glib` via `hxbridge`. So the exit criterion for *this* work is "hxnet
  compiles in `windows-app`," not "hxnet joins `PORTABLE_CRATES`."

- **Since `windows-app` runs `-Dtests=false`,** the C test callers of
  `hxnet_htxf_open` do not gate the Windows build — only the non-test crate code
  does (the two imports + four `pub extern` fns). That's why work items 1–3
  (test migration) are about keeping the **Linux** tiers green after the
  deletion in item 4, not about unblocking Windows. Item 4 alone unblocks
  Windows; items 1–3 keep `tests.yml` from going red.

---

## Out of scope (name them so they're not a surprise)

- **Decoupling hxnet's core from glib.** hxnet depends on `glib` (for
  `g_critical!` error logging) and `hxbridge` (shared tokio runtime + the
  GLib-main-context event ferry). That compiles on Windows *with* GLib present
  (MSYS2/gvsbuild), which is why the full-app path works — but it's what keeps
  hxnet out of the bare no-GTK tripwire. Making hxnet a standalone, glib-free
  crate (inject a logging callback, abstract the runtime handle) is a separate,
  larger follow-up. Not required to reach the Windows *app* build.

- **The whole-app Windows/macOS port.** `gtkhx.c`'s GIOChannel fd plumbing,
  `libseccomp` (Linux-only, needs a `host_machine.system()` guard in
  `meson.build:158`), `gsound` (no MSYS2 package), and the general C portability
  sweep are tracked by `ports.yml`'s probe steps, not here. This doc is strictly
  hxnet's raw-fd surface.

- **`c_int` return-code constants.** Portable; explicitly staying.

---

## Exit criteria

- No `std::os::unix` (or any `*::os::fd::RawFd` / `from_raw_fd` / `into_raw_fd`)
  in `rust/crates/hxnet/src/` — production or test.
- `hxnet_htxf_open` and the three `hxnet_connection_spawn_fd*` symbols are gone;
  no `extern` decl of them survives in `src/` or `tests/`.
- `cargo build -p hxnet` succeeds for a `*-pc-windows-*` target (given GLib on
  the runner).
- `ports.yml` `windows-app` and `macos-app` reach the hxnet compile without a
  RawFd error.
- `tests.yml` (all Linux tiers) stays green: the migrated HTXF tests exercise
  the same transfer/abort/TLS-banner behaviour through `hxnet_htxf_connect` /
  the safe Rust API.
