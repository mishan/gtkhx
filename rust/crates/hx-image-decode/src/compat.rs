//! gtk-rs + glycin version shim.
//!
//! Exactly one glycin backend is compiled at a time (`glycin-v3` →
//! glycin 3.x / 2+ loaders, the default; `glycin-v2` → glycin 2.x /
//! 1+ loaders, for Debian-stable-class runtimes).
//!
//! **The crate's public gtk-rs family is fixed at the workspace's 0.21
//! line** (`glib` / `gdk` below), independent of the backend. That is what
//! lets gtkhx-ui / gtkhx-ffi — pinned to 0.21 — link this crate and consume
//! the `gdk::Texture` that [`crate::decode::decode_first_frame_async`] returns,
//! and it is what the C FFI `*mut GdkTexture` (`ffi_result.rs`) is built from,
//! regardless of which glycin backend is active.
//!
//! glycin 2.x, however, is on gtk-rs 0.20, so its `Loader`/`Frame` speak the
//! 0.20 family (`glib2` / `gio2` / `gdk2`, compiled only for Linux + glycin-v2).
//! Those types never escape the crate: `decode.rs`'s `adopt_texture` bridges a
//! glycin-2 `Texture` to the 0.21 public one by raw `GdkTexture*` (the pointer
//! is ABI-identical across gtk-rs versions). glycin-v3 needs no bridge — glycin
//! 3.x already uses the public 0.21 family.
//!
//! This module is `pub` (but `#[doc(hidden)]`) purely so the integration test
//! crate (`tests/decode.rs`), which lives outside the library and therefore
//! can't see `crate::compat`, can borrow the same aliases via
//! `hx_image_decode::compat`.

#[cfg(all(feature = "glycin-v2", feature = "glycin-v3"))]
compile_error!(
    "hx-image-decode: enable exactly ONE glycin backend feature \
     (`glycin-v2` xor `glycin-v3`), not both. The Meson `-Dglycin_compat=1` \
     path must pass `--no-default-features` so the default `glycin-v3` is \
     not additively combined with `glycin-v2`. Also check that gtkhx-ffi and \
     gtkhx-ui depend on hx-image-decode with `default-features = false`, so the \
     default backend does not leak in through those edges."
);

#[cfg(not(any(feature = "glycin-v2", feature = "glycin-v3")))]
compile_error!(
    "hx-image-decode: no glycin backend selected. Enable `glycin-v3` \
     (glycin 3.x / 2+ loaders, default) or `glycin-v2` (glycin 2.x / 1+ \
     loaders). Meson selects this via -Dglycin_compat."
);

// Public gtk-rs family — the workspace 0.21 line, always. Re-exported here so
// the rest of the crate imports gtk-rs from one place.
pub use {::gdk, ::glib};

// glycin-v2's gtk-rs 0.20 family — Linux + glycin-v2 only. Used solely to talk
// to glycin 2.x and bridge its texture to the public `gdk` above.
#[cfg(all(target_os = "linux", feature = "glycin-v2"))]
pub use {::gdk2, ::gio2, ::glib2};

// glycin alias — Linux only. glycin is a Linux-only dependency (see Cargo.toml);
// the non-Linux build decodes via the `image` crate in `decode.rs` and never
// names `compat::glycin`, so the alias simply doesn't exist there.
#[cfg(all(target_os = "linux", feature = "glycin-v3"))]
pub use glycin3 as glycin;

#[cfg(all(target_os = "linux", feature = "glycin-v2"))]
pub use glycin2 as glycin;
