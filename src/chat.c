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
#include <stdlib.h>
#include <gtk/gtk.h>
#include <adwaita.h>
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
#include "gtkurl.h"
#include "plugin.h"
#include "tasks.h"
#include "rcv.h"
#include "connect.h"
#include "log.h"


static char *termed_buf = 0;
extern PangoFontDescription *gtkhx_font_desc;

#define WORD_URL     1
#define WORD_NICK    2
#define WORD_HOST    4
#define WORD_EMAIL   5

/*
 * Phase 2.6.B: 37-entry palette laid out for HexChat's xtext.
 *
 * Slot layout expected by GtkXText (see xtext.h):
 *   0..15   mIRC colors 0..15
 *   16..31  mIRC colors 16..31 (bold/extended; HexChat duplicates 0..15)
 *   32      XTEXT_MARK_FG   selection foreground
 *   33      XTEXT_MARK_BG   selection background
 *   34      XTEXT_FG        default text foreground
 *   35      XTEXT_BG        default text background
 *   36      XTEXT_MARKER    marker line color
 *
 * The previous (GTK 1.2 / XChat 1.8.5) widget only consulted slots 0..19,
 * with 16/17 = mark bg/fg and 18/19 = fg/bg.  HexChat's xtext reads
 * past slot 19 and was getting uninitialized memory, which is why the
 * default background looked weird.
 */
/* Phase 3.10: GdkColor (16-bit per channel + paletted-X11 pixel slot)
 * → GdkRGBA (4 doubles 0..1, no pixel slot). Each row is one color
 * literal preserved exactly via the RGB16 macro from compat.h, which
 * folds (channel/65535.0) at compile time. */
GdkRGBA colors[] =
{
	/* mIRC 0..15 */
	RGB16 (0,      0,      0     ), /* 0  black */
	RGB16 (0xcccc, 0xcccc, 0xcccc), /* 1  white */
	RGB16 (0,      0,      0xcccc), /* 2  blue */
	RGB16 (0,      0xcccc, 0     ), /* 3  green */
	RGB16 (0xcccc, 0,      0     ), /* 4  red */
	RGB16 (0xbbbb, 0xbbbb, 0     ), /* 5  yellow/brown */
	RGB16 (0xbbbb, 0,      0xbbbb), /* 6  purple */
	RGB16 (0xffff, 0xaaaa, 0     ), /* 7  orange */
	RGB16 (0xffff, 0xffff, 0     ), /* 8  yellow */
	RGB16 (0,      0xffff, 0     ), /* 9  green */
	RGB16 (0,      0xcccc, 0xcccc), /* 10 aqua */
	RGB16 (0,      0xffff, 0xffff), /* 11 light aqua */
	RGB16 (0,      0,      0xffff), /* 12 blue */
	RGB16 (0xffff, 0,      0xffff), /* 13 pink */
	RGB16 (0x7777, 0x7777, 0x7777), /* 14 grey */
	RGB16 (0x9999, 0x9999, 0x9999), /* 15 light grey */
	/* mIRC 16..31 — duplicate of 0..15 (HexChat does the same) */
	RGB16 (0,      0,      0     ), /* 16 black */
	RGB16 (0xcccc, 0xcccc, 0xcccc), /* 17 white */
	RGB16 (0,      0,      0xcccc), /* 18 blue */
	RGB16 (0,      0xcccc, 0     ), /* 19 green */
	RGB16 (0xcccc, 0,      0     ), /* 20 red */
	RGB16 (0xbbbb, 0xbbbb, 0     ), /* 21 yellow/brown */
	RGB16 (0xbbbb, 0,      0xbbbb), /* 22 purple */
	RGB16 (0xffff, 0xaaaa, 0     ), /* 23 orange */
	RGB16 (0xffff, 0xffff, 0     ), /* 24 yellow */
	RGB16 (0,      0xffff, 0     ), /* 25 green */
	RGB16 (0,      0xcccc, 0xcccc), /* 26 aqua */
	RGB16 (0,      0xffff, 0xffff), /* 27 light aqua */
	RGB16 (0,      0,      0xffff), /* 28 blue */
	RGB16 (0xffff, 0,      0xffff), /* 29 pink */
	RGB16 (0x7777, 0x7777, 0x7777), /* 30 grey */
	RGB16 (0x9999, 0x9999, 0x9999), /* 31 light grey */
	/* UI roles */
	RGB16 (0xeeee, 0xeeee, 0xeeee), /* 32 XTEXT_MARK_FG (light) */
	RGB16 (0x2020, 0x4a4a, 0x8787), /* 33 XTEXT_MARK_BG (blue) */
	RGB16 (0xcccc, 0xcccc, 0xcccc), /* 34 XTEXT_FG (light) */
	RGB16 (0,      0,      0     ), /* 35 XTEXT_BG (black) */
	RGB16 (0xcccc, 0,      0     ), /* 36 XTEXT_MARKER (red) */
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
	task_new(htlc, RCV_TASK_FN(hx_rcv_user_change), 0, 0, "chat");
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
	chat = chat_with_cid(&the_session, cid);


	if(chat) {
		return;
	}
	else {
		chat = chat_new(&the_session, cid);
		cid = htonl(cid);
		task_new(htlc, RCV_TASK_FN(rcv_task_user_list_switch), chat, 0, "join");
		hlwrite(htlc, HTLC_HDR_CHAT_JOIN, 0, 1, HTLC_DATA_CHAT_ID, 4, &cid);
	}
}

