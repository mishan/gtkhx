# Screenshots

There are two tools here. `tools/screenshots.sh` takes the pictures the README and the AppStream
metadata show, and takes them the same way every time. `tools/screenshot.py` is for everything
else: a quick picture of a theme, a PR description, a look at a change.

## The README and AppStream pictures

```sh
tools/screenshots.sh              # every scene, into data/screenshots/
tools/screenshots.sh chat news    # just these
tools/screenshots.sh --check      # take them afresh; fail if any changed
```

It needs Docker and a [shotbox](https://github.com/mishan/shotbox) checkout beside this one
(`../shotbox`, or wherever `SHOTBOX_DIR` points).
Everything else is in the image `tools/screenshots/Dockerfile` builds, and the image is what
makes the pictures the same on any machine: it pins the toolkit, the fonts and the renderer (the
CI base image, by digest), the Hotline servers (Janus by digest, hxd-ng by the rig's revision),
and ImageMagick. GtkHx is built from the working tree inside it, into a Docker volume that keeps
the build between runs. Bumping a digest changes pictures; regenerate them in the same commit.

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
- **Video** comes from two more GtkHx instances in sessions of their own, publishing a drawn
  picture through the test hooks `GTKHX_VOICE_TEST_VIDEO_SRC=image:PATH` and
  `GTKHX_VOICE_TEST_SCREEN_SRC`, with `GTKHX_SCREEN_AUTOSTART` to share the screen.

Each scene also picks its look. News, the tracker and video are in the dark scheme, the rest
light. The news and video scenes start from a dock layout of their own, written into the fresh
configuration as `dock-layout.ini`, so the picture has only what it's about: threaded news
without the message board beside it, chat and video without an empty column.

A scene drives GtkHx the way a person would, with shotbox's `click`, `drag`, `key` and `type`,
and waits for the things it can see (a window, a user arriving, a voice session in the server's
log). Where it can only wait on the clock, the wait is for something that has certainly finished.
`scenes.py` explains the waits that aren't obvious: the login toast, which pauses while the
pointer rests on it; the message board, whose resting scroll position depends on timing until
it is sent to the top.

To find where something is on screen, the `explore` scene logs in and then runs `$EXPLORE`, a
`;`-separated list of `click:X,Y`, `dclick:X,Y`, `drag:X1,Y1,X2,Y2`, `key:CHORD`, `type:TEXT`
or `wait:SECS` steps, each optionally `@WINDOW-RE`, capturing the screen after each:

```sh
EXPLORE='click:1138,28@GtkHx.*;wait:1' tools/screenshots.sh explore
```

The captures land in `build-screenshots/`, as does a picture of the screen when a scene gives
up waiting for something (`NAME-failed.png`).

### How identical is identical

The scenes come out byte for byte the same run after run, with two known exceptions, which is
why `--check` compares pixels with a small tolerance rather than bytes:

- Now and then the scaled server banner in the header lands one level off in a handful of
  pixels, a rounding difference that depends on the order the window was laid out in. The check
  forgives exactly one level.
- The video tiles are VP8 at a constant bitrate, and how the encoder spends its bits depends on
  live timing. The check forgives up to 5%; the differences seen stay under 4%.

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
