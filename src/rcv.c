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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <gtk/gtk.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>
#include "hx.h"
#include "network.h"
#include "xfers.h"
#include "chat.h"
#include "tasks.h"
#include "files.h"
#include "gtkutil.h"
#include "msg.h"
#include "news.h"
#include "sound.h"
#include "plugin.h"
#include "users.h"
#include "usermod.h"
#include "news15.h"
#include "hfs.h"
#include "connect.h"

#include "sha.h"
#include "md5.h"

static size_t news_len = 0;
static guint8 *news_buf = 0;
static char *hx_timeformat = "%c";
extern int xfer_go_timer (void *__arg);

void rcv_task_user_list (struct htlc_conn *htlc, struct chat *chat, int text);

/*
void print_binary(char *buf, int len)
{
	int i;

	for(i = 0; i < len; i++) {
		int j;

		for(j = 0; j < 8; j++) {
			printf("%d", *buf&j?1:0);
		}
	}
	printf("\n");
}
*/

int task_inerror (struct htlc_conn *htlc)
{
        struct hl_hdr *h = (struct hl_hdr *)htlc->in.buf;

/*		g_print("task_inerror: h->flag == %d\n", h->flag);
		print_binary(&h->flag, 4); */
		
        if ((ntohl(h->flag) & 1))
                return 1;

        return 0;
}

void hx_rcv_chat (struct htlc_conn *htlc)
{
	guint32 cid = 0;
	guint16 len = 0;
	char chatbuf[8192 + 1], *chat;
	guint16 uid = 0;
	session *sess = sess_from_htlc(htlc);
	struct chat *hx_chat = chat_with_cid(sess, 0);

	dh_start(htlc)
		switch (_type) {
		case HTLS_DATA_CHAT:
			len = _len > (sizeof(chatbuf) - 1) ? (sizeof(chatbuf) - 1) : _len;
			memcpy(chatbuf, dh->data, len);
			break;
		case HTLS_DATA_CHAT_ID:
			dh_getint(cid);
			break;
		case HTLS_DATA_UID: /* this only works on hxd 0.1.35 or higher */
			dh_getint(uid);
			break;
		}
	dh_end()

	if(uid) { /* do ignoring stuff */
		struct hx_user *user = hx_user_with_uid(hx_chat->user_list, uid);
		if(user && user->ignore) {
			return;
		}
	}

	CR2LF(chatbuf, len);
	strip_ansi(chatbuf, len);
	chatbuf[len] = 0;

	if (chatbuf[0] == '\n') {
		chat = chatbuf+1;
		len--;
	} else {
		chat = chatbuf;
	}

#ifdef USE_PLUGIN
	if(EMIT_SIGNAL(XP_RCV_CHAT, sess, chat, &cid, &uid, 0, 0) == 1) {
		return;
	}
#endif

	hx_output.chat(htlc, cid, chat, len);
	play_sound(CHAT_POST);
}

void hx_rcv_msg (struct htlc_conn *htlc)
{
	guint16 uid = 0;
	guint16 msglen = 0, nlen = 0;
	char msgbuf[8192 + 1], name[128 + 1];
	session *sess = sess_from_htlc(htlc);
	struct chat *chat = chat_with_cid(sess, 0);
	struct hx_user *user = 0;

	dh_start(htlc) {
		switch (_type) {
		case HTLS_DATA_UID:
			dh_getint(uid);
			break;
		case HTLS_DATA_MSG:
			msglen = (_len > 8192) ? 8192 : _len;
			memcpy(msgbuf, dh->data, msglen);
			break;
		case HTLS_DATA_NAME:
			nlen = (_len > 128) ? 128 : _len;
			memcpy(name, dh->data, nlen);
			strip_ansi(name, nlen);
			break;
		}
	} dh_end();
	
	
	user = hx_user_with_uid(chat->user_list, uid);
	if(user && user->ignore) {
		return;
	}
	
	CR2LF(msgbuf, msglen);
	strip_ansi(msgbuf, msglen);
	name[nlen] = 0;
	msgbuf[msglen] = 0;

#ifdef USE_PLUGIN
	if(EMIT_SIGNAL(XP_RCV_MSG, sess, msgbuf, name, &uid, 0, 0) == 1) {
		return;
	}
#endif

	if(uid > 0) {
		hx_output.msg(name, uid, msgbuf);
	}
	else {
		broadcastmsg(msgbuf);
	}
	play_sound(MSG);

	if (!*last_msg_nick) {
		strncpy(last_msg_nick, name, 31);
		last_msg_nick[31] = 0;
	}
}

void hx_rcv_agreement_file (struct htlc_conn *htlc)
{
	dh_start(htlc) {
		if (_type == HTLS_DATA_NOAGREEMENT)
			return;
		if (_type != HTLS_DATA_AGREEMENT)
			continue;
		CR2LF(dh->data, _len);
		strip_ansi(dh->data, _len);
#ifdef USE_PLUGIN
		if(EMIT_SIGNAL(XP_RCV_AGREE, sess_from_htlc(htlc), dh->data, &_len, 0, 0, 0)
		   == 1) {
			return;
		}
#endif
		hx_output.agreement(sess_from_htlc(htlc), dh->data, _len);
		
	} dh_end();
}

void hx_rcv_news_post (struct htlc_conn *htlc)
{
	dh_start(htlc) {
		if (_type != HTLS_DATA_NEWS)
			continue;

		news_len += _len;
		news_buf = g_realloc(news_buf, news_len + 1);

		memmove(&(news_buf[_len]), news_buf, news_len - _len);
		memcpy(news_buf, dh->data, _len);

		CR2LF(news_buf, _len);
		strip_ansi(news_buf, _len);
		news_buf[news_len] = '\0';

		hx_output.news_post(htlc, news_buf, _len);

		play_sound(NEWS_POST);
	} dh_end();
}

void hx_rcv_task (struct htlc_conn *htlc)
{
	struct hl_hdr *h = (struct hl_hdr *)htlc->in.buf;
	guint32 trans;
	struct task *tsk;
	char error = 0;

	HN32(&trans, &h->trans);
	tsk = task_with_trans(sess_from_htlc(htlc), trans);

	if (task_inerror(htlc)) {
		task_error(htlc);
		error = 1;
	}
	if (tsk) {
		/* XXX tsk->rcv might call task_delete */
		int fd = htlc->fd;
		if (tsk->rcv && !error)
			tsk->rcv(htlc, tsk->ptr, tsk->data);
		if (hxd_files[fd].conn.htlc)
			task_delete(sess_from_htlc(htlc), tsk);
	} else {
		/*	hx_printf_prefix(0, INFOPREFIX, "got task 0x%08x\n", trans); */
	}
}

