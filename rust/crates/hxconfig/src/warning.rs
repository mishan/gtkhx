//! Load-time diagnostics.
//!
//! Loading never fails: a settings file is reconstructible, so a bad one
//! degrades to defaults rather than stopping the client. But it must not
//! degrade *silently* — the old reader turned every malformed number into zero
//! through `atoi` with no diagnostic at all, and a boolean regression that
//! reverted every toggle to its default on every startup shipped because of
//! exactly that quiet. So each thing that could not be used produces a warning
//! the caller can log or surface.

use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Warning {
    /// The dotted path the problem is at, e.g. `chat.scrollback_lines`, or
    /// `trackers.addresses[2]` for one bad element of a list. Empty when the
    /// problem is the file as a whole.
    pub path: String,
    pub kind: WarningKind,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum WarningKind {
    /// The file is not valid TOML. Settings fall back to defaults; the
    /// unreadable file is preserved beside the new one on the next save.
    Unparsable(String),
    /// The file could not be read at all — permissions, a directory in its
    /// place, a broken symlink.
    Unreadable(String),
    WrongType {
        expected: &'static str,
        found: &'static str,
    },
    OutOfRange {
        value: i64,
        min: i64,
        max: i64,
    },
    UnknownValue {
        value: String,
        allowed: &'static [&'static str],
    },
    /// The file was written by a newer build. What parses is loaded; saving is
    /// refused until the caller acknowledges, because rewriting a newer file at
    /// an older schema is how a user who tried a newer build loses settings on
    /// downgrade.
    NewerVersion {
        found: u32,
        current: u32,
    },
    /// The `version` key is present but is not a positive integer. Treated as
    /// the current version: someone mistyped the version of a file that is
    /// otherwise this build's, and running it through every migration in the
    /// chain would do more damage than leaving it alone.
    ///
    /// A *missing* key is not this, and is not a warning at all — it means the
    /// file predates the key, so it is version 1 and goes through the chain
    /// like any other version 1 file.
    BadVersion(String),

    // ---- migrating an old gtkhxrc ----------------------------------------
    /// A key in the old file that no version of the settings table ever had.
    /// Hand-written, or from a fork. Reported rather than dropped in silence,
    /// because it is the one case where the user has something to act on.
    /// `path` holds the old key name.
    UnknownLegacyKey,
    /// An old value that could not be made into the type its new home wants.
    /// The setting keeps its default. The old reader turned an unparseable
    /// number into zero here, with no diagnostic at all.
    UnmigratableValue {
        key: String,
        value: String,
    },
    /// A migrated string still contains a doubled backslash after one round of
    /// unescaping, so the old writer/reader escape asymmetry ran more than
    /// once over it. How many times is unknowable, so the value is handed over
    /// as-is and flagged rather than guessed at.
    OverEscaped {
        key: String,
    },
    /// The old file was not valid UTF-8 and was read as Mac Roman instead.
    /// Not a failure — it is what the C reader did with a nickname that failed
    /// the same check — but the user should know their text was reinterpreted.
    NotUtf8,
}

impl fmt::Display for Warning {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if !self.path.is_empty() {
            write!(f, "{}: ", self.path)?;
        }
        match &self.kind {
            WarningKind::Unparsable(e) => write!(f, "not valid TOML ({e}); using defaults"),
            WarningKind::Unreadable(e) => write!(f, "could not be read ({e}); using defaults"),
            WarningKind::WrongType { expected, found } => {
                write!(f, "expected {expected}, found {found}; keeping the default")
            }
            WarningKind::OutOfRange { value, min, max } => {
                write!(f, "{value} is outside {min}..={max}; keeping the default")
            }
            WarningKind::UnknownValue { value, allowed } => {
                write!(
                    f,
                    "{value:?} is not one of {}; keeping the default",
                    allowed.join(", ")
                )
            }
            WarningKind::NewerVersion { found, current } => write!(
                f,
                "file is schema version {found}, this build understands {current}; \
                 loaded read-only"
            ),
            WarningKind::BadVersion(found) => {
                write!(
                    f,
                    "version {found} is not a positive integer; assuming current"
                )
            }
            WarningKind::UnknownLegacyKey => {
                write!(f, "no setting by this name; not migrated")
            }
            WarningKind::UnmigratableValue { key, value } => write!(
                f,
                "{key}={value:?} in the old gtkhxrc is not a value this setting \
                 can take; keeping the default"
            ),
            WarningKind::OverEscaped { key } => write!(
                f,
                "migrated from {key} and still contains a doubled backslash, so \
                 the old escaping bug may have run over it more than once — \
                 check the value. (A Windows UNC path legitimately looks like \
                 this, in which case it is already correct.)"
            ),
            WarningKind::NotUtf8 => write!(
                f,
                "is not UTF-8; read as Mac Roman, which is what the old client \
                 assumed for a nickname that failed the same check"
            ),
        }
    }
}
