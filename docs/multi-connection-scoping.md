# Multi-connection scoping

**Status: scoping — the two pivotal decisions remain open. Phase M0
(the behavior-preserving session-routing seam) has shipped as an
enabling step; see "M0 implementation notes" below.** This is the
design survey for `MAX_CONN > 1` — the long-deferred ability to be
connected to several Hotline servers at once. It exists to make the
trade-offs legible before anyone commits, and to record the two pivotal
decisions that are deliberately still open: **when** this lands relative
to the Rust port, and **how** per-connection panels relate to the
docking layout. M0 was safe to land ahead of those decisions because it
changes no behavior (N == 1) — it only makes "which session" an
accessor call instead of a hardcoded global.

Companion reading: `docs/docking.md` and `docs/docking-splits.md` (the
dock the UI would extend), `docs/rust/ROADMAP.md` Phase R7 (where the
Rust plan parks this work), `ROADMAP.md` Phase 5 (the original
"multi-server connections" line item).

---

## Where the code actually is today

The old `sessions[MAX_CONN]` / `sess_from_htlc()` scaffolding that the
roadmaps describe as "half-built" is **gone** — it was collapsed to an
explicit single session during the modernization work. The current
shape is one global:

```c
/* session.h */
typedef struct { ... } session;   /* per-session collections + the conn */
extern session the_session;       /* the single-session world */
```

with the connection embedded directly (`struct htlc_conn htlc;` is a
field of `session`, not a pointer). The comment in `session.h` is
explicit:

> Single-session world. Phase 5 will revisit when multi-conn lands; the
> historical `sessions[MAX_CONN]` / `sess_from_htlc()` pretended to be
> an array but always returned `&sessions[0]`.

### Blast radius

`the_session` is referenced **~241 times across ~28 `.c` files**. The
distribution (top consumers):

| File                        | refs |
|-----------------------------|------|
| `rcv.c`                     | 34   |
| `gtkhx.c`                   | 29   |
| `chat.c`                    | 26   |
| `options.c`                 | 18   |
| `files_remote_provider.c`   | 15   |
| `msg.c`                     | 13   |
| `xfers.c`, `news_browser.c` | 12   |
| `toolbar.c`                 | 11   |
| `tracker.c`, `files_browser.c` | 9 |
| …tail across ~18 more files | 1–8  |

Nearly all are `&the_session`, `the_session.htlc`, or
`chat_with_cid (&the_session, …)`-shaped. Turning the global into a
routed collection is the single largest mechanical change in the whole
effort — wide, but shallow.

### What already helps

Three things mean this is less scary than the raw count suggests:

1. **Per-session collections are already fields, not globals.**
   `chats`, `gchats`, `msg_windows`, `tasks` are `GHashTable`s hanging
   off `session` (see the MVC-cleanup table in `CLAUDE.md`). They
   travel cleanly when `session` goes plural — no per-file global to
   chase.

2. **The dock was designed for this.** `docs/docking.md`:

   > When `MAX_CONN > 1` lands, each connection gets its own Chat panel
   > with its own tab view. The singleton [`chat_tabs` `AdwTabView`]
   > turns into a per-Chat-panel field; the API stays the same.

   The `HxSplit` recursive tree, the panel registry, and layout
   persistence (`dock-layout.ini`) are all in place. The UI substrate
   exists.

3. **`GtkhxSession` is already Rust** (Phase R4). The model→view signal
   hub is a `glib::subclass` object. Per-connection reification (below)
   is *more* feasible now than the Rust roadmap assumed when it wrote
   R7 — the object we'd multiply is no longer a C translation unit.

---

## The four design axes

### Axis 1 — Sequencing: now vs. R7 vs. a middle path

The Rust roadmap places multi-conn at **Phase R7, dead last**, after
the entire C→Rust window port (R0–R6). Its rationale (R3 exit note):

> The `MAX_CONN > 1` half-built abstraction can finally be abstracted
> properly because a connection is a struct in Rust, not a global.

Three postures:

