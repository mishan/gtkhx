# Theming

GtkHx is themable along three axes, all of which ship today:

1. **Chrome icons** — the button / glyph PNGs.
2. **Per-area UI scaling** — independent factors for the toolbar, other
   windows' action buttons, the user-list icon and text, and the tasks-row
   glyph.
3. **Colours** — the chat palette's UI-role slots, the user-list name
   colours, and the window chrome, each with a light and a dark variant.

A **theme** is one bundle carrying all three: a GKeyFile `.ini` (optionally a
directory with an `icons/` subdir beside it), living at
`$CONFIG/themes/<name>.ini` or shipped as a GResource. The only theming key in
`gtkhxrc` is `THEMENAME`. The schema reference is
[theming-file-format.md](theming-file-format.md); this document is the *why*.

Implementation: `src/gtkhx_theme.{c,h}` (the `GtkhxTheme` singleton, loader and
accessors) and `src/gtkhx_icon.{c,h}` (the icon resolver).

---

## Why the model looks like this

Two design facts shaped everything else.

### Two unrelated icon systems, and only one of them is themable

GtkHx has two icon pipelines that look similar on screen and have nothing in
common underneath.

**Chrome / button icons** are the glyphs on the toolbar and pane headers, on
the Users / Files / News / Tasks / Tracker action buttons, the task-row and
news-thread row icons, and the file-type icons in the files browser. They come
in two styles (see "Symbolic and classic icons" below): symbolic icons, found
by name in the icon theme, and the classic PNGs in the GResource under
`/com/nasledov/gtkhx/pixmaps/` — 16×16 pixel art, upscaled with
nearest-neighbour to keep the pixels crisp. Either way they are GtkHx's own
chrome and mean nothing to any server.

**Hotline user icons** are `cicn` colour icons decoded out of a Mac-classic
resource fork by `src/cicn.c` — the avatar a user picks in Settings and that
renders next to each name in the user list. The icon *ID* is a wire-protocol
value (`HTLC_DATA_ICON`): it travels to the server, and other clients look the
same number up in their own icon set. Remapping what a number means would be
remapping something two clients have to agree on.

That distinction is what makes the icon axis tractable at all. Icon replacement
is exactly one thing — reskinning the chrome glyphs — with no protocol surface
and no coordination problem. The cicn icons are deliberately not swappable;
they participate in theming only as a surface that gets *scaled*. The brand
logo is likewise excluded, because it identifies the app.

### The hidden base scale, and why "the source art is 100%"

Before theming, every consumer of an icon carried its own hardcoded multiplier:
toolbar buttons at 2×, tasks buttons and the task-row glyph at 2×, the
files-browser buttons at a literal 2, the standalone Users window at 1.25× on
both its icon and its font, and other windows at 1×. An earlier single-knob
experiment added a global "UI scale" percentage that multiplied *on top* of
those constants — so "100%" meant the toolbar rendered at 200% of its source
art, and there was no way to say so in the UI without lying.

The fix is the model the code uses now: **the unscaled source art is the true
100%**, a theme supplies the whole per-area factor, and the built-in default
theme carries GtkHx's historical factors as explicit values rather than
pretending they are 1.0. Call sites hand `gtkhx_theme_scale(area)` their raw
source dimension and multiply — there is exactly one factor source per area and
no hidden multiplier stacked underneath it. The old `TOOLBAR_ICON_SCALE` /
`TASKS_ICON_SCALE` constants and the per-call literal `2`s are gone.

The scale areas are the `GtkhxScaleArea` enum in `src/gtkhx_theme.h`; consumers
are the button helpers in `src/gtkutil.c`, `src/tasks.c`'s row-icon builder, and
`src/users_cell.c`, which reads the icon and text factors live during `measure`
and `snapshot` (so there is no per-cell state to invalidate on a theme change).

One deliberate carve-out: the compact chat-sidebar user list keeps a fixed 1.0
structural density and does not follow the `USERLIST_*` areas. A single
per-area factor can't honestly reproduce two different current densities, so
the knob is scoped to the prominent standalone window. Revisit if the sidebar
should follow too.

---

## How the pieces hang together

`gtkhx_theme_load_active()` runs once in `fe_init()` before any widget is
constructed, so the first measure pass already has the right factors, and emits
`GtkhxTheme::changed`. Changing `THEMENAME` — via the Settings picker or by hand
— re-fires the loader and re-emits. Every consumer subscribes to that one
signal: buttons re-render from source (and auto-unsubscribe on finalize), the
icon resolver's cache is invalidated so the rebuild re-resolves against the new
theme's `icons/` directory, `chat.c::gtkhx_apply_theme_palette()` pushes the
role colours into the chat view via `hx_chat_view_set_palette`, and
`gtkhx_refresh_css` / `gtkhx_refresh_userlist_css` re-emit the `.gtkhx-*` CSS
providers.

