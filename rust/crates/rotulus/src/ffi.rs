//! The C ABI declared in `include/rotulus.h`.
//!
//! **Marks.** `RotulusMark *` is an opaque handle, and here it is a
//! [`MessageId`] rather than a pointer, encoded the way GLib encodes
//! integer handles (`GSIZE_TO_POINTER`). Ids start at 1, so `NULL` is
//! unambiguously "no mark"; on a 32-bit host the id space caps at 4
//! billion rows in one session, which a chat scrollback will not reach.
//! Nothing dereferences it, which is why a stale mark is inert rather
//! than dangling.
//!
//! **Widgets.** Every entry point takes a `GtkWidget *` the caller owns
//! and wraps it with a plain reference ([`view_of`]); none of them may
//! sink a floating reference, which is the easiest way to destroy a
//! widget the caller still thinks it has.

use crate::view::{RotulusView, PALETTE_COLS};
use gtk4::glib::translate::{IntoGlib, IntoGlibPtr, ToGlibPtr};
use gtk4::prelude::*;
use rotulus_layout::{
    Attrs, Block, ColorRef, LoadMoreDirection, Message, MessageFlags, MessageId, MessageKind,
    ParsedText, Span, Style,
};
use std::ffi::{c_char, c_int, c_uint, c_void, CStr};

type CGtkWidget = *mut gtk4::ffi::GtkWidget;

/// # Safety
/// `p` is NULL or a valid NUL-terminated C string.
unsafe fn cstr(p: *const c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

/// # Safety
/// `p` points to `len` readable bytes, or is NULL. A negative `len`
/// means `p` is NUL-terminated.
unsafe fn ctext(p: *const c_char, len: c_int) -> String {
    if p.is_null() {
        return String::new();
    }
    if len < 0 {
        return cstr(p);
    }
    let bytes = std::slice::from_raw_parts(p as *const u8, len as usize);
    String::from_utf8_lossy(bytes).into_owned()
}

/// A `stamp` of 0 means "now": nearly every live append passes 0 for
/// exactly that, and dating those rows to the epoch would make their
/// timestamps silently vanish.
fn stamp_or_now(stamp: i64) -> i64 {
    if stamp != 0 {
        return stamp;
    }
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

fn mark_to_ptr(id: MessageId) -> *mut c_void {
    id.0 as usize as *mut c_void
}

fn ptr_to_mark(p: *mut c_void) -> Option<MessageId> {
    match p as usize as u64 {
        0 => None,
        v => Some(MessageId(v)),
    }
}

/// Wrap a borrowed `GtkWidget *` from C, without disturbing its
/// floating reference.
///
/// **Not `from_glib_none`.** glib-rs implements that for objects as
/// `from_glib_full(g_object_ref_sink(ptr))`, which sinks a floating
/// reference into the wrapper. A view handed to C is floating, like any
/// GTK constructor's result, so the wrapper would own it, drop at the end
/// of the call, and destroy the widget on the first call made on it.
/// `g_object_ref` + `from_glib_full` takes a plain reference instead,
/// which also keeps the object alive for the whole call if a handler
/// reached from it drops the caller's.
///
/// # Safety
/// `w` is NULL or a valid `GtkWidget *` owned by the caller.
unsafe fn view_of(w: CGtkWidget) -> Option<RotulusView> {
    if w.is_null() {
        return None;
    }
    let widget: gtk4::Widget = gtk4::glib::translate::from_glib_full(
        gtk4::glib::gobject_ffi::g_object_ref(w as *mut gtk4::glib::gobject_ffi::GObject)
            as CGtkWidget,
    );
    widget.downcast::<RotulusView>().ok()
}

macro_rules! with_view {
    ($w:expr, $v:ident, $body:expr) => {{
        match view_of($w) {
            Some($v) => $body,
            None => Default::default(),
        }
    }};
}

/// Hand a freshly-built widget to C with refcount 1 and *floating*, the
/// way `g_object_new` returns a GTK widget.
///
/// `into_glib_ptr` alone transfers the reference but leaves the object
/// non-floating, because gtk-rs sinks the floating ref when it wraps an
/// `InitiallyUnowned`; a caller that then `g_object_ref_sink`s it, as C
/// callers do, would leak. `g_object_force_floating` restores the flag
/// without touching the count.
///
/// # Safety
/// Caller takes ownership of the returned pointer.
unsafe fn into_floating_ptr<W: IsA<gtk4::Widget>>(w: W) -> CGtkWidget {
    let ptr = w.upcast::<gtk4::Widget>().into_glib_ptr();
    gtk4::glib::gobject_ffi::g_object_force_floating(ptr as *mut gtk4::glib::gobject_ffi::GObject);
    ptr
}

// ---- types ----------------------------------------------------------

/// `RotulusRun`. C builds these on the stack and we only read them.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RotulusRun {
    pub text: *const c_char,
    pub len: c_int,
    pub color: i16,
    pub attrs: u16,
    pub background: i16,
    pub rgb: u32,
    pub background_rgb: u32,
}

pub const ROTULUS_COLOR_DEFAULT: i16 = -1;

pub const ATTR_BOLD: u16 = 1 << 0;
pub const ATTR_ITALIC: u16 = 1 << 1;
pub const ATTR_UNDERLINE: u16 = 1 << 2;
pub const ATTR_STRIKETHROUGH: u16 = 1 << 3;
pub const ATTR_MONOSPACE: u16 = 1 << 4;
pub const ATTR_REVERSE: u16 = 1 << 5;
pub const ATTR_BACKGROUND: u16 = 1 << 8;
pub const ATTR_RGB: u16 = 1 << 9;
pub const ATTR_BACKGROUND_RGB: u16 = 1 << 10;

/// `RotulusSpeaker`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RotulusSpeaker {
    pub key: u64,
    pub nick: *const c_char,
    pub nick_len: c_int,
}

