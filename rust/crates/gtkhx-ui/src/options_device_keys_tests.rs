//! The Device Keys page against a store of its own.

use super::*;
use hl_identity::{cert, Card, DeviceCert, DeviceKey, IdentityKey};

thread_local! {
    pub(super) static LAST_DIALOG: RefCell<Option<adw::AlertDialog>> = const { RefCell::new(None) };
}

/// The dialog the page last showed, taken.
fn shown() -> adw::AlertDialog {
    LAST_DIALOG
        .with_borrow_mut(Option::take)
        .expect("a dialog was shown")
}

/// An hlid home in `dir` holding a device of `name`'s identity, certified
/// with `caps`.
fn hlid_home(dir: &Path, (seed, device): (u8, u8), name: &str, caps: Option<u64>) -> PathBuf {
    let id = IdentityKey::from_seed(&[seed; 32]);
    let dev = DeviceKey::from_seed(&[device; 32]);
    let home = dir.join(format!("hlid-{seed}-{device}"));
    std::fs::create_dir_all(&home).unwrap();
    let hex: String = dev.seed().iter().map(|b| format!("{b:02x}")).collect();
    std::fs::write(home.join("device.key"), hex).unwrap();
    let mut c = DeviceCert::for_device(&id, &dev, now() - 60, cert::RECOMMENDED_LIFETIME).unwrap();
    c.caps = caps;
    std::fs::write(home.join("cert.bin"), c.sign(&id)).unwrap();
    let card = Card::new(&id, name, now()).sign(&id, vec![]).unwrap();
    std::fs::write(home.join("card.bin"), card).unwrap();
    home
}

/// Every widget of type `T` under `w`, in order.
fn all<T: IsA<gtk::Widget>>(w: &gtk::Widget) -> Vec<T> {
    let mut out = Vec::new();
    let mut child = w.first_child();
    while let Some(c) = child {
        if let Some(t) = c.downcast_ref::<T>() {
            out.push(t.clone());
        }
        out.extend(all::<T>(&c));
        child = c.next_sibling();
    }
    out
}

fn titled(page: &adw::PreferencesPage, title: &str) -> Option<adw::ActionRow> {
    all::<adw::ActionRow>(page.upcast_ref())
        .into_iter()
        .find(|r| r.title() == title)
}

/// The button of `row` named `name`, by its tooltip or its label.
fn button(row: &adw::ActionRow, name: &str) -> gtk::Button {
    all::<gtk::Button>(row.upcast_ref())
        .into_iter()
        .find(|b| b.tooltip_text().as_deref() == Some(name) || b.label().as_deref() == Some(name))
        .unwrap_or_else(|| panic!("no button {name:?}"))
}

/// Run the main loop until the page takes input again: a change is done.
fn settle(page: &adw::PreferencesPage) {
    let ctx = glib::MainContext::default();
    let until = std::time::Instant::now() + std::time::Duration::from_secs(10);
    while !page.is_sensitive() {
        assert!(std::time::Instant::now() < until, "the change never ended");
        ctx.iteration(false);
    }
}

