# Files browser rewrite — scoping

Working doc for the post-news-browser project. Goal: replace the
current single-pane GtkHList-over-GtkTreeView file browser
(`src/files.c`, 1604 LOC) with a modern orthodox-file-manager UI
sitting on GTK 4 / libadwaita native widgets.

Status: scoping. Nothing has been written yet.

---

## Current state

`open_files()` opens one window per remote directory (or replaces the
current one, depending on the `file_samewin` pref). Each window has:

- One `GtkHList` (compat shim → GtkTreeView+GtkListStore) listing the
  directory's entries with columns: icon, name, kind, size, modified.
- A toolbar with: Up, Download, Upload, Get Info, Delete, MkDir,
  Preview, Reload.
- An "Up" button that walks the path history back, breadcrumb-style.
- Drag-source on rows (drag-out to the host OS or to another file
  window's drop target).
- Drop-target on the window (drop OS-side files in to upload).
- Double-click row: descend into a folder (open in same window or a
  new one), or trigger download on a file.

The cache layer (`cached_filelist` / `cfl_lookup`) is per-path and
session-wide; multiple windows on the same path share it. Wire
opcodes used: `HTLC_HDR_GETFILELIST`, `HTLC_HDR_DELETEFILE`,
`HTLC_HDR_MAKEDIR`, `HTLC_HDR_GETFILEINFO`, `HTLC_HDR_DOWNLOADFILE`,
`HTLC_HDR_UPLOADFILE`, `HTLC_HDR_RENAMEFILE`, `HTLC_HDR_MOVEFILE`.

The transfer code (`xfers.c`, 859 LOC) and preview window
(`preview.c`, 1161 LOC) are independent of the browser UI and stay
unchanged in this project — same way `news_browser.c` left the
`hx_news15_*` wire helpers alone.

What's degraded today:

- The `gtk_hlist_compat` shim is fine but deprecation-suppressed
  under GTK 4.10+. CLAUDE.md flags this as a Phase 5 follow-up.
- Click-to-sort by column doesn't work (the news / tracker UIs
  recently gained it, but files didn't).
- No way to view two remote directories side-by-side; no way to
  see local destination state at all without alt-tabbing to a
  desktop file manager.
- No multi-select operations.
- The drag-out-to-desktop path (`get_put_data`) is fragile.

---

## What "orthodox file manager" means here

Norton Commander / Midnight Commander / Total Commander lineage:
two file panels side by side, each shows a flat directory listing,
one panel active at a time, operations route between them, keyboard
is first-class. The Hotline adaptation:

- **Left panel** = remote Hotline filesystem. Each row is a
  `hl_filelist_hdr` (the parsed entry the wire returns).
- **Right panel** = local filesystem (GIO `GFile`-backed). Default
  starts in the user's downloads dir; remembered across runs.
- **Active panel** is tracked separately from the GTK focus. Tab
  toggles it. The active panel gets the row keyboard cursor and is
  the source for cross-panel operations.
- **F-key bar at the bottom** binds the classic OFM keys:
  - F3 — View (preview the active-side file using preview.c)
  - F4 — Edit (open with `xdg-open`, only sensible on the local
    side; greyed on remote)
  - F5 — Copy (transfer active → inactive: download if
    remote→local, upload if local→remote, drop-copy if
    local→local; cross-server copies are a non-goal)
  - F6 — Move / Rename (in-panel rename if no panel-switch
    intended, cross-panel move otherwise — split into two keys?
    Norton uses F6 for both with a destination prompt)
  - F7 — MkDir on the active side
  - F8 — Delete on the active side
  - F9 — Sort / view options menu
  - F10 — Quit / close window
- **Path field** above each panel — editable, Enter to jump there;
  matches the Adwaita address-row idiom from the news browser's
  breadcrumb.
- **Status bar** under each panel showing selection summary ("3
  files selected, 42.1 MB").
- **Multi-select**: standard GTK list multi-select via Ctrl-click,
  Shift-click, Insert (classic OFM bind), Ctrl-A. All cross-panel
  operations work on the selection.

The window is one toplevel with one headerbar — the panels share
the chrome rather than each having its own. The headerbar carries
global actions (Refresh both / Sync / "swap panels") and the
hamburger menu. Per-panel chrome is the path field + active-marker.

### Variants worth flagging up front

1. **Local-only-on-right vs. remote-on-both-sides.** The Norton
   tradition is "both panels can show anywhere"; in our case "both
   panels remote" is genuinely useful (browse two server dirs at
   once, drag-copy between them). Proposal: each panel has a
   side-selector (local / remote) at the top, defaulting to L=remote
   R=local. User can flip either.
2. **One window or multiple?** Norton is single-window-per-instance.
   Our current code allows multiple browsers. Proposal: collapse to
   a single browser window (matches news_browser, simpler state).
   `file_samewin` becomes obsolete the same way `news_samewin` did.
3. **Tabbed panels?** Modern Total Commander has tabs per panel.
   Out of scope for v1; revisit alongside the eventual multi-conn
   tabbing (memory: `gtkhx_future_ui.md`).

---

## UI sketch

```
┌─ Files ──────────────────────────────────────────────[ ⟳ ⇄ ☰ ]─┐
│ ┌─ Remote ──────────────┐ │ ┌─ Local ──────────────────────┐  │
│ │ [/Music/Albums      ] │ │ │ [~/Downloads               ] │  │
│ ├───────────────────────┤ │ ├───────────────────────────────┤  │
│ │ ▴ name      size mod  │ │ │ ▴ name        size mod        │  │
│ │ ──────────────────────│ │ │ ──────────────────────────────│  │
│ │ ▸ Albums    —    2024 │ │ │ Album.zip     142 M  Tue      │  │
│ │ ▸ Singles   —    2024 │ │ │ song.flac      31 M  Mon      │  │
│ │   song1.mp3 5.2M Wed  │ │ │ ...                           │  │
│ │ ▶ song2.mp3 4.8M Wed  │ │ │                               │  │
│ │   …                   │ │ │                               │  │
│ ├───────────────────────┤ │ ├───────────────────────────────┤  │
│ │ 1 of 47 selected      │ │ │ 0 of 12 selected              │  │
│ └───────────────────────┘ │ └───────────────────────────────┘  │
│ F3 View  F4 Edit  F5 Copy  F6 Move  F7 MkDir  F8 Del  F10 Quit │
└────────────────────────────────────────────────────────────────┘
```

`▶` marker on `song2.mp3` shows it's the cursor row; the focused
panel border has an accent-color highlight.

---

## Build pieces (none of which exist yet)

1. **`files_panel.[ch]`** — a self-contained panel widget. Holds
   one path, one model (GListStore of file entries), one
   GtkColumnView. Knows nothing about "remote vs local" directly —
   it talks to an abstract `FilesProvider` interface.
2. **`files_provider.[ch]`** — the abstraction. Two implementations:
   - `RemoteFilesProvider`: wraps `hx_list_dir` + the
     `output_file_list` signal flow + `hx_file_delete` / `mkdir` /
     etc.
   - `LocalFilesProvider`: GIO-based. `g_file_enumerate_children`,
     `g_file_delete`, `g_file_make_directory`. Drag-and-drop wraps
     `GFile`.
3. **`files_browser.[ch]`** — the toplevel window. Owns two
   `files_panel`s, the F-key bar, the action plumbing, the focus
   state machine.
4. **`files_ops.[ch]`** — orchestration of cross-panel operations.
   F5 (copy) takes a `FilesProvider *src`, a `FilesProvider *dst`,
   a selection, dispatches to either xfers.c (remote→local /
   local→remote / remote→remote via download-then-upload) or GIO
   copy (local→local). Per-row progress feeds back into the panel's
   row representation.

The xfers code stays untouched — the new panel uses it through the
same callbacks the current files.c uses. Same for preview.c.

The wire helpers (`hx_list_dir`, `hx_file_delete`, `hx_make_dir`,
`hx_file_info`, `hx_put_file`, `hx_file_link`, `hx_file_move`) stay
where they are. files.c shrinks to just these once the UI is gone,
the same way news15.c did in Phase 6.

---

## Phase plan

**Phase 1 — Panel widget, local-only.** Build `files_panel` and
`LocalFilesProvider`. New `Files (2-pane)` menu entry opens a
toplevel with two local panels side by side. Tab to switch, Enter
to descend, F5/F8/F7 do local→local operations via GIO. No remote
involved yet. This gets the navigation / selection / keyboard /
focus state machine right against the simpler backend.

**Phase 2 — Remote provider.** Implement `RemoteFilesProvider`
around the existing `hx_list_dir` flow + `output_file_list` signal.
The provider hands a fresh `GListStore` of entries to the panel each
time it gets a fresh listing. Add per-panel side-selector (L/R).
Now you can have remote on one side, local on the other, and walk
through both.

**Phase 3 — Cross-panel transfers.** F5 invokes xfers.c through a
new `files_ops_copy` wrapper that picks download / upload based on
sides. Progress reflects in the panel's row chrome (an indeterminate
spinner overlay, like the existing tasks window does). Permission
gating via `htlc->access` bits (UPLOAD_FILES, DOWNLOAD_FILES,
DELETE_FILES, CREATE_FOLDERS, etc.) — grey out F-keys when the
account can't do the action.

