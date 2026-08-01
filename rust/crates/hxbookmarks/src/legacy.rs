//! Read and write the legacy Hotline "HTsc" 460-byte fixed bookmark format
//! (port of `src/bookmarks_io.c`'s parse/save), used for one-time import of
//! old per-file bookmarks and for the "Export legacy format" action.
//!
//! Byte layout (matches the C writer byte-for-byte, so files round-trip with
//! the old client and other Hotline clients):
//!
//! ```text
//!   0.. 4   "HTsc"
//!   4       0x00, 0x01           (version)
//!   6..135  129 zero bytes
//!   135     login length byte    (ignored on read; field is NUL-padded)
//!   136..169  login  (33-byte NUL-padded field)
//!   169     pass length byte     (ignored)
//!   170..203  pass   (33-byte NUL-padded field)
//!   203     server length byte = L
//!   204..204+L  "host" / "host:port"
//!   +0      secure (HOPE) flag
//!   +1      compress index
//!   +2      cipher stable byte
//!   +3      tls flag             (absent in pre-TLS files → 0)
//!   ...     zero padding to a 256-byte server block (total file 460 bytes)
//! ```

use std::fs;
use std::io;
use std::path::Path;

use crate::{hostport, Bookmark};

/// Parse one HTsc file's bytes into a [`Bookmark`] (with an empty `name` —
/// the caller sets it from the filename). Returns `None` for a non-HTsc /
/// truncated / corrupt buffer. Lenient on the trailing TLS byte (absent →
/// off), matching the C reader.
///
/// Only the HTsc binary format is recognized. The ancient pre-HTsc GtkHx
/// 3-line text bookmark format is deliberately not imported — HTsc predates
/// it in the wild and no such files are left (per project decision).
pub fn parse(bytes: &[u8]) -> Option<Bookmark> {
    if bytes.len() < 4 || &bytes[0..4] != b"HTsc" {
        return None;
    }
    let login = cstr_field(bytes.get(136..169)?);
    let pass = cstr_field(bytes.get(170..203)?);

    let len = *bytes.get(203)? as usize;
    let server_raw = bytes.get(204..204 + len)?;
    let server_str = String::from_utf8_lossy(server_raw).into_owned();

    // secure / compress / cipher are required (the C load fails without them);
    // tls is optional (pre-TLS files zero-pad past flag 3).
    let secure = *bytes.get(204 + len)?;
    let compress = *bytes.get(205 + len)?;
    let cipher = *bytes.get(206 + len)?;
    let tls = bytes.get(207 + len).copied().unwrap_or(0);

    let (host, port) = hostport::split(&server_str);
    Some(Bookmark {
        name: String::new(),
        server: host,
        port,
        login,
        password: pass,
        hope: secure != 0,
        compress,
        cipher,
        tls: tls != 0,
        // The format is a fixed 460 bytes with four flag bytes and no room for
        // more, so an imported bookmark inherits the global identity. That is
        // the right answer as well as the only available one: this format is
        // interop with clients that have no concept of a per-connection
        // nickname, and `write` correspondingly drops an override rather than
        // inventing a field the other side would not understand.
        nick: None,
        icon: None,
    })
}

/// Serialize a [`Bookmark`] to the 460-byte HTsc format.
pub fn write(bm: &Bookmark) -> Vec<u8> {
    let mut out = Vec::with_capacity(460);
    out.extend_from_slice(b"HTsc\x00\x01"); // header + version (6)
    out.extend_from_slice(&[0u8; 129]); // → offset 135

    write_field(&mut out, &bm.login); // 34 bytes → 169
    write_field(&mut out, &bm.password); // 34 bytes → 203

    let server_str = hostport::join(&bm.server, &bm.port);
    let sbytes = server_str.as_bytes();
    let len = sbytes.len().min(251);
    out.push(len as u8);
    out.extend_from_slice(&sbytes[..len]);
    out.push(bm.hope as u8);
    out.push(bm.compress);
    out.push(bm.cipher);
    out.push(bm.tls as u8);
    // Zero-pad the server block to 256 bytes (len + 4 flags + pad).
    out.resize(out.len() + (256 - len - 4), 0);

    debug_assert_eq!(out.len(), 460);
    out
}

