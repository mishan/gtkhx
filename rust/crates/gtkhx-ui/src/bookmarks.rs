//! Bookmarks management dialog (ported from `src/bookmarks.c`).
//!
//! A standalone `GtkWindow` with a `GtkPaned`: a `GtkListBox` sidebar of
//! saved bookmarks on the left, an `AdwPreferencesPage` detail form on the
//! right. Exports the C ABI entry point `create_bookmarks_window` its
//! deleted `src/bookmarks.c` used to provide, so the toolbar links
//! unchanged.
//!
//! On-disk CRUD delegates to the byte-identical `hx_bookmark_*` API
//! (`bookmarks_io.c`, still C, Tier-1 tested) — one source of truth for
//! the wire-compatible HTsc format. The cipher / compression picker
//! vocabulary comes from the sibling [`crate::cipher_vocab`] module (shared
//! with [`crate::connect`]); the stable on-disk cipher byte from
//! `bookmark_cipher.c`.
//!
//! Live widgets + selection state live in `thread_local!`s mirroring the
//! old file-static `BookmarksWindow`. Handlers are plain functions that
//! snapshot the widgets out of a short borrow before touching them — the
//! same discipline the Connect dialog uses, so a signal-emitting setter
//! can't re-enter a held borrow.

use crate::cipher_vocab;
use crate::ffi::{self as cffi, BOOKMARK_CIPHER_BYTE_RC4};
use crate::tr::{tr, tr1, tr_fmt};
use crate::{cs, cstr};
use gtk4 as gtk;
use libadwaita as adw;

use adw::prelude::*;
use gtk::gdk;
use gtk::glib;
use std::cell::{Cell, RefCell};
use std::ffi::c_char;
use std::os::raw::c_void;

/// The dialog's live widgets (mirrors bookmarks.c's `BookmarksWindow`
/// pointer fields). Cheap to clone — every field is a refcounted GObject
/// handle.
#[derive(Clone)]
struct BmWidgets {
    window: gtk::Window,
    list_box: gtk::ListBox,
    save_btn: gtk::Button,
    delete_btn: gtk::Button,
    name_row: adw::EntryRow,
    server_row: adw::EntryRow,
    port_row: adw::EntryRow,
    login_row: adw::EntryRow,
    pass_row: adw::PasswordEntryRow,
    hope_row: adw::SwitchRow,
    tls_row: adw::SwitchRow,
    cipher_row: adw::ComboRow,
    compress_row: adw::ComboRow,
    empty_status: adw::StatusPage,
    detail_form: gtk::Box,
}

thread_local! {
    static UI: RefCell<Option<BmWidgets>> = const { RefCell::new(None) };
    /// row-selected handler id, so we can block it around programmatic
    /// selection changes (rebuild / new / clear) and avoid the
    /// load_selection re-entrancy the C code guards the same way.
    static ROW_ID: RefCell<Option<glib::SignalHandlerId>> = const { RefCell::new(None) };
    /// TRUE when a loadable bookmark is bound to the form (Save enabled).
    static BOUND: Cell<bool> = const { Cell::new(false) };
    /// The on-disk name of the currently-selected bookmark. Distinct from
    /// the Name field's text once the user types a rename; None for a
    /// new-unsaved bookmark. Also set (with BOUND still false) for a
    /// corrupt / legacy file so Delete can still clear it.
    static ORIGINAL_NAME: RefCell<Option<String>> = const { RefCell::new(None) };
}

/// Snapshot the live widgets (short borrow) so callers operate on them
/// without holding the thread-local across signal-emitting setters.
fn widgets() -> Option<BmWidgets> {
    UI.with_borrow(|u| u.clone())
}

fn bound() -> bool {
    BOUND.with(|b| b.get())
}
fn set_bound(v: bool) {
    BOUND.with(|b| b.set(v));
}
fn original_name() -> Option<String> {
    ORIGINAL_NAME.with_borrow(|o| o.clone())
}
fn set_original_name(v: Option<String>) {
    ORIGINAL_NAME.with_borrow_mut(|o| *o = v);
}