**Phase 4 — Polish.**
- ✅ F3 → preview window (existing preview.c).
- ✅ F4 → `g_app_info_launch_default_for_uri` on local files.
- ✅ F2 rename — single-select dialog. (Move dialog for
  cross-directory move is deferred — orthodox FM's move-with-
  rename pattern is rarely used now that DnD covers the common
  case.)
- ✅ Multi-select for all destructive ops.
- ✅ DnD between the two panels.
- ✅ Drag-out from LOCAL to host (the remote side stays
  in-app — needs FileTransferPortal plumbing for promise-based
  external drag; deferred to a Phase 4.x sub-phase or skipped).
- ✅ Column sort (Phase 1).
- ✅ Auto-refresh both panels when a transfer finishes.
- ✅ Friendly Kind column for remote rows.

**Deferred to Phase 4.x / Phase 5 polish:**

- Per-panel side selector (L↔R local/remote swap). Needs the
  panel to swap providers mid-life — re-build sort model,
  re-create selection model, re-bind signal handlers. Doable
  but a real chunk of code, and the L=local R=remote default
  covers the 95% case. Two-remote-dirs-at-once is the
  motivating use case, currently workable via the legacy
  files window in a separate session.
- Per-row transfer progress overlay (spinner / progress bar
  on the row mid-transfer). The existing tasks window already
  shows progress; an in-row overlay is nice-to-have rather
  than required.
