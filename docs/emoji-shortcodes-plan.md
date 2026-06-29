# Emoji shortcodes — scoping & plan

> Status: **implementation in progress.** This document lays out the design
> for letting users exchange emoji on Hotline servers that don't speak
> UTF-8, using Slack/Discord-style `:shortcode:` text as the on-wire
> fallback. Per-phase progress is tracked by the ✅ marks in the Sub-phases
> section below — that list is the source of truth for what has landed in
> the current tree.

## Problem

GtkHx already has an emoji picker (`src/emoji.{c,h}` — a `GtkMenuButton`
wrapping `GtkEmojiChooser`, shared by the public chat, private chat, and
private-message inputs). It inserts the picked codepoint(s) verbatim into
the input's `GtkTextBuffer`.

That works on a UTF-8 server. On a legacy (Mac Roman) server it does not:
the outbound text chokepoint `gtkhx_text_for_wire` (`src/text_util.c`)
encodes to `MACINTOSH` via `g_convert_with_fallback`, and every emoji
codepoint — which has no Mac Roman representation — is replaced with `?`
(0x3F). So 😂 goes out as `?`, and that's what every other user sees.
Most surviving Hotline servers are exactly these old Mac Roman servers
(see the reference-server notes in `CLAUDE.md`).

The whole point of this feature is to give those servers a *readable*
fallback instead of `?`.

## Idea

Borrow the Slack/Discord/GitHub convention: a stable, ASCII-only textual
name for each emoji, wrapped in colons — `:joy:`, `:thumbsup:`,
`:heart:`. Then:

- **Send (legacy servers):** before Mac Roman encoding, rewrite any emoji
  in the user's text to its canonical `:shortcode:`. The shortcode is pure
  ASCII, so it survives Mac Roman encoding untouched. Anyone — GtkHx or
  any other Hotline client — sees `:joy:` instead of `?`. Graceful
  degradation falls out for free: a non-GtkHx client just shows the
  literal `:joy:`, which is the same readable thing Slack users saw for a
  decade.

- **Receive (all servers):** at display time, scan incoming chat text for
  `:shortcode:` tokens and replace each known one with its emoji. So a
  message that arrives as `:joy:` renders as 😂. This is the Slack-like
  behaviour: it also lets a user *type* `:joy:` by hand and have it turn
  into an emoji, on any server.

The two directions are independent and symmetric. Two GtkHx users on a
Mac Roman server get full round-trip emoji (😂 → `:joy:` on the wire →
😂). A GtkHx user talking to a vintage client gets `:joy:` outbound (much
better than `?`) and renders the vintage client's literal `:joy:` typing
inbound.

## Locked-in decisions

These were settled before writing the doc:

1. **Receive-side conversion is always on** (Slack-like), not gated on the
   server's encoding. `:joy:` → 😂 happens on UTF-8 and legacy servers
   alike. Rationale: consistent behaviour, and people are used to typing
   shortcodes. A preference can disable it (see Open questions).

2. **The emoji↔shortcode table and the scan/replace logic live in Rust**
   (`rust/crates/hotline-proto`), exposed over the existing `#[no_mangle]
   extern "C"` FFI surface in `ffi.rs`. This matches where Mac Roman
   conversion already lives (`text.rs::to_utf8`, called from
   `text_util.c::gtkhx_text_to_utf8` via `gtkhx_proto_text_to_utf8`), keeps
   `text_util.c` thin, and gets us cheap Rust unit tests over the table.

3. **Send-side conversion runs only in legacy mode** (`utf8_mode ==
   FALSE`). On a UTF-8 server we send the real emoji bytes — no reason to
   downgrade to text when the wire can carry the codepoint. (Receive-side
   still runs on UTF-8 servers, per decision 1, to handle hand-typed
   shortcodes.)

## Dataset

There is no Unicode-blessed shortcode standard; the de-facto sources are:

