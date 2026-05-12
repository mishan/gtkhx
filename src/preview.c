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
 */

/*
 * Preview window with a type-aware viewer dispatcher.
 *
 * The HTXF worker streams the file's data fork through three entry
 * points:
 *
 *   hx_preview_set_info  — once, with type/creator from FILP header
 *   hx_preview_chunk     — per data chunk
 *   hx_preview_done      — once, at end of stream
 *
 * All three are safe to call from the worker; the implementation
 * marshals to the main thread via g_idle_add and per-call payload
 * structs (window/widget mutation only happens on main).
 *
 * The viewer registry below picks a per-format renderer:
 *
 *   text_viewer  — GtkTextView; catch-all fallback (score = 1)
 *   image_viewer — GdkPixbufLoader → GtkPicture; (score ~= 10 on
 *                  image type/creator codes or known extensions)
 *
 * Adding a new viewer: implement the four hx_viewer entry points,
 * append a pointer to the viewers[] table. Higher scores win on
 * dispatch; ties go to the earlier table entry.
 *
 * Lifecycle on the worker→main marshal: each posted job holds a
 * reference to the hx_preview. The user closing the window flips
 * p->closed = TRUE on main; queued jobs check the flag before
 * touching widgets, so a close mid-stream just drops pending
 * chunks instead of running into a freed widget.
 */

#include "config.h"
#include <string.h>
#include <gtk/gtk.h>
#include <adwaita.h>
#ifdef HAVE_POPPLER
#include <poppler.h>
#endif
#ifdef HAVE_GTKSOURCEVIEW
#include <gtksourceview/gtksource.h>
#endif
#include "hx.h"
#include "preview.h"
#include "gtkutil.h"

/* ---- Viewer protocol ----------------------------------------------- */

struct hx_viewer {
	const char *name;

	/* Score (0..N) — higher wins. 0 means "doesn't claim this
	 * file". The text viewer returns 1 unconditionally so it's
	 * always the fallback. */
	int  (*score) (const char *type, const char *creator,
	               const char *filename);

	/* Build the body widget; stash viewer-private state on
	 * p->viewer_data. Runs on main; called from
	 * preview_install_viewer once dispatch has been made. */
	GtkWidget *(*create) (hx_preview *p);

	/* Append a chunk to the in-flight render. Runs on main. */
	void (*chunk) (hx_preview *p, const char *buf, gsize len);

	/* End of stream — viewer commits pending state. Runs on main. */
	void (*done) (hx_preview *p);

	/* Window close-request — viewer frees private state. Runs on
	 * main. The window itself is destroyed by the caller. */
	void (*close) (hx_preview *p);
};

/* ---- hx_preview definition ---------------------------------------- */

struct hx_preview {
	char  *name;
	char   type[5];
	char   creator[5];

	GtkWidget *window;
	GtkWidget *body;          /* the swappable child of the window */
	GtkWidget *placeholder;   /* initial "Loading…" stand-in */

	const struct hx_viewer *viewer;   /* NULL until dispatch */
	void  *viewer_data;

	/* Worker pre-dispatch queue: chunks that arrive before
	 * set_info is processed sit here so the viewer can replay
	 * them. Bounded by the FILP header size on the worker side
	 * (set_info comes before the first data chunk in practice),
	 * so this is small in the common case. */
	GByteArray *pre_queue;

	/* Set when the user closes the window. Both the worker's
	 * marshal helpers and queued idle callbacks bail before
	 * touching widgets if this is TRUE. */
	gboolean closed;

	/* Refcount for the worker→main marshal payloads. The window
	 * holds one ref, each queued job holds one. Last unref frees
	 * the hx_preview. */
	gint refcount;
};

static hx_preview *hx_preview_ref   (hx_preview *p);
static void        hx_preview_unref (hx_preview *p);

/* ---- Text viewer --------------------------------------------------- */

struct text_state {
	GtkWidget *text_view;
};

static int
text_score (const char *type, const char *creator, const char *filename)
{
	(void) type; (void) creator; (void) filename;
	/* Catch-all: any file we can't otherwise classify shows up as
	 * text. Lowest non-zero score so any real match wins. */
	return 1;
}

