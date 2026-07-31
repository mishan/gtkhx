# Multi-connection

This is a design survey for `MAX_CONN > 1` — the long-deferred ability to be
connected to several Hotline servers at once. It is a **plan, not a record**:
almost none of it is built. It exists to make the trade-offs legible before
anyone commits, and to record the decisions that are deliberately still open.

Companion reading: `docs/docking.md` (the dock the
UI would extend), and the Rust roadmap, which parks this work at the end of the
C→Rust window port.

---

## The session model as it stands

The old `sessions[MAX_CONN]` / `sess_from_htlc()` scaffolding — an array that
always returned element zero — is **gone**. It was collapsed to an explicit
single session during the modernization work, and this document is the only
place in the tree that describes the resulting shape in full.

There is one global:

```c
/* session.h */
typedef struct _session { ... } session;   /* per-session collections + the conn */
extern session the_session;                /* the single-session world */
```

The connection is **not** embedded in it. `session` carries
`struct htlc_conn *htlc` — heap-allocated once at startup, owned by the session
for its lifetime, and never NULL after init. The pointer is deliberate: the
connection struct's storage can move behind an opaque owner without every
`sess->htlc->` call site changing again.

Two accessors are the seam that multi-connection routing will grow into:

- **`sess_from_htlc(htlc)`** — the session that *owns* this connection. Model
  code already holds the connection for an event it is handling and must route
  by it, because an event belongs to a specific connection rather than to
  whichever one the user is looking at. This is a real back-pointer: `htlc_conn`
  carries its owning session, set at allocation, and the accessor reads it
  through the connection's opaque interface. It is not a `container_of` — that
  trick required the connection to be embedded in the session, which it no
  longer is — nor a struct field read, since the struct is opaque now.
- **`hx_active_session()`** — the currently *focused* session. UI code (a button
  click, a menu action, a dialog) acts on whichever connection the user is
  looking at. Today it returns the one session; when a connection tab strip
  lands it becomes "the focused tab's session" and the call sites follow without
  further edits.

### Blast radius

Smaller than it used to be, and smaller than a raw grep once suggested. Routing
every consumer through the two accessors above already happened, so the direct
references to `the_session` that remain are concentrated in three places: the
**construction and teardown** sites, where the one session is born and dies —
precisely the future connection factory, so the concrete global there is correct
rather than debt; the **identity binding** in the settings layer (below); and a
**bridge shim** the ported-to-Rust tracker window uses to reach session state,
which follows the same convention and is called out inline.

Everything else asks for a session rather than naming one. Turning the global
into a routed collection is still the single largest mechanical change in the
effort, but it is now a handful of well-marked sites plus the addition of a
collection, rather than a treewide sweep.

**The identity carve-out.** The preferences table binds config keys to storage
addresses, and two of them — the user's icon and nick — live on the
heap-allocated connection, so their addresses aren't compile-time constants. The
static table leaves those slots empty and a binder wires them once the
connection exists, before any preference read or write touches them. Multi-conn
reworks this regardless: per-connection identity is one of the open questions
below, and whatever answer it gets, the binding stops being a one-shot.

### What already helps

Three things mean this is less scary than it looks:

1. **Per-session collections are already fields, not globals.** Chats, private
   message windows and tasks hang off `session`. They travel cleanly when
   `session` goes plural — there is no per-file global to chase.

2. **The dock was designed for this.** From `docs/docking.md`:

   > When `MAX_CONN > 1` lands, each connection gets its own Chat panel with
   > its own tab view. The singleton [`chat_tabs` `AdwTabView`] turns into a
   > per-Chat-panel field; the API stays the same.

   The recursive split tree, the panel registry, and layout persistence
   (`dock-layout.ini`) are all in place. The UI substrate exists.

3. **`GtkhxSession` is already Rust.** The model→view signal hub is a
   `glib::subclass` object living in the `gtkhx-core` crate (alongside the boxed
   signal payloads and the connection struct itself), exporting the same C ABI.
   Per-connection reification is *more* feasible than the Rust roadmap assumed
   when it wrote this work up — the object we'd multiply is no longer a C
   translation unit.

---

## The four design axes

### Axis 1 — Sequencing

The Rust roadmap places multi-conn dead last, after the C→Rust window port, on
the reasoning that a connection is a struct in Rust rather than a global, so the
abstraction can finally be built properly.

The argument that used to weigh against pulling it forward — that building the
abstraction in C would collide head-on with the window ports, which were busy
*deleting* those same files — has largely dissolved. The routing seam landed,
the window ports have landed one after another, and the two efforts no longer
want to edit the same lines. Sequencing is now a question of appetite rather
than of collision risk.

Three postures remain:

- **Do it now, against today's code.** Usable soonest.
- **Wait for the port to finish.** Cheapest per unit of work — refactor against
  mostly-Rust widgets — but gated behind the longest remaining phase.
- **Middle path — model plumbing now, tab UI later.** Do the connection-routing
  groundwork (dissolve the global, per-connection session objects) while the
  Rust session object makes it convenient; defer the multi-tab *UI* to ride
  along with the remaining window ports. Splits the risk: the wide-but-shallow
  model change lands early and independently, and the UI-shaped work lands when
  the UI is being rewritten anyway.

This axis is **open**.

### Axis 2 — Signal routing

`GtkhxSession` is a process-wide singleton. Every model-side emitter calls
`gtkhx_session_emit_*`, and none of the signals carry a notion of *which*
connection they came from. With N connections, an incoming `chat` has no home.

Two options:

1. **One `GtkhxSession` per connection.** The view side subscribes
   per-connection and routes to that connection's UI subtree. Clean; matches the
   long-term direction; the natural fit now that the session object is Rust.
   Each connection's emitters target its own instance, so nothing downstream
   needs to demultiplex.
2. **Keep the singleton, add a connection id to every payload.** Less churn to
   *start*, but every signal and every view-side handler grows a demux step, and
   it's a dead end that gets unwound later.

The recommendation leans to (1), but it's coupled to Axis 1 — doing it early is
the core of the middle path.

### Axis 3 — Panel scope

Three tiers fall out naturally.

**Per-connection** — server *content*, meaningless without a connection:

- Chat, and its private-chat / private-message tabs
- Users list
- News, both the 1.0 and the threaded 1.5 trees. Each server's news is its own;
  this settles the "a News window per server?" question as **yes**.
- Files browser

**Global** — things that exist before and across connections:

- Tracker. It's how you *pick* a server to connect to, and it's already a
  standalone window outside the dock. Stays global.
- Settings and bookmarks.

**Global but tagged:**

- Tasks and the transfer queue. One global list of every connection's in-flight
  transfers, with a per-row tag naming the connection. This matches the "one
  place to watch every transfer" instinct.

  The alternative — per-connection transfer queues — is **rejected**. It reads
  as more consistent with the per-connection tier, but it makes the common
  question ("is anything still going?") into a hunt across tabs, and a stalled
  upload on a connection you aren't currently looking at becomes invisible. The
  queue's job is to be the one place you check.

### Axis 4 — Voice exclusivity

The constraint is **one voice chat at a time, anywhere**: joining voice on
server B, or a different room on server A, must first leave the current one.

Today the voice runtime is per-session, tracking a currently-active chat id —
one runtime drives all rooms on that connection — and the voice model is
likewise per-session, created at startup and cleared on disconnect.

The multi-conn design keeps **per-session voice models** (each server's user
list still needs to render *its* speaker indicators) but gates **runtime
activation** through a process-global arbiter:

- A single global "who owns voice" token: a (session, room) pair, or none.
- Acquiring the token tears down whatever currently holds it — send the voice
  leave on the old room, stop that session's runtime — *before* joining the new
  one.
- The pipeline, mic capture, mute and push-to-talk state are inherently
  singular, because there is one microphone. The arbiter isn't an arbitrary
  restriction; it's modelling the hardware.
- UX: preempt-on-join with a confirmation — "leave voice on *ServerA* to join
  here?" — plus a clear indication in the tab strip of which connection holds
  voice.

This is the least risky of the four axes, precisely because the
singular-by-nature hardware does most of the design work.

---

## The layout model — two directions, both open

Per-connection panels have to relate somehow to the *single* dock tree and its
panel registry, which is keyed today by a stable string id (`"chat"`,
`"users"`, …). Two models, deliberately not decided here.

### Model A — Tab-switched panel sets

A connection tab strip across the top of the main window. Switching tabs swaps
the *entire* set of per-connection panels filling the dock; global panels stay
put.

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

- **Pros:** the conventional IRC-client model. Layout persistence barely
  changes — one dock tree, one layout file, unchanged format.
- **Cons:** you can't see two servers' chats side by side. The dock *shape* is
  shared across all connections, so you can't have a wide Files panel for a file
  server and a wide Chat for a chat server at the same time.
- **Registry impact: minimal.** Panel ids stay stable; each maps to the active
  connection's instance. A tab switch reseats content, not tree shape.

### Model B — Coexisting per-connection panels

Each per-connection panel is *distinct per server*: "Users (ServerA)" and
"Users (ServerB)" are separate, independently dockable, tileable panels in one
unified tree.

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

- **Pros:** maximum flexibility; fits GtkHx's undock-everything ethos. Compare
  two servers at a glance. Natural for someone monitoring several communities.
