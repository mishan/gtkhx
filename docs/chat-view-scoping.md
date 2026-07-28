# Chat view scoping — replacing xtext

Scoping document for a purpose-built chat rendering widget to replace the
vendored `src/xtext.c`. Sibling to `docs/inline-media-plan.md` (whose §9.E
"Option 4 — variable-height xtext" is the seed of this document) and
`docs/rust/ROADMAP.md` Phase R5.

**Status:** C0–C4 shipped; C5 (default flip, then deleting xtext) is next.

| Phase | State |
|---|---|
| C0 — `chat_view.h` seam over xtext | shipped |
| C1 — `hxchat-layout` engine | shipped |
| C2 — `hxchat-view` widget, behind `GTKHX_CHATVIEW=new` | shipped |
| C3 — selection, copy, zoom, links, context menu | shipped |
| C3r — in-buffer search, markdown compose affordances | shipped |
| C4 — inline media, word-click parity, word/line select, auto-scroll | shipped |
| C5 — default flip, delete xtext | not started |
| C5 — flip the default, delete xtext, dissolve the dispatcher | shipped |
| C6 — structured append (**incl. retiring mIRC**), avatar gutter, grouping | not started |

The parity ledger in §6a is the C5-readiness check. Sections below
describe the *design*; where the shipped code diverged from the original
plan the section says so — notably §3.6's typed signals, which were
deferred in favour of emitting xtext's `word-click` so the existing C
handlers keep working unchanged during the A/B.

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
- **Retire the mIRC escape vocabulary** (§3.8). It is GtkHx's own invention, not
  a protocol surface, and structured styling replaces it outright.
- **Markdown as the inline formatting vocabulary** (§3.9), for composing and
  reading.
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
  Markdown included: it rides the wire as literal text, needs no capability
  bit, and degrades to asterisks on every other client (§3.9).
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

**Feedback.** Zoom is otherwise silent: text changes size and nothing
says by how much, or how to get back to 100%. A badge showing the
percentage flashes for ~1.1 s on each change, drawn inside the widget's
own snapshot rather than as an overlay widget — the chat view is packed
as a bare child beside a scrollbar, so a `GtkOverlay` would mean
restructuring every container that holds one for a label that shows for
a second. Colours are the theme's foreground and background *inverted*,
so it contrasts on light and dark without a third colour to keep in
step.

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

### 3.7a One user identity across Chat and Users

The goal: right-clicking a person should behave the same whether you did it
in the Users list or on their name in chat — same menu, same handlers, one
implementation.

**Most of this already exists**, which is worth stating before proposing
anything, because the remaining work is much smaller than it looks:

- `HxMember` / `HxMemberModel` (`rust/crates/hxmodel/src/member.rs`) is
  already the authoritative per-chat membership store — that was M2 of the
  chat-model re-think.
- M4b already re-keyed the view and every UI path off the `hx_user *`
  pointer and onto `(cid, uid)`. `struct hx_user` shrank to a two-field
  signal-payload carrier.
- `user_popup_show (anchor, sess, cid, uid, x, y)` (`users.h:56`) is
  **already a single shared popover builder keyed on `(cid, uid)`**, and its
  own comment says so: "Both the standalone Users window and the pchat
  sidebars share this single popover builder."

So there is already one identity (`(cid, uid)`), one membership model
(`HxMemberModel`), and one menu builder. What is missing is only that the
chat *text* has no way to name a person: xtext gives a click a
whitespace-delimited word, so the only thing a nick click could report is a
string, and matching that string back to a member is guesswork (nicks
collide, contain spaces, and change).

The structured model fixes that by construction. `Speaker { uid, nick,
color, icon }` rides on every message (§3.1), so the view knows exactly who
a rendered nick belongs to, and §3.6's `speaker-activated` signal carries
`(cid, uid, button)` — the same pair `user_popup_show` already takes. The
wiring is then one handler:

```c
/* chat view */                     /* users list */
speaker-activated(cid, uid, btn) ─┐  ┌─ GtkGestureClick
                                  ├──┴──▶ user_popup_show(anchor, sess,
                                                          cid, uid, x, y)
```

Concretely this means, in phase order:

