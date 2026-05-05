/*
 * Copyright (C) 2000-2002 Misha Nasledov <misha@nasledov.com>
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
	0,
	0,
	PREFIX"/share/gtkhx/sounds",
	"",
	"fixed",
	".",
	NULL,
	"hltracker.com",
	NULL,
	PREFIX "/share/gtkhx/icons.rsrc",
	"play",
	500,
	{
		{412, 312, 10, 434, 0, 1},
		{412, 384, 10, 50, 0, 1},
		{0,   0,   442, 0, 0, 0},
		{300, 250, 442, 480, 0, 1},
		{300, 400, 442, 50, 0, 1}
	},
	1,
	1,
	0,
	0,
	0,
	0,
	0,
	1,
	1,
	0,
	0,
	0,
	0,
	0,
	0
};

static void parse_tracker (session *);
static void parse_icons (session *);

struct icon_viewer {
	guint32 icon_high;
	unsigned int nfound;
	GtkWidget *icon_list;
};

void list_icons (void)
{
	/* Phase 3.4: cicn_to_pixbuf returns a GdkPixbuf directly with the
	 * Mac classic mask folded into the alpha channel, so the original
	 * GdkImage/GdkPixmap/GdkGC dance collapses to a single decode +
	 * crop.  Wide icons (the 32x32 family bundles four variants in a
	 * 4*32-pixel row) are clipped to the rightmost 32 px to mirror the
	 * historical "off = width > 400 ? 198 : 0" hack. */
	GtkWidget *icon_list = iv->icon_list;
	gchar *text[2] = {NULL, NULL};
	char buf[16];
	guint16 nres;
	guint32 icon;
	unsigned int nfound = 0;
	int i;

	text[1] = buf;
	gtk_hlist_freeze (GTK_HLIST (icon_list));
	for (i = 0; i < icon_files.n; ++i) {
		if (!icon_files.cicns[i])
			continue;
		nres = macres_file_num_res_of_type (icon_files.cicns[i], TYPE_cicn);
		for (icon = 0; icon < nres; icon++) {
			macres_res *r;
			GdkPixbuf *pb, *cropped;
			int width, height, off;
			gint row;

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

			nfound++;
			g_snprintf (buf, sizeof (buf), "%u", r->resid);
			row = gtk_hlist_append (GTK_HLIST (icon_list), text);
			gtk_hlist_set_row_data (GTK_HLIST (icon_list), row,
			                        GUINT_TO_POINTER (r->resid));
			/* Phase 5 dark-theme: no per-row foreground override —
			 * theme default applies, so the resid label reads on
			 * both light and dark themes. */
			gtk_hlist_set_pixtext (GTK_HLIST (icon_list), row, 0, "", 34,
			                       pb, NULL);
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
		gtk_hlist_sort (GTK_HLIST (icon_list));
	gtk_hlist_thaw (GTK_HLIST (icon_list));
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

/*
  static void changed_nickoricon (session *sess)
  {
  hx_change_name_icon(&the_session.htlc);
  }
*/

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
	dfasyntax ((RE_CHAR_CLASSES | RE_CONTEXT_INDEP_ANCHORS |
				RE_CONTEXT_INDEP_OPS | RE_HAT_LISTS_NOT_NEWLINE |
				RE_NEWLINE_ALT | RE_NO_BK_PARENS | RE_NO_BK_VBAR),
				!gtkhx_prefs.track_case, '\n');
}

static void changed_filesamewin (session *sess)
{
	struct gfile_list *gfl;

	for(gfl = gfile_list; gfl; gfl = gfl->prev) {
		gtk_widget_set_sensitive(gfl->up_btn, gtkhx_prefs.file_samewin);
	}
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
	{"AUTOREPLYMSG", {&gtkhx_prefs.auto_reply_msg}, STRING, 0, NULL, NULL},
	{"AUTOREPLYON", {&gtkhx_prefs.auto_reply}, BOOLEAN, 0, NULL, NULL},
	{"CHATXPOS", {&gtkhx_prefs.geo.chat.xpos}, INT, 0, NULL, NULL},
	{"CHATXSIZE", {&gtkhx_prefs.geo.chat.xsize}, INT, 0, NULL, NULL},
	{"CHATYPOS", {&gtkhx_prefs.geo.chat.ypos}, INT, 0, NULL, NULL},
	{"CHATYSIZE", {&gtkhx_prefs.geo.chat.ysize}, INT, 0, NULL, NULL},
	{"DOWNLOAD", {&gtkhx_prefs.download_path}, STRING, 0, changed_downloadpath,
	 NULL},
	{"FILE_SAMEWINDOW", {&gtkhx_prefs.file_samewin}, BOOLEAN, 0, 
	 changed_filesamewin, NULL},
	{"NEWS_SAMEWINDOW", {&gtkhx_prefs.news_samewin}, BOOLEAN, 0, 
	 changed_newssamewin, NULL},
	{"FONT", {&gtkhx_prefs.font}, STRING, 0, changed_font, NULL},
	{"ICON", {&the_session.htlc.icon}, UINT16, 0, /*changed_nickoricon*/NULL, NULL},
	{"ICONS", {&gtkhx_prefs.icon_str}, STRING, 0, parse_icons, NULL},
#if 0 /* XXX */
	{"LOGGING", {&gtkhx_prefs.logging}, BOOLEAN, 0, changed_logging, NULL},
#endif 
	{"NEWSXPOS", {&gtkhx_prefs.geo.news.xpos}, INT, 0, NULL, NULL},
	{"NEWSXSIZE", {&gtkhx_prefs.geo.news.xsize}, INT, 0, NULL, NULL},
	{"NEWSYPOS", {&gtkhx_prefs.geo.news.ypos}, INT, 0, NULL, NULL},
	{"NEWSYSIZE", {&gtkhx_prefs.geo.news.ysize}, INT, 0, NULL, NULL},
	{"NICK", {the_session.htlc.name}, STRING32, 0, /*changed_nickoricon*/NULL, NULL},
	{"OLD_NICKCOMPLETION", {&gtkhx_prefs.old_nickcompletion}, BOOLEAN, 0, NULL,
	 NULL},
	{"OPENCHAT", {&gtkhx_prefs.geo.chat.init}, BOOLEAN, 0, NULL, NULL},
	{"OPENNEWS", {&gtkhx_prefs.geo.news.init}, BOOLEAN, 0, NULL, NULL},
	{"OPENTASKS", {&gtkhx_prefs.geo.tasks.init}, BOOLEAN, 0, NULL, NULL},
	{"OPENUSERS", {&gtkhx_prefs.geo.users.init}, BOOLEAN, 0, NULL, NULL},
	{"QUEUEDL", {&gtkhx_prefs.queuedl}, BOOLEAN, 0, NULL, NULL},
	{"SHOWBACK", {&gtkhx_prefs.showback}, BOOLEAN, 0, NULL, NULL},
	{"SHOWJOIN", {&gtkhx_prefs.showjoin}, BOOLEAN, 0, NULL, NULL},
	{"SND_CMD", {&gtkhx_prefs.snd_cmd}, STRING, 0, NULL, NULL},
	{"SOUNDCHAT", {&hxsnd.chat}, BOOLEAN, 0, NULL, NULL},
	{"SOUNDERROR", {&hxsnd.error}, BOOLEAN, 0, NULL, NULL},
	{"SOUNDFILE", {&hxsnd.file}, BOOLEAN, 0, NULL, NULL},
	{"SOUNDINVITE", {&hxsnd.invite}, BOOLEAN, 0, NULL, NULL},
	{"SOUNDJOIN", {&hxsnd.join}, BOOLEAN, 0, NULL, NULL},
	{"SOUNDLOGIN", {&hxsnd.login}, BOOLEAN, 0, NULL, NULL},
	{"SOUNDMSG", {&hxsnd.msg}, BOOLEAN, 0, NULL, NULL},
	{"SOUNDNEWS", {&hxsnd.news}, BOOLEAN, 0, NULL, NULL},
	{"SOUNDPART", {&hxsnd.part}, BOOLEAN, 0, NULL, NULL},
	{"SOUNDPATH", {&gtkhx_prefs.sound_path}, STRING, 0, NULL, NULL},
	{"SOUNDSON", {&hxsnd.on}, BOOLEAN, 0, NULL, NULL},
	{"TASKXPOS", {&gtkhx_prefs.geo.tasks.xpos}, INT, 0, NULL, NULL},
	{"TASKXSIZE", {&gtkhx_prefs.geo.tasks.xsize}, INT, 0, NULL, NULL},
	{"TASKYPOS", {&gtkhx_prefs.geo.tasks.ypos}, INT, 0, NULL, NULL},
	{"TASKYSIZE", {&gtkhx_prefs.geo.tasks.ysize}, INT, 0, NULL, NULL},
	{"TIME", {&total_time}, TIME_T, 0, NULL, NULL},
	{"TIMESTAMP", {&gtkhx_prefs.timestamp}, BOOLEAN, 0, NULL, NULL},
	{"TOOLXPOS", {&gtkhx_prefs.geo.tool.xpos}, INT, 0, NULL, NULL},
	{"TOOLYPOS", {&gtkhx_prefs.geo.tool.ypos}, INT, 0, NULL, NULL},
	{"TRACKER", {&gtkhx_prefs.tracker_str}, STRING, 0, parse_tracker, NULL},
	{"TRACKER_CASE", {&gtkhx_prefs.track_case}, BOOLEAN, 0, changed_case, NULL},
	{"USERXPOS", {&gtkhx_prefs.geo.users.xpos}, INT, 0, NULL, NULL},
	{"USERXSIZE", {&gtkhx_prefs.geo.users.xsize}, INT, 0, NULL, NULL},
	{"USERYPOS", {&gtkhx_prefs.geo.users.ypos}, INT, 0, NULL, NULL},
	{"USERYSIZE", {&gtkhx_prefs.geo.users.ysize}, INT, 0, NULL, NULL},
	{"WORDWRAP", {&gtkhx_prefs.word_wrap}, BOOLEAN, 0, changed_xtext, NULL},
	{"XBUF_MAX", {&gtkhx_prefs.xbuf_max}, INT, 0, changed_xtext, NULL}
};

enum
{
	AUTOREPLYMSG_IDX,
	AUTOREPLYON_IDX,
	CHATXPOS_IDX,
	CHATXSIZE_IDX,
	CHATYPOS_IDX,
	CHATYSIZE_IDX,
	DOWNLOAD_IDX,
	FILE_SAMEWINDOW_IDX,
	NEWS_SAMEWINDOW_IDX,
	FONT_IDX,
	ICON_IDX,
	ICONS_IDX,
#if 0 /* XXX */
	LOGGING_IDX,
#endif
	NEWSXPOS_IDX,
	NEWSXSIZE_IDX,
	NEWSYPOS_IDX,
	NEWSYSIZE_IDX,
	NICK_IDX,
	OLD_NICKCOMPLETION_IDX,
	OPENCHAT_IDX,
	OPENNEWS_IDX,
	OPENTASKS_IDX,
	OPENUSERS_IDX,
	QUEUEDL_IDX,
	SHOWBACK_IDX,
	SHOWJOIN_IDX,
	SND_CMD_IDX,
	SOUNDCHAT_IDX,
	SOUNDERROR_IDX,
	SOUNDFILE_IDX,
	SOUNDINVITE_IDX,
	SOUNDJOIN_IDX,
	SOUNDLOGIN_IDX,
	SOUNDMSG_IDX,
	SOUNDNEWS_IDX,
	SOUNDPART_IDX,
	SOUNDPATH_IDX,
	SOUNDSON_IDX,
	TASKXPOS_IDX,
	TASKXSIZE_IDX,
	TASKYPOS_IDX,
	TASKYSIZE_IDX,
	TIME_IDX,
	TIMESTAMP_IDX,
	TOOLXPOS_IDX,
	TOOLYPOS_IDX,
	TRACKER_IDX,
	TRACKER_CASE_IDX,
	USERXPOS_IDX,
	USERXSIZE_IDX,
	USERYPOS_IDX,
	USERYSIZE_IDX,
	WORDWRAP_IDX,
	XBUF_MAX_IDX
};

void init_variables(void) /* default settings if prefs file is not found. */
{
	gtkhx_prefs.font = g_strdup ("Monospace 10");
	gtkhx_font_desc = pango_font_description_from_string (gtkhx_prefs.font);
	cfgvars[FONT_IDX].allocated = 1;


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
	parse_icons(NULL);

	start_time = time(NULL);
}

static int cfgnamecmp (char *key, struct cfgvar *mem)
{
	return strcmp(key, mem->name);
}

static void prefs_allocate(char *tag, char *rest)
{
	struct cfgvar *result;
	result= bsearch (tag, cfgvars, sizeof(cfgvars)/sizeof(cfgvars[0]),
			sizeof(cfgvars[0]), (int (*)(const void*, const void*))cfgnamecmp);

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
			unsigned char c;
			if (*rest == '0') c = 0;
			else if (*rest == '1') c = 1;
			else return;
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

void prefs_read(void)
{
	char *path = g_strdup_printf("%s/.gtkhxrc", getenv("HOME"));
	FILE *prefs = fopen(path, "r");
	char *prefsline;
	size_t prefslinelen = 256;

	prefsline = g_malloc (prefslinelen);

	if(!prefs) {
		/* file doesn't exist */
		if(errno == ENOENT) {
			create_options_window(NULL, NULL);
		}
		else {
			fprintf(stderr, "prefs_read: %s: %s\n",
					path, strerror(errno));
			fflush(stderr);
		}

		g_free(path);
		return;
	}

	while(read_line(prefs, &prefsline, &prefslinelen))
		parse_line(prefsline);

	g_free(prefsline);
	fclose(prefs);
	g_free(path);
}

void prefs_write(void)
{
	char *path = g_strdup_printf("%s/.gtkhxrc", getenv("HOME"));
	FILE *prefs = fopen(path, "w");
	time_t now;
	int i;

	if(!prefs) {
		fprintf(stderr, "prefs_write: %s: %s\n",
				path, strerror(errno));
		fflush(stderr);

		g_free(path);
		return;
	}

	now = time(NULL);
	total_time += (now-start_time);
	start_time = now;

	fprintf(prefs, "# This is the GtkHx preferences file\n");
	fprintf(prefs, "# Lines starting with '#' are comments\n\n");

	for (i=0; i != sizeof(cfgvars)/sizeof(cfgvars[0]); ++i)
	{
		struct cfgvar *v = &cfgvars[i];
		switch (v->type)
		{
			case UINT16:
				fprintf(prefs, "%s=%u\n", v->name, *v->variable.uint16);
				break;
			case STRING:
				fprintf(prefs, "%s=%s\n", v->name, *v->variable.str);
				break;
			case INT:
				fprintf(prefs, "%s=%d\n", v->name, *v->variable.integer);
				break;
			case TIME_T:
				fprintf(prefs, "%s=%ld\n", v->name, *v->variable.timet);
				break;
			case STRING32:
				fprintf(prefs, "%s=%s\n", v->name, v->variable.str32);
				break;
			case BOOLEAN:
				fprintf(prefs, "%s=%d\n", v->name, *v->variable.uchar);
				break;
		}
	}

	fclose(prefs);
	g_free(path);
}

static void parse_icons (session *sess)
{
	char *com, *icons = gtkhx_prefs.icon_str;
	int i;

	if(gtkhx_prefs.icon) {
		for (i = 0; i != gtkhx_prefs.num_icons; ++i) {
			g_free(gtkhx_prefs.icon[i]);
		}
		g_free(gtkhx_prefs.icon);
		gtkhx_prefs.icon = NULL;
	}
	gtkhx_prefs.num_icons = 0;
	if(!*icons || !*(icons+1)) return;
	for (i=0; ; ++i) {
		if (!(com = strchr (icons, ','))) com = &icons[strlen(icons)];
		gtkhx_prefs.num_icons++;
		gtkhx_prefs.icon = g_realloc(gtkhx_prefs.icon, (i+1)*sizeof(char*));
		gtkhx_prefs.icon[i] = g_malloc(com-icons+1);
		memcpy(gtkhx_prefs.icon[i], icons, com-icons);
		gtkhx_prefs.icon[i][com-icons] = '\0';
		if (!*com) break;
		icons = com+1;
	}
	init_icons();
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
	if (cfgvars[TRACKER_IDX].allocated) g_free (gtkhx_prefs.tracker_str);
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
	(void) widget; (void) data;
	options_window = 0;
	g_free(iv);
	iv = NULL;
}

/* Cancel button: destroy the dialog. The destroy signal handler clears
 * the singleton state. */
static void close_options_window_cancel (GtkWidget *btn, gpointer data)
{
	(void) btn; (void) data;
	if (options_window)
		gtk_window_destroy(GTK_WINDOW(options_window));
}


void options_change (GtkWidget *widget, gpointer data)
{
	int i;

	session *sess = g_object_get_data(G_OBJECT(options_window), "sess");

	for (i=0; i != sizeof(cfgvars)/sizeof(cfgvars[0]); ++i)
	{
		struct cfgvar *v = &cfgvars[i];
		if (!v->widget) continue;
		switch (v->type) {
		case UINT16:
		{
			char *str = gtk_editable_get_text(GTK_EDITABLE(v->widget));
			guint16 g;;
			
			if(!str) continue;
			g = atou16(str);
			if (g == *v->variable.uint16) continue;
			*v->variable.uint16 = g;
			break;
			}
			case INT:
			{
				char *st = gtk_editable_get_text(GTK_EDITABLE(v->widget));

				int in;
				if(!st) continue;
				in = atoi(st);
				if (in == *v->variable.integer) continue;
				*v->variable.integer = in;
				break;
			}
			case STRING32:
			{
				char *s = gtk_editable_get_text(GTK_EDITABLE(v->widget));
				if(!s) continue;
				if (!strcmp(s, v->variable.str32)) continue;
				strncpy(v->variable.str32, s, 31);
				v->variable.str32[31] = '\0';
				break;
			}
			case STRING:
			{
				char *string = gtk_editable_get_text(GTK_EDITABLE(v->widget));
				if(!string) continue;
/*				if (!*v->variable.str || strcmp (*v->variable.str, string))
				{
					if (v->allocated)
						g_free (*v->variable.str);
					*v->variable.str = g_strdup(string);
					v->allocated = 1;
					break;
					}*/
				if(*v->variable.str && !strcmp(string, *v->variable.str)) 
					continue;
				if(v->allocated)
					g_free(*v->variable.str);
				*v->variable.str = g_strdup(string);
				v->allocated = 1;
				break;
			}
			case BOOLEAN:
			{
				unsigned char b = gtk_check_button_get_active(
					(GtkCheckButton*)v->widget);
				if (b == *v->variable.uchar) continue;
				*v->variable.uchar = b;
				break;
			}
		}
		if (v->changefunc)
			(*(v->changefunc))(sess);
	}

	if(connected) {
		hx_change_name_icon(&the_session.htlc);
	}

	parse_tracker_list();

	prefs_write();

	if(!GPOINTER_TO_INT(data)) {
		/* Phase 4.5: explicit close from the OK button — destroy
		 * the dialog, which fires close-request and runs the
		 * bookkeeping in close_options_window_request. */
		if (options_window)
			gtk_window_destroy(GTK_WINDOW(options_window));
	}
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

static GtkWidget *
settings_create_group (GtkWidget * vvbox, gchar * title)
{
	GtkWidget *frame;
	GtkWidget *vbox;

	frame = gtk_frame_new (title);
	gtkhx_box_pack(vvbox, frame, FALSE, FALSE, 0);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	(gtk_widget_set_margin_start(vbox, 2), gtk_widget_set_margin_end(vbox, 2), gtk_widget_set_margin_top(vbox, 2), gtk_widget_set_margin_bottom(vbox, 2));
	gtkhx_widget_set_child(frame, vbox);

	return vbox;
}

static void add_tracker(GtkWidget *add, GtkWidget *entry)
{
	char *tracker = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
	gint row;

	row = gtk_hlist_append(GTK_HLIST(tracker_list), &tracker);
	gtk_hlist_set_row_data(GTK_HLIST(tracker_list), row,
						   tracker);
	gtk_editable_set_text(GTK_EDITABLE(entry), "");
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
}

static void settings_page_tracker (GtkWidget *vbox)
{
	GtkWidget *wid;
	GtkWidget *btnhbox, *ent_hbox;
	GtkWidget *add, *remove;
	GtkWidget *lbl, *entry;
	GtkWidget *scroll;
	int i, row;

	wid = settings_create_group(vbox, _("Trackers"));

	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);

	tracker_list = gtk_hlist_new(1);
	gtkhx_widget_set_child(scroll, tracker_list);

	gtkhx_box_pack(wid, scroll, 0, 0, 0);
	gtk_widget_set_size_request(scroll, 232, 246);

	ent_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtkhx_box_pack(wid, ent_hbox, 0, 0, 0);

	lbl = gtk_label_new(_("Address: "));
	entry = gtk_entry_new();

	gtkhx_box_pack(ent_hbox, lbl, 0, 0, 0);
	gtkhx_box_pack(ent_hbox, entry, 0, 0, 0);

	btnhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtkhx_box_pack(wid, btnhbox, 0, 0, 0);

	add = gtk_button_new_with_label(_("Add"));
	g_signal_connect(add, "clicked",
					   G_CALLBACK(add_tracker), entry);

	remove = gtk_button_new_with_label(_("Remove"));
	g_signal_connect(remove, "clicked",
					   G_CALLBACK(remove_tracker),
					   tracker_list);

	gtkhx_box_pack(btnhbox, add, 0, 0, 0);
	gtkhx_box_pack(btnhbox, remove, 0, 0, 0);

	for(i = 0; i < gtkhx_prefs.num_tracker; i++) {
		char *tracker = g_strdup(gtkhx_prefs.tracker[i]);

		row = gtk_hlist_append(GTK_HLIST(tracker_list),
							   &tracker);
		gtk_hlist_set_row_data(GTK_HLIST(tracker_list), row,
							   tracker);
	}
}

static void settings_page_news15 (GtkWidget *vbox)
{
	GtkWidget *wid;
	GtkWidget *table;

	wid = settings_create_group(vbox, _("News Folder Browsing"));
	table = gtkhx_grid_new_table(1, 1, 0);

	cfgvars[NEWS_SAMEWINDOW_IDX].widget = gtk_check_button_new_with_label(
		_("Browse in Same Window"));
	gtk_check_button_set_active((GtkCheckButton*)cfgvars[NEWS_SAMEWINDOW_IDX].widget,
								 gtkhx_prefs.news_samewin);

	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[NEWS_SAMEWINDOW_IDX].widget, 0,
										1, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);


	gtkhx_box_pack(wid, table, 0, 0, 0);
}


static void settings_page_files(GtkWidget *vbox)
{
	GtkWidget *wid;
	GtkWidget *table;

	wid = settings_create_group(vbox, _("File Browsing"));
	table = gtkhx_grid_new_table(1, 1, 0);

	cfgvars[FILE_SAMEWINDOW_IDX].widget = gtk_check_button_new_with_label(
		_("Browse in Same Window"));
	gtk_check_button_set_active((GtkCheckButton*)cfgvars[FILE_SAMEWINDOW_IDX].widget,
								 gtkhx_prefs.file_samewin);

	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[FILE_SAMEWINDOW_IDX].widget, 0,
										1, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);


	gtkhx_box_pack(wid, table, 0, 0, 0);
}

