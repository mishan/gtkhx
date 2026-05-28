/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Bookmarks management dialog. CRUD lives in bookmarks_io.c — that
 * file is intentionally GTK-free so the unit tests can drive it
 * without the libadwaita dep. See bookmarks.h for the public API.
 */

#include "config.h"

#include <string.h>

#include <gtk/gtk.h>
#include <adwaita.h>

#include "hx.h"
#include "bookmarks.h"
#include "connect.h"      /* valid_ciphers[] / valid_compressors[] for combos */
#include "toolbar.h"      /* toolbar_window for dialog transient_for */

#undef _
#include <glib/gi18n.h>


/* ============================================================
 * Management dialog
 *
 * Layout: GtkWindow (resizable, transient over toolbar) with an
 * AdwHeaderBar. Content is GtkPaned (horizontal):
 *
 *   start  GtkBox vertical
 *            ├ scrolled GtkListBox of bookmark rows
 *            └ button row: [+ New]  [Delete]
 *   end    AdwPreferencesPage with one group "Bookmark":
 *            ├ AdwEntryRow      Name
 *            ├ AdwEntryRow      Server
 *            ├ AdwEntryRow      Port
 *            ├ AdwEntryRow      Login
 *            ├ AdwPasswordEntryRow Password
 *            ├ AdwSwitchRow     HOPE
 *            ├ AdwComboRow      Cipher
 *            ├ AdwComboRow      Compression
 *          + Save button below
 * ============================================================ */

typedef struct {
    GtkWidget *window;
    GtkWidget *list_box;
    GtkWidget *new_btn;
    GtkWidget *delete_btn;
    GtkWidget *save_btn;

    /* Detail form */
    GtkWidget *name_row;
    GtkWidget *server_row;
    GtkWidget *port_row;
    GtkWidget *login_row;
    GtkWidget *pass_row;
    GtkWidget *hope_row;
    GtkWidget *cipher_row;
    GtkWidget *compress_row;
    GtkWidget *tls_row;

    GtkWidget *empty_status;  /* AdwStatusPage shown when no selection */
    GtkWidget *detail_form;   /* The form box, hidden when empty */

    gulong row_selected_id;   /* signal handler id on list_box::row-selected,
                                so on_new_clicked can block it around the
                                programmatic unselect_all and avoid the
                                load_selection re-entrancy that would free
                                w->current under our feet */

    /* Currently-bound bookmark. NULL when empty / nothing selected.
	 * Heap-owned; freed on swap or window close. The "original_name"
	 * is what's stored on disk — different from name_row's text when
	 * the user has typed a rename. */
    HxBookmark *current;
    char *original_name;
} BookmarksWindow;

/* Singleton so reopening the menu item focuses the existing window
 * rather than building a second one. */
static BookmarksWindow *the_bookmarks_win = NULL;

static void rebuild_list (BookmarksWindow *w, const char *select_name);
static void load_selection (BookmarksWindow *w);
static void show_empty_state (BookmarksWindow *w);
static void show_detail_state (BookmarksWindow *w);

static void
toast_error (BookmarksWindow *w, const char *msg)
{
    AdwDialog *dialog;
    (void)w;
    dialog = adw_alert_dialog_new (_ ("Bookmark error"), msg);
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "ok", _ ("_OK"));
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "ok");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "ok");
    adw_dialog_present (dialog, w->window);
}

static void
on_window_destroy (GtkWidget *widget, gpointer data)
{
    BookmarksWindow *w = data;
    (void)widget;
    if (the_bookmarks_win == w) {
        the_bookmarks_win = NULL;
    }
    /* Disconnect the row-selected handler before freeing w. GTK 4
	 * tears the window down by unparenting children, and an unparent
	 * of the currently-selected row makes the list_box emit
	 * row-selected with row=NULL. If our handler fires after this
	 * function runs, it dereferences a freed w; if it fires *during*
	 * teardown (with widgets mid-dispose), load_selection touches
	 * AdwEntryRows whose internal state is already torn down. Both
	 * paths crash. Explicit disconnect closes both windows. */
    if (w->row_selected_id != 0 && w->list_box != NULL
        && GTK_IS_LIST_BOX (w->list_box)) {
        g_signal_handler_disconnect (w->list_box, w->row_selected_id);
        w->row_selected_id = 0;
    }
    hx_bookmark_free (w->current);
    g_free (w->original_name);
    g_free (w);
}

