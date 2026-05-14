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

/*
 * news15.c — Hotline 1.5 threaded-news RPC senders.
 *
 * The two-window legacy UI (gnews_folder + gnews_catalog) that used
 * to live in this file has been removed; news_browser.c is the
 * single consumer now and brings its own UI. Everything left here
 * is wire-format: each helper takes the request shape from the
 * caller, registers a task, and emits the HTLC frame. Replies come
 * back through rcv.c's news_folder / news_catalog / news_post
 * tasks and surface as GtkhxSession signals.
 */

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>

#include <glib.h>

#include "hx.h"
#include "network.h"
#include "tasks.h"
#include "rcv.h"
#include "files.h"
#include "news15.h"

void
hx_news15_get_post (struct htlc_conn *htlc, struct news_item *item)
{
    guint8 *hldir;
    guint16 hldirlen;
    guint32 postid;

    hldir = path_to_hldir (item->group->path, &hldirlen, 0);
    task_new (htlc, RCV_TASK_FN (rcv_task_news_post), item, 0, "news_post");

    postid = htonl (item->postid);
    hlwrite (htlc, HTLC_HDR_GETTHREAD, 0, 3, HTLC_DATA_NEWSPATH, hldirlen,
             hldir, HTLC_DATA_THREADID, 4, &postid, HTLC_DATA_NEWSTYPE,
             strlen (item->parts[0].mime_type), item->parts[0].mime_type);
    g_free (hldir);
}

void
hx_news15_cat_list (struct htlc_conn *htlc, struct gnews_catalog *gcnews)
{
    guint8 *hldir;
    guint16 hldirlen;

    gcnews->listing = 1;
    hldir = path_to_hldir (gcnews->path, &hldirlen, 0);
    task_new (htlc, RCV_TASK_FN (rcv_task_newscat_list), gcnews, 0,
              "news_category");
    hlwrite (htlc, HTLC_HDR_NEWSCATLIST, 0, 1, HTLC_DATA_NEWSPATH, hldirlen,
             hldir);
    g_free (hldir);
}

void
hx_news15_fldr_list (struct htlc_conn *htlc, struct gnews_folder *gfnews)
{
    guint8 *hldir;
    guint16 hldirlen;

    gfnews->listing = 1;
    hldir = path_to_hldir (gfnews->path, &hldirlen, 0);
    task_new (htlc, RCV_TASK_FN (rcv_task_newsfolder_list), gfnews, 0,
              "news_folder");

    hlwrite (htlc, HTLC_HDR_NEWSDIRLIST, 0, 1, HTLC_DATA_NEWSPATH, hldirlen,
             hldir);
    g_free (hldir);
}

/* `threadid` is the post being replied to (HTLC_DATA_THREADID on the
 * wire). mhxd writes this into the new post's "References:" header,
 * which is what its catlist response uses to thread replies under
 * their parent. The accompanying HTLC_DATA_PARENTTHREAD chunk is
 * required by the wire spec but not actually consulted by mhxd —
 * we send 0. */
void
hx_news15_post_thread (struct htlc_conn *htlc, char *path, const char *subject,
                       guint32 threadid, char *text)
{
    guint8 *hldir;
    guint16 hldirlen;
    guint32 parent = 0;

    hldir = path_to_hldir (path, &hldirlen, 0);
    task_new (htlc, 0, 0, 0, "news15_post");
    threadid = htonl (threadid);
    hlwrite (htlc, HTLC_HDR_POSTTHREAD, 0, 6, HTLC_DATA_NEWSPATH, hldirlen,
             hldir, HTLC_DATA_PARENTTHREAD, 4, &parent, HTLC_DATA_NEWSTYPE, 11,
             "text/plain", HTLC_DATA_NEWSSUBJECT, strlen (subject), subject,
             HTLC_DATA_NEWSDATA, strlen (text), text, HTLC_DATA_THREADID, 4,
             &threadid);
    g_free (hldir);
}

void
hx_news15_delete_thread (struct htlc_conn *htlc, char *path, guint32 threadid)
{
    guint8 *hldir;
    guint16 hldirlen;

    hldir = path_to_hldir (path, &hldirlen, 0);
    task_new (htlc, 0, 0, 0, "news15_rm_thread");
    threadid = htonl (threadid);
    hlwrite (htlc, HTLC_HDR_DELETETHREAD, 0, 2, HTLC_DATA_NEWSPATH, hldirlen,
             hldir, HTLC_DATA_THREADID, 4, &threadid);
    g_free (hldir);
}

void
hx_news15_delete (struct htlc_conn *htlc, char *path)
{
    guint8 *hldir;
    guint16 hldirlen;

    hldir = path_to_hldir (path, &hldirlen, 0);
    task_new (htlc, 0, 0, 0, "news15_rm");
    hlwrite (htlc, HTLC_HDR_DELNEWSDIRCAT, 0, 1, HTLC_DATA_NEWSPATH, hldirlen,
             hldir);
    g_free (hldir);
}

void
hx_news15_mkcat (struct htlc_conn *htlc, char *path, const char *name)
{
    guint8 *hldir;
    guint16 hldirlen;

    hldir = path_to_hldir (path, &hldirlen, 0);
    task_new (htlc, 0, 0, 0, "news15_mkcat");
    hlwrite (htlc, HTLC_HDR_MAKECATEGORY, 0, 2, HTLC_DATA_NEWSPATH, hldirlen,
             hldir, HTLC_DATA_CATEGORY, strlen (name), name);
    g_free (hldir);
}

void
hx_news15_mkdir (struct htlc_conn *htlc, char *path)
{
    guint8 *hldir;
    guint16 hldirlen;

    hldir = path_to_hldir (path, &hldirlen, 0);
    task_new (htlc, 0, 0, 0, "news15_mkdir");
    hlwrite (htlc, HTLC_HDR_MAKENEWSDIR, 0, 1, HTLC_DATA_NEWSPATH, hldirlen,
             hldir);
    g_free (hldir);
}