- **gemoji** (GitHub's set — `github/gemoji`, `emoji.json`). MIT-ish /
  permissive. ~1900 emoji, one canonical name + aliases each. This is the
  most widely recognised set and the recommended base.
- **emoji-data** (`iamcal/emoji-data`, the Slack set). Adds Slack-specific
  names and the `:skin-tone-N:` modifier convention.
- **Unicode CLDR short names** — descriptive, not the colon-style names
  people type; not a fit.

**Recommendation:** vendor **gemoji** as the canonical base (one canonical
shortcode per emoji for the *send* direction), and additionally accept the
Slack aliases on the *receive* direction so we recognise as many
hand-typed variants as possible. Many-to-one is fine and expected:
multiple shortcodes decode to the same emoji; only one encodes from it.

The table is **generated, then checked in** — same approach as the Mac
Roman table in `text.rs` (hand-generated from `iconv`, committed with a
provenance comment, no build-time codegen).

**As built (E1):** the generator `tools/gen_emoji_table.py` reads the
Python `emoji` package's `EMOJI_DATA`, which conveniently bundles exactly
the material we want — each entry carries a CLDR English name (`en`, e.g.
`:face_with_tears_of_joy:`) plus the GitHub gemoji / Slack-style aliases
(`alias`, e.g. `[":joy:"]`). So a single dependency gives us gemoji/Slack
shortcodes for encode and a permissive superset for decode, without
hand-stitching `emoji.json` and an emojibase Slack preset. The generator
runs by hand; its output `rust/crates/hotline-proto/src/emoji_table.rs` is
committed with a provenance header naming the package version (mirroring
`MAC_ROMAN_HIGH`). Re-run after a package bump.

The generated file holds two plain sorted slices (no `phf`/extra crate
dependency — the crate is std-only, consistent with `text.rs`):

- `ENCODE: &[(&str /*emoji cluster*/, &str /*canonical shortcode*/)]`,
  sorted by cluster for `binary_search`. Encode sources are
  fully-qualified emoji only. The canonical is the **shortest**
  grammar-valid name across both the gemoji/Slack aliases and the
  lowercased CLDR name, **restricted to names that decode back to this same
  emoji** (so it round-trips), tie-broken alphabetically. The
  round-trip restriction matters when two emoji share a name — e.g. ☂ and ☔
  both carry "umbrella"; DECODE awards it to ☂ (first-wins), so ☔'s
  canonical becomes its next uniquely-owned name, "umbrella_with_rain_drops".
  An emoji whose every name is claimed by others is omitted from ENCODE
  (it stays a literal emoji on the wire — the pre-feature `?` fallback);
  vanishingly rare.
- `DECODE: &[(&str /*shortcode*/, &str /*emoji cluster*/)]`, sorted by
  shortcode. Permissive: every grammar-valid name (aliases + CLDR) across
  qualification levels, deduped first-wins with fully-qualified preferred.

Names that don't fit the `[a-z0-9_+-]+` scanner grammar (uppercase,
parens, accented CLDR names) are dropped at generation; the alias usually
covers the same emoji with a clean name.

License note: the `emoji` package is BSD-licensed (GPL-2.0-or-later
compatible); the underlying CLDR/gemoji data is permissive. Recorded here
and in the generated file header.

## Hook points

The codebase has clean single chokepoints for both directions, so the
wiring is small.

### Send

`gtkhx_text_for_wire(utf8, len, utf8_mode, is_body, out_len)` in
`src/text_util.c` is the one place all outbound chat/message/news body
text is encoded. In the `else` (legacy) branch, **before** the
`g_convert_with_fallback` to `MACINTOSH`, run the UTF-8 string through a
new `emojis → :shortcodes:` pass. Because shortcodes are ASCII, the
existing Mac Roman conversion and the `is_body` LF→CR normalisation that
follow are unaffected.

All the legacy senders already flow through this function with the right
`utf8` flag:

- `chat.c::hx_send_chat` (public/private chat, `is_body=TRUE`)
- `commands.c` (line ~341, chat actions)
- `msg.c` (private messages)
- `news.c` / `news15.c` (news posts — *whether* news bodies should get
  emoji shortcodes is an open question; see below)

Doing the rewrite inside `gtkhx_text_for_wire` means we get all of them at
once. If we want chat-only behaviour, gate it on `is_body` plus a new
flag, or do the rewrite in the callers instead (more sites, more control).

**Recommendation:** do it inside `gtkhx_text_for_wire`, legacy branch,
controlled by a parameter (e.g. add `gboolean emoji_shortcodes` or reuse
`is_body`) so news vs chat can be tuned without scattering the call.

### Receive (display)

Inbound text is sanitised through `gtkhx_text_to_utf8` (`src/text_util.c`,
backed by `gtkhx_proto_text_to_utf8`). **Do not** put the shortcode→emoji
pass there — that function also handles server names, file names, news
metadata, etc., and a filename like `report:final:v2` must not get
mangled. The conversion has to be scoped to **chat / message / news body
display only.**

