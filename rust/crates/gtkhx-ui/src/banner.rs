//! Server banner surface + fetch state machines (ported from `src/banner.c`
//! and `src/banner_dispatch.c`).
//!
//! Owns the banner widget docked at the bottom of the toolbar window (a
//! `GtkPicture` + a dim caption that doubles as a URL affordance / fallback),
//! plus the two fetch modes:
//!
//!   - **URL mode**: a Rust hxnet banner fetch (blocking ureq GET on the tokio
//!     blocking pool), drained on a main-loop timeout, then decoded and shown.
//!   - **File (HTXF) mode**: send `HTLC_HDR_DOWNLOAD_BANNER`; the reply's
//!     ref/size drive an HTXF subchannel fetch on the tokio blocking pool, then
//!     the bytes are decoded and shown. A fetch-generation counter drops stale
//!     completions after `banner_clear`.
//!
//! Each connection fetches its own, so two servers logging in at once each
//! get their banner; the one row shows the focused connection's.
//!
//! What's native Rust here (no C round-trip): the image decode
//! ([`hx_image_decode::decode_first_frame_async`] → a `gdk::Texture` directly),
//! the connection-state snapshot ([`gtkhx_core::conn`] accessors), and the TLS
//! subchannel TOFU verify ([`hxtls_trust::ffi::verify_cert`]). What stays on the
//! C ABI: the hxnet fetch / HTXF transport + the tokio worker-spawn bridge
//! (hxnet/hxbridge are deliberately reached over the C ABI — see the crate
//! notes), the SOCKS lookup, and the request, `hx_banner_get` (a
//! gtkhx-ui↔hxhandlers Cargo cycle blocks importing it), whose reply comes
//! back through `banner_handle_htxf_reply`.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_void};

use glib::translate::IntoGlibPtr;

use crate::dock;
use gtk::gio;
use gtk::glib;
use gtk::prelude::*;
use gtk4 as gtk;

use gtkhx_core::conn::{hx_conn_hope_aead, hx_conn_serverhost, hx_conn_serverport, hx_conn_tls};
use hx_image_decode::ffi::HxInlineMediaCaps;
use hx_image_decode::{decode_first_frame_async, ImageDecodeHandle, ImageDecodeOutcome};

use crate::ensure_gtk_init;
use crate::tr::{tr, tr1};

// ------------------------------------------------------------------- //
// Constants (mirror src/banner.c + banner_dispatch.h + hotline.h).

/// Decode ceiling on each banner axis. Most servers ship 468×60.
const BANNER_MAX_W: u32 = 600;
/// Display box for the banner. It is the header bar's title, so its height is
/// the header's content height: a stock 468×60 banner shows at about 265×34.
/// Width is capped too, so a wide banner can't push the header's buttons off.
const BANNER_SHOW_W: i32 = 360;
const BANNER_SHOW_H: i32 = 34;

/// Sanity ceiling on the file-mode banner size advertised by the server — 1 MiB
/// leaves headroom for unusual formats while stopping a hostile server from
/// talking us into a gigabyte allocation for a toolbar decoration.
const BANNER_MAX_HTXF_SIZE: u32 = 1024 * 1024;

/// `HTXF_TYPE_BANNER` (hotline.h) — routes the subchannel through a Mac-native
/// server's banner-send path.
const HTXF_TYPE_BANNER: u16 = 2;
/// `HX_HTXF_PREAMBLE_MAX_BYTES` (hotline.h) — 16-byte header + the 8-byte size64
/// tail; banner transfers only ever use the 16-byte form.
const HX_HTXF_PREAMBLE_MAX_BYTES: usize = 24;
/// Left mouse button.
const GDK_BUTTON_PRIMARY: u32 = 1;

// ------------------------------------------------------------------- //
// FFI: the fetch / send plumbing that stays on the C ABI.

/// Opaque hxnet URL-fetch handle (Rust `HxnetBannerFetch`).
#[repr(C)]
struct HxnetBannerFetch {
    _private: [u8; 0],
}
/// Opaque hxnet HTXF channel handle (Rust `HtxfConn`).
#[repr(C)]
struct HtxfConn {
    _private: [u8; 0],
}
/// Opaque HOPE control-channel AEAD material handle (Rust `HxnetHopeAead`).
#[repr(C)]
struct HxnetHopeAead {
    _private: [u8; 0],
}

/// `hxnet_banner_fetch_poll` result view (mirrors `HxnetBannerOut`).
#[repr(C)]
struct HxnetBannerOut {
    bytes_ptr: *const u8,
    bytes_len: usize,
    err_ptr: *const u8,
    err_len: usize,
}

const HXNET_BANNER_PENDING: c_int = 0;
const HXNET_BANNER_DONE: c_int = 1;
// HXNET_BANNER_ERROR == -1 (anything else).

/// hxnet HTXF TOFU verify callback shape.
type HtxfVerifyCb = unsafe extern "C" fn(*const u8, usize, *mut c_void) -> c_int;
/// The tokio blocking-pool worker / completion pair shape.
type BlockingFn = unsafe extern "C" fn(*mut c_void);

