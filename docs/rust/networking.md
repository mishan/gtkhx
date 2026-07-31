# The Rust networking stack (`hxnet`)

Subject reference for GtkHx's network transport: the control-channel
connect lifecycle, proxy support, and the tracker fetch — all of which
live in the Rust `hxnet` crate. The C side that remains is glue. This is
also the design record for the decisions that are easy to re-break: the
LOGIN-reply replay, the three silent-failure axes around it, why the proxy
config comes from where it does, and what compression still doesn't do.
Companion docs: `network-endgame.md` (the C receive layer still being
retired), `ROADMAP.md` (sequencing).

## The connect lifecycle

`hxnet` owns the control channel end to end. From `hx_connect`'s call
into the bridge, everything below happens in Rust:

- **DNS + TCP connect** (`connect::resolve_and_connect`) — the single
  connect primitive every entry point builds on. It walks the resolved
  address list trying IPv4 entries before IPv6, matching the legacy
  `GSocketClient` preference rather than depending on resolver order.
  No OS socket fd ever crosses the FFI.
- **TLS-from-byte-zero** (`tls.rs`), on the separate-port model: connect
  TCP, hand the stream to `tokio_rustls` immediately, then speak
  ordinary Hotline over the encrypted stream. Trust is WebPKI-first with
  a TOFU fallback — see "TLS trust" below.
- **The magic exchange** (`magic.rs`).
- **LOGIN** (`login.rs` builds and sends the frame; `login_reply.rs`
  receives and parses it).
- **The optional HOPE encryption/compression negotiation** (`hope.rs`,
  `hope_keys.rs`, `hope_blowfish.rs`): step 1, the reply, step 2, key
  derivation, then the transport is wrapped in the negotiated cipher
  adapter (Blowfish-OFB-64 or ChaCha20-Poly1305). HOPE-over-TLS is
  rejected up front as redundant double-encryption.
- **Framing, crypto and compression** on the running connection
  (`connection.rs`, `frame.rs`, `cipher.rs`, `compress.rs`,
  `transform.rs`). The actor reads and writes plaintext Hotline frames;
  the cipher/compression layers are composed onto the inner transport at
  spawn time and are transparent above that point.

`lifecycle.rs` stitches these into three entry lifecycles —
`run_plaintext_lifecycle`, `run_plaintext_tls_lifecycle`, and
`run_hope_lifecycle` — each of which ends by handing the stream to the
actor. State transitions ship as `Event::State(...)` along the way:
Resolving → Connecting → Connected → (TlsHandshaking) → MagicExchange →
LoginSending → LoginReplyWait → HandshakeDone.

### What the C side still does

`src/network.c::hx_connect_via_orchestrator` is the only control-channel
connect path; there is no legacy path and no gate. Its job is:

- **The preamble** — cancel any in-flight connect, close an existing
  connection, clear chat, stamp `serverhost` / `serverport` / `tls` /
  `login` / `ip_addr` onto the connection struct (the HTXF subchannel
  workers read those back), reset the chat-history cursor, and emit
  `GTKHX_CONNECTION_CONNECTING`.
- **Pinning the LOGIN transaction id** and setting the `fd` sentinel —
  see "The three silent-failure axes".
- **Assembling the capability bitmask** advertised at LOGIN (large files,
  text encoding, chat history, inline media, and voice when compiled in).
- **Dispatching to the right transport mode.** HOPE-over-TLS is rejected
  here with a dialog before anything else runs.

`src/hxnet_bridge.c` is the callback seam: it owns the single live hxnet
handle, wires the event / shutdown / state callbacks, maps `HXNET_STATE_*`
onto `GtkhxConnectionState` signals, does the SOCKS proxy lookup, hosts
the TLS-verify trampoline, and turns each `Event::Frame` back into a
`hx_dispatch_frame` call for the C receive layer.

Everything downstream of the LOGIN reply — the post-login field
extraction and side effects — is still `rcv_task_login` in `src/rcv.c`.
See `network-endgame.md`.

### TLS trust

