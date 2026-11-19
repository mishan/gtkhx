//! The Device Keys settings page: the devices this computer proves as, one
//! for each identity, and the two ways one comes here, from an `hlid` home
//! or made here and certified with a bundle `hlid cert --bundle` writes.
//!
//! The keys are `hxkeys::Store`'s. This page lists them, adds them and
//! removes them, and says in plain words why it could not.

use std::cell::RefCell;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::rc::Rc;

use gtk4 as gtk;
use gtk4::prelude::*;
use gtk4::{gio, glib};
use hxkeys::{caps, Device, Error, Fingerprint, Pending, Store};
use libadwaita as adw;
use libadwaita::prelude::*;

use crate::options::group;
use crate::tr::{tr, tr_argv};

/// A bundle holds one certificate and one card, each far smaller.
const BUNDLE_MAX: u64 = 64 * 1024;

/// Where the keys are kept: beside the rest of a profile given with
/// `GTKHX_PATH`, so that one stays whole, and otherwise in the user's
/// data directory, apart from the settings a person may copy around.
#[cfg(not(test))]
pub(crate) fn store_root() -> PathBuf {
    match std::env::var_os("GTKHX_PATH").filter(|p| !p.is_empty()) {
        Some(profile) => PathBuf::from(profile).join("device-keys"),
        None => glib::user_data_dir().join("gtkhx").join("device-keys"),
    }
}

/// The server keys pinned for the devices kept at `root`, beside them.
pub(crate) fn pins_beside(root: &Path) -> PathBuf {
    root.with_file_name("server-keys.toml")
}

/// A test's keys are kept in its scratch configuration, never in the
/// keys of whoever runs it.
#[cfg(test)]
pub(crate) fn store_root() -> PathBuf {
    crate::bookmark_store::config_dir().join("device-keys")
}

pub(crate) fn build(page: &adw::PreferencesPage) {
    build_in(page, store_root());
}

/// The page's state: the store, and the groups it put on the page, which
/// a change replaces.
struct Ui {
    root: PathBuf,
    store: Store,
    page: glib::WeakRef<adw::PreferencesPage>,
    groups: RefCell<Vec<adw::PreferencesGroup>>,
}

/// Why a change did not happen.
enum Failed {
    Store(Error),
    Bundle(std::io::Error),
    BundleTooLarge,
}

impl From<Error> for Failed {
    fn from(e: Error) -> Self {
        Failed::Store(e)
    }
}

/// Lay the page out. The page keeps the state alive, and its rows hold it
/// weakly, so it goes with the page.
fn build_in(page: &adw::PreferencesPage, root: PathBuf) -> Rc<Ui> {
    let ui = Rc::new(Ui {
        store: Store::new(&root),
        root,
        page: page.downgrade(),
        groups: RefCell::new(Vec::new()),
    });
    // SAFETY: stored once under this key, and read back by nobody as
    // another type.
    unsafe { page.set_data("gtkhx-device-keys", ui.clone()) };
    refresh(&ui);
    ui
}

fn now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_secs())
}

/// Lay the page out afresh from what the store holds now.
fn refresh(ui: &Rc<Ui>) {
    let Some(page) = ui.page.upgrade() else {
        return;
    };
    for g in ui.groups.borrow_mut().drain(..) {
        page.remove(&g);
    }
    let mut groups = vec![keys_group(ui)];
    if let Some(g) = pending_group(ui) {
        groups.push(g);
    }
    groups.push(add_group(ui));
    for g in &groups {
        page.add(g);
    }
    *ui.groups.borrow_mut() = groups;
}

fn keys_group(ui: &Rc<Ui>) -> adw::PreferencesGroup {
    let grp = group(&tr("Device Keys"));
    grp.set_description(Some(&tr(
        "The devices this computer proves as, one for each identity. A server that proves \
         devices is shown the device's certificate when you log in.",
    )));
    let listed = match ui.store.list() {
        Ok(l) => l,
        Err(e) => {
            grp.add(&note_row(&plain(&e.into())));
            return grp;
        }
    };
    if listed.is_empty() {
        grp.add(&note_row(&tr("No device keys yet.")));
    }
    for (fp, device) in listed {
        let row = adw::ActionRow::new();
        row.set_subtitle_selectable(true);
        let name = match device {
            Ok(d) => {
                row.set_title(&glib::markup_escape_text(&d.card.name));
                row.set_subtitle(&glib::markup_escape_text(&format!(
                    "{fp}\n{}",
                    summary(&d, now())
                )));
                d.card.name
            }
            Err(e) => {
                row.set_title(&fp.to_string());
                row.set_subtitle(&glib::markup_escape_text(&plain(&e.into())));
                fp.short()
            }
        };
        row.set_subtitle_lines(0);
        let remove = flat_icon(
            "user-trash-symbolic",
            // TRANSLATORS: a button's name; %s is the identity's display name.
            &tr_argv("Remove the device key of %s", &[&name]),
        );
        let weak = Rc::downgrade(ui);
        remove.connect_clicked(move |_| {
            if let Some(ui) = weak.upgrade() {
                confirm(
                    &ui,
                    // TRANSLATORS: a dialog's heading; %s is the identity's
                    // display name.
                    &tr_argv("Remove the Device Key of %s?", &[&name]),
                    &tr(
                        "This deletes the device key from this computer. This computer can prove \
                         this identity again only after a device key is imported or certified \
                         again.",
                    ),
                    &tr("_Remove"),
                    move |store| Ok(store.remove(&fp)?),
                );
            }
        });
        row.add_suffix(&remove);
        grp.add(&row);
    }
    grp
}

