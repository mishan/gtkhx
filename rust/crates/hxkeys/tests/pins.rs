//! The pin store driven through hl-identity's handover vectors: a first
//! pin, a later contact and a pin again after a warning, each read from
//! and written to the file as GtkHx keeps it.

mod common;

use std::path::Path;

use common::{hex, str_of, unhex, unhex32, vectors};
use hl_identity::handover::{Followed, Warning};
use hl_identity::ServerChallenge;
use hxkeys::{Outcome, Pins};
use serde_json::{json, Value as Json};
use sha2::{Digest, Sha256};

fn signed(v: &Json, kind: &str, name: &str) -> Vec<u8> {
    unhex(str_of(&v[kind][name], "signed_hex"))
}

fn names(v: &Json) -> Vec<&str> {
    v.as_array()
        .map(|a| a.iter().map(|n| n.as_str().unwrap()).collect())
        .unwrap_or_default()
}

fn hash_of(v: &Json, name: &str) -> [u8; 32] {
    Sha256::digest(signed(v, "handovers", name)).into()
}

/// The fork record of the handover `name`.
fn record(v: &Json, name: &str) -> Followed {
    let f = &v["handovers"][name]["fields"];
    Followed {
        old: unhex32(str_of(f, "old")),
        sha256: hash_of(v, name),
        time: f["time"].as_u64().unwrap(),
        next: f["next"].as_str().map(unhex32),
    }
}

fn handover_name(v: &Json, sha256: &[u8; 32]) -> String {
    v["handovers"]
        .as_object()
        .unwrap()
        .keys()
        .find(|n| hash_of(v, n) == *sha256)
        .expect("a handover in the file")
        .clone()
}

fn key_name(v: &Json, key: &[u8; 32]) -> String {
    v["keys"]
        .as_object()
        .unwrap()
        .iter()
        .find(|(_, k)| unhex32(str_of(k, "public_hex")) == *key)
        .unwrap()
        .0
        .clone()
}

/// A pins file holding `host` pinned at `key`, as a user's would.
fn write_pinned(
    path: &Path,
    host: &str,
    key: &[u8; 32],
    commitment: Option<[u8; 32]>,
    followed: &[Followed],
    dismissed: &[[u8; 32]],
) {
    let mut text = format!(
        "version = 1\n\n[[host]]\nhost = \"{host}\"\nkey = \"{}\"\n",
        hex(key)
    );
    if let Some(c) = commitment {
        text += &format!("commitment = \"{}\"\n", hex(&c));
    }
    let d: Vec<String> = dismissed
        .iter()
        .map(|d| format!("\"{}\"", hex(d)))
        .collect();
    text += &format!("dismissed = [{}]\n", d.join(", "));
    for f in followed {
        text += &format!(
            "\n[[host.followed]]\nold = \"{}\"\nsha256 = \"{}\"\ntime = {}\n",
            hex(&f.old),
            hex(&f.sha256),
            f.time
        );
        if let Some(n) = f.next {
            text += &format!("next = \"{}\"\n", hex(&n));
        }
    }
    std::fs::write(path, text).unwrap();
}

