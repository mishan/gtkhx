/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "config.h"

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib/gi18n.h>
#include <string.h>

#include "files_complete.h"

/* Match cap. Showing more rows than this is rarely useful and the
 * popover scrolls past being readable. Mirrors GtkFileChooser's
 * behaviour. */
#define MAX_SUGGESTIONS 12

/* Visible-list scroll cap. */
#define POPOVER_VISIBLE_ROWS 8

struct _hx_path_complete {
    GtkEntry *entry; /* not owned */

    GtkWidget *popover;
    GtkWidget *listview;
    GListStore *store; /* GListStore<HxStr>
                                    * (one entry per child directory,
                                    * built fresh per directory) */
    GtkFilterListModel *filter_model;
    GtkCustomFilter *filter;
    GtkSingleSelection *selection;

    /* Currently-enumerated directory (always ends in '/'), so a
     * subsequent keystroke that keeps us in the same directory
     * doesn't re-hit the filesystem. */
    char *last_dir;

    /* The basename prefix the filter is currently matching against.
     * Owned by us; mutated on each notify::text. */
    char *prefix;

    /* Suppress notify::text reactions when WE're mutating the entry
     * (after accept_suggestion calls set_text). */
    gboolean updating;

    gulong text_handler;
    /* Key controller lives on the entry; not a signal handler id,
     * just a widget we add. */
    GtkEventController *key_controller;
};

/* ---- Tiny GObject wrapper around a UTF-8 name ----
 *
 * GtkStringList would do the job, but its model items are
 * GtkStringObject without a "kind" hint; we'll likely want to
 * differentiate folder/file rows visually later (icon). Keeping
 * our own one-string object now means that's a one-property
 * extension when the time comes.
 */

#define HX_TYPE_STR (hx_str_get_type ())
G_DECLARE_FINAL_TYPE (HxStr, hx_str, HX, STR, GObject)

struct _HxStr {
    GObject parent_instance;
    char *value;
};

G_DEFINE_FINAL_TYPE (HxStr, hx_str, G_TYPE_OBJECT)

static void
hx_str_finalize (GObject *obj)
{
    HxStr *s = (HxStr *)obj;
    g_clear_pointer (&s->value, g_free);
    G_OBJECT_CLASS (hx_str_parent_class)->finalize (obj);
}

static void
hx_str_class_init (HxStrClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = hx_str_finalize;
}

static void
hx_str_init (HxStr *self)
{
    (void)self;
}

static HxStr *
hx_str_new (const char *v)
{
    HxStr *s = g_object_new (HX_TYPE_STR, NULL);
    s->value = g_strdup (v);
    return s;
}

static const char *
hx_str_value (HxStr *s)
{
    return s ? s->value : NULL;
}

/* ---- Smart-case match ---- */

/* Returns TRUE if `s` contains any uppercase Unicode character.
 * Used to decide between case-sensitive and case-insensitive
 * matching. */
static gboolean
has_upper (const char *s)
{
    if (!s) {
        return FALSE;
    }
    for (const char *p = s; *p;) {
        gunichar c = g_utf8_get_char (p);
        if (g_unichar_isupper (c)) {
            return TRUE;
        }
        p = g_utf8_next_char (p);
    }
    return FALSE;
}

static gboolean
prefix_matches (const char *name, const char *prefix)
{
    if (!prefix || !*prefix) {
        return TRUE; /* empty prefix → show all */
    }
    if (has_upper (prefix)) {
        /* Sensitive — direct prefix compare. */
        return g_str_has_prefix (name, prefix);
    }
    /* Insensitive — fold both to a canonical form and compare. We
     * use g_utf8_casefold rather than ascii_strncasecmp to handle
     * non-ASCII filenames (the user might have a `Música` folder
     * and type `mú`). */
    char *n = g_utf8_casefold (name, -1);
    char *p = g_utf8_casefold (prefix, -1);
    gboolean ok = g_str_has_prefix (n, p);
    g_free (n);
    g_free (p);
    return ok;
}