void hx_rcv_user_change (struct htlc_conn *htlc)
{
	guint32 cid = 0;
	guint16 nlen = 0, got_color = 0, uid = 0, color = 0, icon = 0;
	guint8 name[32];
	struct chat *chat;
	struct hx_user *user;
	session *sess = sess_from_htlc(htlc);

	if (task_inerror(htlc))
		return;

	dh_start(htlc) {
		switch (_type) {
		case HTLS_DATA_UID:
			dh_getint(uid);
			break;
		case HTLS_DATA_ICON:
			dh_getint(icon);
			break;
		case HTLS_DATA_NAME:
			nlen = (_len > 31) ? 31 : _len;
			memcpy(name, dh->data, nlen);
			name[nlen] = 0;
			strip_ansi(name, nlen);
			break;
		case HTLS_DATA_COLOUR:
			dh_getint(color);
			got_color = 1;
			break;
		case HTLS_DATA_CHAT_ID:
			dh_getint(cid);
			break;
		}
	} dh_end();

	chat = chat_with_cid(sess, cid);
	if (!chat) {
		chat = chat_new(sess, cid);
	}
	user = hx_user_with_uid(chat->user_list, uid);
	if (!user) {
		user = hx_user_new(&chat->user_tail);
		chat->nusers++;
		user->uid = uid;
		hx_output.user_create(htlc, chat, user, name, icon, color);
		play_sound(USER_JOIN);
		if(gtkhx_prefs.showjoin) {
			hx_printf_prefix(htlc, cid, INFOPREFIX, _("join: %s\n"), name);
		}
	}
	
	else {
		if (!got_color) {
			color = user->color;
		}
		hx_output.user_change(htlc, chat, user, name, icon, color);
		if((user->color == color && user->icon == icon) || 
		   strcmp(name, user->name)) {
			if(user->ignore) { 
				return;
			}
			hx_printf_prefix(htlc, cid, INFOPREFIX, _("%s is now known as %s\n"),
							 user->name, name);
		}
		
	}
	if (nlen) {
		memcpy(user->name, name, nlen);
		user->name[nlen] = 0;
	}
	if (icon) {
		user->icon = icon;
	}
	if (got_color) {
		user->color = color;
	}
	if ((uid) && (uid == htlc->uid)) {
		htlc->icon = user->icon;
		htlc->color = user->color;
		strcpy(htlc->name, user->name);
	}
}

void hx_rcv_user_part (struct htlc_conn *htlc)
{
	guint16 uid = 0;
	guint32 cid = 0;
	struct chat *chat;
	struct hx_user *user;
	char *col;
	session *sess = sess_from_htlc(htlc);

	dh_start(htlc) {
		switch (_type) {
		case HTLS_DATA_UID:
			dh_getint(uid);
			break;
		case HTLS_DATA_CHAT_ID:
			dh_getint(cid);
			break;
		}
	} dh_end();
	
	chat = chat_with_cid(sess, cid);
	if (!chat)
		return;

	user = hx_user_with_uid(chat->user_list, uid);
	if (user) {
		col = colorstr(user->color);
		hx_output.user_delete(htlc, chat, user);

		if(gtkhx_prefs.showjoin) {
			hx_printf_prefix(htlc, cid, INFOPREFIX, _("parts: %s \n"), user->name);
		}

		hx_user_delete(&chat->user_tail, user);
		chat->nusers--;
		play_sound(USER_PART);

	}
}

void hx_rcv_chat_subject (struct htlc_conn *htlc)
{
	guint32 cid = 0;
	guint16 slen = 0;
	guint8 subject[256];
	struct chat *chat;
	session *sess = sess_from_htlc(htlc);

	dh_start(htlc) {
		switch (_type) {
		case HTLS_DATA_CHAT_ID:
			dh_getint(cid);
			break;
		case HTLS_DATA_CHAT_SUBJECT:
			slen = (_len > 255) ? 255 : _len;
			memcpy(subject, dh->data, slen);
			break;
		}
	} dh_end();

	if (slen) {
		chat = chat_with_cid(sess, cid);
		if (!chat)
			return;
		if(strcmp(subject, chat->subject) == 0)
			return;
		memcpy(chat->subject, subject, slen);
		chat->subject[slen] = 0;
		
#ifdef USE_PLUGIN
		if(EMIT_SIGNAL(XP_RCV_SUBJ, sess, chat->subject, &cid, 0, 0, 0) == 1) {
			return;
		}
#endif
		hx_output.chat_subject(htlc, cid, chat->subject);
	}
}

void hx_rcv_chat_invite (struct htlc_conn *htlc)
{
	guint16 uid = 0;
	guint32 cid = 0;
	guint16 nlen;
	guint8 name[32];
	session *sess = sess_from_htlc(htlc);
	struct chat *chat = chat_with_cid(sess, 0);
	struct hx_user *user = 0;

	name[0] = 0;
	dh_start(htlc) {
		switch (_type) {
		case HTLS_DATA_UID:
			dh_getint(uid);
			break;
		case HTLS_DATA_CHAT_ID:
			dh_getint(cid);
			break;
		case HTLS_DATA_NAME:
			nlen = (_len > 31) ? 31 : _len;
			memcpy(name, dh->data, nlen);
			name[nlen] = 0;
			strip_ansi(name, nlen);
			break;
		}
	} dh_end();

	user = hx_user_with_uid(chat->user_list, uid);
	if(user && user->ignore) {
		return;
	}
#ifdef USE_PLUGIN
	if(EMIT_SIGNAL(XP_RCV_INVITE, sess, name, &uid, &cid, 0, 0) == 1) {
		return;
	}
#endif
	hx_output.chat_invitation(htlc, cid, name);
	play_sound(CHAT_INVITE);
}

void hx_rcv_user_selfinfo (struct htlc_conn *htlc)
{
	struct hl_userlist_hdr *uh;
	guint16 nlen;

	dh_start(htlc) {
		switch (_type) {
		case HTLS_DATA_ACCESS:
			if (_len != 8)
				break;
			memcpy(&htlc->access, dh->data, 8);
			break;
		case HTLS_DATA_USER_LIST:
			if (_len < (SIZEOF_HL_USERLIST_HDR - SIZEOF_HL_DATA_HDR))
				break;
			uh = (struct hl_userlist_hdr *)dh;
			HN16(&htlc->uid, &htlc->uid);
			HN16(&htlc->icon, &uh->icon);
			HN16(&uh->color, &uh->color);
			HN16(&nlen, &uh->nlen);
			nlen = (nlen > 31) ? 31 : nlen;
			memcpy(htlc->name, uh->name, nlen);
			htlc->name[nlen] = 0;
			break;
		}
	} dh_end();

	setbtns(&sessions[0], 1);
}

