# Network endgame — the connection struct, `rcv.c`, and the bridge

Where the C network layer stands and what is left to remove. The transport
is done and documented in [`networking.md`](networking.md); this doc is
the *receive* side — the connection struct the handlers mutate, the
handlers themselves, and the C↔Rust bridge that feeds them. Two halves:
what Rust already owns (with the decision records worth not
re-litigating), and the plan for what remains.

# Part 1 — What Rust owns now

## The connection struct is opaque and Rust-owned

`struct htlc_conn` — the per-connection Hotline session state — is owned
by the Rust `gtkhx-core::conn` module. `protocol.h` forward-declares the
tag and nothing more; production C allocates one with `hx_conn_new` and
reaches every field through the `hx_conn_*` getter/setter ABI declared in
`src/hxconn.h`. A consumer that includes `hxconn.h` and not the test
layout header sees the accessors but *not* the fields. That is the
end-state contract.

### Decision record: opaque handle, not a `#[repr(C)]` mirror

The alternative was to keep a `#[repr(C)]` struct definition visible to C
— layout pinned by static assertions — so C could keep direct field
access indefinitely while Rust owned the definition.

**Opaque handle + accessor FFI won.** It gives the cleanest end state:
the layout is Rust's alone, and there is no duplicated struct definition
to keep in sync as fields come and go. The cost is shim boilerplate — one
getter and usually one setter per field — paid during the migration. That
boilerplate is mechanical and finite; a drifting mirror is neither. The
mirror was still useful as *scaffolding*, letting C keep direct access
while the accessor seam was built out group by group.

### The field groups, and which ones turned out to be dead

Grouping the struct before moving it is what made the move tractable, and
the useful finding was how much of it nothing read any more:

- **Live session identity and state** — `fd`, `uid`, `trans`, `icon`,
  `version`, `access`, `name`, `login`, `caps`, `nick_color`, the `flags`
  bitfield (`logged_in`, `post_login_fetched`), `serverhost`,
  `serverport`, `ip_addr`, `tls`.
- **Extension limits** — the inline-media advisory ceilings, the
  chat-history retention hints and message-id cursor, the GIF-icons probe
  state.
- **Crypto material — almost entirely dead.** Session key, MAC algorithm,
  the per-direction cipher state unions with their keys and types, the
  cipher mode, the compression header length: leftovers from when the HOPE
  handshake and the ciphers lived in C. The one live survivor is the
  opaque HOPE AEAD material handle, which HTXF subchannels read to derive
  per-transfer keys in-process. The cipher and compress *algorithm name*
  strings also survive — connect-time inputs the Connect dialog sets, not
  crypto state.
- **Legacy list and state-machine plumbing — dead.** The intrusive
  multi-connection list (single-conn now, never walked) and the retired
  two-phase receive state machine's function pointers.
- **I/O staging buffers — deleted outright**, see below.

Culling the dead groups first was a safe, mechanical precondition that
shrank the eventual move substantially and kept the scariest fields — raw
cipher unions — out of it entirely.

### Back-pointer surgery

The struct used to be an embedded *value* on `session`, and
`sess_from_htlc` recovered the session by `container_of` pointer
arithmetic — which only works while the struct is embedded. An opaque
Rust-owned struct can't be embedded by value, because C would need to
know its size. So `session.htlc` became a heap pointer, allocated once at
startup and owned for the session's life, and `sess_from_htlc` now reads
a `session *sess` back-pointer stamped at allocation. (Multi-connection-
ready, incidentally.)

Two wrinkles surfaced, both the kind of thing a grep for field accesses
doesn't find:

1. **Preference variables had bound to an address inside the embedded
   struct.** The static settings table pointed its ICON and NICK entries at
   `&the_session.htlc.icon` and friends — compile-time constants only while
   the struct is embedded, so the two slots had to be patched at runtime once
   the connection existed. That binder is gone with the settings table: the
   nickname and icon are ordinary preferences with their own storage, copied
   into the connection at load rather than aliasing it. See
   [../preferences.md](../preferences.md).
2. **The integration-test harness deliberately embedded its connection in
   a `session`** to keep `container_of` honest. It now owns explicit
   storage and re-arms the back-pointer after each per-test `memset`.

### The pinned layout mirror survives for tests

Production never sees the fields, but the Tier 2 / Tier 3 tests
legitimately need to construct and inspect a connection — they
stack-allocate one, `memset` it, copy it by value, and read fields
directly to drive the accessors and parsers. So a `#[repr(C)]` C mirror
is kept in `src/hxconn_layout.h`, included only by test code and pinned
byte-identical to Rust's `HtlcConn` by a static assertion on each side.
Retiring even that mirror is possible but high-churn and low-value, and
deliberately not planned.

## Delete, don't accessorize — the staging buffers are gone

**When a field can be removed rather than wrapped in an accessor, remove
it.** Wrapping a field that is slated for deletion produces throwaway
boilerplate at every call site and then deletes it again a phase later.
The three I/O staging buffers were the case that proved it; all three are
gone, none via accessors.

