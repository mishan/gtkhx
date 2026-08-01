//! `hxcrypto` — the crypto and compression primitives behind the HOPE
//! handshake and the transport transforms.
//!
//! Four crates, one job. They were split when each replaced its own C file
//! (`hmac.c`, `cipher.c`, `cipher_aead.c`, `compress.c`); all four of those
//! are long deleted, none had a C caller left, and `hxnet` — their only
//! consumer — was reaching past `hxcrypto::compress` to depend on `flate2`, `lz4_flex`
//! and `zstd` directly, so the compression code sat in the dependency graph
//! twice. Merging removes the duplicate and keeps one auditable crate.
//!
//! - [`hash`] — MD5 / SHA-1 / SHA-256 and the HMAC dispatcher, including the
//!   legacy `key||text` branches that HOPE login against old servers needs.
//!   Those are pinned byte-for-byte by tests; do not "fix" them into RFC 2104.
//! - [`stream`] — Blowfish OFB-64, with the save/restore of the 9-byte
//!   feedback state that the speculative-decode rollback depends on.
//! - [`aead`] — ChaCha20-Poly1305 + HKDF-SHA256. `AeadState`'s 48-byte layout
//!   is asserted on both sides of the FFI.
//! - [`compress`] — the GZIP / LZ4 / Zstandard dispatcher.
//!
//! **The negotiated wire format is not ours to change** — only the
//! implementation underneath it. 1.2 / 1.5 / 1.9 compatibility is a hard
//! requirement, and the Tier 3 login matrix is what proves it.

pub mod aead;
pub mod compress;
pub mod hash;
pub mod stream;
