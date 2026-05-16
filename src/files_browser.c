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
#include <adwaita.h>

#include "hx.h"        /* struct htxf_conn — auto-refresh hook */
#include "session.h"   /* the_session — remote drag uses htlc.access */
#include "hl_access.h" /* HL_ACCESS_DOWNLOAD_FILES */
#include "xfers.h"     /* xfer_new for remote drag-to-Downloads */
#include "prefs.h"     /* gtkhx_prefs.download_path */
#include "files.h"     /* hx_file_move for cross-dir Move */
#include "files_entry.h"
#include "files_provider.h"
#include "files_local_provider.h"
#include "files_remote_provider.h"
#include "files_panel.h"
#include "files_complete.h" /* path completion for the Move dialog's dest entry */
#include "files_ops.h"
#include "files_browser.h"
#include "gtkhx_session.h"
#include "gtkutil.h"

/* gi18n.h after the project headers — the codebase's compat.h has
 * a placeholder _ macro that we want overridden by the proper
 * gettext expansion. */
#undef _
#include <glib/gi18n.h>

/* ---- DnD payload type ----
 *
 * Carries a drag's source-panel pointer plus the snapshot of
 * selected entries at drag-start time. Boxed so it can ride
 * inside a GdkContentProvider value; gdk_content_provider_new_for_value
 * deep-copies via the registered copy func, so the source side
 * is free to release its locals after building the provider. */
typedef struct {
    files_panel *src_panel; /* weak: still validated at drop time */
    GPtrArray *entries;     /* HxFileEntry* with one ref each */
} HxFilesDrag;

static HxFilesDrag *
hx_files_drag_copy (HxFilesDrag *src)
{
    HxFilesDrag *dst = g_new0 (HxFilesDrag, 1);
    guint i;
    if (!src) {
        return dst;
    }
    dst->src_panel = src->src_panel;
    if (src->entries) {
        dst->entries = g_ptr_array_new_with_free_func (g_object_unref);
        for (i = 0; i < src->entries->len; i++) {
            HxFileEntry *e = g_ptr_array_index (src->entries, i);
            g_ptr_array_add (dst->entries, g_object_ref (e));
        }
    }
    return dst;
}

static void
hx_files_drag_free (HxFilesDrag *p)
{
    if (!p) {
        return;
    }
    if (p->entries) {
        g_ptr_array_free (p->entries, TRUE);
    }
    g_free (p);
}

/* Forward decl — G_DEFINE_BOXED_TYPE generates the body but
 * doesn't emit a prototype, so -Wmissing-prototypes complains. */
static GType hx_files_drag_get_type (void);
#define HX_TYPE_FILES_DRAG (hx_files_drag_get_type ())
G_DEFINE_BOXED_TYPE (HxFilesDrag, hx_files_drag, hx_files_drag_copy,
                     hx_files_drag_free)

/* The browser holds two panels + the active-panel marker. The
 * active panel is the one with the column-view focus child; we
 * detect focus changes by hooking each column-view's
 * has-focus notify and update both panels' CSS class +
 * `active_panel` accordingly. */
struct browser {
    GtkWidget *window;
    files_panel *left;
    files_panel *right;
    files_panel *active;

    /* Keep refs on the providers separately from the panels so
	 * the connection-state hook can reach the remote one even if
	 * the panel pointer ever needs to be swapped (Phase 2.5 side
	 * selector, future). */
    HxFilesProvider *left_provider;
    HxFilesProvider *right_provider;

    /* GtkhxSession::connection-state handler — fires the remote
	 * provider's "unavailable-changed" so the panel reloads on
	 * login + paints the not-connected state on disconnect. */
    gulong conn_state_handler;

    /* GtkhxSession::file-update handler — used to spot
	 * just-completed transfers and refresh both panels so the
	 * new file appears without the user needing to hit Reload. */
    gulong file_update_handler;

    /* CSS provider that paints the .files-panel-active border.
	 * Lives for the window's lifetime; unrefed in on_close. */
    GtkCssProvider *css;

    /* AdwToastOverlay wrapping the window content — used by the
	 * Copy action to surface "no permission" / "not connected" /
	 * etc. results without an interrupting dialog. */
    AdwToastOverlay *toast;

    /* Copy button — re-tooltip'd on active-panel change so the
	 * arrow direction in the hover text reflects which side is
	 * source vs. dest. */
    GtkWidget *btn_copy;
};

static struct browser *the_browser = NULL;

/* ---- Active-panel tracking ---- */

/* Update Copy's tooltip to reflect which way the transfer goes. */
static void
sync_copy_tooltip (struct browser *br)
{
    if (!br || !br->btn_copy) {
        return;
    }
    if (br->active == br->left) {
        gtk_widget_set_tooltip_text (br->btn_copy,
                                     _ ("Copy selection from left to right"));
    } else if (br->active == br->right) {
        gtk_widget_set_tooltip_text (br->btn_copy,
                                     _ ("Copy selection from right to left"));
    }
}

static void
set_active (struct browser *br, files_panel *p)
{
    if (!br || !p || br->active == p) {
        return;
    }
    files_panel_set_active (br->left, p == br->left);
    files_panel_set_active (br->right, p == br->right);
    br->active = p;
    sync_copy_tooltip (br);
}

static void
show_toast (struct browser *br, const char *text)
{
    if (!br || !br->toast || !text) {
        return;
    }
    adw_toast_overlay_add_toast (br->toast, adw_toast_new (text));
}

/* Wire a focus controller on a panel's root widget. The
 * controller's "enter" signal fires when focus moves into the
 * widget OR any descendant — that's the right semantic for the
 * active-panel marker. Hooking notify::has-focus on the column
 * view alone didn't work: the column view itself rarely gets
 * focus directly; its inner row widget does, and has-focus on
 * the parent doesn't reliably propagate.
 *
 * A click gesture in capture phase covers the case where the
 * user clicks somewhere that's not focusable (the path entry,
 * empty space below the rows) — we want that to flip the active
 * panel anyway so the headerbar actions follow the user's
 * pointer-driven intent. */
static void
on_panel_focus_enter (GtkEventControllerFocus *ctrl, gpointer user_data)
{
    struct browser *br = user_data;
    GtkWidget *panel_root;
    (void)ctrl;

    panel_root = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (ctrl));
    if (br->left && files_panel_get_widget (br->left) == panel_root) {
        set_active (br, br->left);
    } else if (br->right && files_panel_get_widget (br->right) == panel_root) {
        set_active (br, br->right);
    }
}

static void
on_panel_clicked (GtkGestureClick *gesture, int n_press, double x, double y,
                  gpointer user_data)
{
    struct browser *br = user_data;
    GtkWidget *panel_root;
    (void)n_press;
    (void)x;
    (void)y;

    panel_root
        = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
    if (br->left && files_panel_get_widget (br->left) == panel_root) {
        set_active (br, br->left);
    } else if (br->right && files_panel_get_widget (br->right) == panel_root) {
        set_active (br, br->right);
    }
}

static void
attach_panel_focus_tracking (struct browser *br, files_panel *p)
{
    GtkEventController *focus_ctrl;
    GtkGesture *click;
    GtkWidget *root = files_panel_get_widget (p);

    focus_ctrl = gtk_event_controller_focus_new ();
    g_signal_connect (focus_ctrl, "enter", G_CALLBACK (on_panel_focus_enter),
                      br);
    gtk_widget_add_controller (root, focus_ctrl);

    /* BUBBLE phase: column view sees the click first and runs
	 * its built-in click-counting (selection on first press,
	 * activate on second press of a double-click). We observe
	 * on the way back up to flip the active panel. The earlier
	 * CAPTURE-phase version of this gesture broke double-click
	 * activation on the non-active panel — the column view saw
	 * the first click of a double-click pair as just a
	 * selection-with-focus-shift and waited for another pair
	 * before treating it as a double. Symptom: first double-
	 * click in the remote panel did nothing, second double-
	 * click descended. BUBBLE phase keeps the active-flip
	 * working for all cases except clicks that the column view
	 * fully consumes — and even then the focus controller
	 * above catches focus-enter and flips active. */
    click = gtk_gesture_click_new ();
    gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click),
                                                GTK_PHASE_BUBBLE);
    g_signal_connect (click, "pressed", G_CALLBACK (on_panel_clicked), br);
    gtk_widget_add_controller (root, GTK_EVENT_CONTROLLER (click));
}

/* ---- Side selector: panel swap callback ----
 *
 * Each panel's side dropdown calls back here on a selection
 * change. We build a fresh HxLocalFilesProvider /
 * HxRemoteFilesProvider, hand it to the requesting panel, and
 * stash it on br->left_provider or br->right_provider so the
 * connection-state hook and the close-time cleanup can find it.
 *
 * Why fresh providers and not a shared singleton: providers
 * store current_path internally. Two panels sharing one
 * provider would have the same current_path, so navigating one
 * would yank the other. Fresh-per-side keeps them independent.
 *
 * Why the cross-side Move/Copy logic stays correct: those
 * checks use HX_IS_LOCAL_FILES_PROVIDER on
 * files_panel_get_provider(panel), which returns the current
 * provider. After a swap, the type check answers the new
 * reality on the next button click. */
