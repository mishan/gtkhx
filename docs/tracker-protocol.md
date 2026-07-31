# Tracker protocol (HTRK v1 and v3)

GtkHx talks to Hotline trackers to populate the server list. Two wire
versions matter: the original v1 that every legacy tracker speaks, and
v3, which adds a real request/response exchange, non-IPv4 addresses,
UTF-8 strings, and a per-record TLV metadata trailer.

The v3 specification is **not vendored in this repository** and the
upstream link is to an unpinned branch. The tables below are therefore
the in-repo record of the wire format; treat them as the reference.

GtkHx is a *client*. The spec covers three roles — tracker, registering
server, listing client — and only the last is in scope. Server-side
registration, HMAC-signed registration datagrams, nonces, registration
tokens, replay protection, tracker-side rate limiting, federation, and
the tracker-to-tracker protocol are all out of scope and not
implemented. (The `H3` extension magic `0x4833`, which marks the start
of a v3 TLV extension block inside a UDP registration datagram, is
defined in `src/hotline.h` purely so a reader can match the spec
section it appears in — we never register.)

## Version detection: a timed probe with fallback

**Real pre-spec v1 trackers do not respond to a v3 handshake at all.**

The spec's recommended detection is symmetric: read 6 bytes, look at
the version field, read 2 more if it says `0x0003`. That works for a
spec-compliant implementation. It does not describe what is actually
deployed. Every v1 tracker we have tested — `hxtrackd`, and the
hxd-derived trackers generally — `memcmp`s the **full 6-byte magic**
against `"HTRK\0\1"`. A client sending `"HTRK"` + `0x0003` fails that
comparison, and the tracker silently falls through: the connection
stays open and **nothing is sent back**. The extra bytes do not sit
harmlessly in a buffer waiting to be ignored; there is no reply to
ignore them alongside.

So the shipped design is a **probe with a watchdog and a reconnect**:

1. Write the 8-byte v3 handshake: `"HTRK"` + `u16 0x0003` + `u16`
   offered feature flags.
2. Read the 6-byte response **under a timeout**.
3. Dispatch:
   - Response version `0x0003` → read the trailing 2 feature bytes,
     send a listing request, run the v3 record flow.
   - Response version `0x0001` / `0x0002` → a tracker that read only 6
     bytes and answered; drop straight into the v1 record flow on this
     same connection.
   - **Timeout, short read, connection close, or junk → inconclusive.**
     Close the connection, open a fresh one, write the 6-byte v1 magic
     `"HTRK\0\1"`, and run v1. No watchdog on the v1 attempt — we
     already know what this endpoint is.

The watchdog defaults to 2000 ms and is overridable for slow test rigs
via **`GTKHX_TRACKER_V3_PROBE_MS`**.

The inverse — send 6 bytes and upgrade later — is impossible: the
8-byte magic has to go out before we can learn which version the
tracker speaks. The reconnect is the price of that.

Layered on top, the transport itself is TLS-first with a plain-TCP
fallback, so the worst-case ladder is TLS fail → plain → v3 probe → v1.
See `docs/tls.md` for the trust rules and the per-tracker TLS verdict
cache.

## Wire-level differences, v1 → v3

| Aspect | v1 | v3 |
|---|---|---|
| Handshake length | 6 bytes (`HTRK\0\1`) | 8 bytes (`HTRK` + `version u16 = 0x0003` + `feature_flags u16`) |
| Handshake direction | Client → server only; server immediately sends the list | Bidirectional; both sides send 8 bytes, then the client must send a listing request |
| Read strategy | Read the 14-byte response header | Read 6 bytes; if the version is `0x0003`, read 2 more for feature flags |
| Listing request | None — the server sends as soon as the handshake is done | `request_type u16 = 0x0001` + `field_count u16` + N TLV query fields |
| Response header | 14 bytes (`server_count` at offset 10) | 10 bytes: `response_type u16 = 0x0001`, `total_size u32`, `total_servers u16`, `record_count u16` |
| Server record fixed header | 8 bytes: ipv4(4) + port(2) + nusers(2) + name_len(1) | Variable: `addr_type u8` + address + `port u16` + `nusers u16` + `name_len u16` + `desc_len u16` |
| Address types | IPv4 only | `0x04` IPv4 (4B), `0x06` IPv6 (16B), `0x48` hostname (2B length + UTF-8 bytes) |
| String length prefix | 1 byte (Pascal string) | 2 bytes (`u16` big-endian) |
| Per-record TLV trailer | None | `tlv_count u16` + N `{ID:u16, Len:u16, Value:Len bytes}` |
| Padding slot convention | First-byte-zero entries are padding — skip without decrementing the count | Doesn't exist; `addr_type` is the discriminator |
| String encoding | MacRoman in practice | UTF-8 mandatory |
| Total response size cap | `u16` server count | `u32` total size + `u16` total servers (still 65535 servers max) |
| Multi-message response | No | No — single response, but pagination lets the client re-request the next page |
| Transport | Plain TCP/5498 | TLS on TCP/5498 SHOULD; plain TCP MAY be accepted for v1/v2 backcompat |

