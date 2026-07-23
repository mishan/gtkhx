//! gtk-rs + glycin version shim.
//!
//! Exactly one glycin backend is compiled at a time (`glycin-v3` →
//! glycin 3.x / 2+ loaders, the default; `glycin-v2` → glycin 2.x /
//! 1+ loaders, for Debian-stable-class runtimes). The two majors pull
//! different gtk-rs families (0.21 vs 0.20), so the rest of the crate
//! can't name `glib` / `gio` / `gdk` / `glycin` directly without
//! pinning a family. This module re-exports the active family under
//! those stable names; every other module imports from
//! `crate::compat`.
//!
//! Safety of mixing 0.20 and 0.21 in one binary: only one is ever
//! linked (the inactive backend's deps are `optional` and gated off),
//! and even across the staticlib boundary this crate only ever hands C
//! raw `*mut GdkTexture` / `*mut GArray` pointers — never a Rust gtk-rs
//! type — so no other workspace crate observes the family choice.
//!
//! This module is `pub` (but `#[doc(hidden)]`) purely so the
//! integration test crate (`tests/decode.rs`), which lives outside the
//! library and therefore can't see `crate::compat`, can borrow the same
//! aliases via `hx_image_decode::compat`.

#[cfg(all(feature = "glycin-v2", feature = "glycin-v3"))]
compile_error!(
    "hx-image-decode: enable exactly ONE glycin backend feature \
     (`glycin-v2` xor `glycin-v3`), not both. The Meson `-Dglycin_compat=1` \
     path must pass `--no-default-features` so the default `glycin-v3` is \
     not additively combined with `glycin-v2`."
);

#[cfg(not(any(feature = "glycin-v2", feature = "glycin-v3")))]
compile_error!(
    "hx-image-decode: no glycin backend selected. Enable `glycin-v3` \
     (glycin 3.x / 2+ loaders, default) or `glycin-v2` (glycin 2.x / 1+ \
     loaders). Meson selects this via -Dglycin_compat."
);

// gtk-rs family aliases (glib/gio/gdk) — cross-platform. The texture FFI
// (`ffi_result.rs`) needs gdk/glib on every target, and gtk-rs builds on
// Windows/macOS, so these are selected purely by the backend feature.
#[cfg(feature = "glycin-v3")]
pub use {gdk3 as gdk, gio3 as gio, glib3 as glib};

#[cfg(feature = "glycin-v2")]
pub use {gdk2 as gdk, gio2 as gio, glib2 as glib};

// glycin alias — Linux only. glycin is a Linux-only dependency (see Cargo.toml);
// the non-Linux build decodes via the `image` crate in `decode.rs` and never
// names `compat::glycin`, so the alias simply doesn't exist there.
#[cfg(all(target_os = "linux", feature = "glycin-v3"))]
pub use glycin3 as glycin;

#[cfg(all(target_os = "linux", feature = "glycin-v2"))]
pub use glycin2 as glycin;
