# Janus bugs

Janus is VesperNet's server and the rig's target for the fogWraith
extensions (`tests/janus/`, Janus 2.0.13). Its source is closed, so this is
the list to send upstream: each entry says what a client sends, what it gets
back, and what it should get. It also lets a failing test or an odd report be
checked against what is already known before anyone goes looking for a client
bug. The companion list for the other reference server is
[mhxd-bugs.md](mhxd-bugs.md).

An entry marked *verified* was reproduced against the rig's container.

## A rename to the item's own name reports an error

**Verified; still present in 2.0.13.**

- **Sends:** FILE_SETINFO (207) with FILE_NAME `f`, FILE_RENAME `f` (the same
  name), FILE_COMMENT `note`, and the folder's DIR.
- **Gets:** a task error, "Error renaming folder." The comment *is* saved.
- **Should get:** success; the unchanged name is a no-op, as on mhxd.

Hotline clients have long sent the current name back in FILE_RENAME when only
the comment changed, so saving a comment from a client's Get Info reports a
failure that didn't happen.

**GtkHx:** leaves FILE_RENAME out when the name hasn't changed
(`hxrequest::files::set_info`).

## A name of 253 bytes or more panics the handler, and nothing replies

**Verified; still present in 2.0.13.**

- **Sends:** FILE_MKDIR (205) for `/` + a 253-byte name (one DIR component of
  253 bytes). FILE_DELETE naming such an item does the same.
- **Gets:** no reply at all. Janus logs `PANIC`, "runtime error: slice bounds
  out of range [3:0]", in `hotline.(*FilePathItem).Write` (`file_path.go:41`)
  under `HandleNewFolder`, and recovers.
- **Should get:** the folder, or a task error if the name is too long.

It is the length of one path component that matters: names up to 252 bytes
work, at any depth. A DIR component carries a name of up to 255 bytes, and
mhxd takes the full length. The client waits out its timeout, and a GtkHx user
sees a request that never finishes.

**GtkHx:** the end-to-end suite runs the 255-byte name test only where
`Cap::LongNames` says it's safe.

## An unknown opcode gets no reply at all

**Verified; still present in 2.0.13.** A transaction type Janus doesn't know is
dropped silently, with no task error back. A client that tries an optional
request to find out whether the server supports it (GtkHx's GIF icon list, for
one) can't tell "unsupported" from "slow" and has to wait out a timeout. A task
error is the expected answer, as other servers give.

**GtkHx:** probes on a timeout ([gif-icons.md](gif-icons.md)).

## A voice participant whose audio arrives after others joined is never offered to them

**Verified; still present in 2.0.13.**

- **Sends:** two clients VOICE_JOIN (600) the same room within a few
  milliseconds, answer their offers, and unmute.
- **Gets:** the first joiner (A) is sent a renegotiation offer with a
  `mid:user-<B>` section the moment B joins, before B has any media. B's
  initial offer has only `mid:send`, because A's track hadn't arrived yet
  either — and when A's track does arrive (`OnTrack`, a few tens of
  milliseconds later), B is never sent a renegotiation adding A. A hears B;
  B never hears A, for as long as both stay in the room.
- **Should get:** an offer to B adding `mid:user-<A>` once A's track arrives —
  or, since Janus already adds a section to others at join time for a
  participant with no track yet, the same for a newcomer's initial offer.
  hxd-ng offers both directions.

The window is from a participant's join until their ICE and DTLS complete,
which is well under a second on the rig but can be longer on a real network,
so two people joining together — or anyone joining just after someone else
clicks Join — can hit it. Leaving and rejoining recovers, since the new
initial offer includes everyone with a track by then.

**GtkHx:** nothing to do on the client, which the spec gives no way to ask for
a fresh offer. `/integration/voice/simultaneous_join` covers it against
hxd-ng; `GTKHX_VOICE_TEST_PORT=5510` points it at Janus.

## A `0s` duration in the config means the default, not zero

**Verified before 2.0.13; not re-checked since.** The YAML decoder treats a
duration of `0s` as unset and applies the default, so writing `0s` to turn off
a rate limit leaves the default limit in force (10 s per account for inline
media uploads). A setting meant to disable something has to be a tiny non-zero
value (`1ms`) instead ([inline-media.md](inline-media.md)).

## Adding an entry

Reproduce it against the rig first, ideally as an `hx-e2e` probe, and write it
as what the client sends, what comes back, and what should. When a Janus
release fixes one, delete the entry and point the test that covers it at the
fixed behavior.
