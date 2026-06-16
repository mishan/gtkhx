# Inline Media Plan (fogWraith Capability Extension)

Scoping doc for GtkHx's implementation of the **Inline Media** capability
extension to the Hotline protocol. Spec lives at
`fogWraith/Hotline/Docs/Protocol/Capabilities-Inline-Media.md`.

Capability bit 3 (`0x0008`) is already reserved as `HTLC_CAP_INLINE_MEDIA`
in `src/hotline.h:329` from the original capability-bit allocation pass.

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

### Phase A — Wire protocol foundation

- `hotline-proto`: new `inline_media.rs` module.
  - `LimitsAdvertisement` struct parsed out of LOGIN reply's optional
    `0x020C`–`0x0211` fields. All fields tolerated absent;
    spec-default fallbacks recorded in code.
  - `MediaErrorCode` enum (`Generic = 0`, `PayloadTooLarge = 1`,
    `UnsupportedFormat = 2`, `RateLimited = 3`, `NotAuthorized = 4`,
    `ServerBusy = 5`). Unknown codes map to `Generic`.
  - Typed builders/parsers for `TranUploadMedia` (single-shot + chunked
    variants) and `TranDownloadMedia` (request + reply with
    `PartIndex` / `PartCount` / `PartFinal`).
- Tier 2 wire fixtures: golden-bytes for the LOGIN-with-limits reply,
  single-shot upload request + handle reply, multi-chunk upload
  sequence with token echo, download request + chunked reply, error
  reply with code.
- C-side dispatcher: register opcode 750 / 751 receivers in `rcv.c`.
  Capability echo handling alongside the existing voice / chat-history
  capability flags.
- Suppress the v1 send UI when the server does not echo bit 3.
- **No upload or receive flow yet.** This phase ends on parsers,
  builders, and fixtures only — nothing user-visible.

### Phase B — Bounded decoder

- New module: `src/inline_media_decode.{c,h}` (or `media_decode` — name
  TBD). Single entry point that takes a byte buffer and the canonical
  MIME type and returns either a `GdkTexture` (still images) or an
  error code matching the spec's `MediaErrorCode`.
- Magic-byte sniff first. Reject anything not in the JPEG/PNG/GIF
  allowlist regardless of what the server's `DATA_CHAT_MEDIA_TYPE`
  said. We **must not** trust the canonical MIME alone — the spec
  itself flags this on the client side.
- Bounded decode via `GdkPixbufLoader` (or `gdk_texture_new_from_bytes`
  with a pre-flight `gdk_pixbuf_loader_set_size` check). Reject if the
  decoded dimensions exceed our local caps (mirror the spec defaults:
  2048×2048, ~4.2 MP). Hard reject SVG/WebP/AVIF/HEIC at sniff time
  even if a future server were to canonicalise to them.
- Runs on a worker thread; main thread receives the `GdkTexture` (or
  the error). Worker→main marshal pattern is identical to the
  existing `hx_preview` pipeline (`src/preview.{c,h}`) — that path
  already streams chunks via `hx_preview_chunk` into a
  `GdkPixbufLoader` and surfaces a `GtkPicture` on `hx_preview_done`.
  Reuse the loader + thread shape, not the preview widget itself
  (preview is a stand-alone window; inline media renders inside the
  chat view).
- **Animated GIF is deferred to v2.** v1 decodes the first frame of a
  GIF and renders it as a still. We still accept the upload because
  the server might reject any GIF re-encoded to PNG, and we want to
  interop with senders / other capable clients.

### Phase C — Send path

- "Attach image" entry points in chat input bars (chat, private chat,
  PM):
  - Toolbar paperclip button next to send.
  - Paste-from-clipboard (`Ctrl+V` of a `GdkTexture` in the clipboard).
  - Drag-and-drop a single image file onto the input box (`GtkDropTarget`
    accepting `GdkFileList` / `GdkTexture`).
- Pre-flight against the server's advertised
  `DATA_CHAT_MEDIA_MAX_BYTES` / `_MAX_DIMENSION` / `_MAX_PIXELS`;
  offer to resize/recompress if oversized rather than round-tripping a
  known-bad upload.
- Compose preview: thumbnail + filename + "remove attachment" affordance
  next to the send button. Compose state is per-chat-input.
- Upload pipeline: send `TranUploadMedia` with single-shot framing when
  bytes fit (< ~63 KB after framing overhead), otherwise chunk using the
  server-advertised `DATA_CHAT_MEDIA_CHUNK_SIZE`. Echo the
  `DATA_CHAT_MEDIA_UPLOAD_TOKEN` from the first reply on every
  subsequent chunk. Cancellable mid-upload (drop the token; server
  reaps via the 30 s idle timeout).
- On upload success: attach `DATA_CHAT_MEDIA_ID` + `DATA_CHAT_MEDIA_TYPE`
  to the chat send. The user's typed text rides in `DATA_DATA` as
  normal (default to `[image]` if empty — server-side gateway
  fallback only applies to public chat with legacy recipients, but the
  text field still has to be present).
