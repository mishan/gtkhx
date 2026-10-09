//! The login's reply, as production's session reads it, on every rig server
//! and over plaintext, TLS and HOPE: what the server says of itself, and a
//! refusal's reason. `cargo test -p hx-e2e --features rig`.

#![cfg(feature = "rig")]

use std::time::Duration;

use hxnet::lifecycle::{
    run_hope_lifecycle, run_plaintext_lifecycle, run_plaintext_tls_lifecycle, HopeOpenRequest,
    PlaintextOpenRequest,
};
use hxnet::{Connection, Event};
use hxsession::{cap, Closed, Handled, ServerInfo};

/// What GtkHx offers at login with voice and video built in.
const CAPS: u16 = cap::LARGE_FILES
    | cap::TEXT_ENCODING
    | cap::VOICE
    | cap::INLINE_MEDIA
    | cap::CHAT_HISTORY
    | 0x0400;

#[derive(Debug)]
enum Outcome {
    LoggedIn(ServerInfo),
    Refused(Option<String>),
    /// Hung up without a word.
    Ended,
}

#[derive(Clone, Copy)]
enum Over {
    Plain,
    Tls,
    Hope(Option<hxhope::Cipher>),
}

/// Log in to `port` as `login`, as production does, and say how it went.
fn log_in(port: u16, login: &str, password: &str, over: Over) -> Outcome {
    let rt = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(1)
        .enable_all()
        .build()
        .unwrap();
    let (_handle, mut events, cmd_rx, evt_tx) = Connection::make_channels();
    let plain = PlaintextOpenRequest {
        host: "127.0.0.1".into(),
        port,
        login: login.as_bytes().to_vec(),
        password: password.as_bytes().to_vec(),
        name: b"hx-e2e login".to_vec(),
        icon: 414,
        version: hxnet::login::CLIENT_VERSION,
        caps: CAPS,
        trans: 1,
        proxy: None,
    };
    match over {
        Over::Plain => {
            let session = plain.session(Handled::LOGIN);
            rt.spawn(run_plaintext_lifecycle(plain, session, cmd_rx, evt_tx));
        }
        Over::Tls => {
            let session = plain.session(Handled::LOGIN);
            let trust = Some(Box::new(|_: &str| true) as Box<dyn Fn(&str) -> bool + Send>);
            rt.spawn(run_plaintext_tls_lifecycle(
                plain,
                session,
                trust,
                Default::default(),
                cmd_rx,
                evt_tx,
            ));
        }
        Over::Hope(cipher) => {
            let req = HopeOpenRequest {
                host: plain.host,
                port,
                login: plain.login,
                password: plain.password,
                name: plain.name,
                icon: plain.icon,
                version: plain.version,
                caps: CAPS,
                cipher,
                compression: None,
                proxy: None,
            };
            let session = req.session(Handled::LOGIN);
            rt.spawn(run_hope_lifecycle(req, session, cmd_rx, evt_tx));
        }
    }
    rt.block_on(async {
        tokio::time::timeout(Duration::from_secs(15), async {
            loop {
                match events.recv().await {
                    Some(Event::Session(hxsession::Event::LoggedIn(info))) => {
                        return Outcome::LoggedIn(info)
                    }
                    Some(Event::Session(hxsession::Event::Closed(Closed::LoginRefused(why)))) => {
                        return Outcome::Refused(why)
                    }
                    // The login's reply is the session's: never whole.
                    Some(Event::Frame(f)) => assert_ne!(f.header.type_, 0x0001_0000, "{f:?}"),
                    Some(Event::Shutdown(_)) | None => return Outcome::Ended,
                    Some(_) => {}
                }
            }
        })
        .await
        .expect("no answer to the login")
    })
}

#[test]
fn every_server_says_what_it_is() {
    let janus_media = cap::INLINE_MEDIA | cap::CHAT_HISTORY | cap::TEXT_ENCODING;
    // (server, port, how, version, caps it must agree to)
    let cases = [
        ("mhxd", 5500, Over::Plain, 185, 0),
        ("mhxd", 5500, Over::Hope(None), 185, 0),
        (
            "mhxd",
            5500,
            Over::Hope(Some(hxhope::Cipher::Blowfish)),
            185,
            0,
        ),
        ("janus", 5510, Over::Plain, 200, janus_media),
        ("janus", 5610, Over::Tls, 200, janus_media),
        (
            "janus",
            5510,
            Over::Hope(Some(hxhope::Cipher::ChaCha20Poly1305)),
            200,
            janus_media,
        ),
        ("hxd-ng", 5520, Over::Plain, 254, cap::TEXT_ENCODING),
        ("hlservd", 5530, Over::Plain, 190, 0),
    ];
    for (name, port, over, version, caps) in cases {
        let Outcome::LoggedIn(info) = log_in(port, "", "", over) else {
            panic!("{name}:{port}: not logged in");
        };
        assert_eq!(info.version, version, "{name}:{port}");
        assert!(
            info.name.as_deref().is_some_and(|n| !n.is_empty()),
            "{name}:{port}"
        );
        assert_eq!(info.caps & caps, caps, "{name}:{port}: {info:?}");
        if name == "janus" {
            assert!(info.media.chunk_size.is_some(), "{info:?}");
        }
    }
}

/// A login no account has: refused with the server's reason, or, by mhxd,
/// with none.
#[test]
fn a_refusal_carries_the_servers_reason() {
    let nobody = hx_e2e::unique_name("nobody");
    for (name, port) in [
        ("mhxd", 5500),
        ("janus", 5510),
        ("hxd-ng", 5520),
        ("hlservd", 5530),
    ] {
        match log_in(port, &nobody, "wrong", Over::Plain) {
            Outcome::Refused(Some(why)) => assert!(!why.is_empty(), "{name}"),
            Outcome::Ended => assert_eq!(name, "mhxd"),
            other => panic!("{name}: {other:?}"),
        }
    }
}
