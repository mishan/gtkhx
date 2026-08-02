# Multi-connection

This is a design survey for `MAX_CONN > 1` — the long-deferred ability to be
connected to several Hotline servers at once. It is a **plan, not a record**:
almost none of it is built. It exists to make the trade-offs legible before
anyone commits, and to record the decisions that are deliberately still open.

Companion reading: [docking.md](docking.md) (the dock the UI would extend),
[preferences.md](preferences.md) (which owns the connection collection and the
identity model M1 depends on), and the Rust roadmap, which parks this work at the
end of the C→Rust window port.

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

---

## What is already ready

Worth stating first, because it is more than you would guess and it changes the
shape of the estimate.

**The whole network stack is N-ready.** `hxnet` has no global orchestrator: the
connection actor is per-instance with its own command channel and stream, the
frame-dispatch callback takes the connection handle as its first argument and
the callbacks are stored per-connection rather than in a global function
pointer, transfer abort tokens are minted per transfer, and HOPE key material
and TLS configs are per-connection. Its module-level statics — a banner-fetch
semaphore, a tracker TLS-verdict cache, a debug gate — are all genuinely
connection-independent.

**The connection struct is already allocatable N times.** `hx_conn_new()` is a
plain boxed allocation, there is no static holding "the" connection, and the
back-pointer to the owning session is real per-instance storage.

**Transaction spaces are already separate.** The transaction counter is a field
on the connection, not a global, so two connections get independent wire
transaction spaces for free. The task table is per-session and looked up through
the connection. There is no correctness problem here at all.

**Per-session collections are already fields, not globals.** Chats, private
message windows, tasks, the voice model and the voice runtime all hang off
`session` and travel cleanly when `session` goes plural.

**`GtkhxSession` is Rust and carries no per-instance state.** The signal hub is
a `glib::subclass` object whose implementation struct is a unit struct — all
state is in the per-*type* signal registration. Instantiating N is a one-line
change, and the in-crate tests already build free-standing instances. The emit
ABI takes an explicit session pointer rather than reaching for the default
internally, so it supports either design in the routing question below.

**The receive layer routes correctly.** `rcv.c` and the `hxhandlers::recv`
modules take the connection they were handed and pass it into the emit as the
identity argument. There are two exceptions, named below, and they are small.

**The dock substrate exists.** The recursive split tree, the panel registry and
layout persistence are all in place.

---

## What is actually in the way

### ~~The hard stop: the transport bridge is a process singleton~~ — fixed

`src/hxnet_bridge.c` used to hold a file-static connection handle, and
`hx_bridge_send_frame` took no connection at all, so every outbound frame from
every connection would have gone to whichever was installed last. All three
install entry points hard-refused when a handle already existed, so a second
simultaneous connect was rejected outright; and the stale-actor guards, which
compare an event's handle against the installed one, could not tell a second
live connection's events from a dead actor's.

The handle now lives on `htlc_conn` (`hx_conn_bridge_handle` /
`hx_conn_set_bridge_handle`), and `hx_bridge_send_frame`,
`hx_bridge_is_installed`, `hx_bridge_uninstall` and
`hx_bridge_orchestrated_hope_aead` all take the connection they act on. The
install refusal narrowed to what is still a real error — installing twice over
the *same* connection, which would orphan the first actor. Two connections can
now hold transports at once, which is what unblocks everything else here.

What this does *not* do is make a second connection reachable. Nothing in
production constructs a second `htlc_conn` yet — M2 removed the bridge's
objection, not the last obstacle. The rest of this document is still ahead:
signals without connection identity, the flat cid/uid namespaces,
`hx_active_session()`, and the single-slot state in `network.c` below.

One hazard the move introduces, harmless today and worth knowing before
connections get a teardown path: the bridge's callbacks now dereference the
`htlc` they were handed *before* deciding anything, to read its stored
handle. A queued event whose connection has been freed is therefore a read of
freed memory where it previously was not, and uninstalling before the free is
not enough — the already-queued event still carries the pointer.

### ~~Signals without connection identity~~ — every signal is tagged now

