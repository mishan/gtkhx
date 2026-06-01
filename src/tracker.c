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
#include "gtk_hlist.h"
#include "chat.h"
#include "options.h"
#include "cfgkeys.h"
#include "debug.h"
#include "hotline.h"
#include "tracker.h"

static GtkWidget *tracker_window;
static GtkWidget *tracker_sections_box; /* vbox of per-tracker GtkExpanders */
static GtkWidget *tracker_search_entry;
static GtkWidget *tracker_case_btn;
static GtkWidget *lbl_found, *lbl_total;
static int num_found_total, num_total_total;

/* Search filter for the tracker list.
 *
 * Phase 5+: was a hand-rolled `struct dfa *` from a vendored 2500-line
 * GNU regex; now a GLib GRegex (PCRE2). Two states matter:
 *   - current_search == NULL  → empty pattern, match-all (the common
 *                                case when the search entry is empty).
 *   - current_search != NULL  → compiled regex; g_regex_match decides
 *                                each row.
 * Invalid patterns (parse error during compile) also fall to match-all
 * with a one-line warning to the chat output. */
static GRegex *current_search;

/* Column indices for the per-section GtkHList. Defined once so the
 * titles array, width table, and column-visibility calls don't drift. */
enum {
    COL_NAME = 0,
    COL_USERS,
    COL_COUNTRY,
    COL_ADDRESS,
    COL_PORT,
    COL_CAPS,
    COL_DESC,
    N_TRACKER_COLS,
};

/* Per-tracker-listing row.
 *
 * `address` is the printable address from HxTrackerServer.address —
 * inet_ntop("192.0.2.10") for v1 + v3-IPv4 records, inet_ntop6 for
 * v3 IPv6 records, or the literal hostname for v3 0x48 records.
 * Always non-NULL, UTF-8 valid. Used as the dedup key (paired with
 * port) and rendered into the "Address" column directly.
 *
 * Phase E (claude/tracker-v3-phase-a tail end): switched the dedup
 * key from `struct in_addr addr` (32-bit IPv4) to a string so v3's
 * IPv6 + hostname records can flow through the same BST without
 * needing a separate non-IPv4 path. The compare in find_server /
 * insert_server is `strcmp(a, b)` then port; ordering is
 * lexicographic on the address string, which is arbitrary-but-stable
 * (and the user only sees it through tracker_search_tree's render
 * loop, so the order isn't user-facing). */
struct tracker_server {
    char *name;
    char *desc;

    char *address;

    guint16 nusers;
    guint16 port;

    /* Deep-copied snapshot of the v3 TLV metadata at insertion
     * time. Always non-NULL — Phase B's hx_tracker_server_new_v1
     * allocates a zero-init meta for legacy records so callers
     * don't have to NULL-check. NULL pointers inside meta (strings)
     * mean "TLV absent"; that's the meta module's contract. */
    HxTrackerV3Meta *meta;

    struct tracker_server *left, *right;
};

/* One section per tracker URL. Created on tracker_batch_begin,
 * populated by tracker_server_create calls that follow. The visible
 * UI is a GtkExpander whose child is a scrolled GtkHList; section
 * lifetime ties to the expander widget's lifetime (the box owns
 * the expander, we own the section data). Sections survive across
 * search filter changes (search just hides rows) and across a
 * refresh only by URL-keyed recycling — tracker_clear blanks
 * everything per refresh. */
struct tracker_section {
    char *url;        /* hash key — owned */
    guint8 version;   /* 1 or 3, from batch-begin */
    guint16 expected; /* batch-begin's announced count — used in subtitle */

    struct tracker_server *tree;
    int num_total; /* records inserted into this section's tree */
    int num_found; /* records matching the current filter in this section */

