# rust/meson.build — Invoke Cargo to build the Rust workspace and produce
# static libraries that the C binary links against.
#
# Strategy: a custom_target runs `cargo build --release` with the target
# directory inside the Meson build dir. The output is the static library
# archives that src/meson.build links into the gtkhx executable.
#
# Current crates:
#   hxcrypto-hash   — HMAC/MD5/SHA1/SHA256 (replaces src/hmac.c)
#   hxcrypto-stream — Blowfish OFB-64 (replaces src/cipher.c primitives)
#   hxcrypto-aead   — ChaCha20-Poly1305 + HKDF-SHA256 (replaces
#                     src/cipher_aead.c primitives)
#   hxcompress      — GZIP / LZ4 / ZSTD streaming (replaces src/compress.c)
#   hotline-proto   — Phase R2 wire protocol parsers + builders.
#   hxbridge        — Phase R3.0 Rust ↔ GLib bridge (C-owned GObject
#                     wrapping + signal emit). Lifetime model documented
#                     in docs/rust/glib-interop.md.
#   hxvoice-runtime — Phase 8.B GStreamer-based voice runtime. Phase
#                     8.B exposes only gtkhx_voice_init (gst::init
#                     wrapper); per-session runtime handle + state
#                     machine wiring lands in Phase 8.C. Pulls in
#                     gstreamer + gstreamer-app + gstreamer-audio +
#                     gstreamer-sdp + gstreamer-webrtc from
#                     workspace.dependencies.
#   hxvoice         — Phase 8.C pure-Rust voice-chat state machine.
#                     no_std-friendly (hashbrown is the only dep);
#                     deliberately has no GLib / GStreamer / GTK
#                     surface so `cargo test -p hxvoice` runs in
#                     any CI container. Consumed by hxvoice-runtime,
#                     not linked into the C binary directly — hence
#                     no rust_hxvoice_dep declared below.
#
# Each FFI-exposed crate is a staticlib that the C side wraps with a
# thin dispatcher. (hxvoice above is the exception — pure Rust,
# rlib-only, consumed by hxvoice-runtime as a workspace dep with no
# C-side surface at all.) FFI signatures are hand-declared `extern`
# blocks in the relevant C TU rather than cbindgen-generated headers
# — drift surfaces at link time as undefined symbols, which is
# enough for opaque-pointer APIs and keeps the build simpler.
#
# Two crates retired at the R2 closeout: hxrand (whose only purpose was
# wrapping getrandom(2) — the C code calls getrandom() directly via the
# restored src/rand.c now) and hxutil (a build-validation placeholder
# from R0 that nothing depended on).

cargo = find_program('cargo')

