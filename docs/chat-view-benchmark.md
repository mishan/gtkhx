# Chat view: xtext vs hxchat, measured

**Date:** 2026-07-27. **Verdict:** hxchat replaces xtext.

This is the record behind the C5 decision to delete `src/xtext.c` — 6,721
lines that had worked since 2000 and survived the GTK 2 → 3 → 4 climb.
Deleting code of that vintage on the strength of an impression would have
been a bad trade, so the two backends were measured against each other
while both were still in the tree. They cannot be measured against each
other again after C5; this file is the only place that comparison exists.

It also records two measurement failures, at length. Both produced
complete, plausible, entirely wrong result sets, and one of them was
within a commit of being written into `chat-view-scoping.md` as a finding
that argued against the design. The failures are more instructive than
the numbers.

---

## 1. What was compared

| | |
|---|---|
| **xtext** | `src/xtext.c`, HexChat's text widget vendored in Phase 2.6 (commit `e9bb312`) plus ~1,550 lines of GtkHx grafts. Line-uniform vertical layout: every coordinate derives from `fontsize × subline_count`. Wraps at append time. |
| **hxchat** | `rust/crates/hxchat-layout` (engine) + `hxchat-view` (GTK4 widget), phases C1–C4. Pixel-based variable-height layout, retained per-row layout cache, chunked prefix-sum height index. Wraps lazily, per visible row. |

Both were driven through the same `chat_view.h` seam, from the same
binary, in the same window, by the same append path. `GTKHX_CHATVIEW`
selected the backend and was the only difference between runs.

Harness: `src/chat_bench.c`, armed by `GTKHX_CHATVIEW_BENCH=<n>`, driven
by `tools/chatbench.sh`.

## 2. Environment

Misha's machine, Wayland, 60 Hz display (vsync interval **16.67 ms** —
this number matters more than it looks; see §6). One window size, one
theme, one session. 20,000 synthetic messages of 3–19 words each, five
cycling nick widths so the indent gutter settles early and the wrap path
sees varied line lengths rather than one cached width reused.

Three repeats per backend. Medians reported; every raw sample is in §4.

**These numbers are comparable between the two columns and nowhere
else.** Frame timings come from the GTK frame clock and include the
compositor's own work. Do not quote them across machines, display
refresh rates, or window sizes.

## 3. Results

| Metric | xtext | hxchat | Ratio |
|---|---|---|---|
| ingest + paint | 693.1 ms | 142.6 ms | **4.9× faster** |
| ingest alone | 29.0k msgs/s | 143k msgs/s | 4.9× |
| relayout, worst frame | 105.6 ms | 16.9 ms | **6.3× faster** |
| relayout, 10-frame total | 396.6 ms | 166.3 ms | 2.4× |
| scroll frame mean | 29.9 ms | 16.7 ms | 1.8× |
| scroll frame p95 | 42.1 ms | 17.3 ms | **2.4× faster** |
| RSS per 10k messages | — | — | **unmeasured** (§5) |

## 4. Raw data

Every sample from the final run. Times in ms.

**xtext**

| run | ingest | first paint | ingest+paint | relayout total | relayout worst | scroll mean | scroll p95 |
|---|---|---|---|---|---|---|---|
| 1 | 675.7 | 3.4 | 679.1 | 166.8 | 22.6 | 27.71 | 40.33 |
| 2 | 690.0 | 3.1 | 693.1 | 396.6 | 105.7 | 32.01 | 42.05 |
| 3 | 693.5 | 3.8 | 697.3 | 401.6 | 105.6 | 29.94 | 42.17 |

**hxchat**

| run | ingest | first paint | ingest+paint | relayout total | relayout worst | scroll mean | scroll p95 |
|---|---|---|---|---|---|---|---|
| 1 | 134.5 | 1.6 | 136.1 | 168.0 | 18.1 | 16.23 | 17.31 |
| 2 | 140.5 | 2.1 | 142.6 | 166.3 | 16.9 | 16.70 | 17.21 |
| 3 | 139.2 | 10.7 | 149.8 | 150.9 | 16.9 | 16.69 | 17.25 |

## 5. What each metric means

### Ingest + paint — 693 ms → 143 ms

Time to get 20,000 messages onto the screen.

Reported as two numbers that must be added, because the backends divide
the work differently and either half alone is misleading. xtext
line-wraps inside `gtk_xtext_append_entry` → `calc_lines`, so its cost
lands in *ingest*. hxchat stores the message and defers layout to the
frame that needs it, so its cost should land in *first paint*. Timing
only the append call would compare "did the work" against "wrote it
down" and would flatter hxchat for what is merely bookkeeping.

The interesting detail is that hxchat's first-paint figures (1.6–10.7 ms)
do **not** show the deferred cost arriving later. It never arrives,
because only the visible rows are ever laid out. The other 19,990
messages are stored, height-estimated, and never wrapped unless scrolled
to. That is the design working, not a measurement gap.

### Relayout — worst frame 105.6 ms → 16.9 ms

The load-bearing result, and the direct test of the `§3.2` claim that
reflow should cost O(visible) rather than O(scrollback).

A font change invalidates every cached width and every wrap point in all
20,000 messages. The phase changes the font, then samples the next ten
frames, reporting their total and the worst single one.

Divide the totals by ten. **hxchat averages 16.63 ms per frame against a
16.67 ms vsync interval.** Its frames after a total invalidation are
statistically indistinguishable from idle — it re-wrapped what was on
screen and nothing else. xtext averages 39.7 ms per frame over the same
window: roughly 230 ms of extra work, with a single 105 ms frame where
the whole-scrollback re-wrap lands.

