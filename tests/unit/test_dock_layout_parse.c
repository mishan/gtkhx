/*
 * tests/unit/test_dock_layout_parse.c — pin down the dock-layout
 * tree expression parser. The parser is small and recursive but
 * the format is on-disk and user-editable, so changes that subtly
 * shift behaviour (whitespace tolerance, character classes,
 * error handling) need test coverage to catch.
 *
 * Pure GLib — the parser was split off from dock_layout.c into
 * dock_layout_parse.{c,h} specifically so this test can build
 * without dragging in GTK + libpanel + the widget tree.
 */

#include "config.h"
#include <glib.h>
#include <string.h>
#include "dock_layout_parse.h"

/* ---------- Helpers -------------------------------------------------- */

static const char *
nth_id (DLParsedNode *leaf, guint i)
{
    if (leaf == NULL || leaf->panel_ids == NULL || i >= leaf->panel_ids->len)
        return NULL;
    return g_ptr_array_index (leaf->panel_ids, i);
}

/* ---------- 1. Empty + whitespace-only inputs ------------------------ */

static void
test_null_input (void)
{
    g_assert_null (dl_parse_tree (NULL));
}

static void
test_empty_string (void)
{
    g_assert_null (dl_parse_tree (""));
}

static void
test_whitespace_only (void)
{
    g_assert_null (dl_parse_tree ("   \n\t  "));
}

/* ---------- 2. Leaf shapes ------------------------------------------- */

static void
test_empty_leaf (void)
{
    DLParsedNode *n = dl_parse_tree ("L[]");
    g_assert_nonnull (n);
    g_assert_true (n->is_leaf);
    g_assert_nonnull (n->panel_ids);
    g_assert_cmpuint (n->panel_ids->len, ==, 0);
    g_assert_null (n->role);
    g_assert_null (n->child_a);
    g_assert_null (n->child_b);
    dl_parsed_node_free (n);
}

static void
test_single_id (void)
{
    DLParsedNode *n = dl_parse_tree ("L[chat]");
    g_assert_nonnull (n);
    g_assert_true (n->is_leaf);
    g_assert_cmpuint (n->panel_ids->len, ==, 1);
    g_assert_cmpstr (nth_id (n, 0), ==, "chat");
    dl_parsed_node_free (n);
}

static void
test_multiple_ids (void)
{
    DLParsedNode *n = dl_parse_tree ("L[chat,files,news15]");
    g_assert_nonnull (n);
    g_assert_cmpuint (n->panel_ids->len, ==, 3);
    g_assert_cmpstr (nth_id (n, 0), ==, "chat");
    g_assert_cmpstr (nth_id (n, 1), ==, "files");
    g_assert_cmpstr (nth_id (n, 2), ==, "news15");
    dl_parsed_node_free (n);
}

static void
test_leaf_with_role (void)
{
    DLParsedNode *n = dl_parse_tree ("L[users:end]");
    g_assert_nonnull (n);
    g_assert_cmpuint (n->panel_ids->len, ==, 1);
    g_assert_cmpstr (nth_id (n, 0), ==, "users");
    g_assert_cmpstr (n->role, ==, "end");
    dl_parsed_node_free (n);
}

static void
test_empty_leaf_with_role (void)
{
    /* "L[:start]" — the user closed everything but the start
     * default leaf survives. */
    DLParsedNode *n = dl_parse_tree ("L[:start]");
    g_assert_nonnull (n);
    g_assert_cmpuint (n->panel_ids->len, ==, 0);
    g_assert_cmpstr (n->role, ==, "start");
    dl_parsed_node_free (n);
}

/* ---------- 3. Splits ------------------------------------------------ */

static void
test_horizontal_split (void)
{
    DLParsedNode *n = dl_parse_tree ("h(L[a],L[b])");
    g_assert_nonnull (n);
    g_assert_false (n->is_leaf);
    g_assert_cmpint (n->orientation, ==, DL_ORIENT_HORIZONTAL);
    g_assert_nonnull (n->child_a);
    g_assert_nonnull (n->child_b);
    g_assert_cmpstr (nth_id (n->child_a, 0), ==, "a");
    g_assert_cmpstr (nth_id (n->child_b, 0), ==, "b");
    dl_parsed_node_free (n);
}

static void
test_vertical_split (void)
{
    DLParsedNode *n = dl_parse_tree ("v(L[top],L[bot])");
    g_assert_nonnull (n);
    g_assert_cmpint (n->orientation, ==, DL_ORIENT_VERTICAL);
    g_assert_cmpstr (nth_id (n->child_a, 0), ==, "top");
    g_assert_cmpstr (nth_id (n->child_b, 0), ==, "bot");
    dl_parsed_node_free (n);
}