#if 0 /* XXX */
static void settings_page_logging (GtkWidget *vbox)
{
	GtkWidget *wid;
	GtkWidget *table;

	wid = settings_create_group(vbox, _("Logging"));
	table = gtkhx_grid_new_table(1, 1, 0);

	cfgvars[LOGGING_IDX].widget = gtk_check_button_new_with_label(
		_("Log Chats/Private Messages"));
	gtk_check_button_set_active((GtkCheckButton*)cfgvars[LOGGING_IDX].widget,
								 gtkhx_prefs.logging);

	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[LOGGING_IDX].widget, 0,
										1, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);


	gtkhx_box_pack(wid, table, 0, 0, 0);
}
#endif

static void settings_page_sound(GtkWidget *vbox)
{
	GtkWidget *wid;
	GtkWidget *table;
	GtkWidget *table2;
	GtkWidget *lbl;

	wid = settings_create_group(vbox, _("Sound Command"));
	table2 = gtkhx_grid_new_table(1, 2, 0);

	table = gtkhx_grid_new_table(10, 2, 0);
	gtk_grid_set_column_spacing(GTK_GRID(table2), 5);

	lbl = gtk_label_new(_("Sound Command: "));
	gtkhx_grid_attach_table(GTK_GRID(table2), lbl, 0, 1, 2, 3,
					 GTK_FILL, GTK_FILL, 0, 0);

	cfgvars[SND_CMD_IDX].widget = gtk_entry_new();
	/* Phase 4.x: GtkEntry.text is now on the GtkEditable interface.
	 * gtk_entry_set_text/get_text were dropped — use the editable APIs. */
	gtk_editable_set_text(GTK_EDITABLE(cfgvars[SND_CMD_IDX].widget),
						   gtkhx_prefs.snd_cmd);
	gtkhx_grid_attach_table(GTK_GRID(table2), cfgvars[SND_CMD_IDX].widget, 1, 2, 2, 3,
					 (GTK_EXPAND|GTK_FILL), 0, 0, 0);

	gtkhx_box_pack(wid, table2, 0, 0, 0);

	wid = settings_create_group(vbox, _("Sounds"));

	cfgvars[SOUNDSON_IDX].widget = gtk_check_button_new_with_label(_("Play Sounds For:"));
	gtk_check_button_set_active((GtkCheckButton*)cfgvars[SOUNDSON_IDX].widget, hxsnd.on);
	cfgvars[SOUNDINVITE_IDX].widget = gtk_check_button_new_with_label(_("Chat Invitation"));
	gtk_check_button_set_active((GtkCheckButton*)cfgvars[SOUNDINVITE_IDX].widget,
								 hxsnd.invite);
	cfgvars[SOUNDCHAT_IDX].widget = gtk_check_button_new_with_label(_("Chat"));
	gtk_check_button_set_active((GtkCheckButton*)cfgvars[SOUNDCHAT_IDX].widget,
								 hxsnd.chat);
	cfgvars[SOUNDERROR_IDX].widget = gtk_check_button_new_with_label(_("Error"));
	gtk_check_button_set_active((GtkCheckButton*)cfgvars[SOUNDERROR_IDX].widget,
								 hxsnd.error);
	cfgvars[SOUNDFILE_IDX].widget = gtk_check_button_new_with_label(
		_("File Transfer Complete"));
	gtk_check_button_set_active((GtkCheckButton*)cfgvars[SOUNDFILE_IDX].widget,
								 hxsnd.file);
	cfgvars[SOUNDJOIN_IDX].widget = gtk_check_button_new_with_label(_("Join"));
	gtk_check_button_set_active((GtkCheckButton*)cfgvars[SOUNDJOIN_IDX].widget,
								 hxsnd.join);
	cfgvars[SOUNDLOGIN_IDX].widget = gtk_check_button_new_with_label(_("Login"));
	gtk_check_button_set_active((GtkCheckButton*)cfgvars[SOUNDLOGIN_IDX].widget,
								 hxsnd.login);
	cfgvars[SOUNDMSG_IDX].widget = gtk_check_button_new_with_label(_("Private Message"));
	gtk_check_button_set_active((GtkCheckButton*)cfgvars[SOUNDMSG_IDX].widget, hxsnd.msg);
	cfgvars[SOUNDNEWS_IDX].widget = gtk_check_button_new_with_label(_("News"));
	gtk_check_button_set_active((GtkCheckButton*)cfgvars[SOUNDNEWS_IDX].widget,
								 hxsnd.news);
	cfgvars[SOUNDPART_IDX].widget = gtk_check_button_new_with_label(_("Leave"));
	gtk_check_button_set_active((GtkCheckButton*)cfgvars[SOUNDPART_IDX].widget,
								 hxsnd.part);

	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[SOUNDSON_IDX].widget, 0, 1, 0, 1,
					 GTK_EXPAND|GTK_FILL, 0, 0, 4);
	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[SOUNDINVITE_IDX].widget, 1, 2, 1, 2,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[SOUNDCHAT_IDX].widget, 1, 2, 2, 3,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[SOUNDERROR_IDX].widget, 1, 2, 3, 4,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[SOUNDFILE_IDX].widget, 1, 2, 4, 5,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[SOUNDJOIN_IDX].widget, 1, 2, 5, 6,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[SOUNDLOGIN_IDX].widget, 1, 2, 6, 7,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[SOUNDMSG_IDX].widget, 1, 2, 7, 8,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[SOUNDNEWS_IDX].widget, 1, 2, 8, 9,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[SOUNDPART_IDX].widget, 1, 2, 9, 10,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtkhx_box_pack(wid, table, 0, 0, 0);
}

