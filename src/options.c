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
#include "gtk_hlist.h"
#include "gtkhx.h"
#include "news.h"
#include "xtext.h"
#include "regex.h"
#include "cicn.h"
#include "sound.h"
#include "users.h"
#include "dfa.h"
#include "files.h"
#include "network.h"
#include "news15.h"
#include "log.h"
#include "gtkutil.h"
#include "cfgkeys.h"
#include "prefs_parser.h"
#include "options.h"
#include "tracker.h"

/* Phase 4.13: this file uses GtkTreeView + GtkListStore + GtkTreeStore
 * for the icon viewer and the prefs notebook tree (Phase 2.x work),
 * plus GtkDialog for the prefs dialog itself, GtkFontChooserDialog
 * for the font picker, and GtkComboBox for assorted dropdowns. All
 * those API families are deprecated in GTK 4.10+ in favor of
 * GtkColumnView/GListModel, GtkAlertDialog/GtkWindow, GtkFontDialog,
 * and GtkDropDown respectively. The migrations are tracked as
 * Phase 5 (tree-view), Phase 4.7 (dialogs), and a Phase 5 UX
 * follow-up (combo boxes, font chooser); until then suppress
 * deprecations across the file so the rest of the tree can keep
 * -Werror=deprecated-declarations on. */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

time_t start_time;
time_t total_time;

static struct icon_viewer *iv;

extern struct msgwin *msg_list;

GtkWidget *options_window = NULL;
GtkWidget *tracker_list = NULL;

struct gtkhx_prefs gtkhx_prefs =
{
	0,			/* num_tracker */
	CFG_THEME_SYSTEM,	/* theme: see CFG_THEME_* in cfgkeys.h */
	"",			/* auto_reply_msg */
	"fixed",	/* font */
	".",		/* download_path */
	"[%H:%M:%S] ", /* stamp_format — strftime(3); see CFG_STAMP_FORMAT */
	NULL,		/* tracker (char **) */
	"hltracker.com",	/* tracker_str */
	500,		/* xbuf_max */
	{
		{412, 312, 10, 434, 0, 1},
		{412, 384, 10, 50, 0, 1},
		{0,   0,   442, 0, 0, 0},
		{300, 250, 442, 480, 0, 1},
		{300, 400, 442, 50, 0, 1}
	},
	1,	/* queuedl */
	1,	/* showjoin */
	0,	/* showback */
	0,	/* auto_reply */
	0,	/* timestamp */
	0,	/* word_wrap */
	0,	/* file_samewin */
	1,	/* news_samewin */
	1,	/* track_case */
	0,	/* old_nickcompletion */
	0,	/* outrate_limit */
	0,	/* inrate_limit */
	0,	/* logging */

	/* Phase 5: HexChat-style autocopy controls. Default-on for text
	 * matches every modern chat client (HexChat, Discord, Slack, etc.)
	 * and matches the Settings mockup. Stamp / color stay off — most
	 * users want a clean copy of the message body. */
	1,	/* autocopy_text */
	0,	/* autocopy_stamp */
	0,	/* autocopy_color */

	0,	/* out_bps */
	0	/* in_bps */
};

static void parse_tracker (session *);

struct icon_viewer {
	guint32 icon_high;
	unsigned int nfound;
	GtkWidget *icon_list; /* Phase 5: now a GtkFlowBox, not a GtkHList */
};

/* Phase 5: forward declarations for the icon-picker FlowBox helpers.
 * The implementations live further down (after struct cfgvar and
 * cfgvar_for_name) since they touch the cfgvar lookup table; the
 * forward decls let list_icons() and settings_page_identity() —
 * both up here — wire them. */
static int  icon_picker_sort_cb (GtkFlowBoxChild *a, GtkFlowBoxChild *b,
                                 gpointer data);
static void icon_flow_child_activated (GtkFlowBox *flowbox,
                                       GtkFlowBoxChild *child,
                                       gpointer data);

static void list_icons (void)
{
	/* Phase 5: replaced the 1-row-per-icon GtkHList layout (18px tall
	 * rows, 34px icons) with a GtkFlowBox that auto-flows multiple
	 * larger icons per row. Each entry is now a 64x64 image plus the
	 * resource-ID label, packed into a vertical box and inserted as
	 * a flowbox child. The flowbox itself is configured with min/max
	 * children per line so the picker grows to as many columns as
	 * fit the picker width without going single-column-narrow.
	 *
	 * Phase 3.4: cicn_to_pixbuf returns a GdkPixbuf directly with the
	 * Mac classic mask folded into the alpha channel. Wide icons
	 * (the 32x32 family bundles four variants in a 4*32-pixel row)
	 * are clipped to the rightmost 32 px to mirror the historical
	 * "off = width > 400 ? 198 : 0" hack. */
	GtkWidget *icon_list = iv->icon_list;
	guint16 nres;
	guint32 icon;
	unsigned int nfound = 0;
	unsigned int i;

	for (i = 0; i < icon_files.n; ++i) {
		if (!icon_files.cicns[i])
			continue;
		nres = macres_file_num_res_of_type (icon_files.cicns[i], TYPE_cicn);
		for (icon = 0; icon < nres; icon++) {
			macres_res *r;
			GdkPixbuf *pb, *cropped, *scaled;
			GtkWidget *child, *vbox, *image, *label;
			int width, height, off;
			char buf[16];

			r = macres_file_get_nth_res_of_type (icon_files.cicns[i], TYPE_cicn, icon);
			if (!r)
				continue;

			pb = cicn_to_pixbuf (r->data, r->datalen);
			if (!pb) {
				g_free (r);
				continue;
			}
			width  = gdk_pixbuf_get_width (pb);
			height = gdk_pixbuf_get_height (pb);
			off = width > 400 ? 198 : 0;
			if (off) {
				cropped = gdk_pixbuf_new_subpixbuf (pb, off, 0,
				                                   width - off, height);
				g_object_unref (pb);
				pb = cropped;
			}

			/* Scale to a uniform display size — the source pixbufs
			 * vary (16x16, 32x32, sometimes wider) and the picker
			 * looks sloppy if some entries are tiny next to others.
			 * Nearest-neighbor preserves the pixel-art look on
			 * scale-up; 56px is a comfortable middle ground between
			 * the legacy ~34px (too small) and 96px (too dominant).
			 * Hits a tidy 3.5x of 16px and 1.75x of 32px sources —
			 * the slight non-integer scale isn't perfectly crisp but
			 * is fine for a thumbnail at this size. */
			scaled = gdk_pixbuf_scale_simple (pb, 56, 56, GDK_INTERP_NEAREST);
			g_object_unref (pb);
			pb = scaled ? scaled : pb;

			nfound++;
			g_snprintf (buf, sizeof (buf), "%u", r->resid);

			vbox  = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
			/* Phase 5: GtkPicture, not GtkImage. Adwaita's
			 * stylesheet sizes GtkImage to icon-button dimensions
			 * (~16-24px) regardless of the paintable's natural
			 * size, so set_size_request on a wrapper grew the cell
			 * but kept the icon clamped tiny inside it. GtkPicture
			 * with can_shrink=FALSE pins to the paintable's natural
			 * size — same fix gtkhx_pixmap_button uses for the
			 * toolbar buttons. */
			{
				GdkTexture *tex;
				G_GNUC_BEGIN_IGNORE_DEPRECATIONS
				tex = gdk_texture_new_for_pixbuf (pb);
				G_GNUC_END_IGNORE_DEPRECATIONS
				image = gtk_picture_new_for_paintable (GDK_PAINTABLE (tex));
				g_object_unref (tex);
				gtk_picture_set_can_shrink (GTK_PICTURE (image), FALSE);
			}
			label = gtk_label_new (buf);
			gtk_widget_add_css_class (label, "caption");
			gtk_box_append (GTK_BOX (vbox), image);
			gtk_box_append (GTK_BOX (vbox), label);
			gtk_widget_set_margin_start  (vbox, 6);
			gtk_widget_set_margin_end    (vbox, 6);
			gtk_widget_set_margin_top    (vbox, 6);
			gtk_widget_set_margin_bottom (vbox, 6);

			child = gtk_flow_box_child_new ();
			gtk_flow_box_child_set_child (GTK_FLOW_BOX_CHILD (child), vbox);
			g_object_set_data (G_OBJECT (child), "resid",
			                   GUINT_TO_POINTER (r->resid));
			gtk_flow_box_append (GTK_FLOW_BOX (icon_list), child);

			g_object_unref (pb);
			g_free (r);

			/* Cooperative multitasking — keep the dialog responsive while
			 * paging through hundreds of resource entries. */
			if (icon % 10 == 0) {
				while (g_main_context_pending(NULL))
					g_main_context_iteration(NULL, TRUE);
				if (!options_window)
					return;
			}
		}
	}
	if (nfound >= 2)
		gtk_flow_box_invalidate_sort (GTK_FLOW_BOX (icon_list));
}