static GtkWidget *
text_create (hx_preview *p)
{
	struct text_state *s;
	GtkWidget *scroll, *text;

	s = g_new0 (struct text_state, 1);
	text = gtk_text_view_new ();
	gtk_text_view_set_editable      (GTK_TEXT_VIEW (text), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW (text), FALSE);
	gtk_text_view_set_monospace     (GTK_TEXT_VIEW (text), TRUE);
	gtk_text_view_set_wrap_mode     (GTK_TEXT_VIEW (text),
	                                  GTK_WRAP_WORD_CHAR);

	scroll = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
	                                GTK_POLICY_AUTOMATIC,
	                                GTK_POLICY_AUTOMATIC);
	gtkhx_widget_set_child (scroll, text);

	s->text_view = text;
	p->viewer_data = s;
	return scroll;
}

static void
text_chunk (hx_preview *p, const char *buf, gsize len)
{
	struct text_state *s = p->viewer_data;
	GtkTextBuffer *tbuf;
	GtkTextIter end;
	char *valid;
	char *fixed;

	if (!s || len == 0)
		return;

	/* CR→LF: Mac-style line endings show up as one big line otherwise.
	 * Operating on a heap copy because the caller's buffer is shared
	 * with the queue infrastructure. */
	fixed = g_memdup2 (buf, len);
	{
		gsize i;
		for (i = 0; i < len; i++)
			if (fixed[i] == '\r')
				fixed[i] = '\n';
	}

	/* g_utf8_make_valid replaces invalid sequences with U+FFFD so
	 * any text-ish file stays renderable. Binary files get noisy
	 * but at least don't blow up Pango. */
	valid = g_utf8_make_valid (fixed, len);
	tbuf  = gtk_text_view_get_buffer (GTK_TEXT_VIEW (s->text_view));
	gtk_text_buffer_get_end_iter (tbuf, &end);
	gtk_text_buffer_insert (tbuf, &end, valid, -1);
	g_free (valid);
	g_free (fixed);
}

static void
text_done (hx_preview *p)
{
	(void) p;
}

static void
text_close (hx_preview *p)
{
	g_clear_pointer (&p->viewer_data, g_free);
}

static const struct hx_viewer text_viewer = {
	.name   = "text",
	.score  = text_score,
	.create = text_create,
	.chunk  = text_chunk,
	.done   = text_done,
	.close  = text_close,
};

/* ---- Image viewer (GdkPixbufLoader → GtkPicture) ------------------- */

/* Mac creator+type codes for image formats commonly served via
 * Hotline. Pre-OS X TextEdit / Preview / SimpleText era — these
 * are the legacy 4-byte FourCC strings the FILP wrapper carries.
 * Modern servers may omit them (the type field reads as four
 * spaces); the filename extension below covers that case. */
static const char *const image_type_codes[] = {
	"JPEG",  /* JPEG image */
	"GIFf",  /* GIF image */
	"PNGf",  /* PNG image */
	"PNG ",  /* PNG (alt) */
	"BMP ",  /* BMP */
	"BMPp",  /* BMP (alt) */
	"WBMP",  /* WebP — sometimes seen */
	"TIFF",  /* TIFF */
	"PICT",  /* QuickDraw PICT — GdkPixbuf doesn't render these
	          * natively, but we let the loader try; it'll fall
	          * back to the text path if the loader errors out. */
	NULL,
};

static const char *const image_extensions[] = {
	".jpg", ".jpeg", ".jpe",
	".png",
	".gif",
	".bmp",
	".webp",
	".tif", ".tiff",
	".ico",
	".avif",
	".svg",
	NULL,
};

static gboolean
str_in (const char *needle, const char *const *haystack)
{
	while (*haystack) {
		if (g_strcmp0 (*haystack++, needle) == 0)
			return TRUE;
	}
	return FALSE;
}

