# App-specific symbolic icons

The icons GtkHx's symbolic chrome needs that Adwaita doesn't provide — the
Hotline vocabulary: public chat, the user list, news, broadcast. Everything
else in the symbolic set is a stock icon name; the mapping from each classic
pixmap to its symbolic stand-in is the table in `src/gtkhx_icon.c`.

They live under `scalable/actions/` with the app-id prefix, and ship in the
GResource at `/com/nasledov/gtkhx/icons/…`, which `gtkhx_activate` adds to the
icon theme's search path. So they look the same on every platform whatever
Adwaita version the host has, and the prefix keeps them from colliding with an
icon theme's own names.

## Source and license

All four come from GNOME's
[icon-development-kit](https://gitlab.gnome.org/Teams/Design/icon-development-kit),
the icon set behind the Icon Library app, which is released under
[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) — no attribution
required, credited here anyway.

| File | Kit icon |
|---|---|
| `com.nasledov.gtkhx-chat-symbolic.svg` | `icons/chat-bubble.svg` |
| `com.nasledov.gtkhx-users-symbolic.svg` | `icons/people.svg` |
| `com.nasledov.gtkhx-broadcast-symbolic.svg` | `icons/megaphone.svg` |
| `com.nasledov.gtkhx-news-symbolic.svg` | "newspaper" in the kit's earlier single-sheet `src/icons.svg` (the current set has none) |

The kit's current icons use GTK's newer symbolic format — stroked paths with
`gpa:` attributes — which only GTK 4.20 and later draw as intended; older GTK
recolors a symbolic icon by forcing a fill onto every path, which fills in an
outline. So each was converted to plain filled paths: the strokes outlined with
[picosvg](https://github.com/googlefonts/picosvg), transforms flattened, and the
result cropped to a 16×16 `viewBox` with nothing but `<path d>` elements left.
That is the form every supported GTK recolors correctly. To add one, run the kit
SVG through the same steps; a stroked icon that isn't converted will render as
filled blobs on anything older than GTK 4.20.
