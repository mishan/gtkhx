/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "config.h"

#include <glib.h>
#include <string.h>
#include <arpa/inet.h>

#include "hx.h"
/* hx.h pulls compat.h which defines _(s) as a passthrough; undef
 * before gi18n.h gives us the proper gettext() expansion without
 * the redefine warning. */
#undef _
#include <glib/gi18n.h>
#include "session.h"
#include "hotline.h"
#include "hl_access.h"
#include "hotline_proto.h"
#include "network.h"
#include "tasks.h"
#include "rcv.h"
#include "files.h"
#include "xfers.h"
#include "prefs.h"
#include "gtkutil.h"
#include "files_entry.h"
#include "files_provider.h"
#include "files_remote_provider.h"
#include "filelist_walker.h"

struct _HxRemoteFilesProvider {
    GObject parent_instance;
    GListStore *listing;
    char *current_path; /* Hotline-style; "/" at root */
    /* TRUE if the most recent FILE_LIST RPC came back as a task
	 * error (server denied the listing). Cleared on the next
	 * successful listing. Drives the panel's empty-state hint
	 * ("Folder is upload-only" if the access bits also indicate
	 * a drop-box, "Can't list this folder" otherwise) — without
	 * this the user just sees an empty panel and has no idea why
	 * the navigation didn't produce rows. */
    gboolean listing_error;
};

static void
hx_remote_files_provider_iface_init (HxFilesProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (
    HxRemoteFilesProvider, hx_remote_files_provider, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (HX_TYPE_FILES_PROVIDER,
                           hx_remote_files_provider_iface_init))

/* Pending fetches keyed on the cached_filelist* we hand to
 * hx_list_dir's underlying task. The signal carrier (`data` slot
 * in the file-list signal payload) is a HxRemoteFilesProvider* —
 * we look it up here to confirm it's still alive and the fetch
 * was ours.
 *
 * Keying on the provider pointer (not the cfl) is enough because
 * a provider can only have one fetch in flight at a time —
 * reload during a fetch just supersedes the previous request. */
static GHashTable *pending_listings = NULL; /* provider* → reffed provider* */

static void
ensure_pending_table (void)
{
    if (!pending_listings) {
        pending_listings = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                  NULL, g_object_unref);
    }
}

static void
hx_remote_files_provider_finalize (GObject *obj)
{
    HxRemoteFilesProvider *self = HX_REMOTE_FILES_PROVIDER (obj);
    g_clear_object (&self->listing);
    g_free (self->current_path);
    /* If we're in pending_listings, the table holds the ref that's
	 * being dropped now — this finalize was called BECAUSE the
	 * table released us. So no remove call here. */
    G_OBJECT_CLASS (hx_remote_files_provider_parent_class)->finalize (obj);
}

static void
hx_remote_files_provider_class_init (HxRemoteFilesProviderClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = hx_remote_files_provider_finalize;
}

static void
hx_remote_files_provider_init (HxRemoteFilesProvider *self)
{
    self->listing = g_list_store_new (HX_TYPE_FILE_ENTRY);
    self->current_path = g_strdup ("/");
}

HxRemoteFilesProvider *
hx_remote_files_provider_new (void)
{
    return g_object_new (HX_TYPE_REMOTE_FILES_PROVIDER, NULL);
}

/* ---- Connection-state hooks ----
 *
 * The provider is created up-front (before login) so it can sit
 * alongside the local one in the browser. While disconnected it
 * reports an unavailable reason; on login the browser refreshes
 * and lists the root. */

static const char *
remote_get_unavailable_reason (HxFilesProvider *self)
{
    (void)self;
    if (!the_session.htlc.fd) {
        return _ ("Not connected to a server.");
    }
    /* htlc->fd is set as soon as the TCP socket comes up — well
     * before the spec-correct "fully joined" boundary the server
     * uses to gate post-login RPCs (USER_GETLIST, FILE_LIST,
     * chat-history catch-up). If we let the panel reload between
     * those two points, the resulting HTLC_HDR_FILE_LIST lands at
     * the server before our AGREEMENTAGREE and trips a "not yet
     * joined" disconnect on stricter 1.5+ servers. The flag is
     * raised in rcv.c::hx_post_login_fetches and reset in
     * hx_htlc_close. */
    if (!the_session.htlc.flags.post_login_fetched) {
        return _ ("Logging in…");
    }
    return NULL;
}

