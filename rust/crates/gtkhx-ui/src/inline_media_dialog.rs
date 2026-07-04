//! Inline-media click-to-view dialog (ported from `src/inline_media_dialog.c`).
//!
//! Triggered from `chat.c`'s inline-media `word_click` handler, which keeps
//! calling the C ABI export `inline_media_show_dialog`. The dialog builds an
//! `AdwDialog` with a `GtkStack` body (Loading… → image | error), kicks off
//! `inline_media_download_start`, then decodes the payload via
//! `inline_media_decode_async` (glycin) and swaps to the rendered image — or an
//! error page — when it lands.
//!
//! The protocol download machinery (`inline_media_download.c`) and the bounded
//! glycin decoder (`inline_media_decode.c` / the `hx-image-decode` crate) stay
//! C behind the FFI seam below — this module only owns the dialog UI + the
//! Save-As / Open-Externally handlers.
//!
//! ## Lifetime
//!
//! The per-dialog state (`MediaDialog`) is heap-boxed and its raw pointer is
//! threaded through as the `user_data` of every C callback, mirroring the C
//! `hx_media_dialog *`. The `AdwDialog::closed` handler cancels any in-flight
//! download + decode (which suppresses their callbacks) and then reclaims the
//! box — so no callback can fire against freed state. The Save / Open async
//! paths never touch the state after the click: they capture their own
//! `glib::Bytes` ref, so a dismissed dialog can't free the payload out from
//! under a pending file chooser.

use std::os::raw::{c_char, c_int, c_void};
use std::ptr;

use gtk4 as gtk;
use libadwaita as adw;

// adw::prelude re-exports gtk::prelude (+ gio / glib preludes transitively),
// so the widget / file traits used below all resolve through it.
use adw::prelude::*;
use gtk::glib;
use glib::translate::from_glib_none;

use crate::cstr;
use crate::tr::{tr, tr1, tr_argv};

// ---------------------------------------------------------------------
// FFI: download + decode helpers stay C (protocol / glycin machinery).
// ---------------------------------------------------------------------

/// `#[repr(C)]` mirror of GLib's `GByteArray` — only the two fields the
/// download result carries (`data` + `len`).
#[repr(C)]
struct GByteArray {
    data: *const u8,
    len: u32,
}

/// `#[repr(C)]` mirror of `HxInlineMediaDownloadResult`
/// (`inline_media_download.h`). Only read inside `on_download_done`.
#[repr(C)]
struct DownloadResult {
    bytes: *const GByteArray,
    canonical_mime: *const c_char,
    error_code: u16,
    error_message: *const c_char,
    error_message_len: usize,
}

/// `#[repr(C)]` mirror of `HxInlineMediaDecoded` (`inline_media_decode.h`).
/// Layout pinned by the C `_Static_assert` in `inline_media_decode.c`; we
/// only read `texture` + `error_message`.
#[repr(C)]
struct Decoded {
    texture: *mut gtk::gdk::ffi::GdkTexture,
    canonical_mime: *const c_char,
    sniffed_format: c_int,
    error_code: u16,
    _pad0: u16,
    error_message: *const c_char,
    frames: *mut c_void,
}

/// `#[repr(C)]` mirror of `HxInlineMediaCaps`. All-zero = "fall back to
/// `HX_MEDIA_DEFAULT_*` per field", the same value the C dialog passed.
#[repr(C)]
#[derive(Default)]
struct HxInlineMediaCaps {
    max_bytes: u32,
    max_dimension: u32,
    max_pixels: u32,
    max_frames: u32,
    max_duration_ms: u32,
}

type DownloadCb =
    unsafe extern "C" fn(*mut c_void, *const DownloadResult, *mut c_void);
type DecodeCb = unsafe extern "C" fn(*mut Decoded, *mut c_void);