extern "C" {
    // hxnet URL fetch (banner_http.rs).
    fn hxnet_banner_fetch_open(url: *const u8, url_len: usize) -> *mut HxnetBannerFetch;
    fn hxnet_banner_fetch_poll(handle: *mut HxnetBannerFetch, out: *mut HxnetBannerOut) -> c_int;
    fn hxnet_banner_fetch_close(handle: *mut HxnetBannerFetch);

    // hxnet HTXF subchannel (htxf.rs).
    fn hxnet_htxf_pack_preamble(
        buf: *mut u8,
        cap: usize,
        ref_: u32,
        total_size: u64,
        type_: u16,
        flags: u16,
        size64: c_int,
    ) -> usize;
    fn hxnet_htxf_connect(
        host: *const u8,
        host_len: usize,
        port: u16,
        proxy_uri: *const u8,
        proxy_uri_len: usize,
        tls: c_int,
        preamble: *const u8,
        preamble_len: usize,
        hope_aead: *const HxnetHopeAead,
        xfer_ref: u32,
        verify_cert: Option<HtxfVerifyCb>,
        user_data: *mut c_void,
    ) -> *mut HtxfConn;
    fn hxnet_htxf_read_full(handle: *mut HtxfConn, buf: *mut u8, len: usize) -> isize;
    fn hxnet_htxf_close(handle: *mut HtxfConn);
    fn hxnet_hope_aead_clone(h: *const HxnetHopeAead) -> *mut HxnetHopeAead;
    fn hxnet_hope_aead_free(h: *mut HxnetHopeAead);

    // hxbridge — run a blocking worker on tokio's pool, post completion to the
    // GLib main loop.
    fn gtkhx_bridge_spawn_blocking_with_idle(
        worker: BlockingFn,
        completion: BlockingFn,
        user_data: *mut c_void,
    );

    // hxnet_bridge.c — SOCKS proxy lookup for the subchannel endpoint.
    fn hx_bridge_lookup_socks_proxy(host: *const c_char, port: u16) -> *mut c_char;

    // hxhandlers — the file-mode request, its reply answered through
    // banner_handle_htxf_reply below.
    fn hx_banner_get(htlc: *mut c_void);
}

// ------------------------------------------------------------------- //
// Module state — one banner *widget* per process, content and fetches per
// connection, main-thread only.

/// What a connection's banner says, kept apart from the widget that shows it.
///
/// The banner is a single row in the toolbar window's bottom bars, so — like
/// the status bar, the window title and the tray icon — it *follows the focus*
/// rather than existing once per connection. It used to be painted directly and
/// only ever painted, which at one connection is the same thing and at two is a
/// row showing whichever server most recently sent a banner, no matter which
/// tab you were looking at.
#[derive(Default)]
struct BannerContent {
    /// The decoded image, once there is one.
    texture: Option<gtk::gdk::Texture>,
    /// The caption: the URL before the image arrives, an error if it never
    /// does, and empty once the image is up (the picture says it better).
    caption: String,
    /// Cached for click-to-open and the tooltip. URL mode only.
    url: Option<String>,
}

impl BannerContent {
    /// Whether there is anything worth showing. An empty one hides the row.
    fn is_empty(&self) -> bool {
        self.texture.is_none() && self.caption.is_empty()
    }
}

struct BannerUi {
    root: gtk::Box,
    picture: gtk::Picture,
    caption: gtk::Label,
}

/// One connection's banner attempt. Its entry in `FETCHES` exists exactly
/// while the attempt runs — from the server's message until a result, success
/// or failure — so a result for a connection with no entry belongs to an
/// attempt that was cancelled, and is dropped.
#[derive(Default)]
struct Fetch {
    /// URL mode: the hxnet fetch and the timer draining it.
    url_fetch: Option<*mut HxnetBannerFetch>,
    url_drain_source: Option<glib::SourceId>,
    /// File mode: the generation the HTXF worker was spawned with; its
    /// completion is dropped unless the attempt still carries it.
    htxf_generation: u32,
    /// The image decode, either mode.
    decode_handle: Option<ImageDecodeHandle>,
}

impl Fetch {
    fn cancel(self) {
        if let Some(src) = self.url_drain_source {
            src.remove();
        }
        if let Some(h) = self.url_fetch {
            unsafe { hxnet_banner_fetch_close(h) };
        }
        if let Some(h) = self.decode_handle {
            h.cancel();
        }
    }
}

thread_local! {
    static BANNER: RefCell<Option<BannerUi>> = const { RefCell::new(None) };
    static CONTENT: RefCell<HashMap<dock::ConnKey, BannerContent>> =
        RefCell::new(HashMap::new());
    static FETCHES: RefCell<HashMap<dock::ConnKey, Fetch>> = RefCell::new(HashMap::new());
    /// Process-wide, so no two HTXF workers ever share a generation.
    static HTXF_GENERATION: Cell<u32> = const { Cell::new(0) };
}

