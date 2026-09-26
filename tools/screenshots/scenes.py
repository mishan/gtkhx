#!/usr/bin/env python3
"""Take the README and AppStream screenshots. Runs inside the image
tools/screenshots/Dockerfile builds; tools/screenshots.sh is the way in.

    scenes.py OUTDIR [SCENE...]
    scenes.py --compare REFDIR NEWDIR [SCENE...]

Each scene gets a fresh server seeded from content/, the scripted users
log in before GtkHx does (so user ids come out the same every run), and
GtkHx runs in a sealed shotbox session with settings written fresh.
Nothing waits on a clock where it can wait on the thing itself; where it
can't, the wait is for something that has certainly finished by then.
"""

import datetime
import json
import os
import shutil
import signal
import sqlite3
import subprocess
import sys
import time
from pathlib import Path

import hotline

HERE = Path(__file__).resolve().parent
CONTENT = HERE / "content"
SRC = Path(os.environ.get("GTKHX_SRC", "/src"))
GTKHX = os.environ.get("GTKHX_BIN", "/work/build/src/gtkhx")
SHOTBOX = os.environ.get("SHOTBOX_BIN", "/shotbox/bin/shotbox")
WORK = Path("/tmp/shots")

JANUS_PORT = 5500
TRACKER_PORT = 5498
# The fake tracker's name, as the tracker window shows it. The container
# maps it to loopback (tools/screenshots.sh, --add-host); example.org is
# reserved for exactly this.
TRACKER_HOST = "tracker.example.org"
WINDOW = (1280, 800)
SCREEN = (1600, 1000)


def load(name):
    return json.loads((CONTENT / name).read_text())


def epoch(iso):
    return int(datetime.datetime.fromisoformat(iso.replace("Z", "+00:00")).timestamp())


def log(msg):
    print(f"scenes: {msg}", flush=True)


def wait_until(what, test, timeout=30, interval=0.1):
    end = time.monotonic() + timeout
    while not test():
        if time.monotonic() > end:
            # Inside a session, a picture of the screen says more than this.
            if os.environ.get("SHOTBOX_SCRATCH") and os.environ.get("SCENES_DEBUG_SHOT"):
                shotbox("capture", os.environ["SCENES_DEBUG_SHOT"], check=False)
            raise SystemExit(f"scenes: gave up waiting for {what}")
        time.sleep(interval)


# ------------------------------------------------------------------ assets --


def magick(*args):
    subprocess.run(["magick", *map(str, args)], check=True)