static void settings_page_font(GtkWidget *vbox)
{
	GtkWidget *wid;
	GtkWidget *table;
	GtkWidget *table2;
	GtkWidget *lbl;
	GtkWidget *btn;

	wid = settings_create_group(vbox, _("Fonts"));

	table = gtkhx_grid_new_table(1, 2, 0);

	lbl = gtk_label_new(_("Font: "));
	gtkhx_grid_attach_table(GTK_GRID(table), lbl, 0, 1, 2, 3,
					 GTK_FILL, GTK_FILL, 0, 0);

	cfgvars[FONT_IDX].widget = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(cfgvars[FONT_IDX].widget), gtkhx_prefs.font);
	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[FONT_IDX].widget, 1, 2, 2, 3,
					 (GTK_EXPAND|GTK_FILL), 0, 0, 0);

	gtkhx_box_pack(wid, table, 0, 0, 0);

	table2 = gtkhx_grid_new_table(2, 1, 0);

	btn = gtk_button_new_with_label(_("Browse Fonts"));
	gtkhx_grid_attach_table(GTK_GRID(table2), btn, 0, 1, 4, 5, GTK_FILL, GTK_FILL, 0,
					 0);
	g_signal_connect(btn, "clicked",
					   G_CALLBACK(create_fontsel), cfgvars[FONT_IDX].widget);

	gtkhx_box_pack(wid, table2, 0, 0, 0);
}