static void
on_panel_swap_request (files_panel *p, gboolean want_local, gpointer user_data)
{
    struct browser *br = user_data;
    HxFilesProvider *new_prov;

    if (!br || !p) {
        return;
    }

    if (want_local) {
        new_prov = HX_FILES_PROVIDER (hx_local_files_provider_new (NULL));
    } else {
        new_prov = HX_FILES_PROVIDER (hx_remote_files_provider_new ());
    }
    if (!new_prov) {
        return;
    }

    /* Update br's provider-side cache. The browser cleanup path
	 * (on_close) and the connection-state hook key off these. */
    if (p == br->left) {
        g_clear_object (&br->left_provider);
        br->left_provider = g_object_ref (new_prov);
    } else if (p == br->right) {
        g_clear_object (&br->right_provider);
        br->right_provider = g_object_ref (new_prov);
    }

    files_panel_set_provider (p, new_prov);
    g_object_unref (new_prov);

    /* Copy-direction tooltip refresh: the destination kind may
	 * have changed, which affects the arrow direction we show. */
    sync_copy_tooltip (br);
}

/* ---- Actions (scoped to active panel) ---- */

static void
on_refresh_clicked (GtkButton *btn, gpointer user_data)
{
    struct browser *br = user_data;
    (void)btn;
    if (br->active) {
        hx_files_provider_reload (files_panel_get_provider (br->active));
    }
}

/* ---- Preview ----
 *
 * Routes through the provider's activate-entry vtable, which is the
 * same hook that fires on F4 / double-click. On a remote file that
 * kicks off an HTXF preview transfer (opt.preview=1, no on-disk
 * write — the preview window decodes bytes as they stream). On a
 * local file it g_app_info_launch's the OS default app. Folders
 * are no-ops here; descending happens through normal row-activate. */
static void
on_preview_clicked (GtkButton *btn, gpointer user_data)
{
    struct browser *br = user_data;
    HxFileEntry *e;
    (void)btn;

    if (!br->active) {
        return;
    }
    e = files_panel_get_single_selected (br->active);
    if (!e) {
        show_toast (br, _ ("Select a single file to preview."));
        return;
    }
    if (hx_file_entry_is_dir (e)) {
        show_toast (br, _ ("Preview is for files, not folders."));
        return;
    }
    hx_files_provider_activate_entry (files_panel_get_provider (br->active), e);
}

/* ---- Get Info ----
 *
 * Fires HTLC_HDR_FILE_GETINFO for the selected file on the active
 * panel. The wire request is remote-only; the existing rcv path
 * (rcv_task_file_getinfo → output_file_info → file-info GtkhxSession
 * signal → gtkhx.c::on_file_info_signal) opens the existing file-
 * info dialog. Local files have no analog in the Hotline protocol;
 * we toast a hint and bail when the active panel is local. */
static void
on_get_info_clicked (GtkButton *btn, gpointer user_data)
{
    struct browser *br = user_data;
    HxFilesProvider *prov;
    HxFileEntry *e;
    const char *dir, *name;
    (void)btn;

    if (!br->active) {
        return;
    }
    prov = files_panel_get_provider (br->active);
    if (!HX_IS_REMOTE_FILES_PROVIDER (prov)) {
        show_toast (br, _ ("Get Info is only available for remote files."));
        return;
    }
    if (!the_session.htlc.fd) {
        show_toast (br, _ ("Not connected."));
        return;
    }
    e = files_panel_get_single_selected (br->active);
    if (!e) {
        show_toast (br, _ ("Select a single file."));
        return;
    }

    dir = hx_files_provider_get_current_path (prov);
    name = hx_file_entry_get_name (e);
    hx_file_info (&the_session.htlc, dir ? dir : "/", name,
                  name ? strlen (name) : 0);
}

/* ---- Rename (F6 in classic Norton) ---- */

struct rename_ctx {
    struct browser *br;
    files_panel *panel;
    char *old_name;   /* owned */
    GtkWidget *entry; /* not owned — held by dialog */
};

static void
on_rename_response (AdwAlertDialog *dialog, const char *response,
                    gpointer user_data)
{
    struct rename_ctx *ctx = user_data;
    const char *new_name;
    GError *err = NULL;
    (void)dialog;

    if (g_strcmp0 (response, "rename") != 0) {
        return;
    }
    if (!ctx->panel || !ctx->old_name) {
        return;
    }
    new_name = gtk_editable_get_text (GTK_EDITABLE (ctx->entry));
    if (!new_name || !*new_name) {
        return;
    }
    if (g_strcmp0 (new_name, ctx->old_name) == 0) {
        return; /* nothing changed */
    }

    if (!hx_files_provider_rename (files_panel_get_provider (ctx->panel),
                                   ctx->old_name, new_name, &err)) {
        show_toast (ctx->br, err ? err->message : _ ("Rename failed."));
        g_clear_error (&err);
    }
}

static void
on_rename_closed (AdwAlertDialog *dialog, gpointer user_data)
{
    struct rename_ctx *ctx = user_data;
    (void)dialog;
    g_free (ctx->old_name);
    g_free (ctx);
}

static void
on_rename_clicked (GtkButton *btn, gpointer user_data)
{
    struct browser *br = user_data;
    HxFileEntry *e;
    AdwDialog *dialog;
    GtkWidget *entry;
    struct rename_ctx *ctx;
    char *body;
    (void)btn;

    if (!br->active) {
        return;
    }

    /* Rename is a singleton operation — only meaningful when
	 * exactly one row is selected. Multi-rename (mass rename
	 * with a pattern) is a separate feature; toast a hint
	 * and bail. */
    e = files_panel_get_single_selected (br->active);
    if (!e) {
        show_toast (br, _ ("Select a single file to rename."));
        return;
    }

    body = g_strdup_printf (_ ("Rename “%s” to:"), hx_file_entry_get_name (e));
    dialog = ADW_DIALOG (adw_alert_dialog_new (_ ("Rename"), body));
    g_free (body);

    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel",
                                   _ ("_Cancel"));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "rename",
                                   _ ("_Rename"));
    adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog),
                                              "rename", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "rename");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

    entry = gtk_entry_new ();
    gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
    gtk_editable_set_text (GTK_EDITABLE (entry), hx_file_entry_get_name (e));
    adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), entry);

    ctx = g_new0 (struct rename_ctx, 1);
    ctx->br = br;
    ctx->panel = br->active;
    ctx->old_name = g_strdup (hx_file_entry_get_name (e));
    ctx->entry = entry;

    g_signal_connect (dialog, "response", G_CALLBACK (on_rename_response), ctx);
    g_signal_connect (dialog, "closed", G_CALLBACK (on_rename_closed), ctx);

    adw_dialog_present (dialog, br->window);

    /* Focus + select all → user can either accept the default
	 * (just hit Enter to confirm same name = no-op safety) or
	 * start typing immediately to replace. Matches Files
	 * Manager UX in GNOME / Finder. */
    gtk_widget_grab_focus (entry);
    gtk_editable_select_region (GTK_EDITABLE (entry), 0, -1);
}

/* ---- Move (F6 cross-directory) ----
 *
 * Same-directory rename is covered by Rename. Move adds the
 * cross-directory case: rename(old, new) where old and new live
 * under different parents. Defaults the destination to the
 * INACTIVE panel's current path, since that's what the user
 * usually wants ("move these into the other side's directory").
 *
 * Wire-side, Hotline's MOVEFILE opcode (hx_file_move) is path-
 * aware and works for cross-directory moves on the same server.
 * GIO's g_file_move handles local cross-dir moves the same way.
 *
 * Cross-SIDE moves (local→remote or vice versa) aren't really
 * "moves" — there's no atomic primitive that spans filesystems,
 * Hotline's MOVEFILE doesn't either, and a download-then-upload
 * compound op silently degrades the cross-dir guarantee the
 * orthodox-FM convention promises. We refuse them at button-click
 * with a toast directing the user to Copy + Delete; mixed-side
 * Move isn't a common orthodox-FM operation anyway.
 *
 * Remote-side move feedback: hx_file_move is fire-and-forget on
 * the wire — the server acks the MOVEFILE task asynchronously
 * and any rejection (no permission, dest exists, file in use)
 * surfaces through the generic task_error toast on the toolbar
 * window. Our success line therefore reads "Move requested for N
 * items" on the remote side rather than "Moved" — the latter is
 * reserved for the synchronous local g_file_move path where we
 * actually know it worked. The two-toast UX (us optimistic +
 * toolbar pessimistic) is a known wart; a future polish pass
 * could route task_error toasts to the active files browser's
 * overlay when one is open. */

struct move_ctx {
    struct browser *br;
    files_panel *panel;
    GPtrArray *names;           /* owned — g_strdup'd source names */
    GtkWidget *entry;           /* dest-path entry */
    gboolean is_remote;         /* TRUE if the source side is the remote
                         * (HxRemoteFilesProvider). Remote moves
                         * are async — hx_file_move is fire-and-
                         * forget and any server-side error
                         * surfaces later through the generic
                         * task_error toast. Local moves are
                         * synchronous via g_file_move and we
                         * know the outcome at response time. */
    hx_path_complete *complete; /* NULL on the remote-source path
                                 * (synchronous filesystem
                                 * enumeration would block on the
                                 * network), populated for the
                                 * local-source path. Mirrors the
                                 * same gate files_panel uses on
                                 * its own path entries. Freed in
                                 * on_move_closed. */
};

