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
	/* Set TRUE on the main thread when the user closes the preview
	 * window. The hx_preview_text_output path on the worker thread
	 * checks this before queuing a chunk; the preview_chunk_idle
	 * dispatcher (also main thread) checks it again before writing
	 * to the text buffer. If the window's gone, the writes are
	 * skipped instead of running into a freed GtkTextBuffer.
	 * Plain gboolean — single-writer (main) means any stale read
	 * by the worker just queues one extra chunk that the dispatcher
	 * will then drop. */
	gboolean closed;
};

extern struct hx_preview *hx_preview_new(char *creator, char *type, char *name);

#endif /* HX_PREVIEW_H */
