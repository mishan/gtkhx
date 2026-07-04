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
#include "gif_icons.h"  /* hx_icon_save / _set / _clear + GIF_ICONS_* state */
#include "gif_avatar.h" /* gtkhx_avatar_set_animation_enabled (10.D pref) */
#include "toolbar.h"    /* toolbar_show_toast */
#include "hotline_proto.h" /* gtkhx_proto_gif_icon_is_gif */
#include "text_util.h"
#include "tracker.h"
#include "debug.h"
#ifdef HAVE_VOICE
#include "voice_runtime.h"
#include "voice_ptt_keyspec.h"
#endif

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

time_t start_time;
time_t total_time;

static struct icon_viewer *iv;

GtkWidget *options_window = NULL;

/* The Tracker settings page moved to Rust (gtkhx-ui options.rs); it owns its
 * own GListStore + GtkColumnView and serialises back through
 * gtkhx_prefs_set_string(CFG_TRACKER, …), whose parse_tracker changefunc
 * re-derives gtkhx_prefs.tracker[]. The former module-static store/selection/
 * view and the add/remove/parse_tracker_list helpers are gone. */

struct gtkhx_prefs gtkhx_prefs = {
    0,                /* num_tracker */
    CFG_THEME_SYSTEM, /* theme: see CFG_THEME_* in cfgkeys.h */
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
    0, /* tray (init_variables sets default) */
    0, /* timestamp */
    0, /* word_wrap */
    1, /* track_case */
    0, /* old_nickcompletion */
    0, /* outrate_limit */
    0, /* inrate_limit */
    0, /* logging */

    /* Emoji shortcodes — both default ON (init_variables re-asserts). */
    1, /* emoji_shortcodes */
    1, /* emoji_typeahead */

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

    1, /* animate_avatars — default ON (init_variables re-asserts) */
};

static void parse_tracker (session *);