static void
on_move_response (AdwAlertDialog *dialog, const char *response,
                  gpointer user_data)
{
    struct move_ctx *ctx = user_data;
    HxFilesProvider *prov;
    const char *dest_dir, *src_dir;
    guint i, moved = 0, failed = 0;
    GError *last_err = NULL;
    (void)dialog;

    if (g_strcmp0 (response, "move") != 0) {
        return;
    }
    if (!ctx->panel || !ctx->names) {
        return;
    }

    prov = files_panel_get_provider (ctx->panel);
    dest_dir = gtk_editable_get_text (GTK_EDITABLE (ctx->entry));
    src_dir = hx_files_provider_get_current_path (prov);
    if (!dest_dir || !*dest_dir) {
        return;
    }

    /* Same-dir → defer to the regular Rename path; if there's no
	 * rename intent the user could just have done nothing. We
	 * proceed anyway: hx_files_provider_rename treats it as a
	 * no-op-ish call. */
    for (i = 0; i < ctx->names->len; i++) {
        const char *src_name = g_ptr_array_index (ctx->names, i);
        char *new_path = g_build_filename (dest_dir, src_name, NULL);
        GError *err = NULL;
        gboolean ok;

        /* The provider's rename takes leaf names within the
		 * current dir. For cross-dir we pass an absolute path
		 * as new_name — both impls treat new_name starting
		 * with '/' as an absolute path and join correctly.
		 *
		 * Actually checking the impls: local's
		 * hx_local_files_provider_rename calls child_path
		 * which always joins under current_path. Remote's
		 * does the same. So they DON'T support cross-dir
		 * via the existing rename method.
		 *
		 * Workaround: call hx_file_move (remote) /
		 * g_file_move (local) directly with absolute paths.
		 * The provider interface gains a follow-up "move"
		 * method later if cross-dir becomes a routine
		 * operation. For Phase 4 we punch through the
		 * abstraction. */
        char *src_abs
            = g_build_filename (src_dir ? src_dir : "/", src_name, NULL);

        if (HX_IS_LOCAL_FILES_PROVIDER (prov)) {
            GFile *sf = g_file_new_for_path (src_abs);
            GFile *df = g_file_new_for_path (new_path);
            ok = g_file_move (sf, df, G_FILE_COPY_NONE, NULL, NULL, NULL, &err);
            g_object_unref (sf);
            g_object_unref (df);
        } else if (HX_IS_REMOTE_FILES_PROVIDER (prov)) {
            if (!the_session.htlc.fd) {
                ok = FALSE;
                err = g_error_new (G_FILE_ERROR, G_FILE_ERROR_FAILED,
                                   _ ("Not connected to a server."));
            } else if (!hl_access_has ((const guint8 *)&the_session.htlc.access,
                                       HL_ACCESS_MOVE_FILES)) {
                ok = FALSE;
                err = g_error_new (G_FILE_ERROR, G_FILE_ERROR_FAILED,
                                   _ ("You don't have permission to move files "
                                      "on the server."));
            } else {
                hx_file_move (&the_session.htlc, src_abs, new_path);
                ok = TRUE; /* fire-and-forget — server task
				              * error would surface via the
				              * existing task-error toast */
            }
        } else {
            ok = FALSE;
        }

        if (ok) {
            moved++;
        } else {
            failed++;
            if (last_err) {
                g_error_free (last_err);
            }
            last_err = err;
        }

        g_free (src_abs);
        g_free (new_path);
    }

    hx_files_provider_reload (prov);

    if (failed == 0) {
        char *msg;
        if (ctx->is_remote) {
            /* Remote move is fire-and-forget over the wire: server
			 * acks asynchronously, and any rejection (no permission,
			 * destination exists, etc.) flows through the generic
			 * task_error toast on the toolbar window. So our success
			 * line has to read as "request sent" rather than
			 * "definitely done" — otherwise we cheerfully claim
			 * success while the toolbar simultaneously announces
			 * a permission denial. */
            msg = g_strdup_printf (
                g_dngettext (NULL, "Move requested for %u item.",
                             "Move requested for %u items.", moved),
                moved);
        } else {
            msg = g_strdup_printf (
                g_dngettext (NULL, "Moved %u item.", "Moved %u items.", moved),
                moved);
        }
        show_toast (ctx->br, msg);
        g_free (msg);
    } else {
        show_toast (ctx->br, last_err ? last_err->message : _ ("Move failed."));
    }
    if (last_err) {
        g_error_free (last_err);
    }
}

static void
on_move_closed (AdwAlertDialog *dialog, gpointer user_data)
{
    struct move_ctx *ctx = user_data;
    (void)dialog;
    /* Tear the path-completion popover down BEFORE the dialog
	 * destruction drags the entry away — hx_path_complete_free
	 * disconnects the per-entry signal handler and key controller,
	 * and needs the entry to still be valid for that. */
    if (ctx->complete) {
        hx_path_complete_free (ctx->complete);
        ctx->complete = NULL;
    }
    if (ctx->names) {
        g_ptr_array_free (ctx->names, TRUE);
    }
    g_free (ctx);
}

static void
on_move_clicked (GtkButton *btn, gpointer user_data)
{
    struct browser *br = user_data;
    GPtrArray *entries;
    files_panel *dst_panel;
    HxFilesProvider *sp;
    const char *default_dest;
    AdwDialog *dialog;
    GtkWidget *entry;
    struct move_ctx *ctx;
    char *body;
    gboolean src_is_remote;
    guint i;
    (void)btn;

    if (!br->active) {
        return;
    }
    entries = files_panel_get_selected_entries (br->active);
    if (!entries || entries->len == 0) {
        if (entries) {
            g_ptr_array_free (entries, TRUE);
        }
        show_toast (br, _ ("Select files to move first."));
        return;
    }

    sp = files_panel_get_provider (br->active);
    src_is_remote = HX_IS_REMOTE_FILES_PROVIDER (sp);
    dst_panel = (br->active == br->left) ? br->right : br->left;

    /* Cross-side moves are out of scope — Hotline's MOVEFILE only
	 * works within one server, and GIO's g_file_move only within
	 * one filesystem. The user can either drag the items across
	 * (Copy via the existing DnD path) and Delete the source,
	 * or invoke Copy + Delete from the toolbar. Refuse here with
	 * a directed toast so they're not left typing a destination
	 * path that wouldn't actually move anything. */
    if (dst_panel) {
        HxFilesProvider *dp = files_panel_get_provider (dst_panel);
        if (HX_IS_LOCAL_FILES_PROVIDER (sp)
            != HX_IS_LOCAL_FILES_PROVIDER (dp)) {
            g_ptr_array_free (entries, TRUE);
            show_toast (br, _ ("Move only works within one side. Use Copy then "
                               "Delete to move between local and remote."));
            return;
        }
    }

    /* Default destination: the other panel's current path
	 * (guaranteed same-side after the cross-side refusal above). */
    default_dest = NULL;
    if (dst_panel) {
        HxFilesProvider *dp = files_panel_get_provider (dst_panel);
        default_dest = hx_files_provider_get_current_path (dp);
    }
    if (!default_dest) {
        default_dest = hx_files_provider_get_current_path (sp);
    }

    body = (entries->len == 1)
               ? g_strdup_printf (
                     _ ("Move “%s” to:"),
                     hx_file_entry_get_name (g_ptr_array_index (entries, 0)))
               : g_strdup_printf (
                     g_dngettext (NULL, "Move %u item to:", "Move %u items to:",
                                  entries->len),
                     entries->len);
    dialog = ADW_DIALOG (adw_alert_dialog_new (_ ("Move"), body));
    g_free (body);

    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel",
                                   _ ("_Cancel"));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "move",
                                   _ ("_Move"));
    adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "move",
                                              ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "move");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

    entry = gtk_entry_new ();
    gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
    gtk_editable_set_text (GTK_EDITABLE (entry), default_dest);
    adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), entry);

    ctx = g_new0 (struct move_ctx, 1);
    ctx->br = br;
    ctx->panel = br->active;
    ctx->is_remote = src_is_remote;
    ctx->names = g_ptr_array_new_with_free_func (g_free);
    for (i = 0; i < entries->len; i++) {
        HxFileEntry *e = g_ptr_array_index (entries, i);
        g_ptr_array_add (ctx->names, g_strdup (hx_file_entry_get_name (e)));
    }
    ctx->entry = entry;
    g_ptr_array_free (entries, TRUE);

    /* Attach the same smart-case path-completion popover the
	 * local panel's path entry uses. Local-source only — the
	 * completion needs synchronous directory enumeration, which
	 * isn't viable against a Hotline server (each typed char
	 * would trigger an RPC round-trip). Stored on ctx so
	 * on_move_closed can free it before the entry is destroyed
	 * with the dialog. */
    if (!ctx->is_remote) {
        ctx->complete = hx_path_complete_attach (GTK_ENTRY (entry));
    }

    g_signal_connect (dialog, "response", G_CALLBACK (on_move_response), ctx);
    g_signal_connect (dialog, "closed", G_CALLBACK (on_move_closed), ctx);

    adw_dialog_present (dialog, br->window);
    gtk_widget_grab_focus (entry);
    gtk_editable_select_region (GTK_EDITABLE (entry), 0, -1);
}

