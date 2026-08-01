//! The C ABI.
//!
//! C reads settings from a plain C struct it owns and refreshes from here;
//! every *write* goes through this module and lands in the TOML file. That is
//! the whole point of the flip — the old system had the settings table holding
//! raw addresses into a C global, so a write could come from anywhere and the
//! file was rebuilt from those addresses on every save.
//!
//! **The mirror's storage stays in C.** `docs/preferences.md` sketched a
//! `#[repr(C)]` mirror owned by Rust with its layout pinned by
//! `_Static_assert`. Refreshing a C-owned struct through by-name getters
//! instead gets the same property — one write path, and a hundred C read sites
//! that keep compiling untouched — with no layout coupling between the two
//! languages at all. There is nothing to pin, so there is nothing to get
//! wrong. See `src/prefs_mirror.c`.
//!
//! Names are the old SHOUTING_CASE keys, because that is what the Settings
//! pages already use and what a caller can spell without looking anything up.
//! They resolve through the migration map, which already had to know every one
//! of them.

use crate::fields::{kind_of, Kind};
use crate::migrate::{target_of, Target};
use crate::Config;
use std::cell::RefCell;
use std::ffi::{c_char, c_int, CStr, CString};
use std::path::PathBuf;

thread_local! {
    /// The loaded settings. Main-thread only, like everything else that
    /// touches preferences — worker threads marshal through the bridge.
    static CONFIG: RefCell<Option<State>> = const { RefCell::new(None) };
}

struct State {
    config: Config,
    dir: PathBuf,
    /// Set by any mutation, cleared by a flush. The old code rewrote the whole
    /// file on every switch toggle; this lets the caller coalesce.
    dirty: bool,
}

/// Run `f` with the loaded settings, or return `default` if nothing is loaded.
///
/// Not being loaded is a real state, not a bug: several unit tests link this
/// crate without ever calling `hxconfig_load`, and the first-run path reads
/// preferences before deciding whether to pop the Settings dialog.
fn with<T>(default: T, f: impl FnOnce(&mut State) -> T) -> T {
    CONFIG.with(|c| match c.borrow_mut().as_mut() {
        Some(state) => f(state),
        None => default,
    })
}

/// A C index as a slice index, or `None` if it is negative.
///
/// Clamping a negative to 0 instead would answer a nonsense question with
/// element zero, which reads to the caller as a real value.
fn index(i: c_int) -> Option<usize> {
    usize::try_from(i).ok()
}

fn name_of(raw: *const c_char) -> Option<String> {
    if raw.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(raw) }
        .to_str()
        .ok()
        .map(str::to_owned)
}

/// The schema path an old key name resolves to, if it is still a setting.
fn path_of(name: *const c_char) -> Option<&'static str> {
    match target_of(&name_of(name)?)? {
        Target::Path(path) => Some(path),
        Target::Drop(_) => None,
    }
}

// ------------------------------------------------------------ lifecycle --

/// Load `gtkhx.toml` from `dir`, importing an old `gtkhxrc` if there is none.
///
/// `legacy` is the `gtkhxrc` path to import from, and `legacy_home` the
/// `~/.gtkhxrc` fallback; either may be NULL. The config directory is resolved
/// on the C side and passed in — see `docs/preferences.md`'s open question on
/// who should own that, which this deliberately does not answer.
///
/// Returns the number of diagnostics recorded, which the caller can then walk
/// with [`hxconfig_warning`]. Loading never fails.
///
/// # Safety
/// All three arguments must be NUL-terminated C strings or NULL.
#[no_mangle]
pub unsafe extern "C" fn hxconfig_load(
    dir: *const c_char,
    legacy: *const c_char,
    legacy_home: *const c_char,
) -> c_int {
    let Some(dir) = name_of(dir) else {
        return 0;
    };
    let dir = PathBuf::from(dir);

    let legacy_paths: Vec<PathBuf> = [legacy, legacy_home]
        .into_iter()
        .filter_map(name_of)
        .map(PathBuf::from)
        .collect();

    let config = Config::load_or_migrate(&dir, &legacy_paths);
    let count = config.warnings().len() as c_int;
    CONFIG.with(|c| {
        *c.borrow_mut() = Some(State {
            config,
            dir,
            dirty: false,
        });
    });
    count
}

/// Whether the load found no settings file at all — the first-run signal the
/// old code got from `g_file_test` on `gtkhxrc`.
#[no_mangle]
pub extern "C" fn hxconfig_is_first_run() -> c_int {
    with(0, |s| {
        matches!(s.config.provenance(), crate::Provenance::Fresh) as c_int
    })
}

/// Diagnostic `i` as a Rust-allocated string, or NULL when there is no such
/// diagnostic. Free with [`hxconfig_free_string`].
#[no_mangle]
pub extern "C" fn hxconfig_warning(i: c_int) -> *mut c_char {
    with(std::ptr::null_mut(), |s| {
        match index(i).and_then(|i| s.config.warnings().get(i)) {
            Some(w) => into_raw(w.to_string()),
            None => std::ptr::null_mut(),
        }
    })
}

/// Write the file if anything has changed since the last flush.
///
/// Separate from the setters so a run of changes coalesces into one write.
/// Every switch toggle used to rebuild and rewrite the whole file
/// synchronously.
#[no_mangle]
pub extern "C" fn hxconfig_flush() -> c_int {
    with(0, |s| {
        if !s.dirty {
            return 1;
        }
        let dir = s.dir.clone();
        match s.config.save(&dir) {
            Ok(()) => {
                s.dirty = false;
                1
            }
            Err(_) => 0,
        }
    })
}

