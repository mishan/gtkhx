/* X-Chat
 * Copyright (C) 1998 Peter Zelezny.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 * =========================================================================
 *
 * xtext, the text widget used by X-Chat.
 * By Peter Zelezny <zed@xchat.org>.
 *
 */

/*
 * GtkHx Phase 2.6: this file is HexChat's xtext.c (commit master, 2026-04)
 * with hexchat-specific dependencies stripped.  See xtext.h for the list
 * of edits.  The diff against upstream is intentionally narrow so we can
 * pull future bug fixes by re-running the same elision pass.
 *
 * To keep that diff narrow — and because hexchat's xtext picked specific
 * char/uchar conventions and inner-shadowing patterns we won't be undoing
 * — suppress the warnings the vendored code naturally trips on:
 *   -Wpointer-sign           (gchar/unsigned-char mixing on strings)
 *   -Wshadow                 (small inner-scope re-declarations)
 *   -Wsign-compare           (comparisons mixing signed/unsigned ints)
 *   -Wcast-function-type     (GtkXText class_init / instance_init casts)
 *   -Wimplicit-fallthrough   (one deliberate fall-through in find_next_wrap)
 *
 * Glue replaced relative to upstream:
 *   - hexchat config.h / fe-gtk.h / fkeys.h includes are gone
 *   - prefs.hex_text_autocopy_*, hex_stamp_text, hex_text_indent live in
 *     a tiny static `prefs` struct below (autocopy = off; matches the
 *     original GtkHx behavior of not silently copying selections)
 *   - STATE_SHIFT / STATE_CTRL: GDK modifier macros, defined here
 *   - safe_strcpy / nocasestrstr: tiny implementations below
 *   - url_last(): stub returning 0 (no URL highlight tracking yet — the
 *     widget still calls gtkhx_prefs's word_check via urlcheck_function,
 *     this only affects sub-word URL match offsets)
 *   - xtext_get_stamp_str(): strftime("%H:%M:%S ") wrapper, no per-user
 *     hex_stamp_text_format pref
 *   - _hexchat_marshal_VOID__POINTER_POINTER / OBJECT_OBJECT: replaced
 *     with g_cclosure_marshal_generic
 *   - gtk_xtext_set_marker_last(session*) removed
 */

/* See the file-header comment for the rationale on these. */
#pragma GCC diagnostic ignored "-Wpointer-sign"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

#define GDK_MULTIHEAD_SAFE
#define MARGIN 2						/* dont touch. */
#define REFRESH_TIMEOUT 20
#define WORDWRAP_LIMIT 24

#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
/* Phase 4.9: gdk/gdkx.h is the X11-specific GDK backend header. GTK 4
 * makes Wayland the assumed backend (per ROADMAP §Phase 4 gotchas) and
 * gdkx.h's contents (GdkX11Display* types, gdk_x11_*) aren't part of
 * what xtext touches anyway — the include was vestigial from XChat
 * 1.8.5's X11-era assumptions. Drop. */

#include "config.h"
#include "xtext.h"
#include "debug.h"

#define charlen(str) g_utf8_skip[*(guchar *)(str)]

/* --- HexChat glue: keyboard-modifier name aliases -------------------- */
#define STATE_SHIFT		GDK_SHIFT_MASK
#define STATE_ALT		GDK_MOD1_MASK
#define STATE_CTRL		GDK_CONTROL_MASK

/* --- HexChat glue: prefs subset that xtext.c reads ------------------- *
 * The original HexChat code consults global `prefs` for autocopy, stamp,
 * and indent behavior.  GtkHx never offered those toggles; defaults
 * here match the historical GtkHx behavior (no autocopy on selection;
 * timestamps render inline, not as a left indent column).
 *
 * Phase 5 first tried flipping hex_text_autocopy_text to 1 so a
 * drag-select would auto-populate the regular and primary
 * clipboards. That broke visible highlighting in the xtext widget:
 * drags no longer left a visible mark. Reverted to 0; copy from
 * chat / PM / news will be wired through a window-level Ctrl+C
 * shortcut and/or a GdkContentProvider that returns the current
 * selection on demand, neither of which touches the gesture's
 * drag-end render path.
 */
struct hexchat_prefs_subset {
	int hex_text_autocopy_text;
	int hex_text_autocopy_color;
	int hex_text_autocopy_stamp;
	int hex_stamp_text;
	int hex_text_indent;
};
/* Phase 5: not const — Settings drives autocopy_text / _stamp / _color
 * via gtk_xtext_set_autocopy_* (declared in xtext.h). The other two
 * fields stay 0; see comment above for the inline timestamps / no
 * left-column indent baseline. */
static struct hexchat_prefs_subset prefs = {
	0,	/* hex_text_autocopy_text — Settings → "Automatically copy
	     *   selected text". Default off; the Ctrl-C window shortcut
	     *   handles copy without going through drag-end's render path,
	     *   so the user can leave this off without losing clipboard. */
	0,	/* hex_text_autocopy_color — Settings → "Automatically include
	     *   color information". */
	0,	/* hex_text_autocopy_stamp — Settings → "Automatically include
	     *   timestamps". */
	0,	/* hex_stamp_text — drawn inline, no left-column timestamps */
	0,	/* hex_text_indent — corresponds to disabled */
};

void gtk_xtext_set_autocopy_text  (gboolean enabled)
{
	prefs.hex_text_autocopy_text  = enabled ? 1 : 0;
}
void gtk_xtext_set_autocopy_stamp (gboolean enabled)
{
	prefs.hex_text_autocopy_stamp = enabled ? 1 : 0;
}
void gtk_xtext_set_autocopy_color (gboolean enabled)
{
	prefs.hex_text_autocopy_color = enabled ? 1 : 0;
}

/* gtk_xtext_set_stamp_format is defined later (after set_font where
 * the internal helpers it calls — text_width, fix_indent,
 * recalc_widths — are in scope). Setting xtext_stamp_format alone
 * (without a widget handle) is also legal: see the static-store
 * path used by xtext_get_stamp_str at default-fallback time. */

/* --- HexChat glue: safe_strcpy from common/util.c -------------------- */
static inline void
safe_strcpy (char *dest, const char *src, int bytes_left)
{
	if (bytes_left < 1) return;
	g_strlcpy (dest, src, bytes_left);
}

/* --- HexChat glue: case-insensitive substring search from util.c ----- *
 * Currently only the lastlog/search code paths reach this; if those
 * are excluded by the consumer, GCC flags it as unused.  G_GNUC_UNUSED
 * suppresses that without removing the function. */
G_GNUC_UNUSED static char *
nocasestrstr (const char *s, const char *find)
{
	gsize len = strlen (find);
	if (len == 0)
		return (char *) s;

	while (*s) {
		if (g_ascii_strncasecmp (s, find, len) == 0)
			return (char *) s;
		s++;
	}
	return NULL;
}

/* --- HexChat glue: URL match cursor (from common/url.c) ------------- *
 * In HexChat, the urlcheck_function callback updates a thread-local pair
 * of last-match indices; xtext.c reads them back to highlight the
 * matched substring within the word the cursor is over.  GtkHx's
 * word_check returns a boolean and doesn't track sub-word offsets, so
 * we report "no last match" and the widget falls back to whole-word
 * URL behavior.  Phase 3+ can revisit if we want sub-word URL hover. */
static int
url_last (int *lstart, int *lend)
{
	*lstart = 0;
	*lend = 0;
	return 0;
}

/* --- HexChat glue: timestamp formatter ----------------------------- *
 * Phase 5: format string is configurable via Settings → Chat →
 * Timestamp format, persisted as CFG_STAMP_FORMAT. xtext stores a
 * private copy of the string here (single per-process, since every
 * xtext widget shares the same stamp format). gtk_xtext_set_stamp_format
 * updates it; the static default fires when no Settings value has
 * been applied yet (matches HexChat's bare-time default plus our
 * bracketed convention). */
static char *xtext_stamp_format = NULL;

#define XTEXT_STAMP_FORMAT_DEFAULT "[%H:%M:%S] "

static int
xtext_get_stamp_str (time_t tim, char **ret)
{
	char buf[64];
	struct tm tmv;
	int len;
	const char *fmt = xtext_stamp_format && *xtext_stamp_format
	                ? xtext_stamp_format
	                : XTEXT_STAMP_FORMAT_DEFAULT;

	localtime_r (&tim, &tmv);
	len = strftime (buf, sizeof buf, fmt, &tmv);
	if (len <= 0) {
		*ret = g_strdup ("");
		return 0;
	}
	*ret = g_strdup (buf);
	return len;
}

/* is delimiter */
#define is_del(c) \
	(c == ' ' || c == '\n' || c == '>' || c == '<' || c == 0)

/* force scrolling off */
#define dontscroll(buf) (buf)->last_pixel_pos = 0x7fffffff

static GtkWidgetClass *parent_class = NULL;

struct textentry
{
	struct textentry *next;
	struct textentry *prev;
	unsigned char *str;
	time_t stamp;
	gint16 str_width;
	gint16 str_len;
	gint16 mark_start;
	gint16 mark_end;
	gint16 indent;
	gint16 left_len;
	GSList *slp;
	GSList *sublines;
	guchar tag;
	guchar pad1;
	guchar pad2;	/* 32-bit align : 44 bytes total */
	GList *marks;	/* List of found strings */
};

enum
{
	WORD_CLICK,
	SET_SCROLL_ADJUSTMENTS,
	LAST_SIGNAL
};

static guint xtext_signals[LAST_SIGNAL];

/* nocasestrstr() and xtext_get_stamp_str() are defined locally in the
 * glue block above; upstream's extern declarations are dropped. */
static void gtk_xtext_render_page (GtkXText * xtext);
static void gtk_xtext_calc_lines (xtext_buffer *buf, int);
static gboolean gtk_xtext_is_selecting (GtkXText *xtext);
static char *gtk_xtext_selection_get_text (GtkXText *xtext, int *len_ret);
static textentry *gtk_xtext_nth (GtkXText *xtext, int line, int *subline);
static void gtk_xtext_adjustment_changed (GtkAdjustment * adj,
														GtkXText * xtext);
static void gtk_xtext_scroll_adjustments (GtkXText *xtext, GtkAdjustment *hadj,
										GtkAdjustment *vadj);
static int gtk_xtext_render_ents (GtkXText * xtext, textentry *, textentry *);
static void gtk_xtext_recalc_widths (xtext_buffer *buf, int);
static void gtk_xtext_fix_indent (xtext_buffer *buf);
static int gtk_xtext_find_subline (GtkXText *xtext, textentry *ent, int line);
/* static char *gtk_xtext_conv_color (unsigned char *text, int len, int *newlen); */
/* For use by gtk_xtext_strip_color() and its callers -- */
struct offlen_s {
	guint16 off;
	guint16 len;
	guint16 emph;
	guint16 width;
};
typedef struct offlen_s offlen_t;
static unsigned char *
gtk_xtext_strip_color (unsigned char *text, int len, unsigned char *outbuf,
							  int *newlen, GSList **slp, int strip_hidden);
static gboolean gtk_xtext_check_ent_visibility (GtkXText * xtext, textentry *find_ent, int add);
static int gtk_xtext_render_page_timeout (GtkXText * xtext);
static int gtk_xtext_search_offset (xtext_buffer *buf, textentry *ent, unsigned int off);
static GList * gtk_xtext_search_textentry (xtext_buffer *, textentry *);
static void gtk_xtext_search_textentry_add (xtext_buffer *, textentry *, GList *, gboolean);
static void gtk_xtext_search_textentry_del (xtext_buffer *, textentry *);
static void gtk_xtext_search_textentry_fini (gpointer, gpointer);
static void gtk_xtext_search_fini (xtext_buffer *);
static gboolean gtk_xtext_search_init (xtext_buffer *buf, const gchar *text, gtk_xtext_search_flags flags, GError **perr);
static char * gtk_xtext_get_word (GtkXText * xtext, int x, int y, textentry ** ret_ent, int *ret_off, int *ret_len, GSList **slp);
static gboolean gtk_xtext_scroll_cb (GtkEventControllerScroll *controller,
                                     gdouble dx, gdouble dy, gpointer user_data);
static void gtk_xtext_pressed_cb     (GtkGestureClick *gesture, gint n_press,
                                      gdouble dx, gdouble dy, gpointer user_data);
static void gtk_xtext_drag_begin_cb  (GtkGestureDrag *drag, gdouble x, gdouble y,
                                      gpointer user_data);
static void gtk_xtext_drag_update_cb (GtkGestureDrag *drag, gdouble offset_x,
                                      gdouble offset_y, gpointer user_data);
static void gtk_xtext_drag_end_cb    (GtkGestureDrag *drag, gdouble offset_x,
                                      gdouble offset_y, gpointer user_data);
static void gtk_xtext_motion_cb      (GtkEventControllerMotion *controller,
                                      gdouble dx, gdouble dy, gpointer user_data);
static void gtk_xtext_leave_cb       (GtkEventControllerMotion *controller, gpointer user_data);

/* Phase 3.10: palette colors are GdkRGBA, which is what
 * gdk_cairo_set_source_rgba consumes — no per-draw conversion. */
static inline void
xtext_cairo_set_source_color (cairo_t *cr, const GdkRGBA *col)
{
	gdk_cairo_set_source_rgba (cr, (GdkRGBA *) col);
}

static inline void
xtext_cairo_set_source_idx (GtkXText *xt, cairo_t *cr, int idx)
{
	xtext_cairo_set_source_color (cr, &xt->palette[idx]);
}

/* Background fill: either background pattern (if user set one) or
 * solid palette[XTEXT_BG] / palette[xtext->cur_bg]. */
static void
xtext_draw_bg (GtkXText *xt, int x, int y, int w, int h)
{
	cairo_t *cr = xt->cr;
	if (cr == NULL)
	{
		/* Phase 4.9: gtk_widget_queue_draw_area is gone — GTK 4 redraws
		 * always cover the full widget. Position information is
		 * unused, so the area-specific call collapses to a plain
		 * queue_draw. */
		(void) x; (void) y; (void) w; (void) h;
		gtk_widget_queue_draw (GTK_WIDGET (xt));
		return;
	}
	cairo_save (cr);
	xtext_cairo_set_source_idx (xt, cr, xt->cur_bg);
	cairo_rectangle (cr, x, y, w, h);
	cairo_fill (cr);
	cairo_restore (cr);
}

/* ======================================= */
/* ============ PANGO BACKEND ============ */
/* ======================================= */

#define EMPH_ITAL 1
#define EMPH_BOLD 2
#define EMPH_HIDDEN 4
static PangoAttrList *attr_lists[4];
static int fontwidths[4][128];

static PangoAttribute *
xtext_pango_attr (PangoAttribute *attr)
{
	attr->start_index = PANGO_ATTR_INDEX_FROM_TEXT_BEGINNING;
	attr->end_index = PANGO_ATTR_INDEX_TO_TEXT_END;
	return attr;
}

static void
xtext_pango_init (GtkXText *xtext)
{
	size_t i;
	int j;
	char buf[2] = "\000";

	if (attr_lists[0])
	{
		for (i = 0; i < (EMPH_ITAL | EMPH_BOLD); i++)
			pango_attr_list_unref (attr_lists[i]);
	}

	for (i = 0; i < sizeof attr_lists / sizeof attr_lists[0]; i++)
	{
		attr_lists[i] = pango_attr_list_new ();
		switch (i)
		{
		case 0:		/* Roman */
			break;
		case EMPH_ITAL:		/* Italic */
			pango_attr_list_insert (attr_lists[i],
				xtext_pango_attr (pango_attr_style_new (PANGO_STYLE_ITALIC)));
			break;
		case EMPH_BOLD:		/* Bold */
			pango_attr_list_insert (attr_lists[i],
				xtext_pango_attr (pango_attr_weight_new (PANGO_WEIGHT_BOLD)));
			break;
		case EMPH_ITAL | EMPH_BOLD:		/* Italic Bold */
			pango_attr_list_insert (attr_lists[i],
				xtext_pango_attr (pango_attr_style_new (PANGO_STYLE_ITALIC)));
			pango_attr_list_insert (attr_lists[i],
				xtext_pango_attr (pango_attr_weight_new (PANGO_WEIGHT_BOLD)));
			break;
		}

		/* Now initialize fontwidths[i] */
		pango_layout_set_attributes (xtext->layout, attr_lists[i]);
		for (j = 0; j < 128; j++)
		{
			buf[0] = j;
			pango_layout_set_text (xtext->layout, buf, 1);
			pango_layout_get_pixel_size (xtext->layout, &fontwidths[i][j], NULL);
		}
	}
	xtext->space_width = fontwidths[0][' '];
}

static void
backend_font_close (GtkXText *xtext)
{
	pango_font_description_free (xtext->font->font);
}

static void
backend_init (GtkXText *xtext)
{
	if (xtext->layout == NULL)
	{
		xtext->layout = gtk_widget_create_pango_layout (GTK_WIDGET (xtext), 0); 
		if (xtext->font)
			pango_layout_set_font_description (xtext->layout, xtext->font->font);
	}
}

static void
backend_deinit (GtkXText *xtext)
{
	if (xtext->layout)
	{
		g_object_unref (xtext->layout);
		xtext->layout = NULL;
	}
}

static PangoFontDescription *
backend_font_open_real (char *name)
{
	PangoFontDescription *font;

	font = pango_font_description_from_string (name);
	if (font && pango_font_description_get_size (font) == 0)
	{
		pango_font_description_free (font);
		font = pango_font_description_from_string ("sans 11");
	}
	if (!font)
		font = pango_font_description_from_string ("sans 11");

	return font;
}

static void
backend_font_open (GtkXText *xtext, char *name)
{
	PangoLanguage *lang;
	PangoContext *context;
	PangoFontMetrics *metrics;

	xtext->font = &xtext->pango_font;
	xtext->font->font = backend_font_open_real (name);
	if (!xtext->font->font)
	{
		xtext->font = NULL;
		return;
	}

	backend_init (xtext);
	pango_layout_set_font_description (xtext->layout, xtext->font->font);
	xtext_pango_init (xtext);

	/* vte does it this way */
	context = gtk_widget_get_pango_context (GTK_WIDGET (xtext));
	lang = pango_context_get_language (context);
	metrics = pango_context_get_metrics (context, xtext->font->font, lang);
	xtext->font->ascent = pango_font_metrics_get_ascent (metrics) / PANGO_SCALE;
	xtext->font->descent = pango_font_metrics_get_descent (metrics) / PANGO_SCALE;

	/*
	 * In later versions of pango, a font's height should be calculated like
	 * this to account for line gap; a typical symptom of not doing so is
	 * cutting off the underscore on some fonts.
	 */
#if PANGO_VERSION_CHECK(1, 44, 0)
	xtext->fontsize = pango_font_metrics_get_height (metrics) / PANGO_SCALE + 1;

	if (xtext->fontsize == 0)
		xtext->fontsize = xtext->font->ascent + xtext->font->descent;
#else
	xtext->fontsize = xtext->font->ascent + xtext->font->descent;
#endif

	pango_font_metrics_unref (metrics);
}

static int
backend_get_text_width_emph (GtkXText *xtext, guchar *str, int len, int emphasis)
{
	int width;

	if (len <= 0 || *str == 0)
		return 0;

	if ((emphasis & EMPH_HIDDEN))
		return 0;
	emphasis &= (EMPH_ITAL | EMPH_BOLD);

	/* Phase 4.9 follow-up: ask Pango for the layout width of the whole
	 * string. The earlier implementation summed per-character integer
	 * widths from the fontwidths[] table; for any font where Pango's
	 * combined-layout width differs from the integer sum (most non-
	 * monospace runs, and even some "monospace" fonts under hinting /
	 * sub-pixel positioning) the BG rectangle in backend_draw_text_emph
	 * came out 1-3 px narrower than the text Pango actually drew. At
	 * selection boundaries inside a word that drift was visible: a
	 * thin unselected gap appeared between adjacent runs ("connect|ed"
	 * → "connect ed"). Measuring with the same code path drawing uses
	 * keeps width and rendering aligned. */
	pango_layout_set_attributes (xtext->layout, attr_lists[emphasis]);
	pango_layout_set_text (xtext->layout, (const char *) str, len);
	pango_layout_get_pixel_size (xtext->layout, &width, NULL);

	return width;
}

static int
backend_get_text_width_slp (GtkXText *xtext, guchar *str, GSList *slp)
{
	int width = 0;

	while (slp)
	{
		offlen_t *meta;

		meta = slp->data;
		width += backend_get_text_width_emph (xtext, str, meta->len, meta->emph);
		str += meta->len;
		slp = g_slist_next (slp);
	}

	return width;
}

/* simplified version of gdk_draw_layout_line_with_colors() */

static void
xtext_draw_layout_line (cairo_t          *cr,
								gint              x,
								gint              y,
								PangoLayoutLine  *line)
{
	GSList *tmp_list = line->runs;
	PangoRectangle logical_rect;
	gint x_off = 0;

	while (tmp_list)
	{
		PangoLayoutRun *run = tmp_list->data;

		pango_glyph_string_extents (run->glyphs, run->item->analysis.font,
											 NULL, &logical_rect);

		cairo_move_to (cr, x + x_off / PANGO_SCALE, y);
		pango_cairo_show_glyph_string (cr, run->item->analysis.font, run->glyphs);

		x_off += logical_rect.width;
		tmp_list = tmp_list->next;
	}
}

/* Phase 3.4b: GdkGC parameter dropped.  fg_idx/bg_idx are palette
 * indices.  If called outside the draw signal (xtext->cr is NULL),
 * simply skip — paint() will redo the work on the next draw tick.
 *
 * Phase 4.9 follow-up: mark_local_start / mark_local_end are run-local
 * byte offsets [0..len] of the portion of this run that falls inside
 * the current selection (or both -1 / equal for "no selection in this
 * run"). Selection coloring is applied as Pango attributes on top of
 * the emphasis attribute list, so the run lays out as a single
 * continuous string regardless of where the selection sits inside it.
 * That keeps glyph positions stable across selection changes — the
 * old approach of splitting a run at mark boundaries caused the text
 * after the selection to drift slightly when the selection grew or
 * shrank, because Pango's per-layout combined-extent rounding doesn't
 * sum exactly across multiple sub-layouts. */
