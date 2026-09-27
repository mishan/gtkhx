# Performance testing

How GtkHx's hot paths are measured, what has been measured so far, and what
the measurements found. The chat view has its own, older record in
[chat-view-benchmark.md](chat-view-benchmark.md); read its §6 before trusting
any new harness.

## Principles

- **Check the instrument against a known value.** Both failures recorded in
  the chat-view benchmark produced plausible, complete, wrong numbers. Every
  harness here carries a check whose answer is known in advance — a size
  sweep that should not change the result, a raw primitive the wrapped one
  should match — and a harness whose check fails is broken until shown
  otherwise.
- **Medians and spread, from repeated runs, compared on one machine.**
  Numbers from different machines, display refresh rates or window sizes are
  not comparable.
- **CPU benchmarks and frame-clock benchmarks are different things.** CPU
  benchmarks are repeatable enough to gate CI eventually. Frame timings on a
  CI runner are noise and never gate anything.
- **Benchmark at seams that survive the port.** A scenario like "open a
  10k-entry folder" measures the Files browser whether it is written in C or
  Rust, so it doubles as the before-and-after check for the port. Record the
  baseline *before* a content port starts.

## Tiers

| Tier | What | Where | Status |
|---|---|---|---|
| 1 | CPU microbenchmarks, headless | criterion `benches/` in each crate | Started: `hxchat-layout`, `hxcrypto`, `hxtext`, `hxmodel`, `hxmacres` |
| 2 | Throughput and latency over loopback, headless | Rust integration tests against an in-process fake server | Not started |
| 3 | UI scenarios through the real frame clock | `gtkhx-ui`'s `bench` module, run by `tools/uibench.sh` | Started: chat, Files panel |
| 4 | End to end against the Docker rig | the integration tests' Docker rig | Not started |

### Tier 1 — microbenchmarks

```sh
cd rust
cargo bench -p hxchat-layout          # layout engine
cargo bench -p hxcrypto               # ciphers and hashes
cargo bench -p hxcrypto -- aead       # one group
cargo bench -p hxtext                 # Mac Roman, the wire encode, shortcodes
cargo bench -p hxmodel                # member list, nick completion, file list
cargo bench -p hxmacres               # icons.rsrc: open, look up, decode
cargo bench -p hxcrypto -- --warm-up-time 1 --measurement-time 3   # quicker
```

criterion is a dev-dependency only, with its default features off; nothing
that ships links it. Criterion keeps the previous run under
`target/criterion/` and reports the change against it, which is the
quickest way to check a change: bench on `main`, switch branches, bench
again.

Still to add:

- `hxproto` (in hx-libs, so hxd-ng gets them too): frame decode and dispatch
  of common transactions, a large user-list reply, a 10k-entry file list, a
  news listing.
- `hxcrypto` compression, once it is negotiated.
- `hxfiles-xfer`, `hxhfs` (in hx-libs), `hx-image-decode`: the
  fork-header codec, sidecar reads, PICT decode, per-frame GIF decode.
- The tracker codec, once it moves to hx-libs.

### Tier 2 — loopback

- The connection pipeline: frames per second from socket read through
  framing, cipher and the `hxbridge` ferry to a session signal, and the
  latency of one frame along that path. Plain, HOPE-Blowfish, AEAD, TLS.
- HTXF: MB/s for a large file each way; per-file overhead for a folder of
  many small files; and the rate of progress idles reaching the main loop,
  so a fast transfer cannot starve the UI.
- Tracker fetch of a large v3 listing, network to list model.

### Tier 3 — UI scenarios

```sh
tools/uibench.sh                              # chat=20000,files=10000, 3 repeats
tools/uibench.sh files=10000 5                # one scenario, 5 repeats
GTKHX_BENCH=chat GTKHX_BENCH_QUIT=1 ./build/src/gtkhx
```

The harness is `rust/crates/gtkhx-ui/src/bench/`. `hx_bench_maybe_start`,
called once the main window's chat view exists, reads `GTKHX_BENCH` and runs
the scenarios in order; each prints a report. Scenario code awaits the frame
clock — the next tick for sampling frame intervals, `after-paint` for "until
this work is on screen" — so a scenario reads as a sequence of steps.

