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
| 2 | Throughput and latency over loopback, headless | bench binaries against an in-process fake server | The connection pipeline, HTXF transfers, the tracker fetch |
| 3 | UI scenarios through the real frame clock | `gtkhx-ui`'s `bench` module, run by `tools/uibench.sh` | Started: chat, Files panel, Users panel, tracker window, chat media, startup, chat history, video tiles |
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

```sh
cd rust
cargo bench -p hxnet --bench loopback                # everything
cargo bench -p hxnet --bench loopback -- pipeline    # one section: pipeline, htxf, tracker
cargo bench -p hxnet --bench loopback -- htxf aead   # one transport of it
```

**The connection pipeline** (`hxnet/benches/loopback/main.rs`). Each transport
— plain, TLS, HOPE with Blowfish, HOPE with ChaCha20-Poly1305 — connects
through the production entry point, `hxnet_connection_open_*`, to a fake
server on 127.0.0.1 with its own thread and runtime. A frame then takes a
server's frame's path: socket read, TLS or the HOPE cipher, framing, the
actor's event channel, the ferry to the GLib main loop, and the event
callback. The callback only checks the frame and frees it; dispatch, the
handler and the session signal are in the binary, not the crate, and
aren't measured. Per transport:

- **connect**: open call to handshake done.
- **throughput**: a 100,000-frame burst of chat-sized frames; frames a
  second, and the CPU each thread spent per frame — the main thread (the
  ferry and the callback), the client's runtime (read, decrypt, framing)
  and the server. The server buffers its writes, as a busy one would; a
  write per frame measured the server's syscalls instead of the client.
  Each frame is still its own cipher record — under TLS the buffer sits
  beneath it — so the client decrypts per frame, as it would from a real
  server.
- **latency**: one frame at a time, from just before the server's write —
  its encryption included — to the callback, p50 and p99.

Known values: every frame arrives, in order, byte for byte; and the
latency's p50 and p99 sit above a raw loopback floor's — the same pings,
unencrypted, read off a plain socket on a thread of their own.

**HTXF transfers** (`benches/loopback/htxf.rs`): a 256 MiB file down and
up, and a folder of 1,000 4 KiB files down, over plain TCP, TLS and HOPE's
AEAD. Each connects through `hxnet_htxf_connect` and copies through the
production workers on a thread of their own, as the app's blocking pool
runs them; their progress callback posts to the main loop as the app's
does, and the main loop counts the posts and its own CPU. The AEAD
transfer's keys come from a real HOPE-AEAD login against the pipeline's
fake server. Files go to a tmpfs where there is one, so the disk isn't
what's timed. Known values: every byte arrives as sent, and a download
is held under the raw loopback socket's speed, measured alongside.

**The tracker fetch** (`benches/loopback/tracker.rs`): a v3 listing of
2,000 and of 10,000 servers from a fake tracker, opened through
`hxnet_tracker_fetch_open` — TLS first, then in the clear, as against a
real tracker that doesn't speak TLS — and drained the way `network.c`
drains it, a 50 ms timeout polling until empty. Reported: the fetch, open
to the last record; the tail, from the tracker's last write to the last
record on the main loop; and the drain ticks it took, counting the one
that finds the fetch finished. Known values: every
server arrives, in order, with its name and port.

### Tier 3 — UI scenarios

