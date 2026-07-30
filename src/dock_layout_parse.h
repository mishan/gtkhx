/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * dock_layout_parse.h — pure-text dock-layout tree representation
 * and recursive-descent parser. No GTK or libpanel.
 *
 * Split off from dock_layout.c so the parser can be unit-tested
 * without bringing in GTK + libpanel + the widget tree. The live
 * build / serialise paths in dock_layout.c convert between
 * ParsedNode and HxSplit; the format and the in-memory tree
 * representation both live here.
 *
 * Format (also documented in dock_layout.h):
 *
 *   h(A,B)              horizontal split
 *   v(A,B)              vertical split
 *   L[id1,id2,...]      leaf with panel IDs in tab order
 *   L[ids:role]         leaf tagged with one of start / end /
 *                       bottom / center
 *
 * Whitespace between tokens is tolerated so users can hand-edit
 * the file. Panel IDs are anything that isn't a separator
 * character (',' ']' ':' or whitespace) — no quoting needed for
 * the IDs we actually have.
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

G_END_DECLS

#endif /* GTKHX_DOCK_LAYOUT_PARSE_H */
