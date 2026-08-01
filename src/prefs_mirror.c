/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * prefs_mirror.c — the storage behind prefs_mirror.h. See that header for
 * why the values live here and are written from exactly one place.
 *
 * This TU deliberately pulls in nothing but glib and the hxconfig ABI, so a
 * test binary that needs a preference to read can link it without dragging in
 * the Settings dialog, GTK, or the session.
 */

#include "config.h"

#include "prefs_mirror.h"

#include "cfgkeys.h"
#include "prefs.h"
#include "sound.h"

#include <glib.h>

/* The mirror itself. Defined here rather than in options.c so the four test
 * binaries that want a preference without the Settings machinery can link one
 * small object instead of declaring private copies — which is what they used
 * to do, and which would now be a duplicate definition. */
struct gtkhx_prefs gtkhx_prefs;

/* hxconfig's C ABI. Hand-declared, like every other Rust seam in the tree —
 * a signature that drifts shows up as an undefined symbol at link time. */
extern int hxconfig_get_bool (const char *name);
extern int hxconfig_get_int (const char *name);
extern char *hxconfig_get_string (const char *name);
extern void hxconfig_free_string (char *s);
extern int hxconfig_tracker_count (void);
extern char *hxconfig_tracker_at (int i);

/* Replace a mirrored string with the current value.
 *
 * The getter's result is Rust-allocated, so it is copied and handed straight
 * back rather than stored — mixing allocators is how the old `allocated`
 * ownership bit turned into two leaks and a double-free hazard. */
static void
mirror_string (char **slot, const char *name)
{
    char *value = hxconfig_get_string (name);

    g_free (*slot);
    *slot = g_strdup (value != NULL ? value : "");
    hxconfig_free_string (value);
}

static void
mirror_trackers (void)
{
    int n = hxconfig_tracker_count ();
    int i;

    if (gtkhx_prefs.tracker != NULL) {
        for (i = 0; i < gtkhx_prefs.num_tracker; i++) {
            g_free (gtkhx_prefs.tracker[i]);
        }
        g_free (gtkhx_prefs.tracker);
        gtkhx_prefs.tracker = NULL;
    }
    gtkhx_prefs.num_tracker = 0;

    if (n <= 0) {
        return;
    }

    gtkhx_prefs.tracker = g_new0 (char *, (gsize)n);
    for (i = 0; i < n; i++) {
        char *addr = hxconfig_tracker_at (i);
        gtkhx_prefs.tracker[i] = g_strdup (addr != NULL ? addr : "");
        hxconfig_free_string (addr);
    }
    gtkhx_prefs.num_tracker = n;
}

