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
    ChatBuffer, ColorRef, LayoutParams, LineSource, Message, MessageId, ParsedText, Span, Style,
    TextMeasure,
};

use crate::measure::PangoMeasure;

/// Palette size, matching `chat_view.h`'s `HX_CHAT_PAL_COLS`.
pub const PALETTE_COLS: usize = 38;
pub const PAL_FG: usize = 34;
pub const PAL_BG: usize = 35;

/// Pixels of slop within which a scroll position counts as "at the
/// bottom" and resumes following.
const FOLLOW_SLOP: u32 = 8;

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
                    // xtext's signal, registered here purely so the
                    // seven `g_signal_connect (view, "word_click", ...)`
                    // sites in chat.c and msg.c bind without warning
                    // during the coexistence period. It is never
                    // emitted: a click yields a whitespace-delimited
                    // word, callers demux it by string prefix
                    // (`hxmedia:N`, the NBSP load-older sentinel), and
                    // replacing that with the typed signals in scoping
                    // §3.6 is C3 work.
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
                // Width change invalidates layout without recomputing
                // it; rows are re-laid-out as they become visible.
                buf.set_width(width.max(0) as u32);
            }
            obj.sync_adjustment(height.max(0) as u32);
        }

        fn snapshot(&self, snapshot: &gtk4::Snapshot) {
            self.obj().snapshot_content(snapshot);
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

    // ---- configuration ------------------------------------------------

    pub fn set_font_from_string(&self, font: &str) {
        let imp = self.imp_();
        imp.measure
            .borrow_mut()
            .set_font(pango::FontDescription::from_string(font));
        let g = imp.font_generation.get().wrapping_add(1);
        imp.font_generation.set(g);
        imp.buffer.borrow_mut().set_font_generation(g);
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
        self.imp_()
            .buffer
            .borrow_mut()
            .set_max_rows(if n > 0 { n as usize } else { 0 });
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
        imp.measure.borrow_mut().set_zoom_permille(zoom);
        imp.buffer.borrow_mut().set_zoom_permille(zoom);
        self.queue_resize();
    }

    pub fn zoom_permille(&self) -> u32 {
        self.imp_().measure.borrow().zoom_permille()
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
        self.after_content_change();
    }

    pub fn scroll_to_bottom(&self) {
        self.imp_().buffer.borrow_mut().scroll_to_bottom();
        self.after_content_change();
    }

    fn after_content_change(&self) {
        self.sync_adjustment(self.height().max(0) as u32);
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
                let h = this.height().max(0) as u32;
                imp.buffer
                    .borrow_mut()
                    .scroll_to(adj.value().max(0.0) as u64, h, FOLLOW_SLOP);
                this.queue_draw();
            });
            *imp.vadj_handler.borrow_mut() = Some(id);
        }
        *imp.vadjustment.borrow_mut() = adj;
        self.sync_adjustment(self.height().max(0) as u32);
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
        let width = self.width().max(0) as f32;
        let height = self.height().max(0);
        if height <= 0 {
            return;
        }

        // Background.
        let bg = self.imp_().palette.borrow()[PAL_BG];
        snapshot.append_color(
            &bg,
            &gtk4::graphene::Rect::new(0.0, 0.0, width, height as f32),
        );

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

        for (row, row_top) in placed {
            let Some(layout) = buf.layout_at(row) else {
                continue;
            };
            let Some(msg) = buf.message_at(row) else {
                continue;
            };

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
                        // C2 is text-only: a media row draws its
                        // placeholder, which is exactly the Phase 9.D
                        // behaviour. Real inline images are C4.
                        Some(hxchat_layout::Block::Image { alt, .. }) => (alt.as_str(), &[]),
                        None => continue,
                    },
                };
                let slice = text.get(line.range.clone()).unwrap_or("");
                if slice.is_empty() {
                    continue;
                }
                self.draw_runs(
                    snapshot,
                    &draw_layout,
                    slice,
                    line.range.start,
                    spans,
                    line.x as f32,
                    y as f32,
                );
            }
        }
    }

    /// Draw one visual line as a sequence of styled runs.
    #[allow(clippy::too_many_arguments)]
    fn draw_runs(
        &self,
        snapshot: &gtk4::Snapshot,
        layout: &pango::Layout,
        slice: &str,
        slice_start: usize,
        spans: &[hxchat_layout::Span],
        x0: f32,
        y: f32,
    ) {
        let mut x = x0;
        let mut cursor = 0usize;
        let end = slice.len();

        let emit = |text: &str, style: Style, x: &mut f32| {
            if text.is_empty() {
                return;
            }
            // Reset the shared layout rather than allocating one.
            layout.set_attributes(Some(&PangoMeasure::attrs_for(style)));
            layout.set_text(text);
            let (w, h) = layout.pixel_size();
            if style.bg != ColorRef::Default {
                snapshot.append_color(
                    &self.resolve(style.bg, PAL_BG),
                    &gtk4::graphene::Rect::new(*x, y, w as f32, h as f32),
                );
            }
            snapshot.save();
            snapshot.translate(&gtk4::graphene::Point::new(*x, y));
            snapshot.append_layout(layout, &self.resolve(style.fg, PAL_FG));
            snapshot.restore();
            *x += w as f32;
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
            if ss > cursor {
                emit(&slice[cursor..ss], Style::default(), &mut x);
            }
            if se > ss {
                emit(&slice[ss..se], s.style, &mut x);
            }
            cursor = se;
        }
        if cursor < end {
            emit(&slice[cursor..], Style::default(), &mut x);
        }
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
