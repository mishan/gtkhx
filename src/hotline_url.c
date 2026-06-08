/*
 * hotline_url.c — parser for the hotline:// URL scheme. See
 * hotline_url.h for the shape and notes on why this is a separate
 * module (pure-GLib, link-cheap for unit tests).
 *
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "hotline_url.h"

/* Unescape a URL component bounded by [start, end) into `out` of
 * `out_sz` bytes. Truncates silently if the decoded value doesn't
 * fit, since `out` is sized for the destination (login/pass/host
 * have fixed wire sizes and the user-visible truncation matches
 * what would happen if they typed the value into the Connect
 * dialog). Returns TRUE on a successful copy. */
static gboolean
unescape_segment (const char *start, const char *end, char *out, gsize out_sz)
{
    char *segment;
    char *decoded;
    gsize seg_len;

    if (out_sz == 0) {
        return FALSE;
    }
    out[0] = '\0';

    if (!start || !end || end < start) {
        return FALSE;
    }

    seg_len = (gsize)(end - start);
    if (seg_len == 0) {
        return TRUE; /* empty is fine — e.g. no login */
    }

    segment = g_strndup (start, seg_len);
    decoded = g_uri_unescape_string (segment, NULL);
    g_free (segment);
    if (!decoded) {
        return FALSE;
    }

    g_strlcpy (out, decoded, out_sz);
    g_free (decoded);
    return TRUE;
}

gboolean
hotline_url_parse (const char *url, HotlineUrlParts *out)
{
    const char *p, *hostinfo, *userinfo_end, *path_start;
    const char *host_start, *host_end, *port_sep;
    char portbuf[8];

    if (!url || !out) {
        return FALSE;
    }
    memset (out, 0, sizeof (*out));

    if (g_ascii_strncasecmp (url, "hotline://", 10) != 0) {
        return FALSE;
    }
    p = url + 10;

    /* Path / query strip — Hotline URLs don't carry a path component,
	 * but tolerate a trailing "/" or "/?foo=bar" so paste-from-browser
	 * doesn't fail on a stray slash. */
    path_start = strpbrk (p, "/?#");
    if (!path_start) {
        path_start = p + strlen (p);
    }

    /* userinfo split: the LAST '@' before the path. URL-encoded '@'
	 * inside the userinfo (%40) is fine — we only split on a literal
	 * '@'. Scanning to the last '@' instead of the first means a
	 * password containing an unescaped '@' lands in the password
	 * field as long as it isn't trailing. */
    userinfo_end = NULL;
    {
        const char *scan;
        for (scan = p; scan < path_start; scan++) {
            if (*scan == '@') {
                userinfo_end = scan;
            }
        }
    }
    if (userinfo_end) {
        const char *user_end = userinfo_end;
        const char *pass_start = NULL;
        const char *scan;
        /* First ':' in userinfo separates login from password. */
        for (scan = p; scan < userinfo_end; scan++) {
            if (*scan == ':') {
                user_end = scan;
                pass_start = scan + 1;
                break;
            }
        }
        if (!unescape_segment (p, user_end, out->login, sizeof (out->login))) {
            return FALSE;
        }
        if (pass_start
            && !unescape_segment (pass_start, userinfo_end, out->pass,
                                  sizeof (out->pass))) {
            return FALSE;
        }
        hostinfo = userinfo_end + 1;
    } else {
        hostinfo = p;
    }

    /* host[:port]. IPv6 literals would arrive bracketed
	 * ("[::1]:5500"); Hotline 1.x is IPv4 in practice, but accept
	 * the bracket form so a v6 paste doesn't get mis-split on the
	 * last colon. */
    host_start = hostinfo;
    host_end = path_start;
    port_sep = NULL;

    if (host_start < path_start && *host_start == '[') {
        const char *close = memchr (host_start, ']', path_start - host_start);
        if (!close) {
            return FALSE;
        }
        host_start++;
        host_end = close;
        /* After ']' the only allowed continuation before the path is
		 * ":port". Reject trailing junk like "[::1]foo" — otherwise
		 * the trailing bytes get silently dropped and we'd connect to
		 * an endpoint that doesn't match what the user typed. */
        if (close + 1 < path_start) {
            if (*(close + 1) != ':') {
                return FALSE;
            }
            port_sep = close + 1;
        }
    } else {
        const char *scan;
        int n_colons = 0;
        for (scan = host_start; scan < path_start; scan++) {
            if (*scan == ':') {
                port_sep = scan;
                n_colons++;
            }
        }
        /* Unbracketed multi-colon hostinfo is ambiguous: "2001:db8::1"
		 * could be an IPv6 literal OR "2001" with port "db8::1". The
		 * last-':' rule would misread the IPv6 as host="2001:db8::"
		 * port="1" and silently connect to the wrong place. Reject
		 * and require brackets ("[2001:db8::1]") for IPv6 literals. */
        if (n_colons > 1) {
            return FALSE;
        }
        if (port_sep) {
            host_end = port_sep;
        }
    }

    if (host_end <= host_start) {
        return FALSE; /* empty host */
    }
    if (!unescape_segment (host_start, host_end, out->host,
                           sizeof (out->host))) {
        return FALSE;
    }

    if (port_sep) {
        unsigned long port;
        char *endp;
        if (!unescape_segment (port_sep + 1, path_start, portbuf,
                               sizeof (portbuf))) {
            return FALSE;
        }
        if (portbuf[0]) {
            port = strtoul (portbuf, &endp, 10);
            if (endp && *endp == '\0' && port > 0 && port <= 65535) {
                out->port = (guint16)port;
            }
        }
    }

    return TRUE;
}
