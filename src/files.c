/*
 * Copyright (C) 2000-2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include <adwaita.h>
#include <ctype.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include "hx.h"
#include "gtkhx_session.h"
#include "macres.h"
#include "xfers.h"
#include "toolbar.h"
#include "gtkutil.h"
#include "gtkhx.h"
#include "cicn.h"
#include "tasks.h"
#include "rcv.h"
#include "files.h"

#define ICON_FILE 400
#define ICON_FOLDER 401
#define ICON_FOLDER_IN 421
#define ICON_FILE_HTft 402
#define ICON_FILE_SIT 403
#define ICON_FILE_TEXT 404
#define ICON_FILE_IMAGE 406
#define ICON_FILE_APPL 407
#define ICON_FILE_HTLC 408
#define ICON_FILE_SITP 409
#define ICON_FILE_alis 422
#define ICON_FILE_DISK 423
#define ICON_FILE_NOTE 424
#define ICON_FILE_MOOV 425
#define ICON_FILE_ZIP 426

/* ICON_* constants moved to files.h so the new files browser
 * (files_remote_provider.c, files_local_provider.c, files_panel.c)
 * can drive load_icon from the same table. */

guint8 dir_char = '/';

/* human_readable / human_size live in src/human_readable.c so the
 * Tier 1 unit test can link them without dragging in this TU's
 * GTK + Adwaita pile. The "fileutils-4.0/lib/human.c" attribution
 * applies to the body of the algorithm; see human_readable.c. */

/* needle must be uppercase :) */
static int
strcasestr_len (char *haystack, char *needle, size_t len)
{
    char *p, *np = 0, *end = haystack + len;

    for (p = haystack; p < end; p++) {
        if (np) {
            if (toupper (*p) == *np) {
                if (!*++np) {
                    return 1;
                }
            } else {
                np = 0;
            }
        } else if (toupper (*p) == *needle) {
            np = needle + 1;
        }
    }
    return 0;
}

/* Pick a cicn icon ID for a Hotline file based on its 4-byte
 * type code (plus filename, for the drop-box heuristic on
 * folders). Public so the new files browser's remote provider
 * can drive it directly off the parsed wire chunks. */
guint16
icon_of_ftype_and_name (const char *ftype, const char *name, gsize name_len)
{
    if (!ftype) {
        return ICON_FILE;
    }

    if (!memcmp (ftype, "fldr", 4)) {
        if (name
            && (strcasestr_len ((char *)name, "DROP BOX", name_len)
                || strcasestr_len ((char *)name, "UPLOAD", name_len))) {
            return ICON_FOLDER_IN;
        }
        return ICON_FOLDER;
    }
    if (!memcmp (ftype, "JPEG", 4) || !memcmp (ftype, "PNGf", 4)
        || !memcmp (ftype, "GIFf", 4) || !memcmp (ftype, "PICT", 4)) {
        return ICON_FILE_IMAGE;
    }
    if (!memcmp (ftype, "MPEG", 4) || !memcmp (ftype, "MPG ", 4)
        || !memcmp (ftype, "AVI ", 4) || !memcmp (ftype, "MooV", 4)) {
        return ICON_FILE_MOOV;
    }
    if (!memcmp (ftype, "MP3 ", 4)) {
        return ICON_FILE_NOTE;
    }
    if (!memcmp (ftype, "ZIP ", 4)) {
        return ICON_FILE_ZIP;
    }
    if (!memcmp (ftype, "SIT", 3)) {
        return ICON_FILE_SIT;
    }
    if (!memcmp (ftype, "APPL", 4)) {
        return ICON_FILE_APPL;
    }
    if (!memcmp (ftype, "rohd", 4)) {
        return ICON_FILE_DISK;
    }
    if (!memcmp (ftype, "HTft", 4)) {
        return ICON_FILE_HTft;
    }
    if (!memcmp (ftype, "alis", 4)) {
        return ICON_FILE_alis;
    }
    if (!memcmp (ftype, "TEXT", 4)) {
        return ICON_FILE_TEXT;
    }
    return ICON_FILE;
}

guint16
icon_of_fh (struct hl_filelist_hdr *fh)
{
    if (!fh) {
        return ICON_FILE;
    }
    return icon_of_ftype_and_name ((const char *)&fh->ftype,
                                   (const char *)fh->fname, (gsize)fh->fnlen);
}

