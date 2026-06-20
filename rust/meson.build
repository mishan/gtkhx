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
#                     in docs/rust-glib-interop.md.
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
  'crates/hxnet/src/transform.rs',
  'crates/hxvoice/src/lib.rs',
  'crates/hxvoice/src/event.rs',
  'crates/hxvoice/src/action.rs',
  'crates/hxvoice/src/state.rs',
  'crates/hxvoice-runtime/src/lib.rs',
  'crates/hxvoice-runtime/src/audio.rs',
  'crates/hxvoice-runtime/src/runtime.rs',
  'crates/hxvoice-runtime/src/ffi.rs',
  'crates/hx-image-decode/src/lib.rs',
  'crates/hx-image-decode/src/sniff.rs',
  'crates/hx-image-decode/src/ffi.rs',
  'crates/hx-image-decode/src/caps.rs',
  'crates/hx-image-decode/src/decode.rs',
  'crates/hx-image-decode/src/ffi_result.rs',
  'crates/hx-image-decode/src/telemetry.rs',
  'Cargo.toml',
  'Cargo.lock',
  'crates/hxcrypto-hash/Cargo.toml',
  'crates/hxcrypto-stream/Cargo.toml',
  'crates/hxcrypto-aead/Cargo.toml',
  'crates/hxcompress/Cargo.toml',
  'crates/hotline-proto/Cargo.toml',
  'crates/hxbridge/Cargo.toml',
  'crates/hxnet/Cargo.toml',
  'crates/hxvoice/Cargo.toml',
  'crates/hxvoice-runtime/Cargo.toml',
  'crates/hx-image-decode/Cargo.toml',
)
# cargo-sources.json is regenerated from Cargo.lock for the Flatpak
# offline build; it is not consumed by `cargo build` directly. Cargo.lock
# above is the load-bearing dep-graph file that drives this custom_target's
# re-execution when versions move.

rust_target_dir = meson.project_build_root() / 'rust-target'

# Phase R3.2 turns the hxbridge `runtime` cargo feature on so the
# tokio runtime + blocking-pool shim + tokio→GLib channel ferry land
# in the staticlib linked into src/gtkhx. The first production consumer
# is banner.c's HTXF worker, which calls
# gtkhx_bridge_spawn_blocking_with_idle in lieu of pthread_create.
# Subsequent R3.x consumers (xfers.c in R3.4, hxnet Connection actor in
# R3.3) inherit the feature without needing a second flip.
cargo_features = 'hxbridge/runtime'

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
    '@0@ build --release --ignore-rust-version --manifest-path @1@ --target-dir @2@ --features @3@ && touch @4@'.format(
      cargo.full_path(),
      meson.current_source_dir() / 'Cargo.toml',
      rust_target_dir,
      cargo_features,
      '@OUTPUT@',
    ),
  ],
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
rust_hxvoice_runtime_dep = declare_dependency(
  sources : rust_build,
  link_args : [
    '-L' + rust_target_dir / 'release',
    '-lhxvoice_runtime',
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
