# Emoji shortcodes

Letting users exchange emoji on Hotline servers that don't speak UTF-8,
using Slack/Discord-style `:shortcode:` text as the on-wire fallback.

## Problem

GtkHx has an emoji picker (a `GtkMenuButton` wrapping `GtkEmojiChooser`,
shared by the public-chat, private-chat, and private-message inputs)
which inserts the picked codepoints verbatim into the input buffer.

That works on a UTF-8 server. On a legacy (Mac Roman) server it does
not: the outbound chokepoint `gtkhx_text_for_wire` encodes to Mac Roman,
and every emoji codepoint — none of which has a Mac Roman
representation — becomes `?` (0x3F). So 😂 goes out as `?`, and that's
what every other user sees. Most surviving Hotline servers are exactly
these old Mac Roman servers.

The whole point of the feature is to give those servers a *readable*
fallback instead of `?`.

## Idea

Borrow the Slack/Discord/GitHub convention — a stable, ASCII-only name
per emoji wrapped in colons (`:joy:`, `:thumbsup:`, `:heart:`). **Send
(legacy servers):** before Mac Roman encoding, rewrite emoji to the
canonical `:shortcode:`; pure ASCII, so it survives untouched, and a
non-GtkHx client just shows the literal `:joy:`. **Receive (all
servers):** at display time, replace known `:shortcode:` tokens with
their emoji — which also lets a user *type* `:joy:` by hand on any
server.

The two directions are independent and symmetric. Two GtkHx users on a
Mac Roman server get full round-trip emoji (😂 → `:joy:` on the wire →
😂).

## Locked-in decisions

1. **Receive-side conversion is always on** (Slack-like), not gated on
   the server's encoding. `:joy:` → 😂 on UTF-8 and legacy servers
   alike, for consistent behaviour and because people are used to typing
   shortcodes.
2. **The table and the scan/replace logic live in Rust**, in
   `hotline-proto` — the same crate that already owns Mac Roman
   conversion — exposed over the existing C FFI surface. Keeps the
   encoding layer thin and gets cheap Rust unit tests over the table.
3. **Send-side conversion runs only in legacy mode.** On a UTF-8 server
   we send the real emoji bytes; there's no reason to downgrade to text
   when the wire can carry the codepoint. (Receive-side still runs on
   UTF-8 servers, per decision 1, to handle hand-typed shortcodes.)
4. **No capability bit.** This is a pure text convention with graceful
   degradation; negotiating a bit would need server and ecosystem buy-in
   for zero functional gain. Explicitly not doing one.

## The table

There is no Unicode-blessed shortcode standard. The de-facto sources are
GitHub's **gemoji** set (one canonical name plus aliases per emoji) and
**emoji-data** (the Slack set). Unicode CLDR short names are descriptive
rather than the colon-style names people type, so they're only useful as
a supplementary decode source.

The generator (`tools/gen_emoji_table.py`) reads the Python `emoji`
package's `EMOJI_DATA`, which bundles exactly the needed material: each
entry carries a CLDR English name plus the gemoji/Slack-style aliases.
One dependency gives gemoji/Slack shortcodes for encode and a permissive
superset for decode, without hand-stitching two datasets.

The generator runs **by hand** and its output
(`hotline-proto/src/emoji_table.rs`) is checked in with a provenance
header naming the package version — the same approach as the Mac Roman
table next door, which was generated once from `iconv` and committed. No
build-time codegen. Re-run after a package bump.

The generated file holds two plain sorted slices — no `phf`, no
`HashMap`, no extra crate dependency, keeping the crate std-only.

### Canonicalisation rules

These are not reconstructable from the generated table, so they matter:

- **`ENCODE`** — `(emoji cluster, canonical shortcode)`, sorted by
  cluster for binary search. Encode sources are **fully-qualified emoji
  only**; we never want to emit a minimally- or un-qualified form onto
  the wire.
- The canonical name is the **shortest grammar-valid name** across both
  the gemoji/Slack aliases and the lowercased CLDR name, **restricted to
  names that decode back to this same emoji** so it round-trips, with an
  **alphabetical tie-break**.
- The round-trip restriction matters when two emoji share a name. ☂ and
  ☔ both carry "umbrella"; `DECODE` awards it to ☂ (first wins), so ☔'s
  canonical becomes its next uniquely-owned name,
  `umbrella_with_rain_drops`. An emoji whose every name is claimed by
  others is simply omitted from `ENCODE` — it stays a literal emoji on
  the wire, i.e. the pre-feature `?` fallback. Vanishingly rare.