extern "C" {
    fn inline_media_download_start(
        htlc: *mut c_void,
        handle: *const u8,
        handle_len: usize,
        on_done: DownloadCb,
        user_data: *mut c_void,
    ) -> *mut c_void;
    fn inline_media_download_cancel(dl: *mut c_void);

    fn inline_media_decode_async(
        bytes: *const u8,
        len: usize,
        caps: *const HxInlineMediaCaps,
        cb: DecodeCb,
        user_data: *mut c_void,
    ) -> *mut c_void;
    fn inline_media_decode_cancel(token: *mut c_void);
    fn inline_media_decoded_free(decoded: *mut Decoded);
}

// ---------------------------------------------------------------------
// Per-dialog state.
// ---------------------------------------------------------------------

struct MediaDialog {
    dialog: adw::Dialog,
    stack: gtk::Stack,
    picture: gtk::Picture,
    error_label: gtk::Label,
    save_btn: gtk::Button,
    open_btn: gtk::Button,

    /// `hx_inline_media_download *` while a download is in flight; NULL once
    /// it completes or is cancelled.
    download: *mut c_void,
    /// Glycin decode cancel-token while a decode is in flight; NULL otherwise.
    /// It is *also* the canonical free for the token, so it's consumed once
    /// (a no-op cancel) even on the success path.
    decode_token: *mut c_void,

    /// Canonical bytes the Save / Open handlers write out. `None` until the
    /// download succeeds.
    bytes: Option<glib::Bytes>,
    mime: Option<String>,
}

fn mime_to_suffix(mime: Option<&str>) -> &'static str {
    match mime {
        Some(m) if m.eq_ignore_ascii_case("image/png") => ".png",
        Some(m) if m.eq_ignore_ascii_case("image/jpeg") => ".jpg",
        Some(m) if m.eq_ignore_ascii_case("image/gif") => ".gif",
        Some(_) => ".bin",
        None => ".bin",
    }
}

fn swap_to_error(md: &MediaDialog, message: &str) {
    md.error_label.set_text(message);
    md.stack.set_visible_child_name("error");
}

// ---------------------------------------------------------------------
// Download callback (main thread).
// ---------------------------------------------------------------------

unsafe extern "C" fn on_download_done(
    _htlc: *mut c_void,
    result: *const DownloadResult,
    user_data: *mut c_void,
) {
    let md = &mut *(user_data as *mut MediaDialog);
    md.download = ptr::null_mut();

    let r = &*result;
    if r.bytes.is_null() {
        // Surface the spec MediaErrorCode meaningfully.
        let what = match r.error_code {
            1 => tr("Image too large"),
            2 => tr("Unsupported image format"),
            3 => tr("Rate limited — try again shortly"),
            4 => tr("Not authorised to view this image"),
            5 => tr("Server temporarily busy"),
            _ => tr("Failed to load image"),
        };
        if !r.error_message.is_null() && r.error_message_len > 0 {
            let n = r.error_message_len.min(200);
            let extra = std::slice::from_raw_parts(r.error_message as *const u8, n);
            let msg = format!("{}\n{}", what, String::from_utf8_lossy(extra));
            swap_to_error(md, &msg);
        } else {
            swap_to_error(md, &what);
        }
        return;
    }

    // Copy the borrowed canonical bytes into an owned glib::Bytes. GLib APIs
    // routinely represent an empty buffer as (NULL, 0), and from_raw_parts is
    // UB on a NULL/dangling base or a len past isize::MAX — so guard those
    // into an empty slice rather than trust the C-side GByteArray blindly.
    let ga = &*r.bytes;
    let slice: &[u8] = if ga.data.is_null()
        || ga.len == 0
        || ga.len as u64 > isize::MAX as u64
    {
        &[]
    } else {
        std::slice::from_raw_parts(ga.data, ga.len as usize)
    };
    let bytes = glib::Bytes::from(slice);
    md.mime = if r.canonical_mime.is_null() {
        None
    } else {
        Some(cstr(r.canonical_mime))
    };
    md.bytes = Some(bytes);

    // Kick off the async glycin decode. The pointer is consumed synchronously
    // (copied into a glib::Bytes on the Rust side), so borrowing md.bytes for
    // the call is safe. A NULL token means glycin rejected synchronously and
    // on_decode_done already fired.
    let caps = HxInlineMediaCaps::default();
    let data: &[u8] = md.bytes.as_ref().unwrap();
    md.decode_token = inline_media_decode_async(
        data.as_ptr(),
        data.len(),
        &caps,
        on_decode_done,
        user_data,
    );
}