**Every session signal now identifies where it came from.** Most already
carried an `htlc*` or a `session*`; the eight that carried neither —
`connection-state-changed`, `msg`, `user-info`, `file-info`, `file-list`,
`news-folder`, `news-catalog`, `news-thread` — each gained a leading `htlc*`,
and the two handlers that received one and discarded it for
`hx_active_session()` (login and self-updated in `gtkhx.c`) now resolve through
`sess_from_htlc`. The two tracker signals stay untagged, correctly: a tracker
listing belongs to no connection.

Almost every emit site already had the connection in hand — several were
holding it in a parameter named `_htlc`. Only two needed widening
(`hx_msg_recv` and `hx_user_info_recv`), which is the measure of how close the
model side already was.

**The choice this settled.** Tagging one hub was taken over reifying N
`GtkhxSession` objects, on the cost asymmetry noted here before: most signals
were already tagged, so the per-connection object would have meant rewiring
every emit *and* every subscribe site for no behaviour that tagging doesn't
deliver. That door isn't closed — the emit ABI still takes an explicit session
pointer, so a later split can happen without touching the signal shapes again.

**What tagging alone does not fix**, and is deliberately still open:

- Handlers that legitimately act on *app-global* chrome. The
  `connection-state-changed` handler now routes its per-session calls by
  connection, but `set_status_bar`, `gtkhx_tray_set_connected` and
  `toolbar_clear_toasts` take no session at all. See "App-global chrome"
  below.
- Singletons that take any connection's event because they are bound to none:
  the files browser and the news browser. Both now *receive* the connection;
  neither can use it until it becomes a per-connection panel.
- The flat cid/uid key namespaces below. `msg` is the one where tagging
  changed behaviour rather than carrying a pointer — the PM window lookup now
  resolves through the message's own connection — but the chat tab strip still
  keys tabs on a bare uid, so closing a tab still asks the focused session.

### Flat key namespaces that collide across servers

Two servers can both have a user with uid 5 and a private chat at cid 7. Three
tables are keyed on those with no connection dimension:

- **The chat tab strip.** A single process-wide `AdwTabView` with a cid-keyed
  map of private chats and a uid-keyed map of private messages. Every exported
  entry point takes a bare cid or uid with no session. The dock documentation's
  claim that the tab view becomes a per-panel field and "the API stays the same"
  is wrong in one specific way: the key namespace is also wrong, and that is
  true under either layout model.
- **The GIF avatar caches** in `gif_avatar.c`, keyed on bare uid, with a
  clear-all that wipes every connection's avatars on any disconnect.
- **Notification IDs** — `"msg-%u"`, `"chat-%u"`, `"pchat-%u"` and friends.
  `g_application_send_notification` replaces by id, so server B's message from
  uid 5 silently replaces server A's.

By contrast `sess->chats`, `sess->tasks`, `sess->msg_windows` and the voice
model are already per-session and need nothing. It is only the flat indexes.

### The `hx_active_session()` idiom

The single largest mechanical item. Call sites are spread across most of the
UI-facing C files and a smaller set of Rust modules, the large majority
immediately dereferencing `->htlc`. Nearly all become correct once the
*enclosing widget knows its session*, so the fix is structural rather than
site-by-site:

- **`HxUserListView` is the pattern to copy** — it already carries its session
  and cid and exposes accessors for both.
- **The chat render path** routes correctly at the top (`output_chat_from_event`
  uses `sess_from_htlc`) but the renderers beneath it reach for the active
  session to resolve self-identity for the highlight-me test. Each has a view
  pointer in hand.
- **The file browser** is a file-static singleton with no session binding at all;
  every send resolves through the active session, including task creation.
- ~~**The private-message path**~~ — fixed. The `msg` signal carries its
  connection, `msgwin_with_uid` takes the session to search, and a `msgwin`
  remembers which session's table owns it, so a close routes there rather than
  to whatever is focused. What remains is the tab strip's bare-uid key: the
  close dispatcher receives a uid and nothing else, so it still has to ask the
  active session.
- **The Rust UI reaches for a connection through C shims** —
  `gtkhx_active_htlc()` and friends in `gtkhx_ui_bridge.c` and
  `tracker_bridge.c`. Consumers are the news dialogs and browser, the user
  editor, chat input, file info, the voice panel and compose windows. The
  correct pattern is in the same file: `gtkhx_htlc_chat_window(htlc)` routes by
  connection and its comment says why.

