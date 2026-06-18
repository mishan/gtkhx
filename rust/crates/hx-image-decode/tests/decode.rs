//! End-to-end decode tests driving glycin against real image
//! bytes. These run in `cargo test -p hx-image-decode` if the
//! host has the glycin loader binaries at
//! `/usr/libexec/glycin-loaders/2+/` (org.gnome.Platform 47+
//! and modern Debian/Ubuntu/Fedora installs all do). When the
//! loaders are absent the tests skip themselves rather than
//! fail — same shape as the C `g_test_skip` pattern in Tier 2.
//!
//! The fixtures (PNG, JPEG, GIF) come from `tests/common/`
//! which the Tier 3 banner suite already uses. They're small
//! (~2 KB each), deterministic, and match the inline-media
//! spec's allowlist exactly.

use std::cell::RefCell;
use std::ffi::c_void;
use std::path::Path;
use std::sync::Mutex;
use std::time::{Duration, Instant};

/// `glib::MainContext::default()` is a process-wide singleton;
/// only one thread can hold it at a time. cargo test runs
/// tests in parallel by default, which collides. This lock
/// serialises every test that touches the default MainContext.
static MAIN_CTX_LOCK: Mutex<()> = Mutex::new(());

/// Stub for the C-side telemetry bridge so the integration
/// test binary links. In the real binary
/// `src/inline_media_decode.c` provides this and routes the
/// message through `debug_log("media", ...)`. The decoder's
/// telemetry path calls it on decode-start / -done / -failed;
/// for tests we drop the message — `cargo test`'s captured
/// stdout is the right place for diagnostics, not the GtkHx
/// debug-log gate.
#[no_mangle]
pub extern "C" fn hx_image_decode_log(_msg: *const std::ffi::c_char) {}

use hx_image_decode::ffi::{
    inline_media_decode_async, inline_media_decode_cancel, inline_media_decoded_free,
    HxInlineMediaDecoded,
};

/// Path on the host where glycin loaders live. Skip the
/// integration tests if it's missing — typically a slim CI
/// container without the gnome-platform pieces.
fn glycin_loaders_available() -> bool {
    Path::new("/usr/libexec/glycin-loaders/2+/glycin-image-rs").exists()
}

/// Resolve the project's tests/common fixture path. The
/// `CARGO_MANIFEST_DIR` for `hx-image-decode` points at
/// `rust/crates/hx-image-decode/`; back out to repo root +
/// `tests/common/`.
fn fixture_path(name: &str) -> std::path::PathBuf {
    let crate_dir = Path::new(env!("CARGO_MANIFEST_DIR"));
    crate_dir.join("../../../tests/common").join(name)
}

/// Shared decode-result collector. The C-shaped callback
/// receives the result pointer; we ferry it through a thread-
/// local refcell since the unit-test main loop is single-
/// threaded.
struct DecodeResult {
    error_code: u16,
    has_texture: bool,
    has_frames: bool,
    /// Reserved for the animated-GIF round-trip test that
    /// ships alongside the Tier 3 Janus work — keeps the field
    /// in the struct so an animated fixture decode populates
    /// it without churning the struct shape.
    #[allow(dead_code)]
    frame_count: usize,
    width: i32,
    height: i32,
    mime: Option<String>,
}

thread_local! {
    static LAST_RESULT: RefCell<Option<DecodeResult>> = const { RefCell::new(None) };
}

extern "C" fn collect_cb(
    decoded: *mut HxInlineMediaDecoded,
    _user_data: *mut c_void,
) {
    unsafe {
        let r = &*decoded;
        let mime = if r.canonical_mime.is_null() {
            None
        } else {
            Some(
                std::ffi::CStr::from_ptr(r.canonical_mime)
                    .to_string_lossy()
                    .into_owned(),
            )
        };
        let (has_frames, frame_count) = if r.frames.is_null() {
            (false, 0)
        } else {
            let arr = r.frames as *mut glib::ffi::GArray;
            (true, (*arr).len as usize)
        };
        let (width, height) = if r.texture.is_null() {
            (0, 0)
        } else {
            (
                gdk::ffi::gdk_texture_get_width(r.texture),
                gdk::ffi::gdk_texture_get_height(r.texture),
            )
        };
        let result = DecodeResult {
            error_code: r.error_code,
            has_texture: !r.texture.is_null(),
            has_frames,
            frame_count,
            width,
            height,
            mime,
        };
        LAST_RESULT.with(|cell| *cell.borrow_mut() = Some(result));
    }
    unsafe { inline_media_decoded_free(decoded) };
}

