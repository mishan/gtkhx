GtkHx is a GTK+ Hotline Client originally based on Hx. Originally written in C, and currently
being rewritten in Rust.

## Features

- Modern GTK+ 4 / libadwaita / libpanel UI with retro look and feel
- Full TLS support, for servers, file transfers, and trackers
- Cross platform -- works on Linux, macOS, and Windows
- Support for extended Hotline protocol features and capabilities
  - Voice chat
  - Inline media
  - Native UTF-8
  - GIF icons
  - Large file transfers
  - Chat history
  - Colored names
  - Blowfish and ChaCha20-Poly1305 cipher support
  - Tracker v3
- File previews
  - Common image types via glycin
  - QuickDraw PICTs via ImageMagick
  - PDFs via libpoppler
  - markdown / source files via libgtksourceview
- Orthogonal file manager
- Customizable UI allows for single window and multi window layouts
- Themable, with support for light and dark themes
- Systray and app notifications

## Building:

```
meson setup build
cd build && meson compile && meson install
```

## Testing:

A `tests/docker-compose-yml` file provides mhxd, hxtrackd, Janus, and Argus
containers to test against. `tests/run.sh` will build and start all the containers.
Note: The base Dockerfiles live in [mishan/hotline-docker](https://github.com/mishan/hotline-docker),
and [mishan/gtkhx-docker](https://github.com/mishan/gtkhx-docker) for the
CI base image and SOCKS proxy.

```
./tests/run.sh
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