- **The read-accumulation buffer** was already dead — `hxnet` owns receive
  accumulation.
- **The send-staging buffer** was vestigial: each send drained exactly
  what it packed, and the orchestrator send is whole-frame with no
  partial-write retry to buffer against. The pack helpers now return a
  freshly allocated buffer plus length; the send path packs into it, hands
  it to the bridge, and frees it.
- **The receive-staging buffer** was the last holdout: **frames are now
  passed as a slice with a length, not staged on the struct.**
  `hx_bridge_dispatch_frame` assembles the header and body into a
  transient buffer and passes it to `hx_dispatch_frame` as an explicit
  `(frame, frame_len)` pair, freeing it once dispatch returns. Every
  consumer that used to read the buffer off the struct — body handlers,
  the correlator, the task-reply callbacks, the parser wrappers, the
  inline-media paths, the task-error extractor, the protocol trace — reads
  its threaded slice argument instead. This was kept deliberately separate
  from moving handler *bodies* into Rust: it did not require that, and
  conflating the two would have blocked the struct move behind a much
  larger project.

## What the handlers' collaborators run on

Every collaborator a receive handler touches is already behind a Rust
seam, which is what makes the rest tractable rather than a rewrite:

| Concern | Where it lives |
|---|---|
| Wire parse / build | `hotline-proto` |
| Transaction table | `hxtask` |
| Opcode → handler routing | `hotline-proto::dispatch::route` (`hx_recv_route`) |
| Receive handler bodies + signal emit | `hxhandlers::recv` (one module per domain) |
| View boundary (signals) | `GtkhxSession` (`gtkhx-core::session`) |
| Per-session collections | `HxChatRegistry`, `HxMemberModel`, `MediaTable` |
| Transport / crypto / framing / compression | `hxnet`, `hxcrypto` |
| TLS trust decisions | `hxtls-trust` |

`hxhandlers::recv` has a module per protocol domain (chat, user, msg,
news, files, xfer, icon, agreement); it absorbed what were previously
separate per-domain receive crates, so those crate names no longer exist.

# Part 2 — What remains, and in what order

Two C artifacts are left in the receive path. **`src/hxnet_bridge.c`** is
the C↔Rust seam: it owns the single live hxnet handle, wires the event /
shutdown / state callbacks, maps connection states onto `GtkhxSession`
signals, hosts the SOCKS proxy lookup and the TLS-verify trampoline, and
turns each `Event::Frame` into a `hx_dispatch_frame` call. **`src/rcv.c`**
holds the frame-dispatch switch, the transaction correlator, the
post-login sequencing, and the receive handlers that still have C bodies.

## The ordering dependency

**The bridge cannot go until the handlers do.** Its remaining receive job
is to reconstruct a frame the Rust actor already parsed so C handlers can
be called with a `(ptr, len)` slice. The moment no C handler needs that
slice, `Event::Frame` goes straight into Rust dispatch and the whole
reconstruct-then-redispatch hop disappears. Until then the hop is
load-bearing, so deleting the bridge first is not an option.

The bridge's *other* duties are relocations, not deletions: the connect
install, the proxy lookup, and the TLS-verify trampoline are lifecycle
glue that moves into hxnet's connect path; the state and shutdown
callbacks become direct emits from Rust, since `GtkhxSession` is already
Rust.

## What is actually left in `rcv.c`

Most former handlers are now `#[no_mangle]` functions in
`hxhandlers::recv` that C sees only as externs — the prototypes stay in
`src/rcv.h` because the C senders register them through `task_new`, and
the symbols resolve against the Rust crate at link. Derive the current
list from `src/rcv.c` and `src/rcv.h` when you need it; do not trust a
checked-in table. An earlier hand-maintained per-handler inventory rotted
for exactly this reason — if a table is wanted, regenerate it from the
headers.

As of this writing the C bodies group into:

- **The dispatch spine** — `hx_dispatch_frame` (a `switch` over
  `hx_recv_route` calling the selected body handler with the frame slice),
  `hx_rcv_task` (the transaction correlator), and `task_inerror`, a thin
  wrapper over the Rust header check.
- **Login and post-login sequencing** — `rcv_task_login`, which walks the
  LOGIN reply through the Rust parser and then does everything after:
  seeds the HOPE AEAD handle, applies the parsed fields, emits logged-in,
  starts the ping keepalive on 1.5+ servers, and routes the fetch
  decision. Plus `hx_post_login_fetches`, its fallback timer, and the
  reset hook.
- **Server-initiated handlers still in C** — private message / broadcast
  (the broadcast branch renders in C), agreement, banner, transfer queue,
  the unknown-opcode dump, and a one-line icon-change forwarder.
- **Task replies still in C** — the user-editor open, the message-send
  acknowledgement, the kick acknowledgement.
- **Voice** — the three server-initiated voice handlers and the two voice
  task replies, all compiled out when voice is disabled.

## State and behaviour that must be preserved verbatim

The highest-half-life content here. When these handlers move, none of the
following is a cleanup opportunity.