void hx_part_chat(struct htlc_conn *htlc, guint32 cid)
{
	struct chat *chat;

	chat = chat_with_cid(&the_session, cid);
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
	size_t i, len = strlen (word);
	int dots;

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
		if (inet_addr (word) != INADDR_NONE)
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

/* Phase 5: timecpy is gone. The "[HH:MM:SS] " inline-timestamp prefix
 * it produced is now drawn by xtext as a left-column stamp via
 * gtk_xtext_set_time_stamp on each buffer. xprintline / xoutput_chat
 * just append the bare message text; the per-entry timestamp is
 * auto-set in gtk_xtext_append_entry. */

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
	char  *valid;
	gsize  valid_len;

	if((ssize_t)len == -1) {
		len = strlen(chat);
	}
	if(len == 0) {
		len = 1;
	}

	/* Phase 5: chat / msg bytes from the wire arrive in whatever
	 * encoding the server happened to use — historically Mac Roman
	 * on Mac-OS-classic servers, occasionally Latin-1 from later
	 * Unix forks, sometimes already UTF-8 on modern stacks. xtext
	 * eventually hands the bytes to Pango, which asserts UTF-8 and
	 * emits "Invalid UTF-8 string passed to pango_layout_set_text()"
	 * for any 8-bit content. gtkhx_text_to_utf8 walks the
	 * already-UTF-8 / Mac-Roman / fallback-to-substitute cascade and
	 * always returns a valid-UTF-8 g_strdup'd copy. */
	valid = gtkhx_text_to_utf8 (chat, len, &valid_len);
	if (!valid) {
		/* Defensive — gtkhx_text_to_utf8 should never return NULL,
		 * but if it does we'd rather drop the line than crash. */
		return;
	}

	/* Phase 5: timestamps move from inline "[HH:MM:SS] " prefix into
	 * xtext's native left-column stamp. Two reasons:
	 *
	 *   1. HexChat-style drag-select: stamps are visually separate
	 *      from the message body, so a drag-select on a chat line
	 *      doesn't accidentally include the time. Settings →
	 *      "Automatically include timestamps" toggles whether the
	 *      stamp gets prepended on copy.
	 *   2. No double-stamp duplication on copy. With the inline
	 *      prefix, the autocopy_stamp toggle would yield
	 *      "HH:MM:SS [HH:MM:SS] message" because xtext was
	 *      prepending its own stamp on top of our inline one.
	 *
	 * xtext renders the per-entry stamp (ent->stamp, auto-set in
	 * gtk_xtext_append_entry) iff xtext->auto_indent &&
	 * buf->time_stamp. Both are flipped on per-buffer at creation
	 * time in chat.c / msg.c, and re-applied to live buffers when
	 * the user toggles CFG_TIMESTAMP via Settings. */

	/* Phase 5+: HexChat-style nick column. Lines that look like
	 * "  name:  body" get rewritten into a left part ("<name>",
	 * including the brackets) and a right part ("body"); xtext's
	 * gtk_xtext_append_indent draws the left part flush-right
	 * against the indent column and the right part flush-left
	 * after it, so all nicks align visually and the bodies start
	 * at a common left edge. Info messages produced by
	 * hx_printf_prefix (recognisable by the leading INFOPREFIX
	 * with its mIRC-coloured "[hx]") get a "[hx]" nick so they
	 * slot into the same column.
	 *
	 * Lines that don't match either pattern (emotes, raw output
	 * without a colon) fall through to plain append. */
	gsize name_off = 0, name_len = 0;
	gsize body_off = 0, body_len = 0;
	gchar *display_nick = NULL;
	const char *display_body = NULL;
	gsize display_body_len = 0;
	{
		const char *info_prefix = INFOPREFIX;
		gsize info_prefix_len = info_prefix ? strlen (info_prefix) : 0;

		if (info_prefix_len > 0
		    && valid_len >= info_prefix_len
		    && memcmp (valid, info_prefix, info_prefix_len) == 0) {
			/* Preserve the colour codes around "[hx]" so the
			 * info-prefix renders the same hue it always did,
			 * just in the nick column now. */
			display_nick = g_strndup (valid + 1,
			                          info_prefix_len - 2);
			display_body = valid + info_prefix_len;
			display_body_len = valid_len - info_prefix_len;
		} else if (hx_chat_split_nick_body (valid, valid_len,
		                                    &name_off, &name_len,
		                                    &body_off, &body_len)) {
			display_nick = g_strdup_printf ("<%.*s>",
			                                (int) name_len,
			                                valid + name_off);
			display_body = valid + body_off;
			display_body_len = body_len;
		}
	}

	if (display_nick) {
		gtk_xtext_append_indent (GTK_XTEXT (text)->buffer,
		                         (unsigned char *) display_nick,
		                         strlen (display_nick),
		                         (unsigned char *) display_body,
		                         display_body_len,
		                         0);
		g_free (display_nick);
	} else {
		gtk_xtext_append (GTK_XTEXT (text)->buffer,
		                  (unsigned char *) valid, valid_len, 0);
	}

	g_free (valid);
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
				char *new_copy = g_strdup_printf(" [hx] %s", &copy[18]);
				g_free(copy);
				copy = new_copy;
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
	session *sess = &the_session;

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
	session *sess = &the_session;

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
	int cursor_pos = -1;
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

	g_snprintf(not_nick_chars, sizeof(not_nick_chars), " .?%c", ':');

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

		/* len is size_t (unsigned); j is int. Compare directly to avoid
		 * the underflow trap an 'len - j < 0' check would walk into. */
		if (j < 0 || (size_t) j > len)
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
				/* g_slist_remove returns the (possibly new) list head;
				 * dropping it leaks the change for any case where
				 * node2 was the head node, AND triggers
				 * -Wunused-result on the warn_unused_result
				 * attribute. Capture and re-seat. */
				match_list = g_slist_remove(match_list, node2->data);
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


			if (match_pos == (size_t) -1)
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
		size_t nb_off = 0;
		while (match_list) {
			int n;
			nick = ((struct hx_user *)match_list->data)->name;
			n = snprintf (nick_buf + nb_off,
			              sizeof (nick_buf) - nb_off,
			              "%s ", nick);
			if (n < 0 || (size_t) n >= sizeof (nick_buf) - nb_off)
				break;
			nb_off += (size_t) n;
			match_list = g_slist_next (match_list);
		}
		hx_printf(&sess->htlc, 0, "%s", nick_buf);
		if (first) {
			snprintf (buf, sizeof (buf), "%s", match_text);
		}
		else {
			snprintf (buf, sizeof (buf), "%s %s%s", b4, match_text, c5);
			cursor_pos = strlen (b4) + strlen (match_text);
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
			cursor_pos = strlen (b4) + strlen(match_user->name);
			if(b4)
				g_free (b4);
			if(c5)
				g_free (c5);
		}
	}

	ptr = buf;
	while(*ptr == ' ') ptr++;

	{
		GtkTextBuffer *ebuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(entry));
		gtk_text_buffer_set_text(ebuf, ptr, -1);
		if (cursor_pos >= 0) {
			GtkTextIter cursor_iter;
			int total = gtk_text_buffer_get_char_count(ebuf);
			if (cursor_pos > total) cursor_pos = total;
			gtk_text_buffer_get_iter_at_offset(ebuf, &cursor_iter, cursor_pos);
			gtk_text_buffer_place_cursor(ebuf, &cursor_iter);
		}
	}

	return 0;
}

