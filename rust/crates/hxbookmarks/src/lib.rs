//! `hxbookmarks` — GtkHx's bookmark model, persistence, and legacy import/
//! export.
//!
//! Bookmarks live in a single human-editable TOML file at
//! `$XDG_CONFIG_HOME/gtkhx/bookmarks.toml` (an ordered list). This crate owns:
//!
//!  * the [`Bookmark`] / [`Store`] model (serde),
//!  * TOML load/save ([`load_or_bootstrap`], [`save`]),
//!  * first-run bootstrap: when the TOML file doesn't exist yet, import any
//!    legacy per-file HTsc bookmarks (see [`legacy`]) and seed the
//!    [`builtins`] — after which they're ordinary entries the user can edit
//!    or delete,
//!  * legacy HTsc import/export for interop with old clients.
//!
//! Pure Rust (serde + toml), no glib/gtk — the config directory is passed in,
//! so everything here is unit-tested headless against temp dirs.

pub mod cipher;
pub mod legacy;
mod hostport;

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

/// Current on-disk schema version for `bookmarks.toml`.
pub const STORE_VERSION: u32 = 1;

/// One bookmark. Fields mirror the old C `HxBookmark` but idiomatic: host and
/// port are separate, HOPE/TLS are bools, cipher/compress stay as their
/// stable byte / index (see [`cipher`]).
#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq, Eq)]
pub struct Bookmark {
    /// Display name and identity key (unique within a store).
    pub name: String,
    /// Hostname or IP (no port).
    pub server: String,
    /// ASCII port; empty means the Hotline default (5500).
    #[serde(default)]
    pub port: String,
    #[serde(default)]
    pub login: String,
    #[serde(default)]
    pub password: String,
    /// HOPE encryption on (the old `secure` byte).
    #[serde(default)]
    pub hope: bool,
    /// 0 = off; 1+ indexes the compressor vocabulary.
    #[serde(default)]
    pub compress: u8,
    /// Stable cipher byte (see [`cipher`]).
    #[serde(default)]
    pub cipher: u8,
    /// TLS over the server's dedicated TLS port.
    #[serde(default)]
    pub tls: bool,
}

/// The whole bookmarks file: a version tag plus an ordered list.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Store {
    pub version: u32,
    #[serde(default)]
    pub bookmarks: Vec<Bookmark>,
}

impl Default for Store {
    fn default() -> Self {
        Store {
            version: STORE_VERSION,
            bookmarks: Vec::new(),
        }
    }
}

impl Store {
    /// Find a bookmark by name.
    pub fn find(&self, name: &str) -> Option<&Bookmark> {
        self.bookmarks.iter().find(|b| b.name == name)
    }

    /// Bookmark names in stored (display) order.
    pub fn names(&self) -> Vec<String> {
        self.bookmarks.iter().map(|b| b.name.clone()).collect()
    }

    /// Insert `bm`, or replace the existing entry with the same name in place
    /// (keeping its position). Empty-named bookmarks are rejected.
    pub fn upsert(&mut self, bm: Bookmark) -> bool {
        if bm.name.is_empty() {
            return false;
        }
        match self.bookmarks.iter_mut().find(|b| b.name == bm.name) {
            Some(slot) => *slot = bm,
            None => self.bookmarks.push(bm),
        }
        true
    }

    /// Remove the bookmark named `name`. Returns whether one was removed.
    pub fn remove(&mut self, name: &str) -> bool {
        let before = self.bookmarks.len();
        self.bookmarks.retain(|b| b.name != name);
        self.bookmarks.len() != before
    }

    /// Rename `old` → `new` in place. Errors if `old` is absent or `new` is
    /// already taken by a different entry.
    pub fn rename(&mut self, old: &str, new: &str) -> Result<(), RenameError> {
        if old == new {
            return Ok(());
        }
        if new.is_empty() {
            return Err(RenameError::EmptyName);
        }
        if self.find(new).is_some() {
            return Err(RenameError::Exists);
        }
        let slot = self
            .bookmarks
            .iter_mut()
            .find(|b| b.name == old)
            .ok_or(RenameError::NotFound)?;
        slot.name = new.to_string();
        Ok(())
    }
}

/// Why a [`Store::rename`] failed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RenameError {
    NotFound,
    Exists,
    EmptyName,
}

impl std::fmt::Display for RenameError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let s = match self {
            RenameError::NotFound => "no such bookmark",
            RenameError::Exists => "a bookmark with that name already exists",
            RenameError::EmptyName => "bookmark name required",
        };
        f.write_str(s)
    }
}

impl std::error::Error for RenameError {}

/// The bookmarks TOML path for a config directory.
pub fn store_path(config_dir: &Path) -> PathBuf {
    config_dir.join("bookmarks.toml")
}

/// The bookmarks GtkHx ships with. Seeded into the TOML the first time it's
/// created (requirement: they become ordinary, user-removable entries).
pub fn builtins() -> Vec<Bookmark> {
    vec![Bookmark {
        name: "Hotline Communications".to_string(),
        server: "hlserver.com".to_string(),
        ..Default::default()
    }]
}

