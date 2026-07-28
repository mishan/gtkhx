//! `HxChatView` — the GTK4 chat output widget.
//!
//! The tree's first Rust custom-drawn widget: nothing else in
//! `rust/crates/` implements `WidgetImpl::snapshot` / `measure` /
//! `size_allocate` or `ScrollableImpl`. Everything interesting is in
//! `hxchat-layout`; this is the skin.
//!
//! Two departures from xtext are visible right here:
//!
//! - **Native GSK nodes, not cairo.** xtext draws through
//!   `gtk_snapshot_append_cairo()` — a correct Phase 4 decision that
//!   preserved the Phase 3.4b cairo work, but it hands GSK one opaque
//!   texture per frame. `append_layout` / `append_color` hand it real
//!   render nodes it can batch and the GPU can composite.
//! - **The adjustment is in pixels.** xtext's `page_size` is
//!   `height / fontsize` and its `value` is a fractional line number
//!   (xtext.c:919), which is only coherent when every row is the same
//!   height. Here `value`, `upper` and `page_size` are all pixels.
//!
//! C2 scope is text only. Media rows render as their placeholder text
//! (which is exactly what Phase 9.D shipped and what the PM windows —
//! the first surface this is wired to — never produce anyway); real
//! inline images and the chat-history row kinds land in C4.

use gtk4::glib;
use gtk4::prelude::*;
use gtk4::subclass::prelude::*;
use hxchat_layout::{
    Caret, ChatBuffer, ColorRef, LayoutParams, LineSource, Message, MessageId, ParsedText,
    RowSelection, Selection, Span, Style, TextMeasure, MIN_INDENT,
};

use crate::measure::PangoMeasure;

/// Palette size, matching `chat_view.h`'s `HX_CHAT_PAL_COLS`.
pub const PALETTE_COLS: usize = 38;
pub const PAL_FG: usize = 34;
pub const PAL_BG: usize = 35;
/// `HX_CHAT_PAL_HISTORY_MUTED` — the theme's secondary text colour. The
/// timestamp column uses it so the stamps recede behind the message
/// text rather than competing with it.
pub const PAL_HISTORY_MUTED: usize = 37;
/// `HX_CHAT_PAL_MARK_FG` / `_MARK_BG` — the selection colours, filled by
/// the theme exactly as they were for xtext.
pub const PAL_MARK_FG: usize = 32;
pub const PAL_MARK_BG: usize = 33;

/// Pixels of slop within which a scroll position counts as "at the
/// bottom" and resumes following.
const FOLLOW_SLOP: u32 = 8;

enum ScrollKey {
    /// Viewport-sized step; -1 up, 1 down.
    Page(i32),
    Home,
    End,
}

/// Does the focused widget edit text?
///
/// Anything that does has no meaningful use for a page key (the inputs
/// here are a few lines tall at most), so the chat log may claim it.
/// Anything that doesn't — notably the user list's GtkColumnView — keeps
/// its own paging.
fn focus_is_text_entry(c: &gtk4::EventControllerKey) -> bool {
    use gtk4::prelude::*;
    let Some(root) = c.widget().and_then(|w| w.root()) else {
        return false;
    };
    let Some(focus) = root.focus() else {
        return false;
    };
    focus.is::<gtk4::TextView>() || focus.is::<gtk4::Text>() || focus.is::<gtk4::Entry>()
}

/// Floor for a decoded animation's per-frame delay, in ms.
///
/// Matches xtext, which clamps to 10 at both arm sites (xtext.c:6309 and
/// :6365). A higher floor would visibly slow fast GIFs relative to the
/// backend this has to be indistinguishable from during the A/B.
const MIN_FRAME_DELAY_MS: u32 = 10;

#[cfg(not(test))]
extern "C" {
    /// `hx_chat_avatar_for_uid` — see `src/chat_avatar.h`.
    ///
    /// Borrowed, and only until the next call: animated avatars advance
    /// on a shared frame timer, so this is asked per draw rather than
    /// cached. The expensive half (decoding a cicn sprite) is cached on
    /// the C side, keyed by icon id.
    fn hx_chat_avatar_for_uid(
        anchor: *mut gtk4::ffi::GtkWidget,
        uid: u16,
    ) -> *mut gtk4::gdk::ffi::GdkTexture;
}

/// The test binary links no GtkHx C, so stub the resolver — same
/// arrangement `links.rs` uses for `gtkurl_*`. Always "no icon", which
/// is the only answer a headless test could check anyway.
#[cfg(test)]
unsafe fn hx_chat_avatar_for_uid(
    _anchor: *mut gtk4::ffi::GtkWidget,
    _uid: u16,
) -> *mut gtk4::gdk::ffi::GdkTexture {
    std::ptr::null_mut()
}

/// Something under the pointer that responds to being clicked.
///
/// One type for both cases on purpose. Links and nicks want the same
/// affordance — underline on hover, a pointer cursor, a menu on
/// right-click — and modelling them separately is how the two end up
/// underlining under subtly different conditions.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) enum HoverTarget {
    /// A URL: the row, the source it lives in, and its byte range.
    Link {
        message: MessageId,
        source: LineSource,
        range: std::ops::Range<usize>,
    },
    /// A speaker's nick in the gutter. Carries the uid so the
    /// right-click handler doesn't have to re-resolve it.
    Nick { message: MessageId, uid: u16 },
}

impl HoverTarget {
    /// The (message, source, range) this target underlines, if any.
    fn underline(&self) -> Option<(MessageId, LineSource, std::ops::Range<usize>)> {
        match self {
            HoverTarget::Link {
                message,
                source,
                range,
            } => Some((*message, *source, range.clone())),
            // The whole gutter underlines, which the draw path handles
            // by range rather than specially — see hover_range_for.
            HoverTarget::Nick { .. } => None,
        }
    }
}

/// What, if anything, recolours a run of text.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Mark {
    None,
    Match,
    CurrentMatch,
    Selection,
}

impl Mark {
    /// Precedence when bands overlap. Higher wins.
    fn rank(self) -> u8 {
        match self {
            Mark::None => 0,
            Mark::Match => 1,
            Mark::CurrentMatch => 2,
            Mark::Selection => 3,
        }
    }
}

/// Search highlight colours.
///
/// These are the *same* colours the news panel's find bar already uses
/// (`news.rs`, tags `search-match` / `search-current`): Adwaita yellow-4
/// `#f6d32d` on black for every hit, orange `#ff7800` on white for the
/// active one. Two find bars in one app that highlight differently is a
/// papercut, and this way there is one place to change it.
///
/// Fixed rather than themed, deliberately: the palette is a contract
/// with `chat_view.h` (38 slots, mirrored in the theme file format), so
/// widening it for this would mean a schema change every theme has to
/// answer.
const SEARCH_MATCH_BG: gtk4::gdk::RGBA =
    gtk4::gdk::RGBA::new(0.9647, 0.8275, 0.1765, 1.0);
const SEARCH_MATCH_FG: gtk4::gdk::RGBA = gtk4::gdk::RGBA::new(0.0, 0.0, 0.0, 1.0);
const SEARCH_CURRENT_BG: gtk4::gdk::RGBA =
    gtk4::gdk::RGBA::new(1.0, 0.4706, 0.0, 1.0);
const SEARCH_CURRENT_FG: gtk4::gdk::RGBA = gtk4::gdk::RGBA::new(1.0, 1.0, 1.0, 1.0);

/// How long the zoom badge stays fully opaque, then how long it fades.
const ZOOM_BADGE_HOLD_US: i64 = 700_000;
const ZOOM_BADGE_FADE_US: i64 = 400_000;

/// Text lines per wheel notch.
const WHEEL_LINES: f64 = 3.0;

/// Backing for `code` spans and code blocks.
///
/// The monospace attribute alone is invisible here: GtkHx's chat font is
/// *already* monospace by default, so `` `code` `` rendered identically
/// to code with the backticks quietly removed — strictly worse than not
/// parsing it. The tint is what actually says "this is code".
///
/// Derived from the theme foreground at low alpha rather than a fixed
/// grey, so it reads on light and dark without a second colour to keep
/// in step.
const CODE_BG_ALPHA: f32 = 0.10;
const CODE_BORDER_ALPHA: f32 = 0.22;

/// Padding around a fenced block's box, in px.
const CODE_BOX_PAD: f32 = 4.0;

/// Grab tolerance either side of the separator rule, in px.
const SEPARATOR_GRAB: f64 = 4.0;


/// Inset between the widget edge and the text.
///
/// xtext draws hard against its allocation, which reads as cramped now
/// that the view sits directly in a pane rather than inside a frame.
/// Applied by shrinking the content box, not by translating the drawing,
/// so wrapping, scroll extent and (later) hit-testing all agree about
/// where the content actually is.
const PAD_X: i32 = 4;
const PAD_Y: i32 = 2;

/// xtext's built-in timestamp format, which chat.c depends on as the
/// default (`gtk_xtext_set_stamp_format(NULL)` restores it).
const DEFAULT_STAMP_FORMAT: &str = "[%H:%M:%S] ";

/// One decoded media item. Internal widget state.
#[derive(Clone)]
pub(crate) struct MediaEntry {
    /// Animation frames with their durations. A static image is a
    /// single frame with delay 0.
    pub(crate) frames: Vec<(gtk4::gdk::Texture, u32)>,
    /// Index of the frame currently showing.
    pub(crate) current: usize,
    /// When the current frame started, for the advance tick.
    pub(crate) since_us: i64,
}

impl MediaEntry {
    pub(crate) fn texture(&self) -> Option<&gtk4::gdk::Texture> {
        self.frames.get(self.current).map(|(t, _)| t)
    }

    pub(crate) fn is_animated(&self) -> bool {
        self.frames.len() > 1
    }

    /// Intrinsic size, from the first frame — every frame of a glycin
    /// animation shares dimensions.
    pub(crate) fn size(&self) -> Option<hxchat_layout::ImageSize> {
        self.frames.first().map(|(t, _)| hxchat_layout::ImageSize {
            width: t.width().max(0) as u32,
            height: t.height().max(0) as u32,
        })
    }
}

impl std::fmt::Debug for MediaEntry {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("MediaEntry")
            .field("frames", &self.frames.len())
            .field("current", &self.current)
            .finish()
    }
}

mod imp {
    use super::*;
    use std::cell::{Cell, RefCell};

    pub struct HxChatView {
        pub buffer: RefCell<ChatBuffer>,
        pub measure: RefCell<PangoMeasure>,
        pub palette: RefCell<[gtk4::gdk::RGBA; PALETTE_COLS]>,

        pub hadjustment: RefCell<Option<gtk4::Adjustment>>,
        pub vadjustment: RefCell<Option<gtk4::Adjustment>>,
        pub hscroll_policy: Cell<gtk4::ScrollablePolicy>,
        pub vscroll_policy: Cell<gtk4::ScrollablePolicy>,
        pub vadj_handler: RefCell<Option<glib::SignalHandlerId>>,
        /// Set while we are writing the adjustment ourselves, so the
        /// value-changed handler doesn't treat our own write as a user
        /// scroll and clobber the anchor.
        pub updating_adj: Cell<bool>,

        pub font_generation: Cell<u32>,
        pub separator: Cell<bool>,
        /// Set while a drag is moving the gutter separator rather than
        /// selecting text.
        pub moving_separator: Cell<bool>,
        /// In-buffer search: the query, its hits, and the cursor.
        pub search: RefCell<hxchat_layout::SearchState>,
        /// Monotonic time (µs) at which the zoom badge stops showing,
        /// or 0 when it isn't showing. See `flash_zoom_badge`.
        pub zoom_badge_until: Cell<i64>,
        /// Tick driving the badge's fade, so it isn't left on screen.
        pub zoom_badge_tick: RefCell<Option<gtk4::TickCallbackId>>,
        /// What the pointer is currently over, if it is something
        /// activatable. Drives the cursor *and* the hover underline, so
        /// the two cannot disagree about what is hoverable.
        pub(crate) hovered: RefCell<Option<HoverTarget>>,
        /// Whether the timestamp column renders. Driven by CFG_TIMESTAMP
        /// through `hx_chat_view_set_time_stamp`.
        pub time_stamp: Cell<bool>,
        /// The live selection, or None when nothing is selected.
        pub selection: RefCell<Option<Selection>>,
        /// True while a drag is in progress, so motion extends the
        /// selection rather than being ignored.
        pub selecting: Cell<bool>,
        /// The capture-phase Ctrl+C controller installed on our root,
        /// plus a weak ref to that root so it can be removed when the
        /// view moves to a different window.
        pub root_key_handler:
            RefCell<Option<(glib::object::WeakRef<gtk4::Widget>, gtk4::EventController)>>,
        /// strftime format for that column. xtext's default is
        /// "[%H:%M:%S] " and chat.c relies on getting it.
        pub stamp_format: RefCell<String>,
        /// Decoded media, keyed by the per-conversation token.
        ///
        /// The textures live here rather than on the layout's
        /// `Block::Image` because `hxchat-layout` is GTK-free by
        /// design — it carries only the *size*, which is all it needs
        /// to lay the row out. The token is the join.
        pub(crate) media: RefCell<std::collections::HashMap<u32, MediaEntry>>,
        /// Frame-advance tick, running only while something animates.
        pub anim_tick: RefCell<Option<gtk4::TickCallbackId>>,
        /// Last pointer position seen during a drag, widget-relative.
        ///
        /// CLAUDE.md records the xtext version of this as a known
        /// degradation: its scroll timers read `xtext->select_end_y`
        /// rather than the live device position, because GTK 4 has no
        /// synchronous "where is the pointer" accessor. Storing it from
        /// the drag handler and consuming it from a per-frame tick is
        /// the actual answer — the staleness window becomes one frame
        /// instead of one timer period.
        pub drag_pointer: Cell<(f64, f64)>,
        /// Caret the current press landed on, installed as a selection
        /// only once the pointer actually moves. See `install_selection_gestures`.
        pub drag_start: RefCell<Option<Caret>>,
        /// Whether the current press has turned into a real drag.
        pub drag_moved: Cell<bool>,
        /// Auto-scroll tick, running only while a drag is outside the
        /// viewport.
        pub autoscroll_tick: RefCell<Option<gtk4::TickCallbackId>>,
    }