The right seam is where a chat body is built for rendering:

- `proto_helpers.c::hx_chat_event_new` builds `HxChatEvent` (the parsed
  chat line: sender/body split, highlight detection). The `body` substring
  is exactly what we want to transform. Applying the pass here keeps it out
  of `text_util.c` and is unit-testable in the Tier 2 proto harness.
- Private messages: `proto_helpers.c` (~line 1226, the `name`/`body`
  builder) and `msg.c`.
- News: `news.c` post/article display path.

Note the interaction with **mIRC colour codes** and the **info-line
prefix**: chat carries `\003NN` colour runs and a `[hx]` info prefix
(`hx_info_prefix` in `proto_helpers.c`). The shortcode scanner must treat
those control bytes as opaque and only match `:[a-z0-9_+-]+:` runs in the
visible text. Since the scanner only rewrites exact known-shortcode
matches, stray colons in control sequences are inert, but the scanner
should still skip over `\003` runs to avoid splitting a code.

**Where the transformed text lands:** the converted string is what gets
inserted into the xtext buffer (`gtk_xtext_append*` in `chat.c`). That
means the displayed *and copied* text is the emoji, Slack-style. This is
intended. It also means the (future, currently `#if 0`'d) chat logger in
`xoutput_chat` would log emoji, not shortcodes — fine.

**As built (E3).** The decode runs in `hx_chat_event_new` (public/private
chat) and `hx_msg_event_new` (PMs) via a shared
`hx_decode_emoji_shortcodes` helper over `gtkhx_proto_shortcodes_to_emoji`.
For a parsed `Nick: body` line only the body region is converted and
`body_len` is recomputed (the nick column stays literal and `is_self` is
decided against the un-decoded nick first); an unsplit prose line is
converted whole; info lines are skipped. News is deliberately **not**
wired (open question 1 — chat + PM only in v1).

One known edge from `hx_chat_split_nick_body`: a line with *no* real nick
but a shortcode mid-prose (e.g. `*** waves :tada:`) has its first colon
eaten as a (bogus) nick separator, so that shortcode won't decode. Lines
with a real `Nick:` prefix, or lines that fail the split outright (no
colon, colon-at-start, or pre-colon > 31 bytes), decode correctly. The
mis-split case is rare server prose and not worth complicating the
splitter for; revisit if it ever bites.

### Rendering — no new work expected

Emoji already render in GtkHx: the picker inserts them into the input, and
UTF-8-server chat already shows received emoji through xtext (HexChat's
Pango/cairo fork). Pango pulls colour glyphs from the system emoji font
(e.g. Noto Color Emoji). So shortcode→emoji produces codepoints xtext
already knows how to draw. **Verify** during phase 1 that a converted
emoji renders identically to a picker-inserted one (font fallback can be
fussy); if there's a gap it's a font-config issue, not new widget code.

## Typeahead: inline shortcode autocomplete

As the user types `:jo`, show a popup of matching shortcodes (`:joy:`,
`:joystick:`, `:joker:`, …) each with its emoji glyph, navigable with the
keyboard and committed with a keypress — the same affordance Slack and
Discord give in their composers. This makes shortcodes discoverable
without opening the picker and is the primary way most people will reach
emoji once it exists.

### Input model it has to fit into

All three chat-style inputs (public chat, private chat, PM) are
multi-line `GtkTextView`s, not `GtkEntry`s, so `GtkEntryCompletion` is not
available. They share one key handler, `chat.c::chat_input_key_pressed`,
installed as a `GtkEventControllerKey` ("key-pressed"). That handler
already claims **Tab** (nick completion via `tab_nick_comp`), **Return**
(send), **Shift+Return** (newline), and **Up/Down** (input history). The
existing multi-match nick completion has *no* popup — it prints candidates
to the chat log via `hx_printf` and fills the common prefix. So the
typeahead popup is entirely new UI, and the chief integration risk is not
fighting that shared handler for Tab/Return/Up/Down.

### Where it lives

Extend the existing `src/emoji.{c,h}` module — it already owns the
shared-across-three-inputs emoji UI. Add:

```
void hx_emoji_typeahead_attach (GtkWidget *target_text_view);
```

called once per input at chat-window construction (next to where
`hx_emoji_button_new` is wired). Per-input state (the popover, the current
token range, the match model, open/closed flag) hangs off the widget via
`g_object_set_data`, mirroring how `gchat`/`sess` are already attached
there.

### Trigger detection

On every `GtkTextBuffer::changed` (and cursor move — `mark-set` on the
insert mark), inspect the text immediately left of the cursor for an
*open* shortcode token: a `:` that is at line start or preceded by
whitespace, followed by one or more of `[a-z0-9_+-]`, with no closing `:`
and no whitespace between it and the cursor. That run after the colon is
the prefix. Rules:

- Require a small minimum prefix (1–2 chars) before showing, so a bare `:`
  doesn't dump the whole table.
- Dismiss when the token closes (`:` typed), on whitespace, on Esc, on
  cursor leaving the token, or on no matches.
- Debounce is unnecessary at this scale (a prefix query is a bounded scan
  + cap), but keep the match count capped (~8) for a tidy popup.

### Widget & positioning

A `GtkPopover` with `gtk_widget_set_parent` on the text view, pointed at
the caret rectangle from `gtk_text_view_get_cursor_locations` (buffer →
widget coords via `gtk_text_view_buffer_to_window_coords`). It must **not
steal focus** — the user keeps typing into the text view — so
`gtk_popover_set_autohide(FALSE)` and we drive selection/commit/dismiss
ourselves. Contents: a `GtkListView`/`GtkListBox` of rows, each showing
the emoji glyph + `:shortcode:`. Model rows come from
`gtkhx_proto_shortcode_matches`.

### Key routing (the careful part)

When the popup is open it needs Up/Down (move selection), Tab/Return/Enter
(commit), and Esc (dismiss) — exactly the keys the shared handler already
uses. Cleanest approach: attach a **dedicated `GtkEventControllerKey` in
the capture phase** (`gtk_event_controller_set_propagation_phase
(GTK_PHASE_CAPTURE)`) that only consumes those keys **while the popup is
visible** and returns `FALSE` otherwise, letting `chat_input_key_pressed`
keep its current behaviour untouched when no popup is up. This avoids
threading typeahead state into the codebase's most complex key handler.

### What gets committed

Committing a match replaces the partial `:jo` token with the **emoji
glyph** — identical to what the picker (`on_emoji_picked`) inserts. This
unifies the two entry paths: the typeahead is effectively a keyboard-
driven picker. Downstream is then exactly the already-scoped send path
(legacy server → `gtkhx_text_for_wire` rewrites the glyph back to
`:joy:`; UTF-8 server → sends the glyph). The user's own echoed line comes
back through the receive path and renders as the emoji on every server, so
both composer and transcript show the glyph consistently.

(Alternative: commit the literal `:joy:` text instead of the glyph. Works
too — the always-on receive pass renders it on display — but it diverges
from the picker and shows raw text in the composer. Recommend committing
the glyph.)

## Edge cases & rules

- **Match grammar.** Only `:[a-z0-9_+-]+:` tokens that hit the table
  convert. `10:30:00`, `http://`, `C:\path`, and `:)` are left alone
  (`:)` is an emoticon, out of scope — see Open questions). This is the
  same rule Slack/GitHub use and it's why false positives are rare.
- **Skin-tone modifiers.** The picker can emit tone-modified clusters
  (👍🏽 = base + U+1F3FD). Options: (a) drop the tone and emit the base
  `:thumbsup:`; (b) emit Slack-style `:thumbsup::skin-tone-4:` which
  round-trips. **Recommendation:** support the `::skin-tone-N:` suffix on
  both encode and decode so tone survives between two GtkHx clients;
  degrade to the literal text for everyone else.
- **ZWJ sequences** (family/profession emoji, flags). Multi-codepoint
  clusters need cluster-aware scanning (encode must match the longest
  cluster, not the first codepoint). gemoji keys are full clusters, so a
  longest-match-first scan over grapheme-ish boundaries handles this.
  Worst case, unmapped clusters fall back to `?` exactly as today — no
  regression.
- **Length growth.** A shortcode is longer than the emoji it replaces
  (`:joy:` is 5 bytes vs 4; pathological cases like
  `:next_track_button:` reach ~7× the cluster's byte length). The rewrite
  runs before length is measured, so a message that was already near the
  protocol's per-chunk ceiling can be pushed over it: Hotline data chunks
  carry a **16-bit** length, and the Rust chat builder
  (`gtkhx_proto_build_chat_chunks`) rejects a body larger than
  `u16::MAX` (65535). In practice chat lines are tiny and the encode is
  ASCII, so this only matters for a deliberately enormous near-limit
  message; if it ever bites, the body would need splitting across chunks
  (a pre-existing concern the rewrite only nudges). Worth a note, not a
  blocker.
- **Idempotence / no double-encode.** Receive converts `:joy:`→😂 for
  display only; we never re-send what we displayed, so there's no
  round-trip loop. The send path starts from the user's raw input buffer.
- **Performance.** Both passes are linear scans with an O(log n)
  `binary_search` over a sorted slice per candidate token (as built in
  E1 — no `phf`, no `HashMap`, no extra crate dependency, consistent with
  `text.rs`); negligible next to the network path. The encode scan also
  skips the search entirely for ordinary ASCII, since no emoji cluster
  begins with an ASCII letter.

## FFI surface

Mirror the existing `gtkhx_proto_text_to_utf8(buf, len, out, cap) ->
size_t` shape (caller-owned output buffer, returns bytes written). These
are the C-side hand-declared prototypes; lengths/capacities are `size_t`
to match `src/hotline_proto.h` (the Rust side declares them `usize`, which
is ABI-identical):

```c
// emoji -> :shortcodes: (encode, used in the legacy send branch)
size_t gtkhx_proto_emoji_to_shortcodes(const uint8_t *in, size_t in_len,
                                       uint8_t *out, size_t out_cap);

// :shortcodes: -> emoji (decode, used at chat display time)
size_t gtkhx_proto_shortcodes_to_emoji(const uint8_t *in, size_t in_len,
                                       uint8_t *out, size_t out_cap);

// Prefix query for the typeahead popup. `prefix` is the partial name the
// user has typed after the opening colon (no colons), e.g. "jo". Writes up
// to `max` matches into `out` as a NUL-separated run of
// "shortcode\temoji" records (or a small struct array — see note),
// returns the match count. Matches are ranked: exact, then prefix, then
// alias-prefix; capped and stable-sorted for a deterministic popup order.
size_t gtkhx_proto_shortcode_matches(const uint8_t *prefix, size_t prefix_len,
                                     uint8_t *out, size_t out_cap, size_t max);
```

(The match query needs to *iterate* the table, unlike the two converters
which only look up. A NUL/`\t`-delimited fill buffer keeps ownership on the
C side and matches the existing buffer-fill idiom; a `repr(C)` struct array
with borrowed `&'static str` pointers into the checked-in table is the
alternative if we'd rather avoid the parse on the C side.)

Worst-case output sizing: encode can grow (emoji → longer ASCII name), so
the C caller sizes the out buffer generously (e.g. a small fixed multiple,
or a two-call "ask for length, then fill" pattern). Decode only shrinks or
stays equal. Match `text_util.c`'s existing over-allocate-then-`g_realloc`
idiom.

## Sub-phases

Each ends on something testable, per the roadmap's house style.

- **E1 — Table + Rust core. ✅** Generator under `tools/`, checked-in
  `emoji_table.rs`, and the two pure-Rust functions (`emoji_to_shortcodes`,
  `shortcodes_to_emoji`) with the match grammar and longest-cluster scan.
  Rust unit tests: spot-check round-trips, a whole-table canonical
  round-trip / collision guard (`every_canonical_round_trips`), alias +
  CLDR decode, no-match passthrough, control-byte skipping. No C yet.
- **E2 — FFI + send. ✅** The two `#[no_mangle]` shims in `ffi.rs`. Encode
  wired into `gtkhx_text_for_wire`'s legacy branch (with a guard against
  pathological lengths/expansion). Tier 2 test: an emoji-bearing string
  with `utf8_mode=FALSE` carries `:joy:` on the wire and Mac Roman survives.
- **E3 — Receive/display. ✅** Decode wired into `hx_chat_event_new`
  (public/private chat) and `hx_msg_event_new` (PM) via a shared helper.
  Tier 2 tests over `HxChatEvent` / `HxMsgEvent`: body `:tada:` decodes to
  🎉, nick stays literal, `body_len` recomputed, non-shortcode colons left
  alone. News deliberately excluded for v1 (open question 1).
- **E4 — Rendering verification.** Confirm converted emoji render in xtext
  identically to picker-inserted ones; handle any font-fallback surprise.
- **E5 — Typeahead popup.** The match-query FFI
  (`gtkhx_proto_shortcode_matches` + Rust ranking/cap, with unit tests),
  then `hx_emoji_typeahead_attach` in `emoji.c`: trigger detection,
  capture-phase key controller, non-autohide popover at the caret, commit
  = insert glyph. Wire into all three inputs. This is the largest UI piece;
  it depends only on E1 (the table), not on E2/E3.
- **E6 — Preference + docs.** Add Chat-page toggle(s) (see Open
  questions), update this doc and the ROADMAP Phase 5 "UX features" list.
  Consider a Tier 3 round-trip between two harness clients against mhxd
  (Mac Roman) to pin the end-to-end path as a regression guard, per the
  "prefer Tier 3 repro" project norm.

## Test plan

- **Rust unit (E1):** table sort order, a whole-table canonical
  round-trip / collision guard (every ENCODE shortcode decodes back to its
  own emoji — this is what flagged and drove the fix for the ☂/☔
  "umbrella" name clash), alias + CLDR decode coverage, grammar edge cases
  (`10:30`, `C:\`, empty `::`, adjacent `:a::b:`), cluster longest-match,
  control-byte (`\003NN`) skipping.
- **Tier 2 wire fixtures (E2/E3):** send-side asserts wire bytes; receive
  side asserts `HxChatEvent.body` and that `sender_off/body_off`,
  highlight, and info-prefix detection still line up after substitution.
- **Typeahead (E5):** Rust unit tests on `shortcode_matches` (ranking,
  cap, prefix vs alias-prefix, empty/no-match). The popup UI itself is
  exercised manually / by light interaction checks — trigger detection
  against tricky buffers (`http://`, `C:\`, mid-word colons, token at line
  start vs after a word) is the part worth a focused unit test on the
  detection helper, which can live in C and be tested in isolation.
- **Tier 3 (E6):** two orchestrated clients on Dockerized mhxd (legacy /
  Mac Roman): client A picks 😂, client B receives and renders 😂. Guards
  the full pipeline and the "don't break Mac Roman" requirement.

## Open questions

1. **Scope to chat only, or include news/PM/agreement?** Chat and PM are
   the obvious yes. News posts are long-form prose where `:shortcode:`
   substitution may be less wanted — and news is more likely to contain
   literal colon-delimited text. Recommendation: chat + PM in v1; news
   behind the same pref, default off, or excluded.
2. **Emoticons (`:)`, `:D`, `<3`)?** Slack/Discord also auto-convert a
   small emoticon set. Out of scope for v1 (different grammar, higher
   false-positive risk), but a natural follow-up.
3. **Preference granularity.** One master toggle, or separate
   send/receive/typeahead toggles? A single "Convert emoji to/from
   :shortcodes:" checkbox on the Chat settings page may be enough, but the
   **typeahead popup** is a distinct UX that some users will want to
   disable independently (it pops up whenever they type a colon). Likely
   two toggles: the conversion behaviour, and the typeahead popup.
4. **Typeahead match scope & ranking.** Prefix-match canonical names only,
   or also alias names (so `:+1` finds 👍)? Recommendation: include
   aliases in the query, rank exact > canonical-prefix > alias-prefix.
   Also: minimum prefix length before the popup appears (1 vs 2 chars) and
   the result cap (~8).
5. **Commit key.** Tab and Return both commit in the popup-open state. Some
   users expect Return to send even with the popup up. Recommendation:
   while the popup is open, Tab/Enter commit and Esc dismisses; a second
   Return (popup already dismissed) sends. Confirm this feels right in
   E5.
6. **Canonical-name collisions across datasets.** gemoji and Slack
   disagree on a few canonical names. Pin gemoji as canonical for encode;
   accept both as aliases for decode. Record the chosen base's commit in
   the generated file header.
7. **Capability bit?** Not needed — this is a pure text convention with
   graceful degradation, and inventing a capability bit would require
   server/ecosystem buy-in for zero functional gain. Explicitly *not*
   doing one.

## Why this fits the codebase

- Two existing single chokepoints (`gtkhx_text_for_wire` out,
  `gtkhx_text_to_utf8` / `hx_chat_event_new` in) mean tiny, localised
  wiring.
- The Rust-table-with-C-FFI pattern is already established by the Mac
  Roman conversion next door in the same files.
- It's strictly additive and degrades gracefully, so it can't break the
  Hotline 1.2/1.5 wire compat that's a hard project requirement — the wire
  still carries plain text; we're just choosing better plain text than `?`.
