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
#include <unistd.h>
#include <stdlib.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <sys/types.h>
#include <ctype.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>
#include "hx.h"
#include "network.h"
#include "history.h"
#include "gtkutil.h"
#include "xtext.h"
#include "users.h"
#include "gtk_hlist.h"
#include "gtkhx.h"
#include "chat.h"
#include "plugin.h"
#include "tasks.h"
#include "rcv.h"
#include "connect.h"
#include "log.h"

#include "pixmaps/msg.xpm"
#include "pixmaps/chat.xpm"
#include "pixmaps/info.xpm"
#include "pixmaps/kick.xpm"
#include "pixmaps/ban.xpm"
#include "pixmaps/ignore.xpm"

static char *termed_buf = 0;
extern GdkFont *gtkhx_font;
extern GtkStyle *gtktext_style;

#define WORD_URL     1
#define WORD_NICK    2
#define WORD_HOST    4
#define WORD_EMAIL   5

GdkColor colors[] =
{
	{0, 0,      0,      0},      /* 0  black */
	{0, 0xcccc, 0xcccc, 0xcccc}, /* 1  white */
	{0, 0,      0,      0xcccc}, /* 2  blue */
	{0, 0,      0xcccc, 0},      /* 3  green */
	{0, 0xcccc, 0,      0},      /* 4  red */
	{0, 0xbbbb, 0xbbbb, 0},      /* 5  yellow/brown */
	{0, 0xbbbb, 0,      0xbbbb}, /* 6  purple */
	{0, 0xffff, 0xaaaa, 0},      /* 7  orange */
	{0, 0xffff, 0xffff, 0},      /* 8  yellow */
	{0, 0,      0xffff, 0},      /* 9  green */
	{0, 0,      0xcccc, 0xcccc}, /* 10 aqua */
	{0, 0,      0xffff, 0xffff}, /* 11 light aqua */
	{0, 0,      0,      0xffff}, /* 12 blue */
	{0, 0xffff, 0,      0xffff}, /* 13 pink */
	{0, 0x7777, 0x7777, 0x7777}, /* 14 grey */
	{0, 0x9999, 0x9999, 0x9999}, /* 15 light grey */
	{0, 0,      0,      0xcccc}, /* 16 blue markBack */
	{0, 0xeeee, 0xeeee, 0xeeee}, /* 17 white markFore */
	{0, 0xcccc, 0xcccc, 0xcccc}, /* 18 foreground (white) */
	{0, 0,      0,      0},      /* 19 background (black) */
};

void hx_send_chat (struct htlc_conn *htlc, char *str, guint32 cid, 
				   guint16 style)
{
	style = htons(style);

	if (cid) {
		guint32 cids = htonl(cid);
		hlwrite(htlc, HTLC_HDR_CHAT, 0, 3,
				HTLC_DATA_STYLE, 2, &style,
				HTLC_DATA_CHAT, strlen(str), str,
				HTLC_DATA_CHAT_ID, 4, &cids);


	} 
	else {
		hlwrite(htlc, HTLC_HDR_CHAT, 0, 2,
				HTLC_DATA_STYLE, 2, &style,
				HTLC_DATA_CHAT, strlen(str), str);
	}
}

void hx_chat_user (struct htlc_conn *htlc, guint16 uid)
{
	uid = htons(uid);
	task_new(htlc, hx_rcv_user_change, 0, 0, "chat");
	hlwrite(htlc, HTLC_HDR_CHAT_CREATE, 0, 1, HTLC_DATA_UID, 2, &uid);
}

void hx_invite_user(struct htlc_conn *htlc, guint16 uid, guint32 cid)
{
	cid = htonl(cid);
	uid = htons(uid);
	task_new(htlc, 0, 0, 0, "invite");
	hlwrite(htlc, HTLC_HDR_CHAT_INVITE, 0, 2, HTLC_DATA_CHAT_ID, 4, &cid, 
			HTLC_DATA_UID, 2, &uid);
}

void hx_chat_join (struct htlc_conn *htlc, guint32 cid)
{
	struct chat *chat;
	chat = chat_with_cid(sess_from_htlc(htlc), cid);


	if(chat) {
		return;
	}
	else {
		chat = chat_new(sess_from_htlc(htlc), cid);
		cid = htonl(cid);
		task_new(htlc, rcv_task_user_list_switch, chat, 0, "join");
		hlwrite(htlc, HTLC_HDR_CHAT_JOIN, 0, 1, HTLC_DATA_CHAT_ID, 4, &cid);
	}
}

void hx_part_chat(struct htlc_conn *htlc, guint32 cid)
{
	struct chat *chat;

	chat = chat_with_cid(sess_from_htlc(htlc), cid);
	cid = htonl(chat->cid);
	hlwrite(htlc, HTLC_HDR_CHAT_PART, 0, 1, HTLC_DATA_CHAT_ID, 4, &cid);
}

void hx_change_subject(struct htlc_conn *htlc, guint32 cid, char *subject)
{
	cid = htonl(cid);
	hlwrite(htlc, HTLC_HDR_CHAT_SUBJECT, 0, 2, HTLC_DATA_CHAT_ID, 4, &cid, 
			HTLC_DATA_CHAT_SUBJECT, strlen(subject), subject);
}

int word_check (GtkWidget * xtext, char *word)
{
	char *at, *dot;
	int i, dots;
	size_t len = strlen (word);

	if (!strncasecmp (word, "irc://", 6))
		return WORD_URL;

	if (!strncasecmp (word, "irc.", 4))
		return WORD_URL;

	if (!strncasecmp (word, "ftp.", 4))
		return WORD_URL;

	if (!strncasecmp (word, "ftp:", 4))
		return WORD_URL;

	if (!strncasecmp (word, "www.", 4))
		return WORD_URL;

	if (!strncasecmp (word, "http:", 5))
		return WORD_URL;

	if (!strncasecmp (word, "https:", 6))
		return WORD_URL;

/*	if (find_name (sess, word))
	return WORD_NICK; */

	at = strchr (word, '@');	  /* check for email addy */
	dot = strrchr (word, '.');
	if (at && dot)
	{
		if ((unsigned long) at < (unsigned long) dot)
		{
			if (strchr (word, '*'))
				return WORD_HOST;
			else
				return WORD_EMAIL;
		}
	}

	/* check if it's an IP number */
	dots = 0;
	for (i = 0; i < len; i++)
	{
		if (word[i] == '.')
			dots++;
	}
	if (dots == 3)
	{
		if (inet_addr (word) != -1)
			return WORD_HOST;
	}

	if (!strncasecmp (word + len - 5, ".html", 5))
		return WORD_HOST;

	if (!strncasecmp (word + len - 4, ".org", 4))
		return WORD_HOST;

	if (!strncasecmp (word + len - 4, ".net", 4))
		return WORD_HOST;

	if (!strncasecmp (word + len - 4, ".com", 4))
		return WORD_HOST;

	if (!strncasecmp (word + len - 4, ".edu", 4))
		return WORD_HOST;

	if (len > 5)
	{
		if (word[len - 3] == '.' &&
			 isalpha (word[len - 2]) && isalpha (word[len - 1]))
			return WORD_HOST;
	}

	return 0;
}