    impl Default for HxChatView {
        fn default() -> Self {
            HxChatView {
                buffer: RefCell::new(ChatBuffer::new(LayoutParams::default())),
                measure: RefCell::new(PangoMeasure::headless("Monospace 10")),
                palette: RefCell::new([gtk4::gdk::RGBA::BLACK; PALETTE_COLS]),
                hadjustment: RefCell::new(None),
                vadjustment: RefCell::new(None),
                hscroll_policy: Cell::new(gtk4::ScrollablePolicy::Minimum),
                vscroll_policy: Cell::new(gtk4::ScrollablePolicy::Minimum),
                vadj_handler: RefCell::new(None),
                updating_adj: Cell::new(false),
                font_generation: Cell::new(0),
                separator: Cell::new(false),
                moving_separator: Cell::new(false),
                search: RefCell::new(hxchat_layout::SearchState::new()),
                hovered: RefCell::new(None),
                zoom_badge_until: Cell::new(0),
                zoom_badge_tick: RefCell::new(None),
                time_stamp: Cell::new(false),
                selection: RefCell::new(None),
                selecting: Cell::new(false),
                root_key_handler: RefCell::new(None),
                stamp_format: RefCell::new(DEFAULT_STAMP_FORMAT.to_string()),
                media: RefCell::new(std::collections::HashMap::new()),
                anim_tick: RefCell::new(None),
                drag_pointer: Cell::new((0.0, 0.0)),
                drag_start: RefCell::new(None),
                drag_moved: Cell::new(false),
                autoscroll_tick: RefCell::new(None),
            }
        }
    }

    #[glib::object_subclass]
    impl ObjectSubclass for HxChatView {
        const NAME: &'static str = "HxChatView";
        type Type = super::HxChatView;
        type ParentType = gtk4::Widget;
        type Interfaces = (gtk4::Scrollable,);
    }

    impl ObjectImpl for HxChatView {
        fn signals() -> &'static [glib::subclass::Signal] {
            use std::sync::OnceLock;
            static S: OnceLock<Vec<glib::subclass::Signal>> = OnceLock::new();
            S.get_or_init(|| {
                vec![
                    // xtext's signal, and now genuinely emitted.
                    //
                    // Parity beat purity here. The three C handlers
                    // chat.c and msg.c connect — gtkurl, chat-history's
                    // load-older sentinel, and inline media's
                    // `hxmedia:N` — all recognise their targets by
                    // matching the clicked *word* as a string. Emitting
                    // the same signal with the same tokenisation makes
                    // all three work against the new backend with zero
                    // C changes, which is exactly what the A/B needs.
                    // The typed replacements in scoping §3.6 are still
                    // the destination, but they belong with the
                    // structured append in C6, not ahead of parity.
                    //
                    // **"word-click", not "word_click".** glib-rs's
                    // Signal::builder requires a canonical name and
                    // *panics* otherwise — and a panic here is an abort,
                    // since it unwinds out of `class_init` across the
                    // FFI. The C callers keep their underscore spelling
                    // and still resolve: GLib canonicalises `_` to `-`
                    // on both registration and lookup, so
                    // `g_signal_lookup("word_click")` and
                    // `g_signal_lookup("word-click")` return the same id.
                    // (Verified against GLib directly, not assumed —
                    // it's the same equivalence that lets everyone write
                    // "size_allocate" for "size-allocate".)
                    //
                    // Registered as (POINTER, POINTER) to match xtext,
                    // which registers both args as G_TYPE_POINTER —
                    // the marshaller never inspected the concrete
                    // GdkEvent shape.
                    glib::subclass::Signal::builder("word-click")
                        .param_types([glib::Pointer::static_type(), glib::Pointer::static_type()])
                        .build(),
                    // speaker-menu: (uid, x, y) — right-click on a nick.
                    //
                    // The view raises it and stops; chat.c answers by
                    // calling users.c::user_popup_show, so chat and the
                    // user list pop the *same* menu rather than two that
                    // have to be kept in step.
                    glib::subclass::Signal::builder("speaker-menu")
                        .param_types([u32::static_type(), f64::static_type(), f64::static_type()])
                        .build(),
                ]
            })
        }

        fn constructed(&self) {
            self.parent_constructed();
            let obj = self.obj();
            obj.set_hexpand(true);
            obj.set_vexpand(true);
            // Rows are laid out against the allocation but a single
            // over-wide grapheme can exceed it (see TextMeasure::
            // fit_prefix's minimum-progress rule), and a partially
            // scrolled row is drawn straddling the top edge by design.
            // Clip rather than letting either bleed into the sibling
            // widgets.
            obj.set_overflow(gtk4::Overflow::Hidden);
            // Rebuild the measurer against the widget's own Pango
            // context so text is shaped with the real display's font
            // config, not the headless default the struct starts with.
            let ctx = obj.pango_context();
            let font = pango::FontDescription::from_string("Monospace 10");
            *self.measure.borrow_mut() = PangoMeasure::new(ctx, font);

            // Adopt the persisted stamp format. prefs_read applies it
            // before any window exists, so without this a view would
            // keep the built-in default and ignore the user's pref
            // until they happened to change it again.
            if let Some(f) = prefs::STAMP_FORMAT.with(|f| f.borrow().clone()) {
                *self.stamp_format.borrow_mut() = f;
            }

            obj.install_selection_gestures();
            obj.install_zoom_bindings();
            obj.install_link_handlers();
        }

        fn properties() -> &'static [glib::ParamSpec] {
            use std::sync::OnceLock;
            static P: OnceLock<Vec<glib::ParamSpec>> = OnceLock::new();
            P.get_or_init(|| {
                // **Override, don't redeclare.**
                //
                // These four properties belong to the GtkScrollable
                // interface. An implementor overrides them; it does not
                // define new ones. Declaring fresh ParamSpecs with the
                // same names — which is what the first cut did — collides
                // with the interface's, `g_object_class_install_property`
                // refuses them, and the class is left half-built.
                //
                // The symptom was not a warning about properties. It was
                // `g_object_new` handing back something that failed
                // `GTK_IS_WIDGET`, so the very first C call on it
                // (`gtk_widget_set_can_focus`) asserted, `is_hxchat`
                // returned false on it, and the next call was dispatched
                // into xtext with a non-xtext pointer — segfaulting in
                // `gtk_xtext_set_time_stamp` with a garbage buffer. Three
                // symptoms, one cause, none of them pointing here.
                vec![
                    glib::ParamSpecOverride::for_interface::<gtk4::Scrollable>("hadjustment"),
                    glib::ParamSpecOverride::for_interface::<gtk4::Scrollable>("vadjustment"),
                    glib::ParamSpecOverride::for_interface::<gtk4::Scrollable>("hscroll-policy"),
                    glib::ParamSpecOverride::for_interface::<gtk4::Scrollable>("vscroll-policy"),
                ]
            })
        }

        fn set_property(&self, _id: usize, value: &glib::Value, pspec: &glib::ParamSpec) {
            match pspec.name() {
                "hadjustment" => {
                    *self.hadjustment.borrow_mut() = value.get().ok().flatten();
                }
                "vadjustment" => {
                    self.obj().set_vadjustment_internal(value.get().ok().flatten());
                }
                "hscroll-policy" => {
                    if let Ok(v) = value.get() {
                        self.hscroll_policy.set(v);
                    }
                }
                "vscroll-policy" => {
                    if let Ok(v) = value.get() {
                        self.vscroll_policy.set(v);
                    }
                }
                _ => {}
            }
        }

        fn property(&self, _id: usize, pspec: &glib::ParamSpec) -> glib::Value {
            match pspec.name() {
                "hadjustment" => self.hadjustment.borrow().to_value(),
                "vadjustment" => self.vadjustment.borrow().to_value(),
                "hscroll-policy" => self.hscroll_policy.get().to_value(),
                "vscroll-policy" => self.vscroll_policy.get().to_value(),
                _ => glib::Value::from_type(glib::Type::UNIT),
            }
        }
    }

    impl WidgetImpl for HxChatView {
        fn measure(
            &self,
            orientation: gtk4::Orientation,
            _for_size: i32,
        ) -> (i32, i32, i32, i32) {
            // A scrollable's natural size must not depend on its
            // content, or the scrolled window grows to fit the whole
            // scrollback and the scrollbar never appears.
            let m = self.measure.borrow().metrics();
            match orientation {
                gtk4::Orientation::Horizontal => (0, 0, -1, -1),
                _ => (m.line_height as i32, m.line_height as i32, -1, -1),
            }
        }

        fn size_allocate(&self, width: i32, height: i32, _baseline: i32) {
            let obj = self.obj();
            {
                let mut buf = self.buffer.borrow_mut();
                // The content box is the allocation minus the padding, so
                // wrapping, scroll extent and hit-testing all measure
                // against the same rectangle the text is drawn in.
                buf.set_width(content_width(width));
            }
            obj.sync_adjustment(content_height(height));
        }

        fn snapshot(&self, snapshot: &gtk4::Snapshot) {
            let obj = self.obj();
            obj.snapshot_content(snapshot);
            // Chrome, drawn over the content and outside its clip.
            obj.snapshot_zoom_badge(snapshot, obj.width(), obj.height());
        }
    }

    impl ScrollableImpl for HxChatView {}
}

glib::wrapper! {
    pub struct HxChatView(ObjectSubclass<imp::HxChatView>)
        @extends gtk4::Widget,
        @implements gtk4::Accessible, gtk4::Buildable, gtk4::ConstraintTarget,
                    gtk4::Scrollable;
}

impl Default for HxChatView {
    fn default() -> Self {
        Self::new()
    }
}

impl HxChatView {
    pub fn new() -> HxChatView {
        glib::Object::new()
    }

    fn imp_(&self) -> &imp::HxChatView {
        imp::HxChatView::from_obj(self)
    }

    /// The private struct, for tests that need to reach the buffer.
    #[cfg(test)]
    pub(crate) fn imp_ref(&self) -> &imp::HxChatView {
        self.imp_()
    }

    // ---- configuration ------------------------------------------------

    pub fn set_font_from_string(&self, font: &str) {
        let imp = self.imp_();
        imp.measure
            .borrow_mut()
            .set_font(pango::FontDescription::from_string(font));
        let g = imp.font_generation.get().wrapping_add(1);
        imp.font_generation.set(g);
        imp.buffer.borrow_mut().set_font_generation(g);
        // The stamp column is measured in the old font otherwise.
        self.recompute_stamp_width();
        self.queue_resize();
    }

    pub fn set_palette(&self, palette: &[gtk4::gdk::RGBA; PALETTE_COLS]) {
        *self.imp_().palette.borrow_mut() = *palette;
        self.queue_draw();
    }

    pub fn set_word_wrap(&self, on: bool) {
        self.imp_().buffer.borrow_mut().set_word_wrap(on);
        self.queue_draw();
    }

    pub fn set_max_rows(&self, n: i32) {
        // xtext only auto-trims when max_lines > 2 (xtext.c:5442), so 1
        // and 2 mean "no limit" there. Treating them as a literal cap
        // here would truncate the scrollback to nearly nothing on a pref
        // value that is a no-op on the other backend — exactly the kind
        // of silent divergence the A/B is supposed to rule out.
        let cap = if n > 2 { n as usize } else { 0 };
        let m = self.imp_().measure.borrow();
        self.imp_().buffer.borrow_mut().set_max_rows(cap, &*m);
    }

    /// Edge length of the avatar slot in the gutter; 0 hides avatars.
    pub fn set_avatar_size(&self, px: u32) {
        let m = self.imp_().measure.borrow();
        let mut buf = self.imp_().buffer.borrow_mut();
        buf.set_avatar_size(px);
        drop(buf);
        drop(m);
        self.queue_resize();
        self.queue_draw();
    }

