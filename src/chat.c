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
#include <libpanel.h>
#include <gdk/gdkkeysyms.h>
#include <sys/types.h>
#include <ctype.h>
#include <time.h>
#include "hx.h"
#include "hxconn.h"
#include "gtkhx_session.h"
#include "gtkhx_theme.h"
#include "hx_panel.h"
#include "panel_registry.h"
#include "toolbar.h"
#include "network.h"
#include "hotline_proto.h"
#include "proto_helpers.h" /* struct hx_chunk (stack-allocated below) */
#include "chat_history.h"
#include "inline_media_attach.h"
#include "inline_media_decode.h"
#include "inline_media_dialog.h"
#include "inline_media_download.h"
#include "inline_media.h"
#include "gtkutil.h"
#include "gtkhx_icon.h" /* gtkhx_icon_load — theme-bundled chrome icons */
#include "chat_bench.h"
#include "chat_members.h"
#include "chat_view.h"
#include "users.h"
#ifdef HAVE_VOICE
#include "voice_panel.h"
#endif
#include "users_view.h"
#include "gtkhx.h"
#include "chat.h"
#include "chat_members.h"
#include "chat_tabs.h"
#include "gtkurl.h"
#include "emoji.h"
#include "tasks.h"
#include "rcv.h"
#include "connect.h"
#include "debug.h"
#include "compat.h" /* _() i18n macro */

/* The per-conversation view aggregate. Opaque outside this file (see the
 * forward decl in session.h); moved here so a stray gchat->field access
 * elsewhere is a compile error. It stays C: GTK widget handles + the
 * chat-view render cursors can't cross into Rust. The Rust-owned handles
 * it holds (chat_history / media_table / userlist) are opaque pointers. */
/* fogWraith chat-history extension render cursors, grouped.
 * Was five loose gtkhx_chat fields; contained here so their
 * widget-coupled lifetime is one named object. The two cursors are
 * HxChatMark handles (chat_view.h) — opaque weak references to rows in
 * the output view. Chat-view phase C0 made them opaque; they used to be
 * raw textentry* into xtext's internal buffer, which is what kept this
 * render state a C view concern rather than a Rust sub-model like
 * chat_history / media_table. Embedded in gtkhx_chat (zero-init'd by
 * its g_malloc0); reset to zero on (re)render. */
struct hx_chat_history_render {
    /* Smallest message_id we've already rendered for this chat. Used
     * as the BEFORE= cursor on "Load older" fetches so the server
     * returns strictly older entries. 0 means no anchor yet (no
     * history batch arrived) and a "Load older" click is a bare-cursor
     * request (server's default window). */
    guint64 oldest_msgid;
    /* Last batch's has_more flag, mirrored here so the renderer + the
     * click handler can consult it without re-walking the xtext
     * buffer. */
    gboolean has_more;
    /* TRUE while a "Load older" fetch is in-flight; the click handler
     * refuses a second request until the receive path clears it. */
    gboolean loading;
    /* Mark for the opening "── chat history (N) ──" divider row, saved
     * on initial render. Insert-point for all subsequent Load-Older
     * inserts: older entries + the refreshed sentinel land just BEFORE
     * this anchor, so older content stays inside the chat-history block
     * instead of jumping above the server-notice preamble that
     * hx_printf wrote first. */
    HxChatMark *anchor_ent;
    /* Mark for the currently-rendered "↑ Load older" sentinel row, or
     * NULL when none is rendered. Refreshed every batch: removed via
     * hx_chat_view_remove, then re-inserted before the anchor if
     * has_more is still true on the new batch. */
    HxChatMark *load_older_ent;
};

/* The per-conversation window/view. No longer stored in its own
 * table: it hangs off the matching model (struct chat::view) in
 * sess->chats, keyed by cid. cid=0 is the public chat's window (created
 * at startup by create_chat); pchat windows attach in pchat_new. Lookup
 * by cid is O(1) via gchat_with_cid (chat_with_cid(sess, cid)->view);
 * gchat_free reclaims the struct. */
struct gtkhx_chat {
    GtkWidget *window;
    GtkWidget *vscroll;
    GtkWidget *output;
    GtkWidget *input;
    GtkWidget *subject;
    /* Voice Join/Mute icon controls for a private chat, hosted in
     * that pchat's user-list action bar (create_pchat_window). NULL
     * for the public chat — its controls live in the standalone
     * Users window instead (create_users_window), and the voice
     * panels are tracked in voice_panel.c's own registry rather than
     * through this field. Hidden when HTLC_CAP_VOICE wasn't echoed
     * (the common case on most servers). */
    GtkWidget *voice_panel;
    /* per-pchat
     * sidebar is now an HxUserListView GObject (GtkColumnView-
     * backed). Forward-declared as an opaque typedef so this
     * header doesn't have to pull in users_view.h — the field
     * is read/written from chat.c + users.c only. */
    struct _HxUserListView *userlist;
    /* The window's identity: its chat id. The matching model object is the
     * sess->chats entry under this cid, and it owns this view via chat::view
     * — look it up with chat_with_cid(sess, cid) rather than caching a
     * raw struct chat* back-pointer here (the old `chat` field, now gone). */
    guint32 cid;

    /* fogWraith chat-history extension render cursors (Phase 3+),
     * grouped into one named sub-object — see struct
     * hx_chat_history_render above for the per-field notes. The
     * anchor / load-older marks are weak references that a scrollback
     * trim or a clear can invalidate, so treat them as live only
     * during a render pass; hx_chat_view_remove on a stale mark is a
     * safe FALSE. */
    struct hx_chat_history_render render;

    /* Inline-media extension (Phase 9.C UI). Pointer to the
     * paperclip 'Attach Image' button in this chat's input row.
     * Visibility is gated on HTLC_CAP_INLINE_MEDIA — initially
     * hidden, flipped on by inline_media_attach_refresh_all_chats
     * (called from setbtns alongside the voice-panel refresh)
     * once the LOGIN reply populates htlc->caps. Stays NULL on
     * gchats whose input row hasn't been built yet (e.g. before
     * create_chat_window runs). */
    GtkWidget *media_attach_btn;
};

/* The input line history + the inline-media token table used to live on the
 * view above (chat_history / media_table); they moved to the conversation model
 * (HxConversation), reached via hx_chat_input_history / hx_chat_media_table on
 * chat_with_cid(sess, cid) — they're chat state, not view state, and now
 * survive a pchat window close + reopen. */

/* Compose the load-older / loading-older sentinels by translating the
 * bare phrase (e.g. "Load older messages") and stitching the leading
 * up-arrow + NBSP joiners back in. xtext's word tokenizer splits on
 * ASCII space/'\n'/'<'/'>'/NUL, so internal spaces in the translated
 * phrase become NBSPs (U+00A0 = "\xc2\xa0") to keep the row clickable
 * as one token. The leading U+2191 (up-arrow) is part of the click
 * target — chat_history_word_click compares against the same
 * composed string. */
static char *
hx_compose_sentinel (const char *phrase)
{
    GString *s = g_string_new ("\xe2\x86\x91"
                               "\xc2\xa0"); /* ↑ + NBSP */
    for (const char *p = phrase; *p; p++) {
        if (*p == ' ') {
            g_string_append (s, "\xc2\xa0"); /* NBSP */
        } else {
            g_string_append_c (s, *p);
        }
    }
    return g_string_free (s, FALSE);
}

const char *
hx_load_older_sentinel (void)
{
    static char *cached = NULL;
    if (!cached) {
        cached = hx_compose_sentinel (_ ("Load older messages"));
    }
    return cached;
}

const char *
hx_loading_older_sentinel (void)
{
    static char *cached = NULL;
    if (!cached) {
        cached = hx_compose_sentinel (_ ("Loading older messages..."));
    }
    return cached;
}

#define WORD_URL 1
#define WORD_NICK 2
#define WORD_HOST 4
#define WORD_EMAIL 5

/*
 * Phase 2.6.B: 37-entry palette laid out for the chat view.
 *
 * Slot layout (see chat_view.h's HX_CHAT_PAL_* vocabulary):
 *   0..15   mIRC colors 0..15
 *   16..31  mIRC colors 16..31 (bold/extended; HexChat duplicates 0..15)
 *   32      HX_CHAT_PAL_MARK_FG   selection foreground
 *   33      HX_CHAT_PAL_MARK_BG   selection background
 *   34      HX_CHAT_PAL_FG        default text foreground
 *   35      HX_CHAT_PAL_BG        default text background
 *   36      HX_CHAT_PAL_MARKER    marker line color
 *
 * The previous (GTK 1.2 / XChat 1.8.5) widget only consulted slots 0..19,
 * with 16/17 = mark bg/fg and 18/19 = fg/bg.  HexChat's xtext reads
 * past slot 19 and was getting uninitialized memory, which is why the
 * default background looked weird.
 */
/* GdkColor (16-bit per channel + paletted-X11 pixel slot)
 * → GdkRGBA (4 doubles 0..1, no pixel slot). Each row is one color
 * literal preserved exactly via the RGB16 macro from compat.h, which
 * folds (channel/65535.0) at compile time. */
GdkRGBA colors[] = {
    /* mIRC 0..15 */
    RGB16 (0, 0, 0),                /* 0  black */
    RGB16 (0xcccc, 0xcccc, 0xcccc), /* 1  white */
    RGB16 (0, 0, 0xcccc),           /* 2  blue */
    RGB16 (0, 0xcccc, 0),           /* 3  green */
    RGB16 (0xcccc, 0, 0),           /* 4  red */
    RGB16 (0xbbbb, 0xbbbb, 0),      /* 5  yellow/brown */
    RGB16 (0xbbbb, 0, 0xbbbb),      /* 6  purple */
    RGB16 (0xffff, 0xaaaa, 0),      /* 7  orange */
    RGB16 (0xffff, 0xffff, 0),      /* 8  yellow */
    RGB16 (0, 0xffff, 0),           /* 9  green */
    RGB16 (0, 0xcccc, 0xcccc),      /* 10 aqua */
    RGB16 (0, 0xffff, 0xffff),      /* 11 light aqua */
    RGB16 (0, 0, 0xffff),           /* 12 blue */
    RGB16 (0xffff, 0, 0xffff),      /* 13 pink */
    RGB16 (0x7777, 0x7777, 0x7777), /* 14 grey */
    RGB16 (0x9999, 0x9999, 0x9999), /* 15 light grey */
    /* mIRC 16..31 — duplicate of 0..15 (HexChat does the same) */
    RGB16 (0, 0, 0),                /* 16 black */
    RGB16 (0xcccc, 0xcccc, 0xcccc), /* 17 white */
    RGB16 (0, 0, 0xcccc),           /* 18 blue */
    RGB16 (0, 0xcccc, 0),           /* 19 green */
    RGB16 (0xcccc, 0, 0),           /* 20 red */
    RGB16 (0xbbbb, 0xbbbb, 0),      /* 21 yellow/brown */
    RGB16 (0xbbbb, 0, 0xbbbb),      /* 22 purple */
    RGB16 (0xffff, 0xaaaa, 0),      /* 23 orange */
    RGB16 (0xffff, 0xffff, 0),      /* 24 yellow */
    RGB16 (0, 0xffff, 0),           /* 25 green */
    RGB16 (0, 0xcccc, 0xcccc),      /* 26 aqua */
    RGB16 (0, 0xffff, 0xffff),      /* 27 light aqua */
    RGB16 (0, 0, 0xffff),           /* 28 blue */
    RGB16 (0xffff, 0, 0xffff),      /* 29 pink */
    RGB16 (0x7777, 0x7777, 0x7777), /* 30 grey */
    RGB16 (0x9999, 0x9999, 0x9999), /* 31 light grey */
    /* UI roles */
    RGB16 (0xeeee, 0xeeee, 0xeeee), /* 32 HX_CHAT_PAL_MARK_FG (light) */
    RGB16 (0x2020, 0x4a4a, 0x8787), /* 33 HX_CHAT_PAL_MARK_BG (blue) */
    RGB16 (0xcccc, 0xcccc, 0xcccc), /* 34 HX_CHAT_PAL_FG (light) */
    RGB16 (0, 0, 0),                /* 35 HX_CHAT_PAL_BG (black) */
    RGB16 (0xcccc, 0, 0),           /* 36 HX_CHAT_PAL_MARKER (red) */
    /* 37 HX_CHAT_PAL_HISTORY_MUTED — chat-history secondary text.
     * Static default is the dark-theme value (medium grey, visible
     * against black bg); gtkhx_apply_theme_palette recomputes it
     * for the active light/dark scheme as soon as AdwStyleManager
     * has settled. */
    RGB16 (0x9a9a, 0x9a9a, 0x9a9a),
};

