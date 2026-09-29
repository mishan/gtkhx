#!/bin/sh
# Fail if po/POTFILES has drifted from the Rust sources that actually carry
# translatable strings.
#
# The list went years listing no `.rs` file at all, so every string the Rust
# half added was silently untranslatable — a failure with no symptom until a
# translator notices the catalog is missing half the UI. Nothing about adding
# a file to a crate reminds anyone to touch POTFILES, so this is the reminder.
#
# Only the Rust half is checked. The C list predates this and is maintained by
# hand; extending the check to it means deciding what to do about the files
# that legitimately have no strings, which is a separate cleanup.
set -eu
cd "$(dirname "$0")/.."

expected=$(./tools/potfiles-rs.sh)
listed=$(grep '\.rs$' po/POTFILES || true)

if [ "$expected" = "$listed" ]; then
    echo "POTFILES: Rust sources are up to date."
    exit 0
fi

echo "POTFILES is out of date with the Rust sources." >&2
echo >&2
echo "$expected" > /tmp/potfiles-expected.$$
echo "$listed" > /tmp/potfiles-listed.$$
diff -u /tmp/potfiles-listed.$$ /tmp/potfiles-expected.$$ >&2 || true
rm -f /tmp/potfiles-expected.$$ /tmp/potfiles-listed.$$
echo >&2
echo "Regenerate with:" >&2
echo "  { grep -v '\\.rs\$' po/POTFILES; ./tools/potfiles-rs.sh; } > po/POTFILES.new" >&2
echo "  mv po/POTFILES.new po/POTFILES" >&2
exit 1