/* ---- Activate selected entry (F4 / Enter on a row) ---- */

/* Window-level shortcut that mirrors what double-click /
 * Enter-on-row already does, but driven from a keypress that
 * doesn't require the column view to have row focus. Routes
 * the active panel's single selection through the provider's
 * activate_entry (preview for remote, xdg-open for local). */
static gboolean
on_open_shortcut (GtkWidget *widget, GVariant *args, gpointer user_data)
{
    struct browser *br = user_data;
    HxFileEntry *e;
    (void)widget;
    (void)args;

    if (!br->active) {
        return FALSE;
    }
    e = files_panel_get_single_selected (br->active);
    if (!e || hx_file_entry_is_dir (e)) {
        return FALSE;
    }
    hx_files_provider_activate_entry (files_panel_get_provider (br->active), e);
    return TRUE;
}

/* F2 rename shortcut — route to the same dialog the headerbar
 * Rename button uses. Driven from a callback action so the
 * column view's internal F2 (which would start an in-place
 * editor on the row, behaviour we don't want for now) doesn't
 * preempt us. */
static gboolean
on_rename_shortcut (GtkWidget *widget, GVariant *args, gpointer user_data)
{
    (void)widget;
    (void)args;
    on_rename_clicked (NULL, user_data);
    return TRUE;
}

/* F6 move shortcut — Norton/orthodox-FM convention for
 * "move to other panel's directory". Mirrors the headerbar
 * Move button. */
static gboolean
on_move_shortcut (GtkWidget *widget, GVariant *args, gpointer user_data)
{
    (void)widget;
    (void)args;
    on_move_clicked (NULL, user_data);
    return TRUE;
}

/* Ctrl+I get-info shortcut — mirrors the headerbar Get Info button.
 * The classic Mac shortcut was Cmd+I; under Linux the conventional
 * equivalent is Ctrl+I. Remote-only; the handler toasts a hint when
 * the active panel is local. */
static gboolean
on_get_info_shortcut (GtkWidget *widget, GVariant *args, gpointer user_data)
{
    (void)widget;
    (void)args;
    on_get_info_clicked (NULL, user_data);
    return TRUE;
}

/* Copy a GPtrArray of entries from one panel to another and
 * surface a one-shot summary toast. Shared between the Copy
 * headerbar button and the DnD drop handler — both ultimately
 * iterate hx_files_ops_copy and aggregate per-entry results. */
static void
copy_entries_and_toast (struct browser *br, files_panel *src, files_panel *dst,
                        GPtrArray *entries)
{
    guint i, queued = 0, failed = 0;
    HxOpsResult last_err = HX_OPS_OK;

    if (!entries || entries->len == 0) {
        show_toast (br, _ ("Select a file to copy first."));
        return;
    }

    for (i = 0; i < entries->len; i++) {
        HxFileEntry *e = g_ptr_array_index (entries, i);
        HxOpsResult r = hx_files_ops_copy (files_panel_get_provider (src),
                                           files_panel_get_provider (dst), e);
        if (r == HX_OPS_OK) {
            queued++;
        } else {
            failed++;
            last_err = r;
        }
    }

    /* Lead with the failure reason when anything failed since
	 * that's the actionable bit. last_err alone is enough for
	 * the common case where every failure had the same global
	 * cause (no permission, not connected, folder unsupported). */
    if (failed == 0) {
        char *msg = g_strdup_printf (
            g_dngettext (NULL, "Transfer queued (%u item).",
                         "Transfers queued (%u items).", queued),
            queued);
        show_toast (br, msg);
        g_free (msg);
    } else if (queued == 0) {
        show_toast (br, hx_files_ops_result_message (last_err));
    } else {
        char *msg
            = g_strdup_printf (_ ("%u queued, %u failed (%s)."), queued, failed,
                               hx_files_ops_result_message (last_err));
        show_toast (br, msg);
        g_free (msg);
    }
}

/* Issue a batch of remote→remote moves from one panel to another
 * and surface a one-shot summary toast. Used by the DnD drop
 * handler — orthodox-FM convention is that drag-within-same-
 * volume is a MOVE, not a copy, and Hotline's only real intra-
 * server relocation primitive is HTLC_HDR_FILE_MOVE.
 *
 * Both panels must be remote and the connection must be live.
 * Access bit HL_ACCESS_MOVE_FILES is checked once before any
 * requests fire so a permission failure reads as a single toast
 * rather than one per file.
 *
 * The wire send is fire-and-forget (matches the Move dialog's
 * remote path): server-side rejections surface via the existing
 * task_error toast on the toolbar window. Our toast reads "Move
 * requested" rather than "Moved" so the optimism is honest. */
static void
move_entries_and_toast (struct browser *br, files_panel *src, files_panel *dst,
                        GPtrArray *entries)
{
    const char *src_dir, *dst_dir;
    HxFilesProvider *sp, *dp;
    guint i;
    char *msg;

    if (!entries || entries->len == 0) {
        return;
    }

    if (!the_session.htlc.fd) {
        show_toast (br, _ ("Not connected to a server."));
        return;
    }
    if (!hl_access_has ((const guint8 *)&the_session.htlc.access,
                        HL_ACCESS_MOVE_FILES)) {
        show_toast (br, _ ("You don't have permission to move files on the "
                           "server."));
        return;
    }

    sp = files_panel_get_provider (src);
    dp = files_panel_get_provider (dst);
    src_dir = hx_files_provider_get_current_path (sp);
    dst_dir = hx_files_provider_get_current_path (dp);

    for (i = 0; i < entries->len; i++) {
        HxFileEntry *e = g_ptr_array_index (entries, i);
        const char *name = hx_file_entry_get_name (e);
        char *src_abs = g_build_filename (src_dir ? src_dir : "/", name, NULL);
        char *dst_abs = g_build_filename (dst_dir ? dst_dir : "/", name, NULL);
        hx_file_move (&the_session.htlc, src_abs, dst_abs);
        g_free (src_abs);
        g_free (dst_abs);
    }

    /* Reload both panels so the file appears in the destination
	 * and disappears from the source once the server's task acks
	 * come back. */
    hx_files_provider_reload (sp);
    hx_files_provider_reload (dp);

    msg = g_strdup_printf (g_dngettext (NULL, "Move requested for %u item.",
                                        "Move requested for %u items.",
                                        entries->len),
                           entries->len);
    show_toast (br, msg);
    g_free (msg);
}

/* Copy headerbar button — pulls the active panel's selection and
 * issues a batch copy to the inactive panel. */
static void
on_copy_clicked (GtkButton *btn, gpointer user_data)
{
    struct browser *br = user_data;
    files_panel *src, *dst;
    GPtrArray *entries;
    (void)btn;

    if (!br->active) {
        return;
    }
    src = br->active;
    dst = (src == br->left) ? br->right : br->left;
    if (!dst) {
        return;
    }

    entries = files_panel_get_selected_entries (src);
    copy_entries_and_toast (br, src, dst, entries);
    if (entries) {
        g_ptr_array_free (entries, TRUE);
    }
}

/* ---- Drag and drop between panels ---------------------------- */

/* Drag-source prepare: pull the current selection from the
 * source panel, pack into a HxFilesDrag, return a content
 * provider that wraps the boxed value. NULL means "don't
 * start a drag" (empty selection) — GtkDragSource handles that
 * cleanly.
 *
 * For LOCAL sources we additionally publish a GDK_TYPE_FILE_LIST
 * provider in the same drag. That lets external apps (GNOME
 * Files, Finder-via-Wayland, etc.) accept the drop as a real
 * GFile transfer. For REMOTE sources we don't — the file
 * doesn't exist on the host yet, and the FileTransferPortal
 * protocol that would let us promise a download isn't wired
 * up. */
