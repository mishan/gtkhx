//! User Editor + "Open User" dialog (ported from the UI half of
//! `src/usermod.c`). The wire senders (`hx_useredit_create/delete/open`)
//! and the access-bit table stay in C (protocol send-path + byte-order
//! magic); this module drives the Adwaita UI and calls them via FFI.
//!
//! State lives in an `EDITORS` map keyed by a `u64` id. Handlers and the
//! account-read reply trampoline capture the id (Copy) and look the state
//! up, so there are no ref cycles and a reply arriving after the window
//! closed is a safe no-op (the id is gone from the map).

use crate::ffi as cffi;
use crate::tr::tr;
use crate::{cs, cstr};
use gtk4 as gtk;
use libadwaita as adw;

use adw::prelude::*;
use gtk::glib;
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::c_char;
use std::os::raw::{c_int, c_void};
use std::rc::Rc;

/// `void (*)(void *uesp, const char *name, const char *login, const char
/// *pass, hl_access_bits access)` — the account-read reply callback.
type UserOpenCb =
    unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char, *const c_char, u64);

extern "C" {
    // usermod.c — access-bit table accessors (byte-order magic stays in C).
    fn gtkhx_useredit_access_count() -> c_int;
    fn gtkhx_useredit_access_name(i: c_int) -> *const c_char;
    fn gtkhx_useredit_access_bitno(i: c_int) -> c_int;
    // usermod.c — wire senders (protocol send-path).
    fn hx_useredit_create(
        htlc: *mut c_void,
        login: *const c_char,
        pass: *const c_char,
        name: *const c_char,
        access: u64,
    );
    fn hx_useredit_delete(htlc: *mut c_void, login: *const c_char);
    fn hx_useredit_open(
        htlc: *mut c_void,
        login: *const c_char,
        cb: UserOpenCb,
        uesp: *mut c_void,
    );
    // gtkhx_ui_bridge.c — &hx_active_session()->htlc.
    fn gtkhx_active_htlc() -> *mut c_void;
    // rand.c — CSPRNG bytes (getrandom + /dev/urandom fallback).
    fn random_bytes(buf: *mut u8, len: usize) -> usize;
}

struct UserEdit {
    window: gtk::Window,
    login_row: adw::EntryRow,
    name_row: adw::EntryRow,
    pass_row: adw::PasswordEntryRow,
    access_buf: Cell<u64>,
    switches: Vec<(u8, adw::SwitchRow)>,
    /// Clamped login (32-byte protocol field) used by Save / Delete.
    login: RefCell<String>,
    is_new: bool,
}

thread_local! {
    // id/key is usize (pointer-width) so the `id as *mut c_void` → `uesp as
    // usize` round-trip through hx_useredit_open is lossless on every target,
    // including 32-bit (a u64 id would truncate going through void*).
    static EDITORS: RefCell<HashMap<usize, Rc<UserEdit>>> = RefCell::new(HashMap::new());
    static NEXT_ID: Cell<usize> = const { Cell::new(1) };
}

/// Clamp to the 31-byte + NUL protocol field on a char boundary.
fn clamp32(s: &str) -> String {
    if s.len() <= 31 {
        return s.to_owned();
    }
    let mut end = 31;
    while end > 0 && !s.is_char_boundary(end) {
        end -= 1;
    }
    s[..end].to_owned()
}

impl UserEdit {
    fn save(&self) {
        let name = clamp32(&self.name_row.text());
        let pass = clamp32(&self.pass_row.text());
        if self.is_new {
            *self.login.borrow_mut() = clamp32(&self.login_row.text());
        }
        let login = self.login.borrow().clone();
        unsafe {
            hx_useredit_create(
                gtkhx_active_htlc(),
                cs(&login).as_ptr(),
                cs(&pass).as_ptr(),
                cs(&name).as_ptr(),
                self.access_buf.get(),
            );
        }
    }

    fn delete(&self) {
        let login = self.login.borrow().clone();
        unsafe { hx_useredit_delete(gtkhx_active_htlc(), cs(&login).as_ptr()) };
        self.window.destroy();
    }

    fn generate(&self) {
        match gen_password(GENERATED_PASSWORD_LEN) {
            Some(pw) => self.pass_row.set_text(&pw),
            None => {
                glib::g_warning!("gtkhx", "password generation failed: no entropy source");
                self.pass_row.set_text("");
            }
        }
    }
}

/// 16 chars over a 75-char alphabet ≈ 99.7 bits. Mirrors the C generator
/// (rejection sampling for uniform distribution; CSPRNG entropy).
const GENERATED_PASSWORD_LEN: usize = 16;

