# rcv.c receive-handler inventory (N0)

> Companion to `network-untangling-scope.md`. This is the per-handler map that
> makes N3 (move the dispatch skeleton) and N4 (move the handler bodies) plannable.
> Snapshot of `src/rcv.c` at the time N0 was written; line numbers drift, the
> shape doesn't.

## How the dispatch works today

`hxnet` (Rust) hands each received frame to the C bridge, which stages the bytes
into `htlc->in` and calls **`hx_rcv_hdr`** (rcv.c ~1419). `hx_rcv_hdr`:

1. Parses the 22-byte header (masking `type & 0xffff0000 == HTLS_HDR_TASK` so
   `TASK | <opcode>` composite replies land in the TASK arm).
2. Sets `htlc->rcv` to a **body handler** via a `switch` on the opcode.
3. The body handler consumes the body and resets `htlc->rcv = hx_rcv_hdr`.

Two handler kinds:

- **Primary handlers** — server-initiated frames (chat, user change, banner, …),
  dispatched straight from the `hx_rcv_hdr` switch.
- **Task-reply handlers** — replies to *our* requests. The switch routes
  `HTLS_HDR_TASK` to **`hx_rcv_task`** (rcv.c ~516), which reads the header
  `trans`, does `tsk = task_with_trans(sess, trans)`, checks `task_inerror`, and
  invokes `tsk->rcv(htlc, tsk->ptr, tsk->data)` — the callback registered by the
  matching `task_new` on the send side. This is the correlation N1 moves behind a
  Rust table and N3 moves into the Rust dispatcher.

**The load-bearing insight for N3/N4:** the *wire parse* is already almost
entirely in Rust — nearly every handler body calls a `hotline-proto` extractor
(`gtkhx_proto_*` / `hx_*_extract` / `hx_*_parse`) rather than walking bytes in C.
What remains braided in C is only (a) the `hx_rcv_hdr` router + `htlc->rcv`
state machine, (b) the `hx_rcv_task` `trans`→callback correlation, and (c) a thin
`GtkhxSession::emit_*` shim per handler. So most handlers are already "thin
enough" to move once the skeleton is in Rust.

## Primary handlers (server-initiated; dispatched from `hx_rcv_hdr`)

| Handler | Opcode(s) | Wire parse | Emits (`GtkhxSession`) | Notes |
|---|---|---|---|---|
| `hx_rcv_chat` | `CHAT` | `hx_chat_extract`, `gtkhx_proto_*chat_media_meta` | `chat` | inline-media meta parsed in Rust |
| `hx_rcv_msg` | `MSG`, `MSG_BROADCAST`, `POLITEQUIT` | `hx_msg_extract` | `msg` | one handler, three opcodes |
| `hx_rcv_user_change` | `USER_CHANGE`, `CHAT_USER_CHANGE` | `hx_user_change_extract` | `user_create`, `user_change` | new-vs-changed decided in C |
| `hx_rcv_user_part` | `USER_PART`, `CHAT_USER_PART` | `hx_user_part_extract` | `user_delete` | |
| `hx_rcv_news_post` | `NEWS_POST` | `hx_news_post_walk` | `news_post` | |
| `hx_rcv_chat_subject` | `CHAT_SUBJECT` | `hx_chat_subject_extract` | `chat_subject` | |
| `hx_rcv_chat_invite` | `CHAT_INVITE` | `hx_chat_invite_extract` | `chat_invitation` | |
| `hx_rcv_user_selfinfo` | `USER_SELFINFO` | `hx_selfinfo_parse` | `self_updated` | post-login gate (see N-note) |
| `hx_rcv_agreement_file` | `AGREEMENT` | `hx_agreement_extract` | `agreement` | fires `hx_post_login_fetches` on NONE |
| `hx_rcv_banner` | `BANNER` | `hx_banner_extract` | — (spawns async fetch) | metadata → banner.c worker |
| `hx_rcv_xfer_queue` | `QUEUE` | `hx_xfer_queue_extract` | `xfer_queue` | |
| `hx_rcv_icon_change` | `ICON_CHANGE` | `gfx…icon` (proto) | `gif_icon_changed` | GIF-icons extension |
| `hx_rcv_voice_sdp_offer` | `VOICE_SDP_OFFER` | `gtkhx_proto_parse_voice_*` | (voice model) | Phase 8; feature-gated |
| `hx_rcv_voice_ice` | `VOICE_ICE` | `gtkhx_proto_parse_voice_ice_json` | (voice model) | Phase 8 |
| `hx_rcv_voice_room_status` | `VOICE_ROOM_STATUS` | `gtkhx_proto_parse_voice_participants` | (voice model) | Phase 8 |
| `hx_rcv_dump` | `DUMP` | — | — | debug: writes frame to file |
| `hx_rcv_task` | `TASK` (+ composite) | `gtkhx_proto_header_trans` | — (dispatches) | **the correlator**; see above |