    /// Gap that breaks a run of messages from one speaker; 0 disables
    /// grouping. Re-decides the rows already in the buffer, since the
    /// flag describes neighbours rather than messages.
    pub fn set_group_gap_secs(&self, secs: i64) {
        let m = self.imp_().measure.borrow();
        self.imp_().buffer.borrow_mut().set_group_gap_secs(secs, &*m);
        drop(m);
        self.queue_resize();
        self.queue_draw();
    }

    pub fn set_indent(&self, on: bool) {
        self.imp_().buffer.borrow_mut().set_indent(on);
        self.queue_draw();
    }

    pub fn set_max_indent(&self, px: i32) {
        self.imp_()
            .buffer
            .borrow_mut()
            .set_max_indent(px.max(0) as u32);
        self.queue_draw();
    }

    /// Zoom, per-mille. See docs/chat-view-scoping.md §3.7.
    pub fn set_zoom_permille(&self, zoom: u32) {
        let imp = self.imp_();
        let was = imp.measure.borrow().zoom_permille();
        imp.measure.borrow_mut().set_zoom_permille(zoom);
        imp.buffer.borrow_mut().set_zoom_permille(zoom);
        self.recompute_stamp_width();
        self.queue_resize();
        if zoom != was {
            self.flash_zoom_badge();
        }
    }

    /// Show the zoom percentage for a moment.
    ///
    /// Zoom is otherwise silent: text gets bigger or smaller and there is
    /// nothing that says by how much, or how to get back to 100%. The
    /// badge is drawn inside the widget's own snapshot rather than as an
    /// overlay widget, because the chat view is packed as a bare child
    /// next to a scrollbar — adding a GtkOverlay would mean restructuring
    /// every container that holds one, for a label that shows for a
    /// second.
    fn flash_zoom_badge(&self) {
        let imp = self.imp_();
        imp.zoom_badge_until
            .set(glib::monotonic_time() + ZOOM_BADGE_HOLD_US + ZOOM_BADGE_FADE_US);
        self.queue_draw();

        if imp.zoom_badge_tick.borrow().is_some() {
            return; // already animating; the new deadline extends it
        }
        let id = self.add_tick_callback(|view, _clock| {
            let imp = view.imp_();
            if glib::monotonic_time() >= imp.zoom_badge_until.get() {
                imp.zoom_badge_until.set(0);
                *imp.zoom_badge_tick.borrow_mut() = None;
                view.queue_draw();
                return glib::ControlFlow::Break;
            }
            // Redraw so the fade advances. Same self-clearing discipline
            // as the autoscroll tick: whatever ends the callback also
            // clears the handle that names it, or the next flash sees a
            // live tick that isn't.
            view.queue_draw();
            glib::ControlFlow::Continue
        });
        *imp.zoom_badge_tick.borrow_mut() = Some(id);
    }

    /// Draw the zoom badge, if it is showing. Called last in `snapshot`
    /// so it sits above the text.
    fn snapshot_zoom_badge(&self, snapshot: &gtk4::Snapshot, alloc_w: i32, alloc_h: i32) {
        let imp = self.imp_();
        let until = imp.zoom_badge_until.get();
        if until == 0 {
            return;
        }
        let remaining = until - glib::monotonic_time();
        if remaining <= 0 {
            return;
        }
        // Full opacity while held, then a linear fade.
        let alpha = (remaining as f64 / ZOOM_BADGE_FADE_US as f64).clamp(0.0, 1.0) as f32;

        let pct = (self.zoom_permille() + 5) / 10;
        let layout = self.create_pango_layout(Some(&format!("{pct}%")));
        let (tw, th) = layout.pixel_size();

        let pad = 8.0;
        let w = tw as f32 + pad * 2.0;
        let h = th as f32 + pad;
        // Bottom-right, clear of the scrollbar side's text and of the
        // newest message, which is what the eye is on while zooming.
        let x = (alloc_w as f32 - w - 12.0).max(0.0);
        let y = (alloc_h as f32 - h - 12.0).max(0.0);

        // Inverted theme colours, so the badge contrasts on light and
        // dark without a third colour to keep in step.
        let mut bg = imp.palette.borrow()[PAL_FG];
        let mut fg = imp.palette.borrow()[PAL_BG];
        bg = gtk4::gdk::RGBA::new(bg.red(), bg.green(), bg.blue(), 0.85 * alpha);
        fg = gtk4::gdk::RGBA::new(fg.red(), fg.green(), fg.blue(), alpha);

        let rect = gtk4::graphene::Rect::new(x, y, w, h);
        let rounded = gtk4::gsk::RoundedRect::from_rect(rect, h / 2.0);
        snapshot.push_rounded_clip(&rounded);
        snapshot.append_color(&bg, &rect);
        snapshot.pop();

        snapshot.save();
        snapshot.translate(&gtk4::graphene::Point::new(x + pad, y + pad / 2.0));
        snapshot.append_layout(&layout, &fg);
        snapshot.restore();
    }

    pub fn zoom_permille(&self) -> u32 {
        self.imp_().measure.borrow().zoom_permille()
    }

    /// Toggle the timestamp column.
    ///
    /// Recomputes the width the gutter must reserve, since the stamp and
    /// the nick share that band — reserving only the nick width is what
    /// makes them overlap.
    pub fn set_time_stamp(&self, on: bool) {
        let imp = self.imp_();
        if imp.time_stamp.get() == on {
            return;
        }
        imp.time_stamp.set(on);
        self.recompute_stamp_width();
        // Relayout, not just redraw: the gutter width changed, so
        // wrapping, row heights and the scroll extent all move with it.
        self.queue_resize();
    }

    pub fn set_stamp_format(&self, format: &str) {
        let imp = self.imp_();
        let f = if format.is_empty() {
            DEFAULT_STAMP_FORMAT.to_string()
        } else {
            format.to_string()
        };
        if *imp.stamp_format.borrow() == f {
            return;
        }
        *imp.stamp_format.borrow_mut() = f;
        self.recompute_stamp_width();
        self.queue_resize();
    }

    /// Measure the widest plausible rendering of the current format.
    ///
    /// The stamp is fixed-width in practice but the format is arbitrary,
    /// so measure a real formatted value rather than guessing. A moment
    /// with a two-digit hour keeps `%-I`-style formats honest.
    fn recompute_stamp_width(&self) {
        let imp = self.imp_();
        let px = if imp.time_stamp.get() {
            let fmt = imp.stamp_format.borrow().clone();
            // 2001-09-09 01:46:40 UTC — every field two digits wide.
            let sample = format_stamp(1_000_000_000, &fmt).unwrap_or_default();
            if sample.is_empty() {
                0
            } else {
                imp.measure.borrow().run_width(&sample, Style::default())
            }
        } else {
            0
        };
        imp.buffer.borrow_mut().set_stamp_width(px);
    }

    pub fn set_separator(&self, on: bool) {
        self.imp_().separator.set(on);
        self.queue_draw();
    }

    // ---- content ------------------------------------------------------

    pub fn append(&self, msg: Message) -> MessageId {
        let imp = self.imp_();
        let id = {
            let m = imp.measure.borrow();
            imp.buffer.borrow_mut().append(msg, &*m)
        };
        self.after_content_change();
        id
    }

    pub fn insert_before(&self, anchor: Option<MessageId>, msg: Message) -> MessageId {
        let imp = self.imp_();
        let id = {
            let m = imp.measure.borrow();
            imp.buffer.borrow_mut().insert_before(anchor, msg, &*m)
        };
        self.after_content_change();
        id
    }

    pub fn remove(&self, id: MessageId) -> bool {
        let ok = self.imp_().buffer.borrow_mut().remove(id);
        if ok {
            self.after_content_change();
        }
        ok
    }

    pub fn clear(&self) {
        self.imp_().buffer.borrow_mut().clear();
        // Textures are keyed by token, and tokens are per-conversation
        // and reused after a clear — holding stale ones would both leak
        // and let a new row show an old image.
        self.clear_media();
        self.after_content_change();
    }

    pub fn scroll_to_bottom(&self) {
        self.imp_().buffer.borrow_mut().scroll_to_bottom();
        self.after_content_change();
    }

    fn after_content_change(&self) {
        self.sync_adjustment(content_height(self.height()));
        self.queue_draw();
    }

    // ---- scrolling ----------------------------------------------------

    fn set_vadjustment_internal(&self, adj: Option<gtk4::Adjustment>) {
        let imp = self.imp_();
        if let (Some(old), Some(id)) = (
            imp.vadjustment.borrow().as_ref(),
            imp.vadj_handler.borrow_mut().take(),
        ) {
            old.disconnect(id);
        }
        if let Some(a) = &adj {
            let this = self.clone();
            let id = a.connect_value_changed(move |adj| {
                let imp = this.imp_();
                if imp.updating_adj.get() {
                    return;
                }
                let h = content_height(this.height());
                imp.buffer
                    .borrow_mut()
                    .scroll_to(adj.value().max(0.0) as u64, h, FOLLOW_SLOP);
                this.queue_draw();
            });
            *imp.vadj_handler.borrow_mut() = Some(id);
        }
        *imp.vadjustment.borrow_mut() = adj;
        self.sync_adjustment(content_height(self.height()));
    }

    /// Push the buffer's state into the scroll adjustment.
    ///
    /// The value comes from the anchor, never the other way round —
    /// that is what makes append, resize, zoom and history backfill all
    /// preserve reading position without each needing its own fix-up.
    fn sync_adjustment(&self, viewport_height: u32) {
        let imp = self.imp_();
        let Some(adj) = imp.vadjustment.borrow().clone() else {
            return;
        };
        let (total, value) = {
            let mut buf = imp.buffer.borrow_mut();
            (buf.total_height(), buf.scroll_offset(viewport_height))
        };
        let page = f64::from(viewport_height);
        imp.updating_adj.set(true);
        adj.configure(
            value as f64,
            0.0,
            (total as f64).max(page),
            page / 10.0,
            page * 0.9,
            page,
        );
        imp.updating_adj.set(false);
    }

    // ---- rendering ----------------------------------------------------

    fn resolve(&self, c: ColorRef, fallback: usize) -> gtk4::gdk::RGBA {
        let pal = self.imp_().palette.borrow();
        match c {
            ColorRef::Default => pal[fallback],
            ColorRef::Palette(i) => pal[(i as usize).min(PALETTE_COLS - 1)],
            ColorRef::Rgb(v) => gtk4::gdk::RGBA::new(
                ((v >> 16) & 0xff) as f32 / 255.0,
                ((v >> 8) & 0xff) as f32 / 255.0,
                (v & 0xff) as f32 / 255.0,
                1.0,
            ),
        }
    }

