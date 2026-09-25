//! User-list voice-indicator column (ported from `src/users_voice_col.c`).
//!
//! Exports `gtkhx_users_voice_column_new`, which `users_view.rs` appends
//! between the UID and Name columns. Behind the crate's `voice` feature it
//! builds a `GtkColumnViewColumn` whose signal factory renders one small
//! icon per row reflecting the user's `HxVoiceModel` indicator state (NONE /
//! IN_VOICE / SPEAKING / MUTED). With the feature off (voice compiled out of
//! the app) it's a stub returning NULL, so `users_view.rs` just skips it —
//! same contract the C `#ifdef HAVE_VOICE` stub had.
//!
//! The voice model itself (`voice_model.c`) and the session accessor
//! (`voice_bridge.c`) stay C; this module reaches them through the FFI seam.

#![cfg_attr(not(feature = "voice"), allow(unused_imports))]

use std::ffi::c_void;

use gtk4 as gtk;

/// Opaque C `session *`.
type Session = c_void;

// ---------------------------------------------------------------------
// Feature off: NULL stub (users_view.rs skips a NULL column).
// ---------------------------------------------------------------------

#[cfg(not(feature = "voice"))]
#[no_mangle]
pub extern "C" fn gtkhx_users_voice_column_new(
    _sess: *mut Session,
) -> *mut gtk::ffi::GtkColumnViewColumn {
    std::ptr::null_mut()
}

// ---------------------------------------------------------------------
// Feature on: the real voice-indicator column.
// ---------------------------------------------------------------------

#[cfg(feature = "voice")]
mod voice_impl {
    use super::*;

    use std::cell::Cell;
    use std::ffi::c_char;

    use glib::translate::{from_glib_none, ToGlibPtr};
    use gtk::glib;
    use gtk::prelude::*;

    /// qdata key planting the bound `GtkListItem` on the cell so the
    /// right-click handler in `users_view.rs` can recover the row. Must use
    /// the raw C `g_object_set_data` API — shared verbatim with the uid /
    /// name cells (see `users_view.rs::stash_list_item`), and gtk-rs
    /// `set_data` (which boxes the value) is NOT interchangeable with it.
    const LIST_ITEM_KEY: &[u8] = b"user-list-item\0";

    /// Per-cell key for our own `VoiceCellData` (this cell's private state —
    /// safe to use the gtk-rs boxed `set_data`/`data` here since only Rust
    /// reads it back).
    const CELL_DATA_KEY: &str = "voice-cell-data";

    extern "C" {
        /// voice_bridge.c — sess->voice_model (HxVoiceModel GObject), NULL if
        /// the session has none.
        fn hx_session_voice_model(sess: *mut c_void) -> *mut c_void;
        /// voice_model.c — computed indicator for a uid (0 NONE / 1 IN_VOICE
        /// / 2 SPEAKING / 3 MUTED).
        fn hx_voice_model_get_indicator(model: *mut c_void, uid: u16) -> u32;
        /// voice_model.c — HX_VOICE_VIDEO_* flags for a uid (0 when it
        /// publishes nothing).
        fn hx_voice_model_get_video(model: *mut c_void, uid: u16) -> u32;
    }

    const VIDEO_CAMERA: u32 = 1 << 0;
    const VIDEO_SCREEN: u32 = 1 << 1;
    const VIDEO_CAMERA_PAUSED: u32 = 1 << 2;
    const VIDEO_SCREEN_PAUSED: u32 = 1 << 3;

    /// Per-cell subscription state. The cell holds a strong ref to the model
    /// so the disconnect at finalize is always safe (cells outlive
    /// bind/unbind — GtkColumnView recycles widgets), and its Drop tears the
    /// subscription down, mirroring the C `voice_cell_data_free` /
    /// `g_object_set_data_full` pair.
    struct VoiceCellData {
        model: glib::Object,
        handler: Option<glib::SignalHandlerId>,
        video_handler: Option<glib::SignalHandlerId>,
        /// The speaker glyph and, beside it, the camera or screen glyph.
        voice: gtk::Image,
        video: gtk::Image,
        /// Currently-bound row uid; 0 when unbound. The signal closure reads
        /// this to filter frames for other rows.
        uid: Cell<u16>,
    }