- Drag-out from REMOTE to host. Needs FileTransferPortal +
  download-on-promise plumbing.
- Move dialog (orthodox F6 cross-dir move). Rename covers
  in-place; cross-dir is download-then-upload right now.

**Known bugs** (see BUGS):

- Focus drift on remote directory change in the right panel.
  populate_from_chunks's row-widget removal during the
  items-changed propagation steals focus before on_navigated
  can grab it back. Workarounds tried so far in commits
  hand-restore via a wants_focus_restore flag, which fixes
  the row-activate path in isolation but the symptom still
  shows on testing. Three followups noted in the bug entry.

**Phase 5 — Retire legacy.** Delete the old `open_files`,
`gfile_list`, `gfl_*`, and supporting UI helpers from files.c.
Drop `file_samewin` from prefs (already-loaded values silently
ignored). files.c shrinks to the wire helpers only.

Phase boundaries match what worked for news_browser: each phase
ends on a runnable binary with both old and new available, the
toolbar entry point switches over in Phase 4, the legacy code path
goes away in Phase 5.

---

## Risks / open questions

1. **Remote→remote on the same server.** Hotline supports this via
   `HTLC_HDR_MOVEFILE` (rename across paths) and a copy-via-link
   opcode. mhxd's behaviour needs verifying before we commit to F5
   doing remote→remote without round-tripping through local. Worst
   case the v1 fallback is "download then upload" — works against
   every server but uses 2× the bandwidth.
2. **Local drop-boxes.** A folder the user has UPLOAD_FILES on but
   not VIEW_DROP_BOXES + DOWNLOAD_FILES on is uploadable-but-not-
   listable. The panel needs a "you don't have read here" state
   rather than an error toast.
3. **Folder transfers** (HL_ACCESS_UPLOAD_FOLDERS /
   DOWNLOAD_FOLDERS): recursive copy. xfers.c can do these but the
   UI side needs to surface the recursive nature in the selection
   summary (count files transitively, not just direct children).
4. **Keyboard shortcuts on macOS / Wayland.** F3-F10 work but some
   compositors steal F-keys for media controls. Provide
   Ctrl-equivalents (Ctrl-V = View, Ctrl-C/M = Copy/Move, etc.) as
   secondary bindings.
5. **GtkColumnView vs GtkListView.** ColumnView gives us click-to-
   sort and resizable columns for free; ListView is leaner. Pick
   ColumnView — files want columns, news didn't.
6. **Hotline filename encoding.** The wire is byte-oriented; files
   currently get `gtkhx_text_to_utf8`-ed for display. Same path
   here.
7. **HTLC_DATA_FILESIZE on folders.** Folders carry a child-count
   in the size field, not a byte count. The panel needs to format
   "—" or "(7 items)" for folder rows rather than "0 B".

---

## Out of scope

- File search across an entire remote server. Add later as a
  separate UI on top of `hx_list_dir` recursion.
- Re-implementing xfers.c (already done in Phase 5 of the main
  roadmap).
- Re-implementing preview.c.
- Tabbed panels — defer to the multi-conn tabbing project.
- A cloud-mirror / sync view ("show me what's changed on the
  server since last visit"). Cool idea, separate effort.

---

## Answers locked in (2026-05-13)

1. **Default sides**: left = local, right = remote.
2. **Single window**, matching news_browser. `file_samewin` becomes
   obsolete during Phase 5 cleanup.
3. **Buttons-first**, with keyboard shortcuts as secondary.
   Ctrl-equivalents for the classic F-keys (Ctrl-D delete,
   Ctrl-N new folder, Ctrl-R reload, Enter descend, Backspace up).
4. **Local default root**: `XDG_DOWNLOAD_DIR`.
5. **List widget**: `GtkColumnView` for sortable / resizable
   columns.
