i18n = import('i18n')

# i18n.gettext() reads po/POTFILES (list of source files containing
# translatable strings) and po/LINGUAS (list of available languages).
i18n.gettext(meson.project_name(),
  args : [
    '--directory=' + meson.project_source_root(),
    '--from-code=UTF-8',
  ],
)