The rustls verifier (`tls::WebPkiOrTofu`) runs real WebPKI validation
against the system root store, records the verdict in a shared flag, and
then **completes the handshake regardless**. If WebPKI validated, the cert
is trusted silently, exactly like a browser hitting a CA-signed site. Only
when it did not does the lifecycle fall back to the trust-on-first-use
gate — a C callback that reaches `hxtls-trust`, which owns the
`known_hosts` database and the accept/prompt/reject decision; the prompt
itself is marshalled to the GLib main thread. Deferring the *decision*
rather than the *check* is why the verifier completes the handshake
instead of returning the WebPKI error. The cert is never trusted unless
WebPKI validated it or TOFU accepted it, and a reject closes the stream
before any credentials go out.

## The LOGIN reply — replay vs. payload

The orchestrator consumes the LOGIN reply itself
(`login_reply::recv_login_reply`). The C side needs what's in it: the
server's `HTLS_DATA_VERSION`, the banner id, the server name, the
capability echo, and the task-error bit. If the orchestrator swallowed
the reply silently, all of that would be lost. Three options were
weighed.

### Option A — payload on `HandshakeDone`

Extend `Event::State(HandshakeDone)` to carry the parsed `LoginReply`;
the FFI's `on_state` callback grows a reply-chunks payload.

**Pros**: explicit; the C side parses the chunks as before, no flow
change. **Cons**: payload-bearing state events are a new shape — the
`on_state` signature carries no payload today.

### Option B — replay the LOGIN reply as a Frame event

The orchestrator emits the LOGIN-reply bytes back to the C side as a
synthetic `Event::Frame` **before** emitting `HandshakeDone`. The C
side's existing bridge + dispatch table consume it through the normal
LOGIN-reply path (`rcv_task_login`).

**Pros**: no change to the FFI shape, no change to the receive dispatch;
the orchestrator is purely additive transport, so the receive side stays
byte-identical to the legacy path.

**Cons**: the orchestrator has to retain the full reply bytes and re-emit
them. The double-work is smaller than it looks — Rust parses only far
enough to read the task-error bit (success vs. failure); the rich fields
are parsed once, in C. The real cost is the trans-pinning and
install-ordering glue, both of which fail *silently*.

This is what shipped. `LoginReply::raw_frame` retains the verbatim wire
bytes, and each lifecycle re-emits them via `Frame::from_raw` ahead of
`HandshakeDone` — the LOGIN reply on the plaintext and TLS paths, the
step-2 reply on the HOPE path.

### Option C — keep magic + LOGIN on the C side

The orchestrator stops at "TCP connected"; C does magic + LOGIN itself.
Smallest delta, reuses everything — and defeats the point. The reason to
have hxnet own the lifecycle was so the C side's GIO/GPollable machinery
could be deleted, which it now has been.

### The intended convergence: B to bridge, A as the destination

Option B was the right *migration* mechanism: it kept everything
downstream of the LOGIN reply behaviourally identical while the transport
was swapped underneath. But it is not the clean end state.

Option A sidesteps both silent-failure axes — no trans matching, no
install ordering — because the reply never rides the receive dispatch
path; the handshake-done handler gets the parsed chunks directly. Its
one cost is factoring the field extraction out of `rcv_task_login` into a
callable parser, separate from the post-login orchestration. That
refactor is wanted anyway: the Rust side already parses the reply to
decide success, and the C side wants a clean "parse-reply-fields" entry
point decoupled from "arm the post-login machinery". At that point A's
payload is just handing already-parsed fields across instead of
re-serializing them into a synthetic frame for C to re-parse.

The convergence to A is a natural companion to moving `rcv_task_login`
itself into Rust (`network-endgame.md`); doing them together is less net
plumbing than doing either alone.

## The three silent-failure axes

These are the reasons Option B's glue is delicate. Each one lets login
"succeed" while every post-login side effect vanishes without an error.
All three are live in `hx_connect_via_orchestrator` today.

**1. Transaction-ID pinning.** The LOGIN reply dispatches by transaction
id: `hx_rcv_task` does `task_with_trans(trans)`, and a miss is a *silent*
fallthrough. So the synthetic frame only reaches `rcv_task_login` if a
task is registered under the exact trans the orchestrator's LOGIN
carries. The orchestrator owns the send, so both sides must agree on the
value up front — LOGIN is always the first transaction, so it is pinned
to the constant `HX_LOGIN_TRANS`. The plaintext and TLS paths replay the
LOGIN reply (trans `HX_LOGIN_TRANS`); the HOPE path replays the *step-2*
reply, which carries `HX_LOGIN_TRANS + 1`. `htlc->trans` is then bumped
past the replayed value, because the post-login follow-up sends fire from
*inside* `rcv_task_login` during the replayed-frame dispatch and stamp
themselves with the current counter — left unbumped they collide with the
login task. The legacy path got that bump for free from its own LOGIN
send; the orchestrator's send never touches the C counter.