    fn snapshot_content(&self, snapshot: &gtk4::Snapshot) {
        let imp = self.imp_();
        let alloc_w = self.width().max(0);
        let alloc_h = self.height().max(0);
        if alloc_h <= 0 {
            return;
        }
        let height = content_height(alloc_h) as i32;

        // Background covers the whole allocation, padding included —
        // the inset is meant to be empty margin, not a differently
        // coloured border.
        let bg = self.imp_().palette.borrow()[PAL_BG];
        snapshot.append_color(
            &bg,
            &gtk4::graphene::Rect::new(0.0, 0.0, alloc_w as f32, alloc_h as f32),
        );

        // Everything below draws in content coordinates.
        snapshot.save();
        snapshot.translate(&gtk4::graphene::Point::new(PAD_X as f32, PAD_Y as f32));

        let scroll = {
            let mut buf = imp.buffer.borrow_mut();
            buf.scroll_offset(height as u32)
        };

        // Lay out only what is on screen, and resolve each row's top
        // edge while we still hold the mutable borrow — `offset_of`
        // needs `&mut` because it repairs the index's lazy prefix sums.
        // Doing it here rather than inside the draw loop is what keeps
        // the loop on a plain read borrow.
        //
        // This is the O(visible) property: a resize marked every row
        // unmeasured, and only these get re-measured.
        let placed: Vec<(usize, i64)> = {
            let m = imp.measure.borrow();
            let mut buf = imp.buffer.borrow_mut();
            let rows = buf.ensure_visible(scroll, height as u32, &*m);
            rows.into_iter()
                .map(|row| {
                    let top = buf.index_mut().offset_of(row) as i64 - scroll as i64;
                    (row, top)
                })
                .collect()
        };

        // The indent separator, matching gtk_xtext_draw_sep: a full-height
        // vertical rule half a space-width left of the body column, drawn
        // only in indent mode. xtext's non-thin variant is a two-pixel
        // bevel (bg then fg); only that form is ever requested here.
        //
        // Drawn *after* `ensure_visible`, and that ordering is the whole
        // point. Laying out a row with a wider nick than any seen so far
        // widens the shared gutter, so reading `indent_width` before the
        // layout pass gives the value from *before* this frame's rows
        // were measured. The rule then drew at the old position while the
        // text drew at the new one — visible for exactly one frame, on
        // the first message that widened the gutter, and self-correcting
        // on the next message, which is precisely the shape of the bug
        // that was reported.
        //
        // `separator_x()` rather than recomputing the geometry: the hit
        // test uses the same function, so the rule you see and the rule
        // you can grab cannot drift apart.
        if let Some(sx) = self.separator_x() {
            // Widget coords → content coords; the snapshot is translated
            // by PAD_X above.
            let x = (sx - PAD_X as f64) as f32;
            if x >= 1.0 {
                let fg = imp.palette.borrow()[PAL_FG];
                let bgc = imp.palette.borrow()[PAL_BG];
                snapshot.append_color(
                    &bgc,
                    &gtk4::graphene::Rect::new(x - 1.0, 0.0, 1.0, height as f32),
                );
                snapshot.append_color(
                    &fg,
                    &gtk4::graphene::Rect::new(x, 0.0, 1.0, height as f32),
                );
            }
        }

        let measure = imp.measure.borrow();
        let buf = imp.buffer.borrow();
        let font = measure.scaled_font();
        let ctx = measure.context();

        // One Layout for the whole pass, reset per run.
        //
        // Allocating a pango::Layout per styled run — which is what the
        // first cut did — puts an allocation plus a fresh shaping setup
        // in the innermost loop of the render path, so a long colourful
        // line pays for dozens of them every frame. Reusing is sound
        // because gtk_snapshot_append_layout builds its render nodes
        // from the layout's *current* contents immediately; nothing
        // retains a reference to it afterwards.
        let draw_layout = pango::Layout::new(ctx);
        draw_layout.set_font_description(Some(&font));

        let selection = *imp.selection.borrow();
        let show_stamp = imp.time_stamp.get();
        let stamp_format = imp.stamp_format.borrow().clone();
        let muted = self.imp_().palette.borrow()[PAL_HISTORY_MUTED];

        for (row, row_top) in placed {
            let Some(layout) = buf.layout_at(row) else {
                continue;
            };
            let Some(msg) = buf.message_at(row) else {
                continue;
            };

            // The timestamp belongs to the *row*, not to the gutter.
            //
            // Tying it to the gutter line box was wrong twice over:
            // info lines (`[hx] …`, appended with no nick column) have
            // no gutter at all and so never got a stamp, and xtext draws
            // it for every entry whenever auto_indent && time_stamp
            // regardless of whether there is any left text. Drawn once
            // here, against the row's own top edge.
            if show_stamp && row_top + i64::from(layout.height) >= 0 && row_top <= i64::from(height)
            {
                if let Some(stamp) = format_stamp(msg.timestamp, &stamp_format) {
                    draw_layout.set_attributes(None);
                    draw_layout.set_text(&stamp);
                    snapshot.save();
                    snapshot.translate(&gtk4::graphene::Point::new(0.0, row_top as f32));
                    snapshot.append_layout(&draw_layout, &muted);
                    snapshot.restore();
                }
            }

            // Fenced code blocks get a box: a tinted, outlined rect
            // spanning the block's lines. Painted before the text, and
            // derived from the *laid-out* line boxes rather than from
            // separate geometry, so the box cannot land anywhere other
            // than under the code it belongs to.
            if let Some(msg) = buf.message_at(row) {
                for (bi, blk) in msg.blocks.iter().enumerate() {
                    if !matches!(blk, hxchat_layout::Block::Code { .. }) {
                        continue;
                    }
                    let src = LineSource::Block(bi);
                    let mut top = i64::MAX;
                    let mut bot = i64::MIN;
                    let mut left = i32::MAX;
                    for l in layout.lines.iter().filter(|l| l.source == src) {
                        top = top.min(l.y as i64);
                        bot = bot.max(l.y as i64 + l.height as i64);
                        left = left.min(l.x as i32);
                    }
                    if top > bot {
                        continue; // no lines: nothing to box
                    }
                    let x = left as f32 - CODE_BOX_PAD;
                    let y = (row_top + top) as f32 - CODE_BOX_PAD;
                    let w = (content_width(alloc_w) as f32 - x).max(1.0);
                    let h = (bot - top) as f32 + CODE_BOX_PAD * 2.0;

                    let fgc = imp.palette.borrow()[PAL_FG];
                    let fill = gtk4::gdk::RGBA::new(
                        fgc.red(),
                        fgc.green(),
                        fgc.blue(),
                        CODE_BG_ALPHA,
                    );
                    let edge = gtk4::gdk::RGBA::new(
                        fgc.red(),
                        fgc.green(),
                        fgc.blue(),
                        CODE_BORDER_ALPHA,
                    );
                    let rect = gtk4::graphene::Rect::new(x, y, w, h);
                    let rounded = gtk4::gsk::RoundedRect::from_rect(rect, 4.0);
                    snapshot.push_rounded_clip(&rounded);
                    snapshot.append_color(&fill, &rect);
                    snapshot.pop();
                    snapshot.append_border(
                        &rounded,
                        &[1.0; 4],
                        &[edge, edge, edge, edge],
                    );
                }
            }

            // The speaker's avatar, on group heads only. Resolved per
            // frame rather than cached: an animated avatar advances on a
            // shared timer, and holding a texture would freeze it on
            // whichever frame happened to be current when the row was
            // appended.
            if let Some(av) = layout.avatar {
                let tex = unsafe {
                    hx_chat_avatar_for_uid(
                        self.as_ptr() as *mut gtk4::ffi::GtkWidget,
                        av.uid,
                    )
                };
                if !tex.is_null() {
                    let tex: gtk4::gdk::Texture =
                        unsafe { gtk4::glib::translate::from_glib_none(tex) };
                    // Fit inside the slot preserving aspect, so a banner-
                    // shaped icon isn't stretched into a square.
                    let (iw, ih) = (tex.width().max(1), tex.height().max(1));
                    let scale =
                        (av.size as f64 / iw as f64).min(av.size as f64 / ih as f64);
                    let (dw, dh) = (
                        (iw as f64 * scale).max(1.0),
                        (ih as f64 * scale).max(1.0),
                    );
                    snapshot.save();
                    snapshot.translate(&gtk4::graphene::Point::new(
                        av.x as f32,
                        (row_top + av.y as i64) as f32,
                    ));
                    tex.snapshot(snapshot, dw, dh);
                    snapshot.restore();
                }
            }

            for line in &layout.lines {
                let y = row_top + i64::from(line.y);
                if y + i64::from(line.height) < 0 || y > i64::from(height) {
                    continue;
                }
                let (text, spans): (&str, &[Span]) = match line.source {
                    LineSource::Gutter => match &msg.gutter {
                        Some(g) => (g.text.as_str(), &g.spans),
                        None => continue,
                    },
                    LineSource::Block(bi) => match msg.blocks.get(bi) {
                        Some(hxchat_layout::Block::Text(p)) => (p.text.as_str(), &p.spans),
                        Some(hxchat_layout::Block::Quote { content, .. }) => {
                            (content.text.as_str(), &content.spans)
                        }
                        Some(hxchat_layout::Block::Code { text, .. }) => (text.as_str(), &[]),
                        // A decoded image paints as a texture; an
                        // undecoded one falls back to its placeholder
                        // text, which is exactly the Phase 9.D
                        // behaviour and what the user sees while the
                        // fetch is in flight.
                        Some(hxchat_layout::Block::Image { alt, token, size }) => {
                            // Borrowed, not cloned: this runs for every
                            // visible image on every snapshot, and an
                            // animated one snapshots at its frame rate.
                            // A clone here is a GObject ref/unref pair
                            // per image per frame for no gain — nothing
                            // in the branch can touch `media`, so the
                            // borrow safely outlives the draw.
                            let media = imp.media.borrow();
                            if let (Some(sz), Some(tex)) =
                                (size, media.get(token).and_then(|m| m.texture()))
                            {
                                let avail =
                                    (content_width(alloc_w)).saturating_sub(line.x);
                                let (dw, dh) = measure.image_size(
                                    (sz.width, sz.height),
                                    avail,
                                );
                                snapshot.save();
                                snapshot.translate(&gtk4::graphene::Point::new(
                                    line.x as f32,
                                    y as f32,
                                ));
                                tex.snapshot(snapshot, dw as f64, dh as f64);
                                snapshot.restore();
                                continue;
                            }
                            (alt.as_str(), &[][..])
                        }
                        None => continue,
                    },
                };
                let slice = text.get(line.range.clone()).unwrap_or("");
                if slice.is_empty() {
                    continue;
                }

                // x comes straight from the line box — the layout
                // engine right-aligns the gutter, so the view no longer
                // has its own opinion about where it goes.
                let x = line.x as f32;

                // What of this line is selected. Resolved by the buffer,
                // the same call `selected_text` uses, so what is painted
                // and what gets copied cannot drift apart.
                let row_sel = selection
                    .as_ref()
                    .map(|s| buf.row_selection(row, s))
                    .unwrap_or(RowSelection::None);
                let hl = buf
                    .covered_range(row, line.source, &row_sel)
                    .and_then(|r| {
                        let s = r.start.max(line.range.start);
                        let e = r.end.min(line.range.end);
                        if s < e {
                            Some((s, e))
                        } else {
                            None
                        }
                    });

                if trace_selection() && selection.is_some() {
                    eprintln!(
                        "[chatview] row={row} src={:?} line={:?} row_sel={:?} hl={:?}",
                        line.source, line.range, row_sel, hl
                    );
                }

                // Search bands for this line, in block-local bytes and
                // clipped to the line, exactly as `hl` is above.
                let hits: Vec<(usize, usize, bool)> = {
                    let st = imp.search.borrow();
                    if st.is_active() {
                        let id = buf.id_at(row);
                        st.matches()
                            .iter()
                            .filter(|m| Some(m.message) == id && m.source == line.source)
                            .filter_map(|m| {
                                let a = m.start.max(line.range.start);
                                let b = m.end.min(line.range.end);
                                (a < b).then(|| (a, b, st.is_current(m)))
                            })
                            .collect()
                    } else {
                        Vec::new()
                    }
                };

                let hover = buf
                    .id_at(row)
                    .and_then(|id| self.hover_range_for(id, line.source))
                    .and_then(|r| {
                        let a = r.start.max(line.range.start);
                        let b = r.end.min(line.range.end);
                        (a < b).then_some((a, b))
                    });

                self.draw_runs(
                    snapshot,
                    &draw_layout,
                    slice,
                    line.range.start,
                    spans,
                    hl,
                    &hits,
                    hover,
                    x,
                    y as f32,
                );
            }
        }

        snapshot.restore();
    }

