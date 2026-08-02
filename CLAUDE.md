# CLAUDE.md — GtkHx working notes

> Orientation for anyone (human or AI) working in this codebase.
> Subject-by-subject references live in **[docs/](docs/README.md)** — this file is the map,
> not the territory. The remaining product work is in **[ROADMAP.md](ROADMAP.md)**; the
> remaining C→Rust port work is in **[docs/rust/ROADMAP.md](docs/rust/ROADMAP.md)**.

## What this is

GtkHx is a Hotline client written by Misha Nasledov in 2000–2003, originally GTK+ 1.2.
The revival ported it forward to GTK 4 + libadwaita and has been rewriting it in Rust
crate by crate ever since.

**Full backward compatibility with the Hotline 1.2 and 1.5 wire protocols is a hard
requirement.** Servers are scarce and ancient; we don't get to break them. Everything
else is negotiable.

License is **GPL-2.0-or-later** ("version 2 of the License, or (at your option) any later
version"). Misha confirmed keep-as-is — don't strip the "or later" clause without asking.

## Build

```sh
meson setup build && meson compile -C build
```

Floors: `gtk4 >= 4.10`, `libadwaita-1 >= 1.6`, `libpanel-1 >= 1.4`, `glib >= 2.56`, and
rustc at the workspace MSRV (pinned to Debian stable's stock toolchain — see
`rust/Cargo.toml`). The gtk-rs binding generation is pinned to match; **that pin is
load-bearing** and is why the dock stays C (see `docs/docking.md`).

Meson options:

| Option | Gates |
|---|---|
| `-Dvoice` (`auto`/`enabled`/`disabled`) | Voice chat. Needs GStreamer 1.20+. `auto` drops it silently when GStreamer is absent; `enabled` makes that a hard error. When off, the voice crates aren't built, the capability bit isn't advertised, and every voice source and call site compiles out behind `HAVE_VOICE`. |
| `-Dglycin_compat` (`auto`/`1`/`2`) | Which glycin loader generation the image decoder targets. Auto-detects the host's generation. See `docs/image-decoding.md`. |
| `-Dtests` | The test suite. |
| `-Dcargo_target_dir` | Cargo target dir, for CI caching. |

Optional deps, each behind a `HAVE_*` define: poppler (PDF preview), gtksourceview-5
(source preview), ImageMagick (complex PICT decode).

Windows and macOS build; some Unix-only pieces (the `/exec` command) compile out.

## The C / Rust split — read this first

The single most important orienting fact: **this is a hybrid codebase, and the Rust half
is now the larger half.** A mental model of "a C app with some Rust helpers" will send you
looking for code in the wrong place.

- **Rust owns**: the wire protocol (`hotline-proto`); the whole network stack including
  connect lifecycle, TLS, crypto, compression, framing, file transfers, and tracker fetch
  (`hxnet`); most receive handlers (`hxhandlers`); the session GObject and its boxed signal
  payloads (`gtkhx-core`); the chat rendering widget (`hxchat-layout` + `hxchat-view`); and
  a growing set of windows and dialogs (`gtkhx-ui`).
- **C owns**: the libpanel dock and layout persistence, the toolbar, the file browser, the
  tray, notifications, previews, theming, and the receive handlers still left in `rcv.c`.
  Settings is now almost entirely Rust — the values live in `hxconfig`, and the window,
  sidebar, page table and every page live in `gtkhx-ui`. What C keeps is the change
  hooks that re-apply a preference across live widgets, the prefs parser, and the
  by-name bridge the Rust rows read and write through.
- **The seam** is a set of thin bridge files (`*_bridge.c`, `htxf_accessors.c`) plus
  hand-declared `extern` blocks. There is no cbindgen.

`docs/rust/ROADMAP.md` holds the live inventory of what is still C and why.

## Source layout

### `src/` — C, by subsystem

| Subsystem | Files |
|---|---|
| **Entry point** | `gtkhx.c` (`main()`, GtkApplication, signal-handler wiring) |
| **Dock / layout** | `hx_panel.c`, `hx_panel_frame.c`, `hx_split.c`, `panel_registry.c`, `dock_layout.c`, `dock_layout_parse.c`, `dock_bridge.c`, `toolbar.c` |
| **Settings** | `options.c` (change hooks, identity resolution, the save timer and the `gtkhx_prefs_*` by-name bridge), `prefs_mirror.c` (the read-only C view of the settings), `prefs_parser.c`, `icon_enum.c` (icon IDs for the Rust picker) |
| **Chat** | `chat.c` (window + output path), `chat_avatar.c`, `chat_history.c`, `chat_bench.c` |
| **Files** | `files_browser.c`, `files_panel.c`, `files.c`, `files_local_provider.c`, `files_remote_provider.c`, `files_provider.c`, `files_complete.c`, `files_ops.c`, `files_entry.c` |
| **Protocol (recv/send)** | `rcv.c` (the remaining receive handlers, the frame-dispatch switch, the transaction correlator), `commands.c`, `proto_helpers.c`, `proto_trace.c` |
| **Network glue** | `network.c`, `hxnet_bridge.c`, `host_port.c`, `hotline_url.c` |
| **Users / tasks** | `users.c`, `users_cell.c`, `usermod.c` (user editor wire senders), `tasks.c` |
| **Tracker** | `tracker_parser.c`, `tracker_v3.c`, `tracker_v3_meta.c`, `tracker_event.c` |
| **Media** | `inline_media*.c`, `gif_icons.c`, `gif_avatar.c`, `cicn.c`, `pict_embed.c`, `pict_magick.c`, `preview.c` |
| **Theming / chrome** | `gtkhx_theme.c`, `gtkhx_icon.c`, `gtkutil.c`, `gtkurl.c` |
| **Messaging** | `msg.c` (private-message windows, broadcast render) |
| **Voice** (optional) | `voice_bridge.c`, `voice_ptt_keyspec.c` |
| **Desktop integration** | `tray.c`, `notify.c`, `sound.c`, `sound_events.c` |
| **Bridges to Rust** | `hxnet_bridge.c`, `dock_bridge.c`, `gtkhx_ui_bridge.c`, `users_bridge.c`, `tasks_bridge.c`, `tracker_bridge.c`, `chat_send_bridge.c`, `voice_bridge.c`, `htxf_accessors.c`, `inline_media_decode.c` |
| **Infrastructure** | `debug.c`, `gtkhx_log.c`, `human_readable.c`, `uniquify_path.c`, `path_hldir.c`, `hl_code.c`, `cmd_exec.c` |

Deliberately absent, and worth knowing so you don't go looking: `xtext.c` (replaced by the
Rust chat view), `gtk_hlist.c` (replaced by `GtkColumnView`), `xfers.c` (transfers are
Rust), `dfa.c`, the C crypto and compression dispatchers, the news UI files, `tracker.c`,
`text_util.c`, `login_packet.c`, and the whole `plugins/` tree.