static void
backend_draw_text_emph (GtkXText *xtext, int dofill, int fg_idx, int bg_idx,
                        int x, int y, char *str, int len, int str_width, int emphasis,
                        int mark_local_start, int mark_local_end)
{
	cairo_t *cr = xtext->cr;
	PangoLayoutLine *line;
	PangoAttrList *attrs;
	gboolean owns_attrs = FALSE;

	if (cr == NULL)
		return;

	/* Build the layout attribute list. If the run has no selection
	 * overlap, reuse the cached static attr_lists[emphasis] verbatim;
	 * otherwise build a fresh list combining emphasis + mark fg/bg. */
	if (mark_local_start < mark_local_end &&
	    mark_local_start >= 0 && mark_local_end <= len)
	{
		PangoAttribute *attr;
		const GdkRGBA *fg = &xtext->palette[XTEXT_MARK_FG];
		const GdkRGBA *bg = &xtext->palette[XTEXT_MARK_BG];

		attrs = pango_attr_list_new ();
		owns_attrs = TRUE;

		if (emphasis & EMPH_BOLD)
			pango_attr_list_insert (attrs,
				pango_attr_weight_new (PANGO_WEIGHT_BOLD));
		if (emphasis & EMPH_ITAL)
			pango_attr_list_insert (attrs,
				pango_attr_style_new (PANGO_STYLE_ITALIC));

		attr = pango_attr_foreground_new ((guint16)(fg->red   * 65535.0),
		                                  (guint16)(fg->green * 65535.0),
		                                  (guint16)(fg->blue  * 65535.0));
		attr->start_index = mark_local_start;
		attr->end_index   = mark_local_end;
		pango_attr_list_insert (attrs, attr);

		attr = pango_attr_background_new ((guint16)(bg->red   * 65535.0),
		                                  (guint16)(bg->green * 65535.0),
		                                  (guint16)(bg->blue  * 65535.0));
		attr->start_index = mark_local_start;
		attr->end_index   = mark_local_end;
		pango_attr_list_insert (attrs, attr);
	}
	else
	{
		attrs = attr_lists[emphasis];
	}

	pango_layout_set_attributes (xtext->layout, attrs);
	pango_layout_set_text (xtext->layout, str, len);

	if (dofill)
	{
		cairo_save (cr);
		xtext_cairo_set_source_idx (xtext, cr, bg_idx);
		cairo_rectangle (cr, x, y - xtext->font->ascent,
							  str_width, xtext->fontsize);
		cairo_fill (cr);
		cairo_restore (cr);
	}

	cairo_save (cr);
	xtext_cairo_set_source_idx (xtext, cr, fg_idx);

	if (owns_attrs)
	{
		/* pango_cairo_show_layout honors PangoAttribute foreground +
		 * background attributes, which is how the mark fg/bg gets
		 * painted on top of the run-default fill. The cairo source
		 * we set above is used for any glyphs that don't have a
		 * foreground attribute applied. */
		cairo_move_to (cr, x, y - xtext->font->ascent);
		pango_cairo_show_layout (cr, xtext->layout);
	}
	else
	{
		/* No selection in this run — use the existing fast path that
		 * draws raw glyph strings without re-running attribute
		 * application. */
		line = pango_layout_get_lines (xtext->layout)->data;
		xtext_draw_layout_line (cr, x, y, line);
	}
	cairo_restore (cr);

	if (owns_attrs)
		pango_attr_list_unref (attrs);
}

static void
xtext_set_fg (GtkXText *xtext, int index)
{
	xtext->cur_fg = index;
}

static void
xtext_set_bg (GtkXText *xtext, int index)
{
	xtext->cur_bg = index;
}

static void
gtk_xtext_init (GtkXText * xtext)
{
	xtext->cr = NULL;
	xtext->io_tag = 0;
	xtext->add_io_tag = 0;
	xtext->scroll_tag = 0;
	xtext->max_lines = 0;
	xtext->col_back = XTEXT_BG;
	xtext->col_fore = XTEXT_FG;
	xtext->cur_fg = XTEXT_FG;
	xtext->cur_bg = XTEXT_BG;
	xtext->nc = 0;
	xtext->pixel_offset = 0;
	xtext->underline = FALSE;
	xtext->strikethrough = FALSE;
	xtext->hidden = FALSE;
	xtext->font = NULL;
	xtext->layout = NULL;
	xtext->jump_out_offset = 0;
	xtext->jump_in_offset = 0;
	xtext->ts_x = 0;
	xtext->ts_y = 0;
	xtext->clip_x = 0;
	xtext->clip_x2 = 1000000;
	xtext->clip_y = 0;
	xtext->clip_y2 = 1000000;
	xtext->urlcheck_function = NULL;
	xtext->color_paste = FALSE;
	xtext->skip_border_fills = FALSE;
	xtext->skip_stamp = FALSE;
	xtext->render_hilights_only = FALSE;
	xtext->un_hilight = FALSE;
	xtext->recycle = FALSE;
	xtext->dont_render = FALSE;
	xtext->dont_render2 = FALSE;
	gtk_xtext_scroll_adjustments (xtext, NULL, NULL);

	/* Phase 4.9: install GTK 4 event controllers. The widgets-vs-event-
	 * signals split means clicks/motion/scroll/keys reach the widget via
	 * controllers rather than ::button_press_event / ::motion_notify_event
	 * / ::scroll_event. Each controller has its own bound-signal contract;
	 * the handlers live a few hundred lines below. */
	{
		GtkEventController *scroll;
		GtkGesture *click;
		GtkGesture *drag;
		GtkEventController *motion;

		scroll = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
		g_signal_connect (scroll, "scroll", G_CALLBACK (gtk_xtext_scroll_cb), xtext);
		gtk_widget_add_controller (GTK_WIDGET (xtext), scroll);

		/* GtkGestureClick (button = 0, any) handles click-shaped events:
		 * right/middle-click WORD_CLICK plus double/triple-click word/line
		 * select. We don't connect "released" — the primary-button release
		 * path runs through GtkGestureDrag::drag-end below, since
		 * GtkGestureClick suppresses "released" once motion exceeds the
		 * click threshold and that breaks drag-to-select. */
		click = gtk_gesture_click_new ();
		gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
		g_signal_connect (click, "pressed", G_CALLBACK (gtk_xtext_pressed_cb), xtext);
		gtk_widget_add_controller (GTK_WIDGET (xtext), GTK_EVENT_CONTROLLER (click));

		/* GtkGestureDrag drives the primary-button press/move/release
		 * cycle: drag-begin sets up the selection start (or grabs the
		 * separator bar), drag-update extends the selection, drag-end
		 * cleans up + autocopies. Unlike GtkGestureClick, all three
		 * fire unconditionally regardless of drag distance. */
		drag = gtk_gesture_drag_new ();
		gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), GDK_BUTTON_PRIMARY);
		g_signal_connect (drag, "drag-begin",  G_CALLBACK (gtk_xtext_drag_begin_cb),  xtext);
		g_signal_connect (drag, "drag-update", G_CALLBACK (gtk_xtext_drag_update_cb), xtext);
		g_signal_connect (drag, "drag-end",    G_CALLBACK (gtk_xtext_drag_end_cb),    xtext);
		gtk_widget_add_controller (GTK_WIDGET (xtext), GTK_EVENT_CONTROLLER (drag));

		motion = gtk_event_controller_motion_new ();
		g_signal_connect (motion, "motion", G_CALLBACK (gtk_xtext_motion_cb), xtext);
		g_signal_connect (motion, "leave",  G_CALLBACK (gtk_xtext_leave_cb),  xtext);
		gtk_widget_add_controller (GTK_WIDGET (xtext), motion);
	}
}

static void
gtk_xtext_adjustment_set (xtext_buffer *buf, int fire_signal)
{
	GtkAdjustment *adj = buf->xtext->adj;

	if (buf->xtext->buffer == buf)
	{
		double page_size;
		/* Phase 4.13: gtk_widget_get_allocation is deprecated in 4.10
		 * (replacement is gtk_widget_compute_bounds, but we only need
		 * width/height which the bare accessors give us). */
		int alloc_h = gtk_widget_get_height (GTK_WIDGET (buf->xtext));
		page_size = alloc_h / buf->xtext->fontsize;
		gtk_adjustment_set_lower(adj, 0);
		gtk_adjustment_set_upper(adj, buf->num_lines);

		if (gtk_adjustment_get_upper(adj) == 0)
			gtk_adjustment_set_upper(adj, 1);

		gtk_adjustment_set_page_size(adj, page_size);
		gtk_adjustment_set_page_increment(adj, page_size);

		if (gtk_adjustment_get_value(adj) > gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj))
		{
			buf->scrollbar_down = TRUE;
			gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
		}

		if (gtk_adjustment_get_value(adj) < 0)
			gtk_adjustment_set_value(adj, 0);
	}
}

static gint
gtk_xtext_adjustment_timeout (GtkXText * xtext)
{
	gtk_xtext_render_page (xtext);
	xtext->io_tag = 0;
	return 0;
}

static void
gtk_xtext_adjustment_changed (GtkAdjustment * adj, GtkXText * xtext)
{
	double adj_value = gtk_adjustment_get_value(adj);
	double adj_upper = gtk_adjustment_get_upper(adj);
	double adj_page_size = gtk_adjustment_get_page_size(adj);

	if (!gtk_widget_get_realized (GTK_WIDGET (xtext)))
		return;

	if (xtext->buffer->old_value != adj_value)
	{
		if (adj_value >= adj_upper - adj_page_size)
			xtext->buffer->scrollbar_down = TRUE;
		else
			xtext->buffer->scrollbar_down = FALSE;

		if (adj_value + 1 == xtext->buffer->old_value ||
			 adj_value - 1 == xtext->buffer->old_value)	/* clicked an arrow? */
		{
			if (xtext->io_tag)
			{
				g_source_remove (xtext->io_tag);
				xtext->io_tag = 0;
			}
			gtk_xtext_render_page (xtext);
		} else
		{
			if (!xtext->io_tag)
				xtext->io_tag = g_timeout_add (REFRESH_TIMEOUT,
															(GSourceFunc)
															gtk_xtext_adjustment_timeout,
															xtext);
		}
	}
	xtext->buffer->old_value = adj_value;
}

GtkWidget *
gtk_xtext_new (GdkRGBA palette[], int separator)
{
	GtkXText *xtext;

	xtext = g_object_new (gtk_xtext_get_type (), NULL);
	xtext->separator = separator;
	xtext->wordwrap = TRUE;
	xtext->buffer = gtk_xtext_buffer_new (xtext);
	xtext->orig_buffer = xtext->buffer;

	gtk_xtext_set_palette (xtext, palette);

	return GTK_WIDGET (xtext);
}

/* Phase 4.9: GtkWidget::destroy is gone in GTK 4 — teardown moved to
 * GObjectClass::dispose. Same body, different signature. */
static void
gtk_xtext_dispose (GObject * object)
{
	GtkWidget *widget = GTK_WIDGET (object);
	GtkXText *xtext = GTK_XTEXT (widget);

	if (xtext->add_io_tag)
	{
		g_source_remove (xtext->add_io_tag);
		xtext->add_io_tag = 0;
	}

	if (xtext->scroll_tag)
	{
		g_source_remove (xtext->scroll_tag);
		xtext->scroll_tag = 0;
	}

	if (xtext->io_tag)
	{
		g_source_remove (xtext->io_tag);
		xtext->io_tag = 0;
	}

	if (xtext->font)
	{
		backend_font_close (xtext);
		xtext->font = NULL;
	}

	if (xtext->adj)
	{
		g_signal_handlers_disconnect_matched (G_OBJECT (xtext->adj),
					G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, xtext);
		g_object_unref (G_OBJECT (xtext->adj));
		xtext->adj = NULL;
	}

	if (xtext->hand_cursor)
	{
		g_object_unref (xtext->hand_cursor);
		xtext->hand_cursor = NULL;
	}

	if (xtext->resize_cursor)
	{
		g_object_unref (xtext->resize_cursor);
		xtext->resize_cursor = NULL;
	}

	if (xtext->orig_buffer)
	{
		gtk_xtext_buffer_free (xtext->orig_buffer);
		xtext->orig_buffer = NULL;
	}

	G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gtk_xtext_unrealize (GtkWidget * widget)
{
	backend_deinit (GTK_XTEXT (widget));

	/* GTK 3: parent class unrealize tears down the window via
	 * gtk_widget_unregister_window().  The legacy
	 * gdk_window_set_user_data(window, NULL) belt-and-braces is gone. */
	if (parent_class->unrealize)
		(* GTK_WIDGET_CLASS (parent_class)->unrealize) (widget);
}

/*
 * Phase 3.4b: realize over the GTK 3 idioms.  GdkColormap is gone (the
 * window inherits visual + colormap from its parent), GdkGCs are
 * replaced by per-draw cairo source setup (the bgc/fgc/light/dark/thin
 * /marker GCs were just remembered colors with optional tiled fill —
 * see xtext_cairo_set_palette and the bg_pattern cache below), and the
 * gdk_window_set_back_pixmap call is no longer needed because cairo
 * draws fill the entire dirty rectangle each tick.
 */
/* Phase 4.9: GTK 4 widgets are "windowless" — they don't own their
 * own GdkSurface; GdkSurface only attaches to GtkRoot (toplevel /
 * popover / native). The whole "create a child GdkWindow with a
 * specific event mask" pattern is gone. The realize handler now
 * just chains to the parent class and sets up the GTK-4-only state
 * (cursors are GdkCursor*; selection targets move to the gesture/
 * controller layer). */
static void
gtk_xtext_realize (GtkWidget * widget)
{
	GtkXText *xtext = GTK_XTEXT (widget);

	GTK_WIDGET_CLASS (parent_class)->realize (widget);

	/* Phase 4.9: GdkCursor in GTK 4 is themed-name-based; depth is
	 * never read by the cairo draw path so leave it zero. */
	xtext->depth = 0;
	xtext->hand_cursor   = gdk_cursor_new_from_name ("pointer", NULL);
	xtext->resize_cursor = gdk_cursor_new_from_name ("ew-resize", NULL);

	/* Phase 4.9: GtkTargetEntry / gtk_selection_add_targets /
	 * GDK_SELECTION_PRIMARY are gone in GTK 4. PRIMARY-selection text
	 * support migrates to gdk_clipboard_set_value on the surface's
	 * primary clipboard — handled at the controller layer in a
	 * Phase 4.9 follow-up. */

	backend_init (xtext);
}

/* Phase 3.4b: gtk_xtext_size_request removed.  GTK 3 widgets advertise
 * their size via get_preferred_width/height — left as the implicit
 * default for now (the parent GtkScrolledWindow drives layout). */

/* Phase 4.9: GTK 4 size_allocate vfunc takes (widget, width, height,
 * baseline) instead of a GtkAllocation*. Position is implicit (the
 * widget is always at 0,0 in its own coordinate space — GTK 4 does
 * the offset on the parent's behalf). gtk_widget_set_allocation /
 * gdk_window_move_resize are gone; the parent-class chain handles
 * what remains. */
static void
gtk_xtext_size_allocate (GtkWidget * widget, int width, int height, int baseline)
{
	GtkXText *xtext = GTK_XTEXT (widget);
	int height_only = FALSE;
	(void) baseline;

	if (width == xtext->buffer->window_width)
		height_only = TRUE;

	GTK_WIDGET_CLASS (parent_class)->size_allocate (widget, width, height, baseline);

	if (gtk_widget_get_realized (widget))
	{
		xtext->buffer->window_width  = width;
		xtext->buffer->window_height = height;

		dontscroll (xtext->buffer);	/* force scrolling off */
		if (!height_only)
			gtk_xtext_calc_lines (xtext->buffer, FALSE);
		else
		{
			xtext->buffer->pagetop_ent = NULL;
			gtk_xtext_adjustment_set (xtext->buffer, FALSE);
		}
		if (xtext->buffer->scrollbar_down)
			gtk_adjustment_set_value (xtext->adj, gtk_adjustment_get_upper(xtext->adj) -
											  gtk_adjustment_get_page_size(xtext->adj));
	}
}

static int
gtk_xtext_selection_clear (xtext_buffer *buf)
{
	textentry *ent;
	int ret = 0;

	ent = buf->last_ent_start;
	while (ent)
	{
		if (ent->mark_start != -1)
			ret = 1;
		ent->mark_start = -1;
		ent->mark_end = -1;
		if (ent == buf->last_ent_end)
			break;
		ent = ent->next;
	}

	return ret;
}

static int
find_x (GtkXText *xtext, textentry *ent, int x, int subline, int indent)
{
	int xx = indent;
	int suboff;
	GSList *list;
	GSList *hid = NULL;
	offlen_t *meta;
	int off;

	/* Skip to the first chunk of stuff for the subline */
	if (subline > 0)
	{
		suboff = GPOINTER_TO_INT (g_slist_nth_data (ent->sublines, subline - 1));
		for (list = ent->slp; list; list = g_slist_next (list))
		{
			meta = list->data;
			if (meta->off + meta->len > suboff)
				break;
		}
	}
	else
	{
		suboff = 0;
		list = ent->slp;
	}
	if (list == NULL)
		return 0;

	/* Phase 4.9 follow-up: walk the slp chunks measuring the combined-
	 * layout width of each chunk's segment that falls in this subline.
	 * The earlier implementation summed per-character widths; each one
	 * was Pango's ceil-rounded single-char extent, and the rendering
	 * uses Pango's ceil-rounded combined-layout extent — which is
	 * always ≤ the sum of the per-char ceils. The accumulated drift
	 * made find_x reach `x` after fewer characters than the rendering
	 * had actually drawn to that point, and selection started 1-2
	 * characters to the left of the click.
	 *
	 * Now we measure the way we draw: for each chunk get its combined
	 * width (backend_get_text_width_emph already does this after the
	 * earlier commit), and if the click falls inside a chunk hand off
	 * to pango_layout_xy_to_index for accurate sub-chunk lookup.
	 *
	 * `off` is the byte offset of the cursor within ent->str. Each
	 * iteration covers a chunk-shaped segment starting at `off` and
	 * ending at min(meta->off + meta->len, end-of-line). */
	meta = list->data;
	off = (meta->off > suboff) ? meta->off : suboff;
	if (meta->emph & EMPH_HIDDEN)
		hid = list;

	while (list)
	{
		int chunk_end = meta->off + meta->len;
		int seg_len = chunk_end - off;
		int seg_width;

		if (seg_len > 0)
		{
			seg_width = backend_get_text_width_emph (xtext,
			                ent->str + off, seg_len, meta->emph);

			if (xx + seg_width > x)
			{
				int local_x = x - xx;
				int index = 0;
				int trailing = 0;
				int emph_only = meta->emph & (EMPH_ITAL | EMPH_BOLD);

				if (local_x < 0)
					local_x = 0;

				/* Hidden chunks contribute no width but the byte range
				 * still belongs to the line; treat the click as landing
				 * on the chunk's start offset. */
				if (meta->emph & EMPH_HIDDEN)
					return off;

				pango_layout_set_attributes (xtext->layout,
				                             attr_lists[emph_only]);
				pango_layout_set_text (xtext->layout,
				                       (const char *) (ent->str + off),
				                       seg_len);
				pango_layout_xy_to_index (xtext->layout,
				                          local_x * PANGO_SCALE, 0,
				                          &index, &trailing);

				return off + index;
			}

			xx += seg_width;
			off = chunk_end;
		}

		if (meta->emph & EMPH_HIDDEN)
			hid = list;
		list = g_slist_next (list);
		if (list == NULL)
			break;
		meta = list->data;
		if (off < meta->off)
			off = meta->off;
	}

	/* If previous chunk exists and is marked hidden, regard it as unhidden */
	if (hid && list && hid->next == list)
	{
		meta = hid->data;
		off = meta->off;
	}

	/* Click was past the last chunk's end — return the line-end offset */
	return off;
}

static int
gtk_xtext_find_x (GtkXText * xtext, int x, textentry * ent, int subline,
						int line, int *out_of_bounds)
{
	int indent;
	unsigned char *str;

	if (subline < 1)
		indent = ent->indent;
	else
		indent = xtext->buffer->indent;

	if (line > gtk_adjustment_get_page_size(xtext->adj) || line < 0)
	{
		*out_of_bounds = TRUE;
		return 0;
	}

	str = ent->str + gtk_xtext_find_subline (xtext, ent, subline);
	if (str >= ent->str + ent->str_len)
		return 0;

	/* Let user select left a few pixels to grab hidden text e.g. '<' */
	if (x < indent - xtext->space_width)
	{
		*out_of_bounds = 1;
		return (str - ent->str);
	}

	*out_of_bounds = 0;

	return find_x (xtext, ent, x, subline, indent);
}

static textentry *
gtk_xtext_find_char (GtkXText * xtext, int x, int y, int *off, int *out_of_bounds)
{
	textentry *ent;
	int line;
	int subline;
	int outofbounds = FALSE;

	/* Adjust y value for negative rounding, double to int */
	if (y < 0)
		y -= xtext->fontsize;

	line = (y + xtext->pixel_offset) / xtext->fontsize;
	ent = gtk_xtext_nth (xtext, line + (int)gtk_adjustment_get_value(xtext->adj), &subline);
	if (!ent)
		return NULL;

	if (off)
		*off = gtk_xtext_find_x (xtext, x, ent, subline, line, &outofbounds);
	if (out_of_bounds)
		*out_of_bounds = outofbounds;

	return ent;
}

/* Cairo-friendly vertical line: position at half-pixel for crisp rendering. */
static void
xtext_cairo_vline (cairo_t *cr, int x, int y, int height)
{
	cairo_set_line_width (cr, 1);
	cairo_move_to (cr, x + 0.5, y);
	cairo_line_to (cr, x + 0.5, y + height);
	cairo_stroke (cr);
}

static void
gtk_xtext_draw_sep (GtkXText * xtext, int y)
{
	int x, height;
	cairo_t *cr = xtext->cr;

	if (cr == NULL)
		return;	/* deferred to next draw signal */

	if (y == -1)
	{
		y = 0;
		height = gtk_widget_get_height (GTK_WIDGET (xtext));
	} else
	{
		height = xtext->fontsize;
	}

	if (!xtext->separator || !xtext->buffer->indent)
		return;

	x = xtext->buffer->indent - ((xtext->space_width + 1) / 2);
	if (x < 1)
		return;

	cairo_save (cr);

	/* Use neutral palette colors as light/dark — was traditionally a
	 * computed pair around the bg shade.  The "thin" variant is just
	 * the dark color; the bevelled variant draws light + dark side by side. */
	if (xtext->thinline)
	{
		if (xtext->moving_separator)
			xtext_cairo_set_source_idx (xtext, cr, XTEXT_FG);
		else
			xtext_cairo_set_source_idx (xtext, cr, XTEXT_BG);
		xtext_cairo_vline (cr, x, y, height);
	} else
	{
		if (xtext->moving_separator)
		{
			xtext_cairo_set_source_idx (xtext, cr, XTEXT_FG);
			xtext_cairo_vline (cr, x - 1, y, height);
			xtext_cairo_set_source_idx (xtext, cr, XTEXT_BG);
			xtext_cairo_vline (cr, x, y, height);
		} else
		{
			xtext_cairo_set_source_idx (xtext, cr, XTEXT_BG);
			xtext_cairo_vline (cr, x - 1, y, height);
			xtext_cairo_set_source_idx (xtext, cr, XTEXT_FG);
			xtext_cairo_vline (cr, x, y, height);
		}
	}

	cairo_restore (cr);
}

static void
gtk_xtext_draw_marker (GtkXText * xtext, textentry * ent, int y)
{
	int x, width, render_y;

	if (!xtext->marker) return;

	if (xtext->buffer->marker_pos == ent)
	{
		render_y = y + xtext->font->descent;
	}
	else if (xtext->buffer->marker_pos == ent->next && ent->next != NULL)
	{
		render_y = y + xtext->font->descent + xtext->fontsize * g_slist_length (ent->sublines);
	}
	else return;

	{
		x = 0;
		width = gtk_widget_get_width (GTK_WIDGET (xtext));
	}

	if (xtext->cr)
	{
		cairo_save (xtext->cr);
		xtext_cairo_set_source_idx (xtext, xtext->cr, XTEXT_MARKER);
		cairo_set_line_width (xtext->cr, 1);
		cairo_move_to (xtext->cr, x, render_y + 0.5);
		cairo_line_to (xtext->cr, x + width, render_y + 0.5);
		cairo_stroke (xtext->cr);
		cairo_restore (xtext->cr);
	}

	{
		/* Phase 4.9: gtk_widget_get_toplevel and
		 * gtk_window_has_toplevel_focus are gone in GTK 4. The
		 * replacements are gtk_widget_get_root (returns a GtkRoot,
		 * which a GtkWindow implements) and gtk_window_is_active. */
		GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (xtext));
		if (root && GTK_IS_WINDOW (root) &&
		    gtk_window_is_active (GTK_WINDOW (root)))
		{
			xtext->buffer->marker_seen = TRUE;
		}
	}
}