**2. The `fd` sentinel is -1, not 0.** `hx_bridge_dispatch_frame`
early-returns on `fd == 0` — that is the bridge's "connection closed,
drop the frame" signal. The orchestrator owns the socket, so the C side
has no real fd; `-1` means "live but no C-visible fd". `0` would silently
drop every replayed frame. `-1` also keeps the `if (fd) close(...)`
close-time guards firing, and the sentinel is never passed to `close(2)`
— teardown goes through the hxnet handle, not the fd.

**3. Synchronous install ordering.** `hx_bridge_dispatch_frame` also
gates on `hx_bridge_is_installed()`, so the bridge handle must be in the
global slot before the first event callback fires. Because the
orchestrator emits the replayed frame *before* `HandshakeDone`, and
because events arrive on the GLib main loop (which is not re-entered
until the connect function returns), installing the handle synchronously
inside the open call closes the window. An "install on handshake-done"
design would drop the LOGIN reply.

A test that only checks "login succeeded" passes even when the replayed
frame was dropped. The Tier 3 gate therefore asserts the *effects* — the
recorded dispatch count is non-zero and the reply carries the expected
opcode and error bit — which catches axes 1 and 3 at once. Axis 2 is the
one *loud* failure mode: a straggler calling a socket API on the sentinel
gets `EBADF`.

## Connect-task timing parity

The orchestrator originally registered the "login" protocol task up front
(it had to exist before the replayed reply could dispatch to it). That
left the login task visible in the Tasks window *concurrently* with the
coarse "Connecting" task for the whole connect — unlike the legacy path,
where it appears only once the connection is up and credentials are going
out. Root cause: the two paths reached "handshake done" at different
moments. Legacy emitted it when magic was done and login was being sent,
then registered the task; the orchestrator emitted it at the very end,
after the login reply. So legacy meant "entering login phase" and the
orchestrator meant "login complete."

The fix maps the orchestrator's `LoginSending` state onto the coarse
`HANDSHAKE_DONE` view transition and registers the login task there, from
the bridge's state callback (`hx_orchestrator_register_login_task`, which
is idempotent and restores the send counter afterwards). `LoginSending`
is emitted strictly before the replayed reply frame on the same ordered
channel, so the task is registered in time. Rust's end-of-handshake state
no longer drives a view transition; login completion is signalled by
`LOGIN_READY`, as in legacy. Net sequence: CONNECTING → TCP_CONNECTED →
HANDSHAKE_DONE (login task appears) → reply → LOGIN_READY.

## Open: compression is never negotiated

The orchestrator advertises an **empty compression-algorithm list** in
HOPE step 1, so no server ever picks one, and the transport is composed
with `CompressionKind::None`. No connection compresses on the current
path.

`hope::select_algorithms` leaves `compress_alg` at `None` deliberately,
and the reasoning is in the code comment there: echoing a `COMPRESS_ALG`
in step 2 that the transport wouldn't actually apply would commit us to
compressing while sending plaintext, desyncing the server. Wiring
compression means all three together — advertise algorithms in step 1,
populate the choice from the parsed reply, and pass the matching
`CompressionKind` to `compose()` in the lifecycle. Doing one or two of
those is worse than doing none.

This is genuinely open work, not a shrug: the compression adapters
(`compress.rs` in `hxcrypto`) exist and are tested; only the negotiation
is unwired.

## Proxy support

All three network paths — control channel, HTXF subchannel, and tracker —
connect through `connect::resolve_and_connect`, which tunnels through a
SOCKS proxy when one is supplied. `tokio-socks` sits behind a non-default
`socks` Cargo feature; the `ProxyConfig` type and its URI parsing are
always compiled. A build without the feature that is nonetheless handed a
proxy fails loudly rather than silently connecting direct.