/* Refresh palette slots that depend on Light / Dark and push the
 * new palette into every live xtext widget. The mIRC slots (0..31)
 * are theme-agnostic — same red is "red" in both modes, server
 * authors expect those exact values. The six UI-role slots (32..37)
 * come from the active theme (gtkhx_theme_get_color); a theme that
 * omits a slot falls back to the built-in default, which matches
 * the historical light/dark constants byte-for-byte. The historical
 * defaults for reference:
 *
 *   Light  HX_CHAT_PAL_FG            = #1d1d1d  (near-black on white)
 *          HX_CHAT_PAL_BG            = #fafafa  (matches Adwaita's view bg)
 *          MARK_FG             = #ffffff  (selection contrast)
 *          MARK_BG             = #3584e4  (Adwaita accent blue)
 *          HX_CHAT_PAL_HISTORY_MUTED = #5e5e5e  (~5.7:1 vs #fafafa)
 *   Dark   HX_CHAT_PAL_FG            = #cccccc
 *          HX_CHAT_PAL_BG            = #000000
 *          MARK_FG             = #eeeeee
 *          MARK_BG             = #204a87  (Tango blue, the original)
 *          HX_CHAT_PAL_HISTORY_MUTED = #9a9a9a  (~7:1 vs #000000)
 *
 * Called once at startup from gtkhx_activate after the
 * AdwStyleManager has settled on Light or Dark; again any time the
 * manager's `dark` property flips; and again any time the theme's
 * "changed" signal fires (theme file reload, future Settings edit). */
void
gtkhx_apply_theme_palette (gboolean dark)
{
    /* Slot ↔ role mapping. The slot numbers (32..37) are
     * chat_view.h's HX_CHAT_PAL_* vocabulary; the roles are the
     * theme-file-visible names. */
    static const struct {
        int slot;
        GtkhxPaletteRole role;
    } role_to_slot[] = {
        { HX_CHAT_PAL_MARK_FG, GTKHX_PAL_MARK_FG },
        { HX_CHAT_PAL_MARK_BG, GTKHX_PAL_MARK_BG },
        { HX_CHAT_PAL_FG, GTKHX_PAL_FG },
        { HX_CHAT_PAL_BG, GTKHX_PAL_BG },
        { HX_CHAT_PAL_MARKER, GTKHX_PAL_MARKER },
        { HX_CHAT_PAL_HISTORY_MUTED, GTKHX_PAL_HISTORY_MUTED },
    };
    for (size_t i = 0; i < G_N_ELEMENTS (role_to_slot); i++) {
        colors[role_to_slot[i].slot]
            = gtkhx_theme_get_color (role_to_slot[i].role, dark);
    }

    /* Push the new palette into every live xtext widget. Chat /
     * private-chat outputs hang off each model's view (hx_chat_view (chat)) in sess->chats;
     * private-message outputs hang off msgwin in sess->msg_windows.
     * News uses a plain GtkTextView (theme-driven CSS), not an
     * xtext, so it picks up the system theme without help. */
    session *sess = hx_active_session ();
    if (sess->chats) {
        guint n = hx_chats_count (sess->chats);
        for (guint i = 0; i < n; i++) {
            struct chat *c = hx_chats_get_at (sess->chats, i);
            struct gtkhx_chat *gchat = hx_chat_view (c);
            if (gchat && gchat->output) {
                hx_chat_view_set_palette (gchat->output, colors);
                hx_chat_view_refresh (gchat->output);
            }
        }
    }
    if (sess->msg_windows) {
        GHashTableIter iter;
        gpointer val;
        g_hash_table_iter_init (&iter, sess->msg_windows);
        while (g_hash_table_iter_next (&iter, NULL, &val)) {
            struct msgwin *msg = val;
            if (msg->outputbuf) {
                hx_chat_view_set_palette (msg->outputbuf, colors);
                hx_chat_view_refresh (msg->outputbuf);
            }
        }
    }
}

/* hx_send_chat / hx_chat_user / hx_invite_user / hx_chat_join / hx_part_chat
 * / hx_change_subject moved to the hxhandlers::send::chat Rust crate (the port of
 * chat.c's send path, over hotline-proto's native chat builders). chat.h keeps
 * the C ABI decls; the per-htlc cap + chat-model lookups the senders need are
 * in chat_send_bridge.c. */

int
word_check (GtkWidget *xtext, char *word)
{
    char *at, *dot;
    size_t i, len = strlen (word);
    int dots;

    /* Scheme + bare-prefix matching lives in gtkurl.c so the xtext
     * hover answer (here) and the GtkTextView popup answer
     * (gtkurl_textview_install) come off the same scheme list. Adding
     * a new URL scheme is a one-line edit to url_schemes[] in
     * gtkurl.c; no second change needed here. */
    if (gtkurl_word_has_url_scheme (word)) {
        return WORD_URL;
    }

    /*	if (find_name (sess, word))
    return WORD_NICK; */

    at = strchr (word, '@'); /* check for email addy */
    dot = strrchr (word, '.');
    if (at && dot) {
        if ((unsigned long)at < (unsigned long)dot) {
            if (strchr (word, '*')) {
                return WORD_HOST;
            } else {
                return WORD_EMAIL;
            }
        }
    }

    /* check if it's an IP number */
    dots = 0;
    for (i = 0; i < len; i++) {
        if (word[i] == '.') {
            dots++;
        }
    }
    if (dots == 3) {
        if (g_hostname_is_ip_address (word)) {
            return WORD_HOST;
        }
    }

    if (!strncasecmp (word + len - 5, ".html", 5)) {
        return WORD_HOST;
    }

    if (!strncasecmp (word + len - 4, ".org", 4)) {
        return WORD_HOST;
    }

    if (!strncasecmp (word + len - 4, ".net", 4)) {
        return WORD_HOST;
    }

    if (!strncasecmp (word + len - 4, ".com", 4)) {
        return WORD_HOST;
    }

    if (!strncasecmp (word + len - 4, ".edu", 4)) {
        return WORD_HOST;
    }

    if (len > 5) {
        if (word[len - 3] == '.' && isalpha (word[len - 2])
            && isalpha (word[len - 1])) {
            return WORD_HOST;
        }
    }

    return 0;
}

/* timecpy is gone. The "[HH:MM:SS] " inline-timestamp prefix
 * it produced is now drawn by xtext as a left-column stamp via
 * hx_chat_view_set_time_stamp on each view. xprintline / xoutput_chat
 * just append the bare message text; the per-entry timestamp is
 * auto-set by the view on append. */

/* chat lifecycle on the HxChatRegistry. chat_free() is the destroy callback the
 * session->chats registry invokes when an entry is removed or the registry is
 * freed. It frees the chat's authoritative membership model, its attached view
 * (if any), and the struct itself. */

/* Forward decls: chat_free (the model destroy-notify) tears down an
 * attached view via gchat_free, and chats_init installs pchat_close as
 * the tab-close handler. Both are defined further down. */
static void gchat_free (gpointer p);
static void pchat_close (struct htlc_conn *htlc, guint32 cid);

static void
chat_free (gpointer p)
{
    struct chat *chat = p;

    if (!chat) {
        return;
    }
    /* The conversation owns its view. Normally the view is detached (freed,
     * set to NULL) by gchat_delete / pchat_close before the conversation
     * goes; free a still-attached view so deleting a conversation can't leak
     * its window struct. hx_conversation_free then drops the member model
     * and the handle. */
    struct gtkhx_chat *view = hx_chat_view (chat);
    if (view) {
        gchat_free (view);
        hx_chat_set_view (chat, NULL);
    }
    hx_conversation_free (chat);
}

void
chats_init (session *sess)
{
    if (sess->chats) {
        return;
    }
    sess->chats = hx_chats_new (chat_free);
    /* Public chat (cid=0) must always exist while the table does —
     * it's where the server-wide user list lives and where
     * top-level chat messages are routed. Create it eagerly so
     * chat_with_cid(sess, 0) is always non-NULL. */
    chat_new (sess, 0);
    /* The authoritative public-chat membership model.
     * Created once, fed by the users.c fan-out, read by tab_nick_comp. */

    /* Route user-clicks on a private-chat tab's X through pchat_close
     * (which sends hx_part_chat and tears down the view). Idempotent.
     * (moved here from the now-removed gchats_init.) */
    gtkhx_chat_tabs_set_close_pchat_handler (pchat_close);
}

struct chat *
chat_new (session *sess, guint32 cid)
{
    /* The conversation is a Rust HxConversation handle — it creates its own
     * authoritative membership model (fed by the users.c fan-out, read by
     * tab_nick_comp / the user-list view / rcv.c old-state lookups). */
    struct chat *chat = hx_conversation_new (cid);
    hx_chats_insert (sess->chats, cid, chat);
    return chat;
}

void
chat_delete (session *sess, struct chat *chat)
{
    if (!chat || !sess->chats) {
        return;
    }
    hx_chats_remove (sess->chats, hx_chat_cid (chat));
}

struct chat *
chat_with_cid (session *sess, guint32 cid)
{
    if (!sess->chats) {
        return NULL;
    }
    return hx_chats_lookup (sess->chats, cid);
}

/* gtkhx_chat (UI side) lifecycle. The view isn't stored in its own table —
 * it hangs off its conversation (hx_chat_view (chat)). gchat_free g_frees the
 * struct (and drops the strong refs it owns below); the widget subtree
 * (window, output, input, subject, userlist, vscroll) is owned by the parent
 * window and reclaimed when the window is destroyed. It runs either from
 * chat_free (the registry destroy callback, when the conversation goes) or
 * from gchat_delete / pchat_close, which detach the view and destroy the
 * window separately. */
static void
gchat_free (gpointer p)
{
    struct gtkhx_chat *gchat = p;
    if (!gchat) {
        return;
    }
    /* Release our strong ref to the per-pchat HxUserListView (Phase
     * C). The view's column_view widget is already being torn down
     * with the parent window, but we held a separate ref on the
     * GObject itself since create_pchat_window. The public-chat
     * gchat is created in create_chat with userlist=NULL so this is
     * a no-op there. */
    g_clear_object (&gchat->userlist);
    /* chat_history + media_table are owned by the conversation model now, not
     * the view — they're freed in hx_conversation_free (via chat_free), so a
     * view teardown here must NOT touch them. */
    g_free (gchat);
}

/* The view is reached through its model — chat_with_cid(sess,
 * cid)->view — so gchat_with_cid is a thin wrapper and there is no
 * separate gchats table (gchats_init is gone; chats_init installs the
 * tab-close handler now). */
struct gtkhx_chat *
gchat_with_cid (session *sess, guint32 cid)
{
    struct chat *chat = chat_with_cid (sess, cid);
    return hx_chat_view (chat);
}

void
gchat_delete (session *sess, struct gtkhx_chat *gchat)
{
    if (!gchat) {
        return;
    }
    /* Detach the view from its model (the entry lingers as a window-less
     * model) and free the view struct. The model stays in sess->chats
     * until chat_delete. */
    struct chat *chat = chat_with_cid (sess, gchat->cid);
    if (chat && hx_chat_view (chat) == gchat) {
        hx_chat_set_view (chat, NULL);
    }
    gchat_free (gchat);
}

/* Render a single chat line into an xtext buffer with the
 * HexChat-style nick column. Inputs are slices into `line`
 * (already-validated UTF-8); the caller is responsible for the
 * UTF-8 conversion + the sender/body split (either inline, as
 * xprintline below does, or pre-parsed via HxChatEvent for the
 * chat-signal path).
 *
 * `is_info` suppresses the highlight check (info lines are never
 * highlighted). `is_self` controls the bracket colour: mIRC 13
 * (pink) for our own lines, 12 (light blue) for others.
 *
 * If name_len == 0 the line is rendered without a nick column —
 * either because hx_chat_split_nick_body didn't match (emote,
 * raw server prose) or the caller wanted a plain append. */