void hx_rcv_dump (struct htlc_conn *htlc)
{
	int fd;

	fd = open("hx.dump", O_WRONLY|O_APPEND|O_CREAT, 0644);
	if (fd < 0)
		return;
	write(fd, htlc->in.buf, htlc->in.pos);
	fsync(fd);
	close(fd);
}

void hx_rcv_xfer_queue(struct htlc_conn *htlc)
{
	guint32 ref = 0, queueid = 0;
	struct htxf_conn *htxf;

	dh_start(htlc) {
		switch(_type) {
		case HTLS_DATA_HTXF_REF:
			dh_getint(ref);
			break;
		case HTLS_DATA_QUEUE:
			dh_getint(queueid);
			break;
		}
	} dh_end();

	htxf = htxf_with_ref(ref);

	if(!htxf) {
		g_warning(_("Received queue id (%d) for xfer ref %d\n"
				  "No such xfer.\n"), queueid, ref);
		return;
	}
	htxf->queue = queueid;
	hx_output.xfer_queue(sess_from_htlc(htlc), htxf);

	if(!htxf->queue) {
		xfer_ready_write(htxf);
	}
}

void hx_rcv_hdr (struct htlc_conn *htlc)
{
	struct hl_hdr *h = (struct hl_hdr *)htlc->in.buf;
	guint32 len;
	guint32 type;

	HN32(&len, &h->len);
	HN32(&type, &h->type);
	if (len < 2) {
		len = 0;
	}
	else {
		len = ((len > MAX_HOTLINE_PACKET_LEN) ? MAX_HOTLINE_PACKET_LEN : len) -
			2;
	}

	/* htlc->trans = ntohl(h->trans); */
	htlc->rcv = 0;
	switch (type) {
	case HTLS_HDR_CHAT:
		htlc->rcv = hx_rcv_chat;
		break;
	case HTLS_HDR_MSG:
		htlc->rcv = hx_rcv_msg;
		break;
	case HTLS_HDR_USER_CHANGE:
	case HTLS_HDR_CHAT_USER_CHANGE:
		htlc->rcv = hx_rcv_user_change;
		break;
	case HTLS_HDR_USER_PART:
	case HTLS_HDR_CHAT_USER_PART:
		htlc->rcv = hx_rcv_user_part;
		break;
	case HTLS_HDR_NEWS_POST:
		htlc->rcv = hx_rcv_news_post;
		break;
	case HTLS_HDR_TASK:
		htlc->rcv = hx_rcv_task;
		break;
	case HTLS_HDR_CHAT_SUBJECT:
		htlc->rcv = hx_rcv_chat_subject;
		break;
	case HTLS_HDR_CHAT_INVITE:
		htlc->rcv = hx_rcv_chat_invite;
		break;
	case HTLS_HDR_MSG_BROADCAST:
		htlc->rcv = hx_rcv_msg;
		break;
	case HTLS_HDR_USER_SELFINFO:
		htlc->rcv = hx_rcv_user_selfinfo;
		break;
	case HTLS_HDR_AGREEMENT:
		htlc->rcv = hx_rcv_agreement_file;
		break;
	case HTLS_HDR_POLITEQUIT:
		hx_printf_prefix(htlc, 0, INFOPREFIX, _("polite quit\n"));
		htlc->rcv = hx_rcv_msg;
		break;
	case HTLS_HDR_QUEUE:
		htlc->rcv = hx_rcv_xfer_queue;
		break;
	default:
		g_print("0x%08x\n", type);
		hx_printf_prefix(htlc, 0, INFOPREFIX, _("unknown header type 0x%08x\n"),
						 type);
		htlc->rcv = hx_rcv_dump;
		break;
	}

	if (len) {
		qbuf_set(&htlc->in, htlc->in.pos, len);
	} 
	else {
		if (htlc->rcv) {
			htlc->rcv(htlc);
		}
		htlc->rcv = hx_rcv_hdr;
		qbuf_set(&htlc->in, 0, SIZEOF_HL_HDR);
	}
}

void rcv_task_user_open (struct htlc_conn *htlc, struct uesp_fn *uespfn)
{
	char name[32], login[32], pass[32];
	guint16 nlen = 0, llen = 0, plen = 0;
	hl_access_bits access;
	char accessbool=0;

	dh_start(htlc) {
		switch (_type) {
		case HTLS_DATA_NAME:
			if(_len >= sizeof(name))
				nlen = sizeof(name)-1;
			else
				nlen = _len;
			memcpy(name, dh->data, nlen);
			break;
		case HTLS_DATA_LOGIN:
			if(_len >= sizeof(login))
				llen = sizeof(login)-1;
			else
				llen = _len;
			hl_decode(login, dh->data, llen);
			break;
		case HTLS_DATA_PASSWORD:
			if(_len >= sizeof(pass))
				plen = sizeof(pass)-1;
			else
				plen = _len;
			if (plen > 1 && dh->data[0])
				hl_decode(pass, dh->data, plen);
			else
				pass[0] = 0;
			break;
		case HTLS_DATA_ACCESS:
			if (_len >= sizeof(access))
				{
				accessbool = 1;
				memcpy(&access, dh->data, sizeof(access));
			}
			break;
		}
	} dh_end();

	name[nlen] = 0;
	login[llen] = 0;
	pass[plen] = 0;
	if (accessbool) {
		uespfn->fn(uespfn->uesp, name, login, pass, access);
	}
	g_free(uespfn);
}


void rcv_task_msg (struct htlc_conn *htlc, char *msg_buf)
{
	if (msg_buf) {
		hx_printf(htlc, 0, "%s\n", msg_buf);
		g_free(msg_buf);
	}
}

void rcv_task_newscat_list(struct htlc_conn *htlc, 
						   struct gnews_catalog *gcnews)
{
	struct news_group *group = g_malloc(sizeof(struct news_group));
	struct news_item *ni;
	unsigned char *ptr;
	int p, j;
	int exit = 0;

