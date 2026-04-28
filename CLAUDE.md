# CLAUDE.md — GtkHx working notes

> Working notes for AI assistants helping with this codebase.
> The phased plan and locked-in decisions live in **[ROADMAP.md](ROADMAP.md)** — read that first.

## What this is

GtkHx is a Hotline client written by Misha Nasledov in 2000–2003. Original UI is GTK+ 1.2.
The revival project is porting it forward — GTK 2 → 3 → 4 — and modernizing the C, build
system, and crypto along the way. **Full backward compatibility with the Hotline 1.2 and
1.5 wire protocols is a hard requirement.** Servers are scarce and ancient; we don't get
to break them.

## Source layout (~42k LOC C, 88 files in `src/`)

The big rocks, by line count:

| File                    | LOC  | Role                                                          |
|-------------------------|------|---------------------------------------------------------------|
| `src/gtk_hlist.c/.h`    | 8149 | **Custom widget** — fork of GtkCList. 5 consumers in-tree.    |
| `src/xtext.c/.h`        | 3526 | **Custom widget** — fork of XChat 1.8.5 text widget. Phase 2 swaps in HexChat's modern xtext. |
| `src/dfa.c`             | 2550 | Regex/pattern matching engine.                                |
| `src/options.c`         | 1816 | Preferences dialog + persistence.                             |
| `src/rcv.c`             | 1623 | Hotline protocol receive path.                                |
| `src/chat.c`            | 1509 | Chat window UI.                                               |
| `src/files.c`           | 1462 | File browser UI.                                              |
| `src/news15.c`          | 1269 | Threaded news (1.5 protocol).                                 |
| `src/network.c`         | 1196 | Connection / pthread worker.                                  |
| `src/users.c`           |  995 | User list UI.                                                 |
| `src/connect.c`         |  903 | Connection dialog + setup.                                    |
| `src/gtkhx.c`           |  892 | `main()`, custom timer/fd plumbing, GTK init.                 |
| `src/commands.c`        |  ~~~ | Hotline protocol send path (paired with `rcv.c`).             |
| `src/cipher.c`          |  ~~~ | Per-connection cipher (Blowfish/RC4); HOPE negotiation.       |
| `src/compress.c`        |  ~~~ | zlib compression layer; HOPE negotiation.                     |
| `src/hmac.c`            |  ~~~ | HMAC-MD5 / HMAC-SHA / HMAC-HAVAL.                             |
| `src/md5.c` `src/sha.c` `src/haval.c` | ~~~ | Hash primitives (to be replaced by GLib/Nettle in Phase 1). |
| `src/plugin.c`          |  ~~~ | dlopen plugin loader. **Slated for removal** — see ROADMAP.   |
| `src/gtkthreads.c`      |  ~~~ | Custom pthread↔GTK plumbing (pipe + cond + `gdk_input_add`).  |

`src/hx.h` (655 lines) is the kitchen-sink header: session struct, output_functions, most
typedefs. `src/hotline.h` is the wire-protocol constants/structs.

Other top-level dirs:

- `plugins/sample/` — example plugin. Will be deleted.
- `plugins/eliza/` — toy ELIZA chatbot plugin. Keep if it amuses Misha.
- `intl/` — vendored GNU gettext runtime (~30 files). Phase 1 will delete this in favor
  of the system gettext.
- `po/` — translations. Currently has French only.
- `macros/` — autoconf macros. Phase 1 will delete (Meson handles this differently).
- `sounds/` — `.wav` files for chat alerts.

