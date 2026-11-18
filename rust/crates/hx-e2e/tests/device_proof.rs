//! A device proved on the classic wire (capability bit 14) as GtkHx proves
//! one: hxnet's proof between the login reply and the agree, against the
//! rig's hxd-ng, whose `device-a` and `device-b` accounts link this
//! suite's two identities and which revokes `device-b`'s device. What the
//! server made of the proof is read where another user would see it: the
//! session's own entry on the hotline-ng roster, found by a nick no other
//! test goes by, since the tests share the accounts.
#![cfg(feature = "rig")]

use std::path::Path;

use hl_identity::{cert, Card, DeviceCert, DeviceKey, IdentityKey, ServerKey};
use hx_e2e::client::{Transport, CAP_TEXT_ENCODING};
use hx_e2e::ng::Ng;
use hx_e2e::{servers_with, Cap, Client, Server};
use hxkeys::Proof;
use hxnet::device::Armed;
use serde_json::{json, Value};

const NG: &str = "127.0.0.1:5525";
const TLS_PORT: u16 = 5620;
/// What hxd-ng answers every refused proof with but a clock's.
const NOT_PROVED: &str = "The device could not be proved.";

/// The suite's identities, by seed: the rig links `device-a` to A and
/// `device-b` to B, and revokes B's device.
const A: (u8, u8) = (0x41, 0x61);
const B: (u8, u8) = (0x42, 0x62);

fn server() -> Option<&'static Server> {
    servers_with(&[Cap::DeviceProof]).first().copied()
}

fn unix() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_secs()
}

/// `who`'s device, kept in `dir` as GtkHx keeps one, its pins beside it.
fn armed(dir: &Path, who: (u8, u8)) -> Armed {
    let id = IdentityKey::from_seed(&[who.0; 32]);
    let dev = DeviceKey::from_seed(&[who.1; 32]);
    let home = dir.join("hlid");
    std::fs::create_dir_all(&home).unwrap();
    let hex = |b: &[u8]| b.iter().map(|x| format!("{x:02x}")).collect::<String>();
    std::fs::write(home.join("device.key"), hex(&*dev.seed())).unwrap();
    let c = DeviceCert::for_device(&id, &dev, unix() - 60, cert::RECOMMENDED_LIFETIME).unwrap();
    std::fs::write(home.join("cert.bin"), c.sign(&id)).unwrap();
    let card = Card::new(&id, "suite", unix()).sign(&id, vec![]).unwrap();
    std::fs::write(home.join("card.bin"), card).unwrap();
    Armed {
        device: hxkeys::Store::new(dir.join("keys"))
            .import_hlid(&home, unix())
            .unwrap(),
        pins: dir.join("server-keys.toml"),
    }
}

fn fingerprint(who: (u8, u8)) -> String {
    IdentityKey::from_seed(&[who.0; 32])
        .fingerprint()
        .to_string()
}

/// `account` logged in to `s` over `transport` at `host`, going by `nick`,
/// proving `who`'s device with the pins in `dir`.
fn login(
    s: &'static Server,
    dir: &Path,
    host: &str,
    transport: Transport,
    (account, nick): (&str, &str),
    who: (u8, u8),
) -> Client {
    let port = match transport {
        Transport::Tls => TLS_PORT,
        _ => s.port,
    };
    Client::login_proving(
        s,
        host,
        port,
        account,
        account,
        nick,
        CAP_TEXT_ENCODING,
        transport,
        armed(dir, who),
    )
    .unwrap_or_else(|e| panic!("{account} at {host}:{port} over {transport:?}: {e}"))
}

/// The identity the hotline-ng roster shows for the user going by `nick`,
/// who must be on it.
fn shown(nick: &str) -> Option<String> {
    let mut ng = Ng::login(NG, "", "").unwrap_or_else(|e| panic!("{e}"));
    let roster = ng.ok("sync", json!({})).unwrap_or_else(|e| panic!("{e}"));
    let users = roster["users"].as_array().cloned().unwrap_or_default();
    let user = users
        .iter()
        .find(|u: &&Value| u["nick"] == nick)
        .unwrap_or_else(|| panic!("{nick} is not on the roster"));
    user["identity"]["fingerprint"].as_str().map(str::to_owned)
}

fn nick() -> String {
    hx_e2e::unique_name("dp")
}

#[test]
fn a_device_is_proved_on_every_port_and_shown_to_the_others() {
    let Some(s) = server() else { return };
    for transport in [
        Transport::Plain,
        Transport::Tls,
        Transport::Hope(hxhope::Cipher::ChaCha20Poly1305),
    ] {
        let dir = tempfile::tempdir().unwrap();
        let n = nick();
        let c = login(s, dir.path(), s.host, transport, ("device-a", &n), A);
        match c.proofs() {
            [Proof::Proved { identity, .. }] => {
                assert_eq!(identity.to_string(), fingerprint(A), "{transport:?}")
            }
            other => panic!("{transport:?}: {other:?}"),
        }
        assert_eq!(shown(&n), Some(fingerprint(A)), "{transport:?}");
        drop(c);
    }
}