    /// Draw one visual line as a sequence of styled runs.
    #[allow(clippy::too_many_arguments)]
    /// Draw one visual line as styled runs, highlighting `hl`.
    ///
    /// **The selection is drawn here, inside the same walk that draws
    /// the glyphs.** The first version measured the highlight with the
    /// shared layout after `set_attributes(None)` and then drew the text
    /// with per-span attributes — so wherever a style changed the
    /// metrics (bold, and monospace `code` especially) the highlight
    /// rectangle drifted away from the glyphs it was supposed to be
    /// under.
    ///
    /// Splitting each style run at the selection boundaries makes every
    /// emitted piece uniform in *both* style and selectedness, so its
    /// width is measured with exactly the attributes it is rendered
    /// with. Geometry agreement is structural rather than something to
    /// keep in sync.
    #[allow(clippy::too_many_arguments)]
    fn draw_runs(
        &self,
        snapshot: &gtk4::Snapshot,
        layout: &pango::Layout,
        slice: &str,
        slice_start: usize,
        spans: &[hxchat_layout::Span],
        hl: Option<(usize, usize)>,
        search: &[(usize, usize, bool)],
        hover: Option<(usize, usize)>,
        x0: f32,
        y: f32,
    ) {
        let mut x = x0;
        let mut cursor = 0usize;
        let end = slice.len();

        // Selection bounds in slice-local coordinates.
        // Slice-local, and clamped to char boundaries: `&slice[a..b]`
        // panics off a boundary, and a panic inside `snapshot` unwinds
        // across the FFI, which aborts. Offsets come from `fit_prefix`
        // and so should already be aligned; this makes "should" not
        // matter.
        let floor_boundary = |i: usize| {
            let mut i = i.min(end);
            while i > 0 && !slice.is_char_boundary(i) {
                i -= 1;
            }
            i
        };
        // Every band that recolours part of this slice, in slice-local
        // coordinates. Selection outranks the current match outranks a
        // plain match, so a selection dragged over a hit still looks
        // selected.
        let mut bands: Vec<(usize, usize, Mark)> = Vec::new();
        if let Some((a, b)) = hl {
            let (a, b) = (
                floor_boundary(a.saturating_sub(slice_start)),
                floor_boundary(b.saturating_sub(slice_start)),
            );
            if a < b {
                bands.push((a, b, Mark::Selection));
            }
        }
        for (a, b, is_current) in search {
            let (a, b) = (
                floor_boundary(a.saturating_sub(slice_start)),
                floor_boundary(b.saturating_sub(slice_start)),
            );
            if a < b {
                bands.push((
                    a,
                    b,
                    if *is_current {
                        Mark::CurrentMatch
                    } else {
                        Mark::Match
                    },
                ));
            }
        }

        // Hover is tracked separately from `bands` rather than as another
        // Mark, because it is an *attribute* (underline) not a colour,
        // and it has to compose: a hovered link inside a selection
        // should be both selected and underlined.
        let hover_band = hover.map(|(a, b)| {
            (
                floor_boundary(a.saturating_sub(slice_start)),
                floor_boundary(b.saturating_sub(slice_start)),
            )
        });
        let is_hovered = |pos: usize| match hover_band {
            Some((a, b)) => pos >= a && pos < b,
            None => false,
        };

        let mark_at = |pos: usize| {
            bands
                .iter()
                .filter(|(a, b, _)| pos >= *a && pos < *b)
                .map(|(_, _, m)| *m)
                .max_by_key(|m| m.rank())
                .unwrap_or(Mark::None)
        };

        let mark_fg = self.imp_().palette.borrow()[PAL_MARK_FG];
        let mark_bg = self.imp_().palette.borrow()[PAL_MARK_BG];

        let emit = |text: &str, style: Style, mark: Mark, underline: bool, x: &mut f32| {
            if text.is_empty() {
                return;
            }
            let style = if underline {
                Style {
                    attrs: style.attrs.union(hxchat_layout::Attrs::UNDERLINE),
                    ..style
                }
            } else {
                style
            };
            layout.set_attributes(Some(&PangoMeasure::attrs_for(style)));
            layout.set_text(text);
            let (w, h) = layout.pixel_size();

            let band_bg = match mark {
                Mark::Selection => Some(mark_bg),
                Mark::CurrentMatch => Some(SEARCH_CURRENT_BG),
                Mark::Match => Some(SEARCH_MATCH_BG),
                Mark::None => None,
            };
            // Inline `code` gets a tint under it — see CODE_BG_ALPHA for
            // why the monospace attribute alone is not enough. Drawn
            // beneath any band, so a selected code span still reads as
            // selected.
            if style.attrs.contains(hxchat_layout::Attrs::CODE) {
                let fgc = self.imp_().palette.borrow()[PAL_FG];
                let tint = gtk4::gdk::RGBA::new(
                    fgc.red(),
                    fgc.green(),
                    fgc.blue(),
                    CODE_BG_ALPHA,
                );
                snapshot.append_color(
                    &tint,
                    &gtk4::graphene::Rect::new(*x, y, w as f32, h as f32),
                );
            }
            if let Some(bg) = band_bg {
                snapshot.append_color(
                    &bg,
                    &gtk4::graphene::Rect::new(*x, y, w as f32, h as f32),
                );
            } else if style.bg != ColorRef::Default {
                snapshot.append_color(
                    &self.resolve(style.bg, PAL_BG),
                    &gtk4::graphene::Rect::new(*x, y, w as f32, h as f32),
                );
            }

            // The search bands are fixed colours, so their ink is fixed
            // too — the theme foreground is near-white on a dark theme
            // and would vanish against yellow.
            let fg = match mark {
                Mark::Selection => mark_fg,
                Mark::CurrentMatch => SEARCH_CURRENT_FG,
                Mark::Match => SEARCH_MATCH_FG,
                Mark::None => self.resolve(style.fg, PAL_FG),
            };
            snapshot.save();
            snapshot.translate(&gtk4::graphene::Point::new(*x, y));
            snapshot.append_layout(layout, &fg);
            snapshot.restore();
            *x += w as f32;
        };

        // Emit `from..to` of the slice under one style, split wherever
        // the mark changes so each piece is uniformly marked. Splitting
        // here rather than measuring separately is what keeps the
        // highlight geometry and the glyphs in agreement — they come
        // from the same `layout.pixel_size()` call.
        let emit_split = |from: usize, to: usize, style: Style, x: &mut f32| {
            if from >= to {
                return;
            }
            // Boundaries of interest inside this run, sorted.
            let mut cuts: Vec<usize> = vec![from, to];
            for (a, b, _) in &bands {
                for p in [*a, *b] {
                    if p > from && p < to {
                        cuts.push(p);
                    }
                }
            }
            if let Some((a, b)) = hover_band {
                for p in [a, b] {
                    if p > from && p < to {
                        cuts.push(p);
                    }
                }
            }
            cuts.sort_unstable();
            cuts.dedup();

            for w in cuts.windows(2) {
                let (a, b) = (w[0], w[1]);
                if a < b {
                    emit(&slice[a..b], style, mark_at(a), is_hovered(a), x);
                }
            }
        };

        for s in spans {
            // Span ranges are over the block's whole text; shift into
            // slice-local coordinates.
            let ss = s.range.start.saturating_sub(slice_start);
            let se = s.range.end.saturating_sub(slice_start);
            if se <= cursor || ss >= end {
                continue;
            }
            let ss = ss.max(cursor).min(end);
            let se = se.min(end);
            emit_split(cursor, ss, Style::default(), &mut x);
            emit_split(ss, se, s.style, &mut x);
            cursor = se;
        }
        emit_split(cursor, end, Style::default(), &mut x);
    }
}

/// Convenience for tests and the FFI: append a plain system message.
pub fn plain_message(text: &str) -> Message {
    Message::system(ParsedText::plain(text))
}

impl HxChatView {
    /// Mark of the row carrying an image block with `token`.
    pub fn find_image(&self, token: u32) -> Option<MessageId> {
        self.imp_().buffer.borrow().find_image(token)
    }
}

/// Usable content width for a given allocation.
fn content_width(alloc_width: i32) -> u32 {
    (alloc_width - 2 * PAD_X).max(1) as u32
}

/// Usable content height for a given allocation.
///
/// Deliberately subtracted from the *scrollable* height too, not just
/// the drawing origin: if the viewport reported its full allocation
/// while the content drew inset, the last row would sit under the
/// bottom padding and be unreachable at the end of the scroll range.
fn content_height(alloc_height: i32) -> u32 {
    (alloc_height - 2 * PAD_Y).max(1) as u32
}

/// Format a unix timestamp with a strftime-style pattern.
///
/// `glib::DateTime::format` is strftime-compatible and locale-aware,
/// which is what lets the existing `CFG_STAMP_FORMAT` pref keep working
/// unchanged against the new backend. Returns `None` rather than
/// substituting a placeholder when the timestamp or the pattern is
/// unusable — a missing stamp is better than a wrong one.
fn format_stamp(unix: i64, format: &str) -> Option<String> {
    if unix <= 0 {
        return None;
    }
    let dt = glib::DateTime::from_unix_local(unix).ok()?;
    dt.format(format).ok().map(|g| g.to_string())
}

// ---- selection ------------------------------------------------------

impl HxChatView {
    /// Drag-to-select, click-to-clear, and Ctrl+C.
    /// Widget-space x of the drawn separator rule, if one is drawn.
    ///
    /// Single source of truth for both the snapshot and the hit test —
    /// the two disagreeing is how a divider ends up ungrabbable at the
    /// exact pixel it appears on.
    fn separator_x(&self) -> Option<f64> {
        let imp = self.imp_();
        if !imp.separator.get() {
            return None;
        }
        let indent = imp.buffer.borrow().indent_width();
        if indent == 0 {
            return None;
        }
        let half = self.half_space();
        Some(PAD_X as f64 + indent as f64 - half)
    }

    fn half_space(&self) -> f64 {
        let space = self.imp_().measure.borrow().metrics().space_width;
        (space as f64 + 1.0) / 2.0
    }

    /// Is `x` close enough to the separator to grab it?
    ///
    /// xtext used ±1 px, which is unhittable on a fractional-scale
    /// display; the C fork had already widened it to ±4 for that reason.
    fn on_separator(&self, x: f64) -> bool {
        match self.separator_x() {
            Some(sx) => (x - sx).abs() <= SEPARATOR_GRAB,
            None => false,
        }
    }

    /// Move the gutter so the separator lands under the pointer.
    ///
    /// Clamped to a band of the viewport rather than to `max_indent`:
    /// that cap is about how far the gutter may grow unattended, and the
    /// point of the drag is to overrule it. The upper bound keeps the
    /// body column from being squeezed out of existence.
    fn drag_separator_to(&self, x: f64) {
        let width = self.width();
        if width <= 0 {
            return;
        }
        let lo = PAD_X as f64 + MIN_INDENT as f64;
        let hi = (3.0 * width as f64) / 5.0;
        if hi <= lo {
            return;
        }
        let x = x.clamp(lo, hi);
        let indent = (x + self.half_space() - PAD_X as f64).max(0.0) as u32;
        let moved = self.imp_().buffer.borrow_mut().set_indent_width(indent);
        if moved {
            self.queue_resize();
            self.queue_draw();
        }
    }

    fn install_selection_gestures(&self) {
        // The widget has to be focusable for a key controller to reach
        // it; xtext's consumers call gtk_widget_set_can_focus(FALSE) to
        // keep the input box focused, so Ctrl+C is bound on the widget
        // rather than requiring focus.
        let drag = gtk4::GestureDrag::new();
        drag.set_button(gtk4::gdk::BUTTON_PRIMARY);

        let this = self.clone();
        drag.connect_drag_begin(move |g, x, y| {
            let _ = g;
            // The separator wins over selection: it lives in the gutter,
            // where a stray text drag is cheap to redo but a divider you
            // cannot grab is simply broken.
            if this.on_separator(x) {
                this.imp_().moving_separator.set(true);
                this.imp_().selecting.set(false);
                return;
            }
            // Record where the press landed, but do NOT install a
            // collapsed selection yet.
            //
            // This used to set `selection = Some(empty at caret)` right
            // here, which is what broke double- and triple-click: the
            // multi-click handler sets a word/row selection on the same
            // press, and whichever of the two gestures ran second won.
            // GTK gives no ordering guarantee between two controllers on
            // one widget, so the fix is to remove the conflict rather
            // than to sequence it — a press alone now changes nothing,
            // and the collapsed selection appears on first motion, which
            // is the moment it actually means something.
            *this.imp_().drag_start.borrow_mut() = this.caret_at(x, y);
            this.imp_().drag_moved.set(false);
            this.imp_().selecting.set(true);
            trace_clicks(|| format!("drag_begin at ({x:.0},{y:.0})"));
        });

        let this = self.clone();
        drag.connect_drag_update(move |g, dx, dy| {
            if this.imp_().moving_separator.get() {
                if let Some((sx, _)) = g.start_point() {
                    this.drag_separator_to(sx + dx);
                }
                return;
            }
            if !this.imp_().selecting.get() {
                return;
            }
            let Some((sx, sy)) = g.start_point() else {
                return;
            };
            if !this.imp_().drag_moved.get() {
                this.imp_().drag_moved.set(true);
                // First real motion: this is a drag, so anchor the
                // selection at the press point and drop whatever a
                // previous gesture had selected.
                let start = *this.imp_().drag_start.borrow();
                *this.imp_().selection.borrow_mut() =
                    start.map(|c| Selection::new(c, c));
                trace_clicks(|| "drag_update: first motion".to_string());
            }
            let (px, py) = (sx + dx, sy + dy);
            this.imp_().drag_pointer.set((px, py));
            this.sync_autoscroll();
            this.extend_selection_to(px, py);
        });

        let this = self.clone();
        drag.connect_drag_end(move |_, _, _| {
            if this.imp_().moving_separator.get() {
                this.imp_().moving_separator.set(false);
                return;
            }
            this.imp_().selecting.set(false);
            this.sync_autoscroll();
            // Drag-end autocopy, matching xtext's behaviour and driven
            // by the same three prefs (see set_autocopy_* on the C side).
            // Only for a real drag: a bare click selects nothing, and a
            // multi-click does its own copy in the press handler.
            if this.imp_().drag_moved.get() && autocopy_enabled() {
                // Both clipboards, matching xtext's autocopy: it took
                // clipboard ownership on drag-end
                // (gtk_xtext_set_clip_owner), and PRIMARY is what
                // middle-click paste reads. Writing both is also what
                // makes copying usable at all right now — see the note
                // on the Ctrl+C shortcut below.
                this.copy_selection_to(ClipboardTarget::Primary);
                this.copy_selection_to(ClipboardTarget::Clipboard);
            }
        });
        self.add_controller(drag);

        // A plain click with no drag clears the selection, which is what
        // every text view does and what makes "click to dismiss" work.
        let click = gtk4::GestureClick::new();
        click.set_button(gtk4::gdk::BUTTON_PRIMARY);
        let this = self.clone();
        click.connect_released(move |_, n_press, _, _| {
            // A click with no drag dismisses whatever was selected.
            //
            // The first cut had this backwards — it cleared only when
            // the selection was *already* empty, so a real selection
            // could never be dismissed. The drag handler has by now
            // collapsed anchor==focus for a click-without-motion, so
            // "empty but present" is exactly the click case, and a
            // non-empty selection means a drag just finished and must
            // be left alone.
            // A single click that involved no motion dismisses the
            // selection. Keying on "the pointer never moved" rather than
            // on "the selection is empty" is what lets drag_begin stop
            // collapsing the selection — which is what unbroke
            // double-click.
            if n_press != 1 || this.imp_().drag_moved.get() {
                return;
            }
            if this.imp_().selection.borrow().is_some() {
                *this.imp_().selection.borrow_mut() = None;
                this.queue_draw();
            }
        });
        // Double- and triple-click select a word and a line, as xtext
        // does. Handled on `pressed` rather than `released` so the drag
        // gesture's own begin — which fires first and collapses the
        // selection to a caret — doesn't wipe the result.
        let this = self.clone();
        click.connect_pressed(move |_, n_press, x, y| {
            trace_clicks(|| format!("pressed n_press={n_press} at ({x:.0},{y:.0})"));
            if n_press < 2 {
                return;
            }
            let Some(caret) = this.caret_at(x, y) else {
                return;
            };
            let sel = {
                let buf = this.imp_().buffer.borrow();
                if n_press == 2 {
                    buf.select_word(&caret)
                } else {
                    buf.row_of(caret.message).and_then(|r| buf.select_row(r))
                }
            };
            trace_clicks(|| format!("multi-click n={n_press} -> sel={}", sel.is_some()));
            if let Some(sel) = sel {
                *this.imp_().selection.borrow_mut() = Some(sel);
                // A multi-click is not a drag: stop the drag handler
                // from overwriting the focus on the next motion.
                this.imp_().selecting.set(false);
                this.imp_().drag_moved.set(false);
                this.queue_draw();
                if autocopy_enabled() {
                    this.copy_selection_to(ClipboardTarget::Primary);
                }
            }
        });

        // Primary-click word-click, for the handlers that filter on it
        // (chat-history's sentinel and inline media). Emitted on
        // release, and only when no drag happened, so selecting text
        // doesn't also activate whatever was under the press.
        let this = self.clone();
        click.connect_released(move |g, n_press, x, y| {
            if n_press != 1 || this.has_selection() {
                return;
            }
            this.emit_word_click(x, y, g.current_event().as_ref());
        });
        self.add_controller(click);

        // Ctrl+C.
        //
        // A ShortcutController on this widget does not work, global
        // scope or not: `chat.c` calls gtk_widget_set_can_focus(FALSE)
        // so typing goes to the input, the input is a GtkTextView with
        // its own Ctrl+C binding, and being the focused widget it
        // consumes the key first — copying its own (empty) selection.
        //
        // So the handler goes on the *root*, in the capture phase, which
        // runs before the focus path. It consumes the key only when this
        // view actually has a selection, so Ctrl+C in the input still
        // behaves normally the rest of the time.
        let this = self.clone();
        self.connect_root_notify(move |v| v.rebind_root_copy_shortcut());
        this.rebind_root_copy_shortcut();
    }

