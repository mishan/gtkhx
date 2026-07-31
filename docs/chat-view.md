# The chat view

Reference for the chat rendering subsystem: the surface every chat,
private-chat and private-message window draws its output on.

It replaced a vendored copy of HexChat's xtext widget. The measured
comparison that justified deleting xtext is
[chat-view-benchmark.md](chat-view-benchmark.md); that comparison cannot
be repeated, because only one backend exists now.

---

## 1. The three layers

```
src/chat_view.h            the C ABI. chat.c / msg.c / options.c call
                           these. Declarations only — there is no .c file.
        │
rust/crates/hxchat-view    gtk4-rs glib::subclass GtkWidget: measure /
                           size_allocate / snapshot, GtkScrollable, event
                           controllers, GSK render nodes, Pango measuring.
        │
rust/crates/hxchat-layout  pure layout engine, no widget: message model,
                           span parsers, wrap/measure, height index,
                           scroll anchor, hit test, selection, search.
                           No dependencies at all.
```

`src/chat_view.h` is a *declaration* header, not a forwarding layer. The
symbols it declares are exported from `hxchat-view/src/ffi.rs` and C links
against them directly. It was briefly a real dispatcher, while the new
widget coexisted with xtext behind a runtime switch; the seam did its job
— by the end xtext was referenced from that one file and nowhere else in
the tree, which is what made deleting it a mechanical change rather than
an archaeology project.

Two properties of that seam are worth not giving back. **No struct-field
access:** callers once wrote `GTK_XTEXT(w)->wordwrap`, `->max_lines`,
`->urlcheck_function` and read `->buffer` and `->adj`; everything goes
through `hx_chat_view_set_*` / `_get_*` now, so the implementation owes
callers behaviour rather than layout. **No raw entry pointers:** the
chat-history render cursors in `struct hx_chat_history_render` used to be
live `textentry *` into xtext's internal list, and are opaque
`HxChatMark` handles backed by a message id. Marks are weak references —
`hx_chat_view_remove` on a stale one is a safe no-op returning `FALSE`,
and that is the intended way to find out it went stale.

### `hxchat-layout` has no dependencies

Not gtk4, not glib, **not pango**. Text measurement — the one thing that
genuinely needs a font stack — is abstracted behind the `TextMeasure`
trait in `measure.rs`. The widget supplies a Pango-backed implementation;
the crate's own tests supply `FixedMeasure`, where every character is
exactly N pixels wide, which makes wrap assertions exact and readable
instead of font-dependent and brittle. So the whole engine — wrapping,
height indexing, scroll anchoring, span parsing, selection extraction,
search — runs under `cargo test` on display-less CI. That is coverage
xtext never had.

The trait is also a hedge against xtext's worst performance bug. Its
`find_next_wrap` called a width function **per character**, and that did a
`pango_layout_set_text` + `pango_layout_get_pixel_size` round trip each
time. `TextMeasure`'s unit of work is a *run*, never a character, so an
implementation physically cannot repeat that mistake.

---

## 2. The message model

A row is a value with fields, not a byte string. Under xtext a chat line
was bytes with in-band escapes, and a uid, a message id, an avatar or a
hit region had nowhere to live — which is why every feature added late had
to be smuggled in as a magic word (`hxmedia:N`) or a magic
non-breaking-space sentinel. `hxchat-layout::message` carries:

- **`MessageKind`** — `Live`, `History { server_message_id }`, `Divider`,
  `LoadMore(direction)`, `System`. `LoadMore` and `Divider` were ordinary
  text rows under xtext whose meaning was recovered by string-matching the
  rendered bytes: the history handler compared the clicked word against a
  composed "↑\u{a0}Load\u{a0}older\u{a0}messages" sentinel, non-breaking
  spaces and all, because xtext's tokenizer splits on ASCII space.
- **`Speaker { uid, nick, color, icon }`**. `color` is Hotline's real
  per-user colour — a `0x00RRGGBB` attribute on the user record.
- **`Block`** — `Text(ParsedText)`, `Code { text, language }`,
  `Quote { content, depth }`, `Image { token, size, alt }`. Adding a kind
  of content is adding a variant plus a measure arm and a snapshot arm.
  Under xtext, inline media needed a discriminator on `textentry`, a
  side-allocated media struct, a parallel render path, and a padding hack
  in the line-count math.
- **`MessageFlags`** — highlight, muted, action, outgoing, deleted.

