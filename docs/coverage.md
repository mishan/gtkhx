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

For the full report you also want the Docker test matrix running. Bring
up both reference servers with the instructions in `tests/mhxd/README.md`
and `tests/janus/README.md`. The script warns if neither port is open
before launching the test run — without the matrix, Tier 3 tests skip
cleanly but the overall percentage will look artificially low.

## What's measured

| Scope          | Included | Notes                                          |
|----------------|----------|------------------------------------------------|
| `src/*.c`      | Yes      | The whole production surface, with exclusions  |
| `src/xtext.c`  | No       | Vendored HexChat fork (4500 LOC)               |
| `src/dfa.c`    | No       | Vendored regex engine (2550 LOC)               |
| `tests/`       | No       | Test code itself never counts                  |
| `subprojects/` | No       | Wrapped dependencies                           |

Exclusions live in `.gcovr.cfg` at the project root. When you add a new
vendored or about-to-be-deleted file, append it there with a comment
explaining why.

Branch coverage is enabled, but `exclude-throw-branches` and
`exclude-unreachable-branches` are on to suppress the C++/inline-asm
edges gcov sometimes emits through GLib headers — those branches aren't
ours to test.

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
- The script does **not** abort on test failure — partial coverage
  data is still useful, and you'll see the non-zero exit code reported
  in the final output if anything failed. Re-run the failing tests
  individually with `meson test -C build-cov <name> -v` to dig in.
- Re-running on the same `build-cov/` accumulates `.gcda` data
  across runs, which is what you usually want (it sums the coverage
  from all the test invocations). `--reset` wipes that history; use
  it when you want a clean baseline measurement.

## Adding it to CI

Not enabled in CI today. If we want it later, the same `tools/coverage.sh
--quick` invocation works in the existing Fedora job — the harness skips
Tier 3 cleanly and the report can be uploaded as a GitHub Actions
artifact. Uploading to Codecov / Coveralls is a separate step that needs
an account on the service plus one repository secret.
