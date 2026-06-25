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
#include <errno.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include <adwaita.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>
#include "hx.h"
#include "gtkhx.h"
#include "news.h"
#include "xtext.h"
#include "cicn.h"
#include "sound.h"
#include "users.h"
#include "chat.h"
#include "files.h"
#include "network.h"
#include "news15.h"
#include "log.h"
#include "tray.h"
#include "gtkutil.h"
#include "cfgkeys.h"
#include "gtkhx_theme.h"
#include "prefs_parser.h"
#include "options.h"
#include "text_util.h"
#include "tracker.h"
#include "debug.h"
#include "voice_runtime.h"
#include "voice_ptt_keyspec.h"

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

time_t start_time;
time_t total_time;

static struct icon_viewer *iv;

GtkWidget *options_window = NULL;

/* Tracker page (Settings → Trackers): GListStore<GtkStringObject *>
 * is the source of truth; the GtkColumnView reads through a
 * GtkSingleSelection wrapping the store. The widget pointer is
 * kept so close_options_bookkeeping doesn't have to know the
 * model topology — the existing teardown chain unrefs the view,
 * which drops the selection's ref on the model, etc. The store
 * pointer is the one we mutate from add_tracker / remove_tracker.
 * Module-static rather than per-page-local so parse_tracker_list
 * (the Save path) can read the model without plumbing a widget
 * pointer through every dialog handler. */
static GtkWidget *tracker_list = NULL;
static GListStore *tracker_store = NULL;
static GtkSingleSelection *tracker_selection = NULL;

struct gtkhx_prefs gtkhx_prefs = {
    0,                /* num_tracker */
    CFG_THEME_SYSTEM, /* theme: see CFG_THEME_* in cfgkeys.h */
    "",               /* auto_reply_msg */
    "fixed",          /* font */
    ".",              /* download_path */
    "[%H:%M:%S] ",    /* stamp_format — strftime(3); see CFG_STAMP_FORMAT */
    "",   /* highlight_words — empty by default; own nick is implicit */
    NULL, /* tracker (char **) */
    "hltracker.com", /* tracker_str */
    500,             /* xbuf_max */
    { { 412, 312, 10, 434, 0, 1 },
      { 412, 384, 10, 50, 0, 1 },
      { 0, 0, 442, 0, 0, 0 },
      { 300, 250, 442, 480, 0, 1 },
      { 300, 400, 442, 50, 0, 1 } },
    1, /* queuedl */
    1, /* showjoin */
    0, /* showback */
    0, /* tray (init_variables sets default) */
    0, /* auto_reply */
    0, /* timestamp */
    0, /* word_wrap */
    1, /* track_case */
    0, /* old_nickcompletion */
    0, /* outrate_limit */
    0, /* inrate_limit */
    0, /* logging */

    /* HexChat-style autocopy controls. Default-on for text
	 * matches every modern chat client (HexChat, Discord, Slack, etc.)
	 * and matches the Settings mockup. Stamp / color stay off — most
	 * users want a clean copy of the message body. */
    1, /* autocopy_text */
    0, /* autocopy_stamp */
    0, /* autocopy_color */

    /* notification toggles. Zero-initialised here;
	 * init_variables sets the user-facing defaults (mentions on,
	 * private messages on, etc.) and prefs_read overrides those
	 * from gtkhxrc. The static-init defaults are deliberately
	 * conservative (all-off) so a barebones init path doesn't
	 * spam notifications. */
    0, /* notify_chat */
    0, /* notify_chat_highlight */
    0, /* notify_msg */
    0, /* notify_pchat */
    0, /* notify_pchat_highlight */
    0, /* notify_pchat_invite */
    0, /* notify_news */
    0, /* notify_xfer */
    0, /* notify_broadcast */
    0, /* notify_omit_focused */

    0, /* out_bps */
    0, /* in_bps */

    /* chat-history initial fetch count. 50 matches the
	 * fogWraith spec's recommended default and what Phase 1/2/3
	 * shipped with hard-coded. */
    50, /* chat_history_initial */

    /* Colored-Nicknames extension. Default -1 ==
	 * HX_NICK_COLOR_NONE (cast to signed int) so a fresh prefs file
	 * means "no color, use theme default" and hx_change_name_icon
	 * skips the optional HTLC_DATA_COLOR chunk. */
    -1, /* nick_color */

    /* Phase 8.E + 8.E follow-up: voice device + PTT defaults. NULL
     * strings here get reallocated to "" by init_variables, which
     * also flips the cfgvar.allocated bit so prefs_write knows the
     * heap-owned form. Setting them NULL rather than literal "" in
     * this static initializer keeps the string-pointer fields
     * uniformly NULL-or-heap, avoiding the read-only-string vs.
     * g_free hazard if any pref-write codepath ever tries to free
     * one. */
    NULL, /* voice_input_device */
    NULL, /* voice_output_device */
    0,    /* voice_ptt_enabled */
    NULL, /* voice_ptt_key */

    /* Theming: active theme name. NULL / empty falls back to the
	 * built-in "default" theme. All scale and palette state lives in
	 * the theme file ($CONFIG/themes/<name>.ini, fallback to GResource).
	 * See gtkhx_theme.{c,h} and docs/theming-file-format.md. */
    NULL, /* theme_name */
};

static void parse_tracker (session *);

struct icon_viewer {
    guint32 icon_high;
    unsigned int nfound;
    GtkWidget *icon_list; /* multi-column flowbox for narrow icons */
    GtkWidget *wide_list; /* one-per-row flowbox for wide banners */
};

/* A wide cicn (Mac banner-style icon) is anything wider than this
 * before the WIDE_BANNER_LEFT_PAD crop. Same threshold the user-list
 * overlay uses. Wide entries go into the dedicated single-column
 * flowbox so the user sees them at their natural aspect ratio
 * (~432x32 cropped to ~232x32 then scaled to fill the row) rather
 * than squashed into the 56x56 grid cell used for normal icons. */
#define ICON_PICKER_WIDE_THRESHOLD 400
#define ICON_PICKER_WIDE_CROP 198
#define ICON_PICKER_WIDE_HEIGHT 32 /* match cicn native vertical pixel count */

/* forward declarations for the icon-picker FlowBox helpers.
 * The implementations live further down (after struct cfgvar and
 * cfgvar_for_name) since they touch the cfgvar lookup table; the
 * forward decls let list_icons() and settings_page_identity() —
 * both up here — wire them. */
static int icon_picker_sort_cb (GtkFlowBoxChild *a, GtkFlowBoxChild *b,
                                gpointer data);
static void icon_flow_child_activated (GtkFlowBox *flowbox,
                                       GtkFlowBoxChild *child, gpointer data);

static void
list_icons (void)
{
    /* GtkFlowBox auto-flows multiple icons per row. Each entry is a
	 * 64x64 image plus the resource-ID label, packed into a vertical
	 * box and inserted as a flowbox child. The flowbox itself is
	 * configured with min/max children per line so the picker grows
	 * to as many columns as fit the picker width without going
	 * single-column-narrow.
	 *
	 * cicn_to_pixbuf returns a GdkPixbuf directly with the Mac
	 * classic mask folded into the alpha channel. Wide icons (the
	 * 32x32 family bundles four variants in a 4*32-pixel row)
	 * are clipped to the rightmost 32 px to mirror the historical
	 * "off = width > 400 ? 198 : 0" hack.
	 *
	 * Two passes: first walk every rsrc file in icon_files priority
	 * order (user $CONFIG/icons → XDG → $PREFIX per init_icons) and
	 * pick a single winning resource per resid, keeping the FIRST
	 * occurrence so user customizations override system defaults
	 * instead of both showing up as duplicates in the picker.
	 * Second pass renders the winners. */
    GtkWidget *icon_list = iv->icon_list;
    guint16 nres;
    guint32 icon;
    unsigned int nfound = 0;
    unsigned int i;
    unsigned int rendered;
    GHashTable *winners;
    GHashTableIter iter;
    gpointer iter_key, iter_val;

    winners = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
                                     g_free);

    for (i = 0; i < icon_files.n; ++i) {
        if (!icon_files.cicns[i]) {
            continue;
        }
        nres = macres_file_num_res_of_type (icon_files.cicns[i], TYPE_cicn);
        for (icon = 0; icon < nres; icon++) {
            macres_res *r;
            gpointer key;

            r = macres_file_get_nth_res_of_type (icon_files.cicns[i], TYPE_cicn,
                                                 icon);
            if (!r) {
                continue;
            }
            key = GUINT_TO_POINTER ((guint) r->resid);
            if (g_hash_table_contains (winners, key)) {
                /* A higher-priority file already claimed this
				 * resid — discard the duplicate so it doesn't
				 * show up alongside the user's version in the
				 * picker. */
                g_free (r);
                continue;
            }
            g_hash_table_insert (winners, key, r);
        }
    }

    rendered = 0;
    g_hash_table_iter_init (&iter, winners);
    while (g_hash_table_iter_next (&iter, &iter_key, &iter_val)) {
        macres_res *r = iter_val;
        GdkPixbuf *pb, *cropped, *scaled;
        GtkWidget *child, *vbox, *image, *label;
        int width, height, off;
        char buf[16];
        gboolean is_wide;

        pb = cicn_to_pixbuf (r->data, r->datalen);
        if (!pb) {
            continue;
        }
        width = gdk_pixbuf_get_width (pb);
        height = gdk_pixbuf_get_height (pb);
        is_wide = (width > ICON_PICKER_WIDE_THRESHOLD);
        off = is_wide ? ICON_PICKER_WIDE_CROP : 0;
        if (off) {
            cropped
                = gdk_pixbuf_new_subpixbuf (pb, off, 0, width - off, height);
            g_object_unref (pb);
            pb = cropped;
            width -= off;
        }

        /* Narrow icons → uniform 56x56 thumbnail for the grid.
		 * Wide banners → preserve aspect ratio (scale to a row-
		 * height of 56 and proportional width) so the banner
		 * art is recognisable instead of being squashed into a
		 * square cell. */
        if (is_wide) {
            int target_h = 56;
            int target_w = width * target_h / (height > 0 ? height : 1);
            if (target_w < 1) {
                target_w = 1;
            }
            scaled = gdk_pixbuf_scale_simple (pb, target_w, target_h,
                                              GDK_INTERP_NEAREST);
        } else {
            /* Nearest-neighbor preserves pixel-art look at
			 * 3.5x of 16px / 1.75x of 32px sources. */
            scaled = gdk_pixbuf_scale_simple (pb, 56, 56, GDK_INTERP_NEAREST);
        }
        g_object_unref (pb);
        pb = scaled ? scaled : pb;

        nfound++;
        g_snprintf (buf, sizeof (buf), "%u", r->resid);

        vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
        /* GtkPicture, not GtkImage. Adwaita's
		 * stylesheet sizes GtkImage to icon-button dimensions
		 * (~16-24px) regardless of the paintable's natural
		 * size, so set_size_request on a wrapper grew the cell
		 * but kept the icon clamped tiny inside it. GtkPicture
		 * with can_shrink=FALSE pins to the paintable's natural
		 * size — same fix gtkhx_pixmap_button uses for the
		 * toolbar buttons. */
        {
            GdkTexture *tex = gtkhx_texture_from_pixbuf (pb);
            if (tex) {
                image = gtk_picture_new_for_paintable (GDK_PAINTABLE (tex));
                g_object_unref (tex);
                gtk_picture_set_can_shrink (GTK_PICTURE (image), FALSE);
            } else {
                image = gtk_picture_new ();
            }
        }
        label = gtk_label_new (buf);
        gtk_widget_add_css_class (label, "caption");
        gtk_box_append (GTK_BOX (vbox), image);
        gtk_box_append (GTK_BOX (vbox), label);
        gtk_widget_set_margin_start (vbox, 6);
        gtk_widget_set_margin_end (vbox, 6);
        gtk_widget_set_margin_top (vbox, 6);
        gtk_widget_set_margin_bottom (vbox, 6);

        child = gtk_flow_box_child_new ();
        gtk_flow_box_child_set_child (GTK_FLOW_BOX_CHILD (child), vbox);
        g_object_set_data (G_OBJECT (child), "resid",
                           GUINT_TO_POINTER (r->resid));
        /* Wide banners go into their own dedicated flowbox so
		 * they show one-per-row at natural aspect ratio. */
        if (is_wide && iv->wide_list) {
            gtk_flow_box_append (GTK_FLOW_BOX (iv->wide_list), child);
        } else {
            gtk_flow_box_append (GTK_FLOW_BOX (icon_list), child);
        }

        g_object_unref (pb);

        /* Cooperative multitasking — keep the dialog responsive while
		 * paging through hundreds of resource entries. */
        rendered++;
        if (rendered % 10 == 0) {
            while (g_main_context_pending (NULL)) {
                g_main_context_iteration (NULL, TRUE);
            }
            if (!options_window) {
                /* Dialog closed mid-render. winners owns the
				 * remaining macres_res entries; destroying the
				 * table frees them via the destroy_func. */
                g_hash_table_destroy (winners);
                return;
            }
        }
    }

    g_hash_table_destroy (winners);

    if (nfound >= 2) {
        gtk_flow_box_invalidate_sort (GTK_FLOW_BOX (icon_list));
        if (iv->wide_list) {
            gtk_flow_box_invalidate_sort (GTK_FLOW_BOX (iv->wide_list));
        }
    }
}

void
reinit_gtktexts (session *sess)
{
    if (gtkhx_prefs.geo.news.open) {
        gtkhx_apply_text_style (sess->news_text);
    }
    {
        gchar *fontname = pango_font_description_to_string (gtkhx_font_desc);
        if (sess->gchats) {
            GHashTableIter iter;
            gpointer key, val;
            g_hash_table_iter_init (&iter, sess->gchats);
            while (g_hash_table_iter_next (&iter, &key, &val)) {
                struct gtkhx_chat *gchat = val;
                if (GPOINTER_TO_UINT (key) == 0 && !gtkhx_prefs.geo.chat.open) {
                    continue;
                }
                gtk_xtext_set_font (GTK_XTEXT (gchat->output), fontname);
                gtk_xtext_refresh (GTK_XTEXT (gchat->output));
                if (gchat->input) {
                    gtkhx_apply_input_font (gchat->input);
                }
                if (gchat->subject) {
                    gtkhx_apply_text_style (gchat->subject);
                }
            }
        }
        /* walk the PM-window hashtable. The pre-port
		 * code walked `sess->msg_list` here, but that field was
		 * never populated — the real msg_list global lived in
		 * msg.c, so this loop was a silent no-op for years and
		 * font-changes never reached open PM windows. The
		 * migration to session->msg_windows fixes that as a side
		 * effect. */
        if (sess->msg_windows) {
            GHashTableIter iter;
            gpointer val;
            g_hash_table_iter_init (&iter, sess->msg_windows);
            while (g_hash_table_iter_next (&iter, NULL, &val)) {
                struct msgwin *msg = val;
                gtk_xtext_set_font (GTK_XTEXT (msg->outputbuf), fontname);
                gtk_xtext_refresh (GTK_XTEXT (msg->outputbuf));
                gtkhx_apply_input_font (msg->inputbuf);
            }
        }
        g_free (fontname);
    }
}