static void
gtk_xtext_paint (GtkWidget *widget, GdkRectangle *area)
{
	GtkXText *xtext = GTK_XTEXT (widget);
	textentry *ent_start, *ent_end;
	int x, y;

	int alloc_w = gtk_widget_get_width  (widget);
	int alloc_h = gtk_widget_get_height (widget);
	if (area->x == 0 && area->y == 0 &&
		 area->height == alloc_h &&
		 area->width  == alloc_w)
	{
		dontscroll (xtext->buffer);	/* force scrolling off */
		gtk_xtext_render_page (xtext);
		return;
	}

	ent_start = gtk_xtext_find_char (xtext, area->x, area->y, NULL, NULL);
	if (!ent_start)
	{
		xtext_draw_bg (xtext, area->x, area->y, area->width, area->height);
		goto xit;
	}
	ent_end = gtk_xtext_find_char (xtext, area->x + area->width,
											 area->y + area->height, NULL, NULL);
	if (!ent_end)
		ent_end = xtext->buffer->text_last;

	/* can't set a clip here, because fgc/bgc are used to draw the DB too */
/*	backend_set_clip (xtext, area);*/
	xtext->clip_x = area->x;
	xtext->clip_x2 = area->x + area->width;
	xtext->clip_y = area->y;
	xtext->clip_y2 = area->y + area->height;

	/* y is the last pixel y location it rendered text at */
	y = gtk_xtext_render_ents (xtext, ent_start, ent_end);

	if (y && y < alloc_h && !ent_end->next)
	{
		GdkRectangle rect;

		rect.x = 0;
		rect.y = y;
		rect.width = alloc_w;
		rect.height = alloc_h - y;

		/* fill any space below the last line that also intersects with
			the exposure rectangle */
		if (gdk_rectangle_intersect (area, &rect, &rect))
		{
			xtext_draw_bg (xtext, rect.x, rect.y, rect.width, rect.height);
		}
	}

	/*backend_clear_clip (xtext);*/
	xtext->clip_x = 0;
	xtext->clip_x2 = 1000000;
	xtext->clip_y = 0;
	xtext->clip_y2 = 1000000;

xit:
	x = xtext->buffer->indent - ((xtext->space_width + 1) / 2);
	if (area->x <= x)
		gtk_xtext_draw_sep (xtext, -1);
}

/* Phase 3.4b: GTK 3 draw signal handler.  Cairo gives us a clipped cr
 * directly; we compute the dirty rectangle via cairo_clip_extents and
 * stash cr on the xtext for the rendering primitives to use during this
 * paint, then clear it on exit. */
/* Phase 4.9: GtkWidget::draw is gone in GTK 4 — replaced by ::snapshot,
 * which hands us a GtkSnapshot that records render nodes rather than
 * a pre-clipped cairo_t. We preserve the existing cairo paint path
 * (unchanged since Phase 3.4b) by asking the snapshot for a cairo_t
 * over the widget's bounding rectangle and feeding it through. The
 * GdkRectangle synthesised here is the whole widget area; per-region
 * clip optimization can revisit later. */
static void
gtk_xtext_snapshot (GtkWidget * widget, GtkSnapshot * snapshot)
{
	GtkXText *xtext = GTK_XTEXT (widget);
	GdkRectangle area;
	graphene_rect_t bounds;
	cairo_t *cr;

	area.x = 0;
	area.y = 0;
	area.width  = gtk_widget_get_width (widget);
	area.height = gtk_widget_get_height (widget);
	if (area.width <= 0 || area.height <= 0)
		return;

	bounds = GRAPHENE_RECT_INIT (0, 0, (float) area.width, (float) area.height);
	cr = gtk_snapshot_append_cairo (snapshot, &bounds);

	xtext->cr = cr;
	gtk_xtext_paint (widget, &area);
	xtext->cr = NULL;

	cairo_destroy (cr);
}

/* render a selection that has extended or contracted upward */

static void
gtk_xtext_selection_up (GtkXText *xtext, textentry *start, textentry *end,
								int start_offset)
{
	/* render all the complete lines */
	if (start->next == end)
		gtk_xtext_render_ents (xtext, end, NULL);
	else
		gtk_xtext_render_ents (xtext, start->next, end);

	/* now the incomplete upper line */
	if (start == xtext->buffer->last_ent_start)
		xtext->jump_in_offset = xtext->buffer->last_offset_start;
	else
		xtext->jump_in_offset = start_offset;
	gtk_xtext_render_ents (xtext, start, NULL);
	xtext->jump_in_offset = 0;
}

/* render a selection that has extended or contracted downward */

static void
gtk_xtext_selection_down (GtkXText *xtext, textentry *start, textentry *end,
								  int end_offset)
{
	/* render all the complete lines */
	if (end->prev == start)
		gtk_xtext_render_ents (xtext, start, NULL);
	else
		gtk_xtext_render_ents (xtext, start, end->prev);

	/* now the incomplete bottom line */
	if (end == xtext->buffer->last_ent_end)
		xtext->jump_out_offset = xtext->buffer->last_offset_end;
	else
		xtext->jump_out_offset = end_offset;
	gtk_xtext_render_ents (xtext, end, NULL);
	xtext->jump_out_offset = 0;
}

static void
gtk_xtext_selection_render (GtkXText *xtext, textentry *start_ent, textentry *end_ent)
{
	textentry *ent;
	int start_offset = start_ent->mark_start;
	int end_offset = end_ent->mark_end;
	int start, end;

	xtext->skip_border_fills = TRUE;
	xtext->skip_stamp = TRUE;

	/* force an optimized render if there was no previous selection */
	if (xtext->buffer->last_ent_start == NULL && start_ent == end_ent)
	{
		xtext->buffer->last_offset_start = start_offset;
		xtext->buffer->last_offset_end = end_offset;
		goto lamejump;
	}

	/* mark changed within 1 ent only? */
	if (xtext->buffer->last_ent_start == start_ent &&
		 xtext->buffer->last_ent_end == end_ent)
	{
		/* when only 1 end of the selection is changed, we can really
			save on rendering */
		if (xtext->buffer->last_offset_start == start_offset ||
			 xtext->buffer->last_offset_end == end_offset)
		{
lamejump:
			ent = end_ent;
			/* figure out where to start and end the rendering */
			if (end_offset > xtext->buffer->last_offset_end)
			{
				end = end_offset;
				start = xtext->buffer->last_offset_end;
			} else if (end_offset < xtext->buffer->last_offset_end)
			{
				end = xtext->buffer->last_offset_end;
				start = end_offset;
			} else if (start_offset < xtext->buffer->last_offset_start)
			{
				end = xtext->buffer->last_offset_start;
				start = start_offset;
				ent = start_ent;
			} else if (start_offset > xtext->buffer->last_offset_start)
			{
				end = start_offset;
				start = xtext->buffer->last_offset_start;
				ent = start_ent;
			} else
			{	/* WORD selects end up here */
				end = end_offset;
				start = start_offset;
			}
		} else
		{
			/* LINE selects end up here */
			/* so which ent actually changed? */
			ent = start_ent;
			if (xtext->buffer->last_offset_start == start_offset)
				ent = end_ent;

			end = MAX (xtext->buffer->last_offset_end, end_offset);
			start = MIN (xtext->buffer->last_offset_start, start_offset);
		}

		xtext->jump_out_offset = end;
		xtext->jump_in_offset = start;
		gtk_xtext_render_ents (xtext, ent, NULL);
		xtext->jump_out_offset = 0;
		xtext->jump_in_offset = 0;
	}
	/* marking downward? */
	else if (xtext->buffer->last_ent_start == start_ent &&
				xtext->buffer->last_offset_start == start_offset)
	{
		/* find the range that covers both old and new selection */
		ent = start_ent;
		while (ent)
		{
			if (ent == xtext->buffer->last_ent_end)
			{
				gtk_xtext_selection_down (xtext, ent, end_ent, end_offset);
				/*gtk_xtext_render_ents (xtext, ent, end_ent);*/
				break;
			}
			if (ent == end_ent)
			{
				gtk_xtext_selection_down (xtext, ent, xtext->buffer->last_ent_end, end_offset);
				/*gtk_xtext_render_ents (xtext, ent, xtext->buffer->last_ent_end);*/
				break;
			}
			ent = ent->next;
		}
	}
	/* marking upward? */
	else if (xtext->buffer->last_ent_start != NULL &&
				xtext->buffer->last_ent_end == end_ent &&
				xtext->buffer->last_offset_end == end_offset)
	{
		ent = end_ent;
		while (ent)
		{
			if (ent == start_ent && xtext->buffer->last_ent_start)
			{
				gtk_xtext_selection_up (xtext, xtext->buffer->last_ent_start, ent, start_offset);
				/*gtk_xtext_render_ents (xtext, xtext->buffer->last_ent_start, ent);*/
				break;
			}
			if (ent == xtext->buffer->last_ent_start)
			{
				gtk_xtext_selection_up (xtext, start_ent, ent, start_offset);
				/*gtk_xtext_render_ents (xtext, start_ent, ent);*/
				break;
			}
			ent = ent->prev;
		}
	}
	else	/* cross-over mark (stretched or shrunk at both ends) */
	{
		/* unrender the old mark */
		gtk_xtext_render_ents (xtext, xtext->buffer->last_ent_start, xtext->buffer->last_ent_end);
		/* now render the new mark, but skip overlaps */
		if (start_ent == xtext->buffer->last_ent_start)
		{
			/* if the new mark is a sub-set of the old, do nothing */
			if (start_ent != end_ent)
				gtk_xtext_render_ents (xtext, start_ent->next, end_ent);
		} else if (end_ent == xtext->buffer->last_ent_end)
		{
			/* if the new mark is a sub-set of the old, do nothing */
			if (start_ent != end_ent)
				gtk_xtext_render_ents (xtext, start_ent, end_ent->prev);
		} else
			gtk_xtext_render_ents (xtext, start_ent, end_ent);
	}

	xtext->buffer->last_ent_start = start_ent;
	xtext->buffer->last_ent_end = end_ent;
	xtext->buffer->last_offset_start = start_offset;
	xtext->buffer->last_offset_end = end_offset;

	xtext->skip_border_fills = FALSE;
	xtext->skip_stamp = FALSE;
}

/* Phase 4.9: GtkEventControllerScroll callback for the wheel.
 *
 * Replaces the GTK 3 ::scroll-event handler. The controller is created
 * with GTK_EVENT_CONTROLLER_SCROLL_VERTICAL so dy is meaningful (down
 * is positive, up is negative); we ignore dx. The old handler nudged
 * the adjustment by page_increment/10 per scroll tick — preserve that
 * cadence and just multiply by dy so smooth-scroll devices (touchpads,
 * Wayland high-resolution wheels) act proportionally. */
static gboolean
gtk_xtext_scroll_cb (GtkEventControllerScroll *controller,
                     gdouble dx, gdouble dy, gpointer user_data)
{
	GtkXText *xtext = user_data;
	GtkAdjustment *adj = xtext->adj;
	gdouble step;
	gdouble new_value;
	gdouble lower, upper, page_size;

	(void) controller;
	(void) dx;

	step = gtk_adjustment_get_page_increment (adj) / 10.0;
	new_value = gtk_adjustment_get_value (adj) + (dy * step);

	lower     = gtk_adjustment_get_lower (adj);
	upper     = gtk_adjustment_get_upper (adj);
	page_size = gtk_adjustment_get_page_size (adj);

	if (new_value < lower)
		new_value = lower;
	if (new_value > upper - page_size)
		new_value = upper - page_size;

	gtk_adjustment_set_value (adj, new_value);
	return GDK_EVENT_STOP;
}

/* Phase 4.9: GTK 4 input layer.
 *
 * GTK 4 widgets no longer emit ::button_press_event, ::motion_notify_event,
 * ::leave_notify_event, ::scroll_event, ::selection_get, or
 * ::selection_clear_event. Clicks come through a single GtkGestureClick
 * (button=0 — any button — dispatched by current_button in the callback,
 * which keeps n_press unified so double/triple-click detection still
 * works for left-click selection), motion through GtkEventControllerMotion,
 * scroll through GtkEventControllerScroll (above), and clipboard ownership
 * is asserted via gdk_clipboard_set_text on the regular and primary
 * GdkClipboards. Pointer cursors are set with gtk_widget_set_cursor.
 *
 * The selection auto-scroll timers (scrollup_timeout / scrolldown_timeout)
 * used to query live pointer position via xtext_get_pointer; that helper
 * is gone, so they read xtext->select_end_y, which the motion controller
 * keeps updated while the button is held. */

static void
gtk_xtext_selection_draw (GtkXText * xtext, gboolean render)
{
	textentry *ent;
	textentry *ent_end;
	textentry *ent_start;
	int offset_start = 0;
	int offset_end = 0;
	textentry *low_ent, *high_ent;
	int low_x, low_y, low_offs;
	int high_x, high_y, high_offs, high_len;

	if (xtext->buffer->text_first == NULL)
		return;

	ent_start = gtk_xtext_find_char (xtext, xtext->select_start_x, xtext->select_start_y, &offset_start, NULL);
	ent_end = gtk_xtext_find_char (xtext, xtext->select_end_x, xtext->select_end_y, &offset_end, NULL);
	if (ent_start == NULL && ent_end == NULL)
		return;

	if	((ent_start != ent_end && xtext->select_start_y > xtext->select_end_y) || /* different entries */
		(ent_start == ent_end && offset_start > offset_end))	/* same entry, different character offsets */
	{
		/* marking up */
		low_ent = ent_end;
		low_x = xtext->select_end_x;
		low_y = xtext->select_end_y;
		low_offs = offset_end;
		high_ent = ent_start;
		high_x = xtext->select_start_x;
		high_y = xtext->select_start_y;
		high_offs = offset_start;
	}
	else
	{
		/* marking down */
		low_ent = ent_start;
		low_x = xtext->select_start_x;
		low_y = xtext->select_start_y;
		low_offs = offset_start;
		high_ent = ent_end;
		high_x = xtext->select_end_x;
		high_y = xtext->select_end_y;
		high_offs = offset_end;
	}
	if (low_ent == NULL)
	{
		low_ent = xtext->buffer->text_first;
		low_offs = 0;
	}
	if (high_ent == NULL)
	{
		high_ent = xtext->buffer->text_last;
		high_offs = high_ent->str_len;
	}

	/* word selection */
	if (xtext->word_select)
	{
		/* a word selection cannot be started if the cursor is out of bounds in gtk_xtext_button_press */
		gtk_xtext_get_word (xtext, low_x, low_y, NULL, &low_offs, NULL, NULL);

		/* in case the cursor is out of bounds we keep offset_end from gtk_xtext_find_char and fix the length */
		if (gtk_xtext_get_word (xtext, high_x, high_y, NULL, &high_offs, &high_len, NULL) == NULL)
			high_len = high_offs == high_ent->str_len? 0: -1; /* -1 for the space, 0 if at the end */
		high_offs += high_len;
		if (low_y < 0)
			low_offs = xtext->buffer->last_offset_start;
		if (high_y > xtext->buffer->window_height)
			high_offs = xtext->buffer->last_offset_end;
	}
	/* line/ent selection */
	else if (xtext->line_select)
	{
		low_offs = 0;
		high_offs = high_ent->str_len;
	}
	/* character selection */
	else
	{
		if (low_y < 0)
			low_offs = xtext->buffer->last_offset_start;
		if (high_y > xtext->buffer->window_height)
			high_offs = xtext->buffer->last_offset_end;
	}

	/* set all the old mark_ fields to -1 */
	gtk_xtext_selection_clear (xtext->buffer);

	low_ent->mark_start = low_offs;
	low_ent->mark_end = high_offs;

	if (low_ent != high_ent)
	{
		low_ent->mark_end = low_ent->str_len;
		if (high_offs != 0)
		{
			high_ent->mark_start = 0;
			high_ent->mark_end = high_offs;
		}

		/* set all the mark_ fields of the ents within the selection */
		ent = low_ent->next;
		while (ent && ent != high_ent)
		{
			ent->mark_start = 0;
			ent->mark_end = ent->str_len;
			ent = ent->next;
		}
	}

	if (render)
		gtk_xtext_selection_render (xtext, low_ent, high_ent);
}

static int
gtk_xtext_timeout_ms (GtkXText *xtext, int pixes)
{
	int apixes = abs(pixes);

	if (apixes < 6) return 100;
	if (apixes < 12) return 50;
	if (apixes < 20) return 20;
	return 10;
}
static gint
gtk_xtext_scrolldown_timeout (GtkXText * xtext)
{
	int p_y, win_height;
	xtext_buffer *buf = xtext->buffer;
	GtkAdjustment *adj = xtext->adj;

	/* Phase 4.9: GTK 4 has no synchronous "where is the pointer right now"
	 * accessor on a widget; the motion controller updates select_end_y on
	 * every move, so we use that as the pointer's last known y. The timer
	 * keeps firing as long as last-known-y is past the edge. */
	p_y = xtext->select_end_y;
	win_height = gtk_widget_get_height (GTK_WIDGET (xtext));

	if (buf->last_ent_end == NULL ||	/* If context has changed OR */
		 buf->pagetop_ent == NULL ||	/* pagetop_ent is reset OR */
		 p_y <= win_height ||			/* pointer not below bottom margin OR */
		 gtk_adjustment_get_value(adj) >= gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj)) 	/* we're scrolled to bottom */
	{
		xtext->scroll_tag = 0;
		return 0;
	}

	xtext->select_start_y -= xtext->fontsize;
	xtext->select_start_adj++;
	gtk_adjustment_set_value(adj, gtk_adjustment_get_value(adj) + 1);
	gtk_xtext_selection_draw (xtext, TRUE);
	gtk_xtext_render_ents (xtext, buf->pagetop_ent->next, buf->last_ent_end);
	xtext->scroll_tag = g_timeout_add (gtk_xtext_timeout_ms (xtext, p_y - win_height),
													(GSourceFunc)
													gtk_xtext_scrolldown_timeout,
													xtext);

	return 0;
}

static gint
gtk_xtext_scrollup_timeout (GtkXText * xtext)
{
	int p_y;
	xtext_buffer *buf = xtext->buffer;
	GtkAdjustment *adj = xtext->adj;
	int delta_y;

	p_y = xtext->select_end_y;

	if (buf->last_ent_start == NULL ||	/* If context has changed OR */
		 buf->pagetop_ent == NULL ||		/* pagetop_ent is reset OR */
		 p_y >= 0 ||							/* not above top margin OR */
		 gtk_adjustment_get_value(adj) == 0)						/* we're scrolled to the top */
	{
		xtext->scroll_tag = 0;
		return 0;
	}

	if (gtk_adjustment_get_value(adj) < 0)
	{
		delta_y = (int)gtk_adjustment_get_value(adj) * xtext->fontsize;
		gtk_adjustment_set_value(adj, 0);
	} else {
		delta_y = xtext->fontsize;
		gtk_adjustment_set_value(adj, gtk_adjustment_get_value(adj) - 1);
	}
	xtext->select_start_y += delta_y;
	xtext->select_start_adj = gtk_adjustment_get_value(adj);
	gtk_xtext_selection_draw (xtext, TRUE);
	gtk_xtext_render_ents (xtext, buf->pagetop_ent->prev, buf->last_ent_end);
	xtext->scroll_tag = g_timeout_add (gtk_xtext_timeout_ms (xtext, p_y),
													(GSourceFunc)
													gtk_xtext_scrollup_timeout,
													xtext);

	return 0;
}

static void
gtk_xtext_selection_update (GtkXText * xtext, int p_y, gboolean render)
{
	int win_height;
	int moved;

	if (xtext->scroll_tag)
	{
		return;
	}

	win_height = gtk_widget_get_height (GTK_WIDGET (xtext));

	/* selecting past top of window, scroll up! */
	if (p_y < 0 && gtk_adjustment_get_value(xtext->adj) >= 0)
	{
		gtk_xtext_scrollup_timeout (xtext);
	}

	/* selecting past bottom of window, scroll down! */
	else if (p_y > win_height &&
		 gtk_adjustment_get_value(xtext->adj) < (gtk_adjustment_get_upper(xtext->adj) - gtk_adjustment_get_page_size(xtext->adj)))
	{
		gtk_xtext_scrolldown_timeout (xtext);
	}
	else
	{
		moved = (int)gtk_adjustment_get_value(xtext->adj) - xtext->select_start_adj;
		xtext->select_start_y -= (moved * xtext->fontsize);
		xtext->select_start_adj = gtk_adjustment_get_value(xtext->adj);
		gtk_xtext_selection_draw (xtext, render);
	}
}

