# Theming

GtkHx is themable along three axes, all of which ship today:

1. **Chrome icons** — the button / glyph PNGs.
2. **Per-area UI scaling** — independent factors for the toolbar, other
   windows' action buttons, the user-list icon and text, and the tasks-row
   glyph.
3. **Colours** — the chat palette's UI-role slots and the user-list name
   colours, each with a light and a dark variant.

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

**Chrome / button icons** are PNGs in the GResource under
`/com/nasledov/gtkhx/pixmaps/` — the small pixel-art glyphs on the toolbar and
on the Users / Files / News / Tasks / Tracker action buttons, the task-row and
news-thread row icons, and the file-type icons in the files browser. They are
16×16 source art, upscaled at runtime with nearest-neighbour to keep the pixels
crisp. They are GtkHx's own artwork and mean nothing to any server.

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