static void
xprintline_render_parts (GtkWidget *text, const char *name, gsize name_len,
                         const char *body_text, gsize body_text_len,
                         gboolean is_info, gboolean is_self, gint16 info_color,
                         HxChatSpeaker speaker)
{
    /* Gutter runs. At most three: "<", the name, ">". The old code
     * built these as one "\003NN<\003name\003NN>\003" string, which
     * is why the info-prefix branch below used to have to search for
     * the closing escape to find where the name ended. */
    HxChatRun gutter[3];
    int n_gutter = 0;
    const char *display_name = NULL;
    gsize display_name_len = 0;
    const char *display_body = NULL;
    gsize display_body_len = 0;
    gboolean have_nick = FALSE;
    session *sess = hx_active_session ();
    const char *self_nick = (hx_conn_name (sess->htlc)[0] != '\0')
                                ? (const char *)hx_conn_name (sess->htlc)
                                : NULL;

    if (name_len > 0) {
        have_nick = TRUE;
        display_name = name;
        display_name_len = name_len;
        display_body = body_text;
        display_body_len = body_text_len;
    }

    /* Highlight-on-mention. Skip if it's an info line, if we said
     * the line ourselves, or if there's no parsed body to scan. */
    gboolean do_highlight = FALSE;
    if (have_nick && !is_info && !is_self && display_body_len > 0) {
        GPtrArray *words = g_ptr_array_new ();
        if (self_nick && *self_nick) {
            g_ptr_array_add (words, (gpointer)self_nick);
        }
        gchar **extras = NULL;
        if (gtkhx_prefs.highlight_words && *gtkhx_prefs.highlight_words) {
            extras = g_strsplit (gtkhx_prefs.highlight_words, ",", -1);
            for (gsize ei = 0; extras && extras[ei]; ei++) {
                gchar *w = g_strstrip (extras[ei]);
                if (*w) {
                    g_ptr_array_add (words, w);
                }
            }
        }
        g_ptr_array_add (words, NULL);
        do_highlight = hx_highlight_match (display_body, display_body_len,
                                           (const char *const *)words->pdata);
        g_ptr_array_unref (words);
        if (extras) {
            g_strfreev (extras);
        }
    }

    if (have_nick) {
        HxChatRun body_run;

        if (is_info) {
            /* "[hx]", or a broadcast sender's name. Brackets in the
             * wrapper colour, name in whichever colour the prefix
             * asked for — both were literal text with escapes around
             * them before. */
            gutter[n_gutter++]
                = (HxChatRun){ "[", 1, HX_CHAT_INFO_BRACKET_COLOR,
                               HX_CHAT_ATTR_NONE };
            gutter[n_gutter++]
                = (HxChatRun){ display_name, (int)display_name_len, info_color,
                               HX_CHAT_ATTR_NONE };
            gutter[n_gutter++]
                = (HxChatRun){ "]", 1, HX_CHAT_INFO_BRACKET_COLOR,
                               HX_CHAT_ATTR_NONE };
            body_run = HX_CHAT_RUN_PLAIN (display_body, (int)display_body_len);
        } else if (do_highlight) {
            /* Mention: bold light-red nick, plain body. The old form
             * appended a \017 reset byte to the body to stop the
             * attribute leaking into the next row; runs have no
             * running state, so there is nothing to reset. */
            gutter[n_gutter++]
                = (HxChatRun){ display_name, (int)display_name_len,
                               HX_CHAT_HIGHLIGHT_COLOR, HX_CHAT_ATTR_BOLD };
            body_run = HX_CHAT_RUN_PLAIN (display_body, (int)display_body_len);
        } else {
            /* "<nick>" — brackets coloured, name in the default
             * foreground. Pink for our own lines, light blue for
             * everyone else's, as since Phase 5. */
            gint16 brack = is_self ? 13 : 12;
            gutter[n_gutter++]
                = (HxChatRun){ "<", 1, brack, HX_CHAT_ATTR_NONE };
            gutter[n_gutter++]
                = HX_CHAT_RUN_PLAIN (display_name, (int)display_name_len);
            gutter[n_gutter++]
                = (HxChatRun){ ">", 1, brack, HX_CHAT_ATTR_NONE };
            body_run = HX_CHAT_RUN_PLAIN (display_body, (int)display_body_len);
        }

        hx_chat_view_append_runs (text, speaker, gutter, n_gutter, &body_run, 1,
                                  0);
    } else {
        HxChatRun body_run = HX_CHAT_RUN_PLAIN (body_text, (int)body_text_len);
        hx_chat_view_append_runs (text, speaker, NULL, 0, &body_run, 1, 0);
    }
}

/* Offsets-into-one-line wrapper, for the log-line path where the name
 * and body really are slices of the same buffer. */
static void
xprintline_render (GtkWidget *text, const char *line, gsize line_len,
                   gsize name_off, gsize name_len, gsize body_off,
                   gsize body_len, gboolean is_info, gboolean is_self,
                   gint16 info_color, HxChatSpeaker speaker)
{
    if (name_len == 0) {
        xprintline_render_parts (text, NULL, 0, line, line_len, is_info,
                                 is_self, info_color, speaker);
        return;
    }
    xprintline_render_parts (text, line + name_off, name_len, line + body_off,
                             body_len, is_info, is_self, info_color, speaker);
}

/* Strip a repeated "<sender>:  " prefix off a continuation line.
 *
 * Hotline chat is a text stream, and servers format each line as
 * "<padded nick>:  <text>". When a client sends a message containing
 * newlines the server prefixes *every* line, so a five-line message
 * comes back as:
 *
 *     misha:  first
 *     misha:  second
 *     misha:  third
 *
 * We parse the nick off the first line and render it in the gutter, but
 * the continuation lines used to be appended verbatim — so a multi-line
 * message displayed with the sender's name repeated down the left of its
 * own body.
 *
 * The prefix is recognised with hx_chat_split_nick_body, the same
 * function that produced the event's sender in the first place, so what
 * gets stripped is by construction what would have been parsed. Only an
 * exact match on the same sender is stripped: a different name means a
 * different speaker and belongs on screen.
 *
 * A continuation line the user deliberately began with their own
 * "name: " loses that text. That is a real if vanishingly rare cost, and
 * the alternative — leaving the prefix on every line — is wrong far more
 * often. */
static void
chat_strip_repeat_sender (const char *seg, gsize seg_len, const char *sender,
                          gsize sender_len, const char **out, gsize *out_len)
{
    gsize so, sl, bo, bl;

    *out = seg;
    *out_len = seg_len;

    if (sender_len == 0 || seg_len == 0) {
        return;
    }
    if (!hx_chat_split_nick_body (seg, seg_len, &so, &sl, &bo, &bl)) {
        return;
    }
    if (sl != sender_len || memcmp (seg + so, sender, sender_len) != 0) {
        return;
    }
    *out = seg + bo;
    *out_len = bl;
}

/* Join a multi-line body into one string, dropping the sender prefix
 * the server repeats on each line after the first.
 *
 * Returns a newly-allocated string the caller frees, and writes its
 * length to *joined_len.
 *
 * The length is an out-parameter rather than something the caller
 * recovers with strlen because the result can contain NUL: chat bodies
 * arrive off the wire and gtkhx_text_to_utf8 preserves embedded NULs
 * deliberately rather than truncating there. strlen would silently cut
 * the message at the first one — losing the rest of the text and, since
 * the renderer parses markdown over this buffer, potentially leaving a
 * fence unterminated. */
static gchar *
chat_join_body (const char *body, gsize body_len, const char *sender,
                gsize sender_len, gsize *joined_len)
{
    GString *out = g_string_new (NULL);
    const char *cur = body;
    const char *end = body + body_len;
    gboolean first = TRUE;

    /* `<`, not `<=`: at cur == end there is nothing left to emit, but
     * the !first branch would still have appended a separator — so a
     * body ending in '\n' (which broadcasts and many chat lines do)
     * gained a trailing blank line. The old per-line loop stopped at
     * `< end` too, which is why this only appeared once the lines were
     * joined. */
    while (cur < end) {
        const char *nl = memchr (cur, '\n', end - cur);
        gsize seg_len = nl ? (gsize)(nl - cur) : (gsize)(end - cur);
        const char *seg = cur;
        gsize out_len = seg_len;

        if (!first) {
            chat_strip_repeat_sender (cur, seg_len, sender, sender_len, &seg,
                                      &out_len);
            g_string_append_c (out, '\n');
        }
        g_string_append_len (out, seg, (gssize)out_len);
        first = FALSE;
        if (!nl) {
            break;
        }
        cur = nl + 1;
    }
    *joined_len = out->len;
    return g_string_free (out, FALSE);
}

/* Right-click on a nick in chat output.
 *
 * Hands straight to users.c::user_popup_show — the same builder the
 * Users window and the pchat sidebars use, so there is one menu rather
 * than a chat-shaped copy that drifts from it as items are added. The
 * view supplied the uid (see HxChatSpeaker) and the pointer position;
 * everything else the menu needs it looks up itself.
 *
 * `user_data` carries the cid so a private chat pops the menu against
 * its own membership rather than the public room's. */
static void
chat_speaker_menu (GtkWidget *view, guint uid, double x, double y,
                   gpointer user_data)
{
    guint32 cid = GPOINTER_TO_UINT (user_data);

    if (uid == 0) {
        return;
    }
    user_popup_show (view, hx_active_session (), cid, (guint16)uid, x, y);
}

/* Identify who said a line.
 *
 * `wire_uid` is the sender's uid straight off the chat message's UID
 * chunk (`parse_chat` → `HxChatEvent.uid`). When the server sent one,
 * that is authoritative and this does no work.
 *
 * It is 0 when the server sent no UID chunk — older servers omit it, and
 * it is also 0 for lines that didn't come from a chat message at all
 * (log lines, the `hx_printf` path). Then, and only then, fall back to
 * looking the nick up in the conversation's membership model — the same
 * `HxMemberModel` the user list is built from, so chat and Users resolve
 * to one record rather than two that happen to agree.
 *
 * If the fallback misses too, uid stays 0 and stays a miss. The speaker
 * may have parted since talking, two users may share a name, or the
 * "nick" may be server prose that merely looked like one. Guessing would
 * attach someone else's avatar and group two people's messages together,
 * which is worse than showing neither. */
static HxChatSpeaker
chat_speaker_for (guint32 cid, guint16 wire_uid, const char *nick,
                  gsize nick_len, gboolean is_self)
{
    HxChatSpeaker sp = HX_CHAT_SPEAKER_NONE;

    sp.outgoing = is_self;
    struct chat *conv;
    char *nul;

    if (!nick || nick_len == 0) {
        return sp;
    }
    if (wire_uid != 0) {
        sp.uid = wire_uid;
        sp.nick = nick;
        sp.nick_len = (int)nick_len;
        return sp;
    }
    conv = chat_with_cid (hx_active_session (), cid);
    if (!conv) {
        return sp;
    }
    nul = g_strndup (nick, nick_len);
    sp.uid = hx_member_model_find_by_name (hx_chat_member_model (conv), nul);
    g_free (nul);
    /* Borrowed for the append call only, which is why this can hand back
     * a pointer into the caller's buffer — and why the length has to
     * travel with it, since that buffer is the whole chat line and the
     * name is a slice from its middle. */
    sp.nick = sp.uid ? nick : NULL;
    sp.nick_len = (int)nick_len;
    return sp;
}

/* Render a "[tag] body" status line: bracketed tag in the gutter, body
 * on the right. Split out from xprintline_render because a tagged line
 * has no nick to parse and no highlight to consider — it is the one
 * shape where the caller already knows every part. */
static void
xprintline_render_tagged (GtkWidget *text, const char *tag, gsize tag_len,
                          gint16 tag_color, const char *body, gsize body_len)
{
    HxChatRun gutter[3] = {
        { "[", 1, HX_CHAT_INFO_BRACKET_COLOR, HX_CHAT_ATTR_NONE },
        { tag, (int)tag_len, tag_color, HX_CHAT_ATTR_NONE },
        { "]", 1, HX_CHAT_INFO_BRACKET_COLOR, HX_CHAT_ATTR_NONE },
    };
    HxChatRun body_run = HX_CHAT_RUN_PLAIN (body, (int)body_len);

    hx_chat_view_append_system_runs (text, gutter, 3, &body_run, 1, 0);
}

/* Phase 9.E (inline media): auto-fetch a media handle on arrival
 * and swap the styled placeholder row to a true inline-rendered
 * texture once decoding finishes. The ctx is the smallest thing
 * that can survive an entry being auto-trimmed mid-fetch — we
 * carry the chat cid + the per-chat token, then look the entry
 * up at callback time. Stale entry lookups are a quiet no-op:
 * the placeholder stays, the user can still click to view in
 * the dialog. */
struct hx_media_autofetch_ctx {
    guint32 cid;
    guint token;
    /* Glycin migration G.2: the async decode token returned by
     * inline_media_decode_async. NULL until the download
     * succeeds; the decode-done callback cancels (== frees)
     * it. Worth noting: a download success → decode kick-off
     * lands the ctx in the decoder's hands; cancellation from
     * a closed window goes through the decoder's cancel rather
     * than touching ctx directly. */
    gpointer decode_token;
};

/* Forward decl so on_inline_media_autofetch_done can reference
 * the decode-done callback before its definition further down. */
static void on_inline_media_autofetch_decoded (HxInlineMediaDecoded *decoded,
                                               gpointer user_data);

static void
on_inline_media_autofetch_done (struct htlc_conn *htlc,
                                const HxInlineMediaDownloadResult *result,
                                gpointer user_data)
{
    struct hx_media_autofetch_ctx *ctx = user_data;
    (void)htlc;

    if (!ctx) {
        return;
    }
    if (!result || !result->bytes) {
        /* Download failed — leave the styled placeholder up.
         * Click-to-view in the dialog will surface the same
         * spec error message via the Phase 9.D path. */
        debug_log ("media",
                   "inline-media auto-fetch failed cid=%u token=%u code=%u",
                   ctx->cid, ctx->token, result ? result->error_code : 0);
        g_free (ctx);
        return;
    }

    /* Glycin migration G.2: kick off the async decode. The
     * dialog (on_download_done in inline_media_dialog.c) uses
     * the same {0} caps and relies on the decoder's
     * fall-through to HX_MEDIA_DEFAULT_*; matching that
     * keeps the two paths consistent. The ctx is preserved
     * across the async hop — the decode-done callback frees
     * it.
     *
     * Synchronous-reject path: when the decoder bails before
     * scheduling async work (empty payload / cap exceeded /
     * sniff reject) it fires the callback synchronously AND
     * returns NULL. The callback frees `ctx`, so writing
     * ctx->decode_token after the call would be a UAF.
     * Capture into a local first and only thread it into ctx
     * when the call returned a real token. */
    HxInlineMediaCaps caps = { 0 };
    gpointer token = inline_media_decode_async (
        result->bytes->data, result->bytes->len, &caps,
        on_inline_media_autofetch_decoded, ctx);
    if (token) {
        ctx->decode_token = token;
    }
    /* The download result + ctx ownership cross to the decode
     * callback. Do NOT free `ctx` here. */
}

/* Glycin decode callback for the auto-fetch path (G.2). The
 * download already succeeded; this fires after the glycin
 * sandboxed loader either resolves a texture or returns an
 * error. The ctx is the same one the download callback
 * forwarded; we own it here and must free. */
