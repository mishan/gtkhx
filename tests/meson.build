# GtkHx test suite. See tests/PROPOSAL.md for the overall plan.
#
# Tier 1 (unit/) — pure-function tests, no GTK widgets, no network.
# Tier 2 (proto/) — protocol parsing with canned wire bytes; added later.
# Tier 3 (integration/) — end-to-end against the mhxd Docker container.
#
# Each test program is a separate binary. Meson's test() target runs
# them individually so a failure in one doesn't mask the rest. TAP
# output drops cleanly into CI test reporters.
#
# Dependencies kept minimal: glib + gtk4 (for gboolean and friends in
# headers we share with the app). Pure-pure tests can drop gtk_dep,
# but the cost is one extra link arg, not worth it for the tier-1
# scope.

test_deps = [
  glib_dep,
  gtk_dep,
]

# All crypto primitives (hash, stream cipher, AEAD, compression) live in
# the Rust hxcrypto-* / hxcompress crates since Phase R1, and the C
# dispatchers that wrapped them (cipher.c, cipher_aead.c, hope.c) plus the
# nettle-using tests are gone — so neither the tests nor protocol.h /
# compress.h pull in any C cipher library. No nettle dependency here.
proto_test_deps = test_deps + [
  # Phase R2: src/proto_helpers.c (compiled into most proto tests)
  # now calls into the Rust hotline-proto crate for the SELFINFO
  # parse, so every test that links proto_helpers.c needs the
  # staticlib. Harmless for the few proto tests that don't reference
  # the symbol — the linker drops the unused archive members.
  rust_hotline_proto_dep,
  # Phase R3.0: src/gtkhx_session.c::gtkhx_session_emit_task_update
  # delegates to gtkhx_bridge_emit_pointer_pair_signal in hxbridge.
  # The integration harness + the production-connect Tier 3 tests
  # (real_connect, real_htxf_connect) need the staticlib. Same "linker
  # drops unused members" caveat for tests that don't reference it.
  rust_hxbridge_dep,
  # Phase R4: GtkhxSession moved to the Rust `gtkhx-session` crate, so
  # the gtkhx_session_get_default + gtkhx_session_emit_* symbols the
  # real_connect / real_htxf_connect targets used to get from
  # gtkhx_session.c now come from this staticlib. Its undefined
  # references to the boxed-type accessors (hx_*_get_type) resolve
  # against proto_helpers.c / tracker_event.c, which those targets
  # also link. Archive members aren't pulled for tests that don't
  # touch a gtkhx_session symbol.
  rust_gtkhx_session_dep,
  # Phase R4.2: the boxed payload value-types (HxMsgEvent, HxChatEvent +
  # HxChatMedia, HxTrackerServer + HxTrackerV3Meta) moved out of
  # proto_helpers.c / tracker_event.c / tracker_v3_meta.c into the
  # self-contained gtkhx-boxed crate. Proto tests that link those
  # producers and exercise the boxed copy/free (e.g. test_msg_event /
  # test_chat_event call hx_*_event_copy/_free; the tracker tests'
  # meta_free call sites) resolve them against this archive. It's
  # self-contained, so pulling those symbols never drags gtkhx-session's
  # C externs in.
  rust_gtkhx_boxed_dep,
  # Phase R5: src/text_util.c was ported to the hxtext Rust crate, so the
  # gtkhx_text_to_utf8 / _for_wire sanitisers proto_helpers.c calls now
  # resolve against this staticlib instead of a compiled-in text_util.c.
  # Same "linker drops unused members" caveat for targets that don't
  # reference the symbol.
  rust_hxtext_dep,
]

test_inc = include_directories('../src')

# -----------------------------------------------------------------------------
# Tier 1 — unit/
# -----------------------------------------------------------------------------

# hl_access.h is header-only (static inline hl_access_has) so we can
# compile the test program without linking any of GtkHx's source.
test_hl_access = executable('test_hl_access',
  'unit/test_hl_access.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('hl_access', test_hl_access,
     suite : 'unit')

# text_util — the gtkhx_text_to_utf8 / _for_wire sanitisers moved to the
# hxtext Rust crate (Phase R5); their tests moved with them to
# rust/crates/hxtext/src/tests.rs (run under `cargo test`). No C test
# target here anymore.

# byte_swap — HN16 / HN32 macros from protocol.h. Header-only macros;
# protocol.h pulls in cipher.h / compress.h for the htlc_conn state
# types, but those are now header-only (the crypto/compress backends are
# Rust), so test_deps suffices. Worth it: these macros are the
# load-bearing piece of every chunk parse in rcv.c.
test_byte_swap = executable('test_byte_swap',
  'unit/test_byte_swap.c',
  dependencies        : proto_test_deps,
  include_directories : test_inc,
)
test('byte_swap', test_byte_swap,
     suite : 'unit')

# proto_names — proto_hdr_name / proto_data_name from proto_trace.c
# map opcode integers to the symbolic names we print in [proto]
# traces. Compile proto_trace.c and debug.c in directly (debug is
# pulled in as a transitive symbol via debug_log calls inside
# proto_trace.c that the linker still needs to resolve, even though
# the two name functions don't call it themselves).
#
# proto_trace.c was deliberately decoupled from hx.h so we can compile
# it here without GTK / Adwaita; it only needs protocol.h + glib.
test_proto_names = executable('test_proto_names',
  'unit/test_proto_names.c',
  '../src/proto_trace.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : test_inc,
)
test('proto_names', test_proto_names,
     suite : 'unit')

# text_sanitisers — CR2LF / LF2CR (compat.h macros) and strip_ansi
# (protocol.h static inline). Header-only, but strip_ansi's home is
# protocol.h, so the test pulls in proto_test_deps.
test_text_sanitisers = executable('test_text_sanitisers',
  'unit/test_text_sanitisers.c',
  dependencies        : proto_test_deps,
  include_directories : test_inc,
)
test('text_sanitisers', test_text_sanitisers,
     suite : 'unit')

# hmac — the HOPE handshake's MAC dispatcher. Phase R1 moved the
# implementation to Rust (rust/crates/hxcrypto-hash); protocol.h
# now exposes hmac_xxx as a static inline that delegates to
# gtkhx_hmac_xxx. This binary still tests the dispatch glue + the
# RFC 2104 / RFC 2202 canonical vectors + the legacy unprefixed
# key||text construction; the test's call sites are unchanged
# because the shim preserves the signature.
test_hmac = executable('test_hmac',
  'unit/test_hmac.c',
  dependencies        : proto_test_deps + [rust_hxcrypto_hash_dep],
  include_directories : test_inc,
)
test('hmac', test_hmac,
     suite : 'unit')

# path_util — path_basename(path, sep). Pure GLib, no globals
# (the dir_char-aware dirchar_basename in files.c is now a thin
# wrapper around this). Compile path_util.c in directly.
test_path_util = executable('test_path_util',
  'unit/test_path_util.c',
  '../src/path_util.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('path_util', test_path_util,
     suite : 'unit')

# safe_local_basename — sanitizer that defangs a remote-supplied
# file name before joining it onto the local download path.
# Drives the path-traversal defense lives in
# hx_files_provider_safe_local_basename (files_provider.c). The
# function itself is dependency-free string ops, but the test pulls
# in files_provider.c which uses GObject for the iface definition.
test_safe_local_basename = executable('test_safe_local_basename',
  'unit/test_safe_local_basename.c',
  '../src/files_provider.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('safe_local_basename', test_safe_local_basename,
     suite : 'unit')

# pict_embed — sniff-an-embedded-JPEG/PNG/GIF/TIFF helper used by
# preview.c::image_done as a fallback when GdkPixbuf can't decode a
# PICT directly. Pure GLib bytes-in / GBytes-out; the test inputs
# are synthetic PICT-shaped buffers (zero header + opcode bytes +
# image signature + payload), so no real PICT files are needed.
test_pict_embed = executable('test_pict_embed',
  'unit/test_pict_embed.c',
  '../src/pict_embed.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('pict_embed', test_pict_embed,
     suite : 'unit')

# prefs_parser — prefs_parse_boolean. The BOOLEAN-pref logic was
# extracted out of options.c specifically for this test (options.c
# pulls in GTK + Adwaita + the entire UI tree; prefs_parser.c is
# pure GLib). Regression net for the GKeyFile true/false bug we
# already shipped a fix for.
test_prefs_parser = executable('test_prefs_parser',
  'unit/test_prefs_parser.c',
  '../src/prefs_parser.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('prefs_parser', test_prefs_parser,
     suite : 'unit')

# Per-area UI scale model (gtkhx_theme.c). Pure GObject + gtkhx_prefs;
# the test provides its own gtkhx_prefs instead of linking the
# GTK-heavy options.c.
test_theme_scale = executable('test_theme_scale',
  'unit/test_theme_scale.c',
  '../src/gtkhx_theme.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('theme_scale', test_theme_scale,
     suite : 'unit')

# cfgvars[] sort-order regression test. The table in src/options.c
# must be alphabetical by on-disk key string because cfgvar_for_name
# uses bsearch() on it; there's a runtime assertion that g_errors at
# launch, but a launch-time error is a "user discovers the bug" path
# rather than a "CI discovers the bug" path. options.c is too
# GTK-heavy to link into a unit test, so the test parses the source
# text directly (src/cfgkeys.h + src/options.c) and walks the table
# order. Source root passes through as argv[1].
test_cfgvars_sorted = executable('test_cfgvars_sorted',
  'unit/test_cfgvars_sorted.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('cfgvars_sorted', test_cfgvars_sorted,
     args : [meson.project_source_root()],
     suite : 'unit')

