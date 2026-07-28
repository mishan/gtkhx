//! `hxchat-view` — the GTK4 chat output widget (chat-view phase C2).
//!
//! The GTK skin over [`hxchat_layout`]. All the layout logic lives in
//! that crate and is tested headless; this one owns the widget, the
//! Pango font backend, and the C ABI the existing call sites link.
//!
//! It is the tree's first Rust custom-drawn widget — nothing else in
//! `rust/crates/` implements `WidgetImpl::snapshot` / `measure` /
//! `size_allocate` or `ScrollableImpl`. The scoping doc flagged whether
//! `ScrollableImpl` was even usable from a subclass at the pinned
//! gtk4-rs 0.10 as the thing to settle first; it is
//! (`gtk4-0.10.3/src/subclass/scrollable.rs`), so the fallback of
//! hand-rolled adjustment properties wasn't needed.
//!
//! See docs/chat-view-scoping.md.

pub mod ffi;
pub mod measure;
pub mod view;

/// Tell gtk4-rs that GTK is already initialised.
///
/// The app calls `gtk_init` / `adw_init` from C, so gtk4-rs's own init
/// flag is never set — and its widget constructors assert on that flag,
/// aborting across the FFI even though GTK is running. Every C-ABI entry
/// point that constructs a widget calls this first. Same trap, same
/// remedy, as `gtkhx-ui`'s `ensure_gtk_init` (see that crate's lib.rs).
pub(crate) fn ensure_gtk_init() {
    unsafe { gtk4::set_initialized() };
}

#[cfg(test)]
mod tests;

pub use measure::PangoMeasure;
pub use view::HxChatView;
