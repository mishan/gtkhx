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

#include "hx.h"
#include "hxconn.h"
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

#include <stdbool.h>

/* Path-navigation model — the current path + sticky listing-error flag,
 * plus the parent/child path math. Implemented in the hxmodel::files Rust
 * crate (rust/crates/hxmodel/src/files/remote_listing.rs); this provider
 * holds one opaque handle and keeps only the GListStore, the no-reply
 * watchdog, and the reply plumbing. Paths are the server's bytes.
 *
 * `bool` (not `gboolean`) mirrors the Rust `extern "C"` 1-byte bool ABI.
 * `parent` / `child` return freshly-allocated strings the caller releases
 * with gtkhx_files_string_free (a Rust allocation — do NOT g_free it). */
typedef struct HxFilesListing HxFilesListing;
extern HxFilesListing *gtkhx_files_listing_new (void);
extern void gtkhx_files_listing_free (HxFilesListing *l);
extern const char *gtkhx_files_listing_current_path (const HxFilesListing *l);
extern void gtkhx_files_listing_set_path (HxFilesListing *l, const char *path);
extern void gtkhx_files_listing_reset (HxFilesListing *l);
extern bool gtkhx_files_listing_is_root (const HxFilesListing *l);
extern char *gtkhx_files_listing_parent (const HxFilesListing *l);
extern char *gtkhx_files_listing_child (const HxFilesListing *l,
                                        const char *name);
extern bool gtkhx_files_listing_has_error (const HxFilesListing *l);
extern void gtkhx_files_listing_set_error (HxFilesListing *l, bool v);
extern void gtkhx_files_string_free (char *s);

struct _HxRemoteFilesProvider {
    GObject parent_instance;
    /* The session whose server this pane lists. Every send, task and
     * access check below routes through it rather than through
     * hx_active_session(): a remote pane belongs to one server, and asking
     * which one the user is *looking at* is the wrong question the moment
     * there are two. Never NULL — see hx_remote_files_provider_new. */
    session *sess;
    GListStore *listing;
    /* Path-navigation state (current path + sticky listing-error flag).
     * The current path is Hotline-style ("/" at root); the error flag
     * drives the panel's empty-state hint ("Folder is upload-only" if
     * the access bits also indicate a drop-box, "Can't list this
     * folder" otherwise) — without it the user just sees an empty
     * panel and has no idea why the navigation didn't produce rows.
     * Both live in the Rust model behind `model`. */
    HxFilesListing *model;
    /* Watchdog for an in-flight FILE_LIST. Some servers silently drop
     * the request (no reply, no task error) when the account lacks a
     * server-side "list files" permission — mhxd gates HTLC_HDR_FILE_LIST
     * on access_extra.file_list and installs no rcv handler in that case,
     * and classic-Mac servers (MacSecret) behave the same. That permission
     * isn't in the access bitmap the client receives, so we can't
     * pre-check it; instead a timer resolves the pending listing to an
     * error state rather than spinning the panel forever. 0 = disarmed. */
    guint list_timeout_id;
};

/* Seconds to wait for a FILE_LIST reply before giving up. Generous
 * enough that a slow-but-working server building a large directory
 * doesn't false-trip, short enough that a silent drop doesn't leave the
 * panel stuck. */
#define REMOTE_FILE_LIST_TIMEOUT_S 12

static void
hx_remote_files_provider_iface_init (HxFilesProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (
    HxRemoteFilesProvider, hx_remote_files_provider, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (HX_TYPE_FILES_PROVIDER,
                           hx_remote_files_provider_iface_init))

/* The providers with a listing in flight. A provider has one at a time:
 * a reload during a fetch supersedes the previous request. */
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
    if (self->list_timeout_id) {
        g_source_remove (self->list_timeout_id);
        self->list_timeout_id = 0;
    }
    g_clear_object (&self->listing);
    gtkhx_files_listing_free (self->model);
    self->model = NULL;
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
    self->model = gtkhx_files_listing_new ();
}

/* The session behind a provider. Never NULL: hx_remote_files_provider_new
 * refuses a NULL session, and `provider` here is always this object —
 * gpointer only because the interface methods below are handed an
 * HxFilesProvider * while the concrete ones hold an HxRemoteFilesProvider *.
 *
 * Not to be confused with hx_remote_files_provider_session, which takes *any*
 * provider and answers NULL for a local one. That is a different question. */
static session *
hx_sess_of (gpointer provider)
{
    return HX_REMOTE_FILES_PROVIDER (provider)->sess;
}

/* This pane's connection. Also never NULL — a session owns its connection for
 * its whole life.
 *
 * Worth stating rather than assuming, because the hx_conn_* accessors read
 * through the pointer without checking it: there is no NULL-reads-as-zero to
 * lean on, and the calls below that look like NULL guards
 * (`if (!hx_conn_fd (...))`) are testing the socket, not the pointer.
 * Disconnected is fd == 0. */
static struct htlc_conn *
hx_conn_of (gpointer provider)
{
    return hx_sess_of (provider)->htlc;
}

