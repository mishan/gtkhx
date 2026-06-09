/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * dock_layout_parse.c — recursive-descent parser for the
 * dock-layout tree expression. Pure C + GLib so the parser is
 * unit-testable without GTK.
 *
 * See dock_layout_parse.h for the format. The grammar:
 *
 *   tree   := node
 *   node   := split | leaf
 *   split  := ('h' | 'v') '(' node ',' node ')'
 *   leaf   := 'L' '[' ids? (':' role)? ']'
 *   ids    := id (',' id)*
 *   id     := one or more chars from [^,]:[whitespace]
 *   role   := one or more chars from [^]] [whitespace]
 */

#include "config.h"

#include "dock_layout_parse.h"

#include <string.h>

void
dl_parsed_node_free (DLParsedNode *n)
{
    if (n == NULL)
        return;
    if (n->panel_ids != NULL)
        g_ptr_array_unref (n->panel_ids);
    g_free (n->role);
    dl_parsed_node_free (n->child_a);
    dl_parsed_node_free (n->child_b);
    g_free (n);
}

typedef struct { const char *p; const char *end; } Cursor;

static void
skip_ws (Cursor *c)
{
    while (c->p < c->end && g_ascii_isspace (*c->p))
        c->p++;
}

static gboolean
match (Cursor *c, char ch)
{
    skip_ws (c);
    if (c->p < c->end && *c->p == ch) {
        c->p++;
        return TRUE;
    }
    return FALSE;
}

static gboolean
match_prefix (Cursor *c, const char *prefix)
{
    skip_ws (c);
    gsize n = strlen (prefix);
    if ((gsize) (c->end - c->p) >= n && strncmp (c->p, prefix, n) == 0) {
        c->p += n;
        return TRUE;
    }
    return FALSE;
}

static DLParsedNode *parse_node (Cursor *c);

static DLParsedNode *
parse_leaf (Cursor *c)
{
    /* Already consumed 'L'. Now '['; ids separated by ','; optional
     * ':role' inside the brackets; closing ']'. */
    if (!match (c, '['))
        return NULL;

    DLParsedNode *n = g_new0 (DLParsedNode, 1);
    n->is_leaf   = TRUE;
    n->panel_ids = g_ptr_array_new_with_free_func (g_free);

    skip_ws (c);
    /* Empty leaf ("L[]" or "L[:role]")? */
    while (c->p < c->end && *c->p != ']' && *c->p != ':') {
        const char *start = c->p;
        while (c->p < c->end && *c->p != ',' && *c->p != ']'
               && *c->p != ':' && !g_ascii_isspace (*c->p))
            c->p++;
        /* Reject zero-length ids ("L[,a]", "L[a,,b]", "L[a,]" all
         * have at least one empty slot). The serialiser would
         * never produce these, so a hand-edited file with them is
         * structurally broken. */
        if (c->p == start) {
            dl_parsed_node_free (n);
            return NULL;
        }
        g_ptr_array_add (n->panel_ids,
                         g_strndup (start, (gsize) (c->p - start)));
        skip_ws (c);
        if (!match (c, ','))
            break;
        skip_ws (c);
        /* A comma must be followed by another id — anything else
         * is a trailing-comma typo ("L[a,]", "L[a,:end]"). */
        if (c->p >= c->end || *c->p == ']' || *c->p == ':') {
            dl_parsed_node_free (n);
            return NULL;
        }
    }

    skip_ws (c);
    if (match (c, ':')) {
        skip_ws (c);   /* allow "L[a : role]" — match() already
                        * stripped ws before ':' on the way in. */
        const char *start = c->p;
        while (c->p < c->end && *c->p != ']' && !g_ascii_isspace (*c->p))
            c->p++;
        if (c->p == start) {
            /* "L[a:]" — colon with no role behind it is a typo. */
            dl_parsed_node_free (n);
            return NULL;
        }
        n->role = g_strndup (start, (gsize) (c->p - start));
    }

    if (!match (c, ']')) {
        dl_parsed_node_free (n);
        return NULL;
    }
    return n;
}

static DLParsedNode *
parse_split (Cursor *c, DLOrientation orientation)
{
    /* Already consumed "h" or "v". Now '('; child_a; ','; child_b; ')'. */
    if (!match (c, '('))
        return NULL;

    DLParsedNode *a = parse_node (c);
    if (a == NULL)
        return NULL;
    if (!match (c, ',')) {
        dl_parsed_node_free (a);
        return NULL;
    }
    DLParsedNode *b = parse_node (c);
    if (b == NULL) {
        dl_parsed_node_free (a);
        return NULL;
    }
    if (!match (c, ')')) {
        dl_parsed_node_free (a);
        dl_parsed_node_free (b);
        return NULL;
    }

    DLParsedNode *n = g_new0 (DLParsedNode, 1);
    n->orientation = orientation;
    n->child_a     = a;
    n->child_b     = b;
    return n;
}

static DLParsedNode *
parse_node (Cursor *c)
{
    skip_ws (c);
    if (c->p >= c->end)
        return NULL;

    if (match_prefix (c, "h"))
        return parse_split (c, DL_ORIENT_HORIZONTAL);
    if (match_prefix (c, "v"))
        return parse_split (c, DL_ORIENT_VERTICAL);
    if (match_prefix (c, "L"))
        return parse_leaf (c);

    return NULL;
}

DLParsedNode *
dl_parse_tree (const char *text)
{
    if (text == NULL)
        return NULL;
    Cursor c = { text, text + strlen (text) };
    DLParsedNode *root = parse_node (&c);
    skip_ws (&c);
    if (root == NULL || c.p != c.end) {
        dl_parsed_node_free (root);
        return NULL;
    }
    return root;
}