static void
changed_xtext (session *sess)
{
    if (sess) {
        if (sess->gchats) {
            GHashTableIter iter;
            gpointer val;
            g_hash_table_iter_init (&iter, sess->gchats);
            while (g_hash_table_iter_next (&iter, NULL, &val)) {
                struct gtkhx_chat *gchat = val;
                GTK_XTEXT (gchat->output)->wordwrap = gtkhx_prefs.word_wrap;
                GTK_XTEXT (gchat->output)->max_lines = gtkhx_prefs.xbuf_max;
                gtk_xtext_refresh (GTK_XTEXT (gchat->output));
            }
        }
        if (sess->msg_windows) {
            GHashTableIter iter;
            gpointer val;
            g_hash_table_iter_init (&iter, sess->msg_windows);
            while (g_hash_table_iter_next (&iter, NULL, &val)) {
                struct msgwin *msg = val;
                GTK_XTEXT (msg->outputbuf)->wordwrap = gtkhx_prefs.word_wrap;
                GTK_XTEXT (msg->outputbuf)->max_lines = gtkhx_prefs.xbuf_max;
                gtk_xtext_refresh (GTK_XTEXT (msg->outputbuf));
            }
        }
    }
}

/* apply the CFG_TIMESTAMP toggle to every live xtext buffer
 * — chat / pchat outputs in gchat_list, plus PM outputs in msg_windows.
 * Native xtext stamps are flipped per-buffer via gtk_xtext_set_time_stamp.
 * gtk_xtext_refresh forces a full re-render so the new state is visible
 * without scrolling the buffer first. */
static void
changed_timestamp (session *sess)
{
    if (!sess) {
        return;
    }
    if (sess->gchats) {
        GHashTableIter iter;
        gpointer val;
        g_hash_table_iter_init (&iter, sess->gchats);
        while (g_hash_table_iter_next (&iter, NULL, &val)) {
            struct gtkhx_chat *gchat = val;
            gtk_xtext_set_time_stamp (GTK_XTEXT (gchat->output)->buffer,
                                      gtkhx_prefs.timestamp);
            gtk_xtext_refresh (GTK_XTEXT (gchat->output));
        }
    }
    if (sess->msg_windows) {
        GHashTableIter iter;
        gpointer val;
        g_hash_table_iter_init (&iter, sess->msg_windows);
        while (g_hash_table_iter_next (&iter, NULL, &val)) {
            struct msgwin *msg = val;
            gtk_xtext_set_time_stamp (GTK_XTEXT (msg->outputbuf)->buffer,
                                      gtkhx_prefs.timestamp);
            gtk_xtext_refresh (GTK_XTEXT (msg->outputbuf));
        }
    }
}

/* re-enabled. The Settings nick/icon controls used to be
 * inert — changing them updated gtkhx_prefs / the_session.htlc but
 * never told the server, so the user list still showed your old
 * nick/icon until reconnect. hlwrite is already a no-op when
 * htlc->fd is 0 (network.c:1448), so this is safe to call before
 * connect too: prefs_read doesn't run change-callbacks anyway, only
 * the Adw row notify handlers do, so the wire packet only fires
 * when the user actually toggles a control in Settings. */
static void
changed_nickoricon (session *sess)
{
    (void)sess;
    hx_change_name_icon (&the_session.htlc);
}

/* Colored-Nicknames extension — mirror the pref onto the
 * live htlc and re-broadcast via USER_CHANGE. Prefs store the color
 * as int (-1 = "no color"); reinterpret as guint32 so HX_NICK_COLOR
 * _NONE round-trips bit-identically.
 *
 * We don't wait for the server to echo USER_CHANGE back to us before
 * repainting our own row: servers vary on whether they echo a user's
 * own broadcast to that user, and even when they do, the round-trip
 * is enough of a lag that the user perceives the picker as broken.
 * Update self's hx_user.nick_color directly and re-render. The
 * inbound echo (if any) lands on top with the same value — no
 * flicker, just an idempotent rewrite. */
static void
changed_nick_color (session *sess)
{
    (void)sess;
    guint32 nc = (guint32)gtkhx_prefs.nick_color;
    the_session.htlc.nick_color = nc;
    hx_change_name_icon (&the_session.htlc);

    /* Locally re-render our own row in the public chat user list.
	 * Pre-login (no uid yet, or no chat container yet) just no-ops —
	 * apply_loaded_xtext_prefs stamps the loaded pref onto htlc, and
	 * the SELFINFO-driven hx_user_new for self picks it up the same
	 * way it picks up the loaded nick. */
    struct chat *pub = chat_with_cid (&the_session, 0);
    if (pub && the_session.htlc.uid) {
        struct hx_user *self = hx_user_with_uid (pub, the_session.htlc.uid);
        if (self) {
            self->nick_color = nc;
            user_change (&the_session.htlc, pub, self, self->name, self->icon,
                         self->color);
        }
    }
}

static void
changed_font (session *sess)
{
    if (gtkhx_font_desc) {
        pango_font_description_free (gtkhx_font_desc);
        gtkhx_font_desc = NULL;
    }

    if (gtkhx_prefs.font && *gtkhx_prefs.font) {
        gtkhx_font_desc = pango_font_description_from_string (gtkhx_prefs.font);
    }

    if (!gtkhx_font_desc) {
        g_warning ("Bad font \"%s\"\n",
                   gtkhx_prefs.font ? gtkhx_prefs.font : "");
        gtkhx_font_desc = pango_font_description_from_string ("Monospace 10");
        if (gtkhx_prefs.font) {
            g_free (gtkhx_prefs.font);
        }
        gtkhx_prefs.font = g_strdup ("Monospace 10");
    }

    /* rebuild the screen-wide CSS provider so already-tagged
	 * widgets pick up the new font without needing per-widget calls. */
    gtkhx_refresh_css ();

    if (sess) {
        reinit_gtktexts (sess);
    }
}

#if 0 /* XXX */
static void changed_logging (session *sess)
{
	if(!gtkhx_prefs.logging) {
		close_logs();
	}
}
#endif

/* HexChat-style xtext autocopy. Each toggle in Settings →
 * Advanced → Auto Copy Behavior calls one of the three xtext setters
 * to flip the corresponding facet of the drag-end clipboard handler.
 * The cfgvars own the persisted state in gtkhx_prefs.autocopy_* bytes;
 * the changefunc just propagates that to the xtext-internal `prefs`
 * struct so the next drag-end picks up the new behaviour without
 * having to recreate the widget. */
static void
changed_autocopy_text (session *sess)
{
    (void)sess;
    gtk_xtext_set_autocopy_text (gtkhx_prefs.autocopy_text);
}
static void
changed_autocopy_stamp (session *sess)
{
    (void)sess;
    gtk_xtext_set_autocopy_stamp (gtkhx_prefs.autocopy_stamp);
}
static void
changed_autocopy_color (session *sess)
{
    (void)sess;
    gtk_xtext_set_autocopy_color (gtkhx_prefs.autocopy_color);
}

/* apply CFG_STAMP_FORMAT to every live xtext widget. The
 * setter stashes the new format in xtext's module-global, recomputes
 * stamp_width per widget (font-dependent), and grows the buffer
 * indent if the new column is wider than before. queue_draw fires
 * inside the setter so the new column shows up next frame. */
static void
changed_stampformat (session *sess)
{
    if (!sess) {
        return;
    }
    if (sess->gchats) {
        GHashTableIter iter;
        gpointer val;
        g_hash_table_iter_init (&iter, sess->gchats);
        while (g_hash_table_iter_next (&iter, NULL, &val)) {
            struct gtkhx_chat *gchat = val;
            gtk_xtext_set_stamp_format (GTK_XTEXT (gchat->output),
                                        gtkhx_prefs.stamp_format);
        }
    }
    if (sess->msg_windows) {
        GHashTableIter iter;
        gpointer val;
        g_hash_table_iter_init (&iter, sess->msg_windows);
        while (g_hash_table_iter_next (&iter, NULL, &val)) {
            struct msgwin *msg = val;
            gtk_xtext_set_stamp_format (GTK_XTEXT (msg->outputbuf),
                                        gtkhx_prefs.stamp_format);
        }
    }
}

static void
changed_downloadpath (session *sess)
{
    (void)sess;
    /* Empty pref value → use the user's XDG Downloads dir as
	 * the natural default. Falls back further to $HOME if
	 * XDG_DOWNLOAD_DIR isn't set / not a directory, then to "."
	 * as a last resort. This is the value the Files browser
	 * also consults for its initial local panel path, so the
	 * two stay in sync. */
    if (!gtkhx_prefs.download_path || !*gtkhx_prefs.download_path) {
        const char *xdg = g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);
        if (xdg && g_file_test (xdg, G_FILE_TEST_IS_DIR)) {
            gtkhx_prefs.download_path = g_strdup (xdg);
        } else {
            const char *home = g_get_home_dir ();
            if (home && g_file_test (home, G_FILE_TEST_IS_DIR)) {
                gtkhx_prefs.download_path = g_strdup (home);
            } else {
                gtkhx_prefs.download_path = g_strdup (".");
            }
        }
    }
}

static void
changed_case (session *sess)
{
    (void)sess;
    /* case-fold lives on gtkhx_prefs.track_case and is read
     * directly by tracker.c::tracker_rerun_search when compiling the
     * GRegex (G_REGEX_CASELESS toggled per the pref). No global regex-
     * engine state to flip; just kick the live filter so the result
     * list reflects the new mode immediately. No-op when the Tracker
     * isn't open. */
    tracker_search_refresh ();
}

/* apply the THEME pref to libadwaita's style manager. The pref
 * is one of "system" / "light" / "dark"; anything else falls back to
 * the system default so a hand-edited gtkhxrc with a typo doesn't lock
 * the user into a broken state. Called both at startup (after prefs_read)
 * and via the cfgvar change-callback when the user picks a new value
 * in Settings. */
static void
changed_theme (session *sess)
{
    AdwStyleManager *sm = adw_style_manager_get_default ();
    const char *theme
        = gtkhx_prefs.theme ? gtkhx_prefs.theme : CFG_THEME_SYSTEM;
    AdwColorScheme scheme;

    (void)sess;

    if (g_strcmp0 (theme, CFG_THEME_LIGHT) == 0) {
        scheme = ADW_COLOR_SCHEME_FORCE_LIGHT;
    } else if (g_strcmp0 (theme, CFG_THEME_DARK) == 0) {
        scheme = ADW_COLOR_SCHEME_FORCE_DARK;
    } else {
        scheme = ADW_COLOR_SCHEME_DEFAULT;
    }

    adw_style_manager_set_color_scheme (sm, scheme);
}
/* Settings → General → "Show tray icon" toggle. Drives the
 * StatusNotifierItem registration on the session bus — switching
 * ON kicks off tray_activate_register; switching OFF unregisters
 * but leaves the module reachable for a future flip. */
static void
changed_tray (session *sess)
{
    (void)sess;
    gtkhx_tray_set_enabled (gtkhx_prefs.tray);
}

/* Settings → Voice → "Input device" combobox. Pushes the user's
 * pick through to the Rust runtime via FFI; the next VoiceRuntime
 * construction (typically the next Join Voice click) builds the
 * send leg against the resolved device. Empty / NULL means
 * "system default" — autoaudiosrc resolves whichever PulseAudio /
 * PipeWire / ALSA default the host has configured.
 *
 * No effect on a currently-active voice session — the runtime is
 * constructed once per session and the bins are built at Join
 * time; changing the device picker takes effect on the next call.
 * Phase 8.E follow-up could hot-swap by rebuilding the send bin
 * on prefs change, but for now Leave + Join is the prescribed
 * dance. */
static void
changed_voice_input_device (session *sess)
{
    (void)sess;
    gtkhx_voice_set_input_device (gtkhx_prefs.voice_input_device);
}

/* Settings → Voice → "Output device" combobox. Same shape as
 * changed_voice_input_device but for the receive (autoaudiosink)
 * side. */
static void
changed_voice_output_device (session *sess)
{
    (void)sess;
    gtkhx_voice_set_output_device (gtkhx_prefs.voice_output_device);
}

/* changefunc for the active-theme name. A change here (manually
 * editing gtkhxrc, or — in a future theme-editor phase — Settings
 * picking a different theme) reloads the theme file and emits the
 * theme "changed" signal so every subscribed button / user-list view
 * / chat xtext rescales and repaints. */
static void
changed_theme_name (session *sess)
{
    (void)sess;
    gtkhx_theme_load_active ();
}

