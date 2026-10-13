#!/bin/sh
# Fail if an icon-only button is built outside the helpers that name it.
#
# An icon gives a button no name of its own. gtkhx_icon_button_new (C) and
# icon_button (Rust) take one as an argument and set it as both tooltip and
# accessible name; this keeps new buttons going through them.
set -eu
cd "$(dirname "$0")/.."

hits=$({
    grep -rn 'gtk_button_new_from_icon_name' src --include='*.c'
    grep -rn 'Button::from_icon_name' rust/crates --include='*.rs'
} | grep -v -e '^src/gtkutil\.c:' -e '^rust/crates/gtkhx-ui/src/lib\.rs:' || true)

if [ -n "$hits" ]; then
    echo "$hits"
    echo
    echo "Build icon-only buttons with gtkhx_icon_button_new (C) or"
    echo "crate::icon_button (Rust), which set the accessible name."
    exit 1
fi
echo "icon buttons: all named."