	/* Taken from fidelio =) */
#define get_pstring(ret) if(*ptr==0){ret=NULL;}else{ret=g_malloc(1+*ptr); \
 memcpy(ret,ptr+1,*ptr); (ret)[*ptr]=0;ptr+=*ptr;} ptr++;

	dh_start(htlc) {
		switch (_type) {
		case HTLC_DATA_CATLIST:
			ptr = dh->data+4;
			HN32(&group->post_count, ptr); ptr += 4; 
			ptr += *ptr;
			ptr += 2;
			if(!group->post_count) {
				exit = 1;
				break;
			}
			
			group->posts = g_malloc(sizeof(struct news_item)*group->post_count);
			
			for(p = 0, ni = group->posts; p < group->post_count; p++, ni++) {
				HN32(&ni->postid, ptr); ptr += 4;
				HN16(&ni->date.base_year, ptr); ptr += 2;
 				HN16(&ni->date.pad, ptr); ptr += 2;  
				HN32(&ni->date.seconds, ptr); ptr += 4;
				HN32(&ni->parentid, ptr); ptr += 8;
				HN16(&ni->partcount, ptr); ptr += 2; 
				
				get_pstring(ni->subject);
				get_pstring(ni->sender);
				ni->parts = g_malloc(sizeof(struct news_parts)*ni->partcount);
				ni->size = 0;
				for(j = 0; j < ni->partcount; j++) {
					get_pstring(ni->parts[j].mime_type);
					HN16(&ni->parts[j].size, ptr); ptr += 2;
					ni->size+=ni->parts[j].size;
				}
				ni->group = group;
			}
			break;
		}
		
	} dh_end();
/*	
	if(exit) {

		return;
		} */

	gcnews->group = group;
	hx_output.news_catalog(gcnews);
}

void rcv_task_newsfolder_list(struct htlc_conn *htlc, 
							  struct gnews_folder *gfnews)
{
	struct news_folder *folder = g_malloc(sizeof(struct news_folder));
	struct folder_item *item;
	int num = 0;

	folder->entry = g_malloc(sizeof(struct folder_item));
	folder->path = gfnews->path;

	dh_start(htlc) {
		switch (_type) {
		case HTLC_DATA_NEWSFOLDERITEM:
			num++;
			folder->entry = g_realloc(folder->entry, 
									  sizeof(struct folder_item *)*num);
			item = g_malloc(sizeof(struct folder_item));
			item->type = dh->data[0];
			item->name = g_malloc(_len);
			memcpy(item->name, dh->data+1, _len-1);
			item->name[_len-1] = 0;
			folder->entry[num-1] = item;
			break;
		}
	} dh_end();
	
	folder->num_entries = num;

	gfnews->news = folder;
	hx_output.news_folder(gfnews);
}

void rcv_task_news_post (struct htlc_conn *htlc, struct news_item *item)
{
	struct news_post *post = 0;
	guint32 postid;

	dh_start(htlc) {
		switch(_type) {
		case HTLC_DATA_NEWSDATA:
			post = g_malloc(sizeof(struct news_post));
			post->buf = g_malloc(_len+1);
			memcpy(post->buf, dh->data, _len);
			CR2LF(post->buf, _len);
			strip_ansi(post->buf, _len);
			post->buf[_len] = 0;
			break;
		case HTLC_DATA_THREADID:
			dh_getint(postid);
			break;
		case HTLS_DATA_TASKERROR:
			return;
		} 
	} dh_end();
	
	post->item = item;
	hx_output.news_thread(post);
}


void rcv_task_news_users(struct htlc_conn *htlc, struct chat *chat, int text)
{
	/* output user list and then grab news */
	/* this is only used for login events  */
	rcv_task_user_list(htlc, chat, text);

	reload_news(0, sess_from_htlc(htlc));
}

