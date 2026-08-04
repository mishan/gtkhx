#!/bin/sh
# Every Rust source that actually yields a translatable string, in the form
# po/POTFILES wants.
#
# Two passes, and the second is the one that decides. A grep for the helper
# names is fast but wrong on its own: it matches their definitions in tr.rs,
# and it would match a `tr(` that turned out to have no literal to extract. So
# the grep only narrows the candidates, and xgettext — the thing that will
# actually build the catalog — says which of them yield a msgid.
set -eu
cd "$(dirname "$0")/.."

candidates=$(grep -rlE '\b(tr|tr1|tr_fmt|tr_argv|trn|n_)[[:space:]]*\(' \
    --include='*.rs' rust/crates | sort)

for f in $candidates; do
    n=$(xgettext --language=Rust --from-code=UTF-8 \
        --keyword=tr:1 --keyword=tr1:1 --keyword=tr_fmt:1 \
        --keyword=tr_argv:1 --keyword=trn:1,2 --keyword=n_:1 \
        -o - "$f" 2>/dev/null | grep -c '^msgid "..*"' || true)
    [ "$n" -gt 0 ] && echo "$f"
done
exit 0
