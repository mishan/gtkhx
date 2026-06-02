# users.c — GtkHList → GtkColumnView migration scoping

Status: Phases A + B + C shipped on `claude/users-columnview`. Phase D
(`prompt_chat` pchat-picker dialog list) and Phase E (test additions
+ shim deletion) remain — both pinned for follow-up branches. The
standalone Users window and the per-pchat sidebars are both
GtkColumnView-backed now; the only remaining `gtk_hlist_*` call in
users.c is inside `prompt_chat`.

Goal: replace the `gtk_hlist_compat` (GtkTreeView+GtkListStore shim)
backing of the standalone Users window and the pchat / chat user
lists with a native GtkColumnView. Same playbook tracker.c already
followed — `tracker_row.{c,h}` + `tracker_event.{c,h}` + per-section
GListStore → GtkFilterListModel → GtkSortListModel → GtkSingleSelection
→ GtkColumnView with factory-bound cells.

This is one of the remaining three GtkHList consumers
(`users.c`, `news15.c`, `options.c` — `tracker.c` and `files.c` are
done). Migrating users.c also drags chat.c with it; see §1 below.

---

## 1. Current state

### What users.c does with GtkHList

The standalone Users window (`create_users_window`) builds a 2-column
list — `UID` and `Name` — over `sess->users_list`. Operations on
it use the gtk_hlist API surface listed below.

The same widget abstraction is reused for **per-pchat and main-chat
user lists** — `chat.c::create_chat` and `chat.c::create_pchat_window`
both build a 2-column GtkHList stashed on `gchat->userlist`.
`users.c::user_create` / `user_change` / `user_delete` dispatch on
`chat->cid == 0` to pick between `sess->users_list` and
`gchat->userlist`, so the migration has to land them at the same time.

### API surface used in users.c + chat.c userlist creation

Construction / chrome:

- `gtk_hlist_new_with_titles (2, titles)`
- `gtk_hlist_set_column_width (col, px)`
- `gtk_hlist_set_row_height (24)` (18 in pchat lists — see §3.4)
- `gtk_hlist_set_shadow_type (NONE)` — already a no-op
- `gtk_hlist_set_column_justification (1, LEFT)`
- `gtk_hlist_column_set_overlay_pixtext (1, 36)` — Mac-classic
  icon-as-background + name-overlay-at-fixed-offset (22 in pchat)
- `gtk_hlist_column_set_overlay_decoration (1, 1.25, TRUE)` —
  pixel-scale + text-outline (Users window only)

Bulk update:

- `gtk_hlist_freeze` / `gtk_hlist_thaw` (in `user_list`, reload pass)
- `gtk_hlist_clear` (`user_list`, `users_clear`)

Row CRUD:

- `gtk_hlist_append (text[])` → row index
- `gtk_hlist_insert (row, text[])` (used in `user_change` to preserve
  position)
- `gtk_hlist_remove (row)`
- `gtk_hlist_set_text (row, col, nam)`
- `gtk_hlist_set_pixtext (row, col, nam, spacing, pixmap, mask)`
- `gtk_hlist_set_foreground (row, GdkRGBA*)` — per-row foreground
  (drives the colored-nicknames + status palette in `user_create` /
  `user_change`)
- `gtk_hlist_set_row_data (row, user)` /
  `gtk_hlist_get_row_data (row)` /
  `gtk_hlist_find_row_from_data (user)`

Sort + selection + hit-testing:

- `gtk_hlist_set_compare_func (users_sort)` — global comparator that
  reads the file-static `user_click_col` to choose UID-numeric vs.
  name-case-insensitive ordering
- `gtk_hlist_sort ()` — called from `usercol_clicked` after the user
  clicks a column header
- `click_column` signal → `usercol_clicked` (sets `user_click_col`,
  re-sorts)
- `gtk_hlist_select_row (row, 0)`
- `gtk_hlist_get_selection_info (x, y, &row, &column)` — pixel→row
  hit-testing for right-click context menu

State threaded through globals:

- file-static `user_storow`, `user_stocolumn` — stamped on primary
  press in `user_pressed`; read by every toolbar button handler
  (`open_message_btn`, `user_info_btn`, `user_kick_btn`,
  `user_igno_btn`, `user_ban_btn`, `user_chat_btn`) to recover the
  selected `struct hx_user *` via `gtk_hlist_get_row_data`
