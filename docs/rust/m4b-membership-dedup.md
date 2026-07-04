# M4b — membership dedup + retire the C chat structs

Scoping doc for the final phase of the chat-model re-think
([gchats-model-rethink.md](gchats-model-rethink.md)). M4a collapsed the two
per-session tables into one (`sess->chats`, the model owning an optional
`struct chat::view`). M4b is the harder half: **retire the C structs**
(`struct chat`, `struct gtkhx_chat`) so the conversation is a single Rust
object the view references. Its gating problem is the **membership
duplication** (design issue #3).

This is deliberately broken into small, individually-reviewable increments
because there is **no display-level test coverage** on the headless CI (the
gtk4-rs windows can't be built without a display), so the pure-Rust model
layer must carry the verification weight and each C-side migration has to be
reasoned + grep-verified rather than caught by a window test.

## The duplication today

Every `struct chat` carries membership **twice**:

| Store | Type | Written by | Read by |
|-------|------|-----------|---------|
| `chat->users` | C `GHashTable<u16 uid, struct hx_user*>` | `users.c` (`hx_user_new`/`_delete`), `rcv.c` (field writes on USER_CHANGE / USER_LIST) | `rcv.c`, `users.c`, `commands.c` via `hx_user_with_uid` / `_with_name` |
| `chat->member_model` | Rust `HxMemberModel` (`gio::ListModel` of `HxMember`) | `users.c` fan-out (`hx_member_model_upsert`/`_remove`/`_clear`) | nick completion (`hx_nick_complete`) only |

`struct hx_user` and `HxMember` hold the **same fields**: `uid`, `icon`,
`color` (status bitmap), `nick_color`, `name`, `ignore`. The two stores are
kept in step by hand in the `users.c` `user_create` / `_delete` / `_change`
fan-out. `chat->users` is authoritative today; `member_model` is a read-only
shadow used only for completion.

### Why it's coupled

Three things make "just delete `chat->users`" a multi-step job, not a
one-shot:

1. **`rcv.c` writes `hx_user` fields on the hot path.** USER_CHANGE
   (`rcv.c` ~731) and USER_LIST (`rcv.c` ~2330) `memcpy` the name and assign
   `icon` / `color` / `nick_color` directly into the struct. These need FFI
   setters (or to be re-expressed as `hx_member_model_upsert`, which already
   takes all those fields) once the struct is gone.
2. **The view render dereferences `hx_user*`.** `HxUserRow` (`user_row.rs`)
   holds a *borrowed* `struct hx_user *` and, at snapshot time, calls
   `user_nick_color_gdk(user, …)` and `hx_user_uid(user)` on it. The view's
   `HxUserListView::by_user` map is **keyed on the raw `hx_user*` address**
   (`users_view.rs`), not on uid. So the row identity is pointer-coupled to
   the C struct — it must be re-keyed on uid before `chat->users` can go away.
3. **`ignore` lives only in `chat->users`.** `hx_member_model_upsert`
   hard-codes `ignore = false`; the model can't represent it yet. The UI
   toggles `user->ignore` (users.c / commands.c) and `rcv.c` reads it to
   filter ignored users' messages. The model has to become authoritative for
   `ignore` before it can be authoritative at all.

## The FFI gap

`chat_members.rs` today is **write-only** over the model: `upsert` / `remove`
/ `clear` + the completion reader. There is no read-by-uid, no field getters,
and no `ignore` setter. The Rust model gains the `ignore` methods in M4b.2;
the C-facing `#[no_mangle]` FFI (read-by-uid, field getters, ignore
set/get/toggle) lands together with its consumers in M4b.4, so no dead FFI
symbols are introduced ahead of use.

## Increments (leaf-up, each leaves a green build)

- **M4b.1 — kickoff (shipped).** This doc + delete the **dead
  `chat->nusers`** field. It is written in four places (`network.c` reset,
  `rcv.c` ++/--/++) and **read nowhere** — a pure duplicate of the member
  count. Trivially safe; shrinks `struct chat` by one field and removes the
  four write sites. The live member count, if ever needed, is
  `HxMemberModel::n_items`.

- **M4b.2 — model gains the capability to own `ignore` (this change).**
  Pure model-layer, cargo-tested, **no runtime behaviour change yet**:
  `HxMemberModel::set_ignore` / `get_ignore` / `toggle_ignore`, and `upsert`
  (via `HxMember::set_from`) now **preserves** an existing member's `ignore`
  instead of clobbering it on a presence update — so a USER_CHANGE can't clear
  it once it's set. Unit tests cover default-false, round-trip, survives-update,
  absent-uid, and dropped-on-rejoin.

  > **Why the C consumer migration is *not* in this step.** The deep-dive
  > found the `ignore` flag can't move off `hx_user` yet: the UI sites that
  > toggle it (`on_user_ignore`/`_unignore`, `view_igno_btn`) hold a raw
  > `struct hx_user *` (via `struct UserActionCtx { session*; hx_user*; }` and
  > `view_selected_user`) and **carry no cid/chat**, so they can't reach the
  > right `chat->member_model`. And three of the four read sites
  > (`rcv.c` msg/pm/invite) already resolve against the **public chat (cid 0)**
  > while the fourth (USER_CHANGE) uses the event's own cid — so the flag is
  > effectively per-chat today. Migrating reads without writes would silently
  > break ignore-filtering (the model's flag would stay false), so it's
  > all-or-nothing and **gated on the identity re-key below**. `hx_user::ignore`
  > gets deleted as part of M4b.4.

- **M4b.3 — re-key the UI + view identity from `hx_user*` to (cid, uid).**
  *The real prerequisite the deep-dive surfaced.* `hx_user*` is the identity
  handle in three places that must stop depending on the pointer:
  `HxUserListView`'s row map (keyed on the pointer address), the row's
  render-time deref, and `struct UserActionCtx` / the right-click + button
  handlers (which hold `hx_user*` with no cid). Split:
  - ✅ **M4b.3a — key the view's row map on uid (shipped).** `HxUserListView`'s
    `by_user: HashMap<*mut c_void, HxUserRow>` → `by_uid: HashMap<u16, …>`; the
    `add`/`remove`/`update`/`refresh_avatar` FFI derive the uid via
    `hx_user_uid(user)` at the boundary. uid is 1:1 with the pointer within a
    chat (`chat->users` is uid-keyed), so it's behaviour-preserving — but now
    the map survives the `hx_user` structs being freed. View-crate-local, no C
    changes.
  - ✅ **M4b.3b-i — render off the pointer (shipped).** `HxUserRow` caches
    `uid` + `nick_color` (refreshed from the borrowed `user` at construct /
    set_state, where it's known-valid), and `refresh_fg` / `uid_of` read the
    cache instead of dereferencing `hx_user*` — via a new pointer-free
    `user_nick_color_rgb(nick_color, status)` (the old `user_nick_color_gdk`
    now delegates to it) + a `hx_user_nick_color` accessor. The row keeps the
    `user` pointer *only* as an opaque selection token. Behaviour-preserving.
  - ✅ **M4b.3b-ii-A — selection API + toolbar/activate off the pointer
    (shipped).** `HxUserListView` gained its `cid` (both call sites pass it);
    `get_selected_user` → `get_selected_uid` + a `get_cid` accessor;
    `on_activate` uses the row's cached uid + name. The toolbar/menu handlers
    are untouched: `view_selected_user` now resolves the struct *fresh* via
    `hx_user_with_uid(chat_with_cid(sess, get_cid), get_selected_uid)` — which
    also fixes a latent use-after-free (the old borrowed pointer went stale if
    the user left between selection and click). The row still carries the
    `hx_user*` only for the right-click popup path.
  - ✅ **M4b.3b-ii-B — popup off the pointer, drop the token (shipped).**
    `on_secondary_press` + `user_popup_show` + `UserActionCtx` are re-keyed on
    `(cid, uid)`; the popup handlers resolve the struct fresh (`ctx_user`,
    which also removes the dangling-pointer-while-menu-open hazard) only where
    still needed (the header + the not-yet-migrated `ignore` write). The
    `HxUserRow` `user` field, `user_ptr`, `hx_user_row_get_user`, and the
    dormant C-ABI row ctors (`_new` / `_set_state` / `_touch`) are deleted.
    **The view/UI now hold no `hx_user*` at all** — the M4b.4 prerequisite.

- **M4b.4 — model authoritative, retire `chat->users`.** With identity re-keyed
  (M4b.3) and the model able to own `ignore` (M4b.2), make `HxMemberModel` the
  single membership store. Split:
  - ✅ **M4b.4a — `ignore` on the model (shipped).** Added the C FFI
    `hx_member_model_{set,get,toggle}_ignore` and routed every `ignore` site
    through it — the `rcv.c` reads (msg / pm / invite / USER_CHANGE, all of
    which have `chat` + uid in hand), the `users.c` toggles (`on_user_ignore` /
    `_unignore` via the popup's `(cid, uid)`, `view_igno_btn` via the view's
    `cid` + selected uid), and the `commands.c` `/ignore`. `hx_user::ignore` is
    deleted; the per-chat `member_model` is now the authoritative store (the
    per-chat semantics are preserved — reads/writes hit the same chat's model).
  - ✅ **M4b.4b-i — view FFI takes member values (shipped).**
    `hx_user_list_view_add`/`_update`/`_remove`/`_refresh_avatar` now take
    `(uid, …, nick_color)` instead of `struct hx_user*`; `HxUserRow::new_row` /
    `set_state_row` copy the values (the `recache_ids` helper + the
    `hx_user_uid` / `hx_user_nick_color` externs are gone). No `hx_user*`
    crosses into the `gtkhx-ui` crate at all now. The `users.c` fan-out passes
    `user->uid` + `user->nick_color`.
  - ✅ **M4b.4b-ii — C readers → a model read-FFI (shipped).**
    `hx_member_model_get_info(model, uid, struct hx_member_info*)` fills a
    value struct (uid / icon / status / nick_color / name[32]; `#[repr(C)]`
    layout pinned by `offset_of` asserts) and `hx_member_model_find_by_name`
    replaces `hx_user_with_name`. Routed **all** the reader paths: `msg.c` PM
    header, `commands.c` `/msg` + `/ignore`, and (4b-ii-b) `users.c`'s
    `view_selected_user` → `view_selected_member` + the six toolbar handlers,
    the popup header + `on_user_ignore`/`_unignore`/`_msg` (`ctx_user`
    deleted), and `users_refresh_avatar`. `options.c` is a *writer* (deferred
    to 4b-iii with the fan-out). The only `hx_user_with_uid` left is the
    `user_change` fan-out loop + the store defs — all 4b-iii.
  - ✅ **M4b.4b-iii-A — `user_list` + fan-out read from the model (shipped).**
    New FFI `hx_member_model_count` / `_get_at` (walk by index) + `_contains`
    (membership test); `user_list` repopulates the Users view straight from the
    model, and `user_change`'s cid=0 fan-out gates each pchat on `_contains`.
    Leaves `rcv.c` the only C code still touching `chat->users`.
  - **M4b.4b-iii-B — rcv.c → model; delete `chat->users` (remaining).** rcv.c's
    USER_LIST / USER_CHANGE / USER_LEAVE stop creating/reading `hx_user` in
    `chat->users`: read old state (rename detection, colour preservation) via
    `hx_member_model_get_info`, and emit `user_create`/`_change` with a minimal
    transient `hx_user {uid, nick_color}` (the fan-out reads only those two).
    `options.c`'s self nick-colour writer likewise. Then delete `chat->users` +
    `hx_user_new`/`_delete`/`_with_uid`/`_with_name` + the `hx_user_*`
    accessors (`struct hx_user` shrinks to the transient carrier). **The
    highest-risk slice — rcv.c hot path, no display tests — best validated with
    a Tier 3 join/rename/recolour/leave test against mhxd/Janus first.**

- **M4b.5 — `Conversation` replaces `struct chat`.** With membership,
  history, media, and render-cursors already Rust or view-local, `struct chat`
  is down to `{cid, subject, member_model, view}`. Replace it with a Rust
  `Conversation` (or fold into a `ConversationView`) that the view references,
  retire `struct gtkhx_chat`, and drop the last loose `cid`. Registry becomes
  `Conversations` (`cid → Conversation`), which also drops out cleanly per the
  R7 multi-conn design.

## Invariants preserved throughout

- **Wire compat is untouched** — this is client-side state shape only.
- **The public chat (`cid 0`) always has a model**; private-chat models can
  exist without a view (M4a) and members are fed before the view gate, so any
  model read is safe from the receive path.
- Each increment keeps the `GtkhxSession` signal boundary intact.
