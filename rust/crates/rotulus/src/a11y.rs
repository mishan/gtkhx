//! What an assistive technology reads.
//!
//! A screen reader sees the view through `GtkAccessibleText`: one string,
//! addressed by character offset, plus a caret and a selection. The
//! string here is the transcript as a person would read it aloud — each
//! row on its own line, as timestamp, nick and message — which is not the
//! same thing as what is painted: a grouped row still names its speaker,
//! because "who said this" is the first thing a listener needs and the
//! visual cue that stands in for it (the row above) is not available to
//! them.
//!
//! The model is built the first time something asks for it, which on a
//! desktop without a screen reader is never, and from then on kept in step
//! with appends so that announcing a new message is cheap. Anything else
//! that changes the buffer — an insert above, a removal, a trim — drops it
//! to be rebuilt on the next query.
//!
//! Plain Rust over the layout engine's buffer, so all of it is tested
//! without a display; the GTK glue is at the bottom, behind the `v4_14`
//! feature, since the interface arrived in GTK 4.14.

// Without the GTK glue the model has no caller outside its tests; it is
// still built, so those tests run in every configuration.
#![cfg_attr(not(feature = "v4_14"), allow(dead_code))]

use rotulus_layout::{Caret, ChatBuffer, LineSource, MessageId};

/// One row's place in the transcript.
#[derive(Debug, Clone)]
struct RowSpan {
    id: MessageId,
    /// Character offset of the row's first character.
    start: u32,
    /// Byte offset of the same, into `TextModel::text`.
    byte_start: usize,
    /// Where each part of the row starts, as (source, char offset, byte
    /// offset) relative to the row. Parts not present are absent.
    parts: Vec<(LineSource, u32, usize)>,
}

/// The transcript as one accessible string.
#[derive(Debug, Clone, Default)]
pub(crate) struct TextModel {
    text: String,
    rows: Vec<RowSpan>,
    chars: u32,
}

/// Formats a row's timestamp, or declines to.
pub(crate) type StampFn<'a> = &'a dyn Fn(i64) -> Option<String>;

impl TextModel {
    pub(crate) fn build(buf: &ChatBuffer, stamp: StampFn<'_>) -> TextModel {
        let mut m = TextModel::default();
        for row in 0..buf.len() {
            if let Some(id) = buf.id_at(row) {
                m.insert_row(buf, m.rows.len(), id, stamp);
            }
        }
        m
    }

    /// Append the row `id` names. Returns the character range inserted,
    /// including the newline that separates it from its neighbour.
    pub(crate) fn push(
        &mut self,
        buf: &ChatBuffer,
        id: MessageId,
        stamp: StampFn<'_>,
    ) -> (u32, u32) {
        self.insert_row(buf, self.rows.len(), id, stamp)
    }

    /// The row `id` names, as read aloud, and where its parts start.
    fn row_text(
        msg: &rotulus_layout::Message,
        stamp: StampFn<'_>,
    ) -> (String, Vec<(LineSource, u32, usize)>) {
        let mut row = String::new();
        let mut parts = Vec::new();
        let mut part = |row: &mut String, source: LineSource, text: &str| {
            parts.push((source, row.chars().count() as u32, row.len()));
            row.push_str(text);
        };
        if let Some(ts) = stamp(msg.timestamp) {
            row.push_str(ts.trim_end());
            row.push(' ');
        }
        if let Some(g) = msg.gutter.as_ref().filter(|g| !g.text.is_empty()) {
            part(&mut row, LineSource::Gutter, &g.text);
            row.push(' ');
        } else if let Some(sp) = msg.speaker.as_ref().filter(|s| !s.nick.is_empty()) {
            row.push_str(&sp.nick);
            row.push_str(": ");
        }
        for (bi, b) in msg.blocks.iter().enumerate() {
            if bi > 0 {
                row.push('\n');
            }
            let text = match b {
                rotulus_layout::Block::Text(p) => p.text.as_str(),
                rotulus_layout::Block::Code { text, .. } => text.as_str(),
                rotulus_layout::Block::Quote { content, .. } => content.text.as_str(),
                rotulus_layout::Block::Image { alt, .. } => alt.as_str(),
            };
            part(&mut row, LineSource::Block(bi), text);
        }
        (row, parts)
    }