    GtkWidget *expander; /* GtkExpander, child of tracker_sections_box */
    GtkWidget *list;     /* GtkHList inside the expander */
    GtkWidget *scroll;   /* GtkScrolledWindow wrapping the list */
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
 * select_row handler; consumed by Details / Connect buttons in the
 * headerbar. NULL means "nothing selected anywhere." */
static struct tracker_section *selected_section;
static int selected_row;

/* Recursive BST destructor — frees each node's strings + meta then
 * the node itself. NOT responsible for the section it lives in; the
 * caller resets section->tree to NULL after this returns. Doesn't
 * touch any global state — multiple section trees can be torn down
 * independently. */
static void
tracker_tree_destroy (struct tracker_server *root)
{
    if (!root) {
        return;
    }

    tracker_tree_destroy (root->left);
    tracker_tree_destroy (root->right);

    g_free (root->name);
    g_free (root->desc);
    g_free (root->address);
    hx_tracker_v3_meta_free (root->meta);
    g_free (root);
}

/* Free a section: destroy the BST, detach the expander from the
 * sections box, drop the URL key. The widget tear-down chain
 * (gtk_box_remove → expander unrefs scrolled-window → unrefs
 * GtkHList) cleans up the GTK side. */
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
    tracker_tree_destroy (sec->tree);
    sec->tree = NULL;
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

/* Phase 4.5: GTK 4 close-request on (GtkWindow *, gpointer). */
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

/* Phase 5+ (async tracker rewrite): the pthread_t / SIGUSR1 /
 * pthread_kill / pthread_join dance that used to live here is gone.
 * The tracker fetch now runs on the main loop via GSocketClient +
 * GInputStream async; cancellation is a g_cancellable_cancel against
 * a GCancellable held inside network.c's tracker_run_ctx.
 * tracker_kill_threads() in network.c trips that cancellation; the
 * in-flight async callback unwinds cleanly.
 *
 * Net effect: no thread to spawn, no signal handler, no joinable
 * handle to manage, and the UAF that the old (Phase 5) code spent
 * a paragraph defending against (track_tid pointing at freed
 * libpthread state) is structurally impossible. */

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

/* Render every server in `root` that matches `preg` into `sec`'s
 * GtkHList, updating sec->num_found. Walked in-order (left → root
 * → right) across the BST; the resulting visual order is
 * lexicographic by address, which is arbitrary-but-stable. Caller
 * is responsible for freezing the list and clearing it before the
 * first call.
 *
 * Note: tracker_server_create calls this with the just-inserted
 * server as root (left/right == NULL), so for that path the
 * traversal degenerates to "render this one row." The traversal
 * order only matters for tracker_rerun_search which walks the
 * whole tree. */
static void
tracker_search_tree (GRegex *preg, struct tracker_section *sec,
                     struct tracker_server *root)
{
    int row;
    char nusersstr[8], portstr[8], capsbuf[48], *text[N_TRACKER_COLS];
    gboolean flag;

    if (!root || !sec) {
        return;
    }

    tracker_search_tree (preg, sec, root->left);

    /* preg == NULL means "match everything" — the common case when the
     * search entry is empty. Otherwise both name and desc are tried.
     * GRegex copes with embedded NULs because we pass the buffers as
     * NUL-terminated C strings via g_regex_match. */
    if (!preg) {
        flag = TRUE;
    } else {
        flag = g_regex_match (preg, root->name, 0, NULL)
               || g_regex_match (preg, root->desc, 0, NULL);
    }
    if (flag) {
        snprintf (nusersstr, sizeof (nusersstr), "%u", root->nusers);
        snprintf (portstr, sizeof (portstr), "%u", root->port);
        format_caps_badges (root->meta, capsbuf, sizeof (capsbuf));

        text[COL_NAME] = root->name;
        text[COL_USERS] = nusersstr;
        /* country_code is a 2-letter ISO 3166-1 alpha-2 code when
         * the tracker advertised it; absent → empty cell. v1 records
         * always land here as empty since their zero-init meta has
         * country_code == NULL. The whole column is hidden on v1
         * sections so this just defends against v3 records that
         * didn't carry the TLV. */
        text[COL_COUNTRY] = (root->meta && root->meta->country_code)
                                ? root->meta->country_code
                                : (char *)"";
        /* Phase E: render whatever printable address the boxed
         * event delivered (IPv4 / IPv6 / hostname). No inet_ntop
         * call here — the event constructor already did that for
         * the IPv4 / IPv6 records, and the hostname-record path
         * (Argus's promoted_servers format) carries the literal
         * hostname through. */
        text[COL_ADDRESS] = root->address ? root->address : (char *)"";
        text[COL_PORT] = portstr;
        text[COL_CAPS] = capsbuf;
        text[COL_DESC] = root->desc;

        row = gtk_hlist_append (GTK_HLIST (sec->list), text);
        /* Phase 5: no per-row foreground override — let the GTK theme's
         * default foreground apply so the tracker list reads correctly
         * under both light and dark themes. */
        gtk_hlist_set_row_data (GTK_HLIST (sec->list), row, root);
        sec->num_found++;
    }

