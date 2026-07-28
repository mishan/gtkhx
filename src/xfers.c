/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
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
 *
 * You should have received a copy of the GNU General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "config.h"
#include <gtk/gtk.h>
#include "hx.h"
#include "htxf_accessors.h"
#include "preview.h"
#include "xfers.h"


/* xfers.c is the last C remnant of the file-transfer worker shell. The transfer
 * byte-loops (hxnet::xfer), the struct htxf_conn storage + refcount/cancel
 * lifecycle (hxnet::xfer_handle), the xfers[] registry, construction + progress /
 * completion marshaling, the worker dispatch + params, and xfer_go's wire-request
 * build all moved to the Rust transfer module (hxhandlers::xfer). What's left is
 * the last-ref destructor, which does the GTK/preview teardown that stays in C. */

/* The teardown hx_htxf_unref runs on a handle's last ref, registered via
 * hx_htxf_set_destructor. It does the GTK/preview + channel teardown that must
 * stay in C; the cancellation token + the struct itself are freed by hx_htxf_free
 * (Rust), which hx_htxf_unref calls right after this returns. xfer_close_channel
 * is the Rust transfer module's channel-close (hxhandlers::xfer). */
void
htxf_destructor (struct htxf_conn *htxf)
{
    /* Drop the per-htxf ref on the preview window if this transfer
	 * was a preview. The window holds its own ref independently;
	 * if the user has already closed the preview, this is the ref
	 * that keeps the struct alive long enough for the worker to
	 * stop touching it — see hx_preview_new's docstring. The
	 * field is NULL on a download/upload (non-preview) htxf,
	 * which hx_preview_unref handles with an early return. */
    hx_preview_unref ((hx_preview *)htxf->preview);
    htxf->preview = NULL;
    /* Close the hxnet channel (drops its ref to the cancellation token, so the
	 * token frees when hx_htxf_free drops the C-side creation ref). Also
	 * releases any AEAD read-side accumulator buffers. No-op on a transfer
	 * that ran in plaintext mode / was already closed by the worker. */
    xfer_close_channel (htxf);
}
