# Chat view scoping — replacing xtext

Scoping document for a purpose-built chat rendering widget to replace the
vendored `src/xtext.c`. Sibling to `docs/inline-media-plan.md` (whose §9.E
"Option 4 — variable-height xtext" is the seed of this document) and
`docs/rust/ROADMAP.md` Phase R5.

**Status:** scoping. Nothing implemented.

---

## 1. Why replace it

`src/xtext.c` is 6,721 lines: a 5,168-line vendored copy of HexChat's xtext
(commit `e9bb312`, Phase 2.6) plus ~1,550 lines of GtkHx grafts. It works, it
has been through the GTK 2 → 3 → 4 climb, and it is not *bad* code. But every
feature added since Phase 9 has been added *against* its core assumption, not
with it, and the seams are now load-bearing.

### 1.1 The structural problem: a line-uniform grid

xtext's vertical layout is `fontsize × subline_count`, everywhere. Every
coordinate in the widget flows from that identity:

- `gtk_xtext_adjustment_set()` (xtext.c:919) sets `page_size = height / fontsize`
  and `upper = buf->num_lines` — **the scroll adjustment's unit is fractional
  text lines, not pixels.**
- `gtk_xtext_find_char()` (xtext.c:1339) hit-tests with
  `line = (y + pixel_offset) / fontsize`.
- `gtk_xtext_render_page()` (xtext.c:4594) derives its sub-line scroll offset as
  `pixel_offset = (adj_value - floor(adj_value)) * fontsize`.
- Line counting is `g_slist_length(ent->sublines)` at a dozen call sites.

Inline media (Phase 9.E) had to be smuggled into that grid. `gtk_xtext_lines_taken()`
(xtext.c:4325) reserves `ceil(rendered_h / fontsize)` blank sublines and
`gtk_xtext_render_media_line()` (xtext.c:3912) paints the texture across the
band. The known costs are already recorded in `inline-media-plan.md`:

> selection-drag passes through blank padding rows; marker draws above the band
> rather than across the image. Both fall out of the line-uniform grid model.
> Option 4 (variable-height xtext) remains the natural follow-up if the
> compromises chafe.

Every future block-shaped thing — a banner preview, an avatar in the gutter, a
quoted reply, a file card — hits the same wall.

### 1.2 The performance problems

Measured from the code, not from a profiler (benchmarking is a C1 deliverable):

1. **Double parse.** `gtk_xtext_strip_color()` (xtext.c:2972) walks the message
   at append time and builds `ent->slp`, a list of `offlen_t` runs. Then
   `gtk_xtext_render_str()` (xtext.c:3292) **ignores `slp` and re-parses the
   mIRC state machine byte-by-byte on every render pass.** `slp` is only
   consulted for width math and search marking.
2. **Per-character Pango measurement.** `find_next_wrap()` (xtext.c:3685) calls
   `backend_get_text_width_emph()` (xtext.c:638) per character, and that does a
   `pango_layout_set_text()` + `pango_layout_get_pixel_size()` round trip each
   time. Runs of identical emphasis are not batched.
3. **O(total scrollback) reflow.** `gtk_xtext_calc_lines()` (xtext.c:4395) walks
   *every* entry calling `gtk_xtext_lines_taken()` on each. A window resize or a
   font change re-wraps the entire buffer — with a `xbuf_max` of several thousand
   lines that is a visible hitch.
4. **No retained layout.** Nothing shaped is kept between frames. Every visible
   line is re-measured and re-shaped every render.
5. **cairo, not GSK.** The GTK 4 port kept cairo via `gtk_snapshot_append_cairo()`
   (a deliberate, correct Phase 4 decision — it preserved the Phase 3.4b work).
   Native `gtk_snapshot_append_layout()` / texture nodes hand GSK real render
   nodes and let the GPU composite them; `users_cell.c` already demonstrates the
   pattern in-tree.
6. **O(n) scans on interaction.** `gtk_xtext_find_media_entry_by_token()`
   (xtext.c:6386) linear-scans the buffer per media click.

### 1.3 The API problems

The widget is not encapsulated, and the leaks are exactly what makes it hard to
swap or to port to Rust:

- Callers reach into the struct: `chat.c` / `msg.c` / `options.c` write
  `xtext->wordwrap`, `xtext->max_lines`, `xtext->urlcheck_function`, and read
  `xtext->buffer` and `xtext->adj` directly.
- **`chat.c` holds raw `textentry *` pointers.** `struct hx_chat_history_render`
  keeps `anchor_ent` and `load_older_ent` as live pointers into xtext's internal
  linked list. This is the stated reason that struct "stays C" in the R5 chat-model
  re-think — the pointers cannot cross into Rust.
- **Control flow is encoded in strings.** Three `word_click` handlers are chained
  on every chat tab (`gtkurl_xtext_word_click`, `chat_history_word_click`,
  `inline_media_chat_word_click`) and each one demuxes by matching a string
  prefix: `hxmedia:N` for a media click, and a non-breaking-space-padded
  "↑ Load older messages ↑" sentinel for history paging. A click target is a
  word, so anything clickable has to be spelled as a magic word.
- **Message semantics are destroyed at the door.** `xprintline_render()`
  (chat.c:599) flattens everything — nick, colour, highlight, history-muted
  styling, media placeholders — into one mIRC-escaped byte string. Downstream,
  a line is bytes. There is nowhere to hang a uid, a message id, an avatar, or a
  hit region, because a *message* is not a thing xtext knows about.

That last point is the one that blocks what we actually want next.

---

## 2. Goals and non-goals

**Goals**

- Pixel-based variable-height layout. An image, an avatar row, and a two-line
  wrapped message are all just heights.
- Parse once at append; render from a structured representation.
- Retained, invalidation-keyed layout cache. Resize and scroll cost work
  proportional to what is *visible*, not to scrollback size.
- A real message model: id, timestamp, speaker (with uid), typed blocks, flags.
  Extensible by adding a block variant, not by adding a string prefix.
- Typed signals for interaction — no string-prefix demux.
- Headless-testable: wrapping, height indexing, scroll anchoring, span parsing
  and selection extraction all unit-tested in CI without a display.
- **Zoom in / out as a first-class feature** (§3.7). xtext has none today; it is
  the single most valuable accessibility affordance this widget can offer.
- Rust, as a `glib::subclass` widget, exporting a C ABI so the existing C call
  sites link unchanged during migration.
- **Visually identical output at v1.** The layout model is built for richer
  layouts; the pixels don't change until we choose to change them.

**Non-goals**

- No wire-protocol change of any kind. This is client-side rendering only.
- Not a general-purpose rich-text widget. It renders Hotline chat.
- Not replacing the chat *input* — that stays `GtkTextView`.
- Not reproducing xtext's every pref. Anything unused gets dropped deliberately
  and listed, not carried forward by inertia.

---

## 3. Architecture

Three layers, so the interesting parts are testable without a display.

```
┌──────────────────────────────────────────────────────────────┐
│ src/chat_view.h  —  C ABI shim (hx_chat_view_*)              │
│ chat.c / msg.c call this; it dispatches to xtext or hxchat    │
└──────────────────────────────────────────────────────────────┘
                              │
┌──────────────────────────────────────────────────────────────┐
│ rust/crates/hxchat-view  —  gtk4-rs glib::subclass GtkWidget │
│ WidgetImpl {measure, size_allocate, snapshot}, Scrollable,    │
│ event controllers, GSK render nodes, typed signals            │
└──────────────────────────────────────────────────────────────┘
                              │
┌──────────────────────────────────────────────────────────────┐
│ rust/crates/hxchat-layout  —  pure layout engine, no widget  │
│ message model, span parser, wrap/measure, height index,       │
│ scroll anchor, hit test, selection extraction                 │
│ depends on `pango` (+ pangocairo fontmap) — NOT on `gtk4`     │
└──────────────────────────────────────────────────────────────┘
```

Splitting `hxchat-layout` out matters: Pango can shape text headlessly against a
`pangocairo` font map, so the entire layout engine runs under `cargo test` on
display-less CI. That is coverage xtext has never had.

### 3.1 Message model

