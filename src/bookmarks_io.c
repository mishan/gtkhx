/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Bookmark CRUD: load/save/list/rename/delete + filename canonicalization,
 * carved out of bookmarks.c so the test harness can drive it without
 * pulling in GTK + libadwaita. The bookmarks management dialog
 * (bookmarks.c) consumes this module via bookmarks.h; production code
 * paths are identical to the original single-file version.
 *
 * On-disk format is the HTsc 460-byte fixed layout shared with the
 * Connect dialog's "Save Bookmark…" path in connect.c — bytewise
 * compatible so either source can read what the other wrote.
 *
 * The only external dependency is gtkhx_config_dir() from gtkhx.c.
 * Tests link a stub for that and otherwise compile this file as-is.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <dirent.h>

#include <glib.h>

#include "bookmarks.h"

/* Forward-declare gtkhx_config_dir() rather than #include "gtkhx.h":
 * that header transitively pulls in <gtk/gtk.h> (it declares helpers
 * taking GtkWidget / GtkWindow / PangoFontDescription), which would
 * defeat the whole point of carving this module out. Production
 * links against the real definition in gtkhx.c; tests link a stub
 * that returns a tmpdir path. */
extern const char *gtkhx_config_dir (void);

/* ============================================================
 * Path helpers
 * ============================================================ */

static char *
bookmarks_dir_primary (void)
{
    return g_build_filename (gtkhx_config_dir (), "bookmarks", NULL);
}

static char *
bookmarks_dir_legacy (void)
{
    const char *home = g_getenv ("HOME");
    if (!home || !*home) {
        home = g_get_home_dir ();
    }
    if (!home) {
        return NULL;
    }
    return g_build_filename (home, ".hx", "bookmarks", NULL);
}

/* Resolve a name to an existing file. Returns g_strdup'd absolute
 * path on success, NULL if no file. Tries primary first, falls back
 * to legacy. */
static char *
bookmark_resolve_path (const char *name)
{
    char *path;
    char *legacy_dir;
    struct stat st;

    if (!name || !*name) {
        return NULL;
    }
    /* Reject anything that could escape the bookmarks dir before
	 * we hand `name` to g_build_filename. On Unix only '/' is a
	 * directory separator — '\\' is a legitimate byte inside a
	 * filename, and hx_bookmark_safe_filename() *produces* '\\' as
	 * the defanged form of a user-typed '/'. Rejecting '\\' here
	 * would make any such bookmark unreachable (saved fine, never
	 * loadable). Refusing '/' is enough to keep an attacker from
	 * walking out to "../../etc/passwd". */
    if (strchr (name, '/')) {
        return NULL;
    }

    /* primary */
    {
        char *dir = bookmarks_dir_primary ();
        path = g_build_filename (dir, name, NULL);
        g_free (dir);
        if (stat (path, &st) == 0) {
            return path;
        }
        g_free (path);
    }

    /* legacy fallback */
    legacy_dir = bookmarks_dir_legacy ();
    if (!legacy_dir) {
        return NULL;
    }
    path = g_build_filename (legacy_dir, name, NULL);
    g_free (legacy_dir);
    if (stat (path, &st) == 0) {
        return path;
    }
    g_free (path);
    return NULL;
}

/* Return TRUE iff `safe_name` exists in the legacy ~/.hx/bookmarks/
 * dir. Used by Delete / Rename to distinguish "no such bookmark at
 * all" from "legacy-only — the user has it but we can't touch it from
 * the primary dir". The caller has already run the name through
 * hx_bookmark_safe_filename so we don't need to canonicalize again. */
static gboolean
bookmark_in_legacy_only (const char *safe_name)
{
    char *legacy_dir;
    char *legacy_path;
    struct stat st;
    gboolean present;

    if (!safe_name || !*safe_name) {
        return FALSE;
    }
    legacy_dir = bookmarks_dir_legacy ();
    if (!legacy_dir) {
        return FALSE;
    }
    legacy_path = g_build_filename (legacy_dir, safe_name, NULL);
    present = (stat (legacy_path, &st) == 0);
    g_free (legacy_path);
    g_free (legacy_dir);
    return present;
}

/* ============================================================
 * struct lifecycle
 * ============================================================ */

HxBookmark *
hx_bookmark_new (void)
{
    HxBookmark *bm = g_new0 (HxBookmark, 1);
    /* All zero-initialised; the strings stay empty until the caller
	 * fills them in. */
    return bm;
}

void
hx_bookmark_free (HxBookmark *bm)
{
    if (!bm) {
        return;
    }
    g_free (bm->name);
    g_free (bm);
}

/* ============================================================
 * Filename canonicalization
 * ============================================================ */