static void settings_page_xtext(GtkWidget *vbox)
{
	GtkWidget *lbl;
	GtkWidget *wid;
	GtkWidget *table2;
	GtkAdjustment *adj;

	wid = settings_create_group(vbox, _("Miscellaeneous"));

	table2 = gtkhx_grid_new_table(2, 2, 0);

	cfgvars[TIMESTAMP_IDX].widget = gtk_check_button_new_with_label(
		_("Timestamp Chat"));
	gtk_check_button_set_active((GtkCheckButton*)cfgvars[TIMESTAMP_IDX].widget,
								 gtkhx_prefs.timestamp);
	gtkhx_grid_attach_table(GTK_GRID(table2), cfgvars[TIMESTAMP_IDX].widget, 0, 1, 0, 1,
					 GTK_FILL, 0, 0, 0);

	cfgvars[WORDWRAP_IDX].widget = gtk_check_button_new_with_label(
		_("Word Wrap"));
	gtk_check_button_set_active((GtkCheckButton*)cfgvars[WORDWRAP_IDX].widget,
								 gtkhx_prefs.word_wrap);
	gtkhx_grid_attach_table(GTK_GRID(table2), cfgvars[WORDWRAP_IDX].widget, 1, 2, 0, 1,
					 GTK_FILL, 0, 0, 0);

	adj = (GtkAdjustment *)gtk_adjustment_new(gtkhx_prefs.xbuf_max, 0, 0xffff,
											  1, 1, 1);
	cfgvars[XBUF_MAX_IDX].widget = gtk_spin_button_new(adj, 1, 0);
	lbl = gtk_label_new(_("Maximum Lines (0 = Unlimited)    "));

	gtkhx_grid_attach_table(GTK_GRID(table2), lbl, 0, 1, 1, 2, GTK_FILL, 0, 0, 0);
	gtkhx_grid_attach_table(GTK_GRID(table2), cfgvars[XBUF_MAX_IDX].widget, 1, 2, 1, 2,
					 GTK_FILL, 0, 0, 0);

	gtkhx_box_pack(wid, table2, 0, 0, 0);
}