```rust
pub struct Message {
    pub id: MessageId,                  // monotonic, local; stable across reflow
    pub kind: MessageKind,
    pub ts: SystemTime,
    pub speaker: Option<Speaker>,
    pub blocks: Vec<Block>,
    pub flags: MessageFlags,            // HIGHLIGHT | MUTED | ACTION | SELF
}

pub enum MessageKind {
    Live,
    History { server_message_id: u64 },
    Divider,                            // "── chat history (N) ──"
    Action(LoadMoreDirection),          // the "Load older" row, as a row kind
    System,
}

pub struct Speaker {
    pub uid: u16,
    pub nick: String,
    pub color: NickColor,
    pub icon: IconRef,                  // cicn id | GIF avatar | none
}

pub enum Block {
    Text { runs: Vec<Span> },
    Image { paintable: ImageRef, intrinsic: (u32, u32), alt: String },
    // future: BannerPreview, Quote, FileCard, Reactions
}

pub struct Span {
    pub range: Range<usize>,            // byte range into the block's text
    pub fg: ColorRef, pub bg: ColorRef, // palette slot or explicit RGBA
    pub attrs: Attrs,                   // bold|italic|underline|strike|reverse|hidden
    pub link: Option<LinkId>,
}
```

The mIRC escape sequences and URL detection are resolved **once**, at append,
into `Span`s. The renderer never sees a `\003`.

### 3.2 Layout cache and invalidation

Each message carries `Option<LayoutCache>`:

```rust
struct LayoutCache {
    gen: LayoutGeneration,   // (width_px, font_gen, theme_gen)
    height_px: u32,
    lines: Vec<LineBox>,     // y, height, byte range — for hit testing
    shaped: Vec<pango::Layout>,
}
```

A width change, font change or theme change bumps the generation. Caches are
*not* eagerly rebuilt — they're rebuilt lazily when a message is next laid out.
Resize therefore costs O(visible), not O(scrollback). This is the single
biggest departure from `gtk_xtext_calc_lines()`.

### 3.3 Height index — chunked prefix sums

Variable heights need an O(log n) "what is at pixel Y" and a running total. A
plain Fenwick tree is the textbook answer but handles neither front-insertion
(chat-history prepend) nor front-removal (scrollback trim) well — and we do both.

Design: messages live in fixed-size chunks (128 each) inside a `VecDeque<Chunk>`.
Each chunk caches its own summed height; a top-level running prefix over chunks
is repaired lazily from the first dirty chunk.

- Query pixel→message: binary search chunk prefixes, then linear scan ≤128 within.
- Append: touch one chunk + one prefix tail.
- Prepend a history batch: push chunks at the front of the `VecDeque`.
- Trim to `max_lines`: pop chunks off the front.

**Unmeasured messages get an estimate.** A chunk records whether each height is
measured or estimated (from byte length and current width). Scrolled-away
messages are never shaped. The scrollbar `upper` is therefore an estimate that
converges as content is actually laid out. The honest cost of that is thumb drift
— which is why scroll position is not stored in pixels:

### 3.4 Scroll anchoring

```rust
struct ScrollAnchor {
    message: MessageId,
    offset_px: i32,          // within that message
    gravity: Gravity,        // Bottom (stick) | Free
}
```

The `GtkAdjustment` value is *derived* from the anchor, never the source of
truth. Consequences, all of which xtext hand-patches today:

- Stick-to-bottom is `gravity == Bottom`, not a `scrollbar_down` flag plus
  bookkeeping.
- Prepending a history batch cannot make the view jump — the anchor names a
  message, and that message didn't move. `gtk_xtext_insert_indent_before()`
  (xtext.c:5720) currently hand-adjusts `pagetop_line` / `pagetop_subline` to
  approximate this.
- Resize preserves reading position exactly, even though every height changed.
- Height-estimate corrections shift the thumb, never the content.

The adjustment's unit is **pixels**, with `page_size = widget height`.

### 3.5 Rendering

`snapshot` queries the index for the visible range, ensures each visible
message's `LayoutCache`, and emits GSK nodes:

- Text: `gtk_snapshot_append_layout()` per shaped layout — same call
  `users_cell.c:hx_user_cell_name_snapshot()` already uses.
- Images / avatars: `gdk_paintable_snapshot()` or a texture node.
- Selection and highlight bands: colour nodes behind the text.

