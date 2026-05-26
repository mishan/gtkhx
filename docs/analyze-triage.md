# Analyzer triage — baseline run

First run of `tools/analyze.sh` on `main`. This doc carves the
output into "vendored / not our problem", "real bugs worth fixing",
"false positives the analyzer can't prove", and "stylistic noise
to baseline." Numbers reflect the run on commit `0453306`.

## Headline numbers

| Tool                | Findings | Real bugs to fix | False positives | dfa.c only |
|---------------------|---------:|----:|----:|----:|
| GCC `-fanalyzer`    |       51 |   6 |   6 |  37 |
| clang-tidy          |     ~570 |  ~15 | ~50 | 586 |

ASan + UBSan on the full unit + proto suite passes clean. **No
runtime sanitizer findings** — the codebase is well-behaved on
the paths the tests exercise.

### After noise reduction (2026-05, claude/analyzer-noise-reduction)

After landing steps 1–3 from "Suggested execution order":

| Tool                | Before | After | Notes |
|---------------------|-------:|------:|-------|
| GCC `-fanalyzer`    |     51 |    11 | dfa.c (37) silenced via per-file pragma; 6 real bugs fixed; 6 false positives in GLib internals + test code |
| clang-tidy          |   ~570 |   ~80 | dfa.c (586) excluded via regex; `misc-unused-parameters` count drops dramatically with StrictMode=false (sites that already used `(void)x;`) |

Production-code `-fanalyzer` count is now **zero**. Remaining 11
are: 1 in `files_remote_provider.c` (GLib `g_strdup_inline` false
positive), 5 in test code (`g_assert_nonnull` not modelled as
terminator), 3 in GLib `gstrfuncs.h`, 2 in test integration harness.

## Recommendations

Ownership stance (Misha, 2026-05): xtext.c and hfs.c are **ours**
now — we ported xtext to GTK 4, no one else maintains hfs.c, and
treating them as untouchable third-party blobs would just defer
the cleanup. dfa.c is the only file we'd genuinely like to be rid
of; the eventual move is to replace it with a regex library
(GRegex / PCRE2) rather than maintain ~2500 lines of GNU regex
ourselves.

1. **Temporarily baseline `dfa.c` only**, as a stepping-stone to
   replacing it with a library. Per-file
   `#pragma GCC diagnostic ignored "-Wanalyzer-*"` block at the
   top of the file and a clang-tidy filter regex tweak to exclude
   it. The 37/51 `-fanalyzer` warnings and 586 clang-tidy findings
   in dfa.c are noise from the GNU regex codebase and aren't
   informing our work.
2. **Fix the six real `-fanalyzer` findings** below — section
   "Real bugs". Each is a small, scoped change in our own code.
3. **Walk the xtext.c and hfs.c findings as our own.** Both are
   now in-scope for cleanup. Plan: tackle the top clang-tidy
   categories (integer-widening, macro-parentheses, branch-clone,
   misleading-indentation) in batches as we touch the files.
   No need to do it all at once — chip away.
4. **`cicn.c` and `macres.c` similarly.** Mac classic resource
   parsing code, ours to maintain. Same treatment as xtext / hfs.
5. **Walk the clang-tidy findings in priority order** — section
   "clang-tidy by category". The top categories are stylistic
   (`misc-unused-parameters`, `bugprone-macro-parentheses`) and
   can be addressed file-by-file.

## Real bugs from `-fanalyzer`

These are in our own code, plausible to hit at runtime, worth a
defensive fix:

### 🔴 rcv.c:1169 — `post->item = item` on NULL post

`rcv_task_news_post` allocates `post` inside the `HTLC_DATA_NEWSDATA`
case of a chunk-walk loop. If the server replies without that
chunk (malformed packet, future protocol revision, or just an
unexpected order), `post` stays NULL and `post->item = item`
segfaults. Fix: check `post != NULL` before the assignment and
emit a debug warning + return on NULL.

### 🔴 chat.c:2577 — `gchat_with_cid` NULL deref in `hx_clear_chat`

```c
struct gtkhx_chat *gchat = gchat_with_cid (sess, cid);
gtk_xtext_clear (GTK_XTEXT (gchat->output)->buffer, 0);
```

Reachable when `hx_clear_chat` is called for a cid whose window
was destroyed but the model side still has a `chat` entry. NULL
check + early-return.

### 🔴 chat.c:286 — `chat_with_cid` NULL deref in `hx_part_chat`

```c
chat = chat_with_cid (&the_session, cid);
cid = htonl (chat->cid);
```

Same shape — caller-provided `cid` may not exist in the
hashtable. NULL check + early-return.

