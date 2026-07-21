//! C ABI + connect-side orchestration for the TOFU trust store.
//!
//! The thread-safe test seams + `known_hosts` path resolution (was
//! `tls_trust.c`) and the classify → silent-accept / auto-accept / prompt → pin
//! `decide` logic (was `network.c`'s `tls_trust_decide`). Exports the single
//! [`hx_tls_verify_cert`] entry the C verify callbacks call, `hx_tls_trust_pin`
//! (integration-test store seed), and the `hx_tls_test_*` seams.
//!
//! The Adwaita prompt lives in the `gtkhx-ui` crate (it needs GTK). This crate
//! reaches it through a callback registered via [`hx_tls_trust_set_prompt`]; the
//! callback owns its own worker→main marshalling. With no callback registered —
//! the headless integration tests, which always drive a seam — the prompt path
//! rejects. Everything here is `std`-only so the minimal Tier-3 test binaries
//! link it without dragging in libadwaita.

use crate::TrustStatus;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int};
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicI32, AtomicUsize, Ordering};
use std::sync::{Mutex, OnceLock};

#[cfg(not(test))]
extern "C" {
    /// gtkhx.c — the resolved (+ created-at-startup) per-user config directory.
    fn gtkhx_config_dir() -> *const c_char;
}

// The crate's own tests always drive lookups through an explicit path or the
// known_hosts override seam, so this null stub only exists to satisfy `cargo
// test` (where gtkhx.c isn't linked).
#[cfg(test)]
unsafe fn gtkhx_config_dir() -> *const c_char {
    std::ptr::null()
}

// ---- thread-safe test seams ----
//
// The verify callback runs on the hxnet (tokio) worker thread, so tests steer
// behaviour with these process-global overrides instead of `setenv` (which
// would race a worker's env read on `environ`). Each defaults to "unset", in
// which case the corresponding env var is consulted exactly as before.

/// tri-state: <0 unset (consult env), 0 force off, >0 force on.
static AUTO_ACCEPT_OV: AtomicI32 = AtomicI32::new(-1);
static FORCE_TLS_OV: AtomicI32 = AtomicI32::new(-1);
/// prompt: 0 unset (consult env), 1 accept, 2 reject.
static PROMPT_OV: AtomicI32 = AtomicI32::new(0);

fn known_hosts_ov() -> &'static Mutex<Option<String>> {
    static OV: OnceLock<Mutex<Option<String>>> = OnceLock::new();
    OV.get_or_init(|| Mutex::new(None))
}

fn env_nonempty(name: &str) -> bool {
    std::env::var_os(name).is_some_and(|v| !v.is_empty())
}

/// `void hx_tls_test_set_auto_accept (int tri)`
#[no_mangle]
pub extern "C" fn hx_tls_test_set_auto_accept(tri: c_int) {
    AUTO_ACCEPT_OV.store(tri, Ordering::SeqCst);
}

/// `void hx_tls_test_set_force_tls (int tri)`
#[no_mangle]
pub extern "C" fn hx_tls_test_set_force_tls(tri: c_int) {
    FORCE_TLS_OV.store(tri, Ordering::SeqCst);
}

/// `void hx_tls_test_set_prompt_verdict (int verdict)`
#[no_mangle]
pub extern "C" fn hx_tls_test_set_prompt_verdict(verdict: c_int) {
    PROMPT_OV.store(verdict, Ordering::SeqCst);
}

/// `void hx_tls_test_set_known_hosts (const char *path)` — NULL clears it.
///
/// # Safety
/// `path` is NULL or a valid C string.
#[no_mangle]
pub unsafe extern "C" fn hx_tls_test_set_known_hosts(path: *const c_char) {
    let v = if path.is_null() {
        None
    } else {
        Some(CStr::from_ptr(path).to_string_lossy().into_owned())
    };
    *known_hosts_ov().lock().unwrap_or_else(|e| e.into_inner()) = v;
}

/// `gboolean hx_tls_test_force_tls (void)` (`gboolean` is `int`) — network.c's
/// connect path gates the TLS decision on this.
#[no_mangle]
pub extern "C" fn hx_tls_test_force_tls() -> c_int {
    let o = FORCE_TLS_OV.load(Ordering::SeqCst);
    let on = if o >= 0 { o != 0 } else { env_nonempty("GTKHX_TLS") };
    on as c_int
}