    /// Move every row from `from` on by `chars` characters and `bytes`
    /// bytes (negative to pull them back).
    fn shift(&mut self, from: usize, chars: i64, bytes: i64) {
        for r in &mut self.rows[from..] {
            r.start = (i64::from(r.start) + chars) as u32;
            r.byte_start = (r.byte_start as i64 + bytes) as usize;
        }
    }

    /// Insert the row `id` names at model position `index` — the same
    /// position it has in the buffer. Returns the character range
    /// inserted, separator included.
    pub(crate) fn insert_row(
        &mut self,
        buf: &ChatBuffer,
        index: usize,
        id: MessageId,
        stamp: StampFn<'_>,
    ) -> (u32, u32) {
        let index = index.min(self.rows.len());
        let Some(msg) = buf.message(id) else {
            return (self.chars, self.chars);
        };
        let (row, parts) = Self::row_text(msg, stamp);
        let (at_char, at_byte, text, start, byte_start) = if self.rows.is_empty() {
            (0, 0, row, 0, 0)
        } else if index == self.rows.len() {
            // At the end: a separator, then the row.
            let (c, b) = (self.chars, self.text.len());
            (c, b, format!("\n{row}"), c + 1, b + 1)
        } else {
            // Before a row: the row, then a separator.
            let (c, b) = (self.rows[index].start, self.rows[index].byte_start);
            (c, b, format!("{row}\n"), c, b)
        };
        let added = text.chars().count() as u32;
        self.text.insert_str(at_byte, &text);
        self.chars += added;
        self.shift(index, i64::from(added), text.len() as i64);
        self.rows.insert(
            index,
            RowSpan {
                id,
                start,
                byte_start,
                parts,
            },
        );
        (at_char, at_char + added)
    }

    /// Remove the row at model position `index`. Returns the character
    /// range removed, separator included.
    pub(crate) fn remove_row(&mut self, index: usize) -> Option<(u32, u32)> {
        if index >= self.rows.len() {
            return None;
        }
        let r = self.rows.remove(index);
        let (from_char, from_byte, to_char, to_byte) = if let Some(next) = self.rows.get(index) {
            // A row with a successor takes its trailing separator.
            (r.start, r.byte_start, next.start, next.byte_start)
        } else if index > 0 {
            // The last of several takes the separator before it.
            (r.start - 1, r.byte_start - 1, self.chars, self.text.len())
        } else {
            (0, 0, self.chars, self.text.len())
        };
        self.text.replace_range(from_byte..to_byte, "");
        let (chars, bytes) = (to_char - from_char, to_byte - from_byte);
        self.chars -= chars;
        self.shift(index, -i64::from(chars), -(bytes as i64));
        Some((from_char, to_char))
    }

    /// Model position of the row `id` names.
    pub(crate) fn index_of(&self, id: MessageId) -> Option<usize> {
        self.rows.iter().position(|r| r.id == id)
    }

    /// Positions of rows the buffer no longer has, looking at no more than
    /// `limit` of them from the front — where a trim takes rows from.
    pub(crate) fn gone(&self, buf: &ChatBuffer, limit: usize) -> Vec<usize> {
        let mut out = Vec::new();
        for (i, r) in self.rows.iter().enumerate() {
            if out.len() == limit {
                break;
            }
            if buf.row_of(r.id).is_none() {
                out.push(i);
            }
        }
        out
    }

    pub(crate) fn char_len(&self) -> u32 {
        self.chars
    }

    #[cfg(test)]
    pub(crate) fn text(&self) -> &str {
        &self.text
    }

    /// Byte offset of a character offset, clamped to the end.
    fn byte_of(&self, offset: u32) -> usize {
        if offset >= self.chars {
            return self.text.len();
        }
        // Rows are sorted by start; find the one holding the offset and
        // walk characters from there rather than from the beginning.
        let i = self
            .rows
            .partition_point(|r| r.start <= offset)
            .saturating_sub(1);
        let (mut chars, bytes) = match self.rows.get(i) {
            Some(r) => (r.start, r.byte_start),
            None => (0, 0),
        };
        for (b, _) in self.text[bytes..].char_indices() {
            if chars == offset {
                return bytes + b;
            }
            chars += 1;
        }
        self.text.len()
    }

