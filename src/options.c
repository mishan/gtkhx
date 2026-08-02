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
/* Still needed by the two device change hooks, which push the value into the
 * Rust runtime. The page that edits them is Rust now; the hooks are not. */
#include "voice_runtime.h"
#endif

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

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
changed_markdown (void)
{
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

/* The identity a connection should present, resolved and copied.
 *
 * The nickname and icon preferences used to *be* the connection's wire name
 * buffer and icon field — a startup binder patched their two table slots with
 * raw interior pointers, so a write to either went straight onto the wire
 * struct. They are ordinary settings with their own storage now, and this is
 * the one-way copy that replaces the aliasing.
 *
 * Resolution is `override ?? global ?? startup`, three deep because each level
 * answers a different question:
 *
 *   override  what this connection is configured to show, if anything
 *   global    what everything unspecialised shows
 *   startup   what a profile that has never set a nickname shows: $USER,
 *             stamped onto the connection before the settings file is read.
 *             Kept here so a reconnect can restore it — otherwise a /nick
 *             would survive one, which is exactly what this is meant to undo.
 *
 * Copied, never aliased: `/nick` changes the running connection and nothing
 * else, and the next connect puts the configured identity back "as if the
 * command had never been typed". */
static char *pending_nick;
static int pending_icon = -1;
static char *startup_nick;

void
hx_identity_set_startup_default (const char *nick)
{
    g_free (startup_nick);
    startup_nick = g_strdup (nick != NULL ? nick : "");
}

void
hx_identity_set_pending_override (const char *nick, int icon)
{
    g_free (pending_nick);
    pending_nick = (nick != NULL && *nick) ? g_strdup (nick) : NULL;
    /* Negative means "no override". Zero cannot: it is a real, blank icon
     * that someone can legitimately choose. */
    pending_icon = icon;
}

void
hx_identity_apply (struct htlc_conn *htlc)
{
    char *global_nick = gtkhx_prefs_get_string (CFG_NICK);
    const char *nick = pending_nick;

    if (nick == NULL) {
        nick = *global_nick ? global_nick : startup_nick;
    }
    if (nick != NULL && *nick) {
        hx_conn_set_name (htlc, nick);
    }
    g_free (global_nick);

    /* Set unconditionally. This used to skip a zero, treating it as "nothing
     * stored" — but zero is a real (blank) icon and the Settings spin row
     * offers it, so choosing it silently did nothing. */
    hx_conn_set_icon (htlc, (guint16)(pending_icon >= 0
                                          ? pending_icon
                                          : gtkhx_prefs_get_int (CFG_ICON)));

    /* One connect, one override. Anything that wants it again says so again,
     * so a bookmark's nickname can't leak onto a later /server or a tracker
     * double-click. */
    g_clear_pointer (&pending_nick, g_free);
    pending_icon = -1;
}

/* The load-time and Settings-edit path, which has no override in play. */
static void
identity_copy_to_conn (struct htlc_conn *htlc)
{
    hx_identity_apply (htlc);
}

/* The Settings nick/icon controls used to be inert — changing them updated the
 * session's htlc but never told the server, so the user list still showed your
 * old nick/icon until reconnect. The send is a no-op while the connection has
 * no socket, so this is safe to call before connect too. */
static void
changed_nickoricon (struct htlc_conn *htlc)
{
    identity_copy_to_conn (htlc);
    hx_change_name_icon (htlc);
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
changed_nick_color (struct htlc_conn *htlc)
{
    guint32 nc = (guint32)gtkhx_prefs.nick_color;
    hx_conn_set_nick_color (htlc, nc);
    hx_change_name_icon (htlc);

    /* Locally re-render our own row in the public chat user list.
     * Pre-login (no uid yet, or no chat container yet) just no-ops —
     * apply_loaded_xtext_prefs stamps the loaded pref onto htlc, and
     * the SELFINFO-driven membership add for self picks it up the same
     * way it picks up the loaded nick. */
    struct chat *pub = chat_with_cid (sess_from_htlc (htlc), 0);
    if (pub && hx_conn_uid (htlc)) {
        struct hx_member_info mi;
        if (hx_member_model_get_info (hx_chat_member_model (pub),
                                      hx_conn_uid (htlc), &mi)) {
            user_change (htlc, pub, mi.uid, nc, mi.name, mi.icon, mi.status);
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
changed_emoji_shortcodes (void)
{
    gtkhx_text_set_emoji_shortcodes_enabled (gtkhx_prefs.emoji_shortcodes);
}

/* changefunc for CFG_ANIMATE_AVATARS (Phase 10.D). Push the toggle into
 * gif_avatar.c, which starts/stops its frame timer and repaints avatars
 * as either animated or a still first frame. */
static void
changed_animate_avatars (void)
{
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
changed_autocopy_text (void)
{
    hx_chat_view_set_autocopy_text (gtkhx_prefs.autocopy_text);
}
static void
changed_autocopy_stamp (void)
{
    hx_chat_view_set_autocopy_stamp (gtkhx_prefs.autocopy_stamp);
}
static void
changed_autocopy_color (void)
{
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
changed_downloadpath (void)
{
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
changed_case (void)
{
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
changed_theme (void)
{
    AdwStyleManager *sm = adw_style_manager_get_default ();
    const char *theme
        = gtkhx_prefs.theme ? gtkhx_prefs.theme : CFG_THEME_SYSTEM;
    AdwColorScheme scheme;

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
changed_tray (void)
{
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
changed_voice_input_device (void)
{
    gtkhx_voice_set_input_device (gtkhx_prefs.voice_input_device);
}

/* Settings → Voice → "Output device" combobox. Same shape as
 * changed_voice_input_device but for the receive (autoaudiosink)
 * side. */
static void
changed_voice_output_device (void)
{
    gtkhx_voice_set_output_device (gtkhx_prefs.voice_output_device);
}
#endif /* HAVE_VOICE */

/* changefunc for the active-theme name. A change here (manually
 * editing gtkhxrc, or — in a future theme-editor phase — Settings
 * picking a different theme) reloads the theme file and emits the
 * theme "changed" signal so every subscribed button / user-list view
 * / chat xtext rescales and repaints. */
static void
changed_theme_name (void)
{
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
/* Three flavours, because a hook that takes a `session *` and never looks at
 * it is telling the reader something false. Every hook used to take one; only
 * five actually walk a session's live views, thirteen ignored the argument,
 * and two reached past it for whichever session happened to be focused. That
 * last group is the interesting one — see PREF_HOOK_CONN.
 *
 * PREF_HOOK_VIEW    applies to every live view in a session.
 * PREF_HOOK_GLOBAL  a process-wide side effect; nothing session-shaped.
 * PREF_HOOK_CONN    pushes the value onto a connection, which means onto the
 *                   wire. Takes the connection explicitly so the "which one?"
 *                   question is asked once, at the dispatch site, instead of
 *                   eight times inside the bodies. Today the answer is always
 *                   the focused connection and there is only one; when there
 *                   is more than one, this is the seam that has to change, and
 *                   the type makes it findable rather than a grep for
 *                   hx_active_session.
 */
enum pref_hook_kind {
    PREF_HOOK_VIEW,
    PREF_HOOK_GLOBAL,
    PREF_HOOK_CONN,
};

struct pref_hook {
    const char *name;
    enum pref_hook_kind kind;
    union {
        void (*view) (session *);
        void (*global) (void);
        void (*conn) (struct htlc_conn *);
    } fn;
};

#define PREF_VIEW(key, func)                                                   \
    {                                                                          \
        (key), PREF_HOOK_VIEW,                                                 \
        {                                                                      \
            .view = (func)                                                     \
        }                                                                      \
    }
#define PREF_GLOBAL(key, func)                                                 \
    {                                                                          \
        (key), PREF_HOOK_GLOBAL,                                               \
        {                                                                      \
            .global = (func)                                                   \
        }                                                                      \
    }
#define PREF_CONN(key, func)                                                   \
    {                                                                          \
        (key), PREF_HOOK_CONN,                                                 \
        {                                                                      \
            .conn = (func)                                                     \
        }                                                                      \
    }

static const struct pref_hook pref_hooks[] = {
    PREF_GLOBAL (CFG_ANIMATE_AVATARS, changed_animate_avatars),
    PREF_GLOBAL (CFG_AUTOCOPY_COLOR, changed_autocopy_color),
    PREF_GLOBAL (CFG_AUTOCOPY_STAMP, changed_autocopy_stamp),
    PREF_GLOBAL (CFG_AUTOCOPY_TEXT, changed_autocopy_text),
    PREF_VIEW (CFG_CHAT_AVATARS, changed_chat_avatars),
    PREF_GLOBAL (CFG_DOWNLOAD, changed_downloadpath),
    PREF_GLOBAL (CFG_EMOJI_SHORTCODES, changed_emoji_shortcodes),
    PREF_VIEW (CFG_FONT, changed_font),
    PREF_CONN (CFG_ICON, changed_nickoricon),
    PREF_GLOBAL (CFG_MARKDOWN, changed_markdown),
    PREF_CONN (CFG_NICK, changed_nickoricon),
    PREF_CONN (CFG_NICK_COLOR, changed_nick_color),
    PREF_VIEW (CFG_STAMP_FORMAT, changed_stampformat),
    PREF_GLOBAL (CFG_THEME, changed_theme),
    PREF_GLOBAL (CFG_THEME_NAME, changed_theme_name),
    PREF_VIEW (CFG_TIMESTAMP, changed_timestamp),
    PREF_GLOBAL (CFG_TRACKER_CASE, changed_case),
    PREF_GLOBAL (CFG_TRAY, changed_tray),
#ifdef HAVE_VOICE
    /* The device preferences persist whether or not voice is compiled in, so
     * a build without it doesn't discard a user's saved picks; only the hooks
     * that push the value into the Rust runtime are conditional. */
    PREF_GLOBAL (CFG_VOICE_INPUT_DEVICE, changed_voice_input_device),
    PREF_GLOBAL (CFG_VOICE_OUTPUT_DEVICE, changed_voice_output_device),
#endif
    PREF_VIEW (CFG_WORDWRAP, changed_xtext),
    PREF_VIEW (CFG_XBUF_MAX, changed_xtext),
};

static const struct pref_hook *
pref_hook_for (const char *name)
{
    size_t i;

    for (i = 0; i < G_N_ELEMENTS (pref_hooks); i++) {
        if (strcmp (pref_hooks[i].name, name) == 0) {
            return &pref_hooks[i];
        }
    }
    return NULL;
}

/* Run one hook against the session the user is looking at.
 *
 * The only place that decides which session and which connection a preference
 * change applies to. At one connection `hx_active_session` and "the session
 * that owns this connection" coincide; the distinction is in session.h and
 * matters to multi-connection, which will have to give this function a
 * connection rather than let it pick. */
static void
pref_hook_run (const struct pref_hook *hook)
{
    session *sess = hx_active_session ();

    switch (hook->kind) {
    case PREF_HOOK_VIEW:
        hook->fn.view (sess);
        break;
    case PREF_HOOK_GLOBAL:
        hook->fn.global ();
        break;
    case PREF_HOOK_CONN:
        hook->fn.conn (sess->htlc);
        break;
    }
}

/* Everything a successful write has to do after the value has landed in Rust:
 * bring the C mirror back into step, run the key's apply hook, and arm the
 * save timer. */
static void
pref_apply (const char *name)
{
    const struct pref_hook *hook = pref_hook_for (name);

    hx_prefs_mirror_refresh ();
    if (hook) {
        pref_hook_run (hook);
    }
    hx_prefs_save_soon ();
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
 * case-sensitive toggle).
 *
 * This used to route through the live AdwSwitchRow when Settings happened to
 * be showing one for this key, so the visible toggle stayed in lockstep. That
 * stopped working when the Tracker page moved to Rust — its rows are built by
 * the Rust helper, which never registered them — so the lookup has been
 * missing and falling through to the direct write ever since, silently. With
 * the last C page ported there are no registered rows at all, so the registry
 * and the lookup are gone rather than kept as something that cannot fire.
 *
 * If the lockstep behaviour is wanted back, it belongs on the Rust side where
 * the rows now live. */
void
gtkhx_prefs_set_bool (const char *name, int value)
{
    if (hxconfig_set_bool (name, value ? 1 : 0)) {
        pref_apply (name);
    }
}

/* ------------------------------------------------------------------- *
 * The Settings window
 *
 * Every page is Rust now; what remains here is the shell — the window, the
 * sidebar, and the table mapping a page to its builder. The AdwPreferencesRow
 * helpers that used to head this section went with the last page that used
 * them (they live in `rust/crates/gtkhx-ui/src/options.rs`), and the table's
 * `.draw` pointers are the last thing keeping this a C-side framework.
 *
 * No Cancel button — the dialog is live-apply. Closing it is the equivalent of
 * "OK", and each change is saved shortly after it happens, so a crash
 * mid-Settings doesn't lose the last toggle. */

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
        pref_hook_run (&pref_hooks[i]);
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

    /* Nothing else to unwind. Every page is Rust now, and each owns whatever
     * state it has; it all drops with the dialog's widget tree. This used to
     * flush the entry-row debounce timers and tear down the live-row registry,
     * both of which went with the last C page. */
}

/* No Interface page anymore — the new files browser is always a single
 * window. The retired FILE_SAMEWINDOW / NEWS_SAMEWINDOW keys are named in
 * hxconfig's migration map only so an old profile carrying them doesn't trip
 * the unknown-key diagnostic. */

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
extern void gtkhx_options_rs_page_identity (AdwPreferencesPage *);
#ifdef HAVE_VOICE
extern void gtkhx_options_rs_page_voice (AdwPreferencesPage *);
#endif

static const struct settings_entry settings_entries[] = {
    { N_ ("General"), "general", N_ ("General"), "preferences-system-symbolic",
      gtkhx_options_rs_page_general },
    { NULL, "identity", N_ ("Identity"), "user-info-symbolic",
      gtkhx_options_rs_page_identity },
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
      gtkhx_options_rs_page_voice },
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
}

G_GNUC_END_IGNORE_DEPRECATIONS
/* end of file-level deprecation suppression — see top of file. */
