# CLAUDE.md — GtkHx working notes

> Working notes for AI assistants helping with this codebase.
> The phased plan and locked-in decisions live in **[ROADMAP.md](ROADMAP.md)** — read that first.

## What this is

GtkHx is a Hotline client written by Misha Nasledov in 2000–2003. Original UI is GTK+ 1.2.
The revival project is porting it forward — GTK 2 → 3 → 4 — and modernizing the C, build
system, and crypto along the way. **Full backward compatibility with the Hotline 1.2 and
1.5 wire protocols is a hard requirement.** Servers are scarce and ancient; we don't get
to break them.

## Source layout (~36k LOC C, 83 files in `src/`)

The big rocks, by line count:

| File                    | LOC  | Role                                                          |
|-------------------------|------|---------------------------------------------------------------|
| `src/xtext.c/.h`        | 4500 | **Custom widget** — HexChat's modern xtext fork (vendored Phase 2). |
| `src/dfa.c`             | 2550 | Regex/pattern matching engine.                                |
| `src/options.c`         | ~1900| Settings (AdwPreferencesDialog) + GKeyFile persistence.       |
| `src/rcv.c`             | ~1700| Hotline protocol receive path.                                |
| `src/chat.c`            | ~1500| Chat window UI (xtext output, GtkTextView input).             |
| `src/files.c`           | ~1500| File browser UI (gtk_hlist_compat).                           |
| `src/news15.c`          | ~1300| Threaded news (1.5 protocol).                                 |
| `src/network.c`         | ~1300| Connection / pthread worker, hlwrite, ping keepalive.         |
| `src/users.c`           | ~1000| User list UI + right-click popup.                             |
| `src/connect.c`         |  ~900| Connect dialog (AdwDialog) + bookmark management.             |
| `src/gtkhx.c`           |  ~900| `main()`, GIOChannel-based fd plumbing, GtkApplication init.  |
| `src/gtk_hlist_compat.c`|  ~800| Shim: GtkHList API over GtkTreeView+GtkListStore.             |
| `src/commands.c`        |  ~~~ | Hotline protocol send path (paired with `rcv.c`).             |
| `src/cipher.c`          |  ~~~ | Per-connection cipher (Blowfish/RC4); HOPE negotiation.       |
| `src/compress.c`        |  ~~~ | zlib compression layer; HOPE negotiation.                     |
| `src/hmac.c`            |  ~~~ | HMAC-MD5 / HMAC-SHA / HMAC-HAVAL.                             |
| `src/md5.c` `src/sha.c` `src/haval.c` | ~~~ | Hash primitives (Phase 5 follow-up: replace with GLib/Nettle). |
| `src/plugin.c`          |  ~~~ | dlopen plugin loader. **Compiled out** (`USE_PLUGIN` undef).  |
| `src/gtkthreads.c`      |  ~~~ | GRecMutex + custom poll wrapper for worker↔main serialization.|
| `src/debug.c/.h`        |  ~~~ | Categorised runtime logger (`GTKHX_DEBUG=cat1,cat2`).         |
| `src/proto_trace.c/.h`  |  ~~~ | Hotline wire-protocol trace (debug category `proto`).         |
| `src/hl_access.h`       |  ~~~ | Account-access-bit constants matching mhxd's `hl_access_bits`.|

`src/hx.h` (655 lines) is the kitchen-sink header: session struct, output_functions, most
typedefs. `src/hotline.h` is the wire-protocol constants/structs.

Other top-level dirs:

- `plugins/sample/` — example plugin. Build-disabled (`USE_PLUGIN` undef).
- `plugins/eliza/` — toy ELIZA chatbot plugin. Build-disabled.
- `po/` — translations. French only.
- `sounds/` — `.wav` files for chat alerts (was `.aiff`, converted Phase 5 for libcanberra
  compatibility).
- `mhxd/` — full mhxd source vendored locally for cross-reading
  (`hl_access_bits` struct, opcode tables, ChangeLog). Not built; reference only.

(Old `debian/` packaging, `gtkhx.spec`, `intl/`, `macros/` were removed in Phase 0–1.
Re-add fresh packaging when ready to ship.)

## Build status

**Phases 1, 2, 3, and 4 are complete. Phase 5 (post-port modernization) is active.**
`meson.build` pins `gtk4 >= 4.6` and `libadwaita-1 >= 1.6`. `meson setup build &&
meson compile -C build` produces a working binary.

