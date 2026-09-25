//! Screen sharing: consent, capture source, and the indicator.
//!
//! The video spec puts three obligations on a client that shares a
//! screen, and this module is where each is met:
//!
//! - **Consent per share, never remembered.** On Linux the choice goes
//!   through the xdg-desktop-portal ScreenCast interface, whose system
//!   picker is the consent step: every share opens a fresh portal session
//!   with persistence off, so the next share asks again. That is also the
//!   one path that works on Wayland, on X11 and inside the Flatpak sandbox
//!   alike. Elsewhere, where the platform capture element needs no picker,
//!   an explicit confirmation stands in for it.
//! - **A persistent indicator.** While anything is shared the main window
//!   shows a banner saying so, with a button that stops it. Not a toast:
//!   the characteristic failure of screen sharing is forgetting it is on.
//! - **Consent to a room.** A share ends when its publication does — a
//!   stop, a leave, the implicit leave of joining another room, a capture
//!   failure — and the portal session closes with it, so nothing carries
//!   over.
//!
//! The portal client speaks D-Bus through gio directly. The flow is four
//! calls — CreateSession, SelectSources, Start, OpenPipeWireRemote — each
//! of the first three answered asynchronously by a `Response` signal on a
//! request object whose path is predictable from the token we choose, so
//! the signal is subscribed to before the call that triggers it.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;

use gtk4 as gtk;
use gtk4::glib;
use gtk4::prelude::*;
use libadwaita as adw;
#[cfg(not(target_os = "linux"))]
use libadwaita::prelude::*;

use hxvoice_runtime::hxvoice::VideoKind;
use hxvoice_runtime::video::ScreenSource;

use crate::dock;
use crate::tr::tr;

extern "C" {
    fn hx_session_with_serial(serial: u16) -> *mut c_void;
}

thread_local! {
    /// The share each connection has open. At most one per connection:
    /// a runtime publishes one screen.
    static SHARES: RefCell<HashMap<dock::ConnKey, Option<PortalSession>>> =
        RefCell::new(HashMap::new());
    static BANNER: RefCell<Option<adw::Banner>> = const { RefCell::new(None) };
}

/// The freedesktop ScreenCast portal: D-Bus with a passed file
/// descriptor, so Linux only. Elsewhere there is no system picker and a
/// share holds no portal session.
#[cfg(target_os = "linux")]
mod portal {
    use std::cell::RefCell;
    use std::future::Future;
    use std::os::fd::AsRawFd;
    use std::pin::Pin;
    use std::rc::Rc;
    use std::task::{Context, Poll, Waker};

    use gtk4::gio;
    use gtk4::glib;
    use gtk4::prelude::*;

    use hxvoice_runtime::video::ScreenSource;

    use crate::tr::tr;

    const PORTAL_BUS: &str = "org.freedesktop.portal.Desktop";
    const PORTAL_PATH: &str = "/org/freedesktop/portal/desktop";
    const SCREENCAST: &str = "org.freedesktop.portal.ScreenCast";

    /// A live portal share: the session and the PipeWire remote it
    /// opened.
    pub(super) struct PortalSession {
        _session: SessionHandle,
        /// pipewiresrc duplicates the remote, so it is ours to keep open
        /// for the share's life and close after.
        _fd: std::os::fd::OwnedFd,
    }

    /// A portal session, closed when this is dropped. Closing it is what
    /// tells the compositor the share is over, so it is held from the
    /// moment CreateSession answers: a flow that fails or is canceled
    /// after that must not leave the compositor casting for nobody.
    struct SessionHandle {
        conn: gio::DBusConnection,
        handle: String,
    }

