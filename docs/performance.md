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
| 1 | CPU microbenchmarks, headless | criterion `benches/` in each crate | Started: `hxchat-layout`, `hxcrypto` |
| 2 | Throughput and latency over loopback, headless | Rust integration tests against an in-process fake server | Not started |
| 3 | UI scenarios through the real frame clock | a harness generalized from `src/chat_bench.c` | Chat only (`tools/chatbench.sh`) |
| 4 | End to end against the Docker rig | the integration tests' Docker rig | Not started |

### Tier 1 — microbenchmarks

```sh
cd rust
cargo bench -p hxchat-layout          # layout engine
cargo bench -p hxcrypto               # ciphers and hashes
cargo bench -p hxcrypto -- aead       # one group
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
- `hxtext` Mac Roman ↔ UTF-8 on long lines; `:shortcode:` lookup.
- `hxfiles-xfer`, `hxhfs`, `hxmacres`, `hx-image-decode`: the fork-header
  codec, sidecar reads, cicn and PICT decode, per-frame GIF decode.
- `hxmodel`: member-list upsert on a large login, nick completion.
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

Generalize `chat_bench.c` into one harness selected by environment variable,
run under `tools/isolated-run.sh`:

- Chat: the existing ingest + paint, relayout and scroll phases, plus the
  memory measurement the chat-view benchmark had to drop, done properly
  (heaptrack or `mallinfo2`, not an RSS delta).
- Users: login to a large server; a `USER_CHANGE` burst; every GIF icon
  arriving at once.
- Files: open, sort and filter a 10k-entry remote folder; a large local one.
- Tracker window: fill with a large listing; filter-typing latency.
- Chat history replay on join, interleaved with live messages.
- Animated media scrolled out of view — the acceptance test for the known
  offscreen-animation defect.
- Video: per-frame texture upload with four and nine tiles.
- Startup: first window, dock-layout restore, connected.

### Tier 4 — end to end

Login-to-usable time against mhxd, Janus and hxd-ng; a chat flood from a bot
client, measuring event-to-paint latency; an hour-long soak of flood plus
transfers, watching memory for growth in the per-connection caches and the
manually refcounted FFI objects.

### Profiling

sysprof (it shows GTK's frame marks), perf with hotspot, heaptrack for
memory. `GTKHX_DEBUG=bench` is the timing category.

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

## Findings

What the measurements have turned up. Findings 1 and 2 are fixed; the rest
are leads.

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