/* ----------------------------------------------------------
 * Form ↔ HxBookmark plumbing
 * ---------------------------------------------------------- */

static guint
clamp_combo_index (AdwComboRow *combo, guint idx)
{
    GListModel *model = adw_combo_row_get_model (combo);
    guint n;
    if (!model) {
        return 0;
    }
    n = g_list_model_get_n_items (model);
    if (n == 0) {
        return 0;
    }
    return idx >= n ? n - 1 : idx;
}

static void
form_from_bookmark (BookmarksWindow *w, const HxBookmark *bm)
{
    gtk_editable_set_text (GTK_EDITABLE (w->name_row), bm->name ? bm->name : "");
    gtk_editable_set_text (GTK_EDITABLE (w->server_row), bm->server);
    gtk_editable_set_text (GTK_EDITABLE (w->port_row), bm->port);
    gtk_editable_set_text (GTK_EDITABLE (w->login_row), bm->login);
    gtk_editable_set_text (GTK_EDITABLE (w->pass_row), bm->pass);
    adw_switch_row_set_active (ADW_SWITCH_ROW (w->hope_row), bm->secure != 0);
    adw_switch_row_set_active (ADW_SWITCH_ROW (w->tls_row), bm->tls != 0);
    /* combo indexes: 0 = "Off", 1..N = valid_*[N-1]. The on-disk
	 * value matches the combo's selected index, so passing it
	 * through unmodified is correct. Clamp to the model's actual
	 * length: a corrupted or future-format bookmark could carry a
	 * cipher/compress byte that exceeds what we know how to render,
	 * and AdwComboRow would silently fall back to 0 in a way that
	 * hides the bug instead of pinning to the last known value. */
    adw_combo_row_set_selected (
        ADW_COMBO_ROW (w->cipher_row),
        clamp_combo_index (ADW_COMBO_ROW (w->cipher_row),
                           (guint)(unsigned char)bm->cipher));
    adw_combo_row_set_selected (
        ADW_COMBO_ROW (w->compress_row),
        clamp_combo_index (ADW_COMBO_ROW (w->compress_row),
                           (guint)(unsigned char)bm->compress));
}

static void
form_to_bookmark (BookmarksWindow *w, HxBookmark *bm)
{
    const char *s;

    s = gtk_editable_get_text (GTK_EDITABLE (w->server_row));
    g_strlcpy (bm->server, s ? s : "", sizeof (bm->server));

    s = gtk_editable_get_text (GTK_EDITABLE (w->port_row));
    g_strlcpy (bm->port, s ? s : "", sizeof (bm->port));

    s = gtk_editable_get_text (GTK_EDITABLE (w->login_row));
    g_strlcpy (bm->login, s ? s : "", sizeof (bm->login));

    s = gtk_editable_get_text (GTK_EDITABLE (w->pass_row));
    g_strlcpy (bm->pass, s ? s : "", sizeof (bm->pass));

    bm->secure
        = adw_switch_row_get_active (ADW_SWITCH_ROW (w->hope_row)) ? 1 : 0;
    bm->cipher
        = (char)adw_combo_row_get_selected (ADW_COMBO_ROW (w->cipher_row));
    bm->compress
        = (char)adw_combo_row_get_selected (ADW_COMBO_ROW (w->compress_row));
    bm->tls
        = adw_switch_row_get_active (ADW_SWITCH_ROW (w->tls_row)) ? 1 : 0;
}

/* ----------------------------------------------------------
 * Empty / detail state toggle
 * ---------------------------------------------------------- */

static void
show_empty_state (BookmarksWindow *w)
{
    gtk_widget_set_visible (w->detail_form, FALSE);
    gtk_widget_set_visible (w->empty_status, TRUE);
    gtk_widget_set_sensitive (w->save_btn, FALSE);
    gtk_widget_set_sensitive (w->delete_btn, FALSE);
}

static void
show_detail_state (BookmarksWindow *w)
{
    gtk_widget_set_visible (w->empty_status, FALSE);
    gtk_widget_set_visible (w->detail_form, TRUE);
    gtk_widget_set_sensitive (w->save_btn, TRUE);
    gtk_widget_set_sensitive (w->delete_btn, TRUE);
}

/* ----------------------------------------------------------
 * Selection handling
 * ---------------------------------------------------------- */

