# Untangling the users receive path into Rust — scoping

> Status: **scoping / not started.** A survey + sequenced plan for the roster
> (user-list) receive domain, not a committed design. It builds on the network
> untangling work (`network-untangling-scope.md`, the N-plan), the
> `hotline-proto` crate (R2), the `hxmember-model` crate, and the emit-routing
> slice already landed in the `hxuser-recv` crate. Nothing here re-litigates
> those; it picks up where they left off.

## Why this doc exists

The N4 users piece extracted the cleanly-separable slice — the **emit routing**
(which roster signal fires for a resolved change, and the part membership gate)
— into `hxuser-recv`. That was deliberately the shallow cut: it took the part of
the handler that had no dependency on session/chat/htlc objects.

What's left in `rcv.c` is the genuinely entangled remainder. The two live
broadcast handlers (`hx_rcv_user_change`, `hx_rcv_user_part`) and the bulk
loader (`rcv_task_user_list`) still interleave five concerns that this doc maps
so a future session can cut them apart in the right order:

1. **Wire parse** — already delegated to `hotline-proto`
   (`gtkhx_proto_parse_user_change` / `_user_part` / `_user_list_record`), but
   the C handlers still own the `struct hx_*_msg` marshalling around it.
2. **Change decision** — `hx_user_change_plan_resolve` (pure, Tier-2-tested in
   `tests/proto/test_user_change.c`), still living in `proto_helpers.c` as C.
3. **Member-model reads** — `hx_member_model_get_info` / `_get_ignore` /
   `_contains` / `_upsert` against the per-chat `HxMemberModel` (already a Rust
   crate, `hxmember-model`), reached from C.
4. **`htlc` self-bookkeeping** — the "is this record us?" uid adoption and the
   `htlc->icon/color/nick_color` mirroring. Reads and writes raw `struct
   htlc_conn` fields.
5. **View-facing logging / prefs** — `hx_printf_prefix` join/part/rename lines,
   gated on `gtkhx_prefs.showjoin`.

## Current shape (post-N4)

Handlers in the users domain, from the receive inventory:

| Handler | Opcode | Role |
|---------|--------|------|
| `hx_rcv_user_change` | `HTLS_HDR_USER_CHANGE` | Live join / rename / icon-change broadcast. |
| `hx_rcv_user_part`   | `HTLS_HDR_USER_PART`   | Live part broadcast. |
| `rcv_task_user_list` | `HTLC_HDR_USER_GETLIST` reply | Bulk roster load at join. |
| `rcv_task_user_info` | `HTLC_HDR_USER_GETINFO` reply | User-info window payload. |
| `rcv_task_user_open` | user-editor open | Account editor prefill (admin). |
| `hx_rcv_user_selfinfo` | `HTLS_HDR_USER_SELFINFO` | Post-login self access/uid. |
| `rcv_task_kick` | kick reply | One-line "kick successful" ack. |

`hx_rcv_user_change` is the tangle's centre. It: bails on `task_inerror`;
`hx_user_change_extract`s the wire struct; looks up or creates the chat
(`chat_with_cid` / `chat_new`); snapshots the old member state
(`hx_member_model_get_info`); resolves the plan (`hx_user_change_plan_resolve`);
**adopts self uid** into `htlc->uid` when the plan says so; builds a transient
`struct hx_user carrier` (fan-out reads only `->uid` + `->nick_color`); calls
`hx_user_change_recv` (Rust) to route the emit; then does join/rename logging
keyed on the return, the ignore-gate, and the self `htlc->icon/color/nick_color`
mirroring.

`rcv_task_user_list` is a near-duplicate of the change path's tail: per record
it parses (Rust), computes `new` via `hx_member_model_contains`, does the same
self-adoption, and either emits `user_create` (incremental=FALSE, no chime) or
`hx_member_model_upsert`s silently. This duplication is the strongest argument
for untangling: the join path and the bulk path should share one Rust core.

## The knots, specifically

**K1 — the `struct hx_user carrier`.** A transient stack struct built purely to
carry two fields (`uid`, `nick_color`) across the `user-create/change/delete`
signal boundary, because those signals were shaped around the old `hx_user*`
store. It's dead weight: the emits read nothing else off it. Collapsing the
signal payload to scalar `(uid, nick_color)` args removes the struct entirely
and lets the Rust recv crate build the payload without a `#[repr(C)]` mirror.

**K2 — `hx_user_change_plan_resolve` is C.** The decision is pure and tested,
but it lives in `proto_helpers.c`. Moving it into `hotline-proto` (Rust) would
let the whole parse→decide half of the handler be Rust, driven by the existing
`test_user_change.c` cases re-expressed as crate `#[test]`s.