fn auto_accept() -> bool {
    let o = AUTO_ACCEPT_OV.load(Ordering::SeqCst);
    if o >= 0 {
        o != 0
    } else {
        env_nonempty("GTKHX_TLS_AUTO_ACCEPT")
    }
}

fn prompt_verdict() -> i32 {
    let o = PROMPT_OV.load(Ordering::SeqCst);
    if o != 0 {
        return o;
    }
    match std::env::var("GTKHX_TLS_TEST_PROMPT").ok().as_deref() {
        Some(s) if s.eq_ignore_ascii_case("accept") => 1,
        Some(s) if s.eq_ignore_ascii_case("reject") => 2,
        _ => 0,
    }
}

// ---- path resolution ----

/// Resolve the `known_hosts` path: test override → `GTKHX_KNOWN_HOSTS` env →
/// `$CONFIG/known_hosts`. `None` only if the config dir can't be resolved.
fn known_hosts_path() -> Option<PathBuf> {
    if let Some(ov) = known_hosts_ov()
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .clone()
    {
        return Some(PathBuf::from(ov));
    }
    if let Some(env) = std::env::var_os("GTKHX_KNOWN_HOSTS") {
        if !env.is_empty() {
            return Some(PathBuf::from(env));
        }
    }
    let cfg = unsafe { gtkhx_config_dir() };
    if cfg.is_null() {
        return None;
    }
    let cfg = unsafe { CStr::from_ptr(cfg) };
    Some(PathBuf::from(std::ffi::OsStr::from_bytes(cfg.to_bytes())).join("known_hosts"))
}

// ---- prompt registration (the GUI dialog lives in gtkhx-ui) ----

/// The prompt callback gtkhx-ui installs: show the TOFU dialog on the main
/// thread and return 1 = trust / 0 = reject. Args: host, port, fingerprint,
/// status (0/1/2), and the resolved known_hosts path (may be NULL, for the
/// dialog's "pinned certificates live in …" line). The callback owns the
/// worker→main marshalling.
pub type PromptFn = extern "C" fn(
    host: *const c_char,
    port: u16,
    fingerprint: *const c_char,
    status: c_int,
    known_hosts: *const c_char,
) -> c_int;

/// The installed prompt as a raw `usize` (a NULL == none). `AtomicUsize` because
/// function pointers aren't `AtomicPtr`-friendly and we only ever store/load.
static PROMPT: AtomicUsize = AtomicUsize::new(0);

/// `void hx_tls_trust_set_prompt (PromptFn cb)` — install (or, with NULL, clear)
/// the GUI prompt. Called once from the UI init.
#[no_mangle]
pub extern "C" fn hx_tls_trust_set_prompt(cb: Option<PromptFn>) {
    PROMPT.store(cb.map(|f| f as usize).unwrap_or(0), Ordering::SeqCst);
}

fn call_prompt(host: &str, port: u16, fp: &str, status: TrustStatus, kh: Option<&Path>) -> bool {
    let v = PROMPT.load(Ordering::SeqCst);
    if v == 0 {
        // No GUI registered (headless integration tests) → reject. Production
        // always installs one at UI init, and tests always set a seam first.
        return false;
    }
    let cb: PromptFn = unsafe { std::mem::transmute::<usize, PromptFn>(v) };
    let host_c = CString::new(host).unwrap_or_default();
    let fp_c = CString::new(fp).unwrap_or_default();
    let kh_c = kh.and_then(|p| CString::new(p.as_os_str().as_bytes()).ok());
    let kh_ptr = kh_c.as_ref().map_or(std::ptr::null(), |c| c.as_ptr());
    cb(host_c.as_ptr(), port, fp_c.as_ptr(), status as c_int, kh_ptr) != 0
}

// ---- decision + pin ----

/// Today's UTC date as `YYYY-MM-DD` for the pin's "added" comment. Std-only
/// (Howard Hinnant's days→civil), so the crate needs no date dependency.
fn today_utc() -> String {
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let (y, m, d) = civil_from_days((secs / 86_400) as i64);
    format!("{y:04}-{m:02}-{d:02}")
}

fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097; // [0, 146096]
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365; // [0, 399]
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    let mp = (5 * doy + 2) / 153; // [0, 11]
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32; // [1, 31]
    let m = (if mp < 10 { mp + 3 } else { mp - 9 }) as u32; // [1, 12]
    (y + i64::from(m <= 2), m, d)
}