- file-static `user_click_col` — set on column-header click, consumed
  by `users_sort`

### prompt_chat's separate list

`users.c::prompt_chat` builds a small **unrelated** 2-column GtkHList
of existing pchats inside an AdwAlertDialog (`CID` / `Subject`). It's
the only place in users.c that uses `gtk_hlist_new_with_titles` +
`select_row` signal + `gtk_hlist_set_selection_mode (SINGLE)`. Easy
to migrate in passing or to leave behind on the compat shim — see §6.

### Reference: tracker.c already did this

`tracker.c` + `tracker_row.{c,h}` is the closest existing template.
Key elements we'll reuse:

- `HxTrackerRow` GObject wrapping the data needed for the row,
  registered with `G_DECLARE_FINAL_TYPE` (`tracker_row.h:38`).
- GListStore of `HX_TYPE_TRACKER_ROW` (`tracker.c:932`).
- `GtkColumnView` over a `GtkSingleSelection` over
  `GtkSortListModel(GtkFilterListModel(GListStore))`
  (`tracker.c:957–971`). The selection has `autoselect=FALSE` +
  `can_unselect=TRUE` so toolbar actions only fire on
  user-clicked rows.
- Per-column add via a local `add_column` helper that registers a
  setup-fn / bind-fn / GtkCustomSorter (`tracker.c:978–991`).
- Header sorter plumbed back into the sort model:
  `gtk_sort_list_model_set_sorter (sort_model,
  gtk_column_view_get_sorter (cv))` (`tracker.c:998`).
- Right-click via `GtkGestureClick` with
  `GDK_BUTTON_SECONDARY` + capture-phase, then walk up from
  `gtk_widget_pick(...)` to find the `GtkListItem` that owns the
  pressed widget (`tracker.c:1030–1040`). This is the GTK 4-native
  replacement for `gtk_hlist_get_selection_info`.
- Activate (Enter / double-click) → `GtkColumnView::activate`
  signal carries the row position; selection model resolves it to
  the row object.

`files_browser.c` is a second reference for the same patterns at
larger scale.

---

## 2. Target architecture

Two new files mirror the tracker.c split:

- `src/users_row.{c,h}` — `HxUserRow` GObject. Wraps a borrowed
  `struct hx_user *` plus the display state the cell needs (icon
  pixbuf, computed foreground, computed name string). Lifetime: the
  per-chat `chat->users` GHashTable still owns the underlying
  `struct hx_user`; the row owns nothing but its GObject self and a
  ref on the icon pixbuf. Notify-changed signals on row property
  invalidations re-trigger the sort model. (Alternative: don't
  introduce `HxUserRow`, just box `struct hx_user *` directly via a
  trivial GObject wrapper — but having a real properties-bearing
  GObject means we can hand it to `GtkBuilderListItemFactory` later
  or bind cells to row properties.)
- `src/users_view.{c,h}` — `HxUserListView` (a thin GObject or just
  a struct + factory function). Builds the GtkColumnView, holds the
  GListStore + selection model, exposes a small API:
  - `hx_user_list_view_new (session *sess)`
  - `hx_user_list_view_get_widget (v)` (returns the GtkColumnView
    or its scroller, ready to drop into the window)
  - `hx_user_list_view_add (v, struct hx_user *)`
  - `hx_user_list_view_remove (v, struct hx_user *)`
  - `hx_user_list_view_update (v, struct hx_user *, nam, icon, color)`
  - `hx_user_list_view_clear (v)`
  - `hx_user_list_view_get_selected (v)` → `struct hx_user *` or NULL

`sess->users_list` becomes an `HxUserListView *` instead of a
GtkWidget *; same swap for `gchat->userlist`. The view exposes its
toplevel widget separately so packing into windows / scrolled
windows is unchanged at call sites.

users.c keeps:

- `hx_user_new` / `hx_user_delete` / `hx_user_with_uid` /
  `hx_user_with_name` (already model-side; no widget calls).