The custom GtkCList fork is gone (replaced by `gtk_hlist_compat` over GtkTreeView+
GtkListStore — five consumers, ~392 sites); xtext is HexChat's modern fork. The
GtkApplication / activate plumbing in `gtkhx.c` drives all window construction.

What's runnable and reasonably polished on this branch:

- Launches under GTK 4 / libadwaita on Wayland with light/dark/system theme tracking
  via AdwStyleManager.
- Every user-facing window uses `AdwHeaderBar` chrome consistently: toolbar, chat,
  private chat, private message, news, news15, files, users, tasks, tracker, preview,
  agreement, user editor, about. Settings is `AdwPreferencesDialog`; Connect, Open
  User, broadcast, and confirmation prompts are `AdwDialog` / `AdwAlertDialog`.
- Toolbar uses `AdwSplitButton` for Connect-with-bookmark, `AdwBanner` for connection-
  loss notice with Reconnect, `AdwToastOverlay` for transient feedback. Hamburger menu
  via `GMenu` + `GAction` on the application.
- Tracker has a `GtkSearchEntry` and action buttons in the headerbar.
- Settings icon picker is a `GtkFlowBox` grid of 56 px GtkPicture-rendered icons (was
  a 18-px-row GtkHList).
- Hotline protocol layer (`rcv.c` / `commands.c` / `hotline.h` / `cipher.c` /
  `compress.c`) is unchanged — wire-format compat with 1.2/1.5/1.9 servers preserved.
- Chat / private-message text is sanitised through `gtkhx_text_to_utf8` (Mac Roman →
  UTF-8 with U+FFFD fallback) before reaching xtext/Pango.
- Sound playback is in-process via GSound; no fork+exec of an external player.

Phase 5 protocol-aware work landed:

- `HTLC_HDR_PING` keepalive every 60 s while connected, gated on `htlc->version >= 150`
  so 1.0/1.2 servers don't error-spam our toasts.
- Post-login state machine waits for `HTLS_HDR_USER_SELFINFO` before firing
  `USER_GETLIST`, with a 2 s fallback timer for old servers that don't send SELFINFO.
- `hl_access.h` decodes the access bitmap; `news.c` skips auto-fetch when
  `HL_ACCESS_READ_NEWS` is unset; users.c hides Kick/Ban menu and toolbar buttons when
  `HL_ACCESS_DISCONNECT_USERS` is unset; toolbar greys out News / Post / News (1.5+)
  buttons by version + access bits.

What's degraded and remaining:

- **Selection auto-scroll while dragging**: scrollup/down timers read
  `xtext->select_end_y` (kept live by the motion controller) rather than the live
  device position; GTK 4 has no synchronous "where is the pointer" accessor.
- **Window position restoration**: `gtk_window_get_position` is gone and Wayland
  gives clients no portable way to set absolute position. Size restores from prefs;
  position only restores when the compositor cooperates.
- **`MAX_CONN > 1`**: still a half-built abstraction. The plan is tabbed UI for
  multi-conn (see memory `gtkhx_future_ui.md`).
- **CSS-node-insert-after warnings**: occasional `gtk_css_node_insert_after` criticals
  during widget construction. Most call sites have been audited; remaining cases are
  pre-existing GTK 4 noise.

## Model / view boundary (GtkhxSession signals)

Model-side files (`rcv.c`, `network.c`, `commands.c`, `tasks.c`
interior, `banner.c`, `xfers.c`) reach the view by emitting signals
on `GtkhxSession` — a singleton GObject created lazily by
`gtkhx_session_get_default()`. Direct GTK calls (`gtk_*` / `GTK_*`)
in those files are bugs — Phase 2 cleared them out, the audit is
one grep.

The signal taxonomy mirrors the old `hx_output` vtable that Phase 3
replaced:

| Signal                  | Payload                                      |
|-------------------------|----------------------------------------------|
| `chat`                  | htlc, cid, body, len                         |
| `chat-subject`          | htlc, cid, subj                              |
| `chat-invitation`       | htlc, cid, inviter-name                      |
| `msg`                   | sender-name, uid, body                       |
| `agreement`             | session, agreement-string, len               |
| `news-file`             | htlc, news, len                              |
| `news-post`             | htlc, news, len                              |
| `news-folder`           | gfnews                                       |
| `news-catalog`          | gcnews                                       |
| `news-thread`           | post                                         |
| `user-create`           | htlc, chat, user, nam, icon, color           |
| `user-delete`           | htlc, chat, user                             |
| `user-change`           | htlc, chat, user, NEW nam/icon/color         |
| `users-clear`           | htlc, chat                                   |
| `user-info`             | uid, nam, info, len                          |
| `file-info`             | path, name, creator, type, ...               |
| `file-list`             | cfl, fh, data                                |
| `file-update`           | session, htxf                                |
| `xfer-queue`            | session, htxf                                |
| `tracker-server-create` | addr (s_addr), port, nusers, nam, desc, total|
| `task-update`           | session, task                                |

Model-side emitters live in `gtkhx_session.{c,h}` —
`gtkhx_session_emit_<name>(self, args...)` is a one-line wrapper
over `g_signal_emit_by_name`. View-side handlers are static
adapter functions in `gtkhx.c` (`on_<name>_signal`) that bridge the
GObject marshaller signature to the legacy `output_*` /
`user_create` / etc. functions in `chat.c` / `users.c` / `news*.c` /
`tasks.c`. The connect calls all live in `gtkhx_connect_signals()`,
fired once from `fe_init` at startup.

The Phase 2 / Phase 3 cleanup also dropped a clutch of dead vtable
entries (`clear`, `user_list`, `tracker_clear`) whose implementations
were called directly by name elsewhere and never flowed through the
dispatch. Some view-side convenience functions remain called by
name from model files: `hx_printf` / `hx_printf_prefix` (log a line
to chat output), `hx_clear_chat`, `tracker_clear`. They aren't
signal-routed today.