/* Defang the bookmark name so it can't escape the bookmarks dir.
 * Same convention connect.c::save_bookmark_response uses — replace
 * '/' with '\\'. Returns g_strdup'd, never NULL for non-empty input.
 *
 * Exposed (rather than file-static) so the UI can canonicalize the
 * user-typed name up front and store the same string in bm->name as
 * lives on disk. Otherwise rebuild_list+select misses on names that
 * contained a '/' (saved as '\\') and the just-saved row jumps off
 * the user's selection. */
char *
hx_bookmark_safe_filename (const char *name)
{
    char *out;
    char *p;

    if (!name || !*name) {
        return NULL;
    }
    out = g_strdup (name);
    for (p = out; *p; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }
    return out;
}

/* ============================================================
 * Listing
 * ============================================================ */

static void
list_dir_into (GHashTable *seen, GPtrArray *out, const char *path)
{
    DIR *dir;
    struct dirent *ent;

    if (!path || !(dir = opendir (path))) {
        return;
    }
    while ((ent = readdir (dir))) {
        if (*ent->d_name == '.') {
            continue;
        }
        if (g_hash_table_contains (seen, ent->d_name)) {
            continue;
        }
        g_hash_table_add (seen, g_strdup (ent->d_name));
        g_ptr_array_add (out, g_strdup (ent->d_name));
    }
    closedir (dir);
}

static gint
str_collate (gconstpointer a, gconstpointer b)
{
    return g_utf8_collate (*(const char *const *)a, *(const char *const *)b);
}

GList *
hx_bookmark_list (void)
{
    GHashTable *seen;
    GPtrArray *names;
    GList *out = NULL;
    char *primary, *legacy;
    guint i;

    seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    names = g_ptr_array_new_with_free_func (g_free);

    primary = bookmarks_dir_primary ();
    legacy = bookmarks_dir_legacy ();
    list_dir_into (seen, names, primary);
    list_dir_into (seen, names, legacy);
    g_free (primary);
    g_free (legacy);

    g_ptr_array_sort (names, str_collate);

    /* Transfer ownership of each strdup'd name from the GPtrArray
	 * to the returned GList. Clearing the free_func first so the
	 * subsequent g_ptr_array_free doesn't double-free strings the
	 * GList now owns. */
    g_ptr_array_set_free_func (names, NULL);
    for (i = 0; i < names->len; i++) {
        out = g_list_prepend (out, names->pdata[i]);
    }
    g_ptr_array_free (names, TRUE);
    g_hash_table_destroy (seen);
    return g_list_reverse (out);
}

/* ============================================================
 * Load (HTsc parse)
 * ============================================================ */

HxBookmark *
hx_bookmark_load (const char *name)
{
    char *path = bookmark_resolve_path (name);
    int bm;
    HxBookmark *out = NULL;
    char header[5];
    char junk[132];
    char len_addr;
    char server_buf[256];
    char *colon;
    size_t len;

    if (!path) {
        return NULL;
    }
    bm = open (path, O_RDONLY);
    g_free (path);
    if (bm < 0) {
        return NULL;
    }

    if (read (bm, header, 4) != 4) {
        goto fail;
    }
    header[4] = '\0';
    if (strcmp (header, "HTsc") != 0) {
        /* Legacy format. We don't translate it here — the Connect
		 * dialog's prompt_conversion path handles that. Caller will
		 * see NULL and can offer to convert via that path. */
        goto fail;
    }

    out = hx_bookmark_new ();
    out->name = g_strdup (name);

    /* 132-byte prefix to skip past: the writer emitted
	 *   "HTsc\0\1" (6) + 129 zero bytes (=135), then the login
	 *   length byte at offset 135.
	 * The first 4 ("HTsc") were consumed above; here we eat the
	 * next 132 bytes (4+132 = 136), which includes the login
	 * length byte at offset 135 — so reading the next 33 bytes
	 * gives us the NUL-padded login string starting at offset
	 * 136. The length byte is intentionally discarded because
	 * the field is already NUL-terminated within its 33-byte
	 * slot; we treat the byte as part of the prefix to match
	 * connect.c::bookmark_parse's layout. */
    if (read (bm, junk, 132) != 132) {
        goto fail;
    }
    {
        char login_field[34];
        if (read (bm, login_field, 33) != 33) {
            goto fail;
        }
        login_field[33] = '\0';
        g_strlcpy (out->login, login_field, sizeof (out->login));
    }
    /* Pass-length byte, also discarded — same NUL-padded-field
	 * reasoning as the login section above. */
    if (read (bm, &len_addr, 1) != 1) {
        goto fail;
    }
    {
        char pass_field[34];
        if (read (bm, pass_field, 33) != 33) {
            goto fail;
        }
        pass_field[33] = '\0';
        g_strlcpy (out->pass, pass_field, sizeof (out->pass));
    }
    if (read (bm, &len_addr, 1) != 1) {
        goto fail;
    }
    len = (unsigned char)len_addr;
    if (len >= sizeof (server_buf)) {
        goto fail;
    }
    if (read (bm, server_buf, len) != (ssize_t)len) {
        goto fail;
    }
    server_buf[len] = '\0';
    if (read (bm, &out->secure, 1) != 1) {
        goto fail;
    }
    if (read (bm, &out->compress, 1) != 1) {
        goto fail;
    }
    if (read (bm, &out->cipher, 1) != 1) {
        goto fail;
    }

    /* server_buf is "host:port" — split. */
    colon = strrchr (server_buf, ':');
    if (colon) {
        *colon = '\0';
        g_strlcpy (out->port, colon + 1, sizeof (out->port));
    } else {
        out->port[0] = '\0';
    }
    g_strlcpy (out->server, server_buf, sizeof (out->server));

    close (bm);
    return out;

fail:
    close (bm);
    hx_bookmark_free (out);
    return NULL;
}

