# Inline Media Plan (fogWraith Capability Extension)

Scoping doc for GtkHx's implementation of the **Inline Media** capability
extension to the Hotline protocol. Spec lives at
`fogWraith/Hotline/Docs/Protocol/Capabilities-Inline-Media.md`.

Capability bit 3 (`0x0008`) is already reserved as `HTLC_CAP_INLINE_MEDIA`
in `src/hotline.h:329` from the original capability-bit allocation pass.

## Status (June 2026)

Phases A, B, C, D, and F have shipped on `main`. The client is
spec-conformant: it negotiates the capability, decodes server-canonical
bytes through a bounded loader, lets the user attach an image via a
paperclip button in chat input bars, renders a styled placeholder for
inbound media, and surfaces a click-to-view dialog with Save As / Open
Externally. Tier 3 covers seven end-to-end paths against Janus.

Phase E (true in-stream rendering by extending xtext) is **deferred** —
shipping Option 1 first burns the wire / decoder stack in against real
servers before committing to the xtext surgery.

The shipped scope deliberately omits some entries in the original send-
path plan: **paste-from-clipboard and drag-and-drop** are not wired up
yet (paperclip-only for v1), and **chunked upload is not implemented**
(single-shot only, payload capped at u16 — ~63 KB after framing
overhead). Both can be added without protocol-layer changes; see the
"Remaining work" subsection under each phase for details.

## What the extension does

A capable sender uploads image bytes to the server via a dedicated
opcode-style transaction (`TranUploadMedia` = 750), receives an opaque
media handle, and references that handle from a normal
`TranChatSend` / `TranSendInstantMsg`. Capable recipients see the
handle on the inbound chat, fire `TranDownloadMedia` (= 751) to fetch
the canonical bytes, decode, and render inline.

The server validates, canonicalises (re-encodes), and strips metadata
from every upload before issuing a handle. It enforces a fixed
authorization set for downloads, captured at chat-relay time.
Legacy clients see plain text with media fields stripped; the optional
"media gateway" fallback for public chat is server-side only and never
applies to private contexts.

Image bytes are never carried inside a chat transaction. JPEG, PNG, and
GIF only; SVG / WebP / AVIF / HEIC are explicitly forbidden under this
capability bit.

## Scope of work on the client

Three roughly-independent layers:

1. **Wire protocol** — capability negotiation, advisory-limits parsing
   from the LOGIN reply, two new transactions, ~12 new field IDs, an
   error-code enum, and a chunked-upload state machine. Fits the
   established Rust `hotline-proto` pattern (mirrors `voice.rs`,
   `chat_history.rs`, `tracker_v3.rs`).
2. **Decode hardening** — every byte the server hands us still has to
   pass our own bounded decode before going to a renderer. The
   server's re-encode is a defence in depth, not a guarantee.
3. **UI** — send path (paperclip / paste / drag-drop), receive path
   (placeholder + click-to-view in v1, true inline in v2), context
   menu (save-as, copy, open in external viewer).

