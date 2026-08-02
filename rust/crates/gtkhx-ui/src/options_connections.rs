//! Settings → Connections — the connection collection.
//!
//! This was the standalone Bookmarks window. It is a Settings page now because
//! of what [`docs/multi-connection.md`] settles: once you can be connected to
//! several servers at once, "the list of servers" stops being an accessory and
//! becomes configuration, and per-server settings need somewhere to live that
//! outlives any particular connection.
//!
//! The shape follows from that. The page lists the saved connections; opening
//! one pushes an editor dialog carrying everything needed to reach that server
//! *and* how to appear on it. Identity is the interesting part: nick and icon
//! are each an optional override over the Settings → Identity default, absent
//! meaning inherit, resolved live at connect time rather than copied when the
//! entry is created — so changing the global nick moves every connection that
//! hasn't specialised.
//!
//! Connecting to an address that is *not* in this list is deliberately not an
//! act of configuration: a `hotline://` link, a hand-typed address or a server
//! picked out of the tracker produces a transient connection with no entry and
//! nothing written to disk. Each of those paths offers an explicit "save"
//! affordance instead.
//!
//! CRUD goes through [`crate::bookmark_store`] (the single TOML file owned by
//! [`hxbookmarks`]). The cipher / compression vocabulary comes from
//! [`crate::cipher_vocab`], shared with [`crate::connect`].
//!
//! Live editor widgets sit in a `thread_local!` because only one editor is
//! open at a time, and handlers snapshot them out of a short borrow before
//! touching anything — a signal-emitting setter must not re-enter a held
//! borrow. That discipline is inherited from the dialog this replaced and is
//! the same one [`crate::connect`] uses.

use crate::bookmark_store;
use crate::cipher_vocab;
use crate::ffi as cffi;
use crate::icon_picker;
use crate::tr::{tr, tr1, tr_fmt};
use gtk4 as gtk;
use libadwaita as adw;

use adw::prelude::*;
use gtk::glib;
use hxbookmarks::{cipher, Bookmark};
use std::cell::{Cell, RefCell};

/// "No override" for the icon. 0 is a real (blank) icon, so it cannot double
/// as unset — which is why the store holds `Option<u16>` and this holds a
/// sentinel rather than a zero.
const ICON_NONE: i32 = -1;

/// The editor's live widgets. Cheap to clone — every field is a refcounted
/// GObject handle.
#[derive(Clone)]
struct Editor {
    dialog: adw::Dialog,
    name_row: adw::EntryRow,
    server_row: adw::EntryRow,
    port_row: adw::EntryRow,
    login_row: adw::EntryRow,
    pass_row: adw::PasswordEntryRow,
    nick_row: adw::EntryRow,
    nick_clear: gtk::Button,
    icon_row: adw::ActionRow,
    icon_preview: gtk::Picture,
    icon_clear: gtk::Button,
    hope_row: adw::SwitchRow,
    tls_row: adw::SwitchRow,
    cipher_row: adw::ComboRow,
    compress_row: adw::ComboRow,
}

thread_local! {
    /// The list group on the live Settings page, so a save or delete can
    /// rebuild it in place.
    ///
    /// Weak: the group belongs to the Settings dialog's widget tree, and a
    /// strong reference here would keep that page — every row, and the whole
    /// connection list with it — alive after Settings closed, for as long as
    /// the process ran.
    static LIST: RefCell<Option<glib::WeakRef<adw::PreferencesGroup>>> =
        const { RefCell::new(None) };
    /// The rows currently in `LIST`. `AdwPreferencesGroup` can remove a row
    /// but cannot enumerate or clear, so a rebuild has to remember what it
    /// added. These are strong, so closing Settings leaves the last list's
    /// rows alive until the next rebuild drops them — bounded, and the cost
    /// of not needing a second weak upgrade on every row.
    static ROWS: RefCell<Vec<gtk::Widget>> = const { RefCell::new(Vec::new()) };
    /// The open editor, if any.
    static EDITOR: RefCell<Option<Editor>> = const { RefCell::new(None) };
    /// The on-disk name of the entry being edited. Distinct from the Name
    /// field once the user types a rename; `None` for a new unsaved entry,
    /// which is what makes Save take the create branch instead of rename.
    static ORIGINAL_NAME: RefCell<Option<String>> = const { RefCell::new(None) };
    /// The editor's pending icon override, or `ICON_NONE`. Kept beside the
    /// widgets rather than in one, because no row type carries an optional
    /// integer and the sentinel would have to be encoded into a real value.
    static ICON_OVERRIDE: Cell<i32> = const { Cell::new(ICON_NONE) };
}