def make_assets(out):
    """The pictures the content needs, drawn here rather than committed:
    the inline photo, the GIF avatars and the server banner."""
    out.mkdir(parents=True, exist_ok=True)
    strip = ["-strip", "-define", "png:exclude-chunks=date,time"]
    # A harbor at dawn: a warm sky over cool water, a low sun, a pier.
    magick("-size", "480x300", "gradient:#f6c7a1-#7aa6c9", "(",
           "-size", "480x110", "gradient:#6f93b3-#2f4d68", ")", "-gravity", "south",
           "-composite",
           "-fill", "#fff1d6", "-draw", "circle 330,176 330,154",
           "-fill", "#2c3440", "-draw", "rectangle 40,178 250,186",
           "-draw", "rectangle 60,186 64,215", "-draw", "rectangle 120,186 124,215",
           "-draw", "rectangle 180,186 184,215", "-draw", "rectangle 240,186 244,215",
           "-fill", "#f3d9b8", "-draw", "rectangle 300,200 360,202",
           "-draw", "rectangle 312,212 348,213", "-draw", "rectangle 322,224 338,225",
           *strip, out / "harbor.png")
    server = load("server.json")
    for user in server["users"] + [server["you"]]:
        a = user.get("avatar")
        if not a:
            continue
        # 16x16, a classic icon's size: GtkHx draws a GIF avatar at its own
        # pixel size, so this is what fits a user-list row.
        magick("-size", "16x16", "xc:none", "-fill", a["bg"], "-draw", "circle 7.5,7.5 7.5,0",
               "-fill", a["fg"], "-font", "Adwaita-Sans-Bold", "-pointsize", "10",
               "-gravity", "center", "-annotate", "+0+0", a["letter"],
               out / f"{user['nick']}.gif")
    # Camera pictures for the video scene: a person at a desk, drawn flat.
    for name, wall, shirt, skin in (("cam-ada", "#e9dccb", "#f97316", "#c98b62"),
                                    ("cam-marco", "#d6e2e9", "#22c55e", "#8d5a3b")):
        # 4:3, which is what the capture pipeline sends a camera as.
        magick("-size", "640x480", f"gradient:{wall}-#b9a992", "(",
               "-size", "640x120", "xc:#6b4f3a", ")", "-gravity", "south", "-composite",
               "-fill", "#ffffff80", "-draw", "rectangle 450,70 600,240",
               "-fill", "#9fb7c9", "-draw", "rectangle 460,80 590,230",
               "-fill", "#5b7f4a", "-draw", "ellipse 90,300 45,65 0,360",
               "-fill", "#8a5a3b", "-draw", "rectangle 72,340 108,368",
               "-fill", shirt, "-draw", "ellipse 320,470 170,130 180,360",
               "-fill", skin, "-draw", "rectangle 295,250 345,300",
               "-draw", "ellipse 320,210 78,95 0,360",
               "-fill", "#2d2019", "-draw", "ellipse 320,150 82,50 180,360",
               *strip, out / f"{name}.png")
    # A shared screen: the harbor photo open in an editor.
    magick("-size", "1280x720", "xc:#1f2126",
           "-fill", "#2a2d33", "-draw", "rectangle 0,0 1280,44",
           "-fill", "#ff5f57", "-draw", "circle 22,22 28,22",
           "-fill", "#febc2e", "-draw", "circle 44,22 50,22",
           "-fill", "#28c840", "-draw", "circle 66,22 72,22",
           "-fill", "#2a2d33", "-draw", "rectangle 0,44 64,720",
           "-fill", "#3a3e46", "-draw", "roundrectangle 16,64 48,96 6,6",
           "-draw", "roundrectangle 16,112 48,144 6,6", "-draw", "roundrectangle 16,160 48,192 6,6",
           "-fill", "#2a2d33", "-draw", "rectangle 1000,44 1280,720",
           "(", out / "harbor.png", "-resize", "880x550", ")", "-geometry", "+92+100",
           "-composite",
           *[a for n, y in enumerate(range(110, 470, 60)) for a in (
               "-fill", "#4a4f59", "-draw", f"roundrectangle 1024,{y} 1256,{y + 6} 3,3",
               "-fill", "#2f7fb8", "-draw", f"roundrectangle 1024,{y} {1080 + 30 * n},{y + 6} 3,3",
               "-fill", "#e5e7eb", "-draw", f"circle {1080 + 30 * n},{y + 3} {1088 + 30 * n},{y + 3}")],
           *strip, out / "screen-ada.png")
    server = load("server.json")
    magick("-size", "468x60", "gradient:#1f4e79-#2f7fb8", "-fill", "white",
           "-font", "Adwaita-Sans-Bold", "-pointsize", "24", "-gravity", "west",
           "-annotate", "+18+0", server["name"], "-fill", "#d7e7f5",
           "-font", "Adwaita-Sans", "-pointsize", "12", "-gravity", "east",
           "-annotate", "+16+0", "photo walks · sessions · files", out / "banner.gif")


# ------------------------------------------------------------------- Janus --


