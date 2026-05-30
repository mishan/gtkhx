#ifndef HX_TRACKER_H
#define HX_TRACKER_H

#include "tracker_event.h"

/* Render a parsed server record into the tracker window. The
 * event is borrowed for the duration of the call — implementation
 * copies anything it wants to keep. Safe to pass NULL (no-op),
 * or with NULL / empty event->address (logged and dropped).
 *
 * Dedup is keyed on the (event->address, event->port) tuple
 * where event->address is the printable form of the address
 * field — IPv4 dotted-quad, IPv6 colon-hex, or a literal
 * hostname depending on the source record's addr_type. All
 * three are routed through the same BST; callers MUST NOT
 * pre-filter by addr_type. The double-click handler and
 * Connect button hand event->address verbatim to hx_connect,
 * whose getaddrinfo accepts every form transparently. */
extern void tracker_server_create (HxTrackerServer *event);
extern void create_tracker_window (GtkWidget *widget, gpointer data);
extern void tracker_clear (void);
extern void tracker_kill_threads (void);
/* Re-run the tracker search filter against the cached server tree.
 * No-op when the tracker window isn't currently open. Called from
 * options.c's changed_case() so the visible result list stays in
 * lockstep when the user flips the Case-sensitive switch in Settings
 * while the Tracker is open. */
extern void tracker_search_refresh (void);
#endif
