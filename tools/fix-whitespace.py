#!/usr/bin/env python3
"""Whitespace hygiene for the whole tree: no tab indentation, no trailing
whitespace, exactly one final newline.

For every tracked *text* file this tool:

  * expands leading (indentation) tabs to spaces at TAB_WIDTH,
  * strips trailing whitespace from every line,
  * ensures the file ends with exactly one newline.

Only *leading* tabs are converted. Tabs elsewhere on a line (inside a
string literal, a config value, a man-page macro argument) are left
untouched, because rewriting them can change what the program or data
means. That keeps the transform safe to run blindly across shell,
Python, Meson, YAML, Markdown, roff, and config fixtures.

C is additionally kept fully tab-free and brace-normalised by
clang-format (see tools/reformat.sh); Rust by `cargo fmt`. This tool is
the cross-language backstop that those two don't cover, and the shared
"no tab indentation / no trailing whitespace / final newline" gate CI
enforces on everything.

Excluded: binary files (detected by a NUL byte), Makefiles and *.mk
(GNU make requires literal tabs), and *.rsrc Mac resource blobs.

Usage:
    tools/fix-whitespace.py             # fix every tracked file in place
    tools/fix-whitespace.py --check     # exit 1 if any file would change
    tools/fix-whitespace.py FILE...     # limit to the given paths
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

TAB_WIDTH = 4

# Basenames whose tabs are structural / required.
EXCLUDE_BASENAMES = {"Makefile", "GNUmakefile", "makefile"}
# Suffixes to skip: GNU make fragments, and Mac resource blobs (binary).
EXCLUDE_SUFFIXES = (".mk", ".rsrc")
# Directories holding captured input for tests. Their bytes are the fixture —
# tidying them is changing the thing under test. The settings-migration corpus
# is the live example: a real gtkhxrc whose TIMESTAMPFORMAT value ends in a
# space, which is exactly what a test asserts survives the migration, because
# GKeyFile did not escape a trailing space and a reader that trimmed would jam
# the timestamp against the message text.
EXCLUDE_DIRS = ("rust/crates/hxconfig/fixtures/",)


def tracked_files() -> list[str]:
    out = subprocess.run(
        ["git", "ls-files", "-z"], capture_output=True, check=True
    ).stdout
    return [f for f in out.decode().split("\0") if f]


def is_excluded(path: str) -> bool:
    base = os.path.basename(path)
    if base in EXCLUDE_BASENAMES:
        return True
    if path.startswith(EXCLUDE_DIRS):
        return True
    return path.endswith(EXCLUDE_SUFFIXES)


def fix_bytes(data: bytes) -> bytes | None:
    """Return the cleaned bytes, or None if the file should be skipped
    (binary or not valid UTF-8)."""
    if b"\x00" in data:
        return None
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return None

    lines = text.split("\n")
    cleaned = []
    for line in lines:
        # Split off the run of leading whitespace, expand its tabs to
        # spaces, and leave the remainder of the line verbatim.
        i = 0
        while i < len(line) and line[i] in " \t":
            i += 1
        line = line[:i].expandtabs(TAB_WIDTH) + line[i:]
        cleaned.append(line.rstrip(" \t"))

    # Collapse any trailing blank lines and guarantee exactly one final
    # newline (an empty file stays empty).
    while len(cleaned) > 1 and cleaned[-1] == "":
        cleaned.pop()
    result = "\n".join(cleaned)
    if result:
        result += "\n"
    return result.encode("utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="report offenders and exit 1 without modifying files")
    ap.add_argument("files", nargs="*",
                    help="limit to these paths (default: all tracked files)")
    args = ap.parse_args()

    files = args.files or tracked_files()
    offenders = []
    for path in files:
        if is_excluded(path):
            continue
        if not os.path.isfile(path) or os.path.islink(path):
            continue
        with open(path, "rb") as fh:
            data = fh.read()
        fixed = fix_bytes(data)
        if fixed is None or fixed == data:
            continue
        offenders.append(path)
        if not args.check:
            with open(path, "wb") as fh:
                fh.write(fixed)

    if args.check:
        if offenders:
            sys.stderr.write(
                "whitespace check failed; these files have tab indentation, "
                "trailing whitespace, or a missing/extra final newline:\n")
            for path in offenders:
                sys.stderr.write(f"  {path}\n")
            sys.stderr.write("\nrun tools/fix-whitespace.py to fix.\n")
            return 1
        print("whitespace: all files clean.")
        return 0

    if offenders:
        print(f"whitespace: fixed {len(offenders)} file(s).")
        for path in offenders:
            print(f"  {path}")
    else:
        print("whitespace: nothing to fix.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