static int
image_score (const char *type, const char *creator, const char *filename)
{
	char ext_match[16];

	(void) creator;

	if (type && str_in (type, image_type_codes))
		return 20;

	/* Match extensions case-insensitively. g_str_has_suffix is
	 * case-sensitive; lowercase the comparison instead. */
	if (filename) {
		const char *dot = strrchr (filename, '.');
		if (dot && strlen (dot) < sizeof (ext_match)) {
			gsize i;
			for (i = 0; dot[i] && i < sizeof (ext_match) - 1; i++)
				ext_match[i] = g_ascii_tolower (dot[i]);
			ext_match[i] = 0;
			if (str_in (ext_match, image_extensions))
				return 15;
		}
	}
	return 0;
}

/* The image viewer accumulates the file's bytes into a GByteArray
 * and decodes once at end-of-stream via gdk_texture_new_from_bytes.
 *
 * We could decode progressively with GdkPixbufLoader but the
 * pixbuf→GdkPaintable handoff used to go through gdk_texture_new_
 * for_pixbuf, which is deprecated in modern GTK 4. The from-bytes
 * texture API is the supported modern path, and it doesn't accept
 * a streaming feed — it wants the whole image as one GBytes. For
 * the preview use case (single image, kept fully in memory anyway
 * for the texture), this is fine and quite a bit simpler. */
struct image_state {
	GtkWidget   *picture;
	GtkWidget   *status;     /* caption shown while loading / on error */
	GByteArray  *buf;
};

static GtkWidget *
image_create (hx_preview *p)
{
	struct image_state *s;
	GtkWidget *box, *scroll;

	s = g_new0 (struct image_state, 1);
	s->buf = g_byte_array_new ();

	box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

	s->status = gtk_label_new ("Loading image…");
	gtk_widget_add_css_class (s->status, "dim-label");
	gtk_widget_set_margin_top    (s->status, 12);
	gtk_widget_set_margin_bottom (s->status, 12);
	gtk_box_append (GTK_BOX (box), s->status);

	s->picture = gtk_picture_new ();
	gtk_widget_set_vexpand    (s->picture, TRUE);
	gtk_widget_set_hexpand    (s->picture, TRUE);
	gtk_picture_set_can_shrink (GTK_PICTURE (s->picture), TRUE);
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	gtk_picture_set_keep_aspect_ratio (GTK_PICTURE (s->picture), TRUE);
	G_GNUC_END_IGNORE_DEPRECATIONS
	gtk_widget_set_visible (s->picture, FALSE);

	scroll = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
	                                GTK_POLICY_AUTOMATIC,
	                                GTK_POLICY_AUTOMATIC);
	gtkhx_widget_set_child (scroll, s->picture);
	gtk_widget_set_vexpand (scroll, TRUE);
	gtk_box_append (GTK_BOX (box), scroll);

	p->viewer_data = s;
	return box;
}

static void
image_chunk (hx_preview *p, const char *buf, gsize len)
{
	struct image_state *s = p->viewer_data;
	if (!s || !s->buf || len == 0)
		return;
	g_byte_array_append (s->buf, (const guint8 *) buf, len);
}

static void
image_done (hx_preview *p)
{
	struct image_state *s = p->viewer_data;
	GError *err = NULL;
	GdkTexture *tex;
	GBytes *bytes;

	if (!s || !s->buf)
		return;

	if (s->buf->len == 0) {
		gtk_label_set_text (GTK_LABEL (s->status), "No image data");
		return;
	}

	bytes = g_byte_array_free_to_bytes (s->buf);
	s->buf = NULL;

	tex = gdk_texture_new_from_bytes (bytes, &err);
	g_bytes_unref (bytes);

	if (!tex) {
		gtk_label_set_text (
			GTK_LABEL (s->status),
			err ? err->message
			    : "Failed to decode image");
		g_clear_error (&err);
		return;
	}

	gtk_picture_set_paintable (GTK_PICTURE (s->picture),
	                            GDK_PAINTABLE (tex));
	g_object_unref (tex);
	gtk_widget_set_visible (s->picture, TRUE);
	gtk_widget_set_visible (s->status,  FALSE);
}

static void
image_close (hx_preview *p)
{
	struct image_state *s = p->viewer_data;
	if (!s)
		return;
	if (s->buf)
		g_byte_array_unref (s->buf);
	g_free (s);
	p->viewer_data = NULL;
}