    /// The characters `start..end`, clamped.
    pub(crate) fn contents(&self, start: u32, end: u32) -> &str {
        let (a, b) = (self.byte_of(start.min(end)), self.byte_of(end.max(start)));
        &self.text[a..b]
    }

    /// The unit of `granularity` holding `offset`, as (start, end, text).
    pub(crate) fn contents_at(&self, offset: u32, granularity: Granularity) -> (u32, u32, &str) {
        let offset = offset.min(self.chars);
        let (start, end) = match granularity {
            Granularity::Character => (offset, (offset + 1).min(self.chars)),
            Granularity::Word => self.bounds(offset, |c| c.is_whitespace()),
            // Lines and sentences are rows' lines here: chat is not
            // written in paragraphs of sentences, and a message's own
            // punctuation is too unreliable to split on.
            Granularity::Line | Granularity::Sentence => self.bounds(offset, |c| c == '\n'),
            Granularity::Paragraph => {
                let i = self
                    .rows
                    .partition_point(|r| r.start <= offset)
                    .saturating_sub(1);
                match self.rows.get(i) {
                    Some(r) => {
                        let end = self.rows.get(i + 1).map_or(self.chars, |n| n.start - 1);
                        (r.start, end)
                    }
                    None => (0, 0),
                }
            }
        };
        (start, end, self.contents(start, end))
    }

    /// The run around `offset` bounded by characters matching `stop`.
    fn bounds(&self, offset: u32, stop: impl Fn(char) -> bool) -> (u32, u32) {
        let at = self.byte_of(offset);
        let before = &self.text[..at];
        let after = &self.text[at..];
        let start_b = before.rfind(&stop).map_or(0, |i| {
            i + before[i..].chars().next().map_or(1, char::len_utf8)
        });
        let end_b = at + after.find(&stop).unwrap_or(after.len());
        let start = offset - self.text[start_b..at].chars().count() as u32;
        let end = offset + self.text[at..end_b].chars().count() as u32;
        (start, end)
    }

    /// The character offset of a caret, if its row is in the model.
    pub(crate) fn offset_of(&self, caret: &Caret, buf: &ChatBuffer) -> Option<u32> {
        let row = self.rows.iter().find(|r| r.id == caret.message)?;
        let &(_, char_off, _) = row.parts.iter().find(|(s, _, _)| *s == caret.source)?;
        let text = buf.source_text(buf.row_of(caret.message)?, caret.source)?;
        let upto = text.get(..caret.offset.min(text.len()))?;
        Some(row.start + char_off + upto.chars().count() as u32)
    }
}

/// `GtkAccessibleTextGranularity`, without depending on GTK 4.14 to name
/// it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Granularity {
    Character,
    Word,
    Sentence,
    Line,
    Paragraph,
}

#[cfg(feature = "v4_14")]
mod glue {
    use super::*;
    use crate::view::RotulusView;
    use gtk4::prelude::*;
    use gtk4::subclass::prelude::*;

    impl From<gtk4::AccessibleTextGranularity> for Granularity {
        fn from(g: gtk4::AccessibleTextGranularity) -> Self {
            match g {
                gtk4::AccessibleTextGranularity::Character => Granularity::Character,
                gtk4::AccessibleTextGranularity::Word => Granularity::Word,
                gtk4::AccessibleTextGranularity::Sentence => Granularity::Sentence,
                gtk4::AccessibleTextGranularity::Paragraph => Granularity::Paragraph,
                _ => Granularity::Line,
            }
        }
    }

    impl AccessibleTextImpl for crate::view::imp::RotulusView {
        fn contents(&self, start: u32, end: u32) -> Option<glib::Bytes> {
            let obj = self.obj();
            obj.with_a11y(|m, _| Some(glib::Bytes::from(m.contents(start, end).as_bytes())))
        }

        fn contents_at(
            &self,
            offset: u32,
            granularity: gtk4::AccessibleTextGranularity,
        ) -> Option<(u32, u32, glib::Bytes)> {
            let obj = self.obj();
            obj.with_a11y(|m, _| {
                let (a, b, text) = m.contents_at(offset, granularity.into());
                Some((a, b, glib::Bytes::from(text.as_bytes())))
            })
        }