- **Pull it forward (against today's C).** Usable soonest. Cost: build
  the abstraction in C across all 241 `the_session` sites, right as
  R5 is trying to *delete* those same files (`chat.c`, `users.c`,
  `files*.c`, …). Real risk of double-work and merge churn.
- **Keep it at R7.** Cheapest per unit of work — refactor against
  mostly-Rust widgets. Cost: gated behind R5, the longest phase
  (8–12 weeks estimated), so it's far out.
- **Middle path — model plumbing now, tab UI later.** Do the
  connection-routing groundwork (dissolve `the_session`, per-connection
  `GtkhxSession`) while it's fresh and while R4's Rust session object
  makes it convenient; defer the actual multi-tab *UI* to ride with
  R5/R7 window ports. Splits the risk: the wide-but-shallow model change
  lands early and independently; the UI-shaped work lands when the UI is
  being rewritten anyway.

This axis is **open** — see "Open questions".

### Axis 2 — Signal routing: the `GtkhxSession` singleton

`GtkhxSession` is a process-wide singleton (`gtkhx_session_get_default
()`). Every model-side emitter calls `gtkhx_session_emit_*`; the ~24
signals (`chat`, `msg`, `user-create`, `task-update`, …) carry no
notion of *which* connection they came from. With N connections an
incoming `chat` has no home.

Two options (both flagged in R7's gotchas):

1. **One `GtkhxSession` per connection.** The view side subscribes
   per-connection and routes to that connection's UI subtree. Clean;
   matches the long-term direction; the natural fit now that the
   session object is Rust. Each connection's emitters target its own
   instance, so no demux is needed downstream.
2. **Keep the singleton, add `connection_id` to every payload.** Less
   churn to *start*, but every signal and every `on_*_signal` handler
   in `gtkhx.c` grows a demux step, and it's a dead end that gets
   unwound later.

Recommendation leans (1), but it's coupled to Axis 1 — doing it early
is the core of the "middle path".

### Axis 3 — Panel scope: what's per-server vs. global

Three tiers fall out naturally:

**Per-connection** (server *content* — meaningless without a
connection):

- Chat (+ its pchat / PM tabs inside `chat_tabs`)
- Users list
- News 1.0 and News 1.5 (each server's news tree is its own — this is
  the "a News window for each server?" question: **yes**)
- Files browser

**Global** (exist before / across connections):

- Tracker — it's how you *pick* a server to connect to; already a
  standalone window outside the dock (`docs/docking.md` "Stays a real
  window"). Stays global.
- Settings / preferences, Bookmarks.

**Global-but-tagged:**

- Tasks / transfer queue. R7's plan: one global list of all
  connections' in-flight transfers with a per-row "which connection"
  tag. Matches the "one place to watch every transfer" instinct.
  (Alternative: per-connection queues. Weaker — you'd hunt across tabs
  for a stalled upload.)

### Axis 4 — Voice exclusivity

Your constraint: **one voice chat at a time, anywhere** — joining voice
on server B, or a different room on server A, must first leave the
current one.

Today voice runtime is `sess->voice_runtime` (one runtime per session,
tracking a currently-active cid; see `voice_runtime.h` "Per-session,
not per-room — one runtime drives all"). `HxVoiceModel` is also
per-session, created in `fe_init()`, cleared on disconnect.

The multi-conn design keeps **per-session voice *models*** (each
server's user list still needs to show *its* voice indicators via the
`indicator-changed` column) but gates **runtime activation** through a
new **process-global voice arbiter**:

- A single global "who owns voice" token: `(session*, cid)` or none.
- `voice_acquire(session, cid)` tears down whatever currently holds the
  token — send `HTLC_HDR_VOICE_LEAVE` on the old room, stop that
  session's runtime — *before* joining the new room.
- The GStreamer pipeline, mic capture, mute, and PTT state are
  inherently singular (one mic) — this matches the hardware, so the
  arbiter isn't an arbitrary restriction, it's modeling reality.
- UX: preempt-on-join with a confirm — "Leave voice on *ServerA* to
  join here?" — and clear indication in the tab strip of which
  connection currently holds voice.

This axis is the *least* risky of the four; the singular-by-nature
hardware does most of the work.

---

## The layout model — two directions, both open

Per-connection panels (Chat, Users, News, Files) have to relate somehow
to the *single* `HxSplit` dock tree and the panel registry (keyed today
by stable string id — `"chat"`, `"users"`, …). Two models, deliberately
**not** decided here.

### Model A — Tab-switched panel sets

A connection tab strip (`AdwTabView`) across the top of the main
window. Switching tabs swaps the *entire* set of per-connection panels
filling the dock; global panels (Tracker, Tasks) stay put.

```
┌─[ ServerA ][ ServerB ]+─────────────────────────┐  ← connection tabs
│ ┌── News ──┐ ┌──── Chat ────────┐ ┌── Users ──┐ │
│ │ (A's)    │ │ (A's, w/ pchat   │ │ (A's)     │ │
│ │          │ │  + PM subtabs)   │ │           │ │
│ └──────────┘ └──────────────────┘ └───────────┘ │
│              ┌──── Tasks (global, all conns) ──┐ │
│              └─────────────────────────────────┘ │
└──────────────────────────────────────────────────┘
switch to ServerB → the News/Chat/Users/Files panels swap to B's
```

- **Pros:** conventional IRC-client model (HexChat, Konversation).
  Layout persistence barely changes — one dock tree, `dock-layout.ini`
  stays as-is. Panel registry ids stay stable; the *content* behind
  each id swaps per active connection. Much less work.
- **Cons:** can't see two servers' chats side by side. The dock shape
  is shared across all connections (you can't have a wide Files panel
  for a file server and a wide Chat for a chat server simultaneously).
- **Registry impact:** minimal. Each per-connection panel id maps to
  the active connection's instance; a tab switch reseats content, not
  tree shape.

### Model B — Coexisting per-connection panels

Each per-connection panel is *distinct per server*: "Users (ServerA)"
and "Users (ServerB)" are separate, independently dockable, tileable
panels in one unified tree.

```
┌──────────────────────────────────────────────────┐
│ ┌ Chat (ServerA) ┐ ┌ Chat (ServerB) ┐ ┌ Users(A) ┐│
│ │                │ │                │ ├──────────┤│
│ │                │ │                │ │ Users(B) ││
│ └────────────────┘ └────────────────┘ └──────────┘│
│ ┌──────────── Tasks (global) ──────────────────┐  │
│ └───────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────┘
```

- **Pros:** maximum flexibility; fits GtkHx's undock-everything ethos.
  Compare two servers at a glance. Natural for someone monitoring
  several communities at once.
- **Cons:** a real design project. The panel registry key must become a
  `(connection, role)` pair instead of a bare string id. Layout
  persistence (`dock-layout.ini`, the `L[id1,id2,…:role]` s-expression
  in `docs/docking-splits.md`) gains a per-connection dimension it
  doesn't currently model — panel ids need connection qualification,
  and restore has to re-associate panels with (possibly not-yet-
  reconnected) connections. Panel proliferation: 4 servers × 4
  per-connection panels = 16 panels to place / persist / undock.
- **Registry impact:** substantial. `panel_registry` becomes
  two-dimensional; every `HX_PANEL_ID_*` constant becomes a family.

### A hybrid worth noting

Default to **A** (tab-switched sets) for the common case, but allow a
panel to be **pinned / duplicated out** of the active set into the tree
à la **B** when the user explicitly wants to compare two servers. Gets
the simple default and the power-user affordance, at the cost of a
registry that supports both addressing modes. Probably the eventual
destination, but more than a v1.

---

## Cross-cutting concerns to design through

- **Connection identity & lifecycle.** A connection needs a stable
  handle (id) that survives disconnect/reconnect so panels, the
  transfer queue, and the voice arbiter can refer to it. Today the
  identity *is* the global; it needs to become a first-class object.
- **Bookmarks → tabs.** "Open in new tab" affordance (R7 work item 4).
  The toolbar's `AdwSplitButton` connect-with-bookmark path
  (`bookmark_cipher` / `bookmark_rc4_dialog`) needs a "new tab vs.
  replace current" decision.
- **Connection-loss banner** (`AdwBanner` with Reconnect) becomes
  per-connection, shown in that tab's subtree, not app-global.
- **Toolbar access-bit gating.** News / Post / Kick / Ban buttons grey
  out by the *active* connection's version + `hl_access` bits. With
  per-connection panel sets (Model A) this re-evaluates on tab switch.
- **Settings that are per-identity.** Nick, icon, and the fogWraith
  GIF avatar (`$CONFIG/avatar.gif`) are currently global. Multi-conn
  raises "same identity everywhere" vs. "per-bookmark identity" — an
  identity-model question that can stay global for v1 but should be
  named.
- **`chat_tabs` singleton.** Becomes a per-Chat-panel field (already
  anticipated). Straightforward under either layout model.
- **Layout persistence across connections.** `docs/docking.md` Phase 4b
  note: "Per-connection layouts vs. one global layout will become
  relevant once multi-conn lands." Model A → one global layout. Model B
  → the file format grows a connection dimension.
- **Tier 3 test matrix.** Multi-conn wants a test that connects to two
  Dockerized servers at once (e.g. mhxd + Janus) and asserts isolation:
  chat in both, transfer in each, voice preemption across them. The
  existing multi-server matrix already runs several containers.

---

## Rough phasing (illustrative, not committed)

If the "middle path" on Axis 1 is chosen, a plausible decomposition:

| Phase | Scope | Depends on | Status |
|-------|-------|-----------|--------|
| M0 | Session-routing seam: `sess_from_htlc(htlc)` + `hx_active_session()` accessors replace direct `&the_session` access everywhere. Model code routes received events by connection; UI code routes by focused session. No behavior change (N == 1). | — | ✅ shipped |
| M1 | Per-connection `GtkhxSession`; signal routing to per-connection UI subtree. Still single active connection. | M0, M3-lite | ⬜ |
| M2 | Voice arbiter: global token + preempt-on-acquire; per-session voice models retained. | session collection | ⬜ |
| M3 | Reify the connection/session as a heap object behind a collection + factory (the `the_session` construction sites); connection tab strip (`AdwTabView`); pick layout Model A or B; per-Chat-panel `chat_tabs`. | M0 | ⬜ |
| M4 | Global transfer queue with per-connection tags; per-connection loss banner; bookmarks "open in new tab". | M3 | ⬜ |
| M5 | Layout persistence across connections; Tier 3 two-server isolation test. | M3 | ⬜ |

M0 is the model plumbing that's valuable and safe today (behavior-
preserving, landed pre-R7). M1/M2 want at least a session collection to
exist first (there's nothing to route *to* while N == 1), so they now
depend on the M3 reification rather than standing alone. M3–M5 are
UI-shaped (natural to ride with the R5 window ports). This is a sketch
to make the shape discussable, not a plan of record.

### M0 implementation notes (shipped)

Landed on `claude/multi-conn-m0-session-seam` in three commits:

1. **The seam.** `session.h` gains `sess_from_htlc(htlc)` — an exact
   `container_of` over the `htlc_conn` embedded in `session` (the only
   instance in the tree, so it's correct now and once sessions are
   heap-allocated) — and `hx_active_session()` (returns `&the_session`
   for N == 1; defined in `gtkhx.c`).
2. **Model side routes by connection.** `rcv.c`, `network.c`,
   `commands.c`, `xfers.c`, `usermod.c`, `gif_icons.c`, `tasks.c`,
   `inline_media_*`, `notify.c`: an inbound event resolves its session
   from the connection it already holds (`sess_from_htlc(htlc)`, or the
   transfer's `htxf->htlc`). Where the connection had been reached as
   `the_session.htlc`, it collapses to the actual `htlc` param.
3. **UI side routes by focused session.** All window/dialog code uses
   `hx_active_session()`. Display handlers that receive `htlc` from the
   signal (chat/user/news output) route by it instead — more correct.

Deliberately **not** converted (the concrete `the_session` stays):
the construction/teardown sites in `gtkhx.c` (`fe_init`, `hx_quit`,
`main`, `hotline_client_init`) — where the one session is born and dies,
i.e. the future M3 factory — and two `options.c` static `cfgvar` table
entries that need a compile-time-constant address (`&the_session.htlc.
{icon,name}`). Both are documented inline.

Verified: full build clean under `-Wall -Wextra` (no warnings); 68
Tier 1/2 unit + proto tests pass. Rust crates untouched.

---

## Open questions (unresolved by design)

1. **Sequencing (Axis 1).** Pull forward against C now, wait for R7, or
   the middle path (model now / UI later)? Deferred.
2. **Layout model (the two directions above).** A (tab-switched sets),
   B (coexisting per-conn panels), or the hybrid? Deferred — both are
   documented above without a pick.
3. **Identity model.** One identity across all connections, or
   per-bookmark identity (nick / icon / avatar)? Leaning global for v1;
   named here so it isn't decided by accident.
4. **Transfer queue.** Global-with-tags (R7's plan, favored) vs.
   per-connection.
5. **Reconnect & tab restore.** Do tabs persist across app launches
   (auto-reconnect to last session set), and how does that interact
   with `dock-layout.ini`? AdwTabView reordering/detach persistence
   needs its own design (R7 gotcha).
6. **Max connections / resource ceiling.** Is there a sane upper bound,
   or is it "until the machine complains"? Affects whether the tab strip
   needs overflow handling.

---

## What this is *not*

- Not a commitment to any timing relative to the Rust port.
- Not a decision between layout models A and B.
- Not a change to the wire protocol — multi-conn is entirely a
  client-side concern; Hotline 1.2 / 1.5 / 1.9 compatibility is
  untouched, and each connection speaks the protocol exactly as the
  single-session client does today.
