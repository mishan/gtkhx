//! Turning an old `gtkhxrc` into a `gtkhx.toml`.
//!
//! The whole mapping is one table, written out rather than inferred per key,
//! because the renames are not all mechanical — the schema expands
//! abbreviations the old keys compressed, and `THEME` / `THEMENAME` becoming
//! `appearance.color_scheme` / `appearance.theme` is the pair most likely to
//! be transposed by someone working quickly.
//!
//! The table is exhaustive over `cfgvars[]`, and a test asserts it in both
//! directions: every old key resolves to a path the schema really has or to an
//! explicit [`Drop`](Target::Drop), and every path the schema has is reachable
//! from some old key or is listed as new. A key added to one side and
//! forgotten on the other fails the build rather than the user.
//!
//! Migration produces a **document**, not a `Settings`. Everything downstream
//! — the type conversion, the range checks, the diagnostics — is then the
//! ordinary load path doing its ordinary job, rather than a second
//! implementation of it that could disagree.

use crate::fields::Kind;
use crate::legacy::{self, LegacyKeys};
use crate::warning::{Warning, WarningKind};
use toml_edit::{Array, DocumentMut, Item, Table, Value};

/// Where an old key ends up.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Target {
    /// A dotted path in the new schema.
    Path(&'static str),
    /// Deliberately not carried across, with the reason.
    Drop(&'static str),
}

use Target::{Drop, Path};

/// Every key `cfgvars[]` has ever persisted, and where it goes.
///
/// Order is the old table's order, which is alphabetical by key string —
/// the old table was `bsearch`ed, so it had to be sorted, and keeping that
/// order here makes the two easy to diff by eye.
pub const MAP: &[(&str, Target)] = &[
    ("ANIMATEAVATARS", Path("users.animate_avatars")),
    ("AUTOCOPYCOLOR", Path("chat.autocopy.color")),
    ("AUTOCOPYSTAMP", Path("chat.autocopy.timestamp")),
    ("AUTOCOPYTEXT", Path("chat.autocopy.text")),
    ("CHATAVATARS", Path("chat.avatars")),
    ("CHATHISTORYINITIAL", Path("chat.history_initial")),
    ("CHATXSIZE", Drop(PANEL_SIZE)),
    ("CHATYSIZE", Drop(PANEL_SIZE)),
    ("DOWNLOAD", Path("transfers.download_dir")),
    ("EMOJISHORTCODES", Path("chat.emoji.shortcodes")),
    ("EMOJITYPEAHEAD", Path("chat.emoji.typeahead")),
    ("FONT", Path("chat.font")),
    ("HIGHLIGHTWORDS", Path("chat.highlight_words")),
    ("ICON", Path("identity.icon")),
    ("MARKDOWN", Path("chat.markdown")),
    ("NEWSXSIZE", Drop(PANEL_SIZE)),
    ("NEWSYSIZE", Drop(PANEL_SIZE)),
    ("NICK", Path("identity.nick")),
    ("NICKCOLOR", Path("identity.nick_color")),
    ("NOTIFYBROADCAST", Path("notify.broadcast")),
    ("NOTIFYCHAT", Path("notify.chat")),
    ("NOTIFYCHATHIGHLIGHT", Path("notify.chat_highlight")),
    ("NOTIFYMSG", Path("notify.private_message")),
    ("NOTIFYNEWS", Path("notify.news")),
    ("NOTIFYOMITFOCUSED", Path("notify.omit_focused")),
    ("NOTIFYPCHAT", Path("notify.private_chat")),
    (
        "NOTIFYPCHATHIGHLIGHT",
        Path("notify.private_chat_highlight"),
    ),
    ("NOTIFYPCHATINVITE", Path("notify.private_chat_invite")),
    ("NOTIFYXFER", Path("notify.transfer")),
    ("OLD_NICKCOMPLETION", Path("chat.legacy_nick_completion")),
    ("OPENCHAT", Drop(PANEL_LATCH)),
    ("OPENNEWS", Drop(PANEL_LATCH)),
    ("OPENTASKS", Drop(PANEL_LATCH)),
    ("OPENUSERS", Drop(PANEL_LATCH)),
    ("QUEUEDL", Path("transfers.queue")),
    ("SHOWJOIN", Path("chat.show_joins")),
    ("SOUNDCHAT", Path("sound.chat")),
    ("SOUNDERROR", Path("sound.error")),
    ("SOUNDFILE", Path("sound.transfer")),
    ("SOUNDINVITE", Path("sound.invite")),
    ("SOUNDJOIN", Path("sound.join")),
    ("SOUNDLOGIN", Path("sound.login")),
    ("SOUNDMSG", Path("sound.private_message")),
    ("SOUNDNEWS", Path("sound.news")),
    ("SOUNDPART", Path("sound.leave")),
    ("SOUNDSON", Path("sound.enabled")),
    ("SOUNDVOICEJOIN", Path("sound.voice_join")),
    ("SOUNDVOICELEAVE", Path("sound.voice_leave")),
    ("TASKXSIZE", Drop(PANEL_SIZE)),
    ("TASKYSIZE", Drop(PANEL_SIZE)),
    ("THEME", Path("appearance.color_scheme")),
    ("THEMENAME", Path("appearance.theme")),
    ("TIME", Drop(UPTIME)),
    ("TIMESTAMP", Path("chat.timestamp")),
    ("TIMESTAMPFORMAT", Path("chat.timestamp_format")),
    ("TOOLXSIZE", Path("window.toolbar_width")),
    ("TOOLYSIZE", Path("window.toolbar_height")),
    ("TRACKER", Path("trackers.addresses")),
    ("TRACKER_CASE", Path("trackers.case_sensitive")),
    ("TRAY", Path("appearance.tray")),
    ("USERXSIZE", Drop(PANEL_SIZE)),
    ("USERYSIZE", Drop(PANEL_SIZE)),
    ("VOICEINPUTDEVICE", Path("voice.input_device")),
    ("VOICEOUTPUTDEVICE", Path("voice.output_device")),
    ("VOICEPTTENABLED", Path("voice.ptt_enabled")),
    ("VOICEPTTKEY", Path("voice.ptt_key")),
    ("WORDWRAP", Path("chat.word_wrap")),
    ("XBUF_MAX", Path("chat.scrollback_lines")),
    // Keys from versions before the current table. They were already being
    // silently ignored on load, so nothing is lost by naming them here — but
    // naming them is what keeps the "every key in the file is accounted for"
    // diagnostic quiet about them rather than crying wolf on an old profile.
    ("FILE_SAMEWINDOW", Drop(RETIRED)),
    ("NEWS_SAMEWINDOW", Drop(RETIRED)),
];

const PANEL_SIZE: &str = "per-panel window sizes were written on every save and never read back; \
     the dock layout owns panel geometry now";
const PANEL_LATCH: &str =
    "a one-way latch set the first time its panel was built and never cleared, \
     with no setting exposing it — so it was 1 for everyone after first run";
const UPTIME: &str = "accumulated state rather than a preference; it went with the /stats command";
const RETIRED: &str = "retired before the current table, and already ignored on load";

/// Paths in the new schema that no old key feeds, and so are new settings
/// rather than renamed ones. Empty today — the schema deliberately shipped as
/// a rearrangement of `cfgvars[]` and nothing more.
///
/// It exists so the coverage test can be exhaustive in *both* directions: a
/// path added to the schema without a migration source has to be listed here
/// on purpose, rather than quietly defaulting for everyone who upgrades.
pub const NEW_PATHS: &[&str] = &[];

/// Where an old key goes, or `None` if the schema has never heard of it.
pub fn target_of(key: &str) -> Option<Target> {
    MAP.iter().find(|(k, _)| *k == key).map(|(_, t)| *t)
}

/// Build a `gtkhx.toml` document from the contents of a `gtkhxrc`.
///
/// Values are converted only as far as "a TOML value of the right type";
/// range checks, enum validation and the rest happen when the document is read
/// back through the ordinary load path, so there is exactly one place that
/// decides what a valid setting is.
pub fn to_document(
    legacy: &LegacyKeys,
    form: legacy::Form,
    warnings: &mut Vec<Warning>,
) -> DocumentMut {
    let mut doc = DocumentMut::new();
    doc["version"] = Item::Value(Value::from(crate::CURRENT_VERSION as i64));

    // Walk the *schema* rather than the old file, so a migrated profile's
    // first `gtkhx.toml` is laid out exactly like a fresh user's instead of in
    // whatever order the old keys happened to sort in.
    for (path, kind) in crate::fields::FIELDS {
        let Some((key, raw)) = source_for(path, legacy) else {
            continue;
        };

        // The escape asymmetry doubled backslashes once per save, and we can
        // only undo one doubling. Say so rather than hand over a path that is
        // quietly wrong; the download path on Windows is the realistic victim.
        //
        // Only for the GKeyFile form. The line form was never written through
        // `g_key_file_set_string`, so nothing escaped it and nothing unescapes
        // it — a doubled backslash there is just a doubled backslash, and
        // warning about it would explain a bug that cannot have happened to
        // that file.
        if form == legacy::Form::KeyFile
            && matches!(kind, Kind::Text | Kind::List)
            && legacy::looks_over_escaped(raw)
        {
            warnings.push(Warning {
                path: path.to_string(),
                kind: WarningKind::OverEscaped {
                    key: key.to_string(),
                },
            });
        }

        let Some(value) = convert(*kind, path, raw) else {
            warnings.push(Warning {
                path: path.to_string(),
                kind: WarningKind::UnmigratableValue {
                    key: key.to_string(),
                    value: raw.clone(),
                },
            });
            continue;
        };
        put(&mut doc, path, value);
    }

    // A second pass for the diagnostics, because the first walks the schema
    // and so never sees a key the schema has no home for.
    for key in legacy.keys() {
        if target_of(key).is_none() {
            warnings.push(Warning {
                path: key.clone(),
                kind: WarningKind::UnknownLegacyKey,
            });
        }
    }

    doc
}

/// The old key feeding `path`, and its value, if the file has one.
///
/// A `Drop` target never reaches here — it has no path — so this quietly does
/// the right thing for the deliberately-discarded keys without a special case.
fn source_for<'a>(path: &str, legacy: &'a LegacyKeys) -> Option<(&'static str, &'a String)> {
    MAP.iter().find_map(|(key, target)| match target {
        Path(p) if *p == path => legacy.get_key_value(*key).map(|(_, v)| (*key, v)),
        _ => None,
    })
}