/// The active toplevel, for dialog transient-parenting. May be None.
fn active_window() -> Option<gtk::Window> {
    let ap = unsafe { cffi::gtkhx_active_window() };
    if ap.is_null() {
        None
    } else {
        Some(unsafe { glib::translate::from_glib_none(ap) })
    }
}

// ======================================================================
// On-disk plumbing (delegates to bookmarks_io.c)
// ======================================================================

/// A loaded bookmark's fields, copied out of the C `HxBookmark`. `compress`
/// is a dropdown index; `cipher` is a stable bookmark byte.
struct BmData {
    name: String,
    server: String,
    port: String,
    login: String,
    pass: String,
    secure: u8,
    compress: u8,
    cipher: u8,
    tls: u8,
}

/// Load `name` via hx_bookmark_load (None on missing / corrupt / legacy).
fn load_bm(name: &str) -> Option<BmData> {
    unsafe {
        let p = cffi::hx_bookmark_load(cs(name).as_ptr());
        if p.is_null() {
            return None;
        }
        let d = BmData {
            name: cstr((*p).name as *const c_char),
            server: cstr((*p).server.as_ptr()),
            port: cstr((*p).port.as_ptr()),
            login: cstr((*p).login.as_ptr()),
            pass: cstr((*p).pass.as_ptr()),
            secure: (*p).secure as u8,
            compress: (*p).compress as u8,
            cipher: (*p).cipher as u8,
            tls: (*p).tls as u8,
        };
        cffi::hx_bookmark_free(p);
        Some(d)
    }
}

/// Collation-sorted bookmark filenames (primary + legacy dir), via
/// hx_bookmark_list.
fn list_bookmark_names() -> Vec<String> {
    let mut out = Vec::new();
    unsafe {
        let head = cffi::hx_bookmark_list();
        let mut l = head;
        while !l.is_null() {
            let data = (*l).data as *mut c_void;
            if !data.is_null() {
                out.push(cstr(data as *const c_char));
                glib::ffi::g_free(data);
            }
            l = (*l).next;
        }
        if !head.is_null() {
            glib::ffi::g_list_free(head);
        }
    }
    out
}

/// Take ownership of a `GError` out-param message, freeing it. Returns a
/// placeholder when NULL (matches the C "(unknown)" fallback).
unsafe fn take_gerror(err: *mut cffi::GError) -> String {
    if err.is_null() {
        "(unknown)".to_string()
    } else {
        let s = cstr((*err).message);
        glib::ffi::g_error_free(err);
        s
    }
}

fn bm_rename(old: &str, new: &str) -> Result<(), String> {
    unsafe {
        let mut err: *mut cffi::GError = std::ptr::null_mut();
        if cffi::hx_bookmark_rename(cs(old).as_ptr(), cs(new).as_ptr(), &mut err)
            != glib::ffi::GFALSE
        {
            Ok(())
        } else {
            Err(take_gerror(err))
        }
    }
}

fn bm_delete(name: &str) -> Result<(), String> {
    unsafe {
        let mut err: *mut cffi::GError = std::ptr::null_mut();
        if cffi::hx_bookmark_delete(cs(name).as_ptr(), &mut err) != glib::ffi::GFALSE {
            Ok(())
        } else {
            Err(take_gerror(err))
        }
    }
}

/// Copy `s` (truncated) + NUL into a fixed C char buffer, zeroing the rest.
fn set_cfield(field: &mut [c_char], s: &str) {
    let bytes = s.as_bytes();
    let n = bytes.len().min(field.len().saturating_sub(1));
    for (i, slot) in field.iter_mut().enumerate() {
        *slot = if i < n { bytes[i] as c_char } else { 0 };
    }
}

