#!/usr/bin/env python3
"""
Generate a .po file from a .pot template and a translation dictionary.

Usage:
    generate_po.py <pot_path> <output_path> <lang_code> <translator_name> \
                   <language_name> <team_name> <encoding> <translations_module>

The translations module must expose a dict named TRANSLATIONS keyed by msgid.
Every entry in the emitted .po is marked `#, fuzzy` (per Misha's request) so
human translators know to review them.
"""
import importlib.util
import re
import sys
import textwrap
from datetime import datetime, timezone


_PO_UNESCAPE = {
    '\\n': '\n', '\\t': '\t', '\\r': '\r',
    '\\"': '"', '\\\\': '\\',
}


def po_unescape(s):
    """Decode .po-style backslash escapes to real characters."""
    out = []
    i = 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            seq = s[i:i+2]
            if seq in _PO_UNESCAPE:
                out.append(_PO_UNESCAPE[seq])
                i += 2
                continue
        out.append(s[i])
        i += 1
    return ''.join(out)


def po_escape(s):
    """Encode real characters as .po-style backslash escapes."""
    return (s.replace('\\', '\\\\')
             .replace('"', '\\"')
             .replace('\n', '\\n')
             .replace('\t', '\\t')
             .replace('\r', '\\r'))


def parse_pot(path):
    """Yield {refs, flags, comments, msgid, msgid_plural} for each entry.

    msgid and msgid_plural have escape sequences decoded — so consumers
    work with the real string (the same string a translator would type
    in a .po editor), not the on-disk escaped form."""
    entries = []
    current = None
    state = None
    with open(path, encoding='utf-8') as f:
        for raw in f:
            line = raw.rstrip('\n')
            stripped = line.strip()
            if not stripped:
                if current is not None and (current['msgid'] or current.get('header')):
                    entries.append(current)
                current = None
                state = None
                continue
            if current is None:
                current = {
                    'refs': [], 'flags': [], 'comments': [],
                    'msgid': '', 'msgid_plural': '',
                    'header': False,
                }
            if stripped.startswith('#:'):
                current['refs'].append(stripped[2:].strip())
            elif stripped.startswith('#,'):
                for flag in stripped[2:].split(','):
                    flag = flag.strip()
                    if flag and flag not in current['flags']:
                        current['flags'].append(flag)
            elif stripped.startswith('#'):
                current['comments'].append(stripped[1:].rstrip())
            elif stripped.startswith('msgid_plural'):
                m = re.match(r'msgid_plural\s+"(.*)"$', stripped)
                if m:
                    current['msgid_plural'] = m.group(1)
                    state = 'msgid_plural'
            elif stripped.startswith('msgid'):
                m = re.match(r'msgid\s+"(.*)"$', stripped)
                if m:
                    current['msgid'] = m.group(1)
                    state = 'msgid'
            elif stripped.startswith('msgstr'):
                state = None  # skip msgstrs in the .pot (they're empty)
            elif stripped.startswith('"') and state:
                m = re.match(r'"(.*)"$', stripped)
                if m:
                    current[state] += m.group(1)
    if current is not None and (current['msgid'] or current.get('header')):
        entries.append(current)
    # Decode escape sequences in msgid/msgid_plural so callers see real strings.
    for e in entries:
        e['msgid'] = po_unescape(e['msgid'])
        e['msgid_plural'] = po_unescape(e['msgid_plural'])
    return entries


def format_msg(keyword, text):
    """Format a msgid/msgstr field, splitting at embedded newlines.

    `text` is a real string (no .po escapes); we apply po_escape and
    then break at each escaped \\n so multi-line strings render with one
    line per source-line, matching xgettext's output style."""
    if not text:
        return f'{keyword} ""'
    escaped = po_escape(text)
    if '\\n' not in escaped:
        return f'{keyword} "{escaped}"'
    # Split keeping the \n on each piece except the last (which may be empty).
    parts = escaped.split('\\n')
    pieces = [p + '\\n' for p in parts[:-1]]
    if parts[-1]:
        pieces.append(parts[-1])
    lines = [f'{keyword} ""'] + [f'"{p}"' for p in pieces]
    return '\n'.join(lines)


def render_entry(entry, msgstr):
    """Render a single .po entry block."""
    lines = []
    for c in entry['comments']:
        lines.append('#' + c)
    for r in entry['refs']:
        lines.append('#: ' + r)
    flags = list(entry['flags'])
    if 'fuzzy' not in flags:
        flags.insert(0, 'fuzzy')
    lines.append('#, ' + ', '.join(flags))
    lines.append(format_msg('msgid', entry['msgid']))
    if entry['msgid_plural']:
        lines.append(format_msg('msgid_plural', entry['msgid_plural']))
    lines.append(format_msg('msgstr', msgstr))
    return '\n'.join(lines)


def render_header(lang_code, translator_name, language_name, team_name, encoding):
    """Render the .po header entry."""
    now = datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M+0000')
    fields = [
        f'Project-Id-Version: gtkhx\\n',
        f'Report-Msgid-Bugs-To: \\n',
        f'POT-Creation-Date: 2026-05-28 11:18-0700\\n',
        f'PO-Revision-Date: {now}\\n',
        f'Last-Translator: {translator_name}\\n',
        f'Language-Team: {team_name}\\n',
        f'Language: {lang_code}\\n',
        f'MIME-Version: 1.0\\n',
        f'Content-Type: text/plain; charset={encoding}\\n',
        f'Content-Transfer-Encoding: 8bit\\n',
    ]
    body_lines = [f'"{f}"' for f in fields]
    return '\n'.join([
        '# GtkHx ' + language_name + ' translation.',
        '# Copyright (C) 2026 Misha Nasledov',
        '# This file is distributed under the same license as the gtkhx package.',
        '# Auto-generated machine translation, marked fuzzy for human review.',
        '#',
        '#, fuzzy',
        'msgid ""',
        'msgstr ""',
        *body_lines,
    ])


def load_translations(path):
    spec = importlib.util.spec_from_file_location('translations', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.TRANSLATIONS


def main():
    if len(sys.argv) != 9:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    (_, pot_path, out_path, lang_code, translator_name,
     language_name, team_name, encoding, translations_path) = sys.argv

    entries = parse_pot(pot_path)
    translations = load_translations(translations_path)

    out_lines = []
    out_lines.append(render_header(
        lang_code, translator_name, language_name, team_name, encoding))

    missing = []
    found = 0
    for entry in entries:
        if not entry['msgid']:
            continue  # skip the empty-msgid header
        t = translations.get(entry['msgid'], '')
        if t:
            found += 1
        else:
            missing.append(entry['msgid'])
        out_lines.append('')
        out_lines.append(render_entry(entry, t))

    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(out_lines) + '\n')

    total = found + len(missing)
    print(f'{lang_code}: {found}/{total} translated, {len(missing)} missing',
          file=sys.stderr)
    if missing:
        print(f'  first missing: {missing[0]!r}', file=sys.stderr)


if __name__ == '__main__':
    main()