static const struct hx_viewer image_viewer = {
	.name   = "image",
	.score  = image_score,
	.create = image_create,
	.chunk  = image_chunk,
	.done   = image_done,
	.close  = image_close,
};

/* ---- PDF viewer (Poppler → GtkDrawingArea per page) ---------------- */

#ifdef HAVE_POPPLER

/* Cap how many pages we render up-front. Hotline PDFs in the wild
 * tend to be short (manuals, zines, scans), so 20 covers the common
 * case without spending forever rendering a thesis. */
#define PDF_PAGE_CAP   20

/* Render at 1.0× nominal DPI. Higher (1.5×, 2×) is sharper on hi-DPI
 * displays but quadruples the cairo surface memory; the GtkPicture
 * scaling does a passable job upsampling. */
#define PDF_RENDER_SCALE 1.0

struct pdf_state {
	GtkWidget    *body;        /* outer GtkScrolledWindow */
	GtkWidget    *page_box;    /* GtkBox holding the per-page widgets */
	GtkWidget    *status;      /* "Loading…" / error label */
	GByteArray   *buf;
	PopplerDocument *doc;
};

static int
pdf_score (const char *type, const char *creator, const char *filename)
{
	(void) creator;
	if (type && (g_strcmp0 (type, "PDF ") == 0 ||
	             g_strcmp0 (type, "PDF") == 0))
		return 20;
	if (filename && g_str_has_suffix (filename, ".pdf"))
		return 15;
	if (filename && g_str_has_suffix (filename, ".PDF"))
		return 15;
	return 0;
}

/* Per-page draw callback. The PopplerPage is held by g_object_set_data
 * on the GtkDrawingArea so it stays alive as long as the area does;
 * the doc owns the page. */
static void
pdf_draw_page (GtkDrawingArea *area, cairo_t *cr,
               int width, int height, gpointer user_data)
{
	PopplerPage *page = g_object_get_data (G_OBJECT (area), "pdf-page");
	double pw, ph, scale;

	(void) user_data;
	if (!page)
		return;

	poppler_page_get_size (page, &pw, &ph);
	if (pw <= 0 || ph <= 0)
		return;

	/* White paper background — most PDFs assume a white canvas
	 * and look broken without it on dark themes. */
	cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
	cairo_paint (cr);

	/* Fit-width scaling: stretch to the widget's current width and
	 * letterbox the height. The drawing area's content-height
	 * sizing (set after page load) keeps the proportion right. */
	scale = (double) width / pw;
	(void) height;
	cairo_scale (cr, scale, scale);
	poppler_page_render (page, cr);
}

static GtkWidget *
pdf_create (hx_preview *p)
{
	struct pdf_state *s;
	GtkWidget *scroll;

	s = g_new0 (struct pdf_state, 1);
	s->buf = g_byte_array_new ();

	scroll = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
	                                GTK_POLICY_AUTOMATIC,
	                                GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand (scroll, TRUE);
	gtk_widget_set_hexpand (scroll, TRUE);

	s->page_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_top    (s->page_box, 12);
	gtk_widget_set_margin_bottom (s->page_box, 12);
	gtk_widget_set_margin_start  (s->page_box, 12);
	gtk_widget_set_margin_end    (s->page_box, 12);

	s->status = gtk_label_new ("Loading PDF…");
	gtk_widget_add_css_class (s->status, "dim-label");
	gtk_widget_set_margin_top    (s->status, 12);
	gtk_widget_set_margin_bottom (s->status, 12);
	gtk_box_append (GTK_BOX (s->page_box), s->status);

	gtkhx_widget_set_child (scroll, s->page_box);
	s->body = scroll;
	p->viewer_data = s;
	return scroll;
}

static void
pdf_chunk (hx_preview *p, const char *buf, gsize len)
{
	struct pdf_state *s = p->viewer_data;
	if (!s || !s->buf || len == 0)
		return;
	g_byte_array_append (s->buf, (const guint8 *) buf, len);
}