session *
hx_remote_files_provider_session (gpointer provider)
{
    /* The public form, and the one that has to tolerate anything: callers
     * hold an HxFilesProvider * that may be the local provider — or nothing
     * at all, for a pane that hasn't been given one — and read a NULL answer
     * as "that side isn't a server". The explicit NULL test is so that
     * contract stands on its own rather than on what the GType cast macro
     * happens to do with a NULL. */
    if (provider == NULL || !HX_IS_REMOTE_FILES_PROVIDER (provider)) {
        return NULL;
    }
    return HX_REMOTE_FILES_PROVIDER (provider)->sess;
}

HxRemoteFilesProvider *
hx_remote_files_provider_new (session *sess)
{
    HxRemoteFilesProvider *self;

    /* See the header: the provider dereferences this on every operation, and
     * disconnected is fd == 0 rather than a missing session, so there is
     * nothing for NULL to mean. */
    g_return_val_if_fail (sess != NULL, NULL);

    self = g_object_new (HX_TYPE_REMOTE_FILES_PROVIDER, NULL);

    /* Borrowed, not reffed: a session outlives the panes that list it. */
    self->sess = sess;
    return self;
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
    if (!hx_conn_fd (hx_conn_of (self))) {
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
    if (!hx_conn_post_login_fetched (hx_conn_of (self))) {
        return _ ("Logging in…");
    }
    return NULL;
}

/* Watchdog: no FILE_LIST reply arrived in time. Resolve the pending
 * listing to an error state (mirrors the refusal path) so the panel stops
 * spinning.
 * See the list_timeout_id field comment for why a server can drop the
 * request without any reply. */
static gboolean
remote_list_timeout (gpointer data)
{
    HxRemoteFilesProvider *self = data;

    self->list_timeout_id = 0;
    if (!pending_listings || !g_hash_table_contains (pending_listings, self)) {
        /* A reply (or error) already resolved this — nothing to do. */
        return G_SOURCE_REMOVE;
    }

    /* Keep a ref across the table removal: dropping the table's ref could
     * otherwise finalize us mid-cleanup. */
    HxRemoteFilesProvider *keep = g_object_ref (self);
    g_hash_table_remove (pending_listings, self);

    g_list_store_remove_all (keep->listing);
    gtkhx_files_listing_set_error (keep->model, true);
    g_signal_emit_by_name (keep, "navigated",
                           gtkhx_files_listing_current_path (keep->model));

    g_object_unref (keep);
    return G_SOURCE_REMOVE;
}

/* ---- Wire send ----
 *
 * FILE_LIST goes through hxhandlers::send::files, which hands the reply back
 * on the file-list signal with this provider as its data carrier. */
static void
remote_send_file_list (HxRemoteFilesProvider *self, const char *path)
{
    if (!hx_conn_fd (hx_conn_of (self))) {
        return;
    }

    ensure_pending_table ();

    /* A superseding request cancels the previous watchdog; a fresh one is
     * armed below once we've sent. */
    if (self->list_timeout_id) {
        g_source_remove (self->list_timeout_id);
        self->list_timeout_id = 0;
    }

    /* Reffed entry — keeps the provider alive while the RPC is
     * in flight even if the browser closes. Drops on remove. */
    g_hash_table_insert (pending_listings, self, g_object_ref (self));

    hx_list_dir (hx_conn_of (self), path && *path ? path : "/", self);
    /* Arm the no-reply watchdog (see remote_list_timeout). */
    self->list_timeout_id = g_timeout_add_seconds (REMOTE_FILE_LIST_TIMEOUT_S,
                                                   remote_list_timeout, self);
}

/* ---- Reply: the listing, into HxFileEntry rows ----
 *
 * hx_cfl_populate builds the rows (hxmodel::files_entry) and replaces the
 * store's contents in one splice. The listing is the sender's, which frees
 * it once the signal returns. */
gboolean
hx_remote_files_provider_handle_file_list (gpointer cfl_p, gpointer fh,
                                           gpointer data)
{
    HxRemoteFilesProvider *self;
    struct cached_filelist *cfl = cfl_p;
    (void)fh;

    /* hxhandlers::recv::files holds a ref on `data` while the listing is
     * in flight, so it is live here. A reply the provider has stopped
     * waiting for (the watchdog fired) is dropped. */
    if (!data || !G_IS_OBJECT (data) || !HX_IS_REMOTE_FILES_PROVIDER (data)) {
        return FALSE;
    }
    if (!pending_listings || !g_hash_table_contains (pending_listings, data)) {
        return TRUE;
    }

    /* It's ours and still tracked. Steal the ref so we don't get
     * dropped mid-parse if the table removes us first. */
    self = g_object_ref (HX_REMOTE_FILES_PROVIDER (data));
    g_hash_table_remove (pending_listings, data);
    if (self->list_timeout_id) {
        g_source_remove (self->list_timeout_id);
        self->list_timeout_id = 0;
    }
    hx_cfl_populate (cfl, self->listing);

    /* A successful response clears any sticky listing-error state
     * from a previous failed navigation. */
    gtkhx_files_listing_set_error (self->model, false);

    /* Adopt the new path as the current one (the RPC was fired
     * with this path in cfl_path — if a second fetch superseded
     * the first, the more-recent one wins via pending_listings's
     * single-entry-per-provider invariant). */
    gtkhx_files_listing_set_path (self->model, hx_cfl_path (cfl));

    g_signal_emit_by_name (self, "navigated",
                           gtkhx_files_listing_current_path (self->model));

    g_object_unref (self);
    return TRUE;
}

/* Error counterpart to handle_file_list. Called from
 * hxhandlers::recv::files when the server refuses a listing, so the
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
    if (self->list_timeout_id) {
        g_source_remove (self->list_timeout_id);
        self->list_timeout_id = 0;
    }
    g_list_store_remove_all (self->listing);
    gtkhx_files_listing_set_error (self->model, true);

    /* The listing carries the path the user navigated to. Adopt it as the
     * current path even though the listing failed — otherwise the next
     * navigate_up has nothing to walk back from. */
    gtkhx_files_listing_set_path (self->model, hx_cfl_path (cfl));

    g_signal_emit_by_name (self, "navigated",
                           gtkhx_files_listing_current_path (self->model));

    g_object_unref (self);
    return TRUE;
}