### UI singletons that hold per-connection state

Beyond the tab strip: the threaded news browser and its in-flight fetch tables,
the flat news view, the banner, and the create-post window are all
`thread_local` singletons in `gtkhx-ui`. `useredit.rs` is the in-tree
counter-example — an id-keyed map with closures capturing the id by value — and
is the shape all of them want, keyed by connection instead of by dialog.

Genuinely app-level, and correctly so: the tracker window, the connect dialog,
bookmarks, about, and the settings pages.

### Single-slot connection state and timers

Several pieces of per-connection state live as file-statics in `network.c` and
`rcv.c` and would be clobbered by a second connection: the server address and
port (which drive every window title, the status bar and the disconnect banner),
the connected flag, the in-flight connect cancellable, the keepalive timer id
(whose start function early-returns if already set, so connection two would
never get a keepalive), the orchestrated login-reply transaction, and the
post-login fallback timer. The post-login *flag* was correctly moved onto the
connection; the timer id was not.

Note also that **there is no connection-teardown path in the tree at all.**
`hx_conn_free` has no callers; disconnect is a *reset* that leaves the
allocation alive for the next connect. Everything downstream assumes `htlc` is a
permanent, never-NULL allocation, and `hx_active_session()` never returns NULL
and never checks. Runtime creation and destruction of connections is new ground.

### App-global chrome that becomes per-connection

The status bar and its toast overlay, the connection-lost banner and its
Reconnect button, the unexpected-disconnect dialog (which names no server), the
window titles, the tray connected flag, and the disconnect sound. One tell that
this is already uncomfortable at N == 1: the toast overlay is explicitly cleared
on every `CONNECTING` transition, precisely because toasts leak across
reconnects.

Toolbar gating is in better shape than it looks. All of it is access-bit gating
in one function that already takes a session and reads bits through the
connection; there is no version gating left. What is global is the *widget set*
it writes — the user-action buttons are file-scope globals and the user
create/edit actions are app-level `GSimpleAction`s. So the logic is already
per-connection and only the targets need rehoming.

### Two latent bugs, worth fixing independently of this work

- A transfer stamps itself with the *active* connection rather than the one that
  requested it, in `hxhandlers`'s transfer construction. Everything downstream
  inherits that. Harmless at N == 1, wrong the instant there are two.
- `users_bridge.c` sets the user-action buttons sensitive straight off the global
  connected flag, bypassing the access-bit checks entirely.

---

## Connections, not bookmarks

**Decided.** The bookmarks list stops being an accessory and becomes *the
connection collection* — the durable, user-managed record of the servers this
client knows about, with everything needed to connect to each of them. The
Bookmarks dialog becomes a **Connection Manager**, and it moves into Settings
under the Network group, alongside Trackers, as **Settings → Connections**.

This is not a rename. It is the recognition that once you can be connected to
several servers at once, "the list of servers" is configuration, and per-server
settings need somewhere to live that outlives any particular connection.

**The standalone dialog is retired.** Two editors over one store is two
sync problems, and the old dialog already had to re-read the global nickname
on every form load because Settings could be open beside it. The hamburger
item survives as a shortcut that opens Settings on the Connections page. The
toolbar's split-button dropdown of server names is untouched — that is
*connecting*, not managing, and it is the one place the distinction is
already clear.

### Identity: global default, per-connection override

The identity model follows directly:

- **Settings → Identity holds the global default** — nick, icon, avatar. This is
  what a user who never thinks about it sees everywhere.
- **Each connection entry carries optional overrides.** Absent means *inherit*.
- **Effective identity at connect time is `override ?? global`.**
- **Changing the global affects every connection that has no override**, live,
  not just newly created ones. A new connection simply starts with no override,
  and its settings row displays the inherited value as its placeholder.
- **Setting an override pins that connection.** From then on it is managed there
  and stops tracking the global. Both rows carry an explicit revert control
  that clears the override and returns to inheriting — without one the
  decision is one-way, and for the icon there would have been no way back at
  all, since 0 is a real (blank) icon and so cannot double as "unset".

Note the deliberate choice between two readings of "inherit". *Copy on create*
snapshots the global into each new connection; *live fallback* stores nothing
and resolves at use. This design picks **live fallback**, because it makes
"change my nick everywhere I haven't specialised" work, which is the behaviour
people expect from a default. Copy-on-create would make the global write-once
and effectively useless after the first few connections.

