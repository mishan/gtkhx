//! The paperclip beside a chat's or a private message's input: pick a
//! picture, check it against the server's limits and the formats inline
//! media allows, upload it, and send the handle the upload gave on a chat
//! line or a private message.
//!
//! The button shows only while the connection has inline media; every
//! button made is kept, weakly, so a login or a disconnect can show or hide
//! them all.

use std::cell::RefCell;
use std::ffi::{c_char, c_void};
use std::ptr;

use glib::translate::IntoGlibPtr;
use gtk::prelude::*;
use gtk::{gio, glib};
use gtk4 as gtk;

use gtkhx_core::boxed::chat::HxChatMedia;
use gtkhx_core::conn::{hx_conn_has_cap, hx_conn_media_max_bytes, hx_conn_name, hx_conn_sess};
use hxhandlers::media::{hx_send_upload_media, UploadResult};

use crate::dock::{self, Bound, ConnKey};
use crate::tr::{tr, tr1, tr_argv, tr_fmt};

/// `HTLC_CAP_INLINE_MEDIA`.
const CAP_INLINE_MEDIA: u64 = hxsession::cap::INLINE_MEDIA;
/// `HX_MEDIA_DEFAULT_MAX_BYTES`: the largest picture when the server names
/// no limit.
const DEFAULT_MAX_BYTES: u32 = 256 * 1024;
/// What a picture sent alone says to a client without inline media.
const CAPTION: &std::ffi::CStr = c"[image]";

/// Where a picture goes once it is up.
#[derive(Clone, Copy)]
enum Target {
    Chat(u32),
    Message(u16),
}

extern "C" {
    fn toolbar_show_toast(text: *const c_char);
    fn hx_gchat_cid(gchat: *mut c_void) -> u32;
    fn msg_output(sess: *mut c_void, name: *const c_char, uid: u16, buf: *const c_char);
    fn msgwin_with_uid(sess: *mut c_void, uid: u16) -> *mut c_void;
    fn hx_msgwin_outputbuf(msg: *mut c_void) -> *mut gtk::ffi::GtkWidget;
    fn debug_log_str(cat: *const c_char, msg: *const c_char);
}

thread_local! {
    static BUTTONS: RefCell<Vec<(glib::WeakRef<gtk::Button>, ConnKey)>> = const { RefCell::new(Vec::new()) };
}

fn toast(text: &str) {
    let text = crate::cs(text);
    unsafe { toolbar_show_toast(text.as_ptr()) };
}

unsafe fn has_media(htlc: *mut c_void) -> bool {
    !htlc.is_null() && hx_conn_has_cap(htlc.cast(), CAP_INLINE_MEDIA) != glib::ffi::GFALSE
}

/// The connection `bound` named, while it is that connect and has inline
/// media; otherwise says so.
fn live(bound: Bound) -> Option<*mut c_void> {
    let htlc = dock::live_htlc(bound).filter(|&h| unsafe { has_media(h) });
    if htlc.is_none() {
        toast(&tr("Inline media isn't available on this server."));
    }
    htlc
}

/// The server's limit on a picture's size, or the default.
unsafe fn max_bytes(htlc: *mut c_void) -> u32 {
    match hx_conn_media_max_bytes(htlc.cast()) {
        0 => DEFAULT_MAX_BYTES,
        n => n,
    }
}

fn too_large(name: &str, len: u64, max: u32) -> String {
    tr_fmt(
        "%1$s is %2$s KB — over the %3$s KB cap. Pick a smaller image.",
        &[
            name,
            &format!("{:.1}", len as f64 / 1024.0),
            &(max / 1024).to_string(),
        ],
    )
}

fn button(htlc: *mut c_void, target: Target) -> gtk::Button {
    let btn = crate::icon_button("mail-attachment-symbolic", &tr("Attach Image"));
    btn.set_visible(unsafe { has_media(htlc) });
    let conn = dock::conn_key(htlc);
    btn.connect_clicked(move |b| pick(b, dock::bind(conn), target));
    BUTTONS.with_borrow_mut(|v| {
        v.retain(|(w, _)| w.upgrade().is_some());
        v.push((btn.downgrade(), conn));
    });
    btn
}

