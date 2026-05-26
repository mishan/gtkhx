# Tracker Protocol v3 — Scoping Notes for GtkHx

Status: research only, no code yet. Written 2026-05-23 against
<https://github.com/fogWraith/Hotline/blob/main/Docs/Protocol/Tracker/Tracker-Protocol-v3.md>
revision in `main` at fetch time.

GtkHx is a *client*. The spec covers three roles — tracker, registering
server, listing client — and only the last is relevant here. Sections of
the spec dealing with server registration, HMAC-signed registrations,
nonces, registration tokens, replay protection, tracker-side rate
limiting, federation, and the HTFD tracker-to-tracker protocol are out
of scope. They're worth re-reading if/when we decide to build a Hotline
server, but they don't change what we ship in GtkHx.

What stays in scope:

- The 8-byte handshake (was 6 in v1).
- The listing request/response framing (request didn't exist in v1).
- The new server-record format (address-type byte, 2-byte string
  lengths, TLV metadata trailer).
- The TLV catalogue, restricted to fields a client cares about.
- Optional features: `SEARCH_TEXT` / pagination, `FEAT_CLIENT_AUTH`,
  TLS on TCP, IPv6 server records.
- Backward compatibility with v1 trackers (and v2, which we've never
  spoken — same v1-shaped wire format with an auth challenge bolted on).

---

## 1. Current state of GtkHx's tracker code

Three files do all the work, plus light glue in `gtkhx.c` and
`gtkhx_session.{c,h}` for the signal that ferries each server record
back to the UI:

`src/hotline.h` (lines 66–88) holds the wire constants:

```c
#define HTRK_MAGIC "HTRK\0\1"   /* 6 bytes — v1 only */
#define HTRK_MAGIC_LEN 6
#define HTRK_TCPPORT 5498
#define HTRK_UDPPORT 5499       /* unused — we never register */
```

`src/network.c` (lines ~1380–1830) is a 450-line callback chain driven
by `GSocketClient` async on the main loop. Two contexts:
`tracker_run_ctx` holds the list of trackers and the cancellation
handle, `tracker_fetch_ctx` holds the per-connection state and parse
scratch. The state machine:

1. `hx_tracker_list_async` — top-level entry. Cancels any in-flight
   run, dups `gtkhx_prefs.tracker[]` into the run, kicks off the first.
2. `tracker_fetch_start` → `g_socket_client_connect_to_host_async`.
3. `on_tracker_connected` → write 6-byte `HTRK_MAGIC`.
4. `on_tracker_magic_sent` → read the 14-byte v1 response header.
5. `on_tracker_header_read` → pull `nservers` from bytes 10–11
   (`htons` on the wire), then loop one server at a time.
6. `read_next_server_hdr` / `on_server_hdr_read` / `on_server_rest_read`
   → 8 bytes of fixed-width record (IPv4 + port + nusers + reserved +
   name_len), then `on_server_name_read` / `on_server_desc_len_read` /
   `on_server_desc_read` peel off the two 1-byte-prefixed Pascal
   strings.
7. `tracker_emit_server` → `gtkhx_session_emit_tracker_server_create`
   (the only public hand-off into UI land), bump counter, loop.
8. `tracker_fetch_done` → free the per-fetch ctx, advance to the next
   tracker, or free the run.

There's a 16-byte scratch buf and 256-byte name/desc buffers in the
per-fetch ctx. The MacRoman→UTF-8 transcode happens later in
`tracker_server_create` (`tracker.c` line 349) via
`g_convert_with_fallback`.

`src/tracker.c` (584 lines) is the UI side: a `GtkWindow` with an
`AdwHeaderBar`, a `GtkSearchEntry` + Aa case-toggle, a five-column
`GtkHList` of (Name, Users, Address, Port, Description), and a binary
search tree keyed on `(addr, port)` for dedup. `tracker_server_create`
is the only public entry point the network side calls.

`src/gtkhx.c::on_tracker_server_create_signal` (line 1400) is the
session-signal handler — a one-line `tracker_server_create` adapter.

`src/gtkhx_session.{c,h}` declares the `tracker-server-create` signal
(`addr, port, nusers, nam, desc, total`).

`gtkhx_prefs.tracker[]` is a `char**` of n trackers (parsed from the
comma-separated `CFG_TRACKER` string in `options.c::1900-1923`); we
iterate them serially.

No protocol-trace coverage for the tracker path today — `proto_trace.c`
only knows about HTLC/HTLS opcodes inside a Hotline session.

---

## 2. Wire-level diffs from v1

| Aspect                        | v1                                                   | v3                                                                                                |
|-------------------------------|------------------------------------------------------|---------------------------------------------------------------------------------------------------|
| Handshake length              | 6 bytes (`HTRK\0\1`)                                 | 8 bytes (`HTRK` + `version u16=0x0003` + `feature_flags u16`)                                     |
| Handshake direction           | Client → server only; server immediately sends list  | Bidirectional; both sides send 8 bytes, then client must send a listing request                   |
| Read strategy                 | Read 14-byte response header                         | Read 6 bytes; if version is `0x0003`, read 2 more for feature flags                               |
| Listing request               | None — server sends as soon as handshake is done     | `request_type u16 = 0x0001` + `field_count u16` + N TLV query fields                              |
| Response header               | 14 bytes (`server_count` at offset 10)               | 10 bytes: `response_type u16=0x0001`, `total_size u32`, `total_servers u16`, `record_count u16`   |
| Server record fixed header    | 8 bytes: ipv4(4) + port(2) + nusers(2) + name_len(1) | Variable: `addr_type u8` + address + `port u16` + `nusers u16` + `name_len u16` + `desc_len u16`  |
| Address types                 | IPv4 only                                            | `0x04` IPv4 (4B), `0x06` IPv6 (16B), `0x48` hostname (2B length + UTF-8 bytes)                    |
| String length prefix          | 1 byte (Pascal string)                               | 2 bytes (`u16` big-endian)                                                                        |
| Per-record TLV trailer        | None                                                 | `tlv_count u16` + N {ID:u16, Len:u16, Value:Len bytes}                                            |
| Padding slot convention       | First-byte-zero entries are padding (skip)           | Doesn't exist — `addr_type` is the discriminator                                                  |
| String encoding               | MacRoman in practice                                 | UTF-8 mandatory                                                                                   |
| Total response size cap       | `u16` server count                                   | `u32` total size + `u16` total servers (still 65535 servers max)                                  |
| Multi-message response        | No                                                   | No — single response, but pagination lets the client re-request the next page                     |
| Transport                     | Plain TCP/5498                                       | TLS on TCP/5498 SHOULD; plain TCP MAY be accepted for v1/v2 backcompat                            |

The TLV catalogue (full table in spec §Quick-Reference) gives clients
new things to surface:

- **Address & connectivity:** `ADDRESS_IPV6` (0x0100), `HOSTNAME`
  (0x0101) — though for client purposes these are usually folded into
  the record's `addr_type` byte already.
- **Descriptive:** `SERVER_SOFTWARE` (0x0200), `COUNTRY_CODE` (0x0201),
  `REGION` (0x0202), `LANGUAGE` (0x0203), `MAX_USERS` (0x0204),
  `MATURITY` (0x0205, 0=general / 1=teen / 2=mature / 3=adult),
  `UPTIME` (0x0206, seconds), `RULES_URL` (0x0207), `BANNER_URL`
  (0x0208), `ICON_URL` (0x0209), `LINK_{DOWN,UP}_MBIT` (0x020A–B),
  `TIMEZONE_OFFSET_MIN` (0x020C, signed minutes from UTC),
  `CONTACT_URL` (0x020D), `SERVER_LAUNCHED` (0x020E, unix ts),
  `MIN_PROTOCOL_VERSION` (0x0210), `PEAK_24H` (0x0211), `AVG_24H`
  (0x0212), `TAGS` (0x0310, comma-separated).
- **Capabilities:** `PROTOCOL_VERSION` (0x0300, e.g. `0x0197`),
  `SUPPORTS_HOPE` (0x0301), `SUPPORTS_TLS` (0x0302), `TLS_PORT`
  (0x0303), `SUPPORTS_INLINE_MEDIA` (0x0304), `SUPPORTS_VOICE`
  (0x0305), `SUPPORTS_LARGE_FILES` (0x0306), `SUPPORTS_IPV6` (0x0307),
  `HOPE_CIPHERS` (0x0309).
- **Content index:** `NEWS_COUNT` (0x0450), `MSGBOARD_COUNT` (0x0451),
  `FILES_COUNT` (0x0452), `TOTAL_FILE_SIZE` (0x0453),
  `LAST_NEWS_TIMESTAMP` (0x0454), `LAST_CHAT_TIMESTAMP` (0x0455).
- **Privacy:** `LISTING_CATEGORY` (0x0501, 12-value vocab incl.
  `warez`/`adult`-style), `LISTING_LANGUAGE_STRICT` (0x0502).
- **Tracker-injected:** `IS_PROMOTED` (0x0600), `FIRST_SEEN` (0x0601),
  `LAST_HEARTBEAT` (0x0602), `VERIFIED_ONLINE` (0x0603).

Unknown TLV IDs MUST be silently ignored. That's the forward-compat
escape hatch.

Feature flags we'd offer in the handshake:

- `FEAT_IPV6 = 0x0001` — yes, we already build with IPv6.
- `FEAT_QUERY = 0x0002` — only if we're sending `SEARCH_TEXT` /
  `PAGE_LIMIT` (Phase C below).
- `FEAT_CLIENT_AUTH = 0x0004` — never client-offered; the tracker sets
  this in its reply to demand auth before listing.
- `FEAT_REG_ACK = 0x0008`, `FEAT_HMAC = 0x0010` — informational, about
  UDP registration. Not our concern.

---

## 3. Backward compatibility — the only thing we mustn't get wrong

The spec recommends the same strategy on both sides: read 6 bytes,
check the version field, then read 2 more if it's `0x0003`. That works
for clients because v1 trackers send their full 6-byte response
(`HTRK\0\1`) immediately after the client's 8-byte v3 magic — the
extra 2 bytes sit harmlessly in the receive buffer until the server
closes the connection.

This also gives us a clean fallback path: if we send an 8-byte v3
handshake and the tracker comes back with `0x0001`, we transparently
fall into the existing v1 code path with no perceptible difference to
the user. The only thing we lose is the 2 extra bytes the v3 tracker
would have sent, which weren't there in the v1 reply anyway.

What we cannot do is the inverse: send 6 bytes and try to upgrade
later. The 8-byte magic has to go out before we can know which version
the tracker speaks.

The recommended client flow is therefore:

1. Send 8 bytes (`HTRK` + `0x0003` + our offered features).
2. Read 6 bytes.
3. If version == 1 or 2 → enter v1 receive loop (existing code, mostly
   unchanged).
4. If version == 3 → read 2 more bytes for the tracker's features,
   intersect with ours, send a listing request, switch to v3 parser.

`tracker_fetch_ctx` already has a 16-byte scratch buf, so this fits
without storage churn.

---

## 4. What we'd actually build

Below is sorted by user value relative to complexity. The first slice
is the floor for "GtkHx speaks v3 against a v3 tracker"; everything
after that adds capability without changing the floor.

### Phase A — detection + v3 record parser

The floor. With this in place, GtkHx is a working v3 client against any
v3 tracker, and unchanged against v1 trackers.

- Add to `src/hotline.h`: `HTRK_V3_MAGIC` (8 bytes), version
  constants, feature-flag bitmask, address-type constants, request /
  response type constants, H3 extension magic.
- Rework `on_tracker_magic_sent`: send 8 bytes instead of 6.
- Rework `on_tracker_header_read`: read 6, branch on version, possibly
  read 2 more, dispatch.
- New v1 path: same as today (existing code, just lifted into its own
  callback chain).
- New v3 path:
  - Send listing request: `00 01 00 00` (request_type=1, field_count=0).
  - Read 10-byte response header.
  - Loop: read 1-byte address type → branch on addr_type → read
    address bytes → read fixed `port/nusers/name_len(u16)/name/desc_len(u16)/desc/tlv_count(u16)` → loop TLVs.
  - For Phase A, TLV walker just `g_input_stream_skip_async`s the
    value bytes. We don't surface anything; we just need to advance the
    stream to the next record cleanly. (The TLV catalogue lands in
    Phase B.)
- Hostname address type: feed the resolved name (`HOSTNAME` from TLV
  *or* the inline string from `addr_type=0x48`) to
  `g_socket_client_connect_to_host_async` later; for the tracker
  listing itself, we just store it as the display address.
- IPv6 address type: extend `struct tracker_server` (and the
  `tracker-server-create` signal payload) to carry a more flexible
  address. Simplest path: replace the `struct in_addr addr` with a
  small union `{ in_addr, in6_addr, char hostname[256] }` plus a type
  byte. The connect path already goes through `hx_connect()` which
  takes a string, so the connect side doesn't change — only the
  binary-tree key and the column-2 inet_ntop call do.

Tests:

- A Tier 2 test that decodes the spec's example wire capture (the
  84-byte "Retro Hub" record) into a populated `struct tracker_server`
  with three TLVs walked. We'd add a `tracker_v3_parse_record(const
  guint8 *buf, gsize len, struct hx_tv3_record *out)` style helper in
  a new `src/tracker_v3.{c,h}` so it's drivable without the network
  scaffolding.
- A Tier 2 round-trip for the 8-byte handshake encoder.
- Optional: a Go mock v3 tracker in `tests/integration/mock-tracker/`
  (mirroring the chat-history mock pattern) so we can Tier-3 the
  state machine end-to-end. Not strictly needed for Phase A — the
  Tier 2 parser tests cover the wire format and the state machine is
  small enough to walk by hand.

### Phase B — TLV decoding + new metadata in the UI

Once Phase A can advance through TLVs, start *using* them. Each TLV
group is independent and can land in its own commit.

- Extend `struct tracker_server` with the fields we care about.
  Probably worth a sub-struct `struct hx_tv3_meta` so the v1 path can
  leave it `NULL`.
- Surface them in the tracker list. Two reasonable directions:
  - **Conservative:** add 1–3 columns to the existing `GtkHList`
    (e.g. Country flag, HOPE badge, Tags). Cheap, no UI churn.
  - **Ambitious:** redo the tracker window as a `GtkColumnView`
    (long-pending wish across the codebase — it'd also be the first
    consumer to retire `gtk_hlist_compat`) with column visibility
    toggle and richer cells. Probably a separate effort; not a Phase B
    blocker.
- A row click-through that pops a transient "Server details" sheet
  with the full TLV bag (`SERVER_SOFTWARE`, `RULES_URL`,
  `CONTACT_URL`, `UPTIME`, peak users, etc.) — easy to ship and high
  signal for users picking a server.
- Filter against `LISTING_CATEGORY` and `MATURITY` in the tracker UI
  if we want (the spec calls these out explicitly as
  client-presentable filters). Worth deferring until we have real v3
  trackers populated with these fields to test against.
- Optional: tee the parser output through `debug_log("tracker", ...)`
  category so `GTKHX_DEBUG=tracker` traces tracker-side flows
  symmetrically with `proto`.

### Phase C — search & pagination

- Tee the `GtkSearchEntry` text into a `SEARCH_TEXT` TLV in the
  listing request when the tracker advertises `FEAT_QUERY`. Today's
  client-side DFA filter still runs over whatever the tracker returns;
  this just lets the tracker pre-filter at scale.
- Pagination: drop in `PAGE_OFFSET` / `PAGE_LIMIT` if we ever care
  about huge listings. Not urgent — current trackers fit comfortably
  in one response.
- Keep the client-side filter regardless. v1 trackers can't do
  server-side search, and even with v3 we want incremental as-you-type
  filtering to keep working without a network round-trip per
  keystroke. Easiest split: the tracker query is "what we narrow to on
  the network" (re-fired on Refresh / Enter), the in-window search
  entry is "what we narrow to in the rendered list" (re-fired on every
  keystroke against cached results).

### Phase D — TLS on TCP/5498

- `GSocketClient` natively supports TLS via
  `g_socket_client_set_tls(client, TRUE)` plus
  `GTlsClientConnection`. We already use GIO for the connection, so
  the wiring lives entirely inside `tracker_fetch_start`.
- Cert handling: trust the system store for public trackers; offer
  per-tracker "trust this self-signed cert" for private ones (the
  bookmark equivalent of the Hotline connect dialog's per-server pref
  bag). Probably stored as a SHA-256 fingerprint pin in
  `gtkhxrc`/the tracker pref list.
- Fallback flow per spec: try TLS first, on failure retry without TLS
  and treat as v1. That's two connect attempts per tracker on TLS
  failure — fine for the few-trackers-per-session case but worth
  caching the "this tracker doesn't speak TLS" verdict so we don't
  re-pay it on each Refresh.
- This is the only part of the v3 plan that meaningfully touches
  *security* — and even then only on the listing side, which is
  inherently public info. Compromising this isn't a path to compromising
  any actual Hotline session.

### Phase E (optional) — IPv6 server records / hostname records end-to-end

Phase A already parses the address-type byte; Phase E is just making
sure `hx_connect()` and the Connect dialog actually do the right thing
when handed a v6 string or hostname. The connect path uses
`GSocketClient` which handles all three transparently, but the
bookmarks file format and the Connect dialog's parsing routines may
need a once-over. Out of scope: anything that requires changes to the
Hotline session protocol itself — IPv6 transport for a Hotline
*session* is already in place; this is just plumbing the tracker
output through the existing connect path.

### Out of scope (explicitly)

- Server-side registration (`SERVER_SOFTWARE`, HMAC-signed datagrams,
  nonces, REG_TOKEN). We don't run Hotline servers.
- `FEAT_CLIENT_AUTH` listing-time login. None of the public trackers
  we know of demand it, and the spec leaves it optional. If we ever
  hit one that requires it we can add `AUTH_LOGIN` / `AUTH_PASS` TLVs
  to the listing request in a few hours.
- Federation / HTFD. Tracker-to-tracker concern.
- Content Index injection. Same — tracker-side.

---

## 5. Files we'd touch

| File                              | Change                                                                                       |
|-----------------------------------|----------------------------------------------------------------------------------------------|
| `src/hotline.h`                   | New constants (8-byte magic, version, features, addr types, H3 magic, request/response types) |
| `src/tracker_v3.{c,h}` *(new)*    | Pure parser/encoder (TLV walker, record decoder, 8-byte handshake builder, request builder). Sized to be drivable from Tier 2 tests without a socket. |
| `src/network.c`                   | Split tracker state machine into v1 and v3 chains; add version-detect branch in `on_tracker_header_read`; rename to `on_tracker_v1_header_read`, add `on_tracker_v3_features_read` + `on_tracker_v3_listing_request_sent` + `on_tracker_v3_response_header_read` + per-record state. |
| `src/tracker.c`                   | Extend `struct tracker_server`; render new columns; details popover (Phase B); search-text TLV from search entry (Phase C). |
| `src/tracker.h`                   | New signature for `tracker_server_create` (or an additional v3 variant carrying the TLV bag). |
| `src/gtkhx_session.{c,h}`         | Extend `tracker-server-create` signal payload (most ergonomic: a new boxed `HxTrackerServer` type — same pattern Phase 5 used for `HxChatEvent` / `HxMsgEvent`). |
| `src/gtkhx.c`                     | Signal-handler adapter follow-up.                                                              |
| `src/proto_trace.{c,h}`           | New `tracker` debug category if we want hex-trace coverage symmetric with `proto`.            |
| `tests/proto/test_tracker_v3.c` *(new)* | Decode the spec's example wire capture; round-trip the 8-byte handshake; verify the listing-request encoder; TLV walker happy path + malformed-input rejection. |
| `tests/integration/mock-tracker/` *(new, optional)* | Tiny Go mock that speaks v3 for Tier 3 coverage of the state machine. |
| `docs/tracker-v3-scoping.md`      | This file.                                                                                   |

Estimated weight: Phase A is ~600 LOC of C across the new module +
network.c changes, plus ~250 LOC of Tier 2 tests. Phase B–D each add
~150–300 LOC.

---

## 6. Open decisions

These are calls that benefit from your weigh-in before any code lands.
None are blocking — every one of them has a reasonable default.

1. **Boxed `HxTrackerServer` vs. extending the signal arg list.** Phase
   5 already added `HxChatEvent` / `HxMsgEvent` as boxed types for the
   exact same shape of problem (signals whose payload grew past three
   or four fields). I'd lean on that precedent. The alternative is to
   keep the seven scalar args and add a `gpointer tlv_meta` to the
   tail — uglier but a smaller diff if we want to land Phase A and
   defer Phase B.

2. **Mock tracker as a Go service in `tests/integration/mock-tracker/`,
   matching the chat-history mock pattern?** The Tier 2 parser tests
   cover the wire format directly so we don't *need* it, but it'd let
   us Tier-3 the full state machine including timeouts and tracker
   list traversal. Small investment, high reuse if we end up wanting
   to test other tracker-side behaviour later (federation, content
   index).

3. **TLS-first vs. TLS-opt-in for Phase D.** Spec says clients SHOULD
   try TLS first and fall back to plain. Worth a per-tracker pref bit
   ("require TLS" / "try TLS first" / "plain only") since some private
   trackers might be plain-only forever, and we don't want to waste
   the connect+timeout on every refresh.

4. **Self-signed cert UX.** A first-time TOFU prompt ("This tracker
   uses a self-signed certificate. Pin SHA256 ABCD…? [Trust] [Cancel]")
   matches the SSH idiom but is one more dialog. Alternative: pin
   automatically and surface a "manage tracker certs" pane in
   Settings. Mid-term I think TOFU is right, but it depends on how
   often we expect private trackers in this user population.

5. **GtkColumnView migration of the tracker list as part of Phase B,
   or stay on `gtk_hlist_compat`?** v3 doubles the useful columns
   (country flag, software, HOPE/TLS badges, peak users…), which is
   the kind of thing GtkColumnView's per-column cell factories handle
   cleanly. But it's a meaningful detour from "implement v3" into
   "modernize the UI", and we have other consumers waiting on the
   compat shim's retirement (files.c, news15.c, etc.). My instinct is
   to ship Phase A+B on `gtk_hlist_compat` first, then take a separate
   pass at GtkColumnView as part of a broader compat-shim retirement.

6. **Tracker-side debug category.** Want a `GTKHX_DEBUG=tracker`
   category that mirrors `proto` for the listing flow? Free if we add
   it during Phase A; awkward to backfill later if we don't.

7. **Where do v1-tracker memories of "this is v1" live?** Caching the
   verdict per-tracker-hostname avoids paying the TLS+fallback cost on
   every Refresh, but means new tracker software upgrades wouldn't be
   noticed until the user manually clears state. Reasonable resolution:
   cache in-memory for the session, re-probe at startup. Worth
   confirming the trade-off before committing.

---

## 7. Suggested branch + commit shape

Branch: `claude/tracker-v3-phase-a` (then `-phase-b`, etc. as we
expand).

Per the repo conventions, one squashed commit per branch at PR time.
Inside the branch, organize as:

1. `tracker_v3.{c,h}` parser/encoder module + Tier 2 tests (compiles
   standalone, doesn't touch the network path yet).
2. Wire-up commit: state machine split in `network.c`, new constants
   in `hotline.h`, signal payload boxed type.
3. UI wiring commit: new columns, signal-handler adapter update,
   detail popover (Phase B if landing together).

If we ship Phase A only, that's commits 1 + 2; UI work is a no-op
because the new TLV bag isn't surfaced yet, just walked-over.

---

## 8. Server availability

As of 2026-05-23 I'm not aware of any production-deployed v3 tracker.
The reference implementations live under fogWraith's umbrella; Janus
is at v1 today. This is the same shape of problem we had with the
chat-history extension — we'll need a Go mock server in the test
matrix to validate Phase A+B in CI, with the option to retire it (or
just keep it as a regression net) once a real v3 tracker exists.

Worth checking before starting: ping the fogWraith repo to see if
there's a public test endpoint or a reference Go implementation we can
point our integration matrix at instead of writing our own mock from
scratch.
