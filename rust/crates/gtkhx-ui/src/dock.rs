//! Shared thin wrapper over the C dock-embed bridge (`dock_bridge.c`).
//!
//! The docked-window Rust shells (Users, Tasks, …) register their content
//! through this — a translation of the type-free `gtkhx_dock_*` C ABI so no
//! Rust module names a libpanel type. The `kind` / `area` ints mirror
//! `GtkhxDockKind` / `GtkhxDockArea` in `dock_bridge.h`.

use std::ffi::{c_char, c_void};

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

// ---- Panel ids ----
//
// The stable string keys the registry and the layout file use. They mirror
// `HX_PANEL_ID_*` in `panel_registry.h` and are load-bearing: a saved layout
// names its leaves by these, so renaming one silently orphans every user's
// placement for that panel.
//
// Canonical here rather than one `const` per window module, because
// `PER_CONNECTION` below has to be able to name all of them, and a list that
// can drift from the definitions it lists is worse than no list.
pub const ID_CHAT: &str = "chat";
pub const ID_USERS: &str = "users";
pub const ID_TASKS: &str = "tasks";
pub const ID_NEWS: &str = "news";
pub const ID_NEWS15: &str = "news15";
pub const ID_FILES: &str = "files";

/// Every panel whose content belongs to one connection — the set a connection
/// switch has to swap.
///
/// All six of them, today. There is no global panel yet: the transfer queue is
/// the one that wants to be (M6), and the Tracker is already outside the dock
/// entirely, being a standalone window rather than a panel.
pub const PER_CONNECTION: &[&str] = &[ID_CHAT, ID_USERS, ID_TASKS, ID_NEWS, ID_NEWS15, ID_FILES];

extern "C" {
    fn gtkhx_dock_raise_if_open(id: *const c_char) -> glib::ffi::gboolean;
    fn gtkhx_dock_is_embedded(id: *const c_char) -> glib::ffi::gboolean;
    fn gtkhx_dock_set_needs_attention(id: *const c_char, state: glib::ffi::gboolean);
    fn gtkhx_dock_embed(
        id: *const c_char,
        kind: i32,
        area: i32,
        title: *const c_char,
        icon_name: *const c_char,
        page: *const c_char,
        content: *mut gtk4::ffi::GtkWidget,
    ) -> glib::ffi::gboolean;
    fn gtkhx_dock_add_page(
        id: *const c_char,
        page: *const c_char,
        content: *mut gtk4::ffi::GtkWidget,
    ) -> glib::ffi::gboolean;
    fn gtkhx_dock_has_page(id: *const c_char, page: *const c_char) -> glib::ffi::gboolean;
    fn gtkhx_dock_show_page(id: *const c_char, page: *const c_char) -> glib::ffi::gboolean;

    /// `gtkhx_ui_bridge.c` — the connection a session owns.
    fn gtkhx_session_htlc(sess: *mut c_void) -> *mut c_void;
    /// `gtkhx-core` — that connection's process-unique serial. NULL reads as 0.
    fn hx_conn_serial(htlc: *const c_void) -> u16;
    /// `session_registry.c` — the session the user is looking at. NULL before
    /// the first one exists.
    fn hx_active_session() -> *mut c_void;
}

/// The dock page name for a session's content: its connection's serial.
///
/// The serial is process-unique and fixed for the connection's life, which is
/// what a page name has to be — a page outlives any particular server address
/// or nickname, and reusing a name across two connections would silently make
/// one connection's content unreachable.
///
/// A NULL session reads as `"0"`, the serial no connection is assigned. That
/// is a real answer rather than a failure: the caller ends up building a page
/// nothing will ever switch to, which is visible and inert, where a panic or a
/// NULL deref would be neither.
pub fn page_for_session(sess: *mut c_void) -> String {
    let serial = unsafe { hx_conn_serial(gtkhx_session_htlc(sess)) };
    serial.to_string()
}

