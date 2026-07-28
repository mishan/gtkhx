//! Styled text runs.
//!
//! A [`Span`] is a byte range over some rendered text plus the style to
//! draw it with. Spans are the single currency between the parsers
//! (markdown, the mIRC compat shim) and the layout engine: whatever the
//! input vocabulary, the output is always a `ParsedText`.
//!
//! The important property is that styling is resolved **once, at
//! append**. xtext re-ran its mIRC state machine over every visible byte
//! on every render pass (`gtk_xtext_render_str`, xtext.c:3292) *and*
//! separately built an `ent->slp` run list at append that the render path
//! then ignored. Here there is one representation, produced once.

use std::ops::Range;

/// Text attribute bits. A plain `u8` rather than a `bitflags` dependency —
/// there are six of them and they never leave this crate untyped.
#[derive(Clone, Copy, PartialEq, Eq, Default, Hash)]
pub struct Attrs(pub u8);

impl Attrs {
    pub const NONE: Attrs = Attrs(0);
    pub const BOLD: Attrs = Attrs(1 << 0);
    pub const ITALIC: Attrs = Attrs(1 << 1);
    pub const UNDERLINE: Attrs = Attrs(1 << 2);
    pub const STRIKETHROUGH: Attrs = Attrs(1 << 3);
    /// Monospace + tinted background: a markdown `` `code` `` span.
    pub const CODE: Attrs = Attrs(1 << 4);
    /// Swap foreground and background at render time.
    pub const REVERSE: Attrs = Attrs(1 << 5);

    #[inline]
    pub fn contains(self, other: Attrs) -> bool {
        self.0 & other.0 == other.0
    }

    #[inline]
    pub fn union(self, other: Attrs) -> Attrs {
        Attrs(self.0 | other.0)
    }

    #[inline]
    pub fn remove(self, other: Attrs) -> Attrs {
        Attrs(self.0 & !other.0)
    }

    #[inline]
    pub fn is_empty(self) -> bool {
        self.0 == 0
    }
}

impl std::fmt::Debug for Attrs {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.is_empty() {
            return f.write_str("NONE");
        }
        let mut first = true;
        for (bit, name) in [
            (Attrs::BOLD, "BOLD"),
            (Attrs::ITALIC, "ITALIC"),
            (Attrs::UNDERLINE, "UNDERLINE"),
            (Attrs::STRIKETHROUGH, "STRIKETHROUGH"),
            (Attrs::CODE, "CODE"),
            (Attrs::REVERSE, "REVERSE"),
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

/// Where a colour comes from.
///
/// `Palette` indices 32..37 are the theme UI roles (see `chat_view.h`'s
/// `HX_CHAT_PAL_*`). Indices 0..31 are the legacy mIRC slots and exist
/// only so the compat shim (`crate::mirc`) can round-trip during the
/// coexistence period — they have no producer once xtext is deleted at
/// C5. `Rgb` is what Hotline actually gives us: the per-user `nick_color`
/// attribute on the user record, which is a real `0x00RRGGBB` value and
/// was never in-band markup.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub enum ColorRef {
    /// Inherit — the view's default foreground / background.
    #[default]
    Default,
    /// A palette slot.
    Palette(u8),
    /// A literal colour, `0x00RRGGBB`.
    Rgb(u32),
}

/// Index into [`ParsedText::links`].
pub type LinkId = u32;

/// How a run of text is drawn.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash)]
pub struct Style {
    pub attrs: Attrs,
    pub fg: ColorRef,
    pub bg: ColorRef,
    pub link: Option<LinkId>,
}

impl Style {
    pub fn with_attrs(mut self, a: Attrs) -> Style {
        self.attrs = self.attrs.union(a);
        self
    }

    pub fn with_fg(mut self, fg: ColorRef) -> Style {
        self.fg = fg;
        self
    }
}

/// A styled run: a byte range over the owning text, plus its style.
///
/// Ranges are over the **rendered** text, not the source. Markdown
/// removes its delimiters, so `text` and the input string differ; every
/// offset in a `Span` indexes `ParsedText::text`.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Span {
    pub range: Range<usize>,
    pub style: Style,
}

/// A resolved hyperlink.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Link {
    /// The destination, already scheme-checked (see
    /// `crate::markdown::scheme_allowed`).
    pub href: String,
    /// The range of visible text that activates it. Kept alongside the
    /// span's `link` id so a click can report both what was shown and
    /// where it actually goes — the label and the href are allowed to
    /// disagree, and the user is entitled to know when they do.
    pub range: Range<usize>,
}

/// The output of every parser in this crate.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct ParsedText {
    /// The text to draw, with all markup removed.
    pub text: String,
    /// Styled runs, sorted by start offset and non-overlapping. Runs with
    /// a wholly default style are omitted rather than materialised, so a
    /// plain message parses to zero spans.
    pub spans: Vec<Span>,
    /// Link targets, indexed by [`LinkId`].
    pub links: Vec<Link>,
}

impl ParsedText {
    /// A plain, unstyled string.
    pub fn plain(text: impl Into<String>) -> ParsedText {
        ParsedText {
            text: text.into(),
            spans: Vec::new(),
            links: Vec::new(),
        }
    }

    pub fn is_empty(&self) -> bool {
        self.text.is_empty()
    }

    pub fn len(&self) -> usize {
        self.text.len()
    }

    /// The style in effect at byte offset `at`.
    ///
    /// Linear over spans; callers walking the whole text in order should
    /// iterate `spans` directly instead. Present for hit-test and test
    /// assertions, which touch one offset at a time.
    pub fn style_at(&self, at: usize) -> Style {
        for s in &self.spans {
            if s.range.contains(&at) {
                return s.style;
            }
            if s.range.start > at {
                break;
            }
        }
        Style::default()
    }

