//! The xdg-desktop-portal request plumbing that screen sharing and the
//! camera share.
//!
//! Most portal methods answer twice: the call returns a Request object at
//! once, and the real answer arrives later as that object's `Response`
//! signal, after whatever dialog the portal shows. The request's path is
//! predictable from a token we choose, so the signal is subscribed to
//! before the call that triggers it. D-Bus with passed file descriptors,
//! so Linux only.

use std::cell::RefCell;
use std::future::Future;
use std::os::fd::OwnedFd;
use std::pin::Pin;
use std::rc::Rc;
use std::task::{Context, Poll, Waker};
use std::time::Duration;

use gtk4::gio;
use gtk4::glib;
use gtk4::prelude::*;

pub(crate) const PORTAL_BUS: &str = "org.freedesktop.portal.Desktop";
pub(crate) const PORTAL_PATH: &str = "/org/freedesktop/portal/desktop";

/// How long a request that shows nothing may take to answer.
pub(crate) const QUICK: Duration = Duration::from_secs(30);
/// How long one that shows a dialog may take: long enough to choose in,
/// short enough that a backend that died doesn't leave the caller waiting
/// for good.
pub(crate) const DIALOG: Duration = Duration::from_secs(180);

/// Why a portal request didn't answer with results.
pub(crate) enum RequestError {
    /// The user dismissed the dialog: not an error to show.
    Canceled,
    /// The portal answered with a failure.
    Refused,
    /// No answer in time. The request has been closed.
    TimedOut,
    /// Anything else, already worded for the user.
    Other(String),
}

impl RequestError {
    /// The message to show, in the caller's words for a refusal and a
    /// timeout; empty for a cancel, which shows none.
    pub(crate) fn message(
        self,
        refused: impl FnOnce() -> String,
        timed_out: impl FnOnce() -> String,
    ) -> String {
        match self {
            RequestError::Canceled => String::new(),
            RequestError::Refused => refused(),
            RequestError::TimedOut => timed_out(),
            RequestError::Other(m) => m,
        }
    }
}

// ---------------------------------------------------------------------
// A single-shot future fed by a D-Bus signal.
// ---------------------------------------------------------------------

#[derive(Default)]
struct Slot {
    value: Option<glib::Variant>,
    waker: Option<Waker>,
}

struct Response(Rc<RefCell<Slot>>);

impl Future for Response {
    type Output = glib::Variant;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<glib::Variant> {
        let mut slot = self.0.borrow_mut();
        match slot.value.take() {
            Some(v) => Poll::Ready(v),
            None => {
                slot.waker = Some(cx.waker().clone());
                Poll::Pending
            }
        }
    }
}

/// The request object path the portal will answer on for `token`.
fn request_path(conn: &gio::DBusConnection, token: &str) -> String {
    let sender = conn
        .unique_name()
        .map(|n| n.trim_start_matches(':').replace('.', "_"))
        .unwrap_or_default();
    format!("{PORTAL_PATH}/request/{sender}/{token}")
}

/// A fresh handle token.
pub(crate) fn token() -> String {
    format!("gtkhx{}", glib::random_int())
}

/// Call `interface.method` on the portal, which answers through a
/// Request, and wait up to `wait` for the answer. `args` receives the
/// handle token to put in its options.
pub(crate) async fn request(
    conn: &gio::DBusConnection,
    interface: &str,
    method: &str,
    wait: Duration,
    args: impl FnOnce(&str) -> glib::Variant,
) -> Result<glib::VariantDict, RequestError> {
    let token = token();
    let path = request_path(conn, &token);
    let slot = Rc::new(RefCell::new(Slot::default()));
    let subscribe = |path: &str| {
        let slot = Rc::clone(&slot);
        conn.subscribe_to_signal(
            Some(PORTAL_BUS),
            Some("org.freedesktop.portal.Request"),
            Some("Response"),
            Some(path),
            None,
            gio::DBusSignalFlags::NONE,
            move |sig| {
                let mut s = slot.borrow_mut();
                s.value = Some(sig.parameters.clone());
                if let Some(w) = s.waker.take() {
                    w.wake();
                }
            },
        )
    };
    let _sub = subscribe(&path);
    let reply = conn
        .call_future(
            Some(PORTAL_BUS),
            PORTAL_PATH,
            interface,
            method,
            Some(&args(&token)),
            None,
            gio::DBusCallFlags::NONE,
            -1,
        )
        .await
        .map_err(|e| RequestError::Other(e.to_string()))?;
    // A portal older than 0.9 doesn't build the request path from the
    // token, and answers on the one it returns instead. The answer can
    // beat this subscription there; the timeout below covers that.
    let request = reply
        .child_value(0)
        .get::<glib::variant::ObjectPath>()
        .map(|p| p.to_string())
        .unwrap_or_else(|| path.clone());
    let _late_sub = (request != path).then(|| subscribe(&request));
    let Ok(reply) = glib::future_with_timeout(wait, Response(slot)).await else {
        // Closing the request takes down a dialog that is still up, so a
        // choice made after this can't act for a caller that has given up.
        conn.call(
            Some(PORTAL_BUS),
            &request,
            "org.freedesktop.portal.Request",
            "Close",
            None,
            None,
            gio::DBusCallFlags::NONE,
            -1,
            gio::Cancellable::NONE,
            |_| {},
        );
        return Err(RequestError::TimedOut);
    };
    let (code, results) = reply
        .get::<(u32, glib::VariantDict)>()
        .ok_or_else(|| RequestError::Other("malformed portal response".to_string()))?;
    match code {
        0 => Ok(results),
        1 => Err(RequestError::Canceled),
        _ => Err(RequestError::Refused),
    }
}

/// An `a{sv}` of `pairs`.
pub(crate) fn options(pairs: &[(&str, glib::Variant)]) -> glib::Variant {
    let d = glib::VariantDict::new(None);
    for (k, v) in pairs {
        d.insert_value(k, v);
    }
    d.end()
}

/// Call `interface.OpenPipeWireRemote` with `args` and take the remote it
/// passes back.
pub(crate) async fn open_pipewire_remote(
    conn: &gio::DBusConnection,
    interface: &str,
    args: glib::Variant,
) -> Result<OwnedFd, String> {
    let (reply, fds) = conn
        .call_with_unix_fd_list_future(
            Some(PORTAL_BUS),
            PORTAL_PATH,
            interface,
            "OpenPipeWireRemote",
            Some(&args),
            None,
            gio::DBusCallFlags::NONE,
            -1,
            None::<&gio::UnixFDList>,
        )
        .await
        .map_err(|e| e.to_string())?;
    let index = reply
        .child_value(0)
        .get::<glib::variant::Handle>()
        .map(|h| h.0)
        .ok_or_else(|| "the portal returned no PipeWire remote".to_string())?;
    fds.ok_or_else(|| "the portal returned no PipeWire remote".to_string())?
        .get(index)
        .map_err(|e| e.to_string())
}

/// Read a property of the portal's `interface`.
pub(crate) async fn property(
    conn: &gio::DBusConnection,
    interface: &str,
    name: &str,
) -> Option<glib::Variant> {
    let reply = conn
        .call_future(
            Some(PORTAL_BUS),
            PORTAL_PATH,
            "org.freedesktop.DBus.Properties",
            "Get",
            Some(&(interface, name).to_variant()),
            None,
            gio::DBusCallFlags::NONE,
            -1,
        )
        .await
        .ok()?;
    reply.child_value(0).as_variant()
}
