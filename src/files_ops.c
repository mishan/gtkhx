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
        return _ ("Hotline has no server-side copy. Use Move (F6) to "
                  "relocate the file, or drag to your local panel first.");
    case HX_OPS_ERR_FOLDER_UNSUPPORTED:
        /* Local→remote and local→local folder copies work now; only
		 * the remote→local download direction still falls through
		 * here. The message stays generic in case future paths add
		 * back into this enum. */
        return _ ("Folder downloads aren't supported yet — pick individual "
                  "files.");
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

/* Recursive upload of a local directory tree to the Hotline
 * server. Walks `local_path` with a GFileEnumerator and, for
 * each child, either recurses or fires hx_put_file. The remote
 * directory at `remote_path` is created first via hx_make_dir;
 * the server queues the operations in order so files arriving
 * just after the mkdir land correctly.
 *
 * Returns TRUE if the walker completed without local-side I/O
 * errors. The wire-side success of each individual mkdir/put is
 * fire-and-forget — failures surface through the toolbar's
 * existing task_error toast. */
static gboolean
upload_local_dir_recursive (const char *local_path, const char *remote_path)
{
    GFile *src = g_file_new_for_path (local_path);
    GFileEnumerator *en;
    GFileInfo *info;
    GError *err = NULL;
    gboolean all_ok = TRUE;

    /* Make the remote dir first (server: idempotent — recreating
	 * an existing dir is a no-op error we don't surface). */
    {
        char *tmp = g_strdup (remote_path);
        hx_make_dir (&the_session.htlc, tmp);
        g_free (tmp);
    }

    en = g_file_enumerate_children (
        src, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NONE, NULL, &err);
    if (!en) {
        g_warning ("upload walk %s: %s", local_path, err ? err->message : "?");
        g_clear_error (&err);
        g_object_unref (src);
        return FALSE;
    }

    while ((info = g_file_enumerator_next_file (en, NULL, &err))) {
        const char *name = g_file_info_get_name (info);
        GFileType type = g_file_info_get_file_type (info);
        char *child_local = g_build_filename (local_path, name, NULL);
        char *child_remote = join_path (remote_path, name);

        if (type == G_FILE_TYPE_DIRECTORY) {
            if (!upload_local_dir_recursive (child_local, child_remote)) {
                all_ok = FALSE;
            }
        } else {
            hx_put_file (&the_session.htlc, child_local, child_remote);
        }

        g_free (child_local);
        g_free (child_remote);
        g_object_unref (info);
    }

    if (err) {
        g_warning ("upload walk %s: %s", local_path, err->message);
        g_clear_error (&err);
        all_ok = FALSE;
    }

    g_object_unref (en);
    g_object_unref (src);
    return all_ok;
}

/* local → remote upload. For files: hx_put_file. For folders:
 * recursive walker that mkdirs the remote target then queues a
 * hx_put_file per file. Hotline servers process operations in
 * arrival order so the per-folder mkdir always lands before its
 * children's uploads.
 *
 * The Hotline wire wants an absolute remote path; we synthesise
 * it from the dst provider's current path + the source entry's
 * name. */
