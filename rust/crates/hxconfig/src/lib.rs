//! GtkHx settings: a typed schema over a versioned TOML file.
//!
//! This crate owns `gtkhx.toml` — the values, the file, and the rules about
//! when it may be written. It replaces the `cfgvars[]` address table and the
//! GKeyFile behind it. Connections keep their own file in `hxbookmarks`: that
//! one holds plaintext passwords, wants tighter permissions, and deliberately
//! *refuses* to save when corrupt where settings should fall back to defaults.
//! Storage layout and UI layout are unrelated decisions — Settings still
//! presents both together.
//!
//! Four defects of the old system are fixed structurally rather than by care:
//!
//! - **Unknown keys and comments survive a save.** The document is edited in
//!   place, not rebuilt from the schema, so hand-editing the file is safe.
//! - **Escapes don't grow.** One library handles both directions, so there is
//!   no writer-escapes / reader-doesn't asymmetry for a backslash to
//!   accumulate through.
//! - **Malformed values are diagnosed, not silently zeroed.** See
//!   [`Warning`].
//! - **The version field is actually checked.** See [`Provenance`].
//!
//! The config directory is a parameter, not a global, so everything here
//! unit-tests headless against temp dirs. Who resolves that path is a question
//! for when the crate is wired up; today all three config stores ask C.
//!
//! ```no_run
//! # use std::path::Path;
//! let mut config = hxconfig::Config::load(Path::new("/tmp/gtkhx"));
//! for w in config.warnings() {
//!     eprintln!("gtkhx.toml: {w}");
//! }
//! config.settings_mut().chat.timestamp = true;
//! config.save(Path::new("/tmp/gtkhx")).unwrap();
//! ```

mod atomic;
mod fields;
pub mod legacy;
pub mod migrate;
pub mod schema;
mod warning;

#[cfg(test)]
mod tests;

pub use fields::{kind_of, Kind, FIELDS, PATHS};
pub use schema::*;
pub use warning::{Warning, WarningKind};

use std::path::{Path, PathBuf};
use toml_edit::{DocumentMut, Item, Value};

/// The schema version this build writes.
pub const CURRENT_VERSION: u32 = 1;

/// The settings file's name inside the config directory.
pub const FILE_NAME: &str = "gtkhx.toml";

/// Where an unparsable file is moved aside to on the next save. Settings are
/// reconstructible, so a corrupt file falls back to defaults and the client
/// carries on — but a hand-edited file with one typo in it is still worth
/// keeping, and dropping it would recreate the data-loss problem this crate
/// exists to fix.
pub const SALVAGE_SUFFIX: &str = ".corrupt";

/// The settings file's full path inside `config_dir`.
pub fn path(config_dir: &Path) -> PathBuf {
    config_dir.join(FILE_NAME)
}

/// Where the loaded settings came from, and what that implies about saving.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Provenance {
    /// No file on disk. The settings are the compiled-in defaults, and the
    /// first save creates the file.
    Fresh,
    /// Loaded at the current schema version.
    Current,
    /// Loaded from an older schema version and migrated forward. The next save
    /// rewrites the file at [`CURRENT_VERSION`].
    Migrated { from: u32 },
    /// The file declares a newer schema version. What parses is loaded so the
    /// client is usable, but saving is refused until
    /// [`Config::acknowledge_newer`] — silently rewriting a newer file at an
    /// older schema is how someone who tried a newer build loses settings when
    /// they go back.
    Newer { found: u32 },
    /// The file exists but could not be read or parsed. The settings are the
    /// defaults; the next save moves the bad file aside (see
    /// [`SALVAGE_SUFFIX`]) before writing.
    Unusable,
    /// There was no `gtkhx.toml`, and the settings came from an old `gtkhxrc`
    /// instead. Nothing is on disk in the new format until the caller saves.
    Imported { form: legacy::Form },
}

/// A loaded settings file: the typed values, the document they came from, and
/// everything that went wrong on the way in.
#[derive(Debug, Clone)]
pub struct Config {
    settings: Settings,
    /// The parsed document, retained so a save can edit it in place and leave
    /// unknown keys, comments and formatting untouched.
    doc: DocumentMut,
    provenance: Provenance,
    warnings: Vec<Warning>,
    /// Set once the caller has accepted overwriting a newer file.
    newer_acknowledged: bool,
}

