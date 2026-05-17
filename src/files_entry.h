/*
 * files_entry.h — one row in the files browser model.
 *
 * A GObject so it can sit inside a GListStore that
 * GtkColumnView consumes. Both the local provider (Phase 1) and
 * the remote provider (Phase 2) produce HxFileEntry rows; the
 * panel widget doesn't care which side built them.
 */

#ifndef HX_FILES_ENTRY_H
#define HX_FILES_ENTRY_H 1

#include <glib-object.h>

G_BEGIN_DECLS

#define HX_TYPE_FILE_ENTRY (hx_file_entry_get_type ())
G_DECLARE_FINAL_TYPE (HxFileEntry, hx_file_entry, HX, FILE_ENTRY, GObject)

/* Construct a row.
 *
 *   name      — display name (UTF-8). Owned by caller; copied.
 *   is_dir    — TRUE for directories / Hotline folders. Used to
 *               format the size column ("—") and to enable the
 *               descend-on-Enter action.
 *   size      — file size in bytes. Folders set this to a child
 *               count (Hotline-style) which the formatter renders
 *               as "(N items)". Ignored if is_dir is FALSE and the
 *               provider doesn't know the size yet (pass 0).
 *   modified  — Unix timestamp (seconds), or 0 if unknown.
 *   kind      — short human description ("Folder", "MP3 Audio",
 *               etc.). Local provider derives from the GIO content
 *               type; remote provider passes the Hotline 4-byte
 *               file-type code translated through a name table.
 *               Owned by caller; copied. NULL means "unknown".
 *   icon_id   — Mac-classic cicn icon ID (see files.h ICON_*).
 *               The panel uses this to pick a row-icon resource;
 *               0 falls back to ICON_FILE / ICON_FOLDER based on
 *               is_dir.
 */
extern HxFileEntry *hx_file_entry_new (const char *name, gboolean is_dir,
                                       guint64 size, gint64 modified,
                                       const char *kind, guint16 icon_id);

extern const char *hx_file_entry_get_name (HxFileEntry *e);
extern gboolean hx_file_entry_is_dir (HxFileEntry *e);
extern guint64 hx_file_entry_get_size (HxFileEntry *e);
extern gint64 hx_file_entry_get_modified (HxFileEntry *e);
extern const char *hx_file_entry_get_kind (HxFileEntry *e);
extern guint16 hx_file_entry_get_icon_id (HxFileEntry *e);

/* Formatters used by the panel's column bind callbacks. Each
 * returns a fresh g_malloc'd string (caller frees).
 *
 *   size_text:   "—" for folders, "12.4 MB" for files
 *   modified_text: localized "Tue 14:32" / "Mar 5" / "2024-08-12"
 *                  depending on age, or "" when modified == 0
 */
extern char *hx_file_entry_format_size (HxFileEntry *e);
extern char *hx_file_entry_format_modified (HxFileEntry *e);

G_END_DECLS

#endif /* HX_FILES_ENTRY_H */
