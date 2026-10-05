#!/usr/bin/env python3
"""Run GtkHx as if inside a Flatpak, against a stand-in update portal.

    GTKHX_REAL=build/src/gtkhx tools/screenshot.py \\
        --binary tools/fake-flatpak-portal.py --step sleep:3 out.png

The script is the binary screenshot.py launches: on screenshot.py's private
session bus it serves org.freedesktop.portal.Flatpak, then runs GTKHX_REAL
under bwrap with a /.flatpak-info, so the update banner's real D-Bus code
runs without a Flatpak install. Calls it receives go to FAKE_PORTAL_LOG.

The build must be configured with -Dupdate_check=enabled: auto is off on
Linux, and then nothing asks.

FAKE_PORTAL_MODE picks the story:
  available  the remote has a newer commit; Update succeeds (default)
  installed  a newer commit is installed but not running
  fail       Update fails, then the "software center" installs it anyway
  fail-transient  every Update fails and no commit moves, as on a network error
  stall      Update reports once and then nothing, until Close cancels it
FAKE_PORTAL_VERSION sets the portal's version property (default 2).
"""

import os
import subprocess
import sys
import time

from gi.repository import Gio, GLib

ENV = os.environ
LOG = ENV.get("FAKE_PORTAL_LOG", "/tmp/fake-flatpak-portal.log")
MODE = ENV.get("FAKE_PORTAL_MODE", "available")
BUS = "org.freedesktop.portal.Flatpak"
MONITOR_IFACE = "org.freedesktop.portal.Flatpak.UpdateMonitor"
XML = """<node>
 <interface name="org.freedesktop.portal.Flatpak">
  <method name="CreateUpdateMonitor"><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
  <method name="Spawn"><arg type="ay" direction="in"/><arg type="aay" direction="in"/>
   <arg type="a{uh}" direction="in"/><arg type="a{ss}" direction="in"/><arg type="u" direction="in"/>
   <arg type="a{sv}" direction="in"/><arg type="u" direction="out"/></method>
  <property name="version" type="u" access="read"/>
 </interface>
 <interface name="org.freedesktop.portal.Flatpak.UpdateMonitor">
  <method name="Close"/>
  <method name="Update"><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/></method>
  <signal name="UpdateAvailable"><arg type="a{sv}"/></signal>
  <signal name="Progress"><arg type="a{sv}"/></signal>
 </interface>
</node>"""


def serve():
    node = Gio.DBusNodeInfo.new_for_xml(XML)
    log = open(LOG, "a", buffering=1)
    # Per monitor object path: its caller, pending signals and whether an
    # Update is under way. Close cancels both, as the real portal does.
    monitors = {}

    def later(path, ms, member, values):
        m = monitors[path]

        def fire():
            m["timers"].remove(source)
            if member == "Progress" and values["status"].unpack() >= 2:
                m["installing"] = False
            m["conn"].emit_signal(m["dest"], path, MONITOR_IFACE, member,
                                  GLib.Variant("(a{sv})", (values,)))
            return False
        source = GLib.timeout_add(ms, fire)
        m["timers"].append(source)

    def commits(running, local, remote):
        return {"running-commit": GLib.Variant("s", running),
                "local-commit": GLib.Variant("s", local),
                "remote-commit": GLib.Variant("s", remote)}

    def progress(status, percent, **extra):
        d = {"status": GLib.Variant("u", status), "progress": GLib.Variant("u", percent),
             "op": GLib.Variant("u", 0), "n_ops": GLib.Variant("u", 1)}
        d.update({k: GLib.Variant("s", v) for k, v in extra.items()})
        return d

    def method(conn, sender, path, iface, name, params, inv):
        log.write(f"{path} {iface}.{name} {params}\n")
        if name == "CreateUpdateMonitor":
            token = params.unpack()[0].get("handle_token", "t")
            path = ("/org/freedesktop/portal/Flatpak/update_monitor/"
                    f"{sender[1:].replace('.', '_')}/{token}")
            reg = conn.register_object(path, node.interfaces[1], method, None, None)
            monitors[path] = {"conn": conn, "dest": sender, "timers": [],
                              "installing": False, "reg": reg}
            inv.return_value(GLib.Variant("(o)", (path,)))
            later(path, 1000, "UpdateAvailable",
                  commits("a", "b", "b") if MODE == "installed" else commits("a", "a", "b"))
        elif name == "Close":
            m = monitors.pop(path)
            for source in m["timers"]:
                GLib.source_remove(source)
            conn.unregister_object(m["reg"])
            inv.return_value(None)
        elif name == "Update":
            m = monitors[path]
            if m["installing"]:
                inv.return_dbus_error("org.freedesktop.DBus.Error.Failed", "Already installing")
                return
            m["installing"] = True
            inv.return_value(None)
            later(path, 500, "Progress", progress(0, 42))
            if MODE == "stall":
                return
            if MODE.startswith("fail"):
                later(path, 2000, "Progress",
                      progress(3, 0, error="org.freedesktop.portal.Error.NotAllowed",
                               error_message="needs new permissions"))
                if MODE == "fail":
                    later(path, 5000, "UpdateAvailable", commits("a", "b", "b"))
            else:
                later(path, 4000, "Progress", progress(2, 100))
        elif name == "Spawn":
            # Slow, as starting a new sandbox is, so repeated clicks overlap it.
            GLib.timeout_add(1000, lambda: inv.return_value(GLib.Variant("(u)", (4242,))) or False)
        else:
            inv.return_value(None)

    def prop(conn, sender, path, iface, name):
        log.write(f"get {iface}.{name}\n")
        return GLib.Variant("u", int(ENV.get("FAKE_PORTAL_VERSION", "2")))

    def acquired(conn, _name):
        conn.register_object("/org/freedesktop/portal/Flatpak", node.interfaces[0],
                             method, prop, None)

    loop = GLib.MainLoop()
    # Losing the name includes losing the bus, when the session around the
    # run ends: then there is no one left to serve.
    Gio.bus_own_name(Gio.BusType.SESSION, BUS, 0, acquired, None, lambda *_: loop.quit())
    loop.run()


def main():
    if sys.argv[1:2] == ["--serve"]:
        serve()
        return
    subprocess.Popen([sys.executable, __file__, "--serve"])
    bus = Gio.bus_get_sync(Gio.BusType.SESSION)
    for _ in range(50):
        if bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                         "NameHasOwner", GLib.Variant("(s)", (BUS,)), None, 0, -1).unpack()[0]:
            break
        time.sleep(0.1)
    # /.flatpak-info can't be created on the real root, so the sandbox gets a
    # tmpfs root with every top-level entry bound back in.
    info = os.path.join(GLib.get_tmp_dir(), f"fake-flatpak-info-{os.getpid()}")
    with open(info, "w") as f:
        f.write("[Application]\nname=com.nasledov.gtkhx\n")
    args = ["bwrap", "--tmpfs", "/"]
    for name in sorted(os.listdir("/")):
        path = "/" + name
        if os.path.islink(path):
            args += ["--symlink", os.readlink(path), path]
        elif os.path.isdir(path):
            args += ["--dev-bind", path, path]
    args += ["--ro-bind", info, "/.flatpak-info", ENV["GTKHX_REAL"]] + sys.argv[1:]
    os.execvp("bwrap", args)


if __name__ == "__main__":
    main()