static void
on_list_row_selected (GtkListBox *list_box, GtkListBoxRow *row, gpointer data)
{
    BookmarksWindow *w = data;
    (void)list_box;
    (void)row;
    load_selection (w);
}

static void
load_selection (BookmarksWindow *w)
{
    GtkListBoxRow *row = gtk_list_box_get_selected_row (GTK_LIST_BOX (w->list_box));
    const char *name;
    HxBookmark *bm;

    hx_bookmark_free (w->current);
    w->current = NULL;
    g_clear_pointer (&w->original_name, g_free);

    if (!row) {
        show_empty_state (w);
        return;
    }
    name = g_object_get_data (G_OBJECT (row), "bookmark-name");
    if (!name) {
        show_empty_state (w);
        return;
    }
    bm = hx_bookmark_load (name);
    if (!bm) {
        /* Could be a legacy-format file or something corrupt. Surface
		 * the error and leave the form empty — but record the row's
		 * filename in original_name so the Delete button still works,
		 * otherwise the user has no way to clear out a broken entry
		 * from the list. Save stays disabled because there's no
		 * loaded bookmark to write back. */
        g_autofree char *msg = g_strdup_printf (
            _ ("Could not load bookmark \"%s\". The file may be in the "
               "legacy format — pick it from the toolbar's Connect-button "
               "dropdown to convert it first."),
            name);
        toast_error (w, msg);
        w->original_name = g_strdup (name);
        show_empty_state (w);
        gtk_widget_set_sensitive (w->delete_btn, TRUE);
        return;
    }
    w->current = bm;
    w->original_name = g_strdup (bm->name);
    form_from_bookmark (w, bm);
    show_detail_state (w);
}

/* ----------------------------------------------------------
 * List build / rebuild
 * ---------------------------------------------------------- */

static GtkWidget *
make_list_row (const char *name)
{
    GtkWidget *row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), name);
    g_object_set_data_full (G_OBJECT (row), "bookmark-name",
                            g_strdup (name), g_free);
    return row;
}

static void
clear_list (GtkWidget *list_box)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child (list_box))) {
        gtk_list_box_remove (GTK_LIST_BOX (list_box), child);
    }
}

static void
rebuild_list (BookmarksWindow *w, const char *select_name)
{
    GList *names;
    GList *l;
    GtkListBoxRow *to_select = NULL;
    char *select_copy;

    /* select_name often points into w->current->name. Once we
	 * clear the list, GTK_SELECTION_BROWSE fires row-selected with
	 * a NULL row, load_selection frees w->current, and the pointer
	 * we're about to walk past every row to compare against turns
	 * into a use-after-free. dup it up front so the comparison
	 * stays valid no matter what the handler does. */
    select_copy = select_name ? g_strdup (select_name) : NULL;

    /* Block the row-selected handler across the whole rebuild. We
	 * don't want load_selection firing for the spurious intermediate
	 * states — NULL when clear_list removes the selected row, then
	 * the first appended row when GTK_SELECTION_BROWSE auto-selects
	 * the head of the new list. We'll call load_selection exactly
	 * once at the end, after the intended row is picked. */
    g_signal_handler_block (w->list_box, w->row_selected_id);

    clear_list (w->list_box);
    names = hx_bookmark_list ();
    for (l = names; l; l = l->next) {
        const char *nm = l->data;
        GtkWidget *row = make_list_row (nm);
        gtk_list_box_append (GTK_LIST_BOX (w->list_box), row);
        if (select_copy && g_strcmp0 (select_copy, nm) == 0) {
            to_select = GTK_LIST_BOX_ROW (row);
        }
    }
    g_list_free_full (names, g_free);
    g_free (select_copy);

    if (to_select) {
        gtk_list_box_select_row (GTK_LIST_BOX (w->list_box), to_select);
    }

    g_signal_handler_unblock (w->list_box, w->row_selected_id);

    /* Drive the state change explicitly — load_selection picks up
	 * whatever's now selected (the target row, or NULL when nothing
	 * matched) and swings the form / empty state accordingly. */
    load_selection (w);
}

/* ----------------------------------------------------------
 * Button handlers
 * ---------------------------------------------------------- */