void reinit_gtktexts (session *sess)
{
	struct msgwin *msg;
	struct gtkhx_chat *gchat;

	if(gtkhx_prefs.geo.news.open) {
		gtkhx_apply_text_style(sess->news_text);
	}
	{
		gchar *fontname = pango_font_description_to_string (gtkhx_font_desc);
		for(gchat = sess->gchat_list; gchat; gchat = gchat->prev) {
			if(gchat->cid == 0 && !gtkhx_prefs.geo.chat.open)
				continue;
			gtk_xtext_set_font(GTK_XTEXT(gchat->output), fontname);
			gtk_xtext_refresh(GTK_XTEXT(gchat->output));
			if(gchat->input) {
				gtkhx_apply_text_style(gchat->input);
			}
			if(gchat->subject) {
				gtkhx_apply_text_style(gchat->subject);
			}
		}
		for(msg = sess->msg_list; msg; msg = msg->prev) {
			gtk_xtext_set_font(GTK_XTEXT(msg->outputbuf), fontname);
			gtk_xtext_refresh(GTK_XTEXT(msg->outputbuf));
			gtkhx_apply_text_style(msg->inputbuf);
		}
		g_free (fontname);
	}
}


static void changed_xtext (session *sess)
{
	if (sess)
	{
		struct gtkhx_chat *gchat;
		struct msgwin *msg;
		for(gchat = sess->gchat_list; gchat; gchat = gchat->prev) {
			GTK_XTEXT(gchat->output)->wordwrap = gtkhx_prefs.word_wrap;
			GTK_XTEXT(gchat->output)->max_lines = gtkhx_prefs.xbuf_max;
			gtk_xtext_refresh(GTK_XTEXT(gchat->output));
		}
		for(msg = sess->msg_list; msg; msg = msg->prev) {
			GTK_XTEXT(msg->outputbuf)->wordwrap = gtkhx_prefs.word_wrap;
			GTK_XTEXT(msg->outputbuf)->max_lines = gtkhx_prefs.xbuf_max;
			gtk_xtext_refresh(GTK_XTEXT(msg->outputbuf));
		}
	}
}

/* Phase 5: apply the CFG_TIMESTAMP toggle to every live xtext buffer
 * — chat / pchat outputs in gchat_list, plus PM outputs in msg_list.
 * Native xtext stamps are flipped per-buffer via gtk_xtext_set_time_stamp.
 * gtk_xtext_refresh forces a full re-render so the new state is visible
 * without scrolling the buffer first. */
static void changed_timestamp (session *sess)
{
	if (!sess)
		return;
	struct gtkhx_chat *gchat;
	struct msgwin *msg;
	for (gchat = sess->gchat_list; gchat; gchat = gchat->prev) {
		gtk_xtext_set_time_stamp (GTK_XTEXT (gchat->output)->buffer,
		                          gtkhx_prefs.timestamp);
		gtk_xtext_refresh (GTK_XTEXT (gchat->output));
	}
	for (msg = sess->msg_list; msg; msg = msg->prev) {
		gtk_xtext_set_time_stamp (GTK_XTEXT (msg->outputbuf)->buffer,
		                          gtkhx_prefs.timestamp);
		gtk_xtext_refresh (GTK_XTEXT (msg->outputbuf));
	}
}

/* Phase 5: re-enabled. The Settings nick/icon controls used to be
 * inert — changing them updated gtkhx_prefs / the_session.htlc but
 * never told the server, so the user list still showed your old
 * nick/icon until reconnect. hlwrite is already a no-op when
 * htlc->fd is 0 (network.c:1448), so this is safe to call before
 * connect too: prefs_read doesn't run change-callbacks anyway, only
 * the Adw row notify handlers do, so the wire packet only fires
 * when the user actually toggles a control in Settings. */
static void changed_nickoricon (session *sess)
{
	(void) sess;
	hx_change_name_icon (&the_session.htlc);
}