// ---------------------------------------------------------------------
// Glycin decode callback (main thread).
// ---------------------------------------------------------------------

unsafe extern "C" fn on_decode_done(decoded: *mut Decoded, user_data: *mut c_void) {
    let md = &mut *(user_data as *mut MediaDialog);

    // Consume the cancel-token regardless of outcome — the Rust side handed us
    // a strong Rc ref and we must drop it (cancel-after-completion is a no-op
    // free).
    if !md.decode_token.is_null() {
        inline_media_decode_cancel(md.decode_token);
        md.decode_token = ptr::null_mut();
    }

    let d = &*decoded;
    if d.texture.is_null() {
        let reason = if d.error_message.is_null() {
            tr("unknown error")
        } else {
            cstr(d.error_message)
        };
        swap_to_error(md, &tr1("Image decoder rejected: %s", &reason));
        inline_media_decoded_free(decoded);
        return;
    }

    let texture: gtk::gdk::Texture = from_glib_none(d.texture);
    md.picture.set_paintable(Some(&texture));
    md.save_btn.set_sensitive(true);
    md.open_btn.set_sensitive(true);
    md.stack.set_visible_child_name("image");

    inline_media_decoded_free(decoded);
}

// ---------------------------------------------------------------------
// Save As / Open Externally handlers.
// ---------------------------------------------------------------------

fn parent_window(md: &MediaDialog) -> Option<gtk::Window> {
    md.dialog.root().and_downcast::<gtk::Window>()
}

fn on_save_clicked(md: &MediaDialog) {
    let Some(bytes) = md.bytes.clone() else {
        return;
    };
    let fd = gtk::FileDialog::new();
    fd.set_title(&tr("Save Image"));
    fd.set_initial_name(Some(&format!("image{}", mime_to_suffix(md.mime.as_deref()))));

    // Own-lifetime payload: the closure keeps its own glib::Bytes ref, so a
    // dismissed parent dialog can't free it out from under the file chooser.
    fd.save(
        parent_window(md).as_ref(),
        gio::Cancellable::NONE,
        move |res| match res {
            Ok(file) => {
                let data: &[u8] = &bytes;
                // Synchronous write — the payload is <= MAX_BYTES (256 KiB).
                if let Err(e) = file.replace_contents(
                    data,
                    None,
                    false,
                    gio::FileCreateFlags::NONE,
                    gio::Cancellable::NONE,
                ) {
                    glib::g_debug!("gtkhx", "inline-media save-as write failed: {e}");
                }
            }
            Err(e) => {
                if !e.matches(gtk::DialogError::Dismissed) {
                    glib::g_debug!("gtkhx", "inline-media save-as cancel/error: {e}");
                }
            }
        },
    );
}