No `append_cairo()`. Everything is a render node GSK can batch and the GPU can
composite.

### 3.6 Interaction

Hit testing is index → message (O(log n)) → `LineBox` → `pango::Layout::xy_to_index`.

Signals replace the string-prefix demux:

| Signal | Payload | Replaces |
|---|---|---|
| `link-activated` | url, button, modifiers | `word_click` + `urlcheck_function` |
| `message-activated` | message_id, block_index, button | `hxmedia:N` prefix match |
| `load-more` | direction | the NBSP "↑ Load older ↑" sentinel match |
| `speaker-activated` | uid, button | (new — enables avatar/nick click) |
| `selection-changed` | — | autocopy plumbing |

Drag-select autoscroll is a `GtkTickCallback` driven from the last
`GtkEventControllerMotion` coordinates. That is the correct GTK 4 answer to the
degradation CLAUDE.md records ("scrollup/down timers read `xtext->select_end_y`
rather than the live device position") — the tick callback runs per frame, so the
staleness window is one frame rather than one timer period.

Selection across an image block contributes the block's alt text to the copied
string, rather than today's all-or-nothing behaviour.

### 3.7 Zoom

xtext has no zoom. The only way to change chat text size today is Settings →
the chat font pref, which is a modal round trip and doesn't touch anything but
the glyphs. A purpose-built widget should do better, and the layout design
already has the hook: `LayoutGeneration` is keyed on `(width_px, font_gen,
theme_gen)`, so zoom is just a scale factor that bumps `font_gen`.

```rust
struct ZoomState {
    level: f64,              // 1.0 = 100%; clamped to [0.5, 4.0]
    steps: &'static [f64],   // 50 67 80 90 100 110 125 150 175 200 250 300 400
}
```

**What scales.** Zoom is a *view* scale, not a font-size change — everything in
the row scales together so the layout stays proportionate:

- text (font description size × level, in Pango units so it stays hinted)
- the timestamp gutter and nick column widths
- inline media blocks, avatars, banner previews
- indent / padding / the separator position

**Bindings.** `Ctrl` + `+` / `-` / `0`, and `Ctrl` + scroll wheel — the
conventions every browser and terminal already trains people on. Implemented as
a `GtkShortcutController` on the view plus a `GtkEventControllerScroll` checking
for the Ctrl modifier, so it works whether focus is in the output or the input
box. A `zoom-changed` signal lets the window surface the current level
transiently (an `AdwToast`, or the existing toast overlay).

**Interaction with the scroll anchor.** Zooming changes every height in the
buffer, which is exactly the case §3.4's anchor already handles: the anchored
message stays put and the content grows around it. Without anchor-based
scrolling, zoom would fling the viewport — this is a concrete second payoff from
that design decision, and it's why zoom lands cheaply here and would have been
painful to retrofit into xtext.

**Persistence and scope.** Per-view at runtime; the level persists to a pref
(`CFG_CHAT_ZOOM`) as the startup default for every chat surface. Per-tab
override is a possible refinement, deliberately not v1.

**Relationship to the rest of the app.** This is *chat-view* zoom, distinct from
`GtkhxTheme`'s five `GTKHX_SCALE_*` areas (which are static per-theme structural
factors for toolbar / user-list / task icons) and from the desktop-wide
`text-scaling-factor`. The view honours the GTK text scale as its baseline and
applies zoom on top, so a user with a system-wide scale set doesn't get it
multiplied away.

**Phasing.** Zoom lands in **C3**, alongside the other interaction work — the
layout engine must support it from C1 (the generation key and the scale factor
are part of the measure API from day one), but the bindings and the pref come
with selection and context menus.

---

## 4. What this unlocks

The block/gutter model is the point of the exercise.

```
┌────────┬──────────────┬────────────────────────────────────┐
│ gutter │ nick column  │ body blocks                        │
│ 12:04  │ ⟨alice⟩      │ hey, look at this                  │
│ [icon] │              │ ▓▓▓▓▓ image block ▓▓▓▓▓            │
│        │              │ (grouped follow-up, no nick)       │
└────────┴──────────────┴────────────────────────────────────┘
```