static void
on_inline_media_autofetch_decoded (HxInlineMediaDecoded *decoded,
                                   gpointer user_data)
{
    struct hx_media_autofetch_ctx *ctx = user_data;
    if (!ctx) {
        return;
    }
    /* Release the cancel token if we still hold one — needed
     * even on the success path because cancel-after-completion
     * is also the canonical free function. */
    if (ctx->decode_token) {
        inline_media_decode_cancel (ctx->decode_token);
        ctx->decode_token = NULL;
    }

    if (!decoded->texture) {
        debug_log ("media",
                   "inline-media auto-fetch decode rejected (cid=%u "
                   "token=%u): %s",
                   ctx->cid, ctx->token,
                   decoded->error_message ? decoded->error_message : "unknown");
        inline_media_decoded_free (decoded);
        g_free (ctx);
        return;
    }

    /* gchat may have been freed (disconnect / chat-close) and
     * a fresh one with the same cid may even exist — in which
     * case hx_chat_view_media_mark returns NULL (the token
     * lives in the gchat's media table, which was rebuilt
     * fresh). The texture quietly drops. */
    struct gtkhx_chat *gchat = gchat_with_cid (hx_active_session (), ctx->cid);
    if (gchat && gchat->output) {
        HxChatMark *mark = hx_chat_view_media_mark (gchat->output, ctx->token);
        if (mark) {
            if (decoded->frames && decoded->frames->len > 1) {
                /* Animation (G.3). Install the frames array on
                 * the entry; xtext drives the per-frame tick
                 * from the array's per-element delay_ms. */
                debug_log ("media",
                           "inline-media auto-fetch swap-in animation cid=%u "
                           "token=%u %dx%d frames=%u",
                           ctx->cid, ctx->token,
                           gdk_texture_get_width (decoded->texture),
                           gdk_texture_get_height (decoded->texture),
                           decoded->frames->len);
                hx_chat_view_media_set_animation (gchat->output, mark,
                                                  decoded->frames);
            } else {
                debug_log ("media",
                           "inline-media auto-fetch swap-in cid=%u token=%u "
                           "%dx%d",
                           ctx->cid, ctx->token,
                           gdk_texture_get_width (decoded->texture),
                           gdk_texture_get_height (decoded->texture));
                hx_chat_view_media_set_texture (gchat->output, mark,
                                                decoded->texture);
            }
        }
    }

    inline_media_decoded_free (decoded);
    g_free (ctx);
}

/* Render an HxChatEvent (pre-parsed chat message) into a chat
 * window's xtext buffer. Bypasses the legacy hx_printf →
 * chat-log-line → xoutput_chat round-trip — the rcv.c emitter
 * already validated UTF-8 + ran the sender/body split, so this
 * just hands the slices to xprintline_render. Multi-line bodies
 * render the first line with the nick column; subsequent lines
 * fall through as continuation. */
void
output_chat_from_event (struct htlc_conn *htlc, HxChatEvent *e)
{
    struct gtkhx_chat *gchat;
    const char *body;
    gsize first_body_len;
    gchar *joined;
    (void)htlc;

    if (!e) {
        return;
    }
    struct chat *conv = chat_with_cid (sess_from_htlc (htlc), e->cid);
    gchat = hx_chat_view (conv);
    if (!gchat) {
        return;
    }

    if (e->sender_len == 0 && !e->is_info) {
        /* Server prose that didn't parse as "Nick: body" — emote,
         * announcement, etc. Render verbatim, splitting on
         * newlines so each visible line is its own xtext entry
         * (matches xoutput_chat behaviour for raw lines). */
        const char *cur = e->line;
        const char *end = e->line + e->line_len;
        while (cur < end) {
            const char *next_nl = memchr (cur, '\n', end - cur);
            gsize seg_len
                = next_nl ? (gsize)(next_nl - cur) : (gsize)(end - cur);
            hx_chat_view_append (gchat->output, cur, seg_len, 0);
            if (!next_nl) {
                break;
            }
            cur = next_nl + 1;
        }
        return;
    }

    /* Reassemble the body as one string, stripping the sender prefix
     * the server repeats on every line.
     *
     * One row for the whole message, not one row per line. The old
     * split was inherited from xtext, whose model was line-oriented;
     * the chat view's is message-oriented and wraps embedded newlines
     * itself. Splitting also made block-level markdown impossible —
     * a fenced code block spanning three lines arrived as three
     * separate messages, so the opening fence was an unterminated
     * block, the contents were a paragraph, and the closing fence was
     * another block. */
    body = e->line + e->body_off;
    joined = chat_join_body (body, e->body_len, e->line + e->sender_off,
                             e->sender_len, &first_body_len);

    /* Phase 9.E (inline media): the send-half defaults the chat
     * body to "[image]" when the user attaches without typing
     * a caption (see inline_media_attach.c / hx_send_chat_with_
     * media). With Phase E rendering the image inline the row
     * below, that "[image]" text is redundant and clutters the
     * chat. Suppress the body for this exact match; nick column
     * (e.g. `<misha>`) still renders. A user who actually typed
     * "[image]" as a caption alongside a real image attachment
     * loses that text — vanishingly rare; the inline image
     * conveys the same intent. */
    if (e->media && first_body_len == 7 && joined
        && memcmp (joined, "[image]", 7) == 0) {
        first_body_len = 0;
    }

    xprintline_render_parts (
        gchat->output, e->line + e->sender_off, e->sender_len,
        joined ? joined : "", first_body_len, e->is_info, e->is_self,
        HX_CHAT_INFO_COLOR,
        chat_speaker_for (e->cid, e->uid, e->line + e->sender_off,
                          e->sender_len, e->is_self));
    g_free (joined);
    joined = NULL;

    /* Phase 9.D + 9.E — inline-media row. When the chat carried
     * companion CHAT_MEDIA_ID + CHAT_MEDIA_TYPE fields (rcv.c
     * attached them to the event), allocate a per-chat token,
     * deep-copy the metadata into the gchat's media table, and emit
     * a media-typed row. The row's alt-text is the same NBSP-
     * joined `hxmedia:N`-embedding placeholder Phase 9.D shipped
     * — the existing inline_media_chat_word_click handler parses
     * the token off the clicked word and pops the dialog.
     *
     * Phase 9.E layers auto-fetch on top: when the server
     * advertises HTLC_CAP_INLINE_MEDIA, kick off
     * inline_media_download_start immediately so the texture
     * arrives without the user clicking. On decode success the
     * callback finds the entry via its token and swaps the
     * placeholder for the rendered image in place. On failure
     * the placeholder stays — click-to-view in the dialog
     * surfaces the same error message.
     *
     * mIRC colour 14 ("dark grey") still styles the placeholder
     * (visible until the texture lands) so it reads as a
     * subdued caption rather than chat text. */
    if (e->media) {
        guint token
            = hx_media_table_register (hx_chat_media_table (conv), e->media);
        char *placeholder
            = hx_chat_media_placeholder_clickable (e->media, token);
        if (placeholder) {
            hx_chat_view_append_media (gchat->output, NULL /* texture */,
                                       placeholder, token, 0 /* stamp */);
            g_free (placeholder);

            /* Auto-fetch. Cap-gated: on a server that didn't
             * negotiate the extension the placeholder never
             * gets bytes back (the upload-half of the
             * conversation isn't possible there either, so a
             * cap-less server emitting a media-bearing
             * relay row is itself a contract violation —
             * but defend just in case). The id buffer comes
             * from rcv.c via hx_chat_event_attach_media — the
             * deep-copy on event_attach_media keeps it valid
             * for the synchronous send call. */
            if (inline_media_cap_ok (htlc) && e->media->id_len > 0
                && e->media->id_len <= 65535) {
                struct hx_media_autofetch_ctx *ctx
                    = g_new0 (struct hx_media_autofetch_ctx, 1);
                ctx->cid = gchat->cid;
                ctx->token = token;
                hx_inline_media_download *dl = inline_media_download_start (
                    htlc, e->media->id, e->media->id_len,
                    on_inline_media_autofetch_done, ctx);
                if (!dl) {
                    g_free (ctx);
                }
            }
        }
    }

    /* incoming pchat lines mark the tab + Chat
     * panel needs-attention. Public chat (cid 0) skips this — the
     * public-chat tab is what the user sees by default and we
     * don't want the constant attention flag from a busy public
     * room. Self-echoes also skip. */
    if (e->cid != 0 && !e->is_self) {
        gtkhx_chat_tabs_set_attention_pchat (htlc, e->cid, TRUE);
    }
}

/* Chat-history extension: render a batch of historical entries
 * received from the server into chat `cid`'s output. Entries are
 * styled in mIRC colour 14 (grey) so they visually fade behind
 * live chat. Spec flags get specific treatment:
 *
 *   ACTION      → "* nick body" (mIRC /me-style)
 *   SERVER_MSG  → "*** body" with no nick column (info-line)
 *   DELETED     → "[message removed]" placeholder; nick + body
 *                 may be empty on the wire per spec
 *
 * Each batch is bracketed by "── chat history ──" and "── live
 * messages ──" info-line dividers so the boundary between
 * scrollback and the live stream is obvious. has_more is
 * accepted for API symmetry — Phase 2 doesn't use it (no "Load
 * older" row yet); Phase 3 will. */
