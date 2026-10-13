//! The inline picture a chat line or a private message carries, as a row in
//! its chat view: a placeholder caption at once, swapped for the picture (or
//! its animation) once it has been fetched and decoded.
//!
//! The fetch is `inline_media_download_start`, over the same C ABI the
//! click-to-view dialog uses; the decode is hx-image-decode's. The pending
//! fetch holds the view weakly, so a window closed meanwhile just drops the
//! picture, and the row is found by its token in the view it was appended
//! to — never by looking a conversation up again.

use std::ffi::CStr;
use std::os::raw::{c_char, c_void};
use std::ptr;

use glib::translate::{from_glib_none, ToGlibPtr};
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;
use rotulus::RotulusView;

use crate::inline_media_dialog::{inline_media_download_start, DownloadResult};
use crate::tr::trc;
use gtkhx_core::boxed::chat::HxChatMedia;
use gtkhx_core::boxed::media_table::{
    hx_media_table_free, hx_media_table_lookup, hx_media_table_new, hx_media_table_register,
};
use hx_image_decode::ffi::{
    inline_media_decode_async, inline_media_decode_cancel, inline_media_decoded_free,
    HxInlineMediaCaps, HxInlineMediaDecoded, HxInlineMediaFrame,
};

extern "C" {
    fn inline_media_cap_ok(htlc: *mut c_void) -> glib::ffi::gboolean;
    fn debug_log_str(cat: *const c_char, msg: *const c_char);
}

/// The line a picture's row shows until the picture arrives, and what a
/// screen reader reads for it: "[image · PNG · 800×600 · 121.1 KB · click
/// to view]", each detail only when the server sent it.
fn placeholder(mime: Option<&[u8]>, size: Option<(u32, u32)>, bytes: Option<u32>) -> String {
    let mut parts = vec![trc("chat picture", "image")];
    // The type is the server's bytes, so one that isn't UTF-8 is left out
    // rather than put into the line.
    if let Some(Ok(mime)) = mime.map(std::str::from_utf8) {
        let short = ["PNG", "JPEG", "GIF"]
            .into_iter()
            .find(|s| mime.eq_ignore_ascii_case(&format!("image/{s}")));
        if !mime.is_empty() {
            parts.push(short.unwrap_or(mime).to_owned());
        }
    }
    if let Some((w, h)) = size {
        parts.push(format!("{w}×{h}"));
    }
    if let Some(b) = bytes {
        parts.push(match b {
            0..1024 => format!("{b} B"),
            1024..1048576 => format!("{:.1} KB", b as f64 / 1024.0),
            _ => format!("{:.1} MB", b as f64 / 1048576.0),
        });
    }
    parts.push(trc("chat picture", "click to view"));
    format!("[{}]", parts.join(" · "))
}

/// A fetch in flight: where its picture goes.
struct Fetch {
    view: glib::WeakRef<RotulusView>,
    token: u32,
    decode: *mut c_void,
}

/// The media table a view keeps for itself when no conversation lends one.
struct ViewTable(*mut c_void);

impl Drop for ViewTable {
    fn drop(&mut self) {
        // SAFETY: the pointer came from hx_media_table_new and is freed once.
        unsafe { hx_media_table_free(self.0) };
    }
}

const VIEW_TABLE: &str = "hx-inline-media-table";

/// The view's own table, made on first use along with the click that opens
/// a picture of it in the dialog, for `htlc`'s connection.
fn view_table(view: &RotulusView, htlc: *mut c_void) -> *mut c_void {
    // SAFETY: the key is only ever set to a ViewTable.
    if let Some(t) = unsafe { view.data::<ViewTable>(VIEW_TABLE) } {
        return unsafe { t.as_ref().0 };
    }
    let table = hx_media_table_new();
    unsafe { view.set_data(VIEW_TABLE, ViewTable(table)) };
    let htlc = htlc as usize;
    view.connect_local("media-activated", false, move |args| {
        let view = args[0].get::<gtk::Widget>().ok()?;
        let token = args[1].get::<u32>().ok()?;
        // SAFETY: the table lives as long as the view emitting this.
        unsafe {
            let m = hx_media_table_lookup(table, token);
            if let Some(m) = m.as_ref() {
                crate::inline_media_dialog::inline_media_show_dialog(
                    view.to_glib_none().0,
                    htlc as *mut c_void,
                    m.id,
                    m.id_len,
                    m.mime,
                    if m.width_present != 0 { m.width } else { 0 },
                    if m.height_present != 0 { m.height } else { 0 },
                    if m.bytes_present != 0 { m.bytes } else { 0 },
                );
            }
        }
        None
    });
    table
}

