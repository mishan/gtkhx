/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * tracker_row.h — one row in the tracker results list.
 *
 * A GObject so it can sit inside a GListStore that GtkColumnView
 * consumes. Built from an HxTrackerServer event at insert time;
 * carries deep-copied versions of the strings and a deep-copied
 * HxTrackerV3Meta so the originating event can be freed by the
 * signal emitter once tracker_server_create returns.
 *
 * Lives next to (rather than replacing) HxTrackerServer because
 * HxTrackerServer is the wire-event payload — boxed, refcounted
 * for signal-edge use — and HxTrackerRow is a proper GObject
 * specifically because GListStore items must be GObjects. The two
 * stay separate to keep model-side and view-side types from
 * cross-contaminating.
 */

#ifndef HX_TRACKER_ROW_H
#define HX_TRACKER_ROW_H 1

#include <glib-object.h>

#include "tracker_event.h"
#include "tracker_v3_meta.h"

G_BEGIN_DECLS

#define HX_TYPE_TRACKER_ROW (hx_tracker_row_get_type ())
G_DECLARE_FINAL_TYPE (HxTrackerRow, hx_tracker_row, HX, TRACKER_ROW, GObject)

/* Construct a row from a parsed event. Strings + meta are
 * deep-copied so the row outlives the event. `event` MUST be
 * non-NULL with a non-NULL printable address (the constructor
 * guarantees that — see tracker_event.c). */
extern HxTrackerRow *hx_tracker_row_new_from_event (HxTrackerServer *event);

extern const char *hx_tracker_row_get_name (HxTrackerRow *r);
extern const char *hx_tracker_row_get_desc (HxTrackerRow *r);
extern const char *hx_tracker_row_get_address (HxTrackerRow *r);
extern guint16 hx_tracker_row_get_port (HxTrackerRow *r);
extern guint16 hx_tracker_row_get_nusers (HxTrackerRow *r);

/* Borrowed pointer to the row's typed TLV view. Never NULL — v1
 * records get a zero-init meta from hx_tracker_v3_meta_copy of
 * the event's zero-init meta. Caller MUST NOT free; lifetime is
 * tied to the row's. */
extern HxTrackerV3Meta *hx_tracker_row_get_meta (HxTrackerRow *r);

G_END_DECLS

#endif /* HX_TRACKER_ROW_H */
