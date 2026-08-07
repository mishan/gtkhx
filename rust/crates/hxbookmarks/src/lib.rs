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
mod hostport;
pub mod legacy;

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

    /// Show a different nickname on this server than the global default.
    ///
    /// `None` means inherit, and is the normal case — the connect path
    /// resolves `override ?? global` once and copies the answer into the
    /// connection, so nothing aliases anything and `/nick` can change the
    /// running connection without touching what is stored.
    ///
    /// `skip_serializing_if` is load-bearing rather than tidiness: `toml` 1.x
    /// refuses to serialize a `None` in a struct field, so without it every
    /// bookmark that inherits would fail to save. It also means the file says
    /// nothing at all about an inherited value, which is what makes deleting
    /// the line the way to go back to the default.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub nick: Option<String>,

    /// Show a different icon on this server. `None` means inherit.
    ///
    /// Distinct from an icon of `0`, which is a real (blank) icon someone can
    /// legitimately choose — which is the whole reason this is an `Option`
    /// rather than a sentinel.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub icon: Option<u16>,
}

fn default_version() -> u32 {
    STORE_VERSION
}

/// The whole bookmarks file: a version tag plus an ordered list.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Store {
    /// Schema version. Defaults to the current [`STORE_VERSION`] when the key
    /// is absent, so a hand-edit that drops the `version` line still loads.
    #[serde(default = "default_version")]
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
    vec![
        Bookmark {
            name: "Hotline Central Hub".to_string(),
            server: "server.bigredh.com".to_string(),
            tls: true,
            port: "5600".to_string(),
            ..Default::default()
        },
        Bookmark {
            name: "Hotline Communications".to_string(),
            server: "hlserver.com".to_string(),
            ..Default::default()
        },
        Bookmark {
            name: "chatonly.org".to_string(),
            server: "chatonly.org".to_string(),
            ..Default::default()
        },
        Bookmark {
            name: "Classic Macs Hotline Server".to_string(),
            server: "macos.retro-os.live".to_string(),
            tls: true,
            port: "5600".to_string(),
            ..Default::default()
        },
        Bookmark {
            name: "Mobius Strip".to_string(),
            server: "hotline.morphing.cloud".to_string(),
            ..Default::default()
        },
        Bookmark {
            name: "VesperNet".to_string(),
            server: "hotline.vespernet.net".to_string(),
            tls: true,
            port: "5600".to_string(),
            ..Default::default()
        },
    ]
}

/// The existing `bookmarks.toml` couldn't be read or parsed. Carries the
/// path and a human-readable reason. Callers must **not** overwrite the file
/// on this error — surface it and let the user fix the file by hand, so a
/// single typo (or a transient read error) can't destroy real bookmarks.
#[derive(Debug, Clone)]
pub struct Unreadable {
    pub path: PathBuf,
    pub reason: String,
}

impl std::fmt::Display for Unreadable {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}: {}", self.path.display(), self.reason)
    }
}

impl std::error::Error for Unreadable {}

/// Load the store, or bootstrap it on first run.
///
/// - `bookmarks.toml` present + readable → parse and return it.
/// - `bookmarks.toml` present but unreadable / unparseable → [`Unreadable`]
///   (the caller must not overwrite it — this is what keeps a hand-edit typo
///   from being turned into an empty store on the next save).
/// - `bookmarks.toml` absent → import every legacy HTsc bookmark found in
///   `legacy_dirs` (in order), append any [`builtins`] whose names aren't
///   already taken, write the new TOML, and return it.
pub fn load_or_bootstrap(config_dir: &Path, legacy_dirs: &[PathBuf]) -> Result<Store, Unreadable> {
    let path = store_path(config_dir);
    if path.exists() {
        let text = fs::read_to_string(&path).map_err(|e| Unreadable {
            path: path.clone(),
            reason: e.to_string(),
        })?;
        let store: Store = toml::from_str(&text).map_err(|e| Unreadable {
            path: path.clone(),
            reason: e.to_string(),
        })?;
        return Ok(store);
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

    // Best-effort write of the freshly-bootstrapped file. A failure here isn't
    // data loss (no file existed), so we still return the in-memory store.
    let _ = save(config_dir, &store);
    Ok(store)
}

/// Write the store to `bookmarks.toml` atomically (temp file + rename), so a
/// crash mid-write can't truncate the user's bookmarks. Creates `config_dir`
/// if needed.
pub fn save(config_dir: &Path, store: &Store) -> io::Result<()> {
    fs::create_dir_all(config_dir)?;
    let text =
        toml::to_string_pretty(store).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
    let final_path = store_path(config_dir);
    let tmp_path = config_dir.join("bookmarks.toml.tmp");
    fs::write(&tmp_path, text.as_bytes())?;
    // On a rename failure (permissions, cross-device, …) don't leave the
    // half-written temp file lying around to confuse the user or a later save.
    if let Err(e) = fs::rename(&tmp_path, &final_path) {
        let _ = fs::remove_file(&tmp_path);
        return Err(e);
    }
    Ok(())
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
            nick: None,
            icon: None,
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
        let store = load_or_bootstrap(&dir, &[]).unwrap();
        // built-in present
        assert!(store.find("Hotline Communications").is_some());
        // file now exists
        assert!(store_path(&dir).exists());
        // second load reads the file (doesn't re-bootstrap)
        let again = load_or_bootstrap(&dir, &[]).unwrap();
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

        let store = load_or_bootstrap(&cfg, std::slice::from_ref(&legacy)).unwrap();
        // imported (name from filename) + built-in both present, import first
        assert_eq!(store.bookmarks[0].name, "My Old Server");
        assert_eq!(store.bookmarks[0].server, "legacy.example");
        assert!(store.find("Hotline Communications").is_some());
    }

    #[test]
    fn corrupt_toml_errors_and_does_not_clobber() {
        let dir = tmpdir();
        fs::write(store_path(&dir), b"this is not valid toml {{{").unwrap();
        // An unreadable existing file must be an error, NOT an empty store —
        // otherwise a load→mutate→save would wipe the user's real data.
        let err = load_or_bootstrap(&dir, &[]).unwrap_err();
        assert_eq!(err.path, store_path(&dir));
        // file untouched (still the bad bytes) — user can fix it by hand
        let raw = fs::read_to_string(store_path(&dir)).unwrap();
        assert!(raw.starts_with("this is not valid toml"));
    }

    #[test]
    fn missing_version_key_defaults() {
        // A human-edited file that dropped the `version` line still parses.
        let dir = tmpdir();
        fs::write(
            store_path(&dir),
            b"[[bookmarks]]\nname = \"Hand Made\"\nserver = \"h.example\"\n",
        )
        .unwrap();
        let store = load_or_bootstrap(&dir, &[]).unwrap();
        assert_eq!(store.version, STORE_VERSION);
        assert_eq!(store.find("Hand Made").unwrap().server, "h.example");
    }

    #[test]
    fn save_is_atomic_and_reloadable() {
        let dir = tmpdir();
        let mut store = Store::default();
        store.upsert(bm("X", "x.example"));
        save(&dir, &store).unwrap();
        assert!(!dir.join("bookmarks.toml.tmp").exists());
        let back = load_or_bootstrap(&dir, &[]).unwrap();
        assert_eq!(back.find("X").unwrap().server, "x.example");
    }
}