struct cfgvar {
    /* name of variable as it appears in conf file */
    const char *name;
    /* pointer to where data should be writen */
    /* The unionization is to avoid strong-typed nightmares with casting */
    union {
        void *var;
        char **str;
        char *str32;
        int *integer;
        unsigned char *uchar;
        time_t *timet;
        guint16 *uint16;
    } variable;
    /* type of variable pointed to by "variable" is stored in "type": */
#define INT 1                            /* int* */
#define BOOLEAN 2 /* unsigned char:1* */ /* INT1 */
#define STRING 3                         /* string (char *) */
#define STRING32 4
#define UINT16 5
#define TIME_T 6
    const unsigned int type : 7;
    unsigned int allocated : 1; /* only meaningful for a string */
    /* func to call when changed */
    void (*changefunc) (session *);
    GtkWidget *widget;
} cfgvars[] = {
    { CFG_AUTOCOPY_COLOR,
      { &gtkhx_prefs.autocopy_color },
      BOOLEAN,
      0,
      changed_autocopy_color,
      NULL },
    { CFG_AUTOCOPY_STAMP,
      { &gtkhx_prefs.autocopy_stamp },
      BOOLEAN,
      0,
      changed_autocopy_stamp,
      NULL },
    { CFG_AUTOCOPY_TEXT,
      { &gtkhx_prefs.autocopy_text },
      BOOLEAN,
      0,
      changed_autocopy_text,
      NULL },
    { CFG_AUTOREPLY_MSG,
      { &gtkhx_prefs.auto_reply_msg },
      STRING,
      0,
      NULL,
      NULL },
    { CFG_AUTOREPLY_ON, { &gtkhx_prefs.auto_reply }, BOOLEAN, 0, NULL, NULL },
    { CFG_CHAT_HISTORY_INITIAL,
      { &gtkhx_prefs.chat_history_initial },
      INT,
      0,
      NULL,
      NULL },
    { CFG_CHAT_XSIZE, { &gtkhx_prefs.geo.chat.xsize }, INT, 0, NULL, NULL },
    { CFG_CHAT_YSIZE, { &gtkhx_prefs.geo.chat.ysize }, INT, 0, NULL, NULL },
    { CFG_DOWNLOAD,
      { &gtkhx_prefs.download_path },
      STRING,
      0,
      changed_downloadpath,
      NULL },
    /* file_samewin pref: retired in Phase 5 with the legacy single-pane
	 * files browser. Loaded values from old gtkhxrc files are silently
	 * ignored — the cfgvars table just doesn't list the key anymore,
	 * so prefs_read passes over it. */
    { CFG_FONT, { &gtkhx_prefs.font }, STRING, 0, changed_font, NULL },
    { CFG_HIGHLIGHT_WORDS,
      { &gtkhx_prefs.highlight_words },
      STRING,
      0,
      NULL,
      NULL },
    { CFG_ICON,
      { &the_session.htlc.icon },
      UINT16,
      0,
      changed_nickoricon,
      NULL },
#if 0 /* XXX */
	{CFG_LOGGING, {&gtkhx_prefs.logging}, BOOLEAN, 0, changed_logging, NULL},
#endif
    { CFG_NEWS_XSIZE, { &gtkhx_prefs.geo.news.xsize }, INT, 0, NULL, NULL },
    { CFG_NEWS_YSIZE, { &gtkhx_prefs.geo.news.ysize }, INT, 0, NULL, NULL },
    { CFG_NICK,
      { the_session.htlc.name },
      STRING32,
      0,
      changed_nickoricon,
      NULL },
    { CFG_NICK_COLOR,
      { &gtkhx_prefs.nick_color },
      INT,
      0,
      changed_nick_color,
      NULL },
    { CFG_NOTIFY_BROADCAST,
      { &gtkhx_prefs.notify_broadcast },
      BOOLEAN,
      0,
      NULL,
      NULL },
    { CFG_NOTIFY_CHAT, { &gtkhx_prefs.notify_chat }, BOOLEAN, 0, NULL, NULL },
    { CFG_NOTIFY_CHAT_HIGHLIGHT,
      { &gtkhx_prefs.notify_chat_highlight },
      BOOLEAN,
      0,
      NULL,
      NULL },
    { CFG_NOTIFY_MSG, { &gtkhx_prefs.notify_msg }, BOOLEAN, 0, NULL, NULL },
    { CFG_NOTIFY_NEWS, { &gtkhx_prefs.notify_news }, BOOLEAN, 0, NULL, NULL },
    { CFG_NOTIFY_OMIT_FOCUSED,
      { &gtkhx_prefs.notify_omit_focused },
      BOOLEAN,
      0,
      NULL,
      NULL },
    { CFG_NOTIFY_PCHAT, { &gtkhx_prefs.notify_pchat }, BOOLEAN, 0, NULL, NULL },
    { CFG_NOTIFY_PCHAT_HIGHLIGHT,
      { &gtkhx_prefs.notify_pchat_highlight },
      BOOLEAN,
      0,
      NULL,
      NULL },
    { CFG_NOTIFY_PCHAT_INVITE,
      { &gtkhx_prefs.notify_pchat_invite },
      BOOLEAN,
      0,
      NULL,
      NULL },
    { CFG_NOTIFY_XFER, { &gtkhx_prefs.notify_xfer }, BOOLEAN, 0, NULL, NULL },
    { CFG_OLD_NICKCOMP,
      { &gtkhx_prefs.old_nickcompletion },
      BOOLEAN,
      0,
      NULL,
      NULL },
    { CFG_OPEN_CHAT, { &gtkhx_prefs.geo.chat.init }, BOOLEAN, 0, NULL, NULL },
    { CFG_OPEN_NEWS, { &gtkhx_prefs.geo.news.init }, BOOLEAN, 0, NULL, NULL },
    { CFG_OPEN_TASKS, { &gtkhx_prefs.geo.tasks.init }, BOOLEAN, 0, NULL, NULL },
    { CFG_OPEN_USERS, { &gtkhx_prefs.geo.users.init }, BOOLEAN, 0, NULL, NULL },
    { CFG_QUEUEDL, { &gtkhx_prefs.queuedl }, BOOLEAN, 0, NULL, NULL },
    { CFG_SHOWBACK, { &gtkhx_prefs.showback }, BOOLEAN, 0, NULL, NULL },
    { CFG_SHOWJOIN, { &gtkhx_prefs.showjoin }, BOOLEAN, 0, NULL, NULL },
    { CFG_SND_CHAT, { &hxsnd.chat }, BOOLEAN, 0, NULL, NULL },
    { CFG_SND_ERROR, { &hxsnd.error }, BOOLEAN, 0, NULL, NULL },
    { CFG_SND_FILE, { &hxsnd.file }, BOOLEAN, 0, NULL, NULL },
    { CFG_SND_INVITE, { &hxsnd.invite }, BOOLEAN, 0, NULL, NULL },
    { CFG_SND_JOIN, { &hxsnd.join }, BOOLEAN, 0, NULL, NULL },
    { CFG_SND_LOGIN, { &hxsnd.login }, BOOLEAN, 0, NULL, NULL },
    { CFG_SND_MSG, { &hxsnd.msg }, BOOLEAN, 0, NULL, NULL },
    { CFG_SND_NEWS, { &hxsnd.news }, BOOLEAN, 0, NULL, NULL },
    { CFG_SND_PART, { &hxsnd.part }, BOOLEAN, 0, NULL, NULL },
    { CFG_SOUNDS_ON, { &hxsnd.on }, BOOLEAN, 0, NULL, NULL },
    { CFG_TASK_XSIZE, { &gtkhx_prefs.geo.tasks.xsize }, INT, 0, NULL, NULL },
    { CFG_TASK_YSIZE, { &gtkhx_prefs.geo.tasks.ysize }, INT, 0, NULL, NULL },
    { CFG_THEME, { &gtkhx_prefs.theme }, STRING, 0, changed_theme, NULL },
    { CFG_THEME_NAME,
      { &gtkhx_prefs.theme_name },
      STRING,
      0,
      changed_theme_name,
      NULL },
    { CFG_TIME, { &total_time }, TIME_T, 0, NULL, NULL },
    { CFG_TIMESTAMP,
      { &gtkhx_prefs.timestamp },
      BOOLEAN,
      0,
      changed_timestamp,
      NULL },
    { CFG_STAMP_FORMAT,
      { &gtkhx_prefs.stamp_format },
      STRING,
      0,
      changed_stampformat,
      NULL },
    { CFG_TOOL_XSIZE, { &gtkhx_prefs.geo.tool.xsize }, INT, 0, NULL, NULL },
    { CFG_TOOL_YSIZE, { &gtkhx_prefs.geo.tool.ysize }, INT, 0, NULL, NULL },
    { CFG_TRACKER,
      { &gtkhx_prefs.tracker_str },
      STRING,
      0,
      parse_tracker,
      NULL },
    { CFG_TRACKER_CASE,
      { &gtkhx_prefs.track_case },
      BOOLEAN,
      0,
      changed_case,
      NULL },
    { CFG_TRAY, { &gtkhx_prefs.tray }, BOOLEAN, 0, changed_tray, NULL },
    { CFG_USER_XSIZE, { &gtkhx_prefs.geo.users.xsize }, INT, 0, NULL, NULL },
    { CFG_USER_YSIZE, { &gtkhx_prefs.geo.users.ysize }, INT, 0, NULL, NULL },
    { CFG_VOICE_INPUT_DEVICE,
      { &gtkhx_prefs.voice_input_device },
      STRING,
      0,
      changed_voice_input_device,
      NULL },
    { CFG_VOICE_OUTPUT_DEVICE,
      { &gtkhx_prefs.voice_output_device },
      STRING,
      0,
      changed_voice_output_device,
      NULL },
    /* Phase 8.E follow-up: push-to-talk. PTT_ENABLED is a plain
     * BOOLEAN; PTT_KEY is a STRING that holds the canonical key
     * spec (`gdk_keyval_name` output ± modifier prefix). Both are
     * read at runtime-hook activation time — no changefunc, since
     * the hook samples the live prefs on every key event. */
    { CFG_VOICE_PTT_ENABLED,
      { &gtkhx_prefs.voice_ptt_enabled },
      BOOLEAN,
      0,
      NULL,
      NULL },
    { CFG_VOICE_PTT_KEY,
      { &gtkhx_prefs.voice_ptt_key },
      STRING,
      0,
      NULL,
      NULL },
    { CFG_WORDWRAP,
      { &gtkhx_prefs.word_wrap },
      BOOLEAN,
      0,
      changed_xtext,
      NULL },
    { CFG_XBUF_MAX, { &gtkhx_prefs.xbuf_max }, INT, 0, changed_xtext, NULL }
};

/* the parallel FOO_IDX enum that paired up with cfgvars[] is
 * gone. Every (*cfgvar_for_name(CFG_FOO)) reference is now cfgvar_for_name(CFG_FOO),
 * which bsearch-finds the entry by its config-file key. The enum was
 * a maintenance footgun: every new pref needed an entry in two places
 * in a specific order, and a missing #if-guarded entry (LOGGING_IDX)
 * could shift all the indices below it.
 *
 * cfgvars[] stays sorted alphabetically by name (the file/dialog
 * construction order doesn't depend on it), and bsearch over ~50
 * entries is ~6 string compares — negligible against the GTK widget
 * construction these calls drive. */
static int cfgnamecmp_const (const void *key, const void *mem);

/* Verify cfgvars[] is sorted alphabetically by name — bsearch needs
 * this and there's no compiler check. Run once at first call. */
static void
cfgvars_assert_sorted (void)
{
    static gboolean checked;
    gsize i;

    if (checked) {
        return;
    }
    checked = TRUE;

    for (i = 1; i < sizeof (cfgvars) / sizeof (cfgvars[0]); i++) {
        if (strcmp (cfgvars[i - 1].name, cfgvars[i].name) >= 0) {
            g_error ("cfgvars[] is not sorted: \"%s\" must come before \"%s\"",
                     cfgvars[i].name, cfgvars[i - 1].name);
        }
    }
}

static struct cfgvar *
cfgvar_for_name (const char *name)
{
    struct cfgvar *r;

    cfgvars_assert_sorted ();

    r = bsearch (name, cfgvars, sizeof (cfgvars) / sizeof (cfgvars[0]),
                 sizeof (cfgvars[0]), cfgnamecmp_const);
    if (!r) {
        /* Returning &cfgvars[0] as a fallback was actively dangerous —
		 * callers write to whatever field they expect (uchar / int /
		 * char**), and treating a STRING entry as a BOOLEAN scribbles
		 * across the str pointer and crashes on the next free(). NULL
		 * is the honest return; every caller in this file is paired
		 * with a *valid* name literal, so a NULL is a coding bug we
		 * want to surface, not paper over. */
        g_warning ("cfgvar_for_name: unknown pref \"%s\"", name);
        return NULL;
    }
    return r;
}

static int
cfgnamecmp_const (const void *key, const void *mem)
{
    return strcmp ((const char *)key, ((const struct cfgvar *)mem)->name);
}

/* prefs_write is defined after the row helpers but called by them
 * — the prototype lives in options.h. */

/* helper to compare two flowbox children by their stored
 * resource ID so the icon picker stays sorted by ID. Forward-declared
 * up near list_icons() because it's wired there too. */
static int
icon_picker_sort_cb (GtkFlowBoxChild *a, GtkFlowBoxChild *b, gpointer data)
{
    guint id_a = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (a), "resid"));
    guint id_b = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (b), "resid"));
    (void)data;

    if (id_a < id_b) {
        return -1;
    }
    if (id_a > id_b) {
        return 1;
    }
    return 0;
}

/* Handler for the FlowBox's child-activated signal. The activated
 * child carries the resid as g_object_set_data; copy it into the
 * AdwSpinRow so the user's selection becomes the active icon ID. */
static void
icon_flow_child_activated (GtkFlowBox *flowbox, GtkFlowBoxChild *child,
                           gpointer data)
{
    struct cfgvar *v;
    guint icon;
    (void)flowbox;
    (void)data;

    icon = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (child), "resid"));
    if (!icon) {
        return;
    }

    v = cfgvar_for_name (CFG_ICON);
    if (v && v->widget && ADW_IS_SPIN_ROW (v->widget)) {
        adw_spin_row_set_value (ADW_SPIN_ROW (v->widget), icon);
    }
}

/* ------------------------------------------------------------------- *
 * AdwPreferencesRow helpers
 *
 * Each helper builds an AdwPreferencesRow subclass for a cfgvars[]
 * entry, initialized from the cfgvar's current value, with a notify
 * signal wired to write back to gtkhx_prefs and call the cfgvar's
 * change-callback. Replaces the GtkCheckButton / GtkEntry /
 * GtkSpinButton plumbing the old settings_page_*() functions used to
 * build by hand.
 *
 * Wiring convention: the row owns a "cfgvar" qdata pointer (the same
 * struct cfgvar * the helper looked up). The notify callback reads
 * that, updates *v->variable.X, fires v->changefunc(&the_session)
 * if non-NULL, then prefs_write() so the change persists.
 *
 * No Cancel button — AdwPreferencesWindow is live-apply. Closing the
 * window is the equivalent of "OK", and we save on every change too,
 * so a process crash mid-Settings doesn't lose the last toggle. */

static void
pref_apply (struct cfgvar *v)
{
    if (v->changefunc) {
        v->changefunc (&the_session);
    }
    prefs_write ();
}

static void
on_switch_row_active (AdwSwitchRow *row, GParamSpec *pspec, gpointer data)
{
    struct cfgvar *v = data;
    (void)pspec;
    *v->variable.uchar = adw_switch_row_get_active (row) ? 1 : 0;
    pref_apply (v);
}

static GtkWidget *
pref_switch_row (const char *cfgname, const char *title, const char *subtitle)
{
    struct cfgvar *v = cfgvar_for_name (cfgname);
    GtkWidget *row = adw_switch_row_new ();

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
    if (subtitle && *subtitle) {
        adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
    }
    if (!v || v->type != BOOLEAN) {
        gtk_widget_set_sensitive (row, FALSE);
        return row;
    }
    adw_switch_row_set_active (ADW_SWITCH_ROW (row),
                               *v->variable.uchar ? TRUE : FALSE);
    v->widget = row;
    g_signal_connect (row, "notify::active", G_CALLBACK (on_switch_row_active),
                      v);
    return row;
}

/* per-row debounce for entry-row apply.
 *
 * AdwEntryRow's notify::text fires on every keystroke. For most
 * STRING / STRING32 prefs that's harmless (font name, download dir,
 * etc.) — pref_apply just runs changefunc + prefs_write. But CFG_NICK
 * has changed_nickoricon as its changefunc, which sends an
 * HTLC_HDR_USER_CHANGE on the wire. Typing a 5-letter name produced
 * 5 USER_CHANGE packets, with the server faithfully broadcasting each
 * partial-prefix as the user's name to the rest of the chat.
 *
 * Coalesce: schedule pref_apply on a 750 ms one-shot timer per row.
 * Subsequent keystrokes cancel and re-arm the timer. The user has to
 * stop typing for 750 ms before the apply runs, by which point the
 * full intended value is in the buffer and only one wire packet goes
 * out.
 *
 * The timer ID is stashed on the widget via g_object_set_data so we
 * don't need a parallel hash table — the row's lifetime owns it.
 *
 * close_options_bookkeeping flushes pending timers (running
 * pref_apply once for any cfgvar that still has work queued) so a
 * window close mid-keystroke doesn't lose the change. Quick toggles
 * for non-debounced rows still go straight through the regular
 * pref_apply paths in on_switch_row_active / on_spin_row_value /
 * on_combo_row_selected.
 */
#define ENTRY_APPLY_DEBOUNCE_MS 750
#define ENTRY_TIMER_KEY "gtkhx-entry-apply-timer"

static gboolean
entry_apply_timeout_cb (gpointer data)
{
    struct cfgvar *v = data;
    if (v->widget) {
        g_object_set_data (G_OBJECT (v->widget), ENTRY_TIMER_KEY,
                           GUINT_TO_POINTER (0));
    }
    pref_apply (v);
    return G_SOURCE_REMOVE;
}

static void
entry_apply_schedule (struct cfgvar *v)
{
    guint old;
    if (!v->widget) {
        pref_apply (v);
        return;
    }
    old = GPOINTER_TO_UINT (
        g_object_get_data (G_OBJECT (v->widget), ENTRY_TIMER_KEY));
    if (old) {
        g_source_remove (old);
    }
    guint id
        = g_timeout_add (ENTRY_APPLY_DEBOUNCE_MS, entry_apply_timeout_cb, v);
    g_object_set_data (G_OBJECT (v->widget), ENTRY_TIMER_KEY,
                       GUINT_TO_POINTER (id));
}

static void
entry_apply_flush (struct cfgvar *v)
{
    guint id;
    if (!v->widget) {
        return;
    }
    id = GPOINTER_TO_UINT (
        g_object_get_data (G_OBJECT (v->widget), ENTRY_TIMER_KEY));
    if (!id) {
        return;
    }
    g_source_remove (id);
    g_object_set_data (G_OBJECT (v->widget), ENTRY_TIMER_KEY,
                       GUINT_TO_POINTER (0));
    pref_apply (v);
}