impl Config {
    /// Load `gtkhx.toml` from `config_dir`.
    ///
    /// Infallible by design: every failure mode degrades to defaults and
    /// records a [`Warning`]. Inspect [`Config::provenance`] and
    /// [`Config::warnings`] to find out what happened.
    pub fn load(config_dir: &Path) -> Config {
        Config::from_path(&path(config_dir))
    }

    fn from_path(file: &Path) -> Config {
        let mut warnings = Vec::new();

        let text = match std::fs::read_to_string(file) {
            Ok(text) => text,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                return Config::fresh();
            }
            Err(e) => {
                warnings.push(Warning {
                    path: String::new(),
                    kind: WarningKind::Unreadable(e.to_string()),
                });
                return Config {
                    provenance: Provenance::Unusable,
                    warnings,
                    ..Config::fresh()
                };
            }
        };

        let mut doc: DocumentMut = match text.parse() {
            Ok(doc) => doc,
            Err(e) => {
                warnings.push(Warning {
                    path: String::new(),
                    kind: WarningKind::Unparsable(e.to_string()),
                });
                return Config {
                    provenance: Provenance::Unusable,
                    warnings,
                    ..Config::fresh()
                };
            }
        };

        let found = read_version(&doc, &mut warnings);
        let provenance = match found.cmp(&CURRENT_VERSION) {
            std::cmp::Ordering::Equal => Provenance::Current,
            std::cmp::Ordering::Less => {
                migrate_schema_version(&mut doc, found, &mut warnings);
                Provenance::Migrated { from: found }
            }
            std::cmp::Ordering::Greater => {
                warnings.push(Warning {
                    path: "version".into(),
                    kind: WarningKind::NewerVersion {
                        found,
                        current: CURRENT_VERSION,
                    },
                });
                Provenance::Newer { found }
            }
        };