static char *
gtk_xtext_get_word (GtkXText * xtext, int x, int y, textentry ** ret_ent,
						  int *ret_off, int *ret_len, GSList **slp)
{
	textentry *ent;
	int offset;
	unsigned char *word;
	unsigned char *last, *end;
	int len;
	int out_of_bounds = 0;
	int len_to_offset = 0;

	ent = gtk_xtext_find_char (xtext, x, y, &offset, &out_of_bounds);
	if (ent == NULL || out_of_bounds || offset < 0 || offset >= ent->str_len)
		return NULL;

	word = ent->str + offset;
	while ((word = g_utf8_find_prev_char (ent->str, word)))
	{
		if (is_del (*word))
		{
			word++;
			len_to_offset--;
			break;
		}
		len_to_offset += charlen (word);
	}
	if (!word)
		word = ent->str;

	/* remove color characters from the length */
	gtk_xtext_strip_color (word, len_to_offset, xtext->scratch_buffer, &len_to_offset, NULL, FALSE);

	last = word;
	end = ent->str + ent->str_len;
	len = 0;
	do
	{
		if (is_del (*last))
			break;
		len += charlen (last);
		last = g_utf8_find_next_char (last, end);
	}
	while (last);

	if (len > 0 && word[len-1]=='.')
		len--;

	if (ret_ent)
		*ret_ent = ent;
	if (ret_off)
		*ret_off = word - ent->str;
	if (ret_len)
		*ret_len = len;		/* Length before stripping */

	word = gtk_xtext_strip_color (word, len, xtext->scratch_buffer, NULL, slp, FALSE);

	/* avoid turning the cursor into a hand for non-url part of the word */
	if (xtext->urlcheck_function && xtext->urlcheck_function (GTK_WIDGET (xtext), word))
	{
		int start, end;

		/* Phase 5: url_last is a stub that returns 0 (no sub-word match
		 * info — see comment near its definition). The HexChat code
		 * here used the start/end out-params to crop a partial match
		 * inside the larger token; with no match info we fall back to
		 * "the whole word IS the URL", same as the matching call site
		 * in gtk_xtext_get_word_adjust which already gates this on
		 * url_last's return value. Without the gate, start=end=0 made
		 * `len_to_offset - 0 >= 0 - 0` always true (len_to_offset is
		 * non-negative), so we returned NULL for every URL hover and
		 * right-click, which read as "right-click on URL does
		 * nothing" / "URLs don't underline on hover" in chat / msg /
		 * pchat. Guarding the bounds check on url_last's return value
		 * fixes both. */
		if (url_last (&start, &end))
		{
			/* make sure we're not before the start of the match */
			if (len_to_offset < start)
				return NULL;

			/* and not after it */
			if (len_to_offset - start >= end - start)
				return NULL;
		}
	}

	return word;
}

static void
gtk_xtext_unrender_hilight (GtkXText *xtext)
{
	xtext->render_hilights_only = TRUE;
	xtext->skip_border_fills = TRUE;
	xtext->skip_stamp = TRUE;
	xtext->un_hilight = TRUE;

	gtk_xtext_render_ents (xtext, xtext->hilight_ent, NULL);

	xtext->render_hilights_only = FALSE;
	xtext->skip_border_fills = FALSE;
	xtext->skip_stamp = FALSE;
	xtext->un_hilight = FALSE;
}

static void
gtk_xtext_leave (GtkXText *xtext)
{
	GtkWidget *widget = GTK_WIDGET (xtext);

	if (xtext->cursor_hand)
	{
		gtk_xtext_unrender_hilight (xtext);
		xtext->hilight_start = -1;
		xtext->hilight_end = -1;
		xtext->cursor_hand = FALSE;
		gtk_widget_set_cursor (widget, NULL);
		xtext->hilight_ent = NULL;
	}

	if (xtext->cursor_resize)
	{
		gtk_xtext_unrender_hilight (xtext);
		xtext->hilight_start = -1;
		xtext->hilight_end = -1;
		xtext->cursor_resize = FALSE;
		gtk_widget_set_cursor (widget, NULL);
		xtext->hilight_ent = NULL;
	}
}

static void
gtk_xtext_leave_cb (GtkEventControllerMotion *controller, gpointer user_data)
{
	(void) controller;
	gtk_xtext_leave (user_data);
}

/* check if we should mark time stamps, and if a redraw is needed */

static gboolean
gtk_xtext_check_mark_stamp (GtkXText *xtext, GdkModifierType mask)
{
	gboolean redraw = FALSE;

	if ((mask & STATE_SHIFT || prefs.hex_text_autocopy_stamp)
	    && (!prefs.hex_stamp_text || prefs.hex_text_indent))
	{
		if (!xtext->mark_stamp)
		{
			redraw = TRUE;	/* must redraw all */
			xtext->mark_stamp = TRUE;
		}
	} else
	{
		if (xtext->mark_stamp)
		{
			redraw = TRUE;	/* must redraw all */
			xtext->mark_stamp = FALSE;
		}
	}
	return redraw;
}

static int
gtk_xtext_get_word_adjust (GtkXText *xtext, int x, int y, textentry **word_ent, int *offset, int *len)
{
	GSList *slp = NULL;
	unsigned char *word;
	int word_type = 0;

	word = gtk_xtext_get_word (xtext, x, y, word_ent, offset, len, &slp);
	if (word)
	{
		int laststart, lastend;

		word_type = xtext->urlcheck_function (GTK_WIDGET (xtext), word);
		if (word_type > 0)
		{
			if (url_last (&laststart, &lastend))
			{
				int cumlen, startadj = 0, endadj = 0;
				offlen_t *meta;
				GSList *sl;

				for (sl = slp, cumlen = 0; sl; sl = g_slist_next (sl))
				{
					meta = sl->data;
					startadj = meta->off - cumlen;
					cumlen += meta->len;
					if (laststart < cumlen)
						break;
				}
				for (sl = slp, cumlen = 0; sl; sl = g_slist_next (sl))
				{
					meta = sl->data;
					endadj = meta->off - cumlen;
					cumlen += meta->len;
					if (lastend < cumlen)
						break;
				}
				laststart += startadj;
				*offset += laststart;
				*len = lastend + endadj - laststart;
			}
		}
	}
	g_slist_free_full (slp, g_free);

	return word_type;
}

static void
gtk_xtext_motion_cb (GtkEventControllerMotion *controller,
                     gdouble dx, gdouble dy, gpointer user_data)
{
	GtkXText *xtext = user_data;
	GtkWidget *widget = GTK_WIDGET (xtext);
	int x, y, offset, len, line_x;
	textentry *word_ent;
	int word_type;

	(void) controller;
	x = (int) dx;
	y = (int) dy;

	/* While the user is dragging a selection or the separator bar,
	 * GtkGestureDrag's drag-update handler owns the work — we'd just
	 * double up if we tried to extend selection here too. Same for
	 * cursor changes; the cursor stays "selecting" through the drag. */
	if (xtext->button_down || xtext->moving_separator)
		return;

	if (xtext->separator && xtext->buffer->indent)
	{
		line_x = xtext->buffer->indent - ((xtext->space_width + 1) / 2);
		/* Same ±4 px hit zone as gtk_xtext_drag_begin_cb. */
		if (x >= line_x - 4 && x <= line_x + 4)
		{
			if (!xtext->cursor_resize)
			{
				gtk_widget_set_cursor (widget, xtext->resize_cursor);
				xtext->cursor_hand = FALSE;
				xtext->cursor_resize = TRUE;
			}
			return;
		}
	}

	if (xtext->urlcheck_function == NULL)
		return;

	word_type = gtk_xtext_get_word_adjust (xtext, x, y, &word_ent, &offset, &len);
	if (word_type > 0)
	{
		if (!xtext->cursor_hand ||
			 xtext->hilight_ent != word_ent ||
			 xtext->hilight_start != offset ||
			 xtext->hilight_end != offset + len)
		{
			if (!xtext->cursor_hand)
			{
				gtk_widget_set_cursor (widget, xtext->hand_cursor);
				xtext->cursor_hand = TRUE;
				xtext->cursor_resize = FALSE;
			}

			/* un-render the old hilight */
			if (xtext->hilight_ent)
				gtk_xtext_unrender_hilight (xtext);

			xtext->hilight_ent = word_ent;
			xtext->hilight_start = offset;
			xtext->hilight_end = offset + len;

			xtext->skip_border_fills = TRUE;
			xtext->render_hilights_only = TRUE;
			xtext->skip_stamp = TRUE;

			gtk_xtext_render_ents (xtext, word_ent, NULL);

			xtext->skip_border_fills = FALSE;
			xtext->render_hilights_only = FALSE;
			xtext->skip_stamp = FALSE;
		}
		return;
	}

	gtk_xtext_leave (xtext);
}

static void
gtk_xtext_set_clip_owner (GtkWidget * widget)
{
	GtkXText *xtext = GTK_XTEXT (widget);
	char *str;
	int len;

	if (xtext->selection_buffer && xtext->selection_buffer != xtext->buffer)
		gtk_xtext_selection_clear (xtext->selection_buffer);

	xtext->selection_buffer = xtext->buffer;

	str = gtk_xtext_selection_get_text (xtext, &len);
	if (str)
	{
		if (str[0])
		{
			/* Phase 4.9: GTK 4 unified the GtkClipboard / X11-selection
			 * model into GdkClipboard. The regular clipboard is what
			 * Ctrl+V pastes from; the primary clipboard is the X-style
			 * "select to copy, middle-click to paste" selection that
			 * Wayland and X both still expose. gtk_widget_get_clipboard /
			 * gtk_widget_get_primary_clipboard pick the clipboard for the
			 * widget's display. We don't need the GTK 3 selection-owner
			 * dance — gdk_clipboard_set_text is the whole API. */
			gdk_clipboard_set_text (gtk_widget_get_clipboard (widget), str);
			gdk_clipboard_set_text (gtk_widget_get_primary_clipboard (widget), str);
		}

		g_free (str);
	}
}

void
gtk_xtext_copy_selection (GtkXText *xtext)
{
	gtk_xtext_set_clip_owner (GTK_WIDGET (xtext));
}

static void
gtk_xtext_unselect (GtkXText *xtext)
{
	xtext_buffer *buf = xtext->buffer;

	xtext->skip_border_fills = TRUE;
	xtext->skip_stamp = TRUE;

	xtext->jump_in_offset = buf->last_ent_start->mark_start;
	/* just a single ent was marked? */
	if (buf->last_ent_start == buf->last_ent_end)
	{
		xtext->jump_out_offset = buf->last_ent_start->mark_end;
		buf->last_ent_end = NULL;
	}

	gtk_xtext_selection_clear (xtext->buffer);

	/* FIXME: use jump_out on multi-line selects too! */
	xtext->jump_in_offset = 0;
	xtext->jump_out_offset = 0;
	gtk_xtext_render_ents (xtext, buf->last_ent_start, buf->last_ent_end);

	xtext->skip_border_fills = FALSE;
	xtext->skip_stamp = FALSE;

	xtext->buffer->last_ent_start = NULL;
	xtext->buffer->last_ent_end = NULL;
}

/* Phase 4.9 follow-up: GtkGestureClick is the wrong tool for the
 * primary-button drag-to-select path. Once the pointer drifts past
 * the click threshold, the gesture's update routine resets n_press
 * to 0; gtk_gesture_click_end then refuses to emit "released"
 * (it gates on n_press > 0). Result: button_down stuck true, hover
 * motion keeps extending the selection, copy-on-release never runs.
 *
 * Fix: GtkGestureDrag drives the press/move/release cycle on the
 * primary button. drag-begin / drag-update / drag-end fire
 * unconditionally regardless of click recognition. GtkGestureClick
 * still handles right/middle-click WORD_CLICK and double/triple-
 * click word/line select via "pressed" — those are click-shaped,
 * not drag-shaped, so the GtkGestureClick semantics are right. */

static void
gtk_xtext_pressed_cb (GtkGestureClick *gesture, gint n_press,
                      gdouble dx, gdouble dy, gpointer user_data)
{
	GtkXText *xtext = user_data;
	GdkModifierType mask;
	textentry *ent;
	unsigned char *word;
	int offset, len;
	int x = (int) dx;
	int y = (int) dy;
	guint button;
	GdkEvent *event;

	button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));
	mask   = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
	event  = gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (gesture));

	/* right/middle click — emit WORD_CLICK so chat.c (or whoever)
	 * can pop a context menu. */
	if (button == GDK_BUTTON_SECONDARY || button == GDK_BUTTON_MIDDLE)
	{
		word = gtk_xtext_get_word (xtext, x, y, 0, 0, 0, 0);
		g_signal_emit (G_OBJECT (xtext), xtext_signals[WORD_CLICK], 0,
		                    word ? (gpointer) word : (gpointer) "", event);
		return;
	}

	if (button != GDK_BUTTON_PRIMARY)		  /* we only want left button */
		return;

	/* n_press == 1 is handled by the GtkGestureDrag — drag-begin sets
	 * button_down + select_start. We only care about double/triple
	 * click here. */

	if (n_press == 2)	/* WORD select */
	{
		gtk_xtext_check_mark_stamp (xtext, mask);
		if (gtk_xtext_get_word (xtext, x, y, &ent, &offset, &len, 0))
		{
			if (len == 0)
				return;
			gtk_xtext_selection_clear (xtext->buffer);
			ent->mark_start = offset;
			ent->mark_end = offset + len;
			gtk_xtext_selection_render (xtext, ent, ent);
			xtext->word_select = TRUE;
		}

		return;
	}

	if (n_press == 3)	/* LINE select */
	{
		gtk_xtext_check_mark_stamp (xtext, mask);
		if (gtk_xtext_get_word (xtext, x, y, &ent, 0, 0, 0))
		{
			gtk_xtext_selection_clear (xtext->buffer);
			ent->mark_start = 0;
			ent->mark_end = ent->str_len;
			gtk_xtext_selection_render (xtext, ent, ent);
			xtext->line_select = TRUE;
		}

		return;
	}
}

static void
gtk_xtext_drag_begin_cb (GtkGestureDrag *drag, gdouble x, gdouble y,
                         gpointer user_data)
{
	GtkXText *xtext = user_data;
	int ix = (int) x;
	int iy = (int) y;
	int line_x;

	(void) drag;

	/* check if the press landed on the separator bar — give a
	 * generous ±4 px tolerance. The original HexChat code only
	 * matched ±1 px which is hard to hit precisely on modern
	 * fractional-scale displays. */
	if (xtext->separator && xtext->buffer->indent)
	{
		line_x = xtext->buffer->indent - ((xtext->space_width + 1) / 2);
		if (ix >= line_x - 4 && ix <= line_x + 4)
		{
			xtext->moving_separator = TRUE;
			gtk_xtext_draw_sep (xtext, -1);
			return;
		}
	}

	/* normal drag-to-select start */
	xtext->button_down = TRUE;
	xtext->select_start_x = ix;
	xtext->select_start_y = iy;
	xtext->select_start_adj = gtk_adjustment_get_value (xtext->adj);
}

static void
gtk_xtext_drag_update_cb (GtkGestureDrag *drag, gdouble offset_x,
                          gdouble offset_y, gpointer user_data)
{
	GtkXText *xtext = user_data;
	GtkWidget *widget = GTK_WIDGET (xtext);
	GdkModifierType mask;
	int redraw, tmp;
	int x = xtext->select_start_x + (int) offset_x;
	int y = xtext->select_start_y + (int) offset_y;
	int alloc_w;

	mask = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (drag));

	if (xtext->moving_separator)
	{
		alloc_w = gtk_widget_get_width (widget);
		if (x < (3 * alloc_w) / 5 && x > 15)
		{
			tmp = xtext->buffer->indent;
			xtext->buffer->indent = x;
			gtk_xtext_fix_indent (xtext->buffer);
			if (tmp != xtext->buffer->indent)
			{
				gtk_xtext_recalc_widths (xtext->buffer, FALSE);
				if (xtext->buffer->scrollbar_down)
					gtk_adjustment_set_value (xtext->adj,
						gtk_adjustment_get_upper (xtext->adj) -
						gtk_adjustment_get_page_size (xtext->adj));
				if (!xtext->io_tag)
					xtext->io_tag = g_timeout_add (REFRESH_TIMEOUT,
						(GSourceFunc) gtk_xtext_adjustment_timeout, xtext);
				/* Phase 4.9 follow-up: render_page (called from the
				 * adjustment timeout) is a no-op outside the snapshot
				 * pass because xtext->cr is NULL. Without queueing a
				 * draw here the per-entry ent->indent updates that
				 * recalc_widths just made never get repainted — the
				 * separator ghost moves with the pointer but the
				 * nick column stays put. queue_draw schedules a
				 * snapshot which re-enters render_page WITH cr set
				 * and the text column finally reflows. */
				gtk_widget_queue_draw (widget);
			}
		}
		return;
	}

	if (!xtext->button_down)
		return;

	redraw = gtk_xtext_check_mark_stamp (xtext, mask);
	xtext->select_end_x = x;
	xtext->select_end_y = y;
	gtk_xtext_selection_update (xtext, y, !redraw);

	/* user has pressed or released SHIFT, must redraw entire selection */
	if (redraw)
	{
		xtext->force_stamp = TRUE;
		gtk_xtext_render_ents (xtext, xtext->buffer->last_ent_start,
		                              xtext->buffer->last_ent_end);
		xtext->force_stamp = FALSE;
	}
}

static void
gtk_xtext_drag_end_cb (GtkGestureDrag *drag, gdouble offset_x,
                       gdouble offset_y, gpointer user_data)
{
	GtkXText *xtext = user_data;
	GtkWidget *widget = GTK_WIDGET (xtext);
	unsigned char *word;
	int old;
	int alloc_w;
	int x = xtext->select_start_x + (int) offset_x;
	int y = xtext->select_start_y + (int) offset_y;
	GdkModifierType state;
	GdkEvent *event;

	state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (drag));
	event = gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (drag));

	alloc_w = gtk_widget_get_width (widget);
	if (xtext->moving_separator)
	{
		xtext->moving_separator = FALSE;
		old = xtext->buffer->indent;
		if (x < (4 * alloc_w) / 5 && x > 15)
			xtext->buffer->indent = x;
		gtk_xtext_fix_indent (xtext->buffer);
		if (xtext->buffer->indent != old)
		{
			gtk_xtext_recalc_widths (xtext->buffer, FALSE);
			gtk_xtext_adjustment_set (xtext->buffer, TRUE);
			gtk_xtext_render_page (xtext);
			/* Phase 4.9 follow-up: see drag-update for why this is
			 * needed — render_page outside a snapshot pass is a
			 * no-op, so we must queue_draw to schedule one. */
			gtk_widget_queue_draw (widget);
		} else
			gtk_xtext_draw_sep (xtext, -1);
		return;
	}

	xtext->button_down = FALSE;
	if (xtext->scroll_tag)
	{
		g_source_remove (xtext->scroll_tag);
		xtext->scroll_tag = 0;
	}

	/* got a new selection? */
	if (xtext->buffer->last_ent_start)
	{
		xtext->color_paste = FALSE;
		if (state & STATE_CTRL || prefs.hex_text_autocopy_color)
			xtext->color_paste = TRUE;
		if (prefs.hex_text_autocopy_text)
		{
			gtk_xtext_set_clip_owner (widget);
		}
	}

	if (xtext->word_select || xtext->line_select)
	{
		xtext->word_select = FALSE;
		xtext->line_select = FALSE;
		return;
	}

	/* zero-distance drag (i.e. a plain click) → unselect / WORD_CLICK */
	if (xtext->select_start_x == x &&
	    xtext->select_start_y == y &&
	    xtext->buffer->last_ent_start)
	{
		gtk_xtext_unselect (xtext);
		xtext->mark_stamp = FALSE;
		return;
	}

	if (!gtk_xtext_is_selecting (xtext))
	{
		word = gtk_xtext_get_word (xtext, x, y, 0, 0, 0, 0);
		g_signal_emit (G_OBJECT (xtext), xtext_signals[WORD_CLICK], 0,
		                    word ? (gpointer) word : (gpointer) "", event);
	}
}

static gboolean
gtk_xtext_is_selecting (GtkXText *xtext)
{
	textentry *ent;
	xtext_buffer *buf;

	buf = xtext->selection_buffer;
	if (!buf)
		return FALSE;

	for (ent = buf->last_ent_start; ent; ent = ent->next)
	{
		if (ent->mark_start != -1 && ent->mark_end - ent->mark_start > 0)
			return TRUE;

		if (ent == buf->last_ent_end)
			break;
	}

	return FALSE;
}

static char *
gtk_xtext_selection_get_text (GtkXText *xtext, int *len_ret)
{
	textentry *ent;
	char *txt;
	char *pos;
	char *stripped;
	int len;
	int first = TRUE;
	xtext_buffer *buf;

	buf = xtext->selection_buffer;
	if (!buf)
		return NULL;

	/* first find out how much we need to malloc ... */
	len = 0;
	ent = buf->last_ent_start;
	while (ent)
	{
		if (ent->mark_start != -1)
		{
			/* include timestamp? */
			if (ent->mark_start == 0 && xtext->mark_stamp)
			{
				char *time_str;
				int stamp_size = xtext_get_stamp_str (ent->stamp, &time_str);
				g_free (time_str);
				len += stamp_size;
			}

			if (ent->mark_end - ent->mark_start > 0)
				len += (ent->mark_end - ent->mark_start) + 1;
			else
				len++;
		}
		if (ent == buf->last_ent_end)
			break;
		ent = ent->next;
	}

	if (len < 1)
		return NULL;

	/* now allocate mem and copy buffer */
	pos = txt = g_malloc (len);
	ent = buf->last_ent_start;
	while (ent)
	{
		if (ent->mark_start != -1)
		{
			if (!first)
			{
				*pos = '\n';
				pos++;
			}
			first = FALSE;
			if (ent->mark_end - ent->mark_start > 0)
			{
				/* include timestamp? */
				if (ent->mark_start == 0 && xtext->mark_stamp)
				{
					char *time_str;
					int stamp_size = xtext_get_stamp_str (ent->stamp, &time_str);
					memcpy (pos, time_str, stamp_size);
					g_free (time_str);
					pos += stamp_size;
				}

				memcpy (pos, ent->str + ent->mark_start,
						  ent->mark_end - ent->mark_start);
				pos += ent->mark_end - ent->mark_start;
			}
		}
		if (ent == buf->last_ent_end)
			break;
		ent = ent->next;
	}
	*pos = 0;

	if (xtext->color_paste)
	{
		/*stripped = gtk_xtext_conv_color (txt, strlen (txt), &len);*/
		stripped = txt;
		len = strlen (txt);
	}
	else
	{
		stripped = gtk_xtext_strip_color (txt, strlen (txt), NULL, &len, NULL, FALSE);
		g_free (txt);
	}

	*len_ret = len;
	return stripped;
}

/* Phase 4.9: gtk_xtext_selection_get (X11 selection-protocol callback) and
 * gtk_xtext_selection_kill (called when another client took the selection)
 * went away with the GtkClipboard → GdkClipboard transition. GdkClipboard
 * owns its data via a GValue, so we don't supply text on demand and we
 * don't get told when ownership changes; the existing in-buffer marks
 * survive whether or not anyone else owns the X primary selection. */

static void
gtk_xtext_scroll_adjustments (GtkXText *xtext, GtkAdjustment *hadj, GtkAdjustment *vadj)
{
	/* hadj is ignored entirely */

	if (vadj)
		g_return_if_fail (GTK_IS_ADJUSTMENT (vadj));
	else
		vadj = GTK_ADJUSTMENT(gtk_adjustment_new (0, 0, 1, 1, 1, 1));

	if (xtext->adj && (xtext->adj != vadj))
	{
		g_signal_handlers_disconnect_by_func (xtext->adj,
								gtk_xtext_adjustment_changed,
								xtext);
		g_object_unref (xtext->adj);
	}

	if (xtext->adj != vadj)
	{
		xtext->adj = vadj;
		g_object_ref_sink (xtext->adj);

		xtext->vc_signal_tag = g_signal_connect (xtext->adj, "value-changed",
							G_CALLBACK (gtk_xtext_adjustment_changed),
							xtext);

		gtk_xtext_adjustment_changed (xtext->adj, xtext);
	}
}

