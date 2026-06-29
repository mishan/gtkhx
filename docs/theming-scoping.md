# Theming — feasibility & scoping

Status: per-area UI scaling is **implemented** on `claude/theming` (GtkhxTheme +
Settings UI + tests — see the "IMPLEMENTED" section below); icon replacement and
the color palette are scoped here and tracked on the `claude/icon-packs` and
`claude/theming-palette` branches. This doc surveys what exists, judges how hard
each piece of "make GtkHx themable" is, and proposes the architecture the three
branches build out.

## Goal (from Misha)

Make GtkHx themable along three axes:

1. **Icon replacement** — swap the chrome/button icon set (the GResource
   `/com/nasledov/gtkhx/pixmaps/` PNGs only). The Hotline user icons in
   `icons.rsrc` (cicn) are *out of scope for replacement* — they're unrelated to
   this axis. They are still in scope for **scaling** (see axis 2).
2. **Per-area UI scaling** — independent scale for toolbar buttons, other-window
   buttons (Users / Files / News / Tasks / …), and user-list icons + text.
3. **Colors** (xtext widget and friends), with **light/dark variants** definable
   for color-bearing parts of a theme.

The existing `claude/ui-scale` branch is a good start but has two problems we
explicitly want to fix:

- Parts of the UI are *already* upscaled to 125–150% at "100%", so calling the
  knob "100%" is misleading.
- It's a single global knob; we want per-area control.

---

## What exists today (inventory)

### Icons — two unrelated systems

There are two completely separate icon pipelines, and they matter differently for
theming.

**(a) Chrome / button icons — PNG GResource.** ~50 PNGs listed in
`src/gtkhx.gresource.xml` under `/com/nasledov/gtkhx/pixmaps/`, loaded with
`gdk_pixbuf_new_from_resource(...)`. These are the 16×16 pixel-art glyphs on the
toolbar and on the Users / Files / News / Tasks action buttons, plus the
file-type icons for the new files browser (`file_*.png`, `folder*.png`). They are
upscaled at runtime with `GDK_INTERP_NEAREST` to keep the pixels crisp. Compiled
into the binary at build time; no runtime override path exists.

**(b) Hotline user icons — `icons.rsrc` (cicn).** A 388 KB Mac-classic resource
fork holding **613 `cicn` color icons**, decoded by `src/cicn.c` into `GdkPixbuf`.
These are the Hotline avatar icons a user picks in Settings (the `GtkFlowBox`
icon-picker grid) and that render next to each name in the user list. **Not part
of the icon-replacement axis** — a theme does not swap these. They are, however,
a UI surface that gets **scaled** (user-list icon size, picker-grid size), so they
show up under axis 2. The icon *ID* is wire-protocol (`HTLC_DATA_ICON`), which is
another reason theming leaves them alone.

Implication for replacement: the icon-replacement feature is exactly one thing —
reskinning the ~50 chrome glyphs in (a). Self-contained, no protocol surface.

### Scaling — the `claude/ui-scale` branch

- One pref, `CFG_UI_SCALE` (int percent, default 100, clamped [50, 200]), read
  through the inline `gtkhx_ui_scale()` in `prefs.h`. Hot-reloads via a
  `ui-scale-changed` signal on the `GtkhxSession` singleton.
- **Every consumer already carries a hardcoded base scale**, and the global knob
  multiplies *on top* of it:

  | Area | Base scale (at global 100%) | Where |
  |---|---|---|
  | Toolbar buttons | `TOOLBAR_ICON_SCALE = 2` (200% of 16px → 32px) | `toolbar.c` |
  | Tasks buttons | `TASKS_ICON_SCALE = 2` | `tasks.c` |
  | Tasks row icon | `GTASK_ICON_SCALE = 2` | `tasks.c` |
  | Files-browser buttons | literal `2` (`FB_BTN`) | `files_browser.c` |
  | Users / News buttons | `1` (no base bump) | `users.c`, `news_browser.c` |
  | User-list icon + text | `pixel_scale` = `1.0` *or* `1.25` (1.25 in the standalone Users window) | `users_view.c` |

- Mechanically the hot-reload plumbing is already nicely factored:
  `button_compute_scaled_size()` / `button_refresh_picture()` in `gtkutil.c` do
  `base_scale × gtkhx_ui_scale()`; buttons stash their source + base scale as
  object data and subscribe to `ui-scale-changed` via `g_signal_connect_object`
  (auto-unsubscribe on finalize). Fonts fold the scale into CSS in
  `pango_to_css_props()` and into `gtk_xtext_set_font` via
  `gtkhx_scaled_font_name()`. The user list reads `gtkhx_ui_scale()` live in its
  `measure`/`snapshot`.

