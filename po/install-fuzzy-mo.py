#!/usr/bin/env python3
"""Post-install: build each language's .mo with --use-fuzzy.

meson's i18n.gettext() helper invokes msgfmt with no extra flags, and
msgfmt strips every `#, fuzzy` entry from the binary catalog by
default. Our seed de/es/fr/pt translations are entirely fuzzy by
design — they're machine-generated and waiting on human review — so
that default behavior produces empty .mo files (just a header) and
zero translations ship.

This script runs after meson's regular install step and writes each
.mo with `msgfmt --use-fuzzy` instead, overwriting whatever the
default install path produced. Source .po files keep their `#, fuzzy`
markers so a future human translator can find the un-reviewed entries
with `msggrep --msgstr -K -e '' <lang>.po` or any standard PO editor.

When the seeds are reviewed and the markers removed, drop this script
and revert po/meson.build to a plain `i18n.gettext(...)` call.
"""
import os
import shutil
import subprocess
import sys


def main():
    if len(sys.argv) < 3:
        print("usage: install-fuzzy-mo.py <package> <po_source_dir>",
              file=sys.stderr)
        return 1
    package, po_dir = sys.argv[1], sys.argv[2]

    # MESON_INSTALL_DESTDIR_PREFIX is the prefix with DESTDIR already
    # prepended — the right base for any file we write to disk during
    # install. Fall back to MESON_INSTALL_PREFIX for older mesons /
    # ad-hoc invocations.
    install_prefix = os.environ.get('MESON_INSTALL_DESTDIR_PREFIX')
    if not install_prefix:
        install_prefix = os.environ.get('MESON_INSTALL_PREFIX', '/usr/local')

    msgfmt = shutil.which('msgfmt')
    if not msgfmt:
        print("install-fuzzy-mo: msgfmt not on PATH; skipping fuzzy rebuild",
              file=sys.stderr)
        return 0

    linguas_path = os.path.join(po_dir, 'LINGUAS')
    try:
        with open(linguas_path, encoding='utf-8') as f:
            linguas = [line.strip() for line in f
                       if line.strip() and not line.startswith('#')]
    except OSError as exc:
        print(f"install-fuzzy-mo: can't read LINGUAS ({exc}); skipping",
              file=sys.stderr)
        return 0

    for lang in linguas:
        po = os.path.join(po_dir, f'{lang}.po')
        if not os.path.exists(po):
            print(f"install-fuzzy-mo: {po} missing; skipping {lang}",
                  file=sys.stderr)
            continue
        mo_dir = os.path.join(install_prefix, 'share', 'locale',
                              lang, 'LC_MESSAGES')
        os.makedirs(mo_dir, exist_ok=True)
        mo = os.path.join(mo_dir, f'{package}.mo')
        subprocess.check_call([msgfmt, '--use-fuzzy', '-o', mo, po])
        print(f"install-fuzzy-mo: wrote {mo} (fuzzy entries included)")

    return 0


if __name__ == '__main__':
    sys.exit(main())