### `/nick` and `/icon` change the live connection, and do not persist

Both commands run in an active server's chat, so the connection they act on is
unambiguous: they change the identity **for that connection, for that session
only**. They do not write an override, and reconnecting restores the configured
identity — `override ?? global` — as if the command had never been typed.

This keeps the two mechanisms cleanly separated. Persistent identity is
configuration and lives in Settings → Connections, where it is visible and
editable. `/nick` is the IRC-shaped ephemeral gesture: change how you appear
right now, on this server, without editing anything. Nothing is silently
persisted as a side effect of typing in a chat window, and there is no hidden
state change that later makes a global identity edit mysteriously fail to reach
one server.

Worth an affordance: a way to promote the current live identity into the
connection's saved override — a "Save current nickname for this server" item, or
simply the connection settings row showing the live value when it differs. Left
open below.

### Ad-hoc connections are transient

**Decided.** A `hotline://` URL, a hand-typed address in the Connect dialog, or a
server picked out of the tracker produces a **transient connection**. It gets no
entry in the collection, and nothing about it is written to disk. Connecting is
not an act of configuration; adding a connection is, and the user does that
deliberately in Settings → Connections.

The consequences follow cleanly and are worth stating so they don't get
rediscovered as bugs:

- A transient has no persistent home for overrides, so its effective identity is
  simply the global default. There is nowhere to store a per-connection nick
  because there is no connection entry.
- `/nick` still works on a transient, because `/nick` is ephemeral anyway. This
  is the mechanism for "connect somewhere once and use a different name."
- A transient has no stable identity across reconnect, so under layout Model B
  its panel placement cannot be persisted — a transient's panels take the
  default placement and are forgotten on exit. Under Model A the question does
  not arise. This is a further, small point in Model A's favour.
- The Connect dialog wants a "Save as connection…" affordance, so promoting a
  transient into a real entry is one click rather than retyping the address into
  Settings. Without it the rule reads as friction rather than as a clean
  separation.

**This also fixes an existing bug, by construction.** Today `/nick` calls
`hx_conn_set_name`, which writes into the very buffer the preferences table has
bound `CFG_NICK` to — so `/nick` on any server silently rewrites the user's
stored *global* nickname on the next preferences write. There is no per-server
nick today; there is only a global one that any server's chat can clobber. Once
identity storage is decoupled from connection storage, `/nick` writes only the
connection's live field, which is no longer the preferences field, and the
clobber cannot happen. No extra work is needed to fix it.

### Why this is cheaper than it sounds

**Storage is nearly free.** The bookmark store is serde over TOML with a schema
version and `#[serde(default)]` on every optional field, so adding an optional
nick and icon is purely additive: old files load in new builds and new files
load in old ones. The legacy fixed-size HTsc export cannot carry them, which is
correct — it is an interop format for clients that have no concept of
per-connection identity, so overrides simply do not round-trip through it.

**It deletes code rather than adding it.** The preferences table maps config
keys to storage *addresses*, and the nick and icon entries are the only two that
cannot be filled in statically, because they point into the heap-allocated
connection. A binder patches them once at startup. That arrangement has four
distinct problems at N > 1: one slot for N connections, so connections beyond
the first never receive identity from preferences at all; rebinding on tab
switch is a *write* hazard, since a settings dialog left open across a switch
writes into a different connection than it was opened against; the change
functions ignore their session argument and broadcast to the active connection
regardless; and the bound pointer aims into an allocation that is deliberately
never freed today, which becomes a use-after-free the moment connections can be
destroyed.

This is the half of M1 that the preferences rewrite delivers; the sequencing and
the rest of the config design are in [preferences.md](preferences.md).

Decoupling identity storage from connection storage removes all four. Nick and
icon become ordinary preference fields; the connect path resolves the effective
identity and copies it into the connection; the binder is deleted outright.
**This is a net simplification at N == 1 and can land ahead of everything else
here.**

**It supplies the stable connection identity the layout work needs.** Model B
below requires a key that survives disconnect and reconnect so a saved panel
placement can find its connection again. A connection entry in the collection
*is* that key. Making bookmarks the connection collection hands Model B the
identity it was otherwise missing.

