# Image decoding

Every image GtkHx renders from an untrusted source — inline-media
attachments, GIF avatars, server banners, and the file-browser preview
— decodes through one crate: `rust/crates/hx-image-decode`. The C
surface is `src/inline_media_decode.h`; `src/inline_media_decode.c` is a
thin shim (extern declarations, `_Static_assert`s pinning the
cross-language struct and enum layouts, and the `hx_image_decode_log`
bridge that routes Rust telemetry through `debug_log("media", …)`).

On Linux the decoder is [glycin](https://gitlab.gnome.org/GNOME/glycin):
each format's decoder runs in a `bwrap`+seccomp subprocess, animation is
supported natively, and none of it is deprecated (unlike `gdk_pixbuf_*`,
widely deprecated in GTK 4.16). The librsvg/glycin family also covers
the CVE history of in-process image decoders — which is the security
motivation the bounded-decoder work was always going to need eventually.

## The loader-generation problem

Glycin ships its loaders in numbered *compatibility generations*
(`/usr/libexec/glycin-loaders/<gen>+/`), and the crates.io `glycin`
crate version maps onto a generation. The crate's version stream and the
loader generation are not interchangeable:

| loader generation | compatible glycin crate | gtk-rs family | ships in |
|---|---|---|---|
| `1+` | crate **1.x and 2.x** | 0.20 (gdk4 0.9, g\* 0.20) | Debian 13 (trixie) etc. |
| `2+` | crate **3.x** | 0.21 (gdk4 0.10, g\* 0.21) | GNOME 48+, Flatpak |

**Debian 13 packages `glycin-loaders` 1.2.x, which installs only the
`1+` generation.** A 3.x-crate build there finds no compatible loader at
runtime and *every decode falls through to `UnsupportedFormat`* —
inline media silently degrades to the placeholder row, banners never
paint, avatars never appear. That silent-degradation failure mode is
the whole reason both backends exist.

## Selecting a backend

The Meson option `-Dglycin_compat` picks one:

- **`auto`** (default) — probe the build host for the newest installed
  loader generation and resolve to `2` or `1`, falling back to `2` with a
  warning if none are found. For a native build the build host *is* the
  target host, so this picks the backend that will actually find loaders
  at runtime, with no flag needed. The probe (in `rust/meson.build`)
  scans `$XDG_DATA_DIRS` for `glycin[-loaders]/<gen>+/conf.d/*.conf`
  registrations — the same files glycin itself reads — newest generation
  first, with the `/usr/libexec/glycin-loaders/<gen>+/` binary
  directories as a fallback signal.
- **`2`** — glycin crate `~3.0`, `2+` loaders. The Flatpak build pins
  this explicitly, since its configure step runs inside the runtime and
  should never rely on the probe.
- **`1`** — glycin crate `~2.1`, `1+` loaders. Debian stable and other
  older runtimes.

**What forcing the older generation costs: nothing**, for our allowlist.
The `1+` loaders (`glycin-image-rs`) decode JPEG / PNG / GIF including
animated GIF, which is exactly the inline-media spec allowlist. The
glycin 2→3 jump added image *editing* / creation APIs and broader
format and metadata work that a decode-only path never touches.

## Why two gtk-rs families can coexist

The two glycin majors pull different gtk-rs families (0.21 vs 0.20).
That is safe **only because this crate is a leaf**: everything it hands
to C crosses as a raw `*mut GdkTexture` / `*mut GArray` (see
`ffi_result.rs`), never as a Rust gtk-rs type shared with another
workspace crate, so no other crate observes the family choice. Only one
backend is ever linked — the inactive one's dependencies are `optional`
and gated off.

`src/compat.rs` aliases the active family to the in-crate names
`glib` / `gio` / `gdk` / `glycin`; the rest of the crate is
version-agnostic. The deps are renamed and optional (`glycin3`/`glib3`/…
vs `glycin2`/`glib2`/…), selected by the mutually-exclusive features
`glycin-v3` (default) / `glycin-v2`. Enabling both, or neither, is a
`compile_error!`.

Meson wiring: the resolved `2` path leaves cargo defaults on. The
resolved `1` path passes `--no-default-features` to suppress the default
`glycin-v3` — and because that flag is workspace-wide it also drops
other members' defaults, so the cargo line re-supplies the load-bearing
ones (the compression features on `hxcrypto`).

### The concrete API delta

Only two spots actually diverge:

1. **Construction.** 3.x has `Loader::new_bytes(Bytes)` — glycin keeps a
   ref and passes the buffer to the subprocess via memfd. 2.x has **no
   bytes constructor**, only `Loader::new(gio::File)`. So the v2 path
   stages the payload into a private temp file (mode `0600`,
   `create_new`, a random UUID in the name, retried on collision) and
   hands glycin a `gio::File`; glycin reads the file's *content* in our
   process and streams it to the loader over a socket, so the sandbox
   never sees the path. A Drop guard unlinks it once the decode future
   is done reading.
2. **Dimension read.** 3.x `Image::details()` methods vs 2.x
   `Image::info()` fields.

Everything else — `sandbox_selector`, `load`, `next_frame`, `delay`,
`texture`, `ErrorCtx`, GIF animation — is identical across both.

## The third backend: non-Linux

glycin is Linux-only (glycin-common uses `memfd_create` for the shared
memory it returns decoded frames through, plus a seccomp sandbox;
neither exists on macOS or Windows). Off Linux the crate decodes with the
pure-Rust `image` crate — no system libraries, no subprocess sandbox.
`decode.rs`'s `run_decode()` is `cfg`-split and builds a texture from
`image`-decoded RGBA there instead.

The codec set is the inline-media allowlist (JPEG / PNG / GIF) plus the
extra formats the wide-policy preview path opens (BMP / ICO / TIFF /
WebP). **Animated GIF and APNG both collect frames** on this path;
everything else yields a single static texture. AVIF and HEIC need C
libraries and degrade to a decode error, the same way a glycin host
missing those loaders would.

**The `image` dependency carries a load-bearing version ceiling**
(`>=0.25.5, <0.25.10`). 0.25.10 started using `slice_as_chunks` /
`is_multiple_of` without bumping its declared `rust-version`, so the
MSRV-aware resolver doesn't avoid it and it fails to compile on our Rust
floor. Revisit the ceiling when the toolchain floor rises.

## Policy and consumers

`HxImageDecodePolicy` picks the format gate:

- **`HX_IMAGE_DECODE_STRICT`** mirrors the inline-media allowlist —
  JPEG / PNG / GIF only, everything else fails at the sniff gate before
  glycin is spawned at all.
- **`HX_IMAGE_DECODE_WIDE`** skips the allowlist check. The sniff still
  runs for the canonical MIME and telemetry, but any non-empty input is
  handed to the decoder. Used by the file-browser preview, where the user
  explicitly opened a BMP / TIFF / WebP / HEIC / SVG that the
  inline-media spec forbids but glycin's bundled loader set can decode.

The cancel and free contract is identical under both policies.

Consumers today: inline-media chat rows and the click-to-view dialog,
GIF avatars (`src/gif_avatar.c`, strict, with its own tighter caps), the
server banner (`gtkhx-ui/src/banner.rs`, via the native Rust entry point
rather than the C ABI), and the file preview (`src/preview.c`, wide).
Banner convergence onto this crate was an explicit non-goal when the
crate was first scoped; it happened anyway, and the crate name was
chosen with room for it.

Chrome-icon loading for themes (`src/gtkhx_icon.c`) is **not** a
consumer — it is PNG-only through gdk-pixbuf, and routing SVG icon packs
through glycin remains a follow-up.

## API shape

```c
typedef struct {
    GdkTexture *texture;          /* strong ref; still or first frame */
    const char *canonical_mime;   /* borrowed static literal */
    HxInlineMediaFormat sniffed_format;
    guint16 error_code;           /* 0 on success; spec MediaErrorCode */
    guint16 _pad0;
    const char *error_message;    /* borrowed static; NULL on success */
    GArray *frames;               /* NULL for stills; else HxInlineMediaFrame */
} HxInlineMediaDecoded;

typedef struct {
    GdkTexture *texture;          /* strong ref, owned by the array */
    guint32 delay_ms;
} HxInlineMediaFrame;

typedef void (*HxInlineMediaDecodeCallback) (HxInlineMediaDecoded *result,
                                             gpointer user_data);

gpointer inline_media_decode_async (const guint8 *bytes, gsize len,
                                    const HxInlineMediaCaps *caps,
                                    HxInlineMediaDecodeCallback cb,
                                    gpointer user_data);

gpointer hx_image_decode_async_with_policy (
    const guint8 *bytes, gsize len, const HxInlineMediaCaps *caps,
    HxImageDecodePolicy policy, HxInlineMediaDecodeCallback cb,
    gpointer user_data);

void inline_media_decode_cancel (gpointer token);
void inline_media_decoded_free (HxInlineMediaDecoded *decoded);
```

The bytes pointer is consumed synchronously, so the caller's buffer can
be released as soon as the call returns; the callback owns the result
and must free it. A NULL return means the call rejected synchronously —
NULL/empty input, byte cap exceeded, or a blocked format from the sniff
— with the callback already fired exactly once before the call
returned. On a non-NULL return the caller must eventually call
`inline_media_decode_cancel`; it is the canonical free function for the
token, cancel-after-completion is a safe no-op that still frees, and the
token is reference-shared with the in-flight task so a successful decode
racing a late cancel is safe.

The struct layout is pinned `#[repr(C)]` on the Rust side with
`_Static_assert`s on the C side catching drift at compile time.

## Sandbox and packaging notes

Glycin's default sandbox selector (`Auto`) picks bwrap on a normal host
and `flatpak-spawn` inside a Flatpak runtime; the nested-sandbox case is
designed for and works without manifest finagling beyond the runtime
pin. The Flatpak manifest targets the GNOME 49 runtime and grants
`--talk-name=org.freedesktop.Flatpak` for the loader subprocess.

There is one escape hatch: `GTKHX_GLYCIN_NO_SANDBOX=1` forces the
unsandboxed loader path. glycin 3.x has no environment knob for the
selector (it is API-only) and its `Auto` choice runs each loader under
bwrap, which an unprivileged CI container cannot spawn — so a decode
there fails and maps to `UnsupportedFormat`. The decode-test fixtures
are trusted in-tree images. **The variable is unset in production**, so
server-supplied images always keep the `Auto` sandbox.

Glycin's licence on both majors (MPL-2.0 OR LGPL-2.1-or-later) is
GPL-2 compatible. `default-features = false` plus `async-io` keeps us off
the tokio interop helpers; the `gdk4` feature is what gives
`Frame::texture() -> gdk::Texture`.

## Rejected alternatives

- **A reusable `glycin-bridge` crate** for other GNOME-Rust apps. Same
  rule as `hotline-proto`: we may produce one structurally, but we don't
  commit to external API stability.
- **Reviving the retired `hxutil` crate** as the home for this. It was
  empty and the name is generic; a purpose-named leaf crate is the
  better shape.
- **A single glycin backend.** Tempting, and wrong in both directions —
  3.x-only breaks Debian stable silently, 2.x-only strands the Flatpak
  and GNOME 48+ hosts on a generation their runtime doesn't ship.

## Open

- **Offscreen animation is not gated.** The original design called for a
  `playback_visible` bit so animated media scrolled out of view stopped
  burning frames. No such gating survived the chat-view rewrite:
  `hxchat-view/src/view.rs` installs a single tick callback whenever
  *any* media in the view is animated and advances every animated entry
  on it, with no visibility test. Scrollback full of animated GIFs
  therefore keeps advancing frames forever. The redraw is a plain
  `queue_draw` (frame dimensions can't change, so no relayout), which
  bounds the damage, but the frame decode-and-advance work is
  unconditional. The user-list avatar path has the analogous
  shared-timer shape and the same gap, mitigated there by a per-user
  pause and a global animation preference.