/// Build a C `HxBookmark` from the current form and persist it under
/// `name` via hx_bookmark_save. Returns the save error message on failure.
fn save_form_as(w: &BmWidgets, name: &str) -> Result<(), String> {
    unsafe {
        let bm = cffi::hx_bookmark_new();
        if bm.is_null() {
            return Err(tr("Out of memory"));
        }
        (*bm).name = glib::ffi::g_strdup(cs(name).as_ptr());
        set_cfield(&mut (*bm).server, w.server_row.text().as_str());
        set_cfield(&mut (*bm).port, w.port_row.text().as_str());
        set_cfield(&mut (*bm).login, w.login_row.text().as_str());
        set_cfield(&mut (*bm).pass, w.pass_row.text().as_str());
        (*bm).secure = w.hope_row.is_active() as c_char;
        // Cipher: dropdown index → stable bookmark byte.
        (*bm).cipher = cipher_vocab::dropdown_to_cipher_byte(w.cipher_row.selected()) as c_char;
        (*bm).compress = w.compress_row.selected() as c_char;
        (*bm).tls = w.tls_row.is_active() as c_char;

        let mut err: *mut cffi::GError = std::ptr::null_mut();
        let ok = cffi::hx_bookmark_save(bm, &mut err);
        cffi::hx_bookmark_free(bm);
        if ok != glib::ffi::GFALSE {
            Ok(())
        } else {
            Err(take_gerror(err))
        }
    }
}

// ======================================================================
// Form <-> widgets
// ======================================================================

/// Clamp a combo-row index to its model, sending anything out of range to 0
/// ("Off" / "NONE") rather than the last item. A corrupt or forward-format
/// bookmark byte shouldn't silently select a real cipher/compression entry
/// (whose notify handler could then force HOPE on) — matches the Connect
/// dialog's "invalid compress byte → 0" handling.
fn clamp_combo(combo: &adw::ComboRow, idx: u32) -> u32 {
    let n = combo.model().map(|m| m.n_items()).unwrap_or(0);
    if n == 0 || idx >= n {
        0
    } else {
        idx
    }
}

fn form_from_bookmark(w: &BmWidgets, d: &BmData) {
    w.name_row.set_text(&d.name);
    w.server_row.set_text(&d.server);
    w.port_row.set_text(&d.port);
    w.login_row.set_text(&d.login);
    w.pass_row.set_text(&d.pass);
    w.hope_row.set_active(d.secure != 0);
    w.tls_row.set_active(d.tls != 0);
    // Cipher uses the stable bookmark vocabulary; translate to the live
    // dropdown index. An unknown byte (e.g. RC4) maps to 0 ("Off").
    // clamp_combo still wraps a corrupt / forward-format byte past the
    // model length back into range.
    let cipher_idx = clamp_combo(&w.cipher_row, cipher_vocab::cipher_byte_to_dropdown(d.cipher));
    w.cipher_row.set_selected(cipher_idx);
    let compress_idx = clamp_combo(&w.compress_row, d.compress as u32);
    w.compress_row.set_selected(compress_idx);
    sync_sensitivity();
}

// ======================================================================
// HOPE / TLS / cipher / compress coupling (mirrors connect.rs)
// ======================================================================

/// Grey out cipher / compress when HOPE is off; grey out HOPE + both when
/// TLS is on (and force HOPE off underneath — the two transports are
/// mutually exclusive). Same rule the Connect dialog enforces.
fn sync_sensitivity() {
    let Some(w) = widgets() else { return };
    if w.tls_row.is_active() {
        if w.hope_row.is_active() {
            w.hope_row.set_active(false);
        }
        w.hope_row.set_sensitive(false);
        w.cipher_row.set_sensitive(false);
        w.compress_row.set_sensitive(false);
        return;
    }
    w.hope_row.set_sensitive(true);
    let on = w.hope_row.is_active();
    w.cipher_row.set_sensitive(on);
    w.compress_row.set_sensitive(on);
}