static void
timecpy (char *buf)
{
	time_t timval = time (0);
	char *tim = ctime (&timval) + 10;
	memcpy (buf, tim, 9);
	buf[0] = '[';
	buf[9] = ']';
	buf[10] = ' ';
}

struct chat *chat_new (session *sess, guint32 cid)
{
	struct chat *chat;

	chat = g_malloc0(sizeof(struct chat));
	chat->cid = cid;
	chat->user_list = &chat->__user_list;
	chat->user_tail = &chat->__user_list;

	chat->next = 0;
	chat->prev = sess->chat_tail;
	sess->chat_tail->next = chat;
	sess->chat_tail = chat;

	return chat;
}

void
chat_delete (session *sess, struct chat *chat)
{
	if (chat->next)
		chat->next->prev = chat->prev;
	if (chat->prev)
		chat->prev->next = chat->next;
	if (sess->chat_tail == chat)
		sess->chat_tail = chat->prev;
	if (sess->chat_front == chat)
		sess->chat_front = &sess->__chat_list;
	g_free(chat);
}

struct chat *chat_with_cid (session *sess, guint32 cid)
{
	struct chat *chatp;

	for (chatp = sess->chat_front; chatp; chatp = chatp->next)
		if (chatp->cid == cid)
			return chatp;

	return 0;
}

struct gtkhx_chat *gchat_with_cid (session *sess, guint32 cid)
{
	struct gtkhx_chat *gchat;

	for (gchat = sess->gchat_list; gchat; gchat = gchat->prev) {
			if (gchat->cid == cid) {
				return gchat;
			}
	}

	return 0;
}

void gchat_delete (session *sess, struct gtkhx_chat *gchat)
{
	if (gchat->next)
		gchat->next->prev = gchat->prev;
	if (gchat->prev)
		gchat->prev->next = gchat->next;
	if (gchat == sess->gchat_list)
		sess->gchat_list = gchat->prev;
	g_free(gchat);
}

void xprintline(GtkWidget *text, char *chat, size_t len)
{
	char *new_text;

	if(len == -1) {
		len = strlen(chat);
	}
	if(len == 0) {
		len = 1;
	}
	if(gtkhx_prefs.timestamp) {
		new_text = g_malloc(len+12);
		timecpy(new_text);
		memcpy(new_text +11, chat, len);
		gtk_xtext_append(GTK_XTEXT(text), new_text, len+11);
		g_free(new_text);
	}
	else {
		gtk_xtext_append(GTK_XTEXT(text), chat, len);
	}
}

static void xoutput_chat (session *sess, guint32 cid, char *chat)
{
	char *cr;
	struct gtkhx_chat *gchat;

	gchat = gchat_with_cid(sess, cid);

	if(!gchat) {
		return;
	}

#if 0
	if(gtkhx_prefs.logging) {
		if(!server_log) {
			/* XXX: open it up here */
#warning FIXME
		}
		
		
		if(cid == 0 && server_log) {
			char *copy = g_strdup(chat);
			int len = strlen(chat);
			
			if(len > 18 && !strncmp(INFOPREFIX, copy, 18)) {
				memmove(&copy[7], &copy[18], len-17);
				sprintf(copy, " [hx] %s", &copy[7]);
				len = strlen(copy);
			}
			if(gtkhx_prefs.timestamp) {
				char *new_text = g_malloc0(len+12);
				timecpy(new_text);
				memcpy(new_text +11, copy, len);
				print_log(server_log, new_text);
				g_free(new_text);
			}
			else {
				print_log(server_log, copy);
			}
			g_free(copy);
		}
	}
#endif

	cr = strchr(chat, '\n');
	if(cr) {
		while(1) {
			xprintline(gchat->output, chat, cr-chat);
			chat = cr + 1;
			if(*chat == 0) {
				break;
			}
			cr =strchr(chat, '\n');
			if(!cr) {
				xprintline(gchat->output, chat, -1);
				break;
			}
		}
	}
	else {
		xprintline(gchat->output, chat, -1);
	}
}

void hx_printf_prefix (struct htlc_conn *htlc, guint32 cid, const char *prefix,
					   const char *fmt, ...)
{
	va_list ap;
	va_list save;
	char autobuf[256], *buf;
	size_t mal_len;
	size_t plen;
	session *sess = sess_from_htlc(htlc);

	if(!sess) {
		return;
	}

	__va_copy(save, ap);
	mal_len = 256;
	buf = autobuf;
	plen = strlen(prefix);
	for (;;) {
		va_start(ap, fmt);
		vsnprintf(buf + plen, mal_len - plen, fmt, ap);
		va_end(ap);
		if (strlen(buf+plen) != mal_len-plen-1)
			break;
		__va_copy(ap, save);
		mal_len <<= 1;
		if (buf == autobuf)
			buf = g_malloc(mal_len);
		else
			buf = g_realloc(buf, mal_len);
	}
	memcpy(buf, prefix, plen);

	xoutput_chat(sess, cid, buf);

	if (buf != autobuf)
		g_free(buf);
}


void hx_printf (struct htlc_conn *htlc, guint32 cid, const char *fmt, ...)
{
	va_list ap;
	va_list save;
	char autobuf[256], *buf;
	size_t mal_len;
	session *sess = sess_from_htlc(htlc);

	if(!sess) {
		return;
	}

	__va_copy(save, ap);
	mal_len = 256;
	buf = autobuf;
	for (;;) {
		va_start(ap, fmt);
		vsnprintf(buf, mal_len, fmt, ap);
		va_end(ap);
		if (strlen(buf) != mal_len-1)
			break;
		__va_copy(ap, save);
		mal_len <<= 1;
		if (buf == autobuf)
			buf = g_malloc(mal_len);
		else
			buf = g_realloc(buf, mal_len);
	}
	xoutput_chat(sess, cid, buf);

	if (buf != autobuf)
		g_free(buf);
}

static int
nick_comp_get_nick (char *tx, char *n)
{
	size_t c, len = strlen (tx);

	for (c = 0; c < len; c++)
	{
      if (tx[c] == ':' || tx[c] == ',' || tx[c] == ':')
		{
			n[c] = 0;
			return 0;
		}
		if (tx[c] == ' ' || tx[c] == '.' || tx[c] == 0)
			return -1;
		n[c] = tx[c];
	}
	return -1;
}