/* FourCC → human label. Table is intentionally small — only the
 * codes we see often in the wild on Hotline servers. Anything
 * unknown falls through to "<XXXX> file" with the raw FourCC,
 * which is still better than the raw 4-byte glyph the old code
 * showed. Strings here are plain literals; _() runs at lookup
 * time so any later translation catalog picks them up without
 * needing N_() / gettext-noop machinery in this TU. */
const char *
kind_of_ftype (const char *ftype, gboolean *is_static_out)
{
    static const struct {
        const char *code;
        const char *label;
    } table[] = {
        { "fldr", "Folder" },          { "TEXT", "Text Document" },
        { "PDF ", "PDF Document" },    { "JPEG", "JPEG Image" },
        { "GIFf", "GIF Image" },       { "GIF ", "GIF Image" },
        { "PNGf", "PNG Image" },       { "PNG ", "PNG Image" },
        { "PICT", "PICT Image" },      { "TIFF", "TIFF Image" },
        { "BMP ", "BMP Image" },       { "MP3 ", "MP3 Audio" },
        { "MPG3", "MP3 Audio" },       { "AIFF", "AIFF Audio" },
        { "AIFC", "AIFF Audio" },      { "WAVE", "WAV Audio" },
        { "Mp3 ", "MP3 Audio" },       { "MooV", "QuickTime Movie" },
        { "MPEG", "MPEG Video" },      { "MPG ", "MPEG Video" },
        { "M4V ", "MPEG-4 Video" },    { "AVI ", "AVI Video" },
        { "MKV ", "Matroska Video" },  { "ZIP ", "ZIP Archive" },
        { "SIT!", "StuffIt Archive" }, { "SITD", "StuffIt Archive" },
        { "SIT5", "StuffIt Archive" }, { "BINA", "MacBinary Archive" },
        { "TARF", "TAR Archive" },     { "Tar ", "TAR Archive" },
        { "GZIP", "Gzip Archive" },    { "GZip", "Gzip Archive" },
        { "BZIP", "Bzip2 Archive" },   { "APPL", "Application" },
        { "rohd", "Disk Image" },      { "IMG ", "Disk Image" },
        { "ISO ", "ISO Disk Image" },  { "DMG ", "Disk Image" },
        { "HTft", "HTML Document" },   { "HTML", "HTML Document" },
        { "alis", "Alias" },           { "SLNK", "Symbolic Link" },
    };
    gsize i;

    if (!ftype) {
        if (is_static_out) {
            *is_static_out = TRUE;
        }
        return _ ("Unknown");
    }

    for (i = 0; i < G_N_ELEMENTS (table); i++) {
        if (memcmp (ftype, table[i].code, 4) == 0) {
            if (is_static_out) {
                *is_static_out = TRUE;
            }
            return _ (table[i].label);
        }
    }

    /* Fall-through: format a one-off string with the raw FourCC.
	 * Caller frees. Avoids embedding non-printable bytes by
	 * substituting '?' for anything outside printable ASCII —
	 * some Hotline FourCCs are control bytes (NUL-padded
	 * short strings, etc.) that would render as boxes. */
    {
        char safe[5];
        gsize k;
        char *out;
        for (k = 0; k < 4; k++) {
            unsigned char c = (unsigned char)ftype[k];
            safe[k] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        }
        safe[4] = '\0';
        out = g_strdup_printf (_ ("%s file"), safe);
        if (is_static_out) {
            *is_static_out = FALSE;
        }
        return out;
    }
}

