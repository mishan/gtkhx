# Glycin Migration Plan

Scoping doc for replacing `inline_media_decode.c`'s `GdkPixbufLoader`
pipeline with a glycin-driven Rust decoder, and shipping Phase E of the
inline-media extension on that decoder instead of the deprecated
pixbuf-loader path.

## Status (June 2026) — shipped

All six sub-phases shipped on `claude/inline-media-glycin`:

- **G.1**: `hx-image-decode` crate scaffold + magic-byte sniff in
  Rust. FFI shims for sniff / format_is_allowed / format_to_mime.
- **G.2**: Glycin static-image decode + async ripple in C. The
  C-side `inline_media_decode` sync function is gone; the dialog
  callback fires through `inline_media_decode_async` and a new
  decode-done handler. `_Static_assert(sizeof(HxInlineMediaDecoded)
  == 40)` pins the FFI struct shape both sides.
- **G.3**: Animation in decoder + xtext. Frame collection loop
  caps at `max_frames` / `max_duration_ms`. New
  `gtk_xtext_media_set_animation` API with per-frame
  `g_timeout_add` tick. chat.c branches on `frames->len > 1` to
  install animations.
- **G.4**: Pause-when-offscreen tick gating via cheap visibility
  walk from `pagetop_ent`.
- **G.5**: Flatpak manifest documents glycin runtime expectations
  and adds `--talk-name=org.freedesktop.Flatpak` for the loader
  subprocess.
- **G.6**: Rust integration tests drive `inline_media_decode_async`
  end-to-end against the fixture PNG/JPEG/GIF. Tier 3 animated-GIF
  round-trip is the remaining follow-up (see "Future work" below).

The pre-glycin Phase E branch (`claude/inline-media-phase-e`)
stays as the reference checkpoint for the pixbuf-loader path; it
does not merge to main.



Glycin is the modern GTK image loader — sandboxed (each format's decoder
runs in a `bwrap`+seccomp subprocess), supports animation natively,
non-deprecated (vs. `gdk_pixbuf_*` which is widely deprecated in 4.16),
and is what new GNOME apps use. The librsvg/glycin family also covers
the CVE history of in-process image decoders, which is the security
motivation Phase B was always going to need eventually.

This plan does NOT block on the broader R3+ work in
`docs/RUST-ROADMAP.md`. The decoder is a clean leaf — pure compute over
byte input, returns `GdkTexture`. Glycin is async-only so we ripple
that through two call sites in C (dialog + chat.c auto-fetch), but
both already tolerate async.

## What stays C, what moves to Rust

Per `RUST-ROADMAP.md` locked-in decision #6 (`xtext` is not rewritten)
and decision #8 (single-conn + UI windows stay C until R5), the only
part of the inline-media stack that's a viable Rust target today is the
decoder. Specifically:

| File / module                          | Disposition under this plan                                  |
|----------------------------------------|--------------------------------------------------------------|
| `src/inline_media_decode.c`            | **Deleted** — impl moves to Rust crate                       |
| `src/inline_media_decode.h`            | **Stays** as the C surface; declares the new async API       |
| `src/xtext.c` Phase E additions        | **Stays C** — paints `GdkTexture` via cairo, decoder-agnostic |
| `src/chat.c` auto-fetch wiring          | **Stays C** — async-shape upgrade only; chat.c is R5 step 9  |
| `src/inline_media_dialog.c`            | **Stays C** — async-shape upgrade in `on_download_done`      |
| `src/inline_media_{attach,upload,download}.c` | **Stays C** — state machines tied to task lifecycle (R3+) |
| `src/inline_media.c`                   | **Stays C** — cap accessors; no decode logic                 |

The Phase E xtext changes (`gtk_xtext_append_media`, render_media_line,
multi-subline padding, selection promotion, click dispatch) all
manipulate a `GdkTexture *` on a media-typed entry — whether that
texture came from pixbuf-loader or glycin makes no difference to the
xtext side. The only xtext change pulled in by glycin is animation
support (G.3 below), which adds a frames array + tick state to
`xtext_media_data`.

