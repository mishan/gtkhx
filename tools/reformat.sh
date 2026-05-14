#!/bin/bash
# Reformat the project's hand-authored C sources to the .clang-format
# style at repo root. Vendored sources (HexChat's xtext, mhxd, Mac-
# format parsers, in-tree crypto, etc.) are deliberately excluded so
# we can keep tracking their upstreams without conflicts.
#
# Usage:
#   tools/reformat.sh              # reformat all targeted files
#   tools/reformat.sh src/foo.c    # reformat a single file
#   tools/reformat.sh --check      # exit non-zero if anything would
#                                  # change; for CI use
#
# Requires clang-format 15+ (for the InsertBraces option).

set -euo pipefail

if ! command -v clang-format >/dev/null 2>&1; then
    echo "error: clang-format not found in PATH" >&2
    echo "  Debian / Ubuntu: sudo apt install clang-format" >&2
    echo "  Fedora:          sudo dnf install clang-tools-extra" >&2
    echo "  macOS:           brew install clang-format" >&2
    exit 1
fi

# Files we DO NOT reformat. These are either vendored from external
# projects (where we want to track upstream whitespace) or
# format-sensitive in a way that would create churn for no benefit.
#
# Path patterns (basename matching). Edit here when adding new
# vendored files.
EXCLUDED_BASENAMES=(
    # HexChat's xtext widget — vendored, follow upstream.
    "xtext.c"
    "xtext.h"
    "xtextbuf.h"
    # In-tree crypto. Slated for replacement by Nettle / GLib hashes
    # per the roadmap; no point churning the legacy code first.
    "md5.c"   "md5.h"
    "sha.c"   "sha.h"
    "haval.c" "haval.h"
    "hmac.c"  "hmac.h"
    "rand.c"
    "cipher.c"
    "cipher_openssl.h"
    # Mac-format parsers, copied from external implementations.
    "hfs.c"     "hfs.h"
    "macres.c"  "macres.h"
    "cicn.c"    "cicn.h"
    # DFA regex engine — large external code with intricate
    # formatting; reformat would obscure the structure.
    "dfa.c"
)

# Files we reformat: src/*.c, src/*.h, tests/**/*.c, tests/**/*.h.
# Note we don't recurse into mhxd/ — that's the vendored server.
gather_targets () {
    local f
    while IFS= read -r f; do
        local base
        base=$(basename "$f")
        local skip=0
        for ex in "${EXCLUDED_BASENAMES[@]}"; do
            if [[ "$base" == "$ex" ]]; then
                skip=1
                break
            fi
        done
        [[ $skip -eq 0 ]] && printf '%s\n' "$f"
    done < <(
        find src -maxdepth 1 -type f \( -name '*.c' -o -name '*.h' \)
        find tests -type f \( -name '*.c' -o -name '*.h' \) 2>/dev/null || true
    )
}

MODE=apply
if [[ ${1:-} == "--check" ]]; then
    MODE=check
    shift
fi

if [[ $# -gt 0 ]]; then
    # Specific files.
    TARGETS=("$@")
else
    mapfile -t TARGETS < <(gather_targets)
fi

if [[ $MODE == apply ]]; then
    printf 'reformatting %d files...\n' "${#TARGETS[@]}"
    clang-format -i "${TARGETS[@]}"
    printf 'done.\n'
else
    printf 'checking %d files...\n' "${#TARGETS[@]}"
    if clang-format --dry-run -Werror "${TARGETS[@]}"; then
        printf 'all formatted.\n'
    else
        printf 'one or more files would change. run without --check to fix.\n' >&2
        exit 1
    fi
fi