- **C2/C3** — the view hit-tests a `LineSource::Gutter` line box to a
  `Speaker` and emits `speaker-activated`. The gutter is already its own
  line box precisely so a click on the nick is distinguishable from a click
  on the body.
- **C6 (done for the uid half)** — `chat.c` hands over structured
  messages, and `Speaker.uid` is resolved through
  `hx_member_model_find_by_name` against the *same* `HxMemberModel` the
  user list is built from. One record per user, whichever surface you
  clicked, which is what §3.7a asked for. `Speaker.icon` and the avatar
  gutter are still to come.

  **The uid comes off the wire first.** `HTLS_HDR_CHAT` carries a UID
  chunk — `parse_chat` reads it, and `hx_chat_recv` was already using it
  for the ignore gate — it simply wasn't carried onto `HxChatEvent`, so
  the render path couldn't see it. It is now (in the padding after `cid`,
  so no other field moved). The membership lookup is the *fallback*, for
  the servers that omit the chunk and for lines that never came from a
  chat message at all.

  If both miss, uid stays 0 and stays a miss: the user may have parted,
  two users may share a name, or the "nick" may be server prose. A wrong
  uid would attach someone else's avatar and group two people's messages
  together — worse than none.
  `speaker-activated` stays unwired rather than
  guessing.
- **Later, optional** — `HxMember` becomes the thing `Speaker` *borrows*
  rather than copies, so a nick or colour change repaints chat rows as well
  as the user list. Worth doing only if it turns out we want live
  re-rendering of historical rows; copying is cheaper and messages are
  arguably historical records of who said what under what name at the time.

The thing to avoid: introducing a *third* user structure for the chat
view. `Speaker` is a render-time projection of `HxMember`, not a rival
model, and it should stay that way.

### 3.8 Retiring the mIRC escape vocabulary

**The mIRC escapes are not protocol.** Worth establishing before building
anything on top of them, because a note in `CLAUDE.md`'s theming section
asserted the opposite ("servers send specific indices; users don't get to remap
red"). They don't. Verified two ways: by tracing every generation site in the
tree, and from Misha directly — the vocabulary came in with the XChat 1.8.5
xtext fork around 2000 and was never a Hotline concept.

Provenance matters here, so: that claim was **not** a longstanding project
belief. It was introduced by an AI-assisted session in mid-2026 and sat in
`CLAUDE.md` — a file every future session reads as ground truth — for a couple
of months. It is corrected there now. The lesson worth carrying is that a
plausible-sounding rationale invented for an existing design decision is more
durable than an ordinary bug, because nothing downstream fails when it is
wrong.

The findings:

- **The Hotline wire format has no text styling.** `HTLS_HDR_CHAT` is
  `uid + flags + body`. `HTLS_HDR_MSG` is `uid + body`. News, broadcasts, file
  comments, agreements — all plain text. There is no colour field and no style
  field anywhere in the protocol.
- **Every `\003NN` byte in a buffer was written by GtkHx.** All of them, in five
  places: the nick brackets (`chat.c:627`, `msg.c:598`), the highlight wrap
  (`chat.c:669`), the `INFOPREFIX` constant, the history-muted rows and dividers
  (colour 37), and the inline-media placeholder (colour 14).
- **Only three of the eight escape codes are ever generated** — colour, bold,
  reset. Italic, strikethrough, reverse and hidden have no producer at all;
  underline appears only in divider text.
- **Hotline's real per-user colour is a separate `u32` RGB attribute** on the
  user record (`nick_color`), applied by the client when rendering a name. It
  is not, and never was, in-band markup.
- **Nothing else consumes them.** The news viewers, agreement window, user-info
  window and the broadcast dialog are all `GtkTextView` and ignore escapes
  entirely. xtext is the only consumer.
- **A server couldn't inject them anyway.** `hotline-proto`'s `strip_ansi`
  (`sanitize.rs`, mirroring the old `strip_ansi` in `protocol.h`) folds bytes
  14–30 into the printable range on every received text field.

So the escape vocabulary is a private encoding between `chat.c` and xtext, and
it can go. That is a substantial simplification, and it lands in three places:

1. **The span parser stops being a state machine over control bytes.** Nick
   colour, highlight, muted-history and the info prefix become what they
   actually are — `MessageFlags` and `Speaker.color` on the structured message
   (§3.1) — resolved by the layout engine against the theme palette. Slots
   32..37 survive as theme roles; slots 0..31 have no remaining producer once
   the last hard-coded index is gone.
2. **It pulls the structured append API forward.** §6's C6 originally deferred
   "chat.c hands a `Message`, not a mIRC string" to after xtext's deletion, on
   the theory that a string round-trip kept the A/B honest. With no
   compatibility constraint, the *native* API should be structured from C1.
   The mIRC parser survives only as a **compatibility shim** feeding the same
   `Span` output, used solely so the xtext-backed and new-widget-backed views
   render identical content during coexistence — then deleted at C5 with xtext.
3. **It frees `\003` for markdown** (§3.9) rather than having two competing
   inline formatting vocabularies.

The one thing that must not regress: hand-written escapes reaching the wire.
`chat.c` builds display strings *after* the send path, so this is already true —
but the C1 span parser should assert it, and `strip_ansi` stays exactly as it is
on the receive side regardless.

### 3.9 Markdown

With mIRC retired, markdown becomes *the* inline formatting vocabulary — for
composing as well as reading. It produces the same `Vec<Span>` the layout engine
already consumes (§3.1), so this is a new front-end on the parser, not a new
rendering path.

**Supported subset — inline only.** Chat lines are not documents:

| Syntax | Renders as |
|---|---|
| `**bold**` | bold |
| `*italic*` / `_italic_` | italic |
| `` `code` `` | monospace, background-tinted |
| `~~strike~~` | strikethrough |
| `[label](url)` | link (see security note) |
| ` ```lang ` fenced block | code block — the one block-level construct |
| `> quote` | quoted block, at line start |

**Deliberately not supported:** headings (`#` starts far too many real chat
lines), images (`![]()` — inline media has its own server-validated pipeline and
must not be bypassed by arbitrary URLs), tables, raw HTML, footnotes, reference
links, setext headings and thematic breaks (`---` is common in plain prose).
Autolinking stays with the existing URL detector rather than markdown's.

