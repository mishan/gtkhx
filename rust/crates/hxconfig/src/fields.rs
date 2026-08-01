//! The field table: one list mapping every dotted TOML path to the struct
//! field behind it, expanded into a reader and a writer.
//!
//! Both directions come out of the same macro invocation on purpose. Two
//! hand-written parallel functions drift, and a drifted pair is exactly the
//! failure mode the old system had between its C key macros and the Rust copy
//! of them. Here, adding a field to one direction is not expressible.
//!
//! The table's order is also the order a freshly written file comes out in,
//! which is why it reads top-to-bottom like the schema rather than
//! alphabetically.

use crate::schema::{ColorScheme, Settings};
use crate::warning::{Warning, WarningKind};
use toml_edit::{Array, DocumentMut, Item, Table, Value};

macro_rules! field_table {
    ($($path:literal => $kind:ident, $($f:ident).+);+ $(;)?) => {
        /// Overlay whatever the document supplies onto `s`, leaving each
        /// unmentioned field at the default it arrived with.
        pub(crate) fn read_all(r: &mut Reader<'_>, s: &mut Settings) {
            $( r.$kind($path, &mut s.$($f).+); )+
        }

        /// Write every field into the document, in table order.
        pub(crate) fn write_all(w: &mut Writer<'_>, s: &Settings) {
            $( w.$kind($path, &s.$($f).+); )+
        }

        /// Every path the schema knows, in file order. Exposed so a test can
        /// assert the migration map covers all of them once that lands.
        pub const PATHS: &[&str] = &[ $($path),+ ];
    };
}

field_table! {
    "identity.nick"                 => text,     identity.nick;
    "identity.icon"                 => id16,     identity.icon;
    "identity.nick_color"           => signed,   identity.nick_color;

    "appearance.color_scheme"       => scheme,   appearance.color_scheme;
    "appearance.theme"              => text,     appearance.theme;
    "appearance.tray"               => flag,     appearance.tray;

    "chat.font"                     => text,     chat.font;
    "chat.word_wrap"                => flag,     chat.word_wrap;
    "chat.scrollback_lines"         => unsigned, chat.scrollback_lines;
    "chat.timestamp"                => flag,     chat.timestamp;
    "chat.timestamp_format"         => text,     chat.timestamp_format;
    "chat.avatars"                  => flag,     chat.avatars;
    "chat.markdown"                 => flag,     chat.markdown;
    "chat.show_joins"               => flag,     chat.show_joins;
    "chat.history_initial"          => unsigned, chat.history_initial;
    "chat.highlight_words"          => list,     chat.highlight_words;
    "chat.legacy_nick_completion"   => flag,     chat.legacy_nick_completion;

    "chat.autocopy.text"            => flag,     chat.autocopy.text;
    "chat.autocopy.timestamp"       => flag,     chat.autocopy.timestamp;
    "chat.autocopy.color"           => flag,     chat.autocopy.color;

    "chat.emoji.shortcodes"         => flag,     chat.emoji.shortcodes;
    "chat.emoji.typeahead"          => flag,     chat.emoji.typeahead;

    "users.animate_avatars"         => flag,     users.animate_avatars;

    "notify.chat"                   => flag,     notify.chat;
    "notify.chat_highlight"         => flag,     notify.chat_highlight;
    "notify.private_message"        => flag,     notify.private_message;
    "notify.private_chat"           => flag,     notify.private_chat;
    "notify.private_chat_highlight" => flag,     notify.private_chat_highlight;
    "notify.private_chat_invite"    => flag,     notify.private_chat_invite;
    "notify.news"                   => flag,     notify.news;
    "notify.transfer"               => flag,     notify.transfer;
    "notify.broadcast"              => flag,     notify.broadcast;
    "notify.omit_focused"           => flag,     notify.omit_focused;

    "sound.enabled"                 => flag,     sound.enabled;
    "sound.chat"                    => flag,     sound.chat;
    "sound.error"                   => flag,     sound.error;
    "sound.transfer"                => flag,     sound.transfer;
    "sound.invite"                  => flag,     sound.invite;
    "sound.join"                    => flag,     sound.join;
    "sound.leave"                   => flag,     sound.leave;
    "sound.login"                   => flag,     sound.login;
    "sound.private_message"         => flag,     sound.private_message;
    "sound.news"                    => flag,     sound.news;
    "sound.voice_join"              => flag,     sound.voice_join;
    "sound.voice_leave"             => flag,     sound.voice_leave;

    "transfers.download_dir"        => text,     transfers.download_dir;
    "transfers.queue"               => flag,     transfers.queue;

    "trackers.addresses"            => list,     trackers.addresses;
    "trackers.case_sensitive"       => flag,     trackers.case_sensitive;

    "voice.input_device"            => text,     voice.input_device;
    "voice.output_device"           => text,     voice.output_device;
    "voice.ptt_enabled"             => flag,     voice.ptt_enabled;
    "voice.ptt_key"                 => text,     voice.ptt_key;

    "window.toolbar_width"          => extent,   window.toolbar_width;
    "window.toolbar_height"         => extent,   window.toolbar_height;
}

