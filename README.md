GtkHx is a GTK+ Hotline Client originally based on Hx.

## Features

- Modern GTK+ 4 / libadwaita / libpanel UI with retro look and feel
- Full TLS support, for servers, file transfers, and trackers
- Support for extended Hotline protocol features and capabilities
  - Voice chat
  - Inline media
  - Native UTF-8
  - GIF icons
  - Large file transfers
  - Chat history
  - Colored names
  - Blowfish and ChaCha20-Poly1305 cipher support
- Tracker v3 support
- File preview supports common image types, QuickDraw PICTs, PDFs, and markdown / source files
- Orthogonal file manager interface for file transfers
- Server banner support
- Systray and app notifications
- Supports light and dark themes
- Customizable UI allows for single window and multi window layouts

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
