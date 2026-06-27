/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <adwaita.h>
#include "hx.h"
#include "gtkhx.h"
#include "gtkutil.h"
#include "connect.h"
#include "chat.h"
#include "options.h"
#include "cfgkeys.h"
#include "debug.h"
#include "bookmarks.h"
#include "bookmark_cipher.h"
#include "hotline.h"
#include "toolbar.h"
#include "tracker.h"
#include "tracker_row.h"

static GtkWidget *tracker_window;
static GtkWidget *tracker_sections_box; /* vbox of per-tracker GtkExpanders */
static GtkWidget *tracker_search_entry;
static GtkWidget *tracker_case_btn;
static GtkWidget *lbl_found, *lbl_total;
static int num_found_total, num_total_total;

/* Search filter for the tracker list.
 *
 * was a hand-rolled `struct dfa *` from a vendored 2500-line
 * GNU regex; now a GLib GRegex (PCRE2). Two states matter:
 *   - current_search == NULL  → empty pattern, match-all (the common
 *                                case when the search entry is empty).
 *   - current_search != NULL  → compiled regex; g_regex_match decides
 *                                each row.
 * Invalid patterns (parse error during compile) also fall to match-all
 * with a one-line warning to the chat output. */
static GRegex *current_search;

/* Column ids weren't carried over to the GtkColumnView build —
 * each column is created inline in tracker_section_new by name. */

/* One section per tracker URL. Created on tracker_batch_begin,
 * populated by tracker_server_create calls that follow. The visible
 * UI is a GtkExpander whose child is a scrolled GtkColumnView;
 * section lifetime ties to the expander widget's lifetime (the box
 * owns the expander, we own the section data). Sections survive
 * across search filter changes (GtkFilterListModel just hides
 * non-matching rows) and across a refresh only by URL-keyed
 * recycling — tracker_clear blanks everything per refresh. */
struct tracker_section {
    char *url;        /* hash key — owned */
    guint8 version;   /* 1 or 3, from batch-begin */
    guint16 expected; /* batch-begin's announced count — used in subtitle */

    /* Model chain:
     *   store     — GListStore<HxTrackerRow *>, the unfiltered
     *               truth. Append-only during a fetch; cleared on
     *               batch-begin recycling.
     *   filter_model — GtkFilterListModel wrapping `store` and the
     *               shared tracker_filter (one GtkCustomFilter for
     *               the whole window — when the search regex
     *               changes, gtk_filter_changed makes every
     *               section's filter_model re-evaluate).
     *   sort_model — GtkSortListModel wrapping filter_model. Its
     *               sorter is the column view's "header sorter"
     *               (so clicking a column header sorts that
     *               column ascending / descending / unsorted in
     *               the usual cycle).
     *   selection — GtkSingleSelection wrapping sort_model. This
     *               is the model the column view binds to;
     *               notify::selected drives the cross-section
     *               (selected_section, selected_row) state below. */
    GListStore *store;
    GtkFilterListModel *filter_model;
    GtkSortListModel *sort_model;
    GtkSingleSelection *selection;

    /* O(1) dedup: keys are "<addr>:<port>" g_strdup'd; values are
     * unused (we only check presence). Per-section because the
     * same physical server appearing on tracker A AND tracker B
     * is the whole point of grouping by tracker, so the dedup
     * window is intra-section only. */
    GHashTable *dedup;

    GtkWidget *expander;    /* GtkExpander, child of tracker_sections_box */
    GtkWidget *column_view; /* GtkColumnView inside the expander */
    GtkWidget *scroll;      /* GtkScrolledWindow wrapping the column view */

    GtkColumnViewColumn *col_country; /* hidden on v1 sections */
    GtkColumnViewColumn *col_caps;    /* hidden on v1 sections */
};

/* All known sections keyed by URL. Values are owned struct
 * tracker_section *; on remove we free the value. tracker_clear
 * destroys this whole table on each refresh — sections aren't
 * recycled across refreshes because the expected-count subtitle
 * needs to come from the new batch's batch-begin, and the wire
 * version may have flipped (probe-then-fallback). */
static GHashTable *tracker_sections;

/* Creation order, so the visible expanders march down the box in
 * the same order trackers were queried. tracker_sections_box holds
 * the GtkExpander widgets in the same order. */
static GList *tracker_sections_order;

/* Most recent tracker-batch-begin sets this; subsequent
 * tracker-server-create signals land into this section's tree +
 * list. tracker_run_ctx in network.c walks trackers sequentially,
 * so there's no interleaving — the most recent batch-begin is the
 * sole sink until the next one. */
static struct tracker_section *current_section;

/* Selection across all sections — replaces the old single-list
 * tracker_storow + tracker_list pair. Updated by each section's
 * notify::selected handler; consumed by Details / Connect buttons
 * in the headerbar. NULL means "nothing selected anywhere." */
static struct tracker_section *selected_section;
static int selected_row;

/* One shared GtkCustomFilter for the whole window. Every section's
 * filter_model uses this same instance, so when tracker_rerun_search
 * recompiles the regex and calls gtk_filter_changed we get one
 * re-evaluation pass across all sections for free. The match
 * function (tracker_row_matches) reads the current_search GRegex
 * above. */
static GtkCustomFilter *tracker_filter;

/* Free a section: drop the model-chain GObject refs, drop the
 * dedup hashtable, detach the expander from the sections box, drop
 * the URL key. The widget tear-down chain (gtk_box_remove →
 * expander unrefs scrolled-window → unrefs column view, which
 * unrefs its selection model, which unrefs its filter model, which
 * unrefs the underlying store) cleans up the GTK side. We hold
 * parallel refs to the model-chain pieces during the section's
 * lifetime so model-side updates (g_list_store_append) don't need
 * to walk the widget tree. */
static void
tracker_section_free (struct tracker_section *sec)
{
    if (!sec) {
        return;
    }
    /* If this section owns the current selection / current batch,
     * clear the dangling pointers before the section dies. */
    if (selected_section == sec) {
        selected_section = NULL;
        selected_row = -1;
    }
    if (current_section == sec) {
        current_section = NULL;
    }
    /* Null the "tracker-section" qdata before we drop our refs on
     * the model + widget. If a deferred signal (notify::selected,
     * activate) fires during the dispose cascade after gtk_box_remove
     * below, the handler reads the qdata and bails on NULL — we don't
     * want it dereferencing `sec` while we're freeing it. */
    if (sec->selection) {
        g_object_set_data (G_OBJECT (sec->selection), "tracker-section", NULL);
    }
    if (sec->column_view) {
        g_object_set_data (G_OBJECT (sec->column_view), "tracker-section",
                           NULL);
    }
    g_clear_object (&sec->selection);
    g_clear_object (&sec->sort_model);
    g_clear_object (&sec->filter_model);
    g_clear_object (&sec->store);
    g_clear_pointer (&sec->dedup, g_hash_table_unref);
    if (sec->expander && tracker_sections_box) {
        gtk_box_remove (GTK_BOX (tracker_sections_box), sec->expander);
    }
    g_free (sec->url);
    g_free (sec);
}

/* GHashTable value-destroy hook. */
static void
tracker_section_free_hash (gpointer data)
{
    tracker_section_free ((struct tracker_section *)data);
}

void
tracker_clear (void)
{
    /* Drop the creation-order list FIRST so tracker_section_free's
     * gtk_box_remove calls don't leave dangling references in it. */
    g_list_free (tracker_sections_order);
    tracker_sections_order = NULL;

    current_section = NULL;
    selected_section = NULL;
    selected_row = -1;

    /* Destroying the hash table fires the per-value destroy hook
     * which tears each section down (BST + widgets). */
    if (tracker_sections) {
        g_hash_table_remove_all (tracker_sections);
    }

    num_found_total = 0;
    num_total_total = 0;
}

/* GTK 4 close-request on (GtkWindow *, gpointer). */
static gboolean
close_tracker_window (GtkWindow *window, gpointer data)
{
    (void)window;
    (void)data;

    tracker_clear ();
    if (tracker_sections) {
        g_hash_table_unref (tracker_sections);
        tracker_sections = NULL;
    }
    g_clear_object (&tracker_filter);

    tracker_window = NULL;
    tracker_sections_box = NULL;
    tracker_search_entry = NULL;
    tracker_case_btn = NULL;

    if (current_search) {
        g_regex_unref (current_search);
        current_search = NULL;
    }
    return FALSE;
}

/* The tracker fetch runs on the main loop via GSocketClient +
 * GInputStream async; cancellation is a g_cancellable_cancel against
 * a GCancellable held inside network.c's tracker_run_ctx.
 * tracker_kill_threads() in network.c trips that cancellation; the
 * in-flight async callback unwinds cleanly.
 *
 * Net effect: no worker thread to spawn, no signal handler, no
 * joinable handle to manage, and the cancel-after-free UAF the old
 * (Phase 5) design spent a paragraph defending against is
 * structurally impossible. */

static void
tracker_getlist (GtkWidget *widget, gpointer data)
{
    session *sess = data;
    (void)widget;

    /* Cancel any in-flight fetch and start fresh. */
    tracker_kill_threads ();

    tracker_clear ();

    gtk_label_set_text (GTK_LABEL (lbl_found), "  0");
    gtk_label_set_text (GTK_LABEL (lbl_total), " / 0");

    hx_tracker_list_async (sess);
}

/* Build the compact "Caps" column string from the typed v3 meta.
 * Each badge is appended only when its TLV said so; an empty buffer
 * (no caps advertised, or a v1 record with a zero-init meta) renders
 * as an empty cell. Fixed-size stack buffer is fine — the longest
 * possible string we emit fits in ~32 bytes.
 *
 * Badge legend (deliberately terse so the column stays narrow):
 *   ★    is_promoted     (operator-pinned in the tracker UI)
 *   HOPE supports_hope   (gtkhx's legacy crypto extension)
 *   TLS  supports_tls    (Mobius-style separate TLS port — Phase 7)
 *   v6   supports_ipv6   (the server has an IPv6 listener too)
 * Order is fixed for visual stability; the row reads naturally
 * left-to-right even when only one badge is present.
 */
static void
format_caps_badges (const HxTrackerV3Meta *m, char *out, gsize outsz)
{
    out[0] = '\0';
    if (!m) {
        return;
    }
    gboolean first = TRUE;
    /* "★" is U+2605 BLACK STAR — three UTF-8 bytes. The hlist is
     * UTF-8 throughout, so no transcoding needed. */
    if (m->is_promoted) {
        g_strlcat (out, "\xe2\x98\x85", outsz);
        first = FALSE;
    }
    if (m->supports_hope) {
        if (!first) {
            g_strlcat (out, " ", outsz);
        }
        g_strlcat (out, "HOPE", outsz);
        first = FALSE;
    }
    if (m->supports_tls) {
        if (!first) {
            g_strlcat (out, " ", outsz);
        }
        g_strlcat (out, "TLS", outsz);
        first = FALSE;
    }
    if (m->supports_ipv6) {
        if (!first) {
            g_strlcat (out, " ", outsz);
        }
        g_strlcat (out, "v6", outsz);
    }
}