**Escaping.** Backslash escapes any construct char. Code spans suppress all
other parsing inside them. Unmatched delimiters render literally — never eat a
lone asterisk.

**Parser choice: hand-written inline scanner, not `pulldown-cmark`.**
`pulldown-cmark` is the obvious pick (pure Rust, MIT, well-tested) and was
considered. Passed over for three reasons: it has no inline-only mode, so we'd
be filtering a block-level event stream and fighting CommonMark's block rules to
suppress exactly the constructs listed above; its event stream would need
converting into byte-ranged `Span`s anyway, which is most of the work; and a
scanner for seven constructs is a few hundred lines that is exhaustively
unit-testable and behaves predictably on the pathological input chat actually
produces. This matches how the tree treats its other parsers — `hotline-proto`
is hand-written for the same reasons. Revisit if the subset grows.

**Send side — what goes on the wire is the literal text.** The protocol carries
plain text, so `**bold**` is transmitted as `**bold**`. Other GtkHx users see
bold; everyone else sees asterisks. This is exactly how Slack, Discord and IRC
clients have always behaved and needs no capability negotiation, no wire change,
and no server cooperation. It is the reason markdown is the right choice here
and a custom binary styling extension would not be.

**Receive side — the honest tradeoff.** Rendering markdown on *incoming* text
means a message typed as literal `*emphasis*` by a user on a 1997 Mac client
renders as italics. Mitigations, in order: the subset is conservative; unmatched
delimiters stay literal; and a Settings → Chat toggle (`CFG_MARKDOWN`, default
on) turns rendering off entirely for people who'd rather see exactly what was
typed. A per-message "show source" in the context menu is a cheap addition once
messages are structured.

**Security.** `[label](url)` is a phishing vector — the visible text can lie
about the destination. Three rules, all enforced in the parser, not the UI:
scheme allowlist (`http`, `https`, `ftp`, `hotline`, `mailto` — matching
`gtkurl.c`; everything else renders as literal text); the resolved URL is always
shown on hover and in the click confirmation, never just the label; and a link
whose label is itself URL-shaped but points somewhere else is flagged in the
confirmation. Fenced code blocks are inert.

**Shipped.** Rendering runs in `hxchat-view::ffi::body_blocks`, behind
`CFG_MARKDOWN` (Settings → Chat → "Render markdown", default on).

