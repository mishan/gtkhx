//! A line in a connection's chat saying how its device proof ended
//! (`device-proof`).

use std::ffi::c_char;
use std::sync::Once;

use gtkhx_core::session::{
    connect_device_proof, gtkhx_session_emit_chat_log_line, gtkhx_session_get_default,
};
use hxkeys::Proof;

use crate::tr::{tr, tr1, tr_fmt};

extern "C" {
    // gtkhx.c: the tag the client's own lines carry in chat.
    static INFOPREFIX: *const c_char;
}

fn line(p: &Proof) -> String {
    match p {
        Proof::Proved { identity, name, .. } => tr_fmt(
            "device proved as %1$s (%2$s)",
            &[name.as_str(), identity.short().as_str()],
        ),
        Proof::Refused {
            reason: Some(reason),
        } => tr1("device not proved: %s", reason),
        Proof::Refused { reason: None } => tr("device not proved: the server refused it"),
        Proof::KeyChanged { host, .. } => tr1(
            "device not proved: the server key for %s is not the one first seen there",
            host,
        ),
        Proof::NotProved { why } => tr1("device not proved: %s", why),
        Proof::TimedOut => tr("device not proved: the server did not answer in time"),
    }
}

/// Tell each connection's chat how its proof ended. Once.
pub(crate) fn install() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        connect_device_proof(|htlc, p| {
            let line = crate::cs(&(line(p) + "\n"));
            // SAFETY: the signal names a live connection, on the main thread.
            unsafe {
                gtkhx_session_emit_chat_log_line(
                    gtkhx_session_get_default(),
                    htlc,
                    0,
                    INFOPREFIX,
                    crate::broadcast::INFO_COLOR,
                    line.as_ptr(),
                )
            };
        });
    });
}
