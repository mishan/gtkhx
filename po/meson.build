i18n = import('i18n')

# i18n.gettext() reads po/POTFILES (list of source files containing
# translatable strings) and po/LINGUAS (list of available languages).
#
# install: false suppresses meson's default `.mo` install — its msgfmt
# invocation doesn't pass --use-fuzzy, so every entry flagged
# `#, fuzzy` (which is every entry in our seed de/es/fr/pt catalogs,
# pending human review) gets stripped from the binary, and the
# installed .mo ends up being just the header. We still want the
# convenience `gtkhx-pot` and `gtkhx-update-po` targets this helper
# generates, so we keep the call and route the actual install through
# the script below instead.
i18n.gettext(meson.project_name(),
  args : [
    '--directory=' + meson.project_source_root(),
    '--from-code=UTF-8',
  ],
  install : false,
)

# Build + install each language's .mo with --use-fuzzy so the seed
# machine translations actually ship. See po/install-fuzzy-mo.py for
# the full reasoning and the cleanup path once translations are
# human-reviewed.
meson.add_install_script(
  find_program('python3'),
  files('install-fuzzy-mo.py'),
  meson.project_name(),
  meson.current_source_dir(),
)
