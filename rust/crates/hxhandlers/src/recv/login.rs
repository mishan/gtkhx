//! The login's reply, as the session reads it: what the server said of
//! itself and agreed to goes on the connection before `logged-in` tells the
//! view, which titles the windows and enables the toolbar from it. A
//! refusal's reason is shown as any refused request's is.

use std::os::raw::{c_char, c_void};

use gtkhx_core::conn::{
    hx_conn_bridge_handle, hx_conn_has_cap, hx_conn_hope_aead, hx_conn_reset_video_limits,
    hx_conn_set_caps, hx_conn_set_history_max_days, hx_conn_set_history_max_msgs,
    hx_conn_set_hope_aead, hx_conn_set_logged_in, hx_conn_set_media_chunk_size,
    hx_conn_set_media_max_bytes, hx_conn_set_media_max_dimension,
    hx_conn_set_media_max_duration_ms, hx_conn_set_media_max_frames, hx_conn_set_media_max_pixels,
    hx_conn_set_uid, hx_conn_set_version, hx_conn_set_video_limits,
};
use hxnet::ffi::{hxnet_connection_hope_aead_material, hxnet_hope_aead_free};
use hxsession::{cap, ServerInfo};

use super::chat::c_text;

#[cfg(not(test))]
use gtkhx_core::session::{
    gtkhx_session_emit_logged_in, gtkhx_session_emit_request_failed, gtkhx_session_get_default,
};

#[cfg(not(test))]
extern "C" {
    /// Log a pre-formatted line under a debug category (`debug.c`).
    fn debug_log_str(cat: *const c_char, msg: *const c_char);
}

/// The server accepted the login.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn logged_in(htlc: *mut c_void, info: &ServerInfo) {
    let h = htlc.cast();
    hx_conn_set_logged_in(h, 1);
    // The keys an HTXF subchannel derives its own from, where HOPE agreed
    // ChaCha20-Poly1305; NULL otherwise.
    let old = hx_conn_hope_aead(h);
    if !old.is_null() {
        hxnet_hope_aead_free(old.cast());
    }
    let aead = hxnet_connection_hope_aead_material(hx_conn_bridge_handle(h).cast());
    hx_conn_set_hope_aead(h, aead.cast());

    if let Some(uid) = info.uid {
        hx_conn_set_uid(h, uid);
    }
    hx_conn_set_version(h, info.version);
    hx_conn_set_caps(h, u64::from(info.caps));
    // Each limit is optional on the wire, and 0 is the client's default.
    let m = &info.media;
    hx_conn_set_media_max_bytes(h, m.max_bytes.unwrap_or(0));
    hx_conn_set_media_max_dimension(h, m.max_dimension.unwrap_or(0));
    hx_conn_set_media_max_pixels(h, m.max_pixels.unwrap_or(0));
    hx_conn_set_media_chunk_size(h, m.chunk_size.unwrap_or(0));
    hx_conn_set_media_max_frames(h, m.max_frames.unwrap_or(0));
    hx_conn_set_media_max_duration_ms(h, m.max_duration_ms.unwrap_or(0));
    hx_conn_set_history_max_msgs(h, info.history_max_msgs.unwrap_or(0));
    hx_conn_set_history_max_days(h, info.history_max_days.unwrap_or(0));
    hx_conn_reset_video_limits(h);
    for l in &info.video {
        hx_conn_set_video_limits(
            h,
            l.kind.wire(),
            l.max_width,
            l.max_height,
            l.max_fps,
            l.max_bitrate,
        );
    }
    if hx_conn_has_cap(h, u64::from(cap::INLINE_MEDIA)) != 0 {
        let line = format!(
            "server inline-media limits: max_bytes={} max_dim={} max_pixels={} \
             chunk_size={} max_frames={} max_duration_ms={} (0 = use default)",
            m.max_bytes.unwrap_or(0),
            m.max_dimension.unwrap_or(0),
            m.max_pixels.unwrap_or(0),
            m.chunk_size.unwrap_or(0),
            m.max_frames.unwrap_or(0),
            m.max_duration_ms.unwrap_or(0),
        );
        debug_log_str(c"media".as_ptr(), c_text(&line).as_ptr());
    }

    let name = info.name.as_deref().map(c_text);
    gtkhx_session_emit_logged_in(
        gtkhx_session_get_default(),
        htlc,
        name.as_ref().map_or(std::ptr::null(), |n| n.as_ptr()),
    );
}

/// The server refused the login, and said why.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub(crate) unsafe fn refused(htlc: *mut c_void, reason: &str) {
    let reason = c_text(reason);
    gtkhx_session_emit_request_failed(gtkhx_session_get_default(), htlc, reason.as_ptr());
}

#[cfg(test)]
mod tests;

#[cfg(test)]
use tests::{
    debug_log_str, gtkhx_session_emit_logged_in, gtkhx_session_emit_request_failed,
    gtkhx_session_get_default,
};
