//! The key-changed dialog against pins of its own.

use super::*;
use hl_identity::ServerKey;
use hxkeys::Outcome;
use std::cell::Cell;

thread_local! {
    /// How many times a dialog set about pinning a key.
    pub(super) static ACCEPTS: Cell<u32> = const { Cell::new(0) };
    /// How many of those have finished.
    pub(super) static DONE: Cell<u32> = const { Cell::new(0) };
}

const HOST: &str = "hl.example:5500";

fn now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_secs()
}

/// A challenge for `HOST` signed with the key of `seed`, committing to
/// `next`.
fn challenge(seed: u8, next: Option<&[u8; 32]>) -> ServerChallenge {
    let key = ServerKey::from_seed(&[seed; 32]);
    let signed = ServerChallenge::sign_trtp(&key, &[1; 32], HOST, now() + 60, next).unwrap();
    ServerChallenge::parse_trtp(&signed).unwrap()
}

fn pinned(pins: &std::path::Path) -> (hl_identity::keys::PublicKey, Option<[u8; 32]>) {
    let p = Pins::load(pins).unwrap();
    let pin = p.pinned(HOST).expect("pinned");
    (pin.key, pin.commitment)
}

fn key(seed: u8) -> String {
    Fingerprint::of(&ServerKey::from_seed(&[seed; 32]).public()).to_string()
}

/// Run the main loop until the trust asked for last has finished.
fn settle(done: u32) {
    let until = std::time::Instant::now() + std::time::Duration::from_secs(10);
    while DONE.get() == done {
        assert!(std::time::Instant::now() < until, "never finished");
        glib::MainContext::default().iteration(false);
    }
}

/// `then` warned of against `first` pinned for `HOST` in `pins`, and the
/// pin it was checked against.
fn warned(
    pins: &std::path::Path,
    first: &ServerChallenge,
    then: &ServerChallenge,
) -> (Warning, Option<Pin>) {
    let none: &[Vec<u8>] = &[];
    Pins::load(pins)
        .unwrap()
        .check(HOST, first, none, now())
        .unwrap();
    let mut checking = Pins::load(pins).unwrap();
    match checking.check(HOST, then, none, now()) {
        Ok(Outcome::Warn(w)) => (w, checking.pinned(HOST).cloned()),
        other => panic!("{other:?}"),
    }
}

/// A changed key and a changed or dropped commitment are each said as
/// what they are, with the keys concerned; canceling pins nothing, and
/// trusting pins the change, which is then kept.
pub(crate) fn check_key_changed() {
    let cases = [
        (
            "Server Key Changed",
            challenge(8, None),
            challenge(9, None),
            vec![
                format!("Pinned key: {}", key(8)),
                format!("New key: {}", key(9)),
            ],
            "_Trust New Key",
        ),
        (
            "Server Key Commitment Changed",
            challenge(8, Some(&[3; 32])),
            challenge(8, Some(&[4; 32])),
            vec![
                format!("Key, unchanged: {}", key(8)),
                format!("Next key: {}", Fingerprint([4; 32])),
            ],
            "_Trust Next Key",
        ),
        (
            "Server Key Commitment Changed",
            challenge(8, Some(&[3; 32])),
            challenge(8, None),
            vec![
                format!("Key, unchanged: {}", key(8)),
                "Next key: none".into(),
            ],
            "_Trust",
        ),
    ];
    for (heading, first, then, lines, trust) in cases {
        let dir = tempfile::tempdir().unwrap();
        let pins = dir.path().join("server-keys.toml");
        let (warning, shown) = warned(&pins, &first, &then);
        let was = pinned(&pins);

        let d = ask(
            None,
            pins.clone(),
            HOST.into(),
            then.clone(),
            warning.clone(),
            shown.clone(),
        );
        assert_eq!(d.heading().as_deref(), Some(heading));
        assert_eq!(d.response_label("trust"), trust, "{heading}");
        let body = d.body();
        assert!(body.contains(HOST), "{body}");
        assert!(body.contains("nothing was signed"), "{body}");
        for line in &lines {
            assert!(body.contains(line.as_str()), "{heading}: {line} in {body}");
        }
        assert_eq!(
            body.contains("New key"),
            heading == "Server Key Changed",
            "{heading}: {body}"
        );
        let accepts = ACCEPTS.get();
        d.emit_by_name::<()>("response", &[&"cancel"]);
        assert_eq!(
            ACCEPTS.get(),
            accepts,
            "{heading}: nothing pinned when canceled"
        );
        assert_eq!(pinned(&pins), was);

        let d = ask(
            None,
            pins.clone(),
            HOST.into(),
            then.clone(),
            warning,
            shown,
        );
        let done = DONE.get();
        d.emit_by_name::<()>("response", &[&"trust"]);
        settle(done);
        assert_eq!(pinned(&pins), (then.server_key, then.next), "{heading}");
        let none: &[Vec<u8>] = &[];
        let again = Pins::load(&pins).unwrap().check(HOST, &then, none, now());
        assert!(
            matches!(again, Ok(Outcome::Kept(_))),
            "{heading}: {again:?}"
        );
    }
}

/// A later change to a host asked about is asked about in the same dialog,
/// and trusting pins that one; a pin changed after the change was checked
/// against it, before the dialog or during it, is not written over.
pub(crate) fn check_one_host_one_dialog() {
    let dir = tempfile::tempdir().unwrap();
    let pins = dir.path().join("server-keys.toml");
    let (c8, c9, c10) = (challenge(8, None), challenge(9, None), challenge(10, None));
    let (w9, was) = warned(&pins, &c8, &c9);
    let first = ask(None, pins.clone(), HOST.into(), c9, w9, was);
    let (w10, was) = warned(&pins, &c8, &c10);
    let later = ask(None, pins.clone(), HOST.into(), c10.clone(), w10, was);
    assert_eq!(first, later, "one dialog");
    assert!(later.body().contains(&key(10)), "{}", later.body());
    let done = DONE.get();
    later.emit_by_name::<()>("response", &[&"trust"]);
    settle(done);
    assert_eq!(pinned(&pins).0, c10.server_key, "the later change pinned");

    let none: &[Vec<u8>] = &[];
    for dialog_first in [false, true] {
        let dir = tempfile::tempdir().unwrap();
        let pins = dir.path().join("server-keys.toml");
        let c9 = challenge(9, None);
        let (w9, was) = warned(&pins, &c8, &c9);
        let raise = || {
            ask(
                None,
                pins.clone(),
                HOST.into(),
                c9.clone(),
                w9.clone(),
                was.clone(),
            )
        };
        let d = dialog_first.then(raise);
        Pins::load(&pins)
            .unwrap()
            .accept(HOST, was.as_ref(), &c10, none, &w9)
            .unwrap()
            .unwrap();
        let d = d.unwrap_or_else(raise);
        let done = DONE.get();
        d.emit_by_name::<()>("response", &[&"trust"]);
        settle(done);
        assert_eq!(
            pinned(&pins).0,
            c10.server_key,
            "the pin made meanwhile kept, raised first: {dialog_first}"
        );
    }
}
