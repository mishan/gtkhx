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

#ifndef __gtkhx_SESSION_H
#define __gtkhx_SESSION_H 1

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
    void *history;
    /* See gtkhx_chat::chat_history_draft. Same idea: stashed
     * input-buffer text from the moment the user first pressed
     * Up at the bottom-of-history "draft" position, restored
     * on Down past the most recent entry. */
    char *history_draft;
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

/* ---- Chat windows ------------------------------------------------- */

/* no more next/prev. Open chat-window
 * UI lives in session->gchats, a GHashTable<u32 cid, struct
 * gtkhx_chat*>. cid=0 is the public chat's window (created at
 * startup by create_chat); pchat windows go in keyed on their
 * cid. Lookup by cid is O(1) via gchat_with_cid; the value
 * destroy notify (gchat_free in chat.c) reclaims the struct. */
struct gtkhx_chat {
    GtkWidget *window;
    GtkWidget *vscroll;
    GtkWidget *output;
    GtkWidget *input;
    GtkWidget *subject;
    /* Phase 8.D: voice toolbar at the top of the chat tab. NULL
     * before the chat window is created; populated by
     * voice_panel_new in create_{chat,pchat}_window. Hidden
     * when HTLC_CAP_VOICE wasn't echoed (which is the common
     * case on most servers). */
    GtkWidget *voice_panel;
    /* per-pchat
	 * sidebar is now an HxUserListView GObject (GtkColumnView-
	 * backed). Forward-declared as an opaque typedef so this
	 * header doesn't have to pull in users_view.h — the field
	 * is read/written from chat.c + users.c only. */
    struct _HxUserListView *userlist;
    guint32 cid;
    struct chat *chat;
    void *chat_history;   /* GNU readline command-line history. */
    /* Stashed input-buffer text from the moment the user first
     * pressed Up at the bottom-of-history "draft" position. When
     * Down navigates back past the most recent history entry,
     * this is restored into the buffer so the user doesn't lose
     * a partially-typed message to up-arrow. NULL when no draft
     * has been captured for the current navigation cycle. Freed
     * (via g_free) before being reassigned and at chat teardown. */
    char *chat_history_draft;

    /* fogWraith chat-history extension state (Phase 3+).
     *
     * history_oldest_msgid — smallest message_id we've already
     *   rendered for this chat. Used as the BEFORE= cursor on
     *   "Load older" fetches so the server returns strictly
     *   older entries. 0 means we have no anchor yet (no
     *   history batch arrived) and a "Load older" click would
     *   be a bare-cursor request (server's default window).
     *
     * history_has_more — last batch's has_more flag, mirrored
     *   here so the renderer + click handler can both consult
     *   it without re-walking the xtext buffer.
     *
     * history_loading — TRUE while a "Load older" fetch is
     *   in-flight. Click handler refuses to fire a second
     *   request until the first completes (the receive path
     *   clears the flag).
     *
     * history_anchor_ent — pointer to the xtext textentry of
     *   the opening "── chat history (N) ──" divider, saved
     *   on initial render. Acts as the insert-point for all
     *   subsequent Load-Older inserts: new older entries +
     *   the refreshed sentinel land just BEFORE this anchor,
     *   so older content stays inside the chat-history block
     *   instead of jumping above the server-notice preamble
     *   ("[hx] connecting to ...") that hx_printf wrote first.
     *
     * history_load_older_ent — pointer to the textentry of
     *   the currently-rendered "↑ Load older" sentinel row,
     *   or NULL when no sentinel is rendered. Refreshed on
     *   every batch: removed via gtk_xtext_remove_entry, then
     *   re-inserted before the anchor if has_more is still
     *   true on the new batch. */
    guint64    history_oldest_msgid;
    gboolean   history_has_more;
    gboolean   history_loading;
    textentry *history_anchor_ent;
    textentry *history_load_older_ent;

