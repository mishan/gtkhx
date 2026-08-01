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
#include <errno.h>
#include <unistd.h>
#include <gtk/gtk.h>
#include <adwaita.h>
#include <time.h>
#include "hx.h"
#include "hxconn.h"
#include "gtkhx.h"
#include "news.h"
#include "chat_view.h"
#include "cicn.h"
#include "sound.h"
#include "users.h"
#include "chat.h"
#include "chat_members.h" /* hx_member_model_get_info, struct hx_member_info */
#include "files.h"
#include "network.h"
#include "tray.h"
#include "gtkutil.h"
#include "cfgkeys.h"
#include "gtkhx_theme.h"
#include "prefs_mirror.h"
#include "options.h"
#include "gif_icons.h"  /* hx_icon_save / _set / _clear + GIF_ICONS_* state */
#include "gif_avatar.h" /* gtkhx_avatar_set_animation_enabled (10.D pref) */
#include "toolbar.h"    /* toolbar_show_toast */
#include "hotline_proto.h" /* gtkhx_proto_gif_icon_is_gif */
#include "text_util.h"
#include "tracker.h"
#include "panel_registry.h" /* hx_panel_was_constructed */
#ifdef HAVE_VOICE
#include "voice_runtime.h"
#include "voice_ptt_keyspec.h"
#endif

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

static struct icon_viewer *iv;

GtkWidget *options_window = NULL;

/* The Tracker settings page moved to Rust (gtkhx-ui options.rs); it owns its
 * own GListStore + GtkColumnView and serialises back through
 * gtkhx_prefs_set_string(CFG_TRACKER, …). The schema stores the addresses as a
 * real array, so the comma-separated string and the derived char ** beside it
 * are both projections the mirror refresh re-derives — there is no splitting
 * hook any more. */

/* hxconfig's C ABI (rust/crates/hxconfig/src/ffi.rs). Hand-declared, like
 * every other Rust seam in the tree — a signature that drifts shows up as an
 * undefined symbol at link time rather than as a miscompile.
 *
 * Rust owns the values and the file. `gtkhx_prefs` is a read-only C copy whose
 * storage lives in prefs_mirror.c and which is repopulated from here after
 * every change, so the C sites that read a preference keep compiling
 * untouched. Everything in this file that *writes* a preference goes through
 * the setters below, which is what makes the file something we edit rather
 * than regenerate. See docs/preferences.md and src/prefs_mirror.h.
 *
 * Names are the old SHOUTING_CASE keys from cfgkeys.h; hxconfig resolves them
 * through its migration map to schema paths. */
extern int hxconfig_load (const char *dir, const char *legacy,
                          const char *legacy_home);
extern int hxconfig_is_first_run (void);
extern char *hxconfig_warning (int i);
extern int hxconfig_flush (void);
extern int hxconfig_type (const char *name);
extern int hxconfig_get_bool (const char *name);
extern int hxconfig_get_int (const char *name);
extern char *hxconfig_get_string (const char *name);
extern int hxconfig_set_bool (const char *name, int value);
extern int hxconfig_set_int (const char *name, int value);
extern int hxconfig_set_string (const char *name, const char *value);
extern void hxconfig_free_string (char *s);

struct icon_viewer {
    guint32 icon_high;
    unsigned int nfound;
    GtkWidget *icon_list; /* multi-column flowbox for narrow icons */
    GtkWidget *wide_list; /* one-per-row flowbox for wide banners */
    /* The icon picker is now a popup opened from the Identity page's
     * "Browse…" button rather than an inline group. This holds the live
     * popup AdwDialog (or NULL) so a selection can dismiss it and the
     * Settings teardown can close it. icon_list / wide_list point into
     * this popup's flowboxes while it's open. */
    GtkWidget *picker_dialog;
};

/* A wide cicn (Mac banner-style icon) is anything wider than this
 * before the WIDE_BANNER_LEFT_PAD crop. Same threshold the user-list
 * overlay uses. Wide entries go into the dedicated single-column
 * flowbox so the user sees them at their natural aspect ratio
 * (~432x32 cropped to ~232x32 then scaled to fill the row) rather
 * than squashed into the 56x56 grid cell used for normal icons. */
#define ICON_PICKER_WIDE_THRESHOLD 400
#define ICON_PICKER_WIDE_CROP 198
#define ICON_PICKER_WIDE_HEIGHT 32 /* match cicn native vertical pixel count */

/* forward declarations for the icon-picker FlowBox helpers.
 * The implementations live further down, next to the live-row registry they
 * reach into; the forward decls let list_icons() and settings_page_identity()
 * — both up here — wire them. */
static int icon_picker_sort_cb (GtkFlowBoxChild *a, GtkFlowBoxChild *b,
                                gpointer data);
static void icon_flow_child_activated (GtkFlowBox *flowbox,
                                       GtkFlowBoxChild *child, gpointer data);

static void
list_icons (void)
{
    /* GtkFlowBox auto-flows multiple icons per row. Each entry is a
     * 64x64 image plus the resource-ID label, packed into a vertical
     * box and inserted as a flowbox child. The flowbox itself is
     * configured with min/max children per line so the picker grows
     * to as many columns as fit the picker width without going
     * single-column-narrow.
     *
     * cicn_to_pixbuf returns a GdkPixbuf directly with the Mac
     * classic mask folded into the alpha channel. Wide icons (the
     * 32x32 family bundles four variants in a 4*32-pixel row)
     * are clipped to the rightmost 32 px to mirror the historical
     * "off = width > 400 ? 198 : 0" hack.
     *
     * Two passes: first walk every rsrc file in icon_files priority
     * order (user $CONFIG/icons → XDG → $PREFIX per init_icons) and
     * pick a single winning resource per resid, keeping the FIRST
     * occurrence so user customizations override system defaults
     * instead of both showing up as duplicates in the picker.
     * Second pass renders the winners. */
    GtkWidget *icon_list = iv->icon_list;
    guint16 nres;
    guint32 icon;
    unsigned int nfound = 0;
    unsigned int i;
    unsigned int rendered;
    GHashTable *winners;
    GHashTableIter iter;
    gpointer iter_key, iter_val;

    winners
        = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);

    for (i = 0; i < icon_files.n; ++i) {
        if (!icon_files.cicns[i]) {
            continue;
        }
        nres = macres_file_num_res_of_type (icon_files.cicns[i], TYPE_cicn);
        for (icon = 0; icon < nres; icon++) {
            macres_res *r;
            gpointer key;

            r = macres_file_get_nth_res_of_type (icon_files.cicns[i], TYPE_cicn,
                                                 icon);
            if (!r) {
                continue;
            }
            key = GUINT_TO_POINTER ((guint)r->resid);
            if (g_hash_table_contains (winners, key)) {
                /* A higher-priority file already claimed this
                 * resid — discard the duplicate so it doesn't
                 * show up alongside the user's version in the
                 * picker. */
                g_free (r);
                continue;
            }
            g_hash_table_insert (winners, key, r);
        }
    }

    rendered = 0;
    g_hash_table_iter_init (&iter, winners);
    while (g_hash_table_iter_next (&iter, &iter_key, &iter_val)) {
        macres_res *r = iter_val;
        GdkPixbuf *pb, *cropped, *scaled;
        GtkWidget *child, *vbox, *image, *label;
        int width, height, off;
        char buf[16];
        gboolean is_wide;

        pb = cicn_to_pixbuf (r->data, r->datalen);
        if (!pb) {
            continue;
        }
        width = gdk_pixbuf_get_width (pb);
        height = gdk_pixbuf_get_height (pb);
        is_wide = (width > ICON_PICKER_WIDE_THRESHOLD);
        off = is_wide ? ICON_PICKER_WIDE_CROP : 0;
        if (off) {
            cropped
                = gdk_pixbuf_new_subpixbuf (pb, off, 0, width - off, height);
            g_object_unref (pb);
            pb = cropped;
            width -= off;
        }

        /* Narrow icons → uniform 56x56 thumbnail for the grid.
         * Wide banners → preserve aspect ratio (scale to a row-
         * height of 56 and proportional width) so the banner
         * art is recognisable instead of being squashed into a
         * square cell. */
        if (is_wide) {
            int target_h = 56;
            int target_w = width * target_h / (height > 0 ? height : 1);
            if (target_w < 1) {
                target_w = 1;
            }
            scaled = gdk_pixbuf_scale_simple (pb, target_w, target_h,
                                              GDK_INTERP_NEAREST);
        } else {
            /* Nearest-neighbor preserves pixel-art look at
             * 3.5x of 16px / 1.75x of 32px sources. */
            scaled = gdk_pixbuf_scale_simple (pb, 56, 56, GDK_INTERP_NEAREST);
        }
        g_object_unref (pb);
        pb = scaled ? scaled : pb;

        nfound++;
        g_snprintf (buf, sizeof (buf), "%u", r->resid);

        vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
        /* GtkPicture, not GtkImage. Adwaita's
         * stylesheet sizes GtkImage to icon-button dimensions
         * (~16-24px) regardless of the paintable's natural
         * size, so set_size_request on a wrapper grew the cell
         * but kept the icon clamped tiny inside it. GtkPicture
         * with can_shrink=FALSE pins to the paintable's natural
         * size — same fix gtkhx_pixmap_button uses for the
         * toolbar buttons. */
        {
            GdkTexture *tex = gtkhx_texture_from_pixbuf (pb);
            if (tex) {
                image = gtk_picture_new_for_paintable (GDK_PAINTABLE (tex));
                g_object_unref (tex);
                gtk_picture_set_can_shrink (GTK_PICTURE (image), FALSE);
            } else {
                image = gtk_picture_new ();
            }
        }
        label = gtk_label_new (buf);
        gtk_widget_add_css_class (label, "caption");
        gtk_box_append (GTK_BOX (vbox), image);
        gtk_box_append (GTK_BOX (vbox), label);
        gtk_widget_set_margin_start (vbox, 6);
        gtk_widget_set_margin_end (vbox, 6);
        gtk_widget_set_margin_top (vbox, 6);
        gtk_widget_set_margin_bottom (vbox, 6);

        child = gtk_flow_box_child_new ();
        gtk_flow_box_child_set_child (GTK_FLOW_BOX_CHILD (child), vbox);
        g_object_set_data (G_OBJECT (child), "resid",
                           GUINT_TO_POINTER (r->resid));
        /* Wide banners go into their own dedicated flowbox so
         * they show one-per-row at natural aspect ratio. */
        if (is_wide && iv->wide_list) {
            gtk_flow_box_append (GTK_FLOW_BOX (iv->wide_list), child);
        } else {
            gtk_flow_box_append (GTK_FLOW_BOX (icon_list), child);
        }

        g_object_unref (pb);

        /* Cooperative multitasking — keep the dialog responsive while
         * paging through hundreds of resource entries. */
        rendered++;
        if (rendered % 10 == 0) {
            while (g_main_context_pending (NULL)) {
                g_main_context_iteration (NULL, TRUE);
            }
            /* The picker now lives in its own AdwDialog, which the
             * user can close mid-render — yielding above runs its
             * "closed" handler, which nulls iv->icon_list / wide_list.
             * Bail before appending into the now-destroyed flowboxes
             * (also covers the whole Settings dialog closing, which
             * frees iv). winners owns the remaining macres_res entries;
             * destroying the table frees them via the destroy_func. */
            if (!options_window || !iv || iv->icon_list != icon_list) {
                g_hash_table_destroy (winners);
                return;
            }
        }
    }

    g_hash_table_destroy (winners);

    if (nfound >= 2 && options_window && iv && iv->icon_list == icon_list) {
        gtk_flow_box_invalidate_sort (GTK_FLOW_BOX (icon_list));
        if (iv->wide_list) {
            gtk_flow_box_invalidate_sort (GTK_FLOW_BOX (iv->wide_list));
        }
    }
}

void
reinit_gtktexts (session *sess)
{
    if (hx_panel_was_constructed (HX_PANEL_ID_NEWS)) {
        gtkhx_apply_text_style (sess->news_text);
    }
    {
        gchar *fontname = pango_font_description_to_string (gtkhx_font_desc);
        if (sess->chats) {
            guint n = hx_chats_count (sess->chats);
            for (guint i = 0; i < n; i++) {
                struct chat *c = hx_chats_get_at (sess->chats, i);
                struct gtkhx_chat *gchat = hx_chat_view (c);
                if (!gchat) {
                    continue;
                }
                if (hx_chats_cid_at (sess->chats, i) == 0
                    && !hx_panel_was_constructed (HX_PANEL_ID_CHAT)) {
                    continue;
                }
                hx_chat_view_set_font (hx_gchat_output (gchat), fontname);
                hx_chat_view_refresh (hx_gchat_output (gchat));
                if (hx_gchat_input (gchat)) {
                    gtkhx_apply_input_font (hx_gchat_input (gchat));
                }
                if (hx_gchat_subject (gchat)) {
                    gtkhx_apply_text_style (hx_gchat_subject (gchat));
                }
            }
        }
        /* walk the PM-window hashtable. The pre-port
         * code walked `sess->msg_list` here, but that field was
         * never populated — the real msg_list global lived in
         * msg.c, so this loop was a silent no-op for years and
         * font-changes never reached open PM windows. The
         * migration to session->msg_windows fixes that as a side
         * effect. */
        if (sess->msg_windows) {
            GHashTableIter iter;
            gpointer val;
            g_hash_table_iter_init (&iter, sess->msg_windows);
            while (g_hash_table_iter_next (&iter, NULL, &val)) {
                struct msgwin *msg = val;
                hx_chat_view_set_font (msg->outputbuf, fontname);
                hx_chat_view_refresh (msg->outputbuf);
                gtkhx_apply_input_font (msg->inputbuf);
            }
        }
        g_free (fontname);
    }
}

