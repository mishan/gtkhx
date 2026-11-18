//! The device store: devices taken from an hlid home or certified from a
//! bundle, several identities side by side, and what is refused.

use std::path::{Path, PathBuf};

use hl_identity::{caps, cert, Bundle, Card, DeviceCert, DeviceKey, Fingerprint, IdentityKey};
use hxkeys::{Error, Store};

const NOW: u64 = 1_800_000_000;

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

/// A certificate for `dev` from `id`, issued an hour ago.
fn cert_for(id: &IdentityKey, dev: &DeviceKey, edit: impl FnOnce(&mut DeviceCert)) -> Vec<u8> {
    let mut c = DeviceCert::for_device(id, dev, NOW - 3600, cert::RECOMMENDED_LIFETIME).unwrap();
    edit(&mut c);
    c.sign(id)
}

fn card_for(id: &IdentityKey, name: &str) -> Vec<u8> {
    Card::new(id, name, NOW).sign(id, Vec::new()).unwrap()
}

/// An hlid home as `hlid init` lays one out, identity key included.
fn hlid_home(
    dir: &Path,
    id: &IdentityKey,
    dev: &DeviceKey,
    cert: Vec<u8>,
    card: Vec<u8>,
) -> PathBuf {
    let home = dir.join(format!("hlid-{}", id.fingerprint().short()));
    std::fs::create_dir_all(&home).unwrap();
    std::fs::write(home.join("identity.key"), format!("{}\n", hex(&*id.seed()))).unwrap();
    std::fs::write(home.join("device.key"), format!("{}\n", hex(&*dev.seed()))).unwrap();
    std::fs::write(home.join("cert.bin"), cert).unwrap();
    std::fs::write(home.join("card.bin"), card).unwrap();
    home
}

fn person(seed: u8) -> (IdentityKey, DeviceKey) {
    (
        IdentityKey::from_seed(&[seed; 32]),
        DeviceKey::from_seed(&[seed + 100; 32]),
    )
}

fn every_file(dir: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    for e in std::fs::read_dir(dir).unwrap() {
        let p = e.unwrap().path();
        if p.is_dir() {
            out.extend(every_file(&p));
        } else {
            out.push(p);
        }
    }
    out
}

#[test]
fn a_device_taken_from_an_hlid_home_leaves_its_identity_key_behind() {
    let tmp = tempfile::tempdir().unwrap();
    let store = Store::new(tmp.path().join("device-keys"));
    let (id, dev) = person(1);
    let home = hlid_home(
        tmp.path(),
        &id,
        &dev,
        cert_for(&id, &dev, |_| {}),
        card_for(&id, "alice"),
    );

    let d = store.import_hlid(&home, NOW).unwrap();
    assert_eq!(d.identity(), id.fingerprint());
    assert_eq!(d.device(), dev.fingerprint());
    assert_eq!(d.card.name, "alice");
    assert_eq!(d.key().unwrap().public(), dev.public());

    let identity_seed = hex(&*id.seed());
    for f in every_file(&tmp.path().join("device-keys")) {
        let text = String::from_utf8_lossy(&std::fs::read(&f).unwrap()).into_owned();
        assert!(!text.contains(&identity_seed), "{}", f.display());
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let entry = tmp
            .path()
            .join("device-keys")
            .join(id.fingerprint().to_string());
        let mode = |p: &Path| std::fs::metadata(p).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode(&tmp.path().join("device-keys")), 0o700);
        assert_eq!(mode(&entry), 0o700);
        assert_eq!(mode(&entry.join("device.key")), 0o600);
        let pending = store.begin_device().unwrap();
        let dir = tmp.path().join("device-keys").join("pending");
        assert_eq!(mode(&dir), 0o700);
        assert_eq!(mode(&dir.join(format!("{}.key", pending.device))), 0o600);
        store.abandon_device(&pending.device).unwrap();
    }
    let listed = store.list().unwrap();
    assert_eq!(listed.len(), 1);
    assert_eq!(listed[0].0, id.fingerprint());
    assert_eq!(listed[0].1.as_ref().unwrap().cert_bytes(), d.cert_bytes());
}