static void
on_save_clicked (GtkButton *btn, gpointer data)
{
    BookmarksWindow *w = data;
    HxBookmark *bm = w->current;
    const char *raw_name;
    char *safe_name;
    GError *err = NULL;
    char *msg;
    (void)btn;

    if (!bm) {
        return;
    }
    raw_name = gtk_editable_get_text (GTK_EDITABLE (w->name_row));
    if (!raw_name || !*raw_name) {
        toast_error (w, _ ("Bookmark name cannot be empty."));
        return;
    }

    /* Canonicalize: hx_bookmark_save defangs '/' to '\\' on disk, so
	 * if we keep the raw user-typed name in bm->name, the in-memory
	 * "name" and the on-disk filename diverge — rebuild_list+select
	 * misses, and a later load reads back the defanged form. Push
	 * the canonical name through everything (form field included) so
	 * the user sees what's actually stored. */
    safe_name = hx_bookmark_safe_filename (raw_name);
    if (!safe_name) {
        toast_error (w, _ ("Bookmark name cannot be empty."));
        return;
    }
    if (strcmp (raw_name, safe_name) != 0) {
        gtk_editable_set_text (GTK_EDITABLE (w->name_row), safe_name);
    }

    form_to_bookmark (w, bm);

    /* Save flow has to land the on-disk state consistent with the
	 * form regardless of whether the write succeeds. Two failure
	 * modes to defend against:
	 *   1. Rename fails — the old file stays put with old contents,
	 *      we bail with the rename error before touching anything else.
	 *   2. Rename succeeds, then save fails — the file is now sitting
	 *      at the new name with the OLD contents (rename(2) just
	 *      moved the inode). Roll the rename back so the user's list
	 *      row keeps pointing at the same file with the same contents
	 *      it had before they hit Save.
	 *
	 * The alternative — write-then-rename via a tempfile — is more
	 * atomic but would mean diverging from hx_bookmark_save's direct
	 * fopen("w") shape just for this code path. Rollback is local
	 * and good enough: the worst case (save fails AND rollback
	 * fails) is rare and surfaces with a clear follow-up toast. */
    gboolean did_rename = FALSE;
    char *renamed_from = NULL;

    if (w->original_name && g_strcmp0 (safe_name, w->original_name) != 0) {
        if (!hx_bookmark_rename (w->original_name, safe_name, &err)) {
            msg = g_strdup_printf (_ ("Rename failed: %s"),
                                   err ? err->message : "(unknown)");
            toast_error (w, msg);
            g_free (msg);
            g_clear_error (&err);
            g_free (safe_name);
            return;
        }
        did_rename = TRUE;
        renamed_from = g_strdup (w->original_name);
        g_free (bm->name);
        bm->name = g_strdup (safe_name);
        g_free (w->original_name);
        w->original_name = g_strdup (safe_name);
    } else if (!w->original_name) {
        /* New bookmark — name is the user-supplied one (canonicalized). */
        g_free (bm->name);
        bm->name = g_strdup (safe_name);
        w->original_name = g_strdup (safe_name);
    }

    if (!hx_bookmark_save (bm, &err)) {
        char *save_err_msg = g_strdup (err ? err->message : "(unknown)");
        g_clear_error (&err);

        /* Roll the rename back so on-disk state matches what the
		 * user had before Save. If the rollback itself fails we end
		 * up with a stale file at the new name — surface a second
		 * sentence so the user knows. */
        if (did_rename && renamed_from) {
            GError *rb_err = NULL;
            if (hx_bookmark_rename (safe_name, renamed_from, &rb_err)) {
                g_free (bm->name);
                bm->name = g_strdup (renamed_from);
                g_free (w->original_name);
                w->original_name = g_strdup (renamed_from);
                msg = g_strdup_printf (
                    _ ("Save failed: %s. The bookmark was restored to "
                       "its original name."),
                    save_err_msg);
            } else {
                msg = g_strdup_printf (
                    _ ("Save failed: %1$s. The file is now at \"%2$s\" with "
                       "the previous contents — rollback also failed (%3$s)."),
                    save_err_msg, safe_name,
                    rb_err ? rb_err->message : "(unknown)");
                g_clear_error (&rb_err);
            }
        } else {
            msg = g_strdup_printf (_ ("Save failed: %s"), save_err_msg);
        }

        toast_error (w, msg);
        g_free (msg);
        g_free (save_err_msg);
        g_free (renamed_from);
        g_free (safe_name);
        return;
    }

    /* Refresh the toolbar's Connect dropdown so the new/renamed
	 * entry shows up there too. */
    toolbar_refresh_bookmarks ();
    rebuild_list (w, bm->name);
    g_free (renamed_from);
    g_free (safe_name);
}