### Still to decide here

- **Avatar.** The GIF avatar is a single file in the config directory. Per-
  connection avatar is a third axis; leaving it global for v1 is defensible
  ("different name per server, same face") but should be a stated decision.
- **Login and password are already per-bookmark**, so account identity and
  display identity are about to live side by side in the same row. Worth a
  thought about whether the page reads coherently.
- **Promoting a live identity.** If `/nick` is deliberately ephemeral, is there
  an affordance to keep it — and does the connection settings row surface the
  fact that the live identity currently differs from the configured one? Still
  open: the page shows the *configured* identity in each row's subtitle, and
  says nothing about what a live connection is currently using.

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

This axis is **open**. Note that the identity rework above is independently
worth doing under any of the three.

### Axis 2 — Signal routing

Covered under "Signals without connection identity" above. The two options are
one `GtkhxSession` per connection, or one hub with a connection tag on the
signals that currently lack one. The recommendation leans to the former as the
long-term shape; the latter is cheaper today because most signals are already
tagged.

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
- Settings, including the connection collection.

**Global but tagged:**

- Tasks and the transfer queue. One global list of every connection's in-flight
  transfers, with a per-row tag naming the connection. This matches the "one
  place to watch every transfer" instinct.

  The alternative — per-connection transfer queues — is **rejected**. It reads
  as more consistent with the per-connection tier, but it makes the common
  question ("is anything still going?") into a hunt across tabs, and a stalled
  upload on a connection you aren't currently looking at becomes invisible. The
  queue's job is to be the one place you check.

  Implementation note: the live-transfer registry is already one flat
  process-wide list and each handle carries its own connection, so the tagged
  view is close to free. What does not exist is a per-connection *filter* or a
  cancel-sweep, which disconnect will need.

### Axis 4 — Voice exclusivity

The constraint is **one voice chat at a time, anywhere**: joining voice on
server B, or a different room on server A, must first leave the current one.

The runtime is per-session and instantiable, each with its own pipeline and
`webrtcbin` and its own mute state, tracked by an id-keyed registry that was
already built for N. The model is likewise per-session. So the pieces are in
place and the design holds:

- Keep **per-session voice models** — each server's user list still needs to
  render *its* speaker indicators.
- Gate **runtime activation** through a process-global arbiter: a single "who
  owns voice" token holding a (session, room) pair, or none.
- Acquiring the token tears down whatever currently holds it — send the voice
  leave on the old room, stop that session's runtime — *before* joining the new
  one.
- The pipeline, mic capture, mute and push-to-talk state are inherently
  singular, because there is one microphone. The arbiter isn't an arbitrary
  restriction; it's modelling the hardware.
- UX: preempt-on-join with a confirmation — "leave voice on *ServerA* to join
  here?" — plus a clear indication in the tab strip of which connection holds
  voice.

**What is missing is only the arbiter itself.** Nothing today prevents two
runtimes opening the mic simultaneously; the audio backend would decide the
outcome. There is a single clean choke point to add it — the runtime's join
entry point — so this is a contained addition rather than a refactor. The one
loose end is the voice panel's C callbacks, which receive no session context and
resolve through the active session; the runtime already knows its own id, so
threading it through the callback user data is the fix.

This remains the least risky of the four axes.

---

## The layout model — two directions, both open

Per-connection panels have to relate somehow to the *single* dock tree and its
panel registry, which is keyed today by a stable string id (`"chat"`,
`"users"`, …). Two models, deliberately not decided here.

**Shared cost first**, because it dominates and is identical under both: the
content modules that are process singletons (file browser, threaded news
browser, flat news, the chat tab strip) all have to become keyed by connection;
the cid/uid key collision has to be fixed; the per-role "is this view open"
preference flags become per-connection; panels get built by a per-connection
factory at connect time rather than eagerly at toolbar construction. None of
that differs between A and B.

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

- **Pros:** the conventional IRC-client model. Persistence genuinely does not
  change — same ids, one tree, one layout file, one `[Undocked]` section.
- **Cons:** you can't see two servers' chats side by side. The dock *shape* is
  shared across all connections, so you can't have a wide Files panel for a file
  server and a wide Chat for a chat server at the same time.