### 🟡 commands.c:459 — `dup2` on pipe fds without checking pipe()

```c
case 0:  /* forked child */
    ...
    if (dup2 (pfds[1], 1) == -1 || dup2 (pfds[1], 2) == -1) {
```

`pipe(pfds)`'s return wasn't checked before `fork()`. If pipe
fails, `pfds[1]` is uninitialised in the child. Fix: check
`pipe()` for `== -1` and bail before forking. Affects the
`/exec` shell-command-pipe feature — niche but a real bug.

### 🟡 chat.c:1973 — `chat_with_cid(sess, 0)->subject` in create_chat_window

The CLAUDE.md invariant says `chat_with_cid(sess, 0)` is never
NULL while the table exists. The analyzer can't see the
invariant. Either:
- Add a g_assert there (turns analyzer hint into documented
  invariant + UBSan-style check in debug).
- Just keep going — the invariant holds, and 5 other call sites
  use `chat_with_cid(sess, 0)` the same way.

Recommendation: g_assert. Cheap and aligns with the "load-bearing
invariants get asserts" pattern we use elsewhere.

### 🟡 chat.c:1651 — `match_text` possibly-NULL in nick completion

In `chat_input_key_press`'s Tab-complete path, `match_text` is
computed inside one branch of a 100+ line function. The analyzer
flags a path where it stays NULL across to the snprintf. Looking
at the code, the path is unreachable (the outer `if
(match_user == NULL)` block requires match_text to have been
assigned), but the analyzer can't prove it. Easiest silencer:
initialise `match_text = NULL` at declaration and add a defensive
`g_return_if_fail(match_text)` at the snprintf.

## False positives worth noting

### files_remote_provider.c:427 — `strrchr (parent, '/')` after g_strdup

The analyzer follows g_strdup_inline's `if (!str)` true branch,
which means `r->current_path` was NULL. But we already checked
`if (!r->current_path)` two lines above and returned. False
positive from GLib inline expansion. Silence with an
`g_assert(parent)` if it bothers us, otherwise leave.

### Test code — `g_assert_nonnull` followed by use

Five warnings (test_bookmarks.c × 3, test_chat_history.c,
test_hope_chacha20_chat_history.c, integration_harness.h) follow
the pattern:

```c
fp = fopen(path, "wb");
g_assert_nonnull(fp);
fwrite(..., fp);  /* analyzer flags this */
```

`g_assert_nonnull` aborts on NULL, but the analyzer doesn't model
GLib's assertion macros as terminators. False positive in test
code, no action needed.

### dfa.c — 37 warnings in vendored regex

GNU regex from 2003. We don't want to maintain this — the plan
is to replace it with a regex library (GRegex / PCRE2). Until
then, bracket the file with a pragma or filter regex so the
noise doesn't drown the signal.

## clang-tidy by category

Top check counts across all files (including vendored):

| Count | Check | Notes |
|------:|-------|-------|
|    84 | `misc-unused-parameters` | We use `(void)x;` to silence — `StrictMode=true` in `.clang-tidy` rejects that. Recommend flipping to `StrictMode=false` so `(void)x;` works. |
|    32 | `bugprone-implicit-widening-of-multiplication-result` | `int * int → size_t` chains. Real concern on ARM64 / 32-bit. Concentrated in `hfs.c`, `xtext.c`, `cicn.c`. |
|    19 | `bugprone-macro-parentheses` | Macros without parentheses around args. Mostly in `protocol.h` (`HN16(x)` family). Worth a cleanup pass on protocol.h. |
|    13 | `readability-redundant-declaration` | Function declared in header AND re-declared at use site. Mechanical cleanup. |
|     9 | `readability-duplicate-include` | Same header `#include`'d twice. Trivial fix. |
|     7 | `readability-inconsistent-declaration-parameter-name` | Header says `int foo(int x)`, .c says `int foo(int n)`. Cosmetic. |
|     6 | `readability-non-const-parameter` | Pointer params that could be `const`. |
|     4 | `bugprone-not-null-terminated-result` | `memcpy(buf, src, len)` where `buf` is later used as a C string. Look at each. |
|     4 | `bugprone-branch-clone` | `if (x) { foo(); } else { foo(); }` — usually a leftover from a refactor. |
|     3 | `bugprone-suspicious-string-compare` | `strcmp(a, b)` without `!= 0` or `== 0`. Some are deliberate (truthy = "different"), but readability win to spell out. |
|     3 | `bugprone-integer-division` | `int / int` consumed by a float operation. Look at the two xtext.c hits to see if we lose pixels somewhere. |
|     3 | `bugprone-assert-side-effect` | `g_assert(x = foo())` style — assignment inside assert. Real concern in `login_packet.c:349` and `xtext.c:5295/5373`. |
|     3 | `readability-misleading-indentation` | All in xtext.c, baseline. |
|     2 | `readability-redundant-control-flow` | `return;` at end of void function. Trivial. |
|     2 | `misc-redundant-expression` | `if (x && x)` patterns. |
|     1 | `bugprone-redundant-branch-condition` | `if (uid) ... if (uid)` in rcv.c:631. |
|     1 | `bugprone-misplaced-widening-cast` | xfers.c:1259, `(ssize_t)u32` after u32 arithmetic — cast doesn't widen what the author thought. |