### v1 record layout

```text
  14-byte response header
    [0..9]   opaque (msg type + protocol ver + msg-id)
    [10..11] u16 BE — number of server records to follow
    [12..13] opaque

  Per server record (variable length):
    [0..3]   u32 BE — IPv4 address (network byte order; a zero first
                      octet marks a padding/empty slot, NOT a record)
    [4..5]   u16 BE — TCP port
    [6..7]   u16 BE — users currently on this server
    [8..9]   reserved
    [10]     u8     — server name length N
    [11..]           — server name (CR / ANSI possible; normalise)
    [11+N]   u8     — description length M
    [12+N..]         — description (CR / ANSI possible; normalise)
```

Padding slots do not count against `nservers`, which means a buggy or
hostile tracker dribbling zero-prefixed slots forever would hang the
reader; the client caps the number of padding slots it will skip.

### v3 record layout

```text
  addr_type  u8      0x04 IPv4 | 0x06 IPv6 | 0x48 hostname
  address            4 bytes | 16 bytes | u16 BE length + that many UTF-8 bytes
  port       u16 BE
  nusers     u16 BE
  name_len   u16 BE  + name bytes
  desc_len   u16 BE  + description bytes
  tlv_count  u16 BE  + tlv_count × {id u16 BE, len u16 BE, value len bytes}
```

An unknown `addr_type` byte is fatal for the record: there is no length
prefix to skip past, so the parser bails and the fetch closes the
connection. A future spec revision adding address types needs either a
new branch or a skip-unknown length convention.

### Constants

```text
HTRK_MAGIC             "HTRK\0\1"    (6 bytes, v1)
HTRK_V3_MAGIC_PREFIX   "HTRK"        (4 bytes; u16 BE version follows)
HTRK_V3_HANDSHAKE_LEN  8
HTRK_VERSION_V1/V2/V3  0x0001 / 0x0002 / 0x0003
HTRK_V3_REQ_LIST       0x0001        (listing-request type)
HTRK_V3_RESP_LIST      0x0001        (listing-response type)
HTRK_V3_RESP_HDR_LEN   10
HTRK_V3_ADDR_IPV4      0x04
HTRK_V3_ADDR_IPV6      0x06
HTRK_V3_ADDR_HOSTNAME  0x48
HTRK_V3_EXT_MAGIC      0x4833 ("H3")  — registration-datagram extension marker
HTRK_TCPPORT           5498
HTRK_UDPPORT           5499           — registration; unused, we never register
```

### Handshake feature flags

A `u16` BE bitmask in the trailing 2 bytes, offered by both sides; the
negotiated set is the AND of the two. Bits 5–15 are reserved, MUST be
zero, and MUST be ignored on receipt.

| Bit | Name | Value | Client posture |
|---|---|---|---|
| 0 | `FEAT_IPV6` | `0x0001` | Offered — the client handles all three address types |
| 1 | `FEAT_QUERY` | `0x0002` | Not offered; only meaningful once we send query TLVs |
| 2 | `FEAT_CLIENT_AUTH` | `0x0004` | Never client-offered; the tracker sets it to demand auth before listing |
| 3 | `FEAT_REG_ACK` | `0x0008` | Informational, about UDP registration — not our concern |
| 4 | `FEAT_HMAC` | `0x0010` | Informational, about UDP registration — not our concern |

## TLV catalogue

Unknown TLV IDs **MUST be silently ignored**. That is the spec's
forward-compat escape hatch, and the client's walker and typed decoder
both honour it: an unrecognised ID advances the cursor and is dropped.

### Listing-request TLVs (client → tracker, query parameters)

| ID | Name | Type |
|---|---|---|
| `0x1001` | `SEARCH_TEXT` | UTF-8 string |
| `0x1010` | `PAGE_OFFSET` | integer |
| `0x1011` | `PAGE_LIMIT` | integer |

### Server-record TLVs (tracker → client)

All OPTIONAL. Integers are big-endian; booleans are "any non-zero value
byte is true", so "present with value 0" and "absent" both render false.

**Address & connectivity (`0x0100` block)** — usually redundant with
the record's `addr_type` byte:

| ID | Name | Type |
|---|---|---|
| `0x0100` | `ADDRESS_IPV6` | 16 bytes |
| `0x0101` | `HOSTNAME` | UTF-8 string |

