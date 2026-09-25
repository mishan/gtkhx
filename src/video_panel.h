/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * video_panel.h — the Video panel and the screen-sharing indicator, both
 * built in Rust (gtkhx-ui video_panel / screen_share). Video rides the
 * voice session, so everything here is HAVE_VOICE-only.
 */

#ifndef GTKHX_VIDEO_PANEL_H
#define GTKHX_VIDEO_PANEL_H

#include <gtk/gtk.h>
#include "session.h"

/* Open (or raise) the Video panel for sess: the voice room's cameras and
 * screen shares as tiles. The panel decides what this client receives —
 * everything in the room while it is visible, nothing while it isn't. */
extern void create_video_window (GtkWidget *parent, session *sess);

/* Re-read the room for every Video panel on sess's connection, after the
 * login reply or a disconnect changed what the server offers. */
extern void video_panel_refresh_all (session *sess);

/* The "You are sharing your screen" banner for the main window's top
 * bars, revealed while any connection shares. Transfer none. */
extern GtkWidget *gtkhx_screen_share_banner_new (void);

#endif /* GTKHX_VIDEO_PANEL_H */