def janus_config(template, banner):
    """The rig's Janus config, turned into the showcase server's."""
    server = load("server.json")
    replace = {
        "Port": str(JANUS_PORT),
        "TLSCert": '""',
        "TLSKey": '""',
        "Name": server["name"],
        "Description": server["description"],
        "BannerFile": json.dumps(banner),
        "BannerClickURL": '""',
        "EnableTrackerRegistration": "false",
        "EnableVoice": "false",
        "ChatHistoryEnabled": "false",
        "APIAddr": '""',
    }
    lines = []
    for line in template.read_text().splitlines():
        key = line.split(":", 1)[0]
        if key in replace and not line.startswith((" ", "#")):
            line = f"{key}: {replace.pop(key)}"
        lines.append(line)
    if replace:
        raise SystemExit(f"scenes: Janus config has no {', '.join(replace)}")
    return "\n".join(lines) + "\n"


def make_files(root, which=None):
    """The server's file library, or with `which`, another tree from
    files.json; every date fixed."""
    spec = load("files.json")
    if which:
        spec = spec[which]
    when = epoch(spec["date"])
    dirs = []

    def build(base, tree):
        for name, value in tree.items():
            path = base / name
            if isinstance(value, dict):
                path.mkdir()
                build(path, value)
                dirs.append(path)
            else:
                with open(path, "wb") as f:
                    f.truncate(value)
                os.utime(path, (when, when))

    shutil.rmtree(root, ignore_errors=True)
    root.mkdir()
    build(root, spec["tree"])
    for d in dirs + [root]:
        os.utime(d, (when, when))


def seed_news(db):
    """The threaded news, with fixed dates and ids."""
    news = load("news.json")
    con = sqlite3.connect(db)
    con.execute("DELETE FROM articles")
    con.execute("DELETE FROM categories")
    for n, cat in enumerate(news["categories"]):
        path = cat["name"]
        con.execute("INSERT INTO categories (path, name, kind, guid, add_sn, delete_sn, parent_path) "
                    "VALUES (?, ?, 'category', ?, 0, 0, '')",
                    (path, cat["name"], bytes([n + 1]) * 16))
        next_id = [1]

        def post(article, parent):
            aid = next_id[0]
            next_id[0] += 1
            con.execute("INSERT INTO articles (bundle_path, id, parent_id, title, poster, login, "
                        "posted_at, body) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                        (path, aid, parent, article["title"], article["poster"],
                         article["poster"], epoch(article["at"]), article["body"]))
            for reply in article.get("replies", []):
                post(reply, aid)

        for article in cat["articles"]:
            post(article, 0)
    con.commit()
    con.close()


def seed_board(path):
    """The flat (1.2) news: Janus's message board, oldest post first."""
    posts = sorted(load("board.json"), key=lambda p: p["at"])
    with open(path, "w") as f:
        for n, post in enumerate(posts, 1):
            f.write(json.dumps({"id": n, "login": "guest", "nick": post["nick"],
                                "body": post["body"], "ts": post["at"],
                                "ip": "127.0.0.1"}) + "\n")


class Janus:
    """A fresh Janus with the showcase content, for one scene."""

    def __init__(self, assets):
        self.dir = WORK / "janus"
        shutil.rmtree(self.dir, ignore_errors=True)
        shutil.copytree("/opt/janus", self.dir, symlinks=True)
        server = self.dir / "Server"
        for stale in ("news.db", "chat_history.db", "disk_catalog.db"):
            for f in server.glob(stale + "*"):
                f.unlink()
        (server / "config.yaml").write_text(
            janus_config(SRC / "tests/janus/conf/config.yaml", "banner.gif"))
        shutil.copy(assets / "banner.gif", server / "banner.gif")
        (server / "Agreement.txt").write_text(load("server.json")["agreement"])
        # Everyone logs in as guest; the scripted users post an image.
        guest = server / "Users" / "guest.yaml"
        guest.write_text(guest.read_text().replace("SendMedia: false", "SendMedia: true"))
        make_files(server / "Files")
        seed_board(server / "MessageBoard.jsonl")
        self.log = open(WORK / "janus.log", "ab")
        # Once to create its databases, then again with the news seeded.
        self._start()
        wait_until("Janus's news database", lambda: (server / "news.db").exists())
        wait_until("Janus to listen", lambda: port_open(JANUS_PORT))
        self.stop()
        seed_news(server / "news.db")
        self._start()
        wait_until("Janus to listen", lambda: port_open(JANUS_PORT))

    def _start(self):
        self.proc = subprocess.Popen(["./janus"], cwd=self.dir, stdout=self.log,
                                     stderr=self.log, start_new_session=True)

    def stop(self):
        if self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            self.proc.wait(10)
        wait_until("Janus to stop listening", lambda: not port_open(JANUS_PORT))