fn pending_group(ui: &Rc<Ui>) -> Option<adw::PreferencesGroup> {
    let rows: Vec<adw::ActionRow> = match ui.store.pending() {
        Ok(p) if p.is_empty() => return None,
        Ok(p) => p.iter().map(|p| pending_row(ui, p)).collect(),
        Err(e) => vec![note_row(&plain(&e.into()))],
    };
    let grp = group(&tr("Waiting for a Certificate"));
    grp.set_description(Some(&tr(
        "Run the command where the identity key is kept, then add the bundle it writes.",
    )));
    for row in rows {
        grp.add(&row);
    }
    Some(grp)
}

/// What `hlid cert --bundle` is told to write for `p`, named for it so
/// that two devices waiting do not write one file.
fn bundle_name(p: &Pending) -> String {
    format!("gtkhx-{}.bundle", p.device.short())
}

fn pending_row(ui: &Rc<Ui>, p: &Pending) -> adw::ActionRow {
    let command = p.hlid_command(&bundle_name(p));
    let short = p.device.short();
    let row = adw::ActionRow::new();
    // TRANSLATORS: %s is the start of the new device's fingerprint.
    row.set_title(&tr_argv("New device %s", &[&short]));
    row.set_subtitle(&format!("<tt>{}</tt>", glib::markup_escape_text(&command)));
    row.set_subtitle_selectable(true);
    row.set_subtitle_lines(0);

    let copy = flat_icon("edit-copy-symbolic", &tr("Copy the command"));
    copy.connect_clicked(move |b| {
        b.clipboard().set_text(&command);
        if let Some(toasts) = b.ancestor(adw::ToastOverlay::static_type()) {
            toasts
                .downcast::<adw::ToastOverlay>()
                .unwrap()
                .add_toast(adw::Toast::new(&tr("Command copied")));
        }
    });
    row.add_suffix(&copy);

    let finish = gtk::Button::with_mnemonic(&tr("_Add Certificate…"));
    finish.set_valign(gtk::Align::Center);
    let (weak, device) = (Rc::downgrade(ui), p.device);
    finish.connect_clicked(move |b| {
        if let Some(ui) = weak.upgrade() {
            choose_bundle(&ui, b, device);
        }
    });
    row.add_suffix(&finish);

    let discard = flat_icon(
        "user-trash-symbolic",
        // TRANSLATORS: a button's name; %s is the start of the new device's
        // fingerprint.
        &tr_argv("Discard the new device %s", &[&short]),
    );
    let (weak, device) = (Rc::downgrade(ui), p.device);
    discard.connect_clicked(move |_| {
        if let Some(ui) = weak.upgrade() {
            confirm(
                &ui,
                &tr("Discard New Device?"),
                &tr("The key made for it is deleted, and it can no longer be certified."),
                &tr("_Discard"),
                move |store| Ok(store.abandon_device(&device)?),
            );
        }
    });
    row.add_suffix(&discard);
    row
}

fn add_group(ui: &Rc<Ui>) -> adw::PreferencesGroup {
    let grp = group(&tr("Add a Device Key"));

    let import = adw::ActionRow::new();
    import.set_title(&tr("Import from hlid"));
    import.set_subtitle(&tr(
        "The device hlid init made, from its folder (~/.hlid unless HLID_HOME says otherwise). \
         The identity key beside it is not read.",
    ));
    let choose = gtk::Button::with_mnemonic(&tr("_Choose Folder…"));
    choose.set_valign(gtk::Align::Center);
    let weak = Rc::downgrade(ui);
    choose.connect_clicked(move |b| {
        if let Some(ui) = weak.upgrade() {
            choose_home(&ui, b);
        }
    });
    import.add_suffix(&choose);
    grp.add(&import);

    let make = adw::ActionRow::new();
    make.set_title(&tr("Make a device key here"));
    make.set_subtitle(&tr(
        "A new key for this computer alone, which the identity then certifies with hlid cert.",
    ));
    let begin = gtk::Button::with_mnemonic(&tr("_Make Key"));
    begin.set_valign(gtk::Align::Center);
    let weak = Rc::downgrade(ui);
    begin.connect_clicked(move |_| {
        if let Some(ui) = weak.upgrade() {
            run(&ui, |store| Ok(store.begin_device().map(|_| ())?));
        }
    });
    make.add_suffix(&begin);
    grp.add(&make);
    grp
}