static void
pdf_done (hx_preview *p)
{
	struct pdf_state *s = p->viewer_data;
	GError *err = NULL;
	GBytes *bytes;
	int n_pages, i;

	if (!s || !s->buf)
		return;
	if (s->buf->len == 0) {
		gtk_label_set_text (GTK_LABEL (s->status), "No PDF data");
		return;
	}

	bytes = g_byte_array_free_to_bytes (s->buf);
	s->buf = NULL;

	s->doc = poppler_document_new_from_bytes (bytes, NULL, &err);
	g_bytes_unref (bytes);

	if (!s->doc) {
		gtk_label_set_text (
			GTK_LABEL (s->status),
			err ? err->message
			    : "Failed to decode PDF");
		g_clear_error (&err);
		return;
	}

	/* Hide the loading label; replace with rendered pages. */
	gtk_widget_set_visible (s->status, FALSE);

	n_pages = poppler_document_get_n_pages (s->doc);
	for (i = 0; i < n_pages && i < PDF_PAGE_CAP; i++) {
		PopplerPage *page = poppler_document_get_page (s->doc, i);
		GtkWidget *area;
		double pw, ph;
		int content_h;

		if (!page)
			continue;
		poppler_page_get_size (page, &pw, &ph);
		if (pw <= 0 || ph <= 0) {
			g_object_unref (page);
			continue;
		}

		area = gtk_drawing_area_new ();
		/* Set a fixed content width (the natural page width at
		 * PDF_RENDER_SCALE) and a matching content height — the
		 * draw callback scales to whatever width the layout
		 * ends up giving us, but content-height keeps the
		 * vertical proportion correct as the window resizes. */
		gtk_drawing_area_set_content_width  (
			GTK_DRAWING_AREA (area),
			(int) (pw * PDF_RENDER_SCALE));
		content_h = (int) (ph * PDF_RENDER_SCALE);
		gtk_drawing_area_set_content_height (
			GTK_DRAWING_AREA (area), content_h);
		gtk_widget_set_hexpand (area, TRUE);

		/* Stash the page on the widget for the draw callback.
		 * g_object_unref runs at widget destroy time, which
		 * happens before the PopplerDocument is unref'd in
		 * pdf_close, so the page outlives any redraw. */
		g_object_set_data_full (G_OBJECT (area), "pdf-page",
		                        page, g_object_unref);

		gtk_drawing_area_set_draw_func (
			GTK_DRAWING_AREA (area),
			pdf_draw_page, NULL, NULL);

		gtk_box_append (GTK_BOX (s->page_box), area);
	}

	if (n_pages > PDF_PAGE_CAP) {
		GtkWidget *more = gtk_label_new (NULL);
		char *txt = g_strdup_printf (
			"… %d more page%s — save to view fully",
			n_pages - PDF_PAGE_CAP,
			(n_pages - PDF_PAGE_CAP) == 1 ? "" : "s");
		gtk_label_set_text (GTK_LABEL (more), txt);
		g_free (txt);
		gtk_widget_add_css_class (more, "dim-label");
		gtk_widget_set_margin_top    (more, 12);
		gtk_widget_set_margin_bottom (more, 12);
		gtk_box_append (GTK_BOX (s->page_box), more);
	}
}

static void
pdf_close (hx_preview *p)
{
	struct pdf_state *s = p->viewer_data;
	if (!s)
		return;
	if (s->buf)
		g_byte_array_unref (s->buf);
	g_clear_object (&s->doc);
	g_free (s);
	p->viewer_data = NULL;
}

static const struct hx_viewer pdf_viewer = {
	.name   = "pdf",
	.score  = pdf_score,
	.create = pdf_create,
	.chunk  = pdf_chunk,
	.done   = pdf_done,
	.close  = pdf_close,
};

#endif /* HAVE_POPPLER */

/* ---- Source-code / Markdown viewer (GtkSourceView) ----------------- */

#ifdef HAVE_GTKSOURCEVIEW

/* The list of extensions that bumps source_score to "definitely
 * code, render with syntax highlighting." We don't enumerate every
 * language GtkSourceView knows about; just the common-on-Hotline
 * ones. README files (no extension) and weird-extensioned scripts
 * fall through to the text viewer, which is fine. */
