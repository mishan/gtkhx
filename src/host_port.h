/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * host_port.h — one place to parse and format "host:port" strings with
 * correct IPv6 handling, so every call site stops hand-rolling
 * strrchr(':') (which mis-splits IPv6 literals: "::1" becomes host "::"
 * port "1", and "[::1]:5500" leaks the brackets into the host).
 */

#ifndef GTKHX_HOST_PORT_H
#define GTKHX_HOST_PORT_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * Parse a "host", "host:port", "[ipv6]", or "[ipv6]:port" string into a
 * newly-allocated host (IPv6 brackets stripped; caller g_free's) and a
 * port.
 *
 * Returns TRUE on success and writes *host_out (always non-NULL on
 * success). When a port is present *port_out is it and *had_port_out (if
 * non-NULL) is TRUE; with no port *port_out is `default_port` and
 * *had_port_out is FALSE.
 *
 * The port, when present, must be a complete decimal number in 1..65535:
 * a missing-after-colon, non-numeric, trailing-garbage, or out-of-range
 * port makes the call return FALSE (and *host_out is left NULL). An
 * unterminated "[..." also fails.
 *
 * An UNbracketed IPv6 literal (a host token holding two or more colons,
 * which can't be told apart from host:port) is accepted as a host-only
 * result — no port — so "fe80::1" parses as the host, not host "fe80:"
 * port "1".
 *
 * `default_port` may be 0 for callers that treat 0 as "unset". On FALSE
 * nothing is allocated.
 */
gboolean gtkhx_parse_host_port (const char *str, guint16 default_port,
                                char **host_out, guint16 *port_out,
                                gboolean *had_port_out);

/*
 * Format `host` + `port` into a newly-allocated "host:port", bracketing
 * the host as "[host]:port" when it is an IPv6 literal (contains ':').
 * Caller g_free's. `host` must be non-NULL.
 */
char *gtkhx_join_host_port (const char *host, guint16 port);

G_END_DECLS

#endif /* GTKHX_HOST_PORT_H */