/// Heap-boxed file-mode fetch, handed to the blocking worker via `user_data`.
/// The worker (off the main thread) fills `bytes`/`ok`; the completion (main
/// thread) reclaims + frees it.
struct HtxfFetch {
    conn: dock::ConnKey,
    ref_: u32,
    size: u32,
    generation: u32,
    /// Endpoint snapshot (subchannel port = control port + 1).
    serverhost: String,
    serverport: u16,
    tls: bool,
    /// Owned clone of the control channel's HOPE AEAD material (NULL unless
    /// ChaCha20-Poly1305 was negotiated). Freed on drop.
    hope_aead: *mut HxnetHopeAead,
    bytes: Vec<u8>,
    ok: bool,
}

impl Drop for HtxfFetch {
    fn drop(&mut self) {
        if !self.hope_aead.is_null() {
            unsafe { hxnet_hope_aead_free(self.hope_aead) };
            self.hope_aead = std::ptr::null_mut();
        }
    }
}

// ------------------------------------------------------------------- //
// Folded banner_dispatch: pure classification + validation.

/// Classify the 4-byte TYPE chunk as URL mode. Per the 1.9 spec `"URL "` (or
/// `"URL"`) means the server hands a URL and the client fetches; anything else
/// ("GIFf" / "JPEG" / "PICT" / …) is file mode over HTXF. Case-insensitive,
/// trailing-space-tolerant (mhxd pads `"URL "`, others NUL-terminate `"URL"`).
fn banner_type_is_url(type_: &str) -> bool {
    let trimmed = type_.split([' ', '\0']).next().unwrap_or("");
    trimmed.eq_ignore_ascii_case("URL")
}

/// Result of validating the file-mode reply's ref/size pair.
#[derive(Debug, PartialEq, Eq)]
enum HtxfValidation {
    Ok,
    ZeroRef,
    ZeroSize,
    TooLarge,
}

/// Validate the HTXF reply's ref + size. `Ok` iff the transfer is safe to spawn.
/// Order matches the old inline checks: ref/size pair first (server didn't
/// allocate a transfer), then the size ceiling (server wants an absurd alloc).
fn validate_htxf_reply(ref_: u32, size: u32) -> HtxfValidation {
    if ref_ == 0 {
        HtxfValidation::ZeroRef
    } else if size == 0 {
        HtxfValidation::ZeroSize
    } else if size > BANNER_MAX_HTXF_SIZE {
        HtxfValidation::TooLarge
    } else {
        HtxfValidation::Ok
    }
}

// ------------------------------------------------------------------- //
// Public API (C ABI).

/// `GtkWidget *banner_widget_new(void)` — build (once) the banner row and return
/// it for toolbar.c to embed. There is at most one per process.
///
/// # Safety
/// Must run on the GTK main thread.
#[no_mangle]
pub unsafe extern "C" fn banner_widget_new() -> *mut gtk::ffi::GtkWidget {
    ensure_gtk_init();

    // Already built (banner.c returns the existing widget) — hand back a
    // borrowed pointer; toolbar only embeds it once.
    if let Some(ptr) = BANNER.with(|b| {
        b.borrow()
            .as_ref()
            .map(|ui| ui.root.upcast_ref::<gtk::Widget>().as_ptr())
    }) {
        return ptr;
    }

    // A link when the server gave a URL (see `repaint`), so it takes focus
    // and Enter opens it, as a click does.
    let root = gtk::Box::builder()
        .orientation(gtk::Orientation::Horizontal)
        .spacing(8)
        .accessible_role(gtk::AccessibleRole::Link)
        .build();
    root.add_css_class("gtkhx-banner-link");
    root.update_property(&[gtk::accessible::Property::Label(&tr("Server banner"))]);
    root.set_margin_start(6);
    root.set_margin_end(6);
    root.set_visible(false);

    // GtkPicture (not GtkImage — which caps natural size at the icon-size CSS
    // var): renders at the paintable's natural size, bounded via size request.
    let picture = gtk::Picture::new();
    picture.set_size_request(-1, BANNER_SHOW_H);
    picture.set_can_shrink(true);
    picture.add_css_class("gtkhx-header-banner");
    // The picture is the server's name, drawn; say so to screen readers.
    picture.update_property(&[gtk::accessible::Property::Label(&tr("Server banner"))]);
    picture.set_overflow(gtk::Overflow::Hidden);
    picture.set_content_fit(gtk::ContentFit::Contain);
    picture.set_visible(false);
    root.append(&picture);

    let caption = gtk::Label::new(None);
    caption.set_xalign(0.0);
    caption.set_ellipsize(gtk::pango::EllipsizeMode::End);
    caption.set_hexpand(true);
    caption.add_css_class("dim-label");
    root.append(&caption);

    // Click anywhere on the row → open the cached URL in the default browser.
    let click = gtk::GestureClick::new();
    click.set_button(GDK_BUTTON_PRIMARY);
    click.connect_pressed(move |gesture, _n, _x, _y| {
        if let Some(w) = gesture.widget() {
            open_banner_url(&w);
        }
    });
    root.add_controller(click);
    let keys = gtk::EventControllerKey::new();
    keys.connect_key_pressed(|c, key, _, _| {
        use gtk::gdk::Key;
        match c.widget() {
            Some(w) if matches!(key, Key::Return | Key::KP_Enter | Key::space) => {
                open_banner_url(&w);
                glib::Propagation::Stop
            }
            _ => glib::Propagation::Proceed,
        }
    });
    root.add_controller(keys);

    BANNER.with(|b| {
        *b.borrow_mut() = Some(BannerUi {
            root: root.clone(),
            picture,
            caption,
        });
    });

    // Hand the (still-floating) original to C; toolbar's gtk_box_append sinks it.
    root.upcast::<gtk::Widget>().into_glib_ptr()
}