fn editor() -> Option<Editor> {
    EDITOR.with_borrow(|e| e.clone())
}
fn original_name() -> Option<String> {
    ORIGINAL_NAME.with_borrow(|o| o.clone())
}
fn set_original_name(v: Option<String>) {
    ORIGINAL_NAME.with_borrow_mut(|o| *o = v);
}
fn icon_override() -> Option<u16> {
    let v = ICON_OVERRIDE.with(|c| c.get());
    (v >= 0).then_some(v as u16)
}
fn set_icon_override(v: Option<u16>) {
    ICON_OVERRIDE.with(|c| c.set(v.map(|i| i as i32).unwrap_or(ICON_NONE)));
}

// ======================================================================
// Store access
// ======================================================================

/// Load `name` from the store. An unreadable store logs and reads as absent —
/// but the list is built from the same store, so an unreadable file shows no
/// rows to open in the first place.
fn load_bm(name: &str) -> Option<Bookmark> {
    match bookmark_store::find(name) {
        Ok(bm) => bm,
        Err(e) => {
            glib::g_warning!("gtkhx", "connections: {e}");
            None
        }
    }
}

/// The effective nickname when this connection carries no override.
fn global_nick() -> String {
    crate::options::pref_get_string(crate::options::cfg::NICK)
}

/// The effective icon when this connection carries no override.
fn global_icon() -> u16 {
    crate::options::pref_get_int(crate::options::cfg::ICON).clamp(0, u16::MAX as i32) as u16
}

// ======================================================================
// The list
// ======================================================================

/// One connection's row: name, where it points, and whether it specialises
/// the identity — the last of those because "why does this server see a
/// different name?" should be answerable without opening the editor.
fn make_row(bm: &Bookmark) -> adw::ActionRow {
    let row = adw::ActionRow::new();
    row.set_title(&bm.name);

    let where_to = if bm.port.is_empty() {
        bm.server.clone()
    } else {
        format!("{}:{}", bm.server, bm.port)
    };
    let subtitle = match (&bm.nick, bm.icon) {
        (None, None) => where_to,
        (Some(nick), None) => tr_fmt("%1$s — as %2$s", &[&where_to, nick]),
        (Some(nick), Some(_)) => tr_fmt("%1$s — as %2$s, custom icon", &[&where_to, nick]),
        (None, Some(_)) => tr1("%s — custom icon", &where_to),
    };
    row.set_subtitle(&subtitle);
    row.set_activatable(true);

    let chevron = gtk::Image::from_icon_name("go-next-symbolic");
    chevron.add_css_class("dim-label");
    row.add_suffix(&chevron);

    let name = bm.name.clone();
    row.connect_activated(move |row| open_editor(row.upcast_ref(), Some(&name)));
    row
}

/// The row shown when there are no saved connections.
fn make_empty_row() -> adw::ActionRow {
    let row = adw::ActionRow::new();
    row.set_title(&tr("No saved connections"));
    row.set_subtitle(&tr(
        "Add one with the + button. Connecting to an address that isn't saved \
         here works fine — it just doesn't create an entry.",
    ));
    row.set_activatable(false);
    row
}

