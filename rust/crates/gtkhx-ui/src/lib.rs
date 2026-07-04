//! `gtkhx-ui` — Phase R5 window UIs in gtk4-rs.
//!
//! One module per ported window (`tracker` is the first). Each module
//! exports the C ABI entry points its deleted `src/<window>.c` used to
//! provide (via `#[no_mangle] pub extern "C"`), so the C callers
//! (toolbar.c, gtkhx.c signal adapters, options.c, network.c) link
//! against Rust with no source change. Shared infrastructure lives at
//! the crate root: [`ffi`] (extern decls for the C helpers windows
//! call), [`tr`] (gettext).
//!
//! Everything here runs on the GTK main thread; interior state is held in
//! `thread_local!`s. There is no `Send`/`Sync` surface — the windows
//! never touch worker threads directly (network / transfers already
//! marshal back to the main loop in `hxnet`/C before these entry points
//! fire).

mod ffi;
mod tr;
// Shared wrapper over the C dock-embed bridge, used by every docked-window
// shell (Users, Tasks, …). See dock_bridge.c.
mod dock;

pub mod about;
pub mod agreement;
pub mod bookmarks;
pub mod cipher_vocab;
pub mod connect;
// R5.6: the Settings form. 10 of 11 pages are Rust and wired live via the
// gtkhx_options_rs_page_* exports (options.c's create_options_window +
// settings_entries[] call them). Identity + Voice remain C.
pub mod options;
pub mod rc4_dialog;
pub mod tls_trust_dialog;
pub mod tracker;
pub mod useredit;
// R5.8: HxUserRow — the users/chat list row model GObject (was
// users_row.c). Exports the hx_user_row_* C ABI users_view.c links against.
pub mod user_row;
// R5.9: HxUserListView — the GtkColumnView-backed user list. Exports the
// hx_user_list_view_* C ABI users.c / chat.c link against; the custom Name
// cell stays C behind FFI (users_cell.c).
pub mod users_view;
// R5.18: the user-list voice-indicator column (was users_voice_col.c).
// Behind the `voice` feature; a NULL stub otherwise. Exports
// gtkhx_users_voice_column_new that users_view.rs appends.
pub mod users_voice_col;
// R5.19: the per-chat voice toolbar (was voice_panel.c). Wholly behind the
// `voice` feature — its C callers (chat.c / users_bridge.c / gtkutil.c) are
// #ifdef HAVE_VOICE, so no voice-off stub is needed. Exports voice_panel_*.
#[cfg(feature = "voice")]
pub mod voice_panel;
// R5.21: the push-to-talk key controller (was voice_ptt.c). Wholly behind the
// `voice` feature — its C caller (toolbar.c) is #ifdef HAVE_VOICE. The pure
// key-spec vocabulary stays C (voice_ptt_keyspec.c). Exports hx_voice_ptt_attach.
#[cfg(feature = "voice")]
pub mod voice_ptt;
// R5.22: the voice-chat wire-out senders (was voice.c) moved to their own
// lean crate `hxvoice-send` (cargo-testable; native hotline-proto builders).
// The sibling voice modules reach hx_send_voice_* through their existing
// externs, resolved at the final C link against that staticlib.
// R5.7: the Users window shell (raise + dock registration + lifecycle).
// The custom view widget + action-button handlers stay C behind
// users_bridge.c / dock_bridge.c; create_users_window is now this module's
// #[no_mangle] export.
pub mod users;
// R5.10: the Tasks window shell (raise + dock registration + lifecycle).
// The task list content + transfer-model coupling stay C in tasks.c;
// create_tasks_window is now this module's #[no_mangle] export.
pub mod tasks;
// R5.11: the News window shell (1.0/1.2 flat news). Dock registration via
// dock_bridge; the news content (viewers, search, fetch/post RPC) stays C
// in news.c. create_news_window is now this module's #[no_mangle] export.
pub mod news;
// R5.12: the News browser (1.5 threaded news, CENTER) window shell. Dock
// registration via dock_bridge; the browser content + fetch/RPC stay C in
// news_browser.c. create_news_browser_window is this module's export.
pub mod news_browser;
// R5.13: the Chat window shell (public chat, CENTER; hosts the pchat/PM tabs).
// Dock registration via dock_bridge; chat content + xtext + wire senders stay
// C in chat.c. create_chat_window is this module's #[no_mangle] export.
pub mod chat;
// R5.14: Private Message content — create_msgwin builds the PM tab's content
// tree (output frame, input + emoji, recipient info pane) in gtk4-rs around
// the C create_msg model/leaf widgets. The msgwin struct + chat_tabs + wire
// senders stay C.
pub mod msg;
// R5.16: the Files browser window shell (two-panel browser, CENTER). Dock
// registration via dock_bridge; the browser content + DnD + providers +
// transfer integration stay C in files_browser.c. open_files_browser is this
// module's #[no_mangle] export.
pub mod files;
// R5.15: Private Chat content — create_pchat_window builds the pchat tab's
// content tree (subject bar, xtext output, input, user sidebar) in gtk4-rs
// around the C gtkhx_pchat_new leaf widgets + gtkhx_pchat_user_sidebar.
pub mod pchat;
// R5.17: the inline-media click-to-view dialog (Phase 9.D UI). Builds the
// AdwDialog (Loading → image → error stack) + Save-As / Open-Externally
// handlers in gtk4-rs; the download state machine (inline_media_download.c)
// and glycin decoder (inline_media_decode.c / hx-image-decode) stay C behind
// the FFI seam. inline_media_show_dialog is this module's #[no_mangle] export.
pub mod inline_media_dialog;
// R5.23: the Get-User-Info result window (was gtkhx.c::output_user_info). A
// small read-only text window; fired from the user-info GtkhxSession signal.
// output_user_info is this module's #[no_mangle] export.
pub mod user_info;

/// Tell gtk4-rs that GTK is already initialized.
///
/// The app initializes GTK from C (`gtk_init` / `adw_init`), so gtk4-rs's
/// own init flag was never set — and its widget/model constructors call
/// `assert_initialized_main_thread!()` on that flag, which panics (an
/// abort, since it can't unwind across the FFI) even though GTK is
/// running. Every gtk4-rs construction site reached from a C-ABI entry
/// point calls this first. Safe: `set_initialized()` verifies the real
/// `gtk_is_initialized()` and that we're on the main thread, and
/// early-returns once the binding is marked.
pub(crate) fn ensure_gtk_init() {
    unsafe { gtk4::set_initialized() };
}

/// C `char*` → owned `String` (empty on NULL). UTF-8 lossy.
///
/// # Safety
/// `p` is NULL or a valid NUL-terminated C string.
pub(crate) unsafe fn cstr(p: *const std::ffi::c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        std::ffi::CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

/// `&str` → `CString`, dropping any interior NUL (never fails).
pub(crate) fn cs(s: &str) -> std::ffi::CString {
    std::ffi::CString::new(s).unwrap_or_else(|e| {
        let mut v = e.into_vec();
        v.retain(|&b| b != 0);
        std::ffi::CString::new(v).unwrap()
    })
}