/// `void banner_handle_message(struct htlc_conn *htlc, const char *type,
/// gboolean has_url, const char *url)` — dispatch an HTLS_HDR_BANNER receive.
///
/// # Safety
/// `type` / `url` are valid C strings (or NULL); `htlc` a live session pointer.
/// Main thread only.
#[no_mangle]
pub unsafe extern "C" fn banner_handle_message(
    htlc: *mut c_void,
    type_: *const c_char,
    has_url: glib::ffi::gboolean,
    url: *const c_char,
) {
    if BANNER.with(|b| b.borrow().is_none()) {
        return;
    }

    // A new banner replaces this connection's last, and any attempt of its
    // still running; another connection's is none of its business.
    let key = dock::conn_key(htlc);
    cancel_fetch(key);
    FETCHES.with_borrow_mut(|f| f.insert(key, Fetch::default()));
    CONTENT.with(|c| {
        c.borrow_mut().insert(key, BannerContent::default());
    });

    let type_str = if type_.is_null() {
        String::new()
    } else {
        CStr::from_ptr(type_).to_string_lossy().into_owned()
    };

    // Dispatch on TYPE (authoritative), not on URL-chunk presence.
    if banner_type_is_url(&type_str) {
        let url_str = if has_url != glib::ffi::GFALSE && !url.is_null() {
            let s = CStr::from_ptr(url).to_string_lossy().into_owned();
            (!s.is_empty()).then_some(s)
        } else {
            None
        };
        match url_str {
            Some(u) => {
                let cached = u.clone();
                with_content(key, move |c| {
                    c.url = Some(cached.clone());
                    c.caption = cached;
                });
                start_url_fetch(key, &u);
            }
            None => {
                // URL mode advertised but no URL — server misconfigured.
                finish_caption(key, &tr("Server banner: URL mode without URL"));
            }
        }
        return;
    }

    // File mode: TYPE is a binary image-format tag. Ask the server for the
    // bytes over HTXF; show a loading caption meanwhile.
    let loading = tr1("Server banner [%s] — loading…", &type_str);
    show_caption(key, &loading);
    send_download_request(htlc);
}

/// Abandon `conn`'s attempt, if one is running. Out of the map first: the
/// cancels can run GLib code that re-enters it.
fn cancel_fetch(conn: dock::ConnKey) {
    if let Some(f) = FETCHES.with_borrow_mut(|f| f.remove(&conn)) {
        f.cancel();
    }
}

/// `void banner_clear(struct htlc_conn *htlc)` — forget this connection's
/// banner and cancel any fetch still running for it.
///
/// Per connection: disconnecting one server must not blank the banner of
/// another that is still up. It takes the connection rather than clearing
/// whatever is on screen for the same reason the paint path does — the caller
/// is `hx_htlc_close`, which can be closing a connection the user isn't
/// looking at.
///
/// # Safety
/// `htlc` is a `struct htlc_conn *` or NULL. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn banner_clear(htlc: *mut c_void) {
    let key = dock::conn_key(htlc);
    cancel_fetch(key);
    CONTENT.with(|c| c.borrow_mut().remove(&key));
    paint_active();
}