/// TLS-on flips the default port 5500 → 5600; TLS-off flips it back. Custom
/// ports are left alone. Then re-sync the grey-out coupling.
fn on_tls_toggled() {
    let Some(w) = widgets() else { return };
    let portstr = w.port_row.text();
    if w.tls_row.is_active() {
        if portstr == "5500" {
            w.port_row.set_text("5600");
        }
    } else if portstr == "5600" {
        w.port_row.set_text("5500");
    }
    sync_sensitivity();
}

/// A non-Off cipher / compress selection with HOPE off forces HOPE on —
/// silent plaintext-with-a-cipher is the wrong failure mode.
fn on_secure_combo_selected(selected: u32) {
    let Some(w) = widgets() else { return };
    if selected != 0 && !w.hope_row.is_active() {
        w.hope_row.set_active(true);
    }
}

// ======================================================================
// Empty / detail state toggle
// ======================================================================

fn show_empty_state(w: &BmWidgets) {
    w.detail_form.set_visible(false);
    w.empty_status.set_visible(true);
    w.save_btn.set_sensitive(false);
    w.delete_btn.set_sensitive(false);
}

fn show_detail_state(w: &BmWidgets) {
    w.empty_status.set_visible(false);
    w.detail_form.set_visible(true);
    w.save_btn.set_sensitive(true);
    w.delete_btn.set_sensitive(true);
}

// ======================================================================
// Selection handling
// ======================================================================

/// The bookmark filename backing a list row (== the row title we set in
/// make_list_row).
fn row_bookmark_name(row: &gtk::ListBoxRow) -> Option<String> {
    row.clone()
        .downcast::<adw::PreferencesRow>()
        .ok()
        .map(|r| r.title().to_string())
}

fn load_selection() {
    let Some(w) = widgets() else { return };
    set_bound(false);
    set_original_name(None);

    let Some(row) = w.list_box.selected_row() else {
        show_empty_state(&w);
        return;
    };
    let Some(name) = row_bookmark_name(&row) else {
        show_empty_state(&w);
        return;
    };

    let mut data = match load_bm(&name) {
        Some(d) => d,
        None => {
            // Legacy-format or corrupt file. Surface the error and leave
            // the form empty, but record the name so Delete still works
            // (otherwise the user can't clear a broken entry). Save stays
            // disabled — there's no loaded bookmark to write back.
            let msg = tr1(
                "Could not load bookmark \"%s\". The file may be in the legacy format \
                 — pick it from the toolbar's Connect-button dropdown to convert it \
                 first.",
                &name,
            );
            toast_error(&w, &msg);
            set_original_name(Some(name));
            show_empty_state(&w);
            w.delete_btn.set_sensitive(true);
            return;
        }
    };

    // RC4 migration: prompt for a replacement before showing the form, so
    // the user can't accidentally Save back with cipher=0 ("no cipher"),
    // silently turning a previously-encrypted bookmark into plaintext. The
    // dialog rewrites the file in place; reload to pick up the new byte.
    if data.secure != 0 && data.cipher == BOOKMARK_CIPHER_BYTE_RC4 {
        let parent = w.window.as_ptr() as *mut cffi::GtkWindow;
        let new_byte = crate::rc4_dialog::run_sync(parent, &name);
        if new_byte < 0 {
            show_empty_state(&w);
            return;
        }
        match load_bm(&name) {
            Some(d) => data = d,
            None => {
                show_empty_state(&w);
                return;
            }
        }
    }

    set_bound(true);
    set_original_name(Some(data.name.clone()));
    form_from_bookmark(&w, &data);
    show_detail_state(&w);
}

// ======================================================================
// List build / rebuild
// ======================================================================

fn make_list_row(name: &str) -> adw::ActionRow {
    let row = adw::ActionRow::new();
    row.set_title(name);
    row
}

fn clear_list(lb: &gtk::ListBox) {
    while let Some(child) = lb.first_child() {
        lb.remove(&child);
    }
}

fn block_row_selected(lb: &gtk::ListBox) {
    ROW_ID.with_borrow(|id| {
        if let Some(id) = id {
            lb.block_signal(id);
        }
    });
}

