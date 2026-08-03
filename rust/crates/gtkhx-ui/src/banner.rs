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
//! What's native Rust here (no C round-trip): the image decode
//! ([`hx_image_decode::decode_first_frame_async`] → a `gdk::Texture` directly),
//! the connection-state snapshot ([`gtkhx_core::conn`] accessors), and the TLS
//! subchannel TOFU verify ([`hxtls_trust::ffi::verify_cert`]). What stays on the
//! C ABI: the hxnet fetch / HTXF transport + the tokio worker-spawn bridge
//! (hxnet/hxbridge are deliberately reached over the C ABI — see the crate
//! notes), the send primitive + task table (`hlwrite_chunks` / `task_new`, the
//! latter kept `extern` for its type-erased `rcv_task_*` shape), the SOCKS
//! lookup, and `rcv_task_banner_get` (a gtkhx-ui↔hxhandlers Cargo cycle blocks
//! importing it).

use std::cell::RefCell;
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
use hotline_proto::build::HxChunk;
use hx_image_decode::ffi::HxInlineMediaCaps;
use hx_image_decode::{decode_first_frame_async, ImageDecodeHandle, ImageDecodeOutcome};

use crate::ensure_gtk_init;
use crate::tr::tr;

// ------------------------------------------------------------------- //
// Constants (mirror src/banner.c + banner_dispatch.h + hotline.h).

/// Max displayed banner dimensions. Most servers ship 468×60; cap so an
/// unusually large/tall image can't blow out the toolbar layout.
const BANNER_MAX_W: u32 = 600;
const BANNER_MAX_H: i32 = 60;

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
/// `HTLC_HDR_DOWNLOAD_BANNER` (hotline.h) — the zero-chunk file-mode request.
const HTLC_HDR_DOWNLOAD_BANNER: u32 = 0x0000_00d4;
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
/// `rcv_task_fn` — `void (*)(struct htlc_conn *, void *, void *)`. The real
/// `rcv_task_banner_get` has a wider arg list cast to this canonical shape at
/// `task_new` time (the deliberate type erasure the task table relies on).
type RcvTaskFn = unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void);

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

    // hxtask — register the reply task + send the request (Branch 1's Rust send).
    fn task_new(
        htlc: *mut c_void,
        rcv: Option<RcvTaskFn>,
        ptr: *mut c_void,
        data: *mut c_void,
        str_: *const c_char,
    ) -> *mut c_void;
    fn hlwrite_chunks(htlc: *mut c_void, ty: u32, flag: u32, chunks: *const HxChunk, hc: c_int);

    // hxhandlers — the file-mode reply parser (parses ref/size, calls back into
    // banner_handle_htxf_reply below).
    fn rcv_task_banner_get(
        htlc: *mut c_void,
        frame: *const u8,
        frame_len: usize,
        ptr: *mut c_void,
        data: *mut c_void,
    );
}

// ------------------------------------------------------------------- //
// Module state — one banner *widget* per process, content per connection,
// main-thread only.

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
    /// The connection whose banner is being worked on, if any — set when a
    /// message arrives and cleared when the attempt reaches an end, success or
    /// failure. Every result files itself under this.
    ///
    /// One slot, because there is one of each fetch handle below: the module
    /// works on one banner at a time and a new message pre-empts the last. A
    /// server sends its banner once, at login, so the only way to collide is
    /// two connections logging in within a fetch of each other — and the
    /// pre-empted one then has *no* banner rather than a half-finished one,
    /// which is why this is an `Option` rather than a bare key. Knowing whether
    /// an attempt is still running is what lets the pre-emption throw away the
    /// loser's partial content instead of leaving a "loading…" on its tab that
    /// nothing will ever replace.
    pending: Option<dock::ConnKey>,
    /// In-flight URL-mode fetch handle (NULL when none) + its drain timer.
    url_fetch: *mut HxnetBannerFetch,
    url_drain_source: Option<glib::SourceId>,
    /// Bumped whenever a fetch is cancelled; a file-mode worker captures the
    /// value it was spawned with, and its completion drops the result on a
    /// mismatch.
    htxf_generation: u32,
    /// In-flight glycin decode; cancelled on clear / replacement.
    decode_handle: Option<ImageDecodeHandle>,
}

