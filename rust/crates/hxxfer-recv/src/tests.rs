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
