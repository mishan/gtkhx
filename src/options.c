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
	0,
#ifdef USE_GDK_PIXBUF
	255,
	255,
	255,
#endif
#ifdef WIN32
	"sounds",
#else
	PREFIX"/share/gtkhx/sounds",
#endif
	"",
	"fixed",
	".",
	NULL,
	"hltracker.com",
	NULL,
#ifdef WIN32
	"icons.rsrc",
#else
	PREFIX "/share/gtkhx/icons.rsrc",
#endif
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
	GdkPixmap *pixmap;
	GdkBitmap *mask;
	gint row;
	gchar *text[2] = {0, 0};
	guint32 icon;
	unsigned int nfound = 0;
	char buf[16];
	GtkWidget *icon_list = iv->icon_list;
	guint16 nres = 0;
	int i;
	guint32 id;
	GdkColormap *colormap = gtk_widget_get_colormap(icon_list);
	GdkVisual *visual = gtk_widget_get_visual(icon_list);
	GdkGC *gc = NULL;
	GdkImage *image;
	GdkImage *maskim;
	int off;
	GdkColor col = {0, 0, 0};

	text[1] = buf;
	gtk_hlist_freeze(GTK_HLIST(icon_list));
	for (i = 0; i < icon_files.n; ++i) {
		if (!icon_files.cicns[i]) continue;
		nres = macres_file_num_res_of_type(icon_files.cicns[i], TYPE_cicn);
		for(icon = 0; icon < nres; icon++) {
			macres_res *r = 0;
			r = macres_file_get_nth_res_of_type(icon_files.cicns[i], TYPE_cicn, icon);
			if(!r) continue;

			image = (GdkImage *)cicn_to_gdkimage(colormap, visual, r->data, r->datalen, &maskim);

			off = image->width > 400 ? 198 : 0;
			pixmap = gdk_pixmap_new(icon_list->window, image->width-off, image->height, image->depth);
			if (!gc) gc = gdk_gc_new (pixmap);
			gdk_draw_image(pixmap, gc, image, off, 0, 0, 0, image->width-off, image->height);
			if (maskim) {
				mask = gdk_pixmap_new(icon_list->window, image->width-off, image->height, 1);
				if (!mask_gc) mask_gc = gdk_gc_new (mask);
				gdk_draw_image(mask, mask_gc, maskim, off, 0, 0, 0, image->width-off, image->height);
				gdk_image_destroy(maskim);
			}
			else
				mask = 0;

			gdk_image_destroy(image);
			if(!pixmap) continue;

			nfound++;
			sprintf(buf, "%u", r->resid);
			row = gtk_hlist_append(GTK_HLIST(icon_list), text);
			id = r->resid;
			gtk_hlist_set_row_data(GTK_HLIST(icon_list), row, (gpointer)id);
			gtk_hlist_set_foreground(GTK_HLIST(icon_list), row, &col);
			gtk_hlist_set_pixtext(GTK_HLIST(icon_list), row, 0, "", 34, pixmap, mask);
			gdk_pixmap_unref(pixmap);
			if(mask) gdk_pixmap_unref(mask);

			/*	g_free (r->name);  */
			g_free (r);

			/* Cooperative multitasking */
			if (icon % 10 == 0) {
				while (gtk_events_pending()) gtk_main_iteration();

				if (!options_window)/*Have we been destroyed?!*/
					return;
			}
		}
	}
	if(nfound >= 2) {
		gtk_hlist_sort(GTK_HLIST(icon_list));
	}
	gtk_hlist_thaw(GTK_HLIST(icon_list));
}


static void new_style (void)
{
	gtktext_style = gtk_style_new();

	gtktext_style->fg[GTK_STATE_NORMAL] = fg_col;
	gtktext_style->text[GTK_STATE_NORMAL] = fg_col;
	gtktext_style->light[GTK_STATE_NORMAL] = fg_col;

	gtktext_style->bg[GTK_STATE_NORMAL] = bg_col;
	gtktext_style->base[GTK_STATE_NORMAL] = bg_col;
	gtktext_style->dark[GTK_STATE_NORMAL] = bg_col;

	gtktext_style->font = gtkhx_font;
}

void reinit_gtktexts (session *sess)
{
	struct msgwin *msg;
	struct gtkhx_chat *gchat;

	if(gtktext_style) {
		gtk_style_unref(gtktext_style);
	}

	new_style();

	if(gtkhx_prefs.geo.news.open) {
		gtk_widget_set_style(sess->news_text, gtktext_style);
	}
	for(gchat = sess->gchat_list; gchat; gchat = gchat->prev) {
		if(gchat->cid == 0 && !gtkhx_prefs.geo.chat.open)
			continue;
		gtk_xtext_set_font(GTK_XTEXT(gchat->output), gtkhx_font, 0);
		gtk_xtext_refresh(GTK_XTEXT(gchat->output), 1);
		if(gchat->input) {
			gtk_widget_set_style(gchat->input, gtktext_style);
		}
		if(gchat->subject) {
			gtk_widget_set_style(gchat->subject, gtktext_style);
		}
	}
	for(msg = sess->msg_list; msg; msg = msg->prev) {
		gtk_xtext_set_font(GTK_XTEXT(msg->outputbuf), gtkhx_font, 0);
		gtk_xtext_refresh(GTK_XTEXT(gchat->output), 1);
		gtk_widget_set_style(msg->inputbuf, gtktext_style);
	}
}


static void changed_xtext (session *sess)
{
	if (sess)
	{
		struct gtkhx_chat *gchat;
		struct msgwin *msg;
		for(gchat = sess->gchat_list; gchat; gchat = gchat->prev) {
			gtk_xtext_set_background(GTK_XTEXT(gchat->output), 0,
				gtkhx_prefs.trans_xtext, 1);
				GTK_XTEXT(gchat->output)->wordwrap = gtkhx_prefs.word_wrap;
				GTK_XTEXT(gchat->output)->max_lines = gtkhx_prefs.xbuf_max;
			gtk_xtext_refresh(GTK_XTEXT(gchat->output), 1);
		}
		for(msg = sess->msg_list; msg; msg = msg->prev) {
			gtk_xtext_set_background(GTK_XTEXT(msg->outputbuf), 0,
				gtkhx_prefs.trans_xtext, 1);
				GTK_XTEXT(msg->outputbuf)->wordwrap = gtkhx_prefs.word_wrap;
				GTK_XTEXT(msg->outputbuf)->max_lines = gtkhx_prefs.xbuf_max;
			gtk_xtext_refresh(GTK_XTEXT(msg->outputbuf), 1);
		}
	}
}

/*
  static void changed_nickoricon (session *sess)
  {
  hx_change_name_icon(&sessions[0].htlc);
  }
*/

