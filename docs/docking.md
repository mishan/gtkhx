# Docking UI

**Status: Phase 4a shipped.** This is the authoritative reference for the
GtkHx docking work — what's shipped, why it's shaped the way it is, the
libpanel quirks worth remembering, and what's left. Supersedes the older
`docking-scoping.md` and `docking-phase0-findings.md` files (both deleted
when this consolidated doc landed).

## What we built

GtkHx used to be a constellation of independent top-level windows — one
each for the toolbar, public chat, news, users, tasks, file browser,
plus dynamic windows for private chats and private messages. Closing the
toolbar quit the app, but everything else was loose and Wayland refused
to position any of them at startup.

The new model is a single dockable window. The toolbar window hosts a
[`libpanel`](https://gitlab.gnome.org/GNOME/libpanel) `PanelDock` with
three sidebar areas (start / end / bottom) and a center `PanelGrid`.
Each of the six static tool windows became an `HxPanel` inside that
dock. Private chats and private messages don't live in the dock — they
became internal tabs inside the Chat panel's own `AdwTabView`. The
result:

- One main window. Saved size restores on startup; Wayland can position
  the rest of the dock layout on its own.
- The user can DnD panels between frames, drag them out into their own
  windows, drag them back in, undock / redock from a chevron menu.
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
| `PanelDock`          | Top-level dock container. The toolbar window's content area.    |
| `PanelFrame`         | A tabbed group of panels. Every dock area holds one.            |
| `PanelGrid`          | Center area only — lazily creates `PanelFrame`s on add.         |
| `PanelWidget`        | One dockable panel (title, icon, content).                      |
| `panel_widget_close` | Standard close path; we hook `PanelFrame::page-closed`.         |

Pinned floor: `libpanel-1 >= 1.4` in `meson.build`. The Flatpak runtime
ships libpanel-1 in `org.gnome.Platform/49`, so the dependency is cheap
on Flathub.

## Phase status

| Phase   | What                                                       | Status |
|---------|------------------------------------------------------------|--------|
| 0       | Spike — de-risk libpanel API                               | ✅     |
| 1       | Infrastructure: `HxPanel`, panel registry, dock host        | ✅     |
| 2       | Migrate Users / Tasks / News 1.0 / Chat / Files / News 1.5  | ✅     |
| 3       | Conversation tabs inside the Chat panel (pchats + PMs)      | ✅     |
| 4a      | Drag-and-drop undock / redock                              | ✅     |
| 4b      | Layout persistence (save dock state, restore on startup)    | ⬜ next |
| 5       | Custom-group polish, "Reset layout" action, default layout  | ⬜     |
| ∞       | Multi-connection (one `Chat` panel per server, etc.)        | ⬜     |

Phase 2 and Phase 3 have one commit per migration on
`claude/docking-spike`; Phase 4a is one commit (`docking Phase 4a:
drag-out + drag-in (redock) via libpanel hookup`) plus the subsequent
"Copilot review pass N" fixups.

## What docks and what doesn't

**Docks** (persistent tool windows that became `HxPanel`s in the
toolbar dock):

- Chat (`chat.c`, `create_chat_window`, `HX_PANEL_ID_CHAT`, CENTER)
- News 1.0 (`news.c`, `HX_PANEL_ID_NEWS`, CENTER)
- News 1.5 (`news_browser.c`, `HX_PANEL_ID_NEWS15`, CENTER)
- Files (`files_browser.c`, `HX_PANEL_ID_FILES`, CENTER)
- Users (`users.c`, `HX_PANEL_ID_USERS`, START sidebar)
- Tasks (`tasks.c`, `HX_PANEL_ID_TASKS`, BOTTOM sidebar)

**Becomes a tab inside the Chat panel** (Phase 3):

- Private chats (`chat.c::create_pchat_window`, `sess->gchats` keyed
  by cid)
- Private messages (`msg.c::create_msgwin`, `sess->msg_windows` keyed
  by uid)

The Chat panel hosts an `AdwTabView` whose first tab is the public
chat (pinned), with one closeable tab per active conversation. New
activity flags `needs-attention` on the tab and on the Chat dock
panel; selecting any tab clears the indicator.

**Stays a real window** (transient, modal, one-shot, pre-connection,
or fundamentally not server-content):

- Agreement, About, user editor, post-news composer, file preview.
- **Tracker** — server-discovery, not server-content. Exists *before*
  a connection (it's how the user picks one). Stays a standalone
  window owned by its own file-static; the panel registry doesn't
  absorb it.

## Architecture

### `HxPanel` (`src/hx_panel.{c,h}`)

`G_DECLARE_FINAL_TYPE` subclass of `PanelWidget`. Carries:

- A stable string id (`"chat"`, `"news"`, `"users"`, …) so the
  registry can index it and layout persistence (Phase 4b) can refer to
  panels by name, not pointer.
- A kind tag (`CENTER` / `SIDEBAR` / `DYNAMIC`) so the registry knows
  which area to home the panel to.
- A `home_area` and a `home_frame` `GWeakRef`. The area is consulted
  by `hx_panel_ensure_attached` when re-attaching after a close; the
  frame is consulted by `on_undocked_close_request` when the user
  closes the undocked window.

The chevron menu's *Undock* and *Move to …* GActions live on `HxPanel`
via libpanel's `PanelActionMuxer`. `hx_panel_undock` is the public
entry point — both the menu action and the drag-out detector call it.

### Panel registry (`src/panel_registry.{c,h}`)

`GHashTable<id → HxPanel>`. Owns the only post-construction strong ref
to each registered panel — this is what keeps a panel alive across
*Close all pages*, so the toolbar button can re-attach it later.

Lifetime: lazily created on first call. Static panels stay registered
for the process lifetime; dynamic panels unregister explicitly when
their backing model object goes away. `HxPanel::finalize` does not
touch the registry (it would create a chicken-and-egg with the
registry's strong ref).

### Toolbar dock host (`src/toolbar.c`)

`create_toolbar_window` builds the `PanelDock` with three sidebar
`PanelFrame`s (start / end / bottom) and a center `PanelGrid`. Each
frame gets its drag-out hook (`hx_panel_install_drag_out_on_frame`)
and its close dispatcher (`hx_panel_install_close_dispatcher`);
`PanelDropControls` are defanged (`hx_panel_defang_drop_controls_on_frame`)
so events reach the dock-level drop target
(`hx_panel_install_drop_target_on_dock`).

The toolbar's News / Files / Users / Tasks buttons route through
`toolbar_show_panel` which does registry-lookup + raise. News (1.0),
News (1.5), and Chat keep their own click handlers because they need
to fire server fetches (`hx_get_news`, NEWSDIRLIST) when connected.

### Chat panel internal tab strip (`src/chat_tabs.{c,h}`)

Singleton `AdwTabView` inside the Chat panel. Hash-table indices map
`cid → AdwTabPage*` for pchats and `uid → AdwTabPage*` for PMs.
`AdwTabView::close-page` dispatches by `(kind, id)` stored on the
page via `g_object_set_data` to the teardown handler registered by
each module (`pchat_close` in chat.c, `msg_tab_on_close` in msg.c).

When `MAX_CONN > 1` lands, each connection gets its own Chat panel
with its own tab view. The singleton turns into a per-Chat-panel
field; the API stays the same.

## Phase 4a — Drag-and-drop

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
   `PanelFrame`s, pick the deepest match, and move the panel via
   `panel_frame_remove` / `panel_frame_add`. For cross-dock drops, if
   the source undocked window becomes empty we destroy it (after
   disconnecting its close-request handler so the redock path doesn't
   race the already-moved panel).

`hx_panel_undock` is context-aware: when invoked on a panel that's
already in an undocked window (chevron menu in that window), it
closes the undocked window instead, which triggers
`on_undocked_close_request` to redock the panel to its home area.

### libpanel quirks worth knowing

These all bit us during Phase 4a. They're documented inline at the
relevant call sites; recap here:

- **`GtkDragSource:actions` defaults to 0.** libpanel's drag handle
  declares no actions in its `.ui`, so the drag's action set ends up
  empty. With actions=0 no drop target can succeed in action
  negotiation — every drop target's enter silently doesn't fire,
  every drop ends in `drag-cancel`. We explicitly call
  `gtk_drag_source_set_actions(MOVE | COPY)` on libpanel's drag
  source after we find it.
- **The drag content isn't on the source.** libpanel sets the drag
  content via the `GtkDragSource::prepare` signal's return value, not
  via `gtk_drag_source_set_content`. So `gtk_drag_source_get_content`
  returns NULL. The actual content lives on the `GdkDrag` —
  `gdk_drag_get_content(drag)` works.
- **`PanelDropControls` doesn't accept drops in our setup.** It's an
  invisible overlay child of each `PanelFrame`. During drag it becomes
  visible (per libpanel's normal flow) and consumes drop events
  without firing a usable accept. Setting `can-target=FALSE` on every
  `PanelDropControls` (found by walking the frame tree and matching
  `G_OBJECT_TYPE_NAME == "PanelDropControls"`) makes it transparent so
  the dock-level target sees the drop.
- **No public drag-handle accessor.** `PanelFrameHeaderBar`'s drag
  button is private. We find it by traversing the frame tree looking
  for a `GtkButton` with `icon-name == "list-drag-handle-symbolic"`,
  then look up the `GtkDragSource` controller installed on it. Stable
  surface today; worth flagging if a future libpanel restructures the
  header.
- **`G_TYPE_INVALID` + `gtk_drop_target_set_gtypes`.** Single-type
  `GtkDropTarget` constructors don't always match. Going broad —
  `{ PANEL_TYPE_WIDGET, GTK_TYPE_WIDGET, G_TYPE_OBJECT }` — makes the
  `GdkContentFormats` intersection succeed reliably. Preload TRUE so
  enter / motion see the value.

## Phase 4b — Layout persistence (next)

Open. The plan:

- Serialize which panels exist, which frame/area each sits in, the
  pane sizes, and any custom DnD-formed groups. GKeyFile.
- On quit, write to `~/.config/com.nasledov.gtkhx/dock-layout.ini`.
- On startup, restore the layout before the dock fills with the
  default placement. This replaces the per-window `save_geo` /
  `gtkhx_save_window_positions` machinery and finally kills the
  Wayland startup-positioning problem — one window, one saved
  layout.
- "Reset layout" action in the hamburger menu wipes the file and
  rebuilds the default placement.

Per-connection layouts vs. one global layout will become relevant
once multi-conn lands.

## Implementation gotchas

The next person writing or modifying a panel factory needs to know
these up front. They each cost a debugging session.

### Don't `g_object_unref` after `hx_panel_registry_register`

The intuitive read is "the frame holds the widget-tree ref; the
registry's `g_object_ref` is the extra one; my factory's
post-construction ref is the third — drop it." That's wrong. Refcount
walk for the Users factory:

| Step                                                            | Refcount |
|-----------------------------------------------------------------|----------|
| `hx_panel_new`                                                  | 1 (floating) |
| `panel_frame_add` → `AdwTabPage` "child" set:                   |              |
| &nbsp;&nbsp;`g_set_object (&page->child, panel)`                | 2 (floating) |
| &nbsp;&nbsp;`adw_bin_set_child` → `gtk_widget_set_parent` → `g_object_ref_sink` | 2 (floating cleared, **no new ref**) |
| `hx_panel_registry_register` — internal `g_object_ref`          | 3            |
| `g_object_unref (panel)` (the bug)                              | 2 ← drops the registry's ref |

GTK 4's parent-child claims the floating ref via `g_object_ref_sink`
instead of adding a new one. Your factory's post-construction ref *is*
the floating ref. `panel_frame_add` already consumed it. There's no
separate "construction ref" to drop. Unrefing here cancels the
registry's strong ref. The table holds the pointer but no ownership;
the next *Close all pages* destroys the `AdwTabPage`, the panel
finalizes, and the registry's stored pointer dangles.

**Rule**: end every panel factory with
`hx_panel_registry_register (panel);` and nothing after it that
touches `panel`.

### Use `gtk_widget_get_ancestor (panel, PANEL_TYPE_FRAME)` to test "attached"

A bare `gtk_widget_get_parent (panel) != NULL` check is too narrow.
libadwaita's `AdwBin` can survive briefly after the `AdwTabPage` that
wraps it is closed, and the panel still appears to have a parent (the
dying bin) even when it's no longer hooked into the dock. Walking up
to a `PanelFrame` ancestor is the truthful test — present iff the
panel is in the dock's tree. `hx_panel_ensure_attached` uses it.

### "Close all pages" detaches but does not destroy frames

The chevron menu's *Close all pages* action loops over the frame's
pages and calls `panel_widget_close` on each. The frame itself stays
alive: its dock-child's `notify::empty` collapses the sidebar
revealer, but the center grid keeps the frame. So
`toolbar_end_frame` / `toolbar_bottom_frame` / `toolbar_sidebar_frame`
are valid pointers across a close. `panel_frame_add` on them during
re-attach is safe.

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
`disconnect_by_func(panel)` then only matches when the
currently-moving panel happens to be the original. The fix is to
stash the handler id at connect time via `g_object_set_data` on the
window and disconnect by id.

## Phase 0 — historical API surprises

The phase-0 spike on `claude/docking-spike` (predates everything else)
proved the libpanel API maps onto our requirements. The four
load-bearing findings still matter:

1. **`PanelDock` has no C-level "add child" method.** Build the main
   window from a `.ui` file. The Buildable interface is the canonical
   path: `<child type="start|end|top|bottom">` puts a `PanelFrame` or
   `PanelWidget` in that area, and `<child>` (no type) adopts the
   central widget — usually a `PanelGrid`.
2. **Center area is a `PanelGrid`, not a `PanelFrame`.** The grid
   lazily creates its own `PanelFrame`s via the `create-frame` signal.
   The registry indexes `PanelWidget`s, not `PanelFrame`s.
3. **Side-area frames are added directly to the dock.** Users / Tasks
   are natural sidebar residents (START / BOTTOM); Chat / News / Files
   are center-area documents.
4. **Detach-to-window is not in libpanel's public API.** We compose
   it: `panel_frame_remove (old)`, build a fresh dock window,
   `panel_grid_add (new_grid, panel)`. `hx_panel_undock` is the
   bottleneck. `AdwTabView::create-window` doesn't work on
   `PanelFrameHeaderBar` either — we wire drag-out via the
   `drag-cancel` mechanism described in the Phase 4a quirks section.

Also: `panel_init()` is mandatory before any libpanel widget
construction. It registers `PanelPosition` and friends; without it
`gtk_builder_add_from_string` on libpanel XML fails with
`g_type_from_name` errors. Called from `gtkhx.c::app_startup`.