HXD_PORT = 5520
HXD_MEDIA_PORT = 5524


def container_ip():
    """hxd-ng is ICE-lite: it must advertise an address the clients reach
    that isn't loopback."""
    import socket
    # The address the default route leaves from; nothing is sent.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(("192.0.2.1", 9))
        addr = s.getsockname()[0]
    if addr.startswith("127."):
        raise SystemExit("scenes: no non-loopback IPv4 address for hxd-ng to advertise")
    return addr


class HxdNg:
    """A fresh hxd-ng for the video scene: the one server with video."""

    def __init__(self):
        self.dir = WORK / "hxd-ng"
        shutil.rmtree(self.dir, ignore_errors=True)
        self.dir.mkdir(parents=True)
        shutil.copytree(SRC / "tests/hxd-ng/conf/accounts", self.dir / "accounts")
        conf = self.dir / "hxd-ng.toml"
        conf.write_text(
            f'[server]\nbind = "0.0.0.0:{HXD_PORT}"\n'
            f'name = {json.dumps(load("server.json")["name"])}\n\n'
            f'[paths]\naccounts = "{self.dir / "accounts"}"\n\n'
            f'[voice]\nbind = "0.0.0.0:{HXD_MEDIA_PORT}"\n'
            f'advertise = ["{container_ip()}:{HXD_MEDIA_PORT}"]\n\n'
            '[voice.video]\n')
        # A fresh, uncolored log: the video scene reads it to see who joined.
        self.proc = subprocess.Popen(["hxd", "--config", str(conf)], cwd=self.dir,
                                     stdout=open(WORK / "hxd-ng.log", "wb"),
                                     stderr=subprocess.STDOUT, start_new_session=True,
                                     env={**os.environ, "NO_COLOR": "1"})
        wait_until("hxd-ng to listen", lambda: port_open(HXD_PORT))

    def stop(self):
        if self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            self.proc.wait(10)


def port_open(port):
    import socket
    with socket.socket() as s:
        s.settimeout(0.2)
        return s.connect_ex(("127.0.0.1", port)) == 0


# ------------------------------------------------------------------- users --


def log_in_users(assets, avatars=True):
    """The scripted users, in a fixed order, so their user ids are too."""
    bots = {}
    for user in load("server.json")["users"]:
        bot = hotline.Bot("127.0.0.1", JANUS_PORT, user["nick"], user["icon"])
        if "color" in user:
            bot.set_color(int(user["color"][1:], 16))
        avatar = assets / f"{user['nick']}.gif"
        if avatars and avatar.exists():
            bot.set_gif_icon(avatar.read_bytes())
        bots[user["nick"]] = bot
    return bots


# ------------------------------------------------------------------ GtkHx --