/// `void hx_inline_media_row_append (GtkWidget *view, void *table, const
/// HxChatMedia *media, struct htlc_conn *htlc)` — append `media`'s
/// placeholder row to the chat view and, when the server speaks inline
/// media, fetch the picture into it. `table` is the conversation's media
/// table, which a click on the row is resolved against; NULL keeps one on
/// the view, whose clicks open the dialog for `htlc`.
///
/// # Safety
/// Main thread. `view` is NULL or a `RotulusView`; `table` is NULL or from
/// `hx_media_table_new`; `media` is NULL or valid; `htlc` is a live
/// connection.
#[no_mangle]
pub unsafe extern "C" fn hx_inline_media_row_append(
    view: *mut gtk::ffi::GtkWidget,
    table: *mut c_void,
    media: *const HxChatMedia,
    htlc: *mut c_void,
) {
    if view.is_null() || media.is_null() {
        return;
    }
    let widget: gtk::Widget = from_glib_none(view);
    let Ok(view) = widget.downcast::<RotulusView>() else {
        return;
    };
    let table = if table.is_null() {
        view_table(&view, htlc)
    } else {
        table
    };
    let token = hx_media_table_register(table, media);
    let m = &*media;
    // SAFETY: `mime` is NULL or NUL-terminated (the chat event copies it so).
    let mime = (!m.mime.is_null()).then(|| CStr::from_ptr(m.mime).to_bytes());
    let placeholder = crate::cs(&placeholder(
        mime,
        (m.width_present != 0 && m.height_present != 0).then_some((m.width, m.height)),
        (m.bytes_present != 0).then_some(m.bytes),
    ));
    rotulus::ffi::rotulus_view_append_media(
        view.upcast_ref::<gtk::Widget>().to_glib_none().0,
        ptr::null_mut(),
        placeholder.as_ptr(),
        token,
        0,
    );

    // A server without the extension never answers the fetch.
    if inline_media_cap_ok(htlc) == 0 || m.id_len == 0 || m.id_len > 65535 {
        return;
    }
    let fetch = Box::into_raw(Box::new(Fetch {
        view: view.downgrade(),
        token,
        decode: ptr::null_mut(),
    }));
    if inline_media_download_start(htlc, m.id, m.id_len, on_fetched, fetch.cast()).is_null() {
        drop(Box::from_raw(fetch));
    }
}

/// The fetch landed: decode it, or leave the placeholder up for the dialog
/// to report the failure on a click.
unsafe extern "C" fn on_fetched(
    _htlc: *mut c_void,
    result: *const DownloadResult,
    user_data: *mut c_void,
) {
    let fetch = user_data as *mut Fetch;
    let bytes = result.as_ref().and_then(|r| r.bytes.as_ref());
    let Some(bytes) = bytes.filter(|b| !b.data.is_null()) else {
        drop(Box::from_raw(fetch));
        return;
    };
    // A synchronous reject has already run on_decoded, which freed `fetch`.
    let caps = HxInlineMediaCaps::default();
    let token = inline_media_decode_async(
        bytes.data,
        bytes.len as usize,
        &caps,
        on_decoded,
        fetch.cast(),
    );
    if !token.is_null() {
        (*fetch).decode = token;
    }
}

/// The decode landed: put the picture, or its frames, in the row.
unsafe extern "C" fn on_decoded(decoded: *mut HxInlineMediaDecoded, user_data: *mut c_void) {
    let fetch = Box::from_raw(user_data as *mut Fetch);
    if !fetch.decode.is_null() {
        // Also the token's free on the success path.
        inline_media_decode_cancel(fetch.decode);
    }
    let d = &*decoded;
    if d.texture.is_null() {
        if !d.error_message.is_null() {
            debug_log_str(c"media".as_ptr(), d.error_message);
        }
    } else if let Some(view) = fetch.view.upgrade() {
        // The row may have been trimmed off the view meanwhile.
        if view.find_image(fetch.token).is_some() {
            let frames = d.frames as *const glib::ffi::GArray;
            let frames: Vec<(gtk::gdk::Texture, u32)> = match frames.as_ref() {
                Some(a) if a.len > 1 => {
                    std::slice::from_raw_parts(a.data as *const HxInlineMediaFrame, a.len as usize)
                        .iter()
                        .filter(|f| !f.texture.is_null())
                        .map(|f| (from_glib_none(f.texture), f.delay_ms))
                        .collect()
                }
                _ => vec![(from_glib_none(d.texture), 0)],
            };
            view.set_media_frames(fetch.token, frames);
        }
    }
    inline_media_decoded_free(decoded);
}

#[cfg(test)]
mod tests {
    use super::placeholder;

    #[test]
    fn placeholder_shows_only_what_the_server_sent() {
        for (mime, size, bytes, want) in [
            (
                Some(&b"image/png"[..]),
                Some((800, 600)),
                Some(124000),
                "[image · PNG · 800×600 · 121.1 KB · click to view]",
            ),
            (
                Some(b"IMAGE/JPEG"),
                None,
                None,
                "[image · JPEG · click to view]",
            ),
            (
                Some(b"image/webp"),
                None,
                Some(512),
                "[image · image/webp · 512 B · click to view]",
            ),
            (
                Some(b"\xC3\xC3invalid"),
                None,
                None,
                "[image · click to view]",
            ),
            (
                None,
                None,
                Some(3 << 20),
                "[image · 3.0 MB · click to view]",
            ),
        ] {
            assert_eq!(placeholder(mime, size, bytes), want);
        }
    }
}
