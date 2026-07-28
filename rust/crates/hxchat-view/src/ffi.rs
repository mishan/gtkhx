//! The C ABI.
//!
//! Mirrors `src/chat_view.h`, one `hx_chat_view_*` per
//! `hx_chat_view_*`. The C side (`chat_view.c`) is the dispatcher: it
//! decides at construction which backend a view is, then routes every
//! later call by widget type. That is why these are `_impl_`-prefixed
//! rather than simply defining the public names — both backends have to
//! coexist in one binary until C5.
//!
//! **Marks.** `chat_view.h`'s `HxChatMark *` is an opaque handle, and
//! here it is a [`MessageId`] rather than a pointer, encoded the way
//! GLib encodes integer handles (`GSIZE_TO_POINTER`). Ids start at 1, so
//! `NULL` is unambiguously "no mark"; on a 32-bit host the id space caps
//! at 4 billion rows in one session, which a chat scrollback will not
//! reach. Nothing dereferences it — that is the entire point of the
//! opaque type, and the reason a stale mark here is inert where xtext's
//! raw `textentry *` would have dangled.

use crate::view::{HxChatView, PALETTE_COLS};
use gtk4::glib::translate::{IntoGlib, IntoGlibPtr, ToGlibPtr};
use gtk4::prelude::*;
use hxchat_layout::{Block, ColorRef, Message, MessageId, MessageKind, ParsedText, Style};
use std::ffi::{c_char, c_int, c_void, CStr};

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
/// `p` points to `len` readable bytes, or is NULL.
unsafe fn cslice(p: *const c_char, len: c_int) -> String {
    if p.is_null() || len <= 0 {
        return String::new();
    }
    let bytes = std::slice::from_raw_parts(p as *const u8, len as usize);
    String::from_utf8_lossy(bytes).into_owned()
}

/// Resolve the `stamp` argument the way xtext does.
///
/// `gtk_xtext_append_entry` (xtext.c:5399) substitutes `time (0)` when
/// the caller passes 0, and nearly every call site in `chat.c` / `msg.c`
/// passes 0 for live messages precisely to get that. Passing the 0
/// through would silently date every live message to the epoch — and
/// since the timestamp column is what surfaces it, the visible symptom
/// would be timestamps quietly disappearing on the new backend.
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
    let v = p as usize as u64;
    if v == 0 {
        None
    } else {
        Some(MessageId(v))
    }
}

/// Wrap a borrowed `GtkWidget *` from C, without disturbing its
/// floating reference.
///
/// **Not `from_glib_none`.** That is the obvious choice and it is wrong
/// here, in a way that is worth spelling out because it cost a long
/// debugging session. glib-rs implements `from_glib_none` for objects as
/// `from_glib_full(g_object_ref_sink(ptr))` — its own source carries the
/// warning "Attention: this takes ownership of floating references". Our
/// widgets are handed to C *floating* with refcount 1 (see
/// [`into_floating_ptr`]), because that is `gtk_xtext_new`'s contract
/// and `chat.c` sinks it later. So `from_glib_none` sank the floating
/// ref into the Rust wrapper, the wrapper dropped at the end of the
/// call, refcount hit zero, and the widget was destroyed by the *first*
/// FFI call made on it — `hx_chat_view_set_font`, one line after
/// construction. Everything afterwards operated on freed memory:
/// `gtk_widget_set_can_focus` failed `GTK_IS_WIDGET`, `is_hxchat` read a
/// dead type and answered "no", and the call was dispatched into xtext,
/// segfaulting in `gtk_xtext_set_time_stamp`.
///
/// `g_object_ref` + `from_glib_full` instead: a plain reference, which
/// leaves the floating flag alone. The wrapper drops its own reference
/// on scope exit and the object survives with the caller's floating
/// reference intact. This is also the re-entrancy-safe "full" form in
/// the sense of `docs/rust/glib-interop.md` — the object cannot be freed
/// underneath us mid-call — so we keep that property too.
///
/// # Safety
/// `w` is NULL or a valid `GtkWidget *` owned by the caller.
unsafe fn view_of(w: CGtkWidget) -> Option<HxChatView> {
    if w.is_null() {
        return None;
    }
    let widget: gtk4::Widget = gtk4::glib::translate::from_glib_full(
        gtk4::glib::gobject_ffi::g_object_ref(w as *mut gtk4::glib::gobject_ffi::GObject)
            as CGtkWidget,
    );
    widget.downcast::<HxChatView>().ok()
}

