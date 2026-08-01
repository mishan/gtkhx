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
    /// The `version` key is missing or is not a positive integer. Treated as
    /// the current version, on the grounds that a hand-written file that
    /// forgot the key is far more likely than a file from another schema.
    BadVersion(String),
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
        }
    }
}