static void changed_font (session *sess)
{
	if(gtkhx_font) {
		gdk_font_unref(gtkhx_font);
	}

	if(gtkhx_prefs.use_fontset) {
		gtkhx_font = gdk_fontset_load(gtkhx_prefs.font);
	}
	else {
		gtkhx_font = gdk_font_load(gtkhx_prefs.font);
	}

	if(!gtkhx_font) {
		g_warning("Bad font \"%s\"\n", gtkhx_prefs.font);
		gtkhx_font = gdk_font_load("fixed");
		if(gtkhx_prefs.font)
			g_free(gtkhx_prefs.font);
		gtkhx_prefs.font = g_strdup("fixed");
	}

	if (sess) {
		reinit_gtktexts(sess);
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
#ifdef USE_GDK_PIXBUF
	{"BLUE", {&gtkhx_prefs.tint_blue}, INT, 0, NULL, NULL},
#endif
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
	{"FONTSET", {&gtkhx_prefs.use_fontset}, BOOLEAN, 0, NULL, NULL},
#ifdef USE_GDK_PIXBUF
	{"GREEN", {&gtkhx_prefs.tint_green}, INT, 0, NULL, NULL},
#endif
	{"ICON", {&sessions[0].htlc.icon}, UINT16, 0, /*changed_nickoricon*/NULL, NULL},
	{"ICONS", {&gtkhx_prefs.icon_str}, STRING, 0, parse_icons, NULL},
#if 0 /* XXX */
	{"LOGGING", {&gtkhx_prefs.logging}, BOOLEAN, 0, changed_logging, NULL},
#endif 
	{"NEWSXPOS", {&gtkhx_prefs.geo.news.xpos}, INT, 0, NULL, NULL},
	{"NEWSXSIZE", {&gtkhx_prefs.geo.news.xsize}, INT, 0, NULL, NULL},
	{"NEWSYPOS", {&gtkhx_prefs.geo.news.ypos}, INT, 0, NULL, NULL},
	{"NEWSYSIZE", {&gtkhx_prefs.geo.news.ysize}, INT, 0, NULL, NULL},
	{"NICK", {sessions[0].htlc.name}, STRING32, 0, /*changed_nickoricon*/NULL, NULL},
	{"OLD_NICKCOMPLETION", {&gtkhx_prefs.old_nickcompletion}, BOOLEAN, 0, NULL,
	 NULL},
	{"OPENCHAT", {&gtkhx_prefs.geo.chat.init}, BOOLEAN, 0, NULL, NULL},
	{"OPENNEWS", {&gtkhx_prefs.geo.news.init}, BOOLEAN, 0, NULL, NULL},
	{"OPENTASKS", {&gtkhx_prefs.geo.tasks.init}, BOOLEAN, 0, NULL, NULL},
	{"OPENUSERS", {&gtkhx_prefs.geo.users.init}, BOOLEAN, 0, NULL, NULL},
	{"QUEUEDL", {&gtkhx_prefs.queuedl}, BOOLEAN, 0, NULL, NULL},
#ifdef USE_GDK_PIXBUF
	{"RED", {&gtkhx_prefs.tint_red}, INT, 0, NULL, NULL},
#endif
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
	{"TRANSPARENT", {&gtkhx_prefs.trans_xtext}, BOOLEAN, 0, changed_xtext,NULL},
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
	BLUE_IDX,
	CHATXPOS_IDX,
	CHATXSIZE_IDX,
	CHATYPOS_IDX,
	CHATYSIZE_IDX,
	DOWNLOAD_IDX,
	FILE_SAMEWINDOW_IDX,
	NEWS_SAMEWINDOW_IDX,
	FONT_IDX,
	FONTSET_IDX,
	GREEN_IDX,
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
	RED_IDX,
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
	TRANSPARENT_IDX,
	USERXPOS_IDX,
	USERXSIZE_IDX,
	USERYPOS_IDX,
	USERYSIZE_IDX,
	WORDWRAP_IDX,
	XBUF_MAX_IDX
};

void init_variables(void) /* default settings if prefs file is not found. */
{
	gtkhx_font = gdk_font_load("-b&h-lucidatypewriter-medium-r-normal-*-*-100-*-*-m-*-iso8859-1");
	if(!gtkhx_font) {
		gtkhx_font = gdk_font_load("fixed");          /* fall back to the 
														 fixed font if ! 
														 default font */
		gtkhx_prefs.font = g_strdup("fixed");
	}
	else {
		gtkhx_prefs.font = g_strdup("-b&h-lucidatypewriter-medium-r-normal-*-*-100-*-*-m-*-iso8859-1");
	}
	cfgvars[FONT_IDX].allocated = 1;


	fg_col.red = 0xcccc;
	fg_col.green = 0xcccc;
	fg_col.blue = 0xcccc;
	bg_col.red = 0x0000;
	bg_col.green = 0x0000;
	bg_col.blue = 0x0000;

	new_style();

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
		(*(result->changefunc))(&sessions[0]);
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
#ifndef WIN32
	char *path = g_strdup_printf("%s/.gtkhxrc", getenv("HOME"));
	FILE *prefs = fopen(path, "r");
#else
	FILE *prefs = fopen("gtkhxrc", "r");
#endif
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
#ifndef WIN32
					path,
#else
					"gtkhxrc",
#endif
					strerror(errno));
			fflush(stderr);
		}

#ifndef WIN32
		g_free(path);
#endif
		return;
	}

	while(read_line(prefs, &prefsline, &prefslinelen))
		parse_line(prefsline);

	g_free(prefsline);
	fclose(prefs);
#ifndef WIN32
	g_free(path);
#endif
}

