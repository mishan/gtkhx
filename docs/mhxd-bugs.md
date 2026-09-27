# mhxd bugs

mhxd is the rig's reference server (`tests/mhxd/`; source at
[kangsterizer/mhxd](https://github.com/kangsterizer/mhxd)), and the one server
whose code we can read. This is the list of places where it misbehaves, so that
a failing test or an odd report from a real server can be checked against what
is already known before anyone goes looking for a client bug. Each entry says
how it shows up, why (with the function in mhxd's `src/hxd/`), and what GtkHx
does about it.

The Janus list is [janus-bugs.md](janus-bugs.md). These were found while
building the Files end-to-end suite (`rust/crates/hx-e2e`). An
entry marked *verified* was reproduced against the rig's container; *from the
source* means read from the code and not yet reproduced.

## It wedges under concurrent logins

**Verified; cause not found.**

With several clients logging in at once (the C integration tests that open
two connections each, run in parallel), mhxd occasionally stops serving:
connections are accepted and get no LOGIN reply. The rig's image watches for
it (the entrypoint's health probe logs "mhxd wedged — terminating so the
container can restart") and restarts the server after three failed probes,
about 45 seconds, and every test that runs in that window fails. It isn't the
per-address connection limits: `nospam` is off in the rig's `hxd.conf`, so
`conn_max` and `reconn_time` are never checked. Sampling the process with gdb
every two seconds made it stop happening, which points at a timing race.

**GtkHx:** nothing to do on the client side; a test that fails with "timed out
waiting for SELFINFO" or "no LOGIN reply" against mhxd is most likely this.
Restart the container if the watchdog hasn't.

## Rename and move silently replace what is already there

**Verified for an empty folder; from the source for files.**

FILE_SETINFO's rename and FILE_MOVE both end in a bare `rename(2)`
(`rcv_file_setinfo`, `rcv_file_move` in `files.c`) with no check that the
destination is free. `rename(2)` replaces an existing file, or an existing empty
directory, without complaint. So renaming `a` to `b`, or moving `a` into a
folder that already holds a `b`, destroys `b` and reports success. Only a
non-empty destination folder is refused, with "Directory not empty".

The HFS sidecars (resource fork, Finder info) are renamed after the data fork,
also over whatever is there, so a replaced file can end up with the old file's
resource fork if the new one had none.

**GtkHx:** nothing guards against it yet. The browser's rename, move and
drag-and-drop dialogs should refuse a name the destination listing already
holds, which belongs with the Files view port (step 3 in
`docs/rust/ROADMAP.md`).

## A name with `/` in it acts on the parent folder

**Verified.**

`read_filename` rejects a name containing `/` (or exactly `..`) by writing an
empty string, which does stop path traversal. But its callers keep the length
they read off the wire, and never check for the empty result, so the request
carries on with an empty name, and the path it builds is the folder itself:

- **FILE_MOVE** with the item's name containing `/` moves the *containing
  folder* instead. With an empty destination folder, `rename(2)` replaces the
  destination with it: moving `x/y` from `/p` to `/q` left the root with only
  `q`, holding everything `p` held.
- **FILE_SETINFO** with a new name containing `/` tries to rename the item onto
  its own folder, and fails with a misleading "Directory not empty".

A name holding `/` is legitimate Hotline: Classic Mac names allow it, and the
wire carries names as their own chunks for exactly that reason. mhxd, storing
names on a Unix file system, can't hold one, and should refuse such a request
with a clear error instead of acting on a different path.

**GtkHx:** can't reach the move case from the browser today, since a remote
listing from mhxd never contains a `/` name. The rename dialog accepts any
text, so typing `/` into a new name gets the misleading error.

## An unclaimed transfer holds a global slot until mhxd restarts

**Verified.**

A download or upload request that mhxd grants reserves one of its global
transfer slots (`total_downloads` / `total_uploads`, 20 each in the rig) and
hands back a reference for the client to claim on the transfer port. If the
client never claims it, nothing ever gives the slot back: there is no expiry,
and when the client disconnects `htlc_close` (`hlserver.c`) releases the global
count only for a transfer whose thread had started. The same happens when the
claim arrives but mhxd handles the control connection's close first. After 20
such, every transfer on the server is refused until it restarts, with
"maximum number of total downloads reached".

That is exactly what a client does when a transfer is cancelled before its
connection opens, or when it crashes in between. It is also what the rig's
tests did — each run of the C `folder_put` test and of the Rust folder-transfer
test leaked a slot, which is part of why a long-lived rig "accumulates state".
Both now claim the reference and wait for mhxd to close its end before
disconnecting (`Client::cancel_transfer` in `hx-e2e`).

**GtkHx:** cancelling a transfer that hasn't connected yet leaks a slot on an
mhxd server. The client could claim the reference and hang up on cancel, as the
tests do.

## The upload-limit error names the download count

**Verified.** "maximum number of total uploads reached (1 >= 20)": the
message prints `nr_gets` where it means `nr_puts` (`rcv_file_put`,
`rcv_folder_put`), so the number shown is the downloads in progress, not the
uploads that hit the limit.

## Forked and cloned transfer builds release the wrong slot

**From the source; not the rig's build.** When mhxd is built to run
transfers as processes (`CONFIG_HTXF_FORK` / `CONFIG_HTXF_CLONE`),
`hlserver_reap_pid` passes the wrong direction to `free_htxf`: a finished
download releases the upload slot of the same index, and an upload the
download slot. `free_htxf` also returns with its mutex still held when that
slot is empty. A default Linux build uses threads instead, as the rig's
appears to, so none of this shows up in tests.

## Folder Get Info reports a byte size

**Verified.** A quirk rather than a bug.

A folder's FILE_LIST entry carries its child count in the size field, as the
Hotline convention has it, but FILE_GETINFO on the same folder reports the
directory's `st_size` (`rcv_file_getinfo`): 4096 on ext4, whatever the
contents. GtkHx's Get Info shows it as a size; it means nothing to the user.

## Kill Download is rejected

**Verified.** Kill Download (214, `HTLC_HDR_KILLDOWNLOAD`) is an official
1.8.2 transaction for dropping a queued download, and mhxd defines it, but
its dispatch case in `rcv.c` has the handler assignment commented out. The
request gets "Transaction rejected. (Unknown or non-authorised)". GtkHx never
sends it, so nothing depends on it; a client that cancels a queued download on
mhxd has to hang up the transfer instead.

## A folder-upload resume reply carries garbage resume data

**From the source.** When a folder upload asks to resume (a non-zero transfer
option on FILE_PUTFOLDER), `rcv_folder_put` answers with a 74-byte RFLT taken
from a stack buffer it never fills, so the client is told arbitrary offsets.
GtkHx never asks to resume a folder upload, so it never sees this.

## hxtrackd lists a restarted server twice

**Verified.** hxtrackd identifies a registration by its UDP source address and
**port**, and ignores the server's pass ID (`htrk_udp_rcv` in
mhxd's `src/hxtrackd/tracker.c`). mhxd sends from an ephemeral port, so after a
server restarts its new registrations make a second entry beside the old one
until that expires. The rig's listing routinely shows Janus twice. GtkHx lists
what it is sent.

## hxtrackd expires entries whether or not they keep registering

**From the source.** An entry's age counter (`clock`) is incremented by
`tracker_timer` every interval and the entry is dropped at 2, but nothing
resets the counter when a heartbeat arrives; the only reset is in dead code.
So every server falls out of the listing two or three intervals after it
**first** registered, and comes back with its next heartbeat. The rig sets the
interval to a day, which hides it.

## Adding an entry

Reproduce it with an `hx-e2e` probe against the rig first, then record it here
with how it shows up, the mhxd function responsible, and what GtkHx does about
it. When the rig's pinned mhxd picks up a fix, delete the entry and point the
end-to-end test that covers it at the fixed behavior.