/// `void banner_handle_htxf_reply(struct htlc_conn *htlc, guint32 ref, guint32
/// size)` — continuation of the file-mode flow: the server's reply carries a
/// transfer ref + size; spin up the HTXF worker to fetch the bytes.
///
/// # Safety
/// `htlc` is a live session pointer. Main thread only.
#[no_mangle]
pub unsafe extern "C" fn banner_handle_htxf_reply(htlc: *mut c_void, ref_: u32, size: u32) {
    if htlc.is_null() || BANNER.with(|b| b.borrow().is_none()) {
        return;
    }

    // Only while this connection's attempt is still running: a clear or a
    // newer banner between the request and the reply has ended it.
    let key = dock::conn_key(htlc);
    if !FETCHES.with_borrow(|f| f.contains_key(&key)) {
        return;
    }

    match validate_htxf_reply(ref_, size) {
        HtxfValidation::Ok => {}
        HtxfValidation::ZeroRef | HtxfValidation::ZeroSize => {
            finish_caption(key, &tr("Server banner: empty transfer"));
            return;
        }
        HtxfValidation::TooLarge => {
            finish_caption(key, &tr("Server banner: image too large"));
            return;
        }
    }

    // Snapshot the endpoint + bump the generation on the main thread.
    let host_ptr = hx_conn_serverhost(htlc.cast());
    let serverhost = if host_ptr.is_null() {
        String::new()
    } else {
        CStr::from_ptr(host_ptr).to_string_lossy().into_owned()
    };
    let serverport = hx_conn_serverport(htlc.cast()).wrapping_add(1);
    let tls = hx_conn_tls(htlc.cast()) != 0;
    // Clone the control channel's HOPE material (decoupled from htlc's lifetime —
    // the worker can outlive a disconnect that frees htlc->hope_aead).
    let hope_aead = hxnet_hope_aead_clone(hx_conn_hope_aead(htlc.cast()) as *const HxnetHopeAead);

    let generation = HTXF_GENERATION.get().wrapping_add(1);
    HTXF_GENERATION.set(generation);
    FETCHES.with_borrow_mut(|f| {
        if let Some(f) = f.get_mut(&key) {
            f.htxf_generation = generation;
        }
    });

    let fetch = Box::new(HtxfFetch {
        conn: key,
        ref_,
        size,
        generation,
        serverhost,
        serverport,
        tls,
        hope_aead,
        bytes: vec![0u8; size as usize],
        ok: false,
    });

    // Hand off to tokio's blocking pool; completion posts back to the main loop.
    gtkhx_bridge_spawn_blocking_with_idle(
        htxf_worker,
        htxf_completion,
        Box::into_raw(fetch) as *mut c_void,
    );
}

// ------------------------------------------------------------------- //
// Internals.

/// Amend `conn`'s banner content while its attempt runs, and repaint if that
/// is the connection the user is looking at.
///
/// Every result path goes through here rather than touching the widgets, so a
/// background connection's fetch finishing updates what its tab will show
/// without disturbing the tab that is up.
fn with_content(conn: dock::ConnKey, f: impl FnOnce(&mut BannerContent)) {
    // A result for an attempt that has ended was cancelled: its connection
    // closed or sent a newer banner, and writing it would put it back.
    if !FETCHES.with_borrow(|f| f.contains_key(&conn)) {
        return;
    }
    CONTENT.with(|c| {
        if let Some(content) = c.borrow_mut().get_mut(&conn) {
            f(content);
        }
    });
    if conn == dock::active_key() {
        paint_active();
    }
}

/// Record a result and end `conn`'s attempt: nothing more is coming for it.
fn finish_content(conn: dock::ConnKey, f: impl FnOnce(&mut BannerContent)) {
    with_content(conn, f);
    // Ended, not cancelled: whatever was running has run.
    FETCHES.with_borrow_mut(|f| f.remove(&conn));
}

/// A caption that is the end of the attempt — an error, or a banner that turned
/// out to be nothing.
fn finish_caption(conn: dock::ConnKey, text: &str) {
    let text = text.to_owned();
    finish_content(conn, move |c| c.caption = text);
}

/// A caption while the attempt is still running: the URL before its image
/// lands, or "loading…".
fn show_caption(conn: dock::ConnKey, text: &str) {
    let text = text.to_owned();
    with_content(conn, move |c| c.caption = text);
}

/// Paint `content` (or blank, for a connection that has none) onto the one
/// banner row. Pins the picture allocation to the image scaled into
/// BANNER_SHOW_W × BANNER_SHOW_H, aspect preserved, so the header doesn't
/// reflow and the picture carries no letterbox. With an image up, the caption
/// (the URL) moves to the tooltip: the header has no room for both.
fn repaint(ui: &BannerUi, content: Option<&BannerContent>) {
    let Some(content) = content.filter(|c| !c.is_empty()) else {
        ui.picture.set_paintable(gtk::gdk::Paintable::NONE);
        ui.picture.set_visible(false);
        ui.caption.set_text("");
        ui.root.set_tooltip_text(None);
        ui.root.set_visible(false);
        return;
    };

    match &content.texture {
        Some(tex) => {
            let (w, h) = banner_display_size(tex.width(), tex.height());
            ui.picture
                .set_paintable(Some(&scaled::ScaledPaintable::new(tex, w, h)));
            ui.picture.set_size_request(w, h);
            ui.picture.set_visible(true);
        }
        None => {
            // No image (still loading, or it failed): nothing to show in the
            // header's title slot, so the row stays hidden and the window
            // title and connection status keep it. A loading note or an
            // error there would push the status out for no picture.
            ui.picture.set_paintable(gtk::gdk::Paintable::NONE);
            ui.picture.set_visible(false);
            ui.caption.set_text(&content.caption);
            ui.root.set_tooltip_text(None);
            ui.root.set_visible(false);
            return;
        }
    }
    // With the image up the caption (the URL) moves to the tooltip: the
    // header has no room for both.
    ui.caption.set_visible(false);
    ui.caption.set_text(&content.caption);
    ui.root.set_tooltip_text(content.url.as_deref());
    ui.root
        .set_focusable(content.url.as_deref().is_some_and(|u| !u.is_empty()));
    ui.root.set_visible(true);
}

