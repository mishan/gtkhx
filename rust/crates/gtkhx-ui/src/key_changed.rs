//! What GtkHx asks when a server's key, or its commitment to the key after
//! it, is not the one pinned for it (`Proof::KeyChanged`): the device was
//! not proved and nothing was signed. Trusting the change pins it, so the
//! next connect proves there.

use std::cell::RefCell;
use std::collections::HashMap;
use std::path::PathBuf;
use std::rc::Rc;

use gtk4 as gtk;
use gtk4::{gio, glib};
use hxkeys::{Fingerprint, Pin, Pins, ServerChallenge, Warning};
use libadwaita as adw;
use libadwaita::prelude::*;

use crate::options_device_keys::plain_error;
use crate::tr::{tr, tr_argv};

/// A change the user is asked about, and the pin they are shown it against.
#[derive(Clone)]
struct Raised {
    challenge: ServerChallenge,
    warning: Warning,
    was: Option<Pin>,
}

/// The dialog asking about one host, and the change it asks about now.
struct Asking {
    dialog: adw::AlertDialog,
    raised: Rc<RefCell<Raised>>,
}

thread_local! {
    /// The hosts asked about now: a later change to one is asked about in
    /// its dialog, in place of the earlier.
    static ASKING: RefCell<HashMap<String, Asking>> = RefCell::new(HashMap::new());
}

/// Ask whether to trust `challenge`'s key for `host`, which `warning` says
/// is not `was`, the pin in `pins` it was checked against, and pin it if so,
/// unless the pin has changed since. A host already asked about is asked
/// about this change instead.
pub(crate) fn ask(
    parent: Option<&gtk::Widget>,
    pins: PathBuf,
    host: String,
    challenge: ServerChallenge,
    warning: Warning,
    was: Option<Pin>,
) -> adw::AlertDialog {
    let raised = Raised {
        challenge,
        warning,
        was,
    };
    let (heading, body, trust) = words(&host, &raised);
    let asked = ASKING.with_borrow(|a| {
        a.get(&host).map(|asking| {
            *asking.raised.borrow_mut() = raised.clone();
            asking.dialog.clone()
        })
    });
    if let Some(dialog) = asked {
        dialog.set_heading(Some(&heading));
        dialog.set_body(&body);
        dialog.set_response_label("trust", &trust);
        return dialog;
    }

    let dialog = adw::AlertDialog::new(Some(&heading), Some(&body));
    dialog.add_response("cancel", &tr("_Cancel"));
    dialog.add_response("trust", &trust);
    dialog.set_response_appearance("trust", adw::ResponseAppearance::Destructive);
    dialog.set_default_response(Some("cancel"));
    dialog.set_close_response("cancel");
    let raised = Rc::new(RefCell::new(raised));
    ASKING.with_borrow_mut(|a| {
        a.insert(
            host.clone(),
            Asking {
                dialog: dialog.clone(),
                raised: raised.clone(),
            },
        )
    });
    let anchor = parent.map(|p| p.downgrade());
    dialog.connect_response(None, move |_, response| {
        ASKING.with_borrow_mut(|a| a.remove(&host));
        if response != "trust" {
            return;
        }
        #[cfg(test)]
        tests::ACCEPTS.set(tests::ACCEPTS.get() + 1);
        let (pins, host, now) = (pins.clone(), host.clone(), raised.borrow().clone());
        let anchor = anchor.clone();
        glib::spawn_future_local(async move {
            // Off the main thread: the pins are changed under a file lock.
            let done = gio::spawn_blocking(move || {
                let none: &[Vec<u8>] = &[];
                Pins::load(pins).and_then(|mut p| {
                    p.accept(&host, now.was.as_ref(), &now.challenge, none, &now.warning)
                })
            })
            .await;
            #[cfg(test)]
            tests::DONE.set(tests::DONE.get() + 1);
            let why = match done {
                Ok(Ok(Ok(_))) => return,
                Ok(Ok(Err(_))) => tr(
                    "The server's records of its keys conflict with those kept here, so \
                     its new key cannot be trusted.",
                ),
                Ok(Err(e)) => plain_error(&e),
                Err(_) => tr("The server's key could not be changed."),
            };
            let failed = adw::AlertDialog::new(Some(&tr("Server Key Not Trusted")), Some(&why));
            failed.add_response("ok", &tr("_OK"));
            failed.present(anchor.and_then(|a| a.upgrade()).as_ref());
        });
    });
    dialog.present(parent);
    dialog
}

/// The heading, body and trusting verb for `raised` at `host`.
fn words(host: &str, raised: &Raised) -> (String, String, String) {
    let key = Fingerprint::of(&raised.challenge.server_key).to_string();
    let (heading, what, shown, trust) = match raised.warning {
        Warning::Recommitted => (
            tr("Server Key Commitment Changed"),
            tr_argv(
                // TRANSLATORS: %s is a server's address and port.
                "The server at %s has the key GtkHx pinned for it, but now names a \
                 different key to come after it. Its operator may have planned another \
                 key, or another server may be answering in its place.",
                &[host],
            ),
            vec![
                // TRANSLATORS: %s is the server key's fingerprint.
                tr_argv("Key, unchanged: %s", &[&key]),
                match raised.challenge.next {
                    // TRANSLATORS: %s is the fingerprint of the key the
                    // server says comes next.
                    Some(next) => tr_argv("Next key: %s", &[&Fingerprint(next).to_string()]),
                    None => tr("Next key: none"),
                },
            ],
            match raised.challenge.next {
                Some(_) => tr("_Trust Next Key"),
                None => tr("_Trust"),
            },
        ),
        _ => (
            tr("Server Key Changed"),
            tr_argv(
                // TRANSLATORS: %s is a server's address and port.
                "The server at %s has a different key than the one GtkHx pinned for it, \
                 and no record of handing its key over. Its operator may have replaced \
                 the key, or another server may be answering in its place.",
                &[host],
            ),
            raised
                .was
                .iter()
                .map(|pin| {
                    let pinned = Fingerprint::of(&pin.key).to_string();
                    // TRANSLATORS: %s is the pinned key's fingerprint.
                    tr_argv("Pinned key: %s", &[&pinned])
                })
                // TRANSLATORS: %s is the new key's fingerprint.
                .chain([tr_argv("New key: %s", &[&key])])
                .collect(),
            tr("_Trust New Key"),
        ),
    };
    let body = [
        what,
        tr(
            "Your device was not proved on this connection, and nothing was signed. \
             Ask the server's operator before trusting the change; once trusted, your \
             device is proved the next time you connect.",
        ),
        shown.join("\n"),
    ]
    .join("\n\n");
    (heading, body, trust)
}

/// The window a dialog for a connection goes over.
pub(crate) fn active_window() -> Option<gtk::Widget> {
    gio::Application::default()
        .and_downcast::<gtk::Application>()
        .and_then(|app| app.active_window())
        .map(|w| w.upcast())
}

#[cfg(test)]
#[path = "key_changed_tests.rs"]
pub(crate) mod tests;