/* ---- Wire send ----
 *
 * Fires HTLC_HDR_FILE_LIST without going through the legacy
 * hx_list_dir, which is tightly coupled to the gfile_list bookkeeping.
 * We allocate our own cached_filelist (matches the legacy task
 * signature: rcv_task_file_list dereferences cfl->path) and pass
 * the HxRemoteFilesProvider* as the signal data carrier. */
static void
remote_send_file_list (HxRemoteFilesProvider *self, const char *path)
{
    struct cached_filelist *cfl;
    guint16 hldirlen;
    guint8 *hldir;

    if (!the_session.htlc.fd) {
        return;
    }

    ensure_pending_table ();

    cfl = g_malloc0 (sizeof (struct cached_filelist));
    cfl->path = g_strdup (path && *path ? path : "/");

    /* Reffed entry — keeps the provider alive while the RPC is
	 * in flight even if the browser closes. Drops on remove. */
    g_hash_table_insert (pending_listings, self, g_object_ref (self));

    hldir = path_to_hldir (cfl->path, &hldirlen, 0);

    /* chunk layout moved to gtkhx_proto_build_file_list_chunks.
	 * Build BEFORE task_new — see hx_send_msg for the rationale. */
    struct hx_chunk chunks[1];
    int hc = (int)gtkhx_proto_build_file_list_chunks (
        hldir, hldirlen, chunks, G_N_ELEMENTS (chunks));
    if (hc > 0) {
        task_new (&the_session.htlc, RCV_TASK_FN (rcv_task_file_list), cfl,
                  self, "ls");
        hlwrite_chunks (&the_session.htlc, HTLC_HDR_FILE_LIST, 0, chunks, hc);
    }
    g_free (hldir);
}

/* ---- Reply: parse hl_filelist_hdr chunks into HxFileEntry ----
 *
 * The wire walk (each chunk's len-prefixed step + ntohl of the
 * fixed fields) lives in src/filelist_walker.c so the Tier 2 test
 * can exercise it without dragging in this TU's GTK pile. The
 * callback below is the model-binding half: takes the decoded
 * (ftype, fsize, name) and appends an HxFileEntry to the
 * provider's GListStore. */
static void
populate_from_chunks_cb (guint32 ftype, guint32 fsize, const guint8 *name,
                         gsize name_len, void *user_data)
{
    HxRemoteFilesProvider *self = (HxRemoteFilesProvider *)user_data;
    char namebuf[256];
    char *utf8;
    gboolean is_dir;
    HxFileEntry *entry;
    guint32 ftype_be;

    if (name_len > sizeof (namebuf) - 1) {
        name_len = sizeof (namebuf) - 1;
    }
    memcpy (namebuf, name, name_len);
    namebuf[name_len] = '\0';

    /* Hotline filename bytes can be Mac Roman on older servers —
	 * sanitise to UTF-8 for display. */
    utf8 = gtkhx_text_to_utf8 (namebuf, name_len, NULL);

    is_dir = (ftype == 0x666c6472); /* 'fldr' */

    /* Friendly kind label and icon both want the raw big-endian
	 * FourCC bytes (not the host-order ftype int) — they
	 * memcmp() against "JPEG" / "fldr" / etc. literals. Stash a
	 * htonl back into a local for that. */
    ftype_be = htonl (ftype);
    gboolean kind_static = FALSE;
    const char *kind = kind_of_ftype ((const char *)&ftype_be, &kind_static);

    /* For folders, Hotline puts the child count in the size field
	 * rather than a byte count — we keep it (rather than zeroing
	 * it out) so the Size column renders "(N items)" instead of
	 * just "—". hx_file_entry_format_size branches on is_dir to
	 * pick the right wording. */
    entry = hx_file_entry_new (
        utf8 ? utf8 : namebuf, is_dir, (guint64)fsize,
        0, /* no mtime on the wire */
        kind,
        icon_of_ftype_and_name ((const char *)&ftype_be, namebuf, name_len));
    g_list_store_append (self->listing, entry);
    g_object_unref (entry);
    g_free (utf8);
    if (!kind_static) {
        g_free ((char *)kind);
    }
}

static void
populate_from_chunks (HxRemoteFilesProvider *self, struct cached_filelist *cfl)
{
    g_list_store_remove_all (self->listing);

    if (!cfl || !cfl->fh || !cfl->fhlen) {
        return;
    }

    hl_filelist_walk (cfl->fh, cfl->fhlen, populate_from_chunks_cb, self);
}