- **Costs the earlier draft of this document missed:**
  - **There is no content-swap mechanism at all.** The dock bridge sets a
    panel's child exactly once and exposes no way to replace it. Model A needs
    one — most cleanly a per-panel stack.
  - **Every window entry point uses "is the panel already open?" as its
    build-once test**, so connection two early-returns and never builds its
    content. That is a build-order bug, not a UI decision, and it is at the head
    of all six entry points.
  - **Inactive content trees need an owner** or they are destroyed when swapped
    out — and the destroy handlers *are* the model-side teardown paths, so a
    naive reparent-and-drop silently nulls the state of the connection you just
    switched away from.
  - **Attention badges need demultiplexing.** Background activity currently
    badges the one Chat panel; under A it has to reach the *connection tab*
    instead, which is a new signal path.
  - **Undocked windows mutate under the user.** Detach ServerA's user list to
    watch it, switch tabs, and it silently becomes ServerB's — same title, no
    affiliation cue. For a project whose ethos is undock-everything this is a
    real loss of capability, and it is the case Model A cannot express.

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
  two servers at a glance. Per-server undocked windows and per-server titles come
  free, because each panel is a distinct object with its own title. It also keeps
  the existing build-once-embed-once shape exactly — each panel owns one content
  tree for its whole life, which is what every content module already does.
- **The registry and grammar cost is *not* what an earlier draft of this document
  claimed.** The panel registry is a plain string hash table and nothing anywhere
  parses an id's structure, so a composed id like `users@ServerA` needs no
  registry change at all. The layout parser's id character set already accepts
  such ids (it must avoid the comma, bracket and the colon that separates the
  role tag), and GKeyFile tolerates the same characters in the `[Undocked]`
  keys. What actually changes is the set of call sites that pass compile-time
  constant ids — roughly a dozen — plus one id-composition helper.
- **The real cost is persistence and identity**, which the earlier draft did not
  mention:
  - **The save path serialises only the live tree.** A disconnected server's
    panels are gone from the dock, so their ids drop silently out of the layout
    file and a carefully arranged placement is forgotten the moment that server
    disconnects and the debounce fires. B needs merge-with-previous logic keyed
    on connection tags — genuinely new code with new failure modes.
  - **Restore needs a stable connection identity.** A reconnected server whose
    tag does not match what was saved misses its leaf and lands in the default
    frame. This is exactly what the connection collection above provides, and it
    is why that work is a prerequisite rather than a nicety. Restoring a layout
    for a connection that has not come back is benign — the leaf simply stays
    empty, which is the dock's designed behaviour.
  - **Toolbar disambiguation.** The panel buttons carry a literal panel id as
    their action target; under B, "show Users" is ambiguous and needs either a
    per-connection button row or a dropdown.
  - **Proliferation.** Four servers times four per-connection panels is sixteen
    panels to place, persist and undock.

### Where this leaves the choice

On the evidence, the honest summary is the reverse of the earlier framing.
**Model B is cheaper in the dock layer** — no content-swap machinery, no stack
of inactive trees, no badge demux — and **Model A is cheaper in persistence and
identity**, since nothing in the layout file changes and the dock stays six
panels no matter how many servers.