/// Import every HTsc file in `dir`, in filename order. Non-bookmark /
/// unreadable / dotfiles are skipped. Each bookmark's `name` is set from its
/// filename. Returns an empty vec if `dir` doesn't exist.
pub fn import_dir(dir: &Path) -> Vec<Bookmark> {
    let mut out = Vec::new();
    let Ok(rd) = fs::read_dir(dir) else {
        return out;
    };
    let mut entries: Vec<_> = rd.flatten().collect();
    entries.sort_by_key(|e| e.file_name());
    for ent in entries {
        let fname = ent.file_name();
        let fname = fname.to_string_lossy();
        if fname.starts_with('.') {
            continue;
        }
        let Ok(bytes) = fs::read(ent.path()) else {
            continue;
        };
        if let Some(mut bm) = parse(&bytes) {
            bm.name = fname.into_owned();
            out.push(bm);
        }
    }
    out
}

/// Write each bookmark as a separate HTsc file into `dir` (created if
/// needed). The filename is the bookmark name with `/` defanged to `\` (the
/// legacy convention). Returns the number of files written.
pub fn export_dir(bookmarks: &[Bookmark], dir: &Path) -> io::Result<usize> {
    fs::create_dir_all(dir)?;
    let mut n = 0;
    for bm in bookmarks {
        if bm.name.is_empty() {
            continue;
        }
        let path = dir.join(safe_filename(&bm.name));
        fs::write(&path, write(bm))?;
        n += 1;
    }
    Ok(n)
}

/// Defang a bookmark name into a safe on-disk filename (`/` → `\`), matching
/// the C `hx_bookmark_safe_filename`.
pub fn safe_filename(name: &str) -> String {
    name.replace('/', "\\")
}

/// Read a NUL-terminated string out of a fixed-size field.
fn cstr_field(field: &[u8]) -> String {
    let end = field.iter().position(|&c| c == 0).unwrap_or(field.len());
    String::from_utf8_lossy(&field[..end]).into_owned()
}

/// Write a login/pass field: a 1-byte length, then the value clamped to 32
/// bytes and NUL-padded to a 33-byte field — 34 bytes total.
fn write_field(out: &mut Vec<u8>, s: &str) {
    let b = s.as_bytes();
    let len = b.len().min(32);
    out.push(len as u8);
    out.extend_from_slice(&b[..len]);
    out.resize(out.len() + (33 - len), 0);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cipher;

    fn sample() -> Bookmark {
        Bookmark {
            name: "Test Server".into(),
            server: "example.com".into(),
            port: "5501".into(),
            login: "guest".into(),
            password: "secret".into(),
            hope: true,
            compress: 1,
            cipher: cipher::BLOWFISH,
            tls: false,
            nick: None,
            icon: None,
        }
    }

    #[test]
    fn write_is_460_bytes_with_header() {
        let b = write(&sample());
        assert_eq!(b.len(), 460);
        assert_eq!(&b[0..6], b"HTsc\x00\x01");
    }

    #[test]
    fn round_trips_through_bytes() {
        let bm = sample();
        let bytes = write(&bm);
        let mut got = parse(&bytes).expect("parse");
        got.name = bm.name.clone(); // name comes from filename, not the bytes
        assert_eq!(got, bm);
    }

    #[test]
    fn tls_flag_round_trips() {
        let mut bm = sample();
        bm.tls = true;
        let mut got = parse(&write(&bm)).unwrap();
        got.name = bm.name.clone();
        assert!(got.tls);
    }

    #[test]
    fn pre_tls_short_file_reads_tls_off() {
        // Truncate the trailing padding so only 3 flag bytes exist.
        let bm = sample();
        let mut bytes = write(&bm);
        let len = bm.server.len() + 1 + bm.port.len(); // "example.com:5501"
        let cut = 204 + len + 3; // through the cipher flag, no tls byte
        bytes.truncate(cut);
        let got = parse(&bytes).unwrap();
        assert!(!got.tls);
    }

    #[test]
    fn non_htsc_is_none() {
        assert!(parse(b"not a bookmark").is_none());
        assert!(parse(&[]).is_none());
    }

    #[test]
    fn host_only_no_port() {
        let mut bm = sample();
        bm.port = String::new();
        let mut got = parse(&write(&bm)).unwrap();
        got.name = bm.name.clone();
        assert_eq!(got.server, "example.com");
        assert_eq!(got.port, "");
    }
}