/* GtkCustomFilter match callback. */
static gboolean
filter_match (gpointer item, gpointer user_data)
{
    HxStr *s = item;
    hx_path_complete *c = user_data;
    const char *name = hx_str_value (s);
    if (!name || !*name) {
        return FALSE;
    }
    /* Hidden files: only show if the user is explicitly typing a
     * dot at the start of the prefix. */
    if (name[0] == '.') {
        if (!c->prefix || c->prefix[0] != '.') {
            return FALSE;
        }
    }
    return prefix_matches (name, c->prefix);
}

/* ---- Directory enumeration ---- */

/* Comparator for sorting the listing alphabetically (case-insensitive,
 * UTF-8 aware). Folder-only listings don't need a folder/file split
 * sort, so this is straightforward. */
static int
cmp_hxstr (gconstpointer a, gconstpointer b, gpointer user_data)
{
    /* g_ptr_array_sort_with_data hands the comparator pointers to
	 * the array slots (HxStr**), not the items themselves. Cast
	 * and dereference one level. (My earlier `*(HxStr *const *)&a`
	 * was taking the address of the local arg `a` and reading
	 * back the arg's value as an HxStr* — wild pointer, crash in
	 * g_utf8_casefold.) */
    HxStr *const *pa = a;
    HxStr *const *pb = b;
    const char *as = hx_str_value (*pa);
    const char *bs = hx_str_value (*pb);
    char *af, *bf;
    int r;
    (void)user_data;
    af = g_utf8_casefold (as ? as : "", -1);
    bf = g_utf8_casefold (bs ? bs : "", -1);
    r = g_strcmp0 (af, bf);
    g_free (af);
    g_free (bf);
    return r;
}

/* Replace store contents with the directories living directly
 * under `dir`. Files and symlinks-to-non-directories are skipped.
 * On enumeration error we just empty the store and return —
 * caller decides what to do about the empty popover (it'll
 * silently hide). */
