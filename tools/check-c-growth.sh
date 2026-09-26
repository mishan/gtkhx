#!/bin/bash
# Fail if the branch adds more lines of C to src/ than it removes.
#
# The port only moves forward if C stops growing. Feature work lands in
# whichever language the code it touches is in, and for a while that meant
# new features went into the very C files that were next in line to port —
# each one making the eventual port bigger. This check makes that visible at
# review time instead of in a line count months later.
#
# Usage: tools/check-c-growth.sh [base-ref]
#
# Compares the working tree (tracked and untracked, not ignored) against the
# merge base of HEAD and base-ref (default: origin/main, falling back to main).
# Only src/*.c and src/*.h count; tests/ is free to grow.
#
# When growth is the right call — a new bridge shim that lets a larger
# port land, say — say why in the commit message with a trailer:
#
#   C-Growth: <reason>
#
# and the check passes, printing the reason. It is a reviewer's decision, so it
# belongs where the reviewer reads.
set -euo pipefail
cd "$(dirname "$0")/.."
export LC_ALL=C  # join needs the same collation sort used

base_ref=${1:-}
if [ -z "$base_ref" ]; then
    if git rev-parse -q --verify origin/main >/dev/null; then
        base_ref=origin/main
    else
        base_ref=main
    fi
fi

base=$(git merge-base HEAD "$base_ref")
paths=('src/*.c' 'src/*.h')

# `git grep -c ''` prints path:lines for every file; it reads a tree-ish
# directly, so the base side needs no checkout.
count_base() { git grep -c '' "$base" -- "${paths[@]}" | sed "s|^$base:||"; }
count_tree() { git grep --untracked -c '' -- "${paths[@]}"; }

report=$(join -t: -a1 -a2 -e0 -o 0,1.2,2.2 \
    <(count_base | sort -t: -k1,1) <(count_tree | sort -t: -k1,1))

read -r before after < <(awk -F: '{ b += $2; a += $3 } END { print b, a }' <<<"$report")
delta=$((after - before))

echo "C in src/: $before -> $after lines ($(printf '%+d' "$delta")) since ${base:0:12}"

if [ "$delta" -le 0 ]; then
    exit 0
fi

echo
echo "Files that grew:"
awk -F: '$3 > $2 { print $3 - $2, $1 }' <<<"$report" | sort -rn |
    awk '{ printf "  %+6d  %s\n", $1, $2 }'

reason=$(git log --format=%B "$base..HEAD" |
    sed -n 's/^C-Growth:[[:space:]]*//p' | head -n1)
if [ -n "$reason" ]; then
    echo
    echo "Allowed by C-Growth trailer: $reason"
    exit 0
fi

cat >&2 <<'EOF'

This branch adds C to src/. Port the content being changed to Rust first, or
take the growth out of the C side. If growth is genuinely the right call, add
a trailer to the commit message saying why:

  C-Growth: <reason>

See "How the rest of the port gets done" in docs/rust/ROADMAP.md.
EOF
exit 1
