# Moving the 1.5 news receive path to Rust

Scoping doc for collapsing the C GUI-struct round-trip on the news receive
path. Builds on the N2 news-browser view work (tree / factory / dialogs /
compose / rendering all Rust; `hxnews-model` owns all wire→node construction).

## TL;DR

The wire **parsing** is already Rust. What's left in C is a redundant
**round-trip through GUI structs**: `rcv.c`'s `rcv_task_*` handlers convert the
Rust parse result into C structs (`news_item` / `news_group` / `news_folder` /
`folder_item`), emit a signal carrying them, and the `news_browser.c` handler
marshals those same structs *back* into the Rust tree builders' `#[repr(C)]`
arrays. This plan moves the `rcv_task_*` handlers themselves to Rust — they're
**already registered from Rust** (`hxnews-send` passes them to `task_new`) — so
they parse, carry a Rust-owned handle across the signal, and feed the Rust
builder directly. **No news code is left in `rcv.c`**; the only residual C for
news is the generic trans-ID dispatcher (`hx_rcv_task`) shared by every reply
type, which just calls the now-Rust callback. See "How much of rcv.c can go
away" for the two levels.

Three independent paths — **catalog** (threaded post listing), **dirlist**
(folder/category listing), **thread** (single post body) — each roughly a
commit, moving to a new `hxnews-recv` crate. Recommended order: catalog →
dirlist → thread. Catalog is the meatiest and highest-value.

## Current state

Parsing already lives in `hotline-proto`:

- `hx_newscat_parse` / `hx_newscat_clear` — CATLIST (threaded post listing) →
  `struct hx_newscat { hx_newscat_post *posts; u32 post_count }`.
- `hx_news_dirlist_parse_folderitem` / `hx_news_dirlist_parse_categoryitem` —
  one DIRLIST entry chunk → `struct hx_news_dirlist_entry { kind; name[256];
  name_len }`.
- `gtkhx_proto_parse_news_thread_reply` — GETTHREAD reply → body text +
  `has_text` / `has_task_error` flags.

The C glue in `rcv.c` (worker side) then does this per reply:

```
                rcv.c (worker)                         news_browser.c (main)
                --------------                         ---------------------
CATLIST reply → hx_newscat_parse (Rust)
              → news_item_take_from_wire loop:
                  hx_newscat_post[]  ─►  news_item[]
              → wrap in news_group
              → gcnews->group = group
              → emit "news-catalog"(gcnews) ──────►  handle_catlist(gcnews):
                                                        pending_catlists[gcnews] → target
                                                        marshal news_item[] ─► hx_news_post_data[]
                                                        hx_news_build_category_tree (Rust)
                                                        news_group_free(group)
```

So the data path is **Rust-parse → C-struct → signal → C-marshal → Rust-build**.
The C `news_item[]` / `news_group` intermediate exists only to ferry the parse
result from the worker to the main-thread handler. Same shape for dirlist
(`folder_item[]` / `news_folder`) and, more thinly, thread (`news_post`).

### Signal carriers

The signals carry **opaque pointers** (`ptr_value` GValues in
`gtkhx-session/src/lib.rs`), not boxed GObjects:

| Signal          | Carrier arg        | Also the pending-table key |
|-----------------|--------------------|----------------------------|
| `news-catalog`  | `gnews_catalog *`  | `pending_catlists[gcnews]` |
| `news-folder`   | `gnews_folder *`   | `pending_dirlists[gfnews]` |
| `news-thread`   | `news_post *`      | `pending_threads[post->item]` |

The carrier `gnews_catalog` / `gnews_folder` stub is created in
`fetch_catlist` / `fetch_dirlist` (main thread), stored in the pending table,
and is *also* the task userdata the reply dispatch hands back to `rcv_task_*`.
So the carrier is the shared correlation identity for both the task callback and
the pending table. **This identity must be preserved.** What changes is only the
carrier's *payload* field (`->group` / `->news`), from a C struct to a
Rust-owned handle.

## How much of rcv.c can go away

The `rcv_task_*` handlers are **already registered from Rust**. The 1.5 news
senders live in `hxnews-send`, and they call
`task_new(htlc, Some(rcv_task_newscat_list), gcnews, …)` — externing the
handlers out of `rcv.c` and passing them as the reply callback. Only the
handler *bodies* are still in `rcv.c`.

So there are two levels of "in Rust":

- **Level 1 — news-specific code fully in Rust (this port).** Move the three
  handler bodies (`rcv_task_newscat_list` / `rcv_task_newsfolder_list` /
  `rcv_task_news_post`) out of `rcv.c` into a Rust crate (e.g. `hxnews-recv`),
  and point the `hxnews-send` `task_new` calls at the Rust symbols. The Rust
  handler composes pieces that are **already Rust** — the `hotline-proto`
  parser, the `hxnews-model` builder, the `gtkhx-session` signal emit — with no
  C round-trip. After this, **`rcv.c` contains zero news code.** This is the
  same carrier/signal design as below, but the *producer* side is Rust too, not
  thin C glue.