#[test]
fn several_identities_are_kept_side_by_side_and_removed_one_at_a_time() {
    let tmp = tempfile::tempdir().unwrap();
    let store = Store::new(tmp.path().join("device-keys"));
    let mut fps = Vec::new();
    for (seed, name) in [(1, "alice"), (2, "alice at work"), (3, "bob")] {
        let (id, dev) = person(seed);
        let home = hlid_home(
            tmp.path(),
            &id,
            &dev,
            cert_for(&id, &dev, |_| {}),
            card_for(&id, name),
        );
        store.import_hlid(&home, NOW).unwrap();
        fps.push(id.fingerprint());
    }
    let mut listed: Vec<Fingerprint> = store
        .list()
        .unwrap()
        .into_iter()
        .map(|(fp, d)| {
            assert_eq!(d.unwrap().identity(), fp);
            fp
        })
        .collect();
    listed.sort_by_key(|f| f.to_string());
    fps.sort_by_key(|f| f.to_string());
    assert_eq!(listed, fps);

    store.remove(&fps[1]).unwrap();
    assert!(matches!(store.get(&fps[1]), Err(Error::NotFound)));
    assert!(matches!(store.remove(&fps[1]), Err(Error::NotFound)));
    assert_eq!(store.list().unwrap().len(), 2);
    assert!(store.get(&fps[0]).is_ok() && store.get(&fps[2]).is_ok());
}