At 60 Hz a 105 ms frame is six dropped frames — a visible lurch, and the
kind of thing that made resizing the window feel bad with a long
scrollback.

*Caveat, kept rather than averaged away:* one of three xtext runs showed
no spike at all (22.6 ms worst, 166.8 ms total — i.e. idle). Two of three
showed ~105 ms. The effect is real but not deterministic, most likely
depending on where the invalidation lands relative to xtext's `io_tag`
render timeout (`gtk_xtext_adjustment_timeout`). Three samples is enough
to establish the effect exists and not enough to characterise its
distribution.

### Scroll — p95 42.1 ms → 17.3 ms

Frame time while walking the buffer a third of a page per frame, 120
frames sampled, so each frame lands on rows that were not previously
laid out.

This is arguably the result that matters most day to day, because it is
what a user feels continuously rather than once. hxchat's 16.7 ms mean
and 17.3 ms p95 sit on the frame budget: it is vsync-bound, meaning it
has run out of work and the ceiling is the display. xtext's 29.9 ms mean
and 42.1 ms p95 mean it is missing roughly every other frame while
scrolling.

The honest framing is not "hxchat scrolls 2.4× faster" but "hxchat has
stopped being the bottleneck". A further 2× improvement to the widget
would not move this number at all.

### RSS — unmeasured

The harness reports a ~0.1 MB delta across ingest for both backends. That
is impossible: 20,000 messages of 3–19 words is several MB of text before
any per-message structure. The measurement is wrong — most likely the
allocator had already grown the heap during the warmup phase, so the
pages were resident before the delta window opened.

It is recorded here as **unmeasured**, not as parity. Nobody should cite
a memory comparison from this run. If memory becomes a question, the
harness needs a different approach (arena accounting inside the backends,
or `malloc_info`, not `statm` deltas).

## 6. Two ways this benchmark lied, and how it was caught

Both failures produced full, plausible, internally-consistent output.
Neither was caught by the numbers looking wrong. That is the part worth
remembering.

### 6.1 Both halves measured the same backend

`want_hxchat()` in `chat_view.c` accepted only `"new"` and `"hxchat"`.
The harness passed `GTKHX_CHATVIEW=0` and `=1` — the obvious spelling for
a boolean, and what the harness docs, the `chat_bench.c` header and the
`chat_find.rs` module comment all said. `=1` matched nothing and fell
through to xtext.

So the first complete run was six reports, three of them labelled as the
new backend, all six measuring xtext. Every number was plausible. The
comparison showed ingest parity and no reflow difference, which would
have been read as "the new backend isn't buying anything".

It was caught because the report prints which backend it actually used,
and Misha read that line. Nothing else in the output would have revealed
it.

Fixed by: accepting `1/true/yes/on` alongside `new/hxchat`; **warning**
on an unrecognised value instead of silently falling back; logging the
chosen backend on every startup rather than only when hxchat wins; and
making `chatbench.sh` exit non-zero unless it has seen a report from both
backends. The harness now catches its own misconfiguration.

### 6.2 The reflow phase measured nothing at all

The original reflow phase shrank the view's size-request
(`gtk_widget_set_size_request(view, width - 120, -1)`) and timed the next
frame tick.

The chat output is `hexpand`. Lowering a hexpanding widget's *minimum*
width does not change its allocation. Nothing was re-laid-out, in either
backend. Both reported ~16.4 ms, which is one vsync interval, and the
scoping doc came within one commit of recording "reflow: 16.4 ms vs
15.3 ms, no real difference" together with several paragraphs
speculating about why the O(visible) design might not be paying for
itself.

The tell was in the data and was read past: samples of 16.4, 16.4, 16.9,
12.7, 15.3 clustering on 16.67 ms is a frame interval, not a measurement.

**A benchmark result that sits suspiciously close to the frame interval
is probably measuring the frame interval.**

Fixed by: triggering the invalidation with a *font* change, which is
entirely client-side and needs no cooperation from the compositor; adding
a prep frame so the timed transition cannot be a no-op against whatever
the prefs had set; and sampling ten frames instead of one, so a backend
that re-wraps 20k messages cannot hide the cost in the frame after the
one being timed.

With the phase actually measuring something, the same comparison went
from "no difference" to 6.3×.

### 6.3 The general lesson

Both failures shared a shape: the harness produced numbers that were
*consistent with a reasonable hypothesis*, and the hypothesis was wrong.
In the first case the hypothesis was "these two runs differ"; in the
second, "this operation triggered a relayout". Neither was verified, and
neither would have been caught by looking harder at the results.

What caught them was checking the instrument against a known value — the
backend label in one case, the vsync interval in the other. A benchmark
that cannot be checked against something already known is not
trustworthy, however careful its statistics.

## 7. What this does and does not justify

**Justifies:** deleting xtext. Every correctly-measured metric shows a
large win, and in two of them hxchat is vsync-bound, meaning the widget
is no longer the limiting factor.

**Does not justify:** any claim about memory (§5), any claim about
behaviour at scrollback sizes far above 20k, or any cross-machine
comparison. The 105 ms xtext relayout spike is established as real but
not characterised — it appeared in two runs of three.

**Cannot be repeated.** After C5 there is one backend. The reflow harness
survives as a single-backend regression baseline, which is a different
and lesser thing: it can tell you that hxchat got slower, not that it was
ever better than what it replaced. That is why this file exists.

---

*Harness: `src/chat_bench.c`, `tools/chatbench.sh`. Summary table also in
`docs/chat-view-scoping.md` §6c.*
