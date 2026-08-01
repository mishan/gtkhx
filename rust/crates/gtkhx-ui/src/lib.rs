//! `gtkhx-ui` — window UIs in gtk4-rs.
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
mod hl_date;
mod tr;
// Shared wrapper over the C dock-embed bridge, used by every docked-window
// shell (Users, Tasks, …). See dock_bridge.c.
mod dock;

pub mod about;
pub mod agreement;
pub mod bookmark_store;
pub mod bookmarks;
pub mod chat_find;
pub mod cipher_vocab;
pub mod connect;
// the Settings form. 10 of 11 pages are Rust and wired live via the
// gtkhx_options_rs_page_* exports (options.c's create_options_window +
// settings_entries[] call them). Identity + Voice remain C.
pub mod options;
pub mod rc4_dialog;
pub mod tls_trust_dialog;
pub mod tracker;
pub mod useredit;
// HxUserRow — the users/chat list row model GObject (was
// users_row.c). Exports the hx_user_row_* C ABI users_view.c links against.
pub mod user_row;
// HxUserListView — the GtkColumnView-backed user list. Exports the
// hx_user_list_view_* C ABI users.c / chat.c link against; the custom Name
// cell stays C behind FFI (users_cell.c).
pub mod users_view;
// the user-list voice-indicator column (was users_voice_col.c).
// Behind the `voice` feature; a NULL stub otherwise. Exports
// gtkhx_users_voice_column_new that users_view.rs appends.
pub mod users_voice_col;
// the per-chat voice toolbar (was voice_panel.c). Wholly behind the
// `voice` feature — its C callers (chat.c / users_bridge.c / gtkutil.c) are
// #ifdef HAVE_VOICE, so no voice-off stub is needed. Exports voice_panel_*.
#[cfg(feature = "voice")]
pub mod voice_panel;
// the push-to-talk key controller (was voice_ptt.c). Wholly behind the
// `voice` feature — its C caller (toolbar.c) is #ifdef HAVE_VOICE. The pure
// key-spec vocabulary stays C (voice_ptt_keyspec.c). Exports hx_voice_ptt_attach.
#[cfg(feature = "voice")]
pub mod voice_ptt;
// the voice-chat wire-out senders (was voice.c) moved to their own
// lean crate `hxvoice-send` (cargo-testable; native hotline-proto builders).
// The sibling voice modules reach hx_send_voice_* through their existing
// externs, resolved at the final C link against that staticlib.
// the Users window shell (raise + dock registration + lifecycle).
// The custom view widget + action-button handlers stay C behind
// users_bridge.c / dock_bridge.c; create_users_window is now this module's
// #[no_mangle] export.
pub mod users;
// the Tasks window shell (raise + dock registration + lifecycle).
// The task list content + transfer-model coupling stay C in tasks.c;
// create_tasks_window is now this module's #[no_mangle] export.
pub mod tasks;
// the News window shell (1.0/1.2 flat news). Dock registration via
// dock_bridge; the news content (viewers, search, fetch/post RPC) stays C
// in news.c. create_news_window is now this module's #[no_mangle] export.
pub mod news;
// the News browser (1.5 threaded news, CENTER) window shell. Dock
// registration via dock_bridge; the browser content + fetch/RPC stay C in
// news_browser.c. create_news_browser_window is this module's export.
pub mod news_browser;
// the Chat window shell (public chat, CENTER; hosts the pchat/PM tabs).
// Dock registration via dock_bridge; chat content + xtext + wire senders stay
// C in chat.c. create_chat_window is this module's #[no_mangle] export.
pub mod chat;
// The Chat panel's internal AdwTabView tab strip (was chat_tabs.c). Exports the
// gtkhx_chat_tabs_* C ABI chat.c / msg.c / users.c / gtkutil.c + chat.rs use.
pub mod chat_tabs;
// The chat / pchat input GtkTextView key handler (was chat_input_key_pressed).
// Exports gtkhx_chat_input_attach; keeps tab_nick_comp in C.
pub mod chat_input;
// The incoming chat-invitation dialog (was output_chat_invitation +
// chat_invite_response). Join/Decline via the hxhandlers::send::chat Rust senders.
pub mod chat_invite;
// The C ABI over the session-owned HxMemberModel +
// the M1 nick completion (hx_member_model_* / hx_nick_complete).
// HxConversation — the Rust per-chat model that replaces the C struct chat
// (cid + subject + owned HxMemberModel + opaque view pointer).
// Private Message content — create_msgwin builds the PM tab's content
// tree (output frame, input + emoji, recipient info pane) in gtk4-rs around
// the C create_msg model/leaf widgets. The msgwin struct + chat_tabs + wire
// senders stay C.
pub mod msg;
// the Files browser window shell (two-panel browser, CENTER). Dock
// registration via dock_bridge; the browser content + DnD + providers +
// transfer integration stay C in files_browser.c. open_files_browser is this
// module's #[no_mangle] export.
pub mod files;
// Private Chat content — create_pchat_window builds the pchat tab's
// content tree (subject bar, xtext output, input, user sidebar) in gtk4-rs
// around the C gtkhx_pchat_new leaf widgets + gtkhx_pchat_user_sidebar.
pub mod pchat;
// the inline-media click-to-view dialog. Builds the
// AdwDialog (Loading → image → error stack) + Save-As / Open-Externally
// handlers in gtk4-rs; the download state machine (inline_media_download.c)
// and glycin decoder (inline_media_decode.c / hx-image-decode) stay C behind
// the FFI seam. inline_media_show_dialog is this module's #[no_mangle] export.
pub mod inline_media_dialog;
// the Get-User-Info result window (was gtkhx.c::output_user_info). A
// small read-only text window; fired from the user-info GtkhxSession signal.
// output_user_info is this module's #[no_mangle] export.
pub mod user_info;
// the File "Get Info" dialog (was files.c's output_file_info + Save/date
// helpers); fired from the file-info GtkhxSession signal. Dates format natively
// (hl_date), the Save button sends FILE_SETINFO natively (hotline_proto).
// output_file_info is this module's #[no_mangle] export.
pub mod file_info;
// the Create-Post composer (was news.c's post window). A modal
// GtkTextView + Cancel/Post header; the wire sender hx_post_news stays C.
// create_post_window is this module's #[no_mangle] export.
pub mod create_post;
// the 1.5 news-browser create (new folder / category) + delete
// confirmation dialogs (were news_browser.c's open_create_dialog +
// on_delete_clicked bodies). AdwAlertDialogs over the hxhandlers::send::news senders;
// C keeps the selection logic + the refresh bridge. Exports
// gtkhx_news_create_dialog_open / gtkhx_news_delete_dialog_open.
pub mod news_dialogs;
// the 1.5 news-browser tree view: the GtkTreeListModel child-model
// function + the GtkListView row factory (setup/bind/unbind + lazy
// fetch-on-expand), was news_browser.c's factory callbacks. Exports
// gtkhx_news_build_tree_model / gtkhx_news_build_factory.
pub mod news_tree;
// the 1.5 news-browser compose window (New Post / Reply), was
// news_browser.c's open_compose_window + build_reply_context_panel. Modal
// GtkWindow over hxhandlers::send::news's post_thread sender. Exports
// gtkhx_news_compose_open.
pub mod news_compose;
// the 1.5 news-browser right-pane post rendering + selection
// breadcrumb (was news_browser.c's render_selected_post + update_breadcrumb).
// The C functions stay as thin delegators. Exports gtkhx_news_render_post /
// gtkhx_news_update_breadcrumb.
pub mod news_render;
// the emoji picker button + inline `:shortcode:` typeahead (was
// emoji.c). GTK wiring only — the shortcode match list comes from
// hotline-proto. Exports hx_emoji_button_new / _typeahead_attach / _detach.
pub mod emoji;
// the Broadcast composer + wire sender (was toolbar.c's broadcast
// dialog + msg.c's hx_send_broadcast). AdwAlertDialog + AdwEntryRow; the wire
// build is native hotline-proto, the text encoder / task / write stay C.
// gtkhx_broadcast_dialog_open is this module's #[no_mangle] export.
pub mod broadcast;
// the server banner surface + URL / HTXF fetch state machines (was banner.c +
// banner_dispatch.c). Decode / connection-state / TLS-verify are native Rust;
// the hxnet fetch + tokio worker spawn + send/task stay on the C ABI.
pub mod banner;

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