# Collect all .rs sources so Meson knows to re-run when they change.
rust_sources = files(
  'crates/hxcrypto-hash/src/lib.rs',
  'crates/hxcrypto-stream/src/lib.rs',
  'crates/hxcrypto-stream/src/blowfish_ofb.rs',
  'crates/hxcrypto-aead/src/lib.rs',
  'crates/hxcompress/src/lib.rs',
  'crates/hotline-proto/src/lib.rs',
  'crates/hotline-proto/src/wire.rs',
  'crates/hotline-proto/src/messages.rs',
  'crates/hotline-proto/src/text.rs',
  'crates/hotline-proto/src/parse.rs',
  'crates/hotline-proto/src/ffi.rs',
  'crates/hxbridge/src/lib.rs',
  # Phase R3.1+ submodules, gated behind the `runtime` Cargo feature
  # that the rust_build custom_target turns on below.
  'crates/hxbridge/src/blocking.rs',
  'crates/hxbridge/src/channel.rs',
  'crates/hxbridge/src/runtime.rs',
  # Phase R4 — GtkhxSession as a Rust glib::subclass GObject (replaces
  # src/gtkhx_session.c). References the C boxed GTypes via FFI; R4.2
  # re-hosted all three boxed payload types (HxMsgEvent, HxChatEvent,
  # HxTrackerServer) into the gtkhx-boxed crate, so this crate's externs
  # now resolve against gtkhx-boxed instead of proto_helpers.c /
  # tracker_event.c.
  'crates/gtkhx-session/src/lib.rs',
  'crates/gtkhx-session/src/chat_registry.rs',
  # Phase R4.2 — boxed signal-payload value-types (HxMsgEvent, HxChatEvent
  # + HxChatMedia, HxTrackerServer + HxTrackerV3Meta), self-contained
  # (glib only) so proto unit tests can pull their _copy/_free without
  # dragging gtkhx-session's C externs.
  'crates/gtkhx-boxed/src/lib.rs',
  'crates/gtkhx-boxed/src/msg.rs',
  'crates/gtkhx-boxed/src/chat.rs',
  'crates/gtkhx-boxed/src/media_table.rs',
  'crates/gtkhx-boxed/src/tracker.rs',
  # Phase R5 — gtkhx-ui: the window UIs in gtk4-rs (first gtk4-rs UI
  # crate). One module per ported window; the Tracker window is the first.
  'crates/gtkhx-ui/src/lib.rs',
  'crates/gtkhx-ui/src/ffi.rs',
  'crates/gtkhx-ui/src/conversation.rs',
  'crates/gtkhx-ui/src/tr.rs',
  'crates/gtkhx-ui/src/dock.rs',
  'crates/gtkhx-ui/src/about.rs',
  'crates/gtkhx-ui/src/agreement.rs',
  'crates/gtkhx-ui/src/bookmarks.rs',
  'crates/gtkhx-ui/src/cipher_vocab.rs',
  'crates/gtkhx-ui/src/connect.rs',
  'crates/gtkhx-ui/src/options.rs',
  'crates/gtkhx-ui/src/rc4_dialog.rs',
  'crates/gtkhx-ui/src/tls_trust_dialog.rs',
  'crates/gtkhx-ui/src/useredit.rs',
  'crates/gtkhx-ui/src/user_row.rs',
  'crates/gtkhx-ui/src/users_view.rs',
  'crates/gtkhx-ui/src/users_voice_col.rs',
  'crates/gtkhx-ui/src/voice_panel.rs',
  'crates/gtkhx-ui/src/voice_ptt.rs',
  'crates/gtkhx-ui/src/users.rs',
  'crates/gtkhx-ui/src/tasks.rs',
  'crates/gtkhx-ui/src/news.rs',
  'crates/gtkhx-ui/src/news_browser.rs',
  'crates/gtkhx-ui/src/chat.rs',
  'crates/gtkhx-ui/src/chat_tabs.rs',
  'crates/gtkhx-ui/src/chat_members.rs',
  'crates/gtkhx-ui/src/msg.rs',
  'crates/gtkhx-ui/src/files.rs',
  'crates/gtkhx-ui/src/pchat.rs',
  'crates/gtkhx-ui/src/inline_media_dialog.rs',
  'crates/gtkhx-ui/src/user_info.rs',
  'crates/gtkhx-ui/src/create_post.rs',
  'crates/gtkhx-ui/src/emoji.rs',
  'crates/gtkhx-ui/src/broadcast.rs',
  'crates/gtkhx-ui/src/tracker/mod.rs',
  'crates/gtkhx-ui/src/tracker/ffi.rs',
  'crates/gtkhx-ui/src/tracker/meta.rs',
  'crates/gtkhx-ui/src/tracker/row.rs',
  'crates/gtkhx-ui/src/tracker/actions.rs',
  # Phase R3.3 — hxnet Connection actor + FFI surface.
  'crates/hxnet/src/lib.rs',
  'crates/hxnet/src/cipher.rs',
  'crates/hxnet/src/command.rs',
  'crates/hxnet/src/compress.rs',
  'crates/hxnet/src/connection.rs',
  'crates/hxnet/src/event.rs',
  'crates/hxnet/src/ffi.rs',
  'crates/hxnet/src/frame.rs',
  'crates/hxnet/src/hope_blowfish.rs',
  'crates/hxnet/src/htxf.rs',
  'crates/hxnet/src/tls.rs',
  'crates/hxnet/src/transform.rs',
  'crates/hxvoice/src/lib.rs',
  'crates/hxvoice/src/event.rs',
  'crates/hxvoice/src/action.rs',
  'crates/hxvoice/src/state.rs',
  'crates/hxvoice-model/src/lib.rs',
  'crates/hxvoice-model/src/tests.rs',
  'crates/hxvoice-send/src/lib.rs',
  'crates/hxvoice-send/src/tests.rs',
  'crates/hxtext/src/lib.rs',
  'crates/hxtext/src/tests.rs',
  'crates/hxchat-send/src/lib.rs',
  'crates/hxchat-send/src/tests.rs',
  'crates/hxchat-model/src/lib.rs',
  'crates/hxchat-model/src/tests.rs',
  'crates/hxchat-model/src/input_history.rs',
  'crates/hxmember-model/src/lib.rs',
  'crates/hxmember-model/src/tests.rs',
  'crates/hxvoice-runtime/src/lib.rs',
  'crates/hxvoice-runtime/src/audio.rs',
  'crates/hxvoice-runtime/src/runtime.rs',
  'crates/hxvoice-runtime/src/ffi.rs',
  'crates/hx-image-decode/src/lib.rs',
  'crates/hx-image-decode/src/compat.rs',
  'crates/hx-image-decode/src/sniff.rs',
  'crates/hx-image-decode/src/ffi.rs',
  'crates/hx-image-decode/src/caps.rs',
  'crates/hx-image-decode/src/decode.rs',
  'crates/hx-image-decode/src/ffi_result.rs',
  'crates/hx-image-decode/src/telemetry.rs',
  # CI feature-unification shim (crates/feature-unify) — pins Cargo
  # features so the voice-off/voice-on builds share their dependency
  # compiles. Empty lib; see its Cargo.toml.
  'crates/feature-unify/src/lib.rs',
  'Cargo.toml',
  'Cargo.lock',
  'crates/hxcrypto-hash/Cargo.toml',
  'crates/hxcrypto-stream/Cargo.toml',
  'crates/hxcrypto-aead/Cargo.toml',
  'crates/hxcompress/Cargo.toml',
  'crates/hotline-proto/Cargo.toml',
  'crates/hxbridge/Cargo.toml',
  'crates/gtkhx-session/Cargo.toml',
  'crates/gtkhx-boxed/Cargo.toml',
  'crates/gtkhx-ui/Cargo.toml',
  'crates/hxnet/Cargo.toml',
  'crates/hxvoice/Cargo.toml',
  'crates/hxvoice-model/Cargo.toml',
  'crates/hxvoice-send/Cargo.toml',
  'crates/hxtext/Cargo.toml',
  'crates/hxchat-send/Cargo.toml',
  'crates/hxchat-model/Cargo.toml',
  'crates/hxmember-model/Cargo.toml',
  'crates/hxvoice-runtime/Cargo.toml',
  'crates/hx-image-decode/Cargo.toml',
  'crates/feature-unify/Cargo.toml',
)
# cargo-sources.json is regenerated from Cargo.lock for the Flatpak
# offline build; it is not consumed by `cargo build` directly. Cargo.lock
# above is the load-bearing dep-graph file that drives this custom_target's
# re-execution when versions move.