static void
on_new_clicked (GtkButton *btn, gpointer data)
{
    BookmarksWindow *w = data;
    HxBookmark *bm = hx_bookmark_new ();
    (void)btn;

    /* Reasonable defaults. Name empty so the user starts typing
	 * into a clean field — populating with a placeholder like
	 * "New Bookmark" tempts the user to hit Save without renaming,
	 * which produces a file called literally "New Bookmark".
	 * Server/login/pass blank; port empty (client defaults to
	 * 5500). HOPE on with library-default cipher = combo index 0
	 * (which is "Off"); user picks something from the combos as
	 * they fill the form in. */
    bm->name = g_strdup ("");
    bm->secure = 1;

    /* Replace the current binding. */
    hx_bookmark_free (w->current);
    w->current = bm;
    g_clear_pointer (&w->original_name, g_free);
    /* w->original_name stays NULL → on Save, the rename branch
	 * doesn't fire and the new name from name_row becomes the
	 * file. */

    /* Clear list selection so the row click doesn't reload the
	 * old binding behind our back. Block our row-selected handler
	 * across the unselect_all call: GtkListBox::row-selected fires
	 * synchronously with row=NULL, and load_selection() would free
	 * w->current (which we just set to `bm`) before we ever bound
	 * it to the form — classic use-after-free. */
    g_signal_handler_block (w->list_box, w->row_selected_id);
    gtk_list_box_unselect_all (GTK_LIST_BOX (w->list_box));
    g_signal_handler_unblock (w->list_box, w->row_selected_id);

    form_from_bookmark (w, bm);
    show_detail_state (w);
    gtk_widget_grab_focus (w->name_row);
    gtk_editable_select_region (GTK_EDITABLE (w->name_row), 0, -1);
}

static void
on_delete_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
    BookmarksWindow *w = data;
    GError *err = NULL;
    char *msg;
    char *name;

    (void)dialog;
    if (g_strcmp0 (response, "delete") != 0) {
        return;
    }
    if (!w->original_name) {
        return;
    }
    name = g_strdup (w->original_name);
    if (!hx_bookmark_delete (name, &err)) {
        msg = g_strdup_printf (_ ("Delete failed: %s"),
                               err ? err->message : "(unknown)");
        toast_error (w, msg);
        g_free (msg);
        g_clear_error (&err);
        g_free (name);
        return;
    }
    g_free (name);
    toolbar_refresh_bookmarks ();
    rebuild_list (w, NULL);
}

static void
on_delete_clicked (GtkButton *btn, gpointer data)
{
    BookmarksWindow *w = data;
    AdwDialog *dialog;
    char *body;
    (void)btn;

    if (!w->original_name) {
        return;
    }
    body = g_strdup_printf (_ ("Delete the bookmark \"%s\"? This cannot be "
                               "undone."),
                            w->original_name);
    dialog = adw_alert_dialog_new (_ ("Delete Bookmark"), body);
    g_free (body);
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel",
                                   _ ("_Cancel"));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "delete",
                                   _ ("_Delete"));
    adw_alert_dialog_set_response_appearance (
        ADW_ALERT_DIALOG (dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "cancel");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
    g_signal_connect (dialog, "response", G_CALLBACK (on_delete_response), w);
    adw_dialog_present (dialog, w->window);
}

/* ----------------------------------------------------------
 * Window construction
 * ---------------------------------------------------------- */

static GtkStringList *
cipher_combo_strings (void)
{
    GtkStringList *list = gtk_string_list_new (NULL);
    int i;
    gtk_string_list_append (list, _ ("Off"));
    for (i = 0; valid_ciphers[i]; i++) {
        gtk_string_list_append (list, valid_ciphers[i]);
    }
    return list;
}

static GtkStringList *
compress_combo_strings (void)
{
    GtkStringList *list = gtk_string_list_new (NULL);
    int i;
    gtk_string_list_append (list, _ ("Off"));
    for (i = 0; valid_compressors[i]; i++) {
        gtk_string_list_append (list, valid_compressors[i]);
    }
    return list;
}

