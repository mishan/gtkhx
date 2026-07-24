//! Headless tests for the file-transfer reply tail, driven through the
//! `test_env` recording doubles for the emit / xfer C ABIs.

use super::*;

/// A sentinel htxf pointer (never dereferenced by the crate).
fn fake_htxf() -> *mut std::os::raw::c_void {
    0xF11E_usize as *mut std::os::raw::c_void
}

fn announce(queue: u32) {
    unsafe { hx_xfer_announce(std::ptr::null_mut(), fake_htxf(), queue) }
}

#[test]
fn ready_transfer_announces_and_starts() {
    // queue == 0: server cleared us to transfer → announce + start writing.
    test_env::reset();
    announce(0);
    assert_eq!(test_env::EMITTED.with(|c| c.take()), Some(fake_htxf()));
    assert_eq!(test_env::STARTED.with(|c| c.take()), Some(fake_htxf()));
}

#[test]
fn queued_transfer_announces_only() {
    // queue != 0: parked in the server queue → announce position, don't start.
    test_env::reset();
    announce(3);
    assert_eq!(test_env::EMITTED.with(|c| c.take()), Some(fake_htxf()));
    assert_eq!(test_env::STARTED.with(|c| c.take()), None);
}

// ---- reply-handler tests ---------------------------------------------------

// Wire chunk tags (src/hotline.h / messages.rs::tag).
const HTXF_REF: u16 = 0x006b;
const HTXF_SIZE: u16 = 0x006c;
const QUEUE: u16 = 0x0074;
const FILE_NAME: u16 = 0x00c9;
const RFLT: u16 = 0x00cb;
const FILE_SIZE: u16 = 0x00cf;
const FILE_NFILES: u16 = 0x00dc;
const XFERSIZE64: u16 = 0x01f3;

