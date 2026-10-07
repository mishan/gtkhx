//! User Editor + "Open User" dialog (ported from the UI half of
//! `src/usermod.c`). The access-bit table stays in C (byte-order magic);
//! the account is read, made, saved and deleted through
//! `hxhandlers::send::user`, on the connection the editor was opened for,
//! and not once that connection has closed or its tab connected again.
//!
//! State lives in an `EDITORS` map keyed by a `usize` id. Handlers and the
//! account-read reply capture the id (Copy) and look the state up, so there
//! are no ref cycles and a reply arriving after the window closed is a safe
//! no-op (the id is gone from the map).

use crate::cstr;
use crate::dock::{self, Bound};
use crate::ffi as cffi;
use crate::tr::{tr, tr1};
use gtk4 as gtk;
use libadwaita as adw;

use adw::prelude::*;
use gtk::glib;
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::c_char;
use std::os::raw::{c_int, c_void};
use std::rc::Rc;

use hxsession::Account;

extern "C" {
    // usermod.c — access-bit table accessors (byte-order magic stays in C).
    fn gtkhx_useredit_access_count() -> c_int;
    fn gtkhx_useredit_access_name(i: c_int) -> *const c_char;
    fn gtkhx_useredit_access_bitno(i: c_int) -> c_int;
    // sound.c
    fn play_sound(sound: c_int);
}

/// `ERROR` (sound.h).
const SOUND_ERROR: c_int = 2;

/// `HTLC_CAP_TEXT_ENCODING` (hotline.h): the server takes UTF-8 text.
const CAP_TEXT_ENCODING: u64 = 0x0002;
/// `HL_ACCESS_READ_USERS` (hl_access.h): we may read accounts.
const HL_ACCESS_READ_USERS: c_int = 16;
/// `HL_ACCESS_DONT_SHOW_AGREEMENT` (hl_access.h).
const HL_ACCESS_DONT_SHOW_AGREEMENT: c_int = 27;

/// A field as the account read gave it: the server's bytes, sent back as
/// they came unless the user changes what is shown.
#[derive(Default)]
struct Read {
    bytes: Vec<u8>,
    shown: String,
}

impl Read {
    fn new(bytes: &[u8]) -> Self {
        let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
        Read {
            bytes: bytes.to_vec(),
            shown: hxproto::text::to_utf8(&bytes[..end]),
        }
    }

    /// What goes back to the server for a field that now shows `typed`:
    /// its own bytes, or what was typed, encoded as a server that is not
    /// `utf8` takes text, and clamped to the 32-byte protocol field once
    /// encoded — an emoji can become a longer `:shortcode:`.
    fn or_typed(&self, typed: &str, utf8: bool) -> Vec<u8> {
        if typed == self.shown {
            self.bytes.clone()
        } else if utf8 {
            clamp32(typed).into_bytes()
        } else {
            let mut wire = hxtext::for_wire(typed.as_bytes(), false, false);
            wire.truncate(31);
            wire
        }
    }

    /// The password to send for one that now shows `typed`: untouched,
    /// none, which keeps the one the account has. Mobius reads back a
    /// hash, which sent back would become the password.
    fn password(&self, typed: &str) -> Vec<u8> {
        if typed == self.shown {
            Vec::new()
        } else {
            self.or_typed(typed, true)
        }
    }
}

struct UserEdit {
    window: gtk::Window,
    toast: adw::ToastOverlay,
    login_row: adw::EntryRow,
    name_row: adw::EntryRow,
    pass_row: adw::PasswordEntryRow,
    access_buf: Cell<u64>,
    switches: Vec<(u8, adw::SwitchRow)>,
    /// The connection the account is on.
    conn: Bound,
    /// The login Save and Delete name the account by: as typed, clamped to
    /// the 32-byte protocol field, until the read gives the server's own.
    login: RefCell<Vec<u8>>,
    name: RefCell<Read>,
    pass: RefCell<Read>,
    is_new: bool,
    /// The login a New User window has made, once the server said so:
    /// saved again, it is changed, not made a second time. A create the
    /// server refused leaves the next Save a create too, so it can't
    /// overwrite an account that was already there.
    made: RefCell<Option<Vec<u8>>>,
    /// A create, or its check, is out and not yet answered.
    creating: Cell<bool>,
    /// The key this editor has in `EDITORS`.
    id: usize,
}