static void
enumerate_dir (hx_path_complete *c, const char *dir)
{
    GFile *gfile;
    GFileEnumerator *enumr;
    GError *err = NULL;
    GFileInfo *info;
    GPtrArray *names;

    g_list_store_remove_all (c->store);

    gfile = g_file_new_for_path (dir);
    /* G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS: a symlink to a
     * directory should still be navigable as a directory, so we
     * DO want to follow. Use the default flags (0) which DOES
     * dereference symlinks. */
    enumr = g_file_enumerate_children (gfile,
                                       G_FILE_ATTRIBUTE_STANDARD_NAME
                                       "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                       G_FILE_QUERY_INFO_NONE, NULL, &err);
    g_object_unref (gfile);
    if (!enumr) {
        g_clear_error (&err);
        return;
    }

    /* Collect then sort, then append. Two reasons:
     *   1. GListStore has no sort-in-place API, only items-changed
     *      emissions; doing the sort outside is cheaper.
     *   2. Sorted output gives a stable suggestion order across
     *      keystrokes, which is what the user expects. */
    names = g_ptr_array_new_with_free_func (g_object_unref);

    while ((info = g_file_enumerator_next_file (enumr, NULL, NULL)) != NULL) {
        GFileType ft = g_file_info_get_file_type (info);
        if (ft == G_FILE_TYPE_DIRECTORY) {
            const char *nm = g_file_info_get_name (info);
            if (nm && *nm) {
                g_ptr_array_add (names, hx_str_new (nm));
            }
        }
        g_object_unref (info);
    }

    g_file_enumerator_close (enumr, NULL, NULL);
    g_object_unref (enumr);

    g_ptr_array_sort_with_data (names, cmp_hxstr, NULL);

    for (guint i = 0; i < names->len; i++) {
        g_list_store_append (c->store, names->pdata[i]);
    }
    g_ptr_array_unref (names);
}

/* ---- Popover positioning + suggest cycle ---- */

/* Re-anchor and pop up the popover under the entry. Called every
 * time we have matches to show. GtkPopover's positioning is
 * sticky once popped, so we don't need to re-anchor every time —
 * but defensive anchor doesn't hurt. */
static void
show_popover (hx_path_complete *c)
{
    /* Match width of entry approximately. The popover's natural
     * size comes from its content; we set min-width on the
     * scrolled window so single-line entries don't paint a tiny
     * 100 px wide popover. */
    int w = gtk_widget_get_width (GTK_WIDGET (c->entry));
    if (w > 0) {
        gtk_widget_set_size_request (c->popover, w, -1);
    }
    gtk_popover_popup (GTK_POPOVER (c->popover));
}

static void
hide_popover (hx_path_complete *c)
{
    gtk_popover_popdown (GTK_POPOVER (c->popover));
}

/* Recompute the suggestion list from the entry's current text
 * and show/hide the popover accordingly. Called from notify::text
 * (and from accept_suggestion, after the entry has been mutated).
 *
 * The entry's text is split at the LAST '/'. Everything up to and
 * including that slash is `dir` (enumerated via GIO); the
 * remainder is `prefix` (passed to the filter).
 *
 * No leading slash → no path → hide the popover. We don't try to
 * complete relative paths; the local panel always shows an
 * absolute path in the entry. */
static void
update_completions (hx_path_complete *c)
{
    const char *txt;
    const char *slash;
    char *dir;
    char *new_prefix;

    if (c->updating) {
        return;
    }

    /* Don't fire when the entry text changed programmatically —
	 * i.e. the panel's on_navigated handler doing
	 * gtk_editable_set_text on every directory change. We only
	 * want to suggest while the USER is typing. Walking up from
	 * the window's focus widget is the reliable way to check —
	 * gtk_widget_has_focus(entry) does NOT do the right thing,
	 * because GtkEntry forwards focus to an internal GtkText
	 * delegate, so when the user is editing, gtk_window_get_focus
	 * returns the GtkText, not our entry. */
    {
        GtkRoot *root;
        GtkWidget *focused;
        gboolean effectively_focused = FALSE;

        root = gtk_widget_get_root (GTK_WIDGET (c->entry));
        focused = GTK_IS_WINDOW (root)
                      ? gtk_window_get_focus (GTK_WINDOW (root))
                      : NULL;
        while (focused) {
            if (focused == GTK_WIDGET (c->entry)) {
                effectively_focused = TRUE;
                break;
            }
            focused = gtk_widget_get_parent (focused);
        }
        if (!effectively_focused) {
            hide_popover (c);
            return;
        }
    }

    txt = gtk_editable_get_text (GTK_EDITABLE (c->entry));
    if (!txt || !*txt) {
        hide_popover (c);
        return;
    }

    slash = strrchr (txt, '/');
    if (!slash) {
        hide_popover (c);
        return;
    }
    if (slash == txt) {
        dir = g_strdup ("/");
    } else {
        dir = g_strndup (txt, (gsize)(slash - txt) + 1);
    }
    new_prefix = g_strdup (slash + 1);

    /* Re-enumerate only when the parent directory changed. Typing
     * within a directory just refilters the in-memory listing. */
    if (g_strcmp0 (dir, c->last_dir) != 0) {
        enumerate_dir (c, dir);
        g_free (c->last_dir);
        c->last_dir = dir;
    } else {
        g_free (dir);
    }

    g_free (c->prefix);
    c->prefix = new_prefix;

    /* GTK_FILTER_CHANGE_DIFFERENT means "the matched set may have
     * changed in arbitrary ways". Cheaper variants exist (e.g.
     * MORE_STRICT when we've added a character to the prefix)
     * but for a directory of typical size it's not worth the
     * bookkeeping. */
    gtk_filter_changed (GTK_FILTER (c->filter), GTK_FILTER_CHANGE_DIFFERENT);

    /* Cap to MAX_SUGGESTIONS rows. GtkFilterListModel respects
     * GtkSliceListModel composition for that, but for the cap
     * count we just measure the result and choose visibility.
     * No slice model needed because GtkListView is virtual — only
     * visible rows are realised. */
    guint n = g_list_model_get_n_items (G_LIST_MODEL (c->filter_model));
    if (n == 0) {
        hide_popover (c);
        return;
    }
    /* Preselect the first match so Tab/Enter has something to
     * insert without the user having to arrow down first. */
    gtk_single_selection_set_selected (c->selection, 0);
    show_popover (c);
}

/* Apply the suggestion at row `pos` to the entry: replace the
 * basename-prefix portion with the suggestion's name and append a
 * trailing '/'. The trailing slash both signals "this is a
 * directory" and naturally re-triggers completion for the next
 * segment. */
static void
accept_suggestion (hx_path_complete *c, guint pos)
{
    HxStr *s;
    const char *name;
    char *new_text;

    s = g_list_model_get_item (G_LIST_MODEL (c->filter_model), pos);
    if (!s) {
        return;
    }
    name = hx_str_value (s);
    new_text = g_strdup_printf ("%s%s/", c->last_dir ? c->last_dir : "/", name);

    c->updating = TRUE;
    gtk_editable_set_text (GTK_EDITABLE (c->entry), new_text);
    gtk_editable_set_position (GTK_EDITABLE (c->entry), -1);
    c->updating = FALSE;

    g_free (new_text);
    g_object_unref (s);

    /* Re-run for the next segment — typing keeps going from here. */
    update_completions (c);
}

/* ---- Signal handlers ---- */

static void
on_text_notify (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void)obj;
    (void)pspec;
    update_completions ((hx_path_complete *)user_data);
}