- **User icon next to the speaker.** `Speaker.icon` resolves against
  infrastructure that already exists and is already main-thread and already
  cached: `gtkhx_avatar_get(uid)` returns a borrowed `GdkTexture *` for the
  fogWraith GIF avatar (Phase 10), and `load_icon(widget, icon, ifn, recurse,
  &pixbuf_out, &mask_out)` in `cicn.c` fills a `GdkPixbuf *` out-parameter for
  the classic 16-bit icon id. Both snapshot as paintables.
  Animated avatars come nearly free — `gif_avatar.c` already runs one shared
  frame timer for the whole app and exposes `gtkhx_avatar_is_animated()` /
  `gtkhx_avatar_is_paused()`.
- **Banner-format icon preview.** The wide-banner convention already exists in
  `users_cell.c` (`HX_USER_WIDE_ICON_LEFT_PAD`, left ~200px reserved). In a chat
  row that becomes either a wider gutter or its own `Block::BannerPreview` —
  a layout decision, not a widget rewrite.
- **Message grouping.** Consecutive messages from one uid within N seconds
  collapse the nick column and gutter. Pure layout-model logic, unit-testable,
  zero protocol impact.
- **Inline media as a real block.** Actual pixel height, selection that behaves,
  a marker line that draws across the image instead of above the band.
- **Later, cheaply:** quoted replies, reactions, file cards, per-message context
  menus — each is one `Block` variant plus a measure/snapshot arm.

None of this is v1. All of it is one variant away after v1.

---

## 5. Migration and coexistence

Both widgets compiled in; selectable at runtime; xtext deleted last.

### 5.1 `src/chat_view.h` — the compatibility seam

A thin C header exposing the ~20 operations `chat.c` and `msg.c` actually
perform, dispatching to either backend. Building it is worthwhile **even if the
new widget never ships**, because it forces two cleanups:

1. **Close the struct leaks.** `->wordwrap`, `->max_lines`, `->urlcheck_function`,
   `->buffer`, `->adj` become `hx_chat_view_set_*` / `_get_*` calls on both
   backends.
2. **Opaque marks.** The raw `textentry *` in `struct hx_chat_history_render`
   (`anchor_ent`, `load_older_ent`) become an opaque `HxChatMark` handle — a
   `textentry *` under xtext, a `MessageId` under the new widget. This is the
   invasive one, and it is also precisely what R5's chat-model re-think named as
   the blocker keeping that struct in C. It pays twice.

### 5.2 Selector

`GTKHX_CHATVIEW=new|xtext` env var (developer A/B against live servers) →
hidden pref once it's usable → default flip → delete.

### 5.3 Landing order

Private message windows → private chat tabs → main chat. PM windows are the
smallest surface: no inline media, no chat history, no load-older sentinel, one
`word_click` handler. Real burn-in with a small blast radius.

---

## 6. Phasing

| Phase | Scope | Ships on its own? |
|---|---|---|
| **C0** | `chat_view.h` shim over xtext only. Close the five struct leaks; opaque `HxChatMark`. No new widget, no behaviour change. | Yes — pure cleanup |
| **C1** | `hxchat-layout` crate: model, span parser, wrap/measure, chunked height index, scroll anchor, hit test, selection extraction. No widget. Benchmark harness + baseline numbers vs xtext. | Yes — unit-tested library |
| **C2** | `hxchat-view` crate: the GtkWidget. `measure` / `size_allocate` / `snapshot` / `GtkScrollable`, text only. Wired to PM windows behind the env selector. | Yes — behind selector |
| **C3** | Selection, hit testing, links, context menus, autocopy, in-buffer search, scrollback trim, **zoom (§3.7)**. Enable for private chat tabs. | Yes |
| **C4** | Inline media as a real variable-height block; chat history as first-class row kinds with typed `load-more`. Enable for main chat. | Yes |
| **C5** | Default flip; then delete `xtext.c`, `xtext.h`, and the shim's xtext arm — separate commits. | Yes |
| **C6** | The payoff: structured append API (`chat.c` hands a `Message`, not a mIRC string), avatar gutter, banner previews, message grouping. | Yes, incrementally |

