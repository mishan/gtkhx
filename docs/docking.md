# Docking UI

The reference for GtkHx's dock: the one main window's panels, how they
are laid out, and how a layout survives a restart.

## What we built

GtkHx used to be a constellation of independent top-level windows — one
each for the toolbar, public chat, news, users, tasks, file browser,
plus dynamic windows for private chats and private messages. Closing the
toolbar quit the app, but everything else was loose and Wayland refused
to position any of them at startup.

The model now is a single main window whose content is the **dock**: a
[mullion-gtk](https://github.com/mishan/mullion-gtk) `MlnPanes`, a split
tree of tabbed panes. Each static tool window is a pane of it. Private
chats and private messages don't get panes of their own — they are
internal tabs inside the Chat panel's own `AdwTabView`. The user can:

- drag a tab to move, stack or split: onto another tab strip, onto a
  pane to stack it there, onto an edge to split;
- drag the dividers, or focus one and use the arrows;
- close a panel, and bring it back from the main menu's Panels section;
- move a panel into a window of its own, by its tab menu or by dragging
  it out of the window, and back by closing that window or dragging it
  in;
- zoom one pane to fill the window, and use mullion's keyboard chords
  (Alt + arrows to move the focus, Alt + Shift + arrows to move a pane,
  Alt + `\` / Alt + `-` to split, Alt + Enter to zoom, Alt + W to close,
  Alt + 0 to reset).

The protocol layer is untouched by any of this — it is a view layer.

mullion-gtk is a GTK 4 port of [mullion](https://github.com/mishan/mullion),
the same model for a web page; the two keep layouts in the same JSON.
GtkHx builds it from `subprojects/mullion-gtk.wrap` (a pinned release,
static) unless an installed copy is found; the Flatpak builds it as a
module.

## What docks and what doesn't

**Docks.** The content widget tree and handlers for each of these live in
the Rust `gtkhx-ui` crate and are embedded through the C `dock_bridge`
(see *The `dock_bridge` contract*); their model halves stay in C.

| Panel        | Content shell                             | Panel id | Slot    |
|--------------|-------------------------------------------|----------|---------|
| Chat         | `gtkhx-ui/src/chat.rs`                    | `chat`   | center  |
| News 1.5     | `gtkhx-ui/src/news_browser.rs`            | `news15` | center  |
| News 1.0     | `gtkhx-ui/src/news.rs`                    | `news`   | start   |
| Users        | `gtkhx-ui/src/users.rs`                   | `users`  | end     |
| Tasks        | `gtkhx-ui/src/tasks.rs`                   | `tasks`  | bottom  |
| Video        | `gtkhx-ui/src/video_panel.rs` (voice builds) | `video` | end   |

The ids are defined once in `src/panel_registry.h`; the titles, icons and
slots in the panel table at the top of `src/dock_bridge.c`.

**Becomes a tab inside the Chat panel:** private chats and private
messages. New activity flags `needs-attention` on the tab and on the Chat
panel's tab in the dock.

**Stays a real window:** Agreement, About, the user editor, the news
composer, file preview; **Files**, one window per connection
(`docs/files-browser.md`); and the **Tracker**, which exists before any
connection.

## The dock (`src/dock_bridge.c`)

`gtkhx_dock_new` makes the dock once, for the main window
(`create_toolbar_window`):

1. Every static panel is **registered as a pane up front**, each with an
   empty `GtkStack` for content (see *Per-connection content*), its title,
   the toolbar's pixmap as its icon, and a *placement*: the slot it goes to
   when a layout has no place for it.
2. The default layout is set, and the saved one read (`dock_layout.c`)
   and loaded over it.
3. Panels come into view as their content is embedded — at startup for
   every panel the layout has open, later for one the user asks for.

Having every pane registered before the layout loads is what lets the
dock answer "is it open?" for a panel that has no content yet, which is
the whole of *closed panels stay closed*: `toolbar_build_panel` builds a
panel at startup only when `gtkhx_dock_is_open` says the layout has it,
and a user's request builds it regardless and presents it.

### Default layout and slots

```
row: [ News, Tasks ]      [ Chat, News 1.5 ]      [ Users ]
       slots start,bottom   slot center             slot end
```

Tasks shares the News column: the queue is empty most of the time, and a
transfer raises its tab (`gtkhx_dock_show_if_open` in `gtask_new`) unless
the user has closed it. A leaf's slots move with it; when the leaf empties,
they pass to the leaf that takes its room, so a panel new in a release (or
Video, which the default layout leaves out) still lands somewhere sensible.

### Headers: the corner or the strip

By default (`MLN_HEADER_CORNER`) a leaf has no tab strip: its tabs sit over
its top corner, in sight while the pointer or the focus is in the pane (or
for good, below) —
the panels' icons, a grip for a panel alone in its leaf, the front one's
close button, and a button for its tab menu. **Pane Titles** in the main
menu (`app.show-pane-titles`, saved as `[Chrome] pane-titles`) puts tab
strips with titles back (`MLN_HEADER_STRIP`).

The corner lies over the pane's content, so the pane makes room: the first
visible widget tagged `.gtkhx-panel-actions` or `.gtkhx-pane-reserve` (an
action row; the chat's subject line) on each of a panel's pages gets an end
margin of the corner's width, from `mln_panes_get_corner_width`, redone on
`::corner-changed` and whenever a page is added or an action row shown or
hidden. Where the page on screen has such a widget, the corner covers
nothing, so it stays in sight (`mln_panes_set_corner_pinned`) rather than
only on hover and focus; a connection switch re-checks, since another page
may have none.

### The tab menu

mullion-gtk's own items (split, move to a new window or back, zoom, close,
reset) and one of GtkHx's: **Show Action Bar**, `app.pane-actions-<id>`,
stateful per panel. It shows or hides the panel's action row — whatever in
its content carries `.gtkhx-panel-actions` — and is greyed for a panel with
none (Chat). The hidden set is saved as `[Chrome] hidden-actions`.

### Windows of their own

A panel moved out of the main window goes into a window `make_window`
makes: the application's, transient for the main window, with
`init_keyaccel`'s keys. mullion-gtk titles it, keeps it and its size with
the layout, and puts its panels back where they were when it is closed.
Wayland gives clients no portable way to set a window's position, so only
sizes persist.

## Per-connection content

A panel holds a *set* of named content pages rather than one child
(`src/dock_pages.{c,h}`): each connection has its own page, and switching
connection swaps every per-connection panel's visible page at once. The
page is named after the connection's serial; at one connection a panel
holds exactly one page. Removing a page is a real teardown — the content
modules' destroy handlers are their model-side teardown — so it is for
closing a connection, never for switching one.

## Startup

`fe_init` builds each open panel for the first connection and raises
Chat, News, Users and Tasks in a fixed order. Those raises would decide
which tab is in front of each leaf — whichever came last — where the saved
layout already says. So until `gtkhx_dock_settled` (the end of `fe_init`),
`gtkhx_dock_raise_if_open` raises nothing; from then on every raise is the
user's.

## Layout persistence (`src/dock_layout.{c,h}`)

`$gtkhx_config_dir/dock-layout.ini`, GKeyFile:

```ini
[Dock]
layout={"layout":{"dir":"row","size":[0.24,0.58,0.18],"kids":[...]},"closed":["news15"],"floating":[{"layout":{"tabs":["users"]},"size":[500,400]}]}

[Chrome]
toolbar=true
pane-titles=true
hidden-actions=news;users

[Windows]
files=900,600
```

`layout` is the dock's own JSON, handed over on `::layout-kept` and handed
back to `mln_panes_load`: the tree with a share per split child and each
leaf's tabs, front tab (`active`) and slots; the panels the user closed;
the windows of their own with their sizes. mullion reads the same tree.

`[Chrome]` keys are written only when they differ from the default
(toolbar and pane titles off, every action row shown). `[Windows]` keeps
the sizes of windows that are not panels (Files).

Saves are debounced on a 200 ms timer; `dock_layout_shutdown` (from
`hx_quit`) flushes a pending one. **Reset Layout** (`app.reset_layout`)
puts the default layout up now; the chrome settings stay.

### Layouts from before mullion-gtk

A file written by the libpanel dock has `[Dock] tree=`, `sizes=`,
`closed=` and an `[Undocked]` group instead of `layout=`. It is read once,
as the same layout — `dl_import_legacy` in `dock_layout_parse.c`, which is
GLib only and unit-tested — and written in the new form at the next save:

- `h(A,B)` / `v(A,B)` become row / column splits; the divider positions
  (pixels, post-order) become shares of the main window's saved size;
- `L[id,*id:role]` becomes a leaf with its tabs, the `*` page as `active`
  and the role as a slot;
- `closed=` becomes the envelope's `closed`, and each `[Undocked]` panel a
  floating window of its own with its size;
- `files`, a panel once and a window now, is left out.

## The `dock_bridge` contract

`src/dock_bridge.{c,h}` is the small, permanent shim between the Rust
window shells and the dock; the Rust side is a thin translation layer,
`rust/crates/gtkhx-ui/src/dock.rs`. Two enums cross as small ints,
`GtkhxDockKind` and `GtkhxDockArea`; the dock no longer tells kinds apart,
and the slot a panel goes to is the panel table's, so both are kept for the
ABI.

| Function | Contract |
|----------|----------|
| `gtkhx_dock_is_embedded (id)` | Whether the panel has content yet: a routing question (is this connection's content a new panel or a page?), which raises nothing. |
| `gtkhx_dock_raise_if_open (id)` | Whether the panel has content; if so, present it (reopening it if closed) once startup has settled. |
| `gtkhx_dock_embed (id, kind, area, title, icon, page, content)` | The panel's first content: `content` as page `page`, and the title. |
| `gtkhx_dock_add_page` / `has_page` / `show_page` / `remove_page` / `page_count` | A connection's page, through `dock_pages`. |
| `gtkhx_dock_set_needs_attention (id, state)` | The attention mark on the panel's tab, until it is looked at. |
| `gtkhx_dock_connect_shown (id, func)` | `func` each time the panel comes into view (the News browser's fetch on open). |

**`content` is always consumed**: on success the panel's stack takes it; on
failure (no such panel, or the page name taken) the bridge sinks the
floating ref and drops it. Callers skip their post-embed work when the
call returns FALSE and never touch `content` afterwards either way.

The C-only half — `gtkhx_dock_new`, `_settled`, `_is_open`, `_present`,
`_show_if_open`, `_set_pane_titles`, `_reset`, `_add_actions` — is what
`toolbar.c`, `gtkhx.c` and `tasks.c` use.