# Cargo target dir. Defaults inside the meson build dir (historical
# behaviour, keeps `rm -rf build` clean). The `cargo_target_dir` option lets
# CI relocate it to a stable path outside the build dir — required for
# cache-restoring the Rust build before `meson setup` (which refuses a
# non-empty build dir) and for sharing one compiled target tree across the
# voice-disabled + voice-enabled builds in a single job. See meson_options.txt.
_rust_target_opt = get_option('cargo_target_dir')
if _rust_target_opt == ''
  rust_target_dir = meson.project_build_root() / 'rust-target'
else
  rust_target_dir = _rust_target_opt
endif

# Phase R3.2 turns the hxbridge `runtime` cargo feature on so the
# tokio runtime + blocking-pool shim + tokio→GLib channel ferry land
# in the staticlib linked into src/gtkhx. The first production consumer
# is banner.c's HTXF worker, which calls
# gtkhx_bridge_spawn_blocking_with_idle in lieu of pthread_create.
# Subsequent R3.x consumers (xfers.c in R3.4, hxnet Connection actor in
# R3.3) inherit the feature without needing a second flip.
# hxnet/socks: SOCKS proxy support in the connect primitive (R3 item 9).
# Opt-in at the crate level so embedders skip the tokio-socks dep; the
# gtkhx binary wants it, so the production build turns it on here.
cargo_features = 'hxbridge/runtime,hxnet/socks'