**Descriptive (`0x0200` block):**

| ID | Name | Type / notes |
|---|---|---|
| `0x0200` | `SERVER_SOFTWARE` | UTF-8 string, e.g. `hxd/2.0` |
| `0x0201` | `COUNTRY_CODE` | ISO 3166-1 alpha-2 |
| `0x0202` | `REGION` | freeform city / region |
| `0x0203` | `LANGUAGE` | ISO 639-1 |
| `0x0204` | `MAX_USERS` | u16 — 0 is meaningfully different from absent |
| `0x0205` | `MATURITY` | closed vocab: 0 general, 1 teen, 2 mature, 3 adult; unknown values MUST be treated as 0 |
| `0x0206` | `UPTIME` | u32 seconds |
| `0x0207` | `RULES_URL` | UTF-8 string |
| `0x0208` | `BANNER_URL` | UTF-8 string |
| `0x0209` | `ICON_URL` | UTF-8 string |
| `0x020A` | `LINK_DOWN_MBIT` | u32 |
| `0x020B` | `LINK_UP_MBIT` | u32 |
| `0x020C` | `TIMEZONE_OFFSET_MIN` | **signed** i16, minutes from UTC |
| `0x020D` | `CONTACT_URL` | UTF-8 string |
| `0x020E` | `SERVER_LAUNCHED` | u32 unix timestamp |
| `0x0210` | `MIN_PROTOCOL_VERSION` | u16 |
| `0x0211` | `PEAK_24H` | u16 |
| `0x0212` | `AVG_24H` | u16 |

**Capabilities (`0x0300` block):**

| ID | Name | Type / notes |
|---|---|---|
| `0x0300` | `PROTOCOL_VERSION` | u16, e.g. `0x00BE` = 190 |
| `0x0301` | `SUPPORTS_HOPE` | boolean |
| `0x0302` | `SUPPORTS_TLS` | boolean |
| `0x0303` | `TLS_PORT` | u16 |
| `0x0304` | `SUPPORTS_INLINE_MEDIA` | boolean |
| `0x0305` | `SUPPORTS_VOICE` | boolean |
| `0x0306` | `SUPPORTS_LARGE_FILES` | boolean |
| `0x0307` | `SUPPORTS_IPV6` | boolean |
| `0x0309` | `HOPE_CIPHERS` | comma-separated cipher names |
| `0x0310` | `TAGS` | comma-separated (numerically in the 0x0300 block, descriptive in nature) |

**Content index (`0x0400` block):**

| ID | Name | Type / notes |
|---|---|---|
| `0x0450` | `NEWS_COUNT` | u32 |
| `0x0451` | `MSGBOARD_COUNT` | u32 |
| `0x0452` | `FILES_COUNT` | u32 |
| `0x0453` | `TOTAL_FILE_SIZE` | u32 |
| `0x0454` | `LAST_NEWS_TIMESTAMP` | u32 unix ts; 0 = never |
| `0x0455` | `LAST_CHAT_TIMESTAMP` | u32 unix ts; public chat only per spec |

**Privacy / visibility (`0x0500` block):**

| ID | Name | Type / notes |
|---|---|---|
| `0x0500` | `PRIVATE_LISTING` | boolean |
| `0x0501` | `LISTING_CATEGORY` | closed vocab (below); unknown values MUST be treated as 0 |
| `0x0502` | `LISTING_LANGUAGE_STRICT` | boolean, informational |

`LISTING_CATEGORY` vocabulary: 0 unspecified, 1 general, 2 development,
3 archive, 4 warez, 5 gaming, 6 media, 7 education, 8 research,
9 file sharing, 10 social, 11 security, 12 creative.

**Tracker-injected (`0x0600` block)** — set by the tracker, not by the
registering server:

| ID | Name | Type |
|---|---|---|
| `0x0600` | `IS_PROMOTED` | boolean — operator-pinned entry |
| `0x0601` | `FIRST_SEEN` | u32 unix ts |
| `0x0602` | `LAST_HEARTBEAT` | u32 unix ts |
| `0x0603` | `VERIFIED_ONLINE` | boolean |

## Where it lives

- **`rust/crates/hxnet/src/tracker.rs`** — the per-connection protocol
  engine: the v3 probe, the v1 flow, the record loops. Generic over any
  `AsyncRead + AsyncWrite` stream, so it is unit-testable against
  scripted in-memory streams with no socket. It also holds the
  defensive caps: a 16 MiB ceiling on the v3 records blob (the spec
  allows a `u32`, and a hostile tracker shouldn't be able to make the
  client allocate 4 GiB) and the v1 padding-slot cap.
