# Docking — recursive split tree (`HxSplit`)

Phase 5b reference + post-mortem. Companion to `docking.md`. This
file started as a design draft for the recursive-split rework;
once the work shipped, it was rewritten as the architecture
reference. The seven implementation findings below each
represent a debugging session worth preserving for the next
person touching this layer.

## What we built

The dock is **one recursive `HxSplit` tree**. Each tree node is
either:

- a **leaf**: a single `PanelFrame`, or
- an **internal split**: a `GtkPaned` (chosen orientation) whose
  two children are themselves `HxSplit` nodes.

Users can:

1. **Split any leaf** horizontally or vertically. The original
   frame becomes the start child; a fresh empty leaf becomes the
   end child.
2. **Move panels between leaves** via DnD (libpanel's existing
   gesture, unchanged) or via *Move left / right / up / down* in
   the panel chevron menu. With one unified tree, Move walks the
   whole dock — what were previously "area boundaries" (start /
   end / bottom / center) are transparent now.
3. **Close a frame** (a leaf). Any panels in the closing leaf
   first migrate to the sibling, then the leaf collapses; the
   sibling takes the parent split's place in the tree.
4. **Empty leaves stay visible** until explicitly closed. The
   discoverability win — "I just split this, now what?" — is what
   drove the design.
5. **Undock + Redock** unchanged. The panel chevron menu still
   has *Undock*; close-request on the undocked window walks the
   panel back to its home frame.

## Default layout

The first-launch tree:

```
root  (horizontal):
├── left leaf       — News                       (toolbar_sidebar_frame)
└── rest (horizontal):
    ├── middle (vertical):
    │   ├── center leaf  — Chat, Files, News 1.5 (toolbar_center_frame)
    │   └── bottom leaf  — Tasks                 (toolbar_bottom_frame)
    └── right leaf       — Users                 (toolbar_end_frame)
```

The four `toolbar_*_frame` pointers point at the four initial
leaves' `PanelFrame`s and stay **stable** when the user splits
those leaves — `hx_split_split` leaves the original frame in
place and manufactures a sibling. Static-panel factories
(users.c, tasks.c, news.c, news_browser.c, chat.c,
files_browser.c) keep using these pointers as their
`panel_frame_add` target.

## Architecture

### `HxSplit` (`src/hx_split.{c,h}`)

`G_DECLARE_FINAL_TYPE` of `GtkWidget`. State is mutually
exclusive — a node is either a leaf with a `PanelFrame`, or an
internal split with a `GtkPaned` and two child `HxSplit`s.
`GtkBinLayout` forwards size measurement to the single visible
child.

API:

| Function                       | What it does                                            |
|--------------------------------|--------------------------------------------------------|
| `hx_split_new()`               | Construct a leaf with a fresh empty `PanelFrame`.       |
| `hx_split_new_with_frame(f)`   | Construct a leaf wrapping an existing unparented `PanelFrame`. |
| `hx_split_new_internal(a,b,o)` | Construct an internal split directly from two unparented children. Used by the default-layout builder in toolbar.c. |
| `hx_split_split(leaf, o)`      | Convert a leaf into an internal split. Returns the new sibling leaf. |
| `hx_split_close_leaf(leaf)`    | Collapse the leaf; the sibling takes the parent's place. Refuses if the leaf still has pages — caller migrates them first. |
| `hx_split_neighbor(leaf, dir)` | Walk up to the first ancestor split whose orientation matches the direction AND where we can travel away; then descend through the other sub-tree picking the nearest-edge leaf. |
| `hx_split_install_frame_ui(f)` | Install the per-frame menu button (suffix on the header) + `frame-ops.*` action group. Called from `toolbar_install_panel_hooks_on_frame`. |

### Per-frame menu button (`hx_split_install_frame_ui`)

Each `PanelFrame` carries a small `GtkMenuButton` suffix on its
header (`panel_frame_header_add_suffix` at priority 0). The
button uses `view-split-symbolic` and pops three items:

- **Split horizontally** → `frame-ops.split-h`
- **Split vertically** → `frame-ops.split-v`
- **Close frame** → `frame-ops.close-frame`

The action group is installed at prefix `frame-ops` on the
frame widget itself via `gtk_widget_insert_action_group` — no
`PanelActionMuxer` indirection. Action enabled state:

- `split-h`, `split-v` — always TRUE.
- `close-frame` — TRUE iff the leaf has a parent split (i.e. it
  isn't the area root). Recomputed after every split so the
  newly-non-root original leaf flips from greyed to enabled.

Empty frames also show the button — the discoverability fix.

### Per-panel chevron menu

The libpanel chevron (the `pan-down-symbolic` button on each
`PanelFrame`'s header) is per-panel and shows:

- **Move Page Left / Right / Up / Down** — `page.move-left` etc.
  These are libpanel's built-in chevron items (in `PanelFrame`'s
  private `frame_menu` .ui template). libpanel ships them with
  class-action defaults that reflow pages across a `PanelGrid`,
  which we don't have — so they were always-greyed in the
  `HxSplit` world. We commandeer them via **`HxPanelFrame`**
  (`src/hx_panel_frame.{c,h}`), a `PanelFrame` subclass that
  installs its own same-named class actions. Activation runs
  `hx_panel_do_move_in_direction` on the visible `HxPanel`;
  per-direction enable state comes from `hx_panel_can_move_in_direction`
  (TRUE iff `hx_split_neighbor` would find a leaf in that
  direction). See *Reference: GtkActionMuxer + libpanel chevron
  routing* below for the full mechanism, the two attempts that
  *didn't* work, and the hook points that re-fire enabled state
  past libpanel's same-frame disables.
- **Undock** — `panel.undock`. Per-instance GAction routed via
  `PanelActionMuxer` at `page.panel.undock`.

Split + Close moved to the frame-level menu so they're available
on empty frames too. Move stays per-panel — it operates on a
specific panel and only makes sense when there's a panel to
move.

### `PanelDock` as a thin wrapper

The dock contains exactly one widget — the `HxSplit` root, added
as the center child via the buildable `add_child` vfunc with NULL
type. **No sidebars, no revealers, no per-area structure.** The
wrapper exists for one specific reason: libpanel's `PanelFrame`
template instantiates `PanelDropControls` as a private template
child, and `panel_drop_controls_root` (in
`panel-drop-controls.c`) asserts a `PANEL_TYPE_DOCK` ancestor at
root time. Without it every frame emits

```
libpanel-WARNING: PanelDropControls added without a dock, this cannot work.
```

We don't actually use libpanel's drop controls — we have our
own dock-level `GtkDropTarget` — but the warning fires
regardless. The wrapper silences it.

From the user's perspective the dock is still one recursive
HxSplit tree: splits / moves / closes operate on the tree, not
on dock areas.

## Implementation findings

These all cost a debugging session each. Listing them so the next
person doesn't have to rediscover.

### libpanel's chevron menu does not surface empty-frame actions

`PanelFrame`'s built-in `frame_menu` (Move Page L/R/U/D + Close
All + frame.close) is what the chevron pops on an empty frame.
Panel-scoped menus only get prepended via the `setup-menu`
signal when a page is selected. Putting "Split / Close frame"
on the panel chevron means they're invisible the moment the
user creates an empty frame — which is exactly when they need
them most. Move them to a frame-level affordance instead.

### `PanelActionMuxer` composes prefixes

`panel_widget_insert_action_group (self, "panel", group)` puts
the group inside `PanelActionMuxer` at the *internal* prefix
`panel.`. The muxer is then exposed by `PanelFrame` to GTK at
the `page` prefix via `gtk_widget_insert_action_group (frame,
"page", muxer)`. So a menu item referencing
`page.panel.undock` resolves; just `panel.undock` does not
(GTK looks for a top-level `panel` action group above the
popover, finds none, item renders disabled).

Switching the per-frame split/close actions to plain
`gtk_widget_insert_action_group` at prefix `frame-ops` avoided
the double-prefix entirely. Menu items use `frame-ops.split-h`
without any muxer routing.

### Action enabled state needs refreshing after splits

`PanelFrame`'s own `gtk_widget_action_set_enabled` calls happen
on selection changes etc. Ours don't — we have to call them
explicitly. When a split lands, the *previously* root leaf
becomes a non-root and its `close-frame` action should flip
from greyed to enabled. `frame_do_split` walks every leaf in
the area's tree (via `hx_split_foreach_leaf`) and runs
`update_frame_action_enabled` so the state matches the
topology.

