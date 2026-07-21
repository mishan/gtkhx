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