static void
changed_xtext (session *sess)
{
    if (sess) {
        if (sess->chats) {
            guint n = hx_chats_count (sess->chats);
            for (guint i = 0; i < n; i++) {
                struct chat *c = hx_chats_get_at (sess->chats, i);
                struct gtkhx_chat *gchat = hx_chat_view (c);
                if (!gchat) {
                    continue;
                }
                GtkWidget *out = hx_gchat_output (gchat);
                hx_chat_view_set_word_wrap (out, gtkhx_prefs.word_wrap);
                hx_chat_view_set_max_lines (out, gtkhx_prefs.xbuf_max);
                hx_chat_view_refresh (out);
            }
        }
        if (sess->msg_windows) {
            GHashTableIter iter;
            gpointer val;
            g_hash_table_iter_init (&iter, sess->msg_windows);
            while (g_hash_table_iter_next (&iter, NULL, &val)) {
                struct msgwin *msg = val;
                hx_chat_view_set_word_wrap (msg->outputbuf,
                                            gtkhx_prefs.word_wrap);
                hx_chat_view_set_max_lines (msg->outputbuf,
                                            gtkhx_prefs.xbuf_max);
                hx_chat_view_refresh (msg->outputbuf);
            }
        }
    }
}

/* CFG_MARKDOWN is process-wide in the view, so unlike the avatar and
 * timestamp toggles this needs no per-view walk — one call, and every
 * chat surface picks it up for messages appended afterwards. Rows
 * already rendered keep their current form; re-parsing scrollback would
 * mean keeping every row's original source text alive forever. */
static void
changed_markdown (session *sess)
{
    (void)sess;
    hx_chat_view_set_markdown (gtkhx_prefs.markdown);
}

/* apply the CFG_CHAT_AVATARS toggle to every live chat view.
 *
 * Same shape as changed_timestamp below, and for the same reason: the
 * setting is per-view state, so flipping it has to walk the live views
 * rather than wait for them to be rebuilt. */
static void
changed_chat_avatars (session *sess)
{
    int px = gtkhx_prefs.chat_avatars ? HX_CHAT_AVATAR_SIZE_DEFAULT : 0;

    if (!sess) {
        return;
    }
    if (sess->chats) {
        guint n = hx_chats_count (sess->chats);
        for (guint i = 0; i < n; i++) {
            struct chat *c = hx_chats_get_at (sess->chats, i);
            struct gtkhx_chat *gchat = hx_chat_view (c);
            if (!gchat) {
                continue;
            }
            hx_chat_view_set_avatar_size (hx_gchat_output (gchat), px);
        }
    }
    if (sess->msg_windows) {
        GHashTableIter iter;
        gpointer val;
        g_hash_table_iter_init (&iter, sess->msg_windows);
        while (g_hash_table_iter_next (&iter, NULL, &val)) {
            struct msgwin *msg = val;
            hx_chat_view_set_avatar_size (msg->outputbuf, px);
        }
    }
}

/* apply the CFG_TIMESTAMP toggle to every live chat view
 * — chat / pchat outputs in gchat_list, plus PM outputs in msg_windows.
 * View-native stamps are flipped per-view via hx_chat_view_set_time_stamp.
 * hx_chat_view_refresh forces a full re-render so the new state is visible
 * without scrolling the buffer first. */
static void
changed_timestamp (session *sess)
{
    if (!sess) {
        return;
    }
    if (sess->chats) {
        guint n = hx_chats_count (sess->chats);
        for (guint i = 0; i < n; i++) {
            struct chat *c = hx_chats_get_at (sess->chats, i);
            struct gtkhx_chat *gchat = hx_chat_view (c);
            if (!gchat) {
                continue;
            }
            hx_chat_view_set_time_stamp (hx_gchat_output (gchat),
                                         gtkhx_prefs.timestamp);
            hx_chat_view_refresh (hx_gchat_output (gchat));
        }
    }
    if (sess->msg_windows) {
        GHashTableIter iter;
        gpointer val;
        g_hash_table_iter_init (&iter, sess->msg_windows);
        while (g_hash_table_iter_next (&iter, NULL, &val)) {
            struct msgwin *msg = val;
            hx_chat_view_set_time_stamp (msg->outputbuf, gtkhx_prefs.timestamp);
            hx_chat_view_refresh (msg->outputbuf);
        }
    }
}

/* Copy the stored identity onto a connection.
 *
 * The nickname and icon preferences used to *be* the connection's wire name
 * buffer and icon field — a startup binder patched their two table slots with
 * raw interior pointers, so a write to either went straight onto the wire
 * struct. They are ordinary settings with their own storage now, and this is
 * the one-way copy that replaces the aliasing. An empty nickname or a zero
 * icon means "nothing stored", so the connection keeps whatever it was given
 * at construction. */
static void
identity_copy_to_conn (struct htlc_conn *htlc)
{
    char *nick = gtkhx_prefs_get_string (CFG_NICK);
    int icon = gtkhx_prefs_get_int (CFG_ICON);

    if (*nick) {
        hx_conn_set_name (htlc, nick);
    }
    g_free (nick);
    if (icon != 0) {
        hx_conn_set_icon (htlc, (guint16)icon);
    }
}

/* The Settings nick/icon controls used to be inert — changing them updated the
 * session's htlc but never told the server, so the user list still showed your
 * old nick/icon until reconnect. The send is a no-op while the connection has
 * no socket, so this is safe to call before connect too. */
static void
changed_nickoricon (session *sess)
{
    (void)sess;
    identity_copy_to_conn (hx_active_session ()->htlc);
    hx_change_name_icon (hx_active_session ()->htlc);
}

/* Colored-Nicknames extension — mirror the pref onto the
 * live htlc and re-broadcast via USER_CHANGE. Prefs store the color
 * as int (-1 = "no color"); reinterpret as guint32 so HX_NICK_COLOR
 * _NONE round-trips bit-identically.
 *
 * We don't wait for the server to echo USER_CHANGE back to us before
 * repainting our own row: servers vary on whether they echo a user's
 * own broadcast to that user, and even when they do, the round-trip
 * is enough of a lag that the user perceives the picker as broken.
 * Update self's nick_color in the member model directly and re-render. The
 * inbound echo (if any) lands on top with the same value — no
 * flicker, just an idempotent rewrite. */
static void
changed_nick_color (session *sess)
{
    (void)sess;
    guint32 nc = (guint32)gtkhx_prefs.nick_color;
    hx_conn_set_nick_color (hx_active_session ()->htlc, nc);
    hx_change_name_icon (hx_active_session ()->htlc);

    /* Locally re-render our own row in the public chat user list.
     * Pre-login (no uid yet, or no chat container yet) just no-ops —
     * apply_loaded_xtext_prefs stamps the loaded pref onto htlc, and
     * the SELFINFO-driven membership add for self picks it up the same
     * way it picks up the loaded nick. */
    struct chat *pub = chat_with_cid (hx_active_session (), 0);
    if (pub && hx_conn_uid (hx_active_session ()->htlc)) {
        struct hx_member_info mi;
        if (hx_member_model_get_info (hx_chat_member_model (pub),
                                      hx_conn_uid (hx_active_session ()->htlc),
                                      &mi)) {
            user_change (hx_active_session ()->htlc, pub, mi.uid, nc, mi.name,
                         mi.icon, mi.status);
        }
    }
}

static void
changed_font (session *sess)
{
    if (gtkhx_font_desc) {
        pango_font_description_free (gtkhx_font_desc);
        gtkhx_font_desc = NULL;
    }

    if (gtkhx_prefs.font && *gtkhx_prefs.font) {
        gtkhx_font_desc = pango_font_description_from_string (gtkhx_prefs.font);
    }

    if (!gtkhx_font_desc) {
        /* Fall back for *this* run, but leave the stored value alone. It used
         * to be overwritten with the fallback, which silently discarded what
         * the user typed — so a typo became unrecoverable rather than
         * something they could see in Settings and correct. (The write would
         * be discarded by the next mirror refresh now anyway; the mirror is
         * read-only to C.) */
        g_warning ("Bad font \"%s\", using Monospace 10 for this session",
                   gtkhx_prefs.font ? gtkhx_prefs.font : "");
        gtkhx_font_desc = pango_font_description_from_string ("Monospace 10");
    }

    /* rebuild the screen-wide CSS provider so already-tagged
     * widgets pick up the new font without needing per-widget calls. */
    gtkhx_refresh_css ();

    if (sess) {
        reinit_gtktexts (sess);
    }
}

/* changefunc for CFG_EMOJI_SHORTCODES (phase E6). Push the toggle into
 * text_util.c, which both the send encode (gtkhx_text_for_wire) and the
 * receive decode (proto_helpers chat / PM builders) consult. Kept out of
 * those dependency-light translation units' direct gtkhx_prefs reach so
 * their unit tests don't have to link the prefs global. */
static void
changed_emoji_shortcodes (session *sess)
{
    (void)sess;
    gtkhx_text_set_emoji_shortcodes_enabled (gtkhx_prefs.emoji_shortcodes);
}

/* changefunc for CFG_ANIMATE_AVATARS (Phase 10.D). Push the toggle into
 * gif_avatar.c, which starts/stops its frame timer and repaints avatars
 * as either animated or a still first frame. */
static void
changed_animate_avatars (session *sess)
{
    (void)sess;
    gtkhx_avatar_set_animation_enabled (gtkhx_prefs.animate_avatars);
}

/* HexChat-style xtext autocopy. Each toggle in Settings →
 * Advanced → Auto Copy Behavior calls one of the three xtext setters
 * to flip the corresponding facet of the drag-end clipboard handler.
 * The persisted state is mirrored in the gtkhx_prefs.autocopy_* bytes; the
 * hook just propagates it to the view's own process-wide copy so the next
 * drag-end picks up the new behaviour without having to recreate the
 * widget. */
static void
changed_autocopy_text (session *sess)
{
    (void)sess;
    hx_chat_view_set_autocopy_text (gtkhx_prefs.autocopy_text);
}
static void
changed_autocopy_stamp (session *sess)
{
    (void)sess;
    hx_chat_view_set_autocopy_stamp (gtkhx_prefs.autocopy_stamp);
}
static void
changed_autocopy_color (session *sess)
{
    (void)sess;
    hx_chat_view_set_autocopy_color (gtkhx_prefs.autocopy_color);
}

/* apply CFG_STAMP_FORMAT to every live chat view. The
 * setter stashes the new format in xtext's module-global, recomputes
 * stamp_width per widget (font-dependent), and grows the buffer
 * indent if the new column is wider than before. queue_draw fires
 * inside the setter so the new column shows up next frame. */
static void
changed_stampformat (session *sess)
{
    /* The format itself is process-wide; the per-view work below only
     * recomputes column widths. A NULL view records the format and nothing
     * else, which is the only thing that can happen when this runs at load —
     * no chat view exists yet — and is where every view built later picks the
     * format up from. */
    hx_chat_view_set_stamp_format (NULL, gtkhx_prefs.stamp_format);

    if (!sess) {
        return;
    }
    if (sess->chats) {
        guint n = hx_chats_count (sess->chats);
        for (guint i = 0; i < n; i++) {
            struct chat *c = hx_chats_get_at (sess->chats, i);
            struct gtkhx_chat *gchat = hx_chat_view (c);
            if (!gchat) {
                continue;
            }
            hx_chat_view_set_stamp_format (hx_gchat_output (gchat),
                                           gtkhx_prefs.stamp_format);
        }
    }
    if (sess->msg_windows) {
        GHashTableIter iter;
        gpointer val;
        g_hash_table_iter_init (&iter, sess->msg_windows);
        while (g_hash_table_iter_next (&iter, NULL, &val)) {
            struct msgwin *msg = val;
            hx_chat_view_set_stamp_format (msg->outputbuf,
                                           gtkhx_prefs.stamp_format);
        }
    }
}