static void changed_font (session *sess)
{
	if (gtkhx_font_desc) {
		pango_font_description_free (gtkhx_font_desc);
		gtkhx_font_desc = NULL;
	}

	if (gtkhx_prefs.font && *gtkhx_prefs.font)
		gtkhx_font_desc = pango_font_description_from_string (gtkhx_prefs.font);

	if (!gtkhx_font_desc) {
		g_warning ("Bad font \"%s\"\n",
		           gtkhx_prefs.font ? gtkhx_prefs.font : "");
		gtkhx_font_desc = pango_font_description_from_string ("Monospace 10");
		if (gtkhx_prefs.font)
			g_free (gtkhx_prefs.font);
		gtkhx_prefs.font = g_strdup ("Monospace 10");
	}

	/* Phase 3.5: rebuild the screen-wide CSS provider so already-tagged
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

/* Phase 5: HexChat-style xtext autocopy. Each toggle in Settings →
 * Advanced → Auto Copy Behavior calls one of the three xtext setters
 * to flip the corresponding facet of the drag-end clipboard handler.
 * The cfgvars own the persisted state in gtkhx_prefs.autocopy_* bytes;
 * the changefunc just propagates that to the xtext-internal `prefs`
 * struct so the next drag-end picks up the new behaviour without
 * having to recreate the widget. */
static void changed_autocopy_text (session *sess)
{
	(void) sess;
	gtk_xtext_set_autocopy_text (gtkhx_prefs.autocopy_text);
}
static void changed_autocopy_stamp (session *sess)
{
	(void) sess;
	gtk_xtext_set_autocopy_stamp (gtkhx_prefs.autocopy_stamp);
}
static void changed_autocopy_color (session *sess)
{
	(void) sess;
	gtk_xtext_set_autocopy_color (gtkhx_prefs.autocopy_color);
}

/* Phase 5: apply CFG_STAMP_FORMAT to every live xtext widget. The
 * setter stashes the new format in xtext's module-global, recomputes
 * stamp_width per widget (font-dependent), and grows the buffer
 * indent if the new column is wider than before. queue_draw fires
 * inside the setter so the new column shows up next frame. */
static void changed_stampformat (session *sess)
{
	if (!sess)
		return;
	struct gtkhx_chat *gchat;
	struct msgwin *msg;
	for (gchat = sess->gchat_list; gchat; gchat = gchat->prev) {
		gtk_xtext_set_stamp_format (GTK_XTEXT (gchat->output),
		                            gtkhx_prefs.stamp_format);
	}
	for (msg = sess->msg_list; msg; msg = msg->prev) {
		gtk_xtext_set_stamp_format (GTK_XTEXT (msg->outputbuf),
		                            gtkhx_prefs.stamp_format);
	}
}

static void changed_downloadpath (session *sess)
{
	if (!*gtkhx_prefs.download_path)
	{
/*		g_free (gtkhx_prefs.download_path); */
		gtkhx_prefs.download_path = g_strdup(".");
	}
}

static void changed_case (session *sess)
{
	(void) sess;
	dfasyntax ((RE_CHAR_CLASSES | RE_CONTEXT_INDEP_ANCHORS |
				RE_CONTEXT_INDEP_OPS | RE_HAT_LISTS_NOT_NEWLINE |
				RE_NEWLINE_ALT | RE_NO_BK_PARENS | RE_NO_BK_VBAR),
				!gtkhx_prefs.track_case, '\n');
	/* If the Tracker window is open, refresh its filter against the
	 * already-cached server tree so the visible result list reflects
	 * the new fold mode immediately. No-op when the Tracker isn't
	 * open. */
	tracker_search_refresh ();
}

static void changed_filesamewin (session *sess)
{
	struct gfile_list *gfl;

	for(gfl = gfile_list; gfl; gfl = gfl->prev) {
		gtk_widget_set_sensitive(gfl->up_btn, gtkhx_prefs.file_samewin);
	}
}

/* Phase 5: apply the THEME pref to libadwaita's style manager. The pref
 * is one of "system" / "light" / "dark"; anything else falls back to
 * the system default so a hand-edited gtkhxrc with a typo doesn't lock
 * the user into a broken state. Called both at startup (after prefs_read)
 * and via the cfgvar change-callback when the user picks a new value
 * in Settings. */
static void changed_theme (session *sess)
{
	AdwStyleManager *sm = adw_style_manager_get_default ();
	const char *theme = gtkhx_prefs.theme ? gtkhx_prefs.theme : CFG_THEME_SYSTEM;
	AdwColorScheme scheme;

	(void) sess;

	if (g_strcmp0 (theme, CFG_THEME_LIGHT) == 0)
		scheme = ADW_COLOR_SCHEME_FORCE_LIGHT;
	else if (g_strcmp0 (theme, CFG_THEME_DARK) == 0)
		scheme = ADW_COLOR_SCHEME_FORCE_DARK;
	else
		scheme = ADW_COLOR_SCHEME_DEFAULT;

	adw_style_manager_set_color_scheme (sm, scheme);
}
static void changed_newssamewin (session *sess)
{
	struct gnews_folder *gfnews;

	for(gfnews = gfnews_list; gfnews; gfnews = gfnews->prev) {
		gtk_widget_set_sensitive(gfnews->up_btn, gtkhx_prefs.news_samewin);
	}
}

struct cfgvar
{
	/* name of variable as it appears in conf file */
	const char *name;
	/* pointer to where data should be writen */
	/* The unionization is to avoid strong-typed nightmares with casting */
	union
	{
		void *var;
		char **str;
		char *str32;
		int *integer;
		unsigned char *uchar;
		time_t *timet;
		guint16 *uint16;
	} variable;
	/* type of variable pointed to by "variable" is stored in "type": */
#define INT 1 /* int* */
#define BOOLEAN 2 /* unsigned char:1* */ /* INT1 */
#define STRING 3 /* string (char *) */
#define STRING32 4
#define UINT16 5
#define TIME_T 6
	const unsigned int type:7;
	unsigned int allocated:1; /* only meaningful for a string */
	/* func to call when changed */
	void (*changefunc)(session*);
	GtkWidget *widget;
} cfgvars[] =
{
	{CFG_AUTOCOPY_COLOR, {&gtkhx_prefs.autocopy_color}, BOOLEAN, 0,
	 changed_autocopy_color, NULL},
	{CFG_AUTOCOPY_STAMP, {&gtkhx_prefs.autocopy_stamp}, BOOLEAN, 0,
	 changed_autocopy_stamp, NULL},
	{CFG_AUTOCOPY_TEXT, {&gtkhx_prefs.autocopy_text}, BOOLEAN, 0,
	 changed_autocopy_text, NULL},
	{CFG_AUTOREPLY_MSG, {&gtkhx_prefs.auto_reply_msg}, STRING, 0, NULL, NULL},
	{CFG_AUTOREPLY_ON, {&gtkhx_prefs.auto_reply}, BOOLEAN, 0, NULL, NULL},
	{CFG_CHAT_XPOS, {&gtkhx_prefs.geo.chat.xpos}, INT, 0, NULL, NULL},
	{CFG_CHAT_XSIZE, {&gtkhx_prefs.geo.chat.xsize}, INT, 0, NULL, NULL},
	{CFG_CHAT_YPOS, {&gtkhx_prefs.geo.chat.ypos}, INT, 0, NULL, NULL},
	{CFG_CHAT_YSIZE, {&gtkhx_prefs.geo.chat.ysize}, INT, 0, NULL, NULL},
	{CFG_DOWNLOAD, {&gtkhx_prefs.download_path}, STRING, 0,
	 changed_downloadpath, NULL},
	{CFG_FILE_SAMEWIN, {&gtkhx_prefs.file_samewin}, BOOLEAN, 0,
	 changed_filesamewin, NULL},
	{CFG_FONT, {&gtkhx_prefs.font}, STRING, 0, changed_font, NULL},
	{CFG_ICON, {&the_session.htlc.icon}, UINT16, 0,
	 changed_nickoricon, NULL},
	/* Phase 5: ICONS used to be a comma-separated list of *.rsrc files.
	 * Auto-discovery in $CONFIG/icons/ + $PREFIX/share/gtkhx/icons/
	 * replaces it; the cfgvar is gone so a stray ICONS=... line in a
	 * legacy gtkhxrc is silently dropped by prefs_allocate's bsearch
	 * miss path, and prefs_write never re-emits one. */
#if 0 /* XXX */
	{CFG_LOGGING, {&gtkhx_prefs.logging}, BOOLEAN, 0, changed_logging, NULL},
#endif
	{CFG_NEWS_XPOS, {&gtkhx_prefs.geo.news.xpos}, INT, 0, NULL, NULL},
	{CFG_NEWS_XSIZE, {&gtkhx_prefs.geo.news.xsize}, INT, 0, NULL, NULL},
	{CFG_NEWS_YPOS, {&gtkhx_prefs.geo.news.ypos}, INT, 0, NULL, NULL},
	{CFG_NEWS_YSIZE, {&gtkhx_prefs.geo.news.ysize}, INT, 0, NULL, NULL},
	{CFG_NEWS_SAMEWIN, {&gtkhx_prefs.news_samewin}, BOOLEAN, 0,
	 changed_newssamewin, NULL},
	{CFG_NICK, {the_session.htlc.name}, STRING32, 0,
	 changed_nickoricon, NULL},
	{CFG_OLD_NICKCOMP, {&gtkhx_prefs.old_nickcompletion}, BOOLEAN, 0,
	 NULL, NULL},
	{CFG_OPEN_CHAT, {&gtkhx_prefs.geo.chat.init}, BOOLEAN, 0, NULL, NULL},
	{CFG_OPEN_NEWS, {&gtkhx_prefs.geo.news.init}, BOOLEAN, 0, NULL, NULL},
	{CFG_OPEN_TASKS, {&gtkhx_prefs.geo.tasks.init}, BOOLEAN, 0, NULL, NULL},
	{CFG_OPEN_USERS, {&gtkhx_prefs.geo.users.init}, BOOLEAN, 0, NULL, NULL},
	{CFG_QUEUEDL, {&gtkhx_prefs.queuedl}, BOOLEAN, 0, NULL, NULL},
	{CFG_SHOWBACK, {&gtkhx_prefs.showback}, BOOLEAN, 0, NULL, NULL},
	{CFG_SHOWJOIN, {&gtkhx_prefs.showjoin}, BOOLEAN, 0, NULL, NULL},
	{CFG_SND_CHAT, {&hxsnd.chat}, BOOLEAN, 0, NULL, NULL},
	{CFG_SND_ERROR, {&hxsnd.error}, BOOLEAN, 0, NULL, NULL},
	{CFG_SND_FILE, {&hxsnd.file}, BOOLEAN, 0, NULL, NULL},
	{CFG_SND_INVITE, {&hxsnd.invite}, BOOLEAN, 0, NULL, NULL},
	{CFG_SND_JOIN, {&hxsnd.join}, BOOLEAN, 0, NULL, NULL},
	{CFG_SND_LOGIN, {&hxsnd.login}, BOOLEAN, 0, NULL, NULL},
	{CFG_SND_MSG, {&hxsnd.msg}, BOOLEAN, 0, NULL, NULL},
	{CFG_SND_NEWS, {&hxsnd.news}, BOOLEAN, 0, NULL, NULL},
	{CFG_SND_PART, {&hxsnd.part}, BOOLEAN, 0, NULL, NULL},
	/* Phase 5: SOUNDPATH used to set a fallback directory for sound
	 * files. Auto-discovery in $CONFIG/sounds/ + $PREFIX/share/gtkhx/sounds/
	 * replaces it; the cfgvar is gone so legacy gtkhxrc lines are
	 * silently dropped at parse time. */
	{CFG_SOUNDS_ON, {&hxsnd.on}, BOOLEAN, 0, NULL, NULL},
	{CFG_TASK_XPOS, {&gtkhx_prefs.geo.tasks.xpos}, INT, 0, NULL, NULL},
	{CFG_TASK_XSIZE, {&gtkhx_prefs.geo.tasks.xsize}, INT, 0, NULL, NULL},
	{CFG_TASK_YPOS, {&gtkhx_prefs.geo.tasks.ypos}, INT, 0, NULL, NULL},
	{CFG_TASK_YSIZE, {&gtkhx_prefs.geo.tasks.ysize}, INT, 0, NULL, NULL},
	{CFG_THEME, {&gtkhx_prefs.theme}, STRING, 0, changed_theme, NULL},
	{CFG_TIME, {&total_time}, TIME_T, 0, NULL, NULL},
	{CFG_TIMESTAMP, {&gtkhx_prefs.timestamp}, BOOLEAN, 0,
	 changed_timestamp, NULL},
	{CFG_STAMP_FORMAT, {&gtkhx_prefs.stamp_format}, STRING, 0,
	 changed_stampformat, NULL},
	{CFG_TOOL_XPOS, {&gtkhx_prefs.geo.tool.xpos}, INT, 0, NULL, NULL},
	{CFG_TOOL_YPOS, {&gtkhx_prefs.geo.tool.ypos}, INT, 0, NULL, NULL},
	{CFG_TRACKER, {&gtkhx_prefs.tracker_str}, STRING, 0, parse_tracker, NULL},
	{CFG_TRACKER_CASE, {&gtkhx_prefs.track_case}, BOOLEAN, 0,
	 changed_case, NULL},
	{CFG_USER_XPOS, {&gtkhx_prefs.geo.users.xpos}, INT, 0, NULL, NULL},
	{CFG_USER_XSIZE, {&gtkhx_prefs.geo.users.xsize}, INT, 0, NULL, NULL},
	{CFG_USER_YPOS, {&gtkhx_prefs.geo.users.ypos}, INT, 0, NULL, NULL},
	{CFG_USER_YSIZE, {&gtkhx_prefs.geo.users.ysize}, INT, 0, NULL, NULL},
	{CFG_WORDWRAP, {&gtkhx_prefs.word_wrap}, BOOLEAN, 0, changed_xtext, NULL},
	{CFG_XBUF_MAX, {&gtkhx_prefs.xbuf_max}, INT, 0, changed_xtext, NULL}
};

/* Phase 5: the parallel FOO_IDX enum that paired up with cfgvars[] is
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

	if (checked)
		return;
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

	r = bsearch (name, cfgvars,
	             sizeof (cfgvars) / sizeof (cfgvars[0]),
	             sizeof (cfgvars[0]),
	             cfgnamecmp_const);
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

static int cfgnamecmp_const (const void *key, const void *mem)
{
	return strcmp ((const char *) key, ((const struct cfgvar *) mem)->name);
}

/* prefs_write is defined after the row helpers but called by them. */
void prefs_write (void);

/* Phase 5: helper to compare two flowbox children by their stored
 * resource ID so the icon picker stays sorted by ID. Forward-declared
 * up near list_icons() because it's wired there too. */
static int
icon_picker_sort_cb (GtkFlowBoxChild *a, GtkFlowBoxChild *b, gpointer data)
{
	guint id_a = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (a), "resid"));
	guint id_b = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (b), "resid"));
	(void) data;

	if (id_a < id_b) return -1;
	if (id_a > id_b) return 1;
	return 0;
}