/// A texture drawn at a fixed size of our choosing.
///
/// A `GtkPicture` asks for its paintable's intrinsic size, and a size request
/// on the picture is only a floor, so a stock 468×60 banner handed over as-is
/// would stretch the header to 60 px. This reports the display size as the
/// intrinsic one and scales the texture into it when drawn.
mod scaled {
    use std::cell::{Cell, RefCell};

    use gtk::gdk;
    use gtk::glib;
    use gtk::prelude::*;
    use gtk::subclass::prelude::*;
    use gtk4 as gtk;

    glib::wrapper! {
        pub struct ScaledPaintable(ObjectSubclass<imp::ScaledPaintable>)
            @implements gdk::Paintable;
    }

    impl ScaledPaintable {
        pub fn new(texture: &gdk::Texture, width: i32, height: i32) -> Self {
            let obj: Self = glib::Object::new();
            obj.imp().texture.replace(Some(texture.clone()));
            obj.imp().size.set((width, height));
            obj
        }
    }

    mod imp {
        use super::*;

        #[derive(Default)]
        pub struct ScaledPaintable {
            pub texture: RefCell<Option<gdk::Texture>>,
            pub size: Cell<(i32, i32)>,
        }

        #[glib::object_subclass]
        impl ObjectSubclass for ScaledPaintable {
            const NAME: &'static str = "GtkhxScaledPaintable";
            type Type = super::ScaledPaintable;
            type Interfaces = (gdk::Paintable,);
        }

        impl ObjectImpl for ScaledPaintable {}

        impl PaintableImpl for ScaledPaintable {
            fn intrinsic_width(&self) -> i32 {
                self.size.get().0
            }

            fn intrinsic_height(&self) -> i32 {
                self.size.get().1
            }

            fn flags(&self) -> gdk::PaintableFlags {
                gdk::PaintableFlags::STATIC_SIZE | gdk::PaintableFlags::STATIC_CONTENTS
            }

            fn snapshot(&self, snapshot: &gdk::Snapshot, width: f64, height: f64) {
                if let Some(texture) = self.texture.borrow().as_ref() {
                    texture.snapshot(snapshot, width, height);
                }
            }
        }
    }
}

/// Scale an image of `w` × `h` down (never up) into the display box, keeping
/// its aspect ratio. Degenerate sizes come back as the box.
fn banner_display_size(w: i32, h: i32) -> (i32, i32) {
    if w <= 0 || h <= 0 {
        return (BANNER_SHOW_W, BANNER_SHOW_H);
    }
    let scale = f64::min(
        1.0,
        f64::min(
            f64::from(BANNER_SHOW_W) / f64::from(w),
            f64::from(BANNER_SHOW_H) / f64::from(h),
        ),
    );
    let fit = |v: i32| ((f64::from(v) * scale).round() as i32).max(1);
    (fit(w), fit(h))
}

/// Show the focused connection's banner. The tab-switch entry point, and the
/// tail of every content update for the connection that is up.
fn paint_active() {
    let key = dock::active_key();
    BANNER.with(|b| {
        if let Some(ui) = b.borrow().as_ref() {
            CONTENT.with(|c| repaint(ui, c.borrow().get(&key)));
        }
    });
}

/// `void banner_show_active(void)` — repaint the banner for the connection the
/// user is now looking at. Called from `hx_chrome_refresh` alongside the rest
/// of the focus-following chrome.
///
/// # Safety
/// Main thread only.
#[no_mangle]
pub unsafe extern "C" fn banner_show_active() {
    paint_active();
}

/// Kick off the shared glycin decode for freshly-fetched bytes. Caps mirror
/// banner.c: 256 KiB max, BANNER_MAX_W each axis, single frame.
fn start_image_decode(conn: dock::ConnKey, bytes: &[u8]) {
    // Cancel any prior in-flight decode.
    if let Some(h) =
        FETCHES.with_borrow_mut(|f| f.get_mut(&conn).and_then(|f| f.decode_handle.take()))
    {
        h.cancel();
    }

    let caps = HxInlineMediaCaps {
        max_bytes: 256 * 1024,
        max_dimension: BANNER_MAX_W,
        max_pixels: BANNER_MAX_W * BANNER_MAX_W,
        max_frames: 1,
        max_duration_ms: 1,
    };

    let handle = decode_first_frame_async(bytes, caps, move |outcome| match outcome {
        ImageDecodeOutcome::Ok(tex) => finish_content(conn, move |c| {
            c.texture = Some(tex);
            // The image is up; the URL-as-caption is redundant now that
            // the tooltip and the click carry it.
            c.caption.clear();
        }),
        ImageDecodeOutcome::Err { .. } => {
            finish_caption(conn, &tr("Server banner: image not decodable"))
        }
    });

    // Store the handle for cancellation (on async decodes; a sync reject already
    // ran the closure and ended the attempt).
    FETCHES.with_borrow_mut(|f| {
        if let Some(f) = f.get_mut(&conn) {
            f.decode_handle = handle;
        }
    });
}