## Phased plan

### G.1 — Crate scaffold + magic-byte sniff (~150 LOC Rust)

- New crate `rust/crates/hx-image-decode/`. Cargo deps:
  - `glycin = "2"`
  - `gtk4 = "0.10"` (or whatever pin matches the rest of the workspace
    once gtk-rs lands in R5)
  - `gdk-pixbuf` is **not** a dep here — that's the whole point.
- Port `inline_media_sniff` + `inline_media_format_*` from C to Rust.
  Pure magic-byte logic; trivial.
- Stub `decode_async` returning a hardcoded result so the FFI shape
  validates end-to-end before glycin work begins.
- Wire into `rust/Cargo.toml`, `rust/meson.build`.
- `src/inline_media_decode.h` updated to declare the new async API
  (see "API shape" below).
- `src/inline_media_decode.c` becomes a thin extern shim around the
  Rust functions; old loader impl deleted.

### G.2 — Glycin static-image decode (~200 LOC Rust + ~50 LOC C)

- `decode_async` wraps `gly::Loader`:
  - Construct from a `GMemoryInputStream` wrapping the caller's bytes.
  - Set caps via `loader.set_*` for max width / height.
  - `load_async()` → `gly::Image`, `next_frame_async()` → `gly::Frame`.
- First-frame path returns the texture + canonical mime, callback
  fires on main thread.
- Error mapping: glycin errors → spec `MediaErrorCode` (1..=5 + generic).
- `inline_media_sniff` runs BEFORE handing bytes to glycin (defence in
  depth — even if glycin parses something the allowlist rejects, we
  fail at the sniff gate).
- C-side bridges:
  - `inline_media_dialog.c::on_download_done` migrates to the async
    decode call. The existing `GtkStack` Loading page covers the
    extra round-trip latency from the sandbox subprocess.
  - `chat.c::on_inline_media_autofetch_done` migrates similarly. The
    callback chain is already async-tolerant (downloads were always
    async); this just adds a second async step.
- Cap accessors stay in `src/inline_media.h` (C-side) and pass into
  the decode call via `HxInlineMediaCaps`.

### G.3 — Animation (~150 LOC Rust + ~100 LOC C)

- `decode_async` loops `next_frame_async` until end-of-stream,
  collecting an array of `{ GdkTexture *, guint32 delay_ms }` frames.