The recommendation, weakly held: **A as the shipped default**, because its costs
are bounded, mechanical and local, whereas B's are open design problems in
persistence semantics. But the old justification ("B is expensive because of the
registry") should not be used to make that call, because it is wrong.

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
  arbiter can refer to it. The connection collection supplies this; what is still
  missing is any teardown path at all, since disconnect is currently a reset.
  Note `hx_conn_reset` zeroes the whole struct, transport handle included —
  safe only because the one caller uninstalls first. A second caller would leak
  an actor and its socket, so runtime destruction of connections has to keep
  that ordering.
- **Bookmarks to tabs.** An "open in new tab" affordance. The toolbar's
  connect-with-bookmark split button needs a "new tab vs. replace current"
  decision.
- **Connection-loss banner.** Becomes per-connection, shown in that tab's
  subtree, rather than app-global.
- **Toolbar access-bit gating.** The gating logic already takes a session and
  reads the connection's bits correctly; it is the button and action set it
  writes that is global. Under Model A this re-evaluates on every tab switch;
  under Model B the semantics are genuinely ambiguous, since the buttons live in
  a shared toolbar while the panels are per-server.
- **The chat tab view singleton.** Becomes a per-Chat-panel field *and* needs its
  cid/uid key namespace qualified by connection.
- **Layout persistence across connections.** Model A → one global layout,
  unchanged. Model B → connection-tagged ids plus save-side merging.
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
| ~~M1~~ | **Done.** Connection collection: bookmarks are Settings → Connections; identity is decoupled from connection storage and resolved as override-else-global at connect; the preferences binder is deleted. The identity half rode in with the preferences rewrite (P5); the collection half retired the standalone Bookmarks window and added the per-connection icon override with an explicit clear-to-inherit for both fields. | — |
| ~~M2~~ | **Done.** The transport handle moved onto the connection and the send primitive takes one; the install refusal narrowed to a second install over the same connection. Two connections can hold transports at once. | — |
| ~~M3a~~ | **Done.** Connection identity on all eight untagged signals; the two discard-the-htlc handlers fixed; the PM window routed by the message's connection. | M2 |
| M3b | Connection tags on the flat cid/uid indexes and notification ids. Three unrelated clusters: notification ids, the GIF avatar caches, and the chat tab strip — the last of which M4 reworks anyway, so it may be cheaper to do there. | M3a |
| M4 | Reify the connection/session as a heap object behind a collection and factory; connection tab strip; pick layout Model A or B; per-Chat-panel tab view; per-connection factory replaces eager panel construction. | M1, M3 |
| M5 | Voice arbiter: global token, preempt on acquire; per-session voice models retained. | M4 |
| M6 | Global transfer queue with per-connection tags and a disconnect sweep; per-connection loss banner, status bar, titles and tray; bookmarks "open in new tab". | M4 |
| M7 | Layout persistence across connections; two-server isolation test. | M4 |

**M0 has landed** — it is what the "session model as it stands" section
describes. It was safe to do ahead of every open decision precisely because it
changes no behaviour at N == 1.

**M1 and M2 have both landed**, independently of each other and of everything
else here. M1 was a net simplification at one connection, and the
`/nick`-clobbers-your-global bug went with it by construction, exactly as
predicted below — once identity storage stopped being the connection's wire
buffer, there was nothing left for the command to clobber. M2 was the hard
stop: with the transport handle per-connection, the bridge no longer refuses
a second connect, so the rest of this document can be built against something
that will not reject it out of hand. Reaching a second connection still needs
M3 and M4.

**M3a has landed**: every signal now says which connection it belongs to, so
the model side can no longer misroute an event to the focused session by
default. What is left of M3 is the flat key namespaces, which are three
independent clusters rather than one job.

---

## Open questions

1. **Sequencing.** Now, later, or the middle path?
2. **Layout model.** A, B, or the hybrid? Both are documented above without a
   pick, and the cost comparison above corrects an earlier one that got it
   backwards.
3. **Signal routing.** Per-connection `GtkhxSession` objects, or one hub with
   connection-tagged payloads?
4. **Per-connection avatar.** Nick and icon are settled; the GIF avatar is a
   third axis and is currently a single file.
5. **Reconnect and tab restore.** Do tabs persist across app launches, and how
   does that interact with the saved dock layout? Tab reordering and detach
   persistence needs its own design. Transients presumably do not survive a
   restart at all, which is one more thing the tab strip has to express.
6. **Maximum connections.** Is there a sane upper bound, or is it "until the
   machine complains"? Affects whether the tab strip needs overflow handling.

**Settled:** the transfer queue is global with per-connection tags. The
bookmarks list becomes the connection collection under Settings → Connections.
Identity is a global default with per-connection overrides, resolved live rather
than copied on create; `/nick` and `/icon` change the running connection only
and never persist. Connecting to an address that is not in the collection
produces a transient connection — adding an entry is a deliberate act in
Settings, not a side effect of connecting.

## What this is *not*

- Not a commitment to any timing relative to the Rust port.
- Not a decision between layout models A and B.
- Not a change to the wire protocol. Multi-conn is entirely a client-side
  concern; 1.2 / 1.5 / 1.9 compatibility is untouched, and each connection
  speaks the protocol exactly as the single-session client does today.