**Configuration comes from `GProxyResolver` only.** `hx_bridge_lookup_socks_proxy`
queries the resolver at connect time and hands the chosen URI across the
FFI (NULL = direct). This was chosen over parsing environment variables
in Rust because it preserves the full desktop integration — GNOME
settings, per-host rules, PAC — and because `GProxyResolver`'s default
backend already reads `all_proxy` / `ALL_PROXY` anyway, so an env-var
fallback in C would add a second config source for no gain. **An in-app
proxy preference was deliberately not added.**

**The `none://host:port` query trick.** `g_proxy_resolver_lookup` wants a
URI, but a raw Hotline TCP connect has no scheme. `none://` is what
`GSocketClient` itself uses internally for a plain TCP connect, so the
lookup returns the same answer GLib would have picked. The host is
percent-escaped before interpolation (an IPv6 zone id's `%` would
otherwise make the URI malformed) with `:` left unescaped, and the
host:port join brackets IPv6 literals.

**Remote DNS.** In the proxied branch the target is passed to the proxy
as a domain tuple so the *proxy* resolves it — socks5h / socks4a
semantics — so proxy-only hostnames work and DNS doesn't leak. Because we
always do remote DNS, `socks5`/`socks5h` collapse to one scheme and
`socks4`/`socks4a` to the other. The direct branch keeps its local
lookup with the v4-first fallback and is byte-identical to a build
without proxy support.