/* Phase 5: handler for the FlowBox's child-activated signal. The
 * activated child carries the resid as g_object_set_data; copy it
 * into the AdwSpinRow so the user's selection becomes the active
 * icon ID — same write-through behaviour the legacy
 * icon_row_selected() had against the GtkHList row. */
static void
icon_flow_child_activated (GtkFlowBox *flowbox, GtkFlowBoxChild *child,
                           gpointer data)
{
	struct cfgvar *v;
	guint icon;
	(void) flowbox; (void) data;

	icon = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (child), "resid"));
	if (!icon)
		return;

	v = cfgvar_for_name (CFG_ICON);
	if (v && v->widget && ADW_IS_SPIN_ROW (v->widget))
		adw_spin_row_set_value (ADW_SPIN_ROW (v->widget), icon);
}

/* ------------------------------------------------------------------- *
 * Phase 5: AdwPreferencesRow helpers
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
	if (v->changefunc)
		v->changefunc (&the_session);
	prefs_write ();
}

static void
on_switch_row_active (AdwSwitchRow *row, GParamSpec *pspec, gpointer data)
{
	struct cfgvar *v = data;
	(void) pspec;
	*v->variable.uchar = adw_switch_row_get_active (row) ? 1 : 0;
	pref_apply (v);
}

static GtkWidget *
pref_switch_row (const char *cfgname, const char *title, const char *subtitle)
{
	struct cfgvar *v = cfgvar_for_name (cfgname);
	GtkWidget *row = adw_switch_row_new ();

	adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
	if (subtitle && *subtitle)
		adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
	if (!v || v->type != BOOLEAN) {
		gtk_widget_set_sensitive (row, FALSE);
		return row;
	}
	adw_switch_row_set_active (ADW_SWITCH_ROW (row),
	                           *v->variable.uchar ? TRUE : FALSE);
	v->widget = row;
	g_signal_connect (row, "notify::active",
	                  G_CALLBACK (on_switch_row_active), v);
	return row;
}

/* Phase 5: per-row debounce for entry-row apply.
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
	if (v->widget)
		g_object_set_data (G_OBJECT (v->widget), ENTRY_TIMER_KEY,
		                   GUINT_TO_POINTER (0));
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
	if (old)
		g_source_remove (old);
	guint id = g_timeout_add (ENTRY_APPLY_DEBOUNCE_MS,
	                          entry_apply_timeout_cb, v);
	g_object_set_data (G_OBJECT (v->widget), ENTRY_TIMER_KEY,
	                   GUINT_TO_POINTER (id));
}

static void
entry_apply_flush (struct cfgvar *v)
{
	guint id;
	if (!v->widget)
		return;
	id = GPOINTER_TO_UINT (
		g_object_get_data (G_OBJECT (v->widget), ENTRY_TIMER_KEY));
	if (!id)
		return;
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
	(void) pspec;

	if (!txt) txt = "";

	switch (v->type) {
	case STRING:
		if (*v->variable.str && strcmp (*v->variable.str, txt) == 0)
			return;
		if (v->allocated)
			g_free (*v->variable.str);
		*v->variable.str = g_strdup (txt);
		v->allocated = 1;
		break;
	case STRING32:
		if (strncmp (v->variable.str32, txt, 31) == 0)
			return;
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

	if (v->type == STRING)
		gtk_editable_set_text (GTK_EDITABLE (row),
		                       *v->variable.str ? *v->variable.str : "");
	else
		gtk_editable_set_text (GTK_EDITABLE (row), v->variable.str32);

	v->widget = row;
	g_signal_connect (row, "notify::text",
	                  G_CALLBACK (on_entry_row_text), v);
	return row;
}

static void
on_spin_row_value (AdwSpinRow *row, GParamSpec *pspec, gpointer data)
{
	struct cfgvar *v = data;
	double val = adw_spin_row_get_value (row);
	(void) pspec;

	switch (v->type) {
	case INT: {
		int n = (int) val;
		if (n == *v->variable.integer) return;
		*v->variable.integer = n;
		break;
	}
	case UINT16: {
		guint16 n = (guint16) val;
		if (n == *v->variable.uint16) return;
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
	if (subtitle && *subtitle)
		adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
	if (!v || (v->type != INT && v->type != UINT16)) {
		gtk_widget_set_sensitive (row, FALSE);
		return row;
	}

	initial = (v->type == INT) ? *v->variable.integer : *v->variable.uint16;
	adw_spin_row_set_value (ADW_SPIN_ROW (row), initial);
	v->widget = row;
	g_signal_connect (row, "notify::value",
	                  G_CALLBACK (on_spin_row_value), v);
	return row;
}

static void
on_combo_row_selected (AdwComboRow *row, GParamSpec *pspec, gpointer data)
{
	struct cfgvar *v = data;
	GtkStringList *list;
	guint idx;
	const char *selected;
	(void) pspec;

	if (v->type != STRING)
		return;

	list = GTK_STRING_LIST (g_object_get_data (G_OBJECT (row),
	                                            "pref-combo-values"));
	idx = adw_combo_row_get_selected (row);
	selected = list ? gtk_string_list_get_string (list, idx) : NULL;
	if (!selected) return;

	if (*v->variable.str && strcmp (*v->variable.str, selected) == 0)
		return;
	if (v->allocated)
		g_free (*v->variable.str);
	*v->variable.str = g_strdup (selected);
	v->allocated = 1;
	pref_apply (v);
}

/* AdwComboRow with a fixed value list. `values[]` are the strings
 * stored in the cfgvar; `labels[]` are user-visible (translatable)
 * presentation. n is the number of entries; arrays are not freed. */