def gtkhx_config(path, theme="default", scheme="light", extra=None, avatars=True, layout=None):
    you = load("server.json")["you"]
    path.mkdir(parents=True, exist_ok=True)
    settings = {
        "identity": {"nick": you["nick"], "icon": you["icon"], "nick_color": you["color"]},
        "appearance": {"theme": theme, "color_scheme": scheme, "tray": False},
        "chat": {"timestamp": False, "history_initial": 0, "show_joins": False},
        "trackers": {"addresses": [f"{TRACKER_HOST}:{TRACKER_PORT}"]},
        "window": {"toolbar_width": WINDOW[0], "toolbar_height": WINDOW[1]},
    }
    for table, values in (extra or {}).items():
        settings.setdefault(table, {}).update(values)
    lines = []
    for table, values in settings.items():
        lines.append(f"[{table}]")
        lines += [f"{k} = {json.dumps(v)}" for k, v in values.items()]
        lines.append("")
    (path / "gtkhx.toml").write_text("\n".join(lines))
    # The dock's panels, where a scene wants other than the default.
    if layout:
        (path / "dock-layout.ini").write_text(
            "[Dock]\n" + "".join(f"{k}={v}\n" for k, v in layout.items()))
    # This user's own avatar, which GtkHx sends to a server that takes them.
    if avatars:
        shutil.copy(WORK / "assets" / f"{you['nick']}.gif", path / "avatar.gif")
    # The classic user icons, which an installed GtkHx finds in its data dir.
    (path / "icons").mkdir(exist_ok=True)
    shutil.copy(SRC / "icons.rsrc", path / "icons" / "icons.rsrc")


def shotbox(*args, check=True):
    return subprocess.run([SHOTBOX, *map(str, args)], check=check)


# ------------------------------------------------------------------ scenes --
#
# A scene is a function run inside the shotbox session, with GtkHx not yet
# started. It returns once the picture is taken.


def start_gtkhx(connect=True, port=JANUS_PORT, env=None):
    cmd = [GTKHX]
    if connect:
        cmd += ["-s", "127.0.0.1", "-t", str(port)]
    # The file browser's local side opens on the working directory: give
    # it a folder of its own with fixed dates, not the container's root.
    local = WORK / "local"
    make_files(local, "local")
    proc = subprocess.Popen(cmd, cwd=local, stdout=open(WORK / "gtkhx.log", "ab"),
                            stderr=subprocess.STDOUT, start_new_session=True,
                            env={**os.environ, **(env or {})})
    shotbox("wait", "window", "GtkHx.*")
    return proc


def log_in(bots, port=JANUS_PORT, agreement=True, env=None):
    """Start GtkHx, agree to the server's agreement, and clear the chat of
    the connection messages. Returns the app once the others see it."""
    app = start_gtkhx(port=port, env=env)
    you = load("server.json")["you"]["nick"]
    if agreement:
        shotbox("wait", "window", "Agreement")
        shotbox("click", 390, 500, "--window", "Agreement")
    # GtkHx is in once the others see it arrive.
    wait_until("GtkHx to log in", lambda: any(b.saw_user(you) for b in bots.values()))
    shotbox("click", 560, 660, "--window", "GtkHx.*")
    shotbox("type", "/clear\n")
    # Off the window: a pointer resting on the login toast stops it timing
    # out, and would show as a hover anywhere else.
    shotbox("move", SCREEN[0] - 1, SCREEN[1] - 1)
    time.sleep(0.5)
    return app


# How long after login to take a picture, at the least: the "Logged in"
# toast stays up for Adwaita's default of five seconds.
TOAST_GONE = 6.0


def board_to_top():
    """Scroll the message board to its first post. Where it comes to rest
    after loading depends on when the text arrived relative to its
    layout, a pixel either way from run to run."""
    shotbox("click", 150, 400, "--window", "GtkHx.*")
    shotbox("key", "ctrl+Home")
    shotbox("move", SCREEN[0] - 1, SCREEN[1] - 1)
    # The overlay scrollbar the scroll brought up hides after a second.
    time.sleep(2)


def capture_window(out, since):
    """Capture the main window once the login toast has gone."""
    left = since + TOAST_GONE - time.monotonic()
    if left > 0:
        time.sleep(left)
    shotbox("capture", out, "--window", "GtkHx.*")


