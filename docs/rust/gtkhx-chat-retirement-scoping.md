# Scoping: retiring / shrinking `struct gtkhx_chat`

**Status:** scoping / decision doc. Written after the chat-model re-think (M1–M4b.5)
landed — `struct chat` is now the Rust `HxConversation`, and the chat *content*
(tab strip, input handler, invitation dialog) has moved to gtkhx-ui. The last
big C aggregate in the chat area is `struct gtkhx_chat`, the per-conversation
**view**. The M4b.5 plan called this the "UI half"; this doc figures out what
"retire" can actually mean.

## TL;DR

**Full retirement of `struct gtkhx_chat` is not achievable while xtext stays C**
(and xtext stays C forever — vendored, out of scope). The struct's `render`
sub-object holds **raw `textentry*` pointers into xtext's internal entry list**;
those can't cross into Rust and are inherently a C concern.

So the honest, valuable goal is **make `struct gtkhx_chat` opaque** — its
definition private to `chat.c`, every other file reaching it through `hx_gchat_*`
accessors — exactly as `struct chat` became an opaque handle over `HxConversation`.
That completes the model/view boundary and leaves a clean, thin C **view leaf**
(GTK widgets + xtext render state) that is a *permanent seam*, not a TODO.

An optional later step can thin the leaf further by relocating its three
Rust-owned handles (`chat_history`, `media_table`, `userlist`) out of the C
struct and into the Rust `HxConversation`.

## What `struct gtkhx_chat` holds today (`session.h`)

| Field                 | Nature                        | Can leave C?                                   |
|-----------------------|-------------------------------|------------------------------------------------|
| `window`              | GTK widget (panel content box)| No — GTK widget, C-built                       |
| `vscroll`             | GTK widget                    | No                                             |
| `output`              | **xtext** widget              | **No — xtext is C forever**                    |
| `input`               | GtkTextView                   | No (handler already Rust; widget C-built)      |
| `subject`             | GtkEntry                      | No                                             |
| `voice_panel`         | GTK widget (feature-gated)    | No                                             |
| `media_attach_btn`    | GTK widget                    | No                                             |
| `userlist`            | `HxUserListView` (Rust GObject, opaque) | Handle could move to `HxConversation`|
| `chat_history`        | Rust `InputHistory` (opaque)  | Handle could move to `HxConversation`          |
| `media_table`         | Rust `MediaTable` (opaque)    | Handle could move to `HxConversation`          |
| `cid`                 | `guint32`                     | Already on `HxConversation` — this is a dup    |
| `render`              | `hx_chat_history_render` — **raw xtext `textentry*` cursors** | **No — xtext-internal, C only** |

The `render` field is the blocker. Its `anchor_ent` / `load_older_ent` are raw
pointers into xtext's entry list that xtext's `max_lines` pruning can silently
free; xtext.c even has a defensive membership walk documented against exactly
this coupling (`xtext.c` ~L5822, comment). This is fundamentally C-side state.

## Who reaches into the struct

- **`chat.c`** — the owner. Touches every field (builders, the history renderer,
  input, media). Full field access by design; stays.
- **External consumers** (must route through accessors before the struct can go
  opaque) — four files with real code access:
  - `options.c` — `output` / `input` / `subject` (font + theme apply, iterating
    the chat registry).
  - `users.c` — `userlist` (the pchat sidebar view).
  - `inline_media_attach.c` — `cid`, `media_attach_btn`.
  - `notify.c` — `window` (omit-if-focused check).
- `gtkutil.c`, `xtext.c` — mention `gchat->window` / `gchat->render` **in
  comments only**; no code access.
- `msg.c` — **not** a consumer (it uses its own `struct msgwin`).

Existing accessors (`chat.h`): `hx_gchat_output` / `_vscroll` / `_input` /
`_subject` / `_media_btn`, plus `hx_gchat_set_window`. Missing (added by
Phase 1): `hx_gchat_window` (getter), `hx_gchat_cid`, `hx_gchat_userlist`
(`_media_btn` already covers `media_attach_btn`).

> **Status: Phases 1–2 shipped** (`claude/gtkhx-chat-opaque`). `struct gtkhx_chat`
> and `struct hx_chat_history_render` now live privately in `chat.c`; `session.h`
> holds only a forward declaration; the four consumers route through accessors.
> The opaque type immediately caught `notify.c` (which this doc originally
> missed) at compile time. Phase 3 remains optional.

## Plan

### Phase 1 — accessor-clean the external consumers *(small, low-risk)*
Add the missing `hx_gchat_*` accessors and route `options.c`, `users.c`,
`inline_media_attach.c`, `notify.c` through them. No behaviour change; pure
seam work. This is the same move that preceded making `struct chat` opaque.

### Phase 2 — make `struct gtkhx_chat` opaque *(the headline)*
Move the full struct definition out of `session.h` into `chat.c` (a private C
struct), leaving `struct gtkhx_chat;` (incomplete type) in the header. The
compiler then flags any stray external field access as an error — the same
checklist trick that made `struct chat` opaque. After this, the view struct is
behind a clean boundary: `chat.c` owns it, everyone else uses accessors. **This
is the natural stopping point / the achievable "retirement."**

Note: `struct hx_chat_history_render` (embedded in `gtkhx_chat`) currently also
lives in `session.h`; it moves to `chat.c` with the parent struct. Its forward
`typedef struct textentry textentry;` stays where xtext needs it.

### Phase 3 — thin the leaf *(optional, later)*
Relocate the three Rust-owned handles from the C struct into the Rust
`HxConversation` (which already owns the membership model and view pointer):
- `chat_history` (`InputHistory`) and `media_table` (`MediaTable`) — the C view
  only stores + frees them; `HxConversation` is the more natural owner, reached
  from C via `hx_chat_*` accessors on the model.
- `userlist` (`HxUserListView`) — similar.
- Drop the duplicated `cid` (it's already `hx_chat_cid` on the model; the view
  can derive it).

After Phase 3 the C `gtkhx_chat` shrinks to **pure GTK widget handles + the
xtext render cursors** — the irreducible C view leaf. This phase is where the
struct genuinely thins, but it's lower value than Phases 1–2 and can wait.

## What stays C — permanently, by design

- The xtext `output` widget and everything that renders into it
  (`output_chat_from_event`, `xprintline`, the history batch renderer).
- The `render` cursors — raw `textentry*` into xtext internals.
- The GTK widget handles (`window` / `vscroll` / `input` / `subject` /
  `media_attach_btn` / `voice_panel`) and the leaf builders
  (`gtkhx_chat_build_leaves`, `gtkhx_pchat_new`).

These are the leaf-up boundary the ROADMAP's "permanent seams" note describes,
not churn to be removed.

## Recommendation

Do Phases 1–2 as one increment (accessor-clean + opaque). It's bounded, mirrors
the `struct chat` opaque-ification, and finishes the model/view separation with
a clear, honest boundary. Treat Phase 3 as a separate, optional follow-up and
**do not** frame the overall effort as "delete `struct gtkhx_chat`" — the xtext
coupling makes that impossible without porting xtext, which we won't.
