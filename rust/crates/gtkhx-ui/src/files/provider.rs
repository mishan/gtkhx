//! The two C file providers, local (GIO) and remote (Hotline), behind one
//! safe handle.
//!
//! The providers stay C until the next step of the port; the view reaches them
//! only through here, over the `hx_files_provider_*` interface and the two
//! concrete types' own calls.

use std::ffi::{c_char, c_void};

use glib::translate::{from_glib, from_glib_full, from_glib_none};
use gtk::gio;
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use hxmodel::files_entry::HxFileEntry;

type Raw = *mut glib::gobject_ffi::GObject;

extern "C" {
    fn hx_local_files_provider_get_type() -> glib::ffi::GType;
    fn hx_remote_files_provider_get_type() -> glib::ffi::GType;
    fn hx_local_files_provider_new(initial_path: *const c_char) -> Raw;
    fn hx_remote_files_provider_new(sess: *mut c_void) -> Raw;
    fn hx_remote_files_provider_session(provider: Raw) -> *mut c_void;
    fn hx_remote_files_provider_has_listing_error(provider: Raw) -> glib::ffi::gboolean;
    fn hx_remote_files_provider_reset_to_root(provider: Raw);

    fn hx_files_provider_get_listing(p: Raw) -> *mut gio::ffi::GListModel;
    fn hx_files_provider_get_current_path(p: Raw) -> *const c_char;
    fn hx_files_provider_navigate(p: Raw, path: *const c_char);
    fn hx_files_provider_reload(p: Raw);
    fn hx_files_provider_navigate_up(p: Raw);
    fn hx_files_provider_mkdir(
        p: Raw,
        name: *const c_char,
        err: *mut *mut glib::ffi::GError,
    ) -> glib::ffi::gboolean;
    fn hx_files_provider_delete(
        p: Raw,
        name: *const c_char,
        err: *mut *mut glib::ffi::GError,
    ) -> glib::ffi::gboolean;
    fn hx_files_provider_rename(
        p: Raw,
        old_name: *const c_char,
        new_name: *const c_char,
        err: *mut *mut glib::ffi::GError,
    ) -> glib::ffi::gboolean;
    fn hx_files_provider_get_unavailable_reason(p: Raw) -> *const c_char;
    fn hx_files_provider_activate_entry(p: Raw, e: *mut glib::gobject_ffi::GObject);
    fn hx_files_provider_preview_entry(p: Raw, e: *mut glib::gobject_ffi::GObject);

    /// `files_ops.c` — copy one entry from `src`'s folder to `dst`'s: a
    /// download, an upload, or a local copy. An `HxOpsResult`.
    fn hx_files_ops_copy(src: Raw, dst: Raw, e: *mut glib::gobject_ffi::GObject) -> i32;
    fn hx_files_ops_result_message(r: i32) -> *const c_char;

    /// `gtkhx_ui_bridge.c` — the connection a session owns.
    fn gtkhx_session_htlc(sess: *mut c_void) -> *mut c_void;
    fn hx_conn_fd(h: *const c_void) -> i32;
}

use gtkhx_core::conn::hx_conn_access_has;

/// A local or remote file provider.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Provider(glib::Object);

/// A failed copy's reason, as `files_ops.c` words it.
pub struct CopyError(i32);

impl CopyError {
    pub fn message(&self) -> String {
        unsafe { crate::cstr(hx_files_ops_result_message(self.0)) }
    }
}

impl Provider {
    /// A local provider, at the download folder.
    pub fn local() -> Self {
        unsafe {
            Provider(from_glib_full(
                hx_local_files_provider_new(std::ptr::null()),
            ))
        }
    }

    /// A local provider at `path`.
    pub fn local_at(path: &str) -> Self {
        let c = crate::cs(path);
        unsafe { Provider(from_glib_full(hx_local_files_provider_new(c.as_ptr()))) }
    }

    /// A remote provider listing `sess`'s server.
    ///
    /// # Safety
    /// `sess` is a live `session *` that outlives the provider.
    pub unsafe fn remote(sess: *mut c_void) -> Self {
        Provider(from_glib_full(hx_remote_files_provider_new(sess)))
    }

    fn raw(&self) -> Raw {
        self.0.as_ptr()
    }

    pub fn is_local(&self) -> bool {
        let t: glib::Type = unsafe { from_glib(hx_local_files_provider_get_type()) };
        self.0.type_().is_a(t)
    }

    pub fn is_remote(&self) -> bool {
        let t: glib::Type = unsafe { from_glib(hx_remote_files_provider_get_type()) };
        self.0.type_().is_a(t)
    }

    /// The listing, a model of [`HxFileEntry`].
    pub fn listing(&self) -> gio::ListModel {
        unsafe { from_glib_none(hx_files_provider_get_listing(self.raw())) }
    }

    pub fn current_path(&self) -> String {
        unsafe { crate::cstr(hx_files_provider_get_current_path(self.raw())) }
    }

    pub fn navigate(&self, path: &str) {
        let c = crate::cs(path);
        unsafe { hx_files_provider_navigate(self.raw(), c.as_ptr()) }
    }

    pub fn reload(&self) {
        unsafe { hx_files_provider_reload(self.raw()) }
    }