void rcv_task_login (struct htlc_conn *htlc, char *pass)
{
	char buf[HOSTLEN];
	guint16 uid;
	guint16 version;
	guint16 len;
	char servername[8192+1];
	session *sess = sess_from_htlc(htlc);
	guint16 hc;
	guint16 icon16;
	guint8 *p, *mal = 0;
	guint16 mal_len = 0;
	guint16 sklen = 0, macalglen = 0, secure_login = 0, secure_password = 0;
	guint8 password_mac[20];
	guint8 login[32];
	guint16 llen, pmaclen;
#ifdef CONFIG_CIPHER
	u_int8_t *s_cipher_al = 0, *c_cipher_al = 0;
	u_int16_t s_cipher_al_len = 0, c_cipher_al_len = 0;
	u_int8_t s_cipheralg[32], c_cipheralg[32];
	u_int16_t s_cipheralglen = 0, c_cipheralglen = 0;
	u_int8_t cipheralglist[64];
	u_int16_t cipheralglistlen;
#endif
#ifdef CONFIG_COMPRESS
	guint8 *s_compress_al = 0, *c_compress_al = 0;
	guint16 s_compress_al_len = 0, c_compress_al_len = 0;
	guint8 s_compressalg[32], c_compressalg[32];
	guint16 s_compressalglen = 0, c_compressalglen = 0;
	guint8 compressalglist[64];
	guint16 compressalglistlen;
#endif
	
#ifdef USE_IPV6
	getnameinfo(htlc->addr->ai_addr, htlc->addr->ai_addrlen, buf, sizeof(buf),
				NULL, 0, NI_NUMERICHOST);
#else
#ifndef WIN32
	inet_ntop(AF_INET, &htlc->addr.sin_addr, buf, sizeof(buf));
#else
	snprintf(buf, sizeof(buf), "%s", inet_ntoa(htlc->addr.sin_addr));
#endif
#endif

	if(!pass) {
		hx_printf_prefix(htlc, 0, INFOPREFIX, "%s:%u: %s %s\n",
						 buf,
#ifdef USE_IPV6
						 server_port,
#else
						 ntohs(htlc->addr.sin_port),
#endif
						 _("login"),
						 
						 task_inerror(htlc) ? _("failed?") : _("successful"));
	}

	if (pass) {
		dh_start(htlc) {
			switch (_type) {
			case HTLS_DATA_LOGIN:
				if (_len && _len == strlen(htlc->macalg) && !memcmp(htlc->macalg, dh->data, _len))
					secure_login = 1;
				break;
			case HTLS_DATA_PASSWORD:
				if (_len && _len == strlen(htlc->macalg) && !memcmp(htlc->macalg, dh->data, _len))
					secure_password = 1;
				break;
			case HTLS_DATA_MAC_ALG:
				mal_len = _len;
				mal = dh->data;
				break;
#ifdef CONFIG_CIPHER
				case HTLS_DATA_CIPHER_ALG:
					s_cipher_al_len = _len;
					s_cipher_al = dh->data;
					break;
				case HTLC_DATA_CIPHER_ALG:
					c_cipher_al_len = _len;
					c_cipher_al = dh->data;
					break;
				case HTLS_DATA_CIPHER_MODE:
					break;
				case HTLC_DATA_CIPHER_MODE:
					break;
				case HTLS_DATA_CIPHER_IVEC:
					break;
				case HTLC_DATA_CIPHER_IVEC:
					break;
#endif
#if defined(CONFIG_COMPRESS)
			case HTLS_DATA_COMPRESS_ALG:
				s_compress_al_len = _len;
				s_compress_al = dh->data;
				break;
			case HTLC_DATA_COMPRESS_ALG:
				c_compress_al_len = _len;
				c_compress_al = dh->data;
				break;
#endif
			case HTLS_DATA_CHECKSUM_ALG:
				break;
			case HTLC_DATA_CHECKSUM_ALG:
				break;
			case HTLS_DATA_SESSIONKEY:
				sklen = _len > sizeof(htlc->sessionkey) ? sizeof(htlc->sessionkey) : _len;
				memcpy(htlc->sessionkey, dh->data, sklen);
				htlc->sklen = sklen;
				break;
			}
		} dh_end();
		
		if (!mal_len) {
		  no_mal:			hx_printf_prefix(htlc, 0, INFOPREFIX, "No macalg from server\n");
		  hx_htlc_close(htlc, 0);
		  return;
		}
		
		p = list_n(mal, mal_len, 0);
		if (!p || !*p)
			goto no_mal;
		macalglen = *p >= sizeof(htlc->macalg) ? sizeof(htlc->macalg)-1 : *p;
		memcpy(htlc->macalg, p+1, macalglen);
		htlc->macalg[macalglen] = 0;
		
		if (sklen < 20) {
			hx_printf_prefix(htlc, 0, INFOPREFIX,
							 "sessionkey length (%u) not big enough\n", sklen);
			hx_htlc_close(htlc, 0);
			return;
		}
		
		if (task_inerror(htlc)) {
			g_free(pass);
			hx_htlc_close(htlc, 0);
			return;
		}
		task_new(htlc, rcv_task_login, 0, 0, "login");
		icon16 = htons(htlc->icon);
		if (secure_login) {
			llen = hmac_xxx(login, htlc->login, strlen(htlc->login),
							htlc->sessionkey, sklen, htlc->macalg);
			if (!llen) {
				hx_printf_prefix(htlc, 0, INFOPREFIX,
								 "bad HMAC algorithm %s\n", htlc->macalg);
				hx_htlc_close(htlc, 0);
				return;
			}
		} else {
			llen = strlen(htlc->login);
			hl_encode(login, htlc->login, llen);
			login[llen] = 0;
		}
		pmaclen = hmac_xxx(password_mac, pass, strlen(pass),
						   htlc->sessionkey, sklen, htlc->macalg);
		if (!pmaclen) {	
			hx_printf_prefix(htlc, 0, INFOPREFIX,
							 "bad HMAC algorithm %s\n", htlc->macalg);
			hx_htlc_close(htlc, 0);
			return;
		}
		hc = 4;
#ifdef CONFIG_COMPRESS
		if (!htlc->compressalg[0] || !strcmp(htlc->compressalg, "NONE")) {
			hx_printf_prefix(htlc, 0, INFOPREFIX,
							 "WARNING: this connection is not compressed\n");
			compressalglistlen = 0;
			goto no_compress;
		}
		if (!c_compress_al_len || !s_compress_al_len) {
		  no_compress_al:		hx_printf_prefix(htlc, 0, INFOPREFIX,
												 "No compress algorithm from server\n");
		  hx_htlc_close(htlc, 0);
		  return;
		}
		p = list_n(s_compress_al, s_compress_al_len, 0);
		if (!p || !*p)
			goto no_compress_al;
		s_compressalglen = *p >= sizeof(s_compressalg) ? sizeof(s_compressalg)-1 : *p;
		memcpy(s_compressalg, p+1, s_compressalglen);
		s_compressalg[s_compressalglen] = 0;
		p = list_n(c_compress_al, c_compress_al_len, 0);
		if (!p || !*p)
			goto no_compress_al;
		c_compressalglen = *p >= sizeof(c_compressalg) ? sizeof(c_compressalg)-1 : *p;
		memcpy(c_compressalg, p+1, c_compressalglen);
		c_compressalg[c_compressalglen] = 0;
		if (!valid_compress(c_compressalg)) {
			hx_printf_prefix(htlc, 0, INFOPREFIX,
							 "Bad client compress algorithm %s\n", c_compressalg);
			goto ret_badcompress_a;
		} else if (!valid_compress(s_compressalg)) {
			hx_printf_prefix(htlc, 0, INFOPREFIX,
							 "Bad server compress algorithm %s\n", s_compressalg);
		  ret_badcompress_a:
			compressalglistlen = 0;
			hx_htlc_close(htlc, 0);
			return;
		} else {
			{
				guint16 val = 1;
				HN16(compressalglist, &val);
			}
			compressalglistlen = 2;
			compressalglist[compressalglistlen] = s_compressalglen;
			compressalglistlen++;
			memcpy(compressalglist+compressalglistlen, s_compressalg, s_compressalglen);
			compressalglistlen += s_compressalglen;
		}
	  no_compress:
		hc++;
#endif
#ifdef CONFIG_CIPHER
		if (!htlc->cipheralg[0] || !strcmp(htlc->cipheralg, "NONE")) {
			hx_printf_prefix(htlc, 0, INFOPREFIX,
					 "WARNING: this connection is not encrypted\n");
			cipheralglistlen = 0;
			goto no_cipher;
		}
		if (!c_cipher_al_len || !s_cipher_al_len) {
no_cal:			hx_printf_prefix(htlc, 0, INFOPREFIX,
					 "No cipher algorithm from server\n");
			hx_htlc_close(htlc, 0);
			return;
		}
		p = list_n(s_cipher_al, s_cipher_al_len, 0);
		if (!p || !*p)
			goto no_cal;
		s_cipheralglen = *p >= sizeof(s_cipheralg) ? sizeof(s_cipheralg)-1 : *p;
		memcpy(s_cipheralg, p+1, s_cipheralglen);
		s_cipheralg[s_cipheralglen] = 0;
		p = list_n(c_cipher_al, c_cipher_al_len, 0);
		if (!p || !*p)
			goto no_cal;
		c_cipheralglen = *p >= sizeof(c_cipheralg) ? sizeof(c_cipheralg)-1 : *p;
		memcpy(c_cipheralg, p+1, c_cipheralglen);
		c_cipheralg[c_cipheralglen] = 0;
		if (!valid_cipher(c_cipheralg)) {
			hx_printf_prefix(htlc, 0, INFOPREFIX,
					 "Bad client cipher algorithm %s\n", c_cipheralg);
			goto ret_badca;
		} else if (!valid_cipher(s_cipheralg)) {
			hx_printf_prefix(htlc, 0, INFOPREFIX,
					 "Bad server cipher algorithm %s\n", s_cipheralg);
ret_badca:
			cipheralglistlen = 0;
			hx_htlc_close(htlc, 0);
			return;
		} else {
			{
				guint16 val = 1;
				
				HN16(cipheralglist, &val);
			}
			cipheralglistlen = 2;
			cipheralglist[cipheralglistlen] = s_cipheralglen;
			cipheralglistlen++;
			memcpy(cipheralglist+cipheralglistlen, s_cipheralg, s_cipheralglen);
			cipheralglistlen += s_cipheralglen;
		}

		pmaclen = hmac_xxx(htlc->cipher_decode_key, pass, strlen(pass),
				   password_mac, pmaclen, htlc->macalg);
		htlc->cipher_decode_keylen = pmaclen;
		pmaclen = hmac_xxx(htlc->cipher_encode_key, pass, strlen(pass),
				   htlc->cipher_decode_key, pmaclen, htlc->macalg);
		htlc->cipher_encode_keylen = pmaclen;
no_cipher:
		hc++;
#endif
		hlwrite(htlc, HTLC_HDR_LOGIN, 0, hc,
				HTLC_DATA_LOGIN, llen, login,
				HTLC_DATA_PASSWORD, pmaclen, password_mac,
#ifdef CONFIG_CIPHER
			HTLS_DATA_CIPHER_ALG, cipheralglistlen, cipheralglist,
#endif
#ifdef CONFIG_COMPRESS
				HTLS_DATA_COMPRESS_ALG, compressalglistlen, compressalglist,
#endif
				HTLC_DATA_NAME, strlen(htlc->name), htlc->name,
				HTLC_DATA_ICON, 2, &icon16);
		g_free(pass);
#ifdef CONFIG_COMPRESS
		if (compressalglistlen) {
			hx_printf_prefix(htlc, 0, INFOPREFIX, "compress: server %s client %s\n",
							 c_compressalg, s_compressalg);
			if (c_compress_al_len) {
				htlc->compress_encode_type = COMPRESS_GZIP;
				compress_encode_init(htlc);
			}
			if (s_compress_al_len) {
				htlc->compress_decode_type = COMPRESS_GZIP;
				compress_decode_init(htlc);
			}
		}
#endif
#ifdef CONFIG_CIPHER
		if (cipheralglistlen) {
			hx_printf_prefix(htlc, 0, INFOPREFIX, "cipher: server %s client %s\n",
					 c_cipheralg, s_cipheralg);
			if (!strcmp(s_cipheralg, "RC4"))
				htlc->cipher_decode_type = CIPHER_RC4;
			else if (!strcmp(s_cipheralg, "BLOWFISH"))
				htlc->cipher_decode_type = CIPHER_BLOWFISH;
			else if (!strcmp(s_cipheralg, "IDEA"))
				htlc->cipher_decode_type = CIPHER_IDEA;
			if (!strcmp(c_cipheralg, "RC4"))
				htlc->cipher_encode_type = CIPHER_RC4;
			else if (!strcmp(c_cipheralg, "BLOWFISH"))
				htlc->cipher_encode_type = CIPHER_BLOWFISH;
			else if (!strcmp(c_cipheralg, "IDEA"))
				htlc->cipher_encode_type = CIPHER_IDEA;
			cipher_encode_init(htlc);
			cipher_decode_init(htlc);
		}
#endif
	} else {
		if (!task_inerror(htlc)) {
			play_sound(LOGIN);
			changetitlesconnected(sess);
			setbtns(sess, 1);
			set_status_bar(2);
			connected = 1;
			
			dh_start(htlc) {
				switch (_type) {
				case HTLS_DATA_UID:
					dh_getint(uid);
					htlc->uid = uid;
					break;
				case HTLS_DATA_VERSION: /* Hotline 1.5+ servers only */
					dh_getint(version);
					htlc->version = version;
					break;
				case HTLS_DATA_SERVERNAME: /* Hotline 1.5+ servers only */
					len = (_len > sizeof(servername) - 1) ? sizeof(servername) - 1
						: _len;
					memcpy(servername, dh->data, len);
					CR2LF(servername, len);
					strip_ansi(servername, len);
					servername[len] = 0;
					if(server_addr) 
						g_free(server_addr);
					server_addr = g_strdup(servername);
					changetitlesconnected(sess);
					break;
				}
			} dh_end();
			
			
			/* this will get news and users */
			task_new(htlc, rcv_task_news_users, sess->chat_list, 0, "who");
			hlwrite(htlc, HTLC_HDR_USER_GETLIST, 0, 0);
		}	
	}
}
	
	void rcv_task_news_file (struct htlc_conn *htlc)
{
	dh_start(htlc) {
		if (_type != HTLS_DATA_NEWS)
			continue;
		news_len = _len;
		news_buf = g_realloc(news_buf, news_len + 1);
		memcpy(news_buf, dh->data, news_len);
		CR2LF(news_buf, news_len);
		strip_ansi(news_buf, news_len);
		news_buf[news_len] = 0;
	dh_end()

	}

	hx_output.news_file(htlc, news_buf, news_len);
}

