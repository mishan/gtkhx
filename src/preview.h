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
};

extern struct hx_preview *hx_preview_new(char *creator, char *type, char *name);

#endif /* HX_PREVIEW_H */