/// `RotulusRowKind`.
pub const ROW_MESSAGE: c_int = 0;
pub const ROW_SYSTEM: c_int = 1;
pub const ROW_HISTORY: c_int = 2;
pub const ROW_DIVIDER: c_int = 3;
pub const ROW_LOAD_OLDER: c_int = 4;
pub const ROW_LOAD_NEWER: c_int = 5;

pub const ROW_OUTGOING: c_uint = 1 << 0;
pub const ROW_ACTION: c_uint = 1 << 1;

/// `RotulusRow`.
#[repr(C)]
pub struct RotulusRow {
    pub kind: c_int,
    pub flags: c_uint,
    pub stamp: i64,
    pub speaker: RotulusSpeaker,
    pub gutter: *const RotulusRun,
    pub n_gutter: c_int,
    pub body: *const RotulusRun,
    pub n_body: c_int,
}

/// `RotulusFrame`.
#[repr(C)]
pub struct RotulusFrame {
    pub texture: *mut gtk4::gdk::ffi::GdkTexture,
    pub delay_ms: u32,
}

// ---- runs to text ---------------------------------------------------

/// # Safety
/// `p` points to `n` readable values, or is NULL.
unsafe fn slice_of<'a, T>(p: *const T, n: c_int) -> &'a [T] {
    if p.is_null() || n <= 0 {
        &[]
    } else {
        std::slice::from_raw_parts(p, n as usize)
    }
}

/// The `Style` a run carries. One definition, so the uniformity test and
/// the span builder cannot disagree about what "same style" means.
pub(crate) fn run_style(r: &RotulusRun) -> Style {
    let mut attrs = Attrs::NONE;
    for (bit, a) in [
        (ATTR_BOLD, Attrs::BOLD),
        (ATTR_ITALIC, Attrs::ITALIC),
        (ATTR_UNDERLINE, Attrs::UNDERLINE),
        (ATTR_STRIKETHROUGH, Attrs::STRIKETHROUGH),
        (ATTR_MONOSPACE, Attrs::CODE),
        (ATTR_REVERSE, Attrs::REVERSE),
    ] {
        if r.attrs & bit != 0 {
            attrs = attrs.union(a);
        }
    }
    let fg = if r.attrs & ATTR_RGB != 0 {
        ColorRef::Rgb(r.rgb & 0xff_ffff)
    } else if r.color < 0 {
        ColorRef::Default
    } else {
        ColorRef::Palette(r.color.min(255) as u8)
    };
    let bg = if r.attrs & ATTR_BACKGROUND_RGB != 0 {
        ColorRef::Rgb(r.background_rgb & 0xff_ffff)
    } else if r.attrs & ATTR_BACKGROUND != 0 && r.background >= 0 {
        ColorRef::Palette(r.background.min(255) as u8)
    } else {
        ColorRef::Default
    };
    Style {
        attrs,
        fg,
        bg,
        link: None,
    }
}