- The right-click `user_popup` and all on_user_* handlers.
- `user_color_gdk` / `user_nick_color_gdk`.
- `user_kick_user`, `hx_get_user_info`, `hx_change_name_icon`,
  `users_attach_click_gesture` (becomes a thin wrapper that just
  forwards the gesture install — or moves inside `HxUserListView`
  since it's where it conceptually belongs).
- `create_users_window` (now packs an `HxUserListView` into the
  window instead of building the GtkHList directly).
- `prompt_chat` — see §6.

users.c loses:

- `user_storow` / `user_stocolumn` globals (replaced by
  `hx_user_list_view_get_selected`).
- `user_click_col` global + `users_sort` comparator +
  `usercol_clicked` (replaced by per-column GtkCustomSorters
  bound through `gtk_column_view_get_sorter`).
- `user_list` body shrinks to `hx_user_list_view_clear` +
  `hx_user_list_view_add` loop (or a `_set_users` bulk-replace
  using `g_list_store_splice`).
- `user_create` / `user_change` / `user_delete` reduce to dispatch +
  one call into `HxUserListView`.

---

## 3. Visual fidelity — features to preserve

### 3.1 Mac-classic overlay rendering

`gtk_hlist_column_set_overlay_pixtext (col, text_x_offset)` installs a
custom GtkCellRenderer that draws the row's pixbuf at its **natural
width** starting at the cell's left edge, and draws the cell text on
top with margin-start = `text_x_offset` from the cell edge (36 px in
the Users window, 22 px in pchat lists). This keeps the name column
aligned across rows even when icons vary in width — wide banner-style
icons (60+ px on Badmoon & co.) render as the row background instead
of pushing names rightward.

In GtkColumnView land: write a small custom **drawing widget** that
holds (pixbuf, text, foreground RGBA, scale, outline-on) and
implements `snapshot()`:

1. Paint the pixbuf at natural dimensions (multiplied by pixel_scale)
   anchored to the start edge.
2. Paint a Pango layout for the text, positioned at
   `text_x_offset * pixel_scale` from the start edge, vertically
   centered.

If `text_outline` is on, walk an offset cross around the layout to
paint a 1-px contrasting outline before painting the foreground
color (cairo path stroking the layout outline is what the current
code does; the GtkSnapshot equivalent is `gtk_snapshot_append_layout`
with the four offsets then once at the foreground color).

The column-2 setup function in the factory binds this widget to the
HxUserRow's properties; the bind function refreshes them when the
row changes.

Column 1 (UID) stays a plain GtkLabel — no custom rendering needed.

### 3.2 Per-row foreground color

Currently driven by `gtk_hlist_set_foreground (row, GdkRGBA *)` from
`user_create` / `user_change`, computed via `user_nick_color_gdk`
(colored-nicknames extension preferring per-user RGB over the
status palette).

Translation: `HxUserRow` carries a `GdkRGBA *foreground` (NULL ⇒
theme default). The custom drawing widget reads it in `snapshot()`
and passes it to `gtk_snapshot_append_layout`. Recompute on every
update; emit notify on the property so the bound cell re-snapshots.

### 3.3 Pixel scale 1.25x

Users window uses `gtk_hlist_column_set_overlay_decoration
(col, 1.25, TRUE)` — both icon dimensions and the text font scale
to 125%. Pchat lists pass 1.0 (no decoration call).

The custom drawing widget takes `pixel_scale` as a property; both the
PangoFontDescription size and the pixbuf-paintable target size scale
by it. Independent per `HxUserListView` instance — Users window passes
1.25, pchat lists pass 1.0.

### 3.4 Row height

Users window: 24 px. Pchat / chat userlists: 18 px. Surface as a
construction parameter on `hx_user_list_view_new` (or a separate
setter).

### 3.5 Sort behavior

`users_sort` orders by UID numeric or name case-insensitive depending
on which header was clicked. Map to two `GtkCustomSorter`s, one per
column, registered on each `GtkColumnViewColumn`. GtkColumnView's
default header-click is asc → desc → unsorted (three-state cycle);
current GtkHList behavior is asc-only with each click. Recommend
moving to GTK's default cycle — see §6 Q1.

---

## 4. Interaction features to preserve

### 4.1 Double-click → open Msg window

Today: `user_pressed` checks `n_press == 2` and opens a msgwin.

After: GtkColumnView's `activate` signal carries the position; the
view's handler resolves to the row's `struct hx_user *` and calls
`create_msgwin` / `gtk_window_present`. Default activation includes
double-click + Enter — same as Tracker / Files.

### 4.2 Right-click → user_popup

Today: `user_pressed` on `GDK_BUTTON_SECONDARY` calls
`gtk_hlist_get_selection_info` to translate (x, y) → (row, col),
fetches `user = gtk_hlist_get_row_data (row)`, calls
`gtk_hlist_select_row (row, 0)` to highlight, then
`user_popup (list, user, sess, x, y)`.

After: `GtkGestureClick` on the GtkColumnView with
button=`GDK_BUTTON_SECONDARY`, capture phase. Handler picks the
deepest widget under (x, y), walks up to find the cell widget, reads
the `GtkListItem` off it (`tracker.c::on_section_secondary_press` is
the exact pattern), reads the row object off the list-item, calls
`gtk_single_selection_set_selected` to highlight, then opens
`user_popup` unchanged.

### 4.3 Toolbar button handlers

Today: `open_message_btn` / `user_info_btn` / `user_kick_btn` /
`user_igno_btn` / `user_ban_btn` / `user_chat_btn` all read
`gtk_hlist_get_row_data (users_list, user_storow)`.

After: each handler calls
`hx_user_list_view_get_selected (sess->users_list)` to recover the
row → `struct hx_user *`. `user_storow` and `user_stocolumn` go away.

This is also a small bug fix: the current code stamps `user_storow`
on every left-click, so toolbar buttons act on whichever row was last
clicked, *not* the currently selected one (relevant when keyboard
arrow-key navigation changes the selection without a fresh click).
GtkSingleSelection.selected-item is always the live selection.

### 4.4 Bulk reload

`user_list` currently does `freeze` → `clear` → emit `user_create`
for every user → `thaw`. The freeze/thaw don't actually do anything
in the compat shim, but they were a hint about "this is a bulk
operation; don't re-sort or repaint until I'm done."

After: `hx_user_list_view_set_users (v, GHashTable *users)` uses
`g_list_store_splice` to replace the whole list in one shot. Sort
model and selection model see a single change instead of N. Order
of insertion is irrelevant — the sort model handles ordering.

---

## 5. Phasing

### Phase A — view + row scaffolding

Implement `users_row.{c,h}` and `users_view.{c,h}`. No call sites
swap yet. `HxUserListView` is constructible in isolation and exposes
its widget. Custom overlay drawing widget tested visually in a
standalone test page (or compiled with a dev flag that swaps it in
on the Users window).

Includes: GObject registration, properties (icon, name, foreground,
pixel_scale, outline, text_x_offset), `snapshot()` implementation
with text-outline, sort/selection model wiring, GtkColumnView column
construction.

Exit: dev build shows the Users window using `HxUserListView`,
visual diff vs. main is acceptable.

### Phase B — Users window

Switch `create_users_window` to construct an `HxUserListView` and
pack its widget. Rewire `users_attach_click_gesture` and all toolbar
handlers in users.c to consume the view's selection. Drop
`user_storow`, `user_stocolumn`, `user_click_col`, `users_sort`,
`usercol_clicked`. Update `user_list` / `users_clear` /
`user_create` / `user_change` / `user_delete` to dispatch through
the view when `chat->cid == 0`.

Pchat path still goes through the gtk_hlist_compat shim — those
branches in `user_create` / `user_change` / `user_delete` are
untouched in Phase B.

Exit: standalone Users window fully on GtkColumnView; pchats
unchanged.

### Phase C — pchat / chat userlists

Rewire `chat.c::create_chat` and `chat.c::create_pchat_window` to
build `HxUserListView` instead of GtkHList. `gchat->userlist`
becomes `HxUserListView *`. Update the toolbar button handlers
(`open_message_btn` etc.) that take `users_list = data` to instead
take the `HxUserListView *` — they're shared with the standalone
window, so they should already be view-agnostic after Phase B.

Drop the `chat->cid == 0` dispatch arms in users.c's user_create /
user_change / user_delete — they reduce to a single view-targeted
call.

Exit: every user list in the app is GtkColumnView-backed.

### Phase D — prompt_chat picker list

Replace the `gtk_hlist_new_with_titles (2, titles)` inside
`prompt_chat` (Existing-Pchat selector inside the AdwAlertDialog)
with either a GtkColumnView (heavyweight for ~5 rows) or a GtkListBox
with a one-shot row factory (lighter, no sort needed; pchat list is
in arrival order anyway). Recommend GtkListBox — selection model
trivially maps to "which row is selected" without the sort/filter
layers.

This is the only remaining users.c gtk_hlist call after Phase C. Once
this is migrated, users.c no longer includes gtk_hlist.h.

Exit: users.c stops including gtk_hlist.h. The shim still exists for
news15.c and options.c.

### Phase E — cleanups + tests

- Drop `gtk_hlist_compat`-specific test coverage that's now redundant.
- Add unit tests for `HxUserRow` (foreground computation, icon
  binding) and `HxUserListView` (set_users splice, selection round-trip,
  user_change preserves selection identity).
- Tier 3: confirm the User-list interactions (double-click → msgwin,
  right-click → popover, sort cycle on header click) still work
  against mhxd, Janus, and Mobius.

---

## 6. Open questions for Misha

1. **Header-click sort cycle.** GtkColumnView default is
   asc → desc → unsorted. Current GtkHList behavior is asc-only
   (each click re-sorts asc on whatever column was clicked). Move
   to GTK default (recommended) or pin asc-only? Tracker already
   ships the default cycle.
2. **`prompt_chat` picker list.** Migrate in Phase D, or leave on
   the shim until news15.c / options.c also migrate and the shim
   gets deleted wholesale? It's ~30 lines of conversion either way.
3. **Pixel scale 1.25x.** Keep as-is (Users window only), or expose
   as a `gtkhx_prefs.users.text_scale` preference for accessibility?
   Out of scope unless requested — recommend keep-as-is.
4. **Text outline rendering.** Pixel-faithful via custom snapshot
   (current behavior, ~40 LOC in the snapshot fn) vs. CSS
   `text-shadow` four-offset approximation (simpler, slightly
   different anti-aliasing). Recommend pixel-faithful.
5. **HxUserRow vs. direct hx_user GObject-ification.** Introducing
   `HxUserRow` keeps `struct hx_user` a plain POD that the protocol
   model side owns, which matches `tracker_row` separating from the
   wire-event struct. Worth the extra type for parity, or just make
   `struct hx_user` itself a minimal GObject and skip the wrapper?
   Recommend `HxUserRow` for consistency with tracker.

---

## 7. Estimated LOC delta

Adds:

- `users_row.{c,h}` ≈ 150 LOC
- `users_view.{c,h}` ≈ 350 LOC (incl. column construction and
  the custom overlay drawing widget — ~120 LOC of that is the
  snapshot / pango / outline drawing)

Net removals:

- users.c: `user_storow`, `user_stocolumn`, `user_click_col`,
  `users_sort`, `usercol_clicked`, `user_pressed` hit-test branch,
  bulk of `user_create` / `user_change` (~30 lines each shrink to ~5),
  `user_list` (~20 → 5), `users_clear` (~10 → 2). Order ~150 lines net.
- chat.c: the two GtkHList construction blocks (~30 lines each)
  collapse to one `hx_user_list_view_new` + pack call (~3 lines each).
  ~50 lines.
- gtk_hlist_compat: once users.c and chat.c stop calling it, the
  remaining consumers are news15.c and options.c. The shim itself
  (~800 LOC) only gets deleted when both also migrate.

Conservative net: roughly +300 LOC inside users_view/users_row,
−200 LOC across users.c + chat.c, with ~800 LOC of compat shim
removal pending after the last two consumers migrate.

---

## 8. Risks / non-obvious bits

- **Selection identity across `user_change`.** Current code does
  `remove + insert at same row` so the visible position stays put;
  with a sort model in front of the store, position is determined
  by the sorter and is unaffected by mutation order — emit
  `g_list_model_items_changed` on the row (or replace the row via
  splice) and the selection model follows the row by identity, not
  by position. Net win: arrow-key cursor doesn't jump on rename.
- **Worker→main thread.** All user_create / user_change / user_delete
  calls come from the rcv.c → gtkhx_session signal pipeline, which
  is already main-thread. No new threading concerns; the
  `HxUserListView` mutators don't need locks.
- **Theme tracking.** Per-row foreground stays a raw GdkRGBA; the
  "regular user" slot returns NULL ⇒ theme default. The custom
  snapshot has to fall back to `gtk_widget_get_color (label)` when
  foreground is NULL so light/dark theme tracking keeps working.
- **CSS-node lifecycle.** GtkColumnView creates and recycles row
  widgets aggressively; the custom overlay widget needs to handle
  unmap/rebind cleanly (refresh icon paintable, pango layout) on
  each bind, not just on first construction.
- **`MAX_CONN > 1` deferred.** `HxUserListView` is constructed per
  session like the current users_list. When multi-conn lands
  (Phase 5 tabbed UI), each tab gets its own view instance — no
  shared global state in this migration.