fn unblock_row_selected(lb: &gtk::ListBox) {
    ROW_ID.with_borrow(|id| {
        if let Some(id) = id {
            lb.unblock_signal(id);
        }
    });
}

/// Rebuild the sidebar list, re-selecting `select_name` if present. The
/// row-selected handler is blocked across the whole rebuild — we don't
/// want it firing for the spurious intermediate NULL selection when
/// clear_list drops the selected row, nor for GTK_SELECTION_BROWSE's
/// auto-pick of the new head. load_selection is driven once, explicitly,
/// at the end.
fn rebuild_list(w: &BmWidgets, select_name: Option<&str>) {
    block_row_selected(&w.list_box);
    clear_list(&w.list_box);

    let mut to_select: Option<adw::ActionRow> = None;
    for nm in list_bookmark_names() {
        let row = make_list_row(&nm);
        w.list_box.append(&row);
        if select_name == Some(nm.as_str()) {
            to_select = Some(row);
        }
    }
    if let Some(r) = &to_select {
        w.list_box.select_row(Some(r.upcast_ref::<gtk::ListBoxRow>()));
    }

    unblock_row_selected(&w.list_box);
    load_selection();
}

// ======================================================================
// Button handlers
// ======================================================================

fn on_save() {
    let Some(w) = widgets() else { return };
    if !bound() {
        return;
    }
    let raw_name = w.name_row.text().to_string();
    if raw_name.is_empty() {
        toast_error(&w, &tr("Bookmark name cannot be empty."));
        return;
    }

    // Canonicalize: hx_bookmark_save defangs '/' to '\\' on disk, so keep
    // the in-memory name and the on-disk filename in lockstep (otherwise a
    // later rebuild+select misses).
    let safe_name = unsafe {
        let p = cffi::hx_bookmark_safe_filename(cs(&raw_name).as_ptr());
        if p.is_null() {
            None
        } else {
            let s = cstr(p);
            glib::ffi::g_free(p as *mut c_void);
            Some(s)
        }
    };
    let Some(safe_name) = safe_name.filter(|s| !s.is_empty()) else {
        toast_error(&w, &tr("Bookmark name cannot be empty."));
        return;
    };
    if raw_name != safe_name {
        w.name_row.set_text(&safe_name);
    }

    // Rename first, if the name changed. Two failure modes to defend:
    //   1. Rename fails — bail before touching anything else.
    //   2. Rename succeeds, then save fails — the file now sits at the new
    //      name with OLD contents (rename(2) just moved the inode). Roll
    //      the rename back so the list row keeps pointing at the same file
    //      with the same contents it had before Save.
    let orig = original_name();
    let mut renamed_from: Option<String> = None;
    match &orig {
        Some(o) if *o != safe_name => {
            if let Err(msg) = bm_rename(o, &safe_name) {
                toast_error(&w, &tr1("Rename failed: %s", &msg));
                return;
            }
            renamed_from = Some(o.clone());
            set_original_name(Some(safe_name.clone()));
        }
        None => {
            // New bookmark — its name is the user-supplied one.
            set_original_name(Some(safe_name.clone()));
        }
        _ => {}
    }

    match save_form_as(&w, &safe_name) {
        Ok(()) => {
            unsafe { cffi::toolbar_refresh_bookmarks() };
            rebuild_list(&w, Some(&safe_name));
        }
        Err(save_err) => {
            if let Some(from) = &renamed_from {
                match bm_rename(&safe_name, from) {
                    Ok(()) => {
                        set_original_name(Some(from.clone()));
                        toast_error(
                            &w,
                            &tr1(
                                "Save failed: %s. The bookmark was restored to its \
                                 original name.",
                                &save_err,
                            ),
                        );
                    }
                    Err(rb) => {
                        toast_error(
                            &w,
                            &tr_fmt(
                                "Save failed: %1$s. The file is now at \"%2$s\" — with \
                                 the previous contents — rollback also failed (%3$s).",
                                &[save_err.as_str(), safe_name.as_str(), rb.as_str()],
                            ),
                        );
                    }
                }
            } else {
                toast_error(&w, &tr1("Save failed: %s", &save_err));
            }
        }
    }
}