void rcv_task_user_list (struct htlc_conn *htlc, struct chat *chat, int text)
{
	struct hl_userlist_hdr *uh;
	struct hx_user *user;
	guint16 nlen, uid;
	int new = 0;
	struct gtkhx_chat *gchat = gchat_with_cid(sess_from_htlc(htlc), chat->cid);

	dh_start(htlc) {
		if (_type == HTLS_DATA_USER_LIST) {
			uh = (struct hl_userlist_hdr *)dh;
			HN16(&uid, &uh->uid);
			user = hx_user_with_uid(chat->user_list, uid);
			if (!user) {
				new = 1;
				user = hx_user_new(&chat->user_tail);
				chat->nusers++;
			}
			HN16(&user->uid, &uh->uid);
			HN16(&user->icon, &uh->icon);
			HN16(&user->color, &uh->color);
			HN16(&nlen, &uh->nlen);
			nlen = (nlen > 31) ? 31 : nlen;
			memcpy(user->name, uh->name, nlen);
			strip_ansi(user->name, nlen);
			user->name[nlen] = 0;
			if (!htlc->uid && !strcmp(user->name, htlc->name) &&

				user->icon == htlc->icon) {
				htlc->uid = user->uid;
				htlc->color = user->color;
			}
			if (new) {
				hx_output.user_create(htlc, chat, user, user->name, user->icon,
									  user->color);
			}
		}

		else if (_type == HTLS_DATA_CHAT_SUBJECT) {
			guint16 slen = (_len > 255) ? 255 : _len;
			memcpy(chat->subject, dh->data, slen);
			chat->subject[slen] = 0;
			if(gchat && gchat->subject) {
				gtk_entry_set_text(GTK_ENTRY(gchat->subject), chat->subject);
			}
		}
	} dh_end();
}