void
output_chat_history_batch (struct htlc_conn *htlc, guint32 cid,
                           GPtrArray *entries, gboolean has_more)
{
    struct gtkhx_chat *gchat;
    (void)htlc;

    if (!entries) {
        return;
    }
    gchat = gchat_with_cid (sess_from_htlc (htlc), cid);
    if (!gchat) {
        return;
    }

    /* if render.loading was TRUE on entry, this batch
     * is the response to a "Load older" click — render with the
     * prepend path so the older entries land ABOVE the existing
     * buffer content. Otherwise this is the initial post-login
     * batch and we use the original append path.
     *
     * Capture the latch first because gchat->render.loading gets
     * cleared just below as part of the cursor-tracking bookkeeping
     * (Phase 3.1). */
    gboolean prepend_mode = gchat->render.loading;

    /* maintain the "oldest msgid we have rendered" anchor
     * and the has_more flag on the gtkhx_chat. A "Load older" click
     * uses oldest as BEFORE= cursor; rendering the Load-older row is
     * gated on has_more. We update both BEFORE bailing on the
     * empty-batch path so a server response of "no more entries"
     * (entries->len == 0, has_more == FALSE) reliably clears the
     * loading flag. */
    gchat->render.loading = FALSE;
    if (entries->len > 0) {
        for (guint i = 0; i < entries->len; i++) {
            HxHistoryEntry *e = g_ptr_array_index (entries, i);
            if (!e || e->message_id == 0) {
                continue;
            }
            if (gchat->render.oldest_msgid == 0
                || e->message_id < gchat->render.oldest_msgid) {
                gchat->render.oldest_msgid = e->message_id;
            }
        }
    }
    gchat->render.has_more = has_more;

    GtkWidget *view = gchat->output;

    /* evict the existing Load-older sentinel up front.
     * We'll re-insert a fresh one below if has_more is still true
     * on the new batch. Cleared regardless of hx_chat_view_remove's
     * return — a stale mark means the row was already gone, so
     * dropping our reference is the right thing either way. */
    if (gchat->render.load_older_ent) {
        hx_chat_view_remove (view, gchat->render.load_older_ent);
        gchat->render.load_older_ent = NULL;
    }

    /* Empty batch — server confirmed CAP_CHAT_HISTORY but has no
     * messages stored (yet). The Load-older eviction above
     * already handled the stale sentinel; has_more is FALSE here
     * by definition (server has nothing to point at), so we don't
     * re-add one and the buffer ends up clean. */
    if (entries->len == 0) {
        return;
    }

    /* ---- Render each entry through a single dispatcher ----------- *
     *
     * The textual shape is identical in initial vs. Load-older
     * mode — only the insert direction differs:
     *   initial:   append at tail (normal chat-output path)
     *   load-older: insert BEFORE gchat->render.anchor_ent
     *               (the opening "── chat history (N) ──" divider
     *               we saved on the initial render)
     *
     * Both walk entries in CHRONOLOGICAL order; insert_indent_before
     * places each new entry directly before the anchor, so the
     * last one inserted ends up closest to it. Net effect: the
     * Load-older block is chronologically ordered, oldest at the
     * top of the block, newest just above the original opening
     * divider. */
/* Every history row is drawn in the muted palette slot, so the runs
 * only ever differ in their text. HX_MUTED builds one. */
#define HX_MUTED(TEXT, LEN)                                                    \
    ((HxChatRun){ (TEXT), (LEN), HX_CHAT_PAL_HISTORY_MUTED, HX_CHAT_ATTR_NONE })

#define HX_RENDER(GUTTER, N_GUTTER, BODY, N_BODY, STAMP)                       \
    do {                                                                       \
        if (prepend_mode && gchat->render.anchor_ent) {                        \
            hx_chat_view_insert_runs_before (                                  \
                view, gchat->render.anchor_ent, HX_CHAT_SPEAKER_NONE,          \
                (GUTTER), (N_GUTTER), (BODY), (N_BODY), (STAMP));              \
        } else {                                                               \
            hx_chat_view_append_runs (view, HX_CHAT_SPEAKER_NONE, (GUTTER),    \
                                      (N_GUTTER), (BODY), (N_BODY), (STAMP));  \
        }                                                                      \
    } while (0)

    /* Opening "── chat history (N) ──" divider — initial batch
     * only. Load-older batches don't add a divider; the existing
     * anchor divider already marks the bottom of the cumulative
     * chat-history block.
     *
     * Save the appended row's mark as our anchor for any future
     * Load-older inserts — hx_chat_view_append_indent hands it back. */
    if (!prepend_mode) {
        const char *fmt
            = g_dngettext (NULL, "chat history (%u message)",
                           "chat history (%u messages)", entries->len);
        gchar *body = g_strdup_printf (fmt, entries->len);
        gchar *divider = g_strdup_printf ("─── %s ───", body);
        HxChatRun run = HX_MUTED (divider, (int)strlen (divider));
        gchat->render.anchor_ent = hx_chat_view_append_runs (
            view, HX_CHAT_SPEAKER_NONE, NULL, 0, &run, 1, 0);
        g_free (divider);
        g_free (body);
    }

    /* insert the "Load older messages" sentinel BEFORE
     * the entry loop runs. This keeps the sentinel pinned at the
     * TOP of the chat-history block in both modes:
     *
     *   initial mode: list state before this insert is
     *       [... server notices ..., anchor].
     *     Sentinel inserts before anchor → [..., sentinel, anchor].
     *     Then the entry loop APPENDS each entry to the tail, so
     *     entries land AFTER anchor: [..., sentinel, anchor,
     *     entry1, ..., entryN]. Sentinel ends up just above the
     *     opening divider, which is exactly what we want.
     *
     *   load-older mode: list state before this insert is
     *       [..., anchor, entry1, ..., entry50, live-divider].
     *     Sentinel inserts before anchor → [..., sentinel, anchor,
     *     entry1, ...]. Then the entry loop inserts each NEW older
     *     entry before anchor, which means they land BETWEEN
     *     sentinel and anchor (insert-before-anchor places the new
     *     entry directly above the anchor, pushing older inserts
     *     further from it): [..., sentinel, new1, new2, ..., newN,
     *     anchor, entry1, ...]. Sentinel STAYS at the top of the
     *     cumulative chat-history block.
     *
     * Earlier code did this AFTER the entry loop, which in
     * load-older mode meant the sentinel ended up between the
     * just-inserted new entries and the anchor — the bug Misha
     * noticed and asked us to fix.
     *
     * If we don't have an anchor (shouldn't happen in practice —
     * Load-older clicks can only fire after an initial render),
     * insert_indent_before falls back to head-insert. */
    if (has_more) {
        gchar *row = g_strdup_printf ("─── %s ───", hx_load_older_sentinel ());
        HxChatRun run = HX_MUTED (row, (int)strlen (row));
        gchat->render.load_older_ent = hx_chat_view_insert_runs_before (
            view, gchat->render.anchor_ent, HX_CHAT_SPEAKER_NONE, NULL, 0, &run,
            1, 0);
        g_free (row);
    }

    /* Walk entries forward (chronological) in both modes. */
    for (guint i = 0; i < entries->len; i++) {
        HxHistoryEntry *e = g_ptr_array_index (entries, i);
        if (!e) {
            continue;
        }

        time_t stamp = (time_t)e->timestamp;

        if (e->flags & HX_HISTORY_FLAG_DELETED) {
            /* Tombstone — placeholder text, no nick column. */
            const char *line = "[message removed]";
            HxChatRun run = HX_MUTED (line, (int)strlen (line));
            HX_RENDER (NULL, 0, &run, 1, stamp);
            continue;
        }

        if (e->flags & HX_HISTORY_FLAG_SERVER_MSG) {
            /* Server / admin broadcast. Render as info-line with
             * no nick column. */
            gchar *line
                = g_strdup_printf ("*** %s", e->message ? e->message : "");
            HxChatRun run = HX_MUTED (line, (int)strlen (line));
            HX_RENDER (NULL, 0, &run, 1, stamp);
            g_free (line);
            continue;
        }

        if (e->flags & HX_HISTORY_FLAG_ACTION) {
            /* /me emote. Render as "* nick body" — mIRC convention. */
            gchar *line = g_strdup_printf ("* %s %s", e->nick ? e->nick : "",
                                           e->message ? e->message : "");
            HxChatRun run = HX_MUTED (line, (int)strlen (line));
            HX_RENDER (NULL, 0, &run, 1, stamp);
            g_free (line);
            continue;
        }

        /* Standard message: two-column layout matching the live
         * chat path, but the whole thing rendered in the muted
         * theme-aware palette slot (HX_CHAT_PAL_HISTORY_MUTED = 37,
         * see chat.c::gtkhx_apply_theme_palette) instead of the
         * live palette. */
        gchar *nick_wrapped = g_strdup_printf ("<%s>", e->nick ? e->nick : "");
        const char *body_text = e->message ? e->message : "";
        HxChatRun gutter = HX_MUTED (nick_wrapped, (int)strlen (nick_wrapped));
        HxChatRun body_run = HX_MUTED (body_text, (int)strlen (body_text));
        HX_RENDER (&gutter, 1, &body_run, 1, stamp);
        g_free (nick_wrapped);
    }

    /* Closing "── live messages ──" divider follows the initial
     * batch only. Load-older batches don't add one — the closing
     * divider from the initial batch is still in place further
     * down. */
    if (!prepend_mode) {
        gchar *divider = g_strdup_printf ("─── %s ───", _ ("live messages"));
        HxChatRun run = HX_MUTED (divider, (int)strlen (divider));
        hx_chat_view_append_runs (view, HX_CHAT_SPEAKER_NONE, NULL, 0, &run, 1,
                                  0);
        g_free (divider);
    }

#undef HX_RENDER
#undef HX_MUTED
}

/* ----- Phase 3.3 — Load Older click handler --------------------- *
 *
 * The chat output xtext connects two word_click handlers:
 *   1. chat_history_word_click (this function) — filters on our
 *      HX_LOAD_OLDER_SENTINEL and fires the BEFORE= chat-history
 *      fetch.
 *   2. gtkurl_xtext_word_click — filters on URL-shaped words and
 *      pops the URL action menu on right/middle-click.
 *
 * Both run for every word_click. Each ignores words that aren't
 * theirs. gtkurl handles SECONDARY+MIDDLE only and we handle
 * PRIMARY only, so the two never collide on click semantics
 * either.
 *
 * The xtext widget that emitted the signal is passed in as
 * `xtext`; we walk the active session's chats (each model's view) to find which gchat owns
 * it (chat output, not pchat output userlist). */

static struct gtkhx_chat *
find_gchat_by_output (GtkWidget *xtext)
{
    HxChatRegistry *chats = hx_active_session ()->chats;
    if (!chats) {
        return NULL;
    }
    guint n = hx_chats_count (chats);
    for (guint i = 0; i < n; i++) {
        struct chat *c = hx_chats_get_at (chats, i);
        struct gtkhx_chat *g = hx_chat_view (c);
        if (g && g->output == xtext) {
            return g;
        }
    }
    return NULL;
}

void
chat_history_word_click (GtkWidget *xtext, char *word, GdkEvent *event,
                         gpointer data)
{
    guint button;
    GdkEventType evtype;
    struct gtkhx_chat *gchat;
    struct htlc_conn *htlc;
    (void)data;

    if (!event || !word || !*word) {
        return;
    }
    evtype = gdk_event_get_event_type (event);
    if (evtype != GDK_BUTTON_PRESS && evtype != GDK_BUTTON_RELEASE) {
        return;
    }
    button = gdk_button_event_get_button (event);
    /* Primary-button only. SECONDARY/MIDDLE flow through the URL
     * handler. */
    if (button != GDK_BUTTON_PRIMARY) {
        return;
    }
    if (strcmp (word, hx_load_older_sentinel ()) != 0) {
        return;
    }

    gchat = find_gchat_by_output (xtext);
    if (!gchat) {
        debug_log ("chat-history",
                   "Load-older click: no gchat matches the xtext widget");
        return;
    }
    htlc = hx_active_session ()->htlc;

    /* Guard: don't fire a second request while the first is
     * still in flight. The receive path (output_chat_history_batch)
     * clears render.loading on every batch — including empty ones,
     * so a "no more history" reply unsticks us. */
    if (gchat->render.loading) {
        debug_log ("chat-history",
                   "Load-older click: fetch already in flight for cid=%u",
                   gchat->cid);
        return;
    }

    /* CAP_CHAT_HISTORY is a hard prerequisite. hx_get_chat_history
     * already gates on this and returns FALSE, but check up front
     * so we don't even try to register the task. */
    if (!(hx_conn_has_cap (htlc, HTLC_CAP_CHAT_HISTORY))) {
        debug_log ("chat-history",
                   "Load-older click: server didn't negotiate "
                   "CAP_CHAT_HISTORY (caps=0x%" G_GINT64_MODIFIER "x)",
                   hx_conn_caps (htlc));
        return;
    }

    debug_log ("chat-history",
               "Load-older click: cid=%u, before=%" G_GUINT64_FORMAT,
               gchat->cid, gchat->render.oldest_msgid);

    /* Use the same per-batch count as the initial post-login pull
     * (gtkhx_prefs.chat_history_initial, default 50). If the user
     * has set initial=0 to suppress the auto-pull, fall back to a
     * 50-message floor here — the user explicitly engaged the
     * affordance, so they want a meaningful number of messages
     * back, not zero. Clamp positive values to uint16 range
     * (matches the spec's LIMIT field width). */
    int limit = gtkhx_prefs.chat_history_initial;
    if (limit <= 0) {
        limit = 50;
    }
    if (limit > 0xffff) {
        limit = 0xffff;
    }

    gchat->render.loading = TRUE;
    task_new (htlc, RCV_TASK_FN (rcv_task_chat_history),
              GUINT_TO_POINTER (gchat->cid), 0, "chat-history-older");
    if (!hx_get_chat_history (htlc, gchat->cid, gchat->render.oldest_msgid,
                              /*after=*/0, (guint16)limit)) {
        /* Sender refused (e.g. cap dropped mid-session). Roll back
         * the loading flag — the task we just registered will sit
         * unmatched but a future cap-bearing reply on that trans
         * id is extremely unlikely; tasks expire harmlessly. */
        gchat->render.loading = FALSE;
        debug_log ("chat-history",
                   "Load-older click: hx_get_chat_history refused");
        return;
    }

    /* Phase 3 follow-up B: swap the clickable sentinel for a
     * non-clickable "Loading..." row so the user gets immediate
     * feedback that the click registered, and a second click
     * before the response lands is silently a no-op (the loading
     * row's text doesn't match HX_LOAD_OLDER_SENTINEL, so this
     * handler bails on word-mismatch).
     *
     * The eviction-and-reinsert at the top of
     * output_chat_history_batch handles cleanup: it removes
     * whatever entry render.load_older_ent points at — clickable
     * or loading row — and then renders a fresh clickable
     * sentinel above the (now expanded) chat-history block if
     * the new batch still says has_more. So we just update the
     * pointer here, no extra teardown wiring needed.
     *
     * Text uses the same muted palette slot and the same divider
     * framing as the clickable row, so visually only the body text
     * changes between the two states. */
    if (gchat->render.load_older_ent) {
        gchar *loading_row
            = g_strdup_printf ("─── %s ───", hx_loading_older_sentinel ());
        HxChatRun run = { loading_row, (int)strlen (loading_row),
                          HX_CHAT_PAL_HISTORY_MUTED, HX_CHAT_ATTR_NONE };

        hx_chat_view_remove (gchat->output, gchat->render.load_older_ent);
        gchat->render.load_older_ent = hx_chat_view_insert_runs_before (
            gchat->output, gchat->render.anchor_ent, HX_CHAT_SPEAKER_NONE, NULL,
            0, &run, 1, 0);
        g_free (loading_row);
    }
}

/* Phase 9.D inline-media click handler. Filters on words that
 * contain the `hxmedia:N` token embedded by the placeholder
 * formatter (hx_chat_media_placeholder_clickable). Looks up the
 * token in the chat's media table and pops the dialog.
 * Same is_parallel-with-other-handlers pattern as
 * chat_history_word_click — primary-button only; URL handler
 * gets secondary/middle. */
void
inline_media_chat_word_click (GtkWidget *xtext, char *word, GdkEvent *event,
                              gpointer data)
{
    (void)data;
    guint button;
    GdkEventType evtype;

    if (!event || !word || !*word) {
        return;
    }
    evtype = gdk_event_get_event_type (event);
    if (evtype != GDK_BUTTON_PRESS && evtype != GDK_BUTTON_RELEASE) {
        return;
    }
    button = gdk_button_event_get_button (event);
    if (button != GDK_BUTTON_PRIMARY) {
        return;
    }

    guint token = 0;
    if (!hx_chat_media_parse_token (word, &token)) {
        return;
    }

    struct gtkhx_chat *gchat = find_gchat_by_output (xtext);
    struct chat *conv
        = gchat ? chat_with_cid (hx_active_session (), gchat->cid) : NULL;
    if (!conv) {
        return;
    }
    const HxChatMedia *m
        = hx_media_table_lookup (hx_chat_media_table (conv), token);
    if (!m) {
        debug_log ("media",
                   "inline-media click: token %u not found on gchat cid=%u",
                   token, gchat->cid);
        return;
    }

    debug_log ("media",
               "inline-media click: dispatch token=%u mime=%s id_len=%zu",
               token, m->mime ? m->mime : "?", m->id_len);
    inline_media_show_dialog (
        xtext, hx_active_session ()->htlc, m->id, m->id_len, m->mime,
        m->width_present ? m->width : 0, m->height_present ? m->height : 0,
        m->bytes_present ? m->bytes : 0);
}