- Cap enforcement: count frames against `max_frames`, sum delays
  against `max_duration_ms`; reject with `PayloadTooLarge` (the spec
  doesn't separate animation-specific caps).
- `xtext_media_data` gains a frames array, current-frame index, and a
  GLib timeout id. New API:
  - `gtk_xtext_media_set_animation(buf, ent, frames_array)` — swaps in
    the animation, kicks off the tick.
  - `gtk_xtext_media_set_texture` stays — static-image case.
- Tick callback: `g_timeout_add` keyed on the current frame's delay,
  advance to next frame, `gtk_widget_queue_draw` for the row band.
- `xtext_entry_media_data_free` cancels the tick before dropping the
  frames array.
- Existing v1 "first frame only" decision in the inline-media plan
  retired — animation ships in this phase.

### G.4 — Pause-when-offscreen (~75 LOC C)

- xtext gains a per-buffer visibility walk on adjustment change:
  iterate text_first → text_last, mark each media entry's
  `playback_visible:1` bit based on whether its sublines intersect
  the visible region.
- Tick callback checks `playback_visible` and skips the frame
  advance + redraw when offscreen.
- Without this, N animated GIFs in scrollback burn CPU forever even
  when the user has scrolled past them. Confirmed perf issue with
  the gtk-pixbuf-animation path; same problem with glycin.

### G.5 — Flatpak manifest (~30 LOC YAML)

- Update Flatpak manifest:
  - Pin `org.gnome.Platform >= 48` (March 2025+; ships glycin 2.x).
    Earlier platforms have glycin 1.x or none.
  - Add `org.freedesktop.Sdk.Extension.rust-stable` to the sdk
    extensions list, with `prepend-path` for `/usr/lib/sdk/rust-
    stable/bin` in the build env.
  - Verify glycin loaders mount at `/usr/libexec/glycin-loaders/`
    inside the runtime; no manifest finagling beyond the runtime pin.
- Glycin's nested-sandbox-inside-Flatpak case is documented to work
  out of the box on the GNOME platform; loaders are designed for it.
- Local dev (non-Flatpak): glycin packages from distro. User's
  sandbox already has `/usr/include/glycin-2` so the headers are
  present in current Debian/Ubuntu.

### G.6 — Tests (~200 LOC)

- New Rust unit tests in `hx-image-decode`:
  - Magic-byte sniff (parity with current
    `tests/unit/test_inline_media_decode.c` sniff cases).
  - Static decode of fixture PNG / JPEG / GIF bytes (uses the
    existing fixtures under `tests/common/`).
  - Animated-GIF decode → ≥2 frames with positive delays.
  - SVG / WebP / AVIF / HEIC bytes → rejected at sniff with
    `UnsupportedFormat`.
  - Oversized bytes (above max_bytes) → `PayloadTooLarge`.
- Delete `tests/unit/test_inline_media_decode.c` — logic in Rust.
- Tier 3 Janus integration suite (`tests/integration/test_inline_
  media.c`) stays as-is: wire path is unchanged. Add an animated-GIF
  upload + auto-fetch path so the e2e covers the new animation
  rendering.

## API shape

C-side after the migration (`src/inline_media_decode.h`):

```c
typedef struct {
    /* Static still or first frame of an animation. Strong ref. */
    GdkTexture *texture;
    /* NULL for stills; otherwise a GArray of HxInlineMediaFrame.
     * Each frame holds its own strong ref + delay_ms. */
    GArray *frames;
    /* Borrowed pointer to a static literal — caller doesn't free. */
    const char *canonical_mime;
    HxInlineMediaFormat sniffed_format;
    /* 0 on success; spec MediaErrorCode wire value otherwise. */
    guint16 error_code;
    const char *error_message;
} HxInlineMediaDecoded;

typedef struct {
    GdkTexture *texture;  /* strong ref, owned by the array */
    guint32 delay_ms;
} HxInlineMediaFrame;

typedef void (*HxInlineMediaDecodeCallback)(
    HxInlineMediaDecoded *result, gpointer user_data);

/* Caller retains ownership of `bytes`; the FFI copies into the
 * subprocess via the input stream wrapper. `result` is freed by
 * the callback's exit. */
extern void inline_media_decode_async(
    const guint8 *bytes, gsize len,
    const HxInlineMediaCaps *caps,
    HxInlineMediaDecodeCallback cb,
    gpointer user_data);

/* Cancel an in-flight decode. NULL-safe. */
extern void inline_media_decode_cancel(gpointer cancel_token);
```

The sync `inline_media_decode` retired — Misha confirmed we don't need
the sync tests since the entire decode shape is changing.

## Effort estimate

| Phase | Scope                                          | Rough effort |
|-------|------------------------------------------------|--------------|
| G.1   | Crate scaffold + sniff port + FFI stub         | ~1 day       |
| G.2   | Glycin static decode + async ripple in C       | ~2 days      |
| G.3   | Animation in xtext                             | ~2 days      |
| G.4   | Offscreen tick gating                          | ~1 day       |
| G.5   | Flatpak manifest                               | ~0.5 day     |
| G.6   | Rust tests + Tier 3 animated GIF path          | ~1 day       |

Total: ~7–8 focused days, ~900 LOC.

## Phase E disposition

The work already on `claude/inline-media-phase-e` (pixbuf-loader path,
shipped per the previous session) stays as a checkpoint but doesn't
merge to main as-is. The migration order:

1. Land G.1 (scaffold + sniff) on a new branch
   `claude/inline-media-glycin` off `main`.
2. Land G.2 (static decode via glycin) on the same branch.
3. Cherry-pick Phase E's xtext widget changes + chat.c auto-fetch
   onto this branch. The pixbuf-loader-specific bits (pixbuf cache,
   `gdk_pixbuf_get_from_texture`, `gdk_cairo_set_source_pixbuf`)
   get rewritten to talk directly to GdkTexture via cairo
   download-and-paint (one option) or a small textures-to-cairo
   shim. Either way, no deprecated calls.
4. Land G.3 / G.4 / G.5 / G.6.
5. Single PR merges the lot.

This avoids shipping the deprecated-API path to main and gives a
cleaner Phase E commit. The `claude/inline-media-phase-e` branch sits
as a reference checkpoint for "this is what the pixbuf-loader
implementation looked like."

## Open questions

1. **Crate name.** `hx-image-decode` (room for `banner.c` to converge
   later — its banner PNG decode is a natural sibling) or
   `inline-media-decode` (single-purpose, clear scope)? Leaning toward
   `hx-image-decode` for future reuse.
2. **Glycin version pin.** Floor at the Glycin 2.x project line
   (libglycin-2 2.1.x on the host, in org.gnome.Platform 48). The
   Rust crate we depend on (`glycin` on crates.io) has its own
   independent version stream and is at 3.1.x currently — same
   product, different versioning. Both speak the same loader
   subprocess protocol at `/usr/libexec/glycin-loaders/2+/`. The
   Rust crate's 3.x major reflects breaking changes in the *Rust*
   API since extraction; it isn't a new "Glycin 3" product. The
   Cargo.toml on `hx-image-decode` carries this same disambiguation
   so future readers don't trip on the apparent number jump.
3. **Loader sandboxing telemetry.** Glycin spawns a subprocess per
   decode; this surfaces in `ps`. Worth adding a debug-category
   `media` log line at decode start/end so the perf cost is visible
   when investigating "why does the chat window jank when an image
   arrives."
4. **`hxutil` revival or new home.** R0's `hxutil` crate was retired
   at R2 closeout. The image-decode crate is roughly the same shape
   (small leaf, no deps on other gtkhx crates). New crate seems
   right — `hxutil` was empty and the name is generic.

## What this plan explicitly does NOT cover

- Porting `inline_media_attach.c`, `inline_media_upload.c`,
  `inline_media_download.c` to Rust. The wire-format pieces of those
  already delegate to `hotline-proto` per R2; the remaining state
  machines tie into the task lifecycle, which is heavily C until R3
  (network) and R5 (chat window). They migrate naturally with the
  chat window.
- Rewriting `xtext.c` Phase E changes in Rust. Per RUST-ROADMAP
  locked-in decision #6, xtext stays vendored C.
- Banner image decode (`banner.c`). The decoder crate is shaped so
  banner.c could consume it later, but porting banner.c is not part
  of this work.
- A reusable `glycin-bridge` crate for other GNOME-Rust apps. Same
  rule as the R2 `hotline-proto` non-goal: we may produce one
  structurally, but we don't commit to external API stability.

## References

- [Glycin](https://gitlab.gnome.org/GNOME/glycin) — sandboxed image
  loaders for the GNOME platform.
- [Glycin docs.rs](https://docs.rs/glycin/latest/glycin/) — Rust API
  reference.
- `docs/RUST-ROADMAP.md` — phased Rust port (R0–R7 + R∞).
- `docs/inline-media-plan.md` — fogWraith Inline-Media capability spec
  and Phase A → F status.
- `fogWraith/Hotline/Docs/Protocol/Capabilities-Inline-Media.md` — wire
  spec.
