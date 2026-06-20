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
#include "hotline_proto.h"
#include "network.h"
#include "proto_helpers.h" /* struct hx_chunk (stack-allocated below) */
#include "tasks.h"
#include "rcv.h"
#include "files.h"
#include "news15.h"
#include "text_util.h"

void
hx_news15_get_post (struct htlc_conn *htlc, struct news_item *item)
{
    guint8 *hldir;
    guint16 hldirlen;

    hldir = path_to_hldir (item->group->path, &hldirlen, 0);

    /* chunk layout moved to gtkhx_proto_build_news_getthread
	 * _chunks. See hx_news15_cat_list for the task ordering rationale. */
    struct hx_chunk chunks[3];
    guint8 scratch[4];
    const char *mime = item->parts[0].mime_type;
    int hc = (int)gtkhx_proto_build_news_getthread_chunks (
        hldir, hldirlen, item->postid, (const uint8_t *)mime, strlen (mime),
        chunks, G_N_ELEMENTS (chunks), scratch, sizeof (scratch));
    if (hc > 0) {
        task_new (htlc, RCV_TASK_FN (rcv_task_news_post), item, 0, "news_post");
        hlwrite_chunks (htlc, HTLC_HDR_GETTHREAD, 0, chunks, hc);
    }
    g_free (hldir);
}

void
hx_news15_cat_list (struct htlc_conn *htlc, struct gnews_catalog *gcnews)
{
    guint8 *hldir;
    guint16 hldirlen;

    gcnews->listing = 1;
    hldir = path_to_hldir (gcnews->path, &hldirlen, 0);

    /* chunk layout moved to gtkhx_proto_build_news_catlist
	 * _chunks. Build BEFORE task_new — see hx_send_msg for the
	 * rationale (a builder failure must not leave a phantom task
	 * behind). */
    struct hx_chunk chunks[1];
    int hc = (int)gtkhx_proto_build_news_catlist_chunks (
        hldir, hldirlen, chunks, G_N_ELEMENTS (chunks));
    if (hc > 0) {
        task_new (htlc, RCV_TASK_FN (rcv_task_newscat_list), gcnews, 0,
                  "news_category");
        hlwrite_chunks (htlc, HTLC_HDR_NEWSCATLIST, 0, chunks, hc);
    }
    g_free (hldir);
}

