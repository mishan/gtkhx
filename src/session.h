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
#include "hxconn.h" /* hx_conn_sess — sess_from_htlc reads the opaque conn */
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
    /* The session whose msg_windows table owns this window. Stored rather
     * than resolved through hx_active_session() at use, because a private
     * message is addressed to a uid that is only unique within its
     * connection — the window has to remember which one it belongs to. */
    session *sess;
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
    /* PM input line history — a Rust InputHistory (hxmodel::chat), like
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

/* The Rust cid → conversation registry backing session->chats (defined in the
 * gtkhx-core crate; full FFI in chat.h). Opaque here — session.h only needs
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

/* The 1.5 news reply carriers (news_post + gnews_folder / gnews_catalog) and
 * their retired predecessors (news_item / news_group / news_parts /
 * folder_item / news_folder) are all gone from C. The fetch carriers are
 * Rust-owned in hxhandlers::recv::news's `carrier` module: the browser gets an opaque
 * handle, the sender reads its path, the receive handler stashes a Rust-owned
 * hotline-proto parse handle (DirList / CatList), and the tree is built by
 * hx_news_build_dirlist_from_dirlist / hx_news_build_category_tree_from_catlist
 * (hxmodel::news). No C GUI struct in the
 * middle. */

/* ---- User-list / chat membership ---------------------------------- */

struct uesp_fn {
    void *uesp;
    void (*fn) (void *, const char *, const char *, const char *,
                const hl_access_bits);
};

/* (struct hx_user retired — the user-create/change/delete signals now carry
 * uid + nick_color as scalar args, so no transient carrier is needed. Per-chat
 * membership is owned by the Rust HxMemberModel (hx_chat_member_model (chat));
 * readers query it via chat_members.h.) */

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

    /* The server's own advertised name, from the 1.5+ LOGIN reply's
     * SERVERNAME, or NULL. Display only — hx_session_label prefers it over the
     * host:port the user typed, because it is what the server calls itself.
     *
     * On the session rather than the connection because it is chrome, and
     * because putting it on the connection would mean growing the Rust-owned
     * struct and re-pinning its layout for a string nothing on the wire side
     * reads. It was a `server_addr` global, which named whichever connection
     * had most recently logged in. g_malloc'd; freed and re-set on each
     * login. */
    char *server_name;

    /* What this connection's status bar last said (-1 connecting, 0 not
     * connected, 1 connected, 2 logged in).
     *
     * Per session because it decides whether a transition is worth
     * announcing: "disconnected" is only a toast if this connection had got
     * somewhere first. It was one static shared by every connection, which
     * answered for whichever one had most recently changed state. */
    int last_status;

    /* The Users panel's six action buttons.
     *
     * Per-session because each connection has its own Users content page, so
     * each has its own button bar — and because their sensitivity is a
     * statement about one connection: whether it is up, and whether its
     * account holds HL_ACCESS_DISCONNECT_USERS. They were six file-globals in
     * users.c, which was the same thing while there was one connection and
     * became a fight between them the moment there were two.
     *
     * NULL until this session's Users content is built. `setbtns` reads them
     * and tolerates that. */
    struct {
        GtkWidget *msg;
        GtkWidget *chat;
        GtkWidget *info;
        GtkWidget *kick;
        GtkWidget *ban;
        GtkWidget *ignore;
    } user_btns;

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

    /* per-session gfile_list pointer retired with the legacy files browser.
     * The orthodox-FM browser (files_browser.c) keeps one browser per
     * session, in a table of its own keyed on the session. */

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
     * (gtkhx-core crate — see chat.h), not a GHashTable. */
    HxChatRegistry *chats;

    /* The connection this session owns. Heap-allocated (g_new0) once at
     * startup and owned by the session for its lifetime — a pointer, not an
     * embedded value, so the struct's storage can move behind an opaque Rust
     * owner (docs/rust/network-endgame.md) without every `sess->htlc->` call
     * site changing again. Never NULL after fe_init. */
    struct htlc_conn *htlc;

    unsigned int connected : 1;
} session;

/* Sessions are heap objects held by `session_registry.c`, which is also where
 * the factory and the focus live — there is no global session struct to reach
 * for, and `#include "session_registry.h"` is how you get one.
 *
 * The two routing accessors stay here, next to the type they answer with,
 * because which of them a call site wants is a property of the code doing the
 * asking rather than of the collection.
 *
 * The historical `sessions[MAX_CONN]` / `sess_from_htlc()` pretended to be an
 * array but always returned `&sessions[0]`; what follows is the exact
 * accessor rather than the fiction. */

/*
 * sess_from_htlc(htlc) — the session that OWNS this connection.
 *
 * Model-side code (rcv.c, network.c, …) already holds the htlc for a
 * received event and must route by it: an event belongs to a specific
 * connection, not the focused one. htlc_conn carries a back-pointer to its
 * owning session (`sess`, set at allocation), read here through the gtkhx-core::conn
 * accessor — no longer a container_of, which required htlc to be embedded in
 * session, nor a direct field read of the now-opaque
 * struct. NULL in, NULL out.
 */
static inline session *
sess_from_htlc (struct htlc_conn *htlc)
{
    if (htlc == NULL) {
        return NULL;
    }
    return hx_conn_sess (htlc);
}

/*
 * hx_active_session() — the currently-focused session.
 *
 * UI-side code (a button click, a menu action, a dialog) acts on whichever
 * connection the user is looking at. Today there is exactly one, so this and
 * sess_from_htlc coincide; when the connection tab strip lands this becomes
 * "the focused tab's session" and every UI call site follows without further
 * edits.
 *
 * Declared here for the call sites that only want to read the focus. The rest
 * of the collection — the factory, the count, the ordering — is in
 * session_registry.h, which also declares this.
 *
 * NULL before the first session exists. Most callers predate that being
 * possible and don't check; that is still safe in practice, because startup
 * builds one before any UI can run, but new code should not lean on it.
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

/* struct cached_filelist is now owned by the Rust hxhandlers::recv::files module (the
 * hx_cfl_* accessor facade in files.h). C holds it opaquely — allocate via
 * hx_cfl_new, reach the path / fh buffer / completing / filter_argv through the
 * accessors, free via hx_cfl_free. The fh buffer accumulation for a FILE_LIST
 * reply happens natively in Rust; the view (hxmodel::files_entry populate) still walks
 * the same byte layout via hx_cfl_fh / hx_cfl_fhlen. */
struct cached_filelist;

/* The hx_output vtable is gone (Phase 3.6). Every notification it
 * carried became a GObject signal on GtkhxSession; the lifecycle
 * hooks (init / loop) had exactly one implementation each and are
 * now called by name (fe_init / hx_loop) from main(). */
extern void timer_add_secs (time_t secs, int (*fn) (void *), void *ptr);
extern void timer_delete_ptr (void *ptr);

#endif /* ndef GTKHX_SESSION_H */
