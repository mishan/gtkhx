#!/usr/bin/env python3
# Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or (at
# your option) any later version.
#
# Generator for rust/crates/hotline-proto/src/emoji_table.rs.
#
# Emoji shortcodes (the :joy: convention) are not standardised by Unicode.
# The de-facto names come from GitHub's "gemoji" set plus the Slack
# additions. The Python `emoji` package vendors exactly that material: each
# entry carries a CLDR English name (`en`, e.g. ":face_with_tears_of_joy:")
# plus the gemoji/Slack-style aliases (`alias`, e.g. [":joy:"]). We treat
# the gemoji/Slack aliases as canonical for ENCODE (they're what people
# actually type) and accept every grammar-valid name for DECODE.
#
# This generator is run by hand and its output (emoji_table.rs) is checked
# in, mirroring how src/text.rs's Mac Roman table was generated once from
# iconv and committed. Re-run after bumping the `emoji` package:
#
#     pip install emoji --break-system-packages
#     python3 tools/gen_emoji_table.py > rust/crates/hotline-proto/src/emoji_table.rs
#
# Shortcode grammar (must match the scanner in emoji.rs): [a-z0-9_+-]+
# Names that don't fit (uppercase, parentheses, accented CLDR names) are
# dropped — the alias usually covers the same emoji with a clean name.

import re
import sys

import emoji

GRAMMAR = re.compile(r"^[a-z0-9_+-]+$")


def inner(code):
    """Strip the surrounding colons from a :shortcode: token."""
    if code and code.startswith(":") and code.endswith(":"):
        return code[1:-1]
    return code


def rs_str(s):
    """Emit a Rust string literal for an arbitrary str (escape \\ and ")."""
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    data = emoji.EMOJI_DATA

    # ENCODE: emoji cluster -> one canonical shortcode (no colons).
    # Only fully-qualified (status 2) emoji are encode sources; we never
    # want to emit a minimally-/un-qualified form onto the wire.
    encode = {}          # emoji_str -> shortcode
    # DECODE: shortcode (no colons) -> emoji cluster. Permissive: every
    # grammar-valid name from any qualification level, first-wins with
    # fully-qualified preferred.
    decode = {}          # name -> (emoji_str, status)

    # Per-emoji grammar-valid names (aliases first, then the lowercased CLDR
    # `en`), keyed by emoji cluster, recorded alongside the qualification.
    names_by_emoji = {}  # ch -> (status, [names])

    for ch, e in data.items():
        status = e.get("status")
        aliases = [n for n in (inner(a) for a in (e.get("alias") or []))
                   if GRAMMAR.match(n)]
        en = inner(e["en"]).lower() if e.get("en") else None
        en = en if (en and GRAMMAR.match(en)) else None
        names = aliases + ([en] if en else [])
        if names:
            names_by_emoji[ch] = (status, names)

    # PASS 1 — build DECODE. Permissive: every grammar-valid name, first-wins
    # with fully-qualified (status 2) preferred, then 3, 4, 1. A name that
    # several emoji share (e.g. "umbrella" for both ☂ and ☔) resolves to the
    # first emoji that claims it here.
    for want_status in (2, 3, 4, 1):
        for ch, (status, names) in names_by_emoji.items():
            if status != want_status:
                continue
            for n in names:
                if n not in decode:
                    decode[n] = (ch, want_status)

    # PASS 2 — build ENCODE (fully-qualified emoji only). The canonical
    # shortcode must satisfy two things:
    #   1. round-trip: it has to DECODE back to THIS emoji, so we only
    #      consider names whose decode owner is `ch`. This avoids the
    #      collision where ☔'s shortest name "umbrella" actually decodes to
    #      ☂ (which claimed it first) — ☔ instead gets its next shortest
    #      uniquely-owned name, "umbrella_with_rain_drops".
    #   2. brevity: among the round-trip-safe names, pick the SHORTEST,
    #      tie-broken alphabetically for determinism. (The `emoji` package's
    #      alias/en split isn't "short vs verbose" — e.g. ⏭️'s short name
    #      ":next_track_button:" is the `en` while its only alias is the
    #      53-char ":black_right_pointing_double_triangle_with_vertical_bar:"
    #      — so we minimise over the union.)
    # An emoji all of whose names were claimed by others is left out of
    # ENCODE entirely (it stays a literal emoji on the wire — i.e. the
    # pre-feature '?' fallback on Mac Roman); vanishingly rare.
    for ch, (status, names) in names_by_emoji.items():
        if status != 2:
            continue
        own = [n for n in names if decode.get(n, (None,))[0] == ch]
        if own:
            encode[ch] = min(own, key=lambda nm: (len(nm), nm))

    # Sort ENCODE by emoji-cluster bytes for binary_search in Rust.
    enc_rows = sorted(encode.items(), key=lambda kv: kv[0])
    # Longest cluster (in chars) the scanner must try when matching.
    max_enc_chars = max((len(ch) for ch, _ in enc_rows), default=1)

    # Sort DECODE by shortcode for binary_search in Rust.
    dec_rows = sorted((name, em) for name, (em, _st) in decode.items())

    out = sys.stdout.write
    out("// @generated by tools/gen_emoji_table.py — DO NOT EDIT BY HAND.\n")
    out("//\n")
    out("// Source: the Python `emoji` package v%s (CLDR English names +\n"
        % emoji.__version__)
    out("// bundled GitHub gemoji / Slack aliases). Regenerate with:\n")
    out("//   python3 tools/gen_emoji_table.py > "
        "rust/crates/hotline-proto/src/emoji_table.rs\n")
    out("//\n")
    out("// License/provenance: the `emoji` package is New BSD (BSD-3-Clause);\n")
    out("// the underlying CLDR / GitHub-gemoji shortcode names are permissively\n")
    out("// licensed. This generated table of names is therefore distributable\n")
    out("// under GtkHx's GPL-2.0-or-later.\n")
    out("//\n")
    out("// ENCODE: emoji cluster -> canonical :shortcode: (colons added by\n")
    out("// the caller), fully-qualified emoji only, sorted by cluster bytes.\n")
    out("// DECODE: :shortcode: name -> emoji cluster, every grammar-valid\n")
    out("// name from any qualification, sorted by name. Grammar: [a-z0-9_+-]+\n")
    out("\n")
    out("/// Longest emoji cluster (in `char`s) present in [`ENCODE`]; the\n")
    out("/// encode scanner tries candidate lengths from this down to 1.\n")
    out("pub const MAX_ENCODE_CHARS: usize = %d;\n\n" % max_enc_chars)

    out("/// (emoji cluster, canonical shortcode without colons), "
        "sorted by cluster.\n")
    out("pub const ENCODE: &[(&str, &str)] = &[\n")
    for ch, name in enc_rows:
        out("    (%s, %s),\n" % (rs_str(ch), rs_str(name)))
    out("];\n\n")

    out("/// (shortcode without colons, emoji cluster), sorted by shortcode.\n")
    out("pub const DECODE: &[(&str, &str)] = &[\n")
    for name, ch in dec_rows:
        out("    (%s, %s),\n" % (rs_str(name), rs_str(ch)))
    out("];\n")


if __name__ == "__main__":
    main()