static void
nick_comp_chng (session *sess, char *text, int updown)
{
	struct hx_user *user, *last = NULL;
	char nick[64];
	size_t len, slen;

	if (nick_comp_get_nick (text, nick) == -1)
		return;
	len = strlen (nick);

	for(user = sess->chat_front->user_list->next; user; user = user->next)  {
		slen = strlen (user->name);
		if (len != slen) {
			last = user;
			continue;
		}
		if (strncasecmp (user->name, nick, len) == 0) {
			if (updown == 0) {
				if (user->next == NULL) {
					return;
				}
				snprintf (nick, sizeof (nick), "%s%c ", (
							  (struct hx_user *) user->next)->name, ':');
			}

			else {
				if (last == NULL) {
					return;
				}
				snprintf (nick, sizeof (nick), "%s%c ", last->name, ':');
			}
			return;
		}
		last = user;
	}
}

static int
tab_nick_comp_next (session *sess, char *b4, char *nick, char *c5, int shift)
{
	struct hx_user *user = 0, *last = NULL;
	char buf[4096];

	for(user = sess->chat_front->user_list->next; user; user = user->next) {
		if (strcmp (user->name, nick) == 0)
			break;
		last = user;
	}
	if (!user)
		return 0;
	if (shift) {
		if (last)
			snprintf (buf, 4096, "%s %s%s", b4, last->name, c5);
		else
			snprintf (buf, 4096, "%s %s%s", b4, nick, c5);
	}

	else {
		if (user && user->next) {
			snprintf (buf, 4096, "%s %s%s", b4, (user->next)->name, c5);
		}
		else {
			if (sess->chat_front->user_list->next) {
				snprintf (buf, 4096, "%s %s%s", b4, 
						  (sess->chat_front->user_list->next)->name, c5);
			}
			else {
				snprintf (buf, 4096, "%s %s%s", b4, nick, c5);
			}
		}
	}

	return 1;
}

static int tab_nick_comp (session *sess, char *text, int shift, int pos, 
						  GtkWidget *entry)
{
	struct hx_user *user = 0, *match_user = 0;
	char not_nick_chars[16] = "";
	int first = 0, i, j, match_count = 0;
	size_t len, slen, match_pos = 0;
	char buf[2048], nick_buf[2048] = {0}, *b4 = NULL, *c5 = NULL, 
											  *match_text = NULL, 
											  *nick = NULL, 
											  *current_nick = NULL, 
											  match_char = -1, *ptr;
	GSList *match_list = NULL, *first_match = NULL, *node1 = NULL, 
		*node2 = NULL, *next = NULL;

	len = strlen (text);

	/* Is the text more than just a nick? */

	sprintf(not_nick_chars, " .?%c", ':');

	if (strcspn (text, not_nick_chars) != strlen (text)) {
		/* If we're doing old-style nick completion and the text input widget
		 * contains a string of the format: "nicknameSUFFIX" or"nicknameSUFFIX ",
		 * where SUFFIX is the Nickname Completion Suffix character, then cycle
		 * through the available nicknames.
		 */
		if(gtkhx_prefs.old_nickcompletion) {
			char * space = strchr(text, ' ');

			if ((!space || space == &text[len - 1]) && text[len - 
															(space ? 2 : 1)] == 
				':') {
				/* This causes the nickname to cycle. */
				nick_comp_chng(sess, text, shift);
				return 0;
			}
		}
		j = pos;

		/* !! FIXME !! */
		if (len - j < 0)
			return 0;

		b4 = (char *) g_malloc (len + 1);
		c5 = (char *) g_malloc (len + 1);
		memmove (c5, &text[j], len - j);
		c5[len - j] = 0;
		memcpy (b4, text, len + 1);

		for (i = j - 1; i > -1; i--) {
			if (b4[i] == ' ') {
				b4[i] = 0;
				break;
			}
			b4[i] = 0;
		}
		memmove (text, &text[i + 1], (j - i) + 1);
		text[(j - i) - 1] = 0;

		if (tab_nick_comp_next (sess, b4, text, c5, shift)) {
			g_free (b4);
			g_free (c5);
			return 0;
		}
		first = 0;
	} else
		first = 1;

	len = strlen (text);

	if (text[0] == 0)
		return 0;

	/* make a list of matches */
	for(user = sess->chat_front->user_list->next; user; user = user->next) {
		slen = strlen (user->name);
		if (len > slen) {
			continue;
		}
		if (strncasecmp (user->name, text, len) == 0) {
			match_list = g_slist_prepend (match_list, user);
		}
	}
	match_list = g_slist_reverse (match_list); /* faster then _append */
	match_count = g_slist_length (match_list);


	/* no matches, return */
	if (match_count == 0) {
		if (!first) {
			g_free (b4);
			g_free (c5);
		}
		return 0;
	}
	first_match = match_list;
	match_pos = len;

	/* remove duplicate entries */
	for(node1 = match_list; node1; node1 = g_slist_next(node1)) {
		for(node2 = match_list; node2; node2 = next) {
			next = g_slist_next(node2);
			if(node1 && node2 && (node1 != node2) &&
			   node1->data && node2->data && (node1->data != node2->data) &&
			   !strcasecmp(((struct hx_user *)node1->data)->name,
						   ((struct hx_user *)node2->data)->name)) {
				g_slist_remove(match_list, node2->data);
				match_count--;
			}
		}
	}


	if(!gtkhx_prefs.old_nickcompletion && match_count > 1) {
		while (1) {
			while (match_list) {
				current_nick = g_malloc(
					strlen(((struct hx_user *)match_list->data)->name) + 1);
				strcpy (current_nick, 
						((struct hx_user *)match_list->data)->name);
				if (match_char == -1) {
					match_char = current_nick[match_pos];
					match_list = g_slist_next (match_list);
					g_free (current_nick);
					continue;
				}
				if (tolower (current_nick[match_pos]) != tolower (match_char)){
					match_text = g_malloc (match_pos+1);
					current_nick[match_pos] = '\0';
					strcpy (match_text, current_nick);
					free (current_nick);
					match_pos = -1;
					break;
				}
				match_list = g_slist_next (match_list);
				g_free (current_nick);
			}


			if (match_pos == -1)
				break;


			match_list = first_match;
			match_char = -1;
			++match_pos;
		}
		match_list = first_match;
	}
	else {
		match_user = (struct hx_user *) match_list->data;
	}


	/* no match, if we found more common chars among matches, display 
	   them in entry */
	if (match_user == NULL) {
		while (match_list) {
			nick = ((struct hx_user *)match_list->data)->name;
			sprintf (nick_buf, "%s%s ", nick_buf, nick);
			match_list = g_slist_next (match_list);
		}
		hx_printf(&sess->htlc, 0, "%s", nick_buf);
		if (first) {
			snprintf (buf, sizeof (buf), "%s", match_text);
		}
		else {
			snprintf (buf, sizeof (buf), "%s %s%s", b4, match_text, c5);
			GTK_EDITABLE(entry)->current_pos = strlen (b4) + strlen (match_text);
			g_free (b4);
			g_free (c5);
		}
		g_free (match_text);
	}

	else {
		if (first) {
			snprintf (buf, sizeof (buf), "%s%c ", match_user->name, ':');
		}
		else {
			snprintf (buf, sizeof (buf), "%s %s%s", b4, match_user->name, c5);
			GTK_EDITABLE(entry)->current_pos = strlen (b4) + 
				strlen(match_user->name);
			if(b4)
				g_free (b4);
			if(c5)
				g_free (c5);
		}
	}

	ptr = buf;
	while(*ptr == ' ') ptr++;

	gtk_editable_delete_text (GTK_EDITABLE (entry), 0, -1);
	gtk_text_insert(GTK_TEXT(entry), 0, 0, 0, ptr, -1);

	return 0;
}

