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
    (void)iface;

    /* Both signals are declared on the interface; implementations
	 * emit by name (g_signal_emit_by_name) so the dispatch finds
	 * the per-instance signal IDs through GType inheritance. The
	 * signals carry the same payload shape across all providers
	 * so the panel can listen once at the interface level. */
    g_signal_new ("navigated", HX_TYPE_FILES_PROVIDER, G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

    g_signal_new ("error", HX_TYPE_FILES_PROVIDER, G_SIGNAL_RUN_LAST, 0, NULL,
                  NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

    /* The "unavailable-changed" signal lets a panel know when
	 * get_unavailable_reason() flipped from / to NULL — typically
	 * the remote provider firing this on login + disconnect.
	 * Implementations without an unavailable state never emit it. */
    g_signal_new ("unavailable-changed", HX_TYPE_FILES_PROVIDER,
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
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
hx_files_provider_rename (HxFilesProvider *self, const char *old_name,
                          const char *new_name, GError **err)
{
    g_return_val_if_fail (HX_IS_FILES_PROVIDER (self), FALSE);
    return HX_FILES_PROVIDER_GET_IFACE (self)->rename (self, old_name, new_name,
                                                       err);
}

const char *
hx_files_provider_get_unavailable_reason (HxFilesProvider *self)
{
    HxFilesProviderInterface *iface;
    g_return_val_if_fail (HX_IS_FILES_PROVIDER (self), NULL);
    iface = HX_FILES_PROVIDER_GET_IFACE (self);
    /* Optional method — providers that don't override report
	 * "always available" by returning NULL. */
    return iface->get_unavailable_reason ? iface->get_unavailable_reason (self)
                                         : NULL;
}

void
hx_files_provider_activate_entry (HxFilesProvider *self, HxFileEntry *e)
{
    HxFilesProviderInterface *iface;
    g_return_if_fail (HX_IS_FILES_PROVIDER (self));
    iface = HX_FILES_PROVIDER_GET_IFACE (self);
    /* Optional method — providers without an activate action
	 * just silently no-op (the panel's row-activate handler
	 * was the caller; user gets no feedback, same as the
	 * current behaviour pre-feature). */
    if (iface->activate_entry) {
        iface->activate_entry (self, e);
    }
}

void
hx_files_provider_preview_entry (HxFilesProvider *self, HxFileEntry *e)
{
    HxFilesProviderInterface *iface;
    g_return_if_fail (HX_IS_FILES_PROVIDER (self));
    iface = HX_FILES_PROVIDER_GET_IFACE (self);
    /* preview_entry is optional and falls back to activate_entry
	 * — for the local provider that means "the OS default app IS
	 * the preview", which is the right answer. */
    if (iface->preview_entry) {
        iface->preview_entry (self, e);
    } else if (iface->activate_entry) {
        iface->activate_entry (self, e);
    }
}

char *
hx_files_provider_safe_local_basename (const char *remote_name)
{
    char *out, *p;

    /* Empty / NULL: fall back to a generic placeholder. */
    if (!remote_name || !*remote_name) {
        return g_strdup ("download");
    }

    /* Replace path separators with '_'. Hotline's wire name can
	 * legitimately contain '/' under the Classic-Mac convention,
	 * but we're constructing a local path — '/' (and '\\' for
	 * Win-style names) would let a hostile server break out of
	 * the user's download directory. */
    out = g_strdup (remote_name);
    for (p = out; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '_';
        }
    }

    /* Pure-dot names map to the parent / current directory at the
	 * filesystem level; replace with the generic placeholder so
	 * the resulting g_build_filename can't escape upward. */
    if (g_strcmp0 (out, ".") == 0 || g_strcmp0 (out, "..") == 0) {
        g_free (out);
        return g_strdup ("download");
    }

    return out;
}
