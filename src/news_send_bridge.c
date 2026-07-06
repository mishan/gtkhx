/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * news_send_bridge.c — field accessors for the 1.5 news RPC senders that moved
 * to the hxnews-send Rust crate. Three of those senders (get_post / cat_list /
 * fldr_list) take C structs (news_item / gnews_catalog / gnews_folder) whose
 * relevant fields the Rust side reads (or, for the *_list ones, sets the
 * `listing` flag) through these shims — so hxnews-send doesn't mirror those
 * GTK-laden structs. Mirrors chat_send_bridge.c's role for the chat senders.
 */

#include "config.h"

#include <glib.h>

#include "session.h" /* struct news_item / news_group / gnews_catalog / gnews_folder */
#include "news_send_bridge.h"

const char *
news_item_group_path (struct news_item *item)
{
    return (item && item->group) ? item->group->path : NULL;
}

guint32
news_item_postid (struct news_item *item)
{
    return item ? item->postid : 0;
}

const char *
news_item_mime0 (struct news_item *item)
{
    if (item && item->partcount > 0 && item->parts) {
        return item->parts[0].mime_type;
    }
    return NULL;
}

const char *
gnews_catalog_path (struct gnews_catalog *g)
{
    return g ? g->path : NULL;
}

void
gnews_catalog_mark_listing (struct gnews_catalog *g)
{
    if (g) {
        g->listing = 1;
    }
}

const char *
gnews_folder_path (struct gnews_folder *g)
{
    return g ? g->path : NULL;
}

void
gnews_folder_mark_listing (struct gnews_folder *g)
{
    if (g) {
        g->listing = 1;
    }
}