void
xprintline (GtkWidget *text, guint32 cid, char *chat, size_t len,
            const char *tag, gint16 tag_color)
{
    char *valid;
    gsize valid_len;

    if ((ssize_t)len == -1) {
        len = strlen (chat);
    }
    if (len == 0) {
        len = 1;
    }

    /* chat / msg bytes from the wire arrive in whatever
     * encoding the server happened to use — historically Mac Roman
     * on Mac-OS-classic servers, occasionally Latin-1 from later
     * Unix forks, sometimes already UTF-8 on modern stacks. xtext
     * eventually hands the bytes to Pango, which asserts UTF-8 and
     * emits "Invalid UTF-8 string passed to pango_layout_set_text()"
     * for any 8-bit content. gtkhx_text_to_utf8 walks the
     * already-UTF-8 / Mac-Roman / fallback-to-substitute cascade and
     * always returns a valid-UTF-8 g_strdup'd copy. */
    valid = gtkhx_text_to_utf8 (chat, len, &valid_len);
    if (!valid) {
        /* Defensive — gtkhx_text_to_utf8 should never return NULL,
         * but if it does we'd rather drop the line than crash. */
        return;
    }

    /* timestamps move from inline "[HH:MM:SS] " prefix into
     * xtext's native left-column stamp. Two reasons:
     *
     *   1. HexChat-style drag-select: stamps are visually separate
     *      from the message body, so a drag-select on a chat line
     *      doesn't accidentally include the time. Settings →
     *      "Automatically include timestamps" toggles whether the
     *      stamp gets prepended on copy.
     *   2. No double-stamp duplication on copy. With the inline
     *      prefix, the autocopy_stamp toggle would yield
     *      "HH:MM:SS [HH:MM:SS] message" because xtext was
     *      prepending its own stamp on top of our inline one.
     *
     * xtext renders the per-entry stamp (ent->stamp, auto-set in
     * the view on append) iff indent mode is on &&
     * buf->time_stamp. Both are flipped on per-buffer at creation
     * time in chat.c / msg.c, and re-applied to live buffers when
     * the user toggles CFG_TIMESTAMP via Settings. */

    /* Find the parse facts xprintline_render needs: INFOPREFIX
     * branch, or "Nick: body" split, or neither (raw render).
     * This is the same work HxChatEvent does at signal-emit time —
     * duplicated here because log lines (hx_printf path) don't
     * carry an event. */
    gsize name_off = 0, name_len = 0;
    gsize body_off = 0, body_len = 0;
    gboolean is_info = FALSE;
    gboolean said_by_self = FALSE;
    gint16 info_color = HX_CHAT_INFO_COLOR;
    session *sess = hx_active_session ();
    const char *self_nick = (hx_conn_name (sess->htlc)[0] != '\0')
                                ? (const char *)hx_conn_name (sess->htlc)
                                : NULL;

    /* An info line is one the emitter *told* us is one, by naming a
     * gutter tag. This used to be recovered by searching the body for
     * the closing bytes of an escape wrapper GtkHx had written into it
     * moments earlier — a data structure round-tripped through a
     * presentation format. The tag and its colour are signal
     * parameters now (gtkhx_session.h, "chat-log-line"). */
    if (tag && *tag) {
        is_info = TRUE;
        info_color = tag_color;
        /* The tag isn't in `valid` at all; xprintline_render takes it
         * separately below. */
    }

    if (!is_info) {
        if (hx_chat_split_nick_body (valid, valid_len, &name_off, &name_len,
                                     &body_off, &body_len)) {
            if (self_nick && name_len > 0 && strlen (self_nick) == name_len
                && memcmp (valid + name_off, self_nick, name_len) == 0) {
                said_by_self = TRUE;
            }
        } else {
            name_len = 0;
        }
    }

    if (is_info) {
        /* Whole line is the body; the gutter comes from `tag`. */
        xprintline_render_tagged (text, tag, strlen (tag), info_color, valid,
                                  valid_len);
    } else {
        xprintline_render (text, valid, valid_len, name_off, name_len, body_off,
                           body_len, FALSE, said_by_self, info_color,
                           /* No wire uid: a log line never came from a
                            * chat message, so the nick can only be
                            * resolved by lookup — and it has to be
                            * looked up in *this* conversation, not
                            * always the public one. */
                           chat_speaker_for (cid, 0, valid + name_off, name_len,
                                             said_by_self));
    }

    g_free (valid);
}

static void
xoutput_chat (session *sess, guint32 cid, char *chat, const char *tag,
              gint16 tag_color)
{
    char *cr;
    struct gtkhx_chat *gchat;

    gchat = gchat_with_cid (sess, cid);

    if (!gchat) {
        return;
    }
    cr = strchr (chat, '\n');
    if (cr) {
        while (1) {
            xprintline (gchat->output, cid, chat, cr - chat, tag, tag_color);
            chat = cr + 1;
            if (*chat == 0) {
                break;
            }
            cr = strchr (chat, '\n');
            if (!cr) {
                xprintline (gchat->output, cid, chat, -1, tag, tag_color);
                break;
            }
        }
    } else {
        xprintline (gchat->output, cid, chat, -1, tag, tag_color);
    }
}

/* Phase 3 follow-up: hx_printf / hx_printf_prefix moved to
 * gtkhx_log.c so the model→view edge (model files calling these
 * by name to log info lines) now flows through the "chat-log-line"
 * signal on GtkhxSession, same as every other notification.
 *
 * The view-side handler below is what hx_output_chat used to do:
 * dispatch the buffer to xoutput_chat for the actual xtext write.
 * Connected in gtkhx_connect_signals at startup. */
void
chat_log_line_handler (GtkhxSession *emitter, struct htlc_conn *htlc, guint cid,
                       gpointer name, gint color, gpointer body,
                       gpointer user_data)
{
    xoutput_chat (sess_from_htlc (htlc), cid, (char *)body, (const char *)name,
                  (gint16)color);
}

/* Nick completion. The C tab_nick_comp / tab_nick_comp_next / nick_comp_chng /
 * public_chat_users_sorted machinery (~250 lines of raw-buffer + GSList work)
 * moved to Rust: the tested hxmodel::chat::complete over the
 * authoritative membership model for the chat the input belongs to (each
 * struct chat's member_model), reached via hx_nick_complete. The Rust version
 * also fixes two latent C bugs the old code had — Tab-cycling that computed the
 * next nick then discarded it, and mid-buffer caret math that landed one char
 * inside the name. (The old code always completed against the public chat;
 * private-chat inputs now complete against their own participants.)
 *
 * `pos` is a char offset (the GtkTextBuffer insert mark). `reverse` steps the
 * Tab-cycle backwards (Shift+Tab). */
int
tab_nick_comp (session *sess, void *member_model, char *text, gboolean reverse,
               int pos, GtkWidget *entry)
{
    char *ctext = NULL, *cinfo = NULL;
    int ccursor = -1;

    if (!member_model) {
        return 0;
    }
    if (!hx_nick_complete (member_model, text, (gsize)pos, reverse,
                           (gunichar)':', gtkhx_prefs.old_nickcompletion,
                           &ctext, &ccursor, &cinfo)) {
        return 0;
    }

    /* Ambiguous prefix → echo the candidate list to chat output (the Rust
     * completer returns it space-joined, case-insensitively sorted, with no
     * trailing space). */
    if (cinfo) {
        hx_printf (sess->htlc, 0, "%s", cinfo);
        g_free (cinfo);
    }
    if (ctext) {
        GtkTextBuffer *ebuf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (entry));
        gtk_text_buffer_set_text (ebuf, ctext, -1);
        if (ccursor >= 0) {
            GtkTextIter it;
            int total = gtk_text_buffer_get_char_count (ebuf);
            if (ccursor > total) {
                ccursor = total;
            }
            gtk_text_buffer_get_iter_at_offset (ebuf, &it, ccursor);
            gtk_text_buffer_place_cursor (ebuf, &it);
        }
        g_free (ctext);
    }
    return 0;
}

/* configure-event is gone in GTK 4. Window size for the
 * chat window is captured at hx_quit() in gtkhx.c gtkhx_save_window_positions
 * alongside position. */

/* chat_close retired. Public chat is a permanent resident of the toolbar's
 * center PanelGrid; the destroy-time re-parent trick that used to live here is
 * unneeded because the panel never goes away. The old wind_tmp / chat_hbox
 * staging file-statics are gone too — create_chat() now leaves the xtext
 * output + scrollbar parentless (ref-sunk) and the Rust content build
 * (chat.rs::build_content) packs them into the output frame. */

void
generate_colors (GtkWidget *widget)
{
    (void)widget;
    /* nothing to do — the colors[] palette is GdkRGBA now,
     * which has no .pixel field. The function is kept as a stub for
     * the existing caller in fe_init() and could be deleted as a
     * follow-up. */
}

void
create_chat (session *sess)
{
    struct gtkhx_chat *gchat;
    GtkWidget *text;
    GtkWidget *vscroll;

    /* g_malloc0 so the chat_history_draft slot starts at NULL —
     * the first Up press at the bottom-of-history "draft"
     * position g_free()s the previous draft before snapshotting
     * the current entry, and g_free on uninitialized memory is
     * undefined. */
    gchat = g_malloc0 (sizeof (struct gtkhx_chat));

    {
        gchar *fontname = pango_font_description_to_string (gtkhx_font_desc);
        text = hx_chat_view_new (colors, TRUE);
        hx_chat_view_set_font (text, fontname);
        g_free (fontname);
    }
    gtk_widget_set_can_focus (text, FALSE);
    hx_chat_view_set_word_wrap (text, gtkhx_prefs.word_wrap);
    hx_chat_view_set_urlcheck_function (text, word_check);
    hx_chat_view_set_max_lines (text, gtkhx_prefs.xbuf_max);
    /* enable the left-column timestamp rendering. The stamp draws iff
     * the view is in indent mode and the timestamp is on; the latter is
     * flipped from CFG_TIMESTAMP / gtkhx_prefs.timestamp. See xprintline
     * for the rationale (stamps rendered separately from message text,
     * so autocopy_stamp doesn't double-stamp). */
    hx_chat_view_set_indent (text, TRUE);
    hx_chat_view_set_time_stamp (text, gtkhx_prefs.timestamp);
    /* Allow the indent column to grow past its initial stamp-width
     * floor when the first message is appended: the auto-bump is gated
     * on the current indent being below this cap, so a zero default
     * makes the bump impossible and the nick column overlaps the
     * timestamp. 256 px is enough room for the stamp + a medium-length
     * nick without dominating the chat width. */
    hx_chat_view_set_max_indent (text, 256);
    /* Coalesce bursts from one speaker under a single nick. */
    hx_chat_view_set_group_gap (text, HX_CHAT_GROUP_GAP_DEFAULT);
    hx_chat_view_set_avatar_size (
        text, gtkhx_prefs.chat_avatars ? HX_CHAT_AVATAR_SIZE_DEFAULT : 0);
    g_signal_connect (text, "speaker-menu", G_CALLBACK (chat_speaker_menu),
                      GUINT_TO_POINTER (0));
    g_signal_connect (text, "word_click", G_CALLBACK (gtkurl_xtext_word_click),
                      NULL);
    /* chat-history "Load older" sentinel handler runs
     * alongside the URL handler — each self-filters on its own
     * word pattern (URL scheme prefix vs HX_LOAD_OLDER_SENTINEL)
     * and on a different button (URL = SECONDARY/MIDDLE,
     * load-older = PRIMARY) so they never collide. */
    g_signal_connect (text, "word_click", G_CALLBACK (chat_history_word_click),
                      NULL);
    /* Phase 9.D inline-media click handler — same coexistence
     * pattern as chat_history (different word patterns, same
     * primary-button discipline). */
    g_signal_connect (text, "word_click",
                      G_CALLBACK (inline_media_chat_word_click), NULL);

    vscroll = gtk_scrollbar_new (GTK_ORIENTATION_VERTICAL,
                                 hx_chat_view_get_vadjustment (text));

    /* Keep the xtext output + its scrollbar alive parentless (ref-sunk)
     * until the Rust content build (chat.rs::build_content) reads them back
     * via hx_gchat_output / hx_gchat_vscroll and packs them into the output
     * frame — same handoff pchat_new uses. The old chat_hbox / wind_tmp
     * staging (a throwaway GtkWindow that parented the xtext early so a
     * GTK 1.2/2 realize could fire before the panel opened) is gone: under
     * GTK 4 widgets are windowless and take appended output lines while
     * parentless, so no early parent is needed. */
    g_object_ref_sink (text);
    g_object_ref_sink (vscroll);

    gchat->cid = 0;
    gchat->subject = 0;
    gchat->output = text;
    gchat->userlist = 0;
    gchat->vscroll = vscroll;
    gchat->window = 0;
    gchat->input = 0;
    gchat->render.oldest_msgid = 0;
    gchat->render.has_more = FALSE;
    gchat->render.loading = FALSE;
    gchat->render.anchor_ent = NULL;
    gchat->render.load_older_ent = NULL;

    /* Attach the public-chat view to its model (seeded eagerly by
     * chats_init, so chat_with_cid(sess, 0) is non-NULL here). */
    hx_chat_set_view (chat_with_cid (sess, 0), gchat);
}