/// Join a run array into one `ParsedText`.
///
/// # Safety
/// `runs` points to `n` readable `RotulusRun`s, each with a valid
/// `text`/`len` pair.
pub(crate) unsafe fn runs_to_text(runs: *const RotulusRun, n: c_int) -> ParsedText {
    let mut out = ParsedText::default();
    for r in slice_of(runs, n) {
        let text = ctext(r.text, r.len);
        if text.is_empty() {
            continue;
        }
        let start = out.text.len();
        out.text.push_str(&text);
        let style = run_style(r);
        // A plain run is the absence of a span, which keeps an unstyled
        // row's span list empty rather than one-per-run.
        if style != Style::default() {
            out.spans.push(Span {
                range: start..out.text.len(),
                style,
            });
        }
    }
    out
}

/// The single style every body run shares, or `None` when they differ.
///
/// Markdown is only applied to a *stylistically uniform* body. A body
/// assembled from several differently-styled runs was styled by the
/// caller on purpose — a divider, a status line, IRC formatting — and
/// re-parsing it would fight that.
unsafe fn uniform_style(runs: *const RotulusRun, n: c_int) -> Option<Style> {
    let mut seen: Option<Style> = None;
    for r in slice_of(runs, n) {
        let style = run_style(r);
        match seen {
            None => seen = Some(style),
            Some(prev) if prev == style => {}
            Some(_) => return None,
        }
    }
    seen
}

/// Lay `base` under `p`, so text the parser left unstyled still carries
/// the caller's color.
///
/// The renderer treats a gap between spans as *default* style, not as
/// "whatever the row's color was", so without this a muted history line
/// would come back with only its bold words muted.
fn under(p: ParsedText, base: Style) -> ParsedText {
    if base == Style::default() {
        return p;
    }
    let mut out = ParsedText {
        text: p.text,
        spans: Vec::with_capacity(p.spans.len() * 2 + 1),
        links: p.links,
    };
    let mut at = 0usize;
    for sp in p.spans {
        if sp.range.start > at {
            out.spans.push(Span {
                range: at..sp.range.start,
                style: base,
            });
        }
        at = sp.range.end;
        out.spans.push(Span {
            range: sp.range,
            style: Style {
                fg: if sp.style.fg == ColorRef::Default {
                    base.fg
                } else {
                    sp.style.fg
                },
                bg: if sp.style.bg == ColorRef::Default {
                    base.bg
                } else {
                    sp.style.bg
                },
                attrs: sp.style.attrs.union(base.attrs),
                ..sp.style
            },
        });
    }
    if at < out.text.len() {
        out.spans.push(Span {
            range: at..out.text.len(),
            style: base,
        });
    }
    out
}

/// Split a body into blocks, rendering markdown when it is on.
///
/// # Safety
/// As [`runs_to_text`].
pub(crate) unsafe fn body_blocks(
    runs: *const RotulusRun,
    n: c_int,
    markdown: bool,
    links: &rotulus_layout::Linkifier,
) -> Vec<Block> {
    use rotulus_layout::markdown::{parse_inline_with, split_blocks, RawBlock};
    let plain = runs_to_text(runs, n);

    let Some(base) = uniform_style(runs, n).filter(|_| markdown) else {
        // Markdown off, or a body the caller styled run by run: keep it
        // exactly as handed over.
        let mut b = plain;
        crate::links::autolink(&mut b, links);
        return vec![Block::Text(b)];
    };

    let mut out = Vec::new();
    for raw in split_blocks(&plain.text) {
        match raw {
            RawBlock::Paragraph(t) => {
                let mut p = under(parse_inline_with(&t, links), base);
                crate::links::autolink(&mut p, links);
                out.push(Block::Text(p));
            }
            // Fenced code is inert by definition: no inline parsing, and
            // no autolinking either — a URL inside a code fence is being
            // shown, not offered.
            RawBlock::Code { text, language } => out.push(Block::Code { text, language }),
            RawBlock::Quote { text, depth } => {
                let mut p = under(parse_inline_with(&text, links), base);
                crate::links::autolink(&mut p, links);
                out.push(Block::Quote { content: p, depth });
            }
        }
    }
    if out.is_empty() {
        out.push(Block::Text(ParsedText::default()));
    }
    out
}

