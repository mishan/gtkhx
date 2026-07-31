# Files browser

GtkHx's file browser is an orthodox file manager: two panels side by side, one
active at a time, operations routing between them. It replaced a single-pane
window-per-directory browser and is the only files UI — the legacy path
(`open_files`, the per-path list cache, the `file_samewin` preference) is gone,
leaving `src/files.c` as wire senders plus the Get Info dialog plumbing.

The implementation: `src/files_browser.c` (toplevel window, shared chrome,
drag-and-drop, keyboard, active-panel state), `src/files_panel.c` (one panel —
path row, `GtkColumnView`, status footer), `src/files_ops.c` (cross-panel copy
/ move orchestration), and the two providers, `src/files_local_provider.c` (GIO)
and `src/files_remote_provider.c` (Hotline).

---

## What "orthodox file manager" means for a Hotline client

Norton Commander / Midnight Commander / Total Commander lineage: two flat
directory listings, one panel active, keyboard first-class, and every operation
implicitly addressed as "from the active panel to the other one". The Hotline
adaptation is that one side is a remote server whose "copy" is a file transfer.

```
┌─ Files ──────────────────────────────────────[ ⟳ ✎ 🗑 ☰ ]─┐
│ ┌─ Local ▾ ─────────────┐   ┌─ Remote ▾ ──────────────┐  │
│ │ [~/Downloads        ] │   │ [/Music/Albums        ] │  │
│ ├───────────────────────┤   ├─────────────────────────┤  │
│ │ ▴ name      size mod  │ ⟩ │ ▴ name      size   mod  │  │
│ │ ──────────────────────│ ⟨ │ ────────────────────────│  │
│ │ Album.zip  142 M  Tue │ ⇄ │ ▸ Albums  (7 items) 2024│  │
│ │ song.flac   31 M  Mon │   │ ▸ Singles (3 items) 2024│  │
│ │ …                     │   │ ▶ song2.mp3  4.8M   Wed │  │
│ ├───────────────────────┤   ├─────────────────────────┤  │
│ │ 0 of 12 selected      │   │ 1 of 47 selected        │  │
│ └───────────────────────┘   └─────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

The active panel carries an accent highlight; `▶` marks the cursor row. The
buttons between the panels are the cross-panel Copy / Move directions.

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

One toplevel, one headerbar, one button bar — the panels share the chrome rather
than each carrying their own. Single-panel actions (refresh, new folder,
preview, get info, rename, delete) live in the shared bar; the cross-panel Copy
and Move directions live in the column of buttons between the two panels, where
the direction is visually obvious. Per-panel chrome is just the path row and the
side selector.

### Decisions

1. **Left panel is local, right panel is remote** by default — the browser
   constructs a local provider for the left panel and a remote provider for the
   right. (An earlier draft of this document said the opposite in one section
   and this in another; the code is the arbiter.)
2. **Each panel has a side selector** — a two-item dropdown in the path row
   ("Local" / "Remote") that swaps the panel's provider, so both panels can show
   remote directories at once. The dropdown tracks provider identity rather than
   driving it: the selection is set from the actual provider after a swap.
3. **One browser window**, matching the news browser. `file_samewin` is retired.
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
