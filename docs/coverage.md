# Code coverage

GtkHx uses Meson's built-in coverage support (`-Db_coverage=true`) combined
with [`gcovr`](https://gcovr.com/) to produce an HTML coverage report. The
report exists so we can see at a glance where the test suite leaves the
biggest untested surface — it's a tool for picking what to test next, not a
gate.

## Quickstart

```sh
# Full report — Tier 1 + Tier 2 + Tier 3 (needs Docker matrix up).
tools/coverage.sh

# Fast path — Tier 1 + Tier 2 only, no Docker required.
tools/coverage.sh --quick

# Open the report in a browser when done.
tools/coverage.sh --open

# Wipe the build directory and reconfigure from scratch.
tools/coverage.sh --reset
```

Output lands at `coverage/index.html`. The `build-cov/` directory is the
coverage-instrumented build tree; both are gitignored.

## Prerequisites

Install one of the two coverage frontends:

```sh
# Preferred (modern, faster, better filtering):
sudo dnf install gcovr        # Fedora
sudo apt install gcovr        # Debian / Ubuntu
brew install gcovr            # macOS

# Fallback (the script auto-detects):
sudo dnf install lcov
sudo apt install lcov
brew install lcov
```

`gcov` itself ships with gcc; the script bails with a clear error if any
required tool is missing.

For the full report you need the Docker test matrix running. Bring up both
reference servers with the instructions in `tests/mhxd/README.md` and
`tests/janus/README.md`. The script warns if neither port is open before
launching the test run — and that warning is worth heeding, because an
unreachable server is a **hard test failure**, not a skip (see "Tier 3 and
the matrix" below).

## What's measured

`.gcovr.cfg` at the project root sets `filter = src/`, so the report covers
GtkHx's own C sources and nothing else. Everything outside that filter is
either test code (tests never count toward their own coverage) or build
output.

| Scope          | Included | Notes                                          |
|----------------|----------|------------------------------------------------|
| `src/`         | Yes      | The whole C production surface                 |
| `tests/`       | No       | Outside the filter                             |
| `build*/`      | No       | Explicit `exclude` — generated sources         |
| `subprojects/` | No       | Explicit `exclude` — wrapped dependencies      |

There are no per-file exclusions today. There used to be several — the vendored
chat widget, the vendored regex engine, the old RNG — and each entry went away
when its file did; what remains in `.gcovr.cfg` at those positions is a comment
recording the history. If you ever need to add one back (a newly vendored file,
or something on its way out), put it there with a comment saying why, so the
next person doesn't have to guess whether the exclusion is still earning its
place.

Branch coverage is enabled, but `exclude-throw-branches` and
`exclude-unreachable-branches` are on. Both suppress edges that aren't ours to
test: the former is C++ exception-handling machinery, which is a no-op in C but
which gcov still emits through inlined GLib header functions; the latter is the
compiler's "this can't happen" edges, typically from inlined assertions and
`__builtin_unreachable`. Counting either as an untested branch would make the
branch number less honest, not more.

## What this doesn't measure

**The Rust crates.** The `filter = src/` line means the report covers the C
tree only, and by now the majority of the codebase lives in
`rust/crates/` — the protocol, networking, crypto, transfer, chat-view,
session-object and a growing share of the UI. A high overall percentage in this
report says the remaining C is well tested; it says nothing at all about the
larger half of the program.

`.gcovr.cfg` contains a comment saying Rust coverage "is tracked via
cargo-llvm-cov separately". Treat that as aspirational: `cargo-llvm-cov` appears
nowhere else in the tree — not in `tools/`, not in any CI workflow, not in the
Meson build. There is no Rust coverage pipeline today. Wiring one up, and
deciding whether the two reports get stitched together or just live side by
side, is unclaimed work.

## Reading the report

The summary at the top of `index.html` shows three numbers:

- **Lines**: percentage of source lines that ran at least once.
- **Functions**: percentage of declared functions that ran.
- **Branches**: percentage of branch outcomes that ran (both sides of
  every `if` etc.).

Line coverage is the headline number; branch coverage is the more honest
one. A function with 100 % line coverage but 50 % branch coverage is a
function whose tests only exercised one side of every conditional —
that's where bugs hide.

Files are sorted **by uncovered-line count, heaviest first** in the
`sort-key = uncovered-number` setting. The point is to focus where
adding tests has the most leverage: a 2000-line file at 60 % covered
(800 uncovered lines) is a richer target than a 200-line file at 30 %
covered (140 uncovered lines), even though the percentage in the second
looks more alarming.

Click a filename in the listing to see line-by-line coverage with red
highlighting on untested lines. The right margin shows execution count
per line — a `1` on every test-only path is normal; a `0` next to
production code is a candidate for a new test.

## Workflow notes

- The coverage build dir (`build-cov/`) is separate from your normal
  `build/` dir. Coverage instrumentation adds `--coverage` to the
  compile flags, which produces `.gcno` / `.gcda` sidecar files
  alongside every object. Keeping it in a separate directory avoids
  re-linking your normal build whenever you toggle coverage on or off.
- The script re-uses `build-cov/` between runs (it just re-invokes
  `meson configure`), so subsequent runs are incremental. Pass
  `--reset` if you've changed something the meson configure doesn't
  pick up (rare).
- **Stale build directories reset themselves.** A meson build tree bakes in
  the absolute path of the source it was configured from, in both
  `meson-info/meson-info.json` and the regenerate command inside
  `build.ninja`. If `build-cov/` was created from a different checkout, a
  container, or a sandbox where the project was mounted somewhere else, then
  `meson configure` happily accepts the options change and ninja later dies
  with a confusing permission error against a path that doesn't exist here.
  The script probes both files, cheapest first, and if either disagrees with
  the current project root it prints what it found and forces a clean
  reconfigure. The second probe is not redundant: `meson configure` refreshes
  `meson-info.json`, so a tree can have a current-looking meson-info and a
  stale `build.ninja`, which is the one ninja actually uses.
- The script does **not** abort on test failure — partial coverage
  data is still useful, and you'll see the non-zero exit code reported
  in the final output if anything failed. Re-run the failing tests
  individually with `meson test -C build-cov <name> -v` to dig in.
- Re-running on the same `build-cov/` accumulates `.gcda` data
  across runs, which is what you usually want (it sums the coverage
  from all the test invocations). `--reset` wipes that history; use
  it when you want a clean baseline measurement.

## Tier 3 and the matrix

A Tier 3 test that can't reach its server **fails**. It calls
`g_test_fail_printf` with a diagnostic and returns non-zero. This is
deliberate and it is documented at the Tier 3 block in `tests/meson.build`:
earlier revisions called `g_test_skip` for the no-Docker case, and the silent
skips masked real bugs that only surfaced later, in coverage reports, long
after the change that broke them. A skip reads as a pass, which is exactly the
wrong signal.

So there are two honest ways to run the suite: bring the Docker matrix up, or
opt out of the whole tier explicitly with `--no-suite integration` (which is
what `tools/coverage.sh --quick` does by only requesting the `unit` and
`proto` suites). What you should not do is run the integration suite against
nothing and read the result as green.

`tools/coverage.sh` itself never aborts on test failure — partial coverage data
is still useful — so a full run without the matrix will produce a report, plus a
non-zero test exit code echoed at the end, plus a much lower percentage than
the code deserves. Its own header comment still describes the old skip-cleanly
behaviour; the test suite is the authority.

## Adding it to CI

Not enabled in CI today. If we want it later, the same `tools/coverage.sh
--quick` invocation works in the existing Fedora job — it stays inside Tier 1
and Tier 2, so it needs no containers — and the report can be uploaded as a
GitHub Actions artifact. Uploading to Codecov / Coveralls is a separate step
that needs an account on the service plus one repository secret.

## Static analysis

Coverage answers "what ran"; the analyzers answer "what looks wrong without
running". `tools/analyze.sh` drives all three, or one at a time:

```sh
tools/analyze.sh           # all three
tools/analyze.sh sanitize  # ASan + UBSan over the unit + proto suites
tools/analyze.sh analyzer  # GCC -fanalyzer
tools/analyze.sh tidy      # clang-tidy
```

Each uses its own build directory (`build-asan/`, `build-analyzer/`,
`build-tidy/`) and leaves a `*-summary.txt` next to its full log.

The `analyze` GitHub workflow runs the same three on every push and pull
request, in two jobs. The sanitizer job's test run **blocks** — an ASan or UBSan
finding reds the build. The `-fanalyzer` steps inside that same job, and the
entire clang-tidy job, are `continue-on-error`: their findings upload as
artifacts and never fail the build. The intent is that a category gets flipped
to blocking once its production-code count is clean and staying clean.

### Why GLib idioms trip `-fanalyzer`

Two patterns account for most of the residual noise, and neither is a bug in
our code:

- **`g_strdup` looks like it can return NULL.** GLib defines it as an inline
  function containing an `if (!str)` early return. The analyzer follows that
  branch into code where the argument was already checked non-NULL several
  lines above, concludes the result may be NULL, and warns on the next
  dereference. The check it is missing is one it can't see, because the guard
  and the call are in different functions.
- **`g_assert_nonnull` isn't modelled as a terminator.** It aborts on NULL, but
  the analyzer treats it as an ordinary call, so the classic
  `fp = fopen(...); g_assert_nonnull(fp); fwrite(..., fp);` shape warns on the
  `fwrite`. This one shows up almost entirely in test code, where that shape is
  the normal way to write a fixture.

Both are safely ignorable. If a specific site is noisy enough to be annoying, a
plain `g_assert` at the point of use gives the analyzer the fact it's missing
without changing behaviour.
