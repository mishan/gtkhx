//! File "Get Info" dialog.
//!
//! The reply to HTLC_HDR_FILE_GETINFO: a small window showing a file's name
//! (editable), creator / type / size / created / modified (read-only), and its
//! comment (editable). Fired from the `file-info` GtkhxSession signal — the
//! `on_file_info_signal` adapter in gtkhx.c calls this `#[no_mangle]` export, so
//! the C side links unchanged.
//!
//! Everything the dialog needs is native Rust now: the two Hotline date stamps
//! format through [`crate::hl_date::format_wire`] (no raw-bytes → C round-trip),
//! and the Save button sends FILE_SETINFO through `hxhandlers::send::files`.
//! What stays on the C ABI is leaf glue: the active-connection accessor and
//! `human_size`.

use std::ffi::{c_char, c_int, c_void, CStr};

use adw::prelude::*;
use gtk::glib;
use gtk4 as gtk;
use libadwaita as adw;

use crate::ffi as cffi;
use crate::tr::tr;

extern "C" {
    // session_registry.c — the connection a key names, if it is still open.
    fn hx_session_with_serial(serial: u16) -> *mut c_void;
    fn gtkhx_session_htlc(sess: *mut c_void) -> *mut c_void;
    fn hx_conn_fd(htlc: *const c_void) -> c_int;
    // human_readable.c — fileutils-vintage byte-count string into `sizstr`,
    // returning a pointer into it (may be right-justified).
    fn human_size(sizstr: *mut c_char, size: u64) -> *mut c_char;
}

/// Format a Hotline 8-byte wire date stamp for display, or `None` for the
/// no-timestamp sentinel (rendered as an em-dash by the caller). `%c` matches
/// the old C `output_file_info` (locale full date + time).
unsafe fn format_date(bytes: *const u8) -> Option<String> {
    if bytes.is_null() {
        return None;
    }
    let s = std::slice::from_raw_parts(bytes, 8);
    crate::hl_date::format_wire(s, "%c")
}

/// Human-readable size string: `"1.2M (1258291 bytes)"` for >= 1 KiB, else
/// `"512 bytes"`. Empty for a zero size (the row renders an em-dash).
fn size_string(size: u64) -> String {
    if size == 0 {
        return String::new();
    }
    let mut buf = [0 as c_char; 64];
    let human = unsafe {
        let p = human_size(buf.as_mut_ptr(), size);
        CStr::from_ptr(p).to_string_lossy().into_owned()
    };
    if size >= 1024 {
        format!("{human} ({size} {})", tr("bytes"))
    } else {
        format!("{size} {}", tr("bytes"))
    }
}

/// One read-only metadata row: title = field name, subtitle = value (em-dash
/// when empty), selectable for copy.
fn info_row(title: &str, value: &str) -> adw::ActionRow {
    let row = adw::ActionRow::new();
    row.set_title(title);
    row.set_subtitle(if value.is_empty() { "—" } else { value });
    row.set_subtitle_selectable(true);
    row
}

/// Send FILE_SETINFO for a rename + comment edit (was `set_name_comment`).
/// `path` is the file's full path, as the server knows it; `rename` the name
/// the user typed, when it changed; `comments` the dialog's comment.
unsafe fn save_file_info(
    conn: crate::dock::ConnKey,
    path: &[u8],
    rename: Option<&str>,
    comments: &str,
) {
    // To the server the file is on, not the one in focus. The dialog can
    // outlive that connection (left open across a disconnect or a closed tab).
    let sess = hx_session_with_serial(conn);
    if sess.is_null() {
        return;
    }
    let htlc = gtkhx_session_htlc(sess);
    if htlc.is_null() || hx_conn_fd(htlc) == 0 {
        return;
    }
    hxhandlers::send::files::set_info(htlc, path, rename, comments);
}

