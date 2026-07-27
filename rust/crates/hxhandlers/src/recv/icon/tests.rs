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
