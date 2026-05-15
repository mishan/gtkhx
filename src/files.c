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
#include "gtk_hlist.h"
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

/* fileutils-4.0/lib/human.c */
#define LONGEST_HUMAN_READABLE 32

const char human_suffixes[] = {
    0,   /* not used */
    'k', /* kilo */
    'M', /* Mega */
    'G', /* Giga */
    'T', /* Tera */
    'P', /* Peta */
    'E', /* Exa */
    'Z', /* Zetta */
    'Y'  /* Yotta */
};

/* Convert N to a human readable format in BUF.

   N is expressed in units of FROM_BLOCK_SIZE.  FROM_BLOCK_SIZE must
   be positive.

   If OUTPUT_BLOCK_SIZE is positive, use units of OUTPUT_BLOCK_SIZE in
   the output number.  OUTPUT_BLOCK_SIZE must be a multiple of
   FROM_BLOCK_SIZE or vice versa.

   If OUTPUT_BLOCK_SIZE is negative, use a format like "127k" if
   possible, using powers of -OUTPUT_BLOCK_SIZE; otherwise, use
   ordinary decimal format.  Normally -OUTPUT_BLOCK_SIZE is either
   1000 or 1024; it must be at least 2.  Most people visually process
   strings of 3-4 digits effectively, but longer strings of digits are
   more prone to misinterpretation.  Hence, converting to an
   abbreviated form usually improves readability.  Use a suffix
   indicating which power is being used.  For example, assuming
   -OUTPUT_BLOCK_SIZE is 1024, 8500 would be converted to 8.3k,
   133456345 to 127M, 56990456345 to 53G, and so on.  Numbers smaller
   than -OUTPUT_BLOCK_SIZE aren't modified.  */

static char *
human_readable (guint32 n, char *buf, int from_block_size,
                int output_block_size)
{
    guint32 amt;
    uint base;
    int to_block_size;
    uint tenths;
    uint power = 0;
    char *p;

    /* 0 means adjusted N == AMT.TENTHS;
     1 means AMT.TENTHS < adjusted N < AMT.TENTHS + 0.05;
     2 means adjusted N == AMT.TENTHS + 0.05;
     3 means AMT.TENTHS + 0.05 < adjusted N < AMT.TENTHS + 0.1.  */
    uint rounding;

    if (output_block_size < 0) {
        base = -output_block_size;
        to_block_size = 1;
    } else {
        base = 0;
        to_block_size = output_block_size;
    }

    p = buf + LONGEST_HUMAN_READABLE;
    *p = '\0';

    /* Adjust AMT out of FROM_BLOCK_SIZE units and into TO_BLOCK_SIZE units.  */

    if (to_block_size <= from_block_size) {
        int multiplier = from_block_size / to_block_size;
        amt = n * multiplier;
        tenths = rounding = 0;

        if (amt / multiplier != n) {
            /* Overflow occurred during multiplication.  We should use
	     multiple precision arithmetic here, but we'll be lazy and
	     resort to floating point.  This can yield answers that
	     are slightly off.  In practice it is quite rare to
	     overflow uintmax_t, so this is good enough for now.  */

            double damt = n * (double)multiplier;

            if (!base) {
                g_snprintf (buf, LONGEST_HUMAN_READABLE, "%.0f", damt);
            } else {
                double e = 1;
                power = 0;

                do {
                    e *= base;
                    power++;
                } while (e * base <= damt
                         && power < sizeof (human_suffixes) - 1);

                damt /= e;

                g_snprintf (buf, LONGEST_HUMAN_READABLE, "%.1f%c", damt,
                            human_suffixes[power]);
                if (4 < strlen (buf)) {
                    g_snprintf (buf, LONGEST_HUMAN_READABLE, "%.0f%c", damt,
                                human_suffixes[power]);
                }
            }

            return buf;
        }
    } else {
        uint divisor = to_block_size / from_block_size;
        uint r10 = (n % divisor) * 10;
        uint r2 = (r10 % divisor) * 2;
        amt = n / divisor;
        tenths = r10 / divisor;
        rounding = r2 < divisor ? 0 < r2 : 2 + (divisor < r2);
    }

    /* Use power of BASE notation if adjusted AMT is large enough.  */

    if (base && base <= amt) {
        power = 0;

        do {
            uint r10 = (amt % base) * 10 + tenths;
            uint r2 = (r10 % base) * 2 + (rounding >> 1);
            amt /= base;
            tenths = r10 / base;
            rounding
                = (r2 < base ? 0 < r2 + rounding : 2 + (base < r2 + rounding));
            power++;
        } while (base <= amt && power < sizeof (human_suffixes) - 1);

        *--p = human_suffixes[power];

        tenths += 2 < rounding + (tenths & 1);

        if (tenths == 10) {
            amt++;
            tenths = 0;
        }

        *--p = '0' + tenths;
        *--p = '.';
        tenths = 0;
    }

    if (5 < tenths + (2 < rounding + (amt & 1))) {
        amt++;

        if (amt == base && power < sizeof (human_suffixes) - 1) {
            *p = human_suffixes[power + 1];
            *--p = '0';
            *--p = '.';
            amt = 1;
        }
    }

    do {
        *--p = '0' + (int)(amt % 10);
    } while ((amt /= 10) != 0);

    return p;
}