/// Block the GLib main loop until either `LAST_RESULT` is
/// filled or `deadline` elapses. Returns the result on
/// success, panics on timeout.
fn drive_until_done(deadline: Duration) -> DecodeResult {
    let ctx = glib::MainContext::default();
    let start = Instant::now();
    while LAST_RESULT.with(|c| c.borrow().is_none()) {
        if start.elapsed() > deadline {
            panic!("decode did not complete within {:?}", deadline);
        }
        // Iterate once with may_block=true so the future can
        // make progress without spinning the CPU.
        ctx.iteration(true);
    }
    LAST_RESULT.with(|c| c.borrow_mut().take()).unwrap()
}

fn decode_fixture(path: &str) -> DecodeResult {
    let bytes = std::fs::read(fixture_path(path))
        .unwrap_or_else(|e| panic!("read fixture {}: {}", path, e));
    // Spec defaults: caps NULL → glycin sees the spec floor.
    let token = unsafe {
        inline_media_decode_async(
            bytes.as_ptr(),
            bytes.len(),
            std::ptr::null(),
            collect_cb,
            std::ptr::null_mut(),
        )
    };
    let result = drive_until_done(Duration::from_secs(10));
    // Cancel = canonical free for the token even on success.
    unsafe { inline_media_decode_cancel(token) };
    result
}

fn run_in_main_thread<F: FnOnce()>(f: F) {
    // Serialise against other tests racing for the default
    // MainContext. We hold `MAIN_CTX_LOCK` for the duration
    // of the test body, then acquire the MainContext on top
    // of that — the lock guarantees no other test thread has
    // it acquired by the time we get here.
    let _ser = MAIN_CTX_LOCK.lock().unwrap_or_else(|p| p.into_inner());
    let ctx = glib::MainContext::default();
    let _guard = ctx.acquire().expect("MainContext acquire");
    LAST_RESULT.with(|c| *c.borrow_mut() = None);
    f();
}

#[test]
fn png_fixture_decodes() {
    if !glycin_loaders_available() {
        eprintln!(
            "skipping: glycin loaders missing at /usr/libexec/glycin-loaders/2+/"
        );
        return;
    }
    run_in_main_thread(|| {
        let r = decode_fixture("banner_http.png");
        assert_eq!(r.error_code, 0);
        assert!(r.has_texture);
        assert!(!r.has_frames, "PNG is static; no frames array expected");
        assert!(r.width > 0 && r.height > 0);
        assert_eq!(r.mime.as_deref(), Some("image/png"));
    });
}

#[test]
fn jpeg_fixture_decodes() {
    if !glycin_loaders_available() {
        eprintln!("skipping: glycin loaders missing");
        return;
    }
    run_in_main_thread(|| {
        let r = decode_fixture("banner_htxf.jpg");
        assert_eq!(r.error_code, 0);
        assert!(r.has_texture);
        assert!(!r.has_frames);
        assert!(r.width > 0 && r.height > 0);
        assert_eq!(r.mime.as_deref(), Some("image/jpeg"));
    });
}

#[test]
fn gif_fixture_decodes() {
    if !glycin_loaders_available() {
        eprintln!("skipping: glycin loaders missing");
        return;
    }
    // The shipped GIF fixture is a single-frame static — it
    // exercises the GIF code path through glycin without
    // forcing us to maintain an animated GIF fixture in tree.
    // Animated-GIF coverage lands at the Tier 3 layer once we
    // confirm the multi-frame collection loop visibly behaves
    // against Janus.
    run_in_main_thread(|| {
        let r = decode_fixture("banner_htxf.gif");
        assert_eq!(r.error_code, 0);
        assert!(r.has_texture);
        assert!(r.width > 0 && r.height > 0);
        assert_eq!(r.mime.as_deref(), Some("image/gif"));
    });
}

#[test]
fn empty_input_rejects_synchronously() {
    // No glycin needed for this — the byte-cap + sniff gates
    // fire synchronously before the loader ever sees bytes.
    run_in_main_thread(|| {
        let bytes: [u8; 0] = [];
        let token = unsafe {
            inline_media_decode_async(
                bytes.as_ptr(),
                0,
                std::ptr::null(),
                collect_cb,
                std::ptr::null_mut(),
            )
        };
        // Synchronous reject: token is NULL, callback already
        // fired before this call returned.
        assert!(token.is_null());
        let r = LAST_RESULT
            .with(|c| c.borrow_mut().take())
            .expect("synchronous callback");
        assert_eq!(r.error_code, 2 /* UnsupportedFormat */);
        assert!(!r.has_texture);
    });
}

#[test]
fn random_bytes_rejected_synchronously() {
    run_in_main_thread(|| {
        let bytes = b"this is not an image, no magic bytes either";
        let token = unsafe {
            inline_media_decode_async(
                bytes.as_ptr(),
                bytes.len(),
                std::ptr::null(),
                collect_cb,
                std::ptr::null_mut(),
            )
        };
        assert!(token.is_null());
        let r = LAST_RESULT
            .with(|c| c.borrow_mut().take())
            .expect("synchronous callback");
        assert_eq!(r.error_code, 2);
        assert!(!r.has_texture);
    });
}