/// The distinct table prefixes [`PATHS`] navigates through, longest last, each
/// appearing once. Derived rather than listed so it can't fall out of step.
fn table_prefixes() -> Vec<String> {
    let mut seen = std::collections::BTreeSet::new();
    let mut ordered = Vec::new();
    for path in PATHS {
        let mut prefix = String::new();
        let mut parts = path.split('.').peekable();
        while let Some(part) = parts.next() {
            if parts.peek().is_none() {
                break;
            }
            if !prefix.is_empty() {
                prefix.push('.');
            }
            prefix.push_str(part);
            if seen.insert(prefix.clone()) {
                ordered.push(prefix.clone());
            }
        }
    }
    ordered
}

// ---------------------------------------------------------------- reading --

/// Walk a dotted path to the item it names, if every segment above the leaf is
/// a table and the leaf exists.
fn lookup<'a>(doc: &'a DocumentMut, path: &str) -> Option<&'a Item> {
    let mut item: &Item = doc.as_item();
    for segment in path.split('.') {
        item = item.as_table_like()?.get(segment)?;
    }
    Some(item)
}

fn type_name(item: &Item) -> &'static str {
    match item {
        Item::None => "nothing",
        Item::Table(_) => "table",
        Item::ArrayOfTables(_) => "array of tables",
        Item::Value(v) => match v {
            Value::String(_) => "string",
            Value::Integer(_) => "integer",
            Value::Float(_) => "float",
            Value::Boolean(_) => "boolean",
            Value::Datetime(_) => "datetime",
            Value::Array(_) => "array",
            Value::InlineTable(_) => "table",
        },
    }
}

/// Reads typed values out of a parsed document, recording a diagnostic for
/// anything malformed rather than letting it become a silent zero — which is
/// what `atoi` did to every `INT`, `UINT16` and `TIME_T` key in the old
/// reader.
pub(crate) struct Reader<'a> {
    doc: &'a DocumentMut,
    warnings: &'a mut Vec<Warning>,
}

impl<'a> Reader<'a> {
    pub(crate) fn new(doc: &'a DocumentMut, warnings: &'a mut Vec<Warning>) -> Self {
        Reader { doc, warnings }
    }

    fn wrong_type(&mut self, path: &str, expected: &'static str, item: &Item) {
        self.warnings.push(Warning {
            path: path.to_string(),
            kind: WarningKind::WrongType {
                expected,
                found: type_name(item),
            },
        });
    }

    /// Report any prefix the schema expects to be a table that the file spells
    /// as something else — `chat = 5`, say.
    ///
    /// Without this, [`lookup`] would return `None` for every key under such a
    /// prefix, which is indistinguishable from the keys being absent: a whole
    /// page of settings would silently revert to defaults with nothing said.
    /// The check runs once per prefix rather than once per key beneath it, so
    /// one mistyped line produces one diagnostic.
    pub(crate) fn check_table_prefixes(&mut self) {
        for prefix in table_prefixes() {
            let Some(item) = lookup(self.doc, &prefix) else {
                continue;
            };
            if item.as_table_like().is_none() {
                self.wrong_type(&prefix, "table", item);
            }
        }
    }

