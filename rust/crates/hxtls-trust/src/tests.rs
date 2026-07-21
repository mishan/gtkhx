//! Tests over a tmpdir known_hosts file.

use super::*;
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};

struct TmpDir(PathBuf);

impl TmpDir {
    fn new() -> TmpDir {
        static N: AtomicUsize = AtomicUsize::new(0);
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let dir = std::env::temp_dir().join(format!(
            "hxtls-trust-test-{}-{}-{}",
            std::process::id(),
            N.fetch_add(1, Ordering::Relaxed),
            nanos
        ));
        std::fs::create_dir_all(&dir).unwrap();
        TmpDir(dir)
    }
    fn kh(&self) -> PathBuf {
        self.0.join("known_hosts")
    }
}

impl Drop for TmpDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

const FP_A: &str = "sha256:aaaa000000000000000000000000000000000000000000000000000000000000";
const FP_B: &str = "sha256:bbbb000000000000000000000000000000000000000000000000000000000000";

#[test]
fn missing_file_is_unknown() {
    let tmp = TmpDir::new();
    assert_eq!(lookup(&tmp.kh(), "host", 5600, FP_A), TrustStatus::Unknown);
    assert!(!host_has_fingerprint(&tmp.kh(), "host", FP_A));
}

#[test]
fn pin_then_lookup_trusted() {
    let tmp = TmpDir::new();
    pin(&tmp.kh(), "hotline.example", 5600, FP_A, "2026-06-01").unwrap();
    assert_eq!(
        lookup(&tmp.kh(), "hotline.example", 5600, FP_A),
        TrustStatus::Trusted
    );
    // Wrong port → unknown; wrong fingerprint at the right port → mismatch.
    assert_eq!(
        lookup(&tmp.kh(), "hotline.example", 5601, FP_A),
        TrustStatus::Unknown
    );
    assert_eq!(
        lookup(&tmp.kh(), "hotline.example", 5600, FP_B),
        TrustStatus::Mismatch
    );
}

#[test]
fn host_case_insensitive() {
    let tmp = TmpDir::new();
    pin(&tmp.kh(), "Hotline.Example", 5600, FP_A, "2026-06-01").unwrap();
    assert_eq!(
        lookup(&tmp.kh(), "hotline.example", 5600, FP_A),
        TrustStatus::Trusted
    );
}

#[test]
fn pin_replaces_old_entry_and_keeps_others() {
    let tmp = TmpDir::new();
    // Seed a comment + an unrelated host + the host we'll re-pin.
    std::fs::write(
        tmp.kh(),
        format!(
            "# my known hosts\n\
             other.example:5600 {FP_B} # added 2026-01-01\n\
             hotline.example:5600 {FP_A} # added 2026-01-01\n"
        ),
    )
    .unwrap();

    // Re-pin the same host:port with a new fingerprint (rotation).
    pin(&tmp.kh(), "hotline.example", 5600, FP_B, "2026-06-02").unwrap();

    let after = std::fs::read_to_string(tmp.kh()).unwrap();
    assert!(after.contains("# my known hosts")); // comment preserved
    assert!(after.contains(&format!("other.example:5600 {FP_B}"))); // unrelated kept
    // Old fingerprint for hotline gone, new one present exactly once.
    assert_eq!(after.matches("hotline.example:5600").count(), 1);
    assert_eq!(
        lookup(&tmp.kh(), "hotline.example", 5600, FP_B),
        TrustStatus::Trusted
    );
    // The old fingerprint is no longer pinned, but the host:port still has an
    // entry (now FP_B), so looking up FP_A is a Mismatch, not Unknown.
    assert_eq!(
        lookup(&tmp.kh(), "hotline.example", 5600, FP_A),
        TrustStatus::Mismatch
    );
}

#[test]
fn host_has_fingerprint_any_port() {
    let tmp = TmpDir::new();
    pin(&tmp.kh(), "host", 5600, FP_A, "2026-06-01").unwrap();
    // Same fp, different port → the any-port check sees it (silent reuse).
    assert!(host_has_fingerprint(&tmp.kh(), "host", FP_A));
    assert!(!host_has_fingerprint(&tmp.kh(), "host", FP_B));
    assert!(!host_has_fingerprint(&tmp.kh(), "other", FP_A));
}

#[test]
fn hostname_only_entry_matches_any_port() {
    let tmp = TmpDir::new();
    std::fs::write(tmp.kh(), format!("legacy.example {FP_A}\n")).unwrap();
    assert_eq!(
        lookup(&tmp.kh(), "legacy.example", 1234, FP_A),
        TrustStatus::Trusted
    );
    assert_eq!(
        lookup(&tmp.kh(), "legacy.example", 9999, FP_A),
        TrustStatus::Trusted
    );
}