Three decisions worth recording:

*The gutter is never parsed.* A nick containing asterisks is a nick.

*Only a stylistically uniform body is parsed.* A body assembled from
several differently-styled runs is chrome the caller styled deliberately
— a divider, a `[hx]` status line — and re-parsing it would fight that.
In practice every real body is a single run: plain for live chat, muted
for history.

*The row's own colour is laid **under** the parse.* The renderer treats a
gap between spans as *default* style, not "whatever the row was", so
without this a muted history line would come back with only its bold
words muted and everything else at full contrast. There is a test.

Fenced code is inert, and deliberately not autolinked either: a URL
inside a code fence is being *shown*, not offered.

**Code needs a box, not a font.** The first cut relied on the `CODE`
attribute alone, which sets the Pango font family to Monospace — and
GtkHx's chat font is *already* monospace, so `` `code` `` rendered
identically to code with the backticks quietly deleted. Strictly worse
than not parsing it. Inline code now gets a tint behind it and fenced
blocks a tinted, outlined rounded box, both derived from the theme
foreground at low alpha so they read on light and dark without a second
colour to keep in step. The block's box is computed from its *laid-out
line boxes* rather than from separate geometry, so it cannot land
anywhere other than under the code it belongs to.

**A one-line fence is a code block.** ```` ```like this``` ```` is how
people actually type one in a chat box, because chat boxes send on
Enter. The line scanner read it as an *opening* fence, made the rest of
the line the "language", and searched for a close that never came —
yielding an empty block, i.e. a blank row where the text should have
been.

The toggle affects messages appended after it. Rows already in a buffer
keep the rendering they were built with, because re-parsing scrollback
would mean holding every row's original source text alive forever — a
permanent memory cost for a setting nobody flips twice.

**Composing.** v1 is render-on-display only — no live preview, no WYSIWYG input.
Two cheap affordances ride along: `Ctrl+B` / `Ctrl+I` / `Ctrl+Shift+C` wrap the
selection (or insert the delimiter pair), and the input box gets subdued syntax
tinting so you can see what will render. The `:shortcode:` emoji typeahead
already in the input is unaffected — emoji decoding happens before span parsing
and the two vocabularies don't collide.

**Phasing.** The parser is **C1** work (it is the span front-end, and it is pure
logic with a large unit-test surface). Rendering the resulting spans needs no
new engine support beyond the code-block and quote block variants, which land
with the other `Block` variants in **C4**. The compose affordances and the
Settings toggle come in **C3** with the rest of the interaction work.

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
| **C1** | `hxchat-layout` crate: model, **structured append API**, wrap/measure, chunked height index, scroll anchor, hit test, selection extraction, **markdown span parser (§3.9)** + the mIRC compat shim. No widget. Benchmark harness + baseline numbers vs xtext. | Yes — unit-tested library |
| **C2** | `hxchat-view` crate: the GtkWidget. `measure` / `size_allocate` / `snapshot` / `GtkScrollable`, text only. Wired to PM windows behind the env selector. | Yes — behind selector |
| **C3** | Selection, hit testing, links, context menus, autocopy, in-buffer search, scrollback trim, **zoom (§3.7)**, markdown compose affordances + Settings toggles. Enable for private chat tabs. | Yes |
| **C4** | Inline media as a real variable-height block; code-block and quote blocks; chat history as first-class row kinds with typed `load-more`. Enable for main chat. | Yes |
| **C5** | Default flip; then delete `xtext.c`, `xtext.h`, **the mIRC compat shim**, and the shim's xtext arm — separate commits. | Yes |
| **C6** | The payoff: avatar gutter, banner previews, message grouping. | Yes, incrementally |

Note on the structured append API: an earlier draft of this plan deferred it to
C6, on the theory that having the view parse the mIRC string `chat.c` already
builds kept C2–C5 an honest drop-in. §3.8 removed that constraint — the escape
vocabulary is GtkHx's own, not a compatibility surface — so the native API is
structured from C1 and the mIRC parser survives only as a compat shim, kept
purely so the two backends render identical content during the A/B, and deleted
at C5. That also means the uid reaches the renderer from the start, which is
what the avatar gutter needs.

---

## 6a. Parity ledger (C5 readiness)

