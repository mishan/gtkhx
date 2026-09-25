# Screenshots

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
