//! `hxtask` — the control channel's send primitive, `hlwrite_chunks`, and
//! the trans the next request goes out on. The session expects each reply
//! itself (`Session::expect`), so nothing here keeps a request once it is
//! sent.

pub mod send;

/// Opaque `struct htlc_conn`, only ever held as a pointer and handed back to C.
#[repr(C)]
pub struct HtlcConn {
    _private: [u8; 0],
}
