//! Headless tests for the file-transfer receive handlers, driven through the
//! `test_env` recording doubles (a fake htxf + outcome flags).

use super::*;

/// A sentinel htxf pointer (never dereferenced by the crate).
fn fake_htxf() -> *mut std::os::raw::c_void {
    0xF11E_usize as *mut std::os::raw::c_void
}

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
/// concatenated TLV chunks — the shape `ChunkIter::over_message` expects.
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

type Handler = unsafe extern "C" fn(
    *mut std::os::raw::c_void,
    *const std::os::raw::c_void,
    usize,
    *mut std::os::raw::c_void,
    *mut std::os::raw::c_void,
);

/// Invoke a handler with a frame + the sentinel htxf as `ptr`.
unsafe fn call(f: Handler, frame: &[u8]) {
    f(
        std::ptr::null_mut(),
        frame.as_ptr() as *const std::os::raw::c_void,
        frame.len(),
        fake_htxf(),
        std::ptr::null_mut(),
    );
}

fn htxf() -> test_env::FakeHtxf {
    test_env::HTXF.with(|c| c.borrow().clone())
}

// ---- shared announce tail --------------------------------------------------

#[test]
fn ready_transfer_announces_and_starts() {
    test_env::reset();
    unsafe { hx_xfer_announce(std::ptr::null_mut(), fake_htxf(), 0) };
    assert!(test_env::ANNOUNCED.with(|c| c.get()));
    assert!(test_env::STARTED.with(|c| c.get()));
}

#[test]
fn queued_transfer_announces_only() {
    test_env::reset();
    unsafe { hx_xfer_announce(std::ptr::null_mut(), fake_htxf(), 3) };
    assert!(test_env::ANNOUNCED.with(|c| c.get()));
    assert!(!test_env::STARTED.with(|c| c.get()));
}

// ---- file_get --------------------------------------------------------------

#[test]
fn file_get_ready_stamps_and_announces() {
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
    let h = htxf();
    assert_eq!(h.ref_, 7);
    assert_eq!(h.total_size, 4096);
    assert_eq!(h.queue, 0);
    assert!(h.start_stamped);
    assert_eq!(h.serverhost, b"server.example");
    assert_eq!(h.serverport, 5501); // subchannel = control port + 1
    assert!(test_env::ANNOUNCED.with(|c| c.get()));
    assert!(test_env::STARTED.with(|c| c.get()));
}

#[test]
fn file_get_prefers_size64() {
    test_env::reset();
    let big: u64 = 0x1_0000_0000; // 4 GiB — doesn't fit the legacy u32
    let f = reply(
        false,
        &[
            (HTXF_REF, u32b(9)),
            (HTXF_SIZE, u32b(1)),
            (XFERSIZE64, big.to_be_bytes().to_vec()),
        ],
    );
    unsafe { call(rcv_task_file_get, &f) };
    assert_eq!(htxf().total_size, big);
}

#[test]
fn file_get_cancelled_transfer_dropped() {
    test_env::reset();
    test_env::IN_LIST.with(|c| c.set(0));
    let f = reply(false, &[(HTXF_REF, u32b(7)), (HTXF_SIZE, u32b(4096))]);
    unsafe { call(rcv_task_file_get, &f) };
    assert_eq!(htxf().ref_, 0);
    assert!(!test_env::ANNOUNCED.with(|c| c.get()));
}

#[test]
fn file_get_task_error_retries_when_opted() {
    test_env::reset();
    test_env::OPT_RETRY.with(|c| c.set(1));
    unsafe { call(rcv_task_file_get, &reply(true, &[])) };
    assert!(test_env::RETRY_TIMER.with(|c| c.get()));
    assert_eq!(htxf().gone, Some(0));
    assert!(!test_env::XFER_DELETED.with(|c| c.get()));
}

#[test]
fn file_get_task_error_deletes_without_retry() {
    test_env::reset();
    unsafe { call(rcv_task_file_get, &reply(true, &[])) };
    assert!(test_env::GTASK_DELETED.with(|c| c.get()));
    assert!(test_env::XFER_DELETED.with(|c| c.get()));
    assert!(!test_env::RETRY_TIMER.with(|c| c.get()));
}

#[test]
fn file_get_malformed_no_ref_gated() {
    test_env::reset();
    unsafe { call(rcv_task_file_get, &reply(false, &[(HTXF_SIZE, u32b(4096))])) };
    assert_eq!(htxf().ref_, 0);
    assert!(!test_env::ANNOUNCED.with(|c| c.get()));
}