static void
on_entry_row_text (AdwEntryRow *row, GParamSpec *pspec, gpointer data)
{
    struct cfgvar *v = data;
    const char *txt = gtk_editable_get_text (GTK_EDITABLE (row));
    (void)pspec;

    if (!txt) {
        txt = "";
    }

    switch (v->type) {
    case STRING:
        if (*v->variable.str && strcmp (*v->variable.str, txt) == 0) {
            return;
        }
        if (v->allocated) {
            g_free (*v->variable.str);
        }
        *v->variable.str = g_strdup (txt);
        v->allocated = 1;
        break;
    case STRING32:
        if (strncmp (v->variable.str32, txt, 31) == 0) {
            return;
        }
        {
            char lbl[64];
            g_snprintf (lbl, sizeof lbl, "entry_row %s", v->name);
            debug_log_name_write (lbl, txt, strlen (txt));
        }
        strncpy (v->variable.str32, txt, 31);
        v->variable.str32[31] = '\0';
        break;
    default:
        return;
    }
    entry_apply_schedule (v);
}

static GtkWidget *
pref_entry_row (const char *cfgname, const char *title)
{
    struct cfgvar *v = cfgvar_for_name (cfgname);
    GtkWidget *row = adw_entry_row_new ();

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
    if (!v || (v->type != STRING && v->type != STRING32)) {
        gtk_widget_set_sensitive (row, FALSE);
        return row;
    }

    if (v->type == STRING) {
        gtk_editable_set_text (GTK_EDITABLE (row),
                               *v->variable.str ? *v->variable.str : "");
    } else {
        gtk_editable_set_text (GTK_EDITABLE (row), v->variable.str32);
    }

    v->widget = row;
    g_signal_connect (row, "notify::text", G_CALLBACK (on_entry_row_text), v);
    return row;
}

static void
on_spin_row_value (AdwSpinRow *row, GParamSpec *pspec, gpointer data)
{
    struct cfgvar *v = data;
    double val = adw_spin_row_get_value (row);
    (void)pspec;

    switch (v->type) {
    case INT: {
        int n = (int)val;
        if (n == *v->variable.integer) {
            return;
        }
        *v->variable.integer = n;
        break;
    }
    case UINT16: {
        guint16 n = (guint16)val;
        if (n == *v->variable.uint16) {
            return;
        }
        *v->variable.uint16 = n;
        break;
    }
    default:
        return;
    }
    pref_apply (v);
}

static GtkWidget *
pref_spin_row (const char *cfgname, const char *title, const char *subtitle,
               double min, double max, double step)
{
    struct cfgvar *v = cfgvar_for_name (cfgname);
    GtkWidget *row = adw_spin_row_new_with_range (min, max, step);
    double initial = 0;

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
    if (subtitle && *subtitle) {
        adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
    }
    if (!v || (v->type != INT && v->type != UINT16)) {
        gtk_widget_set_sensitive (row, FALSE);
        return row;
    }

    initial = (v->type == INT) ? *v->variable.integer : *v->variable.uint16;
    adw_spin_row_set_value (ADW_SPIN_ROW (row), initial);
    v->widget = row;
    g_signal_connect (row, "notify::value", G_CALLBACK (on_spin_row_value), v);
    return row;
}

/* Colored-Nicknames Settings row. Adds an AdwActionRow
 * with a GtkColorDialogButton suffix (the picker itself) and a
 * Clear button that resets to HX_NICK_COLOR_NONE / theme
 * default. */

static void
on_nick_color_changed (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    struct cfgvar *v = user_data;
    const GdkRGBA *rgba;
    (void) pspec;

    rgba = gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (obj));
    if (!rgba) {
        return;
    }
    /* Pack as 0x00RRGGBB per fogWraith spec — high byte reserved. */
    guint8 r = (guint8) (rgba->red * 255.0 + 0.5);
    guint8 g = (guint8) (rgba->green * 255.0 + 0.5);
    guint8 b = (guint8) (rgba->blue * 255.0 + 0.5);
    int packed = (int) (((guint32) r << 16) | ((guint32) g << 8) | (guint32) b);
    if (v && v->type == INT && *v->variable.integer != packed) {
        *v->variable.integer = packed;
        /* Route through pref_apply so the changefunc receives the
		 * session pointer the other rows pass and prefs_write lands
		 * on the same code path. Without this the changefunc gets
		 * NULL — currently changed_nick_color ignores its arg, but
		 * future logic could need the session. */
        pref_apply (v);
    }
}

static void
on_nick_color_clear (GtkButton *btn, gpointer user_data)
{
    struct cfgvar *v = user_data;
    GtkColorDialogButton *picker
        = g_object_get_data (G_OBJECT (btn), "pref-color-picker");
    (void)btn;
    if (!v || v->type != INT) {
        return;
    }
    if (*v->variable.integer == -1) {
        return;
    }
    *v->variable.integer = -1;
    /* Reset the picker swatch to black so the user gets a clear
	 * "no color is set" visual cue. We block the notify::rgba
	 * signal around the call so the synthetic set doesn't fight
	 * the clear (it would otherwise pack 0x000000 back into the
	 * pref). */
    if (picker) {
        GdkRGBA black = { 0, 0, 0, 1.0 };
        g_signal_handlers_block_by_func (picker, on_nick_color_changed, v);
        gtk_color_dialog_button_set_rgba (picker, &black);
        g_signal_handlers_unblock_by_func (picker, on_nick_color_changed, v);
    }
    /* Same pref_apply routing as on_nick_color_changed, for the
	 * same reasons (consistent session-arg + persistence path). */
    pref_apply (v);
}

static GtkWidget *
pref_nick_color_row (void)
{
    struct cfgvar *v = cfgvar_for_name (CFG_NICK_COLOR);
    GtkColorDialog *dialog;
    GtkWidget *picker;
    GtkWidget *row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                   _ ("Nickname color"));
    adw_action_row_set_subtitle (
        ADW_ACTION_ROW (row),
        _ ("Optional RGB color shown on servers that support the "
           "Colored-Nicknames extension"));

    if (!v || v->type != INT) {
        gtk_widget_set_sensitive (row, FALSE);
        return row;
    }

    /* Picker. GtkColorDialogButton (4.10+) supersedes the
	 * deprecated GtkColorButton; presents a GtkColorDialog
	 * when the user clicks the swatch and exposes the picked
	 * colour via the `rgba` property. We don't need to keep a
	 * separate ref on the dialog — the button retains it
	 * internally for the widget's lifetime. */
    dialog = gtk_color_dialog_new ();
    gtk_color_dialog_set_title (dialog, _ ("Pick Nickname Color"));
    picker = gtk_color_dialog_button_new (dialog);

    if (*v->variable.integer != -1) {
        guint32 packed = (guint32) *v->variable.integer;
        GdkRGBA rgba = { ((packed >> 16) & 0xff) / 255.0,
                         ((packed >> 8) & 0xff) / 255.0,
                         (packed & 0xff) / 255.0, 1.0 };
        gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (picker),
                                          &rgba);
    }
    gtk_widget_set_valign (picker, GTK_ALIGN_CENTER);
    /* The GtkColorButton "color-set" signal was a per-pick
	 * notification; on GtkColorDialogButton the equivalent is
	 * the notify::rgba property change — fires whenever the
	 * picked colour actually changes, which is the behaviour
	 * we want here. */
    g_signal_connect (picker, "notify::rgba",
                      G_CALLBACK (on_nick_color_changed), v);

    GtkWidget *clear = gtk_button_new_with_label (_ ("Clear"));
    gtk_widget_set_valign (clear, GTK_ALIGN_CENTER);
    g_object_set_data (G_OBJECT (clear), "pref-color-picker", picker);
    g_signal_connect (clear, "clicked", G_CALLBACK (on_nick_color_clear), v);

    adw_action_row_add_suffix (ADW_ACTION_ROW (row), picker);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), clear);
    v->widget = picker;
    return row;
}

static void
on_combo_row_selected (AdwComboRow *row, GParamSpec *pspec, gpointer data)
{
    struct cfgvar *v = data;
    GtkStringList *list;
    guint idx;
    const char *selected;
    (void)pspec;

    if (v->type != STRING) {
        return;
    }

    list = GTK_STRING_LIST (
        g_object_get_data (G_OBJECT (row), "pref-combo-values"));
    idx = adw_combo_row_get_selected (row);
    selected = list ? gtk_string_list_get_string (list, idx) : NULL;
    if (!selected) {
        return;
    }

    if (*v->variable.str && strcmp (*v->variable.str, selected) == 0) {
        return;
    }
    if (v->allocated) {
        g_free (*v->variable.str);
    }
    *v->variable.str = g_strdup (selected);
    v->allocated = 1;
    pref_apply (v);
}

/* AdwComboRow with a fixed value list. `values[]` are the strings
 * stored in the cfgvar; `labels[]` are user-visible (translatable)
 * presentation. n is the number of entries; arrays are not freed. */
static GtkWidget *
pref_combo_row (const char *cfgname, const char *title, const char **values,
                const char **labels, int n)
{
    struct cfgvar *v = cfgvar_for_name (cfgname);
    GtkWidget *row = adw_combo_row_new ();
    GtkStringList *labels_model;
    GtkStringList *values_model;
    int i, selected = 0;
    const char *current;

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
    if (!v || v->type != STRING) {
        gtk_widget_set_sensitive (row, FALSE);
        return row;
    }

    labels_model = gtk_string_list_new (NULL);
    values_model = gtk_string_list_new (NULL);
    for (i = 0; i < n; i++) {
        gtk_string_list_append (labels_model, labels[i]);
        gtk_string_list_append (values_model, values[i]);
    }
    adw_combo_row_set_model (ADW_COMBO_ROW (row), G_LIST_MODEL (labels_model));
    g_object_set_data_full (G_OBJECT (row), "pref-combo-values", values_model,
                            g_object_unref);
    g_object_unref (labels_model);

    current = *v->variable.str ? *v->variable.str : "";
    for (i = 0; i < n; i++) {
        if (strcmp (current, values[i]) == 0) {
            selected = i;
            break;
        }
    }
    adw_combo_row_set_selected (ADW_COMBO_ROW (row), selected);
    v->widget = row;
    g_signal_connect (row, "notify::selected",
                      G_CALLBACK (on_combo_row_selected), v);
    return row;
}

void
init_variables (void) /* default settings if prefs file is not found. */
{
    gtkhx_prefs.font = g_strdup ("Monospace 10");
    gtkhx_font_desc = pango_font_description_from_string (gtkhx_prefs.font);
    (*cfgvar_for_name (CFG_FONT)).allocated = 1;

    /* GdkRGBA defaults — light grey foreground on black,
	 * preserving the historic 0xcccc/0xffff fraction. */
    fg_col.red = 0xcccc / 65535.0;
    fg_col.green = 0xcccc / 65535.0;
    fg_col.blue = 0xcccc / 65535.0;
    fg_col.alpha = 1.0;
    bg_col.red = 0.0;
    bg_col.green = 0.0;
    bg_col.blue = 0.0;
    bg_col.alpha = 1.0;

    changed_case (NULL);

    parse_tracker (NULL);

    /* Tray icon defaults to ON. The runtime is a no-op without an SNI
	 * host (e.g. stock GNOME Wayland minus AppIndicator extension),
	 * so leaving it on by default doesn't hurt environments that
	 * can't render it. */
    gtkhx_prefs.tray = 1;

    /* Notification defaults match the HexChat convention: the
	 * high-signal events (mentions, PMs, invites) are on; the
	 * noisy ones (every chat line, every news post) are off so a
	 * fresh install doesn't immediately spam the user. They can
	 * dial each event up or down individually in Settings →
	 * Notifications.
	 *
	 * notify_omit_focused defaults ON so a notification only
	 * fires when the relevant window doesn't already have the
	 * user's attention. */
    gtkhx_prefs.notify_chat = 0;
    gtkhx_prefs.notify_chat_highlight = 1;
    gtkhx_prefs.notify_msg = 1;
    gtkhx_prefs.notify_pchat = 1;
    gtkhx_prefs.notify_pchat_highlight = 1;
    gtkhx_prefs.notify_pchat_invite = 1;
    gtkhx_prefs.notify_news = 0;
    gtkhx_prefs.notify_xfer = 1;
    gtkhx_prefs.notify_broadcast = 1;
    gtkhx_prefs.notify_omit_focused = 1;

    /* Voice device defaults: empty string === "use system default
	 * via autoaudiosrc / autoaudiosink". The Rust runtime side
	 * normalises NULL and "" identically, so we just allocate an
	 * empty string for the prefs round-trip. cfgvar_for_name's
	 * allocated flag tells the read/write path the string is
	 * heap-owned. */
    gtkhx_prefs.voice_input_device = g_strdup ("");
    (*cfgvar_for_name (CFG_VOICE_INPUT_DEVICE)).allocated = 1;
    gtkhx_prefs.voice_output_device = g_strdup ("");
    (*cfgvar_for_name (CFG_VOICE_OUTPUT_DEVICE)).allocated = 1;

    /* Push-to-talk defaults: feature off, no key. Same allocated-
     * string convention as the device prefs so prefs_write knows
     * the buffer is heap-owned. */
    gtkhx_prefs.voice_ptt_enabled = 0;
    gtkhx_prefs.voice_ptt_key = g_strdup ("");
    (*cfgvar_for_name (CFG_VOICE_PTT_KEY)).allocated = 1;

    start_time = time (NULL);
}

static void
prefs_allocate (char *tag, char *rest)
{
    struct cfgvar *result;
    result = bsearch (tag, cfgvars, sizeof (cfgvars) / sizeof (cfgvars[0]),
                      sizeof (cfgvars[0]), cfgnamecmp_const);

    if (!result) {
        return;
    }

    switch (result->type) {
    case INT: {
        int i = atoi (rest);
        if (i == *result->variable.integer) {
            return;
        }
        *result->variable.integer = i;
        break;
    }
    case BOOLEAN: {
        /* prefs_write emits booleans via
			 * g_key_file_set_boolean which writes the literal
			 * "true" / "false" — but the historical parser only
			 * accepted '0'/'1' and silently fell through on
			 * anything else, reverting every BOOLEAN pref to its
			 * struct-init default on every startup. The fix
			 * accepts both spellings, case-insensitively, plus
			 * "yes"/"no" since GKeyFile's own get_boolean does
			 * too. The parser logic now lives in prefs_parser.c
			 * so the unit tests can drive it without a GTK build. */
        unsigned char c;
        if (!prefs_parse_boolean (rest, &c)) {
            return;
        }
        if (*result->variable.uchar == c) {
            return;
        }
        *result->variable.uchar = c;
        break;
    }
    case STRING:
        if (!*result->variable.str || strcmp (rest, *result->variable.str) != 0) {
            if (result->allocated) {
                g_free (*result->variable.str);
            }
            *result->variable.str = g_strdup (rest);
            result->allocated = 1;
            break;
        }
        return;
    case TIME_T: {
        time_t t = atol (rest);
        if (t == *result->variable.timet) {
            return;
        }
        *result->variable.timet = t;
        break;
    }
    case STRING32: {
        gsize rest_len = strlen (rest);
        /* trace every STRING32 load so the htlc->name
			 * corruption hunt has a full audit trail of what came
			 * out of gtkhxrc before any sanitisation. The label
			 * encodes the key name so we can disambiguate NICK
			 * from any future STRING32. */
        {
            char lbl[64];
            g_snprintf (lbl, sizeof lbl, "prefs_load %s", result->name);
            debug_log_name_write (lbl, rest, rest_len);
        }
        /* Defend against corrupt non-UTF-8 bytes in the prefs
			 * file. NICK lives in a STRING32 buffer that doubles
			 * as htlc->name on the wire AND as the source for a
			 * GtkEntry in Settings. GTK's input method context
			 * asserts the surrounding text is valid UTF-8
			 * (gtk_im_context_set_surrounding_with_selection),
			 * so a NICK with non-UTF-8 bytes makes the field
			 * un-editable. Validate UTF-8 here and pipe through
			 * gtkhx_text_to_utf8 (Mac Roman → UTF-8, then
			 * g_utf8_make_valid replacement-char fallback) if
			 * the bytes aren't already valid. */
        if (g_utf8_validate (rest, rest_len, NULL)) {
            if (!strncmp (result->variable.str32, rest, 31)) {
                return;
            }
            strncpy (result->variable.str32, rest, 31);
            result->variable.str32[31] = '\0';
        } else {
            gchar *clean = gtkhx_text_to_utf8 (rest, rest_len, NULL);
            if (!strncmp (result->variable.str32, clean, 31)) {
                g_free (clean);
                return;
            }
            strncpy (result->variable.str32, clean, 31);
            result->variable.str32[31] = '\0';
            g_free (clean);
        }
        break;
    }
    case UINT16: {
        guint16 g = (guint16)atoi (rest);
        if (g == *result->variable.uint16) {
            return;
        }
        *result->variable.uint16 = g;
        break;
    }
    }

    if (result->changefunc) {
        (*(result->changefunc)) (&the_session);
    }
}

