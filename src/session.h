/*
 * session.h — GUI/session-level types.
 *
 * This is where GtkWidget* fields, the per-connection `session` struct,
 * the output_functions vtable, and chat/news/file UI structures live.
 * Anything that includes this header transitively pulls in <gtk/gtk.h>.
 *
 * Files that don't talk to widgets (hmac.c, rand.c, network code) should
 * include protocol.h directly instead.
 */

#ifndef __gtkhx_SESSION_H
#define __gtkhx_SESSION_H 1

#include <gtk/gtk.h>

#include "compat.h"
#include "protocol.h"
#include "prefs.h"
#include "macres.h"

/* ---- GTK 3 compat shims ------------------------------------------- */
/*
 * Phase 3.2: GdkPixmap, GdkBitmap, and GdkColormap were removed in GTK 3.
 * Code throughout this tree still declares variables of those types — most
 * of which are eventually fed into gtk_image_new_from_pixbuf or the
 * gtk_hlist shim, which want GdkPixbuf*. Aliasing the legacy types to
 * GdkPixbuf lets the bulk-mechanical sweep stay mechanical: pixmap *and*
 * mask now point to pixbufs, and the pixmap_create_from_xpm_d → pixbuf
 * migration in Phase 3.2 already feeds pixbufs in. GdkColormap isn't
 * actually used at runtime past truecolor — alias to gpointer for the
 * leftover declarations in init_colors() / cicn.c.
 *
 * Phase 3.4 (cairo) removes the cicn drawing paths that still reference
 * "real" pixmaps/drawables; once that lands the typedefs can be replaced
 * with their proper GdkPixbuf names at each declaration site.
 */
#if !GTK_CHECK_VERSION(3, 0, 0)
#  error "GTK 3 required for the Phase 3 compat shims in session.h"
#endif
typedef GdkPixbuf GdkPixmap;
typedef GdkPixbuf GdkBitmap;
typedef gpointer  GdkColormap;
/*
 * GdkGC and GdkImage have no runtime equivalents in GTK 3 (cairo replaces
 * graphics-contexts; GdkPixbuf replaces image buffers). The legacy
 * declarations of these still appear in users.h / options.c / cicn.c —
 * code that Phase 3.4 will rewrite over cairo. Aliasing them to gpointer
 * lets the headers parse so the rest of the tree compiles in the
 * meantime; any *use* site will fail to link against missing functions
 * (gdk_gc_new, gdk_draw_image, ...) and is on the Phase 3.4 worklist.
 */
typedef gpointer  GdkGC;
typedef gpointer  GdkImage;
/* GdkDrawable was the abstract parent of GdkWindow + GdkPixmap in GTK 2.
 * GTK 3 deletes it; cairo surfaces are the modern equivalent. xtext.h
 * still declares one; aliasing it to gpointer lets the header parse for
 * unrelated consumers. */
typedef gpointer  GdkDrawable;

/* ---- Message-window threading ------------------------------------- */

/* Phase 5+ (GLib-collections): no more next/prev. Open PM windows
 * live in session->msg_windows, a GHashTable<u16 uid, struct
 * msgwin*>. Lookup by uid is O(1) via msgwin_with_uid (now a thin
 * g_hash_table_lookup wrapper); the table owns the msgwin and frees
 * it (name + uid heap pointer + struct itself) via msgwin_free when
 * the user closes the window. */
struct msgwin {
	guint16 *uid;
	char *name;
	GtkWidget *outputbuf;
	GtkWidget *inputbuf;
	GtkWidget *vscroll;
	GtkWidget *window;
	/* Phase 5: header pane above the chat showing the recipient's
	 * icon + name + status. info_image is a GtkImage (Mac classic
	 * cicn rendered to GdkPixbuf via load_icon); info_label is a
	 * single GtkLabel with Pango markup — bold (optionally
	 * coloured) name on top, dim small details (UID · Icon ·
	 * Admin/Guest [Away]) below. msgwin_refresh_user_info()
	 * repopulates both when the user changes their nick / icon /
	 * idle state. */
	GtkWidget *info_image;
	GtkWidget *info_label;
	void *history;
};

/* ---- Custom timer wheel (gtkhx.c) --------------------------------- */

struct timer {
	struct timer *next, *prev;
	guint id;
	int (*fn)(void *);
	void *ptr;
};

/* ---- Icon/sound resource bundle ----------------------------------- */

