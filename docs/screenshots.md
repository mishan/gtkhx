# Screenshots

There are two tools here. `tools/screenshots.sh` takes the pictures the README and the AppStream
metadata show, and takes them the same way every time. `tools/screenshot.py` is for everything
else: a quick picture of a theme, a PR description, a look at a change.

## The README and AppStream pictures

```sh
tools/screenshots.sh              # every scene, into data/screenshots/
tools/screenshots.sh chat news    # just these
tools/screenshots.sh --check      # take them afresh; fail if any, or its tree, changed
```

A release's AppStream metadata points its screenshots at its own tag, so the pictures a tag
carries are the ones software centers show for that whole release. Retake them before the
release-prep PR, the one that drops `-dev` from the version in `meson.build`: the
`screenshots` workflow runs `--check` on that PR, and can be run by hand from the Actions tab.
It also runs on `main` every Monday, so a change that costs a screen reader a name, or moves a
picture, shows up within the week rather than at release prep.

It needs only Docker. Everything else is in the image `tools/screenshots/Dockerfile` builds,
and the image is what makes the pictures the same on any machine: it pins the toolkit, the fonts
and the renderer (the CI base image, by digest), the Hotline servers (Janus by digest, hxd-ng by
the rig's revision), ImageMagick, and [shotbox](https://github.com/mishan/shotbox) (the release
from PyPI, by version and hash). GtkHx is built from the working tree inside it, into a Docker
volume that keeps the build between runs. Bumping a digest or shotbox's version can change
pictures; regenerate them in the same commit.

To try a change to shotbox itself against these scenes, point `SHOTBOX_DIR` at a checkout of it.
The checkout is mounted into the container and used in place of the pinned release:

```sh
SHOTBOX_DIR=../shotbox tools/screenshots.sh --check
```

### What's in a scene

Each scene starts from nothing: a fresh server, seeded from `tools/screenshots/content/`, a fresh
GtkHx configuration, and a sealed shotbox session.

- **The server** is Northwind Commons, an invented community. Janus serves most scenes, because
  it has the extensions the pictures show off: colored names, GIF avatars and inline images.
  Its file library, message board and threaded news are written straight to disk with fixed
  dates, not posted, so no date depends on when the run happened. The video scene uses hxd-ng,
  the one server with video.
- **The people** are scripted users (`hotline.py`), a few lines of Hotline each. They log in
  before GtkHx, in a fixed order, so the user ids come out the same every run. The pictures
  they show are drawn with ImageMagick at run time rather than committed.
- **The tracker** is a stand-in in `hotline.py` that answers with a fixed v1 listing, under a
  name from the reserved `example.org` domain.
- **Transfers** go through a relay in `hotline.py`. The transfers scene connects GtkHx to it
  rather than to Janus, so its file transfers reach the relay's port + 1, and the relay hands each
  one exactly as many bytes as the scene allowed and then stalls it, where a live transfer's
  progress, speed and time left would be different on every run. The scene stalls a download,
  pauses it and resumes it on a connection the relay holds, so the row shows what it had and no
  speed (a row shows speed only while bytes arrive); stalls and pauses another, which shows its
  size and "Paused"; and asks for a third while the resumed one is still in flight, which Janus,
  allowed one download at a time for this scene, puts in its queue. The resumed one is a large file, so that
  Janus can't push the rest of it into the sockets between it and the relay and call it done.
- **Video** comes from two more GtkHx instances in sessions of their own, publishing a drawn
  picture through the test hooks `GTKHX_VOICE_TEST_VIDEO_SRC=image:PATH` and
  `GTKHX_VOICE_TEST_SCREEN_SRC`, with `GTKHX_SCREEN_AUTOSTART` to share the screen.

Each scene also picks its look. News, the tracker, video and transfers are in the dark scheme,
the rest light. The video scene starts from a dock layout of its own, written into the fresh
configuration as `dock-layout.ini`, so the picture has only what it's about: chat and video
without an empty column.

