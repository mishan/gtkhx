//! The structured message model.
//!
//! This is the thing xtext never had. There, a chat line was a byte
//! string with in-band escapes; a uid, a message id, an avatar or a hit
//! region had nowhere to live, which is why every feature since Phase 9
//! had to be smuggled in as a magic word (`hxmedia:N`) or a magic
//! non-breaking-space sentinel. Here a message is a value with fields.
//!
//! See docs/chat-view-scoping.md §3.1.

use crate::span::ParsedText;

/// Stable identity for one row, unique within a [`crate::ChatBuffer`].
///
/// Allocated by the buffer, never reused within a session. Marks handed
/// out to callers are `MessageId`s, so a row that gets trimmed or cleared
/// leaves the caller holding an id that simply no longer resolves — the
/// weak-reference semantics `chat_view.h` already documents, but without
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

/// Which icon to draw in a speaker's gutter.
///
/// Resolved by the view against infrastructure that already exists and
/// is already per-uid and main-thread: `gtkhx_avatar_get` for the
/// fogWraith GIF avatar, `load_icon` for the classic 16-bit icon id.
/// The layout engine only needs the intrinsic size, which it asks the
/// [`crate::measure::TextMeasure`] implementation for.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum IconRef {
    #[default]
    None,
    /// Classic Hotline icon id (the `0x0068` user attribute).
    IconId(u16),
    /// Per-uid GIF avatar (Phase 10).
    Avatar(u16),
}

/// Who said it.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Speaker {
    pub uid: u16,
    pub nick: String,
    /// Hotline's real per-user colour: a `0x00RRGGBB` attribute on the
    /// user record. `None` means "use the view's default nick colour".
    pub color: Option<u32>,
    pub icon: IconRef,
}

impl Message {
    /// The key two messages must share to be grouped, or `None` for a
    /// message that never groups (system rows, dividers, `/me`).
    ///
    /// Both halves matter, and each catches a case the other misses:
    ///
    /// - The **uid** separates two people who happen to share a nick.
    /// - The **rendered nick** separates one person before and after a
    ///   rename. The uid survives a rename, so keying on it alone would
    ///   group the messages and the new name would simply never appear —
    ///   which is worse than repeating it, since the change is exactly
    ///   what the reader needs to see.
    ///
    /// The nick compared is the *gutter text as drawn*, not
    /// `Speaker.nick`, because what a reader notices is the label on
    /// screen changing.
    pub fn group_key(&self) -> Option<GroupKey<'_>> {
        if self.flags.contains(MessageFlags::ACTION)
            || self.flags.contains(MessageFlags::DELETED)
        {
            return None;
        }
        // Client-generated notices never group. They share a gutter
        // ("[hx]") without sharing a speaker, so keying on the drawn
        // nick would collapse a run of unrelated status lines —
        // "connecting", "connected", "login ok" — into one block under
        // a single tag, which reads as one event rather than three.
        //
        // Checked on the kind rather than on `speaker.is_none()`: a
        // pre-1.5 server sends chat with no uid, and those rows are real
        // messages from a real person that should still group by nick.
        if self.kind == MessageKind::System {
            return None;
        }
        let uid = self.speaker.as_ref().map(|s| s.uid).unwrap_or(0);
        // A row with no gutter at all is a system line and never groups.
        let nick = match &self.gutter {
            Some(g) if !g.text.is_empty() => g.text.as_str(),
            _ => return None,
        };
        Some(GroupKey { uid, nick })
    }
}

/// What makes two adjacent messages "the same speaker, still".
///
/// Equality is on both fields: same person *and* same displayed name.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GroupKey<'a> {
    /// 0 when the server sent no uid — then the nick carries the whole
    /// decision, which is the best available answer on those servers.
    pub uid: u16,
    /// The gutter text as rendered.
    pub nick: &'a str,
}

impl Speaker {
    pub fn new(uid: u16, nick: impl Into<String>) -> Speaker {
        Speaker {
            uid,
            nick: nick.into(),
            color: None,
            icon: IconRef::None,
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
    /// `chat_view.h`'s `HxChatSpeaker.outgoing`.
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
    /// Unix seconds. `0` means "stamp on append", matching the `stamp`
    /// argument the C append API already takes.
    pub timestamp: i64,
    pub speaker: Option<Speaker>,
    /// Pre-rendered left column, overriding whatever [`Self::speaker`]
    /// would produce.
    ///
    /// This is the compat path's escape hatch, and it earns its keep
    /// until C6. `chat.c` builds the nick column as a styled string —
    /// `\00312<\003alice\00312>\003`, brackets in one colour and the
    /// nick in another — and during the A/B the new view has to
    /// reproduce that *exactly*, which a bare `Speaker { nick }` can't.
    /// So the compat append path parses the left text into a
    /// `ParsedText` and puts it here.
    ///
    /// Once `chat.c` hands over structured messages (C6), the view
    /// renders the gutter from `speaker` — nick, colour, avatar — and
    /// this goes away with the mIRC shim.
    pub gutter: Option<ParsedText>,
    pub blocks: Vec<Block>,
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
            blocks: vec![Block::Text(body)],
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
            blocks: vec![Block::Text(body)],
            flags: MessageFlags::NONE,
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