void
hx_prefs_mirror_refresh (void)
{
    /* Identity lives on the connection, not here — see options.c. */

    mirror_string (&gtkhx_prefs.theme, CFG_THEME);
    mirror_string (&gtkhx_prefs.theme_name, CFG_THEME_NAME);
    gtkhx_prefs.tray = hxconfig_get_bool (CFG_TRAY) ? 1 : 0;

    mirror_string (&gtkhx_prefs.font, CFG_FONT);
    mirror_string (&gtkhx_prefs.stamp_format, CFG_STAMP_FORMAT);
    mirror_string (&gtkhx_prefs.highlight_words, CFG_HIGHLIGHT_WORDS);
    gtkhx_prefs.word_wrap = hxconfig_get_bool (CFG_WORDWRAP) ? 1 : 0;
    gtkhx_prefs.xbuf_max = hxconfig_get_int (CFG_XBUF_MAX);
    gtkhx_prefs.timestamp = hxconfig_get_bool (CFG_TIMESTAMP) ? 1 : 0;
    gtkhx_prefs.chat_avatars = hxconfig_get_bool (CFG_CHAT_AVATARS) ? 1 : 0;
    gtkhx_prefs.markdown = hxconfig_get_bool (CFG_MARKDOWN) ? 1 : 0;
    gtkhx_prefs.showjoin = hxconfig_get_bool (CFG_SHOWJOIN) ? 1 : 0;
    gtkhx_prefs.chat_history_initial
        = hxconfig_get_int (CFG_CHAT_HISTORY_INITIAL);
    gtkhx_prefs.old_nickcompletion
        = hxconfig_get_bool (CFG_OLD_NICKCOMP) ? 1 : 0;
    gtkhx_prefs.nick_color = hxconfig_get_int (CFG_NICK_COLOR);

    gtkhx_prefs.autocopy_text = hxconfig_get_bool (CFG_AUTOCOPY_TEXT) ? 1 : 0;
    gtkhx_prefs.autocopy_stamp = hxconfig_get_bool (CFG_AUTOCOPY_STAMP) ? 1 : 0;
    gtkhx_prefs.autocopy_color = hxconfig_get_bool (CFG_AUTOCOPY_COLOR) ? 1 : 0;

    gtkhx_prefs.emoji_shortcodes
        = hxconfig_get_bool (CFG_EMOJI_SHORTCODES) ? 1 : 0;
    gtkhx_prefs.emoji_typeahead
        = hxconfig_get_bool (CFG_EMOJI_TYPEAHEAD) ? 1 : 0;

    gtkhx_prefs.animate_avatars
        = hxconfig_get_bool (CFG_ANIMATE_AVATARS) ? 1 : 0;

    gtkhx_prefs.notify_chat = hxconfig_get_bool (CFG_NOTIFY_CHAT) ? 1 : 0;
    gtkhx_prefs.notify_chat_highlight
        = hxconfig_get_bool (CFG_NOTIFY_CHAT_HIGHLIGHT) ? 1 : 0;
    gtkhx_prefs.notify_msg = hxconfig_get_bool (CFG_NOTIFY_MSG) ? 1 : 0;
    gtkhx_prefs.notify_pchat = hxconfig_get_bool (CFG_NOTIFY_PCHAT) ? 1 : 0;
    gtkhx_prefs.notify_pchat_highlight
        = hxconfig_get_bool (CFG_NOTIFY_PCHAT_HIGHLIGHT) ? 1 : 0;
    gtkhx_prefs.notify_pchat_invite
        = hxconfig_get_bool (CFG_NOTIFY_PCHAT_INVITE) ? 1 : 0;
    gtkhx_prefs.notify_news = hxconfig_get_bool (CFG_NOTIFY_NEWS) ? 1 : 0;
    gtkhx_prefs.notify_xfer = hxconfig_get_bool (CFG_NOTIFY_XFER) ? 1 : 0;
    gtkhx_prefs.notify_broadcast
        = hxconfig_get_bool (CFG_NOTIFY_BROADCAST) ? 1 : 0;
    gtkhx_prefs.notify_omit_focused
        = hxconfig_get_bool (CFG_NOTIFY_OMIT_FOCUSED) ? 1 : 0;

    /* The sound toggles are a separate global rather than part of the prefs
     * struct, and stay one: sound.c reads them twelve times in a single
     * function and nothing else touches them. Mirroring costs three lines
     * more than converting that function would, and leaves it untouched. */
    hxsnd.on = hxconfig_get_bool (CFG_SOUNDS_ON) ? 1 : 0;
    hxsnd.chat = hxconfig_get_bool (CFG_SND_CHAT) ? 1 : 0;
    hxsnd.error = hxconfig_get_bool (CFG_SND_ERROR) ? 1 : 0;
    hxsnd.file = hxconfig_get_bool (CFG_SND_FILE) ? 1 : 0;
    hxsnd.invite = hxconfig_get_bool (CFG_SND_INVITE) ? 1 : 0;
    hxsnd.join = hxconfig_get_bool (CFG_SND_JOIN) ? 1 : 0;
    hxsnd.part = hxconfig_get_bool (CFG_SND_PART) ? 1 : 0;
    hxsnd.login = hxconfig_get_bool (CFG_SND_LOGIN) ? 1 : 0;
    hxsnd.msg = hxconfig_get_bool (CFG_SND_MSG) ? 1 : 0;
    hxsnd.news = hxconfig_get_bool (CFG_SND_NEWS) ? 1 : 0;
    hxsnd.voice_join = hxconfig_get_bool (CFG_SND_VOICE_JOIN) ? 1 : 0;
    hxsnd.voice_leave = hxconfig_get_bool (CFG_SND_VOICE_LEAVE) ? 1 : 0;

    mirror_string (&gtkhx_prefs.download_path, CFG_DOWNLOAD);
    gtkhx_prefs.queuedl = hxconfig_get_bool (CFG_QUEUEDL) ? 1 : 0;

    /* The comma-separated string and the derived array beside it were kept in
     * step by a change hook. The schema stores a real array, so the hook is
     * gone and both shapes are projections of it — the string only survives
     * because the Settings page's entry row still edits one. */
    mirror_string (&gtkhx_prefs.tracker_str, CFG_TRACKER);
    mirror_trackers ();
    gtkhx_prefs.track_case = hxconfig_get_bool (CFG_TRACKER_CASE) ? 1 : 0;

    mirror_string (&gtkhx_prefs.voice_input_device, CFG_VOICE_INPUT_DEVICE);
    mirror_string (&gtkhx_prefs.voice_output_device, CFG_VOICE_OUTPUT_DEVICE);
    gtkhx_prefs.voice_ptt_enabled
        = hxconfig_get_bool (CFG_VOICE_PTT_ENABLED) ? 1 : 0;
    mirror_string (&gtkhx_prefs.voice_ptt_key, CFG_VOICE_PTT_KEY);

    gtkhx_prefs.geo.tool.xsize = hxconfig_get_int (CFG_TOOL_XSIZE);
    gtkhx_prefs.geo.tool.ysize = hxconfig_get_int (CFG_TOOL_YSIZE);
}