static void
on_row_activated (GtkListView *view, guint pos, gpointer user_data)
{
    (void)view;
    accept_suggestion ((hx_path_complete *)user_data, pos);
}

/* Key handling on the ENTRY: we want to capture Up/Down/Esc/Tab
 * even though the popover is "open" alongside the entry — focus
 * stays on the entry while it's active. Connect at the BUBBLE
 * phase so the entry's own bindings (Home/End/etc.) still work. */
static gboolean
on_key_pressed (GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                GdkModifierType state, gpointer user_data)
{
    hx_path_complete *c = user_data;
    guint n;
    guint sel;
    gboolean popped;

    (void)ctrl;
    (void)keycode;
    (void)state;

    popped = gtk_widget_get_visible (c->popover);

    switch (keyval) {
    case GDK_KEY_Escape:
        if (popped) {
            hide_popover (c);
            return TRUE;
        }
        return FALSE;
    case GDK_KEY_Down:
        if (!popped) {
            return FALSE;
        }
        n = g_list_model_get_n_items (G_LIST_MODEL (c->selection));
        if (n == 0) {
            return FALSE;
        }
        sel = gtk_single_selection_get_selected (c->selection);
        if (sel == GTK_INVALID_LIST_POSITION) {
            sel = 0;
        } else if (sel + 1 < n) {
            sel++;
        }
        gtk_single_selection_set_selected (c->selection, sel);
        return TRUE;
    case GDK_KEY_Up:
        if (!popped) {
            return FALSE;
        }
        sel = gtk_single_selection_get_selected (c->selection);
        if (sel == GTK_INVALID_LIST_POSITION || sel == 0) {
            return TRUE; /* swallow */
        }
        gtk_single_selection_set_selected (c->selection, sel - 1);
        return TRUE;
    case GDK_KEY_Tab:
        if (!popped) {
            return FALSE;
        }
        n = g_list_model_get_n_items (G_LIST_MODEL (c->selection));
        if (n == 0) {
            return FALSE;
        }
        sel = gtk_single_selection_get_selected (c->selection);
        if (sel == GTK_INVALID_LIST_POSITION) {
            sel = 0;
        }
        accept_suggestion (c, sel);
        return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        /* Enter only intercepted when the popover is up AND the
         * user has demonstrably interacted with it (selection
         * moved past the auto-selected row 0). Otherwise fall
         * through to the entry's activate handler, which navigates
         * to whatever's typed — letting the user type a full path
         * and just hit Enter to go, without having to dismiss the
         * popover first. */
        if (!popped) {
            return FALSE;
        }
        /* Heuristic: if the visible text matches exactly one
         * suggestion (i.e. the prefix == the directory name with
         * its own trailing /), prefer the typed text — they've
         * spelled the whole thing out. Otherwise commit the
         * highlighted suggestion. */
        /* Simpler heuristic: only commit-from-popover if the user
         * has moved the selection (current pos != 0). Otherwise
         * fall through. This matches what most shell-style
         * completion UIs do. */
        sel = gtk_single_selection_get_selected (c->selection);
        if (sel == 0 || sel == GTK_INVALID_LIST_POSITION) {
            return FALSE;
        }
        accept_suggestion (c, sel);
        return TRUE;
    }
    return FALSE;
}

