/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 */

/*
 * See proto_helpers.h for the rationale and the contract for each
 * function in here.
 *
 * Pure-GLib + protocol.h. Do NOT include hx.h, gtk/gtk.h, or
 * anything else that would force the unit tests to link the GUI
 * tree — that's the whole point of this translation unit.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "protocol.h"
#include "hotline.h"
#include "proto_helpers.h"

gboolean
task_error_extract (struct htlc_conn *htlc, char *out,
                    gsize out_size, gsize *out_len)
{
	if (!out || out_size == 0)
		return FALSE;

	gboolean found = FALSE;
	dh_start (htlc) {
		if (_type == HTLS_DATA_TASKERROR && !found) {
			gsize copy_len = _len;
			if (copy_len > out_size - 1)
				copy_len = out_size - 1;
			memcpy (out, dh->data, copy_len);
			CR2LF (out, copy_len);
			strip_ansi (out, copy_len);
			out[copy_len] = '\0';
			if (out_len) *out_len = copy_len;
			found = TRUE;
		}
	} dh_end ();

	return found;
}
