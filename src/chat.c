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
#include <gdk/gdkkeysyms.h>
#include <sys/types.h>
#include <ctype.h>
#include <time.h>
#include "hx.h"
#include "hxconn.h"
#include "gtkhx_session.h"
#include "session_registry.h" /* hx_session_count, hx_session_at */
#include "gtkhx_theme.h"
#include "gtkhx_log.h"
#include "panel_registry.h"
#include "toolbar.h"
#include "network.h"
#include "hotline_proto.h"
#include "proto_helpers.h" /* struct hx_chunk (stack-allocated below) */
#include "inline_media_attach.h"
#include "inline_media_dialog.h"
#include "gtkutil.h"
#include "gtkhx_icon.h" /* gtkhx_icon_load — theme-bundled chrome icons */
#include "chat_members.h"
#include <rotulus.h>
#include "users.h"
#ifdef HAVE_VOICE
#include "voice_panel.h"
#endif
#include "users_view.h"
#include "gtkhx.h"
#include "chat.h"
#include "msg.h"
#include "sound.h"
#include "chat_members.h"
#include "chat_tabs.h"
#include "emoji.h"
#include "connect.h"
#include "debug.h"
#include "compat.h" /* _() i18n macro */

/* gtkhx-ui bench module: runs the GTKHX_BENCH scenarios, if any. */
extern void hx_bench_maybe_start (GtkWidget *chat_view);

/* The per-conversation view: its widgets, of which the output's rows are
 * gtkhx-ui chat_output.rs's to draw. Opaque outside this file (see the
 * forward decl in session.h), so a stray gchat->field access elsewhere is
 * a compile error. It hangs off the matching model (struct chat::view) in
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

    /* Inline-media extension (Phase 9.C UI). Pointer to the
     * paperclip 'Attach Image' button in this chat's input row.
     * Visibility is gated on HTLC_CAP_INLINE_MEDIA — initially
     * hidden, flipped on by inline_media_attach_refresh
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

/*
 * The palette laid out for the chat view.
 *
 * Slot layout (see rotulus.h's ROTULUS_PAL_* vocabulary):
 *   0..15   mIRC colors 0..15
 *   16..31  mIRC colors 16..31 (bold/extended; HexChat duplicates 0..15)
 *   32..46  UI roles: selection, fg/bg, marker line, history text,
 *           timestamps, nicks and their brackets, the [hx] tag, mentions,
 *           the column divider
 *   47..54  per-nick colors, from the theme's nick_colors list
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
    /* UI roles and per-nick colors: placeholders, fully transparent,
     * until gtkhx_apply_theme_palette fills them from the active theme
     * in gtkhx_activate. The chat view fe_init builds before that reads
     * transparent as "follow the system", and it re-reads the palette
     * on every draw, so nothing is drawn with a placeholder. */
    [ROTULUS_PAL_COLS - 1] = { 0, 0, 0, 0 },
};

G_STATIC_ASSERT (G_N_ELEMENTS (colors) == ROTULUS_PAL_COLS);
G_STATIC_ASSERT (HX_CHAT_LOG_INFO_COLOR == HX_CHAT_INFO_COLOR);
G_STATIC_ASSERT (ROTULUS_PAL_NICK_COLORS == GTKHX_NICK_COLORS_MAX);
/* Values gtkhx-ui's msg.rs, broadcast.rs and chat_output.rs mirror. */
G_STATIC_ASSERT (ROTULUS_PAL_MUTED == 37);
G_STATIC_ASSERT (ROTULUS_PAL_SYSTEM_BRACKET == 44);
G_STATIC_ASSERT (ROTULUS_PAL_HIGHLIGHT == 45);
G_STATIC_ASSERT (ROTULUS_PAL_NICK_BRACKET == 41);
G_STATIC_ASSERT (ROTULUS_PAL_SELF_BRACKET == 42);
G_STATIC_ASSERT (HX_CHAT_LOG_INFO_COLOR == 43 && MSG == 6);

/* Whether the active theme has any nick_colors. The slot a nick hashes to
 * never depends on how many (see hx_chat_nick_color), only on this. */
static gboolean have_nick_colors;

/* Refresh the UI-role slots from the active theme for the current
 * light/dark variant and push the palette into every live chat view.
 * The mIRC slots (0..31) are theme-agnostic — same red is "red" in both
 * modes, server authors expect those exact values.
 *
 * A role the theme leaves out takes the built-in default, which for
 * fg/bg (and the nick roles that derive from fg) is transparent: the
 * view then follows libadwaita's view colors. See gtkhx_theme.h.
 *
 * Called once at startup from gtkhx_activate after the
 * AdwStyleManager has settled on Light or Dark; again any time the
 * manager's `dark` property flips; and again any time the theme's
 * "changed" signal fires (theme file reload, Settings theme picker). */
