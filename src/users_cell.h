/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * users_cell.h — HxUserCellName, the custom Name-column cell widget.
 *
 * Split out of users_view.c when HxUserListView was ported to Rust
 * (Phase R5.9): the custom snapshot/measure rendering (Mac-classic
 * icon-as-background + name overlay, wide-banner shift, text-outline
 * halo, GIF-avatar click-to-pause) stays C for now, behind this minimal
 * ABI. The Rust view (gtkhx-ui `users_view` module) constructs one cell
 * per Name column item via hx_user_cell_name_new and rebinds it with
 * hx_user_cell_name_set_row on each factory bind/unbind.
 *
 * A GtkWidget subclass. Backed by a borrowed HxUserRow (the row model,
 * itself Rust since R5.8) whose "changed" signal the cell tracks so an
 * in-place rename / status flip re-snapshots without a model splice.
 */

#ifndef HX_USERS_CELL_H
#define HX_USERS_CELL_H 1

#include <gtk/gtk.h>

#include "users_row.h" /* HxUserRow — borrowed row model */

G_BEGIN_DECLS

#define HX_TYPE_USER_CELL_NAME (hx_user_cell_name_get_type ())
G_DECLARE_FINAL_TYPE (HxUserCellName, hx_user_cell_name, HX, USER_CELL_NAME,
                      GtkWidget)

/* Construct a Name cell. `text_x_offset` is the unscaled start-edge
 * offset the name paints at (icon renders behind it); `themed` follows
 * the GTKHX_SCALE_USERLIST_* theme areas (standalone Users window) vs a
 * fixed 1.0 density (compact chat sidebar); `text_outline` paints the
 * contrast halo; `row_height` is the base row height tuned at the
 * default-theme icon scale. */
GtkWidget *hx_user_cell_name_new (int text_x_offset, gboolean themed,
                                  gboolean text_outline, int row_height);

/* Bind (or, with row==NULL, unbind) the cell to a borrowed HxUserRow.
 * Reconnects the row's "changed" handler and re-resolves the icon. */
void hx_user_cell_name_set_row (HxUserCellName *cell, HxUserRow *row);

G_END_DECLS

#endif /* HX_USERS_CELL_H */