static void settings_page_path(GtkWidget *vbox)
{
	GtkWidget *wid;
	GtkWidget *table;
	GtkWidget *lbl;
	GtkWidget *desc;

	wid = settings_create_group(vbox, _("Notes"));

	desc = gtk_label_new(_("You can have more than one icon list.\n"
						   "Enter as many icon lists as you want,"
						   " comma separated."));

	gtkhx_box_pack(wid, desc, 0, 0, 0);

	wid = settings_create_group(vbox, _("Paths"));

	table = gtkhx_grid_new_table(3, 2, 0);
	gtk_grid_set_row_spacing(GTK_GRID(table), 10);
	gtk_grid_set_column_spacing(GTK_GRID(table), 5);

	lbl = gtk_label_new(_("Icon Path:"));
	gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_LEFT);
	gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
	gtkhx_grid_attach_table(GTK_GRID(table), lbl, 0, 1, 0, 1, GTK_FILL,
					 0, 0, 0);

	cfgvars[ICONS_IDX].widget = gtk_entry_new();
	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[ICONS_IDX].widget, 1, 2, 0, 1,
					 (GTK_EXPAND|GTK_FILL),
					 0, 0, 0);
	gtk_editable_set_text(GTK_EDITABLE(cfgvars[ICONS_IDX].widget), gtkhx_prefs.icon_str);

	lbl = gtk_label_new(_("Sound Path:"));
	gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_LEFT);
	gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
	gtkhx_grid_attach_table(GTK_GRID(table), lbl, 0, 1, 1, 2, GTK_FILL,
					 0, 0, 0);

	cfgvars[SOUNDPATH_IDX].widget = gtk_entry_new();
	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[SOUNDPATH_IDX].widget, 1, 2, 1, 2,
					 (GTK_EXPAND|GTK_FILL),
					 0, 0, 0);
	gtk_editable_set_text(GTK_EDITABLE(cfgvars[SOUNDPATH_IDX].widget),
						   gtkhx_prefs.sound_path);

	lbl = gtk_label_new(_("Download Path:"));
	gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_LEFT);
	gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
	gtkhx_grid_attach_table(GTK_GRID(table), lbl, 0, 1, 2, 3, GTK_FILL,
					 0, 0, 0);

	cfgvars[DOWNLOAD_IDX].widget = gtk_entry_new();
	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[DOWNLOAD_IDX].widget, 1, 2, 2,
					 3, (GTK_EXPAND|GTK_FILL),
					 0, 0, 0);
	gtk_editable_set_text(GTK_EDITABLE(cfgvars[DOWNLOAD_IDX].widget),
						   gtkhx_prefs.download_path);

	gtkhx_box_pack(wid, table, 0, 0, 0);
}