```sh
tools/uibench.sh                              # startup,chat=20000,…,media=50,history=1000, 3 repeats
tools/uibench.sh files=10000 5                # one scenario, 5 repeats
GTKHX_BIN=build-voice/src/gtkhx tools/uibench.sh video=9   # needs a voice build
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
| `files[=N]` | The real files panel (`gtkhx-ui` `files::panel`) in its own window: populate from a synthetic FILE_LIST reply through the remote decode path; sort by size and by name; scrolling; listing a real N-file directory through the local provider — the call, the wait until the listing lands, and the longest frame meanwhile. | Row count equals N; rows actually in size order after the sort. |
| `users[=N]` | The main window's real Users panel, fed through the real receive handlers against the unconnected session: a login's USER_LIST reply for N users; a USER_CHANGE for every one of them at once; a GIF icon for every one at once through `gtkhx_avatar_update`, as a GIF-icons server's ICON_GETLIST reply delivers them; then frames and scrolling with every icon animating. Clears the list before and after through `users-clear`. | Row count equals N; every row shows the new status after the burst; every icon decodes and animates. |

| `tracker[=N]` | The real tracker window, opened without fetching: a listing of N servers delivered the way a fetch's drain delivers them — `tracker-batch-begin`, then per server an event from `hx_tracker_server_new_v3`, the `tracker-server-create` signal and the Tasks progress tick, all in one main-loop turn; then two searches typed into the window's own entry a key at a time, each cleared after: one that narrows the list from its first keys, one that keeps every server until its last few. Keys are inserted as typing inserts them, and the entry's typing delay is held off, so each keystroke measures the filter. Refuses to run in a tracker window the user already has open. | N servers listed; each search shows exactly the servers its regex matches, counted independently; clearing shows all N. |
| `media[=N]` | The main window's real chat view, cleared, then filled with N animated inline images followed by a few hundred lines of text, so pinned to the bottom it shows none of them. Three states, a few seconds each: text only, the images out of view, the images on screen — frames the clock ran, paints and main-thread CPU, per second. The acceptance test for offscreen animation. Nothing has the keyboard focus for the run: a focused text cursor blinks, repainting every frame while it fades. | On screen, the animation repaints; out of view, it doesn't. |
| `startup` | Launch to a usable main window, timed from `GTKHX_BENCH_T0` (the launch time `tools/uibench.sh` stamps) or else from `/proc` to 10 ms: the chat panel built (the bench hook, before the main loop starts), the chat view's first paint, and settled — the main loop's first low-priority idle after it — plus the main thread's CPU to first paint. Always runs first. The first run on a fresh configuration is a first run: Settings opens, and caches start empty. | The moments come in order; a launch time was found. |
| `history[=N]` | The main window's real chat view, against the unconnected session: a chat-history replay of N entries through `rcv_task_chat_history` — the parse, the `chat-history-batch` signal and `chat.c`'s renderer — then a "Load older" page of N rows inserted one at a time above an anchor through the view's insert-above call, as the renderer inserts an older page (that path's own trigger is C state with no accessor), then a scrollback's worth of live messages, timed. Refuses a connected session, and runs only when the app exits afterwards (`GTKHX_BENCH_QUIT`), since it leaves `chat.c`'s own "Load older" cursor on the fake replay; clears the view and restores the connection's history cursor. | The replay adds N rows and its three framing rows; the page adds N rows; live traffic past the cap keeps every history row. |
| `video[=N]` | The Video panel's tiles (`gtkhx-ui` `video_panel`), in a panel of its own in its own window, tied to no connection: N cameras at 640×480 and 30 fps, then the same with a 1920×1080 screen share at 15 fps on the stage. A feeder thread stands in for the receive bins' appsinks — it stores RGBA frames in the runtime's own `FrameStore` and posts a notice when none is on its way — and the notice does what the panel's does. Per phase: tiles up to first paint; frame intervals; the main thread's CPU; notices a second and their cost; frames each tile showed a second against the rate sent. Decoding is left out: the frames are RGBA from the start. Needs a voice build. | Every tile shows at least nine in ten of the frames sent to it and no more, and ends on a picture of its stream's size. |

The Files panel has no filter, so none is measured. The Users scenario
refuses to run on a connected session: it writes fake users into the public
chat and clears it afterwards.

All the planned scenarios are in.

Frames only come while the compositor is presenting the window. On a locked
desktop GNOME holds them back, and every scenario that samples frames then
reports that a frame never came. A headless mutter measures on the GPU all
the same:

```sh
dbus-run-session -- sh -c '
  mutter --headless --no-x11 --wayland-display=hxbench-0 \
    --virtual-monitor 1920x1080@60 & sleep 3
  WAYLAND_DISPLAY=hxbench-0 GDK_BACKEND=wayland \
    GTKHX_BIN=build-voice/src/gtkhx tools/uibench.sh video=9
  kill $!'
