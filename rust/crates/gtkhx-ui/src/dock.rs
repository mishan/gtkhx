//! Shared thin wrapper over the C dock-embed bridge (`dock_bridge.c`).
//!
//! The docked-window Rust shells (Users, Tasks, …) register their content
//! through this — a translation of the type-free `gtkhx_dock_*` C ABI so no
//! Rust module names a dock type. The `kind` / `area` ints mirror
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
pub const ID_VIDEO: &str = "video";

/// Every panel whose content belongs to one connection — the set a connection
/// switch has to swap.
///
/// Files and the threaded news browser are absent because they are not panels:
/// each connection's is a window of its own (see `files/mod.rs` and
/// `news_browser.rs`), closed with the rest of the connection's content in
/// [`gtkhx_dock_remove_session_pages`].
///
/// Tasks is deliberately absent: the transfer queue is one list for the whole
/// application, tagged per row, so switching tabs must leave it alone. See
/// [`GLOBAL_PAGE`] and docs/multi-connection.md, "Global but tagged". The
/// Tracker is absent for a different reason — it is a standalone window rather
/// than a panel at all.
pub const PER_CONNECTION: &[&str] = &[ID_CHAT, ID_USERS, ID_NEWS, ID_VIDEO];

/// The page name a global panel lives under.
///
/// Panels are addressed by page even when there is only ever one, because the
/// dock bridge has no other vocabulary. A name rather than a serial, so it can
/// never collide with a connection's — serials start at 1 and are decimal.
pub const GLOBAL_PAGE: &str = "global";

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
    fn gtkhx_dock_remove_page(id: *const c_char, page: *const c_char) -> glib::ffi::gboolean;

    /// `gtkhx_ui_bridge.c` — the connection a session owns.
    fn gtkhx_session_htlc(sess: *mut c_void) -> *mut c_void;
    /// `gtkhx-core` — that connection's process-unique serial. NULL reads as 0.
    fn hx_conn_serial(htlc: *const c_void) -> u16;
    /// `session_registry.c` — the session the user is looking at. NULL before
    /// the first one exists.
    fn hx_active_session() -> *mut c_void;
    /// `session_registry.c` — the connection a key names, if it is still
    /// open.
    fn hx_conn_with_serial(serial: u16) -> *mut c_void;
    fn hx_conn_fd(htlc: *const c_void) -> std::ffi::c_int;

    /// `gtkutil.c` — what to call a session: the server's name once it has
    /// sent one, else the address. Newly allocated.
    fn hx_session_label(sess: *mut c_void) -> *mut c_char;
    fn gtkhx_get_application() -> *mut gtk4::gio::ffi::GApplication;
    /// `dock_layout.c` — the last size a named window was left at; FALSE
    /// when none was saved.
    fn dock_layout_get_window_size(
        name: *const c_char,
        w: *mut std::ffi::c_int,
        h: *mut std::ffi::c_int,
    ) -> glib::ffi::gboolean;
    fn dock_layout_set_window_size(name: *const c_char, w: std::ffi::c_int, h: std::ffi::c_int);
}

/// Which connection a thing belongs to.
///
/// A connection's serial: process-unique, fixed for its life, and 0 for "no
/// connection". Every per-connection index in this crate is keyed on it — the
/// dock's page names, the chat tab strip's conversation maps, the connection
/// tab strip, and each content module's own state — so they all agree on what
/// "the same connection" means without any of them holding a session pointer
/// they would then have to keep valid.
pub type ConnKey = u16;

/// The connection's key. NULL reads as 0.
pub fn conn_key(htlc: *mut c_void) -> ConnKey {
    unsafe { hx_conn_serial(htlc) }
}

/// The key of the connection a session owns. NULL reads as 0.
pub fn key_for_session(sess: *mut c_void) -> ConnKey {
    unsafe { hx_conn_serial(gtkhx_session_htlc(sess)) }
}

/// The connection `conn` names, or NULL once it has closed.
pub fn htlc_for(conn: ConnKey) -> *mut c_void {
    unsafe { hx_conn_with_serial(conn) }
}

/// What to call a session: the server's name once it has sent one, else the
/// address. Empty when it has neither.
pub fn session_label(sess: *mut c_void) -> String {
    unsafe {
        let p = hx_session_label(sess);
        if p.is_null() {
            return String::new();
        }
        let s = std::ffi::CStr::from_ptr(p).to_string_lossy().into_owned();
        glib::ffi::g_free(p as *mut c_void);
        s
    }
}

/// Make `win` one of a connection's windows of its own (Files, threaded News):
/// the application's, with the app-wide keys, and sized as the last one left
/// under `name`, else `default`.
pub fn adopt_window(win: &libadwaita::Window, name: &'static str, default: (i32, i32)) {
    use glib::translate::from_glib_none;
    use gtk4::prelude::*;

    let cname = crate::cs(name);
    let (mut w, mut h) = default;
    unsafe { dock_layout_get_window_size(cname.as_ptr(), &mut w, &mut h) };
    win.set_default_size(w, h);

    // Registered with the application so it counts as one of its windows
    // (dialogs parent to it, and the app doesn't consider itself idle).
    let app = unsafe { gtkhx_get_application() };
    if !app.is_null() {
        let app: gtk4::gio::Application = unsafe { from_glib_none(app) };
        if let Ok(app) = app.downcast::<gtk4::Application>() {
            app.add_window(win);
        }
    }

    // The app-wide accelerators (Ctrl+W close, Ctrl+Q quit, …) on the window
    // itself: Ctrl+W acts on a GtkWindow, and the content box can't be one.
    unsafe { crate::ffi::init_keyaccel(win.as_ptr() as *mut crate::ffi::GtkWidget) };
    // Push-to-talk while this window has the focus, as the dock's own windows.
    #[cfg(feature = "voice")]
    unsafe {
        crate::voice_ptt::hx_voice_ptt_attach(win.as_ptr() as *mut gtk4::ffi::GtkWidget)
    };

    // Remember the size as the user changes it, for the next one. On every
    // change rather than at close: a connection closing destroys the window
    // without a close-request, and quitting destroys nothing at all. The
    // layout file's save is debounced, so a drag is one write.
    let save_size = move |win: &libadwaita::Window| {
        let (w, h) = win.default_size();
        if w > 0 && h > 0 {
            let name = crate::cs(name);
            unsafe { dock_layout_set_window_size(name.as_ptr(), w, h) };
        }
    };
    win.connect_default_width_notify(save_size);
    win.connect_default_height_notify(save_size);
}