    pub fn navigate_up(&self) {
        unsafe { hx_files_provider_navigate_up(self.raw()) }
    }

    pub fn mkdir(&self, name: &str) -> Result<(), glib::Error> {
        let c = crate::cs(name);
        let mut err = std::ptr::null_mut();
        let ok = unsafe { hx_files_provider_mkdir(self.raw(), c.as_ptr(), &mut err) };
        result(ok, err)
    }

    pub fn delete(&self, name: &str) -> Result<(), glib::Error> {
        let c = crate::cs(name);
        let mut err = std::ptr::null_mut();
        let ok = unsafe { hx_files_provider_delete(self.raw(), c.as_ptr(), &mut err) };
        result(ok, err)
    }

    pub fn rename(&self, old_name: &str, new_name: &str) -> Result<(), glib::Error> {
        let (o, n) = (crate::cs(old_name), crate::cs(new_name));
        let mut err = std::ptr::null_mut();
        let ok = unsafe { hx_files_provider_rename(self.raw(), o.as_ptr(), n.as_ptr(), &mut err) };
        result(ok, err)
    }

    /// Why the provider can't list right now ("Not connected…"), or `None`.
    pub fn unavailable_reason(&self) -> Option<String> {
        let p = unsafe { hx_files_provider_get_unavailable_reason(self.raw()) };
        (!p.is_null()).then(|| unsafe { crate::cstr(p) })
    }

    /// The default action for a file: open it locally, download it remotely.
    pub fn activate(&self, e: &HxFileEntry) {
        unsafe {
            hx_files_provider_activate_entry(self.raw(), e.upcast_ref::<glib::Object>().as_ptr())
        }
    }

    pub fn preview(&self, e: &HxFileEntry) {
        unsafe {
            hx_files_provider_preview_entry(self.raw(), e.upcast_ref::<glib::Object>().as_ptr())
        }
    }

    /// Copy `e` from this provider's folder to `dst`'s.
    pub fn copy_to(&self, dst: &Provider, e: &HxFileEntry) -> Result<(), CopyError> {
        let r = unsafe {
            hx_files_ops_copy(
                self.raw(),
                dst.raw(),
                e.upcast_ref::<glib::Object>().as_ptr(),
            )
        };
        if r == 0 {
            Ok(())
        } else {
            Err(CopyError(r))
        }
    }

    /// The session a remote provider lists; null for a local one.
    pub fn session(&self) -> *mut c_void {
        if self.is_remote() {
            unsafe { hx_remote_files_provider_session(self.raw()) }
        } else {
            std::ptr::null_mut()
        }
    }

    /// The connection a remote provider lists, or `None` for a local one.
    pub fn conn(&self) -> Option<Conn> {
        let sess = self.session();
        if sess.is_null() {
            return None;
        }
        let htlc = unsafe { gtkhx_session_htlc(sess) };
        (!htlc.is_null()).then_some(Conn(htlc))
    }

    /// Whether a remote provider's latest listing came back as an error.
    pub fn has_listing_error(&self) -> bool {
        self.is_remote()
            && unsafe { hx_remote_files_provider_has_listing_error(self.raw()) }
                != glib::ffi::GFALSE
    }

    /// Drop a remote provider's listing and go back to the server's root.
    pub fn reset_to_root(&self) {
        if self.is_remote() {
            unsafe { hx_remote_files_provider_reset_to_root(self.raw()) }
        }
    }

    /// "navigated": the listing now shows the new path.
    pub fn connect_navigated<F: Fn(&str) + 'static>(&self, f: F) -> glib::SignalHandlerId {
        self.0.connect_local("navigated", false, move |args| {
            let path = args
                .get(1)
                .and_then(|v| v.get::<Option<String>>().ok())
                .flatten()
                .unwrap_or_default();
            f(&path);
            None
        })
    }

    /// "unavailable-changed": the remote side connected or disconnected.
    pub fn connect_unavailable_changed<F: Fn() + 'static>(&self, f: F) -> glib::SignalHandlerId {
        self.0
            .connect_local("unavailable-changed", false, move |_| {
                f();
                None
            })
    }

    pub fn emit_unavailable_changed(&self) {
        self.0.emit_by_name::<()>("unavailable-changed", &[]);
    }

    pub fn disconnect(&self, id: glib::SignalHandlerId) {
        self.0.disconnect(id);
    }
}

fn result(ok: glib::ffi::gboolean, err: *mut glib::ffi::GError) -> Result<(), glib::Error> {
    if ok != glib::ffi::GFALSE {
        return Ok(());
    }
    if err.is_null() {
        Err(glib::Error::new(glib::FileError::Failed, ""))
    } else {
        Err(unsafe { from_glib_full(err) })
    }
}

/// A remote pane's connection.
#[derive(Clone, Copy)]
pub struct Conn(*mut c_void);

impl Conn {
    pub fn ptr(self) -> *mut c_void {
        self.0
    }

    pub fn connected(self) -> bool {
        unsafe { hx_conn_fd(self.0) != 0 }
    }

    /// Whether the account has access bit `bit` (`hl_access.h`).
    pub fn access(self, bit: i32) -> bool {
        unsafe { hx_conn_access_has(self.0.cast(), bit) != glib::ffi::GFALSE }
    }
}