- **`DECODE`** — `(shortcode, emoji cluster)`, sorted by shortcode.
  Permissive: every grammar-valid name (aliases + CLDR) across
  qualification levels, deduped first-wins with fully-qualified
  preferred.
- Names that don't fit the `[a-z0-9_+-]+` scanner grammar (uppercase,
  parentheses, accented CLDR names) are dropped at generation; an alias
  usually covers the same emoji with a clean name.

**Skin tones ship as whole-cluster table entries**, not as a
`::skin-tone-N:` suffix scheme. 👆🏽 is its own `ENCODE` key mapping to
`index_pointing_up_medium_skin_tone`. The longest-cluster-first scan is
what makes this work, and it means tone survives to any client that
reads the literal text, not just to another GtkHx.

The `emoji` package is BSD-3-Clause and the underlying CLDR/gemoji data
is permissive, so the generated name table is distributable under
GtkHx's GPL-2.0-or-later. Recorded in the generated file header.

## Hook points

### Send

`gtkhx_text_for_wire` (the `hxtext` crate) is the one place all outbound
chat / message / news body text is encoded. In the legacy branch, the
UTF-8 string goes through the encode pass **before** the Mac Roman
conversion. Because shortcodes are ASCII, the conversion and the
body-mode LF→CR normalisation that follow are unaffected. Every legacy
sender already flows through this function, so one edit covers them all.

A pathological-length guard runs up front, before any slice is
constructed, bounding both the shortcode-rewrite buffer and the encode
length.

### Receive (display)

The decode pass deliberately does **not** live in the general inbound
sanitiser — that function also handles server names, file names, and
news metadata, and a filename like `report:final:v2` must not get
mangled. It is scoped to chat and private-message bodies, applied in the
event constructors in `src/proto_helpers.c` (`hx_chat_event_new` and
`hx_msg_event_new`) via a shared helper.

**The chat decode runs over the whole line**, before the info-prefix
check and the nick split, with the split then run on the decoded text so
the sender/body offsets stay consistent. Whole-line rather than
body-only was a deliberate revision: an integration test against a live
server found that **mhxd formats public chat without a `Nick:` colon**
(` *** Name message`), so a body-only scope let a shortcode's own colon
be mistaken for the nick separator and skipped the conversion entirely.

Whole-line is safe for the nick column because the grammar only matches
colon-delimited **lowercase** tokens — a `Nick:` prefix has a colon on
one side only, and any uppercase disqualifies it — and info lines carry
no shortcodes. For private messages the body is a standalone buffer, so
there are no offsets to juggle; the sender name stays literal.

News bodies are deliberately not wired: long-form prose is both less
likely to want substitution and more likely to contain literal
colon-delimited text.

The converted string is what reaches the chat view, so the displayed
*and copied* text is the emoji. That's intended.

### Legacy guard: the mIRC colour byte

The decoder skips over `\x03` plus its numeric spec (`\x03NN[,NN]`) so a
colour run can never be mistaken for shortcode text or have a token
split across it. There is a unit test pinning this.

**This is a legacy guard, not a live mechanism.** The in-band `\003`
escape vocabulary came from the XChat xtext fork and has been retired —
chat rows are built from styled-run arrays now, nothing produces the
escapes, and Hotline itself has no text-styling concept at all. The
guard stays because it is cheap and because inbound text is not ours to
make assumptions about; without this note it would be unexplainable.

## Typeahead

As the user types `:jo`, a popup lists matching shortcodes with their
glyphs, navigable by keyboard.

All three chat-style inputs are multi-line `GtkTextView`s, not
`GtkEntry`s, so `GtkEntryCompletion` isn't available. They share one key
handler that already claims **Tab** (nick completion), **Return**
(send), **Shift+Return** (newline), and **Up/Down** (input history).
Not fighting that handler was the chief integration risk.

Implemented in `gtkhx-ui/src/emoji.rs`, alongside the picker button that
already owns the shared-across-three-inputs emoji UI, and attached once
per input at window construction.

**Trigger grammar.** On every buffer change and cursor move, backward-scan
`[a-z0-9_+-]` from the caret to an opening `:` that is at line start or
preceded by whitespace, with no closing colon and no whitespace between
it and the caret. That run is the prefix. A minimum of 2 characters is
required so a bare `:` doesn't dump the whole table; the backward scan
is itself bounded. The popup dismisses when the token closes, on
whitespace, on Esc, when the cursor leaves the token, or on no matches.
The match count is capped at 8 for a tidy popup — a bounded scan, so no
debounce is needed at this scale.

