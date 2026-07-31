# GIF Icons

GtkHx's implementation of the fogWraith **GIF Icons** extension to the
Hotline protocol (`GIF-Icons.md`). The spec is not vendored in this
tree, so the wire details below are the in-repo record.

## What the extension does

Standard Hotline gives each user a 16-bit icon ID (`DATA_ICON` /
`0x0068`) indexing a built-in sprite. This extension supplements that
with a per-user **custom GIF avatar** that other clients download and
display.

The design is **pull-based**: the server stores GIF data per session but
never pushes the bytes. When a user sets or clears their icon, the
server broadcasts a lightweight **Icon Change** notification carrying
*only* the user's ID; clients that care re-fetch on demand. That caps
bandwidth — a busy room doesn't flood everyone with avatar blobs on
every join.

The GIF avatar is **independent of** the 16-bit icon; both travel on the
wire and a capable client decides at the presentation layer. Icons are
per-session and not persisted across reconnects (server-dependent).
Servers impose a size cap (the spec suggests 32 KiB) and validate the
`GIF87a` / `GIF89a` signature.

## Relationship to the legacy cicn icons

Custom per-user icons are **one feature with two payload encodings on
the same transactions**, not two features. The transactions are shared;
legacy carried a Mac **cicn** resource in field `0x0e90`
(`HTLS_DATA_ICON_CICN`), the fogWraith form carries a **GIF** in field
`0x0300` plus the packed list entry `0x0301`.

**Legacy cicn-over-wire is vestigial.** No reachable server serves a
cicn payload — verified by probing both: setting an icon via field
`0x0e90` and reading it back, **mhxd discards it** (returns the
previously-set GIF) and **Janus discards it** (returns no icon data).
mhxd's *header* defines `HTLS_DATA_ICON_CICN` but no `.c` file ever
reads or writes it; its handlers are GIF-only. Even the sibling `ghx`
client never fetched peer cicn icons. GtkHx therefore implements the GIF
payload only; the `0x0e90` slot stays reserved and unused.

`src/hotline.h` once had `HTLC_HDR_ICON_GET` defined as `0x0e90` —
that's the cicn *data field* number, not the opcode. The real icon-get
opcode is `0x0747`. The constant was dormant (only the protocol tracer
referenced it), so the bug never fired; it is corrected in place with a
note, and worth knowing about because the same confusion is easy to
reintroduce from a stale reference.

## Wire protocol

### Transactions (1861–1864 / `0x0745`–`0x0748`)

| Dec | Hex | Name | Direction | C constant |
|---|---|---|---|---|
| 1861 | `0x0745` | Get Icon List | C→S | `HTLC_HDR_ICON_GETLIST` |
| 1862 | `0x0746` | Set Icon | C→S | `HTLC_HDR_ICON_SET` |
| 1863 | `0x0747` | Get Icon | C→S | `HTLC_HDR_ICON_GET` |
| 1864 | `0x0748` | Icon Change | S→C | `HTLS_HDR_ICON_CHANGE` |

Constant names match mhxd's (`mhxd/src/common/hotline.h`) so the two
codebases cross-read cleanly.

### Fields

| Dec | Hex | Name | C constant |
|---|---|---|---|
| 768 | `0x0300` | GIF Icon Data | `HTLC`/`HTLS_DATA_ICON_GIF` |
| 769 | `0x0301` | Icon List Entry | `HTLS_DATA_ICON_LIST` |
| 103 | `0x0067` | User ID | `HTLC`/`HTLS_DATA_UID` |

> **Numeric coincidence, not a collision.** `0x0300` / `0x0301` also
> appear in `hotline.h` as `HTRK_V3_TLV_PROTOCOL_VERSION` /
> `HTRK_V3_TLV_SUPPORTS_HOPE` — but those are **tracker-v3 TLV** field
> IDs, a separate namespace from Hotline transaction `DATA_` fields. No
> macro clash as long as the new constants keep the `HTLC_DATA_` /
> `HTLS_DATA_` prefix. Do not "deduplicate" them.

### Packed Icon List Entry (`0x0301`)