/* GtkCustomFilter match callback. Called by every section's
 * GtkFilterListModel whenever a row arrives or the shared filter
 * is notified via gtk_filter_changed. Returns TRUE if the row
 * should be visible.
 *
 * current_search == NULL means "match everything" — the common
 * case when the search entry is empty. Otherwise both name and
 * desc are tried; GRegex's match callback handles NUL-terminated
 * UTF-8 strings via the row getters. */
static gboolean
tracker_row_matches (gpointer item, gpointer user_data)
{
    HxTrackerRow *row = item;
    (void)user_data;

    if (!row) {
        return FALSE;
    }
    if (!current_search) {
        return TRUE;
    }
    return g_regex_match (current_search, hx_tracker_row_get_name (row), 0,
                          NULL)
           || g_regex_match (current_search, hx_tracker_row_get_desc (row), 0,
                             NULL);
}

/* Convenience: counts straight off the model chain. The store's
 * count is "every row this tracker emitted, dedup'd"; the filter
 * model's count is "every row that passes the current filter."
 * Both update synchronously as rows are appended / removed / when
 * the filter changes — no need for parallel manual counters. */
static guint
tracker_section_num_total (struct tracker_section *sec)
{
    return sec && sec->store
               ? g_list_model_get_n_items (G_LIST_MODEL (sec->store))
               : 0;
}

static guint
tracker_section_num_found (struct tracker_section *sec)
{
    return sec && sec->filter_model
               ? g_list_model_get_n_items (G_LIST_MODEL (sec->filter_model))
               : 0;
}

/* Refresh the section's expander title with the latest counts. The
 * URL renders in bold (so it reads as the "section name"), with a
 * subtitle on the same line covering version + record counts.
 * Filter-narrowed counts use "N / M servers" shape so the user can
 * tell "I have 200 results from this tracker, search shows 12."
 *
 * The label is built with Pango markup; the URL is g_markup_escape'd
 * so an entity-like character (& in a URL query string, < in some
 * exotic hostname) doesn't blow up the markup parse. */
static void
tracker_section_update_title (struct tracker_section *sec)
{
    char *markup;
    char *url_escaped;
    char vbuf[8];
    guint n_total, n_found;

    if (!sec || !sec->expander) {
        return;
    }
    g_snprintf (vbuf, sizeof (vbuf), "v%u", (unsigned)sec->version);
    url_escaped = g_markup_escape_text (sec->url, -1);
    n_total = tracker_section_num_total (sec);
    n_found = tracker_section_num_found (sec);

    if (n_total == 0) {
        /* Empty so far. Show the expected count from batch-begin so
         * the user sees "expecting 200" rather than a flat "0
         * servers" while the tracker is still streaming records.
         * On a tracker that legitimately has nothing to list, the
         * expected==0 case falls to "no servers." */
        if (sec->expected) {
            markup = g_strdup_printf (
                "<b>%s</b>  <span alpha=\"60%%\">%s · %s %u</span>",
                url_escaped, vbuf, _ ("expecting"), (unsigned)sec->expected);
        } else {
            markup = g_strdup_printf (
                "<b>%s</b>  <span alpha=\"60%%\">%s · %s</span>", url_escaped,
                vbuf, _ ("no servers"));
        }
    } else if (n_found == n_total) {
        markup
            = g_strdup_printf ("<b>%s</b>  <span alpha=\"60%%\">%s · %u %s</span>",
                               url_escaped, vbuf, n_total,
                               n_total == 1 ? _ ("server") : _ ("servers"));
    } else {
        markup = g_strdup_printf (
            "<b>%s</b>  <span alpha=\"60%%\">%s · %u / %u %s</span>",
            url_escaped, vbuf, n_found, n_total,
            n_total == 1 ? _ ("server") : _ ("servers"));
    }
    /* Flip use-markup ON before writing the label so the first
     * call (when sec->expander was just constructed with no label)
     * can't transiently render the Pango tags as literal characters
     * — GtkExpander internally constructs its label widget on the
     * first set_label, and the markup bit needs to be in effect by
     * the time that widget renders. */
    gtk_expander_set_use_markup (GTK_EXPANDER (sec->expander), TRUE);
    gtk_expander_set_label (GTK_EXPANDER (sec->expander), markup);
    g_free (markup);
    g_free (url_escaped);
}

/* tracker_search runs every time the visible filter needs to
 * be re-applied — when the search entry's text changes, when the user
 * hits Enter, or when the case-sensitive toggle flips. We always read
 * the current entry text out of the cached tracker_search_entry rather
 * than out of the signal's `widget` argument, so the same routine can
 * be called from any of the three signal handlers (search-changed,
 * activate, case-toggle) plus the on-resync paths that don't get
 * called with a widget at all. */
static void
tracker_rerun_search (void)
{
    char *num;
    const char *str;
    GList *l;

    if (!tracker_sections_box || !tracker_search_entry) {
        return;
    }

    if (current_search) {
        g_regex_unref (current_search);
        current_search = NULL;
    }

    str = gtk_editable_get_text (GTK_EDITABLE (tracker_search_entry));
    if (str && *str) {
        /* Case-folding follows the gtkhx_prefs.track_case toggle:
         * track_case ON → match case; OFF → G_REGEX_CASELESS.
         * Bad patterns fall through to match-all + a chat warning, so
         * a user typing an incomplete regex doesn't blank the list. */
        GRegexCompileFlags flags = 0;
        GError *err = NULL;
        if (!gtkhx_prefs.track_case) {
            flags |= G_REGEX_CASELESS;
        }
        current_search = g_regex_new (str, flags, 0, &err);
        if (!current_search) {
            hx_printf_prefix (&the_session.htlc, 0, INFOPREFIX,
                              "Tracker regex: %s\n",
                              err ? err->message : "compile failed");
            g_clear_error (&err);
        }
    }

    /* One notification to the shared filter — every section's
     * GtkFilterListModel re-evaluates against the new regex.
     * GTK_FILTER_CHANGE_DIFFERENT means "treat all current
     * decisions as potentially invalidated" — the conservative
     * answer that's always correct after a fresh regex compile. */
    if (tracker_filter) {
        gtk_filter_changed (GTK_FILTER (tracker_filter),
                            GTK_FILTER_CHANGE_DIFFERENT);
    }

    /* Selection state needs to clear ON TWO FRONTS:
     *   - the global (selected_section, selected_row) so the
     *     headerbar Connect / Details actions no-op rather than
     *     act on a row that may have been filtered out or shifted
     *   - every per-section GtkSingleSelection so the column view
     *     doesn't keep a row visually highlighted when the
     *     headerbar actions would refuse to operate on it
     *
     * Without the second sweep, the UI is inconsistent: a row
     * looks selected but Connect / Details quietly do nothing.
     * Clearing both keeps the visual state in lockstep with the
     * functional state.
     *
     * The notify::selected handler on each section sees pos =
     * INVALID and clears the global state — which is fine since
     * we're about to overwrite the globals anyway. */
    selected_section = NULL;
    selected_row = -1;
    for (l = tracker_sections_order; l; l = l->next) {
        struct tracker_section *sec = l->data;
        if (sec->selection) {
            gtk_single_selection_set_selected (
                GTK_SINGLE_SELECTION (sec->selection),
                GTK_INVALID_LIST_POSITION);
        }
    }

    /* Roll up the per-section found counts and refresh every
     * section's expander subtitle. */
    num_found_total = 0;
    for (l = tracker_sections_order; l; l = l->next) {
        struct tracker_section *sec = l->data;
        tracker_section_update_title (sec);
        num_found_total += (int)tracker_section_num_found (sec);
    }

    num = g_strdup_printf ("  %d", num_found_total);
    gtk_label_set_text (GTK_LABEL (lbl_found), num);
    g_free (num);
}

static void
tracker_search (GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    tracker_rerun_search ();
}

void
tracker_search_refresh (void)
{
    tracker_rerun_search ();
}

/* "Aa" case-sensitive toggle in the search row. Routes through the
 * generic prefs setter so the Settings switch (if open) stays in
 * lockstep; the cfgvar change-callback then invokes our
 * tracker_search_refresh, which recompiles the GRegex with the new
 * G_REGEX_CASELESS bit so the visible result list reflects the new
 * case mode immediately. */
static void
tracker_case_toggled (GtkToggleButton *btn, gpointer data)
{
    (void)data;
    gtkhx_prefs_set_bool (CFG_TRACKER_CASE,
                          gtk_toggle_button_get_active (btn) ? 1 : 0);
    tracker_rerun_search ();
}

/* Per-section dedup key: "<address>:<port>". String-keyed
 * GHashTable inside each section so the same physical server
 * showing up on two different trackers lands in both sections,
 * but a tracker sending the same record twice gets dropped. */
static char *
dedup_key_for (const char *address, guint16 port)
{
    return g_strdup_printf ("%s:%u", address, (unsigned)port);
}

/* ---------------------------------------------------------------- */
/* Column factories (setup + bind pairs for GtkSignalListItemFactory) */
/* ---------------------------------------------------------------- */

/* Setup helpers build the per-row widget once when the list item
 * factory creates a new ListItem; bind helpers refresh that widget
 * from the HxTrackerRow as the column view scrolls and recycles
 * ListItems against different rows. */

static void
text_setup_left (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkWidget *lbl = gtk_label_new (NULL);
    (void)f;
    (void)d;
    gtk_label_set_xalign (GTK_LABEL (lbl), 0.0f);
    gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start (lbl, 6);
    gtk_widget_set_margin_end (lbl, 6);
    gtk_list_item_set_child (item, lbl);
}

static void
text_setup_center (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkWidget *lbl = gtk_label_new (NULL);
    (void)f;
    (void)d;
    gtk_label_set_xalign (GTK_LABEL (lbl), 0.5f);
    gtk_label_set_ellipsize (GTK_LABEL (lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start (lbl, 6);
    gtk_widget_set_margin_end (lbl, 6);
    gtk_list_item_set_child (item, lbl);
}

/* Stash a weak (non-ref) pointer to the GtkListItem on the cell
 * widget. The right-click gesture handler walks up the widget
 * tree from the click target until it finds a widget with this
 * qdata set, then reads the row position out of the list item.
 * GtkListItem is owned by the column view; the stash is a borrow
 * that's only valid while the cell is bound (which is the only
 * window during which a click can find it anyway).
 *
 * Also stash on the row widget so right-clicks in the row's
 * vertical padding (the few pixels Adwaita adds above + below
 * each cell, where no cell widget covers) can still locate the
 * row. GtkColumnView's internal hierarchy is:
 *   GtkColumnView → listview → row → cell → our label
 * so the row widget is our parent's parent. Stashing the same
 * GtkListItem on all seven cells' row-widget-grandparents is a
 * no-op redundancy (it's the same row widget across cells), but
 * keeps the bind callback uniform and avoids racing on which
 * cell binds first. */
static void
stash_list_item (GtkWidget *cell, GtkListItem *item)
{
    GtkWidget *cell_w, *row_w;
    g_object_set_data (G_OBJECT (cell), "tracker-list-item", item);
    cell_w = gtk_widget_get_parent (cell);
    row_w = cell_w ? gtk_widget_get_parent (cell_w) : NULL;
    if (row_w) {
        g_object_set_data (G_OBJECT (row_w), "tracker-list-item", item);
    }
}

static void
name_bind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkLabel *lbl = GTK_LABEL (gtk_list_item_get_child (item));
    HxTrackerRow *row = gtk_list_item_get_item (item);
    (void)f;
    (void)d;
    stash_list_item (GTK_WIDGET (lbl), item);
    gtk_label_set_text (lbl, row ? hx_tracker_row_get_name (row) : "");
}

static void
desc_bind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkLabel *lbl = GTK_LABEL (gtk_list_item_get_child (item));
    HxTrackerRow *row = gtk_list_item_get_item (item);
    (void)f;
    (void)d;
    stash_list_item (GTK_WIDGET (lbl), item);
    gtk_label_set_text (lbl, row ? hx_tracker_row_get_desc (row) : "");
}

