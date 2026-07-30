/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "config.h"

#include <errno.h>
#include <string.h>

#include "host_port.h"

gboolean
gtkhx_parse_host_port (const char *str, guint16 default_port, char **host_out,
                       guint16 *port_out, gboolean *had_port_out)
{
    if (host_out) {
        *host_out = NULL;
    }
    if (!str) {
        return FALSE;
    }

    const char *host_start;
    const char *host_end;
    const char *port_str = NULL; /* NULL = no port present */

    if (*str == '[') {
        /* Bracketed IPv6: "[addr]" or "[addr]:port". */
        const char *close = strchr (str, ']');
        if (!close) {
            return FALSE; /* unterminated bracket */
        }
        host_start = str + 1;
        host_end = close;
        const char *after = close + 1;
        if (*after == ':') {
            port_str = after + 1;
        } else if (*after != '\0') {
            return FALSE; /* junk after the closing bracket */
        }
    } else {
        const char *first = strchr (str, ':');
        const char *last = strrchr (str, ':');
        if (first && first == last) {
            /* Exactly one colon → host:port. */
            host_start = str;
            host_end = last;
            port_str = last + 1;
        } else {
            /* No colon (plain host / IPv4) or two-plus colons (an
             * unbracketed IPv6 literal, which can't carry a port
             * unambiguously) → host only. */
            host_start = str;
            host_end = str + strlen (str);
        }
    }

    if (host_end == host_start) {
        return FALSE; /* empty host */
    }

    guint16 port = default_port;
    gboolean had_port = FALSE;
    if (port_str) {
        if (*port_str == '\0') {
            return FALSE; /* trailing ':' with no port */
        }
        char *endptr = NULL;
        errno = 0;
        gint64 v = g_ascii_strtoll (port_str, &endptr, 10);
        if (errno != 0 || endptr == port_str || *endptr != '\0' || v < 1
            || v > 65535) {
            return FALSE; /* non-numeric / garbage / out of range */
        }
        port = (guint16)v;
        had_port = TRUE;
    }

    if (host_out) {
        *host_out = g_strndup (host_start, (gsize)(host_end - host_start));
    }
    if (port_out) {
        *port_out = port;
    }
    if (had_port_out) {
        *had_port_out = had_port;
    }
    return TRUE;
}

char *
gtkhx_join_host_port (const char *host, guint16 port)
{
    g_return_val_if_fail (host != NULL, NULL);

    /* A bare ':' marks an IPv6 literal; bracket it so the result parses
     * back unambiguously (RFC 3986 §3.2.2). IPv4 / hostnames pass
     * through as-is. */
    if (strchr (host, ':') != NULL) {
        return g_strdup_printf ("[%s]:%u", host, (unsigned)port);
    }
    return g_strdup_printf ("%s:%u", host, (unsigned)port);
}
