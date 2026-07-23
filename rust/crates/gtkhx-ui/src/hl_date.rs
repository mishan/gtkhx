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

/// `#[repr(C)]` mirror of C's `struct date_time` (hxnews-model's `HxNewsDate`):
/// a post's timestamp in the parsed 3-field wire form.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct DateTime {
    base_year: u16,
    pad: u16,
    seconds: u32,
}

extern "C" {
    /// Copy a news node's post date into `*out` (hxnews-model).
    fn hx_news_node_get_date(node: *mut c_void, out: *mut DateTime);
}

/// Format an 8-byte Hotline wire timestamp with `fmt` (glib strftime codes) in
/// the host's local timezone. `None` for the no-timestamp sentinel / out-of-
/// range year, or if glib can't build the date.
fn format_wire(bytes: &[u8], fmt: &str) -> Option<String> {
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
    let mut d = DateTime::default();
    hx_news_node_get_date(node, &mut d);
    let mut bytes = [0u8; 8];
    bytes[0..2].copy_from_slice(&d.base_year.to_be_bytes());
    bytes[2..4].copy_from_slice(&d.pad.to_be_bytes());
    bytes[4..8].copy_from_slice(&d.seconds.to_be_bytes());
    format_wire(&bytes, "%a %b %e %H:%M:%S %Y")
}