static void
changed_downloadpath (session *sess)
{
    (void)sess;
    /* Empty pref value → use the user's XDG Downloads dir as
     * the natural default. Falls back further to $HOME if
     * XDG_DOWNLOAD_DIR isn't set / not a directory, then to "."
     * as a last resort. This is the value the Files browser
     * also consults for its initial local panel path, so the
     * two stay in sync. */
    if (!gtkhx_prefs.download_path || !*gtkhx_prefs.download_path) {
        const char *xdg = g_get_user_special_dir (G_USER_DIRECTORY_DOWNLOAD);
        const char *home = g_get_home_dir ();
        char *win_mac_dl
            = home ? g_build_filename (home, "Downloads", NULL) : NULL;
        const char *chosen;

        if (xdg && g_file_test (xdg, G_FILE_TEST_IS_DIR)) {
            chosen = xdg;
        } else if (win_mac_dl && g_file_test (win_mac_dl, G_FILE_TEST_IS_DIR)) {
            /* g_get_user_special_dir(DOWNLOAD) is NULL on Windows
             * (KNOWNFOLDERID, not CSIDL); <home>/Downloads is the real
             * location there and on macOS. Keep this in sync with
             * default_root() in files_local_provider.c so the download dest
             * and the local panel agree. */
            chosen = win_mac_dl;
        } else if (home && g_file_test (home, G_FILE_TEST_IS_DIR)) {
            chosen = home;
        } else {
            chosen = ".";
        }

        /* Store it rather than patching the mirror. The mirror is a copy that
         * the next refresh overwrites, so an assignment here would take
         * effect and then quietly vanish — leaving downloads pointed at the
         * empty string. Going through the setter persists the resolved path
         * and refreshes the mirror on the way. Re-entering this hook is safe:
         * the second pass sees a non-empty value and does nothing. */
        gtkhx_prefs_set_string (CFG_DOWNLOAD, chosen);
        g_free (win_mac_dl);
    }
}

static void
changed_case (session *sess)
{
    (void)sess;
    /* case-fold lives on gtkhx_prefs.track_case and is read
     * directly by tracker.c::tracker_rerun_search when compiling the
     * GRegex (G_REGEX_CASELESS toggled per the pref). No global regex-
     * engine state to flip; just kick the live filter so the result
     * list reflects the new mode immediately. No-op when the Tracker
     * isn't open. */
    tracker_search_refresh ();
}

/* apply the THEME pref to libadwaita's style manager. The pref
 * is one of "system" / "light" / "dark"; anything else falls back to
 * the system default so a hand-edited file with a typo doesn't lock
 * the user into a broken state. Called at startup and again whenever the
 * user picks a new value in Settings. */
static void
changed_theme (session *sess)
{
    AdwStyleManager *sm = adw_style_manager_get_default ();
    const char *theme
        = gtkhx_prefs.theme ? gtkhx_prefs.theme : CFG_THEME_SYSTEM;
    AdwColorScheme scheme;

    (void)sess;

    if (g_strcmp0 (theme, CFG_THEME_LIGHT) == 0) {
        scheme = ADW_COLOR_SCHEME_FORCE_LIGHT;
    } else if (g_strcmp0 (theme, CFG_THEME_DARK) == 0) {
        scheme = ADW_COLOR_SCHEME_FORCE_DARK;
    } else {
        scheme = ADW_COLOR_SCHEME_DEFAULT;
    }

    adw_style_manager_set_color_scheme (sm, scheme);
}
/* Settings → General → "Show tray icon" toggle. Drives the
 * StatusNotifierItem registration on the session bus — switching
 * ON kicks off tray_activate_register; switching OFF unregisters
 * but leaves the module reachable for a future flip. */
static void
changed_tray (session *sess)
{
    (void)sess;
    gtkhx_tray_set_enabled (gtkhx_prefs.tray);
}

#ifdef HAVE_VOICE
/* Settings → Voice → "Input device" combobox. Pushes the user's
 * pick through to the Rust runtime via FFI; the next VoiceRuntime
 * construction (typically the next Join Voice click) builds the
 * send leg against the resolved device. Empty / NULL means
 * "system default" — autoaudiosrc resolves whichever PulseAudio /
 * PipeWire / ALSA default the host has configured.
 *
 * No effect on a currently-active voice session — the runtime is
 * constructed once per session and the bins are built at Join
 * time; changing the device picker takes effect on the next call.
 * Phase 8.E follow-up could hot-swap by rebuilding the send bin
 * on prefs change, but for now Leave + Join is the prescribed
 * dance. */
static void
changed_voice_input_device (session *sess)
{
    (void)sess;
    gtkhx_voice_set_input_device (gtkhx_prefs.voice_input_device);
}

/* Settings → Voice → "Output device" combobox. Same shape as
 * changed_voice_input_device but for the receive (autoaudiosink)
 * side. */
static void
changed_voice_output_device (session *sess)
{
    (void)sess;
    gtkhx_voice_set_output_device (gtkhx_prefs.voice_output_device);
}
#endif /* HAVE_VOICE */

/* changefunc for the active-theme name. A change here (manually
 * editing gtkhxrc, or — in a future theme-editor phase — Settings
 * picking a different theme) reloads the theme file and emits the
 * theme "changed" signal so every subscribed button / user-list view
 * / chat xtext rescales and repaints. */
static void
changed_theme_name (session *sess)
{
    (void)sess;
    gtkhx_theme_load_active ();
}

/* The value-kind tags the by-name bridge reports, and the numbering
 * hxconfig_type hands back. Unchanged from the tags the old address table
 * carried, because the Settings pages — the C ones here and the Rust ones in
 * gtkhx-ui — switch on them. There is no STRING32 any more: the only key that
 * had it was the nickname, whose storage was the connection's 32-byte wire
 * name buffer, and identity has its own storage now. */
#define INT 1
#define BOOLEAN 2
#define STRING 3
#define UINT16 5

/* Name → apply hook.
 *
 * All that survives of the old address table's `changefunc` column. A linear
 * scan over two dozen entries is a handful of string compares against the GTK
 * work every one of these hooks drives, so there is nothing to gain from
 * sorting it — and nothing to get wrong, which is what the sorted table needed
 * a fatal runtime assertion and a source-scanning unit test to guarantee.
 *
 * Most keys need no hook at all — they configure something that reads the
 * mirror live, so there is nothing to push anywhere. One is worth calling out
 * as a deliberate removal rather than an absence: CFG_TRACKER used to re-split
 * its comma-separated value into the derived `char **` beside it. The schema
 * stores a real array and the mirror projects both shapes from it, so the
 * splitting hook is gone. */
typedef void (*pref_hook_fn) (session *);

struct pref_hook {
    const char *name;
    pref_hook_fn fn;
};

static const struct pref_hook pref_hooks[] = {
    { CFG_ANIMATE_AVATARS, changed_animate_avatars },
    { CFG_AUTOCOPY_COLOR, changed_autocopy_color },
    { CFG_AUTOCOPY_STAMP, changed_autocopy_stamp },
    { CFG_AUTOCOPY_TEXT, changed_autocopy_text },
    { CFG_CHAT_AVATARS, changed_chat_avatars },
    { CFG_DOWNLOAD, changed_downloadpath },
    { CFG_EMOJI_SHORTCODES, changed_emoji_shortcodes },
    { CFG_FONT, changed_font },
    { CFG_ICON, changed_nickoricon },
    { CFG_MARKDOWN, changed_markdown },
    { CFG_NICK, changed_nickoricon },
    { CFG_NICK_COLOR, changed_nick_color },
    { CFG_STAMP_FORMAT, changed_stampformat },
    { CFG_THEME, changed_theme },
    { CFG_THEME_NAME, changed_theme_name },
    { CFG_TIMESTAMP, changed_timestamp },
    { CFG_TRACKER_CASE, changed_case },
    { CFG_TRAY, changed_tray },
#ifdef HAVE_VOICE
    /* The device preferences persist whether or not voice is compiled in, so
     * a build without it doesn't discard a user's saved picks; only the hooks
     * that push the value into the Rust runtime are conditional. */
    { CFG_VOICE_INPUT_DEVICE, changed_voice_input_device },
    { CFG_VOICE_OUTPUT_DEVICE, changed_voice_output_device },
#endif
    { CFG_WORDWRAP, changed_xtext },
    { CFG_XBUF_MAX, changed_xtext },
};

static pref_hook_fn
pref_hook_for (const char *name)
{
    size_t i;

    for (i = 0; i < G_N_ELEMENTS (pref_hooks); i++) {
        if (strcmp (pref_hooks[i].name, name) == 0) {
            return pref_hooks[i].fn;
        }
    }
    return NULL;
}

/* The Settings rows that are on screen right now, keyed by config name.
 *
 * The old table carried a `widget` slot per entry for this, and two things
 * still need it: gtkhx_prefs_set_bool drives the live switch row when a
 * toggle is flipped from somewhere other than Settings (the Tracker window's
 * case-sensitive button), and the icon picker stamps the chosen resource ID
 * onto the Identity page's spin row. Populated by the row builders below and
 * emptied when the dialog closes, so a lookup that finds nothing means
 * "Settings isn't open" — exactly what a NULL widget slot used to mean.
 *
 * The keys are copied because callers spell a name however they like; the
 * values are borrowed widgets owned by the dialog's tree. */
static GHashTable *pref_widgets;

static void
pref_widget_register (const char *name, GtkWidget *row)
{
    if (!pref_widgets) {
        pref_widgets
            = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    }
    g_hash_table_insert (pref_widgets, g_strdup (name), row);
}

static GtkWidget *
pref_widget_lookup (const char *name)
{
    return pref_widgets ? g_hash_table_lookup (pref_widgets, name) : NULL;
}

/* Everything a successful write has to do after the value has landed in Rust:
 * bring the C mirror back into step, run the key's apply hook, and arm the
 * save timer. */
static void
pref_apply (const char *name)
{
    pref_hook_fn hook = pref_hook_for (name);

    hx_prefs_mirror_refresh ();
    if (hook) {
        hook (hx_active_session ());
    }
    hx_prefs_save_soon ();
}

/* helper to compare two flowbox children by their stored
 * resource ID so the icon picker stays sorted by ID. Forward-declared
 * up near list_icons() because it's wired there too. */
static int
icon_picker_sort_cb (GtkFlowBoxChild *a, GtkFlowBoxChild *b, gpointer data)
{
    guint id_a = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (a), "resid"));
    guint id_b = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (b), "resid"));
    (void)data;

    if (id_a < id_b) {
        return -1;
    }
    if (id_a > id_b) {
        return 1;
    }
    return 0;
}

/* Handler for the FlowBox's child-activated signal. The activated
 * child carries the resid as g_object_set_data; copy it into the
 * AdwSpinRow so the user's selection becomes the active icon ID. */
static void
icon_flow_child_activated (GtkFlowBox *flowbox, GtkFlowBoxChild *child,
                           gpointer data)
{
    GtkWidget *row;
    guint icon;
    (void)flowbox;
    (void)data;

    icon = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (child), "resid"));
    if (!icon) {
        return;
    }

    row = pref_widget_lookup (CFG_ICON);
    if (row && ADW_IS_SPIN_ROW (row)) {
        adw_spin_row_set_value (ADW_SPIN_ROW (row), icon);
    }
    /* Picking an icon dismisses the Browse popup; the value we just
     * stamped onto the spin row drives the Identity page's inline
     * preview via its notify::value handler. */
    if (iv && iv->picker_dialog) {
        adw_dialog_close (ADW_DIALOG (iv->picker_dialog));
    }
}

/* ---- Typed by-name accessors ------------------------------------
 *
 * The bridge the Rust settings pages (gtkhx-ui options.rs) and the remaining
 * per-window toggles read and write preferences through. Each one is a thin
 * pass to hxconfig; a write that actually changes something then refreshes
 * the mirror, runs the key's apply hook and arms the save timer, so the apply
 * semantics are identical no matter which page did the writing.
 *
 * A setter that reports "unchanged" does no work at all. That short-circuit
 * matters: the nickname's hook puts a USER_CHANGE on the wire. */

int
gtkhx_prefs_type (const char *name)
{
    return hxconfig_type (name);
}

int
gtkhx_prefs_get_bool (const char *name)
{
    return hxconfig_get_bool (name) ? 1 : 0;
}

int
gtkhx_prefs_get_int (const char *name)
{
    return hxconfig_get_int (name);
}

/* Returns a g_malloc'd copy (caller frees with g_free); never NULL.
 *
 * hxconfig hands back a Rust allocation, which g_free must never see, so the
 * value is copied out and the original released through its own free. */
char *
gtkhx_prefs_get_string (const char *name)
{
    char *value = hxconfig_get_string (name);
    char *copy = g_strdup (value != NULL ? value : "");

    hxconfig_free_string (value);
    return copy;
}

void
gtkhx_prefs_set_int (const char *name, int val)
{
    if (hxconfig_set_int (name, val)) {
        pref_apply (name);
    }
}

void
gtkhx_prefs_set_string (const char *name, const char *val)
{
    if (val && hxconfig_set_string (name, val)) {
        pref_apply (name);
    }
}

/* Flip a BOOLEAN from outside the Settings window (e.g. the Tracker window's
 * case-sensitive toggle). When Settings happens to be showing a switch row for
 * this key, route through the row so its visible state stays in lockstep — its
 * notify::active handler then does the write. Otherwise write directly. */
void
gtkhx_prefs_set_bool (const char *name, int value)
{
    GtkWidget *row = pref_widget_lookup (name);
    int new_val = value ? 1 : 0;

    if (row && ADW_IS_SWITCH_ROW (row)) {
        if (adw_switch_row_get_active (ADW_SWITCH_ROW (row))
            != (new_val ? TRUE : FALSE)) {
            adw_switch_row_set_active (ADW_SWITCH_ROW (row),
                                       new_val ? TRUE : FALSE);
        }
        return;
    }

    if (hxconfig_set_bool (name, new_val)) {
        pref_apply (name);
    }
}

