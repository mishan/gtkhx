//! A pins file named with no directory is the working directory's. Its
//! own test binary, since it moves the process's working directory.

use hl_identity::{ServerChallenge, ServerKey};
use hxkeys::{Outcome, Pins};

#[test]
fn a_file_named_alone_is_written_where_the_process_is() {
    let dir = tempfile::tempdir().unwrap();
    std::env::set_current_dir(dir.path()).unwrap();
    let key = ServerKey::from_seed(&[4; 32]);
    let bytes = ServerChallenge::sign_trtp(&key, &[1; 32], "hl.example:5500", 100, None).unwrap();
    let c = ServerChallenge::parse_trtp(&bytes).unwrap();
    let mut pins = Pins::load("server-keys.toml").unwrap();
    let none: &[Vec<u8>] = &[];
    let outcome = pins.check("hl.example:5500", &c, none, 50).unwrap();
    assert!(matches!(outcome, Outcome::Pinned(_)), "{outcome:?}");
    assert!(dir.path().join("server-keys.toml").is_file());
}