/* Phase 4.5: GTK 4 widgets don't fire key-press-event. The chat input's
 * Tab nick completion + Return-to-send + Up/Down history is the most
 * complex key handler in the codebase per ROADMAP. It now hangs off a
 * GtkEventControllerKey installed on the chat input view; the
 * "key-pressed" signal carries (controller, keyval, keycode, state).
 *
 * Returning TRUE inhibits further propagation (so the GtkTextView's
 * default text input doesn't insert the Return / Tab / Up / Down).
 * FALSE lets the default proceed (used for Shift+Return → newline and
 * for ordinary printable characters).
 *
 * The session/gchat pointers come from g_object_set_data on the
 * widget, set at chat-window construction time; the helper retrieves
 * them via the controller's widget lookup. */
static gboolean
chat_input_key_pressed (GtkEventControllerKey *ctrl, guint keyval,
                        guint keycode, GdkModifierType state, gpointer user_data)
{
	GtkWidget *widget = gtk_event_controller_get_widget (
		GTK_EVENT_CONTROLLER (ctrl));
	GtkTextView *text;
	GtkTextBuffer *buf;
	GtkTextMark *insert_mark;
	GtkTextIter insert_iter;
	guint point;
	HIST_ENTRY *hent = NULL;
	struct gtkhx_chat *gchat = g_object_get_data (G_OBJECT (widget), "gchat");
	session *sess = g_object_get_data (G_OBJECT (widget), "sess");
	(void) keycode; (void) user_data;

	if (!gchat || !sess)
		return FALSE;

	text = GTK_TEXT_VIEW (widget);
	buf  = gtk_text_view_get_buffer (text);

	insert_mark = gtk_text_buffer_get_insert (buf);
	gtk_text_buffer_get_iter_at_mark (buf, &insert_iter, insert_mark);
	point = gtk_text_iter_get_offset (&insert_iter);

	if (state & GDK_CONTROL_MASK) {
		switch (keyval) {
		case 'k':
		case 'K':
			create_connect_window (0, &the_session);
			return TRUE;
		}
	}
	else if ((state & GDK_SHIFT_MASK) && keyval == GDK_KEY_Return) {
		/* Insert a linebreak if shift is held — let GtkTextView default. */
		return FALSE;
	}
	else if (keyval == GDK_KEY_Return) {
		GtkTextIter start, end;

		gtk_text_view_set_editable (text, FALSE);
		g_free (termed_buf);

		gtk_text_buffer_get_start_iter (buf, &start);
		gtk_text_buffer_get_end_iter   (buf, &end);
		termed_buf = gtk_text_buffer_get_text (buf, &start, &end, FALSE);

		add_history  (gchat->chat_history, termed_buf);
		using_history (gchat->chat_history);

		hotline_client_input (&sess->htlc, termed_buf, gchat->cid,
		                      (state & GDK_CONTROL_MASK) ? 1 : 0);

		gtk_text_buffer_get_start_iter (buf, &start);
		gtk_text_buffer_get_end_iter   (buf, &end);
		gtk_text_buffer_delete (buf, &start, &end);
		gtk_text_view_set_editable (text, TRUE);
		return TRUE;
	}
	else if (keyval == GDK_KEY_Tab) {
		GtkTextIter start, end;
		char *p;

		gtk_text_buffer_get_start_iter (buf, &start);
		gtk_text_buffer_get_end_iter   (buf, &end);
		p = gtk_text_buffer_get_text (buf, &start, &end, FALSE);
		tab_nick_comp (sess, p, 1, point, widget);
		g_free (p);
		gtk_widget_grab_focus (GTK_WIDGET (text));
		return TRUE;
	}
	else if (keyval == GDK_KEY_Up) {
		hent = previous_history (gchat->chat_history);
	}
	else if (keyval == GDK_KEY_Down) {
		hent = next_history (gchat->chat_history);
	}

	if (hent) {
		GtkTextIter end;

		gtk_text_buffer_set_text (buf, hent->line, strlen (hent->line));
		gtk_text_buffer_get_end_iter (buf, &end);
		gtk_text_buffer_place_cursor (buf, &end);
		return TRUE;
	}

	return FALSE;
}