    /// (Re)install the capture-phase Ctrl+C handler on the current root.
    fn rebind_root_copy_shortcut(&self) {
        let imp = self.imp_();
        // Drop the old one first — a view can be re-rooted when its tab
        // moves, and leaving handlers on stale windows would both leak
        // and double-fire.
        if let Some((root, id)) = imp.root_key_handler.borrow_mut().take() {
            if let Some(w) = root.upgrade() {
                w.remove_controller(&id);
            }
        }
        let Some(root) = self.root() else {
            return;
        };
        let key = gtk4::EventControllerKey::new();
        key.set_propagation_phase(gtk4::PropagationPhase::Capture);
        let this = self.clone();
        key.connect_key_pressed(move |c, keyval, _, state| {
            let ctrl = state.contains(gtk4::gdk::ModifierType::CONTROL_MASK);
            let is_c = keyval == gtk4::gdk::Key::c || keyval == gtk4::gdk::Key::C;
            if ctrl && is_c && this.has_selection() {
                this.copy_selection_to(ClipboardTarget::Clipboard);
                return glib::Propagation::Stop;
            }
            if this.handle_scroll_key(c, keyval, state) {
                return glib::Propagation::Stop;
            }
            glib::Propagation::Proceed
        });
        let root_widget: gtk4::Widget = root.clone().upcast();
        root_widget.add_controller(key.clone());
        *imp.root_key_handler.borrow_mut() =
            Some((glib::object::WeakRef::new(), key.upcast()));
        if let Some((weak, _)) = imp.root_key_handler.borrow().as_ref() {
            weak.set(Some(&root_widget));
        }
    }

    /// Page/Home/End handling for the capture-phase root controller.
    ///
    /// These keys have to be stolen from the focus path rather than
    /// bound here, because consumers call
    /// `gtk_widget_set_can_focus(FALSE)` on the chat view so the message
    /// input keeps focus — and GtkTextView binds Page_Up/Page_Down to
    /// its own cursor movement, so a global-scope GtkShortcut (which
    /// runs *after* normal propagation) would never fire. That is why
    /// paging has never worked in GtkHx: nothing in the tree ever bound
    /// it, and the widget that had focus swallowed it.
    ///
    /// The steal is narrow on purpose. It only applies when focus is in
    /// a text-entry widget — the message input or the subject entry,
    /// where paging means nothing — so the user list keeps its own
    /// page-by-page keyboard navigation.
    fn handle_scroll_key(
        &self,
        c: &gtk4::EventControllerKey,
        keyval: gtk4::gdk::Key,
        state: gtk4::gdk::ModifierType,
    ) -> bool {
        use gtk4::gdk::Key;
        // A view that isn't on screen (a background tab, a closed private
        // chat) must not eat the window's keys.
        if !self.is_mapped() {
            return false;
        }
        let ctrl = state.contains(gtk4::gdk::ModifierType::CONTROL_MASK);
        let shift = state.contains(gtk4::gdk::ModifierType::SHIFT_MASK);

        let action = match keyval {
            Key::Page_Up | Key::KP_Page_Up => ScrollKey::Page(-1),
            Key::Page_Down | Key::KP_Page_Down => ScrollKey::Page(1),
            Key::Home | Key::KP_Home if ctrl => ScrollKey::Home,
            Key::End | Key::KP_End if ctrl => ScrollKey::End,
            _ => return false,
        };

        // Shift+PgUp/PgDn is the long-standing IRC binding for "scroll
        // the log" and is unambiguous anywhere, so it bypasses the
        // focus check. Unmodified paging only applies over an input.
        if !(shift && matches!(action, ScrollKey::Page(_))) && !focus_is_text_entry(c) {
            return false;
        }

        match action {
            ScrollKey::Page(dir) => self.scroll_page(dir),
            ScrollKey::Home => self.scroll_to_extreme(false),
            ScrollKey::End => self.scroll_to_extreme(true),
        }
        true
    }

    /// Scroll one viewport, less a line of overlap so the line being
    /// read stays on screen across the jump.
    pub fn scroll_page(&self, dir: i32) {
        let height = content_height(self.height());
        if height == 0 {
            return;
        }
        let line = self.imp_().measure.borrow().metrics().line_height.max(1);
        let step = height.saturating_sub(line).max(line) as i64;
        let cur = {
            let mut buf = self.imp_().buffer.borrow_mut();
            buf.scroll_offset(height) as i64
        };
        let next = (cur + step * dir as i64).max(0) as u64;
        self.scroll_absolute(next, height);
    }

    /// Jump to the top of the scrollback, or back to following the tail.
    pub fn scroll_to_extreme(&self, bottom: bool) {
        let height = content_height(self.height());
        if bottom {
            let total = self.imp_().buffer.borrow_mut().total_height();
            self.scroll_absolute(total.saturating_sub(height as u64), height);
        } else {
            self.scroll_absolute(0, height);
        }
    }

    fn scroll_absolute(&self, y: u64, height: u32) {
        {
            let mut buf = self.imp_().buffer.borrow_mut();
            buf.scroll_to(y, height, FOLLOW_SLOP);
        }
        self.sync_adjustment(height);
        self.queue_draw();
    }

    // ---- in-buffer search --------------------------------------------

    /// Run a query and reveal the first hit at or below the viewport.
    ///
    /// Returns `(match_count, current_ordinal)` for the find bar's
    /// readout. An empty needle clears the search rather than matching
    /// everything.
    pub fn search_set(&self, needle: &str, case_sensitive: bool) -> (usize, usize) {
        let imp = self.imp_();
        if needle.is_empty() {
            self.search_clear();
            return (0, 0);
        }
        let hits = imp.buffer.borrow().search(needle, case_sensitive);
        let height = content_height(self.height());
        // Anchor the first step to whatever is on screen, so opening the
        // bar in a long scrollback starts where you are looking rather
        // than at the top.
        let from_row = {
            let mut buf = imp.buffer.borrow_mut();
            let y = buf.scroll_offset(height);
            buf.index_mut().locate(y).map(|h| h.row).unwrap_or(0)
        };
        {
            let mut st = imp.search.borrow_mut();
            st.set_results(needle, case_sensitive, hits);
            let buf = imp.buffer.borrow();
            st.seek_from(|id| buf.row_of(id), from_row);
        }
        self.reveal_current_match();
        self.search_readout()
    }

    /// Step to the next (`dir > 0`) or previous (`dir < 0`) match.
    pub fn search_step(&self, dir: i32) -> (usize, usize) {
        {
            let mut st = self.imp_().search.borrow_mut();
            st.step(dir);
        }
        self.reveal_current_match();
        self.search_readout()
    }

    pub fn search_clear(&self) {
        self.imp_().search.borrow_mut().clear();
        self.queue_draw();
    }

    /// `(total, ordinal)`; ordinal is 1-based, 0 for "none current".
    pub fn search_readout(&self) -> (usize, usize) {
        let st = self.imp_().search.borrow();
        (st.len(), st.ordinal().unwrap_or(0))
    }

    fn reveal_current_match(&self) {
        let imp = self.imp_();
        let cur = imp.search.borrow().current();
        if let Some(m) = cur {
            let height = content_height(self.height());
            let measure = imp.measure.borrow();
            imp.buffer.borrow_mut().reveal(m.message, height, &*measure);
            drop(measure);
            self.sync_adjustment(height);
        }
        self.queue_draw();
    }

    /// Widget-space point → document position.
    fn caret_at(&self, x: f64, y: f64) -> Option<Caret> {
        let imp = self.imp_();
        let height = content_height(self.height());
        let scroll = {
            let mut buf = imp.buffer.borrow_mut();
            buf.scroll_offset(height)
        };
        // Undo the padding origin, then convert to buffer coordinates.
        let cx = (x as i32) - PAD_X;
        let cy = ((y as i32) - PAD_Y).max(0) as u64 + scroll;
        let m = imp.measure.borrow();
        let mut buf = imp.buffer.borrow_mut();
        buf.hit_test(cx, cy, &*m)
    }

    /// The selected text, or empty.
    ///
    /// Honours `autocopy_stamp`: when on, each copied row is prefixed
    /// with its timestamp, which is what xtext's `mark_stamp` did. The
    /// pref exists precisely because pasting a chat excerpt with times
    /// is sometimes what you want and usually is not, so silently
    /// ignoring it — as this did until now — loses a real behaviour.
    pub fn selected_text(&self) -> String {
        let imp = self.imp_();
        let sel = *imp.selection.borrow();
        let Some(s) = sel.filter(|s| !s.is_empty()) else {
            return String::new();
        };
        let buf = imp.buffer.borrow();
        if !prefs::AUTOCOPY_STAMP.with(|c| c.get()) {
            return buf.selected_text(&s);
        }
        // Per *row*, not per output line.
        //
        // The first version post-processed the joined string with
        // `lines()`, assuming one line per row. A row's own text can
        // contain hard newlines — the wrap engine supports them — so
        // that assumption breaks on the first multi-line message, and
        // the stamps then drift onto the wrong rows and fall off the
        // end. Asking the buffer for the rows directly removes the
        // guess.
        let fmt = imp.stamp_format.borrow().clone();
        buf.selected_rows(&s)
            .into_iter()
            .map(|(row, text)| {
                match buf
                    .message_at(row)
                    .and_then(|m| format_stamp(m.timestamp, &fmt))
                {
                    Some(ts) => format!("{ts}{text}"),
                    None => text,
                }
            })
            .collect::<Vec<_>>()
            .join("\n")
    }

    pub fn has_selection(&self) -> bool {
        self.imp_()
            .selection
            .borrow()
            .map(|s| !s.is_empty())
            .unwrap_or(false)
    }

    pub fn clear_selection(&self) {
        if self.imp_().selection.borrow().is_some() {
            *self.imp_().selection.borrow_mut() = None;
            self.queue_draw();
        }
    }

    /// Which clipboard a copy targets.
    ///
    /// GTK 4 dropped `GdkAtom` selections: there are exactly two
    /// clipboards on a display, so this is a bool with a name.
    fn copy_selection_to(&self, target: ClipboardTarget) {
        let text = self.selected_text();
        if text.is_empty() {
            return;
        }
        let display = WidgetExt::display(self);
        let cb = match target {
            ClipboardTarget::Primary => display.primary_clipboard(),
            ClipboardTarget::Clipboard => display.clipboard(),
        };
        cb.set_text(&text);
    }
}