The wire half is straightforward. Almost all of the design risk is in
the "render image inside xtext" question; see [§ xtext embedding](#xtext-embedding-feasibility)
below.

## Phased plan

### Phase A — Wire protocol foundation [SHIPPED]

- `hotline-proto`: new `inline_media.rs` module shipped.
  - `LimitsAdvertisement` struct parsed out of LOGIN reply's optional
    `0x020C`–`0x0211` fields. All fields tolerated absent;
    spec-default fallbacks live in `inline_media_limits.h` /
    `inline_media.c::inline_media_max_bytes`.
  - `MediaErrorCode` enum (`Generic = 0`, `PayloadTooLarge = 1`,
    `UnsupportedFormat = 2`, `RateLimited = 3`, `NotAuthorized = 4`,
    `ServerBusy = 5`). Unknown codes map to `Generic`.
  - Typed builders / parsers for `TranUploadMedia` (single-shot
    builder; **chunked variant fixtures landed in the crate but the
    chunked state machine is not wired through the C path yet**) and
    `TranDownloadMedia` (request + reply with `PartIndex` /
    `PartCount` / `PartFinal`, including chunked-reply accumulator on
    the receive side).
- Tier 2 wire fixtures: golden-bytes for LOGIN-with-limits reply,
  single-shot upload request + handle reply, multi-chunk upload
  sequence with token echo, download request + chunked reply, error
  reply with code, chat-media-meta + orphan handling.
- C-side dispatcher: opcode 750 / 751 receivers registered in `rcv.c`
  alongside the existing voice / chat-history capability flags. The
  capability bit is echoed into `htlc->caps` from the LOGIN reply.
- Send UI is gated on `HTLC_CAP_INLINE_MEDIA` (see Phase C).

### Phase B — Bounded decoder [SHIPPED]

- Module: `src/inline_media_decode.{c,h}`. Single entry point
  `inline_media_decode(buf, len, caps)` returns a `HxInlineMediaDecoded`
  struct carrying either a `GdkTexture` or an error message. Cap
  defaults are pulled from the `HxInlineMediaCaps` struct, with the
  spec recommended values applied when fields are 0.
- Magic-byte sniff first via `inline_media_sniff`. The JPEG / PNG / GIF
  allowlist is enforced regardless of the server's
  `DATA_CHAT_MEDIA_TYPE`; `inline_media_format_is_allowed` is the
  gate. SVG / WebP / AVIF / HEIC are rejected at sniff time.
- Bounded decode via `GdkPixbufLoader` with a `size-prepared` callback
  that aborts when announced dimensions exceed the cap. The decoded
  `GdkPixbuf` is converted to a `GdkTexture` via
  `gdk_texture_new_for_pixbuf`.
- The decoder runs synchronously on the main thread for v1 — the spec
  cap (256 KiB encoded, 2048×2048) means decode is fast enough not to
  warrant the worker-thread plumbing yet. The worker-thread plan from
  the original scoping doc is held in reserve if profiling shows it's
  needed.
- **Animated GIF is deferred to v2** — v1 decodes the first frame and
  renders as a still, per the original plan.

### Phase C — Send path [SHIPPED, partial]

Shipped:

- **Paperclip button** in chat input bars (chat, private chat, PM)
  via `src/inline_media_attach.{c,h}`. Visibility is gated on
  `HTLC_CAP_INLINE_MEDIA` — the button is hidden on servers that don't
  advertise the cap, since showing inert chrome would be misleading
  given most Hotline servers don't support the extension.
- File picker via `GtkFileDialog` with a mime-type filter for
  `image/png` / `image/jpeg` / `image/gif`.
- Two-stage pre-flight: `g_file_query_info` rejects oversized files
  before any bytes are loaded; after `g_file_load_bytes_async` lands,
  a magic-byte sniff + size check runs against the effective per-
  upload cap (the smaller of the server's `DATA_CHAT_MEDIA_MAX_BYTES`
  and the 16-bit single-shot wire-framing ceiling).
- Upload state machine in `src/inline_media_upload.{c,h}`. Single-shot
  framing only — chunked is not wired through yet. The per-upload
  context lifecycle is tied to the task table via the new
  `task->ptr_free` hook (see the round-2 review commit on
  `claude/inline-media-phase-c-ui`), so a disconnect mid-upload
  reclaims both the upload-helper context and the caller's per-upload
  context (the attach ctx) via `g_hash_table_remove_all` rather than
  leaking.
- Cap re-check at both async boundaries (`on_file_picked` and
  `on_bytes_loaded`) so a disconnect / reconnect-to-non-capable-server
  race surfaces the actionable "Inline media isn't available on this
  server" toast instead of the generic "couldn't start upload."
- Chat send with media attached via `hx_send_chat_with_media`, which
  threads `DATA_CHAT_MEDIA_ID` + `DATA_CHAT_MEDIA_TYPE` companions
  alongside the usual chat fields. The text body defaults to
  `[image]` when empty.
- On upload error: toast surfaces a per-`MediaErrorCode` string
  (`Image too large for this server`, `Server rejected the image
  format`, `Rate limited — try again shortly`, …).

Remaining work (post-Phase-C polish, not blocking spec conformance):

- **Paste-from-clipboard** (`Ctrl+V` of a `GdkTexture`) — not wired.
- **Drag-and-drop** onto input box via `GtkDropTarget` — not wired.
- **Compose preview** (thumbnail + remove affordance before send) —
  not built. v1 fires the upload immediately on file pick.
- **Chunked upload** — `inline_media_upload.c` rejects payloads
  larger than 65 535 bytes since the single-shot builder can't frame
  more. Chunked framing exists at the wire level; needs an upload-
  side state machine that holds the token across replies. With the
  server-recommended 256 KiB cap, the gap matters for screenshots
  but not for typical chat attachments.
- **Resize-on-oversized** affordance instead of a hard reject —
  pending a UX call.

### Phase D — Receive path (Option 1: placeholder + dialog) [SHIPPED]

- Inbound chat / PM messages carrying `DATA_CHAT_MEDIA_ID` +
  `DATA_CHAT_MEDIA_TYPE` render an NBSP-joined placeholder row in
  xtext, styled with mIRC colour 14 (subdued grey) so it reads as a
  distinct token. The placeholder text mirrors the original spec:
  `[image · <type> · <dims> · <size> · click to view]` with hint
  fields elided when the server omits them.
- Per-chat `media_handles` `GHashTable<guint token id, handle bytes>`
  on each chat window maps the clickable token back to the media
  handle. Click on the placeholder fires the download dialog.
- **In-app dialog** in `src/inline_media_dialog.{c,h}`: an `AdwDialog`
  with a `GtkStack` body (Loading / Image / Error pages). Loading page
  shows an `AdwSpinner`. Image page is a `GtkPicture` inside a
  `GtkScrolledWindow`. Header bar carries **Save As…** and **Open
  Externally** buttons, both disabled until the download completes.
- Async download via `src/inline_media_download.{c,h}`:
  `inline_media_download_start` registers a task, the chunked-reply
  accumulator (`GByteArray`) collects payload across `TranDownloadMedia`
  replies, terminal chunk triggers decode and renders. The download
  context's lifetime is tied to the task table via `task->ptr_free`,
  same shape as the upload helper.
- Save As writes via `GtkFileDialog` + `g_file_replace_contents`. The
  callback context holds its own `GBytes` ref so closing the parent
  dialog mid-pick can't UAF the save handler.
- Open Externally writes to a unique temp file under
  `XDG_RUNTIME_DIR` (fallback `/tmp`) using `g_file_create` with
  `G_FILE_CREATE_PRIVATE` (O_CREAT|O_EXCL, 0600) and a 64-bit random
  tail name; `GtkFileLauncher` then asks the portal / desktop to
  open it.
- Spec `MediaErrorCode` is mapped to user-readable strings on the
  error page (`Image too large`, `Unsupported image format`, …).
- **Copy Image** is not shipped in v1 — the dialog focuses on Save /
  Open. Adding clipboard copy is a follow-up.

End of Phase D, the client is spec-conformant: it sends, receives,
decodes, and lets the user view media. No xtext surgery yet.

### Phase E — Receive path (Option 2: multi-subline padding inline) [DEFERRED]

This phase replaces the placeholder with true in-stream rendering. It
is the part that touches xtext, and it is gated on Phase D being
shipped and used long enough to validate the wire / decoder stack.
Phase D is shipped; Phase E is intentionally deferred until there's
data on whether the placeholder-and-dialog UX is enough.

- Extend `textentry` with a discriminator (`tag` field is already
  there; use a new value) and a `GdkTexture *` ref for media entries.
- New API: `gtk_xtext_append_media (buf, texture, alt_text, stamp)` —
  appends a media entry. Height is `ceil(texture_height / fontsize)`
  blank text rows, the texture is painted into the bounding box during
  render.
- `gtk_xtext_render_line` branches on the entry kind: text path
  unchanged, media path is single-shot `cairo_set_source_surface` (or
  equivalent `gdk_texture` paint) at the entry's reserved band, with
  clipping to `clip_y` / `clip_y2`.
- `gtk_xtext_lines_taken` returns the padded row count for media
  entries; the rest of `calc_lines` / `nth` / scroll math sees a
  normal multi-subline entry.
- Selection over a media entry: all-or-nothing, with the alt text
  going to the clipboard on copy (not whitespace from the blank
  sublines).
- Right-click on a media row surfaces the same Save As / Copy /
  Open-External context menu Phase D introduced for the placeholder.
- Click semantics: still opens the in-app dialog at full size, since
  the rendered inline copy is sized down to fit the chat width.

The known compromises (selection-drag passes through blank rows;
marker draw lands on blanks) are documented as the cost of staying in
the line-uniform grid model. If they prove too rough, Option 4
(variable-height xtext) becomes the natural follow-up against a
codebase that already has the data model.

### Phase F — Tier 3 integration against Janus [SHIPPED]

Janus support was confirmed before any other phase: the Tier 3
container echoes capability bit 3 in the LOGIN reply and advertises
the `DATA_CHAT_MEDIA_MAX_*` limits. No mock server needed.

Shipped Tier 3 binary: `tests/integration/test_inline_media.c`,
gated on the Janus matrix row via `HX_TEST_CAP_INLINE_MEDIA`.

Seven end-to-end paths cover:

- `cap_negotiation` — capability bit echoed in LOGIN reply.
- `limits_advertised` — `DATA_CHAT_MEDIA_MAX_*` fields parse out of
  the LOGIN reply.
- `upload_round_trip` — `TranUploadMedia` with a runtime-encoded 4×4
  PNG (`gdk_pixbuf_save_to_buffer`); assert the final reply carries
  a non-empty handle + canonical MIME and the announced dimensions.
- `chat_with_media_round_trip` — chain upload → `hx_send_chat_with_media`
  → drain to the broadcast relay, assert
  `gtkhx_proto_extract_chat_media_meta` finds the handle on the
  inbound chat.
- `download_round_trip` — upload a PNG, fetch via
  `TranDownloadMedia`, assert reply carries `CHAT_MEDIA_PAYLOAD` with
  a valid PNG signature.
- `oversized_rejected` — server returns `MediaErrorCode = 1` on
  oversized upload.
- `unauthorized_download` — capability-correct request for a
  non-existent handle surfaces as a task-error with the spec
  `NotAuthorized` mapping.

Per-account rate-limit interval in the test container's
`tests/janus/conf/config.yaml` is dropped to `1ms` so the suite's
back-to-back uploads through the shared `guest` account don't
collide with the spec-default 10 s interval. **Caveat**: Janus's Go
YAML decoder treats `0s` as "unset / use default" and ignores it;
keep the value explicit and non-zero.

Capability-not-confirmed paths (mhxd / Mobius strip media fields)
and the mixed-cap chat-relay case are not exercised in Tier 3 yet.
Adding them is bookmarked under [[gtkhx_multi_server_test_plan]].

Tracking [[gtkhx_janus]] for the broader Janus matrix.

## xtext embedding feasibility

Restating the question because it's the load-bearing design risk:
**can the xtext widget render images inline at all?**

Reading `gtk_xtext_render_line`, `gtk_xtext_calc_lines`,
`gtk_xtext_lines_taken`, and `gtk_xtext_nth`:

- Every textentry's vertical extent is `fontsize × len(ent->sublines)`.
- The scroll adjustment unit is one text line.
- y↔line maps (clicks, motion, selection, marker draw) all do
  `y / fontsize` and `fontsize × line` arithmetic.
- HexChat's upstream xtext has never carried inline images; no patch
  to borrow.

Four approaches, in increasing surgery:

1. **Placeholder only.** A media message renders as a single styled
   row, click opens externally / in dialog. Zero xtext changes. Spec-
   conformant: the spec mandates decoding + "reasonable display," not
   inline rendering. **This is Phase D.**
2. **Multi-subline padding.** Reserve `ceil(img_h / fontsize)` blank
   sublines, paint the texture into that band during render. Reuses
   all existing scroll/click/calc math. Costs: selection drag passes
   through blank rows, marker draw on blanks. ~200–400 LOC focused on
   `render_line` + a new `gtk_xtext_append_media`. **This is Phase E.**
3. **GtkOverlay with positioned GtkPicture children.** Pixel-perfect
   imagery, but the placement gymnastics under wrap/resize/clip get
   nasty. Not pursuing.
4. **Variable-height xtext.** Add `pixel_height` to textentry, rebuild
   scroll math in pixels. Touches every y↔line site. ~600–1000 LOC of
   xtext surgery. The right long-term answer; deferred until Phase E
   has data on whether the line-grid compromises are acceptable.

Shipping Option 1 first lets the wire / decoder stack burn in against
real servers (when they exist) before any xtext surgery. If Option 2
turns out cramped (selection ergonomics, animated GIF), Option 4
becomes the natural follow-up against a codebase that already has the
data model and decoder pipeline in place.

## Open questions

- **Pre-fetch policy.** Phase D fetches on click. Should there be a
  "auto-load images" preference toggle that pre-fetches inline as
  chat arrives? Easy to add once the on-click path works; the bigger
  question is what the default is. Most modern chat clients
  pre-fetch; some let the user gate it on per-room or per-DM.
- **Server-recommended chunk size.** The spec carries
  `DATA_CHAT_MEDIA_CHUNK_SIZE` but doesn't bound it. We should clamp
  what we honour from the server to a sane ceiling (say 60 KB) to
  avoid a hostile server requesting absurd allocations.
- **Limits-not-advertised fallback.** Spec defaults are recommended,
  not required. If the LOGIN reply confirms the capability bit but
  omits some / all of the `MAX_*` fields, we use the spec's
  recommended defaults — same as the spec instructs clients to do.
  Worth a debug log when we do so, to spot servers that aren't
  advertising correctly.
- **Cache lifetime on the client.** The server expires handles after
  24 h. The client also caches decoded `GdkTexture`s; how big a cache?
  Probably tie to chat-window lifetime (texture refs held while the
  chat-output buffer holds the textentry, dropped on
  `gtk_xtext_clear` / window destroy). No on-disk cache.

## Status by phase

| Phase | Scope | Status |
|---|---|---|
| A | Wire protocol + capability echo + fixtures | Shipped |
| B | Bounded decoder | Shipped |
| C | Send UI (paperclip + file picker + single-shot upload) | Shipped; paste / drag-drop / chunked / compose preview pending |
| D | Receive: placeholder + click-to-view dialog | Shipped; Copy Image pending |
| E | Inline render via multi-subline padding | Deferred |
| F | Tier 3 integration against Janus | Shipped |

Phases A → D + F bring spec conformance. Phase E is the embedding
payoff; deferred pending feedback on whether the placeholder UX is
enough.

## Future work (v2 and beyond)

- **Animated GIF rendering.** v1 renders the first frame; v2 wires a
  `GdkPixbufAnimation` (or successor) into the xtext media-entry paint
  path with a per-row redraw timer.
- **Variable-height xtext.** If Phase E's line-grid compromises chafe,
  this is the right answer. Probably also unlocks better long-message
  layout independent of media.
- **Image cache across sessions.** A small content-addressed on-disk
  cache keyed by `DATA_CHAT_MEDIA_ID` + server-host could survive
  reconnects. Bounded size, LRU eviction.
- **WebP / AVIF / HEIC.** Defer to a future capability bit, per the
  spec. Decoder hardening varies widely; the format allowlist is the
  cheapest defence we have today.

## Decisions locked in

- **Animated GIFs are v2.** v1 decodes the first frame and renders as
  a still. Shipped that way in Phase B.
- **In-app dialog for click-to-view** via `AdwDialog` +
  `GtkPicture` + `GtkStack` (Loading / Image / Error), shipped in
  Phase D. Did not end up reusing the `src/preview.{c,h}` plumbing —
  the click-to-view dialog is its own widget shape and the streaming
  loader pattern was unnecessary for the spec-bounded payload size.
  External viewer is still available via the **Open Externally**
  button.
- **Save As + Open Externally** are buttons on the dialog header bar.
  Right-click context menu on the placeholder is not wired; **Copy
  Image** to the clipboard is the main remaining item from the
  original right-click menu spec.
- **JPEG / PNG / GIF only** at the decoder. Spec-mandated; SVG / WebP
  / AVIF / HEIC explicitly rejected at sniff time.
- **Send-button visibility is cap-gated.** The paperclip is hidden
  unless `HTLC_CAP_INLINE_MEDIA` is negotiated. Most Hotline servers
  don't speak the extension; an inert button would be misleading.
- **Per-task heap context is reclaimed by the task table.** Added
  during the Phase 9.C round-2 review: `struct task` now carries an
  optional `GDestroyNotify ptr_free` that fires when the entry is
  removed (including the `g_hash_table_remove_all` sweep
  hx_htlc_close runs on disconnect). The inline-media upload and
  download helpers opt in; this closes a leak where in-flight
  contexts survived the connection that owned them.