void
gtkhx_apply_theme_palette (gboolean dark)
{
    /* Slot ↔ role mapping. The slot numbers are rotulus.h's
     * ROTULUS_PAL_* vocabulary; the roles are the theme-file-visible
     * names. */
    static const struct {
        int slot;
        GtkhxPaletteRole role;
    } role_to_slot[] = {
        { ROTULUS_PAL_MARK_FG, GTKHX_PAL_MARK_FG },
        { ROTULUS_PAL_MARK_BG, GTKHX_PAL_MARK_BG },
        { ROTULUS_PAL_FG, GTKHX_PAL_FG },
        { ROTULUS_PAL_BG, GTKHX_PAL_BG },
        { ROTULUS_PAL_MARKER, GTKHX_PAL_MARKER },
        { ROTULUS_PAL_MUTED, GTKHX_PAL_HISTORY_MUTED },
        { ROTULUS_PAL_TIMESTAMP, GTKHX_PAL_TIMESTAMP },
        { ROTULUS_PAL_NICK, GTKHX_PAL_NICK },
        { ROTULUS_PAL_SELF_NICK, GTKHX_PAL_SELF_NICK },
        { ROTULUS_PAL_NICK_BRACKET, GTKHX_PAL_NICK_BRACKET },
        { ROTULUS_PAL_SELF_BRACKET, GTKHX_PAL_SELF_BRACKET },
        { ROTULUS_PAL_SYSTEM, GTKHX_PAL_SYSTEM },
        { ROTULUS_PAL_SYSTEM_BRACKET, GTKHX_PAL_SYSTEM_BRACKET },
        { ROTULUS_PAL_HIGHLIGHT, GTKHX_PAL_HIGHLIGHT },
        { ROTULUS_PAL_RULE, GTKHX_PAL_RULE },
    };
    for (size_t i = 0; i < G_N_ELEMENTS (role_to_slot); i++) {
        colors[role_to_slot[i].slot]
            = gtkhx_theme_get_color (role_to_slot[i].role, dark);
    }
    /* A theme can recolor the view by its chrome rather than its chat
     * palette, while tinting is on (gtkhx.c). The chat view can't read a
     * CSS background, and keeps its text legible against the one it is
     * told of, so it is told. */
    if (colors[ROTULUS_PAL_BG].alpha == 0 && gtkhx_prefs.tint_window) {
        GdkRGBA view;
        if (gtkhx_theme_get_chrome_color (GTKHX_CHROME_VIEW, dark, &view)) {
            colors[ROTULUS_PAL_BG] = view;
        }
    }

    /* Per-nick slots. Every slot is filled — a theme's list repeats
     * across them — because a rendered line keeps the slot number it
     * was built with. Nicks hash over all the slots, not over the
     * theme's count, so a light/dark flip between lists of different
     * lengths recolors a person's old lines and new lines alike. A
     * theme with no list fills every slot with the plain nick color. */
    {
        GdkRGBA list[GTKHX_NICK_COLORS_MAX];
        int n = gtkhx_theme_get_nick_colors (dark, list);

        have_nick_colors = n > 0;
        for (int i = 0; i < ROTULUS_PAL_NICK_COLORS; i++) {
            colors[ROTULUS_PAL_NICK_COLOR0 + i]
                = n > 0 ? list[i % n] : colors[ROTULUS_PAL_NICK];
        }
    }

    /* Push the new palette into every live chat view, of every connection:
     * chat and private-chat outputs in each session's chats, and its
     * private-message outputs. News is a plain GtkTextView and follows
     * the theme's CSS without help. */
    for (guint s = 0; s < hx_session_count (); s++) {
        session *sess = hx_session_at (s);
        guint n = hx_chats_count (sess->chats);

        for (guint i = 0; i < n; i++) {
            struct gtkhx_chat *gchat
                = hx_chat_view (hx_chats_get_at (sess->chats, i));
            if (gchat && gchat->output) {
                rotulus_view_set_palette (ROTULUS_VIEW (gchat->output), colors);
            }
        }
        hx_msg_set_palette (sess, colors);
    }
}

/* hx_send_chat / hx_chat_user / hx_invite_user / hx_chat_join / hx_part_chat
 * / hx_change_subject moved to the hxhandlers::send::chat Rust crate (the port of
 * chat.c's send path, over hxproto's native chat builders). chat.h keeps
 * the C ABI decls; the per-htlc cap + chat-model lookups the senders need are
 * in chat_send_bridge.c. */

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

