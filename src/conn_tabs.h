/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * conn_tabs.h — the C ABI of the connection tab strip.
 *
 * One tab per open connection. Selecting one moves the session focus and
 * swaps every per-connection dock panel to that connection's content page —
 * the visible half of the tab-switched layout (docs/multi-connection.md).
 *
 * Not chat_tabs.h, which is the strip *inside* the Chat panel and switches
 * conversations within whichever connection is currently showing. The two
 * nest: this one picks the server, that one picks the conversation on it.
 *
 * Implemented in Rust (rust/crates/gtkhx-ui/src/conn_tabs.rs); there is no
 * conn_tabs.c. Every entry point is main-thread only.
 */

#ifndef HX_CONN_TABS_H
#define HX_CONN_TABS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

struct htlc_conn;
struct _session;

/* Build the strip and return the widget to pack. Idempotent — a second call
 * hands back the same widget rather than a second strip.
 *
 * Transfer none: gtkhx-ui keeps the owning reference and the caller's
 * container takes its own. The strip hides itself while there is one
 * connection, so packing it costs nothing until it has something to say. */
GtkWidget *gtkhx_conn_tabs_new (void);

/* Add a tab for `sess` and select it — a new connection becomes the one the
 * user is looking at. `title` is whatever there is to call it at the time;
 * gtkhx_conn_tabs_set_title replaces it once the connection has a host. */
void gtkhx_conn_tabs_add (struct _session *sess, const char *title);

/* Select a connection's tab, as if the user had clicked it — moving the focus
 * and swapping every per-connection panel through the same path. No-op for a
 * session with no tab. */
void gtkhx_conn_tabs_select (struct _session *sess);

/* Retitle a connection's tab. No-op for a connection with no tab. */
void gtkhx_conn_tabs_set_title (struct htlc_conn *htlc, const char *title);

/* Flag / clear a connection's tab. Setting is ignored for the connection
 * already selected — a badge on the tab you are looking at says "look here"
 * about the thing you are already looking at. Clearing always applies. */
void gtkhx_conn_tabs_set_attention (struct htlc_conn *htlc, gboolean state);

/* How many connections the strip is showing.
 *
 * Declared for completeness; no C caller today. It exists for the Rust test,
 * which needs a way to assert on the index without reaching into the module's
 * thread-local. */
guint32 gtkhx_conn_tabs_count (void);

G_END_DECLS

#endif /* ndef HX_CONN_TABS_H */