    impl Drop for SessionHandle {
        fn drop(&mut self) {
            self.conn.call(
                Some(PORTAL_BUS),
                &self.handle,
                "org.freedesktop.portal.Session",
                "Close",
                None,
                None,
                gio::DBusCallFlags::NONE,
                -1,
                gio::Cancellable::NONE,
                |_| {},
            );
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

    fn token() -> String {
        format!("gtkhx{}", glib::random_int())
    }

    /// Call a portal method that answers through a Request, and wait for the
    /// answer. `args` receives the handle token to put in its options.
    async fn portal_request(
        conn: &gio::DBusConnection,
        method: &str,
        args: impl FnOnce(&str) -> glib::Variant,
    ) -> Result<glib::VariantDict, String> {
        let token = token();
        let path = request_path(conn, &token);
        let slot = Rc::new(RefCell::new(Slot::default()));
        let sub = {
            let slot = Rc::clone(&slot);
            conn.subscribe_to_signal(
                Some(PORTAL_BUS),
                Some("org.freedesktop.portal.Request"),
                Some("Response"),
                Some(&path),
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
        conn.call_future(
            Some(PORTAL_BUS),
            PORTAL_PATH,
            SCREENCAST,
            method,
            Some(&args(&token)),
            None,
            gio::DBusCallFlags::NONE,
            -1,
        )
        .await
        .map_err(|e| e.to_string())?;
        let reply = Response(slot).await;
        drop(sub);
        let (code, results) = reply
            .get::<(u32, glib::VariantDict)>()
            .ok_or_else(|| "malformed portal response".to_string())?;
        match code {
            0 => Ok(results),
            1 => Err(String::new()), // the user canceled: not an error to show
            _ => Err(tr("The screen sharing request failed.")),
        }
    }

    fn options(pairs: &[(&str, glib::Variant)]) -> glib::Variant {
        let d = glib::VariantDict::new(None);
        for (k, v) in pairs {
            d.insert_value(k, v);
        }
        d.end()
    }

    fn object_path(p: &str) -> Result<glib::Variant, String> {
        glib::variant::ObjectPath::try_from(p.to_string())
            .map(|p| p.to_variant())
            .map_err(|_| "bad session handle".to_string())
    }

    /// Run the portal flow: the user picks a screen or a window, and the
    /// result is a PipeWire stream to capture from.
    pub(super) async fn portal_pick() -> Result<(PortalSession, ScreenSource), String> {
        let conn = gio::bus_get_future(gio::BusType::Session)
            .await
            .map_err(|e| e.to_string())?;

        let created = portal_request(&conn, "CreateSession", |tok| {
            (options(&[
                ("handle_token", tok.to_variant()),
                ("session_handle_token", token().to_variant()),
            ]),)
                .to_variant()
        })
        .await?;
        // The spec types the handle as a string; some portals send an
        // object path.
        let handle = created
            .lookup_value("session_handle", None)
            .and_then(|v| {
                v.get::<String>()
                    .or_else(|| v.get::<glib::variant::ObjectPath>().map(|p| p.to_string()))
            })
            .ok_or_else(|| "the portal returned no session".to_string())?;
        let guard = SessionHandle {
            conn: conn.clone(),
            handle,
        };

        // Monitors and windows (1 | 2), one at a time, cursor drawn into the
        // stream when the compositor offers it, and persist_mode 0: consent
        // is per share, never remembered.
        let session = object_path(&guard.handle)?;
        let select = |cursor: bool| {
            let session = session.clone();
            move |tok: &str| {
                let mut opts = vec![
                    ("handle_token", tok.to_variant()),
                    ("types", 3u32.to_variant()),
                    ("multiple", false.to_variant()),
                    ("persist_mode", 0u32.to_variant()),
                ];
                if cursor {
                    opts.push(("cursor_mode", 2u32.to_variant()));
                }
                glib::Variant::tuple_from_iter([session, options(&opts)])
            }
        };
        if let Err(e) = portal_request(&conn, "SelectSources", select(true)).await {
            // A cancel is final. Anything else may be a portal that doesn't
            // draw cursors into the stream and refuses the option; the share
            // is still worth having without one.
            if e.is_empty() {
                return Err(e);
            }
            portal_request(&conn, "SelectSources", select(false)).await?;
        }

        let started = portal_request(&conn, "Start", |tok| {
            glib::Variant::tuple_from_iter([
                session.clone(),
                "".to_variant(),
                options(&[("handle_token", tok.to_variant())]),
            ])
        })
        .await?;
        let streams = started
            .lookup_value("streams", None)
            .ok_or_else(|| tr("Nothing was chosen to share."))?;
        let node = streams
            .iter()
            .next()
            .and_then(|s| s.child_value(0).get::<u32>())
            .ok_or_else(|| tr("Nothing was chosen to share."))?;

        let (reply, fds) = conn
            .call_with_unix_fd_list_future(
                Some(PORTAL_BUS),
                PORTAL_PATH,
                SCREENCAST,
                "OpenPipeWireRemote",
                Some(&glib::Variant::tuple_from_iter([
                    session.clone(),
                    options(&[]),
                ])),
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
        let fd = fds
            .ok_or_else(|| "the portal returned no PipeWire remote".to_string())?
            .get(index)
            .map_err(|e| e.to_string())?;
        let raw = fd.as_raw_fd();
        Ok((
            PortalSession {
                _session: guard,
                _fd: fd,
            },
            ScreenSource::PipeWire { fd: raw, node },
        ))
    }
}

#[cfg(target_os = "linux")]
use portal::PortalSession;

/// Off Linux a share has no portal session to hold.
#[cfg(not(target_os = "linux"))]
enum PortalSession {}

// ---------------------------------------------------------------------
// Starting and ending a share.
// ---------------------------------------------------------------------

/// Ask for consent, then start publishing the screen on `sess`'s voice
/// runtime. `done(true)` once the publication has been requested,
/// `done(false)` if the user declined or it could not start.
pub(crate) fn start(sess: *mut c_void, parent: &gtk::Widget, done: impl Fn(bool) + 'static) {
    let conn = dock::key_for_session(sess);
    // Test hook: with the runtime's test source standing in for every
    // capture there is nothing to pick, and a headless run has no portal
    // to pick with.
    if std::env::var_os(hxvoice_runtime::video::TEST_SRC_ENV).is_some() {
        done(begin(conn, None, ScreenSource::Platform));
        return;
    }
    #[cfg(target_os = "linux")]
    {
        // The portal parents its own picker.
        let _ = parent;
        glib::MainContext::default().spawn_local(async move {
            match portal::portal_pick().await {
                Ok((portal, source)) => done(begin(conn, Some(portal), source)),
                Err(e) => {
                    if !e.is_empty() {
                        let msg = crate::cs(&tr1_err(&e));
                        unsafe { crate::ffi::toolbar_show_toast(msg.as_ptr()) };
                    }
                    done(false);
                }
            }
        });
    }
    #[cfg(not(target_os = "linux"))]
    {
        // No system picker: ask here, every time.
        let dialog = adw::AlertDialog::new(
            Some(&tr("Share your screen?")),
            Some(&tr(
                "Everyone in this voice chat who chooses to watch will see your whole screen.",
            )),
        );
        dialog.add_response("cancel", &tr("_Cancel"));
        dialog.add_response("share", &tr("_Share"));
        dialog.set_response_appearance("share", adw::ResponseAppearance::Suggested);
        dialog.set_default_response(Some("cancel"));
        dialog.set_close_response("cancel");
        dialog.connect_response(None, move |_, r| {
            done(r == "share" && begin(conn, None, ScreenSource::Platform));
        });
        dialog.present(Some(parent));
    }
}

#[cfg(target_os = "linux")]
fn tr1_err(e: &str) -> String {
    crate::tr::tr1("Couldn't share the screen: %s", e)
}

/// Hand the chosen source to the runtime and start the publication.
fn begin(conn: dock::ConnKey, portal: Option<PortalSession>, source: ScreenSource) -> bool {
    let sess = unsafe { hx_session_with_serial(conn) };
    let Some(rt) = (unsafe { crate::video_panel::runtime(sess) }) else {
        return false;
    };
    rt.set_screen_source(Some(source));
    SHARES.with(|s| s.borrow_mut().insert(conn, portal));
    rt.video_start(VideoKind::Screen);
    // The voice session can end while the picker is open, and then the
    // start is ignored. Nothing is shared, so hold nothing.
    if rt.video_local(VideoKind::Screen).is_none() {
        rt.set_screen_source(None);
        ended(conn);
        return false;
    }
    update_banner();
    crate::video_panel::present(sess);
    true
}

/// Stop the screen publication on `sess`, if there is one.
pub(crate) fn stop(sess: *mut c_void) {
    stop_conn(dock::key_for_session(sess));
}

/// Stop `conn`'s share. The connection may already be gone, and then
/// there is no publication left to stop, only the portal session.
fn stop_conn(conn: dock::ConnKey) {
    let sess = unsafe { hx_session_with_serial(conn) };
    if let Some(rt) = unsafe { crate::video_panel::runtime(sess) } {
        rt.video_stop(VideoKind::Screen);
    }
    ended(conn);
}

/// Drop `conn`'s share if its publication is gone. Disconnecting frees
/// the voice runtime without a word to its observers, so the view's
/// refresh on the connection change is what notices.
pub(crate) fn prune(conn: dock::ConnKey, rt: Option<&hxvoice_runtime::runtime::VoiceRuntime>) {
    if sharing(conn) && rt.is_none_or(|r| r.video_local(VideoKind::Screen).is_none()) {
        ended(conn);
    }
}

/// The publication is gone, however it went: close the portal session and
/// take the indicator down if nothing else is shared.
pub(crate) fn ended(conn: dock::ConnKey) {
    // Dropping the removed session is what closes it.
    SHARES.with(|s| s.borrow_mut().remove(&conn));
    update_banner();
}

/// Whether `conn` is sharing its screen right now.
pub(crate) fn sharing(conn: dock::ConnKey) -> bool {
    SHARES.with(|s| s.borrow().contains_key(&conn))
}

fn update_banner() {
    let any = SHARES.with(|s| !s.borrow().is_empty());
    BANNER.with(|b| {
        if let Some(banner) = b.borrow().as_ref() {
            banner.set_revealed(any);
        }
    });
}

/// The indicator: an `AdwBanner` for the main window's top bars, revealed
/// while anything is shared. Its button stops every share.
///
/// # Safety
/// GTK main thread. Transfer none: this module keeps the reference.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_screen_share_banner_new() -> *mut gtk::ffi::GtkWidget {
    crate::ensure_gtk_init();
    let banner = adw::Banner::new(&tr("You are sharing your screen"));
    banner.set_button_label(Some(&tr("Stop Sharing")));
    banner.set_revealed(false);
    banner.connect_button_clicked(|_| {
        let conns: Vec<dock::ConnKey> = SHARES.with(|s| s.borrow().keys().copied().collect());
        for conn in conns {
            stop_conn(conn);
        }
    });
    BANNER.with(|b| *b.borrow_mut() = Some(banner.clone()));
    let widget: &gtk::Widget = banner.upcast_ref();
    glib::translate::ToGlibPtr::to_glib_none(widget).0
}