# Theme discovery (gtkhx_theme_list_available_at). Drives the
# user-dir walk with tmp-dir fixtures, and the GResource walk
# against the live built-ins (default + solarized) by linking
# gtkhx_resources into the test target — the resource's
# generated constructor self-registers at process load so the
# built-ins surface through g_resources_enumerate_children.
test_theme_listing = executable('test_theme_listing',
  'unit/test_theme_listing.c',
  '../src/gtkhx_theme.c',
  gtkhx_resources,
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('theme_listing', test_theme_listing,
     suite : 'unit')

# Icon resolver (gtkhx_icon_load + cache invalidation). Drives the
# theme-bundle lookup with tmp-dir PNG fixtures and the stock-pixmap
# fallback via the linked-in gtkhx_resources (so e.g. "tasks" resolves
# to the shipped pixmap), and checks the cache hands back the same
# pixbuf until invalidated.
test_icon_pack = executable('test_icon_pack',
  'unit/test_icon_pack.c',
  '../src/gtkhx_icon.c',
  '../src/gtkhx_theme.c',
  gtkhx_resources,
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('icon_pack', test_icon_pack,
     suite : 'unit')

# hxnet_ffi — smoke test for the hxnet C-callable FFI surface
# (Phase R3.3.b). Builds a connected TCP loopback pair
# (bind/listen/accept + connect on 127.0.0.1) → spawn_fd → round-
# trip a Hotline frame + bytes both directions → assert EOF
# surfaces as HXNET_SHUTDOWN_EOF. The TCP loopback (rather than
# AF_UNIX socketpair) matches what production passes: the FFI
# wraps the fd as tokio::net::TcpStream and a peer_addr probe
# rejects non-TCP sockets up front. Links the Rust hxnet
# staticlib directly via rust_hxnet_dep; transitively pulls in
# hxbridge for the shared tokio runtime. No C-side production
# source is linked — the test exercises the FFI in isolation.
test_hxnet_ffi = executable('test_hxnet_ffi',
  'unit/test_hxnet_ffi.c',
  dependencies        : test_deps + [rust_hxnet_dep, rust_hxbridge_dep],
  include_directories : test_inc,
)
test('hxnet_ffi', test_hxnet_ffi,
     suite : 'unit')

# htxf_cancel — the HTXF cooperative-cancel C shim (Phase R3 X1). Drives
# the real htxf_io.c shim + the hxnet abort token over a TCP loopback
# pair: the canceled-flag boundary check, abort waking a parked transport
# read, ECANCELED reclassification, NULL-safety, and the token lifecycle
# under ASan. Links htxf_io.c + debug.c (for debug_log) + the hxnet
# staticlib; no GUI tree. The full xfers.c worker path is covered by the
# Tier 3 transfer matrix on the happy path.
test_htxf_cancel = executable('test_htxf_cancel',
  'unit/test_htxf_cancel.c',
  '../src/htxf_io.c',
  '../src/debug.c',
  dependencies        : test_deps + [rust_hxnet_dep, rust_hxbridge_dep],
  include_directories : test_inc,
)
test('htxf_cancel', test_htxf_cancel,
     suite : 'unit')

# hxnet_bridge — translation layer between hxnet's HxnetFrame
# callback shape and the existing rcv state machine in src/rcv.c.
# Phase R3.3.e-4a ships only the header-packing leg as a Tier 1
# test; the full dispatch path through htlc_conn lives behind
# the production refactor that R3.3.e-4b will land. Links the
# bridge source + proto_helpers.c (for hl_hdr_decode round-trip
# verification) without dragging the GUI tree.
test_hxnet_bridge = executable('test_hxnet_bridge',
  'unit/test_hxnet_bridge.c',
  '../src/hxnet_bridge.c',
  '../src/host_port.c',
  '../src/proto_helpers.c',
  dependencies        : test_deps + [rust_hotline_proto_dep],
  include_directories : test_inc,
)
test('hxnet_bridge', test_hxnet_bridge,
     suite : 'unit')

# bookmarks — HTsc on-disk format + CRUD module. The CRUD lives in
# bookmarks_io.c (carved out of bookmarks.c, which still holds the
# GTK / libadwaita dialog) so this test can drive it without the
# whole UI tree. The test provides its own gtkhx_config_dir() stub
# so each test sees a fresh tmpdir for primary + legacy bookmark
# directories.
test_bookmarks = executable('test_bookmarks',
  'unit/test_bookmarks.c',
  '../src/bookmarks_io.c',
  '../src/host_port.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('bookmarks', test_bookmarks,
     suite : 'unit')

# bookmark_cipher — stable byte ↔ cipher-name vocabulary used by
# the HTsc bookmark file format. Independent of the UI dropdown
# (valid_ciphers[]) so reordering the dropdown doesn't shift the
# meaning of a saved byte. Pins the byte assignments numerically
# so a renumber breaks here before it breaks every bookmark on
# disk. RC4 keeps its slot named even though the cipher itself
# was retired in claude/remove-rc4 — the migration dialog needs
# string equality on this slot to detect a legacy RC4 bookmark.
test_bookmark_cipher = executable('test_bookmark_cipher',
  'unit/test_bookmark_cipher.c',
  '../src/bookmark_cipher.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('bookmark_cipher', test_bookmark_cipher,
     suite : 'unit')

# hotline_url — parser for the hotline:// URL scheme. Drives the
# Connect / Save Bookmark entries on the gtkurl right-click popup,
# so the shapes pinned here come from real paste-from-chat inputs
# (login:pass@host:port + URL-decoded segments + IPv6 brackets).
# hotline_url.c is pure-GLib so the test links nothing else.
test_hotline_url = executable('test_hotline_url',
  'unit/test_hotline_url.c',
  '../src/hotline_url.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('hotline_url', test_hotline_url,
     suite : 'unit')

# compress_bufsize — Phase R1 round 3 fix for the
# compress_encode_bufsize overflow guard. Pins that the function
# returns 0 when 2*len + 1024 would overflow u32 (rather than
# wrapping around silently and producing an undersized allocation).
# compress_encode_bufsize is defined `static inline` in compress.h
# specifically so this test can drive it without linking compress.c
# (which transitively pulls GTK via hx.h).
test_compress_bufsize = executable('test_compress_bufsize',
  'unit/test_compress_bufsize.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('compress_bufsize', test_compress_bufsize,
     suite : 'unit')

# task_hash — the Phase 1.1 GLib-collections migration replaced the
# per-session task linked list (the intrusive __task_list / task_list
# / task_tail trio) with GHashTable<u32 trans, struct task*>. The
# table factory lives in tasks_table.c specifically so this test can
# build it without dragging in GTK; pins down the lifecycle contract
# (insert / lookup / replace / remove / iterate / destroy → value
# destroy notify reclaims the struct task + its `str` label).
test_task_hash = executable('test_task_hash',
  'unit/test_task_hash.c',
  '../src/tasks_table.c',
  dependencies        : proto_test_deps,
  include_directories : test_inc,
)
test('task_hash', test_task_hash,
     suite : 'unit')

# dock_layout_parse — recursive-descent parser for the saved-layout
# tree expression. Pure GLib so the test links just the parser .c
# without dragging in GTK + libpanel + the rest of dock_layout.c.
# Covers valid trees, whitespace tolerance, malformed input
# variants, and the empty-leaf-with-role case (the all-undocked-
# layout edge that broke before the role-default fix).
test_dock_layout_parse = executable('test_dock_layout_parse',
  'unit/test_dock_layout_parse.c',
  '../src/dock_layout_parse.c',
  dependencies        : proto_test_deps,
  include_directories : test_inc,
)
test('dock_layout_parse', test_dock_layout_parse,
     suite : 'unit')

# msgwin_hash — Phase 1.2 replaces the doubly-linked msg_list (a
# file-scope global in msg.c) with GHashTable<u16 uid, struct msgwin*>
# on the session. struct msgwin lives in session.h, which pulls in
# gtk + adwaita, so this test exercises a layout-compatible stub
# rather than linking msg.c. The integration suite covers the real
# msgwin path end-to-end against mhxd.
test_msgwin_hash = executable('test_msgwin_hash',
  'unit/test_msgwin_hash.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('msgwin_hash', test_msgwin_hash,
     suite : 'unit')

# chat_hash — Phase 1.3 replaces the chat_front / chat_tail /
# chat_list / __chat_list sentinel quartet on the session with
# GHashTable<u32 cid, struct chat*>. struct chat sits alongside
# gtk-dependent types in session.h, so the test uses a layout-
# compatible stub; the integration suite covers the real path.
test_chat_hash = executable('test_chat_hash',
  'unit/test_chat_hash.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('chat_hash', test_chat_hash,
     suite : 'unit')

# gchat_hash — Phase 1.4 replaces session->gchat_list (the doubly-
# linked list of open chat / pchat UI windows) with
# GHashTable<u32 cid, struct gtkhx_chat*>. Same layout-stub approach
# as chat_hash. Focuses on the public-chat-coexists-with-pchats
# patterns specific to the gchats table.
test_gchat_hash = executable('test_gchat_hash',
  'unit/test_gchat_hash.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('gchat_hash', test_gchat_hash,
     suite : 'unit')

# algo_list — HOPE algorithm-list walker (list_n). Pinned-input
# crash defenses called out by name in the HOPE-Secure-Login spec —
# empty list (count=0, listlen=2) used to NULL-deref the legacy
# implementation, killing gtkhx on Mobius servers.
test_algo_list = executable('test_algo_list',
  'unit/test_algo_list.c',
  '../src/algo_list.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('algo_list', test_algo_list,
     suite : 'unit')

# uniquify_path — "foo.txt" → "foo (1).txt" collision resolver.
# Refactored from xfers.c's static uniquify_local_path to take an
# exists predicate callback (production caller plugs in stat + Apple
# Double resource fork detection; the test plugs in a GHashTable
# lookup). Pins the extension-insertion vs. trailing-append shapes
# (foo.txt vs README vs .bashrc) plus the counter advance behaviour.
test_uniquify_path = executable('test_uniquify_path',
  'unit/test_uniquify_path.c',
  '../src/uniquify_path.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('uniquify_path', test_uniquify_path,
     suite : 'unit')

# human_readable — fileutils-vintage byte-count formatter
# (human_size / human_readable). Extracted to src/human_readable.c
# so this test can link without dragging in files.c's GTK pile.
# Pins the user-visible suffix boundaries (1023 → "1023", 1024 →
# "1.0k", 8500 → "8.3k", 4294967295u → "4.0G"). Catches any future
# tidy-up of the .5-rounding state machine that would change what
# the user sees in the tasks window / file browser size column.
test_human_readable = executable('test_human_readable',
  'unit/test_human_readable.c',
  '../src/human_readable.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('human_readable', test_human_readable,
     suite : 'unit')

# htxf_hdr — 16-byte HTXF subchannel handshake header. Pure
# layout test: re-encode the magic + ref + len + (type/reserved)
# fields and pin the bytes. Catches the "Oni Tracks folder hang"
# regression (always-zero type field) and any future struct
# rearrangement. Pulls in proto_test_deps because proto_helpers.c
# needs the Rust hotline-proto staticlib.
test_htxf_hdr = executable('test_htxf_hdr',
  'unit/test_htxf_hdr.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : test_inc,
)
test('htxf_hdr', test_htxf_hdr,
     suite : 'unit')

# path_hldir — path_to_hldir + dirmask. Pure path encoders extracted
# from files.c so this test can link them without dragging in GTK /
# Adwaita / files.c's UI globals. path_to_hldir references an extern
# `dir_char` (set by the server's DIRECTORYCHAR handshake at run
# time); the test provides its own definition.
test_path_hldir = executable('test_path_hldir',
  'unit/test_path_hldir.c',
  '../src/path_hldir.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('path_hldir', test_path_hldir,
     suite : 'unit')

# hl_date — Hotline 8-byte timestamp decoder. Both wire formats
# (Mac 1904 epoch and Capabilities.md's "modern" format) are
# auto-detected via the year field. Tests pin Unix-time decoding
# for each format, plus the all-zero sentinel and out-of-range
# year refusals.
test_hl_date = executable('test_hl_date',
  'unit/test_hl_date.c',
  '../src/hl_date.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('hl_date', test_hl_date,
     suite : 'unit')

# hl_code — Hotline's XOR-with-0xff "encryption" of LOGIN / PASSWORD
# chunks. Extracted to its own translation unit (hl_code.c) so the
# test can link the real implementation without dragging in
# network.c's GSocket / thread / cipher / compress pile.
test_hl_code = executable('test_hl_code',
  'unit/test_hl_code.c',
  '../src/hl_code.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('hl_code', test_hl_code,
     suite : 'unit')

# tls_trust — TLS Phase 3 TOFU trust DB. Pure GLib/GIO (SHA-256
# fingerprint cache in SSH known_hosts shape). Tier 1 drives the
# pin/lookup/MISMATCH semantics against a tmpdir via the
# GTKHX_KNOWN_HOSTS env override; the dialog UX layer
# (tls_trust_dialog) is Adwaita-only and exercised via Tier 3 with
# GTKHX_TLS_AUTO_ACCEPT=1 instead of GUI scripting.
test_tls_trust = executable('test_tls_trust',
  'unit/test_tls_trust.c',
  '../src/tls_trust.c',
  '../src/host_port.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('tls_trust', test_tls_trust,
     suite : 'unit')

# host_port — the shared "host:port" parse/join helper (IPv6-aware),
# replacing the hand-rolled strrchr(':') splits that mis-handled IPv6
# literals across bookmarks_io / connect / tls_trust / network.
test_host_port = executable('test_host_port',
  'unit/test_host_port.c',
  '../src/host_port.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('host_port', test_host_port,
     suite : 'unit')

# hx_user_hash — Phase 1.5 replaces the intrusive hx_user linked
# list per chat (chat->__user_list / user_list / user_tail with
# next/prev fields on each hx_user) with chat->users, a
# GHashTable<u16 uid, struct hx_user*>. Layout-stub approach;
# the integration suite covers the real path against mhxd.
test_hx_user_hash = executable('test_hx_user_hash',
  'unit/test_hx_user_hash.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('hx_user_hash', test_hx_user_hash,
     suite : 'unit')

# Voice unit tests are built only when voice is compiled in (-Dvoice /
# the voice_enabled boolean from the top-level meson.build) — they
# compile the voice_*.c sources that are excluded from the binary when
# voice is off.
if voice_enabled
# voice_ptt_keyspec — PTT key-spec vocabulary + canonicalisation.
# Pure functions over GDK keysyms; the unit test drives them
# without any GTK widget construction. gtk_init_check is called so
# the gtk_accelerator_name / _parse symbol tables are populated;
# headless CI works because we use _check (not _init).
test_voice_ptt_keyspec = executable('test_voice_ptt_keyspec',
  'unit/test_voice_ptt_keyspec.c',
  '../src/voice_ptt_keyspec.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('voice_ptt_keyspec', test_voice_ptt_keyspec,
     suite : 'unit')

# voice_model — the indicator-policy + signal-emission contract moved to the
# hxvoice-model Rust crate along with the port (voice_model.c retired). Its
# tests run under `cargo test -p hxvoice-model`, so there's no C target here.
endif # voice_enabled

# inline_media_decode — Phase 9.B bounded image decoder.
# Sniff cases over hand-crafted prefixes pin the JPEG / PNG / GIF
# allowlist and the explicit-reject set (SVG / WebP / AVIF / HEIC /
# TIFF / BMP / ICO).
# Sniff-only since the glycin migration G.2: the decode-layer
# test cases moved to the Rust crate (cargo test -p
# hx-image-decode), this target now exercises the C-side FFI
# shim against the same magic-byte fixtures the legacy C impl
# had. The block-comment "Phase 9.B bounded image decoder"
# above still applies to the *binary's purpose* (gating the
# inline-media decoder's allowlist) but the implementation
# layer it covers has narrowed.
test_inline_media_decode = executable('test_inline_media_decode',
  'unit/test_inline_media_decode.c',
  '../src/inline_media_decode.c',
  '../src/debug.c',
  dependencies        : test_deps + [
    rust_hx_image_decode_dep,
    # G.2 glycin transitive sys deps (see notes in meson.build's
    # `deps` array — same reason here for the test binary). The
    # in-process glycin loader needs the seccomp + fontconfig +
    # lcms2 surface even though our sniff-only tests don't
    # actually invoke decode.
    fontconfig_dep,
    libseccomp_dep,
    lcms2_dep,
  ],
  include_directories : test_inc,
)
test('inline_media_decode', test_inline_media_decode,
     suite : 'unit')

# -----------------------------------------------------------------------------
# Tier 2 — proto/
#
# Protocol-parser tests that drive a fake struct htlc_conn (with an
# in.buf the wire_fixture builder packed by hand) through helpers
# extracted from rcv.c / tasks.c into proto_helpers.c.
# -----------------------------------------------------------------------------

# Shared wire-fixture builder used by every Tier 2 test. Compile it
# in via a static library so individual test binaries don't each
# carry their own copy of the object code.
proto_test_inc = include_directories('proto', '../src')

wire_fixture_lib = static_library(
  'wire_fixture',
  'proto/wire_fixture.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
)

test_task_error = executable('test_task_error',
  'proto/test_task_error.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('task_error', test_task_error,
     suite : 'proto')

# selfinfo — hx_selfinfo_parse extracts the access bitmap, UID, icon,
# and display name from a HTLS_HDR_USER_SELFINFO payload. The Phase
# 5 post-login state machine fires off USER_GETLIST + news on this
# message, so getting the parse wrong means downstream gating
# (READ_NEWS, DISCONNECT_USERS) compares against the wrong values.
test_selfinfo = executable('test_selfinfo',
  'proto/test_selfinfo.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('selfinfo', test_selfinfo,
     suite : 'proto')

# chat — hx_chat_extract pulls body + chat-id + uid out of an
# HTLS_HDR_CHAT message and runs the same CR2LF / strip_ansi /
# leading-LF strip the original handler does, so the chat widget
# never sees raw wire bytes.
test_chat_proto = executable('test_chat_proto',
  'proto/test_chat.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('chat', test_chat_proto,
     suite : 'proto')

# msg — hx_msg_extract pulls uid + sender name + body out of an
# HTLS_HDR_MSG (private message) frame. The handler treats name and
# body asymmetrically: both go through strip_ansi but only the body
# gets CR2LF (since names shouldn't have line endings). Pin that
# down so a future "consistency" pass that adds CR2LF to names
# trips the test.
test_msg_proto = executable('test_msg_proto',
  'proto/test_msg.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('msg', test_msg_proto,
     suite : 'proto')

# banner — hx_banner_extract pulls the 4-byte type code and optional
# URL out of an HTLS_HDR_BANNER message. Type chunk is pinned to
# exactly 4 bytes by the protocol; wrong sizes are refused so the
# downstream "URL " vs "JPEG"/"GIFf"/"PICT" dispatch in banner.c
# only ever compares against well-formed type codes.
test_banner = executable('test_banner',
  'proto/test_banner.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('banner', test_banner,
     suite : 'proto')

# banner_dispatch — pure URL-vs-file mode classifier + HTXF reply
# sanity checker carved out of banner.c so this test can drive the
# dispatch decisions without dragging in GTK / libsoup / pthread.
# Two helpers, ~30 lines each, both load-bearing in the wire-mode
# routing path that the live Tier 3 banner tests exercise
# downstream.
test_banner_dispatch = executable('test_banner_dispatch',
  'proto/test_banner_dispatch.c',
  '../src/banner_dispatch.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('banner_dispatch', test_banner_dispatch,
     suite : 'proto')

# tracker_parser — pure HTRK reply parsers carved out of
# network.c's async tracker fetch state machine. Four small
# helpers (reply header / padding sentinel / fixed record / text
# normalize), each pinned against canned byte fixtures. The
# byte ordering invariants and the reserved-bytes-ignored
# contract are the bits most worth a regression net.
test_tracker_parser = executable('test_tracker_parser',
  'proto/test_tracker_parser.c',
  '../src/tracker_parser.c',
  dependencies        : proto_test_deps,
  include_directories : test_inc,
)
test('tracker_parser', test_tracker_parser,
     suite : 'proto')

# tracker_v3 — pure encoders / parsers for the Hotline tracker v3
# wire format (fogWraith spec). Phase A floor: handshake encoder,
# 6/8-byte response detection, listing-request encoder, response
# header, variable-length server records (IPv4 / IPv6 / hostname),
# TLV walker. Pinned against synthesized byte fixtures so we don't
# need a live (or mock) v3 tracker to catch regressions.
test_tracker_v3 = executable('test_tracker_v3',
  'proto/test_tracker_v3.c',
  '../src/tracker_v3.c',
  dependencies        : proto_test_deps,
  include_directories : test_inc,
)
test('tracker_v3', test_tracker_v3,
     suite : 'proto')

# tracker_v3_meta — Tier 2 coverage for the typed TLV-trailer
# accessor in src/tracker_v3_meta.c (Phase B of tracker v3).
# Pins one canonical decode per TLV type (string, u8, u16, i16,
# u32, bool), the closed-vocab clamping for MATURITY and
# LISTING_CATEGORY, the unknown-id forward-compat skip, malformed-
# input rejection, UTF-8 sanitisation, deep-copy semantics. Pure
# function under test; no GIO / no network.
test_tracker_v3_meta = executable('test_tracker_v3_meta',
  'proto/test_tracker_v3_meta.c',
  '../src/tracker_v3.c',
  '../src/tracker_v3_meta.c',
  dependencies        : proto_test_deps,
  include_directories : test_inc,
)
test('tracker_v3_meta', test_tracker_v3_meta,
     suite : 'proto')

# hlwrite — round-trip the wire packer (hlpack, extracted from
# hlwrite() in network.c) through the dh_start walker. Proves the
# send-side encoder and the receive-side decoder agree on the wire
# format end-to-end.
test_hlwrite = executable('test_hlwrite',
  'proto/test_hlwrite.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  '../src/login_packet.c',
  '../src/hl_code.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
)
test('hlwrite', test_hlwrite,
     suite : 'proto')

# small_handlers — three small chunk-walkers (user_part, chat_subject,
# chat_invite) batched into one binary. Same shape, no per-handler
# complexity to justify separate test programs.
test_small_handlers = executable('test_small_handlers',
  'proto/test_small_handlers.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('small_handlers', test_small_handlers,
     suite : 'proto')

# user_change — five-chunk handler covering user joins / renames /
# icon and colour changes in the chat user list. The optional
# COLOUR chunk + got_color flag is the contract worth pinning down.
test_user_change = executable('test_user_change',
  'proto/test_user_change.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('user_change', test_user_change,
     suite : 'proto')

# xfer_queue — REF + QUEUE chunks during file-transfer queueing.
# queueid 0 is the "ready to start" signal.
test_xfer_queue = executable('test_xfer_queue',
  'proto/test_xfer_queue.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('xfer_queue', test_xfer_queue,
     suite : 'proto')

# news_post — hx_news_post_walk callback API. The previous handler
# maintained a dead-code accumulator with cross-call leakage; this
# test pins down the simpler "sanitise per chunk, emit per chunk"
# semantics the cleanup committed to.
test_news_post = executable('test_news_post',
  'proto/test_news_post.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('news_post', test_news_post,
     suite : 'proto')

# agreement — HTLS_HDR_AGREEMENT handler. Three-way result:
# OK / NONE / NOT_FOUND. The order-sensitive cases (NOAGREEMENT
# before AGREEMENT wins) are the contract worth pinning down.
test_agreement = executable('test_agreement',
  'proto/test_agreement.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('agreement', test_agreement,
     suite : 'proto')

# news_file — one-shot HTLS_HDR_NEWS_FILE response (the news_file
# fetched via HTLC_HDR_NEWS_GETFILE). First-NEWS-chunk-wins; same
# truncation + sanitisation contract as the other text extractors.
test_news_file = executable('test_news_file',
  'proto/test_news_file.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('news_file', test_news_file,
     suite : 'proto')

# (test_news_dirlist / test_newscat removed — the C parse shims they exercised
# (hx_news_dirlist_parse_* / hx_newscat_parse) were retired when the 1.5 news
# receive path moved to owned handles; the parsers are covered by hotline-proto's
# native cargo tests, e.g. parse::tests::dirlist_* and catlist_*.)

# chat_event — hx_chat_event_new is the parsed value-object the
# GtkhxSession::chat signal carries to chat.c and notify.c. Pins
# the UTF-8 fix-up, "[hx]" info-prefix detect, sender/body split,
# and is_self compare so a future refactor of any one step doesn't
# silently de-sync the consumers. Doesn't use the wire fixture —
# the constructor takes already-sanitised body bytes, not a wire
# frame.
test_chat_event = executable('test_chat_event',
  'proto/test_chat_event.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
)
test('chat_event', test_chat_event,
     suite : 'proto')

# filelist_walker — hl_filelist_walk extracted from
# files_remote_provider.c::populate_from_chunks. Callback-based
# iterator over a packed run of hl_filelist_hdr chunks. Pins per-
# entry stride + ntohl decode of ftype/fsize/fnlen + the
# truncated-buffer defense the new helper added.
test_filelist_walker = executable('test_filelist_walker',
  'proto/test_filelist_walker.c',
  '../src/filelist_walker.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
)
test('filelist_walker', test_filelist_walker,
     suite : 'proto')

# folder_xfer — wire-format lockdown for the 1.5 folder-transfer
# opcodes (HTLC_HDR_FILE_GETFOLDER 0xd2, HTLC_HDR_FILE_PUTFOLDER 0xd5)
# plus the HTLS task-reply chunks (HTXF_REF / HTXF_SIZE / QUEUE /
# FILE_NFILES) that rcv_task_folder_get / rcv_task_folder_put walk.
# Two halves in one binary: hlpack the request, dh_start the reply.
# Pins the magic opcode + chunk IDs and the u32 dh_getint contract
# for FILE_NFILES (the chunk we added with the folder-transfer
# feature on this branch).
test_folder_xfer = executable('test_folder_xfer',
  'proto/test_folder_xfer.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('folder_xfer', test_folder_xfer,
     suite : 'proto')

# capabilities — DATA_CAPABILITIES (0x01f0) negotiation per the
# fogWraith/Hotline Capabilities.md spec. Send-side: hlpack a LOGIN
# with our cap bits and verify the chunk layout. Recv-side: hand-
# build a LOGIN reply with a CAPABILITIES echo (2-byte and 8-byte
# variable-width forms) and verify the variable-width decoder
# extracts the right u64 caps mask. Pins the bit constants so
# renumbering breaks the test before it breaks the wire.
test_capabilities = executable('test_capabilities',
  'proto/test_capabilities.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('capabilities', test_capabilities,
     suite : 'proto')

# chat_history — fogWraith Capabilities-Chat-History.md extension.
# Pins the field-ID and flag-bit numeric constants, exercises the
# packed-binary HxHistoryEntry parser (typical / minimum / flag /
# sub-field-skip / malformed inputs) and the request-sender
# hx_get_chat_history (cap-gate refusal, bare request, before /
# after / range cursors, header type).
test_chat_history = executable('test_chat_history',
  'proto/test_chat_history.c',
  '../src/chat_history.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('chat_history', test_chat_history,
     suite : 'proto')

# voice — fogWraith voice-chat extension receive/parse FFI ABI (Phase 8.A).
# Pins the field IDs (DATA_VOICE_SDP/ICE/CODEC/MUTED/PARTICIPANTS at
# 0x01F5-0x01F9), the 600-606 opcode block, and the FFI ABI of the typed
# reply / participant / mid-label / SDP-summary / ICE-JSON extractors +
# builder. The send wrappers (hx_send_voice_*) moved to the hxvoice-send
# Rust crate along with src/voice.c; their cap-gate + wire-shape cases run
# under `cargo test -p hxvoice-send`, so this no longer compiles voice.c.
# Source: fogWraith Capabilities-Voice.md.
if voice_enabled
test_voice = executable('test_voice',
  'proto/test_voice.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('voice', test_voice,
     suite : 'proto')
endif # voice_enabled

# inline_media — fogWraith inline-media extension wire shape (Phase 9.A).
# Pins HTLC_CAP_INLINE_MEDIA (bit 3 = 0x0008), the 0x0201-0x0212 field
# IDs, the 750 / 751 opcode block, the spec-recommended default cap
# values, the CAP gate on the send wrappers, the limits-fallback
# accessors, and the FFI ABI of every gtkhx_proto_build_* / parse_* /
# extract_* entry point against hand-packed messages. Source:
# fogWraith Capabilities-Inline-Media.md.
test_inline_media = executable('test_inline_media',
  'proto/test_inline_media.c',
  '../src/inline_media.c',
  '../src/inline_media_upload.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('inline_media', test_inline_media,
     suite : 'proto')

# large_file — Capabilities-Large-File extension. Pins the 64-bit
# field IDs (DATA_FILESIZE64 / OFFSET64 / XFERSIZE64 /
# FOLDER_ITEM_COUNT64), the HTXF handshake flag constants
# (HTXF_FLAG_LARGE_FILE / HTXF_FLAG_SIZE64), the send/recv byte
# shapes for the 64-bit XFERSIZE companion, and the three handshake
# variants (legacy 16-byte, large-file < 4 GiB, large-file > 4 GiB
# with the 24-byte 8-byte-length appendix). Source:
# fogWraith/Hotline Docs/Protocol/Capabilities-Large-File.md.
test_large_file = executable('test_large_file',
  'proto/test_large_file.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('large_file', test_large_file,
     suite : 'proto')

# msg_event — HxMsgEvent is the private-message counterpart to
# HxChatEvent, payload of GtkhxSession::msg. The constructor's
# contract: stash uid + set is_broadcast on uid==0, UTF-8-sanitise
# name + body once, stamp is_self on byte-equal name vs. self_nick.
# Pins each step so msg.c and notify.c stay in agreement.
test_msg_event = executable('test_msg_event',
  'proto/test_msg_event.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
)
test('msg_event', test_msg_event,
     suite : 'proto')


# NOTE: the former Tier-2 test_htxf_io target is gone. It exercised the
# hand-rolled C AEAD byte pump in htxf_io.c (plaintext / AEAD round-trip,
# multi-frame sequencing, oversized-write rejection, truncated-frame /
# tag-tamper paths). That framing moved into the hxnet crate during the
# HTXF→Rust H2 re-wire (rust/crates/hxnet/src/htxf.rs), where the same
# cases are covered by unit tests (passthrough_roundtrip,
# aead_single_frame_roundtrip, aead_multi_frame_and_partial_reads,
# aead_tampered_frame_errors, aead_truncated_frame_errors) plus an
# FFI-over-a-real-socket integration test. htxf_io.c is now a thin
# errno-translating shim with nothing framing-specific left to unit-test
# in C.

# -----------------------------------------------------------------------------
# Tier 3 — integration/
#
# Real-server protocol-level tests. Each test opens a TCP connection
# to the configured Hotline server (mhxd Docker container at
# localhost:5500 by default; override with GTKHX_TEST_HOST /
# GTKHX_TEST_PORT) and drives the wire protocol end-to-end.
#
# Server-unreachable is a hard failure: the test calls
# g_test_fail_printf with a diagnostic message and returns -1.
# (Earlier revisions called g_test_skip here for the
# no-Docker-installed case; silent skips masked real bugs that
# only showed up later in coverage reports, so they were removed.
# Bring the Docker matrix up before running the integration suite,
# or pass `--no-suite integration` to meson test to skip the
# whole tier if you don't have Docker available.)
#
# Bring up a server for these tests with:
#   cd tests/mhxd && docker build -t gtkhx-mhxd .
#   docker run --rm -p 5500:5500 gtkhx-mhxd
#
# See tests/mhxd/README.md for details.
# -----------------------------------------------------------------------------

integration_test_inc = include_directories('integration', '../src')

# Shared harness lib — opens the socket, runs the magic handshake,
# provides blocking send / recv primitives, and (via proto_helpers
# bundled in) packs / parses Hotline messages on top of the wire.
# Tests link this lib and layer login / chat / file-list flows on
# top.
#
# Production C files compiled in (rather than re-implemented in
# harness translation units) so the test path drives the same wire
# encoders the production binary does — no test-only forks. Today
# that's hl_code.c (XOR encoder for LOGIN/PASSWORD chunks); other
# extractions are tracked in the post-merge follow-up.
integration_harness_lib = static_library(
  'integration_harness',
  'integration/integration_harness.c',
  'integration/server_matrix.c',
  'integration/tracker_matrix.c',
  '../src/proto_helpers.c',
  '../src/debug.c',
  '../src/hl_code.c',
  '../src/login_packet.c',
  '../src/agreement_packet.c',
  '../src/chat_history.c',
  # The Tier 3 HOPE login + HTXF transfers run through the production
  # orchestrator (Rust owns all the crypto now); the C cipher dispatchers
  # (cipher.c stream, cipher_aead.c AEAD) are gone. htxf_io.c +
  # htxf_subchannel.c remain as the thin C shim / preamble packer over
  # the Rust HTXF channel.
  '../src/htxf_io.c',
  '../src/htxf_subchannel.c',
  '../src/qbuf.c',
  '../src/rand.c',
  '../src/algo_list.c',
  '../src/debug.c',
  # The harness login entry points drive connect + magic + LOGIN through
  # the production hxnet orchestrator via the polling FFI
  # (hxnet_connection_open_plaintext_polling / try_recv_frame /
  # send_frame), so the suite pulls in the hxnet staticlib and its
  # compression dependency.
  dependencies        : proto_test_deps + [rust_hxcrypto_hash_dep, rust_hxcrypto_aead_dep, rust_hxcrypto_stream_dep, rust_hxcompress_dep, rust_hxnet_dep],
  include_directories : integration_test_inc,
)

test_integration_handshake = executable('test_integration_handshake',
  'integration/test_handshake.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('handshake', test_integration_handshake,
     suite   : 'integration',
     timeout : 30)

# real_connect — Tier 3 coverage for the production connect path:
# the "hxnet-owns-the-whole-lifecycle" orchestrator connect
# (docs/rust/phase-g-migration.md). Drives the production hx_connect
# (which now dispatches to the hxnet orchestrator unconditionally —
# the connect-path gate was removed with delete-old-connect) against a
# REAL mhxd container so the whole orchestrator runs end
# to end (DNS + TCP + magic + LOGIN + LOGIN-reply + Option-B replay).
# Asserts mhxd accepted the guest login and the reply was replayed to
# the C dispatch with the pinned trans + success flag. No fake_server
# — this one needs a live server, so it fails loudly if mhxd is down.
test_integration_real_connect = executable(
  'test_integration_real_connect',
  'integration/test_real_connect.c',
  'integration/connect_test_stubs.c',
  'integration/server_matrix.c',
  '../src/network.c',
  '../src/host_port.c',
  '../src/hxnet_bridge.c',
  '../src/tls_trust.c',
  '../src/login_packet.c',
  '../src/agreement_packet.c',
  '../src/inline_media.c',
  '../src/proto_helpers.c',
  '../src/proto_trace.c',
  # gtkhx_session.c → Rust crate (Phase R4); linked via
  # rust_gtkhx_session_dep in this target's dependencies.
  '../src/gtkhx_log.c',
  '../src/debug.c',
  '../src/qbuf.c',
  '../src/rand.c',
  '../src/algo_list.c',
  '../src/tracker_parser.c',
  '../src/tracker_v3.c',
  '../src/tracker_v3_meta.c',
  '../src/tracker_event.c',
  '../src/hl_code.c',
  '../src/tasks_table.c',
  '../src/htxf_io.c',
  '../src/htxf_subchannel.c',
  dependencies        : proto_test_deps + [rust_hxcrypto_hash_dep, rust_hxcrypto_aead_dep, rust_hxcrypto_stream_dep, rust_hxcompress_dep, rust_hxnet_dep],
  include_directories : integration_test_inc,
)
test('real_connect', test_integration_real_connect,
     suite   : 'integration',
     timeout : 30)

# tracker_signals — Tier 3 coverage for the tracker fetch -> view-signal
# boundary: the exact input the (Rust) Tracker window consumes. Drives the
# PRODUCTION hx_tracker_list_async -> tracker_fetch_dispatch_event path in
# network.c against the live matrix trackers and asserts the emitted
# GtkhxSession tracker-batch-begin / tracker-server-create signals (a plain
# GObject, so headless — no GdkDisplay needed). Complements test_tracker_
# fetch, which stops at the hxnet FFI below the network.c HxTrackerServer
# construction + signal emit. Same source pile as real_connect (network.c +
# connect_test_stubs.c stubs the GUI refs, incl. the no-op track_prog_update
# so the tasks-window widgets never get built), plus tracker_matrix.c for
# hx_test_trackers_with.
test_integration_tracker_signals = executable(
  'test_integration_tracker_signals',
  'integration/test_tracker_signals.c',
  'integration/connect_test_stubs.c',
  'integration/server_matrix.c',
  'integration/tracker_matrix.c',
  '../src/network.c',
  '../src/host_port.c',
  '../src/hxnet_bridge.c',
  '../src/tls_trust.c',
  '../src/login_packet.c',
  '../src/agreement_packet.c',
  '../src/inline_media.c',
  '../src/proto_helpers.c',
  '../src/proto_trace.c',
  '../src/gtkhx_log.c',
  '../src/debug.c',
  '../src/qbuf.c',
  '../src/rand.c',
  '../src/algo_list.c',
  '../src/tracker_parser.c',
  '../src/tracker_v3.c',
  '../src/tracker_v3_meta.c',
  '../src/tracker_event.c',
  '../src/hl_code.c',
  '../src/tasks_table.c',
  '../src/htxf_io.c',
  '../src/htxf_subchannel.c',
  dependencies        : proto_test_deps + [rust_hxcrypto_hash_dep, rust_hxcrypto_aead_dep, rust_hxcrypto_stream_dep, rust_hxcompress_dep, rust_hxnet_dep],
  include_directories : integration_test_inc,
)
test('tracker_signals', test_integration_tracker_signals,
     suite   : 'integration',
     timeout : 60)

# orchestrator_harness — increment 2 foundation. Pure-FFI test (no
# C production source): drives the production plaintext connect+login
# lifecycle from a synchronous, no-GLib-main-loop test via the polling
# FFI (hxnet_connection_open_plaintext_polling), the way a Tier 3
# harness routed through the orchestrator would. Proves a guest login
# + post-login frame read end-to-end against a real server.
test_integration_orchestrator_harness = executable(
  'test_integration_orchestrator_harness',
  'integration/test_orchestrator_harness.c',
  dependencies        : proto_test_deps + [rust_hxcrypto_hash_dep, rust_hxcrypto_aead_dep, rust_hxcrypto_stream_dep, rust_hxcompress_dep, rust_hxnet_dep],
  include_directories : integration_test_inc,
)
test('orchestrator_harness', test_integration_orchestrator_harness,
     suite   : 'integration',
     timeout : 30)

# socks — Tier 3 coverage for the SOCKS proxy transport (R3 item 9).
# Same pure-FFI polling path as orchestrator_harness, but routes the
# production connect through a real SOCKS5 proxy (the tests/socks-proxy
# container, GTKHX_TEST_SOCKS) to mhxd: a guest login over the tunnel
# plus a dead-proxy negative control proving the proxy isn't bypassed.
test_integration_socks = executable(
  'test_integration_socks',
  'integration/test_integration_socks.c',
  dependencies        : proto_test_deps + [rust_hxcrypto_hash_dep, rust_hxcrypto_aead_dep, rust_hxcrypto_stream_dep, rust_hxcompress_dep, rust_hxnet_dep],
  include_directories : integration_test_inc,
)
test('socks', test_integration_socks,
     suite   : 'integration',
     timeout : 60)

# real_htxf_connect — Tier 3 coverage for PRODUCTION htxf_connect, the
# subchannel-connect function the HTXF->Rust H2 re-wire rewrites (no
# other test calls it). Links production network.c (for htxf_connect)
# alongside the harness control helpers; the harness's hlwrite_chunks /
# hx_htlc_close stubs are __attribute__((weak)) so network.c's strong
# definitions win. Links the full production connect source pile
# (network.c + cipher / hope / proto / htxf_io) plus the harness
# control files (integration_harness/tls/tracker_matrix +
# chat_history that the harness references).
test_integration_real_htxf_connect = executable('test_integration_real_htxf_connect',
  'integration/test_real_htxf_connect.c',
  'integration/connect_test_stubs.c',
  'integration/integration_harness.c',
  'integration/tracker_matrix.c',
  'integration/server_matrix.c',
  '../src/network.c',
  '../src/host_port.c',
  '../src/hxnet_bridge.c',
  '../src/tls_trust.c',
  '../src/login_packet.c',
  '../src/agreement_packet.c',
  '../src/chat_history.c',
  '../src/inline_media.c',
  '../src/proto_helpers.c',
  '../src/proto_trace.c',
  # gtkhx_session.c → Rust crate (Phase R4); linked via
  # rust_gtkhx_session_dep in this target's dependencies.
  '../src/gtkhx_log.c',
  '../src/debug.c',
  '../src/qbuf.c',
  '../src/rand.c',
  '../src/algo_list.c',
  '../src/tracker_parser.c',
  '../src/tracker_v3.c',
  '../src/tracker_v3_meta.c',
  '../src/tracker_event.c',
  '../src/hl_code.c',
  '../src/tasks_table.c',
  '../src/htxf_io.c',
  '../src/htxf_subchannel.c',
  dependencies        : proto_test_deps + [rust_hxcrypto_hash_dep, rust_hxcrypto_aead_dep, rust_hxcrypto_stream_dep, rust_hxcompress_dep, rust_hxnet_dep],
  include_directories : integration_test_inc,
)
test('real_htxf_connect', test_integration_real_htxf_connect,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 30)

# real_tls_login — Phase 1 close-out coverage: full login + chat
# round-trip over TLS. real_connect's /real_connect/tls_login pins the
# connect-state-machine progression via production hx_connect; this
# binary actually speaks the Hotline protocol over the wrapped socket
# end-to-end against Janus's tls_port, driving the control channel
# through PRODUCTION rustls (the orchestrator's TLS polling FFI — see
# integration_open_login_tls_or_skip). Asserts SELFINFO parses cleanly
# after TLS decrypt + a non-ASCII chat body round-trips intact through
# TLS-encrypt-on-send + TLS-decrypt-on-recv.
#
# Links integration_harness_lib only — the production rustls transport
# rides the hxnet staticlib the harness already pulls in.
test_integration_real_tls_login = executable('test_integration_real_tls_login',
  'integration/test_real_tls_login.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
# Janus-only, is_parallel: false per the per-IP connect-rate caveat.
test('real_tls_login', test_integration_real_tls_login,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 60)

# real_tls_banner — Phase 2 coverage: HTXF banner subchannel over TLS.
# The control channel logs in through production rustls (the
# orchestrator), and the banner-image body rides a second TLS
# subchannel opened via the SAME hxnet_htxf_open(tls=1) path banner.c's
# production worker uses, drained through htxf_io_read. The TLS HTXF
# socket uses Janus's tls_xfer_port (5611 host-mapped).
#
# (TLS file-mode HTXF body round-trip is covered by real_htxf_connect's
# file_get_tls sibling, which drives production htxf_connect over TLS —
# so there's no separate real_tls_file_get binary.)
test_integration_real_tls_banner = executable(
  'test_integration_real_tls_banner',
  'integration/test_real_tls_banner.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('real_tls_banner', test_integration_real_tls_banner,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 60)

# tracker_v3_listing — Tier 3 coverage for the tracker v3 client
# against any v3-capable target in the matrix. Drives the
# production tracker_v3.c encoders + parsers over a real TCP
# connection, pinning the 8-byte handshake → 4-byte listing
# request → 10-byte response header → variable-length records
# flow against software we didn't write.
#
# Picks the first HX_TEST_TRACKER_CAP_V3 entry — today Argus only,
# but the test is tracker-agnostic. The known-seed name check
# (Argus's "Promoted Alpha") is gated on the picked entry's name,
# so a future v3 tracker added to the matrix exercises the
# generic flow without needing test changes.
#
# Asserts at the wire-parser layer rather than the boxed-event
# signal layer because the UI render path would need a GMainLoop
# test harness this binary doesn't ship; the production state
# machine processes the same byte sequence either way.
#
# Test name is `tracker_v3_listing` (not bare `tracker_v3`) to
# avoid collision with the proto-suite test of the same name.
# Fails loudly when no v3-capable container is reachable — per the
# no-silent-skip feedback, missing Tier 3 coverage should surface
# in CI as a red light.
test_integration_tracker_v3 = executable(
  'test_integration_tracker_v3',
  'integration/test_tracker_v3.c',
  '../src/tracker_v3.c',
  '../src/tracker_v3_meta.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('tracker_v3_listing', test_integration_tracker_v3,
     suite       : 'integration',
     timeout     : 30)

# tracker_v3_tls_listing — Tier 3 coverage for the Phase D TLS-on-
# the-tracker-port path. Talks to any matrix entry advertising
# HX_TEST_TRACKER_CAP_TLS via its tls_port. Argus has no native
# TLS support so tests/argus/ adds a stunnel sidecar that
# terminates TLS on port 6498 and forwards to plain Argus on
# 5698. The cert is self-signed at image-build time; the test
# accepts any cert inline (we have no GMainLoop / GtkApplication
# to drive the production TOFU prompt against).
#
# Uses GSocketClient + set_tls(TRUE) — the same TLS plumbing the
# production tracker fetch uses in network.c::tracker_fetch_
# connect — so if the wire flow over TLS breaks, this test
# catches it before a user does. Fails loudly when no TLS-capable
# container is reachable, matching the no-silent-skips policy.
test_integration_tracker_v3_tls = executable(
  'test_integration_tracker_v3_tls',
  'integration/test_tracker_v3_tls.c',
  '../src/tracker_v3.c',
  '../src/tracker_v3_meta.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('tracker_v3_tls_listing', test_integration_tracker_v3_tls,
     suite       : 'integration',
     timeout     : 30)

# tracker_fetch — Tier 3 coverage for the hxnet tracker-fetch BRIDGE
# end-to-end (R3 item 8, T4). Drives the production hxnet_tracker_fetch_*
# FFI (the same surface network.c's bridge uses) over the whole matrix
# URL list: serial walk + connect/TLS/probe fallback + listing + the
# HxnetTrackerEvent ABI marshalling, asserting real begin/record/done
# events arrive. Links integration_harness_lib only — the FFI rides the
# hxnet staticlib the harness already pulls in. Honours
# GTKHX_TRACKER_V3_PROBE_MS; 60 s timeout leaves room for a v1-only
# tracker's probe-timeout + TLS-first attempt.
test_integration_tracker_fetch = executable(
  'test_integration_tracker_fetch',
  'integration/test_tracker_fetch.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('tracker_fetch', test_integration_tracker_fetch,
     suite       : 'integration',
     timeout     : 60)

# tracker_v1_listing — Tier 3 coverage for the v1 fallback wire
# path against any v1-only target in the matrix. Pins the wire
# shape after the production state machine's watchdog has expired
# the v3 probe: 6-byte v1 magic → 14-byte reply header (HTRK_MAGIC
# echo + chunk header) → records.
#
# Picks the first matrix entry that has HX_TEST_TRACKER_CAP_V1 but
# NOT HX_TEST_TRACKER_CAP_V3 — today hxtrackd only, but the test
# is tracker-agnostic. The known-seed name check (hxtrackd's
# "hxtrackd test server") is gated on the picked entry's name, so
# a future v1-only tracker added to the matrix exercises the
# generic flow without needing test changes.
#
# Uses src/tracker_parser.c on real wire bytes from a real tracker
# — complements the canned-fixture coverage in tests/proto/
# test_tracker_parser.c. Asserts at the wire-parser layer rather
# than driving the main-loop state machine (which would need a
# GMainLoop test harness this binary doesn't ship); the production
# machine's v1 path is the same byte sequence either way.
#
# Test name is `tracker_v1_listing` (not bare `tracker_v1`) for
# symmetry with the v3-tier-3 test (`tracker_v3_listing`) and to
# leave the bare `tracker_v1` slot free for a future proto-suite
# test of the v1 parser. Fails loudly with rc=134 (SIGABRT) when
# no v1-only container is reachable, matching the no-silent-skips
# policy.
test_integration_tracker_v1 = executable(
  'test_integration_tracker_v1',
  'integration/test_tracker_v1.c',
  '../src/tracker_parser.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('tracker_v1_listing', test_integration_tracker_v1,
     suite       : 'integration',
     timeout     : 30)

# server_matrix — in-process unit coverage for the multi-server
# matrix helpers (hx_test_servers_with, hx_test_server_default,
# GTKHX_TEST_SERVERS env filter). Doesn't talk to a Hotline server,
# but lives in the integration suite because it exercises
# integration-harness code.
test_integration_server_matrix = executable('test_integration_server_matrix',
  'integration/test_server_matrix.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('server_matrix', test_integration_server_matrix,
     suite   : 'integration',
     timeout : 10)

# login — guest login + post-login state-machine round-trip. Sends
# HTLC_HDR_LOGIN, drains pre-SELFINFO chatter, verifies SELFINFO
# parses cleanly and our display name round-trips. Negative case
# checks a malformed login doesn't yield SELFINFO.
test_integration_login = executable('test_integration_login',
  'integration/test_login.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('login', test_integration_login,
     suite   : 'integration',
     timeout : 30)

# chat_roundtrip — login, send chat, expect the broadcast back.
# Single-user mhxd still broadcasts the message (to us), which is
# how the chat widget shows our own typing.
test_integration_chat = executable('test_integration_chat',
  'integration/test_chat_roundtrip.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('chat_roundtrip', test_integration_chat,
     suite   : 'integration',
     timeout : 30)

# user_list — login, send HTLC_HDR_USER_GETLIST, parse the
# HTLS_DATA_USER_LIST chunks, find our own session in the list,
# verify name + icon round-trip via the wire.
test_integration_user_list = executable('test_integration_user_list',
  'integration/test_user_list.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('user_list', test_integration_user_list,
     suite   : 'integration',
     timeout : 30)

# ping — verify the Phase 5 keepalive opcode round-trips. mhxd
# (>=1.5 servers in general) accepts HTLC_HDR_PING and replies
# with a non-error TASK. If a future mhxd commit removes the
# handler this test fails loudly.
test_integration_ping = executable('test_integration_ping',
  'integration/test_ping.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('ping', test_integration_ping,
     suite   : 'integration',
     timeout : 30)

# file_list — request the root file list and verify the round-trip
# completes (whether with a real listing or a tolerant task-error
# if files/ is empty on the server).
test_integration_file_list = executable('test_integration_file_list',
  'integration/test_file_list.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('file_list', test_integration_file_list,
     suite   : 'integration',
     timeout : 30)

# news_fetch — HTLC_HDR_NEWS_GETFILE round-trip. Verifies the
# server returns the news body and uses hx_news_file_extract
# (a Tier 2 helper) to parse the chunk.
test_integration_news_fetch = executable('test_integration_news_fetch',
  'integration/test_news_fetch.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('news_fetch', test_integration_news_fetch,
     suite   : 'integration',
     timeout : 30)

# two_client_chat — the cross-user broadcast path. Client A sends
# chat, client B (a separate logged-in connection) receives it
# tagged with A's uid + name. Proves the server actually fans out
# to all members.
test_integration_two_client_chat = executable('test_integration_two_client_chat',
  'integration/test_two_client_chat.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('two_client_chat', test_integration_two_client_chat,
     suite   : 'integration',
     timeout : 30)

# emoji_roundtrip — A encodes an emoji-bearing line through the legacy
# send path (emoji → :tada:), B decodes the broadcast back to the emoji.
# Exercises phase E2 + E3 end-to-end against the Mac Roman mhxd and guards
# the "don't break Mac Roman" requirement (wire form stays ASCII).
test_integration_emoji_roundtrip = executable('test_integration_emoji_roundtrip',
  'integration/test_emoji_roundtrip.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('emoji_roundtrip', test_integration_emoji_roundtrip,
     suite   : 'integration',
     timeout : 30)

# gif_icons — fogWraith GIF-icons extension (TRAN 1861-1864). No
# capability bit, so it runs against the default matrix server; both
# mhxd and Janus implement it. Set/get round-trip, getlist, the
# ICON_CHANGE broadcast to a second client, and clear.
test_integration_gif_icons = executable('test_integration_gif_icons',
  'integration/test_gif_icons.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('gif_icons', test_integration_gif_icons,
     suite   : 'integration',
     timeout : 30)

# msg_roundtrip — A→B private message via HTLC_HDR_MSG. B parses
# the broadcast through hx_msg_extract (a Tier 2 helper) and
# verifies uid + name + body all round-trip.
test_integration_msg = executable('test_integration_msg',
  'integration/test_msg_roundtrip.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('msg_roundtrip', test_integration_msg,
     suite   : 'integration',
     timeout : 30)

# user_info — HTLC_HDR_USER_GETINFO for our own session. Verifies
# the TASK reply carries HTLS_DATA_USER_INFO + HTLS_DATA_NAME and
# the info text mentions our login.
test_integration_user_info = executable('test_integration_user_info',
  'integration/test_user_info.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('user_info', test_integration_user_info,
     suite   : 'integration',
     timeout : 30)

# user_account — full ACCOUNT_MODIFY / ACCOUNT_READ / ACCOUNT_DELETE
# lifecycle exercising the wire format src/usermod.c's New User /
# Edit User / Delete dialog flows produce. Logs in as `admin` (the
# upstream mhxd default with no password and full access bits) and
# round-trips a synthesised account through create → read → modify →
# read → delete → read-fails. A fourth subtest pins down the access-
# bit serialisation specifically — the regression net for the
# useredit_chk_activate signature bug that left every newly-created
# account with an all-zero access mask.
#
# Not parallelised against other Tier 3 tests because it logs in as
# `admin` and mutates the server's accounts directory; concurrent
# tests against the same login or against the same synthesized
# account names would race. Each subtest's account name has a
# per-process random suffix so concurrent meson runs don't collide.
test_integration_user_account = executable('test_integration_user_account',
  'integration/test_user_account.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('user_account', test_integration_user_account,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 30)

# user_change_broadcast — Alice and Bob both connect; Alice
# verifies she receives HTLS_HDR_USER_CHANGE when Bob joins and
# HTLS_HDR_USER_PART when Bob disconnects. Exercises mhxd's
# snd_user_join / snd_user_part broadcast paths.
test_integration_user_change = executable('test_integration_user_change',
  'integration/test_user_change_broadcast.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('user_change_broadcast', test_integration_user_change,
     suite   : 'integration',
     timeout : 30)

# news_post — post to news, fetch back, verify it's there. Plus
# the unsolicited HTLS_HDR_NEWS_POST broadcast to other clients.
# Mutates server state; uses a per-run random marker so reruns
# in the same container don't false-positive on residue.
test_integration_news_post = executable('test_integration_news_post',
  'integration/test_news_post.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('news_post', test_integration_news_post,
     suite   : 'integration',
     timeout : 30)

# chat_create — Alice creates a private chat naming Bob; Bob's
# connection receives HTLS_HDR_CHAT_INVITE with Alice's uid +
# name + the new chat_id. End-to-end exercise of mhxd's
# rcv_chat_create + snd_chat_invite.
test_integration_chat_create = executable('test_integration_chat_create',
  'integration/test_chat_create.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('chat_create', test_integration_chat_create,
     suite   : 'integration',
     timeout : 30)

# user_rename — Alice changes name + icon via HTLC_HDR_USER_CHANGE;
# Bob's connection receives HTLS_HDR_USER_CHANGE with the new
# values. Filter by uid AND name to walk past Alice's join
# broadcast that landed earlier under her original name.
test_integration_user_rename = executable('test_integration_user_rename',
  'integration/test_user_rename.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('user_rename', test_integration_user_rename,
     suite   : 'integration',
     timeout : 30)

# nick_colors — Tier 3 coverage for the Colored-Nicknames extension.
# Two subtests:
#   user_change_round_trip — outbound USER_CHANGE with DATA_COLOR is
#     echoed back as a HTLS_HDR_USER_CHANGE broadcast carrying the
#     same chunk (the spec's auto-opt-in path).
#   user_list_trailer      — USER_LIST records carry the per-user
#     color as a 4-byte trailer after the name (Janus's extension
#     to hl_userlist_hdr; the production parser in rcv.c reads it
#     gated on _len >= 8 + nlen + 4).
# Filters the matrix for HX_TEST_CAP_NICK_COLORS; today only Janus
# advertises the bit (mhxd would silently drop the DATA_COLOR chunk).
# Janus-only → is_parallel: false per the per-IP connect-rate
# limit caveat documented on the other Janus tests.
test_integration_nick_colors = executable('test_integration_nick_colors',
  'integration/test_nick_colors.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('nick_colors', test_integration_nick_colors,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 30)

# file_get — full HTXF subchannel exchange driving the PRODUCTION
# receive state machine. Sends FILE_GET on the main port (5500),
# receives the TASK reply with HTXF_REF + HTXF_SIZE, opens the subchannel
# (5501 by default; override via GTKHX_TEST_XFER_PORT), then calls the
# real xfers_recv.c::file_recv_one and asserts the decoded on-disk file
# is byte-for-byte the seeded 'hello world\n'. Links xfers_recv.c + hfs.c
# (receive machine + HFS sidecar) and the hxfiles-xfer FFO codec on top
# of integration_harness_lib (which bundles htxf_io.c + the hxnet
# channel). The Dockerfile seeds files/test.txt for this purpose.
test_integration_file_get = executable('test_integration_file_get',
  'integration/test_file_get.c',
  '../src/xfers_recv.c',
  '../src/hfs.c',
  dependencies        : proto_test_deps + [rust_hxfiles_xfer_dep],
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('file_get', test_integration_file_get,
     suite   : 'integration',
     timeout : 30)

# NOTE: the former file_get_htxf_io target is gone. It drove the
# plaintext passthrough leg of the C byte pump through a hand-rolled
# subchannel. The H2 re-wire moved that pump into the hxnet crate, and
# the full production path (htxf_connect -> htxf_io_read) is now covered
# across plaintext / AEAD / TLS by test_real_htxf_connect's three
# siblings, so this hand-rolled variant is redundant.

# banner — verify the post-AGREEMENTAGREE banner broadcast.
# is_parallel: false because the htxf_mode subtest picks the first
# matrix server advertising HX_TEST_CAP_BANNER_HTXF — today Janus,
# which shares the per-IP connect-rate caveat called out on the
# chacha20 sister tests.
test_integration_banner = executable('test_integration_banner',
  'integration/test_banner.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('banner', test_integration_banner,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 30)

# chat_subject — Alice creates a private chat naming Bob, then sets
# the chat subject; verifies HTLS_HDR_CHAT_SUBJECT broadcasts to
# the chat members with the new subject text.
test_integration_chat_subject = executable('test_integration_chat_subject',
  'integration/test_chat_subject.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('chat_subject', test_integration_chat_subject,
     suite   : 'integration',
     timeout : 30)

# news15 — threaded-news 1.5+ opcodes. HTLC_HDR_NEWSDIRLIST root
# listing returns at least one HTLS_DATA_NEWS_DIRLIST_EXTENDED
# chunk (mhxd ships with cat_irasshaimase pre-seeded under
# newsdir/).
test_integration_news15 = executable('test_integration_news15',
  'integration/test_news15.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('news15', test_integration_news15,
     suite   : 'integration',
     timeout : 30)

# file_put — full upload round-trip driving the PRODUCTION send state
# machine. Sends HTLC_HDR_FILE_PUT to the seeded Uploads/ dir, streams an
# FFO-wrapped file via xfers_send.c::file_send_one, then downloads it
# back through xfers_recv.c::file_recv_one and asserts byte-exact. Links
# both machines + hfs.c + the FFO codec on top of integration_harness_lib.
# Requires the guest account to have UPLOAD_FILES + UPLOAD_ANYWHERE.
test_integration_file_put = executable('test_integration_file_put',
  'integration/test_file_put.c',
  '../src/xfers_send.c',
  '../src/xfers_recv.c',
  '../src/hfs.c',
  dependencies        : proto_test_deps + [rust_hxfiles_xfer_dep],
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('file_put', test_integration_file_put,
     suite   : 'integration',
     timeout : 30)

# file_info — HTLC_HDR_FILE_GETINFO on the seeded test.txt. Verifies
# the TASK reply carries HTLS_DATA_FILE_NAME and a FILE_SIZE that
# matches the 12-byte "hello world\n" payload.
test_integration_file_info = executable('test_integration_file_info',
  'integration/test_file_info.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('file_info', test_integration_file_info,
     suite   : 'integration',
     timeout : 30)

# file_list_subdir — FILE_LIST with a hand-built HTLC_DATA_DIR
# pointing at the seeded files/Uploads/ subdirectory. Covers the
# DIR-chunked variant of file listing (the existing test_file_list
# only exercises the no-DIR root path).
test_integration_file_list_subdir = executable('test_integration_file_list_subdir',
  'integration/test_file_list_subdir.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('file_list_subdir', test_integration_file_list_subdir,
     suite   : 'integration',
     timeout : 30)

# chat_join — Bob accepts Alice's private-chat invite via
# HTLC_HDR_CHAT_JOIN. Verifies Bob's TASK reply carries USER_LIST
# entries for the joined members AND that Alice's connection sees
# HTLS_HDR_CHAT_USER_CHANGE tagged with Bob's uid. Pins down
# mhxd's rcv_chat_join broadcast loop end-to-end.
test_integration_chat_join = executable('test_integration_chat_join',
  'integration/test_chat_join.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('chat_join', test_integration_chat_join,
     suite   : 'integration',
     timeout : 30)

# chat_decline — Bob refuses Alice's private-chat invite via
# HTLC_HDR_CHAT_DECLINE. mhxd's rcv_chat_decline is silent (no
# reply, no broadcast); we round-trip a PING on each side after
# the decline to prove no stray frames slipped in and the
# connections stay healthy.
test_integration_chat_decline = executable('test_integration_chat_decline',
  'integration/test_chat_decline.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('chat_decline', test_integration_chat_decline,
     suite   : 'integration',
     timeout : 30)

# chat_part — Bob joins Alice's chat then leaves. Alice's
# connection should receive HTLS_HDR_CHAT_USER_PART tagged with
# the chat_id and Bob's uid. Pins down mhxd's rcv_chat_part
# broadcast loop; complements chat_join from batch 8.
test_integration_chat_part = executable('test_integration_chat_part',
  'integration/test_chat_part.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('chat_part', test_integration_chat_part,
     suite   : 'integration',
     timeout : 30)

# login_bad — login with a non-existent account name. Verifies the
# server replies with a TASK + flag=1 (error bit) carrying a
# TASKERROR chunk, and crucially that no SELFINFO follows. Pins
# the negative path of the login state machine.
test_integration_login_bad = executable('test_integration_login_bad',
  'integration/test_login_bad.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('login_bad', test_integration_login_bad,
     suite   : 'integration',
     timeout : 30)

# msg_self — pin down what mhxd does when the client sends
# HTLC_HDR_MSG addressed to its own uid. The actual handling
# (echo / drop / task-error) is tolerant; the assertion is that
# whatever happens, the stream stays clean enough that a follow-up
# PING round-trips with matching trans.
test_integration_msg_self = executable('test_integration_msg_self',
  'integration/test_msg_self.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('msg_self', test_integration_msg_self,
     suite   : 'integration',
     timeout : 30)

# chat_in_pchat — chat message routed through a private chat
# (HTLC_DATA_CHAT_ID != 0). Alice creates a pchat with Bob, Bob
# joins, Alice sends a chat tagged with the chat_id; Bob receives
# HTLS_HDR_CHAT carrying that same chat_id and the body. Pins the
# multi-cid routing path that complements the public-chat broadcast
# already covered by test_chat_roundtrip / test_two_client_chat.
test_integration_chat_in_pchat = executable('test_integration_chat_in_pchat',
  'integration/test_chat_in_pchat.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('chat_in_pchat', test_integration_chat_in_pchat,
     suite   : 'integration',
     timeout : 30)

# unauthorized_opcode — guest sends HTLC_HDR_FILE_MKDIR without
# access.create_folders. mhxd's main rcv loop gates the dispatch
# on the access bit and silently drops the request when it's
# unset. Test asserts no TASK reply correlates to our trans, then
# round-trips a PING to prove the dispatcher is still healthy.
# Catches a regression where the dispatcher accidentally promotes
# a privileged opcode for the guest role.
test_integration_unauthorized_opcode = executable('test_integration_unauthorized_opcode',
  'integration/test_unauthorized_opcode.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('unauthorized_opcode', test_integration_unauthorized_opcode,
     suite   : 'integration',
     timeout : 30)

# msg_to_unknown_uid — sending HTLC_HDR_MSG to a uid that isn't a
# live connection. mhxd's hxd_rcv_msg looks targets up via
# isclient(); a NULL hit drops the message without acknowledgment.
# Test verifies no echo / task-error came back and the stream is
# still in a clean dispatcher state via a follow-up PING.
test_integration_msg_to_unknown_uid = executable('test_integration_msg_to_unknown_uid',
  'integration/test_msg_to_unknown_uid.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('msg_to_unknown_uid', test_integration_msg_to_unknown_uid,
     suite   : 'integration',
     timeout : 30)

# news_catlist — list articles inside the seeded cat_irasshaimase
# threaded-news category. The existing news15 test only covers the
# root-level dirlist; this goes one level deeper and exercises
# mhxd's HTLC_HDR_NEWSCATLIST + cat_to_path packed-path parser.
# Tolerant to tnews-disabled / empty-category edge cases.
test_integration_news_catlist = executable('test_integration_news_catlist',
  'integration/test_news_catlist.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('news_catlist', test_integration_news_catlist,
     suite   : 'integration',
     timeout : 30)

# user_list_grows — Alice's USER_GETLIST reply count grows by one
# when Bob logs in. Complements test_user_list (single-client
# self-presence) by proving the server actually enumerates other
# users in the reply, not just the requester.
test_integration_user_list_grows = executable('test_integration_user_list_grows',
  'integration/test_user_list_grows.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('user_list_grows', test_integration_user_list_grows,
     suite   : 'integration',
     timeout : 30)

# chat_long — 1500-byte chat message survives the broadcast wire
# intact. Stresses the proto parser past any < 256 byte happy-path
# and the qbuf growth in network.c. Catches signed-byte
# truncation / off-by-one regressions that would clip the body.
test_integration_chat_long = executable('test_integration_chat_long',
  'integration/test_chat_long.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('chat_long', test_integration_chat_long,
     suite   : 'integration',
     timeout : 30)

# folder_get — HTLC_HDR_FILE_GETFOLDER (0xd2) full round-trip. The
# Hotline 1.5 folder-download opcode streams a whole tree over a
# single HTXF subchannel via the FILE_NEXT state machine. This
# test drives the state machine end-to-end: sends GETFOLDER for
# the seeded files/test_folder/, parses each next_file_info
# entry, sends FILE_SEND for each file leaf, and substring-
# searches the FILP-wrapped payloads for the seeded content.
# Fixture: Dockerfile seeds files/test_folder/leaf_{a,b}.txt.
test_integration_folder_get = executable('test_integration_folder_get',
  'integration/test_folder_get.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('folder_get', test_integration_folder_get,
     suite   : 'integration',
     timeout : 30)

# chat_history — Tier 3 coverage for the fogWraith chat-history
# extension (TRAN 700 + DATA_HISTORY_*). Filters the matrix for
# HX_TEST_CAP_CHAT_HISTORY; today only Janus advertises the bit.
# Compiles src/chat_history.c in directly so the test can reuse
# the production HxHistoryEntry parser (the harness lib stays
# chat-history-free — only this binary needs the parser).
# Skips silently when no chat-history-capable server is configured
# in the matrix (e.g. CI runs with GTKHX_TEST_SERVERS=mhxd).
test_integration_chat_history = executable('test_integration_chat_history',
  'integration/test_chat_history.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)

# hope_chacha20 — Tier 3 coverage for the HOPE-Secure-Login +
# ChaCha20-Poly1305 AEAD path against a server that supports it
# (Janus today; filtered via HX_TEST_CAP_CHACHA20). Skips silently
# when no chacha20-capable server is in the matrix.
#
# The whole rest of the test-vs-prod dedup work on this branch (the
# src/login_packet.{c,h} module, the harness AEAD wrappers,
# integration_open_login_hope_or_skip) exists to make this test
# possible.
test_integration_hope_chacha20 = executable('test_integration_hope_chacha20',
  'integration/test_hope_chacha20.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
# is_parallel: false on every Janus-only test. Janus has an internal
# per-IP cap on simultaneous connections that we can't fully disable
# from config (the binary's MaxConnectionsPerIP only covers the
# top-level accept-rate path; the HTXF subchannel + HOPE handshake
# share a different counter). Under meson's default parallelism the
# 4 Janus-targeting tests fire concurrent connections from 127.0.0.1
# and trip a 5s drain timeout on whichever one loses the race —
# usually hope_chacha20_banner since it opens *two* connections
# (control + HTXF subchannel). Each test still has plenty of work
# under serial: hope_chacha20_banner takes ~250 ms when alone, so
# the four added together cost ~1 s on top of the rest of the
# integration suite, which runs at full parallelism.
test('hope_chacha20', test_integration_hope_chacha20,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 60)

# hope_hmac — Tier 3 coverage for the HOPE handshake against an
# HMAC-only HOPE server (no AEAD). Filters on HX_TEST_CAP_HOPE so
# it picks up mhxd (built with --enable-hope). Sister test to
# hope_chacha20: same handshake state machine, no AEAD framing.
# Exists because Janus's HOPE container isn't shippable yet, so
# hope_chacha20 skips silently — this one keeps the live HOPE
# handshake exercised against mhxd until that gap closes.
test_integration_hope_hmac = executable('test_integration_hope_hmac',
  'integration/test_hope_hmac.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('hope_hmac', test_integration_hope_hmac,
     suite   : 'integration',
     timeout : 60)

# hope_blowfish — Tier 3 coverage for the legacy HOPE stream-cipher
# path. Same handshake state machine as hope_chacha20 but with the
# per-message 3/16-probability rekey marker exercised end-to-end.
# Sends 12 PINGs in a row against an mhxd-style server, which gives
# ~90% odds of hitting at least one rekey-marker round-trip — far
# better coverage of the cipher state machine than hand-driven GUI
# testing produced. (A sister hope_rc4 test was retired alongside
# the RC4 removal — there's no longer a code path for it to drive.)
test_integration_hope_blowfish = executable('test_integration_hope_blowfish',
  'integration/test_hope_blowfish.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('hope_blowfish', test_integration_hope_blowfish,
     suite   : 'integration',
     timeout : 60)

# hope_chacha20_chat_history — Tier 3 for the fogWraith chat-history
# extension under HOPE+ChaCha20-Poly1305 AEAD framing. Janus is the
# only matrix entry advertising both bits; the test skips cleanly
# when neither is in scope. Exists because the user observed
# regressions with chat history + cipher enabled — the harness
# previously had no HOPE-aware chat-history send helper, so this
# code path was uncovered.
test_integration_hope_chacha20_chat_history = executable(
  'test_integration_hope_chacha20_chat_history',
  'integration/test_hope_chacha20_chat_history.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('hope_chacha20_chat_history', test_integration_hope_chacha20_chat_history,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 60)

# hope_chacha20_banner — Tier 3 for the file-mode banner HTXF
# subchannel under HOPE+ChaCha20-Poly1305. Drives the production
# htxf_io.c AEAD wrappers (htxf_io_read / htxf_io_write) plus the
# per-transfer cipher_aead_derive_transfer_keys derivation, so the
# test exercises exactly the path production xfers.c follows for
# regular file downloads. Requires a server in BANNER_MODE=binary
# (e.g. Janus with a JPEG/GIF banner).
test_integration_hope_chacha20_banner = executable(
  'test_integration_hope_chacha20_banner',
  'integration/test_hope_chacha20_banner.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('hope_chacha20_banner', test_integration_hope_chacha20_banner,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 60)

# Blowfish variants of the chat-history + banner coverage. Same
# shape as the chacha20 sisters above; the cap filter routes the
# test to whichever matrix entry advertises both BLOWFISH and the
# feature. mhxd advertises BLOWFISH+BANNER_HTXF but no chat-history;
# Janus has chat-history+CHACHA20 but no stream cipher, so the
# chat-history stream test skips cleanly until a matrix entry
# combining stream-cipher + chat-history shows up. The RC4 sisters
# were retired alongside the RC4 removal.
test_integration_hope_blowfish_chat_history = executable(
  'test_integration_hope_blowfish_chat_history',
  'integration/test_hope_blowfish_chat_history.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
# Stream-cipher × {chat_history, banner} matrices route to Janus
# (mhxd advertises neither CHAT_HISTORY nor — since the matrix-fix
# branch — BANNER_HTXF), so they're subject to the same per-IP
# connect-rate caveat as the chacha20 sisters above. is_parallel:
# false keeps them from racing each other in meson's default
# parallel run.
test('hope_blowfish_chat_history', test_integration_hope_blowfish_chat_history,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 60)

test_integration_hope_blowfish_banner = executable(
  'test_integration_hope_blowfish_banner',
  'integration/test_hope_blowfish_banner.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('hope_blowfish_banner', test_integration_hope_blowfish_banner,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 60)

test('chat_history', test_integration_chat_history,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 60)

# inline_media — Phase 9.F Tier 3 coverage. First step per
# docs/inline-media-plan.md: verify Janus actually implements the
# fogWraith inline-media extension by asserting the LOGIN reply
# echoes CAP_INLINE_MEDIA and advertises at least one
# DATA_CHAT_MEDIA_MAX_* advisory field. Filtered on
# HX_TEST_CAP_INLINE_MEDIA — today only Janus advertises the bit.
# Follow-up commits in the same Phase 9.F branch will add full
# upload + chat-with-media-handle + download round-trip once
# this first step is green. Failure mode is a clean assertion,
# not a silent skip (per the no-test-skips feedback memory),
# which gives a fast signal if Janus turns out to not actually
# implement the extension and the mock-server fallback path
# needs to be taken.
test_integration_inline_media = executable('test_integration_inline_media',
  'integration/test_inline_media.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('inline_media', test_integration_inline_media,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 60)

# folder_put — HTLC_HDR_FILE_PUTFOLDER (0xd5) request/reply phase.
# Mirror of test_file_put's scope: sends the opcode targeted at
# Uploads/<random>, verifies the TASK reply correlates by trans
# and either carries an HTXF_REF or a task-error. Full upload
# (FILP-wrapped data streamed over the subchannel) is a follow-
# up batch.
test_integration_folder_put = executable('test_integration_folder_put',
  'integration/test_folder_put.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('folder_put', test_integration_folder_put,
     suite   : 'integration',
     timeout : 30)

# voice_* — Phase 8.F coverage for the fogWraith voice-chat
# extension (HTLC_CAP_VOICE + 600-606 transactions + 0x01F5-0x01F9
# fields). Filters the matrix for HX_TEST_CAP_VOICE; today only
# Janus advertises the bit, so each test fails loudly when the
# Janus container isn't reachable. Janus voice is server-side
# opt-in (EnableVoice: true + per-account VoiceChat: true); the
# bundled tests/janus/Dockerfile config has both on by default.
#
# All voice tests are Janus-only → is_parallel: false for the same
# per-IP connect-rate caveat documented on the hope_chacha20
# sister tests. The multi-client tests (participants_broadcast,
# disconnect_cleanup) open two connections each, so the cost is
# higher than the single-client tests but still well under the
# 60 s timeout.
#
# Built only when voice is compiled in (-Dvoice / voice_enabled).
# The matrix infrastructure (server_matrix.c's HX_TEST_CAP_VOICE /
# voice_port fields) stays compiled regardless — only these
# end-to-end binaries are gated.
if voice_enabled
test_integration_voice_join = executable('test_integration_voice_join',
  'integration/test_voice_join.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('voice_join', test_integration_voice_join,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 60)

test_integration_voice_sdp_roundtrip = executable(
  'test_integration_voice_sdp_roundtrip',
  'integration/test_voice_sdp_roundtrip.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('voice_sdp_roundtrip', test_integration_voice_sdp_roundtrip,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 60)

test_integration_voice_ice_trickle = executable(
  'test_integration_voice_ice_trickle',
  'integration/test_voice_ice_trickle.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('voice_ice_trickle', test_integration_voice_ice_trickle,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 60)

test_integration_voice_mute = executable('test_integration_voice_mute',
  'integration/test_voice_mute.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('voice_mute', test_integration_voice_mute,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 60)

test_integration_voice_implicit_leave = executable(
  'test_integration_voice_implicit_leave',
  'integration/test_voice_implicit_leave.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('voice_implicit_leave', test_integration_voice_implicit_leave,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 60)

test_integration_voice_participants = executable(
  'test_integration_voice_participants',
  'integration/test_voice_participants.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('voice_participants', test_integration_voice_participants,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 60)

test_integration_voice_disconnect_cleanup = executable(
  'test_integration_voice_disconnect_cleanup',
  'integration/test_voice_disconnect_cleanup.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
# Longer meson timeout than the other voice binaries: the post-
# disconnect 605 update arrives on a Janus housekeeping tick, not
# synchronously off the FIN read, so the per-recv wait is bumped to
# 15 s inside the test and we want headroom on top of that for
# slow runners.
test('voice_disconnect_cleanup', test_integration_voice_disconnect_cleanup,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 120)

test_integration_voice_rejoin_cycle = executable(
  'test_integration_voice_rejoin_cycle',
  'integration/test_voice_rejoin_cycle.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('voice_rejoin_cycle', test_integration_voice_rejoin_cycle,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 60)

# voice_rejoin_media — MEDIA-level Tier 3 repro: two real VoiceRuntimes
# (webrtcbin + ICE/DTLS/SRTP) against Janus. A joins alone (bundled
# sendrecv "send" transceiver), B joins, B leaves + rejoins; the test
# asserts A keeps receiving B's RTP across the rejoin. Needs the voice
# runtime staticlib + the GStreamer stack linked (unlike the
# signaling-only voice tests above), and UDP reachability to Janus.
test_integration_voice_rejoin_media = executable(
  'test_integration_voice_rejoin_media',
  'integration/test_voice_rejoin_media.c',
  dependencies        : proto_test_deps + [
    rust_hxvoice_runtime_dep,
    gstreamer_dep,
    gstreamer_app_dep,
    gstreamer_audio_dep,
    gstreamer_sdp_dep,
    gstreamer_webrtc_dep,
  ],
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('voice_rejoin_media', test_integration_voice_rejoin_media,
     suite       : 'integration',
     is_parallel : false,
     timeout     : 180)
endif # voice_enabled
