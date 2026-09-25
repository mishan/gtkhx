# Files browser

GtkHx's file browser is an orthodox file manager: two panels side by side, one
active at a time, operations routing between them. It replaced a single-pane
window-per-directory browser and is the only files UI — the legacy path
(`open_files`, the per-path list cache, the `file_samewin` preference) is gone,
leaving `src/files.c` as wire senders plus the Get Info dialog plumbing.

The implementation: `rust/crates/gtkhx-ui/src/files.rs` (the window),
`src/files_browser.c` (the content: shared actions, transfer buttons, the row
menu, drag-and-drop, keyboard, active-panel state), `src/files_panel.c` (one
panel — path row, `GtkColumnView`, status footer), `src/files_ops.c`
(cross-panel copy / move orchestration), and the two providers,
`src/files_local_provider.c` (GIO) and `src/files_remote_provider.c`
(Hotline).

### A window, not a dock panel

The browser is a window of its own, one per connection, titled with the
server's name — not a panel in the main window's dock. A two-panel file
manager wants more width than a dock frame gives it (docked, it shared the
center frame with Chat at about 450px, and neither panel could show more than
names), and it is used in bursts: browse, queue transfers, leave. Transfers
carry on in the Tasks panel, so closing the window costs nothing, and closing
it closes the browser — the content's destroy is the browser's teardown.

The window opens from the main menu (Files) or the toolbar's Files button,
for the connection the user is looking at; a second request raises the open
one. Its title is retitled at login, when the server has named itself. Its
size is saved to the dock layout file's `[Windows]` group (`files=W,H`) as
the user changes it — not at close, since a connection closing destroys the
window without a close-request and quitting destroys nothing. A connection
that goes away takes its window with it (`gtkhx_dock_remove_session_pages`).
The window installs the app-wide accelerators itself, so Ctrl+W closes it.

A layout saved while Files was a dock panel still names it. The loader
prunes the id (`dl_tree_drop_panel`); a leaf Files had to itself collapses
rather than coming back as an empty pane, and the saved divider positions
skip the splits that went with it.

---

## What "orthodox file manager" means for a Hotline client

Norton Commander / Midnight Commander / Total Commander lineage: two flat
directory listings, one panel active, keyboard first-class, and every operation
implicitly addressed as "from the active panel to the other one". The Hotline
adaptation is that one side is a remote server whose "copy" is a file transfer.

```
┌─[ ⟳ 📁 👁 ℹ ]──────── Files — Server ─────────[ ✎ 🗑 ]─┐
│ [Local ▾][↑][~/Downloads    ] [Remote ▾][↑][/Music    ] │
│ ┌──────────────────────────┐ ┌──────────────────────────┐ │
│ │ Name        Size  Modif. │ │ Name          Size Modif.│ │
│ │ Album.zip  142 M     Tue │ │ Albums    (7 items)  2024│ │
│ │ song.flac   31 M     Mon │ │ song2.mp3   4.8 M     Wed│ │
│ └──────────────────────────┘ └──────────────────────────┘ │
│ 0 of 12 selected  [Upload ›] 1 of 47 selected [‹ Download]│
└───────────────────────────────────────────────────────────┘
```

The active panel carries a thin accent outline. Each panel's footer has its
transfer button, which says in words what it does to that panel's selection
(see *Transfer buttons*).

### Function keys to Hotline operations

The bindings live in one shortcut controller in `files_browser.c`, and every
wrapper routes to the same handler the matching headerbar button uses, so the
behaviour is identical whether you pressed the key or clicked the icon.