static const char *const source_extensions[] = {
	".md", ".markdown",
	".c", ".h", ".cpp", ".hpp", ".cc", ".cxx",
	".py", ".pyw",
	".js", ".ts", ".jsx", ".tsx",
	".rs",
	".go",
	".java", ".kt",
	".rb",
	".pl", ".pm",
	".php",
	".sh", ".bash", ".zsh",
	".sql",
	".json", ".yaml", ".yml", ".toml",
	".xml", ".html", ".htm", ".css",
	".lua",
	".swift",
	".scala",
	".ex", ".exs",
	".hs",
	".diff", ".patch",
	".lisp", ".scm", ".el",
	".m",       /* Objective-C */
	NULL,
};

static int
source_score (const char *type, const char *creator, const char *filename)
{
	const char *dot;
	char ext_match[16];
	gsize i;

	(void) type; (void) creator;
	if (!filename)
		return 0;
	dot = strrchr (filename, '.');
	if (!dot || strlen (dot) >= sizeof (ext_match))
		return 0;
	for (i = 0; dot[i]; i++)
		ext_match[i] = g_ascii_tolower (dot[i]);
	ext_match[i] = 0;

	for (i = 0; source_extensions[i]; i++) {
		if (g_strcmp0 (ext_match, source_extensions[i]) == 0) {
			/* Beat the text fallback but lose to image / PDF
			 * matches (which score 15+ on extension). */
			return 10;
		}
	}
	return 0;
}

struct source_state {
	GtkWidget        *view;
	GtkSourceBuffer  *buf;
};

static GtkWidget *
source_create (hx_preview *p)
{
	struct source_state *s;
	GtkWidget *scroll;
	GtkSourceLanguageManager *langs;
	GtkSourceLanguage        *lang;
	GtkSourceStyleSchemeManager *schemes;
	GtkSourceStyleScheme     *scheme;
	gboolean dark;

	s = g_new0 (struct source_state, 1);
	s->buf  = gtk_source_buffer_new (NULL);
	s->view = gtk_source_view_new_with_buffer (s->buf);

	gtk_source_view_set_show_line_numbers (
		GTK_SOURCE_VIEW (s->view), TRUE);
	gtk_source_view_set_highlight_current_line (
		GTK_SOURCE_VIEW (s->view), TRUE);
	gtk_text_view_set_editable      (GTK_TEXT_VIEW (s->view), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW (s->view), FALSE);
	gtk_text_view_set_monospace     (GTK_TEXT_VIEW (s->view), TRUE);
	gtk_text_view_set_wrap_mode     (GTK_TEXT_VIEW (s->view),
	                                  GTK_WRAP_WORD_CHAR);

	/* Pick a syntax based on the filename. Returns NULL for
	 * extensions we listed but GtkSourceView doesn't know about;
	 * the buffer just stays unhighlighted in that case. */
	langs = gtk_source_language_manager_get_default ();
	lang  = gtk_source_language_manager_guess_language (langs,
	                                                    p->name, NULL);
	if (lang)
		gtk_source_buffer_set_language (s->buf, lang);

	/* Track the libadwaita color scheme: classic for light,
	 * classic-dark for dark. Both ship with GtkSourceView. */
	schemes = gtk_source_style_scheme_manager_get_default ();
	dark = adw_style_manager_get_dark (
		adw_style_manager_get_default ());
	scheme = gtk_source_style_scheme_manager_get_scheme (
		schemes, dark ? "classic-dark" : "classic");
	if (scheme)
		gtk_source_buffer_set_style_scheme (s->buf, scheme);

	scroll = gtk_scrolled_window_new ();
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
	                                GTK_POLICY_AUTOMATIC,
	                                GTK_POLICY_AUTOMATIC);
	gtkhx_widget_set_child (scroll, s->view);

	p->viewer_data = s;
	return scroll;
}