/* ------------------------------------------------------------------- *
 * AdwPreferencesRow helpers
 *
 * Each helper builds an AdwPreferencesRow subclass bound to one config key:
 * initialised from the key's current value, rendered insensitive if the schema
 * doesn't have the key in the shape the row needs, and wired to write back
 * (which applies + persists) on change.
 *
 * Wiring convention: the config name — always a string literal from cfgkeys.h,
 * so it outlives every widget — is passed straight through as the signal's
 * user data, and the row is registered in pref_widgets so anything that needs
 * to drive it while Settings is open can find it.
 *
 * No Cancel button — the dialog is live-apply. Closing it is the equivalent of
 * "OK", and each change is saved shortly after it happens, so a crash
 * mid-Settings doesn't lose the last toggle. */

static void
on_switch_row_active (AdwSwitchRow *row, GParamSpec *pspec, gpointer data)
{
    const char *name = data;
    (void)pspec;

    if (hxconfig_set_bool (name, adw_switch_row_get_active (row) ? 1 : 0)) {
        pref_apply (name);
    }
}

static GtkWidget *
pref_switch_row (const char *cfgname, const char *title, const char *subtitle)
{
    GtkWidget *row = adw_switch_row_new ();

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
    if (subtitle && *subtitle) {
        adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
    }
    if (hxconfig_type (cfgname) != BOOLEAN) {
        gtk_widget_set_sensitive (row, FALSE);
        return row;
    }
    adw_switch_row_set_active (ADW_SWITCH_ROW (row),
                               hxconfig_get_bool (cfgname) ? TRUE : FALSE);
    pref_widget_register (cfgname, row);
    g_signal_connect (row, "notify::active", G_CALLBACK (on_switch_row_active),
                      (gpointer)cfgname);
    return row;
}

/* per-row debounce for entry-row apply.
 *
 * AdwEntryRow's notify::text fires on every keystroke. For most string prefs
 * that's harmless (font name, download dir, etc.). But CFG_NICK's hook sends
 * an HTLC_HDR_USER_CHANGE on the wire: typing a 5-letter name produced 5
 * USER_CHANGE packets, with the server faithfully broadcasting each partial
 * prefix as the user's name to the rest of the chat.
 *
 * Coalesce: schedule the write on a 750 ms one-shot timer per row, cancelled
 * and re-armed by each subsequent keystroke. The timer ID and the config name
 * ride on the widget via g_object_set_data, so the row's lifetime owns them
 * and no parallel bookkeeping is needed.
 *
 * close_options_bookkeeping flushes pending timers so a window close
 * mid-keystroke doesn't lose the change. */
#define ENTRY_APPLY_DEBOUNCE_MS 750
#define ENTRY_TIMER_KEY "gtkhx-entry-apply-timer"
#define ENTRY_NAME_KEY "gtkhx-entry-pref-name"

static void
entry_commit (GtkWidget *row)
{
    const char *name = g_object_get_data (G_OBJECT (row), ENTRY_NAME_KEY);
    const char *txt = gtk_editable_get_text (GTK_EDITABLE (row));

    if (!name) {
        return;
    }
    if (hxconfig_set_string (name, txt ? txt : "")) {
        pref_apply (name);
    }
}

static gboolean
entry_apply_timeout_cb (gpointer data)
{
    GtkWidget *row = data;

    g_object_set_data (G_OBJECT (row), ENTRY_TIMER_KEY, GUINT_TO_POINTER (0));
    entry_commit (row);
    return G_SOURCE_REMOVE;
}

static void
entry_apply_schedule (GtkWidget *row)
{
    guint old = GPOINTER_TO_UINT (
        g_object_get_data (G_OBJECT (row), ENTRY_TIMER_KEY));
    guint id;

    if (old) {
        g_source_remove (old);
    }
    id = g_timeout_add (ENTRY_APPLY_DEBOUNCE_MS, entry_apply_timeout_cb, row);
    g_object_set_data (G_OBJECT (row), ENTRY_TIMER_KEY, GUINT_TO_POINTER (id));
}

/* Run a row's pending write now, if it has one. Safe on any widget — a row
 * that isn't a debounced entry never has a timer stashed. */
static void
entry_apply_flush (GtkWidget *row)
{
    guint id;

    if (!row) {
        return;
    }
    id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (row), ENTRY_TIMER_KEY));
    if (!id) {
        return;
    }
    g_source_remove (id);
    g_object_set_data (G_OBJECT (row), ENTRY_TIMER_KEY, GUINT_TO_POINTER (0));
    entry_commit (row);
}

static void
on_entry_row_text (AdwEntryRow *row, GParamSpec *pspec, gpointer data)
{
    (void)pspec;
    (void)data;
    entry_apply_schedule (GTK_WIDGET (row));
}

static GtkWidget *
pref_entry_row (const char *cfgname, const char *title)
{
    GtkWidget *row = adw_entry_row_new ();
    char *current;

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
    if (hxconfig_type (cfgname) != STRING) {
        gtk_widget_set_sensitive (row, FALSE);
        return row;
    }

    current = gtkhx_prefs_get_string (cfgname);
    gtk_editable_set_text (GTK_EDITABLE (row), current);
    g_free (current);

    g_object_set_data (G_OBJECT (row), ENTRY_NAME_KEY, (gpointer)cfgname);
    pref_widget_register (cfgname, row);
    g_signal_connect (row, "notify::text", G_CALLBACK (on_entry_row_text),
                      NULL);
    return row;
}

static void
on_spin_row_value (AdwSpinRow *row, GParamSpec *pspec, gpointer data)
{
    const char *name = data;
    (void)pspec;

    if (hxconfig_set_int (name, (int)adw_spin_row_get_value (row))) {
        pref_apply (name);
    }
}

static GtkWidget *
pref_spin_row (const char *cfgname, const char *title, const char *subtitle,
               double min, double max, double step)
{
    GtkWidget *row = adw_spin_row_new_with_range (min, max, step);
    int type = hxconfig_type (cfgname);

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
    if (subtitle && *subtitle) {
        adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
    }
    if (type != INT && type != UINT16) {
        gtk_widget_set_sensitive (row, FALSE);
        return row;
    }

    adw_spin_row_set_value (ADW_SPIN_ROW (row), hxconfig_get_int (cfgname));
    pref_widget_register (cfgname, row);
    g_signal_connect (row, "notify::value", G_CALLBACK (on_spin_row_value),
                      (gpointer)cfgname);
    return row;
}

/* Colored-Nicknames Settings row. An AdwActionRow with a
 * GtkColorDialogButton suffix (the picker itself) and a Clear button that
 * resets to HX_NICK_COLOR_NONE / theme default. */

static void
on_nick_color_changed (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    const char *name = user_data;
    const GdkRGBA *rgba;
    int packed;
    (void)pspec;

    rgba = gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (obj));
    if (!rgba) {
        return;
    }
    /* Pack as 0x00RRGGBB per fogWraith spec — high byte reserved. */
    packed = (int)(((guint32)(guint8)(rgba->red * 255.0 + 0.5) << 16)
                   | ((guint32)(guint8)(rgba->green * 255.0 + 0.5) << 8)
                   | (guint32)(guint8)(rgba->blue * 255.0 + 0.5));
    if (hxconfig_set_int (name, packed)) {
        pref_apply (name);
    }
}

static void
on_nick_color_clear (GtkButton *btn, gpointer user_data)
{
    const char *name = user_data;
    GtkColorDialogButton *picker
        = g_object_get_data (G_OBJECT (btn), "pref-color-picker");

    if (hxconfig_get_int (name) == -1) {
        return;
    }
    /* Reset the picker swatch to black so the user gets a clear "no color is
     * set" visual cue. Block the notify::rgba signal around the call so the
     * synthetic set doesn't fight the clear by packing 0x000000 back in. */
    if (picker) {
        GdkRGBA black = { 0, 0, 0, 1.0 };
        g_signal_handlers_block_by_func (picker, on_nick_color_changed,
                                         (gpointer)name);
        gtk_color_dialog_button_set_rgba (picker, &black);
        g_signal_handlers_unblock_by_func (picker, on_nick_color_changed,
                                           (gpointer)name);
    }
    if (hxconfig_set_int (name, -1)) {
        pref_apply (name);
    }
}

static GtkWidget *
pref_nick_color_row (void)
{
    GtkColorDialog *dialog;
    GtkWidget *picker, *clear;
    GtkWidget *row = adw_action_row_new ();
    int current;

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                   _ ("Nickname color"));
    adw_action_row_set_subtitle (
        ADW_ACTION_ROW (row),
        _ ("Optional RGB color shown on servers that support the "
           "Colored-Nicknames extension"));

    if (hxconfig_type (CFG_NICK_COLOR) != INT) {
        gtk_widget_set_sensitive (row, FALSE);
        return row;
    }

    /* GtkColorDialogButton (4.10+) supersedes the deprecated GtkColorButton;
     * it presents a GtkColorDialog when the user clicks the swatch and exposes
     * the picked colour via the `rgba` property. No separate ref on the dialog
     * is needed — the button retains it for the widget's lifetime. */
    dialog = gtk_color_dialog_new ();
    gtk_color_dialog_set_title (dialog, _ ("Pick Nickname Color"));
    picker = gtk_color_dialog_button_new (dialog);

    current = hxconfig_get_int (CFG_NICK_COLOR);
    if (current != -1) {
        guint32 packed = (guint32)current;
        GdkRGBA rgba
            = { ((packed >> 16) & 0xff) / 255.0, ((packed >> 8) & 0xff) / 255.0,
                (packed & 0xff) / 255.0, 1.0 };
        gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (picker),
                                          &rgba);
    }
    gtk_widget_set_valign (picker, GTK_ALIGN_CENTER);
    /* GtkColorButton's "color-set" was a per-pick notification; on
     * GtkColorDialogButton the equivalent is the notify::rgba property change,
     * which fires whenever the picked colour actually changes. */
    g_signal_connect (picker, "notify::rgba",
                      G_CALLBACK (on_nick_color_changed),
                      (gpointer)CFG_NICK_COLOR);

    clear = gtk_button_new_with_label (_ ("Clear"));
    gtk_widget_set_valign (clear, GTK_ALIGN_CENTER);
    g_object_set_data (G_OBJECT (clear), "pref-color-picker", picker);
    g_signal_connect (clear, "clicked", G_CALLBACK (on_nick_color_clear),
                      (gpointer)CFG_NICK_COLOR);

    adw_action_row_add_suffix (ADW_ACTION_ROW (row), picker);
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), clear);
    pref_widget_register (CFG_NICK_COLOR, picker);
    return row;
}

/* Only the Voice page builds a combo row, so the whole mechanism is behind
 * the same gate as that page — otherwise a build without voice carries two
 * functions nothing calls. */
#ifdef HAVE_VOICE
static void
on_combo_row_selected (AdwComboRow *row, GParamSpec *pspec, gpointer data)
{
    const char *name = data;
    GtkStringList *list;
    const char *selected;
    (void)pspec;

    list = GTK_STRING_LIST (
        g_object_get_data (G_OBJECT (row), "pref-combo-values"));
    selected = list ? gtk_string_list_get_string (
                          list, adw_combo_row_get_selected (row))
                    : NULL;
    if (!selected) {
        return;
    }
    if (hxconfig_set_string (name, selected)) {
        pref_apply (name);
    }
}

/* AdwComboRow with a fixed value list. `values[]` are the strings stored in
 * the preference; `labels[]` are user-visible (translatable) presentation. n
 * is the number of entries; arrays are not freed. */
static GtkWidget *
pref_combo_row (const char *cfgname, const char *title, const char **values,
                const char **labels, int n)
{
    GtkWidget *row = adw_combo_row_new ();
    GtkStringList *labels_model;
    GtkStringList *values_model;
    char *current;
    int i, selected = 0;

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
    if (hxconfig_type (cfgname) != STRING) {
        gtk_widget_set_sensitive (row, FALSE);
        return row;
    }

    labels_model = gtk_string_list_new (NULL);
    values_model = gtk_string_list_new (NULL);
    for (i = 0; i < n; i++) {
        gtk_string_list_append (labels_model, labels[i]);
        gtk_string_list_append (values_model, values[i]);
    }
    adw_combo_row_set_model (ADW_COMBO_ROW (row), G_LIST_MODEL (labels_model));
    g_object_set_data_full (G_OBJECT (row), "pref-combo-values", values_model,
                            g_object_unref);
    g_object_unref (labels_model);

    current = gtkhx_prefs_get_string (cfgname);
    for (i = 0; i < n; i++) {
        if (strcmp (current, values[i]) == 0) {
            selected = i;
            break;
        }
    }
    g_free (current);
    adw_combo_row_set_selected (ADW_COMBO_ROW (row), selected);
    pref_widget_register (cfgname, row);
    g_signal_connect (row, "notify::selected",
                      G_CALLBACK (on_combo_row_selected), (gpointer)cfgname);
    return row;
}
#endif /* HAVE_VOICE */