A scene drives GtkHx the way a person would, through shotbox's Python API (`shotbox.here()`,
the session's display, over one X connection): it clicks, drags, types and presses keys, and
waits for the things it can see (a window, a user arriving, a voice session or a camera in the
server's log). After a click it waits for the window to hold still rather than for a fixed
time, and before a picture it parks the pointer in the corner, off anything it would hover — except
the video scene, which rests it on a camera tile so the tile's controls show.
Where it can only wait on the clock, for a timer in the app or live video that never holds
still, the wait is for something that has certainly finished. `scenes.py` explains the waits
that aren't obvious: the login toast, which pauses while the pointer rests on it and gives no
sign before it goes; the message board, whose resting scroll position depends on timing until
it is sent to the top.

To find where something is on screen, the `explore` scene logs in, captures the screen as
`explore-0.png`, and then plays `$EXPLORE`, a script for
[`shotbox play`](https://github.com/mishan/shotbox/blob/main/docs/commands.md#play), a step a
line. Its captures named `explore-N.png` are kept:

```sh
EXPLORE=$'click 1138 28 --window GtkHx.*\ncapture explore-1.png --park' tools/screenshots.sh explore
```

The captures land in `build-screenshots/`, as does a picture of the screen when a scene gives
up waiting for something (`NAME-failed.png`, shotbox's `$SHOTBOX_FAILED`).

### How identical is identical

The scenes come out byte for byte the same run after run, with two known exceptions, which is
why `--check` compares pixels with a small tolerance rather than bytes (`shotbox compare`, a
line per picture, with the differences of any that fail beside them as `NAME-diff.png`):

- Now and then the scaled server banner in the header lands one level off in a handful of
  pixels, a rounding difference that depends on the order the window was laid out in. The check
  forgives exactly one level.
- The video tiles are VP8 at a constant bitrate, and how the encoder spends its bits depends on
  live timing. The check forgives up to 5%; the differences seen stay under 4%.

### What a screen reader hears

Every scene runs with an accessibility bus of its own (shotbox's `--a11y`), which leaves the
pictures as they were. Once a scene has taken its picture, it writes GtkHx's accessible tree, all
its windows, beside the picture as `data/screenshots/NAME.a11y.txt`, and runs shotbox's
accessibility lint over the same windows: a control, image or focusable node with no name fails
the scene, and so the run, in CI as anywhere. `--check` compares the trees line for line and
prints any difference, so a change to what a screen reader hears shows up in review beside the
pictures; a failed check leaves the fresh trees in `build-screenshots/` too. A change meant to
make one regenerates the trees with the pictures.

A tree has a node a line: its role, its name, its description and the states a screen reader
says (focusable, focused, selected, expanded...), indented by depth. No bounds, object paths,
process ids or times, and the scenes fix everything they show, so the trees come out the same
text on every run. The role names are at-spi2-core's, and differ between its releases: a push
button is `push button` in 2.52 (Ubuntu 24.04's) and `button` in 2.58 (the image's). The trees
are taken inside the pinned image, so they read as its at-spi2-core does; one taken elsewhere,
with another release, can differ in its roles alone. The lint compares roles by their enum, not
their names, and holds either way.

shotbox walks the nodes GTK says are showing. That still takes in one node collapsed out of sight:
the screen-sharing banner in every main window, which nobody in the scenes is sharing, is an
`AdwBanner` with its revealer closed, and GTK counts the banner itself as showing. The lint
covers it too.

## Quick pictures

`tools/screenshot.py` runs GtkHx headlessly and screenshots it, sealed off from the machine it
runs on. It exists for pictures that have to be comparable — theme work, docs, PR descriptions
— where a screenshot of a desktop session shows that desktop as much as it shows GtkHx.

```sh
tools/screenshot.py out.png
tools/screenshot.py --theme solarized --scheme light out.png
tools/screenshot.py --server 127.0.0.1:5500 --chat --set chat.timestamp=true chat.png
tools/screenshot.py --step key:ctrl+comma --step sleep:2 --crop full settings.png
```

It needs a built `build/src/gtkhx`, `xvfb-run`, `dbus-run-session` and ImageMagick (`import`,
`convert`); pointer and key steps also need `python3-xlib`. The app's own output lands in
`<out>.log`, and the bus and X server's in `<out>.wrapper.log`.

## What it isolates, and why each one matters

Every part of the sandbox is there because something leaked through without it:

- **A private X display** (Xvfb) with a software renderer (`GSK_RENDERER=cairo`), so the pixels
  don't depend on the GPU or the compositor.
- **An empty home.** `HOME` and the `XDG_*` directories point at a throwaway directory. The one
  that matters most is `~/.config/gtk-4.0/gtk.css`: GTK loads a user stylesheet there at *user*
  priority, above the app's own CSS, so a desktop theme installed that way repaints GtkHx over
  anything a GtkHx theme sets. It also keeps out user fonts and the user's GtkHx settings.
- **A private D-Bus session, in-memory GSettings, and no portals.** libadwaita reads the desktop's
  accent color and color scheme through the settings portal. Without this, the GNOME accent
  shows up as the accent of every "default" screenshot. The environment is set before the bus
  starts, because services it activates on demand inherit the bus's environment and would read
  the real desktop settings back in. gvfs is kept off the bus too (`GIO_USE_VFS=local`).
- **A fixed locale** (`C.UTF-8` unless `--lang` says otherwise).

The run's GtkHx settings are written fresh into `$GTKHX_PATH/gtkhx.toml`: the nick, color
scheme, theme and window size from the options, the tray off (there is no StatusNotifier host
to show it), and anything passed with `--set section.key=value`. Values are TOML: `true`,
numbers and quoted strings pass through, and a bare word is quoted for you.

`--theme-file` copies a theme `.ini` into the sandbox's themes directory and selects it, for
trying a theme without building it into the binary.

## A room with people in it

`--chat` (with `--server`) fills the public chat before the shot. The script logs in a few users
with a minimal Hotline client that lives in the tool, and has them talk once the app has
connected: a few lines from different nicks, and one that mentions the app's own nick, so the
mention highlight is in the picture. `--chat-file` plays your own script instead — a JSON list
of `{"nick", "icon", "at", "text"}`, where `at` is seconds after the first line and `{nick}` in
a text stands for the app's nick.

The local mhxd from `tests/run.sh` (`127.0.0.1:5500`) is the usual server. It accumulates news
and chat history across runs, like every long-lived container in the rig — reset it when a
picture needs to be clean.

## Driving the app

`--step` actions run in order after the app has settled (`--wait`, by default 6 seconds or long
enough for the chat script to finish):

| Step | Does |
|---|---|
| `move:X,Y` | moves the pointer, for hover states |
| `click:X,Y` | moves and clicks |
| `key:COMBO` | presses a key or chord: `key:ctrl+comma`, `key:Escape`, `key:alt+shift+Tab` |
| `sleep:S` | waits |
| `shot:PATH[@CROP]` | takes an intermediate shot, cropped by `@CROP` or else by `--crop` |

Coordinates are screen pixels. The window sits at the top-left corner of the virtual screen
at the size `--size` gives it (1100x700 by default), with room below and to the right for menus
and popovers that extend past it. `--crop` takes `window` (the default), `full`, or `X,Y,W,H`.

A final shot is always taken to the output path after the steps. If the app exits early, the
tool says so and prints the end of its log.