#[test]
fn file_get_builds_preview_when_opted() {
    test_env::reset();
    test_env::OPT_PREVIEW.with(|c| c.set(1)); // preview requested, none yet
    let f = reply(false, &[(HTXF_REF, u32b(7)), (HTXF_SIZE, u32b(10))]);
    unsafe { call(rcv_task_file_get, &f) };
    assert!(test_env::PREVIEW_BUILT.with(|c| c.get()));
    assert_eq!(htxf().preview, 0xB0);
}

// ---- folder_get ------------------------------------------------------------

#[test]
fn folder_get_zero_size_clamps_to_one() {
    test_env::reset();
    let f = reply(false, &[(HTXF_REF, u32b(5)), (FILE_NFILES, u32b(3))]);
    unsafe { call(rcv_task_folder_get, &f) };
    let h = htxf();
    assert_eq!(h.ref_, 5);
    assert_eq!(h.total_size, 1);
}

// ---- file_put --------------------------------------------------------------

#[test]
fn file_put_sizes_upload_from_fs_probes() {
    test_env::reset();
    test_env::STAT_SIZE.with(|c| c.set(1000)); // data-fork size
    test_env::RSRC_LEN.with(|c| c.set(50));
    test_env::COMMENT_LEN.with(|c| c.set(10));
    let mut rflt = vec![0u8; 66];
    rflt[46..50].copy_from_slice(&100u32.to_be_bytes()); // data_pos
    rflt[62..66].copy_from_slice(&20u32.to_be_bytes()); // rsrc_pos
    let f = reply(
        false,
        &[(HTXF_REF, u32b(11)), (QUEUE, u32b(2)), (RFLT, rflt)],
    );
    unsafe { call(rcv_task_file_put, &f) };
    let h = htxf();
    assert_eq!(h.ref_, 11);
    assert_eq!(h.queue, 2);
    assert_eq!(h.data_pos, 100);
    assert_eq!(h.rsrc_pos, 20);
    assert_eq!(h.data_size, 1000);
    assert_eq!(h.rsrc_size, 50);
    // 133 + 16 (rsrc remaining) + 10 (comment) + 900 (data remaining) + 30 (rsrc remaining)
    assert_eq!(h.total_size, 133 + 16 + 10 + 900 + 30);
}

#[test]
fn file_put_task_error_deletes() {
    test_env::reset();
    unsafe { call(rcv_task_file_put, &reply(true, &[])) };
    assert!(test_env::GTASK_DELETED.with(|c| c.get()));
    assert!(test_env::XFER_DELETED.with(|c| c.get()));
}

// ---- folder_put ------------------------------------------------------------

#[test]
fn folder_put_stamps_ref_and_queue() {
    test_env::reset();
    let f = reply(false, &[(HTXF_REF, u32b(21)), (QUEUE, u32b(0))]);
    unsafe { call(rcv_task_folder_put, &f) };
    let h = htxf();
    assert_eq!(h.ref_, 21);
    assert_eq!(h.queue, 0);
    assert!(test_env::ANNOUNCED.with(|c| c.get()));
}

// ---- banner_get ------------------------------------------------------------

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
    unsafe { call(rcv_task_banner_get, &reply(true, &[])) };
    assert_eq!(test_env::BANNER.with(|c| c.get()), None);
}

// ---- file_getinfo ----------------------------------------------------------

#[test]
fn file_getinfo_emits_name_and_size() {
    test_env::reset();
    let f = reply(
        false,
        &[(FILE_NAME, b"report.txt".to_vec()), (FILE_SIZE, u32b(4096))],
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
        test_env::FILE_INFO.with(|c| c.borrow_mut().take()),
        Some((b"report.txt".to_vec(), 4096))
    );
    // Success: the label transfers to the file-info window — the handler must
    // NOT free it (that would double-free with close_file_info).
    assert_eq!(test_env::FREED.with(|c| c.get()), None);
}

#[test]
fn file_getinfo_task_error_frees_label() {
    test_env::reset();
    // A sentinel standing in for the g_strdup'd path label held as the task ptr.
    let label = 0x1_abe1_usize as *mut std::os::raw::c_void;
    let f = reply(true, &[]);
    unsafe {
        rcv_task_file_getinfo(
            std::ptr::null_mut(),
            f.as_ptr() as *const std::os::raw::c_void,
            f.len(),
            label,
            std::ptr::null_mut(),
        )
    };
    // No dialog opens on a task error, so the handler frees the label itself.
    assert_eq!(test_env::FREED.with(|c| c.get()), Some(label));
    assert!(test_env::FILE_INFO.with(|c| c.borrow().is_none()));
}
