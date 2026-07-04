# Re-thinking the chat / gchats model for Rust

**Status:** design / decision doc. Written mid-way through the `chat.c` port
(after the wire senders and the public-chat content build landed in Rust),
when the next planned increment — the input handler + nick completion — turned
out to be the heaviest consumer of the per-session chat model. Misha's steer
was explicit: the gchats model *"really needs a re-think, not just a straight
port."* This doc is that re-think, and it sequences the input/nick-completion
work behind it.

## TL;DR

The client keeps **two** per-session hashtables keyed by the same chat id and
maintained in lockstep — `session->chats` (`struct chat`, the protocol model)
and `session->gchats` (`struct gtkhx_chat`, the UI). `struct gtkhx_chat` is a
god-object: it mixes a back-pointer into the model, seven live widget handles,
GNU-readline command history, chat-history render cursors that are **raw
pointers into xtext's internal entries**, and an inline-media token table.
Membership is stored once in `chat->users` (a `GHashTable<uid, hx_user*>`) and
then *again*, effectively, in the `HxUserListView`'s list model.

A straight field-for-field port would carry all of that tangle into Rust. The
proposal instead splits the one god-struct into the three concerns it actually
is:

1. a **pure, testable conversation model** (`Conversation` + `Member`) that
   owns membership and subject and nothing GTK;
2. a **`gio::ListModel` of members** that the user list binds to directly, so
   membership has a single source of truth; and
3. a **per-conversation view object** that owns the widgets + the readline
   history + the chat-history render cursors + the inline-media table as named
   sub-structs, not as siblings in one flat allocation.

Nick completion and the tab-cycle logic become **methods on the pure model**,
unit-tested without a display — which is exactly why the model re-think comes
*before* the input-handler port rather than after it.

Wire-format compatibility is untouched throughout: this is all client-side
state shape, not protocol.

## Where we are today

### The structs

`struct chat` (`session.h`) — the protocol model, one per open chat:

```c
struct chat {
    guint32     cid;        /* 0 == public / lobby, always present */
    guint32     nusers;
    GHashTable *users;      /* <u16 uid, struct hx_user*> */
    char        subject[256];
};
```

`struct hx_user` — a member (also the thing the user-list view renders):

```c
struct hx_user {
    guint16 uid, icon, color;   /* color = Admin/Guest/Away status bitmap */
    guint32 nick_color;         /* 0x00RRGGBB, or HX_NICK_COLOR_NONE */
    char    name[32];
    unsigned int ignore : 1;
};
```

`struct gtkhx_chat` (the "gchat") — one per open chat window/tab, and the
problem child. Its fields fall into four unrelated groups:

