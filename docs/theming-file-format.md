# Theme file format

GtkHx theming state — per-area UI scales, the chat palette, the
user-list name colours, and bundled chrome icons — lives in **theme
files**: a GKeyFile-format `.ini` per theme, loaded by `GtkhxTheme`
at startup. The active theme is named by the `THEMENAME` key in
`gtkhxrc`; that is the only theming knob in the user's prefs.
Everything else is in the theme file.

This is intentional. Themes are shareable, hand-editable, and
bundleable: drop a friend's `funky-dark.ini` into
`$XDG_CONFIG_HOME/gtkhx/themes/`, set `THEMENAME=funky-dark`, and
the whole client re-skins. If the scale and palette knobs lived in
`gtkhxrc`, you could only have one "look" per user and nothing to
trade. The design rationale is in [theming.md](theming.md).

Settings → Appearance carries a theme **picker** (see "Picking a
theme" below). There is no theme *editor* — no colour rows, no scale
spin rows, no "Save as". To change a theme's contents, edit the file
directly or drop in someone else's.

## Where theme files live

A theme can live in either of two layouts:

| Layout | Path | Purpose |
|---|---|---|
| Flat file | `$XDG_CONFIG_HOME/gtkhx/themes/<name>.ini` | Colour + scale only. No bundled icons. |
| Directory bundle | `$XDG_CONFIG_HOME/gtkhx/themes/<name>/theme.ini` | Same `.ini` shape, but the surrounding directory can also ship icons at `<name>/icons/<logical>.png`. |
| Built-in | GResource `/com/nasledov/gtkhx/themes/<name>.ini` or `<name>/theme.ini` | Compiled into the binary; the same flat-vs-bundle distinction applies. |

The loader prefers the dir-form when both are present at the same
name (it's the richer layout). On the GResource side it prefers
the dir-form for the same reason. If `THEMENAME` is unset / empty,
the loader uses `"default"`. A name containing `/` or `\` is
rejected (defensive against escaping the themes directory).

## Bundled icons

A directory-form theme can override any of GtkHx's chrome icons
by dropping a same-named PNG into `<name>/icons/`. Per-icon
fallback: anything the bundle doesn't supply falls through to the
built-in GResource pixmap (`/com/nasledov/gtkhx/pixmaps/<logical>.png`).
A bundle that ships only `connect.png` works fine — every other icon
keeps the stock glyph.

```
$XDG_CONFIG_HOME/gtkhx/themes/
├── solarized.ini             # flat-form: colours/scales only
├── mychunky/                 # dir-form bundle
│   ├── theme.ini             # colours + scales (same schema as
│   │                         #   flat-form .ini files)
│   └── icons/                # optional; replaces individual
│       ├── connect.png       #   chrome icons by logical name
│       ├── tasks.png
│       └── download.png
└── …
```

The chrome icons currently swappable (logical names — drop a same-
named .png into the bundle's `icons/` to override): `ban`,
`broadcast`, `chat`, `connect`, `download`, `upload`, `up`, `down`,
`start`, `refresh`, `mkdir`, `trash`, `pencil`, `preview`, `kick`,
`ignore`, `info`, `message`, `news`, `news_folder`, `news_category`,
`news_post`, `post_news`, `tasks`, `tracker`, `users`, `files`,
`edit_user`, `new_user`, `move`, `options`, `quit`, plus the
file-type icons `file`, `folder`, `folder_dropbox`, `file_alias`,
`file_app`, `file_disk`, `file_html`, `file_image`, `file_movie`,
`file_note`, `file_sit`, `file_text`, `file_zip`, `file_move`,
`file_move_lr`, `file_move_rl`.

Format constraints (v1):

- **PNG only.** Other formats (SVG, WEBP, …) are rejected by the
  loader.
- **No size requirement** — icons render at the size the relevant
  scale knob asks for. Pixel-art bundles should ship 16×16 source
  PNGs and rely on the nearest-neighbour upscale baked into the
  button helpers. Hand-crafted larger PNGs work too.
- **The brand logo (`gtkhx.png`)** is deliberately NOT swappable —
  it identifies the app.
- **Hotline user icons (cicn)** are out of scope — the icon *ID* is
  a wire-protocol value other clients look up in their own set, so
  remapping it would break something two clients have to agree on.
  `cicn.c` handles them separately.

SVG bundles via glycin / `hx-image-decode` (async decode, returns
`GdkTexture` directly, decode-at-target-size for vectors) are the
planned v2. See [theming.md](theming.md).

## Picking a theme

Two ways to switch themes; both fire the same reload + repaint:

1. **Settings → Appearance → GtkHx theme** — combo populated by
   `gtkhx_theme_list_available()`, which enumerates built-ins from
   GResource plus any `.ini` files under `$CONFIG/themes/`. Display
   names come from each file's `[gtkhx-theme] name` key, falling
   back to the basename. Order: `default` pinned first, everything
   else alphabetical by display name.
2. **Edit `THEMENAME` in `gtkhxrc`** directly. Same code path —
   the `THEMENAME` cfgvar hook calls `gtkhx_theme_load_active()`.

To pick up edits to the body of the active theme (i.e. you changed
`default.ini` or `mybundle/theme.ini` while the app is running),
briefly switch `THEMENAME` to another theme and back — there's no
filesystem watch on the theme file or its icons.

## Built-in themes

GtkHx ships two themes baked into the binary. Both supply distinct
light + dark variants and let `AdwStyleManager`'s `dark` property
(driven by Settings → Appearance → Theme) pick which one renders.

| `THEMENAME` | Display | Description |
|---|---|---|
| `default` | Default | GtkHx's classic appearance. Adwaita-aligned `#fafafa`/`#1d1d1d` light + `#000`/`#cccccc` dark, with the Adwaita accent blue for selection. |
| `solarized` | Solarized | Ethan Schoonover's [Solarized](https://ethanschoonover.com/solarized/) palette. `palette.light` holds the canonical Solarized Light values (cream `#fdf6e3` bg, `#657b83` body text); `palette.dark` holds Solarized Dark (`#002b36` bg, `#839496` body text). Picking "Solarized" gives you Solarized Light on a light desktop and Solarized Dark on a dark one — the way the palette was designed. |

The built-ins live at `src/themes/<name>.ini` in the source tree.
Copy one to `$CONFIG/themes/my-theme.ini` and edit as a starting
point for a custom theme.

## Schema

```ini
# --- Metadata ----------------------------------------------------------
[gtkhx-theme]
# `name` is the display string the theme picker shows (falling back
# to the file's basename when unset). Optional.
name = My Theme
# `description` is documentation only — nothing in the loader or the
# picker reads it today. Write it for whoever opens the file next.
description = Short tagline.

# --- Per-area UI scales ------------------------------------------------
# Integer percent against the *unscaled* source art (16×16 button
# pixmaps; user-list icon's natural size; base font) — a theme owns
# the whole factor, with no hidden multiplier stacked underneath.
# Range: [50, 300] — values outside the range are clamped on load.
# Omit a key to inherit the built-in fallback for that area (see the
# table under "Scale defaults" below).
[scale]
toolbar        = 200    # toolbar window button icons
window_buttons = 200    # action buttons in Users / Files / News /
                        # Tasks / Tracker windows
userlist_icon  = 125    # user-list avatar icon
userlist_text  = 125    # user-list name text
tasks_row_icon = 200    # per-task glyph in the tasks list

# --- Chat palette: light variant --------------------------------------
# Six UI-role color slots for the chat view. The chat palette also
# has 32 legacy in-band slots (0..31) that a theme cannot reach;
# they're a private vocabulary GtkHx wrote for itself, being retired,
# and not something a theme should be asked to define.
#
# Format: "#RRGGBB" hex. Upper- or lower-case, optional "#"
# prefix. Alpha is implicit 1.0 (chat surfaces are opaque). A
# malformed value falls back to the built-in default for that slot
# (with a runtime warning); the rest of the file still loads.
#
#   fg             default text foreground       (GTKHX_PAL_FG)
#   bg             default text background       (GTKHX_PAL_BG)
#   mark_fg        selection text foreground     (GTKHX_PAL_MARK_FG)
#   mark_bg        selection background          (GTKHX_PAL_MARK_BG)
#   marker         marker line                   (GTKHX_PAL_MARKER)
#   history_muted  rendered chat-history text    (GTKHX_PAL_HISTORY_MUTED)
[palette.light]
fg            = #1d1d1d
bg            = #fafafa
mark_fg       = #ffffff
mark_bg       = #3584e4
marker        = #cc0000
history_muted = #5e5e5e

# --- Chat palette: dark variant ---------------------------------------
# Same six slots. The active variant follows AdwStyleManager's
# `dark` property (which respects the Settings → Appearance → Theme
# combo and, on "Follow system", the desktop's color scheme).
[palette.dark]
fg            = #cccccc
bg            = #000000
mark_fg       = #eeeeee
mark_bg       = #204a87
marker        = #cc0000
history_muted = #9a9a9a

# --- User-list name colors --------------------------------------------
# Four-slot palette for the user-list name text, keyed by the
# 2-bit status field (idle / admin). Without overrides, names use
# the historical defaults in src/gtkhx.c (regular → GTK
# foreground, idle → grey, admin → red, admin-idle → light pink).
# A per-user RGB colour carried on the wire (the Colored-Nicknames
# extension) still wins over both the theme slot and the default.
# A theme that colors the listview background via `fg`/`bg` above
# usually wants to override these too so names stay readable
# against the themed row.
#
#   active     — regular user (status & 3 == 0)
#   idle       — idle / away (status & 3 == 1)
#   admin      — admin (status & 3 == 2)
#   admin_idle — admin + idle (status & 3 == 3)
#
# Each slot is independently optional: omit a key to inherit the
# historical default for that slot. The Colored-Nicknames
# extension's per-user RGB still wins over both theme and default.
[users.light]
active     = #586e75
idle       = #93a1a1
admin      = #dc322f
admin_idle = #871f1d

[users.dark]
active     = #93a1a1
idle       = #586e75
admin      = #dc322f
admin_idle = #871f1d
```

The full built-in default is at `src/themes/default.ini` in the
source tree — copy it as a starting point.

## Scale defaults

Two different things are called "the default" for a scale area, and
they are not currently the same value:

- **The built-in fallback**, compiled into `src/gtkhx_theme.c`. It
  applies when the *active theme file omits* a `[scale]` key.
  Toolbar 200, window buttons 200, user-list icon 125, user-list
  text 125, tasks row icon 200 — GtkHx's historical factors,
  expressed against the unscaled source art.
- **The shipped `default` theme**, `src/themes/default.ini`. It sets
  all five keys explicitly to 100, so when that theme is active the
  built-in fallbacks above never come into play.

An explicitly-written key always wins over the fallback, so a theme
that says nothing about scales does not get the same result as a
theme that copies `default.ini`'s `[scale]` block. Worth knowing
before you copy that block into a new theme and wonder why your
buttons look different from a theme that left the section out.

## Loading and reload

- `gtkhx_theme_load_active()` is called from `fe_init()` before any
  widget construction so the very first measure pass gets the right
  factors. It emits `GtkhxTheme::changed`.
- Changing the active theme while the app is running — through the
  Settings picker or by editing `THEMENAME` in `gtkhxrc` — fires the
  same loader and re-emits `changed`; every button, user-list view,
  and chat view rescales and repaints in place. (The *Theme* combo in
  Settings → Appearance is a different control: it drives Adwaita's
  light / dark mode, which is a separate axis.)
- There is no filesystem watch on the theme file itself. To pick up
  edits to the active theme without an app restart, briefly change
  `THEMENAME` to another value and back.

## How loaders handle missing data

| Situation | Behavior |
|---|---|
| `THEMENAME` unset or empty | Falls back to `"default"`. |
| `$CONFIG/themes/<name>.ini` missing | Falls back to the built-in default GResource. |
| Theme file present but unparseable | Logs a warning, falls back to the built-in default. |
| Scale key missing | Inherits the built-in fallback for that area (see "Scale defaults"). |
| Scale value out of range | Clamped to `[50, 300]` on load. |
| Scale value `≤ 0` | Treated as "unset" → inherits the fallback. |
| Palette key missing | Inherits the built-in default for that slot. |
| Palette value malformed (bad hex) | Logs a warning, inherits the default for that slot. |

The principle throughout: a bad or missing key never breaks the
load. The user gets the built-in default for that specific slot,
and the rest of the theme applies as written.

## Implementation pointers

- Singleton state, loader, accessors: `src/gtkhx_theme.{c,h}`.
- Active theme name pref: `CFG_THEME_NAME` in `src/cfgkeys.h`,
  `gtkhx_prefs.theme_name` in `src/prefs.h`.
- Built-in default file: `src/themes/default.ini`, listed in
  `src/gtkhx.gresource.xml`.
- Icon resolver (the theme-bundle `icons/` lookup): `src/gtkhx_icon.{c,h}`.
- Palette consumer: `src/chat.c::gtkhx_apply_theme_palette()`, which
  pushes the array into the chat view via `hx_chat_view_set_palette`
  (declared in `src/chat_view.h`; implemented in the Rust
  `hxchat-view` crate).
- CSS-surface consumers: `gtkhx_refresh_css` /
  `gtkhx_refresh_userlist_css` in `src/gtkhx.c`.
- Scale consumers: button helpers in `src/gtkutil.c`, the task-row
  icon builder in `src/tasks.c`, and `src/users_cell.c`'s
  `measure` / `snapshot`.
- Tests: `tests/unit/test_theme_scale.c` (clamping, fallbacks,
  keyfile round-trip, `changed` emission) and
  `tests/unit/test_theme_listing.c` (discovery, sort order,
  user-file shadowing).
