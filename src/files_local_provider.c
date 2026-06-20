/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "config.h"

#include <gio/gio.h>
#include <glib.h>

#include "files_entry.h"
#include "files_provider.h"
#include "files_local_provider.h"
#include "prefs.h" /* gtkhx_prefs.download_path */

/* gi18n.h after the project headers — compat.h (pulled in via
 * prefs.h above) ships an identity-macro `_` for the no-gettext
 * case, and we want the real gettext expansion here. Same dance
 * as files_browser.c. */
#undef _
#include <glib/gi18n.h>

struct _HxLocalFilesProvider {
    GObject parent_instance;
    GListStore *listing; /* owns refs to HxFileEntry rows */
    char *current_path;
};

/* Signals live on the HxFilesProvider GInterface (see
 * files_provider.c); emission goes through g_signal_emit_by_name
 * so the dispatch finds the per-instance signal ID via GType
 * inheritance. */

static void
hx_local_files_provider_iface_init (HxFilesProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (
    HxLocalFilesProvider, hx_local_files_provider, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (HX_TYPE_FILES_PROVIDER,
                           hx_local_files_provider_iface_init))

static void
hx_local_files_provider_finalize (GObject *obj)
{
    HxLocalFilesProvider *self = HX_LOCAL_FILES_PROVIDER (obj);
    g_clear_object (&self->listing);
    g_free (self->current_path);
    G_OBJECT_CLASS (hx_local_files_provider_parent_class)->finalize (obj);
}

static void
hx_local_files_provider_class_init (HxLocalFilesProviderClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
    gobject_class->finalize = hx_local_files_provider_finalize;
}

static void
hx_local_files_provider_init (HxLocalFilesProvider *self)
{
    self->listing = g_list_store_new (HX_TYPE_FILE_ENTRY);
}

/* Sensible default starting directory. Prefer gtkhx_prefs.download_path
 * — that's where DnD downloads (and other transfer paths) land, so
 * having the Files browser start there means the user's drag-out
 * destination and the panel's current view agree by default. The
 * pref itself defaults to XDG_DOWNLOAD_DIR via changed_downloadpath
 * in options.c, so we get the XDG path as the effective initial
 * value without re-reading XDG here.
 *
 * Fall back to XDG → $HOME → "/" if the pref is unset / not a
 * directory (paranoia — changed_downloadpath should have set it by
 * the time we're called, but the first-run path has us building
 * widgets before options_apply runs, so the pref can be NULL or
 * "."). */
static char *
default_root (void)
{
    const char *dir;
    if (gtkhx_prefs.download_path && *gtkhx_prefs.download_path
        && g_file_test (gtkhx_prefs.download_path, G_FILE_TEST_IS_DIR)) {
        return g_strdup (gtkhx_prefs.download_path);
    }
    dir = g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);
    if (dir && g_file_test (dir, G_FILE_TEST_IS_DIR)) {
        return g_strdup (dir);
    }
    dir = g_get_home_dir ();
    if (dir && g_file_test (dir, G_FILE_TEST_IS_DIR)) {
        return g_strdup (dir);
    }
    return g_strdup ("/");
}

HxLocalFilesProvider *
hx_local_files_provider_new (const char *initial_path)
{
    HxLocalFilesProvider *self;

    self = g_object_new (HX_TYPE_LOCAL_FILES_PROVIDER, NULL);
    self->current_path
        = initial_path ? g_strdup (initial_path) : default_root ();

    /* Initial list happens on first show — caller can also fire
	 * navigate() / reload() immediately if it wants. We don't
	 * auto-list here to avoid emitting "navigated" before the
	 * caller has connected its handler. */
    return self;
}

GListModel *
hx_local_files_provider_get_listing (HxLocalFilesProvider *self)
{
    g_return_val_if_fail (HX_IS_LOCAL_FILES_PROVIDER (self), NULL);
    return G_LIST_MODEL (self->listing);
}