void
hx_news15_fldr_list (struct htlc_conn *htlc, struct gnews_folder *gfnews)
{
    guint8 *hldir;
    guint16 hldirlen;

    gfnews->listing = 1;
    hldir = path_to_hldir (gfnews->path, &hldirlen, 0);

    /* chunk layout moved to gtkhx_proto_build_news_dirlist
	 * _chunks. See hx_news15_cat_list for the task ordering note. */
    struct hx_chunk chunks[1];
    int hc = (int)gtkhx_proto_build_news_dirlist_chunks (
        hldir, hldirlen, chunks, G_N_ELEMENTS (chunks));
    if (hc > 0) {
        task_new (htlc, RCV_TASK_FN (rcv_task_newsfolder_list), gfnews, 0,
                  "news_folder");
        hlwrite_chunks (htlc, HTLC_HDR_NEWSDIRLIST, 0, chunks, hc);
    }
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

    hldir = path_to_hldir (path, &hldirlen, 0);

    /* Phase E2/E3: subject is a single-line name field (no LF→CR);
	 * the article body is a body field (with LF→CR normalisation
	 * on legacy servers). */
    gboolean utf8 = (htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
    gsize subj_len = 0, text_len = 0;
    char *subj_wire = gtkhx_text_for_wire (subject, strlen (subject), utf8,
                                           /*is_body=*/FALSE, &subj_len);
    char *text_wire = gtkhx_text_for_wire (text, strlen (text), utf8,
                                           /*is_body=*/TRUE, &text_len);

    /* chunk layout moved to gtkhx_proto_build_news_post
	 * _thread_chunks. NEWSTYPE is hard-coded "text/plain" — gtkhx
	 * only sends plain-text articles. See hx_news15_cat_list for the
	 * task ordering rationale. */
    static const char mime_type[] = "text/plain";
    struct hx_chunk chunks[6];
    guint8 scratch[8];
    int hc = (int)gtkhx_proto_build_news_post_thread_chunks (
        hldir, hldirlen, /*parent_thread=*/0, (const uint8_t *)mime_type,
        strlen (mime_type), (const uint8_t *)subj_wire, subj_len,
        (const uint8_t *)text_wire, text_len, threadid, chunks,
        G_N_ELEMENTS (chunks), scratch, sizeof (scratch));
    if (hc > 0) {
        task_new (htlc, 0, 0, 0, "news15_post");
        hlwrite_chunks (htlc, HTLC_HDR_POSTTHREAD, 0, chunks, hc);
    }
    g_free (hldir);
    g_free (subj_wire);
    g_free (text_wire);
}

void
hx_news15_delete_thread (struct htlc_conn *htlc, char *path, guint32 threadid)
{
    guint8 *hldir;
    guint16 hldirlen;

    hldir = path_to_hldir (path, &hldirlen, 0);

    /* chunk layout moved to gtkhx_proto_build_news_delete
	 * _thread_chunks. See hx_news15_cat_list for the task ordering. */
    struct hx_chunk chunks[2];
    guint8 scratch[4];
    int hc = (int)gtkhx_proto_build_news_delete_thread_chunks (
        hldir, hldirlen, threadid, chunks, G_N_ELEMENTS (chunks), scratch,
        sizeof (scratch));
    if (hc > 0) {
        task_new (htlc, 0, 0, 0, "news15_rm_thread");
        hlwrite_chunks (htlc, HTLC_HDR_DELETETHREAD, 0, chunks, hc);
    }
    g_free (hldir);
}

void
hx_news15_delete (struct htlc_conn *htlc, char *path)
{
    guint8 *hldir;
    guint16 hldirlen;

    hldir = path_to_hldir (path, &hldirlen, 0);

    /* chunk layout moved to gtkhx_proto_build_news_delete
	 * _chunks. See hx_news15_cat_list for the task ordering note. */
    struct hx_chunk chunks[1];
    int hc = (int)gtkhx_proto_build_news_delete_chunks (
        hldir, hldirlen, chunks, G_N_ELEMENTS (chunks));
    if (hc > 0) {
        task_new (htlc, 0, 0, 0, "news15_rm");
        hlwrite_chunks (htlc, HTLC_HDR_DELNEWSDIRCAT, 0, chunks, hc);
    }
    g_free (hldir);
}

void
hx_news15_mkcat (struct htlc_conn *htlc, char *path, const char *name)
{
    guint8 *hldir;
    guint16 hldirlen;

    hldir = path_to_hldir (path, &hldirlen, 0);

    /* Phase E (follow-up): encode the category name. The path
	 * (NEWSPATH chunk) is built byte-verbatim by path_to_hldir;
	 * DIR-style component encoding is deferred (consistent with
	 * other DIR chunks). */
    gboolean utf8 = (htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
    gsize name_len = 0;
    char *name_wire
        = gtkhx_text_for_wire (name, strlen (name), utf8, FALSE, &name_len);

    /* chunk layout moved to gtkhx_proto_build_news_mkcat
	 * _chunks. See hx_news15_cat_list for the task ordering. */
    struct hx_chunk chunks[2];
    int hc = (int)gtkhx_proto_build_news_mkcat_chunks (
        hldir, hldirlen, (const uint8_t *)name_wire, name_len, chunks,
        G_N_ELEMENTS (chunks));
    if (hc > 0) {
        task_new (htlc, 0, 0, 0, "news15_mkcat");
        hlwrite_chunks (htlc, HTLC_HDR_MAKECATEGORY, 0, chunks, hc);
    }
    g_free (hldir);
    g_free (name_wire);
}

void
hx_news15_mkdir (struct htlc_conn *htlc, char *path)
{
    guint8 *hldir;
    guint16 hldirlen;

    hldir = path_to_hldir (path, &hldirlen, 0);

    /* chunk layout moved to gtkhx_proto_build_news_mkdir
	 * _chunks. See hx_news15_cat_list for the task ordering note. */
    struct hx_chunk chunks[1];
    int hc = (int)gtkhx_proto_build_news_mkdir_chunks (
        hldir, hldirlen, chunks, G_N_ELEMENTS (chunks));
    if (hc > 0) {
        task_new (htlc, 0, 0, 0, "news15_mkdir");
        hlwrite_chunks (htlc, HTLC_HDR_MAKENEWSDIR, 0, chunks, hc);
    }
    g_free (hldir);
}
