/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "config.h"

#include <glib-object.h>

#include "files_provider.h"

G_DEFINE_INTERFACE (HxFilesProvider, hx_files_provider, G_TYPE_OBJECT)

static void
hx_files_provider_default_init (HxFilesProviderInterface *iface)
{
	(void) iface;

	/* Both signals are declared on the interface; implementations
	 * emit by name (g_signal_emit_by_name) so the dispatch finds
	 * the per-instance signal IDs through GType inheritance. The
	 * signals carry the same payload shape across all providers
	 * so the panel can listen once at the interface level. */
	g_signal_new ("navigated",
		HX_TYPE_FILES_PROVIDER,
		G_SIGNAL_RUN_LAST, 0,
		NULL, NULL, NULL,
		G_TYPE_NONE, 1, G_TYPE_STRING);

	g_signal_new ("error",
		HX_TYPE_FILES_PROVIDER,
		G_SIGNAL_RUN_LAST, 0,
		NULL, NULL, NULL,
		G_TYPE_NONE, 1, G_TYPE_STRING);

	/* The "unavailable-changed" signal lets a panel know when
	 * get_unavailable_reason() flipped from / to NULL — typically
	 * the remote provider firing this on login + disconnect.
	 * Implementations without an unavailable state never emit it. */
	g_signal_new ("unavailable-changed",
		HX_TYPE_FILES_PROVIDER,
		G_SIGNAL_RUN_LAST, 0,
		NULL, NULL, NULL,
		G_TYPE_NONE, 0);
}

/* Wrapper helpers — each just dispatches through the interface
 * vtable. Keep the surface narrow: panel + browser code calls
 * these, never iface->method directly. */

GListModel *
hx_files_provider_get_listing (HxFilesProvider *self)
{
	g_return_val_if_fail (HX_IS_FILES_PROVIDER (self), NULL);
	return HX_FILES_PROVIDER_GET_IFACE (self)->get_listing (self);
}

const char *
hx_files_provider_get_current_path (HxFilesProvider *self)
{
	g_return_val_if_fail (HX_IS_FILES_PROVIDER (self), "");
	return HX_FILES_PROVIDER_GET_IFACE (self)->get_current_path (self);
}

const char *
hx_files_provider_get_label (HxFilesProvider *self)
{
	g_return_val_if_fail (HX_IS_FILES_PROVIDER (self), "");
	return HX_FILES_PROVIDER_GET_IFACE (self)->get_label (self);
}

void
hx_files_provider_navigate (HxFilesProvider *self, const char *path)
{
	g_return_if_fail (HX_IS_FILES_PROVIDER (self));
	HX_FILES_PROVIDER_GET_IFACE (self)->navigate (self, path);
}

void
hx_files_provider_reload (HxFilesProvider *self)
{
	g_return_if_fail (HX_IS_FILES_PROVIDER (self));
	HX_FILES_PROVIDER_GET_IFACE (self)->reload (self);
}

void
hx_files_provider_navigate_up (HxFilesProvider *self)
{
	g_return_if_fail (HX_IS_FILES_PROVIDER (self));
	HX_FILES_PROVIDER_GET_IFACE (self)->navigate_up (self);
}

gboolean
hx_files_provider_mkdir (HxFilesProvider *self, const char *name, GError **err)
{
	g_return_val_if_fail (HX_IS_FILES_PROVIDER (self), FALSE);
	return HX_FILES_PROVIDER_GET_IFACE (self)->mkdir (self, name, err);
}

gboolean
hx_files_provider_delete (HxFilesProvider *self, const char *name, GError **err)
{
	g_return_val_if_fail (HX_IS_FILES_PROVIDER (self), FALSE);
	return HX_FILES_PROVIDER_GET_IFACE (self)->delete_entry (self, name, err);
}

gboolean
hx_files_provider_rename (HxFilesProvider *self,
                          const char *old_name, const char *new_name,
                          GError    **err)
{
	g_return_val_if_fail (HX_IS_FILES_PROVIDER (self), FALSE);
	return HX_FILES_PROVIDER_GET_IFACE (self)->rename (self,
		old_name, new_name, err);
}

const char *
hx_files_provider_get_unavailable_reason (HxFilesProvider *self)
{
	HxFilesProviderInterface *iface;
	g_return_val_if_fail (HX_IS_FILES_PROVIDER (self), NULL);
	iface = HX_FILES_PROVIDER_GET_IFACE (self);
	/* Optional method — providers that don't override report
	 * "always available" by returning NULL. */
	return iface->get_unavailable_reason
		? iface->get_unavailable_reason (self)
		: NULL;
}