macro_rules! with_view {
    ($w:expr, $v:ident, $body:expr) => {{
        match view_of($w) {
            Some($v) => $body,
            None => Default::default(),
        }
    }};
}

// ---- construction / configuration ---------------------------------

/// # Safety
/// `palette` points to `HX_CHAT_PAL_COLS` `GdkRGBA`s, or is NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_new(
    palette: *const gtk4::gdk::ffi::GdkRGBA,
    separator: c_int,
) -> CGtkWidget {
    crate::ensure_gtk_init();
    let view = HxChatView::new();
    view.set_separator(separator != 0);
    if !palette.is_null() {
        let mut pal = [gtk4::gdk::RGBA::BLACK; PALETTE_COLS];
        let src = std::slice::from_raw_parts(palette, PALETTE_COLS);
        for (dst, s) in pal.iter_mut().zip(src.iter()) {
            *dst = gtk4::gdk::RGBA::new(s.red, s.green, s.blue, s.alpha);
        }
        view.set_palette(&pal);
    }
    into_floating_ptr(view)
}

/// Hand a freshly-built widget to C with refcount 1 and *floating*.
///
/// Same helper, same two lines, as `gtkhx-ui`'s `into_floating_ptr`
/// (`chat.rs:71`, and three more call sites in `news.rs`, `emoji.rs`,
/// `voice_panel.rs`). Worth spelling out why both halves are needed,
/// because getting either wrong is a crash or a leak:
///
/// - `to_glib_none()` would hand back a *borrowed* pointer and then drop
///   the wrapper at scope exit, taking the last reference with it. That
///   was the C2 startup crash: C held freed memory, and the SIGSEGV
///   landed in `gtk_xtext_set_font` because `is_hxchat` read the dead
///   object's type, failed the check, and dispatched to the wrong
///   backend.
/// - `into_glib_ptr` alone transfers the reference but leaves the object
///   non-floating, because gtk-rs sinks the floating ref when it wraps
///   an `InitiallyUnowned`. `g_object_new` — and so `gtk_xtext_new` —
///   returns refcount 1 *and* floating, and `chat.c:1789` does
///   `g_object_ref_sink (text)` on the result. A non-floating ref there
///   leaks instead of sinking.
///
/// `g_object_force_floating` restores the flag without touching the
/// count, so the two backends are genuinely interchangeable.
///
/// # Safety
/// Caller takes ownership of the returned pointer.
unsafe fn into_floating_ptr<W: IsA<gtk4::Widget>>(w: W) -> CGtkWidget {
    let ptr = w.upcast::<gtk4::Widget>().into_glib_ptr();
    gtk4::glib::gobject_ffi::g_object_force_floating(
        ptr as *mut gtk4::glib::gobject_ffi::GObject,
    );
    debug_assert!(
        gtk4::glib::gobject_ffi::g_object_is_floating(
            ptr as *mut gtk4::glib::gobject_ffi::GObject
        ) != 0,
        "a widget handed to C must be floating, like a GTK C constructor"
    );
    ptr
}

/// # Safety
/// See module docs; `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_get_type() -> gtk4::glib::ffi::GType {
    crate::ensure_gtk_init();
    use gtk4::glib::prelude::StaticType;
    HxChatView::static_type().into_glib()
}

/// # Safety
/// `w` is a valid `HxChatView *`; `font` a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_set_font(w: CGtkWidget, font: *const c_char) {
    with_view!(w, v, {
        let f = cstr(font);
        if !f.is_empty() {
            v.set_font_from_string(&f);
        }
    })
}