/// Which of a display's two clipboards to write.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum ClipboardTarget {
    /// Middle-click paste buffer; drag-end writes here, as xtext did.
    Primary,
    /// Ctrl+V clipboard.
    Clipboard,
}

/// Process-wide prefs, mirroring the ones xtext keeps as module globals.
///
/// These are genuinely process-wide rather than per-view — `options.c`
/// sets them once from the cfgvars, and `prefs_read` applies the stamp
/// format before any window exists — so the view crate keeps them the
/// same way. `thread_local` rather than a lock because every one of
/// these is touched only from the GTK main thread.
pub(crate) mod prefs {
    use std::cell::{Cell, RefCell};

    thread_local! {
        /// Drag-end copies to the clipboards. xtext's default is on.
        pub static AUTOCOPY_TEXT: Cell<bool> = const { Cell::new(true) };
        /// Include the timestamp column in copied text.
        pub static AUTOCOPY_STAMP: Cell<bool> = const { Cell::new(false) };
        /// Retain colour codes in copied text. Meaningless here — the
        /// new backend copies plain text — but accepted so the pref
        /// round-trips rather than erroring.
        pub static AUTOCOPY_COLOR: Cell<bool> = const { Cell::new(false) };
        /// The persisted stamp format, applied by `prefs_read` before
        /// any view exists. Views read it at construction; without this
        /// they would silently keep the built-in default and ignore the
        /// user's pref until they next edited it in Settings.
        pub static STAMP_FORMAT: RefCell<Option<String>> = const { RefCell::new(None) };
    }
}

/// Whether drag-end should copy. Driven by `CFG_AUTOCOPY_TEXT`.
fn autocopy_enabled() -> bool {
    prefs::AUTOCOPY_TEXT.with(|c| c.get())
}


// ---- zoom (scoping §3.7) --------------------------------------------

/// Zoom steps, per-mille. The browser/terminal ladder people already
/// have muscle memory for.
const ZOOM_STEPS: [u32; 13] = [
    500, 670, 800, 900, 1000, 1100, 1250, 1500, 1750, 2000, 2500, 3000, 4000,
];

impl HxChatView {
    fn install_zoom_bindings(&self) {
        // Ctrl + scroll.
        let scroll = gtk4::EventControllerScroll::new(
            gtk4::EventControllerScrollFlags::VERTICAL,
        );
        let this = self.clone();
        scroll.connect_scroll(move |c, _dx, dy| {
            if !c.current_event_state().contains(gtk4::gdk::ModifierType::CONTROL_MASK) {
                // Scroll the view ourselves.
                //
                // Implementing GtkScrollable is not enough: that only
                // does anything inside a GtkScrolledWindow, and the chat
                // view is packed as a bare child next to a plain
                // GtkScrollbar (chat.rs::build_content). Nothing was
                // consuming wheel events at all — Proceed handed them to
                // a parent that had no idea what to do with them.
                this.scroll_by_notches(dy);
                return glib::Propagation::Stop;
            }
            if dy < 0.0 {
                this.zoom_step(1);
            } else if dy > 0.0 {
                this.zoom_step(-1);
            }
            glib::Propagation::Stop
        });
        self.add_controller(scroll);

        // Ctrl + / - / 0. Global scope because focus lives in the chat
        // input, not here.
        let controller = gtk4::ShortcutController::new();
        controller.set_scope(gtk4::ShortcutScope::Global);
        for (accel, delta) in [
            ("<Control>plus", 1i32),
            ("<Control>equal", 1),
            ("<Control>KP_Add", 1),
            ("<Control>minus", -1),
            ("<Control>KP_Subtract", -1),
        ] {
            let this = self.clone();
            let action = gtk4::CallbackAction::new(move |_, _| {
                this.zoom_step(delta);
                glib::Propagation::Stop
            });
            if let Some(trigger) = gtk4::ShortcutTrigger::parse_string(accel) {
                controller.add_shortcut(gtk4::Shortcut::new(Some(trigger), Some(action)));
            }
        }
        let this = self.clone();
        let reset = gtk4::CallbackAction::new(move |_, _| {
            this.set_zoom_permille(1000);
            glib::Propagation::Stop
        });
        if let Some(trigger) = gtk4::ShortcutTrigger::parse_string("<Control>0") {
            controller.add_shortcut(gtk4::Shortcut::new(Some(trigger), Some(reset)));
        }
        self.add_controller(controller);
    }

    /// Scroll by wheel notches. Three text lines each, the usual step.
    fn scroll_by_notches(&self, notches: f64) {
        let height = content_height(self.height());
        if height == 0 {
            return;
        }
        let line = self.imp_().measure.borrow().metrics().line_height.max(1);
        let delta = notches * (line as f64) * WHEEL_LINES;
        let cur = {
            let mut buf = self.imp_().buffer.borrow_mut();
            buf.scroll_offset(height) as f64
        };
        let next = (cur + delta).max(0.0) as u64;
        {
            let mut buf = self.imp_().buffer.borrow_mut();
            buf.scroll_to(next, height, FOLLOW_SLOP);
        }
        self.sync_adjustment(height);
        self.queue_draw();
    }

    /// Move `delta` notches along [`ZOOM_STEPS`].
    ///
    /// Stepping through a fixed ladder rather than multiplying keeps the
    /// levels round and reproducible — repeated in/out returns to
    /// exactly 100% instead of drifting.
    pub fn zoom_step(&self, delta: i32) {
        let cur = self.zoom_permille();
        let idx = ZOOM_STEPS
            .iter()
            .position(|z| *z >= cur)
            .unwrap_or(ZOOM_STEPS.len() - 1) as i32;
        let next = (idx + delta).clamp(0, ZOOM_STEPS.len() as i32 - 1) as usize;
        if ZOOM_STEPS[next] != cur {
            self.set_zoom_permille(ZOOM_STEPS[next]);
        }
    }
}

// ---- links and the context menu -------------------------------------

impl HxChatView {
    fn install_link_handlers(&self) {
        // Hover: pointer cursor over a link, default elsewhere.
        let motion = gtk4::EventControllerMotion::new();
        let this = self.clone();
        motion.connect_motion(move |_, x, y| {
            let on_sep = this.on_separator(x) || this.imp_().moving_separator.get();
            let target = if on_sep {
                None
            } else {
                this.hover_target_at(x, y)
            };

            // Redraw only when the target actually changed. Motion fires
            // per pointer event; queueing a draw on every one of them
            // would repaint the whole view while the mouse merely
            // crosses it.
            let changed = *this.imp_().hovered.borrow() != target;
            if changed {
                *this.imp_().hovered.borrow_mut() = target.clone();
                this.queue_draw();
            }

            let want = if on_sep {
                "col-resize"
            } else if target.is_some() {
                "pointer"
            } else {
                "text"
            };
            if this.cursor().and_then(|c| c.name()).as_deref() != Some(want) {
                this.set_cursor_from_name(Some(want));
            }
        });
        let this = self.clone();
        motion.connect_leave(move |_| {
            // Drop the hover, or an underline is left behind when the
            // pointer leaves the widget without crossing off the target.
            if this.imp_().hovered.borrow().is_some() {
                *this.imp_().hovered.borrow_mut() = None;
                this.queue_draw();
            }
            this.set_cursor_from_name(Some("text"));
        });
        self.add_controller(motion);

        // Secondary / middle click: the URL menu if over a link, our own
        // context menu otherwise.
        //
        // Matching xtext's split exactly (`gtkurl_xtext_word_click`
        // filters out left-click and hands everything else to
        // gtkurl_show_popup), so the two backends agree about which
        // button does what.
        for button in [gtk4::gdk::BUTTON_SECONDARY, gtk4::gdk::BUTTON_MIDDLE] {
            let click = gtk4::GestureClick::new();
            click.set_button(button);
            let this = self.clone();
            click.connect_pressed(move |g, _, x, y| {
                // word-click first: gtkurl's handler filters on
                // secondary/middle and pops the URL menu itself, and the
                // media handler wants the token. Emitting keeps every
                // existing C consumer working.
                // A nick outranks everything: the gutter is where nicks
                // live, and the user menu is what a right-click there
                // means. Handled before word-click emission so the URL
                // handler can't also fire on a URL-shaped nick.
                if button == gtk4::gdk::BUTTON_SECONDARY {
                    if let Some(HoverTarget::Nick { uid, .. }) =
                        this.hover_target_at(x, y)
                    {
                        this.emit_speaker_menu(uid, x, y);
                        return;
                    }
                }

                this.emit_word_click(x, y, g.current_event().as_ref());

                match this.link_at_point(x, y) {
                    // Only pop our own URL menu for links *we* detected
                    // but gtkurl's word tokenisation didn't — otherwise
                    // the emission above already popped one and we'd
                    // stack two.
                    Some((href, _label)) if !this.word_is_url(x, y) => {
                        crate::links::show_url_popup(&this, &href, x, y);
                    }
                    Some(_) => {}
                    None if button == gtk4::gdk::BUTTON_SECONDARY => {
                        this.show_context_menu(x, y);
                    }
                    None => {}
                }
            });
            self.add_controller(click);
        }
    }

    /// The link under a widget-space point, as (href, visible label).
    /// Ask the C side for the user context menu on `uid`.
    ///
    /// A signal rather than a direct call, because the menu is built by
    /// `users.c::user_popup_show` — the *same* builder the Users window
    /// and the pchat sidebars use. The view has no business knowing what
    /// is on that menu (Get Info, Send Message, Kick, the voice volume
    /// slider, whatever is added next); it knows a uid and where the
    /// pointer was.
    fn emit_speaker_menu(&self, uid: u16, x: f64, y: f64) {
        self.emit_by_name::<()>("speaker-menu", &[&(uid as u32), &x, &y]);
    }

    /// What the pointer is over, if it is activatable.
    ///
    /// A nick takes precedence over a link inside it, since the gutter
    /// is where nicks live and a URL-shaped nick is a curiosity rather
    /// than something you want to open.
    fn hover_target_at(&self, x: f64, y: f64) -> Option<HoverTarget> {
        // The avatar first: it is painted from the layout's avatar box
        // rather than from a line box, so the caret hit-test below cannot
        // see it. Clicking someone's icon should mean the same thing as
        // clicking their name.
        {
            let imp = self.imp_();
            let height = content_height(self.height());
            let scroll = imp.buffer.borrow_mut().scroll_offset(height);
            let cx = (x as i32) - PAD_X;
            let cy = ((y as i32) - PAD_Y).max(0) as u64 + scroll;
            // One borrow, released before anything else touches the
            // buffer. The first cut chained a borrow() inside an
            // and_then() on a live borrow_mut(), which is a RefCell
            // panic the moment the pointer crosses an icon.
            let hit = imp.buffer.borrow_mut().avatar_at(cx, cy);
            if let Some((message, uid)) = hit {
                return Some(HoverTarget::Nick { message, uid });
            }
        }

        let caret = self.caret_at(x, y)?;
        let buf = self.imp_().buffer.borrow();
        if caret.source == LineSource::Gutter {
            if let Some(sp) = buf.speaker_of(caret.message) {
                if sp.uid != 0 {
                    return Some(HoverTarget::Nick {
                        message: caret.message,
                        uid: sp.uid,
                    });
                }
            }
            return None;
        }
        buf.link_range_at(&caret).map(|range| HoverTarget::Link {
            message: caret.message,
            source: caret.source,
            range,
        })
    }

    /// The byte range this line should underline, if the hovered target
    /// lands in it.
    fn hover_range_for(
        &self,
        row_id: MessageId,
        source: LineSource,
    ) -> Option<std::ops::Range<usize>> {
        let hovered = self.imp_().hovered.borrow();
        match hovered.as_ref()? {
            HoverTarget::Nick { message, .. } => {
                if *message != row_id || source != LineSource::Gutter {
                    return None;
                }
                self.imp_().buffer.borrow().gutter_range(row_id)
            }
            t => {
                let (m, s, r) = t.underline()?;
                (m == row_id && s == source).then_some(r)
            }
        }
    }

    fn link_at_point(&self, x: f64, y: f64) -> Option<(String, String)> {
        let caret = self.caret_at(x, y)?;
        self.imp_().buffer.borrow().link_at(&caret)
    }

