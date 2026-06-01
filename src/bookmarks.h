/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HX_BOOKMARKS_H
#define HX_BOOKMARKS_H

#include <glib.h>

/*
 * Bookmark CRUD + management dialog.
 *
 * The on-disk format is the HTsc 460-byte fixed layout that
 * connect.c::save_bookmark_response and bookmark_parse have used
 * since Phase 5; this module reads and writes the same byte sequence
 * so a bookmark created here is indistinguishable from one created
 * via the Connect dialog's "Save Bookmark…" path.
 *
 * Files live under $XDG_CONFIG_HOME/gtkhx/bookmarks/ (resolved via
 * gtkhx_config_dir()). The legacy ~/.hx/bookmarks/ path is read-only:
 * we surface bookmarks that exist there in the list, but Save always
 * writes to the primary dir, and Delete/Rename refuse legacy-only
 * entries (so the user doesn't accidentally lose them).
 */

/* In-memory representation. Strings are fixed-size buffers to match
 * the on-disk layout — login/pass are 33 bytes because the wire
 * protocol's STRING32 chunk maxes out at 32 chars + NUL. */
typedef struct {
    char *name;       /* filename in bookmarks dir; heap-alloc, NULL for
                         a new-unsaved bookmark */
    char server[128]; /* hostname (no port) */
    char port[8];     /* ASCII port number, empty = default 5500 */
    char login[33];
    char pass[33];
    char secure;      /* HOPE on */
    char compress;    /* 0 = off, 1+ indexes valid_compressors[] */
    /* Stable bookmark cipher byte; see bookmark_cipher.h for the
     * byte ↔ cipher-name mapping. Independent of valid_ciphers[]
     * so reordering the connect dialog's dropdown doesn't shift
     * the meaning of any saved bookmark. */
    char cipher;
    char tls;         /* 0 = off, 1 = TLS over the server's dedicated TLS port */
} HxBookmark;

extern HxBookmark *hx_bookmark_new (void);
extern void hx_bookmark_free (HxBookmark *bm);

/* Lets callers use g_autoptr(HxBookmark) for scope-bound ownership of
 * the result of hx_bookmark_load. The cleanup func is hx_bookmark_free,
 * which is already NULL-safe, so g_autoptr drops cleanly on early
 * return / goto / branch without per-path g_free bookkeeping. */
G_DEFINE_AUTOPTR_CLEANUP_FUNC (HxBookmark, hx_bookmark_free)

/* Canonicalize a user-typed name into the on-disk filename:
 * replaces '/' with '\\' (the convention connect.c::save_bookmark_response
 * has used since Phase 5). Returns g_strdup'd; NULL on NULL/empty
 * input. Caller frees with g_free. Use this before stuffing a name
 * into HxBookmark.name so what the UI shows matches what hx_bookmark_save
 * writes to disk — otherwise rebuild/select after Save can miss. */
extern char *hx_bookmark_safe_filename (const char *name);

/* GList of newly-allocated UTF-8 bookmark names (filenames) ordered
 * by g_utf8_collate. Caller frees with g_list_free_full(list, g_free). */
extern GList *hx_bookmark_list (void);

/* Load `name` (or NULL on missing / corrupt / legacy-only-format).
 * Caller frees with hx_bookmark_free. */
extern HxBookmark *hx_bookmark_load (const char *name);

/* Persist `bm` to disk under `bm->name`. Creates the bookmarks dir
 * on first use. Returns TRUE on success; on failure sets *err if
 * non-NULL. Refuses NULL/empty bm->name. */
extern gboolean hx_bookmark_save (const HxBookmark *bm, GError **err);

/* Delete the bookmark file `name` from the primary bookmarks dir.
 * Returns TRUE only if a file actually went away; sets *err on any
 * failure, *including* ENOENT. The ENOENT-is-an-error contract is
 * deliberate: a bookmark that exists only in the legacy
 * ~/.hx/bookmarks/ dir is absent from the primary dir, so Delete
 * surfaces "no such file" rather than silently succeeding and
 * leaving the legacy entry in place where the user will keep
 * seeing it. */
extern gboolean hx_bookmark_delete (const char *name, GError **err);

/* Rename the bookmark file `old_name` → `new_name`. Returns TRUE
 * on success; sets *err if filesystem rename fails or new_name
 * already exists. */
extern gboolean hx_bookmark_rename (const char *old_name, const char *new_name,
                                    GError **err);

/* Open (or focus) the bookmarks management dialog over the toolbar
 * window. Idempotent — a second call while open just re-raises. */
extern void create_bookmarks_window (void);

#endif /* HX_BOOKMARKS_H */