static void
change_subject (GtkWidget *widget, gpointer data)
{
    const char *subject;

    subject = gtk_editable_get_text (GTK_EDITABLE (widget));
    hx_change_subject (hx_active_session ()->htlc, GPOINTER_TO_INT (data),
                       (char *)subject);
}

/* Clear the public-chat gchat's pointers into its (now-dead) widget tree.
 * Connected to the panel content box's "destroy" from the Rust content build
 * (chat.rs::build_content). The public-chat gchat is persistent (session-
 * owned), so if the dock embed fails — the should-never-happen "toolbar dock
 * not built yet", where the Rust shell has dock_bridge destroy the content —
 * or at app teardown, these must be nulled so nothing dereferences freed
 * widgets afterwards. */
void
gtkhx_chat_clear_content_ptrs (struct gtkhx_chat *gchat)
{
    if (!gchat) {
        return;
    }
    gchat->window = NULL;
    gchat->input = NULL;
    gchat->subject = NULL;
    gchat->media_attach_btn = NULL;
}

/* Create the public-chat gchat's C-coupled leaf widgets — the subject entry,
 * the input text view (+ its key controller / qdata), and the inline-media
 * attach button — and store them on the gchat. The xtext output + scrollbar
 * were already made by create_chat() at session init; the surrounding box tree
 * + the AdwTabView are assembled in Rust (chat.rs::build_content), which reads
 * every leaf back via the hx_gchat_* accessors. Mirrors gtkhx_pchat_new. */
struct gtkhx_chat *
gtkhx_chat_build_leaves (session *sess)
{
    struct gtkhx_chat *gchat;

    g_return_val_if_fail (sess != NULL, NULL);

    gchat = gchat_with_cid (sess, 0);
    /* chats_init seeds cid=0 (public chat) on session bring-up; this only
     * fires post-login, so the lookup is invariant non-NULL. Bail loudly on
     * the invariant break — we can't build a chat panel without the struct. */
    if (!gchat) {
        g_warning ("gtkhx_chat_build_leaves: no public-chat gchat — skipping");
        return NULL;
    }

    /* Arm the A/B benchmark if GTKHX_CHATVIEW_BENCH is set. Here rather
     * than in create_chat because the tick callback it installs needs a
     * frame clock, which the view only has once it is in a mapped
     * window — and build_leaves runs on the way there. No-op otherwise.
     * See chat_bench.c. */
    hx_chat_bench_maybe_start (gchat->output);

    gchat->subject = gtk_entry_new ();
    {
        struct chat *pub = chat_with_cid (sess, 0);
        gtk_editable_set_text (GTK_EDITABLE (gchat->subject),
                               pub ? hx_chat_subject (pub) : "");
    }
    gtkhx_apply_text_style (gchat->subject);
    g_signal_connect (gchat->subject, "activate", G_CALLBACK (change_subject),
                      GINT_TO_POINTER (0));

    gchat->input = gtk_text_view_new ();
    /* Theme monospace via gtk_text_view_set_monospace (gtkhx_apply_input_font).
     * We deliberately do NOT use the Settings font here — applying it triggered
     * an unresolved ascender-ink clip on newly typed glyphs at small Monospace
     * sizes. See gtkhx_apply_input_font in gtkhx.c. Colours still track the
     * active GtkHx theme via the .gtkhx-input class. */
    gtkhx_apply_input_font (gchat->input);
    gtkhx_apply_input_style (gchat->input);
    gtkhx_chat_input_attach (
        gchat->input, sess, gchat->cid,
        hx_chat_input_history (chat_with_cid (sess, gchat->cid)));
    gtk_text_view_set_editable (GTK_TEXT_VIEW (gchat->input), TRUE);
    gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (gchat->input), GTK_WRAP_WORD);
    /* Inner margins so the text doesn't sit flush against the rounded-corner
     * frame (the left+top corner used to clip the leading character). */
    gtk_text_view_set_left_margin (GTK_TEXT_VIEW (gchat->input), 6);
    gtk_text_view_set_right_margin (GTK_TEXT_VIEW (gchat->input), 6);
    gtk_text_view_set_top_margin (GTK_TEXT_VIEW (gchat->input), 4);
    gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (gchat->input), 4);

    /* Phase 9.C inline-media attach. Initially hidden; the
     * setbtns->inline_media_attach_refresh_all_chats path flips it visible once
     * the LOGIN reply confirms CAP_INLINE_MEDIA. Most servers don't ship the
     * extension, so it stays hidden there — same shape as the voice gating. */
    gchat->media_attach_btn
        = hx_inline_media_attach_button_new (gchat, sess->htlc);

    return gchat;
}

void
gtkhx_chat_after_embed (session *sess)
{
    struct gtkhx_chat *gchat;
    HxPanel *panel;

    g_return_if_fail (sess != NULL);

    /* Carry the session on the dock panel for the panel-level handlers
     * (the chat_tabs close dispatcher reads it back). The panel is
     * available from the registry once dock_bridge embedded us. */
    panel = hx_panel_registry_lookup (HX_PANEL_ID_CHAT);
    if (panel != NULL) {
        g_object_set_data (G_OBJECT (panel), "sess", sess);
    }

    hx_panel_mark_constructed (HX_PANEL_ID_CHAT);

    gchat = gchat_with_cid (sess, 0);
    if (gchat != NULL && gchat->input != NULL) {
        gtk_widget_grab_focus (gchat->input);
    }
}

struct gtkhx_chat *
pchat_new (session *sess, struct chat *chat)
{
    GtkWidget *text;
    GtkWidget *vscroll;
    GtkWidget *subject;
    struct gtkhx_chat *gchat;

    /* g_malloc0 so the chat_history_draft slot starts at NULL —
     * the first Up press at the bottom-of-history "draft"
     * position g_free()s the previous draft before snapshotting
     * the current entry, and g_free on uninitialized memory is
     * undefined. */
    gchat = g_malloc0 (sizeof (struct gtkhx_chat));

    {
        gchar *fontname = pango_font_description_to_string (gtkhx_font_desc);
        text = hx_chat_view_new (colors, TRUE);
        hx_chat_view_set_font (text, fontname);
        g_free (fontname);
    }
    hx_chat_view_set_word_wrap (text, gtkhx_prefs.word_wrap);
    hx_chat_view_set_urlcheck_function (text, word_check);
    hx_chat_view_set_max_lines (text, gtkhx_prefs.xbuf_max);
    /* view-native timestamps — see the matching call in
     * create_chat_window above for the rationale. */
    hx_chat_view_set_indent (text, TRUE);
    hx_chat_view_set_time_stamp (text, gtkhx_prefs.timestamp);
    hx_chat_view_set_max_indent (text, 256);
    /* Coalesce bursts from one speaker under a single nick. */
    hx_chat_view_set_group_gap (text, HX_CHAT_GROUP_GAP_DEFAULT);
    hx_chat_view_set_avatar_size (
        text, gtkhx_prefs.chat_avatars ? HX_CHAT_AVATAR_SIZE_DEFAULT : 0);
    g_signal_connect (text, "speaker-menu", G_CALLBACK (chat_speaker_menu),
                      GUINT_TO_POINTER (hx_chat_cid (chat)));
    g_signal_connect (text, "word_click", G_CALLBACK (gtkurl_xtext_word_click),
                      NULL);
    /* chat-history "Load older" sentinel handler runs
     * alongside the URL handler — each self-filters on its own
     * word pattern (URL scheme prefix vs HX_LOAD_OLDER_SENTINEL)
     * and on a different button (URL = SECONDARY/MIDDLE,
     * load-older = PRIMARY) so they never collide. */
    g_signal_connect (text, "word_click", G_CALLBACK (chat_history_word_click),
                      NULL);
    /* Phase 9.D inline-media click handler — same coexistence
     * pattern as chat_history (different word patterns, same
     * primary-button discipline). */
    g_signal_connect (text, "word_click",
                      G_CALLBACK (inline_media_chat_word_click), NULL);

    vscroll = gtk_scrollbar_new (GTK_ORIENTATION_VERTICAL,
                                 hx_chat_view_get_vadjustment (text));

    subject = gtk_entry_new ();
    gtkhx_apply_text_style (subject);

    /* pchat_new is only called from create_pchat_window, which
     * builds the visible HxUserListView itself further down — the
     * sidebar's userlist starts NULL here and gets filled in
     * there. */
    g_object_ref_sink (text);
    g_object_ref_sink (vscroll);
    g_object_ref_sink (subject);

    gchat->cid = hx_chat_cid (chat);
    gchat->output = text;
    gchat->vscroll = vscroll;
    gchat->subject = subject;
    gchat->userlist = NULL;
    gchat->render.oldest_msgid = 0;
    gchat->render.has_more = FALSE;
    gchat->render.loading = FALSE;
    gchat->render.anchor_ent = NULL;
    gchat->render.load_older_ent = NULL;
    /* Attach this view to its model (the `chat` arg, already in
     * sess->chats) instead of a separate gchats entry. */
    hx_chat_set_view (chat, gchat);

    return gchat;
}

/* Invoked from the chat_tabs close-page dispatcher with the cid of
 * the tab being closed. Look up the view via gchat_with_cid; if it's
 * still there, send the protocol-side leave and tear down the gchat
 * struct (which removes it from the hashtable via gchat_delete's
 * value-destroy notify). */
static void
pchat_close (struct htlc_conn *htlc, guint32 cid)
{
    /* The tab strip hands back the connection the tab belonged to, so the
     * leave goes to that server and the teardown finds the right
     * conversation. It used to get a bare cid and resolve through the
     * focused session, which at two connections would have left the wrong
     * private chat — or sent a leave to a server that was never in it. */
    session *sess = sess_from_htlc (htlc);
    struct gtkhx_chat *gchat = sess ? gchat_with_cid (sess, cid) : NULL;

    if (gchat == NULL) {
        return;
    }

    /* Unparent the emoji typeahead popover from the input before the
     * tab's content tree is disposed — GtkTextView::dispose otherwise
     * spins forever on the foreign popover child and freezes the app.
     * Same fix as the PM tab close path. */
    if (gchat->input) {
        hx_emoji_typeahead_detach (gchat->input);
    }

    hx_part_chat (sess->htlc, cid);
    gchat_delete (sess, gchat);
}

/* pure view function — just paints the
 * new subject into the chat-window subject entry. The broadcast
 * handler in rcv.c (hx_rcv_chat_subject) is responsible for the
 * 'Subject Changed to X' chat log line; the initial-subject
 * discovery path in rcv_task_user_list calls this too but without
 * the announce, since "joined a chat that already had a subject"
 * isn't a subject change from the user's perspective.
 *
 * htlc is unused here — only kept for vtable signature uniformity
 * with the other output_functions members. */
void
output_chat_subject (struct htlc_conn *htlc, guint32 cid, char *buf)
{
    session *sess = sess_from_htlc (htlc);
    struct gtkhx_chat *gchat = gchat_with_cid (sess, cid);
    (void)htlc;

    if (!gchat || !gchat->subject) {
        return;
    }
    /* buf comes from the conversation subject — set by hx_rcv_chat_subject
     * (HTLS_HDR_CHAT_SUBJECT broadcast) and by the
     * HTLS_DATA_CHAT_SUBJECT branch of rcv_task_user_list
     * (initial-subject-discovery). Both paths copy the raw wire
     * bytes verbatim, so a Mac-Roman-only server (Heidrun's Inn:
     * 0xd5 = curly apostrophe in "Heidrun's Inn") delivers
     * bytes that aren't valid UTF-8.
     *
     * Pango's editable widget accepts only valid UTF-8 and emits
     * a runtime warning otherwise — gtkhx_text_to_utf8 walks the
     * common encodings (UTF-8 / Latin-1 / Mac Roman) and returns
     * a fresh g_malloc'd UTF-8 copy that we hand to GTK. The
     * chat-line log path already runs through hx_printf which
     * does the same conversion; this matches the subject widget
     * to that path. */
    gsize utf8_len = 0;
    char *utf8 = gtkhx_text_to_utf8 (buf, strlen (buf), &utf8_len);
    gtk_editable_set_text (GTK_EDITABLE (gchat->subject), utf8 ? utf8 : buf);
    g_free (utf8);
}

/* View-side handler for the "chat-subject-notice" signal — the "Subject Changed
 * to: <subject>" chat-output line for a real subject change (the chat-subject
 * signal already updated the subject bar). The Rust chat-subject receive handler
 * (hxhandlers::recv::chat, hx_rcv_chat_subject) emits it; the gettext + INFOPREFIX live
 * here on the view side, same as every other model→view notification. Connected
 * in gtkhx_connect_signals at startup. */
void
chat_subject_notice_handler (GtkhxSession *emitter, struct htlc_conn *htlc,
                             guint cid, gpointer subject, gpointer user_data)
{
    hx_printf_prefix (htlc, cid, INFOPREFIX, "%s: %s\n",
                      _ ("Subject Changed to"), (const char *)subject);
}

