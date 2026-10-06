//! Files, as the session reads them: the replies to the requests
//! `send::files` and the transfers make, each matched by its trans to what
//! asked for it. What a transfer's reply sets going is `recv::xfer`'s.
//!
//! A listing reaches the files browser as it always did: the `file-list`
//! signal carries a [`CachedFileList`], the folder it lists and what the
//! session read of it, for the remote provider that asked, which fills its
//! store through [`hx_cfl_populate`].

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::{c_char, CString};
use std::os::raw::c_void;

use hxrequest::Request;
use hxsession::{FileEntry, FileInfo, Transfer};

use super::xfer;

/// A listing, for the remote provider that asked: the folder it lists and
/// what is in it. Opaque to C, reached through `hx_cfl_*`.
pub struct CachedFileList {
    path: CString,
    files: Vec<FileEntry>,
}

/// # Safety
/// `cfl` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn hx_cfl_path(cfl: *const CachedFileList) -> *const c_char {
    (*cfl).path.as_ptr()
}

/// Replace `store`'s contents with what the listing holds.
///
/// # Safety
/// `cfl` is a live handle; `store` is a live `GListStore` of `HxFileEntry`.
#[no_mangle]
pub unsafe extern "C" fn hx_cfl_populate(cfl: *const CachedFileList, store: *mut c_void) {
    hxmodel::files_entry::populate(store, &(*cfl).files);
}

/// # Safety
/// `cfl` is a live handle from a `file-list` emit, or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_cfl_free(cfl: *mut CachedFileList) {
    if !cfl.is_null() {
        drop(Box::from_raw(cfl));
    }
}

/// What a request in flight is answered into.
pub(crate) enum Asked {
    /// A folder's listing, for the remote provider that asked.
    Listing { provider: Provider, path: CString },
    /// Get Info on the file the label names, its folder and its name.
    Info(CString),
    /// A download of a file or a folder, into its transfer.
    Download { htxf: Xfer, folder: bool },
    /// An upload of a file or a folder, from its transfer.
    Upload { htxf: Xfer, folder: bool },
    /// The move half of a move and rename: the rename, sent once the move
    /// has gone through.
    Rename(Request),
    /// The server's banner, for its fetch.
    Banner,
}

/// A remote files provider, kept alive while its listing is in flight: a
/// reply that comes after the browser closed, or after the provider gave
/// up waiting, reaches a live object, which ignores it.
pub(crate) struct Provider(*mut c_void);

impl Provider {
    /// # Safety
    /// `p` is NULL or a live GObject.
    pub(crate) unsafe fn new(p: *mut c_void) -> Self {
        if !p.is_null() {
            object_ref(p);
        }
        Provider(p)
    }
}

impl Drop for Provider {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { object_unref(self.0) };
        }
    }
}

/// A transfer, kept alive while its reply is in flight: one cancelled and
/// freed meanwhile cannot have its address taken by another, which the
/// reply would then start.
pub(crate) struct Xfer(*mut c_void);

impl Xfer {
    /// # Safety
    /// `htxf` is a live transfer.
    pub(crate) unsafe fn new(htxf: *mut c_void) -> Self {
        htxf_ref(htxf);
        Xfer(htxf)
    }
}

impl Drop for Xfer {
    fn drop(&mut self) {
        unsafe { htxf_unref(self.0) };
    }
}

#[cfg(not(test))]
unsafe fn htxf_ref(p: *mut c_void) {
    hxnet::xfer_handle::hx_htxf_ref(p.cast());
}
#[cfg(not(test))]
unsafe fn htxf_unref(p: *mut c_void) {
    hxnet::xfer_handle::hx_htxf_unref(p.cast());
}

#[cfg(not(test))]
unsafe fn object_ref(p: *mut c_void) {
    glib::gobject_ffi::g_object_ref(p.cast());
}
#[cfg(not(test))]
unsafe fn object_unref(p: *mut c_void) {
    glib::gobject_ffi::g_object_unref(p.cast());
}

thread_local! {
    /// The requests in flight, by connection and trans.
    static ASKED: RefCell<HashMap<(usize, u32), Asked>> = RefCell::new(HashMap::new());
}

