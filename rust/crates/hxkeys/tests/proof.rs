//! The host a challenge is asked for, the challenge read, and the proof
//! signed, against hl-identity's vectors.

mod common;

use common::{str_of, unhex, unhex32, vectors};
use hl_identity::{DeviceKey, DeviceProof, ServerChallenge};
use hxkeys::proof::{answer, proof_host, read_challenge, SKEW};
use hxkeys::{Outcome, Pins};

#[test]
fn the_host_asked_for_always_names_its_port() {
    for (host, port, want) in [
        ("hl.example", 5500, Some("hl.example:5500")),
        ("HL.Example", 5500, Some("hl.example:5500")),
        ("hl.example", 443, Some("hl.example:443")),
        ("hl.example", 80, Some("hl.example:80")),
        ("hl.example.", 5500, Some("hl.example.:5500")),
        ("192.0.2.7", 5500, Some("192.0.2.7:5500")),
        ("::1", 5500, Some("[::1]:5500")),
        ("[2001:DB8:0:0:0:0:0:1]", 5600, Some("[2001:db8::1]:5600")),
        ("", 5500, None),
        ("hl.example/x", 5500, None),
        ("user@hl.example", 5500, None),
    ] {
        assert_eq!(proof_host(host, port).as_deref(), want, "{host} {port}");
    }
}

#[test]
fn a_challenge_is_read_for_the_host_it_was_asked_for_and_in_its_time() {
    let v = vectors("identity-test-vectors.json");
    let o = &v["server_challenge_trtp"]["object"];
    let bytes = unhex(str_of(o, "signed_hex"));
    let host = str_of(&o["fields"], "host");
    let expires = o["fields"]["expires"].as_u64().unwrap();

    let c = read_challenge(&bytes, host, expires - 30).unwrap();
    assert_eq!(c.host, host);
    assert_eq!(c.server_key, unhex32(str_of(&o["fields"], "server_key")));
    assert!(read_challenge(&bytes, host, expires + SKEW).is_ok());
    for (sent, now, want) in [
        (
            "other.example:5500",
            expires,
            hl_identity::Error::HostMismatch,
        ),
        ("hl.example:5501", expires, hl_identity::Error::HostMismatch),
        (host, expires + SKEW + 1, hl_identity::Error::Expired),
    ] {
        assert_eq!(
            read_challenge(&bytes, sent, now),
            Err(want),
            "{sent} at {now}"
        );
    }
    // The ng port's challenge is not one the classic wire may be answered
    // with, nor the other way about.
    let ng = unhex(str_of(&v["server_challenge"]["object"], "signed_hex"));
    assert_eq!(
        read_challenge(&ng, host, expires),
        Err(hl_identity::Error::BadSignature)
    );
}

#[test]
fn the_proof_is_the_vectors_with_the_exporter_and_unbound_without() {
    let v = vectors("identity-test-vectors.json");
    let p = &v["device_proof"];
    let f = &p["object"]["fields"];
    let key = DeviceKey::from_seed(&unhex32(str_of(&v["keys"]["device"], "seed_hex")));
    let challenge = ServerChallenge {
        challenge: unhex32(str_of(p, "challenge_hex")),
        server_key: unhex32(str_of(p, "server_key_hex")),
        host: str_of(p, "host").to_owned(),
        expires: 0,
        next: None,
    };
    let exporter = unhex32(str_of(p, "exporter_hex"));
    let time = f["time"].as_u64().unwrap();
    // Signed only for a key the pins trust: here, at first contact.
    let dir = tempfile::tempdir().unwrap();
    let mut pins = Pins::load(dir.path().join("server-keys.toml")).unwrap();
    let Outcome::Pinned(challenge) = pins
        .check(str_of(p, "host"), &challenge, &[] as &[Vec<u8>], time)
        .unwrap()
    else {
        panic!("a first contact pins");
    };

    let bound = answer(&key, &challenge, Some(&exporter), time).unwrap();
    assert_eq!(bound, unhex(str_of(&p["object"], "signed_hex")));

    let unbound = DeviceProof::parse(&answer(&key, &challenge, None, time).unwrap()).unwrap();
    assert_eq!(unbound.exporter, None);
    assert_eq!(unbound.device, key.public());
    assert_eq!(unbound.host, challenge.challenge().host);
}