fn flat_icon(icon: &str, name: &str) -> gtk::Button {
    let b = crate::icon_button(icon, name);
    b.set_valign(gtk::Align::Center);
    b.add_css_class("flat");
    b
}

fn note_row(text: &str) -> adw::ActionRow {
    let row = adw::ActionRow::new();
    row.set_title(&glib::markup_escape_text(text));
    row.add_css_class("dim-label");
    row
}

/// What a device can do and until when, for its row.
fn summary(d: &Device, now: u64) -> String {
    let date = glib::DateTime::from_unix_local(d.cert.expires as i64)
        .and_then(|t| t.format("%Y/%m/%d"))
        .map(|s| s.to_string())
        .unwrap_or_default();
    let until = if d.cert.expires < now {
        // TRANSLATORS: %s is a date, such as 2027/02/16.
        tr_argv("Expired %s", &[&date])
    } else {
        // TRANSLATORS: %s is a date, such as 2027/02/16.
        tr_argv("Expires %s", &[&date])
    };
    let can = match (d.cert.allows(caps::MESSAGE), d.cert.allows(caps::MANAGE)) {
        (true, true) => tr("can message and manage the identity"),
        (true, false) => tr("can message"),
        (false, true) => tr("can manage the identity"),
        (false, false) => return until,
    };
    // TRANSLATORS: a device key's row: %1$s is when its certificate expires,
    // %2$s what it may do, such as "can message".
    tr_argv("%1$s · %2$s", &[&until, &can])
}

/// What went wrong, in words a person can act on.
fn plain(f: &Failed) -> String {
    match f {
        Failed::Store(e) => plain_error(e),
        Failed::Bundle(err) => {
            // TRANSLATORS: %s is the system's reason.
            tr_argv("The bundle could not be read: %s", &[&err.to_string()])
        }
        Failed::BundleTooLarge => tr("That file is too large to be a certificate bundle."),
    }
}

/// What the store refused, in words a person can act on.
pub(crate) fn plain_error(e: &Error) -> String {
    match e {
        Error::Io(err) => tr_argv(
            // TRANSLATORS: %s is the system's reason.
            "The device keys could not be read or written: %s",
            &[&err.to_string()],
        ),
        Error::Unreadable { file, .. } => {
            // TRANSLATORS: %s is a file name, such as device.key.
            tr_argv("%s is missing or is not what hlid writes there.", &[file])
        }
        Error::NotFound => tr("That device key is no longer here."),
        Error::WrongDevice => tr("The certificate is for another device than this key."),
        Error::WrongIdentity => tr("The card is another identity's than the certificate's."),
        Error::NoLogin => tr("The certificate does not let this device log in."),
        Error::Expired { .. } => tr("The certificate has expired. Ask the identity for a new one."),
        Error::NotYetValid { .. } => {
            tr("The certificate is not valid yet. Check this computer's clock.")
        }
        // TRANSLATORS: %s is a file name, such as card.bin.
        Error::TooLarge { file } => tr_argv("%s is too large to prove with.", &[file]),
        Error::OtherDevice(fp) => tr_argv(
            // TRANSLATORS: %s is the start of the other device's fingerprint.
            "Another device of this identity is kept here (%s). Remove it first.",
            &[&fp.short()],
        ),
        Error::Older { .. } => tr("The device key kept here has a newer certificate."),
        Error::WrongHost => tr("The server's challenge was for another address."),
        Error::PinChanged => {
            tr("The server's key was changed here meanwhile. Connect again to see it.")
        }
    }
}

/// Say why something could not be done.
fn fail(ui: &Ui, text: &str) {
    let dialog = adw::AlertDialog::new(Some(&tr("Could Not Change Device Keys")), Some(text));
    dialog.add_response("ok", &tr("_OK"));
    dialog.present(ui.page.upgrade().as_ref());
    #[cfg(test)]
    tests::LAST_DIALOG.with_borrow_mut(|d| *d = Some(dialog.clone()));
}

/// Do `job` off the main thread, on a store of its own, since it waits for
/// the store's lock and for the disk; the page takes no input meanwhile,
/// then is laid out afresh or says why not.
fn run(ui: &Rc<Ui>, job: impl FnOnce(&Store) -> Result<(), Failed> + Send + 'static) {
    let Some(page) = ui.page.upgrade() else {
        return;
    };
    page.set_sensitive(false);
    let root = ui.root.clone();
    let weak = Rc::downgrade(ui);
    glib::spawn_future_local(async move {
        let done = gio::spawn_blocking(move || job(&Store::new(root))).await;
        page.set_sensitive(true);
        let Some(ui) = weak.upgrade() else {
            return;
        };
        match done {
            Ok(Ok(())) => refresh(&ui),
            Ok(Err(f)) => fail(&ui, &plain(&f)),
            Err(_) => fail(&ui, &tr("The device keys could not be changed.")),
        }
    });
}