**This table is the root of complaint #1.** The "100%" knob sits on top of a
pile of per-area constants (2×, 2×, 1.25×, …). So global 100% already means the
toolbar renders at 200% of its source art.

### Colors / CSS — partly themed already, but not user-configurable

- The xtext chat palette is a **static 38-entry `GdkRGBA colors[]` array** in
  `chat.c` (mIRC 0–31, then UI roles: MARK_FG/BG, XTEXT_FG/BG, MARKER,
  HISTORY_MUTED). Hardcoded literals; not user-editable.
- **Light/dark already works** for that palette via
  `gtkhx_apply_theme_palette(gboolean dark)` in `chat.c`: it rewrites only the
  UI-role slots (mIRC 0–31 are deliberately theme-invariant because servers
  expect exact values) and is driven by `AdwStyleManager::notify::dark`
  (`gtkhx.c`). So the *mechanism* for a two-variant (light/dark) color set is
  already in place — it's just sourced from compiled-in constants, not from prefs
  or a theme file.
- Font is user-configurable (`CFG_*` font pref → `pango_font_description`,
  applied through `.gtkhx-text` / `.gtkhx-userlist` CSS providers in `gtkhx.c`
  and `gtk_xtext_set_font`). Theme (light/dark/system) is a pref applied via
  `adw_style_manager_set_color_scheme`.

So we already have: a per-slot color model, a working light/dark switch, live CSS
providers, and a hot-reload signal bus. We do **not** have: any of it driven by
user data.

---

## Feasibility verdict, axis by axis

### 1. Icon replacement — **feasible, self-contained**

Scope is just the ~50 chrome glyphs in the GResource. All loads funnel through
`gdk_pixbuf_new_from_resource(name)` (and `button_load_source()` in `gtkutil.c`).
Introduce one resolver — `gtkhx_icon_load(logical_name)` — that checks an active
icon-pack directory (e.g. `$CONFIG/icons/<pack>/<name>.png` or `.svg`) and falls
back to the GResource defaults. ~50 names, all already symbolic; no protocol
surface; no cicn involvement. Switching pack sources to SVG would also kill the
nearest-neighbor upscaling hack and make per-area scaling look better at large
sizes. The cicn user icons are untouched by this axis.

### 2. Per-area scaling — **feasible; this is mostly a refactor of what's there**

The `ui-scale` plumbing already proves the hard parts (live resize, signal
fan-out, CSS folding). The redesign is about *replacing one knob + scattered base
constants with a small set of named, honestly-labeled knobs.* See the design
section below — this is the core recommendation and it fixes both complaints.

### 3. Colors with light/dark variants — **feasible; most net-new code, but on rails**

The model (`colors[]` + role slots), the switch (`apply_theme_palette(dark)`),
the live-refresh (push palette into every xtext), and the dark-tracking signal
all exist. The work is to **lift the palette out of compiled constants into a
theme source that carries a light set and a dark set**, then have
`apply_theme_palette` read the active variant from there instead of literals.
Expose the editable subset (at least XTEXT_FG/BG, mark colors, marker, muted; the
mIRC 0–31 can stay locked or be "advanced") in Settings. The user-list text/icon
and other CSS-driven surfaces can take colors the same way (extend the existing
`.gtkhx-*` providers with theme-sourced color props).

Nothing here requires new toolkit capability — it's wiring user/file data into
existing sinks.

---

## Recommended architecture: a `GtkhxTheme`

Pull the three axes under one concept rather than three unrelated prefs blobs.

A **theme** is a named bundle of:

- **icon pack** — a directory (or "built-in") of chrome glyphs resolved by a
  single `gtkhx_icon_load()` chokepoint, with fallback to the GResource defaults
  (cicn user icons are not part of the pack);
- **scale set** — the named per-area scales (below);
- **palette** — per-slot colors, each with a **light** and a **dark** value,
  consumed by `gtkhx_apply_theme_palette()` and the `.gtkhx-*` CSS providers.