static void chat_input_key_press (GtkWidget *widget, GdkEventKey *event)
{
	GtkText *text;
	guint point, len;
	guint k, s;
	char *p;
	HIST_ENTRY *hent = NULL;
	struct gtkhx_chat *gchat = gtk_object_get_data(GTK_OBJECT(widget), "gchat");
	session *sess = gtk_object_get_data(GTK_OBJECT(widget), "sess");

	text = GTK_TEXT(widget);

	point = gtk_text_get_point(text);
	len = gtk_text_get_length(text);

	k = event->keyval; s = event->state;
	/* handle this weird bug */
	if (s & GDK_CONTROL_MASK) {
		switch(k) {
		case 'k':
		case 'K':
			create_connect_window(0,&sessions[0]);
			break;
		}
	}
	else if (s & GDK_SHIFT_MASK && k == GDK_Return) {
		/* insert a linebreak if shift is held */
		int position;

		position = text->point.index;
		gtk_editable_insert_text (GTK_EDITABLE(text), "\n", 1, &position);
		return;
	}
	else if (k == GDK_Return) {

		gtk_signal_emit_stop_by_name(GTK_OBJECT(widget), "key_press_event");
		gtk_text_set_editable(text, 0);
		g_free(termed_buf);
		termed_buf = gtk_editable_get_chars(GTK_EDITABLE(text), 0, len);

		add_history(gchat->chat_history, termed_buf);
		using_history(gchat->chat_history);

		hotline_client_input(&sess->htlc, termed_buf, gchat->cid, 
							 (s&GDK_CONTROL_MASK) ? 1 : 0);

		gtk_text_set_point(text, 0);
		gtk_text_forward_delete(text, len);
		gtk_text_set_editable(text, 1);
	}


	else if (k == GDK_Tab) {
		gtk_signal_emit_stop_by_name(GTK_OBJECT(widget), "key_press_event");
		p = gtk_editable_get_chars(GTK_EDITABLE(text), 0, -1);
		tab_nick_comp(sess, p, 1, point, widget);
		g_free(p);
		gtk_widget_grab_focus(GTK_WIDGET(text));
	}


	else if (k == GDK_Up) {
		hent = previous_history(gchat->chat_history);
	}
	else if (k == GDK_Down) {
		hent = next_history(gchat->chat_history);
	}

	if (hent) {
		gtk_text_freeze(text);
		gtk_text_set_point(text, 0);
		len = gtk_text_get_length(text);
		gtk_text_forward_delete(text, len);


		len = strlen(hent->line);
		gtk_text_insert(text, 0, 0, 0, hent->line, len);
		gtk_text_thaw(text);

		len = gtk_text_get_length(text);
		gtk_editable_set_position(GTK_EDITABLE(text), len);

	}
}

static void chat_move(GtkWidget *w, GdkEventConfigure *e, gpointer data) 
{
	int x, y, width, height;
	struct gtkhx_chat *gchat = data;
	session *sess = gtk_object_get_data(GTK_OBJECT(w), "sess");

	gdk_window_get_root_origin(sess->chat_window->window, &x, &y);
	gdk_window_get_size(sess->chat_window->window, &width, &height);

	if(e->send_event) { /* Is a position event */
		gtkhx_prefs.geo.chat.xpos = x;
		gtkhx_prefs.geo.chat.ypos = y;
	}
	else { /* Is a size event */
		gtkhx_prefs.geo.chat.xsize = width;
		gtkhx_prefs.geo.chat.ysize = height;
	}

	if(gtkhx_prefs.trans_xtext) {
		gtk_xtext_refresh(GTK_XTEXT(gchat->output), 1);
	}
}

static GtkWidget *chat_hbox;
static GtkWidget *wind_tmp;

static void chat_close (GtkWidget *widget, gpointer data)
{
	GtkWidget *hbox = chat_hbox;
	struct gtkhx_chat *gchat = data;

	wind_tmp = gtk_window_new(GTK_WINDOW_TOPLEVEL);

	gtk_container_remove(GTK_CONTAINER(hbox->parent), hbox);
	gtk_container_add(GTK_CONTAINER(wind_tmp), hbox);
	gtk_widget_realize(gchat->output);
	gchat->input = 0;
	gchat->subject = 0;

	gtkhx_prefs.geo.chat.open = 0;
	gtkhx_prefs.geo.chat.init = 0;
}

void generate_colors(GtkWidget *widget)
{
	int i;

	if (!colors[0].pixel) {
		for(i=0; i<20; i++) {
			colors[i].pixel = (unsigned long)((colors[i].red & 0xff00) * 256 +
											  (colors[i].green & 0xff00) +
											  (colors[i].blue & 0xff00) / 256);
			gdk_color_alloc(gtk_widget_get_colormap(widget), &colors[i]);
		}
	}
}