/// What is refused, the certificate and card, and the refusal expected.
type Case = (&'static str, Vec<u8>, Vec<u8>, fn(&Error) -> bool);

#[test]
fn a_device_that_cannot_prove_is_refused_and_nothing_is_kept() {
    let tmp = tempfile::tempdir().unwrap();
    let (id, dev) = person(1);
    let (other_id, other_dev) = person(2);
    let card = card_for(&id, "alice");
    let cases: Vec<Case> = vec![
        (
            "another device's certificate",
            cert_for(&id, &other_dev, |_| {}),
            card.clone(),
            |e| matches!(e, Error::WrongDevice),
        ),
        (
            "another identity's card",
            cert_for(&id, &dev, |_| {}),
            card_for(&other_id, "bob"),
            |e| matches!(e, Error::WrongIdentity),
        ),
        (
            "a certificate that may not log in",
            cert_for(&id, &dev, |c| c.caps = Some(caps::MESSAGE)),
            card.clone(),
            |e| matches!(e, Error::NoLogin),
        ),
        (
            "an expired certificate",
            cert_for(&id, &dev, |c| {
                c.issued = NOW - 7200;
                c.expires = NOW - 3600
            }),
            card.clone(),
            |e| matches!(e, Error::Expired { expires } if *expires == NOW - 3600),
        ),
        (
            "a certificate not yet valid",
            cert_for(&id, &dev, |c| {
                c.issued = NOW + 3600;
                c.expires = NOW + 7200
            }),
            card.clone(),
            |e| matches!(e, Error::NotYetValid { .. }),
        ),
        (
            "a certificate that does not read",
            b"not a certificate".to_vec(),
            card.clone(),
            |e| {
                matches!(
                    e,
                    Error::Unreadable {
                        file: "cert.bin",
                        ..
                    }
                )
            },
        ),
        (
            "a certificate too large to send",
            vec![0; 513],
            card.clone(),
            |e| matches!(e, Error::TooLarge { file: "cert.bin" }),
        ),
    ];
    for (i, (what, cert, card, refused)) in cases.into_iter().enumerate() {
        let store = Store::new(tmp.path().join(format!("store-{i}")));
        let home = hlid_home(&tmp.path().join(format!("home-{i}")), &id, &dev, cert, card);
        let e = store.import_hlid(&home, NOW).expect_err(what);
        assert!(refused(&e), "{what}: {e:?}");
        assert!(store.list().unwrap().is_empty(), "{what}");
    }
    let store = Store::new(tmp.path().join("store-nokey"));
    let home = hlid_home(
        &tmp.path().join("home-nokey"),
        &id,
        &dev,
        cert_for(&id, &dev, |_| {}),
        card,
    );
    std::fs::remove_file(home.join("device.key")).unwrap();
    assert!(matches!(
        store.import_hlid(&home, NOW),
        Err(Error::Unreadable {
            file: "device.key",
            ..
        })
    ));
}

#[test]
fn the_same_device_renews_and_another_of_its_identity_is_refused() {
    let tmp = tempfile::tempdir().unwrap();
    let store = Store::new(tmp.path().join("device-keys"));
    let (id, dev) = person(1);
    let home = hlid_home(
        tmp.path(),
        &id,
        &dev,
        cert_for(&id, &dev, |_| {}),
        card_for(&id, "alice"),
    );
    let first = store.import_hlid(&home, NOW).unwrap();

    let later = NOW + 60 * 86_400;
    let renewed = cert_for(&id, &dev, |c| {
        c.issued = later;
        c.expires = later + cert::RECOMMENDED_LIFETIME
    });
    let home = hlid_home(
        &tmp.path().join("again"),
        &id,
        &dev,
        renewed.clone(),
        card_for(&id, "alice b"),
    );
    let d = store.import_hlid(&home, later).unwrap();
    assert_eq!(d.cert_bytes(), renewed);
    assert_eq!(store.get(&id.fingerprint()).unwrap().card.name, "alice b");
    assert!(
        first.usable(later + cert::RECOMMENDED_LIFETIME).is_err()
            && d.usable(later + 86_400).is_ok()
    );

    let other = DeviceKey::from_seed(&[42; 32]);
    let home = hlid_home(
        &tmp.path().join("other"),
        &id,
        &other,
        cert_for(&id, &other, |_| {}),
        card_for(&id, "alice"),
    );
    match store.import_hlid(&home, NOW) {
        Err(Error::OtherDevice(fp)) => assert_eq!(fp, dev.fingerprint()),
        r => panic!("{r:?}"),
    }
    assert_eq!(
        store.get(&id.fingerprint()).unwrap().device(),
        dev.fingerprint()
    );
}

#[test]
fn a_device_made_here_is_kept_once_a_holder_certifies_it() {
    let tmp = tempfile::tempdir().unwrap();
    let store = Store::new(tmp.path().join("device-keys"));
    let id = IdentityKey::from_seed(&[1; 32]);
    let pending = store.begin_device().unwrap();
    assert_eq!(store.pending().unwrap(), std::slice::from_ref(&pending));
    let cmd = pending.hlid_command("gtkhx.bundle");
    assert!(
        cmd.contains(&hex(&pending.public)) && cmd.contains(&hex(&pending.public_enc)),
        "{cmd}"
    );

    // A bundle for some other device does not finish it.
    let stranger = DeviceKey::from_seed(&[9; 32]);
    let wrong = Bundle {
        cert: cert_for(&id, &stranger, |_| {}),
        card: card_for(&id, "alice"),
    }
    .encode();
    assert!(matches!(
        store.finish_device(&pending.device, &wrong, NOW),
        Err(Error::WrongDevice)
    ));
    assert!(matches!(
        store.finish_device(&pending.device, b"junk", NOW),
        Err(Error::Unreadable { file: "bundle", .. })
    ));
    assert_eq!(store.pending().unwrap().len(), 1, "still waiting");

    let issued = DeviceCert::for_keys(
        &id,
        pending.public,
        pending.public_enc,
        NOW,
        cert::RECOMMENDED_LIFETIME,
    )
    .unwrap();
    let bundle = Bundle {
        cert: issued.sign(&id),
        card: card_for(&id, "alice"),
    }
    .encode();
    let d = store.finish_device(&pending.device, &bundle, NOW).unwrap();
    assert_eq!(d.device(), pending.device);
    assert_eq!(d.key().unwrap().public(), pending.public);
    assert!(store.pending().unwrap().is_empty());
    // Finished again with its bundle, it is the device kept; with
    // another, there is nothing to finish.
    let again = store.finish_device(&pending.device, &bundle, NOW).unwrap();
    assert_eq!(again.cert_bytes(), d.cert_bytes());
    assert!(matches!(
        store.finish_device(&pending.device, &wrong, NOW),
        Err(Error::NotFound)
    ));

    let given_up = store.begin_device().unwrap();
    store.abandon_device(&given_up.device).unwrap();
    assert!(store.pending().unwrap().is_empty());
    assert_eq!(store.list().unwrap().len(), 1);
}

#[test]
fn a_reimport_never_takes_an_older_certificate() {
    let tmp = tempfile::tempdir().unwrap();
    let store = Store::new(tmp.path().join("device-keys"));
    let (id, dev) = person(1);
    let newer = cert_for(&id, &dev, |_| {});
    let home = hlid_home(tmp.path(), &id, &dev, newer.clone(), card_for(&id, "alice"));
    store.import_hlid(&home, NOW).unwrap();
    let older = cert_for(&id, &dev, |c| c.issued = NOW - 86_400);
    let home = hlid_home(
        &tmp.path().join("old"),
        &id,
        &dev,
        older,
        card_for(&id, "alice"),
    );
    match store.import_hlid(&home, NOW) {
        Err(Error::Older { kept_issued }) => assert_eq!(kept_issued, NOW - 3600),
        r => panic!("{r:?}"),
    }
    assert_eq!(store.get(&id.fingerprint()).unwrap().cert_bytes(), newer);
}

#[test]
fn what_a_crash_leaves_is_finished_by_the_next_change() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path().join("device-keys");
    let store = Store::new(&root);
    let (id, dev) = person(1);
    let home = hlid_home(
        tmp.path(),
        &id,
        &dev,
        cert_for(&id, &dev, |_| {}),
        card_for(&id, "alice"),
    );
    store.import_hlid(&home, NOW).unwrap();
    let fp = id.fingerprint().to_string();

    // A device set aside for its replacement, with the replacement never
    // renamed in, a directory being written and a file half written.
    std::fs::rename(root.join(&fp), root.join(format!(".old-{fp}"))).unwrap();
    std::fs::create_dir(root.join(".tmp-half-1-0")).unwrap();
    // A pending seed long abandoned, one that does not read, and a seed
    // half written.
    let stale = store.begin_device().unwrap();
    std::fs::write(root.join("pending").join(".tmp-device.key-1-0"), b"x").unwrap();
    let stale_path = root.join("pending").join(format!("{}.key", stale.device));
    let month_ago = std::time::SystemTime::now() - std::time::Duration::from_secs(31 * 86_400);
    std::fs::File::options()
        .write(true)
        .open(&stale_path)
        .unwrap()
        .set_modified(month_ago)
        .unwrap();
    std::fs::write(root.join("pending").join("junk.key"), b"not a seed").unwrap();
    assert_eq!(
        store.pending().unwrap(),
        std::slice::from_ref(&stale),
        "the junk passed over"
    );

    let fresh = store.begin_device().unwrap();
    assert_eq!(
        store.get(&id.fingerprint()).unwrap().device(),
        dev.fingerprint(),
        "set back"
    );
    let names: Vec<String> = std::fs::read_dir(&root)
        .unwrap()
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    assert!(
        names
            .iter()
            .all(|n| !n.starts_with(".tmp-") && !n.starts_with(".old-")),
        "{names:?}"
    );
    assert!(!root.join("pending").join(".tmp-device.key-1-0").exists());
    assert_eq!(store.pending().unwrap(), [fresh], "the stale seed gone");
}

