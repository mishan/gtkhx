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
  the GPU, but neither the GNOME runtime nor the bundle scripts ship it. A
  hidden Video page also still turns every local preview frame into a texture.
- **The camera monitor starts on the main thread.** Each voice join
  (unless Settings already holds one) starts the `GstDeviceMonitor`
  synchronously, a PipeWire round trip plus libcamera's manager, a few
  tenths of a second; a hung PipeWire would freeze the join. Starting it
  off the main thread would fix that.
- **A covered window on X11.** A hidden window stops receiving video when
  it is minimized or suspended, but X11 reports neither for a window that
  is merely covered by others, so there it keeps receiving.

## Voice

- **`voice_rejoin_media` against Janus is intermittent.** B sometimes stays in
  ICE Connecting after a rejoin or a concurrent join, and `vad_speaker`
  sometimes misses A's speaking flag. The rate didn't change when a
  stale-answer race in the runtime was fixed, and it survived moving the rig to
  a current Janus (September 2026), so it's ours. It shows up under
  `tools/isolated-run.sh` — roughly one run in four, on `main` as much as on
  any branch — but rarely when the tests share a desktop session's PipeWire,
  which points at timing rather than the server. The test's wall time also
  varies widely between runs.
- **Every first answer now waits for the microphone's caps**, up to 1500 ms.
  `voice_rejoin_media` asserts that every answer declares the send SSRC, so a
  slow audio source would fail that assertion rather than hang.

## Nick colors

- **Clearing a color mid-session doesn't reach the server.** `src/users.c`
  leaves DATA_COLOR out of the USER_CHANGE when there is no color, so as not to
  opt in, but servers read an absent DATA_COLOR as "unchanged": everyone keeps
  seeing the old color. Once a session has sent a color, clearing it should send
  `HX_NICK_COLOR_NONE` (0xFFFFFFFF).
- **`test_nick_colors` runs against Janus only** (`HX_TEST_CAP_NICK_COLORS`).
  hxd-ng supports Colored Nicknames since mishan/hxd-ng#170, so the rig's
  hxd-ng can advertise it too.

## UI and theming

- **Hand-review the symbolic icon picks (Misha).** The mapping from each classic
  pixmap to its symbolic stand-in (`symbolic_icons[]` in `src/gtkhx_icon.c`) was
  chosen by searching Adwaita's symbolic set and the icon-development-kit, not by
  browsing them. Go through both — the Icon Library app is the easiest way — and
  swap in anything that fits a button better. The weakest current matches:
  kick (`system-log-out`), tasks (`view-list`), message (`mail-unread`), post
  news (`mail-message-new`), news posts (`text-x-generic`), the drop box and
  download (both `folder-download`), upload (`document-send`), disk images
  (`media-optical`), and HTML files (`text-x-generic`; Adwaita has no symbolic
  HTML icon). A kit icon has to be converted to filled paths before it's
  vendored — see `src/icons/README.md`.

## Tests and rig

- The hxd-ng entrypoint falls back to advertising `127.0.0.1` on an IPv6-only or
  loopback-only host, and the media tests then fail with "never reached
  CONNECTED". The README and the compose comment say "addresses" where only one
  is advertised.
- hxd-ng's `xfer_port` (5521) isn't set in the rig config. Nothing uses it yet.

## Legacy servers

- hlserver.com eventually closes the connection, possibly on an idle timeout.
  If that is what it is, say so rather than reporting a plain disconnect.
- hlserver.com broadcasts "0 command(s) at a time" on login.