/// # Safety
/// `palette` points to `HX_CHAT_PAL_COLS` `GdkRGBA`s.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_set_palette(
    w: CGtkWidget,
    palette: *const gtk4::gdk::ffi::GdkRGBA,
) {
    if palette.is_null() {
        return;
    }
    with_view!(w, v, {
        let mut pal = [gtk4::gdk::RGBA::BLACK; PALETTE_COLS];
        let src = std::slice::from_raw_parts(palette, PALETTE_COLS);
        for (dst, s) in pal.iter_mut().zip(src.iter()) {
            *dst = gtk4::gdk::RGBA::new(s.red, s.green, s.blue, s.alpha);
        }
        v.set_palette(&pal);
    })
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_set_word_wrap(w: CGtkWidget, on: c_int) {
    with_view!(w, v, v.set_word_wrap(on != 0))
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_set_max_lines(w: CGtkWidget, n: c_int) {
    with_view!(w, v, v.set_max_rows(n))
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_set_indent(w: CGtkWidget, on: c_int) {
    with_view!(w, v, v.set_indent(on != 0))
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_set_max_indent(w: CGtkWidget, px: c_int) {
    with_view!(w, v, v.set_max_indent(px))
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_set_time_stamp(w: CGtkWidget, on: c_int) {
    with_view!(w, v, v.set_time_stamp(on != 0))
}

/// # Safety
/// `w` is a valid `HxChatView *` or NULL; `fmt` a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_set_stamp_format(
    w: CGtkWidget,
    fmt: *const c_char,
) {
    let f = cstr(fmt);
    // A NULL view means "format only", which `prefs_read` does before
    // any window exists. Recording it process-wide is load-bearing: it
    // is the *only* time the persisted pref is delivered, so dropping it
    // would leave every view on the built-in default until the user
    // happened to edit the setting again.
    crate::view::prefs::STAMP_FORMAT.with(|slot| {
        *slot.borrow_mut() = if f.is_empty() { None } else { Some(f.clone()) };
    });
    if !w.is_null() {
        with_view!(w, v, v.set_stamp_format(&f))
    }
}

/// The word classifier `chat_view.h` takes.
///
/// Typed rather than `*mut c_void` on purpose: casting a function
/// pointer to a data pointer is undefined behaviour in C — the standard
/// does not guarantee they are even the same width — so the dispatcher
/// must be able to pass this through without an illegal cast. Matching
/// the real signature on both sides is also what makes a link-time
/// signature mismatch impossible.
pub type UrlCheckFn = unsafe extern "C" fn(CGtkWidget, *mut c_char) -> c_int;

/// # Safety
/// `w` is a valid `HxChatView *`; `f` is NULL or a valid function pointer.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_set_urlcheck_function(
    w: CGtkWidget,
    f: Option<UrlCheckFn>,
) {
    // C3: link activation is part of the interaction phase, and it will
    // arrive as a typed `link-activated` signal rather than a
    // word-classifier callback (scoping §3.6). Accepted and dropped so
    // the dispatcher stays uniform — but accepted with its real type, so
    // no caller has to launder it through void*.
    let _ = (w, f);
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_get_vadjustment(
    w: CGtkWidget,
) -> *mut gtk4::ffi::GtkAdjustment {
    match view_of(w) {
        Some(v) => {
            // Create one on demand if the view isn't in a
            // GtkScrolledWindow — chat.c packs a bare GtkScrollbar and
            // hands it this adjustment, exactly as it did with xtext.
            let adj = match v.vadjustment() {
                Some(a) => a,
                None => {
                    let a = gtk4::Adjustment::new(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
                    v.set_vadjustment(Some(&a));
                    a
                }
            };
            // Borrowed, matching xtext's `return xtext->adj;`. Safe
            // because the view itself holds a reference in either branch
            // above — in the None branch `set_vadjustment` stored one
            // before the local wrapper drops — and the caller
            // (`gtk_scrollbar_new`) takes its own.
            adj.to_glib_none().0
        }
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_refresh(w: CGtkWidget) {
    with_view!(w, v, v.queue_draw())
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_clear(w: CGtkWidget) {
    with_view!(w, v, v.clear())
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_set_zoom_permille(w: CGtkWidget, zoom: c_int) {
    with_view!(w, v, v.set_zoom_permille(zoom.max(0) as u32))
}

/// # Safety
/// Process-wide; takes no view.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_set_autocopy_text(enabled: c_int) {
    crate::view::prefs::AUTOCOPY_TEXT.with(|c| c.set(enabled != 0));
}

/// # Safety
/// Process-wide; takes no view.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_set_autocopy_stamp(enabled: c_int) {
    crate::view::prefs::AUTOCOPY_STAMP.with(|c| c.set(enabled != 0));
}

/// # Safety
/// Process-wide; takes no view.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_set_autocopy_color(enabled: c_int) {
    crate::view::prefs::AUTOCOPY_COLOR.with(|c| c.set(enabled != 0));
}

// ---- appending -----------------------------------------------------

/// Build a message from the compat path's two mIRC strings.
///
/// The left column keeps its own styling via `Message::gutter` — see the
/// field's docs for why a bare `Speaker { nick }` can't reproduce what
/// `chat.c` emits.
/// Build a row from two plain strings.
///
/// Plain, now: these used to run through `mirc::parse`, which decoded
/// the in-band `\003NN` escape vocabulary. Nothing produces those any
/// more — style arrives as runs (`hx_chat_view_append_runs`) — and
/// continuing to *interpret* them here would be actively worse than
/// useless, because the remaining callers pass text that came off the
/// wire. A server could set colours in your chat log by sending the
/// bytes. It cannot now: they are characters like any other.
fn compat_message(left: &str, right: &str, stamp: i64) -> Message {
    let gutter = if left.is_empty() {
        None
    } else {
        Some(ParsedText::plain(left))
    };
    let mut body = ParsedText::plain(right);
    crate::links::autolink(&mut body);
    Message {
        kind: MessageKind::Live,
        timestamp: stamp_or_now(stamp),
        speaker: None,
        gutter,
        blocks: vec![Block::Text(body)],
        flags: hxchat_layout::MessageFlags::NONE,
    }
}

/// # Safety
/// `w` is a valid `HxChatView *` or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_set_group_gap(w: CGtkWidget, secs: c_int) {
    with_view!(w, v, v.set_group_gap_secs(secs as i64));
}

// ---- styled runs (C6) ----------------------------------------------

/// `HxChatRun` from `chat_view.h`. Layout is pinned by the assertion
/// below; C builds these on the stack and we only read them.
#[repr(C)]
pub struct HxChatRun {
    pub(crate) text: *const c_char,
    pub(crate) len: c_int,
    pub(crate) color: i16,
    pub(crate) attrs: u16,
}

const HX_CHAT_COLOR_DEFAULT: i16 = -1;

/// `HxChatSpeaker` from `chat_view.h`.
#[repr(C)]
pub struct HxChatSpeaker {
    pub(crate) uid: u16,
    pub(crate) nick: *const c_char,
    pub(crate) outgoing: c_int,
}

/// `None` when the caller couldn't identify the speaker.
///
/// uid 0 means "not known", not "user zero". Chat messages carry a UID
/// chunk and the C side passes it straight through, but the chunk is
/// optional — older servers omit it, and `parse_chat` defaults it to 0 —
/// so the caller falls back to a nick lookup, which can itself miss.
/// Guessing would attach the wrong avatar and group two people's
/// messages together, so a miss stays a miss.
unsafe fn speaker_of(s: &HxChatSpeaker) -> Option<hxchat_layout::Speaker> {
    if s.uid == 0 {
        return None;
    }
    let nick = if s.nick.is_null() {
        String::new()
    } else {
        cstr(s.nick)
    };
    Some(hxchat_layout::Speaker::new(s.uid, nick))
}

/// Mirror of chat_view.h's `HX_CHAT_ATTR_*`.
fn attrs_from_c(bits: u16) -> hxchat_layout::Attrs {
    let mut a = hxchat_layout::Attrs::NONE;
    if bits & (1 << 0) != 0 {
        a = a.union(hxchat_layout::Attrs::BOLD);
    }
    if bits & (1 << 1) != 0 {
        a = a.union(hxchat_layout::Attrs::ITALIC);
    }
    if bits & (1 << 2) != 0 {
        a = a.union(hxchat_layout::Attrs::UNDERLINE);
    }
    a
}

/// Build a `ParsedText` from a C run array.
///
/// # Safety
/// `runs` points to `n` readable `HxChatRun`s, each with a valid
/// `text`/`len` pair.
pub(crate) unsafe fn runs_to_text(runs: *const HxChatRun, n: c_int) -> ParsedText {
    let mut out = ParsedText::default();
    if runs.is_null() || n <= 0 {
        return out;
    }
    for i in 0..n as usize {
        let r = &*runs.add(i);
        let text = cslice(r.text, r.len);
        if text.is_empty() {
            continue;
        }
        let start = out.text.len();
        out.text.push_str(&text);
        let style = Style {
            fg: if r.color == HX_CHAT_COLOR_DEFAULT {
                ColorRef::Default
            } else {
                ColorRef::Palette(r.color as u8)
            },
            attrs: attrs_from_c(r.attrs),
            ..Style::default()
        };
        // Only record a span when it actually says something; a plain
        // run is the absence of a span, which is what keeps an
        // unstyled row's span list empty rather than one-per-run.
        if style != Style::default() {
            out.spans.push(hxchat_layout::Span {
                range: start..out.text.len(),
                style,
            });
        }
    }
    out
}

/// # Safety
/// `gutter` / `body` point to their respective run counts, or are NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_append_runs(
    w: CGtkWidget,
    speaker: HxChatSpeaker,
    gutter: *const HxChatRun,
    n_gutter: c_int,
    body: *const HxChatRun,
    n_body: c_int,
    stamp: i64,
) -> *mut c_void {
    match view_of(w) {
        Some(v) => mark_to_ptr(
            v.append(runs_message(&speaker, gutter, n_gutter, body, n_body, stamp)),
        ),
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// As `hx_chat_view_append_runs`; `anchor` is a mark or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_insert_runs_before(
    w: CGtkWidget,
    anchor: *mut c_void,
    speaker: HxChatSpeaker,
    gutter: *const HxChatRun,
    n_gutter: c_int,
    body: *const HxChatRun,
    n_body: c_int,
    stamp: i64,
) -> *mut c_void {
    match view_of(w) {
        Some(v) => {
            let msg = runs_message(&speaker, gutter, n_gutter, body, n_body, stamp);
            mark_to_ptr(v.insert_before(ptr_to_mark(anchor), msg))
        }
        None => std::ptr::null_mut(),
    }
}

unsafe fn runs_message(
    speaker: &HxChatSpeaker,
    gutter: *const HxChatRun,
    n_gutter: c_int,
    body: *const HxChatRun,
    n_body: c_int,
    stamp: i64,
) -> Message {
    let g = runs_to_text(gutter, n_gutter);
    let mut b = runs_to_text(body, n_body);
    crate::links::autolink(&mut b);
    Message {
        kind: MessageKind::Live,
        timestamp: stamp_or_now(stamp),
        speaker: speaker_of(speaker),
        gutter: if g.text.is_empty() { None } else { Some(g) },
        blocks: vec![Block::Text(b)],
        flags: if speaker.outgoing != 0 {
            hxchat_layout::MessageFlags::OUTGOING
        } else {
            hxchat_layout::MessageFlags::NONE
        },
    }
}

/// # Safety
/// `text` points to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_append(
    w: CGtkWidget,
    text: *const c_char,
    len: c_int,
    stamp: i64,
) {
    with_view!(w, v, {
        let raw = cslice(text, len);
        let mut body = ParsedText::plain(&raw);
        crate::links::autolink(&mut body);
        v.append(Message {
            kind: MessageKind::Live,
            timestamp: stamp_or_now(stamp),
            speaker: None,
            gutter: None,
            blocks: vec![Block::Text(body)],
            flags: hxchat_layout::MessageFlags::NONE,
        });
    })
}

/// # Safety
/// `left` / `right` point to their respective readable byte counts.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_append_indent(
    w: CGtkWidget,
    left: *const c_char,
    left_len: c_int,
    right: *const c_char,
    right_len: c_int,
    stamp: i64,
) -> *mut c_void {
    match view_of(w) {
        Some(v) => {
            let l = cslice(left, left_len);
            let r = cslice(right, right_len);
            mark_to_ptr(v.append(compat_message(&l, &r, stamp)))
        }
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// `left` / `right` point to their respective readable byte counts.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_insert_before(
    w: CGtkWidget,
    anchor: *mut c_void,
    left: *const c_char,
    left_len: c_int,
    right: *const c_char,
    right_len: c_int,
    stamp: i64,
) -> *mut c_void {
    match view_of(w) {
        Some(v) => {
            let l = cslice(left, left_len);
            let r = cslice(right, right_len);
            mark_to_ptr(v.insert_before(ptr_to_mark(anchor), compat_message(&l, &r, stamp)))
        }
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_remove(w: CGtkWidget, mark: *mut c_void) -> c_int {
    match (view_of(w), ptr_to_mark(mark)) {
        (Some(v), Some(id)) => c_int::from(v.remove(id)),
        _ => 0,
    }
}

// ---- inline media (C4) ---------------------------------------------

/// # Safety
/// `alt` is a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_append_media(
    w: CGtkWidget,
    texture: *mut c_void,
    alt: *const c_char,
    token: u32,
    stamp: i64,
) {
    with_view!(w, v, {
        // Append the row *first*.
        //
        // `set_media_frames` attaches the decoded size by looking the
        // row up with `find_image(token)`, so installing frames before
        // the row exists finds nothing, leaves `Block::Image.size` at
        // None, and the snapshot path — which requires `Some` — renders
        // the placeholder forever. Usually the texture arrives later via
        // set_texture and the order is moot; it is the pre-decoded case
        // that breaks.
        let alt = cstr(alt);
        v.append(Message {
            kind: MessageKind::Live,
            timestamp: stamp_or_now(stamp),
            speaker: None,
            gutter: None,
            blocks: vec![Block::Image {
                token,
                size: None,
                alt,
            }],
            flags: hxchat_layout::MessageFlags::NONE,
        });
        if !texture.is_null() {
            let tex: gtk4::gdk::Texture = gtk4::glib::translate::from_glib_none(
                texture as *mut gtk4::gdk::ffi::GdkTexture,
            );
            v.set_media_frames(token, vec![(tex, 0)]);
        }
    })
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_media_mark(w: CGtkWidget, token: u32) -> *mut c_void {
    match view_of(w) {
        Some(v) => v.find_image(token).map_or(std::ptr::null_mut(), mark_to_ptr),
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_media_set_texture(
    w: CGtkWidget,
    mark: *mut c_void,
    texture: *mut c_void,
) {
    let Some(v) = view_of(w) else { return };
    let Some(token) = token_for_mark(&v, mark) else {
        return;
    };
    if texture.is_null() {
        // NULL reverts the row to its placeholder — the decode-failed
        // path, which xtext also supports.
        v.set_media_frames(token, Vec::new());
        return;
    }
    let tex: gtk4::gdk::Texture =
        gtk4::glib::translate::from_glib_none(texture as *mut gtk4::gdk::ffi::GdkTexture);
    v.set_media_frames(token, vec![(tex, 0)]);
}

/// One animation frame, matching `HxInlineMediaFrame`
/// (src/inline_media_decode.h): a strong `GdkTexture *` owned by the
/// GArray, and the milliseconds to show it for.
#[repr(C)]
struct HxInlineMediaFrame {
    texture: *mut gtk4::gdk::ffi::GdkTexture,
    delay_ms: u32,
}

/// The token carried by the image block a mark names.
unsafe fn token_for_mark(v: &HxChatView, mark: *mut c_void) -> Option<u32> {
    let id = ptr_to_mark(mark)?;
    v.image_token_of(id)
}

/// # Safety
/// `w` is a valid `HxChatView *`.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_media_set_animation(
    w: CGtkWidget,
    mark: *mut c_void,
    frames: *mut c_void,
) {
    let Some(v) = view_of(w) else { return };
    let Some(token) = token_for_mark(&v, mark) else {
        return;
    };
    if frames.is_null() {
        v.set_media_frames(token, Vec::new());
        return;
    }
    let arr = frames as *mut gtk4::glib::ffi::GArray;
    let len = (*arr).len as usize;
    let data = (*arr).data as *const HxInlineMediaFrame;
    let mut out = Vec::with_capacity(len);
    for i in 0..len {
        let f = &*data.add(i);
        if f.texture.is_null() {
            continue;
        }
        // from_glib_none: the GArray owns these refs and drops them in
        // its clear_func, so we take our own rather than stealing them.
        let tex: gtk4::gdk::Texture = gtk4::glib::translate::from_glib_none(f.texture);
        out.push((tex, f.delay_ms));
    }
    v.set_media_frames(token, out);
}

// ---- in-buffer search ---------------------------------------------

/// Write the `(total, ordinal)` readout back through the out-params,
/// either of which may be NULL.
unsafe fn put_readout(n_matches: *mut u32, current: *mut u32, r: (usize, usize)) {
    if !n_matches.is_null() {
        *n_matches = r.0 as u32;
    }
    if !current.is_null() {
        *current = r.1 as u32;
    }
}

/// # Safety
/// `needle` is a NUL-terminated string or NULL; the out-params are
/// writable `guint`s or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_search(
    w: CGtkWidget,
    needle: *const c_char,
    case_sensitive: c_int,
    n_matches: *mut u32,
    current: *mut u32,
) {
    let needle = cstr(needle);
    let r = with_view!(w, v, v.search_set(&needle, case_sensitive != 0));
    put_readout(n_matches, current, r);
}

/// # Safety
/// The out-params are writable `guint`s or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_search_step(
    w: CGtkWidget,
    dir: c_int,
    n_matches: *mut u32,
    current: *mut u32,
) {
    let r = with_view!(w, v, v.search_step(dir));
    put_readout(n_matches, current, r);
}

/// # Safety
/// `w` is an `HxChatView` or NULL.
#[no_mangle]
pub unsafe extern "C" fn hx_chat_view_search_clear(w: CGtkWidget) {
    with_view!(w, v, v.search_clear());
}

/// Unused today; keeps `ParsedText` reachable for the C4 work without a
/// dead-import warning in the meantime.
#[allow(dead_code)]
fn _keep(_: ParsedText) {}