        fn caret_position(&self) -> u32 {
            let obj = self.obj();
            let sel = *self.selection.borrow();
            obj.with_a11y(|m, buf| sel.and_then(|s| m.offset_of(&s.focus, buf)))
                .unwrap_or(0)
        }

        fn selection(&self) -> Vec<gtk4::AccessibleTextRange> {
            let obj = self.obj();
            let Some(sel) = *self.selection.borrow() else {
                return Vec::new();
            };
            if sel.is_empty() {
                return Vec::new();
            }
            obj.with_a11y(|m, buf| {
                let a = m.offset_of(&sel.anchor, buf)?;
                let b = m.offset_of(&sel.focus, buf)?;
                let (lo, hi) = (a.min(b), a.max(b));
                Some(vec![gtk4::AccessibleTextRange::new(
                    lo as usize,
                    (hi - lo) as usize,
                )])
            })
            .unwrap_or_default()
        }

        fn attributes(
            &self,
            _offset: u32,
        ) -> Vec<(gtk4::AccessibleTextRange, glib::GString, glib::GString)> {
            Vec::new()
        }

        fn default_attributes(&self) -> Vec<(glib::GString, glib::GString)> {
            Vec::new()
        }
    }

    impl RotulusView {
        /// Run `f` over the accessible text, building it first if nothing
        /// has asked before.
        pub(crate) fn with_a11y<T>(
            &self,
            f: impl FnOnce(&TextModel, &ChatBuffer) -> Option<T>,
        ) -> Option<T> {
            let imp = self.imp_ref();
            let buf = imp.buffer.borrow();
            let mut slot = imp.a11y.borrow_mut();
            if slot.is_none() {
                let stamp = self.a11y_stamp_fn();
                *slot = Some(TextModel::build(&buf, &*stamp));
            }
            f(slot.as_ref()?, &buf)
        }

        /// Run `f` over the accessible text if something has asked for
        /// it, and report the ranges it changed. Nobody asking means
        /// nobody to tell.
        fn a11y_edit(
            &self,
            f: impl FnOnce(&mut TextModel, &ChatBuffer, StampFn<'_>) -> Vec<(bool, u32, u32)>,
        ) {
            let imp = self.imp_ref();
            let changes = {
                let mut slot = imp.a11y.borrow_mut();
                let Some(m) = slot.as_mut() else {
                    return;
                };
                let stamp = self.a11y_stamp_fn();
                f(m, &imp.buffer.borrow(), &*stamp)
            };
            for (inserted, a, b) in changes {
                let change = if inserted {
                    gtk4::AccessibleTextContentChange::Insert
                } else {
                    gtk4::AccessibleTextContentChange::Remove
                };
                self.update_contents(change, a, b);
            }
        }

        /// A row was appended, and `trimmed` of the oldest went to make
        /// room for it.
        pub(crate) fn a11y_appended(&self, id: MessageId, trimmed: usize) {
            self.a11y_edit(|m, buf, stamp| {
                let mut changes = Vec::new();
                // Highest first, so each position is still right when it
                // is removed.
                for i in m.gone(buf, trimmed).into_iter().rev() {
                    if let Some((a, b)) = m.remove_row(i) {
                        changes.push((false, a, b));
                    }
                }
                let (a, b) = m.push(buf, id, stamp);
                changes.push((true, a, b));
                changes
            });
        }

        /// A row went in above the end — a page of history.
        pub(crate) fn a11y_inserted(&self, id: MessageId) {
            self.a11y_edit(|m, buf, stamp| {
                let Some(at) = buf.row_of(id) else {
                    return Vec::new();
                };
                let (a, b) = m.insert_row(buf, at, id, stamp);
                vec![(true, a, b)]
            });
        }

        /// A row was removed from the buffer.
        pub(crate) fn a11y_removed(&self, id: MessageId) {
            self.a11y_edit(|m, _, _| {
                let Some(i) = m.index_of(id) else {
                    return Vec::new();
                };
                m.remove_row(i)
                    .map(|(a, b)| vec![(false, a, b)])
                    .unwrap_or_default()
            });
        }

        /// A row's content was replaced in place.
        pub(crate) fn a11y_replaced(&self, id: MessageId) {
            self.a11y_edit(|m, buf, stamp| {
                let Some(i) = m.index_of(id) else {
                    return Vec::new();
                };
                let mut changes = Vec::new();
                if let Some((a, b)) = m.remove_row(i) {
                    changes.push((false, a, b));
                }
                let (a, b) = m.insert_row(buf, i, id, stamp);
                changes.push((true, a, b));
                changes
            });
        }

        pub(crate) fn a11y_reset(&self) {
            let old = self.imp_ref().a11y.borrow_mut().take();
            let Some(old) = old else {
                return;
            };
            // Everything that was there went; everything there now arrived.
            // Coarse, and only for what changes every row at once — a
            // clear, a new timestamp format — since a listener told the
            // truth coarsely is better off than one told something finer
            // that is wrong.
            self.update_contents(gtk4::AccessibleTextContentChange::Remove, 0, old.char_len());
            let len = self.with_a11y(|m, _| Some(m.char_len())).unwrap_or(0);
            if len > 0 {
                self.update_contents(gtk4::AccessibleTextContentChange::Insert, 0, len);
            }
        }

        pub(crate) fn a11y_selection_changed(&self) {
            if self.imp_ref().a11y.borrow().is_some() {
                self.update_caret_position();
                self.update_selection_bound();
            }
        }
    }
}

#[cfg(not(feature = "v4_14"))]
impl crate::view::RotulusView {
    pub(crate) fn a11y_appended(&self, _id: MessageId, _trimmed: usize) {}
    pub(crate) fn a11y_inserted(&self, _id: MessageId) {}
    pub(crate) fn a11y_removed(&self, _id: MessageId) {}
    pub(crate) fn a11y_replaced(&self, _id: MessageId) {}
    pub(crate) fn a11y_reset(&self) {}
    pub(crate) fn a11y_selection_changed(&self) {}
}

#[cfg(test)]
mod tests {
    use super::*;
    use rotulus_layout::{
        Block, FixedMeasure, LayoutParams, Message, MessageFlags, MessageKind, ParsedText, Speaker,
    };

