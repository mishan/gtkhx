//! Empty by design. This crate exists only so both CI builds (voice-disabled
//! then voice-enabled, sharing one Cargo target dir) resolve identical Cargo
//! features on `either`, `hashbrown`, `num-traits`, and `smallvec`, so the
//! ~108 shared dependency crates compile once instead of twice. See
//! `Cargo.toml` for the full rationale. Nothing links or references this
//! crate.