        Config::from_document(doc, provenance, warnings)
    }

    /// Read a document into typed settings.
    ///
    /// Shared by the load path and the migration, which is the point: a
    /// migrated file gets the same type conversion, range checks and
    /// diagnostics as one read off disk, rather than a second implementation
    /// of them that could disagree.
    fn from_document(
        doc: DocumentMut,
        provenance: Provenance,
        mut warnings: Vec<Warning>,
    ) -> Config {
        // Start from the defaults and overlay whatever the document supplies,
        // so a key it omits keeps its default rather than becoming zero.
        let mut settings = Settings::default();
        let mut reader = fields::Reader::new(&doc, &mut warnings);
        reader.check_table_prefixes();
        fields::read_all(&mut reader, &mut settings);

        Config {
            settings,
            doc,
            provenance,
            warnings,
            newer_acknowledged: false,
        }
    }

    fn fresh() -> Config {
        Config {
            settings: Settings::default(),
            doc: DocumentMut::new(),
            provenance: Provenance::Fresh,
            warnings: Vec::new(),
            newer_acknowledged: false,
        }
    }

    /// Load `gtkhx.toml`, importing an old `gtkhxrc` if there isn't one yet.
    ///
    /// `legacy_paths` are tried in order and the first that exists wins — the
    /// caller supplies `$CONFIG/gtkhxrc` then `$HOME/.gtkhxrc`, matching where
    /// the C reader looks.
    ///
    /// **The old file is left exactly where it is.** Not deleted, not renamed,
    /// not rewritten. If this build turns out to be a mistake, the previous one
    /// still starts up with the user's settings intact — which is also how the
    /// legacy bookmark import behaves, and for the same reason.
    ///
    /// Importing does not save. The caller decides when to write, so a failed
    /// first run doesn't leave a half-migrated file behind.
    pub fn load_or_migrate(config_dir: &Path, legacy_paths: &[PathBuf]) -> Config {
        let config = Config::load(config_dir);
        // Anything other than "no file at all" means the new format is already
        // in charge. A file that exists but is corrupt deliberately does *not*
        // fall back to the old one: the salvage path preserves it and the user
        // carries on at defaults, and silently reverting to a years-old
        // gtkhxrc would be a far stranger thing to do.
        if config.provenance != Provenance::Fresh {
            return config;
        }

        for file in legacy_paths {
            // Bytes, not a string: a legacy file need not be UTF-8, and
            // `read_to_string` would reject the whole thing. Treating that as
            // "no file" would silently reset every setting the user had, once,
            // with no way back. See `legacy::decode`.
            let bytes = match std::fs::read(file) {
                Ok(bytes) => bytes,
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => continue,
                Err(e) => {
                    // Exists but can't be read — permissions, a directory in
                    // its place. That is not the same as absent, and falling
                    // through to an older file would be the wrong answer, so
                    // stop here and say why.
                    return Config {
                        warnings: vec![Warning {
                            path: file.display().to_string(),
                            kind: WarningKind::Unreadable(e.to_string()),
                        }],
                        ..config
                    };
                }
            };

            // The first file that *exists* decides, even if it turns out to
            // hold nothing. The C reader works the same way, and "your current
            // file was empty so I used your twenty-year-old one" would be a
            // strange thing to do unasked.
            let mut warnings = Vec::new();
            let (text, not_utf8) = legacy::decode(&bytes);
            if not_utf8 {
                warnings.push(Warning {
                    path: file.display().to_string(),
                    kind: WarningKind::NotUtf8,
                });
            }
            let (keys, form) = legacy::parse(&text);
            if keys.is_empty() {
                return Config { warnings, ..config };
            }
            let doc = migrate::to_document(&keys, &mut warnings);
            return Config::from_document(doc, Provenance::Imported { form }, warnings);
        }

        config
    }

    /// Compiled-in defaults, backed by an empty document. Useful for tests and
    /// for the first-run path.
    pub fn defaults() -> Config {
        Config::fresh()
    }

    pub fn settings(&self) -> &Settings {
        &self.settings
    }

    pub fn settings_mut(&mut self) -> &mut Settings {
        &mut self.settings
    }

    pub fn provenance(&self) -> &Provenance {
        &self.provenance
    }

    pub fn warnings(&self) -> &[Warning] {
        &self.warnings
    }

    /// Whether [`Config::save`] will write. False only for a file from a newer
    /// schema that the caller hasn't accepted overwriting yet.
    pub fn can_save(&self) -> bool {
        !matches!(self.provenance, Provenance::Newer { .. }) || self.newer_acknowledged
    }

    /// Accept overwriting a file written by a newer build, downgrading it to
    /// this build's schema. Call after the user has been told what they are
    /// about to lose.
    pub fn acknowledge_newer(&mut self) {
        self.newer_acknowledged = true;
    }

    /// Render the current settings as the file's contents would be.
    ///
    /// Separate from [`Config::save`] so the serialization can be tested
    /// without a filesystem, and so a caller can diff before writing.
    pub fn to_toml(&self) -> String {
        let mut doc = self.doc.clone();
        set_version(&mut doc, CURRENT_VERSION);
        fields::write_all(&mut fields::Writer::new(&mut doc), &self.settings);
        doc.to_string()
    }

    /// Write the settings to `config_dir`, atomically.
    ///
    /// Refuses when [`Config::can_save`] is false. When the loaded file was
    /// unusable, the original is moved aside to `gtkhx.toml.corrupt` first —
    /// best-effort, since failing to preserve it is not a reason to refuse the
    /// save.
    pub fn save(&mut self, config_dir: &Path) -> Result<(), SaveError> {
        if !self.can_save() {
            return Err(SaveError::NewerSchema);
        }
        let file = path(config_dir);

        if self.provenance == Provenance::Unusable {
            let mut salvage = file.clone().into_os_string();
            salvage.push(SALVAGE_SUFFIX);
            let _ = std::fs::rename(&file, PathBuf::from(salvage));
        }

        let rendered = self.to_toml();
        atomic::write_atomic(&file, rendered.as_bytes()).map_err(SaveError::Io)?;

        // The file on disk is now this build's schema, whatever it was before,
        // so a second save in the same run isn't still salvaging or migrating —
        // and the load-time diagnostics all describe a file that no longer
        // exists, so a caller re-rendering them would be showing resolved
        // problems.
        self.doc = rendered.parse().unwrap_or_else(|_| self.doc.clone());
        self.provenance = Provenance::Current;
        self.newer_acknowledged = false;
        self.warnings.clear();
        Ok(())
    }
}

