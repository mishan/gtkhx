//! File "Get Info" dialog (ported from `src/files.c::output_file_info` + its
//! Save / date-format helpers).
//!
//! The reply to HTLC_HDR_FILE_GETINFO: a small window showing a file's name
//! (editable), creator / type / size / created / modified (read-only), and its
//! comment (editable). Fired from the `file-info` GtkhxSession signal — the
//! `on_file_info_signal` adapter in gtkhx.c calls this `#[no_mangle]` export, so
//! the C side links unchanged.
//!
//! Everything the dialog needs is native Rust now: the two Hotline date stamps
//! format through [`crate::hl_date::format_wire`] (no raw-bytes → C round-trip),
//! and the Save button builds the FILE_SETINFO request with
//! `hotline_proto::build::build_file_setinfo_chunks` + the Rust send primitive.
//! What stays on the C ABI is leaf glue: the active-connection accessor, the
//! path helpers (`dirchar_basename` / `path_to_hldir`), the task table +
//! `hlwrite_chunks`, and `human_size`.

use std::ffi::{c_char, c_void, CStr};

use gtk4 as gtk;
use gtk::glib;
use gtk::prelude::*;
use libadwaita as adw;
use adw::prelude::*;

use hotline_proto::build::{build_file_setinfo_chunks, FileSetInfoRequest, HxChunk};

use crate::ffi as cffi;
use crate::tr::tr;

// HTLC_HDR_FILE_SETINFO (hotline.h) + the text-encoding capability bit.
const HTLC_HDR_FILE_SETINFO: u32 = 0x0000_00cf;
const HTLC_CAP_TEXT_ENCODING: u64 = 0x0002;

/// `rcv_task_fn` — FILE_SETINFO registers a no-reply task (rcv fn NULL).
type RcvTaskFn = unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void);

// Native imports (real Rust crates, type-checked).
use gtkhx_core::conn::hx_conn_has_cap;
use hxtext::gtkhx_text_for_wire;