static void
gtk_xtext_class_init (GtkXTextClass * class)
{
	GObjectClass   *object_class;
	GtkWidgetClass *widget_class;
	GtkXTextClass  *xtext_class;

	object_class = (GObjectClass *) class;
	widget_class = (GtkWidgetClass *) class;
	xtext_class  = (GtkXTextClass *) class;

	parent_class = g_type_class_peek (gtk_widget_get_type ());

	/* Phase 2.6: HexChat carries hand-written marshallers in
	 * common/marshal.c; we use g_cclosure_marshal_generic instead.
	 * Phase 3.4b: GtkObject is gone in GTK 3 — every signal now hangs
	 * off the GObjectClass / GtkWidgetClass directly. */
	xtext_signals[WORD_CLICK] =
		g_signal_new ("word_click",
							G_TYPE_FROM_CLASS (class),
							G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION,
							G_STRUCT_OFFSET (GtkXTextClass, word_click),
							NULL, NULL,
							g_cclosure_marshal_generic,
							G_TYPE_NONE,
							2, G_TYPE_POINTER, G_TYPE_POINTER);
	xtext_signals[SET_SCROLL_ADJUSTMENTS] =
		g_signal_new ("set_scroll_adjustments",
							G_TYPE_FROM_CLASS (class),
							G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
							G_STRUCT_OFFSET (GtkXTextClass, set_scroll_adjustments),
							NULL, NULL,
							g_cclosure_marshal_generic,
							G_TYPE_NONE,
							2, GTK_TYPE_ADJUSTMENT, GTK_TYPE_ADJUSTMENT);

	/* Phase 4.9: GTK 4 vfunc wiring.
	 *  - GtkWidgetClass::destroy is gone; teardown moved to
	 *    GObjectClass::dispose.
	 *  - GtkWidgetClass::draw is gone; widgets implement ::snapshot.
	 *  - button_press_event / button_release_event / motion_notify_event /
	 *    leave_notify_event / scroll_event / selection_get /
	 *    selection_clear_event are gone — clicks/motion/scroll move
	 *    to GtkEventController controllers attached at instance_init,
	 *    and selection ownership migrates to GdkClipboard. The
	 *    underlying handlers (gtk_xtext_button_press etc.) are
	 *    Phase 4.9 follow-up: they're still defined below but no
	 *    longer wired into the class.
	 */
	object_class->dispose = gtk_xtext_dispose;

	widget_class->realize       = gtk_xtext_realize;
	widget_class->unrealize     = gtk_xtext_unrealize;
	widget_class->size_allocate = gtk_xtext_size_allocate;
	widget_class->snapshot      = gtk_xtext_snapshot;

	xtext_class->word_click             = NULL;
	xtext_class->set_scroll_adjustments = gtk_xtext_scroll_adjustments;
}

GType
gtk_xtext_get_type (void)
{
	static GType xtext_type = 0;

	if (!xtext_type)
	{
		static const GTypeInfo xtext_info =
		{
			sizeof (GtkXTextClass),
			NULL,		/* base_init */
			NULL,		/* base_finalize */
			(GClassInitFunc) gtk_xtext_class_init,
			NULL,		/* class_finalize */
			NULL,		/* class_data */
			sizeof (GtkXText),
			0,		/* n_preallocs */
			(GInstanceInitFunc) gtk_xtext_init,
		};

		xtext_type = g_type_register_static (GTK_TYPE_WIDGET, "GtkXText",
														 &xtext_info, 0);
	}

	return xtext_type;
}

/* strip MIRC colors and other attribs. */

/* CL: needs to strip hidden when called by gtk_xtext_text_width, but not when copying text */

typedef struct chunk_s {
	GSList *slp;
	int off1, len1, emph;
	offlen_t meta;
} chunk_t;

static void
xtext_do_chunk(chunk_t *c)
{
	offlen_t *meta;

	if (c->len1 == 0)
		return;

	meta = g_new (offlen_t, 1);
	meta->off = c->off1;
	meta->len = c->len1;
	meta->emph = c->emph;
	meta->width = 0;
	c->slp = g_slist_append (c->slp, meta);

	c->len1 = 0;
}

static unsigned char *
gtk_xtext_strip_color (unsigned char *text, int len, unsigned char *outbuf,
							  int *newlen, GSList **slpp, int strip_hidden)
{
	chunk_t c;
	int i = 0;
	int rcol = 0, bgcol = 0;
	int hidden = FALSE;
	unsigned char *new_str;
	unsigned char *text0 = text;
	int mbl;	/* multi-byte length */

	if (outbuf == NULL)
		new_str = g_malloc (len + 2);
	else
		new_str = outbuf;

	c.slp = NULL;
	c.off1 = 0;
	c.len1 = 0;
	c.emph = 0;
	while (len > 0)
	{
		mbl = charlen (text);
		if (mbl > len)
			goto bad_utf8;

		if (rcol > 0 && (isdigit (*text) || (*text == ',' && isdigit (text[1]) && !bgcol)))
		{
			if (text[1] != ',') rcol--;
			if (*text == ',')
			{
				rcol = 2;
				bgcol = 1;
			}
		} else
		{
			rcol = bgcol = 0;
			switch (*text)
			{
			case ATTR_COLOR:
				xtext_do_chunk (&c);
				rcol = 2;
				break;
			case ATTR_BEEP:
			case ATTR_RESET:
			case ATTR_REVERSE:
			case ATTR_BOLD:
			case ATTR_UNDERLINE:
			case ATTR_STRIKETHROUGH:
			case ATTR_ITALICS:
				xtext_do_chunk (&c);
				if (*text == ATTR_RESET)
					c.emph = 0;
				if (*text == ATTR_ITALICS)
					c.emph ^= EMPH_ITAL;
				if (*text == ATTR_BOLD)
					c.emph ^= EMPH_BOLD;
				break;
			case ATTR_HIDDEN:
				xtext_do_chunk (&c);
				c.emph ^= EMPH_HIDDEN;
				hidden = !hidden;
				break;
			default:
				if (strip_hidden == 2 || (!(hidden && strip_hidden)))
				{
					if (c.len1 == 0)
						c.off1 = text - text0;
					memcpy (new_str + i, text, mbl);
					i += mbl;
					c.len1 += mbl;
				}
			}
		}
		text += mbl;
		len -= mbl;
	}

bad_utf8:		/* Normal ending sequence, and give up if bad utf8 */
	xtext_do_chunk (&c);

	new_str[i] = 0;

	if (newlen != NULL)
		*newlen = i;

	if (slpp)
		*slpp = c.slp;
	else
		g_slist_free_full (c.slp, g_free);

	return new_str;
}

/* gives width of a string, excluding the mIRC codes */

static int
gtk_xtext_text_width_ent (GtkXText *xtext, textentry *ent)
{
	unsigned char *new_buf;
	GSList *slp0, *slp;
	int width;

	if (ent->slp)
	{
		g_slist_free_full (ent->slp, g_free);
		ent->slp = NULL;
	}

	new_buf = gtk_xtext_strip_color (ent->str, ent->str_len, xtext->scratch_buffer,
												NULL, &slp0, 2);

	width =  backend_get_text_width_slp (xtext, new_buf, slp0);
	ent->slp = slp0;

	for (slp = slp0; slp; slp = g_slist_next (slp))
	{
		offlen_t *meta;

		meta = slp->data;
		meta->width = backend_get_text_width_emph (xtext, ent->str + meta->off, meta->len, meta->emph);
	}
	return width;
}

static int
gtk_xtext_text_width (GtkXText *xtext, unsigned char *text, int len)
{
	unsigned char *new_buf;
	int new_len;
	GSList *slp;
	int width;

	new_buf = gtk_xtext_strip_color (text, len, xtext->scratch_buffer,
												&new_len, &slp, !xtext->ignore_hidden);

	width =  backend_get_text_width_slp (xtext, new_buf, slp);
	g_slist_free_full (slp, g_free);

	return width;
}

/* actually draw text to screen (one run with the same color/attribs)
 *
 * Phase 3.4b: the previous implementation built a per-run GdkPixmap,
 * tile-aligned the bg GC, drew text into it, then blitted via
 * gdk_draw_drawable to align the background pattern across runs.  Cairo
 * achieves the same effect natively: a CAIRO_EXTEND_REPEAT pattern set
 * with cairo_pattern_set_matrix at (-ts_x, -ts_y) tiles correctly across
 * the whole widget without any per-run buffering.  So we just draw
 * straight to xtext->cr — no intermediate surface, no blit. */

static int
gtk_xtext_render_flush (GtkXText * xtext, int x, int y, unsigned char *str,
                        int len, int *emphasis,
                        int mark_local_start, int mark_local_end)
{
	int str_width, dofill;
	cairo_t *cr = xtext->cr;
	int dest_x = 0;

	if (xtext->dont_render || len < 1 || xtext->hidden)
		return 0;

	str_width = backend_get_text_width_emph (xtext, str, len, *emphasis);

	if (xtext->dont_render2)
		return str_width;

	if (x > xtext->clip_x2 || x + str_width < xtext->clip_x)
		return str_width;
	if (y - xtext->font->ascent > xtext->clip_y2 || (y - xtext->font->ascent) + xtext->fontsize < xtext->clip_y)
		return str_width;

	if (xtext->render_hilights_only)
	{
		if (!xtext->in_hilight)	/* is it a hilight prefix? */
			return str_width;
		if (!xtext->un_hilight)	/* doing a hilight? no need to draw the text */
			goto dounder;
	}

	dest_x = x;
	dofill = TRUE;

	backend_draw_text_emph (xtext, dofill, xtext->cur_fg, xtext->cur_bg,
	                        x, y, (char *) str, len, str_width, *emphasis,
	                        mark_local_start, mark_local_end);

	if (cr && xtext->strikethrough)
	{
		int sy = y - xtext->font->ascent + (xtext->fontsize / 2);
		cairo_save (cr);
		xtext_cairo_set_source_idx (xtext, cr, xtext->cur_fg);
		cairo_set_line_width (cr, 1);
		cairo_move_to (cr, dest_x, sy + 0.5);
		cairo_line_to (cr, dest_x + str_width, sy + 0.5);
		cairo_stroke (cr);
		cairo_restore (cr);
	}

	if (xtext->underline)
	{
dounder:
		if (cr)
		{
			int uy = y + 1;
			cairo_save (cr);
			xtext_cairo_set_source_idx (xtext, cr, xtext->cur_fg);
			cairo_set_line_width (cr, 1);
			cairo_move_to (cr, dest_x, uy + 0.5);
			cairo_line_to (cr, dest_x + str_width, uy + 0.5);
			cairo_stroke (cr);
			cairo_restore (cr);
		}
	}

	return str_width;
}

static void
gtk_xtext_reset (GtkXText * xtext, int mark, int attribs)
{
	if (attribs)
	{
		xtext->underline = FALSE;
		xtext->strikethrough = FALSE;
		xtext->hidden = FALSE;
	}
	if (!mark)
	{
		xtext->backcolor = FALSE;
		if (xtext->col_fore != XTEXT_FG)
			xtext_set_fg (xtext, XTEXT_FG);
		if (xtext->col_back != XTEXT_BG)
			xtext_set_bg (xtext, XTEXT_BG);
	}
	xtext->col_fore = XTEXT_FG;
	xtext->col_back = XTEXT_BG;
	xtext->parsing_color = FALSE;
	xtext->parsing_backcolor = FALSE;
	xtext->nc = 0;
}

/*
 * gtk_xtext_search_offset (buf, ent, off) --
 * Look for arg offset in arg textentry
 * Return one or more flags:
 * 	GTK_MATCH_MID if we are in a match
 * 	GTK_MATCH_START if we're at the first byte of it
 * 	GTK_MATCH_END if we're at the first byte past it
 * 	GTK_MATCH_CUR if it is the current match
 */
#define GTK_MATCH_START	1
#define GTK_MATCH_MID	2
#define GTK_MATCH_END	4
#define GTK_MATCH_CUR	8
static int
gtk_xtext_search_offset (xtext_buffer *buf, textentry *ent, unsigned int off)
{
	GList *gl;
	offsets_t o;
	int flags = 0;

	for (gl = g_list_first (ent->marks); gl; gl = g_list_next (gl))
	{
		o.u = GPOINTER_TO_UINT (gl->data);
		if (off < o.o.start || off > o.o.end)
			continue;
		flags = GTK_MATCH_MID;
		if (off == o.o.start)
			flags |= GTK_MATCH_START;
		if (off == o.o.end)
		{
			gl = g_list_next (gl);
			if (gl)
			{
				o.u = GPOINTER_TO_UINT (gl->data);
				if (off ==  o.o.start)	/* If subseq match is adjacent */
				{
					flags |= (gl == buf->curmark)? GTK_MATCH_CUR: 0;
				}
				else		/* If subseq match is not adjacent */
				{
					flags |= GTK_MATCH_END;
				}
			}
			else		/* If there is no subseq match */
			{
				flags |= GTK_MATCH_END;
			}
		}
		else if (gl == buf->curmark)	/* If not yet at the end of this match */
		{
			flags |= GTK_MATCH_CUR;
		}
		break;
	}
	return flags;
}

/* render a single line, which WONT wrap, and parse mIRC colors */

/* Phase 4.9 follow-up: compute the run-local byte range of the selection
 * for the run that's about to be flushed (pstr..pstr+j inside ent->str).
 * mark_start / mark_end are entry-wide byte offsets; translate by the
 * run's offset within ent->str. Empty/clamped-to-zero range means "no
 * selection in this run." Doing it inline in the macro keeps RENDER_FLUSH
 * a single expression so the existing `x += RENDER_FLUSH` callsites
 * compile unchanged. */
#define RENDER_FLUSH \
	x += gtk_xtext_render_flush (xtext, x, y, pstr, j, emphasis, \
		((ent->mark_start < 0) \
			? -1 \
			: CLAMP (ent->mark_start - (int)((pstr) - ent->str), 0, j)), \
		((ent->mark_end < 0) \
			? -1 \
			: CLAMP (ent->mark_end   - (int)((pstr) - ent->str), 0, j)))

static int
gtk_xtext_render_str (GtkXText * xtext, int y, textentry * ent,
							 unsigned char *str, int len, int win_width, int indent,
							 int line, int left_only, int *x_size_ret, int *emphasis)
{
	int i = 0, x = indent, j = 0;
	unsigned char *pstr = str;
	int col_num, tmp;
	int offset;
	int mark = FALSE;
	int ret = 1;
	int k;
	int srch_underline = FALSE;
	int srch_mark = FALSE;

	xtext->in_hilight = FALSE;

	offset = str - ent->str;
	/* Phase 3.4b: GdkGC removed; xtext->cur_fg/cur_bg track current colors. */

	if (ent->mark_start != -1 &&
		 ent->mark_start <= i + offset && ent->mark_end > i + offset)
	{
		/* Phase 4.9 follow-up: don't switch run colors here — the
		 * mark fg/bg get applied per-run via PangoAttribute in
		 * backend_draw_text_emph instead, so glyph positions don't
		 * depend on whether the selection sits inside this subline. */
		mark = TRUE;
	}
	if (xtext->hilight_ent == ent &&
		 xtext->hilight_start <= i + offset && xtext->hilight_end > i + offset)
	{
		if (!xtext->un_hilight)
		{
			xtext->underline = TRUE;
		}
		xtext->in_hilight = TRUE;
	}

	if (!xtext->skip_border_fills && !xtext->dont_render)
	{
		/* draw background to the left of the text */
		if (str == ent->str && indent > MARGIN && xtext->buffer->time_stamp)
		{
			/* don't overwrite the timestamp */
			if (indent > xtext->stamp_width)
			{
				xtext_draw_bg (xtext, xtext->stamp_width, y - xtext->font->ascent,
									indent - xtext->stamp_width, xtext->fontsize);
			}
		} else
		{
			/* fill the indent area with background gc */
			if (indent >= xtext->clip_x)
			{
				xtext_draw_bg (xtext, 0, y - xtext->font->ascent,
									MIN (indent, xtext->clip_x2), xtext->fontsize);
			}
		}
	}

	if (xtext->jump_in_offset > 0 && offset < xtext->jump_in_offset)
		xtext->dont_render2 = TRUE;

	while (i < len)
	{

		if (xtext->hilight_ent == ent && xtext->hilight_start == (i + offset))
		{
			RENDER_FLUSH;
			pstr += j;
			j = 0;
			if (!xtext->un_hilight)
			{
				xtext->underline = TRUE;
			}

			xtext->in_hilight = TRUE;
		}

		if ((xtext->parsing_color && isdigit (str[i]) && xtext->nc < 2) ||
			 (xtext->parsing_color && str[i] == ',' && isdigit (str[i+1]) && xtext->nc < 3 && !xtext->parsing_backcolor))
		{
			pstr++;
			if (str[i] == ',')
			{
				xtext->parsing_backcolor = TRUE;
				if (xtext->nc)
				{
					xtext->num[xtext->nc] = 0;
					xtext->nc = 0;
					col_num = atoi (xtext->num);
					if (col_num == 99)	/* mIRC lameness */
						col_num = XTEXT_FG;
					else
					if (col_num > XTEXT_MAX_COLOR)
						col_num = col_num % XTEXT_MIRC_COLS;
					xtext->col_fore = col_num;
					if (!mark)
						xtext_set_fg (xtext, col_num);
				}
			} else
			{
				xtext->num[xtext->nc] = str[i];
				if (xtext->nc < 7)
					xtext->nc++;
			}
		} else
		{
			if (xtext->parsing_color)
			{
				xtext->parsing_color = FALSE;
				if (xtext->nc)
				{
					xtext->num[xtext->nc] = 0;
					xtext->nc = 0;
					col_num = atoi (xtext->num);
					if (xtext->parsing_backcolor)
					{
						if (col_num == 99)	/* mIRC lameness */
							col_num = XTEXT_BG;
						else
						if (col_num > XTEXT_MAX_COLOR)
							col_num = col_num % XTEXT_MIRC_COLS;
						if (col_num == XTEXT_BG)
							xtext->backcolor = FALSE;
						else
							xtext->backcolor = TRUE;
						if (!mark)
							xtext_set_bg (xtext, col_num);
						xtext->col_back = col_num;
					} else
					{
						if (col_num == 99)	/* mIRC lameness */
							col_num = XTEXT_FG;
						else
						if (col_num > XTEXT_MAX_COLOR)
							col_num = col_num % XTEXT_MIRC_COLS;
						if (!mark)
							xtext_set_fg (xtext, col_num);
						xtext->col_fore = col_num;
					}
					xtext->parsing_backcolor = FALSE;
				} else
				{
					/* got a \003<non-digit>... i.e. reset colors */
					RENDER_FLUSH;
					pstr += j;
					j = 0;
					gtk_xtext_reset (xtext, mark, FALSE);
				}
			}

			if (!left_only && !mark &&
				 (k = gtk_xtext_search_offset (xtext->buffer, ent, offset + i)))
			{
				RENDER_FLUSH;
				pstr += j;
				j = 0;
				if (!(xtext->buffer->search_flags & highlight))
				{
					if (k & GTK_MATCH_CUR)
					{
						xtext_set_bg (xtext, XTEXT_MARK_BG);
						xtext_set_fg (xtext, XTEXT_MARK_FG);
						xtext->backcolor = TRUE;
						srch_mark = TRUE;
					} else
					{
						xtext_set_bg (xtext, xtext->col_back);
						xtext_set_fg (xtext, xtext->col_fore);
						xtext->backcolor = (xtext->col_back != XTEXT_BG)? TRUE: FALSE;
						srch_mark = FALSE;
					}
				}
				else
				{
					xtext->underline = (k & GTK_MATCH_CUR)? TRUE: FALSE;
					if (k & (GTK_MATCH_START | GTK_MATCH_MID))
					{
						xtext_set_bg (xtext, XTEXT_MARK_BG);
						xtext_set_fg (xtext, XTEXT_MARK_FG);
						xtext->backcolor = TRUE;
						srch_mark = TRUE;
					}
					if (k & GTK_MATCH_END)
					{
						xtext_set_bg (xtext, xtext->col_back);
						xtext_set_fg (xtext, xtext->col_fore);
						xtext->backcolor = (xtext->col_back != XTEXT_BG)? TRUE: FALSE;
						srch_mark = FALSE;
						xtext->underline = FALSE;
					}
					srch_underline = xtext->underline;
				}
			}

			switch (str[i])
			{
			case '\n':
			/*case ATTR_BEEP:*/
				break;
			case ATTR_REVERSE:
				RENDER_FLUSH;
				pstr += j + 1;
				j = 0;
				tmp = xtext->col_fore;
				xtext->col_fore = xtext->col_back;
				xtext->col_back = tmp;
				if (!mark)
				{
					xtext_set_fg (xtext, xtext->col_fore);
					xtext_set_bg (xtext, xtext->col_back);
				}
				if (xtext->col_back != XTEXT_BG)
					xtext->backcolor = TRUE;
				else
					xtext->backcolor = FALSE;
				break;
			case ATTR_BOLD:
				RENDER_FLUSH;
				*emphasis ^= EMPH_BOLD;
				pstr += j + 1;
				j = 0;
				break;
			case ATTR_UNDERLINE:
				RENDER_FLUSH;
				xtext->underline = !xtext->underline;
				pstr += j + 1;
				j = 0;
				break;
			case ATTR_STRIKETHROUGH:
				RENDER_FLUSH;
				xtext->strikethrough = !xtext->strikethrough;
				pstr += j + 1;
				j = 0;
				break;
			case ATTR_ITALICS:
				RENDER_FLUSH;
				*emphasis ^= EMPH_ITAL;
				pstr += j + 1;
				j = 0;
				break;
			case ATTR_HIDDEN:
				RENDER_FLUSH;
				xtext->hidden = (!xtext->hidden) & (!xtext->ignore_hidden);
				pstr += j + 1;
				j = 0;
				break;
			case ATTR_RESET:
				RENDER_FLUSH;
				*emphasis = 0;
				pstr += j + 1;
				j = 0;
				gtk_xtext_reset (xtext, mark, !xtext->in_hilight);
				break;
			case ATTR_COLOR:
				RENDER_FLUSH;
				xtext->parsing_color = TRUE;
				pstr += j + 1;
				j = 0;
				break;
			default:
				tmp = charlen (str + i);
				/* invalid utf8 safe guard */
				if (tmp + i > len)
					tmp = len - i;
				j += tmp;	/* move to the next utf8 char */
			}
		}
		i += charlen (str + i);	/* move to the next utf8 char */
		/* invalid utf8 safe guard */
		if (i > len)
			i = len;

		/* Separate the left part, the space and the right part
		   into separate runs, and reset bidi state inbetween.
		   Perform this only on the first line of the message.
                */
		if (offset == 0)
		{
			/* we've reached the end of the left part? */
			if ((pstr-str)+j == ent->left_len)
			{
				RENDER_FLUSH;
				pstr += j;
				j = 0;
			}
			else if ((pstr-str)+j == ent->left_len+1)
			{
				RENDER_FLUSH;
				pstr += j;
				j = 0;
			}
		}

		/* have we been told to stop rendering at this point? */
		if (xtext->jump_out_offset > 0 && xtext->jump_out_offset <= (i + offset))
		{
			RENDER_FLUSH;
			ret = 0;	/* skip the rest of the lines, we're done. */
			j = 0;
			break;
		}

		if (xtext->jump_in_offset > 0 && xtext->jump_in_offset == (i + offset))
		{
			RENDER_FLUSH;
			pstr += j;
			j = 0;
			xtext->dont_render2 = FALSE;
		}

		if (xtext->hilight_ent == ent && xtext->hilight_end == (i + offset))
		{
			RENDER_FLUSH;
			pstr += j;
			j = 0;
			xtext->underline = FALSE;
			xtext->in_hilight = FALSE;
			if (xtext->render_hilights_only)
			{
				/* stop drawing this ent */
				ret = 0;
				break;
			}
		}

		/* Phase 4.9 follow-up: selection coloring is now applied as
		 * Pango attributes on whatever run contains the marked range
		 * (see backend_draw_text_emph + RENDER_FLUSH). We still track
		 * `mark` as a state flag — it gates srch_underline interaction
		 * and the post-loop final-run color reset — but we no longer
		 * split runs at mark_start / mark_end, which is what was
		 * causing the trailing text to drift when the selection
		 * extended (per-run combined-extent rounding doesn't sum
		 * across multiple sub-layouts the way a single layout does). */
		if (!mark && ent->mark_start != -1 &&
		    ent->mark_start <= (i + offset) && ent->mark_end > (i + offset))
		{
			if (srch_underline)
			{
				xtext->underline = FALSE;
				srch_underline = FALSE;
			}
			mark = TRUE;
		}

		if (mark && ent->mark_end <= (i + offset))
		{
			mark = FALSE;
		}

	}

	if (j)
		RENDER_FLUSH;

	if (mark || srch_mark)
	{
		xtext_set_bg (xtext, xtext->col_back);
		xtext_set_fg (xtext, xtext->col_fore);
		if (xtext->col_back != XTEXT_BG)
			xtext->backcolor = TRUE;
		else
			xtext->backcolor = FALSE;
	}

	/* draw background to the right of the text */
	if (!left_only && !xtext->dont_render)
	{
		/* draw separator now so it doesn't appear to flicker */
		gtk_xtext_draw_sep (xtext, y - xtext->font->ascent);
		if (!xtext->skip_border_fills && xtext->clip_x2 >= x)
		{
			int xx = MAX (x, xtext->clip_x);

			xtext_draw_bg (xtext,
								xx,	/* x */
								y - xtext->font->ascent, /* y */
				MIN (xtext->clip_x2 - xx, (win_width + MARGIN) - xx), /* width */
								xtext->fontsize);		/* height */
		}
	}

	xtext->dont_render2 = FALSE;

	/* return how much we drew in the x direction */
	if (x_size_ret)
		*x_size_ret = x - indent;

	return ret;
}