/// Rebuild the list in place from the store.
fn rebuild_list() {
    let group = LIST.with_borrow(|l| l.as_ref().and_then(|w| w.upgrade()));
    let Some(group) = group else {
        // Settings was closed. Drop the rows too — they are the only thing
        // still holding the old page's widgets alive.
        ROWS.with_borrow_mut(|rows| rows.clear());
        return;
    };
    ROWS.with_borrow_mut(|rows| {
        for row in rows.drain(..) {
            group.remove(&row);
        }
    });

    // One read of the store for the whole list. Asking for the names and then
    // looking each one up would re-read and re-parse the file once per
    // connection, and this runs on every Settings open.
    let store = bookmark_store::load();
    let mut added: Vec<gtk::Widget> = Vec::new();
    if store.bookmarks.is_empty() {
        let row = make_empty_row();
        group.add(&row);
        added.push(row.upcast());
    } else {
        for bm in &store.bookmarks {
            let row = make_row(bm);
            group.add(&row);
            added.push(row.upcast());
        }
    }
    ROWS.with_borrow_mut(|rows| *rows = added);
}

// ======================================================================
// Editor: form <-> store
// ======================================================================

/// Clamp a combo-row index to its model, sending anything out of range to 0
/// ("Off" / "NONE") rather than the last item. A corrupt or forward-format
/// byte shouldn't silently select a real cipher/compression entry (whose
/// notify handler could then force HOPE on).
fn clamp_combo(combo: &adw::ComboRow, idx: u32) -> u32 {
    let n = combo.model().map(|m| m.n_items()).unwrap_or(0);
    if n == 0 || idx >= n {
        0
    } else {
        idx
    }
}

/// Put the inherited nickname in the row's title, so an empty field reads as
/// "you will appear as this" rather than as nothing.
///
/// `AdwEntryRow` has no placeholder — the title *is* the placeholder, floating
/// up out of the way once there is text — so the default has to go there to be
/// visible without hovering. Re-set on every load, because the global can
/// change underneath: the Identity page is one click away in the same window.
fn set_nick_title(row: &adw::EntryRow) {
    let global = global_nick();
    if global.is_empty() {
        row.set_title(&tr("Nickname on this server"));
    } else {
        row.set_title(&format!("{} ({global})", tr("Nickname on this server")));
    }
}

/// Repaint the icon row from the current override, and gate "Use default".
///
/// The subtitle names which of the two states you are in, because the preview
/// alone can't distinguish "inherited, and the default happens to be this" from
/// "pinned to this".
fn refresh_icon_row(e: &Editor) {
    let (effective, overridden) = match icon_override() {
        Some(id) => (id, true),
        None => (global_icon(), false),
    };
    icon_picker::set_preview(&e.icon_preview, effective);
    e.icon_row.set_subtitle(&if overridden {
        tr1("Icon %s on this server", &effective.to_string())
    } else {
        tr1("Using the default (icon %s)", &effective.to_string())
    });
    e.icon_clear.set_sensitive(overridden);
}

fn refresh_nick_clear(e: &Editor) {
    e.nick_clear.set_sensitive(!e.nick_row.text().is_empty());
}

fn form_from_bookmark(e: &Editor, d: &Bookmark) {
    e.name_row.set_text(&d.name);
    e.server_row.set_text(&d.server);
    e.port_row.set_text(&d.port);
    e.login_row.set_text(&d.login);
    e.pass_row.set_text(&d.password);

    e.nick_row.set_text(d.nick.as_deref().unwrap_or(""));
    set_nick_title(&e.nick_row);
    refresh_nick_clear(e);

    set_icon_override(d.icon);
    refresh_icon_row(e);

    e.hope_row.set_active(d.hope);
    e.tls_row.set_active(d.tls);
    // Cipher uses the stable bookmark vocabulary; translate to the live
    // dropdown index. An unknown byte (e.g. RC4) maps to 0 ("Off").
    let cipher_idx = clamp_combo(
        &e.cipher_row,
        cipher_vocab::cipher_byte_to_dropdown(d.cipher),
    );
    e.cipher_row.set_selected(cipher_idx);
    let compress_idx = clamp_combo(&e.compress_row, d.compress as u32);
    e.compress_row.set_selected(compress_idx);
    sync_sensitivity();
}

