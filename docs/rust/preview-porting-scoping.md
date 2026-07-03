# Porting preview.c to Rust — scoping

**Status:** scoping / decision doc. `preview.c` came up as an R5 target after
the TLS-trust dialog (R5.4). Unlike the dialogs, it's a large subsystem with
external-library viewers and cross-thread worker marshaling, so it needs a
plan before ~1.5k lines of Rust get written.

## TL;DR

`preview.c` (1538 LOC) is the file-preview window: a **standalone** GtkWindow
(not docked — no libpanel involvement) that streams a downloaded file's data
fork through a **type-aware viewer registry** and offers a Save button. Four
viewers: text (GtkTextView), image (glycin async decode + PICT/ImageMagick
fallback), PDF (poppler + cairo), source (GtkSourceView). The worker→main
marshaling is refcounted and uses an atomic end-of-stream flag.

**Recommendation:** portable to Rust as a standalone effort (it needs none of
the dock bridge). The one real dependency question is the **PDF and source
viewers**, which need `poppler-rs` + `sourceview5` crates aligned to the
pinned **gtk4 0.10 / glib 0.21** stack. `sourceview5` almost certainly has a
matching release (0.9.x — it's a lockstep gtk-rs crate); `poppler-rs` is the
one to actually verify. Gate both behind Cargo features mirroring the
existing `HAVE_POPPLER` / `HAVE_GTKSOURCEVIEW` meson options, so a missing /
misaligned binding just drops that viewer to the text fallback (exactly what
the C build already does when the C lib is absent) rather than blocking the
whole port. Sequence it whenever; it's independent of the docked windows.

## What preview.c is

### Lifecycle + threading

`rcv.c::rcv_task_file_get` creates the window (`hx_preview_new`), wires a
cancel hook (`hx_preview_set_cancel_cb` = `xfer_delete` + the htxf), and the
HTXF worker then streams the data fork through three entry points **called on
the worker thread**:

```c
void hx_preview_new (const char *name) -> hx_preview *   /* main; refcount = 2 */
void hx_preview_set_info  (hx_preview *, type, creator);  /* worker */
void hx_preview_chunk     (hx_preview *, buf, len);       /* worker */
void hx_preview_done      (hx_preview *);                 /* worker */
void hx_preview_set_cancel_cb (hx_preview *, fn, data);   /* main */
void hx_preview_unref     (hx_preview *);                 /* either */
```

The worker entry points don't touch widgets: each takes a ref and
`g_idle_add`s a heap payload (`set_info_job` / `chunk_job` / the preview
itself) to the main thread, where `*_dispatch` mutates the UI. Key subtleties
the port must preserve:

- **Refcount = 2 at construction** (window holds one, the caller stashes one
  on `htxf->preview`) closes a use-after-free where closing the window
  mid-transfer would free the struct while the worker still reads it.
- **`stream_finished` is an atomic** set on the *worker* the instant
  `hx_preview_done` is called (not in the main-thread dispatch), so a
  window-close racing the end of stream doesn't fire the cancel hook on an
  already-finished transfer.
- **`closed` flag**: queued idle jobs bail before touching widgets if the
  user closed the window mid-stream.

### The viewer registry

`struct hx_viewer` is a vtable — `{ name, score(type,creator,filename),
create(p)->widget, chunk(p,buf,len), done(p), close(p) }`. `pick_viewer`
runs every `score()` and installs the highest (ties → earlier table entry);
the text viewer scores 1 unconditionally, so it's the universal fallback.
`p->bytes` (a GByteArray) accumulates the whole stream regardless of viewer
so Save always works.

| Viewer  | Backend                                   | Optional? |
|---------|-------------------------------------------|-----------|
| text    | `GtkTextView`                             | no (always the fallback) |
| image   | glycin async decode → `GdkTexture`, with a PICT sniff (`pict_embed`) + ImageMagick raster (`pict_magick`) fallback chain | ImageMagick gated by `HAVE_IMAGEMAGICK` |
| pdf     | poppler (`PopplerDocument`/`Page`) drawn into a `GtkDrawingArea` via cairo | `HAVE_POPPLER` |
| source  | `GtkSourceView` / `GtkSourceBuffer` / language detection | `HAVE_GTKSOURCEVIEW` |

The PDF and source viewers are already **optional at build time** — without
the C lib, `config.h` leaves the define unset and that file type falls
through to the text viewer. The Rust port should keep that property.

## Rust port shape

- **Standalone window** — `hx_preview_new` builds a `gtk::Window` +
  `AdwHeaderBar` + Save button and `present()`s it; no dock bridge (contrast
  with the docked windows, see `dock-porting-scoping.md`). The entry points
  stay `#[no_mangle]` (rcv.c / the HTXF worker call them); `preview.h` keeps
  the C ABI decls.
- **Viewer registry** — `hx_viewer` becomes a Rust trait (`score` / `create`
  / `chunk` / `done` / `close`); the `viewers[]` table becomes a slice of
  trait objects. Straightforward.
- **Cross-thread state** — the tricky part. Split the struct: an
  `Arc`-shared core for the bits the worker touches (name, atomic
  `stream_finished`, the refcount — or just lean on `Arc`'s own count, cancel
  hook) vs. a main-thread-only `PreviewUi` (widgets, viewer state, `bytes`).
  The worker entry points marshal to main via the existing R3 bridge
  (`gtkhx_bridge_post_to_main`, already used by banner.c / xfers.c) or a
  direct `glib::idle_add`. This is the one place to be careful — get the
  Send/Sync boundary and the close-vs-done race right, ideally with a Tier-2
  test around the marshaling shim.
- **Save** — `GtkFileDialog` async save of `bytes`; a small `#[no_mangle]`-free
  Rust path.

## External-dependency question (the crux)

Three of the four viewers reach outside gtk4 itself:

- **image** — already solved. The glycin decode goes through the existing
  `hx-image-decode` crate + the async FFI (`HxInlineMediaDecoded`) that
  inline-media / banner already use; PICT/ImageMagick fallback stays in
  `pict_embed.c` / `pict_magick.c` and is called via FFI. No new dep.
- **source** — needs [`sourceview5`](https://crates.io/crates/sourceview5).
  It's a first-class lockstep gtk-rs crate (bilelmoussaoui). Current is 0.10
  (the gtk4 0.11 cycle); the **gtk4 0.10 cycle release is 0.9.x**, which
  matches our pin (same cycle as libadwaita 0.8). Very likely a clean add.
- **pdf** — needs a poppler binding (`poppler-rs`, cairo-based). Exists in
  the gtk-rs ecosystem but is less actively tracked than sourceview5;
  **verify a release targets gtk4 0.10 / glib 0.21 / cairo of that cycle**
  before committing. This is the same version-alignment trap as libpanel
  (whose current 0.6 needs gtk4-sys 0.11): the *current* poppler release
  probably targets gtk4 0.11, so we'd pin the previous-cycle version.

**Verification step (do first, at port time):** in the workspace, run
`cargo add sourceview5@0.9 --dry-run` and the equivalent for the poppler
crate, and confirm the resolver picks a version whose transitive `gtk4-sys` /
`glib-sys` / `cairo-sys` match the locked 0.10 / 0.21 generation. If poppler
doesn't resolve, fall back (below) rather than bumping the whole stack (the
Debian-stable floor is a hard constraint — see `rust/Cargo.toml`).

## Options

### A. Full port, both optional viewers as Cargo features — recommended

Port all the machinery + text + image now; add `sourceview5` and the poppler
crate as **optional workspace deps behind Cargo features** wired to the
existing `-Dpoppler` / `-Dgtksourceview` meson options (which already gate the
C build). `#[cfg(feature = "...")]` the two viewers; when a feature is off,
the file type drops to the text viewer — identical to today's `HAVE_*`
behaviour. If poppler-rs won't align, ship with the pdf feature off (PDF →
text fallback) until the stack bumps; source still ships.