Three `clang-diagnostic-error` entries are not real findings —
they're from clang-tidy not knowing how to handle the auto-
generated `gtkhx-resources.c` (built by `glib-compile-resources`,
no entry in compile_commands.json). Harmless; can be filtered by
adjusting the run-clang-tidy regex to exclude `gtkhx-resources.c`.

### clang-tidy findings by file (top 15)

| File | Count | Status |
|------|------:|--------|
| dfa.c | 586 | Replacing with library — temp baseline |
| xtext.c | 310 | Ours (GTK 4 port) — worth a pass |
| hfs.c | 195 | Ours — worth a pass |
| proto_helpers.c | 145 | Ours — worth a pass |
| chat.c | 139 | Ours — worth a pass |
| commands.c | 138 | Ours — worth a pass |
| protocol.h | 128 | Mostly macros — `bugprone-macro-parentheses` pass |
| gtkhx.c | 126 | Ours — worth a pass |
| rcv.c | 102 | Ours — worth a pass |
| xfers.c | 81 | Ours — worth a pass |
| connect.c | 48 | Ours — worth a pass |
| hfs.h | 44 | Ours (companion to hfs.c) |
| cicn.c | 44 | Ours (Mac icon decode) — worth a pass |
| macres.c | 42 | Ours (Mac resource parse) — worth a pass |
| login_packet.c | 31 | Recent code — fix the 3 bugprone hits |

## Suggested execution order

If we're going to chip away at this:

1. ✅ **Fix the 6 real bugs** (rcv.c:1169, chat.c:286/2577/1973/1651,
   commands.c:459). Shipped on `claude/analyzer-bug-fixes`. After
   this the prod-code `-fanalyzer` count drops to ~6 false positives.
2. ✅ **Flip `.clang-tidy` `StrictMode=false`** for
   `misc-unused-parameters`. Shipped on
   `claude/analyzer-noise-reduction`. Drops the subset of 84 that
   already had `(void)x;` casts in place.
3. ✅ **Baseline `dfa.c` only** (per-file pragma + clang-tidy regex
   filter). Shipped on `claude/analyzer-noise-reduction`. 586
   clang-tidy + 37 -fanalyzer noise findings gone. Track the
   GRegex/PCRE2 replacement as the long-term fix.
4. ✅ **Tackle `protocol.h` macro-parentheses**. Shipped on
   `claude/analyzer-protocol-cleanup`. Fixed HN16/HN32/dh_getint in
   protocol.h, plus stragglers in compat.h (X2X/atou{16,32}),
   xtext.c (is_del), macres.c (e_int{16,24,32}), and usermod.c
   (ENTRY + test/set/unset_bit). 28 warnings → 0.
5. ✅ **Duplicate-include + redundant-declaration cleanups**.
   Shipped on `claude/analyzer-protocol-cleanup`. Dropped 9
   duplicate `#include` lines and 13 redundant in-file extern
   prototypes whose declarations already live in the matching
   header.
6. **Walk xtext.c, hfs.c, cicn.c, macres.c findings** — these
   are ours; chip away at the bugprone-* clusters in batches
   when touching the files.
7. **Address the remaining `bugprone-*` clusters in core code**
   — assert-side-effect, integer-division, not-null-terminated-
   result, branch-clone. Each is small and surfaces a real
   concern even when benign.

After steps 1–3 (now complete), the per-PR signal-to-noise should
be good enough to flip the CI jobs from `continue-on-error: true`
to mandatory. Anything new failing the check would be a real
regression. Recommend flipping `gcc-analyzer` first (the
production-code count is zero); `clang-tidy` can follow once a
few more rounds knock the prod-code count down further.

## Reproducing

```sh
tools/analyze.sh           # all three
tools/analyze.sh sanitize  # ASan + UBSan
tools/analyze.sh analyzer  # GCC -fanalyzer
tools/analyze.sh tidy      # clang-tidy
```

Build dirs: `build-asan/`, `build-analyzer/`, `build-tidy/`. Each
leaves a `*-summary.txt` next to its full log.