/// What the store made of a case, in the vectors' `expect` form but for
/// a warning's reason, which names the check of one implementation.
fn decide(v: &Json, case: &Json, path: &Path) -> Json {
    let now = v["now"].as_u64().unwrap();
    let host = str_of(case, "host");
    let chain: Vec<Vec<u8>> = names(&case["chain"])
        .into_iter()
        .map(|n| signed(v, "handovers", n))
        .collect();
    let challenge =
        ServerChallenge::parse(&signed(v, "challenges", str_of(case, "challenge"))).unwrap();
    if challenge
        .check(
            &challenge.challenge,
            &challenge.server_key,
            host,
            now,
            v["skew"].as_u64().unwrap(),
        )
        .is_err()
    {
        return json!({ "outcome": "warn" });
    }
    let followed: Vec<Followed> = names(&case["followed"])
        .into_iter()
        .map(|n| record(v, n))
        .collect();
    let dismissed: Vec<[u8; 32]> = names(&case["dismissed"])
        .into_iter()
        .map(|n| hash_of(v, n))
        .collect();
    let commitment = case["commitment"].as_str().map(unhex32);
    let fork =
        |sha256: &[u8; 32]| json!({ "outcome": "warn", "handover": handover_name(v, sha256) });
    let named = |fs: &[Followed]| -> Vec<String> {
        fs.iter().map(|f| handover_name(v, &f.sha256)).collect()
    };

    match str_of(case, "kind") {
        "pin" => {
            let mut pins = Pins::load(path).unwrap();
            let Outcome::Pinned(t) = pins.check(host, &challenge, &chain, now).unwrap() else {
                panic!("a first contact pins");
            };
            assert_eq!(t.challenge(), &challenge);
            let pin = Pins::load(path).unwrap().pinned(host).unwrap().clone();
            assert_eq!(pin.key, challenge.server_key);
            json!({
                "outcome": "pin",
                "commitment": pin.commitment.map(|c| hex(&c)),
                "aliases": named(&pin.followed),
            })
        }
        "repin" => {
            if !followed.is_empty() || !dismissed.is_empty() {
                write_pinned(
                    path,
                    host,
                    &followed[0].old,
                    commitment,
                    &followed,
                    &dismissed,
                );
            }
            let before = std::fs::read(path).ok();
            let mut pins = Pins::load(path).unwrap();
            match pins
                .accept(host, &challenge, &chain, &Warning::Recommitted)
                .unwrap()
            {
                Ok(t) => {
                    assert_eq!(t.challenge(), &challenge);
                    let pin = Pins::load(path).unwrap().pinned(host).unwrap().clone();
                    assert_eq!(pin.key, challenge.server_key);
                    assert_eq!(pin.followed, followed, "the records kept, no aliases taken");
                    json!({
                        "outcome": "pin",
                        "commitment": pin.commitment.map(|c| hex(&c)),
                        "aliases": [],
                    })
                }
                Err(Warning::Fork { sha256 }) => {
                    assert_eq!(std::fs::read(path).ok(), before, "nothing changed");
                    fork(&sha256)
                }
                Err(w) => panic!("a re-pin warns only of a fork: {w:?}"),
            }
        }
        _ => {
            let pinned = unhex32(str_of(&v["keys"][str_of(case, "pinned")], "public_hex"));
            write_pinned(path, host, &pinned, commitment, &followed, &dismissed);
            let before = std::fs::read(path).unwrap();
            let mut pins = Pins::load(path).unwrap();
            let outcome = pins.check(host, &challenge, &chain, now).unwrap();
            let pin = Pins::load(path).unwrap().pinned(host).unwrap().clone();
            match outcome {
                Outcome::Kept(t) => {
                    assert_eq!(t.challenge(), &challenge);
                    assert_eq!(std::fs::read(path).unwrap(), before, "nothing written");
                    json!({ "outcome": "keep" })
                }
                Outcome::Followed(t) => {
                    assert_eq!(t.challenge(), &challenge);
                    assert_eq!(pin.followed[..followed.len()], followed[..], "records kept");
                    json!({
                        "outcome": "follow",
                        "key": key_name(v, &pin.key),
                        "commitment": pin.commitment.map(|c| hex(&c)),
                        "followed": named(&pin.followed[followed.len()..]),
                    })
                }
                Outcome::Warn(w) => {
                    assert_eq!(std::fs::read(path).unwrap(), before, "nothing written");
                    match w {
                        Warning::Fork { sha256 } => fork(&sha256),
                        _ => json!({ "outcome": "warn" }),
                    }
                }
                Outcome::Pinned(_) => panic!("a host already pinned"),
            }
        }
    }
}

#[test]
fn the_handover_vectors_through_the_pin_file() {
    let v = vectors("server-handover-vectors.json");
    let dir = tempfile::tempdir().unwrap();
    for (i, case) in v["client"].as_array().unwrap().iter().enumerate() {
        let path = dir.path().join(format!("server-keys-{i}.toml"));
        let mut expect = case["expect"].clone();
        if let Some(e) = expect.as_object_mut() {
            e.remove("reason");
        }
        assert_eq!(decide(&v, case, &path), expect, "{}", str_of(case, "name"));
    }
}

#[test]
fn a_fork_the_user_accepts_is_not_warned_of_again() {
    let v = vectors("server-handover-vectors.json");
    let now = v["now"].as_u64().unwrap();
    let host = str_of(&v, "host");
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("server-keys.toml");
    let c1 = ServerChallenge::parse(&signed(&v, "challenges", "c1")).unwrap();
    let k1 = unhex32(str_of(&v["keys"]["k1"], "public_hex"));
    write_pinned(&path, host, &k1, c1.next, &[record(&v, "h0_1")], &[]);
    let stray = [signed(&v, "handovers", "h0_stray")];

    let mut pins = Pins::load(&path).unwrap();
    let Outcome::Warn(w @ Warning::Fork { .. }) = pins.check(host, &c1, &stray, now).unwrap()
    else {
        panic!("a fork warns");
    };
    assert!(pins.accept(host, &c1, &stray, &w).unwrap().is_ok());
    let mut pins = Pins::load(&path).unwrap();
    assert!(matches!(
        pins.check(host, &c1, &stray, now).unwrap(),
        Outcome::Kept(_)
    ));
    pins.forget(host).unwrap();
    assert!(Pins::load(&path).unwrap().pinned(host).is_none());
}

#[test]
fn a_file_that_does_not_read_is_refused_not_overwritten() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("server-keys.toml");
    std::fs::write(
        &path,
        "version = 1\n[[host]]\nhost = \"a:1\"\nkey = \"nothex\"\n",
    )
    .unwrap();
    assert!(matches!(
        Pins::load(&path),
        Err(hxkeys::Error::Unreadable { .. })
    ));
    assert!(Pins::load(dir.path().join("absent.toml"))
        .unwrap()
        .pinned("a:1")
        .is_none());
}