static int listsorthelper (GtkHList *hlist,
				GtkHListRow *ptr1,
				GtkHListRow *ptr2)
{
	int i1=atoi(GTK_HELL_TEXT(ptr1->cell[1])->text);
	int i2=atoi(GTK_HELL_TEXT(ptr2->cell[1])->text);
	if (i1 < i2) return -1;
	if (i1 > i2) return 1;
	return 0;
}

/* Phase 4.5: GdkEventButton is gone; the gtk_hlist_compat shim emits
 * "select_row" with a NULL GdkEvent so the param is just GdkEvent *
 * now. The handler only reads `row'. */
static void
icon_row_selected (GtkWidget *widget, gint row, gint column,
                   GdkEvent *event, gpointer data)
{
	guint16 icon;
	char buf[16];
	(void) column; (void) event; (void) data;

	if(!GTK_HLIST(widget)->rows) return;
	icon = GPOINTER_TO_INT(gtk_hlist_get_row_data(GTK_HLIST(widget), row));
	if(!icon) {
		return;
	}
	g_snprintf(buf, sizeof(buf), "%u", icon);
	gtk_editable_set_text(GTK_EDITABLE(cfgvars[ICON_IDX].widget), buf);
}

static void settings_page_icon(GtkWidget *vbox)
{
	GtkWidget *wid;
	GtkWidget *table;
	GtkWidget *scroll;
	GtkWidget *icon_list;
	GtkWidget *label;
	char iconstr[16];

	iv = g_malloc(sizeof(struct icon_viewer));

	wid = settings_create_group(vbox, _("Icon"));

	table = gtkhx_grid_new_table(3, 2, 0);
	gtk_grid_set_row_spacing(GTK_GRID(table), 10);
	gtk_grid_set_column_spacing(GTK_GRID(table), 5);

	label = gtk_label_new(_("Icon ID: "));
	gtkhx_grid_attach_table(GTK_GRID(table), label, 0, 1, 0, 1, 0, 0, 0, 0);

	cfgvars[ICON_IDX].widget = gtk_entry_new();
	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[ICON_IDX].widget, 1, 2, 0, 1,
			 (GTK_EXPAND | GTK_FILL),
			 0, 0, 0);
	g_snprintf(iconstr, sizeof(iconstr), "%u", the_session.htlc.icon);
	gtk_editable_set_text(GTK_EDITABLE(cfgvars[ICON_IDX].widget), iconstr);

	scroll = gtk_scrolled_window_new();

	icon_list = gtk_hlist_new(2);
	gtk_hlist_set_selection_mode(GTK_HLIST(icon_list), GTK_SELECTION_SINGLE);
	gtk_hlist_set_column_width(GTK_HLIST(icon_list), 0, 260);
	gtk_hlist_set_column_width(GTK_HLIST(icon_list), 1, 42);
	gtk_hlist_set_row_height(GTK_HLIST(icon_list), 18);
	gtk_hlist_set_compare_func(GTK_HLIST(icon_list),
							   (GtkHListCompareFunc)listsorthelper);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
	gtk_widget_set_size_request(scroll, 232, 256);

	g_signal_connect(icon_list, "select_row",
			   G_CALLBACK(icon_row_selected), iv);

	gtkhx_widget_set_child(scroll, icon_list);
	gtkhx_grid_attach_table(GTK_GRID(table), scroll, 0, 2, 1, 2,
			 (GTK_EXPAND | GTK_FILL),
			 0, 0, 0);

	iv->icon_list = icon_list;
	iv->nfound = 0;
	iv->icon_high = 0;

	gtkhx_box_pack(wid, table, 0, 0, 0);
}