#[test]
fn a_partial_identity_does_not_stand_in_the_way() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path().join("device-keys");
    let store = Store::new(&root);
    let (id, dev) = person(1);
    // Another device's key, alone: what an older, unlocked write may leave.
    let partial = root.join(id.fingerprint().to_string());
    std::fs::create_dir_all(&partial).unwrap();
    std::fs::write(partial.join("device.key"), format!("{}\n", hex(&[77; 32]))).unwrap();
    assert!(store.get(&id.fingerprint()).is_err());
    let home = hlid_home(
        tmp.path(),
        &id,
        &dev,
        cert_for(&id, &dev, |_| {}),
        card_for(&id, "alice"),
    );
    let d = store.import_hlid(&home, NOW).unwrap();
    assert_eq!(store.get(&id.fingerprint()).unwrap().device(), d.device());
}

#[test]
fn imports_from_two_threads_keep_one_whole_device() {
    let tmp = tempfile::tempdir().unwrap();
    let root = tmp.path().join("device-keys");
    let (id, _) = person(1);
    let homes: Vec<PathBuf> = (0..8u8)
        .map(|i| {
            let dev = DeviceKey::from_seed(&[200 + i; 32]);
            hlid_home(
                &tmp.path().join(format!("h{i}")),
                &id,
                &dev,
                cert_for(&id, &dev, |_| {}),
                card_for(&id, "alice"),
            )
        })
        .collect();
    let kept: Vec<bool> = std::thread::scope(|s| {
        let joins: Vec<_> = homes
            .iter()
            .map(|h| {
                let root = root.clone();
                s.spawn(move || Store::new(root).import_hlid(h, NOW).is_ok())
            })
            .collect();
        joins.into_iter().map(|j| j.join().unwrap()).collect()
    });
    assert_eq!(
        kept.iter().filter(|k| **k).count(),
        1,
        "one device, the rest refused"
    );
    let store = Store::new(&root);
    assert!(store.get(&id.fingerprint()).unwrap().key().is_ok());
}