def say_lines(bots, script, assets):
    you = load("server.json")["you"]["nick"]
    for line in script:
        image = (assets / line["image"]).read_bytes() if "image" in line else None
        bots[line["nick"]].say(line["text"].replace("{nick}", you), image=image)
        # Lines come from different connections: space them so the server
        # relays them in script order.
        time.sleep(0.4)


def scene_chat(out, bots, assets):
    app = log_in(bots)
    since = time.monotonic()
    say_lines(bots, load("chat.json"), assets)
    # The image is fetched after its line arrives; a second is plenty.
    time.sleep(2)
    board_to_top()
    capture_window(out, since)
    app.terminate()


def menu(item_y):
    """Pick an item from the main menu, by its height in the popover."""
    shotbox("click", 1138, 28, "--window", "GtkHx.*")
    time.sleep(0.5)
    shotbox("click", 1114, item_y, "--window", "GtkHx.*")


def scene_files(out, bots, assets):
    app = log_in(bots)
    since = time.monotonic()
    menu(175)                                   # Files
    shotbox("wait", "window", "Files.*")
    time.sleep(1)
    win = ["--window", "Files.*"]
    shotbox("click", 558, 183, "--double", *win)  # Photos
    time.sleep(1)
    shotbox("click", 558, 158, "--double", *win)  # Harbor
    time.sleep(1)
    shotbox("click", 600, 183, *win)            # fog-over-the-pier.jpg
    shotbox("move", SCREEN[0] - 1, SCREEN[1] - 1)
    time.sleep(max(1.0, since + TOAST_GONE - time.monotonic()))
    shotbox("capture", out, *win)
    app.terminate()


def scene_news(out, bots, assets):
    app = log_in(bots)
    since = time.monotonic()
    win = ["--window", "GtkHx.*"]
    shotbox("click", 16, 169, *win)             # Photography
    time.sleep(1.5)
    shotbox("click", 36, 192, *win)             # Settings for low light?
    time.sleep(1.5)
    shotbox("click", 56, 215, *win)             # Tripod and a slow shutter
    time.sleep(1.5)
    shotbox("click", 160, 215, *win)
    time.sleep(1.5)
    # Room for the nested reply's title.
    shotbox("drag", 286, 450, 370, 450, *win)
    time.sleep(0.5)
    capture_window(out, since)
    app.terminate()


def scene_tracker(out, bots, assets):
    app = log_in(bots)
    since = time.monotonic()
    menu(143)                                   # Tracker
    shotbox("wait", "window", "Tracker")
    # The listing arrives in one burst; the count in the header says so.
    time.sleep(3)
    time.sleep(max(0, since + TOAST_GONE - time.monotonic()))
    shotbox("capture", out, "--window", "Tracker")
    app.terminate()


# Voice and video for a GtkHx with no microphone or camera: a test tone,
# and join as soon as the server allows it.
VOICE_ENV = {"GTKHX_VOICE_AUTOJOIN": "1", "GTKHX_VOICE_TEST_AUDIO_SRC": "1"}


def publisher(nick, icon, picture, screen=None):
    """Another GtkHx, in a session of its own, that joins voice and turns
    its camera on, showing `picture`. Returns the shotbox process."""
    conf = WORK / f"config-{nick}"
    shutil.rmtree(conf, ignore_errors=True)
    conf.mkdir(parents=True)
    (conf / "gtkhx.toml").write_text(
        f'[identity]\nnick = "{nick}"\nicon = {icon}\n'
        '[appearance]\ntray = false\n')
    env = ["--env", f"GTKHX_DEBUG={os.environ.get('GTKHX_DEBUG', '')}",
           "--env", f"GTKHX_PATH={conf}", "--env", "GTKHX_VIDEO_AUTOSTART=1",
           "--env", f"GTKHX_VOICE_TEST_VIDEO_SRC=image:{picture}"]
    env += [a for k, v in VOICE_ENV.items() for a in ("--env", f"{k}={v}")]
    if screen:
        env += ["--env", "GTKHX_SCREEN_AUTOSTART=1",
                "--env", f"GTKHX_VOICE_TEST_SCREEN_SRC=image:{screen}"]
    return subprocess.Popen(
        [SHOTBOX, "run", *env, "--", GTKHX, "-s", "127.0.0.1", "-t", str(HXD_PORT)],
        stdout=open(WORK / f"{nick}.log", "ab"), stderr=subprocess.STDOUT,
        start_new_session=True)


