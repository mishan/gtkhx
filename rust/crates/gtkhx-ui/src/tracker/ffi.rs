//! Tracker-specific FFI: the tracker network entry points. The event and
//! metadata types, and the metadata copy and free, come from
//! `gtkhx-core::boxed::tracker`.

use std::os::raw::c_void;

pub use gtkhx_core::boxed::tracker::{
    hx_tracker_v3_meta_copy, hx_tracker_v3_meta_free, HxTrackerServer,
};

extern "C" {
    // network.c tracker fetch. `sess` is a `session *` we hold opaquely.
    pub fn hx_tracker_list_async(sess: *mut c_void);
    pub fn tracker_kill_threads();
}
