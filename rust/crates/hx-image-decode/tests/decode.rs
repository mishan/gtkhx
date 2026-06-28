//! End-to-end decode tests driving glycin against real image
//! bytes. These run in `cargo test -p hx-image-decode` if the
//! host has glycin loaders installed (detected via their config
//! under `$XDG_DATA_DIRS/glycin-loaders/*/conf.d/` — see
//! `glycin_loaders_available`). When the loaders are absent the
//! tests skip on a dev box but fail loudly under CI (the `CI`
//! env var) so a missing-loader CI image can't mask a regression.
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

/// Stub for the C-side telemetry bridge so the integration test binary
/// links. In the real binary `src/inline_media_decode.c` provides this
/// and routes the message through `debug_log("media", ...)`. The
/// decoder's telemetry path calls it on decode-start / -done / -failed
/// — the -failed line carries glycin's error category. We print to
/// stderr (visible with `cargo test -- --nocapture`, and on a failing
/// test cargo prints captured output anyway), so a CI decode failure is
/// self-diagnosing instead of needing a guess-and-rerun cycle.
#[no_mangle]
pub extern "C" fn hx_image_decode_log(msg: *const std::ffi::c_char) {
    if msg.is_null() {
        return;
    }
    let s = unsafe { std::ffi::CStr::from_ptr(msg) }.to_string_lossy();
    eprintln!("[hx-image-decode] {s}");
}

use hx_image_decode::ffi::{
    inline_media_decode_async, inline_media_decode_cancel, inline_media_decoded_free,
    HxInlineMediaDecoded,
};
// Borrow the crate's version-selected gtk-rs family so the test's raw
// GArray / GdkTexture FFI calls match whichever glycin backend is built
// (0.21 under glycin-v3, 0.20 under glycin-v2). See crate::compat.
use hx_image_decode::compat::{gdk, glib};

/// Whether glycin image loaders are installed on this host.
///
/// glycin discovers loaders through config files at
/// `$XDG_DATA_DIRS/glycin-loaders/<api>+/conf.d/*.conf` (the `Exec=` in
/// each points at the loader binary). We probe for that config rather
/// than a hardcoded binary path because the binary's location varies by
/// distro — `/usr/libexec/glycin-loaders/…` on Debian/GNOME, `/usr/lib64`
/// on Fedora — and the API-version dir (`1+`, `2+`, …) tracks the glycin
/// release, whereas the config always lives under the data dirs glycin
/// itself searches. Keying off the config keeps the gate honest across
/// CI (Fedora) and dev boxes (Debian/Ubuntu) without guessing paths.
fn glycin_loaders_available() -> bool {
    let data_dirs = std::env::var("XDG_DATA_DIRS")
        .unwrap_or_else(|_| "/usr/local/share:/usr/share".to_string());
    data_dirs
        .split(':')
        .filter(|d| !d.is_empty())
        .map(|d| Path::new(d).join("glycin-loaders"))
        .any(|root| glycin_conf_present_under(&root))
}

/// True if `root` (a `…/glycin-loaders` directory) holds at least one
/// `<api>+/conf.d/*.conf` loader config, for any API-version subdir.
fn glycin_conf_present_under(root: &Path) -> bool {
    let Ok(versions) = std::fs::read_dir(root) else {
        return false;
    };
    versions.filter_map(Result::ok).any(|ver| {
        let confd = ver.path().join("conf.d");
        std::fs::read_dir(&confd).is_ok_and(|entries| {
            entries
                .filter_map(Result::ok)
                .any(|e| e.path().extension().is_some_and(|x| x == "conf"))
        })
    })
}