- On upload error: toast with a generic message, branching on
  `DATA_CHAT_MEDIA_ERROR_CODE` when present (offer to resize on `1`,
  suggest different file on `2`, back off on `3`/`5`). One retry max,
  no auto-retry loop.

### Phase D — Receive path (Option 1: placeholder + dialog)

- When a chat / PM message arrives with both `DATA_CHAT_MEDIA_ID` and
  `DATA_CHAT_MEDIA_TYPE`, render a styled placeholder textentry:

  ```
  [image · PNG · 800×600 · 124 KB · click to view]
  ```

- Click → open in an in-app dialog. The chat-window's existing image
  preview infrastructure (`src/preview.{c,h}` —
  `hx_preview_new` / `_set_info` / `_chunk` / `_done` with cancel-cb)
  is the natural backing — same worker-thread streaming pattern, same
  `GdkPixbufLoader` → `GtkPicture` surface. Either reuse the preview
  window directly or factor its body widget into a transient
  `AdwDialog`.
- Right-click context menu on the placeholder (and on the dialog's
  picture): **Save As**, **Copy Image** (copies the `GdkTexture` to the
  clipboard for paste-into-other-apps), **Open in External Viewer**
  (via `GtkFileLauncher` on a temp file — Flatpak portal handles
  sandbox crossings).
- Async download: fire `TranDownloadMedia` on click rather than on
  receive, to avoid prefetching every image in a busy room. The
  authorization set is fixed at relay time per the spec, so deferred
  fetch is safe as long as we're inside the 24h handle lifetime.
  Chunked replies stream through the same loader path.
- Decode failure surfaces as a toast on the chat window + the
  placeholder flips to `[image · cannot display]` and the click action
  goes inert.

End of Phase D, the client is spec-conformant: it sends, receives,
decodes, and lets the user view media. No xtext surgery yet.

### Phase E — Receive path (Option 2: multi-subline padding inline)

This phase replaces the placeholder with true in-stream rendering. It
is the part that touches xtext, and it is gated on Phase D being
shipped and used long enough to validate the wire / decoder stack.

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

### Phase F — Tier 3 integration against Janus

Janus is the fogWraith reference server and already implements the
inline-media extension (same playbook as chat-history and voice — the
fogWraith specs and Janus ship together). The Tier 3 path lights up
without needing a mock server.

- **First step: verify Janus support.** Connect a debug-built client to
  the existing `janus` Tier 3 container, watch the LOGIN reply for the
  capability echo of bit 3 and the `DATA_CHAT_MEDIA_MAX_*` advisory
  fields. If they're not there, fall back to building a Go mock server
  in `tests/integration/mock-server/inline-media/` (same shape as the
  chat-history mock); the plan in that case is unchanged from what an
  earlier draft of this doc described.
- Matrix entry: add `HX_TEST_CAP_INLINE_MEDIA` to the Janus container
  flags, mirroring how `HX_TEST_CAP_CHAT_HISTORY` and
  `HX_TEST_CAP_VOICE` already gate their respective Tier 3 binaries.
- Tier 3 binaries: end-to-end send (single + chunked), end-to-end
  receive, capability-not-confirmed (mhxd / Mobius — server strips
  fields), error-code surfacing (oversize, rate-limit,
  unsupported-format), authorization (unauthorized download → generic
  "not found"), handle expiry. Most of these reduce to "GtkHx sends X,
  observes Janus's reply matches the spec."
- A few authorization-set + legacy-recipient cases will be hard to
  exercise against Janus alone (need a chat where one user is capable
  and another isn't). The Tier 3 matrix already runs multiple servers
  in parallel for chat-history; mixing a capable Janus member with an
  mhxd or Mobius observer in a public chat covers the legacy-fallback
  path without a mock.

Tracking the supports-or-not check against [[gtkhx_janus]] and
[[gtkhx_multi_server_test_plan]].

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

## Effort sketch

| Phase | Scope | Rough effort |
|---|---|---|
| A | Wire protocol + capability echo + fixtures | 1 week |
| B | Bounded decoder + worker plumbing | 3–5 days |
| C | Send UI (attach, paste, drop, upload state machine) | 1–2 weeks |
| D | Receive: placeholder + dialog + context menu | 1 week |
| E | Inline render via multi-subline padding | 1–2 weeks |
| F | Tier 3 integration against Janus | 2–4 days |

Phases A → D bring spec conformance. Phase E is the embedding payoff.
Phase F can land in parallel with A–D once the wire protocol stabilises.

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
  a still. Documented in Phase B.
- **In-app dialog for click-to-view**, reusing the existing
  `src/preview.{c,h}` widget pipeline. No external viewer in the
  default click path (still available via right-click "Open in
  External Viewer").
- **Right-click context menu on media** (placeholder in Phase D, true
  inline row in Phase E): Save As, Copy Image, Open in External
  Viewer.
- **JPEG / PNG / GIF only** at the decoder. Spec-mandated; SVG / WebP
  / AVIF / HEIC explicitly rejected at sniff time even if some future
  server canonicalises to them.

---

Status: scoping. No code on this branch — the phased work lands on
follow-up branches (`claude/inline-media-phase-a`, etc).