static void
address_bind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkLabel *lbl = GTK_LABEL (gtk_list_item_get_child (item));
    HxTrackerRow *row = gtk_list_item_get_item (item);
    (void)f;
    (void)d;
    stash_list_item (GTK_WIDGET (lbl), item);
    gtk_label_set_text (lbl, row ? hx_tracker_row_get_address (row) : "");
}

static void
port_bind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkLabel *lbl = GTK_LABEL (gtk_list_item_get_child (item));
    HxTrackerRow *row = gtk_list_item_get_item (item);
    char buf[8];
    (void)f;
    (void)d;
    stash_list_item (GTK_WIDGET (lbl), item);
    if (!row) {
        gtk_label_set_text (lbl, "");
        return;
    }
    g_snprintf (buf, sizeof (buf), "%u",
                (unsigned)hx_tracker_row_get_port (row));
    gtk_label_set_text (lbl, buf);
}

static void
users_bind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkLabel *lbl = GTK_LABEL (gtk_list_item_get_child (item));
    HxTrackerRow *row = gtk_list_item_get_item (item);
    char buf[8];
    (void)f;
    (void)d;
    stash_list_item (GTK_WIDGET (lbl), item);
    if (!row) {
        gtk_label_set_text (lbl, "");
        return;
    }
    g_snprintf (buf, sizeof (buf), "%u",
                (unsigned)hx_tracker_row_get_nusers (row));
    gtk_label_set_text (lbl, buf);
}

static void
country_bind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkLabel *lbl = GTK_LABEL (gtk_list_item_get_child (item));
    HxTrackerRow *row = gtk_list_item_get_item (item);
    HxTrackerV3Meta *m;
    (void)f;
    (void)d;
    stash_list_item (GTK_WIDGET (lbl), item);
    if (!row) {
        gtk_label_set_text (lbl, "");
        return;
    }
    /* country_code is a 2-letter ISO 3166-1 alpha-2 code when the
     * tracker advertised it; absent → empty cell. The column is
     * hidden on v1 sections anyway. */
    m = hx_tracker_row_get_meta (row);
    gtk_label_set_text (lbl, (m && m->country_code) ? m->country_code : "");
}

static void
caps_bind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkLabel *lbl = GTK_LABEL (gtk_list_item_get_child (item));
    HxTrackerRow *row = gtk_list_item_get_item (item);
    char buf[48];
    (void)f;
    (void)d;
    stash_list_item (GTK_WIDGET (lbl), item);
    if (!row) {
        gtk_label_set_text (lbl, "");
        return;
    }
    format_caps_badges (hx_tracker_row_get_meta (row), buf, sizeof (buf));
    gtk_label_set_text (lbl, buf);
}

/* ---------------------------------------------------------------- */
/* Per-column comparison functions                                   */
/* ---------------------------------------------------------------- */

/* All comparators follow GCompareDataFunc semantics: return < 0
 * if `a` should sort before `b`, > 0 if after, 0 if equal. String
 * comparisons go through g_utf8_collate so locale-aware ordering
 * survives accented names ("Östra" sorts as the user expects).
 *
 * When a string field is NULL or empty, it sorts BEFORE non-empty
 * values — same convention as g_strcmp0. Country / Caps cells on
 * v1 records are always empty, but the v1 sections hide those
 * columns anyway so it doesn't visually matter. */

static int
cmp_str (const char *a, const char *b)
{
    if (a == b) {
        return 0;
    }
    if (!a) {
        return -1;
    }
    if (!b) {
        return 1;
    }
    return g_utf8_collate (a, b);
}

static int
cmp_uint (guint a, guint b)
{
    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    return 0;
}

static int
cmp_name (gconstpointer a, gconstpointer b, gpointer ud)
{
    (void)ud;
    return cmp_str (hx_tracker_row_get_name ((HxTrackerRow *)a),
                    hx_tracker_row_get_name ((HxTrackerRow *)b));
}

static int
cmp_desc (gconstpointer a, gconstpointer b, gpointer ud)
{
    (void)ud;
    return cmp_str (hx_tracker_row_get_desc ((HxTrackerRow *)a),
                    hx_tracker_row_get_desc ((HxTrackerRow *)b));
}

static int
cmp_address (gconstpointer a, gconstpointer b, gpointer ud)
{
    (void)ud;
    return cmp_str (hx_tracker_row_get_address ((HxTrackerRow *)a),
                    hx_tracker_row_get_address ((HxTrackerRow *)b));
}

static int
cmp_users (gconstpointer a, gconstpointer b, gpointer ud)
{
    (void)ud;
    return cmp_uint (hx_tracker_row_get_nusers ((HxTrackerRow *)a),
                     hx_tracker_row_get_nusers ((HxTrackerRow *)b));
}

static int
cmp_port (gconstpointer a, gconstpointer b, gpointer ud)
{
    (void)ud;
    return cmp_uint (hx_tracker_row_get_port ((HxTrackerRow *)a),
                     hx_tracker_row_get_port ((HxTrackerRow *)b));
}

static int
cmp_country (gconstpointer a, gconstpointer b, gpointer ud)
{
    HxTrackerV3Meta *ma = hx_tracker_row_get_meta ((HxTrackerRow *)a);
    HxTrackerV3Meta *mb = hx_tracker_row_get_meta ((HxTrackerRow *)b);
    (void)ud;
    return cmp_str (ma ? ma->country_code : NULL,
                    mb ? mb->country_code : NULL);
}

/* Caps column sorts on the same formatted-badge string the column
 * renders. Same characters, same widths, same ordering — gives a
 * stable, human-recognisable sort (★-prefixed rows cluster, HOPE-
 * only follow, etc.). */
static int
cmp_caps (gconstpointer a, gconstpointer b, gpointer ud)
{
    char ba[48], bb[48];
    (void)ud;
    format_caps_badges (hx_tracker_row_get_meta ((HxTrackerRow *)a), ba,
                        sizeof (ba));
    format_caps_badges (hx_tracker_row_get_meta ((HxTrackerRow *)b), bb,
                        sizeof (bb));
    return cmp_str (ba, bb);
}

/* Build one column from a (setup, bind) pair. Wraps the boilerplate
 * for a signal-based factory + column construction + sorter
 * attachment. Returns a borrowed pointer (column view holds the
 * only strong ref afterwards).
 *
 * `cmp` may be NULL for columns that shouldn't be sortable, but
 * the tracker view makes every column sortable. */
static GtkColumnViewColumn *
add_column (GtkColumnView *view, const char *title,
            void (*setup) (GtkSignalListItemFactory *, GtkListItem *, gpointer),
            void (*bind) (GtkSignalListItemFactory *, GtkListItem *, gpointer),
            GCompareDataFunc cmp, int fixed_width, gboolean expand)
{
    GtkListItemFactory *factory;
    GtkColumnViewColumn *col;

    factory = gtk_signal_list_item_factory_new ();
    g_signal_connect (factory, "setup", G_CALLBACK (setup), NULL);
    g_signal_connect (factory, "bind", G_CALLBACK (bind), NULL);

    col = gtk_column_view_column_new (title, factory);
    if (fixed_width > 0) {
        gtk_column_view_column_set_fixed_width (col, fixed_width);
    }
    gtk_column_view_column_set_expand (col, expand);
    gtk_column_view_column_set_resizable (col, TRUE);

    if (cmp) {
        GtkSorter *sorter = GTK_SORTER (gtk_custom_sorter_new (cmp, NULL, NULL));
        gtk_column_view_column_set_sorter (col, sorter);
        g_object_unref (sorter);
    }

    gtk_column_view_append_column (view, col);
    g_object_unref (col);
    return col;
}

/* Forward decls — the selection / activate handlers live below
 * tracker_section_new so they can reach the section's siblings via
 * the typed struct; tracker_section_new wires them on its newly-
 * built selection + column view. */
static void on_section_selected_changed (GObject *obj, GParamSpec *ps,
                                         gpointer data);
static void on_section_activate (GtkColumnView *cv, guint pos, gpointer data);
static void on_section_secondary_press (GtkGestureClick *gesture, int n_press,
                                        double x, double y, gpointer data);
/* The right-click popover wires these as button click actions. */
static void tracker_connect (void);
static void tracker_save_bookmark (void);
static void tracker_details (void);

/* Build the per-section UI. Model chain:
 *   GListStore → GtkFilterListModel (shared filter)
 *              → GtkSingleSelection → GtkColumnView
 *
 * v1 sections hide the Country / Caps columns since their records
 * can't carry those TLVs. The section pointer is stashed on both
 * the selection model (for notify::selected) and the column view
 * (for activate) so the handlers can recover their owner without
 * a global widget→section lookup. */