The image block deliberately holds no `GdkTexture`: the layout engine only
needs the size, and a texture cannot cross into a GTK-free crate. The view
keys its own texture table off `token`. `size` is `None` until the decode
lands, and the block measures as its `alt` text until then.

### Runs: how C hands style over

C builds a row from `HxChatRun` arrays — `(text, palette index, attrs)` —
one array for the gutter, one for the body, borrowed for the duration of
the call and built on the stack. `hx_chat_view_append_runs` takes a
speaker; `_append_system_runs` is the same shape without one and marks the
row as a system message (see grouping below for why that matters);
`_insert_runs_before` is the history-backfill form.

Named palette slots (`HX_CHAT_INFO_COLOR`, `HX_CHAT_HIGHLIGHT_COLOR`,
`HX_CHAT_PLACEHOLDER_COLOR`, …) exist because these were once bare numbers
inside printf format strings, which is how a colour choice ends up
undocumented and unsearchable.

### Speaker identity is the user list's identity

`HxChatSpeaker.uid` is the Hotline user id, `0` when unknown — which is
the honest answer more often than it looks. Hotline chat is a *text
stream*: a chat line carries a name, and the uid comes from the
`HTLS_HDR_CHAT` UID chunk when the server sends one, or from a nick lookup
against the conversation's membership model when it doesn't. That lookup
can miss — the user parted, two users share a name, the "nick" is server
prose — and a wrong uid is worse than none: it would attach someone else's
avatar and group two people's messages together. So uid stays 0 and stays
a miss.

The lookup goes through `hx_member_model_find_by_name` against the *same*
`HxMemberModel` the user list is built from. One user, one record,
whichever surface you clicked. `Speaker` is a render-time projection of
`HxMember`, not a rival model, and the thing to avoid is a third user
structure. On a right-click over a nick or avatar the view emits
`speaker-menu (uid, x, y)` and stops there; `chat.c` answers by calling
`users.c::user_popup_show`, the same builder the Users window and the
pchat sidebars use, so chat and the user list pop the same menu rather
than two that have to be kept in step.

### Grouping and the avatar gutter

Consecutive messages from one speaker collapse the nick column, and only
the group head draws an avatar. `Message::group_key()` keys on **both**
the uid and the *rendered gutter text*, and each half catches a case the
other misses: the uid separates two people who happen to share a nick, and
the rendered nick separates one person before and after a rename. The uid
survives a rename, so keying on it alone would group the messages and the
new name would simply never appear — worse than repeating it, since the
change is exactly what the reader needs to see.

System rows never group. They share a gutter (`[hx]`) without sharing a
speaker, so keying on the drawn nick would collapse "connecting",
"connected", "login ok" into one block under a single tag and read as one
event rather than three. That check is on the *kind*, not on
`speaker.is_none()`: a pre-1.5 server sends chat with no uid, and those
rows are real messages from a real person that should still group by nick.

The gap that breaks a run defaults to five minutes — short enough that a
burst collapses under one name, long enough that coming back to a room
shows who is talking rather than attaching your message to something you
said an hour ago. Continuation rows still *reserve* the gutter width, so a
run forming does not shift the column.

Avatars resolve through `src/chat_avatar.h`, which shares the user list's
precedence rule: a fogWraith GIF avatar wins over the classic cicn icon
id. Duplicating that precedence would mean chat and Users disagreeing
about which icon a user "has". The texture is borrowed and only until the
next call — animated avatars advance on a shared frame timer, so the view
asks per draw rather than caching a frame that would freeze. Rows whose
speaker is unknown get no slot at all; there would be nothing to look up.

---

## 3. Layout, the height index, and scroll anchoring

Each row carries an optional `LayoutCache`: a `LayoutGeneration` key
(width, font, theme, zoom), a pixel height, the `LineBox`es for hit
testing, the gutter width the row naturally wanted, and the avatar slot if
it has one. A width, font, theme or zoom change bumps the generation.
Caches are *not* eagerly rebuilt — they are rebuilt lazily when a row is
next laid out, so a resize costs O(visible) rather than O(scrollback).
That is the single biggest departure from xtext's `gtk_xtext_calc_lines`,
which walked every entry on every width change.

### Chunked prefix sums, not a Fenwick tree