/* Runtime state that isn't a preference and so has no schema entry. Runs
 * before prefs_read, which then overwrites nothing here.
 *
 * This used to also stamp the compiled-in defaults into the prefs struct. It
 * doesn't any more: the defaults live in the Rust schema, and prefs_read hands
 * them to the mirror whether or not a file exists. */
void
init_variables (void)
{
    /* GdkRGBA defaults — light grey foreground on black, preserving the
     * historic 0xcccc/0xffff fraction. */
    fg_col.red = 0xcccc / 65535.0;
    fg_col.green = 0xcccc / 65535.0;
    fg_col.blue = 0xcccc / 65535.0;
    fg_col.alpha = 1.0;
    bg_col.red = 0.0;
    bg_col.green = 0.0;
    bg_col.blue = 0.0;
    bg_col.alpha = 1.0;

    /* A usable font description before the settings load runs changed_font,
     * so anything constructed in between measures against something real. */
    if (!gtkhx_font_desc) {
        gtkhx_font_desc = pango_font_description_from_string ("Monospace 10");
    }
}

/* The pre-hxconfig settings file, kept only so the one-shot import has
 * somewhere to read from: $CONFIG/gtkhxrc first, then ~/.gtkhxrc. Neither is
 * written any more, and the import leaves both in place — if this build gets
 * abandoned, the old one still works. */
static char *
prefs_legacy_path (void)
{
    const char *home = g_getenv ("HOME");

    if (!home || !*home) {
        home = g_get_home_dir ();
    }
    if (!home) {
        return NULL;
    }
    return g_build_filename (home, ".gtkhxrc", NULL);
}

void
prefs_read (void)
{
    char *legacy = g_build_filename (gtkhx_config_dir (), "gtkhxrc", NULL);
    char *legacy_home = prefs_legacy_path ();
    int n_warnings;
    int i;

    n_warnings = hxconfig_load (gtkhx_config_dir (), legacy, legacy_home);
    g_free (legacy);
    g_free (legacy_home);

    /* Loading never fails. Every failure mode degrades to the default and
     * records a diagnostic naming the path and what was wrong with it, so a
     * malformed number is reported rather than becoming the silent zero atoi()
     * used to make of it. */
    for (i = 0; i < n_warnings; i++) {
        char *w = hxconfig_warning (i);
        if (w) {
            g_message ("settings: %s", w);
            hxconfig_free_string (w);
        }
    }

    hx_prefs_mirror_refresh ();

    /* Seed the connection from the stored identity. This is the copy that
     * replaces the old aliasing, and it is why /nick no longer rewrites the
     * saved global nickname: /nick writes the connection, which nothing reads
     * back. */
    identity_copy_to_conn (hx_active_session ()->htlc);

    /* Apply every hook, not just the ones whose stored value differs from a
     * compiled-in default. The old loader did the latter by accident — the
     * apply lived inside the value-changed branch — so a setting saved at its
     * default never reached the runtime it configures, and a hand-written
     * function existed alongside to re-apply the ones where that mattered.
     * Applying uniformly deletes both the bug and the compensator. */
    for (i = 0; i < (int)G_N_ELEMENTS (pref_hooks); i++) {
        pref_hooks[i].fn (hx_active_session ());
    }

    /* No settings file anywhere — first run. Pop Settings, as before. */
    if (hxconfig_is_first_run ()) {
        create_options_window (NULL, NULL);
    }
}

/* Saving coalesces on a short timer with a synchronous flush at exit, the way
 * the dock layout already does it. Every Settings toggle used to rebuild and
 * rewrite the whole file synchronously.
 *
 * The window is short on purpose: it is there to collapse a burst (a drag
 * resize, a run of keystrokes that each already passed the row debounce), not
 * to defer the write far enough that a crash could lose it. */
#define PREFS_SAVE_DEBOUNCE_MS 250

static guint prefs_save_timer;

static void
prefs_flush (void)
{
    if (!hxconfig_flush ()) {
        g_warning ("Could not write gtkhx.toml — settings changed this "
                   "session may not survive a restart");
    }
}

static gboolean
prefs_save_timeout_cb (gpointer data)
{
    (void)data;
    prefs_save_timer = 0;
    prefs_flush ();
    return G_SOURCE_REMOVE;
}

void
hx_prefs_save_soon (void)
{
    if (prefs_save_timer != 0) {
        g_source_remove (prefs_save_timer);
    }
    prefs_save_timer
        = g_timeout_add (PREFS_SAVE_DEBOUNCE_MS, prefs_save_timeout_cb, NULL);
}

void
hx_prefs_save_now (void)
{
    if (prefs_save_timer != 0) {
        g_source_remove (prefs_save_timer);
        prefs_save_timer = 0;
    }
    prefs_flush ();
}

/* bookkeeping that runs on every dialog teardown path. Wired to
 * AdwDialog::closed (see create_options_window), which AdwDialog emits
 * once the dialog is actually closed — whether by Esc, the header-bar
 * close button, adw_dialog_close(), or the parent window going away.
 * That's the single teardown chokepoint for an AdwDialog (there's no
 * separate confirm-vs-destroy split like GtkWindow's close-request vs
 * destroy), so hooking it catches every path and guarantees
 * options_window never points at a freed GObject the next time
 * create_options_window runs. */
static void
close_options_bookkeeping (GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    options_window = 0;
    /* If the icon Browse popup is still up, dismiss it before we free
     * iv — its "closed" handler (on_icon_picker_closed) nulls the
     * pointers, and the guard there tolerates iv already being NULL. */
    if (iv && iv->picker_dialog) {
        adw_dialog_close (ADW_DIALOG (iv->picker_dialog));
    }
    g_free (iv);
    iv = NULL;

    /* The Tracker page (now Rust) owns its own GListStore + selection; they
     * drop with the dialog's widget tree, so there's nothing to clear here. */

    /* The rows registered in pref_widgets are children of the dialog and are
     * about to be destroyed with it, so a later lookup would hand back a freed
     * widget. Dropping the table is also what makes an empty lookup mean
     * "Settings isn't open".
     *
     * Flush any pending entry-row debounce timers first, so a close
     * mid-keystroke doesn't lose the change. */
    if (pref_widgets) {
        GHashTableIter iter;
        gpointer row;

        g_hash_table_iter_init (&iter, pref_widgets);
        while (g_hash_table_iter_next (&iter, NULL, &row)) {
            entry_apply_flush (row);
        }
        g_hash_table_destroy (pref_widgets);
        pref_widgets = NULL;
    }
}

/* No Interface page anymore — the new files browser is always a single
 * window. The retired FILE_SAMEWINDOW / NEWS_SAMEWINDOW keys are named in
 * hxconfig's migration map only so an old profile carrying them doesn't trip
 * the unknown-key diagnostic. */

/* ---- Custom GIF avatar picker (GIF-icons extension, Phase 10.C) --- */

/* GTKHX_AVATAR_MAX_BYTES (the spec-recommended 32 KiB upload cap) is
 * defined in gif_icons.h so the picker and the persistence layer share
 * one source of truth. We reject larger files client-side with a clear
 * message rather than earn a server rejection. (Auto-downscale is
 * deferred: gdk-pixbuf has no GIF encoder, so recompressing to GIF would
 * need ImageMagick.) */

/* Cap the preview decode. GdkPixbufLoader's "size-prepared" fires once
 * the header (dimensions) is parsed but before the full raster decode,
 * so a highly-compressed GIF that advertises huge dimensions gets scaled
 * down to a sane preview size here rather than allocating a giant canvas
 * on the UI thread. (The avatar that's actually sent/rendered still goes
 * through the bounded, sandboxed gif_avatar decoder; this only bounds the
 * local Settings preview.) */
#define AVATAR_PREVIEW_MAX_DIM 512
static void
avatar_loader_size_prepared (GdkPixbufLoader *ld, int w, int h, gpointer data)
{
    (void)data;
    int big = w > h ? w : h;
    if (big <= AVATAR_PREVIEW_MAX_DIM) {
        return;
    }
    double s = (double)AVATAR_PREVIEW_MAX_DIM / big;
    int nw = (int)(w * s);
    int nh = (int)(h * s);
    gdk_pixbuf_loader_set_size (ld, nw > 0 ? nw : 1, nh > 0 ? nh : 1);
}

static void
avatar_preview_from_gif (GtkWidget *preview, const guchar *bytes, gsize len)
{
    GdkTexture *tex = NULL;
    GdkPixbufLoader *ld = gdk_pixbuf_loader_new ();
    g_signal_connect (ld, "size-prepared",
                      G_CALLBACK (avatar_loader_size_prepared), NULL);
    /* close() must run exactly once regardless of how write() fared, so
     * sequence the two calls into separate statements rather than relying
     * on && short-circuit (which would skip close() when write() fails,
     * and tempt a second close() in an else branch). */
    gboolean wrote = gdk_pixbuf_loader_write (ld, bytes, len, NULL);
    gboolean closed = gdk_pixbuf_loader_close (ld, NULL);
    if (wrote && closed) {
        GdkPixbuf *pb = gdk_pixbuf_loader_get_pixbuf (ld); /* borrowed */
        if (pb) {
            tex = gtkhx_texture_from_pixbuf (pb);
        }
    }
    gtk_picture_set_paintable (GTK_PICTURE (preview),
                               tex ? GDK_PAINTABLE (tex) : NULL);
    g_clear_object (&tex);
    g_object_unref (ld);
}

static void
on_avatar_file_chosen (GObject *src, GAsyncResult *res, gpointer user_data)
{
    GtkWidget *preview = user_data; /* reffed by on_avatar_choose_clicked */
    GError *err = NULL;
    GFile *file
        = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (src), res, &err);

    if (!file) {
        if (err
            && !g_error_matches (err, GTK_DIALOG_ERROR,
                                 GTK_DIALOG_ERROR_DISMISSED)) {
            char *m
                = g_strdup_printf (_ ("File picker failed: %s"), err->message);
            toolbar_show_toast (m);
            g_free (m);
        }
        g_clear_error (&err);
        g_object_unref (preview);
        return;
    }

    /* Preflight the size before reading the whole file into memory —
     * picking a huge file shouldn't cause a long synchronous read +
     * allocation on the UI thread just to reject it afterward. One
     * stat() on local files; cheap. If size can't be queried (some
     * network mounts) we fall through and the post-read cap catches it. */
    GFileInfo *finfo = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                          G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (finfo) {
        goffset sz = g_file_info_get_size (finfo);
        g_object_unref (finfo);
        if (sz > GTKHX_AVATAR_MAX_BYTES) {
            /* Size is checked before the GIF-signature validation below,
             * so a large non-GIF lands here too — keep the wording neutral. */
            char *m = g_strdup_printf (
                _ ("That file is %.1f KB — the limit is %d KB. Pick a smaller "
                   "one."),
                sz / 1024.0, GTKHX_AVATAR_MAX_BYTES / 1024);
            toolbar_show_toast (m);
            g_free (m);
            g_object_unref (file);
            g_object_unref (preview);
            return;
        }
    }

    char *contents = NULL;
    gsize len = 0;
    GError *load_err = NULL;
    if (!g_file_load_contents (file, NULL, &contents, &len, NULL, &load_err)) {
        char *m = g_strdup_printf (_ ("Couldn't read the file: %s"),
                                   load_err ? load_err->message : "?");
        toolbar_show_toast (m);
        g_free (m);
        g_clear_error (&load_err);
        g_object_unref (file);
        g_object_unref (preview);
        return;
    }
    g_object_unref (file);

    if (len == 0) {
        toolbar_show_toast (_ ("That file is empty."));
        g_free (contents);
        g_object_unref (preview);
        return;
    }
    if (len > GTKHX_AVATAR_MAX_BYTES) {
        char *m = g_strdup_printf (
            _ ("That file is %.1f KB — the limit is %d KB. Pick a smaller "
               "one."),
            len / 1024.0, GTKHX_AVATAR_MAX_BYTES / 1024);
        toolbar_show_toast (m);
        g_free (m);
        g_free (contents);
        g_object_unref (preview);
        return;
    }
    if (!gtkhx_proto_gif_icon_is_gif ((const guint8 *)contents, len)) {
        toolbar_show_toast (_ ("That file isn't a GIF image."));
        g_free (contents);
        g_object_unref (preview);
        return;
    }

    /* Persist the choice regardless of the current connection, then
     * send it if (and only if) the live server supports the extension.
     * If not, it'll be sent automatically the next time we connect to a
     * capable server (hx_icon_send_saved, from the post-login probe). */
    if (!hx_icon_save ((const guint8 *)contents, len)) {
        toolbar_show_toast (
            _ ("Couldn't save the avatar to disk — check permissions."));
        g_free (contents);
        g_object_unref (preview);
        return;
    }
    avatar_preview_from_gif (preview, (const guchar *)contents, len);
    if (hx_conn_gif_icons_state (hx_active_session ()->htlc)
        == GIF_ICONS_SUPPORTED) {
        hx_icon_set (hx_active_session ()->htlc, (const guint8 *)contents, len);
        toolbar_show_toast (_ ("Avatar updated."));
    } else {
        toolbar_show_toast (_ ("Avatar saved — it'll be sent when you connect "
                               "to a server that supports GIF icons."));
    }
    g_free (contents);
    g_object_unref (preview);
}