# Inline-media image decoder backend (docs/glycin-migration-plan.md).
# -Dglycin_compat selects which glycin loader generation hx-image-decode
# targets, by flipping a cargo feature on that crate:
#
#   '2': glycin 3.x / 2+ loaders. The crate's own default feature is
#       already glycin-v3, so we just append it explicitly (harmless, and
#       keeps the cargo line self-documenting). Defaults stay on, so every
#       workspace member builds as before.
#
#   '1': glycin 2.x / 1+ loaders (Debian stable). This MUST suppress the
#       crate's default glycin-v3 — enabling both backends at once is a
#       hard compile_error (src/compat.rs) — so we pass
#       --no-default-features. Because that flag is workspace-wide, it
#       also drops the *other* members' defaults; only hxcompress has
#       load-bearing ones (lz4 + zstd), so we re-supply exactly those.
#       Any future member that grows a default feature must be added
#       here too.
#
#   'auto' (default): probe the build host for the newest installed
#       loader generation and resolve to '2' or '1'. For native builds
#       the build host IS the target host, so this picks the backend that
#       will actually find loaders at runtime; for the Flatpak build the
#       configure step runs inside the runtime (which the manifest still
#       pins to '2' explicitly, so it never depends on the probe).
glycin_opt = get_option('glycin_compat')
if glycin_opt == 'auto' and meson.is_cross_build()
  warning('glycin loader auto-detect probes the BUILD host, which may not ' +
          'match the target runtime in a cross build. Set -Dglycin_compat=2 ' +
          '(2+ loaders) or =1 (1+ loaders) explicitly for the target.')
endif
if glycin_opt == 'auto'
  # Scan, newest-first, for a registered loader generation. glycin itself
  # discovers loaders via `<datadir>/glycin[-loaders]/<gen>+/conf.d/*.conf`
  # across $XDG_DATA_DIRS, so the presence of those .conf files is the
  # authoritative "decoding will work here" signal; the libexec dirs are a
  # secondary fallback. Prints '2', '1', or 'none'.
  glycin_probe = '''
    f1=0; f2=0
    roots="$(printf %s "${XDG_DATA_DIRS:-/usr/local/share:/usr/share}" | tr : " ") /usr/share /usr/local/share"
    for base in $roots; do
      for sub in glycin glycin-loaders; do
        for c in "$base/$sub/2+/conf.d/"*.conf; do [ -e "$c" ] && f2=1; done
        for c in "$base/$sub/1+/conf.d/"*.conf; do [ -e "$c" ] && f1=1; done
      done
    done
    for d in /usr/libexec/glycin-loaders /usr/lib/*/glycin-loaders /usr/lib64/glycin-loaders; do
      [ -d "$d/2+" ] && f2=1
      [ -d "$d/1+" ] && f1=1
    done
    if [ "$f2" = 1 ]; then echo 2; elif [ "$f1" = 1 ]; then echo 1; else echo none; fi
  '''
  glycin_detect = run_command(find_program('sh'), '-c', glycin_probe, check : false)
  glycin_found = glycin_detect.stdout().strip()
  if glycin_found == '1'
    glycin_gen = '1'
    message('glycin loader auto-detect: found the 1+ generation → glycin 2.x backend.')
  elif glycin_found == '2'
    glycin_gen = '2'
    message('glycin loader auto-detect: found the 2+ generation → glycin 3.x backend.')
  else
    glycin_gen = '2'
    warning('glycin loader auto-detect: no glycin loaders found on the build ' +
            'host; defaulting to the 2+ / glycin 3.x backend. If this build ' +
            'targets a 1+-only runtime (e.g. Debian stable), pass ' +
            '-Dglycin_compat=1.')
  endif
else
  glycin_gen = glycin_opt
endif

if glycin_gen == '1'
  cargo_no_default = '--no-default-features'
  cargo_features = cargo_features + ',hxcompress/lz4,hxcompress/zstd,hx-image-decode/glycin-v2'
else
  cargo_no_default = ''
  cargo_features = cargo_features + ',hx-image-decode/glycin-v3'
endif