## Task-reply handlers (invoked via `tsk->rcv` from `hx_rcv_task`)

| Handler | Reply to | Wire parse | Emits / effect |
|---|---|---|---|
| `rcv_task_login` | `LOGIN` | header fields | `logged_in`; arms SELFINFO fallback; re-seeds `trans` |
| `rcv_task_user_list` | `USER_GETLIST` | (roster walk) | `user_create` ×N |
| `rcv_task_user_list_switch` | `USER_GETLIST` (room switch) | — | `chat_subject` |
| `rcv_task_news_users` | composite | — | calls `rcv_task_user_list` + `reload_news` |
| `rcv_task_user_info` | `USER_INFO` | (extract uid) | `user_info` |
| `rcv_task_user_open` | user-editor open | `gtkhx_proto_parse_account_read` | user-editor UI |
| `rcv_task_msg` | `MSG` (send ack) | — | placeholder |
| `rcv_task_news_file` | news `FILE_GET` | `hx_news_file_extract` | `news_file` |
| `rcv_task_icon_get` | `ICON_GET` | `gtkhx_proto_parse_icon_get_reply` | `gif_icon_data` |
| `rcv_task_icon_getlist` | `ICON_GETLIST` | `gtkhx_proto_parse_icon_list` | probe result; watchdog |
| `rcv_task_chat_history` | `GET_CHAT_HISTORY` | (proto history parse) | `chat_history_batch` |
| `rcv_task_kick` | `KICK` | — | close chat, kicked signal |
| `rcv_task_file_list` | `FILE_LIST` | `gtkhx_proto_parse_dirlist` | `file_info` ×N |
| `rcv_task_file_getinfo` | `FILE_GETINFO` | (extract) | file-info UI |
| `rcv_task_file_get` | `FILE_GET` | — | spawns xfer; `xfer_queue` |
| `rcv_task_folder_get` | `FOLDER_GET` | — | spawns folder xfer; `xfer_queue` |
| `rcv_task_file_put` | `FILE_PUT` | — | spawns upload; `xfer_queue` |
| `rcv_task_folder_put` | `FOLDER_PUT` | — | spawns folder upload; `xfer_queue` |
| `rcv_task_banner_get` | `BANNER` (data) | — | banner image |
| `rcv_task_voice_join` | `VOICE_START` | `gtkhx_proto_parse_voice_reply/sdp` | voice session (Phase 8) |
| `rcv_task_voice_simple_ack` | `VOICE_STOP`/`KICK` | `gtkhx_proto_voice_reply_field` | voice ack (Phase 8) |

## Grouping for the N4 migration (one branch per family)

The handlers cluster by the model/recv crate that would own their emit shim:

- **chat** — `hx_rcv_chat`, `hx_rcv_chat_subject`, `hx_rcv_chat_invite`,
  `rcv_task_user_list_switch` → chat model/recv.
- **users** — `hx_rcv_user_change`, `hx_rcv_user_part`, `hx_rcv_user_selfinfo`,
  `rcv_task_user_list`, `rcv_task_user_info`, `rcv_task_user_open`,
  `rcv_task_kick`.
- **msg** — `hx_rcv_msg`, `rcv_task_msg`.
- **news** — `hx_rcv_news_post`, `rcv_task_news_file`, `rcv_task_news_users`
  (news receive already has `hxnews-recv`; these fold in).
- **files / xfers** — `rcv_task_file_*`, `rcv_task_folder_*`,
  `hx_rcv_xfer_queue`.
- **banner / icons** — `hx_rcv_banner`, `rcv_task_banner_get`,
  `hx_rcv_icon_change`, `rcv_task_icon_get`, `rcv_task_icon_getlist`.
- **voice** — the four `*voice*` handlers (already feature-gated; parse in Rust).
- **login / lifecycle** — `rcv_task_login`, `hx_rcv_agreement_file`,
  `hx_rcv_user_selfinfo`'s post-login gate, `hx_rcv_dump`.

## State + behaviour to preserve (don't "clean up" during the move)

- **`hx_rcv_task` error suppression:** login/reconnect task errors are toasted,
  not dialogged; other task errors surface normally. Preserve verbatim.
- **`trans` correlation semantics:** `task_with_trans` on a short/zero read must
  behave exactly as today (trans 0 is a *real* key — first frame on a fresh
  connection). N1's table keeps `g_direct_hash` + `GUINT_TO_POINTER(trans)`.
- **Post-login sequencing:** SELFINFO-before-USER_GETLIST with the 2 s fallback
  timer (`rcv_task_login` arms it; `hx_rcv_user_selfinfo` / `hx_rcv_agreement_file`
  fire `hx_post_login_fetches`). This ordering is load-bearing on 1.2 servers.
- **The `htlc->rcv` reset discipline:** every body handler resets to
  `hx_rcv_hdr`; the Rust dispatcher (N3) must reproduce the header→body→header
  cycle exactly, including the composite-TASK mask.