static GdkContentProvider *
on_drag_prepare (GtkDragSource *source, double x, double y, gpointer user_data)
{
    files_panel *p;
    GPtrArray *entries;
    HxFilesDrag drag;
    GValue val = G_VALUE_INIT;
    GdkContentProvider *cp_internal;
    GdkContentProvider *cp_files = NULL;
    (void)x;
    (void)y;
    (void)user_data;

    p = g_object_get_data (G_OBJECT (source), "panel");
    if (!p) {
        return NULL;
    }

    entries = files_panel_get_selected_entries (p);
    if (!entries || entries->len == 0) {
        if (entries) {
            g_ptr_array_free (entries, TRUE);
        }
        return NULL;
    }

    /* gdk_content_provider_new_for_value deep-copies the boxed
	 * payload via hx_files_drag_copy, so freeing our locals
	 * afterwards is safe. */
    drag.src_panel = p;
    drag.entries = entries;

    g_value_init (&val, HX_TYPE_FILES_DRAG);
    g_value_set_boxed (&val, &drag);
    cp_internal = gdk_content_provider_new_for_value (&val);
    g_value_unset (&val);

    /* External-drag enrichment.
	 *
	 *   LOCAL panel  → offer GDK_TYPE_FILE_LIST pointing at the
	 *                  real on-disk files. External apps drop
	 *                  this as a normal GFile copy. Folders
	 *                  ride in too; GIO handles recursion on
	 *                  the receiver side.
	 *
	 *   REMOTE panel → kick off an xfer_new download to
	 *                  ~/Downloads (or whatever download_path
	 *                  is set to) for each selected file, and
	 *                  publish a text/uri-list pointing at the
	 *                  eventual paths. The receiver app gets
	 *                  URIs that may not have full data yet —
	 *                  for small files on fast links the copy
	 *                  completes before the receiver reads;
	 *                  for large files the user will see the
	 *                  file appear in Downloads via the tasks
	 *                  window regardless. Not a "true" promised
	 *                  drag (no FileTransferPortal plumbing)
	 *                  but a workable approximation. Folders
	 *                  on the remote side are skipped — Hotline
	 *                  folder downloads need a recursive
	 *                  xfer path. Toast tells the user what
	 *                  happened so the drag completing without
	 *                  the receiver getting bytes isn't a
	 *                  mystery. */
    {
        HxFilesProvider *prov = files_panel_get_provider (p);
        /* "Other panel is local" gate for the remote-source path —
	 * see the comment on the remote branch below. */
        files_panel *other_panel = NULL;
        gboolean other_is_local = FALSE;
        if (the_browser) {
            other_panel = (p == the_browser->left) ? the_browser->right
                                                   : the_browser->left;
            if (other_panel) {
                other_is_local = HX_IS_LOCAL_FILES_PROVIDER (
                    files_panel_get_provider (other_panel));
            }
        }

        if (HX_IS_LOCAL_FILES_PROVIDER (prov)) {
            const char *dir = hx_files_provider_get_current_path (prov);
            GSList *flist = NULL;
            guint i;
            for (i = 0; i < entries->len; i++) {
                HxFileEntry *e = g_ptr_array_index (entries, i);
                char *abspath = g_build_filename (
                    dir ? dir : "/", hx_file_entry_get_name (e), NULL);
                flist = g_slist_prepend (flist, g_file_new_for_path (abspath));
                g_free (abspath);
            }
            flist = g_slist_reverse (flist);

            g_value_init (&val, GDK_TYPE_FILE_LIST);
            g_value_set_boxed (&val, flist);
            cp_files = gdk_content_provider_new_for_value (&val);
            g_value_unset (&val);
            g_slist_free_full (flist, g_object_unref);
        } else if (HX_IS_REMOTE_FILES_PROVIDER (prov) && the_session.htlc.fd
                   && hl_access_has ((const guint8 *)&the_session.htlc.access,
                                     HL_ACCESS_DOWNLOAD_FILES)
                   && other_is_local) {
            /* Eager-download path: source is remote and the user
	     * might drop on our local panel OR on an external app
	     * on the host filesystem. We start the download to
	     * ~/Downloads at prepare time so by the drop the bytes
	     * are already moving, and advertise a text/uri-list
	     * pointing at the eventual local paths.
	     *
	     * Skipped when the OTHER panel is remote (the
	     * side-selector configuration with both sides remote):
	     * the user almost certainly intends a remote→remote op,
	     * which routes through the internal HX_TYPE_FILES_DRAG
	     * provider in cp_internal and is handled in on_drop.
	     * Firing xfer_new unconditionally there would yank
	     * files to ~/Downloads that the user didn't ask for. */
            const char *rdir = hx_files_provider_get_current_path (prov);
            const char *ldir = gtkhx_prefs.download_path
                                   ? gtkhx_prefs.download_path
                                   : g_get_home_dir ();
            GString *uri_list = g_string_new (NULL);
            guint i, kicked = 0;
            for (i = 0; i < entries->len; i++) {
                HxFileEntry *e = g_ptr_array_index (entries, i);
                char *rpath, *lpath, *uri;
                GFile *lf;
                struct htxf_conn *htxf;

                if (hx_file_entry_is_dir (e)) {
                    continue; /* recursive
				                                         * remote
				                                         * dl not
				                                         * wired */
                }
                rpath = g_build_filename (rdir ? rdir : "/",
                                          hx_file_entry_get_name (e), NULL);
                lpath
                    = g_build_filename (ldir, hx_file_entry_get_name (e), NULL);

                /* xfer_new takes the remote location as a
				 * (dir, name, name_len) triple. The name's bytes
				 * may include '/' (legal in Hotline names — the
				 * wire format is per-component, not slash-
				 * separated); pulling them out of the joined
				 * rpath is what survives the round-trip. */
                {
                    const char *nm = hx_file_entry_get_name (e);
                    gsize nm_len = nm ? strlen (nm) : 0;
                    htxf = xfer_new (lpath, rdir ? rdir : "/", nm, nm_len,
                                     XFER_GET, 0,
                                     (guint32)hx_file_entry_get_size (e));
                }
                if (htxf) {
                    htxf->filter_argv = 0;
                    htxf->opt.retry = 0;
                    kicked++;
                }

                /* Build the uri-list entry in the same loop —
				 * the URI points at where the file WILL be after
				 * the download lands. RFC 2483: each entry on
				 * its own line, CRLF-terminated. */
                lf = g_file_new_for_path (lpath);
                uri = g_file_get_uri (lf);
                if (uri) {
                    g_string_append (uri_list, uri);
                    g_string_append (uri_list, "\r\n");
                    g_free (uri);
                }
                g_object_unref (lf);

                g_free (rpath);
                g_free (lpath);
            }

            if (uri_list->len > 0) {
                /* Steal the buffer with the modern
				 * g_string_free_and_steal API; the
				 * deprecation-suppressing g_string_free
				 * (..., FALSE) form trips
				 * -Wdeprecated-declarations on recent
				 * GLib. Returned string is g_malloc'd
				 * and g_bytes_new_take takes ownership. */
                gsize len = uri_list->len;
                char *buf = g_string_free_and_steal (uri_list);
                GBytes *b = g_bytes_new_take (buf, len);
                cp_files
                    = gdk_content_provider_new_for_bytes ("text/uri-list", b);
                g_bytes_unref (b);
            } else {
                g_string_free (uri_list, TRUE);
            }

            /* Always toast — the user needs to know the drag is
			 * really "start download to <path>", not the usual
			 * "transfer bytes via DnD". */
            {
                struct browser *br = the_browser;
                if (br && kicked > 0) {
                    char *msg = g_strdup_printf (
                        g_dngettext (NULL, "Downloading %u file to %s",
                                     "Downloading %u files to %s", kicked),
                        kicked, ldir);
                    show_toast (br, msg);
                    g_free (msg);
                }
            }
        }
    }

    g_ptr_array_free (entries, TRUE);

    if (cp_files) {
        /* Union: external apps receive GDK_TYPE_FILE_LIST (or
		 * text/uri-list), our internal drop target receives
		 * HX_TYPE_FILES_DRAG. GDK negotiates whichever the
		 * target accepts.
		 *
		 * gdk_content_provider_new_union takes ownership of
		 * every provider in the array (transfer-full per the
		 * GIR annotation). DON'T unref cp_internal / cp_files
		 * after the call — those refs now belong to the union,
		 * and dropping them double-frees. The crash signature
		 * is a SIGSEGV inside gdk_content_provider_ref_formats
		 * later in the drag lifecycle when GDK queries the
		 * union's now-dangling inner providers. */
        GdkContentProvider *providers[2] = { cp_internal, cp_files };
        return gdk_content_provider_new_union (providers, 2);
    }
    return cp_internal;
}

/* Drop target callback. Validates the drag's source-panel
 * pointer still resolves to one of our panels (defensive — the
 * drag could in theory outlive the source widget, though in
 * practice both panels are siblings of the drop target inside
 * the same window) and that it's a CROSS-panel drop. Same-panel
 * drops short-circuit without firing any transfers — orthodox
 * FM convention is that intra-panel DnD is a no-op (no "move
 * within directory" semantic). */
static gboolean
on_drop (GtkDropTarget *target, const GValue *value, double x, double y,
         gpointer user_data)
{
    struct browser *br;
    files_panel *dst;
    HxFilesDrag *drag;
    (void)x;
    (void)y;
    (void)user_data;

    br = g_object_get_data (G_OBJECT (target), "browser");
    dst = g_object_get_data (G_OBJECT (target), "panel");
    if (!br || !dst) {
        return FALSE;
    }
    if (!G_VALUE_HOLDS (value, HX_TYPE_FILES_DRAG)) {
        return FALSE;
    }
    drag = g_value_get_boxed (value);
    if (!drag || !drag->src_panel || !drag->entries) {
        return FALSE;
    }

    /* Drop on the same panel — no-op. GTK still considers the
	 * drop "accepted" so we return TRUE; otherwise the drag
	 * animates back to the source with a rejection sting. */
    if (drag->src_panel == dst) {
        return TRUE;
    }

    /* Source panel must be one of ours (paranoia — if the drag
	 * came from somewhere else with a matching type, refuse). */
    if (drag->src_panel != br->left && drag->src_panel != br->right) {
        return FALSE;
    }

    /* Orthodox-FM convention: drag-within-same-volume is a MOVE,
	 * not a copy. Remote→remote on the same server is the case
	 * the user hits when both panels are set to Remote via the
	 * side selector. Route through hx_file_move so the file
	 * relocates rather than getting symlinked or download-then-
	 * re-uploaded.
	 *
	 * Cross-side (local→remote, remote→local) and local→local
	 * keep their existing Copy semantics; cross-side because the
	 * filesystems are distinct, local→local because the user has
	 * not yet asked for a behaviour change there. */
    {
        HxFilesProvider *sp = files_panel_get_provider (drag->src_panel);
        HxFilesProvider *dp = files_panel_get_provider (dst);
        if (HX_IS_REMOTE_FILES_PROVIDER (sp)
            && HX_IS_REMOTE_FILES_PROVIDER (dp)) {
            move_entries_and_toast (br, drag->src_panel, dst, drag->entries);
            return TRUE;
        }
    }

    copy_entries_and_toast (br, drag->src_panel, dst, drag->entries);
    return TRUE;
}