/* Phase 4.5: configure-event is gone in GTK 4. Window size for the
 * chat window is captured at hx_quit() in gtkhx.c gtkhx_save_window_positions
 * alongside position. */

static GtkWidget *chat_hbox;
static GtkWidget *wind_tmp;

static void chat_close (GtkWidget *widget, gpointer data)
{
	GtkWidget *hbox = chat_hbox;
	struct gtkhx_chat *gchat = data;

	wind_tmp = gtk_window_new();

	gtkhx_widget_remove_child(gtk_widget_get_parent(hbox), hbox);
	gtkhx_widget_set_child(wind_tmp, hbox);
	/* Phase 4.5: dropped GTK 1.2/2-era gtk_widget_realize. */
	gchat->input = 0;
	gchat->subject = 0;

	gtkhx_prefs.geo.chat.open = 0;
	gtkhx_prefs.geo.chat.init = 0;
}

void generate_colors(GtkWidget *widget)
{
	(void) widget;
	/* Phase 3.10: nothing to do — the colors[] palette is GdkRGBA now,
	 * which has no .pixel field. The function is kept as a stub for
	 * the existing caller in fe_init() and could be deleted as a
	 * follow-up. */
}


void create_chat(session *sess)
{
	struct gtkhx_chat *gchat;
	GtkWidget *text;
	GtkWidget *vscroll;

	gchat = g_malloc(sizeof(struct gtkhx_chat));

	{
		gchar *fontname = pango_font_description_to_string (gtkhx_font_desc);
		text = gtk_xtext_new (colors, 0);
		gtk_xtext_set_font (GTK_XTEXT (text), fontname);
		g_free (fontname);
	}
	gtk_widget_set_can_focus(text, FALSE);
	GTK_XTEXT(text)->wordwrap = gtkhx_prefs.word_wrap;
	GTK_XTEXT(text)->urlcheck_function = word_check;
	GTK_XTEXT(text)->max_lines = gtkhx_prefs.xbuf_max;
	/* Phase 5: enable xtext's left-column timestamp rendering. The
	 * stamp draws iff auto_indent && buf->time_stamp; the latter is
	 * flipped from CFG_TIMESTAMP / gtkhx_prefs.timestamp. See xprintline
	 * for the rationale (HexChat-style stamps separate from message
	 * text, no double-stamping on autocopy_stamp). */
	gtk_xtext_set_indent (GTK_XTEXT (text), TRUE);
	gtk_xtext_set_time_stamp (GTK_XTEXT (text)->buffer,
	                          gtkhx_prefs.timestamp);
	g_signal_connect (text, "word_click",
	                  G_CALLBACK (gtkurl_xtext_word_click), NULL);

	vscroll = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, GTK_XTEXT(text)->adj);

	g_object_ref_sink(text);
	g_object_ref_sink(vscroll);

	chat_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	/* Phase 5: dropped GTK 1.2-era set_size_request derived from saved
	 * xsize/ysize. set_size_request sets BOTH minimum and natural
	 * size in GTK 4 — so saving a wide chat window then re-opening it
	 * baked the previous width in as a hard floor that prevented the
	 * user from shrinking it. With hexpand/vexpand+FILL on the inner
	 * widgets (handled by gtkhx_box_pack with expand=fill=1) the
	 * window now resizes freely down to chat_window's own
	 * set_size_request floor. */

	g_object_ref_sink(chat_hbox);
	gtkhx_box_pack(chat_hbox, text, 1, 1, 0);
	gtkhx_box_pack(chat_hbox, vscroll, 0, 0, 0);

	wind_tmp = gtk_window_new();
	gtkhx_widget_set_child(wind_tmp, chat_hbox);
	/* Phase 4.5: dropped explicit gtk_widget_realize(text). Forcing
	 * realize before the toplevel maps was a GTK 1.2/2 idiom; under
	 * GTK 4 widgets are windowless and realize automatically once
	 * their root widget is shown. The early-realize call here was
	 * the trigger for a gtk_css_node_insert_after critical at
	 * startup. */

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

