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
#include <string.h>

#include "hx.h"
#include "session.h"
#include "hl_access.h"
#include "files.h"
#include "xfers.h"
#include "files_entry.h"
#include "files_provider.h"
#include "files_local_provider.h"
#include "files_remote_provider.h"
#include "files_ops.h"

#undef _
#include <glib/gi18n.h>

const char *
hx_files_ops_result_message (HxOpsResult r)
{
    switch (r) {
    case HX_OPS_OK:
        return _ ("Copied.");
    case HX_OPS_ERR_NO_SOURCE:
        return _ ("Nothing to copy.");
    case HX_OPS_ERR_NO_TARGET:
        return _ ("No target panel.");
    case HX_OPS_ERR_NOT_CONNECTED:
        return _ ("Not connected to a server.");
    case HX_OPS_ERR_NO_PERMISSION:
        return _ ("You don't have permission for that.");
    case HX_OPS_ERR_UNSUPPORTED:
        return _ ("Copying directly between two remote locations isn't "
                  "supported yet.");
    case HX_OPS_ERR_FOLDER_UNSUPPORTED:
        return _ (
            "Folder copies aren't supported yet — pick individual files.");
    case HX_OPS_ERR_LOCAL_FAIL:
        return _ ("Local copy failed.");
    }
    return _ ("Copy failed.");
}

/* Side-detection helpers. Each provider implements either
 * HxLocalFilesProvider or HxRemoteFilesProvider; both implement
 * HxFilesProvider. The browser only knows abstract providers, so
 * we re-discover the concrete type here to dispatch the right
 * transfer machinery. */
static gboolean
provider_is_local (HxFilesProvider *p)
{
    return p && HX_IS_LOCAL_FILES_PROVIDER (p);
}

static gboolean
provider_is_remote (HxFilesProvider *p)
{
    return p && HX_IS_REMOTE_FILES_PROVIDER (p);
}

/* Compose a path that joins a directory with an entry name using
 * the appropriate separator for the side. Hotline always uses
 * '/'; local paths are POSIX so '/' is fine too. Caller frees. */
static char *
join_path (const char *dir, const char *name)
{
    if (!dir || !*dir) {
        return g_strdup (name ? name : "");
    }
    if (!name || !*name) {
        return g_strdup (dir);
    }
    if (g_strcmp0 (dir, "/") == 0) {
        return g_strdup_printf ("/%s", name);
    }
    return g_strdup_printf ("%s/%s", dir, name);
}

/* htlc->access is a 64-bit big-endian bitmap; the helper in
 * hl_access.h reads it as a byte array. */
static gboolean
has_access (int bit)
{
    const guint8 *bits = (const guint8 *)&the_session.htlc.access;
    return hl_access_has (bits, bit);
}

/* ---- Per-direction handlers ---- */

/* local → remote upload. The Hotline wire wants an absolute remote
 * path; we synthesise it from the dst provider's current path +
 * the source entry's name. hx_put_file is the existing wrapper
 * for the XFER_PUT side of xfer_new. */
static HxOpsResult
copy_local_to_remote (HxFilesProvider *src, HxFilesProvider *dst,
                      HxFileEntry *e)
{
    const char *src_dir, *dst_dir;
    char *lpath, *rpath;

    if (!the_session.htlc.fd) {
        return HX_OPS_ERR_NOT_CONNECTED;
    }
    if (!has_access (HL_ACCESS_UPLOAD_FILES)) {
        return HX_OPS_ERR_NO_PERMISSION;
    }
    if (hx_file_entry_is_dir (e)) {
        return HX_OPS_ERR_FOLDER_UNSUPPORTED;
    }

    src_dir = hx_files_provider_get_current_path (src);
    dst_dir = hx_files_provider_get_current_path (dst);
    lpath = join_path (src_dir, hx_file_entry_get_name (e));
    rpath = join_path (dst_dir, hx_file_entry_get_name (e));

    hx_put_file (&the_session.htlc, lpath, rpath);

    g_free (lpath);
    g_free (rpath);
    return HX_OPS_OK;
}

