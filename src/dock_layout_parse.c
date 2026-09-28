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
 *   id     := '*'? one or more chars from [^,]:[whitespace]
 *   role   := one or more chars from [^]] [whitespace]
 *
 * The optional '*' prefix marks the leaf's foreground page. At most
 * one ID per leaf may carry it — a second one is a structurally
 * broken file, not a tie to break arbitrarily, so it's rejected.
 */

#include "config.h"

#include "dock_layout_parse.h"

#include <stdio.h>
#include <string.h>

void
dl_parsed_node_free (DLParsedNode *n)
{
    if (n == NULL) {
        return;
    }
    if (n->panel_ids != NULL) {
        g_ptr_array_unref (n->panel_ids);
    }
    g_free (n->role);
    dl_parsed_node_free (n->child_a);
    dl_parsed_node_free (n->child_b);
    g_free (n);
}

typedef struct {
    const char *p;
    const char *end;
} Cursor;

static void
skip_ws (Cursor *c)
{
    while (c->p < c->end && g_ascii_isspace (*c->p)) {
        c->p++;
    }
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
    if ((gsize)(c->end - c->p) >= n && strncmp (c->p, prefix, n) == 0) {
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
    if (!match (c, '[')) {
        return NULL;
    }

    DLParsedNode *n = g_new0 (DLParsedNode, 1);
    n->is_leaf = TRUE;
    n->panel_ids = g_ptr_array_new_with_free_func (g_free);
    n->selected = -1;

    skip_ws (c);
    /* Empty leaf ("L[]" or "L[:role]")? */
    while (c->p < c->end && *c->p != ']' && *c->p != ':') {
        const char *start;
        gboolean is_selected = FALSE;

        /* Foreground marker. Consumed before the id scan so the '*'
         * never lands in the id itself. A second marker in the same
         * leaf means two foreground pages in one frame, which the
         * serialiser can't produce and we can't act on. */
        if (*c->p == '*') {
            if (n->selected >= 0) {
                dl_parsed_node_free (n);
                return NULL;
            }
            is_selected = TRUE;
            c->p++;
            skip_ws (c); /* "L[ * b ]" — the file is hand-editable */
        }

        start = c->p;
        /* '*' terminates an id as well as separating one, so a second
         * marker ("L[**a]") can't be quietly absorbed into the id and
         * then match no panel at restore time. No real panel id
         * contains one. */
        while (c->p < c->end && *c->p != ',' && *c->p != ']' && *c->p != ':'
               && *c->p != '*' && !g_ascii_isspace (*c->p)) {
            c->p++;
        }
        /* Reject zero-length ids ("L[,a]", "L[a,,b]", "L[a,]" all
         * have at least one empty slot; "L[*]" is a marker with
         * nothing behind it). The serialiser would never produce
         * these, so a hand-edited file with them is structurally
         * broken. */
        if (c->p == start) {
            dl_parsed_node_free (n);
            return NULL;
        }
        g_ptr_array_add (n->panel_ids,
                         g_strndup (start, (gsize)(c->p - start)));
        if (is_selected) {
            n->selected = (int)n->panel_ids->len - 1;
        }
        skip_ws (c);
        if (!match (c, ',')) {
            break;
        }
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
        skip_ws (c); /* allow "L[a : role]" — match() already
                        * stripped ws before ':' on the way in. */
        const char *start = c->p;
        while (c->p < c->end && *c->p != ']' && !g_ascii_isspace (*c->p)) {
            c->p++;
        }
        if (c->p == start) {
            /* "L[a:]" — colon with no role behind it is a typo. */
            dl_parsed_node_free (n);
            return NULL;
        }
        n->role = g_strndup (start, (gsize)(c->p - start));
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
    if (!match (c, '(')) {
        return NULL;
    }

    DLParsedNode *a = parse_node (c);
    if (a == NULL) {
        return NULL;
    }
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
    n->child_a = a;
    n->child_b = b;
    n->selected = -1; /* leaf-only field; never read on a split */
    return n;
}

static DLParsedNode *
parse_node (Cursor *c)
{
    skip_ws (c);
    if (c->p >= c->end) {
        return NULL;
    }

    if (match_prefix (c, "h")) {
        return parse_split (c, DL_ORIENT_HORIZONTAL);
    }
    if (match_prefix (c, "v")) {
        return parse_split (c, DL_ORIENT_VERTICAL);
    }
    if (match_prefix (c, "L")) {
        return parse_leaf (c);
    }

    return NULL;
}

DLParsedNode *
dl_parse_tree (const char *text)
{
    if (text == NULL) {
        return NULL;
    }
    Cursor c = { text, text + strlen (text) };
    DLParsedNode *root = parse_node (&c);
    skip_ws (&c);
    if (root == NULL || c.p != c.end) {
        dl_parsed_node_free (root);
        return NULL;
    }
    return root;
}

/* ---- the old format, as mullion's JSON ------------------------------- */

static void
json_string (GString *out, const char *s)
{
    g_string_append_c (out, '"');
    for (const char *c = s; *c != '\0'; c++) {
        if (*c == '"' || *c == '\\') {
            g_string_append_c (out, '\\');
            g_string_append_c (out, *c);
        } else if ((unsigned char)*c < 0x20) {
            g_string_append_printf (out, "\\u%04x", (unsigned char)*c);
        } else {
            g_string_append_c (out, *c);
        }
    }
    g_string_append_c (out, '"');
}

/* Every split's divider position, by the node, in the post-order sizes=
 * was written in. */
static void
number_splits (DLParsedNode *n, GHashTable *at, char **sizes, guint *next)
{
    if (n->is_leaf) {
        return;
    }
    number_splits (n->child_a, at, sizes, next);
    number_splits (n->child_b, at, sizes, next);
    if (sizes != NULL && *next < g_strv_length (sizes)) {
        g_hash_table_insert (
            at, n,
            GINT_TO_POINTER ((int)g_ascii_strtoll (sizes[*next], NULL, 10)));
    }
    (*next)++;
}

/* A panel this layout leaves out of the tree: Files, now a window, and
 * the undocked ones, which get windows of their own. */
static gboolean
left_out (const char *id, GHashTable *floating)
{
    return g_strcmp0 (id, "files") == 0 || g_hash_table_contains (floating, id);
}

/* A node as JSON, in a box `w' by `h': the extent a divider's position
 * is a share of. An empty leaf is written too, with its slot, for the
 * dock to hand the slot on to the leaf that takes its room. */
static void
node_json (GString *out, DLParsedNode *n, GHashTable *at, GHashTable *floating,
           double w, double h)
{
    if (n->is_leaf) {
        int active = -1, kept = 0;

        g_string_append (out, "{\"tabs\":[");
        for (guint i = 0; i < n->panel_ids->len; i++) {
            const char *id = g_ptr_array_index (n->panel_ids, i);

            if (left_out (id, floating)) {
                continue;
            }
            if ((int)i == n->selected) {
                active = kept;
            }
            if (kept++ > 0) {
                g_string_append_c (out, ',');
            }
            json_string (out, id);
        }
        g_string_append_c (out, ']');
        if (active > 0) {
            g_string_append_printf (out, ",\"active\":%d", active);
        }
        if (n->role != NULL) {
            g_string_append (out, ",\"slots\":[");
            json_string (out, n->role);
            g_string_append_c (out, ']');
        }
        g_string_append_c (out, '}');
        return;
    }

    {
        gboolean row = n->orientation == DL_ORIENT_HORIZONTAL;
        double extent = row ? w : h;
        double pos = GPOINTER_TO_INT (g_hash_table_lookup (at, n));
        double share = pos > 0 && extent > 0 ? pos / extent : 0.5;
        char a[G_ASCII_DTOSTR_BUF_SIZE], b[G_ASCII_DTOSTR_BUF_SIZE];

        share = CLAMP (share, 0.05, 0.95);
        /* Three places, which is finer than a pixel and reads back. */
        share = (double)(int)(share * 1000 + 0.5) / 1000;
        g_string_append_printf (
            out, "{\"dir\":\"%s\",\"size\":[%s,%s],\"kids\":[",
            row ? "row" : "col", g_ascii_formatd (a, sizeof a, "%g", share),
            g_ascii_formatd (b, sizeof b, "%g", 1 - share));
        node_json (out, n->child_a, at, floating, row ? w * share : w,
                   row ? h : h * share);
        g_string_append_c (out, ',');
        node_json (out, n->child_b, at, floating, row ? w * (1 - share) : w,
                   row ? h : h * (1 - share));
        g_string_append (out, "]}");
    }
}

char *
dl_import_legacy (const char *tree, const char *sizes, const char *closed,
                  char **undocked, int width, int height)
{
    DLParsedNode *parsed = tree != NULL ? dl_parse_tree (tree) : NULL;
    GHashTable *at, *floating;
    g_auto (GStrv) positions = NULL;
    g_auto (GStrv) shut = NULL;
    GString *layout, *out;
    guint next = 0;
    gboolean any_shut = FALSE;

    if (parsed == NULL) {
        return NULL;
    }

    at = g_hash_table_new (NULL, NULL);
    floating = g_hash_table_new (g_str_hash, g_str_equal);
    positions = sizes != NULL ? g_strsplit (sizes, ";", -1) : NULL;
    number_splits (parsed, at, positions, &next);

    for (char **u = undocked; u != NULL && u[0] != NULL && u[1] != NULL;
         u += 2) {
        g_hash_table_insert (floating, u[0], u[1]);
    }

    layout = g_string_new (NULL);
    node_json (layout, parsed, at, floating, width > 0 ? width : 1100,
               height > 0 ? height : 700);

    shut = closed != NULL ? g_strsplit (closed, ";", -1) : NULL;
    for (char **c = shut; c != NULL && *c != NULL; c++) {
        g_strstrip (*c);
        if (**c != '\0' && !left_out (*c, floating)) {
            any_shut = TRUE;
        }
    }

    /* The envelope only with something to put in it beside the tree:
     * mullion reads a bare tree as the layout, and the envelope with no
     * version only when it has closed panes or windows. */
    if (!any_shut && g_hash_table_size (floating) == 0) {
        out = layout;
    } else {
        gboolean first = TRUE;

        out = g_string_new ("{\"layout\":");
        g_string_append (out, layout->str);
        g_string_free (layout, TRUE);

        if (any_shut) {
            g_string_append (out, ",\"closed\":[");
            for (char **c = shut; *c != NULL; c++) {
                if (**c == '\0' || left_out (*c, floating)) {
                    continue;
                }
                if (!first) {
                    g_string_append_c (out, ',');
                }
                json_string (out, *c);
                first = FALSE;
            }
            g_string_append_c (out, ']');
        }

        if (g_hash_table_size (floating) > 0) {
            first = TRUE;
            g_string_append (out, ",\"floating\":[");
            for (char **u = undocked; u != NULL && u[0] != NULL && u[1] != NULL;
                 u += 2) {
                int fw = 0, fh = 0;

                if (g_strcmp0 (u[0], "files") == 0) {
                    continue;
                }
                if (!first) {
                    g_string_append_c (out, ',');
                }
                g_string_append (out, "{\"layout\":{\"tabs\":[");
                json_string (out, u[0]);
                g_string_append (out, "]}");
                if (sscanf (u[1], "%d,%d", &fw, &fh) == 2 && fw > 0 && fh > 0) {
                    g_string_append_printf (out, ",\"size\":[%d,%d]", fw, fh);
                }
                g_string_append_c (out, '}');
                first = FALSE;
            }
            g_string_append_c (out, ']');
        }
        g_string_append_c (out, '}');
    }

    g_hash_table_unref (at);
    g_hash_table_unref (floating);
    dl_parsed_node_free (parsed);

    return g_string_free (out, FALSE);
}