static void change_subject(GtkWidget *widget, gpointer data)
{
	const char *subject;

	subject = gtk_editable_get_text(GTK_EDITABLE(widget));
	hx_change_subject(&the_session.htlc, GPOINTER_TO_INT(data), (char *) subject);
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
		gtk_window_present(GTK_WINDOW(sess->chat_window));
		return;
	}

	gchat = gchat_with_cid(sess, 0);
	chat_window = gtk_window_new();
	/* Phase 5: AdwHeaderBar replaces the default GtkWindow title bar
	 * for the unified Adwaita look across all GtkHx windows. */
	gtk_window_set_titlebar(GTK_WINDOW(chat_window), adw_header_bar_new());

	gtk_widget_set_size_request(chat_window, 412, 280);
	gtk_window_set_resizable(GTK_WINDOW(chat_window), TRUE);

	g_signal_connect(chat_window, "destroy",
					   G_CALLBACK(chat_close), gchat);
	gtk_window_set_title(GTK_WINDOW(chat_window), _("Chat"));
	(gtk_widget_set_margin_start(chat_window, 0), gtk_widget_set_margin_end(chat_window, 0), gtk_widget_set_margin_top(chat_window, 0), gtk_widget_set_margin_bottom(chat_window, 0));

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	(gtk_widget_set_margin_start(vbox, 5), gtk_widget_set_margin_end(vbox, 5), gtk_widget_set_margin_top(vbox, 5), gtk_widget_set_margin_bottom(vbox, 5));

	gchat->subject = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(gchat->subject), sess->chat_front->subject);
	subj_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	subj_frame = gtk_frame_new(0);
	gtkhx_widget_set_child(subj_frame, subj_hbox);
	gtkhx_box_pack(subj_hbox, gchat->subject, 1, 1, 0);
	gtkhx_box_pack(vbox, subj_frame, 0, 1, 0);
	gtkhx_apply_text_style(gchat->subject);
	g_signal_connect(gchat->subject, "activate",
					   G_CALLBACK(change_subject), GINT_TO_POINTER(0));

	outputframe = gtk_frame_new(0);
	inputframe = gtk_frame_new(0);

	/* Phase 5+: replace the vertical GtkPaned with a plain box. The
	 * paned'"'"'s draggable divider was nice but it pinned the input area
	 * to a fixed height (50px minimum, user-resizable via drag),
	 * which meant a single-line input took two visible lines'"'"' worth of
	 * space and a multi-line paste required scrolling inside a
	 * fixed-height widget. The new layout has the output frame
	 * vexpand=TRUE eating remaining vertical space, and the input
	 * scrolled window using min/max-content-height + propagate-
	 * natural-height to grow with the buffer'"'"'s line count (capped at
	 * 5 lines; scrolls beyond that). */
	GtkWidget *vstack = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_margin_start (vstack, 5);
	gtk_widget_set_margin_end   (vstack, 5);
	gtk_widget_set_margin_top   (vstack, 5);
	gtk_widget_set_margin_bottom(vstack, 5);
	gtk_widget_set_vexpand (outputframe, TRUE);
	gtk_widget_set_vexpand (inputframe,  FALSE);
	gtk_box_append (GTK_BOX(vstack), outputframe);
	gtk_box_append (GTK_BOX(vstack), inputframe);
	(void) vpaned;	/* declared above but no longer used */

	gtkhx_widget_set_child(chat_window, vbox);
	gtkhx_box_pack(vbox, vstack, 1, 1, 0);

	if(wind_tmp) {
		gtkhx_widget_remove_child(wind_tmp, chat_hbox);
		gtkhx_widget_destroy(wind_tmp);
	}


	gtkhx_widget_set_child(outputframe, chat_hbox);
	/* Phase 4.5: dropped GTK 1.2/2-era gtk_widget_realize. */

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtkhx_widget_set_child(inputframe, hbox);

	gchat->input = gtk_text_view_new();
	gtkhx_apply_text_style(gchat->input);
	g_object_set_data(G_OBJECT(gchat->input), "gchat", gchat);
	g_object_set_data(G_OBJECT(gchat->input), "sess", sess);
	{
		/* Phase 4.5: key-press-event is gone — install the chat-input
		 * Tab/Return/history controller. Object-data stash above is
		 * what chat_input_key_pressed reads to find sess + gchat. */
		GtkEventController *kctrl = gtk_event_controller_key_new ();
		g_signal_connect (kctrl, "key-pressed",
		                  G_CALLBACK (chat_input_key_pressed), NULL);
		gtk_widget_add_controller (gchat->input, kctrl);
	}
	gtk_text_view_set_editable(GTK_TEXT_VIEW(gchat->input), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(gchat->input), GTK_WRAP_WORD);


	{
		GtkWidget *input_scroll = gtk_scrolled_window_new();
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(input_scroll),
		                               GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
		/* Auto-grow input behaviour: the scrolled window reports a
		 * natural height that matches the embedded GtkTextView'"'"'s
		 * content, clamped between a single-line minimum and a
		 * 5-line maximum. Below the min the input gets the floor;
		 * above the max the scrollbar takes over. The font size
		 * varies per theme so we pick generous pixel approximations
		 * (28px ≈ 1 line of body text, 120px ≈ 5 lines). */
		gtk_scrolled_window_set_propagate_natural_height(
			GTK_SCROLLED_WINDOW(input_scroll), TRUE);
		gtk_scrolled_window_set_min_content_height(
			GTK_SCROLLED_WINDOW(input_scroll), 28);
		gtk_scrolled_window_set_max_content_height(
			GTK_SCROLLED_WINDOW(input_scroll), 120);
		gtkhx_widget_set_child(input_scroll, gchat->input);
		gtkhx_box_pack(hbox, input_scroll, 1, 1, 0);
	}
	

	g_object_set_data(G_OBJECT(chat_window), "sess", sess);

	/* Phase 3.x: only apply saved geometry when the prefs file actually
	 * has one (see users.c for rationale — zero-size collapses the
	 * window under GTK 3). The earlier set_size_request(412, 280)
	 * call serves as the default. */
	if (gtkhx_prefs.geo.chat.xpos > 0 || gtkhx_prefs.geo.chat.ypos > 0)
		/* Phase 4.2: gtk_window_move removed (Wayland) */
	if (gtkhx_prefs.geo.chat.xsize > 0 && gtkhx_prefs.geo.chat.ysize > 0)
		gtk_window_set_default_size(GTK_WINDOW(chat_window),
		                            gtkhx_prefs.geo.chat.xsize,
		                            gtkhx_prefs.geo.chat.ysize);

	gtk_window_present(GTK_WINDOW(chat_window));
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

	{
		gchar *fontname = pango_font_description_to_string (gtkhx_font_desc);
		text = gtk_xtext_new (colors, 0);
		gtk_xtext_set_font (GTK_XTEXT (text), fontname);
		g_free (fontname);
	}
	GTK_XTEXT(text)->wordwrap = gtkhx_prefs.word_wrap;
	GTK_XTEXT(text)->urlcheck_function = word_check;
	GTK_XTEXT(text)->max_lines = gtkhx_prefs.xbuf_max;
	/* Phase 5: native xtext timestamps — see the matching call in
	 * create_chat_window above for the rationale. */
	gtk_xtext_set_indent (GTK_XTEXT (text), TRUE);
	gtk_xtext_set_time_stamp (GTK_XTEXT (text)->buffer,
	                          gtkhx_prefs.timestamp);
	g_signal_connect (text, "word_click",
	                  G_CALLBACK (gtkurl_xtext_word_click), NULL);

	vscroll = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, GTK_XTEXT(text)->adj);

	subject = gtk_entry_new();
	gtkhx_apply_text_style(subject);

	userlist = gtk_hlist_new(2);
	gtk_hlist_set_column_width(GTK_HLIST(userlist), 0, 30);
	gtk_hlist_set_column_width(GTK_HLIST(userlist), 1, 210);
	gtk_hlist_set_row_height(GTK_HLIST(userlist), 18);
	gtk_hlist_set_shadow_type(GTK_HLIST(userlist), GTK_SHADOW_NONE);
	/* Phase 5: Mac-classic overlay layout for the pchat name column —
	 * wide icons render as row background, name overlays at fixed
	 * 22-px offset (clears typical 18-px stock icons with breathing
	 * room). See users.c create_users_window for rationale. */
	gtk_hlist_column_set_overlay_pixtext (GTK_HLIST (userlist), 1, 22);
	gtk_hlist_set_column_justification(GTK_HLIST(userlist), 0, 
									   GTK_JUSTIFY_LEFT);
	g_object_ref_sink(text);
	g_object_ref_sink(vscroll);
	g_object_ref_sink(subject);
	g_object_ref_sink(userlist);

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
	session *sess = g_object_get_data(G_OBJECT(widget), "sess");

	hx_part_chat(&sess->htlc, gchat->cid);
	gchat_delete(sess, gchat);
}