/// The page lists each identity by name and fingerprint, with when its
/// certificate ends and what it may do; a new device waits with the
/// command that certifies it; removing asks first and says the key goes;
/// a change waits for the store off the main loop; and what the store
/// refuses is said in plain words.
pub(crate) fn check_device_keys_page() {
    let dir = tempfile::tempdir().unwrap();
    let root = dir.path().join("device-keys");
    let store = Store::new(&root);
    let alice_home = hlid_home(dir.path(), (1, 101), "alice", None);
    store.import_hlid(&alice_home, now()).unwrap();
    let bob_home = hlid_home(dir.path(), (2, 102), "bob", Some(caps::LOGIN));
    store.import_hlid(&bob_home, now()).unwrap();
    let pending = store.begin_device().unwrap();
    let short = pending.device.short();

    let page = adw::PreferencesPage::new();
    // Held by nothing but the page, as the Settings window has it.
    let ui = Rc::downgrade(&build_in(&page, root.clone()));
    let alice = titled(&page, "alice").expect("alice listed");
    let fp = IdentityKey::from_seed(&[1; 32]).fingerprint().to_string();
    let sub = alice.subtitle().unwrap_or_default();
    assert!(sub.contains(&fp), "{sub}");
    assert!(sub.contains("can message and manage"), "{sub}");
    let bob = titled(&page, "bob")
        .expect("bob listed")
        .subtitle()
        .unwrap_or_default();
    assert!(bob.contains("Expires") && !bob.contains("can "), "{bob}");
    let date = bob.split("Expires ").nth(1).unwrap_or_default();
    let shape: String = date
        .chars()
        .take(10)
        .map(|c| if c.is_ascii_digit() { '9' } else { c })
        .collect();
    assert_eq!(shape, "9999/99/99", "the expiry is year first: {bob}");
    let waiting = titled(&page, &format!("New device {short}")).expect("waiting");
    let cmd = waiting.subtitle().unwrap_or_default();
    assert!(
        cmd.contains("hlid cert --device-pub") && cmd.contains(&format!("-o gtkhx-{short}.bundle")),
        "each device waiting writes a bundle of its own: {cmd}"
    );

    // Removing asks, and cancelling keeps the key.
    let remove = "Remove the device key of alice";
    button(&alice, remove).emit_clicked();
    let ask = shown();
    assert!(ask
        .body()
        .contains("deletes the device key from this computer"));
    ask.emit_by_name::<()>("response", &[&"cancel"]);
    assert_eq!(store.list().unwrap().len(), 2);
    button(&alice, remove).emit_clicked();
    shown().emit_by_name::<()>("response", &[&"remove"]);
    settle(&page);
    assert_eq!(store.list().unwrap().len(), 1);
    assert!(titled(&page, "alice").is_none(), "no longer listed");

    let ui = ui.upgrade().expect("the page keeps its state");
    // Imported again while another holds the store, the main loop runs on
    // and the page waits; then it is listed again.
    let held = std::fs::File::open(root.join(".lock")).unwrap();
    held.lock().unwrap();
    let release = std::thread::spawn(move || {
        std::thread::sleep(std::time::Duration::from_secs(1));
        drop(held);
    });
    let ticks = Rc::new(std::cell::Cell::new(0));
    let tick = glib::timeout_add_local(std::time::Duration::from_millis(20), {
        let ticks = ticks.clone();
        move || {
            ticks.set(ticks.get() + 1);
            glib::ControlFlow::Continue
        }
    });
    let asked = std::time::Instant::now();
    import_from(&ui, alice_home.clone());
    while asked.elapsed() < std::time::Duration::from_millis(500) {
        glib::MainContext::default().iteration(true);
    }
    tick.remove();
    assert!(
        ticks.get() >= 10,
        "the main loop stopped while the store was held"
    );
    assert!(!page.is_sensitive(), "the page waits for the change");
    settle(&page);
    release.join().unwrap();
    assert!(titled(&page, "alice").is_some(), "listed once imported");

    // A folder that holds no device, said so in words.
    import_from(&ui, dir.path().join("nowhere"));
    settle(&page);
    let said = shown();
    assert_eq!(
        said.heading().as_deref(),
        Some("Could Not Change Device Keys")
    );
    assert_eq!(
        said.body(),
        "device.key is missing or is not what hlid writes there."
    );
    // Another device of an identity kept here.
    import_from(&ui, hlid_home(dir.path(), (2, 103), "bob", None));
    settle(&page);
    assert!(shown().body().contains("Remove it first"));

    // A bundle that is not there, or far too long to be one.
    finish_from(&ui, pending.device, dir.path().join("none.bundle"));
    settle(&page);
    let body = shown().body();
    assert!(body.starts_with("The bundle could not be read: "), "{body}");
    let big = dir.path().join("big.bundle");
    std::fs::write(&big, vec![0u8; 1 << 20]).unwrap();
    finish_from(&ui, pending.device, big);
    settle(&page);
    assert_eq!(
        shown().body(),
        "That file is too large to be a certificate bundle."
    );

    // Discarding the device waiting asks in its own words.
    let waiting = titled(&page, &format!("New device {short}")).expect("waiting");
    button(&waiting, &format!("Discard the new device {short}")).emit_clicked();
    let ask = shown();
    assert_eq!(ask.heading().as_deref(), Some("Discard New Device?"));
    ask.emit_by_name::<()>("response", &[&"remove"]);
    settle(&page);
    assert!(store.pending().unwrap().is_empty());
    assert!(titled(&page, &format!("New device {short}")).is_none());

    // Devices waiting that cannot be read are said so, not passed over.
    let broken = dir.path().join("broken");
    std::fs::create_dir(&broken).unwrap();
    std::fs::write(broken.join("pending"), "").unwrap();
    let page = adw::PreferencesPage::new();
    build_in(&page, broken);
    let titles: Vec<String> = all::<adw::ActionRow>(page.upcast_ref())
        .iter()
        .map(|r| r.title().to_string())
        .collect();
    assert!(
        titles
            .iter()
            .any(|t| t.starts_with("The device keys could not be read or written")),
        "{titles:?}"
    );
}
