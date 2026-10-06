# The Rust networking stack (`hxnet`)

Subject reference for GtkHx's network transport: the control-channel
connect lifecycle, proxy support, and the tracker fetch — all of which
live in the Rust `hxnet` crate. The C side that remains is glue. This is
also the design record for the decisions that are easy to re-break: how
the LOGIN reply reaches GtkHx, the silent-failure axes around it, why the proxy
config comes from where it does, and how compression is negotiated.
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
- **The session** (`session.rs`): from the magic on, hx-libs'
  `hxsession` drives the connection — the magic, the login and its reply,
  the agreement and the two-second wait for one, a 1.2 server's user
  change — and the actor in `session.rs` is its I/O. It runs the session
  in raw mode, because GtkHx still has receive handlers of its own, with
  the session handling chat, users, messages, news and the transfer queue
  itself (`Handled::CHAT`, `Handled::USERS`, `Handled::MSG`,
  `Handled::NEWS`, `Handled::FILES`): what it makes of a chat line, an
  invitation, a subject, a history reply, a user arriving, changing or
  leaving, what the server says about us, a private message, a broadcast,
  the server's parting words, a flat news post, or a queued transfer
  moving up reaches the main thread
  as `Event::Session`, on the channel the frames take, so the two arrive
  in the order the server sent them. Every other transaction reaches C
  whole, and what C sends goes out as C built it. See "The session",
  below.
- **HOPE**, the secure login, is the session's too
  (`Session::with_hope`, over hx-libs' `hxhope`): step 1, its reply, step
  2, and from step 2's reply on, the cipher (Blowfish OFB-64 or
  ChaCha20-Poly1305) and the compression it agreed, as a codec between the
  session and the socket. What the actor moves is the socket's bytes,
  whatever they are. HOPE-over-TLS is rejected up front as redundant
  double-encryption.

`lifecycle.rs` stitches these into three entry lifecycles —
`run_plaintext_lifecycle`, `run_plaintext_tls_lifecycle`, and
`run_hope_lifecycle` — each of which connects and hands the stream to the
session; the plaintext and HOPE ones differ only in the session they are
given. State transitions ship as `Event::State(...)` along the way:
Resolving → Connecting → Connected → (TlsHandshaking) → MagicExchange →
LoginSending → LoginReplyWait → HandshakeDone → LoginReady.

### The session