static void
set_name_comment (GtkWidget *btn, gpointer data)
{
    GtkWidget *name_entry = g_object_get_data (G_OBJECT (btn), "name");
    GtkWidget *comments_text = g_object_get_data (G_OBJECT (btn), "comments");
    char *path = g_object_get_data (G_OBJECT (btn), "path");
    const char *name;
    char *comments;
    char *file;
    GtkTextBuffer *cbuf;
    GtkTextIter cstart, cend;

    (void)data;

    /* gtk_editable_get_text returns a const string owned by the
	 * entry — don't free it. */
    name = gtk_editable_get_text (GTK_EDITABLE (name_entry));
    if (!name) {
        name = "";
    }

    /* The comments widget is a GtkTextView, not a GtkEditable —
	 * gtk_editable_get_chars on it would null-deref, which is what
	 * the previous code did and what crashed Save. Pull the text
	 * via the buffer API instead. gtk_text_buffer_get_text returns
	 * a fresh g_malloc'd copy that we own. */
    cbuf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (comments_text));
    gtk_text_buffer_get_start_iter (cbuf, &cstart);
    gtk_text_buffer_get_end_iter (cbuf, &cend);
    comments = gtk_text_buffer_get_text (cbuf, &cstart, &cend, FALSE);
    if (!comments) {
        comments = g_strdup ("");
    }

    file = dirchar_basename (path);
    task_new (&the_session.htlc, 0, 0, 0, "set file info");

    /* Phase E (follow-up): encode the user-facing strings to the
	 * negotiated wire encoding. file (the current basename, which
	 * we received from the server in Mac Roman bytes and converted
	 * to UTF-8 for display) round-trips back as Mac Roman; the new
	 * name typed by the user in the rename dialog and the comment
	 * also go through the encoder. is_body = FALSE — file/name
	 * fields are single-line; comment is multi-line but the spec
	 * lists DATA_FILE_COMMENT as is_body too. */
    struct htlc_conn *htlc = &the_session.htlc;
    gboolean utf8 = (htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
    gsize file_len = 0, name_len = 0, comments_len = 0;
    char *file_wire = gtkhx_text_for_wire (file, strlen (file), utf8, FALSE,
                                           &file_len);
    char *name_wire = gtkhx_text_for_wire (name, strlen (name), utf8, FALSE,
                                           &name_len);
    char *comments_wire = gtkhx_text_for_wire (
        comments, strlen (comments), utf8, /*is_body=*/TRUE, &comments_len);

    if (file != path) {
        guint16 hldirlen = 0;
        guint8 *hldir = path_to_hldir (path, &hldirlen, 1);
        hlwrite (htlc, HTLC_HDR_FILE_SETINFO, 0, 4, HTLC_DATA_FILE_NAME,
                 (guint16)file_len, file_wire, HTLC_DATA_FILE_RENAME,
                 (guint16)name_len, name_wire, HTLC_DATA_FILE_COMMENT,
                 (guint16)comments_len, comments_wire, HTLC_DATA_DIR,
                 hldirlen, hldir);
        g_free (hldir);
    } else {
        hlwrite (htlc, HTLC_HDR_FILE_SETINFO, 0, 3, HTLC_DATA_FILE_NAME,
                 (guint16)file_len, file_wire, HTLC_DATA_FILE_RENAME,
                 (guint16)name_len, name_wire, HTLC_DATA_FILE_COMMENT,
                 (guint16)comments_len, comments_wire);
    }
    g_free (file_wire);
    g_free (name_wire);
    g_free (comments_wire);

    g_free (comments);
}

static void
close_file_info (GtkWidget *win, char *path)
{
    g_free (path);
}

/* Single read-only metadata row in the File Info dialog. Title is
 * the field name (e.g. "Size"); subtitle is the value, rendered as
 * an em-dash when the value is missing or empty so the row stays
 * informative either way. The subtitle is selectable for copy. */
static GtkWidget *
hx_file_info_row (const char *title, const char *value)
{
    GtkWidget *row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row),
                                 (value && value[0]) ? value : "—");
    adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), TRUE);
    return row;
}