The coexistence period ends when the new backend can be defaulted, so the
question that matters is "what does xtext still do that hxchat doesn't?".
`chat_view.h` is the whole parity surface — it was built from the actual
call sites, so anything not in it is dead xtext API and irrelevant.

**Done.** Construction, font, palette, word wrap, scrollback cap (with
xtext's `> 2` semantics), indent mode and cap, timestamp column and
format, the scroll adjustment, refresh, clear, append, two-column
append, insert-before-a-mark, remove, media rows with real textures and
animation, drag-select and copy, double-click word select, triple-click
line select, selection auto-scroll past the viewport edge, autocopy (all
three prefs), the indent separator *and dragging it*, keyboard paging,
zoom, links, the context menu, and `word-click` emission — which is what keeps the three existing C
handlers working unchanged.

Selection auto-scroll is worth a note: CLAUDE.md lists xtext's version as
a known *degradation*, because its scroll timers read
`xtext->select_end_y` rather than the live device position — GTK 4 has no
synchronous "where is the pointer" accessor. Storing the position from
the drag handler and consuming it from a `GtkTickCallback` is the real
answer, so this is one place the new backend is better rather than
merely equal. The rate is frame-time based, so it scrolls at the same
speed on a 60 Hz and a 144 Hz display.

Two more places the new backend is better rather than equal:

**Keyboard paging.** PgUp/PgDn have never worked in GtkHx — nothing in
the tree binds them, and the chat view is deliberately not focusable
(`gtk_widget_set_can_focus(FALSE)` in `chat.c`, so the input keeps
focus), so the focused GtkTextView swallowed the key. A global-scope
`GtkShortcut` would not help: those run *after* normal propagation, so
the TextView still wins. The binding therefore lives on the same
capture-phase root controller Ctrl+C uses, and steals the key only when
focus is in a text-entry widget — the message input or the subject entry,
where paging means nothing. The user list's `GtkColumnView` keeps its own
page-by-page navigation. Shift+PgUp/PgDn, the long-standing IRC binding,
works regardless of focus; Ctrl+Home/End jump to the top of the
scrollback and back to following the tail.

**A pinned separator.** xtext left the gutter's auto-grow enabled after a
drag, so a long nick could silently undo a narrowing the user had just
made by hand — a widening only stuck because it happened to exceed
`max_auto_indent`, which switched the auto path off as a side effect.
Here a drag pins the gutter explicitly, and nothing but an explicit
unpin releases it (not a buffer clear, not a stamp-width change). The
grab tolerance is ±4 px rather than xtext's ±1, which is unhittable on a
fractional-scale display, and the drawn rule and the hit test share one
`separator_x()` so they cannot drift apart.

**Known gaps, in rough order of how much they'd be missed:**

| Gap | Notes |
|---|---|
| Marker line | xtext tracks a last-read marker (`gtk_xtext_reset_marker_pos`, `_moveto_marker_pos`, `_check_marker_visibility`). Not exposed through `chat_view.h` and not currently called from C, so it is dead today — but it was a real feature and someone will notice its absence if it is ever rewired. |
| ~~In-buffer search~~ | **Shipped in C3r**, as a new feature rather than a port. xtext's `gtk_xtext_search` (xtext.c:5190) is a GRegex engine plus a `search_found` list threaded through the entry chain — and it has *no caller anywhere in GtkHx*. It arrived with the HexChat vendoring and has never run under GTK 4, so wiring it would have meant debugging a dead subsystem C5 deletes. The engine is `hxchat-layout::search` instead, driven through `hx_chat_view_search*`; `hx_chat_view_can_search` returns FALSE for xtext and the find bar simply isn't built there. |
| `set_urlcheck_function` | Accepted and ignored. The new backend autodetects with `gtkurl_scan` directly rather than asking a per-view classifier. Same scheme list, so the behaviour matches; the callback is simply redundant. |
| *(none currently open)* | Selection auto-scroll and word/line select were the last two; both shipped. |

**Not gaps, deliberately:** `set_show_separator` / `set_thin_separator` /
`set_error_function` / `gtk_xtext_foreach` / the `buffer_new`/`_free`/
`_show` trio are all xtext API with no caller in this tree; they die with
xtext rather than being reproduced.

### 6a2. Why retiring mIRC is C6, not C5

C5 was scoped to include §3.8's retirement of the `\003NN` escape
vocabulary, on the reasoning that a dead escape vocabulary left in the
tree is how it survives another decade. Surveying it first changed the
answer: **retiring mIRC is the structured-append API, which is C6's
core.** It is not a shim removal.

The escapes are produced at 28 sites — `chat.c` (17), `msg.c` (9),
`gtkhx.c`, `proto_helpers.c` — encoding six distinct things: nick
brackets in the speaker's colour, bold+red highlight, the dark-grey media
placeholder, history-muted rows, the `[hx]` info prefix, and
broadcastmsg's per-sender `[name]` prefix.

Two of those sites are the real argument. `chat.c:1587` and `msg.c:711`
**re-parse GtkHx's own escape output** to find where a name ends:

```c
static const char wrap_open[]  = " \00310[";
static const char wrap_close[] = "\00310]\003 ";
```

That is a data structure round-tripped through a presentation format and
parsed back out. Removing the escapes without giving the API somewhere to
put the structure would mean inventing a *different* string convention to
re-parse, which is the same mistake with fresh bytes.

The replacement is a run-based append — `(text, style)` pairs for the
gutter and the body, mapping 1:1 onto the span model `hxchat-layout`
already has — after which `mirc.rs`, `chat.c::colors[]`, both round-trip
parsers and the escapes all go at once. That is exactly the "chat.c hands
a `Message`, not a mIRC string" item §6 lists under C6, and §3.8 (below)
already argues should be pulled forward.

So C5 shipped as xtext-only, and C6 took the retirement on.

**Status after C6's first two commits.** The run API exists
(`HxChatRun`, `hx_chat_view_append_runs` /
`_insert_runs_before`) and every *chat rendering* producer now uses it:
the nick column in `chat.c` and `msg.c`, the mention highlight, all nine
chat-history row shapes, the load-older sentinel, and the inline-media
placeholder. The `\017` reset byte the highlight used to append is gone
with them — runs carry no running state, so nothing can leak into the
next row.

**Done.** The `chat-log-line` signal now carries `(htlc, cid, name,
colour, body)` instead of a pre-formatted string, so `INFOPREFIX` is the
bare string `"hx"` and broadcastmsg passes its sender name and colour as
parameters (`hx_printf_named`). The forty-odd `hx_printf_prefix` callers
are unchanged — the prefix argument simply means the tag now.

With that, the parser in `chat.c` that scanned for the closing bytes of
an escape wrapper is gone, and so is `mirc.rs`.

Two things worth noting from the last step:

**`broadcast_sanitise_name` used to be load-bearing for correctness.**
The sender's name went inside a
`" \00310[\003<col><name>\00310]\003 "` wrapper that the chat side
scanned for a closing sequence, so a name containing a raw `\003` could
terminate the wrapper early, break info-line detection, or smuggle its
own colours into the log. That is unreachable now — there is no wrapper
to escape from. The sanitiser stays because control bytes in a text
layout are still undesirable, but it has been demoted from a security
boundary to hygiene.

**Dropping `mirc::parse` from the plain-append path is a small security
improvement.** Those calls now do `ParsedText::plain`. The remaining
callers pass text that came *off the wire*, so continuing to interpret
escapes there would have let a server set colours in your chat log by
sending the bytes. It cannot: they are characters like any other.

**One dead thing remains**, flagged rather than removed:
`proto_helpers.c:742` still holds a copy of the old prefix and checks
incoming server chat against it. Nothing produces the prefix, and the
check only ever sees server-sent text, so it cannot fire. Removing it
means retiring the two proto-test cases that feed it the literal string —
a separate change.

### 6b. Notes from C3r

**Search is O(scrollback), on purpose.** `ChatBuffer::search` walks the
*model*, not the layout, so a match in a row that has never been laid
out is still found — which is the entire point, since the reason to
search is to reach the part of the scrollback you haven't scrolled to.
The cost is paid with a 120 ms debounce in the find bar rather than with
an index, because an index would have to be maintained across append,
prepend, trim and replace for a feature used seconds at a time.

**Find accelerators.** Ctrl+F opens and focuses, selecting the existing
query so typing replaces it. Pressed *again* while the entry already has
focus and a query, it advances to the next match instead — that is the
"hit Ctrl+F, type, keep hitting Ctrl+F" flow, and gating it on the entry
already being focused is what keeps the reopen-and-retype case intact.
Ctrl+G / Ctrl+Shift+G and F3 / Shift+F3 both step, because which pair is
muscle memory depends on where someone came from; two extra shortcuts is
cheaper than making them guess. All of them wrap, since `SearchState::step`
wraps at both ends.

**Two find bars, one look.** The chat find bar reuses the news panel's
highlight colours exactly (`#f6d32d` on black for hits, `#ff7800` on
white for the active one) rather than inventing its own or widening the
38-slot palette contract in `chat_view.h`.

**The tinting scanner is deliberately not the renderer.**
`markdown::scan_delims` reports ranges in the *source*, which
`parse_inline` cannot — it reports ranges in the rendered text, with the
delimiters removed. Teaching the renderer to carry source offsets
through its recursion would mean changing a heavily-tested function for
a cosmetic feature, so the tinting is a separate, shallower pass that
reuses the rules that are actually subtle (`can_open` / `can_close`
flanking, the `_` intraword guards). It doesn't nest and doesn't do
links. Being wrong in the compose box tints a character that won't
render, on text the user is still editing and can see; being wrong in
the renderer would change what a message *says*. A test pins the one
thing they must agree on: whether a delimiter is live at all.

**Double- and triple-click were broken on arrival.** The multi-click
handler set a word/row selection on press, and the drag gesture's
`drag-begin` collapsed the selection to a caret on the same press. GTK
gives no ordering guarantee between two controllers on one widget, so
the C4 comment claiming drag "fires first" was an assumption, not a
fact. Fixed by removing the conflict rather than sequencing it:
`drag-begin` now only records the press point, and the collapsed
selection is installed on first *motion* — the moment it means
something. Click-to-dismiss consequently keys on "the pointer never
moved" instead of "the selection is empty".

### 6c. The C5 gate: measure before deleting

xtext is 6,721 lines and deleting it is irreversible in practice, so the
flip wants numbers rather than impressions. `src/chat_bench.c` measures
both backends *in situ* — same binary, same window, same append path,
with `GTKHX_CHATVIEW` as the only difference:

```sh
tools/chatbench.sh 20000 3      # 20k messages, 3 repeats per backend
```

**Check the `backend` line in every report before believing any of it.**
The first run collected was worthless: `want_hxchat` accepted only
`"new"` / `"hxchat"`, the harness passed `GTKHX_CHATVIEW=1`, and both
passes therefore ran xtext — producing a complete, plausible, entirely
meaningless comparison. The selector now takes `1/true/yes/on` too and
*warns* on a value it doesn't recognise instead of silently falling back,
`want_hxchat` logs which backend it chose on every startup, and the
script fails loudly if it doesn't see a report from both. A benchmark
that can quietly measure the same thing twice is worse than no benchmark,
because it produces numbers you'll act on.

**Metrics, and how to read them.**

| Metric | What it captures |
|---|---|
| ingest | Appending N messages. **Not comparable alone** — see below. |
| first paint | The frame that pays off whatever layout was deferred. |
| **ingest + paint** | The honest cost of getting N messages on screen. Compare this. |
| reflow (width) | First frame after a width change. The most informative single number. |
| scroll frame mean / p95 | Frame time while walking the buffer a third of a page at a time. |
| RSS delta | Resident memory across the ingest phase, Linux only. |

Ingest alone is the trap. xtext line-wraps at append time
(`gtk_xtext_append_entry` → `calc_lines`); hxchat stores the message and
lays it out in the frame that needs it. Timing the append call therefore
compares "did the work" with "wrote it down", and would flatter the new
backend for a difference that is only bookkeeping. Their sum is the
comparable quantity.

Reflow is where the architectural claim in §3.2 either shows up or
doesn't: xtext re-wraps the entire scrollback on a width change, hxchat
re-wraps what is visible. If that difference isn't visible at 20k
messages, the O(visible) claim is not paying for itself and the doc
should say so.

Frame timings include GTK's own compositing, so they are comparable
*between two runs on one machine* and nowhere else. Run repeats and read
the spread; a single pair of runs cannot separate a real difference from
scheduler noise.

**Full write-up: [chat-view-benchmark.md](chat-view-benchmark.md)** —
raw samples, per-metric methodology, and a record of the two ways this
benchmark produced convincing wrong answers before it produced a right
one. Summary below.

**Results** — 20k messages, 3 repeats per backend, medians. Wayland,
one machine, one window size; see the caveats above before quoting these
anywhere else.

| Metric | xtext | hxchat | Ratio |
|---|---|---|---|
| ingest + paint | 693.1 ms | 142.6 ms | **4.9×** |
| ingest alone | 29.0k msgs/s | 143k msgs/s | 4.9× |
| relayout, worst frame | 105.6 ms | 16.9 ms | **6.3×** |
| relayout, 10-frame total | 396.6 ms | 166.3 ms | 2.4× |
| scroll frame mean | 29.9 ms | 16.7 ms | 1.8× |
| scroll frame p95 | 42.1 ms | 17.3 ms | **2.4×** |
| RSS / 10k msgs | 0.1 MB | 0.1 MB | *not credible — see below* |

**Read this way.**

*Ingest + paint*: 693 ms → 143 ms to get 20k messages on screen. The §3.2
retained-layout claim, doing what it was designed to.

*Relayout is the clearest result in the table, and the most load-bearing.*
Divide the 10-frame totals by 10: hxchat averages **16.63 ms/frame**
against a 16.67 ms vsync interval. A font change invalidates every cached
width and every wrap point in all 20k messages, and hxchat's frames after
it are indistinguishable from idle — it re-wrapped what was visible and
nothing else. xtext averages 39.7 ms/frame over the same window, ~230 ms
of extra work, with a **105 ms** worst frame. That is the
whole-scrollback re-wrap, and it is the §3.2 O(visible) claim confirmed
directly rather than argued.

One caveat: one of three xtext runs showed only a 22.6 ms worst frame
(166.8 ms total, i.e. idle). Two of three showed ~105 ms. The effect is
real but not present in every run, most likely depending on where the
invalidation lands relative to xtext's `io_tag` render timeout.

*Scroll* is what a user feels continuously rather than once. hxchat's
16.7 ms mean is the frame budget — it is vsync-bound, i.e. out of work.
xtext at 29.9 ms mean / 42.1 ms p95 is missing roughly every other frame.

*RSS is not credible and should not be quoted.* A 0.1 MB delta for 20k
messages is impossible; the message text alone is several MB. The harness
is measuring something wrong — most likely the allocator had already
grown the heap during warmup, so the pages were resident before the phase
began. Reported only so it isn't mistaken for evidence of parity. Memory
comparison remains **unmeasured**.

**Retracted.** An earlier revision of this section recorded "reflow:
16.4 ms vs 15.3 ms, no real difference" and speculated at length about
why the O(visible) claim might not be paying for itself. That measurement
was worthless: it shrank the view's `size-request`, and the chat output is
`hexpand`, so the allocation never changed and nothing re-wrapped in
either backend. Both numbers were one vsync interval. The lesson is
cheap to state and was nearly expensive: **a benchmark result that clusters
suspiciously near the frame interval is probably measuring the frame
interval.**

**Verdict for C5.** Every metric that is measured correctly shows a large
win — ingest 4.9×, relayout worst-frame 6.3×, scroll p95 2.4× — and in
two of them hxchat is simply vsync-bound, meaning the ceiling is the
display rather than the widget. Memory is unmeasured. Proceed.

## 7. Testing

- **Headless unit tests** (`cargo test`, no display) in `hxchat-layout`: wrap
  points at given widths against a fixed font; height-index queries under append
  / prepend / trim; scroll-anchor preservation across resize, prepend and trim;
  span parsing against a corpus of real mIRC strings captured from `xprintline_render`;
  URL detection parity with `gtkurl.c`; selection → text extraction including
  image alt text.
- **Benchmarks with targets.** Planned for C1 and *not built there* — C1
  shipped without them and this line went unamended, which is how a plan
  quietly becomes fiction. The harness now exists as `src/chat_bench.c`,
  driven by `tools/chatbench.sh`, and is the C5 gate (§6c).
- **Golden-render tests**: still not built. Listed here since C0; worth
  saying plainly rather than leaving as an implied "done".
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
