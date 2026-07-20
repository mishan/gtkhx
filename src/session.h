/*
 * session.h — GUI/session-level types.
 *
 * This is where GtkWidget* fields, the per-connection `session` struct,
 * the output_functions vtable, and chat/news/file UI structures live.
 * Anything that includes this header transitively pulls in <gtk/gtk.h>.
 *
 * Files that don't talk to widgets (hmac.c, network code) should
 * include protocol.h directly instead.
 */

#ifndef GTKHX_SESSION_H
#define GTKHX_SESSION_H 1

#include <gtk/gtk.h>

#include "compat.h"
#include "protocol.h"
#include "prefs.h"
#include "macres.h"

/* ---- GTK 3 compat shims ------------------------------------------- */
/*
 * GdkPixmap, GdkBitmap, and GdkColormap were removed in GTK 3.
 * Code throughout this tree still declares variables of those types — most
 * of which are eventually fed into gtk_image_new_from_pixbuf, which wants
 * GdkPixbuf*. Aliasing the legacy types to GdkPixbuf lets the bulk-
 * mechanical sweep stay mechanical: pixmap *and* mask now point to
 * pixbufs, and the pixmap_create_from_xpm_d → pixbuf migration in Phase
 * 3.2 already feeds pixbufs in. GdkColormap isn't actually used at
 * runtime past truecolor — alias to gpointer for the leftover
 * declarations in init_colors() / cicn.c.
 *
 * Phase 3.4 (cairo) removes the cicn drawing paths that still reference
 * "real" pixmaps/drawables; once that lands the typedefs can be replaced
 * with their proper GdkPixbuf names at each declaration site.
 */
#if !GTK_CHECK_VERSION(3, 0, 0)
#error "GTK 3 required for the Phase 3 compat shims in session.h"
#endif
typedef GdkPixbuf GdkPixmap;
typedef GdkPixbuf GdkBitmap;
typedef gpointer GdkColormap;
/*
 * GdkGC and GdkImage have no runtime equivalents in GTK 3 (cairo replaces
 * graphics-contexts; GdkPixbuf replaces image buffers). The legacy
 * declarations of these still appear in users.h / options.c / cicn.c —
 * code that Phase 3.4 will rewrite over cairo. Aliasing them to gpointer
 * lets the headers parse so the rest of the tree compiles in the
 * meantime; any *use* site will fail to link against missing functions
 * (gdk_gc_new, gdk_draw_image, ...) and is on the Phase 3.4 worklist.
 */
typedef gpointer GdkGC;
typedef gpointer GdkImage;
/* GdkDrawable was the abstract parent of GdkWindow + GdkPixmap in GTK 2.
 * GTK 3 deletes it; cairo surfaces are the modern equivalent. xtext.h
 * still declares one; aliasing it to gpointer lets the header parse for
 * unrelated consumers. */
typedef gpointer GdkDrawable;

/* ---- Message-window threading ------------------------------------- */

/* no more next/prev. Open PM windows
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
    /* header pane above the chat showing the recipient's
	 * icon + name + status. info_image is a GtkImage (Mac classic
	 * cicn rendered to GdkPixbuf via load_icon); info_label is a
	 * single GtkLabel with Pango markup — bold (optionally
	 * coloured) name on top, dim small details (UID · Icon ·
	 * Admin/Guest [Away]) below. msgwin_refresh_user_info()
	 * repopulates both when the user changes their nick / icon /
	 * idle state. */
    GtkWidget *info_image;
    GtkWidget *info_label;
    /* PM input line history — a Rust InputHistory (hxchat-model), like
     * gtkhx_chat::chat_history. Owns the Up-arrow draft internally. */
    void *history;
};

/* ---- Custom timer wheel (gtkhx.c) --------------------------------- */

struct timer {
    struct timer *next, *prev;
    guint id;
    int (*fn) (void *);
    void *ptr;
};

/* ---- Icon/sound resource bundle ----------------------------------- */

struct ifn {
    char **files;
    macres_file **cicns;
    unsigned int n;
};

/* Forward declaration so the chat-history bookkeeping below can
 * carry textentry pointers without dragging in xtext.h (which
 * itself #include "session.h" — including it back here would be a
 * circular include). The full typedef lives in xtext.h; C11
 * allows redundant typedefs of the same struct. */
typedef struct textentry textentry;

/* The Rust cid → conversation registry backing session->chats (defined in the
 * gtkhx-session crate; full FFI in chat.h). Opaque here — session.h only needs
 * the pointer type for the struct field below. gnu11 permits this redundant
 * typedef alongside the identical one in chat.h. */
typedef struct HxChatRegistry HxChatRegistry;

/* ---- Chat windows ------------------------------------------------- */