```

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

After moving the local listing onto a worker (finding 7), same setup,
median of five, against `main` measured alongside:

| Files panel, 10,000 entries | Before | After |
|---|---|---|
| local listing: navigate call (UI frozen) | 82.8 ms | 0.01 ms |
| local listing: until on screen | 110 ms (call + first paint) | 110 ms (until listed + first paint) |
| local listing: longest frame | 110 ms — the call and the paint | 55 ms |

The wall time is unchanged; what moved is where the main thread waits.

The Users scenario, **2026-09-27**, same setup, median of three. "Before"
is this branch with each fix backed out, measured in the same session; a
decode that never finished is marked as such.

| Users | Before | After |
|---|---|---|
| login, 1,000 users: into the store (UI frozen) | 42.7 ms | 16.0 ms |
| login, 1,000 users: login + paint | 59.1 ms | 33.0 ms |
| USER_CHANGE burst, 1,000 users (UI frozen) | 2.0 ms | 2.0 ms |
| 200 GIF icons: until decoded | 3.9 s | 146 ms |
| 200 GIF icons: longest frame | 3.7 s | 21 ms |
| 500 GIF icons: until decoded | never | 326 ms |
| scroll p95 with every icon animating | 16.7 ms | 16.7 ms |

The first run in a session is consistently slower on the login paint (about
75 ms after), and is the spread to expect.

The tracker scenario, **2026-09-27**, same setup, median of three. "Before"
is this branch with finding 16's fix backed out. The listing is timed
through to the stores, splice included.

| Tracker window, listing | Before | After |
|---|---|---|
| 2,000 servers (UI frozen) | 187 ms | 43.3 ms |
| 2,000 servers: listing + paint | 239 ms | 93.2 ms |
| 10,000 servers (UI frozen) | 1.81 s | 76.5 ms |
| 10,000 servers: listing + paint | 1.86 s | 128 ms |

| Tracker window, searching | 2,000 servers | 10,000 servers |
|---|---|---|
| narrowing query: keystroke / until painted, mean | 8.7 / 26.6 ms | 12.5 / 26.9 ms |
| broad query: keystroke / until painted, mean | 2.3 / 16.3 ms | 6.8 / 18.2 ms |
| clear search: until painted | 66 ms | 72 ms |

The media scenario, **2026-09-27**, same setup, 50 animated images at 10
frames a second, median of three. "Before" is this branch with finding
18's fix backed out.

| Chat view, animated images | Before | After |
|---|---|---|
| out of view: frames / paints a second | 60 / 10 | 0 / 0 |
| out of view: main-thread CPU | 9.7 ms/s | 0 |
| on screen: frames / paints a second | 60 / 10 | 60 / 10 |
| on screen: main-thread CPU | 13.3 ms/s | 13.1 ms/s |

The startup scenario, **2026-09-27**, same setup, median of four normal
launches (a first run excluded). "Before" is this branch without finding
21's fix.

| Startup, from launch | Before | After |
|---|---|---|
| chat panel built (bench hook) | 228 ms | 228 ms |
| first paint | 372 ms | 366 ms |
| settled (main loop idle) | 975 ms | 392 ms |
| main-thread CPU to first paint | 312 ms | 310 ms |

The history scenario, **2026-09-27**, same setup, median of three, with the
default 500-row scrollback. "Before" is this branch without finding 19's
fix. The replay now keeps every entry, since history doesn't count against
the cap (finding 20), so a 5,000-row page lands in a buffer of about 10,000
rows rather than 5,500; what remains is linear per insert.

| Chat history | Before | After |
|---|---|---|
| replay, 50 entries: call / + paint | 0.3 / 23 ms | unchanged |
| replay, 1,000 entries: call / + paint | 3.9 / 23 ms | unchanged |
| live message at the cap, `history=1000` (~2,000 history rows) | — | 2.8 µs |
| "Load older", 50 rows (UI frozen) | 0.15 ms | 0.14 ms |
| "Load older", 1,000 rows (UI frozen) | 13.8 ms | 4.1 ms |
| "Load older", 5,000 rows (UI frozen) | 170 ms | 54 ms |

The video scenario, **2026-09-28**, under a headless mutter (above) so the
GPU renders, and under `tools/isolated-run.sh` for comparison. Median of
three to five runs for nine cameras; the other rows are one or two runs
each, a guide to the shape rather than a baseline. Every run showed every
tile at the rate it was sent — 30 fps cameras, 15 fps screen — with frames
at the refresh interval.

| Video panel, main-thread CPU | headless mutter | Xvfb |
|---|---|---|
| 1 camera | 2.3 % | 3.1 % |
| 1 camera + screen share | 5.3 % | 9.9 % |
| 9 cameras | 7.5 % | 21 % |
| 9 cameras + screen share | 9.5 % | 21 % |
| 25 cameras + screen share | 11 % | 21 % |

Frame notices ran at about 270 a second for nine cameras, one per frame —
the streams run out of step, so little lands together to coalesce — at
6 µs each.

### Loopback

**2026-09-28**, same machine, median of three runs of the bench's own
median of five. "Before" is without finding 24's fix — its read buffer
set to pass-through. Latency and connect time didn't move and are given
once.

| Connection pipeline | Before: frames/s | After: frames/s | Client runtime, ns/frame, before → after | Main thread, ns/frame | Latency p50 / p99 |
|---|---|---|---|---|---|
| plain | 609,000 | 2,380,000 | 1,640 → 420 | 370 | 16 / 25 µs |
| TLS | 1,640,000 | 1,600,000 | 610 → 620 | 330 | 17 / 26 µs |
| HOPE-Blowfish | 125,000 | 159,000 | 8,000 → 6,300 | 245 | 19 / 101 µs |
| HOPE-AEAD | 314,000 | 583,000 | 3,180 → 1,710 | 250 | 20 / 36 µs |

The raw socket floor is 10 µs p50, 15 µs p99; connecting takes 0.2 ms, or
3 ms with TLS's handshake. Plain's throughput swings between runs more
than the others' — from 1.4 to 2.5 million frames a second — with the
CPU per frame on both client threads moving with it, which points at
where the scheduler places the threads rather than at the code.

HTXF, same machine and day, median of two runs of the bench's own median
of three; files on `/dev/shm`. "Before" is without finding 27's
throttle. The raw socket floor is about 5 GB/s.

| HTXF | Download: before → after | Main thread during it, before → after | Upload | Folder, per file: before → after |
|---|---|---|---|---|
| plain | 2.52 → 2.67 GB/s | 180 → 1.5 ms/s | 2.2 GB/s | 80 → 67 µs |
| TLS | 1.32 → 1.58 GB/s | 330 → 1.7 ms/s | 1.5 GB/s | 122 → 113 µs |
| AEAD | 0.95 → 1.02 GB/s | 84 → 0.8 ms/s | 0.97 GB/s | 92 → 87 µs |

Progress posts went from 28,000–80,000 a second to 20–26.

The tracker fetch, median of five, after finding 28's fix: 2,000 servers
in 50 ms and 10,000 in 52 ms, every run in a single drain tick; 48 ms of
either is the wait for that tick, the fetch itself taking a few. Before
it, a run could take several ticks — 2,000 servers in 50 to 150 ms here,
10,000 in up to 900 ms in a reviewer's runs.

## Findings

What the measurements have turned up. Findings 1, 2, 6, 7, 8, 9, 10, 11,
12, 13, 15, 16, 18, 19, 20, 21, 23, 24, 26, 27 and 28 are fixed and 14
is worked around; the rest are leads. Findings 6 to 23 are from the UI scenarios, 24
onwards from loopback.

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
   that still grows with directory size. **Fixed:** the folder is read on
   GLib's worker pool (`gtkhx-ui` `files::local`), with each content type
   described once rather than once per file, and the main thread only
   builds the rows and splices them in — about 2 ms for the rows. The
   longest frame while listing fell from 110 ms to 55 ms, and what is left
   is the splice: the column view taking the new rows and the sort, the
   same cost a remote populate pays. Starting a listing cancels the read
   before it, and one that lands after being overtaken is dropped. The
   provider's current path moves when its listing lands, not when
   `navigate` is called, so a delete or rename in between still acts on the
   folder the user is looking at.
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
    line takes 1.3 µs and 64 KiB 0.95 ms, about 65 MiB/s.
11. **Every Files row built a text editor.** `files_populate` builds
    10,000 entries in 5.5 ms, yet the UI scenario's remote populate froze
    the UI for about 170 ms. Detaching the models one at a time put the rest
    on the column view: taking the new rows, it builds a couple of hundred
    at once — 205 here — and each Name cell was a `GtkEditableLabel`, which
    carries a whole text editor with its input method, shortcuts, context
    menu and styling. Those rows took 99 of the 141 ms. **Fixed:** the cell
    is a plain label, and the editor is built when a rename opens and
    removed when it closes.
12. **Every GIF icon at once froze the UI, and past a few hundred never
    finished.** A GIF-icons server answers ICON_GETLIST at login with every
    user's icon, and each started a decode. A decode is a D-Bus call to
    glycin's pooled loader, and every call waiting on its reply checks every
    message the connection receives — on the main thread, so N decodes in
    flight cost N² there. 50 icons froze the UI for 0.6 s and 200 for 3.7 s;
    at 500, not one had finished after a minute. **Fixed:** decodes take one
    of a few slots first (`hx-image-decode`'s `slots`), in order, so the
    rest wait for free. More slots decode faster but bring the long frames
    back, and more so on more cores, where the loader answers in bursts:
    at 500 icons, 8 slots gave 36 ms frames on 2 cores and 350 ms on 24.
    Four stay near 30 ms on both. Every image decode in the app shares the
    slots, so a decode that holds one past a generous deadline fails and
    lets it go — a loader that never answered would otherwise stall every
    decode after it.
13. **Animated images were decoded over and over.** glycin's
    `next_frame` loops back to the first frame when an animation ends, and
    the decode loop stopped only at the frame or duration cap: a two-frame
    looping icon came back as 256 frames, and each decode took about 85 ms
    in the loader. **Fixed:** the loop stops when glycin's frame index comes
    round to the first frame's again. (An animation long enough to reach
    the duration cap first stopped there instead.) With finding 12's slots, 500 icons
    decode in 0.3 s with no frame over 24 ms, and 1,000 in 0.55 s.
14. **glycin's loader leaks a thread per animated image, and then fails
    every decode.** glycin's image-rs loader keeps a thread alive for each
    animated image it decodes, and releasing the image doesn't end it: 60
    icons leave the loader at 67 threads, and they stay until the loader has
    sat idle long enough for glycin to shut it down. Under glycin's bwrap
    sandbox — the usual case on a desktop — the loader runs under an
    address-space limit glycin sets from free memory, and each thread takes
    a slice of it, so once enough have built up it cannot start another and
    every decode it holds fails. Where that happens depends on free memory:
    about 2,000 icons on one run, about 100 of 1,000 on another with 7 GB
    free. Without that sandbox (flatpak-spawn, or unsandboxed) there is no
    limit, and the threads simply pile up. **Worked around:** `hx-image-decode` gives new
    decodes a fresh glycin pool, and so a fresh loader process, every 32
    images. glycin shuts a pool's loader down about 30 s after its last use,
    and the leaked threads go with it; no loader holds more than a few dozen.
    1,000 icons decode in 1.3 s and 2,000 in 2.8 s, every one of them, at
    the cost of starting a loader per 32 icons — 0.55 s for 1,000 when the
    leak hadn't yet bitten, and a longest frame of about 70 ms instead of
    30. glycin 2 has the same bug, but starts a loader per image and ends
    it with the image, so nothing builds up. The leak itself is upstream's
    to fix.
15. **A login added users to the list one row at a time.** Each
    `hx_user_list_view_add` appended to the store, and each append cost the
    sort model and the column view a round of work — the same shape as
    finding 6. **Fixed:** the view queues new rows and adds them in one
    splice from a high-priority idle, which runs before the next frame. At
    1,000 users the UI is frozen for 16 ms instead of 43 — the handler and
    the one splice, timed together — and login plus paint went from 59 ms
    to 33 ms.
16. **A tracker listing added servers one row at a time, in quadratic
    time.** A fetch delivers every ready record in one main-loop turn, one
    `tracker-server-create` each, and each appended to the section's store
    and rewrote the section title's markup. Every append's `items-changed`
    made the column view's list item manager walk its whole row tree, so
    the listing grew with the square of its size: 187 ms at 2,000 servers,
    1.81 s at 10,000. **Fixed:** records queue per section and land in one
    splice from a high-priority idle, which runs before the next frame, with
    the titles and counts brought up to date once: 43 ms and 77 ms, splice
    included.
17. **A search keystroke that hides rows costs a dropped frame.** What a
    key costs depends on how much it changes which rows show. One that
    narrows the list costs 9–13 ms, and about 27 ms until painted, at 2,000
    servers and at 10,000 alike; one that leaves the rows as they were costs
    2–7 ms. Clearing the search, which brings every row back, takes 66–72 ms
    to paint. The filter's own matching is a small part of it — most is GTK
    rebuilding row widgets for the rows that change. Moving the sort below
    the filter, so a search never reaches the sort model, measured no
    different. A lead.
18. **Animated images scrolled out of view kept the chat view busy.** The
    view ran one frame tick whenever any image in the scrollback was
    animated, advanced every one of them, and repainted whenever one
    advanced — so a GIF long scrolled away kept the frame clock at 60 frames
    a second and repainted the view at the GIF's own rate, for as long as it
    stayed in the scrollback. **Fixed:** the snapshot records which images
    it drew, only those advance, and the tick stops itself once none of
    them is animated; the next snapshot that draws one starts it again.
    Unmapping the view — a tab switched away, the window hidden — forgets
    what was drawn, since no snapshot runs then to say so. Out of view, 50
    animated images now cost no frames, no paints and no CPU, against 60
    frames, 10 paints and 9.7 ms of CPU a second before. The
    user list's animated avatars have the same shape on their own timer in
    `gif_avatar.c`, and are still ungated.
19. **A "Load older" page cost time growing with the scrollback, for every
    row.** The renderer inserts an older page a row at a time above one
    anchor. Each insert dirtied the buffer's id-to-row map, and the next
    lookup — the insert anchor, then the reading position the scrollbar is
    set from after every change — rebuilt the whole map: a page of N rows
    cost N rebuilds. 1,000 rows took 14 ms and 5,000 took 170 ms. **Fixed:**
    the buffer remembers the rows it was last asked for, moves them with
    every insert, removal and trim, and checks each against its row before
    trusting it: 4.1 ms for 1,000 rows. The default page is 50 rows, which
    never showed it. What remains grows with the buffer per insert — the
    height index's insert — which only a very large page would notice.
20. **A "Load older" page on a full scrollback was gone at the next
    message.** Inserting above never trimmed, so a page took the buffer past
    its row cap, and the next appended message trimmed everything over the
    cap from the top — the whole page the user had just asked for, and the
    oldest live row with it. With the default 500-row cap, that was any
    "Load older" once the chat had seen 500 rows. **Fixed:** history rows —
    the replay, older pages, and the rows that frame them — don't count
    against the cap, and a message over it drops the oldest *live* row,
    wherever it sits. History — the replay on join, a reconnect's catch-up,
    older pages — is bounded by the server and the replay preference rather
    than the cap, and cleared on reconnect. The view learns which rows are
    history from `chat.c`, which draws every history row entirely in the
    history palette slot and nothing else in it. With history at the top,
    the live row dropped is below it rather than at the front; removing it
    by position keeps each message at the cap to about 0.5 µs under ~100
    history rows and 9.4 µs under ~10,000.
21. **Every chat input built GTK's emoji chooser at startup.** The emoji
    button made its `GtkEmojiChooser` up front, and GTK fills a new chooser
    with every emoji, measuring each glyph, in idle batches — most of the
    main thread for about 600 ms after the window first appeared, and again
    for every private chat and message window, for a picker most sessions
    never open. **Fixed:** the chooser is built the first time the button
    opens. The main loop now settles 392 ms after launch instead of 975.
22. **Startup is almost all CPU, spread thin.** About 310 ms of the ~365 ms
    to first paint is main-thread CPU. A warm-launch profile shows no single
    hot spot: page faults (~11%) and the dynamic linker resolving symbols
    (~7%) — a good part of it loading and linking a large binary and its
    libraries — with the rest spread across allocation, CSS and GL setup.
    A lead.
23. **The Video panel keeps up, and tiles out of view cost the UI almost
    nothing — but they were still received.** Every tile showed every frame
    sent, with frames at the refresh interval, from one camera to 25 with
    a screen share. Main-thread cost grows with the tiles on screen, not
    the tiles in the panel: GTK uploads only the textures it draws, so 25
    cameras cost little more than 9. The frame notices themselves are
    negligible. Declaring the frames premultiplied, which is exact for
    opaque video, measured no different on GTK 4.24. What the scenario
    cannot see is the receive side: the panel subscribed to every
    publication while it was on screen, so a camera scrolled out of view
    was still sent, depacketized, decoded and converted to RGBA — about
    600 kb/s each at the encoder's target, and 1–7% of a core to decode
    and convert, from a simple scene to noise. Now the panel subscribes
    to the tiles in view and within half a view height of it, and lets a
    tile go once it is a view and a half away, recomputing once scrolling
    comes to rest. A tile scrolled back from further shows its first
    frame about a quarter of a second after the request on the local rig,
    plus the panel's 150 ms debounce — up to a second more when the
    server has just asked that publisher for a keyframe for someone else.
    Fixed.
24. **The connection read the socket a frame part at a time.** The actor
    reads a frame as its 22-byte header and then its body, and HOPE's
    ciphers read up to each frame boundary, straight from the socket — a
    read call or more per frame. TLS escaped it, because rustls buffers
    underneath, which is how a plain connection came to deliver fewer than
    half the frames a second a TLS one did while doing less work.
    **Fixed:**
    every lifecycle wraps the stream in a 64 KiB read buffer once, before
    the magic, and keeps it into the actor; ciphers sit above it and
    decrypt as they take bytes, so HOPE-Blowfish's rekeying still happens
    at the frame boundary. Plain went from 609,000 frames a second to
    2.4 million, HOPE-AEAD from 314,000 to 583,000; latency is unchanged.
25. **HOPE-Blowfish costs about 6 µs a frame, and its p99 latency is
    100 µs.** Both are its rekeying: on about three frames in sixteen the
    sender marks a key rotation, and each side then runs up to 63 HMAC
    iterations over the key — the key changes every iteration, so nothing
    carries over — and a fresh Blowfish key schedule. The latency counts
    both sides' share, the fake server's included. It is what the
    protocol asks for; at chat rates it is nothing. A lead only if a
    server sends frames fast enough to notice.
26. **A TLS upload lost its end.** The upload workers closed the transfer
    connection straight after the last write. With anything unread from
    the server at that moment — a TLS 1.3 server's session tickets are
    enough — the close reset the connection instead of ending it, and the
    reset made the server's kernel drop what the server hadn't read yet:
    1.8–2.2 MB of a 256 MiB upload, every time. **Fixed:** the workers
    finish an upload first — `close_notify`, a half-close, and a wait for
    the server to close — before closing (`hxnet_htxf_finish_send`).
27. **A fast transfer kept the main loop busy with its progress.** The
    workers post an update to the main loop for every chunk they copy,
    and there was a post for each: 41,000 a second for a plain download
    on loopback, 80,000 over TLS, which reads in 16 KiB records — 18% and
    33% of the main thread before the Tasks panel's handler did anything
    with them. **Fixed:** the progress callback posts at most every
    50 ms per transfer (`progress_due`); the byte count is exact
    regardless, and every worker posts once more when it finishes.
    Downloads got faster with it, TLS's by a fifth.
28. **A large tracker listing trickled in, 64 servers a tick.** The fetch
    hands a tracker's records over in a burst once the listing is read,
    through a channel with room for 64, and the main loop drains it on a
    50 ms timeout until it finds it empty. When a drain outran the
    fetch's refilling, the rest waited for the next tick: a 10,000-server
    listing could take seconds instead of one tick. **Fixed:** the
    channel has room for a whole listing, so it's all there for the
    first drain. What remains is the wait for that tick, up to 50 ms;
    draining on the fetch's own wakeup would remove it.
29. **A TLS folder download needs the server's `close_notify`.** A
    folder's end is the server closing, with no marker before it, and
    rustls takes a close without `close_notify` for a truncation: the
    folder arrives whole and the transfer is reported failed. The
    benchmark's fake server closed that way at first. Checked against the
    rig: Janus, its one TLS transfer server, sends `close_notify`, so it
    doesn't bite there. What the check turned up instead was every Janus
    folder download taking ten seconds longer than its tree — Janus
    closes only on its own timeout — which the item-count change in #716
    fixes. A lead only for a TLS server that closes bare.
