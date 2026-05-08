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
