//! Shared thin wrapper over the C dock-embed bridge (`dock_bridge.c`).
//!
//! The docked-window Rust shells (Users, Tasks, …) register their content
//! through this — a translation of the type-free `gtkhx_dock_*` C ABI so no
//! Rust module names a libpanel type. The `kind` / `area` ints mirror
//! `GtkhxDockKind` / `GtkhxDockArea` in `dock_bridge.h`.

use std::ffi::c_char;

use crate::tr::tr;

// GtkhxDockKind.
pub const KIND_CENTER: i32 = 0;
pub const KIND_SIDEBAR: i32 = 1;
// Reserved for the per-pchat / per-PM dynamic panels (Chat port).
#[allow(dead_code)]
pub const KIND_DYNAMIC: i32 = 2;

// GtkhxDockArea → toolbar_{sidebar,end,bottom,center}_frame.
pub const AREA_START: i32 = 0;
pub const AREA_END: i32 = 1;
pub const AREA_BOTTOM: i32 = 2;
pub const AREA_CENTER: i32 = 3;

extern "C" {
    fn gtkhx_dock_raise_if_open(id: *const c_char) -> glib::ffi::gboolean;
    fn gtkhx_dock_set_needs_attention(id: *const c_char, state: glib::ffi::gboolean);
    fn gtkhx_dock_embed(
        id: *const c_char,
        kind: i32,
        area: i32,
        title: *const c_char,
        icon_name: *const c_char,
        content: *mut gtk4::ffi::GtkWidget,
    ) -> glib::ffi::gboolean;
}

/// TRUE iff a panel with `id` was already registered (re-attached + raised,
/// so the caller returns early instead of rebuilding its content).
pub fn raise_if_open(id: &str) -> bool {
    let cid = crate::cs(id);
    unsafe { gtkhx_dock_raise_if_open(cid.as_ptr()) != glib::ffi::GFALSE }
}

/// Set / clear the needs-attention hint on the registered panel `id` (no-op
/// if it isn't registered). The dock tab strip badges a flagged panel when
/// it isn't the visible tab.
pub fn set_needs_attention(id: &str, state: bool) {
    let cid = crate::cs(id);
    let g = if state {
        glib::ffi::GTRUE
    } else {
        glib::ffi::GFALSE
    };
    unsafe { gtkhx_dock_set_needs_attention(cid.as_ptr(), g) }
}

/// Embed `content` as a static docked panel titled `title_key` (translated)
/// with `icon_name`, at `kind` / `area`. Returns TRUE on success; on failure
/// the bridge has already destroyed `content`, so the caller just skips its
/// post-embed work. `content` is consumed either way — don't touch it after.
pub fn embed(
    id: &str,
    kind: i32,
    area: i32,
    title_key: &str,
    icon_name: &str,
    content: *mut gtk4::ffi::GtkWidget,
) -> bool {
    let cid = crate::cs(id);
    let ctitle = crate::cs(&tr(title_key));
    let cicon = crate::cs(icon_name);
    unsafe {
        gtkhx_dock_embed(
            cid.as_ptr(),
            kind,
            area,
            ctitle.as_ptr(),
            cicon.as_ptr(),
            content,
        ) != glib::ffi::GFALSE
    }
}