def scene_video(out, bots, assets):
    log = WORK / "hxd-ng.log"

    def in_voice(uid):
        return f"voice session connected uid={uid} " in log.read_text(errors="replace")

    # One at a time, so ada is uid 1 and marco uid 2 on every run.
    pubs = [publisher("ada", 128, assets / "cam-ada.png", screen=assets / "screen-ada.png")]
    try:
        wait_until("ada to join voice", lambda: in_voice(1), timeout=60)
        pubs.append(publisher("marco", 129, assets / "cam-marco.png"))
        wait_until("marco to join voice", lambda: in_voice(2), timeout=60)
        # Cameras and ada's screen go on a few seconds after joining.
        time.sleep(8)
        users = {u["nick"]: u for u in load("server.json")["users"]}
        for nick in ("priya", "jonas", "lena"):
            bots[nick] = hotline.Bot("127.0.0.1", HXD_PORT, nick, users[nick]["icon"])
        app = log_in(bots, port=HXD_PORT, agreement=False,
                     env={**VOICE_ENV, "GTKHX_VIDEO_AUTOPRESENT": "1"})
        since = time.monotonic()
        say_lines(bots, load("video-chat.json"), assets)
        # Joining, the offer and answer, the first keyframes.
        time.sleep(25)
        shotbox("move", SCREEN[0] - 1, SCREEN[1] - 1)
        capture_window(out, since)
        app.terminate()
    finally:
        for p in pubs:
            os.killpg(p.pid, signal.SIGTERM)


def scene_explore(out, bots, assets):
    """Not a picture for the README: for finding where things are. Logs
    in, then for each step in $EXPLORE (`click:X,Y`, `dclick:X,Y`,
    `drag:X1,Y1,X2,Y2`, `key:CHORD`, `type:TEXT`, `wait:SECS`, each optionally `@WINDOW-RE`)
    takes a picture of the whole screen, numbered after the output."""
    app = log_in(bots)
    since = time.monotonic()
    time.sleep(max(0, since + TOAST_GONE - time.monotonic()))
    stem = Path(out).with_suffix("")
    shotbox("capture", f"{stem}-0.png")
    for n, step in enumerate(filter(None, os.environ.get("EXPLORE", "").split(";")), 1):
        step, _, window = step.partition("@")
        kind, _, arg = step.partition(":")
        where = ["--window", window] if window else []
        if kind in ("click", "dclick"):
            x, y = arg.split(",")
            shotbox("click", x, y, *where, *(["--double"] if kind == "dclick" else []))
        elif kind == "drag":
            shotbox("drag", *arg.split(","), *where)
        elif kind == "key":
            shotbox("key", arg)
        elif kind == "type":
            shotbox("type", arg)
        elif kind == "wait":
            time.sleep(float(arg))
        time.sleep(1.5)
        shotbox("move", SCREEN[0] - 1, SCREEN[1] - 1)
        shotbox("capture", f"{stem}-{n}.png")
    app.terminate()