/* walk through str until this line doesn't fit anymore */

static int
find_next_wrap (GtkXText * xtext, textentry * ent, unsigned char *str,
					 int win_width, int indent)
{
	unsigned char *last_space = str;
	unsigned char *orig_str = str;
	int str_width = indent;
	int rcol = 0, bgcol = 0;
	int hidden = FALSE;
	int mbl;
	int char_width;
	int ret;
	int limit_offset = 0;
	int emphasis = 0;
	GSList *lp;

	/* single liners */
	if (win_width >= ent->str_width + ent->indent)
		return ent->str_len;

	/* it does happen! */
	if (win_width < 1)
	{
		ret = ent->str_len - (str - ent->str);
		goto done;
	}

	/* Find emphasis value for the offset that is the first byte of our string */
	for (lp = ent->slp; lp; lp = g_slist_next (lp))
	{
		offlen_t *meta = lp->data;
		unsigned char *start, *end;

		start = ent->str + meta->off;
		end = start + meta->len;
		if (str >= start && str < end)
		{
			emphasis = meta->emph;
			break;
		}
	}

	while (1)
	{
		if (rcol > 0 && (isdigit (*str) || (*str == ',' && isdigit (str[1]) && !bgcol)))
		{
			if (str[1] != ',') rcol--;
			if (*str == ',')
			{
				rcol = 2;
				bgcol = 1;
			}
			limit_offset++;
			str++;
		} else
		{
			rcol = bgcol = 0;
			switch (*str)
			{
			case ATTR_COLOR:
				rcol = 2;
			case ATTR_BEEP:
			case ATTR_RESET:
			case ATTR_REVERSE:
			case ATTR_BOLD:
			case ATTR_UNDERLINE:
			case ATTR_STRIKETHROUGH:
			case ATTR_ITALICS:
				if (*str == ATTR_RESET)
					emphasis = 0;
				if (*str == ATTR_ITALICS)
					emphasis ^= EMPH_ITAL;
				if (*str == ATTR_BOLD)
					emphasis ^= EMPH_BOLD;
				limit_offset++;
				str++;
				break;
			case ATTR_HIDDEN:
				if (xtext->ignore_hidden)
					goto def;
				hidden = !hidden;
				limit_offset++;
				str++;
				break;
			default:
			def:
				mbl = charlen (str);
				char_width = backend_get_text_width_emph (xtext, str, mbl, emphasis);
				if (!hidden) str_width += char_width;
				if (str_width > win_width)
				{
					if (xtext->wordwrap)
					{
						if (str - last_space > WORDWRAP_LIMIT + limit_offset)
							ret = str - orig_str; /* fall back to character wrap */
						else
						{
							if (*last_space == ' ')
								last_space++;
							ret = last_space - orig_str;
							if (ret == 0) /* fall back to character wrap */
								ret = str - orig_str;
						}
						goto done;
					}
					ret = str - orig_str;
					goto done;
				}

				/* keep a record of the last space, for wordwrapping */
				if (is_del (*str))
				{
					last_space = str;
					limit_offset = 0;
				}

				/* progress to the next char */
				str += mbl;

			}
		}

		if (str >= ent->str + ent->str_len)
		{
			ret = str - orig_str;
			goto done;
		}
	}

done:

	/* must make progress */
	if (ret < 1)
		ret = 1;

	return ret;
}

/* find the offset, in bytes, that wrap number 'line' starts at */

static int
gtk_xtext_find_subline (GtkXText *xtext, textentry *ent, int line)
{
	int rlen = 0;

	if (line > 0)
	{
		rlen = GPOINTER_TO_UINT (g_slist_nth_data (ent->sublines, line - 1));
		if (rlen == 0)
			rlen = ent->str_len;
	}
	return rlen;
}

/* horrible hack for drawing time stamps */

static void
gtk_xtext_render_stamp (GtkXText * xtext, textentry * ent,
								char *text, int len, int line, int win_width)
{
	textentry tmp_ent;
	int jo, ji, hs;
	int xsize, y, emphasis;

	/* trashing ent here, so make a backup first */
	memcpy (&tmp_ent, ent, sizeof (tmp_ent));
	jo = xtext->jump_out_offset;	/* back these up */
	ji = xtext->jump_in_offset;
	hs = xtext->hilight_start;
	xtext->jump_out_offset = 0;
	xtext->jump_in_offset = 0;
	xtext->hilight_start = 0xffff;	/* temp disable */
	emphasis = 0;

	if (xtext->mark_stamp)
	{
		/* if this line is marked, mark this stamp too */
		if (ent->mark_start == 0)	
		{
			ent->mark_start = 0;
			ent->mark_end = len;
		}
		else
		{
			ent->mark_start = -1;
			ent->mark_end = -1;
		}
		ent->str = text;
	}

	y = (xtext->fontsize * line) + xtext->font->ascent - xtext->pixel_offset;
	gtk_xtext_render_str (xtext, y, ent, text, len,
								 win_width, 2, line, TRUE, &xsize, &emphasis);

	/* restore everything back to how it was */
	memcpy (ent, &tmp_ent, sizeof (tmp_ent));
	xtext->jump_out_offset = jo;
	xtext->jump_in_offset = ji;
	xtext->hilight_start = hs;

	/* with a non-fixed-width font, sometimes we don't draw enough
		background i.e. when this stamp is shorter than xtext->stamp_width */
	xsize += MARGIN;
	if (xsize < xtext->stamp_width)
	{
		y -= xtext->font->ascent;
		xtext_draw_bg (xtext,
							xsize,	/* x */
							y,			/* y */
							xtext->stamp_width - xsize,	/* width */
							xtext->fontsize					/* height */);
	}
}

/* render a single line, which may wrap to more lines */

static int
gtk_xtext_render_line (GtkXText * xtext, textentry * ent, int line,
							  int lines_max, int subline, int win_width)
{
	unsigned char *str;
	int indent, taken, entline, len, y, start_subline;
	int emphasis = 0;

	entline = taken = 0;
	str = ent->str;
	indent = ent->indent;
	start_subline = subline;

	/* draw the timestamp */
	debug_log ("stamp",
	           "render_line: auto_indent=%d time_stamp=%d skip_stamp=%d "
	           "mark_stamp=%d force_stamp=%d ent->indent=%d buf->indent=%d "
	           "stamp_width=%d",
	           xtext->auto_indent, xtext->buffer->time_stamp,
	           xtext->skip_stamp, xtext->mark_stamp, xtext->force_stamp,
	           ent->indent, xtext->buffer->indent, xtext->stamp_width);
	if (xtext->auto_indent && xtext->buffer->time_stamp &&
		 (!xtext->skip_stamp || xtext->mark_stamp || xtext->force_stamp))
	{
		char *time_str;
		int len;

		len = xtext_get_stamp_str (ent->stamp, &time_str);
		debug_log ("stamp",
		           "render_line: drawing stamp '%s' (len=%d) at line=%d",
		           time_str, len, line);
		gtk_xtext_render_stamp (xtext, ent, time_str, len, line, win_width);
		g_free (time_str);
	}

	/* draw each line one by one */
	do
	{
		if (entline > 0)
			len = GPOINTER_TO_INT (g_slist_nth_data (ent->sublines, entline)) - GPOINTER_TO_INT (g_slist_nth_data (ent->sublines, entline - 1));
		else
			len = GPOINTER_TO_INT (g_slist_nth_data (ent->sublines, entline));

		entline++;

		y = (xtext->fontsize * line) + xtext->font->ascent - xtext->pixel_offset;
		if (!subline)
		{
			if (!gtk_xtext_render_str (xtext, y, ent, str, len, win_width,
												indent, line, FALSE, NULL, &emphasis))
			{
				/* small optimization */
				gtk_xtext_draw_marker (xtext, ent, y - xtext->fontsize * (taken + start_subline + 1));
				return g_slist_length (ent->sublines) - subline;
			}
		} else
		{
			xtext->dont_render = TRUE;
			gtk_xtext_render_str (xtext, y, ent, str, len, win_width,
										 indent, line, FALSE, NULL, &emphasis);
			xtext->dont_render = FALSE;
			subline--;
			line--;
			taken--;
		}

		indent = xtext->buffer->indent;
		line++;
		taken++;
		str += len;

		if (line >= lines_max)
			break;

	}
	while (str < ent->str + ent->str_len);

	gtk_xtext_draw_marker (xtext, ent, y - xtext->fontsize * (taken + start_subline));

	return taken;
}

void
gtk_xtext_set_palette (GtkXText * xtext, GdkRGBA palette[])
{
	int i;

	for (i = (XTEXT_COLS-1); i >= 0; i--)
	{
		xtext->palette[i] = palette[i];
	}

	xtext->col_fore = XTEXT_FG;
	xtext->col_back = XTEXT_BG;
	xtext->cur_fg = XTEXT_FG;
	xtext->cur_bg = XTEXT_BG;
	gtk_widget_queue_draw (GTK_WIDGET (xtext));
}

static void
gtk_xtext_fix_indent (xtext_buffer *buf)
{
	int j;

	/* make indent a multiple of the space width */
	if (buf->indent && buf->xtext->space_width)
	{
		j = 0;
		while (j < buf->indent)
		{
			j += buf->xtext->space_width;
		}
		buf->indent = j;
	}

	dontscroll (buf);	/* force scrolling off */
}

static void
gtk_xtext_recalc_widths (xtext_buffer *buf, int do_str_width)
{
	textentry *ent;

	/* since we have a new font, we have to recalc the text widths */
	ent = buf->text_first;
	while (ent)
	{
		if (do_str_width)
		{
			ent->str_width = gtk_xtext_text_width_ent (buf->xtext, ent);
		}
		if (ent->left_len != -1)
		{
			ent->indent =
				(buf->indent -
				 gtk_xtext_text_width (buf->xtext, ent->str,
										ent->left_len)) - buf->xtext->space_width;
			if (ent->indent < MARGIN)
				ent->indent = MARGIN;
		}
		ent = ent->next;
	}

	gtk_xtext_calc_lines (buf, FALSE);
}

int
gtk_xtext_set_font (GtkXText *xtext, char *name)
{

	if (xtext->font)
		backend_font_close (xtext);

	/* Phase 3.4b: the GTK 1.2-era forced realize here was needed so the
	 * old XDrawString backend had a XDisplay. The cairo+Pango backend
	 * resolves fonts through the widget's Pango context, which falls
	 * back to the default screen on unrealized widgets — and forcing
	 * realize at construction time triggers an `anchored' assertion
	 * because callers set the font right after gtk_xtext_new(), before
	 * parenting the widget. Just drop it. */

	backend_font_open (xtext, name);
	if (xtext->font == NULL)
		return FALSE;

	{
		char *time_str;
		int stamp_size = xtext_get_stamp_str (time(0), &time_str);
		xtext->stamp_width =
			gtk_xtext_text_width (xtext, time_str, stamp_size) + MARGIN;
		g_free (time_str);
	}

	gtk_xtext_fix_indent (xtext->buffer);

	if (gtk_widget_get_realized (GTK_WIDGET(xtext)))
		gtk_xtext_recalc_widths (xtext->buffer, TRUE);

	return TRUE;
}

/* Phase 5: live update of the timestamp format. Stores a copy in the
 * module-global xtext_stamp_format (single per-process — every xtext
 * widget shares the same format). Recomputes xtext->stamp_width
 * against the new format, since the pixel width depends on what
 * strftime expanded to. Re-grows buf->indent if the new width is
 * wider than the old indent (matches the set_time_stamp grow path
 * so the message body stays right of the stamp column). Queues a
 * redraw so the new format and column width are picked up
 * immediately.
 *
 * NULL / empty restores the built-in default (XTEXT_STAMP_FORMAT_DEFAULT).
 * The caller's buffer can be freed afterwards; we keep a g_strdup. */
void
gtk_xtext_set_stamp_format (GtkXText *xtext, const char *format)
{
	g_free (xtext_stamp_format);
	xtext_stamp_format = (format && *format) ? g_strdup (format) : NULL;

	if (!xtext || !xtext->font)
		return;

	{
		char *time_str;
		int stamp_size = xtext_get_stamp_str (time (0), &time_str);
		xtext->stamp_width = gtk_xtext_text_width (xtext, time_str,
		                                           stamp_size) + MARGIN;
		g_free (time_str);
	}

	if (xtext->buffer && xtext->buffer->time_stamp) {
		int min_indent = xtext->stamp_width + xtext->space_width;
		if (xtext->buffer->indent < min_indent) {
			xtext->buffer->indent = min_indent;
			gtk_xtext_fix_indent (xtext->buffer);
			gtk_xtext_recalc_widths (xtext->buffer, FALSE);
		}
	}
	if (gtk_widget_get_realized (GTK_WIDGET (xtext)))
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
}

/* gtk_xtext_save was dropped from the public API in the GTK 4 port
 * (see xtext.h header comment) — no consumers, and the helper relied
 * on hexchat write helpers we don't carry. The body is kept as a
 * static so its stripping-by-color logic can be referenced by the
 * eventual saved-log feature without re-discovering the algorithm. */
static void G_GNUC_UNUSED
gtk_xtext_save (GtkXText * xtext, int fh)
{
	textentry *ent;
	int newlen;
	char *buf;

	ent = xtext->buffer->text_first;
	while (ent)
	{
		buf = gtk_xtext_strip_color (ent->str, ent->str_len, NULL,
											  &newlen, NULL, FALSE);
		write (fh, buf, newlen);
		write (fh, "\n", 1);
		g_free (buf);
		ent = ent->next;
	}
}

/* count how many lines 'ent' will take (with wraps) */

static int
gtk_xtext_lines_taken (xtext_buffer *buf, textentry * ent)
{
	unsigned char *str;
	int indent, len;
	int win_width;

	g_slist_free (ent->sublines);
	ent->sublines = NULL;
	win_width = buf->window_width - MARGIN;

	if (win_width >= ent->indent + ent->str_width)
	{
		ent->sublines = g_slist_append (ent->sublines, GINT_TO_POINTER (ent->str_len));
		return 1;
	}

	indent = ent->indent;
	str = ent->str;

	do
	{
		len = find_next_wrap (buf->xtext, ent, str, win_width, indent);
		ent->sublines = g_slist_append (ent->sublines, GINT_TO_POINTER (str + len - ent->str));
		indent = buf->indent;
		str += len;
	}
	while (str < ent->str + ent->str_len);

	return g_slist_length (ent->sublines);
}

/* Calculate number of actual lines (with wraps), to set adj->lower. *
 * This should only be called when the window resizes.               */

static void
gtk_xtext_calc_lines (xtext_buffer *buf, int fire_signal)
{
	textentry *ent;
	int width;
	int height;
	int lines;

	/* Phase 4.9: gtk_widget_get_window / gdk_window_get_width / _height
	 * are gone in GTK 4. Widget size is available directly via
	 * gtk_widget_get_width / gtk_widget_get_height (the same numbers
	 * the size_allocate vfunc receives). */
	height = gtk_widget_get_height (GTK_WIDGET (buf->xtext));
	width  = gtk_widget_get_width  (GTK_WIDGET (buf->xtext));
	width -= MARGIN;

	if (width < 30 || height < buf->xtext->fontsize || width < buf->indent + 30)
		return;

	lines = 0;
	ent = buf->text_first;
	while (ent)
	{
		lines += gtk_xtext_lines_taken (buf, ent);
		ent = ent->next;
	}

	buf->pagetop_ent = NULL;
	buf->num_lines = lines;
	gtk_xtext_adjustment_set (buf, fire_signal);
}

/* find the n-th line in the linked list, this includes wrap calculations */

static textentry *
gtk_xtext_nth (GtkXText *xtext, int line, int *subline)
{
	int lines = 0;
	textentry *ent;

	ent = xtext->buffer->text_first;

	/* -- optimization -- try to make a short-cut using the pagetop ent */
	if (xtext->buffer->pagetop_ent)
	{
		if (line == xtext->buffer->pagetop_line)
		{
			*subline = xtext->buffer->pagetop_subline;
			return xtext->buffer->pagetop_ent;
		}
		if (line > xtext->buffer->pagetop_line)
		{
			/* lets start from the pagetop instead of the absolute beginning */
			ent = xtext->buffer->pagetop_ent;
			lines = xtext->buffer->pagetop_line - xtext->buffer->pagetop_subline;
		}
		else if (line > xtext->buffer->pagetop_line - line)
		{
			/* move backwards from pagetop */
			ent = xtext->buffer->pagetop_ent;
			lines = xtext->buffer->pagetop_line - xtext->buffer->pagetop_subline;
			while (1)
			{
				if (lines <= line)
				{
					*subline = line - lines;
					return ent;
				}
				ent = ent->prev;
				if (!ent)
					break;
				lines -= g_slist_length (ent->sublines);
			}
			return NULL;
		}
	}
	/* -- end of optimization -- */

	while (ent)
	{
		lines += g_slist_length (ent->sublines);
		if (lines > line)
		{
			*subline = g_slist_length (ent->sublines) - (lines - line);
			return ent;
		}
		ent = ent->next;
	}
	return NULL;
}

/* render enta (or an inclusive range enta->entb) */

static int
gtk_xtext_render_ents (GtkXText * xtext, textentry * enta, textentry * entb)
{
	textentry *ent, *orig_ent, *tmp_ent;
	int line;
	int lines_max;
	int width;
	int height;
	int subline;
	int drawing = FALSE;

	/* Phase 4.9 follow-up: outside a snapshot pass we have no cairo
	 * context to draw into — backend_draw_text_emph silently bails on
	 * cr==NULL. That's fine for the actual drawing, but the caller
	 * has already mutated the source-of-truth state (mark_start/end
	 * for selection, hilight_ent / hilight_start / hilight_end for
	 * URL hilights, jump_in/out_offset, etc.). Queue a redraw so the
	 * next snapshot picks up those changes; the snapshot path's own
	 * gtk_xtext_paint will re-enter render_ents with cr set and
	 * actually draw.
	 *
	 * Without this, drag-to-select, single-click-to-deselect, and
	 * URL hand-cursor hilighting all silently no-op until something
	 * else invalidates the widget (a dialog, scroll, focus change). */
	if (xtext->cr == NULL)
	{
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
		return 0;
	}

	if (xtext->buffer->indent < MARGIN)
		xtext->buffer->indent = MARGIN;	  /* 2 pixels is our left margin */

	/* Phase 4.9: see gtk_xtext_calc_lines / gtk_xtext_render_page note. */
	height = gtk_widget_get_height (GTK_WIDGET (xtext));
	width  = gtk_widget_get_width  (GTK_WIDGET (xtext));
	width -= MARGIN;

	if (width < 32 || height < xtext->fontsize || width < xtext->buffer->indent + 30)
		return 0;

	lines_max = ((height + xtext->pixel_offset) / xtext->fontsize) + 1;
	line = 0;
	orig_ent = xtext->buffer->pagetop_ent;
	subline = xtext->buffer->pagetop_subline;

	/* used before a complete page is in buffer */
	if (orig_ent == NULL)
		orig_ent = xtext->buffer->text_first;

	/* check if enta is before the start of this page */
	if (entb)
	{
		tmp_ent = orig_ent;
		while (tmp_ent)
		{
			if (tmp_ent == enta)
				break;
			if (tmp_ent == entb)
			{
				drawing = TRUE;
				break;
			}
			tmp_ent = tmp_ent->next;
		}
	}

	ent = orig_ent;
	while (ent)
	{
		if (entb && ent == enta)
			drawing = TRUE;

		if (drawing || ent == entb || ent == enta)
		{
			gtk_xtext_reset (xtext, FALSE, TRUE);
			line += gtk_xtext_render_line (xtext, ent, line, lines_max,
													 subline, width);
			subline = 0;
			xtext->jump_in_offset = 0;	/* jump_in_offset only for the 1st */
		} else
		{
			if (ent == orig_ent)
			{
				line -= subline;
				subline = 0;
			}
			line += g_slist_length (ent->sublines);
		}

		if (ent == entb)
			break;

		if (line >= lines_max)
			break;

		ent = ent->next;
	}

	/* space below last line */
	return (xtext->fontsize * line) - xtext->pixel_offset;
}

/* render a whole page/window, starting from 'startline' */

