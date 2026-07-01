//! `HxTrackerRow` — one row in the tracker results list, as a Rust
//! `glib::subclass` GObject (ported from `src/tracker_row.c`).
//!
//! A GObject so it can sit in a `gio::ListStore` the `GtkColumnView`
//! consumes. Built from a borrowed `HxTrackerServer` event: strings are
//! copied into Rust `String`s and the typed `HxTrackerV3Meta` is
//! deep-copied (via `gtkhx-boxed`'s `hx_tracker_v3_meta_copy`) so the row
//! outlives the event the signal emitter frees after
//! `tracker_server_create` returns. The owned meta is freed in `dispose`.
//!
//! Unlike the R4 boxed types this exports **no C ABI** — its only C
//! consumer was `tracker.c`, which this branch deletes, so the type lives
//! purely in Rust now.

use crate::tracker::cstr;
use crate::tracker::ffi::{hx_tracker_v3_meta_copy, hx_tracker_v3_meta_free, HxTrackerServer};
use crate::tracker::meta::HxTrackerV3Meta;
use glib::subclass::prelude::*;
use std::cell::{Cell, RefCell};
use std::ptr;

mod imp {
    use super::*;

    pub struct HxTrackerRow {
        pub name: RefCell<String>,
        pub desc: RefCell<String>,
        pub address: RefCell<String>,
        pub port: Cell<u16>,
        pub nusers: Cell<u16>,
        /// Owned `HxTrackerV3Meta*` (glib-allocated). Freed in `dispose`.
        /// Never NULL after `from_event` — v1 records still carry a
        /// zero-init meta (the C event constructors guarantee it).
        pub meta: Cell<*mut HxTrackerV3Meta>,
    }

    impl Default for HxTrackerRow {
        fn default() -> Self {
            Self {
                name: RefCell::new(String::new()),
                desc: RefCell::new(String::new()),
                address: RefCell::new(String::new()),
                port: Cell::new(0),
                nusers: Cell::new(0),
                meta: Cell::new(ptr::null_mut()),
            }
        }
    }

    #[glib::object_subclass]
    impl ObjectSubclass for HxTrackerRow {
        const NAME: &'static str = "HxTrackerRow";
        type Type = super::HxTrackerRow;
        type ParentType = glib::Object;
    }

    impl ObjectImpl for HxTrackerRow {
        fn dispose(&self) {
            let m = self.meta.replace(ptr::null_mut());
            if !m.is_null() {
                unsafe { hx_tracker_v3_meta_free(m) };
            }
        }
    }
}

glib::wrapper! {
    pub struct HxTrackerRow(ObjectSubclass<imp::HxTrackerRow>);
}

impl HxTrackerRow {
    /// Build a row from a borrowed event. Deep-copies strings + meta.
    ///
    /// # Safety
    /// `event` is a valid `HxTrackerServer*` with a non-NULL address
    /// (the C event constructors guarantee it).
    pub unsafe fn from_event(event: *const HxTrackerServer) -> Self {
        let obj: Self = glib::Object::new();
        let e = &*event;
        let imp = obj.imp();
        *imp.address.borrow_mut() = cstr(e.address);
        imp.port.set(e.port);
        imp.nusers.set(e.nusers);
        *imp.name.borrow_mut() = cstr(e.name);
        *imp.desc.borrow_mut() = cstr(e.desc);
        imp.meta.set(hx_tracker_v3_meta_copy(e.meta));
        obj
    }

    pub fn name(&self) -> String {
        self.imp().name.borrow().clone()
    }
    pub fn desc(&self) -> String {
        self.imp().desc.borrow().clone()
    }
    pub fn address(&self) -> String {
        self.imp().address.borrow().clone()
    }
    pub fn port(&self) -> u16 {
        self.imp().port.get()
    }
    pub fn nusers(&self) -> u16 {
        self.imp().nusers.get()
    }
    /// Borrowed pointer to the row's typed TLV view. Lifetime tied to the
    /// row; never dereference past the row's own lifetime.
    pub fn meta(&self) -> *mut HxTrackerV3Meta {
        self.imp().meta.get()
    }
}