Variable heights need an O(log n) "what is at pixel Y" and a running
total. A Fenwick tree is the textbook answer for prefix sums with point
updates, and it is the wrong shape here: it is indexed from a fixed
origin, and this buffer grows at *both* ends (chat-history backfill
prepends) and shrinks at the front (scrollback trim). Every prepend would
renumber the whole tree.

Instead rows live in fixed-target-size chunks in a `VecDeque`, each chunk
caching its own summed height, with a lazily-repaired running prefix over
the chunks.

- Query pixel→row: binary search the chunk prefixes, then scan within one
  chunk, bounded by the chunk target.
- Append: touch one chunk plus the prefix tail.
- Prepend a history batch: push chunks at the front.
- Trim to `max_lines`: pop chunks off the front.

A chunk that grows past a split threshold after middle inserts is split,
so the within-chunk scan stays bounded.

**Unmeasured rows report an estimate** rather than forcing a measure; the
index records per row whether the height is real or estimated. Rows that
have never been on screen are never shaped. The honest cost is that the
scrollbar's extent is approximate until estimates are replaced — which is
survivable precisely because scroll position is not stored in pixels.

### The anchor

The scroll position is `(row, offset within it, gravity)`, and the
`GtkAdjustment` value is *derived* from it, never the source of truth.
This is the most load-bearing design decision in the engine. A raw pixel
scroll value is only meaningful relative to a particular set of row
heights, and everything interesting that happens to a chat buffer changes
those heights: a resize re-wraps, a zoom rescales, an image finishes
decoding and a 16-pixel placeholder becomes 240 pixels, a history batch
prepends rows above the viewport, a trim drops rows off the top.

Consequences, all of which xtext hand-patched case by case:

- Stick-to-bottom is `gravity == Bottom`, not a flag plus bookkeeping.
- Prepending a history batch cannot make the view jump — the anchor names
  a row, and that row did not move. xtext's insert path bumped
  `pagetop_line`, `last_pixel_pos`, `old_value` and the adjustment by the
  inserted row's subline count to approximate this, and the trim path did
  the mirror-image decrement.
- Resize preserves reading position exactly, even though every height
  changed. A font change under xtext just accepted the jump.
- Height-estimate corrections shift the thumb, never the content. Thumb
  drift is survivable; content jumping is not.

The adjustment's unit is **pixels**, with `page_size = widget height`.
xtext's unit was fractional text lines.

### Rendering

`snapshot` queries the index for the visible range, ensures each visible
row's `LayoutCache`, and emits GSK nodes — shaped layouts for text,
paintables or texture nodes for images and avatars, colour nodes behind
the text for selection and highlight bands. No `append_cairo()`.

---

## 4. Markdown

Markdown is the inline formatting vocabulary. It produces the same spans
the layout engine already consumes, so it is a front-end on the parser,
not a rendering path. It is gated on `CFG_MARKDOWN` (the "Render
markdown" switch in Settings, default on), which is process-wide — one
checkbox, and every chat surface should agree.

**Supported subset — inline, plus two block constructs.** Chat lines are
not documents:

| Syntax | Renders as |
|---|---|
| `**bold**` | bold |
| `*italic*` / `_italic_` | italic |
| `` `code` `` | monospace, background-tinted |
| `~~strike~~` | strikethrough |
| `[label](url)` | link (see security below) |
| ` ```lang ` fenced block | code block |
| `> quote` | quoted block, at line start |

**Deliberately not supported:** headings (`#` opens far too many ordinary
chat lines), images (`![]()` — inline media has a server-validated
pipeline and must not be bypassable by an arbitrary URL), tables, raw
HTML, reference links, footnotes, thematic breaks (`---` is common in
plain prose), setext headings, and autolinking. Autolinking stays with
`src/gtkurl.c`, which owns the canonical scheme list and the
trailing-punctuation trimming; a second detector in Rust would guarantee
the two eventually disagreed about what a link is.

Backslash escapes any construct char, code spans suppress all other
parsing inside them, unmatched delimiters render literally (never eat a
lone asterisk), and nesting is depth-capped so a line of five thousand
asterisks cannot recurse the parser into the stack guard.

**Parser choice: a hand-written inline scanner, not `pulldown-cmark`.**
`pulldown-cmark` is the obvious pick (pure Rust, MIT, well-tested) and was
considered. Passed over for three reasons: it has no inline-only mode, so
we would be filtering a block-level event stream and fighting CommonMark's
block rules to suppress exactly the constructs listed above; its event
stream would still need converting into byte-ranged spans, which is most
of the work; and a scanner for a handful of constructs is exhaustively
unit-testable and predictable on the pathological input chat actually
produces. `hotline-proto` is hand-written for the same reasons.