/* remote → local download. xfer_new wants the server-side file
 * size up front (resume / rename decisions key off it) — we
 * stashed it on HxFileEntry at parse time. */
static HxOpsResult
copy_remote_to_local (HxFilesProvider *src, HxFilesProvider *dst,
                      HxFileEntry *e)
{
    const char *src_dir, *dst_dir;
    char *lpath;
    struct htxf_conn *htxf;
    guint64 size;

    if (!the_session.htlc.fd) {
        return HX_OPS_ERR_NOT_CONNECTED;
    }
    if (!has_access (HL_ACCESS_DOWNLOAD_FILES)) {
        return HX_OPS_ERR_NO_PERMISSION;
    }
    if (hx_file_entry_is_dir (e)) {
        return HX_OPS_ERR_FOLDER_UNSUPPORTED;
    }

    src_dir = hx_files_provider_get_current_path (src);
    dst_dir = hx_files_provider_get_current_path (dst);
    lpath = join_path (dst_dir, hx_file_entry_get_name (e));

    size = hx_file_entry_get_size (e);
    /* xfer_new takes the remote location as (dir, name, name_len)
	 * so the name's bytes (possibly including '/') ride through to
	 * the wire FILE_NAME chunk untouched. */
    {
        const char *nm = hx_file_entry_get_name (e);
        gsize nm_len = nm ? strlen (nm) : 0;
        htxf = xfer_new (lpath, src_dir ? src_dir : "", nm, nm_len, XFER_GET, 0,
                         (guint32)size);
    }
    if (htxf) {
        htxf->filter_argv = 0;
        htxf->opt.retry = 0;
    }

    g_free (lpath);
    return htxf ? HX_OPS_OK : HX_OPS_ERR_LOCAL_FAIL;
}

/* local → local via GIO. Blocking, but fast enough for the user
 * to not notice unless the file is huge. Async progress UI is a
 * Phase 4 polish item. */
static HxOpsResult
copy_local_to_local (HxFilesProvider *src, HxFilesProvider *dst, HxFileEntry *e)
{
    const char *src_dir, *dst_dir;
    char *spath, *dpath;
    GFile *sf, *df;
    GError *err = NULL;
    gboolean ok;

    if (hx_file_entry_is_dir (e)) {
        return HX_OPS_ERR_FOLDER_UNSUPPORTED;
    }

    src_dir = hx_files_provider_get_current_path (src);
    dst_dir = hx_files_provider_get_current_path (dst);
    spath = join_path (src_dir, hx_file_entry_get_name (e));
    dpath = join_path (dst_dir, hx_file_entry_get_name (e));

    sf = g_file_new_for_path (spath);
    df = g_file_new_for_path (dpath);
    ok = g_file_copy (sf, df, G_FILE_COPY_NONE, NULL, NULL, NULL, &err);
    if (!ok && err) {
        g_warning ("local copy %s → %s: %s", spath, dpath, err->message);
        g_clear_error (&err);
    }
    g_object_unref (sf);
    g_object_unref (df);
    g_free (spath);
    g_free (dpath);

    if (ok) {
        hx_files_provider_reload (dst);
    }
    return ok ? HX_OPS_OK : HX_OPS_ERR_LOCAL_FAIL;
}

HxOpsResult
hx_files_ops_copy (HxFilesProvider *src, HxFilesProvider *dst, HxFileEntry *e)
{
    if (!e) {
        return HX_OPS_ERR_NO_SOURCE;
    }
    if (!src) {
        return HX_OPS_ERR_NO_SOURCE;
    }
    if (!dst) {
        return HX_OPS_ERR_NO_TARGET;
    }

    if (provider_is_local (src) && provider_is_remote (dst)) {
        return copy_local_to_remote (src, dst, e);
    }
    if (provider_is_remote (src) && provider_is_local (dst)) {
        return copy_remote_to_local (src, dst, e);
    }
    if (provider_is_local (src) && provider_is_local (dst)) {
        return copy_local_to_local (src, dst, e);
    }

    /* Both remote — unsupported in Phase 3. */
    return HX_OPS_ERR_UNSUPPORTED;
}
