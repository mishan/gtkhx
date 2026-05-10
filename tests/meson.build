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

# -----------------------------------------------------------------------------
# Tier 3 — integration/
#
# Real-server protocol-level tests. Each test opens a TCP connection
# to the configured Hotline server (mhxd Docker container at
# localhost:5500 by default; override with GTKHX_TEST_HOST /
# GTKHX_TEST_PORT) and drives the wire protocol end-to-end.
#
# Skip-if-unavailable: if the server isn't reachable within the
# connect timeout, the test calls g_test_skip and exits cleanly.
# That lets the integration suite live alongside unit + proto in
# the default `meson test` invocation without requiring everyone to
# run Docker.
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
integration_harness_lib = static_library(
  'integration_harness',
  'integration/integration_harness.c',
  '../src/proto_helpers.c',
  dependencies        : proto_test_deps,
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

# file_get — full HTXF subchannel exchange. Sends FILE_GET on the
# main port (5500), receives the TASK reply with HTXF_REF +
# HTXF_SIZE, opens a fresh TCP connection to the subchannel
# port (5501 by default; override via GTKHX_TEST_XFER_PORT),
# sends the 16-byte HTXF magic header, reads the streamed
# payload, and substring-searches for our seeded 'hello world'
# content. The Dockerfile seeds files/test.txt for this purpose.
test_integration_file_get = executable('test_integration_file_get',
  'integration/test_file_get.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('file_get', test_integration_file_get,
     suite   : 'integration',
     timeout : 30)

# banner — verify the post-AGREEMENTAGREE banner broadcast.
test_integration_banner = executable('test_integration_banner',
  'integration/test_banner.c',
  dependencies        : proto_test_deps,
  include_directories : integration_test_inc,
  link_with           : integration_harness_lib,
)
test('banner', test_integration_banner,
     suite   : 'integration',
     timeout : 30)

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

# file_put — request/reply phase only. Sends HTLC_HDR_FILE_PUT
# targeted at the seeded Uploads/ directory; verifies the TASK
# reply correlates by trans and either carries an HTXF_REF or a
# task-error (server-permission-dependent). Full upload (FILP-
# wrapped data over the subchannel) is a future extension.
test_integration_file_put = executable('test_integration_file_put',
  'integration/test_file_put.c',
  dependencies        : proto_test_deps,
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
