# Porting the docked windows to Rust — libpanel scoping

**Status:** scoping / decision doc. Written after the R5.3 (Connect dialog)
port, when the next roadmap window (Tasks) surfaced the fact that the
remaining UI is dominated by windows embedded in the **libpanel dock**, and
that we need one deliberate strategy for all of them rather than an ad-hoc
decision per window.

## TL;DR

The bulk of the still-C UI — **Chat, Users, News, News browser (1.5), Tasks,
Files** — are not standalone windows. Each is an `HxPanel` (a subclass of
libpanel's `PanelWidget`) docked into a `PanelFrame` inside the toolbar
window's `PanelDock`. gtk4-rs has no libpanel bindings, and libpanel is a
fast-moving GNOME-Builder library.

**Recommendation:** do **not** bind libpanel in Rust. Keep libpanel + the
`HxPanel` / dock infrastructure (`hx_panel.c`, `panel_registry.c`,
`hx_panel_frame.c`, `hx_split.c`, `dock_layout*.c`) in C, and add **one
small, reusable C "dock-embed" bridge** (`dock_bridge.c`). Each ported
window builds its *content widget tree* + handlers in Rust and hands the
content to the bridge, which does the `hx_panel_new` / `panel_frame_add` /
registry plumbing. This is the same leaf-up shape as `tracker_bridge.c` /
`gtkhx_ui_bridge.c`, keeps the wire/session boundary where it already is,
and involves zero throwaway once Files/xfers land (the bridge is permanent
until the dock itself is someday ported, which is out of scope for R5).

## Current state

### What's docked vs standalone

Standalone `GtkWindow` / `AdwDialog` (no libpanel) — **all already ported**:

- Tracker (R5.1) — `gtk::Window`, `present()`. Never docked (pre-R5.1
  `tracker.c` also used `gtk_window_new`; the `HX_PANEL_KIND_CENTER`
  comment listing "tracker" and the `HX_PANEL_ID_TRACKER` define are stale
  — corrected in this pass).
- About / Agreement / User Editor (R5.2), Connect (R5.3) — dialogs.

That's *why* libpanel hasn't come up yet: nothing docked has been ported.

Docked (`hx_panel_new` → `panel_frame_add`), **all still C**:

| Window        | file               | id        | kind    | home area |
|---------------|--------------------|-----------|---------|-----------|
| Chat          | `chat.c`           | `chat`    | CENTER  | CENTER    |
| Files         | `files_browser.c`  | `files`   | CENTER  | CENTER    |
| News browser  | `news_browser.c`   | `news15`  | CENTER  | CENTER    |
| News (1.0/1.2)| `news.c`           | `news`    | SIDEBAR | START     |
| Users         | `users.c`          | `users`   | SIDEBAR | END       |
| Tasks         | `tasks.c`          | `tasks`   | SIDEBAR | BOTTOM    |

Plus **dynamic** panels (`HX_PANEL_KIND_DYNAMIC`): per-private-chat and
per-private-message tabs spun up by `chat.c` / `msg.c`, which additionally
register a close handler (`hx_panel_set_close_handler`) that tears down the
backing `gchat` / `msgwin` state when the tab closes.

### What the dock actually is

`HxPanel : PanelWidget` (libpanel) is deliberately thin — it adds a string
`id`, a `kind`, a `home_area`, a `GWeakRef` to its home `PanelFrame`, and an
optional close callback. Content goes in via libpanel's inherited
`panel_widget_set_child()`. The heavy lifting (frames, splits, drag-out,
undock-to-window, layout persistence) lives in `panel_registry.c`,
`hx_panel_frame.c`, `hx_split.c`, `dock_layout*.c` and the toolbar's dock
construction — several thousand lines of C built directly on libpanel types
(`PanelDock`, `PanelFrame`, `PanelGrid`, `PanelWidget`, `PanelArea`, the
`PANEL_WIDGET()` / `PANEL_FRAME()` casts).

### The per-window shape today

Every docked window follows the same two-function split:

- `create_X(sess)` — build the content widgets early (e.g. `create_tasks`
  builds the `GtkListBox` + scroller and stashes them on the session).
- `create_X_window(...)` — on the toolbar button: look the panel up in the
  registry; if present, re-attach + raise; else `hx_panel_new(...)`,
  `panel_widget_set_child(panel, content)`, `panel_frame_add(frame, panel)`,
  `hx_panel_set_home_frame`, `hx_panel_registry_register`.

The model→view entry points (`task_update`, `file_update`, `output_*`, the
`user_*` / news / chat updaters) are called from C model files and would
become `#[no_mangle]` Rust exports, exactly as the tracker's did.

## Options considered

### A. Generic C dock-embed bridge — **recommended**

One new `dock_bridge.{c,h}` exposing a handful of C functions that wrap the
libpanel plumbing so Rust never names a libpanel type:

```c
/* Raise an already-open panel; returns TRUE if it existed (Rust then
 * returns early instead of rebuilding content). */
gboolean gtkhx_dock_raise_if_open (const char *id);

/* Create-or-embed: builds the HxPanel for `id`, sets `content` as its
 * child, adds it to the home frame for `area`, registers it, and raises.
 * `kind`/`area` are passed as small ints (mirrored enums in a tiny Rust
 * `mod dock`), not libpanel types. */
void gtkhx_dock_embed (const char *id, int kind, int area,
                       const char *title, const char *icon_name,
                       GtkWidget *content);

/* Dynamic panels only (chat tabs / PMs): same, plus a Rust close
 * trampoline invoked before unregister. */
void gtkhx_dock_embed_dynamic (const char *id, int area,
                               const char *title, const char *icon_name,
                               GtkWidget *content,
                               void (*on_close)(void *ud), void *ud);
```

A Rust window then reads:

```rust
if dock::raise_if_open(HX_ID_TASKS) { return; }
let content = build_content();            // pure gtk4-rs
dock::embed(HX_ID_TASKS, KIND_SIDEBAR, AREA_BOTTOM, &tr("Tasks"),
            "view-list-symbolic", &content);
```

- **Pros:** libpanel stays entirely in C where it's understood and tested;
  no gir/binding maintenance; tiny, permanent bridge (mirrors the existing
  `*_bridge.c` pattern); each window port is "build content in Rust +
  register via bridge"; zero rework when Files/xfers land; the `create_X` /
  `create_X_window` split maps cleanly (build-content vs embed).
- **Cons:** the bridge grows a couple of entry points for dynamic-panel
  close callbacks and for any per-window dock nuance (e.g. Files' center
  placement vs sidebar); the dock itself never becomes Rust-native (a
  non-goal for R5).

### B. Bind libpanel + HxPanel in Rust (gir or hand-written)

Generate `libpanel-rs` from libpanel's `.gir` and expose/port `HxPanel` as
a Rust `glib::subclass` of `PanelWidget`.

- **Pros:** idiomatic; Rust windows drive the dock directly; no bridge.
- **Cons:** large, ongoing cost. libpanel has no stable published Rust
  bindings; we'd own a gir crate pinned to the GNOME version we ship
  (Flatpak GNOME 49 today), tracking API churn. Subclassing `PanelWidget`
  from Rust needs the full base-class bindings *and* the subclass vtable
  plumbing. This is strictly more work than Option A and front-loads it
  before any window benefit — the wrong order for a leaf-up migration.

### C. Port the dock infrastructure to Rust first

Port `hx_panel.c` + `panel_registry.c` + `hx_split.c` + `dock_layout*.c` to
Rust before the windows.

- **Cons:** subsumes Option B's binding cost *and* is upside-down ordering
  (infra before leaves), for no near-term payoff. Rejected.

## Decision

**Option A.** Add `dock_bridge.{c,h}`; keep libpanel/HxPanel/dock infra in
C. Ported docked windows build content + handlers in Rust and register
through the bridge. Revisit a Rust-native dock (Option B) only if/when
libpanel gains maintained bindings *and* the dock infra is the last C UI
standing — explicitly out of scope for R5.

## Application to each window (leaf-up order)

The bridge is built once, as part of the **first docked-window port**. Users
is the natural first target: SIDEBAR/simple, no transfer coupling, and it
pairs with the chat work later.

1. **Users** (`users.c`, `users_row.c`, `users_view.c`) — build the bridge
   here. `GtkColumnView` (already), user-list model, right-click popup.
   SIDEBAR/END. Model entry points: `user_create` / `user_delete` /
   `user_change` / `users_clear` / `user_info`.
2. **Tasks** (`tasks.c`, keep `tasks_table.c` + the protocol task model in
   C) — SIDEBAR/BOTTOM. Needs the small htxf/task field accessors scoped in
   the abandoned Tasks attempt (progress/size/path/label) plus thin FFI to
   the xfers queue API (`xfer_up/down/go/num/delete`). Some of that bridge
   is throwaway once Files/xfers move — acceptable, and smaller than a
   pre-Files attempt implied once the dock bridge exists.
3. **News** (`news.c`) and **News browser** (`news_browser.c`) — first
   windows with real async (fetch-list / fetch-thread). Good exercise of the
   R3 tokio→GLib bridge from the UI side.
4. **Files** (`files_browser.c` + the `files_*.c` subsystem) — the largest;
   drag-and-drop, virtual folders, transfer integration. Do Tasks' htxf
   coupling and Files together (or Files first) so the transfer-side bridge
   is designed once.
5. **Chat** (`chat.c`) — CENTER + the **dynamic** per-pchat/PM panels. This
   is where `gtkhx_dock_embed_dynamic` + the close-callback trampoline get
   used; xtext stays vendored C behind a tiny gtk4-rs wrapper (see ROADMAP).

`options.c` (Settings) is **not** docked (it's an `AdwPreferencesDialog`), so
it can be ported any time independent of this bridge — a good clean win to
slot in between docked-window ports.

## What stays C / what moves to Rust (per docked window)

- **Stays C:** libpanel, `HxPanel`, `panel_registry.c`, `hx_panel_frame.c`,
  `hx_split.c`, `dock_layout*.c`, the toolbar's dock construction, and the
  new `dock_bridge.c`. Also each window's protocol/model half that already
  lives outside the UI (e.g. the task hashtable + `tasks_table.c`, the wire
  senders).
- **Moves to Rust:** the window's content widget tree, its event handlers,
  and its `#[no_mangle]` model→view entry points — registered into the dock
  via the bridge.

## Corrections made in this pass

- The tracker was **never docked**. Fixed the stale comments in `hx_panel.h`
  (the panel-list and `HX_PANEL_KIND_CENTER` enum comment) that implied it
  was, and removed the unused `HX_PANEL_ID_TRACKER` define from
  `panel_registry.h` (no `.c` referenced it).