| Key | Alternate | Operation | On the remote side |
|---|---|---|---|
| F2 | — | Rename | `HTLC_HDR_FILE_MOVE` in place |
| F3 | Ctrl+P | Preview | fetch + `preview.c` |
| F4 | — | Open / activate | descend, or `xdg-open` locally |
| F5 | — | Copy active → inactive | download, upload, or GIO copy |
| F6 | — | Move (destination dialog, defaulting to the inactive panel's path) | `HTLC_HDR_FILE_MOVE` |
| F7 | Ctrl+N | New folder | `HTLC_HDR_FILE_MKDIR` |
| F8 | Delete, Ctrl+D | Delete | `HTLC_HDR_FILE_DELETE` |
| — | Ctrl+I | Get Info | `HTLC_HDR_FILE_GETINFO` |
| — | Ctrl+R | Reload | re-list |
| Shift+F10 | Menu | Row menu at the focused row | — |
| Tab | — | Switch active panel | — |
| Backspace | — | Up one directory | — |

**Several function keys get stolen by desktop compositors** — F-keys are
commonly grabbed for media controls and brightness. That is why each classic
binding has a Ctrl-equivalent, following GNOME convention where it overlaps
(Ctrl+N, Ctrl+R) and being novel-but-reasonable where it doesn't (Ctrl+P,
Ctrl+D). Two deliberate gaps: F5 has no Ctrl form, because Ctrl+C is universally
clipboard-copy and stealing it would break the mental model for the whole app;
F6 has none, because Ctrl+M collides with Return in terminal legacies and Ctrl+I
is already Get Info.

### Active panel vs. GTK focus

The browser keeps an `active` panel pointer, and the panel it names gets the
accent CSS class and is the source for every cross-panel operation. It is
*derived from* GTK focus rather than being a parallel notion of focus: each
panel's root has a focus controller whose `enter` sets the marker, plus a click
gesture as a second path in.

The click gesture runs in the **bubble** phase, not capture, and that detail is
load-bearing. With the gesture in the capture phase the column view saw the
first click of a double-click as a plain selection-with-focus-shift and waited
for another pair before treating it as a double — so the first double-click in
the non-active panel did nothing and only the second one descended. Observing
on the way back up leaves the column view's own click counting intact, and the
focus controller covers the clicks the column view fully consumes.

### Shared chrome

One window, one header bar — the panels share the chrome rather than each
carrying their own. The single-panel actions (refresh, new folder, preview, get
info at the start; rename, delete at the end) sit in the window's header bar:
the content builds them and hands them over on its content box
(`hx-files-header-start` / `-end`), and `files.rs` packs them. Per-panel chrome
is the path row, the side selector, and the footer.

### Transfer buttons

Each panel's footer ends in a button that sends its selection to the other
panel's folder. It is labelled with what that is, from the two panels' sides:
**Download** (remote to local), **Upload** (local to remote) or **Copy** (both
local), with an arrow toward the other panel, and it is insensitive with
nothing selected. With both panels remote it stays insensitive, its tooltip
pointing at Move (F6): Hotline has no server-side copy. The label and state
follow selection changes, new listings, and side swaps. It replaced a column of two arrow buttons between the
panels, whose direction was clear and whose meaning wasn't — and which ate the
middle of the window. F5 still copies from the active panel.

### Row menu

Right-click on a row acts on that row: it joins the selection if already in
it, and replaces the selection otherwise, so the menu never acts on rows the
user can't see are picked. The menu offers Open, the transfer (with the same
verb as the footer button, and left out when both panels are remote),
Preview, Get Info, Move, Rename, Delete, New Folder and Reload; on empty
space, only the last two. Shift+F10 or the Menu key opens it at the focused
row. Open acts on the entry the menu opened on, found again at open time,
so a reload underneath doesn't retarget it. The row under the pointer is found
through the `GtkListItem` every cell's bind stashes on its widget
(`files_panel_entry_at`).

### Layout

The two panels start level — half the paned's width, applied once the
window's width has held across two frames, since a window's first allocations
arrive in steps (`on_paned_settle_tick`) — and resize together from then on.
Name takes whatever width Size and Modified leave; Kind starts hidden, because
the icon already says folder or file, and comes back from any column header's
right-click menu. Row icons show at their native 16px.
Sizes are rounded in the column, with the exact byte count as the cell's
tooltip.

### Decisions

1. **Left panel is local, right panel is remote** by default — the browser
   constructs a local provider for the left panel and a remote provider for the
   right. (An earlier draft of this document said the opposite in one section
   and this in another; the code is the arbiter.)
2. **Each panel has a side selector** — a two-item dropdown in the path row
   ("Local" / "Remote") that swaps the panel's provider, so both panels can show
   remote directories at once. The dropdown tracks provider identity rather than
   driving it: the selection is set from the actual provider after a swap.
3. **One browser window per connection**, a window rather than a dock panel
   (see *A window, not a dock panel*). `file_samewin` is retired.
