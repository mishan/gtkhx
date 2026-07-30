//! `hxtls-trust` — the TLS trust-on-first-use (TOFU) `known_hosts` store, ported
//! from `src/tls_trust.c`.
//!
//! When a peer's TLS certificate fails WebPKI validation, GtkHx falls back to
//! SSH-style cert pinning: it looks the leaf cert's SHA-256 fingerprint up in a
//! per-user `known_hosts` file and classifies the result as
//! [`TrustStatus::Trusted`] / [`Unknown`](TrustStatus::Unknown) /
//! [`Mismatch`](TrustStatus::Mismatch). On an accepted first-use / rotation it
//! pins the new fingerprint.
//!
//! File format (one entry per non-comment line):
//!
//! ```text
//! <host>[:port] sha256:<64-hex> # added <ISO-8601 date>
//! ```
//!
//! Blank lines and `#` comments round-trip so a hand-edit isn't clobbered by the
//! next pin. A hostname-only entry (no `:port`) matches every port for that host
//! (legacy SSH convention; we always *write* `host:port`).
//!
//! `lib.rs` (this file) is the pure DB: every operation takes an explicit
//! `known_hosts` path, so `cargo test` runs headless against a tmpdir. The rest
//! of the trust brain — `known_hosts` path resolution (`$CONFIG` dir +
//! `GTKHX_KNOWN_HOSTS` override), the thread-safe test seams, and the
//! classify/decide orchestration plus the C ABI (`hx_tls_verify_cert` etc.) —
//! lives in this crate's `ffi.rs`. Only the Adwaita prompt + its worker→main
//! hop live in `gtkhx-ui`, reached through a callback registered via
//! `hx_tls_trust_set_prompt`.

use std::path::Path;

/// The verdict of looking a `(host, port, fingerprint)` up in `known_hosts`.
///
/// Numeric values are part of the C ABI: they’re passed as an `int` through the
/// registered prompt callback and must remain stable (Trusted=0, Unknown=1, Mismatch=2).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(i32)]
pub enum TrustStatus {
    /// Fingerprint matches a pinned entry for this host:port — accept silently.
    Trusted = 0,
    /// No entry pinned for this host:port — first time we've seen this server.
    Unknown = 1,
    /// An entry IS pinned but the fingerprint differs — rotation, move, or MITM.
    Mismatch = 2,
}

/// Split a `known_hosts` host field into `(host, port)`. `port == 0` means a
/// hostname-only entry (matches any port). Returns `None` for a malformed field
/// (e.g. a non-numeric or out-of-range port) so a typo never widens into the
/// any-port wildcard. Handles `[ipv6]:port`, `host:port`, and bare host / bare
/// IPv6-literal (taken as host-only), mirroring `gtkhx_parse_host_port`.
fn parse_host_field(field: &str) -> Option<(String, u16)> {
    if let Some(rest) = field.strip_prefix('[') {
        // Bracketed IPv6: `[::1]` or `[::1]:5600`.
        let close = rest.find(']')?;
        let host = &rest[..close];
        if host.is_empty() {
            return None;
        }
        let after = &rest[close + 1..];
        if after.is_empty() {
            return Some((host.to_string(), 0));
        }
        let port = after.strip_prefix(':')?.parse::<u16>().ok()?;
        if port == 0 {
            return None;
        }
        return Some((host.to_string(), port));
    }

    match field.bytes().filter(|&b| b == b':').count() {
        // Bare host, or an unbracketed IPv6 literal → host-only (no port).
        0 => Some((field.to_string(), 0)),
        n if n > 1 => Some((field.to_string(), 0)),
        // Exactly one colon → host:port.
        _ => {
            let (host, port_str) = field.rsplit_once(':')?;
            if host.is_empty() {
                return None;
            }
            let port = port_str.parse::<u16>().ok()?;
            if port == 0 {
                return None;
            }
            Some((host.to_string(), port))
        }
    }
}