/* ---- Row factory for the popover's listview ---- */

static void
row_setup (GtkSignalListItemFactory *fac, GObject *obj, gpointer user_data)
{
    GtkListItem *item = GTK_LIST_ITEM (obj);
    GtkWidget *label;
    (void)fac;
    (void)user_data;
    label = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
    gtk_widget_set_margin_start (label, 8);
    gtk_widget_set_margin_end (label, 8);
    gtk_widget_set_margin_top (label, 4);
    gtk_widget_set_margin_bottom (label, 4);
    gtk_list_item_set_child (item, label);
}

static void
row_bind (GtkSignalListItemFactory *fac, GObject *obj, gpointer user_data)
{
    GtkListItem *item = GTK_LIST_ITEM (obj);
    HxStr *s = HX_STR (gtk_list_item_get_item (item));
    GtkWidget *label = gtk_list_item_get_child (item);
    (void)fac;
    (void)user_data;
    gtk_label_set_text (GTK_LABEL (label), hx_str_value (s));
}

/* ---- Public API ---- */

hx_path_complete *
hx_path_complete_attach (GtkEntry *entry)
{
    hx_path_complete *c;
    GtkListItemFactory *fac;
    GtkWidget *scrolled;

    g_return_val_if_fail (GTK_IS_ENTRY (entry), NULL);

    c = g_new0 (hx_path_complete, 1);
    c->entry = entry;

    /* Model: GListStore<HxStr> -> GtkFilterListModel (custom
     * smart-case filter, keyed on c->prefix) -> GtkSingleSelection
     * -> GtkListView. */
    c->store = g_list_store_new (HX_TYPE_STR);
    c->filter
        = (GtkCustomFilter *)gtk_custom_filter_new (filter_match, c, NULL);
    c->filter_model = gtk_filter_list_model_new (G_LIST_MODEL (c->store),
                                                 GTK_FILTER (c->filter));
    c->selection = (GtkSingleSelection *)gtk_single_selection_new (
        G_LIST_MODEL (c->filter_model));
    /* Don't auto-select; we drive selection explicitly so the
     * pre-popup state ("nothing chosen yet") is distinguishable
     * from "row 0 is the user's choice". */
    gtk_single_selection_set_autoselect (c->selection, FALSE);
    gtk_single_selection_set_can_unselect (c->selection, TRUE);

    /* Row factory. */
    fac = gtk_signal_list_item_factory_new ();
    g_signal_connect (fac, "setup", G_CALLBACK (row_setup), c);
    g_signal_connect (fac, "bind", G_CALLBACK (row_bind), c);

    c->listview = gtk_list_view_new (GTK_SELECTION_MODEL (c->selection), fac);
    gtk_list_view_set_single_click_activate (GTK_LIST_VIEW (c->listview), TRUE);
    g_signal_connect (c->listview, "activate", G_CALLBACK (on_row_activated),
                      c);

    /* Scrolled window caps height. The min-content-height keeps
     * the popover compact when there are few matches; the max-
     * content-height caps it for big directories. */
    scrolled = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scrolled),
                                                32 * POPOVER_VISIBLE_ROWS);
    gtk_scrolled_window_set_propagate_natural_height (
        GTK_SCROLLED_WINDOW (scrolled), TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), c->listview);

    /* Popover: anchored to the entry, below. autohide = FALSE so
     * a click on a popover row doesn't lose focus on the entry
     * (we want the user to be able to keep typing); we manually
     * close on Escape and on cross-focus. has_arrow = FALSE for
     * the standard "dropdown" look. */
    c->popover = gtk_popover_new ();
    gtk_popover_set_has_arrow (GTK_POPOVER (c->popover), FALSE);
    gtk_popover_set_autohide (GTK_POPOVER (c->popover), FALSE);
    gtk_popover_set_position (GTK_POPOVER (c->popover), GTK_POS_BOTTOM);
    gtk_widget_set_parent (c->popover, GTK_WIDGET (entry));
    gtk_popover_set_child (GTK_POPOVER (c->popover), scrolled);
    gtk_widget_add_css_class (c->popover, "menu");

    /* Refresh on every keystroke. Cheap — refilter is O(N) in
     * directory size. */
    c->text_handler = g_signal_connect (entry, "notify::text",
                                        G_CALLBACK (on_text_notify), c);

    /* Key handling: install a capture-phase controller so we get
     * Up/Down/Tab/Esc/Return BEFORE the entry's own bindings (the
     * entry would otherwise swallow Up/Down and move the cursor;
     * Return triggers activate). */
    c->key_controller = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (c->key_controller,
                                                GTK_PHASE_CAPTURE);
    g_signal_connect (c->key_controller, "key-pressed",
                      G_CALLBACK (on_key_pressed), c);
    gtk_widget_add_controller (GTK_WIDGET (entry), c->key_controller);

    return c;
}