static void
on_avatar_choose_clicked (GtkButton *btn, gpointer user_data)
{
    GtkWidget *preview = user_data; /* borrowed; reffed for the async call */
    GtkFileDialog *fd = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (fd, _ ("Choose GIF avatar"));

    GtkFileFilter *f = gtk_file_filter_new ();
    gtk_file_filter_set_name (f, _ ("GIF images"));
    gtk_file_filter_add_mime_type (f, "image/gif");
    GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
    g_list_store_append (filters, f);
    g_object_unref (f);
    gtk_file_dialog_set_filters (fd, G_LIST_MODEL (filters));
    g_object_unref (filters);

    GtkWindow *parent = NULL;
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (btn));
    if (GTK_IS_WINDOW (root)) {
        parent = GTK_WINDOW (root);
    }
    gtk_file_dialog_open (fd, parent, NULL, on_avatar_file_chosen,
                          g_object_ref (preview));
    g_object_unref (fd);
}

static void
on_avatar_clear_clicked (GtkButton *btn, gpointer user_data)
{
    GtkWidget *preview = user_data;
    (void)btn;
    gboolean removed = hx_icon_forget ();
    gtk_picture_set_paintable (GTK_PICTURE (preview), NULL);
    /* Tell the server to drop it too, if we're on a capable one. */
    if (hx_conn_gif_icons_state (hx_active_session ()->htlc)
        == GIF_ICONS_SUPPORTED) {
        hx_icon_clear (hx_active_session ()->htlc);
    }
    /* Don't claim it's cleared if the persisted file survived deletion —
     * it'll reload and re-send next start. */
    toolbar_show_toast (removed ? _ ("Avatar cleared.")
                                : _ ("Avatar cleared for now, but the saved "
                                     "file could not be deleted."));
}

/* --- Identity icon: inline preview + Browse popup ----------------- *
 *
 * The Identity page shows the currently-selected icon next to an
 * editable numeric ID (an AdwSpinRow) plus a "Browse…" button. Browse
 * pops a grid of every icon in the loaded resource files; activating
 * one stamps its ID onto the spin row (which repaints the inline
 * preview) and closes the popup. */

/* Render icon `resid` from the loaded resource files into `picture`.
 * load_icon walks icon_files by ID and hands back an owned pixbuf,
 * falling back to the default icon when the ID isn't found. */
static void
identity_icon_preview_update (GtkWidget *picture, guint resid)
{
    GdkPixbuf *pb = NULL;

    load_icon (NULL, (guint16)resid, &icon_files, 1, &pb, NULL);
    if (pb) {
        GdkTexture *tex = gtkhx_texture_from_pixbuf (pb);
        gtk_picture_set_paintable (GTK_PICTURE (picture),
                                   tex ? GDK_PAINTABLE (tex) : NULL);
        if (tex) {
            g_object_unref (tex);
        }
        g_object_unref (pb);
    } else {
        gtk_picture_set_paintable (GTK_PICTURE (picture), NULL);
    }
}

/* notify::value on the icon-ID spin row — repaint the inline preview.
 * Persistence is handled separately by on_spin_row_value (also wired by
 * pref_spin_row); this handler only touches the preview. */
static void
identity_icon_value_changed (GObject *row, GParamSpec *pspec, gpointer data)
{
    GtkWidget *preview = data;
    (void)pspec;
    identity_icon_preview_update (
        preview, (guint)adw_spin_row_get_value (ADW_SPIN_ROW (row)));
}

/* The Browse popup closed (selection, Esc, or window close): drop our
 * dangling pointers into its now-destroyed flowboxes. */
static void
on_icon_picker_closed (AdwDialog *dlg, gpointer data)
{
    (void)dlg;
    (void)data;
    if (iv) {
        iv->picker_dialog = NULL;
        iv->icon_list = NULL;
        iv->wide_list = NULL;
    }
}

/* Build one of the picker's flowboxes with the shared configuration.
 * `wide` selects the one-per-row banner strip vs. the multi-column grid
 * — the same two-flowbox split the inline picker used before. */
static GtkWidget *
icon_picker_make_flowbox (gboolean wide)
{
    GtkWidget *fb = gtk_flow_box_new ();

    gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (fb), GTK_SELECTION_SINGLE);
    gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (fb), wide ? FALSE : TRUE);
    gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (fb), wide ? 1 : 2);
    gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (fb), wide ? 1 : 4);
    gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (fb), 4);
    gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (fb), 4);
    gtk_flow_box_set_sort_func (GTK_FLOW_BOX (fb), icon_picker_sort_cb, NULL,
                                NULL);
    g_signal_connect (fb, "child-activated",
                      G_CALLBACK (icon_flow_child_activated), iv);
    return fb;
}

/* Deferred to idle so the popup's first layout pass has run: find the
 * flowbox child whose resid matches the current icon ID, select it, and
 * grab its focus — a focused child inside a GtkScrolledWindow gets
 * scrolled into view automatically. */
static gboolean
icon_picker_scroll_to_selected (gpointer data)
{
    guint want = GPOINTER_TO_UINT (data);
    GtkFlowBox *boxes[2];
    int b;

    if (!iv) {
        return G_SOURCE_REMOVE;
    }
    boxes[0] = iv->icon_list ? GTK_FLOW_BOX (iv->icon_list) : NULL;
    boxes[1] = iv->wide_list ? GTK_FLOW_BOX (iv->wide_list) : NULL;

    for (b = 0; b < 2; b++) {
        GtkWidget *child;

        if (!boxes[b]) {
            continue;
        }
        for (child = gtk_widget_get_first_child (GTK_WIDGET (boxes[b])); child;
             child = gtk_widget_get_next_sibling (child)) {
            guint id;

            if (!GTK_IS_FLOW_BOX_CHILD (child)) {
                continue;
            }
            id = GPOINTER_TO_UINT (
                g_object_get_data (G_OBJECT (child), "resid"));
            if (id == want) {
                gtk_flow_box_select_child (boxes[b],
                                           GTK_FLOW_BOX_CHILD (child));
                gtk_widget_grab_focus (child);
                return G_SOURCE_REMOVE;
            }
        }
    }
    return G_SOURCE_REMOVE;
}

/* "Browse…" clicked — pop the icon grid, presented on the Settings
 * dialog. list_icons() fills the flowboxes once they exist. */
static void
on_icon_browse_clicked (GtkButton *btn, gpointer data)
{
    AdwDialog *dlg;
    GtkWidget *tv, *scroll, *picker_box;
    (void)btn;
    (void)data;

    if (!iv) {
        return;
    }

    /* Already open — bring the existing popup forward rather than
     * spawning a second one (which would orphan the first with stale
     * iv->picker_dialog / icon_list / wide_list pointers). */
    if (iv->picker_dialog) {
        adw_dialog_present (ADW_DIALOG (iv->picker_dialog), options_window);
        return;
    }

    dlg = ADW_DIALOG (adw_dialog_new ());
    adw_dialog_set_title (dlg, _ ("Choose Icon"));
    adw_dialog_set_content_width (dlg, 420);
    adw_dialog_set_content_height (dlg, 520);
    gtk_widget_set_size_request (GTK_WIDGET (dlg), 300, 360);
    gtkhx_dialog_add_close_shortcuts (GTK_WIDGET (dlg));

    picker_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top (picker_box, 6);
    gtk_widget_set_margin_bottom (picker_box, 6);
    gtk_widget_set_margin_start (picker_box, 6);
    gtk_widget_set_margin_end (picker_box, 6);
    iv->icon_list = icon_picker_make_flowbox (FALSE);
    iv->wide_list = icon_picker_make_flowbox (TRUE);
    gtk_box_append (GTK_BOX (picker_box), iv->icon_list);
    gtk_box_append (GTK_BOX (picker_box), iv->wide_list);

    scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand (scroll, TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), picker_box);

    tv = adw_toolbar_view_new ();
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), adw_header_bar_new ());
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), scroll);
    adw_dialog_set_child (dlg, tv);

    iv->picker_dialog = GTK_WIDGET (dlg);
    g_signal_connect (dlg, "closed", G_CALLBACK (on_icon_picker_closed), NULL);

    adw_dialog_present (dlg, options_window);

    list_icons ();

    /* Preselect + scroll to the icon currently set on the spin row. */
    {
        GtkWidget *spin = pref_widget_lookup (CFG_ICON);
        guint cur = 0;

        if (spin && ADW_IS_SPIN_ROW (spin)) {
            cur = (guint)adw_spin_row_get_value (ADW_SPIN_ROW (spin));
        }
        g_idle_add (icon_picker_scroll_to_selected, GUINT_TO_POINTER (cur));
    }
}

/* Phase 5 follow-up: the old standalone General page (just NICK) folds
 * into the Identity page since they're both "who am I to the server"
 * settings. Display name first, then icon ID, then the resource picker.
 *
 * The icon picker is a GtkFlowBox of 64x64 pixel-art icons in a
 * multi-column grid. See icon_flow_child_activated for the click
 * handler. */
static void
settings_page_identity (AdwPreferencesPage *page)
{
    AdwPreferencesGroup *name_grp, *id_grp;

    /* g_malloc0 — zero-fill the struct so any read of
     * iv->icon_list / nfound / icon_high / picker_dialog before the
     * Browse popup sets them returns 0 / NULL deterministically. */
    iv = g_malloc0 (sizeof (struct icon_viewer));

    name_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (name_grp, _ ("Display name"));
    adw_preferences_group_add (name_grp,
                               pref_entry_row (CFG_NICK, _ ("Your name")));
    adw_preferences_group_add (name_grp, pref_nick_color_row ());
    adw_preferences_page_add (page, name_grp);

    id_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (id_grp, _ ("Identity icon"));
    {
        /* pref_spin_row hands back an AdwSpinRow (an AdwActionRow
         * subclass), so we can hang a live preview of the selected icon
         * on the prefix and a "Browse…" button (which pops the full
         * icon grid) on the suffix. The numeric ID stays directly
         * editable in the spin entry. */
        GtkWidget *icon_row = pref_spin_row (
            CFG_ICON, _ ("Icon ID"),
            _ ("Numeric ID from the loaded icon resource files"), 0, 65535, 1);

        if (ADW_IS_SPIN_ROW (icon_row)) {
            GtkWidget *preview = gtk_picture_new ();
            GtkWidget *browse;

            gtk_widget_set_size_request (preview, 40, 40);
            gtk_widget_set_valign (preview, GTK_ALIGN_CENTER);
            gtk_picture_set_content_fit (GTK_PICTURE (preview),
                                         GTK_CONTENT_FIT_CONTAIN);
            identity_icon_preview_update (
                preview,
                (guint)adw_spin_row_get_value (ADW_SPIN_ROW (icon_row)));
            g_signal_connect (icon_row, "notify::value",
                              G_CALLBACK (identity_icon_value_changed),
                              preview);
            adw_action_row_add_prefix (ADW_ACTION_ROW (icon_row), preview);

            browse = gtk_button_new_with_label (_ ("Browse…"));
            gtk_widget_set_valign (browse, GTK_ALIGN_CENTER);
            g_signal_connect (browse, "clicked",
                              G_CALLBACK (on_icon_browse_clicked), NULL);
            adw_action_row_add_suffix (ADW_ACTION_ROW (icon_row), browse);
        }
        adw_preferences_group_add (id_grp, icon_row);
    }
    adw_preferences_page_add (page, id_grp);

    /* Custom GIF avatar (GIF-icons extension, Phase 10.C). Independent
     * of the numeric icon above — a GIF other capable clients see in
     * place of your icon, rendered like a normal icon / wide banner.
     * You can pick one any time; it's persisted ($CONFIG/avatar.gif)
     * and sent automatically once you're on a server that supports the
     * extension, so the picker is never gated on the live connection. */
    {
        AdwPreferencesGroup *gif_grp
            = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
        adw_preferences_group_set_title (gif_grp, _ ("Custom GIF avatar"));
        adw_preferences_group_set_description (
            gif_grp,
            _ ("A GIF other users see in place of your icon. Best authored at "
               "icon size (or wide-banner size); max 32 KB. Sent "
               "automatically when you connect to a server that supports it."));

        AdwActionRow *gif_row = ADW_ACTION_ROW (adw_action_row_new ());
        adw_preferences_row_set_title (ADW_PREFERENCES_ROW (gif_row),
                                       _ ("Avatar"));

        GtkWidget *preview = gtk_picture_new ();
        gtk_widget_set_size_request (preview, 48, 48);
        gtk_widget_set_valign (preview, GTK_ALIGN_CENTER);
        gtk_picture_set_content_fit (GTK_PICTURE (preview),
                                     GTK_CONTENT_FIT_CONTAIN);
        /* Seed from the saved avatar (the user's choice), not the live
         * per-session cache — so it shows even before connecting. */
        {
            GBytes *saved = hx_icon_load_saved ();
            if (saved) {
                gsize slen = 0;
                const guchar *sdata = g_bytes_get_data (saved, &slen);
                avatar_preview_from_gif (preview, sdata, slen);
                g_bytes_unref (saved);
            }
        }
        adw_action_row_add_prefix (gif_row, preview);

        GtkWidget *choose = gtk_button_new_with_label (_ ("Choose…"));
        gtk_widget_set_valign (choose, GTK_ALIGN_CENTER);
        g_signal_connect (choose, "clicked",
                          G_CALLBACK (on_avatar_choose_clicked), preview);
        GtkWidget *clear = gtk_button_new_with_label (_ ("Clear"));
        gtk_widget_set_valign (clear, GTK_ALIGN_CENTER);
        g_signal_connect (clear, "clicked",
                          G_CALLBACK (on_avatar_clear_clicked), preview);
        adw_action_row_add_suffix (gif_row, choose);
        adw_action_row_add_suffix (gif_row, clear);

        adw_preferences_group_add (gif_grp, GTK_WIDGET (gif_row));

        /* Animate avatars (Phase 10.D). Off renders the still first
         * frame; per-user pause (click / right-click) is separate. */
        adw_preferences_group_add (
            gif_grp,
            pref_switch_row (CFG_ANIMATE_AVATARS, _ ("Animate GIF avatars"),
                             _ ("Play animated avatars in the user list. Turn "
                                "off to show a still frame.")));

        adw_preferences_page_add (page, gif_grp);
    }
}