**Task-error suppression.** A server task error surfaces as a toast plus
the error sound — deliberately *not* a modal dialog, because the common
case is the server rejecting one of our auto-fired bootstrap requests, and
a dialog blocks the user before they can do anything. Speculative
bootstrap probes whose rejection is expected and non-actionable (the
GIF-icons capability probe: no capability bit, no version tie, so an error
just means "unsupported") are suppressed entirely — their own handler
records the verdict on the error path. Separately, an errored reply is
still dispatched to its handler for tasks that own per-transfer state —
single-file and folder transfers, inline-media upload and download —
because that handler is what frees it; skipping it strands an orphaned
transfer in the Tasks window forever. Non-transfer handlers have nothing
to free and are skipped. Voice error replies get their own inspection so
the voice state machine can choose between tearing the session down and
just toasting.

**Transaction ID zero is a real key, not a sentinel.** A short or
malformed header leaves the extracted trans at 0, and the table treats 0
as "no such task" — a safe fallthrough only because nothing else relies
on 0 being reserved. It is a legitimate key for the first frame on a
fresh connection. The table keys on the raw integer; keep it that way.

**Post-login ordering.** The fetch fan-out (user list, gated news,
GIF-icons probe, chat-history batch) is idempotent behind a single-fire
flag on the connection, and fires at the spec-correct "fully joined"
boundary — after `AGREEMENTAGREE` goes out, not after the login reply and
not after self-info. Sending RPCs before that boundary trips
"action attributed to not-yet-joined session" errors on 1.5+ servers and
outright disconnects on stricter ones. The routing:

- **1.0/1.2 servers** (no version chunk in the LOGIN reply) have no
  agreement flow at all, so there is no boundary to wait for: deliver
  name and icon via a user-change and fire the fetches immediately.
- **1.5+ servers** wait. `hx_send_agreement_agree` fires the fetches after
  the wire send, whether that came from the user clicking Agree or from
  the auto-agree path when the account has no agreement to show.
- **A short fallback timer** arms as a last resort for a misbehaving
  server that sends no agreement opcode at all.

Self-info still matters, but as a different signal: it sets the
`logged_in` flag that the agreement window's Agree button reads to decide
whether sending `AGREEMENTAGREE` is appropriate at all — some 1.9 servers
disconnect on one for an already-logged-in session. It is not a fetch
trigger.

## The structural knot, as it stands

The receive side used not to come apart one thread at a time, for four
reasons. Three are resolved:

- **The transaction table shared by send and receive** was the true
  centre. It is Rust now (`hxtask`), and still stores a C-ABI function
  pointer per entry — but that pointer points into either language
  indifferently, which is what let the handlers migrate one at a time.
- **The two-phase receive state machine** (a function pointer on the
  connection, aimed at a body handler by a header handler and reset
  afterwards) is gone; dispatch routes the parsed opcode straight to the
  body handler.
- **Outbound framing spread across many sender sites**: serialization is
  `hotline-proto`'s now, and both send entry points go through it.

The fourth — **handlers braiding wire parse, task correlation, and view
emission** — is what's left, and it is mostly unbraided by construction:
the parse half went to `hotline-proto` and the emit half to
`hxhandlers::recv` before the bodies moved. What remains braided is
concentrated in the login and post-login path.

## A latent bug the dispatch move caught

Moving the opcode→handler decision into a testable Rust router surfaced a
bug sitting in the header enum: two message types were swapped — the chat
and private-message opcodes had each other's values. Nothing routed on
them at the time, so nothing was broken; building the router on top of
them would have inverted chat and private messages with no test to catch
it. A concrete argument for building the testable core before the thing
that depends on it.

## Why headless coverage has to come from crate tests

The dispatch → signal → view path has **no headless integration
coverage**, and can't get any from the integration suite as built. The
suite's wire round-trips go through the harness's own receive helpers,
not the production dispatch. The production-connect tests do drive the
real connect path, but they stub the dispatch entry point — recording the
replayed frame rather than handling it — because linking the real
handlers would drag in the GTK widget tree, which a headless test binary
can't link. That stubbing is *why* those tests validate the
bridge→dispatch handoff and the LOGIN-reply capability echo but not the
handlers' signal emission.

So per-handler signal coverage comes from extracting each handler into a
crate, where its parse→emit logic is unit-testable in isolation. That is
not a workaround for an immature suite; it is the only place the coverage
can exist while the receive path terminates in widgets — and it means
each handler that moves gains coverage it could not otherwise have.

## Constraints carried over — do not re-decide

- **Wire compatibility with 1.2 / 1.5 / 1.9 is frozen.** Only the
  implementation moves. Every step gates on the production-connect and
  HTXF Tier 3 tests plus the proto suites staying green against the
  server matrix.
- **Single-connection scope holds.** Do not resurrect the multi-connection
  list while moving handlers; multi-conn is a later, independent UI phase.
- **C hand-declares the `extern` prototypes** for these crates, so
  signature drift is a link error. No cbindgen here.