/* View-side handler for the "user-notice" signal — the roster notice lines
 * (join / parts / rename) the Rust user-roster receive handlers (hxhandlers::recv::user)
 * emit. The gettext + INFOPREFIX formatting and the showjoin-pref gate live
 * here on the view side, same as every other model→view notification; the model
 * just says "user X joined chat C". `old_name` is NULL except for a rename.
 * Connected in gtkhx_connect_signals at startup. */
void
user_notice_handler (GtkhxSession *emitter, struct htlc_conn *htlc, guint cid,
                     guint kind, gpointer name, gpointer old_name,
                     gpointer user_data)
{
    switch (kind) {
    case HX_USER_NOTICE_JOIN:
        if (gtkhx_prefs.showjoin) {
            hx_printf_prefix (htlc, cid, INFOPREFIX, _ ("join: %s\n"),
                              (const char *)name);
        }
        break;
    case HX_USER_NOTICE_PART:
        if (gtkhx_prefs.showjoin) {
            hx_printf_prefix (htlc, cid, INFOPREFIX, _ ("parts: %s \n"),
                              (const char *)name);
        }
        break;
    case HX_USER_NOTICE_RENAME:
        hx_printf_prefix (htlc, cid, INFOPREFIX,
                          _ ("%1$s is now known as %2$s\n"),
                          (const char *)old_name, (const char *)name);
        break;
    default:
        break;
    }
}

/* hx_reject_chat + output_chat_invitation (the incoming chat-invitation
 * dialog) moved to Rust: gtkhx-ui/src/chat_invite.rs presents the AdwAlertDialog
 * and routes Join/Decline to the hxhandlers::send::chat senders (hx_chat_join /
 * hx_reject_chat). */

/* pchat_update_trans was a configure-event handler that
 * forced an xtext refresh on every resize so transparency would track
 * the new window position. configure-event is gone in GTK 4 and
 * Wayland doesn't expose true window-relative transparency anyway. */

/* Content-build helpers for the Rust private-chat window (gtkhx-ui `pchat`).
 * create_pchat_window moved to Rust: it assembles the tab layout around the
 * C-coupled leaf widgets these helpers build. Mirrors msg.c's create_msg +
 * hx_msgwin_* accessor seam. */

GtkWidget *
hx_gchat_output (struct gtkhx_chat *g)
{
    return g ? g->output : NULL;
}
GtkWidget *
hx_gchat_vscroll (struct gtkhx_chat *g)
{
    return g ? g->vscroll : NULL;
}
GtkWidget *
hx_gchat_input (struct gtkhx_chat *g)
{
    return g ? g->input : NULL;
}
GtkWidget *
hx_gchat_subject (struct gtkhx_chat *g)
{
    return g ? g->subject : NULL;
}
GtkWidget *
hx_gchat_media_btn (struct gtkhx_chat *g)
{
    return g ? g->media_attach_btn : NULL;
}
void
hx_gchat_set_window (struct gtkhx_chat *g, GtkWidget *w)
{
    if (g) {
        g->window = w;
    }
}
GtkWidget *
hx_gchat_window (struct gtkhx_chat *g)
{
    return g ? g->window : NULL;
}
guint32
hx_gchat_cid (struct gtkhx_chat *g)
{
    return g ? g->cid : 0;
}
struct _HxUserListView *
hx_gchat_userlist (struct gtkhx_chat *g)
{
    return g ? g->userlist : NULL;
}

/* Create the pchat gchat + its C-coupled leaf widgets: output/vscroll (via
 * pchat_new), plus the input text view, subject entry, and inline-media
 * button. Rust assembles the layout around them. */
struct gtkhx_chat *
gtkhx_pchat_new (struct htlc_conn *htlc, struct chat *chat)
{
    session *sess = sess_from_htlc (htlc);
    struct gtkhx_chat *gchat = pchat_new (sess, chat);

    /* Reuse the subject entry pchat_new already created + styled + ref-sank —
     * overwriting gchat->subject with a fresh gtk_entry_new would leak that
     * one. Just populate its text and wire the activate handler. */
    gtk_editable_set_text (GTK_EDITABLE (gchat->subject),
                           hx_chat_subject (chat));
    g_signal_connect (gchat->subject, "activate", G_CALLBACK (change_subject),
                      GINT_TO_POINTER (hx_chat_cid (chat)));

    gchat->input = gtk_text_view_new ();
    gtkhx_apply_input_font (gchat->input);
    gtkhx_apply_input_style (gchat->input);
    gtkhx_chat_input_attach (
        gchat->input, sess, gchat->cid,
        hx_chat_input_history (chat_with_cid (sess, gchat->cid)));
    gtk_text_view_set_editable (GTK_TEXT_VIEW (gchat->input), TRUE);
    gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (gchat->input), GTK_WRAP_WORD);
    gtk_text_view_set_left_margin (GTK_TEXT_VIEW (gchat->input), 6);
    gtk_text_view_set_right_margin (GTK_TEXT_VIEW (gchat->input), 6);
    gtk_text_view_set_top_margin (GTK_TEXT_VIEW (gchat->input), 4);
    gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (gchat->input), 4);

    gchat->media_attach_btn
        = hx_inline_media_attach_button_new (gchat, sess->htlc);
    return gchat;
}

/* Build the pchat user sidebar (HxUserListView + action buttons + voice
 * panel). Returns the user_vbox for the Rust layout to pack into the hpane. */
GtkWidget *
gtkhx_pchat_user_sidebar (struct htlc_conn *htlc, struct chat *chat)
{
    session *sess = sess_from_htlc (htlc);
    struct gtkhx_chat *gchat = gchat_with_cid (sess, hx_chat_cid (chat));
    GtkWidget *user_vbox, *userframe, *topframe, *scroll, *hbuttonbox;
    GtkWidget *msg_btn, *kick_btn, *ban_btn, *info_btn, *igno_btn, *chat_btn;
    GtkWidget *pix;
    GdkPixmap *icon;

    if (!gchat) {
        return NULL;
    }

    /* HxUserListView GObject (STYLE_CHAT — 18-px rows, 1.0× scale,
     * 36-px text offset). The view installs its own right-click
     * gesture and double-click → msgwin handler, so the older
     * users_attach_click_gesture wiring is gone. Selection lives
     * on the view's GtkSingleSelection — the action buttons below
     * pull it via hx_user_list_view_get_selected_user. */
    gchat->userlist
        = hx_user_list_view_new (sess, gchat->cid, HX_USER_LIST_STYLE_CHAT);

    if (!users_font_desc) {
        users_font_desc = pango_font_description_from_string ("Sans 10");
    }
    gtkhx_refresh_userlist_css (users_font_desc);

    msg_btn = gtk_button_new ();
    icon = (GdkPixmap *)gtkhx_icon_load (
        "/com/nasledov/gtkhx/pixmaps/message.png");
    pix = gtkhx_image_new_from_pixbuf ((GdkPixbuf *)icon);
    gtkhx_widget_set_child (msg_btn, pix);
    g_signal_connect (msg_btn, "clicked", G_CALLBACK (view_msg_btn),
                      gchat->userlist);
    gtk_widget_set_tooltip_text (msg_btn, _ ("Msg"));
    /* gtkhx_image_new_from_pixbuf wraps the pixbuf in a texture
     * that holds its own ref on the pixbuf bytes via the GBytes
     * free-func — the texture stays valid for the GtkImage's
     * lifetime independent of our local reference. Drop ours so
     * the gtkhx_icon_load ref isn't leaked. The if-guard handles
     * the gtkhx_icon_load-returned-NULL path (g_object_unref isn't
     * NULL-safe). */
    if (icon) {
        g_object_unref (icon);
    }
    icon = NULL;
    pix = NULL;

    kick_btn = gtk_button_new ();
    icon
        = (GdkPixmap *)gtkhx_icon_load ("/com/nasledov/gtkhx/pixmaps/kick.png");
    pix = gtkhx_image_new_from_pixbuf ((GdkPixbuf *)icon);
    gtkhx_widget_set_child (kick_btn, pix);
    g_signal_connect (kick_btn, "clicked", G_CALLBACK (view_kick_btn),
                      gchat->userlist);
    gtk_widget_set_tooltip_text (kick_btn, _ ("Kick"));
    if (icon) {
        g_object_unref (icon);
    }
    icon = NULL;
    pix = NULL;

    info_btn = gtk_button_new ();
    icon
        = (GdkPixmap *)gtkhx_icon_load ("/com/nasledov/gtkhx/pixmaps/info.png");
    pix = gtkhx_image_new_from_pixbuf ((GdkPixbuf *)icon);
    gtkhx_widget_set_child (info_btn, pix);
    g_signal_connect (info_btn, "clicked", G_CALLBACK (view_info_btn),
                      gchat->userlist);
    gtk_widget_set_tooltip_text (info_btn, _ ("User Info"));
    if (icon) {
        g_object_unref (icon);
    }
    icon = NULL;
    pix = NULL;

    ban_btn = gtk_button_new ();
    g_signal_connect (ban_btn, "clicked", G_CALLBACK (view_ban_btn),
                      gchat->userlist);
    icon = (GdkPixmap *)gtkhx_icon_load ("/com/nasledov/gtkhx/pixmaps/ban.png");
    pix = gtkhx_image_new_from_pixbuf ((GdkPixbuf *)icon);
    gtkhx_widget_set_child (ban_btn, pix);
    gtk_widget_set_tooltip_text (ban_btn, _ ("Ban"));
    if (icon) {
        g_object_unref (icon);
    }
    icon = NULL;
    pix = NULL;

    chat_btn = gtk_button_new ();
    gtk_widget_set_tooltip_text (chat_btn, _ ("Private Chat"));
    g_signal_connect (chat_btn, "clicked", G_CALLBACK (view_chat_btn),
                      gchat->userlist);
    icon
        = (GdkPixmap *)gtkhx_icon_load ("/com/nasledov/gtkhx/pixmaps/chat.png");
    pix = gtkhx_image_new_from_pixbuf ((GdkPixbuf *)icon);
    gtkhx_widget_set_child (chat_btn, pix);
    if (icon) {
        g_object_unref (icon);
    }
    icon = NULL;
    pix = NULL;

    igno_btn = gtk_button_new ();
    gtk_widget_set_tooltip_text (igno_btn, _ ("Ignore"));
    g_signal_connect (igno_btn, "clicked", G_CALLBACK (view_igno_btn),
                      gchat->userlist);
    icon = (GdkPixmap *)gtkhx_icon_load (
        "/com/nasledov/gtkhx/pixmaps/ignore.png");
    pix = gtkhx_image_new_from_pixbuf ((GdkPixbuf *)icon);
    gtkhx_widget_set_child (igno_btn, pix);
    if (icon) {
        g_object_unref (icon);
    }
    icon = NULL;
    pix = NULL;

    topframe = gtk_frame_new (0);
    gtk_widget_set_size_request (topframe, -1, 30);

    hbuttonbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtkhx_widget_set_child (topframe, hbuttonbox);

    gtkhx_box_pack (hbuttonbox, msg_btn, 0, 0, 0);

    gtkhx_box_pack (hbuttonbox, chat_btn, 0, 0, 2);
    gtkhx_box_pack (hbuttonbox, info_btn, 0, 0, 0);
    gtkhx_box_pack (hbuttonbox, kick_btn, 0, 0, 2);
    gtkhx_box_pack (hbuttonbox, ban_btn, 0, 0, 0);
    gtkhx_box_pack (hbuttonbox, igno_btn, 0, 0, 0);

#ifdef HAVE_VOICE
    /* Voice Join/Leave + Mute icon controls for this private room,
     * scoped to hx_chat_cid (chat). Sit at the end of the user-list action
     * bar; hidden entirely unless the server echoed HTLC_CAP_VOICE. */
    gchat->voice_panel = voice_panel_new (sess, hx_chat_cid (chat));
    gtkhx_box_pack (hbuttonbox, gchat->voice_panel, 0, 0, 4);
#endif

    user_vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    userframe = gtk_frame_new (0);

    scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_widget_set_size_request (scroll, 232, 232);
    gtkhx_widget_set_child (scroll,
                            hx_user_list_view_get_widget (gchat->userlist));
    gtkhx_widget_set_child (userframe, scroll);

    gtkhx_box_pack (user_vbox, topframe, 0, 0, 0);
    gtkhx_box_pack (user_vbox, userframe, 1, 1, 0);

    return user_vbox;
}

void
hx_clear_chat (struct htlc_conn *htlc, guint32 cid, int subj)
{
    session *sess = sess_from_htlc (htlc);
    struct gtkhx_chat *gchat = gchat_with_cid (sess, cid);

    /* gchat_with_cid is the UI-side hashtable lookup. If the chat
     * window was closed but the server still pushes a clear-chat
     * for this cid (or the cid drifts during reconnect), gchat is
     * NULL — nothing to clear, just return. */
    if (!gchat) {
        return;
    }
    hx_chat_view_clear (gchat->output);
    if (hx_panel_was_constructed (HX_PANEL_ID_CHAT)) {
        if (subj) {
            gtk_editable_set_text (GTK_EDITABLE (gchat->subject), "");
        }
    }
}