fn pin_now(path: &Path, host: &str, port: u16, fp: &str) {
    if let Err(e) = crate::pin(path, host, port, fp, &today_utc()) {
        // The connection is already up; the trust state just won't persist, so
        // the user sees the prompt again next connect. Warn, don't disturb it.
        eprintln!(
            "gtkhx: TLS trust pin failed for {host}:{port} ({e}) — connection \
             allowed but the trust state won't persist; check known_hosts \
             permissions."
        );
    }
}

/// The shared TOFU decision. Classify against the store and accept silently
/// (Trusted, or the same cert already pinned for the host on another port),
/// auto-accept (headless tests), or prompt — pinning on accept. Safe on any
/// thread; the prompt callback owns its worker→main marshalling.
fn decide(host: &str, port: u16, fingerprint: &str) -> bool {
    let path = known_hosts_path();
    let status = match &path {
        Some(p) => crate::lookup(p, host, port, fingerprint),
        None => TrustStatus::Unknown,
    };

    if status == TrustStatus::Trusted {
        return true;
    }

    // Same cert pinned for this host on another port (control :5600 → HTXF
    // :5601) — accept + pin the new port silently. Only on strict-Unknown.
    if status == TrustStatus::Unknown {
        if let Some(p) = &path {
            if crate::host_has_fingerprint(p, host, fingerprint) {
                pin_now(p, host, port, fingerprint);
                return true;
            }
        }
    }

    // Headless / scripted escape hatch. MISMATCH is logged loudly so a silent
    // override can't hide a real fingerprint change.
    if auto_accept() {
        if status == TrustStatus::Mismatch {
            eprintln!(
                "gtkhx: GTKHX_TLS_AUTO_ACCEPT overriding TLS MISMATCH for \
                 {host}:{port} (fp={fingerprint}) — the pinned fingerprint \
                 differs. Test harnesses only; production should never see this."
            );
        }
        if let Some(p) = &path {
            pin_now(p, host, port, fingerprint);
        }
        return true;
    }

    // Real prompt (via the registered GUI callback), or the test-only verdict.
    let accepted = match prompt_verdict() {
        1 => true,
        2 => false,
        _ => call_prompt(host, port, fingerprint, status, path.as_deref()),
    };
    if accepted {
        if let Some(p) = &path {
            pin_now(p, host, port, fingerprint);
        }
    }
    accepted
}

/// `gboolean hx_tls_verify_cert (const char *host, guint16 port,
/// const char *fingerprint)` — the single TOFU verify entry the C verify
/// callbacks (control / HTXF / tracker / banner) call.
///
/// # Safety
/// `host` / `fingerprint` are valid C strings.
#[no_mangle]
pub unsafe extern "C" fn hx_tls_verify_cert(
    host: *const c_char,
    port: u16,
    fingerprint: *const c_char,
) -> c_int {
    if host.is_null() || fingerprint.is_null() {
        return 0;
    }
    let host = CStr::from_ptr(host).to_string_lossy().into_owned();
    let fingerprint = CStr::from_ptr(fingerprint).to_string_lossy().into_owned();
    decide(&host, port, &fingerprint) as c_int
}