**Send side — what goes on the wire is the literal text.** The protocol
carries plain text, so `**bold**` is transmitted as `**bold**`. Other
GtkHx users see bold; everyone else sees asterisks. This is how Slack,
Discord and IRC clients have always behaved; it needs no capability
negotiation, no wire change and no server cooperation, and it is the
reason markdown is the right choice here where a custom binary styling
extension would not be.

**Receive side — the honest tradeoff.** Rendering markdown on *incoming*
text means a message typed as literal `*emphasis*` on a 1997 Mac client
renders as italics. Mitigations, in order: the subset is conservative,
unmatched delimiters stay literal, and the toggle turns rendering off
entirely for people who would rather see exactly what was typed. The
toggle affects messages appended after it; rows already in a buffer keep
the rendering they were built with, because re-parsing scrollback would
mean holding every row's original source text alive forever — a permanent
memory cost for a setting nobody flips twice.

**Security.** `[label](url)` is a phishing vector: the visible text can
lie about the destination. The parser enforces a scheme allowlist —
`http`, `https`, `ftp`, `hotline`, `mailto`, matching what `gtkurl.c`
accepts minus the bare-host autolink forms. Anything else — `javascript:`,
`data:`, `file:`, an unrecognised scheme — makes the whole construct
render as literal text, delimiters included, so the user sees exactly what
was typed rather than a link they cannot inspect. Activating a link routes
through the shared `gtkurl_show_popup`, whose header shows the resolved
URL before anything opens. Fenced code is inert.

Three decisions worth recording. *The gutter is never parsed* — a nick
containing asterisks is a nick. *Only a stylistically uniform body is
parsed*: a body assembled from several differently-styled runs is chrome
the caller styled deliberately (a divider, a `[hx]` status line) and
re-parsing it would fight that; in practice every real body is a single
run, plain for live chat and muted for history. *The row's own colour is
laid **under** the parse* — the renderer treats a gap between spans as
*default* style, not "whatever the row was", so without this a muted
history line would come back with only its bold words muted and everything
else at full contrast. There is a test.

Fenced code is deliberately not autolinked either: a URL inside a code
fence is being *shown*, not offered.

### Two gotchas worth keeping

**Code needs a box, not a font.** The first cut relied on the `CODE`
attribute alone, which sets the Pango font family to Monospace — and
GtkHx's chat font is *already* monospace, so `` `code` `` rendered
identically to code with the backticks quietly deleted. Strictly worse
than not parsing it. Inline code now gets a tint behind it and fenced
blocks a tinted, outlined rounded box, both derived from the theme
foreground at low alpha so they read on light and dark without a second
colour to keep in step. The block's box is computed from its *laid-out
line boxes* rather than from separate geometry, so it cannot land anywhere
other than under the code it belongs to.

**A one-line fence is a code block.** ```` ```like this``` ```` is how
people actually type one in a chat box, because chat boxes send on Enter.
The line scanner read it as an *opening* fence, made the rest of the line
the "language", and searched for a close that never came — yielding an
empty block, i.e. a blank row where the text should have been.

An unterminated fence runs to the end of the body. The alternative
(treating it as literal) means a message someone is mid-way through typing
flickers between two renderings.

**Composing** is render-on-display only — no live preview, no WYSIWYG
input. `Ctrl+B` / `Ctrl+I` / `Ctrl+Shift+C` wrap the selection (or insert
the delimiter pair), and the input box gets subdued syntax tinting so you
can see what will render. The `:shortcode:` emoji typeahead is unaffected:
emoji decoding happens before span parsing and the two vocabularies do not
collide.

That tinting is a separate, shallower scanner (`markdown::scan_delims`),
not the renderer, because it needs ranges in the *source* while the
renderer reports ranges in the rendered text with the delimiters removed.
Being wrong in the compose box tints a character that will not render, on
text the user can see and is still editing; being wrong in the renderer
would change what a message *says*. A test pins the one thing they must
agree on: whether a delimiter is live at all.

---

## 5. Interaction

### Selection

Drag-select, double-click word select, triple-click line select, and
autocopy (copy on drag-end, optionally including the timestamp column and
the colour codes — three process-wide prefs). Selection across an image
block contributes the block's alt text to the copied string rather than
xtext's all-or-nothing behaviour.