    tracker_search_tree (preg, sec, root->right);
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

    if (!sec || !sec->expander) {
        return;
    }
    g_snprintf (vbuf, sizeof (vbuf), "v%u", (unsigned)sec->version);
    url_escaped = g_markup_escape_text (sec->url, -1);

    if (sec->num_total == 0) {
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
    } else if (sec->num_found == sec->num_total) {
        markup = g_strdup_printf (
            "<b>%s</b>  <span alpha=\"60%%\">%s · %d %s</span>", url_escaped,
            vbuf, sec->num_total,
            sec->num_total == 1 ? _ ("server") : _ ("servers"));
    } else {
        markup = g_strdup_printf (
            "<b>%s</b>  <span alpha=\"60%%\">%s · %d / %d %s</span>",
            url_escaped, vbuf, sec->num_found, sec->num_total,
            sec->num_total == 1 ? _ ("server") : _ ("servers"));
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

/* Phase 5: tracker_search runs every time the visible filter needs to
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

    /* Walk every section, blank its list, re-render with the new
     * filter. The aggregate count for the headerbar label sums each
     * section's num_found after the per-section walk settles. */
    num_found_total = 0;
    for (l = tracker_sections_order; l; l = l->next) {
        struct tracker_section *sec = l->data;
        gtk_hlist_clear (GTK_HLIST (sec->list));
        sec->num_found = 0;
        gtk_hlist_freeze (GTK_HLIST (sec->list));
        tracker_search_tree (current_search, sec, sec->tree);
        gtk_hlist_thaw (GTK_HLIST (sec->list));
        tracker_section_update_title (sec);
        num_found_total += sec->num_found;
    }

    /* Selection state's row index may now point past the end of
     * its section's list — clear so Connect / Details can't act
     * on a stale row. The user just needs to click again. */
    selected_section = NULL;
    selected_row = -1;

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

/* Phase E: dedup key is (address-string, port). Compare order is
 * strcmp(address) then numeric port. Both fields are non-NULL by
 * construction — tracker_server_create populates address from
 * event->address (always non-NULL, see tracker_event.c) and port
 * from event->port. Per-section dedup now: each section gets its
 * own BST since the same physical server may legitimately appear
 * on multiple trackers, and the user wants to see who's reporting
 * what. */
static int
find_server (const char *address, guint16 port, struct tracker_server *root)
{
    if (!root || !address) {
        return 0;
    }
    int c = strcmp (root->address, address);
    if (c == 0 && root->port == port) {
        return 1;
    }
    struct tracker_server *next;
    if (c == 0) {
        next = (root->port > port) ? root->left : root->right;
    } else {
        next = (c > 0) ? root->left : root->right;
    }
    return find_server (address, port, next);
}

static void
insert_server_at (struct tracker_server *server, struct tracker_server *root)
{
    int c = strcmp (root->address, server->address);
    if (c == 0) {
        if (root->port > server->port) {
            if (root->left) {
                insert_server_at (server, root->left);
                return;
            }
            root->left = server;
            return;
        }
        if (root->right) {
            insert_server_at (server, root->right);
            return;
        }
        root->right = server;
        return;
    }

    if (c > 0) {
        if (root->left) {
            insert_server_at (server, root->left);
            return;
        }
        root->left = server;
        return;
    }

    if (root->right) {
        insert_server_at (server, root->right);
        return;
    }

    root->right = server;
}

static void
insert_server (struct tracker_server *server, struct tracker_section *sec)
{
    if (!sec->tree) {
        sec->tree = server;
        server->left = NULL;
        server->right = NULL;
        return;
    }
    insert_server_at (server, sec->tree);
}

/* Forward decls — the gesture / selection handlers live below the
 * window builder; tracker_section_new wires them on its newly-built
 * list. */
static void tracker_section_pressed (GtkGestureClick *gesture, int n_press,
                                     double x, double y, gpointer data);
static void tracker_section_row_selected (GtkHList *hlist, gint row,
                                          gint column, gpointer event,
                                          gpointer data);

/* Build the per-section UI: a GtkExpander whose child is a scrolled
 * GtkHList with the standard tracker columns. v1 sections hide the
 * Country / Caps columns since their records can't carry those TLVs.
 * The list pointer is stored as widget data on the list itself so
 * the click + select handlers can recover the owning section. */
static struct tracker_section *
tracker_section_new (const char *url, guint8 version, guint16 expected)
{
    struct tracker_section *sec;
    const char *titles[N_TRACKER_COLS];
    GtkGesture *click;

    sec = g_new0 (struct tracker_section, 1);
    sec->url = g_strdup (url ? url : "");
    sec->version = version;
    sec->expected = expected;

    titles[COL_NAME] = _ ("Name");
    titles[COL_USERS] = _ ("Users");
    titles[COL_COUNTRY] = _ ("Country");
    titles[COL_ADDRESS] = _ ("Address");
    titles[COL_PORT] = _ ("Port");
    titles[COL_CAPS] = _ ("Caps");
    titles[COL_DESC] = _ ("Description");

    sec->list = gtk_hlist_new_with_titles (N_TRACKER_COLS, (gchar **)titles);
    gtk_hlist_set_column_width (GTK_HLIST (sec->list), COL_NAME, 200);
    gtk_hlist_set_column_width (GTK_HLIST (sec->list), COL_USERS, 76);
    gtk_hlist_set_column_justification (GTK_HLIST (sec->list), COL_USERS,
                                        GTK_JUSTIFY_CENTER);
    gtk_hlist_set_column_width (GTK_HLIST (sec->list), COL_COUNTRY, 60);
    gtk_hlist_set_column_justification (GTK_HLIST (sec->list), COL_COUNTRY,
                                        GTK_JUSTIFY_CENTER);
    gtk_hlist_set_column_width (GTK_HLIST (sec->list), COL_ADDRESS, 150);
    gtk_hlist_set_column_width (GTK_HLIST (sec->list), COL_PORT, 70);
    gtk_hlist_set_column_width (GTK_HLIST (sec->list), COL_CAPS, 110);
    gtk_hlist_set_column_width (GTK_HLIST (sec->list), COL_DESC, 280);

    /* v1 sections suppress the columns that can ONLY ever be empty
     * for v1 records. v3 sections keep them visible even when
     * individual records didn't advertise those TLVs (a v3 tracker
     * could carry mixed records, and the column being present is
     * informative on its own). */
    if (version == 1) {
        gtk_hlist_set_column_visible (GTK_HLIST (sec->list), COL_COUNTRY,
                                      FALSE);
        gtk_hlist_set_column_visible (GTK_HLIST (sec->list), COL_CAPS, FALSE);
    }

    /* Stash the section pointer on the list so the click + select
     * handlers can recover their owner without a global lookup. */
    g_object_set_data (G_OBJECT (sec->list), "tracker-section", sec);

    g_signal_connect (sec->list, "select_row",
                      G_CALLBACK (tracker_section_row_selected), NULL);
    click = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click),
                                   GDK_BUTTON_PRIMARY);
    g_signal_connect (click, "pressed", G_CALLBACK (tracker_section_pressed),
                      NULL);
    gtk_widget_add_controller (sec->list, GTK_EVENT_CONTROLLER (click));

