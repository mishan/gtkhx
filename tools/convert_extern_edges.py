"""Convert Rust->Rust `extern "C"` declarations into real Cargo imports.

One-shot migration helper for docs/rust/crate-consolidation-scoping.md step 2.
Run from `rust/crates`. For each `extern "C" { ... }` block, any declaration
whose symbol is defined by a `#[no_mangle]` fn in *another workspace crate*
becomes a `use <crate>::<mod>::<sym>;`. Declarations that resolve to C stay in
the block verbatim. Any `#[cfg(...)]` attribute on the block is replicated onto
every generated `use`, so `#[cfg(test)]` doubles keep shadowing them.

Delete once the migration is finished.
"""
import re, collections, glob, sys

# gtkhx-ui -> hxchat-send -> hxuser-recv -> gtkhx-ui: Cargo forbids circular
# dependencies, so these three edges keep their C indirection until the crates
# merge (scoping doc step 3).
CYCLIC = {('gtkhx-ui', 'hxchat-send'),
          ('hxchat-send', 'hxuser-recv'),
          ('hxuser-recv', 'gtkhx-ui')}

# One level of paren nesting, so `#[cfg(not(test))]` is captured whole.
BLOCK = re.compile(r'((?:#\[cfg\((?:[^()]|\([^()]*\))*\)\]\n)?)extern\s+"C"\s*\{([\s\S]*?)\n\}')
NOMANGLE = re.compile(r'#\[(?:unsafe\()?no_mangle\)?\][\s\S]{0,200}?\bfn\s+([A-Za-z0-9_]+)')


def load_defs():
    """symbol -> (crate, module_path)"""
    d = {}
    for f in glob.glob('*/src/**/*.rs', recursive=True):
        crate, rel = f.split('/', 1)
        rel = rel[len('src/'):]
        mod = '' if rel == 'lib.rs' else re.sub(r'(/mod)?\.rs$', '', rel).replace('/', '::')
        for m in NOMANGLE.finditer(open(f).read()):
            d[m.group(1)] = (crate, mod)
    return d


def split_items(body):
    """Yield (fn_name_or_None, verbatim_text) per declaration in a block."""
    items, buf, depth, cur = [], [], 0, None
    for ln in body.split('\n'):
        buf.append(ln)
        if cur is None:
            m = re.search(r'\bfn\s+([A-Za-z0-9_]+)', ln)
            if m:
                cur = m.group(1)
        if cur:
            depth += ln.count('(') - ln.count(')')
            if depth <= 0 and ln.rstrip().endswith(';'):
                items.append((cur, '\n'.join(buf)))
                buf, cur, depth = [], None, 0
    if buf and any(x.strip() for x in buf):
        items.append((None, '\n'.join(buf)))
    return items


def convert(path, defs, needpub):
    crate = path.split('/')[0]
    src = open(path).read()
    out, hits = src, []
    for blk in list(BLOCK.finditer(src)):
        cfg, body = blk.group(1), blk.group(2)
        use, keep = collections.defaultdict(list), []
        for name, text in split_items(body):
            tgt = defs.get(name) if name else None
            if tgt and tgt[0] != crate and (crate, tgt[0]) not in CYCLIC:
                use[tgt].append(name)
            else:
                keep.append(text)
        if not use:
            continue
        parts = []
        for (oc, om) in sorted(use):
            syms = sorted(set(use[(oc, om)]))
            path_ = oc.replace('-', '_') + (('::' + om) if om else '')
            if om:
                needpub.add((oc, om))
            inner = syms[0] if len(syms) == 1 else '{' + ', '.join(syms) + '}'
            parts.append(f"{cfg}use {path_}::{inner};")
        rest = ''
        if any(x.strip() for x in keep):
            rest = '\n\n' + cfg + 'extern "C" {' + '\n'.join(keep).rstrip() + '\n}'
        out = out.replace(blk.group(0), '\n'.join(parts) + rest, 1)
        hits.append(([f"{a}::{b}" if b else a for a, b in sorted(use)],
                     len([k for k in keep if k.strip()])))
    if hits:
        open(path, 'w').write(out)
    return hits


def main(paths):
    defs, needpub = load_defs(), set()
    for p in paths:
        for targets, nkept in convert(p, defs, needpub):
            print(f"  {p}: -> {', '.join(targets)}   (kept {nkept} C decls)")
    for crate, mod in sorted(needpub):
        lib, top = f"{crate}/src/lib.rs", mod.split('::')[0]
        s = open(lib).read()
        if re.search(rf'^pub mod {top};', s, re.M):
            continue
        if re.search(rf'^mod {top};', s, re.M):
            open(lib, 'w').write(re.sub(rf'^mod {top};', f'pub mod {top};', s, count=1, flags=re.M))
            print(f"  {crate}: mod {top} -> pub mod")
        else:
            print(f"  !! {crate}: could not find `mod {top};` in lib.rs")


if __name__ == '__main__':
    main(sys.argv[1:])