// URL-mode fetch --------------------------------------------------- //

fn start_url_fetch(conn: dock::ConnKey, url: &str) {
    unsafe {
        let handle = hxnet_banner_fetch_open(url.as_ptr(), url.len());
        if handle.is_null() {
            return; // bad URL — the caption (the URL itself) stays up
        }
        // 50 ms drain — a small banner image lands quickly.
        let source = glib::timeout_add_local(std::time::Duration::from_millis(50), move || {
            banner_url_drain(conn)
        });
        FETCHES.with_borrow_mut(|f| {
            if let Some(f) = f.get_mut(&conn) {
                f.url_fetch = Some(handle);
                f.url_drain_source = Some(source);
            }
        });
    }
}

/// Poll `conn`'s one-shot URL fetch. Returns whether to keep draining.
fn banner_url_drain(conn: dock::ConnKey) -> glib::ControlFlow {
    let Some(handle) = FETCHES.with_borrow(|f| f.get(&conn).and_then(|f| f.url_fetch)) else {
        return glib::ControlFlow::Break;
    };

    let mut out = HxnetBannerOut {
        bytes_ptr: std::ptr::null(),
        bytes_len: 0,
        err_ptr: std::ptr::null(),
        err_len: 0,
    };
    let rc = unsafe { hxnet_banner_fetch_poll(handle, &mut out) };

    if rc == HXNET_BANNER_PENDING {
        return glib::ControlFlow::Continue;
    }

    // One-shot: the attempt lets go of the handle and the source (which
    // removes itself by returning Break), so nothing cancels them twice.
    FETCHES.with_borrow_mut(|f| {
        if let Some(f) = f.get_mut(&conn) {
            f.url_fetch = None;
            f.url_drain_source = None;
        }
    });
    if rc == HXNET_BANNER_DONE {
        if !out.bytes_ptr.is_null() && out.bytes_len > 0 {
            // The decode copies synchronously, so it's safe to close after.
            let bytes = unsafe { std::slice::from_raw_parts(out.bytes_ptr, out.bytes_len) };
            start_image_decode(conn, bytes);
        } else {
            finish_caption(conn, &tr("Server banner: empty response"));
        }
    } else {
        finish_caption(conn, &tr("Server banner: fetch failed"));
    }
    unsafe { hxnet_banner_fetch_close(handle) };
    glib::ControlFlow::Break
}

// File (HTXF) mode ------------------------------------------------- //

fn send_download_request(htlc: *mut c_void) {
    if !htlc.is_null() {
        unsafe { hx_banner_get(htlc) };
    }
}

/// TLS TOFU trampoline for the banner HTXF subchannel — hxnet calls this only
/// when WebPKI validation failed. Runs on the worker thread; `verify_cert` is
/// safe on any thread. `user_data` is the `HtxfFetch` (endpoint snapshot).
unsafe extern "C" fn banner_verify_cb(
    fp: *const u8,
    fp_len: usize,
    user_data: *mut c_void,
) -> c_int {
    if user_data.is_null() || fp.is_null() {
        return 0;
    }
    let f = &*(user_data as *const HtxfFetch);
    let fp_str = String::from_utf8_lossy(std::slice::from_raw_parts(fp, fp_len));
    hxtls_trust::ffi::verify_cert(&f.serverhost, f.serverport, &fp_str) as c_int
}

/// Blocking worker (tokio pool). Opens the subchannel, sends the preamble, reads
/// `size` bytes into the fetch buffer. No main-thread / GTK access here.
unsafe extern "C" fn htxf_worker(arg: *mut c_void) {
    let f = &mut *(arg as *mut HtxfFetch);

    let mut hdr = [0u8; HX_HTXF_PREAMBLE_MAX_BYTES];
    let hdr_len = hxnet_htxf_pack_preamble(
        hdr.as_mut_ptr(),
        hdr.len(),
        f.ref_,
        f.size as u64,
        HTXF_TYPE_BANNER,
        0,
        0,
    );
    if hdr_len == 0 {
        return;
    }

    // SOCKS lookup (same config the control channel + main transfers honour).
    let host_c = crate::cs(&f.serverhost);
    let proxy = hx_bridge_lookup_socks_proxy(host_c.as_ptr(), f.serverport);
    let (proxy_ptr, proxy_len) = if proxy.is_null() {
        (std::ptr::null(), 0usize)
    } else {
        (proxy as *const u8, CStr::from_ptr(proxy).to_bytes().len())
    };

    let hx = hxnet_htxf_connect(
        f.serverhost.as_ptr(),
        f.serverhost.len(),
        f.serverport,
        proxy_ptr,
        proxy_len,
        f.tls as c_int,
        hdr.as_ptr(),
        hdr_len,
        f.hope_aead,
        f.ref_,
        Some(banner_verify_cb),
        arg,
    );
    if !proxy.is_null() {
        glib::ffi::g_free(proxy as *mut c_void);
    }
    if hx.is_null() {
        return;
    }

    let want = f.size as usize;
    if hxnet_htxf_read_full(hx, f.bytes.as_mut_ptr(), want) == want as isize {
        f.ok = true;
    }
    hxnet_htxf_close(hx);
}