The Get Icon List reply carries one `0x0301` field per user that has an
icon:

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 2 | u16 BE | User ID |
| 2 | 2 | u16 BE | GIF data length |
| 4 | N | bytes | Raw GIF |

The 2-byte length caps a single entry's GIF at 64 KiB on the wire,
comfortably above the 32 KiB upload recommendation.

### Per-transaction field usage

- **Get Icon List (1861)** — request: no fields. Reply: 0..N × `0x0301`.
- **Set Icon (1862)** — request: `0x0300` (raw GIF; **an empty payload
  clears**). Reply: bare task completion, no fields. The server then
  broadcasts 1864.
- **Get Icon (1863)** — request: `0x0067` (target UID). Reply: `0x0067`
  + `0x0300`.
- **Icon Change (1864)** — broadcast: `0x0067` (changed UID) only. No
  GIF data.

## Negotiation: probe and fallback

**The spec defines no `DATA_CAPABILITIES` bit for this extension**, and
inventing a speculative one would be guessing at fogWraith's future
allocation. Support is discovered by probing.

**The probe must be timeout-based, not error-based.** An unsupported
server does not return a task error for an unknown opcode — Janus
*silently drops* it with no reply at all (verified with a bogus opcode:
zero response). So the probe cannot rely on a non-zero error code; it
uses a watchdog, the same shape the tracker-v3 probe already uses.

As built (`src/gif_icons.c`):

1. After login, fire Get Icon List once, recording the transaction ID it
   will be keyed on (`task_new` snapshots the counter before
   `hlwrite_chunks` increments it).
2. Arm a 2-second watchdog.
3. A reply marks the session capable — ingest the entries, enable the
   send path, and auto-send the user's saved avatar if there is one.
4. The watchdog firing marks the session unsupported, stays silent, and
   dismisses the probe's row from the Tasks window (a legacy server
   never replies, so the row would otherwise sit there forever). Only
   the UI row is removed, not the model task — a merely *slow* server's
   late reply still dispatches normally and loads avatars.

Worst case against a legacy server: one ignored transaction and a
two-second timer.

## Receive, cache, and render

Wire parsing lives entirely in `hotline-proto::gif_icons` — whole-message
walkers for all four transactions plus the packed-entry unpacker, with
bounds-checked rejection of truncated entries, and a `GIF87a`/`GIF89a`
signature validator. The C receive handlers hand the raw input buffer
straight in and act only on the typed result; they do no chunk walking.
They reach the view through two `GtkhxSession` signals:
`gif-icon-changed (htlc, uid)` for the broadcast (which triggers a
re-fetch) and `gif-icon-data (htlc, uid, gif, len)` for fetched bytes.

`src/gif_avatar.c` owns the per-UID cache. **Decoding goes through the
shared bounded, sandboxed image decoder** (see
[image decoding](image-decoding.md)) under the strict JPEG/PNG/GIF
policy, with avatar-specific caps: 64 KiB input, 256 px per axis, up to
256 frames and 30 s of cumulative animation. At most one decode per UID
is in flight; a newer update for the same UID cancels the previous one.
The cache is cleared on disconnect, since icons are session-scoped
server-side and a fresh connection re-probes from scratch.

Avatars route through the **same** user-list cell path as cicn sprites —
intrinsic pixels × theme scale, with the wide-banner left-shift for
banner-width art — so a GIF authored at classic icon or banner
dimensions renders identically. A GIF avatar wins over the 16-bit icon;
the cicn sprite is the fallback. `src/chat_avatar.c` applies the same
precedence for the chat gutter, so chat and the user list never disagree
about which icon a user "has".

### Animation

Animated avatars are decoded to all their frames and played by **a
single shared frame timer** — the whole cache advances on one 60 ms
tick, and `gtkhx_avatar_get(uid)` simply returns whichever frame is
current, so a cell needs no animation state of its own. The timer runs
only while at least one animated, unpaused avatar exists and the global
preference is on.

Three controls:

- **"Animate GIF avatars"** (`CFG_ANIMATE_AVATARS`, default on) on
  Settings → Identity. Off renders every avatar as its still first
  frame.