static struct tracker_section *
tracker_section_new (const char *url, guint8 version, guint16 expected)
{
    struct tracker_section *sec;
    GtkColumnView *cv;

    sec = g_new0 (struct tracker_section, 1);
    sec->url = g_strdup (url ? url : "");
    sec->version = version;
    sec->expected = expected;
    sec->dedup
        = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    sec->store = g_list_store_new (HX_TYPE_TRACKER_ROW);

    /* The shared tracker_filter (owned by create_tracker_window)
     * gates which rows pass through. set_incremental(FALSE)
     * because the row counts here are modest (typically <500 per
     * tracker) and we want the filter view to reflect a single
     * append synchronously — the headerbar count label and the
     * section subtitle both read from get_n_items right after
     * the store append. */
    sec->filter_model
        = gtk_filter_list_model_new (G_LIST_MODEL (g_object_ref (sec->store)),
                                     GTK_FILTER (
                                         g_object_ref (tracker_filter)));
    gtk_filter_list_model_set_incremental (sec->filter_model, FALSE);

    /* Sort model sits between the filter and the selection. Its
     * sorter is plugged in below — gtk_column_view_get_sorter
     * returns a "header sorter" that updates as the user clicks
     * column headers (cycle: ascending → descending → unsorted).
     * Starts with no sorter so the visible order matches
     * tracker-arrival order until the user touches a header. */
    sec->sort_model = gtk_sort_list_model_new (
        G_LIST_MODEL (g_object_ref (sec->filter_model)), NULL);
    gtk_sort_list_model_set_incremental (sec->sort_model, FALSE);

    sec->selection = gtk_single_selection_new (
        G_LIST_MODEL (g_object_ref (sec->sort_model)));
    /* The default is autoselect: the model picks position 0 on
     * model load. We want the headerbar Details / Connect buttons
     * to act on rows the user has actually clicked, not an
     * arbitrary first row, so turn autoselect off and start
     * unselected. */
    gtk_single_selection_set_autoselect (sec->selection, FALSE);
    gtk_single_selection_set_can_unselect (sec->selection, TRUE);
    gtk_single_selection_set_selected (sec->selection,
                                       GTK_INVALID_LIST_POSITION);

    sec->column_view = gtk_column_view_new (
        GTK_SELECTION_MODEL (g_object_ref (sec->selection)));
    cv = GTK_COLUMN_VIEW (sec->column_view);
    /* Single-click activation is a touch aggressive — keep the
     * default (double-click / Enter to activate) so the Connect
     * button + single-click selection stay distinguishable. */
    gtk_column_view_set_show_column_separators (cv, TRUE);
    gtk_column_view_set_show_row_separators (cv, FALSE);

    add_column (cv, _ ("Name"), text_setup_left, name_bind, cmp_name, 200,
                TRUE);
    add_column (cv, _ ("Users"), text_setup_center, users_bind, cmp_users, 76,
                FALSE);
    sec->col_country = add_column (cv, _ ("Country"), text_setup_center,
                                   country_bind, cmp_country, 60, FALSE);
    add_column (cv, _ ("Address"), text_setup_left, address_bind, cmp_address,
                150, FALSE);
    add_column (cv, _ ("Port"), text_setup_center, port_bind, cmp_port, 70,
                FALSE);
    sec->col_caps = add_column (cv, _ ("Caps"), text_setup_left, caps_bind,
                                cmp_caps, 110, FALSE);
    add_column (cv, _ ("Description"), text_setup_left, desc_bind, cmp_desc,
                280, TRUE);

    /* Plumb the column view's header sorter into the sort model.
     * gtk_column_view_get_sorter returns a GtkColumnViewSorter that
     * tracks which header was clicked + the sort direction; setting
     * it on the sort model makes the model re-sort on every click.
     * The sorter belongs to the column view (no extra ref needed). */
    gtk_sort_list_model_set_sorter (sec->sort_model,
                                    gtk_column_view_get_sorter (cv));

    /* v1 sections suppress the columns that can ONLY ever be empty
     * for v1 records. v3 sections keep them visible even when
     * individual records didn't advertise those TLVs (a v3 tracker
     * could carry mixed records, and the column being present is
     * informative on its own). */
    if (version == 1) {
        gtk_column_view_column_set_visible (sec->col_country, FALSE);
        gtk_column_view_column_set_visible (sec->col_caps, FALSE);
    }

    /* Stash the section pointer on both the selection (for
     * notify::selected) and the column view (for activate). The
     * handlers recover their owner from there — no global
     * widget→section table needed. */
    g_object_set_data (G_OBJECT (sec->selection), "tracker-section", sec);
    g_object_set_data (G_OBJECT (sec->column_view), "tracker-section", sec);

    g_signal_connect (sec->selection, "notify::selected",
                      G_CALLBACK (on_section_selected_changed), NULL);
    g_signal_connect (sec->column_view, "activate",
                      G_CALLBACK (on_section_activate), NULL);

    /* Right-click context menu: GtkGestureClick on the column view
     * itself listening for the secondary mouse button. Capture phase
     * so we see the press before GtkColumnView's internal handlers
     * (they ignore secondary clicks anyway, but capture-phase keeps
     * the ordering deterministic). The handler picks the deepest
     * widget under (x,y), walks up to find a cell with a stashed
     * GtkListItem, and pops up a row-context menu. */
    {
        GtkGesture *rclick = gtk_gesture_click_new ();
        gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (rclick),
                                       GDK_BUTTON_SECONDARY);
        gtk_event_controller_set_propagation_phase (
            GTK_EVENT_CONTROLLER (rclick), GTK_PHASE_CAPTURE);
        g_signal_connect (rclick, "pressed",
                          G_CALLBACK (on_section_secondary_press), NULL);
        gtk_widget_add_controller (sec->column_view,
                                   GTK_EVENT_CONTROLLER (rclick));
    }

    /* GtkColumnView wraps its content in its own internal scroller
     * for column-header alignment, but we still want a per-section
     * GtkScrolledWindow to give horizontal column-overflow scrolling
     * AND a visible frame around each section.
     *
     * Vertical = NEVER + propagate-natural-height: the section's
     *   height = the natural height of its content (header + rows).
     *   The outer sections-stack scroller handles the vertical
     *   scrolling for the window as a whole.
     * Horizontal = AUTOMATIC: per-section horizontal scrollbar
     *   appears when the columns overflow. Per-section so that
     *   v1 and v3 sections (with different visible columns) can
     *   each present their own row of columns without one section
     *   forcing the other's column widths. */
    sec->scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sec->scroll),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (sec->scroll), TRUE);
    gtk_scrolled_window_set_propagate_natural_height (
        GTK_SCROLLED_WINDOW (sec->scroll), TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sec->scroll),
                                   sec->column_view);

    sec->expander = gtk_expander_new (NULL);
    gtk_expander_set_expanded (GTK_EXPANDER (sec->expander), TRUE);
    gtk_expander_set_child (GTK_EXPANDER (sec->expander), sec->scroll);

    tracker_section_update_title (sec);

    return sec;
}

void
tracker_batch_begin (const char *tracker_url, guint8 version,
                     guint16 expected_count)
{
    struct tracker_section *sec;

    /* Tracker window not open — model still fires the signals (it
     * doesn't know about UI lifecycle), so just no-op. The next
     * window open will see a fresh empty state and the user can
     * hit Refresh to drive a new run. */
    if (!tracker_sections_box || !tracker_sections) {
        return;
    }
    if (!tracker_url) {
        tracker_url = "";
    }

    /* A batch-begin for a URL we already have a section for means
     * the user hit Refresh between fetches and the old section is
     * still hanging around (shouldn't happen — tracker_clear runs
     * at the top of tracker_getlist — but treat it idempotently to
     * be safe). Recycle the existing section.
     *
     * The aggregate headerbar counters track sum-over-all-sections,
     * so emptying this section means subtracting its old totals
     * from them. Similarly, if our selection was in this section,
     * clear it now since the row about to be wiped from the store
     * would leave a dangling selected_row index. */
    sec = g_hash_table_lookup (tracker_sections, tracker_url);
    if (sec) {
        guint old_total = tracker_section_num_total (sec);
        guint old_found = tracker_section_num_found (sec);
        char *num;

        sec->version = version;
        sec->expected = expected_count;
        if (selected_section == sec) {
            selected_section = NULL;
            selected_row = -1;
            if (sec->selection) {
                gtk_single_selection_set_selected (
                    GTK_SINGLE_SELECTION (sec->selection),
                    GTK_INVALID_LIST_POSITION);
            }
        }
        g_list_store_remove_all (sec->store);
        g_hash_table_remove_all (sec->dedup);
        gtk_column_view_column_set_visible (sec->col_country, version != 1);
        gtk_column_view_column_set_visible (sec->col_caps, version != 1);

        /* Reconcile aggregate counters: this section just went
         * from (old_total, old_found) → (0, 0). */
        num_total_total -= (int)old_total;
        num_found_total -= (int)old_found;
        num = g_strdup_printf ("  %d", num_found_total);
        gtk_label_set_text (GTK_LABEL (lbl_found), num);
        g_free (num);
        num = g_strdup_printf (" / %d", num_total_total);
        gtk_label_set_text (GTK_LABEL (lbl_total), num);
        g_free (num);

        tracker_section_update_title (sec);
        current_section = sec;
        return;
    }

    sec = tracker_section_new (tracker_url, version, expected_count);
    g_hash_table_insert (tracker_sections, g_strdup (tracker_url), sec);
    tracker_sections_order = g_list_append (tracker_sections_order, sec);
    gtk_box_append (GTK_BOX (tracker_sections_box), sec->expander);
    current_section = sec;
}

void
tracker_server_create (HxTrackerServer *event)
{
    HxTrackerRow *row;
    struct tracker_section *sec;
    char *dedup_key;
    char *num;
    guint old_found, new_found;

    if (!event || !tracker_sections_box) {
        return;
    }

    /* Drop into the section that the most recent batch-begin selected.
     * If a record sneaks through without a preceding batch-begin (a
     * model bug we want to notice, not paper over), debug-log and
     * drop the record rather than render it into a random section. */
    sec = current_section;
    if (!sec) {
        debug_log ("tracker",
                   "tracker_server_create with no current_section "
                   "(batch-begin missed?); dropping record name=%s",
                   event->name ? event->name : "(none)");
        return;
    }

    /* event->address is always non-NULL (the constructor guarantees
     * it for every addr_type — see tracker_event.c::format_address).
     * Defensive bail anyway so a future refactor that loosens that
     * invariant doesn't NULL-deref the dedup key. */
    if (!event->address || !*event->address) {
        debug_log ("tracker",
                   "skipping record with empty address (addr_type=0x%02x, "
                   "name=%s)",
                   event->addr_type, event->name ? event->name : "(no name)");
        return;
    }

    /* Dedup is per-section: the same server appearing on tracker A
     * and tracker B is intentionally shown twice, once per section,
     * because the per-tracker view IS the whole point of the
     * grouping. The hashtable owns the dedup key strings; we
     * insert with NULL value since we only care about presence. */
    dedup_key = dedup_key_for (event->address, event->port);
    if (g_hash_table_contains (sec->dedup, dedup_key)) {
        g_free (dedup_key);
        return;
    }
    g_hash_table_insert (sec->dedup, dedup_key, NULL);

    /* Append to the per-section GListStore. The filter model
     * decides synchronously (set_incremental FALSE) whether the
     * new row passes the current GRegex; if so, the section's
     * num_found ticks up by one. We capture old/new found counts
     * to update the headerbar's aggregate filtered count cheaply
     * (avoids walking every section). */
    old_found = tracker_section_num_found (sec);
    row = hx_tracker_row_new_from_event (event);
    g_list_store_append (sec->store, row);
    g_object_unref (row);
    new_found = tracker_section_num_found (sec);

    num_total_total++;
    if (new_found != old_found) {
        num_found_total += (int)(new_found - old_found);
    }

    tracker_section_update_title (sec);

    /* num_total_total always advanced (one new record landed,
     * matched or not), so the trailing " / N" label must redraw
     * every time — otherwise a filter that hides incoming records
     * leaves the total stuck at whatever value it had when the
     * last match arrived. The leading "  M" label only changes
     * when a match landed, so keep that one conditional to skip
     * the no-op pango-relayout. */
    num = g_strdup_printf (" / %d", num_total_total);
    gtk_label_set_text (GTK_LABEL (lbl_total), num);
    g_free (num);

    if (new_found != old_found) {
        num = g_strdup_printf ("  %d", num_found_total);
        gtk_label_set_text (GTK_LABEL (lbl_found), num);
        g_free (num);
    }
}

/* Helper: pull the HxTrackerRow currently at `pos` in `sec`'s
 * filtered view (i.e. what's actually visible in the column view).
 * Returns a transfer-full GObject ref — caller must g_object_unref. */
static HxTrackerRow *
tracker_section_get_row (struct tracker_section *sec, int pos)
{
    if (!sec || pos < 0) {
        return NULL;
    }
    return g_list_model_get_item (G_LIST_MODEL (sec->selection), (guint)pos);
}

/* notify::selected handler — fires on every selection change on
 * every section. Two responsibilities:
 *   1. Mirror the section's selection into our global (selected_
 *      section, selected_row) so the headerbar Details / Connect
 *      buttons act on the right row.
 *   2. Cross-section single-selection invariant: when section A
 *      gets a selection, clear section B's so only one row is
 *      visually highlighted at a time.
 *
 * Recursion control: clearing B's selection fires its own
 * notify::selected with pos==INVALID. The handler checks whether
 * selected_section still points at it before nulling the global
 * state, so the recursive call sees the new selected_section is
 * already someone else and no-ops. */