/* ============================================================
 * Save (HTsc write)
 * ============================================================ */

/* Write `n` bytes from `buf` to `f`, treating a short write as a
 * failure. Returns TRUE only when the whole block landed. */
static gboolean
write_all (FILE *f, const void *buf, size_t n)
{
    return n == 0 || fwrite (buf, 1, n, f) == n;
}

gboolean
hx_bookmark_save (const HxBookmark *bm, GError **err)
{
    char *dir;
    char *safe;
    char *path;
    char *server_str = NULL;
    FILE *f = NULL;
    char zeros[256];
    char hdr[6] = { 'H', 'T', 's', 'c', 0, 1 };
    char flags[3];
    char lenbyte;
    size_t len, len_total;
    gboolean ok = FALSE;

    if (!bm || !bm->name || !*bm->name) {
        g_set_error (err, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                     "bookmark name required");
        return FALSE;
    }

    safe = hx_bookmark_safe_filename (bm->name);
    dir = bookmarks_dir_primary ();
    path = g_build_filename (dir, safe, NULL);

    if (g_mkdir_with_parents (dir, 0770) != 0) {
        g_set_error (err, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "create %s: %s", dir, g_strerror (errno));
        goto out;
    }

    if (bm->port[0]) {
        server_str = g_strdup_printf ("%s:%s", bm->server, bm->port);
    } else {
        server_str = g_strdup (bm->server);
    }

    f = fopen (path, "w");
    if (!f) {
        g_set_error (err, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "open %s for writing: %s", path, g_strerror (errno));
        goto out;
    }

    memset (zeros, 0, sizeof (zeros));

    /* Each block runs through write_all so a partial write
	 * (out-of-space mid-file, broken pipe, …) surfaces as an
	 * error instead of silently producing a corrupt file. We
	 * dropped the fprintf calls because fprintf returns the
	 * formatted byte count and there's no reason to involve
	 * format parsing for these literal bytes. */

    /* Header + version (6 bytes) + 129 zero pad. */
    if (!write_all (f, hdr, sizeof (hdr))
        || !write_all (f, zeros, 129)) {
        goto io_err;
    }

    /* Login: 1 length byte + 33-byte NUL-padded field. */
    len = strlen (bm->login);
    if (len > 32) {
        len = 32;
    }
    len_total = 33 - len;
    lenbyte = (char)len;
    if (!write_all (f, &lenbyte, 1) || !write_all (f, bm->login, len)
        || !write_all (f, zeros, len_total)) {
        goto io_err;
    }

    /* Password: same shape. */
    len = strlen (bm->pass);
    if (len > 32) {
        len = 32;
    }
    len_total = 33 - len;
    lenbyte = (char)len;
    if (!write_all (f, &lenbyte, 1) || !write_all (f, bm->pass, len)
        || !write_all (f, zeros, len_total)) {
        goto io_err;
    }

    /* Server "host:port" + 3 trailing flag bytes + zero padding up
	 * to a 256-byte block (256 - len - 3 zeros). */
    len = strlen (server_str);
    if (len > 252) {
        /* Truncate to fit; 252 = 256 - 1 (length byte) - 3 (flags) —
		 * not a path we expect to hit since server max is 128 +
		 * port max ~7 + ':'. */
        len = 252;
    }
    len_total = 256 - len;
    lenbyte = (char)len;
    flags[0] = bm->secure;
    flags[1] = bm->compress;
    flags[2] = bm->cipher;
    if (!write_all (f, &lenbyte, 1) || !write_all (f, server_str, len)
        || !write_all (f, flags, sizeof (flags))
        || !write_all (f, zeros, len_total - 3)) {
        goto io_err;
    }

    /* fclose can still surface a deferred error from a previously
	 * buffered short write — keep its return value. */
    if (fclose (f) != 0) {
        f = NULL;
        g_set_error (err, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "close %s: %s", path, g_strerror (errno));
        /* Best-effort: drop the partially-written file. */
        unlink (path);
        goto out;
    }
    f = NULL;
    ok = TRUE;
    goto out;

io_err:
    g_set_error (err, G_FILE_ERROR, g_file_error_from_errno (errno),
                 "write %s: %s", path, g_strerror (errno));
    fclose (f);
    f = NULL;
    unlink (path);

out:
    if (f) {
        fclose (f);
    }
    g_free (server_str);
    g_free (path);
    g_free (dir);
    g_free (safe);
    return ok;
}