fn gen_password(len: usize) -> Option<String> {
    const ALPHABET: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$%^&*()-_=+?";
    let alpha = ALPHABET.len();
    let limit = (256 / alpha) * alpha;
    let mut out = String::with_capacity(len);
    let mut buf = [0u8; 64];
    let mut bi = buf.len();
    while out.len() < len {
        if bi >= buf.len() {
            if unsafe { random_bytes(buf.as_mut_ptr(), buf.len()) } != buf.len() {
                return None;
            }
            bi = 0;
        }
        if (buf[bi] as usize) < limit {
            out.push(ALPHABET[buf[bi] as usize % alpha] as char);
        }
        bi += 1;
    }
    Some(out)
}

/// Account-read reply → fill the dialog. `uesp` carries the editor id.
///
/// # Safety
/// Called by the C rcv task with the (name, login, pass) C strings.
unsafe extern "C" fn user_open_cb(
    uesp: *mut c_void,
    name: *const c_char,
    login: *const c_char,
    pass: *const c_char,
    access: u64,
) {
    let id = uesp as usize;
    let Some(st) = EDITORS.with_borrow(|m| m.get(&id).cloned()) else {
        return; // editor already closed — safe no-op
    };
    let login_s = cstr(login);
    st.login_row.set_text(&login_s);
    st.name_row.set_text(&cstr(name));
    st.pass_row.set_text(&cstr(pass));
    *st.login.borrow_mut() = clamp32(&login_s);
    // Set the canonical buffer first (preserves any bits no switch shows),
    // then reflect it into the switches (their notify handlers re-confirm
    // the same bits — idempotent).
    st.access_buf.set(access);
    for (bitno, sw) in &st.switches {
        sw.set_active((access >> bitno) & 1 == 1);
    }
}

/// `void create_useredit_window(const char *login, int new)`.
///
/// # Safety
/// `login` is NULL or a valid C string.
#[no_mangle]
pub unsafe extern "C" fn create_useredit_window(login: *const c_char, new: c_int) {
    crate::ensure_gtk_init();
    let login = cstr(login);
    let is_new = new != 0;

    let window = gtk::Window::new();
    window.set_default_size(520, 680);
    if is_new {
        window.set_title(Some(&tr("New User")));
    } else {
        window.set_title(Some(&format!("{}: {}", tr("User Editor"), login)));
    }

    let header = adw::HeaderBar::new();
    let save_btn = gtk::Button::with_label(&tr("Save"));
    save_btn.add_css_class("suggested-action");
    header.pack_end(&save_btn);
    let delete_btn = if is_new {
        None
    } else {
        let b = gtk::Button::with_label(&tr("Delete"));
        b.add_css_class("destructive-action");
        header.pack_start(&b);
        Some(b)
    };

    let page = adw::PreferencesPage::new();

    // Identity group.
    let info = adw::PreferencesGroup::new();
    info.set_title(&tr("Identity"));
    let login_row = adw::EntryRow::new();
    login_row.set_title(&tr("Login"));
    if !is_new {
        login_row.set_editable(false); // login is the server-side key
    }
    info.add(&login_row);
    let name_row = adw::EntryRow::new();
    name_row.set_title(&tr("Display name"));
    info.add(&name_row);
    let pass_row = adw::PasswordEntryRow::new();
    pass_row.set_title(&tr("Password"));
    info.add(&pass_row);
    let gen_btn = gtk::Button::from_icon_name("view-refresh-symbolic");
    gen_btn.set_valign(gtk::Align::Center);
    gen_btn.add_css_class("flat");
    gen_btn.set_tooltip_text(Some(&tr("Generate a random password")));
    pass_row.add_suffix(&gen_btn);
    page.add(&info);

    // Access bits — sentinels (bitno == -1) start a new group.
    let count = gtkhx_useredit_access_count();
    let mut switches: Vec<(u8, adw::SwitchRow)> = Vec::new();
    let mut current_grp: Option<adw::PreferencesGroup> = None;
    for i in 0..count {
        let bitno = gtkhx_useredit_access_bitno(i);
        let name = cstr(gtkhx_useredit_access_name(i));
        if bitno == -1 {
            let g = adw::PreferencesGroup::new();
            g.set_title(&name);
            page.add(&g);
            current_grp = Some(g);
            continue;
        }
        let sw = adw::SwitchRow::new();
        sw.set_title(&name);
        if let Some(g) = &current_grp {
            g.add(&sw);
        }
        switches.push((bitno as u8, sw));
    }

    window.set_titlebar(Some(&header));
    window.set_child(Some(&page));
    cffi::init_keyaccel_dialog(window.as_ptr() as *mut cffi::GtkWidget);

    let id: usize = NEXT_ID.with(|c| {
        let id = c.get();
        c.set(id.wrapping_add(1));
        id
    });
    let state = Rc::new(UserEdit {
        window: window.clone(),
        login_row,
        name_row,
        pass_row,
        access_buf: Cell::new(0),
        switches,
        login: RefCell::new(clamp32(&login)),
        is_new,
    });
    EDITORS.with_borrow_mut(|m| m.insert(id, state.clone()));
    // try_with (not with_borrow_mut): on Ctrl-Q the C hx_quit calls exit(),
    // whose TLS destructors drop EDITORS, disposing the window and firing
    // "destroy" while EDITORS is mid-destruction — a plain access would
    // panic (AccessError) and abort across the FFI.
    window.connect_destroy(move |_| {
        let _ = EDITORS.try_with(|m| {
            m.borrow_mut().remove(&id);
        });
    });

    for (bitno, sw) in &state.switches {
        let bitno = *bitno;
        sw.connect_active_notify(move |sw| {
            EDITORS.with_borrow(|m| {
                if let Some(st) = m.get(&id) {
                    let mut a = st.access_buf.get();
                    if sw.is_active() {
                        a |= 1u64 << bitno;
                    } else {
                        a &= !(1u64 << bitno);
                    }
                    st.access_buf.set(a);
                }
            });
        });
    }
    save_btn.connect_clicked(move |_| {
        EDITORS.with_borrow(|m| {
            if let Some(st) = m.get(&id) {
                st.save();
            }
        });
    });
    if let Some(db) = &delete_btn {
        db.connect_clicked(move |_| {
            EDITORS.with_borrow(|m| {
                if let Some(st) = m.get(&id) {
                    st.delete();
                }
            });
        });
    }
    gen_btn.connect_clicked(move |_| {
        EDITORS.with_borrow(|m| {
            if let Some(st) = m.get(&id) {
                st.generate();
            }
        });
    });

    if !is_new {
        let lc = cs(&login);
        hx_useredit_open(
            gtkhx_active_htlc(),
            lc.as_ptr(),
            user_open_cb,
            id as *mut c_void,
        );
    }

    window.present();
}