void create_chat(session *sess)
{
	struct gtkhx_chat *gchat;
	GtkWidget *text;
	GtkWidget *vscroll;

	gchat = g_malloc(sizeof(struct gtkhx_chat));

	text = gtk_xtext_new(0,0);
	gtk_xtext_set_palette (GTK_XTEXT (text), colors);
	gtk_xtext_set_font(GTK_XTEXT(text), gtkhx_font, 0);
	gtk_xtext_set_background(GTK_XTEXT(text), NULL, gtkhx_prefs.trans_xtext, 1);
	GTK_WIDGET_UNSET_FLAGS(text, GTK_CAN_FOCUS);
#ifdef USE_GDK_PIXBUF
	GTK_XTEXT(text)->tint_red = gtkhx_prefs.tint_red;
	GTK_XTEXT(text)->tint_green = gtkhx_prefs.tint_green;
	GTK_XTEXT(text)->tint_blue = gtkhx_prefs.tint_blue;
#endif
	GTK_XTEXT(text)->wordwrap = gtkhx_prefs.word_wrap;
	GTK_XTEXT(text)->urlcheck_function = word_check;
	GTK_XTEXT(text)->max_lines = gtkhx_prefs.xbuf_max;

	vscroll = gtk_vscrollbar_new(GTK_XTEXT(text)->adj);

	gtk_object_ref(GTK_OBJECT(text));
	gtk_object_sink(GTK_OBJECT(text));
	gtk_object_ref(GTK_OBJECT(vscroll));
	gtk_object_sink(GTK_OBJECT(vscroll));

	chat_hbox = gtk_hbox_new(0, 0);
	gtk_widget_set_usize(chat_hbox, (gtkhx_prefs.geo.chat.xsize<<6)/82, 
						 (gtkhx_prefs.geo.chat.ysize<<6)/1000);

	gtk_object_ref(GTK_OBJECT(chat_hbox));
	gtk_object_sink(GTK_OBJECT(chat_hbox));
	gtk_box_pack_start(GTK_BOX(chat_hbox), text, 1, 1, 0);
	gtk_box_pack_start(GTK_BOX(chat_hbox), vscroll, 0, 0, 0);

	wind_tmp = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_container_add(GTK_CONTAINER(wind_tmp), chat_hbox);
	gtk_widget_realize(text);

	gchat->chat_history = history_new();
	gchat->cid = 0;
	gchat->subject = 0;
	gchat->output = text;
	gchat->userlist = 0;
	gchat->next = 0;
	gchat->prev = sess->gchat_list;
	gchat->vscroll = vscroll;
	gchat->chat = 0;
	gchat->window = 0;
	gchat->input = 0;

	sess->gchat_list = gchat;
}

void change_subject(GtkWidget *widget, gpointer data)
{
	char *subject;

	subject = gtk_entry_get_text(GTK_ENTRY(widget));
	hx_change_subject(&sessions[0].htlc, GPOINTER_TO_INT(data), subject);
}

void create_chat_window (GtkWidget *widget, gpointer data)
{
	GtkWidget *hbox;
	GtkWidget *outputframe, *inputframe, *subj_frame;
	GtkWidget *vpaned;
	GtkWidget *chat_window;
	GtkWidget *vbox, *subj_hbox;
	struct gtkhx_chat *gchat;
	session *sess = data;

	if (gtkhx_prefs.geo.chat.open) {
		gdk_window_raise(sess->chat_window->window);
		return;
	}

	gchat = gchat_with_cid(sess, 0);
	chat_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_wmclass(GTK_WINDOW(chat_window), "chat", "GtkHx");

	gtk_widget_set_usize(chat_window, 412, 280);
	gtk_window_set_policy(GTK_WINDOW(chat_window), 1, 1, 0);

	gtk_signal_connect(GTK_OBJECT(chat_window), "destroy",
					   GTK_SIGNAL_FUNC(chat_close), gchat);
	gtk_window_set_title(GTK_WINDOW(chat_window), _("Chat"));
	gtk_container_set_border_width(GTK_CONTAINER(chat_window), 0);

	vbox = gtk_vbox_new(0, 4);
	gtk_container_set_border_width(GTK_CONTAINER(vbox), 5);

	gchat->subject = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(gchat->subject), sess->chat_front->subject);
	subj_hbox = gtk_hbox_new(0, 0);
	gtk_widget_set_usize(subj_hbox, (gtkhx_prefs.geo.chat.xsize<<6)/82, 20);
	subj_frame = gtk_frame_new(0);
	gtk_frame_set_shadow_type(GTK_FRAME(subj_frame), GTK_SHADOW_OUT);
	gtk_container_add(GTK_CONTAINER(subj_frame), subj_hbox);
	gtk_box_pack_start(GTK_BOX(subj_hbox), gchat->subject, 1, 1, 0);
	gtk_box_pack_start(GTK_BOX(vbox), subj_frame, 0, 1, 0);
	gtk_widget_set_style(gchat->subject, gtktext_style);
	gtk_signal_connect(GTK_OBJECT(gchat->subject), "activate",
					   GTK_SIGNAL_FUNC(change_subject), GINT_TO_POINTER(0));

	outputframe = gtk_frame_new(0);
	gtk_frame_set_shadow_type(GTK_FRAME(outputframe), GTK_SHADOW_IN);
	gtk_widget_set_usize(outputframe, (gtkhx_prefs.geo.chat.xsize<<6)/82, 
						 (gtkhx_prefs.geo.chat.ysize<<6)/100);

	inputframe = gtk_frame_new(0);
	gtk_frame_set_shadow_type(GTK_FRAME(inputframe), GTK_SHADOW_IN);

	vpaned = gtk_vpaned_new();
	gtk_paned_pack1(GTK_PANED(vpaned), outputframe, 0, 1);
	gtk_paned_pack2(GTK_PANED(vpaned), inputframe, 0, 1);
	gtk_container_set_border_width(GTK_CONTAINER(vpaned), 5);

	gtk_container_add(GTK_CONTAINER(chat_window), vbox);
	gtk_box_pack_start(GTK_BOX(vbox), vpaned, 1, 1, 0);

	if(wind_tmp) {
		gtk_container_remove(GTK_CONTAINER(wind_tmp), chat_hbox);
		gtk_widget_destroy(wind_tmp);
	}


	gtk_container_add(GTK_CONTAINER(outputframe), chat_hbox);
	gtk_widget_realize(gchat->output);

	hbox = gtk_hbox_new(0, 0);
	gtk_widget_set_usize(hbox, (gtkhx_prefs.geo.chat.xsize<<6)/100, 50);
	gtk_container_add(GTK_CONTAINER(inputframe), hbox);

	gchat->input = gtk_text_new(0, 0);
	gtk_widget_set_style(gchat->input, gtktext_style);
	gtk_signal_connect(GTK_OBJECT(gchat->input), "key_press_event", 
					   GTK_SIGNAL_FUNC(chat_input_key_press), 0);
	gtk_object_set_data(GTK_OBJECT(gchat->input), "gchat", gchat);
	gtk_object_set_data(GTK_OBJECT(gchat->input), "sess", sess);
	gtk_text_set_editable(GTK_TEXT(gchat->input), 1);
	gtk_text_set_word_wrap(GTK_TEXT(gchat->input), 1);


	gtk_box_pack_start(GTK_BOX(hbox), gchat->input, 1, 1, 0);
	gtk_signal_connect(GTK_OBJECT(chat_window), "configure_event", 
					   GTK_SIGNAL_FUNC(chat_move), gchat);

	gtk_object_set_data(GTK_OBJECT(chat_window), "sess", sess);

	gtk_widget_set_uposition(chat_window, gtkhx_prefs.geo.chat.xpos, 
							 gtkhx_prefs.geo.chat.ypos);
	gtk_widget_set_usize(chat_window, gtkhx_prefs.geo.chat.xsize, 
						 gtkhx_prefs.geo.chat.ysize);

	gtk_widget_show_all(chat_window);
	init_keyaccel(chat_window);

	if(connected) {
		changetitlespecific(chat_window, _("Chat"));
	}

	gtkhx_prefs.geo.chat.open = 1;
	gtkhx_prefs.geo.chat.init = 1;
	gtk_widget_grab_focus(gchat->input);

	sess->chat_window = chat_window;
}

