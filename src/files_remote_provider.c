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
#include "network.h"
#include "tasks.h"
#include "rcv.h"
#include "files.h"
#include "gtkutil.h"
#include "files_entry.h"
#include "files_provider.h"
#include "files_remote_provider.h"

struct _HxRemoteFilesProvider {
	GObject     parent_instance;
	GListStore *listing;
	char       *current_path;     /* Hotline-style; "/" at root */
};

static void hx_remote_files_provider_iface_init (HxFilesProviderInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (HxRemoteFilesProvider, hx_remote_files_provider,
	G_TYPE_OBJECT,
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
static GHashTable *pending_listings = NULL;   /* provider* → reffed provider* */

static void
ensure_pending_table (void)
{
	if (!pending_listings)
		pending_listings = g_hash_table_new_full (
			g_direct_hash, g_direct_equal,
			NULL, g_object_unref);
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
	self->listing      = g_list_store_new (HX_TYPE_FILE_ENTRY);
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
	(void) self;
	return the_session.htlc.fd ? NULL : _("Not connected to a server.");
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
	guint16  hldirlen;
	guint8  *hldir;

	if (!the_session.htlc.fd) return;

	ensure_pending_table ();

	cfl = g_malloc0 (sizeof (struct cached_filelist));
	cfl->path = g_strdup (path && *path ? path : "/");

	/* Reffed entry — keeps the provider alive while the RPC is
	 * in flight even if the browser closes. Drops on remove. */
	g_hash_table_insert (pending_listings, self, g_object_ref (self));

	hldir = path_to_hldir (cfl->path, &hldirlen, 0);
	task_new (&the_session.htlc,
		RCV_TASK_FN (rcv_task_file_list), cfl, self, "ls");
	hlwrite (&the_session.htlc, HTLC_HDR_FILE_LIST, 0, 1,
		HTLC_DATA_DIR, hldirlen, hldir);
	g_free (hldir);
}

/* ---- Reply: parse hl_filelist_hdr chunks into HxFileEntry ----
 *
 * Lifted from output_file_list in files.c — same wire walker,
 * just dumping into the new model shape. Fields available on
 * the wire: ftype (4 bytes — "fldr" for folders, FourCC for
 * files), fcreator, fsize, fnlen, fname. No mtime; the Modified
 * column stays empty for remote rows (Phase 4 polish: fetch
 * info lazily on row selection).
 *
 * Sub-byte conversions follow the same htonl / packed-struct
 * dance as the legacy code — file_list field bytes are
 * big-endian on the wire even though most other Hotline ints
 * arrive pre-byteswapped by the receive path. */
static void
populate_from_chunks (HxRemoteFilesProvider *self,
                      struct cached_filelist *cfl)
{
	struct hl_filelist_hdr *fh;
	char namebuf[256];
	char kindbuf[8];
	char *utf8;
	gboolean is_dir;
	guint32 fnlen, fsize, ftype;
	HxFileEntry *entry;

	g_list_store_remove_all (self->listing);

	if (!cfl || !cfl->fh || !cfl->fhlen)
		return;

	for (fh = cfl->fh;
	     (guint32) ((char *) fh - (char *) cfl->fh) < cfl->fhlen;
	     fh = (struct hl_filelist_hdr *)
		((char *) fh + fh->len + SIZEOF_HL_DATA_HDR))
	{
		/* fh->len drives the for-step pointer advance after the
		 * loop body, so it MUST be in host byte order before the
		 * next iteration. Skipping this is the same bug the
		 * legacy output_file_list path hit in commit ...: the
		 * network-order u16 (e.g. 0x0031) reads as 0x3100 on
		 * little-endian, the increment overshoots by orders of
		 * magnitude, and the loop terminates after one entry no
		 * matter how many the server sent. Byteswap in place
		 * mirrors the legacy walker. */
		fh->len = ntohs (fh->len);

		/* Field-by-field byteswap into locals. We read out of
		 * the receive buffer and never write back so cfl can be
		 * freed cleanly afterwards. (fh->len above is the
		 * exception — but that field's used only by the walker,
		 * not surfaced to the row.) */
		HN32 (&fnlen, &fh->fnlen);
		HN32 (&fsize, &fh->fsize);
		HN32 (&ftype, &fh->ftype);

		if (fnlen > sizeof (namebuf) - 1)
			fnlen = sizeof (namebuf) - 1;
		memcpy (namebuf, fh->fname, fnlen);
		namebuf[fnlen] = '\0';

		/* Hotline filename bytes can be Mac Roman on older
		 * servers — sanitise to UTF-8 for display. */
		utf8 = gtkhx_text_to_utf8 (namebuf, fnlen, NULL);

		is_dir = (ftype == 0x666c6472);   /* 'fldr' */

		/* Render the 4-byte FourCC as kind text for files; the
		 * legacy single-pane UI did the same. mhxd commonly
		 * sends "TEXT", "PDF ", "MPG3", etc. */
		if (is_dir) {
			g_strlcpy (kindbuf, _("Folder"), sizeof (kindbuf));
		} else {
			guint32 ftype_be = ftype;
			ftype_be = htonl (ftype_be);
			memcpy (kindbuf, &ftype_be, 4);
			kindbuf[4] = '\0';
		}

		/* Classify icon from the 4-byte Hotline file-type code.
		 * icon_of_ftype_and_name reads the network-byte-order
		 * bytes directly from the receive buffer — we pass the
		 * raw FourCC, not the htonl'd local. */
		entry = hx_file_entry_new (
			utf8 ? utf8 : namebuf,
			is_dir,
			is_dir ? 0 : (guint64) fsize,
			0,             /* no mtime on the wire */
			kindbuf,
			icon_of_ftype_and_name (
				(const char *) &fh->ftype,
				namebuf, fnlen));
		g_list_store_append (self->listing, entry);
		g_object_unref (entry);
		g_free (utf8);
	}
}

gboolean
hx_remote_files_provider_handle_file_list (gpointer cfl_p, gpointer fh,
                                            gpointer data)
{
	HxRemoteFilesProvider *self;
	struct cached_filelist *cfl = cfl_p;
	(void) fh;

	if (!pending_listings) return FALSE;
	if (!g_hash_table_contains (pending_listings, data)) return FALSE;

	/* It's ours. Steal the ref so we don't get dropped mid-parse
	 * if the table removes us first. */
	self = g_object_ref (HX_REMOTE_FILES_PROVIDER (data));
	g_hash_table_remove (pending_listings, data);

	populate_from_chunks (self, cfl);

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
	(void) self;
	return _("Remote");
}

static void
remote_navigate (HxFilesProvider *self, const char *path)
{
	HxRemoteFilesProvider *r = HX_REMOTE_FILES_PROVIDER (self);
	if (!path || !*path) return;
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

	if (!r->current_path || g_strcmp0 (r->current_path, "/") == 0)
		return;

	parent = g_strdup (r->current_path);
	slash = strrchr (parent, '/');
	if (!slash) {
		g_free (parent);
		return;
	}
	if (slash == parent)
		slash[1] = '\0';     /* "/foo" → "/" */
	else
		*slash = '\0';        /* "/foo/bar" → "/foo" */
	remote_send_file_list (r, parent);
	g_free (parent);
}

/* Build a server-side child path from current_path + name. */
static char *
remote_child_path (HxRemoteFilesProvider *r, const char *name)
{
	if (!r->current_path || g_strcmp0 (r->current_path, "/") == 0)
		return g_strdup_printf ("/%s", name ? name : "");
	return g_strdup_printf ("%s/%s", r->current_path, name ? name : "");
}

static gboolean
remote_mkdir (HxFilesProvider *self, const char *name, GError **err)
{
	HxRemoteFilesProvider *r = HX_REMOTE_FILES_PROVIDER (self);
	char *path;
	(void) err;

	if (!name || !*name) return FALSE;
	if (!the_session.htlc.fd) return FALSE;
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
	(void) err;

	if (!name || !*name) return FALSE;
	if (!the_session.htlc.fd) return FALSE;
	path = remote_child_path (r, name);
	hx_file_delete (&the_session.htlc, path);
	g_free (path);
	remote_send_file_list (r, r->current_path);
	return TRUE;
}

static gboolean
remote_rename (HxFilesProvider *self,
               const char *old_name, const char *new_name,
               GError    **err)
{
	HxRemoteFilesProvider *r = HX_REMOTE_FILES_PROVIDER (self);
	char *src, *dst;
	(void) err;

	if (!old_name || !new_name) return FALSE;
	if (!the_session.htlc.fd) return FALSE;
	src = remote_child_path (r, old_name);
	dst = remote_child_path (r, new_name);
	hx_file_move (&the_session.htlc, src, dst);
	g_free (src);
	g_free (dst);
	remote_send_file_list (r, r->current_path);
	return TRUE;
}

static void
hx_remote_files_provider_iface_init (HxFilesProviderInterface *iface)
{
	iface->get_listing            = remote_get_listing;
	iface->get_current_path       = remote_get_current_path;
	iface->get_label              = remote_get_label;
	iface->navigate               = remote_navigate;
	iface->reload                 = remote_reload;
	iface->navigate_up            = remote_navigate_up;
	iface->mkdir                  = remote_mkdir;
	iface->delete_entry           = remote_delete_entry;
	iface->rename                 = remote_rename;
	iface->get_unavailable_reason = remote_get_unavailable_reason;
}