- **Per-user pause** by clicking an animated avatar in the user list — a
  cell gesture that claims the press only on the icon column of an
  animated avatar, so selection still works everywhere else.
- **"Pause/Resume Animation"** in the right-click menu, shown only for
  animated avatars, as the discoverable equivalent.

Resuming restarts the frame clock from now rather than from a timestamp
left over from before the pause, so a long-paused avatar doesn't jump.

## Send

Settings → Identity has a **Custom GIF avatar** group: a preview, a
Choose… button (GIF-filtered file dialog) and Clear. Choosing validates
the GIF signature and a 32 KiB cap.

**Downscale is not offered** because gdk-pixbuf has no GIF *encoder* —
recompressing an oversize GIF back to GIF would need ImageMagick. An
oversize file is rejected with an actionable message instead. (The
Settings *preview* decode is separately bounded: a `size-prepared` hook
scales anything over 512 px down before the raster decode, so a
highly-compressed GIF advertising huge dimensions can't allocate a giant
canvas on the UI thread.)

**The choice is decoupled from capability.** Rather than gate the picker
on the live server, the avatar is persisted to `$CONFIG/avatar.gif` the
moment it's chosen — even offline, even on a non-supporting server. It
is sent immediately if the current server is capable, and otherwise
**sent automatically on the next connect to a capable server**, from the
post-login probe once support is confirmed. Clear forgets the saved file
and, if connected and capable, sends a clear. The preview seeds from the
saved file so it shows before connecting.

An in-memory copy backs both the preview and the auto-send, so the
receive path never touches the disk after the first load, and it only
ever holds bytes that already passed signature and size validation. A
corrupt, oversize, or non-GIF file on disk is treated as "no avatar".

## Janus verification

Probed the live Janus container (guest, empty password) on 2026-06-28.
All four transactions behave per spec:

- **Get Icon List (1861)** → `err=0`; empty list when nobody has an
  icon, and the expected `0x0301` entry after a set.
- **Set Icon (1862)** with a real `GIF89a` → `err=0`.
- **Get Icon (1863)** for own uid → `err=0`, reply carries field `103` +
  field `0x0300`; the GIF round-tripped byte-for-byte.
- **Icon Change (1864)** → with two clients connected, client A's Set
  Icon triggered a `type=1864` broadcast to client B carrying only field
  `103` with A's uid. Exactly the pull-based design.
- **Control:** a bogus opcode (9999) got **no reply at all**, which is
  what forces the timeout-based probe.

So Janus is a ready integration target; no mock server was needed.

A harness gap surfaced during this work and was fixed: **Janus delivers
the session UID in the LOGIN task reply** (alongside NAME and
CAPABILITIES), **not in SELFINFO**, so the shared login helper was
leaving `htlc->uid == 0` on Janus. The harness now stashes the
login-reply uid and preserves it across the SELFINFO parse — which
benefits any test needing the uid on that server.

## Testing

- **Wire fixtures** in `hotline-proto` cover the four builders, the
  `0x0301` entry walker (including truncated and oversized rejection),
  and the GIF signature validator.
- **Integration** (`tests/integration/test_gif_icons.c`) covers the
  set→get round-trip, getlist, the 1864 broadcast to a second client,
  and clear — green against both mhxd and Janus.

## Open

- **No unit coverage for the cache and pause behaviour** — per-UID
  invalidation, the still-vs-animated selection under the pause
  preference, and the shared timer's start/stop bookkeeping are
  currently only exercised by running the app.
- **The probe-timeout watchdog has no negative-server test.** It needs a
  server that ignores the opcode, and mhxd can no longer play that role
  — it implements the extension. Some other non-supporting target, or a
  deliberately silent mock, is needed to pin the fallback path.
- **Where else the avatar should appear.** The user list and the chat
  gutter render it today; private-message windows do not.
- **Re-fetch storms.** The 1864 broadcast carries no content hash, so a
  strict client re-fetches on every change. Fine at avatar sizes; a
  short per-UID debounce is the mitigation if it ever bites. Keeping the
  re-fetch event-driven (rather than polling) also sidesteps the spec's
  note that Get Icon should not reset the server's idle timer.