### `panel_frame_remove` is synchronous

Calling `panel_frame_remove` followed immediately by
`panel_frame_add` to a different frame works — libpanel's
default `close-page` handler calls `adw_tab_view_close_page_finish`
synchronously, so the page is gone by the time `remove`
returns. The `n_pages > 0` guard in `hx_split_close_leaf` is
defensive, not async-driven; the move loop in `on_frame_close`
runs before the close attempt and the guard passes.

### `gtk_widget_unparent` cannot replace
"defang drop-controls"

`PanelDropControls` is a private template child of `PanelFrame`
(panel-frame.ui has it as an `<child type="overlay">`).
`PanelFrame`'s C code holds `priv->drop_controls` and
dereferences it from several places (`panel_drop_controls_set_area`
etc.). Unparenting it would dangle that pointer and crash
libpanel later. We're stuck with `can_target = FALSE` to make
the controls drop-event-transparent without removing them. The
thin-PanelDock-wrapper handles the related `PANEL_TYPE_DOCK`
ancestor warning at root time.

### `PanelDock` accepts any GtkWidget as its center child

`panel-dock.c::panel_dock_add_child` only special-cases
`PanelWidget` heading into a sidebar area (where it wraps in
`PanelPaned` + auto-creates a `PanelFrame`). Every other case
falls through to `panel_dock_child_set_child (dock_child,
widget)` which accepts any `GtkWidget`. So an `HxSplit` as the
center child works without any special handshake, and there's
no requirement to use `PanelGrid` for the center.

### Move-direction descent: deterministic vs UX-clever

When `hx_split_neighbor` needs to descend through a sub-tree to
pick the entry leaf, the cross-axis pick is arbitrary. For
Move LEFT entering a sub-tree from the right, the rightmost
descendant is well-defined for horizontal paneds (`child_b`)
but ambiguous for vertical paneds (both children are
horizontally equivalent). The current code picks `child_a`
deterministically. Future polish could pick whichever child is
vertically closest to the source's allocation, but that
requires runtime size info and probably isn't worth it.

## Reference: GtkActionMuxer + libpanel chevron routing

This section captures the full mental model of how a menu item's
click traverses widgets, muxers, and class-action stores to reach
a handler — and why the chevron's *Move Page L/R/U/D* items
needed a `PanelFrame` subclass to take over.

### The widgets between the menu item and the handler

```
GtkPopoverMenu (chevron pop-up, AdwTabBar per-tab button)
   │   action attribute "page.move-left"
   ▼
GtkMenuTrackerItem (observer; caches "enabled" + "sensitive")
   │   registers with action observable = the muxer it walked
   │   up the widget tree to find
   ▼
GtkActionMuxer (per-widget; lives on PanelFrame for this name)
   │   query_action / activate_action go through here
   ▼
PanelFrame's class-action store (priv->actions, a linked list)
```

Each `GtkWidget` carries an internal `GtkActionMuxer`; a child's
muxer chains to its parent's via `parent` pointer.
`action_muxer_query_action` (`gtk/gtkactionmuxer.c`) walks a muxer
with this priority:

1. Class actions installed on `muxer->widget` via
   `gtk_widget_class_install_action`. Match is `strcmp` head-first
   over `priv->actions`. **This is the first thing checked.**
2. Inserted action groups via `gtk_widget_insert_action_group`
   (split on `.` into prefix + name).
3. Parent muxer (recursive).

So class actions on the host widget beat inserted action groups
at the same prefix on the same widget. That's the key surprise
and the reason two earlier attempts failed.

### What didn't work and why

| Attempt | Where it broke |
|---------|----------------|
| `gtk_widget_insert_action_group(frame, "page", group_with_move_handlers)` at frame instance level. | libpanel's `panel_frame_update_actions` runs `gtk_widget_insert_action_group(self, "page", visible_child→muxer)` on every `notify::selected-page`. Our inserted group at `"page"` was clobbered on the first selection change. |
| `panel_widget_class_install_action(HxPanelClass, "move-left", ...)` so the action surfaces on the visible `HxPanel`'s `PanelActionMuxer` (which libpanel routes `page.*` through). | The muxer query for `page.move-left` reaches the *frame's* muxer first and finds libpanel's class action `page.move-left` there. The frame-level class-action lookup wins before the recursion ever reaches the inserted `"page"` group pointing at the panel's muxer. |