**Two controllers on one widget have no GTK-guaranteed ordering.**
Double- and triple-click were broken on arrival: the multi-click handler
set a word/row selection on press, and the drag gesture's `drag-begin`
collapsed the selection to a caret on the same press. Whichever ran second
won, and a code comment claiming drag "fires first" was an assumption, not
a fact. Fixed by removing the conflict rather than sequencing it:
`drag-begin` now only records the press point, and the collapsed selection
is installed on first *motion* — the moment it means something.
Click-to-dismiss consequently keys on "the pointer never moved" instead of
"the selection is empty".

**Auto-scroll while dragging past the viewport edge** is a
`GtkTickCallback` driven from the last recorded drag position. xtext's
scroll timers read a stale `select_end_y` rather than the live device
position, because GTK 4 has no synchronous "where is the pointer"
accessor; storing the position from the drag handler and consuming it from
a tick callback is the real answer. The rate is frame-time based, so it
scrolls at the same speed on a 60 Hz and a 144 Hz display.

### Search

**Search is O(scrollback), on purpose.** `ChatBuffer::search` walks the
*model*, not the layout, so a match in a row that has never been laid out
is still found — which is the entire point, since the reason to search is
to reach the part of the scrollback you have not scrolled to. The cost is
paid with a short debounce in the find bar rather than with an index,
because an index would have to be maintained across append, prepend, trim
and replace for a feature used seconds at a time. Matching is literal, not
regex: the needle is what the user typed, so there is no metacharacter
vocabulary to explain and no pathological backtracking to defend against.

The find bar lives in `gtkhx-ui/src/chat_find.rs` and drives
`hx_chat_view_search` / `_search_step` / `_search_clear`. Ctrl+F opens and
focuses, selecting the existing query so typing replaces it; pressed
*again* while the entry already has focus and a query, it advances to the
next match instead — that is the "hit Ctrl+F, type, keep hitting Ctrl+F"
flow, and gating it on the entry already being focused is what keeps the
reopen-and-retype case intact. Ctrl+G / Ctrl+Shift+G and F3 / Shift+F3
both step and both wrap, because which pair is muscle memory depends on
where someone came from. The bar reuses the news panel's highlight colours
exactly rather than inventing its own or widening the palette contract in
`chat_view.h`.

### Keyboard paging, and why a global shortcut cannot do it

**PgUp/PgDn never worked in GtkHx.** Nothing in the tree ever bound them,
and the chat view is deliberately not focusable — `chat.c` calls
`gtk_widget_set_can_focus(FALSE)` so the message input keeps focus — so
the focused `GtkTextView` swallowed the key with its own cursor-movement
binding.

A global-scope `GtkShortcut` would not help: global shortcuts run *after*
normal propagation, so the TextView still wins. The binding therefore
lives on a capture-phase key controller installed on the widget's **root**
(the same one Ctrl+C uses, for the same reason), which runs before the
focus path.

The steal is narrow on purpose. Unmodified paging applies only when focus
is in a text-entry widget — the message input or the subject entry, where
paging means nothing — so the user list's `GtkColumnView` keeps its own
page-by-page navigation. Shift+PgUp/PgDn, the long-standing IRC binding
for "scroll the log", is unambiguous anywhere and bypasses the focus
check. Ctrl+Home/End jump to the top of the scrollback and back to
following the tail. A view that is not mapped (a background tab, a closed
private chat) must not eat the window's keys, so the handler checks that
first. A page scroll keeps one line of overlap, so the line being read
survives the jump.

### Zoom

xtext had none: the only way to change chat text size was the Settings
font pref, a modal round trip that touched nothing but the glyphs.

Zoom is a *view* scale, not a font-size change — text, the timestamp
gutter and nick column, inline media, avatars, indent and padding all
scale together, so the layout stays proportionate. It is stored in
per-mille and steps through a fixed ladder from 50% to 400%. Bindings are
`Ctrl` + `+` / `-` / `0` and `Ctrl` + scroll wheel. It is distinct from
`GtkhxTheme`'s `GTKHX_SCALE_*` areas (static per-theme structural factors
for toolbar / user-list / task icons) and from the desktop-wide text
scaling factor.