| Group | Fields |
|-------|--------|
| **model back-ref** | `cid` (duplicates `chat->cid`), `chat` (raw ptr) |
| **widgets** | `window`, `vscroll`, `output` (xtext), `input`, `subject`, `voice_panel`, `userlist` (`HxUserListView`) |
| **readline history** | `chat_history` (GNU readline `HISTORY`), `chat_history_draft` |
| **chat-history ext. render cursors** | `history_oldest_msgid`, `history_has_more`, `history_loading`, `history_anchor_ent` + `history_load_older_ent` (**`textentry*` — pointers into xtext's buffer**) |
| **inline media** | `media_attach_btn`, `media_handles` (`<token, HxChatMedia*>`), `media_next_id` |

Both structs live in per-session `GHashTable`s keyed by `cid`:
`session->chats` and `session->gchats`, each seeded with the public chat at
`cid = 0`.

### Who touches it

A quick census of the source tree (`.c` files referencing each):

- `chat_with_cid` — chat.c, chat_send_bridge.c, commands.c, msg.c, notify.c,
  options.c, rcv.c, users.c
- `gchat_with_cid` — chat.c, notify.c, users.c
- `->users` — chat.c, msg.c, network.c, users_bridge.c, users.c
- `->gchats` — chat.c, chat_tabs.c, gtkutil.c, inline_media_attach.c,
  options.c, users.c
- `hx_user_with_uid` — commands.c, msg.c, options.c, rcv.c, users.c

Membership mutation (add / remove / rename / recolour) originates in `rcv.c`
on `USER_LIST` / `USER_CHANGE` / `CHAT_USER_CHANGE`, plus `users.c`. The
model→view hop already exists as the `GtkhxSession` signal taxonomy
(`user-create` / `user-delete` / `user-change` / `users-clear`, `chat`,
`chat-subject`, `chat-invitation`), so today's flow is roughly: wire → fill
`chat->users` → emit a `GtkhxSession` signal → a view handler re-derives the
`HxUserListView` row.

## What's actually wrong (not just "it's C")

1. **Two hashtables in lockstep.** `chats` and `gchats` are keyed by the same
   `cid` and created/destroyed in parallel by hand (`chat_new`/`chat_delete`
   vs `gchat_delete`/`gchat_free`). `gtkhx_chat` even re-stores `cid` and holds
   a raw `chat*` back-pointer. Nothing enforces that the two tables agree; it's
   a convention maintained across eight files.

2. **`gtkhx_chat` is four objects wearing a trench coat.** A single `g_malloc0`
   / `chat_free` pair manages a model back-ref, live GTK widgets, a readline
   history handle, xtext-internal render cursors, and a media token table.
   These have different lifetimes and different owners (some cleared on panel
   destroy via `gtkhx_chat_clear_content_ptrs`, some freed in `gchat_free`),
   and the flat layout hides that.

3. **Membership is stored twice.** `chat->users` is the authoritative map, but
   the `HxUserListView` keeps its own `gio::ListModel` of rows that has to be
   kept in step by re-deriving from signals. Two representations of one fact.

4. **Render cursors are pointers into a widget's guts.** `history_anchor_ent`
   and `history_load_older_ent` are `textentry*` — raw pointers into xtext's
   internal buffer entries — living on the model-ish struct. They're fragile
   (entry auto-trim can dangle them) and they weld the view-model to one
   widget's private representation.

5. **Membership has no order.** `chat->users` is a hashtable, but both nick
   completion (`public_chat_users_sorted`) and the user list want *ordered*
   iteration, so a fresh sorted `GSList` is rebuilt on demand each time.

6. **None of it is testable.** Nick completion, dedup, and the tab-cycle logic
   are ~500 lines of raw-buffer + `GSList` manipulation tangled with GTK
   widgets, `gtkhx_prefs`, and the active-session global. There is no
   window-level test coverage on the display-less CI, so this logic is
   currently unverified except by hand.

## Target shape

Split the god-struct into the three concerns it conflates. The guiding line is
the same model / view boundary the rest of the R4–R5 work has drawn: pure,
testable model in Rust; GObject where GTK needs to observe it; widgets and
their transient render state on the view.

### 1. `Conversation` + `Member` — the pure model

A plain-Rust, no-GTK crate (working name `hxchat-model`), unit-tested like
`hotline-proto` / `hxchat-send`:

```rust
pub struct Member {
    pub uid: u16,
    pub icon: u16,
    pub status: u16,          // Admin/Guest/Away bitmap (was hx_user.color)
    pub nick_color: NickColor,// Some(rgb) | None (was the 0xFFFFFFFF sentinel)
    pub name: String,         // real String, not char[32]
    pub ignore: bool,
}

pub struct Conversation {
    pub cid: u32,
    pub subject: String,
    members: MemberList,      // ordered + uid-indexed (see below)
}
```

`Conversation` owns the membership operations that are scattered across
`rcv.c` / `users.c` / `chat.c` today — `add` / `remove` / `update` /
`get(uid)` — and, crucially, the completion logic:

```rust
impl Conversation {
    pub fn nick_matches(&self, prefix: &str) -> Vec<&Member>;   // was tab_nick_comp's match list
    pub fn common_prefix(&self, prefix: &str) -> Option<String>;// was the match_char/match_pos walk
    pub fn cycle_nick(&self, current: &str, reverse: bool) -> Option<&str>; // was nick_comp_chng
}
```

Those three are pure functions of `(members, prefix)` — perfect unit-test
targets, and the reason this crate lands *before* the key handler.

**Membership representation.** Back `MemberList` by an insertion/name-ordered
structure with O(1) uid lookup and stable iteration, retiring the
rebuild-a-sorted-GSList-per-keystroke pattern. `IndexMap<u16, Member>` (order +
lookup in one container) is the natural fit; a `Vec<Member>` + `HashMap<u16,
usize>` side-index is the dependency-free alternative.

### 2. Members as a `gio::ListModel` — one source of truth

Rather than fill `chat->users` and separately drive the `HxUserListView`'s
model, expose the membership *as* the list model the view binds to. `HxUserRow`
is already a Rust GObject (`users_row.rs`); the `HxUserListView` is already
Rust (`users_view.rs`). Give `Conversation`'s member list a `gio::ListStore`
(or a custom `ListModel` over the Rust model) of member GObjects, and the view
binds directly. Membership changes become list-model splices — GTK's list
diffing does the row add/remove/update, and the hand-rolled
`user-create`/`-delete`/`-change` fan-out either disappears or becomes a thin
adapter that mutates the model. One representation, observed in two places.

### 3. `ConversationView` — widgets + transient render state

The remainder of `gtkhx_chat` becomes a per-conversation view object (a Rust
GObject, or a plain struct owned by the tab) holding a *reference* to its
`Conversation` — not a raw back-pointer plus a duplicated `cid`:

```rust
struct ConversationView {
    convo: Rc<Conversation>,          // or a GObject handle
    // widgets: output(xtext, stays C leaf), input, subject, vscroll, ...
    history: ReadlineHistory,         // was chat_history + _draft
    render: HistoryRenderState,       // was the history_* cursors, named
    media: MediaTable,                // was media_handles + media_next_id
}
```

The xtext-internal `textentry*` cursors stay a *view* concern (they belong to
the widget), but as a named `HistoryRenderState` sub-object rather than loose
fields on a shared struct — so their fragile widget-coupled lifetime is
contained and obvious.

### 4. One registry, not two

Collapse `session->chats` + `session->gchats` into a single `Conversations`
map (`cid → Conversation`) in the model, with views attaching/detaching. The
public chat (`cid = 0`) is seeded once. A future per-connection design (the
`MAX_CONN` abstraction is still a placeholder — see the multi-conn notes)
drops out naturally as one `Conversations` per connection, instead of today's
two-tables-times-N.

## Phasing (leaf-up, always compiling, wire-compat untouched)

- ✅ **M1 — pure model + nick completion.** *Shipped.* Stood up `hxchat-model`
  with `Member` / `MemberList` and the tested completion (`complete_styled`).
  `chat.c::tab_nick_comp` calls into it via `hx_nick_complete`; the input
  handler's Tab branch is now a thin call into the Rust completer. (`chat->users`
  stays the C authoritative store for now — see M2/M4b.)