The light/dark variant is selected at apply time from `AdwStyleManager`'s `dark`
property, so a theme ships both and the system mode picks one. The same handlers
run on `notify::dark`, so a system-mode flip repaints without a reload.

Whether a theme *opted in* to a colour matters, not just what the colour is:
`gtkhx_theme_palette_role_is_set()` distinguishes "the theme chose this" from
"the theme was silent and inherited the built-in default", and the listview CSS
is gated on it. A theme that doesn't set chat `fg`/`bg` leaves the tracker /
users / tasks / files / news row backgrounds at the system theme instead of
having GtkHx's own fallbacks forced onto them.

### A theme colors the whole window

Themes used to color only the content: the chat, the lists, the text
surfaces. The window around them stayed stock Adwaita gray. That works
only for palettes close to Adwaita's own; anything with a character of
its own (Solarized's navy, or a Hotline-nostalgia theme) came out as
colored boxes pasted onto a gray window.

So a theme now also sets the window chrome, the way GNOME's terminals
tint their header bar from the terminal palette. It does this by
overriding libadwaita's named colors, not by styling widgets:
`gtkhx_theme_build_chrome_css()` turns the `[chrome.*]` roles into a
`:root` block of `--window-bg-color`-family variables plus the matching
`@define-color`s, and `gtkhx_refresh_css` puts that at the top of the
application provider. Every stock widget (header bar, libpanel pane
headers, popovers, selection) follows with no per-widget rule,
and so does GtkHx's own `chrome.css`, which reads `@accent_bg_color`
and `@window_bg_color`. Both forms are emitted because libadwaita seeds
its variables from the named colors, and chrome.css reads the named
colors directly.

One chrome role is a style, not a color swap: `action`. Adwaita fills
suggested-action buttons (Connect, Save, Post) with the accent, and a
theme gets one accent — so a design that uses its accent for position
(selection, focus) but draws actions as links had no way to say so. A
theme that sets `action` gets those buttons as an outline in that color,
turning the accent on hover, keyboard focus and press, from a rule block
`gtkhx_theme_build_chrome_css()` appends after the variables.

Roles a theme leaves out are derived (window from the chat `bg`, the
layered surfaces stepped from window toward fg), so a short theme can't
leave half the window gray. The opt-in rule is the same as for the
listview CSS: no chat fg/bg and no chrome keys means no chrome CSS, and
the default theme stays stock. The "Tint window to match theme" setting
turns the chrome CSS off for users who want system chrome around a
themed chat; it's a user preference, not a theme key, because it's about
the user's desktop rather than the theme.

### An unset chat color follows the system

The same clash ran the other way for the default theme: its chat was
hard `#000` on a dark desktop and `#fafafa` on a light one, a box that
matched neither Adwaita's gray window nor its view color. So chat `fg`
and `bg` no longer have built-in colors. Left unset, they resolve to a
transparent `GdkRGBA`, which every consumer reads as "follow the
system": the chat view carries Adwaita's `.view` class, draws such text
in its CSS color and skips its own background fill, and the
`.gtkhx-text` / `.gtkhx-input` CSS names `@view_fg_color` /
`@view_bg_color` instead of a hex value. Roles derived from fg (the
nicks) follow along.

### The chat gutter is themed

The gutter used to be colored from the fixed mIRC slots — full-intensity
blue and pink brackets, a green `[hx]`, a red mention — which no theme
could reach, and which were close to invisible on a dark background. It
now has roles of its own (`timestamp`, `nick`, `self_nick`, the two
bracket roles, `system`, `system_bracket`, `highlight`) plus a
`nick_colors` list that each nick is hashed onto. That gives the chat a
hierarchy: timestamps and brackets recede, names stand out and tell
people apart, and the body carries the weight.

### Symbolic and classic icons

The chrome icons have a second, modern style: GNOME's symbolic icons. A
symbolic icon is drawn in the widget's CSS color, so it follows the theme —
Neon Doll's purple, Solarized's tones, the accent on a selected row — where
pixel art is the same colored bitmap everywhere. Symbolic is the default; a
theme asks for the pixel art with `icons = classic` in `[gtkhx-theme]`, and the
built-in **Classic** theme does, along with the chat colors GtkHx always had,
for anyone who wants the nostalgic look.

The choice rides on the theme rather than on a separate setting because the
two go together: classic icons belong with the classic chat, and a modern
palette with the modern icons. A user who wants to mix them writes a theme.

`gtkhx_icon_symbolic_name()` (src/gtkhx_icon.c) is the one decision point. Its
table maps each classic logical name to an icon name:

- **Stock Adwaita names** for anything standard — refresh, trash, up/down,
  edit, info, the file types. They match the rest of the desktop and follow a
  user's own icon theme. Only names outside Adwaita's `legacy/` set, which is on
  its way out.
- **App-prefixed names** (`com.nasledov.gtkhx-chat-symbolic`, …) for the
  Hotline vocabulary Adwaita has no icon for: public chat, the user list, news,
  broadcast. These are vendored in the GResource icon tree from GNOME's CC0
  icon-development-kit, converted to plain filled paths so GTK versions before
  4.20 recolor them correctly; `src/icons/README.md` has the provenance and the
  conversion.

It returns NULL — use the pixmap — when the theme is classic, when the theme
ships its own PNG for that icon (a theme's glyph wins in either style), or for a
name with no symbolic counterpart. Every place that shows a chrome icon asks it
first: the pixmap-button helper, the task rows, the file and news-tree cells
(per bind, so a theme change reaches rows as they rebind), and
`gtkhx_icon_image_new()` for one-off images.

Symbolic icons are drawn at 16px times the area's scale factor, the same size
the pixel art is drawn at, so a theme's `[scale]` means the same in both styles.

---

## Open: SVG icon bundles

Icon bundles are PNG-only today. SVG packs should go through **glycin**, not
librsvg / GdkPixbuf directly. We already ship glycin via the `hx-image-decode`
Rust crate (inline-media / banner / chat decode through it; see
`docs/image-decoding.md`), so an SVG pack reuses that pipeline instead of
adding a dependency. Two implications the resolver has to absorb, both already
precedented in that crate:

- **Async-only.** glycin decode returns via callback, unlike the synchronous
  `gdk_pixbuf_new_from_resource` path the resolver uses now. For a few dozen
  small chrome glyphs the clean pattern is decode-once-into-a-cache at startup
  and on theme switch (keyed by logical name × target px), then buttons pull
  synchronously from the cache. `button_refresh_picture` already rebuilds
  lazily off a signal, so a "pack-loaded" emission on the existing
  `GtkhxTheme::changed` bus fits without new plumbing.
- **Returns `GdkTexture`, not `GdkPixbuf`.** This actually *fits better* than
  today's path — `button_refresh_picture` already ends at a `GdkTexture` +
  `GtkPicture`, so an SVG-sourced texture skips the pixbuf→texture round-trip.
  The wrinkle is scaling: raster packs scale a pixbuf with
  `gdk_pixbuf_scale_simple`, but SVG wants to be **rendered at the target pixel
  size** — glycin can decode vectors at a requested size, so the per-area scale
  feeds the decode request rather than a post-scale. The resolver branches:
  built-in / PNG pack → pixbuf path; SVG pack → glycin-at-size path. Both
  converge on a `GdkTexture` for the button.

PNG-only packs remain the zero-Rust, fully-synchronous fallback (at the cost of
the upscaling-blur question at large scales), which is why the SVG path could be
a follow-up rather than a blocker.

Both `src/gtkhx_icon.h` and `src/gtkhx_icon.c` point at this section.

---

## Open: theme editor UI

Settings → Appearance has a "GtkHx theme" `AdwComboRow` and nothing else. It is
a *picker*: it enumerates themes via `gtkhx_theme_list_available()` (GResource
built-ins plus `$CONFIG/themes/*.ini`, default-first then alphabetical by
display name, user files shadowing same-name built-ins) and writes `THEMENAME`.

Editing a theme's *body* is unbuilt: no scale spin rows, no colour-picker rows,
no "Save as" to fork a theme, no import / export. Storage is already in place,
so this is pure UI work plus a write-back path so a Settings edit modifies the
active theme file. For now, editing the `.ini` is the way.

Related open item on the scaling axis: the chat / PM font is still a separate
non-theme preference rather than a named theme axis.

---

## Parked: CSS-as-theme-file vs. the `.ini` schema

The current model is a GKeyFile `.ini` plus a small set of `.gtkhx-*` CSS
providers that the loader emits at runtime from the `.ini`'s palette values. An
alternative is to let themes BE CSS — written in standard GTK CSS, loaded
directly via `GtkCssProvider`. Worth thinking about; not worth implementing
right now. The trade-off is captured here so the decision is informed when we
come back to it.

### What's actually CSS-shaped in our model

Already pure CSS (emitted by `gtkhx_refresh_css` /
`gtkhx_refresh_userlist_css`):

- `.gtkhx-text` / `.gtkhx-input` — read-only text views and editable inputs
  (foreground / background / caret). A user-supplied CSS file could replace
  these 1:1.
- `.gtkhx-listview` / `.gtkhx-userlist` — listview row colours, including the
  `:not(:hover):not(:active)` carve-out that keeps hover feedback alive.

Not CSS-shaped — these can't move to CSS without first restructuring the
machinery behind them:

- **Per-area scales.** These multiply *source* pixmap sizes at decode time
  inside `gtkutil.c::button_load_source`, the tasks row-icon builder, and the
  user-list `measure` / `snapshot`. They aren't styling; they're load-time
  factors fed back into code. GTK CSS has no native way to express "decode a
  16×16 PNG at 200% with nearest-neighbour before handing it to a button."
- **The chat palette.** The chat view takes its colours as an array through
  `hx_chat_view_set_palette`; it never consults a style context. The palette
  has to be a structure someone can hand over.
- **User-list name colours.** `src/users_cell.c` appends its Pango layout with
  an explicit `GdkRGBA`. Same story: no CSS consultation on the draw path.
- **Icon-pack bundling.** The resolver looks up
  `$CONFIG/themes/<name>/icons/<logical>.png`. File resolution, not styling.

### Four viable paths

1. **Hybrid** — keep the `.ini` for the non-CSS-shaped state, and ALSO load an
   optional `style.css` companion from the theme bundle as a `GtkCssProvider`
   layered over the existing `.gtkhx-*` providers. Modest implementation; both
   audiences happy. UX wart: a bundle can carry two files.

2. **Pure CSS, full migration** — make scales custom CSS properties, teach the
   chat view and the user-list cell to consult CSS for their colours, drop the
   `.ini` entirely. Win: theme authors learn one format already familiar from
   web and GTK. Cost: authors of trivial themes end up writing more CSS than
   they would key=values, and the draw paths gain a style-consultation surface
   to maintain.

   *The old cost estimate for this path is superseded.* It was written against a
   large vendored cairo chat widget that no longer exists, and priced in weeks
   of surgery on it plus the risk of re-merging against upstream HexChat. The
   chat surface today is a Rust widget (`hxchat-view` over `hxchat-layout`)
   behind the C ABI in `src/chat_view.h`, and the palette reaches it through one
   clean setter. The honest remaining cost is teaching that widget and its
   layout engine to source colours from a style context rather than being handed
   an array — the layout crate is deliberately dependency-free, so that's a
   design change, not a call-site change — doing the same for the user-list
   cell, and building CSS-property parsing for the scales. Smaller than the old
   estimate, still not small, and still carrying chat-rendering regression risk.

3. **Status quo + `style.css` as a power-user hook** — don't change the loader
   beyond documenting that a bundle may ship a `style.css` that gets attached as
   a `GtkCssProvider`. Path 1 minus the layering question; absorbs into path 1
   later without breaking anything.

4. **Drop the theme-file format, just expose CSS** — stop shipping and loading
   theme files entirely. Loses scales, palette overrides, user-list colour
   overrides and icon bundling, on the theory that they're only worthwhile if
   expressible in pure CSS. Most aggressive simplification; loses real
   capability.

### Lean

Path 1 is the smallest change that gets the "you can write CSS if you want"
affordance without the widget surgery path 2 demands: authors who don't want to
touch CSS keep the readable `.ini` for colour swaps, scale tweaks and dropped-in
icons, and authors who want hover states or per-cell styling drop a `style.css`
next to the `theme.ini`. Path 3 is path 1 deferred — a fine stepping stone if
the loader hook turns out larger than expected. Path 2 is the right long-term
endpoint if GtkHx grows enough theme authors that the `.ini` starts feeling
limiting, at which point the rework amortizes across a real user base. Path 4 is
probably not worth it: the scales and palette overrides solve real problems —
the Solarized-plus-chunky-icons combination that motivated the bundle model, and
the admin/idle name colours that motivated the user-colour axis — and throwing
them away to land on one format is a regression for users who picked Solarized
expecting it to work.

Concretely, path 1 is a `style.css` loader hook in
`gtkhx_theme.c::gtkhx_theme_load_active`, attached via
`gtk_style_context_add_provider_for_display` at `PRIORITY_APPLICATION + 1` so it
sits above our `.gtkhx-*` rules, plus one section in
[theming-file-format.md](theming-file-format.md). Path 2 is its own project, and
would want benching against Solarized plus a hand-written theme to be sure chat
output doesn't regress.

No urgency either way — the current model works, and the feedback we've had has
all been about specific theme behaviours (hover states, name colours, listview
backgrounds) rather than about wanting a different schema.