/// Completion (main thread). Drops stale results, else decodes + shows.
unsafe extern "C" fn htxf_completion(arg: *mut c_void) {
    let f = Box::from_raw(arg as *mut HtxfFetch); // reclaim; Drop frees hope_aead

    let current = FETCHES.with_borrow(|m| m.get(&f.conn).map(|a| a.htxf_generation));
    if current != Some(f.generation) {
        return; // the attempt ended (a clear, a close, a newer banner)
    }

    if !f.ok || f.bytes.is_empty() {
        finish_caption(f.conn, &tr("Server banner: HTXF fetch failed"));
        return;
    }

    // decode_first_frame_async copies the bytes synchronously, so `f` may drop
    // right after.
    start_image_decode(f.conn, &f.bytes);
}

// Click handler ---------------------------------------------------- //

fn open_banner_url(widget: &gtk::Widget) {
    // The visible banner belongs to the focused connection, so that is whose
    // URL a click on it opens.
    let key = dock::active_key();
    let url = CONTENT.with(|c| c.borrow().get(&key).and_then(|c| c.url.clone()));
    let Some(url) = url else {
        return;
    };
    if url.is_empty() {
        return;
    }
    let parent = widget.root().and_downcast::<gtk::Window>();
    let launcher = gtk::UriLauncher::new(&url);
    launcher.launch(parent.as_ref(), gio::Cancellable::NONE, |_res| {});
}

// ------------------------------------------------------------------- //

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn display_size_fits_the_header() {
        // The stock Hotline banner: height-bound.
        assert_eq!(banner_display_size(468, 60), (265, 34));
        // Already small enough: never upscaled.
        assert_eq!(banner_display_size(120, 30), (120, 30));
        // Very wide: width-bound.
        assert_eq!(banner_display_size(1200, 40), (360, 12));
        assert_eq!(banner_display_size(0, 60), (BANNER_SHOW_W, BANNER_SHOW_H));
    }

    #[test]
    fn type_is_url_matches_padded_and_terminated() {
        assert!(banner_type_is_url("URL "));
        assert!(banner_type_is_url("URL"));
        assert!(banner_type_is_url("url ")); // case-insensitive
        assert!(banner_type_is_url("URL\0extra"));
    }

    #[test]
    fn type_is_url_rejects_file_modes() {
        assert!(!banner_type_is_url("GIFf"));
        assert!(!banner_type_is_url("JPEG"));
        assert!(!banner_type_is_url("PICT"));
        assert!(!banner_type_is_url("")); // empty
        assert!(!banner_type_is_url("URLABCD")); // longer prefix must not match
    }

    #[test]
    fn type_is_url_defensive_edges() {
        // Leading space → the first (empty) segment isn't "URL" (the C helper
        // stopped copying at the first space too, so " URL" is not URL mode).
        assert!(!banner_type_is_url(" URL"));
        assert!(!banner_type_is_url("  URL "));
        // A long unterminated string that merely starts with "URL" must not
        // match (no space / NUL to trim on — the whole thing is compared).
        assert!(!banner_type_is_url("URLAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
        // And an arbitrarily long non-URL string is safely rejected, not
        // truncated into a match.
        assert!(!banner_type_is_url(&"X".repeat(1024)));
        // NUL-terminated "URL" (a server that NUL-pads the 4-byte code) still
        // classifies as URL mode.
        assert!(banner_type_is_url("URL\0GIFf"));
    }

    #[test]
    fn validate_htxf_reply_matrix() {
        assert_eq!(validate_htxf_reply(0, 100), HtxfValidation::ZeroRef);
        assert_eq!(validate_htxf_reply(5, 0), HtxfValidation::ZeroSize);
        assert_eq!(
            validate_htxf_reply(5, BANNER_MAX_HTXF_SIZE + 1),
            HtxfValidation::TooLarge
        );
        assert_eq!(validate_htxf_reply(5, 100), HtxfValidation::Ok);
        assert_eq!(
            validate_htxf_reply(5, BANNER_MAX_HTXF_SIZE),
            HtxfValidation::Ok
        );
        // ref checked before size.
        assert_eq!(validate_htxf_reply(0, 0), HtxfValidation::ZeroRef);
    }
}
