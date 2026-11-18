//! hl-identity's vectors, read from the checkout of the revision this
//! workspace pins, so that they are always the pinned one's.
//!
//! The checkout is found with `cargo metadata`, offline, which resolves
//! the whole lockfile, every platform's packages included, where a build
//! downloads only its own platform's: after `cargo fetch` it needs no
//! network.

#![allow(dead_code)]

use std::path::PathBuf;

use serde_json::Value as Json;

fn hl_identity_dir() -> PathBuf {
    let out = std::process::Command::new(env!("CARGO"))
        .args([
            "metadata",
            "--format-version",
            "1",
            "--offline",
            "--manifest-path",
        ])
        .arg(concat!(env!("CARGO_MANIFEST_DIR"), "/Cargo.toml"))
        .output()
        .expect("cargo metadata runs");
    assert!(
        out.status.success(),
        "cargo metadata, offline, needs the lockfile's packages for every \
         platform: run `cargo fetch` once with network\n{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let meta: Json = serde_json::from_slice(&out.stdout).unwrap();
    let pkg = meta["packages"]
        .as_array()
        .unwrap()
        .iter()
        .find(|p| p["name"] == "hl-identity")
        .expect("hl-identity in the graph");
    PathBuf::from(pkg["manifest_path"].as_str().unwrap())
        .parent()
        .unwrap()
        .to_owned()
}

pub fn vectors(file: &str) -> Json {
    let path = hl_identity_dir().join("vectors").join(file);
    serde_json::from_str(&std::fs::read_to_string(&path).unwrap()).unwrap()
}

pub fn str_of<'a>(v: &'a Json, key: &str) -> &'a str {
    v[key].as_str().unwrap_or_else(|| panic!("{key} in {v}"))
}

pub fn unhex(s: &str) -> Vec<u8> {
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
        .collect()
}

pub fn unhex32(s: &str) -> [u8; 32] {
    unhex(s).try_into().unwrap()
}

pub fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}