struct icon_viewer {
    guint32 icon_high;
    unsigned int nfound;
    GtkWidget *icon_list; /* multi-column flowbox for narrow icons */
    GtkWidget *wide_list; /* one-per-row flowbox for wide banners */
    /* The icon picker is now a popup opened from the Identity page's
     * "Browse…" button rather than an inline group. This holds the live
     * popup AdwDialog (or NULL) so a selection can dismiss it and the
     * Settings teardown can close it. icon_list / wide_list point into
     * this popup's flowboxes while it's open. */
    GtkWidget *picker_dialog;
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
            /* The picker now lives in its own AdwDialog, which the
             * user can close mid-render — yielding above runs its
             * "closed" handler, which nulls iv->icon_list / wide_list.
             * Bail before appending into the now-destroyed flowboxes
             * (also covers the whole Settings dialog closing, which
             * frees iv). winners owns the remaining macres_res entries;
             * destroying the table frees them via the destroy_func. */
            if (!options_window || !iv || iv->icon_list != icon_list) {
                g_hash_table_destroy (winners);
                return;
            }
        }
    }

    g_hash_table_destroy (winners);

    if (nfound >= 2 && options_window && iv && iv->icon_list == icon_list) {
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
 * inert — changing them updated gtkhx_prefs / the session's htlc but
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
    hx_change_name_icon (&hx_active_session ()->htlc);
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
    hx_active_session ()->htlc.nick_color = nc;
    hx_change_name_icon (&hx_active_session ()->htlc);

    /* Locally re-render our own row in the public chat user list.
	 * Pre-login (no uid yet, or no chat container yet) just no-ops —
	 * apply_loaded_xtext_prefs stamps the loaded pref onto htlc, and
	 * the SELFINFO-driven hx_user_new for self picks it up the same
	 * way it picks up the loaded nick. */
    struct chat *pub = chat_with_cid (hx_active_session (), 0);
    if (pub && hx_active_session ()->htlc.uid) {
        struct hx_user *self = hx_user_with_uid (pub, hx_active_session ()->htlc.uid);
        if (self) {
            self->nick_color = nc;
            user_change (&hx_active_session ()->htlc, pub, self, self->name, self->icon,
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

/* changefunc for CFG_EMOJI_SHORTCODES (phase E6). Push the toggle into
 * text_util.c, which both the send encode (gtkhx_text_for_wire) and the
 * receive decode (proto_helpers chat / PM builders) consult. Kept out of
 * those dependency-light translation units' direct gtkhx_prefs reach so
 * their unit tests don't have to link the prefs global. */
static void
changed_emoji_shortcodes (session *sess)
{
    (void)sess;
    gtkhx_text_set_emoji_shortcodes_enabled (gtkhx_prefs.emoji_shortcodes);
}

/* changefunc for CFG_ANIMATE_AVATARS (Phase 10.D). Push the toggle into
 * gif_avatar.c, which starts/stops its frame timer and repaints avatars
 * as either animated or a still first frame. */
static void
changed_animate_avatars (session *sess)
{
    (void)sess;
    gtkhx_avatar_set_animation_enabled (gtkhx_prefs.animate_avatars);
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

#ifdef HAVE_VOICE
/* Settings → Voice → "Input device" combobox. Two steps:
 *
 *   1. Push the pick to the global device preference (DEVICE_PREFS in
 *      the Rust audio module) so any FUTURE VoiceRuntime / send bin is
 *      built against the resolved device. Empty / NULL means "system
 *      default" — autoaudiosrc resolves the host's configured default.
 *   2. If a voice runtime is live for the active session, hot-swap the
 *      capture device NOW by rebuilding the send bin in place (reusing
 *      the existing WebRTC transceiver, no renegotiation) so the change
 *      takes effect immediately rather than only on the next Join. The
 *      global-preference push in step 1 must happen first — the reload
 *      reads that freshly-stored value. */
static void
changed_voice_input_device (session *sess)
{
    gtkhx_voice_set_input_device (gtkhx_prefs.voice_input_device);
    if (sess && sess->voice_runtime) {
        gtkhx_voice_runtime_reload_input_device (sess->voice_runtime);
    }
}

/* Settings → Voice → "Output device" combobox. Same two-step shape as
 * changed_voice_input_device but for the receive (autoaudiosink) side:
 * update the global preference, then hot-swap by rebuilding every live
 * receive bin against the new sink. */
static void
changed_voice_output_device (session *sess)
{
    gtkhx_voice_set_output_device (gtkhx_prefs.voice_output_device);
    if (sess && sess->voice_runtime) {
        gtkhx_voice_runtime_reload_output_device (sess->voice_runtime);
    }
}
#endif /* HAVE_VOICE */

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
    /* GIF-icons (Phase 10.D): animate avatars. Kept first to preserve
	 * the alphabetical key order ("ANIMATEAVATARS" < "AUTOCOPY…") that
	 * the bsearch in this table requires. changefunc pushes the toggle
	 * into gif_avatar.c so a live change starts/stops animation. */
    { CFG_ANIMATE_AVATARS,
      { &gtkhx_prefs.animate_avatars },
      BOOLEAN,
      0,
      changed_animate_avatars,
      NULL },
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
    /* Emoji shortcodes (phase E6). emoji_shortcodes drives both the
	 * legacy-server send encode and the always-on receive decode; its
	 * changefunc pushes the value to the text_util/proto_helpers toggle.
	 * emoji_typeahead drives the inline :prefix popup and is read live in
	 * emoji.c, so it needs no changefunc. */
    { CFG_EMOJI_SHORTCODES,
      { &gtkhx_prefs.emoji_shortcodes },
      BOOLEAN,
      0,
      changed_emoji_shortcodes,
      NULL },
    { CFG_EMOJI_TYPEAHEAD,
      { &gtkhx_prefs.emoji_typeahead },
      BOOLEAN,
      0,
      NULL,
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
      /* Static cfgvar table entries need a compile-time-constant address,
       * so these identity fields (icon/name) bind to the concrete
       * the_session storage rather than the hx_active_session() accessor.
       * Multi-conn reworks prefs<->identity binding — per-connection
       * identity is an open M-phase question; for now this is the one
       * session. */
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
      { the_session.htlc.name },  /* concrete storage — see CFG_ICON note above */
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
    /* Voice join/leave sounds. Kept as pure storage regardless of
     * HAVE_VOICE (like the CFG_VOICE_*_DEVICE prefs above) so a build
     * without voice doesn't drop a user's saved toggles; only the
     * Settings rows are compiled out when voice is absent. Positioned
     * here to keep the table sorted by key string
     * (SOUNDSON < SOUNDVOICEJOIN < SOUNDVOICELEAVE < TASKXSIZE) for
     * the bsearch in cfgvar_for_name. */
    { CFG_SND_VOICE_JOIN, { &hxsnd.voice_join }, BOOLEAN, 0, NULL, NULL },
    { CFG_SND_VOICE_LEAVE, { &hxsnd.voice_leave }, BOOLEAN, 0, NULL, NULL },
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
    /* The device prefs persist in gtkhxrc regardless of whether voice
     * is compiled in (so a build without voice doesn't drop a user's
     * saved picks). The change-callbacks push the value into the Rust
     * runtime, so they only exist when HAVE_VOICE — otherwise the
     * entries carry a NULL changefunc and are pure storage. */
    { CFG_VOICE_INPUT_DEVICE,
      { &gtkhx_prefs.voice_input_device },
      STRING,
      0,
#ifdef HAVE_VOICE
      changed_voice_input_device,
#else
      NULL,
#endif
      NULL },
    { CFG_VOICE_OUTPUT_DEVICE,
      { &gtkhx_prefs.voice_output_device },
      STRING,
      0,
#ifdef HAVE_VOICE
      changed_voice_output_device,
#else
      NULL,
#endif
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
    /* Picking an icon dismisses the Browse popup; the value we just
     * stamped onto the spin row drives the Identity page's inline
     * preview via its notify::value handler. */
    if (iv && iv->picker_dialog) {
        adw_dialog_close (ADW_DIALOG (iv->picker_dialog));
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
 * that, updates *v->variable.X, fires v->changefunc(hx_active_session ())
 * if non-NULL, then prefs_write() so the change persists.
 *
 * No Cancel button — AdwPreferencesWindow is live-apply. Closing the
 * window is the equivalent of "OK", and we save on every change too,
 * so a process crash mid-Settings doesn't lose the last toggle. */

static void
pref_apply (struct cfgvar *v)
{
    if (v->changefunc) {
        v->changefunc (hx_active_session ());
    }
    prefs_write ();
}

/* ---- Rust settings-form bridge (Phase R5) ------------------------
 *
 * The settings *form* is moving to Rust (gtkhx-ui options.rs); the
 * cfgvar registry, the changed_* apply hooks, and the on-disk
 * persistence (prefs_write) stay here. These typed by-name accessors
 * (an extension of the existing gtkhx_prefs_set_bool family) let the
 * Rust rows read a pref's current value and write a new one — a write
 * also fires the cfgvar's changefunc + persists, exactly like the C
 * rows (on_switch_row_active / on_entry_row_text / …) did, so the apply
 * semantics can't drift. BOOLEAN writes reuse gtkhx_prefs_set_bool
 * (below). STRING writes honour the `allocated` bit; both string types
 * short-circuit an unchanged value (matching the C handlers, which skip
 * redundant changefunc runs / wire packets). */
int
gtkhx_prefs_type (const char *name)
{
    struct cfgvar *v = cfgvar_for_name (name);
    return v ? (int) v->type : 0;
}

int
gtkhx_prefs_get_bool (const char *name)
{
    struct cfgvar *v = cfgvar_for_name (name);
    if (!v || v->type != BOOLEAN) {
        return 0;
    }
    return *v->variable.uchar ? 1 : 0;
}

int
gtkhx_prefs_get_int (const char *name)
{
    struct cfgvar *v = cfgvar_for_name (name);
    if (!v) {
        return 0;
    }
    switch (v->type) {
    case INT:
        return *v->variable.integer;
    case UINT16:
        return (int) *v->variable.uint16;
    case TIME_T:
        return (int) *v->variable.timet;
    default:
        return 0;
    }
}

void
gtkhx_prefs_set_int (const char *name, int val)
{
    struct cfgvar *v = cfgvar_for_name (name);
    if (!v) {
        return;
    }
    switch (v->type) {
    case INT:
        *v->variable.integer = val;
        break;
    case UINT16:
        *v->variable.uint16 = (guint16) val;
        break;
    case TIME_T:
        *v->variable.timet = (time_t) val;
        break;
    default:
        return;
    }
    pref_apply (v);
}

/* Returns a g_malloc'd copy (caller frees with g_free); never NULL. */
char *
gtkhx_prefs_get_string (const char *name)
{
    struct cfgvar *v = cfgvar_for_name (name);
    if (!v) {
        return g_strdup ("");
    }
    if (v->type == STRING) {
        return g_strdup (*v->variable.str ? *v->variable.str : "");
    }
    if (v->type == STRING32) {
        return g_strndup (v->variable.str32, 31);
    }
    return g_strdup ("");
}

void
gtkhx_prefs_set_string (const char *name, const char *val)
{
    struct cfgvar *v = cfgvar_for_name (name);
    if (!v || !val) {
        return;
    }
    if (v->type == STRING) {
        if (*v->variable.str && strcmp (*v->variable.str, val) == 0) {
            return;
        }
        if (v->allocated) {
            g_free (*v->variable.str);
        }
        *v->variable.str = g_strdup (val);
        v->allocated = 1;
    } else if (v->type == STRING32) {
        if (strncmp (v->variable.str32, val, 31) == 0) {
            return;
        }
        strncpy (v->variable.str32, val, 31);
        v->variable.str32[31] = '\0';
    } else {
        return;
    }
    pref_apply (v);
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

    /* Emoji shortcodes — both on by default (Mac Roman fallback +
	 * Slack-style typeahead). */
    gtkhx_prefs.emoji_shortcodes = 1;
    gtkhx_prefs.emoji_typeahead = 1;

    /* GIF-icons (Phase 10.D): animate avatars on by default. */
    gtkhx_prefs.animate_avatars = 1;

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
        (*(result->changefunc)) (hx_active_session ());
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
    hx_active_session ()->htlc.nick_color = (guint32)gtkhx_prefs.nick_color;

    gtk_xtext_set_autocopy_text (gtkhx_prefs.autocopy_text);
    gtk_xtext_set_autocopy_stamp (gtkhx_prefs.autocopy_stamp);
    gtk_xtext_set_autocopy_color (gtkhx_prefs.autocopy_color);

    /* Same load-vs-changefunc concern: prefs_read doesn't fire
	 * changefuncs, so push the loaded emoji-shortcode toggle into the
	 * text_util/proto_helpers conversion gate explicitly. (Typeahead is
	 * read live from gtkhx_prefs in emoji.c, so it needs no push.) */
    gtkhx_text_set_emoji_shortcodes_enabled (gtkhx_prefs.emoji_shortcodes);

    /* GIF-icons (Phase 10.D): same concern — push the loaded
	 * animate-avatars toggle into gif_avatar.c so a persisted OFF takes
	 * effect at startup, not only after the user touches the setting. */
    gtkhx_avatar_set_animation_enabled (gtkhx_prefs.animate_avatars);

#ifdef HAVE_VOICE
    /* Voice capture / playback device: same load-vs-changefunc concern.
	 * prefs_read doesn't fire changed_voice_{input,output}_device, so the
	 * loaded device names live in gtkhx_prefs but never reach the Rust
	 * runtime's DEVICE_PREFS. Without this push a saved device is shown
	 * correctly in Settings yet ignored on the first Join after launch —
	 * the send/receive bins fall back to autoaudiosrc/autoaudiosink. Push
	 * both here so a persisted pick actually takes effect at startup, not
	 * only after the user re-touches the setting. (The setters just store
	 * into a Mutex-guarded static; no GStreamer init required, so it's
	 * safe this early in fe_init.) */
    gtkhx_voice_set_input_device (gtkhx_prefs.voice_input_device);
    gtkhx_voice_set_output_device (gtkhx_prefs.voice_output_device);
#endif

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

/* bookkeeping that runs on every dialog teardown path. Wired to
 * AdwDialog::closed (see create_options_window), which AdwDialog emits
 * once the dialog is actually closed — whether by Esc, the header-bar
 * close button, adw_dialog_close(), or the parent window going away.
 * That's the single teardown chokepoint for an AdwDialog (there's no
 * separate confirm-vs-destroy split like GtkWindow's close-request vs
 * destroy), so hooking it catches every path and guarantees
 * options_window never points at a freed GObject the next time
 * create_options_window runs. */
static void
close_options_bookkeeping (GtkWidget *widget, gpointer data)
{
    size_t i;
    (void)widget;
    (void)data;
    options_window = 0;
    /* If the icon Browse popup is still up, dismiss it before we free
     * iv — its "closed" handler (on_icon_picker_closed) nulls the
     * pointers, and the guard there tolerates iv already being NULL. */
    if (iv && iv->picker_dialog) {
        adw_dialog_close (ADW_DIALOG (iv->picker_dialog));
    }
    g_free (iv);
    iv = NULL;

    /* The Tracker page (now Rust) owns its own GListStore + selection; they
     * drop with the dialog's widget tree, so there's nothing to clear here. */

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

/* No Interface page anymore — the new files browser is always a
 * single window. Legacy FILE_SAMEWINDOW prefs are dropped from the
 * cfgvars table; any pre-existing key in an old gtkhxrc is silently
 * ignored on load. */

/* ---- Custom GIF avatar picker (GIF-icons extension, Phase 10.C) --- */

/* GTKHX_AVATAR_MAX_BYTES (the spec-recommended 32 KiB upload cap) is
 * defined in gif_icons.h so the picker and the persistence layer share
 * one source of truth. We reject larger files client-side with a clear
 * message rather than earn a server rejection. (Auto-downscale is
 * deferred: gdk-pixbuf has no GIF encoder, so recompressing to GIF would
 * need ImageMagick.) */

/* Cap the preview decode. GdkPixbufLoader's "size-prepared" fires once
 * the header (dimensions) is parsed but before the full raster decode,
 * so a highly-compressed GIF that advertises huge dimensions gets scaled
 * down to a sane preview size here rather than allocating a giant canvas
 * on the UI thread. (The avatar that's actually sent/rendered still goes
 * through the bounded, sandboxed gif_avatar decoder; this only bounds the
 * local Settings preview.) */
#define AVATAR_PREVIEW_MAX_DIM 512
static void
avatar_loader_size_prepared (GdkPixbufLoader *ld, int w, int h, gpointer data)
{
    (void) data;
    int big = w > h ? w : h;
    if (big <= AVATAR_PREVIEW_MAX_DIM) {
        return;
    }
    double s = (double) AVATAR_PREVIEW_MAX_DIM / big;
    int nw = (int) (w * s);
    int nh = (int) (h * s);
    gdk_pixbuf_loader_set_size (ld, nw > 0 ? nw : 1, nh > 0 ? nh : 1);
}

static void
avatar_preview_from_gif (GtkWidget *preview, const guchar *bytes, gsize len)
{
    GdkTexture *tex = NULL;
    GdkPixbufLoader *ld = gdk_pixbuf_loader_new ();
    g_signal_connect (ld, "size-prepared",
                      G_CALLBACK (avatar_loader_size_prepared), NULL);
    /* close() must run exactly once regardless of how write() fared, so
	 * sequence the two calls into separate statements rather than relying
	 * on && short-circuit (which would skip close() when write() fails,
	 * and tempt a second close() in an else branch). */
    gboolean wrote = gdk_pixbuf_loader_write (ld, bytes, len, NULL);
    gboolean closed = gdk_pixbuf_loader_close (ld, NULL);
    if (wrote && closed) {
        GdkPixbuf *pb = gdk_pixbuf_loader_get_pixbuf (ld); /* borrowed */
        if (pb) {
            tex = gtkhx_texture_from_pixbuf (pb);
        }
    }
    gtk_picture_set_paintable (GTK_PICTURE (preview),
                               tex ? GDK_PAINTABLE (tex) : NULL);
    g_clear_object (&tex);
    g_object_unref (ld);
}

static void
on_avatar_file_chosen (GObject *src, GAsyncResult *res, gpointer user_data)
{
    GtkWidget *preview = user_data; /* reffed by on_avatar_choose_clicked */
    GError *err = NULL;
    GFile *file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (src), res, &err);

    if (!file) {
        if (err
            && !g_error_matches (err, GTK_DIALOG_ERROR,
                                 GTK_DIALOG_ERROR_DISMISSED)) {
            char *m
                = g_strdup_printf (_ ("File picker failed: %s"), err->message);
            toolbar_show_toast (m);
            g_free (m);
        }
        g_clear_error (&err);
        g_object_unref (preview);
        return;
    }

    /* Preflight the size before reading the whole file into memory —
	 * picking a huge file shouldn't cause a long synchronous read +
	 * allocation on the UI thread just to reject it afterward. One
	 * stat() on local files; cheap. If size can't be queried (some
	 * network mounts) we fall through and the post-read cap catches it. */
    GFileInfo *finfo = g_file_query_info (
        file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, NULL,
        NULL);
    if (finfo) {
        goffset sz = g_file_info_get_size (finfo);
        g_object_unref (finfo);
        if (sz > GTKHX_AVATAR_MAX_BYTES) {
            /* Size is checked before the GIF-signature validation below,
			 * so a large non-GIF lands here too — keep the wording neutral. */
            char *m = g_strdup_printf (
                _ ("That file is %.1f KB — the limit is %d KB. Pick a smaller "
                   "one."),
                sz / 1024.0, GTKHX_AVATAR_MAX_BYTES / 1024);
            toolbar_show_toast (m);
            g_free (m);
            g_object_unref (file);
            g_object_unref (preview);
            return;
        }
    }

    char *contents = NULL;
    gsize len = 0;
    GError *load_err = NULL;
    if (!g_file_load_contents (file, NULL, &contents, &len, NULL, &load_err)) {
        char *m = g_strdup_printf (_ ("Couldn't read the file: %s"),
                                   load_err ? load_err->message : "?");
        toolbar_show_toast (m);
        g_free (m);
        g_clear_error (&load_err);
        g_object_unref (file);
        g_object_unref (preview);
        return;
    }
    g_object_unref (file);

    if (len == 0) {
        toolbar_show_toast (_ ("That file is empty."));
        g_free (contents);
        g_object_unref (preview);
        return;
    }
    if (len > GTKHX_AVATAR_MAX_BYTES) {
        char *m = g_strdup_printf (
            _ ("That file is %.1f KB — the limit is %d KB. Pick a smaller "
               "one."),
            len / 1024.0, GTKHX_AVATAR_MAX_BYTES / 1024);
        toolbar_show_toast (m);
        g_free (m);
        g_free (contents);
        g_object_unref (preview);
        return;
    }
    if (!gtkhx_proto_gif_icon_is_gif ((const guint8 *) contents, len)) {
        toolbar_show_toast (_ ("That file isn't a GIF image."));
        g_free (contents);
        g_object_unref (preview);
        return;
    }

    /* Persist the choice regardless of the current connection, then
	 * send it if (and only if) the live server supports the extension.
	 * If not, it'll be sent automatically the next time we connect to a
	 * capable server (hx_icon_send_saved, from the post-login probe). */
    if (!hx_icon_save ((const guint8 *) contents, len)) {
        toolbar_show_toast (
            _ ("Couldn't save the avatar to disk — check permissions."));
        g_free (contents);
        g_object_unref (preview);
        return;
    }
    avatar_preview_from_gif (preview, (const guchar *) contents, len);
    if (hx_active_session ()->htlc.gif_icons_state == GIF_ICONS_SUPPORTED) {
        hx_icon_set (&hx_active_session ()->htlc, (const guint8 *) contents, len);
        toolbar_show_toast (_ ("Avatar updated."));
    } else {
        toolbar_show_toast (_ ("Avatar saved — it'll be sent when you connect "
                               "to a server that supports GIF icons."));
    }
    g_free (contents);
    g_object_unref (preview);
}

static void
on_avatar_choose_clicked (GtkButton *btn, gpointer user_data)
{
    GtkWidget *preview = user_data; /* borrowed; reffed for the async call */
    GtkFileDialog *fd = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (fd, _ ("Choose GIF avatar"));

    GtkFileFilter *f = gtk_file_filter_new ();
    gtk_file_filter_set_name (f, _ ("GIF images"));
    gtk_file_filter_add_mime_type (f, "image/gif");
    GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
    g_list_store_append (filters, f);
    g_object_unref (f);
    gtk_file_dialog_set_filters (fd, G_LIST_MODEL (filters));
    g_object_unref (filters);

    GtkWindow *parent = NULL;
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (btn));
    if (GTK_IS_WINDOW (root)) {
        parent = GTK_WINDOW (root);
    }
    gtk_file_dialog_open (fd, parent, NULL, on_avatar_file_chosen,
                          g_object_ref (preview));
    g_object_unref (fd);
}

static void
on_avatar_clear_clicked (GtkButton *btn, gpointer user_data)
{
    GtkWidget *preview = user_data;
    (void) btn;
    gboolean removed = hx_icon_forget ();
    gtk_picture_set_paintable (GTK_PICTURE (preview), NULL);
    /* Tell the server to drop it too, if we're on a capable one. */
    if (hx_active_session ()->htlc.gif_icons_state == GIF_ICONS_SUPPORTED) {
        hx_icon_clear (&hx_active_session ()->htlc);
    }
    /* Don't claim it's cleared if the persisted file survived deletion —
	 * it'll reload and re-send next start. */
    toolbar_show_toast (removed
                            ? _ ("Avatar cleared.")
                            : _ ("Avatar cleared for now, but the saved "
                                 "file could not be deleted."));
}

/* --- Identity icon: inline preview + Browse popup ----------------- *
 *
 * The Identity page shows the currently-selected icon next to an
 * editable numeric ID (an AdwSpinRow) plus a "Browse…" button. Browse
 * pops a grid of every icon in the loaded resource files; activating
 * one stamps its ID onto the spin row (which repaints the inline
 * preview) and closes the popup. */

/* Render icon `resid` from the loaded resource files into `picture`.
 * load_icon walks icon_files by ID and hands back an owned pixbuf,
 * falling back to the default icon when the ID isn't found. */
static void
identity_icon_preview_update (GtkWidget *picture, guint resid)
{
    GdkPixbuf *pb = NULL;

    load_icon (NULL, (guint16)resid, &icon_files, 1, &pb, NULL);
    if (pb) {
        GdkTexture *tex = gtkhx_texture_from_pixbuf (pb);
        gtk_picture_set_paintable (GTK_PICTURE (picture),
                                   tex ? GDK_PAINTABLE (tex) : NULL);
        if (tex) {
            g_object_unref (tex);
        }
        g_object_unref (pb);
    } else {
        gtk_picture_set_paintable (GTK_PICTURE (picture), NULL);
    }
}

/* notify::value on the icon-ID spin row — repaint the inline preview.
 * Persistence is handled separately by on_spin_row_value (also wired by
 * pref_spin_row); this handler only touches the preview. */
static void
identity_icon_value_changed (GObject *row, GParamSpec *pspec, gpointer data)
{
    GtkWidget *preview = data;
    (void)pspec;
    identity_icon_preview_update (
        preview, (guint)adw_spin_row_get_value (ADW_SPIN_ROW (row)));
}

/* The Browse popup closed (selection, Esc, or window close): drop our
 * dangling pointers into its now-destroyed flowboxes. */
static void
on_icon_picker_closed (AdwDialog *dlg, gpointer data)
{
    (void)dlg;
    (void)data;
    if (iv) {
        iv->picker_dialog = NULL;
        iv->icon_list = NULL;
        iv->wide_list = NULL;
    }
}

/* Build one of the picker's flowboxes with the shared configuration.
 * `wide` selects the one-per-row banner strip vs. the multi-column grid
 * — the same two-flowbox split the inline picker used before. */
static GtkWidget *
icon_picker_make_flowbox (gboolean wide)
{
    GtkWidget *fb = gtk_flow_box_new ();

    gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (fb), GTK_SELECTION_SINGLE);
    gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (fb), wide ? FALSE : TRUE);
    gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (fb), wide ? 1 : 2);
    gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (fb), wide ? 1 : 4);
    gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (fb), 4);
    gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (fb), 4);
    gtk_flow_box_set_sort_func (GTK_FLOW_BOX (fb), icon_picker_sort_cb, NULL,
                                NULL);
    g_signal_connect (fb, "child-activated",
                      G_CALLBACK (icon_flow_child_activated), iv);
    return fb;
}

/* Deferred to idle so the popup's first layout pass has run: find the
 * flowbox child whose resid matches the current icon ID, select it, and
 * grab its focus — a focused child inside a GtkScrolledWindow gets
 * scrolled into view automatically. */
static gboolean
icon_picker_scroll_to_selected (gpointer data)
{
    guint want = GPOINTER_TO_UINT (data);
    GtkFlowBox *boxes[2];
    int b;

    if (!iv) {
        return G_SOURCE_REMOVE;
    }
    boxes[0] = iv->icon_list ? GTK_FLOW_BOX (iv->icon_list) : NULL;
    boxes[1] = iv->wide_list ? GTK_FLOW_BOX (iv->wide_list) : NULL;

    for (b = 0; b < 2; b++) {
        GtkWidget *child;

        if (!boxes[b]) {
            continue;
        }
        for (child = gtk_widget_get_first_child (GTK_WIDGET (boxes[b])); child;
             child = gtk_widget_get_next_sibling (child)) {
            guint id;

            if (!GTK_IS_FLOW_BOX_CHILD (child)) {
                continue;
            }
            id = GPOINTER_TO_UINT (
                g_object_get_data (G_OBJECT (child), "resid"));
            if (id == want) {
                gtk_flow_box_select_child (boxes[b],
                                          GTK_FLOW_BOX_CHILD (child));
                gtk_widget_grab_focus (child);
                return G_SOURCE_REMOVE;
            }
        }
    }
    return G_SOURCE_REMOVE;
}

/* "Browse…" clicked — pop the icon grid, presented on the Settings
 * dialog. list_icons() fills the flowboxes once they exist. */
static void
on_icon_browse_clicked (GtkButton *btn, gpointer data)
{
    AdwDialog *dlg;
    GtkWidget *tv, *scroll, *picker_box;
    (void)btn;
    (void)data;

    if (!iv) {
        return;
    }

    /* Already open — bring the existing popup forward rather than
     * spawning a second one (which would orphan the first with stale
     * iv->picker_dialog / icon_list / wide_list pointers). */
    if (iv->picker_dialog) {
        adw_dialog_present (ADW_DIALOG (iv->picker_dialog), options_window);
        return;
    }

    dlg = ADW_DIALOG (adw_dialog_new ());
    adw_dialog_set_title (dlg, _ ("Choose Icon"));
    adw_dialog_set_content_width (dlg, 420);
    adw_dialog_set_content_height (dlg, 520);
    gtk_widget_set_size_request (GTK_WIDGET (dlg), 300, 360);
    gtkhx_dialog_add_close_shortcuts (GTK_WIDGET (dlg));

    picker_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top (picker_box, 6);
    gtk_widget_set_margin_bottom (picker_box, 6);
    gtk_widget_set_margin_start (picker_box, 6);
    gtk_widget_set_margin_end (picker_box, 6);
    iv->icon_list = icon_picker_make_flowbox (FALSE);
    iv->wide_list = icon_picker_make_flowbox (TRUE);
    gtk_box_append (GTK_BOX (picker_box), iv->icon_list);
    gtk_box_append (GTK_BOX (picker_box), iv->wide_list);

    scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand (scroll, TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), picker_box);

    tv = adw_toolbar_view_new ();
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), adw_header_bar_new ());
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), scroll);
    adw_dialog_set_child (dlg, tv);

    iv->picker_dialog = GTK_WIDGET (dlg);
    g_signal_connect (dlg, "closed", G_CALLBACK (on_icon_picker_closed), NULL);

    adw_dialog_present (dlg, options_window);

    list_icons ();

    /* Preselect + scroll to the icon currently set on the spin row. */
    {
        struct cfgvar *v = cfgvar_for_name (CFG_ICON);
        guint cur = 0;

        if (v && v->widget && ADW_IS_SPIN_ROW (v->widget)) {
            cur = (guint)adw_spin_row_get_value (ADW_SPIN_ROW (v->widget));
        }
        g_idle_add (icon_picker_scroll_to_selected, GUINT_TO_POINTER (cur));
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
    AdwPreferencesGroup *name_grp, *id_grp;

    /* g_malloc0 — zero-fill the struct so any read of
     * iv->icon_list / nfound / icon_high / picker_dialog before the
     * Browse popup sets them returns 0 / NULL deterministically. */
    iv = g_malloc0 (sizeof (struct icon_viewer));

    name_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (name_grp, _ ("Display name"));
    adw_preferences_group_add (name_grp,
                               pref_entry_row (CFG_NICK, _ ("Your name")));
    adw_preferences_group_add (name_grp, pref_nick_color_row ());
    adw_preferences_page_add (page, name_grp);

    id_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (id_grp, _ ("Identity icon"));
    {
        /* pref_spin_row hands back an AdwSpinRow (an AdwActionRow
		 * subclass), so we can hang a live preview of the selected icon
		 * on the prefix and a "Browse…" button (which pops the full
		 * icon grid) on the suffix. The numeric ID stays directly
		 * editable in the spin entry. */
        GtkWidget *icon_row = pref_spin_row (
            CFG_ICON, _ ("Icon ID"),
            _ ("Numeric ID from the loaded icon resource files"), 0, 65535, 1);

        if (ADW_IS_SPIN_ROW (icon_row)) {
            GtkWidget *preview = gtk_picture_new ();
            GtkWidget *browse;

            gtk_widget_set_size_request (preview, 40, 40);
            gtk_widget_set_valign (preview, GTK_ALIGN_CENTER);
            gtk_picture_set_content_fit (GTK_PICTURE (preview),
                                         GTK_CONTENT_FIT_CONTAIN);
            identity_icon_preview_update (
                preview,
                (guint)adw_spin_row_get_value (ADW_SPIN_ROW (icon_row)));
            g_signal_connect (icon_row, "notify::value",
                              G_CALLBACK (identity_icon_value_changed),
                              preview);
            adw_action_row_add_prefix (ADW_ACTION_ROW (icon_row), preview);

            browse = gtk_button_new_with_label (_ ("Browse…"));
            gtk_widget_set_valign (browse, GTK_ALIGN_CENTER);
            g_signal_connect (browse, "clicked",
                              G_CALLBACK (on_icon_browse_clicked), NULL);
            adw_action_row_add_suffix (ADW_ACTION_ROW (icon_row), browse);
        }
        adw_preferences_group_add (id_grp, icon_row);
    }
    adw_preferences_page_add (page, id_grp);

    /* Custom GIF avatar (GIF-icons extension, Phase 10.C). Independent
	 * of the numeric icon above — a GIF other capable clients see in
	 * place of your icon, rendered like a normal icon / wide banner.
	 * You can pick one any time; it's persisted ($CONFIG/avatar.gif)
	 * and sent automatically once you're on a server that supports the
	 * extension, so the picker is never gated on the live connection. */
    {
        AdwPreferencesGroup *gif_grp
            = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
        adw_preferences_group_set_title (gif_grp, _ ("Custom GIF avatar"));
        adw_preferences_group_set_description (
            gif_grp,
            _ ("A GIF other users see in place of your icon. Best authored at "
               "icon size (or wide-banner size); max 32 KB. Sent "
               "automatically when you connect to a server that supports it."));

        AdwActionRow *gif_row = ADW_ACTION_ROW (adw_action_row_new ());
        adw_preferences_row_set_title (ADW_PREFERENCES_ROW (gif_row),
                                       _ ("Avatar"));

        GtkWidget *preview = gtk_picture_new ();
        gtk_widget_set_size_request (preview, 48, 48);
        gtk_widget_set_valign (preview, GTK_ALIGN_CENTER);
        gtk_picture_set_content_fit (GTK_PICTURE (preview),
                                     GTK_CONTENT_FIT_CONTAIN);
        /* Seed from the saved avatar (the user's choice), not the live
		 * per-session cache — so it shows even before connecting. */
        {
            GBytes *saved = hx_icon_load_saved ();
            if (saved) {
                gsize slen = 0;
                const guchar *sdata = g_bytes_get_data (saved, &slen);
                avatar_preview_from_gif (preview, sdata, slen);
                g_bytes_unref (saved);
            }
        }
        adw_action_row_add_prefix (gif_row, preview);

        GtkWidget *choose = gtk_button_new_with_label (_ ("Choose…"));
        gtk_widget_set_valign (choose, GTK_ALIGN_CENTER);
        g_signal_connect (choose, "clicked",
                          G_CALLBACK (on_avatar_choose_clicked), preview);
        GtkWidget *clear = gtk_button_new_with_label (_ ("Clear"));
        gtk_widget_set_valign (clear, GTK_ALIGN_CENTER);
        g_signal_connect (clear, "clicked",
                          G_CALLBACK (on_avatar_clear_clicked), preview);
        adw_action_row_add_suffix (gif_row, choose);
        adw_action_row_add_suffix (gif_row, clear);

        adw_preferences_group_add (gif_grp, GTK_WIDGET (gif_row));

        /* Animate avatars (Phase 10.D). Off renders the still first
		 * frame; per-user pause (click / right-click) is separate. */
        adw_preferences_group_add (
            gif_grp,
            pref_switch_row (CFG_ANIMATE_AVATARS, _ ("Animate GIF avatars"),
                             _ ("Play animated avatars in the user list. Turn "
                                "off to show a still frame.")));

        adw_preferences_page_add (page, gif_grp);
    }
}