#[test]
fn the_server_key_is_pinned_at_first_contact_and_kept_after() {
    let Some(s) = server() else { return };
    let dir = tempfile::tempdir().unwrap();
    let n = nick();
    let first = login(s, dir.path(), s.host, Transport::Plain, ("device-a", &n), A);
    assert!(
        matches!(first.proofs(), [Proof::Proved { .. }]),
        "{:?}",
        first.proofs()
    );
    drop(first);
    let asked = format!("{}:{}", s.host, s.port);
    let pins = hxkeys::Pins::load(dir.path().join("server-keys.toml")).unwrap();
    let key = pins.pinned(&asked).expect("pinned").key;
    let again = login(s, dir.path(), s.host, Transport::Plain, ("device-a", &n), A);
    assert!(
        matches!(again.proofs(), [Proof::Proved { .. }]),
        "{:?}",
        again.proofs()
    );
    let pins = hxkeys::Pins::load(dir.path().join("server-keys.toml")).unwrap();
    assert_eq!(pins.pinned(&asked).unwrap().key, key, "kept");
}

/// What the server or this side refuses ends the proof with the session
/// still logged in, unproved.
#[test]
fn a_proof_refused_or_not_signed_leaves_the_session_logged_in_unproved() {
    let Some(s) = server() else { return };
    // A revoked device, said alike with every other refusal.
    let dir = tempfile::tempdir().unwrap();
    let n = nick();
    let revoked = login(s, dir.path(), s.host, Transport::Plain, ("device-b", &n), B);
    assert_eq!(
        revoked.proofs(),
        [Proof::Refused {
            reason: Some(NOT_PROVED.into())
        }]
    );
    assert_eq!(shown(&n), None);
    drop(revoked);

    // An address the server does not answer as: no challenge.
    let dir = tempfile::tempdir().unwrap();
    let n = nick();
    let elsewhere = login(
        s,
        dir.path(),
        "localhost",
        Transport::Plain,
        ("device-a", &n),
        A,
    );
    assert!(
        matches!(elsewhere.proofs(), [Proof::Refused { reason: Some(_) }]),
        "{:?}",
        elsewhere.proofs()
    );
    drop(elsewhere);

    // A key pinned for the host that is not the server's: nothing signed.
    let dir = tempfile::tempdir().unwrap();
    let asked = format!("{}:{}", s.host, s.port);
    let other = ServerKey::from_seed(&[0x77; 32]);
    let c = hl_identity::ServerChallenge::sign_trtp(&other, &[1; 32], &asked, unix() + 60, None)
        .unwrap();
    let c = hl_identity::ServerChallenge::parse_trtp(&c).unwrap();
    hxkeys::Pins::load(dir.path().join("server-keys.toml"))
        .unwrap()
        .check(&asked, &c, &[] as &[Vec<u8>], unix())
        .unwrap();
    let n = nick();
    let changed = login(s, dir.path(), s.host, Transport::Plain, ("device-a", &n), A);
    assert!(
        matches!(changed.proofs(), [Proof::KeyChanged { host, .. }] if *host == asked),
        "{:?}",
        changed.proofs()
    );
    assert_eq!(shown(&n), None, "not proved");
}

/// A server that never agrees to bit 14 is logged in to as ever, no proof
/// begun or ended, and the name the login goes by reaches it, under HOPE
/// too.
#[test]
fn a_server_that_proves_no_devices_is_logged_in_to_as_ever() {
    for s in servers_with(&[]) {
        let ciphers = [hxhope::Cipher::ChaCha20Poly1305, hxhope::Cipher::Blowfish];
        let hope = ciphers.iter().find(|c| s.has(Cap::Hope(**c))).copied();
        let transports = [Some(Transport::Plain), hope.map(Transport::Hope)];
        for transport in transports.into_iter().flatten() {
            let what = format!("{} over {transport:?}", s.name);
            let dir = tempfile::tempdir().unwrap();
            let nick = hx_e2e::unique_name("dp");
            let c = Client::login_proving(
                s,
                s.host,
                s.port,
                "",
                "",
                &nick,
                CAP_TEXT_ENCODING,
                transport,
                armed(dir.path(), A),
            )
            .unwrap_or_else(|e| panic!("{what}: {e}"));
            assert!(c.proofs().is_empty(), "{what}: {:?}", c.proofs());
            assert!(!dir.path().join("server-keys.toml").exists(), "{what}");
            let mut other = Client::guest(s, 0);
            let list = other.request(&hxrequest::Request {
                opcode: hxproto::messages::ClientHdr::UserGetList as u32,
                chunks: Vec::new(),
            });
            let named = list.raw.windows(nick.len()).any(|w| w == nick.as_bytes());
            assert!(named, "{what}: {nick} on the user list");
        }
    }
}
