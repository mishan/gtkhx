#!/bin/bash
#
# tools/analyze.sh — run the same sanitizer + static-analyzer
# pipeline that .github/workflows/analyze.yml runs in CI, but
# locally. Useful for catching regressions before pushing.
#
# Each step uses its own build directory so they don't interfere
# with the regular `meson setup build` you're using for development.
#
# Usage:
#   tools/analyze.sh           # run all three (sanitize, analyzer, tidy)
#   tools/analyze.sh sanitize  # just ASan/UBSan
#   tools/analyze.sh analyzer  # just gcc -fanalyzer
#   tools/analyze.sh tidy      # just clang-tidy
#
# Build dirs created:
#   build-asan/       # ASan + UBSan
#   build-analyzer/   # GCC -fanalyzer
#   build-tidy/       # compile_commands.json for clang-tidy
#
# Each leaves a summary file (analyzer-summary.txt /
# clang-tidy-summary.txt) for the eye-the-trend pass.

set -e
cd "$(dirname "$0")/.."

run_sanitize() {
    echo "=== sanitize: ASan + UBSan ==="
    if [ ! -d build-asan ]; then
        meson setup build-asan \
            -Db_sanitize=address,undefined \
            -Db_lundef=false \
            -Dbuildtype=debug
    fi
    meson compile -C build-asan
    ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:print_summary=1 \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0 \
        meson test -C build-asan --suite unit --suite proto \
            --print-errorlogs
}

run_analyzer() {
    echo "=== gcc-analyzer: -fanalyzer ==="
    if [ ! -d build-analyzer ]; then
        meson setup build-analyzer \
            -Dc_args='-fanalyzer -Wno-analyzer-too-complex'
    fi
    # Tee to a log AND a summary file. Don't abort on warnings —
    # -fanalyzer fires false positives + accumulates known noise we
    # haven't paid down yet.
    meson compile -C build-analyzer 2>&1 \
        | tee build-analyzer/analyzer.log
    {
        echo "=== -fanalyzer warning summary ==="
        grep -oE '\[-W[a-z-]+\]' build-analyzer/analyzer.log \
            | sort | uniq -c | sort -rn
    } > build-analyzer/analyzer-summary.txt || true
    echo
    cat build-analyzer/analyzer-summary.txt
}

run_tidy() {
    echo "=== clang-tidy ==="
    if ! command -v run-clang-tidy >/dev/null; then
        echo "run-clang-tidy not found — install clang-tools-extra (Fedora)" \
            "or clang-tidy + run-clang-tidy (Debian)."
        return 1
    fi
    if [ ! -d build-tidy ]; then
        meson setup build-tidy
    fi
    # Negative lookahead skips src/dfa.c (vendored GNU regex —
    # slated for replacement with GRegex/PCRE2 per
    # docs/analyze-triage.md). Python re supports (?!…) so
    # run-clang-tidy's pattern arg accepts it.
    set +e
    run-clang-tidy -p build-tidy -j "$(nproc)" \
        -extra-arg=-Wno-unknown-warning-option \
        'src/(?!dfa\.c$).*\.c$' 2>&1 \
        | tee build-tidy/clang-tidy.log
    set -e
    {
        echo "=== clang-tidy findings summary ==="
        grep -oE '\[[a-z-]+-[a-z][a-z0-9-]*\]$' \
            build-tidy/clang-tidy.log \
            | sort | uniq -c | sort -rn
    } > build-tidy/clang-tidy-summary.txt || true
    echo
    cat build-tidy/clang-tidy-summary.txt
}

case "${1:-all}" in
    sanitize) run_sanitize ;;
    analyzer) run_analyzer ;;
    tidy)     run_tidy ;;
    all)      run_sanitize && run_analyzer && run_tidy ;;
    *)
        echo "usage: $0 [sanitize|analyzer|tidy|all]"
        exit 1
        ;;
esac
