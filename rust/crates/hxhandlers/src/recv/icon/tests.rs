//! Headless signal-behaviour tests for the icon-change handler, driven through
//! the `test_env` doubles for the parse / emit C ABIs.

use super::*;

fn recv() {
    // buf/len/htlc are opaque to the doubles; the parse result is driven by
    // test_env::PARSE_UID.
    unsafe { hx_icon_change_recv(std::ptr::null_mut(), std::ptr::null(), 0) };
}

#[test]
fn emits_gif_icon_changed_with_parsed_uid() {
    test_env::reset();
    test_env::PARSE_UID.with(|c| c.set(Some(4242)));

    recv();

    assert_eq!(test_env::EMITTED.with(|c| c.take()), Some(4242));
}

#[test]
fn drops_frame_with_no_uid() {
    test_env::reset();
    test_env::PARSE_UID.with(|c| c.set(None)); // malformed / uid absent

    recv();

    assert_eq!(test_env::EMITTED.with(|c| c.take()), None);
}

/// A real, GIF-signed payload forwards verbatim (non-null ptr, same len).
#[test]
fn valid_gif_forwards_bytes() {
    test_env::reset();
    test_env::IS_GIF.with(|c| c.set(true));
    let bytes = [0u8; 16];
    unsafe { hx_icon_data_recv(std::ptr::null_mut(), 7, bytes.as_ptr(), 16) };
    assert_eq!(
        test_env::DATA_EMITTED.with(|c| c.take()),
        Some((7, /*ptr_is_null=*/ false, 16))
    );
}

/// A zero-length payload is a cleared avatar → forward (NULL, 0), never
/// consulting the signature check.
#[test]
fn empty_payload_forwards_cleared() {
    test_env::reset();
    unsafe { hx_icon_data_recv(std::ptr::null_mut(), 7, std::ptr::null(), 0) };
    assert_eq!(
        test_env::DATA_EMITTED.with(|c| c.take()),
        Some((7, /*ptr_is_null=*/ true, 0))
    );
}

/// A non-empty payload failing the GIF signature is coerced to cleared.
#[test]
fn non_gif_payload_coerced_to_cleared() {
    test_env::reset();
    test_env::IS_GIF.with(|c| c.set(false));
    let bytes = [0xFFu8; 8];
    unsafe { hx_icon_data_recv(std::ptr::null_mut(), 9, bytes.as_ptr(), 8) };
    assert_eq!(
        test_env::DATA_EMITTED.with(|c| c.take()),
        Some((9, /*ptr_is_null=*/ true, 0))
    );
}

// ---- ICON_GET / ICON_GETLIST reply handlers --------------------------------

use hotline_proto::messages::{tag, ServerHdr};
use std::os::raw::c_void;

const GIF87: &[u8] = b"GIF87a\x00\x00";

/// Build a 22-byte transaction header (`flag & 1` = task-error) followed by the
/// concatenated TLV chunks — the shape `ChunkIter::over_message` expects.
fn reply(error: bool, chunks: &[(u16, Vec<u8>)]) -> Vec<u8> {
    let mut v = Vec::new();
    v.extend_from_slice(&(ServerHdr::Task as u32).to_be_bytes()); // type
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

/// Pack one `ICON_LIST` entry body: u16 uid + u16 gif_len + gif bytes.
fn list_entry(uid: u16, gif: &[u8]) -> Vec<u8> {
    let mut v = uid.to_be_bytes().to_vec();
    v.extend_from_slice(&(gif.len() as u16).to_be_bytes());
    v.extend_from_slice(gif);
    v
}

unsafe fn call_frame(
    f: unsafe extern "C" fn(*mut c_void, *const c_void, usize, *mut c_void, *mut c_void),
    frame: &[u8],
) {
    f(
        std::ptr::null_mut(),
        frame.as_ptr() as *const c_void,
        frame.len(),
        std::ptr::null_mut(),
        std::ptr::null_mut(),
    );
}

/// A well-formed ICON_GET reply flips state to SUPPORTED and publishes the avatar.
#[test]
fn icon_get_flips_supported_and_emits() {
    test_env::reset();
    let frame = reply(
        false,
        &[
            (tag::UID, 7u16.to_be_bytes().to_vec()),
            (tag::ICON_GIF, GIF87.to_vec()),
        ],
    );
    unsafe { call_frame(rcv_task_icon_get, &frame) };
    assert_eq!(test_env::STATE.with(|c| c.get()), 1 /* SUPPORTED */);
    assert_eq!(
        test_env::DATA_EMITTED.with(|c| c.take()),
        Some((7, /*ptr_is_null=*/ false, GIF87.len() as u32))
    );
}

/// An ICON_GET reply with no UID is dropped — no state change, no emit.
#[test]
fn icon_get_missing_uid_drops() {
    test_env::reset();
    let frame = reply(false, &[(tag::ICON_GIF, GIF87.to_vec())]);
    unsafe { call_frame(rcv_task_icon_get, &frame) };
    assert_eq!(test_env::STATE.with(|c| c.get()), 0 /* untouched */);
    assert_eq!(test_env::DATA_EMITTED.with(|c| c.take()), None);
}

/// A task-error ICON_GETLIST is the "unsupported" verdict: state UNSUPPORTED,
/// watchdog disarmed, no saved-avatar push, no emits.
#[test]
fn icon_getlist_error_marks_unsupported() {
    test_env::reset();
    test_env::PROBE_TIMER.with(|c| c.set(42));
    let frame = reply(true, &[]);
    unsafe { call_frame(rcv_task_icon_getlist, &frame) };
    assert_eq!(test_env::STATE.with(|c| c.get()), 2 /* UNSUPPORTED */);
    assert_eq!(test_env::SOURCE_REMOVED.with(|c| c.get()), Some(42));
    assert_eq!(test_env::PROBE_TIMER.with(|c| c.get()), 0);
    assert!(!test_env::SEND_SAVED.with(|c| c.get()));
    assert_eq!(test_env::DATA_COUNT.with(|c| c.get()), 0);
}

/// A successful ICON_GETLIST flips SUPPORTED, disarms the watchdog, pushes the
/// saved avatar, and publishes every listed entry.
#[test]
fn icon_getlist_success_publishes_entries() {
    test_env::reset();
    test_env::PROBE_TIMER.with(|c| c.set(99));
    let frame = reply(
        false,
        &[
            (tag::ICON_LIST, list_entry(1, GIF87)),
            (tag::ICON_LIST, list_entry(2, GIF87)),
            (tag::ICON_LIST, list_entry(3, &[])), // cleared avatar
        ],
    );
    unsafe { call_frame(rcv_task_icon_getlist, &frame) };
    assert_eq!(test_env::STATE.with(|c| c.get()), 1 /* SUPPORTED */);
    assert_eq!(test_env::SOURCE_REMOVED.with(|c| c.get()), Some(99));
    assert_eq!(test_env::PROBE_TIMER.with(|c| c.get()), 0);
    assert!(test_env::SEND_SAVED.with(|c| c.get()));
    assert_eq!(test_env::DATA_COUNT.with(|c| c.get()), 3);
}