- **Level 2 — remove rcv.c from the loop entirely (separate, much larger).**
  The frame still reaches the handler through `rcv.c`'s **generic** dispatcher:
  `hx_rcv_task` looks the task up by trans-ID (`task_with_trans`) and calls
  `tsk->rcv(htlc, ptr, data)`. That function-pointer call is shared by *every*
  reply type (chat, users, files, xfers, voice) — it is not news code. News
  flows *through* it but it holds nothing news-specific. Cutting it out means
  letting `hxnet`/Rust own frame + task dispatch for the whole protocol, which
  touches every reply path. Out of scope here; tracked as a future
  "receive-dispatch core → Rust" effort.

**This plan targets Level 1.** The one residual `rcv.c` touchpoint for news is
the generic dispatcher calling a (now-Rust) callback — no news logic, no news
structs, no news glue left in the file.

## Target architecture

The handler moves to Rust (`hxnews-recv`); `hxnews-send` registers it. The
carrier + pending-table logic is unchanged; the payload becomes a Rust-owned
parse handle read on the builder side:

Keep the carrier + pending-table logic exactly as-is. Replace the payload with a
Rust-owned parse handle, and read it from the builder side:

```
             hxnews-send registers the Rust handler:
             task_new(htlc, Some(rcv_task_newscat_list /* Rust */), gcnews)

CATLIST reply → hx_rcv_task (C, generic dispatch) → rcv_task_newscat_list (RUST):
                  parse (hotline-proto, Rust)     → HxNewscat*  (heap, owned)
                  gcnews->parsed = handle
                  emit "news-catalog"(gcnews) ───►  handle_catlist(gcnews):
                                                       pending_catlists[gcnews] → target
                                                       build_category_tree_from_catlist(
                                                           dest, cat_path, gcnews->parsed)
                                                       hx_news_catlist_free(gcnews->parsed)
```

The whole news column is Rust — only the generic `hx_rcv_task` step (shared by
every reply type) is C, and it just calls the registered Rust callback. No
`news_item` / `news_group`, no `news_item_take_from_wire`, no marshal loop.
`hxnews-model` already owns node construction (N2d/N2i); it grows a
`*_from_catlist` / `*_from_dirlist` entry that reads the `hotline-proto` parse
struct instead of a freshly-marshalled `#[repr(C)]` array.

The carrier payload becomes an opaque `void *parsed` (a boxed Rust value), set
by the Rust handler and freed by the (Rust-backed) `handle_catlist`. Neither the
carrier struct nor the signal marshalling ever dereferences it.

### Where the new code lives

- `hxnews-recv` (new crate): the three reply handlers as `#[no_mangle]`
  `rcv_task_*` functions — the C ABI the `hxnews-send` `task_new` calls already
  reference. Each parses (via `hotline-proto`), stashes the owned handle on the
  carrier, and emits the `gtkhx-session` signal. This is the code that leaves
  `rcv.c`. (Could also live inside `hxnews-send` — it already holds the paired
  senders — but a separate `-recv` crate keeps send/receive concerns split and
  cargo-testable in isolation.)
- `hotline-proto`: an *owned-handle* parse entry per path (the current
  `hx_newscat_parse` fills a caller-stack struct that's cleared immediately;
  we need one that returns a heap handle surviving the signal). Likely
  `hx_news_catlist_parse_owned(htlc) -> *mut HxNewscat` + `hx_news_catlist_free`,
  or reuse the existing struct behind a `Box`.
- `hxnews-model`: `hx_news_build_category_tree_from_catlist(dest, cat_path,
  handle)` reading the parse handle, factored to share the threading core with
  the existing `hx_news_build_category_tree` (keep both, or make the array
  variant a thin adapter — the threading logic + tests stay one place).

## Phase A — catalog (`news-catalog`)

1. `hotline-proto`: add an owned-handle catlist parse + free (or box the
   existing `hx_newscat`). Cargo tests for the handle lifecycle.
2. `hxnews-model`: `hx_news_build_category_tree_from_catlist` reading the
   handle; refactor so the threading/two-pass-append core is shared with the
   array builder. Headless tests over the handle.
3. `hxnews-recv`: `rcv_task_newscat_list` becomes a Rust function — parse to the
   owned handle, set `gcnews->parsed`, emit `news-catalog`. Point the
   `hxnews-send` `cat_list` `task_new` at the Rust symbol; **delete the C body +
   `news_item_take_from_wire` from `rcv.c`** (rcv.c now has no catalog code).
4. `news_browser.c`: rewrite `gnews_browser_handle_catlist` — pass
   `gcnews->parsed` to the builder, free the handle. Drop the marshal loop +
   `catlist_thread_into`'s array marshalling for this path.
5. `session.h`: change `gnews_catalog.group` (a `news_group *`) to an opaque
   `parsed` handle. `struct news_group` loses its catalog consumer (see
   elimination below).

## Phase B — dirlist (`news-folder`)

`rcv_task_newsfolder_list` moves to `hxnews-recv` like the catalog handler. It
currently does the DIRLIST chunk-walk in C (`dh_start` loop, calling the
per-chunk Rust parsers) and accumulates `folder_item[]`. Two ways to do the walk
on the Rust side:

- **B1 (smaller):** the Rust handler walks the reply's chunks (the same
  iteration `hotline-proto` parsers already do), calls the existing per-chunk
  parsers, and accumulates into a Rust-owned handle carried on `gfnews->parsed`.
  Minimal new parser surface.
- **B2 (fuller):** add a whole-message DIRLIST parser to `hotline-proto` that
  walks the chunks itself and returns an owned entry list. Slightly larger, but
  a cleaner single entry point.

Then rewrite `gnews_browser_handle_dirlist` to feed `hx_news_build_dirlist_into`
from the handle (drop the `hx_news_dir_item[]` marshal added in N2i), and delete
`struct folder_item` / `struct news_folder`. Either way the C body leaves
`rcv.c`.

Recommendation: **B1 first** (parity, low risk), leave B2 as an optional
follow-up.

## Phase C — thread (`news-thread`)

Thinnest path. `rcv_task_news_post` moves to `hxnews-recv` too; it already
parses the body in Rust, and `news_post` just carries `{ buf, item }`. The
`item` back-pointer is the `pending_threads` key (a **stub** `news_item`, not a
listing one). The only real change is carrying the body as a Rust-owned string
instead of a C `g_malloc` buffer, which is marginal — the value here is finishing
the "no news code in `rcv.c`" goal, not the data-shape win. Lowest value on its
own; do last.

## Struct elimination + the `news_item` send-path entanglement

`struct news_item` has **two** unrelated uses:

1. **Catalog listing** — `news_group.posts[]`, built by `rcv.c`, consumed by
   `handle_catlist`. **Eliminable in Phase A.**
2. **Get-post send stub** — `fetch_thread` (news_browser.c) builds a *single*
   stub `news_item { postid, group{path}, parts[0].mime_type }` purely to feed
   `hx_news15_get_post`, which reads it via `news_send_bridge.c`
   (`news_item_group_path` / `_postid` / `_mime0`). This is a **send** concern
   and the `pending_threads` key.

So after Phases A–C, `struct news_item` still survives for the send stub. Fully
deleting it is a **separate send-path cleanup**: change `hx_news15_get_post` to
take `(path, postid, mime_type)` directly (retiring the three `news_item_*`
accessors in `news_send_bridge.c` and the stub-building in `fetch_thread`).
`news_item.iter` (a `GtkTreeIter`) is already **dead** — the old two-window 1.5
UI that used it was retired — so it can be dropped independently as a freebie.

Elimination order:
- Phase A deletes `news_group` + the listing use of `news_item`.
- Phase B deletes `folder_item` + `news_folder`.
- Phase C (optional) slims `news_post`.
- Send-path cleanup (optional, separate) deletes `struct news_item` outright.

## Threading & lifetime

- Emit points are unchanged — `rcv_task_*` still runs where it does now and
  emits the same signals; only the payload it stuffs into the carrier changes.
- The owned parse handle is allocated in `rcv_task_*` and freed in the
  `news_browser.c` handler — same ownership span the C `news_group` /
  `news_folder` had (allocated in rcv, freed in the handler). No new
  cross-thread sharing: the handle is created, handed off via the carrier, and
  consumed on the main thread, exactly as the C structs are today.
- The handle is opaque (`void *`) at the FFI boundary; only Rust dereferences
  it. It must be `Send`-safe to the degree the current C structs are (they cross
  the same boundary as raw pointers today, so parity holds).

## Testing

- `hotline-proto`: the owned-handle parse entries get cargo tests (parse a
  known CATLIST/DIRLIST buffer → assert handle contents; free without leak —
  run under the existing ASan job).
- `hxnews-model`: the `*_from_catlist` / `*_from_dirlist` builders get headless
  tests mirroring the N2d/N2i ones (build a handle, run the builder, assert the
  tree). The threading core is already covered.
- End-to-end (tree actually populates from a live reply) is **not**
  headless-testable — it wants Tier 3 against mhxd / Badmoon. A Tier 3 test that
  fetches a seeded category and asserts the resulting node tree would be the
  regression guard; see the multi-server test notes.

## Open questions

1. **Owned handle vs. box the existing struct.** Does `hx_newscat` already have
   a heap/handle-returning variant (`hotline_proto.h` hints "caller owns the
   returned handle"), or do we add one? Prefer reusing to avoid a parallel
   parser.
2. **Share the builder core or duplicate.** `hx_news_build_category_tree` (array)
   vs `_from_catlist` (handle): factor the threading + two-pass append into one
   private core both call, so there's a single tested implementation.
3. **B1 vs B2 for dirlist.** Keep the C chunk-walk (B1) or move it to a
   whole-message parser (B2)? B1 for parity now; B2 later if we want the last
   C chunk-walk gone.
4. **How far to push `news_item` deletion.** Stop at the receive port (type
   survives for the send stub), or fold in the `hx_news15_get_post` signature
   change to delete it outright? The latter is a clean but separable follow-up.