The session numbers every transaction on the connection, C's included,
from one counter. Its own are the login (HOPE's two steps, on 1 and 2),
the agreement, a 1.2 server's user change and the keep-alive. A C
request takes its trans from the session when its task is keyed
(`task_new`, through `hxnet_connection_take_trans`), and the send that
follows goes out on it; the connection holds that one trans reserved in
between (`htlc->trans`, 0 when none). The connection handle and the actor
share the session (`SharedSession`), which is made when the connection
opens so a request can be numbered before the login is answered.

`LoginReady` is the session saying the login is settled: the agreement
answered, or none to answer, or none come after two seconds. It is what
fires `hx_post_login_fetches` — before it, a 1.5+ server takes a user
list or a news fetch as coming from a user who has not joined. An
agreement with text reaches C as its frame and is shown, and nothing
follows the login until it is answered: the Agree button sends
`Command::Agree` with the user's name and icon as they are then, and
Disagree, or closing the window, disconnects. One with nothing to show
the session answers itself; so does one that says there is none from a
server that gave no version, which is a 1.5 server keeping that to itself.

Every reply reaches C, the session's own included — a refused agree or
login is dispatched and reported as any refused request is — but those C
said to expect (`Session::expect`, through `connection_expect`): a
chat-history request, a chat invitation, the user list, a user's info,
a kick, creating and joining a private chat, a private message, a
broadcast, an account read, made, saved or deleted, every news request —
flat news's file and posts, and threaded news's listings, articles, posts,
deletions and new bundles and categories — every files request: a
listing, Get Info, a folder made, something deleted, moved or renamed, a
comment set, and a download or upload of a file or a folder — the
banner's, the GIF icons' (the login's probe of everyone's, a user's, and
ours set), and each part of a picture going up or coming down. Their
replies are the session's to read, and come back as its events, a
refusal as `Failed`, which is shown and heard as any refused request is,
but for the icon probe's and the saved avatar's, which the user never
asked for. A picture's refusal is `MediaFailed`, with the extension's
code, and goes to whatever started the upload or download.
None of them is a task, so none shows in the Tasks list; a transfer
shows there as its own row. A joined private chat is made when its reply
arrives, a user's info reaches the user it was asked of, an account the
editor that asked, a news reply the browser node that asked, a listing
the files pane that asked, a transfer's reply the transfer, the banner's
its fetch, and a picture's part the upload or download it belongs to,
each matched by its trans; the rename of a move-and-rename goes once the
move's reply says it went through. What arrives is traced from the
session's tap (`Session::set_tap`, on under `GTKHX_DEBUG=proto`): each
transaction as it came, before the session acts on it and in plaintext
under HOPE, through `proto_trace.c` on the main thread, whatever handles
it. The actor traces what the session sends itself.

Until the login is answered the actor leaves C's commands in the channel,
so nothing goes out ahead of the login.

The keep-alive is the session's: a 1.5+ server (version 150 and up, the
bar mhxd's PING handler sets) is sent an empty `HTLC_HDR_PING` once the
login has settled and 60 seconds have passed with nothing sent, C's
requests included; a 1.0/1.2 server, which refuses the opcode, never is.
Its reply is the session's and does not reach C, so a server between 1.5
and 1.8.5 that refuses the opcode no longer costs the user an error toast
and sound every minute. A ping only when
nothing else has gone out is all an idle timer needs, and waiting for the
login to settle keeps it from reaching a 1.5+ server ahead of the
agreement.

### What the C side still does

`src/network.c::hx_connect_via_orchestrator` is the only control-channel
connect path; there is no legacy path and no gate. Its job is:

- **The preamble** — cancel any in-flight connect, close an existing
  connection, clear chat, stamp `serverhost` / `serverport` / `tls` /
  `login` / `ip_addr` onto the connection struct (the HTXF subchannel
  workers read those back), reset the chat-history cursor, and emit
  `GTKHX_CONNECTION_CONNECTING`.
- **Pinning the LOGIN transaction id** and setting the `fd` sentinel —
  see "The two silent-failure axes".
- **Assembling the capability bitmask** advertised at LOGIN (large files,
  text encoding, chat history, inline media, and voice when compiled in).
- **Dispatching to the right transport mode.** HOPE-over-TLS is rejected
  here with a dialog before anything else runs.

`src/hxnet_bridge.c` is the callback seam: it owns the single live hxnet
handle, wires the event / shutdown / state callbacks, maps `HXNET_STATE_*`
onto `GtkhxConnectionState` signals, does the SOCKS proxy lookup, hosts
the TLS-verify trampoline, and turns each `Event::Frame` back into a
`hx_dispatch_frame` call for the C receive layer, and each
`Event::Session` into an `hx_recv_session_event` call (hxhandlers) behind
the same gates.

The LOGIN reply is the session's too; see below.

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

## The LOGIN reply

The session reads the LOGIN reply itself (HOPE's step-2 reply under HOPE)
and says what it said as `Event::LoggedIn` (`hxsession::ServerInfo`): the
version, 0 for a 1.0/1.2 server, which sends none; the name; our uid; the
capabilities agreed, of those offered; inline media's advisory limits;
chat history's retention; and video's ceiling for each kind. GtkHx's
session handles the login (`Handled::LOGIN`), so the reply never reaches
C whole. The actor hands the event on as `Event::Session` ahead of
`HandshakeDone`, and `hxhandlers::recv::login` puts it on the connection,
with the HOPE transfer keys an HTXF subchannel derives its own from, then
emits `logged-in` (the connection, and the server's name or NULL). Its
view handler prints the "login successful" and capability lines, keeps
the name on the session and titles the windows; the login chime rides
the same signal.

A refused login is `Closed::LoginRefused`. The actor hands it on as
`Event::Session` before it ends, and the server's reason goes to
`request-failed`, a toast and the error sound; the shutdown that follows
closes the connection as any other. mhxd says nothing when it refuses a
login: it hangs up.

Order is the server's. What a server sends before answering the login
waits for the reply and follows `LoggedIn`; the agreement, when there is
one, follows that; `LoginReady` comes last, once the agreement is
answered or none came in two seconds, or at once on a 1.0/1.2 server.

A session that handles no domain (the polling FFIs the C integration
harness uses) also hands the reply over whole, ahead of `LoggedIn`.

## The two silent-failure axes

Each of these lets a login "succeed" while every post-login side effect
vanishes without an error. Both are live in `hx_connect_via_orchestrator`
today, and both gate what the session says as well as frames
(`bridge_on_session_cb`).

**1. The `fd` sentinel is -1, not 0.** `hx_bridge_dispatch_frame`
early-returns on `fd == 0` — that is the bridge's "connection closed,
drop the frame" signal. The orchestrator owns the socket, so the C side
has no real fd; `-1` means "live but no C-visible fd". `0` would silently
drop every event. `-1` also keeps the `if (fd) close(...)` close-time
guards firing, and the sentinel is never passed to `close(2)` — teardown
goes through the hxnet handle, not the fd.

**2. Synchronous install ordering.** The callbacks also gate on the
event's handle being the one stored on the connection, so the handle
must be stored before the first event callback fires. Because the actor
hands on `LoggedIn` *before* `HandshakeDone`, and because events arrive
on the GLib main loop (which is not re-entered until the connect function
returns), installing the handle synchronously inside the open call
closes the window. An "install on handshake-done" design would drop the
login.

A test that only checks "login succeeded" passes even when the login's
event was dropped. The Tier 3 gate (`test_real_connect`) therefore
asserts that a session event reached the C side before the first frame,
and that frames followed.

## Connect-state timing

The orchestrator's `LoginSending` state maps onto the coarse
`HANDSHAKE_DONE` view transition, as the legacy connect path's
send_login did: magic done, credentials going out. It deletes the coarse
"Connecting" task. Rust's own `HandshakeDone` drives no view transition;
the login's completion is `logged-in`, and the login settling is
`LOGIN_READY`. Net sequence: CONNECTING → TCP_CONNECTED → HANDSHAKE_DONE
→ `logged-in` → LOGIN_READY.

## Compression

A HOPE login offers the one compression the user picked in the connect
dialog or the bookmark — GZIP, LZ4 or ZSTD — and none when the row says
NONE, which is the default. The server takes it or not; a server that
names none (an empty list, an empty name, or "NONE") gets none. A bookmark
saved by the original 2000-era client stores its compression as a byte
whose 1 meant GZIP and now reads as ZSTD; against an mhxd-family server,
which has only GZIP, that asks for something it lacks and the login runs
uncompressed rather than failing.

It is opt-in because under a cipher, compression leaks: how long a
compressed record is depends on what it says, and an observer who can
also put text of their own into the stream — a chat line, a file name —
can learn from the lengths whether it matched something secret
(CRIME-style). Without compression a record's length says only how long
the plaintext was.

mhxd takes GZIP, with or without a cipher. Janus takes any of the three
under ChaCha20-Poly1305, and GZIP or ZSTD under Blowfish; LZ4 under
Blowfish it accepts and then reads nothing the client sends until the
connection closes ([janus-bugs.md](../janus-bugs.md)), so GtkHx never
offers LZ4 with Blowfish and that login runs uncompressed. The suite
covers each working pair against a server that has it: `hope_compression`
asks mhxd for GZIP under Blowfish, and the rig's Janus, whose compression
is on, for ZSTD and LZ4 under ChaCha20-Poly1305 and for GZIP and ZSTD
under Blowfish. Every other test offers none and runs uncompressed.

The compression sits beneath the cipher, sending and receiving, and one
`take_outgoing`'s worth of transactions is one unit: a zlib full flush,
an LZ4 frame, a Zstandard frame. Under a compression Blowfish carries no
rekey marker either way, as mhxd sends none: the cipher sees compressed
bytes, with no transactions in them to mark. The codecs are hx-libs'
`hxhope`.

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
already Rust in `hxproto`; this move was about the transport
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

**Event emission.** The C bridge in `network.c` drains fetch events when
the fetch wakes it — once a tracker's events are all in the channel, and
when the walk ends (`hxnet_tracker_fetch_watch`) — and re-emits the existing `tracker-batch-begin` /
`tracker-server-create` `GtkhxSession` signals, so the view is unchanged
and per-record progress ticks still fire. One cadence difference is worth
knowing: the Rust engine reads a whole listing before returning it, so a
tracker's records arrive as a burst and progress ticks per tracker rather
than per record *within* a tracker. Acceptable for the listing sizes real
trackers serve; cross-tracker progress is unchanged. The drain re-checks
the handle before every poll, because a signal subscriber can
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
session agreed chat history with it (`hxnet_connection_agreed_caps`), and a `login.rs` unit test
guards the send side.

### The remaining coverage hole

There is **no automated legacy-server regression target**. The container
matrix has no 1.0/1.2 server — mhxd speaks 1.x with HOPE, Janus is 1.9 —
so 1.0/1.2 behaviour is covered by manual smoke against old Mac servers
plus the session's tolerance of transactions ahead of the login reply. A
1.0/1.2 mock target would be a real regression guard; it is a
nice-to-have, and worth less than a real server.

Post-login protocol handling (chat/news/file round-trips through the
production receive path) also can't run headless while receive handlers
reach the widget tree — see `network-endgame.md`.