/// One old string to one TOML value. `None` when the old value cannot be made
/// sense of, in which case the caller leaves the setting at its default —
/// which is what the C reader did too, for booleans at least. It turned an
/// unparseable *number* into zero instead, silently, which is the behaviour
/// this deliberately does not reproduce.
fn convert(kind: Kind, path: &str, raw: &str) -> Option<Value> {
    // An empty theme name meant "use the built-in default" — the C field is
    // NULL until the user picks one, and the writer emits NULL as "". The new
    // schema spells that default as the string "default", so carrying the
    // empty value across would leave a theme name nothing resolves. Skipping
    // the key lets the schema default win.
    //
    // Deliberately specific to this one path rather than a blanket rule:
    // empty is *meaningful* for the two voice devices ("system default") and
    // for the push-to-talk key ("enabled but not yet bound").
    if path == "appearance.theme" && raw.is_empty() {
        return Some(Value::from(crate::Appearance::default().theme));
    }

    Some(match kind {
        Kind::Flag => Value::from(legacy::parse_boolean(raw)?),
        // A window size of zero was the "never saved" sentinel and the C
        // struct's static default, so an old profile that predates the
        // toolbar ever being resized carries `TOOLXSIZE=0`. That is not the
        // user getting something wrong, so let the schema default win rather
        // than reporting it as out of range.
        Kind::Extent if raw.trim() == "0" => {
            let default = crate::Window::default();
            let fallback = match path {
                "window.toolbar_height" => default.toolbar_height,
                _ => default.toolbar_width,
            };
            return Some(Value::from(fallback as i64));
        }
        Kind::Signed | Kind::Unsigned | Kind::Extent | Kind::Id16 => {
            Value::from(raw.trim().parse::<i64>().ok()?)
        }
        // The nickname is the connection's 32-byte wire name field, and the C
        // reader clamped it on the way in. A file the C writer produced cannot
        // exceed that, but a hand-edited or bare-lines one can, and an
        // over-long nick would otherwise surface as a wire problem at connect
        // time rather than here.
        Kind::Text if path == "identity.nick" => Value::from(clamp_wire_name(raw)),
        Kind::Text | Kind::Scheme => Value::from(raw),
        Kind::List => {
            let mut array = Array::new();
            for item in legacy::split_list(raw) {
                array.push(item.as_str());
            }
            Value::Array(array)
        }
    })
}

/// Truncate to what the Hotline wire name field holds: 32 bytes including the
/// terminator, so 31 usable. Cut on a character boundary rather than a byte
/// one, or a multi-byte character at the limit becomes invalid UTF-8 and the
/// TOML is unwritable.
fn clamp_wire_name(s: &str) -> &str {
    const LIMIT: usize = 31;
    if s.len() <= LIMIT {
        return s;
    }
    let mut end = LIMIT;
    while end > 0 && !s.is_char_boundary(end) {
        end -= 1;
    }
    &s[..end]
}

/// Set a dotted path, creating the intermediate tables. A cut-down twin of
/// `fields::Writer::put` — no decor to preserve and no existing value to
/// compare against, because the document is being built from nothing.
fn put(doc: &mut DocumentMut, path: &str, value: Value) {
    let mut segments = path.split('.').peekable();
    let mut item: &mut Item = doc.as_item_mut();
    while let Some(segment) = segments.next() {
        if segments.peek().is_none() {
            item[segment] = Item::Value(value);
            return;
        }
        let slot = &mut item[segment];
        if slot.is_none() {
            let mut table = Table::new();
            table.set_implicit(true);
            *slot = Item::Table(table);
        }
        item = slot;
    }
}