static void
test_default_layout (void)
{
    /* The actual shipping default-layout expression. */
    const char *text =
        "h(L[news:start],"
          "h(v(L[chat,files,news15:center],L[tasks:bottom]),"
            "L[users:end]))";
    DLParsedNode *n = dl_parse_tree (text);
    g_assert_nonnull (n);
    g_assert_false (n->is_leaf);
    g_assert_cmpint (n->orientation, ==, DL_ORIENT_HORIZONTAL);

    /* Left sub-tree: L[news:start] */
    g_assert_true (n->child_a->is_leaf);
    g_assert_cmpstr (n->child_a->role, ==, "start");
    g_assert_cmpstr (nth_id (n->child_a, 0), ==, "news");

    /* Right sub-tree: h(v(...), L[users:end]) */
    g_assert_false (n->child_b->is_leaf);
    g_assert_cmpstr (n->child_b->child_b->role, ==, "end");
    g_assert_cmpstr (nth_id (n->child_b->child_b, 0), ==, "users");

    /* Center+bottom: v(L[chat,files,news15:center], L[tasks:bottom]) */
    DLParsedNode *vsplit = n->child_b->child_a;
    g_assert_false (vsplit->is_leaf);
    g_assert_cmpint (vsplit->orientation, ==, DL_ORIENT_VERTICAL);
    g_assert_cmpstr (vsplit->child_a->role, ==, "center");
    g_assert_cmpuint (vsplit->child_a->panel_ids->len, ==, 3);
    g_assert_cmpstr (nth_id (vsplit->child_a, 0), ==, "chat");
    g_assert_cmpstr (nth_id (vsplit->child_a, 1), ==, "files");
    g_assert_cmpstr (nth_id (vsplit->child_a, 2), ==, "news15");
    g_assert_cmpstr (vsplit->child_b->role, ==, "bottom");
    g_assert_cmpstr (nth_id (vsplit->child_b, 0), ==, "tasks");

    dl_parsed_node_free (n);
}

/* ---------- 4. Whitespace tolerance ---------------------------------- */

static void
test_whitespace_tolerance (void)
{
    /* Same expression as test_horizontal_split, with whitespace
     * sprinkled at every legal boundary. Hand-edited files should
     * parse the same way as compact serialiser output. */
    const char *text =
        "  h(  L[ a , b ]  ,  v(  L[ c ]  ,  L[]  )  )  ";
    DLParsedNode *n = dl_parse_tree (text);
    g_assert_nonnull (n);
    g_assert_cmpint (n->orientation, ==, DL_ORIENT_HORIZONTAL);
    g_assert_cmpuint (n->child_a->panel_ids->len, ==, 2);
    g_assert_cmpstr (nth_id (n->child_a, 0), ==, "a");
    g_assert_cmpstr (nth_id (n->child_a, 1), ==, "b");
    g_assert_cmpint (n->child_b->orientation, ==, DL_ORIENT_VERTICAL);
    g_assert_cmpstr (nth_id (n->child_b->child_a, 0), ==, "c");
    g_assert_cmpuint (n->child_b->child_b->panel_ids->len, ==, 0);
    dl_parsed_node_free (n);
}

/* ---------- 5. Malformed inputs reject cleanly ----------------------- */

static void
test_malformed_unterminated_leaf (void)
{
    g_assert_null (dl_parse_tree ("L[chat"));     /* no ] */
    g_assert_null (dl_parse_tree ("L["));         /* no ] */
}

static void
test_malformed_unterminated_split (void)
{
    g_assert_null (dl_parse_tree ("h(L[a]"));     /* no ) */
    g_assert_null (dl_parse_tree ("h(L[a],"));    /* truncated */
    g_assert_null (dl_parse_tree ("h(L[a],L[b]")); /* no ) */
}

static void
test_malformed_split_needs_two_children (void)
{
    g_assert_null (dl_parse_tree ("h(L[a])"));    /* only one child */
    g_assert_null (dl_parse_tree ("h()"));        /* no children */
}

static void
test_malformed_unknown_token (void)
{
    g_assert_null (dl_parse_tree ("X[chat]"));    /* not h/v/L */
    g_assert_null (dl_parse_tree ("?"));
}

static void
test_malformed_trailing_garbage (void)
{
    /* parse_tree rejects extra non-whitespace input after the
     * top-level node. Round-trip from serializer should never
     * produce trailing garbage. */
    g_assert_null (dl_parse_tree ("L[a] L[b]"));
    g_assert_null (dl_parse_tree ("L[a]extra"));
}

static void
test_malformed_empty_id (void)
{
    /* Zero-length ids would be a typo or corrupted file — the
     * serialiser never produces them. */
    g_assert_null (dl_parse_tree ("L[,a]"));
    g_assert_null (dl_parse_tree ("L[a,,b]"));
    g_assert_null (dl_parse_tree ("L[a,]"));
    g_assert_null (dl_parse_tree ("L[,]"));
}

