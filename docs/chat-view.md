# The chat view

The GtkHx side of the chat rendering surface: the view every chat,
private-chat and private-message window draws its output on.

The widget is **Rotulus**, a GTK4 scrollback view for text-stream chat,
LGPL-2.1-or-later. It lives in its own repository,
[mishan/rotulus](https://github.com/mishan/rotulus), and GtkHx takes it
from crates.io like any other dependency. It knows nothing about Hotline.
How it works — the layout engine, the message model, scroll anchoring,
markdown, selection, search, the signals and properties an application
hooks, and its measurements — is in its
[design doc](https://github.com/mishan/rotulus/blob/main/docs/design.md).
This document covers what GtkHx does with it: how the build finds it, what
GtkHx configures, how to move to a new release, and how to change the
widget when GtkHx needs it to do something new.

It replaced a vendored copy of HexChat's xtext widget. The measured
comparison that justified deleting xtext is the
[xtext benchmark](https://github.com/mishan/rotulus/blob/main/docs/xtext-benchmark.md),
run inside GtkHx while both backends were still in the tree.

---

## 1. Where it comes from

Three crates, released together from the rotulus repository:

- **`rotulus`** — the GTK4 widget, and the C ABI in `include/rotulus.h`.
- **`rotulus-layout`** — its layout engine, with no dependencies at all.
  GtkHx uses it directly for the link detector (`Linkifier`,
  `DEFAULT_SCHEMES`), the compose box's markdown highlighting
  (`scan_delims`), and the default grouping gap.
- **`rotulus-mirc`** — mIRC formatting codes to styled runs. GtkHx doesn't
  use it (Hotline has no in-band styling); it arrives as a dependency of
  `rotulus`.

`rust/Cargo.toml` names the first two in `[workspace.dependencies]` with a
caret requirement (`rotulus = "0.1"`, `rotulus-layout = "0.1"`), and
`gtkhx-ui` and `gtkhx-ffi` take them with `workspace = true`. The exact
versions are whatever `rust/Cargo.lock` holds, so a build never moves to a
new release on its own. The two must move together: the widget is built
on a particular release of its layout engine, and GtkHx's direct uses of
the engine should be the same copy, not a second one alongside it. The
news views find links with GtkHx's `Linkifier` while the chat view uses
the widget's, and the compose box highlights markdown with the parser the
view renders it with; two copies could disagree on both.

`rust/meson.build` turns on `rotulus/v4_14` when the GTK it builds against
is 4.14 or newer; that feature is what makes the view implement
`GtkAccessibleText`.

The Flatpak build is offline, so the crates are vendored through
`rust/cargo-sources.json` like every other crates.io dependency. It is
generated from the lockfile and has to be regenerated whenever the lockfile
changes:

```sh
python3 tools/flatpak-cargo-generator.py rust/Cargo.lock
```

---

## 2. The C side: the header and the translations

The widget's symbols are exported from the `rotulus` rlib, which
`gtkhx-ffi` bundles into `libgtkhx_ffi.a` with everything else; C links
straight to them. There is no dispatcher file between C and the widget.

The declarations are in `rotulus.h`, which ships inside the crate. GtkHx
keeps no copy of it. `rust/rotulus.py` asks `cargo metadata --locked`
where the locked `rotulus` package lives — in the cargo registry for a
normal build, in the vendored sources for the Flatpak — and
`rust/meson.build` uses that directory's `include/` as `rotulus_inc`, which
`src/meson.build` adds to the C include path. The directory's name carries
the crate's version, so `rust/meson.build` reads `Cargo.lock` at configure
time, which makes it a configure dependency: moving the lock reconfigures
the build and picks up the new header.

The C ABI's methods take a `RotulusView *`, and GtkHx's C holds its views
as `GtkWidget *`, so the call sites in `chat.c`, `msg.c` and `options.c`
cast with `ROTULUS_VIEW()`.

The widget's own strings (its two context menus) are in the `rotulus`
gettext domain, and their catalogs ship in the crate's `po/`. The same
script compiles them: a `rotulus-locale` target in `rust/meson.build`
writes `build/rust/rotulus-locale/<lang>/LC_MESSAGES/rotulus.mo`, and an
install script copies those under the configured `localedir`. The macOS
and Windows bundle scripts pick them up from the same build directory.
`gtkhx.c` binds the `rotulus` domain beside GtkHx's own at startup.

Nothing in GtkHx translates those strings. A fix to one goes to the
rotulus repository.

---

## 3. What GtkHx configures, and where

### `chat_view.rs`

`rust/crates/gtkhx-ui/src/chat_view.rs` is where GtkHx tells the widget
what it needs to know about Hotline. Every chat output is built by
`gtkhx_chat_view_new`, which sets the palette and font, then:

- **What never changes** (`setup`): the view never takes focus, so typing
  stays in the input box beside it; the indented two-column layout with a
  draggable separator; the maximum gutter width; the grouping gap; the
  link schemes; the avatar resolver; and the two link handlers.
- **The preferences** (`configure`): word wrap, scrollback length,
  timestamps and their format, avatars, markdown, "Open links with a single
  click", and the autocopy settings. `gtkhx_chat_view_configure` re-applies
  them; `options.c` calls it from one change hook, `changed_chat_view`,
  which walks the open views once rather than once per setting.

**Links.** The scheme list is `rotulus-layout`'s defaults plus
`hotline://`, held in one `Linkifier`. The same `Linkifier` backs
`gtkurl_scan`, which the news views' URL tagging calls, so the chat view
and the news views cannot disagree about what a link is. A primary click
on a `hotline://` link connects through `connect_open_hotline_url`; any
other scheme falls through to the desktop. A right-click pops
`gtkurl_show_popup`, the URL menu every GtkHx surface shares, whose header
shows the resolved URL before anything opens.

**Avatars.** The resolver is `hx_chat_avatar_for_key` in
`src/chat_avatar.c`, which applies the user list's precedence rule: a
fogWraith GIF avatar wins over the classic cicn icon. Duplicating that
rule would mean chat and Users disagreeing about which icon a user has.
The C resolver *lends* its paintable, valid only until the next call,
while the widget's avatar function returns a full reference. The C ABI
can't bridge that, so `chat_view.rs` registers the resolver through the
crate's Rust API, `set_avatar_func`, and takes its own reference on the
way out.

### `chat.c` and `msg.c`

**The palette.** `chat.c::gtkhx_apply_theme_palette` fills the view's
palette from the active theme and pushes it to every open view with
`rotulus_view_set_palette`. `src/chat.h` names the slots GtkHx builds rows
with (`HX_CHAT_INFO_COLOR`, `HX_CHAT_HIGHLIGHT_COLOR`,
`HX_CHAT_PLACEHOLDER_COLOR`, …) in terms of the header's `ROTULUS_PAL_*`
roles, and nicks go through `hx_chat_nick_color`, which hashes the name
onto the theme's `nick_colors` when it has any. See
[theming.md](theming.md).

**Speaker identity.** A row's `RotulusSpeaker.key` is the Hotline user id,
`0` when unknown. It comes from the `HTLS_HDR_CHAT` UID field when the
server sends one, and otherwise from `hx_member_model_find_by_name` against
the same `HxMemberModel` the user list is built from. One user, one
record, whichever surface you clicked; the thing to avoid is a third user
structure. A lookup that misses stays 0.

**Signals.** `chat.c` connects the ones that need GtkHx's state:

| Signal | GtkHx's answer |
|---|---|
| `speaker-menu` | `users.c::user_popup_show`, the same menu the Users window and the private-chat sidebars pop |
| `load-more` | fetch the next page of chat history |
| `media-activated` | the inline media click-to-view dialog ([inline-media.md](inline-media.md)) |

`link-activated` and `link-menu` are connected in `chat_view.rs`, above.

**The find bar** is `rust/crates/gtkhx-ui/src/chat_find.rs`, driving
`rotulus_view_search`, `_search_step` and `_search_clear`. The rotulus
design doc describes its key bindings as the model for an application's
bar.

GtkHx doesn't set a last-read marker yet, and zoom has no preference
behind it: it is per view and resets on restart.

### Benchmarks

The in-app benchmarks that drive the chat view — `chat`, `media` and
`history` — are in `rust/crates/gtkhx-ui/src/bench/` and run through
`tools/uibench.sh`; see [performance.md](performance.md). The widget's own
headless benchmarks and render tests live in the rotulus repository.

---

## 4. Moving GtkHx to a new Rotulus release

1. Raise the requirement on `rotulus` and `rotulus-layout` together in
   `rust/Cargo.toml`, when the new release is outside the current caret
   range (a new `0.x`). Within the range, the lockfile is the only thing
   that moves.
2. Update the lock for all three crates at once:

   ```sh
   cd rust && cargo update -p rotulus -p rotulus-layout -p rotulus-mirc
   ```

3. Regenerate the Flatpak's vendored sources:

   ```sh
   python3 tools/flatpak-cargo-generator.py rust/Cargo.lock
   ```

4. Reconfigure (the lockfile change does this on the next build) so the
   new `rotulus.h` and translations are picked up.
5. Adapt to API changes. Most land in `chat_view.rs`; C ABI changes land at
   the `ROTULUS_VIEW()` call sites in `chat.c`, `msg.c` and `options.c`,
   and in `src/chat_avatar.c` if the avatar contract moves again. The
   `chat` bench in `gtkhx-ui` builds rows through the crate's Rust FFI
   types directly, so it breaks loudly when those change.

Then run the full set of gates in `CLAUDE.md`, both voice configurations
included.

---

## 5. Working on the widget itself

When a GtkHx change needs something the widget doesn't do, the change goes
into the rotulus repository, not into GtkHx. The widget has to stay
ignorant of Hotline: a feature GtkHx wants should be expressed as
something any chat application could use, with GtkHx's specifics left in
`chat_view.rs` or C.

The normal path is to make the change there, release it, and move GtkHx to
the release as above.

While developing both sides at once, point Cargo at a local checkout with a
`[patch.crates-io]` entry in `rust/Cargo.toml` — here, a rotulus checkout
beside this one:

```toml
[patch.crates-io]
rotulus = { path = "../../rotulus/crates/rotulus" }
rotulus-layout = { path = "../../rotulus/crates/rotulus-layout" }
rotulus-mirc = { path = "../../rotulus/crates/rotulus-mirc" }
```

`rotulus.py` follows the patch, since it asks `cargo metadata` where the
crate is, so the header and translations come from the checkout too.
**Never commit the patch.** It points at a path that exists only on your
machine, the Flatpak's offline build cannot see it, and a GtkHx that
depends on unreleased widget code cannot be built by anyone else. Release
the widget, then move GtkHx to the release.

---

## 6. The retired mIRC escape vocabulary

GtkHx used to build chat rows as byte strings with in-band `\003NN` color
escapes, which xtext interpreted on every render. That vocabulary is gone.
The rotulus design doc covers why the widget takes structured rows
instead; this section is the GtkHx half: why the escapes were never
Hotline's, what GtkHx's call sites became, and what survives.

### The escapes were never protocol

The vocabulary came in with the XChat 1.8.5 xtext fork around 2000 and was
never a Hotline concept. Tracing every generation site in the tree found:

- **The Hotline wire format has no text styling.** `HTLS_HDR_CHAT` is
  `uid + flags + body`. `HTLS_HDR_MSG` is `uid + body`. News, broadcasts,
  file comments, agreements — all plain text. There is no color field and
  no style field anywhere in the protocol.
- **Every `\003NN` byte in a buffer was written by GtkHx**: the nick
  brackets, the highlight wrap, the `INFOPREFIX` constant, the
  history-muted rows and dividers, and the inline-media placeholder.
- **Only three of the eight escape codes were ever generated** — color,
  bold, reset. Italic, strikethrough, reverse and hidden had no producer
  at all; underline appeared only in divider text.
- **Hotline's real per-user color is a separate `u32` RGB attribute** on
  the user record, applied by the client when rendering a name. It is not,
  and never was, in-band markup.
- **Nothing else consumed them.** The news viewers, agreement window,
  user-info window and broadcast dialog are all `GtkTextView` and ignore
  escapes entirely. xtext was the only consumer.
- **A server could not inject them anyway.** `hxproto`'s `strip_ansi`
  (`sanitize.rs`) folds bytes 14–30 into the printable range on every
  received text field.

### What the call sites became

The escapes were produced at sites scattered through `chat.c`, `msg.c`,
`gtkhx.c` and `proto_helpers.c`, encoding six distinct things: nick
brackets in the speaker's color, bold-red highlight, the dark-gray media
placeholder, history-muted rows, the `[hx]` info prefix, and broadcast's
per-sender `[name]` prefix. Two of those sites, in `chat.c` and `msg.c`,
re-parsed GtkHx's own escape output to find where a name ended. Each of
the six is now a field of a row or a palette index on a run.

The `chat-log-line` session signal changed shape with it: it carries
`(htlc, cid, name, color, body)` rather than a pre-formatted string, so
`INFOPREFIX` is the bare string `"hx"` and broadcast passes its sender name
and color as parameters (`hx_printf_named`). The `hx_printf_prefix`
callers are unchanged — the prefix argument simply means the tag now.

### Two security consequences

**`broadcast_sanitise_name` used to be load-bearing for correctness.** The
sender's name went inside an escape wrapper that the chat side scanned for
a closing sequence, so a name containing a raw `\003` could terminate the
wrapper early, break info-line detection, or smuggle its own colors into
the log. That is unreachable now — there is no wrapper to escape from.
The sanitizer stays because control bytes in a text layout are still
undesirable, but it has been demoted from a security boundary to hygiene.

**Text off the wire is characters.** The callers that append server text
pass it as a run, which the view never interprets, so a server cannot set
colors in the chat log by sending the bytes.

### What survives

The palette. Slots 0..31 keep their historical mIRC values; after them
come the roles `GtkhxTheme` fills (see `gtkhx_theme.h`'s matching
`GTKHX_PAL_*` enum and `chat.c::gtkhx_apply_theme_palette`), then the
per-nick colors. `rotulus.h` is the sole definition of that layout.

**One dead remnant remains, flagged rather than removed.**
`src/proto_helpers.c` still holds a copy of the old `[hx]` prefix and
checks *incoming server chat* against it. Nothing produces the prefix, and
the check only ever sees server-sent text, so it cannot fire. Removing it
means retiring the proto-test cases that feed it the literal string, which
is its own change. Every other `\003` in the tree is inside a comment
explaining what used to be there.