static GtkWidget *
pref_combo_row (const char *cfgname, const char *title,
                const char **values, const char **labels, int n)
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
	g_object_set_data_full (G_OBJECT (row), "pref-combo-values",
	                        values_model, g_object_unref);
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

void init_variables(void) /* default settings if prefs file is not found. */
{
	gtkhx_prefs.font = g_strdup ("Monospace 10");
	gtkhx_font_desc = pango_font_description_from_string (gtkhx_prefs.font);
	(*cfgvar_for_name(CFG_FONT)).allocated = 1;


	/* Phase 3.10: GdkRGBA defaults — light grey foreground on black,
	 * preserving the historic 0xcccc/0xffff fraction. */
	fg_col.red   = 0xcccc / 65535.0;
	fg_col.green = 0xcccc / 65535.0;
	fg_col.blue  = 0xcccc / 65535.0;
	fg_col.alpha = 1.0;
	bg_col.red   = 0.0;
	bg_col.green = 0.0;
	bg_col.blue  = 0.0;
	bg_col.alpha = 1.0;

	changed_case(NULL);

	parse_tracker(NULL);

	start_time = time(NULL);
}

static void prefs_allocate(char *tag, char *rest)
{
	struct cfgvar *result;
	result = bsearch (tag, cfgvars,
	                  sizeof (cfgvars) / sizeof (cfgvars[0]),
	                  sizeof (cfgvars[0]),
	                  cfgnamecmp_const);

	if (!result) return;

	switch (result->type)
	{
		case INT:
		{
			int i = atoi(rest);
			if (i == *result->variable.integer) return;
			*result->variable.integer = i;
			break;
		}
		case BOOLEAN:
		{
			/* Phase 5: prefs_write emits booleans via
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
			if (!prefs_parse_boolean (rest, &c)) return;
			if (*result->variable.uchar == c) return;
			*result->variable.uchar = c;
			break;
		}
		case STRING:
			if (!*result->variable.str || strcmp(rest, *result->variable.str))
			{
				if (result->allocated)
					g_free (*result->variable.str);
				*result->variable.str = g_strdup (rest);
				result->allocated = 1;
				break;
			}
			return;
		case TIME_T:
		{
			time_t t = atol(rest);
			if (t == *result->variable.timet) return;
			*result->variable.timet = t;
			break;
		}
		case STRING32:
			if (!strncmp(result->variable.str32, rest, 31)) return;
			strncpy (result->variable.str32, rest, 31);
			result->variable.str32[31] = '\0';
			break;
		case UINT16:
		{
			guint16 g = (guint16) atoi(rest);
			if (g == *result->variable.uint16) return;
			*result->variable.uint16 = g;
			break;
		}
	}

	if (result->changefunc)
		(*(result->changefunc))(&the_session);
}

static void parse_line(char *line)
{
	char *rest=0, *p;

	/* Change any '#' to a null char. We aren't concerned about comments. */
	/* But if a delimeter is found, handle that. */
	for (p=line; *p; ++p)
	{
		if (*p == '#')
		{
			*p = '\0';
			break;
		}
		/*else */if (*p == '=' && !rest)
		{
			/* separate to distinct strings */
			*p = '\0';
			rest = p+1;
		}
	}
	if (!rest) return; /* No delimeter? Forget it! */

	prefs_allocate(line, rest);
}

static size_t read_line(FILE *prefs, char **line, size_t *len)
{
	size_t pos=0;
	while (fgets((*line)+pos, *len-pos, prefs))
	{
		size_t chunklen = strlen ((*line)+pos);
		pos += chunklen;
		if (!chunklen || (*line)[pos-1] == '\n')
		{
			if (pos)
				(*line)[pos-1] = '\0';

			return pos;
		}
		*len += 256;
		*line = g_realloc(*line, *len);
	}
	return 0;
}

void create_options_window(GtkWidget *widget, gpointer data);

/* Phase 5: prefs path resolution. Primary location is
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
	if (!home || !*home)
		home = g_get_home_dir ();
	if (!home)
		return NULL;
	return g_build_filename (home, ".gtkhxrc", NULL);
}

/* Phase 5: read a GKeyFile [gtkhx] section, feeding each entry through
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
	if (!g_key_file_load_from_file (kf, path,
	                                G_KEY_FILE_KEEP_COMMENTS, &err)) {
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
		gchar *value = g_key_file_get_value (kf, CFG_KEYFILE_GROUP,
		                                     keys[i], NULL);
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

	if (!prefs)
		return FALSE;

	prefsline = g_malloc (prefslinelen);
	while (read_line (prefs, &prefsline, &prefslinelen))
		parse_line (prefsline);

	g_free (prefsline);
	fclose (prefs);
	return TRUE;
}

/* Phase 5: prefs_read intentionally does not run cfgvar changefuncs
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
	gtk_xtext_set_autocopy_text  (gtkhx_prefs.autocopy_text);
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

void prefs_read(void)
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
				fprintf (stderr, "prefs_read: %s: %s\n",
				         path, strerror (errno));
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
				g_message ("Migrating prefs from %s to %s on next save",
				           legacy, path);
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

void prefs_write(void)
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
	                        " Edit values under [" CFG_KEYFILE_GROUP "] or use Settings.",
	                        NULL);

	for (i = 0; i != (int)(sizeof (cfgvars) / sizeof (cfgvars[0])); ++i) {
		struct cfgvar *v = &cfgvars[i];
		switch (v->type) {
		case UINT16:
			g_key_file_set_integer (kf, CFG_KEYFILE_GROUP, v->name,
			                        (gint) *v->variable.uint16);
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
			                      (gint64) *v->variable.timet);
			break;
		case STRING32:
			g_key_file_set_string (kf, CFG_KEYFILE_GROUP, v->name,
			                       v->variable.str32);
			break;
		case BOOLEAN:
			g_key_file_set_boolean (kf, CFG_KEYFILE_GROUP, v->name,
			                        *v->variable.uchar ? TRUE : FALSE);
			break;
		}
	}

	if (!g_key_file_save_to_file (kf, path, &err)) {
		fprintf (stderr, "prefs_write: %s: %s\n",
		         path, err ? err->message : "unknown error");
		fflush (stderr);
		g_clear_error (&err);
	}

	g_key_file_free (kf);
	g_free (path);
}

static void parse_tracker (session *sess)
{
	char *com, *trackers = gtkhx_prefs.tracker_str;
	int i;

	if(gtkhx_prefs.tracker) {
		for (i = 0; i != gtkhx_prefs.num_tracker; ++i) {
			g_free(gtkhx_prefs.tracker[i]);
		}
		g_free(gtkhx_prefs.tracker);
		gtkhx_prefs.tracker = NULL;
	}
	gtkhx_prefs.num_tracker = 0;
	if(!*trackers || !*(trackers+1)) return;
	for (i=0; ; ++i) {
		if (!(com = strchr (trackers, ','))) com = &trackers[strlen(trackers)];
		gtkhx_prefs.num_tracker++;
		gtkhx_prefs.tracker = g_realloc(gtkhx_prefs.tracker,
										(i+1)*sizeof(char*));
		gtkhx_prefs.tracker[i] = g_malloc(com-trackers+1);
		memcpy(gtkhx_prefs.tracker[i], trackers, com-trackers);
		gtkhx_prefs.tracker[i][com-trackers] = '\0';
		if (!*com) break;
		trackers = com+1;
	}
}

static void parse_tracker_list(void)
{
	GtkWidget *list = tracker_list;
	int i = 0;
	size_t len = 0;

	/* go through the CList and populate the gtkhx_prefs.tracker
	   array and create gtkhx_prefs.tracker_str */
	if(gtkhx_prefs.tracker) {
		for (i = 0; i != gtkhx_prefs.num_tracker; ++i) {
			g_free(gtkhx_prefs.tracker[i]);
		}
		g_free(gtkhx_prefs.tracker);
	}

	gtkhx_prefs.num_tracker = GTK_HLIST(list)->rows;
	gtkhx_prefs.tracker = g_malloc(GTK_HLIST(list)->rows * sizeof(char*));
	if ((*cfgvar_for_name(CFG_TRACKER)).allocated) g_free (gtkhx_prefs.tracker_str);
	gtkhx_prefs.tracker_str = g_malloc0(1);

	for(i = 0; i < GTK_HLIST(list)->rows; i++) {
		char *tracker = gtk_hlist_get_row_data(GTK_HLIST(list), i);
		size_t trackersize = strlen(tracker)+1;
		gtkhx_prefs.tracker_str = g_realloc(gtkhx_prefs.tracker_str,
											len+trackersize+1);
		if (i) {
			gtkhx_prefs.tracker_str[len] = ',';
			memcpy(gtkhx_prefs.tracker_str+len+1, tracker, trackersize);
			len++;
		}
		else
			memcpy(gtkhx_prefs.tracker_str+len, tracker, trackersize);
		len += trackersize-1;
		gtkhx_prefs.tracker[i] = g_strdup(tracker);
	}
}

/* Phase 4.5: bookkeeping that runs on every dialog teardown path —
 * Cancel button, OK button (close-on-OK), and the user clicking the
 * window's close X. We attach to GtkWidget::destroy because in GTK 4
 * gtk_window_destroy() does NOT emit close-request: the close-request
 * signal is only fired for user-initiated close attempts (or
 * gtk_window_close()). Hooking destroy catches every path the
 * teardown can take, so options_window never points at a freed
 * GObject the next time create_options_window runs. */
static void close_options_bookkeeping (GtkWidget *widget, gpointer data)
{
	size_t i;
	(void) widget; (void) data;
	options_window = 0;
	g_free(iv);
	iv = NULL;
	/* Phase 5: per-cfgvar widget pointers are populated as Settings
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
	for (i = 0; i < sizeof (cfgvars) / sizeof (cfgvars[0]); i++)
		entry_apply_flush (&cfgvars[i]);
	for (i = 0; i < sizeof (cfgvars) / sizeof (cfgvars[0]); i++)
		cfgvars[i].widget = NULL;
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

	if (!v || v->type != BOOLEAN)
		return;
	if (*v->variable.uchar == new_val)
		return;

	if (v->widget && ADW_IS_SWITCH_ROW (v->widget)) {
		/* The notify::active handler does all the bookkeeping. */
		adw_switch_row_set_active (ADW_SWITCH_ROW (v->widget),
		                           new_val ? TRUE : FALSE);
		return;
	}

	*v->variable.uchar = (unsigned char) new_val;
	pref_apply (v);
}