static void
gtk_xtext_render_page (GtkXText * xtext)
{
	textentry *ent;
	int line;
	int lines_max;
	int width;
	int height;
	int subline;
	int startline = (int)gtk_adjustment_get_value(xtext->adj);
	int pos, overlap;

	if(!gtk_widget_get_realized(GTK_WIDGET(xtext)))
	  return;

	if (xtext->buffer->indent < MARGIN)
		xtext->buffer->indent = MARGIN;	  /* 2 pixels is our left margin */

	/* Phase 4.9: GdkWindow / gdk_window_get_width / _height /
	 * gtk_widget_get_window are gone in GTK 4. Widget pixel size is
	 * available directly off the widget. */
	width  = gtk_widget_get_width  (GTK_WIDGET (xtext));
	height = gtk_widget_get_height (GTK_WIDGET (xtext));

	if (width < 34 || height < xtext->fontsize || width < xtext->buffer->indent + 32)
		return;

	xtext->pixel_offset = ((int)gtk_adjustment_get_value(xtext->adj) - startline) * xtext->fontsize;

	subline = line = 0;
	ent = xtext->buffer->text_first;

	if (startline > 0)
		ent = gtk_xtext_nth (xtext, startline, &subline);

	xtext->buffer->pagetop_ent = ent;
	xtext->buffer->pagetop_subline = subline;
	xtext->buffer->pagetop_line = startline;

	if (xtext->buffer->num_lines <= gtk_adjustment_get_page_size(xtext->adj))
		dontscroll (xtext->buffer);

	pos = (int)gtk_adjustment_get_value(xtext->adj) * xtext->fontsize;
	overlap = xtext->buffer->last_pixel_pos - pos;
	xtext->buffer->last_pixel_pos = pos;
	(void) overlap;	/* Phase 3.4b: scroll-by-blit optimization removed
		 * (relied on gdk_draw_drawable + gdk_gc_set_exposures).  Cairo's
		 * draw model handles partial repaints via the dirty rect supplied
		 * to the draw signal — caller code does gtk_widget_queue_draw_area
		 * for selective repaint instead. */

	width -= MARGIN;
	lines_max = ((height + xtext->pixel_offset) / xtext->fontsize) + 1;

	while (ent)
	{
		gtk_xtext_reset (xtext, FALSE, TRUE);
		line += gtk_xtext_render_line (xtext, ent, line, lines_max,
												 subline, width);
		subline = 0;

		if (line >= lines_max)
			break;

		ent = ent->next;
	}

	line = (xtext->fontsize * line) - xtext->pixel_offset;
	/* fill any space below the last line with our background GC */
	xtext_draw_bg (xtext, 0, line, width + MARGIN, height - line);

	/* draw the separator line */
	gtk_xtext_draw_sep (xtext, -1);
}

void
gtk_xtext_refresh (GtkXText * xtext)
{
	if (gtk_widget_get_realized (GTK_WIDGET (xtext)))
	{
		/* Phase 5: render_page only paints when xtext->cr is non-NULL
		 * (i.e. inside the snapshot pass). Calling it from a regular
		 * callback like changed_xtext / changed_timestamp finds cr
		 * NULL and silently no-ops — the toggle takes effect on the
		 * next snapshot the widget happens to get, which may be much
		 * later. Queue a draw so GTK schedules a snapshot right away;
		 * the in-snapshot render_page that follows actually paints. */
		gtk_xtext_render_page (xtext);
		gtk_widget_queue_draw (GTK_WIDGET (xtext));
	}
}

static int
gtk_xtext_kill_ent (xtext_buffer *buffer, textentry *ent)
{
	int visible;

	/* Set visible to TRUE if this is the current buffer */
	/* and this ent shows up on the screen now */
	visible = buffer->xtext->buffer == buffer &&
				 gtk_xtext_check_ent_visibility (buffer->xtext, ent, 0);

	if (ent == buffer->pagetop_ent)
		buffer->pagetop_ent = NULL;

	if (ent == buffer->last_ent_start)
	{
		buffer->last_ent_start = ent->next;
		buffer->last_offset_start = 0;
	}

	if (ent == buffer->last_ent_end)
	{
		buffer->last_ent_start = NULL;
		buffer->last_ent_end = NULL;
	}

	if (buffer->marker_pos == ent)
	{
		/* Allow for "Marker line reset because exceeded scrollback limit. to appear. */
		buffer->marker_pos = ent->next;
		buffer->marker_state = MARKER_RESET_BY_KILL;
	}

	if (ent->marks)
	{
		gtk_xtext_search_textentry_del (buffer, ent);
	}

	g_slist_free_full (ent->slp, g_free);
	g_slist_free (ent->sublines);

	g_free (ent);
	return visible;
}

/* remove the topline from the list */

static void
gtk_xtext_remove_top (xtext_buffer *buffer)
{
	textentry *ent;

	ent = buffer->text_first;
	if (!ent)
		return;
	buffer->num_lines -= g_slist_length (ent->sublines);
	buffer->pagetop_line -= g_slist_length (ent->sublines);
	buffer->last_pixel_pos -= (g_slist_length (ent->sublines) * buffer->xtext->fontsize);
	buffer->text_first = ent->next;
	if (buffer->text_first)
		buffer->text_first->prev = NULL;
	else
		buffer->text_last = NULL;

	buffer->old_value -= g_slist_length (ent->sublines);
	if (buffer->xtext->buffer == buffer)	/* is it the current buffer? */
	{
		gtk_adjustment_set_value(buffer->xtext->adj, gtk_adjustment_get_value(buffer->xtext->adj) - g_slist_length (ent->sublines));
		buffer->xtext->select_start_adj -= g_slist_length (ent->sublines);
	}

	if (gtk_xtext_kill_ent (buffer, ent))
	{
		if (!buffer->xtext->add_io_tag)
		{
			/* remove scrolling events */
			if (buffer->xtext->io_tag)
			{
				g_source_remove (buffer->xtext->io_tag);
				buffer->xtext->io_tag = 0;
			}
			buffer->xtext->force_render = TRUE;
			buffer->xtext->add_io_tag = g_timeout_add (REFRESH_TIMEOUT * 2,
														(GSourceFunc)
														gtk_xtext_render_page_timeout,
														buffer->xtext);
		}
	}
}

static void
gtk_xtext_remove_bottom (xtext_buffer *buffer)
{
	textentry *ent;

	ent = buffer->text_last;
	if (!ent)
		return;
	buffer->num_lines -= g_slist_length (ent->sublines);
	buffer->text_last = ent->prev;
	if (buffer->text_last)
		buffer->text_last->next = NULL;
	else
		buffer->text_first = NULL;

	if (gtk_xtext_kill_ent (buffer, ent))
	{
		if (!buffer->xtext->add_io_tag)
		{
			/* remove scrolling events */
			if (buffer->xtext->io_tag)
			{
				g_source_remove (buffer->xtext->io_tag);
				buffer->xtext->io_tag = 0;
			}
			buffer->xtext->force_render = TRUE;
			buffer->xtext->add_io_tag = g_timeout_add (REFRESH_TIMEOUT * 2,
														(GSourceFunc)
														gtk_xtext_render_page_timeout,
														buffer->xtext);
		}
	}
}

/* If lines=0 => clear all */

void
gtk_xtext_clear (xtext_buffer *buf, int lines)
{
	textentry *next;
	int marker_reset = FALSE;

	if (lines != 0)
	{
		if (lines < 0)
		{
			/* delete lines from bottom */
			lines *= -1;
			while (lines)
			{
				if (buf->text_last == buf->marker_pos)
					marker_reset = TRUE;
				gtk_xtext_remove_bottom (buf);
				lines--;
			}
		}
		else
		{
			/* delete lines from top */
			while (lines)
			{
				if (buf->text_first == buf->marker_pos)
					marker_reset = TRUE;
				gtk_xtext_remove_top (buf);
				lines--;
			}
		}
	}
	else
	{
		/* delete all */
		if (buf->search_found)
			gtk_xtext_search_fini (buf);
		if (buf->xtext->auto_indent) {
			/* Phase 5: preserve the timestamp-column indent. When
			 * buf->time_stamp is on, gtk_xtext_set_time_stamp grew
			 * buf->indent to stamp_width + space_width so the
			 * message body draws to the right of the bare-HH:MM:SS
			 * stamp xtext draws on the left margin. Resetting to
			 * MARGIN here (hx_clear_chat → gtk_xtext_clear, fires
			 * at the start of every connect) wipes that out, and
			 * the subsequent appends end up with ent->indent = 2;
			 * render_stamp paints, render_str repaints the same
			 * column with the message bytes, and the stamps look
			 * invisible. Re-grow to the stamp floor when applicable,
			 * fall back to MARGIN otherwise. */
			if (buf->time_stamp) {
				buf->indent = buf->xtext->stamp_width
				            + buf->xtext->space_width;
				gtk_xtext_fix_indent (buf);
			} else {
				buf->indent = MARGIN;
			}
		}
		buf->scrollbar_down = TRUE;
		buf->last_ent_start = NULL;
		buf->last_ent_end = NULL;
		buf->marker_pos = NULL;
		if (buf->text_first)
			marker_reset = TRUE;
		dontscroll (buf);

		while (buf->text_first)
		{
			next = buf->text_first->next;
			g_free (buf->text_first);
			buf->text_first = next;
		}
		buf->text_last = NULL;
	}

	if (buf->xtext->buffer == buf)
	{
		gtk_xtext_calc_lines (buf, TRUE);
		gtk_xtext_refresh (buf->xtext);
	} else
	{
		gtk_xtext_calc_lines (buf, FALSE);
	}

	if (marker_reset)
		buf->marker_state = MARKER_RESET_BY_CLEAR;
}

static gboolean
gtk_xtext_check_ent_visibility (GtkXText * xtext, textentry *find_ent, int add)
{
	textentry *ent;
	int lines;
	xtext_buffer *buf = xtext->buffer;
	int height;

	if (find_ent == NULL)
	{
		return FALSE;
	}

	/* Phase 4.9: see render_page note. */
	height = gtk_widget_get_height (GTK_WIDGET (xtext));

	ent = buf->pagetop_ent;
	/* If top line not completely displayed return FALSE */
	if (ent == find_ent && buf->pagetop_subline > 0)
	{
		return FALSE;
	}
	/* Loop through line positions looking for find_ent */
	lines = ((height + xtext->pixel_offset) / xtext->fontsize) + buf->pagetop_subline + add;
	while (ent)	
	{
		lines -= g_slist_length (ent->sublines);
		if (lines <= 0)
		{
			return FALSE;
		}
		if (ent == find_ent)
		{
			return TRUE;
		}
		ent = ent->next;
	}

	return FALSE;
}

void
gtk_xtext_check_marker_visibility (GtkXText * xtext)
{
	if (gtk_xtext_check_ent_visibility (xtext, xtext->buffer->marker_pos, 1))
		xtext->buffer->marker_seen = TRUE;
}

static void
gtk_xtext_unstrip_color (gint start, gint end, GSList *slp, GList **gl, gint maxo)
{
	gint off1, off2, curlen;
	GSList *cursl;
	offsets_t marks;
	offlen_t *meta;

	off1 = 0;
	curlen = 0;
	cursl = slp;
	while (cursl)
	{
		meta = cursl->data;
		if (start < meta->len)
		{
			off1 = meta->off + start;
			break;
		}
		curlen += meta->len;
		start -= meta->len;
		end -= meta->len;
		cursl = g_slist_next (cursl);
	}

	off2 = off1;
	while (cursl)
	{
		meta = cursl->data;
		if (end < meta->len)
		{
			off2 = meta->off + end;
			break;
		}
		curlen += meta->len;
		end -= meta->len;
		cursl = g_slist_next (cursl);
	}
	if (!cursl)
	{
		off2 = maxo;
	}

	marks.o.start = off1;
	marks.o.end = off2;
	*gl = g_list_append (*gl, GUINT_TO_POINTER (marks.u));
}

/* Search a single textentry for occurrence(s) of search arg string */
static GList *
gtk_xtext_search_textentry (xtext_buffer *buf, textentry *ent)
{
	gchar *str;								/* text string to be searched */
	GList *gl = NULL;
	GSList *slp;
	gint lstr;

	if (buf->search_text == NULL)
	{
		return gl;
	}

	str = gtk_xtext_strip_color (ent->str, ent->str_len, buf->xtext->scratch_buffer,
										  &lstr, &slp, !buf->xtext->ignore_hidden);

	/* Regular-expression matching --- */
	if (buf->search_flags & regexp)
	{
		GMatchInfo *gmi;
		gint start, end;

		if (buf->search_re == NULL)
		{
			return gl;
		}
		g_regex_match (buf->search_re, str, 0, &gmi);
		while (g_match_info_matches (gmi))
		{
			g_match_info_fetch_pos (gmi, 0,  &start, &end);
			gtk_xtext_unstrip_color (start, end, slp, &gl, ent->str_len);
			g_match_info_next (gmi, NULL);
		}
		g_match_info_free (gmi);

	/* Non-regular-expression matching --- */
	} else {
		gchar *hay, *pos;
		gint lhay, off, len;
		gint match = buf->search_flags & case_match;

		hay = match? g_strdup (str): g_utf8_casefold (str, lstr);
		lhay = strlen (hay);

		for (pos = hay, len = lhay; len;
			  off += buf->search_lnee, pos = hay + off, len = lhay - off)
		{
			str = g_strstr_len (pos, len, buf->search_nee);
			if (str == NULL)
			{
				break;
			}
			off = str - hay;
			gtk_xtext_unstrip_color (off, off + buf->search_lnee,
											 slp, &gl, ent->str_len);
		}

		g_free (hay);
	}

	/* Common processing --- */
	g_slist_free_full (slp, g_free);
	return gl;
}

/* Add a list of found search results to an entry, maybe NULL */
static void
gtk_xtext_search_textentry_add (xtext_buffer *buf, textentry *ent, GList *gl, gboolean pre)
{
	ent->marks = gl;
	if (gl)
	{
		buf->search_found = (pre? g_list_prepend: g_list_append) (buf->search_found, ent);
		if (pre == FALSE && buf->hintsearch == NULL)
		{
			buf->hintsearch = ent;
		}
	}
}

/* Free all search information for a textentry */
static void
gtk_xtext_search_textentry_del (xtext_buffer *buf, textentry *ent)
{
	g_list_free (ent->marks);
	ent->marks = NULL;
	if (buf->cursearch && buf->cursearch->data == ent)
	{
		buf->cursearch = NULL;
		buf->curmark = NULL;
		buf->curdata.u = 0;
	}
	if (buf->pagetop_ent == ent)
	{
		buf->pagetop_ent = NULL;
	}
	if (buf->hintsearch == ent)
	{
		buf->hintsearch = NULL;
	}
	buf->search_found = g_list_remove (buf->search_found, ent);
}

/* Used only by glist_foreach */
static void
gtk_xtext_search_textentry_fini (gpointer entp, gpointer dummy)
{
	textentry *ent = entp;

	g_list_free (ent->marks);
	ent->marks = NULL;
}

/* Free all search information for all textentrys and the xtext_buffer */
static void
gtk_xtext_search_fini (xtext_buffer *buf)
{
	g_list_foreach (buf->search_found, gtk_xtext_search_textentry_fini, 0);
	g_list_free (buf->search_found);
	buf->search_found = NULL;
	g_free (buf->search_text);
	buf->search_text = NULL;
	g_free (buf->search_nee);
	buf->search_nee = NULL;
	buf->search_flags = 0;
	buf->cursearch = NULL;
	buf->curmark = NULL;
	/* but leave buf->curdata.u alone! */
	if (buf->search_re)
	{
		g_regex_unref (buf->search_re);
		buf->search_re = NULL;
	}
}

/* Returns TRUE if the base search information exists and is still okay to use */
static gboolean
gtk_xtext_search_init (xtext_buffer *buf, const gchar *text, gtk_xtext_search_flags flags, GError **perr)
{
	/* Of the five flags, backward and highlight_all do not need a new search */
	if (buf->search_found &&
		 strcmp (buf->search_text, text) == 0 &&
		 (buf->search_flags & case_match) == (flags & case_match) &&
		 (buf->search_flags & follow) == (flags & follow) &&
		 (buf->search_flags & regexp) == (flags & regexp))
	{
		return TRUE;
	}
	buf->hintsearch = buf->cursearch? buf->cursearch->data: NULL;
	gtk_xtext_search_fini (buf);
	buf->search_text = g_strdup (text);
	if (flags & regexp)
	{
		buf->search_re = g_regex_new (text, (flags & case_match)? 0: G_REGEX_CASELESS, 0, perr);
		if (perr && *perr)
		{
			return FALSE;
		}
	}
	else
	{
		if (flags & case_match)
		{
			buf->search_nee = g_strdup (text);
		}
		else
		{
			buf->search_nee = g_utf8_casefold (text, strlen (text));
		}
		buf->search_lnee = strlen (buf->search_nee);
	}
	buf->search_flags = flags;
	buf->cursearch = NULL;
	buf->curmark = NULL;
	/* but leave buf->curdata.u alone! */
	return FALSE;
}

#define BACKWARD (flags & backward)
#define FIRSTLAST(lp)  (BACKWARD? g_list_last(lp): g_list_first(lp))
#define NEXTPREVIOUS(lp) (BACKWARD? g_list_previous(lp): g_list_next(lp))
textentry *
gtk_xtext_search (GtkXText * xtext, const gchar *text, gtk_xtext_search_flags flags, GError **perr)
{
	textentry *ent = NULL;
	xtext_buffer *buf = xtext->buffer;
	GList *gl;

	if (buf->text_first == NULL)
	{
		return NULL;
	}

	/* If the text arg is NULL, one of these has been toggled: highlight follow */
	if (text == NULL)		/* Here on highlight or follow toggle */
	{
		gint oldfollow = buf->search_flags & follow;
		gint newfollow = flags & follow;

		/* If "Follow" has just been checked, search possible new textentries --- */
		if (newfollow && (newfollow != oldfollow))
		{
			gl = g_list_last (buf->search_found);
			ent = gl? gl->data: buf->text_first;
			for (; ent; ent = ent->next)
			{
				GList *gl;

				gl = gtk_xtext_search_textentry (buf, ent);
				gtk_xtext_search_textentry_add (buf, ent, gl, FALSE);
			}
		}
		buf->search_flags = flags;
		ent = buf->pagetop_ent;
	}

	/* if the text arg is "", the reset button has been clicked or Control-Shift-F has been hit */
	else if (text[0] == 0)		/* Let a null string do a reset. */
	{
		gtk_xtext_search_fini (buf);
	}

	/* If the text arg is neither NULL nor "", it's the search string */
	else
	{
		if (gtk_xtext_search_init (buf, text, flags, perr) == FALSE)	/* If a new search: */
		{
			if (perr && *perr)
			{
				return NULL;
			}
			for (ent = buf->text_first; ent; ent = ent->next)
			{
				GList *gl;

				gl = gtk_xtext_search_textentry (buf, ent);
				gtk_xtext_search_textentry_add (buf, ent, gl, TRUE);
			}
			buf->search_found = g_list_reverse (buf->search_found);
		}

		/* Now base search results are in place. */

		if (buf->search_found)
		{
			/* If we're in the midst of moving among found items */
			if (buf->cursearch)
			{
				ent = buf->cursearch->data;
				buf->curmark = NEXTPREVIOUS (buf->curmark);
				if (buf->curmark == NULL)
				{
					/* We've returned all the matches for this textentry. */
					buf->cursearch = NEXTPREVIOUS (buf->cursearch);
					if (buf->cursearch)
					{
						ent = buf->cursearch->data;
						buf->curmark = FIRSTLAST (ent->marks);
					}
					else	/* We've returned all the matches for all textentries */
					{
						ent = NULL;
					}
				}
			}

			/* If user changed the search, let's look starting where he was */
			else if (buf->hintsearch)
			{
				GList *mark;
				offsets_t last, this;
				/*
				 * If we already have a 'current' item from the last search, and if
				 * the first character of an occurrence on this line for this new search
				 * is within that former item, use the occurrence as current.
				 */
				ent = buf->hintsearch;
				last.u = buf->curdata.u;
				for (mark = ent->marks; mark; mark = mark->next)
				{
					this.u = GPOINTER_TO_UINT (mark->data);
					if (this.o.start >= last.o.start && this.o.start < last.o.end)
					break;
				}
				if (mark == NULL)
				{
					for (ent = buf->hintsearch; ent; ent = BACKWARD? ent->prev: ent->next)
						if (ent->marks)
							break;
					mark = ent? FIRSTLAST (ent->marks): NULL;
				}
				buf->cursearch = g_list_find (buf->search_found, ent);
				buf->curmark = mark;
			}

			/* This is a fresh search */
			else
			{
				buf->cursearch = FIRSTLAST (buf->search_found);
				ent = buf->cursearch->data;
				buf->curmark = FIRSTLAST (ent->marks);
			}
			buf->curdata.u = (buf->curmark)? GPOINTER_TO_UINT (buf->curmark->data): 0;
		}
	}
	buf->hintsearch = ent;

	if (!gtk_xtext_check_ent_visibility (xtext, ent, 1))
	{
		GtkAdjustment *adj = xtext->adj;
		float value;

		buf->pagetop_ent = NULL;
		for (value = 0, ent = buf->text_first;
			  ent && ent != buf->hintsearch; ent = ent->next)
		{
			value += g_slist_length (ent->sublines);
		}
		if (value > gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj))
		{
			value = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj);
		}
		else if ((flags & backward)  && ent)
		{
			value -= gtk_adjustment_get_page_size(adj) - g_slist_length (ent->sublines);
			if (value < 0)
			{
				value = 0;
			}
		}
		gtk_adjustment_set_value (adj, value);
	}

	gtk_widget_queue_draw (GTK_WIDGET (xtext));

	return buf->hintsearch;
}
#undef BACKWARD
#undef FIRSTLAST
#undef NEXTPREVIOUS