/// Build a [`Bookmark`] from the current form and persist it under `name`.
fn save_form_as(e: &Editor, name: &str) -> Result<(), String> {
    let bm = Bookmark {
        name: name.to_string(),
        server: e.server_row.text().to_string(),
        port: e.port_row.text().to_string(),
        login: e.login_row.text().to_string(),
        password: e.pass_row.text().to_string(),
        // Absent means inherit, so an empty field is stored as nothing at all
        // rather than as an empty string — the file then says nothing about
        // this connection's identity, which is what makes deleting the line
        // equivalent to pressing "Use default".
        nick: Some(e.nick_row.text().to_string()).filter(|t| !t.is_empty()),
        icon: icon_override(),
        hope: e.hope_row.is_active(),
        cipher: cipher_vocab::dropdown_to_cipher_byte(e.cipher_row.selected()),
        compress: e.compress_row.selected() as u8,
        tls: e.tls_row.is_active(),
    };
    bookmark_store::upsert(bm)
}

// ======================================================================
// HOPE / TLS / cipher / compress coupling (mirrors connect.rs)
// ======================================================================

/// Grey out cipher / compress when HOPE is off; grey out HOPE + both when TLS
/// is on (and force HOPE off underneath — the two transports are mutually
/// exclusive). Same rule the Connect dialog enforces.
fn sync_sensitivity() {
    let Some(e) = editor() else { return };
    if e.tls_row.is_active() {
        if e.hope_row.is_active() {
            e.hope_row.set_active(false);
        }
        e.hope_row.set_sensitive(false);
        e.cipher_row.set_sensitive(false);
        e.compress_row.set_sensitive(false);
        return;
    }
    e.hope_row.set_sensitive(true);
    let on = e.hope_row.is_active();
    e.cipher_row.set_sensitive(on);
    e.compress_row.set_sensitive(on);
}

/// TLS-on flips the default port 5500 → 5600; TLS-off flips it back. Custom
/// ports are left alone.
fn on_tls_toggled() {
    let Some(e) = editor() else { return };
    let portstr = e.port_row.text();
    if e.tls_row.is_active() {
        if portstr == "5500" {
            e.port_row.set_text("5600");
        }
    } else if portstr == "5600" {
        e.port_row.set_text("5500");
    }
    sync_sensitivity();
}

/// A non-Off cipher / compress selection with HOPE off forces HOPE on —
/// silent plaintext-with-a-cipher is the wrong failure mode.
fn on_secure_combo_selected(selected: u32) {
    let Some(e) = editor() else { return };
    if selected != 0 && !e.hope_row.is_active() {
        e.hope_row.set_active(true);
    }
}

// ======================================================================
// Editor: actions
// ======================================================================

fn alert(anchor: &gtk::Widget, title: &str, msg: &str) {
    let dialog = adw::AlertDialog::new(Some(title), Some(msg));
    dialog.add_response("ok", &tr("_OK"));
    dialog.set_default_response(Some("ok"));
    dialog.set_close_response("ok");
    dialog.present(Some(anchor));
}

fn editor_error(e: &Editor, msg: &str) {
    alert(e.dialog.upcast_ref(), &tr("Connection error"), msg);
}

fn on_save() {
    let Some(e) = editor() else { return };
    let name = e.name_row.text().to_string();
    if name.is_empty() {
        editor_error(&e, &tr("A connection needs a name."));
        e.name_row.grab_focus();
        return;
    }

    // Rename first, if the name changed. Two failure modes to defend:
    //   1. Rename fails (the new name is already taken) — bail.
    //   2. Rename succeeds, then the content save fails — roll the rename
    //      back so the entry keeps its original name *and* contents.
    let orig = original_name();
    let mut renamed_from: Option<String> = None;
    match &orig {
        Some(o) if *o != name => {
            if let Err(msg) = bookmark_store::rename(o, &name) {
                editor_error(&e, &tr1("Rename failed: %s", &msg));
                return;
            }
            renamed_from = Some(o.clone());
            set_original_name(Some(name.clone()));
        }
        None => {
            // New entry: refuse to land on top of an existing one. `upsert`
            // would replace it silently, and the + button makes the collision
            // easy to reach — type a name you already have and your password
            // and identity for that server are gone.
            if bookmark_store::exists(&name) {
                editor_error(
                    &e,
                    &tr1("There is already a connection named \"%s\".", &name),
                );
                e.name_row.grab_focus();
                return;
            }
            set_original_name(Some(name.clone()));
        }
        _ => {}
    }

    match save_form_as(&e, &name) {
        Ok(()) => {
            unsafe { cffi::toolbar_refresh_bookmarks() };
            // Close before rebuilding: the rebuild drops the row this dialog
            // was presented on.
            e.dialog.close();
            rebuild_list();
        }
        Err(save_err) => match &renamed_from {
            None => editor_error(&e, &tr1("Save failed: %s", &save_err)),
            Some(from) => match bookmark_store::rename(&name, from) {
                Ok(()) => {
                    set_original_name(Some(from.clone()));
                    e.name_row.set_text(from);
                    editor_error(
                        &e,
                        &tr1(
                            "Save failed: %s. The connection was restored to its \
                             original name.",
                            &save_err,
                        ),
                    );
                }
                Err(rb) => editor_error(
                    &e,
                    &tr_fmt(
                        "Save failed: %1$s. The entry is now named \"%2$s\" — with \
                         the previous contents — because rolling the rename back \
                         also failed (%3$s).",
                        &[save_err.as_str(), name.as_str(), rb.as_str()],
                    ),
                ),
            },
        },
    }
}