/* Getter for the panel to consult when building empty-state
 * messaging. TRUE iff the most recent FILE_LIST RPC failed. */
gboolean
hx_remote_files_provider_has_listing_error (HxRemoteFilesProvider *self)
{
    return self ? gtkhx_files_listing_has_error (self->model) : FALSE;
}

void
hx_remote_files_provider_reset_to_root (HxRemoteFilesProvider *self)
{
    if (!self) {
        return;
    }
    g_list_store_remove_all (self->listing);
    /* Return to the server root and clear the error flag. The provider is
     * created once and reused across connections, so a stale deep path
     * from the server we just left would otherwise carry into the next
     * session — where it doesn't exist, leaving "Up" unable to walk back
     * to a valid parent. Resetting here means the next connection always
     * starts listing from "/". */
    gtkhx_files_listing_reset (self->model);
    /* Re-emit "navigated" so the panel's path bar + status footer
     * reflect the reset root and the now-empty listing. */
    g_signal_emit_by_name (self, "navigated",
                           gtkhx_files_listing_current_path (self->model));
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
    const char *p = gtkhx_files_listing_current_path (r->model);
    return p ? p : "/";
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
    remote_send_file_list (r, gtkhx_files_listing_current_path (r->model));
}

/* Walk one component off the end of the current path. The parent
 * math lives in the Rust model (parent() returns NULL at the root or
 * when there's no separator to walk back over — the no-op case). */
static void
remote_navigate_up (HxFilesProvider *self)
{
    HxRemoteFilesProvider *r = HX_REMOTE_FILES_PROVIDER (self);
    char *parent = gtkhx_files_listing_parent (r->model);

    if (parent) {
        remote_send_file_list (r, parent);
        gtkhx_files_string_free (parent);
    }
}

/* Build a server-side child path from the current path + name. The
 * model owns the join; copy the Rust-allocated result into a
 * g_free-able string so callers keep their g_free convention. */
static char *
remote_child_path (HxRemoteFilesProvider *r, const char *name)
{
    char *rs = gtkhx_files_listing_child (r->model, name);
    char *out = g_strdup (rs ? rs : "");
    gtkhx_files_string_free (rs);
    return out;
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
    if (!hx_conn_fd (hx_conn_of (self))) {
        return FALSE;
    }
    path = remote_child_path (r, name);
    hx_make_dir (hx_conn_of (self), path);
    g_free (path);

    /* Settle with a re-list of the current directory. The wire
     * response carries success-or-failure as a task error; if it
     * failed, the user sees an empty refresh + the existing
     * server-error toast machinery already surfaces a message. */
    remote_send_file_list (r, gtkhx_files_listing_current_path (r->model));
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
    if (!hx_conn_fd (hx_conn_of (self))) {
        return FALSE;
    }
    path = remote_child_path (r, name);
    hx_file_delete (hx_conn_of (self), path);
    g_free (path);
    remote_send_file_list (r, gtkhx_files_listing_current_path (r->model));
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
    if (!hx_conn_fd (hx_conn_of (self))) {
        return FALSE;
    }
    src = remote_child_path (r, old_name);
    dst = remote_child_path (r, new_name);
    hx_file_move (hx_conn_of (self), src, dst);
    g_free (src);
    g_free (dst);
    remote_send_file_list (r, gtkhx_files_listing_current_path (r->model));
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
    if (!hx_conn_fd (hx_conn_of (self))) {
        return;
    }
    if (!hx_conn_access_has (hx_conn_of (self), HL_ACCESS_DOWNLOAD_FILES)) {
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
     * verbatim: the bytes the listing sent. */
    {
        const char *name = hx_file_entry_get_wire_name (e);
        gsize name_len = name ? strlen (name) : 0;
        htxf = xfer_new (hx_conn_of (self), lpath, dir ? dir : "", name,
                         name_len, XFER_GET, preview, 0);
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