struct gtkhx_chat *pchat_new (session *sess, struct chat *chat)
{
	GtkWidget *text;
	GtkWidget *vscroll;
	GtkWidget *subject;
	GtkWidget *userlist;
	struct gtkhx_chat *gchat;

	gchat = g_malloc(sizeof(struct gtkhx_chat));
	gchat->next = 0;
	gchat->prev = sess->gchat_list;

	if (sess->gchat_list) {
		sess->gchat_list->next = gchat;
	}

	text = gtk_xtext_new(0,0);
	gtk_xtext_set_palette(GTK_XTEXT(text), colors);
	gtk_xtext_set_font(GTK_XTEXT(text), gtkhx_font, 0);
#ifdef USE_GDK_PIXBUF
	GTK_XTEXT(text)->tint_red = gtkhx_prefs.tint_red;
	GTK_XTEXT(text)->tint_green = gtkhx_prefs.tint_green;
	GTK_XTEXT(text)->tint_blue = gtkhx_prefs.tint_blue;
#endif
	GTK_XTEXT(text)->wordwrap = gtkhx_prefs.word_wrap;
	GTK_XTEXT(text)->urlcheck_function = word_check;
	GTK_XTEXT(text)->max_lines = gtkhx_prefs.xbuf_max;

	gtk_xtext_set_background(GTK_XTEXT(text), 0, gtkhx_prefs.trans_xtext, 1);
	vscroll = gtk_vscrollbar_new(GTK_XTEXT(text)->adj);

	subject = gtk_entry_new();
	gtk_widget_set_style(subject, gtktext_style);

	userlist = gtk_hlist_new(2);
	gtk_hlist_set_column_width(GTK_HLIST(userlist), 0, 30);
	gtk_hlist_set_column_width(GTK_HLIST(userlist), 1, 210);
	gtk_hlist_set_row_height(GTK_HLIST(userlist), 18);
	gtk_hlist_set_shadow_type(GTK_HLIST(userlist), GTK_SHADOW_NONE);
	gtk_hlist_set_column_justification(GTK_HLIST(userlist), 0, 
									   GTK_JUSTIFY_LEFT);
	gtk_object_ref(GTK_OBJECT(text));
	gtk_object_sink(GTK_OBJECT(text));
	gtk_object_ref(GTK_OBJECT(vscroll));
	gtk_object_sink(GTK_OBJECT(vscroll));
	gtk_object_ref(GTK_OBJECT(subject));
	gtk_object_sink(GTK_OBJECT(subject));
	gtk_object_ref(GTK_OBJECT(userlist));
	gtk_object_sink(GTK_OBJECT(userlist));

	gchat->cid = chat->cid;
	gchat->chat = chat;
	gchat->output = text;
	gchat->vscroll = vscroll;
	gchat->subject = subject;
	gchat->userlist = userlist;
	gchat->chat_history = history_new();
	sess->gchat_list = gchat;

	return gchat;
}

static void pchat_close (GtkWidget *widget, gpointer data)
{
	struct gtkhx_chat *gchat = data;
	session *sess = gtk_object_get_data(GTK_OBJECT(widget), "sess");

	hx_part_chat(&sess->htlc, gchat->cid);
	gchat_delete(sess, gchat);
}


static void join_chat(GtkWidget *widget, gpointer data)
{
	GtkWidget *dialog = (GtkWidget *)gtk_object_get_data(GTK_OBJECT(widget), 
														 "dialog");
	struct htlc_conn *htlc = gtk_object_get_data(GTK_OBJECT(widget), "htlc");

	gtk_widget_destroy(dialog);
	hx_chat_join(htlc, GPOINTER_TO_INT(data));
}

void output_chat_subject(struct htlc_conn *htlc, guint32 cid, char *buf)
{
	session *sess = sess_from_htlc(htlc);
	struct gtkhx_chat *gchat = gchat_with_cid(sess, cid);

	if(!gchat)
		return;

	gtk_entry_set_text(GTK_ENTRY(gchat->subject), buf);
	hx_printf_prefix(htlc, cid, INFOPREFIX, "%s: %s", _("Subject Changed to"),
					 buf);
}

void hx_reject_chat(struct htlc_conn *htlc, guint32 _cid)
{
	guint32 cid = htonl(_cid);

	hlwrite(htlc, HTLC_HDR_CHAT_DECLINE, 0, 1,
			HTLC_DATA_CHAT_ID, 4, &cid);
}

void reject_chat(GtkWidget *btn, GtkWidget *dialog)
{
	guint32 cid = GPOINTER_TO_INT(gtk_object_get_data(GTK_OBJECT(btn), "cid"));

	hx_reject_chat(&sessions[0].htlc, cid);
	gtk_widget_destroy(dialog);
}

void output_chat_invitation(struct htlc_conn *htlc, guint32 cid, char *name)
{
	GtkWidget *dialog;
	GtkWidget *join;
	GtkWidget *cancel;
	GtkWidget *hbox;
	GtkWidget *label;
	char *message;

	dialog = gtk_dialog_new();
	gtk_window_set_title(GTK_WINDOW(dialog), _("Chat Invitation"));
	gtk_container_border_width (GTK_CONTAINER(dialog), 5);
	message = g_strdup_printf("%s %s: 0x%08x", name,
							  _("invites you to private chat"),  cid);

	label = gtk_label_new(message);
	join = gtk_button_new_with_label(_("Join"));
	gtk_object_set_data(GTK_OBJECT(join), "dialog", dialog);
	gtk_object_set_data(GTK_OBJECT(join), "htlc", htlc);
	gtk_signal_connect(GTK_OBJECT(join), "clicked", GTK_SIGNAL_FUNC(join_chat),
					   GINT_TO_POINTER(cid));


	cancel = gtk_button_new_with_label(_("Decline"));
	gtk_object_set_data(GTK_OBJECT(cancel), "cid", GINT_TO_POINTER(cid));
	gtk_signal_connect(GTK_OBJECT(cancel), "clicked", 
					   GTK_SIGNAL_FUNC(reject_chat), 
					   GTK_OBJECT(dialog));

	hbox = gtk_hbox_new(0,0);
	GTK_WIDGET_SET_FLAGS(join, GTK_CAN_DEFAULT);

	gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), label, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->action_area), hbox, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), join, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbox), cancel, 0, 0, 0);
	gtk_widget_grab_default(join);

	gtk_widget_show_all(dialog);
	g_free(message);
}

