//! How a connection's device proof ended (`device-proof`): a line in its
//! chat, the header's badge, and for a server key that changed, whether to
//! trust it.

use std::ffi::{c_char, c_void};
use std::sync::Once;

use gtkhx_core::session::{
    connect_device_proof, gtkhx_session_emit_chat_log_line, gtkhx_session_get_default,
};
use hxkeys::{Proof, Warning};

use crate::options_device_keys::{pins_beside, store_root};
use crate::tr::{tr, tr_argv};

extern "C" {
    // gtkhx.c: the tag the client's own lines carry in chat.
    static INFOPREFIX: *const c_char;
}

/// The chat's line for how a proof ended, on one row: the server's reason
/// and the card's name are put in as they came, line breaks aside.
fn line(p: &Proof) -> String {
    let text = match p {
        Proof::Proved { identity, name, .. } => tr_argv(
            "device proved as %1$s (%2$s)",
            &[name.as_str(), identity.short().as_str()],
        ),
        Proof::Refused {
            reason: Some(reason),
        } => tr_argv("device not proved: %s", &[reason]),
        Proof::Refused { reason: None } => tr("device not proved: the server refused it"),
        Proof::KeyChanged {
            host,
            warning: Warning::Recommitted,
            ..
        } => tr_argv(
            "device not proved: the server at %s names another next key than the one pinned for it",
            &[host],
        ),
        Proof::KeyChanged { host, .. } => tr_argv(
            "device not proved: the server key for %s is not the one pinned for it",
            &[host],
        ),
        Proof::NotProved { why } => tr_argv("device not proved: %s", &[why]),
        Proof::TimedOut => tr("device not proved: no answer came before the deadline"),
    };
    text.replace("\r\n", " ").replace(['\r', '\n'], " ")
}

/// Put `text` in `htlc`'s chat, as the client's own line.
pub(crate) fn say(htlc: *mut c_void, text: &str) {
    let line = crate::cs(&(text.to_owned() + "\n"));
    // SAFETY: called on the main thread for a live connection.
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
}

/// Tell each connection's chat how its proof ended. Once.
pub(crate) fn install() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        connect_device_proof(|htlc, p| {
            say(htlc, &line(p));
            crate::encryption_lock::proof_told(htlc);
            if let Proof::KeyChanged {
                host,
                warning,
                challenge,
            } = p
            {
                crate::key_changed::ask(
                    crate::key_changed::active_window().as_ref(),
                    pins_beside(&store_root()),
                    host.clone(),
                    (**challenge).clone(),
                    warning.clone(),
                );
            }
        });
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use hxkeys::Fingerprint;

    /// What the server and the card say is shown as they say it, on one
    /// row.
    #[test]
    fn a_line_shows_what_it_was_told_as_told_on_one_row() {
        let fp = Fingerprint([7; 32]);
        let proved = line(&Proof::Proved {
            identity: fp,
            device: fp,
            name: "Alice %2$s %s".into(),
        });
        assert!(proved.contains("Alice %2$s %s"), "{proved}");
        assert!(proved.contains(&fp.short()), "{proved}");
        let refused = line(&Proof::Refused {
            reason: Some("one\rtwo\nthree\r\nfour".into()),
        });
        assert!(!refused.contains(['\r', '\n']), "{refused:?}");
        assert!(refused.contains("one two three four"), "{refused:?}");
    }
}