struct ifn {
	char **files;
	macres_file **cicns;
	unsigned int n;
};

/* ---- Chat windows ------------------------------------------------- */

struct gtkhx_chat {
	struct gtkhx_chat *next, *prev;
	GtkWidget *window;
	GtkWidget *vscroll;
	GtkWidget *output;
	GtkWidget *input;
	GtkWidget *subject;
	GtkWidget *userlist;
	guint32 cid;
	struct chat *chat;
	void *chat_history;
};

/* ---- News (1.5 threaded protocol) --------------------------------- */

struct date_time {
	guint16 base_year;
	guint16 pad;
	guint32 seconds;
};

struct news_post {
	char *buf;
	struct news_item *item;
};

struct news_parts {
	int size;
	char *mime_type;
};

struct news_item {
	guint32 postid, parentid;
	char *sender;
	char *subject;
	struct date_time date;
	guint16 partcount;
	guint16 size;
	struct news_parts *parts;
	GtkTreeIter iter;	/* Phase 2.8: was GtkCTreeNode *node */
	struct news_group *group;
};

struct news_group {
	int post_count;
	struct news_item *posts;
	char *path;
};

struct gnews_catalog {
	struct news_group *group;
	struct gnews_catalog *next, *prev;
	char *path;
	GtkWidget *window;
	GtkWidget *news_tree;	/* GtkTreeView since Phase 2.8 */
	GtkTreeStore *news_store;
	GtkWidget *news_text;
	GtkWidget *authorlbl, *subjectlbl, *datelbl;
	/* selection is queried from news_tree at use time; no cached row */
	char listing;
};

struct path_hist {
	char path[4096];
	struct path_hist *prev;
};

struct gnews_folder {
	GtkWidget *window;
	GtkWidget *news_list;
	gint row, col;
	struct news_folder *news;
	struct gnews_folder *next, *prev;
	char *path;
	char listing;
	GtkWidget *up_btn;
	struct path_hist *path_list;
};

struct folder_item {
	char *name;
/*	guint16 icon; */
	int type;
};

struct news_folder {
	struct folder_item **entry;
	char *path;
	guint32 num_entries;
};

/* ---- User-list / chat membership ---------------------------------- */

struct uesp_fn {
	void *uesp;
	void (*fn)(void *, const char *, const char *, const char *, const hl_access_bits);
};

struct hx_user {
	struct hx_user *next, *prev;
	guint16 uid;
	guint16 icon;
	guint16 color;
	/* Display name. Stored as char[] (rather than unsigned char[]) so
	 * the rest of the codebase can pass it to strcmp/strlen/strcpy
	 * without -Wpointer-sign casts. The Hotline wire is byte-oriented
	 * but in practice the name field always holds an ASCII/Mac Roman
	 * string. */
	char name[32];
	unsigned int ignore:1;
};

struct chat {
	struct chat *next, *prev;
	guint32 cid;
	guint32 nusers;
	struct hx_user __user_list;
	struct hx_user *user_list;
	struct hx_user *user_tail;
	char subject[256];
};

/* ---- The session struct ------------------------------------------- */

typedef struct _session {
	GtkWidget *toolbar_window;
	GtkWidget *news_window;
	GtkWidget *chat_window;
	GtkWidget *tasks_window;
	GtkWidget *users_window;

	GtkWidget *users_list;

	GtkWidget *news_text;
	GtkWidget *postButton;
	GtkWidget *reloadButton;

	GtkWidget *agreementwin;

	struct gtkhx_chat *gchat_list;

	struct gtask *gtask_list;
	GtkWidget *gtklist, *gtask_scroll;

	struct gnews_folder *gfnews_list;

	struct gfile_list *gfile_list;

	/* Phase 5+: open PM windows keyed on the recipient's uid.
	 * Replaces the file-scope `msg_list` global in msg.c and the
	 * dead `sess->msg_list` field that was declared here but never
	 * populated (a long-standing bug — options.c font / wordwrap /
	 * timestamp iterations over the dead session pointer were
	 * silently no-oping). Lookup is O(1) via msgwin_with_uid;
	 * iteration uses GHashTableIter. */
	GHashTable *msg_windows;

	struct gnews_catalog *gcnews_list;

	/* Phase 5+: tasks keyed on the 32-bit trans id. Replaces the
	 * intrusive __task_list / task_list / task_tail trio. Lookup
	 * by trans is O(1); iteration is via GHashTableIter. The
	 * hashtable owns each task; values get freed via task_free
	 * (tasks.c) when removed. */
	GHashTable *tasks;

	/* Phase 5: removed session-level user_list / user_tail / __user_list.
	 * Those fields were declared but never wired up (network.c only ever
	 * initializes chat_list->user_list, and every consumer reads through
	 * chat->user_list — usually chat_with_cid(sess, 0)->user_list, the
	 * public chat). Dead fields are a hazard: I dereferenced
	 * session.user_list in the new PM info pane and segfaulted on the
	 * first PM open. The canonical "global user list" lookup is the
	 * public chat at cid=0 — use chat_with_cid(sess, 0)->user_list. */

	struct chat *chat_front, *chat_tail, *chat_list;
	struct chat __chat_list;

	struct htlc_conn htlc;

	unsigned int connected:1;
} session;