gint16
hx_chat_nick_color (const char *nick, gsize nick_len, gboolean is_self)
{
    guint32 h = 5381;

    if (is_self) {
        return ROTULUS_PAL_SELF_NICK;
    }
    if (!have_nick_colors || !nick) {
        return ROTULUS_PAL_NICK;
    }
    /* djb2 over the bytes: stable across runs and platforms, so a
     * person keeps their color from one session to the next. */
    for (gsize i = 0; i < nick_len; i++) {
        h = h * 33 + (guchar)nick[i];
    }
    return (gint16)(ROTULUS_PAL_NICK_COLOR0 + h % ROTULUS_PAL_NICK_COLORS);
}

/* Right-click on a nick in chat output.
 *
 * Hands straight to users.c::user_popup_show — the same builder the
 * Users window and the pchat sidebars use, so there is one menu rather
 * than a chat-shaped copy that drifts from it as items are added. The
 * view supplied the uid (see RotulusSpeaker) and the pointer position;
 * everything else the menu needs it looks up itself.
 *
 * `user_data` carries the cid so a private chat pops the menu against
 * its own membership rather than the public room's. */
static void
chat_speaker_menu (GtkWidget *view, guint64 uid, double x, double y,
                   gpointer user_data)
{
    guint32 cid = GPOINTER_TO_UINT (user_data);

    if (uid == 0 || uid > G_MAXUINT16) {
        return;
    }
    user_popup_show (view, gtkhx_chat_view_session (view), cid, (guint16)uid, x,
                     y);
}

/* The chat window whose output is `view`. */
static struct gtkhx_chat *
find_gchat_by_output (GtkWidget *view)
{
    session *sess = gtkhx_chat_view_session (view);
    HxChatRegistry *chats = sess ? sess->chats : NULL;
    if (!chats) {
        return NULL;
    }
    guint n = hx_chats_count (chats);
    for (guint i = 0; i < n; i++) {
        struct chat *c = hx_chats_get_at (chats, i);
        struct gtkhx_chat *g = hx_chat_view (c);
        if (g && g->output == view) {
            return g;
        }
    }
    return NULL;
}

/* media-activated on a chat output: pop the click-to-view dialog for
 * the inline image the token names. */
static void
inline_media_chat_activated (GtkWidget *view, guint token, gpointer data)
{
    (void)data;
    struct gtkhx_chat *gchat = find_gchat_by_output (view);
    struct chat *conv
        = gchat ? chat_with_cid (gtkhx_chat_view_session (view), gchat->cid)
                : NULL;
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
        view, gtkhx_chat_view_session (view)->htlc, m->id, m->id_len, m->mime,
        m->width_present ? m->width : 0, m->height_present ? m->height : 0,
        m->bytes_present ? m->bytes : 0);
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
        text = gtkhx_chat_view_new (colors, fontname,
                                    hx_conn_serial (sess->htlc));
        g_free (fontname);
    }
    g_signal_connect (text, "speaker-menu", G_CALLBACK (chat_speaker_menu),
                      GUINT_TO_POINTER (0));
    g_signal_connect (text, "media-activated",
                      G_CALLBACK (inline_media_chat_activated), NULL);

    vscroll = gtk_scrollbar_new (
        GTK_ORIENTATION_VERTICAL,
        rotulus_view_get_vadjustment (ROTULUS_VIEW (text)));

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

    /* Start any benchmarks GTKHX_BENCH asks for. Here rather than in
     * create_chat because they wait on the view's frame clock, which it
     * only has once it is in a mapped window — and build_leaves runs on
     * the way there. No-op otherwise. See gtkhx-ui's bench module. */
    hx_bench_maybe_start (gchat->output);

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
     * setbtns->inline_media_attach_refresh path flips it visible once
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

    g_return_if_fail (sess != NULL);

    /* The Chat panel used to carry a "sess" qdata here, for panel-level
     * handlers said to read it back. Nothing ever did — one write, no reads
     * anywhere in the tree — and the panel is one process-wide object while
     * the pointer was per connection, so the last connection to embed won and
     * the entry outlived whatever it named. Now that closing a tab frees the
     * session it was a dangling pointer waiting to be dereferenced by the
     * first handler that believed the comment. Anything that does need the
     * session from a panel should take the connection's serial and ask
     * hx_session_with_serial. */

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
        text = gtkhx_chat_view_new (colors, fontname,
                                    hx_conn_serial (sess->htlc));
        g_free (fontname);
    }
    g_signal_connect (text, "speaker-menu", G_CALLBACK (chat_speaker_menu),
                      GUINT_TO_POINTER (hx_chat_cid (chat)));
    g_signal_connect (text, "media-activated",
                      G_CALLBACK (inline_media_chat_activated), NULL);

    vscroll = gtk_scrollbar_new (
        GTK_ORIENTATION_VERTICAL,
        rotulus_view_get_vadjustment (ROTULUS_VIEW (text)));

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
 * new subject into the chat-window subject entry. A subject change
 * (hxhandlers::recv::chat) also logs the 'Subject Changed to X' chat
 * line; the subject a joined chat's user list carries
 * (hxhandlers::recv::user) paints without the announce, since "joined
 * a chat that already had a subject" isn't a subject change from the
 * user's perspective.
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
    /* Both paths decode the subject to UTF-8 before it gets here, which
     * is all Pango's editable widget accepts. */
    gtk_editable_set_text (GTK_EDITABLE (gchat->subject), buf);
}