    fn said(nick: &str, body: &str) -> Message {
        Message {
            kind: MessageKind::Live,
            timestamp: 1_000,
            speaker: Some(Speaker::new(1, nick)),
            gutter: Some(ParsedText::plain(format!("<{nick}>"))),
            blocks: vec![Block::Text(ParsedText::plain(body))].into(),
            flags: MessageFlags::NONE,
        }
    }

    fn no_stamp(_: i64) -> Option<String> {
        None
    }

    fn buffer(rows: &[Message]) -> ChatBuffer {
        let m = FixedMeasure::new(8);
        let mut b = ChatBuffer::new(LayoutParams::default());
        for r in rows {
            b.append(r.clone(), &m);
        }
        b
    }

    #[test]
    fn rows_read_as_lines_of_nick_and_message() {
        let b = buffer(&[said("al", "hello"), said("bo", "hi there")]);
        let m = TextModel::build(&b, &no_stamp);
        assert_eq!(m.text(), "<al> hello\n<bo> hi there");
        assert_eq!(m.char_len(), m.text().chars().count() as u32);
    }

    #[test]
    fn a_grouped_row_still_names_its_speaker() {
        let m = FixedMeasure::new(8);
        let mut b = ChatBuffer::new(LayoutParams::default());
        b.set_group_gap_secs(300, &m);
        b.append(said("al", "one"), &m);
        b.append(said("al", "two"), &m);
        assert!(b
            .message_at(1)
            .unwrap()
            .flags
            .contains(MessageFlags::GROUPED));
        let t = TextModel::build(&b, &no_stamp);
        assert_eq!(t.text(), "<al> one\n<al> two");
    }

    #[test]
    fn timestamps_lead_each_row_when_shown() {
        let b = buffer(&[said("al", "hello")]);
        let stamp = |_: i64| Some("[12:00] ".to_string());
        let m = TextModel::build(&b, &stamp);
        assert_eq!(m.text(), "[12:00] <al> hello");
    }

    #[test]
    fn contents_are_addressed_in_characters() {
        let b = buffer(&[said("é", "ñandú")]);
        let m = TextModel::build(&b, &no_stamp);
        assert_eq!(m.text(), "<é> ñandú");
        assert_eq!(m.contents(4, 9), "ñandú");
        assert_eq!(
            m.contents(4, u32::MAX),
            "ñandú",
            "an end past the text clamps"
        );
        assert_eq!(
            m.contents(9, 4),
            "ñandú",
            "a reversed range is the same range"
        );
    }