static void
attach_panel_dnd (struct browser *br, files_panel *p)
{
    GtkWidget *view = files_panel_get_column_view (p);
    GtkDragSource *src;
    GtkDropTarget *drop;

    if (!view) {
        return;
    }

    /* Source: drags initiated by clicking a row and pulling
	 * past GTK's movement threshold. Action is COPY only (no
	 * MOVE/LINK in Phase 4 — Move comes later, and Link doesn't
	 * map cleanly onto either side). */
    src = gtk_drag_source_new ();
    gtk_drag_source_set_actions (src, GDK_ACTION_COPY);
    g_object_set_data (G_OBJECT (src), "panel", p);
    g_signal_connect (src, "prepare", G_CALLBACK (on_drag_prepare), NULL);
    gtk_widget_add_controller (view, GTK_EVENT_CONTROLLER (src));

    /* Target: accepts our boxed type only. GtkDropTarget adds
	 * a .drop-active CSS class to the widget while a compatible
	 * drag hovers, which gives a visual cue for free (Adwaita's
	 * default style for it is fine). */
    drop = gtk_drop_target_new (HX_TYPE_FILES_DRAG, GDK_ACTION_COPY);
    g_object_set_data (G_OBJECT (drop), "browser", br);
    g_object_set_data (G_OBJECT (drop), "panel", p);
    g_signal_connect (drop, "drop", G_CALLBACK (on_drop), NULL);
    gtk_widget_add_controller (view, GTK_EVENT_CONTROLLER (drop));
}

struct mkdir_ctx {
    struct browser *br;
    files_panel *panel;
    GtkWidget *entry;
};

static void
on_mkdir_response (AdwAlertDialog *dialog, const char *response,
                   gpointer user_data)
{
    struct mkdir_ctx *ctx = user_data;
    const char *name;
    GError *err = NULL;
    (void)dialog;

    if (g_strcmp0 (response, "create") != 0) {
        return;
    }
    if (!ctx->panel) {
        return;
    }
    name = gtk_editable_get_text (GTK_EDITABLE (ctx->entry));
    if (!name || !*name) {
        return;
    }

    if (!hx_files_provider_mkdir (files_panel_get_provider (ctx->panel), name,
                                  &err)) {
        g_warning ("mkdir failed: %s", err ? err->message : "unknown");
        g_clear_error (&err);
    }
}

static void
on_mkdir_closed (AdwAlertDialog *dialog, gpointer user_data)
{
    (void)dialog;
    g_free (user_data);
}

static void
on_mkdir_clicked (GtkButton *btn, gpointer user_data)
{
    struct browser *br = user_data;
    AdwDialog *dialog;
    GtkWidget *entry;
    struct mkdir_ctx *ctx;
    (void)btn;

    if (!br->active) {
        return;
    }

    dialog = ADW_DIALOG (adw_alert_dialog_new (
        _ ("New Folder"), _ ("Enter a name for the new folder.")));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel",
                                   _ ("_Cancel"));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "create",
                                   _ ("C_reate"));
    adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog),
                                              "create", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "create");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

    entry = gtk_entry_new ();
    gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
    adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), entry);

    ctx = g_new0 (struct mkdir_ctx, 1);
    ctx->br = br;
    ctx->panel = br->active;
    ctx->entry = entry;

    g_signal_connect (dialog, "response", G_CALLBACK (on_mkdir_response), ctx);
    g_signal_connect (dialog, "closed", G_CALLBACK (on_mkdir_closed), ctx);

    adw_dialog_present (dialog, br->window);
    /* Focus the entry so the user can type immediately + hit
	 * Enter. activates-default = TRUE on the entry routes that
	 * Enter to AdwAlertDialog's default response ("create").
	 * Has to happen AFTER adw_dialog_present — the dialog isn't
	 * realized before that and grab_focus is a no-op on an
	 * unmapped widget. */
    gtk_widget_grab_focus (entry);
}

struct delete_ctx {
    struct browser *br;
    files_panel *panel;
    GPtrArray *names; /* owned — array of g_strdup'd names */
};

static void
on_delete_response (AdwAlertDialog *dialog, const char *response,
                    gpointer user_data)
{
    struct delete_ctx *ctx = user_data;
    HxFilesProvider *prov;
    guint i;
    (void)dialog;

    if (g_strcmp0 (response, "delete") != 0) {
        return;
    }
    if (!ctx->panel || !ctx->names) {
        return;
    }
    prov = files_panel_get_provider (ctx->panel);

    for (i = 0; i < ctx->names->len; i++) {
        const char *name = g_ptr_array_index (ctx->names, i);
        GError *err = NULL;
        if (!hx_files_provider_delete (prov, name, &err)) {
            g_warning ("delete %s: %s", name, err ? err->message : "unknown");
            g_clear_error (&err);
        }
    }
}

static void
on_delete_closed (AdwAlertDialog *dialog, gpointer user_data)
{
    struct delete_ctx *ctx = user_data;
    (void)dialog;
    if (ctx->names) {
        g_ptr_array_free (ctx->names, TRUE);
    }
    g_free (ctx);
}

/* Build the delete-confirmation body text. Singular for one
 * entry (with the actual name so the user can sanity-check),
 * plural with a count for multi-select since fitting N names
 * into one toast line gets unwieldy. */
static char *
delete_body_text (GPtrArray *entries)
{
    HxFileEntry *e;
    if (!entries || entries->len == 0) {
        return g_strdup ("");
    }
    if (entries->len == 1) {
        e = g_ptr_array_index (entries, 0);
        return g_strdup_printf (_ ("Delete “%s”? This cannot be undone."),
                                hx_file_entry_get_name (e));
    }
    return g_strdup_printf (
        g_dngettext (NULL, "Delete %u item? This cannot be undone.",
                     "Delete %u items? This cannot be undone.", entries->len),
        entries->len);
}

static void
on_delete_clicked (GtkButton *btn, gpointer user_data)
{
    struct browser *br = user_data;
    GPtrArray *entries;
    AdwDialog *dialog;
    struct delete_ctx *ctx;
    char *body;
    guint i;
    (void)btn;

    if (!br->active) {
        return;
    }
    entries = files_panel_get_selected_entries (br->active);
    if (!entries || entries->len == 0) {
        if (entries) {
            g_ptr_array_free (entries, TRUE);
        }
        return;
    }

    body = delete_body_text (entries);
    dialog = ADW_DIALOG (adw_alert_dialog_new (_ ("Delete"), body));
    g_free (body);

    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel",
                                   _ ("_Cancel"));
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "delete",
                                   _ ("_Delete"));
    adw_alert_dialog_set_response_appearance (
        ADW_ALERT_DIALOG (dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "cancel");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

    /* Snapshot just the names — the dialog runs async and the
	 * selection set could shift in between. Same defensive
	 * pattern the news_browser delete uses. */
    ctx = g_new0 (struct delete_ctx, 1);
    ctx->br = br;
    ctx->panel = br->active;
    ctx->names = g_ptr_array_new_with_free_func (g_free);
    for (i = 0; i < entries->len; i++) {
        HxFileEntry *e = g_ptr_array_index (entries, i);
        g_ptr_array_add (ctx->names, g_strdup (hx_file_entry_get_name (e)));
    }
    g_ptr_array_free (entries, TRUE);

    g_signal_connect (dialog, "response", G_CALLBACK (on_delete_response), ctx);
    g_signal_connect (dialog, "closed", G_CALLBACK (on_delete_closed), ctx);

    adw_dialog_present (dialog, br->window);
}

/* ---- Keyboard shortcut: Tab switches active panel ---- */

/* The window-level shortcut controller fires at GTK_PHASE_CAPTURE,
 * so we see Backspace / Tab BEFORE the focused widget does. That's
 * desirable for keystrokes targeting the column view (where Tab
 * would otherwise be consumed by the focus chain), but it breaks
 * editing in the path entry — Backspace turns into "navigate up"
 * instead of deleting a character, Tab steals focus to the other
 * panel mid-edit.
 *
 * Compromise: if focus is on any GtkEditable (entry, search-entry,
 * spin button), don't fire — let the keystroke through. The
 * column view isn't editable in that sense; the panels themselves
 * are not editable widgets. */