    /// Mark `range` as a hyperlink, returning its id.
    ///
    /// Used for *autodetected* URLs, which are found after parsing —
    /// markdown links come out of the parser already resolved. The
    /// difference matters: an autodetected URL can land anywhere,
    /// including straddling existing style runs, so this splits spans at
    /// the boundaries rather than assuming a clean fit.
    ///
    /// Returns `None` for an empty or out-of-bounds range, or one that
    /// isn't on char boundaries — a detector working in bytes shouldn't
    /// be able to corrupt the span list.
    pub fn add_link(&mut self, range: Range<usize>, href: impl Into<String>) -> Option<LinkId> {
        if range.start >= range.end
            || range.end > self.text.len()
            || !self.text.is_char_boundary(range.start)
            || !self.text.is_char_boundary(range.end)
        {
            return None;
        }
        let id = self.links.len() as LinkId;
        self.links.push(Link {
            href: href.into(),
            range: range.clone(),
        });

        let mut out: Vec<Span> = Vec::with_capacity(self.spans.len() + 2);
        let mut cursor = range.start;

        // Everything before the link, plus the part of any straddling
        // span that lies before it.
        for s in &self.spans {
            if s.range.end <= range.start || s.range.start >= range.end {
                out.push(s.clone());
                continue;
            }
            if s.range.start < range.start {
                out.push(Span {
                    range: s.range.start..range.start,
                    style: s.style,
                });
            }
            // The overlapping middle keeps its own styling and gains the
            // link — so a URL inside a bold run stays bold.
            let ms = s.range.start.max(range.start);
            let me = s.range.end.min(range.end);
            if ms > cursor {
                out.push(Span {
                    range: cursor..ms,
                    style: link_style(Style::default(), id),
                });
            }
            if ms < me {
                out.push(Span {
                    range: ms..me,
                    style: link_style(s.style, id),
                });
                cursor = me;
            }
            if s.range.end > range.end {
                out.push(Span {
                    range: range.end..s.range.end,
                    style: s.style,
                });
            }
        }
        if cursor < range.end {
            out.push(Span {
                range: cursor..range.end,
                style: link_style(Style::default(), id),
            });
        }
        out.sort_by_key(|s| s.range.start);
        self.spans = out;
        self.debug_assert_well_formed();
        Some(id)
    }

    /// The link covering byte `at`, if any.
    pub fn link_at(&self, at: usize) -> Option<&Link> {
        let id = self.style_at(at).link?;
        self.links.get(id as usize)
    }

    /// Panics (in debug) unless the spans are sorted, non-empty,
    /// non-overlapping and inside the text. Called by the parsers'
    /// tests; cheap enough to also call in debug builds of callers.
    pub fn debug_assert_well_formed(&self) {
        if !cfg!(debug_assertions) {
            return;
        }
        let mut prev_end = 0usize;
        for s in &self.spans {
            assert!(s.range.start < s.range.end, "empty span {:?}", s.range);
            assert!(
                s.range.start >= prev_end,
                "overlapping or unsorted spans: {:?} after end {}",
                s.range,
                prev_end
            );
            assert!(
                s.range.end <= self.text.len(),
                "span {:?} past text len {}",
                s.range,
                self.text.len()
            );
            assert!(
                self.text.is_char_boundary(s.range.start)
                    && self.text.is_char_boundary(s.range.end),
                "span {:?} not on char boundaries",
                s.range
            );
            prev_end = s.range.end;
        }
    }
}

/// Accumulates text + styled runs, coalescing adjacent equal styles.
///
/// The parsers all build their output through this so that
/// `**a**` + `**b**` written adjacently produces one bold span rather
/// than two, which keeps the shaped-run count down in the layout engine.
#[derive(Default)]
pub(crate) struct SpanBuilder {
    text: String,
    spans: Vec<Span>,
    links: Vec<Link>,
}

impl SpanBuilder {
    pub(crate) fn new() -> SpanBuilder {
        SpanBuilder::default()
    }

    pub(crate) fn len(&self) -> usize {
        self.text.len()
    }

    /// Append `s` styled with `style`, merging into the previous run when
    /// the style matches and the runs abut.
    pub(crate) fn push(&mut self, s: &str, style: Style) {
        if s.is_empty() {
            return;
        }
        let start = self.text.len();
        self.text.push_str(s);
        let end = self.text.len();

        if style == Style::default() {
            return;
        }
        if let Some(last) = self.spans.last_mut() {
            if last.range.end == start && last.style == style {
                last.range.end = end;
                return;
            }
        }
        self.spans.push(Span {
            range: start..end,
            style,
        });
    }

    /// Reserve a link id before its label has been emitted.
    ///
    /// The id has to exist first so it can be carried in the label's
    /// [`Style`]; the visible range is only known once the label is
    /// pushed, so it starts empty and is filled in by
    /// [`SpanBuilder::set_link_range`].
    pub(crate) fn reserve_link(&mut self, href: String) -> LinkId {
        self.links.push(Link { href, range: 0..0 });
        (self.links.len() - 1) as LinkId
    }

    pub(crate) fn set_link_range(&mut self, id: LinkId, range: Range<usize>) {
        if let Some(l) = self.links.get_mut(id as usize) {
            l.range = range;
        }
    }

    pub(crate) fn finish(self) -> ParsedText {
        ParsedText {
            text: self.text,
            spans: self.spans,
            links: self.links,
        }
    }
}

/// A style with a link attached. Underlined so links read as links
/// regardless of what colour the message already carries.
fn link_style(base: Style, id: LinkId) -> Style {
    let mut s = base;
    s.link = Some(id);
    s.attrs = s.attrs.union(Attrs::UNDERLINE);
    s
}