    fn flag(&mut self, path: &str, out: &mut bool) {
        let Some(item) = lookup(self.doc, path) else {
            return;
        };
        match item.as_bool() {
            Some(v) => *out = v,
            None => self.wrong_type(path, "boolean", item),
        }
    }

    fn text(&mut self, path: &str, out: &mut String) {
        let Some(item) = lookup(self.doc, path) else {
            return;
        };
        match item.as_str() {
            Some(v) => *out = v.to_string(),
            None => self.wrong_type(path, "string", item),
        }
    }

    /// Shared integer path: fetch, type-check, range-check. Returns `None` and
    /// records the reason when the value can't be used, so the caller leaves
    /// the default in place.
    fn integer(&mut self, path: &str, min: i64, max: i64) -> Option<i64> {
        let item = lookup(self.doc, path)?;
        let Some(v) = item.as_integer() else {
            self.wrong_type(path, "integer", item);
            return None;
        };
        if v < min || v > max {
            self.warnings.push(Warning {
                path: path.to_string(),
                kind: WarningKind::OutOfRange { value: v, min, max },
            });
            return None;
        }
        Some(v)
    }

    fn signed(&mut self, path: &str, out: &mut i32) {
        if let Some(v) = self.integer(path, i32::MIN as i64, i32::MAX as i64) {
            *out = v as i32;
        }
    }

    fn unsigned(&mut self, path: &str, out: &mut u32) {
        if let Some(v) = self.integer(path, 0, u32::MAX as i64) {
            *out = v as u32;
        }
    }

    fn id16(&mut self, path: &str, out: &mut u16) {
        if let Some(v) = self.integer(path, 0, u16::MAX as i64) {
            *out = v as u16;
        }
    }

    /// A window dimension: like [`Reader::unsigned`] but floored at 1.
    ///
    /// Zero used to be the toolbar's "no size saved" sentinel, and the C
    /// struct's static default. Dropping the sentinel means a migrated zero
    /// would otherwise be honoured as a literal 0×0 window.
    fn extent(&mut self, path: &str, out: &mut u32) {
        if let Some(v) = self.integer(path, 1, u32::MAX as i64) {
            *out = v as u32;
        }
    }

    /// A non-string element is dropped with a diagnostic naming its index,
    /// rather than failing the whole list — one bad tracker address shouldn't
    /// cost you the other four.
    fn list(&mut self, path: &str, out: &mut Vec<String>) {
        let Some(item) = lookup(self.doc, path) else {
            return;
        };
        let Some(array) = item.as_array() else {
            self.wrong_type(path, "array of strings", item);
            return;
        };
        let mut collected = Vec::with_capacity(array.len());
        for (i, element) in array.iter().enumerate() {
            match element.as_str() {
                Some(s) => collected.push(s.to_string()),
                None => self.warnings.push(Warning {
                    path: format!("{path}[{i}]"),
                    kind: WarningKind::WrongType {
                        expected: "string",
                        found: type_name(&Item::Value(element.clone())),
                    },
                }),
            }
        }
        *out = collected;
    }

    fn scheme(&mut self, path: &str, out: &mut ColorScheme) {
        let Some(item) = lookup(self.doc, path) else {
            return;
        };
        let Some(s) = item.as_str() else {
            self.wrong_type(path, "string", item);
            return;
        };
        match ColorScheme::parse(s) {
            Some(v) => *out = v,
            None => self.warnings.push(Warning {
                path: path.to_string(),
                kind: WarningKind::UnknownValue {
                    value: s.to_string(),
                    allowed: ColorScheme::ALL,
                },
            }),
        }
    }
}

// ---------------------------------------------------------------- writing --