/* Forward decl — hx_reject_chat is defined further down (in the
 * "subject change" cluster) but the AdwAlertDialog response handler
 * needs to see it. */
void hx_reject_chat (struct htlc_conn *htlc, guint32 _cid);

/* Phase 5: Phase 4 invitation flow used qdata on the Join button to
 * thread state into the click handler; AdwAlertDialog dispatches by
 * response id, so we carry the htlc + cid pair through the response
 * signal as a small heap-allocated context. The dialog's "closed"
 * signal frees it after the response handler runs. */
struct chat_invite_ctx {
	struct htlc_conn *htlc;
	guint32           cid;
};

static void
chat_invite_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
	struct chat_invite_ctx *ctx = data;
	(void) dialog;

	if (g_strcmp0 (response, "join") == 0)
		hx_chat_join (ctx->htlc, ctx->cid);
	else
		hx_reject_chat (ctx->htlc, ctx->cid);
}

static void
chat_invite_closed (AdwDialog *dialog, gpointer data)
{
	(void) dialog;
	g_free (data);
}

void output_chat_subject(struct htlc_conn *htlc, guint32 cid, char *buf)
{
	session *sess = &the_session;
	struct gtkhx_chat *gchat = gchat_with_cid(sess, cid);

	if(!gchat)
		return;

	gtk_editable_set_text(GTK_EDITABLE(gchat->subject), buf);
	hx_printf_prefix(htlc, cid, INFOPREFIX, "%s: %s", _("Subject Changed to"),
					 buf);
}

void hx_reject_chat(struct htlc_conn *htlc, guint32 _cid)
{
	guint32 cid = htonl(_cid);

	hlwrite(htlc, HTLC_HDR_CHAT_DECLINE, 0, 1,
			HTLC_DATA_CHAT_ID, 4, &cid);
}

/* Phase 5: AdwAlertDialog with two responses (Join / Decline) replaces
 * the hand-rolled GtkDialog + label + two buttons. Decline (and ESC)
 * declines the invite via hx_reject_chat; Join calls hx_chat_join.
 * Both go through the same response handler: the response id keys
 * the action. */
void output_chat_invitation(struct htlc_conn *htlc, guint32 cid, char *name)
{
	AdwDialog *dialog;
	struct chat_invite_ctx *ctx;
	char *body;

	body = g_strdup_printf ("%s %s: 0x%08x",
	                        name, _("invites you to private chat"), cid);

	dialog = adw_alert_dialog_new (_("Chat Invitation"), body);
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
	                               "decline", _("_Decline"));
	adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog),
	                               "join",    _("_Join"));
	adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog),
	                                          "join",
	                                          ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog),
	                                       "join");
	adw_alert_dialog_set_close_response   (ADW_ALERT_DIALOG (dialog),
	                                       "decline");

	ctx = g_new (struct chat_invite_ctx, 1);
	ctx->htlc = htlc;
	ctx->cid  = cid;
	g_signal_connect (dialog, "response",
	                  G_CALLBACK (chat_invite_response), ctx);
	g_signal_connect (dialog, "closed",
	                  G_CALLBACK (chat_invite_closed), ctx);

	adw_dialog_present (dialog, GTK_WIDGET (the_session.chat_window));
	g_free (body);
}