void
output_file_info (char *path, char *name, char *creator, char *type,
                  char *comments, char *modified, char *created, guint64 size)
{
    GtkWidget *window, *header, *savebtn;
    GtkWidget *vbox;
    GtkWidget *name_group, *name_entry;
    GtkWidget *info_group;
    GtkWidget *comments_group, *comments_scroll, *comments_text;
    GtkTextBuffer *cbuf;
    char humanbuf[LONGEST_HUMAN_READABLE + 1];
    char sizestr[64];

    window = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (window), _ ("File Info"));
    gtk_window_set_default_size (GTK_WINDOW (window), 460, 540);

    /* AdwHeaderBar with Save action on the trailing edge. */
    header = adw_header_bar_new ();
    savebtn = gtk_button_new_with_label (_ ("Save"));
    gtk_widget_add_css_class (savebtn, "suggested-action");
    adw_header_bar_pack_end (ADW_HEADER_BAR (header), savebtn);
    gtk_window_set_titlebar (GTK_WINDOW (window), header);

    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start (vbox, 12);
    gtk_widget_set_margin_end (vbox, 12);
    gtk_widget_set_margin_top (vbox, 12);
    gtk_widget_set_margin_bottom (vbox, 12);

    /* Name (editable). AdwEntryRow keeps the label inline with the
	 * field and gives us a wide entry that doesn't truncate the
	 * file name visually. */
    name_group = adw_preferences_group_new ();
    name_entry = adw_entry_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (name_entry),
                                   _ ("Name"));
    gtk_editable_set_text (GTK_EDITABLE (name_entry), name ? name : "");
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (name_group), name_entry);
    gtk_box_append (GTK_BOX (vbox), name_group);

    /* Read-only metadata. */
    if (size > 0) {
        char *human = human_size (humanbuf, size);
        if (size >= 1024) {
            g_snprintf (sizestr, sizeof sizestr, "%s (%" G_GUINT64_FORMAT
                                                 " %s)",
                        human, (guint64)size, _ ("bytes"));
        } else {
            g_snprintf (sizestr, sizeof sizestr, "%" G_GUINT64_FORMAT " %s",
                        (guint64)size, _ ("bytes"));
        }
    } else {
        sizestr[0] = '\0';
    }

    info_group = adw_preferences_group_new ();
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (info_group),
                               hx_file_info_row (_ ("Creator"), creator));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (info_group),
                               hx_file_info_row (_ ("Type"), type));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (info_group),
                               hx_file_info_row (_ ("Size"), sizestr));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (info_group),
                               hx_file_info_row (_ ("Created"), created));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (info_group),
                               hx_file_info_row (_ ("Modified"), modified));
    gtk_box_append (GTK_BOX (vbox), info_group);

    /* Comments. AdwPreferencesGroup gives us a titled section; the
	 * scrolled text view is added directly to the group. */
    comments_group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (comments_group),
                                     _ ("Comments"));
    comments_text = gtk_text_view_new ();
    gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (comments_text),
                                 GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_editable (GTK_TEXT_VIEW (comments_text), TRUE);
    cbuf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (comments_text));
    gtk_text_buffer_set_text (cbuf, comments ? comments : "",
                              comments ? strlen (comments) : 0);

    comments_scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (comments_scroll),
                                    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (comments_scroll),
                                       TRUE);
    gtk_widget_set_size_request (comments_scroll, -1, 140);
    gtkhx_widget_set_child (comments_scroll, comments_text);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (comments_group),
                               comments_scroll);
    gtk_widget_set_vexpand (comments_group, TRUE);
    gtk_box_append (GTK_BOX (vbox), comments_group);

    gtkhx_widget_set_child (window, vbox);

    g_object_set_data (G_OBJECT (savebtn), "name", name_entry);
    g_object_set_data (G_OBJECT (savebtn), "comments", comments_text);
    g_object_set_data (G_OBJECT (savebtn), "path", path);
    g_signal_connect (savebtn, "clicked", G_CALLBACK (set_name_comment), NULL);

    g_signal_connect (window, "destroy", G_CALLBACK (close_file_info), path);

    gtk_window_present (GTK_WINDOW (window));
    init_keyaccel (window);
}

struct cached_filelist *
cfl_lookup (const char *path)
{
    /* Phase 5: the legacy implementation walked the global
	 * gfile_list to share a cached_filelist with an open browser
	 * window for that path. With the legacy browser retired the
	 * sharing has nothing to share with — every caller now gets a
	 * fresh zeroed cfl and fills in `path` itself. The single
	 * remaining caller (rcv.c::rcv_task_file_list, recursive
	 * folder download path) does exactly that. */
    (void)path;
    return g_malloc0 (sizeof (struct cached_filelist));
}

void
cfl_print (struct cached_filelist *cfl, void *data)
{
    struct hl_filelist_hdr *fh = cfl->fh;

    if (data) {
        gtkhx_session_emit_file_list (gtkhx_session_get_default (), cfl, fh,
                                      data);
    }
}

/* path_to_hldir lives in src/path_hldir.c so the Tier 1 unit test
 * can link it without dragging in this TU's GTK + Adwaita pile.
 * The dir_char global below is what that extracted code references. */

/* Phase 5: dirchar_basename is now a thin wrapper around the
 * dir_char-free path_basename(path, sep) so the unit tests can
 * exercise the underlying logic without linking files.c. The shape
 * stays identical for callers; dir_char is still the global the
 * Hotline-server-driven dirchar_change() rewrites. */
char *
dirchar_basename (char *path)
{
    return path_basename (path, (char)dir_char);
}