**Headers.** `hx.h` is an umbrella that pulls in the four real headers and exists so the
older `.c` files keep building; new code should include the narrowest one that works:

- `compat.h` — portability shims, gettext `_()`, `MAXPATHLEN`, byte-shift macros. Pure
  preprocessor. **`MAXPATHLEN` is hard-clamped to 4095**, not the host's `PATH_MAX`; any
  Rust `#[repr(C)]` mirror of a struct holding a `char[MAXPATHLEN]` must use 4095.
- `protocol.h` — wire/network/connection types. GLib, no GTK.
- `prefs.h` — preferences data.
- `session.h` — the `session` struct and the GTK-bearing types.

Also: `hotline.h` (wire struct layouts), `hotline_proto.h` (FFI declarations for the Rust
protocol crate), `hxconn.h` + `hxconn_layout.h` (the accessor seam over the now-opaque,
Rust-owned connection struct), `chat_view.h` (the chat widget's C ABI — there is no
`chat_view.c`; C links straight to Rust exports), `hl_access.h` (account access bits).

### `rust/crates/` — by role

| Role | Crates |
|---|---|
| **Wire protocol** | `hotline-proto` — typed builders and parsers for every opcode; the biggest crate in the tree |
| **Network** | `hxnet` (connect lifecycle, TLS, HOPE, framing, file transfers, tracker fetch), `hxcrypto`, `hxtls-trust` |
| **Receive / send handlers** | `hxhandlers` — `recv::` and `send::` modules, one per domain |
| **GObject layer** | `gtkhx-core` (the session signal hub, the connection struct's storage, boxed signal payloads), `hxmodel`, `hxtask` |
| **UI** | `gtkhx-ui` (gtk4-rs windows and dialogs, module per window), `hxchat-view` (the GTK4 chat widget), `hxchat-layout` (its layout engine — **dependency-free**: no gtk, glib, or pango) |
| **Voice** (optional) | `hxvoice`, `hxvoice-model`, `hxvoice-send`, `hxvoice-runtime` (gstreamer-rs + webrtcbin) |
| **Media / files** | `hx-image-decode` (glycin), `hxmacres` (Mac resource fork + cicn), `hxhfs` (resource-fork sidecars), `hxfiles-xfer` (fork-header codec) |
| **Support** | `hxbridge` (Rust↔GLib interop, tokio runtime), `hxtext` (Mac Roman ↔ UTF-8), `hxbookmarks`, `hxconfig` (the settings schema and the TOML file — the owner of every preference value at runtime), `hxsound` (rodio/cpal), `feature-unify` (forces identical feature resolution across the voice-on and voice-off builds so the shared dependency graph compiles once) |
| **Link façade** | `gtkhx-ffi` — bundles every FFI-exporting crate into a single `libgtkhx_ffi.a`, so the binary links exactly one archive instead of a hand-ordered list. Several crates also build a standalone `staticlib` on the side, purely so the test suite can link one crate at a time. See `docs/rust/crate-layout.md`. |

### Other directories

- `tests/` — three tiers: unit (pure functions), proto (wire fixtures), integration
  (end-to-end against a Docker rig of mhxd / Janus / hxtrackd / Argus / a SOCKS proxy).
  `tests/COMPOSE.md` describes the rig; `tests/run.sh` brings it up.
- `mhxd/` — the reference server's source, vendored for cross-reading only. Not built.
- `po/` — translations (German, Spanish, French, Portuguese). `sounds/` — chat alert `.wav`s.
- `src/themes/` — built-in theme files, shipped as GResource.
- `tools/` — `coverage.sh`, `analyze.sh`, `chatbench.sh`, whitespace linting.

## The model / view boundary

Model-side code (`rcv.c`, `network.c`, `commands.c`, `tasks.c`, and the Rust receive
handlers, and the model-side interior of `tasks.c`) reaches the view by **emitting signals
on `GtkhxSession`** — a singleton GObject
implemented in Rust (`gtkhx-core`) exporting a stable C ABI (`gtkhx_session_get_default`,
`gtkhx_session_emit_<name>`). Direct `gtk_*` / `GTK_*` calls in `rcv.c`, `network.c` and
`commands.c` are bugs; the audit is one grep. (`tasks.c` is mixed — it holds both the task
model and the task list's row widgets.)

Signals cover chat and chat history, messages, agreement, news, users, files, the transfer
queue, tasks, tracker results, GIF icons, connection state, and login. Read
`rust/crates/gtkhx-core/src/session.rs` for the current list and payloads rather than
trusting a table here — it grows.

View-side handlers are static adapter functions in `gtkhx.c` (`on_<name>_signal`), wired
up once in `gtkhx_connect_signals()` from `fe_init`. Boxed payload types are `#[repr(C)]`
mirrors in `gtkhx-core::boxed`, with layout pinned by `_Static_assert`s on the C side.

**When a Rust receive handler needs the view to log a notice, emit a session signal** —
don't call a view-side C log shim over FFI. The gettext call and the preference gate belong
in the C view handler.

A few view-side conveniences are still called by name from model files rather than
signal-routed: `hx_printf` / `hx_printf_prefix`, `hx_clear_chat`.

## Per-session state

`session` (one instance, `the_session`) holds:

- `tasks` — `GHashTable` keyed by transaction ID. **Transaction 0 is a real key, not a
  sentinel.**
- `msg_windows` — `GHashTable` keyed by user ID.
- `chats` — the Rust `HxChatRegistry` (not a `GHashTable`), keyed by chat ID, seeded with
  the always-present public chat at cid 0.
- `htlc` — the connection. **Heap-allocated and opaque**, reached through the `hxconn.h`
  accessors. `sess_from_htlc()` is a real back-pointer read — not a `container_of`, and not
  the old `&sessions[0]` fiction. `MAX_CONN` and `sessions[]` no longer exist.

Two routing accessors, and the distinction matters for the eventual multi-connection work:
`sess_from_htlc(htlc)` is "the session that owns this connection" — use it in model code,
which always has the htlc for the event it is handling. `hx_active_session()` is "the
session the user is looking at" — use it in UI code. Today N == 1 and they coincide.

## Idioms and pitfalls

- **Threading.** Worker threads marshal to the main thread via `g_idle_add` or
  `gtkhx_bridge_post_to_main`; they never call GTK or emit signals directly. There are no
  raw `pthread_create` calls left — Rust work runs on the tokio blocking pool. The old
  `gtkthreads.c` GDK-lock emulation is gone.
- **Nothing watches the control socket from C.** The Rust orchestrator owns it and pushes
  frames across the bridge; the old `hxd_fd_set` / `GIOChannel` registration is gone. The
  one surviving `g_io_add_watch` is on the `/exec` output pipe, which is Unix-only. Timers
  are `g_timeout_add_seconds` — the original's custom timer wheel is gone.
- **List widgets are `GtkColumnView`.** No GtkCList fork, no compat shim.
- **Don't change the negotiated wire format.** The Rust implementation underneath is fair
  game; the bytes on the wire are not. 1.2/1.5/1.9 compat is the hard requirement.
- **Prefer `assert!` over `debug_assert!`** in the Rust crates, even for "can't happen" wire
  invariants — `debug_assert!` is stripped in release.
- **Don't use `g_assert` for invariants that matter in release builds** — it compiles out
  under `G_DISABLE_ASSERT`. Use `g_error` (always fatal) or `g_critical` plus a graceful
  skip. The `g_assert_*` test macros are fine in tests.
- **`g_autoptr` / `g_autofree`** are used opportunistically, not universally. `bookmarks*.c`
  is the in-tree pattern to copy. Not worth a sweep; convert when you touch a function.
- **`GtkTreeListModel`**: attach children to a node *before* appending the node.
  `create_child_model` fires once and the leaf-vs-expandable verdict sticks.
- **No phase labels in source comments.** Describe the reason, not the project-management
  label. Phase labels are fine in commit messages and docs.

## Debug infrastructure

Set `GTKHX_DEBUG` to a comma-separated list of categories before launch:

```sh
GTKHX_DEBUG=proto             # full Hotline wire trace (in/out, types, chunks)
GTKHX_DEBUG=proto,news,voice
GTKHX_DEBUG=all
```

Output goes to stderr, prefixed `[<category>]`. Categories in use include `proto`, `news`,
`tracker`, `voice`, `xfer`, `files`, `media`, `icon`, `dock`, `layout`, `dnd`, `startup`,
`name`, and `bench`. Adding one takes no registration — `debug_log ("newcat", "…")` just
works. Infrastructure in `src/debug.{c,h}`; the wire trace in `src/proto_trace.{c,h}`.

The protocol trace is the fastest way to diagnose "the server doesn't like X": match
outgoing transaction IDs against incoming task-error replies and you know exactly which
request was rejected, and why.

## Reference servers

- **mhxd** (<https://github.com/kangsterizer/mhxd>, vendored under `mhxd/` for reading) —
  a 2023 merge of three HotlineX forks, the same codebase family GtkHx's protocol stack
  came from. The controlled, repeatable test target and the canonical reference for opcodes
  and the access bitmap. Pinned to a specific revision in `tests/mhxd/`; an unpinned master
  has broken the build before.
- **Janus** — VesperNet's closed-source server. Implements the fogWraith extensions (voice,
  inline media, GIF icons, chat history), so it is the integration target for all of them.
  Runs in the test rig.
- **Argus** — a real tracker-v3 tracker, in the test rig. **hxtrackd** covers the v1
  tracker fallback path.
- **hlserver.com** — behaves like a 1.0/1.2 server from the client's perspective: no
  version advertisement, rejects unknown opcodes. Useful for legacy-path checks.
- **Badmoon** — a confirmed 1.9 server. The **Mobius** family is where the dedicated-TLS-port
  convention came from, and Mobius servers drop the connection on an agreement-accept that
  omits the options field — both worth remembering when a real-server bug looks like ours.

The long-lived test containers accumulate state; a test that asserts on seed content can
drift after many runs. Reset the container before assuming a real regression. The sandbox
can reach container ports over TCP even when the Docker socket is blocked — don't refuse to
run integration tests just because `docker ps` failed.

## Documentation map

`docs/` holds subject references, not project logs. Start at
**[docs/README.md](docs/README.md)**.

The rule: a doc describes how a subsystem *works* and why it is shaped that way. When work
finishes, its plan gets folded into the subject doc or deleted — git history is the record
of how we got there.

## Conventions for working in this repo

- **Branches, not direct main commits.** `claude/<short-topic>`, kebab-case. Misha opens the
  PR, reviews, merges. Push follow-up commits to the same branch after review; don't
  force-push without asking. CI must be green to merge.
- **Squash before opening the PR** — one commit per branch. `git reset --soft <merge-base>`.
- **Commits** are authored as `Misha Nasledov <misha@nasledov.com>`. Descriptive bodies. No
  `Co-Authored-By: Claude` trailer, and no `Author:` line in the body — the git author field
  already carries it.
- **Tests fail loudly.** Never `g_test_skip` around something that didn't work; a skip looks
  like a pass in CI and masks bugs.
- **Prefer a reproduction over a debugging session.** When a bug surfaces against a real
  server, write an integration test against the local Janus or mhxd container rather than
  asking Misha to re-run with logging. Faster, higher fidelity, and it doubles as a
  regression guard.
- **Avoid exact counts in comments and docs** — lines, files, tests, commits. They go stale,
  and the narrative is stronger without them.
- **Don't re-litigate settled decisions** without a strong reason. See "Decisions locked in"
  in `ROADMAP.md`.