    /* GtkTreeView (under the GtkHList shim) doesn't ship its own
     * scrollbars — it relies on a wrapping GtkScrolledWindow for
     * column overflow + header alignment. We still want that wrapper,
     * but VERTICALLY the section should be exactly as tall as its
     * rows (no inner scrollbar, no fixed height): the outer scroller
     * on the sections box does the up/down scrolling for the whole
     * stack, so a section's inner list scrolling would be a second
     * scrollbar fighting the first.
     *
     * Vertical = NEVER: the scrolled window adopts its child's
     *   natural height (sum of header + row heights). Empty
     *   sections collapse to roughly the header height; busy
     *   sections grow until they hit whatever space the outer
     *   scroller is willing to give them (i.e. unbounded — that's
     *   what the outer scroller exists for).
     * Horizontal = AUTOMATIC: when columns overflow the window's
     *   width, the section gets its own horizontal scrollbar.
     *   That's per-section so each tracker's column widths stay
     *   independent — clean across mixed v1 / v3 sections where
     *   the visible columns differ. */
    sec->scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sec->scroll),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (sec->scroll), TRUE);
    /* Without this, GtkScrolledWindow caps its reported natural
     * height at its default (~minimum) — the section would still
     * collapse to a few rows even with policy NEVER. With it
     * enabled the scrolled window passes through the GtkTreeView's
     * natural height (header + every row), so the section ends up
     * exactly as tall as its content. */
    gtk_scrolled_window_set_propagate_natural_height (
        GTK_SCROLLED_WINDOW (sec->scroll), TRUE);

    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sec->scroll),
                                   sec->list);

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
     * be safe). Recycle the existing section. */
    sec = g_hash_table_lookup (tracker_sections, tracker_url);
    if (sec) {
        sec->version = version;
        sec->expected = expected_count;
        tracker_tree_destroy (sec->tree);
        sec->tree = NULL;
        sec->num_total = 0;
        sec->num_found = 0;
        gtk_hlist_clear (GTK_HLIST (sec->list));
        gtk_hlist_set_column_visible (GTK_HLIST (sec->list), COL_COUNTRY,
                                      version != 1);
        gtk_hlist_set_column_visible (GTK_HLIST (sec->list), COL_CAPS,
                                      version != 1);
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
    struct tracker_server *server;
    struct tracker_section *sec;
    char *num;
    int old_num_found;

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
     * invariant doesn't NULL-deref strcmp. */
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
     * grouping. */
    if (find_server (event->address, event->port, sec->tree)) {
        return;
    }

    sec->num_total++;
    num_total_total++;
    server = g_malloc0 (sizeof (struct tracker_server));
    server->address = g_strdup (event->address);
    server->port = event->port;
    server->nusers = event->nusers;
    /* MacRoman → UTF-8 transcoding lives in the boxed-event
     * constructor now (hx_tracker_server_new_v1). v3 records arrive
     * already-UTF-8 per spec, validated with g_utf8_make_valid in
     * the constructor. Either way, by the time we get here the
     * strings are Pango-safe — just g_strdup so the dedup tree
     * owns its own copies and the event can be freed by the
     * signal emitter. */
    server->name = g_strdup (event->name ? event->name : "");
    server->desc = g_strdup (event->desc ? event->desc : "");
    /* Snapshot the typed TLV view so server-tree rows can render
     * metadata columns (country, capabilities) and the details
     * popover without re-walking the raw TLV blob. Deep copy
     * because the signal subscriber's event is freed after the
     * emit. hx_tracker_v3_meta_copy(NULL) returns NULL, but the
     * constructors always populate event->meta (v1 with a
     * zero-init meta, v3 with the parsed one), so this is
     * always non-NULL in practice. */
    server->meta = hx_tracker_v3_meta_copy (event->meta);
    insert_server (server, sec);

    old_num_found = sec->num_found;
    tracker_search_tree (current_search, sec, server);
    if (old_num_found != sec->num_found) {
        num_found_total += (sec->num_found - old_num_found);
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

    if (old_num_found != sec->num_found) {
        num = g_strdup_printf ("  %d", num_found_total);
        gtk_label_set_text (GTK_LABEL (lbl_found), num);
        g_free (num);
    }
}

/* Phase 4.5: button-press-event is gone in GTK 4. Each section's
 * GtkHList carries its own gesture controller — the controller's
 * widget is the GtkHList, and we recover the owning section via
 * the "tracker-section" qdata stashed at section construction.
 *
 * On any press: update selected_section + selected_row to point at
 * the row just clicked (across all sections — this section becomes
 * the new active one for Details / Connect). On double-click:
 * connect immediately. */
static void
tracker_section_pressed (GtkGestureClick *gesture, int n_press, double x,
                         double y, gpointer data)
{
    GtkWidget *widget
        = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
    struct tracker_section *sec;
    int row = -1, col = -1;
    (void)data;

    sec = g_object_get_data (G_OBJECT (widget), "tracker-section");
    if (!sec) {
        return;
    }
    gtk_hlist_get_selection_info (GTK_HLIST (widget), (int)x, (int)y, &row,
                                  &col);
    selected_section = sec;
    selected_row = row;

    if (n_press == 2) {
        struct tracker_server *server;

        server = gtk_hlist_get_row_data (GTK_HLIST (sec->list), row);
        if (!server || !server->address) {
            return;
        }
        memset (the_session.htlc.compressalg, 0,
                sizeof (the_session.htlc.compressalg));
        memset (the_session.htlc.cipheralg, 0,
                sizeof (the_session.htlc.cipheralg));
        /* Tracker-driven connects are always plaintext: the tracker
		 * doesn't advertise a TLS port today. If a future tracker
		 * variant returns one (see docs/tls-scoping.md §10), this
		 * stamp gets replaced with that field.
		 *
		 * Phase E: server->address is the printable string the boxed
		 * event delivered — IPv4 dotted-quad, IPv6 colon-hex, or
		 * a literal hostname. hx_connect's first arg is a host
		 * string that GSocketClient resolves via getaddrinfo, so
		 * all three forms route through the same resolver and
		 * connect happily. */
        hx_connect (&the_session.htlc, server->address, server->port, "", "", 0,
                    /*tls=*/0);
    }
}

static void
tracker_connect (void)
{
    struct tracker_server *server;

    if (!selected_section || selected_row < 0) {
        return;
    }
    server = gtk_hlist_get_row_data (GTK_HLIST (selected_section->list),
                                     selected_row);
    if (server && server->address) {
        create_connect_window (0, &the_session);
        /* Phase E: hand the connect-dialog the address string verbatim
         * (IPv4 dotted-quad, IPv6 colon-hex, or hostname). The dialog
         * just stuffs it into the host entry where the user can
         * tweak it before connecting. */
        connect_set_entries (server->address, 0, 0, server->port);
    }
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
show_server_details (struct tracker_server *server)
{
    AdwDialog *dlg;
    GtkWidget *toolbar_view, *header, *content, *clamp, *scrolled;
    AdwPreferencesGroup *grp;
    HxTrackerV3Meta *m;
    char title_buf[256];
    char addrport[256];
    char nbuf[32];
    int n;

    if (!server) {
        return;
    }

    dlg = ADW_DIALOG (adw_dialog_new ());
    g_snprintf (title_buf, sizeof (title_buf), _ ("Server details — %s"),
                (server->name && *server->name)
                    ? server->name
                    : (server->address ? server->address : "?"));
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

    m = server->meta;

    /* ------------------ Server (always present) ------------------ */
    grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (grp, _ ("Server"));
    n = 0;
    if (server->name && *server->name) {
        n += details_add_str_row (grp, _ ("Name"), server->name);
    }
    if (server->address && *server->address) {
        /* Bracket the address when it contains a colon so an IPv6
         * literal (either from an IPV6-type wire record or from a
         * HOSTNAME-type record carrying an IPv6 literal, which is
         * what Argus emits for promoted servers) doesn't render as
         * `2001:db8::1:5500` and leave the reader guessing where the
         * port starts. RFC 3986 §3.2.2 bracket form. IPv4 and bare
         * hostnames have no colons, so they pass through unchanged. */
        if (strchr (server->address, ':') != NULL) {
            g_snprintf (addrport, sizeof (addrport), "[%s]:%u", server->address,
                        server->port);
        } else {
            g_snprintf (addrport, sizeof (addrport), "%s:%u", server->address,
                        server->port);
        }
        n += details_add_str_row (grp, _ ("Address"), addrport);
    }
    if (server->desc && *server->desc) {
        n += details_add_str_row (grp, _ ("Description"), server->desc);
    }
    g_snprintf (nbuf, sizeof (nbuf), "%u", server->nusers);
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

/* Headerbar "Details" button handler. Uses the last-selected row
 * (tracker_storow), same convention as tracker_connect — the
 * select_row signal handler below keeps that current as the user
 * clicks or arrow-keys through the list. If no row has been
 * selected yet, fall through silently rather than opening an empty
 * dialog. */
static void
tracker_details (void)
{
    struct tracker_server *server;

    if (!selected_section || selected_row < 0) {
        return;
    }
    server = gtk_hlist_get_row_data (GTK_HLIST (selected_section->list),
                                     selected_row);
    if (server) {
        show_server_details (server);
    }
}

/* Track the selection so the Details / Connect buttons act on the
 * row the user has highlighted (with mouse OR arrow keys). The
 * tracker_section_pressed gesture handler also updates the selection
 * on click — that's still needed for the n_press == 2 double-click
 * path which fires before any select_row arrives. The owning section
 * is read out of the list's "tracker-section" qdata stashed at
 * section construction time. */
static void
tracker_section_row_selected (GtkHList *hlist, gint row,
                              gint column G_GNUC_UNUSED,
                              gpointer event G_GNUC_UNUSED,
                              gpointer data G_GNUC_UNUSED)
{
    struct tracker_section *sec
        = g_object_get_data (G_OBJECT (hlist), "tracker-section");
    if (!sec) {
        return;
    }
    selected_section = sec;
    selected_row = row;
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

    /* Per-section sink. Created here, populated by tracker_batch_begin
     * as each tracker comes online. The value-destroy hook tears each
     * section down on hash removal. */
    tracker_sections = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                              tracker_section_free_hash);
    tracker_sections_order = NULL;
    current_section = NULL;
    selected_section = NULL;
    selected_row = -1;

    /* Phase 5: GtkSearchEntry replaces the legacy "Search:" label +
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

    /* Phase 5: action buttons live in the AdwHeaderBar now, not in a
	 * row beneath. Refresh + Connect on the leading edge, the live
	 * found-count "N / M" indicator on the trailing edge. */
    refreshbtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/refresh.png",
                                      _ ("Refresh tracker list"), 2,
                                      G_CALLBACK (tracker_getlist), sess);
    connbtn = gtkhx_pixmap_button ("/com/nasledov/gtkhx/pixmaps/connect.png",
                                   _ ("Connect to selected server"), 2,
                                   G_CALLBACK (tracker_connect), 0);
    /* Phase B: opens the v3 metadata popover for whichever row is
     * currently highlighted. Uses a stock symbolic icon rather than
     * a bespoke pixmap so the button reads correctly under both
     * light + dark themes. */
    detailsbtn = gtk_button_new_from_icon_name ("dialog-information-symbolic");
    gtk_widget_set_tooltip_text (detailsbtn,
                                 _ ("Show details for selected server"));
    g_signal_connect (detailsbtn, "clicked", G_CALLBACK (tracker_details),
                      NULL);

    header = adw_header_bar_new ();
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), refreshbtn);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), connbtn);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), detailsbtn);

    count_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append (GTK_BOX (count_box), lbl_found);
    gtk_box_append (GTK_BOX (count_box), lbl_total);
    adw_header_bar_pack_end (ADW_HEADER_BAR (header), count_box);

    gtk_window_set_titlebar (GTK_WINDOW (tracker_window), header);

    /* Per-tracker sections stack inside a scrolled vbox. Each section
     * is a GtkExpander with its own scrolled GtkHList inside; the
     * section's inner scroller is vertical=NEVER so the list sizes
     * to its full content (header + every row) and contributes that
     * full height up to this outer scroller, which does the actual
     * vertical scrolling for the stack. The per-section scroller
     * IS still used for horizontal column overflow. 8 px gutter
     * between sections. */
    tracker_sections_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    sections_scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sections_scroll),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (sections_scroll),
                                       FALSE);
    gtk_widget_set_vexpand (sections_scroll, TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sections_scroll),
                                   tracker_sections_box);

    /* Phase 5 layout: actions live in the headerbar, content vbox
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