**K3 — member-model reads cross FFI one call at a time.** `get_info` fills a
`struct hx_member_info` value snapshot; `get_ignore` / `contains` / `upsert` are
individual C calls. Since `hxmember-model` is already Rust, a recv crate that
takes the `HxMemberModel*` handle could call the native Rust API directly rather
than round-tripping the C ABI — but only once the crate boundary is drawn so it
doesn't double-define the model's `#[no_mangle]` symbols (extern the C ABI, per
the hxnews-recv precedent).

**K4 — `htlc` self-bookkeeping.** Both handlers write `htlc->uid` (self
adoption) and `htlc->icon/color/nick_color`. This is the hardest to move: it
needs `struct htlc_conn` field accessor shims (a `users_bridge.c` in the N1
`tasks_bridge.c` mould), and the write-back semantics are subtle (the "don't
copy server name into htlc->name" carve-out for guest-name overrides must
survive). Recommend leaving this in C until last, or keeping it in C
permanently behind a narrow `hx_user_adopt_self(htlc, uid, ...)` shim.

**K5 — chat lookup / create.** `chat_with_cid` / `chat_new` return `struct
chat*` (C, view-side). The recv crate can keep treating `chat` as an opaque
pointer it forwards to the emits (as `hxuser-recv` already does) — no need to
move chat lifecycle. This knot is a non-issue if the crate stays a forwarder
for the chat pointer.

## Sequenced plan (U-series)

Ordered leaf-up so each step is independently shippable with headless tests and
a green Tier-3 `real_connect`. Each is its own `claude/network-untangling-uN-*`
branch, squashed before PR.

- **U1 — collapse the `hx_user` carrier (K1). SHIPPED.** The
  `user-create/change/delete` signals now carry scalar `(uid, nick_color)`
  instead of `struct hx_user*`: the three `param_types`, the three emit wrappers
  (gtkhx-session), the `on_*_signal` bridges + the two `sound_events.c`
  subscribers, the three `users.c` view fns, `hxuser-recv`, and the emit sites in
  `rcv.c` (`hx_rcv_user_change` / `hx_rcv_user_part` / `rcv_task_user_list`) +
  `options.c`. With every construction site gone, `struct hx_user` itself was
  retired. No behaviour change; unit/proto + Tier-3 join/part green. Unblocks
  U2/U3.

- **U2 — move `hx_user_change_plan_resolve` into `hotline-proto` (K2). SHIPPED.**
  The pure decision (self detection, new-vs-change, colour/nick-colour preserve,
  rename-notice flag) now lives in `hotline-proto/src/user_change.rs` as a native
  `resolve()` core plus a `#[no_mangle]` FFI over `#[repr(C)]` mirrors of
  `hx_user_change_msg` / `hx_user_change_plan` (layout pinned by
  `_Static_assert`s in `proto_helpers.c`). The C copy in `proto_helpers.c` is
  deleted; `hx_rcv_user_change` calls the Rust symbol unchanged. The 11 logic
  cases moved to crate `#[test]`s; `test_user_change.c` keeps its parser tests
  plus two thin FFI smoke tests (rename + self-adoption) that drive the real
  C ABI end to end.

- **U3 — unify the change + bulk-load core in `hxuser-recv` (K3).** With U1+U2
  done, fold `rcv_task_user_list`'s per-record tail and `hx_rcv_user_change`'s
  emit tail into one shared Rust routine that takes the member-model handle and
  does the `contains` / `upsert` / emit routing, driven by an `incremental`
  flag. Kills the duplication that is the whole point of this exercise. The self
  `htlc` writes stay behind the K4 shim, called from Rust via `users_bridge.c`.

- **U4 — self-bookkeeping shim (K4), optional / last.** If desired, wrap the
  `htlc->uid/icon/color/nick_color` writes behind
  `hx_user_adopt_self` / `hx_user_mirror_self` in a `users_bridge.c` so the recv
  crate owns the full handler flow. Preserve the htlc->name carve-out. Can be
  deferred indefinitely — leaving self-writes in a thin C tail is acceptable.

## What stays C

- Chat lifecycle (`chat_with_cid` / `chat_new` / `chat_delete`) — view-side,
  forwarded as opaque pointers (K5).
- `hx_printf_prefix` logging + `gtkhx_prefs.showjoin` gating — view/prefs. The
  recv crate returns *what it did* (the N4 pattern) so C does the logging; no
  need to move `hx_printf` across the boundary.
- `rcv_task_user_info` / `rcv_task_user_open` / `rcv_task_kick` — separate
  surfaces (user-info window, account editor, kick ack); out of scope for this
  roster-broadcast untangling, tackle as their own leaves if worthwhile.

## Testing

Every step is covered by (a) `hxuser-recv` / `hotline-proto` crate `#[test]`s
through the recording-double / pure-function pattern already in use, and (b) the
Tier-3 `real_connect` suite against mhxd, which exercises the live join/part/
rename broadcast and the bulk USER_LIST load end-to-end. There is no headless
coverage for the dispatch→signal→view path; the crate tests are the mechanism
for asserting the routing without a live server.