- **Pros:** one coherent port; keeps the optional-viewer story; no C left
  behind; no stack bump.
- **Cons:** the cross-thread marshaling needs care (the main real risk);
  poppler-rs may force the pdf viewer off for now.

### B. Port the frame + text + image now, keep pdf/source viewers in C

Move `hx_preview` + the registry + text + image to Rust, but leave
`pdf_viewer` / `source_viewer` as C `hx_viewer` entries the Rust registry
calls via FFI.

- **Cons:** a Rust registry holding C vtable pointers is awkward, and it
  leaves `preview.c` half-alive for little benefit. Only worth it if *both*
  poppler-rs and sourceview5 turn out misaligned — unlikely, since sourceview5
  should be fine. Rejected unless verification forces it.

### C. Defer preview until the gtk-rs stack bumps to 0.22+

- **Cons:** preview doesn't otherwise depend on the bump (sourceview5 0.9
  covers source today; only pdf is at risk), so deferring the whole thing for
  one optional viewer is overkill. Rejected.

## Decision / recommendation

**Option A.** Port `preview.c` → `preview.rs` as a standalone window (no dock
dependency), gating the pdf + source viewers behind Cargo features mirroring
the existing meson options. Verify the poppler crate's gtk4-0.10 alignment
first; ship pdf-off if it doesn't resolve. Reuse the glycin FFI + the R3
main-thread bridge; put a Tier-2 test on the worker→main marshaling shim
(refcount + the close-vs-done race are the parts most worth pinning).

**Ordering:** independent of the dock work (preview is a standalone window),
so it can land any time. It *is* transfer-coupled (the HTXF worker drives it
and the cancel hook is `xfer_delete`), so doing it near the Files/xfers work
lets the worker-marshaling design be shared — but that's a convenience, not a
blocker.