Worker threads marshal to main via `g_idle_add` (or `gtkhx_post_to_main`,
xfers.c's wrapper that takes a refcount), never call GTK or emit
signals directly. The idle callback runs on the main thread; only
there does it call view functions or emit signals. Workers verified
clean post-Phase-3: banner.c HTXF worker, xfers.c progress updates,
preview.c async parses, network.c conn worker, tracker.c list worker
all marshal correctly. `hx_printf` / `hx_printf_prefix` (which now
emit through GtkhxSession via `gtkhx_log.c`) are called only from
main-thread paths.

## Per-session collections (Phase 1 of MVC cleanup)

Every per-session collection on `struct _session` is a `GHashTable`
since Phase 1, replacing the intrusive doubly-linked-list patterns
the original code used. The shape is uniform:
`g_direct_hash + g_direct_equal + NULL key destroy + g_free-ish
value destroy`. The lookup APIs (`task_with_trans`,
`msgwin_with_uid`, `chat_with_cid`, `gchat_with_cid`,
`hx_user_with_uid`) are now O(1) wrappers; iteration uses
`GHashTableIter`. The hashtables and their factories live in:

| Session field    | Keyed on    | Factory           | Test                             |
|------------------|-------------|-------------------|----------------------------------|
| `tasks`          | `u32 trans` | `tasks_init`      | `tests/unit/test_task_hash.c`    |
| `msg_windows`    | `u16 uid`   | `msg_windows_init`| `tests/unit/test_msgwin_hash.c`  |
| `chats`          | `u32 cid`   | `chats_init`      | `tests/unit/test_chat_hash.c`    |
| `gchats`         | `u32 cid`   | `gchats_init`     | `tests/unit/test_gchat_hash.c`   |
| per-chat `users` | `u16 uid`   | `chat_new` seeds  | `tests/unit/test_hx_user_hash.c` |

`chats_init` additionally seeds the always-present public chat at
`cid=0` — `chat_with_cid(sess, 0)` is never NULL while the table
exists. Same convention for `gchats`. `gfile_list` is a plain
`GList` (small N + the legacy `file_samewin=false` pref allows
duplicate paths, so a hashtable can't represent it cleanly).

## Idioms and pitfalls specific to this codebase

- **Multi-connection scaffolding is a lie.** `hx.h` has `MAX_CONN`/`sessions[]`, but
  `sess_from_htlc()` literally returns `&sessions[0]`. Don't propagate the abstraction
  during follow-up work — collapse to explicit single-session. Real multi-conn lands
  in Phase 5 with a tabbed UI (see memory `gtkhx_future_ui.md`).
- **Socket watches in `gtkhx.c`.** `hxd_fd_set`/`hxd_fd_clr` are now thin wrappers
  around `g_io_add_watch` on a `GIOChannel`. The custom-timer-wheel from the original
  is gone; `g_timeout_add_seconds` is used everywhere now (e.g. ping keepalive,
  post-login fallback).
- **Threading.** `gtkthreads.c` uses a `GRecMutex` plus a custom `GMainContext` poll
  wrapper that releases the lock during `poll()` and re-acquires it after. Worker
  threads (network.c, xfers.c) call `gtk_threads_enter()` / `gtk_threads_leave()`
  brackets around any GTK widget access — same single-mutex semantics the GDK lock
  used to provide, just on a non-deprecated foundation. Per-window UI dispatch from
  workers prefers `g_idle_add` (see preview.c) so the worker doesn't hold the lock
  during slow GTK operations.
- **`gtk_hlist_compat`.** Five consumers (`tracker.c`, `news15.c`, `options.c`,
  `users.c`, `files.c`) still use the GtkHList API. The compat shim wraps
  GtkTreeView+GtkListStore. Eventually we want `GtkColumnView` per consumer and the
  shim deleted; for now the shim works and isn't on fire.
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

## Coverage

`tools/coverage.sh` builds with `-Db_coverage=true`, runs the test suite,
and emits an HTML report at `coverage/index.html` via `gcovr` (or `lcov`
fallback). Default is all tiers including Tier 3 against the Docker matrix;
`--quick` skips Tier 3. Exclusions for vendored / soon-deleted code
(`xtext.c`, `dfa.c`, `rand.c`) live in `.gcovr.cfg`. Use it to pick the
heaviest-uncovered file when planning what to test next. Full notes in
`docs/coverage.md`.

## Debug infrastructure

Set `GTKHX_DEBUG` to a comma-separated list of categories before launch:

```sh
GTKHX_DEBUG=proto             # full Hotline wire trace (in/out, types, chunks)
GTKHX_DEBUG=proto,news,login  # several at once
GTKHX_DEBUG=all               # everything
```

Output goes to stderr, prefixed `[<category>]`. See `src/debug.{c,h}` for the
infrastructure and `src/proto_trace.{c,h}` for the protocol trace (the first and
biggest consumer). Existing categories: `proto`, `news`, `login`, `msg`. Add new
ones inline — `debug_log("xfer", "starting transfer %u", htxf->ref)` just works,
no registration needed.

The protocol trace is invaluable for diagnosing "server doesn't like X" bugs —
matching outgoing trans IDs against incoming `HTLS_HDR_TASK flag=1` task-error
replies tells you exactly which client request the server rejected and why.

## Reference servers and protocol versions

Several live and local servers are useful for compatibility testing:

- **mhxd** (<https://github.com/kangsterizer/mhxd>, also vendored under `mhxd/`
  for offline cross-reading) — 2023 merge of three forks of HotlineX, same
  codebase family that GtkHx's protocol stack came from. Builds `hxd` server,
  `hxtrackd` tracker, console `hx` client, `ghx` GTK client. Default port 5500.
  GPL-2.0-or-later. Use it as the controlled / repeatable test target and as
  the canonical reference for opcodes, the access bitmap struct, and the
  ChangeLog of post-original protocol additions.
- **hlserver.com** — running a modern-but-not-original Hotline server. Doesn't
  advertise `HTLS_DATA_VERSION` and rejects unknown opcodes (e.g.
  `HTLC_HDR_PING`) with task errors, so behaves like a 1.0/1.2 server from the
  client's perspective. Useful for "does our 1.0/1.2 path still work" checks.
- **Badmoon** — confirmed Hotline 1.9 server (`HTLS_DATA_VERSION=0x00be=190`).
  Speaks `HTLC_HDR_PING`. The first concrete 1.9-server data point we have.

Hotline 1.9 protocol exists in the wild — TLS-over-Hotline has been seen in
third-party screenshots. See ROADMAP Phase ∞ and the long-form notes in memory
(`gtkhx_protocol_19.md`).

## Conventions for working in this repo

- **Branches, not direct main commits.** All changes go on a feature branch named
  `claude/<short-topic>` (kebab-case). Don't commit to `main` directly. Misha opens a
  pull request from the branch, reviews, and merges. If you need to make follow-up
  changes after review feedback, push more commits to the same branch — don't squash
  / force-push without asking. CI (`.github/workflows/tests.yml`) runs on every push
  and PR; a green build is the merge gate.
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