void rcv_task_user_list_switch (struct htlc_conn *htlc, struct chat *chat)
{
	session *sess = sess_from_htlc(htlc);

	if (task_inerror(htlc)) {
		chat_delete(sess, chat);
		return;
	}

	rcv_task_user_list(htlc, chat, 0);
}

void rcv_task_kick (struct htlc_conn *htlc)
{
	if (task_inerror(htlc))
		return;

	hx_printf_prefix(htlc, 0, INFOPREFIX, "%s\n", _("kick successful"));
}


void rcv_task_user_info (struct htlc_conn *htlc, guint16 *_uid, int text)
{
	guint16 ilen = 0, nlen = 0;
	guint8 info[4096 + 1], name[32];
	guint16 uid = *_uid;
	g_free(_uid);

	name[0] = 0;
	dh_start(htlc) {
		switch (_type) {
		case HTLS_DATA_USER_INFO:
			ilen = (_len > 4096) ? 4096 : _len;
			memcpy(info, dh->data, ilen);
			info[ilen] = 0;
			break;
		case HTLS_DATA_NAME:
			nlen = (_len > 31) ? 31 : _len;
			memcpy(name, dh->data, nlen);
			name[nlen] = 0;
			strip_ansi(name, nlen);
			break;
		}
	} dh_end();

	if (nlen && ilen) {
		CR2LF(info, ilen);
		strip_ansi(info, ilen);
		hx_output.user_info(uid, name, info, ilen);
	}
}

void rcv_task_file_list (struct htlc_conn *htlc, struct cached_filelist *cfl,
						 void *data)
{
	struct hl_filelist_hdr *fh = 0;
	guint32 fh_len;
	guint16 fhlen;

	if (task_inerror(htlc)) {
		g_free(cfl);
		return;
	}
	dh_start(htlc){ 
		if (_type != HTLS_DATA_FILE_LIST)
			continue;
		fh = (struct hl_filelist_hdr *)dh;
		if (cfl->completing > 1) {
			char *pathbuf;
			int fnlen, len;
			guint32 ftype;
			
			HN32(&fnlen, &fh->fnlen);
			len = strlen(cfl->path) + 1 + fnlen + 1;
			pathbuf = g_malloc(len+1);
			snprintf(pathbuf, len, "%s%c%.*s", cfl->path[1] ? cfl->path : "",
					 dir_char, (int)fnlen, fh->fname);
			HN32(&ftype, &fh->ftype);
			if (ftype == 0x666c6472) {
				struct cached_filelist *ncfl;
				guint16 hldirlen;
				guint8 *hldir;

				ncfl = cfl_lookup(pathbuf);
				ncfl->completing = cfl->completing;
				ncfl->filter_argv = cfl->filter_argv;
				if (!ncfl->path)
					ncfl->path = g_strdup(pathbuf);
				hldir = path_to_hldir(pathbuf, &hldirlen, 0);
				task_new(&sessions[0].htlc, rcv_task_file_list, ncfl, 0, 
						 "ls_complete");
				hlwrite(&sessions[0].htlc, HTLC_HDR_FILE_LIST, 0, 1,
						HTLC_DATA_DIR, hldirlen, hldir);
				g_free(hldir);
			} else if (cfl->completing == COMPLETE_GET_R) {
				struct htxf_conn *htxf;
				char *lpath, *p;
				
				lpath = g_malloc(len+1);
				dirmask(lpath, pathbuf, "/");
				p = lpath+1;
				while ((p = strchr(p, dir_char))) {
					*p = 0;
					if (mkdir(lpath+1, S_IRUSR|S_IWUSR|S_IXUSR)) {
						if (errno != EEXIST)
							hx_printf_prefix(htlc, 0, INFOPREFIX, 
											 "mkdir(%s): %s\n", lpath+1,
											 strerror(errno));
					}
					*p++ = '/';
					while (*p == dir_char)
						*p++ = '/';
				}
				p = basename(lpath+1);
				if (p)
					dirchar_fix(p);
				htxf = xfer_new(lpath+1, pathbuf, XFER_GET);
				htxf->filter_argv = cfl->filter_argv;
				g_free(lpath);
			}
			g_free(pathbuf);
		}
		HN16(&fhlen, &fh->len);
		fh_len = SIZEOF_HL_DATA_HDR + fhlen;
		fh_len += 4 - (fh_len % 4);
		cfl->fh = g_realloc(cfl->fh, cfl->fhlen + fh_len);
		memcpy((char *)cfl->fh + cfl->fhlen, fh, SIZEOF_HL_DATA_HDR + fhlen);
		*((guint16 *)((char *)cfl->fh + cfl->fhlen + 2)) = 
			htons(fh_len - SIZEOF_HL_DATA_HDR);
		cfl->fhlen += fh_len;
	} dh_end();
	
	cfl_print(cfl, data);
	cfl->completing = COMPLETE_NONE;
}

void rcv_task_file_getinfo (struct htlc_conn *htlc, char *path)
{
	guint8 icon[4], type[32], crea[32], date_create[8], date_modify[8];
	guint8 name[256], comment[256];
	guint16 nlen, clen, tlen;
	guint32 size = 0;
	char created[32], modified[32];
	time_t t;
	struct tm tm;

	if (task_inerror(htlc))
		return;
	name[0] = comment[0] = type[0] = crea[0] = 0;
	dh_start(htlc) {
		switch(_type) {
		case HTLS_DATA_FILE_ICON:
			if (_len >= 4)
				memcpy(icon, dh->data, 4);
			break;
		case HTLS_DATA_FILE_TYPE:
			tlen = (_len > 31) ? 31 : _len;
			memcpy(type, dh->data, tlen);
			type[tlen] = 0;
			break;
		case HTLS_DATA_FILE_CREATOR:
			clen = (_len > 31) ? 31 : _len;
			memcpy(crea, dh->data, clen);
			crea[clen] = 0;
			break;
		case HTLS_DATA_FILE_SIZE:
			if (_len >= 4)
				HN32(&size, dh->data);
			break;
		case HTLS_DATA_FILE_NAME:
			nlen = (_len > 255) ? 255 : _len;
			memcpy(name, dh->data, nlen);
			name[nlen] = 0;
			strip_ansi(name, nlen);
			break;
		case HTLS_DATA_FILE_DATE_CREATE:
			if (_len >= 8)
				memcpy(date_create, dh->data, 8);
			break;
		case HTLS_DATA_FILE_DATE_MODIFY:
			if (_len >= 8)
				memcpy(date_modify, dh->data, 8);
			break;
		case HTLS_DATA_FILE_COMMENT:
			clen = (_len > 255) ? 255 : _len;
			memcpy(comment, dh->data, clen);
			comment[clen] = 0;
			CR2LF(comment, clen);
			strip_ansi(name, clen);
			break;
		}
	} dh_end();
	
	t = ntohl(*((guint32 *)&date_create[4])) - 0x7c25b080;
	localtime_r(&t, &tm);
	strftime(created, 31, hx_timeformat, &tm);
	t = ntohl(*((guint32 *)&date_modify[4])) - 0x7c25b080;
	localtime_r(&t, &tm);
	strftime(modified, 31, hx_timeformat, &tm);

	hx_output.file_info(path, name, crea, type, comment, modified, created, 
						size);
}