#ifdef HAVE_VOICE
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
           "Changes take effect immediately, including during an active "
           "call."));
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
#endif /* HAVE_VOICE */

/* Sidebar-driven Settings navigation.
 *
 * Settings was previously an AdwPreferencesDialog, whose built-in
 * navigation is a top AdwViewSwitcher (icon tabs across the header,
 * adaptively collapsing to a bottom bar). This rebuilds it as a plain
 * AdwDialog wrapping an AdwNavigationSplitView: a left GtkListBox
 * sidebar of categories — grouped under section headers via the
 * list-box header func — and a right content pane that swaps
 * AdwPreferencesPage children in a GtkStack. The settings_page_*()
 * draw functions are unchanged: each still fills an AdwPreferencesPage,
 * only the outer container differs. (Note: dropping AdwPreferencesDialog
 * also drops its built-in search entry — the sidebar is the navigation
 * affordance now.)
 *
 * AdwNavigationSplitView is available since libadwaita 1.4 and the
 * sidebar is a plain GtkListBox with the .navigation-sidebar style
 * class, so this needs no bump of the meson libadwaita pin. */

#ifndef N_
#define N_(s) (s)
#endif

struct settings_entry {
    const char *section; /* section header, or NULL to continue previous */
    const char *name;    /* stable GtkStack child name */
    const char *title;   /* sidebar + content-header display title */
    const char *icon;    /* symbolic icon name */
    void (*draw) (AdwPreferencesPage *);
};

