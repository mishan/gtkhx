//! The DIR-chunk encoding, pinned byte for byte.

use super::*;

/// The component names a DIR chunk carries, and checks its framing on the way.
fn components(enc: &[u8]) -> Vec<Vec<u8>> {
    let count = u16::from_be_bytes([enc[0], enc[1]]) as usize;
    let mut out = Vec::new();
    let mut pos = 2;
    for _ in 0..count {
        assert_eq!(&enc[pos..pos + 2], [0, 0], "enc field");
        let n = enc[pos + 2] as usize;
        out.push(enc[pos + 3..pos + 3 + n].to_vec());
        pos += 3 + n;
    }
    assert_eq!(pos, enc.len(), "no trailing bytes");
    out
}

fn names(v: &[&str]) -> Vec<Vec<u8>> {
    v.iter().map(|s| s.as_bytes().to_vec()).collect()
}

#[test]
fn empty_path_has_no_components() {
    assert_eq!(encode_dir(b"", false), [0, 0]);
    assert_eq!(encode_dir(b"", true), [0, 0]);
    assert_eq!(encode_dir(b"/", false), [0, 0]);
}

#[test]
fn single_component() {
    assert_eq!(encode_dir(b"files", false), b"\0\x01\0\0\x05files");
    assert_eq!(encode_dir(b"README.txt", true), [0, 0]);
}

#[test]
fn nested_path() {
    let enc = encode_dir(b"files/Oni Tracks/loop", false);
    assert_eq!(enc.len(), 30);
    assert_eq!(components(&enc), names(&["files", "Oni Tracks", "loop"]));
    let enc = encode_dir(b"files/Oni Tracks/loop", true);
    assert_eq!(components(&enc), names(&["files", "Oni Tracks"]));
}

#[test]
fn leading_trailing_and_doubled_separators_are_skipped() {
    assert_eq!(
        components(&encode_dir(b"/files/", false)),
        names(&["files"])
    );
    assert_eq!(components(&encode_dir(b"a//b", false)), names(&["a", "b"]));
    assert_eq!(components(&encode_dir(b"/a/b/", true)), names(&["a", "b"]));
    assert_eq!(components(&encode_dir(b"/a/b", true)), names(&["a"]));
}

#[test]
fn names_are_opaque_bytes() {
    let enc = encode_dir("caf\u{e9}".as_bytes(), false);
    assert_eq!(components(&enc), vec!["caf\u{e9}".as_bytes().to_vec()]);
    let enc = encode_dir(b"\xa5\x80:x", false);
    assert_eq!(components(&enc), vec![b"\xa5\x80:x".to_vec()]);
}

#[test]
fn a_long_component_is_cut_to_what_its_length_byte_holds() {
    let long = vec![b'x'; 300];
    let enc = encode_dir(&long, false);
    assert_eq!(components(&enc), vec![vec![b'x'; 255]]);
}

#[test]
fn a_chunk_never_outgrows_its_u16_length() {
    let mut path = Vec::new();
    for _ in 0..300 {
        path.extend_from_slice(&[b'y'; 250]);
        path.push(b'/');
    }
    let enc = encode_dir(&path, false);
    assert!(enc.len() <= u16::MAX as usize);
    let parts = components(&enc);
    assert!(parts.len() < 300);
    assert!(parts.iter().all(|p| p.len() == 250));
}

#[test]
fn split_keeps_the_root_and_drops_other_trailing_separators() {
    assert_eq!(split(b"/a/b/c"), (&b"/a/b"[..], &b"c"[..]));
    assert_eq!(split(b"/c"), (&b"/"[..], &b"c"[..]));
    assert_eq!(split(b"c"), (&b""[..], &b"c"[..]));
    assert_eq!(split(b"/a/"), (&b"/a"[..], &b""[..]));
}

#[test]
fn the_c_abi_matches_and_hands_over_a_glib_buffer() {
    let mut len = 0u16;
    let p = unsafe { path_to_hldir(c"/Uploads/sub".as_ptr(), &mut len, 0) };
    let got = unsafe { std::slice::from_raw_parts(p, len as usize) }.to_vec();
    unsafe { glib::ffi::g_free(p as *mut _) };
    assert_eq!(got, encode_dir(b"/Uploads/sub", false));
}