4. **Buttons first, keyboard second.** Every operation is reachable by click;
   the F-keys and Ctrl-equivalents are accelerators onto the same handlers.
5. **Local default root** is `XDG_DOWNLOAD_DIR`; **`GtkColumnView`** for the
   listing, because files want click-to-sort and resizable columns.
6. **Transfers and preview were not rewritten.** The browser drives the existing
   transfer path and `preview.c` through the same entry points the old UI used.

---

## Protocol notes

The opcodes the browser drives: `HTLC_HDR_FILE_LIST`, `HTLC_HDR_FILE_MKDIR`,
`HTLC_HDR_FILE_DELETE`, `HTLC_HDR_FILE_GETINFO`, `HTLC_HDR_FILE_SETINFO`,
`HTLC_HDR_FILE_MOVE`, `HTLC_HDR_FILE_SYMLINK`, plus the transfer requests for
files and, for recursive copies, `HTLC_HDR_FILE_GETFOLDER` and
`HTLC_HDR_FILE_PUTFOLDER` — whose payloads stream over an HTXF subchannel with
`HTXF_TYPE_FOLDER` framing rather than the plain file framing.

**A folder's size field is a child count, not a byte count.** Hotline carries
the count in the file-size field for folder rows, so the size column formats
`"(7 items)"` for a remote folder with a count and an em-dash for a local one
(whose byte size would be a meaningless 4096). The consequence reaches sorting:
every column comparator bubbles folders above files so a 7-item folder is never
ranked against a 7-byte file, because that comparison has no meaning to the
user.

**Filenames are byte-oriented.** The wire name's bytes ride out untouched — a
Classic-Mac name can legitimately contain `/` — and get converted from Mac Roman
for display. When a remote name is used to build an on-disk path it goes through
a sanitiser that replaces path separators, rejects `.` and `..`, and falls back
to a generic name for empty input.

**Drop boxes** are folders you can upload into but not list. The panel
distinguishes them from ordinary listing failures by cross-referencing the
account's access bits: `UPLOAD_FILES` set and `VIEW_DROP_BOXES` unset on a folder
whose listing came back as a task error means almost certainly a drop box, and
the empty state says "folder is upload-only — drop files here to upload" instead
of "can't list this folder".

**There is no copy opcode.** The server-side symlink opcode creates a *hard*
link — shared bytes, where modifying or deleting one path affects the other —
which is not a copy, and the browser doesn't pretend otherwise.

---

## Rejected: eager download for remote drag-out

Remote-source drags briefly took an "eager download" path: at drag-prepare time
the browser kicked off a transfer for every selected file into the configured
download directory and published a URI list pointing at the eventual local
paths, so dropping onto an external app would "just work".

It was removed. The path fired on every drag *start*, including ones the user
immediately cancelled — so picking up a file in the remote panel merely to look
at it downloaded the whole thing unconditionally. That was hit as a real bug in
testing, not predicted in review.

Remote-to-local-panel drops still work, because the drop handler routes them
through the same copy path as everything else and so acts at drop time rather
than at pickup time. Local-source drags still publish a real file list for
external apps. What's missing is remote-to-external-app, and it's missing
because doing it correctly needs promise-style transfers rather than an
approximation that guesses at intent.

---

## Open

- **Per-row transfer progress overlay.** Progress is visible in the tasks
  window; an in-row spinner or bar on the file being transferred is a
  nice-to-have that hasn't been built.
- **Remote drag-out via the file transfer portal.** The promise-style path that
  would replace the rejected eager download above. Until it exists, dragging out
  of the remote panel to the host desktop carries no file list.
- **Remote-to-remote copy.** This is the one open *research* question rather
  than an implementation task. Drag-and-drop between two remote panels currently
  routes to the server's move operation, following the orthodox convention that
  a drag within one volume is a move; the Copy button between two remote panels
  reports that the operation is unsupported and points the user at Move. Whether
  a real server-side copy is achievable — and how the reference server behaves
  if you try — has not been verified. The always-works fallback is
  download-then-upload, at twice the bandwidth.

## Out of scope

- Server-wide file search. A separate UI on top of recursive listing, later.
- Tabbed panels — defer to the multi-connection tabbing work.
- A sync / "what changed since last visit" view.