static gboolean
focus_is_editable (struct browser *br)
{
    GtkRoot *root;
    GtkWidget *focused;

    if (!br || !br->window) {
        return FALSE;
    }
    root = gtk_widget_get_root (br->window);
    if (!GTK_IS_WINDOW (root)) {
        return FALSE;
    }
    focused = gtk_window_get_focus (GTK_WINDOW (root));
    if (!focused) {
        return FALSE;
    }
    /* GtkEntry delegates editing to an internal GtkText, so when
	 * the path entry has focus gtk_window_get_focus returns the
	 * GtkText, not the GtkEntry. Both implement GtkEditable, so
	 * GTK_IS_EDITABLE covers both cases. */
    return GTK_IS_EDITABLE (focused);
}

static gboolean
on_tab_shortcut (GtkWidget *widget, GVariant *args, gpointer user_data)
{
    struct browser *br = user_data;
    files_panel *other;
    (void)widget;
    (void)args;

    if (focus_is_editable (br)) {
        return FALSE;
    }

    if (!br->active) {
        return FALSE;
    }
    other = (br->active == br->left) ? br->right : br->left;
    if (other) {
        gtk_widget_grab_focus (files_panel_get_column_view (other));
    }
    return TRUE;
}

static gboolean
on_backspace_shortcut (GtkWidget *widget, GVariant *args, gpointer user_data)
{
    struct browser *br = user_data;
    (void)widget;
    (void)args;

    if (focus_is_editable (br)) {
        return FALSE;
    }

    if (br->active) {
        hx_files_provider_navigate_up (files_panel_get_provider (br->active));
    }
    return TRUE;
}

/* ---- CSS for the active-panel highlight ---- */

/* Active-panel marker via inset box-shadow rather than a real
 * 2px border. The border version reflowed the frame's inner
 * scrolled-window-and-column-view by 4px when the active class
 * was toggled, which is enough of a layout invalidation to
 * make GtkColumnView throw away in-progress click sequences.
 * box-shadow paints over existing pixels without taking
 * layout space, so the column view is the same size before
 * and after the active flip.
 *
 * Hardcoded hex rather than @accent_color so the rule resolves
 * unambiguously across libadwaita color-scheme + accent
 * settings — gtkurl.c does the same thing for its URL tag.
 * #1c71d8 is libadwaita's default light-theme accent. */
static const char *active_css = ".files-panel-active {\n"
                                "  box-shadow: inset 0 0 0 2px #1c71d8;\n"
                                "  border-radius: 8px;\n"
                                "}\n";