**The HTTP-CONNECT gap is deliberate, not a TODO.** `tokio-socks` speaks
SOCKS4/5 only. An explicit `http(s)://` URI handed to
`ProxyConfig::from_uri` is rejected loudly; an `http://` *result* from
`GProxyResolver` is warned about (with any `user:pass@` userinfo redacted
so proxy credentials don't reach the log) and skipped. We can't tunnel a
raw Hotline stream through HTTP CONNECT without writing a CONNECT shim,
and no network we've hit needs one.

**TLS over the tunnel** uses the **real target** for SNI and for the TOFU
fingerprint — not the proxy's. rustls simply wraps the tunnelled stream;
the hostname it verifies against is the server the user asked for.

**The negative control is what proves it works.** Tier 3 runs a microsocks
container (`tests/socks-proxy/`) and
`tests/integration/test_integration_socks.c` drives the production connect
through it to mhxd. The important case is the *dead-proxy* one: point the
connect at a proxy address that refuses, and require it to fail. mhxd is
directly reachable in the matrix, so a bug that ignored the configured
proxy and connected direct would wrongly *succeed* there. That is the
assertion that catches a silent bypass. A blocked-egress network namespace
would be stricter, but the negative control covers the main risk with far
less rig.

### Deferred

- **Asynchronous resolver lookup.** `g_proxy_resolver_lookup` is called
  synchronously on the GLib main thread. The default backends match rules
  in memory and don't block, but a PAC/WPAD backend could stall the UI
  during connect. The fix — the async lookup with the open call deferred
  into its completion callback — restructures the "handle installed
  before return" contract that axis 3 above depends on, so it was
  deliberately left alone.
- **An in-app proxy preference.** A "SOCKS proxy host:port" row in
  Settings would let users configure one without touching system or
  environment config. UX-only.
- **HTTP CONNECT.** Add a CONNECT shim only if an HTTP-only-proxy network
  turns up.

## Tracker fetch

`hxnet::tracker` (the per-connection protocol engine) and
`hxnet::tracker_fetch` (the serial walk over configured tracker URLs) own
the tracker behind the `hxnet_tracker_fetch_*` FFI. The parsers were
already Rust in `hotline-proto`; this move was about the transport
orchestration.

**It was never on the critical path.** The fetch was already off the
worker threads — it ran on the GLib main loop via chained `GSocketClient`
async callbacks — so moving it unblocked nothing.

**The real payoff was collapsing a second TLS stack.** Tracker TLS went
through `g_socket_client_set_tls` (glib-networking / GnuTLS) plus its own
TOFU handler: a *different* TLS implementation, reaching the same trust
decision, writing into the same `known_hosts` file as the main session.
Two independent implementations of one security decision is the kind of
duplication that drifts silently. The migration unified both on rustls
plus the one trust database, and deleted a large body of bespoke C async
along with it.

**Rust owns the connect**, deliberately — not the "C connects and hands
over a file descriptor" split used for HTXF. Because the module owns
connect, both fallback ladders stay entirely inside it with no fd
round-trip:

- **TLS→plain**: try rustls first; on a TLS *handshake* failure record a
  per-tracker `No` verdict and reopen plaintext. A *transport* failure
  (DNS, refused, timeout) does not fall back — a plain retry to an
  unreachable host just doubles the wait. A cert rejected by the TOFU
  check is a *hard* failure: no fallback, no verdict recorded. Silently
  downgrading after the trust store or the user rejected a cert would be
  a security downgrade.
- **v3→v1**: send the v3 handshake and watchdog the response; on an
  inconclusive probe (silence, short read, junk) drop the connection and
  reopen with the v1 magic. Real pre-spec v1 trackers silently ignore the
  v3 version byte, so the timeout *is* the signal.

**The per-tracker TLS verdict cache** is process-scoped, keyed by URL,
and lives in Rust. A walk snapshots it, updates its copy, and stores it
back, so a Refresh doesn't re-pay a handshake known to fail — same
behaviour as the C cache it replaced.

**Event emission.** The C bridge in `network.c` drains fetch events on a
main-loop timeout and re-emits the existing `tracker-batch-begin` /
`tracker-server-create` `GtkhxSession` signals, so the view is unchanged
and per-record progress ticks still fire. One cadence difference is worth
knowing: the Rust engine reads a whole listing before returning it, so a
tracker's records arrive as a burst and progress ticks per tracker rather
than per record *within* a tracker. Acceptable for the listing sizes real
trackers serve; cross-tracker progress is unchanged. The drain re-checks
the handle at the top of every iteration, because a signal subscriber can
re-enter and cancel the fetch mid-drain.

**Probe-watchdog timing is a test dependency.** The Tier 3 v1 path leans
on the watchdog firing when a pre-spec tracker ignores the v3 byte, so
`GTKHX_TRACKER_V3_PROBE_MS` stays honoured (clamped to a sane range) to
let a slow rig lengthen it.

The tracker UI is Rust now (`gtkhx-ui`); it consumes the two signals and
the `HxTrackerServer` boxed event and never touches a socket.

## Tier 3 coverage of the production connect path

A capabilities-negotiation regression once shipped on the orchestrator
path — the LOGIN omitted `HTLC_DATA_CAPABILITIES`, so chat-history,
inline-media and voice never negotiated — and **no integration test
caught it**, even though the suite has chat-history coverage. The reason
is durable and worth keeping.

### Why: there were two client wire implementations in the tree

1. **The integration harness** hand-rolled its own magic + LOGIN + receive
   loop over a raw blocking socket, building its own chunk list. It never
   linked the production connect path. So "chat-history works" proved the
   *server* supported the extension against the *harness's* login — it
   never touched the production LOGIN builder.
2. **The production-connect tests** did drive real connect code, but
   stubbed the receive layer and the UI.

The capability bitmask lives in the production LOGIN builder. No test
drove that builder *and* inspected the negotiated result, so the
regression was invisible.

The fix was structural: the harness's login entry points now open through
the same production lifecycle the GUI uses (via polling-mode siblings of
the open FFIs) and return a synthetic fd that the send / recv / close
helpers route through the actor, rebuilding the wire header so every
downstream chunk walker sees byte-identical input. No per-test changes
were needed. A residual raw-socket surface remains for a handful of
low-level helpers (the handshake and plain-login tests, and the C HOPE
step senders); retiring it is what unblocks deleting the last C crypto
modules.

Alongside that, `/real_connect/capabilities_negotiated` drives the
production orchestrator against a capability-aware server and asserts the
server echoed `HTLC_DATA_CAPABILITIES` back, and a `login.rs` unit test
guards the send side.

### The remaining coverage hole

There is **no automated legacy-server regression target**. The container
matrix has no 1.0/1.2 server — mhxd speaks 1.x with HOPE, Janus is 1.9 —
so 1.0/1.2 behaviour is covered by manual smoke against old Mac servers
plus the pre-TASK-frame tolerance in `login_reply.rs`. A 1.0/1.2 mock
target would be a real regression guard; it is a nice-to-have, and worth
less than a real server.

Post-login protocol handling (chat/news/file round-trips through the
production receive path) also can't run headless while receive handlers
reach the widget tree — see `network-endgame.md`.