void
dirchar_fix (char *lpath)
{
    char *p;

    for (p = lpath; *p; p++) {
        if (*p == '/') {
            *p = (dir_char == '/' ? ':' : dir_char);
        }
    }
}

/* dirmask is in src/path_hldir.c alongside path_to_hldir — see the
 * comment there. */

int
exists_remote (char *path)
{
    /* Phase 5: the legacy implementation walked the now-deleted
	 * gfile_list cache to answer "is path present in any open
	 * browser's last listing?" The new files browser doesn't
	 * maintain that global cache — its listings live inside the
	 * provider as a transient GListStore that's rebuilt per
	 * directory.
	 *
	 * The single caller (xfers.c::xfer_go on the upload path)
	 * uses the answer to decide whether to attach a FILE_PREVIEW
	 * "is-resume" chunk to HTLC_HDR_FILE_PUT. Returning 0 here
	 * matches the legacy code's first-call behaviour (cache miss
	 * → async listing fires → return 0, no FILE_PREVIEW on this
	 * upload). The server's rename-on-collision behaviour is
	 * unchanged. */
    (void)path;
    return 0;
}

/* Phase 5: hx_list_dir is gone. The files browser's remote provider
 * (files_remote_provider.c::remote_send_file_list) emits its own
 * HTLC_HDR_FILE_LIST with the provider as the signal-data carrier,
 * which is what lets the response route back through
 * hx_remote_files_provider_handle_file_list rather than the
 * deleted legacy gfile_list dispatcher. */

void
hx_make_dir (struct htlc_conn *htlc, char *path)
{
    guint16 hldirlen;
    guint8 *hldir;

    hldir = path_to_hldir (path, &hldirlen, 0);
    task_new (htlc, 0, 0, 0, "mkdir");
    hlwrite (htlc, HTLC_HDR_FILE_MKDIR, 0, 1, HTLC_DATA_DIR, hldirlen, hldir);

    g_free (hldir);
}

