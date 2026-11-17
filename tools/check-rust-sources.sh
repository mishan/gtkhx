#!/bin/sh
# Fail when a Rust source the build compiles is missing from rust_sources in
# rust/meson.build. Meson re-runs cargo only when a listed file changes, so an
# unlisted one leaves an incremental build linking its old code.
#
# Counted: every crate's Cargo.toml and src/**/*.rs, except hx-e2e (tests
# only) and test modules (tests.rs, *_tests.rs, doubles.rs, anything under a
# tests/ directory), which the library never compiles.
set -eu
cd "$(dirname "$0")/../rust"

listed=$(mktemp)
wanted=$(mktemp)
trap 'rm -f "$listed" "$wanted"' EXIT

sed -n "/^rust_sources = files(/,/^)/p" meson.build \
    | grep -o "'crates/[^']*'" | tr -d "'" | sort -u > "$listed"
git ls-files 'crates/*/Cargo.toml' 'crates/*/src/*.rs' \
    | grep -v '^crates/hx-e2e/' \
    | grep -v -E '/tests/|/tests\.rs$|_tests\.rs$|/doubles\.rs$' \
    | sort -u > "$wanted"

missing=$(comm -13 "$listed" "$wanted")
if [ -n "$missing" ]; then
    echo "rust/meson.build: rust_sources is missing:" >&2
    printf '  %s\n' $missing >&2
    exit 1
fi
echo "rust_sources: every Rust source is listed."