#[cfg(test)]
mod override_tests {
    use super::*;

    fn plain() -> Bookmark {
        Bookmark {
            name: "Server".into(),
            server: "example.com".into(),
            ..Default::default()
        }
    }

    #[test]
    fn an_inherited_identity_is_absent_from_the_file() {
        // Not an empty string: the file should say nothing at all about a
        // bookmark that inherits, so deleting the line is how someone goes
        // back to the default. It is also what `skip_serializing_if` is for —
        // toml 1.x refuses to serialize a None outright, so without it every
        // inheriting bookmark would fail to save.
        let mut store = Store::default();
        store.upsert(plain());
        let text = toml::to_string_pretty(&store).expect("serialize");

        assert!(!text.contains("nick"), "{text}");
        assert!(!text.contains("icon"), "{text}");

        let back: Store = toml::from_str(&text).expect("parse");
        assert_eq!(back.find("Server").unwrap().nick, None);
        assert_eq!(back.find("Server").unwrap().icon, None);
    }

    #[test]
    fn an_override_round_trips() {
        let mut store = Store::default();
        store.upsert(Bookmark {
            nick: Some("someone else".into()),
            icon: Some(410),
            ..plain()
        });
        let text = toml::to_string_pretty(&store).expect("serialize");
        let back: Store = toml::from_str(&text).expect("parse");

        let bm = back.find("Server").expect("found");
        assert_eq!(bm.nick.as_deref(), Some("someone else"));
        assert_eq!(bm.icon, Some(410));
    }

    #[test]
    fn icon_zero_is_an_override_not_an_absence() {
        // Zero is a real, blank icon someone can choose. Storing it as a
        // sentinel for "inherit" is the bug this Option exists to avoid, and
        // the one the global path had for years.
        let mut store = Store::default();
        store.upsert(Bookmark {
            icon: Some(0),
            ..plain()
        });
        let text = toml::to_string_pretty(&store).expect("serialize");
        assert!(text.contains("icon = 0"), "{text}");

        let back: Store = toml::from_str(&text).expect("parse");
        assert_eq!(back.find("Server").unwrap().icon, Some(0));
    }

    #[test]
    fn a_file_written_before_overrides_still_loads() {
        // Additive plus serde(default), so no schema bump was needed — this
        // pins that, since bumping without a migration chain would be worse
        // than leaving the version alone.
        let text = "version = 1\n\n[[bookmarks]]\nname = \"Old\"\nserver = \"a.example\"\n";
        let store: Store = toml::from_str(text).expect("parse");
        let bm = store.find("Old").expect("found");
        assert_eq!(bm.nick, None);
        assert_eq!(bm.icon, None);
    }

    #[test]
    fn the_legacy_format_carries_no_override() {
        // 460 fixed bytes with four flag bytes and nowhere to put one. The
        // export is interop with clients that have no concept of a
        // per-connection nickname, so an override is dropped rather than
        // smuggled into the reserved space.
        let bm = Bookmark {
            nick: Some("someone else".into()),
            icon: Some(410),
            ..plain()
        };
        let bytes = legacy::write(&bm);
        assert_eq!(bytes.len(), 460);

        let mut back = legacy::parse(&bytes).expect("parse");
        back.name = bm.name.clone();
        assert_eq!(back.nick, None);
        assert_eq!(back.icon, None);
        // Everything the format *does* carry still round-trips.
        assert_eq!(back.server, bm.server);
    }
}