    impl Drop for VoiceCellData {
        fn drop(&mut self) {
            if let Some(id) = self.handler.take() {
                self.model.disconnect(id);
            }
            if let Some(id) = self.video_handler.take() {
                self.model.disconnect(id);
            }
        }
    }

    fn stash_list_item(cell: &gtk::Widget, item: &gtk::ListItem) {
        let key = LIST_ITEM_KEY.as_ptr() as *const c_char;
        let ip = item.as_ptr() as glib::ffi::gpointer;
        unsafe {
            glib::gobject_ffi::g_object_set_data(cell.as_ptr() as *mut _, key, ip);
        }
    }

    /// Map the indicator enum to a stock symbolic icon, or None for NONE.
    fn indicator_icon(ind: u32) -> Option<&'static str> {
        match ind {
            1 => Some("audio-volume-low-symbolic"), // IN_VOICE — dim speaker
            2 => Some("audio-volume-high-symbolic"), // SPEAKING
            3 => Some("microphone-disabled-symbolic"), // MUTED
            _ => None,                              // NONE / unknown
        }
    }

    fn voice_cell_refresh(img: &gtk::Image, model: &glib::Object, uid: u16) {
        let ind = unsafe { hx_voice_model_get_indicator(model.as_ptr() as *mut c_void, uid) };
        match indicator_icon(ind) {
            Some(name) => {
                img.set_icon_name(Some(name));
                img.set_visible(true);
            }
            None => {
                img.clear();
                img.set_visible(false);
            }
        }
        // SPEAKING gets Adwaita's `.accent`; every other state `.dim-label`.
        // Applied additively (remove the other, add this one) so the right
        // one wins the cascade — same as the C original.
        if ind == 2 {
            img.remove_css_class("dim-label");
            img.add_css_class("accent");
        } else {
            img.remove_css_class("accent");
            img.add_css_class("dim-label");
        }
    }

    /// Show who is publishing, whether or not anyone here is watching:
    /// the spec asks clients to, so a voice-only participant can at least
    /// know a camera is on. A screen share outranks a camera; paused
    /// publications are drawn dim.
    fn video_cell_refresh(img: &gtk::Image, model: &glib::Object, uid: u16) {
        let f = unsafe { hx_voice_model_get_video(model.as_ptr() as *mut c_void, uid) };
        let shown = if f & VIDEO_SCREEN != 0 {
            Some((
                "screen-shared-symbolic",
                f & VIDEO_SCREEN_PAUSED != 0,
                crate::tr::tr("Sharing their screen"),
            ))
        } else if f & VIDEO_CAMERA != 0 {
            Some((
                "camera-video-symbolic",
                f & VIDEO_CAMERA_PAUSED != 0,
                crate::tr::tr("Camera on"),
            ))
        } else {
            None
        };
        match shown {
            Some((icon, paused, tip)) => {
                img.set_icon_name(Some(icon));
                img.set_tooltip_text(Some(&tip));
                img.set_visible(true);
                if paused {
                    img.add_css_class("dim-label");
                } else {
                    img.remove_css_class("dim-label");
                }
            }
            None => {
                img.clear();
                img.set_visible(false);
            }
        }
    }

    /// Recover this cell's `VoiceCellData` (installed at setup). None when the
    /// session had no voice model.
    fn cell_data(cell: &gtk::Box) -> Option<&VoiceCellData> {
        unsafe {
            cell.data::<VoiceCellData>(CELL_DATA_KEY)
                .map(|p| p.as_ref())
        }
    }

    pub(super) unsafe fn build_column(sess: *mut c_void) -> *mut gtk::ffi::GtkColumnViewColumn {
        if sess.is_null() {
            return std::ptr::null_mut();
        }
        // Fetched once; the model is session-lifetime. May be NULL — then the
        // factory is a no-op and every cell stays empty + invisible, so a
        // server without voice shows nothing here.
        let model_ptr = hx_session_voice_model(sess);

        let factory = gtk::SignalListItemFactory::new();

        factory.connect_setup(move |_f, item| {
            let item = item.downcast_ref::<gtk::ListItem>().unwrap();
            let img = gtk::Image::new();
            img.set_pixel_size(12);
            img.set_halign(gtk::Align::Center);
            img.set_valign(gtk::Align::Center);
            img.add_css_class("dim-label");
            img.set_visible(false); // hidden until a bind reveals an indicator
            let vimg = gtk::Image::new();
            vimg.set_pixel_size(12);
            vimg.set_valign(gtk::Align::Center);
            vimg.set_visible(false);
            let cell = gtk::Box::new(gtk::Orientation::Horizontal, 2);
            cell.set_halign(gtk::Align::Center);
            cell.append(&img);
            cell.append(&vimg);

            if !model_ptr.is_null() {
                let model: glib::Object =
                    unsafe { from_glib_none(model_ptr as *mut glib::gobject_ffi::GObject) };
                let cell_weak = cell.downgrade();
                // Fires whenever ANY uid's indicator flips; this cell only
                // cares about its currently-bound row.
                let handler = model.connect_local("indicator-changed", false, move |args| {
                    let changed = args.get(1).and_then(|v| v.get::<u32>().ok()).unwrap_or(0) as u16;
                    if let Some(cell) = cell_weak.upgrade() {
                        if let Some(data) = cell_data(&cell) {
                            let bound = data.uid.get();
                            if bound != 0 && bound == changed {
                                voice_cell_refresh(&data.voice, &data.model, bound);
                            }
                        }
                    }
                    None
                });
                let cell_weak = cell.downgrade();
                let video_handler = model.connect_local("video-changed", false, move |args| {
                    let changed = args.get(1).and_then(|v| v.get::<u32>().ok()).unwrap_or(0) as u16;
                    if let Some(cell) = cell_weak.upgrade() {
                        if let Some(data) = cell_data(&cell) {
                            let bound = data.uid.get();
                            if bound != 0 && bound == changed {
                                video_cell_refresh(&data.video, &data.model, bound);
                            }
                        }
                    }
                    None
                });
                let data = VoiceCellData {
                    model,
                    handler: Some(handler),
                    video_handler: Some(video_handler),
                    voice: img.clone(),
                    video: vimg.clone(),
                    uid: Cell::new(0),
                };
                unsafe { cell.set_data(CELL_DATA_KEY, data) };
            }

            item.set_child(Some(&cell));
        });

        factory.connect_bind(move |_f, item| {
            let item = item.downcast_ref::<gtk::ListItem>().unwrap();
            let Some(cell) = item.child().and_downcast::<gtk::Box>() else {
                return;
            };
            stash_list_item(cell.upcast_ref(), item);
            let uid = item
                .item()
                .map(|row| unsafe {
                    crate::user_row::hx_user_row_get_uid(row.as_ptr() as *mut c_void)
                })
                .unwrap_or(0);
            if let Some(data) = cell_data(&cell) {
                data.uid.set(uid);
                voice_cell_refresh(&data.voice, &data.model, uid);
                video_cell_refresh(&data.video, &data.model, uid);
            }
        });

        factory.connect_unbind(move |_f, item| {
            let item = item.downcast_ref::<gtk::ListItem>().unwrap();
            let Some(cell) = item.child().and_downcast::<gtk::Box>() else {
                return;
            };
            if let Some(data) = cell_data(&cell) {
                data.uid.set(0);
                for img in [&data.voice, &data.video] {
                    img.clear();
                    img.set_visible(false);
                }
            }
        });

        // Header glyph: a single speaker emoji as a compact column label.
        let col = gtk::ColumnViewColumn::new(
            Some("\u{1F508}"),
            Some(factory.upcast::<gtk::ListItemFactory>()),
        );
        // Room for the speaker and a camera beside it.
        col.set_fixed_width(36);
        col.set_resizable(false);
        // Transfer full — users_view.rs consumes the ref via from_glib_full.
        ToGlibPtr::to_glib_full(&col)
    }
}

/// # Safety
/// `sess` must be a non-null pointer to a live `Session`; it is borrowed
/// only for the duration of the call to build the voice-indicator column.
#[cfg(feature = "voice")]
#[no_mangle]
pub unsafe extern "C" fn gtkhx_users_voice_column_new(
    sess: *mut Session,
) -> *mut gtk::ffi::GtkColumnViewColumn {
    crate::ensure_gtk_init();
    voice_impl::build_column(sess)
}