// -------------------------------------------------------------- reading --

/// The kind of value a name holds, or 0 for a name the schema doesn't have.
///
/// The tags match the old `cfgvars[]` type constants so the Settings pages,
/// which switch on them, need no changes.
#[no_mangle]
pub extern "C" fn hxconfig_type(name: *const c_char) -> c_int {
    let Some(path) = path_of(name) else {
        return 0;
    };
    match kind_of(path) {
        // A colour is an integer everywhere but the file, where it is
        // #rrggbb — the Settings picker and the C mirror both deal in ints.
        Some(Kind::Unsigned) | Some(Kind::Extent) | Some(Kind::Color) => 1, /* INT */
        Some(Kind::Flag) => 2,                                              /* BOOLEAN */
        Some(Kind::Text) | Some(Kind::Scheme) | Some(Kind::List) => 3,      /* STRING */
        Some(Kind::Id16) => 5,                                              /* UINT16 */
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn hxconfig_get_bool(name: *const c_char) -> c_int {
    let Some(path) = path_of(name) else {
        return 0;
    };
    with(0, |s| {
        crate::fields::get_bool(s.config.settings(), path) as c_int
    })
}

#[no_mangle]
pub extern "C" fn hxconfig_get_int(name: *const c_char) -> c_int {
    let Some(path) = path_of(name) else {
        return 0;
    };
    with(0, |s| crate::fields::get_int(s.config.settings(), path))
}

/// A string value, Rust-allocated and never NULL for a known name. Free with
/// [`hxconfig_free_string`].
#[no_mangle]
pub extern "C" fn hxconfig_get_string(name: *const c_char) -> *mut c_char {
    let Some(path) = path_of(name) else {
        return into_raw(String::new());
    };
    with(into_raw(String::new()), |s| {
        into_raw(crate::fields::get_string(s.config.settings(), path))
    })
}

// -------------------------------------------------------------- writing --

#[no_mangle]
pub extern "C" fn hxconfig_set_bool(name: *const c_char, value: c_int) -> c_int {
    let Some(path) = path_of(name) else {
        return 0;
    };
    with(0, |s| {
        let changed = crate::fields::set_bool(s.config.settings_mut(), path, value != 0);
        s.dirty |= changed;
        changed as c_int
    })
}

#[no_mangle]
pub extern "C" fn hxconfig_set_int(name: *const c_char, value: c_int) -> c_int {
    let Some(path) = path_of(name) else {
        return 0;
    };
    with(0, |s| {
        let changed = crate::fields::set_int(s.config.settings_mut(), path, value);
        s.dirty |= changed;
        changed as c_int
    })
}

/// # Safety
/// `value` must be a NUL-terminated C string or NULL, which is read as empty.
#[no_mangle]
pub unsafe extern "C" fn hxconfig_set_string(name: *const c_char, value: *const c_char) -> c_int {
    let Some(path) = path_of(name) else {
        return 0;
    };
    let value = name_of(value).unwrap_or_default();
    with(0, |s| {
        let changed = crate::fields::set_string(s.config.settings_mut(), path, &value);
        s.dirty |= changed;
        changed as c_int
    })
}

// ---------------------------------------------------------- the trackers --

/// How many tracker addresses are configured.
///
/// The old struct carried a `char **` array rebuilt by a change hook beside
/// the comma-separated string it was derived from; the schema stores a real
/// array, so the derived copy and its hook are gone.
#[no_mangle]
pub extern "C" fn hxconfig_tracker_count() -> c_int {
    with(0, |s| s.config.settings().trackers.addresses.len() as c_int)
}

/// Tracker address `i`, Rust-allocated. Free with [`hxconfig_free_string`].
#[no_mangle]
pub extern "C" fn hxconfig_tracker_at(i: c_int) -> *mut c_char {
    with(std::ptr::null_mut(), |s| {
        match index(i).and_then(|i| s.config.settings().trackers.addresses.get(i)) {
            Some(a) => into_raw(a.clone()),
            None => std::ptr::null_mut(),
        }
    })
}

// --------------------------------------------------------------- strings --

fn into_raw(s: String) -> *mut c_char {
    // A NUL inside a setting cannot come from the file — TOML strings can hold
    // one, but nothing writes it and the migration refuses the escape that
    // would produce it. Truncating at the first NUL is the safe reading.
    let bytes = s.into_bytes();
    let end = bytes.iter().position(|b| *b == 0).unwrap_or(bytes.len());
    CString::new(&bytes[..end]).unwrap_or_default().into_raw()
}

/// Free a string returned by any getter here.
///
/// # Safety
/// `s` must have come from this module and must not be freed twice. It is
/// Rust-allocated, so `g_free` on it is undefined — copy it first if the value
/// needs to outlive the call.
#[no_mangle]
pub unsafe extern "C" fn hxconfig_free_string(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}

/// Reset to compiled-in defaults with no file behind them. Test support: the
/// unit binaries that link this crate need a loaded state without a config
/// directory to write into.
#[no_mangle]
pub extern "C" fn hxconfig_load_defaults_for_test() {
    CONFIG.with(|c| {
        *c.borrow_mut() = Some(State {
            config: Config::defaults(),
            dir: PathBuf::new(),
            dirty: false,
        });
    });
}

/// The settings as currently loaded. Only used by the tests in this crate.
#[cfg(test)]
pub(crate) fn snapshot() -> Option<crate::Settings> {
    CONFIG.with(|c| c.borrow().as_ref().map(|s| s.config.settings().clone()))
}