/// The key of the connection the user is looking at.
///
/// The right question for anything driven by the *user*: only the visible
/// page's widgets can receive input, so a button click or a tree expansion
/// always belongs to the focused connection. It is the wrong question for
/// anything driven by the *wire* — a reply belongs to the connection that
/// asked, which may not be the one on screen by the time it lands.
pub fn active_key() -> ConnKey {
    key_for_session(unsafe { hx_active_session() })
}

/// A connection as it is now, for a window that outlives what it acts on:
/// the connection can close, and its tab connect again to another server.
#[derive(Debug, Clone, Copy)]
pub struct Bound {
    conn: ConnKey,
    generation: u32,
}

/// `conn` as it is connected now.
pub fn bind(conn: ConnKey) -> Bound {
    let htlc = unsafe { hx_conn_with_serial(conn) };
    let generation = if htlc.is_null() {
        0
    } else {
        unsafe { gtkhx_core::conn::generation(htlc.cast()) }
    };
    Bound { conn, generation }
}

/// The connection `bound` named, while it is still that connect.
pub fn live_htlc(bound: Bound) -> Option<*mut c_void> {
    unsafe {
        let htlc = hx_conn_with_serial(bound.conn);
        (!htlc.is_null()
            && hx_conn_fd(htlc) != 0
            && gtkhx_core::conn::generation(htlc.cast()) == bound.generation)
            .then_some(htlc)
    }
}

/// The dock page name for a session's content: its connection's key.
///
/// A page name has to be stable for the connection's life — a page outlives
/// any particular server address or nickname, and reusing a name across two
/// connections would silently make one connection's content unreachable.
///
/// A NULL session reads as `"0"`. That is a real answer rather than a failure:
/// the caller ends up building a page nothing will ever switch to, which is
/// visible and inert, where a panic or a NULL deref would be neither.
pub fn page_for_session(sess: *mut c_void) -> String {
    key_for_session(sess).to_string()
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
    /// Nothing to do: this connection's content is already up, now the visible
    /// page with the panel raised.
    Done,
    /// Build content and hand it to [`place`] under this page name.
    Build(String),
}

/// The head of every per-connection window entry point: decide whether to
/// build.
///
/// Two answers: this connection already has a page here — in which case it is
/// now the visible one and the panel is raised — or it doesn't and the caller
/// should build.
///
/// The build-once test used to be panel-level, and asking "is this *panel*
/// open?" answered yes as soon as any connection had content in it — so a
/// second connection returned early here and never built a page of its own.
/// That was a build-order bug rather than a UI decision, which is why the fix
/// belongs at the head of every entry point rather than in one of them.
///
/// It briefly had a third answer — a refusal, for a role whose content module
/// was still a process singleton. Removing the panel-level test had also
/// removed the accident that kept a second connection out of those, and every
/// one of them needed making per-connection before the refusal could go.
pub fn open(id: &str, sess: *mut c_void) -> Open {
    open_page(id, page_for_session(sess))
}

/// [`open`] for a panel that belongs to the whole application rather than to
/// one connection.
///
/// Same two answers, but the page is fixed: the first connection to ask builds
/// it and every one after that gets `Done`. Callers that still have
/// per-connection state to push into a shared panel must do so on `Done` as
/// well as after building — nothing else will, because no page is created for
/// the connections that come later.
pub fn open_global(id: &str) -> Open {
    open_page(id, GLOBAL_PAGE.to_owned())
}

fn open_page(id: &str, page: String) -> Open {
    let cid = crate::cs(id);
    let cpage = crate::cs(&page);

    unsafe {
        if gtkhx_dock_has_page(cid.as_ptr(), cpage.as_ptr()) != glib::ffi::GFALSE {
            gtkhx_dock_show_page(cid.as_ptr(), cpage.as_ptr());
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
            if page == GLOBAL_PAGE || page == page_for_session(hx_active_session()) {
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

/// `void gtkhx_dock_remove_session_pages (session *sess)` — destroy this
/// connection's content page in every per-connection panel.
///
/// A remove, not the switch-away a tab change does: every content module's
/// destroy handler *is* its model-side teardown, so this is what unwinds the
/// connection's view state. A panel that has no page for it is skipped.
///
/// # Safety
/// `sess` is a `session *` or NULL. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_dock_remove_session_pages(sess: *mut c_void) {
    let page = page_for_session(sess);
    let cpage = crate::cs(&page);
    for id in PER_CONNECTION {
        let cid = crate::cs(id);
        gtkhx_dock_remove_page(cid.as_ptr(), cpage.as_ptr());
    }
    // Not pages, but the same connection's content, with the same teardown
    // on destroy.
    crate::files::close_for_session(sess);
    crate::news_browser::close_for_session(sess);
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