/// `None` when the caller couldn't identify the speaker.
///
/// # Safety
/// `s.nick` is NULL or valid for `s.nick_len` bytes (or NUL-terminated
/// when it is negative).
pub(crate) unsafe fn speaker_of(s: &RotulusSpeaker) -> Option<rotulus_layout::Speaker> {
    if s.key == 0 {
        return None;
    }
    // Length-delimited as well as NUL-delimited: a caller slicing the
    // sender out of a received line hands over a pointer into the middle
    // of it, and reading to the NUL would take the rest of the line.
    Some(rotulus_layout::Speaker::new(
        s.key,
        ctext(s.nick, s.nick_len),
    ))
}

fn row_kind(kind: c_int) -> MessageKind {
    match kind {
        ROW_SYSTEM => MessageKind::System,
        ROW_HISTORY => MessageKind::History {
            server_message_id: 0,
        },
        ROW_DIVIDER => MessageKind::Divider,
        ROW_LOAD_OLDER => MessageKind::LoadMore(LoadMoreDirection::Older),
        ROW_LOAD_NEWER => MessageKind::LoadMore(LoadMoreDirection::Newer),
        _ => MessageKind::Live,
    }
}

/// Build a message from a C row, under the view's own markdown and link
/// settings.
///
/// # Safety
/// `row` points to a valid `RotulusRow` whose run arrays are readable.
pub(crate) unsafe fn row_message(view: &RotulusView, row: &RotulusRow) -> Message {
    // The gutter is never markdown-parsed: a nick containing asterisks is
    // a nick, not emphasis.
    let gutter = runs_to_text(row.gutter, row.n_gutter);
    // Borrowed rather than cloned: a clone is a dozen allocations per row.
    let links = view.imp_ref().linkifier.borrow();
    let blocks = body_blocks(row.body, row.n_body, view.markdown(), &links);
    drop(links);
    let mut flags = MessageFlags::NONE;
    if row.flags & ROW_OUTGOING != 0 {
        flags = flags.union(MessageFlags::OUTGOING);
    }
    if row.flags & ROW_ACTION != 0 {
        flags = flags.union(MessageFlags::ACTION);
    }
    Message {
        kind: row_kind(row.kind),
        timestamp: stamp_or_now(row.stamp),
        speaker: speaker_of(&row.speaker),
        gutter: if gutter.text.is_empty() {
            None
        } else {
            Some(gutter)
        },
        blocks: blocks.into(),
        flags,
    }
}

// ---- construction and configuration --------------------------------

#[no_mangle]
pub extern "C" fn rotulus_view_new() -> CGtkWidget {
    crate::ensure_gtk_init();
    unsafe { into_floating_ptr(RotulusView::new()) }
}

#[no_mangle]
pub extern "C" fn rotulus_view_get_type() -> gtk4::glib::ffi::GType {
    crate::ensure_gtk_init();
    RotulusView::static_type().into_glib()
}

#[no_mangle]
pub extern "C" fn rotulus_load_direction_get_type() -> gtk4::glib::ffi::GType {
    crate::view::LoadDirection::static_type().into_glib()
}

/// # Safety
/// `w` is a valid `RotulusView *`; `palette` points to `ROTULUS_PAL_COLS`
/// `GdkRGBA`s, or is NULL.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_set_palette(
    w: CGtkWidget,
    palette: *const gtk4::gdk::ffi::GdkRGBA,
) {
    if palette.is_null() {
        return;
    }
    with_view!(w, v, {
        let mut pal = [gtk4::gdk::RGBA::BLACK; PALETTE_COLS];
        for (dst, s) in pal
            .iter_mut()
            .zip(std::slice::from_raw_parts(palette, PALETTE_COLS))
        {
            *dst = gtk4::gdk::RGBA::new(s.red, s.green, s.blue, s.alpha);
        }
        v.set_palette(&pal);
    })
}

/// # Safety
/// `w` is a valid `RotulusView *`; `font` is a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_set_font(w: CGtkWidget, font: *const c_char) {
    with_view!(w, v, v.set_font_from_string(&cstr(font)))
}

macro_rules! bool_setter {
    ($name:ident, $method:ident) => {
        /// # Safety
        /// `w` is a valid `RotulusView *`.
        #[no_mangle]
        pub unsafe extern "C" fn $name(w: CGtkWidget, on: c_int) {
            with_view!(w, v, v.$method(on != 0))
        }
    };
}