thread_local! {
    static BANNER: RefCell<Option<BannerUi>> = const { RefCell::new(None) };
    static CONTENT: RefCell<HashMap<dock::ConnKey, BannerContent>> =
        RefCell::new(HashMap::new());
}

/// Heap-boxed file-mode fetch, handed to the blocking worker via `user_data`.
/// The worker (off the main thread) fills `bytes`/`ok`; the completion (main
/// thread) reclaims + frees it.
struct HtxfFetch {
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

    let root = gtk::Box::new(gtk::Orientation::Horizontal, 8);
    root.set_margin_start(6);
    root.set_margin_end(6);
    root.set_margin_top(4);
    root.set_margin_bottom(4);
    root.set_visible(false);

    // GtkPicture (not GtkImage — which caps natural size at the icon-size CSS
    // var): renders at the paintable's natural size, bounded via size request.
    let picture = gtk::Picture::new();
    picture.set_size_request(-1, BANNER_MAX_H);
    picture.set_can_shrink(true);
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
        on_banner_clicked(gesture);
    });
    root.add_controller(click);

    BANNER.with(|b| {
        *b.borrow_mut() = Some(BannerUi {
            root: root.clone(),
            picture,
            caption,
            pending: None,
            url_fetch: std::ptr::null_mut(),
            url_drain_source: None,
            htxf_generation: 0,
            decode_handle: None,
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

    // Take the fetch machine — there is only the one. An attempt already
    // running loses, and loses *completely*: its half-written content goes with
    // it, so its tab shows no banner rather than a "loading…" that nothing will
    // ever finish. A finished banner is left alone; only an attempt still in
    // flight is discardable.
    let key = dock::conn_key(htlc);
    let preempted = BANNER.with(|b| b.borrow().as_ref().and_then(|ui| ui.pending));
    cancel_in_flight();
    if let Some(loser) = preempted.filter(|l| *l != key) {
        CONTENT.with(|c| {
            c.borrow_mut().remove(&loser);
        });
    }
    BANNER.with(|b| {
        if let Some(ui) = b.borrow_mut().as_mut() {
            ui.pending = Some(key);
        }
    });
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
                with_owner_content(move |c| {
                    c.url = Some(cached.clone());
                    c.caption = cached;
                });
                start_url_fetch(&u);
            }
            None => {
                // URL mode advertised but no URL — server misconfigured.
                finish_caption(&tr("Server banner: URL mode without URL"));
            }
        }
        return;
    }

    // File mode: TYPE is a binary image-format tag. Ask the server for the
    // bytes over HTXF; show a loading caption meanwhile.
    let loading = crate::tr::tr1("Server banner [%s] — loading…", &type_str);
    show_caption(&loading);
    send_download_request(htlc);
}

