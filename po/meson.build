i18n = import('i18n')

# i18n.gettext() reads po/POTFILES (list of source files containing
# translatable strings) and po/LINGUAS (list of available languages),
# builds each `.mo`, installs it, and gives us the `gtkhx-pot` and
# `gtkhx-update-po` convenience targets.
#
# No catalog carries a `#, fuzzy` entry any more, so meson's stock
# msgfmt invocation is enough — the install-time --use-fuzzy override
# that used to live here is gone. If a future language lands as an
# unreviewed seed, mark it fuzzy and reintroduce the override rather
# than shipping it silently. Terminology contract: po/GLOSSARY.md.
i18n.gettext(meson.project_name(),
  args : [
    '--directory=' + meson.project_source_root(),
    '--from-code=UTF-8',
    # xgettext's defaults know about gettext / ngettext / dgettext but
    # not the glib variants. Tell it that g_dngettext's plural arg
    # pair lives at positions 2 (singular) and 3 (plural) — see
    # `info gettext "Marking Plural Forms"` — so plural strings from
    # files_browser.c / chat.c / etc. land in gtkhx.pot.
    '--keyword=g_dngettext:2,3',
    # The Rust half's gettext shims (rust/crates/gtkhx-ui/src/tr.rs). xgettext
    # has recognised Rust since gettext 0.25, and lexes it properly — it does
    # not mistake the `_str` tail of `push_str("…")` for a keyword — but it has
    # no idea what our wrappers are called, so name each one and say where its
    # msgid sits. `trn` is the ngettext form and needs the singular/plural
    # pair.
    #
    # Only the `gtkhx-pot` and `gtkhx-update-po` targets run xgettext, and
    # neither is built by default, so an older gettext costs a maintainer
    # regenerating the catalog rather than anybody building the client.
    '--keyword=tr:1',
    '--keyword=tr1:1',
    '--keyword=tr_fmt:1',
    '--keyword=tr_argv:1',
    '--keyword=trn:1,2',
    # The N_() marker: a literal that is translated somewhere else later.
    '--keyword=n_:1',
  ],
)