# Voice crate gating. When voice is compiled out (see -Dvoice / the
# `voice_enabled` boolean in the top-level meson.build), exclude the two
# voice crates from the workspace build: hxvoice-runtime pulls in the
# gstreamer-rs family (and therefore needs the GStreamer system libs at
# build time), and hxvoice is its only consumer. Nothing else in the
# C-linked crate set depends on either, so `--workspace --exclude` is a
# clean cut — the build then needs no GStreamer present at all. With
# voice on, the empty string leaves the default all-members build intact.
# The `pkg/feature` syntax in cargo_features stays valid alongside
# `--workspace`.
if voice_enabled
  cargo_voice_args = ''
  # Turn on gtkhx-ui's `voice` feature so the user-list voice-indicator
  # column + the per-chat voice toolbar compile their real (voice-model /
  # runtime / wire-sender-backed) implementations instead of the NULL/no-op
  # stubs. Package-qualified feature syntax, same shape as cargo_features
  # above; valid at the virtual-workspace root the way the existing
  # hxbridge/runtime + hxnet/socks entries already are.
  cargo_features = cargo_features + ',gtkhx-ui/voice'
else
  cargo_voice_args = '--workspace --exclude hxvoice --exclude hxvoice-runtime --exclude hxvoice-model --exclude hxvoice-send'
endif

# --ignore-rust-version: several transitive deps (zbus / zvariant /
# zvariant_derive / zvariant_utils / zbus_macros / zbus_names /
# glycin-common / glycin-utils) carry `rust-version` >= 1.87 in
# their Cargo.toml manifests, but their actual source compiles
# cleanly on rustc 1.85 (Debian trixie's stock rustc, the project's
# native-build floor). The MSRV labels are over-conservative; the
# code itself doesn't use any post-1.85 language or stdlib feature.
# Passing --ignore-rust-version tells cargo to stop treating the
# label mismatch as a hard error. See rust-toolchain.toml for the
# rationale on the 1.85 floor, and the gtk-rs-0.21 / gstreamer-rs-0.24
# downgrade in Cargo.toml for the version-pin half of the same
# Debian-stable story.
#
# Build the Rust workspace. The custom_target output is a stamp file that
# Meson uses for dependency tracking; the actual artifacts are the .a
# archives under rust_target_dir/release/.
rust_build = custom_target('rust-workspace',
  command : [
    'sh', '-c',
    '@0@ build --release --ignore-rust-version @5@ @6@ --manifest-path @1@ --target-dir @2@ --features @3@ && touch @4@'.format(
      cargo.full_path(),
      meson.current_source_dir() / 'Cargo.toml',
      rust_target_dir,
      cargo_features,
      '@OUTPUT@',
      cargo_no_default,
      cargo_voice_args,
    ),
  ],
  # Pass the meson project version to the cargo build so the gtkhx-ui
  # About dialog can embed it via option_env!("GTKHX_VERSION").
  env : {'GTKHX_VERSION': meson.project_version()},
  depend_files : rust_sources,
  output : 'rust-workspace.stamp',
  build_by_default : true,
  console : true,
)

# Crypto hash (Phase R1; will replace src/hmac.c when wired through).
rust_hxcrypto_hash_dep = declare_dependency(
  sources : rust_build,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxcrypto_hash',
  ],
)

# Stream cipher (Phase R1; will replace src/cipher.c Blowfish path
# when wired through).
rust_hxcrypto_stream_dep = declare_dependency(
  sources : rust_build,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxcrypto_stream',
  ],
)

# AEAD (Phase R1; will replace src/cipher_aead.c when wired through).
rust_hxcrypto_aead_dep = declare_dependency(
  sources : rust_build,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxcrypto_aead',
  ],
)

# Compression (Phase R1; will replace src/compress.c when wired through).
rust_hxcompress_dep = declare_dependency(
  sources : rust_build,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxcompress',
  ],
)

# Wire protocol (Phase R2). Replaces the byte-twiddling in rcv.c /
# commands.c one opcode at a time; the C side keeps the dispatch table
# and the GtkhxSession signal emit. Foundation commit wires the
# SELFINFO + TASK-header parsers. FFI prototypes are hand-declared on
# the C side (no cbindgen) — drift surfaces as a link-time undefined
# symbol, same discipline as the R1 crates.
rust_hotline_proto_dep = declare_dependency(
  sources : rust_build,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhotline_proto',
  ],
)

# hxchat-send (Phase R5) — the Rust port of chat.c's send path. Exports the
# hx_send_chat / hx_chat_user / hx_invite_user / hx_chat_join / hx_part_chat /
# hx_change_subject C ABI the chat.h callers link against; implemented over
# hotline-proto's native chat builders. Unconditional (chat is always built),
# so it's part of the default --workspace cargo build.
rust_hxchat_send_dep = declare_dependency(
  sources : rust_build,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxchat_send',
  ],
)

