# Theme file format

GtkHx theming state — per-area UI scales and the xtext chat palette
— lives in **theme files**: a GKeyFile-format `.ini` per theme,
loaded by `GtkhxTheme` at startup. The active theme is named by the
`THEMENAME` key in `gtkhxrc`; that is the only theming knob in the
user's prefs. Everything else is in the theme file.

This is intentional. Themes are shareable, hand-editable, and
bundleable: drop a friend's `funky-dark.ini` into
`$XDG_CONFIG_HOME/gtkhx/themes/`, set `THEMENAME=funky-dark`, and
the whole client re-skins. If the scale and palette knobs lived in
`gtkhxrc`, you could only have one "look" per user and nothing to
trade. The scoping for this design is in
[theming-scoping.md](theming-scoping.md).

A user-facing theme editor (Settings picker, color rows, "Save as")
is **not** part of this phase. For now, edit the file directly or
drop in someone else's.

## Where theme files live

| Location | Purpose |
|---|---|
| `$XDG_CONFIG_HOME/gtkhx/themes/<name>.ini` | User themes. First place the loader looks. A user file with the same basename as a built-in shadows it. |
| GResource `/com/nasledov/gtkhx/themes/<name>.ini` | Built-in themes (see list below). Compiled into the binary; always available. |

If `THEMENAME` is unset / empty, the loader uses `"default"`. A name
containing `/` or `\` is rejected (defensive against escaping the
themes directory).

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
`default.ini` while the app is running), briefly switch `THEMENAME`
to another theme and back — there's no filesystem watch on the
theme file itself.

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
# Human-readable, used by a future theme picker. Optional.
name = My Theme
description = Short tagline shown in the picker.

# --- Per-area UI scales ------------------------------------------------
# Integer percent against the *unscaled* source art (16×16 button
# pixmaps; user-list icon's natural size; base font). The toolbar /
# window-button defaults are 200% because the source pixmaps are 16
# px and GtkHx has always drawn them at 32. Range: [50, 300] —
# values outside the range are clamped on load. Omit a key to
# inherit the built-in default for that area.
[scale]
toolbar        = 200    # toolbar window button icons
window_buttons = 200    # action buttons in Users / Files / News /
                        # Tasks / Tracker windows
userlist_icon  = 125    # user-list avatar icon
userlist_text  = 125    # user-list name text
tasks_row_icon = 200    # per-task glyph in the tasks list

# --- Chat palette: light variant --------------------------------------
# Six UI-role color slots for the xtext chat widget. mIRC palette
# slots (0..31) are deliberately not exposed — servers send specific
# indices and expect specific colors; remapping "red" would break
# message intent.
#
# Format: "#RRGGBB" hex. Upper- or lower-case, optional "#"
# prefix. Alpha is implicit 1.0 (chat surfaces are opaque). A
# malformed value falls back to the built-in default for that slot
# (with a runtime warning); the rest of the file still loads.
#
#   fg             default text foreground       (XTEXT_FG)
#   bg             default text background       (XTEXT_BG)
#   mark_fg        selection text foreground     (XTEXT_MARK_FG)
#   mark_bg        selection background          (XTEXT_MARK_BG)
#   marker         marker line                   (XTEXT_MARKER)
#   history_muted  rendered chat-history text    (XTEXT_HISTORY_MUTED)
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

## Loading and reload

- `gtkhx_theme_load_active()` is called from `fe_init()` before any
  widget construction so the very first measure pass gets the right
  factors. It emits `GtkhxTheme::changed`.
- Editing `THEMENAME` in `gtkhxrc` while the app is running fires
  the same loader and re-emits `changed`; every button, user-list
  view, and chat xtext rescales and repaints in place. (The Theme
  combo in Settings → Appearance still drives Adwaita's light/dark
  mode, which is a separate axis.)
- There is no filesystem watch on the theme file itself. To pick up
  edits to the active theme without an app restart, briefly change
  `THEMENAME` to another value and back.

## How loaders handle missing data

| Situation | Behavior |
|---|---|
| `THEMENAME` unset or empty | Falls back to `"default"`. |
| `$CONFIG/themes/<name>.ini` missing | Falls back to the built-in default GResource. |
| Theme file present but unparseable | Logs a warning, falls back to the built-in default. |
| Scale key missing | Inherits the built-in default for that area. |
| Scale value out of range | Clamped to `[50, 300]` on load. |
| Scale value `≤ 0` | Treated as "unset" → inherits the default. |
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
- Palette consumer: `src/chat.c::gtkhx_apply_theme_palette()`.
- Scale consumers: button helpers in `src/gtkutil.c`, `src/users_view.c`'s
  `measure` / `snapshot`.
- Tests: `tests/unit/test_theme_scale.c`.