static void
source_chunk (hx_preview *p, const char *buf, gsize len)
{
	struct source_state *s = p->viewer_data;
	GtkTextIter end;
	char *valid;
	char *fixed;
	gsize i;

	if (!s || !s->buf || len == 0)
		return;

	/* Same CR→LF + UTF-8 fix-up as the text viewer — Hotline files
	 * often arrive with classic-Mac line endings. */
	fixed = g_memdup2 (buf, len);
	for (i = 0; i < len; i++)
		if (fixed[i] == '\r')
			fixed[i] = '\n';
	valid = g_utf8_make_valid (fixed, len);

	gtk_text_buffer_get_end_iter (GTK_TEXT_BUFFER (s->buf), &end);
	gtk_text_buffer_insert (GTK_TEXT_BUFFER (s->buf), &end, valid, -1);

	g_free (valid);
	g_free (fixed);
}

static void
source_done (hx_preview *p)
{
	(void) p;
}

static void
source_close (hx_preview *p)
{
	struct source_state *s = p->viewer_data;
	if (!s)
		return;
	g_clear_object (&s->buf);
	g_free (s);
	p->viewer_data = NULL;
}

static const struct hx_viewer source_viewer = {
	.name   = "source",
	.score  = source_score,
	.create = source_create,
	.chunk  = source_chunk,
	.done   = source_done,
	.close  = source_close,
};

#endif /* HAVE_GTKSOURCEVIEW */

/* ---- Viewer registry ---------------------------------------------- */

static const struct hx_viewer *const viewers[] = {
#ifdef HAVE_POPPLER
	&pdf_viewer,
#endif
	&image_viewer,
#ifdef HAVE_GTKSOURCEVIEW
	&source_viewer,
#endif
	&text_viewer,         /* last: catch-all */
};

static const struct hx_viewer *
pick_viewer (const char *type, const char *creator, const char *filename)
{
	const struct hx_viewer *best = NULL;
	int best_score = 0;
	gsize i;

	for (i = 0; i < G_N_ELEMENTS (viewers); i++) {
		int s = viewers[i]->score (type, creator, filename);
		if (s > best_score) {
			best_score = s;
			best = viewers[i];
		}
	}
	return best ? best : &text_viewer;
}

/* ---- hx_preview lifecycle / dispatcher ---------------------------- */

static hx_preview *
hx_preview_ref (hx_preview *p)
{
	g_atomic_int_inc (&p->refcount);
	return p;
}

static void
hx_preview_unref (hx_preview *p)
{
	if (!g_atomic_int_dec_and_test (&p->refcount))
		return;
	g_free (p->name);
	if (p->pre_queue)
		g_byte_array_unref (p->pre_queue);
	g_free (p);
}

static gboolean
preview_close_request (GtkWindow *window, gpointer user_data)
{
	hx_preview *p = user_data;
	(void) window;
	if (!p)
		return FALSE;
	p->closed = TRUE;
	if (p->viewer && p->viewer->close)
		p->viewer->close (p);
	/* Drop the window's ref. Worker-queued jobs hold their own
	 * refs and keep the struct alive until they drain. */
	hx_preview_unref (p);
	return FALSE;     /* let default destroy proceed */
}

/* Install the chosen viewer's body widget into the window. Called
 * on main once we have the file type info. Replays any queued
 * pre-dispatch chunks. */
static void
preview_install_viewer (hx_preview *p)
{
	const struct hx_viewer *v;
	GtkWidget *body;

	if (p->closed || p->viewer)
		return;

	v = pick_viewer (p->type, p->creator, p->name);
	body = v->create (p);
	p->viewer = v;

	/* gtkhx_widget_set_child unparents the previous child (the
	 * placeholder, on the common path), so the placeholder
	 * pointer is stale immediately after this call. */
	gtkhx_widget_set_child (p->window, body);
	p->placeholder = NULL;
	p->body = body;

	/* Replay any chunks that arrived before set_info was
	 * processed. Common case: an empty queue (set_info almost
	 * always lands first because the worker calls it before
	 * preview_get even reads the data fork). */
	if (p->pre_queue && p->pre_queue->len > 0 && v->chunk) {
		v->chunk (p, (const char *) p->pre_queue->data,
		           p->pre_queue->len);
	}
	if (p->pre_queue) {
		g_byte_array_unref (p->pre_queue);
		p->pre_queue = NULL;
	}
}