void pchat_update_trans (GtkWidget *win, GdkEventConfigure *event, 
						 gpointer data)
{
	GtkWidget *xtext = data;

	if(gtkhx_prefs.trans_xtext) {
		gtk_xtext_refresh(GTK_XTEXT(xtext), 1);
	}

}

struct gtkhx_chat *create_pchat_window (struct htlc_conn *htlc, 
										struct chat *chat)
{
	GtkWidget *vbox, *hbox;
	GtkWidget *outputframe, *inputframe, *userframe, *topframe;
	GtkWidget *pchat_hbox;
	GtkWidget *pchat_window;
	GtkWidget *subj_hbox;
	GtkWidget *subj_frame;
	GtkWidget *vpane;
	GtkWidget *hpane;
	GtkWidget *scroll;
	GtkWidget *user_vbox;
	GtkWidget *hbuttonbox;
	GtkWidget *msg_btn;
	GtkWidget *kick_btn;
	GtkWidget *ban_btn;
	GtkWidget *info_btn;
	GtkWidget *igno_btn;
	GtkWidget *chat_btn;
	GtkStyle *style;
	GdkBitmap *mask;
	GtkWidget *pix;
	GdkPixmap *icon;
	char *title;
	gchar *titles[2];
	session *sess = sess_from_htlc(htlc);
	struct gtkhx_chat *gchat = pchat_new(sess, chat);

	titles[0] = _("UID");
	titles[1] = _("Name");

	pchat_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_wmclass(GTK_WINDOW(pchat_window), "pchat", "GtkHx");
	gtk_widget_realize(pchat_window);

	style = gtk_widget_get_style(pchat_window);
	gtk_widget_set_usize(pchat_window, 700, 320);
	gtk_window_set_policy(GTK_WINDOW(pchat_window), 1, 1, 0);

	gtk_object_set_data(GTK_OBJECT(pchat_window), "sess", sess);
	gtk_signal_connect(GTK_OBJECT(pchat_window), "destroy",
					   GTK_SIGNAL_FUNC(pchat_close), gchat);
	title = g_strdup_printf("%s: 0x%08x", _("Private Chat"), chat->cid);
	gtk_window_set_title(GTK_WINDOW(pchat_window), title);
	gtk_container_set_border_width(GTK_CONTAINER(pchat_window), 0);

	vbox = gtk_vbox_new(0, 4);
	gtk_container_set_border_width(GTK_CONTAINER(vbox), 5);

	subj_hbox = gtk_hbox_new(0, 0);
	gtk_widget_set_usize(subj_hbox, (612<<6)/82, 20);
	subj_frame = gtk_frame_new(0);
	gtk_frame_set_shadow_type(GTK_FRAME(subj_frame), GTK_SHADOW_OUT);
	gtk_container_add(GTK_CONTAINER(subj_frame), subj_hbox);
	gchat->subject = gtk_entry_new();
	gtk_box_pack_start(GTK_BOX(subj_hbox), gchat->subject, 1, 1, 0);
	gtk_entry_set_text(GTK_ENTRY(gchat->subject), chat->subject);
	gtk_box_pack_start(GTK_BOX(vbox), subj_frame, 0, 1, 0);
	gtk_widget_set_style(gchat->subject, gtktext_style);
	gtk_signal_connect(GTK_OBJECT(gchat->subject), "activate", 
					   GTK_SIGNAL_FUNC(change_subject), 
					   GINT_TO_POINTER(chat->cid));

	outputframe = gtk_frame_new(0);
	gtk_frame_set_shadow_type(GTK_FRAME(outputframe), GTK_SHADOW_IN);

	inputframe = gtk_frame_new(0);
	gtk_frame_set_shadow_type(GTK_FRAME(inputframe), GTK_SHADOW_IN);

	vpane = gtk_vpaned_new();
	gtk_paned_pack1(GTK_PANED(vpane), outputframe, 0, 1);
	gtk_paned_pack2(GTK_PANED(vpane), inputframe, 0, 1);
	gtk_paned_set_position(GTK_PANED(vpane), 240);

	gtk_box_pack_start(GTK_BOX(vbox), vpane, 1, 1, 0);

	pchat_hbox = gtk_hbox_new(0, 0);
	gtk_widget_set_usize(pchat_hbox, 500, 400);

	gtk_box_pack_start(GTK_BOX(pchat_hbox), gchat->output, 1, 1, 0);
	gtk_box_pack_start(GTK_BOX(pchat_hbox), gchat->vscroll, 0, 0, 0);
	gtk_container_add(GTK_CONTAINER(outputframe), pchat_hbox);

	hbox = gtk_hbox_new(0, 0);
	gtk_widget_set_usize(hbox, 140, 50);
	gtk_container_add(GTK_CONTAINER(inputframe), hbox);

	gchat->input = gtk_text_new(0, 0);
	gtk_widget_set_style(gchat->input, gtktext_style);
	gtk_signal_connect(GTK_OBJECT(gchat->input), "key_press_event", 
					   GTK_SIGNAL_FUNC(chat_input_key_press), 0);
	gtk_object_set_data(GTK_OBJECT(gchat->input), "sess", sess);
	gtk_object_set_data(GTK_OBJECT(gchat->input), "gchat", gchat);
	gtk_text_set_editable(GTK_TEXT(gchat->input), 1);
	gtk_text_set_word_wrap(GTK_TEXT(gchat->input), 1);
	gtk_box_pack_start(GTK_BOX(hbox), gchat->input, 1, 1, 0);

 	gtk_signal_connect(GTK_OBJECT(pchat_window), "configure_event", 
					   GTK_SIGNAL_FUNC(pchat_update_trans), gchat->output);

	gchat->userlist = gtk_hlist_new_with_titles(2, titles);
	gtk_hlist_set_column_width(GTK_HLIST(gchat->userlist), 0, 35);
	gtk_hlist_set_column_width(GTK_HLIST(gchat->userlist), 1, 240);
	gtk_hlist_set_row_height(GTK_HLIST(gchat->userlist), 18);
	gtk_hlist_set_shadow_type(GTK_HLIST(gchat->userlist), GTK_SHADOW_NONE);
	gtk_hlist_set_column_justification(GTK_HLIST(gchat->userlist), 1, 
									   GTK_JUSTIFY_LEFT);
	gtk_signal_connect(GTK_OBJECT(gchat->userlist), "button_press_event",
			   GTK_SIGNAL_FUNC(user_clicked), 0);