/// `gboolean hx_tls_trust_pin (const char *host, guint16 port,
/// const char *fingerprint)` — path-resolving pin (today's date) used by the
/// integration tests to seed the store through the same resolver production
/// uses.
///
/// # Safety
/// `host` / `fingerprint` are valid C strings.
#[no_mangle]
pub unsafe extern "C" fn hx_tls_trust_pin(
    host: *const c_char,
    port: u16,
    fingerprint: *const c_char,
) -> c_int {
    if host.is_null() || fingerprint.is_null() {
        return 0;
    }
    let host = CStr::from_ptr(host).to_string_lossy().into_owned();
    let fingerprint = CStr::from_ptr(fingerprint).to_string_lossy().into_owned();
    let Some(path) = known_hosts_path() else {
        return 0;
    };
    match crate::pin(&path, &host, port, &fingerprint, &today_utc()) {
        Ok(()) => 1,
        Err(_) => 0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The seams are process-global; serialize the tests that mutate them.
    fn seam_lock() -> std::sync::MutexGuard<'static, ()> {
        static L: OnceLock<Mutex<()>> = OnceLock::new();
        L.get_or_init(|| Mutex::new(()))
            .lock()
            .unwrap_or_else(|e| e.into_inner())
    }

    fn reset_seams() {
        AUTO_ACCEPT_OV.store(-1, Ordering::SeqCst);
        FORCE_TLS_OV.store(-1, Ordering::SeqCst);
        PROMPT_OV.store(0, Ordering::SeqCst);
        *known_hosts_ov().lock().unwrap() = None;
        PROMPT.store(0, Ordering::SeqCst);
    }

    struct Tmp(PathBuf);
    impl Tmp {
        fn new() -> Tmp {
            let dir = std::env::temp_dir().join(format!(
                "hxtls-ffi-{}-{}",
                std::process::id(),
                std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap()
                    .as_nanos()
            ));
            std::fs::create_dir_all(&dir).unwrap();
            Tmp(dir)
        }
        fn kh(&self) -> PathBuf {
            self.0.join("known_hosts")
        }
    }
    impl Drop for Tmp {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    const FP_A: &str = "sha256:aaaa000000000000000000000000000000000000000000000000000000000000";
    const FP_B: &str = "sha256:bbbb000000000000000000000000000000000000000000000000000000000000";

    fn set_kh(p: &Path) {
        *known_hosts_ov().lock().unwrap() = Some(p.to_string_lossy().into_owned());
    }

    #[test]
    fn decide_trusted_needs_no_prompt() {
        let _g = seam_lock();
        reset_seams();
        let tmp = Tmp::new();
        set_kh(&tmp.kh());
        // No prompt registered → if decide reached the prompt it would reject.
        crate::pin(&tmp.kh(), "host", 5600, FP_A, "2026-06-01").unwrap();
        assert!(decide("host", 5600, FP_A));
        reset_seams();
    }

    #[test]
    fn decide_unknown_without_gui_rejects() {
        let _g = seam_lock();
        reset_seams();
        let tmp = Tmp::new();
        set_kh(&tmp.kh());
        // Unknown + no auto-accept, no verdict seam, no GUI → reject.
        assert!(!decide("host", 5600, FP_A));
        // …and nothing was pinned.
        assert_eq!(crate::lookup(&tmp.kh(), "host", 5600, FP_A), TrustStatus::Unknown);
        reset_seams();
    }

    #[test]
    fn decide_auto_accept_pins_over_mismatch() {
        let _g = seam_lock();
        reset_seams();
        let tmp = Tmp::new();
        set_kh(&tmp.kh());
        crate::pin(&tmp.kh(), "host", 5600, FP_A, "2026-06-01").unwrap();
        AUTO_ACCEPT_OV.store(1, Ordering::SeqCst);
        // A different fp for the pinned host:port is a MISMATCH; auto-accept
        // overrides + re-pins to the new fp.
        assert!(decide("host", 5600, FP_B));
        assert_eq!(crate::lookup(&tmp.kh(), "host", 5600, FP_B), TrustStatus::Trusted);
        reset_seams();
    }

    #[test]
    fn decide_prompt_verdict_reject_does_not_pin() {
        let _g = seam_lock();
        reset_seams();
        let tmp = Tmp::new();
        set_kh(&tmp.kh());
        PROMPT_OV.store(2, Ordering::SeqCst); // reject
        assert!(!decide("host", 5600, FP_A));
        assert_eq!(crate::lookup(&tmp.kh(), "host", 5600, FP_A), TrustStatus::Unknown);
        reset_seams();
    }

    #[test]
    fn decide_reuses_cert_across_ports() {
        let _g = seam_lock();
        reset_seams();
        let tmp = Tmp::new();
        set_kh(&tmp.kh());
        crate::pin(&tmp.kh(), "host", 5600, FP_A, "2026-06-01").unwrap();
        // Same cert on a new port → silent accept + pin, no prompt needed.
        assert!(decide("host", 5601, FP_A));
        assert_eq!(crate::lookup(&tmp.kh(), "host", 5601, FP_A), TrustStatus::Trusted);
        reset_seams();
    }

    #[test]
    fn force_tls_seam_tri_state() {
        let _g = seam_lock();
        reset_seams();
        assert_eq!(hx_tls_test_force_tls(), 0); // unset + env off (test env)
        FORCE_TLS_OV.store(1, Ordering::SeqCst);
        assert_eq!(hx_tls_test_force_tls(), 1);
        FORCE_TLS_OV.store(0, Ordering::SeqCst);
        assert_eq!(hx_tls_test_force_tls(), 0);
        reset_seams();
    }
}