void
hx_file_delete (struct htlc_conn *htlc, char *path)
{
    guint16 hldirlen;
    guint8 *hldir;
    char *file;

    task_new (htlc, 0, 0, 0, "rm");
    file = dirchar_basename (path);

    /* Phase E (follow-up): encode the filename. is_body = FALSE
	 * (filenames are single-line). */
    gboolean utf8 = (htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
    gsize file_len = 0;
    char *file_wire
        = gtkhx_text_for_wire (file, strlen (file), utf8, FALSE, &file_len);

    if (file != path) {
        hldir = path_to_hldir (path, &hldirlen, 1);
        hlwrite (htlc, HTLC_HDR_FILE_DELETE, 0, 2, HTLC_DATA_FILE_NAME,
                 (guint16)file_len, file_wire, HTLC_DATA_DIR, hldirlen, hldir);
        g_free (hldir);
    } else {
        hlwrite (htlc, HTLC_HDR_FILE_DELETE, 0, 1, HTLC_DATA_FILE_NAME,
                 (guint16)file_len, file_wire);
    }
    g_free (file_wire);
}
void
hx_file_info (struct htlc_conn *htlc, const char *dir_path,
              const char *file_name, gsize file_name_len)
{
    guint8 *hldir;
    guint16 hldirlen;
    char *task_label;

    /* task_new captures a copy of a path-shaped string for display
	 * in the tasks window; rcv_task_file_getinfo also forwards it
	 * to the file-info widget. Build a display string from dir +
	 * name; the display layer just shows it, doesn't split it back. */
    if (dir_path && *dir_path
        && !(dir_path[0] == (char)dir_char && dir_path[1] == 0)) {
        task_label = g_strdup_printf ("%s%c%.*s", dir_path, (char)dir_char,
                                      (int)file_name_len, file_name);
    } else {
        task_label = g_strndup (file_name, file_name_len);
    }
    task_new (htlc, RCV_TASK_FN (rcv_task_file_getinfo), task_label, 0,
              "finfo");

    /* Phase E (follow-up): encode FILE_NAME for the wire. The
	 * dir_path portion is built into a DIR chunk by path_to_hldir
	 * which copies the bytes verbatim — same encoding shape as
	 * other DIR-chunk sends (deferred for now; ASCII paths are
	 * the overwhelming common case). */
    gboolean utf8 = (htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
    gsize name_len = 0;
    char *name_wire = gtkhx_text_for_wire (file_name, file_name_len, utf8,
                                           FALSE, &name_len);

    if (dir_path && *dir_path
        && !(dir_path[0] == (char)dir_char && dir_path[1] == 0)) {
        hldir = path_to_hldir (dir_path, &hldirlen, 0);
        hlwrite (htlc, HTLC_HDR_FILE_GETINFO, 0, 2, HTLC_DATA_FILE_NAME,
                 (guint16)name_len, name_wire, HTLC_DATA_DIR, hldirlen, hldir);
        g_free (hldir);
    } else {
        hlwrite (htlc, HTLC_HDR_FILE_GETINFO, 0, 1, HTLC_DATA_FILE_NAME,
                 (guint16)name_len, name_wire);
    }
    g_free (name_wire);
}

void
hx_put_file (struct htlc_conn *htlc, char *lpath, char *rpath)
{
    struct htxf_conn *htxf;
    const char *base;
    char rdir[MAXPATHLEN];
    gsize dir_len;

    /* The caller still passes a flat rpath here — uploads pick
	 * the upload target via a local file picker, so the filename
	 * portion is whatever POSIX rules permit (no `/`). Split off
	 * the last component for the structured xfer_new call. */
    base = dirchar_basename (rpath);
    dir_len = (gsize)(base - rpath);
    if (dir_len >= sizeof rdir) {
        dir_len = sizeof rdir - 1;
    }
    memcpy (rdir, rpath, dir_len);
    rdir[dir_len] = 0;
    /* Strip trailing dir_char if present so xfer_go's "is this
	 * just the root?" test matches the local-path expectation. */
    if (dir_len > 1 && rdir[dir_len - 1] == (char)dir_char) {
        rdir[dir_len - 1] = 0;
    }

    /* Uploads don't use srv_data_size — that's a download-side
	 * heuristic for resume vs rename. */
    htxf = xfer_new (lpath, rdir, base, strlen (base), XFER_PUT, 0, 0);
    htxf->filter_argv = 0;
    htxf->opt.retry = 0;
}

void
hx_get_folder (struct htlc_conn *htlc, const char *lpath_root, const char *rdir,
               const char *name, gsize name_len)
{
    struct htxf_conn *htxf;
    char lpath[MAXPATHLEN];
    char rdir_buf[MAXPATHLEN];
    guint16 hldirlen = 0;
    guint8 *hldir = NULL;
    gsize rdir_len;

    if (!name_len) {
        return;
    }

    /* Build the local destination root: lpath_root + '/' + name.
	 * folder_get_thread snapshots this as base_path and rebuilds
	 * the full per-file path inside its loop. */
    {
        gsize root_len = strlen (lpath_root);
        gsize sep = (root_len > 0 && lpath_root[root_len - 1] != '/') ? 1 : 0;
        if (root_len + sep + name_len + 1 > sizeof (lpath)) {
            return;
        }
        memcpy (lpath, lpath_root, root_len);
        if (sep) {
            lpath[root_len] = '/';
        }
        memcpy (lpath + root_len + sep, name, name_len);
        lpath[root_len + sep + name_len] = 0;
    }

    /* The remote directory for the GETFOLDER request is the
	 * parent — the basename of the folder is the FILE_NAME chunk.
	 * The wire framing is the same as FILE_GET in that respect. */
    rdir_len = rdir ? strlen (rdir) : 0;
    if (rdir_len >= sizeof (rdir_buf)) {
        rdir_len = sizeof (rdir_buf) - 1;
    }
    memcpy (rdir_buf, rdir ? rdir : "", rdir_len);
    rdir_buf[rdir_len] = 0;

    htxf = xfer_new_folder (lpath, rdir_buf, name, name_len, XFER_GET);
    htxf->filter_argv = 0;
    htxf->opt.retry = 0;

    /* Register the rcv callback BEFORE sending the request — the
	 * task's trans id is captured by hlwrite and routed back to
	 * rcv_task_folder_get by hx_rcv_task. */
    task_new (htlc, RCV_TASK_FN (rcv_task_folder_get), htxf, 0,
              "xfer_go_folder");
    /* Phase E (follow-up): encode the folder name for the wire. */
    gboolean utf8 = (htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
    gsize name_wire_len = 0;
    char *name_wire
        = gtkhx_text_for_wire (name, name_len, utf8, FALSE, &name_wire_len);

    if (rdir_buf[0] && !(rdir_buf[0] == (char)dir_char && rdir_buf[1] == 0)) {
        hldir = path_to_hldir (rdir_buf, &hldirlen, 0);
        hlwrite (htlc, HTLC_HDR_FILE_GETFOLDER, 0, 2, HTLC_DATA_FILE_NAME,
                 (guint16)name_wire_len, name_wire, HTLC_DATA_DIR, hldirlen,
                 hldir);
        g_free (hldir);
    } else {
        hlwrite (htlc, HTLC_HDR_FILE_GETFOLDER, 0, 1, HTLC_DATA_FILE_NAME,
                 (guint16)name_wire_len, name_wire);
    }
    g_free (name_wire);
}

/* Walk a local directory tree and sum the on-disk sizes of all
 * regular files. The aggregate goes into HTLC_DATA_HTXF_SIZE on
 * the PUTFOLDER request so the server has something sensible to
 * display while the actual per-file sizes stream in. */
static void
hx_folder_aggregate (const char *root, guint64 *total_bytes_out,
                     guint32 *nfiles_out)
{
    GDir *d;
    const char *name;

    d = g_dir_open (root, 0, NULL);
    if (!d) {
        return;
    }
    while ((name = g_dir_read_name (d))) {
        struct stat sb;
        char *full = g_build_filename (root, name, NULL);
        if (lstat (full, &sb) == 0) {
            if (S_ISDIR (sb.st_mode)) {
                hx_folder_aggregate (full, total_bytes_out, nfiles_out);
            } else if (S_ISREG (sb.st_mode)) {
                *total_bytes_out += (guint64)sb.st_size;
                (*nfiles_out)++;
            }
        }
        g_free (full);
    }
    g_dir_close (d);
}

void
hx_put_folder (struct htlc_conn *htlc, const char *lpath, const char *rdir,
               const char *name, gsize name_len)
{
    struct htxf_conn *htxf;
    char rdir_buf[MAXPATHLEN];
    guint16 hldirlen = 0;
    guint8 *hldir = NULL;
    gsize rdir_len;
    guint64 total_bytes = 0;
    guint32 nfiles = 0;
    guint32 size_n;
    guint32 nfiles_n;

    if (!name_len) {
        return;
    }

    /* Pre-walk for the SIZE / NFILES chunks. The server uses
	 * these for the queue/display, not for framing. */
    hx_folder_aggregate (lpath, &total_bytes, &nfiles);
    /* HTLC_DATA_HTXF_SIZE is u32; clamp on overflow. */
    if (total_bytes > G_MAXUINT32) {
        size_n = htonl (G_MAXUINT32);
    } else {
        size_n = htonl ((guint32)total_bytes);
    }
    nfiles_n = htonl (nfiles);

    rdir_len = rdir ? strlen (rdir) : 0;
    if (rdir_len >= sizeof (rdir_buf)) {
        rdir_len = sizeof (rdir_buf) - 1;
    }
    memcpy (rdir_buf, rdir ? rdir : "", rdir_len);
    rdir_buf[rdir_len] = 0;

    htxf = xfer_new_folder (lpath, rdir_buf, name, name_len, XFER_PUT);
    htxf->filter_argv = 0;
    htxf->opt.retry = 0;
    /* Stash the aggregate up front; folder_put_thread fills
	 * total_pos as the stream progresses. */
    if (total_bytes > G_MAXUINT32) {
        htxf->total_size = G_MAXUINT32;
    } else if (total_bytes > 0) {
        htxf->total_size = (guint32)total_bytes;
    } else {
        htxf->total_size = 1;
    }

    task_new (htlc, RCV_TASK_FN (rcv_task_folder_put), htxf, 0,
              "xfer_go_folder");

    /* Phase E (follow-up): encode the folder name. */
    gboolean utf8 = (htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
    gsize name_wire_len = 0;
    char *name_wire
        = gtkhx_text_for_wire (name, name_len, utf8, FALSE, &name_wire_len);

    if (rdir_buf[0] && !(rdir_buf[0] == (char)dir_char && rdir_buf[1] == 0)) {
        hldir = path_to_hldir (rdir_buf, &hldirlen, 0);
        hlwrite (htlc, HTLC_HDR_FILE_PUTFOLDER, 0, 4, HTLC_DATA_FILE_NAME,
                 (guint16)name_wire_len, name_wire, HTLC_DATA_DIR, hldirlen,
                 hldir, HTLC_DATA_HTXF_SIZE, 4, &size_n, HTLC_DATA_FILE_NFILES,
                 4, &nfiles_n);
        g_free (hldir);
    } else {
        hlwrite (htlc, HTLC_HDR_FILE_PUTFOLDER, 0, 3, HTLC_DATA_FILE_NAME,
                 (guint16)name_wire_len, name_wire, HTLC_DATA_HTXF_SIZE, 4,
                 &size_n, HTLC_DATA_FILE_NFILES, 4, &nfiles_n);
    }
    g_free (name_wire);
}

void
hx_file_link (struct htlc_conn *htlc, char *src_path, char *dst_path)
{
    char *src_file, *dst_file;
    guint16 hldirlen, rnhldirlen;
    guint8 *hldir, *rnhldir;

    src_file = dirchar_basename (src_path);
    dst_file = dirchar_basename (dst_path);
    hldir = path_to_hldir (src_path, &hldirlen, 1);
    rnhldir = path_to_hldir (dst_path, &rnhldirlen, 1);
    task_new (htlc, 0, 0, 0, "ln");

    /* Phase E (follow-up): encode src + dst basenames. */
    gboolean utf8 = (htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
    gsize src_len = 0, dst_len = 0;
    char *src_wire = gtkhx_text_for_wire (src_file, strlen (src_file), utf8,
                                          FALSE, &src_len);
    char *dst_wire = gtkhx_text_for_wire (dst_file, strlen (dst_file), utf8,
                                          FALSE, &dst_len);

    hlwrite (htlc, HTLC_HDR_FILE_SYMLINK, 0, 4, HTLC_DATA_FILE_NAME,
             (guint16)src_len, src_wire, HTLC_DATA_DIR, hldirlen, hldir,
             HTLC_DATA_DIR_RENAME, rnhldirlen, rnhldir, HTLC_DATA_FILE_RENAME,
             (guint16)dst_len, dst_wire);
    g_free (src_wire);
    g_free (dst_wire);
    g_free (rnhldir);
    g_free (hldir);
}

void
hx_file_move (struct htlc_conn *htlc, char *src_path, char *dst_path)
{
    char *dst_file, *src_file;
    guint16 hldirlen, rnhldirlen;
    guint8 *hldir, *rnhldir;
    size_t len;

    dst_file = dirchar_basename (dst_path);
    src_file = dirchar_basename (src_path);

    hldir = path_to_hldir (src_path, &hldirlen, 1);
    len = strlen (dst_path) - (strlen (dst_path) - (dst_file - dst_path));

    /* Phase E (follow-up): encode src + dst basenames. */
    gboolean utf8 = (htlc->caps & HTLC_CAP_TEXT_ENCODING) != 0;
    gsize src_len = 0, dst_len = 0;
    char *src_wire = gtkhx_text_for_wire (src_file, strlen (src_file), utf8,
                                          FALSE, &src_len);
    char *dst_wire = gtkhx_text_for_wire (dst_file, strlen (dst_file), utf8,
                                          FALSE, &dst_len);

    if (len
        && (len
                != strlen (src_path)
                       - (strlen (src_path) - (src_file - src_path))
            || memcmp (dst_path, src_path, len) != 0)) {
        rnhldir = path_to_hldir (dst_path, &rnhldirlen, 1);
        task_new (htlc, 0, 0, 0, "mv");
        hlwrite (htlc, HTLC_HDR_FILE_MOVE, 0, 3, HTLC_DATA_FILE_NAME,
                 (guint16)src_len, src_wire, HTLC_DATA_DIR, hldirlen, hldir,
                 HTLC_DATA_DIR_RENAME, rnhldirlen, rnhldir);
        g_free (rnhldir);
    }
    if (*dst_file && strcmp (src_file, dst_file) != 0) {
        task_new (htlc, 0, 0, 0, "mv");
        hlwrite (htlc, HTLC_HDR_FILE_SETINFO, 0, 3, HTLC_DATA_FILE_NAME,
                 (guint16)src_len, src_wire, HTLC_DATA_FILE_RENAME,
                 (guint16)dst_len, dst_wire, HTLC_DATA_DIR, hldirlen, hldir);
    }
    g_free (src_wire);
    g_free (dst_wire);
    g_free (hldir);
}
