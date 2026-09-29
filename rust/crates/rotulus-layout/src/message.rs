//! The structured message model.
//!
//! This is the thing xtext never had. There, a chat line was a byte
//! string with in-band escapes; a speaker, a message id, an avatar or a hit
//! region had nowhere to live, which is why late features had to be
//! smuggled in as a magic word (`hxmedia:N`) or a magic
//! non-breaking-space sentinel. Here a message is a value with fields.
//!
//! See docs/chat-view.md "The message model".

use crate::span::ParsedText;

/// Stable identity for one row, unique within a [`crate::ChatBuffer`].
///
/// Allocated by the buffer, never reused within a session. Marks handed
/// out to callers are `MessageId`s, so a row that gets trimmed or cleared
/// leaves the caller holding an id that simply no longer resolves — the
/// weak-reference semantics `rotulus.h` already documents, but without
/// the dangling-pointer hazard xtext's raw `textentry *` cursors had.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug, Hash)]
pub struct MessageId(pub u64);

/// Which direction a [`MessageKind::LoadMore`] row pages in.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LoadMoreDirection {
    Older,
    Newer,
}

/// What a row *is*.
///
/// Note `LoadMore` and `Divider`: under xtext both were ordinary text
/// rows whose meaning was recovered by string-matching the rendered
/// bytes — `chat_history_word_click` compared the clicked word against a
/// composed "↑\u{a0}Load\u{a0}older\u{a0}messages" sentinel, non-breaking
/// spaces and all, because xtext's tokenizer splits on ASCII space.
/// Making them row kinds retires that.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum MessageKind {
    /// A live message from the server.
    Live,
    /// A backfilled message, carrying the server's message id so the
    /// paging cursor can be derived from the buffer rather than tracked
    /// alongside it.
    History { server_message_id: u64 },
    /// A rule with a caption ("chat history (12 messages)").
    Divider,
    /// The clickable paging row.
    LoadMore(LoadMoreDirection),
    /// Client-generated notice — connection state, task errors, /me
    /// output, the old `INFOPREFIX` lines.
    System,
}

impl MessageKind {
    /// Part of a chat-history block: backfilled messages and the rows that
    /// frame them. These don't count against the scrollback cap — the user
    /// asked for them — and are never trimmed to make room for live rows.
    pub fn is_history(&self) -> bool {
        matches!(
            self,
            MessageKind::History { .. } | MessageKind::Divider | MessageKind::LoadMore(_)
        )
    }
}

/// Who said it.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Speaker {
    /// The application's identity for this person, or 0 when it has
    /// none.
    ///
    /// Opaque to the view: it is compared for grouping, handed back in
    /// `speaker-menu`, and passed to the avatar resolver, and nothing
    /// else. A Hotline client uses its 16-bit user id; an IRC client
    /// might hash the account name. 0 is "unknown" rather than "user
    /// zero", and a row with an unknown speaker gets no avatar slot,
    /// since there would be nothing to resolve it against.
    pub key: u64,
    pub nick: String,
    /// A per-person color, `0x00RRGGBB`. `None` means "use the view's
    /// default nick color".
    pub color: Option<u32>,
}

impl Message {
    /// The key two messages must share to be grouped, or `None` for a
    /// message that never groups (system rows, dividers, `/me`).
    ///
    /// Both halves matter, and each catches a case the other misses:
    ///
    /// - The **key** separates two people who happen to share a nick.
    /// - The **rendered nick** separates one person before and after a
    ///   rename. The key survives a rename, so keying on it alone would
    ///   group the messages and the new name would simply never appear —
    ///   which is worse than repeating it, since the change is exactly
    ///   what the reader needs to see.
    ///
    /// The nick compared is the *gutter text as drawn*, not
    /// `Speaker.nick`, because what a reader notices is the label on
    /// screen changing.
    pub fn group_key(&self) -> Option<GroupKey<'_>> {
        if self.flags.contains(MessageFlags::ACTION) || self.flags.contains(MessageFlags::DELETED) {
            return None;
        }
        // Client-generated notices never group. They share a gutter
        // ("[hx]") without sharing a speaker, so keying on the drawn
        // nick would collapse a run of unrelated status lines —
        // "connecting", "connected", "login ok" — into one block under
        // a single tag, which reads as one event rather than three.
        //
        // Checked on the kind rather than on `speaker.is_none()`: some
        // sources carry no identity at all (a pre-1.5 Hotline server
        // sends chat with no uid), and those rows are real messages from
        // a real person that should still group by nick.
        if self.kind == MessageKind::System {
            return None;
        }
        let key = self.speaker.as_ref().map(|s| s.key).unwrap_or(0);
        // A row with no gutter at all is a system line and never groups.
        let nick = match &self.gutter {
            Some(g) if !g.text.is_empty() => g.text.as_str(),
            _ => return None,
        };
        Some(GroupKey { key, nick })
    }
}

