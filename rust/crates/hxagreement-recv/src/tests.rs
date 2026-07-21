//! Headless tests for the agreement receive decision, driven through the
//! `test_env` recording double for the agreement-emit C ABI.

use super::*;
use std::ffi::CString;

fn recv(has_agreement: bool, body: &str) -> c_int {
    let cbuf = CString::new(body).unwrap();
    unsafe {
        hx_agreement_recv(
            std::ptr::null_mut(),
            c_int::from(has_agreement),
            cbuf.as_ptr(),
            body.len() as u16,
        )
    }
}

#[test]
fn real_agreement_is_shown() {
    test_env::reset();
    assert_eq!(recv(/*has_agreement=*/ true, "Be nice."), HX_AGREEMENT_ACT_SHOWN);
    assert_eq!(
        test_env::EMITTED.with(|c| c.take()),
        Some((b"Be nice.".to_vec(), 8))
    );
}

#[test]
fn no_agreement_auto_agrees_without_emit() {
    // No-agreement / malformed → nothing to show; the C side sends AGREEMENTAGREE.
    test_env::reset();
    assert_eq!(recv(/*has_agreement=*/ false, ""), HX_AGREEMENT_ACT_AUTO_AGREE);
    assert_eq!(test_env::EMITTED.with(|c| c.take()), None);
}
