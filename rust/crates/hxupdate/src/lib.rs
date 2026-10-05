//! The pure half of the update check: what a GtkHx version string means and
//! how two of them order, the `updates.json` feed the outside-Flatpak check
//! reads, and the decision of when to ask and what to tell the user. The
//! Flatpak's own decision, from what the portal reports, is in [`flatpak`].
//!
//! Fetching the feed and showing the banner belong to the caller. See
//! docs/updates.md.

pub mod flatpak;

use serde::{Deserialize, Deserializer};
use std::collections::BTreeMap;
use std::time::{Duration, SystemTime};

/// Where a version sits relative to its release. Declaration order is the
/// ordering: the `-dev` tree before the betas, betas before release
/// candidates, and all of them before the release itself.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Stage {
    Dev,
    Beta(u32),
    Rc(u32),
    Release,
}

/// A parsed GtkHx version: `1.4.1`, `1.4.1b2`, `1.4.1rc1` or `1.4.1-dev`,
/// with an optional leading `v` as release tags carry.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct Version {
    pub major: u32,
    pub minor: u32,
    pub patch: u32,
    pub stage: Stage,
}

impl Version {
    /// `None` for anything that isn't exactly one of the shapes above. A
    /// version that can't be read is never newer than anything, so a snapshot
    /// build or a typo in the feed produces no notice rather than a wrong one.
    pub fn parse(s: &str) -> Option<Version> {
        let s = s.strip_prefix('v').unwrap_or(s);
        let split = s
            .find(|c: char| !c.is_ascii_digit() && c != '.')
            .unwrap_or(s.len());
        let (core, suffix) = s.split_at(split);

        let mut parts = core.split('.').map(number);
        let (Some(Some(major)), Some(Some(minor)), Some(Some(patch)), None) =
            (parts.next(), parts.next(), parts.next(), parts.next())
        else {
            return None;
        };

        let stage = match suffix {
            "" => Stage::Release,
            "-dev" => Stage::Dev,
            _ => {
                if let Some(n) = suffix.strip_prefix("rc") {
                    Stage::Rc(number(n)?)
                } else {
                    Stage::Beta(number(suffix.strip_prefix('b')?)?)
                }
            }
        };
        Some(Version {
            major,
            minor,
            patch,
            stage,
        })
    }

    pub fn is_prerelease(&self) -> bool {
        self.stage != Stage::Release
    }
}

/// Digits only: `str::parse` would also take a leading `+`.
fn number(s: &str) -> Option<u32> {
    if s.is_empty() || !s.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    s.parse().ok()
}

/// More than any real feed needs; it keeps a misbehaving server from handing
/// the parser something large.
pub const MAX_FEED_BYTES: usize = 64 * 1024;

/// The only `schema` this reader understands. A later one may mean something
/// else by the same fields, so it is refused rather than guessed at.
pub const FEED_SCHEMA: u32 = 1;

/// `updates.json`. Fields this version doesn't know are ignored, so the feed
/// can grow without breaking the builds already out there.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct Feed {
    pub schema: u32,
    pub channels: Channels,
}

/// A channel that doesn't parse reads as absent, so a mistake in one doesn't
/// silence the other.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct Channels {
    #[serde(default, deserialize_with = "lenient")]
    pub stable: Option<Entry>,
    #[serde(default, deserialize_with = "lenient")]
    pub beta: Option<Entry>,
}

fn lenient<'de, D: Deserializer<'de>>(d: D) -> Result<Option<Entry>, D::Error> {
    let value = serde_json::Value::deserialize(d)?;
    Ok(Entry::deserialize(value).ok())
}

/// One channel's latest release.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct Entry {
    pub version: String,
    #[serde(default)]
    pub date: String,
    #[serde(default)]
    pub notes_url: String,
    /// Package name (e.g. `windows`, `macos-arm64`) to its download URL.
    #[serde(default)]
    pub downloads: BTreeMap<String, String>,
}

#[derive(Debug)]
pub enum FeedError {
    TooLarge,
    Malformed(serde_json::Error),
    UnknownSchema(u32),
}

impl std::fmt::Display for FeedError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            FeedError::TooLarge => write!(f, "feed is larger than {MAX_FEED_BYTES} bytes"),
            FeedError::Malformed(e) => write!(f, "feed is not valid: {e}"),
            FeedError::UnknownSchema(n) => write!(f, "feed schema {n} is not supported"),
        }
    }
}

impl std::error::Error for FeedError {}

pub fn parse_feed(bytes: &[u8]) -> Result<Feed, FeedError> {
    if bytes.len() > MAX_FEED_BYTES {
        return Err(FeedError::TooLarge);
    }
    let feed: Feed = serde_json::from_slice(bytes).map_err(FeedError::Malformed)?;
    if feed.schema != FEED_SCHEMA {
        return Err(FeedError::UnknownSchema(feed.schema));
    }
    Ok(feed)
}