# Rust ↔ GLib bridge (Phase R3.0 + R3.1 + R3.2). hxbridge wraps a
# C-owned GtkhxSession GObject, exposes signal-emit shims for the C
# side (R3.0), and — with the `runtime` cargo feature this meson
# build now turns on — owns the process-wide tokio runtime, the
# tokio→GLib event ferry, and the spawn_blocking_with_idle worker
# shim (R3.1 + R3.2). The first production consumer of the runtime
# pieces is banner.c's HTXF worker
# (gtkhx_bridge_spawn_blocking_with_idle), which replaced the legacy
# pthread_create + g_idle_add pair.
#
# Pulls in glib/gio 0.22 transitively, plus (now) tokio + async-channel
# from crates.io. tokio is pure Rust — no new C-side system-lib link
# surface. The gstreamer-rs crate family pinned in
# workspace.dependencies isn't consumed yet; Phase 8.B wires the
# gstreamer-webrtc bindings.
rust_hxbridge_dep = declare_dependency(
  sources : rust_build,
  # tokio's blocking-pool internals reference `pow` from libm
  # (the exponential-backoff math in its worker scheduler).
  # The main src/gtkhx binary picks libm up transitively via
  # GTK; integration tests link a tighter set that doesn't,
  # so libm is missing-DSO at link time without this. Wire it
  # in via the top-level `math_dep` (cc.find_library('m',
  # required:false)) rather than a hard-coded '-lm' link arg so
  # the dep gracefully no-ops on platforms where libm doesn't
  # need (or doesn't have) an explicit linker flag — matches
  # how the rest of the project's libm consumers handle this.
  dependencies : [math_dep],
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxbridge',
  ],
)

# GtkhxSession GObject (Phase R4). The model→view signal hub, now a
# Rust glib::subclass type. Exports the C ABI the deleted
# src/gtkhx_session.c used to: gtkhx_session_get_type,
# gtkhx_session_get_default, and one gtkhx_session_emit_* wrapper per signal.
# The staticlib has undefined references to the C boxed-type accessors
# (hx_chat_event_get_type / hx_msg_event_get_type /
# hx_tracker_server_get_type) which the linking C target resolves from
# proto_helpers.c / tracker_event.c — same leaf-up "C resolves Rust's
# externs at final link" shape the other crates use. Pulls in glib/
# gobject (already C-side deps), no new system-library surface.
rust_gtkhx_session_dep = declare_dependency(
  sources : rust_build,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lgtkhx_session',
  ],
)

# Boxed signal-payload value-types (Phase R4.2). gtkhx-boxed holds the
# GObject boxed types carried by GtkhxSession signals — all three:
# HxMsgEvent (msg.rs), HxChatEvent + HxChatMedia (chat.rs), and
# HxTrackerServer + HxTrackerV3Meta (tracker.rs). It exports the C ABI
# the deleted G_DEFINE_BOXED_TYPE registrations in proto_helpers.c /
# tracker_event.c / tracker_v3_meta.c used to (hx_*_get_type / _copy /
# _free). Self-contained (glib only): a C target that pulls a _copy /
# _free symbol links this archive alone without dragging gtkhx-session's
# still-C boxed externs in via codegen-unit merging. Both the gtkhx
# binary and the proto unit tests that link the matching producer
# (proto_helpers.c) carry this dep.
rust_gtkhx_boxed_dep = declare_dependency(
  sources : rust_build,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lgtkhx_boxed',
  ],
)

# hxtext (Phase R5) — the Rust port of src/text_util.c. Exports the four
# gtkhx_text_* C ABI symbols (to_utf8 / for_wire / the emoji-shortcode
# toggle) the ~16 C callers link against; implemented over hotline-proto's
# native text/emoji primitives. Unconditional (text encoding is always
# needed), so it's part of the default --workspace cargo build.
rust_hxtext_dep = declare_dependency(
  sources : rust_build,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxtext',
  ],
)