fn on_delete() {
    let Some(e) = editor() else { return };
    let Some(orig) = original_name() else {
        // Never saved — closing is the same thing as discarding it.
        e.dialog.close();
        return;
    };

    let body = tr1(
        "Delete the connection \"%s\"? This cannot be undone.",
        &orig,
    );
    let dialog = adw::AlertDialog::new(Some(&tr("Delete Connection")), Some(&body));
    dialog.add_response("cancel", &tr("_Cancel"));
    dialog.add_response("delete", &tr("_Delete"));
    dialog.set_response_appearance("delete", adw::ResponseAppearance::Destructive);
    dialog.set_default_response(Some("cancel"));
    dialog.set_close_response("cancel");
    dialog.connect_response(None, move |_, resp| {
        if resp != "delete" {
            return;
        }
        let Some(e) = editor() else { return };
        let Some(orig) = original_name() else { return };
        match bookmark_store::delete(&orig) {
            Ok(()) => {
                unsafe { cffi::toolbar_refresh_bookmarks() };
                e.dialog.close();
                rebuild_list();
            }
            Err(msg) => editor_error(&e, &tr1("Delete failed: %s", &msg)),
        }
    });
    dialog.present(Some(e.dialog.upcast_ref::<gtk::Widget>()));
}

// ======================================================================
// Editor: construction
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