/* ============================================================
 * Delete / Rename
 * ============================================================ */

gboolean
hx_bookmark_delete (const char *name, GError **err)
{
    char *path;
    char *dir;
    char *safe;
    gboolean ok = FALSE;
    int saved_errno;

    if (!name || !*name) {
        g_set_error (err, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                     "bookmark name required");
        return FALSE;
    }
    safe = hx_bookmark_safe_filename (name);
    dir = bookmarks_dir_primary ();
    path = g_build_filename (dir, safe, NULL);

    /* ENOENT-is-an-error contract — see the header docstring. A
	 * bookmark that lives only in the legacy ~/.hx/bookmarks/ dir
	 * is absent from the primary dir, so unlink returns ENOENT.
	 * Distinguish the two cases for the user: a legacy-only entry
	 * is read-only by design, so surface a clear "convert via
	 * Connect dialog" message; an actually-missing file gets the
	 * standard "no such file" so a tampered or stale list row is
	 * still recoverable. */
    if (unlink (path) == 0) {
        ok = TRUE;
    } else {
        saved_errno = errno;
        if (saved_errno == ENOENT && bookmark_in_legacy_only (safe)) {
            g_set_error (err, G_FILE_ERROR, G_FILE_ERROR_PERM,
                         "\"%s\" is a legacy bookmark and can't be "
                         "deleted from here. Open it from the Connect "
                         "dialog to convert.",
                         name);
        } else {
            g_set_error (err, G_FILE_ERROR,
                         g_file_error_from_errno (saved_errno),
                         "unlink %s: %s", path, g_strerror (saved_errno));
        }
    }

    g_free (path);
    g_free (dir);
    g_free (safe);
    return ok;
}

gboolean
hx_bookmark_rename (const char *old_name, const char *new_name, GError **err)
{
    char *dir;
    char *old_safe, *new_safe;
    char *old_path, *new_path;
    struct stat st;
    int saved_errno;
    gboolean ok = FALSE;

    if (!old_name || !*old_name || !new_name || !*new_name) {
        g_set_error (err, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                     "bookmark name required");
        return FALSE;
    }
    if (strcmp (old_name, new_name) == 0) {
        return TRUE; /* no-op */
    }

    dir = bookmarks_dir_primary ();
    old_safe = hx_bookmark_safe_filename (old_name);
    new_safe = hx_bookmark_safe_filename (new_name);
    old_path = g_build_filename (dir, old_safe, NULL);
    new_path = g_build_filename (dir, new_safe, NULL);

    /* Refuse to clobber an existing bookmark — the caller should
	 * have caught name collisions in the UI, but defend at the
	 * filesystem layer too. */
    if (stat (new_path, &st) == 0) {
        g_set_error (err, G_FILE_ERROR, G_FILE_ERROR_EXIST,
                     "bookmark \"%s\" already exists", new_name);
        goto out;
    }

    if (rename (old_path, new_path) == 0) {
        ok = TRUE;
    } else {
        saved_errno = errno;
        if (saved_errno == ENOENT && bookmark_in_legacy_only (old_safe)) {
            g_set_error (err, G_FILE_ERROR, G_FILE_ERROR_PERM,
                         "\"%s\" is a legacy bookmark and can't be "
                         "renamed from here. Open it from the Connect "
                         "dialog to convert.",
                         old_name);
        } else {
            g_set_error (err, G_FILE_ERROR,
                         g_file_error_from_errno (saved_errno),
                         "rename %s -> %s: %s", old_path, new_path,
                         g_strerror (saved_errno));
        }
    }

out:
    g_free (old_path);
    g_free (new_path);
    g_free (old_safe);
    g_free (new_safe);
    g_free (dir);
    return ok;
}
