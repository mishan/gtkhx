//! Headless tests for the FILE_LIST receive handler + the Rust-owned cfl.

use super::*;

use hotline_proto::parse::FTYPE_FLDR;

const HTLS_DATA_FILE_LIST: u16 = 0x00c8;
const OTHER_TAG: u16 = 0x0064;
/// An arbitrary non-folder file type ('TEXT') for test entries.
const FTYPE_TEXT: u32 = u32::from_be_bytes(*b"TEXT");

/// One FILE_LIST entry body: ftype / fcreator / fsize / unknown / fnlen / name.
fn entry_body(ftype: u32, fsize: u32, name: &[u8]) -> Vec<u8> {
    let mut v = Vec::new();
    v.extend_from_slice(&ftype.to_be_bytes());
    v.extend_from_slice(&0u32.to_be_bytes()); // fcreator
    v.extend_from_slice(&fsize.to_be_bytes());
    v.extend_from_slice(&0u32.to_be_bytes()); // unknown
    v.extend_from_slice(&(name.len() as u32).to_be_bytes()); // fnlen
    v.extend_from_slice(name);
    v
}

/// Build a FILE_LIST reply: 22-byte header + the given chunks.
fn frame(chunks: &[(u16, Vec<u8>)]) -> Vec<u8> {
    let mut v = Vec::new();
    v.extend_from_slice(&0x0001_0000u32.to_be_bytes()); // type (TASK)
    v.extend_from_slice(&1u32.to_be_bytes()); // trans
    v.extend_from_slice(&0u32.to_be_bytes()); // flag (no error)
    v.extend_from_slice(&0u32.to_be_bytes()); // len
    v.extend_from_slice(&0u32.to_be_bytes()); // len2
    v.extend_from_slice(&(chunks.len() as u16).to_be_bytes()); // hc
    for (tag, data) in chunks {
        v.extend_from_slice(&tag.to_be_bytes());
        v.extend_from_slice(&(data.len() as u16).to_be_bytes());
        v.extend_from_slice(data);
    }
    v
}

/// An error reply (flag & 1) with no chunks.
fn error_frame() -> Vec<u8> {
    let mut v = Vec::new();
    v.extend_from_slice(&0x0001_0000u32.to_be_bytes());
    v.extend_from_slice(&1u32.to_be_bytes());
    v.extend_from_slice(&1u32.to_be_bytes()); // flag = error
    v.extend_from_slice(&0u32.to_be_bytes());
    v.extend_from_slice(&0u32.to_be_bytes());
    v.extend_from_slice(&0u16.to_be_bytes());
    v
}

const PROVIDER: *mut std::os::raw::c_void = 0xDA7A_usize as *mut std::os::raw::c_void;

unsafe fn run(cfl: *mut CachedFileList, f: &[u8], data: *mut std::os::raw::c_void) {
    rcv_task_file_list(
        std::ptr::null_mut(),
        f.as_ptr() as *const std::os::raw::c_void,
        f.len(),
        cfl as *mut std::os::raw::c_void,
        data,
    );
}

#[test]
fn basic_listing_accumulates_and_emits() {
    test_env::reset();
    let cfl = hx_cfl_new();
    let f = frame(&[
        (HTLS_DATA_FILE_LIST, entry_body(FTYPE_TEXT, 10, b"a.txt")),
        (OTHER_TAG, vec![0, 0, 0, 0]), // ignored non-FILE_LIST chunk
        (HTLS_DATA_FILE_LIST, entry_body(FTYPE_FLDR, 0, b"sub")),
    ]);
    unsafe { run(cfl, &f, PROVIDER) };

    // Two FILE_LIST records accumulated; the non-FILE_LIST chunk skipped.
    // Each record: 4 (hdr) + 20 (fixed) + name, rounded up to the next mult of 4.
    let rec1 = round4(4 + 20 + 5);
    let rec2 = round4(4 + 20 + 3);
    assert_eq!(unsafe { hx_cfl_fhlen(cfl) } as usize, rec1 + rec2);
    assert!(test_env::EMITTED.with(|c| c.get()));
    assert_eq!(unsafe { hx_cfl_completing(cfl) }, 0);
    assert!(test_env::COMPLETE_ENTRIES.with(|c| c.borrow().is_empty()));
    unsafe { hx_cfl_free(cfl) };
}

/// The C accumulation rounds each record up to the next multiple of 4 — and
/// bumps an already-aligned record by a full 4.
fn round4(n: usize) -> usize {
    n + (4 - (n % 4))
}

#[test]
fn appended_record_length_is_patched_to_padded_body() {
    test_env::reset();
    let cfl = hx_cfl_new();
    // name "hi" → body 20 + 2 = 22, record 4 + 22 = 26, padded to 28.
    let f = frame(&[(HTLS_DATA_FILE_LIST, entry_body(FTYPE_TEXT, 7, b"hi"))]);
    unsafe { run(cfl, &f, std::ptr::null_mut()) };

    let len = unsafe { hx_cfl_fhlen(cfl) } as usize;
    assert_eq!(len, 28);
    let buf = unsafe { std::slice::from_raw_parts(hx_cfl_fh(cfl) as *const u8, len) };
    // Patched length field (offset 2..4) = padded body = 28 - 4 = 24.
    assert_eq!(u16::from_be_bytes([buf[2], buf[3]]), 24);
    // data==NULL → no emit.
    assert!(!test_env::EMITTED.with(|c| c.get()));
    unsafe { hx_cfl_free(cfl) };
}

#[test]
fn task_error_notifies_provider_and_frees() {
    test_env::reset();
    let cfl = hx_cfl_new();
    unsafe { run(cfl, &error_frame(), PROVIDER) };
    // Provider got the error hook; nothing emitted. (cfl was freed by the
    // handler — do not touch it again.)
    assert!(test_env::PROVIDER_ERROR.with(|c| c.get()));
    assert!(!test_env::EMITTED.with(|c| c.get()));
}

#[test]
fn recursive_mode_hands_entries_to_the_engine() {
    test_env::reset();
    let cfl = hx_cfl_new();
    unsafe { hx_cfl_set_completing(cfl, 3) }; // COMPLETE_GET_R
    let f = frame(&[
        (HTLS_DATA_FILE_LIST, entry_body(FTYPE_FLDR, 0, b"dir")),
        (HTLS_DATA_FILE_LIST, entry_body(FTYPE_TEXT, 42, b"file.bin")),
    ]);
    unsafe { run(cfl, &f, std::ptr::null_mut()) };

    let entries = test_env::COMPLETE_ENTRIES.with(|c| c.borrow().clone());
    assert_eq!(entries.len(), 2);
    assert_eq!(entries[0], (true, b"dir".to_vec(), 0)); // 'fldr' → is_folder
    assert_eq!(entries[1], (false, b"file.bin".to_vec(), 42));
    // Records still accumulated even in recursive mode.
    assert!(unsafe { hx_cfl_fhlen(cfl) } > 0);
    unsafe { hx_cfl_free(cfl) };
}

#[test]
fn set_path_round_trips() {
    let cfl = hx_cfl_new();
    let p = std::ffi::CString::new("/Files/sub").unwrap();
    unsafe { hx_cfl_set_path(cfl, p.as_ptr()) };
    let got = unsafe { std::ffi::CStr::from_ptr(hx_cfl_path(cfl)) };
    assert_eq!(got.to_bytes(), b"/Files/sub");
    unsafe { hx_cfl_free(cfl) };
}