static void
parse_line (char *line)
{
    char *rest = 0, *p;

    /* Change any '#' to a null char. We aren't concerned about comments. */
    /* But if a delimeter is found, handle that. */
    for (p = line; *p; ++p) {
        if (*p == '#') {
            *p = '\0';
            break;
        }
        /*else */ if (*p == '=' && !rest) {
            /* separate to distinct strings */
            *p = '\0';
            rest = p + 1;
        }
    }
    if (!rest) {
        return; /* No delimeter? Forget it! */
    }

    prefs_allocate (line, rest);
}

static size_t
read_line (FILE *prefs, char **line, size_t *len)
{
    size_t pos = 0;
    while (fgets ((*line) + pos, *len - pos, prefs)) {
        size_t chunklen = strlen ((*line) + pos);
        pos += chunklen;
        if (!chunklen || (*line)[pos - 1] == '\n') {
            if (pos) {
                (*line)[pos - 1] = '\0';
            }

            return pos;
        }
        *len += 256;
        *line = g_realloc (*line, *len);
    }
    return 0;
}

/* prefs path resolution. Primary location is
 *   $CONFIG/gtkhxrc
 * (where $CONFIG is gtkhx_config_dir()). Fall back to the legacy
 * ~/.gtkhxrc on first run so existing users don't lose their config —
 * subsequent saves go to the new path, and the legacy file is left
 * alone for the user to clean up themselves. */
static char *
prefs_primary_path (void)
{
    return g_build_filename (gtkhx_config_dir (), "gtkhxrc", NULL);
}

static char *
prefs_legacy_path (void)
{
    const char *home = g_getenv ("HOME");
    if (!home || !*home) {
        home = g_get_home_dir ();
    }
    if (!home) {
        return NULL;
    }
    return g_build_filename (home, ".gtkhxrc", NULL);
}

/* read a GKeyFile [gtkhx] section, feeding each entry through
 * prefs_allocate. Reuses the legacy parser's type dispatch — no
 * per-cfgvar plumbing change. Returns TRUE if the keyfile parsed
 * cleanly (whether or not the section was empty). The section name
 * ("gtkhx") lives in cfgkeys.h as CFG_KEYFILE_GROUP. */
static gboolean
prefs_read_keyfile (const char *path)
{
    GKeyFile *kf;
    GError *err = NULL;
    gchar **keys;
    gsize i, n_keys;

    kf = g_key_file_new ();
    if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_KEEP_COMMENTS, &err)) {
        g_key_file_free (kf);
        g_error_free (err);
        return FALSE;
    }

    keys = g_key_file_get_keys (kf, CFG_KEYFILE_GROUP, &n_keys, &err);
    if (!keys) {
        /* No [gtkhx] section — almost certainly a legacy KEY=VALUE
		 * file we just got lucky parsing. Fall through to the
		 * line-by-line parser. */
        g_clear_error (&err);
        g_key_file_free (kf);
        return FALSE;
    }

    for (i = 0; i < n_keys; i++) {
        gchar *value
            = g_key_file_get_value (kf, CFG_KEYFILE_GROUP, keys[i], NULL);
        if (value) {
            prefs_allocate (keys[i], value);
            g_free (value);
        }
    }

    g_strfreev (keys);
    g_key_file_free (kf);
    return TRUE;
}

/* Legacy KEY=VALUE line-by-line reader. Used as a fallback when the
 * file at the primary path turned out not to be a GKeyFile (because
 * it's the pre-migration format) and for reading the legacy
 * ~/.gtkhxrc on first run after upgrade. */
static gboolean
prefs_read_legacy_lines (const char *path)
{
    FILE *prefs = fopen (path, "r");
    char *prefsline;
    size_t prefslinelen = 256;

    if (!prefs) {
        return FALSE;
    }

    prefsline = g_malloc (prefslinelen);
    while (read_line (prefs, &prefsline, &prefslinelen)) {
        parse_line (prefsline);
    }

    g_free (prefsline);
    fclose (prefs);
    return TRUE;
}

/* prefs_read intentionally does not run cfgvar changefuncs
 * (see comment on changed_nickoricon for the reasoning around the
 * wire-packet path). Most cfgvars don't need application at load —
 * the changefunc just propagates the value to a derived runtime
 * structure that's already read directly from gtkhx_prefs.* anyway.
 *
 * The xtext autocopy_* knobs are different: xtext keeps its own
 * static `prefs` struct (xtext.c) that the drag-end code reads, and
 * we only sync gtkhx_prefs → that struct via the setters. Without an
 * explicit apply at load, the runtime values would default to 0 even
 * if the user had set them in gtkhxrc.
 *
 * Call the setters once at the end of every prefs_read path. */
static void
apply_loaded_xtext_prefs (void)
{
    /* Colored-Nicknames extension. Stamp htlc->nick_color
	 * from the loaded pref so the first hx_change_name_icon (fired
	 * at login) carries our color. Same load-vs-changefunc concern
	 * as the xtext autocopy_* knobs below: the cfgvars changefunc
	 * doesn't fire on prefs_read, so without an explicit copy here
	 * htlc->nick_color stays at network.c's HX_NICK_COLOR_NONE
	 * default and we'd silently never advertise. */
    the_session.htlc.nick_color = (guint32)gtkhx_prefs.nick_color;

    gtk_xtext_set_autocopy_text (gtkhx_prefs.autocopy_text);
    gtk_xtext_set_autocopy_stamp (gtkhx_prefs.autocopy_stamp);
    gtk_xtext_set_autocopy_color (gtkhx_prefs.autocopy_color);
    /* Stamp format is widget-aware but the module-global it stashes
	 * into is read by xtext_get_stamp_str. Pass NULL for the widget
	 * here — at this point no xtext widgets exist yet (chat windows
	 * are constructed AFTER prefs_read in fe_init). The recompute-
	 * stamp_width / re-grow-indent paths inside the setter are
	 * widget-conditioned, so passing NULL is a clean
	 * format-only update. */
    gtk_xtext_set_stamp_format (NULL, gtkhx_prefs.stamp_format);
}

void
prefs_read (void)
{
    char *path = prefs_primary_path ();

    /* Try the new GKeyFile format first. */
    if (g_file_test (path, G_FILE_TEST_EXISTS)) {
        if (!prefs_read_keyfile (path)) {
            /* File exists but isn't a GKeyFile — must be the
			 * pre-migration KEY=VALUE format sitting at the new
			 * path. Read it via the legacy line parser; the next
			 * prefs_write will rewrite it as GKeyFile. */
            if (!prefs_read_legacy_lines (path)) {
                fprintf (stderr, "prefs_read: %s: %s\n", path,
                         strerror (errno));
                fflush (stderr);
            }
        }
        g_free (path);
        apply_loaded_xtext_prefs ();
        return;
    }

    /* New-style file doesn't exist; try the legacy ~/.gtkhxrc as a
	 * migration read so existing users don't lose their config. */
    {
        char *legacy = prefs_legacy_path ();
        if (legacy) {
            if (g_file_test (legacy, G_FILE_TEST_EXISTS)) {
                g_message ("Migrating prefs from %s to %s on next save", legacy,
                           path);
                prefs_read_legacy_lines (legacy);
                g_free (legacy);
                g_free (path);
                apply_loaded_xtext_prefs ();
                return;
            }
            g_free (legacy);
        }
    }

    /* No prefs anywhere — first run; pop the prefs dialog. */
    create_options_window (NULL, NULL);
    apply_loaded_xtext_prefs ();
    g_free (path);
}

void
prefs_write (void)
{
    char *path = prefs_primary_path ();
    GKeyFile *kf;
    GError *err = NULL;
    time_t now;
    int i;

    now = time (NULL);
    total_time += (now - start_time);
    start_time = now;

    kf = g_key_file_new ();
    g_key_file_set_comment (kf, NULL, NULL,
                            " GtkHx preferences (GKeyFile format).\n"
                            " Edit values under [" CFG_KEYFILE_GROUP
                            "] or use Settings.",
                            NULL);

    for (i = 0; i != (int)(sizeof (cfgvars) / sizeof (cfgvars[0])); ++i) {
        struct cfgvar *v = &cfgvars[i];
        switch (v->type) {
        case UINT16:
            g_key_file_set_integer (kf, CFG_KEYFILE_GROUP, v->name,
                                    (gint)*v->variable.uint16);
            break;
        case STRING:
            g_key_file_set_string (kf, CFG_KEYFILE_GROUP, v->name,
                                   *v->variable.str ? *v->variable.str : "");
            break;
        case INT:
            g_key_file_set_integer (kf, CFG_KEYFILE_GROUP, v->name,
                                    *v->variable.integer);
            break;
        case TIME_T:
            g_key_file_set_int64 (kf, CFG_KEYFILE_GROUP, v->name,
                                  (gint64)*v->variable.timet);
            break;
        case STRING32: {
            /* trace the value about to be persisted. If
			 * the htlc->name corruption has happened between the
			 * explicit write site and this save, the hex here
			 * shows what's actually going to disk. The gtkhxrc
			 * file is the only stable record of the corrupt
			 * state, so log the bytes here too. */
            char lbl[64];
            g_snprintf (lbl, sizeof lbl, "prefs_write %s", v->name);
            debug_log_name_write (lbl, v->variable.str32,
                                  strlen (v->variable.str32));
            g_key_file_set_string (kf, CFG_KEYFILE_GROUP, v->name,
                                   v->variable.str32);
            break;
        }
        case BOOLEAN:
            g_key_file_set_boolean (kf, CFG_KEYFILE_GROUP, v->name,
                                    *v->variable.uchar ? TRUE : FALSE);
            break;
        }
    }

    if (!g_key_file_save_to_file (kf, path, &err)) {
        fprintf (stderr, "prefs_write: %s: %s\n", path,
                 err ? err->message : "unknown error");
        fflush (stderr);
        g_clear_error (&err);
    }

    g_key_file_free (kf);
    g_free (path);
}

static void
parse_tracker (session *sess)
{
    char *com, *trackers = gtkhx_prefs.tracker_str;
    int i;

    if (gtkhx_prefs.tracker) {
        for (i = 0; i != gtkhx_prefs.num_tracker; ++i) {
            g_free (gtkhx_prefs.tracker[i]);
        }
        g_free (gtkhx_prefs.tracker);
        gtkhx_prefs.tracker = NULL;
    }
    gtkhx_prefs.num_tracker = 0;
    if (!*trackers || !*(trackers + 1)) {
        return;
    }
    for (i = 0;; ++i) {
        if (!(com = strchr (trackers, ','))) {
            com = &trackers[strlen (trackers)];
        }
        gtkhx_prefs.num_tracker++;
        gtkhx_prefs.tracker
            = g_realloc (gtkhx_prefs.tracker, (i + 1) * sizeof (char *));
        gtkhx_prefs.tracker[i] = g_malloc (com - trackers + 1);
        memcpy (gtkhx_prefs.tracker[i], trackers, com - trackers);
        gtkhx_prefs.tracker[i][com - trackers] = '\0';
        if (!*com) {
            break;
        }
        trackers = com + 1;
    }
}

/* Re-derive gtkhx_prefs.tracker[] + tracker_str from whatever the
 * GListStore<GtkStringObject *> currently holds. Called from the
 * add / remove handlers below; the result feeds prefs_write +
 * the next hx_tracker_list_async. The serialised tracker_str is
 * comma-separated to match the on-disk gtkhxrc format the
 * cfgvar handles. */
static void
parse_tracker_list (void)
{
    guint n;
    size_t len = 0;
    int i;

    if (gtkhx_prefs.tracker) {
        for (i = 0; i != gtkhx_prefs.num_tracker; ++i) {
            g_free (gtkhx_prefs.tracker[i]);
        }
        g_free (gtkhx_prefs.tracker);
        gtkhx_prefs.tracker = NULL;
    }

    n = tracker_store
            ? g_list_model_get_n_items (G_LIST_MODEL (tracker_store))
            : 0;
    gtkhx_prefs.num_tracker = (int) n;
    gtkhx_prefs.tracker = n ? g_malloc (n * sizeof (char *)) : NULL;
    if ((*cfgvar_for_name (CFG_TRACKER)).allocated) {
        g_free (gtkhx_prefs.tracker_str);
    }
    gtkhx_prefs.tracker_str = g_malloc0 (1);

    for (guint j = 0; j < n; j++) {
        GtkStringObject *so
            = g_list_model_get_item (G_LIST_MODEL (tracker_store), j);
        const char *tracker = gtk_string_object_get_string (so);
        size_t trackersize = strlen (tracker) + 1;
        gtkhx_prefs.tracker_str
            = g_realloc (gtkhx_prefs.tracker_str, len + trackersize + 1);
        if (j) {
            gtkhx_prefs.tracker_str[len] = ',';
            memcpy (gtkhx_prefs.tracker_str + len + 1, tracker, trackersize);
            len++;
        } else {
            memcpy (gtkhx_prefs.tracker_str + len, tracker, trackersize);
        }
        len += trackersize - 1;
        gtkhx_prefs.tracker[j] = g_strdup (tracker);
        g_object_unref (so);
    }
}

/* bookkeeping that runs on every dialog teardown path —
 * Cancel button, OK button (close-on-OK), and the user clicking the
 * window's close X. We attach to GtkWidget::destroy because in GTK 4
 * gtk_window_destroy() does NOT emit close-request: the close-request
 * signal is only fired for user-initiated close attempts (or
 * gtk_window_close()). Hooking destroy catches every path the
 * teardown can take, so options_window never points at a freed
 * GObject the next time create_options_window runs. */
static void
close_options_bookkeeping (GtkWidget *widget, gpointer data)
{
    size_t i;
    (void)widget;
    (void)data;
    options_window = 0;
    g_free (iv);
    iv = NULL;

    /* Tracker page model chain: drop our parallel refs on the
     * store + selection so they don't dangle past the dialog's
     * widget tree teardown. The column view holds its own refs
     * via gtk_column_view_new and disposes them in its own
     * unref chain; we just need to make sure the module-static
     * pointers aren't usable after the dialog is gone. */
    tracker_list = NULL;
    g_clear_object (&tracker_selection);
    g_clear_object (&tracker_store);
    /* per-cfgvar widget pointers are populated as Settings
	 * pages are constructed (pref_switch_row, pref_entry_row, etc.) and
	 * point at AdwPreferencesRow children of the dialog. Once the dialog
	 * tears down those rows are freed; any external caller that later
	 * does `if (v->widget) ...` would dereference garbage. Clear the
	 * pointers so callers like gtkhx_prefs_set_bool() can safely test
	 * v->widget for "Settings is open right now".
	 *
	 * Flush any pending entry-row debounce timers first so a close
	 * mid-keystroke doesn't lose the change — entry_apply_flush runs
	 * pref_apply synchronously when there's a pending timer. */
    for (i = 0; i < sizeof (cfgvars) / sizeof (cfgvars[0]); i++) {
        entry_apply_flush (&cfgvars[i]);
    }
    for (i = 0; i < sizeof (cfgvars) / sizeof (cfgvars[0]); i++) {
        cfgvars[i].widget = NULL;
    }
}

