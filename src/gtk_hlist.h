/*
 * gtk_hlist.h — backward-compatibility redirect to the modern shim.
 *
 * Phase 2.7: the original 9k-LOC GtkCList fork (gtk_hlist.c) has been
 * replaced by a thin GtkTreeView-backed shim (gtk_hlist_compat.[ch]).
 * Consumers continue to `#include "gtk_hlist.h"` unchanged; this header
 * just forwards to the shim. Once the GTK 4 port lands and the rest of
 * the codebase has been migrated to native GtkTreeView/GtkColumnView
 * idioms, both headers can disappear together.
 */

#ifndef __GTK_HLIST_H__
#define __GTK_HLIST_H__

#include "gtk_hlist_compat.h"

#endif /* __GTK_HLIST_H__ */