thread_local! {
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
        if self.is_new {
            *self.login.borrow_mut() = clamp32(&self.login_row.text()).into_bytes();
        }
        if let Some(htlc) = dock::live_htlc(self.conn) {
            let utf8 = unsafe { gtkhx_core::conn::hx_conn_has_cap(htlc.cast(), CAP_TEXT_ENCODING) };
            // The name in the server's encoding; the login and password as
            // typed, as GtkHx's login sends them.
            let name = self
                .name
                .borrow()
                .or_typed(&self.name_row.text(), utf8 != 0);
            let pass = self.pass.borrow().password(&self.pass_row.text());
            let access = access_to_wire(self.access_buf.get());
            let login = self.login.borrow();
            if self.is_new && self.made.borrow().as_ref() != Some(&*login) {
                // One create at a time: a second, sent before the first is
                // answered, would make the account twice, or find it taken.
                if self.creating.replace(true) {
                    return;
                }
                let (id, conn, made) = (self.id, self.conn, login.clone());
                let mark = move |r: Result<(), Option<String>>| {
                    if let Some(st) = EDITORS.with_borrow(|m| m.get(&id).cloned()) {
                        st.creating.set(false);
                        if r.is_ok() {
                            *st.made.borrow_mut() = Some(made);
                        }
                    }
                    if let Err(Some(reason)) = r {
                        refused(id, conn, &reason);
                    }
                };
                // Told it is there, nothing is made: a server may replace
                // an account with a new one of the same login.
                let shown = Read::new(&login).shown;
                let exists = move || {
                    if let Some(st) = EDITORS.with_borrow(|m| m.get(&id).cloned()) {
                        st.creating.set(false);
                    }
                    refused(id, conn, &tr1("An account named %s already exists", &shown));
                };
                let can_read = unsafe {
                    gtkhx_core::conn::hx_conn_access_has(htlc.cast(), HL_ACCESS_READ_USERS)
                } != 0;
                unsafe {
                    hxhandlers::send::user::account_create(
                        htlc, &login, &pass, &name, access, can_read, exists, mark,
                    )
                };
            } else {
                let (id, conn) = (self.id, self.conn);
                let done = move |r: Result<(), Option<String>>| {
                    if let Err(Some(reason)) = r {
                        refused(id, conn, &reason);
                    }
                };
                unsafe {
                    hxhandlers::send::user::account_save(htlc, &login, &pass, &name, access, done)
                };
            }
        }
    }

    fn delete(&self) {
        if let Some(htlc) = dock::live_htlc(self.conn) {
            unsafe { hxhandlers::send::user::account_delete(htlc, &self.login.borrow()) };
        }
        self.window.close();
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

/// The server's refusal of what editor `id` asked, `reason`, shown in the
/// editor: a toast behind it goes unseen. Once the editor has closed, shown
/// as any refusal is.
fn refused(id: usize, conn: Bound, reason: &str) {
    let generic;
    let reason = if reason.is_empty() {
        generic = tr("The server refused the change");
        &generic
    } else {
        reason
    };
    if let Some(st) = EDITORS.with_borrow(|m| m.get(&id).cloned()) {
        st.toast.add_toast(adw::Toast::new(reason));
        unsafe { play_sound(SOUND_ERROR) };
    } else if let Some(htlc) = dock::live_htlc(conn) {
        let msg = crate::cs(reason);
        unsafe {
            gtkhx_core::session::gtkhx_session_emit_request_failed(
                gtkhx_core::session::gtkhx_session_get_default(),
                htlc,
                msg.as_ptr(),
            )
        };
    }
}

/// 16 chars over a 75-char alphabet ≈ 99.7 bits. Mirrors the C generator
/// (rejection sampling for uniform distribution; CSPRNG entropy).
const GENERATED_PASSWORD_LEN: usize = 16;

fn gen_password(len: usize) -> Option<String> {
    const ALPHABET: &[u8] =
        b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$%^&*()-_=+?";
    let alpha = ALPHABET.len();
    let limit = (256 / alpha) * alpha;
    let mut out = String::with_capacity(len);
    let mut buf = [0u8; 64];
    let mut bi = buf.len();
    while out.len() < len {
        if bi >= buf.len() {
            // CSPRNG entropy via the getrandom crate (portable: Linux
            // getrandom(2), macOS getentropy, Windows BCryptGenRandom).
            if getrandom::fill(&mut buf).is_err() {
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

/// The editor's access bits, numbered as the C table numbers them, as the
/// wire carries them: the table's numbering is of the bitmap's bytes in
/// host order.
fn access_to_wire(bits: u64) -> [u8; 8] {
    bits.to_ne_bytes()
}

fn access_from_wire(wire: [u8; 8]) -> u64 {
    u64::from_ne_bytes(wire)
}

/// The table's bit `bitno` as the wire numbers it (`hl_access.h`): bit 0
/// the first byte's top bit.
fn wire_bit(bitno: u8) -> c_int {
    let wire = access_to_wire(1 << bitno);
    let byte = wire.iter().position(|&b| b != 0).unwrap_or_default();
    (byte * 8) as c_int + wire[byte].leading_zeros() as c_int
}

/// The account read for editor `id`, into its fields.
fn fill(id: usize, account: &Account) {
    let Some(st) = EDITORS.with_borrow(|m| m.get(&id).cloned()) else {
        return; // editor already closed — safe no-op
    };
    let login = Read::new(&account.login);
    st.login_row.set_text(&login.shown);
    *st.login.borrow_mut() = login.bytes;
    let (name, pass) = (Read::new(&account.name), Read::new(&account.password));
    st.name_row.set_text(&name.shown);
    st.pass_row.set_text(&pass.shown);
    *st.name.borrow_mut() = name;
    *st.pass.borrow_mut() = pass;
    // Set the canonical buffer first (preserves any bits no switch shows),
    // then reflect it into the switches (their notify handlers re-confirm
    // the same bits — idempotent).
    let access = access_from_wire(account.access.unwrap_or_default().to_be_bytes());
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
    open_editor(dock::bind(dock::active_key()), &cstr(login), new != 0);
}

/// The editor for account `login` on `conn`, or for a new one.
///
/// # Safety
/// Main thread.
unsafe fn open_editor(conn: Bound, login: &str, is_new: bool) {
    crate::ensure_gtk_init();

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
        // A server, mhxd aside, refuses a new account a privilege its maker
        // lacks; the 1.9 server's own rule spares Don't Show Agreement.
        let bit = wire_bit(bitno as u8);
        let mine = |htlc: *mut c_void| unsafe {
            gtkhx_core::conn::hx_conn_access_has(htlc.cast(), bit) != 0
        };
        if is_new
            && bit != HL_ACCESS_DONT_SHOW_AGREEMENT
            && dock::live_htlc(conn).is_some_and(|h| !mine(h))
        {
            sw.set_sensitive(false);
            sw.set_subtitle(&tr("You don't have this privilege"));
        }
        if let Some(g) = &current_grp {
            g.add(&sw);
        }
        switches.push((bitno as u8, sw));
    }

    window.set_titlebar(Some(&header));
    let toast = adw::ToastOverlay::new();
    toast.set_child(Some(&page));
    window.set_child(Some(&toast));
    cffi::init_keyaccel_dialog(window.as_ptr() as *mut cffi::GtkWidget);

    let id: usize = NEXT_ID.with(|c| {
        let id = c.get();
        c.set(id.wrapping_add(1));
        id
    });
    let state = Rc::new(UserEdit {
        window: window.clone(),
        toast,
        login_row,
        name_row,
        pass_row,
        access_buf: Cell::new(0),
        switches,
        conn,
        login: RefCell::new(clamp32(login).into_bytes()),
        name: RefCell::default(),
        pass: RefCell::default(),
        is_new,
        made: RefCell::default(),
        creating: Cell::new(false),
        id,
    });
    EDITORS.with_borrow_mut(|m| m.insert(id, state.clone()));
    // Out of the map when it closes: the map's Rc holds the window, so
    // nothing else lets go of it.
    window.connect_close_request(move |_| {
        EDITORS.with_borrow_mut(|m| m.remove(&id));
        glib::Propagation::Proceed
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
            // Not under the borrow: closing the window takes its entry out
            // of EDITORS.
            if let Some(st) = EDITORS.with_borrow(|m| m.get(&id).cloned()) {
                st.delete();
            }
        });
    }
    gen_btn.connect_clicked(move |_| {
        EDITORS.with_borrow(|m| {
            if let Some(st) = m.get(&id) {
                st.generate();
            }
        });
    });

    if let Some(htlc) = dock::live_htlc(state.conn).filter(|_| !is_new) {
        hxhandlers::send::user::account_read(htlc, login.as_bytes(), move |a| fill(id, a));
    }

    window.present();
}

/// `void useredit_open_dialog(void)` — the "Open User" AdwAlertDialog.
#[no_mangle]
pub extern "C" fn useredit_open_dialog() {
    crate::ensure_gtk_init();

    // The server the user was on when they asked, whichever is in focus
    // once they answer.
    let conn = dock::bind(dock::active_key());
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
                    unsafe { open_editor(conn, &login, false) };
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
                unsafe { open_editor(conn, &login, false) };
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_table_bit_is_the_wire_bit_it_names() {
        // usermod.c's ENTRY macro, for spec bit 32, Can Broadcast: the
        // first bit of the bitmap's fifth byte.
        let x = 32;
        let host = if cfg!(target_endian = "big") {
            x
        } else {
            x % 8 + 8 * (7 - x / 8)
        };
        let bitno = 63 - host;
        let wire = [0, 0, 0, 0, 0x80, 0, 0, 0];
        assert_eq!(access_to_wire(1 << bitno), wire);
        assert_eq!(access_from_wire(wire), 1 << bitno);
        assert_eq!(wire_bit(bitno as u8), x);
    }

    #[test]
    fn an_untouched_password_goes_as_none() {
        let hash = b"$2a$04$GtwQO3DnEdJDwZ1OFN6or.umlmsGrh6BpDbqnKklgW3nakS0XJFsa";
        let cases: [(&[u8], &str, &[u8]); 4] = [
            (hash, std::str::from_utf8(hash).unwrap(), b""),
            (hash, "new", b"new"),
            (b"", "", b""),
            (b"", "pw", b"pw"),
        ];
        for (read, typed, want) in cases {
            assert_eq!(Read::new(read).password(typed), want, "{typed:?}");
        }
    }

    #[test]
    fn a_read_field_goes_back_as_the_server_s_bytes_unless_changed() {
        // 0x8E is Mac Roman é, shown as such; 0xC3 0xA9 is é in UTF-8. A
        // changed field goes in the server's encoding.
        let cases: [(&[u8], &str, bool, &[u8]); 6] = [
            (b"Ren\x8e", "René", false, b"Ren\x8e"),
            (b"Ren\x8e", "René", true, b"Ren\x8e"),
            ("René".as_bytes(), "René", false, "René".as_bytes()),
            (b"Ren\x8e", "Renée", false, b"Ren\x8ee"),
            (b"Ren\x8e", "Renée", true, "Renée".as_bytes()),
            (b"", "", false, b""),
        ];
        // Clamped to the field once encoded: 40 é are 40 bytes of Mac
        // Roman, or 80 of UTF-8 cut at a character.
        let long = "é".repeat(40);
        let mac = [0x8e; 31];
        let utf = "é".repeat(15);
        let clamped = [
            (&b""[..], long.as_str(), false, &mac[..]),
            (b"", long.as_str(), true, utf.as_bytes()),
        ];
        for (bytes, typed, utf8, want) in cases.into_iter().chain(clamped) {
            assert_eq!(
                Read::new(bytes).or_typed(typed, utf8),
                want,
                "{typed:?} {utf8}"
            );
        }
    }
}