gboolean
hx_remote_files_provider_handle_file_list (gpointer cfl_p, gpointer fh,
                                           gpointer data)
{
    HxRemoteFilesProvider *self;
    struct cached_filelist *cfl = cfl_p;
    (void)fh;

    /* The dispatcher in gtkhx.c::on_file_list_signal falls through
	 * to the legacy output_file_list (which casts `data` to
	 * struct gfile_list *) when we return FALSE. That's only safe
	 * if data ISN'T a HxRemoteFilesProvider — otherwise the cast
	 * misreads a GObject as a gfile_list and crashes inside
	 * gtk_window_set_title on a bogus window pointer.
	 *
	 * Identify by type first. GObject's type check is safe on any
	 * pointer that could be either flavour. When this provider DOES
	 * own the response, claim it whether or not it's still in
	 * pending_listings — a second response for the same provider
	 * (e.g. when the panel fired multiple FILE_LIST requests in
	 * quick succession) used to fall through to the legacy path,
	 * which was the source of the crash. Now we just drop the
	 * duplicate harmlessly. */
    if (!data || !G_IS_OBJECT (data) || !HX_IS_REMOTE_FILES_PROVIDER (data)) {
        return FALSE;
    }
    if (!pending_listings || !g_hash_table_contains (pending_listings, data)) {
        /* Stale response for one of our providers (most recent
		 * request already handled, or this fired before any
		 * pending entry existed). Swallow it so the legacy
		 * output_file_list isn't called on a GObject pointer. */
        return TRUE;
    }

    /* It's ours and still tracked. Steal the ref so we don't get
	 * dropped mid-parse if the table removes us first. */
    self = g_object_ref (HX_REMOTE_FILES_PROVIDER (data));
    g_hash_table_remove (pending_listings, data);

    populate_from_chunks (self, cfl);

    /* A successful response clears any sticky listing-error state
	 * from a previous failed navigation. */
    self->listing_error = FALSE;

    /* Adopt the new path as the current one (the RPC was fired
	 * with this path in cfl_path — if a second fetch superseded
	 * the first, the more-recent one wins via pending_listings's
	 * single-entry-per-provider invariant). */
    if (cfl && cfl->path) {
        g_free (self->current_path);
        self->current_path = g_strdup (cfl->path);
    }

    g_signal_emit_by_name (self, "navigated", self->current_path);

    /* The cached_filelist was allocated by us in remote_send_file_list
	 * and isn't tracked by the legacy cfl_lookup table — free it.
	 * The fh data inside it points into the receive buffer and is
	 * owned by the rcv path; we don't free fh. */
    if (cfl) {
        g_free (cfl->path);
        g_free (cfl);
    }

    g_object_unref (self);
    return TRUE;
}

/* Error counterpart to handle_file_list. Called from
 * rcv.c::rcv_task_file_list's task_inerror short-circuit so the
 * provider knows its in-flight listing was denied — without this
 * the panel sat showing nothing with no idea why.
 *
 * Behaviour matches the success path's cleanup: remove the
 * provider from pending_listings (drops the table's ref), clear
 * the listing rows (so any old content from a previous folder
 * doesn't linger on the new path), and flip listing_error TRUE so
 * the panel's status footer can show a contextual message instead
 * of "0 items". Emits "navigated" with the current path — the
 * panel's existing on_navigated handler then updates the path
 * entry and refreshes the footer through update_status. */
gboolean
hx_remote_files_provider_handle_file_list_error (gpointer cfl_p, gpointer data)
{
    HxRemoteFilesProvider *self;
    struct cached_filelist *cfl = cfl_p;

    if (!data || !G_IS_OBJECT (data) || !HX_IS_REMOTE_FILES_PROVIDER (data)) {
        return FALSE;
    }
    if (!pending_listings || !g_hash_table_contains (pending_listings, data)) {
        return TRUE;
    }

    self = g_object_ref (HX_REMOTE_FILES_PROVIDER (data));
    g_hash_table_remove (pending_listings, data);

    g_list_store_remove_all (self->listing);
    self->listing_error = TRUE;

    /* The cfl we allocated in remote_send_file_list carries the
	 * path the user navigated to. Adopt it as the current path
	 * even though the listing failed — otherwise the next
	 * navigate_up has nothing to walk back from. */
    if (cfl && cfl->path) {
        g_free (self->current_path);
        self->current_path = g_strdup (cfl->path);
    }

    g_signal_emit_by_name (self, "navigated", self->current_path);

    /* cfl is owned by rcv.c's caller; we don't free it here.
	 * The success path's twin (handle_file_list above) does
	 * free cfl because rcv_task_file_list's success arm
	 * surrenders ownership to cfl_print → us. The error arm
	 * keeps ownership upstream. */

    g_object_unref (self);
    return TRUE;
}

