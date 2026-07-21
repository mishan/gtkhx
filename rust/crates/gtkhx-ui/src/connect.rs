//! Connect dialog + bookmark / hotline:// URL service logic (ported from
//! `src/connect.c`).
//!
//! The dialog is an `AdwDialog` form (server / account / connection groups).
//! The service functions (bookmark menu, direct-connect-by-name, builtin
//! bookmarks, reconnect-last, URL open/save) are re-exported with their C
//! ABI so their callers (toolbar.c, chat.c, msg.c, tray.c, gtkurl.c,
//! gtkhx.c, bookmark_rc4_dialog.c) link unchanged.
//!
//! The cipher / compression picker vocabulary lives in the sibling
//! [`crate::cipher_vocab`] module (shared with [`crate::bookmarks`]); the
//! stable on-disk cipher byte comes from `hxbookmarks::cipher`.
//!
//! Bookmark storage is the single TOML file owned by [`hxbookmarks`], reached
//! through [`crate::bookmark_store`]. Built-in bookmarks are ordinary entries
//! seeded on first run, so there's no separate "builtins" concept here.
//!
//! Widget state lives in a thread-local `ConnectUi` (mirrors the file-static
//! `GtkWidget*`s the C had). Handlers snapshot the widgets they need out of
//! that borrow *before* touching them, because `set_active` / `set_selected`
//! / `set_sensitive` synchronously re-emit notify signals whose handlers
//! re-enter the thread-local — holding the borrow across them would panic.

use crate::cipher_vocab;
use crate::ffi as cffi;
use crate::tr::{tr, tr1};
use crate::{cs, cstr};
use gtk4 as gtk;
use libadwaita as adw;

use adw::prelude::*;
use gtk::gio;
use gtk::glib;
use glib::translate::IntoGlibPtr;
use std::cell::RefCell;
use std::ffi::c_char;
use std::os::raw::{c_int, c_void};

/// `#[repr(C)]` mirror of `HotlineUrlParts` (hotline_url.h).
#[repr(C)]
struct HotlineUrlParts {
    host: [c_char; 128],
    login: [c_char; 33],
    pass: [c_char; 33],
    port: u16,
}

extern "C" {
    // gtkhx_ui_bridge.c — set sess->htlc.{compressalg,cipheralg} + hx_connect.
    fn gtkhx_connect_apply(
        sess: *mut c_void,
        server: *const c_char,
        port: u16,
        login: *const c_char,
        pass: *const c_char,
        secure: c_char,
        compress_name: *const c_char,
        cipher_name: *const c_char,
        tls: c_char,
    );
    // hotline_url.c — hotline:// URL parser.
    fn hotline_url_parse(url: *const c_char, out: *mut HotlineUrlParts) -> glib::ffi::gboolean;
    // gtkhx.c / gtkutil.c / toolbar.c helpers.
    fn error_dialog(title: *const c_char, msg: *const c_char);
    fn toolbar_refresh_bookmarks();
}

use crate::bookmark_store;
use hxbookmarks::{cipher, Bookmark};

/// The connect dialog's live widgets (mirrors connect.c's file statics).
/// Cheap to clone — every field is a refcounted GObject handle.
#[derive(Clone, Default)]
struct ConnectUi {
    window: Option<adw::Dialog>,
    address: Option<adw::EntryRow>,
    login: Option<adw::EntryRow>,
    password: Option<adw::PasswordEntryRow>,
    port: Option<adw::EntryRow>,
    hope: Option<adw::SwitchRow>,
    compress: Option<adw::ComboRow>,
    cipher: Option<adw::ComboRow>,
    tls: Option<adw::SwitchRow>,
}

/// Parameters of the most-recent connect, surviving past hx_htlc_close.
/// `compress` is a dropdown index (0 = NONE); `cipher` is a stable
/// bookmark byte — matching what connect_with_args takes, so the reconnect
/// / dialog-preload round-trip is lossless.
#[derive(Clone)]
struct LastConn {
    server: String,
    port: u16,
    login: String,
    pass: String,
    secure: u8,
    compress: u8,
    cipher: u8,
    tls: u8,
}

thread_local! {
    static UI: RefCell<ConnectUi> = RefCell::new(ConnectUi::default());
    static LAST_CONN: RefCell<Option<LastConn>> = const { RefCell::new(None) };
}

