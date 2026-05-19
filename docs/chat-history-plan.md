# Chat History Extension — Implementation Plan

Reviving GtkHx, planning notes drafted 2026-05-18.
Targets the fogWraith Capabilities-Chat-History.md spec
(https://github.com/fogWraith/Hotline/blob/main/Docs/Protocol/Capabilities-Chat-History.md).

Server-side persistence of public chat messages with cursor-based
pagination. Optional, capability-negotiated (bit 4 in
DATA_CAPABILITIES). Channel 0 only in v1; named channels reserved for
a later spec.

---

## Locked UX decisions

These were chosen up front and the plan below assumes them.

| Decision           | Choice                                                  |
| ------------------ | ------------------------------------------------------- |
| Initial load       | Configurable, default 50 (pref `CHAT_HISTORY_INITIAL`)  |
| Scrollback trigger | Explicit "Load older" clickable row at the buffer top   |
| Cross-session      | Fresh each session — no last-seen persistence           |
| Visual style       | Slightly muted history text + boundary dividers         |

Rationale: keep the network footprint predictable (no surprise fetches
on scroll), avoid the per-server state file, but still give power users
a knob (initial count) and a clear visual signal that what they're
seeing came from the past.

---

## What's in scope for v1

- Capability negotiation: advertise bit 4 in `DATA_CAPABILITIES`,
  parse echoed bit + retention hints from login reply.
- `TRAN_GET_CHAT_HISTORY` (700) request sender.
- `DATA_HISTORY_ENTRY` (0x0F05) packed-binary parser with mini-TLV
  forward-compat (skip unknown sub-types).
- Access bit 56 (`accessReadChatHistory`) with fallback to bit 9
  (`accessReadChat`) per spec.
- Public chat only (`DATA_CHANNEL_ID = 0`).
- UI:
  - Initial fetch on chat-join (count from pref, default 50).
  - "Load N older messages…" row pinned at the top of the chat buffer.
  - "Beginning of chat history" marker when `has_more = 0`.
  - History messages render in a muted foreground colour (theme-aware).
  - "[message removed]" placeholder for tombstoned entries (flag bit 2).
  - Boundary divider between history and live chat after first fetch.
- Settings: one new pref (`CHAT_HISTORY_INITIAL`, uint16, default 50,
  0 = off).
- Tier 1/2 tests for the parser, capability echo, and access fallback.
- Tier 3 integration test against an mhxd build that ships the extension
  (or hand-rolled fixture — see "Open questions").

## What's deliberately out of scope

- Cross-session persistence of last-seen message ID. Punted for now.
- Named channels (`DATA_CHANNEL_ID` ≥ 1) — spec reserves but doesn't
  define them yet.
- Admin transactions 701–704 (delete, edit, channel mgmt).
- Auto-fetch on scroll-to-top. Explicit row only.
- Local on-disk cache of history. In-memory per chat for the session.
- Voice/file/etc. extensions to history.

---

## Architecture

### Wire layer (`src/`)

- **`protocol.h`**: add field IDs in the `0x0F01`–`0x0F08` range.
  - `HTLC_DATA_CHANNEL_ID 0x0F01`
  - `HTLC_DATA_HISTORY_BEFORE 0x0F02`
  - `HTLC_DATA_HISTORY_AFTER 0x0F03`
  - `HTLC_DATA_HISTORY_LIMIT 0x0F04`
  - `HTLC_DATA_HISTORY_ENTRY 0x0F05`
  - `HTLC_DATA_HISTORY_HAS_MORE 0x0F06`
  - `HTLC_DATA_HISTORY_MAX_MSGS 0x0F07`
  - `HTLC_DATA_HISTORY_MAX_DAYS 0x0F08`
  - `HTLC_TRAN_GET_CHAT_HISTORY 700`
  - `CAP_CHAT_HISTORY 0x0010` (bit 4)
- **`hl_access.h`**: add bit 56 (`HL_ACCESS_READ_CHAT_HISTORY`).
- **`commands.c`**: implement `hx_get_chat_history (htlc, cid, before,
  after, limit)`. Sends a 700 with whatever cursor fields are non-zero.
  `before == 0 && after == 0` ⇒ omit both (latest-N query).
- **`rcv.c`**: handler for 700 replies. Walk `DATA_HISTORY_ENTRY`
  fields, decode each into a new `HxHistoryEntry` struct, batch into a
  `GArray`. Read `DATA_HISTORY_HAS_MORE`. Emit a new signal
  `chat-history-batch` on `GtkhxSession`.
- **`network.c`** (login):
  - Advertise `CAP_CHAT_HISTORY` in `DATA_CAPABILITIES`.
  - Parse echoed `DATA_CAPABILITIES` to detect server support.
  - Parse `DATA_HISTORY_MAX_MSGS` / `DATA_HISTORY_MAX_DAYS` and stash
    on `htlc->history_max_msgs` / `htlc->history_max_days` (new fields
    on `htlc_conn`).

### History entry decoder (new module)

`src/history.c` / `src/history.h`:

```c
typedef struct {
    guint64 message_id;
    gint64  timestamp;   /* unix epoch UTC */
    guint16 flags;
    guint16 icon_id;
    gchar  *nick;        /* UTF-8 (already transcoded by server) */
    gchar  *message;     /* UTF-8 */
    /* sub-fields ignored in v1 — just skip unknowns */
} HxHistoryEntry;

void hx_history_entry_free (HxHistoryEntry *e);

/* Decode one packed entry from a buffer. Returns NULL on
 * truncation / bad length. Defensive — these come from the wire. */
HxHistoryEntry *hx_history_entry_parse (const guint8 *data, gsize len);
```

This file is the easiest to land first — pure decoder, easy to
fixture-test (Tier 2).

### Model / view boundary

New `GtkhxSession` signal:

| Signal               | Payload                                              |
| -------------------- | ---------------------------------------------------- |
| `chat-history-batch` | htlc, cid, entries (GArray*), has_more (bool)        |

Emitted from `rcv.c` after parsing the 700 reply. The view handler
(`on_chat_history_batch` in `gtkhx.c`) routes to a new
`chat_render_history_batch (sess, cid, entries, has_more)` in `chat.c`.

### Per-chat state (`struct gtkhx_chat`)

Add:

- `guint64 history_oldest`  — message ID of the oldest entry currently
  in the buffer. Used as `BEFORE` cursor for the next "Load older" fetch.
- `gboolean history_has_more` — set from `DATA_HISTORY_HAS_MORE`.
  Drives whether the "Load older" row is shown vs the "Beginning of
  history" marker.
- `gboolean history_pending` — in-flight fetch; suppresses double-tap.
- `textentry *history_marker_ent` — pointer to the "boundary" entry so
  we know where to insert older messages above.

### UI rendering

Public chat already uses `gtk_xtext_append_indent` (two-column layout
with separator, theme-aware palette). Reuse it.

- **Initial fetch**: triggered on `create_chat` / `chat_join`. Skip if
  pref is 0 or server didn't echo the cap or access bit is denied.
- **Loading state**: while `history_pending`, the "Load older" row
  shows "Loading…" with a spinner-ish glyph. Replace text in place;
  no extra widget needed.
- **Muting**: introduce two new palette slots — `XTEXT_HISTORY_FG`
  and `XTEXT_HISTORY_FG_DIM` — picked by `gtkhx_apply_theme_palette`.
  When rendering a history entry, push a mIRC colour escape that
  selects the muted index. `chat.c::output_chat_from_event` doesn't
  need to know; the history-batch path uses its own append wrapper
  that injects the colour code.
- **Boundary divider**: a single info-line entry (`gtk_xtext_append`
  with `\003` colour + ASCII rule, e.g. `─── live messages ───`).
- **"Load older" row**: a clickable info-line at the top. Click is
  caught by `word_click` (we already use this for URLs); use a
  sentinel string like `__hx_history_load_older__` and route it in
  `gtkhx_chat_word_click` to a fetch call.
- **Tombstones**: render as `[message removed]` in the muted colour,
  no nick column.
- **Action messages** (`is_action`): render as `* nick does …`
  (matching mIRC `/me`).
- **Server messages** (`is_server_msg`): render as info-line with the
  server label and no two-column split.

### Catch-up after reconnect

Even without cross-session persistence, within a single session a
reconnect after a drop should fetch with `AFTER=last_seen_in_buffer`
to fill the gap. Track the most recent live message ID per chat
(`gchat->history_newest`); on `connection-state == CONNECTED` after a
prior disconnect, fire `hx_get_chat_history (cid=0, after=newest)`.
This needs no new pref — just plumbing.

### Settings

Add one row to the existing chat preferences page:

> Initial chat history to load   [ 50 ▾ ]

`AdwSpinRow` or `AdwComboRow` with steps 0 / 10 / 25 / 50 / 100 / 200.
Stored as `CHAT_HISTORY_INITIAL` uint16.

---

## Phases (one feature branch each, in order)

### Phase 1 — Wire layer + parser

- `protocol.h` / `hl_access.h` constants.
- `history.{c,h}` parser with Tier 2 fixture tests.
- Round-trip test: encode a known-good entry, parse it, assert.
- Cap-negotiation parse on login reply (`htlc->history_max_msgs/days`).
- `commands.c` sender (no UI consumer yet).

Branch: `claude/chat-history-wire`. Commit gates: clean build, Tier 1
+ Tier 2 tests pass. No behavioural change (we advertise the cap but
don't fetch yet — server may legacy-broadcast, that's already handled
because TRAN_CHAT_MSG is unchanged).

### Phase 2 — Initial fetch + render

- `chat-history-batch` signal on `GtkhxSession`.
- View handler appends entries to the chat buffer with muted colour.
- Initial fetch on chat-join, gated on pref + cap + access bit.
- Boundary divider entry after the last history entry.
- Tombstone + action + server-message rendering.
- Pref `CHAT_HISTORY_INITIAL` + Settings row.

Branch: `claude/chat-history-render`. Acceptance: against an mhxd
build that supports the extension, joining a chat shows the last N
messages above a "── live messages ──" divider.

### Phase 3 — "Load older" row + scrollback

- Clickable "Load N older messages…" row at the top of the buffer.
- Track `history_oldest` / `history_has_more` / `history_pending`.
- Fetch with `BEFORE=oldest`, append above existing entries.
- Replace the row with "Beginning of chat history" when has_more=0.
- Click suppression while a fetch is in flight.

Branch: `claude/chat-history-scrollback`. Acceptance: clicking the
row loads the next batch, history scrolls up. End-of-history marker
appears when the server returns has_more=0.

### Phase 4 — Reconnect catch-up + polish

- Track `history_newest` per chat from live messages.
- On reconnect-after-drop, fetch `AFTER=newest` and merge.
- Theme-aware muted palette slots (light/dark).
- Retention hint shown somewhere unobtrusive (status bar tooltip or
  Settings page sub-label: "This server keeps up to {N} messages /
  {D} days").

Branch: `claude/chat-history-catchup`.

### Phase 5 — Tests + docs

- Tier 3 integration test against mhxd-with-history (need to check
  whether mhxd has merged this extension; if not, may need to use a
  Mobius or fogWraith server image).
- README / ChangeLog note.
- Memory-system note about lessons learned.

Branch: `claude/chat-history-tests`.

---

## Open questions to resolve at start of next session

1. ~~**Which test server ships the extension?**~~ Answered by the
   multi-server spike (`docs/multi-server-test-spike.md`): **none
   do yet.** Mobius issue #105 (the simpler "replay last N lines"
   feature) is still open. mhxd doesn't have it. Greg Gant runs
   Mobius, so most likely deployment order is Hotline Navigator
   client → Mobius server → others. Phase 1 lands on Tier 2 wire
   fixtures; Phase 2+ uses a small Go mock server we ship in
   `tests/integration/mock-server/`. Real-server Tier 3 light up
   when (a) Mobius gets the extension, or (b) Greg publishes one.
2. **Retention hint placement.** Status-bar tooltip vs Settings
   sub-label vs nothing. Probably "show it in Settings only, no chrome
   space spent on it."
3. **Live-chat channel ID after named channels arrive.** Spec notes
   that `TRAN_CHAT_MSG` (106) doesn't carry `DATA_CHANNEL_ID` today
   and a future spec will add it. Not our problem until then.
4. **Per-server vs global pref.** Initial-load count is global today.
   If users want different defaults per server, bookmarks could carry
   an override. Defer until someone asks.
5. **Editing history config from a UI test.** Confirm whether mhxd's
   Dockerfile in `tests/integration/` exposes a config knob for
   enabling history — if it merges the extension at all.

---

## File-by-file change estimate

| File                       | Phase | Change                                          |
| -------------------------- | ----- | ----------------------------------------------- |
| `src/protocol.h`           | 1     | +9 #defines                                     |
| `src/hl_access.h`          | 1     | +1 bit                                          |
| `src/history.c/.h` (new)   | 1     | ~200 LOC parser                                 |
| `src/commands.c`           | 1     | +50 LOC sender                                  |
| `src/rcv.c`                | 1     | +80 LOC handler + signal emit                   |
| `src/network.c`            | 1     | +30 LOC for cap echo + retention parse          |
| `src/gtkhx_session.c/.h`   | 2     | +1 signal definition                            |
| `src/gtkhx.c`              | 2     | +1 signal handler                               |
| `src/chat.c/.h`            | 2,3   | ~300 LOC for render path + state + click route  |
| `src/options.c`            | 2     | +1 pref row                                     |
| `src/prefs.h`              | 2     | +1 field                                        |
| `src/cfgvars.c` (or eqv)   | 2     | +1 row in the table                             |
| `tests/unit/test_history*` | 1     | new                                             |
| `tests/integration/`       | 5     | new                                             |

Total guess: ~700 LOC plus tests. Roughly two evenings' worth of work
once Phase 1 is in.