/// Do these two values mean the same thing, whatever they look like?
///
/// The comparison is semantic rather than textual on purpose. TOML has more
/// ways to spell a value than the writer emits — `'single quoted'`, `1_000`,
/// `0x0066ccff`, an array across several lines with a comment beside each
/// element — and a textual comparison would call every one of them a change,
/// then rewrite it in the writer's spelling on a save that never touched it.
/// Comparing meaning is what makes "a save changes exactly the lines it had
/// to" true for a hand-edited file and not just for one we wrote ourselves.
fn same_value(old: &Value, new: &Value) -> bool {
    match (old, new) {
        (Value::Boolean(a), Value::Boolean(b)) => a.value() == b.value(),
        (Value::Integer(a), Value::Integer(b)) => a.value() == b.value(),
        (Value::String(a), Value::String(b)) => a.value() == b.value(),
        (Value::Array(a), Value::Array(b)) => {
            a.len() == b.len() && a.iter().zip(b.iter()).all(|(x, y)| same_value(x, y))
        }
        _ => false,
    }
}

/// Writes typed values back *into the document that was loaded*, so unknown
/// keys, comments, key order and formatting all survive the round trip. The
/// old writer built a fresh `GKeyFile` from its table instead, which is why
/// hand-editing the file was a trap.
pub(crate) struct Writer<'a> {
    doc: &'a mut DocumentMut,
}

impl<'a> Writer<'a> {
    pub(crate) fn new(doc: &'a mut DocumentMut) -> Self {
        Writer { doc }
    }

    /// Set a dotted path, creating intermediate tables as needed.
    ///
    /// Two deliberate preservations: an untouched value is left byte-identical
    /// rather than reserialized, and a rewritten one inherits the old
    /// formatting — so a trailing `# comment` on a line survives a change to
    /// the value on that line.
    pub(crate) fn put(&mut self, path: &str, new: Value) {
        let mut segments = path.split('.').peekable();
        let mut item: &mut Item = self.doc.as_item_mut();
        while let Some(segment) = segments.next() {
            // toml_edit's `IndexMut for Item` panics on anything that is not a
            // table, an inline table or a hole — so a hand-edited file that
            // spells `chat = 5` would abort the client on save, having loaded
            // without complaint. Replace the offending item instead. The load
            // already warned about it (see `check_table_prefixes`), so this
            // discards only something the user has been told is unusable.
            if !item.is_none() && item.as_table_like().is_none() {
                let mut table = Table::new();
                table.set_implicit(true);
                *item = Item::Table(table);
            }
            if segments.peek().is_none() {
                let slot = &mut item[segment];
                if let Some(old) = slot.as_value() {
                    if same_value(old, &new) {
                        return;
                    }
                    let decor = old.decor().clone();
                    let mut new = new;
                    *new.decor_mut() = decor;
                    *slot = Item::Value(new);
                } else {
                    *slot = Item::Value(new);
                }
                return;
            }
            // Indexing alone would auto-create an *inline* table, which
            // round-trips fine and reads terribly — a whole page of settings on
            // one line. A file people are meant to hand-edit gets real `[chat]`
            // headers. An intermediate the file already spells as an inline
            // table is left as it found it.
            let slot = &mut item[segment];
            if slot.is_none() {
                let mut table = Table::new();
                table.set_implicit(true);
                *slot = Item::Table(table);
            }
            item = slot;
        }
    }

    fn flag(&mut self, path: &str, v: &bool) {
        self.put(path, Value::from(*v));
    }

    fn text(&mut self, path: &str, v: &str) {
        self.put(path, Value::from(v));
    }

    fn signed(&mut self, path: &str, v: &i32) {
        self.put(path, Value::from(*v as i64));
    }

    fn unsigned(&mut self, path: &str, v: &u32) {
        self.put(path, Value::from(*v as i64));
    }

    fn id16(&mut self, path: &str, v: &u16) {
        self.put(path, Value::from(*v as i64));
    }

    fn extent(&mut self, path: &str, v: &u32) {
        self.put(path, Value::from(*v as i64));
    }

    fn list(&mut self, path: &str, v: &[String]) {
        let mut array = Array::new();
        for element in v {
            array.push(element.as_str());
        }
        self.put(path, Value::Array(array));
    }

    fn scheme(&mut self, path: &str, v: &ColorScheme) {
        self.put(path, Value::from(v.as_str()));
    }
}
