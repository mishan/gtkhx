# Backlog

Known gaps and follow-ups that are smaller than a roadmap item: things a review
turned up, or a feature left deliberately unfinished. Product direction lives in
[ROADMAP.md](ROADMAP.md). When an item is done, delete it; git history is the
record.

## Video

### Gaps

- **Screen sharing on macOS and Windows** is wired up but untested. So is a Linux
  desktop without a ScreenCast portal, which has no `ximagesrc` fallback.
- **RTX and NACK on receive** (`do-nack`), and REMB. The SSRC map already
  follows `ssrc-group:FID`, but the answer pins VP8 without RTX. If a server did
  negotiate RTX, its stream would expose a pad that can't link to
  `rtpvp8depay`.
- **Renderer cost.** The panel builds a new `GdkMemoryTexture` for every frame,
  and GTK uploads each one to the GPU. `gtk4paintablesink` would keep frames on
  the GPU, but neither the GNOME runtime nor the bundle scripts ship it.
- **A covered window on X11.** A hidden window stops receiving video when
  it is minimized or suspended, but X11 reports neither for a window that
  is merely covered by others, so there it keeps receiving.

## Voice

- **Every first answer now waits for the microphone's caps**, up to 1500 ms.
  `voice_rejoin_media` asserts that every answer declares the send SSRC, so a
  slow audio source would fail that assertion rather than hang.

## Nick colors

- **`test_nick_colors` runs against Janus only** (`HX_TEST_CAP_NICK_COLORS`).
  The rig's pinned hxd-ng already includes Colored Nicknames
  (mishan/hxd-ng#170), so its row in `server_matrix.c` can claim the bit.

## UI and theming

- **Hand-review the symbolic icon picks (Misha).** The mapping from each classic
  pixmap to its symbolic stand-in (`symbolic_icons[]` in `src/gtkhx_icon.c`) was
  chosen by searching Adwaita's symbolic set and the icon-development-kit, not by
  browsing them. Go through both — the Icon Library app is the easiest way — and
  swap in anything that fits a button better. The weakest current matches:
  tasks (`view-list`), message (`mail-unread`), post news (`mail-message-new`), news posts (`text-x-generic`), the drop box and
  download (both `folder-download`), upload (`document-send`), disk images
  (`media-optical`), and HTML files (`text-x-generic`; Adwaita has no symbolic
  HTML icon). A kit icon has to be converted to filled paths before it's
  vendored — see `src/icons/README.md`.
- **The classic theme's pixmaps load through gdk-pixbuf**, which decodes
  through glycin in its own sandbox, out of `GTKHX_GLYCIN_NO_SANDBOX`'s
  reach. Where bubblewrap can't start (CI's runners, Docker's default
  profiles) they fail and the dock's tabs fall back to titles. GTK's own PNG
  loader would take them without a sandbox.

## Tests and rig

- The hxd-ng entrypoint falls back to advertising `127.0.0.1` on an IPv6-only or
  loopback-only host, and the media tests then fail with "never reached
  CONNECTED". The README and the compose comment say "addresses" where only one
  is advertised.
- hxd-ng's `xfer_port` (5521) isn't set in the rig config, which relies on the
  server's default; the server matrix and `hx-e2e` use 5521.
- `hx-e2e`'s `callback` test fails now and then when mhxd doesn't finish a
  login ("the login never settled"). A check that mhxd answers before the
  suite runs, or a restart of a wedged container, would tell that apart from
  a client bug.
- **No test reaches Wayland input.** The connection-tab drag crash only
  reproduced under a real Wayland compositor with animations off. Headless
  sway with a wlroots virtual pointer (`zwlr_virtual_pointer_v1`), `grim` for
  pictures and the app under gdb found it in seconds; made into a test tier, it
  would cover drag and drop and other input that Xvfb drives differently.

## Legacy servers

- hlserver.com eventually closes the connection, possibly on an idle timeout.
  If that is what it is, say so rather than reporting a plain disconnect.