void
create_bookmarks_window (void)
{
    BookmarksWindow *w;
    GtkWidget *header;
    GtkWidget *paned;
    GtkWidget *sidebar_box;
    GtkWidget *scrolled;
    GtkWidget *button_row;
    GtkWidget *detail_box;
    GtkWidget *page;
    GtkWidget *group;
    GtkWidget *save_row;
    GtkEventController *shortcuts;
    GtkShortcut *sh;

    if (the_bookmarks_win) {
        gtk_window_present (GTK_WINDOW (the_bookmarks_win->window));
        return;
    }

    w = g_new0 (BookmarksWindow, 1);
    the_bookmarks_win = w;

    w->window = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (w->window), _ ("Bookmarks"));
    gtk_window_set_default_size (GTK_WINDOW (w->window), 760, 560);
    if (toolbar_window) {
        gtk_window_set_transient_for (GTK_WINDOW (w->window),
                                      GTK_WINDOW (toolbar_window));
    }
    g_signal_connect (w->window, "destroy", G_CALLBACK (on_window_destroy), w);

    header = adw_header_bar_new ();
    gtk_window_set_titlebar (GTK_WINDOW (w->window), header);

    /* Esc + Ctrl-W close. Same pattern as the User Editor. */
    shortcuts = gtk_shortcut_controller_new ();
    gtk_event_controller_set_propagation_phase (shortcuts, GTK_PHASE_CAPTURE);
    gtk_shortcut_controller_set_scope (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                       GTK_SHORTCUT_SCOPE_LOCAL);
    gtk_widget_add_controller (w->window, shortcuts);
    sh = gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                           gtk_named_action_new ("window.close"));
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                          sh);
    sh = gtk_shortcut_new (
        gtk_keyval_trigger_new (GDK_KEY_w, GDK_CONTROL_MASK),
        gtk_named_action_new ("window.close"));
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                          sh);

    /* Body: GtkPaned with sidebar on the left, detail form on the
	 * right. position 240 gives the list a comfortable width while
	 * leaving the form room to breathe. */
    paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position (GTK_PANED (paned), 240);
    gtk_paned_set_resize_start_child (GTK_PANED (paned), FALSE);
    gtk_paned_set_shrink_start_child (GTK_PANED (paned), FALSE);
    gtk_window_set_child (GTK_WINDOW (w->window), paned);

    /* ---- Sidebar ---- */
    sidebar_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request (sidebar_box, 200, -1);

    w->list_box = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (w->list_box),
                                     GTK_SELECTION_BROWSE);
    gtk_widget_add_css_class (w->list_box, "navigation-sidebar");
    w->row_selected_id
        = g_signal_connect (w->list_box, "row-selected",
                            G_CALLBACK (on_list_row_selected), w);

    scrolled = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand (scrolled, TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), w->list_box);
    gtk_box_append (GTK_BOX (sidebar_box), scrolled);

    /* + and Delete buttons under the list. Symbolic icons + tooltips
	 * keep the bottom row compact. */
    button_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start (button_row, 6);
    gtk_widget_set_margin_end (button_row, 6);
    gtk_widget_set_margin_top (button_row, 6);
    gtk_widget_set_margin_bottom (button_row, 6);

    w->new_btn = gtk_button_new_from_icon_name ("list-add-symbolic");
    gtk_widget_set_tooltip_text (w->new_btn, _ ("New bookmark"));
    g_signal_connect (w->new_btn, "clicked", G_CALLBACK (on_new_clicked), w);
    gtk_box_append (GTK_BOX (button_row), w->new_btn);

    w->delete_btn = gtk_button_new_from_icon_name ("list-remove-symbolic");
    gtk_widget_set_tooltip_text (w->delete_btn, _ ("Delete selected bookmark"));
    gtk_widget_add_css_class (w->delete_btn, "destructive-action");
    g_signal_connect (w->delete_btn, "clicked", G_CALLBACK (on_delete_clicked),
                      w);
    gtk_box_append (GTK_BOX (button_row), w->delete_btn);

    gtk_box_append (GTK_BOX (sidebar_box), button_row);
    gtk_paned_set_start_child (GTK_PANED (paned), sidebar_box);

    /* ---- Detail ---- */
    detail_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand (detail_box, TRUE);
    gtk_widget_set_vexpand (detail_box, TRUE);
    gtk_paned_set_end_child (GTK_PANED (paned), detail_box);

    /* Empty-state placeholder shown when no row is selected. */
    w->empty_status = adw_status_page_new ();
    adw_status_page_set_icon_name (ADW_STATUS_PAGE (w->empty_status),
                                   "user-bookmarks-symbolic");
    adw_status_page_set_title (ADW_STATUS_PAGE (w->empty_status),
                               _ ("No Bookmark Selected"));
    adw_status_page_set_description (
        ADW_STATUS_PAGE (w->empty_status),
        _ ("Pick one from the list, or create a new one with the + "
           "button."));
    gtk_widget_set_hexpand (w->empty_status, TRUE);
    gtk_widget_set_vexpand (w->empty_status, TRUE);
    gtk_box_append (GTK_BOX (detail_box), w->empty_status);

    /* Form */
    w->detail_form = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand (w->detail_form, TRUE);
    gtk_widget_set_vexpand (w->detail_form, TRUE);

    page = adw_preferences_page_new ();
    group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group),
                                     _ ("Bookmark"));

    w->name_row = adw_entry_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w->name_row),
                                   _ ("Name"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), w->name_row);

    w->server_row = adw_entry_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w->server_row),
                                   _ ("Server"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), w->server_row);

    w->port_row = adw_entry_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w->port_row),
                                   _ ("Port (blank = 5500)"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), w->port_row);

    w->login_row = adw_entry_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w->login_row),
                                   _ ("Login"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), w->login_row);

    w->pass_row = adw_password_entry_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w->pass_row),
                                   _ ("Password"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), w->pass_row);

    /* TLS comes first — toggling it grey-outs HOPE + cipher + compress
     * in the Connect dialog. The bookmarks editor doesn't currently
     * mirror that sensitivity coupling (the user could in principle
     * save a bookmark with both tls=1 and secure=1, in which case
     * connect_with_args forces HOPE off at connect time). */
    w->tls_row = adw_switch_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w->tls_row),
                                   _ ("Use TLS"));
    adw_action_row_set_subtitle (
        ADW_ACTION_ROW (w->tls_row),
        _ ("Connect to the server's TLS port. Disables HOPE and "
           "compression — they're not meaningful over a TLS-encrypted "
           "stream."));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), w->tls_row);

    w->hope_row = adw_switch_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w->hope_row),
                                   _ ("HOPE (encrypted handshake)"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), w->hope_row);

    w->cipher_row = adw_combo_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w->cipher_row),
                                   _ ("Cipher"));
    {
        /* cipher_combo_strings()/compress_combo_strings() return a
		 * floating-ref-free GtkStringList that we own. AdwComboRow's
		 * set_model takes its own reference, so we drop ours after the
		 * call to avoid leaking the list every time the dialog opens. */
        GtkStringList *cipher_model = cipher_combo_strings ();
        adw_combo_row_set_model (ADW_COMBO_ROW (w->cipher_row),
                                 G_LIST_MODEL (cipher_model));
        g_object_unref (cipher_model);
    }
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), w->cipher_row);

    w->compress_row = adw_combo_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w->compress_row),
                                   _ ("Compression"));
    {
        GtkStringList *compress_model = compress_combo_strings ();
        adw_combo_row_set_model (ADW_COMBO_ROW (w->compress_row),
                                 G_LIST_MODEL (compress_model));
        g_object_unref (compress_model);
    }
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), w->compress_row);

    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                              ADW_PREFERENCES_GROUP (group));
    gtk_widget_set_vexpand (page, TRUE);
    gtk_box_append (GTK_BOX (w->detail_form), page);

    /* Save button below the form. */
    save_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign (save_row, GTK_ALIGN_END);
    gtk_widget_set_margin_start (save_row, 12);
    gtk_widget_set_margin_end (save_row, 12);
    gtk_widget_set_margin_top (save_row, 4);
    gtk_widget_set_margin_bottom (save_row, 12);
    w->save_btn = gtk_button_new_with_label (_ ("Save"));
    gtk_widget_add_css_class (w->save_btn, "suggested-action");
    gtk_widget_add_css_class (w->save_btn, "pill");
    g_signal_connect (w->save_btn, "clicked", G_CALLBACK (on_save_clicked), w);
    gtk_box_append (GTK_BOX (save_row), w->save_btn);
    gtk_box_append (GTK_BOX (w->detail_form), save_row);

    gtk_widget_set_visible (w->detail_form, FALSE);
    gtk_box_append (GTK_BOX (detail_box), w->detail_form);

    rebuild_list (w, NULL);
    show_empty_state (w);

    gtk_window_present (GTK_WINDOW (w->window));
}