bool_setter!(rotulus_view_set_word_wrap, set_word_wrap);
bool_setter!(rotulus_view_set_indent, set_indent);
bool_setter!(rotulus_view_set_separator, set_separator);
bool_setter!(rotulus_view_set_show_timestamps, set_time_stamp);
bool_setter!(rotulus_view_set_markdown, set_markdown);
bool_setter!(rotulus_view_set_autocopy, set_autocopy);
bool_setter!(rotulus_view_set_copy_timestamps, set_copy_timestamps);
bool_setter!(rotulus_view_set_activate_links, set_activate_links);

/// # Safety
/// `w` is a valid `RotulusView *`.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_set_max_lines(w: CGtkWidget, n: c_int) {
    with_view!(w, v, v.set_max_rows(n))
}

/// # Safety
/// `w` is a valid `RotulusView *`.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_set_max_indent(w: CGtkWidget, px: c_int) {
    with_view!(w, v, v.set_max_indent(px))
}

/// # Safety
/// `w` is a valid `RotulusView *`; `fmt` is NULL or NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_set_timestamp_format(w: CGtkWidget, fmt: *const c_char) {
    with_view!(w, v, v.set_stamp_format(&cstr(fmt)))
}

/// # Safety
/// `w` is a valid `RotulusView *`.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_set_avatar_size(w: CGtkWidget, px: c_int) {
    with_view!(w, v, v.set_avatar_size(px.max(0) as u32))
}

/// # Safety
/// `w` is a valid `RotulusView *`.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_set_group_gap(w: CGtkWidget, secs: c_int) {
    with_view!(w, v, v.set_group_gap_secs(i64::from(secs)))
}

/// # Safety
/// `w` is a valid `RotulusView *`.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_set_zoom(w: CGtkWidget, zoom: f64) {
    with_view!(
        w,
        v,
        v.set_zoom_permille((zoom.clamp(0.1, 10.0) * 1000.0).round() as u32)
    )
}

/// # Safety
/// `w` is a valid `RotulusView *`; `schemes` is NULL or a NULL-terminated
/// array of NUL-terminated strings.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_set_link_schemes(
    w: CGtkWidget,
    schemes: *const *const c_char,
) {
    let mut list = Vec::new();
    if !schemes.is_null() {
        let mut p = schemes;
        while !(*p).is_null() {
            list.push(cstr(*p));
            p = p.add(1);
        }
    }
    with_view!(w, v, {
        let refs: Vec<&str> = list.iter().map(String::as_str).collect();
        v.set_link_schemes(&refs);
    })
}

/// `RotulusAvatarFunc`.
pub type AvatarFuncC =
    unsafe extern "C" fn(CGtkWidget, u64, *mut c_void) -> *mut gtk4::gdk::ffi::GdkPaintable;

/// The C side of an avatar function: its pointer, its data, and what to
/// call when the view lets go of both.
struct CAvatar {
    func: AvatarFuncC,
    data: *mut c_void,
    destroy: Option<unsafe extern "C" fn(*mut c_void)>,
}

impl Drop for CAvatar {
    fn drop(&mut self) {
        if let Some(d) = self.destroy {
            unsafe { d(self.data) };
        }
    }
}

/// # Safety
/// `w` is a valid `RotulusView *`; `func` is NULL or a valid function
/// pointer, called with `data` until the view is destroyed or the
/// function is replaced, when `destroy` (if any) is called on `data`.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_set_avatar_func(
    w: CGtkWidget,
    func: Option<AvatarFuncC>,
    data: *mut c_void,
    destroy: Option<unsafe extern "C" fn(*mut c_void)>,
) {
    let Some(v) = view_of(w) else {
        if let Some(d) = destroy {
            d(data);
        }
        return;
    };
    let Some(func) = func else {
        if let Some(d) = destroy {
            d(data);
        }
        v.set_avatar_func(None);
        return;
    };
    let c = CAvatar {
        func,
        data,
        destroy,
    };
    v.set_avatar_func(Some(Box::new(move |view: &RotulusView, key| {
        let p = (c.func)(
            view.upcast_ref::<gtk4::Widget>().to_glib_none().0,
            key,
            c.data,
        );
        if p.is_null() {
            None
        } else {
            // Borrowed, per the contract: take our own reference.
            Some(gtk4::glib::translate::from_glib_none(p))
        }
    })));
}

