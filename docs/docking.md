# Docking UI

The authoritative reference for GtkHx's dockable window layer — why it's
shaped the way it is, how the recursive split tree works, the libpanel
quirks worth remembering, and how a layout survives a restart.

## What we built

GtkHx used to be a constellation of independent top-level windows — one
each for the toolbar, public chat, news, users, tasks, file browser,
plus dynamic windows for private chats and private messages. Closing the
toolbar quit the app, but everything else was loose and Wayland refused
to position any of them at startup.

The new model is a single dockable window. The toolbar window hosts a
[`libpanel`](https://gitlab.gnome.org/GNOME/libpanel) `PanelDock` whose
one child is a recursive split tree; each of the static tool windows
became an `HxPanel` inside that tree. Private chats and private messages
don't get panels of their own — they became internal tabs inside the
Chat panel's own `AdwTabView`. The result:

- One main window. Saved size restores on startup; the dock tree and
  paned positions restore from `dock-layout.ini`.
- The user can split any frame, DnD panels between frames, drag them out
  into their own windows, drag them back in, undock / redock from a
  chevron menu.
- The Chat panel internally tab-strips through public chat + every
  active conversation, so the dock strip stays focused on structural
  windows.

The protocol layer (`rcv.c`, `commands.c`, HOPE negotiation) is
untouched — this is a view-layer rewrite. Hotline 1.2 / 1.5 / 1.9 wire
compatibility was a hard requirement throughout.

## Why libpanel

GTK 4 ships no docking primitive. Options were:

1. **Roll our own** — `AdwTabView` + `GtkPaned` + manual widget
   reparenting + drag-and-drop + layout serialization. A large amount
   of fiddly, bug-prone work for a result that's worse than the
   alternative.
2. **Use libpanel** — GNOME Builder's docking library. Provides almost
   exactly the primitives we need.

We use libpanel. The primitives line up:

| libpanel type        | Role here                                                        |
|----------------------|------------------------------------------------------------------|
| `PanelDock`          | Thin wrapper around the split tree; the toolbar window's content. |
| `PanelFrame`         | A tabbed group of panels. Every split-tree leaf holds one.        |
| `PanelGrid`          | Undocked windows only — lazily creates `PanelFrame`s on add.      |
| `PanelWidget`        | One dockable panel (title, icon, content).                        |
| `panel_widget_close` | Standard close path; we hook `PanelFrame::page-closed`.           |

Availability was checked before committing to the dependency: Debian
trixie ships libpanel 1.10.0, and the `org.gnome.Platform/49` Flatpak
runtime ships libpanel-1, so the dependency is cheap on both the distro
and Flathub paths. `meson.build` pins the floor at `libpanel-1 >= 1.4`:
`create-frame` on `PanelDock` / `PanelGrid` landed in 1.2 and
`panel_workbench_foreach_workspace` in 1.4, so anything older predates
the API surface the docking work leans on.

## What docks and what doesn't

**Docks** — persistent tool windows that became `HxPanel`s in the
toolbar dock. The content widget tree and handlers for each of these now
live in the Rust `gtkhx-ui` crate and are embedded through the C
`dock_bridge` (see *The `dock_bridge` contract*); their model halves
stay in C.

| Panel        | Content shell                             | Panel id | Kind    | Home area |
|--------------|-------------------------------------------|----------|---------|-----------|
| Chat         | `gtkhx-ui/src/chat.rs`                    | `chat`   | CENTER  | CENTER    |
| Files        | `gtkhx-ui/src/files.rs`                   | `files`  | CENTER  | CENTER    |
| News 1.5     | `gtkhx-ui/src/news_browser.rs`            | `news15` | CENTER  | CENTER    |
| News 1.0     | `gtkhx-ui/src/news.rs`                    | `news`   | SIDEBAR | START     |
| Users        | `gtkhx-ui/src/users.rs`                   | `users`  | SIDEBAR | END       |
| Tasks        | `gtkhx-ui/src/tasks.rs`                   | `tasks`  | SIDEBAR | BOTTOM    |

The ids are defined once in `src/panel_registry.h` so collisions show up
at compile time.

**Becomes a tab inside the Chat panel:**

- Private chats (`sess->gchats`, keyed by cid)
- Private messages (`sess->msg_windows`, keyed by uid)

The Chat panel hosts an `AdwTabView` whose first tab is the public
chat (pinned), with one closeable tab per active conversation. New
activity flags `needs-attention` on the tab and on the Chat dock
panel; selecting any tab clears the indicator. The C ABI for that tab
strip is `src/chat_tabs.h`; the implementation is Rust
(`rust/crates/gtkhx-ui/src/chat_tabs.rs`) — there is no `chat_tabs.c`.

**Stays a real window** (transient, modal, one-shot, pre-connection,
or fundamentally not server-content):

- Agreement, About, user editor, post-news composer, file preview.
- **Tracker** — server-discovery, not server-content. Exists *before*
  a connection (it's how the user picks one). It is a plain
  `gtk::Window` built in `rust/crates/gtkhx-ui/src/tracker/`; the panel
  registry doesn't absorb it, and there is deliberately no
  `HX_PANEL_ID_TRACKER`.

The toolbar's Files / Users / Chat / Tasks buttons route through
`toolbar_show_panel`, which does registry-lookup + re-attach + raise.
News (1.0) and News (1.5+) keep their own entry points because they also
need to fire a server fetch when connected.

## The `HxSplit` recursive split tree

The dock is **one recursive `HxSplit` tree**. Each tree node is
either:

- a **leaf**: a single `PanelFrame`, or
- an **internal split**: a `GtkPaned` (chosen orientation) whose
  two children are themselves `HxSplit` nodes.

There is no separate three-sidebar / center-grid structure — earlier
revisions of the dock had one, and it is gone. Splits, moves and closes
all operate over a single uniform tree; what were previously "area
boundaries" (start / end / bottom / center) survive only as four role
tags used to anchor the `toolbar_*_frame` globals.

Users can:

1. **Split any leaf** horizontally or vertically. The original
   frame becomes the start child; a fresh empty leaf becomes the
   end child.
2. **Move panels between leaves** via DnD (libpanel's existing
   gesture, unchanged) or via *Move left / right / up / down* in
   the panel chevron menu. With one unified tree, Move walks the
   whole dock.
3. **Close a frame** (a leaf). Any panels in the closing leaf
   first migrate to the sibling, then the leaf collapses; the
   sibling takes the parent split's place in the tree.
4. **Empty leaves stay visible** until explicitly closed. The
   discoverability win — "I just split this, now what?" — is what
   drove the design.
5. **Undock + Redock.** The panel chevron menu has *Undock*;
   close-request on the undocked window walks the panel back to its
   home frame.

### Default layout

The first-launch tree, built in `toolbar.c::create_toolbar_window`:

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
place and manufactures a sibling. The dock-embed bridge uses these
pointers as its `panel_frame_add` target, so panel placement doesn't
have to know anything about the tree.

The Users panel's natural width makes the right leaf start out wider
than it needs to be, so the default-layout path halves its share on
first allocation via a one-shot `notify::max-position` handler. That's
only attached for the default layout; saved layouts come back with
explicit paned positions instead.

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
| `hx_split_new_internal(a,b,o)` | Construct an internal split directly from two unparented children. Used by the default-layout builder in `toolbar.c` and by the saved-layout builder in `dock_layout.c`. |
| `hx_split_split(leaf, o)`      | Convert a leaf into an internal split. Returns the new sibling leaf. |
| `hx_split_close_leaf(leaf)`    | Collapse the leaf; the sibling takes the parent's place. Refuses if the leaf still has pages — caller migrates them first. |
| `hx_split_neighbor(leaf, dir)` | Walk up to the first ancestor split whose orientation matches the direction AND where we can travel away; then descend through the other sub-tree picking the nearest-edge leaf. |
| `hx_split_find_for_frame(root, f)` | Find the leaf holding a given `PanelFrame`. |
| `hx_split_foreach_leaf(root, …)`   | Iterate every leaf, left-to-right / top-to-bottom. Callback must not mutate the tree. |
| `hx_split_install_frame_ui(f)` | Install the per-frame menu button (suffix on the header) + `frame-ops.*` action group. |

### Per-frame menu button

Each `PanelFrame` carries a small `GtkMenuButton` suffix on its
header (`panel_frame_header_add_suffix`). The button uses
`view-split-symbolic` and pops three items:

- **Split horizontally** → `frame-ops.split-h`
- **Split vertically** → `frame-ops.split-v`
- **Close frame** → `frame-ops.close-frame`

The action group is installed at prefix `frame-ops` on the
frame widget itself via `gtk_widget_insert_action_group` — no
`PanelActionMuxer` indirection. Action enabled state:

- `split-h`, `split-v` — always TRUE.
- `close-frame` — TRUE iff the leaf has a parent split (i.e. it
  isn't the tree root). Recomputed after every split and after every
  close so the newly-non-root original leaf flips from greyed to
  enabled (and back, when a collapse promotes a leaf to root).

Empty frames also show the button — the discoverability fix.

### `frame-ops.close-page` and the header's X

The group carries a fourth action that isn't in that menu:
`close-page`, which backs the close (X) button libpanel puts in the
header's controls box.

That button ships wired to `frame.close-page-or-frame`, and both its
handler and the enabled state `panel_frame_update_actions` computes
for it open with `gtk_widget_get_ancestor (frame, PANEL_TYPE_GRID)`.
There is no `PanelGrid` in the main dock, so the X was **dead for the
entire life of the `HxSplit` dock** — closing a single panel meant
*Close all pages* on the whole frame and then reopening the ones you
wanted.

The `HxPanelFrame` trick that rescued the chevron's *Move Page* items
does not work here, and the reason is the useful part. Overriding the
class action from the subclass wins the lookup, but libpanel goes on
disabling our action by name — and `panel_frame_add` and
`panel_frame_remove` each call `panel_frame_update_actions` on the way
*out*, after every notify a refresh hook could ride on. A menu item
survives that: `AdwTabView::setup-menu` fires just before the popover
reads state, so it can be fixed just in time. A button has no such
moment — it reflects the enabled bit continuously, and libpanel always
gets the last word. This was tried first and lost exactly there.

So the fix doesn't contest the action; it retargets the button.
`adopt_frame_close_button` finds it (controls box by CSS class, then
the `window-close-symbolic` `GtkButton` inside — scoped that way
because the pages popover's per-row close buttons share the icon
name) and points its `action-name` at `frame-ops.close-page`. That
group is an *inserted* one, invisible to `panel_frame_update_actions`,
and the enabled state lives in a `GSimpleAction` we own outright,
driven off `notify::visible-child`. The button keeps its icon,
placement and circular styling; libpanel never touches `action-name`
after the template is built, and `close_button` isn't even bound as a
template child, so nothing in libpanel can reach it.

The "or frame" half is deliberately not reimplemented — closing an
empty frame is *Close frame* on the view-split menu — so the X simply
greys out when the frame has no page.

`tests/unit/test_frame_close_button.c` pins both halves: that the
button is found and adopted (a libpanel header restructure breaks the
search silently otherwise), and that its enabled state survives
repeated add/remove churn, which is precisely where the class-action
attempt failed.

### Per-panel chevron menu

The libpanel chevron (the `pan-down-symbolic` button on each
`PanelFrame`'s header) is per-panel and shows:

- **Move Page Left / Right / Up / Down** — `page.move-left` etc.
  These are libpanel's built-in chevron items (in `PanelFrame`'s
  private `frame_menu` .ui template). libpanel ships them with
  class-action defaults that reflow pages across a `PanelGrid`,
  which the main dock doesn't have — so they were always-greyed in the
  `HxSplit` world. We commandeer them via **`HxPanelFrame`**
  (`src/hx_panel_frame.{c,h}`), a `PanelFrame` subclass that
  installs its own same-named class actions. Activation runs
  `hx_panel_do_move_in_direction` on the visible `HxPanel`;
  per-direction enable state comes from `hx_panel_can_move_in_direction`
  (TRUE iff `hx_split_neighbor` would find a leaf in that
  direction). See *Appendix A* for the full mechanism, the two attempts
  that *didn't* work, and the hook points that re-fire enabled state
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

## `HxPanel` and the panel registry

### `HxPanel` (`src/hx_panel.{c,h}`)

`G_DECLARE_FINAL_TYPE` subclass of `PanelWidget`. Carries:

- A stable string id (`"chat"`, `"news"`, `"users"`, …) so the
  registry can index it and layout persistence can refer to
  panels by name, not pointer.
- A kind tag (`CENTER` / `SIDEBAR` / `DYNAMIC`) so the registry knows
  which area to home the panel to.
- A `home_area` and a `home_frame` `GWeakRef`. The area is consulted
  by `hx_panel_ensure_attached` when re-attaching after a close; the
  frame is consulted by `on_undocked_close_request` when the user
  closes the undocked window.
- An optional per-panel close callback, used by DYNAMIC panels to tear
  down their backing model state.

`hx_panel_ensure_attached` prefers the recorded `home_frame` whenever
its weak ref still resolves to a live frame — which is the common case
once the user has moved the panel into a custom split leaf. That
deliberately preserves the user's placement across a *Close all pages* →
re-show round trip. Only when `home_frame` has expired does it fall back
to the `home_area` default (the matching `toolbar_*_frame`; since there
is no `PanelGrid` in the main dock, even CENTER resolves to a
`PanelFrame`), and the stored `home_frame` is updated to that fallback.

The chevron menu's *Undock* GAction lives on `HxPanel` via libpanel's
`PanelActionMuxer`. `hx_panel_undock` is the public entry point — both
the menu action and the drag-out detector call it.

### Panel registry (`src/panel_registry.{c,h}`)

`GHashTable<id → HxPanel>`. Owns the only post-construction strong ref
to each registered panel — this is what keeps a panel alive across
*Close all pages*, so the toolbar button can re-attach it later.

Lifetime: lazily created on first call. Static panels stay registered
for the process lifetime; dynamic panels unregister explicitly when
their backing model object goes away. `HxPanel::finalize` does not
touch the registry (it would create a chicken-and-egg with the
registry's strong ref). Main thread only — workers marshal via
`g_idle_add` like every other GTK call.

## Undock and drag-and-drop

Two flows, both via libpanel's existing drag handle button (the
six-dot icon in the frame header):

1. **Drag-out** (panel → undocked window). We connect to the
   `GtkDragSource::drag-cancel` signal on libpanel's drag button. On
   `GDK_DRAG_CANCEL_NO_TARGET` or `GDK_DRAG_CANCEL_ERROR` we read the
   dragged panel from `gdk_drag_get_content` and call
   `hx_panel_undock`.

2. **Drop on frame** (in-dock move OR cross-dock redock). A dock-level
   `GtkDropTarget` on `toolbar_dock` accepts `PANEL_TYPE_WIDGET`. On
   drop we hit-test the drop coordinates against descendant
   `PanelFrame`s (walking through `HxSplit` nodes transparently), pick
   the deepest match, and move the panel via `panel_frame_remove` /
   `panel_frame_add`. For cross-dock drops, if the source undocked
   window becomes empty we destroy it (after disconnecting its
   close-request handler so the redock path doesn't race the
   already-moved panel).

### Drop feedback is per-pane

GTK's default stylesheet outlines *any* widget with an active drop
target under the pointer (`:not(window):drop(active)`). Our target is a
single one on the dock, and the dock fills the window — so the whole
window lit up and told the user nothing about which pane the panel
would land in.

Two halves to the fix, both in `hx_panel.c`:

- The dock carries an `hx-dock-drop-host` class and a rule that turns
  its own `:drop(active)` box-shadow off.
- `enter` and `motion` run `frame_at_dock_coords` — the *same*
  hit-test the drop uses, so the highlight can't disagree with where
  the panel actually goes — and put an `hx-drop-target` class on that
  frame. `leave` and `drop` clear it.

The frame's highlight is `outline`, not `box-shadow: inset`, and the
distinction is load-bearing: `gtk_widget_snapshot` paints background
and border *before* the children and the outline *after*, so an inset
shadow would be covered by whatever content fills the pane. A negative
`outline-offset` keeps it inside the frame's own allocation instead of
bleeding onto the neighbour.

The tracking pointer is a `GWeakRef`-style weak pointer
(`g_set_weak_pointer`): the highlighted frame can be destroyed
mid-drag — a cross-dock drag that empties an undocked window does
exactly that — and a raw pointer would dangle until the next motion
event cleared it.

Colours come from `@accent_bg_color` rather than
`var(--accent-bg-color)`: the named colour works across the whole
supported libadwaita range, and CSS custom properties don't reach back
to our floor.

`hx_panel_undock` builds a fresh `AdwApplicationWindow` containing a
`PanelDock` + `PanelGrid` from an inline builder string, and moves the
panel into the grid. It is context-aware: when invoked on a panel that's
already in an undocked window it closes the undocked window instead,
which triggers `on_undocked_close_request` to redock the panel to its
home frame. The "already undocked?" test is simply "is this panel's
`GtkRoot` the toolbar window?".

Nothing reaches that branch from the UI any more — it existed for the
*Undock* item on an undocked window's own chevron, and that chevron is
now hidden (see below). The branch stays because the entry point is
public and the guard is one comparison; losing it would mean a second
undocked window nested out of the first.

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

libpanel's default class-action handlers are at least *coherent*
in the undocked context — they move pages between `PanelGrid`
cells — where ours would be dead. That argument has weakened
since: the chevron that surfaced those items is now hidden (see
below), so nothing reaches them either way. What's left is the
narrower reason, which still holds: `HxPanelFrame` exists to
serve the `HxSplit` tree, an undocked window has no `HxSplit`
ancestor, and subclassing there would buy nothing but a class
whose four actions can never be enabled.

Frame-level hookups (`close-dispatcher`, `drag-out`,
`drop-controls defang`) still apply to undocked frames — they
operate via `gtk_widget_insert_action_group` and signals on the
specific frame instance, not via class-level overrides.

### An undocked window is a leaf

Nothing docks *into* an undocked window. The dock-level
`GtkDropTarget` lives on `toolbar_dock` alone, so a drop over an
undocked window has never been accepted — but libpanel's header went
on advertising the machinery for it, which reads as "this should
work" and then doesn't.

So `hx_panel_undocked_create_frame` hides the header's `.controls`
box, which is the `pan-down-symbolic` chevron and the
`window-close-symbolic` X together (`panel-frame-header-bar.ui` puts
both in that one box). Neither means anything in a one-panel window:
the chevron's *Move Page L/R/U/D* has nowhere to move to, and the X
closes the *page*, leaving an empty window behind rather than
redocking.

The box is a private template child with no accessor, so we find it
by CSS class — same hunt-by-property approach as the drag handle, and
the same caveat about libpanel restructuring its header. A miss is a
`g_warning`, not a silent skip.

The drag handle and the title button stay, so both ways back into the
dock are unchanged: drag the panel over a pane, or close the window
and let `on_undocked_close_request` redock it to its home frame.

## Layout persistence

`src/dock_layout.{c,h}` plus the GTK-free parser in
`src/dock_layout_parse.{c,h}` (split out so the grammar can be
unit-tested without dragging in GTK and libpanel). File:
`$gtkhx_config_dir/dock-layout.ini`, GKeyFile format.

```ini
[Dock]
# Recursive s-expression:
#   h(A,B)              horizontal split
#   v(A,B)              vertical split
#   L[id1,id2,...]      leaf with panel IDs in tab order
#   L[id1,*id2,...]     '*' marks the leaf's foreground page
#   L[id1,id2,...:role] leaf tagged with a role; the four roles
#                       (start, end, bottom, center) anchor the
#                       toolbar_*_frame globals
tree=h(L[news:start],h(v(L[*chat,files,news15:center],L[tasks:bottom]),L[users:end]))

# Paned divider positions in depth-first order; same count as
# internal splits in the tree.
sizes=240;620;380

# Static panels that were in no dock at all at save time.
closed=files;news15
```

Whitespace between tokens is tolerated so the file can be hand-edited.
Panel ids are anything that isn't a separator character (`,` `]` `:` or
whitespace) — no quoting needed for the ids we actually have.

### The foreground-page marker

A frame shows one page at a time, and which one is the user's choice, so
it is saved: `*` in front of an id means that page was the frame's
visible child. At most one per leaf — two is a file that can't be acted
on rather than a tie to break, so the parser rejects it and the loader
falls back to defaults.

Restoring it has to be the **last** thing startup does, and that's the
whole subtlety. `panel_frame_add` selects the page it adds, and `fe_init`
raises Chat, News, Users and Tasks in a fixed order after the dock is
built — so whichever of those touched a frame last won it. A leaf holding
both Chat and Tasks came up on Tasks however the user had left it.
`dock_layout_apply_selection`, called at the end of `fe_init` once every
panel exists and is placed, is what has the final say. It's one-shot: the
saved selection describes launch, and every tab change after that belongs
to the user.

The list is keyed by panel id, not by frame — a panel id belongs to
exactly one leaf, so the flat list is unambiguous, and it survives the
reseat `dock_layout_place_panel` may do afterwards, which a frame-keyed
record would not.

### Closed panels stay closed

`closed=` is the set of static panels (`hx_panel_static_ids`) that were in
no dock at save time — neither in the main dock nor in an undocked window.
On the next launch those panels' factories are not called at all;
`toolbar_build_panel` checks `dock_layout_panel_was_closed` first. The
toolbar button builds the panel on demand, passing
`respect_saved_state=FALSE`, because an explicit user request outranks
what was on disk.

Two deliberate choices:

- **The closed set is serialised, not the open set.** A panel id this
  version has never heard of therefore defaults to open, and an absent
  key — every layout file written before this existed — reads as "nothing
  was closed", which is what those files meant.
- **It is recomputed from live state at every save**, never tracked
  incrementally. Reopening a panel makes it open again with no bookkeeping,
  and there is no second source of truth to drift.

Building a panel late is safe because the model-side state each one
renders is created elsewhere and outlives it: `create_chat` and
`create_tasks` run per session in `gtkhx.c`, so chat output accumulates
into a parentless chat view and the whole scrollback is there when the
panel finally packs it; `gtkhx_users_bridge_after_embed` replays the user
list from the member model; `gtkhx_tasks_after_embed` replays the task and
transfer lists; Files and News fetch fresh on open. A panel built on the
first toolbar click comes up populated, not empty.

Window size lives in `gtkhxrc` (the existing `gtkhx_prefs.geo.tool`
mechanism) — `dock-layout.ini` stays focused on the dock tree.

### Save trigger

`dock_layout_request_save()` is debounced on a 200 ms timer so a
burst of dock changes (a paned drag, a rapid sequence of splits)
collapses to a single write 200 ms after the last request. It's a
debounce, not a throttle: every request resets the timer. Wired
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
- `on_frame_page_closed` in `hx_panel.c` requests a save for every
  panel kind, not just DYNAMIC — closing a panel changes the
  `closed=` set. `hx_panel_ensure_attached` requests one on the
  mirror path, when a panel is spliced back in.
- `HxPanelFrame`'s `notify::visible-child` handler requests a save —
  a tab switch changes the `*` marker. Startup produces a burst of
  these as panels are added one at a time; the debounce absorbs them
  and the write that lands is the settled state.
- `hx_quit` calls `dock_layout_shutdown`, which flushes any pending
  save synchronously before exit.

### Load policy

`dock_layout_load` runs from `create_toolbar_window` before the
default-build path. A missing file, a missing `tree` key, or a malformed
expression all return FALSE and the default-layout code runs as before —
defaults are always the recovery path.

Missing role tags are *not* a fallback trigger. The four
`toolbar_*_frame` globals must be non-NULL because they're the
`panel_frame_add` target for every panel factory, but a saved tree can
legitimately lack some tags: closing a default leaf reseats the matching
global onto the surviving sibling, so after enough closes several
globals point at the same leaf and only the topmost role gets
serialised. Rebuilding defaults in that case would erase the user's
arrangement (the "I undocked everything and now the main dock is one
empty leaf" case). Instead, any missing role is pointed at the first
leaf in the tree. Only a tree with no leaves at all falls back.

After construction, `dock_layout_apply_geometry` pushes the saved
paned positions onto the now-mounted paneds. It has to happen after
mounting: a fresh `GtkPaned` reports `max-position` 0 and
`gtk_paned_set_position` clamps to it, so each position is applied from
a one-shot `notify::max-position` handler rather than directly.

### Panel placement

Panel factories place their panel in the `toolbar_*_frame` for its
area — they don't know about saved layouts. After that,
`hx_panel_registry_register` calls `dock_layout_place_panel`, which
looks up the panel's id in the saved id→frame map. If the saved frame
differs from where the factory just placed the panel, the panel is
reseated (and its `home_frame` updated). The window is invisible at this
point so the user doesn't see the brief flash.

### Undocked window persistence

Each panel whose root is not the main toolbar window gets a key
in an `[Undocked]` section:

```ini
[Undocked]
users=600,400
```

One key per panel id, value `W,H`. Save walks
`hx_panel_registry_foreach` and emits a key for every panel whose
root differs from `toolbar_window`. GKeyFile drops empty groups, so the
section simply doesn't appear when nothing is undocked. Load parses the
section into a one-shot pending-undock map.

`dock_layout_place_panel`'s flow is therefore two-phase:

1. Reseat into the saved leaf (main-dock placement).
2. If the panel's id is in the pending-undock map, consume the
   entry, call `hx_panel_undock`, and apply the saved size to the
   resulting top-level via `gtk_window_set_default_size`.

The map entry is removed after consumption so a later
register-after-redock-then-close doesn't re-undock.

Wayland gives clients no portable way to set absolute window
position, so we don't try — only size persists.

### Reset

`app.reset_layout` (hamburger menu *Reset Layout*) calls
`dock_layout_reset`, which unlinks the saved file, clears the in-memory
maps, and cancels any pending save. It doesn't tear down the live dock —
that'd require destroying and re-creating every panel — but the next
launch comes up with the default layout, and a toast tells the user so.

Reset also sets a session-scoped `save_disabled` flag that
`request_save` checks before scheduling. Without it, a
`notify::position` from a divider drag (or any of the other triggers
above) would re-create the file we just deleted, and next launch would
restore whatever was in flight at the moment of reset rather than the
defaults.

## Gotchas

Each of these cost a debugging session. They're documented inline at the
relevant call sites; this is the recap.

### `GtkDragSource:actions` defaults to 0

libpanel's drag handle declares no actions in its `.ui`, so the drag's
action set ends up empty. With actions=0 no drop target can succeed in
action negotiation — every drop target's enter silently doesn't fire,
every drop ends in `drag-cancel`. We explicitly call
`gtk_drag_source_set_actions (MOVE | COPY)` on libpanel's drag source
after we find it.

### The drag content isn't on the source

libpanel sets the drag content via the `GtkDragSource::prepare` signal's
return value, not via `gtk_drag_source_set_content`. So
`gtk_drag_source_get_content` returns NULL. The actual content lives on
the `GdkDrag` — `gdk_drag_get_content (drag)` works.

### `PanelDropControls` swallows drops

It's an invisible overlay child of each `PanelFrame`. During drag it
becomes visible (per libpanel's normal flow) and consumes drop events
without firing a usable accept. Setting `can-target = FALSE` on every
`PanelDropControls` (found by walking the frame tree and matching
`G_OBJECT_TYPE_NAME == "PanelDropControls"`) makes it transparent so the
dock-level target sees the drop.

`gtk_widget_unparent` is **not** an alternative. `PanelDropControls` is
a private template child of `PanelFrame` (`panel-frame.ui` has it as a
`<child type="overlay">`), `PanelFrame`'s C code holds
`priv->drop_controls` and dereferences it from several places
(`panel_drop_controls_set_area` etc.), and unparenting it would dangle
that pointer and crash libpanel later. Defanging is the only option; the
thin-`PanelDock`-wrapper handles the related `PANEL_TYPE_DOCK` ancestor
warning at root time.

### No public drag-handle accessor

`PanelFrameHeaderBar`'s drag button is private. We find it by traversing
the frame tree looking for a `GtkButton` with
`icon-name == "list-drag-handle-symbolic"`, then look up the
`GtkDragSource` controller installed on it. Stable surface today; worth
flagging if a future libpanel restructures the header.

### `G_TYPE_INVALID` + `gtk_drop_target_set_gtypes`

Single-type `GtkDropTarget` constructors don't always match. Going
broad — `{ PANEL_TYPE_WIDGET, GTK_TYPE_WIDGET, G_TYPE_OBJECT }` — makes
the `GdkContentFormats` intersection succeed reliably. Preload TRUE so
enter / motion see the value.

### Don't `g_object_unref` after `hx_panel_registry_register`

The intuitive read is "the frame holds the widget-tree ref; the
registry's `g_object_ref` is the extra one; my factory's
post-construction ref is the third — drop it." That's wrong. Refcount
walk:

| Step                                                            | Refcount |
|-----------------------------------------------------------------|----------|
| `hx_panel_new`                                                  | 1 (floating) |
| `panel_frame_add` → `AdwTabPage` "child" set:                   |              |
| &nbsp;&nbsp;`g_set_object (&page->child, panel)`                | 2 (floating) |
| &nbsp;&nbsp;`adw_bin_set_child` → `gtk_widget_set_parent` → `g_object_ref_sink` | 2 (floating cleared, **no new ref**) |
| `hx_panel_registry_register` — internal `g_object_ref`          | 3            |
| `g_object_unref (panel)` (the bug)                              | 2 ← drops the registry's ref |

GTK 4's parent-child claims the floating ref via `g_object_ref_sink`
instead of adding a new one. The post-construction ref *is*
the floating ref. `panel_frame_add` already consumed it. There's no
separate "construction ref" to drop. Unrefing here cancels the
registry's strong ref. The table holds the pointer but no ownership;
the next *Close all pages* destroys the `AdwTabPage`, the panel
finalizes, and the registry's stored pointer dangles.

**Rule**: end every embed with `hx_panel_registry_register (panel);` and
nothing after it that touches `panel`.

### Use `gtk_widget_get_ancestor (panel, PANEL_TYPE_FRAME)` to test "attached"

A bare `gtk_widget_get_parent (panel) != NULL` check is too narrow.
libadwaita's `AdwBin` can survive briefly after the `AdwTabPage` that
wraps it is closed, and the panel still appears to have a parent (the
dying bin) even when it's no longer hooked into the dock. Walking up
to a `PanelFrame` ancestor is the truthful test — present iff the
panel is in the dock's tree. `hx_panel_ensure_attached` uses it.

### "Close all pages" detaches but does not destroy frames

The chevron menu's *Close all pages* action loops over the frame's
pages and calls `panel_widget_close` on each. The leaf and its
`PanelFrame` stay in the split tree — only an explicit *Close frame*
destroys a leaf. So the `toolbar_*_frame` pointers remain valid across a
close, and `panel_frame_add` on them during re-attach is safe.

When a leaf *is* closed, `on_frame_close` migrates every page to the
sibling leaf, reseats each moved panel's `home_frame` onto the sibling
frame, and reseats whichever `toolbar_*_frame` globals pointed at the
dying frame — all before calling `hx_split_close_leaf`.

### `panel_frame_remove` is synchronous

Calling `panel_frame_remove` followed immediately by
`panel_frame_add` to a different frame works — libpanel's
default `close-page` handler calls `adw_tab_view_close_page_finish`
synchronously, so the page is gone by the time `remove`
returns. The `n_pages > 0` guard in `hx_split_close_leaf` is
defensive, not async-driven; the move loop in `on_frame_close`
runs before the close attempt and the guard passes.

### `g_weak_ref_get` returns a strong ref — pass it through

`hx_panel_get_home_frame` calls `g_weak_ref_get` and returns the
pointer. The caller must `g_object_unref` it when done. Returning a
"borrowed" pointer would dangle if the weak ref held the last live
reference (the `unref` inside the accessor would destroy the frame
before the pointer left the function). The accessor's header doc
spells this out; `on_undocked_close_request` honours it.

### `disconnect_by_func` with `user_data` is fragile

`hx_panel_undock` connects `on_undocked_close_request` with the
panel that initially populated the undocked window. The user can
later drag additional panels into that same window;
`disconnect_by_func (panel)` then only matches when the
currently-moving panel happens to be the original. The fix is to
stash the handler id at connect time via `g_object_set_data` on the
window and disconnect by id.

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

### libpanel's chevron menu does not surface empty-frame actions

`PanelFrame`'s built-in `frame_menu` (Move Page L/R/U/D + Close
All + frame.close) is what the chevron pops on an empty frame.
Panel-scoped menus only get prepended via the `setup-menu`
signal when a page is selected. Putting "Split / Close frame"
on the panel chevron means they're invisible the moment the
user creates an empty frame — which is exactly when they need
them most. Hence the frame-level affordance instead.

### Action enabled state needs refreshing after splits

`PanelFrame`'s own `gtk_widget_action_set_enabled` calls happen
on selection changes etc. Ours don't — we have to call them
explicitly. When a split lands, the *previously* root leaf
becomes a non-root and its `close-frame` action should flip
from greyed to enabled. `frame_do_split` walks every leaf in
the tree (via `hx_split_foreach_leaf`) and runs
`update_frame_action_enabled` so the state matches the
topology; `on_frame_close` does the same after a collapse.

### `PanelDock` accepts any `GtkWidget` as its center child

`panel-dock.c::panel_dock_add_child` only special-cases
`PanelWidget` heading into a sidebar area (where it wraps in
`PanelPaned` + auto-creates a `PanelFrame`). Every other case
falls through to `panel_dock_child_set_child (dock_child,
widget)` which accepts any `GtkWidget`. So an `HxSplit` as the
center child works without any special handshake, and there's
no requirement to use `PanelGrid` for the center.

### `PanelDock` has no C-level "add child" method

There's no `panel_dock_add_child()` in the public API — the Buildable
interface is the canonical path. `toolbar.c` gets at it by fetching the
dock's `GtkBuildableIface` and calling `add_child (…, NULL)` directly
with the `HxSplit` root; the undocked-window path uses a real
`GtkBuilder` XML string instead.

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

### `panel_init()` is mandatory

It registers `PanelPosition` and friends. Without it,
`gtk_builder_add_from_string` on libpanel XML fails with
`g_type_from_name` errors, and constructing any libpanel widget
crashes. Called from `gtkhx.c` right after `gtk_init()`, before
`fe_init` builds the dock. It's idempotent and doesn't require an
`AdwApplication` instance — which matters, because there isn't one yet
at that point in startup.

## Why the dock stays C

The docked windows' content is Rust; the dock itself is not, and that's
a settled answer rather than a pending item.

Rust bindings for libpanel do exist — the gtk-rs "World"
[`libpanel`](https://crates.io/crates/libpanel) crate (`libpanel-sys` +
`libpanel`). When it was evaluated, its release (0.6.0) was built
against **gtk4-sys ^0.11**, i.e. the gtk-rs **0.22** generation, while
this project pins **gtk4 0.10 / glib 0.21 / libadwaita 0.8** — the
gtk-rs 0.21 family, held there by the Debian-stable rustc floor (see
`rust/Cargo.toml`). Two `-sys` generations of GTK cannot coexist in one
binary, so the crate isn't usable without bumping the entire gtk-rs
stack.

The stronger argument is the second one: **bindings wouldn't remove the
bulk of the work.** The heavy part isn't `HxPanel`, which is a thin
subclass — it's `panel_registry.c`, `hx_panel_frame.c`, `hx_split.c`,
`dock_layout*.c` and the toolbar's dock construction, all wired directly
to libpanel types (`PanelDock`, `PanelFrame`, `PanelGrid`,
`PanelWidget`, `PanelArea`, the `PANEL_WIDGET()` / `PANEL_FRAME()`
casts). Getting a *window's content* into a panel needs almost none of
that; a Rust-native dock means porting all of it, on top of the subclass
work — front-loading a large, stack-wide change before any single window
benefits, which is the wrong order for a leaf-up migration.

So: libpanel and the dock infrastructure stay in C, where they're
understood and tested. Each ported window builds its *content widget
tree* + handlers in Rust and hands the content to a small C bridge that
does the libpanel plumbing. Rust never names a libpanel type. Revisiting
this would take both a gtk-rs stack bump *and* a decision to port the
dock infrastructure itself.

## The `dock_bridge` contract

`src/dock_bridge.{c,h}` is the keystone that makes the above work — the
same leaf-up shape as `tracker_bridge.c` / `gtkhx_ui_bridge.c`: a small,
permanent shim that keeps the wire / session / dock boundary where it
already is. The Rust side is a thin translation layer,
`rust/crates/gtkhx-ui/src/dock.rs`.

Two enums cross as small ints, mirrored in `dock.rs` so Rust never
includes a libpanel header:

- `GtkhxDockKind` — `CENTER` / `SIDEBAR` / `DYNAMIC`, mirroring
  `HxPanelKind`.
- `GtkhxDockArea` — `START` / `END` / `BOTTOM` / `CENTER`. The bridge
  maps each to a `PanelArea` *and* the matching `toolbar_*_frame`, so
  the caller picks one value and the area→frame pairing stays in one
  place.

The API:

| Function | Contract |
|----------|----------|
| `gtkhx_dock_raise_if_open (id)` | Registry lookup; on a hit, `hx_panel_ensure_attached` + `panel_widget_raise`, return TRUE. This is the head of every window entry point — TRUE means "already built, return early". |
| `gtkhx_dock_embed (id, kind, area, title, icon, content)` | Build the `HxPanel`, title/icon it, `panel_widget_set_child (content)`, `panel_frame_add` to the area's home frame, record the home frame, register. |
| `gtkhx_dock_embed_dynamic (…, on_close, user_data, destroy)` | Same, always `DYNAMIC`, plus a close trampoline that fires before the panel is unregistered and finalized. |
| `gtkhx_dock_set_needs_attention (id, state)` | Set / clear the needs-attention hint on a registered panel; no-op if unregistered. Used by the Rust chat-tabs manager to badge the Chat panel from a background tab. |

Ownership is the part worth reading twice: **`content` is always
consumed.** On success the panel takes its reference; on failure (the
toolbar dock isn't built yet — a `g_critical`) the bridge sinks the
floating ref and drops it. Callers skip their post-embed work when the
call returns FALSE and never touch `content` afterwards either way. For
the dynamic variant, a failed embed also runs `destroy` on `user_data`
immediately so the caller's teardown still fires.

A Rust window therefore reads:

```rust
if dock::raise_if_open(HX_ID_TASKS) { return; }
let content = build_content();            // pure gtk4-rs
dock::embed(HX_ID_TASKS, dock::KIND_SIDEBAR, dock::AREA_BOTTOM,
            "Tasks", "view-list-symbolic", content);
```

`gtkhx_dock_embed_dynamic` shipped and is currently unused: it was
designed for per-private-chat / per-PM panels, but those became
`AdwTabView` tabs inside the Chat panel instead, so the prediction that
drove its design turned out to be wrong.

## Appendix A: `GtkActionMuxer` + libpanel chevron routing

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

### What does work: `HxPanelFrame` as a `PanelFrame` subclass

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

### How `HxPanelFrame`'s instance attaches the setup-menu hook

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

## Appendix B: `GtkPaned` size negotiation

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

### `DEFAULT_LEAF_MIN_WIDTH`

`toolbar.c::MAKE_LEAF_FRAME` sets `gtk_widget_set_size_request (out,
DEFAULT_LEAF_MIN_WIDTH, -1)` on every default leaf; the constant lives
in `toolbar.h` so the saved-layout loader uses the same value. It
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

Both `shrink_start_child` and `shrink_end_child` are set FALSE on every
paned `HxSplit` creates (`resize_*` are TRUE). 300 was chosen to fit the
widest of the default panels' button rows (Users: six icon-buttons at
~44px + spacing + margins ≈ 280-290). Bump the constant if a panel still
clips at 300.

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

## Open

- **DnD hit-testing after user splits.** The dock-level `GtkDropTarget`
  hit-tests for the deepest descendant `PanelFrame` and walks through
  `HxSplit` nodes transparently, so it should keep working through
  arbitrary split topologies. Worth verifying against a deeply split
  dock.
- **Move-direction cross-axis descent is arbitrary.** Descending into a
  sub-tree along the perpendicular axis picks `child_a` for
  determinism rather than the leaf nearest the source panel's
  allocation. Fine in practice; occasionally surprising.
- **Named layouts.** Pre-shipped layout presets and per-user named
  layouts don't exist; there is one saved layout and a Reset action.
- **Multi-connection.** With more than one session, each connection
  would want its own Chat panel (and its own tab view — the `AdwTabView`
  singleton becomes a per-Chat-panel field, API unchanged), and the
  question of per-connection vs. one global dock layout becomes real.
- **Window position.** Wayland gives clients no portable way to set
  absolute window position, so only sizes persist — for the main window
  and for undocked panel windows alike.