	if (!users_style) {
		if (!users_font)
			users_font = gdk_font_load("-adobe-helvetica-normal-r-*-*-10"
									   "-140-*-*-*-*-*-*");
		if (users_font) {
			users_style = gtk_style_new();
			users_style->font = users_font;
		}
	}
	else {
		gtk_widget_set_style(gchat->userlist, users_style);
	}

	msg_btn = gtk_button_new();
	gtk_object_set_data(GTK_OBJECT(msg_btn), "sess", sess);
	icon = gdk_pixmap_create_from_xpm_d (pchat_window->window, &mask, 
										 &style->bg[GTK_STATE_NORMAL], msg_xpm);
	pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(msg_btn), pix);
	gtk_signal_connect(GTK_OBJECT(msg_btn), "clicked", 
					   GTK_SIGNAL_FUNC(open_message_btn), gchat->userlist);
	gtk_tooltips_set_tip(tooltips, msg_btn, _("Msg"), 0);
	icon = 0, pix = 0, mask = 0;

	kick_btn = gtk_button_new();
	gtk_object_set_data(GTK_OBJECT(kick_btn), "sess", sess);
	icon = gdk_pixmap_create_from_xpm_d (pchat_window->window, &mask, 
										 &style->bg[GTK_STATE_NORMAL], 
										 kick_xpm);
    pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(kick_btn), pix);
	gtk_signal_connect(GTK_OBJECT(kick_btn), "clicked", 
					   GTK_SIGNAL_FUNC(user_kick_btn), gchat->userlist);
	gtk_tooltips_set_tip(tooltips, kick_btn, _("Kick"), 0);
	icon = 0, pix = 0, mask = 0;

	info_btn = gtk_button_new();
	gtk_object_set_data(GTK_OBJECT(info_btn), "sess", sess);
	icon = gdk_pixmap_create_from_xpm_d (pchat_window->window, &mask,
										 &style->bg[GTK_STATE_NORMAL], 
										 info_xpm);
    pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(info_btn), pix);
	gtk_signal_connect(GTK_OBJECT(info_btn), "clicked", 
					   GTK_SIGNAL_FUNC(user_info_btn), gchat->userlist);
	gtk_tooltips_set_tip(tooltips, info_btn, _("User Info"), 0);
	icon = 0, pix = 0, mask = 0;

	ban_btn = gtk_button_new();
	gtk_object_set_data(GTK_OBJECT(ban_btn), "sess", sess);
	gtk_signal_connect(GTK_OBJECT(ban_btn), "clicked", 
					   GTK_SIGNAL_FUNC(user_ban_btn), gchat->userlist);
	icon = gdk_pixmap_create_from_xpm_d(pchat_window->window,
										&mask, 
										&style->bg[GTK_STATE_NORMAL], 
										ban_xpm);
	pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(ban_btn), pix);
	gtk_tooltips_set_tip(tooltips, ban_btn, _("Ban"), 0);
	icon = 0, pix = 0, mask = 0;

	chat_btn = gtk_button_new();
	gtk_object_set_data(GTK_OBJECT(chat_btn), "sess", sess);
	gtk_tooltips_set_tip(tooltips, chat_btn, _("Private Chat"), 0);
	gtk_signal_connect(GTK_OBJECT(chat_btn), "clicked", 
					   GTK_SIGNAL_FUNC(user_chat_btn), gchat->userlist);
	icon = gdk_pixmap_create_from_xpm_d (pchat_window->window, &mask, 
										 &style->bg[GTK_STATE_NORMAL], 
										 chat_xpm);
    pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(chat_btn), pix);
	icon = 0, pix = 0, mask = 0;

	igno_btn = gtk_button_new();
	gtk_object_set_data(GTK_OBJECT(igno_btn), "sess", sess);
	gtk_tooltips_set_tip(tooltips, igno_btn, _("Ignore"), 0);
	gtk_signal_connect(GTK_OBJECT(igno_btn), "clicked", 
					   GTK_SIGNAL_FUNC(user_igno_btn), gchat->userlist);
	icon = gdk_pixmap_create_from_xpm_d(pchat_window->window, &mask, 
										&style->bg[GTK_STATE_NORMAL], 
										ignore_xpm);
	pix = gtk_pixmap_new(icon, mask);
	gtk_container_add(GTK_CONTAINER(igno_btn), pix);

	topframe = gtk_frame_new(0);
	gtk_widget_set_usize(topframe, -2, 30);
	gtk_frame_set_shadow_type(GTK_FRAME(topframe), GTK_SHADOW_OUT);

	hbuttonbox = gtk_hbox_new(0,0);
	gtk_container_add(GTK_CONTAINER(topframe), hbuttonbox);

	gtk_box_pack_start(GTK_BOX(hbuttonbox), msg_btn, 0, 0, 0);

	gtk_box_pack_start(GTK_BOX(hbuttonbox), chat_btn, 0, 0, 2);
	gtk_box_pack_start(GTK_BOX(hbuttonbox), info_btn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbuttonbox), kick_btn, 0, 0, 2);
	gtk_box_pack_start(GTK_BOX(hbuttonbox), ban_btn, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(hbuttonbox), igno_btn, 0, 0, 0);


	user_vbox = gtk_vbox_new(0, 0);

	userframe = gtk_frame_new(0);
	gtk_frame_set_shadow_type(GTK_FRAME(userframe), GTK_SHADOW_IN);

	scroll = gtk_scrolled_window_new(0, 0);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
								   GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
	gtk_widget_set_usize(scroll, 232, 232);
	gtk_container_add(GTK_CONTAINER(scroll), gchat->userlist);
	gtk_container_add(GTK_CONTAINER(userframe), scroll);

	gtk_box_pack_start(GTK_BOX(user_vbox), topframe, 0, 0, 0);
	gtk_box_pack_start(GTK_BOX(user_vbox), userframe, 1, 1, 0);


	hpane = gtk_hpaned_new();
	gtk_paned_pack1(GTK_PANED(hpane), vbox, 0, 1);
	gtk_paned_pack2(GTK_PANED(hpane), user_vbox, 0, 1);
	gtk_paned_set_position(GTK_PANED(hpane), 435);

	gtk_container_add(GTK_CONTAINER(pchat_window), hpane);

	gtk_widget_show_all(pchat_window);
	init_keyaccel(pchat_window);


	gtk_widget_grab_focus(gchat->input);
	g_free(title);



	gchat->window = pchat_window;

	return gchat;
}

void hx_clear_chat(struct htlc_conn *htlc, guint32 cid, int subj)
{
	session *sess = sess_from_htlc(htlc);
	struct gtkhx_chat *gchat = gchat_with_cid(sess, cid);

	gtk_xtext_clear(GTK_XTEXT(gchat->output));
	if(gtkhx_prefs.geo.chat.open) {
		if(subj) {
			gtk_entry_set_text(GTK_ENTRY(gchat->subject), "");
		}
	}
}