static void settings_page_misc(GtkWidget *vbox)
{
	GtkWidget *wid;
	GtkWidget *table;

	wid = settings_create_group(vbox, _("Miscellaeneous"));

	table = gtkhx_grid_new_table(7, 2, 0);
	gtk_grid_set_row_spacing(GTK_GRID(table), 10);
	gtk_grid_set_column_spacing(GTK_GRID(table), 5);


	cfgvars[AUTOREPLYON_IDX].widget = gtk_check_button_new_with_label(
		_("Auto Reply"));
	gtk_check_button_set_active((GtkCheckButton*)cfgvars[AUTOREPLYON_IDX].widget,
								 gtkhx_prefs.auto_reply);
	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[AUTOREPLYON_IDX].widget, 0, 1, 0, 1,
					 GTK_FILL, 0, 0, 0);

	cfgvars[AUTOREPLYMSG_IDX].widget = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(cfgvars[AUTOREPLYMSG_IDX].widget),
						   gtkhx_prefs.auto_reply_msg);
	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[AUTOREPLYMSG_IDX].widget, 1, 2, 0,
					 1, 0, 0, 0, 0);

	cfgvars[SHOWBACK_IDX].widget = gtk_check_button_new_with_label (
		_("Show Private Messages at Back"));
	gtk_check_button_set_active((GtkCheckButton*)cfgvars[SHOWBACK_IDX].widget,
								 gtkhx_prefs.showback);
	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[SHOWBACK_IDX].widget, 0, 1, 1, 2,
			 (GTK_FILL),
			 (GTK_FILL), 0, 0);

	cfgvars[QUEUEDL_IDX].widget = gtk_check_button_new_with_label(
		_("Queue File Transfers"));
	gtk_check_button_set_active((GtkCheckButton*)
								 cfgvars[QUEUEDL_IDX].widget, gtkhx_prefs.queuedl);
	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[QUEUEDL_IDX].widget, 0, 1, 2, 3,
			 (GTK_FILL), (GTK_FILL), 0, 0);

	cfgvars[SHOWJOIN_IDX].widget = gtk_check_button_new_with_label(
		_("Show Join/Leave in Chat"));
	gtk_check_button_set_active((GtkCheckButton*)
									cfgvars[SHOWJOIN_IDX].widget,
									gtkhx_prefs.showjoin);
	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[SHOWJOIN_IDX].widget, 0, 1, 3, 4,
			 (GTK_FILL),
			 (GTK_FILL), 0, 0);

	cfgvars[TRACKER_CASE_IDX].widget = gtk_check_button_new_with_label(
		_("Case Sensitive Tracker Searching"));
	gtk_check_button_set_active((GtkCheckButton*)
								 cfgvars[TRACKER_CASE_IDX].widget,
								 gtkhx_prefs.track_case);
	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[TRACKER_CASE_IDX].widget, 0, 1, 4, 5,
			 (GTK_FILL),
			 (GTK_FILL), 0, 0);

	cfgvars[OLD_NICKCOMPLETION_IDX].widget = gtk_check_button_new_with_label(
		_("Use old-style nick completion"));
	gtk_check_button_set_active((GtkCheckButton*)
								 cfgvars[OLD_NICKCOMPLETION_IDX].widget,
								 gtkhx_prefs.old_nickcompletion);
	gtkhx_grid_attach_table(GTK_GRID(table),cfgvars[OLD_NICKCOMPLETION_IDX].widget, 0,
					 1, 5, 6, (GTK_FILL),
					 (GTK_FILL), 0, 0);

	gtkhx_box_pack(wid, table, 0, 0, 0);
}

static void settings_page_general(GtkWidget *vbox)
{
	GtkWidget *wid;
	GtkWidget *table;
	GtkWidget *name;

	wid = settings_create_group(vbox, _("General"));

	table = gtkhx_grid_new_table(2, 2, 0);
	gtk_grid_set_row_spacing(GTK_GRID(table), 10);
	gtk_grid_set_column_spacing(GTK_GRID(table), 5);

	cfgvars[NICK_IDX].widget = gtk_entry_new();
	gtkhx_grid_attach_table(GTK_GRID(table), cfgvars[NICK_IDX].widget, 1, 2, 0, 1,
			 (GTK_EXPAND | GTK_FILL),
			 0, 0, 0);
	gtk_editable_set_text(GTK_EDITABLE(cfgvars[NICK_IDX].widget), the_session.htlc.name);

	name = gtk_label_new(_("Your Name:"));
	gtkhx_grid_attach_table(GTK_GRID(table), name, 0, 1, 0, 1,
	                  (GTK_FILL),
	                  (GTK_FILL), 0, 0);
	gtk_label_set_justify(GTK_LABEL(name), GTK_JUSTIFY_LEFT);
	gtk_label_set_xalign(GTK_LABEL(name), 0.0);

	gtkhx_box_pack(wid, table, 0, 0, 0);
}

/* Phase 2.8: GtkCTree → GtkTreeView in tree mode.
 *
 * The settings dialog's left-hand category tree used to be a GtkCTree;
 * it's now a GtkTreeView backed by a GtkTreeStore with two columns:
 *
 *   OPT_COL_LABEL  G_TYPE_STRING   the visible category name
 *   OPT_COL_PAGE   G_TYPE_INT      the GtkNotebook page index to flip to
 *
 * The selection callback reads the page index out of the model and
 * sets the notebook page; helper settings_create_page() now takes
 * GtkTreeIter * arguments instead of GtkCTreeNode **. */
enum {
	OPT_COL_LABEL = 0,
	OPT_COL_PAGE,
	OPT_N_COLS
};

static void
settings_ctree_select (GtkTreeView *tree, gpointer user_data)
{
	GtkWidget *book;
	GtkTreeSelection *sel;
	GtkTreeModel *model;
	GtkTreeIter iter;
	gint page;

	(void)user_data;

	sel = gtk_tree_view_get_selection(tree);
	if (!gtk_tree_selection_get_selected(sel, &model, &iter))
		return;

	book = GTK_WIDGET (g_object_get_data (G_OBJECT (tree), "user_data"));
	gtk_tree_model_get(model, &iter, OPT_COL_PAGE, &page, -1);

	gtk_notebook_set_current_page (GTK_NOTEBOOK (book), page);
}

static GtkWidget *
settings_create_page (GtkWidget *book, gchar *book_label, GtkTreeStore *store,
							 gchar *tree_label, GtkTreeIter *parent,
							 GtkTreeIter *node, gint page_index,
							 void (*draw_func) (GtkWidget *))
{
	GtkWidget *frame;
	GtkWidget *label;
	GtkWidget *vvbox;
	GtkWidget *vbox;

	vvbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	/* border for the label */
	frame = gtk_frame_new (NULL);
	gtkhx_box_pack(vvbox, frame, FALSE, TRUE, 0);

	/* label */
	label = gtk_label_new (book_label);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_widget_set_margin_start  (label, 2);
	gtk_widget_set_margin_end    (label, 2);
	gtk_widget_set_margin_top    (label, 1);
	gtk_widget_set_margin_bottom (label, 1);
	gtkhx_widget_set_child(frame, label);

	/* vbox for the tab */
	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	(gtk_widget_set_margin_start(vbox, 4), gtk_widget_set_margin_end(vbox, 4), gtk_widget_set_margin_top(vbox, 4), gtk_widget_set_margin_bottom(vbox, 4));
	gtkhx_widget_set_child(vvbox, vbox);

	/* row in the category tree */
	gtk_tree_store_append (store, node, parent);
	gtk_tree_store_set (store, node,
						OPT_COL_LABEL, tree_label,
						OPT_COL_PAGE, page_index,
						-1);

	/* call the draw func if there is one */
	if (draw_func)
		draw_func (vbox);


	gtk_notebook_append_page (GTK_NOTEBOOK (book), vvbox, NULL);
	return vbox;
}