/// A request goes out on `trans`, its reply to go into `what`. A
/// provider's new listing replaces the one it asked for before, whose reply
/// would otherwise show the folder the user has left.
pub(crate) fn asked(htlc: *mut c_void, trans: u32, what: Asked) {
    ASKED.with(|a| {
        let mut a = a.borrow_mut();
        if let Asked::Listing { provider, .. } = &what {
            let p = provider.0;
            a.retain(|_, w| !matches!(w, Asked::Listing { provider, .. } if provider.0 == p));
        }
        a.insert((htlc as usize, trans), what)
    });
}

fn answered(htlc: *mut c_void, trans: u32) -> Option<Asked> {
    ASKED.with(|a| a.borrow_mut().remove(&(htlc as usize, trans)))
}

/// Let go of what `htlc` asked for before: a closed connection gets no more
/// replies, and a new one numbers its requests afresh. With `keep_banner`,
/// the banner's request stays.
pub(crate) fn forget(htlc: *mut c_void, keep_banner: bool) {
    // Dropped once ASKED is released: letting go of a provider or transfer
    // can run its teardown, which must be free to reach ASKED.
    let _gone: HashMap<_, _> = ASKED.with(|a| {
        let mut a = a.borrow_mut();
        let (gone, kept) = std::mem::take(&mut *a)
            .into_iter()
            .partition(|((h, _), w)| {
                *h == htlc as usize && !(keep_banner && matches!(w, Asked::Banner))
            });
        *a = kept;
        gone
    });
}

#[cfg(not(test))]
use gtkhx_core::session::{gtkhx_session_emit_file_list, gtkhx_session_get_default};

#[cfg(not(test))]
extern "C" {
    /// files_remote_provider.c — a refused listing, for the provider that
    /// asked: it shows why there is nothing to show.
    fn hx_remote_files_provider_handle_file_list_error(
        cfl: *mut c_void,
        data: *mut c_void,
    ) -> std::os::raw::c_int;
}

/// What a folder holds, for the provider that asked.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn listed(htlc: *mut c_void, trans: u32, files: &[FileEntry]) {
    if let Some(Asked::Listing { provider, path }) = answered(htlc, trans) {
        let cfl = Box::into_raw(Box::new(CachedFileList {
            path,
            files: files.to_vec(),
        }));
        gtkhx_session_emit_file_list(
            gtkhx_session_get_default(),
            htlc,
            cfl.cast(),
            std::ptr::null_mut(),
            provider.0,
        );
        hx_cfl_free(cfl);
    }
}

/// Get Info's reply, for the file that was asked about.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn info(htlc: *mut c_void, trans: u32, info: &FileInfo) {
    if let Some(Asked::Info(label)) = answered(htlc, trans) {
        xfer::file_info(htlc, &label, info);
    }
}

/// A transfer's reply, for the transfer that asked.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn transfer(htlc: *mut c_void, trans: u32, t: &Transfer) {
    match answered(htlc, trans) {
        Some(Asked::Download { htxf, folder }) => xfer::download_ready(htlc, htxf.0, folder, t),
        Some(Asked::Upload { htxf, folder }) => xfer::upload_ready(htlc, htxf.0, folder, t),
        Some(Asked::Banner) => xfer::banner(htlc, t),
        _ => {}
    }
}

/// A change went through; the rename of a move and rename follows it.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn changed(htlc: *mut c_void, trans: u32) {
    if let Some(Asked::Rename(rename)) = answered(htlc, trans) {
        crate::send::files::change(htlc, Some(rename));
    }
}

/// A request on `trans` was refused, or its reply cut short. The reason, if
/// any, is `request-failed`'s to show.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn failed(htlc: *mut c_void, trans: u32) {
    match answered(htlc, trans) {
        Some(Asked::Listing { provider, path }) => {
            let cfl = Box::into_raw(Box::new(CachedFileList {
                path,
                files: Vec::new(),
            }));
            hx_remote_files_provider_handle_file_list_error(cfl.cast(), provider.0);
            hx_cfl_free(cfl);
        }
        Some(Asked::Download { htxf, .. }) => xfer::download_refused(htlc, htxf.0),
        Some(Asked::Upload { htxf, .. }) => xfer::upload_refused(htlc, htxf.0),
        Some(Asked::Info(_) | Asked::Rename(_) | Asked::Banner) | None => {}
    }
}

#[cfg(test)]
mod doubles;
#[cfg(test)]
use doubles::*;

#[cfg(test)]
mod tests;