const char *
hx_local_files_provider_get_current_path (HxLocalFilesProvider *self)
{
    g_return_val_if_fail (HX_IS_LOCAL_FILES_PROVIDER (self), "");
    return self->current_path ? self->current_path : "";
}

const char *
hx_local_files_provider_get_label (HxLocalFilesProvider *self)
{
    (void)self;
    return _ ("Local");
}

/* Read `path` via GIO and replace listing's contents. Errors are
 * surfaced via the "error" signal. `path` must be absolute.
 *
 * Why blocking GIO rather than the async enumerate? Local file
 * enumeration in practice completes in <1 ms for normal dir sizes,
 * and async adds either a cancellable / completion-callback dance
 * or a g_idle_add round-trip. >10k-entry directories would want
 * incremental rendering — deferred. The remote backend already
 * runs async on the slow path. */
static void
do_list (HxLocalFilesProvider *self, const char *path)
{
    GFile *dir;
    GFileEnumerator *enumer;
    GFileInfo *info;
    GError *err = NULL;

    if (!path || !*path) {
        g_signal_emit_by_name (self, "error", _ ("No path to list"));
        return;
    }

    dir = g_file_new_for_path (path);
    enumer = g_file_enumerate_children (
        dir,
        G_FILE_ATTRIBUTE_STANDARD_NAME
        "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME
        "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE
        "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN
        "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE
        "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
        G_FILE_QUERY_INFO_NONE, NULL, &err);

    if (!enumer) {
        char *msg = g_strdup_printf (_ ("Can't read %1$s: %2$s"), path,
                                     err ? err->message : "unknown error");
        g_signal_emit_by_name (self, "error", msg);
        g_free (msg);
        g_clear_error (&err);
        g_object_unref (dir);
        return;
    }

    g_list_store_remove_all (self->listing);

    while ((info = g_file_enumerator_next_file (enumer, NULL, &err))) {
        const char *name;
        const char *content_type;
        GFileType type;
        guint64 size;
        gint64 mtime;
        gboolean is_dir, is_hidden;
        char *kind;
        HxFileEntry *entry;

        name = g_file_info_get_display_name (info);
        if (!name) {
            name = g_file_info_get_name (info);
        }
        type = g_file_info_get_file_type (info);
        is_dir = (type == G_FILE_TYPE_DIRECTORY);
        is_hidden = g_file_info_get_is_hidden (info);
        size = g_file_info_get_size (info);
        mtime = g_file_info_get_attribute_uint64 (
            info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        content_type = g_file_info_get_content_type (info);

        /* Skip dotfiles by default; matches most file managers'
		 * out-of-the-box behaviour. A "show hidden" toggle is a
		 * polish-item deferred. */
        if (is_hidden) {
            g_object_unref (info);
            continue;
        }

        if (is_dir) {
            kind = g_strdup (_ ("Folder"));
        } else if (content_type) {
            kind = g_content_type_get_description (content_type);
            if (!kind) {
                kind = g_strdup ("");
            }
        } else {
            kind = g_strdup ("");
        }

        /* icon_id 0 → hx_file_entry_new picks the generic folder
		 * vs. file icon based on is_dir. Polish-item: map the GIO
		 * content_type to richer icons (image/audio/archive) the
		 * same way the remote provider does for Hotline FourCCs. */
        entry = hx_file_entry_new (name, is_dir, size, (gint64)mtime, kind, 0);
        g_list_store_append (self->listing, entry);
        g_object_unref (entry);
        g_free (kind);
        g_object_unref (info);
    }

    if (err) {
        /* Partial-read error — keep what we got and tell the user. */
        char *msg
            = g_strdup_printf (_ ("Error reading %1$s: %2$s"), path, err->message);
        g_signal_emit_by_name (self, "error", msg);
        g_free (msg);
        g_clear_error (&err);
    }

    g_object_unref (enumer);
    g_object_unref (dir);

    g_signal_emit_by_name (self, "navigated", self->current_path);
}

void
hx_local_files_provider_navigate (HxLocalFilesProvider *self, const char *path)
{
    g_return_if_fail (HX_IS_LOCAL_FILES_PROVIDER (self));
    if (!path || !*path) {
        return;
    }

    g_free (self->current_path);
    self->current_path = g_strdup (path);
    do_list (self, self->current_path);
}

void
hx_local_files_provider_reload (HxLocalFilesProvider *self)
{
    g_return_if_fail (HX_IS_LOCAL_FILES_PROVIDER (self));
    do_list (self, self->current_path);
}

void
hx_local_files_provider_navigate_up (HxLocalFilesProvider *self)
{
    GFile *cur, *parent;
    char *parent_path;

    g_return_if_fail (HX_IS_LOCAL_FILES_PROVIDER (self));
    cur = g_file_new_for_path (self->current_path);
    parent = g_file_get_parent (cur);
    g_object_unref (cur);
    if (!parent) {
        return; /* already at root */
    }

    parent_path = g_file_get_path (parent);
    g_object_unref (parent);
    if (!parent_path) {
        return;
    }

    hx_local_files_provider_navigate (self, parent_path);
    g_free (parent_path);
}

/* Build an absolute child path from current_path + name without
 * needing GFile for what's mostly a string-join. */
static char *
child_path (HxLocalFilesProvider *self, const char *name)
{
    if (!self->current_path) {
        return NULL;
    }
    return g_build_filename (self->current_path, name, NULL);
}

gboolean
hx_local_files_provider_mkdir (HxLocalFilesProvider *self, const char *name,
                               GError **err)
{
    GFile *f;
    gboolean ok;
    char *path;

    g_return_val_if_fail (HX_IS_LOCAL_FILES_PROVIDER (self), FALSE);
    if (!name || !*name) {
        return FALSE;
    }
    path = child_path (self, name);
    if (!path) {
        return FALSE;
    }
    f = g_file_new_for_path (path);
    ok = g_file_make_directory (f, NULL, err);
    g_object_unref (f);
    g_free (path);
    if (ok) {
        hx_local_files_provider_reload (self);
    }
    return ok;
}

gboolean
hx_local_files_provider_delete (HxLocalFilesProvider *self, const char *name,
                                GError **err)
{
    GFile *f;
    gboolean ok;
    char *path;

    g_return_val_if_fail (HX_IS_LOCAL_FILES_PROVIDER (self), FALSE);
    if (!name || !*name) {
        return FALSE;
    }
    path = child_path (self, name);
    if (!path) {
        return FALSE;
    }
    f = g_file_new_for_path (path);
    /* g_file_delete is non-recursive — directories must be empty.
	 * Recursive / trash variants deferred. */
    ok = g_file_delete (f, NULL, err);
    g_object_unref (f);
    g_free (path);
    if (ok) {
        hx_local_files_provider_reload (self);
    }
    return ok;
}

gboolean
hx_local_files_provider_rename (HxLocalFilesProvider *self,
                                const char *old_name, const char *new_name,
                                GError **err)
{
    GFile *src, *dst;
    gboolean ok;
    char *src_path, *dst_path;

    g_return_val_if_fail (HX_IS_LOCAL_FILES_PROVIDER (self), FALSE);
    if (!old_name || !new_name) {
        return FALSE;
    }
    src_path = child_path (self, old_name);
    dst_path = child_path (self, new_name);
    src = g_file_new_for_path (src_path);
    dst = g_file_new_for_path (dst_path);
    ok = g_file_move (src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, err);
    g_object_unref (src);
    g_object_unref (dst);
    g_free (src_path);
    g_free (dst_path);
    if (ok) {
        hx_local_files_provider_reload (self);
    }
    return ok;
}

/* ---- HxFilesProvider interface implementation ---- */

/* Forwarders. The concrete typed functions are kept public so
 * existing callers don't need to change; the interface vtable
 * goes through these thin shims. */

static GListModel *
iface_get_listing (HxFilesProvider *self)
{
    return hx_local_files_provider_get_listing (HX_LOCAL_FILES_PROVIDER (self));
}

static const char *
iface_get_current_path (HxFilesProvider *self)
{
    return hx_local_files_provider_get_current_path (
        HX_LOCAL_FILES_PROVIDER (self));
}

static const char *
iface_get_label (HxFilesProvider *self)
{
    return hx_local_files_provider_get_label (HX_LOCAL_FILES_PROVIDER (self));
}

static void
iface_navigate (HxFilesProvider *self, const char *path)
{
    hx_local_files_provider_navigate (HX_LOCAL_FILES_PROVIDER (self), path);
}

static void
iface_reload (HxFilesProvider *self)
{
    hx_local_files_provider_reload (HX_LOCAL_FILES_PROVIDER (self));
}

static void
iface_navigate_up (HxFilesProvider *self)
{
    hx_local_files_provider_navigate_up (HX_LOCAL_FILES_PROVIDER (self));
}

static gboolean
iface_mkdir (HxFilesProvider *self, const char *name, GError **err)
{
    return hx_local_files_provider_mkdir (HX_LOCAL_FILES_PROVIDER (self), name,
                                          err);
}

static gboolean
iface_delete_entry (HxFilesProvider *self, const char *name, GError **err)
{
    return hx_local_files_provider_delete (HX_LOCAL_FILES_PROVIDER (self), name,
                                           err);
}

static gboolean
iface_rename (HxFilesProvider *self, const char *old_name, const char *new_name,
              GError **err)
{
    return hx_local_files_provider_rename (HX_LOCAL_FILES_PROVIDER (self),
                                           old_name, new_name, err);
}

/* Activate a local file: hand it to the desktop's default app
 * via the standard portal-aware GAppInfo launcher. Folder
 * activation never reaches here — the panel intercepts and
 * navigates instead. */
static void
iface_activate_entry (HxFilesProvider *self, HxFileEntry *e)
{
    HxLocalFilesProvider *r = HX_LOCAL_FILES_PROVIDER (self);
    const char *dir;
    char *abspath;
    GFile *f;
    char *uri;
    GError *err = NULL;

    if (!e || hx_file_entry_is_dir (e)) {
        return;
    }
    dir = hx_local_files_provider_get_current_path (r);
    abspath
        = g_build_filename (dir ? dir : "/", hx_file_entry_get_name (e), NULL);
    f = g_file_new_for_path (abspath);
    uri = g_file_get_uri (f);
    g_object_unref (f);
    g_free (abspath);

    if (!uri) {
        return;
    }

    /* g_app_info_launch_default_for_uri spins up the system's
	 * registered handler (the same one a "Open" right-click in
	 * GNOME Files would use). NULL launch context = default
	 * environment. Errors get logged but not surfaced — the
	 * panel doesn't have a toast hook reaching down here yet. */
    if (!g_app_info_launch_default_for_uri (uri, NULL, &err)) {
        g_warning ("launch %s: %s", uri, err ? err->message : "unknown");
        g_clear_error (&err);
    }
    g_free (uri);
}

static void
hx_local_files_provider_iface_init (HxFilesProviderInterface *iface)
{
    iface->get_listing = iface_get_listing;
    iface->get_current_path = iface_get_current_path;
    iface->get_label = iface_get_label;
    iface->navigate = iface_navigate;
    iface->reload = iface_reload;
    iface->navigate_up = iface_navigate_up;
    iface->mkdir = iface_mkdir;
    iface->delete_entry = iface_delete_entry;
    iface->rename = iface_rename;
    iface->get_unavailable_reason = NULL; /* local is always ready */
    iface->activate_entry = iface_activate_entry;
}
