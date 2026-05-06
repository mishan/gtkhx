#ifndef HX_PREVIEW_H
#define HX_PREVIEW_H 1

struct hx_preview {
	char creator[5];
	char type[5];
	char *name;
	void (*output)(struct hx_preview *p, char *buf, int len);
	void *data; /* ptr to the appropriate struct */

	struct hx_preview *next, *prev;
};

struct hx_text_preview { /* text viewing is built-in */
	GtkWidget *window;
	GtkWidget *text;
	struct hx_preview *p;
	/* Phase 5: set TRUE when the user closes the preview window. The
	 * download worker thread checks this before writing to the text
	 * buffer — if the window's gone, the writes are skipped instead
	 * of running into a freed GtkTextBuffer. The flag is read/written
	 * under gtk_threads_enter so the worker and the close-request
	 * handler don't race. */
	gboolean closed;
};

extern struct hx_preview *hx_preview_new(char *creator, char *type, char *name);

#endif /* HX_PREVIEW_H */