/* struct gtkhx_chat — the per-conversation view — is an opaque handle here.
 * Its definition (and struct hx_chat_history_render, which it embeds) lives in
 * chat.c: it is a C-only view aggregate (GTK widget handles, the Rust
 * InputHistory / MediaTable / HxUserListView handles, and render cursors that
 * are raw pointers into xtext internal entries). Code outside chat.c reaches it
 * through the hx_gchat_* accessors in chat.h. */
struct gtkhx_chat;

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
    GtkTreeIter iter;
    struct news_group *group;
};

struct news_group {
    int post_count;
    struct news_item *posts;
    char *path;
};

struct gnews_catalog {
    /* Owned `CatList *` from gtkhx_proto_parse_catlist, stashed by the Rust
     * receive handler (rcv_task_newscat_list, hxnews-recv) and freed by
     * gnews_browser_handle_catlist. Opaque here — only Rust dereferences it. */
    void *parsed;
    struct gnews_catalog *next, *prev;
    char *path;
    GtkWidget *window;
    GtkWidget *news_tree;
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
    void (*fn) (void *, const char *, const char *, const char *,
                const hl_access_bits);
};

/* Transient signal-payload carrier, NOT a store. Per-chat membership
 * (uid / icon / status colour / nick colour / name / ignore) is owned by
 * the Rust HxMemberModel (hx_chat_member_model (chat)); readers query it
 * via chat_members.h. The user-create/change/delete signals still carry a
 * `struct hx_user *`, but the users.c fan-out reads only ->uid and
 * ->nick_color off it, so rcv.c / options.c fill a short-lived stack
 * instance per emit. Everything else rides in as explicit signal args. */
struct hx_user {
    guint16 uid;
    /* Colored-Nicknames 0x00RRGGBB nick colour, or HX_NICK_COLOR_NONE.
	 * Carried on the signal because it isn't a separate marshalled arg
	 * (the status `color` is), so the render path can read it straight
	 * off the payload. */
    guint32 nick_color;
};

/* struct chat is an opaque handle: the Rust HxConversation
 * (gtkhx-ui/src/conversation.rs) owns the per-chat state — the cid, subject,
 * member model, and view. C holds `struct chat *` and reaches each through
 * its hx_chat_* accessor in chat.h (hx_chat_cid / _subject /
 * hx_chat_member_model / hx_chat_view). Left an incomplete type here on
 * purpose, so a stray `chat->field` is a compile error rather than a silent
 * layout coupling. */
struct chat;

/* ---- The session struct ------------------------------------------- */

typedef struct _session {
    GtkWidget *toolbar_window;
    GtkWidget *news_window;
    GtkWidget *chat_window;
    GtkWidget *tasks_window;
    GtkWidget *users_window;

    /* the standalone Users window's row list is now a
	 * HxUserListView GObject (GtkColumnView-backed). Forward-declared
	 * as an opaque typedef so this header doesn't have to pull in
	 * gtk-side users_view.h — the field is read/written from
	 * users.c only. The view holds the GtkColumnView widget
	 * internally; the column view is packed into a scrolled window
	 * inside the toplevel users_window. */
    struct _HxUserListView *users_view;

    /* Phase 8.D runtime wiring: opaque per-session voice runtime
     * handle (Box<VoiceRuntime> on the Rust side). NULL until the
     * first voice interaction (Join Voice click); the lazy-create
     * helper in voice_panel.c handles construction. Freed on
     * session teardown / disconnect by network.c. */
    struct gtkhx_voice_runtime *voice_runtime;

    /* Phase 8 follow-up: per-uid speaker indicator state. Driven
     * from two sides — rcv.c calls hx_voice_model_ingest_participants
     * on every VOICE_PARTICIPANTS blob (presence + mute bit), and
     * voice_panel.c bridges the runtime's speaker_changed callback
     * into hx_voice_model_set_speaking. users_view renders the
     * resulting indicator column on the chat / users windows.
     *
     * Initialised once per session (cheap empty hashtable) so the
     * users_view can subscribe even before any voice interaction
     * has happened. NULL is also tolerated — the rcv / voice_panel
     * ingest paths null-check before calling. */
    struct _HxVoiceModel *voice_model;

    GtkWidget *news_text;
    GtkWidget *postButton;
    GtkWidget *reloadButton;

    GtkWidget *agreementwin;

    struct gtask *gtask_list;
    GtkWidget *gtklist, *gtask_scroll;

    struct gnews_folder *gfnews_list;

    /* per-session gfile_list pointer retired with the
	 * legacy files browser. The new orthodox-FM browser
	 * (files_browser.c) is a singleton owned by its own static
	 * `the_browser` variable. */

    /* open PM windows keyed on the recipient's uid.
	 * Replaces the file-scope `msg_list` global in msg.c and the
	 * dead `sess->msg_list` field that was declared here but never
	 * populated (a long-standing bug — options.c font / wordwrap /
	 * timestamp iterations over the dead session pointer were
	 * silently no-oping). Lookup is O(1) via msgwin_with_uid;
	 * iteration uses GHashTableIter. */
    GHashTable *msg_windows;

    struct gnews_catalog *gcnews_list;

    /* tasks keyed on the 32-bit trans id. Replaces the
	 * intrusive __task_list / task_list / task_tail trio. Lookup
	 * by trans is O(1); iteration is via GHashTableIter. The
	 * hashtable owns each task; values get freed via task_free
	 * (tasks.c) when removed. */
    GHashTable *tasks;

    /* No session-level user_list / user_tail / __user_list — the
	 * canonical "global user list" lookup is the public chat at cid=0
	 * (use chat_with_cid(sess, 0)->user_list). Do not reintroduce
	 * session.user_list — it was a dead field that had no writer but
	 * looked plausible to read, and tripped a segfault from the PM info
	 * pane the last time someone tried. */

    /* chats keyed on the 32-bit chat-id. Replaces the
	 * chat_front / chat_tail / chat_list trio + the embedded
	 * __chat_list sentinel. cid=0 is the public/server-wide chat
	 * and is created at session init by chats_init() — it must
	 * always exist while the table does. Other chats (private
	 * pchats) are inserted by chat_new and removed by chat_delete.
	 * Lookup via chat_with_cid; the registry's destroy callback
	 * (chat_free in chat.c) tears down each conversation's view then the
	 * conversation handle when it is removed. It's the Rust HxChatRegistry
	 * (gtkhx-session crate — see chat.h), not a GHashTable. */
    HxChatRegistry *chats;

    struct htlc_conn htlc;

    unsigned int connected : 1;
} session;

