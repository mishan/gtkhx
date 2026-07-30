#!/bin/bash
# Reformat the project's hand-authored C sources to the .clang-format
# style at repo root. Every hand-authored C source is covered — the
# once-vendored sources that used to be excluded here are all gone
# (deleted or ported to the Rust crates under rust/).
#
# Usage:
#   tools/reformat.sh              # reformat all targeted files
#   tools/reformat.sh src/foo.c    # reformat a single file
#   tools/reformat.sh --check      # exit non-zero if anything would
#                                  # change; for CI use
#
# Pinned to clang-format 21.1.8. clang-format's output drifts between
# major versions (notably around braced-list macros), so install the
# exact version for reproducible results:  pip install 'clang-format==21.1.8'
# (needs 15+ at minimum for the InsertBraces option). CI (lint.yml) pins
# the same version.

set -euo pipefail

if ! command -v clang-format >/dev/null 2>&1; then
    echo "error: clang-format not found in PATH" >&2
    echo "  Pinned version (recommended): pip install 'clang-format==21.1.8'" >&2
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
#
# There are none right now: the once-vendored sources (HexChat's xtext,
# the in-tree crypto, the DFA regex engine, the Mac-format parsers) have
# all been deleted or ported to the Rust crates under rust/, and cicn.c
# is now a thin wrapper we own. So every hand-authored C source is
# reformatted. Add a basename here if genuinely-vendored C returns.
EXCLUDED_BASENAMES=(
)

# Files we reformat: src/*.c, src/*.h, tests/**/*.c, tests/**/*.h.
# Note we don't recurse into mhxd/ — that's an untracked local server
# clone used only as a test target, not part of the tree.
gather_targets () {
    local f
    while IFS= read -r f; do
        local base
        base=$(basename "$f")
        local skip=0
        # ${arr[@]+"${arr[@]}"} so an empty EXCLUDED_BASENAMES doesn't
        # trip `set -u` on older bash (e.g. macOS's 3.2).
        for ex in ${EXCLUDED_BASENAMES[@]+"${EXCLUDED_BASENAMES[@]}"}; do
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
