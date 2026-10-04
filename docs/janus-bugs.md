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

## LZ4 under Blowfish: nothing the client sends is read until it hangs up

**Verified; present in 2.0.13 with `EnableCompression: true`.**

- **Sends:** a HOPE login offering cipher `BLOWFISH` and compression `LZ4`
  (step 1), step 2 echoing both, then, in Blowfish-enciphered LZ4 frames,
  the agreement's answer and a user list request.
- **Gets:** the step-2 reply and everything after it from Janus, LZ4 frames
  under Blowfish that decode cleanly; but Janus acts on none of the client's
  requests. Its log shows "Accept agreement" and "Get user list" only when
  the client closes the connection, all at once.
- **Should get:** the requests answered as they arrive, as under Blowfish
  with GZIP or ZSTD, and under ChaCha20-Poly1305 with LZ4.

Changing the client's framing doesn't help: one LZ4 frame per
transaction or per write, Janus's own frame settings (independent 4 MiB
blocks, a content checksum), the content size in the frame header, and a
skippable frame or 64 KiB of padding after each frame all stall the same
way. The reader under Blowfish appears to wait for the end of the stream
before decompressing.

**GtkHx:** does not offer LZ4 with Blowfish; that login runs uncompressed
(`HopeOpenRequest::session` in `hxnet`).

## A HOPE login racing another user's arrival or departure gets plaintext

**Verified; present in 2.0.13.**

- **Sends:** HOPE step 2 (LOGIN 107, with a cipher agreed: CHACHA20-POLY1305
  or BLOWFISH, with or without compression) while another user logs in,
  agrees or disconnects.
- **Gets:** that user's NotifyChangeUser (301) or NotifyDeleteUser (302) in
  plaintext, after Janus has accepted step 2 and before its login reply,
  which is encrypted. Janus logs "HOPE login successful".
- **Should get:** nothing in plaintext once step 2 is accepted: the
  broadcast either goes through the cipher after the login reply, or is not
  sent to the connection until its cipher is in place. The HOPE spec turns
  encryption on with the step-2 reply and encrypts everything after it, and
  mhxd sets the cipher before it replies.

The connection seems to join the broadcast set before its cipher is on the
writer. The window is tens of microseconds, so a busy server hits it when
people come and go. Nothing arrives between the step-1 reply and step 2.

**GtkHx:** decrypts the plaintext, which fails (ChaCha20-Poly1305: a record
that does not authenticate; Blowfish: a nonsense transaction length), and
the login is lost. No workaround: accepting plaintext where the spec
promises encryption would let anyone on the path inject transactions. The
HOPE integration tests that may land on Janus run alone.

## Adding an entry

Reproduce it against the rig first, ideally as an `hx-e2e` probe, and write it
as what the client sends, what comes back, and what should. When a Janus
release fixes one, delete the entry and point the test that covers it at the
fixed behavior.