extern "C" {
    // gtkhx_ui_bridge.c — the active connection.
    fn gtkhx_active_htlc() -> *mut c_void;
    // path helpers (path_util.c / path_hldir.c): current basename (a pointer
    // into `path`) + the wire "hldir" encoding of the parent directory.
    fn dirchar_basename(path: *mut c_char) -> *mut c_char;
    fn path_to_hldir(path: *const c_char, hldirlen: *mut u16, is_file: i32) -> *mut u8;
    // hxtask — register the (no-reply) task + send.
    fn task_new(
        htlc: *mut c_void,
        rcv: Option<RcvTaskFn>,
        ptr: *mut c_void,
        data: *mut c_void,
        str_: *const c_char,
    ) -> *mut c_void;
    fn hlwrite_chunks(htlc: *mut c_void, ty: u32, flag: u32, chunks: *const HxChunk, hc: i32);
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
    let mut buf = [0i8; 64];
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

/// Encode `text` for the wire (Mac Roman / UTF-8 per the negotiated cap) and run
/// `f` with the borrowed encoded bytes; frees the g_malloc'd buffer after. Empty
/// on a NULL encode.
unsafe fn with_wire<R>(text: &str, utf8: bool, is_body: bool, f: impl FnOnce(&[u8]) -> R) -> R {
    let mut len: usize = 0;
    let buf = gtkhx_text_for_wire(
        text.as_ptr() as *const c_char,
        text.len(),
        if utf8 { glib::ffi::GTRUE } else { glib::ffi::GFALSE },
        if is_body { glib::ffi::GTRUE } else { glib::ffi::GFALSE },
        &mut len,
    );
    if buf.is_null() {
        return f(&[]);
    }
    let out = f(std::slice::from_raw_parts(buf as *const u8, len));
    glib::ffi::g_free(buf as *mut c_void);
    out
}

/// Send FILE_SETINFO for a rename + comment edit (was `set_name_comment`).
/// `path` is the file's full path (used for the current basename + parent dir);
/// `new_name` and `comments` are the dialog's editable fields.
unsafe fn save_file_info(path: &str, new_name: &str, comments: &str) {
    let htlc = gtkhx_active_htlc();
    if htlc.is_null() {
        return;
    }
    let utf8 = hx_conn_has_cap(htlc.cast(), HTLC_CAP_TEXT_ENCODING) != glib::ffi::GFALSE;

    // Current basename (a pointer into a mutable copy of `path`), and whether the
    // file lives under a directory (→ include the HTLC_DATA_DIR chunk).
    let mut path_buf: Vec<c_char> = path
        .bytes()
        .map(|b| b as c_char)
        .chain(std::iter::once(0))
        .collect();
    let base_ptr = dirchar_basename(path_buf.as_mut_ptr());
    let has_dir = base_ptr != path_buf.as_mut_ptr();
    let base = CStr::from_ptr(base_ptr).to_string_lossy().into_owned();

    // Optional wire "hldir" for the parent directory (g_malloc'd → g_free below).
    let mut hldirlen: u16 = 0;
    let hldir = if has_dir {
        path_to_hldir(path_buf.as_ptr(), &mut hldirlen, 1)
    } else {
        std::ptr::null_mut()
    };
    let hldir_slice: Option<&[u8]> = if hldir.is_null() {
        None
    } else {
        Some(std::slice::from_raw_parts(hldir, hldirlen as usize))
    };

    // Encode name (current basename), rename (typed name), comment; build the
    // chunk array referencing the borrowed encoded bytes, then send. The nested
    // with_wire closures keep every wire buffer alive across the send.
    with_wire(&base, utf8, false, |name_w| {
        with_wire(new_name, utf8, false, |rename_w| {
            with_wire(comments, utf8, true, |comment_w| {
                let req = FileSetInfoRequest {
                    name: name_w,
                    rename: rename_w,
                    comment: Some(comment_w),
                    dir: hldir_slice,
                };
                let mut chunks = [HxChunk::EMPTY; 4];
                let hc = build_file_setinfo_chunks(&req, &mut chunks);
                if hc > 0 {
                    let label = crate::cs("set file info");
                    task_new(htlc, None, std::ptr::null_mut(), std::ptr::null_mut(), label.as_ptr());
                    hlwrite_chunks(htlc, HTLC_HDR_FILE_SETINFO, 0, chunks.as_ptr(), hc as i32);
                }
            })
        })
    });

    if !hldir.is_null() {
        glib::ffi::g_free(hldir as *mut c_void);
    }
}

/// `void output_file_info(char *path, char *name, char *creator, char *type,
/// char *comments, const guint8 *date_modify, const guint8 *date_create,
/// guint64 size)` — present the File Info window.
///
/// # Safety
/// C-ABI signal handler on the main thread. `path` is an owned (`g_malloc`'d)
/// string this fn takes over + frees; the other string args are borrowed for the
/// duration of the call; `date_*` point at 8 wire bytes each (or NULL).
#[no_mangle]
pub unsafe extern "C" fn output_file_info(
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

    // `path` ownership transfers here (the signal passes it as a raw pointer and
    // the receive handler doesn't free it on success). Copy it for the dialog's
    // lifetime and free the C buffer now.
    let path_str = if path.is_null() {
        String::new()
    } else {
        let s = CStr::from_ptr(path).to_string_lossy().into_owned();
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
        let buf = comments_for_save.buffer();
        let (start, end) = buf.bounds();
        let comments = buf.text(&start, &end, false).to_string();
        unsafe { save_file_info(&path_str, &new_name, &comments) };
    });

    // Esc-close accelerator (same C helper user_info.rs uses); present keeps the
    // mapped toplevel alive after the Rust wrappers drop here.
    cffi::init_keyaccel(window.as_ptr() as *mut cffi::GtkWidget);
    window.present();
}