- **`rust/crates/hxnet/src/tracker_fetch.rs`** — the fetch runner: the
  serial walk over the configured tracker list and the full
  connect/transport/probe fallback ladder. Unlike the old C path, which
  interleaved per-record progress as bytes arrived, the engine reads a
  whole listing before returning it, so records for one tracker arrive
  as a burst and the progress indicator ticks per tracker.
- **`src/tracker_v3.{c,h}`** — the surviving C ABI over the pure v3
  encoders/parsers: handshake builder, handshake-response decoder,
  listing-request builder, response-header parser, single-record parser
  (borrowing slices into the caller's buffer, reporting bytes
  consumed), and the TLV walker. The parsing delegates to
  `hotline-proto`.
- **`src/tracker_v3_meta.{c,h}`** — the typed TLV decoder. One sweep
  over the blob, each ID stored into the matching struct field,
  unknown IDs skipped. Strings are `g_utf8_make_valid`-ed at
  construction so subscribers can hand them straight to Pango.
  Companion `has_*` flags distinguish "set to zero" from "absent" for
  the numeric fields where that matters.
- **`src/tracker_parser.{c,h}`** — the v1 header and fixed-record
  parsers. They stay: any tracker still running v1 has to keep working.
- **`src/tracker_event.{c,h}`** — the boxed `HxTrackerServer` payload
  of the `tracker-server-create` session signal, including the address
  formatter that renders all three `addr_type` forms to a display
  string, and the MacRoman → UTF-8 transcode on the v1 path. (v3 is
  UTF-8 mandatory and is validity-checked, not transcoded.)
- **`rust/crates/gtkhx-ui/src/tracker/`** — the window: per-tracker
  sections, the Country and Caps columns fed from the typed meta, the
  server-details dialog, and the client-side search filter.

## Security posture

The only part of this that touches security is the transport, and only
on the listing side — a tracker listing is inherently public
information. Compromising it is not a path to compromising a Hotline
session. The certificate handling is nevertheless the same store and
the same decision as the control channel (see `docs/tls.md`), so a pin
set for a session server and a pin set for a tracker coexist without
stepping on each other.

## Test targets

**Argus** (VesperNet) is a real, production v3 tracker and is in the
integration matrix as a container (`tests/argus/`). It speaks v1, v2,
and v3 on one TCP port with automatic version detection, and serves
deterministic content from a `promoted_servers` config section, so no
real Hotline server has to register for a listing to have content. A Go
mock is not needed and was not built.

- **Argus has no native TLS**, so the container runs an `stunnel`
  sidecar that terminates TLS and forwards to plain Argus on localhost.
  The TLS test walks the same handshake → listing-request → records
  flow the plain test does, pinning that the wire shape is unchanged
  under TLS.
- **Gotcha:** Argus emits *all* `promoted_servers` entries as v3 `0x48`
  hostname records, even when the configured address is a literal IP.
  The spec permits it — a hostname is a UTF-8 string and an IP literal
  is a valid hostname — and the client's parser and emit path handle
  all three address types uniformly.
- `promoted_servers` only takes address / name / description, so the
  TLV trailer Argus emits for them is just the tracker-injected
  `0x0600` block. The richer `0x0200` / `0x0300` / `0x0400` / `0x0500`
  blocks only appear if a real server registers and supplies them, so
  those get their coverage from synthetic fixtures against the typed
  decoder.

**`hxtrackd`** (bundled with mhxd) speaks v1 only, and is the pinned
target for the probe-then-fallback path — the real-world case the timed
probe exists for.

## Open

- **Server-side search and pagination.** The client offers only
  `FEAT_IPV6` in its handshake today; it never sets `FEAT_QUERY` and
  never sends `SEARCH_TEXT` / `PAGE_OFFSET` / `PAGE_LIMIT`. The design
  when it lands splits cleanly in two: the **tracker query** is what we
  narrow to on the network (re-fired on Refresh / Enter, only when the
  tracker advertises `FEAT_QUERY`), and the **in-window search entry**
  is what we narrow to in the rendered list (re-fired on every
  keystroke against cached results). The client-side filter stays
  regardless — v1 trackers cannot search server-side, and incremental
  as-you-type filtering must keep working without a network round-trip
  per keystroke. Pagination is not urgent: current listings fit
  comfortably in one response.
- **`FEAT_CLIENT_AUTH` listing-time login.** No public tracker we know
  of demands it and the spec leaves it optional. It would be
  `AUTH_LOGIN` / `AUTH_PASS` TLVs on the listing request.
- **Per-tracker TLS preference.** The TLS-first-then-plain ladder is
  currently automatic with an in-memory verdict cache. A per-tracker
  "require TLS / try TLS / plain only" preference would let a
  known-plain private tracker skip the handshake attempt permanently
  instead of re-probing each launch.