    /* Inline-media extension (Phase 9.C UI). Pointer to the
	 * paperclip 'Attach Image' button in this chat's input row.
	 * Visibility is gated on HTLC_CAP_INLINE_MEDIA — initially
	 * hidden, flipped on by inline_media_attach_refresh_all_chats
	 * (called from setbtns alongside the voice-panel refresh)
	 * once the LOGIN reply populates htlc->caps. Stays NULL on
	 * gchats whose input row hasn't been built yet (e.g. before
	 * create_chat_window runs). */
    GtkWidget *media_attach_btn;

    /* Inline-media extension (Phase 9.D dialog). Per-chat token
	 * → HxChatMedia* lookup. When a chat carries inline media,
	 * output_chat_from_event allocates a new token via
	 * media_next_id++, deep-copies the HxChatMedia into the
	 * table, and embeds the token in the placeholder row text
	 * as `hxmedia:N`. The xtext word_click handler
	 * (inline_media_chat_word_click) scans the clicked word for
	 * that substring and dispatches the lookup.
	 *
	 * Lazy-allocated; lives for the chat's lifetime. Freed in
	 * chat_free.
	 *
	 * The HxChatMedia values are owned by the table (the
	 * GHashTable destroy function frees them). */
    GHashTable *media_handles;
    guint       media_next_id;
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
    GtkTreeIter iter;
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

/* no more next/prev. Users live in
 * chat->users, a GHashTable<u16 uid, struct hx_user*>. Lookup by
 * uid via hx_user_with_uid is now O(1); name lookup (uncommon)
 * still walks. */
struct hx_user {
    guint16 uid;
    guint16 icon;
    guint16 color;
    /* Colored-Nicknames extension — per-user 32-bit
	 * 0x00RRGGBB nickname color. HX_NICK_COLOR_NONE (0xFFFFFFFF) is
	 * the sentinel for "no color set"; the renderer falls back to
	 * the legacy `color` status bitmap (Admin/Guest/Away) in that
	 * case. Populated from:
	 *   - HTLS_DATA_COLOR (0x0500) chunk on USER_CHANGE /
	 *     CHAT_USER_CHANGE broadcasts (per-user updates).
	 *   - Same chunk on SELFINFO (server's view of our color, mirrors
	 *     onto htlc->nick_color).
	 *   - USER_LIST record-trailer extension: 4 BE bytes appended
	 *     after the name in every hl_userlist_hdr, so the initial
	 *     post-login user-list paints colors directly without
	 *     waiting for a follow-up USER_CHANGE broadcast for each
	 *     existing user. Servers implement this opportunistically
	 *     (Janus confirmed in the wild); read in rcv_task_user_list
	 *     gated on _len >= 8 + nlen + 4. */
    guint32 nick_color;
    /* Display name. Stored as char[] (rather than unsigned char[]) so
	 * the rest of the codebase can pass it to strcmp/strlen/strcpy
	 * without -Wpointer-sign casts. The Hotline wire is byte-oriented
	 * but in practice the name field always holds an ASCII/Mac Roman
	 * string. */
    char name[32];
    unsigned int ignore : 1;
};

struct chat {
    /* no next/prev — chats live in session->chats, a
	 * GHashTable<u32 cid, struct chat*>. Members likewise live in
	 * chat->users, a GHashTable<u16 uid, struct hx_user*>. */
    guint32 cid;
    guint32 nusers;
    GHashTable *users;
    char subject[256];
};

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

    /* open chat-window UI keyed on cid. Replaces the
	 * doubly-linked gchat_list. The public chat (cid=0) is added
	 * by create_chat at startup; pchat_new adds private-chat
	 * windows. */
    GHashTable *gchats;

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
	 * Lookup is O(1) via chat_with_cid; the value-destroy notify
	 * (chat_free in chat.c) walks chat->user_list and reclaims the
	 * heap-allocated hx_user nodes before freeing the chat. */
    GHashTable *chats;

    struct htlc_conn htlc;

    unsigned int connected : 1;
} session;

/* Single-session world. Phase 5 will revisit when multi-conn lands;
 * the historical sessions[MAX_CONN] / sess_from_htlc() pretended to be
 * an array but always returned &sessions[0]. */
extern session the_session;

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

#endif /* ndef __gtkhx_SESSION_H */