/* Single-session world. Phase 5 will revisit when multi-conn lands;
 * the historical sessions[MAX_CONN] / sess_from_htlc() pretended to be
 * an array but always returned &sessions[0]. */
extern session the_session;

extern char last_msg_nick[32];
extern const char *INFOPREFIX;
extern char *g_user_colors[4];

extern char *colorstr (guint16 color);

extern void hotline_client_input (struct htlc_conn *htlc, char *str, guint32 cid, guint16 style);

extern void hx_connect (struct htlc_conn *htlc, const char *serverstr,
			guint16 port, const char *login, const char *pass,
			char secure);
extern void hx_tracker_list (session *sess, char *addrstr, guint16 port);
extern void hx_quit (void);

/* ---- File browser cache ------------------------------------------- */

struct cached_filelist {
	struct cached_filelist *next, *prev;
	char *path;
	struct hl_filelist_hdr *fh;
	guint32 fhlen;
	unsigned int completing:2;
	char **filter_argv;
};

/* ---- Output backend vtable (only the GUI implementation is alive) -- */

struct output_functions {
	void (*init)(int argc, char **argv);
	void (*loop)(void);
	void (*clear)(struct htlc_conn *htlc, guint32 cid, int subj);
	void (*chat)(struct htlc_conn *htlc, guint32 cid, char *chat, guint16 len);
	void (*msg)(char *name, guint16 uid, char *buf);
	void (*agreement)(session *sess, const char *agreement, guint16 len);
	void (*news_file)(struct htlc_conn *htlc, char *news, guint16 len);
	void (*news_post)(struct htlc_conn *htlc, char *news, guint16 len);
	void (*user_info)(guint16 uid, const char *nam, const char *info, guint16 len);
	void (*file_info) (char *path, char *name, char *creator, char *type, char *comments, char *modified, char *created, guint32 size);
	void (*user_create)(struct htlc_conn *htlc, struct chat *chat, struct hx_user *user, const char *nam, guint16 icon, guint16 color);
	void (*user_delete)(struct htlc_conn *htlc, struct chat *chat, struct hx_user *user);
	void (*user_change)(struct htlc_conn *htlc, struct chat *chat, struct hx_user *user, const char *nam, guint16 icon, guint16 color);
	void (*user_list)(session *sess);
	void (*users_clear)(struct htlc_conn *htlc, struct chat *chat);
	void (*file_list)(struct cached_filelist *cfl, struct hl_filelist_hdr *fh, void *data);
	void (*file_update)(session *sess, struct htxf_conn *htxf);
	void (*tracker_server_create)(struct in_addr addr, guint16 port, guint16 nusers, const char *nam, const char *desc, int total);
	void (*task_update)(session *sess, struct task *tsk);
	void (*news_folder)(struct gnews_folder *gfnews);
	void (*news_catalog)(struct gnews_catalog *gcnews);
	void (*news_thread)(struct news_post *post);
	void (*chat_subject)(struct htlc_conn *htlc, guint32 cid, char *buf);
	void (*chat_invitation)(struct htlc_conn *htlc, guint32 cid, char *name);
	void (*xfer_queue)(session *sess, struct htxf_conn *htxf);
	void (*tracker_clear)(void);
};

extern struct output_functions hx_output;
extern void timer_add_secs (time_t secs, int (*fn)(void *), void *ptr);
extern void timer_delete_ptr (void *ptr);

#endif /* ndef __gtkhx_SESSION_H */