/* Generic public BOOLEAN cfgvar setter, used by per-window UI (e.g.
 * the Tracker case-sensitive toggle) to flip a cfgvar without reaching
 * into options.c internals. If the Settings window is open, we route
 * through the AdwSwitchRow so its visible state stays in lockstep
 * (the row's notify::active handler then writes the variable + calls
 * the change-callback + persists). Otherwise we write the variable
 * directly and run the change-callback ourselves. */
void
gtkhx_prefs_set_bool (const char *name, int value)
{
    struct cfgvar *v = cfgvar_for_name (name);
    int new_val = value ? 1 : 0;

    if (!v || v->type != BOOLEAN) {
        return;
    }
    if (*v->variable.uchar == new_val) {
        return;
    }

    if (v->widget && ADW_IS_SWITCH_ROW (v->widget)) {
        /* The notify::active handler does all the bookkeeping. */
        adw_switch_row_set_active (ADW_SWITCH_ROW (v->widget),
                                   new_val ? TRUE : FALSE);
        return;
    }

    *v->variable.uchar = (unsigned char)new_val;
    pref_apply (v);
}

static void
fontsel_response (GtkDialog *dialog, gint response, gpointer user_data)
{
    GtkWidget *entry = user_data;

    if (response == GTK_RESPONSE_OK) {
        char *font = gtk_font_chooser_get_font (GTK_FONT_CHOOSER (dialog));
        if (font) {
            gtk_editable_set_text (GTK_EDITABLE (entry), font);
            g_free (font);
        }
    }
    gtkhx_widget_destroy (GTK_WIDGET (dialog));
}

static void
create_fontsel (GtkWidget *btn, GtkWidget *entry)
{
    GtkWindow *parent = gtkhx_active_window ();
    GtkWidget *fontsel
        = gtk_font_chooser_dialog_new (_ ("Browse Fonts"), parent);
    (void)btn;

    /* The Settings AdwDialog is presented modal against the main
     * window; without transient_for + modal here, GTK keeps the
     * input grab on Settings and the font chooser receives no
     * keyboard or mouse events until Settings is dismissed. (Also
     * silences "GtkDialog mapped without a transient parent".) */
    gtk_window_set_modal (GTK_WINDOW (fontsel), TRUE);

    if (gtkhx_prefs.font && *gtkhx_prefs.font) {
        gtk_font_chooser_set_font (GTK_FONT_CHOOSER (fontsel),
                                   gtkhx_prefs.font);
    }

    g_signal_connect (fontsel, "response", G_CALLBACK (fontsel_response),
                      entry);

    gtk_window_present (GTK_WINDOW (fontsel));
}

static void
add_tracker (GtkWidget *add, GtkWidget *entry)
{
    const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
    GtkStringObject *so;
    (void)add;

    if (!text || !*text || !tracker_store) {
        return;
    }
    /* GtkStringObject's constructor copies the input — the
     * GListStore then owns one strong ref; we drop our ref after
     * append. */
    so = gtk_string_object_new (text);
    g_list_store_append (tracker_store, so);
    g_object_unref (so);

    gtk_editable_set_text (GTK_EDITABLE (entry), "");
    parse_tracker_list ();
    prefs_write ();
}

static void
remove_tracker (GtkWidget *del, gpointer data)
{
    guint pos;
    (void)del;
    (void)data;

    if (!tracker_store || !tracker_selection) {
        return;
    }
    /* GtkSingleSelection reports GTK_INVALID_LIST_POSITION when no
     * row is selected — bail rather than try to remove position 0
     * by mistake. */
    pos = gtk_single_selection_get_selected (tracker_selection);
    if (pos == GTK_INVALID_LIST_POSITION) {
        return;
    }
    g_list_store_remove (tracker_store, pos);
    parse_tracker_list ();
    prefs_write ();
}

/* Column factory pair for the single "URL" column on the Settings
 * tracker list. setup creates a left-aligned, ellipsised GtkLabel
 * once per recycled list item; bind reads the URL out of the
 * row's GtkStringObject and updates the label. */
static void
tracker_url_setup (GtkSignalListItemFactory *f, GtkListItem *item,
                   gpointer d)
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
tracker_url_bind (GtkSignalListItemFactory *f, GtkListItem *item, gpointer d)
{
    GtkLabel *lbl = GTK_LABEL (gtk_list_item_get_child (item));
    GtkStringObject *so = gtk_list_item_get_item (item);
    (void)f;
    (void)d;
    gtk_label_set_text (lbl, so ? gtk_string_object_get_string (so) : "");
}

/* Tracker page: Add / Remove a GListStore-backed list of URL
 * strings displayed in a GtkColumnView. The column view sits
 * inside a custom AdwPreferencesRow so it lives flush with the
 * other Adw pages rather than as a floating chunk of GTK. */
static void
settings_page_tracker (AdwPreferencesPage *page)
{
    AdwPreferencesGroup *grp;
    GtkWidget *row;
    GtkWidget *vbox, *scroll, *ent_hbox, *btnhbox;
    GtkWidget *lbl, *entry, *add_btn, *remove_btn;
    GtkColumnViewColumn *col;
    GtkListItemFactory *factory;
    int i;

    grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (grp, _ ("Trackers"));
    adw_preferences_group_set_description (
        grp, _ ("Servers polled when the Tracker window opens"));

    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top (vbox, 6);
    gtk_widget_set_margin_bottom (vbox, 6);
    gtk_widget_set_margin_start (vbox, 6);
    gtk_widget_set_margin_end (vbox, 6);

    /* Build the model chain bottom-up: GListStore is the truth;
     * GtkSingleSelection wraps it so the column view has a
     * selection model to render off (and remove_tracker reads
     * the selected position from). The store ref the selection
     * takes is the long-lived one — we drop ours at function
     * end since the selection (and through it the column view)
     * keeps the chain alive for the dialog's lifetime. */
    tracker_store = g_list_store_new (GTK_TYPE_STRING_OBJECT);
    tracker_selection = gtk_single_selection_new (
        G_LIST_MODEL (g_object_ref (tracker_store)));
    gtk_single_selection_set_autoselect (tracker_selection, FALSE);
    gtk_single_selection_set_can_unselect (tracker_selection, TRUE);
    gtk_single_selection_set_selected (tracker_selection,
                                       GTK_INVALID_LIST_POSITION);

    tracker_list = gtk_column_view_new (
        GTK_SELECTION_MODEL (g_object_ref (tracker_selection)));
    gtk_column_view_set_show_column_separators (
        GTK_COLUMN_VIEW (tracker_list), FALSE);
    gtk_column_view_set_show_row_separators (GTK_COLUMN_VIEW (tracker_list),
                                             FALSE);

    factory = gtk_signal_list_item_factory_new ();
    g_signal_connect (factory, "setup", G_CALLBACK (tracker_url_setup), NULL);
    g_signal_connect (factory, "bind", G_CALLBACK (tracker_url_bind), NULL);
    col = gtk_column_view_column_new (_ ("URL"), factory);
    gtk_column_view_column_set_expand (col, TRUE);
    gtk_column_view_column_set_resizable (col, TRUE);
    gtk_column_view_append_column (GTK_COLUMN_VIEW (tracker_list), col);
    g_object_unref (col);

    scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), tracker_list);
    gtk_widget_set_size_request (scroll, -1, 220);
    gtk_box_append (GTK_BOX (vbox), scroll);

    ent_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    lbl = gtk_label_new (_ ("Address:"));
    entry = gtk_entry_new ();
    gtk_widget_set_hexpand (entry, TRUE);
    gtk_box_append (GTK_BOX (ent_hbox), lbl);
    gtk_box_append (GTK_BOX (ent_hbox), entry);
    gtk_box_append (GTK_BOX (vbox), ent_hbox);

    btnhbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign (btnhbox, GTK_ALIGN_END);
    add_btn = gtk_button_new_with_label (_ ("Add"));
    gtk_widget_add_css_class (add_btn, "suggested-action");
    g_signal_connect (add_btn, "clicked", G_CALLBACK (add_tracker), entry);
    remove_btn = gtk_button_new_with_label (_ ("Remove"));
    gtk_widget_add_css_class (remove_btn, "destructive-action");
    /* The remove handler reads tracker_store + tracker_selection
     * directly from module-static state; no per-widget data
     * needed. */
    g_signal_connect (remove_btn, "clicked", G_CALLBACK (remove_tracker),
                      NULL);
    gtk_box_append (GTK_BOX (btnhbox), remove_btn);
    gtk_box_append (GTK_BOX (btnhbox), add_btn);
    gtk_box_append (GTK_BOX (vbox), btnhbox);

    row = adw_preferences_row_new ();
    gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), vbox);
    adw_preferences_group_add (grp, row);

    /* Seed the store from the current pref. GtkStringObject's
     * constructor copies the input; we drop our ref after
     * append (the store keeps one). */
    for (i = 0; i < gtkhx_prefs.num_tracker; i++) {
        GtkStringObject *so = gtk_string_object_new (gtkhx_prefs.tracker[i]);
        g_list_store_append (tracker_store, so);
        g_object_unref (so);
    }

    adw_preferences_page_add (page, grp);

    /* Tracker-specific search option in its own Search group. */
    {
        AdwPreferencesGroup *search_grp
            = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
        adw_preferences_group_set_title (search_grp, _ ("Search"));
        adw_preferences_group_add (
            search_grp,
            pref_switch_row (CFG_TRACKER_CASE,
                             _ ("Case-sensitive tracker search"), NULL));
        adw_preferences_page_add (page, search_grp);
    }
}

/* No Interface page anymore — the new files browser is always a
 * single window. Legacy FILE_SAMEWINDOW prefs are dropped from the
 * cfgvars table; any pre-existing key in an old gtkhxrc is silently
 * ignored on load. */

static void
settings_page_sound (AdwPreferencesPage *page)
{
    AdwPreferencesGroup *master, *events;

    master = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (master, _ ("Sounds"));
    adw_preferences_group_add (
        master,
        pref_switch_row (CFG_SOUNDS_ON, _ ("Play sounds"),
                         _ ("Master switch for chat and transfer alerts")));
    adw_preferences_page_add (page, master);

    events = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (events, _ ("Events"));
    adw_preferences_group_add (
        events, pref_switch_row (CFG_SND_INVITE, _ ("Chat invitation"), NULL));
    adw_preferences_group_add (
        events, pref_switch_row (CFG_SND_CHAT, _ ("Chat message"), NULL));
    adw_preferences_group_add (
        events, pref_switch_row (CFG_SND_ERROR, _ ("Error"), NULL));
    adw_preferences_group_add (
        events, pref_switch_row (CFG_SND_FILE, _ ("Transfer complete"), NULL));
    adw_preferences_group_add (
        events, pref_switch_row (CFG_SND_JOIN, _ ("Join"), NULL));
    adw_preferences_group_add (
        events, pref_switch_row (CFG_SND_LOGIN, _ ("Login"), NULL));
    adw_preferences_group_add (
        events, pref_switch_row (CFG_SND_MSG, _ ("Private message"), NULL));
    adw_preferences_group_add (
        events, pref_switch_row (CFG_SND_NEWS, _ ("News post"), NULL));
    adw_preferences_group_add (
        events, pref_switch_row (CFG_SND_PART, _ ("Leave"), NULL));
    adw_preferences_page_add (page, events);
}

/* Phase 5 follow-up: the old standalone Font page only ever applied
 * to the xtext-based chat / private-message widgets, so it folds into
 * the Chat page as a Font group. SHOWJOIN and OLD_NICKCOMP also live
 * here now (moved from Misc → Behavior) since they're chat-window
 * concerns rather than session-wide misc. */
static void
settings_page_chat (AdwPreferencesPage *page)
{
    AdwPreferencesGroup *output_grp, *font_grp, *behavior_grp;
    GtkWidget *entry_row, *btn;

    output_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (output_grp, _ ("Chat output"));
    adw_preferences_group_add (
        output_grp,
        pref_switch_row (CFG_TIMESTAMP, _ ("Show timestamps"), NULL));
    adw_preferences_group_add (
        output_grp, pref_switch_row (CFG_WORDWRAP, _ ("Word wrap"), NULL));
    adw_preferences_group_add (
        output_grp,
        pref_spin_row (CFG_XBUF_MAX, _ ("Scrollback lines"),
                       _ ("0 keeps unlimited scrollback"), 0, 0xffff, 1));
    adw_preferences_page_add (page, output_grp);

    /* timestamp format. Separate group so the strftime
     * hint can live as a group description without making the
     * Chat-output group feel cluttered. */
    {
        AdwPreferencesGroup *stamp_grp
            = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
        adw_preferences_group_set_title (stamp_grp, _ ("Timestamp format"));
        adw_preferences_group_set_description (
            stamp_grp, _ ("strftime(3) format string. Default: "
                          "\"[%H:%M:%S] \". See `man 3 strftime` for the full "
                          "list of conversion specifiers."));
        adw_preferences_group_add (
            stamp_grp, pref_entry_row (CFG_STAMP_FORMAT, _ ("Format")));
        adw_preferences_page_add (page, stamp_grp);
    }

    /* Highlight words — comma-separated extras to flag in chat
     * (own nick is always implicit so the field stays empty by
     * default). Matched lines render bold red. */
    {
        AdwPreferencesGroup *hl_grp
            = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
        adw_preferences_group_set_title (hl_grp, _ ("Highlight"));
        adw_preferences_group_set_description (
            hl_grp, _ ("Comma-separated words to highlight in chat (in "
                       "addition to your own nick). Matches are case-"
                       "insensitive at word boundaries."));
        adw_preferences_group_add (
            hl_grp, pref_entry_row (CFG_HIGHLIGHT_WORDS, _ ("Words")));
        adw_preferences_page_add (page, hl_grp);
    }

    /* fogWraith chat-history extension (Janus and any
     * future server that implements Capabilities-Chat-History.md).
     * Single spin row for the initial pull count — also used as
     * the per-click Load-older count, with a 50-floor in the
     * click handler so the affordance still works when initial
     * is set to 0. */
    {
        AdwPreferencesGroup *hist_grp
            = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
        adw_preferences_group_set_title (hist_grp, _ ("Chat history"));
        adw_preferences_group_set_description (
            hist_grp, _ ("Servers that implement the chat-history "
                         "extension (e.g. Janus) replay recent chat "
                         "to you on login. 0 disables the initial "
                         "pull; the \"Load older messages\" link in "
                         "chat still works to fetch on demand."));
        adw_preferences_group_add (
            hist_grp,
            pref_spin_row (CFG_CHAT_HISTORY_INITIAL,
                           _ ("Initial messages to fetch"),
                           _ ("Also used as the page size for "
                              "\"Load older messages\""),
                           0, 0xffff, 1));
        adw_preferences_page_add (page, hist_grp);
    }

    font_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (font_grp, _ ("Font"));
    adw_preferences_group_set_description (
        font_grp, _ ("Pango font description, e.g. \"Monospace 11\""));

    entry_row = pref_entry_row (CFG_FONT, _ ("Font"));

    /* Add a Browse button as a suffix on the entry row so users get a
	 * native font picker without leaving the prefs context. */
    btn = gtk_button_new_with_label (_ ("Browse"));
    gtk_widget_set_valign (btn, GTK_ALIGN_CENTER);
    g_signal_connect (btn, "clicked", G_CALLBACK (create_fontsel), entry_row);
    adw_entry_row_add_suffix (ADW_ENTRY_ROW (entry_row), btn);

    adw_preferences_group_add (font_grp, entry_row);
    adw_preferences_page_add (page, font_grp);

    behavior_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (behavior_grp, _ ("Behavior"));
    adw_preferences_group_add (
        behavior_grp,
        pref_switch_row (CFG_SHOWJOIN, _ ("Show join / leave in chat"), NULL));
    adw_preferences_group_add (
        behavior_grp,
        pref_switch_row (CFG_OLD_NICKCOMP, _ ("Old-style nick completion"),
                         _ ("Match against the most recently typed prefix "
                            "instead of all users")));
    adw_preferences_page_add (page, behavior_grp);

    /* HexChat-style auto-copy controls. Three independent
     * toggles drive xtext's drag-end clipboard behaviour. The three
     * gtk_xtext_set_autocopy_* setters take care of propagating the
     * value to the widget; the changefunc on each cfgvar calls the
     * matching setter. */
    {
        AdwPreferencesGroup *autocopy_grp
            = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
        adw_preferences_group_set_title (autocopy_grp,
                                         _ ("Auto Copy Behavior"));
        adw_preferences_group_set_description (
            autocopy_grp,
            _ ("Drag-select in chat / news / private message text "
               "to populate the clipboard. Ctrl-V or middle-click "
               "pastes the selection elsewhere."));
        adw_preferences_group_add (
            autocopy_grp,
            pref_switch_row (CFG_AUTOCOPY_TEXT,
                             _ ("Automatically copy selected text"), NULL));
        adw_preferences_group_add (
            autocopy_grp,
            pref_switch_row (CFG_AUTOCOPY_STAMP,
                             _ ("Automatically include timestamps"), NULL));
        adw_preferences_group_add (
            autocopy_grp,
            pref_switch_row (CFG_AUTOCOPY_COLOR,
                             _ ("Automatically include color information"),
                             NULL));
        adw_preferences_page_add (page, autocopy_grp);
    }
}

