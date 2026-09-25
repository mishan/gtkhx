#!/usr/bin/env python3
"""Run GtkHx headlessly, sealed off from the desktop, and screenshot it.

    tools/screenshot.py out.png
    tools/screenshot.py --theme solarized --scheme light out.png
    tools/screenshot.py --server 127.0.0.1:5500 --chat --set chat.timestamp=true out.png
    tools/screenshot.py --step key:ctrl+comma --step sleep:2 --crop full settings.png

The app runs on a private Xvfb display, a private D-Bus session and an
empty home, so nothing from the machine it runs on reaches the picture:
not the desktop's GTK stylesheet (~/.config/gtk-4.0/gtk.css loads above
the app's own CSS), not its GNOME accent color (libadwaita reads that
over the session bus), not its fonts, and not the user's GtkHx settings.
The GtkHx settings the run needs are written fresh into a throwaway
config directory; --set adds to them.

--chat fills the room with a few scripted users (a minimal Hotline client
in this file), so a screenshot shows a conversation rather than an empty
pane. It needs --server; the local mhxd from tests/run.sh works.

--step drives the app between launch and the final shot: move the
pointer, click, press keys, wait, or take an intermediate shot. See
docs/screenshots.md.

Needs Xvfb (xvfb-run), dbus-run-session, ImageMagick (import, convert),
and, for --step pointer and key actions, python3-xlib.
"""

import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INNER_ENV = "GTKHX_SCREENSHOT_INNER"
HOME_ENV = "GTKHX_SCREENSHOT_HOME"

# Margin around the window on the virtual screen, so popovers and menus
# that extend past the window have somewhere to draw (--crop full).
SCREEN_MARGIN = (300, 200)

# The conversation --chat plays when no --chat-file is given. "at" is
# seconds after the first line; {nick} is the app's own nick, so the
# mention highlight shows.
DEMO_CHAT = [
    {"nick": "alice", "icon": 128, "at": 0.0, "text": "hey all, anyone tried the new build?"},
    {"nick": "bob", "icon": 129, "at": 0.8, "text": "yep, running it against Janus right now"},
    {"nick": "carol", "icon": 130, "at": 1.6, "text": "the new themes look great"},
    {"nick": "alice", "icon": 128, "at": 2.4, "text": "{nick}: the dock layout survives restarts now"},
    {"nick": "bob", "icon": 129, "at": 3.2, "text": "voice works too"},
]


def die(msg):
    # Inside the sandbox stderr goes to the wrapper log (see
    # reexec_isolated), so errors meant for the user go to stdout.
    inner = os.environ.get(INNER_ENV) == "1"
    print(f"screenshot: {msg}", file=sys.stdout if inner else sys.stderr)
    sys.exit(1)


def parse_args(argv):
    p = argparse.ArgumentParser(
        prog="tools/screenshot.py",
        description="Run GtkHx headlessly, isolated from the desktop, and screenshot it.",
    )
    p.add_argument("out", help="output PNG path")
    p.add_argument("--theme", default="default", help="GtkHx theme name (default: default)")
    p.add_argument(
        "--theme-file",
        help="a theme .ini to install into the isolated config and select; "
        "for trying a theme that isn't built in",
    )
    p.add_argument(
        "--scheme",
        choices=["light", "dark", "system"],
        default="dark",
        help="color scheme (default: dark; 'system' is light here, there is no desktop)",
    )
    p.add_argument("--size", default="1100x700", help="window size WxH (default: 1100x700)")
    p.add_argument("--nick", default="misha", help="the app's own nick (default: misha)")
    p.add_argument("--server", help="HOST[:PORT] to connect to on launch")
    p.add_argument("--chat", action="store_true", help="fill the chat with scripted users (needs --server)")
    p.add_argument(
        "--chat-file",
        help='JSON list of {"nick", "icon", "at", "text"} to play instead of the demo; implies --chat',
    )
    p.add_argument(
        "--set",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="extra gtkhx.toml setting, e.g. chat.timestamp=true (repeatable)",
    )
    p.add_argument(
        "--wait",
        type=float,
        help="seconds to let the app settle before the steps and the shot "
        "(default: 6, or long enough for the chat script to finish)",
    )
    p.add_argument(
        "--step",
        action="append",
        default=[],
        metavar="ACTION",
        help="move:X,Y | click:X,Y | key:ctrl+comma | sleep:S | shot:PATH[@CROP] "
        "(repeatable, run in order; a shot's @CROP overrides --crop)",
    )
    p.add_argument(
        "--crop",
        default="window",
        help="window | full | X,Y,W,H — what every shot keeps (default: window)",
    )
    p.add_argument("--lang", default="C.UTF-8", help="locale for the run (default: C.UTF-8)")
    p.add_argument("--binary", default=os.path.join(ROOT, "build", "src", "gtkhx"), help="gtkhx binary")
    p.add_argument("--log", help="write the app's stderr here (default: next to the output, .log)")
    p.add_argument("--keep", action="store_true", help="keep the throwaway home and print where it is")
    p.add_argument("--debug", help="GTKHX_DEBUG categories for the run")
    args = p.parse_args(argv)

    try:
        w, h = (int(v) for v in args.size.lower().split("x"))
    except ValueError:
        die(f"--size wants WxH, got {args.size!r}")
    args.width, args.height = w, h
    if args.chat_file:
        args.chat = True
    if args.chat and not args.server:
        die("--chat needs --server")
    return args