Zoom is otherwise silent: text changes size and nothing says by how much
or how to get back to 100%. A badge showing the percentage holds for
around a second and then fades, drawn inside the widget's own snapshot
rather than as an overlay widget — the chat view is packed as a bare child
beside a scrollbar, so a `GtkOverlay` would mean restructuring every
container that holds one for a label that shows for a second.

Zoom changes every height in the buffer, which is exactly the case the
scroll anchor already handles: the anchored row stays put and the content
grows around it. Without anchor-based scrolling, zoom would fling the
viewport — a concrete second payoff from that design decision, and why
zoom landed cheaply here where it would have been painful to retrofit into
xtext.

**Zoom does not persist.** It is per-view and resets on restart; there is
no pref backing it.

### The indent separator

A drag pins the gutter explicitly, and nothing but an explicit unpin
releases it — not a buffer clear, not a stamp-width change. xtext left the
gutter's auto-grow enabled after a drag, so a long nick could silently
undo a narrowing the user had just made by hand; a widening only stuck
because it happened to exceed the auto-indent cap, which switched the auto
path off as a side effect. The grab tolerance is a few pixels rather than
xtext's ±1, which is unhittable on a fractional-scale display, and the
drawn rule and the hit test share one `separator_x()` so they cannot drift
apart. The drag is clamped to a band of the viewport rather than to
`max_indent`: that cap is about how far the gutter may grow unattended,
and the point of the drag is to overrule it.

---

## 6. The retired mIRC escape vocabulary

Rows used to be built as byte strings with in-band `\003NN` colour
escapes. That vocabulary is gone. It is worth recording why, because the
reasoning is the reason the structured model exists at all — and because
of how the record went wrong once.

### The escapes were never protocol

Verified two ways: by tracing every generation site in the tree, and from
Misha directly — the vocabulary came in with the XChat 1.8.5 xtext fork
around 2000 and was never a Hotline concept.

Provenance matters here, so: the claim that it *was* protocol was **not** a
longstanding project belief. It was introduced by an AI-assisted session
in mid-2026 and sat in `CLAUDE.md` — a file every future session reads as
ground truth — for a couple of months. It is corrected there now. The
lesson worth carrying is that a plausible-sounding rationale invented for
an existing design decision is more durable than an ordinary bug, because
nothing downstream fails when it is wrong.

The findings:

- **The Hotline wire format has no text styling.** `HTLS_HDR_CHAT` is
  `uid + flags + body`. `HTLS_HDR_MSG` is `uid + body`. News, broadcasts,
  file comments, agreements — all plain text. There is no colour field and
  no style field anywhere in the protocol.
- **Every `\003NN` byte in a buffer was written by GtkHx.** All of them:
  the nick brackets, the highlight wrap, the `INFOPREFIX` constant, the
  history-muted rows and dividers, and the inline-media placeholder.
- **Only three of the eight escape codes were ever generated** — colour,
  bold, reset. Italic, strikethrough, reverse and hidden had no producer
  at all; underline appeared only in divider text.
- **Hotline's real per-user colour is a separate `u32` RGB attribute** on
  the user record, applied by the client when rendering a name. It is not,
  and never was, in-band markup.
- **Nothing else consumed them.** The news viewers, agreement window,
  user-info window and broadcast dialog are all `GtkTextView` and ignore
  escapes entirely. xtext was the only consumer.
- **A server could not inject them anyway.** `hotline-proto`'s
  `strip_ansi` (`sanitize.rs`) folds bytes 14–30 into the printable range
  on every received text field.

### Why retiring them *was* the structured-append API

A dead escape vocabulary left in the tree is how it survives another
decade, so retiring it looked like a shim removal. It was not.

The escapes were produced at sites scattered through `chat.c`, `msg.c`,
`gtkhx.c` and `proto_helpers.c`, encoding six distinct things: nick
brackets in the speaker's colour, bold-red highlight, the dark-grey media
placeholder, history-muted rows, the `[hx]` info prefix, and broadcast's
per-sender `[name]` prefix.

Two of those sites were the real argument. `chat.c` and `msg.c`
**re-parsed GtkHx's own escape output** to find where a name ended:

```c
static const char wrap_open[]  = " \00310[";
static const char wrap_close[] = "\00310]\003 ";
```

That is a data structure round-tripped through a presentation format and
parsed back out. Removing the escapes without giving the API somewhere to
put the structure would have meant inventing a *different* string
convention to re-parse — the same mistake with fresh bytes. The run API is
that somewhere.