static HxOpsResult
copy_local_to_remote (HxFilesProvider *src, HxFilesProvider *dst,
                      HxFileEntry *e)
{
    const char *src_dir, *dst_dir;
    char *lpath, *rpath;
    gboolean is_dir;

    if (!the_session.htlc.fd) {
        return HX_OPS_ERR_NOT_CONNECTED;
    }
    if (!has_access (HL_ACCESS_UPLOAD_FILES)) {
        return HX_OPS_ERR_NO_PERMISSION;
    }
    is_dir = hx_file_entry_is_dir (e);
    if (is_dir && !has_access (HL_ACCESS_UPLOAD_FOLDERS)) {
        return HX_OPS_ERR_NO_PERMISSION;
    }

    src_dir = hx_files_provider_get_current_path (src);
    dst_dir = hx_files_provider_get_current_path (dst);
    lpath = join_path (src_dir, hx_file_entry_get_name (e));
    rpath = join_path (dst_dir, hx_file_entry_get_name (e));

    if (is_dir) {
        gboolean ok = upload_local_dir_recursive (lpath, rpath);
        g_free (lpath);
        g_free (rpath);
        return ok ? HX_OPS_OK : HX_OPS_ERR_LOCAL_FAIL;
    }

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
        /* Folder downloads need a state-machine that drives
		 * HTLC_HDR_FILE_LIST recursively and queues xfer_new
		 * for each file under a local-prefix subtree. The
		 * legacy infrastructure in rcv.c::rcv_task_file_list's
		 * COMPLETE_GET_R branch is half-baked for this — it
		 * mkdirs relative to cwd, which puts files in the
		 * wrong place under Flatpak. A follow-up commit will
		 * rewire it to honour the user's download_path pref.
		 * For now, single files only.*/
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

/* Recursive copy of a local directory tree via GIO. Walks `src`
 * with a GFileEnumerator and, for each child, either recurses
 * into a subdirectory or g_file_copy's the file. Synchronous —
 * runs on the main thread, fine for the byte sizes Hotline
 * downloads usually weigh.
 *
 * Returns TRUE iff every file copied cleanly. On any failure we
 * still walk to completion (gives the user partial results
 * rather than abandoning mid-walk) but the return value is FALSE
 * so the caller's toast reads as a failure. */
static gboolean
copy_local_dir_recursive (GFile *src_dir, GFile *dst_dir, GError **err_out)
{
    GFileEnumerator *en;
    GFileInfo *info;
    GError *err = NULL;
    gboolean all_ok = TRUE;

    /* Make the destination directory. Pre-existing is OK — the
	 * caller may have created it already. */
    if (!g_file_make_directory_with_parents (dst_dir, NULL, &err)) {
        if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
            if (err_out && !*err_out) {
                *err_out = err;
            } else {
                g_error_free (err);
            }
            return FALSE;
        }
        g_clear_error (&err);
    }

    en = g_file_enumerate_children (src_dir,
                                    G_FILE_ATTRIBUTE_STANDARD_NAME
                                    "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                    G_FILE_QUERY_INFO_NONE, NULL, &err);
    if (!en) {
        if (err_out && !*err_out) {
            *err_out = err;
        } else {
            g_clear_error (&err);
        }
        return FALSE;
    }

    while ((info = g_file_enumerator_next_file (en, NULL, &err))) {
        const char *name = g_file_info_get_name (info);
        GFileType type = g_file_info_get_file_type (info);
        GFile *child_src, *child_dst;

        child_src = g_file_get_child (src_dir, name);
        child_dst = g_file_get_child (dst_dir, name);

        if (type == G_FILE_TYPE_DIRECTORY) {
            if (!copy_local_dir_recursive (child_src, child_dst, err_out)) {
                all_ok = FALSE;
            }
        } else {
            GError *cerr = NULL;
            if (!g_file_copy (child_src, child_dst, G_FILE_COPY_NONE, NULL,
                              NULL, NULL, &cerr)) {
                g_warning ("local copy: %s", cerr ? cerr->message : "?");
                if (err_out && !*err_out && cerr) {
                    *err_out = cerr;
                } else {
                    g_clear_error (&cerr);
                }
                all_ok = FALSE;
            }
        }

        g_object_unref (child_src);
        g_object_unref (child_dst);
        g_object_unref (info);
    }

    /* Enumerator EOF — `err` from next_file is NULL on normal
	 * termination, set on IO failure. */
    if (err) {
        if (err_out && !*err_out) {
            *err_out = err;
        } else {
            g_clear_error (&err);
        }
        all_ok = FALSE;
    }

    g_object_unref (en);
    return all_ok;
}

/* local → local via GIO. For files: g_file_copy. For folders:
 * recursive walker that recreates the tree under dst. Blocking,
 * but fast enough for the user to not notice unless the file is
 * huge. Async progress UI is a Phase 4 polish item. */
static HxOpsResult
copy_local_to_local (HxFilesProvider *src, HxFilesProvider *dst, HxFileEntry *e)
{
    const char *src_dir, *dst_dir;
    char *spath, *dpath;
    GFile *sf, *df;
    GError *err = NULL;
    gboolean ok;

    src_dir = hx_files_provider_get_current_path (src);
    dst_dir = hx_files_provider_get_current_path (dst);
    spath = join_path (src_dir, hx_file_entry_get_name (e));
    dpath = join_path (dst_dir, hx_file_entry_get_name (e));

    sf = g_file_new_for_path (spath);
    df = g_file_new_for_path (dpath);

    if (hx_file_entry_is_dir (e)) {
        ok = copy_local_dir_recursive (sf, df, &err);
    } else {
        ok = g_file_copy (sf, df, G_FILE_COPY_NONE, NULL, NULL, NULL, &err);
    }
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

    /* Remote → remote: Hotline has no FILE_COPY opcode. The
	 * server-side SYMLINK opcode creates a HARD link (shared
	 * bytes), which isn't a real copy — modifying or deleting
	 * one path affects the other. We don't pretend otherwise.
	 * Drag-and-drop between two remote panels routes through
	 * hx_file_move in files_browser.c instead (orthodox-FM
	 * "drag-within-same-volume = move" convention). The Copy
	 * button between two remote panels falls through here and
	 * the toast points the user at Move. */
    return HX_OPS_ERR_UNSUPPORTED;
}