A frame wait gives up after a few seconds — frames stop for a closed or
hidden window — and the report then carries a failed check rather than
hanging. Closing the Files window mid-run ends that scenario the same way.

Every report leads with the **idle frame interval**, measured before the
scenario does anything: the refresh interval on a real display, and the
floor under every frame number that follows. It has already caught one bug
in the harness itself — a first paint reported below it, because the paint
was being timed to the wrong point of the frame.

Run it with a scratch configuration so the app neither reads nor changes
yours, and cannot auto-connect anywhere:

```sh
T=$(mktemp -d); XDG_CONFIG_HOME=$T/c XDG_DATA_HOME=$T/d XDG_CACHE_HOME=$T/k \
  tools/uibench.sh
```

| Scenario | Measures | Checks |
|---|---|---|
| `chat[=N]` | The phases of the original chat-view benchmark: ingest + first paint, relayout after a font change, scrolling. | The idle frame. |
| `files[=N]` | The real files panel (`gtkhx-ui` `files::panel`) in its own window: populate from a synthetic FILE_LIST reply through the remote decode path; sort by size and by name; scrolling; listing a real N-file directory through the local provider. | Row count equals N; rows actually in size order after the sort. |

The panel has no filter, so none is measured.

Still to add: users (a large login, a `USER_CHANGE` burst, every GIF icon
at once); the tracker window (a large listing, filter typing); chat-history
replay on join; animated media scrolled out of view — the acceptance test
for the known offscreen-animation defect; video tiles; startup.

### Tier 4 — end to end

Login-to-usable time against mhxd, Janus and hxd-ng; a chat flood from a bot
client, measuring event-to-paint latency; an hour-long soak of flood plus
transfers, watching memory for growth in the per-connection caches and the
manually refcounted FFI objects.

### Profiling