/* ---- Worker → main marshal helpers -------------------------------- */

struct set_info_job {
	hx_preview *p;
	char        type[5];
	char        creator[5];
};

static gboolean
set_info_dispatch (gpointer data)
{
	struct set_info_job *j = data;
	if (!j->p->closed) {
		memcpy (j->p->type,    j->type,    sizeof (j->p->type));
		memcpy (j->p->creator, j->creator, sizeof (j->p->creator));
		preview_install_viewer (j->p);
	}
	hx_preview_unref (j->p);
	g_free (j);
	return G_SOURCE_REMOVE;
}

struct chunk_job {
	hx_preview *p;
	char  *data;
	gsize  len;
};

static gboolean
chunk_dispatch (gpointer data)
{
	struct chunk_job *j = data;
	if (!j->p->closed) {
		if (j->p->viewer && j->p->viewer->chunk) {
			j->p->viewer->chunk (j->p, j->data, j->len);
		} else {
			/* Viewer not installed yet — stash the bytes so
			 * preview_install_viewer can replay them. */
			if (!j->p->pre_queue)
				j->p->pre_queue = g_byte_array_new ();
			g_byte_array_append (j->p->pre_queue,
			                      (const guint8 *) j->data,
			                      j->len);
		}
	}
	g_free (j->data);
	hx_preview_unref (j->p);
	g_free (j);
	return G_SOURCE_REMOVE;
}

static gboolean
done_dispatch (gpointer data)
{
	hx_preview *p = data;
	if (!p->closed && p->viewer && p->viewer->done)
		p->viewer->done (p);
	hx_preview_unref (p);
	return G_SOURCE_REMOVE;
}

/* ---- Public API --------------------------------------------------- */

hx_preview *
hx_preview_new (const char *name)
{
	hx_preview *p;
	GtkWidget *placeholder;

	p = g_new0 (hx_preview, 1);
	p->refcount = 1;          /* held by the window */
	p->name = g_strdup (name ? name : "");

	p->window = gtk_window_new ();
	gtk_window_set_title    (GTK_WINDOW (p->window), p->name);
	gtk_window_set_titlebar (GTK_WINDOW (p->window),
	                          adw_header_bar_new ());
	gtk_window_set_default_size (GTK_WINDOW (p->window), 480, 360);

	placeholder = gtk_label_new ("Loading…");
	gtk_widget_add_css_class (placeholder, "dim-label");
	gtk_widget_set_valign (placeholder, GTK_ALIGN_CENTER);
	gtk_widget_set_halign (placeholder, GTK_ALIGN_CENTER);
	gtk_widget_set_vexpand (placeholder, TRUE);
	gtk_widget_set_hexpand (placeholder, TRUE);
	gtkhx_widget_set_child (p->window, placeholder);
	p->placeholder = placeholder;

	g_signal_connect (p->window, "close-request",
	                  G_CALLBACK (preview_close_request), p);

	gtk_window_present (GTK_WINDOW (p->window));
	return p;
}

void
hx_preview_set_info (hx_preview *p,
                     const char *type, const char *creator)
{
	struct set_info_job *j;

	if (!p)
		return;
	j = g_new0 (struct set_info_job, 1);
	j->p = hx_preview_ref (p);
	if (type)
		memcpy (j->type,    type,    MIN (strlen (type), 4u));
	if (creator)
		memcpy (j->creator, creator, MIN (strlen (creator), 4u));
	g_idle_add (set_info_dispatch, j);
}

void
hx_preview_chunk (hx_preview *p, const char *buf, gsize len)
{
	struct chunk_job *j;

	if (!p || !buf || len == 0)
		return;
	j = g_new0 (struct chunk_job, 1);
	j->p    = hx_preview_ref (p);
	j->data = g_memdup2 (buf, len);
	j->len  = len;
	g_idle_add (chunk_dispatch, j);
}

void
hx_preview_done (hx_preview *p)
{
	if (!p)
		return;
	hx_preview_ref (p);
	g_idle_add (done_dispatch, p);
}