/// One parsed non-comment line: its host field + the `sha256:` fingerprint token.
struct Entry {
    host: String,
    port: u16,
    fingerprint: String,
}

/// Parse a `known_hosts` line into an [`Entry`], or `None` for a comment / blank
/// / malformed line.
fn parse_line(line: &str) -> Option<Entry> {
    let trimmed = line.trim_start_matches([' ', '\t']);
    if trimmed.is_empty() || trimmed.starts_with('#') {
        return None;
    }
    let mut fields = trimmed.split_whitespace();
    let host_field = fields.next()?;
    let (host, port) = parse_host_field(host_field)?;
    // The fingerprint token ends at whitespace OR a `#` — the old C reader let a
    // comment abut the fingerprint (`sha256:...#added`) with no intervening
    // space, so cut at the first `#` to keep matching those hand-edited lines.
    // (A sha256 hex fingerprint never contains `#`.)
    let fp_token = fields.next()?;
    let fp = fp_token.split('#').next().unwrap_or(fp_token);
    if !fp.starts_with("sha256:") {
        return None;
    }
    Some(Entry {
        host,
        port,
        fingerprint: fp.to_string(),
    })
}

/// Case-insensitive host match; `entry_port == 0` (hostname-only) matches any.
fn host_port_match(entry_host: &str, entry_port: u16, host: &str, port: u16) -> bool {
    entry_host.eq_ignore_ascii_case(host) && (entry_port == 0 || entry_port == port)
}

/// Classify `(host, port, fingerprint)` against the `known_hosts` file. A
/// missing / unreadable file is treated as empty (every cert is `Unknown`),
/// giving a sane first-run experience.
pub fn lookup(known_hosts: &Path, host: &str, port: u16, fingerprint: &str) -> TrustStatus {
    // Read raw bytes + a lossy UTF-8 view (never read_to_string): a stray
    // non-UTF-8 byte in a comment must not make the whole store look empty and
    // re-prompt for an otherwise-valid pinned entry.
    let Ok(bytes) = std::fs::read(known_hosts) else {
        return TrustStatus::Unknown;
    };
    let contents = String::from_utf8_lossy(&bytes);
    let mut result = TrustStatus::Unknown;
    for line in contents.lines() {
        let Some(e) = parse_line(line) else { continue };
        if !host_port_match(&e.host, e.port, host, port) {
            continue;
        }
        if e.fingerprint == fingerprint {
            return TrustStatus::Trusted; // exact match wins immediately
        }
        // Same host:port, different fingerprint — at least a mismatch, but keep
        // walking in case a later (hand-edited) line has the right fingerprint.
        result = TrustStatus::Mismatch;
    }
    result
}

/// Whether any entry pins `fingerprint` for `host` at *any* port (including a
/// hostname-only entry). Used to silently accept a cert already trusted for the
/// same host on another port (control channel :5600 → HTXF subchannel :5601).
/// Only safe to consult on a strict-`Unknown`; never overrides a `Mismatch`.
pub fn host_has_fingerprint(known_hosts: &Path, host: &str, fingerprint: &str) -> bool {
    let Ok(bytes) = std::fs::read(known_hosts) else {
        return false;
    };
    let contents = String::from_utf8_lossy(&bytes);
    contents
        .lines()
        .filter_map(parse_line)
        .any(|e| e.host.eq_ignore_ascii_case(host) && e.fingerprint == fingerprint)
}

