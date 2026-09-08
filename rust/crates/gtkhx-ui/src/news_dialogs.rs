//! `news_dialogs` — the 1.5 news-browser create + delete dialogs (were
//! `news_browser.c::open_create_dialog` / `create_response` and the
//! `on_delete_clicked` confirm dialog).
//!
//! Both are `AdwAlertDialog`s. The C side keeps the selection logic (the
//! toolbar button handlers pick the target node); it hands us either the
//! parent node (create) or a snapshot of the target's identity (delete), and
//! we own the dialog + the wire send + the post-send refresh.
//!
//! The senders are the Rust `hxhandlers::send::news` ones (`hx_news15_mkdir` / `mkcat` /
//! `delete` / `delete_thread`); none has a reply handler, so we re-fetch the
//! affected listing ourselves via `gtkhx_news_browser_refresh` once the RPC is
//! away. Create refreshes just the parent folder (targeted, tree state mostly
//! preserved); delete refreshes from the root — the model has no path→node
//! index yet, so locating the affected parent cheaply isn't doable, and a root
//! refetch is correct if heavier.

use std::ffi::{c_char, c_int, c_void};

use adw::prelude::*;
use glib::translate::from_glib_none;
use gtk::glib;
use gtk4 as gtk;
use libadwaita as adw;

use crate::tr::tr;

// Mirror of the NB_KIND_* enum in news_browser.c.
const NB_KIND_FOLDER: c_int = 1;
const NB_KIND_CATEGORY: c_int = 2;
const NB_KIND_POST: c_int = 3;

use hxhandlers::send::news::{
    hx_news15_delete, hx_news15_delete_thread, hx_news15_mkcat, hx_news15_mkdir,
};

// The node's Hotline path (NULL for a pathless node) comes straight from the
// hxmodel crate — it is Rust, and a hand-written `extern "C"` block for it only
// removed the compiler's ability to check the signature.
use hxmodel::news::node::hx_news_node_path;

extern "C" {
    // gtkhx_ui_bridge.c — the active session's htlc (single-conn today).
    fn gtkhx_active_htlc() -> *mut c_void;
    // gtkutil.c — Ctrl+W / Esc close accelerators on a dialog.
    fn gtkhx_dialog_add_close_shortcuts(dialog: *mut gtk::ffi::GtkWidget);
    // news_browser.c — re-fetch a listing after a create/delete (the server
    // doesn't push notifications for these). `node` NULL means refresh the root.
    fn gtkhx_news_browser_refresh(node: *mut c_void);
}

/// Present the "New News Folder" / "New News Category" dialog.
///
/// `parent` is the folder to create inside (an `HxNewsNode *`, or NULL for the
/// root); `kind` is `NB_KIND_FOLDER` or `NB_KIND_CATEGORY`. `parent_window` is
/// the browser window the dialog is presented over.
///
/// # Safety
/// C-ABI entry from a toolbar handler on the GTK main thread. `parent` is NULL
/// or a valid `HxNewsNode *`; `parent_window` is a valid `GtkWidget *`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_news_create_dialog_open(
    parent_window: *mut gtk::ffi::GtkWidget,
    parent: *mut c_void,
    kind: c_int,
) {
    crate::ensure_gtk_init();

    if kind != NB_KIND_FOLDER && kind != NB_KIND_CATEGORY {
        return;
    }

    let (title, body, create_label) = if kind == NB_KIND_FOLDER {
        (
            tr("New News Folder"),
            tr("Enter a name for the new news folder."),
            tr("C_reate Folder"),
        )
    } else {
        (
            tr("New News Category"),
            tr("Enter a name for the new news category."),
            tr("C_reate Category"),
        )
    };

    let dialog = adw::AlertDialog::new(Some(&title), Some(&body));
    dialog.add_response("cancel", &tr("_Cancel"));
    dialog.add_response("create", &create_label);
    dialog.set_response_appearance("create", adw::ResponseAppearance::Suggested);
    dialog.set_default_response(Some("create"));
    dialog.set_close_response("cancel");

    gtkhx_dialog_add_close_shortcuts(dialog.as_ptr() as *mut gtk::ffi::GtkWidget);

    let entry = gtk::Entry::new();
    entry.set_activates_default(true);
    dialog.set_extra_child(Some(&entry));

    // Keep the parent node alive for the dialog's lifetime — the C original
    // reffed it too. AdwAlertDialog fires "response" exactly once (the close
    // response on dismiss), so the matching unref happens there.
    if !parent.is_null() {
        glib::gobject_ffi::g_object_ref(parent as *mut glib::gobject_ffi::GObject);
    }

    dialog.connect_response(None, move |_dlg, response| unsafe {
        if response == "create" {
            let text = entry.text();
            let text = text.as_str();
            if !text.is_empty() {
                let parent_path = if parent.is_null() {
                    "/".to_string()
                } else {
                    crate::cstr(hx_news_node_path(parent.cast()))
                };
                let htlc = gtkhx_active_htlc();
                // Both senders take (parent path, new name): the server
                // resolves the path as an existing folder and creates the name
                // inside it. Folder creation used to join the two and send one
                // path, which asked the server to resolve a folder that did not
                // exist yet — ENOENT, every time.
                if let (Ok(p), Ok(n)) = (
                    std::ffi::CString::new(parent_path.clone()),
                    std::ffi::CString::new(text),
                ) {
                    if kind == NB_KIND_FOLDER {
                        hx_news15_mkdir(htlc, p.as_ptr(), n.as_ptr());
                    } else {
                        hx_news15_mkcat(htlc, p.as_ptr(), n.as_ptr());
                    }
                }
                // Settle: re-fetch the parent's listing so the new item shows.
                gtkhx_news_browser_refresh(parent);
            }
        }
        if !parent.is_null() {
            glib::gobject_ffi::g_object_unref(parent as *mut glib::gobject_ffi::GObject);
        }
    });

    let parent_win: Option<gtk::Widget> = if parent_window.is_null() {
        None
    } else {
        Some(from_glib_none(parent_window))
    };
    dialog.present(parent_win.as_ref());
}