/// `void useredit_open_dialog(void)` — the "Open User" AdwAlertDialog.
#[no_mangle]
pub extern "C" fn useredit_open_dialog() {
    crate::ensure_gtk_init();

    let dialog = adw::AlertDialog::new(
        Some(&tr("Open User")),
        Some(&tr("Enter the login of the account to edit.")),
    );
    dialog.add_response("cancel", &tr("_Cancel"));
    dialog.add_response("open", &tr("_Open"));
    dialog.set_response_appearance("open", adw::ResponseAppearance::Suggested);
    dialog.set_default_response(Some("open"));
    dialog.set_close_response("cancel");

    let grp = adw::PreferencesGroup::new();
    let entry = adw::EntryRow::new();
    entry.set_title(&tr("Login"));
    grp.add(&entry);
    dialog.set_extra_child(Some(&grp));

    unsafe { cffi::gtkhx_dialog_add_close_shortcuts(dialog.as_ptr() as *mut cffi::GtkWidget) };

    {
        let entry = entry.clone();
        dialog.connect_response(None, move |_, resp| {
            if resp == "open" {
                let login = entry.text();
                if !login.is_empty() {
                    unsafe { create_useredit_window(cs(&login).as_ptr(), 0) };
                }
            }
        });
    }
    // AdwEntryRow swallows Enter for its own "entry-activated" signal, so
    // bridge it to the same open action + dismiss.
    {
        let dlg = dialog.clone();
        entry.connect_entry_activated(move |e| {
            let login = e.text();
            if !login.is_empty() {
                unsafe { create_useredit_window(cs(&login).as_ptr(), 0) };
            }
            dlg.close();
        });
    }

    // Present over the active toplevel (usually the toolbar window).
    let ap = unsafe { cffi::gtkhx_active_window() };
    let parent: Option<gtk::Window> = if ap.is_null() {
        None
    } else {
        Some(unsafe { glib::translate::from_glib_none(ap) })
    };
    dialog.present(parent.as_ref().map(|w| w.upcast_ref::<gtk::Widget>()));
    entry.grab_focus();
}