static void
test_malformed_empty_role (void)
{
    /* Colon with no role after it. */
    g_assert_null (dl_parse_tree ("L[a:]"));
    g_assert_null (dl_parse_tree ("L[:]"));
}

static void
test_role_whitespace_tolerance (void)
{
    /* "L[a : end]" — whitespace around the colon. The serialiser
     * uses the compact form (no spaces) but a hand-edited file
     * should still parse. */
    DLParsedNode *n = dl_parse_tree ("L[a : end]");
    g_assert_nonnull (n);
    g_assert_cmpstr (nth_id (n, 0), ==, "a");
    g_assert_cmpstr (n->role, ==, "end");
    dl_parsed_node_free (n);
}

/* ---------- 6. Edge cases ------------------------------------------- */

static void
test_panel_ids_with_punctuation (void)
{
    /* IDs can contain anything that isn't a separator (',' ']'
     * ':' or whitespace). msg-12345 (PM panel) and pchat-7 (per-
     * chat panel) are the realistic non-static cases. */
    DLParsedNode *n = dl_parse_tree ("L[msg-12345,pchat-7]");
    g_assert_nonnull (n);
    g_assert_cmpuint (n->panel_ids->len, ==, 2);
    g_assert_cmpstr (nth_id (n, 0), ==, "msg-12345");
    g_assert_cmpstr (nth_id (n, 1), ==, "pchat-7");
    dl_parsed_node_free (n);
}

static void
test_deeply_nested (void)
{
    /* Five levels of nesting — exercises the recursion. */
    DLParsedNode *n = dl_parse_tree (
        "h(L[a],h(L[b],h(L[c],h(L[d],L[e]))))");
    g_assert_nonnull (n);
    g_assert_cmpstr (nth_id (n->child_a, 0), ==, "a");
    g_assert_cmpstr (nth_id (n->child_b->child_a, 0), ==, "b");
    g_assert_cmpstr (nth_id (n->child_b->child_b->child_a, 0), ==, "c");
    g_assert_cmpstr (nth_id (n->child_b->child_b->child_b->child_a, 0),
                     ==, "d");
    g_assert_cmpstr (nth_id (n->child_b->child_b->child_b->child_b, 0),
                     ==, "e");
    dl_parsed_node_free (n);
}

/* ---------- Test registration ---------------------------------------- */

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/dock_layout_parse/null",                 test_null_input);
    g_test_add_func ("/dock_layout_parse/empty_string",         test_empty_string);
    g_test_add_func ("/dock_layout_parse/whitespace_only",      test_whitespace_only);

    g_test_add_func ("/dock_layout_parse/leaf/empty",           test_empty_leaf);
    g_test_add_func ("/dock_layout_parse/leaf/single_id",       test_single_id);
    g_test_add_func ("/dock_layout_parse/leaf/multiple_ids",    test_multiple_ids);
    g_test_add_func ("/dock_layout_parse/leaf/role",            test_leaf_with_role);
    g_test_add_func ("/dock_layout_parse/leaf/empty_with_role", test_empty_leaf_with_role);

    g_test_add_func ("/dock_layout_parse/split/horizontal",     test_horizontal_split);
    g_test_add_func ("/dock_layout_parse/split/vertical",       test_vertical_split);
    g_test_add_func ("/dock_layout_parse/default_layout",       test_default_layout);

    g_test_add_func ("/dock_layout_parse/whitespace_tolerance", test_whitespace_tolerance);

    g_test_add_func ("/dock_layout_parse/malformed/leaf_open",  test_malformed_unterminated_leaf);
    g_test_add_func ("/dock_layout_parse/malformed/split_open", test_malformed_unterminated_split);
    g_test_add_func ("/dock_layout_parse/malformed/one_child",  test_malformed_split_needs_two_children);
    g_test_add_func ("/dock_layout_parse/malformed/unknown",    test_malformed_unknown_token);
    g_test_add_func ("/dock_layout_parse/malformed/trailing",   test_malformed_trailing_garbage);
    g_test_add_func ("/dock_layout_parse/malformed/empty_id",   test_malformed_empty_id);
    g_test_add_func ("/dock_layout_parse/malformed/empty_role", test_malformed_empty_role);

    g_test_add_func ("/dock_layout_parse/role_whitespace",      test_role_whitespace_tolerance);

    g_test_add_func ("/dock_layout_parse/punct_ids",            test_panel_ids_with_punctuation);
    g_test_add_func ("/dock_layout_parse/deeply_nested",        test_deeply_nested);

    return g_test_run ();
}