/* Phase 3.9: GtkFontSelectionDialog was deprecated in GTK 3.2 in favor
 * of GtkFontChooserDialog. The two have entirely different APIs —
 * Selection exposes ok_button / cancel_button widgets you wire up by
 * hand, Chooser uses the GtkDialog "response" signal. The handler
 * below is the GtkChooserDialog equivalent of the old set_font
 * + destroy pair. */
static void
fontsel_response (GtkDialog *dialog, gint response, gpointer user_data)
{
	GtkWidget *entry = user_data;

	if (response == GTK_RESPONSE_OK) {
		char *font = gtk_font_chooser_get_font (GTK_FONT_CHOOSER (dialog));
		if (font) {
			gtk_editable_set_text(GTK_EDITABLE(entry), font);
			g_free (font);
		}
	}
	gtkhx_widget_destroy (GTK_WIDGET (dialog));
}

static void create_fontsel (GtkWidget *btn, GtkWidget *entry)
{
	GtkWidget *fontsel = gtk_font_chooser_dialog_new (_("Browse Fonts"), NULL);
	(void) btn;

	if (gtkhx_prefs.font && *gtkhx_prefs.font)
		gtk_font_chooser_set_font (GTK_FONT_CHOOSER (fontsel),
		                           gtkhx_prefs.font);

	g_signal_connect (fontsel, "response",
	                  G_CALLBACK (fontsel_response), entry);

	gtk_window_present(GTK_WINDOW(fontsel));
}

static void add_tracker(GtkWidget *add, GtkWidget *entry)
{
	char *tracker = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
	gint row;

	row = gtk_hlist_append(GTK_HLIST(tracker_list), &tracker);
	gtk_hlist_set_row_data(GTK_HLIST(tracker_list), row,
						   tracker);
	gtk_editable_set_text(GTK_EDITABLE(entry), "");
	parse_tracker_list();
	prefs_write();
}

static void remove_tracker(GtkWidget *del, GtkWidget *list)
{
	/* Phase 3.2: GtkCList tracked the focused row in a public ->focus_row
	 * field. The gtk_hlist_compat shim is built on GtkTreeView, where
	 * "focus" lives in GtkTreeSelection.  Resolve the selected row's
	 * index via the underlying tree-view selection and feed it to
	 * gtk_hlist_remove. */
	GtkTreeSelection *sel;
	GtkTreeModel *model;
	GtkTreeIter iter;
	GtkTreePath *path;
	gint row;

	sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(list));
	if (!gtk_tree_selection_get_selected(sel, &model, &iter))
		return;

	path = gtk_tree_model_get_path(model, &iter);
	row = gtk_tree_path_get_indices(path)[0];
	gtk_tree_path_free(path);

	gtk_hlist_remove(GTK_HLIST(list), row);
	parse_tracker_list();
	prefs_write();
}

/* Tracker page hosts the existing GtkHList + Add/Remove controls inside
 * a custom AdwPreferencesRow. Phase 5 commit E follow-up: replace the
 * GtkHList with a proper Adw rendering (likely AdwExpanderRow per
 * tracker, or a custom listbox model). For now the embedded layout
 * keeps the existing add/remove flow working under the new shell. */
static void settings_page_tracker (AdwPreferencesPage *page)
{
	AdwPreferencesGroup *grp;
	GtkWidget *row;
	GtkWidget *vbox, *scroll, *ent_hbox, *btnhbox;
	GtkWidget *lbl, *entry, *add_btn, *remove_btn;
	int i, hrow;

	grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (grp, _("Trackers"));
	adw_preferences_group_set_description (grp,
		_("Servers polled when the Tracker window opens"));

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_top    (vbox, 6);
	gtk_widget_set_margin_bottom (vbox, 6);
	gtk_widget_set_margin_start  (vbox, 6);
	gtk_widget_set_margin_end    (vbox, 6);

	scroll = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
	                                GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
	tracker_list = gtk_hlist_new (1);
	gtkhx_widget_set_child (scroll, tracker_list);
	gtk_widget_set_size_request (scroll, -1, 220);
	gtk_box_append (GTK_BOX (vbox), scroll);

	ent_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	lbl = gtk_label_new (_("Address:"));
	entry = gtk_entry_new ();
	gtk_widget_set_hexpand (entry, TRUE);
	gtk_box_append (GTK_BOX (ent_hbox), lbl);
	gtk_box_append (GTK_BOX (ent_hbox), entry);
	gtk_box_append (GTK_BOX (vbox), ent_hbox);

	btnhbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_halign (btnhbox, GTK_ALIGN_END);
	add_btn = gtk_button_new_with_label (_("Add"));
	gtk_widget_add_css_class (add_btn, "suggested-action");
	g_signal_connect (add_btn, "clicked", G_CALLBACK (add_tracker), entry);
	remove_btn = gtk_button_new_with_label (_("Remove"));
	gtk_widget_add_css_class (remove_btn, "destructive-action");
	g_signal_connect (remove_btn, "clicked",
	                  G_CALLBACK (remove_tracker), tracker_list);
	gtk_box_append (GTK_BOX (btnhbox), remove_btn);
	gtk_box_append (GTK_BOX (btnhbox), add_btn);
	gtk_box_append (GTK_BOX (vbox), btnhbox);

	row = adw_preferences_row_new ();
	gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
	gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
	gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), vbox);
	adw_preferences_group_add (grp, row);

	for (i = 0; i < gtkhx_prefs.num_tracker; i++) {
		char *tracker = g_strdup (gtkhx_prefs.tracker[i]);
		hrow = gtk_hlist_append (GTK_HLIST (tracker_list), &tracker);
		gtk_hlist_set_row_data (GTK_HLIST (tracker_list), hrow, tracker);
	}

	adw_preferences_page_add (page, grp);

	/* Phase 5: TRACKER_CASE used to be tucked into Misc → Behavior;
	 * it's a tracker-specific search option, so it lives here in
	 * its own Search group. */
	{
		AdwPreferencesGroup *search_grp =
			ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
		adw_preferences_group_set_title (search_grp, _("Search"));
		adw_preferences_group_add (search_grp,
			pref_switch_row (CFG_TRACKER_CASE,
			                 _("Case-sensitive tracker search"), NULL));
		adw_preferences_page_add (page, search_grp);
	}
}