/// Ask before deleting a key, with `heading`, `body` and the verb that
/// does it, then do it with `job`.
fn confirm(
    ui: &Rc<Ui>,
    heading: &str,
    body: &str,
    verb: &str,
    job: impl FnOnce(&Store) -> Result<(), Failed> + Send + 'static,
) {
    let dialog = adw::AlertDialog::new(Some(heading), Some(body));
    dialog.add_response("cancel", &tr("_Cancel"));
    dialog.add_response("remove", verb);
    dialog.set_response_appearance("remove", adw::ResponseAppearance::Destructive);
    dialog.set_default_response(Some("cancel"));
    dialog.set_close_response("cancel");
    let weak = Rc::downgrade(ui);
    let job = RefCell::new(Some(job));
    dialog.connect_response(None, move |_, response| {
        if response != "remove" {
            return;
        }
        if let (Some(ui), Some(job)) = (weak.upgrade(), job.take()) {
            run(&ui, job);
        }
    });
    dialog.present(ui.page.upgrade().as_ref());
    #[cfg(test)]
    tests::LAST_DIALOG.with_borrow_mut(|d| *d = Some(dialog.clone()));
}

/// Say a file chooser's failure, but not its being dismissed.
fn chooser_failed(ui: &Ui, err: &glib::Error) {
    if !err.matches(gtk::DialogError::Dismissed) {
        fail(
            ui,
            // TRANSLATORS: %s is the system's reason.
            &tr_argv("The file chooser failed: %s", &[err.message()]),
        );
    }
}

fn choose_home(ui: &Rc<Ui>, anchor: &gtk::Button) {
    let dialog = gtk::FileDialog::new();
    dialog.set_title(&tr("Choose the hlid Folder"));
    let home = std::env::var_os("HLID_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| glib::home_dir().join(".hlid"));
    // Set whether or not it is seen from here: a sandbox does not see it,
    // and the portal opens it outside.
    dialog.set_initial_folder(Some(&gio::File::for_path(&home)));
    let window = anchor.root().and_downcast::<gtk::Window>();
    let weak = Rc::downgrade(ui);
    dialog.select_folder(window.as_ref(), gio::Cancellable::NONE, move |res| {
        let Some(ui) = weak.upgrade() else {
            return;
        };
        match res.map(|f| f.path()) {
            Ok(Some(path)) => import_from(&ui, path),
            Ok(None) => fail(&ui, &tr("That folder is not on a local disk.")),
            Err(err) => chooser_failed(&ui, &err),
        }
    });
}

fn import_from(ui: &Rc<Ui>, home: PathBuf) {
    run(ui, move |store| {
        Ok(store.import_hlid(&home, now()).map(|_| ())?)
    });
}

fn choose_bundle(ui: &Rc<Ui>, anchor: &gtk::Button, device: Fingerprint) {
    let dialog = gtk::FileDialog::new();
    dialog.set_title(&tr("Choose the Certificate Bundle"));
    let window = anchor.root().and_downcast::<gtk::Window>();
    let weak = Rc::downgrade(ui);
    dialog.open(window.as_ref(), gio::Cancellable::NONE, move |res| {
        let Some(ui) = weak.upgrade() else {
            return;
        };
        match res.map(|f| f.path()) {
            Ok(Some(path)) => finish_from(&ui, device, path),
            Ok(None) => fail(&ui, &tr("That file is not on a local disk.")),
            Err(err) => chooser_failed(&ui, &err),
        }
    });
}

fn finish_from(ui: &Rc<Ui>, device: Fingerprint, path: PathBuf) {
    run(ui, move |store| {
        let bundle = read_bundle(&path)?;
        Ok(store.finish_device(&device, &bundle, now()).map(|_| ())?)
    });
}

/// The bundle at `path`, read no further than a bundle can be long.
fn read_bundle(path: &Path) -> Result<Vec<u8>, Failed> {
    let mut bundle = Vec::new();
    std::fs::File::open(path)
        .and_then(|f| f.take(BUNDLE_MAX + 1).read_to_end(&mut bundle))
        .map_err(Failed::Bundle)?;
    if bundle.len() as u64 > BUNDLE_MAX {
        return Err(Failed::BundleTooLarge);
    }
    Ok(bundle)
}

#[cfg(test)]
#[path = "options_device_keys_tests.rs"]
pub(crate) mod tests;