    /// Right-click menu for ordinary text: Copy and Select All.
    fn show_context_menu(&self, x: f64, y: f64) {
        let menu = gtk4::gio::Menu::new();
        menu.append(Some(&crate::tr("Copy")), Some("chatview.copy"));
        menu.append(Some(&crate::tr("Select All")), Some("chatview.select-all"));

        let group = gtk4::gio::SimpleActionGroup::new();

        let copy = gtk4::gio::SimpleAction::new("copy", None);
        let this = self.clone();
        copy.connect_activate(move |_, _| {
            this.copy_selection_to(ClipboardTarget::Clipboard);
        });
        // Greyed out with nothing selected, rather than silently doing
        // nothing.
        copy.set_enabled(self.has_selection());
        group.add_action(&copy);

        let select_all = gtk4::gio::SimpleAction::new("select-all", None);
        let this = self.clone();
        select_all.connect_activate(move |_, _| {
            this.select_all();
        });
        group.add_action(&select_all);
        self.insert_action_group("chatview", Some(&group));

        let popover = gtk4::PopoverMenu::from_model(Some(&menu));
        popover.set_parent(self);
        popover.set_has_arrow(false);
        popover.set_pointing_to(Some(&gtk4::gdk::Rectangle::new(
            x as i32, y as i32, 1, 1,
        )));
        // The popover owns itself: unparent on close, or it leaks and
        // keeps the view alive.
        popover.connect_closed(|p| p.unparent());
        popover.popup();
    }

    /// Select the whole buffer.
    pub fn select_all(&self) {
        let imp = self.imp_();
        // The buffer decides what "everything" is — see
        // ChatBuffer::select_all for the two ways doing it here got it
        // wrong.
        let sel = imp.buffer.borrow().select_all();
        if sel.is_none() {
            return;
        }
        *imp.selection.borrow_mut() = sel;
        self.queue_draw();
    }
}

// ---- word-click (xtext parity) --------------------------------------

impl HxChatView {
    /// Emit `word-click` for the word under a widget-space point.
    ///
    /// The word is handed over as a raw `char *` because that is what
    /// xtext's signal signature is and what the C handlers expect. The
    /// `CString` lives for the duration of the emission and no longer —
    /// every handler in the tree either compares it or copies out of it
    /// synchronously, which is the same contract xtext offered (its
    /// pointer was into a scratch buffer reused on the next click).
    fn emit_word_click(&self, x: f64, y: f64, event: Option<&gtk4::gdk::Event>) {
        let Some(caret) = self.caret_at(x, y) else {
            return;
        };
        let Some(word) = self.imp_().buffer.borrow().word_at(&caret) else {
            return;
        };
        let Ok(c_word) = std::ffi::CString::new(word) else {
            return;
        };
        let word_ptr = c_word.as_ptr() as glib::ffi::gpointer;
        let event_ptr = event
            .map(|e| {
                use gtk4::glib::translate::ToGlibPtr;
                let p: *mut gtk4::gdk::ffi::GdkEvent = e.to_glib_none().0;
                p as glib::ffi::gpointer
            })
            .unwrap_or(std::ptr::null_mut());
        self.emit_by_name::<()>("word-click", &[&word_ptr, &event_ptr]);
    }
}

impl HxChatView {
    /// Whether the word under the point is one `gtkurl` would itself
    /// recognise — i.e. whether the `word-click` emission has already
    /// caused a URL menu to pop.
    ///
    /// Needed because two detectors are in play: `gtkurl_scan`, which
    /// finds URLs inside a line and gives us the link spans, and
    /// `gtkurl_is_url`, which classifies a whitespace-delimited *word*
    /// and is what the signal handler uses. They mostly agree; where
    /// they don't, this stops us stacking a second popover on top of
    /// the one the handler already opened.
    fn word_is_url(&self, x: f64, y: f64) -> bool {
        let Some(caret) = self.caret_at(x, y) else {
            return false;
        };
        let Some(word) = self.imp_().buffer.borrow().word_at(&caret) else {
            return false;
        };
        crate::links::word_is_url(&word)
    }
}

// ---- inline media (C4) ----------------------------------------------

impl HxChatView {
    /// Install (or replace) the decoded frames for a media token, and
    /// resize the row to match.
    ///
    /// This is the operation the old design was worst at. xtext had to
    /// recompute the entry's subline list, diff the count against the
    /// old one, and patch `buf->num_lines` plus every scroll anchor
    /// (`gtk_xtext_media_set_texture`). Here it is a size change on a
    /// block, and the scroll anchor absorbs it — a decode landing above
    /// the viewport no longer shifts what the user is reading.
    pub fn set_media_frames(&self, token: u32, frames: Vec<(gtk4::gdk::Texture, u32)>) {
        let imp = self.imp_();
        if frames.is_empty() {
            imp.media.borrow_mut().remove(&token);
            // Clear the stored size too, or the row keeps the height of
            // an image it no longer has and the placeholder text draws
            // inside a tall empty box — contradicting the FFI's promise
            // that a NULL texture reverts the row to its placeholder.
            let m = imp.measure.borrow();
            let mut buf = imp.buffer.borrow_mut();
            if let Some(id) = buf.find_image(token) {
                buf.set_image_size(id, token, None, &*m);
            }
        } else {
            let entry = MediaEntry {
                frames,
                current: 0,
                since_us: 0,
            };
            let size = entry.size();
            imp.media.borrow_mut().insert(token, entry);

            if let Some(size) = size {
                let m = imp.measure.borrow();
                let mut buf = imp.buffer.borrow_mut();
                if let Some(id) = buf.find_image(token) {
                    buf.set_image_size(id, token, Some(size), &*m);
                }
            }
        }
        self.sync_animation_tick();
        self.queue_resize();
    }

    /// Start the frame timer if anything animates, stop it otherwise.
    ///
    /// One shared tick for the whole view rather than a timer per image
    /// — the same shape `gif_avatar.c` settled on for the user list, and
    /// for the same reason: dozens of independent timeouts is a lot of
    /// wakeups for something the frame clock already provides.
    fn sync_animation_tick(&self) {
        let imp = self.imp_();
        let animated = imp.media.borrow().values().any(|m| m.is_animated());
        let running = imp.anim_tick.borrow().is_some();
        if animated == running {
            return;
        }
        if !animated {
            if let Some(id) = imp.anim_tick.borrow_mut().take() {
                id.remove();
            }
            return;
        }
        let id = self.add_tick_callback(move |view, clock| {
            let imp = view.imp_();
            let now = clock.frame_time();
            let mut advanced = false;
            {
                let mut media = imp.media.borrow_mut();
                for entry in media.values_mut() {
                    if !entry.is_animated() {
                        continue;
                    }
                    let delay = entry
                        .frames
                        .get(entry.current)
                        .map(|(_, d)| *d)
                        .unwrap_or(100)
                        .max(MIN_FRAME_DELAY_MS) as i64
                        * 1000;
                    if entry.since_us == 0 {
                        entry.since_us = now;
                        continue;
                    }
                    if now - entry.since_us >= delay {
                        entry.current = (entry.current + 1) % entry.frames.len();
                        entry.since_us = now;
                        advanced = true;
                    }
                }
            }
            if advanced {
                // Only a redraw: every frame of an animation shares
                // dimensions, so the row's height cannot change.
                view.queue_draw();
            }
            glib::ControlFlow::Continue
        });
        *imp.anim_tick.borrow_mut() = Some(id);
    }

    /// Drop every decoded texture. Called with `clear`.
    fn clear_media(&self) {
        self.imp_().media.borrow_mut().clear();
        self.sync_animation_tick();
    }
}

impl HxChatView {
    /// The media token on the image block a mark names.
    pub fn image_token_of(&self, id: MessageId) -> Option<u32> {
        let buf = self.imp_().buffer.borrow();
        let msg = buf.message(id)?;
        msg.blocks.iter().find_map(|b| match b {
            hxchat_layout::Block::Image { token, .. } => Some(*token),
            _ => None,
        })
    }
}

/// `GTKHX_CHATVIEW_TRACE=selection` turns on a per-line dump of what the
/// snapshot pass thinks is selected.
///
/// Added because selection state spans three layers — gesture, model,
/// renderer — and static reading cannot tell which one is empty-handed.
/// The equivalent trick (a `g_message` reporting what was actually
/// constructed) is what identified the C2 floating-reference bug after
/// several wrong guesses.
/// `GTKHX_CHATVIEW_TRACE=clicks` — press counts and drag transitions.
///
/// Here because the sandbox this was developed in has no display, so a
/// click-handling bug can only be observed on the user's machine. One
/// run with this on says whether GTK is delivering `n_press >= 2` at
/// all, which is the fork in the road for any further diagnosis.
fn trace_clicks(msg: impl FnOnce() -> String) {
    use std::sync::OnceLock;
    static ON: OnceLock<bool> = OnceLock::new();
    let on = *ON.get_or_init(|| {
        std::env::var("GTKHX_CHATVIEW_TRACE")
            .map(|v| v.split(',').any(|p| p.trim() == "clicks"))
            .unwrap_or(false)
    });
    if on {
        eprintln!("[chatview] {}", msg());
    }
}

fn trace_selection() -> bool {
    use std::sync::OnceLock;
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| {
        std::env::var("GTKHX_CHATVIEW_TRACE")
            .map(|v| v.split(',').any(|p| p.trim() == "selection"))
            .unwrap_or(false)
    })
}

// ---- selection auto-scroll ------------------------------------------

/// Pixels per second at the maximum overshoot, and the overshoot at
/// which that rate is reached. Between zero and this the rate ramps, so
/// a small overshoot creeps and a large one moves.
const AUTOSCROLL_MAX_PPS: f64 = 1200.0;
const AUTOSCROLL_FULL_AT: f64 = 120.0;

impl HxChatView {
    /// Extend the live selection to a widget-space point.
    fn extend_selection_to(&self, x: f64, y: f64) {
        let Some(focus) = self.caret_at(x, y) else {
            return;
        };
        let mut sel = self.imp_().selection.borrow_mut();
        if let Some(s) = sel.as_mut() {
            s.focus = focus;
        }
        drop(sel);
        self.queue_draw();
    }

    /// How far outside the viewport the drag pointer is, in pixels.
    /// Negative above the top, positive below the bottom, 0 inside.
    fn drag_overshoot(&self) -> f64 {
        let (_, y) = self.imp_().drag_pointer.get();
        let top = f64::from(PAD_Y);
        let bottom = f64::from(self.height().max(0) - PAD_Y);
        if y < top {
            y - top
        } else if y > bottom {
            y - bottom
        } else {
            0.0
        }
    }

    /// Start the auto-scroll tick while a drag is outside the viewport,
    /// stop it otherwise.
    fn sync_autoscroll(&self) {
        let imp = self.imp_();
        let want = imp.selecting.get() && self.drag_overshoot() != 0.0;
        let running = imp.autoscroll_tick.borrow().is_some();
        if want == running {
            return;
        }
        if !want {
            if let Some(id) = imp.autoscroll_tick.borrow_mut().take() {
                id.remove();
            }
            return;
        }
        // Cell, not a plain local: add_tick_callback wants an `Fn`, so
        // the closure cannot mutate captured state directly.
        let last_us: std::cell::Cell<Option<i64>> = std::cell::Cell::new(None);
        let id = self.add_tick_callback(move |view, clock| {
            let imp = view.imp_();
            // Both stop conditions have to clear the stored id as well
            // as returning Break, or `autoscroll_tick` outlives the
            // callback it names: sync_autoscroll reads `is_some()` as
            // "running", so a self-terminated tick makes it believe
            // autoscroll is live and skip the restart, and its cleanup
            // path would call remove() on an id GTK has already
            // invalidated. Dropping a TickCallbackId is inert (gtk4-rs
            // has no Drop impl for it — removal is the explicit
            // `remove()`), so taking it here is exactly right.
            let stop = |view: &HxChatView| {
                *view.imp_().autoscroll_tick.borrow_mut() = None;
                glib::ControlFlow::Break
            };
            if !imp.selecting.get() {
                return stop(view);
            }
            let overshoot = view.drag_overshoot();
            if overshoot == 0.0 {
                return stop(view);
            }
            // Frame-time based rather than per-tick constant, so the
            // scroll speed is the same on a 60 Hz and a 144 Hz display.
            let now = clock.frame_time();
            let dt = match last_us.get() {
                Some(prev) => ((now - prev) as f64 / 1_000_000.0).clamp(0.0, 0.1),
                None => 0.0,
            };
            last_us.set(Some(now));

            let ramp = (overshoot.abs() / AUTOSCROLL_FULL_AT).clamp(0.0, 1.0);
            let delta = overshoot.signum() * AUTOSCROLL_MAX_PPS * ramp * dt;

            let height = content_height(view.height());
            let cur = {
                let mut buf = imp.buffer.borrow_mut();
                buf.scroll_offset(height) as f64
            };
            let next = (cur + delta).max(0.0) as u64;
            {
                let mut buf = imp.buffer.borrow_mut();
                buf.scroll_to(next, height, FOLLOW_SLOP);
            }
            view.sync_adjustment(height);

            // Extend to the pointer, clamped into the viewport — the
            // caret we want is the one at the edge we are scrolling
            // towards, not one off-screen.
            let (px, py) = imp.drag_pointer.get();
            let clamped_y = py.clamp(
                f64::from(PAD_Y),
                f64::from(view.height().max(0) - PAD_Y),
            );
            view.extend_selection_to(px, clamped_y);
            glib::ControlFlow::Continue
        });
        *imp.autoscroll_tick.borrow_mut() = Some(id);
    }
}
