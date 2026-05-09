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

# Tests that include protocol.h need cipher / compress headers via
# nettle and zlib. Kept separate from test_deps so pure-GLib tests
# (hl_access, text_util) don't pull in unnecessary link args.
proto_test_deps = test_deps + [
  nettle_dep,
  zlib_dep,
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

# text_util — gtkhx_text_to_utf8 lives in src/text_util.c (broken out
# of gtkutil.c specifically so this test doesn't have to drag in GTK,
# Adwaita, and the rest of gtkhx's header pile). Compile the source
# in directly rather than linking a shared lib — it's pure GLib, the
# binary stays small, and the test stays a single self-contained
# translation unit pair.
test_text_util = executable('test_text_util',
  'unit/test_text_util.c',
  '../src/text_util.c',
  dependencies        : test_deps,
  include_directories : test_inc,
)
test('text_util', test_text_util,
     suite : 'unit')

# byte_swap — HN16 / HN32 macros from protocol.h. Header-only macros,
# but protocol.h pulls in cipher.h / compress.h, so the test needs
# nettle + zlib on its dep list. Worth it: these macros are the
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

# hmac — the HOPE handshake's MAC dispatcher. RFC 2104 / RFC 2202
# canonical vectors for the HMAC-prefixed branches; cross-checked
# GChecksum hand-computed digests for the legacy unprefixed key||text
# branches. hmac.c is a thin GChecksum / GHmac wrapper, so this
# really tests the dispatch glue + GLib's primitives.
test_hmac = executable('test_hmac',
  'unit/test_hmac.c',
  '../src/hmac.c',
  dependencies        : proto_test_deps,
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
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('msg', test_msg_proto,
     suite : 'proto')

# hlwrite — round-trip the wire packer (hlpack, extracted from
# hlwrite() in network.c) through the dh_start walker. Proves the
# send-side encoder and the receive-side decoder agree on the wire
# format end-to-end.
test_hlwrite = executable('test_hlwrite',
  'proto/test_hlwrite.c',
  '../src/proto_helpers.c',
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
  dependencies        : proto_test_deps,
  include_directories : proto_test_inc,
  link_with           : wire_fixture_lib,
)
test('news_file', test_news_file,
     suite : 'proto')