#[test]
fn a_challenge_is_checked_only_for_the_host_it_was_signed_for() {
    let v = vectors("server-handover-vectors.json");
    let now = v["now"].as_u64().unwrap();
    let c1 = ServerChallenge::parse(&signed(&v, "challenges", "c1")).unwrap();
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("server-keys.toml");
    let mut pins = Pins::load(&path).unwrap();
    let none: &[Vec<u8>] = &[];
    for host in ["other.example:5700", "hl.example:5701"] {
        assert!(
            matches!(
                pins.check(host, &c1, none, now),
                Err(hxkeys::Error::WrongHost)
            ),
            "{host}"
        );
        assert!(matches!(
            pins.accept(host, &c1, none, &Warning::Recommitted),
            Err(hxkeys::Error::WrongHost)
        ));
    }
    assert!(!path.exists(), "nothing pinned");
}

#[test]
fn two_processes_sharing_the_file_keep_each_others_pins() {
    let v = vectors("server-handover-vectors.json");
    let now = v["now"].as_u64().unwrap();
    let c1 = ServerChallenge::parse(&signed(&v, "challenges", "c1")).unwrap();
    let c2 = ServerChallenge::parse(&signed(&v, "challenges", "c2")).unwrap();
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("server-keys.toml");
    let none: &[Vec<u8>] = &[];
    let host = str_of(&v, "host");
    // Both opened before either wrote.
    let mut a = Pins::load(&path).unwrap();
    let mut b = Pins::load(&path).unwrap();
    assert!(matches!(
        a.check(host, &c1, none, now).unwrap(),
        Outcome::Pinned(_)
    ));
    // b's first sight of the host finds a's pin, and keeps to it.
    assert!(matches!(
        b.check(host, &c2, none, now).unwrap(),
        Outcome::Warn(_)
    ));
    assert_eq!(b.pinned(host).unwrap().key, c1.server_key);
    // A pin b makes of another host leaves a's, and a's forgetting leaves b's.
    let other = "hl.example:5701";
    let mut c = c2.clone();
    c.host = other.to_owned();
    assert!(matches!(
        b.check(other, &c, none, now).unwrap(),
        Outcome::Pinned(_)
    ));
    let both = Pins::load(&path).unwrap();
    assert_eq!(both.pinned(host).unwrap().key, c1.server_key);
    assert_eq!(both.pinned(other).unwrap().key, c2.server_key);
    a.forget(host).unwrap();
    let left = Pins::load(&path).unwrap();
    assert!(left.pinned(host).is_none() && left.pinned(other).is_some());
}

#[test]
fn a_host_is_one_however_it_is_written_and_each_port_its_own() {
    let v = vectors("server-handover-vectors.json");
    let now = v["now"].as_u64().unwrap();
    let c1 = ServerChallenge::parse(&signed(&v, "challenges", "c1")).unwrap();
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("server-keys.toml");
    let none: &[Vec<u8>] = &[];
    let mut pins = Pins::load(&path).unwrap();
    assert!(matches!(
        pins.check("HL.Example:5700", &c1, none, now).unwrap(),
        Outcome::Pinned(_)
    ));
    assert!(pins.pinned("hl.example:5700").is_some());
    assert!(matches!(
        pins.check("hl.example:5700", &c1, none, now).unwrap(),
        Outcome::Kept(_)
    ));
    // A file written by hand in another case reads as the same host.
    let text = std::fs::read_to_string(&path).unwrap();
    std::fs::write(&path, text.replace("hl.example:5700", "HL.EXAMPLE:5700")).unwrap();
    let mut pins = Pins::load(&path).unwrap();
    assert!(pins.pinned("Hl.Example:5700").is_some());
    pins.forget("HL.example:5700").unwrap();
    assert!(Pins::load(&path)
        .unwrap()
        .pinned("hl.example:5700")
        .is_none());

    // 80 and 443 are named, and are two hosts; a host without a port, or
    // that is not one, is none.
    // Each challenge names the host as signed, its port left off.
    let c80 = ServerChallenge {
        host: "hl.example".into(),
        ..c1.clone()
    };
    let c443 = ServerChallenge {
        server_key: unhex32(str_of(&v["keys"]["k2"], "public_hex")),
        ..c80.clone()
    };
    assert!(matches!(
        pins.check("hl.example:80", &c80, none, now).unwrap(),
        Outcome::Pinned(_)
    ));
    assert!(matches!(
        pins.check("hl.example:443", &c443, none, now).unwrap(),
        Outcome::Pinned(_)
    ));
    let pins = Pins::load(&path).unwrap();
    assert_eq!(pins.pinned("hl.example:80").unwrap().key, c1.server_key);
    assert_eq!(pins.pinned("hl.example:443").unwrap().key, c443.server_key);
    let mut pins = pins;
    for bad in ["hl.example", "hl.example:x", "hl/example:5700"] {
        assert!(
            matches!(
                pins.check(bad, &c1, none, now),
                Err(hxkeys::Error::WrongHost)
            ),
            "{bad}"
        );
        assert!(pins.pinned(bad).is_none(), "{bad}");
    }
}