/* Phase 5 follow-up: the old standalone General page (just NICK) folds
 * into the Identity page since they're both "who am I to the server"
 * settings. Display name first, then icon ID, then the resource picker.
 *
 * The icon picker is a GtkFlowBox of 64x64 pixel-art icons in a
 * multi-column grid. See icon_flow_child_activated for the click
 * handler. */
static void
settings_page_identity (AdwPreferencesPage *page)
{
    AdwPreferencesGroup *name_grp, *id_grp, *picker_grp;
    GtkWidget *picker_row, *vbox, *scroll, *icon_list, *wide_list;

    /* g_malloc0 — zero-fill the struct so any read of
     * iv->icon_list / nfound / icon_high before they're set later in
     * this function returns 0 / NULL deterministically. The previous
     * g_malloc gave us a struct full of whatever was at that address,
     * which is exactly the kind of "crashes without gdb, runs fine
     * with gdb" Heisenbug glibc's allocator likes to deliver. */
    iv = g_malloc0 (sizeof (struct icon_viewer));

    name_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (name_grp, _ ("Display name"));
    adw_preferences_group_add (name_grp,
                               pref_entry_row (CFG_NICK, _ ("Your name")));
    adw_preferences_group_add (name_grp, pref_nick_color_row ());
    adw_preferences_page_add (page, name_grp);

    id_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (id_grp, _ ("Identity icon"));
    adw_preferences_group_add (
        id_grp,
        pref_spin_row (CFG_ICON, _ ("Icon ID"),
                       _ ("Numeric ID from the loaded icon resource files"), 0,
                       65535, 1));
    adw_preferences_page_add (page, id_grp);

    picker_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (picker_grp, _ ("Available icons"));
    adw_preferences_group_set_description (
        picker_grp, _ ("Click an entry to copy its ID into the field above"));

    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top (vbox, 6);
    gtk_widget_set_margin_bottom (vbox, 6);
    gtk_widget_set_margin_start (vbox, 6);
    gtk_widget_set_margin_end (vbox, 6);

    scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    /* Picker height tuned to ~3 rows of 56px icons + their labels
	 * visible at a time, plus a little extra for the wide-banner
	 * area below the narrow grid. */
    gtk_widget_set_size_request (scroll, -1, 380);

    /* Two flowboxes share one scrolled window so the picker reads
     * as a single unified list: the multi-column grid for narrow
     * icons sits on top, the one-per-row strip of wide banner
     * icons sits directly under it inside the same scroll area. */
    {
        GtkWidget *picker_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);

        icon_list = gtk_flow_box_new ();
        gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (icon_list),
                                         GTK_SELECTION_SINGLE);
        gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (icon_list), TRUE);
        /* Two columns minimum, four maximum. With 56px icons, four
         * columns fits comfortably in the typical settings dialog
         * width without horizontal cramming. */
        gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (icon_list), 2);
        gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (icon_list), 4);
        gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (icon_list), 4);
        gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (icon_list), 4);
        gtk_flow_box_set_sort_func (GTK_FLOW_BOX (icon_list),
                                    icon_picker_sort_cb, NULL, NULL);
        g_signal_connect (icon_list, "child-activated",
                          G_CALLBACK (icon_flow_child_activated), iv);

        wide_list = gtk_flow_box_new ();
        gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (wide_list),
                                         GTK_SELECTION_SINGLE);
        /* homogeneous=FALSE so each child keeps the banner's natural
         * scaled width; 1/1 children per line forces one banner per
         * row regardless of available width. */
        gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (wide_list), FALSE);
        gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (wide_list), 1);
        gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (wide_list), 1);
        gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (wide_list), 4);
        gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (wide_list), 4);
        gtk_flow_box_set_sort_func (GTK_FLOW_BOX (wide_list),
                                    icon_picker_sort_cb, NULL, NULL);
        g_signal_connect (wide_list, "child-activated",
                          G_CALLBACK (icon_flow_child_activated), iv);

        gtk_box_append (GTK_BOX (picker_box), icon_list);
        gtk_box_append (GTK_BOX (picker_box), wide_list);

        gtkhx_widget_set_child (scroll, picker_box);
    }
    gtk_box_append (GTK_BOX (vbox), scroll);

    picker_row = adw_preferences_row_new ();
    gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (picker_row), FALSE);
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (picker_row), FALSE);
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (picker_row), vbox);
    adw_preferences_group_add (picker_grp, picker_row);

    iv->icon_list = icon_list;
    iv->wide_list = wide_list;
    iv->nfound = 0;
    iv->icon_high = 0;

    adw_preferences_page_add (page, picker_grp);
}

/* Notifications page. One row per event class that can
 * fire a desktop notification, plus a global "don't notify when
 * the relevant window is focused" toggle. Mention matching uses
 * the same word list as the chat highlight colouring (own nick
 * + CFG_HIGHLIGHT_WORDS, comma-separated), so what gets
 * highlighted visually is what triggers a notification. */
static void
settings_page_notifications (AdwPreferencesPage *page)
{
    AdwPreferencesGroup *events, *behavior, *mentions;

    events = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (events, _ ("Events"));
    adw_preferences_group_set_description (
        events, _ ("Show a desktop notification when these events happen."));

    adw_preferences_group_add (
        events, pref_switch_row (CFG_NOTIFY_MSG, _ ("Private message"),
                                 _ ("Someone sends you a 1-to-1 message")));
    adw_preferences_group_add (
        events,
        pref_switch_row (CFG_NOTIFY_PCHAT_INVITE, _ ("Private chat invitation"),
                         _ ("Someone invites you to a private chat")));
    adw_preferences_group_add (
        events,
        pref_switch_row (
            CFG_NOTIFY_CHAT_HIGHLIGHT, _ ("Mention in public chat"),
            _ ("Your name or a highlight word appears in a chat message")));
    adw_preferences_group_add (
        events,
        pref_switch_row (
            CFG_NOTIFY_PCHAT_HIGHLIGHT, _ ("Mention in private chat"),
            _ ("Your name or a highlight word appears in a private chat")));
    adw_preferences_group_add (
        events,
        pref_switch_row (CFG_NOTIFY_CHAT, _ ("Every public chat message"),
                         _ ("Noisy — only useful on quiet servers")));
    adw_preferences_group_add (
        events, pref_switch_row (CFG_NOTIFY_PCHAT,
                                 _ ("Every private chat message"), NULL));
    adw_preferences_group_add (
        events, pref_switch_row (CFG_NOTIFY_NEWS, _ ("New news post"), NULL));
    adw_preferences_group_add (
        events,
        pref_switch_row (CFG_NOTIFY_XFER, _ ("File transfer complete"), NULL));
    adw_preferences_group_add (
        events,
        pref_switch_row (CFG_NOTIFY_BROADCAST, _ ("Server broadcast"),
                         _ ("Admin-issued announcement to every user")));

    adw_preferences_page_add (page, events);

    behavior = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (behavior, _ ("Behavior"));
    adw_preferences_group_add (
        behavior,
        pref_switch_row (CFG_NOTIFY_OMIT_FOCUSED,
                         _ ("Don't notify when the relevant window is focused"),
                         _ ("If a chat or private message window is already "
                            "active, don't pop a notification on top of it")));
    adw_preferences_page_add (page, behavior);

    /* Mirror the Chat page's highlight-word entry so users can
     * configure it from either place. Edits in either flow
     * the same gtkhx_prefs.highlight_words string. */
    mentions = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (mentions, _ ("Mention Words"));
    adw_preferences_group_set_description (
        mentions, _ ("Comma-separated. Your nickname is always matched in "
                     "addition to this list. The same list drives chat "
                     "message highlighting."));
    adw_preferences_group_add (
        mentions, pref_entry_row (CFG_HIGHLIGHT_WORDS, _ ("Highlight words")));
    adw_preferences_page_add (page, mentions);
}

/* Phase 8.E: Voice device pickers. Queries gtkhx_voice_list_input_
 * /output_devices at page-build time (no live monitoring yet — a
 * Settings re-open after plugging a new mic re-runs the scan), then
 * builds a "System default" + per-device combo via the existing
 * pref_combo_row machinery. The values array stores stable
 * gst::Device::name()s; the labels array stores display_name()s; the
 * empty-string sentinel is the "use autoaudiosrc/autoaudiosink"
 * choice. cfgvar_for_name's STRING type handles persistence; the
 * change-callbacks (changed_voice_input_device /
 * changed_voice_output_device) push the new value through to the
 * Rust runtime via FFI. */

/* ============================================================ */
/* Push-to-talk: enable toggle + key capture row.                */
/* ============================================================ */

/* Refresh the action row's subtitle to reflect the current bind
 * (or "Not set" when CFG_VOICE_PTT_KEY is empty / unparseable).
 * The visible label is the canonical spec string itself ("F8",
 * "<Control>F12") — that's the same vocabulary the user sees in
 * any GTK / GNOME accelerator UI, so no translation gymnastics.
 *
 * The stored spec is validated via the same parser the runtime
 * hook uses; a corrupt or out-of-vocabulary prefs value reads as
 * "Not set" here so Settings reflects the EFFECTIVE bind (what
 * the runtime would honour) rather than the bytes on disk. */
static void
ptt_row_refresh_subtitle (AdwActionRow *row)
{
    const char *spec = gtkhx_prefs.voice_ptt_key;
    if (spec && *spec
        && hx_voice_ptt_keyspec_parse (spec, NULL, NULL)) {
        adw_action_row_set_subtitle (row, spec);
    } else {
        adw_action_row_set_subtitle (row, _ ("Not set — click to capture"));
    }
}

/* Persist the captured spec into prefs + write the GKeyFile.
 *
 * Mirrors the entry-row STRING-pref update path: gate the free on
 * `v->allocated` (the static initializer leaves voice_ptt_key as
 * a read-only NULL pointer until init_variables runs, and that
 * read-only state must never be g_free'd) and set the bit after
 * the fresh g_strdup so any future write hits the heap-owned
 * branch. */
static void
ptt_save_key_spec (const char *new_spec)
{
    struct cfgvar *v = cfgvar_for_name (CFG_VOICE_PTT_KEY);
    if (v && v->allocated && gtkhx_prefs.voice_ptt_key) {
        g_free (gtkhx_prefs.voice_ptt_key);
    }
    gtkhx_prefs.voice_ptt_key = g_strdup (new_spec ? new_spec : "");
    if (v) {
        v->allocated = 1;
    }
    prefs_write ();
}

/* Key-pressed handler installed on the capture dialog. Returns
 * TRUE for any keyval — we consume every keypress so the user
 * can't accidentally trigger app-wide shortcuts during capture.
 * Valid key → write + close. Escape → close without writing.
 * Anything else → flash an error label and stay open. */
static gboolean
ptt_capture_key_pressed (GtkEventControllerKey *ctrl, guint keyval,
                         guint keycode, GdkModifierType state,
                         gpointer user_data)
{
    AdwAlertDialog *dlg = user_data;
    AdwActionRow *parent_row
        = g_object_get_data (G_OBJECT (dlg), "ptt-parent-row");
    GtkLabel *err_lbl = g_object_get_data (G_OBJECT (dlg), "ptt-err-label");
    (void) ctrl;
    (void) keycode;

    if (keyval == GDK_KEY_Escape
        && (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) == 0) {
        adw_dialog_close (ADW_DIALOG (dlg));
        return TRUE;
    }
    if (!hx_voice_ptt_keyspec_allowed (keyval, state)) {
        /* Show the dialog's error label so the user knows why
         * their key was rejected. The text spells out the
         * vocabulary in one line; localisable. Reveal the
         * label widget too — it's hidden by default so the
         * dialog body doesn't reserve a blank row. */
        if (err_lbl) {
            gtk_label_set_text (
                err_lbl,
                _ ("That key would conflict with chat typing. "
                   "Try a function key (F1–F24), Pause, or a "
                   "Ctrl/Alt-modified combination."));
            gtk_widget_set_visible (GTK_WIDGET (err_lbl), TRUE);
        }
        return TRUE;
    }
    char *spec = hx_voice_ptt_keyspec_canonicalize (keyval, state);
    if (!spec) {
        return TRUE;
    }
    ptt_save_key_spec (spec);
    g_free (spec);
    if (parent_row) {
        ptt_row_refresh_subtitle (parent_row);
    }
    adw_dialog_close (ADW_DIALOG (dlg));
    return TRUE;
}

/* Open the capture dialog. The dialog is a tiny AdwAlertDialog
 * (no buttons; user presses a key or Escape to cancel). */