/// Glycin-loader gate for the decode tests. Returns `true` when the
/// loaders are present and the test should run.
///
/// When they're absent the behaviour depends on the environment:
///
///   - **Under CI** (`CI` env var, which GitHub Actions always sets) a
///     missing loader is a HARD failure. A silent skip there would let a
///     real decode regression sail through a green run — the exact
///     footgun the project's no-silent-skips rule guards against. If this
///     fires in CI, the fix is to install the loaders (the `glycin-loaders`
///     package) in the job, not to skip.
///   - **Locally** (a slim dev box with no gnome-platform pieces) it
///     skips with a notice, so `cargo test` stays usable off-desktop.
fn require_glycin() -> bool {
    if glycin_loaders_available() {
        return true;
    }
    if std::env::var_os("CI").is_some() {
        panic!(
            "glycin loaders not found via $XDG_DATA_DIRS/glycin-loaders/*/conf.d \
             under CI: install the glycin-loaders package so this decode test \
             runs — refusing to silently skip"
        );
    }
    eprintln!(
        "skipping glycin decode test: no loader config under \
         $XDG_DATA_DIRS/glycin-loaders/*/conf.d (set CI=1 to make this fatal)"
    );
    false
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
    /// Glycin's error string on the failure path (the ErrorCtx the
    /// decoder stashes — includes glycin's "Used config" dump, which
    /// names the loader dirs + API version it searched). Surfaced in
    /// the fixture assertions so a CI failure is self-diagnosing.
    error_message: Option<String>,
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
        let error_message = if r.error_message.is_null() {
            None
        } else {
            Some(
                std::ffi::CStr::from_ptr(r.error_message)
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
            error_message,
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
    // Force glycin's unsandboxed loader path (see src/decode.rs): its
    // default Auto sandbox runs bwrap, which can't spawn in CI
    // containers or some dev sandboxes — there the decode would fail
    // and map to UnsupportedFormat. The fixtures are trusted in-tree
    // images. Set process-wide (decode tests are serialised by
    // MAIN_CTX_LOCK, and the value never varies) so the test passes
    // regardless of how the runner configures its environment.
    std::env::set_var("GTKHX_GLYCIN_NO_SANDBOX", "1");
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
    if !require_glycin() {
        return;
    }
    run_in_main_thread(|| {
        let r = decode_fixture("banner_http.png");
        assert_eq!(
            r.error_code, 0,
            "glycin decode failed: error_code={} message={:?} \
             (has_texture={}, mime={:?}). error_code 2 = UnsupportedFormat, \
             which the decoder also returns for any glycin loader error \
             (e.g. no loader installed for the format / loader API mismatch \
             — glycin's message names the config it searched).",
            r.error_code, r.error_message, r.has_texture, r.mime
        );
        assert!(r.has_texture);
        assert!(!r.has_frames, "PNG is static; no frames array expected");
        assert!(r.width > 0 && r.height > 0);
        assert_eq!(r.mime.as_deref(), Some("image/png"));
    });
}

#[test]
fn jpeg_fixture_decodes() {
    if !require_glycin() {
        return;
    }
    run_in_main_thread(|| {
        let r = decode_fixture("banner_htxf.jpg");
        assert_eq!(
            r.error_code, 0,
            "glycin decode failed: error_code={} message={:?} \
             (has_texture={}, mime={:?}). error_code 2 = UnsupportedFormat, \
             which the decoder also returns for any glycin loader error \
             (e.g. no loader installed for the format / loader API mismatch \
             — glycin's message names the config it searched).",
            r.error_code, r.error_message, r.has_texture, r.mime
        );
        assert!(r.has_texture);
        assert!(!r.has_frames);
        assert!(r.width > 0 && r.height > 0);
        assert_eq!(r.mime.as_deref(), Some("image/jpeg"));
    });
}

#[test]
fn gif_fixture_decodes() {
    if !require_glycin() {
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
        assert_eq!(
            r.error_code, 0,
            "glycin decode failed: error_code={} message={:?} \
             (has_texture={}, mime={:?}). error_code 2 = UnsupportedFormat, \
             which the decoder also returns for any glycin loader error \
             (e.g. no loader installed for the format / loader API mismatch \
             — glycin's message names the config it searched).",
            r.error_code, r.error_message, r.has_texture, r.mime
        );
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