#ifdef HAVE_VOICE
/* Phase 8.E: Voice device pickers. Queries gtkhx_voice_list_input_
 * /output_devices at page-build time (no live monitoring yet — a
 * Settings re-open after plugging a new mic re-runs the scan), then
 * builds a "System default" + per-device combo via the existing
 * pref_combo_row machinery. The values array stores stable
 * gst::Device::name()s; the labels array stores display_name()s; the
 * empty-string sentinel is the "use autoaudiosrc/autoaudiosink"
 * choice. The schema's string type handles persistence; the
 * change-callbacks (changed_voice_input_device /
 * changed_voice_output_device) push the new value through to the
 * Rust runtime via FFI. */

/* ============================================================ */
/* Push-to-talk: enable toggle + key capture row.                */
/* ============================================================ */

/* Refresh the action row's subtitle to reflect the current bind
 * (or "Not set" when CFG_VOICE_PTT_KEY is empty / unparseable).
 * The visible label is the canonical spec string itself ("F8",
 * "<Control>F12") — that's the same vocabulary the user sees in
 * any GTK / GNOME accelerator UI, so no translation gymnastics.
 *
 * The stored spec is validated via the same parser the runtime
 * hook uses; a corrupt or out-of-vocabulary prefs value reads as
 * "Not set" here so Settings reflects the EFFECTIVE bind (what
 * the runtime would honour) rather than the bytes on disk. */
static void
ptt_row_refresh_subtitle (AdwActionRow *row)
{
    const char *spec = gtkhx_prefs.voice_ptt_key;
    if (spec && *spec && hx_voice_ptt_keyspec_parse (spec, NULL, NULL)) {
        adw_action_row_set_subtitle (row, spec);
    } else {
        adw_action_row_set_subtitle (row, _ ("Not set — click to capture"));
    }
}

/* Persist the captured spec. The by-name setter refreshes the mirror that
 * ptt_row_refresh_subtitle reads back, so the row's subtitle and the stored
 * value can't disagree. */
static void
ptt_save_key_spec (const char *new_spec)
{
    gtkhx_prefs_set_string (CFG_VOICE_PTT_KEY, new_spec ? new_spec : "");
}

/* Key-pressed handler installed on the capture dialog. Returns
 * TRUE for any keyval — we consume every keypress so the user
 * can't accidentally trigger app-wide shortcuts during capture.
 * Valid key → write + close. Escape → close without writing.
 * Anything else → flash an error label and stay open. */
static gboolean
ptt_capture_key_pressed (GtkEventControllerKey *ctrl, guint keyval,
                         guint keycode, GdkModifierType state,
                         gpointer user_data)
{
    AdwAlertDialog *dlg = user_data;
    AdwActionRow *parent_row
        = g_object_get_data (G_OBJECT (dlg), "ptt-parent-row");
    GtkLabel *err_lbl = g_object_get_data (G_OBJECT (dlg), "ptt-err-label");
    (void)ctrl;
    (void)keycode;

    if (keyval == GDK_KEY_Escape
        && (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) == 0) {
        adw_dialog_close (ADW_DIALOG (dlg));
        return TRUE;
    }
    if (!hx_voice_ptt_keyspec_allowed (keyval, state)) {
        /* Show the dialog's error label so the user knows why
         * their key was rejected. The text spells out the
         * vocabulary in one line; localisable. Reveal the
         * label widget too — it's hidden by default so the
         * dialog body doesn't reserve a blank row. */
        if (err_lbl) {
            gtk_label_set_text (err_lbl,
                                _ ("That key would conflict with chat typing. "
                                   "Try a function key (F1–F24), Pause, or a "
                                   "Ctrl/Alt-modified combination."));
            gtk_widget_set_visible (GTK_WIDGET (err_lbl), TRUE);
        }
        return TRUE;
    }
    char *spec = hx_voice_ptt_keyspec_canonicalize (keyval, state);
    if (!spec) {
        return TRUE;
    }
    ptt_save_key_spec (spec);
    g_free (spec);
    if (parent_row) {
        ptt_row_refresh_subtitle (parent_row);
    }
    adw_dialog_close (ADW_DIALOG (dlg));
    return TRUE;
}

/* Open the capture dialog. The dialog is a tiny AdwAlertDialog
 * (no buttons; user presses a key or Escape to cancel). */
static void
ptt_open_capture_dialog (AdwActionRow *parent_row)
{
    AdwDialog *dlg = adw_alert_dialog_new (
        _ ("Capture push-to-talk key"),
        _ ("Press the key (or modifier+key combination) you want to "
           "use as your push-to-talk binding.\n\n"
           "Accepted: F1–F24, Pause, Scroll Lock, Insert, Print, "
           "Menu, or any Ctrl/Alt/Super combination with another "
           "key. Plain letters and digits are rejected so they "
           "don't conflict with chat input.\n\n"
           "Press Escape to cancel."));
    /* Inline error label, hidden until a rejected key tries to
     * land — `gtk_widget_set_visible (FALSE)` rather than just an
     * empty label so the dialog body doesn't reserve a blank row
     * underneath the prompt at first paint. AdwAlertDialog doesn't
     * expose its body label directly; we add a GtkLabel via the
     * dialog's "extra-child" slot and toggle visibility from
     * ptt_capture_key_pressed. */
    GtkWidget *err = gtk_label_new (NULL);
    gtk_widget_add_css_class (err, "error");
    gtk_label_set_wrap (GTK_LABEL (err), TRUE);
    gtk_label_set_xalign (GTK_LABEL (err), 0.0);
    gtk_widget_set_visible (err, FALSE);
    adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dlg), err);
    g_object_set_data (G_OBJECT (dlg), "ptt-err-label", err);
    g_object_set_data (G_OBJECT (dlg), "ptt-parent-row", parent_row);

    /* The key controller has to be installed on the dialog widget
     * itself so it sees keys while the dialog has focus. */
    GtkEventController *kctrl = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (kctrl, GTK_PHASE_CAPTURE);
    g_signal_connect (kctrl, "key-pressed",
                      G_CALLBACK (ptt_capture_key_pressed), dlg);
    gtk_widget_add_controller (GTK_WIDGET (dlg), kctrl);

    adw_dialog_present (dlg, GTK_WIDGET (parent_row));
}

/* AdwActionRow's "activated" fires on click + Enter. Open the
 * capture dialog from here so both interaction paths work. */
static void
on_ptt_row_activated (AdwActionRow *row, gpointer user_data)
{
    (void)user_data;
    ptt_open_capture_dialog (row);
}

/* Suffix "Clear" button — wipes the current bind back to "Not set"
 * without opening the capture dialog. Useful for the "I bound the
 * wrong key and want to start over" flow. */
static void
on_ptt_clear_clicked (GtkButton *btn, gpointer user_data)
{
    AdwActionRow *row = user_data;
    (void)btn;
    ptt_save_key_spec ("");
    ptt_row_refresh_subtitle (row);
}

/* Build the AdwPreferencesGroup and add it to the page. The group
 * has two rows: the boolean enable toggle (standard pref_switch_row)
 * and the key capture action row built by hand. */
static void
settings_page_voice_ptt_group (AdwPreferencesPage *page)
{
    AdwPreferencesGroup *grp
        = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (grp, _ ("Push-to-Talk"));
    adw_preferences_group_set_description (
        grp, _ ("When enabled, you start muted and unmute by holding the "
                "captured key. Works from any focused widget in the GtkHx "
                "window."));

    /* Enable toggle. */
    adw_preferences_group_add (grp, pref_switch_row (CFG_VOICE_PTT_ENABLED,
                                                     _ ("Enable push-to-talk"),
                                                     NULL));

    /* Key capture row. */
    AdwActionRow *key_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (key_row),
                                   _ ("PTT key"));
    /* AdwActionRow inherits from GtkListBoxRow; activatable is the
     * row-level "click / Enter fires 'activated'" toggle. */
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (key_row), TRUE);
    g_signal_connect (key_row, "activated", G_CALLBACK (on_ptt_row_activated),
                      NULL);

    /* Suffix Clear button. */
    GtkWidget *clear_btn
        = gtk_button_new_from_icon_name ("edit-clear-symbolic");
    gtk_widget_set_valign (clear_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (clear_btn, _ ("Clear PTT key"));
    gtk_widget_add_css_class (clear_btn, "flat");
    g_signal_connect (clear_btn, "clicked", G_CALLBACK (on_ptt_clear_clicked),
                      key_row);
    adw_action_row_add_suffix (key_row, clear_btn);

    ptt_row_refresh_subtitle (key_row);
    adw_preferences_group_add (grp, GTK_WIDGET (key_row));
    adw_preferences_page_add (page, grp);
}

static void
settings_page_voice (AdwPreferencesPage *page)
{
    AdwPreferencesGroup *devices_grp;
    gtkhx_voice_device_list *inputs = gtkhx_voice_list_input_devices ();
    gtkhx_voice_device_list *outputs = gtkhx_voice_list_output_devices ();
    size_t n_in = inputs ? gtkhx_voice_device_list_len (inputs) : 0;
    size_t n_out = outputs ? gtkhx_voice_device_list_len (outputs) : 0;
    /* +1 for the leading "System default" entry whose value is "". */
    const char **in_vals = g_new (const char *, n_in + 1);
    const char **in_labels = g_new (const char *, n_in + 1);
    const char **out_vals = g_new (const char *, n_out + 1);
    const char **out_labels = g_new (const char *, n_out + 1);
    size_t i;

    in_vals[0] = "";
    in_labels[0] = _ ("System default");
    for (i = 0; i < n_in; i++) {
        in_vals[i + 1] = gtkhx_voice_device_list_name (inputs, i);
        in_labels[i + 1] = gtkhx_voice_device_list_display_name (inputs, i);
    }

    out_vals[0] = "";
    out_labels[0] = _ ("System default");
    for (i = 0; i < n_out; i++) {
        out_vals[i + 1] = gtkhx_voice_device_list_name (outputs, i);
        out_labels[i + 1] = gtkhx_voice_device_list_display_name (outputs, i);
    }

    devices_grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (devices_grp, _ ("Audio Devices"));
    adw_preferences_group_set_description (
        devices_grp,
        _ ("Capture and playback devices for voice chat. "
           "\"System default\" follows your desktop's audio configuration. "
           "Changes take effect the next time you join a voice room."));
    adw_preferences_group_add (
        devices_grp,
        pref_combo_row (CFG_VOICE_INPUT_DEVICE, _ ("Input (microphone)"),
                        in_vals, in_labels, (int)(n_in + 1)));
    adw_preferences_group_add (
        devices_grp,
        pref_combo_row (CFG_VOICE_OUTPUT_DEVICE, _ ("Output (speakers)"),
                        out_vals, out_labels, (int)(n_out + 1)));
    adw_preferences_page_add (page, devices_grp);

    /* pref_combo_row's gtk_string_list_append copies each string into
     * its own GtkStringList, so freeing the parallel arrays here is
     * safe. The underlying char* pointers for index >= 1 live as long
     * as the device list does — we hold those lists alive for the
     * page's lifetime by stashing them on the page widget so a later
     * close-then-reopen rebuilds against a fresh scan. */
    g_free (in_vals);
    g_free (in_labels);
    g_free (out_vals);
    g_free (out_labels);
    g_object_set_data_full (G_OBJECT (page), "voice-input-devices", inputs,
                            (GDestroyNotify)gtkhx_voice_device_list_free);
    g_object_set_data_full (G_OBJECT (page), "voice-output-devices", outputs,
                            (GDestroyNotify)gtkhx_voice_device_list_free);

    /* Push-to-talk: toggle + key capture. The toggle binds via the
     * normal boolean row flow; the key capture is bespoke
     * because the row's content (subtitle = current bind, with a
     * Clear button) and its interaction (click → capture dialog,
     * Escape → cancel, valid key → write the canonical spec back)
     * don't fit any of the generic pref_* row helpers. */
    settings_page_voice_ptt_group (page);
}
#endif /* HAVE_VOICE */