/// What the caller should do next.
#[derive(Debug, PartialEq, Eq)]
pub enum Action<'a> {
    /// Ask the feed now. Until it answers, or if it fails, show `notice` if
    /// there is one: the last answer still says what is out.
    Fetch { notice: Option<&'a Entry> },
    /// Ask again after `after`. Until then, show `notice` if there is one.
    Wait {
        after: Duration,
        notice: Option<&'a Entry>,
    },
}

/// Whether to ask the feed, and what the last answer means for the user.
///
/// `running` is this build's version. A pre-release hears about betas as well
/// as releases; a release hears only about releases, so nobody is moved onto
/// betas who didn't choose one. The notice is the newest entry that is newer
/// than `running` and isn't the version the user said to skip.
///
/// A clock that went backwards past `last_checked` counts as due, so a wrong
/// clock can't stop the check for as long as it was wrong by.
pub fn decide<'a>(
    running: &str,
    skip_version: &str,
    feed: Option<&'a Feed>,
    last_checked: Option<SystemTime>,
    now: SystemTime,
    interval: Duration,
) -> Action<'a> {
    let notice = feed.and_then(|feed| newer_than(running, skip_version, feed));
    match last_checked.and_then(|t| now.duration_since(t).ok()) {
        Some(since) if since < interval => Action::Wait {
            after: interval - since,
            notice,
        },
        _ => Action::Fetch { notice },
    }
}