Storage: a `GKeyFile` per theme (`$CONFIG/themes/<name>.ini`) mirrors how prefs
already persist, is hand-editable, and is shareable. The built-in default theme
is the current appearance, expressed as data. A single `GtkhxTheme` GObject holds
the active theme; mutating it emits the existing-style change signals
(`theme-changed`, subsuming `ui-scale-changed`) on `GtkhxSession`. Every consumer
already subscribes to a session signal, so the hot-reload bus is reused, not
rebuilt.

This keeps the door open to the "ship/share a theme" UX later and to bundling a
couple of presets (e.g. "Classic", "Modern", "High-contrast").

### Per-area scaling, done honestly — IMPLEMENTED (now theme-file backed)

> **Update — theme files landed.** The initial scaling impl cut a
> corner and stuffed the four `scale_*` overrides into
> `gtkhx_prefs` / `gtkhxrc`. That's been corrected: all per-axis
> theming state — scales today, palette colors as of the same
> branch, future axes too — now lives in a GKeyFile theme file at
> `$CONFIG/themes/<name>.ini` (with the built-in default shipped
> as a GResource). The only theming key in `gtkhxrc` is
> `THEMENAME`. The four `CFG_SCALE_*` keys and the
> `gtkhx_prefs.scale_*` fields are gone, the "UI Scaling" group
> on the Settings → Appearance page is gone, and the on-disk
> shape now matches what this scoping doc described from the
> start (see the Storage paragraph under "Recommended
> architecture"). Full schema reference:
> [theming-file-format.md](theming-file-format.md). The C/H
> shapes summarised below still hold — only the storage source
> changed.


Replace the single `CFG_UI_SCALE` + scattered base constants with a fixed set of
named scales. The model that kills complaint #1: **the unscaled source art is the
true 100%**, and a *theme* supplies a per-area scale. The built-in **default
theme** carries the real factors GtkHx has always applied as explicit values
(buttons 200%, the standalone Users window 125%) — not relabeled as 100%. A user
override, when set, replaces the default-theme value for that area. So a fresh
install reproduces today's look *and* Settings shows the honest 200% / 125%.

| Area (`GtkhxScaleArea`) | Replaces | Default-theme % |
|---|---|---|
| `GTKHX_SCALE_TOOLBAR` | `TOOLBAR_ICON_SCALE` (2) | 200 |
| `GTKHX_SCALE_WINDOW_BUTTONS` | `TASKS/FB/news/tracker/users` button `2`s | 200 |
| `GTKHX_SCALE_USERLIST_ICON` | `users_view` `pixel_scale` 1.25 (icon half) | 125 |
| `GTKHX_SCALE_USERLIST_TEXT` | `users_view` `pixel_scale` 1.25 (font half) | 125 |

How it landed:

- `src/gtkhx_theme.{c,h}` — a `GtkhxTheme` singleton GObject with a `changed`
  signal. `gtkhx_theme_scale(area)` returns the factor a call site multiplies
  into its raw source size; `default_theme_pct[]` holds the shipped factors;
  the user override lives in `gtkhx_prefs.scale_*` (`0` = "unset → default
  theme"). Persisted via the existing `cfgvars[]` GKeyFile path
  (`CFG_SCALE_*`), with `changed_scale` → `gtkhx_theme_notify_changed()`.
- **Buttons:** `gtkhx_pixmap_button` / `gtkhx_pixbuf_button` lost their `int
  scale` arg for a `GtkhxScaleArea area`; the helper renders at `source ×
  gtkhx_theme_scale(area)` and subscribes each button to the theme `changed`
  signal (auto-unsubscribed on finalize) so Settings rescales it live. The old
  per-call `2` and the `TOOLBAR_ICON_SCALE` / `TASKS_ICON_SCALE` constants are
  gone — there is no hidden multiplier left, exactly one factor source.
- **User list:** `users_view.c` reads the icon/text scales **live** at
  measure/snapshot (no per-cell state to refresh); icon, text-offset and row
  height follow `USERLIST_ICON`, font follows `USERLIST_TEXT`. The themed path
  is the standalone Users window; the compact chat-sidebar list keeps its fixed
  1.0 structural density (a single per-area default can't honestly reproduce two
  different current factors, so the knob is scoped to the prominent window — a
  documented limitation, revisit if the sidebar should follow too).
- **Settings:** a "UI Scaling" group on the Appearance page with four
  `AdwSpinRow`s (`pref_scale_spin_row` seeds the displayed value from the
  *effective* percent so the 0-sentinel never clamps the row to the floor).
- **Test:** `tests/unit/test_theme_scale.c` pins the clamp matrix, the default
  factors, the unset→default fallback, override clamping, and the `changed`
  emission.

Not yet done on this axis (follow-ups): `chat_font` as a named area (chat/PM font
still goes through the existing font pref); task-row (non-button) icons still use
`GTASK_ICON_SCALE`; the compact sidebar exclusion above.

### Reuse vs. rework of `claude/ui-scale`

- **Kept (as patterns):** the `button_load_source` / `button_refresh_picture`
  shape and the live-`measure` user-list approach were re-derived on `main`
  against the non-deprecated `gtkhx_texture_from_pixbuf` path. The single-knob
  design itself was *not* reused — it's the thing being corrected.
- **Reworked:** one global pct → named per-area scales; the misleading "global
  100% on top of hidden 2×" → "source is 100%, default theme owns the real
  factor as an override."
- **Branch:** built fresh on `claude/theming` off current `main` (the
  `ui-scale` branch was behind `main` and carried the design we're replacing), so
  no rebase of that branch was needed.

---

## Risks / watch-items

- **cicn user icons are out of scope for replacement.** They're a separate system
  (Mac resource fork, protocol-numbered IDs) and the theme does not swap them.
  They only participate in *scaling* (user-list icon + picker grid).
- **mIRC palette slots 0–31 are semantically fixed.** Servers send specific color
  indices and expect specific colors. Make these locked/advanced in any color
  editor; default theming targets the UI-role slots (32–37) and CSS surfaces.
- **xtext input fields stay monospace-pinned** (`gtk_text_view_set_monospace`) due
  to a known ascender-clip issue noted on the branch — per-area font scaling has
  to respect that carve-out.
- **Wire-compat is untouched.** None of this goes near `rcv.c` / `commands.c` /
  `hotline.h`. Pure presentation layer. (The one protocol-adjacent surface — the
  icon *ID* the user picks — already exists and doesn't change.)
- **SVG icon packs → go through glycin**, not librsvg/GdkPixbuf directly. We
  already ship glycin via the `hx-image-decode` Rust crate (inline-media / banner
  / chat decode through it; see `docs/glycin-migration-plan.md`), so an SVG pack
  reuses that pipeline instead of adding a dep. Two implications the resolver has
  to absorb, both already precedented in the crate:
  - **Async-only.** glycin decode returns via callback, unlike the current
    synchronous `gdk_pixbuf_new_from_resource`. For ~50 small chrome glyphs the
    clean pattern is decode-once-into-a-cache at startup / on theme-switch (keyed
    by logical name × target px), then buttons pull synchronously from the cache.
    `button_refresh_picture` already rebuilds lazily off a signal, so a
    "pack-loaded" / `theme-changed` emission to trigger the rebuild fits the
    existing bus.
  - **Returns `GdkTexture`, not `GdkPixbuf`.** This actually *fits better* than
    today's path — `button_refresh_picture` already ends at a `GdkTexture` +
    `GtkPicture`, so an SVG-sourced texture skips the pixbuf→texture round-trip
    (and the deprecated `gdk_texture_new_for_pixbuf`). The wrinkle is scaling:
    raster packs scale a pixbuf with `gdk_pixbuf_scale_simple`, but SVG wants to
    be **rendered at the target pixel size** — glycin can decode vectors at a
    requested size, so the per-area scale feeds the decode request rather than a
    post-scale. Resolver branches: built-in/PNG pack → pixbuf path; SVG pack →
    glycin-at-size path. Both converge on a `GdkTexture` for the button/picture.
  - PNG-only packs remain the zero-Rust, fully-synchronous fallback (at the cost
    of the upscaling-blur question at large scales), so the SVG path can be a
    follow-up rather than a blocker for shipping icon replacement.
- **Settings surface growth.** Several new knobs + a color editor + an icon-pack
  picker. The existing `AdwPreferencesDialog` "Appearance" page is the home;
  budget UI time, and consider an "advanced" disclosure for the mIRC palette.

## Rough effort sketch

| Piece | Effort |
|---|---|
| `GtkhxTheme` object + GKeyFile load/save + default-as-data | 2–3 days |
| Per-area scaling refactor (fold constants, absolute units, split icon/text) | 3–5 days |
| `gtkhx_icon_load()` resolver + chrome icon-pack override | 2–3 days |
| Color palette → theme-sourced light/dark + Settings color editor | 4–6 days |
| Settings UI (Appearance page expansion, pickers, hot-reload) | 3–4 days |

Evenings/weekends: multiply by ~3, as the ROADMAP notes elsewhere.

## Status and suggested next step

The **per-area scaling refactor** shipped on `claude/theming` (see the
"IMPLEMENTED" block above): `GtkhxTheme` singleton, four named scale areas, the
default-as-data pattern, `theme-changed` signal bus, Settings UI, and a unit
test. Both of Misha's original complaints are addressed.

What's still on the table, in rough order of value-per-effort:

1. **Theme editor UI** — a Settings → Appearance theme editor:
   scale spin rows (returning the affordance that the file-format
   refactor temporarily removed), six color-picker rows for the
   UI-role palette, a "Save as" path for forking a theme, and
   (eventually) import / export. Storage is in place — this phase
   is pure UI work, plus a small write-back path so a Settings
   edit modifies the active theme file. The theme *picker* shipped;
   the *editor* is still out-of-scope per Misha's "that's a whole
   theme-editor thing" call.
2. ✅ **Chrome icons bundle with themes** — landed on
   `claude/icon-packs`. `src/gtkhx_icon.{c,h}` is the single
   resolver chokepoint. Themes can ship as flat `.ini` (no icons)
   or as a directory `<name>/theme.ini` plus `<name>/icons/*.png`;
   per-icon GResource pixmap fallback keeps partial bundles
   working. There's no separate icon-pack pref — picking a theme
   picks the look end-to-end. SVG bundles via glycin /
   `hx-image-decode` remain the v2 follow-up — async decode +
   `GdkTexture` direct return + decode-at-target-size for
   vectors, all precedented in that crate. Schema in
   [theming-file-format.md](theming-file-format.md).
3. **Follow-ups on the scaling refactor**: lift `chat_font` into a
   named theme axis (currently the chat/PM font is still a separate
   non-theme pref); revisit the compact chat-sidebar exclusion if a
   second `USERLIST_*` variant turns out to be wanted.
   (`tasks_row_icon` already landed as `GTKHX_SCALE_TASKS_ROW_ICON`.)

---

## Parked: CSS-as-theme-file vs. our .ini schema

The current model uses a GKeyFile `.ini` for theme state plus a
small set of `.gtkhx-*` CSS providers that the loader emits at
runtime from the .ini's palette values. An alternative is to let
themes BE CSS files — written by the user in standard GTK CSS,
loaded directly via `GtkCssProvider`. Worth thinking about; not
worth implementing right now. Capturing the trade-off here so
the decision is informed when we come back to it.

### What's actually CSS-shaped in our model

Already pure CSS (lives in `gtkhx_refresh_css` /
`gtkhx_refresh_userlist_css`):

- `.gtkhx-text` / `.gtkhx-input` — read-only text view + editable
  input fg/bg/caret. Already a CSS provider; a user-supplied CSS
  file could replace it 1:1.
- `.gtkhx-listview` / `.gtkhx-userlist` — listview row colors,
  including the `:not(:hover):not(:active)` carve-out so hover
  feedback survives.

Not CSS-shaped — these can't move to CSS without first restructuring
the C-side machinery:

- **Per-area scales** (`GTKHX_SCALE_TOOLBAR`,
  `GTKHX_SCALE_WINDOW_BUTTONS`, `GTKHX_SCALE_USERLIST_ICON`,
  `GTKHX_SCALE_USERLIST_TEXT`, `GTKHX_SCALE_TASKS_ROW_ICON`).
  These multiply *source* pixmap sizes at PNG-decode time inside
  `gtkutil.c::button_load_source`, `tasks.c::gtask_make_icon`, and
  the user-list `measure`/`snapshot`. They're not styling — they're
  load-time integer factors fed back into C. GTK CSS has no native
  way to express "decode a 16×16 PNG at 200% via nearest-neighbour
  before handing it to a button."
- **xtext chat palette** (38 GdkRGBA slots, see
  `chat.c::gtkhx_apply_theme_palette` and `gtk_xtext_set_palette`).
  xtext is a custom widget that draws with cairo from its own
  `colors[]` array; it never consults `gtk_widget_get_style_context`.
  The palette has to be a C-readable structure.
- **User-list name colors** (`GTKHX_USER_COLOR_ACTIVE` …
  `GTKHX_USER_COLOR_ADMIN_IDLE`, queried in
  `users.c::user_color_gdk`). `hx_user_cell_name_snapshot` calls
  `gtk_snapshot_append_layout(snapshot, layout, &fg_color)` with
  an explicit `GdkRGBA`. Same story: no CSS consultation.
- **Icon-pack bundling** (`gtkhx_icon.{c,h}` resolver looking up
  `$CONFIG/themes/<name>/icons/<logical>.png`). File resolution,
  not styling.

### Four viable paths

1. **Hybrid** — keep the `.ini` for the non-CSS-shaped state
   (scales / xtext palette / user-list / icon-pack name), ALSO
   load an optional `style.css` companion from the theme bundle as
   a `GtkCssProvider` layered over the existing `.gtkhx-*`
   providers. Modest implementation (loader-hook + provider
   attach); both audiences happy — trivial themes stay readable
   key=value, theme authors who want full CSS power for the
   CSS-shaped surfaces get it. UX wart: a theme bundle can carry
   two files (`theme.ini` + `style.css`).

2. **Pure CSS, full migration** — make scales custom CSS
   properties (`--gtkhx-scale-toolbar: 200%;`, queried via
   `gtk_style_context_lookup_color`-style machinery, with a small
   parser to pull integer percents out of CSS variables). Rework
   xtext to consult CSS for its palette (significant — xtext is
   ~4500 LOC and the cairo draw path nowhere touches the GTK
   style context today). Rework `users_view.c::hx_user_cell_name_snapshot`
   to consult CSS for name colors (smaller, but the cell-snapshot
   path also has to invalidate on theme changes). Drop the `.ini`
   entirely. Win: theme authors learn one format that's already
   familiar from web/GTK. Cost: weeks of work; real regression risk
   in the chat rendering; xtext gains a CSS-consultation surface
   we'd have to maintain against future xtext upstream merges from
   HexChat. Authors of trivial themes (a colour swap) end up
   writing more CSS than they would key=values.

3. **Status quo + style.css as a power-user hook** — don't change
   the loader at all. Document that a theme bundle may ship a
   `style.css` next to `theme.ini`; the loader attaches it as a
   `GtkCssProvider` (one-line wiring). All the existing `.gtkhx-*`
   rules keep emitting from the .ini. Smallest possible change,
   narrowest audience (those who'd write CSS for it). Effectively
   path 1 minus the layering question (just "and also CSS is here
   if you want it"). Could absorb path 1 later without breaking
   anything.

4. **Drop the theme-file format, just expose CSS** — stop shipping
   `default.ini` / `solarized.ini`, stop loading theme files.
   Bundled icons go away (no theme bundle to put them in). Users
   do everything through a single CSS file. Loses scales, palette
   overrides, user-list color overrides, icon-pack bundling
   entirely — accepts they're only worthwhile if expressible in
   pure CSS. Most aggressive simplification; loses real
   capability.

### Lean

Path 1 (hybrid) is the smallest change that gets the "you can
write CSS if you want" power-user affordance without the xtext /
snapshot surgery path 2 demands. Theme authors who don't want to
touch CSS keep the readable `.ini` for the common cases (colour
swap, scale tweak, drop in some icons); authors who want to do
something fancy with hover states or per-cell styling drop a
`style.css` next to the `theme.ini`. The two-files-per-bundle wart
is mild.

Path 2 is the right long-term endpoint if the GtkHx community
grows enough theme authors that the .ini schema starts feeling
limiting — but at that point the xtext / snapshot rework is
amortized across a real user base. Today it'd be a lot of work
for a small audience.

Path 3 is path 1 deferred; could land as a stepping stone if path
1's loader-hook turns out larger than expected.

Path 4 is probably not worth it — the scales and palette
overrides solve real problems (the Solarized + chunky-icons combo
that motivated the bundle model in the first place; the
admin/idle name colors that motivated the user-color theme axis).
Throwing them away to land on one format is a regression for
users who picked Solarized expecting it to work.

### What to do when we come back

If path 1: add a `style.css` loader hook to `gtkhx_theme.c::gtkhx_theme_load_active`,
attach it via `gtk_style_context_add_provider_for_display` at
`PRIORITY_APPLICATION + 1` (above our `.gtkhx-*` rules so it can
override them). One-section doc addition to
`theming-file-format.md`. ~50 LOC + the doc.

If path 2: scope as its own multi-week phase. xtext CSS
consultation is the long pole. Bench against real Solarized + a
hand-written custom theme to make sure the chat output doesn't
regress.

Either way, no urgency — current model works and the user-facing
feedback we've gotten so far has all been about specific theme
behaviours (hover states, name colours, listview backgrounds),
not about wanting a different schema.