/* Getter for the panel to consult when building empty-state
 * messaging. TRUE iff the most recent FILE_LIST RPC failed. */
gboolean
hx_remote_files_provider_has_listing_error (HxRemoteFilesProvider *self)
{
    return self ? self->listing_error : FALSE;
}

void
hx_remote_files_provider_reset_to_root (HxRemoteFilesProvider *self)
{
    if (!self) {
        return;
    }
    g_list_store_remove_all (self->listing);
    self->listing_error = FALSE;
    /* Return to the server root. The provider is created once and
     * reused across connections, so a stale deep path from the server
     * we just left would otherwise carry into the next session — where
     * it doesn't exist, leaving "Up" unable to walk back to a valid
     * parent. Resetting here means the next connection always starts
     * listing from "/". */
    g_free (self->current_path);
    self->current_path = g_strdup ("/");
    /* Re-emit "navigated" so the panel's path bar + status footer
     * reflect the reset root and the now-empty listing. */
    g_signal_emit_by_name (self, "navigated", self->current_path);
}

/* ---- Interface implementations ---- */

static GListModel *
remote_get_listing (HxFilesProvider *self)
{
    return G_LIST_MODEL (HX_REMOTE_FILES_PROVIDER (self)->listing);
}

static const char *
remote_get_current_path (HxFilesProvider *self)
{
    HxRemoteFilesProvider *r = HX_REMOTE_FILES_PROVIDER (self);
    return r->current_path ? r->current_path : "/";
}

static const char *
remote_get_label (HxFilesProvider *self)
{
    (void)self;
    return _ ("Remote");
}

static void
remote_navigate (HxFilesProvider *self, const char *path)
{
    HxRemoteFilesProvider *r = HX_REMOTE_FILES_PROVIDER (self);
    if (!path || !*path) {
        return;
    }
    remote_send_file_list (r, path);
}

static void
remote_reload (HxFilesProvider *self)
{
    HxRemoteFilesProvider *r = HX_REMOTE_FILES_PROVIDER (self);
    remote_send_file_list (r, r->current_path);
}

/* Walk one component off the end of current_path. Hotline paths
 * use '/' as the separator (server-side it's stored as a
 * length-prefixed array of component pstrings, but the canonical
 * string form uses '/'). Root is "/". */
static void
remote_navigate_up (HxFilesProvider *self)
{
    HxRemoteFilesProvider *r = HX_REMOTE_FILES_PROVIDER (self);
    char *parent, *slash;

    if (!r->current_path || g_strcmp0 (r->current_path, "/") == 0) {
        return;
    }

    parent = g_strdup (r->current_path);
    slash = strrchr (parent, '/');
    if (!slash) {
        g_free (parent);
        return;
    }
    if (slash == parent) {
        slash[1] = '\0'; /* "/foo" → "/" */
    } else {
        *slash = '\0'; /* "/foo/bar" → "/foo" */
    }
    remote_send_file_list (r, parent);
    g_free (parent);
}

/* Build a server-side child path from current_path + name. */
static char *
remote_child_path (HxRemoteFilesProvider *r, const char *name)
{
    if (!r->current_path || g_strcmp0 (r->current_path, "/") == 0) {
        return g_strdup_printf ("/%s", name ? name : "");
    }
    return g_strdup_printf ("%s/%s", r->current_path, name ? name : "");
}

static gboolean
remote_mkdir (HxFilesProvider *self, const char *name, GError **err)
{
    HxRemoteFilesProvider *r = HX_REMOTE_FILES_PROVIDER (self);
    char *path;
    (void)err;

    if (!name || !*name) {
        return FALSE;
    }
    if (!the_session.htlc.fd) {
        return FALSE;
    }
    path = remote_child_path (r, name);
    hx_make_dir (&the_session.htlc, path);
    g_free (path);

    /* Settle with a re-list of the current directory. The wire
	 * response carries success-or-failure as a task error; if it
	 * failed, the user sees an empty refresh + the existing
	 * server-error toast machinery already surfaces a message. */
    remote_send_file_list (r, r->current_path);
    return TRUE;
}

static gboolean
remote_delete_entry (HxFilesProvider *self, const char *name, GError **err)
{
    HxRemoteFilesProvider *r = HX_REMOTE_FILES_PROVIDER (self);
    char *path;
    (void)err;

    if (!name || !*name) {
        return FALSE;
    }
    if (!the_session.htlc.fd) {
        return FALSE;
    }
    path = remote_child_path (r, name);
    hx_file_delete (&the_session.htlc, path);
    g_free (path);
    remote_send_file_list (r, r->current_path);
    return TRUE;
}