### What does work: HxPanelFrame as a PanelFrame subclass

`gtk_widget_class_install_action` writes into the calling class's
`priv->actions` list. The list is prepended-into, head-first, and
inherits from the parent: `gtk_widget_base_class_init` runs for
every subclass and copies the parent class's `GtkWidgetClassPrivate`,
which means `priv->actions` for `HxPanelFrameClass` starts at the
head of `PanelFrameClass`'s chain. When `hx_panel_frame_class_init`
installs `page.move-{left,right,up,down}`, those four actions go
in at the front of the same chain.

A class action chain dump (one of the diagnostic tools that
unblocked this work; `gtk_widget_class_query_action` walks the
chain and reports each action's owner) for `HxPanelFrameClass`:

```
[0] page.move-down  (owner=HxPanelFrame)
[1] page.move-up    (owner=HxPanelFrame)
[2] page.move-right (owner=HxPanelFrame)
[3] page.move-left  (owner=HxPanelFrame)
[4] frame.close-all          (owner=PanelFrame)
[5] frame.page               (owner=PanelFrame)
[6] frame.close              (owner=PanelFrame)
[7] frame.close-page-or-frame (owner=PanelFrame)
[8] page.move-up    (owner=PanelFrame)
[9] page.move-down  (owner=PanelFrame)
[10] page.move-left  (owner=PanelFrame)
[11] page.move-right (owner=PanelFrame)
```

For lookups by `strcmp`, the head-first walk lands on indices 0-3
for the four `page.move-*` names. Activation runs our handler;
state queries return our action's enabled bit.

### The disable-bit gotcha and the re-enable hook chain

`gtk_widget_action_set_enabled (widget, name, enabled)` walks the
same `priv->actions` list head-first, finds the first `strcmp`
match, and toggles a bit in the muxer's `widget_actions_disabled`
bitmask indexed by the action's `position` (distance from action
to NULL through `->next`). libpanel's `panel_frame_update_actions`
calls this on every selected-page / root / unroot with
`enabled=FALSE` (its no-PanelGrid path), and that call lands on
**our** action by name match — disabling our entry instead of the
inherited libpanel one. Net: even though our action is in front
for lookups, libpanel keeps switching its bit off.

`HxPanelFrame` re-enables the four actions at every hook point
where libpanel might have just disabled them:

| Hook | When it fires | Purpose |
|------|--------------|---------|
| `g_signal_connect_after(self, "notify::visible-child", ...)` | After libpanel emits `PROP_VISIBLE_CHILD` (which it does at the end of `panel_frame_notify_selected_page_cb`, AFTER `update_actions` runs). | Refresh on every selected-page change. |
| `widget_class->root` override | After parent root vfunc (`panel_frame_root` calls `update_actions`). | Refresh when the frame is added to the dock. |
| `widget_class->unroot` override | After parent unroot. | Refresh when removed (the next add re-runs root anyway, but matches libpanel's own bookkeeping). |
| `g_signal_connect_after(tab_view, "setup-menu", ...)` | After libpanel's `panel_frame_setup_menu_cb` rebuilds the joined menu, immediately before the popover is shown. | **The critical one.** |

`AdwTabView::setup-menu` is critical because `GtkMenuTrackerItem`
caches `sensitive` at observer-registration time by calling
`action_muxer_query_action` exactly once, and the observer
registers when the popover first opens. Without a refresh hook
co-timed with the popover, the cache picks up whatever state
libpanel left at the last `update_actions` and the items show
greyed forever — even though our re-enable signals on
`notify::visible-child` would update the cache, that requires the
observer to already exist. The user-visible symptom of having
gotten this wrong: items greyed at first chevron open, become
enabled after any subsequent action triggers
`action-enabled-changed` (i.e. a panel switch — the user moves
once and then it works).

### How HxPanelFrame's instance attaches the setup-menu hook

`PanelFrame`'s `AdwTabView` is a private template child a few
layers deep (`GtkOverlay → GtkBox → GtkStack → AdwTabView`), no
public accessor. `HxPanelFrame` overrides `GObject.constructed`
to walk descendants for `ADW_IS_TAB_VIEW` after the parent's
`constructed` has built the template tree, and connects the
hook there. If the walk ever fails (libpanel changes its
template), the code emits a `g_warning` and the chevron items
fall back to whatever state libpanel last set — visible failure
mode, not silent.

### Per-direction enable

`refresh_move_enabled` (called from all four hook points above)
calls `hx_panel_can_move_in_direction(panel, dir)` for each of
the four directions:

```c
gboolean
hx_panel_can_move_in_direction (HxPanel *self, GtkDirectionType dir)
{
    HxSplit *leaf = panel_get_split_leaf (self);
    return leaf != NULL && hx_split_neighbor (leaf, dir) != NULL;
}
```

So a corner panel has only the toward-center directions enabled;
an empty frame has all four disabled.

## Reference: GtkPaned size negotiation

The other deep-dive worth keeping. The trail of bugs around the
right-frame minimum width turned out to be one short function in
`gtk/gtkpaned.c`.

### What `shrink_X_child` actually does

`gtk_paned_compute_position`:

```c
min = paned->shrink_start_child ? 0 : start_child_req;

max = allocation;
if (!paned->shrink_end_child)
    max = MAX (1, max - end_child_req);
```

and `gtk_paned_get_preferred_size_for_orientation`:

```c
if (paned->end_child) {
    gtk_widget_measure (paned->end_child, ..., &child_min, &child_nat, ...);
    if (!paned->shrink_end_child)
        *minimum += child_min;
    *natural += child_nat;
}
```

Two consequences of `shrink_end_child = TRUE` are entangled:

1. **`max_position = allocation`** — the divider can be dragged
   or programmatically set all the way to the right, leaving the
   end child zero pixels.
2. **The end child contributes 0 to the paned's own reported
   minimum** — which propagates up to grand-parents (and
   ultimately the `GtkWindow`'s auto-computed minimum), so the
   window can be sized below the end child's requested width.

If you want halving below natural but a hard floor (e.g. via
`gtk_widget_set_size_request`), `shrink=TRUE` actively defeats
both. The size-request is irrelevant for user-drag and irrelevant
for the window's minimum.

### What `end_child_req` actually is

The first surprise (one we got wrong on the first pass): the
`end_child_req` in `compute_position` and the `child_min` in the
paned's measure are both `gtk_widget_measure(...HORIZONTAL...,
&minimum, ...)` — the **minimum**, which respects
`gtk_widget_set_size_request`, not the natural.

So with `shrink_end_child = FALSE` and `size-request = 300`:

- `end_child_req = max(internal_min, 300) = 300`
- `max_position = allocation - 300` — divider can be dragged or
  set anywhere up to `allocation - 300`, giving the end child as
  little as 300px.
- The paned's reported min includes `300` from the end child, so
  the window's minimum reflects it.

This is exactly the behavior we want: halving below natural still
works (the target is well above 300), the user can drag to 300,
and the window can't shrink past the sum-of-mins.

### `DEFAULT_LEAF_MIN_WIDTH 300`

`toolbar.c::MAKE_LEAF_FRAME` sets `gtk_widget_set_size_request(out,
DEFAULT_LEAF_MIN_WIDTH, -1)` on every default leaf. The value
propagates:

```
HxPanelFrame.size_request = 300
   ↓ (via gtk_widget_measure honoring size_request as floor)
HxSplit.measure (BinLayout delegates to PanelFrame.measure)
   ↓
GtkPaned.measure includes leaf min when shrink_X_child = FALSE
   ↓
parent GtkPaned.measure includes child paned's min
   ↓ ... up the dock tree ...
GtkWindow auto-computes min = root.min + chrome
```

`shrink_X_child` on every paned is `FALSE` (the
`hx_split_new_internal` default — see `src/hx_split.c`). 300 was
chosen to fit the widest of the default panels' button rows
(Users: 6 icon-buttons at ~44px + spacing + margins ≈ 280-290).
Bump the constant if a panel still clips at 300.

### `GTK_OVERFLOW_HIDDEN` on `HxPanelFrame`

`GtkWidget`'s default `overflow = VISIBLE` lets a child draw past
the parent's allocation. With `shrink_*_child = FALSE` on every
dock paned plus the per-leaf `DEFAULT_LEAF_MIN_WIDTH` size-request
floor, steady-state allocations stay at or above each panel's
minimum width — so a spill shouldn't happen in equilibrium. But
GTK can hand a frame a smaller slot than its children's natural
width during transients: paned animations, resize-during-allocation
passes, and split / merge / undock operations all reshape the tree
before the new size-requests have re-propagated. During those few
frames, content would render at natural width starting from the
frame's left edge and bleed onto neighbouring frames or off the
window. `HxPanelFrame` sets `GTK_OVERFLOW_HIDDEN` in instance init
so the visible content stays bounded to the slot regardless. Cheap
defensive measure, applies to every leaf in the main dock.

Popovers and tooltips create their own top-level surfaces in
GTK 4 so they aren't affected by parent overflow.

### Undocked windows use plain `PanelFrame`, not `HxPanelFrame`

`hx_panel_undocked_create_frame` (the `create-frame` handler for
the undocked window's `PanelGrid`) deliberately calls
`panel_frame_new` rather than `hx_panel_frame_new`. The
`HxPanelFrame` subclass exists to repurpose libpanel's
chevron `page.move-{left,right,up,down}` items for the main
dock's `HxSplit` tree — but undocked windows have no `HxSplit`
ancestor, so our override would disable those items entirely
(`hx_panel_can_move_in_direction` finds no leaf and returns
FALSE for every direction).

libpanel's default class-action handlers do the right thing in
the undocked context: they move pages between `PanelGrid` cells
and auto-create a new column when moving past the edge, which is
useful behaviour for a multi-panel undocked window. Keeping
plain `PanelFrame` there preserves it.

Frame-level hookups (`close-dispatcher`, `drag-out`,
`drop-controls defang`) still apply to undocked frames — they
operate via `gtk_widget_insert_action_group` and signals on the
specific frame instance, not via class-level overrides.

## Migration steps (chronological commits)

For history if anyone needs to rewind:

1. **`ec17b88` docking: default News + News 1.5 to the left sidebar.**
   Smallest possible win before the bigger work.
2. **`0bacfa0` HxSplit + design doc.** Just the widget — no
   integration yet.
3. **`b22962b` wrap each dock area in an HxSplit root.** Four
   independent trees, one per `PanelDock` area. Plumbing only.
4. **`b7d6dc0` split / close-frame / move-direction actions on
   panel chevron.** First user-visible payload.
5. **`8df0fe3` panel.undock action via
   panel_widget_insert_action_group.** Bug fix — `gtk_widget_
   insert_action_group` was the wrong muxer.
6. **`04e3740` spell action `page.panel.undock`.** Bug fix —
   prefix composition.
7. **`c10614d` move split/close-frame to per-frame menu.** Bug
   fix — empty frames had no split options + close-frame was
   greyed even on non-root frames.
8. **`648d9ba` dedupe Frame menu, refresh close-frame enabled,
   cross-area Move.** Three follow-on bugs.
9. **`eb7d72e` Phase 5b: drop PanelDock; whole dock = one
   HxSplit tree.** The big shift to a unified tree.
10. **`8463f2e` restore PanelDock as thin wrapper.** Bug fix —
    libpanel's PanelDropControls warning + DnD redock breakage.

## What's still open

- **DnD hit-testing for splits** (task #40). Should work
  through `HxSplit` transparently — the dock-level
  `GtkDropTarget` still hits the deepest descendant
  `PanelFrame`. Worth verifying after several user splits.
- **Layout persistence (Phase 4b)** (task #41). Now much
  simpler than the previous four-tree model. Main dock + undocked
  windows shipped in `src/dock_layout.{c,h}` — see *Layout
  persistence* below for the file format and the
  `[Undocked]`-section round-trip.

## Layout persistence

`src/dock_layout.{c,h}`. File: `$gtkhx_config_dir/dock-layout.ini`,
GKeyFile format.

```ini
[Dock]
# Recursive s-expression:
#   h(A,B)              horizontal split
#   v(A,B)              vertical split
#   L[id1,id2,...]      leaf with panel IDs in tab order
#   L[id1,id2,...:role] leaf tagged with a role; the four roles
#                       (start, end, bottom, center) anchor the
#                       toolbar_*_frame globals
tree=h(L[news:start],h(v(L[chat,files,news15:center],L[tasks:bottom]),L[users:end]))

# Paned divider positions in depth-first order; same count as
# internal splits in the tree.
sizes=240;620;380
```

Window size lives in `gtkhxrc` (the existing `gtkhx_prefs.geo.tool`
mechanism) — `dock-layout.ini` stays focused on the dock tree.

### Save trigger

`dock_layout_request_save()` is debounced on a 200 ms idle so a
burst of dock changes (a paned drag, a rapid sequence of splits)
collapses to a single write 200 ms after the last request. Wired
into the entry points that mutate shape or placement:

- `hx_split_new_internal` connects `notify::position` on every
  paned — covers user-drag of the divider.
- `hx_split_split` and `hx_split_close_leaf` call request-save
  directly — covers tree-shape changes.
- The panel-migration sites in `hx_panel.c` (`on_frame_drop`,
  `hx_panel_undock`, `on_undocked_close_request`,
  `hx_panel_do_move_in_direction`) call request-save explicitly.
  `PanelFrame` doesn't expose `page-added` / `page-removed` —
  only `page-closed` and `adopt-widget` — and `AdwTabView`'s
  page-attached/-detached is a private template child, so the
  per-site calls are the simplest way to cover panel movement.
- `hx_panel_undock` also connects `notify::default-width` and
  `notify::default-height` on the new top-level so user-resize
  of the undocked window persists.
- `hx_quit` calls `dock_layout_shutdown` which flushes any pending
  save synchronously before exit.

`dock_layout_reset` (hamburger Reset Layout) sets a session-scoped
`save_disabled` flag that `request_save` checks before scheduling.
Without that flag, a notify::position from a divider drag (or any
of the other triggers above) could re-create the file we just
deleted between Reset and next launch.

### Load policy

`dock_layout_load` runs from `create_toolbar_window` before the
default-build path. If the file is missing, malformed, or doesn't
have all four role-tagged leaves, it returns FALSE and the
default-layout code runs as before — defaults are always the
recovery path. The four `toolbar_*_frame` globals get set from
the role tags (`start` → `toolbar_sidebar_frame`, etc.).

After construction, `dock_layout_apply_geometry` pushes the saved
paned positions onto the now-mounted paneds.

### Panel placement

Static-panel factories still call `panel_frame_add(toolbar_*_frame,
panel)` — they don't know about saved layouts. After the factory
runs, `hx_panel_registry_register` calls `dock_layout_place_panel`
which looks up the panel's id in the saved id→frame map. If the
saved frame differs from where the factory just placed the panel,
the panel is reseated. The window is invisible at this point so
the user doesn't see the brief flash.

### Reset

`app.reset_layout` (hamburger menu *Reset Layout*) calls
`dock_layout_reset` to wipe the saved file. Doesn't tear down the
live dock — that'd require destroying and re-creating every panel
— but the next launch comes up with the default layout. A toast
tells the user the action only takes effect on next launch.

### Undocked window persistence

Each panel whose root is not the main toolbar window gets a key
in an `[Undocked]` section:

```
[Undocked]
tracker=600,400
```

One key per panel id, value `W,H`. Save walks
`hx_panel_registry_foreach` and emits a key for every panel whose
root differs from `toolbar_window`. Load parses the section into a
one-shot pending-undock map (`dock.id_to_undock_size`).

When each panel's factory runs at startup,
`hx_panel_registry_register` fires `dock_layout_place_panel`. Its
two-phase flow:

1. Reseat into the saved leaf (main-dock placement).
2. If the panel's id is in the pending-undock map, consume the
   entry, call `hx_panel_undock`, and apply the saved size to the
   resulting top-level via `gtk_window_set_default_size`.

The map entry is removed after consumption so a later
register-after-redock-then-close doesn't re-undock.

Wayland gives clients no portable way to set absolute window
position, so we don't try — only size persists.

### Out of scope (for now)

- Pre-shipped layouts and per-user named layout presets.

## What didn't change

- libpanel's `PanelFrame` still owns its own tabs / drag handle
  / chevron menu. We're not touching per-frame UI internals.
- The `GtkhxSession` signal boundary, `rcv.c` / `commands.c`
  protocol stack, and HOPE negotiation are untouched. Hotline
  1.2 / 1.5 / 1.9 wire compatibility was a hard requirement
  throughout.
- Undocked windows still use their own `PanelDock` +
  `PanelGrid` internally (the same shape `hx_panel_undock` has
  built since Phase 4a).