**Key routing** is the careful part: while open, the popup needs
Up/Down, Tab/Enter, and Esc — exactly the keys the shared handler
already uses. The answer is a **dedicated key controller in the capture
phase** that consumes those keys *only while the popover is visible* and
declines otherwise, so the existing bubble-phase handler keeps its
behaviour untouched whenever no popup is up. This avoids threading
typeahead state through the codebase's most complex key handler.

The popover is non-autohiding (the user keeps typing into the text
view), anchored at the caret, and parented to the view so GTK tears it
down with the window.

**Committing inserts the emoji glyph**, replacing the partial token —
identical to what the picker inserts, so the typeahead is just a
keyboard-driven picker and downstream is exactly the send path already
described. Matches are ranked exact-first, then shortest, then
alphabetical, for a stable deterministic order.

## Preferences

Two toggles on Settings → Chat → Emoji, both default ON:
`CFG_EMOJI_SHORTCODES` (the conversion, both directions — they're a
matched pair) and `CFG_EMOJI_TYPEAHEAD` (the popup, a distinct UX a user
may want off independently). The conversion flag lives as a
module-local toggle in `hxtext` rather than reading the global prefs
struct, so the dependency-light encode/decode paths stay unit-testable.

## Edge cases and rules

- **Match grammar.** Only `:[a-z0-9_+-]+:` tokens that hit the table
  convert. `10:30:00`, `http://`, `C:\path` and `:)` are left alone
  (`:)` is an emoticon — different grammar, higher false-positive risk,
  out of scope). This is the same rule Slack and GitHub use, and it's
  why false positives are rare.
- **ZWJ sequences** (family / profession emoji, flags, keycaps). Encode
  matches the **longest cluster** at each position, not the leading
  codepoint. Table keys are full clusters, so longest-match-first
  handles them. Unmapped clusters fall back to `?` exactly as before —
  no regression.
- **Length growth is the one real hazard.** A shortcode is longer than
  the emoji it replaces (`:joy:` is 5 bytes vs 4; pathological cases
  like `:next_track_button:` reach several times the cluster's byte
  length). The rewrite runs *before* length is measured, so a message
  already near the protocol's per-chunk ceiling can be pushed over it:
  **Hotline data chunks carry a 16-bit length**, and the chat builder
  rejects a body larger than 65535. In practice chat lines are tiny and
  the encode is ASCII, so this only bites a deliberately enormous
  near-limit message; the fix would be splitting the body across chunks,
  a pre-existing concern the rewrite only nudges.
- **Idempotence.** Receive converts for display only; we never re-send
  what we displayed, and the send path starts from the user's raw input
  buffer. No round-trip loop.
- **Performance.** Both passes are linear scans with a binary search over
  a sorted slice per candidate token — negligible next to the network
  path. The encode scan skips the search entirely for ordinary ASCII,
  since no emoji cluster begins with an ASCII letter.

## Testing

Rust unit tests cover table sort order, a whole-table canonical
round-trip / collision guard (every `ENCODE` shortcode must decode back
to its own emoji — this is what flagged and drove the fix for the ☂/☔
"umbrella" clash), alias and CLDR decode, grammar edge cases (`10:30`,
`C:\`, empty `::`, adjacent `:a::b:`), cluster longest-match, colour-byte
skipping, and the typeahead match ranking. Wire fixtures assert the
outbound bytes and that the chat event's sender/body offsets, highlight
detection and info-prefix detection still line up after substitution.
An integration test runs two clients against live mhxd: A encodes 🎉
through the real legacy send path, mhxd relays it (asserting the wire
form is pure ASCII — "don't break Mac Roman"), and B decodes it back to
🎉. That test is what surfaced the whole-line-decode revision.

The popover's live behaviour and the trigger detection both need a
display, so neither is auto-tested.

## Rejected alternatives

- **A capability bit.** See locked-in decision 4.
- **Committing the literal `:joy:` text** from the typeahead instead of
  the glyph. It works — the always-on receive pass renders it — but it
  diverges from the picker and shows raw text in the composer.
- **Putting the decode in the general inbound sanitiser.** Would mangle
  filenames and other colon-bearing metadata.
- **Emoticon conversion** (`:)`, `:D`, `<3`). Different grammar, much
  higher false-positive risk. A natural follow-up, not v1.