void prefs_write(void)
{
#ifndef WIN32
	char *path = g_strdup_printf("%s/.gtkhxrc", getenv("HOME"));
 	FILE *prefs = fopen(path, "w");
#else
 	FILE *prefs = fopen("gtkhxrc", "w");
#endif
	time_t now;
	int i;

	if(!prefs) {
		fprintf(stderr, "prefs_write: %s: %s\n", 
#ifndef WIN32
				  path,
#else
				  "gtkhxrc",
#endif
				  strerror(errno));
		fflush(stderr);

#ifndef WIN32
	g_free(path);
#endif
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
#ifndef WIN32
	g_free(path);
#endif
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

	gtkhx_prefs.num_tracker = GTK_CLIST(list)->rows;
	gtkhx_prefs.tracker = g_malloc(GTK_CLIST(list)->rows * sizeof(char*));
	if (cfgvars[TRACKER_IDX].allocated) g_free (gtkhx_prefs.tracker_str);
	gtkhx_prefs.tracker_str = g_malloc0(1);

	for(i = 0; i < GTK_CLIST(list)->rows; i++) {
		char *tracker = gtk_clist_get_row_data(GTK_CLIST(list), i);
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

static void close_options_window (void)
{
	gtk_widget_destroy(options_window);
	options_window = 0;
	g_free(iv);
}


void options_change (GtkWidget *widget, gpointer data)
{
	int i;

	session *sess = gtk_object_get_data(GTK_OBJECT(options_window), "sess");

	for (i=0; i != sizeof(cfgvars)/sizeof(cfgvars[0]); ++i)
	{
		struct cfgvar *v = &cfgvars[i];
		if (!v->widget) continue;
		switch (v->type) {
		case UINT16:
		{
			char *str = gtk_entry_get_text(GTK_ENTRY(v->widget));
			guint16 g;;
			
			if(!str) continue;
			g = atou16(str);
			if (g == *v->variable.uint16) continue;
			*v->variable.uint16 = g;
			break;
			}
			case INT:
			{
				char *st = gtk_entry_get_text (GTK_ENTRY(v->widget));

				int in;
				if(!st) continue;
				in = atoi(st);
				if (in == *v->variable.integer) continue;
				*v->variable.integer = in;
				break;
			}
			case STRING32:
			{
				char *s = gtk_entry_get_text(GTK_ENTRY(v->widget));
				if(!s) continue;
				if (!strcmp(s, v->variable.str32)) continue;
				strncpy(v->variable.str32, s, 31);
				v->variable.str32[31] = '\0';
				break;
			}
			case STRING:
			{
				char *string = gtk_entry_get_text(GTK_ENTRY(v->widget));
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
				unsigned char b = gtk_toggle_button_get_active(
					(GtkToggleButton*)v->widget);
				if (b == *v->variable.uchar) continue;
				*v->variable.uchar = b;
				break;
			}
		}
		if (v->changefunc)
			(*(v->changefunc))(sess);
	}

	if(connected) {
		hx_change_name_icon(&sessions[0].htlc);
	}

	parse_tracker_list();

	prefs_write();

	if(!GPOINTER_TO_INT(data)) {
		close_options_window();
	}
}

static void set_font (GtkWidget *btn, GtkWidget *fontsel)
{
	GtkWidget *entry = (GtkWidget *)gtk_object_get_data(GTK_OBJECT(btn),
														"entry");

	gtk_entry_set_text(GTK_ENTRY(entry),
					   gtk_font_selection_dialog_get_font_name(
						   GTK_FONT_SELECTION_DIALOG(fontsel)));

	gtk_widget_destroy(fontsel);
}

static void create_fontsel (GtkWidget *btn, GtkWidget *entry)
{
	GtkWidget *fontsel = gtk_font_selection_dialog_new(_("Browse Fonts"));

	gtk_signal_connect_object(GTK_OBJECT(
								  GTK_FONT_SELECTION_DIALOG(
									  fontsel)->cancel_button),
							  "clicked", (GtkSignalFunc) gtk_widget_destroy,
							  GTK_OBJECT(fontsel));

	gtk_object_set_data(GTK_OBJECT(
							GTK_FONT_SELECTION_DIALOG(fontsel)->ok_button),
						"entry", entry);
	gtk_signal_connect(GTK_OBJECT(
						   GTK_FONT_SELECTION_DIALOG(fontsel)->ok_button),
					   "clicked", GTK_SIGNAL_FUNC(set_font), fontsel);
	gtk_font_selection_dialog_set_font_name(GTK_FONT_SELECTION_DIALOG(fontsel),
											gtkhx_prefs.font);

	gtk_widget_show_all(fontsel);
}

#ifdef USE_GDK_PIXBUF
static void
settings_slider_cb (GtkAdjustment * adj, int *value)
{
	struct gtkhx_chat *gchat;
	struct msgwin *msg;
	int i;

	*value = adj->value;
	for(i = 0; i < sizeof(sessions)/sizeof(sessions[0]); i++) {
		for(gchat = sessions[i].gchat_list; gchat; gchat = gchat->prev) {
			GTK_XTEXT (gchat->output)->tint_red = gtkhx_prefs.tint_red;
			GTK_XTEXT (gchat->output)->tint_green = gtkhx_prefs.tint_green;
			GTK_XTEXT (gchat->output)->tint_blue = gtkhx_prefs.tint_blue;
			gtk_xtext_refresh (GTK_XTEXT (gchat->output), gtkhx_prefs.trans_xtext);
		}
	}
	for(msg = msg_list; msg; msg = msg->prev) {
		GTK_XTEXT (msg->outputbuf)->tint_red = gtkhx_prefs.tint_red;
		GTK_XTEXT (msg->outputbuf)->tint_green = gtkhx_prefs.tint_green;
		GTK_XTEXT (msg->outputbuf)->tint_blue = gtkhx_prefs.tint_blue;
		gtk_xtext_refresh (GTK_XTEXT (msg->outputbuf),
						   gtkhx_prefs.trans_xtext);
	}
}
static void
settings_slider (GtkWidget * vbox, char *label, int *value)
{
	GtkAdjustment *adj;
	GtkWidget *wid, *hbox, *lbox, *lab;

	hbox = gtk_hbox_new (0, 0);
	gtk_box_pack_start (GTK_BOX (vbox), hbox, 0, 0, 2);

	lbox = gtk_hbox_new (0, 0);
	gtk_box_pack_start (GTK_BOX (hbox), lbox, 0, 0, 2);
	gtk_widget_set_usize (lbox, 60, 0);

	lab = gtk_label_new (label);
	gtk_box_pack_end (GTK_BOX (lbox), lab, 0, 0, 0);

	adj = (GtkAdjustment *) gtk_adjustment_new (*value, 0, 255.0, 1, 25, 0);
	gtk_signal_connect (GTK_OBJECT (adj), "value_changed",
							  GTK_SIGNAL_FUNC (settings_slider_cb), value);

	wid = gtk_hscale_new (adj);
	gtk_scale_set_value_pos ((GtkScale *) wid, GTK_POS_RIGHT);
	gtk_scale_set_digits ((GtkScale *) wid, 0);
	gtk_container_add (GTK_CONTAINER (hbox), wid);
}

#endif

static GtkWidget *
settings_create_group (GtkWidget * vvbox, gchar * title)
{
	GtkWidget *frame;
	GtkWidget *vbox;

	frame = gtk_frame_new (title);
	gtk_box_pack_start (GTK_BOX (vvbox), frame, FALSE, FALSE, 0);

	vbox = gtk_vbox_new (FALSE, 2);
	gtk_container_set_border_width (GTK_CONTAINER (vbox), 2);
	gtk_container_add (GTK_CONTAINER (frame), vbox);

	return vbox;
}

static void add_tracker(GtkWidget *add, GtkWidget *entry)
{
	char *tracker = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
	gint row;

	row = gtk_clist_append(GTK_CLIST(tracker_list), &tracker);
	gtk_clist_set_row_data(GTK_CLIST(tracker_list), row,
						   tracker);
	gtk_entry_set_text(GTK_ENTRY(entry), "");
}

static void remove_tracker(GtkWidget *del, GtkWidget *list)
{
	gtk_clist_remove(GTK_CLIST(list), GTK_CLIST(list)->focus_row);
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

	scroll = gtk_scrolled_window_new(0, 0);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);

	tracker_list = gtk_clist_new(1);
	gtk_container_add(GTK_CONTAINER(scroll), tracker_list);

	gtk_box_pack_start(GTK_BOX(wid), scroll, 0, 0, 0);
	gtk_widget_set_usize(scroll, 232, 246);

	ent_hbox = gtk_hbox_new(0, 0);
	gtk_box_pack_start(GTK_BOX(wid), ent_hbox, 0, 0, 0);

	lbl = gtk_label_new(_("Address: "));
	entry = gtk_entry_new();

	gtk_box_pack_start(GTK_BOX(ent_hbox), lbl, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(ent_hbox), entry, 0, 0, 0);

	btnhbox = gtk_hbox_new(0, 0);
	gtk_box_pack_start(GTK_BOX(wid), btnhbox, 0, 0, 0);

	add = gtk_button_new_with_label(_("Add"));
	gtk_signal_connect(GTK_OBJECT(add), "clicked",
					   GTK_SIGNAL_FUNC(add_tracker), entry);

	remove = gtk_button_new_with_label(_("Remove"));
	gtk_signal_connect(GTK_OBJECT(remove), "clicked",
					   GTK_SIGNAL_FUNC(remove_tracker),
					   tracker_list);

	gtk_box_pack_start(GTK_BOX(btnhbox), add, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(btnhbox), remove, 0, 0, 0);

	for(i = 0; i < gtkhx_prefs.num_tracker; i++) {
		char *tracker = g_strdup(gtkhx_prefs.tracker[i]);

		row = gtk_clist_append(GTK_CLIST(tracker_list),
							   &tracker);
		gtk_clist_set_row_data(GTK_CLIST(tracker_list), row,
							   tracker);
	}
}

static void settings_page_news15 (GtkWidget *vbox)
{
	GtkWidget *wid;
	GtkWidget *table;

	wid = settings_create_group(vbox, _("News Folder Browsing"));
	table = gtk_table_new(1, 1, 0);

	cfgvars[NEWS_SAMEWINDOW_IDX].widget = gtk_check_button_new_with_label(
		_("Browse in Same Window"));
	gtk_toggle_button_set_active((GtkToggleButton*)cfgvars[NEWS_SAMEWINDOW_IDX].widget,
								 gtkhx_prefs.news_samewin);

	gtk_table_attach(GTK_TABLE(table), cfgvars[NEWS_SAMEWINDOW_IDX].widget, 0,
										1, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);


	gtk_box_pack_start(GTK_BOX(wid), table, 0, 0, 0);
}


static void settings_page_files(GtkWidget *vbox)
{
	GtkWidget *wid;
	GtkWidget *table;

	wid = settings_create_group(vbox, _("File Browsing"));
	table = gtk_table_new(1, 1, 0);

	cfgvars[FILE_SAMEWINDOW_IDX].widget = gtk_check_button_new_with_label(
		_("Browse in Same Window"));
	gtk_toggle_button_set_active((GtkToggleButton*)cfgvars[FILE_SAMEWINDOW_IDX].widget,
								 gtkhx_prefs.file_samewin);

	gtk_table_attach(GTK_TABLE(table), cfgvars[FILE_SAMEWINDOW_IDX].widget, 0,
										1, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);


	gtk_box_pack_start(GTK_BOX(wid), table, 0, 0, 0);
}

#if 0 /* XXX */
static void settings_page_logging (GtkWidget *vbox)
{
	GtkWidget *wid;
	GtkWidget *table;

	wid = settings_create_group(vbox, _("Logging"));
	table = gtk_table_new(1, 1, 0);

	cfgvars[LOGGING_IDX].widget = gtk_check_button_new_with_label(
		_("Log Chats/Private Messages"));
	gtk_toggle_button_set_active((GtkToggleButton*)cfgvars[LOGGING_IDX].widget,
								 gtkhx_prefs.logging);

	gtk_table_attach(GTK_TABLE(table), cfgvars[LOGGING_IDX].widget, 0,
										1, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);


	gtk_box_pack_start(GTK_BOX(wid), table, 0, 0, 0);
}
#endif

static void settings_page_sound(GtkWidget *vbox)
{
	GtkWidget *wid;
	GtkWidget *table;
	GtkWidget *table2;
	GtkWidget *lbl;

	wid = settings_create_group(vbox, _("Sound Command"));
	table2 = gtk_table_new(1, 2, 0);

	table = gtk_table_new(10, 2, 0);
	gtk_table_set_col_spacings(GTK_TABLE(table2), 5);

	lbl = gtk_label_new(_("Sound Command: "));
	gtk_table_attach(GTK_TABLE(table2), lbl, 0, 1, 2, 3,
					 GTK_FILL, GTK_FILL, 0, 0);

	cfgvars[SND_CMD_IDX].widget = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(cfgvars[SND_CMD_IDX].widget),
					   gtkhx_prefs.snd_cmd);
	gtk_table_attach(GTK_TABLE(table2), cfgvars[SND_CMD_IDX].widget, 1, 2, 2, 3,
					 (GTK_EXPAND|GTK_FILL), 0, 0, 0);

	gtk_box_pack_start(GTK_BOX(wid), table2, 0, 0, 0);

	wid = settings_create_group(vbox, _("Sounds"));

	cfgvars[SOUNDSON_IDX].widget = gtk_check_button_new_with_label(_("Play Sounds For:"));
	gtk_toggle_button_set_active((GtkToggleButton*)cfgvars[SOUNDSON_IDX].widget, hxsnd.on);
	cfgvars[SOUNDINVITE_IDX].widget = gtk_check_button_new_with_label(_("Chat Invitation"));
	gtk_toggle_button_set_active((GtkToggleButton*)cfgvars[SOUNDINVITE_IDX].widget,
								 hxsnd.invite);
	cfgvars[SOUNDCHAT_IDX].widget = gtk_check_button_new_with_label(_("Chat"));
	gtk_toggle_button_set_active((GtkToggleButton *)cfgvars[SOUNDCHAT_IDX].widget,
								 hxsnd.chat);
	cfgvars[SOUNDERROR_IDX].widget = gtk_check_button_new_with_label(_("Error"));
	gtk_toggle_button_set_active((GtkToggleButton *)cfgvars[SOUNDERROR_IDX].widget,
								 hxsnd.error);
	cfgvars[SOUNDFILE_IDX].widget = gtk_check_button_new_with_label(
		_("File Transfer Complete"));
	gtk_toggle_button_set_active((GtkToggleButton *)cfgvars[SOUNDFILE_IDX].widget,
								 hxsnd.file);
	cfgvars[SOUNDJOIN_IDX].widget = gtk_check_button_new_with_label(_("Join"));
	gtk_toggle_button_set_active((GtkToggleButton *)cfgvars[SOUNDJOIN_IDX].widget,
								 hxsnd.join);
	cfgvars[SOUNDLOGIN_IDX].widget = gtk_check_button_new_with_label(_("Login"));
	gtk_toggle_button_set_active((GtkToggleButton *)cfgvars[SOUNDLOGIN_IDX].widget,
								 hxsnd.login);
	cfgvars[SOUNDMSG_IDX].widget = gtk_check_button_new_with_label(_("Private Message"));
	gtk_toggle_button_set_active((GtkToggleButton *)cfgvars[SOUNDMSG_IDX].widget, hxsnd.msg);
	cfgvars[SOUNDNEWS_IDX].widget = gtk_check_button_new_with_label(_("News"));
	gtk_toggle_button_set_active((GtkToggleButton *)cfgvars[SOUNDNEWS_IDX].widget,
								 hxsnd.news);
	cfgvars[SOUNDPART_IDX].widget = gtk_check_button_new_with_label(_("Leave"));
	gtk_toggle_button_set_active((GtkToggleButton *)cfgvars[SOUNDPART_IDX].widget,
								 hxsnd.part);

	gtk_table_attach(GTK_TABLE(table), cfgvars[SOUNDSON_IDX].widget, 0, 1, 0, 1,
					 GTK_EXPAND|GTK_FILL, 0, 0, 4);
	gtk_table_attach(GTK_TABLE(table), cfgvars[SOUNDINVITE_IDX].widget, 1, 2, 1, 2,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtk_table_attach(GTK_TABLE(table), cfgvars[SOUNDCHAT_IDX].widget, 1, 2, 2, 3,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtk_table_attach(GTK_TABLE(table), cfgvars[SOUNDERROR_IDX].widget, 1, 2, 3, 4,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtk_table_attach(GTK_TABLE(table), cfgvars[SOUNDFILE_IDX].widget, 1, 2, 4, 5,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtk_table_attach(GTK_TABLE(table), cfgvars[SOUNDJOIN_IDX].widget, 1, 2, 5, 6,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtk_table_attach(GTK_TABLE(table), cfgvars[SOUNDLOGIN_IDX].widget, 1, 2, 6, 7,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtk_table_attach(GTK_TABLE(table), cfgvars[SOUNDMSG_IDX].widget, 1, 2, 7, 8,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtk_table_attach(GTK_TABLE(table), cfgvars[SOUNDNEWS_IDX].widget, 1, 2, 8, 9,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtk_table_attach(GTK_TABLE(table), cfgvars[SOUNDPART_IDX].widget, 1, 2, 9, 10,
					 GTK_EXPAND|GTK_FILL, 0, 0, 0);

	gtk_box_pack_start(GTK_BOX(wid), table, 0, 0, 0);
}

static void settings_page_font(GtkWidget *vbox)
{
	GtkWidget *wid;
	GtkWidget *table;
	GtkWidget *table2;
	GtkWidget *lbl;
	GtkWidget *btn;

	wid = settings_create_group(vbox, _("Fonts"));

	table = gtk_table_new(1, 2, 0);

	lbl = gtk_label_new(_("Font: "));
	gtk_table_attach(GTK_TABLE(table), lbl, 0, 1, 2, 3,
					 GTK_FILL, GTK_FILL, 0, 0);

	cfgvars[FONT_IDX].widget = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(cfgvars[FONT_IDX].widget), gtkhx_prefs.font);
	gtk_table_attach(GTK_TABLE(table), cfgvars[FONT_IDX].widget, 1, 2, 2, 3,
					 (GTK_EXPAND|GTK_FILL), 0, 0, 0);

	gtk_box_pack_start(GTK_BOX(wid), table, 0, 0, 0);

	table2 = gtk_table_new(2, 1, 0);

	cfgvars[FONTSET_IDX].widget = gtk_check_button_new_with_label(
		_("Use gdk_fontset_load"));
	gtk_toggle_button_set_active((GtkToggleButton *)
								 cfgvars[FONTSET_IDX].widget,
								 gtkhx_prefs.use_fontset);
	gtk_table_attach(GTK_TABLE(table2), cfgvars[FONTSET_IDX].widget, 0, 1, 3,
					 4, GTK_FILL, GTK_FILL, 0, 0);

	btn = gtk_button_new_with_label(_("Browse Fonts"));
	gtk_table_attach(GTK_TABLE(table2), btn, 0, 1, 4, 5, GTK_FILL, GTK_FILL, 0,
					 0);
	gtk_signal_connect(GTK_OBJECT(btn), "clicked",
					   GTK_SIGNAL_FUNC(create_fontsel), cfgvars[FONT_IDX].widget);

	gtk_box_pack_start(GTK_BOX(wid), table2, 0, 0, 0);
}

static void settings_page_xtext(GtkWidget *vbox)
{
	GtkWidget *lbl;
	GtkWidget *wid;
	GtkWidget *table;
	GtkWidget *table2;
	GtkAdjustment *adj;

	wid = settings_create_group(vbox, _("Transparency"));

	table = gtk_table_new(4, 1, 0);
	gtk_table_set_row_spacings(GTK_TABLE(table), 10);
	gtk_container_set_border_width(GTK_CONTAINER(table), 10);

	cfgvars[TRANSPARENT_IDX].widget = gtk_check_button_new_with_label(
		_("Use Transparent XText Widget"));
	gtk_toggle_button_set_active((GtkToggleButton *)
								 cfgvars[TRANSPARENT_IDX].widget,
								 gtkhx_prefs.trans_xtext);
	gtk_table_attach(GTK_TABLE(table), cfgvars[TRANSPARENT_IDX].widget, 0, 1, 0, 1,
					 GTK_FILL, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(wid), table, 0, 0, 0);

#ifdef USE_GDK_PIXBUF
	settings_slider(wid, _("Red: "), &gtkhx_prefs.tint_red);
	settings_slider(wid, _("Green: "), &gtkhx_prefs.tint_green);
	settings_slider(wid, _("Blue: "), &gtkhx_prefs.tint_blue);
#endif

	wid = settings_create_group(vbox, _("Miscellaeneous"));

	table2 = gtk_table_new(2, 2, 0);

	cfgvars[TIMESTAMP_IDX].widget = gtk_check_button_new_with_label(
		_("Timestamp Chat"));
	gtk_toggle_button_set_active((GtkToggleButton *)cfgvars[TIMESTAMP_IDX].widget,
								 gtkhx_prefs.timestamp);
	gtk_table_attach(GTK_TABLE(table2), cfgvars[TIMESTAMP_IDX].widget, 0, 1, 0, 1,
					 GTK_FILL, 0, 0, 0);

	cfgvars[WORDWRAP_IDX].widget = gtk_check_button_new_with_label(
		_("Word Wrap"));
	gtk_toggle_button_set_active((GtkToggleButton *)cfgvars[WORDWRAP_IDX].widget,
								 gtkhx_prefs.word_wrap);
	gtk_table_attach(GTK_TABLE(table2), cfgvars[WORDWRAP_IDX].widget, 1, 2, 0, 1,
					 GTK_FILL, 0, 0, 0);

	adj = (GtkAdjustment *)gtk_adjustment_new(gtkhx_prefs.xbuf_max, 0, 0xffff,
											  1, 1, 1);
	cfgvars[XBUF_MAX_IDX].widget = gtk_spin_button_new(adj, 1, 0);
	lbl = gtk_label_new(_("Maximum Lines (0 = Unlimited)    "));

	gtk_table_attach(GTK_TABLE(table2), lbl, 0, 1, 1, 2, GTK_FILL, 0, 0, 0);
	gtk_table_attach(GTK_TABLE(table2), cfgvars[XBUF_MAX_IDX].widget, 1, 2, 1, 2,
					 GTK_FILL, 0, 0, 0);

	gtk_box_pack_start(GTK_BOX(wid), table2, 0, 0, 0);
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

	gtk_box_pack_start(GTK_BOX(wid), desc, 0, 0, 0);

	wid = settings_create_group(vbox, _("Paths"));

	table = gtk_table_new(3, 2, 0);
	gtk_table_set_row_spacings(GTK_TABLE(table), 10);
	gtk_table_set_col_spacings(GTK_TABLE(table), 5);

	lbl = gtk_label_new(_("Icon Path:"));
	gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_LEFT);
	gtk_misc_set_alignment(GTK_MISC(lbl), 0, 0.5);
	gtk_table_attach(GTK_TABLE(table), lbl, 0, 1, 0, 1, GTK_FILL,
					 (GtkAttachOptions) 0, 0, 0);

	cfgvars[ICONS_IDX].widget = gtk_entry_new();
	gtk_table_attach(GTK_TABLE(table), cfgvars[ICONS_IDX].widget, 1, 2, 0, 1,
					 (GtkAttachOptions) (GTK_EXPAND|GTK_FILL),
					 (GtkAttachOptions) 0, 0, 0);
	gtk_entry_set_text(GTK_ENTRY(cfgvars[ICONS_IDX].widget), gtkhx_prefs.icon_str);

	lbl = gtk_label_new(_("Sound Path:"));
	gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_LEFT);
	gtk_misc_set_alignment(GTK_MISC(lbl), 0, 0.5);
	gtk_table_attach(GTK_TABLE(table), lbl, 0, 1, 1, 2, GTK_FILL,
					 (GtkAttachOptions) 0, 0, 0);

	cfgvars[SOUNDPATH_IDX].widget = gtk_entry_new();
	gtk_table_attach(GTK_TABLE(table), cfgvars[SOUNDPATH_IDX].widget, 1, 2, 1, 2,
					 (GtkAttachOptions) (GTK_EXPAND|GTK_FILL),
					 (GtkAttachOptions) 0, 0, 0);
	gtk_entry_set_text(GTK_ENTRY(cfgvars[SOUNDPATH_IDX].widget),
					   gtkhx_prefs.sound_path);

	lbl = gtk_label_new(_("Download Path:"));
	gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_LEFT);
	gtk_misc_set_alignment(GTK_MISC(lbl), 0, 0.5);
	gtk_table_attach(GTK_TABLE(table), lbl, 0, 1, 2, 3, GTK_FILL,
					 (GtkAttachOptions) 0, 0, 0);

	cfgvars[DOWNLOAD_IDX].widget = gtk_entry_new();
	gtk_table_attach(GTK_TABLE(table), cfgvars[DOWNLOAD_IDX].widget, 1, 2, 2,
					 3, (GtkAttachOptions) (GTK_EXPAND|GTK_FILL),
					 (GtkAttachOptions) 0, 0, 0);
	gtk_entry_set_text(GTK_ENTRY(cfgvars[DOWNLOAD_IDX].widget),
					   gtkhx_prefs.download_path);

	gtk_box_pack_start(GTK_BOX(wid), table, 0, 0, 0);
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

static void
icon_row_selected (GtkWidget *widget, gint row, gint column, GdkEventButton *event, gpointer data)
{
	guint16 icon;
	char buf[16];

	if(!GTK_HLIST(widget)->rows) return;
	icon = GPOINTER_TO_INT(gtk_hlist_get_row_data(GTK_HLIST(widget), row));
	if(!icon) {
		return;
	}
	sprintf(buf, "%u", icon);
	gtk_entry_set_text(GTK_ENTRY(cfgvars[ICON_IDX].widget), buf);
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

	table = gtk_table_new(3, 2, 0);
	gtk_table_set_row_spacings(GTK_TABLE(table), 10);
	gtk_table_set_col_spacings(GTK_TABLE(table), 5);

	label = gtk_label_new(_("Icon ID: "));
	gtk_table_attach(GTK_TABLE(table), label, 0, 1, 0, 1, 0, 0, 0, 0);

	cfgvars[ICON_IDX].widget = gtk_entry_new();
	gtk_table_attach(GTK_TABLE(table), cfgvars[ICON_IDX].widget, 1, 2, 0, 1,
			 (GtkAttachOptions)(GTK_EXPAND | GTK_FILL),
			 (GtkAttachOptions)0, 0, 0);
	sprintf(iconstr, "%u", sessions[0].htlc.icon);
	gtk_entry_set_text(GTK_ENTRY(cfgvars[ICON_IDX].widget), iconstr);

	scroll = gtk_scrolled_window_new(0,0);

	icon_list = gtk_hlist_new(2);
	gtk_hlist_set_selection_mode(GTK_HLIST(icon_list), GTK_SELECTION_SINGLE);
	gtk_hlist_set_column_width(GTK_HLIST(icon_list), 0, 260);
	gtk_hlist_set_column_width(GTK_HLIST(icon_list), 1, 42);
	gtk_hlist_set_row_height(GTK_HLIST(icon_list), 18);
	gtk_hlist_set_compare_func(GTK_HLIST(icon_list),
							   (GtkHListCompareFunc)listsorthelper);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
	gtk_widget_set_usize(scroll, 232, 256);

	gtk_signal_connect(GTK_OBJECT(icon_list), "select_row",
			   GTK_SIGNAL_FUNC(icon_row_selected), iv);

	gtk_container_add(GTK_CONTAINER(scroll), icon_list);
	gtk_table_attach(GTK_TABLE(table), scroll, 0, 2, 1, 2,
			 (GtkAttachOptions)(GTK_EXPAND | GTK_FILL),
			 (GtkAttachOptions)0, 0, 0);

	iv->icon_list = icon_list;
	iv->nfound = 0;
	iv->icon_high = 0;

	gtk_box_pack_start(GTK_BOX(wid), table, 0, 0, 0);
}

static void settings_page_misc(GtkWidget *vbox)
{
	GtkWidget *wid;
	GtkWidget *table;

	wid = settings_create_group(vbox, _("Miscellaeneous"));

	table = gtk_table_new(7, 2, 0);
	gtk_table_set_row_spacings(GTK_TABLE(table), 10);
	gtk_table_set_col_spacings(GTK_TABLE(table), 5);


	cfgvars[AUTOREPLYON_IDX].widget = gtk_check_button_new_with_label(
		_("Auto Reply"));
	gtk_toggle_button_set_active((GtkToggleButton *)cfgvars[AUTOREPLYON_IDX].widget,
								 gtkhx_prefs.auto_reply);
	gtk_table_attach(GTK_TABLE(table), cfgvars[AUTOREPLYON_IDX].widget, 0, 1, 0, 1,
					 GTK_FILL, 0, 0, 0);

	cfgvars[AUTOREPLYMSG_IDX].widget = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(cfgvars[AUTOREPLYMSG_IDX].widget),
					   gtkhx_prefs.auto_reply_msg);
	gtk_table_attach(GTK_TABLE(table), cfgvars[AUTOREPLYMSG_IDX].widget, 1, 2, 0,
					 1, 0, 0, 0, 0);

	cfgvars[SHOWBACK_IDX].widget = gtk_check_button_new_with_label (
		_("Show Private Messages at Back"));
	gtk_toggle_button_set_active((GtkToggleButton *)cfgvars[SHOWBACK_IDX].widget,
								 gtkhx_prefs.showback);
	gtk_table_attach(GTK_TABLE(table), cfgvars[SHOWBACK_IDX].widget, 0, 1, 1, 2,
			 (GtkAttachOptions)(GTK_FILL),
			 (GtkAttachOptions)(GTK_FILL), 0, 0);

	cfgvars[QUEUEDL_IDX].widget = gtk_check_button_new_with_label(
		_("Queue File Transfers"));
	gtk_toggle_button_set_active((GtkToggleButton *)
								 cfgvars[QUEUEDL_IDX].widget, gtkhx_prefs.queuedl);
	gtk_table_attach(GTK_TABLE(table), cfgvars[QUEUEDL_IDX].widget, 0, 1, 2, 3,
			 (GtkAttachOptions)(GTK_FILL), (GtkAttachOptions)(GTK_FILL), 0, 0);

	cfgvars[SHOWJOIN_IDX].widget = gtk_check_button_new_with_label(
		_("Show Join/Leave in Chat"));
	gtk_toggle_button_set_active((GtkToggleButton *)
									cfgvars[SHOWJOIN_IDX].widget,
									gtkhx_prefs.showjoin);
	gtk_table_attach(GTK_TABLE(table), cfgvars[SHOWJOIN_IDX].widget, 0, 1, 3, 4,
			 (GtkAttachOptions)(GTK_FILL),
			 (GtkAttachOptions)(GTK_FILL), 0, 0);

	cfgvars[TRACKER_CASE_IDX].widget = gtk_check_button_new_with_label(
		_("Case Sensitive Tracker Searching"));
	gtk_toggle_button_set_active((GtkToggleButton *)
								 cfgvars[TRACKER_CASE_IDX].widget,
								 gtkhx_prefs.track_case);
	gtk_table_attach(GTK_TABLE(table), cfgvars[TRACKER_CASE_IDX].widget, 0, 1, 4, 5,
			 (GtkAttachOptions)(GTK_FILL),
			 (GtkAttachOptions)(GTK_FILL), 0, 0);

	cfgvars[OLD_NICKCOMPLETION_IDX].widget = gtk_check_button_new_with_label(
		_("Use old-style nick completion"));
	gtk_toggle_button_set_active((GtkToggleButton *)
								 cfgvars[OLD_NICKCOMPLETION_IDX].widget,
								 gtkhx_prefs.old_nickcompletion);
	gtk_table_attach(GTK_TABLE(table),cfgvars[OLD_NICKCOMPLETION_IDX].widget, 0,
					 1, 5, 6, (GtkAttachOptions)(GTK_FILL),
					 (GtkAttachOptions)(GTK_FILL), 0, 0);

	gtk_box_pack_start(GTK_BOX(wid), table, 0, 0, 0);
}

static void settings_page_general(GtkWidget *vbox)
{
	GtkWidget *wid;
	GtkWidget *table;
	GtkWidget *name;

	wid = settings_create_group(vbox, _("General"));

	table = gtk_table_new(2, 2, 0);
	gtk_table_set_row_spacings(GTK_TABLE(table), 10);
	gtk_table_set_col_spacings(GTK_TABLE(table), 5);

	cfgvars[NICK_IDX].widget = gtk_entry_new();
	gtk_table_attach(GTK_TABLE(table), cfgvars[NICK_IDX].widget, 1, 2, 0, 1,
			 (GtkAttachOptions)(GTK_EXPAND | GTK_FILL),
			 (GtkAttachOptions)0, 0, 0);
	gtk_entry_set_text(GTK_ENTRY(cfgvars[NICK_IDX].widget), sessions[0].htlc.name);

	name = gtk_label_new(_("Your Name:"));
	gtk_table_attach(GTK_TABLE(table), name, 0, 1, 0, 1,
	                  (GtkAttachOptions)(GTK_FILL),
	                  (GtkAttachOptions)(GTK_FILL), 0, 0);
	gtk_label_set_justify(GTK_LABEL(name), GTK_JUSTIFY_LEFT);
	gtk_misc_set_alignment(GTK_MISC(name), 0, 0.5);

	gtk_box_pack_start(GTK_BOX(wid), table, 0, 0, 0);
}

static void
settings_ctree_select (GtkWidget * ctree, GtkCTreeNode * node)
{
	GtkWidget *book;
	gint page;

	if (!GTK_CLIST (ctree)->selection)
		return;

	book = GTK_WIDGET (gtk_object_get_user_data (GTK_OBJECT (ctree)));
	page = (gint) gtk_ctree_node_get_row_data (GTK_CTREE (ctree), node);

	gtk_notebook_set_page (GTK_NOTEBOOK (book), page);
}

static GtkWidget *
settings_create_page (GtkWidget * book, gchar * book_label, GtkWidget * ctree,
							 gchar * tree_label, GtkCTreeNode * parent,
							 GtkCTreeNode ** node, gint page_index,
							 void (*draw_func) (GtkWidget *))
{
	GtkWidget *frame;
	GtkWidget *label;
	GtkWidget *vvbox;
	GtkWidget *vbox;

	vvbox = gtk_vbox_new (FALSE, 0);

	/* border for the label */
	frame = gtk_frame_new (NULL);
	gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_OUT);
	gtk_box_pack_start (GTK_BOX (vvbox), frame, FALSE, TRUE, 0);

	/* label */
	label = gtk_label_new (book_label);
	gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
	gtk_misc_set_padding (GTK_MISC (label), 2, 1);
	gtk_container_add (GTK_CONTAINER (frame), label);

	/* vbox for the tab */
	vbox = gtk_vbox_new (FALSE, 2);
	gtk_container_set_border_width (GTK_CONTAINER (vbox), 4);
	gtk_container_add (GTK_CONTAINER (vvbox), vbox);

	/* label on the tree */
	*node = gtk_ctree_insert_node (GTK_CTREE (ctree), parent, NULL,
								   &tree_label, 0, NULL, NULL, NULL, NULL,
								   FALSE, FALSE);
	gtk_ctree_node_set_row_data (GTK_CTREE (ctree), *node,
										  (gpointer) page_index);

	/* call the draw func if there is one */
	if (draw_func)
		draw_func (vbox);


	gtk_notebook_append_page (GTK_NOTEBOOK (book), vvbox, NULL);
	return vbox;
}

void create_options_window(GtkWidget *widget, gpointer data)
{
	GtkCTreeNode *last_top;
	GtkCTreeNode *last_child;
	GtkWidget *dialog;
	GtkWidget *hbbox;
	GtkWidget *frame;
	GtkWidget *ctree;
	GtkWidget *book;
	GtkWidget *hbox;
	GtkWidget *vbox;
	GtkWidget *wid;
	gchar *titles[1];
	gint page_index;
	session *sess = data;

	if(options_window) {
		gdk_window_raise(options_window->window);
		return;
	}

	dialog = gtk_dialog_new ();
	gtk_window_set_wmclass(GTK_WINDOW(dialog), "options", "GtkHx");
	gtk_window_set_title (GTK_WINDOW (dialog), _("GtkHx Preferences"));
	gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_CENTER);
	gtk_widget_set_usize(dialog, 570, 400);
	gtk_object_set_data(GTK_OBJECT(dialog), "sess", sess);
	gtk_signal_connect_object (GTK_OBJECT (dialog), "delete_event",
							   GTK_SIGNAL_FUNC (close_options_window), 0);

	options_window = dialog;

	gtk_container_set_border_width
		(GTK_CONTAINER (GTK_DIALOG (dialog)->action_area), 2);
	gtk_box_set_homogeneous (GTK_BOX (GTK_DIALOG (dialog)->action_area),
									 FALSE);

	hbbox = gtk_hbutton_box_new ();
	gtk_button_box_set_spacing (GTK_BUTTON_BOX (hbbox), 4);
	gtk_box_pack_end (GTK_BOX (GTK_DIALOG (dialog)->action_area), hbbox,
							FALSE, FALSE, 0);

	wid = gtk_button_new_with_label (_("OK"));
	gtk_signal_connect (GTK_OBJECT (wid), "clicked",
						GTK_SIGNAL_FUNC (options_change), GINT_TO_POINTER(0));
	gtk_box_pack_start (GTK_BOX (hbbox), wid, 0, 0, 0);

	wid = gtk_button_new_with_label (_("Apply"));
	gtk_signal_connect (GTK_OBJECT (wid), "clicked",
						GTK_SIGNAL_FUNC (options_change), GINT_TO_POINTER(1));
	gtk_box_pack_start (GTK_BOX (hbbox), wid, 0, 0, 0);

	wid = gtk_button_new_with_label (_("Cancel"));
	gtk_signal_connect (GTK_OBJECT (wid), "clicked",
							  GTK_SIGNAL_FUNC (close_options_window), 0);
	gtk_box_pack_start (GTK_BOX (hbbox), wid, 0, 0, 0);

	hbox = gtk_hbox_new (0, 6);
	gtk_container_set_border_width (GTK_CONTAINER (hbox), 6);
	gtk_box_pack_start (GTK_BOX (GTK_DIALOG (dialog)->vbox),
							  hbox, TRUE, TRUE, 0);

	titles[0] = _("Categories");
	ctree = gtk_ctree_new_with_titles (1, 0, titles);
	gtk_clist_set_selection_mode (GTK_CLIST (ctree), GTK_SELECTION_BROWSE);
	gtk_ctree_set_indent (GTK_CTREE (ctree), 15);
	gtk_widget_set_usize (ctree, 140, 0);
	gtk_box_pack_start (GTK_BOX (hbox), ctree, 0, 0, 0);

	frame = gtk_frame_new (NULL);
	gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_IN);
	gtk_box_pack_start (GTK_BOX (hbox), frame, TRUE, TRUE, 0);

	book = gtk_notebook_new ();
	gtk_notebook_set_show_tabs (GTK_NOTEBOOK (book), FALSE);
	gtk_notebook_set_show_border (GTK_NOTEBOOK (book), FALSE);
	gtk_container_add (GTK_CONTAINER (frame), book);
	gtk_object_set_user_data (GTK_OBJECT (ctree), book);
	gtk_signal_connect (GTK_OBJECT (ctree), "tree_select_row",
						GTK_SIGNAL_FUNC (settings_ctree_select), NULL);
	page_index = 0;

	vbox = settings_create_page(book, _("General Settings"), ctree,
								_("General Settings"), 0, &last_top,
								page_index++, settings_page_general);
	gtk_ctree_select (GTK_CTREE (ctree), last_top);

	vbox = settings_create_page(book, _("Tracker Settings"), ctree,
								_("Tracker Settings"), last_top, &last_child,
								page_index++, settings_page_tracker);

	vbox = settings_create_page(book, _("Icon Settings"), ctree,
								_("Icon Settings"), last_top, &last_child,
								page_index++, settings_page_icon);

	vbox = settings_create_page(book, _("Path Settings"), ctree,
								_("Paths"), last_top, &last_child,
								page_index++, settings_page_path);

	vbox = settings_create_page(book, _("Files Settings"), ctree,
								_("Files Settings"), last_top, &last_child,
								page_index++, settings_page_files);

	vbox = settings_create_page(book, _("Threaded News Settings"), ctree,
								_("Threaded News"), last_top, &last_child,
								page_index++, settings_page_news15);

#if 0 /* XXX */
	vbox = settings_create_page(book, _("Logging Settings"), ctree,
								_("Logging Settings"), last_top, &last_child,
								page_index++, settings_page_logging);
#endif

	vbox = settings_create_page(book, _("Miscellaeneous"), ctree,
								_("Miscellaeneous"), last_top, &last_child,
								page_index++, settings_page_misc);

	vbox = settings_create_page(book, _("XText Settings"), ctree,
								_("XText Settings"), 0, &last_top,
								page_index++, settings_page_xtext);

	vbox = settings_create_page(book, _("Font Settings"), ctree,
								_("Fonts Settings"), last_top, &last_child,
								page_index++, settings_page_font);

	vbox = settings_create_page(book, _("Sound Settings"), ctree,
								_("Sound Settings"), 0, &last_top,
								page_index++, settings_page_sound);

	gtk_ctree_expand_recursive ((GtkCTree *) ctree, 0);
	gtk_clist_select_row (GTK_CLIST (ctree), 0, 0);

	gtk_widget_show_all(dialog);

	list_icons();
}