static int
gtk_xtext_render_page_timeout (GtkXText * xtext)
{
	GtkAdjustment *adj = xtext->adj;

	xtext->add_io_tag = 0;

	/* less than a complete page? */
	if (xtext->buffer->num_lines <= gtk_adjustment_get_page_size(adj))
	{
		xtext->buffer->old_value = 0;
		gtk_adjustment_set_value(adj, 0);
		gtk_xtext_render_page (xtext);
	} else if (xtext->buffer->scrollbar_down)
	{
		g_signal_handler_block (xtext->adj, xtext->vc_signal_tag);
		gtk_xtext_adjustment_set (xtext->buffer, FALSE);
		gtk_adjustment_set_value (adj, gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
		g_signal_handler_unblock (xtext->adj, xtext->vc_signal_tag);
		xtext->buffer->old_value = gtk_adjustment_get_value(adj);
		gtk_xtext_render_page (xtext);
	} else
	{
		gtk_xtext_adjustment_set (xtext->buffer, TRUE);
		if (xtext->force_render)
		{
			xtext->force_render = FALSE;
			gtk_xtext_render_page (xtext);
		}
	}

	return 0;
}

/* append a textentry to our linked list */

static void
gtk_xtext_append_entry (xtext_buffer *buf, textentry * ent, time_t stamp)
{
	int i;

	/* we don't like tabs */
	i = 0;
	while (i < ent->str_len)
	{
		if (ent->str[i] == '\t')
			ent->str[i] = ' ';
		i++;
	}

	ent->stamp = stamp;
	if (stamp == 0)
		ent->stamp = time (0);
	ent->slp = NULL;
	ent->str_width = gtk_xtext_text_width_ent (buf->xtext, ent);
	ent->mark_start = -1;
	ent->mark_end = -1;
	ent->next = NULL;
	ent->marks = NULL;

	if (ent->indent < MARGIN)
		ent->indent = MARGIN;	  /* 2 pixels is the left margin */

	/* append to our linked list */
	if (buf->text_last)
		buf->text_last->next = ent;
	else
		buf->text_first = ent;
	ent->prev = buf->text_last;
	buf->text_last = ent;

	ent->sublines = NULL;
	buf->num_lines += gtk_xtext_lines_taken (buf, ent);

	{
		/* Phase 4.9: gtk_widget_get_toplevel and
		 * gtk_window_has_toplevel_focus are gone; use the GtkRoot path
		 * (matches the gtk_xtext_draw_marker site). */
		GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (buf->xtext));
		gboolean toplevel_active =
			(root && GTK_IS_WINDOW (root) &&
			 gtk_window_is_active (GTK_WINDOW (root)));

		if ((buf->marker_pos == NULL || buf->marker_seen)
		    && (buf->xtext->buffer != buf || !toplevel_active))
		{
			buf->marker_pos = ent;
			buf->marker_state = MARKER_IS_SET;
			dontscroll (buf); /* force scrolling off */
			buf->marker_seen = FALSE;
		}
	}

	if (buf->xtext->max_lines > 2 && buf->xtext->max_lines < buf->num_lines)
	{
		gtk_xtext_remove_top (buf);
	}

	if (buf->xtext->buffer == buf)
	{
		/* this could be improved */
		if ((buf->num_lines - 1) <= gtk_adjustment_get_page_size(buf->xtext->adj))
			dontscroll (buf);

		if (!buf->xtext->add_io_tag)
		{
			/* remove scrolling events */
			if (buf->xtext->io_tag)
			{
				g_source_remove (buf->xtext->io_tag);
				buf->xtext->io_tag = 0;
			}
			buf->xtext->add_io_tag = g_timeout_add (REFRESH_TIMEOUT * 2,
															(GSourceFunc)
															gtk_xtext_render_page_timeout,
															buf->xtext);
		}
	}
	if (buf->scrollbar_down)
	{
		buf->old_value = buf->num_lines - gtk_adjustment_get_page_size(buf->xtext->adj);
		if (buf->old_value < 0)
			buf->old_value = 0;
	}
	if (buf->search_flags & follow)
	{
		GList *gl;

		gl = gtk_xtext_search_textentry (buf, ent);
		gtk_xtext_search_textentry_add (buf, ent, gl, FALSE);
	}
}

/* Public wrapper: remove the topmost (first / oldest) entry from
 * the buffer. Internal callers use gtk_xtext_remove_top directly;
 * this thin wrapper exposes the same operation to chat.c for the
 * chat-history Load-Older render path, which needs to evict the
 * existing "── load older ──" sentinel row before prepending a
 * fresh batch + a refreshed sentinel.
 *
 * Intentionally a one-liner — duplicating the bookkeeping would
 * just be a maintenance hazard. */
void
gtk_xtext_remove_first (xtext_buffer *buf)
{
	if (buf) {
		gtk_xtext_remove_top (buf);
	}
}

/* prepend a textentry to the head of our linked list. Mirror of
 * gtk_xtext_append_entry; called via gtk_xtext_prepend_indent
 * when rendering a "Load older" chat-history batch on top of an
 * already-populated buffer. The scroll-anchoring logic mirrors
 * gtk_xtext_remove_top exactly: incrementing pagetop_line /
 * last_pixel_pos / old_value / adj_value by the new entry's
 * subline count is what keeps the user's viewport pinned to the
 * SAME content that was on-screen before the prepend.
 *
 * Deliberately omitted vs. append_entry:
 *   - marker_pos update (inserting OLD content shouldn't bump
 *     the "last seen" marker)
 *   - max_lines auto-trim (would re-evict the entry we just
 *     prepended)
 *   - scrollbar_down "snap-to-bottom" path (this entry is at
 *     the top, not the bottom — don't snap)
 *   - search rescan (chat-history older fetches aren't part of
 *     the search session)
 */
static void
gtk_xtext_prepend_entry (xtext_buffer *buf, textentry *ent, time_t stamp)
{
	int i;
	int sublines;

	/* we don't like tabs */
	i = 0;
	while (i < ent->str_len)
	{
		if (ent->str[i] == '\t')
			ent->str[i] = ' ';
		i++;
	}

	ent->stamp = stamp;
	if (stamp == 0)
		ent->stamp = time (0);
	ent->slp = NULL;
	ent->str_width = gtk_xtext_text_width_ent (buf->xtext, ent);
	ent->mark_start = -1;
	ent->mark_end = -1;
	ent->marks = NULL;

	if (ent->indent < MARGIN)
		ent->indent = MARGIN;	  /* 2 pixels is the left margin */

	/* prepend to our linked list — opposite of the append path */
	if (buf->text_first)
		buf->text_first->prev = ent;
	else
		buf->text_last = ent;
	ent->next = buf->text_first;
	ent->prev = NULL;
	buf->text_first = ent;

	ent->sublines = NULL;
	sublines = gtk_xtext_lines_taken (buf, ent);
	buf->num_lines += sublines;

	/* Scroll anchoring: every subline we just prepended pushed
	 * the previously-visible content down by one line. Bump the
	 * positional anchors by the same amount so what the user is
	 * looking at stays put. The remove_top mirror does the
	 * inverse decrement. */
	buf->pagetop_line   += sublines;
	buf->last_pixel_pos += sublines * buf->xtext->fontsize;
	buf->old_value      += sublines;

	if (buf->xtext->buffer == buf)
	{
		GtkAdjustment *adj = buf->xtext->adj;
		gtk_adjustment_set_value (adj,
		                          gtk_adjustment_get_value (adj) + sublines);
		buf->xtext->select_start_adj += sublines;
	}

	/* Schedule a re-render the same way append does. */
	if (buf->xtext->buffer == buf)
	{
		if (!buf->xtext->add_io_tag)
		{
			if (buf->xtext->io_tag)
			{
				g_source_remove (buf->xtext->io_tag);
				buf->xtext->io_tag = 0;
			}
			buf->xtext->force_render = TRUE;
			buf->xtext->add_io_tag = g_timeout_add (REFRESH_TIMEOUT * 2,
			                                       (GSourceFunc)
			                                       gtk_xtext_render_page_timeout,
			                                       buf->xtext);
		}
	}
}

/* the main two public functions */

void
gtk_xtext_append_indent (xtext_buffer *buf,
								 unsigned char *left_text, int left_len,
								 unsigned char *right_text, int right_len,
								 time_t stamp)
{
	textentry *ent;
	unsigned char *str;
	int space;
	int tempindent;
	int left_width;

	if (left_len == -1)
		left_len = strlen (left_text);

	if (right_len == -1)
		right_len = strlen (right_text);

	if (left_len + right_len + 2 >= sizeof (buf->xtext->scratch_buffer))
		right_len = sizeof (buf->xtext->scratch_buffer) - left_len - 2;

	if (right_text[right_len-1] == '\n')
		right_len--;

	ent = g_malloc (left_len + right_len + 2 + sizeof (textentry));
	str = (unsigned char *) ent + sizeof (textentry);

	if (left_len)
		memcpy (str, left_text, left_len);
	str[left_len] = ' ';
	if (right_len)
		memcpy (str + left_len + 1, right_text, right_len);
	str[left_len + 1 + right_len] = 0;

	left_width = gtk_xtext_text_width (buf->xtext, left_text, left_len);

	ent->left_len = left_len;
	ent->str = str;
	ent->str_len = left_len + 1 + right_len;
	ent->indent = (buf->indent - left_width) - buf->xtext->space_width;

	/* This is copied into the scratch buffer later, double check math */
	g_assert (ent->str_len < sizeof (buf->xtext->scratch_buffer));

	if (buf->time_stamp)
		space = buf->xtext->stamp_width;
	else
		space = 0;

	/* do we need to auto adjust the separator position? */
	if (buf->xtext->auto_indent &&
		 buf->indent < buf->xtext->max_auto_indent &&
		 ent->indent < MARGIN + space)
	{
		tempindent = MARGIN + space + buf->xtext->space_width + left_width;

		if (tempindent > buf->indent)
			buf->indent = tempindent;

		if (buf->indent > buf->xtext->max_auto_indent)
			buf->indent = buf->xtext->max_auto_indent;

		gtk_xtext_fix_indent (buf);
		gtk_xtext_recalc_widths (buf, FALSE);

		ent->indent = (buf->indent - left_width) - buf->xtext->space_width;
		buf->xtext->force_render = TRUE;
	}

	gtk_xtext_append_entry (buf, ent, stamp);
}

/* Phase 5 (chat-history extension): prepend version of
 * gtk_xtext_append_indent. Identical to its append sibling except
 * the final list-insertion call goes through gtk_xtext_prepend_entry
 * — same textentry layout, same width / indent calculation, same
 * empty-left handling for info-line rows. Callers that want to
 * insert a multi-entry batch above existing buffer content should
 * invoke this once per entry IN REVERSE ORDER (entry N first, then
 * N-1, ..., 1) so the prepended sequence ends up oldest-to-newest
 * at the top of the list. */
void
gtk_xtext_prepend_indent (xtext_buffer *buf,
                          unsigned char *left_text, int left_len,
                          unsigned char *right_text, int right_len,
                          time_t stamp)
{
	textentry *ent;
	unsigned char *str;
	int space;
	int tempindent;
	int left_width;

	if (left_len == -1)
		left_len = strlen (left_text);

	if (right_len == -1)
		right_len = strlen (right_text);

	if (left_len + right_len + 2 >= sizeof (buf->xtext->scratch_buffer))
		right_len = sizeof (buf->xtext->scratch_buffer) - left_len - 2;

	if (right_text[right_len-1] == '\n')
		right_len--;

	ent = g_malloc (left_len + right_len + 2 + sizeof (textentry));
	str = (unsigned char *) ent + sizeof (textentry);

	if (left_len)
		memcpy (str, left_text, left_len);
	str[left_len] = ' ';
	if (right_len)
		memcpy (str + left_len + 1, right_text, right_len);
	str[left_len + 1 + right_len] = 0;

	left_width = gtk_xtext_text_width (buf->xtext, left_text, left_len);

	ent->left_len = left_len;
	ent->str = str;
	ent->str_len = left_len + 1 + right_len;
	ent->indent = (buf->indent - left_width) - buf->xtext->space_width;

	g_assert (ent->str_len < sizeof (buf->xtext->scratch_buffer));

	if (buf->time_stamp)
		space = buf->xtext->stamp_width;
	else
		space = 0;

	/* Same auto-indent grow-into-larger-nick logic as
	 * gtk_xtext_append_indent. Prepended chat-history nicks can
	 * widen the indent column the same as live messages can. */
	if (buf->xtext->auto_indent &&
	    buf->indent < buf->xtext->max_auto_indent &&
	    ent->indent < MARGIN + space)
	{
		tempindent = MARGIN + space + buf->xtext->space_width + left_width;

		if (tempindent > buf->indent)
			buf->indent = tempindent;

		if (buf->indent > buf->xtext->max_auto_indent)
			buf->indent = buf->xtext->max_auto_indent;

		gtk_xtext_fix_indent (buf);
		gtk_xtext_recalc_widths (buf, FALSE);

		ent->indent = (buf->indent - left_width) - buf->xtext->space_width;
		buf->xtext->force_render = TRUE;
	}

	gtk_xtext_prepend_entry (buf, ent, stamp);
}

void
gtk_xtext_append (xtext_buffer *buf, unsigned char *text, int len, time_t stamp)
{
	textentry *ent;
	gboolean truncate = FALSE;

	if (len == -1)
		len = strlen (text);

	if (text[len-1] == '\n')
		len--;

	if (len >= sizeof (buf->xtext->scratch_buffer))
	{
		len = sizeof (buf->xtext->scratch_buffer) - 1;
		truncate = TRUE;
	}

	ent = g_malloc (len + 1 + sizeof (textentry));
	ent->str = (unsigned char *) ent + sizeof (textentry);
	ent->str_len = len;
	if (len)
	{
		if (!truncate)
		{
			memcpy (ent->str, text, len);
			ent->str[len] = '\0';
		}
		else
		{
			safe_strcpy (ent->str, text, sizeof (buf->xtext->scratch_buffer));
			ent->str_len = strlen (ent->str);
		}
	}
	/* Phase 5: respect buf->indent so the message text starts to the
	 * right of the stamp column (gtk_xtext_set_time_stamp grew indent
	 * to cover stamp_width). Without this the message draws over the
	 * stamp — append_entry bumps a 0 to MARGIN (2 px), and render_str
	 * paints message bytes starting at that position. Aligning with
	 * buf->indent matches gtk_xtext_append_indent's behaviour for the
	 * left-text-bearing append path. */
	ent->indent = buf->indent;
	ent->left_len = -1;

	gtk_xtext_append_entry (buf, ent, stamp);
}

gboolean
gtk_xtext_is_empty (xtext_buffer *buf)
{
	return buf->text_first == NULL;
}


int
gtk_xtext_lastlog (xtext_buffer *out, xtext_buffer *search_area)
{
	textentry *ent;
	int matches;
	GList *gl;

	ent = search_area->text_first;
	matches = 0;

	while (ent)
	{
		gl = gtk_xtext_search_textentry (out, ent);
		if (gl)
		{
			matches++;
			/* copy the text over */
			if (search_area->xtext->auto_indent)
			{
				gtk_xtext_append_indent (out, ent->str, ent->left_len,
												 ent->str + ent->left_len + 1,
												 ent->str_len - ent->left_len - 1, 0);
			}
			else
			{
				gtk_xtext_append (out, ent->str, ent->str_len, 0);
			}

			if (out->text_last)
			{
				out->text_last->stamp = ent->stamp;
				gtk_xtext_search_textentry_add (out, out->text_last, gl, TRUE);
			}
		}
		ent = ent->next;
	}
	out->search_found = g_list_reverse (out->search_found);

	return matches;
}

void
gtk_xtext_foreach (xtext_buffer *buf, GtkXTextForeach func, void *data)
{
	textentry *ent = buf->text_first;

	while (ent)
	{
		(*func) (buf->xtext, ent->str, data);
		ent = ent->next;
	}
}

void
gtk_xtext_set_indent (GtkXText *xtext, gboolean indent)
{
	xtext->auto_indent = indent;
}

void
gtk_xtext_set_max_indent (GtkXText *xtext, int max_auto_indent)
{
	xtext->max_auto_indent = max_auto_indent;
}

void
gtk_xtext_set_max_lines (GtkXText *xtext, int max_lines)
{
	xtext->max_lines = max_lines;
}

void
gtk_xtext_set_show_marker (GtkXText *xtext, gboolean show_marker)
{
	xtext->marker = show_marker;
}

void
gtk_xtext_set_show_separator (GtkXText *xtext, gboolean show_separator)
{
	xtext->separator = show_separator;
}

void
gtk_xtext_set_thin_separator (GtkXText *xtext, gboolean thin_separator)
{
	xtext->thinline = thin_separator;
}

/* Phase 5: gtk_xtext_set_time_stamp also grows buf->indent so the
 * message body text starts to the right of the stamp column. Without
 * this, every entry added via gtk_xtext_append gets ent->indent
 * bumped to MARGIN (2 px) in append_entry, and the message
 * overwrites the stamp at draw time — render_stamp draws, then
 * render_str fills the same area with the message body. The width
 * is rounded to a multiple of space_width via gtk_xtext_fix_indent.
 *
 * Toggling time_stamp off doesn't shrink buf->indent: collapsing
 * already-rendered entries down to a smaller indent would require
 * recomputing every ent->indent and re-laying out the text, which
 * for a long backlog costs more than just leaving a slightly wider
 * left margin. New entries appended after the toggle keep whatever
 * indent buf->indent currently has.
 */
void
gtk_xtext_set_time_stamp (xtext_buffer *buf, gboolean time_stamp)
{
	buf->time_stamp = time_stamp;
	if (time_stamp) {
		int min_indent = buf->xtext->stamp_width
		               + buf->xtext->space_width;
		if (buf->indent < min_indent) {
			buf->indent = min_indent;
			gtk_xtext_fix_indent (buf);
			gtk_xtext_recalc_widths (buf, FALSE);
		}
	}
}

void
gtk_xtext_set_urlcheck_function (GtkXText *xtext, int (*urlcheck_function) (GtkWidget *, char *))
{
	xtext->urlcheck_function = urlcheck_function;
}

void
gtk_xtext_set_wordwrap (GtkXText *xtext, gboolean wordwrap)
{
	xtext->wordwrap = wordwrap;
}

/* gtk_xtext_set_marker_last() removed in Phase 2.6 vendoring: it took a
 * `session *` and reached into hexchat's per-session widget cache to
 * find the buffer.  GtkHx has no equivalent struct.  If a marker-line
 * UI is wanted later, expose a per-buffer setter on `xtext_buffer *`. */

void
gtk_xtext_reset_marker_pos (GtkXText *xtext)
{
	if (xtext->buffer->marker_pos)
	{
		xtext->buffer->marker_pos = NULL;
		dontscroll (xtext->buffer); /* force scrolling off */
		gtk_xtext_render_page (xtext);
		xtext->buffer->marker_state = MARKER_RESET_MANUALLY;
	}
}

int
gtk_xtext_moveto_marker_pos (GtkXText *xtext)
{
	gdouble value = 0;
	xtext_buffer *buf = xtext->buffer;
	textentry *ent = buf->text_first;
	GtkAdjustment *adj = xtext->adj;

	if (buf->marker_pos == NULL)
		return buf->marker_state;

	if (gtk_xtext_check_ent_visibility (xtext, buf->marker_pos, 1) == FALSE)
	{
		while (ent)
		{
			if (ent == buf->marker_pos)
				break;
			value += g_slist_length (ent->sublines);
			ent = ent->next;
		}
		if (value >= gtk_adjustment_get_value(adj) && value < gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj))
			return MARKER_IS_SET;
		value -= gtk_adjustment_get_page_size(adj) / 2;
		if (value < 0)
			value = 0;
		if (value > gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj))
			value = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj);
		gtk_adjustment_set_value (adj, value);
		gtk_xtext_render_page (xtext);
	}

	/* If we previously lost marker position to scrollback limit -- */
	if (buf->marker_pos == buf->text_first &&
		 buf->marker_state == MARKER_RESET_BY_KILL)
		return MARKER_RESET_BY_KILL;
	else
		return MARKER_IS_SET;
}

void
gtk_xtext_buffer_show (GtkXText *xtext, xtext_buffer *buf, int render)
{
	int w, h;

	buf->xtext = xtext;

	if (xtext->buffer == buf)
		return;

/*printf("text_buffer_show: xtext=%p buffer=%p\n", xtext, buf);*/

	if (xtext->add_io_tag)
	{
		g_source_remove (xtext->add_io_tag);
		xtext->add_io_tag = 0;
	}

	if (xtext->io_tag)
	{
		g_source_remove (xtext->io_tag);
		xtext->io_tag = 0;
	}

	if (!gtk_widget_get_realized (GTK_WIDGET (xtext)))
		gtk_widget_realize (GTK_WIDGET (xtext));

	/* Phase 4.9: see render_page note. */
	h = gtk_widget_get_height (GTK_WIDGET (xtext));
	w = gtk_widget_get_width  (GTK_WIDGET (xtext));

	/* after a font change */
	if (buf->needs_recalc)
	{
		buf->needs_recalc = FALSE;
		gtk_xtext_recalc_widths (buf, TRUE);
	}

	/* now change to the new buffer */
	xtext->buffer = buf;
	dontscroll (buf);	/* force scrolling off */
	gtk_adjustment_set_value(xtext->adj, buf->old_value);
	gtk_adjustment_set_upper(xtext->adj, buf->num_lines);

	/* if the scrollbar was down, keep it down */
	if (xtext->buffer->scrollbar_down && gtk_adjustment_get_value(xtext->adj) <
		gtk_adjustment_get_upper(xtext->adj) - gtk_adjustment_get_page_size(xtext->adj))
	{
		gtk_adjustment_set_value(xtext->adj, gtk_adjustment_get_upper(xtext->adj) - gtk_adjustment_get_page_size(xtext->adj));
	}

	if (gtk_adjustment_get_upper(xtext->adj) == 0)
		gtk_adjustment_set_upper(xtext->adj, 1);
	/* sanity check */
	else if (gtk_adjustment_get_value(xtext->adj) > gtk_adjustment_get_upper(xtext->adj) - gtk_adjustment_get_page_size(xtext->adj))
	{
		/*buf->pagetop_ent = NULL;*/
		gtk_adjustment_set_value(xtext->adj, gtk_adjustment_get_upper(xtext->adj) - gtk_adjustment_get_page_size(xtext->adj));
		if (gtk_adjustment_get_value(xtext->adj) < 0)
			gtk_adjustment_set_value(xtext->adj, 0);
	}

	if (render)
	{
		/* did the window change size since this buffer was last shown? */
		if (buf->window_width != w)
		{
			buf->window_width = w;
			buf->window_height = h;
			gtk_xtext_calc_lines (buf, FALSE);
			if (buf->scrollbar_down)
				gtk_adjustment_set_value (xtext->adj, gtk_adjustment_get_upper(xtext->adj) -
												  gtk_adjustment_get_page_size(xtext->adj));
		} else if (buf->window_height != h)
		{
			buf->window_height = h;
			buf->pagetop_ent = NULL;
			if (buf->scrollbar_down)
				gtk_adjustment_set_value(xtext->adj, gtk_adjustment_get_upper(xtext->adj));
			gtk_xtext_adjustment_set (buf, FALSE);
		}

		gtk_xtext_render_page (xtext);
	}
}

xtext_buffer *
gtk_xtext_buffer_new (GtkXText *xtext)
{
	xtext_buffer *buf;

	buf = g_new0 (xtext_buffer, 1);
	buf->old_value = -1;
	buf->xtext = xtext;
	buf->scrollbar_down = TRUE;
	buf->indent = xtext->space_width * 2;
	dontscroll (buf);

	return buf;
}

void
gtk_xtext_buffer_free (xtext_buffer *buf)
{
	textentry *ent, *next;

	if (buf->xtext->buffer == buf)
		buf->xtext->buffer = buf->xtext->orig_buffer;

	if (buf->xtext->selection_buffer == buf)
		buf->xtext->selection_buffer = NULL;

	if (buf->search_found)
	{
		gtk_xtext_search_fini (buf);
	}

	ent = buf->text_first;
	while (ent)
	{
		next = ent->next;
		g_free (ent);
		ent = next;
	}

	g_free (buf);
}
