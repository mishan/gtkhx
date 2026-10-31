# AGENTS.md — GtkHx working notes

> Orientation for anyone (human or AI) working in this codebase.
> Subject-by-subject references live in **[docs/](docs/README.md)** — this file is the map,
> not the territory. The remaining product work is in **[ROADMAP.md](ROADMAP.md)**; the
> remaining C→Rust port work is in **[docs/rust/ROADMAP.md](docs/rust/ROADMAP.md)**.

## What this is

GtkHx is a Hotline client on GTK 4 + libadwaita: a C codebase being ported to Rust crate
by crate.

**Full backward compatibility with the Hotline 1.2 and 1.5 wire protocols is a hard
requirement.** Servers are scarce and ancient; we don't get to break them. Everything
else is negotiable.

License is **GPL-2.0-or-later** ("version 2 of the License, or (at your option) any later
version"). The "or later" clause is deliberate; don't strip it.

## Build

```sh
meson setup build && meson compile -C build
```

Floors: `gtk4 >= 4.16`, `libadwaita-1 >= 1.6`, `mullion-gtk-0 >= 0.3.1` (fetched and built
static by `subprojects/mullion-gtk.wrap` when not installed), `glib >= 2.76`, and
rustc at the workspace MSRV, which is the current gtk-rs line's (see
`rust-toolchain.toml`): the Rust floor follows gtk-rs, not a distribution's packaged
rustc, and the gtk-rs family moves up a line at a time, all together. The system
library floors above are what decide where GtkHx runs, and the bindings' features ask
for no more than they do.

Meson options:

| Option | Gates |
|---|---|
| `-Dvoice` (`auto`/`enabled`/`disabled`) | Voice and video chat. Needs GStreamer 1.20+. `auto` drops it silently when GStreamer is absent; `enabled` makes that a hard error. When off, the voice crates aren't built, the capability bit isn't advertised, and every voice source and call site compiles out behind `HAVE_VOICE`. |
| `-Dglycin_compat` (`auto`/`1`/`2`) | Which glycin loader generation the image decoder targets. Auto-detects the host's generation. See `docs/image-decoding.md`. |
| `-Dtests` | The test suite. |
| `-Dupdate_check` (`auto`/`enabled`/`disabled`) | Update notices. A combo, so `--auto-features` doesn't touch it. `auto` is on for Windows and macOS hosts (Homebrew/MacPorts/MSYS2 packages pass `disabled`); the Flatpak manifest passes `enabled`. See `docs/updates.md`. |
| `-Dbuild_version` | The version the build reports (About, the update check). Empty uses the project version; the release workflow sets it. |
| `-Dcargo_target_dir` | Cargo target dir, for CI caching. |

Optional deps, each behind a `HAVE_*` define: poppler (PDF preview), gtksourceview-5
(source preview), ImageMagick (complex PICT decode).

Windows and macOS build; some Unix-only pieces (the `/exec` command) compile out.

## The C / Rust split — read this first

The single most important orienting fact: **this is a hybrid codebase, and the Rust half
is the larger half.** A mental model of "a C app with some Rust helpers" will send you
looking for code in the wrong place.