The `chat-log-line` signal changed shape with it: it carries
`(htlc, cid, name, colour, body)` rather than a pre-formatted string, so
`INFOPREFIX` is the bare string `"hx"` and broadcast passes its sender name
and colour as parameters (`hx_printf_named`). The `hx_printf_prefix`
callers are unchanged — the prefix argument simply means the tag now.

### Two security consequences of dropping the escape parser

**`broadcast_sanitise_name` used to be load-bearing for correctness.** The
sender's name went inside a `" \00310[\003<col><name>\00310]\003 "` wrapper
that the chat side scanned for a closing sequence, so a name containing a
raw `\003` could terminate the wrapper early, break info-line detection, or
smuggle its own colours into the log. That is unreachable now — there is
no wrapper to escape from. The sanitiser stays because control bytes in a
text layout are still undesirable, but it has been demoted from a security
boundary to hygiene.

**Plain appends no longer interpret escapes.** The remaining callers pass
text that came *off the wire*, so continuing to interpret escapes there
would have let a server set colours in your chat log by sending the bytes.
It cannot: they are characters like any other.

### What survives

The palette. Slots 32..37 are the UI roles `GtkhxTheme` fills (see
`gtkhx_theme.h`'s matching `GTKHX_PAL_*` enum and
`chat.c::gtkhx_apply_theme_palette`); slots 0..31 keep their historical
mIRC index values, so a theme that already sets them keeps rendering the
same, and the named constants above address them. `chat_view.h` is now the
sole definition of that contract — the Rust side asserts against its
values, so the agreement is still checked, just from the other end.

**One dead remnant remains, flagged rather than removed.**
`src/proto_helpers.c` still holds a copy of the old `[hx]` prefix and
checks *incoming server chat* against it. Nothing produces the prefix, and
the check only ever sees server-sent text, so it cannot fire. Removing it
means retiring the proto-test cases that feed it the literal string, which
is its own change. Every other `\003` in the tree is inside a comment
explaining what used to be there.

---

## 7. Not built

### Typed interaction signals

The design called for typed signals — `link-activated`,
`message-activated`, `load-more`, `selection-changed` — to replace the
string-prefix demux. They were never built. `src/chat_view.h`'s note that
`word-click` and its urlcheck companion are "still xtext-shaped" points
here, at **Typed interaction signals**.

What actually exists is xtext's `word-click`, genuinely emitted, plus
`speaker-menu`. Parity beat purity: the C handlers `chat.c` and `msg.c`
connect — gtkurl's, chat-history's load-older sentinel, and inline media's
`hxmedia:N` — all recognise their targets by matching the clicked *word*
as a string, and emitting the same signal with the same tokenisation kept
all three working unchanged. Retiring them is a semantic change to three C
handlers, not a mechanical one.

(A GLib detail worth not rediscovering: the signal is registered as
`"word-click"`, because glib-rs's `Signal::builder` requires a canonical
name and *panics* otherwise — and a panic there is an abort, since it
unwinds out of `class_init` across the FFI. The C callers keep their
underscore spelling and still resolve, because GLib canonicalises `_` to
`-` on both registration and lookup.)

`hx_chat_view_set_urlcheck_function` is accepted and ignored: the view
autodetects with `gtkurl_scan` directly rather than asking a per-view
classifier, so the callback is redundant. It is accepted with its real
type so no caller has to launder it through `void *`.

### Marker line

xtext tracked a last-read marker. Nothing in GtkHx ever called it under
GTK 4, so it was not reproduced — only the palette slot
(`HX_CHAT_PAL_MARKER`) is reserved. It was a real feature and someone will
notice its absence if it is ever wanted.

### Accessibility

The widget gets `GtkAccessible` by virtue of being a `GtkWidget` and
implements nothing beyond that — no `GtkAccessibleText`, so a screen
reader sees an opaque box. xtext exposed nothing either, so this is not a
regression, but it is a scoped omission rather than an oversight. Zoom is
the accessibility affordance that did ship.

### Golden-render tests

There are none. The layout engine is covered headlessly and the widget has
its own tests, but nothing pins actual rendered pixels.

### Live re-rendering of historical rows

`Speaker` copies a nick and colour rather than borrowing the `HxMember`,
so a rename or a colour change repaints the user list and not the chat
scrollback. Copying is cheaper, and messages are arguably historical
records of who said what under what name at the time — worth changing only
if live re-rendering of old rows turns out to be wanted.