void create_options_window(GtkWidget *widget, gpointer data)
{
	GtkTreeIter last_top;
	GtkTreeIter last_child;
	GtkTreeStore *store;
	GtkTreeViewColumn *col;
	GtkCellRenderer *renderer;
	GtkWidget *dialog;
	GtkWidget *hbbox;
	GtkWidget *frame;
	GtkWidget *ctree;
	GtkWidget *book;
	GtkWidget *hbox;
	GtkWidget *vbox;
	GtkWidget *wid;
	gint page_index;
	session *sess = data;

	if(options_window) {
		gtk_window_present(GTK_WINDOW(options_window));
		return;
	}

	dialog = gtk_dialog_new ();
	gtk_window_set_title (GTK_WINDOW (dialog), _("GtkHx Preferences"));
	/* Phase 4.2: gtk_window_set_position removed in GTK 4 */
	gtk_widget_set_size_request(dialog, 570, 400);
	/* Phase 5: parent the prefs dialog on the active toplevel so GTK
	 * doesn't warn about a top-level dialog mapped without
	 * transient_for. The Options window is invoked from the toolbar
	 * and from menu items in any window; whichever one has focus is
	 * the natural parent. */
	{
		GtkWindow *parent = gtkhx_active_window ();
		if (parent)
			gtk_window_set_transient_for (GTK_WINDOW (dialog), parent);
	}
	g_object_set_data(G_OBJECT(dialog), "sess", sess);
	/* Phase 5: hook destroy (not close-request) so the bookkeeping
	 * fires on every teardown path — Cancel/OK buttons call
	 * gtk_window_destroy which does NOT emit close-request in GTK 4. */
	g_signal_connect (dialog, "destroy",
	                  G_CALLBACK (close_options_bookkeeping), NULL);

	options_window = dialog;

	{
		GtkWidget *aa = gtkhx_dialog_action_area(GTK_DIALOG(dialog));
		/* Phase 4.x: gtk_container_set_border_width is gone. Use the
		 * widget margin properties instead. */
		gtk_widget_set_margin_start (aa, 2);
		gtk_widget_set_margin_end (aa, 2);
		gtk_widget_set_margin_top (aa, 2);
		gtk_widget_set_margin_bottom (aa, 2);
		gtk_box_set_homogeneous (GTK_BOX (aa), FALSE);
	}

	/* Phase 4.x: GtkButtonBox is gone. A horizontal GtkBox with a small
	 * spacing is the documented replacement. */
	hbbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
	gtkhx_box_pack_end(gtkhx_dialog_action_area(GTK_DIALOG(dialog)), hbbox, FALSE, FALSE, 0);

	wid = gtk_button_new_with_label (_("OK"));
	g_signal_connect (wid, "clicked",
						G_CALLBACK (options_change), GINT_TO_POINTER(0));
	gtkhx_box_pack(hbbox, wid, 0, 0, 0);

	wid = gtk_button_new_with_label (_("Apply"));
	g_signal_connect (wid, "clicked",
						G_CALLBACK (options_change), GINT_TO_POINTER(1));
	gtkhx_box_pack(hbbox, wid, 0, 0, 0);

	wid = gtk_button_new_with_label (_("Cancel"));
	g_signal_connect (wid, "clicked",
							  G_CALLBACK (close_options_window_cancel), 0);
	gtkhx_box_pack(hbbox, wid, 0, 0, 0);

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	(gtk_widget_set_margin_start(hbox, 6), gtk_widget_set_margin_end(hbox, 6), gtk_widget_set_margin_top(hbox, 6), gtk_widget_set_margin_bottom(hbox, 6));
	gtkhx_box_pack(gtk_dialog_get_content_area(GTK_DIALOG (dialog)), hbox, TRUE, TRUE, 0);

	store = gtk_tree_store_new(OPT_N_COLS, G_TYPE_STRING, G_TYPE_INT);
	ctree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
	g_object_unref(store);  /* the view owns the only ref now */
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(ctree), TRUE);
	renderer = gtk_cell_renderer_text_new();
	col = gtk_tree_view_column_new_with_attributes(_("Categories"), renderer,
												   "text", OPT_COL_LABEL,
												   NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(ctree), col);
	gtk_tree_selection_set_mode(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(ctree)),
		GTK_SELECTION_BROWSE);
	gtk_widget_set_size_request (ctree, 140, 0);
	gtkhx_box_pack(hbox, ctree, 0, 0, 0);

	frame = gtk_frame_new (NULL);
	gtkhx_box_pack(hbox, frame, TRUE, TRUE, 0);

	book = gtk_notebook_new ();
	gtk_notebook_set_show_tabs (GTK_NOTEBOOK (book), FALSE);
	gtk_notebook_set_show_border (GTK_NOTEBOOK (book), FALSE);
	gtkhx_widget_set_child(frame, book);
	g_object_set_data (G_OBJECT (ctree), "user_data", book);
	g_signal_connect (ctree, "cursor-changed",
						G_CALLBACK (settings_ctree_select), NULL);
	page_index = 0;

	vbox = settings_create_page(book, _("General Settings"), store,
								_("General Settings"), NULL, &last_top,
								page_index++, settings_page_general);

	vbox = settings_create_page(book, _("Tracker Settings"), store,
								_("Tracker Settings"), &last_top, &last_child,
								page_index++, settings_page_tracker);

	vbox = settings_create_page(book, _("Icon Settings"), store,
								_("Icon Settings"), &last_top, &last_child,
								page_index++, settings_page_icon);

	vbox = settings_create_page(book, _("Path Settings"), store,
								_("Paths"), &last_top, &last_child,
								page_index++, settings_page_path);

	vbox = settings_create_page(book, _("Files Settings"), store,
								_("Files Settings"), &last_top, &last_child,
								page_index++, settings_page_files);

	vbox = settings_create_page(book, _("Threaded News Settings"), store,
								_("Threaded News"), &last_top, &last_child,
								page_index++, settings_page_news15);

#if 0 /* XXX */
	vbox = settings_create_page(book, _("Logging Settings"), store,
								_("Logging Settings"), &last_top, &last_child,
								page_index++, settings_page_logging);
#endif

	vbox = settings_create_page(book, _("Miscellaeneous"), store,
								_("Miscellaeneous"), &last_top, &last_child,
								page_index++, settings_page_misc);

	vbox = settings_create_page(book, _("XText Settings"), store,
								_("XText Settings"), NULL, &last_top,
								page_index++, settings_page_xtext);

	vbox = settings_create_page(book, _("Font Settings"), store,
								_("Fonts Settings"), &last_top, &last_child,
								page_index++, settings_page_font);

	vbox = settings_create_page(book, _("Sound Settings"), store,
								_("Sound Settings"), NULL, &last_top,
								page_index++, settings_page_sound);

	gtk_tree_view_expand_all (GTK_TREE_VIEW (ctree));
	{
		GtkTreeIter first;
		if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &first))
			gtk_tree_selection_select_iter(
				gtk_tree_view_get_selection(GTK_TREE_VIEW(ctree)),
				&first);
	}

	gtk_window_present(GTK_WINDOW(dialog));

	list_icons();
}

G_GNUC_END_IGNORE_DEPRECATIONS
/* Phase 4.13: end of file-level deprecation suppression — see top of file. */