/// Open the editor for `name`, or for a new connection when `name` is `None`.
fn open_editor(anchor: &gtk::Widget, name: Option<&str>) {
    // One editor at a time: the thread-local holds exactly one, so a second
    // would silently steal the first's handlers. Re-presenting is right when
    // it is the *same* entry, but showing someone Server A's form because
    // they clicked Server B would be worse than either — so a different entry
    // closes the stale one first. (Reachable because an AdwDialog presented
    // from inside another is a sibling, not a child: closing Settings leaves
    // this one up and its `closed` handler unfired.)
    if let Some(open) = editor() {
        if original_name().as_deref() == name {
            open.dialog.present(Some(anchor));
            return;
        }
        open.dialog.close();
    }

    // A load failure must not read as "new". They arrive here as the same
    // `None`, and taking the new-entry branch would open a blank form under
    // the real entry's name — pressing Save then overwrites it with nothing.
    // That is the one destructive path in this page, so it is refused rather
    // than guessed at.
    let existing = match name {
        None => None,
        Some(name) => match load_bm(name) {
            Some(bm) => Some(bm),
            None => {
                alert(
                    anchor,
                    &tr("Connection error"),
                    &tr1(
                        "Could not load the connection \"%s\". It may have been \
                         changed or removed by something else.",
                        name,
                    ),
                );
                rebuild_list();
                return;
            }
        },
    };

    // RC4 migration, before the form is shown: the user must not be able to
    // Save back with cipher=0 ("no cipher") and silently turn a previously
    // encrypted connection into a plaintext one. The dialog rewrites the
    // entry in place, so reload to pick up the new byte.
    let existing = match existing {
        Some(d) if d.hope && d.cipher == cipher::RC4 => {
            let parent = anchor
                .root()
                .and_downcast::<gtk::Window>()
                .map(|w| w.as_ptr() as *mut cffi::GtkWindow)
                .unwrap_or(std::ptr::null_mut());
            if unsafe { crate::rc4_dialog::run_sync(parent, &d.name) } < 0 {
                return;
            }
            match load_bm(&d.name) {
                Some(d) => Some(d),
                None => return,
            }
        }
        other => other,
    };

    let dialog = adw::Dialog::new();
    dialog.set_title(&if existing.is_some() {
        tr("Edit Connection")
    } else {
        tr("New Connection")
    });
    dialog.set_content_width(560);
    dialog.set_content_height(660);
    dialog.set_size_request(360, 400);
    unsafe { cffi::gtkhx_dialog_add_close_shortcuts(dialog.as_ptr() as *mut _) };

    let page = adw::PreferencesPage::new();

    // ---- Server ----
    let server_group = adw::PreferencesGroup::new();
    server_group.set_title(&tr("Server"));

    let name_row = adw::EntryRow::new();
    name_row.set_title(&tr("Name"));
    server_group.add(&name_row);

    let server_row = adw::EntryRow::new();
    server_row.set_title(&tr("Address"));
    server_group.add(&server_row);

    let port_row = adw::EntryRow::new();
    port_row.set_title(&tr("Port (blank = 5500)"));
    server_group.add(&port_row);
    page.add(&server_group);

    // ---- Account ----
    let account_group = adw::PreferencesGroup::new();
    account_group.set_title(&tr("Account"));
    account_group.set_description(Some(&tr("Leave both blank to connect as a guest.")));

    let login_row = adw::EntryRow::new();
    login_row.set_title(&tr("Login"));
    account_group.add(&login_row);

    let pass_row = adw::PasswordEntryRow::new();
    pass_row.set_title(&tr("Password"));
    account_group.add(&pass_row);
    page.add(&account_group);

    // ---- Identity ----
    //
    // Separate from Account on purpose, even though both answer "who am I
    // here": the login is who the server *authenticates*, this is who
    // everyone *sees*. Putting them in one group made the four rows read as
    // one credential.
    let identity_group = adw::PreferencesGroup::new();
    identity_group.set_title(&tr("Identity on this server"));
    identity_group.set_description(Some(&tr(
        "Leave these alone to use your usual name and icon from Settings → \
         Identity. Changing those later moves every connection you haven't \
         given its own.",
    )));

    let nick_row = adw::EntryRow::new();
    set_nick_title(&nick_row);
    let nick_clear = gtk::Button::from_icon_name("edit-undo-symbolic");
    nick_clear.set_valign(gtk::Align::Center);
    nick_clear.set_tooltip_text(Some(&tr("Use the default nickname")));
    nick_clear.add_css_class("flat");
    nick_row.add_suffix(&nick_clear);
    identity_group.add(&nick_row);

    let icon_row = adw::ActionRow::new();
    icon_row.set_title(&tr("Icon"));
    let icon_preview = icon_picker::preview_widget(40);
    icon_row.add_prefix(&icon_preview);
    let icon_choose = gtk::Button::with_label(&tr("Choose…"));
    icon_choose.set_valign(gtk::Align::Center);
    let icon_clear = gtk::Button::from_icon_name("edit-undo-symbolic");
    icon_clear.set_valign(gtk::Align::Center);
    icon_clear.set_tooltip_text(Some(&tr("Use the default icon")));
    icon_clear.add_css_class("flat");
    icon_row.add_suffix(&icon_choose);
    icon_row.add_suffix(&icon_clear);
    identity_group.add(&icon_row);
    page.add(&identity_group);

    // ---- Transport ----
    let transport_group = adw::PreferencesGroup::new();
    transport_group.set_title(&tr("Transport"));

    // TLS first — toggling it greys out HOPE + cipher + compress (and forces
    // HOPE off underneath), and flips the default port 5500 ⇄ 5600, the same
    // coupling the Connect dialog enforces.
    let tls_row = adw::SwitchRow::new();
    tls_row.set_title(&tr("Use TLS"));
    tls_row.set_subtitle(&tr(
        "Connect to the server's TLS port. Disables HOPE and compression — \
         they're not meaningful over a TLS-encrypted stream.",
    ));
    tls_row.connect_active_notify(|_| on_tls_toggled());
    transport_group.add(&tls_row);

    let hope_row = adw::SwitchRow::new();
    hope_row.set_title(&tr("HOPE (encrypted handshake)"));
    hope_row.connect_active_notify(|_| sync_sensitivity());
    transport_group.add(&hope_row);

    let cipher_row = adw::ComboRow::new();
    cipher_row.set_title(&tr("Cipher"));
    cipher_row.set_model(Some(&combo_model(cipher_vocab::VALID_CIPHERS)));
    cipher_row.connect_selected_notify(|c| on_secure_combo_selected(c.selected()));
    transport_group.add(&cipher_row);

    let compress_row = adw::ComboRow::new();
    compress_row.set_title(&tr("Compression"));
    compress_row.set_model(Some(&combo_model(cipher_vocab::VALID_COMPRESSORS)));
    compress_row.connect_selected_notify(|c| on_secure_combo_selected(c.selected()));
    transport_group.add(&compress_row);
    page.add(&transport_group);

    // ---- Delete ----
    if existing.is_some() {
        let danger = adw::PreferencesGroup::new();
        let delete_row = adw::ActionRow::new();
        delete_row.set_title(&tr("Delete this connection"));
        delete_row.set_activatable(true);
        delete_row.add_css_class("error");
        delete_row.connect_activated(|_| on_delete());
        danger.add(&delete_row);
        page.add(&danger);
    }

    // ---- Chrome ----
    let header = adw::HeaderBar::new();
    header.set_show_end_title_buttons(false);
    let cancel = gtk::Button::with_label(&tr("Cancel"));
    let dialog_weak = dialog.downgrade();
    cancel.connect_clicked(move |_| {
        if let Some(d) = dialog_weak.upgrade() {
            d.close();
        }
    });
    header.pack_start(&cancel);
    let save = gtk::Button::with_label(&tr("Save"));
    save.add_css_class("suggested-action");
    save.connect_clicked(|_| on_save());
    header.pack_end(&save);

    let view = adw::ToolbarView::new();
    view.add_top_bar(&header);
    view.set_content(Some(&page));
    dialog.set_child(Some(&view));

    let e = Editor {
        dialog: dialog.clone(),
        name_row,
        server_row,
        port_row,
        login_row,
        pass_row,
        nick_row,
        nick_clear,
        icon_row,
        icon_preview,
        icon_clear,
        hope_row,
        tls_row,
        cipher_row,
        compress_row,
    };

    // Published before the form is filled: form_from_bookmark drives
    // sync_sensitivity, which reads the editor back out of the thread-local.
    EDITOR.with_borrow_mut(|slot| *slot = Some(e.clone()));

    // Identity handlers, wired after publishing for the same reason.
    e.nick_row.connect_changed(|_| {
        if let Some(e) = editor() {
            refresh_nick_clear(&e);
        }
    });
    e.nick_clear.connect_clicked(|_| {
        if let Some(e) = editor() {
            e.nick_row.set_text("");
        }
    });
    e.icon_clear.connect_clicked(|_| {
        let Some(e) = editor() else { return };
        set_icon_override(None);
        refresh_icon_row(&e);
    });
    icon_choose.connect_clicked(move |btn| {
        // One picker at a time — a second would start a second
        // full-catalogue render alongside the first.
        btn.set_sensitive(false);
        let btn_weak = btn.downgrade();
        // Seeded with the effective icon, so opening the picker on an
        // inheriting connection starts where the default is rather than at 0.
        let seed = icon_override().unwrap_or_else(global_icon);
        let picked = icon_picker::open(btn.upcast_ref(), seed, move |id| {
            let Some(e) = editor() else { return };
            set_icon_override(Some(id));
            refresh_icon_row(&e);
        });
        picked.connect_closed(move |_| {
            if let Some(btn) = btn_weak.upgrade() {
                btn.set_sensitive(true);
            }
        });
    });

    // Only tear down the state if this dialog is still the one it belongs to.
    //
    // `open_editor` closes a stale editor and immediately publishes a new one,
    // and AdwDialog::closed is not guaranteed to have run by then — it fires
    // after the close animation. An unconditional teardown would let the old
    // dialog's late signal wipe the *new* editor's widgets and its
    // original-name, leaving a visible form whose Save would take the
    // create-new branch and write a duplicate entry.
    dialog.connect_closed(|closing| {
        let is_current = EDITOR.with_borrow(|slot| {
            slot.as_ref()
                .is_some_and(|e| e.dialog.as_ptr() == closing.as_ptr())
        });
        if !is_current {
            return;
        }
        EDITOR.with_borrow_mut(|slot| *slot = None);
        set_original_name(None);
        set_icon_override(None);
    });

    match &existing {
        Some(d) => {
            set_original_name(Some(d.name.clone()));
            form_from_bookmark(&e, d);
        }
        None => {
            set_original_name(None);
            // HOPE on by default: it is the better transport where the server
            // supports it, and the Connect dialog defaults the same way.
            form_from_bookmark(
                &e,
                &Bookmark {
                    hope: true,
                    ..Default::default()
                },
            );
            e.name_row.grab_focus();
        }
    }

    dialog.present(Some(anchor));
}