/// Build a 22-byte transaction header (`flag & 1` = task-error) followed by the
/// concatenated TLV chunks — the exact shape `ChunkIter::over_message` expects.
fn reply(error: bool, chunks: &[(u16, Vec<u8>)]) -> Vec<u8> {
    let mut v = Vec::new();
    v.extend_from_slice(&0x0001_0000u32.to_be_bytes()); // type (TASK)
    v.extend_from_slice(&1u32.to_be_bytes()); // trans
    v.extend_from_slice(&(error as u32).to_be_bytes()); // flag
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

fn u32b(v: u32) -> Vec<u8> {
    v.to_be_bytes().to_vec()
}

/// Invoke a handler with a frame + the sentinel htxf as `ptr`.
unsafe fn call(
    f: unsafe extern "C" fn(
        *mut std::os::raw::c_void,
        *const std::os::raw::c_void,
        usize,
        *mut std::os::raw::c_void,
        *mut std::os::raw::c_void,
    ),
    frame: &[u8],
) {
    f(
        std::ptr::null_mut(),
        frame.as_ptr() as *const std::os::raw::c_void,
        frame.len(),
        fake_htxf(),
        std::ptr::null_mut(),
    );
}

#[test]
fn file_get_ready_applies_scalars() {
    test_env::reset();
    let f = reply(
        false,
        &[
            (HTXF_REF, u32b(7)),
            (HTXF_SIZE, u32b(4096)),
            (QUEUE, u32b(0)),
        ],
    );
    unsafe { call(rcv_task_file_get, &f) };
    assert_eq!(test_env::FILE_GET.with(|c| c.get()), Some((7, 4096, 0)));
    assert!(!test_env::GET_ERROR.with(|c| c.get()));
}

#[test]
fn file_get_prefers_size64() {
    test_env::reset();
    let big: u64 = 0x1_0000_0000; // 4 GiB, doesn't fit in the legacy u32
    let f = reply(
        false,
        &[
            (HTXF_REF, u32b(9)),
            (HTXF_SIZE, u32b(1)),
            (XFERSIZE64, big.to_be_bytes().to_vec()),
        ],
    );
    unsafe { call(rcv_task_file_get, &f) };
    assert_eq!(test_env::FILE_GET.with(|c| c.get()), Some((9, big, 0)));
}

#[test]
fn file_get_cancelled_transfer_dropped() {
    test_env::reset();
    test_env::IN_LIST.with(|c| c.set(0)); // htxf no longer live
    let f = reply(false, &[(HTXF_REF, u32b(7)), (HTXF_SIZE, u32b(4096))]);
    unsafe { call(rcv_task_file_get, &f) };
    assert_eq!(test_env::FILE_GET.with(|c| c.get()), None);
    assert!(!test_env::GET_ERROR.with(|c| c.get()));
}

#[test]
fn file_get_task_error_routes_to_retry() {
    test_env::reset();
    let f = reply(true, &[]);
    unsafe { call(rcv_task_file_get, &f) };
    assert!(test_env::GET_ERROR.with(|c| c.get()));
    assert_eq!(test_env::FILE_GET.with(|c| c.get()), None);
}

#[test]
fn file_get_malformed_no_ref_gated() {
    test_env::reset();
    // Size present but no ref → the (!ref) gate rejects it.
    let f = reply(false, &[(HTXF_SIZE, u32b(4096))]);
    unsafe { call(rcv_task_file_get, &f) };
    assert_eq!(test_env::FILE_GET.with(|c| c.get()), None);
    assert!(!test_env::GET_ERROR.with(|c| c.get()));
}

#[test]
fn folder_get_zero_size_clamps_to_one() {
    test_env::reset();
    let f = reply(false, &[(HTXF_REF, u32b(5)), (FILE_NFILES, u32b(3))]);
    unsafe { call(rcv_task_folder_get, &f) };
    assert_eq!(test_env::FOLDER_GET.with(|c| c.get()), Some((5, 1, 0)));
}

#[test]
fn file_put_applies_resume_offsets() {
    test_env::reset();
    let mut rflt = vec![0u8; 66];
    rflt[46..50].copy_from_slice(&100u32.to_be_bytes()); // data_pos
    rflt[62..66].copy_from_slice(&200u32.to_be_bytes()); // rsrc_pos
    let f = reply(
        false,
        &[(HTXF_REF, u32b(11)), (QUEUE, u32b(2)), (RFLT, rflt)],
    );
    unsafe { call(rcv_task_file_put, &f) };
    assert_eq!(test_env::FILE_PUT.with(|c| c.get()), Some((11, 2, 100, 200)));
}

#[test]
fn file_put_task_error_deletes() {
    test_env::reset();
    let f = reply(true, &[]);
    unsafe { call(rcv_task_file_put, &f) };
    assert!(test_env::PUT_ERROR.with(|c| c.get()));
    assert_eq!(test_env::FILE_PUT.with(|c| c.get()), None);
}

#[test]
fn folder_put_applies_ref_queue() {
    test_env::reset();
    let f = reply(false, &[(HTXF_REF, u32b(21)), (QUEUE, u32b(0))]);
    unsafe { call(rcv_task_folder_put, &f) };
    assert_eq!(test_env::FOLDER_PUT.with(|c| c.get()), Some((21, 0)));
}

#[test]
fn banner_get_forwards_ref_and_size() {
    test_env::reset();
    let f = reply(false, &[(HTXF_REF, u32b(3)), (HTXF_SIZE, u32b(8192))]);
    unsafe { call(rcv_task_banner_get, &f) };
    assert_eq!(test_env::BANNER.with(|c| c.get()), Some((3, 8192)));
}

#[test]
fn banner_get_task_error_dropped() {
    test_env::reset();
    let f = reply(true, &[]);
    unsafe { call(rcv_task_banner_get, &f) };
    assert_eq!(test_env::BANNER.with(|c| c.get()), None);
}

#[test]
fn file_getinfo_forwards_name_and_size() {
    test_env::reset();
    let f = reply(
        false,
        &[
            (FILE_NAME, b"report.txt".to_vec()),
            (FILE_SIZE, u32b(4096)),
        ],
    );
    unsafe {
        rcv_task_file_getinfo(
            std::ptr::null_mut(),
            f.as_ptr() as *const std::os::raw::c_void,
            f.len(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        )
    };
    assert_eq!(
        test_env::FILE_INFO_APPLY.with(|c| c.borrow_mut().take()),
        Some((b"report.txt".to_vec(), 4096))
    );
}

#[test]
fn file_info_forwards_name_and_size() {
    use std::ffi::CString;
    test_env::reset();
    let path = CString::new("/Files/report.txt").unwrap();
    let name = CString::new("report.txt").unwrap();
    let empty = CString::new("").unwrap();
    unsafe {
        hx_file_info_recv(
            path.as_ptr(),
            name.as_ptr(),
            empty.as_ptr(),
            empty.as_ptr(),
            empty.as_ptr(),
            empty.as_ptr(),
            empty.as_ptr(),
            4096,
        );
    }
    assert_eq!(
        test_env::FILE_INFO.with(|c| c.borrow_mut().take()),
        Some((b"report.txt".to_vec(), 4096))
    );
}