static void
on_section_selected_changed (GObject *obj, GParamSpec *ps, gpointer data)
{
    GtkSingleSelection *sel = GTK_SINGLE_SELECTION (obj);
    struct tracker_section *sec
        = g_object_get_data (G_OBJECT (obj), "tracker-section");
    guint pos;
    (void)ps;
    (void)data;

    if (!sec) {
        return;
    }
    pos = gtk_single_selection_get_selected (sel);

    if (pos == GTK_INVALID_LIST_POSITION) {
        /* This section is being cleared. Only blank the global
         * state if it still points at us — otherwise we're the
         * recursive callee handling someone else's clear. */
        if (selected_section == sec) {
            selected_section = NULL;
            selected_row = -1;
        }
        return;
    }

    /* Set global state FIRST so the recursive clear below sees
     * selected_section != cleared_sec and skips the global
     * blanking. */
    {
        struct tracker_section *old = selected_section;
        selected_section = sec;
        selected_row = (int)pos;

        if (old && old != sec && old->selection) {
            gtk_single_selection_set_selected (
                GTK_SINGLE_SELECTION (old->selection),
                GTK_INVALID_LIST_POSITION);
        }
    }
}

/* ---------------------------------------------------------------- */
/* Best-effort v3 security picking                                   */
/*                                                                   */
/* When the row's tracker carried v3 TLVs telling us this server     */
/* supports TLS or HOPE, we'd like to auto-prefer the stronger       */
/* transport rather than connecting plaintext-by-default. The        */
/* picker returns:                                                   */
/*   *out_port    — the port to connect to (regular port, or         */
/*                  tls_port when TLS is on)                         */
/*   *out_tls     — 1 if TLS, 0 otherwise                            */
/*   *out_secure  — 1 if HOPE on, 0 otherwise                        */
/*   *out_cipher  — bookmark-vocabulary cipher byte (0 = none)       */
/* The picker is conservative: TLS wins over HOPE when both are      */
/* offered, since dedicated-port TLS is independently auditable      */
/* and doesn't rely on the legacy HOPE handshake. The HOPE-cipher    */
/* preference order is CHACHA20-POLY1305 > BLOWFISH (RC4 retired).   */
/* ---------------------------------------------------------------- */

/* Substring-match a cipher name in the comma/space-separated
 * hope_ciphers string. Case-insensitive so a tracker emitting
 * "chacha20-poly1305" matches our canonical "CHACHA20-POLY1305".
 * Whole-token match: searching for "RC4" doesn't trigger on
 * "RC4-DROP" (a hypothetical future name) because we require the
 * match to be bracketed by start-of-string / end-of-string / a
 * delimiter on both sides. */
static gboolean
hope_ciphers_offers (const char *list, const char *needle)
{
    gchar *folded_list, *folded_needle;
    gchar **tokens;
    gboolean found = FALSE;
    int i;

    if (!list || !*list || !needle || !*needle) {
        return FALSE;
    }
    folded_list = g_ascii_strdown (list, -1);
    folded_needle = g_ascii_strdown (needle, -1);
    /* Split on common delimiters seen in the wild: comma, space,
     * semicolon. g_strsplit_set tolerates empty tokens, so a list
     * like "A,,B" doesn't trip the strcmp below. */
    tokens = g_strsplit_set (folded_list, ", ;", -1);
    for (i = 0; tokens[i]; i++) {
        const char *t = g_strstrip (tokens[i]);
        if (*t && g_strcmp0 (t, folded_needle) == 0) {
            found = TRUE;
            break;
        }
    }
    g_strfreev (tokens);
    g_free (folded_list);
    g_free (folded_needle);
    return found;
}

static void
tracker_row_pick_security (HxTrackerRow *row, guint16 *out_port,
                           gboolean *out_tls, gboolean *out_secure,
                           unsigned char *out_cipher)
{
    HxTrackerV3Meta *m;
    guint16 port;

    port = hx_tracker_row_get_port (row);
    *out_port = port;
    *out_tls = FALSE;
    *out_secure = FALSE;
    *out_cipher = BOOKMARK_CIPHER_BYTE_NONE;

    m = hx_tracker_row_get_meta (row);
    if (!m) {
        return;
    }

    /* TLS first: dedicated port, independently auditable. tls_port
     * MUST be non-zero — supports_tls without a port is a malformed
     * TLV combo, defer to HOPE. */
    if (m->supports_tls && m->tls_port) {
        *out_port = m->tls_port;
        *out_tls = TRUE;
        return;
    }

    /* HOPE: pick the strongest cipher the server advertised. The
     * hope_ciphers TLV is optional — older HOPE-supporting servers
     * may not advertise specifics, in which case we leave the cipher
     * unset and let hx_connect's HOPE handshake negotiate. */
    if (m->supports_hope) {
        *out_secure = TRUE;
        if (hope_ciphers_offers (m->hope_ciphers, "CHACHA20-POLY1305")) {
            *out_cipher = BOOKMARK_CIPHER_BYTE_CHACHA20_POLY1305;
        } else if (hope_ciphers_offers (m->hope_ciphers, "BLOWFISH")) {
            *out_cipher = BOOKMARK_CIPHER_BYTE_BLOWFISH;
        }
        /* No hope_ciphers info → leave cipher = NONE; the HOPE
         * handshake will negotiate whatever the server + client
         * have in common. */
    }
}

/* ---------------------------------------------------------------- */
/* Bookmark save                                                    */
/* ---------------------------------------------------------------- */

#define TRACKER_MAX_BOOKMARK_NAME 96

/* Sanitize a tracker-provided server name for use as a bookmark
 * filename. Strategy:
 *   1. Strip ASCII control chars (< 0x20, 0x7F).
 *   2. Replace filesystem-meaningful punctuation (/, \\, :, *, ?,
 *      ", <, >, |) with '_'. The union of POSIX-bad and Windows-bad
 *      keeps the resulting filename portable to network mounts.
 *   3. Strip leading whitespace + dots so we don't generate hidden
 *      files or oddly-leading names.
 *   4. Strip trailing whitespace.
 *   5. Reject "." / ".." reserved names (post-stripping).
 *   6. Cap length at TRACKER_MAX_BOOKMARK_NAME bytes, snapped to a
 *      UTF-8 boundary so we don't slice a multi-byte sequence.
 *   7. Empty result → fall back to `fallback` (typically the
 *      server address), or "server" if that's empty too.
 *
 * Returns a fresh g_strdup'd buffer; caller frees with g_free. */
static char *
tracker_safe_bookmark_filename (const char *raw, const char *fallback)
{
    GString *out;
    const char *p;
    gsize start;

    if (!raw || !*raw) {
        raw = (fallback && *fallback) ? fallback : "server";
    }

    out = g_string_new (NULL);
    for (p = raw; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7F) {
            continue;
        }
        switch (c) {
        case '/':
        case '\\':
        case ':':
        case '*':
        case '?':
        case '"':
        case '<':
        case '>':
        case '|':
            g_string_append_c (out, '_');
            break;
        default:
            g_string_append_c (out, c);
            break;
        }
    }

    /* Strip leading whitespace + dots. */
    start = 0;
    while (start < out->len
           && (out->str[start] == ' ' || out->str[start] == '\t'
               || out->str[start] == '.')) {
        start++;
    }
    if (start > 0) {
        g_string_erase (out, 0, (gssize)start);
    }
    /* Trim trailing whitespace. */
    while (out->len > 0
           && (out->str[out->len - 1] == ' '
               || out->str[out->len - 1] == '\t')) {
        g_string_truncate (out, out->len - 1);
    }

    /* Reserved names → fall back. */
    if (out->len == 0 || g_strcmp0 (out->str, ".") == 0
        || g_strcmp0 (out->str, "..") == 0) {
        g_string_truncate (out, 0);
        g_string_append (out,
                         (fallback && *fallback) ? fallback : "server");
    }

    /* Cap length. If the truncation would slice mid-UTF8, snap back
     * to the last valid boundary. */
    if (out->len > TRACKER_MAX_BOOKMARK_NAME) {
        const char *valid_end = out->str + TRACKER_MAX_BOOKMARK_NAME;
        if (!g_utf8_validate (out->str, TRACKER_MAX_BOOKMARK_NAME,
                              &valid_end)) {
            /* valid_end now points at the first invalid byte; back
             * up to the start of the partial sequence. */
        }
        g_string_truncate (out, (gsize)(valid_end - out->str));
    }

    return g_string_free (out, FALSE);
}

/* Build + save an HxBookmark for the given row. Applies the v3
 * security picker so the bookmark records "use TLS on tls_port"
 * or "use HOPE with cipher X" when the tracker advertised them.
 * Surfaces success / failure via toolbar_show_toast. */
static void
tracker_save_bookmark_for_row (HxTrackerRow *row)
{
    HxBookmark *bm;
    char *safe_name, *display_name;
    GError *err = NULL;
    guint16 port;
    gboolean tls, secure;
    unsigned char cipher;
    char toastbuf[256];

    if (!row) {
        return;
    }

    bm = hx_bookmark_new ();
    if (!bm) {
        return;
    }

    /* Sanitize the server name into a filesystem-safe filename
     * with a fallback to the server address. hx_bookmark_safe_
     * filename swaps any residual '/' for '\\' on top of our
     * sanitizer's '/'→'_' pass. */
    safe_name = tracker_safe_bookmark_filename (
        hx_tracker_row_get_name (row), hx_tracker_row_get_address (row));
    display_name = safe_name ? hx_bookmark_safe_filename (safe_name) : NULL;
    g_free (safe_name);
    if (!display_name) {
        toolbar_show_toast (
            _ ("Couldn't pick a bookmark filename for this server."));
        hx_bookmark_free (bm);
        return;
    }

    /* Refuse to clobber an existing bookmark with the same name.
     * Saving silently with "w" would overwrite whatever's there;
     * surface the conflict so the user can rename or delete the
     * existing one from the Bookmarks dialog. */
    {
        g_autoptr (HxBookmark) existing = hx_bookmark_load (display_name);
        if (existing) {
            g_snprintf (toastbuf, sizeof (toastbuf),
                        _ ("Bookmark \"%s\" already exists. Manage it from "
                           "the Bookmarks dialog."),
                        display_name);
            toolbar_show_toast (toastbuf);
            g_free (display_name);
            hx_bookmark_free (bm);
            return;
        }
    }

    bm->name = display_name;

    /* Pick best transport security. */
    tracker_row_pick_security (row, &port, &tls, &secure, &cipher);

    g_strlcpy (bm->server, hx_tracker_row_get_address (row),
               sizeof (bm->server));
    g_snprintf (bm->port, sizeof (bm->port), "%u", (unsigned)port);
    /* Leave login/pass empty: tracker hits don't carry credentials,
     * and the user can fill them in via the bookmarks dialog. */
    bm->login[0] = '\0';
    bm->pass[0] = '\0';
    bm->secure = secure ? 1 : 0;
    bm->compress = 0; /* HOPE compression negotiates at connect time */
    bm->cipher = (char)cipher;
    bm->tls = tls ? 1 : 0;

    if (!hx_bookmark_save (bm, &err)) {
        g_snprintf (toastbuf, sizeof (toastbuf),
                    _ ("Couldn't save bookmark: %s"),
                    err ? err->message : "unknown error");
        toolbar_show_toast (toastbuf);
        g_clear_error (&err);
    } else {
        g_snprintf (toastbuf, sizeof (toastbuf),
                    _ ("Saved bookmark \"%s\""), bm->name);
        toolbar_show_toast (toastbuf);
    }
    hx_bookmark_free (bm);
}