    #[test]
    fn granularities_find_their_unit() {
        let b = buffer(&[said("al", "one two"), said("bo", "three")]);
        let m = TextModel::build(&b, &no_stamp);
        // "<al> one two\n<bo> three"
        assert_eq!(m.contents_at(6, Granularity::Word).2, "one");
        assert_eq!(m.contents_at(6, Granularity::Character).2, "n");
        assert_eq!(m.contents_at(6, Granularity::Line).2, "<al> one two");
        assert_eq!(m.contents_at(15, Granularity::Line).2, "<bo> three");
        assert_eq!(m.contents_at(15, Granularity::Paragraph).2, "<bo> three");
    }

    #[test]
    fn an_append_extends_the_text_and_reports_where() {
        let mut b = buffer(&[said("al", "one")]);
        let mut m = TextModel::build(&b, &no_stamp);
        let id = b.append(said("bo", "two"), &FixedMeasure::new(8));
        let (start, end) = m.push(&b, id, &no_stamp);
        assert_eq!(m.text(), "<al> one\n<bo> two");
        assert_eq!(
            (start, end),
            (8, m.char_len()),
            "the insert starts at the old end, newline included"
        );
        assert_eq!(
            TextModel::build(&b, &no_stamp).text(),
            m.text(),
            "incremental matches a rebuild"
        );
    }

    #[test]
    fn inserts_and_removes_match_a_rebuild_and_report_their_ranges() {
        let m8 = FixedMeasure::new(8);
        let mut b = buffer(&[said("al", "one"), said("bo", "two"), said("cy", "three")]);
        let mut m = TextModel::build(&b, &no_stamp);

        // Above the end, the way a page of history arrives.
        let id = b.insert_before(b.id_at(1), said("dee", "older"), &m8);
        let (a, z) = m.insert_row(&b, b.row_of(id).unwrap(), id, &no_stamp);
        assert_eq!(m.text(), TextModel::build(&b, &no_stamp).text());
        assert_eq!(
            &m.text()[a as usize..z as usize],
            "<dee> older\n",
            "the row and its separator"
        );

        // From the middle, the front and the end.
        for pick in [2usize, 0, 1] {
            let gone = b.id_at(pick).unwrap();
            let i = m.index_of(gone).unwrap();
            let before = m.text().to_string();
            b.remove(gone, &m8);
            let (a, z) = m.remove_row(i).unwrap();
            assert_eq!(m.text(), TextModel::build(&b, &no_stamp).text());
            assert_eq!(
                z - a,
                (before.chars().count() - m.text().chars().count()) as u32
            );
        }
        let last = b.id_at(0).unwrap();
        b.remove(last, &m8);
        m.remove_row(0).unwrap();
        assert_eq!(m.text(), "");
    }

    #[test]
    fn a_trim_at_the_cap_finds_the_rows_that_went() {
        let m8 = FixedMeasure::new(8);
        let mut b = buffer(&[said("al", "one"), said("bo", "two"), said("cy", "three")]);
        b.set_max_rows(3, &m8);
        let mut m = TextModel::build(&b, &no_stamp);
        let id = b.append(said("dee", "four"), &m8);
        assert_eq!(b.len(), 3, "the append trimmed one");
        let gone = m.gone(&b, 1);
        assert_eq!(gone, [0]);
        m.remove_row(0).unwrap();
        m.push(&b, id, &no_stamp);
        assert_eq!(m.text(), TextModel::build(&b, &no_stamp).text());
    }

    #[test]
    fn a_caret_maps_to_its_character_offset() {
        let b = buffer(&[said("al", "one"), said("bo", "ñandú")]);
        let m = TextModel::build(&b, &no_stamp);
        let id = b.id_at(1).unwrap();
        let caret = Caret {
            message: id,
            source: LineSource::Block(0),
            offset: "ña".len(),
        };
        // "<al> one\n<bo> ñandú": row 1 starts at 9, its body at 14.
        assert_eq!(m.offset_of(&caret, &b), Some(16));
        let gutter = Caret {
            message: id,
            source: LineSource::Gutter,
            offset: 0,
        };
        assert_eq!(m.offset_of(&gutter, &b), Some(9));
    }
}