/// Append (or replace) a `(host, port, fingerprint)` pin. Existing entries for
/// the same `(host, port)` are dropped (the old fingerprint we're replacing);
/// comments / blank lines / unrelated entries round-trip verbatim. Written
/// atomically (temp file + rename) with mode 0600. `now_date` is the
/// `YYYY-MM-DD` string for the "added" comment (injected so the pure logic stays
/// testable).
pub fn pin(
    known_hosts: &Path,
    host: &str,
    port: u16,
    fingerprint: &str,
    now_date: &str,
) -> std::io::Result<()> {
    // Lossy UTF-8 view of the raw bytes — never read_to_string, whose decode
    // error on a stray non-UTF-8 byte would default to "" and clobber every
    // existing pin + comment on the rewrite below. A missing file → "".
    let existing = std::fs::read(known_hosts)
        .map(|b| String::from_utf8_lossy(&b).into_owned())
        .unwrap_or_default();

    let mut out = String::new();
    for line in existing.lines() {
        match parse_line(line) {
            // Drop the entry we're replacing; keep everything else verbatim.
            Some(e) if host_port_match(&e.host, e.port, host, port) => continue,
            _ => {
                out.push_str(line);
                out.push('\n');
            }
        }
    }
    out.push_str(&format!("{host}:{port} {fingerprint} # added {now_date}\n"));

    write_atomic(known_hosts, out.as_bytes())
}

/// Apply a Unix file mode to `opts` on platforms that model one; a no-op on
/// Windows, whose `OpenOptions` has no mode concept (the temp file inherits the
/// config directory's ACL there instead of an explicit 0600).
#[cfg(unix)]
fn with_mode(opts: &mut std::fs::OpenOptions, mode: u32) {
    use std::os::unix::fs::OpenOptionsExt;
    opts.mode(mode);
}
#[cfg(not(unix))]
fn with_mode(_opts: &mut std::fs::OpenOptions, _mode: u32) {}

/// Atomic write: temp file in the same directory (mode 0600 on Unix) + rename.
/// Mirrors `hx_tls_trust_pin`'s "surgical file primitives" — the config
/// directory is assumed to exist (created at app startup), so no `mkdir` here.
fn write_atomic(path: &Path, data: &[u8]) -> std::io::Result<()> {
    use std::io::{ErrorKind, Write};

    let dir = path.parent().unwrap_or_else(|| Path::new("."));
    let file_name = path
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| "known_hosts".into());

    // Open a fresh temp file in the same dir (rename must be same-fs) with
    // O_EXCL so we never clobber a concurrent writer's temp. The name mixes
    // pid + a nanosecond stamp + a monotonic counter for entropy, but a stale
    // temp left by a crashed run whose pid gets reused could still collide, so
    // retry a few times on AlreadyExists (each attempt draws a new stamp/seq)
    // rather than failing the pin outright.
    let mut f = None;
    let mut tmp = std::path::PathBuf::new();
    let mut last_err = None;
    for _ in 0..16 {
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        tmp = dir.join(format!(
            ".{file_name}.tmp.{}.{}.{}",
            std::process::id(),
            nanos,
            next_seq()
        ));
        let mut opts = std::fs::OpenOptions::new();
        opts.write(true).create_new(true);
        with_mode(&mut opts, 0o600);
        match opts.open(&tmp) {
            Ok(handle) => {
                f = Some(handle);
                break;
            }
            Err(e) if e.kind() == ErrorKind::AlreadyExists => {
                last_err = Some(e);
                continue;
            }
            Err(e) => return Err(e),
        }
    }
    let Some(mut f) = f else {
        return Err(last_err.unwrap_or_else(|| {
            std::io::Error::new(ErrorKind::AlreadyExists, "known_hosts temp file collision")
        }));
    };

    if let Err(e) = f.write_all(data).and_then(|_| f.sync_all()) {
        let _ = std::fs::remove_file(&tmp);
        return Err(e);
    }
    drop(f);
    if let Err(e) = std::fs::rename(&tmp, path) {
        let _ = std::fs::remove_file(&tmp);
        return Err(e);
    }
    Ok(())
}

fn next_seq() -> u64 {
    use std::sync::atomic::{AtomicU64, Ordering};
    static SEQ: AtomicU64 = AtomicU64::new(0);
    SEQ.fetch_add(1, Ordering::Relaxed)
}

pub mod ffi;

#[cfg(test)]
mod tests;