/* Phase 5: News + Files used to be two separate pages, each with a
 * single Browse-in-Same-Window toggle. They're consolidated here as
 * an "Interface" page with one group per browser. */
static void settings_page_interface (AdwPreferencesPage *page)
{
	AdwPreferencesGroup *files_grp, *news_grp;

	files_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (files_grp, _("File Browsing"));
	adw_preferences_group_add (files_grp,
		pref_switch_row (CFG_FILE_SAMEWIN,
		                 _("Browse in Same Window"),
		                 _("Replace the current window when descending into a folder")));
	adw_preferences_page_add (page, files_grp);

	news_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (news_grp, _("News Folder Browsing"));
	adw_preferences_group_add (news_grp,
		pref_switch_row (CFG_NEWS_SAMEWIN,
		                 _("Browse in Same Window"),
		                 _("Replace the current window when descending into a folder")));
	adw_preferences_page_add (page, news_grp);
}

static void settings_page_sound (AdwPreferencesPage *page)
{
	AdwPreferencesGroup *master, *events;

	master = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (master, _("Sounds"));
	adw_preferences_group_add (master,
		pref_switch_row (CFG_SOUNDS_ON,
		                 _("Play sounds"),
		                 _("Master switch for chat and transfer alerts")));
	adw_preferences_page_add (page, master);

	events = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (events, _("Events"));
	adw_preferences_group_add (events,
		pref_switch_row (CFG_SND_INVITE, _("Chat invitation"), NULL));
	adw_preferences_group_add (events,
		pref_switch_row (CFG_SND_CHAT,   _("Chat message"),    NULL));
	adw_preferences_group_add (events,
		pref_switch_row (CFG_SND_ERROR,  _("Error"),           NULL));
	adw_preferences_group_add (events,
		pref_switch_row (CFG_SND_FILE,   _("Transfer complete"), NULL));
	adw_preferences_group_add (events,
		pref_switch_row (CFG_SND_JOIN,   _("Join"),            NULL));
	adw_preferences_group_add (events,
		pref_switch_row (CFG_SND_LOGIN,  _("Login"),           NULL));
	adw_preferences_group_add (events,
		pref_switch_row (CFG_SND_MSG,    _("Private message"), NULL));
	adw_preferences_group_add (events,
		pref_switch_row (CFG_SND_NEWS,   _("News post"),       NULL));
	adw_preferences_group_add (events,
		pref_switch_row (CFG_SND_PART,   _("Leave"),           NULL));
	adw_preferences_page_add (page, events);
}

/* Phase 5 follow-up: the old standalone Font page only ever applied
 * to the xtext-based chat / private-message widgets, so it folds into
 * the Chat page as a Font group. SHOWJOIN and OLD_NICKCOMP also live
 * here now (moved from Misc → Behavior) since they're chat-window
 * concerns rather than session-wide misc. */
static void settings_page_chat (AdwPreferencesPage *page)
{
	AdwPreferencesGroup *output_grp, *font_grp, *behavior_grp;
	GtkWidget *entry_row, *btn;

	output_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (output_grp, _("Chat output"));
	adw_preferences_group_add (output_grp,
		pref_switch_row (CFG_TIMESTAMP, _("Show timestamps"), NULL));
	adw_preferences_group_add (output_grp,
		pref_switch_row (CFG_WORDWRAP, _("Word wrap"), NULL));
	adw_preferences_group_add (output_grp,
		pref_spin_row   (CFG_XBUF_MAX,
		                 _("Scrollback lines"),
		                 _("0 keeps unlimited scrollback"),
		                 0, 0xffff, 1));
	adw_preferences_page_add (page, output_grp);

	/* Phase 5: timestamp format. Separate group so the strftime
	 * hint can live as a group description without making the
	 * Chat-output group feel cluttered. */
	{
		AdwPreferencesGroup *stamp_grp =
			ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
		adw_preferences_group_set_title (stamp_grp,
			_("Timestamp format"));
		adw_preferences_group_set_description (stamp_grp,
			_("strftime(3) format string. Default: "
			  "\"[%H:%M:%S] \". See `man 3 strftime` for the full "
			  "list of conversion specifiers."));
		adw_preferences_group_add (stamp_grp,
			pref_entry_row (CFG_STAMP_FORMAT, _("Format")));
		adw_preferences_page_add (page, stamp_grp);
	}

	font_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (font_grp, _("Font"));
	adw_preferences_group_set_description (font_grp,
		_("Pango font description, e.g. \"Monospace 11\""));

	entry_row = pref_entry_row (CFG_FONT, _("Font"));

	/* Add a Browse button as a suffix on the entry row so users get a
	 * native font picker without leaving the prefs context. */
	btn = gtk_button_new_with_label (_("Browse"));
	gtk_widget_set_valign (btn, GTK_ALIGN_CENTER);
	g_signal_connect (btn, "clicked",
	                  G_CALLBACK (create_fontsel), entry_row);
	adw_entry_row_add_suffix (ADW_ENTRY_ROW (entry_row), btn);

	adw_preferences_group_add (font_grp, entry_row);
	adw_preferences_page_add (page, font_grp);

	behavior_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (behavior_grp, _("Behavior"));
	adw_preferences_group_add (behavior_grp,
		pref_switch_row (CFG_SHOWJOIN,
		                 _("Show join / leave in chat"), NULL));
	adw_preferences_group_add (behavior_grp,
		pref_switch_row (CFG_OLD_NICKCOMP,
		                 _("Old-style nick completion"),
		                 _("Match against the most recently typed prefix instead of all users")));
	adw_preferences_page_add (page, behavior_grp);

	/* Phase 5: HexChat-style auto-copy controls. Three independent
	 * toggles drive xtext's drag-end clipboard behaviour. The three
	 * gtk_xtext_set_autocopy_* setters take care of propagating the
	 * value to the widget; the changefunc on each cfgvar calls the
	 * matching setter. */
	{
		AdwPreferencesGroup *autocopy_grp =
			ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
		adw_preferences_group_set_title (autocopy_grp,
			_("Auto Copy Behavior"));
		adw_preferences_group_set_description (autocopy_grp,
			_("Drag-select in chat / news / private message text "
			  "to populate the clipboard. Ctrl-V or middle-click "
			  "pastes the selection elsewhere."));
		adw_preferences_group_add (autocopy_grp,
			pref_switch_row (CFG_AUTOCOPY_TEXT,
			                 _("Automatically copy selected text"),
			                 NULL));
		adw_preferences_group_add (autocopy_grp,
			pref_switch_row (CFG_AUTOCOPY_STAMP,
			                 _("Automatically include timestamps"),
			                 NULL));
		adw_preferences_group_add (autocopy_grp,
			pref_switch_row (CFG_AUTOCOPY_COLOR,
			                 _("Automatically include color information"),
			                 NULL));
		adw_preferences_page_add (page, autocopy_grp);
	}
}


/* Phase 5 follow-up: the old standalone General page (just NICK) folds
 * into the Identity page since they're both "who am I to the server"
 * settings. Display name first, then icon ID, then the resource picker.
 *
 * Phase 5 follow-up #2: icon picker is now a GtkFlowBox with 64x64
 * pixel-art icons in a multi-column grid, replacing the legacy
 * 1-row-per-icon GtkHList. See icon_flow_child_activated (above) for
 * the click handler — this used to be icon_row_selected but the
 * flowbox emits child-activated, not select_row, so the dispatch
 * shape changed. */
