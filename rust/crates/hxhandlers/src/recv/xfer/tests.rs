//! Headless tests for the file-transfer receive handlers, driven through the
//! `test_env` recording doubles (a fake htxf + outcome flags).

use super::*;

/// A sentinel htxf pointer (never dereferenced by the crate).
fn fake_htxf() -> *mut std::os::raw::c_void {
    0xF11E_usize as *mut std::os::raw::c_void
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

// ---- downloads and uploads ------------------------------------------------

fn transfer(reference: u32, size: u64, queue: u32) -> Transfer {
    Transfer {
        reference,
        size,
        queue,
        ..Transfer::default()
    }
}

#[test]
fn a_download_stamps_and_starts() {
    test_env::reset();
    unsafe {
        download_ready(
            std::ptr::null_mut(),
            fake_htxf(),
            false,
            &transfer(7, 4096, 0),
        )
    };
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

/// What starts nothing: a cancelled transfer, no reference, and a file
/// download with no size. A folder may be empty.
#[test]
fn a_download_reply_that_cannot_start_is_dropped() {
    for (in_list, folder, t) in [
        (0, false, transfer(7, 4096, 0)),
        (1, false, transfer(0, 4096, 0)),
        (1, false, transfer(7, 0, 0)),
        (1, true, transfer(0, 9, 0)),
    ] {
        test_env::reset();
        test_env::IN_LIST.with(|c| c.set(in_list));
        unsafe { download_ready(std::ptr::null_mut(), fake_htxf(), folder, &t) };
        assert_eq!(htxf().ref_, 0);
        assert!(!test_env::ANNOUNCED.with(|c| c.get()));
    }
}

#[test]
fn an_empty_folder_download_counts_one_byte() {
    test_env::reset();
    unsafe { download_ready(std::ptr::null_mut(), fake_htxf(), true, &transfer(5, 0, 0)) };
    let h = htxf();
    assert_eq!(h.ref_, 5);
    assert_eq!(h.total_size, 1);
}

#[test]
fn a_refused_download_retries_when_asked() {
    test_env::reset();
    test_env::OPT_RETRY.with(|c| c.set(1));
    unsafe { download_refused(std::ptr::null_mut(), fake_htxf()) };
    assert!(test_env::RETRY_TIMER.with(|c| c.get()));
    assert_eq!(htxf().gone, Some(0));
    assert!(!test_env::XFER_DELETED.with(|c| c.get()));
}

#[test]
fn a_refused_transfer_is_deleted() {
    for upload in [false, true] {
        test_env::reset();
        unsafe {
            if upload {
                upload_refused(std::ptr::null_mut(), fake_htxf())
            } else {
                download_refused(std::ptr::null_mut(), fake_htxf())
            }
        };
        assert!(test_env::GTASK_DELETED.with(|c| c.get()));
        assert!(test_env::XFER_DELETED.with(|c| c.get()));
        assert!(!test_env::RETRY_TIMER.with(|c| c.get()));
    }
}

#[test]
fn a_download_builds_its_preview_when_asked() {
    test_env::reset();
    test_env::OPT_PREVIEW.with(|c| c.set(1)); // preview requested, none yet
    unsafe {
        download_ready(
            std::ptr::null_mut(),
            fake_htxf(),
            false,
            &transfer(7, 10, 0),
        )
    };
    assert!(test_env::PREVIEW_BUILT.with(|c| c.get()));
    assert_eq!(htxf().preview, 0xB0);
}

#[test]
fn a_file_upload_is_sized_from_the_disk_and_where_it_resumes() {
    test_env::reset();
    test_env::STAT_SIZE.with(|c| c.set(1000)); // data-fork size
    test_env::RSRC_LEN.with(|c| c.set(50));
    test_env::COMMENT_LEN.with(|c| c.set(10));
    let t = Transfer {
        data_from: 100,
        rsrc_from: 20,
        ..transfer(11, 0, 2)
    };
    unsafe { upload_ready(std::ptr::null_mut(), fake_htxf(), false, &t) };
    let h = htxf();
    assert_eq!(h.ref_, 11);
    assert_eq!(h.queue, 2);
    assert_eq!(h.data_pos, 100);
    assert_eq!(h.rsrc_pos, 20);
    assert_eq!(h.data_size, 1000);
    assert_eq!(h.rsrc_size, 50);
    // 133 + 16 (rsrc remaining) + 10 (comment) + 900 (data remaining) + 30 (rsrc remaining)
    assert_eq!(h.total_size, 133 + 16 + 10 + 900 + 30);
    assert!(!test_env::STARTED.with(|c| c.get()));
}

#[test]
fn a_folder_upload_stamps_ref_and_queue() {
    test_env::reset();
    unsafe { upload_ready(std::ptr::null_mut(), fake_htxf(), true, &transfer(21, 0, 0)) };
    let h = htxf();
    assert_eq!(h.ref_, 21);
    assert_eq!(h.queue, 0);
    assert_eq!(h.total_size, 0);
    assert!(test_env::STARTED.with(|c| c.get()));
}

// ---- banner ----------------------------------------------------------------

#[test]
fn the_banner_goes_to_its_fetch_with_its_size_clamped_to_32_bits() {
    for (size, want) in [(8192, 8192), (u64::from(u32::MAX) + 1, u32::MAX)] {
        test_env::reset();
        let t = Transfer {
            reference: 3,
            size,
            ..Transfer::default()
        };
        unsafe { banner(std::ptr::null_mut(), &t) };
        assert_eq!(test_env::BANNER.with(|c| c.get()), Some((3, want)));
    }
}

// ---- file_info -------------------------------------------------------------

#[test]
fn file_info_opens_the_dialog_for_the_label() {
    test_env::reset();
    let info = FileInfo {
        name: "caf\u{e9}".into(),
        kind: "TEXT".into(),
        creator: "ttxt".into(),
        comment: String::new(),
        size: 4096,
        created: [0; 8],
        modified: [0; 8],
    };
    unsafe { file_info(std::ptr::null_mut(), c"/pub/caf\x8e", &info) };
    assert_eq!(
        test_env::FILE_INFO.with(|c| c.borrow_mut().take()),
        Some((
            b"/pub/caf\x8e".to_vec(),
            "caf\u{e9}".as_bytes().to_vec(),
            4096
        ))
    );
}