sysprof (it shows GTK's frame marks), perf with hotspot, heaptrack for
memory.

### CI

Not yet. The plan is instruction counts (iai-callgrind) for Tiers 1 and 2,
reported but non-blocking until they have proven stable, then gating on
regressions. Tiers 3 and 4 are run by hand before a release, with results
added to the baseline below.

## Baseline

**2026-09-26**, AMD Ryzen 9 5900X, `cargo bench` defaults. Criterion's median
estimate. Comparable only with runs on the same machine.

### `hxchat-layout`

Against `FixedMeasure`, so this is the engine without Pango shaping. Width
800 px, viewport 600 px, 3–19-word messages with five nick widths.

| Benchmark | 2,000 rows | 20,000 rows | Expected shape |
|---|---|---|---|
| `ingest` (all rows) | 155 µs | 1.62 ms | O(n) — per-row cost flat |
| `first_paint` | 13.9 µs | 70 µs | O(visible) — **not met**; finding 3 |
| `relayout` | 4.4 µs | 8.0 µs | O(visible) — met, small O(n) invalidation |
| `scroll_walk` (120 frames) | 438 µs | 2.04 ms | O(visible) — was not met; finding 2 |
| `live_at_cap` (one message) | 27.9 µs | 285 µs | O(visible) — was not met; finding 1 |
| `search` | 1.68 ms | 17.0 ms | O(n) — by design |
| `parse_inline` (one line) | 225 ns | | |

After the fixes for findings 1 and 2 (same machine, measured the same
day, under more background load):

| Benchmark | 2,000 rows | 20,000 rows |
|---|---|---|
| `scroll_walk` (120 frames) | 463 µs | 430 µs |
| `live_at_cap` (one message) | 1.06 µs | 3.25 µs |

`scroll_walk` is now flat in the scrollback size. `live_at_cap` still
grows a little, which fits the height index's prefix repair — O(chunks),
not O(rows) — though that is not separately measured.

### `hxcrypto`

| Benchmark | 128 B | 16 KiB | 60 KiB (HTXF chunk) |
|---|---|---|---|
| `blowfish_ofb64` | 173 MiB/s | 177 MiB/s | 177 MiB/s |
| `blowfish_block` (raw) | | | 181 MiB/s |
| `aead_seal` | 93 MiB/s | 1.65 GiB/s | 1.81 GiB/s |
| `aead_open` | 75 MiB/s | 1.58 GiB/s | 1.79 GiB/s |

| Benchmark | Time |
|---|---|
| `blowfish_rollback` (save + restore) | 1.3 ns |
| `hope_rekey_63`, HMAC-MD5 | 51 µs |
| `hope_rekey_63`, HMAC-SHA1 | 34 µs |

The instrument check passes: Blowfish OFB-64 runs at the raw block cipher's
rate, so the per-byte XOR loop costs nothing measurable and Blowfish itself
is the ceiling — far above any Hotline link.

### `hxtext`

**2026-09-27**, as are the `hxmodel` and `hxmacres` tables below; same
machine and settings. Measured through the entry points GtkHx calls, so the numbers include
hxproto's primitives underneath and the `g_malloc` copy the C ABI hands
back.

| Benchmark | 80 B | 4 KiB | 64 KiB |
|---|---|---|---|
| `to_utf8`, already UTF-8 | 39 ns | 1.69 µs | 27.6 µs |
| `to_utf8/validate_copy` (the check) | 39 ns | 1.68 µs | 30.7 µs |
| `to_utf8`, Mac Roman | 276 ns | 11.4 µs | 182 µs |
| `for_wire`, UTF-8 negotiated | 12 ns | 53 ns | 928 ns |
| `for_wire`, Mac Roman, accented text | 7.45 µs | 406 µs | 6.45 ms |
| `for_wire`, Mac Roman, emoji text | 3.16 µs | 173 µs | 2.84 ms |

| Benchmark | Time |
|---|---|
| `shortcodes/to_emoji`, a line with four shortcodes | 646 ns |
| `shortcode_matches`, prefix `s` / `smi` / `thumbsup` | 30 µs / 14 µs / 12 µs |

The instrument check passes: the UTF-8 fast path runs at the rate of
`str::from_utf8` and a copy. Decoding Mac Roman runs at about 340 MiB/s.
Encoding to it is finding 10.

### `hxmodel`

Members are `user00001` upwards; the file list is one folder in ten and
Mac Roman names, so every name takes the decode path.

| Benchmark | 100 | 1,000 | 5,000 |
|---|---|---|---|
| `member_login` (all) | 36 µs | 353 µs | 1.73 ms |
| `member_change` (all, in place) | 13 µs | 131 µs | 688 µs |
| `member_leave` (the first) | 1.5 µs | 11.7 µs | 63 µs |
| `nick_complete`, one match, C ABI | 11.5 µs | 107 µs | 527 µs |
| `nick_complete`, one match, `MemberList` | 3.1 µs | 28 µs | 141 µs |
| `nick_complete`, every member matches, C ABI | 48 µs | 2.96 ms | **62.8 ms** |
| `nick_complete`, every member matches, `MemberList` | 39 µs | 2.90 ms | **62.4 ms** |

| Benchmark | 1,000 | 10,000 |
|---|---|---|
| `files_populate` | 574 µs | 5.49 ms |

Login, change and leave are what they should be: flat per member for the
first two, O(n) for a leave, which re-indexes the members behind it, and
none of them large. The one-match completion is O(n) as expected, the
C ABI's extra three-to-four times being its walk of the GObject model and
a copy of every name per Tab. The every-member case is finding 9.
`files_populate` is finding 11.

### `hxmacres`

Against the `icons.rsrc` GtkHx ships (613 `cicn`s).

| Benchmark | Time |
|---|---|
| `parse`, the whole file | 7.7 µs |
| `lookup`, `nth_res_of_type(0)` / first id / last id | 3.4 ns / 3.6 ns / 346 ns |
| `decode`, icon 135 (`DEFAULT_ICON`) | 1.25 µs |
| `decode`, every icon | 1.15 ms — 1.9 µs each |

The instrument check passes: looking up the first id costs what the
direct index does. `load_icon` does a lookup and a decode on every call
with no cache, but at under 2.5 µs for both, a 1,000-user login spends a
few milliseconds on icons. Not worth a cache yet.

### UI scenarios

**2026-09-26**, same machine, under `tools/isolated-run.sh` (Xvfb, software
rendering) — so the frame numbers are Xvfb's, not a desktop compositor's,
and the next baseline worth recording is one from a real display. Median of
three runs; the first run of a session is consistently slower (cold caches)
and is the spread to expect.

| Chat, 20,000 messages | |
|---|---|
| idle frame | 16.7 ms |
| ingest + paint | 69 ms |
| relayout, worst frame | 16.9 ms — one idle frame; the whole-scrollback re-wrap is gone |
| scroll p95 | 16.7 ms — at the frame floor |

| Files panel, 10,000 entries | |
|---|---|
| idle frame | 16.7 ms |
| remote populate (UI frozen) | **1.32 s** |
| remote populate + paint | 1.36 s |
| sort by size: call / until painted | 9.4 ms / 23 ms |
| sort by name: call / until painted | 65 ms / 72 ms |
| scroll p95 | 16.9 ms |
| local listing of a 10,000-file directory (UI frozen) | **3.4 s** |

Two numbers here are unsettled. The first sort that brings files to the
top, and the scroll that follows, sometimes take 170–190 ms instead of the
~23 ms and ~17 ms above: once in three runs here, and in every run of a
reviewer's 2,000-entry check. Something paid once per process on first
display would fit — loading icon textures for kinds not yet shown is the
leading guess — but it is not yet explained.

After batching the populate (finding 6), same setup, median of three:

| Files panel, 10,000 entries | Before | After |
|---|---|---|
| remote populate (UI frozen) | 1.32 s | 165 ms |
| local listing (UI frozen) | 3.4 s | 238 ms |

After caching collation keys (finding 8), same setup, median of five,
against `main` measured alongside:

| Files panel, 10,000 entries | Before | After |
|---|---|---|
| sort by name: call | 69.6 ms | 12.3 ms |
| sort by name: until painted | 78.2 ms | 22.8 ms |
| remote populate (UI frozen) | 182 ms | 172 ms |

The keys are built on first sort, so the populate — which sorts — would
show any cost; it is within the noise.

After building the rename editor only for a rename (finding 11),
**2026-09-27**, same setup, median of five. Measured before finding 8's fix,
so both columns include the slower name sort:

| Files panel, 10,000 entries | Before | After |
|---|---|---|
| remote populate (UI frozen) | 176 ms | 44 ms |
| remote populate + paint | 221 ms | 70 ms |
| local listing (UI frozen) | 244 ms | 80 ms |

## Findings

What the measurements have turned up. Findings 1, 2, 6, 8, 9, 10 and 11
are fixed; the rest are leads. Findings 6 onwards are from the UI scenarios.

1. **At the scrollback cap, each new message costs O(scrollback).** The same
   benchmark with no cap is flat at about 30 µs a message at both sizes, so
   the extra is all in the trim path. Once the buffer is full, every append
   trims the oldest row, `ChatBuffer::trim` marks the id → position map
   dirty, and the next frame's `scroll_offset` rebuilds the whole map — the
   likely bulk of it. Negligible at the default 500-row cap; 285 µs a
   message at 20,000, paid on every line a busy server sends.

   **Fixed** in two parts. Trimming from the front shifts every position by
   the same amount, so the map now keeps a base offset and drops only the
   trimmed ids. And trim re-decided message grouping for the whole buffer
   when only the new front row can change — grouping depends only on the
   row directly above. That second walk also ran on every chat-history
   insert, making a Load-Older batch cost O(scrollback) per row; inserts
   now regroup just the new row and the one below it.
2. **Every `scroll_to` copies every row id.** `ChatBuffer::scroll_to`
   collects all row ids into a fresh `Vec` so the anchor resolver can look
   one up. That is the O(n) term in `scroll_walk`: with the closure
   borrowing the rows instead (`rows.get(row).map(|r| r.id)`), the 120-frame
   walk at 20,000 rows falls from 2.04 ms to 430 µs, level with the
   2,000-row figure. **Fixed.**
3. **First paint pays for gutter settling across the whole scrollback.**
   Laying out the first visible rows widens the shared nick gutter, and
   each widening drops every row's cached layout and marks every height
   unmeasured — an O(n) walk, repeated until the gutter settles. In one
   side-by-side run, pinning the gutter (`set_indent_width`) took first
   paint at 20,000 rows from 63 µs to 13 µs. This is also the noisiest
   number in the table: a cold 20,000-row buffer is sensitive to cache
   state, and a reviewer's run under load measured 445 µs. It happens once per buffer, so this is a note rather than a
   problem; it would matter if the gutter kept widening during a session.
4. **Search takes a frame and more at large scrollbacks.** The find bar runs
   it once typing pauses for 120 ms (`chat_find.rs`), and it scans the
   model with a character-by-character case-insensitive match: 17 ms at
   20,000 rows, under a millisecond at the default cap. One dropped frame
   per pause, not per key.
5. **AEAD allocates per record.** `AeadState::seal` and `open` use the
   allocating `encrypt` / `decrypt` and copy the result out, rather than
   the in-place detached form. At 1.8 GiB/s it does not matter for
   throughput; it is allocator churn on the transfer path, and small
   records (75–93 MiB/s at 128 bytes) are dominated by fixed per-record
   cost.
6. **Populating the Files panel appends one row at a time.** Both providers
   do it — `fill` in `hxmodel`'s file-list decode, and the local provider's
   `do_list` — and every append fires `items-changed`, which the panel
   answers with a status-footer update and the sort model with an insert.
   10,000 entries froze the UI for 1.32 s. **Fixed:** both providers now
   collect the rows and replace the store's contents with one `splice`,
   one `items-changed` however large the folder. The remote populate is
   down to 165 ms and the local listing to 238 ms; what remains is building
   the entries and one sort.
7. **The local listing is synchronous.** A 10,000-file directory froze the
   UI for 3.4 s: enumeration, a content-type description per file, and the
   per-row appends above. With the appends batched it is 238 ms, still on
   the main thread; enumerating off it is the remaining half, and the one
   that still grows with directory size.
8. **Sorting by name costs 65 ms at 10,000 rows**, against 9 ms by size.
   `cmp_name` called `g_utf8_collate` on every comparison, which re-derives
   a collation key each time. **Fixed:** each entry derives its
   `g_utf8_collate_key` on first use and keeps it, and the sort compares
   keys bytewise — the same order. Sorting by name is down to 12 ms, level
   with size; the Kind column got the same treatment.
9. **Nick completion is quadratic in the number of matches.** When the
   prefix is ambiguous, `complete_styled` (`hxmodel::chat`) drops
   case-insensitive duplicates by checking each match against every match
   kept so far. With every member matching, one Tab takes 48 µs at 100
   members, 2.9 ms at 1,000 and 63 ms at 5,000. A one-letter prefix on a
   large server is the case that hits it. **Fixed:** the names arrive
   sorted case-insensitively, so duplicates are adjacent and comparing with
   the last match kept is enough. The every-member case is now 14 µs,
   132 µs and 651 µs, linear in the matches like the rest.
10. **Encoding to Mac Roman ran at 10 MiB/s**, about a microsecond for each
    non-ASCII character: 7.5 µs for an 80-byte line of accented text, 6.3 ms
    for 64 KiB. Emoji text was faster only because it has fewer non-ASCII
    characters per byte. For each one, `emoji_to_shortcodes` (hxproto, in
    hx-libs) tried all ten cluster lengths against the emoji table, each an
    exact binary search. The Mac Roman table's linear search was the other
    suspect, but replacing it moved nothing. **Fixed** in hx-libs: the
    search grows the candidate a character at a time and stops once no
    emoji starts with it, which finds the same longest match. The 80-byte
    line takes 1.3 µs and 64 KiB 0.95 ms, about 65 MiB/s. It reaches GtkHx
    with the next hx-libs pin.
11. **Every Files row built a text editor.** `files_populate` builds
    10,000 entries in 5.5 ms, yet the UI scenario's remote populate froze
    the UI for about 170 ms. Detaching the models one at a time put the rest
    on the column view: taking the new rows, it builds a couple of hundred
    at once — 205 here — and each Name cell was a `GtkEditableLabel`, which
    carries a whole text editor with its input method, shortcuts, context
    menu and styling. Those rows took 99 of the 141 ms. **Fixed:** the cell
    is a plain label, and the editor is built when a rename opens and
    removed when it closes.