/// Abandon whatever fetch is in flight, whoever it belongs to. There is one of
/// each handle, so starting a banner and cancelling one are the same act.
unsafe fn cancel_in_flight() {
    BANNER.with(|b| {
        let mut br = b.borrow_mut();
        let Some(ui) = br.as_mut() else {
            return;
        };

        // The URL-mode fetch: drop the drain timer + close the handle.
        if let Some(src) = ui.url_drain_source.take() {
            src.remove();
        }
        if !ui.url_fetch.is_null() {
            hxnet_banner_fetch_close(ui.url_fetch);
            ui.url_fetch = std::ptr::null_mut();
        }

        // Bump the HTXF generation so any in-flight worker's completion drops.
        ui.htxf_generation = ui.htxf_generation.wrapping_add(1);

        // Any in-flight decode (covers both modes).
        if let Some(h) = ui.decode_handle.take() {
            h.cancel();
        }
    });
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

    // Only stop a fetch that belongs to this connection — another's is none of
    // our business.
    if BANNER.with(|b| {
        b.borrow()
            .as_ref()
            .is_some_and(|ui| ui.pending == Some(key))
    }) {
        cancel_in_flight();
        BANNER.with(|b| {
            if let Some(ui) = b.borrow_mut().as_mut() {
                ui.pending = None;
            }
        });
    }

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

    // The reply has to belong to the attempt that is actually running. The
    // task was registered per connection so this is always the right `htlc`,
    // but between the request and the reply another connection may have taken
    // the fetch machine — and starting a second HTXF worker here would break
    // the one-at-a-time invariant the whole routing rests on, landing this
    // connection's image on the other one's tab.
    if BANNER.with(|b| b.borrow().as_ref().and_then(|ui| ui.pending)) != Some(dock::conn_key(htlc))
    {
        return;
    }

    match validate_htxf_reply(ref_, size) {
        HtxfValidation::Ok => {}
        HtxfValidation::ZeroRef | HtxfValidation::ZeroSize => {
            finish_caption(&tr("Server banner: empty transfer"));
            return;
        }
        HtxfValidation::TooLarge => {
            finish_caption(&tr("Server banner: image too large"));
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

    let generation = BANNER.with(|b| {
        let mut br = b.borrow_mut();
        let ui = br.as_mut().unwrap();
        ui.htxf_generation = ui.htxf_generation.wrapping_add(1);
        ui.htxf_generation
    });

    let fetch = Box::new(HtxfFetch {
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

/// Amend the in-flight connection's banner content, and repaint if that is the
/// connection the user is looking at.
///
/// Every result path goes through here rather than touching the widgets, so a
/// background connection's fetch finishing updates what its tab will show
/// without disturbing the tab that is up.
fn with_owner_content(f: impl FnOnce(&mut BannerContent)) {
    let Some(owner) = BANNER.with(|b| b.borrow().as_ref().and_then(|ui| ui.pending)) else {
        return;
    };
    // `get_mut`, not `entry().or_default()`: a result that arrives for a
    // connection with no entry belongs to one that has been closed or
    // pre-empted, and re-creating its row here would resurrect a connection
    // nothing will ever clear again.
    CONTENT.with(|c| {
        if let Some(content) = c.borrow_mut().get_mut(&owner) {
            f(content);
        }
    });
    if owner == dock::active_key() {
        paint_active();
    }
}

/// Record a result and end the attempt: nothing more is coming for this
/// connection, so the fetch machine is free for another.
fn finish_content(f: impl FnOnce(&mut BannerContent)) {
    with_owner_content(f);
    BANNER.with(|b| {
        if let Some(ui) = b.borrow_mut().as_mut() {
            ui.pending = None;
        }
    });
}

/// A caption that is the end of the attempt — an error, or a banner that turned
/// out to be nothing.
fn finish_caption(text: &str) {
    let text = text.to_owned();
    finish_content(move |c| c.caption = text);
}

/// A caption while the attempt is still running: the URL before its image
/// lands, or "loading…".
fn show_caption(text: &str) {
    let text = text.to_owned();
    with_owner_content(move |c| c.caption = text);
}

/// Paint `content` (or blank, for a connection that has none) onto the one
/// banner row. Pins the picture allocation (capped to BANNER_MAX_W/H, aspect
/// preserved by Contain) so the layout doesn't reflow.
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
            ui.picture.set_paintable(Some(tex));
            ui.picture.set_size_request(
                tex.width().min(BANNER_MAX_W as i32),
                tex.height().min(BANNER_MAX_H),
            );
            ui.picture.set_visible(true);
        }
        None => {
            ui.picture.set_paintable(gtk::gdk::Paintable::NONE);
            ui.picture.set_visible(false);
        }
    }
    ui.caption.set_text(&content.caption);
    ui.root.set_tooltip_text(content.url.as_deref());
    ui.root.set_visible(true);
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
fn start_image_decode(bytes: &[u8]) {
    // Cancel any prior in-flight decode.
    BANNER.with(|b| {
        if let Some(ui) = b.borrow_mut().as_mut() {
            if let Some(h) = ui.decode_handle.take() {
                h.cancel();
            }
        }
    });

    let caps = HxInlineMediaCaps {
        max_bytes: 256 * 1024,
        max_dimension: BANNER_MAX_W,
        max_pixels: BANNER_MAX_W * BANNER_MAX_W,
        max_frames: 1,
        max_duration_ms: 1,
    };

    let handle = decode_first_frame_async(bytes, caps, move |outcome| {
        BANNER.with(|b| {
            if let Some(ui) = b.borrow_mut().as_mut() {
                ui.decode_handle = None;
            }
        });
        match outcome {
            ImageDecodeOutcome::Ok(tex) => finish_content(move |c| {
                c.texture = Some(tex);
                // The image is up; the URL-as-caption is redundant now that
                // the tooltip and the click carry it.
                c.caption.clear();
            }),
            ImageDecodeOutcome::Err { .. } => {
                finish_caption(&tr("Server banner: image not decodable"))
            }
        }
    });

    // Store the handle for cancellation (on async decodes; a sync reject already
    // ran the closure and left decode_handle None).
    BANNER.with(|b| {
        if let Some(ui) = b.borrow_mut().as_mut() {
            ui.decode_handle = handle;
        }
    });
}

// URL-mode fetch --------------------------------------------------- //

fn start_url_fetch(url: &str) {
    unsafe {
        let handle = hxnet_banner_fetch_open(url.as_ptr(), url.len());
        if handle.is_null() {
            return; // bad URL — the caption (the URL itself) stays up
        }
        // 50 ms drain — a small banner image lands quickly.
        let source =
            glib::timeout_add_local(std::time::Duration::from_millis(50), banner_url_drain);
        BANNER.with(|b| {
            if let Some(ui) = b.borrow_mut().as_mut() {
                ui.url_fetch = handle;
                ui.url_drain_source = Some(source);
            }
        });
    }
}

/// Poll the one-shot URL fetch result. Returns whether to keep draining.
fn banner_url_drain() -> glib::ControlFlow {
    let handle = BANNER.with(|b| b.borrow().as_ref().map(|ui| ui.url_fetch));
    let Some(handle) = handle else {
        return glib::ControlFlow::Break;
    };
    if handle.is_null() {
        return glib::ControlFlow::Break;
    }

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

    if rc == HXNET_BANNER_DONE {
        if !out.bytes_ptr.is_null() && out.bytes_len > 0 {
            // The decode copies synchronously, so it's safe to close after.
            let bytes = unsafe { std::slice::from_raw_parts(out.bytes_ptr, out.bytes_len) };
            start_image_decode(bytes);
        } else {
            finish_caption(&tr("Server banner: empty response"));
        }
    } else {
        finish_caption(&tr("Server banner: fetch failed"));
    }

    // One-shot: clear our source id + close the handle. (The source removes
    // itself by returning Break.)
    BANNER.with(|b| {
        if let Some(ui) = b.borrow_mut().as_mut() {
            ui.url_drain_source = None;
            if !ui.url_fetch.is_null() {
                unsafe { hxnet_banner_fetch_close(ui.url_fetch) };
                ui.url_fetch = std::ptr::null_mut();
            }
        }
    });
    glib::ControlFlow::Break
}

// File (HTXF) mode ------------------------------------------------- //

fn send_download_request(htlc: *mut c_void) {
    if htlc.is_null() {
        return;
    }
    unsafe {
        // Register the reply task (rcv_task_banner_get, type-erased to the
        // canonical 3-arg rcv_task_fn shape — the same cast the C RCV_TASK_FN
        // macro performs), then send the zero-chunk DOWNLOAD_BANNER opcode.
        let rcv: RcvTaskFn = std::mem::transmute(
            rcv_task_banner_get
                as unsafe extern "C" fn(*mut c_void, *const u8, usize, *mut c_void, *mut c_void),
        );
        let label = crate::cs("banner_get");
        task_new(
            htlc,
            Some(rcv),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            label.as_ptr(),
        );
        hlwrite_chunks(
            htlc,
            HTLC_HDR_DOWNLOAD_BANNER,
            0,
            std::ptr::null::<HxChunk>(),
            0,
        );
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

    let current = BANNER.with(|b| b.borrow().as_ref().map(|ui| ui.htxf_generation));
    if current != Some(f.generation) {
        return; // user moved on (reconnect / clear / new banner)
    }

    if !f.ok || f.bytes.is_empty() {
        finish_caption(&tr("Server banner: HTXF fetch failed"));
        return;
    }

    // decode_first_frame_async copies the bytes synchronously, so `f` may drop
    // right after.
    start_image_decode(&f.bytes);
}

// Click handler ---------------------------------------------------- //

fn on_banner_clicked(gesture: &gtk::GestureClick) {
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
    let widget = gesture.widget();
    let parent = widget.and_then(|w| w.root()).and_downcast::<gtk::Window>();
    let launcher = gtk::UriLauncher::new(&url);
    launcher.launch(parent.as_ref(), gio::Cancellable::NONE, |_res| {});
}

// ------------------------------------------------------------------- //

#[cfg(test)]
mod tests {
    use super::*;

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