/// Snapshot the live widgets (short borrow) so callers can operate on them
/// without holding the thread-local across signal-emitting setters.
fn widgets() -> ConnectUi {
    UI.with_borrow(|ui| ui.clone())
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
// Actual connect path
// ======================================================================

/// Shared "actually connect" path. `compress` is a dropdown index (0 =
/// NONE, 1..N → valid_compressors[N-1]); `cipher` is a stable bookmark
/// byte. TLS forces HOPE / cipher / compress off (mutually-exclusive
/// transports). Updates the last-connection cache and fires hx_connect.
fn connect_with_args(
    sess: *mut c_void,
    server: &str,
    port: u16,
    login: &str,
    pass: &str,
    secure: u8,
    compress: u8,
    cipher: u8,
    tls: u8,
) {
    let (mut secure, mut compress, mut cipher) = (secure, compress, cipher);
    if tls != 0 {
        secure = 0;
        compress = 0;
        cipher = 0;
    }

    // Resolve the dropdown index / stable byte to HOPE algorithm names.
    // Both the compressor and cipher names are Rust `&str`s (from the
    // vocabulary / hxbookmarks::cipher), so hold their CStrings for the
    // duration of the FFI call. Both are gated on HOPE being on.
    let compress_cs: Option<std::ffi::CString> = if secure != 0 && compress > 0 {
        cipher_vocab::compress_name((compress - 1) as usize)
            .filter(|n| cipher_vocab::valid_compress(n))
            .map(cs)
    } else {
        None
    };
    let compress_name: *const c_char =
        compress_cs.as_ref().map(|c| c.as_ptr()).unwrap_or(std::ptr::null());
    let cipher_cs: Option<std::ffi::CString> = if secure != 0 && cipher != 0 {
        cipher::name(cipher)
            .filter(|n| cipher_vocab::valid_cipher(n))
            .map(cs)
    } else {
        None
    };
    let cipher_name: *const c_char =
        cipher_cs.as_ref().map(|c| c.as_ptr()).unwrap_or(std::ptr::null());

    LAST_CONN.with_borrow_mut(|lc| {
        *lc = Some(LastConn {
            server: server.to_owned(),
            port,
            login: login.to_owned(),
            pass: pass.to_owned(),
            secure,
            compress,
            cipher,
            tls,
        });
    });

    let cserver = cs(server);
    let clogin = cs(login);
    let cpass = cs(pass);
    unsafe {
        gtkhx_connect_apply(
            sess,
            cserver.as_ptr(),
            port,
            clogin.as_ptr(),
            cpass.as_ptr(),
            secure as c_char,
            compress_name,
            cipher_name,
            tls as c_char,
        );
    }
}

/// Reconnect to the most-recently-used server, no dialog. Falls back to
/// opening the Connect dialog if nothing has connected yet this run.
#[no_mangle]
pub extern "C" fn connect_reconnect_last() {
    let lc = LAST_CONN.with_borrow(|l| l.clone());
    match lc {
        Some(lc) if !lc.server.is_empty() => {
            let sess = unsafe { cffi::hx_active_session() };
            connect_with_args(
                sess, &lc.server, lc.port, &lc.login, &lc.pass, lc.secure, lc.compress,
                lc.cipher, lc.tls,
            );
        }
        _ => unsafe {
            create_connect_window(std::ptr::null_mut(), cffi::hx_active_session());
        },
    }
}

// ======================================================================
// Connect dialog form
// ======================================================================

/// Grey out cipher / compress when HOPE is off; grey out HOPE + both when
/// TLS is on (and force HOPE off underneath).
fn hope_coupling_sync_sensitivity() {
    let w = widgets();
    let Some(hope) = w.hope else { return };
    let tls_on = w.tls.as_ref().map(|t| t.is_active()).unwrap_or(false);
    if tls_on {
        if hope.is_active() {
            hope.set_active(false);
        }
        hope.set_sensitive(false);
        if let Some(c) = &w.compress {
            c.set_sensitive(false);
        }
        if let Some(c) = &w.cipher {
            c.set_sensitive(false);
        }
        return;
    }
    hope.set_sensitive(true);
    let on = hope.is_active();
    if let Some(c) = &w.compress {
        c.set_sensitive(on);
    }
    if let Some(c) = &w.cipher {
        c.set_sensitive(on);
    }
}

/// TLS-on flips the default port 5500 → 5600; TLS-off flips it back. Custom
/// ports are left alone.
fn on_tls_active_notify() {
    let w = widgets();
    let (Some(tls), Some(port)) = (w.tls.clone(), w.port.clone()) else {
        return;
    };
    let on = tls.is_active();
    let portstr = port.text();
    if on && portstr == "5500" {
        port.set_text("5600");
    } else if !on && portstr == "5600" {
        port.set_text("5500");
    }
    hope_coupling_sync_sensitivity();
}

/// A non-NONE cipher / compress selection with HOPE off forces HOPE on —
/// silent plaintext-with-a-cipher is the wrong failure mode.
fn on_secure_combo_selected(selected: u32) {
    let w = widgets();
    let Some(hope) = w.hope else { return };
    if selected != 0 && !hope.is_active() {
        hope.set_active(true);
    }
}

/// Fill the form. `compress` is a dropdown index; `cipher` is a stable byte
/// (translated to a dropdown index here). Repairs legacy bookmarks that
/// selected an algorithm but left HOPE off.
fn set_the_entries_impl(
    address: &str,
    login: &str,
    password: &str,
    port: &str,
    secure: u8,
    compress: u8,
    cipher: u8,
    tls: u8,
) {
    let w = widgets();
    if let Some(a) = &w.address {
        a.set_text(if address.is_empty() { "" } else { address });
    }
    if let Some(l) = &w.login {
        l.set_text(if login.is_empty() { "" } else { login });
    }
    if let Some(p) = &w.password {
        p.set_text(if password.is_empty() { "" } else { password });
    }
    if let Some(p) = &w.port {
        p.set_text(if port.is_empty() { "5500" } else { port });
    }

    let secure = if secure == 0 && (compress != 0 || cipher != 0) {
        1
    } else {
        secure
    };

    if let Some(h) = &w.hope {
        h.set_active(secure != 0);
    }
    if let Some(c) = &w.compress {
        // A hand-edited bookmark can hold an arbitrary compress byte, and the
        // model is NONE + N items — a byte past N would select an out-of-range
        // index (GTK warning / undefined selection). Clamp anything outside
        // 0..=N back to 0 (NONE).
        let n = cipher_vocab::VALID_COMPRESSORS.len() as u32;
        let sel = if (compress as u32) <= n { compress as u32 } else { 0 };
        c.set_selected(sel);
    }
    if let Some(c) = &w.cipher {
        // cipher_byte_to_dropdown already maps unknown bytes to 0.
        c.set_selected(cipher_vocab::cipher_byte_to_dropdown(cipher));
    }
    if let Some(t) = &w.tls {
        t.set_active(tls != 0);
    }
    hope_coupling_sync_sensitivity();
}

/// `void set_the_entries(char *address, char *login, char *password,
/// char *port, char secure, char compress, char cipher, char tls)`.
///
/// # Safety
/// String args are NULL or valid C strings.
#[no_mangle]
pub unsafe extern "C" fn set_the_entries(
    address: *mut c_char,
    login: *mut c_char,
    password: *mut c_char,
    port: *mut c_char,
    secure: c_char,
    compress: c_char,
    cipher: c_char,
    tls: c_char,
) {
    set_the_entries_impl(
        &cstr(address),
        &cstr(login),
        &cstr(password),
        &cstr(port),
        secure as u8,
        compress as u8,
        cipher as u8,
        tls as u8,
    );
}

/// `void connect_set_entries(const char *address, const char *login,
/// const char *password, guint16 port)` — set the four basic fields.
///
/// # Safety
/// String args are NULL or valid C strings.
#[no_mangle]
pub unsafe extern "C" fn connect_set_entries(
    address: *const c_char,
    login: *const c_char,
    password: *const c_char,
    port: u16,
) {
    let w = widgets();
    if !address.is_null() {
        if let Some(a) = &w.address {
            a.set_text(&cstr(address));
        }
    }
    if !login.is_null() {
        if let Some(l) = &w.login {
            l.set_text(&cstr(login));
        }
    }
    if !password.is_null() {
        if let Some(p) = &w.password {
            p.set_text(&cstr(password));
        }
    }
    if let Some(p) = &w.port {
        p.set_text(&format!("{port}"));
    }
}

/// The Connect button: read the form, connect, close the dialog.
fn server_connect(sess: *mut c_void) {
    // create_connect_window's `data` is the session to act on; every caller
    // passes hx_active_session(). Fall back to it defensively if NULL, so the
    // button actually connects rather than caching + closing with a no-op.
    let sess = if sess.is_null() {
        unsafe { cffi::hx_active_session() }
    } else {
        sess
    };
    let w = widgets();
    let server = w.address.as_ref().map(|e| e.text().to_string()).unwrap_or_default();
    let login = w.login.as_ref().map(|e| e.text().to_string()).unwrap_or_default();
    let pass = w.password.as_ref().map(|e| e.text().to_string()).unwrap_or_default();
    let portstr = w.port.as_ref().map(|e| e.text().to_string()).unwrap_or_default();
    let secure = w.hope.as_ref().map(|h| h.is_active()).unwrap_or(false) as u8;
    let compress = w.compress.as_ref().map(|c| c.selected()).unwrap_or(0) as u8;
    let cipher = cipher_vocab::dropdown_to_cipher_byte(
        w.cipher.as_ref().map(|c| c.selected()).unwrap_or(0),
    );
    let tls = w.tls.as_ref().map(|t| t.is_active()).unwrap_or(false) as u8;

    // Empty port field keeps the 5500 default; a non-empty field parses
    // atoi-style (invalid → 0 → connect fails loudly), matching connect.c.
    let port: u16 = if portstr.is_empty() {
        5500
    } else {
        atoi_port(&portstr)
    };

    connect_with_args(sess, &server, port, &login, &pass, secure, compress, cipher, tls);

    if let Some(dlg) = w.window {
        dlg.close();
    }
    UI.with_borrow_mut(|ui| *ui = ConnectUi::default());
}

/// Build a NONE-prefixed GtkStringList from a cipher_vocab dropdown model.
fn vocab_string_list(items: &[&str]) -> gtk::StringList {
    let list = gtk::StringList::new(&["NONE"]);
    for it in items {
        list.append(it);
    }
    list
}

/// `void create_connect_window(GtkWidget *btn, gpointer data)` — data is the
/// `session *` the Connect button acts on.
///
/// # Safety
/// `data` is a valid `session *` (or NULL; the button then no-ops safely).
#[no_mangle]
pub unsafe extern "C" fn create_connect_window(_btn: *mut cffi::GtkWidget, data: *mut c_void) {
    // Already open → raise it.
    if let Some(dlg) = widgets().window {
        dlg.present(active_window().as_ref().map(|w| w.upcast_ref::<gtk::Widget>()));
        return;
    }
    crate::ensure_gtk_init();

    let dlg = adw::Dialog::new();
    dlg.set_title(&tr("Connect"));
    dlg.set_content_width(480);
    dlg.set_content_height(680);
    cffi::gtkhx_dialog_add_close_shortcuts(dlg.as_ptr() as *mut cffi::GtkWidget);

    let header = adw::HeaderBar::new();
    let save_btn = gtk::Button::with_label(&tr("Save Bookmark…"));
    save_btn.connect_clicked(|_| save_dialog());
    header.pack_end(&save_btn);
    let connect_btn = gtk::Button::with_label(&tr("Connect"));
    connect_btn.add_css_class("suggested-action");
    connect_btn.connect_clicked(move |_| server_connect(data));
    header.pack_end(&connect_btn);

    let toolbar_view = adw::ToolbarView::new();
    toolbar_view.add_top_bar(&header);

    let content = gtk::Box::new(gtk::Orientation::Vertical, 18);
    content.set_margin_top(18);
    content.set_margin_bottom(18);
    content.set_margin_start(18);
    content.set_margin_end(18);

    // --- Server group ---
    let server_grp = adw::PreferencesGroup::new();
    server_grp.set_title(&tr("Server"));
    server_grp.set_description(Some(&tr(
        "Enter the server address. If you have an account, fill in your login \
         and password below; otherwise leave them blank.",
    )));
    let address = adw::EntryRow::new();
    address.set_title(&tr("Server"));
    address.set_activates_default(true);
    server_grp.add(&address);
    let port = adw::EntryRow::new();
    port.set_title(&tr("Port"));
    port.set_text("5500");
    port.set_activates_default(true);
    server_grp.add(&port);
    content.append(&server_grp);

    // --- Account group ---
    let account_grp = adw::PreferencesGroup::new();
    account_grp.set_title(&tr("Account"));
    let login = adw::EntryRow::new();
    login.set_title(&tr("Login"));
    login.set_activates_default(true);
    account_grp.add(&login);
    let password = adw::PasswordEntryRow::new();
    password.set_title(&tr("Password"));
    password.set_activates_default(true);
    account_grp.add(&password);
    content.append(&account_grp);

    // --- Connection group ---
    let conn_grp = adw::PreferencesGroup::new();
    conn_grp.set_title(&tr("Connection"));

    let tls = adw::SwitchRow::new();
    tls.set_title(&tr("Use TLS"));
    tls.set_subtitle(&tr(
        "Connect to the server's TLS port. Disables HOPE and compression — \
         they're not meaningful over a TLS-encrypted stream.",
    ));
    tls.set_active(false);
    tls.connect_active_notify(|_| on_tls_active_notify());
    conn_grp.add(&tls);

    let hope = adw::SwitchRow::new();
    hope.set_title(&tr("Secure (HOPE)"));
    hope.set_subtitle(&tr("Encrypt and optionally compress the connection"));
    hope.set_active(false);
    hope.connect_active_notify(|_| hope_coupling_sync_sensitivity());
    conn_grp.add(&hope);

    let compress = adw::ComboRow::new();
    compress.set_title(&tr("Compression"));
    compress.set_model(Some(&vocab_string_list(cipher_vocab::VALID_COMPRESSORS)));
    compress.set_selected(0);
    compress.connect_selected_notify(|c| on_secure_combo_selected(c.selected()));
    conn_grp.add(&compress);

    let cipher = adw::ComboRow::new();
    cipher.set_title(&tr("Cipher"));
    cipher.set_model(Some(&vocab_string_list(cipher_vocab::VALID_CIPHERS)));
    cipher.set_selected(0);
    cipher.connect_selected_notify(|c| on_secure_combo_selected(c.selected()));
    conn_grp.add(&cipher);

    content.append(&conn_grp);

    let clamp = adw::Clamp::new();
    clamp.set_child(Some(&content));
    let scrolled = gtk::ScrolledWindow::new();
    scrolled.set_policy(gtk::PolicyType::Never, gtk::PolicyType::Automatic);
    scrolled.set_child(Some(&clamp));
    toolbar_view.set_content(Some(&scrolled));
    dlg.set_child(Some(&toolbar_view));

    connect_btn.set_receives_default(true);
    // Reset the whole ConnectUi on close so every row's strong ref drops (not
    // just the window) and stale entry points (connect_set_entries /
    // set_the_entries) can't mutate a closed dialog's widgets.
    dlg.connect_closed(|_| UI.with_borrow_mut(|ui| *ui = ConnectUi::default()));

    // Publish the widgets before the preload so set_the_entries_impl sees them.
    UI.with_borrow_mut(|ui| {
        *ui = ConnectUi {
            window: Some(dlg.clone()),
            address: Some(address.clone()),
            login: Some(login.clone()),
            password: Some(password.clone()),
            port: Some(port.clone()),
            hope: Some(hope.clone()),
            compress: Some(compress.clone()),
            cipher: Some(cipher.clone()),
            tls: Some(tls.clone()),
        };
    });

    hope_coupling_sync_sensitivity();

    // Pre-populate from the last connection this run (reconnect-after-
    // disconnect UX). Route through set_the_entries_impl so the legacy
    // normalization applies.
    if let Some(lc) = LAST_CONN.with_borrow(|l| l.clone()) {
        if !lc.server.is_empty() {
            set_the_entries_impl(
                &lc.server,
                &lc.login,
                &lc.pass,
                &format!("{}", lc.port),
                lc.secure,
                lc.compress,
                lc.cipher,
                lc.tls,
            );
        }
    }

    dlg.present(active_window().as_ref().map(|w| w.upcast_ref::<gtk::Widget>()));
    address.grab_focus();
}

// ======================================================================
// Save Bookmark dialog
// ======================================================================

/// Read the current form into a [`Bookmark`] and persist it to the TOML store.
fn save_bookmark_from_form(name: &str) {
    let w = widgets();
    let bm = Bookmark {
        name: name.to_string(),
        server: w.address.as_ref().map(|e| e.text().to_string()).unwrap_or_default(),
        port: w.port.as_ref().map(|e| e.text().to_string()).unwrap_or_default(),
        login: w.login.as_ref().map(|e| e.text().to_string()).unwrap_or_default(),
        password: w.password.as_ref().map(|e| e.text().to_string()).unwrap_or_default(),
        hope: w.hope.as_ref().map(|h| h.is_active()).unwrap_or(false),
        compress: w.compress.as_ref().map(|c| c.selected()).unwrap_or(0) as u8,
        cipher: cipher_vocab::dropdown_to_cipher_byte(
            w.cipher.as_ref().map(|c| c.selected()).unwrap_or(0),
        ),
        tls: w.tls.as_ref().map(|t| t.is_active()).unwrap_or(false),
    };

    match bookmark_store::upsert(bm) {
        Ok(()) => unsafe { toolbar_refresh_bookmarks() },
        Err(msg) => unsafe {
            error_dialog(cs(&tr("Error")).as_ptr(), cs(&msg).as_ptr());
        },
    }
}

/// The "Save Bookmark…" AdwAlertDialog with a name entry.
fn save_dialog() {
    let dialog = adw::AlertDialog::new(
        Some(&tr("Save Bookmark")),
        Some(&tr("Enter a name for this bookmark.")),
    );
    dialog.add_response("cancel", &tr("_Cancel"));
    dialog.add_response("save", &tr("_Save"));
    dialog.set_response_appearance("save", adw::ResponseAppearance::Suggested);
    dialog.set_default_response(Some("save"));
    dialog.set_close_response("cancel");
    unsafe { cffi::gtkhx_dialog_add_close_shortcuts(dialog.as_ptr() as *mut cffi::GtkWidget) };

    let name_entry = gtk::Entry::new();
    name_entry.set_activates_default(true);
    dialog.set_extra_child(Some(&name_entry));

    {
        let entry = name_entry.clone();
        dialog.connect_response(None, move |_, resp| {
            if resp != "save" {
                return;
            }
            let name = entry.text();
            if name.is_empty() {
                unsafe {
                    error_dialog(
                        cs(&tr("Error")).as_ptr(),
                        cs(&tr(
                            "You must specify a name for this bookmark with at least one character.",
                        ))
                        .as_ptr(),
                    );
                }
                return;
            }
            save_bookmark_from_form(&name);
        });
    }
    {
        let dlg = dialog.clone();
        name_entry.connect_activate(move |_| dlg.emit_by_name::<()>("response", &[&"save"]));
    }

    dialog.present(active_window().as_ref().map(|w| w.upcast_ref::<gtk::Widget>()));
}

// ======================================================================
// Bookmark load + open
// ======================================================================

/// Run the RC4-replacement picker if the bookmark carries the legacy RC4
/// byte. Returns the (possibly-updated) cipher byte, or None if the user
/// cancelled (caller should abandon).
fn rc4_migrate(name: &str, hope: bool, cipher_byte: u8) -> Option<u8> {
    if hope && cipher_byte == cipher::RC4 {
        let parent = unsafe { cffi::gtkhx_active_window() };
        let nb = crate::rc4_dialog::run_sync(parent, name);
        if nb < 0 {
            return None;
        }
        return Some(nb as u8);
    }
    Some(cipher_byte)
}

/// `void connect_open_bookmark_by_name(const char *name)` — connect to a
/// saved bookmark directly, no dialog.
///
/// # Safety
/// `name` is NULL or a valid C string.
#[no_mangle]
pub unsafe extern "C" fn connect_open_bookmark_by_name(name: *const c_char) {
    if name.is_null() {
        return;
    }
    let name = cstr(name);
    if name.is_empty() {
        return;
    }
    let bm = match bookmark_store::find(&name) {
        Ok(Some(bm)) => bm,
        Ok(None) => {
            glib::g_warning!("gtkhx", "{} \"{}\"", tr("No such bookmark"), name);
            return;
        }
        Err(e) => {
            unsafe { error_dialog(cs(&tr("Error")).as_ptr(), cs(&e).as_ptr()) };
            return;
        }
    };
    let port: u16 = if bm.port.is_empty() {
        5500
    } else {
        atoi_port(&bm.port)
    };
    let Some(cipher_byte) = rc4_migrate(&name, bm.hope, bm.cipher) else {
        return;
    };
    let sess = cffi::hx_active_session();
    connect_with_args(
        sess, &bm.server, port, &bm.login, &bm.password, bm.hope as u8, bm.compress, cipher_byte,
        bm.tls as u8,
    );
}

/// Fill the connect-dialog form from a saved bookmark (the preload path used
/// by connect_bookmark_name; the SplitButton uses direct-connect instead).
fn open_bookmark_preload(name: &str) {
    let bm = match bookmark_store::find(name) {
        Ok(Some(bm)) => bm,
        Ok(None) => {
            glib::g_warning!("gtkhx", "{} \"{}\"", tr("No such bookmark"), name);
            return;
        }
        Err(e) => {
            unsafe { error_dialog(cs(&tr("Error")).as_ptr(), cs(&e).as_ptr()) };
            return;
        }
    };
    let Some(cipher_byte) = rc4_migrate(name, bm.hope, bm.cipher) else {
        return;
    };
    set_the_entries_impl(
        &bm.server, &bm.login, &bm.password, &bm.port, bm.hope as u8, bm.compress, cipher_byte,
        bm.tls as u8,
    );
}

/// `void connect_bookmark_name(char *name)` — open the dialog, then preload
/// it from the named bookmark.
///
/// # Safety
/// `name` is NULL or a valid C string.
#[no_mangle]
pub unsafe extern "C" fn connect_bookmark_name(name: *mut c_char) {
    create_connect_window(std::ptr::null_mut(), cffi::hx_active_session());
    // Open the (empty) dialog even with no name, but only preload when we
    // actually have one — an empty name would probe the bookmarks dir itself
    // (spurious "no such bookmark" / conversion prompts).
    if name.is_null() {
        return;
    }
    let name = cstr(name);
    if !name.is_empty() {
        open_bookmark_preload(&name);
    }
}

// ======================================================================
// Bookmark menu (toolbar SplitButton dropdown)
// ======================================================================

/// `GMenu *connect_build_bookmark_menu(void)` — one item per saved bookmark
/// (built-ins are ordinary entries now). Returns a new GMenu; caller owns the
/// ref.
#[no_mangle]
pub extern "C" fn connect_build_bookmark_menu() -> *mut gio::ffi::GMenu {
    let menu = gio::Menu::new();
    for name in bookmark_store::names() {
        let item = gio::MenuItem::new(Some(&name), None);
        item.set_action_and_target_value(
            Some("app.open_bookmark"),
            Some(&glib::Variant::from(name.as_str())),
        );
        menu.append_item(&item);
    }
    menu.into_glib_ptr()
}

// ======================================================================
// hotline:// URL handlers
// ======================================================================

/// `gboolean connect_open_hotline_url(const char *url)`.
///
/// # Safety
/// `url` is NULL or a valid C string.
#[no_mangle]
pub unsafe extern "C" fn connect_open_hotline_url(url: *const c_char) -> glib::ffi::gboolean {
    if url.is_null() {
        return glib::ffi::GFALSE;
    }
    let mut parts = std::mem::zeroed::<HotlineUrlParts>();
    if hotline_url_parse(url, &mut parts) == glib::ffi::GFALSE {
        return glib::ffi::GFALSE;
    }
    let host = cstr(parts.host.as_ptr());
    let login = cstr(parts.login.as_ptr());
    let pass = cstr(parts.pass.as_ptr());
    let port = if parts.port != 0 { parts.port } else { 5500 };
    // Plain Hotline — the URL form carries no HOPE / TLS / compress / cipher.
    connect_with_args(cffi::hx_active_session(), &host, port, &login, &pass, 0, 0, 0, 0);
    glib::ffi::GTRUE
}

/// `gboolean connect_save_hotline_url_as_bookmark(const char *url,
/// char **out_name, GError **err)`.
///
/// # Safety
/// `url` is a valid C string; `out_name` / `err` are NULL or valid out-ptrs.
#[no_mangle]
pub unsafe extern "C" fn connect_save_hotline_url_as_bookmark(
    url: *const c_char,
    out_name: *mut *mut c_char,
    err: *mut *mut glib::ffi::GError,
) -> glib::ffi::gboolean {
    if !out_name.is_null() {
        *out_name = std::ptr::null_mut();
    }
    if url.is_null() {
        set_file_error(
            err,
            glib::ffi::G_FILE_ERROR_INVAL,
            &tr("Couldn't parse hotline:// URL"),
        );
        return glib::ffi::GFALSE;
    }

    let mut parts = std::mem::zeroed::<HotlineUrlParts>();
    if hotline_url_parse(url, &mut parts) == glib::ffi::GFALSE {
        set_file_error(
            err,
            glib::ffi::G_FILE_ERROR_INVAL,
            &tr("Couldn't parse hotline:// URL"),
        );
        return glib::ffi::GFALSE;
    }

    let host = cstr(parts.host.as_ptr());
    // The bookmark name is just the host (the store keys on name; no
    // filename escaping needed now).
    let name = host.clone();
    if name.is_empty() {
        set_file_error(
            err,
            glib::ffi::G_FILE_ERROR_INVAL,
            &tr("Couldn't derive a bookmark name from URL"),
        );
        return glib::ffi::GFALSE;
    }

    // Refuse to clobber an existing bookmark of the same name.
    if bookmark_store::exists(&name) {
        set_file_error(
            err,
            glib::ffi::G_FILE_ERROR_EXIST,
            &tr1(
                "Bookmark \"%s\" already exists. Manage it from the Bookmarks dialog.",
                &name,
            ),
        );
        return glib::ffi::GFALSE;
    }

    let port = if parts.port != 0 {
        format!("{}", parts.port)
    } else {
        String::new()
    };
    let bm = Bookmark {
        name: name.clone(),
        server: host,
        port,
        login: cstr(parts.login.as_ptr()),
        password: cstr(parts.pass.as_ptr()),
        ..Default::default()
    };

    match bookmark_store::upsert(bm) {
        Ok(()) => {
            if !out_name.is_null() {
                *out_name = glib::ffi::g_strdup(cs(&name).as_ptr());
            }
            toolbar_refresh_bookmarks();
            glib::ffi::GTRUE
        }
        Err(msg) => {
            set_file_error(err, glib::ffi::G_FILE_ERROR_FAILED, &msg);
            glib::ffi::GFALSE
        }
    }
}

// ======================================================================
// Small helpers
// ======================================================================

/// Parse a port string the way the C dialog's `atoi` did: optional leading
/// whitespace, an optional `+`/`-` sign, then digits (stopping at the first
/// non-digit). Anything with no digits (empty, "abc") yields 0 — which makes
/// the connect fail loudly rather than silently falling back to a default.
/// The signed value is truncated to 16 bits, matching the old `atoi` →
/// `guint16` assignment (so e.g. "-1" wraps to 65535, "+5500" is 5500).
fn atoi_port(s: &str) -> u16 {
    let t = s.trim_start();
    let (neg, rest) = if let Some(r) = t.strip_prefix('-') {
        (true, r)
    } else {
        (false, t.strip_prefix('+').unwrap_or(t))
    };
    let digits: String = rest.chars().take_while(|c| c.is_ascii_digit()).collect();
    let v = digits.parse::<i64>().unwrap_or(0);
    let v = if neg { v.wrapping_neg() } else { v };
    (v & 0xffff) as u16
}

/// g_set_error on a G_FILE_ERROR-domain out-param (no-op if `err` is NULL,
/// matching glib's own contract). `code` preserves the specific G_FILE_ERROR_*
/// the C connect_save_hotline_url_as_bookmark used, so callers that branch on
/// err->code (INVAL vs EXIST vs NOMEM) keep working.
unsafe fn set_file_error(err: *mut *mut glib::ffi::GError, code: c_int, msg: &str) {
    glib::ffi::g_set_error_literal(
        err,
        glib::ffi::g_file_error_quark(),
        code,
        cs(msg).as_ptr(),
    );
}
