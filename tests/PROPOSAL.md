# Unit testing proposal for GtkHx

## Why now

Phase 4 (GTK 4 port) is done; Phase 5 (modernization) is mostly mechanical
follow-up. The wire-protocol layer, the access-bit decoder, the prefs
parser, and the encoding-conversion helper are now stable enough that a
test on any of them won't have to be rewritten next month — they have a
clear shape.

We also just hit a bug class that tests would have caught immediately:
the BOOLEAN prefs parser silently accepting `'0'` / `'1'` but ignoring
GKeyFile's `true` / `false` (commit `6bb3928`). Six lines, one regex,
one obvious unit test would have failed loudly. There are similar
parsers throughout the codebase. Tests are now a force multiplier, not
a tax.

## Recommendation

Adopt **GLib's built-in test framework** (`g_test_*`) and Meson's `test()`
target for the runner. Three tiers of tests in priority order. Tier 1
gives the fastest feedback for the lowest effort; tiers 2 and 3 require
some refactoring as we go.

The existing **mhxd Docker container** (see `tests/mhxd/`) is the
integration-testing target for Tier 3.

## Framework choice — `g_test`

Pros:
- Zero new dependencies. GLib is already a hard requirement and ships
  the testing API.
- Integrates cleanly with Meson via `test(name, exe)`.
- The output format Meson reports (TAP) drops straight into GitHub
  Actions / GitLab CI test reporters when we get there.
- `g_test_add_func()` registers individual cases; `g_assert_cmpstr` /
  `g_assert_cmpint` / `g_assert_cmphex` give meaningful failure
  messages rather than just "FAIL".
- Built-in `g_test_subprocess()` for testing code paths that abort or
  exit, and `g_test_trap_*` for capturing stderr (useful for our
  diagnostic logging).

Alternatives considered and rejected:
- **cmocka / Unity / Check**: extra dep, no compelling feature win for
  what we'd be testing.
- **C++ frameworks (Catch2, Google Test)**: would force a per-test
  language switch. Not worth it.

## What we should test

### Tier 1 — pure functions, no GTK, no I/O

These are the lowest-effort, highest-value tests. Every entry below is
a function with no side effects and a clear contract. No refactoring
required to test them.

| Target                              | File / line                              | Why it matters                              |
|-------------------------------------|------------------------------------------|---------------------------------------------|
| `hl_access_has(access, bit)`        | `src/hl_access.h` (inline)               | Bit-decoding correctness; we just got this wrong once already |
| `gtkhx_text_to_utf8(bytes, len, *)` | `src/gtkutil.c`                          | Mac Roman → UTF-8 cascade with U+FFFD fallback; covers chat / news / preview content |
| `dirchar_basename(path)`            | `src/files.c:1290`                       | Pure path helper used in xfer paths        |
| `CR2LF(buf, len)` / `LF2CR`         | macros in `src/hx.h`                     | Wire-format line ending conversions        |
| `strip_ansi(buf, len)`              | wherever it's defined                    | Pure-byte transform; used everywhere chat lands |
| BOOLEAN parser in `prefs_allocate`  | `src/options.c` near the type switch     | The bug we already shipped a fix for       |
| Hotline byte-swap macros (`HN16`/`HN32` round-trip) | `src/protocol.h`                | Endianness sanity                           |
| `hmac_xxx()` with canonical inputs  | `src/hmac.c`                             | RFC 2104 / RFC 4231 test vectors            |

Estimated effort: **1-2 days** to write the first eight test programs
plus the meson scaffolding. Probably 30-60 lines of test code per
target.

### Tier 2 — protocol parsing with canned wire bytes

Most of the receive-side code in `rcv.c` reads from `htlc->in.buf`,
parses chunks, and updates state. We can construct a fake
`struct htlc_conn`, hand-pack the buffer with a known-good wire
sequence, and call the handler. The handlers don't talk to the network
themselves, just to the htlc state.

Concrete first targets:

- **`hx_rcv_user_selfinfo`** with a known SELFINFO payload — verify
  `htlc->access`, `htlc->uid`, `htlc->name` get set correctly. Also
  the post-login fetch-trigger; we'd want to verify it fires (might
  need to inject the helper).
- **`hx_rcv_chat`** / **`hx_rcv_msg`** with valid + non-UTF-8 +
  CR-line-ending payloads, asserting the sanitised string we'd hand
  to `hx_output.chat()`.
- **`task_error()`** with a HTLS_DATA_TASKERROR chunk, verifying the
  toast text is what we expected.
- **`hlwrite()` round-trip**: send a constructed HTLC_HDR_LOGIN with
  known chunks, capture the bytes that hit `htlc->out.buf`, parse
  them back via `hx_rcv_hdr` + chunk walker, assert structure
  matches.

This tier requires a small amount of mocking — at minimum, a fake
`hx_output` vtable that records calls instead of touching widgets.
That's a one-time setup cost (~1 day) that pays back across every
protocol test we write.

Estimated effort: **3-5 days** for the first batch covering the most
critical handlers, plus the mock-output infrastructure.

### Tier 3 — integration tests against mhxd