// ======================================================================
// Legacy export
// ======================================================================

/// Pick a folder, then write every connection as a classic HTsc file into it.
///
/// The legacy format has no field for a per-connection nickname or icon, which
/// is correct rather than a gap — it is an interop format for clients with no
/// concept of one, so overrides simply don't round-trip through it.
fn on_export_legacy(anchor: &gtk::Widget) {
    let window = anchor.root().and_downcast::<gtk::Window>();
    let anchor = anchor.clone();
    let dialog = gtk::FileDialog::new();
    dialog.set_title(&tr("Export Connections (legacy format)"));
    dialog.select_folder(window.as_ref(), gtk::gio::Cancellable::NONE, move |res| {
        // Cancelled or dismissed → nothing to do.
        let Ok(folder) = res else { return };
        let Some(path) = folder.path() else { return };
        match bookmark_store::export_legacy(&path) {
            Ok(n) => alert(
                &anchor,
                &tr("Connections exported"),
                &tr1(
                    "Wrote %s file(s) in the legacy Hotline format.",
                    &n.to_string(),
                ),
            ),
            Err(e) => alert(&anchor, &tr("Export failed"), &tr1("Export failed: %s", &e)),
        }
    });
}

// ======================================================================
// The page
// ======================================================================

pub(crate) fn build(page: &adw::PreferencesPage) {
    let group = adw::PreferencesGroup::new();
    group.set_title(&tr("Connections"));
    group.set_description(Some(&tr(
        "Servers this client knows about. Each one remembers how to reach it, \
         how to log in, and optionally a nickname and icon to use there.",
    )));

    let add = gtk::Button::from_icon_name("list-add-symbolic");
    add.set_valign(gtk::Align::Center);
    add.set_tooltip_text(Some(&tr("Add a connection")));
    add.add_css_class("flat");
    add.connect_clicked(|btn| open_editor(btn.upcast_ref(), None));
    group.set_header_suffix(Some(&add));

    page.add(&group);
    // Order matters: publish the new group, *then* forget the previous page's
    // rows, and only then build. Rebuilding first would hand `group.remove()`
    // rows that belong to the old group, which is a GTK critical.
    LIST.with_borrow_mut(|l| *l = Some(group.downgrade()));
    ROWS.with_borrow_mut(|r| r.clear());
    rebuild_list();

    let tools = adw::PreferencesGroup::new();
    let export = adw::ActionRow::new();
    export.set_title(&tr("Export in the legacy format…"));
    export.set_subtitle(&tr(
        "Write one classic Hotline bookmark file per connection, for other \
         clients to import.",
    ));
    export.set_activatable(true);
    export.add_suffix(&gtk::Image::from_icon_name("document-export-symbolic"));
    export.connect_activated(|row| on_export_legacy(row.upcast_ref()));
    tools.add(&export);
    page.add(&tools);
}
