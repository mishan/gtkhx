#!/usr/bin/env python3
"""The parts of the Rotulus crate that C needs, found through cargo.

The chat view comes from crates.io, and its crate carries the C header
and the translations of its own strings. Rather than keep copies here,
the build asks cargo where the locked version of the crate lives, which
works the same online and in the Flatpak's offline, vendored build.

  rotulus.py dir CARGO MANIFEST
      Print the crate's directory.
  rotulus.py include-dir CRATE_DIR SOURCE_ROOT BASE
      Print the crate's include/ for include_directories() in BASE. Meson
      refuses an absolute path into the source tree, which it is when
      CARGO_HOME lives in the checkout, as it does on CI; there it is
      relative to BASE instead.
  rotulus.py locale CRATE_DIR OUT_DIR STAMP
      Compile the crate's po/ into OUT_DIR/<lang>/LC_MESSAGES/rotulus.mo.
  rotulus.py install-locale OUT_DIR LOCALEDIR
      Install those, under $MESON_INSTALL_DESTDIR_PREFIX (a meson install
      script).
"""

import json
import os
import shutil
import subprocess
import sys


def crate_dir(cargo, manifest):
    out = subprocess.run(
        [cargo, "metadata", "--format-version", "1", "--locked",
         "--manifest-path", manifest],
        # The JSON is UTF-8, whatever the locale's encoding (cp1252 on
        # Windows) says.
        check=True, capture_output=True, encoding="utf-8",
    ).stdout
    for pkg in json.loads(out)["packages"]:
        if pkg["name"] == "rotulus":
            return os.path.dirname(pkg["manifest_path"])
    sys.exit("rotulus is not in the dependency graph")


def include_dir(crate, root, base):
    inc = os.path.abspath(os.path.join(crate, "include"))
    root = os.path.abspath(root)
    try:
        if os.path.commonpath([inc, root]) == root:
            return os.path.relpath(inc, os.path.abspath(base))
    except ValueError:
        pass  # different drives on Windows: never inside
    return inc


def languages(po_dir):
    with open(os.path.join(po_dir, "LINGUAS")) as f:
        return [l.strip() for l in f if l.strip() and not l.startswith("#")]


def compile_locale(crate, out, stamp):
    po_dir = os.path.join(crate, "po")
    for lang in languages(po_dir):
        dest = os.path.join(out, lang, "LC_MESSAGES")
        os.makedirs(dest, exist_ok=True)
        subprocess.run(
            ["msgfmt", "--check-format", "-o", os.path.join(dest, "rotulus.mo"),
             os.path.join(po_dir, lang + ".po")],
            check=True,
        )
    with open(stamp, "w"):
        pass


def install_locale(out, localedir):
    prefix = os.environ["MESON_INSTALL_DESTDIR_PREFIX"]
    target = os.path.join(prefix, localedir)
    for lang in sorted(os.listdir(out)):
        src = os.path.join(out, lang, "LC_MESSAGES", "rotulus.mo")
        dest = os.path.join(target, lang, "LC_MESSAGES")
        os.makedirs(dest, exist_ok=True)
        shutil.copyfile(src, os.path.join(dest, "rotulus.mo"))


if __name__ == "__main__":
    cmd, *args = sys.argv[1:]
    if cmd == "dir":
        print(crate_dir(*args))
    elif cmd == "include-dir":
        print(include_dir(*args))
    elif cmd == "locale":
        compile_locale(*args)
    elif cmd == "install-locale":
        install_locale(*args)
    else:
        sys.exit(f"unknown command {cmd}")
