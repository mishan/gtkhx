/*
 * prefs.h — user preferences and persisted window geometry.
 *
 * Pure data — no GTK widget pointers, no protocol types. session.h pulls
 * this in for the prefs-bearing fields, but options.c can include just
 * this header when it's only touching the prefs struct.
 */

#ifndef __gtkhx_PREFS_H
#define __gtkhx_PREFS_H 1

#include "compat.h"
#include "cfgkeys.h" /* UI_SCALE_PCT_MIN / MAX / DEFAULT */

typedef struct {
    int xsize, ysize;
    int xpos, ypos;
    unsigned int open : 1;
    unsigned char init;
} Window_Geo;

struct gtkhx_prefs {
    int num_tracker;
    /* Phase 5: color-scheme preference, applied via AdwStyleManager.
	 * Stored as one of "system" / "light" / "dark" (case-sensitive).
	 * "system" follows the desktop-wide org.freedesktop.appearance
	 * setting; the others force the corresponding scheme. */
    char *theme;
    char *auto_reply_msg;
    char *font;
    char *download_path;
    /* Phase 5: strftime(3) format for the xtext per-line timestamp
	 * column (Settings → Chat → Timestamp format). Default
	 * '[%H:%M:%S] ' includes brackets + trailing space; user can
	 * use any strftime spec ('%I:%M %p ', '%H:%M ', '<%H%M> ', ...). */
    char *stamp_format;
    /* Phase 5: chat-highlight word list — comma-separated string
	 * persisted as CFG_HIGHLIGHT_WORDS. Each entry matches case-
	 * insensitive at word boundaries against incoming chat bodies;
	 * when any matches (or the body mentions our own nick), the
	 * chat line renders in bold red via inline ATTR_BOLD+ATTR_COLOR
	 * codes. Empty string disables additional words — own-nick
	 * detection still happens. */
    char *highlight_words;
    char **tracker;
    char *tracker_str;
    int xbuf_max;

    struct {
        Window_Geo chat;
        Window_Geo news;
        Window_Geo tool;
        Window_Geo tasks;
        Window_Geo users;
    } geo;

    unsigned char queuedl;
    unsigned char showjoin;
    unsigned char showback;
    /* Phase 5+: show a system-tray icon (StatusNotifierItem). When
	 * on, the toolbar's close button hides all windows instead of
	 * quitting; click on the tray icon toggles visibility; the
	 * right-click menu offers Show/Hide, Connect, Disconnect, Quit.
	 * Settings → General → "Show tray icon" drives this. Default
	 * ON; the tray module silently no-ops if no SNI host is around. */
    unsigned char tray;
    unsigned char auto_reply;
    unsigned char timestamp;
    unsigned char word_wrap;
    unsigned char track_case;
    unsigned char old_nickcompletion;
    unsigned char outrate_limit;
    unsigned char inrate_limit;
    unsigned char logging;

    /* Phase 5: HexChat-style xtext autocopy controls — driven by
	 * Settings → Advanced → Auto Copy Behavior, persisted as the
	 * AUTOCOPY_TEXT / AUTOCOPY_STAMP / AUTOCOPY_COLOR keys, applied
	 * to xtext via gtk_xtext_set_autocopy_*. See the comment on the
	 * `prefs` struct in xtext.c for the per-field semantics. */
    unsigned char autocopy_text;
    unsigned char autocopy_stamp;
    unsigned char autocopy_color;

    /* Phase 5+: per-event desktop notification toggles. See the
	 * CFG_NOTIFY_* keys in cfgkeys.h for the user-facing surface
	 * and src/notify.c for the dispatch logic. */
    unsigned char notify_chat;
    unsigned char notify_chat_highlight;
    unsigned char notify_msg;
    unsigned char notify_pchat;
    unsigned char notify_pchat_highlight;
    unsigned char notify_pchat_invite;
    unsigned char notify_news;
    unsigned char notify_xfer;
    unsigned char notify_broadcast;
    unsigned char notify_omit_focused;

    int out_bps;
    int in_bps;

    /* Phase 5+: fogWraith chat-history extension — number of
	 * entries to pull on the initial post-login fetch AND on each
	 * subsequent "Load older messages" click. 0 disables the
	 * initial pull entirely (the click handler still uses 50 as
	 * a floor when 0 is set, so the affordance still works once
	 * the user manually engages it). Persisted as
	 * CFG_CHAT_HISTORY_INITIAL. */
    int chat_history_initial;

    /* Colored-Nicknames extension — our own 32-bit
	 * 0x00RRGGBB nickname color. -1 (== HX_NICK_COLOR_NONE cast to
	 * signed int) is the "no color set" sentinel and means
	 * hx_change_name_icon omits the HTLC_DATA_COLOR chunk entirely
	 * (the spec's auto-opt-in then doesn't fire). Stored as `int`
	 * to fit the cfgvars INT slot; reinterpreted as guint32 when
	 * stamped onto htlc->nick_color. Persisted as CFG_NICK_COLOR. */
    int nick_color;

    /* Phase 5+: global UI scale as an integer percentage. Drives
	 * icon dimensions (users window 1.25x, files/news rows 1.5x,
	 * toolbar button upscale, settings icon picker grid) and font
	 * sizes for the global gtkhx-text / gtkhx-userlist CSS providers
	 * and the xtext chat output. Default 100 = unchanged behaviour.
	 * Discrete preset values land via Settings (85/100/115/125/150)
	 * but any integer >= 50 and <= 200 is accepted on read so users
	 * can hand-edit gtkhxrc. Persisted as CFG_UI_SCALE. */
    int ui_scale_pct;
};

extern struct gtkhx_prefs gtkhx_prefs;

/*
 * Phase 5+: read gtkhx_prefs.ui_scale_pct as a double scale factor.
 * 100 → 1.0, 125 → 1.25, etc. Values outside the validated range are
 * clamped to the default so a corrupt or hand-edited gtkhxrc can't
 * shrink icons to 0 px or balloon the toolbar past compositor limits.
 * Defined inline so consumers (users_view.c, files_panel.c, gtkhx.c,
 * toolbar.c, ...) don't need to add an #include for one accessor.
 * Lives in prefs.h (rather than gtkutil.h) so unit tests can call it
 * without dragging GTK into the test binary.
 */
static inline double
gtkhx_ui_scale (void)
{
    int pct = gtkhx_prefs.ui_scale_pct;
    if (pct < UI_SCALE_PCT_MIN || pct > UI_SCALE_PCT_MAX) {
        pct = UI_SCALE_PCT_DEFAULT;
    }
    return pct / 100.0;
}

struct hx_sounds {
    unsigned char on;
    unsigned char invite, chat, error, file, join, login, msg, news, part;
};

#endif /* ndef __gtkhx_PREFS_H */