/// What makes two adjacent messages "the same speaker, still".
///
/// Equality is on both fields: same person *and* same displayed name.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GroupKey<'a> {
    /// 0 when the speaker's identity is unknown — then the nick carries
    /// the whole decision, which is the best available answer.
    pub key: u64,
    /// The gutter text as rendered.
    pub nick: &'a str,
}

impl Speaker {
    pub fn new(key: u64, nick: impl Into<String>) -> Speaker {
        Speaker {
            key,
            nick: nick.into(),
            color: None,
        }
    }
}

/// Intrinsic pixel size of an image block, as reported by the decoder.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ImageSize {
    pub width: u32,
    pub height: u32,
}

/// One piece of a message's body.
///
/// Adding a kind of content is adding a variant here plus a measure arm
/// and a snapshot arm in the view — as opposed to xtext, where inline
/// media needed a discriminator on `textentry`, a side-allocated
/// `xtext_media_data`, a parallel render path, and a padding hack in the
/// line-count math.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Block {
    Text(ParsedText),
    /// A fenced markdown code block. Rendered monospace in a tinted
    /// panel, never wrapped mid-token, never parsed for other markup.
    Code {
        text: String,
        language: Option<String>,
    },
    /// A markdown `>` quote. `depth` counts nesting.
    Quote {
        content: ParsedText,
        depth: u8,
    },
    /// Server-validated inline media. `texture` is deliberately absent
    /// from this crate — the layout engine only needs the size, and a
    /// `GdkTexture` cannot cross into a GTK-free crate. The view keys its
    /// own texture table off `token`.
    Image {
        token: u32,
        /// `None` until the decode lands; the block measures as its
        /// `alt` text until then, exactly as the placeholder row does
        /// today.
        size: Option<ImageSize>,
        alt: String,
    },
}

impl Block {
    pub fn text(t: impl Into<String>) -> Block {
        Block::Text(ParsedText::plain(t))
    }
}

/// A message's body.
///
/// Nearly every message is one text block, which is held inline; a body
/// markdown split into paragraphs, code and quotes holds a vector. A plain
/// `Vec` for every row would cost a separate allocation (and its header)
/// for a single block, which is most of a short message's body. Reads go
/// through the slice either way.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Blocks {
    One(Block),
    Many(Vec<Block>),
}

impl Blocks {
    pub fn new(mut v: Vec<Block>) -> Blocks {
        if v.len() == 1 {
            Blocks::One(v.pop().expect("one block"))
        } else {
            Blocks::Many(v)
        }
    }
}

impl std::ops::Deref for Blocks {
    type Target = [Block];

    fn deref(&self) -> &[Block] {
        match self {
            Blocks::One(b) => std::slice::from_ref(b),
            Blocks::Many(v) => v,
        }
    }
}

impl std::ops::DerefMut for Blocks {
    fn deref_mut(&mut self) -> &mut [Block] {
        match self {
            Blocks::One(b) => std::slice::from_mut(b),
            Blocks::Many(v) => v,
        }
    }
}

impl From<Vec<Block>> for Blocks {
    fn from(v: Vec<Block>) -> Blocks {
        Blocks::new(v)
    }
}

impl From<Block> for Blocks {
    fn from(b: Block) -> Blocks {
        Blocks::One(b)
    }
}

impl<'a> IntoIterator for &'a Blocks {
    type Item = &'a Block;
    type IntoIter = std::slice::Iter<'a, Block>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<'a> IntoIterator for &'a mut Blocks {
    type Item = &'a mut Block;
    type IntoIter = std::slice::IterMut<'a, Block>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter_mut()
    }
}

/// Per-message rendering flags.
#[derive(Clone, Copy, PartialEq, Eq, Default, Hash)]
pub struct MessageFlags(pub u8);