#[test]
fn malformed_entries_skipped() {
    let tmp = TmpDir::new();
    std::fs::write(
        tmp.kh(),
        format!(
            "host:notaport {FP_A}\n\
             host:99999 {FP_A}\n\
             host:5600 notafingerprint\n\
             host:5600 {FP_A}\n"
        ),
    )
    .unwrap();
    // Only the last (well-formed) line counts.
    assert_eq!(lookup(&tmp.kh(), "host", 5600, FP_A), TrustStatus::Trusted);
}

#[test]
fn bracketed_ipv6_entry() {
    let tmp = TmpDir::new();
    std::fs::write(tmp.kh(), format!("[::1]:5600 {FP_A}\n")).unwrap();
    assert_eq!(lookup(&tmp.kh(), "::1", 5600, FP_A), TrustStatus::Trusted);
    assert_eq!(lookup(&tmp.kh(), "::1", 5601, FP_A), TrustStatus::Unknown);
}

#[test]
fn later_correct_fingerprint_beats_earlier_mismatch() {
    let tmp = TmpDir::new();
    // Two hand-edited pins for the same host:port; the matching one wins.
    std::fs::write(
        tmp.kh(),
        format!("host:5600 {FP_B}\nhost:5600 {FP_A}\n"),
    )
    .unwrap();
    assert_eq!(lookup(&tmp.kh(), "host", 5600, FP_A), TrustStatus::Trusted);
}

#[test]
fn pinned_other_host_is_unknown() {
    let tmp = TmpDir::new();
    pin(&tmp.kh(), "localhost", 5600, FP_A, "2026-06-01").unwrap();
    // A different host (even the right fp) isn't covered → first-run Unknown.
    assert_eq!(
        lookup(&tmp.kh(), "other.example", 5600, FP_A),
        TrustStatus::Unknown
    );
}

#[test]
fn hostname_only_wrong_fp_is_mismatch() {
    let tmp = TmpDir::new();
    std::fs::write(tmp.kh(), format!("myserver {FP_A} # hand-pinned\n")).unwrap();
    // The relaxed any-port match must NOT relax fingerprint matching: a
    // different fp on a hostname-only entry is still a MISMATCH.
    assert_eq!(
        lookup(&tmp.kh(), "myserver", 5500, FP_B),
        TrustStatus::Mismatch
    );
}

#[test]
fn malformed_port_never_widens_trust() {
    let tmp = TmpDir::new();
    // Each hand-typo'd port form must be rejected outright — never silently
    // turned into a hostname-only (any-port) wildcard pin.
    for bad in [
        format!("bad.example:foo {FP_A}\n"),        // non-numeric
        format!("bad.example: {FP_A}\n"),           // empty port
        format!("bad.example:99999 {FP_A}\n"),      // out of u16 range
        format!("bad.example:5500garbage {FP_A}\n"), // digit prefix + junk
        format!("bad.example:-1 {FP_A}\n"),         // negative
    ] {
        std::fs::write(tmp.kh(), &bad).unwrap();
        assert_eq!(
            lookup(&tmp.kh(), "bad.example", 5500, FP_A),
            TrustStatus::Unknown,
            "malformed line should not match: {bad:?}"
        );
        assert_eq!(
            lookup(&tmp.kh(), "bad.example", 0, FP_A),
            TrustStatus::Unknown,
            "malformed line should not match any-port: {bad:?}"
        );
        assert!(
            !host_has_fingerprint(&tmp.kh(), "bad.example", FP_A),
            "malformed line should not widen host_has_fingerprint: {bad:?}"
        );
    }
    // Sanity: a genuine hostname-only entry (no colon) still works.
    std::fs::write(tmp.kh(), format!("good.example {FP_A}\n")).unwrap();
    assert_eq!(
        lookup(&tmp.kh(), "good.example", 5500, FP_A),
        TrustStatus::Trusted
    );
}

#[test]
fn leading_whitespace_entry_parses() {
    let tmp = TmpDir::new();
    // Spaces + a tab before the host token must still match (the trim keeps
    // the host/port/fp fields intact).
    std::fs::write(tmp.kh(), format!("  \tspaced.example:5500 {FP_A}\n")).unwrap();
    assert_eq!(
        lookup(&tmp.kh(), "spaced.example", 5500, FP_A),
        TrustStatus::Trusted
    );
}

#[test]
fn pin_appends_after_existing_lines() {
    let tmp = TmpDir::new();
    std::fs::write(
        tmp.kh(),
        format!("# header comment\n\nother.example:5500 {FP_B} # added 2026-01-01\n\n"),
    )
    .unwrap();
    pin(&tmp.kh(), "localhost", 5610, FP_A, "2026-06-02").unwrap();
    let after = std::fs::read_to_string(tmp.kh()).unwrap();
    // Comment before the unrelated entry before the freshly-appended pin.
    let p_comment = after.find("# header comment").unwrap();
    let p_other = after.find("other.example").unwrap();
    let p_pin = after.find("localhost:5610").unwrap();
    assert!(p_comment < p_other && p_other < p_pin);
    assert!(after.contains(FP_A));
}
