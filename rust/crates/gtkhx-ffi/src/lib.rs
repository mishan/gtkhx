//! `gtkhx-ffi` — the workspace's single staticlib.
//!
//! # Why this crate exists
//!
//! Before this crate, all ~39 FFI-exporting members declared
//! `crate-type = ["staticlib", "rlib"]`, so each produced its own `.a` and
//! `src/meson.build` listed all of them on the C link line **in a
//! hand-maintained order**. Static archives are resolved left-to-right and a
//! given archive is only searched for symbols that are undefined *at the point
//! the linker reaches it*, so every crate had to be placed ahead of the crates
//! providing its externs. That produced three problems:
//!
//! 1. Sibling Rust crates talked to each other through `extern "C"` blocks
//!    rather than Cargo dependencies — no type checking, no inlining, and
//!    signature drift only surfaced as a link error.
//! 2. Adding a crate meant working out where it belonged in the order and
//!    writing a comment explaining why (~40 lines of such commentary).
//! 3. Merging two crates was blocked outright: a `staticlib` bundles its rlib
//!    dependencies, so two archives could end up defining the same
//!    `#[no_mangle]` symbol and collide at the final link. That is exactly why
//!    the boxed payload types were split into a crate of their own, away from
//!    the session that used them (see `docs/rust/crate-layout.md`).
//!
//! Making this the *only* `staticlib` and every other member an `rlib` fixes
//! all three. rustc bundles the whole rlib graph into one archive, so each
//! `#[no_mangle]` symbol is defined exactly once, link order stops mattering,
//! and crates are free to depend on each other the normal way.
//!
//! # How it works
//!
//! This crate has no code of its own. It exists to name every FFI-exporting
//! member as a dependency, which puts them in the crate graph; rustc then emits
//! their object code — C ABI symbols included — into `libgtkhx_ffi.a`.
//!
//! The `use ... as _;` bindings below are load-bearing. A dependency that is
//! declared in `Cargo.toml` but never referenced can be dropped from the crate
//! graph, taking its `#[no_mangle]` symbols with it. The anonymous import is
//! the standard way to say "link this crate, I don't name anything from it".
//! `tests/symbols.rs` is the backstop: it asserts the archive still defines
//! every C ABI symbol the C tree expects, so a dropped crate fails the test
//! suite rather than the meson link.
//!
//! # Adding a crate
//!
//! Add it to `[dependencies]` and add a `use` line here. There is no ordering
//! to reason about and nothing to touch in `src/meson.build`.

// --- GObject / UI layer ---
use gtkhx_core as _;
use gtkhx_ui as _;

// --- protocol core ---
use hotline_proto as _;
use hxbridge as _;
use hxnet as _;

// --- crypto / compression ---
use hxcrypto as _;

// --- protocol handler layer (recv + send) ---
use hxhandlers as _;

// --- models ---
use hxmodel as _;

// --- per-session state owner (extern-ful: cannot live in gtkhx-core) ---
use hxtask as _;

// --- leaf utilities ---
use hx_image_decode as _;
use hxbookmarks as _;
use hxchat_view as _;
use hxfiles_xfer as _;
use hxhfs as _;
use hxmacres as _;
use hxsound as _;
use hxtext as _;
use hxtls_trust as _;

// --- voice (gated by -Dvoice, see rust/meson.build) ---
#[cfg(feature = "voice")]
use hxvoice as _;
#[cfg(feature = "voice")]
use hxvoice_model as _;
#[cfg(feature = "voice")]
use hxvoice_runtime as _;
#[cfg(feature = "voice")]
use hxvoice_send as _;
