/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef HX_HOTLINE_URL_H
#define HX_HOTLINE_URL_H

#include <glib.h>

/*
 * hotline_url — parser for the hotline:// URL scheme.
 *
 * Shape (de facto standard from the original Mac client):
 *
 *     hotline://[login[:password]@]host[:port][/]
 *
 * Components are URL-decoded via g_uri_unescape_string. Missing port
 * is reported as 0; callers default to 5500 when binding to the wire.
 *
 * Pure GLib — no GTK dependency — so this is link-cheap for unit
 * tests. The Connect / Save Bookmark action handlers in connect.c
 * wrap the parsed struct in the GTK plumbing the rest of the
 * connect path needs.
 */

/* Field sizes follow the Hotline wire limits: host gets a 128-byte
 * buffer; login / pass are 33 bytes (STRING32 = 32 + NUL).
 * Truncation on overlong input is silent — matches what would
 * happen if the user typed the value into the Connect dialog. */
typedef struct {
    char host[128];
    char login[33];
    char pass[33];
    guint16 port; /* 0 == "absent / use protocol default" */
} HotlineUrlParts;

/* Parse `url'. Returns TRUE iff the URL is hotline:// and contains a
 * non-empty host. On FALSE, the contents of *out are unspecified. */
gboolean hotline_url_parse (const char *url, HotlineUrlParts *out);

#endif /* HX_HOTLINE_URL_H */