Real client behaviour: connect, log in, fetch users / news / files,
send chat, disconnect. Run against the **mhxd Docker container**
(`tests/mhxd/Dockerfile`). Two flavours:

- **Headless protocol-level tests** (recommended start) — reuse the
  protocol layer (rcv.c / commands.c / network.c) without GTK,
  drive a connection from a small test harness, assert against
  trace / state. Same code path as the real client minus the UI.
  Doable as a second binary that links against the protocol code.
- **Full GTK smoke tests** — launch the actual GtkHx binary
  pointed at the test mhxd, wait for "logged in" log line, kill
  it, assert no crashes / no Pango warnings. These are good for
  regression-catching but slow and finicky.

Estimated effort: **1-2 weeks** to get a useful headless harness
running against the Docker container. Big enough that I'd suggest
not starting it until Tier 1 + 2 are landed and shaping how we
modularize the protocol code.

### Out of scope

Some things are too hard to unit-test cost-effectively and we shouldn't
fight them:

- **GTK widget construction**: visual layout testing is brittle,
  doesn't catch real bugs, and would require running an X11 / Wayland
  display. The libadwaita-ification work didn't introduce regressions
  that visual tests would have caught and the ones that mattered
  showed up immediately when Misha ran the app.
- **Threading / timing-dependent behaviour**: race conditions in the
  worker-thread / gtk_threads_enter dance are real but hard to
  reliably reproduce in tests. The PING keepalive, post-login
  state machine, and preview-window concurrency are all in this
  category. Better tested through long-running soak tests or
  ThreadSanitizer in CI.
- **Worker thread cancellation paths**: same reason.

## Layout

```
tests/
  PROPOSAL.md           ← this file
  mhxd/
    Dockerfile          ← integration test server
    README.md
  meson.build           ← test() targets
  unit/
    test_hl_access.c    ← Tier 1
    test_text_to_utf8.c
    test_prefs_parser.c
    test_path_helpers.c
    ...
  proto/                ← Tier 2 (added later)
    test_rcv_chat.c
    test_rcv_selfinfo.c
    mock_output.c       ← shared fake hx_output vtable
  integration/          ← Tier 3 (added later)
    test_login_flow.c
    test_user_list.c
```

A test program is one binary per concern. Meson's `test()` target picks
up each binary and reports pass/fail individually. `meson test` runs
them all; `meson test test_hl_access` runs just one.

## Meson scaffolding (proposed shape)

In root `meson.build`:

```meson
if get_option('tests')
  subdir('tests')
endif
```

In `tests/meson.build`:

```meson
test_deps = [glib_dep, gtk_dep]   # GTK only because hl_access.h
                                  # uses gboolean. Pure-pure tests
                                  # can drop gtk_dep.

test_hl_access = executable('test_hl_access',
  'unit/test_hl_access.c',
  dependencies : test_deps,
  include_directories : include_directories('../src'),
)
test('hl_access', test_hl_access)

# ... per test
```

Plus a Meson option in `meson_options.txt`:

```meson
option('tests', type : 'boolean', value : true,
       description : 'Build the test suite')
```

Build + run:

```sh
meson configure build -Dtests=true
meson test -C build              # all
meson test -C build hl_access    # one
```

## Concrete starter PR

If we want to land the first piece this session or next, the
**smallest useful first step** is:

1. Add `tests/meson.build` with the scaffolding above.
2. Add `tests/unit/test_hl_access.c` with maybe 8-10 cases for
   `hl_access_has`: known bit positions on, off, out-of-range,
   the canonical `0x6070_00a0_0080_0000` vector we know maps to
   "guest with chat + msg + dl/up but no news".
3. Wire it into the root `meson.build` behind a `tests` option
   (default on so it runs locally, easily off for distro builds).

That's a half-day of work that gives us:
- A working test harness people can extend.
- A regression net for the access-bit code we just wrote.
- Real-world example for adding more tests.

After that lands and Misha is comfortable with the shape, we
expand into the rest of Tier 1 and start Tier 2.

## CI / coverage / future work

Once the test suite has a meaningful surface area:

- **GitHub Actions** workflow that runs `meson test` on every push.
  Trivial — Meson's TAP output drops straight into the test report
  pane.
- **AddressSanitizer / UndefinedBehaviorSanitizer** under CI for
  the test runs. Catches the kind of memory bugs that 23 years of
  C accumulates.
- **Code coverage** via `meson configure -Db_coverage=true` + lcov.
  Coverage isn't a goal in itself but it's useful to see which
  protocol paths haven't been exercised yet.
- **ThreadSanitizer** for the worker-thread paths once the protocol
  test harness exists.

These are all post-Tier-1 concerns; mentioning them here only so we
keep the path in mind while building out the basics.

## Open questions for Misha

1. Default `tests` Meson option to `true` or `false`? I'd argue `true`
   — encourages running tests locally during development; distros
   that want a leaner build pass `-Dtests=false`.
2. Mock `hx_output` strategy for Tier 2 — record calls into a list
   for assertion, or assert inline via callback? Both work; recording
   feels cleaner.
3. Anything specific you'd want covered first beyond the Tier 1 list?
   E.g., a known-flaky path you want a regression net on.
