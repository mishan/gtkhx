//! The device picker and the device armed for an open, against a store of
//! their own.

use super::*;
use crate::options_device_keys::tests::hlid_home;
use hl_identity::IdentityKey;

fn now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_secs()
}

fn identity(seed: u8) -> String {
    IdentityKey::from_seed(&[seed; 32])
        .fingerprint()
        .to_string()
}

/// A device kept here is armed with the pins beside it; one that is not is
/// said so in plain words, and nothing is armed.
#[test]
fn a_device_kept_is_armed_and_one_that_is_not_is_said() {
    let dir = tempfile::tempdir().unwrap();
    let root = dir.path().join("device-keys");
    let home = hlid_home(dir.path(), (1, 101), "alice", None);
    Store::new(&root).import_hlid(&home, now()).unwrap();

    let a = armed(&root, &identity(1)).unwrap();
    assert_eq!(a.device.identity().to_string(), identity(1));
    assert_eq!(a.pins, dir.path().join("server-keys.toml"));
    for missing in [identity(2), "not a fingerprint".to_owned()] {
        assert_eq!(
            armed(&root, &missing).err().as_deref(),
            Some("That device key is no longer here."),
            "{missing}"
        );
    }
}

/// A connect arms its device for its own host and port, and leaves
/// nothing armed after, whether or not it opened; a device that does not
/// read arms nothing.
#[test]
fn a_connect_arms_its_device_for_itself_and_leaves_nothing_after() {
    let dir = tempfile::tempdir().unwrap();
    let root = dir.path().join("device-keys");
    let home = hlid_home(dir.path(), (1, 101), "alice", None);
    Store::new(&root).import_hlid(&home, now()).unwrap();

    let mut taken = None;
    around(&root, Some(&identity(1)), "hl.example", 5500, || {
        taken = hxnet::device::take_armed()
    })
    .unwrap();
    let taken = taken.expect("armed for the open");
    assert_eq!((taken.host.as_str(), taken.port), ("hl.example", 5500));
    assert_eq!(taken.armed.device.identity().to_string(), identity(1));

    around(&root, Some(&identity(1)), "hl.example", 5500, || {}).unwrap();
    assert!(
        hxnet::device::take_armed().is_none(),
        "left armed by a connect that never opened"
    );

    let why = around(&root, Some(&identity(2)), "hl.example", 5500, || {
        assert!(hxnet::device::take_armed().is_none(), "nothing to arm")
    });
    assert_eq!(
        why.err().as_deref(),
        Some("That device key is no longer here.")
    );
}

/// The picker offers none and each device kept here, chooses what a saved
/// connection names, and keeps one no longer kept here chosen.
pub(crate) fn check_picker() {
    let dir = tempfile::tempdir().unwrap();
    let root = dir.path().join("device-keys");
    for (who, name) in [((1, 101), "alice"), ((2, 102), "bob")] {
        let home = hlid_home(dir.path(), who, name, None);
        Store::new(&root).import_hlid(&home, now()).unwrap();
    }
    let p = Picker::new(&root);
    let labels = |p: &Picker| -> Vec<String> {
        let model = p.row.model().unwrap();
        (0..model.n_items())
            .map(|i| {
                model
                    .item(i)
                    .and_downcast::<gtk::StringObject>()
                    .unwrap()
                    .string()
                    .to_string()
            })
            .collect()
    };
    let shown = labels(&p);
    assert_eq!(shown.len(), 3, "{shown:?}");
    assert_eq!(shown[0], "No device key");
    assert!(shown.iter().any(|l| l.starts_with("bob (")), "{shown:?}");
    assert_eq!(p.chosen(), None, "none at first");

    p.choose(Some(&identity(2)));
    assert_eq!(p.chosen(), Some(identity(2)));
    assert!(labels(&p)[p.row.selected() as usize].starts_with("bob ("));
    p.choose(None);
    assert_eq!(p.chosen(), None);

    let gone = identity(3);
    p.choose(Some(&gone));
    assert_eq!(p.chosen(), Some(gone.clone()), "kept chosen");
    let label = &labels(&p)[p.row.selected() as usize];
    assert!(label.ends_with(" (missing)"), "{label}");
}
