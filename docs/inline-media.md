# Inline Media

GtkHx's implementation of the fogWraith **Inline Media** capability
extension to the Hotline protocol (`Capabilities-Inline-Media.md`). The
spec is not vendored in this tree, so the wire details below are the
in-repo record.

## What the extension does

A capable sender uploads image bytes to the server with a dedicated
transaction, receives an **opaque media handle**, and references that
handle from an ordinary chat / instant-message send. Capable recipients
see the handle on the inbound chat, fire a download transaction to fetch
the canonical bytes, decode, and render inline.

**Image bytes never travel inside a chat transaction** — only handles
do. The server validates and canonicalises (re-encodes) every upload
before issuing a handle, keeping a canonical-bytes store, and enforces a
fixed authorization set for downloads that is **captured at chat-relay
time**. Legacy clients see plain text with the media fields stripped;
the optional "media gateway" fallback — which forwards the image to an
external HTTPS endpoint so non-capable clients see a plain URL — is
server-side, opt-in, and applies to public chat only, never to private
contexts.

JPEG, PNG, and GIF only. SVG / WebP / AVIF / HEIC are explicitly
forbidden under this capability bit.

## Wire protocol

Capability bit 3 (`0x0008`) — `HTLC_CAP_INLINE_MEDIA` in
`DATA_CAPABILITIES`. The server echoes it in the LOGIN reply when it
speaks the extension.

Transactions:

| Dec | Hex | Name |
|---|---|---|
| 750 | `0x02EE` | `TranUploadMedia` (`HTLC_HDR_UPLOAD_MEDIA`) |
| 751 | `0x02EF` | `TranDownloadMedia` (`HTLC_HDR_DOWNLOAD_MEDIA`) |

Field IDs `0x0201`–`0x021F` are reserved for the extension.

| Hex | Field | Notes |
|---|---|---|
| `0x0201` | `CHAT_MEDIA_TYPE` | Canonical MIME. Companion on chat transactions. |
| `0x0202` | `CHAT_MEDIA_ID` | Opaque handle. Companion on chat transactions. |
| `0x0203` | `CHAT_MEDIA_PAYLOAD` | Image bytes. Only in 750 / 751. |
| `0x0204` | `CHAT_MEDIA_DECLARED_TYPE` | Optional client MIME hint on upload. |
| `0x0205` / `0x0206` / `0x0207` | `WIDTH` / `HEIGHT` / `BYTES` | Server-supplied canonical metadata, u32 BE. |
| `0x0208` | `UPLOAD_TOKEN` | Issued with the first chunk's reply, echoed on every follow-up. |
| `0x0209` | `PART_INDEX` | u16 BE, zero-based. |
| `0x020a` | `PART_COUNT` | u16 BE, total chunk count. |
| `0x020b` | `PART_FINAL` | u8, non-zero on the last chunk. |
| `0x020c`…`0x0211` | advisory limits | See below. All u32 BE, LOGIN reply. |
| `0x0212` | `CHAT_MEDIA_ERROR_CODE` | u16 BE. Optional on error replies. |

`CHAT_MEDIA_ID` and `CHAT_MEDIA_TYPE` appear together on a chat
transaction or not at all; the server overwrites `TYPE` with the
canonical MIME after re-encoding, before relay.

### Advisory limits (LOGIN reply)

`0x020c` `MAX_BYTES`, `0x020d` `MAX_DIMENSION`, `0x020e` `MAX_PIXELS`,
`0x020f` `CHUNK_SIZE`, `0x0210` `MAX_FRAMES`, `0x0211` `MAX_DURATION_MS`.

Every field is independently optional — clients must tolerate any of
them being absent and fall back to the recommended defaults, which is
what the accessors in `src/inline_media.h` do (a zero value means
"absent", since zero is not a meaningful cap in any of these units).
The limits are wiped both at disconnect and before walking each LOGIN
reply, so a reconnect to a server that doesn't advertise the cap cannot
inherit the previous session's values.

Defaults: 256 KiB encoded, 2048 px per axis, 2048×2048 total pixels,
150 animation frames, 15 s of cumulative animation.

**Chunk size is both a fallback and a ceiling.** The spec doesn't bound
`CHUNK_SIZE`, so the accessor clamps whatever the server advertises down
to 60000 bytes — the same value used when the field is absent. 60000
leaves room inside the 65535-byte wire frame for the chunk header plus
the `PART_INDEX` / `PART_FINAL` / `UPLOAD_TOKEN` wrapper chunks. A
hostile server therefore can't drive us into absurd per-chunk
allocations. The upload token itself is capped at 1 KiB for the same
reason (the spec describes tokens as ≤ 64 bytes; the slack absorbs
future spec evolution).

Note the numeric overlap with the tracker-v3 TLV constants — `0x020C`
and `0x0211` also name TLVs there. Different namespace; the two sets
must not be deduplicated.

### Error codes

`MediaErrorCode`, carried in `0x0212`: `Generic = 0`,
`PayloadTooLarge = 1`, `UnsupportedFormat = 2`, `RateLimited = 3`,
`NotAuthorized = 4`, `ServerBusy = 5`. Unknown codes map to `Generic`.
The human-readable `DATA_ERROR` text stays authoritative for display;
the code drives the actionable toast wording.

