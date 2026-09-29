/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * dock_layout_parse.h — the dock-layout tree expression kept before the
 * dock was mullion-gtk, its recursive-descent parser, and its import into
 * the dock's JSON (dl_import_legacy). No GTK.
 *
 * Split off from dock_layout.c so it can be unit-tested without bringing
 * in GTK and the dock.
 *
 * Format (also documented in dock_layout.h):
 *
 *   h(A,B)              horizontal split
 *   v(A,B)              vertical split
 *   L[id1,id2,...]      leaf with panel IDs in tab order
 *   L[id1,*id2]         '*' marks the leaf's foreground page
 *   L[ids:role]         leaf tagged with one of start / end /
 *                       bottom / center
 *
 * Whitespace between tokens is tolerated so users can hand-edit
 * the file. Panel IDs are anything that isn't a separator
 * character (',' ']' ':' or whitespace) — no quoting needed for
 * the IDs we actually have. A leading '*' on an ID is the
 * foreground marker and is not part of the ID; at most one ID per
 * leaf may carry it.
 */

#ifndef GTKHX_DOCK_LAYOUT_PARSE_H
#define GTKHX_DOCK_LAYOUT_PARSE_H 1

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    DL_ORIENT_HORIZONTAL = 0,
    DL_ORIENT_VERTICAL = 1,
} DLOrientation;

typedef struct DLParsedNode {
    /* Leaf state. is_leaf TRUE → these are populated, the
     * internal-split fields are NULL/0. */
    gboolean is_leaf;
    GPtrArray *panel_ids; /* char *, owned */
    char *role;           /* owned; NULL when untagged */

    /* Index into panel_ids of the page that was in the foreground
     * when the layout was saved, or -1 when the leaf recorded none
     * (an empty leaf, or a file written before the marker existed).
     * NOT zero-defaulted: 0 is a legitimate index, so every
     * construction path sets this explicitly. */
    int selected;

    /* Internal-split state. is_leaf FALSE → these are populated. */
    DLOrientation orientation;
    struct DLParsedNode *child_a;
    struct DLParsedNode *child_b;
} DLParsedNode;

/* Parse a tree expression. Returns NULL on malformed input
 * (truncated, extra trailing input, unbalanced brackets, etc.).
 * The returned tree is owned by the caller — free with
 * dl_parsed_node_free. */
DLParsedNode *dl_parse_tree (const char *text);

void dl_parsed_node_free (DLParsedNode *n);

/* A layout kept in this format -- the [Dock] tree=, sizes= and closed=
 * keys and the [Undocked] group -- as the JSON the dock (mullion-gtk)
 * reads: a tree of splits with a share per child and leaves with their
 * tabs, the page in front as "active" and the role as a slot; the
 * closed panels as the envelope's "closed"; each undocked panel a
 * floating window of its own. `sizes' are the old dividers in pixels,
 * post-order, turned into shares against a window of `width' by
 * `height'. The Files panel, which is a window now, is left out.
 * `undocked' is NULL-terminated id, "W,H" pairs, or NULL. NULL for a
 * tree that does not parse. Pure GLib, for the unit test. */
char *dl_import_legacy (const char *tree, const char *sizes, const char *closed,
                        char **undocked, int width, int height);

G_END_DECLS

#endif /* GTKHX_DOCK_LAYOUT_PARSE_H */