/// Load the store, or bootstrap it on first run.
///
/// If `bookmarks.toml` exists it's parsed and returned (a parse error yields
/// an empty store *without* overwriting the file, so a hand-edit typo never
/// destroys the user's data). Otherwise this imports every legacy HTsc
/// bookmark found in `legacy_dirs` (in order), appends any [`builtins`] whose
/// names aren't already taken, writes the new TOML, and returns it.
pub fn load_or_bootstrap(config_dir: &Path, legacy_dirs: &[PathBuf]) -> Store {
    let path = store_path(config_dir);
    if path.exists() {
        return match fs::read_to_string(&path) {
            Ok(text) => toml::from_str(&text).unwrap_or_default(),
            Err(_) => Store::default(),
        };
    }

    let mut store = Store::default();
    for dir in legacy_dirs {
        for bm in legacy::import_dir(dir) {
            // Later dirs don't shadow earlier ones with the same name.
            if store.find(&bm.name).is_none() {
                store.bookmarks.push(bm);
            }
        }
    }
    for bm in builtins() {
        if store.find(&bm.name).is_none() {
            store.bookmarks.push(bm);
        }
    }

    let _ = save(config_dir, &store);
    store
}

/// Write the store to `bookmarks.toml` atomically (temp file + rename), so a
/// crash mid-write can't truncate the user's bookmarks. Creates `config_dir`
/// if needed.
pub fn save(config_dir: &Path, store: &Store) -> io::Result<()> {
    fs::create_dir_all(config_dir)?;
    let text = toml::to_string_pretty(store)
        .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
    let final_path = store_path(config_dir);
    let tmp_path = config_dir.join("bookmarks.toml.tmp");
    fs::write(&tmp_path, text.as_bytes())?;
    fs::rename(&tmp_path, &final_path)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmpdir() -> PathBuf {
        let mut p = std::env::temp_dir();
        p.push(format!(
            "hxbookmarks-test-{}-{}",
            std::process::id(),
            // A cheap unique-ish suffix so parallel tests don't collide.
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir_all(&p).unwrap();
        p
    }

    fn bm(name: &str, server: &str) -> Bookmark {
        Bookmark {
            name: name.into(),
            server: server.into(),
            ..Default::default()
        }
    }

    #[test]
    fn toml_round_trips() {
        let mut store = Store::default();
        store.upsert(Bookmark {
            name: "S1".into(),
            server: "a.example".into(),
            port: "5501".into(),
            login: "u".into(),
            password: "p".into(),
            hope: true,
            compress: 2,
            cipher: cipher::CHACHA20_POLY1305,
            tls: true,
        });
        store.upsert(bm("S2", "b.example"));

        let text = toml::to_string_pretty(&store).unwrap();
        assert!(text.contains("[[bookmarks]]"));
        let back: Store = toml::from_str(&text).unwrap();
        assert_eq!(back, store);
    }

    #[test]
    fn upsert_replaces_in_place() {
        let mut store = Store::default();
        store.upsert(bm("A", "1"));
        store.upsert(bm("B", "2"));
        store.upsert(bm("A", "updated"));
        assert_eq!(store.bookmarks.len(), 2);
        assert_eq!(store.find("A").unwrap().server, "updated");
        // position preserved (A still first)
        assert_eq!(store.bookmarks[0].name, "A");
    }

    #[test]
    fn remove_and_rename() {
        let mut store = Store::default();
        store.upsert(bm("A", "1"));
        store.upsert(bm("B", "2"));
        assert!(store.remove("A"));
        assert!(!store.remove("A"));
        assert_eq!(store.rename("B", "C"), Ok(()));
        assert!(store.find("C").is_some());
        store.upsert(bm("D", "4"));
        assert_eq!(store.rename("D", "C"), Err(RenameError::Exists));
        assert_eq!(store.rename("nope", "x"), Err(RenameError::NotFound));
    }

    #[test]
    fn bootstrap_seeds_builtins_and_writes_file() {
        let dir = tmpdir();
        let store = load_or_bootstrap(&dir, &[]);
        // built-in present
        assert!(store.find("Hotline Communications").is_some());
        // file now exists
        assert!(store_path(&dir).exists());
        // second load reads the file (doesn't re-bootstrap)
        let again = load_or_bootstrap(&dir, &[]);
        assert_eq!(again, store);
    }

    #[test]
    fn bootstrap_imports_legacy_then_seeds() {
        let cfg = tmpdir();
        let legacy = tmpdir();
        // Drop a legacy HTsc file into the legacy dir.
        let old = Bookmark {
            name: String::new(),
            server: "legacy.example".into(),
            port: "5500".into(),
            ..Default::default()
        };
        fs::write(legacy.join("My Old Server"), legacy::write(&old)).unwrap();

        let store = load_or_bootstrap(&cfg, &[legacy.clone()]);
        // imported (name from filename) + built-in both present, import first
        assert_eq!(store.bookmarks[0].name, "My Old Server");
        assert_eq!(store.bookmarks[0].server, "legacy.example");
        assert!(store.find("Hotline Communications").is_some());
    }

    #[test]
    fn corrupt_toml_does_not_clobber() {
        let dir = tmpdir();
        fs::write(store_path(&dir), b"this is not valid toml {{{").unwrap();
        let store = load_or_bootstrap(&dir, &[]);
        assert!(store.bookmarks.is_empty());
        // file untouched (still the bad bytes) — user can fix it
        let raw = fs::read_to_string(store_path(&dir)).unwrap();
        assert!(raw.starts_with("this is not valid toml"));
    }

    #[test]
    fn save_is_atomic_and_reloadable() {
        let dir = tmpdir();
        let mut store = Store::default();
        store.upsert(bm("X", "x.example"));
        save(&dir, &store).unwrap();
        assert!(!dir.join("bookmarks.toml.tmp").exists());
        let back = load_or_bootstrap(&dir, &[]);
        assert_eq!(back.find("X").unwrap().server, "x.example");
    }
}