void
hx_path_complete_free (hx_path_complete *c)
{
    if (!c) {
        return;
    }
    if (c->entry && c->text_handler) {
        g_signal_handler_disconnect (c->entry, c->text_handler);
    }
    if (c->key_controller && c->entry) {
        gtk_widget_remove_controller (GTK_WIDGET (c->entry), c->key_controller);
        /* gtk_widget_remove_controller takes the ref. */
    }
    /* Ownership chain set up in hx_path_complete_new:
	 *   gtk_filter_list_model_new (store, filter)   — consumes both
	 *   gtk_single_selection_new (filter_model)     — consumes it
	 *   gtk_list_view_new (selection, factory)      — consumes both
	 *   gtk_scrolled_window_set_child (..., listview)
	 *   gtk_popover_set_child (popover, scrolled)
	 *   gtk_widget_set_parent (popover, entry)
	 *
	 * Every cached pointer in the struct (store, filter,
	 * filter_model, selection, listview) is held alive only
	 * transitively through the popover. Unparenting the popover
	 * disposes the chain top-down and our cached pointers become
	 * dangling — so we must NOT g_object_unref them. Just NULL
	 * them and free the wrapper struct. */
    if (c->popover) {
        gtk_widget_unparent (c->popover);
        c->popover = NULL;
    }
    c->filter_model = NULL;
    c->filter = NULL;
    c->selection = NULL;
    c->store = NULL;
    c->listview = NULL;
    g_clear_pointer (&c->last_dir, g_free);
    g_clear_pointer (&c->prefix, g_free);
    g_free (c);
}