/// # Safety
/// `w` is a valid `RotulusView *`.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_get_vadjustment(
    w: CGtkWidget,
) -> *mut gtk4::ffi::GtkAdjustment {
    match view_of(w) {
        Some(v) => {
            // Created on demand when the view isn't in a GtkScrolledWindow,
            // for a bare GtkScrollbar packed beside it.
            let adj = match v.vadjustment() {
                Some(a) => a,
                None => {
                    let a = gtk4::Adjustment::new(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
                    v.set_vadjustment(Some(&a));
                    a
                }
            };
            // Borrowed: the view holds a reference in either branch, and a
            // caller that keeps it (gtk_scrollbar_new) takes its own.
            adj.to_glib_none().0
        }
        None => std::ptr::null_mut(),
    }
}

// ---- content --------------------------------------------------------

/// # Safety
/// `w` is a valid `RotulusView *`; `row` points to a valid `RotulusRow`.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_append(w: CGtkWidget, row: *const RotulusRow) -> *mut c_void {
    match (view_of(w), row.as_ref()) {
        (Some(v), Some(row)) => mark_to_ptr(v.append(row_message(&v, row))),
        _ => std::ptr::null_mut(),
    }
}

/// # Safety
/// As [`rotulus_view_append`]; `anchor` is a mark or NULL.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_insert_before(
    w: CGtkWidget,
    anchor: *mut c_void,
    row: *const RotulusRow,
) -> *mut c_void {
    match (view_of(w), row.as_ref()) {
        (Some(v), Some(row)) => {
            let msg = row_message(&v, row);
            mark_to_ptr(v.insert_before(ptr_to_mark(anchor), msg))
        }
        _ => std::ptr::null_mut(),
    }
}

/// # Safety
/// As [`rotulus_view_append`]; `mark` is a mark or NULL.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_replace(
    w: CGtkWidget,
    mark: *mut c_void,
    row: *const RotulusRow,
) -> c_int {
    match (view_of(w), ptr_to_mark(mark), row.as_ref()) {
        (Some(v), Some(id), Some(row)) => {
            let msg = row_message(&v, row);
            c_int::from(v.replace(id, msg))
        }
        _ => 0,
    }
}

/// # Safety
/// `w` is a valid `RotulusView *`; `mark` is a mark or NULL.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_remove(w: CGtkWidget, mark: *mut c_void) -> c_int {
    match (view_of(w), ptr_to_mark(mark)) {
        (Some(v), Some(id)) => c_int::from(v.remove(id)),
        _ => 0,
    }
}

/// # Safety
/// `w` is a valid `RotulusView *`; `text` points to `len` readable bytes,
/// or is NUL-terminated when `len` is negative.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_append_text(
    w: CGtkWidget,
    text: *const c_char,
    len: c_int,
    stamp: i64,
) -> *mut c_void {
    let run = RotulusRun {
        text,
        len,
        color: ROTULUS_COLOR_DEFAULT,
        attrs: 0,
        background: 0,
        rgb: 0,
        background_rgb: 0,
    };
    let row = RotulusRow {
        kind: ROW_MESSAGE,
        flags: 0,
        stamp,
        speaker: RotulusSpeaker {
            key: 0,
            nick: std::ptr::null(),
            nick_len: -1,
        },
        gutter: std::ptr::null(),
        n_gutter: 0,
        body: &run,
        n_body: 1,
    };
    rotulus_view_append(w, &row)
}

/// # Safety
/// `w` is a valid `RotulusView *`.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_clear(w: CGtkWidget) {
    with_view!(w, v, v.clear())
}

/// # Safety
/// `w` is a valid `RotulusView *`.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_get_last(w: CGtkWidget) -> *mut c_void {
    with_view!(w, v, v.last().map_or(std::ptr::null_mut(), mark_to_ptr))
}

/// # Safety
/// `w` is a valid `RotulusView *`; `mark` is a mark or NULL.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_set_marker(w: CGtkWidget, mark: *mut c_void) {
    with_view!(w, v, v.set_marker(ptr_to_mark(mark)))
}

// ---- inline media ---------------------------------------------------