impl MessageFlags {
    pub const NONE: MessageFlags = MessageFlags(0);
    /// Matched the highlight word list — the whole row draws emphasised.
    pub const HIGHLIGHT: MessageFlags = MessageFlags(1 << 0);
    /// Backfilled history: drawn in the muted secondary colour.
    pub const MUTED: MessageFlags = MessageFlags(1 << 1);
    /// A `/me` action: "* nick does something", no nick column.
    pub const ACTION: MessageFlags = MessageFlags(1 << 2);
    /// Originated here rather than arriving from the server.
    ///
    /// Direction, not sender identity — "is the sender me" cannot tell
    /// the echo of a message you just sent from the server's copy of it
    /// when you message yourself, and grouping needs to. See
    /// `rotulus.h`'s `RotulusSpeaker.outgoing`.
    pub const OUTGOING: MessageFlags = MessageFlags(1 << 3);
    /// Server tombstone for a deleted message.
    pub const DELETED: MessageFlags = MessageFlags(1 << 4);
    /// A continuation of the row above: same speaker, close in time, so
    /// the gutter is suppressed and only the body draws. Set by
    /// [`ChatBuffer`](crate::ChatBuffer), never by the caller — it is a
    /// property of a message's *neighbours*, not of the message, and
    /// gets recomputed when they change.
    pub const GROUPED: MessageFlags = MessageFlags(1 << 5);

    #[inline]
    pub fn contains(self, other: MessageFlags) -> bool {
        self.0 & other.0 == other.0
    }

    #[inline]
    pub fn union(self, other: MessageFlags) -> MessageFlags {
        MessageFlags(self.0 | other.0)
    }
}

impl std::fmt::Debug for MessageFlags {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.0 == 0 {
            return f.write_str("NONE");
        }
        let mut first = true;
        for (bit, name) in [
            (MessageFlags::HIGHLIGHT, "HIGHLIGHT"),
            (MessageFlags::MUTED, "MUTED"),
            (MessageFlags::ACTION, "ACTION"),
            (MessageFlags::OUTGOING, "OUTGOING"),
            (MessageFlags::DELETED, "DELETED"),
        ] {
            if self.contains(bit) {
                if !first {
                    f.write_str("|")?;
                }
                f.write_str(name)?;
                first = false;
            }
        }
        Ok(())
    }
}

/// A row.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Message {
    pub kind: MessageKind,
    /// Unix seconds. The C API turns a `stamp` of 0 into "now".
    pub timestamp: i64,
    pub speaker: Option<Speaker>,
    /// The nick column as the application styled it — brackets in one
    /// color, the name in another, a status tag — overriding the bare
    /// [`Self::speaker`] nick, which only sizes the column when this is
    /// absent.
    pub gutter: Option<ParsedText>,
    pub blocks: Blocks,
    pub flags: MessageFlags,
}

impl Message {
    /// A live message with a single text body.
    pub fn live(speaker: Speaker, body: ParsedText) -> Message {
        Message {
            kind: MessageKind::Live,
            timestamp: 0,
            speaker: Some(speaker),
            gutter: None,
            blocks: Blocks::One(Block::Text(body)),
            flags: MessageFlags::NONE,
        }
    }

    /// A client-generated notice with no speaker.
    pub fn system(body: ParsedText) -> Message {
        Message {
            kind: MessageKind::System,
            timestamp: 0,
            speaker: None,
            gutter: None,
            blocks: Blocks::One(Block::Text(body)),
            flags: MessageFlags::NONE,
        }
    }

    /// Give back the spare capacity building the message left behind. See
    /// [`ParsedText::compact`].
    pub fn compact(&mut self) {
        use crate::span::{exact_string, exact_vec};
        if let Some(s) = &mut self.speaker {
            exact_string(&mut s.nick);
        }
        if let Some(g) = &mut self.gutter {
            g.compact();
        }
        if let Blocks::Many(v) = &mut self.blocks {
            exact_vec(v);
        }
        for b in self.blocks.iter_mut() {
            match b {
                Block::Text(p) => p.compact(),
                Block::Quote { content, .. } => content.compact(),
                Block::Code { text, language } => {
                    exact_string(text);
                    if let Some(l) = language {
                        exact_string(l);
                    }
                }
                Block::Image { alt, .. } => exact_string(alt),
            }
        }
    }

    pub fn with_flags(mut self, f: MessageFlags) -> Message {
        self.flags = self.flags.union(f);
        self
    }

    pub fn with_timestamp(mut self, ts: i64) -> Message {
        self.timestamp = ts;
        self
    }

    /// The plain text of the whole message, blocks joined by newlines.
    /// Used for clipboard extraction and for the search index; image
    /// blocks contribute their alt text, which is the behaviour xtext
    /// approximated by storing the placeholder as `ent->str`.
    pub fn to_plain_text(&self) -> String {
        let mut out = String::new();
        for (i, b) in self.blocks.iter().enumerate() {
            if i > 0 {
                out.push('\n');
            }
            match b {
                Block::Text(p) => out.push_str(&p.text),
                Block::Code { text, .. } => out.push_str(text),
                Block::Quote { content, .. } => out.push_str(&content.text),
                Block::Image { alt, .. } => out.push_str(alt),
            }
        }
        out
    }
}