- ✅ **M2 — members as a ListModel (Option A).** *Shipped.* Membership is a
  `gio::ListModel` (`HxMember` / `HxMemberModel`, `hxmember-model`), and each
  chat owns an authoritative `struct chat::member_model` fed by the `users.c`
  fan-out. Landed as **Option A** (authoritative model, view observes) rather
  than the bolder "bind `HxUserListView` to it directly and retire the row
  store": the rendered list keeps its own `HxUserRow` store (the C snapshot
  cell + selection need `hx_user*`), and the model is the *data* source of
  truth for consumers (completion). Fully binding the view to the model — and
  retiring the parallel `chat->users` — is folded into **M4b**.

- ✅ **M3 — collapse the gchat.** *Shipped, adjusted.* Rather than introduce a
  single `ConversationView` GObject up front, the god-struct's sub-concerns
  were extracted one at a time: the readline history → Rust `InputHistory`
  (retired `history.c`; also wired into PM inputs); the media token table →
  Rust `MediaTable` (`gtkhx-boxed`; retired the `GHashTable` + `media_next_id`);
  the chat-history render cursors → a named C `struct hx_chat_history_render`
  (**stays C** — its two `textentry*` are raw pointers into xtext internals and
  can't cross FFI); and the raw `gtkhx_chat->chat` back-pointer dropped (derive
  via `chat_with_cid`, keeping `cid` as the view's identity).

- **M4 — retire the C structs.** *In progress.* The survey found the model and
  view are **not 1:1 in lifetime** (private-chat models appear first on
  `USER_CHANGE`; the view attaches lazily in `create_pchat_window` and detaches
  first on close), so this can't be one atomic "delete both structs" step. It
  splits:
  - ✅ **M4a — one registry.** *Shipped.* Collapsed `sess->gchats` into
    `sess->chats`: the model (`struct chat`, always present) is the single
    per-conversation entry and owns an optional `struct chat::view`.
    `gchat_with_cid` is now a thin wrapper over `chat_with_cid(sess, cid)->view`;
    `create_chat` / `pchat_new` attach the view to its model; `gchat_delete` /
    `pchat_close` detach + free it while the model lingers; `chat_free` frees a
    still-attached view. Every `sess->gchats` iteration (theme, options, users
    fan-out, inline-media refresh, teardown) now walks `sess->chats` and takes
    `->view` (skipping window-less models). Kills the two-tables-in-lockstep
    hazard (issue #1).
  - **M4b — retire the structs.** *Remaining.* Fold the membership duplication
    (`chat->users` vs `member_model`, issue #3) into one authoritative store the
    view binds to, and — once a `Conversation`/`ConversationView` object the
    view references exists — delete `struct chat` / `struct gtkhx_chat` and the
    last loose `cid`, routing every remaining consumer (`rcv.c`, `users.c`,
    `notify.c`, `inline_media_attach.c`, …) through the model's FFI or porting
    it.

Each phase leaves a working binary and keeps the `GtkhxSession` signal boundary
intact (or subsumes it deliberately), rather than fighting it.

## Why this ordering (the input-handler connection)

The input handler is entangled with the model precisely at nick completion:
`tab_nick_comp` walks `chat_with_cid(sess, 0)->users`, builds a match list,
dedups it, and computes a common prefix — all logic that belongs *on the
model*. If we ported `chat_input_key_pressed` first, against today's C
`chat->users`, the completion half would be rewritten again the moment the
model moved. Doing M1 first means the key handler is written once, against the
tested Rust `Conversation`, and the completion algorithm gets real unit tests
in the bargain — the thing it has never had.

## Constraints and non-goals

- **Wire compatibility is untouched.** This is client-side state shape only;
  no `USER_LIST` / `USER_CHANGE` / `CHAT_*` byte changes. The 1.2/1.5/1.9
  compat guarantee is unaffected.
- **xtext stays C.** The output widget is vendored and stays C; the render
  cursors that point into it stay a view concern, just better contained.
- **Test burden lives in the model.** Because there's no window-level Tier-3
  coverage on the display-less CI, the pure `Conversation` model must carry the
  verification weight — which is the point of extracting it.
- **Not a multi-conn project.** M1–M4 can stay single-session; the design just
  shouldn't *preclude* a future per-connection `Conversations` map (it doesn't).
```
