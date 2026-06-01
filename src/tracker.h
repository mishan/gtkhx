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

/* Set up the per-tracker section for the upcoming batch of records.
 * network.c emits this signal once after the v1/v3 wire version is
 * known and BEFORE any tracker_server_create calls land for that
 * batch — see gtkhx_session_emit_tracker_batch_begin's docs. The
 * view creates (or finds) a section for `tracker_url`, sets it as
 * the current sink for records, and chooses which columns to show
 * (v1 sections suppress Country / Caps since v1 records can never
 * carry the TLV-backed metadata). `expected_count` is the wire-
 * declared total; the view renders it in the section's subtitle. */
extern void tracker_batch_begin (const char *tracker_url, guint8 version,
                                 guint16 expected_count);

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
