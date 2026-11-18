//! How the connect's device proof ended (`hxnet::Event::DeviceProof`),
//! handed to the view.

use std::os::raw::c_void;

use gtkhx_core::session::{gtkhx_session_emit_device_proof, gtkhx_session_get_default};

/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn proof(htlc: *mut c_void, proof: &hxkeys::Proof) {
    gtkhx_session_emit_device_proof(gtkhx_session_get_default(), htlc, proof);
}