SCENES = {
    "chat": (scene_chat, {}),
    # Threaded news alone in the middle: the message board beside it
    # would be a second news view saying less.
    "news": (scene_news, {"scheme": "dark", "layout": {
        "tree": "h(L[chat,*news15:center],L[*users,video:end])",
        "sizes": "880", "closed": "news;tasks"}}),
    "files": (scene_files, {}),
    "tracker": (scene_tracker, {"scheme": "dark"}),
    # Chat and the video beside it; nothing in the left column to crowd it.
    "video": (scene_video, {"scheme": "dark", "layout": {
        "tree": "h(L[*chat,news15:center],L[users,*video:end])",
        "sizes": "470", "closed": "news;tasks"}}),
    "classic": (scene_chat, {"theme": "classic"}),
    "explore": (scene_explore, {}),
}

# The scenes on hxd-ng rather than Janus.
ON_HXD_NG = {"video"}


def inner(scene, out):
    """Inside the session: the scene, with its server and users."""
    assets = WORK / "assets"
    fn, opts = SCENES[scene]
    if scene in ON_HXD_NG:
        server, bots = HxdNg(), {}
    else:
        server = Janus(assets)
        bots = log_in_users(assets, avatars=opts.get("avatars", True))
    try:
        fn(Path(out), bots, assets)
    finally:
        for b in bots.values():
            b.close()
        server.stop()


# How far a fresh picture may stray from the committed one: shotbox
# compare's --fuzz. The scenes nearly always come out the same bytes, but
# now and then the header's scaled banner lands one level off in a handful
# of pixels, a rounding difference that depends on the order the window
# was laid out in. 0.5% forgives exactly that one level. The video tiles
# are VP8 at a constant bitrate, and how the encoder spends its bits
# depends on live timing: never more than 4% in any channel.
FUZZ = {"video": "5%"}
DEFAULT_FUZZ = "0.5%"


def compare(ref, new, names):
    """Compare fresh pictures against committed ones; the failing ones'
    differences go beside them as NAME-diff.png."""
    bad = []
    for name in names:
        a, b = Path(ref) / f"{name}.png", Path(new) / f"{name}.png"
        diff = Path(new) / f"{name}-diff.png"
        r = subprocess.run([SHOTBOX, "compare", a, b, "--diff", diff,
                            "--fuzz", FUZZ.get(name, DEFAULT_FUZZ)], capture_output=True, text=True)
        log(f"{name}: {r.stdout.strip() or r.stderr.strip()}")
        if r.returncode:
            bad.append(name)
    if bad:
        log(f"differ: {', '.join(bad)}; see {new}")
        return 1
    return 0


def main(argv):
    if len(argv) >= 1 and argv[0] == "--inner":
        return inner(argv[1], argv[2])
    if len(argv) >= 3 and argv[0] == "--compare":
        names = argv[3:] or [n for n in SCENES if n != "explore"]
        return compare(argv[1], argv[2], names)
    if not argv:
        raise SystemExit(__doc__)
    outdir = Path(argv[0])
    names = argv[1:] or [n for n in SCENES if n != "explore"]
    WORK.mkdir(parents=True, exist_ok=True)
    make_assets(WORK / "assets")
    hotline.serve_tracker(TRACKER_PORT, load("tracker.json"))
    for name in names:
        _, opts = SCENES[name]
        conf = WORK / f"config-{name}"
        shutil.rmtree(conf, ignore_errors=True)
        gtkhx_config(conf, **opts)
        log(f"{name}...")
        shotbox("run", "--screen", f"{SCREEN[0]}x{SCREEN[1]}",
                "--env", f"GTKHX_PATH={conf}",
                "--env", f"GTKHX_DEBUG={os.environ.get('GTKHX_DEBUG', '')}",
                "--env", f"EXPLORE={os.environ.get('EXPLORE', '')}",

                "--env", f"SCENES_DEBUG_SHOT={outdir / (name + '-failed.png')}", "--pass", "GTKHX_SRC", "--pass", "PYTHONPATH",
                "--", sys.executable, __file__, "--inner", name, outdir / f"{name}.png")
        log(f"{name}: {outdir / (name + '.png')}")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