(Old `debian/` packaging and `gtkhx.spec` were removed in Phase 0 — re-add fresh
packaging when there's a buildable binary again.)

## Build status

**Phase 1 is complete; Phase 2 (GTK 2 port) in progress.** `meson.build` already pins
`gtk+-2.0 >= 2.24`, but the source is still GTK 1.2 idioms. `meson setup build && meson
compile -C build` runs and produces a punch list of GTK-2-incompatible API uses:

- `src/gtk_hlist.c` (~140 errors) — in-tree GtkCList fork. **Don't fix it; replace it.**
  Phase 2.7 builds a `gtk_hlist_compat.[ch]` shim over GtkTreeView+GtkListStore that keeps
  the existing `gtk_hlist_*` API surface, then drops the fork. The 5 consumers
  (`tracker.c`, `news15.c`, `options.c`, `users.c`, `files.c`, ~392 sites) keep compiling
  unchanged.
- `src/xtext.c` (~6 errors) — in-tree XChat 1.8.5 fork. **Don't fix it; replace it.**
  Phase 2.6 vendors HexChat's xtext.
- The rest is mostly mechanical: 165 `gtk_signal_connect` (Phase 2.2), 27 `GtkStyle->font`
  (Phase 2.3), GtkText → GtkTextView in `about.c`/`news15.c` (Phase 2.4), GtkPixmap → GtkImage
  and GtkOptionMenu → GtkComboBox (Phase 2.5).

`gtkthreads.c` and `gtkhx.c` need a threading first-cut (Phase 2.9) for `g_thread_init` /
`gdk_threads_enter`. Then it should run.

## Idioms and pitfalls specific to this codebase

- **Multi-connection scaffolding is a lie.** `hx.h` has `MAX_CONN`/`sessions[]`, but
  `sess_from_htlc()` literally returns `&sessions[0]`. Don't propagate the abstraction
  during ports — collapse to explicit single-session, then build real multi-conn against
  the modernized codebase in Phase 5.
- **Custom timer/fd plumbing in `gtkhx.c`.** Uses `hxd_fd_set`/`hxd_fd_clr` for socket
  watches and a custom timer wheel rather than `g_timeout_add` / `g_io_add_watch` / a
  `GMainContext`. Will be replaced when the GTK port lands.
- **Threading.** `gtkthreads.c` uses a pipe + `pthread_cond_t` + `gdk_input_add` to
  marshal results from worker threads back to the GTK thread. The GTK 2 port can keep
  this with `gdk_input_add` → `g_io_add_watch`; Phase 4 (GTK 4) should move to
  `g_main_context_invoke` / `GTask`.
- **`gtk_hlist` consumers** (5 files): `tracker.c`, `news15.c`, `options.c`, `users.c`,
  `files.c`. ~392 use sites total. The plan is a `gtk_hlist_compat` shim over
  GtkTreeView/GtkListStore, then migrate consumers one at a time, then delete the shim.
- **HOPE handshake.** `cipher.c` and `compress.c` negotiate the optional HOPE
  encryption/compression extension during connection setup. Don't change the negotiated
  format — only the implementation underneath. `hmac.c` checks for `"HMAC-HAVAL"` MAC at
  lines 65, 111. Verify whether any extant Hotline server still advertises HAVAL before
  deleting `haval.[ch]`.
- **Crypto is moving to Nettle + GLib hashes** (see ROADMAP). Don't introduce new
  dependencies on `md5.c`/`sha.c`/`haval.c`/`rand.c` — those files are slated for
  replacement. `cipher.c` already has an `#ifdef OPENSSL` branch via `cipher_openssl.h`;
  Nettle becomes the new primary path.
- **License: GPL-2.0-or-later** ("version 2 of the License, or (at your option) any
  later version" header text). Misha confirmed keep-as-is. Don't strip the "or later"
  clause without explicit confirmation.

## Reference server

For testing once Phase 2 produces a binary: **mhxd**
(<https://github.com/kangsterizer/mhxd>) — 2023 merge of three forks of HotlineX, same
codebase family that GtkHx's protocol stack came from. Builds `hxd` server, `hxtrackd`
tracker, console `hx` client, and a `ghx` GTK client. Default port 5500. GPL-2.0-or-later.

## Conventions for working in this repo

- **Commits.** Author identity is `Misha Nasledov <misha@nasledov.com>` (matches the
  CVS-import commit). One logical change per commit, descriptive bodies. No `Co-Authored-By:
  Claude` trailers unless Misha asks.
- **Don't re-litigate roadmap decisions** without a strong reason. The locked-in choices
  (Meson, Nettle+GLib crypto, vendor HexChat's xtext, drop plugin API, GPL-2.0-or-later,
  single-conn during ports) were made deliberately. ROADMAP.md is the source of truth.
- **Don't break Hotline 1.2/1.5 wire compat.** Modern transport security is a Phase ∞
  effort that requires inventing a new protocol layer AND server-side cooperation
  (mhxd is the natural target). It is explicitly out of scope for the GTK ports.

## Repo origin

This is a CVS import (commit `4d96dd5`, "initial commit from CVS"). The `Phase 0` cleanup
removed `CVS/` metadata directories, `.cvsignore` files, emacs scratch files, and
regenerable autotools artifacts. Earlier commits in `git log` show the cleanup steps as
discrete logical units in case anything needs to be reversed.