char *
human_size (char *sizstr, guint32 size)
{
    return human_readable (size, sizstr, 1, -1024);
}

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
    if (file != path) {
        guint16 hldirlen = 0;
        guint8 *hldir = path_to_hldir (path, &hldirlen, 1);
        hlwrite (&the_session.htlc, HTLC_HDR_FILE_SETINFO, 0, 4,
                 HTLC_DATA_FILE_NAME, strlen (file), file,
                 HTLC_DATA_FILE_RENAME, strlen (name), name,
                 HTLC_DATA_FILE_COMMENT, strlen (comments), comments,
                 HTLC_DATA_DIR, hldirlen, hldir);
        g_free (hldir);
    } else {
        hlwrite (&the_session.htlc, HTLC_HDR_FILE_SETINFO, 0, 3,
                 HTLC_DATA_FILE_NAME, strlen (file), file,
                 HTLC_DATA_FILE_RENAME, strlen (name), name,
                 HTLC_DATA_FILE_COMMENT, strlen (comments), comments);
    }

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
                  char *comments, char *modified, char *created, guint32 size)
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
            g_snprintf (sizestr, sizeof sizestr, "%s (%u %s)", human, size,
                        _ ("bytes"));
        } else {
            g_snprintf (sizestr, sizeof sizestr, "%u %s", size, _ ("bytes"));
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

struct x_fhdr {
    guint16 enc PACKED;
    guint8 len, name[1];
};

guint8 *
path_to_hldir (const char *path, guint16 *hldirlen, int is_file)
{
    guint8 *hldir;
    struct x_fhdr *fh;
    char const *p, *p2;
    guint16 pos = 2, dc = 0;
    guint8 nlen;

    hldir = g_malloc (2);
    p = path;
    while ((p2 = strchr (p, dir_char))) {
        if (!(p2 - p)) {
            p++;
            continue;
        }
        nlen = (guint8)(p2 - p);
        pos += 3 + nlen;
        hldir = g_realloc (hldir, pos);
        fh = (struct x_fhdr *)(&(hldir[pos - (3 + nlen)]));
        memset (&fh->enc, 0, 2);
        fh->len = nlen;
        memcpy (fh->name, p, nlen);
        dc++;
        p = p2 + 1;
    }
    if (!is_file && *p) {
        nlen = (guint8)strlen (p);
        pos += 3 + nlen;
        hldir = g_realloc (hldir, pos);
        fh = (struct x_fhdr *)(&(hldir[pos - (3 + nlen)]));
        memset (&fh->enc, 0, 2);
        fh->len = nlen;
        memcpy (fh->name, p, nlen);
        dc++;
    }
    *((guint16 *)hldir) = htons (dc);

    *hldirlen = pos;
    return hldir;
}

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

void
dirmask (char *dst, char *src, char *mask)
{
    while (*mask && *src && *mask++ == *src++)
        ;

    strcpy (dst, src);
}

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
    if (file != path) {
        hldir = path_to_hldir (path, &hldirlen, 1);
        hlwrite (htlc, HTLC_HDR_FILE_DELETE, 0, 2, HTLC_DATA_FILE_NAME,
                 strlen (file), file, HTLC_DATA_DIR, hldirlen, hldir);
        g_free (hldir);
    } else {
        hlwrite (htlc, HTLC_HDR_FILE_DELETE, 0, 1, HTLC_DATA_FILE_NAME,
                 strlen (file), file);
    }
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

    /* Wire FILE_NAME is the name verbatim — no split needed because
	 * we never joined. */
    if (dir_path && *dir_path
        && !(dir_path[0] == (char)dir_char && dir_path[1] == 0)) {
        hldir = path_to_hldir (dir_path, &hldirlen, 0);
        hlwrite (htlc, HTLC_HDR_FILE_GETINFO, 0, 2, HTLC_DATA_FILE_NAME,
                 file_name_len, file_name, HTLC_DATA_DIR, hldirlen, hldir);
        g_free (hldir);
    } else {
        hlwrite (htlc, HTLC_HDR_FILE_GETINFO, 0, 1, HTLC_DATA_FILE_NAME,
                 file_name_len, file_name);
    }
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
    hlwrite (htlc, HTLC_HDR_FILE_SYMLINK, 0, 4, HTLC_DATA_FILE_NAME,
             strlen (src_file), src_file, HTLC_DATA_DIR, hldirlen, hldir,
             HTLC_DATA_DIR_RENAME, rnhldirlen, rnhldir, HTLC_DATA_FILE_RENAME,
             strlen (dst_file), dst_file);
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
    if (len
        && (len
                != strlen (src_path)
                       - (strlen (src_path) - (src_file - src_path))
            || memcmp (dst_path, src_path, len))) {
        rnhldir = path_to_hldir (dst_path, &rnhldirlen, 1);
        task_new (htlc, 0, 0, 0, "mv");
        hlwrite (htlc, HTLC_HDR_FILE_MOVE, 0, 3, HTLC_DATA_FILE_NAME,
                 strlen (src_file), src_file, HTLC_DATA_DIR, hldirlen, hldir,
                 HTLC_DATA_DIR_RENAME, rnhldirlen, rnhldir);
        g_free (rnhldir);
    }
    if (*dst_file && strcmp (src_file, dst_file)) {
        task_new (htlc, 0, 0, 0, "mv");
        hlwrite (htlc, HTLC_HDR_FILE_SETINFO, 0, 3, HTLC_DATA_FILE_NAME,
                 strlen (src_file), src_file, HTLC_DATA_FILE_RENAME,
                 strlen (dst_file), dst_file, HTLC_DATA_DIR, hldirlen, hldir);
    }
    g_free (hldir);
}