/// TRUE iff a panel with `id` was already registered (re-attached + raised,
/// so the caller returns early instead of rebuilding its content).
pub fn raise_if_open(id: &str) -> bool {
    let cid = crate::cs(id);
    unsafe { gtkhx_dock_raise_if_open(cid.as_ptr()) != glib::ffi::GFALSE }
}

/// Whether `id` names a panel in the dock. No side effects — see the note on
/// `gtkhx_dock_is_embedded` for why this and `raise_if_open` both exist.
pub fn is_embedded(id: &str) -> bool {
    let cid = crate::cs(id);
    unsafe { gtkhx_dock_is_embedded(cid.as_ptr()) != glib::ffi::GFALSE }
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

/// What a window entry point should do about opening role `id` for `sess`.
pub enum Open {
    /// Nothing. Either this connection's content is already up — now the
    /// visible page, panel raised — or the role is spoken for by another
    /// connection and this one can't have it yet.
    Done,
    /// Build content and hand it to [`place`] under this page name.
    Build(String),
}

/// The head of all six window entry points: decide whether to build.
///
/// Three answers folded into two, because the caller does the same thing with
/// two of them:
///
/// 1. This connection already has a page here → show it, raise the panel, done.
/// 2. Another connection owns the role's still-singleton content module →
///    raise the panel so the click does *something*, and don't build. See
///    [`claim_singleton`].
/// 3. Otherwise → build.
///
/// The build-once test used to be panel-level, and asking "is this *panel*
/// open?" answered yes as soon as any connection had content in it — so a
/// second connection returned early here and never built a page of its own.
/// That was a build-order bug rather than a UI decision, which is why the fix
/// belongs at the head of every entry point rather than in one of them.
pub fn open(id: &str, sess: *mut c_void) -> Open {
    let page = page_for_session(sess);
    let cid = crate::cs(id);
    let cpage = crate::cs(&page);

    unsafe {
        if gtkhx_dock_has_page(cid.as_ptr(), cpage.as_ptr()) != glib::ffi::GFALSE {
            gtkhx_dock_show_page(cid.as_ptr(), cpage.as_ptr());
            gtkhx_dock_raise_if_open(cid.as_ptr());
            return Open::Done;
        }
        if !claim_singleton(id, &page) {
            // Raise anyway: the panel exists, it just belongs to someone else.
            // Doing nothing at all would make the toolbar button look broken,
            // which is a worse way to discover a missing feature than seeing
            // the wrong connection's content with the connection tab to
            // explain it.
            gtkhx_dock_raise_if_open(cid.as_ptr());
            return Open::Done;
        }
    }
    Open::Build(page)
}

/// Put `content` in panel `id` as this connection's page, building the panel
/// itself if this is the first connection to want it. Titled `title_key`
/// (translated) with `icon_name`, at `kind` / `area` — all four ignored when
/// the panel already exists, since a panel's chrome belongs to the role, not
/// to whichever connection happened to open it first.
///
/// Returns TRUE on success. `content` is consumed either way: embedded on
/// success, destroyed by the bridge on failure. Don't touch it after the
/// call; skip any post-embed work when this returns FALSE.
pub fn place(
    id: &str,
    page: &str,
    kind: i32,
    area: i32,
    title_key: &str,
    icon_name: &str,
    content: *mut gtk4::ffi::GtkWidget,
) -> bool {
    let cid = crate::cs(id);
    let cpage = crate::cs(page);

    unsafe {
        if gtkhx_dock_is_embedded(cid.as_ptr()) != glib::ffi::GFALSE {
            // Another connection got here first, so this is a second page in
            // the panel it built.
            if gtkhx_dock_add_page(cid.as_ptr(), cpage.as_ptr(), content) == glib::ffi::GFALSE {
                return false;
            }
            // Show it only if it belongs to the connection the user is
            // looking at. Content gets built on connect and login, not only
            // when someone clicks — so a background server finishing its
            // login must not yank all six panels over to itself while the
            // user is reading another one. A page added behind the current
            // one is exactly right in that case; the connection tab is what
            // advertises it.
            if page == page_for_session(hx_active_session()) {
                gtkhx_dock_show_page(cid.as_ptr(), cpage.as_ptr());
                gtkhx_dock_raise_if_open(cid.as_ptr());
            }
            return true;
        }

        let ctitle = crate::cs(&tr(title_key));
        let cicon = crate::cs(icon_name);
        gtkhx_dock_embed(
            cid.as_ptr(),
            kind,
            area,
            ctitle.as_ptr(),
            cicon.as_ptr(),
            cpage.as_ptr(),
            content,
        ) != glib::ffi::GFALSE
    }
}

/// May `page` build content for role `id`, given that the module behind it is
/// still a process singleton?
///
/// TRUE for the first connection to ask, FALSE for every other one, for as
/// long as the first is around.
///
/// This is a guard against a hazard the per-connection build-once test
/// *created*. The old panel-level test answered "already built" as soon as any
/// connection had content, which incidentally stopped a second connection
/// reaching a content module at all. Keying on the connection removed that
/// accident — correctly, since it was also what stopped a second connection
/// having its own page — and what it exposed is that four of the six modules
/// keep their state in one process-global slot. Reaching them twice does not
/// produce two independent views; it overwrites the first connection's, leaks
/// its signal handlers, and leaves whichever page is destroyed first nulling
/// the other's state.
///
/// So a refusal here is the behaviour the doc already described: a second
/// connection gets no page in that panel and switching to it leaves the first
/// connection's content up. Visibly incomplete, rather than quietly corrupt.
///
/// **Every entry in the table below is M4g's worklist.** As each module grows
/// per-connection state, its call to this goes away — and when the last one
/// does, so does this function.
pub fn claim_singleton(id: &str, page: &str) -> bool {
    use std::cell::RefCell;
    use std::collections::HashMap;

    thread_local! {
        /// role → the connection page that owns it.
        static OWNER: RefCell<HashMap<String, String>> = RefCell::new(HashMap::new());
    }

    OWNER.with(|o| {
        let mut owner = o.borrow_mut();
        match owner.get(id) {
            Some(held) if held != page => false,
            Some(_) => true,
            None => {
                owner.insert(id.to_owned(), page.to_owned());
                true
            }
        }
    })
}

/// Make `page` the visible one in panel `id`, for a caller switching every
/// per-connection panel at once. FALSE when that connection has no content in
/// this panel, which is a no-op rather than an error — see the note in
/// `conn_tabs` about what that leaves on screen.
pub fn show_page(id: &str, page: &str) -> bool {
    let cid = crate::cs(id);
    let cpage = crate::cs(page);
    unsafe { gtkhx_dock_show_page(cid.as_ptr(), cpage.as_ptr()) != glib::ffi::GFALSE }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The singleton claim is first-come, and idempotent for the holder.
    ///
    /// No display needed — it is a plain map — so this is an ordinary
    /// `#[test]`, unlike the widget checks in `crate::gtk_tests`.
    #[test]
    fn a_role_belongs_to_the_first_connection_that_asks() {
        // Role names local to this test: the thread_local is per-test-thread,
        // but using the real ids would still couple this to whatever order
        // another test in the same thread ran in.
        assert!(claim_singleton("role-a", "1"), "first claim refused");
        assert!(
            claim_singleton("role-a", "1"),
            "the holder was refused its own role — a rebuild would be blocked"
        );
        assert!(
            !claim_singleton("role-a", "2"),
            "a second connection was let into a singleton module"
        );

        // Roles are independent: losing one doesn't lose the rest.
        assert!(claim_singleton("role-b", "2"), "unrelated role refused");
        assert!(!claim_singleton("role-b", "1"));
    }
}