static void
install_css (struct browser *br)
{
    GdkDisplay *display;

    br->css = gtk_css_provider_new ();
    gtk_css_provider_load_from_string (br->css, active_css);
    display = gdk_display_get_default ();
    if (display) {
        gtk_style_context_add_provider_for_display (
            display, GTK_STYLE_PROVIDER (br->css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
}

/* ---- Lifecycle ---- */

static gboolean
on_close (GtkWindow *window, gpointer user_data)
{
    struct browser *br = user_data;
    GdkDisplay *display;
    (void)window;

    if (the_browser == br) {
        the_browser = NULL;
    }

    if (br->conn_state_handler) {
        g_signal_handler_disconnect (gtkhx_session_get_default (),
                                     br->conn_state_handler);
        br->conn_state_handler = 0;
    }
    if (br->file_update_handler) {
        g_signal_handler_disconnect (gtkhx_session_get_default (),
                                     br->file_update_handler);
        br->file_update_handler = 0;
    }

    if (br->css) {
        display = gdk_display_get_default ();
        if (display) {
            gtk_style_context_remove_provider_for_display (
                display, GTK_STYLE_PROVIDER (br->css));
        }
        g_clear_object (&br->css);
    }

    files_panel_free (br->left);
    files_panel_free (br->right);
    g_clear_object (&br->left_provider);
    g_clear_object (&br->right_provider);
    g_free (br);
    return FALSE;
}

/* GtkhxSession fires this when the connection state pivots.
 * State is a GtkhxConnectionState — we don't differentiate
 * here; any state change might flip get_unavailable_reason()
 * from / to NULL, so we just nudge whichever providers are
 * currently bound to either side. The side selector lets the
 * user park remote on the left, or have BOTH sides be remote;
 * the local-provider case is a no-op since its
 * get_unavailable_reason always returns NULL. */
static void
on_connection_state (GtkhxSession *sess, guint state, gpointer user_data)
{
    struct browser *br = user_data;
    (void)sess;
    (void)state;
    if (br->left_provider) {
        g_signal_emit_by_name (br->left_provider, "unavailable-changed");
    }
    if (br->right_provider) {
        g_signal_emit_by_name (br->right_provider, "unavailable-changed");
    }
}

/* file-update fires repeatedly during a transfer (progress
 * tick + final "done" tick). Detect just-finished the same
 * way gtkhx.c does (total_pos catches up to total_size); the
 * xfer worker sets these explicitly at end-of-stream and the
 * state is reached exactly once per htxf. Each finish reloads
 * both panels so the new file appears on the destination side
 * — cheaper than tracking which panel was the dest, and the
 * source side's row icons / sizes might have shifted too
 * (e.g. uploads that triggered a remote rename-on-conflict). */
static void
on_file_update (GtkhxSession *sess, gpointer sess_p, gpointer htxf_p,
                gpointer user_data)
{
    struct browser *br = user_data;
    struct htxf_conn *x = htxf_p;
    (void)sess;
    (void)sess_p;

    if (!x) {
        return;
    }
    if (x->total_size == 0 || x->total_pos < x->total_size) {
        return;
    }

    if (br->left_provider) {
        hx_files_provider_reload (br->left_provider);
    }
    if (br->right_provider) {
        hx_files_provider_reload (br->right_provider);
    }
}

void
open_files_browser (void)
{
    struct browser *br;
    GtkWidget *header, *paned, *refresh_btn, *mkdir_btn, *move_btn,
        *preview_btn, *info_btn, *rename_btn, *delete_btn;
    GtkEventController *shortcuts;
    GtkShortcut *sh;

    if (the_browser) {
        gtk_window_present (GTK_WINDOW (the_browser->window));
        return;
    }

    br = g_new0 (struct browser, 1);

    br->window = gtk_window_new ();
    /* Title goes through changetitlespecific so the connected
	 * server's name lands in parentheses ("Files (Badmoon)"),
	 * matching the rest of the windows. */
    changetitlespecific (br->window, _ ("Files"));
    gtk_widget_set_size_request (br->window, 980, 560);

    install_css (br);

    /* Headerbar:
	 *   pack_start: Refresh, New Folder, Copy, Move, Preview, Get Info
	 *   pack_end:   Delete, Rename
	 *
	 * Icons come from the in-tree pixmap set via gtkhx_pixmap_button,
	 * mirroring the classic Hotline look the toolbar and users
	 * windows already use. Scaling at 2x matches the toolbar.
	 *
	 * Copy moves the active panel's selection to the inactive
	 * panel's current path. The arrow direction in the tooltip
	 * gets re-set on active-panel changes via sync_copy_tooltip.
	 *
	 * Where the classic icon set doesn't have an obvious match
	 * (Move, Rename), we reuse what's close (mkdir for Move,
	 * edituser as a generic edit icon for Rename) rather than
	 * mixing symbolic icons in with the period look. */
    header = adw_header_bar_new ();
#define FB_BTN(resource) gtkhx_pixmap_button ((resource), NULL, 2, NULL, NULL)
    refresh_btn = FB_BTN ("/com/nasledov/gtkhx/pixmaps/refresh.xpm");
    mkdir_btn = FB_BTN ("/com/nasledov/gtkhx/pixmaps/mkdir.xpm");
    br->btn_copy = FB_BTN ("/com/nasledov/gtkhx/pixmaps/dl.xpm");
    move_btn = FB_BTN ("/com/nasledov/gtkhx/pixmaps/move.xpm");
    preview_btn = FB_BTN ("/com/nasledov/gtkhx/pixmaps/preview.xpm");
    info_btn = FB_BTN ("/com/nasledov/gtkhx/pixmaps/info.xpm");
    rename_btn = FB_BTN ("/com/nasledov/gtkhx/pixmaps/edituser.xpm");
    delete_btn = FB_BTN ("/com/nasledov/gtkhx/pixmaps/trash.xpm");
#undef FB_BTN

    gtk_widget_set_tooltip_text (refresh_btn,
                                 _ ("Reload active panel (Ctrl+R)"));
    gtk_widget_set_tooltip_text (mkdir_btn,
                                 _ ("New folder in active panel (Ctrl+N)"));
    gtk_widget_set_tooltip_text (br->btn_copy,
                                 _ ("Copy selection to the other panel (F5)"));
    gtk_widget_set_tooltip_text (
        move_btn, _ ("Move selection to another directory (F6)"));
    gtk_widget_set_tooltip_text (preview_btn, _ ("Preview selected file (F4)"));
    gtk_widget_set_tooltip_text (info_btn,
                                 _ ("Get Info for selected file (Ctrl+I)"));
    gtk_widget_set_tooltip_text (rename_btn, _ ("Rename selected file (F2)"));
    gtk_widget_set_tooltip_text (
        delete_btn, _ ("Delete selection in active panel (Ctrl+D)"));

    g_signal_connect (refresh_btn, "clicked", G_CALLBACK (on_refresh_clicked),
                      br);
    g_signal_connect (mkdir_btn, "clicked", G_CALLBACK (on_mkdir_clicked), br);
    g_signal_connect (br->btn_copy, "clicked", G_CALLBACK (on_copy_clicked),
                      br);
    g_signal_connect (move_btn, "clicked", G_CALLBACK (on_move_clicked), br);
    g_signal_connect (preview_btn, "clicked", G_CALLBACK (on_preview_clicked),
                      br);
    g_signal_connect (info_btn, "clicked", G_CALLBACK (on_get_info_clicked),
                      br);
    g_signal_connect (rename_btn, "clicked", G_CALLBACK (on_rename_clicked),
                      br);
    g_signal_connect (delete_btn, "clicked", G_CALLBACK (on_delete_clicked),
                      br);

    adw_header_bar_pack_start (ADW_HEADER_BAR (header), refresh_btn);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), mkdir_btn);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), br->btn_copy);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), move_btn);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), preview_btn);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), info_btn);
    adw_header_bar_pack_end (ADW_HEADER_BAR (header), delete_btn);
    adw_header_bar_pack_end (ADW_HEADER_BAR (header), rename_btn);
    gtk_window_set_titlebar (GTK_WINDOW (br->window), header);

    /* Two panels in a horizontal GtkPaned. Phase 1: both local;
	 * Phase 2 swaps the right panel's provider for a remote one
	 * (and adds the L/R side selectors). The user can drag the
	 * divider.
	 *
	 * NOTE: we set the position explicitly. With no position set,
	 * GtkPaned recomputes the split during every allocation pass
	 * based on the children's natural sizes — and the panel
	 * GtkColumnView's natural size changes when items-changed
	 * fires on the underlying GListStore (e.g. when a directory
	 * listing arrives). Each recomputation cascades into another
	 * allocation pass, which in GTK 4 ends up moving focus back to
	 * whichever panel has the "default" focus. End result: the
	 * focus-drift bug logged in BUGS — double-click on a folder in
	 * the right (remote) panel takes an extra click because the
	 * first click's focus is stolen mid-population.
	 *
	 * The empirical confirmation: the user reported that manually
	 * dragging the divider once stops the focus drift for the rest
	 * of the session. That's because a user drag toggles
	 * `position-set` to TRUE internally, after which GtkPaned no
	 * longer recomputes. Setting the position here up-front gets
	 * us into the same state from the start. */
    paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_resize_start_child (GTK_PANED (paned), TRUE);
    gtk_paned_set_resize_end_child (GTK_PANED (paned), TRUE);
    gtk_paned_set_shrink_start_child (GTK_PANED (paned), FALSE);
    gtk_paned_set_shrink_end_child (GTK_PANED (paned), FALSE);
    /* Centered split for the default 980 px window — see
	 * gtk_widget_set_size_request a few lines up. */
    gtk_paned_set_position (GTK_PANED (paned), 490);

    /* L = local FS (XDG_DOWNLOAD_DIR by default).
	 * R = remote Hotline server. The remote provider sits idle
	 * until the connection is up — the panel paints a
	 * "Not connected" state until then.
	 *
	 * Either side can be swapped at runtime via the per-panel
	 * side selector (see on_panel_swap_request below). When a
	 * swap fires we build a fresh provider of the requested
	 * kind — providers store current_path internally, so sharing
	 * one across both panels wouldn't compose (navigating one
	 * would yank the other). Fresh instances keep their state
	 * independent. */
    {
        HxLocalFilesProvider *local;
        HxRemoteFilesProvider *remote;
        local = hx_local_files_provider_new (NULL);
        remote = hx_remote_files_provider_new ();
        br->left_provider = HX_FILES_PROVIDER (local);
        br->right_provider = HX_FILES_PROVIDER (remote);
    }
    br->left = files_panel_new (br->left_provider, on_panel_swap_request, br);
    br->right = files_panel_new (br->right_provider, on_panel_swap_request, br);

    br->conn_state_handler = g_signal_connect (
        gtkhx_session_get_default (), "connection-state-changed",
        G_CALLBACK (on_connection_state), br);

    /* file-update for auto-refresh on transfer completion. The
	 * same signal already routes through gtkhx.c::on_file_update_signal
	 * for the legacy progress + toast notifications; we ride
	 * alongside that with a second listener. */
    br->file_update_handler
        = g_signal_connect (gtkhx_session_get_default (), "file-update",
                            G_CALLBACK (on_file_update), br);

    gtk_paned_set_start_child (GTK_PANED (paned),
                               files_panel_get_widget (br->left));
    gtk_paned_set_end_child (GTK_PANED (paned),
                             files_panel_get_widget (br->right));

    /* Wrap in a toast overlay so the Copy action (and future
	 * polish-phase actions) have somewhere to surface transient
	 * feedback ("Transfer queued.", "You don't have permission
	 * for that.", etc.) without an interrupting dialog. */
    br->toast = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
    adw_toast_overlay_set_child (br->toast, paned);
    gtk_window_set_child (GTK_WINDOW (br->window), GTK_WIDGET (br->toast));

    /* Track which panel has focus / was clicked so the headerbar
	 * actions know who to operate on. Wired AFTER both panels
	 * exist so attach_panel_focus_tracking can reach them via
	 * files_panel_get_widget. */
    attach_panel_focus_tracking (br, br->left);
    attach_panel_focus_tracking (br, br->right);

    /* DnD between panels: drag a row out of one panel and drop
	 * on the other to fire the same Copy machinery the headerbar
	 * button uses. Same-panel drops are a no-op. */
    attach_panel_dnd (br, br->left);
    attach_panel_dnd (br, br->right);

    /* Window-level keyboard shortcuts.
	 *
	 *   Tab        — switch active panel
	 *   Backspace  — up one directory in active panel
	 *
	 * Capture phase is the only way to intercept Tab — without it,
	 * GtkColumnView's built-in focus chain consumes the keystroke
	 * for column-to-column navigation before the window-level
	 * shortcut sees it. Same logic for Backspace though that one
	 * isn't normally claimed by descendants. */
    shortcuts = gtk_shortcut_controller_new ();
    gtk_event_controller_set_propagation_phase (shortcuts, GTK_PHASE_CAPTURE);
    gtk_shortcut_controller_set_scope (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                       GTK_SHORTCUT_SCOPE_GLOBAL);
    gtk_widget_add_controller (br->window, shortcuts);

    sh = gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Tab, 0),
                           gtk_callback_action_new (on_tab_shortcut, br, NULL));
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                          sh);

    sh = gtk_shortcut_new (
        gtk_keyval_trigger_new (GDK_KEY_BackSpace, 0),
        gtk_callback_action_new (on_backspace_shortcut, br, NULL));
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                          sh);

    /* F4 = "view/edit" (Norton F4 was Edit). Same action as
	 * Enter-on-row / double-click — routes through the
	 * provider's activate_entry, which picks preview for
	 * remote and xdg-open for local. Useful for users whose
	 * row focus isn't where their selection is (keyboard
	 * navigation in a multi-select). F3 (classic View)
	 * doesn't get a dedicated binding; the activation is
	 * preview-first anyway. */
    sh = gtk_shortcut_new (
        gtk_keyval_trigger_new (GDK_KEY_F4, 0),
        gtk_callback_action_new (on_open_shortcut, br, NULL));
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                          sh);

    /* F2 = Rename (modern Files-Manager keybinding). Route
	 * through on_rename_shortcut so the column view's internal
	 * F2 handling doesn't preempt us. */
    sh = gtk_shortcut_new (
        gtk_keyval_trigger_new (GDK_KEY_F2, 0),
        gtk_callback_action_new (on_rename_shortcut, br, NULL));
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                          sh);

    /* F6 = Move (classic Norton). Opens the move-destination
	 * dialog defaulting to the inactive panel's path. */
    sh = gtk_shortcut_new (
        gtk_keyval_trigger_new (GDK_KEY_F6, 0),
        gtk_callback_action_new (on_move_shortcut, br, NULL));
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                          sh);

    /* Ctrl+I = Get Info. Classic Mac was Cmd+I; we map to the
	 * Linux conventional equivalent. Remote-only — see the
	 * on_get_info_clicked toast for the local-panel hint. */
    sh = gtk_shortcut_new (
        gtk_keyval_trigger_new (GDK_KEY_i, GDK_CONTROL_MASK),
        gtk_callback_action_new (on_get_info_shortcut, br, NULL));
    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts),
                                          sh);

    g_signal_connect (br->window, "close-request", G_CALLBACK (on_close), br);

    the_browser = br;

    /* Standard window accelerators — Ctrl+W close, Ctrl+Q quit,
	 * Ctrl+K connect, Ctrl+T tracker. Same set every other
	 * window in the app picks up via init_keyaccel. Capture
	 * phase means the column views' internal focus chain
	 * doesn't swallow them. */
    init_keyaccel (br->window);

    /* Initial focus on the left panel so the user has a working
	 * active selection right away. */
    set_active (br, br->left);
    sync_copy_tooltip (br);
    gtk_widget_grab_focus (files_panel_get_column_view (br->left));

    gtk_window_present (GTK_WINDOW (br->window));
}
