//! Cameras inside the Flatpak sandbox, through the Camera portal.
//!
//! The sandbox has no `/dev/video*`, so the device monitor finds nothing
//! there. The Camera portal is the way in: `AccessCamera` asks the user
//! (once; the portal's permission store remembers the answer), and
//! `OpenPipeWireRemote` then hands over a PipeWire connection that shows
//! the cameras and nothing else. The runtime keeps that remote and captures
//! through it with `pipewiresrc`.
//!
//! Access is asked for the first time a camera is turned on, never earlier:
//! a permission dialog at startup, or from the settings page, is a question
//! the user hasn't given any reason to be asked.
//!
//! Outside the sandbox none of this runs, and the camera path is exactly
//! what it was. `GTKHX_CAMERA_PORTAL` forces the portal path on a host
//! session, for testing it.

use hxvoice_runtime::video;

/// Whether turning the camera on has to go through the portal first.
pub(crate) fn needed() -> bool {
    video::camera_via_portal() && !video::camera_remote_open()
}

#[cfg(target_os = "linux")]
mod imp {
    use std::cell::{Cell, RefCell};

    use gtk4::gio;
    use gtk4::glib;
    use gtk4::prelude::*;

    use hxvoice_runtime::video;

    use crate::portal::{self, options, DIALOG};
    use crate::tr::tr;

    const CAMERA: &str = "org.freedesktop.portal.Camera";

    type Waiter = Box<dyn FnOnce(Result<(), String>)>;

    thread_local! {
        /// Everyone waiting on the access request in flight. Two panels
        /// asking at once share the one dialog.
        static WAITERS: RefCell<Vec<Waiter>> = const { RefCell::new(Vec::new()) };
        static PROBED: Cell<bool> = const { Cell::new(false) };
        /// The watch on the portal's `IsCameraPresent`.
        static WATCH: RefCell<Option<gio::SignalSubscription>> = const { RefCell::new(None) };
    }

    /// Ask the portal whether there is a camera at all, and call `done`
    /// with the answer recorded, then again whenever the portal's answer
    /// changes: a camera plugged in later lights the button. Asking shows
    /// nothing, so this is safe to do as soon as a camera button appears;
    /// it lets the button grey out on a machine without one.
    pub(crate) fn probe(done: impl Fn() + 'static) {
        if !video::camera_via_portal() || PROBED.with(|p| p.replace(true)) {
            return;
        }
        glib::MainContext::default().spawn_local(async move {
            let Ok(conn) = gio::bus_get_future(gio::BusType::Session).await else {
                return;
            };
            let done = std::rc::Rc::new(done);
            // Subscribe before reading, so a change between the two isn't
            // lost. Held for the life of the process, like the answer.
            let watch = {
                let done = std::rc::Rc::clone(&done);
                conn.subscribe_to_signal(
                    Some(portal::PORTAL_BUS),
                    Some("org.freedesktop.DBus.Properties"),
                    Some("PropertiesChanged"),
                    Some(portal::PORTAL_PATH),
                    Some(CAMERA),
                    gio::DBusSignalFlags::NONE,
                    move |sig| {
                        let Some((_, changed, _)) =
                            sig.parameters
                                .get::<(String, glib::VariantDict, Vec<String>)>()
                        else {
                            return;
                        };
                        if let Some(present) =
                            changed.lookup::<bool>("IsCameraPresent").ok().flatten()
                        {
                            video::set_camera_portal_present(present);
                            done();
                        }
                    },
                )
            };
            WATCH.with(|w| *w.borrow_mut() = Some(watch));
            if let Some(present) = portal::property(&conn, CAMERA, "IsCameraPresent")
                .await
                .and_then(|v| v.get::<bool>())
            {
                video::set_camera_portal_present(present);
                done();
            }
        });
    }

    /// Whether an access request is waiting on the user.
    pub(crate) fn pending() -> bool {
        WAITERS.with(|w| !w.borrow().is_empty())
    }

    /// Get camera access and the portal's remote, then `done(Ok(()))`.
    /// `done(Err(message))` otherwise; the message is empty when the user
    /// dismissed the dialog, which needs no telling.
    pub(crate) fn ensure(done: impl FnOnce(Result<(), String>) + 'static) {
        if !super::needed() {
            done(Ok(()));
            return;
        }
        let first = WAITERS.with(|w| {
            let mut w = w.borrow_mut();
            w.push(Box::new(done));
            w.len() == 1
        });
        if !first {
            return;
        }
        glib::MainContext::default().spawn_local(async move {
            let result = access().await;
            let waiters = WAITERS.with(|w| std::mem::take(&mut *w.borrow_mut()));
            for waiter in waiters {
                waiter(result.clone());
            }
        });
    }

    async fn access() -> Result<(), String> {
        let conn = gio::bus_get_future(gio::BusType::Session)
            .await
            .map_err(|e| e.to_string())?;
        // Ask for access only if there is a camera to give. A portal too
        // old to have the property answers nothing, and then the request
        // itself will say.
        if let Some(present) = portal::property(&conn, CAMERA, "IsCameraPresent")
            .await
            .and_then(|v| v.get::<bool>())
        {
            video::set_camera_portal_present(present);
            if !present {
                return Err(tr("No camera is available"));
            }
        }
        portal::request(&conn, CAMERA, "AccessCamera", DIALOG, |tok| {
            glib::Variant::tuple_from_iter([options(&[("handle_token", tok.to_variant())])])
        })
        .await
        .map_err(|e| {
            e.message(
                || tr("GtkHx isn't allowed to use the camera. You can change that in your system's app settings."),
                || tr("The camera request timed out."),
            )
        })?;
        let fd = portal::open_pipewire_remote(
            &conn,
            CAMERA,
            glib::Variant::tuple_from_iter([options(&[])]),
        )
        .await?;
        video::set_camera_remote(fd);
        Ok(())
    }
}

#[cfg(not(target_os = "linux"))]
mod imp {
    pub(crate) fn probe(_done: impl Fn() + 'static) {}

    pub(crate) fn pending() -> bool {
        false
    }

    pub(crate) fn ensure(done: impl FnOnce(Result<(), String>) + 'static) {
        done(Ok(()));
    }
}

pub(crate) use imp::{ensure, pending, probe};