/* GtkColumnView "activate" — fired on double-click or Enter. Same
 * intent as the old gesture handler's n_press==2 path: connect to
 * the row's server immediately. When the tracker carried v3 TLVs
 * about transport security (supports_tls / supports_hope /
 * hope_ciphers), the picker auto-enables the strongest available
 * — TLS on tls_port if offered, else HOPE with the best known
 * cipher. v1 records all fall through plaintext. */
static void
on_section_activate (GtkColumnView *cv, guint pos, gpointer data)
{
    struct tracker_section *sec
        = g_object_get_data (G_OBJECT (cv), "tracker-section");
    HxTrackerRow *row;
    guint16 port;
    gboolean tls, secure;
    unsigned char cipher_byte;
    const char *cipher_name;
    (void)data;

    if (!sec) {
        return;
    }
    row = tracker_section_get_row (sec, (int)pos);
    if (!row) {
        return;
    }

    memset (the_session.htlc.compressalg, 0,
            sizeof (the_session.htlc.compressalg));
    memset (the_session.htlc.cipheralg, 0,
            sizeof (the_session.htlc.cipheralg));

    tracker_row_pick_security (row, &port, &tls, &secure, &cipher_byte);
    cipher_name = bookmark_cipher_name (cipher_byte);
    if (cipher_name) {
        g_strlcpy (the_session.htlc.cipheralg, cipher_name,
                   sizeof (the_session.htlc.cipheralg));
    }

    /* Phase E: address is the printable string the boxed event
     * delivered — IPv4 dotted-quad, IPv6 colon-hex, or a literal
     * hostname. hx_connect's first arg is a host string that
     * GSocketClient resolves via getaddrinfo, so all three forms
     * route through the same resolver and connect happily. */
    hx_connect (&the_session.htlc, hx_tracker_row_get_address (row), port,
                "", "", secure ? 1 : 0, tls ? 1 : 0);
    g_object_unref (row);
}

/* Helper: build one button for the right-click popover. The
 * callback receives the popover so it can pop it down before
 * (or after) invoking the action. */
typedef void (*menu_action_func) (void);

static void
on_popover_action_clicked (GtkButton *btn, gpointer user_data)
{
    menu_action_func action = g_object_get_data (G_OBJECT (btn), "menu-action");
    GtkWidget *popover = user_data;

    if (action) {
        action ();
    }
    if (popover) {
        gtk_popover_popdown (GTK_POPOVER (popover));
    }
}

static GtkWidget *
build_menu_button (const char *label, menu_action_func action,
                   GtkWidget *popover)
{
    GtkWidget *btn = gtk_button_new_with_label (label);
    GtkWidget *child;
    gtk_button_set_has_frame (GTK_BUTTON (btn), FALSE);
    /* Left-align the label like a real menu item rather than the
     * default center-aligned GtkButton. */
    child = gtk_button_get_child (GTK_BUTTON (btn));
    if (child) {
        gtk_widget_set_halign (child, GTK_ALIGN_START);
    }
    gtk_widget_set_hexpand (btn, TRUE);
    g_object_set_data (G_OBJECT (btn), "menu-action", (gpointer)action);
    g_signal_connect (btn, "clicked", G_CALLBACK (on_popover_action_clicked),
                      popover);
    return btn;
}

/* The popover is built fresh on each right-click and parented to
 * the column view. The "closed" signal handler unparents + drops
 * it so we don't leak. */
static void
on_context_popover_closed (GtkPopover *popover, gpointer user_data)
{
    (void)user_data;
    gtk_widget_unparent (GTK_WIDGET (popover));
}

static void
on_section_secondary_press (GtkGestureClick *gesture, int n_press, double x,
                            double y, gpointer data)
{
    GtkWidget *cv_widget
        = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
    struct tracker_section *sec
        = g_object_get_data (G_OBJECT (cv_widget), "tracker-section");
    GtkWidget *picked, *walker;
    GtkListItem *item = NULL;
    guint pos;
    GtkWidget *popover, *vbox, *btn;
    GdkRectangle rect;
    (void)n_press;
    (void)data;

    if (!sec) {
        return;
    }

    /* Find which row was right-clicked. gtk_widget_pick returns the
     * deepest pickable descendant under (x,y); we walk up the parent
     * chain until we find a cell that has the GtkListItem stash from
     * a bind callback. If we never find one (right-click was on
     * column headers or the gap below the rows), bail. */
    picked = gtk_widget_pick (cv_widget, x, y, GTK_PICK_DEFAULT);
    for (walker = picked; walker && walker != cv_widget;
         walker = gtk_widget_get_parent (walker)) {
        item = g_object_get_data (G_OBJECT (walker), "tracker-list-item");
        if (item) {
            break;
        }
    }
    if (!item) {
        return;
    }
    pos = gtk_list_item_get_position (item);
    if (pos == GTK_INVALID_LIST_POSITION) {
        return;
    }

    /* Select the row first so the action handlers (Connect, Save
     * Bookmark, Get Info) all act on it via selected_section /
     * selected_row. notify::selected fires synchronously here, which
     * is what we want — the global state is up to date by the time
     * the user clicks an item in the menu. */
    gtk_single_selection_set_selected (
        GTK_SINGLE_SELECTION (sec->selection), pos);

    /* Build a fresh popover. Same pattern users.c uses (hand-built
     * GtkBox of flat buttons instead of GtkPopoverMenu) since
     * GtkPopoverMenu had the well-known hover-routing + max-height
     * regressions documented in users.c. */
    popover = gtk_popover_new ();
    gtk_popover_set_has_arrow (GTK_POPOVER (popover), FALSE);
    gtk_widget_set_halign (popover, GTK_ALIGN_START);
    rect.x = (int)x;
    rect.y = (int)y;
    rect.width = 1;
    rect.height = 1;
    gtk_widget_set_parent (popover, cv_widget);
    gtk_popover_set_pointing_to (GTK_POPOVER (popover), &rect);
    g_signal_connect (popover, "closed",
                      G_CALLBACK (on_context_popover_closed), NULL);

    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start (vbox, 4);
    gtk_widget_set_margin_end (vbox, 4);
    gtk_widget_set_margin_top (vbox, 4);
    gtk_widget_set_margin_bottom (vbox, 4);
    gtk_popover_set_child (GTK_POPOVER (popover), vbox);

    /* The three actions reuse the same handlers the headerbar
     * buttons fire; they all read selected_section + selected_row
     * which we just refreshed via set_selected above. Order is
     * Connect (most common) → Save Bookmark → Get Info. */
    btn = build_menu_button (_ ("Connect"), tracker_connect, popover);
    gtk_box_append (GTK_BOX (vbox), btn);
    btn = build_menu_button (_ ("Save Bookmark"), tracker_save_bookmark,
                             popover);
    gtk_box_append (GTK_BOX (vbox), btn);
    btn = build_menu_button (_ ("Get Info"), tracker_details, popover);
    gtk_box_append (GTK_BOX (vbox), btn);

    gtk_popover_popup (GTK_POPOVER (popover));
}

static void
tracker_connect (void)
{
    HxTrackerRow *row;
    guint16 port;
    gboolean tls, secure;
    unsigned char cipher_byte;
    char addrbuf[256];
    char portbuf[16];

    if (!selected_section || selected_row < 0) {
        return;
    }
    row = tracker_section_get_row (selected_section, selected_row);
    if (!row) {
        return;
    }

    create_connect_window (0, &the_session);

    /* Phase E: hand the connect-dialog the address string verbatim
     * (IPv4 dotted-quad, IPv6 colon-hex, or hostname). The dialog
     * just stuffs it into the host entry where the user can
     * tweak it before connecting. The port + security fields are
     * the v3-aware picks: TLS on tls_port when supports_tls, else
     * HOPE with the best advertised cipher, else plaintext. Pre-
     * populating these on the dialog matches what the double-
     * click-to-connect path does, so the user sees the same
     * setting either way and only has to confirm. */
    tracker_row_pick_security (row, &port, &tls, &secure, &cipher_byte);
    g_strlcpy (addrbuf, hx_tracker_row_get_address (row), sizeof (addrbuf));
    g_snprintf (portbuf, sizeof (portbuf), "%u", (unsigned)port);
    /* set_the_entries takes char* (not const char*) by convention;
     * the buffers we just filled are stack-local so the cast is
     * safe. compress=0 — HOPE compression negotiates at connect
     * time, not driven by the dropdown. */
    set_the_entries (addrbuf, (char *)"", (char *)"", portbuf,
                     secure ? 1 : 0, 0, (char)cipher_byte,
                     tls ? 1 : 0);
    g_object_unref (row);
}

/* Headerbar "Save bookmark" handler. Saves a bookmark for the
 * currently-selected row; toast surfaces success / failure. */
static void
tracker_save_bookmark (void)
{
    HxTrackerRow *row;

    if (!selected_section || selected_row < 0) {
        return;
    }
    row = tracker_section_get_row (selected_section, selected_row);
    if (!row) {
        return;
    }
    tracker_save_bookmark_for_row (row);
    g_object_unref (row);
}

/* ---------------------------------------------------------------- */
/* Server-details dialog                                            */
/*                                                                  */
/* Read-only AdwDialog that lays out the per-row v3 TLV snapshot in */
/* AdwPreferencesGroups. Each TLV maps to one AdwActionRow with the */
/* TLV name as title and the decoded value as subtitle. Rows for    */
/* absent TLVs are omitted entirely, and groups with zero rows are  */
/* dropped — a v1 record therefore renders the "Server" group only  */
/* (name / address / users / desc), which is genuinely all the      */
/* tracker told us about it.                                        */
/*                                                                  */
/* Triggered from the headerbar "Details" button, which uses the    */
/* last-selected row (tracker_storow, kept current via the          */
/* select_row signal wired in create_tracker_window).               */
/* ---------------------------------------------------------------- */

/* Add an AdwActionRow with the given title + subtitle text to the
 * group. Skips silently and returns FALSE when the value is NULL or
 * empty, so call sites can chain `n += add_str_row(...)` to count
 * how many rows actually got added (used to drop empty groups).
 */
static gboolean
details_add_str_row (AdwPreferencesGroup *grp, const char *title,
                     const char *value)
{
    GtkWidget *row;

    if (!value || !*value) {
        return FALSE;
    }
    row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), value);
    /* Subtitle-selectable so the user can copy fingerprint-ish
     * values (URLs, country codes) out of the dialog. */
    adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), TRUE);
    adw_preferences_group_add (grp, row);
    return TRUE;
}

/* Same as details_add_str_row but with a numeric value rendered as
 * "%u". A value of 0 is treated as "absent" and skipped — that's the
 * convention for the count-like TLVs (news_count, msgboard_count,
 * etc.) where 0 and "TLV missing" are indistinguishable on the wire.
 */