fn on_new() {
    let Some(w) = widgets() else { return };

    // A fresh, unsaved bookmark: name empty (so the user types into a clean
    // field rather than saving a literal "New Bookmark"), HOPE on, no
    // cipher/compress yet. original_name stays None → Save takes the
    // new-file branch, not rename.
    set_bound(true);
    set_original_name(None);

    // Clear the list selection so a later row click doesn't reload behind
    // our back. Block our handler across unselect_all: GtkListBox emits
    // row-selected synchronously with row=NULL, and load_selection would
    // wipe the state we just set.
    block_row_selected(&w.list_box);
    w.list_box.unselect_all();
    unblock_row_selected(&w.list_box);

    let defaults = BmData {
        name: String::new(),
        server: String::new(),
        port: String::new(),
        login: String::new(),
        pass: String::new(),
        secure: 1,
        compress: 0,
        cipher: 0,
        tls: 0,
    };
    form_from_bookmark(&w, &defaults);
    show_detail_state(&w);
    w.name_row.grab_focus();
    w.name_row.select_region(0, -1);
}

fn on_delete() {
    let Some(w) = widgets() else { return };
    let Some(orig) = original_name() else { return };

    let body = tr1("Delete the bookmark \"%s\"? This cannot be undone.", &orig);
    let dialog = adw::AlertDialog::new(Some(&tr("Delete Bookmark")), Some(&body));
    dialog.add_response("cancel", &tr("_Cancel"));
    dialog.add_response("delete", &tr("_Delete"));
    dialog.set_response_appearance("delete", adw::ResponseAppearance::Destructive);
    dialog.set_default_response(Some("cancel"));
    dialog.set_close_response("cancel");
    dialog.connect_response(None, move |_, resp| {
        if resp == "delete" {
            on_delete_confirmed();
        }
    });
    dialog.present(Some(w.window.upcast_ref::<gtk::Widget>()));
}

fn on_delete_confirmed() {
    let Some(w) = widgets() else { return };
    let Some(orig) = original_name() else { return };
    match bm_delete(&orig) {
        Ok(()) => {
            unsafe { cffi::toolbar_refresh_bookmarks() };
            rebuild_list(&w, None);
        }
        Err(msg) => toast_error(&w, &tr1("Delete failed: %s", &msg)),
    }
}

fn toast_error(w: &BmWidgets, msg: &str) {
    let dialog = adw::AlertDialog::new(Some(&tr("Bookmark error")), Some(msg));
    dialog.add_response("ok", &tr("_OK"));
    dialog.set_default_response(Some("ok"));
    dialog.set_close_response("ok");
    dialog.present(Some(w.window.upcast_ref::<gtk::Widget>()));
}

// ======================================================================
// Window construction
// ======================================================================

/// A combo model: a translated "Off" row at index 0, then the vocabulary.
fn combo_model(items: &[&str]) -> gtk::StringList {
    let list = gtk::StringList::new(&[]);
    list.append(&tr("Off"));
    for it in items {
        list.append(it);
    }
    list
}

/// `void create_bookmarks_window(void)` — open (or focus) the dialog.
#[no_mangle]
pub extern "C" fn create_bookmarks_window() {
    if let Some(w) = widgets() {
        w.window.present();
        return;
    }
    build_window();
}

fn on_close() {
    // Disconnect the row-selected handler before dropping our refs. GTK
    // tears the window down by unparenting children, and unparenting the
    // selected row makes the list_box emit row-selected with row=NULL.
    // (load_selection already no-ops once UI is cleared, but disconnecting
    // first matches the C and avoids any teardown-time work.)
    if let Some(w) = widgets() {
        ROW_ID.with_borrow_mut(|id| {
            if let Some(id) = id.take() {
                w.list_box.disconnect(id);
            }
        });
    }
    UI.with_borrow_mut(|u| *u = None);
    ROW_ID.with_borrow_mut(|r| *r = None);
    set_bound(false);
    set_original_name(None);
}