/// Present the delete-confirmation dialog for a folder / category / post.
///
/// The C side snapshots the target's identity at click time rather than holding
/// a node ref across the dialog: a held ref keeps the node alive but doesn't
/// stop the `GListStore` dropping its ref during a refresh, which clears the
/// path pointer in flight (→ NULL to `path_to_hldir`, crash). So we take
/// `kind` / `name` / `path` / `postid` by value.
///
/// # Safety
/// C-ABI entry from the delete toolbar handler on the GTK main thread.
/// `name` / `path` are NUL-terminated C strings; `parent_window` is a valid
/// `GtkWidget *`.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_news_delete_dialog_open(
    parent_window: *mut gtk::ffi::GtkWidget,
    kind: c_int,
    name: *const c_char,
    path: *const c_char,
    postid: u32,
) {
    crate::ensure_gtk_init();

    let name = crate::cstr(name);
    // Adwaita idiom: the question is the heading, the affected item's name is
    // the body. (The C original packed both into one "%s"-formatted body with a
    // literal "Delete" heading; keeping the name out of the translated string
    // avoids a placeholder, same as the chat-invitation dialog.)
    let (heading, delete_label) = match kind {
        NB_KIND_FOLDER => (
            tr("Delete this folder and all its contents?"),
            tr("_Delete Folder"),
        ),
        NB_KIND_CATEGORY => (
            tr("Delete this category and all its posts?"),
            tr("_Delete Category"),
        ),
        NB_KIND_POST => (tr("Delete this post?"), tr("_Delete Post")),
        _ => return,
    };
    let body_text = format!("\u{201c}{name}\u{201d}");

    // Own the path snapshot for the deferred send.
    let path_owned: Option<std::ffi::CString> = if path.is_null() {
        None
    } else {
        Some(std::ffi::CStr::from_ptr(path).to_owned())
    };

    let dialog = adw::AlertDialog::new(Some(&heading), Some(&body_text));
    dialog.add_response("cancel", &tr("_Cancel"));
    dialog.add_response("delete", &delete_label);
    dialog.set_response_appearance("delete", adw::ResponseAppearance::Destructive);
    dialog.set_default_response(Some("cancel"));
    dialog.set_close_response("cancel");

    gtkhx_dialog_add_close_shortcuts(dialog.as_ptr() as *mut gtk::ffi::GtkWidget);

    dialog.connect_response(None, move |_dlg, response| unsafe {
        if response == "delete" {
            if let Some(p) = path_owned.as_ref() {
                let htlc = gtkhx_active_htlc();
                if kind == NB_KIND_POST {
                    hx_news15_delete_thread(htlc, p.as_ptr(), postid);
                } else {
                    hx_news15_delete(htlc, p.as_ptr());
                }
                // Root refresh — heavier but correct without a path→node index.
                gtkhx_news_browser_refresh(std::ptr::null_mut());
            }
        }
    });

    let parent_win: Option<gtk::Widget> = if parent_window.is_null() {
        None
    } else {
        Some(from_glib_none(parent_window))
    };
    dialog.present(parent_win.as_ref());
}
