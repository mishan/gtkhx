# hlservd bugs

hlservd is the Hotline Server 1.9.5 as a headless POSIX daemon, ported from
the Hotsprings 2003 GPL release by the author of Underline Hotline, and the
rig's stand-in for the original 1.9 server (`tests/hlservd/`, build
`1.9.5-d40d2b75`). Its source isn't published yet, so like
[janus-bugs.md](janus-bugs.md) this is written to send upstream: what the
client sends, what comes back, and what should. The other reference
servers' lists are [mhxd-bugs.md](mhxd-bugs.md) and
[janus-bugs.md](janus-bugs.md).

An entry marked *verified* was reproduced against the rig's container.

## MKDIR errors leave the folder's name out

**Verified.**

- **Sends:** FILE_MKDIR (205) for a folder that already exists, or one
  whose parent is missing.
- **Gets:** "Cannot create folder «» because there is already a file or
  folder with that name." (or "…because the enclosing folder could not be
  found."), with nothing between the quotes.
- **Should get:** the folder's name between them, as the delete error does:
  "Cannot delete «nope» because it does not exist or cannot be found."

## A folder upload's partial file is taken again from its start

**Verified.**

- **Sends:** FILE_PUTFOLDER (213) with the resume flag (204) for a folder
  an earlier upload left a partial file in, then that file's item.
- **Gets:** SEND (1), and the whole file again, over the partial `.hpf`.
- **Should get:** RESUME (2) with an RFLT of the partial file, as a resumed
  file upload (203 with 204) already gets.

The result is right, only slower: the file arrives whole.

## A download leaves the file's comment out

**Verified.**

- **Sends:** FILE_SETINFO (207) giving `test.txt` a comment, then FILE_GET
  (202) for it.
- **Gets:** GET_INFO reads the comment back, but the download's INFO fork
  ends right after the name, with no comment length and no comment.
- **Should get:** the comment length and the comment after the name, as the
  Mac server sends them, so the comment survives a download.

The fork itself is readable — GtkHx reads one that ends at the name as
having no comment — so only the comment is lost.

## The log double-encodes non-ASCII text

**Verified.** Cosmetic. The admin-account line in the server's log reads
"Created admin account 'admin' ‚Äî connect from this machine…": the em dash
arrives as `‚Äî`, its UTF-8 bytes decoded as Mac Roman and encoded to UTF-8
again.

## Adding an entry

Reproduce it against the rig first, ideally as an `hx-e2e` probe, and write
it as what the client sends, what comes back, and what should. When a new
build fixes one, delete the entry and bump the pinned build in
hotline-docker.