static void settings_page_identity (AdwPreferencesPage *page)
{
	AdwPreferencesGroup *name_grp, *id_grp, *picker_grp;
	GtkWidget *picker_row, *vbox, *scroll, *icon_list;

	/* Phase 5: g_malloc0 — zero-fill the struct so any read of
	 * iv->icon_list / nfound / icon_high before they're set later in
	 * this function returns 0 / NULL deterministically. The previous
	 * g_malloc gave us a struct full of whatever was at that address,
	 * which is exactly the kind of "crashes without gdb, runs fine
	 * with gdb" Heisenbug glibc's allocator likes to deliver. */
	iv = g_malloc0 (sizeof (struct icon_viewer));

	name_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (name_grp, _("Display name"));
	adw_preferences_group_add (name_grp, pref_entry_row (CFG_NICK, _("Your name")));
	adw_preferences_page_add (page, name_grp);

	id_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (id_grp, _("Identity icon"));
	adw_preferences_group_add (id_grp,
		pref_spin_row (CFG_ICON,
		               _("Icon ID"),
		               _("Numeric ID from the loaded icon resource files"),
		               0, 65535, 1));
	adw_preferences_page_add (page, id_grp);

	picker_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (picker_grp, _("Available icons"));
	adw_preferences_group_set_description (picker_grp,
		_("Click an entry to copy its ID into the field above"));

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_margin_top    (vbox, 6);
	gtk_widget_set_margin_bottom (vbox, 6);
	gtk_widget_set_margin_start  (vbox, 6);
	gtk_widget_set_margin_end    (vbox, 6);

	scroll = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
	                                GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
	/* Phase 5: picker height tuned to ~3 rows of 56px icons + their
	 * labels visible at a time. Was 460 at 96px icons (too dominant);
	 * 320 at 56px keeps the picker present without taking over the
	 * page. */
	gtk_widget_set_size_request (scroll, -1, 320);

	icon_list = gtk_flow_box_new ();
	gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (icon_list),
	                                 GTK_SELECTION_SINGLE);
	gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (icon_list), TRUE);
	/* Two columns minimum, four maximum. With 56px icons, four
	 * columns fits comfortably in the typical settings dialog
	 * width without horizontal cramming. */
	gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (icon_list), 2);
	gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (icon_list), 4);
	gtk_flow_box_set_row_spacing    (GTK_FLOW_BOX (icon_list), 4);
	gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (icon_list), 4);
	gtk_flow_box_set_sort_func (GTK_FLOW_BOX (icon_list),
	                            icon_picker_sort_cb, NULL, NULL);
	g_signal_connect (icon_list, "child-activated",
	                  G_CALLBACK (icon_flow_child_activated), iv);
	gtkhx_widget_set_child (scroll, icon_list);
	gtk_box_append (GTK_BOX (vbox), scroll);

	picker_row = adw_preferences_row_new ();
	gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (picker_row), FALSE);
	gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (picker_row), FALSE);
	gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (picker_row), vbox);
	adw_preferences_group_add (picker_grp, picker_row);

	iv->icon_list = icon_list;
	iv->nfound = 0;
	iv->icon_high = 0;

	adw_preferences_page_add (page, picker_grp);
}

/* Phase 5: Misc used to be a catchall for Behavior toggles. Most of
 * those have moved to the page they belong on (showjoin /
 * old_nickcomp → Chat, tracker_case → Trackers); what stays here is
 * Auto Reply plus the two genuinely cross-cutting behaviours
 * (queue downloads, show pchats at back). */
static void settings_page_misc (AdwPreferencesPage *page)
{
	AdwPreferencesGroup *behavior, *autoreply;

	autoreply = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (autoreply, _("Auto Reply"));
	adw_preferences_group_add (autoreply,
		pref_switch_row (CFG_AUTOREPLY_ON,
		                 _("Enable auto reply"), NULL));
	adw_preferences_group_add (autoreply,
		pref_entry_row (CFG_AUTOREPLY_MSG, _("Reply message")));
	adw_preferences_page_add (page, autoreply);

	behavior = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (behavior, _("Behavior"));
	adw_preferences_group_add (behavior,
		pref_switch_row (CFG_SHOWBACK,
		                 _("Show private messages at back"),
		                 _("Don't raise the chat window when a private message arrives")));
	adw_preferences_group_add (behavior,
		pref_switch_row (CFG_QUEUEDL,
		                 _("Queue file transfers"),
		                 _("Run downloads one at a time instead of in parallel")));
	adw_preferences_page_add (page, behavior);
}

/* Phase 5: General page consolidates Appearance (theme combo) + Paths
 * (download directory). Both pages were small enough to feel
 * silly as standalone sidebar entries — folding them together gives
 * a tidier first stop in the Settings sidebar. */
static void settings_page_general (AdwPreferencesPage *page)
{
	AdwPreferencesGroup *appearance_grp, *paths_grp;
	static const char *vals[]   = {
		CFG_THEME_SYSTEM, CFG_THEME_LIGHT, CFG_THEME_DARK
	};
	const char *labels[3];

	labels[0] = _("Follow system");
	labels[1] = _("Light");
	labels[2] = _("Dark");

	appearance_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (appearance_grp, _("Appearance"));
	adw_preferences_group_set_description (appearance_grp,
		_("Color scheme. \"Follow system\" tracks the desktop's "
		  "light/dark preference."));
	adw_preferences_group_add (appearance_grp,
		pref_combo_row (CFG_THEME, _("Theme"), vals, labels, 3));
	adw_preferences_page_add (page, appearance_grp);

	/* Phase 5: icon and sound paths used to live here too. Auto-
	 * discovery now finds *.rsrc icon packs in ~/.config/gtkhx/icons/
	 * and sound files in ~/.config/gtkhx/sounds/ + the system data
	 * dir, so neither needs a UI row. The ICONS / SOUNDPATH cfgvars
	 * stay defined so legacy gtkhxrc files still parse cleanly. */
	paths_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
	adw_preferences_group_set_title (paths_grp, _("Paths"));
	adw_preferences_group_add (paths_grp,
		pref_entry_row (CFG_DOWNLOAD, _("Download directory")));
	adw_preferences_page_add (page, paths_grp);
}

/* Helper: build a fresh AdwPreferencesPage with title + icon and run the
 * draw_func against it. Centralizes the metadata so adding pages stays
 * a one-liner. */
static void
settings_add_page (AdwPreferencesDialog *dlg,
                   const char *title,
                   const char *icon,
                   void (*draw_func) (AdwPreferencesPage *))
{
	AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
	adw_preferences_page_set_title (page, title);
	if (icon)
		adw_preferences_page_set_icon_name (page, icon);
	if (draw_func)
		draw_func (page);
	adw_preferences_dialog_add (dlg, page);
}

void create_options_window (GtkWidget *widget, gpointer data)
{
	AdwPreferencesDialog *dlg;
	GtkWidget *parent;
	session *sess = data;

	(void) widget;

	parent = GTK_WIDGET (gtkhx_active_window ());

	if (options_window) {
		adw_dialog_present (ADW_DIALOG (options_window), parent);
		return;
	}

	/* Phase 5: AdwPreferencesDialog (libadwaita 1.6+) replaces
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
	adw_dialog_set_title (ADW_DIALOG (dlg), _("GtkHx Preferences"));
	adw_dialog_set_content_width  (ADW_DIALOG (dlg), 840);
	adw_dialog_set_content_height (ADW_DIALOG (dlg), 640);

	g_object_set_data (G_OBJECT (dlg), "sess", sess);
	g_signal_connect (dlg, "closed",
	                  G_CALLBACK (close_options_bookkeeping), NULL);

	options_window = GTK_WIDGET (dlg);

	settings_add_page (dlg, _("General"),   "preferences-system-symbolic",
	                   settings_page_general);
	settings_add_page (dlg, _("Identity"),  "user-info-symbolic",
	                   settings_page_identity);
	settings_add_page (dlg, _("Chat"),      "user-available-symbolic",
	                   settings_page_chat);
	settings_add_page (dlg, _("Sound"),     "audio-speakers-symbolic",
	                   settings_page_sound);
	settings_add_page (dlg, _("Interface"), "view-list-symbolic",
	                   settings_page_interface);
	settings_add_page (dlg, _("Trackers"),  "network-server-symbolic",
	                   settings_page_tracker);
	settings_add_page (dlg, _("Misc"),      "applications-other-symbolic",
	                   settings_page_misc);

	adw_dialog_present (ADW_DIALOG (dlg), parent);

	/* Populate the icon picker now that its hlist exists. list_icons
	 * walks the loaded resource files and inserts a row per icon. */
	list_icons ();
}

G_GNUC_END_IGNORE_DEPRECATIONS
/* Phase 4.13: end of file-level deprecation suppression — see top of file. */