- **Cons:** a real design project, and the cost lands squarely on the registry
  and the layout file. **The panel-registry key has to grow a dimension** —
  from a bare string id to a (connection, role) pair — and every panel-id
  constant becomes a family rather than a value. **The layout file's grammar
  has to grow with it**: the persisted split expression names panels by id, so
  ids need connection qualification, and restore has to re-associate panels with
  connections that may not have reconnected yet. Then there is sheer
  proliferation: four servers times four per-connection panels is sixteen panels
  to place, persist and undock.
- **Registry impact: substantial**, in the specific sense that a
  one-dimensional key becomes two-dimensional and the file format follows.

### A hybrid worth noting

Default to **A** for the common case, but allow a panel to be pinned or
duplicated out of the active set into the tree à la **B** when the user
explicitly wants to compare two servers. Gets the simple default and the
power-user affordance, at the cost of a registry that supports both addressing
modes. Probably the eventual destination, but more than a v1.

---

## Cross-cutting concerns to design through

- **Connection identity and lifecycle.** A connection needs a stable handle that
  survives disconnect and reconnect, so panels, the transfer queue and the voice
  arbiter can refer to it. Today the identity *is* the global; it has to become
  a first-class object.
- **Bookmarks to tabs.** An "open in new tab" affordance. The toolbar's
  connect-with-bookmark split button needs a "new tab vs. replace current"
  decision.
- **Connection-loss banner.** Becomes per-connection, shown in that tab's
  subtree, rather than app-global.
- **Toolbar access-bit gating.** News / Post / Kick / Ban grey out by the
  *active* connection's version and access bits. Under Model A this
  re-evaluates on every tab switch.
- **Per-identity settings.** Nick, icon and the avatar are global today.
  Multi-conn raises "same identity everywhere" against "per-bookmark identity".
  It can stay global for v1, but it should be named rather than decided by
  accident — and note that the identity preference binding described above is
  built on there being exactly one connection.
- **The chat tab view singleton.** Becomes a per-Chat-panel field. Already
  anticipated by the dock design; straightforward under either layout model.
- **Layout persistence across connections.** Model A → one global layout. Model
  B → the file format grows a connection dimension.
- **Test matrix.** Multi-conn wants an end-to-end test that connects to two
  Dockerized servers at once and asserts isolation: chat in both, a transfer in
  each, voice preemption across them. The existing multi-server matrix already
  runs several containers, so the substrate is there.

---

## A plausible decomposition

Illustrative, not committed. If the middle path on Axis 1 is chosen:

| Phase | Scope | Depends on |
|-------|-------|-----------|
| M0 | Session-routing seam: `sess_from_htlc()` and `hx_active_session()` replace direct global access. Model code routes received events by connection; UI code routes by focused session. No behaviour change at N == 1. | — |
| M1 | Per-connection `GtkhxSession`; signal routing to a per-connection UI subtree. Still one active connection. | M0, M3 |
| M2 | Voice arbiter: global token, preempt on acquire; per-session voice models retained. | a session collection |
| M3 | Reify the connection/session as a heap object behind a collection and factory (the construction sites named above); connection tab strip; pick layout Model A or B; per-Chat-panel tab view. | M0 |
| M4 | Global transfer queue with per-connection tags; per-connection loss banner; bookmarks "open in new tab". | M3 |
| M5 | Layout persistence across connections; two-server isolation test. | M3 |

**M0 has landed** — it is what the "session model as it stands" section above
describes. It was safe to do ahead of every open decision precisely because it
changes no behaviour at N == 1: it only makes "which session" an accessor call
instead of a hardcoded global. M1 and M2 want at least a session collection to
exist first — there is nothing to route *to* while N == 1 — so they depend on
the M3 reification rather than standing alone. M3 through M5 are UI-shaped and
natural to ride with the remaining window ports.

---

## Open questions

1. **Sequencing.** Now, later, or the middle path?
2. **Layout model.** A, B, or the hybrid? Both are documented above without a
   pick.
3. **Identity model.** One identity across all connections, or per-bookmark
   identity? Leaning global for v1.
4. **Transfer queue.** Global-with-tags (favoured, above) versus
   per-connection.
5. **Reconnect and tab restore.** Do tabs persist across app launches, and how
   does that interact with the saved dock layout? Tab reordering and detach
   persistence needs its own design.
6. **Maximum connections.** Is there a sane upper bound, or is it "until the
   machine complains"? Affects whether the tab strip needs overflow handling.

## What this is *not*

- Not a commitment to any timing relative to the Rust port.
- Not a decision between layout models A and B.
- Not a change to the wire protocol. Multi-conn is entirely a client-side
  concern; 1.2 / 1.5 / 1.9 compatibility is untouched, and each connection
  speaks the protocol exactly as the single-session client does today.
