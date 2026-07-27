//! View-side formatting of Hotline wire timestamps.
//!
//! The wire *decode* (which of the two formats, sentinel + range checks) is the
//! protocol layer's job — `hotline_proto::hl_date::parse_hl_date`. Turning the
//! result into an absolute instant (the modern format is relative to Jan 1 of
//! its year in *local* time) and formatting it for display is the view's, via
//! `glib::DateTime` (local timezone, strftime-style format codes). This is where
//! the old C `news_recv_bridge.c::post_date_format` + `hl_date.c` lived.

use hotline_proto::hl_date::{parse_hl_date, HlDate, MAC_TO_UNIX_EPOCH_OFFSET};
use std::os::raw::c_void;

// The post timestamp type comes from hxnews-model directly now. This module
// used to carry its own `#[repr(C)]` struct mirroring `HxNewsDate` — a second
// definition kept in sync by hand, which the old `extern "C"` declaration of
// `hx_news_node_get_date` could not check. One definition, checked by rustc.
use hxmodel::news::HxNewsDate;

use hxmodel::news::node::hx_news_node_get_date;

/// Format an 8-byte Hotline wire timestamp with `fmt` (glib strftime codes) in
/// the host's local timezone. `None` for the no-timestamp sentinel / out-of-
/// range year, or if glib can't build the date.
pub(crate) fn format_wire(bytes: &[u8], fmt: &str) -> Option<String> {
    let dt = match parse_hl_date(bytes)? {
        HlDate::Mac1904 { secs } => {
            // Absolute UTC instant, rendered local.
            glib::DateTime::from_unix_local(secs as i64 - MAC_TO_UNIX_EPOCH_OFFSET as i64).ok()?
        }
        HlDate::Modern { year, secs } => {
            // Seconds since Jan 1 `year` 00:00:00 local time.
            let base = glib::DateTime::from_local(year as i32, 1, 1, 0, 0, 0.0).ok()?;
            glib::DateTime::from_unix_local(base.to_unix() + secs as i64).ok()?
        }
    };
    dt.format(fmt).ok().map(|g| g.to_string())
}

/// Format a news post node's date the way the browser's post pane +
/// reply-context card show it. `None` when the node has no (valid) date.
///
/// # Safety
/// `node` is NULL or a valid `HxNewsNode *`.
pub(crate) unsafe fn news_node_date_string(node: *mut c_void) -> Option<String> {
    if node.is_null() {
        return None;
    }
    let mut d = HxNewsDate::default();
    hx_news_node_get_date(node as *mut glib::gobject_ffi::GObject, &mut d);
    let mut bytes = [0u8; 8];
    bytes[0..2].copy_from_slice(&d.base_year.to_be_bytes());
    bytes[2..4].copy_from_slice(&d.pad.to_be_bytes());
    bytes[4..8].copy_from_slice(&d.seconds.to_be_bytes());
    format_wire(&bytes, "%a %b %e %H:%M:%S %Y")
}