static gboolean
details_add_uint_row (AdwPreferencesGroup *grp, const char *title, guint val)
{
    char buf[32];

    if (val == 0) {
        return FALSE;
    }
    g_snprintf (buf, sizeof (buf), "%u", val);
    return details_add_str_row (grp, title, buf);
}

/* Bool row — emits only when val is TRUE, with subtitle "Yes". The
 * "No" case is implicit (TLV absent / FALSE → omit the row entirely)
 * because the dialog is meant to read like a capability list, not a
 * checklist. */
static gboolean
details_add_bool_row (AdwPreferencesGroup *grp, const char *title, gboolean val)
{
    if (!val) {
        return FALSE;
    }
    return details_add_str_row (grp, title, _ ("Yes"));
}

/* Human-readable uptime: "Xd Yh Zm". Zero is treated as absent. */
static gboolean
details_add_uptime_row (AdwPreferencesGroup *grp, const char *title,
                        guint32 secs)
{
    char buf[64];

    if (secs == 0) {
        return FALSE;
    }
    guint days = secs / 86400;
    guint hours = (secs / 3600) % 24;
    guint mins = (secs / 60) % 60;
    if (days) {
        g_snprintf (buf, sizeof (buf), "%ud %uh %um", days, hours, mins);
    } else if (hours) {
        g_snprintf (buf, sizeof (buf), "%uh %um", hours, mins);
    } else if (mins) {
        g_snprintf (buf, sizeof (buf), "%um", mins);
    } else {
        g_snprintf (buf, sizeof (buf), "%us", (unsigned) secs);
    }
    return details_add_str_row (grp, title, buf);
}

/* Unix timestamp → "YYYY-MM-DD HH:MM UTC". Zero is treated as
 * "never / not set", which matches the spec's convention for
 * last_news_timestamp / last_chat_timestamp. */
static gboolean
details_add_timestamp_row (AdwPreferencesGroup *grp, const char *title,
                           guint32 unix_ts)
{
    GDateTime *dt;
    gchar *s;
    gboolean ret;

    if (unix_ts == 0) {
        return FALSE;
    }
    dt = g_date_time_new_from_unix_utc ((gint64)unix_ts);
    if (!dt) {
        return FALSE;
    }
    s = g_date_time_format (dt, "%Y-%m-%d %H:%M UTC");
    g_date_time_unref (dt);
    if (!s) {
        return FALSE;
    }
    ret = details_add_str_row (grp, title, s);
    g_free (s);
    return ret;
}

/* Byte count → "1.2 MB" via g_format_size. Zero is "absent". */
static gboolean
details_add_byte_size_row (AdwPreferencesGroup *grp, const char *title,
                           guint64 bytes)
{
    gchar *s;
    gboolean ret;

    if (bytes == 0) {
        return FALSE;
    }
    s = g_format_size (bytes);
    if (!s) {
        return FALSE;
    }
    ret = details_add_str_row (grp, title, s);
    g_free (s);
    return ret;
}

static const char *
maturity_label (HxTrackerV3Maturity m)
{
    switch (m) {
    case HX_TRACKER_V3_MATURITY_TEEN:
        return _ ("Teen");
    case HX_TRACKER_V3_MATURITY_MATURE:
        return _ ("Mature");
    case HX_TRACKER_V3_MATURITY_ADULT:
        return _ ("Adult");
    case HX_TRACKER_V3_MATURITY_GENERAL:
    default:
        return _ ("General");
    }
}

static const char *
category_label (HxTrackerV3Category c)
{
    switch (c) {
    case HX_TRACKER_V3_CATEGORY_GENERAL:
        return _ ("General");
    case HX_TRACKER_V3_CATEGORY_DEVELOPMENT:
        return _ ("Development");
    case HX_TRACKER_V3_CATEGORY_ARCHIVE:
        return _ ("Archive");
    case HX_TRACKER_V3_CATEGORY_WAREZ:
        return _ ("Warez");
    case HX_TRACKER_V3_CATEGORY_GAMING:
        return _ ("Gaming");
    case HX_TRACKER_V3_CATEGORY_MEDIA:
        return _ ("Media");
    case HX_TRACKER_V3_CATEGORY_EDUCATION:
        return _ ("Education");
    case HX_TRACKER_V3_CATEGORY_RESEARCH:
        return _ ("Research");
    case HX_TRACKER_V3_CATEGORY_FILE_SHARING:
        return _ ("File Sharing");
    case HX_TRACKER_V3_CATEGORY_SOCIAL:
        return _ ("Social");
    case HX_TRACKER_V3_CATEGORY_SECURITY:
        return _ ("Security");
    case HX_TRACKER_V3_CATEGORY_CREATIVE:
        return _ ("Creative");
    case HX_TRACKER_V3_CATEGORY_UNSPECIFIED:
    default:
        return _ ("Unspecified");
    }
}

/* If `count > 0`, append the group to `content` and return; otherwise
 * sink + drop the floating reference so it doesn't leak. Empty groups
 * get suppressed so the dialog reads tightly — e.g. a v1 record only
 * renders the Server group. */
static void
details_finish_group (GtkWidget *content, AdwPreferencesGroup *grp, int count)
{
    if (count > 0) {
        gtk_box_append (GTK_BOX (content), GTK_WIDGET (grp));
    } else {
        /* AdwPreferencesGroup is a GInitiallyUnowned — sink + unref
         * to drop it cleanly without appending. */
        g_object_ref_sink (grp);
        g_object_unref (grp);
    }
}

static void
show_server_details (HxTrackerRow *server)
{
    AdwDialog *dlg;
    GtkWidget *toolbar_view, *header, *content, *clamp, *scrolled;
    AdwPreferencesGroup *grp;
    HxTrackerV3Meta *m;
    const char *name;
    const char *address;
    const char *desc;
    guint16 port;
    guint16 nusers;
    char title_buf[256];
    char addrport[256];
    char nbuf[32];
    int n;

    if (!server) {
        return;
    }

    name = hx_tracker_row_get_name (server);
    address = hx_tracker_row_get_address (server);
    desc = hx_tracker_row_get_desc (server);
    port = hx_tracker_row_get_port (server);
    nusers = hx_tracker_row_get_nusers (server);

    dlg = ADW_DIALOG (adw_dialog_new ());
    g_snprintf (title_buf, sizeof (title_buf), _ ("Server details — %s"),
                (name && *name) ? name : (address ? address : "?"));
    adw_dialog_set_title (dlg, title_buf);
    adw_dialog_set_content_width (dlg, 520);
    adw_dialog_set_content_height (dlg, 640);

    gtkhx_dialog_add_close_shortcuts (GTK_WIDGET (dlg));

    header = adw_header_bar_new ();
    toolbar_view = adw_toolbar_view_new ();
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

    content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_top (content, 18);
    gtk_widget_set_margin_bottom (content, 18);
    gtk_widget_set_margin_start (content, 18);
    gtk_widget_set_margin_end (content, 18);

    m = hx_tracker_row_get_meta (server);

    /* ------------------ Server (always present) ------------------ */
    grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (grp, _ ("Server"));
    n = 0;
    if (name && *name) {
        n += details_add_str_row (grp, _ ("Name"), name);
    }
    if (address && *address) {
        /* Bracket the address when it contains a colon so an IPv6
         * literal (either from an IPV6-type wire record or from a
         * HOSTNAME-type record carrying an IPv6 literal, which is
         * what Argus emits for promoted servers) doesn't render as
         * `2001:db8::1:5500` and leave the reader guessing where the
         * port starts. RFC 3986 §3.2.2 bracket form. IPv4 and bare
         * hostnames have no colons, so they pass through unchanged. */
        if (strchr (address, ':') != NULL) {
            g_snprintf (addrport, sizeof (addrport), "[%s]:%u", address, port);
        } else {
            g_snprintf (addrport, sizeof (addrport), "%s:%u", address, port);
        }
        n += details_add_str_row (grp, _ ("Address"), addrport);
    }
    if (desc && *desc) {
        n += details_add_str_row (grp, _ ("Description"), desc);
    }
    g_snprintf (nbuf, sizeof (nbuf), "%u", nusers);
    n += details_add_str_row (grp, _ ("Users online"), nbuf);
    if (m && m->has_max_users) {
        g_snprintf (nbuf, sizeof (nbuf), "%u", m->max_users);
        n += details_add_str_row (grp, _ ("Maximum users"), nbuf);
    }
    details_finish_group (content, grp, n);

    if (m) {
        /* ------------------------- Identity ------------------------- */
        grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
        adw_preferences_group_set_title (grp, _ ("Identity"));
        n = 0;
        n += details_add_str_row (grp, _ ("Software"), m->server_software);
        n += details_add_str_row (grp, _ ("Country"), m->country_code);
        n += details_add_str_row (grp, _ ("Region"), m->region);
        n += details_add_str_row (grp, _ ("Language"), m->language);
        n += details_add_str_row (grp, _ ("Tags"), m->tags);
        if (m->maturity != HX_TRACKER_V3_MATURITY_GENERAL) {
            n += details_add_str_row (grp, _ ("Maturity"),
                                      maturity_label (m->maturity));
        }
        if (m->listing_category != HX_TRACKER_V3_CATEGORY_UNSPECIFIED) {
            n += details_add_str_row (grp, _ ("Category"),
                                      category_label (m->listing_category));
        }
        n += details_add_str_row (grp, _ ("Contact"), m->contact_url);
        n += details_add_str_row (grp, _ ("Rules"), m->rules_url);
        n += details_add_str_row (grp, _ ("Banner URL"), m->banner_url);
        n += details_add_str_row (grp, _ ("Icon URL"), m->icon_url);
        if (m->has_timezone_offset) {
            char tzbuf[32];
            int mins = m->timezone_offset_min;
            int sign = (mins < 0) ? -1 : 1;
            int absm = mins * sign;
            g_snprintf (tzbuf, sizeof (tzbuf), "UTC%c%02d:%02d",
                        sign < 0 ? '-' : '+', absm / 60, absm % 60);
            n += details_add_str_row (grp, _ ("Timezone"), tzbuf);
        }
        n += details_add_timestamp_row (grp, _ ("Server launched"),
                                        m->server_launched);
        n += details_add_uptime_row (grp, _ ("Uptime"), m->uptime_secs);
        details_finish_group (content, grp, n);

        /* ----------------------- Capabilities ----------------------- */
        grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
        adw_preferences_group_set_title (grp, _ ("Capabilities"));
        n = 0;
        if (m->protocol_version) {
            char pvbuf[32];
            g_snprintf (pvbuf, sizeof (pvbuf), "0x%04x (%u)",
                        m->protocol_version, m->protocol_version);
            n += details_add_str_row (grp, _ ("Protocol version"), pvbuf);
        }
        if (m->min_proto_version) {
            char pvbuf[32];
            g_snprintf (pvbuf, sizeof (pvbuf), "0x%04x (%u)",
                        m->min_proto_version, m->min_proto_version);
            n += details_add_str_row (grp, _ ("Minimum client protocol"),
                                      pvbuf);
        }
        n += details_add_bool_row (grp, _ ("HOPE encryption"),
                                   m->supports_hope);
        n += details_add_str_row (grp, _ ("HOPE ciphers"), m->hope_ciphers);
        n += details_add_bool_row (grp, _ ("TLS"), m->supports_tls);
        if (m->supports_tls && m->tls_port) {
            char tpbuf[16];
            g_snprintf (tpbuf, sizeof (tpbuf), "%u", m->tls_port);
            n += details_add_str_row (grp, _ ("TLS port"), tpbuf);
        }
        n += details_add_bool_row (grp, _ ("IPv6"), m->supports_ipv6);
        n += details_add_bool_row (grp, _ ("Inline media"),
                                   m->supports_inline_media);
        n += details_add_bool_row (grp, _ ("Voice"), m->supports_voice);
        n += details_add_bool_row (grp, _ ("Large files"),
                                   m->supports_large_files);
        n += details_add_uint_row (grp, _ ("Link down (Mbit/s)"),
                                   m->link_down_mbit);
        n += details_add_uint_row (grp, _ ("Link up (Mbit/s)"),
                                   m->link_up_mbit);
        details_finish_group (content, grp, n);

        /* --------------------- Activity / Content --------------------- */
        grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
        adw_preferences_group_set_title (grp, _ ("Activity"));
        n = 0;
        n += details_add_uint_row (grp, _ ("Peak users (24h)"), m->peak_24h);
        n += details_add_uint_row (grp, _ ("Average users (24h)"), m->avg_24h);
        n += details_add_uint_row (grp, _ ("News posts"), m->news_count);
        n += details_add_uint_row (grp, _ ("Message-board posts"),
                                   m->msgboard_count);
        n += details_add_uint_row (grp, _ ("Files"), m->files_count);
        n += details_add_byte_size_row (grp, _ ("Total file size"),
                                        (guint64)m->total_file_size);
        n += details_add_timestamp_row (grp, _ ("Last news activity"),
                                        m->last_news_timestamp);
        n += details_add_timestamp_row (grp, _ ("Last public chat"),
                                        m->last_chat_timestamp);
        details_finish_group (content, grp, n);

        /* --------------- Tracker / Privacy ---------------- */
        grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
        adw_preferences_group_set_title (grp, _ ("Tracker"));
        n = 0;
        n += details_add_bool_row (grp, _ ("Promoted"), m->is_promoted);
        n += details_add_bool_row (grp, _ ("Verified online"),
                                   m->verified_online);
        n += details_add_bool_row (grp, _ ("Private listing"),
                                   m->private_listing);
        n += details_add_bool_row (grp, _ ("Language strict"),
                                   m->language_strict);
        n += details_add_timestamp_row (grp, _ ("First seen"), m->first_seen);
        n += details_add_timestamp_row (grp, _ ("Last heartbeat"),
                                        m->last_heartbeat);
        details_finish_group (content, grp, n);
    }

    clamp = adw_clamp_new ();
    adw_clamp_set_child (ADW_CLAMP (clamp), content);
    scrolled = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), clamp);
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), scrolled);
    adw_dialog_set_child (dlg, toolbar_view);

    adw_dialog_present (ADW_DIALOG (dlg),
                        tracker_window ? GTK_WIDGET (tracker_window) : NULL);
}

