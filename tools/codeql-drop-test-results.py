#!/usr/bin/env python3
"""Drop CodeQL results that sit in test code from a SARIF file, in place.

CodeQL flags test fixtures the same way it flags shipping code: a fixed
key in a cipher round-trip test is a "hard-coded cryptographic value", a
deliberate double free in the integration harness is a "double free".
Dismissing those in the UI does not stick. An alert is matched to its
dismissal by a fingerprint of the surrounding code, so moving a crate or
editing near a test reopens the whole set under new alert numbers.
Filtering the SARIF before upload means test-code results never become
alerts at all.

Test code is:

  * anything under the top-level tests/ tree;
  * a Rust crate's tests/ and benches/ directories;
  * a Rust item gated by #[cfg(test)] (or cfg(all(test, ...))), whether
    inline (a `mod tests { ... }` block, a stub fn) or out of line
    (`mod tests;`, `#[path = "x_tests.rs"] mod x_tests;`), along with
    every module file declared from inside one.

Inline spans are found by indentation: rustfmt puts an item's closing
brace at the item's own indent, and CI enforces rustfmt, so no brace
matching is needed.

Usage: codeql-drop-test-results.py SARIF [SARIF ...]
Run from the repository root.
"""

import json
import os
import re
import sys

CFG_TEST = re.compile(r"#\[cfg\((test|all\(test\b)")
PATH_ATTR = re.compile(r'#\[path\s*=\s*"([^"]+)"\]')
MOD_DECL = re.compile(r"(?:pub(?:\([^)]*\))?\s+)?mod\s+(\w+)\s*;")
TEST_DIRS = re.compile(r"^(tests/|rust/crates/[^/]+/(tests|benches)/)")


def indent_of(line):
    return len(line) - len(line.lstrip(" "))


def module_file(parent, name, path_attr):
    """Resolve an out-of-line `mod name;` declared at the top of `parent`."""
    here = os.path.dirname(parent)
    if path_attr:
        return os.path.normpath(os.path.join(here, path_attr))
    stem = os.path.splitext(os.path.basename(parent))[0]
    base = here if stem in ("lib", "main", "mod") else os.path.join(here, stem)
    for cand in (os.path.join(base, name + ".rs"), os.path.join(base, name, "mod.rs")):
        if os.path.exists(cand):
            return os.path.normpath(cand)
    return None


def mod_decls(path, lines, lo, hi):
    """Out-of-line module files declared at the top level of lines[lo:hi]."""
    found = []
    path_attr = None
    for line in lines[lo:hi]:
        m = PATH_ATTR.search(line)
        if m:
            path_attr = m.group(1)
            continue
        m = MOD_DECL.match(line.strip())
        if m and indent_of(line) == 0:
            f = module_file(path, m.group(1), path_attr)
            if f:
                found.append(f)
        if not line.strip().startswith("#["):
            path_attr = None
    return found


def scan_rust(path):
    """Return (inline test spans as 1-based inclusive line ranges, test module files)."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().split("\n")
    spans, files = [], []
    i = 0
    while i < len(lines):
        if not CFG_TEST.match(lines[i].strip()):
            i += 1
            continue
        ind = " " * indent_of(lines[i])
        start = i
        path_attr = None
        j = i + 1
        while j < len(lines) and lines[j].strip().startswith("#["):
            m = PATH_ATTR.search(lines[j])
            if m:
                path_attr = m.group(1)
            j += 1
        if j >= len(lines):
            break
        m = MOD_DECL.match(lines[j].strip())
        if m:
            if not ind:
                f = module_file(path, m.group(1), path_attr)
                if f:
                    files.append(f)
            spans.append((start + 1, j + 1))
            i = j + 1
            continue
        # Find where the item's header ends: a `;` item is one statement, a
        # `{` item runs to the closing brace at the item's own indent.
        k = j
        while k < len(lines) and not lines[k].rstrip().endswith(("{", ";")):
            k += 1
        if k >= len(lines):
            break
        if lines[k].rstrip().endswith("{"):
            k += 1
            while k < len(lines) and lines[k].rstrip() not in (ind + "}", ind + "};"):
                k += 1
            files.extend(mod_decls(path, lines, j + 1, k))
        spans.append((start + 1, k + 1))
        i = k + 1
    return spans, files


def collect_rust(root):
    spans = {}
    test_files = set()
    pending = []
    for dirpath, _, names in os.walk(root):
        for name in names:
            if name.endswith(".rs"):
                p = os.path.normpath(os.path.join(dirpath, name))
                s, f = scan_rust(p)
                if s:
                    spans[p] = s
                pending.extend(f)
    # A module file reached from a test module is test code, and so is
    # everything it declares in turn.
    while pending:
        f = pending.pop()
        if f in test_files or not os.path.exists(f):
            continue
        test_files.add(f)
        with open(f, encoding="utf-8", errors="replace") as fh:
            lines = fh.read().split("\n")
        pending.extend(mod_decls(f, lines, 0, len(lines)))
    return spans, test_files


def is_test(uri, line, spans, test_files):
    path = os.path.normpath(uri)
    if TEST_DIRS.match(path) or path in test_files:
        return True
    return any(lo <= line <= hi for lo, hi in spans.get(path, ()))


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    spans, test_files = collect_rust("rust")
    for sarif in argv[1:]:
        with open(sarif, encoding="utf-8") as fh:
            doc = json.load(fh)
        for run in doc.get("runs", []):
            kept, dropped = [], 0
            for res in run.get("results", []):
                loc = (res.get("locations") or [{}])[0].get("physicalLocation", {})
                uri = loc.get("artifactLocation", {}).get("uri", "")
                line = loc.get("region", {}).get("startLine", 0)
                if uri and is_test(uri, line, spans, test_files):
                    dropped += 1
                else:
                    kept.append(res)
            run["results"] = kept
            print(f"{sarif}: kept {len(kept)}, dropped {dropped} in test code")
        with open(sarif, "w", encoding="utf-8") as fh:
            json.dump(doc, fh)


if __name__ == "__main__":
    main(sys.argv)