## Decode security posture

Every byte the server hands back still passes our own bounded decode
before reaching a renderer. **The server's re-encode is defence in
depth, not a guarantee.**

- **Sniff before decode.** A magic-byte sniff runs first and inspects at
  most the leading bytes of the input. It recognises the blocked formats
  (SVG / WebP / AVIF / HEIC / TIFF / ICO / BMP) explicitly so a
  rejection log line can say *why*.
- **The allowlist is enforced regardless of the declared MIME.** A
  hostile or buggy server claiming a different canonical type does not
  get its bytes past the sniff gate under the strict policy.
- Dimension and pixel caps are checked from the parsed header *before*
  the full raster decode; frame count and cumulative duration are
  capped during frame collection.
- Decoding runs out-of-process in a sandbox — see
  [image decoding](image-decoding.md).

## Where it lives

- **Wire format** — `hotline-proto::inline_media` (typed builders and
  parsers for both transactions, the chunked-upload state shapes, the
  limits advertisement, and the error-code enum).
- **Decoder** — the `hx-image-decode` crate, behind the C ABI in
  `src/inline_media_decode.h`.
- **Send path** — `src/inline_media_attach.c` (paperclip button + file
  dialog + pre-flight) and `src/inline_media_upload.c` (single-shot and
  chunked dispatch). The paperclip is hidden unless the capability is
  negotiated for the live session; showing inert chrome would be
  misleading given how few servers speak the extension.
- **Receive path** — `src/inline_media_download.c` (chunked-reply
  accumulator), `src/chat.c` (placeholder row + auto-fetch + swap-in),
  and the click-to-view dialog in `gtkhx-ui/src/inline_media_dialog.rs`.
- **Chat rendering** — the `hx_chat_view_append_media` /
  `_media_mark` / `_media_set_texture` / `_media_set_animation` family
  declared in `src/chat_view.h`, implemented in the `hxchat-view` crate.

Per-upload and per-download heap contexts are owned by the task table
via a `GDestroyNotify` hook on the task, so a disconnect mid-transfer
reclaims them rather than leaking.

### Rendering

An inbound chat carrying the media companions appends a media row whose
alt text is a placeholder caption (type / dimensions / size, with fields
elided when the server omits them) styled in the subdued grey palette
slot, embedding a clickable `hxmedia:N` token that maps back to the
handle through the conversation's media table. The client immediately
fires the download; on success the row's texture is swapped in place and
the row resizes, with the scroll anchor absorbing the change so a decode
landing above the viewport does not shift what the user is reading. On
failure the placeholder stays and click-to-view still works.

Sizing is native when the image fits the column, otherwise scaled down by
width preserving aspect ratio. Never upscaled. **Animated GIF plays** —
frames are installed on the row and advanced by a shared frame-clock
tick.

The click-to-view dialog is an `AdwDialog` with Loading / Image / Error
pages and **Save As…** / **Open Externally** buttons on its header bar.
Open Externally stages the bytes into a `0600`, `O_EXCL`, randomly-named
temp file under `XDG_RUNTIME_DIR` and hands it to the portal.

## Server behaviour observed (Janus)

Which `MediaErrorCode` the server picks, from the integration suite:

- Chunked upload over `CHAT_MEDIA_MAX_BYTES` → `PayloadTooLarge` (1).
- Single-shot garbage payload → `UnsupportedFormat` (2); the magic-byte
  sniff trips before the size check.
- Single-shot SVG with declared MIME `image/svg+xml` → `UnsupportedFormat`
  (both that and `Generic` are spec-conforming).
- Download of a non-existent handle → `NotAuthorized` (4) or `Generic`.

**Live server bug worth knowing.** Janus's Go YAML decoder treats a
`0s` duration as *unset* and silently applies the default, so the
rate-limit interval in a test container's config must be an explicit,
non-zero value (the suite uses `1ms`) — writing `0s` to mean "no limit"
leaves the spec-default 10 s per-account interval in force, and
back-to-back uploads through a shared account then fail with a
rate-limit task error.

Integration coverage runs against Janus: capability echo, limits
parsing, single-shot and chunked upload round-trips, chat-with-media
relay, single-chunk and multi-chunk download, and each of the error
paths above. The mixed-capability relay case (a capable and a legacy
client in the same room) is not exercised.

## Not built

- **Paste from clipboard** and **drag-and-drop** onto the input box.
  Attachment is paperclip-only.
- **Compose preview** — a thumbnail with a remove affordance before
  send. The upload fires immediately on file pick.
- **Resize-on-oversized** instead of a hard reject.
- **Copy Image** from the click-to-view dialog, and a right-click
  context menu on the rendered row.
- **An auto-load preference.** Inbound media is always fetched when the
  capability is negotiated; there is no toggle.
- **A cross-session image cache.** Decoded textures live as long as the
  chat rows that reference them; nothing is written to disk. Server-side
  handles expire on their own schedule.
- **WebP / AVIF / HEIC**, which the spec defers to a future capability
  bit. The format allowlist is the cheapest hardening available today.