/* Single-session world today (N == 1). The multi-connection routing
 * seam below (see docs/multi-connection-scoping.md, phase M0) replaces
 * direct `&the_session` access so the single global can later become
 * one of N sessions without re-touching call sites.
 *
 * The historical sessions[MAX_CONN] / sess_from_htlc() pretended to be
 * an array but always returned &sessions[0]; this reintroduces
 * sess_from_htlc() as an exact accessor rather than a fiction. */
extern session the_session;

/*
 * sess_from_htlc(htlc) — the session that OWNS this connection.
 *
 * Model-side code (rcv.c, network.c, …) already holds the htlc for a
 * received event and must route by it: an event belongs to a specific
 * connection, not the focused one. `struct htlc_conn` is embedded in
 * `session` (the `htlc` field), and that embedded instance is the only
 * htlc_conn in the tree, so container_of is exact — and stays correct
 * when sessions become heap-allocated (each session carries its own
 * embedded htlc). NULL in, NULL out.
 */
static inline session *
sess_from_htlc (struct htlc_conn *htlc)
{
    if (htlc == NULL)
        return NULL;
    return (session *) ((char *) htlc - G_STRUCT_OFFSET (session, htlc));
}

/*
 * hx_active_session() — the currently-focused session.
 *
 * UI-side code (a button click, a menu action, a dialog) acts on
 * whichever connection the user is looking at. Today there is exactly
 * one, so this returns &the_session; when the connection tab strip
 * lands it becomes "the focused tab's session" and every
 * UI call site follows without further edits.
 */
extern session *hx_active_session (void);

extern char last_msg_nick[32];
/* INFOPREFIX's extern decl moved to gtkhx_log.h so non-widget
 * callers (Tier 2 test stubs) can resolve it without dragging in
 * this header's GTK surface. */
extern char *g_user_colors[4];

extern char *colorstr (guint16 color);

extern void hotline_client_input (struct htlc_conn *htlc, char *str,
                                  guint32 cid, guint16 style);

extern void hx_connect (struct htlc_conn *htlc, const char *serverstr,
                        guint16 port, const char *login, const char *pass,
                        char secure, char tls);
extern void hx_tracker_list_async (session *sess);
extern void hx_quit (void);

/* ---- File browser cache ------------------------------------------- */

/* cached_filelist had next/prev fields
 * left over from a long-defunct linked-list design that nothing
 * ever wired up. Dropped. Each cfl is owned by a gfile_list entry
 * (gfl->cfl) — the canonical "find a cfl for path P" lookup goes
 * through gfile_list, not through a cfl-side data structure. */
struct cached_filelist {
    char *path;
    struct hl_filelist_hdr *fh;
    guint32 fhlen;
    unsigned int completing : 2;
    char **filter_argv;
};

/* The hx_output vtable is gone (Phase 3.6). Every notification it
 * carried became a GObject signal on GtkhxSession; the lifecycle
 * hooks (init / loop) had exactly one implementation each and are
 * now called by name (fe_init / hx_loop) from main(). */
extern void timer_add_secs (time_t secs, int (*fn) (void *), void *ptr);
extern void timer_delete_ptr (void *ptr);

#endif /* ndef GTKHX_SESSION_H */
