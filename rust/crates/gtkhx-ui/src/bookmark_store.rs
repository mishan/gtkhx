//! Thin glue between the GtkHx dialogs and the [`hxbookmarks`] crate.
//!
//! Resolves the config directory (via the C `gtkhx_config_dir`) and the
//! legacy import locations, then exposes the load/mutate/save operations the
//! Connect and Bookmarks dialogs need. Each mutation loads the whole store,
//! changes it, and writes it back — the file is tiny, so there's no shared
//! mutable state to keep in sync.
//!
//! The store lives in a single TOML file (`$CONFIG/gtkhx/bookmarks.toml`);
//! the first load bootstraps it by importing any legacy per-file HTsc
//! bookmarks and seeding the built-ins (see [`hxbookmarks::load_or_bootstrap`]).

use std::ffi::c_char;
use std::io;
use std::path::{Path, PathBuf};

use gtk4::glib;

use crate::cstr;
use hxbookmarks::{legacy, Store};

pub use hxbookmarks::Bookmark;

extern "C" {
    fn gtkhx_config_dir() -> *const c_char;
}

/// `$XDG_CONFIG_HOME/gtkhx` (honours `GTKHX_PATH`), from the C resolver.
fn config_dir() -> PathBuf {
    PathBuf::from(unsafe { cstr(gtkhx_config_dir()) })
}

/// Directories scanned for legacy per-file HTsc bookmarks on first run: the
/// old per-file dir under the config dir, then the classic `~/.hx/bookmarks`.
fn legacy_dirs() -> Vec<PathBuf> {
    let mut dirs = vec![config_dir().join("bookmarks")];
    let home = std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(glib::home_dir);
    dirs.push(home.join(".hx").join("bookmarks"));
    dirs
}

/// Load the store (bootstrapping + importing legacy bookmarks on first run).
pub fn load() -> Store {
    hxbookmarks::load_or_bootstrap(&config_dir(), &legacy_dirs())
}

/// Persist the store.
pub fn save(store: &Store) -> io::Result<()> {
    hxbookmarks::save(&config_dir(), store)
}

/// Bookmark names in display order.
pub fn names() -> Vec<String> {
    load().names()
}

/// The bookmark named `name`, if present.
pub fn find(name: &str) -> Option<Bookmark> {
    load().find(name).cloned()
}

/// Whether a bookmark named `name` exists.
pub fn exists(name: &str) -> bool {
    find(name).is_some()
}

/// Insert or replace `bm` (keyed on name) and save. Returns a human-readable
/// error message on failure.
pub fn upsert(bm: Bookmark) -> Result<(), String> {
    let mut store = load();
    if !store.upsert(bm) {
        return Err("bookmark name required".to_string());
    }
    save(&store).map_err(|e| e.to_string())
}

/// Remove the bookmark named `name` and save.
pub fn delete(name: &str) -> Result<(), String> {
    let mut store = load();
    if !store.remove(name) {
        return Err(format!("No such bookmark: {name}"));
    }
    save(&store).map_err(|e| e.to_string())
}

/// Rename `old` → `new` and save.
pub fn rename(old: &str, new: &str) -> Result<(), String> {
    let mut store = load();
    store.rename(old, new).map_err(|e| e.to_string())?;
    save(&store).map_err(|e| e.to_string())
}

/// Write every bookmark as a legacy HTsc file into `dir` (the "Export legacy
/// format" action). Returns the number of files written.
pub fn export_legacy(dir: &Path) -> io::Result<usize> {
    legacy::export_dir(&load().bookmarks, dir)
}