/* View-side handler for the "chat-subject-notice" signal — the "Subject Changed
 * to: <subject>" chat-output line for a real subject change (the chat-subject
 * signal already updated the subject bar). The Rust chat-subject receive handler
 * (hxhandlers::recv::chat) emits it; the gettext + INFOPREFIX live
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
 * (join / parts / rename, and a kick that worked) the Rust user-roster receive
 * handlers (hxhandlers::recv::user) emit. The gettext + INFOPREFIX formatting and the showjoin-pref gate live
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
    case HX_USER_NOTICE_KICKED:
        hx_printf_prefix (htlc, cid, INFOPREFIX, "%s\n", _ ("kick successful"));
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
 * C-coupled leaf widgets these helpers build. */

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

    if (!gchat) {
        return NULL;
    }

    /* HxUserListView GObject (STYLE_CHAT — 18-px rows, 1.0× scale,
     * 36-px text offset). The view installs its own right-click
     * gesture and double-click → private-message handler, so the older
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
    gtkhx_widget_set_child (
        msg_btn,
        gtkhx_icon_image_new ("/com/nasledov/gtkhx/pixmaps/message.png"));
    g_signal_connect (msg_btn, "clicked", G_CALLBACK (view_msg_btn),
                      gchat->userlist);
    gtkhx_set_name_tip (msg_btn, _ ("Private Message"));

    kick_btn = gtk_button_new ();
    gtkhx_widget_set_child (
        kick_btn,
        gtkhx_icon_image_new ("/com/nasledov/gtkhx/pixmaps/kick.png"));
    g_signal_connect (kick_btn, "clicked", G_CALLBACK (view_kick_btn),
                      gchat->userlist);
    gtkhx_set_name_tip (kick_btn, _ ("Kick"));

    info_btn = gtk_button_new ();
    gtkhx_widget_set_child (
        info_btn,
        gtkhx_icon_image_new ("/com/nasledov/gtkhx/pixmaps/info.png"));
    g_signal_connect (info_btn, "clicked", G_CALLBACK (view_info_btn),
                      gchat->userlist);
    gtkhx_set_name_tip (info_btn, _ ("User Info"));

    ban_btn = gtk_button_new ();
    g_signal_connect (ban_btn, "clicked", G_CALLBACK (view_ban_btn),
                      gchat->userlist);
    gtkhx_widget_set_child (
        ban_btn, gtkhx_icon_image_new ("/com/nasledov/gtkhx/pixmaps/ban.png"));
    gtkhx_set_name_tip (ban_btn, _ ("Ban"));

    chat_btn = gtk_button_new ();
    gtkhx_set_name_tip (chat_btn, _ ("Private Chat"));
    g_signal_connect (chat_btn, "clicked", G_CALLBACK (view_chat_btn),
                      gchat->userlist);
    gtkhx_widget_set_child (
        chat_btn,
        gtkhx_icon_image_new ("/com/nasledov/gtkhx/pixmaps/chat.png"));

    igno_btn = gtk_button_new ();
    gtkhx_set_name_tip (igno_btn, _ ("Ignore"));
    g_signal_connect (igno_btn, "clicked", G_CALLBACK (view_igno_btn),
                      gchat->userlist);
    gtkhx_widget_set_child (
        igno_btn,
        gtkhx_icon_image_new ("/com/nasledov/gtkhx/pixmaps/ignore.png"));

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
    gtkhx_chat_output_clear (gchat->output);
    /* The subject entry belongs to the Chat panel's content, so it is
     * NULL both before the panel is first built and after it is closed
     * (gtkhx_chat_clear_content_ptrs nulls it on the content box's
     * destroy). Test the pointer, not hx_panel_was_constructed — that
     * latch is set once and never cleared, so it stays TRUE across a
     * close and would wave a NULL straight into GTK_EDITABLE. */
    if (subj && gchat->subject != NULL) {
        gtk_editable_set_text (GTK_EDITABLE (gchat->subject), "");
    }
}