fn newer_than<'a>(running: &str, skip_version: &str, feed: &'a Feed) -> Option<&'a Entry> {
    let running = Version::parse(running)?;
    let skip = Version::parse(skip_version);
    let channels = &feed.channels;
    let beta = channels.beta.as_ref().filter(|_| running.is_prerelease());
    // Stable last: `max_by_key` keeps the last of equals, and a release and a
    // beta naming the same version should announce the release.
    [beta, channels.stable.as_ref()]
        .into_iter()
        .flatten()
        .filter_map(|e| Some((Version::parse(&e.version)?, e)))
        .filter(|(v, _)| *v > running && Some(*v) != skip)
        .max_by_key(|(v, _)| *v)
        .map(|(_, e)| e)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn versions_order_through_a_release_cycle() {
        let ordered = [
            "1.4.0",
            "1.4.1-dev",
            "1.4.1b1",
            "1.4.1b2",
            "1.4.1b10",
            "1.4.1rc1",
            "1.4.1",
            "1.4.2-dev",
            "1.10.0",
            "2.0.0",
        ];
        for pair in ordered.windows(2) {
            let (a, b) = (Version::parse(pair[0]), Version::parse(pair[1]));
            assert!(a.is_some() && b.is_some(), "{pair:?}");
            assert!(a < b, "{} should sort before {}", pair[0], pair[1]);
        }
    }

    #[test]
    fn parse_accepts_only_the_known_shapes() {
        for (text, expected) in [
            ("v1.3.2", Some((1, 3, 2, Stage::Release))),
            ("1.4.1b1", Some((1, 4, 1, Stage::Beta(1)))),
            ("1.4.1rc3", Some((1, 4, 1, Stage::Rc(3)))),
            ("1.4.1-dev", Some((1, 4, 1, Stage::Dev))),
            ("snapshot-a1b2c3d", None),
            ("", None),
            ("v", None),
            ("1.4", None),
            ("1.4.1.2", None),
            ("1..1", None),
            ("1.4.1b", None),
            ("1.4.1b+1", None),
            ("1.4.1-beta1", None),
            ("1.4.1rc", None),
            ("1.4.1 ", None),
            ("V1.4.1", None),
            ("1.4.99999999999", None),
        ] {
            let got = Version::parse(text).map(|v| (v.major, v.minor, v.patch, v.stage));
            assert_eq!(got, expected, "{text:?}");
        }
    }

    #[test]
    fn feed_parses_and_ignores_what_it_cannot_use() {
        let json = br#"{
            "schema": 1,
            "future": true,
            "channels": {
                "stable": {
                    "version": "1.4.1",
                    "date": "2026-10-01",
                    "notes_url": "https://example.org/notes",
                    "downloads": {"windows": "https://example.org/w.zip"},
                    "sha256": "abc"
                },
                "beta": {"version": 7},
                "nightly": {"version": "x"}
            }
        }"#;
        let feed = parse_feed(json).expect("parse");
        let stable = feed.channels.stable.expect("stable");
        assert_eq!(stable.version, "1.4.1");
        assert_eq!(stable.downloads["windows"], "https://example.org/w.zip");
        assert!(feed.channels.beta.is_none());
    }

    #[test]
    fn feed_reads_what_the_publish_writes() {
        let bytes = include_bytes!("../../../../tests/update-feed/feed-after.json");
        let channels = parse_feed(bytes).expect("parse").channels;
        assert_eq!(channels.stable.expect("stable").version, "1.4.1");
        assert_eq!(channels.beta.expect("beta").version, "1.4.1");
    }

    #[test]
    fn feed_rejects_what_it_cannot_trust() {
        let big = vec![b' '; MAX_FEED_BYTES + 1];
        assert!(matches!(parse_feed(&big), Err(FeedError::TooLarge)));
        assert!(matches!(
            parse_feed(br#"{"schema":2,"channels":{}}"#),
            Err(FeedError::UnknownSchema(2))
        ));
        for bad in [&b"not json"[..], br#"{"channels":{}}"#, br#"{"schema":1}"#] {
            assert!(matches!(parse_feed(bad), Err(FeedError::Malformed(_))));
        }
    }

    fn feed(stable: Option<&str>, beta: Option<&str>) -> Feed {
        let entry = |v: &str| Entry {
            version: v.into(),
            date: String::new(),
            notes_url: String::new(),
            downloads: BTreeMap::new(),
        };
        Feed {
            schema: FEED_SCHEMA,
            channels: Channels {
                stable: stable.map(entry),
                beta: beta.map(entry),
            },
        }
    }

    #[test]
    fn decide_picks_the_notice() {
        // (running, skip, stable, beta, expected notice)
        let cases = [
            ("1.4.0", "", Some("1.4.1"), None, Some("1.4.1")),
            ("1.4.1", "", Some("1.4.1"), None, None),
            ("1.4.2", "", Some("1.4.1"), None, None),
            ("v1.4.0", "", Some("v1.4.1"), None, Some("v1.4.1")),
            // A release never hears about betas.
            ("1.4.0", "", Some("1.4.0"), Some("1.4.1b1"), None),
            ("1.4.0", "", Some("1.4.1"), Some("1.4.2b1"), Some("1.4.1")),
            // A pre-release hears about whichever is newest.
            (
                "1.4.1b1",
                "",
                Some("1.4.0"),
                Some("1.4.1b2"),
                Some("1.4.1b2"),
            ),
            ("1.4.1b2", "", Some("1.4.1"), Some("1.4.1b2"), Some("1.4.1")),
            // The same version in both channels is announced as the release.
            ("1.4.1b2", "", Some("1.4.1"), Some("v1.4.1"), Some("1.4.1")),
            (
                "1.4.1-dev",
                "",
                Some("1.4.0"),
                Some("1.4.1b1"),
                Some("1.4.1b1"),
            ),
            // Skipping holds only for the version skipped.
            ("1.4.0", "1.4.1", Some("1.4.1"), None, None),
            ("1.4.0", "v1.4.1", Some("1.4.1"), None, None),
            ("1.4.0", "1.4.1", Some("1.4.2"), None, Some("1.4.2")),
            // Skipping a beta leaves a release still newer than this build.
            (
                "1.4.1b2",
                "1.4.2b1",
                Some("1.4.1"),
                Some("1.4.2b1"),
                Some("1.4.1"),
            ),
            // Anything unreadable stays quiet, or falls back to the other channel.
            ("snapshot-a1b2c3d", "", Some("1.4.1"), None, None),
            ("1.4.0", "", Some("garbage"), None, None),
            ("1.4.1b1", "", Some("1.4.1"), Some("garbage"), Some("1.4.1")),
            ("1.4.0", "", None, None, None),
        ];
        let day = Duration::from_secs(86_400);
        let now = SystemTime::UNIX_EPOCH + day * 100;
        for (running, skip, stable, beta, expected) in cases {
            let f = feed(stable, beta);
            let action = decide(running, skip, Some(&f), Some(now), now, day);
            let Action::Wait { notice, .. } = action else {
                panic!("{running}: expected Wait, got {action:?}");
            };
            assert_eq!(
                notice.map(|e| e.version.as_str()),
                expected,
                "running {running}, skip {skip:?}, stable {stable:?}, beta {beta:?}"
            );
        }
    }

    #[test]
    fn decide_fetches_once_the_interval_has_passed() {
        let f = feed(Some("1.4.1"), None);
        let notice = f.channels.stable.as_ref();
        let day = Duration::from_secs(86_400);
        let hour = Duration::from_secs(3_600);
        let now = SystemTime::UNIX_EPOCH + day * 100;
        for (last_checked, expected) in [
            (None, None),
            (Some(now), Some(day)),
            (Some(now - hour), Some(day - hour)),
            (Some(now - day), None),
            (Some(now - day * 30), None),
            // The clock went backwards.
            (Some(now + hour), None),
        ] {
            let action = decide("1.4.0", "", Some(&f), last_checked, now, day);
            let expected = match expected {
                None => Action::Fetch { notice },
                Some(after) => Action::Wait { after, notice },
            };
            assert_eq!(action, expected, "last checked {last_checked:?}");
        }
    }
}