/* Flat sidebar list, grouped under section headers. Order here is the
 * order rows appear; a non-NULL .section starts a new header group. */
/* Rust page builders (gtkhx-ui options.rs, Phase R5.6) — build the ported
 * pages' content into a C-created AdwPreferencesPage. The two custom-widget
 * pages (Identity + Voice) stay C for now and keep their C draw functions
 * below. */
extern void gtkhx_options_rs_page_general (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_file_transfers (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_chat_appearance (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_chat_behavior (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_chat_history (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_chat_emoji (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_notify_events (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_notify_behavior (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_sound (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_tracker (AdwPreferencesPage *);

static const struct settings_entry settings_entries[] = {
    { N_ ("General"), "general", N_ ("General"),
      "preferences-system-symbolic", gtkhx_options_rs_page_general },
    { NULL, "identity", N_ ("Identity"), "user-info-symbolic",
      settings_page_identity },
    { NULL, "filexfer", N_ ("File Transfers"), "folder-download-symbolic",
      gtkhx_options_rs_page_file_transfers },
    { N_ ("Chat"), "chat_appearance", N_ ("Appearance"),
      "user-available-symbolic", gtkhx_options_rs_page_chat_appearance },
    { NULL, "chat_behavior", N_ ("Behavior"), "preferences-other-symbolic",
      gtkhx_options_rs_page_chat_behavior },
    { NULL, "chat_history", N_ ("History"), "document-open-recent-symbolic",
      gtkhx_options_rs_page_chat_history },
    { NULL, "chat_emoji", N_ ("Emoji"), "face-smile-symbolic",
      gtkhx_options_rs_page_chat_emoji },
    { N_ ("Notifications"), "notify_events", N_ ("Events"),
      "preferences-system-notifications-symbolic",
      gtkhx_options_rs_page_notify_events },
    { NULL, "notify_behavior", N_ ("Behavior"), "preferences-other-symbolic",
      gtkhx_options_rs_page_notify_behavior },
    { N_ ("Audio"), "sound", N_ ("Sound"), "audio-speakers-symbolic",
      gtkhx_options_rs_page_sound },
#ifdef HAVE_VOICE
    { NULL, "voice", N_ ("Voice"), "audio-input-microphone-symbolic",
      settings_page_voice },
#endif
    { N_ ("Network"), "trackers", N_ ("Trackers"), "network-server-symbolic",
      gtkhx_options_rs_page_tracker },
};

/* GtkListBox header func: draw a section label above the first row of
 * each section. Every row carries its (already-translated) section text
 * in "section" qdata, so a header is inserted whenever a row's section
 * differs from the row above it. */
static void
settings_sidebar_header (GtkListBoxRow *row, GtkListBoxRow *before,
                         gpointer data)
{
    const char *section = g_object_get_data (G_OBJECT (row), "section");
    const char *prev
        = before ? g_object_get_data (G_OBJECT (before), "section") : NULL;
    GtkWidget *label;
    (void)data;

    if (!section || (before && g_strcmp0 (section, prev) == 0)) {
        gtk_list_box_row_set_header (row, NULL);
        return;
    }

    label = gtk_label_new (section);
    gtk_widget_add_css_class (label, "heading");
    gtk_widget_add_css_class (label, "dim-label");
    gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
    gtk_widget_set_margin_start (label, 12);
    gtk_widget_set_margin_end (label, 12);
    gtk_widget_set_margin_top (label, before ? 12 : 6);
    gtk_widget_set_margin_bottom (label, 3);
    gtk_list_box_row_set_header (row, label);
}

/* Sidebar selection → swap the content stack, retitle the content
 * header, and (when collapsed) navigate to the content pane. The
 * split view / stack / content page are stashed as qdata on the
 * list box so this handler needs no module-static state. */
static void
settings_row_selected (GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    GtkStack *stack;
    AdwNavigationSplitView *split;
    AdwNavigationPage *content;
    const char *name, *title;
    (void)data;

    if (!row) {
        return;
    }
    stack = g_object_get_data (G_OBJECT (box), "stack");
    split = g_object_get_data (G_OBJECT (box), "split");
    content = g_object_get_data (G_OBJECT (box), "content-page");
    name = g_object_get_data (G_OBJECT (row), "page-name");
    title = g_object_get_data (G_OBJECT (row), "page-title");

    if (stack && name) {
        gtk_stack_set_visible_child_name (stack, name);
    }
    if (content && title) {
        adw_navigation_page_set_title (content, title);
    }
    if (split) {
        adw_navigation_split_view_set_show_content (split, TRUE);
    }
}

void
create_options_window (GtkWidget *widget, gpointer data)
{
    AdwDialog *dlg;
    AdwNavigationSplitView *split;
    AdwNavigationPage *sidebar_page, *content_page;
    AdwToolbarView *sidebar_tv, *content_tv;
    AdwBreakpoint *bp;
    GtkWidget *listbox, *sidebar_scroll, *stack;
    GtkWidget *parent;
    GtkListBoxRow *first_row = NULL;
    GValue collapsed = G_VALUE_INIT;
    const char *cur_section = NULL;
    size_t i;
    session *sess = data;

    (void)widget;

    parent = GTK_WIDGET (gtkhx_active_window ());

    if (options_window) {
        adw_dialog_present (ADW_DIALOG (options_window), parent);
        return;
    }

    /* Outer container is a plain AdwDialog (the project's meson floor is
	 * libadwaita >= 1.6): it
	 * auto-handles transient_for / modal-against-parent / adaptive
	 * sizing. content_width is the *preferred* size and must be wide
	 * enough for the sidebar + a preferences page side-by-side, or Adw
	 * warns "AdwNavigationSplitView exceeds AdwDialog width". The
	 * width/height-request set the collapsed *minimum* — without them
	 * Adw warns "AdwDialog does not have a minimum size". Below the
	 * breakpoint the split view collapses to a single navigable pane,
	 * so the minimum only needs to fit one pane. */
    dlg = ADW_DIALOG (adw_dialog_new ());
    adw_dialog_set_title (dlg, _ ("GtkHx Preferences"));
    adw_dialog_set_content_width (dlg, 920);
    adw_dialog_set_content_height (dlg, 680);
    gtk_widget_set_size_request (GTK_WIDGET (dlg), 360, 480);

    /* Esc closes via AdwDialog's built-in close_response; wire Ctrl+W
	 * (close) and Ctrl+Q (app.quit) for keyboard parity. */
    gtkhx_dialog_add_close_shortcuts (GTK_WIDGET (dlg));

    g_object_set_data (G_OBJECT (dlg), "sess", sess);
    g_signal_connect (dlg, "closed", G_CALLBACK (close_options_bookkeeping),
                      NULL);

    options_window = GTK_WIDGET (dlg);

    /* Content stack: one AdwPreferencesPage per settings_entry. */
    stack = gtk_stack_new ();
    gtk_widget_set_hexpand (stack, TRUE);
    gtk_widget_set_vexpand (stack, TRUE);

    /* Sidebar category list. */
    listbox = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (listbox),
                                     GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class (listbox, "navigation-sidebar");
    gtk_list_box_set_header_func (GTK_LIST_BOX (listbox),
                                  settings_sidebar_header, NULL, NULL);

    for (i = 0; i < sizeof (settings_entries) / sizeof (settings_entries[0]);
         i++) {
        const struct settings_entry *e = &settings_entries[i];
        AdwPreferencesPage *page
            = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
        GtkWidget *row, *rbox, *img, *lbl;
        const char *title = _ (e->title);

        if (e->section) {
            cur_section = _ (e->section);
        }

        adw_preferences_page_set_title (page, title);
        if (e->draw) {
            e->draw (page);
        }
        gtk_stack_add_named (GTK_STACK (stack), GTK_WIDGET (page), e->name);

        row = gtk_list_box_row_new ();
        rbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_start (rbox, 6);
        gtk_widget_set_margin_end (rbox, 6);
        gtk_widget_set_margin_top (rbox, 8);
        gtk_widget_set_margin_bottom (rbox, 8);
        img = gtk_image_new_from_icon_name (e->icon);
        lbl = gtk_label_new (title);
        gtk_label_set_xalign (GTK_LABEL (lbl), 0.0f);
        gtk_box_append (GTK_BOX (rbox), img);
        gtk_box_append (GTK_BOX (rbox), lbl);
        gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), rbox);

        /* qdata drives the header func + selection handler. name is a
		 * static literal (no dup); section/title are gettext returns
		 * (stable, but dup'd for lifetime safety across teardown). */
        g_object_set_data_full (G_OBJECT (row), "section",
                                g_strdup (cur_section), g_free);
        g_object_set_data (G_OBJECT (row), "page-name", (gpointer)e->name);
        g_object_set_data_full (G_OBJECT (row), "page-title",
                                g_strdup (title), g_free);
        gtk_list_box_append (GTK_LIST_BOX (listbox), row);
        if (!first_row) {
            first_row = GTK_LIST_BOX_ROW (row);
        }
    }

    sidebar_scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sidebar_scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sidebar_scroll),
                                   listbox);
    gtk_widget_set_vexpand (sidebar_scroll, TRUE);

    /* Sidebar pane: header bar + scrolled category list. */
    sidebar_tv = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
    adw_toolbar_view_add_top_bar (sidebar_tv, adw_header_bar_new ());
    adw_toolbar_view_set_content (sidebar_tv, sidebar_scroll);
    sidebar_page = ADW_NAVIGATION_PAGE (
        adw_navigation_page_new (GTK_WIDGET (sidebar_tv), _ ("Preferences")));

    /* Content pane: header bar (title tracks the selected page) + stack. */
    content_tv = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
    adw_toolbar_view_add_top_bar (content_tv, adw_header_bar_new ());
    adw_toolbar_view_set_content (content_tv, stack);
    content_page = ADW_NAVIGATION_PAGE (
        adw_navigation_page_new (GTK_WIDGET (content_tv), _ ("General")));

    split = ADW_NAVIGATION_SPLIT_VIEW (adw_navigation_split_view_new ());
    adw_navigation_split_view_set_sidebar (split, sidebar_page);
    adw_navigation_split_view_set_content (split, content_page);
    adw_navigation_split_view_set_max_sidebar_width (split, 240);

    g_object_set_data (G_OBJECT (listbox), "stack", stack);
    g_object_set_data (G_OBJECT (listbox), "split", split);
    g_object_set_data (G_OBJECT (listbox), "content-page", content_page);
    g_signal_connect (listbox, "row-selected",
                      G_CALLBACK (settings_row_selected), NULL);

    adw_dialog_set_child (dlg, GTK_WIDGET (split));

    /* Adaptive: collapse to a single navigable pane on narrow widths. */
    bp = adw_breakpoint_new (
        adw_breakpoint_condition_parse ("max-width: 500sp"));
    g_value_init (&collapsed, G_TYPE_BOOLEAN);
    g_value_set_boolean (&collapsed, TRUE);
    adw_breakpoint_add_setter (bp, G_OBJECT (split), "collapsed", &collapsed);
    g_value_unset (&collapsed);
    adw_dialog_add_breakpoint (dlg, bp);

    /* Select the first category so the content pane isn't blank. */
    if (first_row) {
        gtk_list_box_select_row (GTK_LIST_BOX (listbox), first_row);
    }

    adw_dialog_present (dlg, parent);
    /* The icon picker is no longer inline — it's populated on demand
	 * when the Identity page's "Browse…" button opens the popup, so
	 * there's no list_icons() call here. */
}

G_GNUC_END_IGNORE_DEPRECATIONS
/* end of file-level deprecation suppression — see top of file. */