fn pick(btn: &gtk::Button, bound: Bound, target: Target) {
    if live(bound).is_none() {
        return;
    }
    let filter = gtk::FileFilter::new();
    filter.set_name(Some(&tr("Images (PNG, JPEG, GIF)")));
    for mime in ["image/png", "image/jpeg", "image/gif"] {
        filter.add_mime_type(mime);
    }
    let filters = gio::ListStore::new::<gtk::FileFilter>();
    filters.append(&filter);
    let dialog = gtk::FileDialog::new();
    dialog.set_title(&tr("Attach Image"));
    dialog.set_filters(Some(&filters));
    let parent = btn.root().and_downcast::<gtk::Window>();
    dialog.open(parent.as_ref(), gio::Cancellable::NONE, move |r| match r {
        Ok(file) => unsafe { picked(&file, bound, target) },
        Err(e) if !e.matches(gtk::DialogError::Dismissed) => {
            toast(&tr1("File picker failed: %s", e.message()));
        }
        Err(_) => {}
    });
}

/// The file dialog took a moment: the connection may have gone, or come
/// back without inline media, meanwhile.
unsafe fn picked(file: &gio::File, bound: Bound, target: Target) {
    let name = file
        .basename()
        .map_or_else(|| tr("image"), |p| p.to_string_lossy().into_owned());
    let Some(htlc) = live(bound) else {
        return;
    };
    // Before reading the file: the filter doesn't bound its size.
    let max = max_bytes(htlc);
    if let Ok(info) = file.query_info(
        gio::FILE_ATTRIBUTE_STANDARD_SIZE,
        gio::FileQueryInfoFlags::NOFOLLOW_SYMLINKS,
        gio::Cancellable::NONE,
    ) {
        if info.size() > i64::from(max) {
            toast(&too_large(&name, info.size() as u64, max));
            return;
        }
    }
    file.load_bytes_async(gio::Cancellable::NONE, move |r| match r {
        Ok((bytes, _)) => unsafe { loaded(&bytes, &name, bound, target) },
        Err(e) => toast(&tr_argv("Couldn't read %s: %s", &[&name, e.message()])),
    });
}

/// A picture on its way up, with where it goes and what to call it.
struct Upload {
    target: Target,
    name: String,
}

unsafe fn loaded(bytes: &glib::Bytes, name: &str, bound: Bound, target: Target) {
    let Some(htlc) = live(bound) else {
        return;
    };
    let max = max_bytes(htlc);
    if bytes.len() > max as usize {
        toast(&too_large(name, bytes.len() as u64, max));
        return;
    }
    let format = hx_image_decode::sniff(bytes);
    let mime = hx_image_decode::format_to_mime(format);
    if !hx_image_decode::format_is_allowed(format) {
        toast(&match mime {
            Some(kind) => tr_argv(
                "%s uses %s, which isn't supported by inline media. Try PNG, JPEG, or GIF.",
                &[name, kind],
            ),
            None => tr1("%s isn't a recognized image. Try PNG, JPEG, or GIF.", name),
        });
        return;
    }
    let mime = mime.unwrap_or_default();
    let upload = Box::into_raw(Box::new(Upload {
        target,
        name: name.to_owned(),
    }));
    if hx_send_upload_media(
        htlc,
        bytes.as_ref().as_ptr(),
        bytes.len(),
        mime.as_ptr().cast(),
        mime.len(),
        Some(uploaded),
        upload.cast(),
        Some(drop_upload),
    ) == glib::ffi::GFALSE
    {
        drop(Box::from_raw(upload));
        toast(&tr("Couldn't start image upload."));
    }
}

unsafe extern "C" fn drop_upload(upload: *mut c_void) {
    drop(Box::from_raw(upload.cast::<Upload>()));
}