/// # Safety
/// `w` is a valid `RotulusView *`; `texture` is NULL or a `GdkTexture *`;
/// `alt` is NULL or NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_append_media(
    w: CGtkWidget,
    texture: *mut gtk4::gdk::ffi::GdkTexture,
    alt: *const c_char,
    token: c_uint,
    stamp: i64,
) -> *mut c_void {
    let Some(v) = view_of(w) else {
        return std::ptr::null_mut();
    };
    // Append the row *first*: installing frames attaches the decoded size
    // by finding the row with the token, so frames installed before the
    // row exists find nothing, and the row shows its alt text forever.
    let id = v.append(Message {
        kind: MessageKind::Live,
        timestamp: stamp_or_now(stamp),
        speaker: None,
        gutter: None,
        blocks: vec![Block::Image {
            token,
            size: None,
            alt: cstr(alt),
        }]
        .into(),
        flags: MessageFlags::NONE,
    });
    if !texture.is_null() {
        let tex: gtk4::gdk::Texture = gtk4::glib::translate::from_glib_none(texture);
        v.set_media_frames(token, vec![(tex, 0)]);
    }
    mark_to_ptr(id)
}

/// # Safety
/// `w` is a valid `RotulusView *`.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_media_mark(w: CGtkWidget, token: c_uint) -> *mut c_void {
    match view_of(w) {
        Some(v) => v
            .find_image(token)
            .map_or(std::ptr::null_mut(), mark_to_ptr),
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// `w` is a valid `RotulusView *`; `texture` is NULL or a `GdkTexture *`.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_media_set_texture(
    w: CGtkWidget,
    mark: *mut c_void,
    texture: *mut gtk4::gdk::ffi::GdkTexture,
) {
    let Some(v) = view_of(w) else { return };
    let Some(token) = ptr_to_mark(mark).and_then(|id| v.image_token_of(id)) else {
        return;
    };
    let frames = if texture.is_null() {
        Vec::new()
    } else {
        vec![(gtk4::glib::translate::from_glib_none(texture), 0)]
    };
    v.set_media_frames(token, frames);
}

/// # Safety
/// `w` is a valid `RotulusView *`; `frames` points to `n` readable
/// `RotulusFrame`s, or is NULL.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_media_set_frames(
    w: CGtkWidget,
    mark: *mut c_void,
    frames: *const RotulusFrame,
    n: c_uint,
) {
    let Some(v) = view_of(w) else { return };
    let Some(token) = ptr_to_mark(mark).and_then(|id| v.image_token_of(id)) else {
        return;
    };
    let out = slice_of(frames, n.min(c_int::MAX as c_uint) as c_int)
        .iter()
        .filter(|f| !f.texture.is_null())
        // from_glib_none: the caller keeps its references; we take ours.
        .map(|f| (gtk4::glib::translate::from_glib_none(f.texture), f.delay_ms))
        .collect();
    v.set_media_frames(token, out);
}

// ---- search ---------------------------------------------------------

/// Write the `(total, ordinal)` readout through the out-params, either of
/// which may be NULL.
unsafe fn put_readout(n_matches: *mut c_uint, current: *mut c_uint, r: (usize, usize)) {
    if let Some(n) = n_matches.as_mut() {
        *n = r.0 as c_uint;
    }
    if let Some(c) = current.as_mut() {
        *c = r.1 as c_uint;
    }
}

/// # Safety
/// `w` is a valid `RotulusView *`; `needle` is NULL or NUL-terminated;
/// the out-params are writable or NULL.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_search(
    w: CGtkWidget,
    needle: *const c_char,
    case_sensitive: c_int,
    n_matches: *mut c_uint,
    current: *mut c_uint,
) {
    let needle = cstr(needle);
    let r = with_view!(w, v, v.search_set(&needle, case_sensitive != 0));
    put_readout(n_matches, current, r);
}

/// # Safety
/// `w` is a valid `RotulusView *`; the out-params are writable or NULL.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_search_step(
    w: CGtkWidget,
    dir: c_int,
    n_matches: *mut c_uint,
    current: *mut c_uint,
) {
    let r = with_view!(w, v, v.search_step(dir));
    put_readout(n_matches, current, r);
}

/// # Safety
/// `w` is a valid `RotulusView *`.
#[no_mangle]
pub unsafe extern "C" fn rotulus_view_search_clear(w: CGtkWidget) {
    with_view!(w, v, v.search_clear())
}