void rcv_task_file_get (struct htlc_conn *htlc, struct htxf_conn *htxf)
{
	guint32 ref = 0, size = 0, queue = 0;
	int i;
#ifdef USE_IPV6
	int error;
	char portstr[HOSTLEN];
	struct addrinfo hints;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = PF_UNSPEC;
	hints.ai_flags = AI_NUMERICHOST;
	hints.ai_socktype = SOCK_STREAM;
#endif

	LOCK_HTXF(htlc);
	for(i = 0; i < nxfers; i++) {
		if(xfers[i] == htxf) {
			break;
		}
	}
	UNLOCK_HTXF(htlc);

	if(i == nxfers) {
		return;
	}
	if (task_inerror(htlc)) {
		if (htxf->opt.retry) {
			htxf->gone = 0;
			timer_add_secs(1, xfer_go_timer, htxf);
		} else {
			gtask_delete_htxf(sess_from_htlc(htlc), htxf);
			LOCK_HTXF(htlc);
			xfer_delete(htxf);
			UNLOCK_HTXF(htlc);
		}
		return;
	}

	dh_start(htlc) {
 		switch (_type) {
		case HTLS_DATA_HTXF_SIZE:
			dh_getint(size);
			break;
		case HTLS_DATA_HTXF_REF:
			dh_getint(ref);
			break;
		case HTLS_DATA_QUEUE:
			dh_getint(queue); /* Only on 1.5+ servers */
			break;
		}
	}	dh_end();

	if (!size || !ref)
		return;

	htxf->ref = ref;
	htxf->total_size = size;
	htxf->queue = queue;

	gettimeofday(&htxf->start, 0);
#ifdef USE_IPV6
	{
		char buf[HOSTLEN];
		sprintf(portstr, "%u", server_port+1);

		inet_ntop(htlc->addr->ai_family, &htlc->addr->ai_addr->sa_data[2],
				  buf, HOSTLEN);
		if((error = getaddrinfo(buf, portstr, &hints, &htxf->listen_addr)))
			{
				hx_printf_prefix(&sessions[0].htlc, 0, INFOPREFIX, 
								 "htxf %s:%u failed: %s\n", 
								 htlc->addr->ai_canonname, server_port+1, 
								 gai_strerror(error));
				gtask_delete_htxf(sess_from_htlc(htlc), htxf);
				xfer_delete(htxf);
				return;
			}
	}
#else
	htxf->listen_addr = htlc->addr;
	htxf->listen_addr.sin_port = htons(ntohs(htxf->listen_addr.sin_port)+1);
#endif

	hx_output.xfer_queue(sess_from_htlc(htlc), htxf); /* we most certainly want
														 to output its position
														 in the queue */

	if(!htxf->queue) {
		xfer_ready_write(htxf);
	}
}

void rcv_task_file_put (struct htlc_conn *htlc, struct htxf_conn *htxf)
{
	guint32 ref = 0, data_pos = 0, rsrc_pos = 0, queue = 0;
	struct stat sb;
#ifdef USE_IPV6
	char portstr[HOSTLEN];
	struct addrinfo hints;
	int error;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = PF_UNSPEC;
	hints.ai_flags = AI_NUMERICHOST;
	hints.ai_socktype = SOCK_STREAM;
#endif

	if (task_inerror(htlc)) {
		gtask_delete_htxf(sess_from_htlc(htlc), htxf);
		LOCK_HTXF(htlc);
		xfer_delete(htxf);
		UNLOCK_HTXF(htlc);
		return;
	}

	dh_start(htlc) {
		switch (_type) {
		case HTLS_DATA_HTXF_REF:
			dh_getint(ref);
			break;
		case HTLS_DATA_QUEUE:
			dh_getint(queue);
			break;
		case HTLS_DATA_RFLT:
			if (_len >= 66) {
				HN32(&data_pos, &dh->data[46]);
				HN32(&rsrc_pos, &dh->data[62]);
			}
			break;
		}
	} dh_end();
	
	if (!ref) {
		return;
	}

	htxf->data_pos = data_pos;
	htxf->rsrc_pos = rsrc_pos;
	htxf->queue = queue;

	if (!stat(htxf->path, &sb))
		htxf->data_size = sb.st_size;
	htxf->rsrc_size = resource_len(htxf->path);
	htxf->total_size = 133 + ((htxf->rsrc_size - htxf->rsrc_pos) ? 16 : 0) + 
		comment_len(htxf->path) + (htxf->data_size - htxf->data_pos) + 
		(htxf->rsrc_size - htxf->rsrc_pos);
	htxf->ref = ref;
	gettimeofday(&htxf->start, 0);

#ifdef USE_IPV6
	{
		char buf[HOSTLEN];

		sprintf(portstr, "%u", server_port+1);
		inet_ntop(htlc->addr->ai_family, &htlc->addr->ai_addr->sa_data[2], 
				  buf, HOSTLEN);
		if((error = getaddrinfo(buf, portstr, &hints,
								&htxf->listen_addr))) {
			hx_printf_prefix(&sessions[0].htlc, 0, INFOPREFIX, 
							 "htxf %s:%u failed: %s\n", 
							 htlc->addr->ai_canonname, server_port+1, 
							 gai_strerror(error));
			gtask_delete_htxf(sess_from_htlc(htlc), htxf);
			xfer_delete(htxf);
			return;
		}
	}
#else
	htxf->listen_addr = htlc->addr;
	htxf->listen_addr.sin_port = htons(ntohs(htxf->listen_addr.sin_port)+1);
#endif

	hx_output.xfer_queue(sess_from_htlc(htlc), htxf);

	if(!htxf->queue) {
		xfer_ready_write(htxf);
	}
}
