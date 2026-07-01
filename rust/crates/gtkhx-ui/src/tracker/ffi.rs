//! Tracker-specific FFI: the `HxTrackerServer` boxed-payload mirror the
//! `tracker-server-create` signal delivers, plus the tracker network
//! entry points and the `gtkhx-boxed` meta deep-copy/free funcs.

use crate::tracker::meta::HxTrackerV3Meta;
use std::ffi::c_char;
use std::os::raw::c_void;

/// `#[repr(C)]` mirror of `struct _HxTrackerServer` (`tracker_event.h`),
/// matching `gtkhx-boxed::tracker::HxTrackerServer` (72 bytes). We only
/// read it (in `tracker_server_create`); the C producers fill it.
#[repr(C)]
pub struct HxTrackerServer {
    pub addr_type: u8,
    pub address: *mut c_char,
    pub port: u16,
    pub nusers: u16,
    pub name: *mut c_char,
    pub desc: *mut c_char,
    pub tlv_count: u16,
    pub tlv_bytes: *mut c_void, // GBytes* — unused here
    pub meta: *mut HxTrackerV3Meta,
    pub total: i32,
}

const _: () = {
    use std::mem::{offset_of, size_of};
    assert!(size_of::<HxTrackerServer>() == 72);
    assert!(offset_of!(HxTrackerServer, address) == 8);
    assert!(offset_of!(HxTrackerServer, port) == 16);
    assert!(offset_of!(HxTrackerServer, meta) == 56);
};

extern "C" {
    // gtkhx-boxed: deep copy / free of the typed v3 meta. A row owns a
    // deep copy so it outlives the borrowed event.
    pub fn hx_tracker_v3_meta_copy(src: *mut HxTrackerV3Meta) -> *mut HxTrackerV3Meta;
    pub fn hx_tracker_v3_meta_free(m: *mut HxTrackerV3Meta);

    // network.c tracker fetch. `sess` is a `session *` we hold opaquely.
    pub fn hx_tracker_list_async(sess: *mut c_void);
    pub fn tracker_kill_threads();
}