Note C6's structured-append: at v1 the view parses the mIRC string `chat.c`
already builds, so C2–C5 are a genuine drop-in and the A/B is honest. C6 removes
the string round-trip and is what actually lets a uid reach the renderer.

---

## 7. Testing

- **Headless unit tests** (`cargo test`, no display) in `hxchat-layout`: wrap
  points at given widths against a fixed font; height-index queries under append
  / prepend / trim; scroll-anchor preservation across resize, prepend and trim;
  span parsing against a corpus of real mIRC strings captured from `xprintline_render`;
  URL detection parity with `gtkurl.c`; selection → text extraction including
  image alt text.
- **Golden-render tests**: render a fixed message list at a fixed width and font
  into a texture via an offscreen `gsk::CairoRenderer` (works without a display)
  and compare hashes. Pixel-level regression coverage in CI — xtext has none.
- **Benchmarks with targets**, recorded in C1 against xtext as the baseline:
  append throughput (msgs/s), cold reflow after resize with 50k messages,
  steady-state scroll frame time, resident memory per 10k messages.
- **Tier 3 unaffected** — the protocol layer isn't touched. The existing chat /
  chat-history / inline-media integration tests keep passing against both
  backends, which is the strongest correctness argument the coexistence period
  buys us.

---

## 8. Risks and open questions

- **First Rust custom-drawn widget in the tree.** Nothing in `rust/crates/`
  currently implements `WidgetImpl::snapshot`, `measure`, `size_allocate` or
  `ScrollableImpl` — every existing Rust GObject is a data-model row or a
  composed widget tree. Budget for gtk4-rs subclass friction; the
  `gtk4::set_initialized()` trap (`gtkhx-ui/src/lib.rs:176`) is precedent for the
  category. **Spike this first in C2**: confirm `ScrollableImpl` is usable from a
  subclass at the pinned gtk4-rs 0.10, and if not, fall back to owning
  `hadjustment` / `vadjustment` properties manually — which is what xtext
  effectively does anyway.
- **Estimated-height scrollbar drift.** Mitigated by anchor-based scrolling plus
  an idle-time measure backfill for off-screen messages. Worth an explicit
  acceptance test.
- **Pango shaping is the performance floor.** Cache aggressively; measure runs,
  never characters (xtext's per-char measurement is the thing to not repeat).
- **Accessibility.** xtext exposes nothing today, so shipping without it is not a
  regression — but a fresh custom widget is the moment to decide whether to
  implement `GtkAccessible` / `GtkAccessibleText`. Flag as a scoped decision, not
  a silent omission. Zoom (§3.7) is committed to in C3 regardless, since it is
  the highest-value accessibility affordance per line of code and the layout
  design makes it nearly free.
- **Scope creep.** C2–C5 must be pixel-identical to xtext. Everything that
  changes what chat *looks like* lives in C6, after the old widget is gone.
- **Roadmap conflict.** `docs/rust/ROADMAP.md` locked-in decision #6 reads:

  > **Custom widgets stay vendored C: xtext (HexChat's modern fork) does not
  > get rewritten.** It is 4,500 LOC of cairo + Pango + mIRC color parsing that
  > HexChat maintains and we benefit from for free. Wrapping it as a gtk-rs
  > subclass when the rest of the UI is in Rust is acceptable; rewriting it is
  > not in scope.

  This document deliberately re-opens that decision, and the decision's own
  premises are what have expired. It says "4,500 LOC"; the file is now 6,721.
  It says "HexChat maintains and we benefit from for free"; ~1,550 of those
  lines are GtkHx-only — inline media (~760), chat-history insert/remove (~370),
  autocopy/timestamp setters, the two extra palette roles — and HexChat will
  never maintain any of it for us. Nothing about the *rest* of the decision list
  is disturbed. If this plan proceeds, decision #6 should be amended in place
  with a pointer here rather than quietly ignored.

---

## 9. Suggested first step

**C0.** It's a self-contained cleanup with no new widget: introduce
`src/chat_view.h`, route `chat.c` / `msg.c` / `options.c` through it, close the
five direct-struct-access leaks, and make the two chat-history render cursors
opaque handles. Green build, no behaviour change, and it improves the tree
whether or not C1 ever starts.