# gtkhx-ui GtkHx window UIs (Phase R5). The first gtk4-rs UI crate: each
# ported window exports the C ABI its deleted src/<window>.c provided.
# The Tracker window (first module) exports create_tracker_window /
# tracker_batch_begin / tracker_server_create / tracker_clear /
# tracker_search_refresh, which the C callers (toolbar.c, gtkhx.c signal
# adapters, options.c) resolve against here. The staticlib has undefined
# refs to the gtkhx_* util helpers, the bookmark API, the connect dialog,
# the tracker_bridge.c session/prefs shim, and hx_tracker_v3_meta_copy/
# _free (gtkhx-boxed) — all resolved at final link by the C objects /
# the gtkhx-boxed archive (listed AFTER this one in src/meson.build).
# Pulls in libgtk-4 / libadwaita-1 / libgio-2.0, already C-side deps, so
# the system-library surface doesn't grow.
rust_gtkhx_ui_dep = declare_dependency(
  sources : rust_build,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lgtkhx_ui',
  ],
)

# Hotline Connection actor (Phase R3.3). hxnet wraps tokio + the
# hotline-proto frame layer behind a C-callable FFI surface
# (rust/crates/hxnet/src/ffi.rs). The first consumer is the smoke
# test in tests/unit/test_hxnet_ffi.c; a later phase (R3.3.e)
# wires the production switch in network.c. hxnet shares
# hxbridge's tokio runtime
# singleton via the Runtime::global accessor, so the staticlib
# doesn't bring a second tokio binary into the link.
#
# Depends on hxbridge for the tokio runtime. Static-archive
# resolution wants the consumer (-lhxnet) listed BEFORE its
# provider (-lhxbridge) so the linker has hxnet's unresolved
# symbols on the table when it scans hxbridge — see
# tests/meson.build's `[rust_hxnet_dep, rust_hxbridge_dep]`
# ordering for the test binary.
rust_hxnet_dep = declare_dependency(
  sources : rust_build,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxnet',
  ],
)

# GStreamer voice runtime (Phase 8.B). hxvoice-runtime wraps gst::init
# and the audio device-enumeration / element-factory helpers. The
# Phase 8.B exit criterion is a clean build + Tier 2 loopback test
# passing; per-session runtime handle + state-machine wiring land in
# Phase 8.C. Pulls in the gstreamer-rs 0.25 crate family (which itself
# links the gstreamer-1.0 / -app / -audio / -sdp / -webrtc system
# libraries probed at the top-level meson.build). The system libs are
# already in `deps` on the gtkhx executable, so the link surface
# doesn't grow.
# Declared unconditionally (a declare_dependency is lazy — it only puts
# -lhxvoice_runtime on a link line when something actually consumes it).
# src/meson.build adds it to the gtkhx executable only when voice_enabled,
# which is the same condition under which cargo built the staticlib above.
rust_hxvoice_runtime_dep = declare_dependency(
  sources : rust_build,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxvoice_runtime',
  ],
)

# Per-uid voice presence model (HxVoiceModel), the Rust port of
# src/voice_model.c. Same lazy-dependency treatment as hxvoice_runtime:
# declared unconditionally but only linked when voice_enabled (src/meson.build),
# which is the same condition under which cargo built the staticlib (the crate
# is in the --exclude list for the voice-off build above). The gtkhx-ui `voice`
# modules + rcv.c / gtkhx.c / network.c / users.c resolve hx_voice_model_*
# against this archive.
rust_hxvoice_model_dep = declare_dependency(
  sources : rust_build,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxvoice_model',
  ],
)

# Voice wire-out senders (hx_send_voice_*), the Rust port of src/voice.c.
# Same lazy voice-gated treatment: excluded from the voice-off cargo build,
# linked into src/gtkhx only when voice_enabled. rcv.c + the gtkhx-ui voice
# modules resolve hx_send_voice_* against this archive.
rust_hxvoice_send_dep = declare_dependency(
  sources : rust_build,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxvoice_send',
  ],
)

# Image decoder (glycin migration G.1+). G.1 ships the sniff layer +
# FFI scaffold. G.2 lands the glycin loader (the actual decode) plus
# the async ripple in the C consumers (inline_media_dialog.c,
# chat.c::on_inline_media_autofetch_done). The link surface stays
# small on G.1 — pure-Rust sniff has no system deps; glycin adds
# libglycin-2 (already on the host via the gtk4 + gnome-platform
# install) when G.2 lands.
#
# See docs/glycin-migration-plan.md for the full phased plan.
rust_hx_image_decode_dep = declare_dependency(
  sources : rust_build,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhx_image_decode',
  ],
)
