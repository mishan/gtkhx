# The Hotline protocol

A reference for the Hotline wire protocol as it was shipped by Hotline Communications
and Hotsprings (1.0 through 1.9), and for every extension layered on it since. It is
written for implementers of clients and servers. The goal is that someone holding
only this document can talk to a 1.2 server, a 1.9 server, and a modern extended
server, and know which of the things they are doing are protocol and which are one
implementation's habit.

GtkHx's own subsystem docs go deeper on the parts GtkHx implements:
[tls.md](tls.md), [tracker-protocol.md](tracker-protocol.md), [voice.md](voice.md),
[video.md](video.md), [inline-media.md](inline-media.md), [gif-icons.md](gif-icons.md).
Where this document and one of those disagree about the wire, this one should be
fixed to match the evidence, not the other way round.

## Sources, and which one wins

The protocol has never had a single authoritative text. What exists, ranked by how
much weight it carries when sources disagree:

1. **The original source code.** Hotsprings released the Hotline Connect 1.9 client,
   server and both trackers under the GPL in April 2003 as "Openline 1.0.1"
   (SourceForge project `hotline`, folder "Openline"). What the 1.9 server actually
   does is the ground truth for the base protocol. Mirrors and descendants:
   [scottanderson/openline](https://github.com/scottanderson/openline) (a 2026 MinGW-w64
   port that builds `hlserver.exe` and both trackers with CMake),
   [NebuHiiEjamu/Openline](https://github.com/NebuHiiEjamu/Openline), and Underline
   (below). File names cited in this document without a path are in that tree:
   `HotlineClientServerCommon.h` (the transaction and field constants),
   `HotlineServTrans.cp` (the server's transaction handlers), `HotlineServ.cp`,
   `UTransact.cp` and `UFieldData.cp` (framing), `UFileSys(M).cp` / `UFileSys(W).cp`
   (the flattened file format), and the two tracker sources.
2. **The original servers, observed.** The official Windows builds of Hotline Server
   1.8.2, 1.8.4 and 1.9.1 run under Wine (see
   [Reference implementations](#reference-implementations)). Behavior quoted here as
   "the 1.9 server does X" has been read from the source and, where it matters, checked
   against a running binary.
3. **The official protocol document**, "Hotline Protocol 1.9" (`HLProtocol.doc`,
   Hotline Communications, last saved April 2003). It is accurate on layouts and
   transaction lists and thin on behavior. Readable copies:
   [codebox.org.uk](https://codebox.org.uk/assets/documents/hotline/HLProtocol.doc)
   (the SourceForge download link is dead), and Markdown ports in
   [scottanderson/openline](https://github.com/scottanderson/openline/blob/main/HLProtocol.md),
   [fogWraith/Hotline](https://github.com/fogWraith/Hotline/blob/main/Docs/Protocol/Hotline.md),
   and [jhalter/mobius](https://github.com/jhalter/mobius/blob/master/docs/PROTOCOL.md).
4. **Extension specifications.** Each extension section below names its own. Most
   modern extensions are specified in [fogWraith/Hotline](https://github.com/fogWraith/Hotline)
   under `Docs/Protocol/`; that repository calls itself a draft and tracks an unpinned
   branch, so its line numbers drift.
5. **Reverse-engineered guides and wikis.** Virtual1's "Hotline Server Protocol Guide"
   (v1.1.1, 2000; [PDF](https://codebox.net/assets/documents/hotline/HLProtocol.pdf))
   and his file-transfer guide
   ([PDF](https://codebox.net/assets/documents/hotline/HLFileTransferProtocol.pdf)) predate
   the official document's publication and are still useful for 1.2-era behavior.
   [hlwiki.com](https://hlwiki.com/index.php/Protocol) (source:
   [tagban/hlwiki](https://github.com/tagban/hlwiki)) has the "Unofficial Protocol
   Extensions" page, which is the only catalog of several community extensions.
6. **Other implementations**, especially mhxd ([kangsterizer/mhxd](https://github.com/kangsterizer/mhxd)),
   Mobius, Janus and hxd-ng. These are where extensions live, and where quirks come
   from.

Numbers in this document are decimal unless written `0x…`. Every multi-byte integer
on the wire is big-endian.

## History and lineage

Hotline was written by Adam Hinkley for the classic Mac OS in 1996 on his AppWarrior
application framework; Hotline Communications was incorporated in 1997. The company
lost its funding in 2001, cancelled 2.0, and its assets went to Hotsprings Inc. in
2002, which shipped the last official releases in 2003 and then published the source.

Each release reports a version number in field 160 of the login exchange. That number
is the only way either side learns what the other speaks, and the protocol forks on it
in several places (see [Version negotiation](#version-negotiation)).

| Release | Field 160 | What it added to the protocol |
|---|---|---|
| 1.0–1.2.3 (1996–98) | absent | The base set: chat, private chat, private messages, the flat message board, files, accounts, broadcasts. The login carries the user's name and icon. 1.2.3 was a paid product with registration codes. |
| 1.5 (1999) | 150 | Threaded news ("bundles", categories, articles), the agreement handshake (transaction 121), the new login shape without name and icon. The first 1.5 builds used an older category-list record, replaced on 15 April 1999. |
| 1.5.1 / 1.5.5 | 151 | Server-generated refuse-message and auto-response notices; server banners (122) for clients that report 151 or later. |
| 1.7.0 / 1.7.2 | 151 | HTTP tunneling (ports base+2 and base+3), folder downloads and uploads, the download queue. |
| 1.8.2 (2000) | 182 | Folder-transfer, private-chat and messaging privileges honored; the Kill Download transaction. The expiry date was removed and the server became freely redistributable. |
| 1.8.4 | 184 | Multi-account administration (348/349); the "has all privileges" and "has no privileges" user flags. |
| 1.8.5 | 185 | The keepalive transaction (500); queued downloads wait for the server's go-ahead (211). |
| 1.9 / 1.9.1 / 1.9.2 (2003, Hotsprings) | 190 | The flat message board back alongside threaded news; IP-range bans; XML logging. 1.9.2 was the last official client. |

Community lineages, each of which added its own extensions:

- **The hxd family** (Unix): hxd (Ryan Nielsen), shxd/synhxd (Devin Teske), kxd, and
  mhxd, a 2023 merge of the three. HOPE came from here. mhxd
  ([kangsterizer/mhxd](https://github.com/kangsterizer/mhxd)) is the rig's most-used target; its known bugs are in
  [mhxd-bugs.md](mhxd-bugs.md).
- **Avaraline** (avaraline.net): GIF icons and a handful of other transactions,
  adopted by mhxd in 2004.
- **GLoarbLine**: a derivative of the official source (1.9.7) with extra transactions,
  fields and privilege bits.
- **Underline**: a 2003 fork of the Openline client that added Blowfish encryption on
  the control connection. Revived in 2026 by "199x" as
  [Underline Hotline 1.9.6](https://199x.online/underline/), with client, server and
  tracker builds for everything from 68K Macs to Linux, Windows 11 and Apple silicon.
- **Mobius** (Go, [jhalter/mobius](https://github.com/jhalter/mobius)): dedicated-port
  TLS, and the TLS port in tracker registrations.
- **fogWraith / VesperNet**: the Janus server and Argus tracker, and the specifications
  for capability negotiation, UTF-8, large files, voice, inline media, chat history,
  messaging, colored nicknames and tracker v3.
- **hxd-ng** ([mishan/hxd-ng](https://github.com/mishan/hxd-ng)): a Rust server sharing
  hx-libs with GtkHx; the origin of the video extension.
- Others that matter for interop: Heidrun (client and server), phxd, Lemoniscate,
  Frogblast, Panorama, AniClient, Pitbull Pro, Hotline Navigator, Obsession.
  fogWraith's [client-server-versions.md](https://github.com/fogWraith/Hotline/blob/main/Docs/client-server-versions.md)
  lists the field-160 value each of them reports.

---

## Transport

### Ports

A server has a *base port*, 5500 by default. The official server listens on four:

| Port | Use |
|---|---|
| base | control connection (transactions) |
| base + 1 | file transfers (HTXF) |
| base + 2 | control connection tunneled over HTTP (1.7+) |
| base + 3 | file transfers tunneled over HTTP (1.7+) |

Trackers listen on TCP 5498 for listings and UDP 5499 for registrations.

TLS is not part of the original protocol. The convention, from Mobius, is a second
pair of ports carrying the unchanged protocol inside TLS from the first byte: 5600 for
control and 5601 for transfers (see [TLS](#tls-on-dedicated-ports)).

### Handshake

The client opens the control connection and sends 12 bytes:

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | protocol ID | `TRTP` |
| 4 | 4 | sub-protocol ID | `HOTL` |
| 8 | 2 | version | 1 |
| 10 | 2 | sub-version | 2 |

The server answers with 8 bytes: `TRTP` and a 32-bit error code, 0 for success. On a
non-zero code both sides close.

What the 1.9 server checks (`UTransact.cp`, `HotlineServ.cp`):

- A protocol ID other than `TRTP` is dropped with **no reply**.
- A version other than 1, a sub-protocol other than `HOTL`, or a sub-version other than
  2 or 3 gets `TRTP` + error 1, then a disconnect.
- Sub-version **3** turns the socket into a file-transfer connection: the server
  discards the TRTP layer and expects an HTXF header next, as if the connection had
  arrived on base+1. No official client uses this; it exists for single-port setups.
- The handshake times out after 20 seconds.

mhxd compares the 12 bytes exactly and tolerates a client that sends its login before
reading the server's 8 bytes ("old hx"), for one packet. GtkHx treats any 8-byte reply
other than `TRTP 00 00 00 00` as fatal.

### Transactions

After the handshake, both directions carry *transactions*. Each is a 20-byte header
followed by a data part:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | flags | Reserved, 0. HOPE reuses it as a rekey count (see [HOPE](#hope)). |
| 1 | 1 | is reply | 0 = request or unsolicited notification, 1 = reply. Receivers treat any non-zero value as a reply. |
| 2 | 2 | type | The transaction type. |
| 4 | 4 | ID | Chosen by the sender of a request; a reply carries the request's ID. |
| 8 | 4 | error code | 0 = success. Meaningful only in replies. |
| 12 | 4 | total size | Size of the whole transaction's data, across all parts. |
| 16 | 4 | data size | Size of the data that follows this header. |

The data part starts with a 16-bit **field count**, followed by that many fields.
Some implementations (mhxd, GtkHx's C headers) describe a 22-byte header that includes
the count. The count is data: it is inside both size fields.

**Replies.** The 1.9 server's replies have type **0**; match replies to requests by ID
alone. Some servers echo the request type instead (Heidrun's server did, as
`0x0001006b` for a login reply), so a client should not require 0 either. The 1.9 server
only ever sends error code 0 or 1. On error it includes field 100 (error text), which
the 1.9 client displays whenever it is present, even with error 0.

**Transaction IDs.** The official client and server count up per connection and skip
0. Nothing forbids 0: GtkHx and mhxd both treat it as an ordinary ID.

**Fragmentation.** A transaction may be split across several frames with the same ID.
Every frame repeats the total size and carries its own data size; the receiver appends
until it has the total (`UTransact.cp`). The official software always sends a
transaction in one frame, as do mhxd and GtkHx. A receiver **must** find the end of a
frame from the data size (offset 16), not the total size (offset 12): framing by the
total size desyncs the stream on the first fragmented transaction.

GtkHx reads frames through hx-libs' `hxsession`, which does both: it frames by the
data size and joins a fragmented transaction before handing it on. A frame whose
fields already fill it is taken whole whatever its total size says, since a server
overstating the total is likelier than a split that ends exactly on a field
boundary.

**Limits in the 1.9 server:**

- A frame whose total size or data size is **0**, or larger than the maximum, **kills
  the connection**. Always send at least the 2-byte field count, even with no fields.
- The maximum is 512 KB on the server (the framework default is 2 MB). mhxd caps at
  256 KB; GtkHx at 1 MB.

### Field encoding

```
u16 count
repeat count:
    u16 field ID
    u16 size
    u8  data[size]
```

- Fields are not padded or aligned.
- Order is not significant. A field ID may repeat (user lists, file lists, invitee
  lists), and repeated fields must be read by position: the 1.9 server's lookup-by-ID
  returns an arbitrary one of several.
- A truncated field ends parsing silently; the fields before it are kept.
- No field can exceed 65535 bytes. Payloads that might (a large file list, a long
  message board) are split into many fields or capped by the sender.

**Integers** are variable-width. The official writer emits 2 bytes when the value fits
in 16 bits unsigned, and 4 bytes otherwise, so a negative icon ID goes out as 4 bytes.
The official reader accepts 1, 2 and 4 bytes, and reads the first 4 bytes of anything
longer. Readers should accept 1, 2, 4 and (for extension fields) 8 bytes; writers of
base fields should use 2 or 4. mhxd sends some sizes in the narrowest width that fits.

A few "integers" are really opaque bytes. The **reference number** (107) and the
**banner type** (152) are copied raw from server memory. A Windows server's reference
numbers are therefore byte-swapped relative to a Mac server's. That is harmless as long
as clients echo the four bytes back unchanged, which they must.

**Strings** are raw bytes with no length prefix and no terminator; the field size is
the length. The encoding is **Mac Roman** unless the UTF-8 extension has been
negotiated, and line breaks are **CR**. Several structured fields carry a "script"
word next to a name; it is always sent as 0 and ignored.

**Obfuscated strings.** Logins and passwords are sent with every byte inverted
(`b ^ 0xFF`). This is not encryption. It applies to the login and password in Login
(107) and to logins in the account transactions, with one exception noted there.

### Dates

An 8-byte date:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | base year |
| 2 | 2 | milliseconds |
| 4 | 4 | seconds since 00:00, 1 January of the base year |

The date is January 1 of `year` plus `seconds` plus `milliseconds`. A Mac server sends
year 1904 and Mac-epoch seconds, in local time. The Windows server fills it from local
time with the current year as the base. mhxd sends 1904. A receiver must normalize
rather than assume either base. The 32-bit seconds field overflows for 1904-based dates
in 2040; the capabilities extension's "modern dates" bit exists for that.

### File paths

Fields 202, 212 and 325 carry a path:

```
u16 count
repeat count:
    u16 script   (0)
    u8  length
    u8  name[length]
```

- Paths are relative to the client's root. On the 1.9 server that is the `Files`
  folder, or `Users/<login>/Files` if that exists: a per-account root. News paths work
  the same way with `News` and `Users/<login>/News`.
- A path field shorter than 6 bytes, or absent, means the root.
- A component of `..` must be refused. The official server validates names in its
  platform layer; mhxd rejects `..` and `/`.
- Each component is at most 255 bytes. Janus fails on components of 253 bytes or more
  ([janus-bugs.md](janus-bugs.md)), and a Windows-hosted server is limited by the host's
  path length.

---

## Session lifecycle

### Version negotiation

Both sides send field 160 in the login exchange. A client puts its version in the Login
request; a server puts its version in the Login reply. The number decides three things:

- **The login shape.** Pre-1.5 clients send their name and icon in Login. 1.5 and later
  clients send them afterward, in Agreed (121).
- **The agreement.** A server that sends field 160 is a 1.5+ server and will send the
  agreement (109) and wait for Agreed. A server that sends no field 160 is a 1.0–1.2
  server: there is no agreement step, and a 1.5-shaped client must send its name and
  icon with Set Client User Info (304) instead.
- **Feature gates.** The official client enables threaded news for servers ≥ 150,
  expects the download-queue go-ahead from ≥ 185, sends keepalives to ≥ 185, uses
  Kill Download on ≥ 182 and multi-account administration on ≥ 184. The official
  server sends the banner (122) only to clients ≥ 151, and the threaded-news category
  list in its current format only to clients ≥ 15 (a relic of the April 1999
  cut-over: a client that sends no version gets the old format).

Values observed in the field: none from 1.2.3 and hlserver.com; 150 from 1.5; 151 from
1.5.5 and 1.7.2; 182, 184, 185 and 190 from the matching official releases (182, 184
and 190 confirmed against the running binaries); 185 from mhxd when configured as a
1.8.5 server; 190 from Badmoon; 200 from Janus and from the HotStuff client; 254 (0xFE)
from GtkHx as a client. mhxd can be set to send none, to impersonate a 1.2.3 server.

GtkHx's 254 is its own entry in fogWraith's list, not its release version, and stays
the same from one release to the next. Releases up to 1.4.0 sent 185, which the list
gives to the official 1.8.5 client and to Pitbull Pro, so a server could not tell them
apart. 254 clears every gate a server is known to apply to clients (the banner at 151,
mhxd's keepalive at 150) and nothing documented changes at 190 or above.

### Login, 1.5 and later

```
C → S   Login (107): login, password, version
S → C   reply: version, community banner ID, server name
S → C   Show Agreement (109)
C → S   Agreed (121): name, icon, options[, auto-response]
S → C   reply (empty)
S → C   Notify Change User (301) to everyone, about the new user
S → C   User Access (354)
S → C   Server Banner (122), if configured and the client is ≥ 151
C → S   Get User Name List (300), news, and so on
```

- **Login (107)** carries field 105 (login) and 106 (password), both obfuscated, and
  160 (version). An empty or missing login means the guest account. The 1.9 server
  lowercases the login, replaces CR with `-`, and looks up the account by name.
- **Agreement (109)** carries the text in field 101, or field 154 = 1 when the account
  has the "don't show agreement" privilege, or no field at all when the server has no
  agreement file.
- **Agreed (121)** carries 102 (name), 104 (icon), 113 (options: bit 0 refuse private
  messages, bit 1 refuse private chat, bit 2 automatic response), and 215 (the
  automatic response text) when bit 2 is set. **Always include field 113**, even as 0:
  Mobius drops the connection when it is missing.
- Before the login completes, the 1.9 server accepts only Login and Agreed. **Any other
  transaction disconnects the client** as a probable attack. A client that sends Get
  User Name List or news requests before Agreed will be dropped; this is why GtkHx
  holds its post-login requests until after it has agreed.
- A failed login gets error 1 and field 100 ("Incorrect login.", or a ban message).
  The server disconnects 30 seconds later if the client doesn't.

### Login, 1.0 to 1.2

A 1.2 client sends 102 (name) and 104 (icon) inside Login and never sends Agreed. The
1.9 server keeps a compatibility path for it: a Login carrying a non-empty name **and**
a non-zero icon is logged in immediately and sent User Access (354), then the
agreement. A Login with an icon of 0 does not take that path.

The converse trap: a client that sends a 1.5-shaped Login to a 1.9 server and then Set
Client User Info (304) instead of Agreed is disconnected, because 304 is not allowed
before login. Against a server that reports no version, 304 is the right move, and the
first User Name List reply may show the client with an empty name until it arrives.

### Keepalive and timers

- **Keepalive (500)** is empty in both directions and resets the server's idle and
  away timers. The official client sends it after 3 minutes of silence to servers
  ≥ 185, and Get User Name List (300) to older ones. Servers older than 1.8.5 answer
  500 with an error ("Uh, no." from some); hlserver.com drops idle connections, so a
  client should keep pre-1.8.5 sessions alive with 300. GtkHx pings every 60 seconds
  when the server version is ≥ 150.
- The 1.9 server disconnects a connection that hasn't logged in within 60 seconds.
- After 10 minutes without a transaction, the server marks the user away (flag bit 0)
  and broadcasts 301. Any transaction clears it. There is no idle disconnect for
  logged-in users.

### Flood protection

The 1.9 server enforces limits that are compiled in and cannot be configured. Clients
and test harnesses have to stay under them:

| Trigger | Consequence |
|---|---|
| More than 10 consecutive connections from one address within 30 seconds | 30-minute ban |
| More than 120 transactions from one client in a minute | disconnect + 30-minute ban |
| More than 10 KB of chat per minute (each send counts at least 90 bytes) | disconnect + ban |
| More than 20 private messages, invitations or broadcasts per minute | disconnect + ban |
| More than 25 name or icon changes per minute | disconnect + ban |
| Five consecutive identical messages, chat lines or posts | disconnect + ban |

Temporary bans live in memory, so a server restart clears them. The server also has a
per-address connection limit (5 by default, 0 for unlimited): connections over it are
closed without a reply.

---

## Transactions

The complete 1.9 set, as defined in `HotlineClientServerCommon.h` and handled in
`HotlineServTrans.cp`. "C" is client, "S" is server. Unless a row says otherwise, a
request gets a reply with the listed reply fields, or an error reply with field 100.
Field numbers are listed in [Fields](#fields).

### Chat, messages and users

| ID | Name | Dir | Request | Reply / payload | Notes |
|---|---|---|---|---|---|
| 100 | Error | — | | | Defined, never sent. Errors travel in the reply header. |
| 101 | Get Messages | C→S | none | 101: the whole message board | The flat 1.2-style message board, capped at 65535 bytes. Privilege 20. |
| 102 | New Message | S→C | | 101: the new post | Pushed to every reader after a post. The client prepends it. |
| 103 | Old Post News | C→S | 101 | empty | Privilege 21. Posts over 8192 bytes are refused. The server adds a `From <name> (<date>):` header and an underscore footer. |
| 104 | Server Message | S→C | | **Private message:** 103, 102, 113, 101, optional 214 (quoted text). **Notice:** 101, optional 109 = 1. | Non-zero 103 means a private message from that user. Without it, 109 = 1 marks a server message and anything else an administrator message. |
| 105 | Send Chat | C→S | 101, 109 (1 = emote), 114 (absent or 0 = public chat) | **no reply** | Privilege 10. The server formats the line: `\r` + the name right-aligned in 13 columns + `:  ` + text, or `\r *** name text` for an emote. Text is capped at 8192 bytes; the official client refuses to send more than 2048. |
| 106 | Chat Message | S→C | | 101 (formatted, starts with CR), 114 for a private chat | Sent to clients with privilege 9. |
| 107 | Login | C→S | 105, 106, 160; 1.2 clients add 102, 104 | 160, 161, 162 | See [Session lifecycle](#session-lifecycle). |
| 108 | Send Instant Message | C→S | 103 (target), 113, 101 (≤ 4096), optional 214 | empty | Privilege 40. Options: 1 user message, 2 refuse-message notice, 3 refuse-chat notice, 4 automatic response. Delivered to the target as 104. Since 1.5.1 the server answers for a target who refuses messages or has an automatic response. |
| 109 | Show Agreement | S→C | | 101, or 154 = 1, or nothing | The official client also accepts banner fields 151–153 here; the 1.9 server uses 122 instead. |
| 110 | Disconnect User | C→S | 103, 113 (1 = also temporary ban, 2 = also permanent ban), 101 (ban reason) | empty | Privilege 22. Refused for a target with privilege 23. |
| 111 | Disconnect Message | S→C | | 101 | Sent only when the server's operator quits politely. |
| 112 | Invite to New Chat | C→S | 103 × n | 114, 103, 104, 112, 102 (the creator) | Privilege 11. Creates a private chat whose only member is the requester and invites the listed users. |
| 113 | Invite to Chat | both | C→S: 114, 103 × n | S→C: 114, 103 (inviter), 102 | The C→S form gets **no reply**. |
| 114 | Reject Chat Invite | C→S | 114 | **no reply** | The chat sees a `<<< name declined invitation to chat >>>` line. Pre-1.5.1 clients with auto-response or refuse-chat set answer invitations with this automatically. |
| 115 | Join Chat | C→S | 114 | 115, then 300 × n (every member, including the joiner) | **No invitation check**: anyone who knows a chat ID can join. The server makes chat IDs hard to guess for that reason. |
| 116 | Leave Chat | C→S | 114 | **no reply** | An empty chat is destroyed. |
| 117 | Notify Chat Change User | S→C | | 114, 103, 104, 112, 102 | A user joined a private chat. |
| 118 | Notify Chat Delete User | S→C | | 114, 103 | A user left, including by disconnecting. |
| 119 | Notify Chat Subject | S→C | | 114, 115 | |
| 120 | Set Chat Subject | C→S | 114, 115 | **no reply** | No membership check. |
| 121 | Agreed | C→S | 102, 104, 113, 215 | empty | See [Login, 1.5 and later](#login-15-and-later). |
| 122 | Server Banner | S→C | | 152 (four-char type: `URL `, `JPEG`, `GIFf`, `PICT`, `BMP `), 153 (URL: the image, or the click-through link) | For an image banner, the client fetches the bytes with 212. |
| 300 | Get User Name List | C→S | none | 300 × n | Users not yet logged in, or with empty names, are omitted. |
| 301 | Notify Change User | S→C | | 103, 104, 112, 102 | A user joined or changed name, icon or flags. |
| 302 | Notify Delete User | S→C | | 103 | |
| 303 | Get Client Info Text | C→S | 103 | 101 (formatted text: name, account, address, transfers), 102 | Privilege 24. |
| 304 | Set Client User Info | C→S | 102, 104, 113, 215 | **no reply** | Broadcasts 301. Not accepted before login. |
| 355 | User Broadcast | both | C→S: 101 (≤ 4096) | C→S reply: empty. S→C: 103, 102, 101 | Privilege 32. Relayed to everyone but the sender. |
| 500 | Keep Connection Alive | C→S | none | empty | 1.8.5 and later. |

### Files

| ID | Name | Dir | Request | Reply | Notes |
|---|---|---|---|---|---|
| 200 | Get File Name List | C→S | 202 (optional) | 200 × n | No privilege needed. Invisible files are omitted. See [drop boxes](#drop-boxes-and-upload-folders). |
| 202 | Download File | C→S | 201, 202, optional 203 (resume data), optional 204 = 2 ("view": raw data fork, no wrapper) | 108 (bytes that will follow on the transfer connection), 207 (data + resource fork size), 107, 116 (only if queued) | Privilege 2. A view request for an empty file gets only 108 = 0. |
| 203 | Upload File | C→S | 201, 202, 108, 204 = 1 to resume | 107; 203 when resuming | Privilege 1. Without privilege 25, the target folder's name must contain "upload" or "drop box". Refused if the file exists (unless resuming), or if the disk is short of size + 500 KB. |
| 204 | Delete File | C→S | 201, 202 | empty | Privilege 0 or 6, by item type. The 1.9 server moves the item to the Trash / Recycle Bin rather than deleting it. |
| 205 | New Folder | C→S | 201, 202 (parent) | empty | Privilege 5. |
| 206 | Get File Info | C→S | 201, 202 | 205, 206, 213, 207 (files only), 201, 208, 209, 210 | No privilege needed. 205 is a friendly type name ("Folder", "Text File", …) or the raw type code. |
| 207 | Set File Info | C→S | 201, 202, optional 211 (new name), optional 210 (comment; a single `00` byte clears it) | empty | Rename needs privilege 3 or 7, comment 28 or 29. |
| 208 | Move File | C→S | 201, 202, 212 (destination folder) | empty | Privilege 4 or 8. |
| 209 | Make File Alias | C→S | 201, 202, 212 | empty | Privilege 31. A Mac alias or a Windows shortcut. |
| 210 | Download Folder | C→S | 201, 202 | 108 (sum of the files' fork sizes), 220 (items below the folder, folders included), 107, 116 | Privilege 39. An empty folder gets 108 = 0 and 220 = 0 and no reference. |
| 211 | Download Info | S→C | | 107, 116 | Queue position update; 116 = 0 means "connect now". |
| 212 | Download Banner | C→S | none | 108, 107 | Then a type-2 transfer. |
| 213 | Upload Folder | C→S | 201, 202, 108, 220, 204 = 1 to resume | 107, or empty when 220 is 0 | Privilege 38. Same folder-name rules as 203. |
| 214 | Kill Download | C→S | 107 | empty | Removes one of the requester's queued downloads. 1.8.2 and later. |

### Accounts

| ID | Name | Dir | Request | Reply | Notes |
|---|---|---|---|---|---|
| 348 | Get User List | C→S | none | 101 × n, each itself a complete field list: 102, 105 (obfuscated), 106 (`x` if set), 110 | 1.8.4 and later. Privilege 16. |
| 349 | Set User List | C→S | 101 × n, each a nested field list | empty | Batch create, rename, modify and delete (see below). |
| 350 | New User | C→S | 102, 105, 106, 110 | empty | Privilege 14. **You cannot grant privileges you lack**, except "don't show agreement". |
| 351 | Delete User | C→S | 105 | empty | Privilege 15. Online users of the account are notified and disconnected. |
| 352 | Get User | C→S | 105 **in plain text** | 102, 105 (obfuscated), 106 (`x` if set), 110 | Privilege 16. The only place a login is sent unobfuscated. |
| 353 | Set User | C→S | 102, 105, 106, 110 | empty | Privilege 17. A password of exactly one `00` byte means "keep the current one". **No check against granting more than you have**, unlike 350. Online users of the account get a fresh 354 and 301. |
| 354 | User Access | S→C | | 110 | After login and after changes to the account. |

Each record of **Set User List (349)** is a nested field list (the server copies at most
512 bytes of each):

| Contents | Meaning |
|---|---|
| 101 (old login, obfuscated) alone | delete that account |
| 101 + 105 with a different login | rename, then modify |
| 101 + other fields, same login | modify (password `00` = keep) |
| no 101, account doesn't exist | create |
| no 101, account exists | a 104 notice, not an error |

Any failed record makes the final reply error 1.

### News

| ID | Name | Dir | Request | Reply | Notes |
|---|---|---|---|---|---|
| 370 | Get News Category Name List | C→S | 325 (optional) | 323 × n; 320 × n for a client that sent no version or < 15 | No privilege needed. |
| 371 | Get News Article Name List | C→S | 325 (a category) | 321 | No privilege needed. |
| 380 | Delete News Item | C→S | 325 (including the item) | empty | Privilege 37 for a bundle, 35 for a category. |
| 381 | New News Folder | C→S | 201 (name), 325 (parent) | empty | Privilege 36. mhxd requires exactly this shape; a 325 that already includes the new name fails. |
| 382 | New News Category | C→S | 322, 325 (parent) | empty | Privilege 34. |
| 400 | Get News Article Data | C→S | 325, 326, 327 (`text/plain`) | 333 (absent if the flavor doesn't exist), 331, 332, 335, 336, 328, 329, 327, 330 | Privilege 20. |
| 410 | Post News Article | C→S | 325, 326 (parent; 0 = top level), 328 (≤ 31 bytes kept), 334, 327, 333 | empty | Privilege 21. The poster is the user's current name and the date the server's clock. |
| 411 | Delete News Article | C→S | 325, 326, 337 (1 = also replies) | empty | Privilege 33. |

The 1.9 server serves the flat message board (101/102/103) and threaded news side by
side; 1.5 through 1.8 servers have threaded news only. The 1.9 client shows the board
only for servers ≥ 190 or < 150.

Transaction IDs 201, 305–347, 356–369, 372–379, 383–399, 401–409 and 412–499 are not
used by the official protocol. See [ID allocations](#id-allocations) for what
extensions have put in the gaps.

---

## Fields

The base set. "Int" means a variable-width integer (see [Field encoding](#field-encoding)). "Str" means
raw bytes, the field size being the length.

| ID | Hex | Name | Type | Notes |
|---|---|---|---|---|
| 100 | 0x64 | Error Text | Str | |
| 101 | 0x65 | Data | Str / binary | Chat, messages, agreement, board, info text; nested lists in 348/349. |
| 102 | 0x66 | User Name | Str | The 1.9 server keeps 31 bytes. |
| 103 | 0x67 | User ID | Int | 16-bit; 0 is never assigned. |
| 104 | 0x68 | User Icon ID | Int | Signed 16-bit. |
| 105 | 0x69 | User Login | Str, obfuscated | |
| 106 | 0x6A | User Password | Str, obfuscated | |
| 107 | 0x6B | Reference Number | 4 raw bytes | Echo unchanged. |
| 108 | 0x6C | Transfer Size | Int | 32-bit. mhxd sends it as 16-bit for banners. |
| 109 | 0x6D | Chat Options | Int | 1 = emote; in 104, 1 = server message. |
| 110 | 0x6E | User Access | 8 bytes | See [Access privileges](#access-privileges). |
| 111 | 0x6F | User Alias | Str | Stored by the server, never sent by the official client. |
| 112 | 0x70 | User Flags | Int | See [User flags](#user-flags). mhxd calls it "color". |
| 113 | 0x71 | Options | Int | Private-message options, user options, ban options. mhxd calls it "ban". |
| 114 | 0x72 | Chat ID | Int | 32-bit. |
| 115 | 0x73 | Chat Subject | Str | |
| 116 | 0x74 | Waiting Count | Int | Download queue position. |
| 150 | 0x96 | Server Agreement | — | Defined, unused. |
| 151 | 0x97 | Server Banner | binary | Only parsed inside 109. |
| 152 | 0x98 | Server Banner Type | 4 raw bytes | A four-char code. |
| 153 | 0x99 | Server Banner URL | Str | |
| 154 | 0x9A | No Server Agreement | Int | 1. |
| 160 | 0xA0 | Version | Int | Client version in Login, server version in its reply. |
| 161 | 0xA1 | Community Banner ID | Int | 0 on everything but licensed "network" servers. |
| 162 | 0xA2 | Server Name | Str | |
| 200 | 0xC8 | File Name with Info | record | See [File list record](#file-list-record). |
| 201 | 0xC9 | File Name | Str | |
| 202 | 0xCA | File Path | path | |
| 203 | 0xCB | File Resume Data | RFLT | See [Resume data](#resume-data-rflt). |
| 204 | 0xCC | File Transfer Options | Int | 202: 2 = view. 203/213: 1 = resume. |
| 205 | 0xCD | File Type String | Str | |
| 206 | 0xCE | File Creator String | Str | |
| 207 | 0xCF | File Size | Int | |
| 208 | 0xD0 | File Create Date | date | |
| 209 | 0xD1 | File Modify Date | date | |
| 210 | 0xD2 | File Comment | Str | |
| 211 | 0xD3 | File New Name | Str | |
| 212 | 0xD4 | File New Path | path | |
| 213 | 0xD5 | File Type | 4 bytes | |
| 214 | 0xD6 | Quoting Message | Str | |
| 215 | 0xD7 | Automatic Response | Str | ≤ 127 bytes. |
| 220 | 0xDC | Folder Item Count | Int | |
| 300 | 0x12C | User Name with Info | record | See [User record](#user-record). |
| 319 | 0x13F | News Category GUID | — | Defined, unused. |
| 320 | 0x140 | News Category List Data | record | The pre-April-1999 format. |
| 321 | 0x141 | News Article List Data | record | |
| 322 | 0x142 | News Category Name | Str | |
| 323 | 0x143 | News Category List Data 1.5 | record | |
| 325 | 0x145 | News Path | path | |
| 326 | 0x146 | News Article ID | Int | 32-bit. In a post, the parent. |
| 327 | 0x147 | News Article Data Flavor | Str | A MIME type, `text/plain`. |
| 328 | 0x148 | News Article Title | Str | |
| 329 | 0x149 | News Article Poster | Str | |
| 330 | 0x14A | News Article Date | date | |
| 331 | 0x14B | Previous Article ID | Int | |
| 332 | 0x14C | Next Article ID | Int | |
| 333 | 0x14D | News Article Data | binary | |
| 334 | 0x14E | News Article Flags | Int | Some implementations misnamed this "parent thread"; the parent is 335. |
| 335 | 0x14F | Parent Article ID | Int | |
| 336 | 0x150 | First Child Article ID | Int | |
| 337 | 0x151 | Recursive Delete | Int | |

### File list record

Field 200, one per item:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | type code: `fldr` for a folder, `alis` for an alias whose target is gone |
| 4 | 4 | creator code (0 for folders) |
| 8 | 4 | size: data + resource fork for a file, **the number of items** for a folder |
| 12 | 4 | reserved (0) |
| 16 | 2 | name script (0) |
| 18 | 2 | name length |
| 20 | n | name |

An alias is listed under its own name with its target's type and size. Invisible files
are omitted. A Windows server derives type and creator from the file's extension. A
folder's Get File Info size is not a child count on every server: mhxd reports the
directory's size on disk there.

### News records

**323, News Category List Data 1.5** (Get News Category Name List, clients ≥ 15), one
per item. A bundle (folder):

```
u16 type = 2   u16 item count   u8 name length   name
```

A category:

```
u16 type = 3   u16 article count   u8 guid[16]   u32 add serial   u32 delete serial
u8 name length   name
```

The GUID and serials let a client track what it has read. The official client rejects
records under 5 or over 300 bytes, and parses the name by its length, since some
compilers pad the structure.

**320, News Category List Data** (the pre-April-1999 format): `u8 kind` followed by the
name, where kind 1 is a bundle, 10 a category and 255 anything else.

**321, News Article List Data** (Get News Article Name List), one field for the whole
category:

```
u32 ID (0)   u32 article count   u8 name length (0)   u8 description length (0)
repeat article count:
    u32 article ID
    date (8 bytes)
    u32 parent ID          (0 = top level)
    u32 flags
    u16 flavor count
    u8  title length, title       (≤ 63)
    u8  poster length, poster     (≤ 31)
    repeat flavor count:
        u8 MIME type length, MIME type
        u16 size
```

Articles are listed in thread order, depth first.

### User record

Field 300, also used in the Join Chat reply:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | user ID |
| 2 | 2 | icon ID (signed) |
| 4 | 2 | flags |
| 6 | 2 | name length |
| 8 | n | name |

The colored-nicknames extension appends a 4-byte color.

### User flags

Field 112, and the flags word in the user record:

| Bit | Mask | Meaning |
|---|---|---|
| 0 | 0x01 | away (idle 10 minutes) |
| 1 | 0x02 | administrator: the account has privilege 22 (Disconnect Users) |
| 2 | 0x04 | refuses private messages |
| 3 | 0x08 | refuses private chat |
| 4 | 0x10 | the account has every privilege (1.8.4+) |
| 5 | 0x20 | the account has no privileges (1.8.4+) |

### Access privileges

Field 110 is 8 bytes. **Bit 0 is the most significant bit of the first byte**: bit *n*
is `(byte[n / 8] >> (7 - n % 8)) & 1`, regardless of the host's byte order. (The
capabilities bitmask, later, numbers the other way.)

| Bit | Name | Bit | Name |
|---|---|---|---|
| 0 | Delete File | 21 | Post News Article |
| 1 | Upload File | 22 | Disconnect Users |
| 2 | Download File | 23 | Cannot Be Disconnected |
| 3 | Rename File | 24 | Get Client Info |
| 4 | Move File | 25 | Upload Anywhere |
| 5 | Create Folder | 26 | Use Any Name |
| 6 | Delete Folder | 27 | Don't Show Agreement |
| 7 | Rename Folder | 28 | Set File Comment |
| 8 | Move Folder | 29 | Set Folder Comment |
| 9 | Read Chat | 30 | View Drop Boxes |
| 10 | Send Chat | 31 | Make Aliases |
| 11 | Create Private Chat | 32 | Broadcast |
| 12 | Close Chat | 33 | Delete News Article |
| 13 | Show in List | 34 | Create News Category |
| 14 | Create Accounts | 35 | Delete News Category |
| 15 | Delete Accounts | 36 | Create News Folder |
| 16 | Read Accounts | 37 | Delete News Folder |
| 17 | Modify Accounts | 38 | Upload Folders |
| 18 | Change Own Password | 39 | Download Folders |
| 19 | Send Private Message | 40 | Send Message |
| 20 | Read News Article | 41–63 | unused by the official protocol |

What the 1.9 server actually enforces:

- 12, 13, 18 and 19 are **never checked**. The server gates private messages on 40, not
  19. mhxd leaves 12, 13, 18 and 19 reserved.
- Without 26, the user's displayed name is forced to the account name.
- Get File Name List, Get File Info, and both news list transactions need no privilege.
- The official client ignores 11, 38, 39 and 40 for servers older than 1.8.2 and
  assumes they are granted.
- Old clients can't set bit 40 when editing an account, so editing an account from a
  1.2.3 client clears it. mhxd works around this with a per-account override.

Bits 55 and up are allocated by extensions (see [ID allocations](#id-allocations)).
hlwiki lists GLoarbLine privilege bits in the 40–54 range, which overlaps the official
bit 40; treat GLoarbLine's bits as private to it. GtkHx's advice for 41–54 stands: don't
assign them, because deployed servers may already use them privately.

A 1.0/1.2 server may never send field 110. A client should treat a missing or all-zero
bitmap as "unknown, let the server decide" rather than "nothing allowed". Be aware that
mhxd's User Access is not the account's real access: it sends a fixed "everything"
bitmap.

### Drop boxes and upload folders

Both are recognized **by folder name**, case-insensitively:

- A folder whose name contains `drop box` is a drop box. Without privilege 30 (View Drop
  Boxes), a user listing it sees only incomplete uploads, and can't download from it or
  rename it.
- Without privilege 25 (Upload Anywhere), uploads are allowed only into folders whose
  name contains `upload` or `drop box`.

---

## File transfers

### Opening a transfer

A successful 202, 203, 210, 212 or 213 reply carries a **reference number** (107). The
client opens a new TCP connection to base+1 (or base+3 through the HTTP tunnel, or the
control port with sub-version 3) and sends a 16-byte header:

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | protocol | `HTXF` |
| 4 | 4 | reference | the four bytes of field 107, verbatim |
| 8 | 4 | data size | the upload size for 203; 0 otherwise |
| 12 | 2 | type | 0 file, 1 folder, 2 banner |
| 14 | 2 | reserved | 0 (the large-file extension puts flags here) |

- The official server identifies the transfer by reference, and for file uploads takes
  its expected size from the header's data size. mhxd ignores the header's type and
  uses the one it stored; Mac servers dispatch on it, so a folder transfer sent with
  type 0 hangs, each side waiting for the other.
- There is no completion message. The receiver counts bytes, then the connection
  closes.
- The official server expires an unclaimed reference after 500 seconds and aborts an
  upload that sends nothing for 120 seconds. mhxd leaks unclaimed references until
  restart ([mhxd-bugs.md](mhxd-bugs.md)).
- The 1.9 server checks its ban lists on the transfer port at accept time; on the
  control port it checks them at login.

### Flattened file object (FILP)

A file download and each file in a folder transfer is sent as a flattened file:

```
header, 24 bytes:   "FILP"  u16 version = 1  u8 reserved[16]  u16 fork count
fork header, 16:    u32 fork type  u32 compression (0)  u32 reserved (0)  u32 size
                    followed by size bytes
```

Forks are `INFO`, `DATA` and `MACR` (the resource fork). The **Mac server** always
sends three forks in that order, MACR even when empty. The **Windows server** sends two,
INFO and DATA. Receivers must accept forks in any order, skip unknown fork types, and
treat a non-zero compression type as an error. Period clients expect a zero-length MACR
header even when the count says 2, so writers should include one.

The INFO fork:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | platform: `AMAC` or `MWIN` |
| 4 | 4 | type code |
| 8 | 4 | creator code |
| 12 | 4 | flags (0) |
| 16 | 4 | platform flags (Finder flags on a Mac) |
| 20 | 32 | reserved |
| 52 | 8 | create date |
| 60 | 8 | modify date |
| 68 | 2 | name script |
| 70 | 2 | name length |
| 72 | n | name |
| 72+n | 2 | comment length |
| 74+n | m | comment |

- The Windows server caps the name at 63 characters, always sends an empty comment, and
  ignores the INFO fork on uploads. The Mac server applies the type, creator and Finder
  flags, clearing the alias and inited bits.
- mhxd writes the literal name `hxd` rather than the file's name, and so does GtkHx's
  uploader; receivers use the name from the transaction, not from INFO.
- The download's transfer size (108) is the whole flattened object:
  `24 + 16 × forks + fork sizes`. Field 207 is the raw data + resource size.

A **view** download (204 = 2) sends the data fork alone, with no FILP. The official
client uses it for its preview window.

### Resume data (RFLT)

```
"RFLT"  u16 version = 1  u8 reserved[34]  u16 count
repeat count:  u32 fork type  u32 bytes held  u32 reserved  u32 reserved
```

The canonical form has two entries, DATA and MACR, and is 74 bytes.

- **Download resume:** the client sends RFLT in field 203 with the bytes it already has.
  The server sends the FILP header, the whole INFO fork, and each fork's remainder.
- **Upload resume:** the client sets 204 = 1 and the server's reply carries RFLT
  describing its partial file.
- The **Windows server's upload-resume RFLT is malformed**: it says two entries and
  carries one, so it is 16 bytes short. mhxd's folder-upload resume reply sends an
  uninitialized RFLT. Parse by fixed offset (DATA at byte 46, MACR at 62), as mhxd does.
- A partial upload is stored with type `HTft` and creator `HTLC`, the Hotline
  partial-file type, and shown as such in listings. The Windows server also appends
  `.hpf` to the on-disk name.

### Folder transfers

After the HTXF header (type 1), the two sides exchange 16-bit **action codes**: 1 send
this file, 2 resume this file, 3 next item.

Each item is announced by a header:

```
u16 size of what follows
u16 type (1 = folder, 0 = file)
u16 path component count
path components (script, length, name), ending with the item's own name
```

The path is relative to the transferred folder. Items are depth-first, each folder's
header before its contents.

**Download** (the client drives):

```
C: HTXF header, then u16 3          (the official client sends both in one write)
loop:
  S: item header                    (or closes: no more items)
  folder:  C: u16 3
  file:    C: u16 1                 → S: u32 size, FILP
           C: u16 2, u16 n, RFLT    → S: u32 size, resumed FILP
           C: u16 3                 → skipped
```

**Upload** (the server drives):

```
C: HTXF header
loop:
  S: u16 3
  C: item header
  folder:  S creates it, then u16 3
  file:    S: u16 1                 → C: u32 size, FILP
           S: u16 2, u16 n, RFLT    → C: u32 size, the remainder
           S: u16 3                 → skipped (exists, or failed)
```

- The request's 220 counts every file and folder below the root; 108 is the sum of the
  raw fork sizes, not the flattened sizes.
- The official server skips items whose path exceeds 1024 bytes, excludes drop boxes
  the user can't see, and in the "waiting for next" state silently discards codes other
  than 3.
- mhxd does not recurse: subfolders arrive as empty folder items.
- The large-file spec describes the folder download with the server speaking first.
  Every implementation, and the official document, do what is described here. Its path
  encoding now matches the one above.
- In large-file mode the `u32 size` before a FILP is advisory: an item whose FILP does
  not fit in 32 bits is announced as 0, and the receiver finds the item's end from the
  FILP's own fork headers. GtkHx does so when the size is 0, waiting for the resource
  fork's header when the FILP declares a third fork and reading the empty one servers
  send anyway on a short timeout.

### Banner

212 returns a size and a reference; a type-2 transfer then carries the banner's raw
bytes, with no FILP. The 1.9 server leaves the socket open afterward. Banners are at most
256 KB. mhxd allows one banner fetch per session, after Agreed.

### Download queue

The official server counts active downloads against a global limit and a per-client
limit. A request over either is still accepted: the reply carries 116 > 0 and the item
waits. The server sends 211 whenever the position changes, and 211 with 116 = 0 when it
may start. Clients ≥ 1.8.5 wait for that before connecting; older clients connect at
once and their connection idles until the item is promoted. mhxd keeps a waiting
connection alive with an empty write about every two minutes.

---

## HTTP tunneling

1.7 servers accept the protocol wrapped in HTTP on base+2 (control) and base+3
(transfers), for clients behind proxies that pass only HTTP. The client opens two
connections to the tunnel port, one upstream and one downstream, identified by the same
36-character GUID:

```
POST http://host:port/<GUID> HTTP/1.0      Content-Type: hotline/protocol
                                           Content-Length: 999999999
GET  http://host:port/<GUID> HTTP/1.0
```

The server answers `302 Found` / `200 OK`. Each body is a sequence of chunks, an 8-byte
header `u32 code, u32 size` then the payload: code 1 is data, 2 padding, 3 disconnect.
Inside the data chunks runs exactly the byte stream of the plain ports. Tracker
listings through the tunnel use port 5497. Reconnect behavior has not been traced.

GtkHx does not implement tunneling. The protocol's own proxy story today is SOCKS.

---

## Trackers

### v1: listing

The client connects to TCP 5498 and sends `HTRK` + u16 version 1. The tracker echoes the
same six bytes and sends the list as one or more **batches**, each ≤ 8 KB:

```
u16 type = 1
u16 size of this batch after these 4 bytes
u16 total servers           (the same in every batch)
u16 servers in this batch
records
```

A record never straddles two batches. Each record is:

```
u8  address[4]        (network order)
u16 port
u16 user count
u16 flags             (0; the tracker echoes what the server registered)
u8  name length, name
u8  description length, description
```

The tracker **does not close the connection** after the list. A client stops after
reading *total servers* records. The simplest robust reader skips any 8-byte chunk that
starts with `00` without counting it: those are the continuation batch headers, and an
IPv4 address never starts with 0. hltracker.com batches at 30 000 bytes; the mhxd family
at 8191. The official client ignores the flags, always uses port 5498 (so a
`host:port` tracker address doesn't work with it), and stops parsing a batch when fewer
than 12 bytes remain.

Names and descriptions are Mac Roman.

### v1: registration

A server announces itself with a UDP datagram to port 5499, at startup and then every
5 minutes (mhxd: every 4 minutes and on every login and logout):

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | version = 1 |
| 2 | 2 | the server's TCP port |
| 4 | 2 | user count |
| 6 | 2 | flags = 0 |
| 8 | 4 | pass ID: a random number kept for the server's lifetime |
| 12 | … | name, description, tracker password, each a Pascal string |

- **The password string is mandatory**, even if empty (a single `00`). The official
  trackers drop a datagram without it.
- The tracker takes the address from the UDP source, not the datagram.
- The official server writes the pass ID in host byte order; trackers only compare it.
  The official trackers identify a server by name plus address, or name plus pass ID.
  mhxd's hxtrackd keys on source address and source port, so a restarted server
  appears twice until the old entry expires.
- The official trackers expire entries after 11 minutes (Old Tracker) or 31 minutes
  (New Tracker). mhxd's hxtrackd never refreshes its expiry counter on a heartbeat, so
  entries drop out a few intervals after they first registered and reappear on the
  next heartbeat.
- mhxd's hxtrackd truncates names to 31 bytes and then misreads the description of any
  longer name.

### v2

The official document defines a version 2 handshake that carries a login and password
(32 bytes each), and a registration that carries a login as well as a password. The
1.9 source has both behind switches that are turned off, and no tracker in that tree
accepts them. fogWraith's
[Tracker-Protocol-v2.md](https://github.com/fogWraith/Hotline/blob/main/Docs/Protocol/Tracker/Tracker-Protocol-v2.md)
reconstructs it from GLoarbLine and synhxd; synhxd's handler is a stub and phxd closes
v2 connections. Argus claims v2. In practice: send and accept v1; treat a v2 reply to a
listing as v1.

The mhxd family also recognizes `TRXL`/`TRXR` listing magics and a registration
version `0x5801`, none of which were ever implemented.

### v3

Specified by fogWraith in
[Tracker-Protocol-v3.md](https://github.com/fogWraith/Hotline/blob/main/Docs/Protocol/Tracker/Tracker-Protocol-v3.md)
and implemented by Argus (tracker), Janus and hxd-ng (registration) and GtkHx
(listing). The listing handshake is 8 bytes, `HTRK` + version 3 + a feature-flag word;
the answer is a request/response exchange of typed records with TLV metadata, UTF-8
strings, IPv6 and hostname addresses, search, paging and optional client
authentication. Registration is the v1 datagram with version 3 and a TLV block after
magic `H3`, with optional HMAC-SHA256 signing and a tracker acknowledgment. Companion
specs cover tracker federation and an HTTP API.

The full wire format, TLV catalog, and the probe GtkHx uses to fall back to v1 are in
[tracker-protocol.md](tracker-protocol.md). Two points belong here:

- **Pre-spec v1 trackers do not tolerate the v3 handshake.** They compare all six bytes
  of `HTRK 00 01` and either close or go silent. A client must probe with a timeout and
  reconnect with v1, not rely on the spec's "the extra two bytes are harmless".
- The spec's worked examples have arithmetic errors in their byte counts; implement
  from the tables.

**Mobius** extends v1 differently: since late 2025 it puts the server's TLS port in the
reserved 2 bytes after the user count, in both the registration datagram and the listing
record.

---

## Extensions

Each extension below names who introduced it, where it is specified, and the numbers
it uses. [ID allocations](#id-allocations) collects the numbers in one place.

### Negotiation: how a client learns what a server supports

There is no single mechanism, which is the most important thing to know about Hotline
extensions:

- **Version number (160).** The base protocol's only one, and too coarse for anything
  after 1.9.
- **HOPE's login exchange** negotiates ciphers and compression, and carries the only
  in-band way for a client to name itself (App ID and App String).
- **The capabilities bitmask (field 0x01F0)** is the modern mechanism: the client
  advertises bits in Login, and the server echoes the subset it enables.
- **Probing.** GIF icons have no bit, so a client sends one of the transactions and
  waits. That must be timeout-based: Janus answers unknown transactions with nothing at
  all, while the official server and hlserver.com answer with an error.
- **Auto-opt-in.** Colored nicknames turn on for a session the first time the server
  receives a color from it.
- **Out of band.** Tracker v3 metadata advertises TLS, HOPE and other features;
  fogWraith's [Hotline-Info-Port.md](https://github.com/fogWraith/Hotline/blob/main/Docs/Protocol/Hotline-Info-Port.md)
  proposes a JSON discovery listener at base − 1.

Every extension must be safe against a server that knows nothing about it. Fields a
server doesn't recognize are ignored by every implementation examined; transactions it
doesn't recognize are not.

### HOPE

**HOPE** ("Hotline One-time Password Extension"; hlwiki expands it as "Hotline Open
Protocol Extensions") replaces the obfuscated password with a challenge-response login
and optionally encrypts and compresses the control connection. It came from the hxd
family around 2002–2003, is implemented by mhxd, Janus and GtkHx, and is specified after
the fact in fogWraith's
[HOPE-Secure-Login.md](https://github.com/fogWraith/Hotline/blob/main/Docs/Protocol/HOPE-Secure-Login.md).
Underline's 2003 client fork added a Blowfish variant of the same scheme on the control
connection only.

Fields:

| ID | Name | Notes |
|---|---|---|
| 0x0E01 | App ID | 4-byte code naming the client (GtkHx sends `GTKx`). |
| 0x0E02 | App String | Client name and version. |
| 0x0E03 | Session Key | 64 bytes from the server. |
| 0x0E04 | MAC Algorithm | Algorithm list. |
| 0x0EC1 / 0x0EC2 | Cipher | Server→client / client→server direction. |
| 0x0EC3 / 0x0EC4 | Cipher Mode | `STREAM` (default) or `AEAD`. |
| 0x0EC5 / 0x0EC6 | Cipher IV | Defined; no implementation uses it. |
| 0x0EC7 / 0x0EC8 | Checksum Algorithm | Defined; no implementation uses it. |
| 0x0EC9 / 0x0ECA | Compression | Server→client / client→server. |

An **algorithm list** is `u16 count`, then per entry `u8 length` and ASCII name.

**Step 1, identification.** The client sends Login with login = one `00` byte, password
= one `00` byte, a MAC list (e.g. `HMAC-SHA256`, `HMAC-SHA1`, `HMAC-MD5`), optionally a
cipher list (0x0EC2) and compression list (0x0ECA), App ID and App String, and an empty
session key. The single `00` bytes are required in practice: mhxd and Janus read an
empty login as a plain guest login.

**Step 1 reply.** The server sends the session key (mhxd: its own IPv4 address and port
followed by 58 random bytes, so a client can detect a man in the middle by checking
them, which breaks behind NAT), the chosen MAC as a one-entry list, and the chosen cipher
and compression. mhxd repeats each choice under both direction IDs, and puts the chosen
MAC's name in the login field as the signal that the login itself should be sent as a
MAC.

**Step 2, authenticated login.** The client sends Login again:

- login: `MAC(key = login, text = session key)` if the server signaled it, otherwise the
  obfuscated login;
- password: `MAC(key = password, text = session key)`;
- the chosen cipher (0x0EC1) as a one-entry list, **omitted entirely** when none was
  chosen (mhxd closes on a present but empty list);
- name and icon, **always**, even if empty (mhxd closes without them);
- version, and the capabilities field.

mhxd finds the account by trying `MAC(key = account name)` against every account, caps
received MACs at 20 bytes (so it cannot verify HMAC-SHA256), and accepts only
`HMAC-SHA1`, `HMAC-MD5`, `SHA1` and `MD5`. The plain `SHA1`/`MD5` "MACs" are
`hash(key ‖ text)`, not HMAC.

**Encryption.** It starts right after step 2 is sent: the step-2 reply is the first
encrypted frame. With `M` the negotiated MAC, `pw` the password and `sk` the session key:

```
password_mac = M(pw, sk)
K1 = M(pw, password_mac)        server → client key
K2 = M(pw, K1)                  client → server key
```

The stream ciphers are `RC4`, `BLOWFISH` (OFB-64, IV zero) and `IDEA` (OFB-64), all
applied as a continuous stream per direction. mhxd applies one cipher to both
directions; its client supports one per direction. RC4 is retired from GtkHx.

**Rekeying.** A sender may put a count *N* in the header's flags byte. The header is
encrypted under the current key; the sender then replaces its key *N* times with
`M(key, sk)` and encrypts the body under the new key. The receiver decrypts the header,
reads *N*, rotates its own key the same way, clears the byte, and decrypts the body. The
OFB state is **not** reset on rekey. mhxd rekeys about 3 frames in 16, with *N* between
1 and 63, and never while compressing. A receiver must be frame-aware: a plain byte
stream desyncs on the first rekey.

**Compression.** `GZIP` on the wire is zlib (RFC 1950), one persistent stream per
direction, each transaction flushed with `Z_SYNC_FLUSH`. Compress, then encrypt. Janus
adds `LZ4` (one frame per transaction) and `ZSTD`, and requires a cipher for any
compression. GtkHx never negotiates compression.

**ChaCha20-Poly1305** is a modern HOPE cipher specified in
[HOPE-ChaCha20-Poly1305.md](https://github.com/fogWraith/Hotline/blob/main/Docs/Protocol/HOPE-ChaCha20-Poly1305.md)
and implemented by Janus and GtkHx. Keys are derived with HKDF-SHA256 from K1 and K2
(`info` = `hope-chacha-encode` / `hope-chacha-decode`, named from the server's side);
records are `u32 length` + ciphertext + 16-byte tag, with a 12-byte nonce of a direction
byte and a 64-bit counter. It also encrypts file transfers, with a per-transfer key
derived from the reference number; the HTXF header itself stays in plaintext. There is
no rekey.

Server bugs worth knowing: Janus before 2.0.13 fails HOPE for any account with a
non-empty password, and its plain login compares the obfuscated password without
undoing the obfuscation, so only the empty password logs in. 2.0.13 fixes both.

### TLS on dedicated ports

Introduced by **Mobius** (November 2025, [docs/tls.md](https://github.com/jhalter/mobius/blob/master/docs/tls.md))
and adopted by Janus, Heidrun's server and GtkHx. The server listens on a second pair of
ports that carry the unchanged protocol inside TLS from the first byte: 5600 for
control and 5601 for transfers by convention (control + 100; transfers are always the
TLS control port + 1). There is no STARTTLS and no new transaction, so any server can be
put behind `stunnel`. Advertised through tracker v3 (`SUPPORTS_TLS`, `TLS_PORT`) and
Mobius's tracker registration. Most servers run on bare IP addresses without
CA-issued certificates, so clients are expected to pin on first use. GtkHx's trust model
is in [tls.md](tls.md). GtkHx does not run HOPE inside TLS.

### Capabilities

Specified in fogWraith's
[Capabilities.md](https://github.com/fogWraith/Hotline/blob/main/Docs/Protocol/Capabilities.md).
Field **0x01F0** is a big-endian bitmask of 1 to 8 bytes (2 is typical). The client
sends it in Login; the server echoes, in the Login reply, the subset it enables for the
session, and omits it if none. Unknown bits are ignored. Servers that don't know the
field ignore it, so sending it is always safe.

**Bit *n* is `1 << n`**: bit 0 is the least significant bit, the opposite of the access
bitmap.

| Bit | Mask | Name | Spec |
|---|---|---|---|
| 0 | 0x0001 | Large files | Capabilities-Large-File.md |
| 1 | 0x0002 | Text encoding (UTF-8) | Capabilities-Text-Encoding.md |
| 2 | 0x0004 | Voice | Capabilities-Voice.md |
| 3 | 0x0008 | Inline media | Capabilities-Inline-Media.md |
| 4 | 0x0010 | Chat history | Capabilities-Chat-History.md |
| 5 | 0x0020 | Extended privileges (128-bit access) | referenced, not yet written |
| 6 | 0x0040 | Messaging | Capabilities-Messaging.md |
| 7 | 0x0080 | Direct transfer | Capabilities-Messaging.md |
| 8 | 0x0100 | Messenger session (requires bit 6) | Capabilities-Messaging.md |
| 9 | 0x0200 | Modern dates | Capabilities.md |
| 10 | 0x0400 | Video (only together with bit 2) | hxd-ng `docs/capabilities-video.md` |

With **modern dates**, dates use the real year as the base instead of 1904, which
avoids the 2040 overflow. Nothing yet defines the layout of the 128-bit access map
(bit 5); a client that doesn't advertise it should expect 8 bytes.

For a value wider than 8 bytes, hxd-ng keeps the low 64 bits and hx-libs the first 8
bytes; senders should not exceed 8.

### Text encoding (UTF-8)

Capability bit 1,
[Capabilities-Text-Encoding.md](https://github.com/fogWraith/Hotline/blob/main/Docs/Protocol/Capabilities-Text-Encoding.md).
When echoed, every string field, including names embedded in the file-list and user
records, is UTF-8; otherwise it is Mac Roman, and characters with no Mac Roman form are
sent as `?`. Servers transcode between sessions, and use the client's version number to
guess the encoding of clients that don't negotiate. Mobius transcodes file names
([text-encoding.md](https://github.com/jhalter/mobius/blob/master/docs/text-encoding.md)).
A client can decode robustly without the bit by trying UTF-8 first and falling back to
Mac Roman. GtkHx's Mac Roman table matches glibc's `MACINTOSH` (0xC6 → U+0394, 0xF0 →
U+E01E), not Unicode's mapping file.

### Large files

Capability bit 0,
[Capabilities-Large-File.md](https://github.com/fogWraith/Hotline/blob/main/Docs/Protocol/Capabilities-Large-File.md).
Adds 64-bit companions to the 32-bit size fields: 0x01F1 file size, 0x01F2 offset,
0x01F3 transfer size, 0x01F4 folder item count, and 0x01FA resume digest (a 64-bit
window length and a SHA-256 over the tail of the partial file, replacing RFLT for large
uploads). The legacy fields are clamped to `0xFFFFFFFF` alongside.

The HTXF header's last word carries flags: bit 0 large-file mode (64-bit fork lengths:
the high half in the compression word, the low half in the size word), bit 1 an 8-byte
size follows the header, bit 2 a 40-byte resume digest follows, bit 3 a large upload
carries a full FILP. The spec defines bytes 12–15 as one 32-bit flags word; hx-libs and
GtkHx keep the transfer type in bytes 12–13 and the flags in 14–15, which only agree for
file downloads. A large upload without bit 3 sends the data fork raw, losing type,
creator and comment; GtkHx therefore uses large-file mode only when a file actually
exceeds 4 GB. The spec's example uses wrong hex IDs for the path, offset and queue
fields; the tables are right.

### Voice

Capability bit 2, access bit 55,
[Capabilities-Voice.md](https://github.com/fogWraith/Hotline/blob/main/Docs/Protocol/Capabilities-Voice.md).
A WebRTC selective forwarding unit on the server, with signaling in transactions on the
control connection: 600 join, 601 leave, 602 SDP offer (server to client), 603 SDP
answer, 604 ICE candidate (both ways), 605 room status, 606 mute. Fields 0x01F5 SDP,
0x01F6 ICE candidate (JSON; empty = end of candidates), 0x01F7 codec, 0x01F8 muted,
0x01F9 participants (6-byte entries: user ID, flags, codec ID; flags bit 0 is mute, and
the video extension's bits 1 and 2 say the user has a camera or a screen publication),
and 0x01FB transport
(plain RTP for clients without DTLS). The server is always the offerer and ICE-lite;
audio is PCMU. Implemented by Janus and hxd-ng. GtkHx's implementation and the server
bugs it works around are in [voice.md](voice.md).

### Video

Capability bit 10 (only with bit 2), access bits 59 (camera) and 60 (screen), specified
by hxd-ng's `docs/capabilities-video.md` in the shape of a fogWraith capability
document. Rides the voice room's peer connection: 607 start, 608 stop, 609 pause
state, 610 subscribe, 611 status (server to client), with 612–619 reserved; fields
0x0220–0x0225, with 0x0220–0x023F reserved. VP8 only. hxd-ng is the only server. See
[video.md](video.md).

### Inline media

Capability bit 3, access bit 57,
[Capabilities-Inline-Media.md](https://github.com/fogWraith/Hotline/blob/main/Docs/Protocol/Capabilities-Inline-Media.md).
Images in chat and private messages, carried as server-issued handles: 750 upload
(chunked, the server validates and re-encodes to JPEG, PNG or GIF) and 751 download, with
fields 0x0201–0x0212 (0x0201–0x021F reserved). The handle and type ride on chat and
message transactions; legacy clients receive the text alone. Limits are advertised in the
Login reply. Implemented by Janus and hxd-ng. See [inline-media.md](inline-media.md).

### Chat history

Capability bit 4, access bit 56 (falling back to 9, Read Chat),
[Capabilities-Chat-History.md](https://github.com/fogWraith/Hotline/blob/main/Docs/Protocol/Capabilities-Chat-History.md).
Transaction 700 fetches a page of server-stored chat by cursor (701–704 reserved).
Fields 0x0F01 channel, 0x0F02 before, 0x0F03 after, 0x0F04 limit, 0x0F05 entry (repeated;
a packed record of message ID, Unix timestamp, flags, icon, nick and text, with a TLV
trailer), 0x0F06 has-more, and 0x0F07 / 0x0F08 retention limits in the Login reply.
0x0F01–0x0F1F are reserved. Implemented by Janus and hxd-ng.

### Messaging

Capability bits 6, 7 and 8, access bit 58,
[Capabilities-Messaging.md](https://github.com/fogWraith/Hotline/blob/main/Docs/Protocol/Capabilities-Messaging.md).
A buddy-list instant-messaging layer: transactions 800–821 for roster, presence, message
delivery and acknowledgment, typing, file offers with relayed or direct transfer, and
call invitations; limit fields 0x0620–0x0622. Neither GtkHx nor hxd-ng implements it;
hxd-ng keeps its allocations reserved.

### GIF icons

**Avaraline** (avaraline.net), adopted by mhxd 0.4.10 in 2004; re-specified in
fogWraith's [GIF-Icons.md](https://github.com/fogWraith/Hotline/blob/main/Docs/Protocol/GIF-Icons.md).
Per-user GIF avatars alongside the 16-bit icon: 1861 get icon list, 1862 set icon, 1863
get icon, 1864 icon changed (server to client, carrying only the user ID). Fields 0x0300
(GIF bytes) and 0x0301 (list entry: user ID, length, GIF). Pull-based: the server never
pushes image bytes. No capability bit; discovered by probing. An older field, 0x0E90,
carried a Mac `cicn` icon and is now ignored everywhere. See [gif-icons.md](gif-icons.md).

Avaraline also defined transactions 2048–2051 (link login, join, leave, packet), 2149
(unformatted news), 2160 (unformatted user info), 2304 (modify own account) and 2305
(permission list), and search, offset and limit fields; see hlwiki's
[Unofficial Protocol Extensions](https://hlwiki.com/index.php/Unofficial_Protocol_Extensions).
Nothing current implements them.

### Colored nicknames

[Colored-Nicknames.md](https://github.com/fogWraith/Hotline/blob/main/Docs/Protocol/Colored-Nicknames.md).
Field **0x0500**, a 32-bit `0x00RRGGBB`, with `0xFFFFFFFF` or absence meaning "client
default". Carried in 301, 117, 354, 304 and as a trailer on the user record. No
capability bit: the server marks a session color-aware the first time that session sends
a color, so a client should send one only when the user has chosen a color. Not the same
as field 112 (user flags), which mhxd confusingly names "color" too. Janus implements it
and reserves red for administrators.

### Smaller and historical extensions

| Origin | Numbers | What |
|---|---|---|
| mhxd | transaction 3808, fields 0x0E80–0x0E82 | File hash (MD5, HAVAL, SHA-1). Experimental, HOPE builds only. |
| mhxd | field 0x0EA1 | Toggle away from chat. |
| mhxd | field 0x0E67 | "SID". |
| mhxd | IRC on the Hotline port | mhxd answers `NICK`/`USER` on the control port as an IRC server. |
| phxd | field 1024 (0x0400) | Old IRC nickname, for an IRC bridge. |
| GLoarbLine | transactions 123–131, 412; fields 117–124; privilege bits in 40–54 | Icon change, nickname change, "fake red", away, visibility, admin inspector, block download, standard message; edit news article. |
| Mobius | [feed-backed-news.md](https://github.com/jhalter/mobius/blob/master/docs/feed-backed-news.md) | RSS/Atom feeds as news categories. Server-side; no wire change. |

The emoji shortcodes GtkHx sends to non-UTF-8 servers are a client convention, not a wire
extension ([emoji-shortcodes.md](emoji-shortcodes.md)).

---

## ID allocations

Numbers in use, so that a new extension doesn't collide. Tracker v3 TLV IDs are a
separate namespace (several coincide numerically with field IDs here).

**Transactions:**

| Range | Owner |
|---|---|
| 100–122 | official |
| 123–131 | GLoarbLine |
| 200–214 | official |
| 300–304, 348–355 | official |
| 370–382, 400, 410, 411 | official |
| 412 | GLoarbLine |
| 500 | official (1.8.5) |
| 600–606 | voice |
| 607–611 (612–619 reserved) | video |
| 700 (701–704 reserved) | chat history |
| 750–751 | inline media |
| 800–821 | messaging |
| 1861–1864 | GIF icons (Avaraline) |
| 2048–2051, 2149, 2160, 2304, 2305 | Avaraline |
| 3808 | mhxd file hash |

**Fields:**

| Range | Owner |
|---|---|
| 100–116, 150–154, 160–162, 200–215, 220, 300, 319–337 | official |
| 117–124 | GLoarbLine |
| 0x01F0 | capabilities |
| 0x01F1–0x01F4, 0x01FA | large files |
| 0x01F5–0x01F9, 0x01FB | voice |
| 0x0201–0x021F | inline media |
| 0x0220–0x023F | video |
| 0x0300–0x0301 | GIF icons |
| 0x0400 | phxd IRC nickname |
| 0x0500 | colored nicknames |
| 0x0620–0x0622 | messaging |
| 0x0E01–0x0E04, 0x0EC1–0x0ECA | HOPE |
| 0x0E67, 0x0E80–0x0E82, 0x0EA1 | mhxd |
| 0x0E90 | legacy cicn icon |
| 0x0F01–0x0F1F | chat history |

**Access bits:** 0–40 official (12, 13, 18, 19 unenforced); 40–54 GLoarbLine (overlapping
the official 40); 41–54 otherwise unassigned; 55 voice; 56 chat history; 57 send media;
58 messaging; 59 camera; 60 screen share; 61–63 unassigned.

**Capability bits:** 0–10 as in [Capabilities](#capabilities); 11 and up unassigned.

---

## Server behavior worth knowing

Beyond the bugs each section notes, these are the differences that most often look like
a client bug:

| Server | Behavior |
|---|---|
| Official 1.9 | Replies with type 0. Disconnects on anything but Login/Agreed before login. Flood bans (see [Flood protection](#flood-protection)). Deletes move to the Trash. |
| Official, Windows builds | Two-fork FILP, no comments (they are silently dropped), type and creator derived from the extension (`BINA`/`dosa` for unknown ones), malformed upload-resume RFLT, `.hpf` partial files, local-time dates with the current year as base. |
| hlserver.com | Behaves as 1.0/1.2: no version field, errors on unknown transactions and on keepalive, drops idle connections. |
| Badmoon | A confirmed 1.9 server (190), serving flat and threaded news. |
| mhxd | Configurable version (0 = act as 1.2.3). Fake "everything" access in 354. Rename and move replace existing items; `/` in names misbehaves; transfer references leak ([mhxd-bugs.md](mhxd-bugs.md)). Kill Download is defined but never dispatched. |
| Mobius | Panics on Agreed without field 113. Reads 326 as a post's parent and ignores 334. |
| Janus | Silent on unknown transactions. Plain logins with a password fail. Components of 253+ bytes panic ([janus-bugs.md](janus-bugs.md)). Delivers the user ID in the Login reply. |
| Heidrun (older) | Echoed the request type in reply headers. |
| Old Mac servers (RetroMac, MacDomain) | Push User Access, Agreement and Banner before the Login reply. |

---

## Reference implementations

For testing a client against something closer to the original than mhxd or Janus:

| Target | Source | How it runs |
|---|---|---|
| **Hotline Server 1.9.1, 1.8.4, 1.8.2** (Windows, official binaries) | Tucows mirror on archive.org (1.9.1); Wayback copies of bigredh.com (1.8.x) | Wine + Xvfb. The licenses permit redistributing the unmodified installers; a build should unpack the installer rather than ship the extracted program. |
| **Openline 1.9 via MinGW** | [scottanderson/openline](https://github.com/scottanderson/openline), GPL | Cross-builds `hlserver.exe` in a Debian container in minutes; runs under Wine + Xvfb. Patchable, and behaves like the official 1.9.1 build on everything checked so far. |
| **Underline 1.9.6** | [199x.online/underline](https://199x.online/underline/), GPL; the published source includes the Linux platform layer except its file-system backend | A native Linux x86-64 (and arm64, i386, PowerPC) build that needs an X display, so it runs under Xvfb. Its Linux port sends folder type codes byte-swapped (`rdlf`) and looks accounts up case-sensitively (the shipped `Users/Guest` folder doesn't match the `guest` login). The Windows build is correct on both counts. |
| **Mac servers** (1.2.3, 1.7.2, 1.8.4, 1.9.1 Classic and Carbon) | archive.org, Wayback, Macintosh Garden | Would need SheepShaver or Basilisk II; not tried. The only route to the Mac behavior: three-fork FILP, comments, real type codes. |

None of these servers can be configured without the GUI, but all of them can be
configured **before** start by writing their files:

- `Users/<login>/UserData` holds each account, a 734-byte packed record: `u16 version =
  1`, `i16 icon`, 8 bytes of access, `u16 max downloads`, 512 reserved bytes, then name
  (64), alias (64), login (32) and password (32), each preceded by `u16 script` and
  `u16 length`. The password is stored obfuscated, exactly as it travels on the wire.
  Without an admin account with a password, the server stops at a modal dialog.
- `Prefs` holds the port, server name, trackers and options (version 5 for 1.9, version 4
  for 1.8.x, which stores no tracker logins). Leave the "use tracker" option off in
  tests, or the server registers with hltracker.com.

The 1.9 server ignores `SIGTERM`; stop it with `SIGKILL`. Its flood ban means a test
suite that opens more than ten connections in thirty seconds from one address has to be
paced, run against a fresh server, or run against a build with the check compiled out.