fn build_window() {
    crate::ensure_gtk_init();

    let window = gtk::Window::new();
    window.set_title(Some(&tr("Bookmarks")));
    window.set_default_size(760, 680);
    if let Some(parent) = active_window() {
        window.set_transient_for(Some(&parent));
    }
    window.connect_close_request(|_| {
        on_close();
        glib::Propagation::Proceed
    });

    let header = adw::HeaderBar::new();
    window.set_titlebar(Some(&header));

    // Esc + Ctrl-W close (same pattern as the User Editor).
    let shortcuts = gtk::ShortcutController::new();
    shortcuts.set_propagation_phase(gtk::PropagationPhase::Capture);
    shortcuts.set_scope(gtk::ShortcutScope::Local);
    shortcuts.add_shortcut(gtk::Shortcut::new(
        Some(gtk::KeyvalTrigger::new(gdk::Key::Escape, gdk::ModifierType::empty())),
        Some(gtk::NamedAction::new("window.close")),
    ));
    shortcuts.add_shortcut(gtk::Shortcut::new(
        Some(gtk::KeyvalTrigger::new(gdk::Key::w, gdk::ModifierType::CONTROL_MASK)),
        Some(gtk::NamedAction::new("window.close")),
    ));
    window.add_controller(shortcuts);

    // Body: GtkPaned, sidebar left, detail form right.
    let paned = gtk::Paned::new(gtk::Orientation::Horizontal);
    paned.set_position(240);
    paned.set_resize_start_child(false);
    paned.set_shrink_start_child(false);
    window.set_child(Some(&paned));

    // ---- Sidebar ----
    let sidebar_box = gtk::Box::new(gtk::Orientation::Vertical, 0);
    sidebar_box.set_size_request(200, -1);

    let list_box = gtk::ListBox::new();
    list_box.set_selection_mode(gtk::SelectionMode::Browse);
    list_box.add_css_class("navigation-sidebar");

    let scrolled = gtk::ScrolledWindow::new();
    scrolled.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
    scrolled.set_vexpand(true);
    scrolled.set_child(Some(&list_box));
    sidebar_box.append(&scrolled);

    let button_row = gtk::Box::new(gtk::Orientation::Horizontal, 4);
    button_row.set_margin_start(6);
    button_row.set_margin_end(6);
    button_row.set_margin_top(6);
    button_row.set_margin_bottom(6);

    let new_btn = gtk::Button::from_icon_name("list-add-symbolic");
    new_btn.set_tooltip_text(Some(&tr("New bookmark")));
    new_btn.connect_clicked(|_| on_new());
    button_row.append(&new_btn);

    let delete_btn = gtk::Button::from_icon_name("list-remove-symbolic");
    delete_btn.set_tooltip_text(Some(&tr("Delete selected bookmark")));
    delete_btn.add_css_class("destructive-action");
    delete_btn.connect_clicked(|_| on_delete());
    button_row.append(&delete_btn);

    sidebar_box.append(&button_row);
    paned.set_start_child(Some(&sidebar_box));

    // ---- Detail ----
    let detail_box = gtk::Box::new(gtk::Orientation::Vertical, 0);
    detail_box.set_hexpand(true);
    detail_box.set_vexpand(true);
    paned.set_end_child(Some(&detail_box));

    let empty_status = adw::StatusPage::new();
    empty_status.set_icon_name(Some("user-bookmarks-symbolic"));
    empty_status.set_title(&tr("No Bookmark Selected"));
    empty_status.set_description(Some(&tr(
        "Pick one from the list, or create a new one with the + button.",
    )));
    empty_status.set_hexpand(true);
    empty_status.set_vexpand(true);
    detail_box.append(&empty_status);

    let detail_form = gtk::Box::new(gtk::Orientation::Vertical, 0);
    detail_form.set_hexpand(true);
    detail_form.set_vexpand(true);

    let page = adw::PreferencesPage::new();
    let group = adw::PreferencesGroup::new();
    group.set_title(&tr("Bookmark"));

    let name_row = adw::EntryRow::new();
    name_row.set_title(&tr("Name"));
    group.add(&name_row);

    let server_row = adw::EntryRow::new();
    server_row.set_title(&tr("Server"));
    group.add(&server_row);

    let port_row = adw::EntryRow::new();
    port_row.set_title(&tr("Port (blank = 5500)"));
    group.add(&port_row);

    let login_row = adw::EntryRow::new();
    login_row.set_title(&tr("Login"));
    group.add(&login_row);

    let pass_row = adw::PasswordEntryRow::new();
    pass_row.set_title(&tr("Password"));
    group.add(&pass_row);

    // TLS first — toggling it greys out HOPE + cipher + compress (and
    // forces HOPE off underneath), and flips the default port 5500 ⇄ 5600,
    // the same coupling the Connect dialog enforces (see sync_sensitivity /
    // on_tls_toggled).
    let tls_row = adw::SwitchRow::new();
    tls_row.set_title(&tr("Use TLS"));
    tls_row.set_subtitle(&tr(
        "Connect to the server's TLS port. Disables HOPE and compression — \
         they're not meaningful over a TLS-encrypted stream.",
    ));
    tls_row.connect_active_notify(|_| on_tls_toggled());
    group.add(&tls_row);

    let hope_row = adw::SwitchRow::new();
    hope_row.set_title(&tr("HOPE (encrypted handshake)"));
    hope_row.connect_active_notify(|_| sync_sensitivity());
    group.add(&hope_row);

    let cipher_row = adw::ComboRow::new();
    cipher_row.set_title(&tr("Cipher"));
    cipher_row.set_model(Some(&combo_model(cipher_vocab::VALID_CIPHERS)));
    cipher_row.connect_selected_notify(|c| on_secure_combo_selected(c.selected()));
    group.add(&cipher_row);

    let compress_row = adw::ComboRow::new();
    compress_row.set_title(&tr("Compression"));
    compress_row.set_model(Some(&combo_model(cipher_vocab::VALID_COMPRESSORS)));
    compress_row.connect_selected_notify(|c| on_secure_combo_selected(c.selected()));
    group.add(&compress_row);

    page.add(&group);
    page.set_vexpand(true);
    detail_form.append(&page);

    // Save button below the form.
    let save_row = gtk::Box::new(gtk::Orientation::Horizontal, 0);
    save_row.set_halign(gtk::Align::End);
    save_row.set_margin_start(12);
    save_row.set_margin_end(12);
    save_row.set_margin_top(4);
    save_row.set_margin_bottom(12);
    let save_btn = gtk::Button::with_label(&tr("Save"));
    save_btn.add_css_class("suggested-action");
    save_btn.add_css_class("pill");
    save_btn.connect_clicked(|_| on_save());
    save_row.append(&save_btn);
    detail_form.append(&save_row);

    detail_form.set_visible(false);
    detail_box.append(&detail_form);

    let w = BmWidgets {
        window: window.clone(),
        list_box: list_box.clone(),
        save_btn,
        delete_btn,
        name_row,
        server_row,
        port_row,
        login_row,
        pass_row,
        hope_row,
        tls_row,
        cipher_row,
        compress_row,
        empty_status,
        detail_form,
    };
    UI.with_borrow_mut(|u| *u = Some(w.clone()));

    // Connect row-selected after publishing the widgets (rebuild_list uses
    // block_row_selected, which needs ROW_ID set first).
    let row_id = list_box.connect_row_selected(|_, _| load_selection());
    ROW_ID.with_borrow_mut(|r| *r = Some(row_id));

    rebuild_list(&w, None);
    show_empty_state(&w);

    window.present();
}