/* Sidebar-driven Settings navigation.
 *
 * Settings was previously an AdwPreferencesDialog, whose built-in
 * navigation is a top AdwViewSwitcher (icon tabs across the header,
 * adaptively collapsing to a bottom bar). This rebuilds it as a plain
 * AdwDialog wrapping an AdwNavigationSplitView: a left GtkListBox
 * sidebar of categories — grouped under section headers via the
 * list-box header func — and a right content pane that swaps
 * AdwPreferencesPage children in a GtkStack. The settings_page_*()
 * draw functions are unchanged: each still fills an AdwPreferencesPage,
 * only the outer container differs. (Note: dropping AdwPreferencesDialog
 * also drops its built-in search entry — the sidebar is the navigation
 * affordance now.)
 *
 * AdwNavigationSplitView is available since libadwaita 1.4 and the
 * sidebar is a plain GtkListBox with the .navigation-sidebar style
 * class, so this needs no bump of the meson libadwaita pin. */

#ifndef N_
#define N_(s) (s)
#endif

struct settings_entry {
    const char *section; /* section header, or NULL to continue previous */
    const char *name;    /* stable GtkStack child name */
    const char *title;   /* sidebar + content-header display title */
    const char *icon;    /* symbolic icon name */
    void (*draw) (AdwPreferencesPage *);
};

/* Flat sidebar list, grouped under section headers. Order here is the
 * order rows appear; a non-NULL .section starts a new header group. */
/* Rust page builders (gtkhx-ui options.rs) — build the ported
 * pages' content into a C-created AdwPreferencesPage. The two custom-widget
 * pages (Identity + Voice) stay C for now and keep their C draw functions
 * below. */
extern void gtkhx_options_rs_page_general (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_file_transfers (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_chat_appearance (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_chat_behavior (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_chat_history (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_chat_emoji (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_notify_events (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_notify_behavior (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_sound (AdwPreferencesPage *);
extern void gtkhx_options_rs_page_tracker (AdwPreferencesPage *);

static const struct settings_entry settings_entries[] = {
    { N_ ("General"), "general", N_ ("General"), "preferences-system-symbolic",
      gtkhx_options_rs_page_general },
    { NULL, "identity", N_ ("Identity"), "user-info-symbolic",
      settings_page_identity },
    { NULL, "filexfer", N_ ("File Transfers"), "folder-download-symbolic",
      gtkhx_options_rs_page_file_transfers },
    { N_ ("Chat"), "chat_appearance", N_ ("Appearance"),
      "user-available-symbolic", gtkhx_options_rs_page_chat_appearance },
    { NULL, "chat_behavior", N_ ("Behavior"), "preferences-other-symbolic",
      gtkhx_options_rs_page_chat_behavior },
    { NULL, "chat_history", N_ ("History"), "document-open-recent-symbolic",
      gtkhx_options_rs_page_chat_history },
    { NULL, "chat_emoji", N_ ("Emoji"), "face-smile-symbolic",
      gtkhx_options_rs_page_chat_emoji },
    { N_ ("Notifications"), "notify_events", N_ ("Events"),
      "preferences-system-notifications-symbolic",
      gtkhx_options_rs_page_notify_events },
    { NULL, "notify_behavior", N_ ("Behavior"), "preferences-other-symbolic",
      gtkhx_options_rs_page_notify_behavior },
    { N_ ("Audio"), "sound", N_ ("Sound"), "audio-speakers-symbolic",
      gtkhx_options_rs_page_sound },
#ifdef HAVE_VOICE
    { NULL, "voice", N_ ("Voice"), "audio-input-microphone-symbolic",
      settings_page_voice },
#endif
    { N_ ("Network"), "trackers", N_ ("Trackers"), "network-server-symbolic",
      gtkhx_options_rs_page_tracker },
};

/* GtkListBox header func: draw a section label above the first row of
 * each section. Every row carries its (already-translated) section text
 * in "section" qdata, so a header is inserted whenever a row's section
 * differs from the row above it. */
static void
settings_sidebar_header (GtkListBoxRow *row, GtkListBoxRow *before,
                         gpointer data)
{
    const char *section = g_object_get_data (G_OBJECT (row), "section");
    const char *prev
        = before ? g_object_get_data (G_OBJECT (before), "section") : NULL;
    GtkWidget *label;
    (void)data;

    if (!section || (before && g_strcmp0 (section, prev) == 0)) {
        gtk_list_box_row_set_header (row, NULL);
        return;
    }

    label = gtk_label_new (section);
    gtk_widget_add_css_class (label, "heading");
    gtk_widget_add_css_class (label, "dim-label");
    gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
    gtk_widget_set_margin_start (label, 12);
    gtk_widget_set_margin_end (label, 12);
    gtk_widget_set_margin_top (label, before ? 12 : 6);
    gtk_widget_set_margin_bottom (label, 3);
    gtk_list_box_row_set_header (row, label);
}

/* Sidebar selection → swap the content stack, retitle the content
 * header, and (when collapsed) navigate to the content pane. The
 * split view / stack / content page are stashed as qdata on the
 * list box so this handler needs no module-static state. */
static void
settings_row_selected (GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    GtkStack *stack;
    AdwNavigationSplitView *split;
    AdwNavigationPage *content;
    const char *name, *title;
    (void)data;

    if (!row) {
        return;
    }
    stack = g_object_get_data (G_OBJECT (box), "stack");
    split = g_object_get_data (G_OBJECT (box), "split");
    content = g_object_get_data (G_OBJECT (box), "content-page");
    name = g_object_get_data (G_OBJECT (row), "page-name");
    title = g_object_get_data (G_OBJECT (row), "page-title");

    if (stack && name) {
        gtk_stack_set_visible_child_name (stack, name);
    }
    if (content && title) {
        adw_navigation_page_set_title (content, title);
    }
    if (split) {
        adw_navigation_split_view_set_show_content (split, TRUE);
    }
}

void
create_options_window (GtkWidget *widget, gpointer data)
{
    AdwDialog *dlg;
    AdwNavigationSplitView *split;
    AdwNavigationPage *sidebar_page, *content_page;
    AdwToolbarView *sidebar_tv, *content_tv;
    AdwBreakpoint *bp;
    GtkWidget *listbox, *sidebar_scroll, *stack;
    GtkWidget *parent;
    GtkListBoxRow *first_row = NULL;
    GValue collapsed = G_VALUE_INIT;
    const char *cur_section = NULL;
    size_t i;
    session *sess = data;

    (void)widget;

    parent = GTK_WIDGET (gtkhx_active_window ());

    if (options_window) {
        adw_dialog_present (ADW_DIALOG (options_window), parent);
        return;
    }

    /* Outer container is a plain AdwDialog (the project's meson floor is
     * libadwaita >= 1.6): it
     * auto-handles transient_for / modal-against-parent / adaptive
     * sizing. content_width is the *preferred* size and must be wide
     * enough for the sidebar + a preferences page side-by-side, or Adw
     * warns "AdwNavigationSplitView exceeds AdwDialog width". The
     * width/height-request set the collapsed *minimum* — without them
     * Adw warns "AdwDialog does not have a minimum size". Below the
     * breakpoint the split view collapses to a single navigable pane,
     * so the minimum only needs to fit one pane. */
    dlg = ADW_DIALOG (adw_dialog_new ());
    adw_dialog_set_title (dlg, _ ("GtkHx Preferences"));
    adw_dialog_set_content_width (dlg, 920);
    adw_dialog_set_content_height (dlg, 680);
    gtk_widget_set_size_request (GTK_WIDGET (dlg), 360, 480);

    /* Esc closes via AdwDialog's built-in close_response; wire Ctrl+W
     * (close) and Ctrl+Q (app.quit) for keyboard parity. */
    gtkhx_dialog_add_close_shortcuts (GTK_WIDGET (dlg));

    g_object_set_data (G_OBJECT (dlg), "sess", sess);
    g_signal_connect (dlg, "closed", G_CALLBACK (close_options_bookkeeping),
                      NULL);

    options_window = GTK_WIDGET (dlg);

    /* Content stack: one AdwPreferencesPage per settings_entry. */
    stack = gtk_stack_new ();
    gtk_widget_set_hexpand (stack, TRUE);
    gtk_widget_set_vexpand (stack, TRUE);

    /* Sidebar category list. */
    listbox = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (listbox),
                                     GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class (listbox, "navigation-sidebar");
    gtk_list_box_set_header_func (GTK_LIST_BOX (listbox),
                                  settings_sidebar_header, NULL, NULL);

    for (i = 0; i < sizeof (settings_entries) / sizeof (settings_entries[0]);
         i++) {
        const struct settings_entry *e = &settings_entries[i];
        AdwPreferencesPage *page
            = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
        GtkWidget *row, *rbox, *img, *lbl;
        const char *title = _ (e->title);

        if (e->section) {
            cur_section = _ (e->section);
        }

        adw_preferences_page_set_title (page, title);
        if (e->draw) {
            e->draw (page);
        }
        gtk_stack_add_named (GTK_STACK (stack), GTK_WIDGET (page), e->name);

        row = gtk_list_box_row_new ();
        rbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_start (rbox, 6);
        gtk_widget_set_margin_end (rbox, 6);
        gtk_widget_set_margin_top (rbox, 8);
        gtk_widget_set_margin_bottom (rbox, 8);
        img = gtk_image_new_from_icon_name (e->icon);
        lbl = gtk_label_new (title);
        gtk_label_set_xalign (GTK_LABEL (lbl), 0.0f);
        gtk_box_append (GTK_BOX (rbox), img);
        gtk_box_append (GTK_BOX (rbox), lbl);
        gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), rbox);

        /* qdata drives the header func + selection handler. name is a
         * static literal (no dup); section/title are gettext returns
         * (stable, but dup'd for lifetime safety across teardown). */
        g_object_set_data_full (G_OBJECT (row), "section",
                                g_strdup (cur_section), g_free);
        g_object_set_data (G_OBJECT (row), "page-name", (gpointer)e->name);
        g_object_set_data_full (G_OBJECT (row), "page-title", g_strdup (title),
                                g_free);
        gtk_list_box_append (GTK_LIST_BOX (listbox), row);
        if (!first_row) {
            first_row = GTK_LIST_BOX_ROW (row);
        }
    }

    sidebar_scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sidebar_scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sidebar_scroll),
                                   listbox);
    gtk_widget_set_vexpand (sidebar_scroll, TRUE);

    /* Sidebar pane: header bar + scrolled category list. */
    sidebar_tv = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
    adw_toolbar_view_add_top_bar (sidebar_tv, adw_header_bar_new ());
    adw_toolbar_view_set_content (sidebar_tv, sidebar_scroll);
    sidebar_page = ADW_NAVIGATION_PAGE (
        adw_navigation_page_new (GTK_WIDGET (sidebar_tv), _ ("Preferences")));

    /* Content pane: header bar (title tracks the selected page) + stack. */
    content_tv = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
    adw_toolbar_view_add_top_bar (content_tv, adw_header_bar_new ());
    adw_toolbar_view_set_content (content_tv, stack);
    content_page = ADW_NAVIGATION_PAGE (
        adw_navigation_page_new (GTK_WIDGET (content_tv), _ ("General")));

    split = ADW_NAVIGATION_SPLIT_VIEW (adw_navigation_split_view_new ());
    adw_navigation_split_view_set_sidebar (split, sidebar_page);
    adw_navigation_split_view_set_content (split, content_page);
    adw_navigation_split_view_set_max_sidebar_width (split, 240);

    g_object_set_data (G_OBJECT (listbox), "stack", stack);
    g_object_set_data (G_OBJECT (listbox), "split", split);
    g_object_set_data (G_OBJECT (listbox), "content-page", content_page);
    g_signal_connect (listbox, "row-selected",
                      G_CALLBACK (settings_row_selected), NULL);

    adw_dialog_set_child (dlg, GTK_WIDGET (split));

    /* Adaptive: collapse to a single navigable pane on narrow widths. */
    bp = adw_breakpoint_new (
        adw_breakpoint_condition_parse ("max-width: 500sp"));
    g_value_init (&collapsed, G_TYPE_BOOLEAN);
    g_value_set_boolean (&collapsed, TRUE);
    adw_breakpoint_add_setter (bp, G_OBJECT (split), "collapsed", &collapsed);
    g_value_unset (&collapsed);
    adw_dialog_add_breakpoint (dlg, bp);

    /* Select the first category so the content pane isn't blank. */
    if (first_row) {
        gtk_list_box_select_row (GTK_LIST_BOX (listbox), first_row);
    }

    adw_dialog_present (dlg, parent);
    /* The icon picker is no longer inline — it's populated on demand
     * when the Identity page's "Browse…" button opens the popup, so
     * there's no list_icons() call here. */
}

G_GNUC_END_IGNORE_DEPRECATIONS
/* end of file-level deprecation suppression — see top of file. */
