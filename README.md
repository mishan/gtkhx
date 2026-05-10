GtkHx is a GTK+ Hotline Client based on Hx, a console Hotline Client written by 
Ryan Nielsen. It is still under development and some features of it may not be
completely working.

## Building:

```
meson setup build
cd build && meson compile && meson install
```

## Testing:

A local mhxd instance should be running for the integration tests to run. An
mhxd Docker container is provided. To run it:

```
docker build -t gtkhx-mhxd tests/mhxd
docker run -d -p 5500:5500 -p 5501:5501 gtkhx-mhxd
cd build & meson test
```

## Flatpak:

```
flatpak-builder --user --install --force-clean build-flatpak \
                com.nasledov.gtkhx.yml
flatpak run com.nasledov.gtkhx
```

The manifest pins org.gnome.Platform 49 and pulls the source from the
working tree, so edits land in subsequent flatpak-builder runs without
needing a fresh clone.