/// The server's answer to an upload: send the handle it gave where the
/// picture was meant to go, or say why there is none.
unsafe extern "C" fn uploaded(htlc: *mut c_void, r: *const UploadResult, upload: *mut c_void) {
    let upload = Box::from_raw(upload.cast::<Upload>());
    let r = &*r;
    if r.media_id.is_null() {
        let why = refusal(r);
        debug_log_str(
            c"media".as_ptr(),
            crate::cs(&format!("attach upload failed: {why}")).as_ptr(),
        );
        toast(&why);
        return;
    }
    match upload.target {
        Target::Chat(cid) => hxhandlers::send::chat::hx_send_chat_with_media(
            htlc,
            CAPTION.as_ptr(),
            cid,
            0,
            r.media_id,
            r.media_id_len,
            r.media_type,
            r.media_type_len,
        ),
        Target::Message(uid) => {
            let id = std::slice::from_raw_parts(r.media_id, r.media_id_len);
            let mime = std::slice::from_raw_parts(r.media_type.cast::<u8>(), r.media_type_len);
            hxhandlers::send::msg::send_msg_with_media(htlc, uid, CAPTION, id, mime);
            echo(htlc, uid, r);
        }
    }
    toast(&tr1("Sent %s", &upload.name));
}

/// A server sends a private message to its recipient only, so the sender's
/// window shows its own: the picture fetched by the handle, as the
/// recipient's is.
unsafe fn echo(htlc: *mut c_void, uid: u16, r: &UploadResult) {
    let sess = hx_conn_sess(htlc.cast());
    if sess.is_null() {
        return;
    }
    msg_output(sess, hx_conn_name(htlc.cast()), uid, c"".as_ptr());
    let msg = msgwin_with_uid(sess, uid);
    if msg.is_null() {
        return;
    }
    let media = HxChatMedia {
        id: r.media_id.cast_mut(),
        id_len: r.media_id_len,
        mime: r.media_type.cast_mut(),
        mime_len: r.media_type_len,
        width: r.width,
        height: r.height,
        bytes: r.bytes,
        width_present: r.width_present,
        height_present: r.height_present,
        bytes_present: r.bytes_present,
    };
    crate::inline_media_row::hx_inline_media_row_append(
        hx_msgwin_outputbuf(msg),
        ptr::null_mut(),
        &media,
        htlc,
    );
}

/// What to tell the user of a refused upload: the error code's reading,
/// with the server's own words after it.
unsafe fn refusal(r: &UploadResult) -> String {
    let what = match r.error_code {
        1 => tr("Image too large for this server."),
        2 => tr("Server rejected the image format."),
        3 => tr("Rate limited — try again shortly."),
        4 => tr("Not authorized to send images."),
        5 => tr("Server temporarily busy."),
        _ => tr("Image upload failed."),
    };
    if r.error_message.is_null() || r.error_message_len == 0 {
        return what;
    }
    let said = std::slice::from_raw_parts(r.error_message.cast::<u8>(), r.error_message_len);
    let said: String = String::from_utf8_lossy(said).chars().take(200).collect();
    format!("{what} ({said})")
}

/// `GtkWidget *hx_inline_media_attach_button_new (struct gtkhx_chat *gchat,
/// struct htlc_conn *htlc)` — the paperclip for the chat `gchat`.
///
/// # Safety
/// Main thread; `gchat` is a live chat view and `htlc` its connection.
#[no_mangle]
pub unsafe extern "C" fn hx_inline_media_attach_button_new(
    gchat: *mut c_void,
    htlc: *mut c_void,
) -> *mut gtk::ffi::GtkWidget {
    crate::ensure_gtk_init();
    let cid = if gchat.is_null() {
        0
    } else {
        hx_gchat_cid(gchat)
    };
    button(htlc, Target::Chat(cid))
        .upcast::<gtk::Widget>()
        .into_glib_ptr()
}

/// The paperclip for the private messages to `uid` on `htlc`.
///
/// # Safety
/// Main thread; `htlc` is a live connection.
pub unsafe fn message_button(htlc: *mut c_void, uid: u16) -> gtk::Button {
    button(htlc, Target::Message(uid))
}

/// `void inline_media_attach_refresh (struct htlc_conn *htlc)` — show or
/// hide `htlc`'s paperclips as it has inline media or not.
///
/// # Safety
/// Main thread; `htlc` is NULL or a live connection.
#[no_mangle]
pub unsafe extern "C" fn inline_media_attach_refresh(htlc: *mut c_void) {
    let show = has_media(htlc);
    let conn = dock::conn_key(htlc);
    BUTTONS.with_borrow_mut(|v| {
        v.retain(|(w, _)| w.upgrade().is_some());
        for (w, key) in v.iter() {
            if *key == conn {
                if let Some(b) = w.upgrade() {
                    b.set_visible(show);
                }
            }
        }
    });
}