/// `void output_file_info(struct htlc_conn *htlc, char *path, char *name,
/// char *creator, char *type, char *comments, const guint8 *date_modify,
/// const guint8 *date_create, guint64 size)` — present the File Info window
/// for a file on `htlc`.
///
/// # Safety
/// C-ABI signal handler on the main thread. `path` is an owned (`g_malloc`'d)
/// string this fn takes over + frees; the other string args are borrowed for the
/// duration of the call; `date_*` point at 8 wire bytes each (or NULL).
#[no_mangle]
pub unsafe extern "C" fn output_file_info(
    htlc: *mut c_void,
    path: *mut c_char,
    name: *const c_char,
    creator: *const c_char,
    type_: *const c_char,
    comments: *const c_char,
    date_modify: *const u8,
    date_create: *const u8,
    size: u64,
) {
    crate::ensure_gtk_init();
    let conn = crate::dock::conn_key(htlc);

    // `path` ownership transfers here (the signal passes it as a raw pointer and
    // the receive handler doesn't free it on success). Copy it for the dialog's
    // lifetime and free the C buffer now.
    let path_bytes = if path.is_null() {
        Vec::new()
    } else {
        let s = CStr::from_ptr(path).to_bytes().to_vec();
        glib::ffi::g_free(path as *mut c_void);
        s
    };
    let name_str = crate::cstr(name);
    let creator_str = crate::cstr(creator);
    let type_str = crate::cstr(type_);
    let comments_str = crate::cstr(comments);
    let created = format_date(date_create).unwrap_or_default();
    let modified = format_date(date_modify).unwrap_or_default();

    let window = gtk::Window::new();
    window.set_title(Some(&tr("File Info")));
    window.set_default_size(460, 540);

    // AdwHeaderBar with a Save action on the trailing edge.
    let header = adw::HeaderBar::new();
    let savebtn = gtk::Button::with_label(&tr("Save"));
    savebtn.add_css_class("suggested-action");
    header.pack_end(&savebtn);
    window.set_titlebar(Some(&header));

    let vbox = gtk::Box::new(gtk::Orientation::Vertical, 18);
    vbox.set_margin_start(12);
    vbox.set_margin_end(12);
    vbox.set_margin_top(12);
    vbox.set_margin_bottom(12);

    // Name (editable).
    let name_group = adw::PreferencesGroup::new();
    let name_entry = adw::EntryRow::new();
    name_entry.set_title(&tr("Name"));
    name_entry.set_text(&name_str);
    name_group.add(&name_entry);
    vbox.append(&name_group);

    // Read-only metadata.
    let info_group = adw::PreferencesGroup::new();
    info_group.add(&info_row(&tr("Creator"), &creator_str));
    info_group.add(&info_row(&tr("Type"), &type_str));
    info_group.add(&info_row(&tr("Size"), &size_string(size)));
    info_group.add(&info_row(&tr("Created"), &created));
    info_group.add(&info_row(&tr("Modified"), &modified));
    vbox.append(&info_group);

    // Comments (editable).
    let comments_group = adw::PreferencesGroup::new();
    comments_group.set_title(&tr("Comments"));
    let comments_text = gtk::TextView::new();
    comments_text.set_wrap_mode(gtk::WrapMode::WordChar);
    comments_text.set_editable(true);
    comments_text.buffer().set_text(&comments_str);
    let comments_scroll = gtk::ScrolledWindow::new();
    comments_scroll.set_policy(gtk::PolicyType::Automatic, gtk::PolicyType::Automatic);
    comments_scroll.set_has_frame(true);
    comments_scroll.set_size_request(-1, 140);
    comments_scroll.set_child(Some(&comments_text));
    comments_group.add(&comments_scroll);
    comments_group.set_vexpand(true);
    vbox.append(&comments_group);

    window.set_child(Some(&vbox));

    // Save: read the editable fields + send FILE_SETINFO. The closure owns
    // clones of the widgets + the path string, so they live as long as the
    // button (i.e. the window) does — no g_object_set_data / manual free.
    let name_for_save = name_entry.clone();
    let comments_for_save = comments_text.clone();
    savebtn.connect_clicked(move |_| {
        let new_name = name_for_save.text().to_string();
        let rename = (new_name != name_str).then_some(new_name.as_str());
        let buf = comments_for_save.buffer();
        let (start, end) = buf.bounds();
        let comments = buf.text(&start, &end, false).to_string();
        unsafe { save_file_info(conn, &path_bytes, rename, &comments) };
    });

    // Esc-close accelerator (same C helper user_info.rs uses); present keeps the
    // mapped toplevel alive after the Rust wrappers drop here.
    cffi::init_keyaccel(window.as_ptr() as *mut cffi::GtkWidget);
    window.present();
}
