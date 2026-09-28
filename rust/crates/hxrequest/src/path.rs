//! Remote paths and the DIR-chunk encoding.
//!
//! Remote paths are `/`-separated, with `/` as the root. On the wire a
//! directory travels as a DIR chunk: a big-endian u16 component count, then
//! per component two zero bytes, a length byte, and the name's bytes. Every
//! file-bearing opcode carries one for the directory part of its target.

#[cfg(feature = "c-abi")]
use std::ffi::{c_char, c_int, CStr};

/// The remote path separator.
pub const SEP: u8 = b'/';

/// The longest name one DIR component can carry (its length is one byte).
pub const MAX_COMPONENT: usize = u8::MAX as usize;

/// Encode `path` as a DIR chunk. Empty components — a leading, trailing or
/// doubled separator — are skipped. With `is_file` the last component names
/// the file itself and is left out, for the requests that carry the name in
/// its own FILE_NAME chunk.
///
/// A component longer than [`MAX_COMPONENT`] bytes is cut to fit. Components
/// that would push the chunk past what its u16 length can describe are
/// dropped.
pub fn encode_dir(path: &[u8], is_file: bool) -> Vec<u8> {
    let mut parts: Vec<&[u8]> = path.split(|&b| b == SEP).collect();
    // Only a component with a separator after it is a directory when the
    // path names a file.
    if is_file {
        parts.pop();
    }
    let mut out = vec![0u8, 0u8];
    let mut count: u16 = 0;
    for part in parts.into_iter().filter(|p| !p.is_empty()) {
        let name = &part[..part.len().min(MAX_COMPONENT)];
        if out.len() + 3 + name.len() > u16::MAX as usize {
            break;
        }
        out.extend_from_slice(&[0, 0, name.len() as u8]);
        out.extend_from_slice(name);
        count += 1;
    }
    out[..2].copy_from_slice(&count.to_be_bytes());
    out
}

/// The offset at which the last component of `path` starts: 0 with no
/// separator, `path.len()` with a trailing one.
pub fn basename_offset(path: &[u8]) -> usize {
    path.iter().rposition(|&b| b == SEP).map_or(0, |i| i + 1)
}

/// `path` without its last component, as `(dir, name)`. The directory keeps
/// no trailing separator, except the root itself.
pub fn split(path: &[u8]) -> (&[u8], &[u8]) {
    let base = basename_offset(path);
    let dir = match base {
        0 => &path[..0],
        1 => &path[..1],
        n => &path[..n - 1],
    };
    (dir, &path[base..])
}

/// Whether a remote directory names something below the root.
pub fn below_root(dir: &[u8]) -> bool {
    !dir.is_empty() && dir != [SEP]
}

#[cfg(feature = "c-abi")]
/// `guint8 *path_to_hldir (const char *path, guint16 *hldirlen, int is_file)`
/// — [`encode_dir`] for C and for the Rust callers still on the C shape.
/// Returns a `g_malloc`'d buffer the caller `g_free`s, its length in
/// `hldirlen`.
///
/// # Safety
/// `path` is a NUL-terminated C string; `hldirlen` is valid for a write.
#[no_mangle]
pub unsafe extern "C" fn path_to_hldir(
    path: *const c_char,
    hldirlen: *mut u16,
    is_file: c_int,
) -> *mut u8 {
    let bytes = if path.is_null() {
        &[][..]
    } else {
        CStr::from_ptr(path).to_bytes()
    };
    let enc = encode_dir(bytes, is_file != 0);
    let buf = glib::ffi::g_malloc(enc.len()) as *mut u8;
    std::ptr::copy_nonoverlapping(enc.as_ptr(), buf, enc.len());
    if !hldirlen.is_null() {
        *hldirlen = enc.len() as u16;
    }
    buf
}

#[cfg(test)]
mod tests;
