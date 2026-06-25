# Tracker → hxnet scoping (Phase R3, work item 8)

Scoping for moving the tracker fetch off its hand-rolled C async state
machine and onto the `hxnet` (tokio) transport, the way the main
connection and HTXF subchannels already are.

## TL;DR / recommendation

- **Not on the critical path.** The tracker fetch is *already* off the
  pthread — it runs on the GLib main loop via `GSocketClient` async, not a
  worker thread. So this does **not** unblock retiring `gtkthreads.c`
  (that's `xfers.c`, the last `pthread_create`). Sequence this *after*
  the xfers.c conversion.
- **The real payoff is collapsing the second TLS stack.** Tracker TLS
  today goes through `g_socket_client_set_tls` (glib-networking / GnuTLS)
  plus the shared `on_socket_client_event` TOFU handler — a *different*
  TLS implementation than the main connection's `hxnet` rustls path. This
  migration unifies both on rustls + the one trust DB, and lets us delete
  ~1,100 lines of bespoke C async (the `network.c` tracker state machine
  and its helpers).
- **The Rust module owns the whole transport** — connect, TLS, handshake,
  read-loop, and the v3→v1 / TLS→plain fallback — rather than the
  HTXF-style "C connects, hands fd to Rust" split. We're explicitly *not*
  preserving SOCKS via GLib's `GProxyResolver` here; proxy support is a
  later `tokio-socks` add (already noted as the plan in
  `rust/crates/hxnet/src/connect.rs`), accepted as a temporary gap, not a blocker.

## Current state (what's already done)

The wire-format work is finished — only the transport orchestration is
still C:

- **Parsers/encoders are already Rust.** `hotline-proto::parse` carries
  both the v1 reply parsers (`parse_tracker_header`,
  `parse_tracker_record_fixed`, `tracker_record_is_padding`,
  `tracker_normalize_text`) and the full v3 set (handshake pack/parse,
  listing-request pack, response-header parse, per-record parse, TLV
  iteration). `src/tracker_parser.c`, `src/tracker_v3.c`, and
  `src/tracker_v3_meta.c` are thin C shims over those. **Nothing to port
  here.**
- **The transport state machine is C, on the main loop.**
  `src/network.c` (~lines 1343–2449) owns it: a `tracker_run_ctx` walks
  `gtkhx_prefs.tracker[]` serially, each `tracker_fetch_ctx` runs the
  protocol through chained `GSocketClient` / `GInputStream` async
  callbacks. It implements:
  - **Probe-then-fallback:** send the 8-byte v3 handshake, watchdog
    (`GTKHX_TRACKER_V3_PROBE_MS`, default 2000 ms); on timeout close and
    reopen with the 6-byte v1 magic (real pre-spec v1 trackers silently
    reject the `0x0003` version byte).
  - **TLS with per-tracker verdict cache:** try TLS first, fall back to
    plain on handshake failure, and remember `OK` / `NO` per tracker URL
    so Refresh doesn't re-pay a failed handshake. TLS uses the shared
    `on_socket_client_event` TOFU handler writing into the same
    `known_hosts` file as the main session.
  - **Streaming emit:** v1 emits record-by-record as it reads; v3 reads
    the whole (capped, 16 MiB) payload then walks it. Both drive the
    `tracker-batch-begin` and `tracker-server-create` GtkhxSession
    signals.
- **The UI is independent.** `src/tracker.c` only consumes those two
  signals and the `HxTrackerServer` boxed event (`tracker_event.c`). It
  never touches the socket.

## What moves vs. what stays

**Stays untouched:**

- `src/tracker.c` (the GtkColumnView/section UI).
- The `tracker-batch-begin` / `tracker-server-create` signals and the
  `HxTrackerServer` boxed type (`tracker_event.c`).
- The Rust parsers in `hotline-proto` (the new module *calls* them).
- The public entry points `hx_tracker_list_async()` /
  `tracker_kill_threads()` keep their signatures — only their bodies
  change to drive the hxnet handle.

**Moves into a new `hxnet` `tracker` module (tokio):**

- Transport: connect (tokio `TcpStream`), optional TLS (`hxnet::tls`),
  and the serial walk over the configured tracker URLs.
- Per-connection protocol: send handshake, read the response, branch
  v3/v1, run the read-loop, call the `hotline-proto` parsers, surface
  records.
- Probe watchdog, v3→v1 fallback, TLS→plain fallback, the verdict cache.

**Deleted from C once the module lands:**

- The `network.c` tracker state machine (the `tracker_run_ctx` /
  `tracker_fetch_ctx` chain, ~1,100 LOC), the tracker `tls_endpoint`
  plumbing, the verdict-cache helpers, the watchdog.

## Design

**The Rust `tracker` module owns the whole transport** — connect (tokio
`TcpStream`), optional TLS (the existing `hxnet::tls` rustls path),
handshake, read-loop, and both fallbacks. The C side shrinks to "start a
fetch over these tracker URLs / cancel it / drain events." This is the
cleaner end state and matches where the main connection lifecycle already
lives; we're deliberately not splitting the connect out to C just to ride
GLib's `GProxyResolver`.

Because Rust owns connect, the tracker's fallbacks stay entirely inside
the module — no fd round-trip to C:

- **v3→v1 fallback:** send the v3 handshake, watchdog the response; on
  timeout, drop the connection and reopen with the v1 magic.
- **TLS→plain fallback:** try rustls first; on handshake failure, reopen
  plaintext. The per-tracker `OK`/`NO` verdict cache lives in the module
  (process-scoped map keyed by URL).
- The serial walk over `gtkhx_prefs.tracker[]` is a `for` loop in the
  module's tokio task; C just passes the URL list at open time.

**Proxy note:** `tokio::net::TcpStream::connect` is not proxy-aware, so
this drops the transparent SOCKS that the current `GSocketClient` path
gets from `GProxyResolver`. That's an accepted, temporary gap — when it
matters, `rust/crates/hxnet/src/connect.rs` already names `tokio-socks` as the add,
and doing it once in `hxnet` covers the main connection, HTXF, and the
tracker together rather than per-call.

**Streaming records back to C:** reuse the event-bridge/poll pattern the
lifecycle + htxf paths already use (opaque handle, events drained on the
main loop via `g_idle`/poll, never touching GTK from the tokio task). The
drain re-emits the *existing* `tracker-batch-begin` /
`tracker-server-create` signals, so `tracker.c` needs zero changes and
the live count tickers keep updating. Keep per-record emit (not
batch-at-end) so v1's incremental feel survives.

**TLS / TOFU:** reuse the main connection's rustls verifier + the C trust
bridge (`tls_trust.c` / `tls_trust_dialog.c`) that the TLS phase already
built. Confirm the bridge is reentrant for a second endpoint hitting it
concurrently with (or just after) the session connection. Pins land in
the same `known_hosts` file as today.

**Verdict cache:** move it into the Rust module (process-scoped map keyed
by URL) so the C helper deletes cleanly; the module consults it to decide
whether to start an attempt at TLS or plain.

**Cancellation:** destroying the handle aborts the in-flight attempt —
same semantics as `hxnet_htxf_*` / the connection lifecycle.
`tracker_kill_threads()` becomes "destroy the handle."

## FFI surface (sketch)

Mirror `hxnet_htxf_*`. Exact shape TBD during T1:

- `hxnet_tracker_fetch_open(urls[], n_urls, &handle)` — start a fetch run;
  the module owns connect, TLS, probe/fallback, and the serial walk.
- A poll/drain call returning queued events
  (`BatchBegin{url, version, expected}`, `Record{...}`, `Done`,
  `Error{url, msg}`). All connect/TLS/fallback churn is internal — C only
  sees the resulting batch/record stream.
- `hxnet_tracker_fetch_close(handle)` — abort + free (this is what
  `tracker_kill_threads()` becomes).

## Phasing

- **T1 — Rust module + unit tests.** `hxnet::tracker`: per-connection
  engine over an `AsyncRead/Write`, calling the `hotline-proto` parsers.
  Unit-test handshake/probe/record-walk against recorded byte streams.
  No C wiring yet.
- **T2 — FFI + C bridge.** Add the FFI, re-point `hx_tracker_list_async`
  / `tracker_kill_threads` onto it (open the handle, drain events on the
  main loop, re-emit the existing signals). Signals stay byte-identical;
  `tracker.c` is untouched.
- **T3 — Delete the C state machine.** Remove the `network.c` tracker
  machine, `tls_endpoint` tracker plumbing, watchdog, and verdict-cache
  helpers.
- **T4 — Tier 3 green.** Validate against the existing targets.

## Testing

Coverage already exists and exercises the *public* entry points + signals,
so it regression-guards the migration with little change:

- `tests/integration/test_tracker_v1.c`, `test_tracker_v3.c`,
  `test_tracker_v3_tls.c`, and `tracker_matrix.{c,h}`.
- Containers: `tests/hxtrackd` (v1), Argus (v3), and the Janus/Mobius TLS
  port for `test_tracker_v3_tls`.
- Add Rust unit tests for the probe-watchdog and TLS-verdict transitions
  (the logic most likely to drift in the move).

Watch the probe-fallback timing: the Tier 3 v1 path leans on the watchdog
firing when a pre-spec tracker ignores the v3 byte. Keep
`GTKHX_TRACKER_V3_PROBE_MS` honoured (or its Rust equivalent) so the test
rig can shorten it.

## Risks / open questions

1. **TOFU bridge reentrancy.** The rustls→C trust prompt must tolerate a
   tracker endpoint and the session endpoint hitting it close together.
   Verify before T2.
2. **Streaming cadence.** Preserve per-record emit so the live "found /
   total" counters tick as they do now; don't collapse to a single
   end-of-fetch batch.
3. **SOCKS gap (accepted).** Moving connect into tokio drops the
   transparent `GProxyResolver` SOCKS support until `tokio-socks` lands in
   `hxnet`. Known and accepted, not a blocker; worth a line in the commit
   so it's not mistaken for a regression.

## Effort

Comparable to the HTXF H2 re-wire: a Rust module + FFI + C re-point +
deletion. Roughly 1–1.5 on the R-phase "weeks" scale, dominated by the
TLS/TOFU bridge reuse and Tier 3 validation. Letting Rust own the connect
(no fd-handoff, SOCKS deferred) keeps the module self-contained, which
trims the C-side surface versus the HTXF pattern.