#[derive(Debug)]
pub enum SaveError {
    /// The loaded file came from a newer schema and the overwrite hasn't been
    /// acknowledged.
    NewerSchema,
    Io(std::io::Error),
}

impl std::fmt::Display for SaveError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SaveError::NewerSchema => write!(
                f,
                "refusing to overwrite a settings file from a newer version of GtkHx"
            ),
            SaveError::Io(e) => write!(f, "{e}"),
        }
    }
}

impl std::error::Error for SaveError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            SaveError::NewerSchema => None,
            SaveError::Io(e) => Some(e),
        }
    }
}

// -------------------------------------------------------------- versioning --

/// Read the top-level `version` key.
///
/// The two failure modes get different answers, and the difference only starts
/// to matter once there is a version 2:
///
/// - **Missing** → 1. A file without the key predates the key, so it is the
///   first schema and must go through the migration chain. Answering "current"
///   here would look harmless today and quietly skip migrating a real v1 file
///   later.
/// - **Malformed** → current. Someone mistyped the version of a file that is
///   otherwise this build's, and running it through every migration in the
///   chain would do more damage than leaving it alone.
fn read_version(doc: &DocumentMut, warnings: &mut Vec<Warning>) -> u32 {
    let Some(item) = doc.as_item().as_table_like().and_then(|t| t.get("version")) else {
        return 1;
    };
    match item.as_integer() {
        Some(v) if v >= 1 && v <= u32::MAX as i64 => v as u32,
        _ => {
            warnings.push(Warning {
                path: "version".into(),
                kind: WarningKind::BadVersion(
                    item.as_value()
                        .map(|v| v.to_string().trim().to_string())
                        .unwrap_or_else(|| "(not a value)".into()),
                ),
            });
            CURRENT_VERSION
        }
    }
}

fn set_version(doc: &mut DocumentMut, version: u32) {
    let slot = &mut doc["version"];
    if let Some(old) = slot.as_value() {
        if old.as_integer() == Some(version as i64) {
            return;
        }
    }
    *slot = Item::Value(Value::from(version as i64));
    // TOML requires a top-level scalar to precede any table header, and this
    // key may have just been created after them.
    doc.as_table_mut().sort_values_by(|k1, _, k2, _| {
        let rank = |k: &str| if k == "version" { 0 } else { 1 };
        rank(k1.get()).cmp(&rank(k2.get()))
    });
}

/// One step of the migration chain, rewriting keys inside the document.
///
/// Steps work on the document rather than the typed struct on purpose: the
/// typed read happens afterwards, against the already-migrated document, so a
/// step never has to know what the schema became — only what it was.
type MigrationStep = fn(&mut DocumentMut, &mut Vec<Warning>);

/// `STEPS[i]` brings a file from version `i + 1` to version `i + 2`, so the
/// list is always [`CURRENT_VERSION`] − 1 long — empty today, because version 1
/// is the first schema and there is nothing below it to come from.
///
/// **Adding the first step is the moment two things that look equivalent today
/// stop being so.** [`read_version`] resolves a missing key to 1 and a
/// malformed one to [`CURRENT_VERSION`]; while `CURRENT_VERSION` is 1 those are
/// the same number, and from the bump onwards they are not. A file with no
/// `version` key will start going through this chain, which is the intended
/// behaviour and is pinned by a test against `read_version` itself rather than
/// through the observable provenance.
const STEPS: &[MigrationStep] = &[];

/// Bring a document forward from `from` to [`CURRENT_VERSION`], one step at a
/// time.
///
/// Not to be confused with the [`migrate`] module, which converts an old
/// `gtkhxrc` into this format. This one moves an already-TOML file between
/// schema versions.
fn migrate_schema_version(doc: &mut DocumentMut, from: u32, warnings: &mut Vec<Warning>) {
    for step in STEPS.iter().skip(from.saturating_sub(1) as usize) {
        step(doc, warnings);
    }
}
