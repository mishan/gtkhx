# Janus bugs

Janus is VesperNet's server and the rig's target for the fogWraith
extensions (`tests/janus/`, Janus 2.0.23). Its source is closed, so this is
the list to send upstream: each entry says what a client sends, what it gets
back, and what it should get. It also lets a failing test or an odd report be
checked against what is already known before anyone goes looking for a client
bug. The companion list for the other reference server is
[mhxd-bugs.md](mhxd-bugs.md).

An entry marked *verified* was reproduced against the rig's container.

## LZ4 under Blowfish: nothing the client sends is read until it hangs up

**Verified; present in 2.0.13 and 2.0.18 with `EnableCompression: true`.**

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

## Fixed upstream

Older servers still behave this way, which is worth knowing when a report
comes from one.

- **A message to no one, and a listing of a threaded-news category that is
  not there, went unanswered** through 2.0.18: no reply on the transaction.
  2.0.23 refuses both with a reason, as mhxd and hlservd do. GtkHx shows
  nothing for the silence, and the session forgets the reply it expected
  once enough newer requests wait.

## Adding an entry

Reproduce it against the rig first, ideally as an `hx-e2e` probe, and write it
as what the client sends, what comes back, and what should. When a Janus
release fixes one, delete the entry and point the test that covers it at the
fixed behavior.
