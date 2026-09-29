i18n = import('i18n')

# The chat view's own catalog, in the "rotulus" gettext domain. The widget
# looks its strings up there, so an application that ships the catalog
# gets them translated without merging them into its own.
i18n.gettext('rotulus',
  args : [
    '--directory=' + meson.current_source_dir() / '..',
    '--from-code=UTF-8',
    '--keyword=tr:1',
  ],
)