fn on_open_clicked(md: &MediaDialog) {
    let Some(bytes) = md.bytes.clone() else {
        return;
    };

    let mut dir = glib::user_runtime_dir();
    if dir.as_os_str().is_empty() {
        dir = glib::tmp_dir();
    }
    let suffix = mime_to_suffix(md.mime.as_deref());

    // Exclusive-create the temp file (O_CREAT|O_EXCL, 0600 via
    // FileCreateFlags::PRIVATE) to avoid the symlink / clobber race a
    // predictable name would expose. Retry a handful of times against the
    // (vanishingly unlikely) 64-bit random-name collision.
    let mut created: Option<gio::File> = None;
    for _ in 0..8 {
        let fname = format!(
            "gtkhx-media-{:08x}{:08x}{}",
            glib::random_int(),
            glib::random_int(),
            suffix
        );
        let file = gio::File::for_path(dir.join(&fname));
        match file.create(gio::FileCreateFlags::PRIVATE, gio::Cancellable::NONE) {
            Ok(out) => {
                let data: &[u8] = &bytes;
                let write_res = out.write_all(data, gio::Cancellable::NONE);
                let _ = out.close(gio::Cancellable::NONE);
                match write_res {
                    Ok((_written, _etag)) => created = Some(file),
                    Err(e) => {
                        glib::g_debug!(
                            "gtkhx",
                            "inline-media open-externally tempfile write failed: {e}"
                        );
                        // Don't leave a partial 0600 temp file behind.
                        let _ = file.delete(gio::Cancellable::NONE);
                    }
                }
                break;
            }
            Err(e) => {
                if !e.matches(gio::IOErrorEnum::Exists) {
                    glib::g_debug!(
                        "gtkhx",
                        "inline-media open-externally tempfile create failed: {e}"
                    );
                    break; // genuine failure — bail
                }
                // Existed — try again with a fresh random tail.
            }
        }
    }

    let Some(file) = created else {
        return;
    };

    let launcher = gtk::FileLauncher::new(Some(&file));
    launcher.launch(
        parent_window(md).as_ref(),
        gio::Cancellable::NONE,
        move |res| {
            if let Err(e) = res {
                glib::g_debug!("gtkhx", "inline-media open-externally failed: {e}");
            }
        },
    );
}

// ---------------------------------------------------------------------
// Public entry point (C ABI).
// ---------------------------------------------------------------------