static void
ptt_open_capture_dialog (AdwActionRow *parent_row)
{
    AdwDialog *dlg = adw_alert_dialog_new (
        _ ("Capture push-to-talk key"),
        _ ("Press the key (or modifier+key combination) you want to "
           "use as your push-to-talk binding.\n\n"
           "Accepted: F1–F24, Pause, Scroll Lock, Insert, Print, "
           "Menu, or any Ctrl/Alt/Super combination with another "
           "key. Plain letters and digits are rejected so they "
           "don't conflict with chat input.\n\n"
           "Press Escape to cancel."));
    /* Inline error label, hidden until a rejected key tries to
     * land — `gtk_widget_set_visible (FALSE)` rather than just an
     * empty label so the dialog body doesn't reserve a blank row
     * underneath the prompt at first paint. AdwAlertDialog doesn't
     * expose its body label directly; we add a GtkLabel via the
     * dialog's "extra-child" slot and toggle visibility from
     * ptt_capture_key_pressed. */
    GtkWidget *err = gtk_label_new (NULL);
    gtk_widget_add_css_class (err, "error");
    gtk_label_set_wrap (GTK_LABEL (err), TRUE);
    gtk_label_set_xalign (GTK_LABEL (err), 0.0);
    gtk_widget_set_visible (err, FALSE);
    adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dlg), err);
    g_object_set_data (G_OBJECT (dlg), "ptt-err-label", err);
    g_object_set_data (G_OBJECT (dlg), "ptt-parent-row", parent_row);

    /* The key controller has to be installed on the dialog widget
     * itself so it sees keys while the dialog has focus. */
    GtkEventController *kctrl = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (kctrl, GTK_PHASE_CAPTURE);
    g_signal_connect (kctrl, "key-pressed",
                      G_CALLBACK (ptt_capture_key_pressed), dlg);
    gtk_widget_add_controller (GTK_WIDGET (dlg), kctrl);

    adw_dialog_present (dlg, GTK_WIDGET (parent_row));
}

/* AdwActionRow's "activated" fires on click + Enter. Open the
 * capture dialog from here so both interaction paths work. */
static void
on_ptt_row_activated (AdwActionRow *row, gpointer user_data)
{
    (void) user_data;
    ptt_open_capture_dialog (row);
}

/* Suffix "Clear" button — wipes the current bind back to "Not set"
 * without opening the capture dialog. Useful for the "I bound the
 * wrong key and want to start over" flow. */
static void
on_ptt_clear_clicked (GtkButton *btn, gpointer user_data)
{
    AdwActionRow *row = user_data;
    (void) btn;
    ptt_save_key_spec ("");
    ptt_row_refresh_subtitle (row);
}

/* Build the AdwPreferencesGroup and add it to the page. The group
 * has two rows: the boolean enable toggle (standard pref_switch_row)
 * and the key capture action row built by hand. */
static void
settings_page_voice_ptt_group (AdwPreferencesPage *page)
{
    AdwPreferencesGroup *grp
        = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (grp, _ ("Push-to-Talk"));
    adw_preferences_group_set_description (
        grp,
        _ ("When enabled, you start muted and unmute by holding the "
           "captured key. Works from any focused widget in the GtkHx "
           "window."));

    /* Enable toggle. */
    adw_preferences_group_add (
        grp,
        pref_switch_row (CFG_VOICE_PTT_ENABLED,
                         _ ("Enable push-to-talk"), NULL));

    /* Key capture row. */
    AdwActionRow *key_row
        = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (key_row),
                                   _ ("PTT key"));
    /* AdwActionRow inherits from GtkListBoxRow; activatable is the
     * row-level "click / Enter fires 'activated'" toggle. */
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (key_row), TRUE);
    g_signal_connect (key_row, "activated",
                      G_CALLBACK (on_ptt_row_activated), NULL);

    /* Suffix Clear button. */
    GtkWidget *clear_btn
        = gtk_button_new_from_icon_name ("edit-clear-symbolic");
    gtk_widget_set_valign (clear_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (clear_btn, _ ("Clear PTT key"));
    gtk_widget_add_css_class (clear_btn, "flat");
    g_signal_connect (clear_btn, "clicked",
                      G_CALLBACK (on_ptt_clear_clicked), key_row);
    adw_action_row_add_suffix (key_row, clear_btn);

    ptt_row_refresh_subtitle (key_row);
    adw_preferences_group_add (grp, GTK_WIDGET (key_row));
    adw_preferences_page_add (page, grp);
}

static void
settings_page_voice (AdwPreferencesPage *page)
{
    AdwPreferencesGroup *devices_grp;
    gtkhx_voice_device_list *inputs = gtkhx_voice_list_input_devices ();
    gtkhx_voice_device_list *outputs = gtkhx_voice_list_output_devices ();
    size_t n_in = inputs ? gtkhx_voice_device_list_len (inputs) : 0;
    size_t n_out = outputs ? gtkhx_voice_device_list_len (outputs) : 0;
    /* +1 for the leading "System default" entry whose value is "". */
    const char **in_vals = g_new (const char *, n_in + 1);
    const char **in_labels = g_new (const char *, n_in + 1);
    const char **out_vals = g_new (const char *, n_out + 1);
    const char **out_labels = g_new (const char *, n_out + 1);
    size_t i;

    in_vals[0] = "";
    in_labels[0] = _ ("System default");
    for (i = 0; i < n_in; i++) {
        in_vals[i + 1] = gtkhx_voice_device_list_name (inputs, i);
        in_labels[i + 1]
            = gtkhx_voice_device_list_display_name (inputs, i);
    }

    out_vals[0] = "";
    out_labels[0] = _ ("System default");
    for (i = 0; i < n_out; i++) {
        out_vals[i + 1] = gtkhx_voice_device_list_name (outputs, i);
        out_labels[i + 1]
            = gtkhx_voice_device_list_display_name (outputs, i);
    }

    devices_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (devices_grp, _ ("Audio Devices"));
    adw_preferences_group_set_description (
        devices_grp,
        _ ("Capture and playback devices for voice chat. "
           "\"System default\" follows your desktop's audio configuration. "
           "Changes take effect the next time you join a voice room."));
    adw_preferences_group_add (
        devices_grp,
        pref_combo_row (CFG_VOICE_INPUT_DEVICE, _ ("Input (microphone)"),
                        in_vals, in_labels, (int)(n_in + 1)));
    adw_preferences_group_add (
        devices_grp,
        pref_combo_row (CFG_VOICE_OUTPUT_DEVICE, _ ("Output (speakers)"),
                        out_vals, out_labels, (int)(n_out + 1)));
    adw_preferences_page_add (page, devices_grp);

    /* pref_combo_row's gtk_string_list_append copies each string into
	 * its own GtkStringList, so freeing the parallel arrays here is
	 * safe. The underlying char* pointers for index >= 1 live as long
	 * as the device list does — we hold those lists alive for the
	 * page's lifetime by stashing them on the page widget so a later
	 * close-then-reopen rebuilds against a fresh scan. */
    g_free (in_vals);
    g_free (in_labels);
    g_free (out_vals);
    g_free (out_labels);
    g_object_set_data_full (G_OBJECT (page), "voice-input-devices",
                            inputs,
                            (GDestroyNotify)gtkhx_voice_device_list_free);
    g_object_set_data_full (G_OBJECT (page), "voice-output-devices",
                            outputs,
                            (GDestroyNotify)gtkhx_voice_device_list_free);

    /* Push-to-talk: toggle + key capture. The toggle binds via the
     * normal cfgvars BOOLEAN flow; the key capture is bespoke
     * because the row's content (subtitle = current bind, with a
     * Clear button) and its interaction (click → capture dialog,
     * Escape → cancel, valid key → write canonical spec back to
     * cfgvars) don't fit any of the generic pref_* row helpers. */
    settings_page_voice_ptt_group (page);
}

/* Misc holds Auto Reply plus the two genuinely cross-cutting
 * behaviours (queue downloads, show pchats at back). Single-page
 * behaviours live with their page (showjoin / old_nickcomp on Chat,
 * tracker_case on Trackers). */
static void
settings_page_misc (AdwPreferencesPage *page)
{
    AdwPreferencesGroup *behavior, *autoreply;

    autoreply = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (autoreply, _ ("Auto Reply"));
    adw_preferences_group_add (
        autoreply,
        pref_switch_row (CFG_AUTOREPLY_ON, _ ("Enable auto reply"), NULL));
    adw_preferences_group_add (
        autoreply, pref_entry_row (CFG_AUTOREPLY_MSG, _ ("Reply message")));
    adw_preferences_page_add (page, autoreply);

    behavior = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (behavior, _ ("Behavior"));
    adw_preferences_group_add (
        behavior,
        pref_switch_row (
            CFG_SHOWBACK, _ ("Show private messages at back"),
            _ ("Don't raise the chat window when a private message arrives")));
    adw_preferences_group_add (
        behavior,
        pref_switch_row (
            CFG_QUEUEDL, _ ("Queue file transfers"),
            _ ("Run downloads one at a time instead of in parallel")));
    adw_preferences_page_add (page, behavior);
}

/* General page consolidates Appearance (theme combo) + Paths
 * (download directory). Both pages were small enough to feel
 * silly as standalone sidebar entries — folding them together gives
 * a tidier first stop in the Settings sidebar. */
static void
settings_page_general (AdwPreferencesPage *page)
{
    AdwPreferencesGroup *appearance_grp, *paths_grp;
    static const char *vals[]
        = { CFG_THEME_SYSTEM, CFG_THEME_LIGHT, CFG_THEME_DARK };
    const char *labels[3];

    labels[0] = _ ("Follow system");
    labels[1] = _ ("Light");
    labels[2] = _ ("Dark");

    appearance_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (appearance_grp, _ ("Appearance"));
    adw_preferences_group_set_description (
        appearance_grp,
        _ ("Color scheme. \"Follow system\" tracks the desktop's "
           "light/dark preference."));
    adw_preferences_group_add (
        appearance_grp,
        pref_combo_row (CFG_THEME, _ ("Theme"), vals, labels, 3));

    /* Theme-file picker. Enumerates built-in themes (default,
     * solarized) plus any user files under
     * $CONFIG/themes/, populates a combo bound to CFG_THEME_NAME.
     * The existing changed_theme_name cfgvar hook calls
     * gtkhx_theme_load_active() on selection, which re-emits
     * GtkhxTheme::changed and repaints every subscriber (buttons,
     * user list, chat xtext) live. */
    {
        g_autoptr (GPtrArray) themes = gtkhx_theme_list_available ();
        guint n = themes->len;
        const char **theme_values = g_new0 (const char *, n);
        const char **theme_labels = g_new0 (const char *, n);
        for (guint i = 0; i < n; i++) {
            GtkhxThemeEntry *e = g_ptr_array_index (themes, i);
            theme_values[i] = e->name;
            theme_labels[i] = e->display;
        }
        adw_preferences_group_add (
            appearance_grp,
            pref_combo_row (CFG_THEME_NAME, _ ("GtkHx theme"),
                            theme_values, theme_labels, (int) n));
        /* pref_combo_row copies the strings into GtkStringList models,
         * so the parallel arrays can go now. The GtkhxThemeEntry
         * strings get freed when the GPtrArray autoptr unwinds at
         * the end of this scope. */
        g_free (theme_values);
        g_free (theme_labels);
    }

    adw_preferences_page_add (page, appearance_grp);

    /* Per-area UI scaling and the chat palette live in the active
     * theme file (THEMENAME → $CONFIG/themes/<name>.ini). A theme
     * editor — scale knobs, color rows, save-as — is a separate
     * later phase; for now the combo above picks an existing theme
     * and edits to a theme's body still mean editing the .ini
     * directly. See gtkhx_theme.{c,h} and
     * docs/theming-file-format.md. */

    paths_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (paths_grp, _ ("Paths"));
    adw_preferences_group_add (
        paths_grp, pref_entry_row (CFG_DOWNLOAD, _ ("Download directory")));
    adw_preferences_page_add (page, paths_grp);

    /* System integration. The tray icon needs a StatusNotifierItem
     * host in the desktop environment — KDE Plasma, Cinnamon, MATE,
     * Budgie and XFCE support it natively; GNOME Shell needs the
     * AppIndicator extension. On a desktop without one, this toggle
     * is effectively inert (the icon registers but nothing renders
     * it). */
    {
        AdwPreferencesGroup *system_grp
            = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
        adw_preferences_group_set_title (system_grp, _ ("System Integration"));
        adw_preferences_group_add (
            system_grp,
            pref_switch_row (CFG_TRAY, _ ("Show tray icon"),
                             _ ("Display a status icon in the system tray. "
                                "Closing the main window hides to tray; click "
                                "the icon to toggle GtkHx's windows.")));
        adw_preferences_page_add (page, system_grp);
    }
}

/* Helper: build a fresh AdwPreferencesPage with title + icon and run the
 * draw_func against it. Centralizes the metadata so adding pages stays
 * a one-liner. */
static void
settings_add_page (AdwPreferencesDialog *dlg, const char *title,
                   const char *icon, void (*draw_func) (AdwPreferencesPage *))
{
    AdwPreferencesPage *page
        = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
    adw_preferences_page_set_title (page, title);
    if (icon) {
        adw_preferences_page_set_icon_name (page, icon);
    }
    if (draw_func) {
        draw_func (page);
    }
    adw_preferences_dialog_add (dlg, page);
}

void
create_options_window (GtkWidget *widget, gpointer data)
{
    AdwPreferencesDialog *dlg;
    GtkWidget *parent;
    session *sess = data;

    (void)widget;

    parent = GTK_WIDGET (gtkhx_active_window ());

    if (options_window) {
        adw_dialog_present (ADW_DIALOG (options_window), parent);
        return;
    }

    /* AdwPreferencesDialog (libadwaita 1.6+) replaces
	 * AdwPreferencesWindow, which became deprecated alongside the
	 * old AdwAboutWindow / AdwMessageDialog when the new adaptive
	 * AdwDialog family arrived. The settings construction is
	 * essentially unchanged — same 9 pages with the same draw
	 * functions — but the outer container is the dialog now,
	 * presented via adw_dialog_present rather than gtk_window_present.
	 *
	 * AdwDialog auto-handles transient_for / modal-against-parent /
	 * proper sizing, so the explicit gtk_window_set_transient_for +
	 * gtk_window_set_modal pair is gone. Default size still pinned
	 * to 840x640 via set_content_width/height — wide enough that the
	 * 9-page top AdwViewSwitcher fits horizontally before libadwaita
	 * adaptively collapses it to a bottom bar. */
    dlg = ADW_PREFERENCES_DIALOG (adw_preferences_dialog_new ());
    adw_dialog_set_title (ADW_DIALOG (dlg), _ ("GtkHx Preferences"));
    adw_dialog_set_content_width (ADW_DIALOG (dlg), 840);
    adw_dialog_set_content_height (ADW_DIALOG (dlg), 640);

    /* Esc closes via AdwDialog's built-in close_response; wire Ctrl+W
	 * (close) and Ctrl+Q (app.quit) for keyboard parity with the
	 * rest of the app. */
    gtkhx_dialog_add_close_shortcuts (GTK_WIDGET (dlg));

    g_object_set_data (G_OBJECT (dlg), "sess", sess);
    g_signal_connect (dlg, "closed", G_CALLBACK (close_options_bookkeeping),
                      NULL);

    options_window = GTK_WIDGET (dlg);

    settings_add_page (dlg, _ ("General"), "preferences-system-symbolic",
                       settings_page_general);
    settings_add_page (dlg, _ ("Identity"), "user-info-symbolic",
                       settings_page_identity);
    settings_add_page (dlg, _ ("Chat"), "user-available-symbolic",
                       settings_page_chat);
    settings_add_page (dlg, _ ("Sound"), "audio-speakers-symbolic",
                       settings_page_sound);
    settings_add_page (dlg, _ ("Voice"), "audio-input-microphone-symbolic",
                       settings_page_voice);
    settings_add_page (dlg, _ ("Notifications"),
                       "preferences-system-notifications-symbolic",
                       settings_page_notifications);
    settings_add_page (dlg, _ ("Trackers"), "network-server-symbolic",
                       settings_page_tracker);
    settings_add_page (dlg, _ ("Misc"), "applications-other-symbolic",
                       settings_page_misc);

    adw_dialog_present (ADW_DIALOG (dlg), parent);

    /* Populate the icon picker now that its hlist exists. list_icons
	 * walks the loaded resource files and inserts a row per icon. */
    list_icons ();
}

G_GNUC_END_IGNORE_DEPRECATIONS
/* end of file-level deprecation suppression — see top of file. */