/* Headerbar "Details" button handler. Uses the last-selected
 * row, kept current by the GtkSingleSelection notify::selected
 * handler in on_section_selected_changed. If no row has been
 * selected yet, fall through silently rather than opening an
 * empty dialog. */
static void
tracker_details (void)
{
    HxTrackerRow *row;

    if (!selected_section || selected_row < 0) {
        return;
    }
    row = tracker_section_get_row (selected_section, selected_row);
    if (row) {
        show_server_details (row);
        g_object_unref (row);
    }
}

void
create_tracker_window (GtkWidget *widget, gpointer data)
{
    GtkWidget *vbox;
    GtkWidget *header;
    GtkWidget *searchhbox;
    GtkWidget *searchentry;
    GtkWidget *sections_scroll;
    GtkWidget *refreshbtn;
    GtkWidget *connbtn;
    GtkWidget *bookmarkbtn;
    GtkWidget *detailsbtn;
    GtkWidget *count_box;
    session *sess = data;

    if (tracker_window) {
        return;
    }

    tracker_window = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (tracker_window), _ ("Tracker"));
    gtk_window_set_default_size (GTK_WINDOW (tracker_window), 860, 500);
    g_signal_connect (tracker_window, "close-request",
                      G_CALLBACK (close_tracker_window), 0);

    /* Register the window with the GtkApplication so dialogs
     * opened while it has focus (the Connect dialog from our
     * Connect button, in particular) parent themselves to the
     * tracker rather than to whichever window happened to be
     * application-active before. gtkhx_active_window() reads
     * gtk_application_get_active_window which only tracks
     * windows that have been added to the application. */
    {
        GApplication *app = gtkhx_get_application ();
        if (app && GTK_IS_APPLICATION (app)) {
            gtk_application_add_window (GTK_APPLICATION (app),
                                        GTK_WINDOW (tracker_window));
        }
    }

    /* Shared filter: one GtkCustomFilter the whole window's
     * sections share. tracker_rerun_search notifies it via
     * gtk_filter_changed when the regex compiles to a new value;
     * every section's GtkFilterListModel re-evaluates in a single
     * pass. The match callback reads `current_search` directly,
     * so we don't need to hand a user_data pointer through. */
    tracker_filter = gtk_custom_filter_new (tracker_row_matches, NULL, NULL);

    /* Per-section sink. Created here, populated by tracker_batch_begin
     * as each tracker comes online. The value-destroy hook tears each
     * section down on hash removal. */
    tracker_sections = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                              tracker_section_free_hash);
    tracker_sections_order = NULL;
    current_section = NULL;
    selected_section = NULL;
    selected_row = -1;

    /* GtkSearchEntry replaces the legacy "Search:" label +
	 * GtkEntry pair. SearchEntry has its own search-glass icon and
	 * a clear-button when text is present, so the label becomes
	 * redundant. The placeholder text takes the label's job.
	 *
	 * "search-changed" fires with a small built-in debounce as the user
	 * types — that's the signal that drives live filtering. "activate"
	 * (Enter) is wired too as a no-op shortcut so the user's muscle
	 * memory of "type query, hit Enter" still works; with live filtering
	 * the list is already up to date by the time Enter lands. */
    searchentry = gtk_search_entry_new ();
    tracker_search_entry = searchentry;
    gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (searchentry),
                                           _ ("Search trackers"));
    g_signal_connect (searchentry, "search-changed",
                      G_CALLBACK (tracker_search), 0);
    g_signal_connect (searchentry, "activate", G_CALLBACK (tracker_search), 0);

    /* Case-sensitive toggle, tucked next to the search field so the
	 * user doesn't have to dig into Settings to flip it. State is
	 * mirrored to the gtkhx_prefs.track_case cfgvar (and from there to
	 * gtkhxrc) via gtkhx_prefs_set_bool, which also keeps the
	 * Settings page in lockstep when both happen to be open. */
    tracker_case_btn = gtk_toggle_button_new_with_label ("Aa");
    gtk_widget_set_tooltip_text (tracker_case_btn,
                                 _ ("Match case in tracker search"));
    gtk_widget_add_css_class (tracker_case_btn, "flat");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (tracker_case_btn),
                                  gtkhx_prefs.track_case ? TRUE : FALSE);
    g_signal_connect (tracker_case_btn, "toggled",
                      G_CALLBACK (tracker_case_toggled), NULL);

    num_found_total = 0;
    num_total_total = 0;
    lbl_found = gtk_label_new ("0");
    lbl_total = gtk_label_new (" / 0");
    gtk_widget_add_css_class (lbl_found, "dim-label");
    gtk_widget_add_css_class (lbl_total, "dim-label");

    /* action buttons live in the AdwHeaderBar now, not in a
	 * row beneath. Refresh + Connect on the leading edge, the live
	 * found-count "N / M" indicator on the trailing edge. */
    refreshbtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/refresh.png",
                                      _ ("Refresh tracker list"), 2,
                                      G_CALLBACK (tracker_getlist), sess);
    connbtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/connect.png",
                                   _ ("Connect to selected server"), 2,
                                   G_CALLBACK (tracker_connect), 0);
    /* Save Bookmark — record the selected server in the user's
     * bookmarks dir. GNOME placeholder icon (the same one Files
     * uses for "Bookmark this location") until we pick a bespoke
     * pixmap; symbolic so it themes correctly. */
    bookmarkbtn = gtk_button_new_from_icon_name ("bookmark-new-symbolic");
    gtk_widget_set_tooltip_text (
        bookmarkbtn, _ ("Save selected server as a bookmark"));
    g_signal_connect (bookmarkbtn, "clicked",
                      G_CALLBACK (tracker_save_bookmark), NULL);

    /* Phase B: opens the v3 metadata popover for whichever row is
     * currently highlighted. Uses the same info pixmap the chat /
     * users / files-browser headerbars use so the iconography stays
     * consistent across windows. */
    detailsbtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/info.png",
                                      _ ("Show details for selected server"),
                                      2, G_CALLBACK (tracker_details), NULL);

    header = adw_header_bar_new ();
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), refreshbtn);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), connbtn);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), bookmarkbtn);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), detailsbtn);

    count_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append (GTK_BOX (count_box), lbl_found);
    gtk_box_append (GTK_BOX (count_box), lbl_total);
    adw_header_bar_pack_end (ADW_HEADER_BAR (header), count_box);

    gtk_window_set_titlebar (GTK_WINDOW (tracker_window), header);

    /* Per-tracker sections stack inside a scrolled vbox. Each section
     * is a GtkExpander with its own scrolled GtkColumnView inside;
     * the column view sizes to its content (header + every row), the
     * inner scroller handles only horizontal column overflow, and
     * this outer scroller handles all the vertical scrolling for the
     * stack as a whole. 8 px gutter between sections. */
    tracker_sections_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    sections_scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sections_scroll),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (sections_scroll),
                                       FALSE);
    gtk_widget_set_vexpand (sections_scroll, TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sections_scroll),
                                   tracker_sections_box);

    /* Actions live in the headerbar, content vbox
	 * just holds the search row + the sections stack. 8 px outer
	 * margin so content doesn't touch the window frame; 8 px gutter
	 * between the search field and the sections area. */
    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start (vbox, 8);
    gtk_widget_set_margin_end (vbox, 8);
    gtk_widget_set_margin_top (vbox, 8);
    gtk_widget_set_margin_bottom (vbox, 8);

    searchhbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtkhx_box_pack (searchhbox, searchentry, 1, 1, 0);
    gtkhx_box_pack (searchhbox, tracker_case_btn, 0, 0, 0);
    gtkhx_box_pack (vbox, searchhbox, 0, 0, 0);

    gtkhx_box_pack (vbox, sections_scroll, 1, 1, 0);
    gtkhx_widget_set_child (tracker_window, vbox);
    init_keyaccel (tracker_window);
    gtk_window_present (GTK_WINDOW (tracker_window));

    gtk_widget_grab_focus (searchentry);

    /* current_search is left NULL → match-all semantics, which is
     * what the empty search entry implies. Results streamed in from
     * the tracker worker via tracker_server_create pass through
     * tracker_search_tree(NULL, ...) and are unconditionally
     * accepted until the user types into the search field. */
    current_search = NULL;

    tracker_getlist (0, sess);
}