static gboolean
remote_rename (HxFilesProvider *self, const char *old_name,
               const char *new_name, GError **err)
{
    HxRemoteFilesProvider *r = HX_REMOTE_FILES_PROVIDER (self);
    char *src, *dst;
    (void)err;

    if (!old_name || !new_name) {
        return FALSE;
    }
    if (!the_session.htlc.fd) {
        return FALSE;
    }
    src = remote_child_path (r, old_name);
    dst = remote_child_path (r, new_name);
    hx_file_move (&the_session.htlc, src, dst);
    g_free (src);
    g_free (dst);
    remote_send_file_list (r, r->current_path);
    return TRUE;
}

/* Common xfer_new path used by both activate (download) and
 * preview, parameterised on the preview flag. xfer_new's
 * preview=1 routes bytes through hx_preview_set_info / _chunk /
 * _done rather than writing to disk; preview=0 writes the file
 * to lpath like a normal download. */
static void
remote_start_get (HxFilesProvider *self, HxFileEntry *e, int preview)
{
    const char *dir;
    char *lpath;
    struct htxf_conn *htxf;

    if (!e || hx_file_entry_is_dir (e)) {
        return;
    }
    if (!the_session.htlc.fd) {
        return;
    }
    if (!hl_access_has ((const guint8 *)&the_session.htlc.access,
                        HL_ACCESS_DOWNLOAD_FILES)) {
        return;
    }

    dir = hx_files_provider_get_current_path (self);

    /* lpath: real on-disk destination for downloads (preview=0)
	 * or a placeholder used only for logging/tooltip on the
	 * preview path (preview=1, opt.preview branch in xfer_new
	 * skips the write). The download dir comes from the user's
	 * Settings → File Browser → Download folder pref, falling
	 * back to /tmp if unset.
	 *
	 * Sanitize the entry name to a safe local basename: a
	 * hostile server could ship "../../etc/passwd" or similar
	 * as a file name to escape the user's download folder via
	 * g_build_filename. The wire-side path still uses the raw
	 * name (xfer_new takes it as a separate (name, name_len)
	 * tuple below) so the over-the-wire request is unchanged. */
    {
        char *safe = hx_files_provider_safe_local_basename (
            hx_file_entry_get_name (e));
        lpath = g_build_filename (
            gtkhx_prefs.download_path ? gtkhx_prefs.download_path : "/tmp",
            safe, NULL);
        g_free (safe);
    }

    /* xfer_new takes the remote location as a (dir, name, name_len)
	 * triple — keeping the name's bytes (which may legally include
	 * `/`) out of the joined path so they survive the wire trip
	 * verbatim. The cached entry's name is already byte-for-byte
	 * what came off the wire. */
    {
        const char *name = hx_file_entry_get_name (e);
        gsize name_len = name ? strlen (name) : 0;
        htxf = xfer_new (lpath, dir ? dir : "", name, name_len, XFER_GET,
                         preview, 0);
    }
    if (htxf) {
        htxf->filter_argv = 0;
        htxf->opt.retry = 0;
    }

    g_free (lpath);
}

/* Enter / double-click on a remote file row: download to the
 * user's download folder. This is the "do the obvious thing"
 * default Misha asked for — preview remains one click away via
 * F3 / Ctrl+P / the Preview headerbar button (which routes
 * through preview_entry instead). */
static void
remote_activate_entry (HxFilesProvider *self, HxFileEntry *e)
{
    remote_start_get (self, e, 0 /* download to disk */);
}

/* Explicit preview path — F3 / Ctrl+P / Preview button. Streams
 * the file into the in-app preview window without writing to
 * disk (opt.preview branch in xfer_new). */
static void
remote_preview_entry (HxFilesProvider *self, HxFileEntry *e)
{
    remote_start_get (self, e, 1 /* preview, no on-disk write */);
}

static void
hx_remote_files_provider_iface_init (HxFilesProviderInterface *iface)
{
    iface->get_listing = remote_get_listing;
    iface->get_current_path = remote_get_current_path;
    iface->get_label = remote_get_label;
    iface->navigate = remote_navigate;
    iface->reload = remote_reload;
    iface->navigate_up = remote_navigate_up;
    iface->mkdir = remote_mkdir;
    iface->delete_entry = remote_delete_entry;
    iface->rename = remote_rename;
    iface->get_unavailable_reason = remote_get_unavailable_reason;
    iface->activate_entry = remote_activate_entry;
    iface->preview_entry = remote_preview_entry;
}