/* Phase 4.5: pchat_update_trans was a configure-event handler that
 * forced an xtext refresh on every resize so transparency would track
 * the new window position. configure-event is gone in GTK 4 and
 * Wayland doesn't expose true window-relative transparency anyway. */

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
	GtkWidget *pix;
	GdkPixmap *icon;
	char *title;
	gchar *titles[2];
	session *sess = &the_session;
	struct gtkhx_chat *gchat = pchat_new(sess, chat);

	titles[0] = _("UID");
	titles[1] = _("Name");

	pchat_window = gtk_window_new();
	/* Phase 5: AdwHeaderBar across all GtkHx windows for visual
	 * consistency. */
	gtk_window_set_titlebar(GTK_WINDOW(pchat_window), adw_header_bar_new());
	/* Phase 3.x: dropped GTK 1.2-era realize+get_style pair (style unused). */

	/* Phase 5: same fix as create_msgwin — set_size_request sets
	 * BOTH minimum and natural size in GTK 4, which combined with
	 * the inner widgets' size_requests below was forcing the
	 * window to come up at the natural-size of the layout (often
	 * larger than the screen, with the chat output clipped to the
	 * paned's allocated band). Use set_default_size for the
	 * initial size and let the user shrink as far as the inner
	 * minimums allow. */
	gtk_window_set_default_size(GTK_WINDOW(pchat_window), 720, 440);
	gtk_window_set_resizable(GTK_WINDOW(pchat_window), TRUE);

	g_object_set_data(G_OBJECT(pchat_window), "sess", sess);
	g_signal_connect(pchat_window, "destroy",
					   G_CALLBACK(pchat_close), gchat);
	title = g_strdup_printf("%s: 0x%08x", _("Private Chat"), chat->cid);
	gtk_window_set_title(GTK_WINDOW(pchat_window), title);
	(gtk_widget_set_margin_start(pchat_window, 0), gtk_widget_set_margin_end(pchat_window, 0), gtk_widget_set_margin_top(pchat_window, 0), gtk_widget_set_margin_bottom(pchat_window, 0));

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	(gtk_widget_set_margin_start(vbox, 5), gtk_widget_set_margin_end(vbox, 5), gtk_widget_set_margin_top(vbox, 5), gtk_widget_set_margin_bottom(vbox, 5));

	subj_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	subj_frame = gtk_frame_new(0);
	gtkhx_widget_set_child(subj_frame, subj_hbox);
	gchat->subject = gtk_entry_new();
	gtkhx_box_pack(subj_hbox, gchat->subject, 1, 1, 0);
	gtk_editable_set_text(GTK_EDITABLE(gchat->subject), chat->subject);
	gtkhx_box_pack(vbox, subj_frame, 0, 1, 0);
	gtkhx_apply_text_style(gchat->subject);
	g_signal_connect(gchat->subject, "activate", 
					   G_CALLBACK(change_subject), 
					   GINT_TO_POINTER(chat->cid));

	outputframe = gtk_frame_new(0);

	inputframe = gtk_frame_new(0);

	/* Phase 5+: drop GtkPaned in favour of a plain box so the input
	 * area can shrink to a single line by default and auto-grow up
	 * to a 5-line cap as the user types. See the matching note in
	 * create_chat_window above for the rationale. */
	{
		GtkWidget *vstack = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
		gtk_widget_set_vexpand (outputframe, TRUE);
		gtk_widget_set_vexpand (inputframe,  FALSE);
		gtk_box_append (GTK_BOX(vstack), outputframe);
		gtk_box_append (GTK_BOX(vstack), inputframe);
		gtkhx_box_pack(vbox, vstack, 1, 1, 0);
	}
	(void) vpane;

	pchat_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

	gtkhx_box_pack(pchat_hbox, gchat->output, 1, 1, 0);
	gtkhx_box_pack(pchat_hbox, gchat->vscroll, 0, 0, 0);
	gtkhx_widget_set_child(outputframe, pchat_hbox);

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtkhx_widget_set_child(inputframe, hbox);

	gchat->input = gtk_text_view_new();
	gtkhx_apply_text_style(gchat->input);
	g_object_set_data(G_OBJECT(gchat->input), "sess", sess);
	g_object_set_data(G_OBJECT(gchat->input), "gchat", gchat);
	{
		/* Phase 4.5: pchat input — same controller as the main chat. */
		GtkEventController *kctrl = gtk_event_controller_key_new ();
		g_signal_connect (kctrl, "key-pressed",
		                  G_CALLBACK (chat_input_key_pressed), NULL);
		gtk_widget_add_controller (gchat->input, kctrl);
	}
	gtk_text_view_set_editable(GTK_TEXT_VIEW(gchat->input), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(gchat->input), GTK_WRAP_WORD);
	{
		GtkWidget *pchat_input_scroll = gtk_scrolled_window_new();
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(pchat_input_scroll),
		                               GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
		gtk_scrolled_window_set_propagate_natural_height(
			GTK_SCROLLED_WINDOW(pchat_input_scroll), TRUE);
		gtk_scrolled_window_set_min_content_height(
			GTK_SCROLLED_WINDOW(pchat_input_scroll), 28);
		gtk_scrolled_window_set_max_content_height(
			GTK_SCROLLED_WINDOW(pchat_input_scroll), 120);
		gtkhx_widget_set_child(pchat_input_scroll, gchat->input);
		gtkhx_box_pack(hbox, pchat_input_scroll, 1, 1, 0);
	}

 	

	gchat->userlist = gtk_hlist_new_with_titles(2, titles);
	gtk_hlist_set_column_width(GTK_HLIST(gchat->userlist), 0, 35);
	gtk_hlist_set_column_width(GTK_HLIST(gchat->userlist), 1, 240);
	gtk_hlist_set_row_height(GTK_HLIST(gchat->userlist), 18);
	gtk_hlist_set_shadow_type(GTK_HLIST(gchat->userlist), GTK_SHADOW_NONE);
	gtk_hlist_set_column_justification(GTK_HLIST(gchat->userlist), 1, 
									   GTK_JUSTIFY_LEFT);
	/* Phase 4.5: button-press-event is gone in GTK 4. Install the same
	 * gesture controller the standalone Users window uses. The previous
	 * GTK-3 path passed user_clicked the chat-userlist with data=0, which
	 * meant the right-click menu would deref a NULL session — fixed in
	 * passing here by handing it the live session. */
	users_attach_click_gesture(gchat->userlist, sess);

	if (!users_font_desc)
		users_font_desc = pango_font_description_from_string ("Sans 10");
	gtkhx_refresh_userlist_css (users_font_desc);
	gtkhx_apply_userlist_style (gchat->userlist);

	msg_btn = gtk_button_new();
	g_object_set_data(G_OBJECT(msg_btn), "sess", sess);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/msg.xpm", NULL);
	pix = gtkhx_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(msg_btn, pix);
	g_signal_connect(msg_btn, "clicked", 
					   G_CALLBACK(open_message_btn), gchat->userlist);
	gtk_widget_set_tooltip_text(msg_btn, _("Msg"));
	icon = 0, pix = 0;

	kick_btn = gtk_button_new();
	g_object_set_data(G_OBJECT(kick_btn), "sess", sess);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/kick.xpm", NULL);
    pix = gtkhx_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(kick_btn, pix);
	g_signal_connect(kick_btn, "clicked", 
					   G_CALLBACK(user_kick_btn), gchat->userlist);
	gtk_widget_set_tooltip_text(kick_btn, _("Kick"));
	icon = 0, pix = 0;

	info_btn = gtk_button_new();
	g_object_set_data(G_OBJECT(info_btn), "sess", sess);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/info.xpm", NULL);
    pix = gtkhx_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(info_btn, pix);
	g_signal_connect(info_btn, "clicked", 
					   G_CALLBACK(user_info_btn), gchat->userlist);
	gtk_widget_set_tooltip_text(info_btn, _("User Info"));
	icon = 0, pix = 0;

	ban_btn = gtk_button_new();
	g_object_set_data(G_OBJECT(ban_btn), "sess", sess);
	g_signal_connect(ban_btn, "clicked", 
					   G_CALLBACK(user_ban_btn), gchat->userlist);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/ban.xpm", NULL);
	pix = gtkhx_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(ban_btn, pix);
	gtk_widget_set_tooltip_text(ban_btn, _("Ban"));
	icon = 0, pix = 0;

	chat_btn = gtk_button_new();
	g_object_set_data(G_OBJECT(chat_btn), "sess", sess);
	gtk_widget_set_tooltip_text(chat_btn, _("Private Chat"));
	g_signal_connect(chat_btn, "clicked", 
					   G_CALLBACK(user_chat_btn), gchat->userlist);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/chat.xpm", NULL);
    pix = gtkhx_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(chat_btn, pix);
	icon = 0, pix = 0;

	igno_btn = gtk_button_new();
	g_object_set_data(G_OBJECT(igno_btn), "sess", sess);
	gtk_widget_set_tooltip_text(igno_btn, _("Ignore"));
	g_signal_connect(igno_btn, "clicked", 
					   G_CALLBACK(user_igno_btn), gchat->userlist);
	icon = (GdkPixmap *)gdk_pixbuf_new_from_resource("/com/nasledov/gtkhx/pixmaps/ignore.xpm", NULL);
	pix = gtkhx_image_new_from_pixbuf((GdkPixbuf *)icon);
	gtkhx_widget_set_child(igno_btn, pix);

	topframe = gtk_frame_new(0);
	gtk_widget_set_size_request(topframe, -1, 30);

	hbuttonbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtkhx_widget_set_child(topframe, hbuttonbox);

	gtkhx_box_pack(hbuttonbox, msg_btn, 0, 0, 0);

	gtkhx_box_pack(hbuttonbox, chat_btn, 0, 0, 2);
	gtkhx_box_pack(hbuttonbox, info_btn, 0, 0, 0);
	gtkhx_box_pack(hbuttonbox, kick_btn, 0, 0, 2);
	gtkhx_box_pack(hbuttonbox, ban_btn, 0, 0, 0);
	gtkhx_box_pack(hbuttonbox, igno_btn, 0, 0, 0);


	user_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	userframe = gtk_frame_new(0);

	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
								   GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
	gtk_widget_set_size_request(scroll, 232, 232);
	gtkhx_widget_set_child(scroll, gchat->userlist);
	gtkhx_widget_set_child(userframe, scroll);

	gtkhx_box_pack(user_vbox, topframe, 0, 0, 0);
	gtkhx_box_pack(user_vbox, userframe, 1, 1, 0);


	hpane = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_set_start_child(GTK_PANED(hpane), vbox);
	gtk_paned_set_end_child(GTK_PANED(hpane), user_vbox);
	gtk_paned_set_position(GTK_PANED(hpane), 435);

	gtkhx_widget_set_child(pchat_window, hpane);

	gtk_window_present(GTK_WINDOW(pchat_window));
	init_keyaccel(pchat_window);


	gtk_widget_grab_focus(gchat->input);
	g_free(title);



	gchat->window = pchat_window;

	return gchat;
}

void hx_clear_chat(struct htlc_conn *htlc, guint32 cid, int subj)
{
	session *sess = &the_session;
	struct gtkhx_chat *gchat = gchat_with_cid(sess, cid);

	gtk_xtext_clear(GTK_XTEXT(gchat->output)->buffer, 0);
	if(gtkhx_prefs.geo.chat.open) {
		if(subj) {
			gtk_editable_set_text(GTK_EDITABLE(gchat->subject), "");
		}
	}
}