- **Rust owns**: the wire protocol (the shared `hxproto` crate from
  [hx-libs](https://github.com/mishan/hx-libs)); the whole network stack including
  connect lifecycle, TLS, crypto, compression, framing, file transfers, and tracker fetch
  (`hxnet`); most receive handlers (`hxhandlers`); the session GObject and its boxed signal
  payloads (`gtkhx-core`); the chat rendering widget (the external `rotulus` crate); and
  a growing set of windows and dialogs (`gtkhx-ui`), the file browser and Settings among
  them.
- **C owns**: the dock (mullion-gtk) and layout persistence, the toolbar, the file
  providers under the browser, the transfer queue, the tray, notifications, previews,
  theming, and the receive handlers left in `rcv.c`. For Settings, the values live in
  `hxconfig` (which also reads and imports the legacy `gtkhxrc`) and the window and every
  page in `gtkhx-ui`; C keeps the change hooks that re-apply a preference across live
  widgets, the load-and-save glue, and the by-name bridge the Rust rows read and write
  through.
- **The seam** is a set of thin bridge files (`*_bridge.c`, `htxf_accessors.c`) plus
  hand-declared `extern` blocks. There is no cbindgen.

`docs/rust/ROADMAP.md` holds the live inventory of what is still C and why.

## Source layout

### `src/` — C, by subsystem

| Subsystem | Files |
|---|---|
| **Entry point** | `gtkhx.c` (`main()`, GtkApplication, signal-handler wiring, opening and closing a connection's tab) |
| **Dock / layout** | `dock_bridge.c` (the dock), `dock_pages.c`, `panel_registry.c`, `dock_layout.c`, `dock_layout_parse.c`, `toolbar.c` |
| **Settings** | `options.c` (change hooks, the startup load through `hxconfig`, the save timer and the `gtkhx_prefs_*` by-name bridge), `prefs_mirror.c` (the read-only C view of the settings), `icon_enum.c` (icon IDs for the Rust picker). The nickname and icon a connection presents are resolved in `gtkhx-ui`'s `identity.rs` |
| **Chat** | `chat.c` (window + output path), `chat_avatar.c`, `chat_history.c` |
| **Files** | `files_local_provider.c`, `files_remote_provider.c`, `files_provider.c`, `files_ops.c` (the providers; the browser itself is `gtkhx-ui`'s `files` module) |
| **Protocol (recv/send)** | `rcv.c` (the remaining receive handlers, and the dispatch switch for the frames the session hands over whole, where a reply nothing expected is shown if it is a refusal; the login's reply, chat, users, messages, news and files, and the replies to the banner, GIF-icon and inline-media requests, reach `hxhandlers` as the session's events instead), `commands.c`, `proto_helpers.c`, `proto_trace.c` |
| **Network glue** | `network.c`, `hxnet_bridge.c`, `host_port.c`, `hotline_url.c` |
| **Users / transfers** | `users.c`, `users_cell.c`, `usermod.c` (the user editor's access-bit table), `tasks.c` (the Transfers panel's queue: transfers only, one list for every connection, each row tagged with its connection; the panel's shell is `gtkhx-ui`'s `tasks` module) |
| **Sessions** | `session_registry.c` (the connection collection, the factory, and which connection has focus) |
| **Tracker** | `tracker_event.c` (the `HxTrackerServer` signal payload). The wire codec is `hxproto::tracker` in hx-libs; the fetch is `hxnet`. |
| **Media** | `inline_media*.c`, `gif_icons.c`, `cicn.c`, `pict_embed.c`, `pict_magick.c`, `preview.c` |
| **Theming / chrome** | `gtkhx_theme.c`, `gtkhx_icon.c`, `gtkutil.c`, `gtkurl.c` |
| **Voice** (optional) | `voice_bridge.c`, `voice_ptt_keyspec.c` |
| **Desktop integration** | `tray.c`, `notify.c`, `sound.c`, `sound_events.c` |
| **Bridges to Rust** | `hxnet_bridge.c`, `dock_bridge.c`, `gtkhx_ui_bridge.c`, `users_bridge.c`, `tracker_bridge.c`, `chat_send_bridge.c`, `voice_bridge.c`, `htxf_accessors.c`, `inline_media_decode.c` |
| **Infrastructure** | `debug.c`, `gtkhx_log.c`, `human_readable.c`, `uniquify_path.c`, `hl_code.c`, `cmd_exec.c` |

**Headers.** `hx.h` is an umbrella over the four real headers; new code should include
the narrowest one that works:

- `compat.h` — portability shims, gettext `_()`, `MAXPATHLEN`, byte-shift macros. Pure
  preprocessor. **`MAXPATHLEN` is hard-clamped to 4095**, not the host's `PATH_MAX`; any
  Rust `#[repr(C)]` mirror of a struct holding a `char[MAXPATHLEN]` must use 4095.
- `protocol.h` — wire/network/connection types. GLib, no GTK.
- `prefs.h` — preferences data.
- `session.h` — the `session` struct and the GTK-bearing types.

Also: `hotline.h` (wire struct layouts), `hotline_proto.h` (FFI declarations for the Rust
protocol crate), `hxconn.h` + `hxconn_layout.h` (the accessor seam over the opaque,
Rust-owned connection struct), `rotulus.h` (the chat widget's C ABI, which ships in the
`rotulus` crate and is found through `rust/rotulus.py`; C links straight to Rust exports),
`hl_access.h` (account access bits).

### Rust crates — by role

| Role | Crates |
|---|---|
| **Shared with hxd-ng** (from hx-libs) | `hxproto` — typed builders and parsers for every opcode, pure Rust; `hxfiles-xfer` (fork-header and HTXF codec); `hxhfs` (resource-fork sidecars); `hxsession` (the classic client session, which GtkHx drives in raw mode); `hxhope` (HOPE, the secure login: the handshake either side plays and the cipher and compression it agrees) over `hxcrypto` (its primitives). All are git dependencies pinned in `rust/Cargo.toml`. GtkHx's C ABI over them lives here: `gtkhx-proto-ffi` (the `gtkhx_proto_*` / `hx_recv_route` shims) and `gtkhx-files-ffi`, each also a standalone staticlib for the focused tests |
| **Network** | `hxnet` (connect lifecycle, TLS, SOCKS, the session's I/O, file transfers, tracker fetch), `hxtls-trust` |
| **Receive / send handlers** | `hxhandlers` — `recv::` and `send::` modules, one per domain; `hxtask` — the control channel's send primitive; `hxrequest` — the requests the client sends, built as plain values with no C imports, so the end-to-end suites can send exactly what production sends |
| **GObject layer** | `gtkhx-core` (the session signal hub, the connection struct's storage, boxed signal payloads, the messaging extension's roster), `hxmodel` |
| **UI** | `gtkhx-ui` (gtk4-rs windows and dialogs, module per window; `dock.rs` holds the per-connection keys and bindings below) |
| **Voice** (optional) | `hxvoice`, `hxvoice-model`, `hxvoice-runtime` (gstreamer-rs + webrtcbin); the requests are `hxhandlers::voice`, behind that crate's `voice` feature |
| **Media** | `hx-image-decode` (glycin), `hxmacres` (Mac resource fork + cicn) |
| **Support** | `hxbridge` (Rust↔GLib interop, tokio runtime), `hxtext` (Mac Roman ↔ UTF-8), `hxbookmarks`, `hxconfig` (the settings schema and the TOML file — the owner of every preference value at runtime), `hxupdate` (update-check version ordering, feed and decision), `hxsound` (rodio/cpal), `feature-unify` (forces identical feature resolution across the voice-on and voice-off builds so the shared dependency graph compiles once) |
| **End-to-end tests** | `hx-e2e` — GtkHx's own requests and reply parsers against the rig's servers, behind its `rig` feature |
| **External, from crates.io** | The chat view, `rotulus` (the GTK4 widget, LGPL) with its dependency-free layout engine `rotulus-layout` and `rotulus-mirc`. It lives in [its own repository](https://github.com/mishan/rotulus) and knows nothing about Hotline; GtkHx configures it in `gtkhx-ui`'s `chat_view.rs`. See `docs/chat-view.md` for how it is pinned, built and updated |
| **Link façade** | `gtkhx-ffi` — bundles every FFI-exporting crate into a single `libgtkhx_ffi.a`, so the binary links exactly one archive instead of a hand-ordered list. Several crates also build a standalone `staticlib` on the side, purely so the test suite can link one crate at a time. See `docs/rust/crate-layout.md`. |

### Other directories

- `tests/` — three tiers: unit (pure functions), proto (wire fixtures), integration
  (end-to-end against a Docker rig of mhxd / Janus / hxd-ng / hlservd / hxtrackd /
  Argus / a SOCKS proxy).
  `tests/COMPOSE.md` describes the rig; `tests/run.sh` brings it up. The C integration
  tests build their frames by hand; `rust/crates/hx-e2e` drives GtkHx's own requests
  and reply parsers against the same rig (`cargo test -p hx-e2e --features rig`), and
  is where new end-to-end coverage goes.
- `po/` — translations (German, Spanish, French, Japanese, Portuguese; `po/LINGUAS`).
  `sounds/` — chat alert `.wav`s.
- `data/` — the desktop file, the AppStream metainfo, and `data/screenshots/` (the README
  and AppStream pictures, each beside its accessible tree, `NAME.a11y.txt`).
- `src/themes/` — built-in theme files, shipped as GResource. `src/icons/` — the
  vendored app-specific symbolic icons (see its README), beside the classic
  `src/pixmaps/`.
- `tools/` — the gates below (`reformat.sh`, `fix-whitespace.py`, `check-potfiles.sh`,
  `check-c-growth.sh`, `check-icon-buttons.sh`, `flatpak-cargo-generator.py`),
  `coverage.sh`, `analyze.sh`, `uibench.sh` (the in-app UI benchmarks — see
  `docs/performance.md`), `isolated-run.sh` (runs a test command sealed off from the
  desktop — see "Before calling anything done"), and the two screenshot tools:
  `screenshots.sh` (the committed scenes, in Docker with a pinned shotbox) and
  `screenshot.py` (quick headless pictures). See `docs/screenshots.md`.

## The model / view boundary

Model-side code (`rcv.c`, `network.c`, `commands.c`, and the Rust receive
handlers) reaches the view by **emitting signals
on `GtkhxSession`** — a singleton GObject
implemented in Rust (`gtkhx-core`) exporting a stable C ABI (`gtkhx_session_get_default`,
`gtkhx_session_emit_<name>`). Direct `gtk_*` / `GTK_*` calls in `rcv.c`, `network.c` and
`commands.c` are bugs; the audit is one grep.

Signals cover chat and chat history, messages, agreement, news, users, files, the transfer
queue, tracker results, GIF icons, connection state, and login. Every signal but the
tracker's carries the connection it came from, and a handler routes by that. Read
`rust/crates/gtkhx-core/src/session.rs` for the current list and payloads rather than
trusting a table here — it grows.

View-side handlers are static adapter functions in `gtkhx.c` (`on_<name>_signal`), wired
up once in `gtkhx_connect_signals()` from `fe_init`. Boxed payload types are `#[repr(C)]`
mirrors in `gtkhx-core::boxed`, with layout pinned by `_Static_assert`s on the C side.

**When a Rust receive handler needs the view to log a notice, emit a session signal** —
don't call a view-side C log shim over FFI. The gettext call and the preference gate belong
in the C view handler.

A few view-side conveniences are called by name from model files rather than
signal-routed: `hx_printf` / `hx_printf_prefix`, `hx_clear_chat`.

## Per-connection state

GtkHx holds several connections at once, one tab each. A `session` is a heap object built
by `hx_session_new()` and held in the collection in `session_registry.c`, in tab order;
`hx_session_open()` and `hx_session_close()` in `gtkhx.c` open and close a tab, and
closing one frees its session and connection. `docs/multi-connection.md` is the design.
Each session holds:

- `chats` — the Rust `HxChatRegistry` (not a `GHashTable`), keyed by chat ID, seeded with
  the always-present public chat at cid 0.
- `htlc` — the connection. **Heap-allocated and opaque**, reached through the `hxconn.h`
  accessors. It owns its transport handle (`hx_conn_bridge_handle`), so `hxnet_bridge.c`
  holds no connection state of its own.

Routing, and getting it wrong sends a request to the wrong server:

- `sess_from_htlc(htlc)` is "the session that owns this connection" — use it in model
  code, which always has the htlc for the event it is handling. It is a real
  back-pointer read.
- `hx_active_session()` is "the focused tab's session" — use it only in UI code that
  really means the focus: the toolbar, the main menu, the tray, the chrome that follows
  the focus. A window that acts on one server holds that server's connection instead.
- A per-connection identity that outlives a close is the connection's serial
  (`hx_conn_serial`, `ConnKey` in Rust), resolved through `hx_session_with_serial` /
  `hx_conn_with_serial`, which answer NULL for a closed one. Hold a serial, not a
  pointer, across the main loop.
- In `gtkhx-ui`, a window bound to a connection holds a `dock::Bound` — the connection
  and which connect of it, so a window left open across a reconnect elsewhere goes
  quiet — and closes with its tab through `dock::close_with`; `useredit.rs` is the
  pattern. Per-connection UI state is a map keyed by `ConnKey` (`news.rs`,
  `create_post.rs`, `files/`, `msg.rs`), never a single process-wide slot.

## Idioms and pitfalls

- **Threading.** Worker threads marshal to the main thread via `g_idle_add` or
  `gtkhx_bridge_post_to_main`; they never call GTK or emit signals directly. Background
  work runs on the tokio blocking pool, not raw threads.
- **The Rust orchestrator owns the control socket** and pushes frames across the bridge;
  nothing in C watches it. The one `g_io_add_watch` is on the `/exec` output pipe, which
  is Unix-only. Timers are `g_timeout_add_seconds`.
- **List widgets are `GtkColumnView`.**
- **Don't change the negotiated wire format.** The Rust implementation underneath is fair
  game; the bytes on the wire are not. 1.2/1.5/1.9 compat is the hard requirement.
- **Prefer `assert!` over `debug_assert!`** in the Rust crates, even for "can't happen" wire
  invariants — `debug_assert!` is stripped in release.
- **Don't use `g_assert` for invariants that matter in release builds** — it compiles out
  under `G_DISABLE_ASSERT`. Use `g_error` (always fatal) or `g_critical` plus a graceful
  skip. The `g_assert_*` test macros are fine in tests.
- **`g_autoptr` / `g_autofree`** are used opportunistically, not universally. `tasks.c`
  and `hxnet_bridge.c` are the in-tree pattern to copy. Not worth a sweep; convert when
  you touch a function.
- **`GtkTreeListModel`**: attach children to a node *before* appending the node.
  `create_child_model` fires once and the leaf-vs-expandable verdict sticks.
- **Icon-only buttons** go through `gtkhx_icon_button_new` (C) or `icon_button` (Rust),
  which give them an accessible name; `tools/check-icon-buttons.sh` enforces it.
- **Call the gettext helpers unqualified** (`use crate::tr::tr;`, then `tr("…")`): gettext
  0.25 doesn't extract a path-qualified `crate::tr::tr("…")`, so the string silently
  drops out of the catalog where it is built with that release.
- **No phase labels in source comments.** Describe the reason, not the project-management
  label. Phase labels are fine in commit messages and docs.

## Debug infrastructure

Set `GTKHX_DEBUG` to a comma-separated list of categories before launch:

```sh
GTKHX_DEBUG=proto             # full Hotline wire trace (in/out, types, chunks)
GTKHX_DEBUG=proto,news,voice
GTKHX_DEBUG=all
```

Output goes to stderr, prefixed `[<category>]`. Categories in use include `proto`, `net`,
`news`, `tracker`, `voice`, `xfer`, `media`, `icon`, `sound`, `dock`, `layout`, `startup`,
and `name`. `secondconn` builds a second, unconnected connection at startup, with its own
page in every panel, so tab switching can be driven by hand. Adding a category takes no
registration — `debug_log ("newcat", "…")` just works. Infrastructure in
`src/debug.{c,h}`; the wire trace in `src/proto_trace.{c,h}` and
`rust/crates/hxnet/src/proto_trace.rs`.

The protocol trace is the fastest way to diagnose "the server doesn't like X": match
outgoing transaction IDs against incoming task-error replies and you know exactly which
request was rejected, and why. Each traced request also names the connection it goes out
on (`on connection #N (host:port)`, N being the serial), which is where a wrong-server bug
shows up.

## Reference servers

- **mhxd** (<https://github.com/kangsterizer/mhxd>; keep a clone *outside* this checkout,
  e.g. `../mhxd`, for reading — it is not part of the repo) — the controlled, repeatable
  test target and the canonical reference for opcodes and the access bitmap. Its known bugs are in `docs/mhxd-bugs.md`, and it wedges: when it
  starts refusing everything, restart its container before suspecting the client. Keep it
  pinned to a specific revision in `tests/mhxd/`; its master moves under the build.
- **Janus** — VesperNet's closed-source server. Implements the fogWraith extensions (voice,
  inline media, GIF icons, chat history), so it is the integration target for all of them.
  Runs in the test rig. Its known bugs, kept to send upstream, are in `docs/janus-bugs.md`.
- **hxd-ng** (<https://github.com/mishan/hxd-ng>) — a Rust server built on the same hx-libs
  crates GtkHx uses. In the rig for voice, video, the pictures it attaches to news
  articles, and its hotline-ng port; the video tests run against it as well as Janus.
  The image is the one hxd-ng's CI publishes, pinned by digest in `tests/hxd-ng/`.
- **hlservd** — the Hotline Server 1.9.5 as a headless POSIX daemon: the rig's stand-in
  for the original 1.9 server, which otherwise needs Wine. Its image lives in hotline-docker; see
  `tests/hlservd/README.md`.
- **Argus** — a real tracker-v3 tracker, in the test rig. **hxtrackd** covers the v1
  tracker fallback path.
- **hlserver.com** — behaves like a 1.0/1.2 server from the client's perspective: no
  version advertisement, rejects unknown opcodes. Useful for legacy-path checks.
- **Badmoon** — a confirmed 1.9 server. **Mobius** servers use a dedicated TLS port and drop
  the connection on an agreement-accept that omits the options field — worth remembering
  when a real-server bug looks like ours.

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

## Before calling anything done

Run the repo's own gates, not an approximation of them, and run them last: a
`clang-format` over just the files you edited answers a different question from the
one CI asks, and any edit after the formatter can undo it.

```sh
tools/reformat.sh --check          # clang-format 21.1.8 (pip), every C file
python3 tools/fix-whitespace.py --check
tools/check-icon-buttons.sh        # icon-only buttons have a name
tools/check-potfiles.sh            # needs gettext 0.25+ for Rust support
tools/check-c-growth.sh            # no net lines of C added to src/
python3 tools/flatpak-cargo-generator.py rust/Cargo.lock \
  && git diff --exit-code rust/cargo-sources.json   # after any Cargo.lock change
cd rust && cargo fmt --all --check
```

**C does not grow.** `check-c-growth.sh` fails a branch that adds more lines of
C to `src/` than it removes; CI runs it on every pull request to `main`, and
locally it wants a fresh `git fetch origin main` first. If the feature
you are building needs real changes to C content, port that content first. When
growth is genuinely the right call, a `C-Growth: <reason>` trailer in a commit
message on the branch records the exception — an empty follow-up commit
carrying it works. Reasoning in `docs/rust/ROADMAP.md`, "How the
rest of the port gets done".

Then, **both** voice configurations — CI builds each, and `#ifdef HAVE_VOICE`
mistakes are invisible in whichever one you happen to be in:

```sh
ninja -C build-voice && ninja -C build-novoice
tools/isolated-run.sh meson test -C build-voice --no-suite integration
tools/isolated-run.sh meson test -C build-novoice --no-suite integration
cd rust
cargo clippy --workspace --all-targets \
  --features hxbridge/runtime,hxnet/socks,hx-image-decode/glycin-v3,gtkhx-ffi/voice \
  -- -D warnings
cargo clippy --workspace --all-targets -- -D warnings
cargo build --target wasm32-unknown-unknown --no-default-features \
  -p hxtext -p hxrequest -p hxvoice --ignore-rust-version
../tools/isolated-run.sh cargo test --workspace
../tools/isolated-run.sh cargo test -p gtkhx-ui --features voice
cargo test -p hxhandlers --features voice
cargo test -p hxnet --features socks
cargo test -p hx-image-decode --lib --no-default-features --features glycin-v2
```

With the rig up (`tests/run.sh`), the integration suites too:

```sh
tools/isolated-run.sh meson test -C build-voice --suite integration
cd rust && cargo test -p hx-e2e --features rig
```

**Run tests under `tools/isolated-run.sh`, not bare `xvfb-run`** — the
integration suite included. It gives the command its own Xvfb display
(with `GDK_BACKEND=x11`), its own D-Bus session bus with nothing
auto-started on it, and its own PipeWire + WirePlumber with a null sink
and source, and it keeps GStreamer's hardware plugins (V4L2, libcamera)
off the plugin path. Under bare `xvfb-run` the voice and video tests
play into your desktop's audio server, scan your real cameras, and can
start real portals. CI runs the same wrapper. It needs PipeWire,
WirePlumber and `dbus-run-session` installed; `ISOLATED_RUN_KEEP=1` keeps
its scratch directory (the PipeWire and WirePlumber logs) for a look
afterwards.

`-D warnings` is not decoration: CI denies, so a lint that is only a warning
locally — a deprecation, an unused import — is a red build there and invisible
here. Deprecations in particular, because the gtk-rs bindings are pinned to a
GTK older than the one on a dev machine.

The Rust test run matters in both: a module behind `#[cfg(feature = "voice")]`
— `voice_arbiter`, `voice_panel`, `hxhandlers::voice` — is not even compiled
without it, so its tests can be failing to build while the default run is
green. Likewise `hx-e2e`'s suites compile to nothing without `rig`.

**Run the display-backed test more than once.** It is one `#[test]` holding
every check in sequence against process-global widget state, so a check that
depends on what an earlier one left behind can pass, fail, and pass again. If a
new check asserts on the live tab strip, prefer building its own widgets.

**Screenshots.** A change to what a scene shows, or to what a screen reader hears in it,
retakes that scene with `tools/screenshots.sh NAME` (Docker only) and commits the new
`data/screenshots/NAME.png` and `NAME.a11y.txt` with it. Each scene also runs shotbox's
accessibility lint, so an unnamed control fails it. The `screenshots` workflow runs
`--check` on the release-prep PR and every Monday on `main`. See `docs/screenshots.md`.

**CI** runs `lint` on every push and pull request. `tests`, `analyze` and `codeql` skip a
change that touches only Markdown, `docs/`, `data/screenshots/`, the metainfo or `LICENSE`;
`lint` still validates the metainfo.

- **Branches, not direct main commits.** `<short-topic>`, kebab-case, no prefix. The
  maintainer opens, reviews and merges PRs. Push follow-up commits to the same branch
  after review. CI must be green to merge.
- **Squash before opening the PR** — one commit per branch. `git reset --soft <merge-base>`.
- **Commits** have descriptive bodies. No `Author:` line in the body — the git author
  field already carries it.
- **No AI attribution** — no `Co-Authored-By` trailer naming an AI, no "Generated with"
  footer in a PR body, and nothing of the kind in branch names, comments, or docs.
- **Tests fail loudly.** Never `g_test_skip` around something that didn't work; a skip looks
  like a pass in CI and masks bugs.
- **Prefer a reproduction over a debugging session.** When a bug surfaces against a real
  server, write an integration test against the local Janus or mhxd container rather than
  asking for a re-run with logging. Faster, higher fidelity, and it doubles as a
  regression guard.
- **Avoid exact counts in comments and docs** — lines, files, tests, commits. They go stale,
  and the narrative is stronger without them.
- **Don't re-litigate settled decisions** without a strong reason. See "Decisions locked in"
  in `ROADMAP.md`.