# --------------------------------------------------------------- outer run --


def sandbox_env(home):
    """The environment for everything the run starts — the bus, Xvfb and
    the app. It has to cover the bus too: services the bus activates on
    demand (the settings portal, for one) inherit its environment, and
    with the real home they would read the desktop's settings back in."""
    env = dict(os.environ)
    for var in ("WAYLAND_DISPLAY", "DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "GTKHX_DEBUG"):
        env.pop(var, None)
    env.update(
        HOME=home,
        XDG_CONFIG_HOME=os.path.join(home, ".config"),
        XDG_DATA_HOME=os.path.join(home, ".local", "share"),
        XDG_CACHE_HOME=os.path.join(home, ".cache"),
        XDG_STATE_HOME=os.path.join(home, ".local", "state"),
        GTKHX_PATH=os.path.join(home, "gtkhx"),
        # Desktop settings: none. In-memory GSettings, and no portals, so
        # libadwaita keeps its own accent and color scheme defaults and
        # the scheme comes from gtkhx.toml alone.
        GSETTINGS_BACKEND="memory",
        ADW_DISABLE_PORTAL="1",
        GDK_DEBUG="no-portals",
        # Deterministic pixels: X11 on Xvfb, the software renderer.
        GDK_BACKEND="x11",
        GSK_RENDERER="cairo",
        NO_AT_BRIDGE="1",
        GTK_A11Y="none",
        # Nothing to mount, and gvfs would be started on the private bus.
        GIO_USE_VFS="local",
        **{HOME_ENV: home},
    )
    return env


def reexec_isolated(argv, args):
    """Re-run this script inside a private D-Bus session and Xvfb display,
    with a throwaway home, and clean the home up afterwards."""
    for tool in ("dbus-run-session", "xvfb-run", "import", "convert"):
        if not shutil.which(tool):
            die(f"{tool} not found (needs dbus, xvfb and imagemagick)")
    if not os.access(args.binary, os.X_OK):
        die(f"{args.binary} isn't built: meson compile -C build")
    home = tempfile.mkdtemp(prefix="gtkhx-shot-")
    sw = args.width + SCREEN_MARGIN[0]
    sh = args.height + SCREEN_MARGIN[1]
    env = sandbox_env(home)
    env[INNER_ENV] = "1"
    cmd = [
        "dbus-run-session", "--",
        "xvfb-run", "-a", "-s", f"-screen 0 {sw}x{sh}x24",
        sys.executable, os.path.abspath(__file__), *argv,
    ]
    # The bus and Xvfb talk on stderr (service activations, teardown);
    # that goes to a log beside the output, not the terminal.
    wrapper_log = os.path.splitext(args.out)[0] + ".wrapper.log"
    try:
        with open(wrapper_log, "w") as log:
            status = subprocess.call(cmd, env=env, stderr=log)
    finally:
        if args.keep:
            print(f"screenshot: kept the run's home at {home}")
        else:
            shutil.rmtree(home, ignore_errors=True)
    sys.exit(status)


# ------------------------------------------------------------------ config --


def toml_value(raw):
    """A --set value as TOML: booleans and numbers pass through, a quoted
    string stays as written, and anything else is quoted as a string."""
    if raw in ("true", "false"):
        return raw
    try:
        float(raw)
        return raw
    except ValueError:
        pass
    if raw.startswith(('"', "[", "'")):
        return raw
    return json.dumps(raw)


def write_config(confdir, args):
    """A fresh gtkhx.toml for the run, grouped into TOML tables."""
    settings = {
        "identity.nick": json.dumps(args.nick),
        "appearance.color_scheme": json.dumps(args.scheme),
        "appearance.theme": json.dumps(args.theme),
        # No tray: there is no StatusNotifier host in the sandbox, and a
        # close-to-tray window would be the wrong thing to photograph.
        "appearance.tray": "false",
        "window.toolbar_width": str(args.width),
        "window.toolbar_height": str(args.height),
    }
    if args.theme_file:
        themes = os.path.join(confdir, "themes")
        os.makedirs(themes, exist_ok=True)
        name = os.path.splitext(os.path.basename(args.theme_file))[0]
        shutil.copy(args.theme_file, os.path.join(themes, name + ".ini"))
        settings["appearance.theme"] = json.dumps(name)
    for item in args.set:
        key, sep, raw = item.partition("=")
        if not sep or "." not in key:
            die(f"--set wants section.key=value, got {item!r}")
        settings[key.strip()] = toml_value(raw.strip())

    tables = {}
    for path, value in settings.items():
        table, _, key = path.rpartition(".")
        tables.setdefault(table, []).append(f"{key} = {value}")
    with open(os.path.join(confdir, "gtkhx.toml"), "w") as f:
        for table, lines in tables.items():
            f.write(f"[{table}]\n" + "\n".join(lines) + "\n\n")


# ---------------------------------------------------------------- chatters --
#
# Just enough Hotline to log in and talk: the handshake, then login (107),
# set-user-info (304) and chat-send (105) transactions. Replies are never
# read — the room only needs these users present and speaking.


def _field(fid, data):
    return struct.pack(">HH", fid, len(data)) + data


def _txn(typ, tid, fields):
    body = struct.pack(">H", len(fields)) + b"".join(fields)
    return struct.pack(">BBHIIII", 0, 0, typ, tid, 0, len(body), len(body)) + body


def _obfuscate(s):
    return bytes(255 - b for b in s.encode())


class Chatter:
    def __init__(self, host, port, nick, icon):
        self.nick = nick
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.sendall(b"TRTPHOTL" + struct.pack(">HH", 1, 2))
        reply = self.sock.recv(8)
        if not reply.startswith(b"TRTP"):
            raise OSError(f"{host}:{port} didn't answer as a Hotline server")
        icon_b = struct.pack(">H", icon)
        self.sock.sendall(
            _txn(107, 1, [
                _field(105, _obfuscate("")), _field(106, _obfuscate("")),
                _field(102, nick.encode()), _field(104, icon_b),
                _field(160, struct.pack(">H", 151)),
            ])
        )
        self.sock.sendall(_txn(304, 2, [_field(102, nick.encode()), _field(104, icon_b)]))
        self.tid = 10

    def say(self, text):
        self.tid += 1
        self.sock.sendall(_txn(105, self.tid, [_field(101, text.encode())]))


def load_script(args):
    if args.chat_file:
        with open(args.chat_file) as f:
            script = json.load(f)
    else:
        script = DEMO_CHAT
    return sorted(script, key=lambda line: line.get("at", 0))


def start_chatters(args, script):
    host, _, port = args.server.partition(":")
    port = int(port or 5500)
    chatters = {}
    for line in script:
        nick = line["nick"]
        if nick not in chatters:
            chatters[nick] = Chatter(host, port, nick, int(line.get("icon", 128)))
    return chatters


def play(chatters, script, nick, start):
    def run():
        for line in script:
            delay = start + float(line.get("at", 0)) - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            chatters[line["nick"]].say(line["text"].replace("{nick}", nick))

    threading.Thread(target=run, daemon=True).start()


# ------------------------------------------------------------------- steps --


class Pointer:
    """XTest input on the Xvfb display. Imported lazily: only runs that
    move the pointer or press keys need python3-xlib."""

    def __init__(self):
        try:
            from Xlib import X, XK, display
            from Xlib.ext import xtest
        except ImportError:
            die("pointer and key steps need python3-xlib")
        self.X, self.XK, self.xtest = X, XK, xtest
        self.d = display.Display()

    def move(self, x, y):
        self.xtest.fake_input(self.d, self.X.MotionNotify, x=x, y=y)
        self.d.sync()

    def click(self, x, y):
        self.move(x, y)
        time.sleep(0.2)
        self.xtest.fake_input(self.d, self.X.ButtonPress, 1)
        self.d.sync()
        time.sleep(0.05)
        self.xtest.fake_input(self.d, self.X.ButtonRelease, 1)
        self.d.sync()

    def key(self, combo):
        aliases = {"ctrl": "Control_L", "shift": "Shift_L", "alt": "Alt_L", "super": "Super_L"}
        codes = []
        for name in combo.split("+"):
            keysym = self.XK.string_to_keysym(aliases.get(name.lower(), name))
            if not keysym:
                die(f"unknown key {name!r} in {combo!r}")
            codes.append(self.d.keysym_to_keycode(keysym))
        for code in codes:
            self.xtest.fake_input(self.d, self.X.KeyPress, code)
        for code in reversed(codes):
            self.xtest.fake_input(self.d, self.X.KeyRelease, code)
        self.d.sync()


def crop_geometry(crop, args):
    if crop == "window":
        return f"{args.width}x{args.height}+0+0"
    if crop == "full":
        return None
    try:
        x, y, w, h = (int(v) for v in crop.split(","))
    except ValueError:
        die(f"a crop is window, full or X,Y,W,H; got {crop!r}")
    return f"{w}x{h}+{x}+{y}"


def shoot(path, args, crop=None):
    with tempfile.NamedTemporaryFile(suffix=".png") as raw:
        subprocess.run(["import", "-window", "root", raw.name], check=True)
        cmd = ["convert", raw.name]
        geometry = crop_geometry(crop or args.crop, args)
        if geometry:
            cmd += ["-crop", geometry, "+repage"]
        subprocess.run(cmd + [path], check=True)
    print(f"screenshot: wrote {path}")


def run_steps(args, app):
    pointer = None
    for step in args.step:
        verb, _, arg = step.partition(":")
        if app.poll() is not None:
            return False
        if verb == "sleep":
            time.sleep(float(arg))
        elif verb == "shot":
            path, _, crop = arg.partition("@")
            shoot(path, args, crop or None)
        elif verb in ("move", "click", "key"):
            pointer = pointer or Pointer()
            if verb == "key":
                pointer.key(arg)
            else:
                x, y = (int(v) for v in arg.split(","))
                (pointer.move if verb == "move" else pointer.click)(x, y)
        else:
            die(f"unknown step {step!r}")
    return True


# --------------------------------------------------------------- inner run --


def run_isolated(args):
    # The sandbox (home, bus, display) was set up by reexec_isolated; this
    # runs inside it and only has to write the app's config and drive it.
    confdir = os.environ["GTKHX_PATH"]
    os.makedirs(confdir, exist_ok=True)
    write_config(confdir, args)

    env = dict(os.environ)
    for var in (INNER_ENV, HOME_ENV):
        env.pop(var, None)
    env.update(LANG=args.lang, LC_ALL=args.lang)
    if args.debug:
        env["GTKHX_DEBUG"] = args.debug

    log_path = args.log or os.path.splitext(args.out)[0] + ".log"
    cmd = [args.binary]
    if args.server:
        host, _, port = args.server.partition(":")
        cmd += ["-s", host, "-t", port or "5500"]

    script = load_script(args) if args.chat else []
    chatters = start_chatters(args, script) if args.chat else {}

    # Chatters log in first so they are in the user list the app receives,
    # and start talking once the app has connected.
    chat_start = 5.0
    wait = args.wait
    if wait is None:
        wait = 6.0
        if script:
            wait = max(wait, chat_start + float(script[-1].get("at", 0)) + 2.0)

    ok = False
    with open(log_path, "w") as log:
        app = subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            if script:
                play(chatters, script, args.nick, time.monotonic() + chat_start)
            time.sleep(wait)
            if app.poll() is None and run_steps(args, app) and app.poll() is None:
                shoot(args.out, args)
                ok = True
        finally:
            app.terminate()
            try:
                app.wait(timeout=5)
            except subprocess.TimeoutExpired:
                app.kill()

    if not ok:
        with open(log_path) as f:
            tail = f.read().splitlines()[-20:]
        die("the app exited before the shot; last lines of its log:\n  " + "\n  ".join(tail))


def main():
    argv = sys.argv[1:]
    args = parse_args(argv)
    if os.environ.get(INNER_ENV) != "1":
        reexec_isolated(argv, args)
    run_isolated(args)


if __name__ == "__main__":
    main()