/// `void inline_media_show_dialog(GtkWidget *parent_widget, struct htlc_conn
/// *htlc, const guint8 *media_id, gsize media_id_len, const char *mime,
/// guint32 width_hint, guint32 height_hint, guint32 bytes_hint)` — present the
/// click-to-view dialog and drive the async download + decode. Returns
/// immediately.
///
/// # Safety
/// `media_id` is valid for `media_id_len` bytes (borrowed only for the
/// synchronous `inline_media_download_start` call), `htlc` is a valid session
/// pointer, `parent_widget` a valid `GtkWidget`. Must run on the GTK main
/// thread.
#[no_mangle]
pub unsafe extern "C" fn inline_media_show_dialog(
    parent_widget: *mut gtk::ffi::GtkWidget,
    htlc: *mut c_void,
    media_id: *const u8,
    media_id_len: usize,
    _mime: *const c_char,
    width_hint: u32,
    height_hint: u32,
    bytes_hint: u32,
) {
    if media_id.is_null() || media_id_len == 0 {
        return;
    }
    crate::ensure_gtk_init();

    // AdwDialog with an AdwToolbarView (header + stack body).
    let dialog = adw::Dialog::new();
    dialog.set_title(&tr("Image"));
    dialog.set_content_width(720);
    dialog.set_content_height(540);

    let toolbar = adw::ToolbarView::new();

    let header = adw::HeaderBar::new();
    let save_btn = gtk::Button::with_label(&tr("Save As…"));
    save_btn.set_sensitive(false);
    header.pack_start(&save_btn);

    let open_btn = gtk::Button::with_label(&tr("Open Externally"));
    open_btn.set_sensitive(false);
    header.pack_end(&open_btn);
    toolbar.add_top_bar(&header);

    // Stack body: Loading / Image / Error.
    let stack = gtk::Stack::new();
    stack.set_vexpand(true);
    stack.set_hexpand(true);

    // Loading page — spinner + hint.
    let loading_box = gtk::Box::new(gtk::Orientation::Vertical, 12);
    loading_box.set_valign(gtk::Align::Center);
    loading_box.set_halign(gtk::Align::Center);
    let spinner = adw::Spinner::new();
    spinner.set_size_request(64, 64);
    loading_box.append(&spinner);
    let hint = if width_hint != 0 && height_hint != 0 && bytes_hint != 0 {
        tr_argv(
            "Loading %u×%u (%u KB)…",
            &[
                &width_hint.to_string(),
                &height_hint.to_string(),
                &(((bytes_hint as u64 + 1023) / 1024).to_string()),
            ],
        )
    } else if width_hint != 0 && height_hint != 0 {
        tr_argv(
            "Loading %u×%u…",
            &[&width_hint.to_string(), &height_hint.to_string()],
        )
    } else {
        tr("Loading…")
    };
    let loading_label = gtk::Label::new(Some(&hint));
    loading_label.add_css_class("dim-label");
    loading_box.append(&loading_label);
    stack.add_named(&loading_box, Some("loading"));

    // Image page — scrolled GtkPicture.
    let picture = gtk::Picture::new();
    picture.set_can_shrink(true);
    picture.set_content_fit(gtk::ContentFit::Contain);
    let scroll = gtk::ScrolledWindow::new();
    scroll.set_policy(gtk::PolicyType::Automatic, gtk::PolicyType::Automatic);
    scroll.set_child(Some(&picture));
    scroll.set_vexpand(true);
    scroll.set_hexpand(true);
    stack.add_named(&scroll, Some("image"));

    // Error page.
    let err_box = gtk::Box::new(gtk::Orientation::Vertical, 12);
    err_box.set_valign(gtk::Align::Center);
    err_box.set_halign(gtk::Align::Center);
    let error_label = gtk::Label::new(Some(&tr("Failed to load image")));
    error_label.set_wrap(true);
    error_label.add_css_class("dim-label");
    err_box.append(&error_label);
    stack.add_named(&err_box, Some("error"));

    stack.set_visible_child_name("loading");
    toolbar.set_content(Some(&stack));
    dialog.set_child(Some(&toolbar));

    // Heap-box the state; its raw pointer is the user_data threaded through
    // every callback (mirrors the C hx_media_dialog *).
    let md_ptr = Box::into_raw(Box::new(MediaDialog {
        dialog: dialog.clone(),
        stack,
        picture,
        error_label,
        save_btn: save_btn.clone(),
        open_btn: open_btn.clone(),
        download: ptr::null_mut(),
        decode_token: ptr::null_mut(),
        bytes: None,
        mime: None,
    }));

    // Button handlers deref the (still-live) state synchronously on click.
    save_btn.connect_clicked(move |_| unsafe { on_save_clicked(&*md_ptr) });
    open_btn.connect_clicked(move |_| unsafe { on_open_clicked(&*md_ptr) });

    // ::closed — cancel any in-flight download + decode (suppresses their
    // callbacks), then reclaim the box.
    dialog.connect_closed(move |_| unsafe {
        {
            let md = &mut *md_ptr;
            if !md.download.is_null() {
                inline_media_download_cancel(md.download);
                md.download = ptr::null_mut();
            }
            if !md.decode_token.is_null() {
                inline_media_decode_cancel(md.decode_token);
                md.decode_token = ptr::null_mut();
            }
        }
        drop(Box::from_raw(md_ptr));
    });

    // Kick off the download. On synchronous reject, swap straight to error.
    let dl = inline_media_download_start(
        htlc,
        media_id,
        media_id_len,
        on_download_done,
        md_ptr as *mut c_void,
    );
    (*md_ptr).download = dl;
    if dl.is_null() {
        swap_to_error(&*md_ptr, &tr("Inline media unavailable on this server"));
    }

    let parent: Option<gtk::Widget> = if parent_widget.is_null() {
        None
    } else {
        Some(from_glib_none(parent_widget))
    };
    dialog.present(parent.as_ref());
}
