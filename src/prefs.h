/*
 * prefs.h — user preferences and persisted window geometry.
 *
 * Pure data — no GTK widget pointers, no protocol types. session.h pulls
 * this in for the prefs-bearing fields, but options.c can include just
 * this header when it's only touching the prefs struct.
 */

#ifndef GTKHX_PREFS_H
#define GTKHX_PREFS_H 1

#include "compat.h"

typedef struct {
    int xsize, ysize;
    int xpos, ypos;
    unsigned int open : 1;
    unsigned char init;
} Window_Geo;

struct gtkhx_prefs {
    int num_tracker;
    /* color-scheme preference, applied via AdwStyleManager.
     * Stored as one of "system" / "light" / "dark" (case-sensitive).
     * "system" follows the desktop-wide org.freedesktop.appearance
     * setting; the others force the corresponding scheme. */
    char *theme;
    char *font;
    char *download_path;
    /* strftime(3) format for the xtext per-line timestamp
     * column (Settings → Chat → Timestamp format). Default
     * '[%H:%M:%S] ' includes brackets + trailing space; user can
     * use any strftime spec ('%I:%M %p ', '%H:%M ', '<%H%M> ', ...). */
    char *stamp_format;
    /* chat-highlight word list — comma-separated string
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
    /* show a system-tray icon (StatusNotifierItem). When
     * on, the toolbar's close button hides all windows instead of
     * quitting; click on the tray icon toggles visibility; the
     * right-click menu offers Show/Hide, Connect, Disconnect, Quit.
     * Settings → General → "Show tray icon" drives this. Default
     * ON; the tray module silently no-ops if no SNI host is around. */
    unsigned char tray;
    unsigned char timestamp;
    /* Show the speaker's avatar / icon in the chat gutter. Only on the
     * first message of a run — see chat_view.h's grouping note. */
    unsigned char chat_avatars;
    /* Render markdown in incoming messages. Sending is unaffected —
     * markdown is transmitted literally, the wire format has no styling. */
    unsigned char markdown;
    unsigned char word_wrap;
    unsigned char track_case;
    unsigned char old_nickcompletion;
    unsigned char outrate_limit;
    unsigned char inrate_limit;
    unsigned char logging;

    /* Emoji shortcodes (phase E6), both default ON:
     *   emoji_shortcodes — convert emoji ↔ :shortcode: text. Drives the
     *     legacy-server send encode (gtkhx_text_for_wire) and the
     *     always-on receive decode (proto_helpers chat / PM builders),
     *     both via the gtkhx_text_emoji_shortcodes_enabled() toggle in
     *     text_util.c. Persisted as CFG_EMOJI_SHORTCODES.
     *   emoji_typeahead — show the inline `:prefix` suggestion popup as
     *     the user types (emoji.c). Read live. Persisted as
     *     CFG_EMOJI_TYPEAHEAD. */
    unsigned char emoji_shortcodes;
    unsigned char emoji_typeahead;

    /* HexChat-style xtext autocopy controls — driven by
     * Settings → Advanced → Auto Copy Behavior, persisted as the
     * AUTOCOPY_TEXT / AUTOCOPY_STAMP / AUTOCOPY_COLOR keys, applied
     * to the view via hx_chat_view_set_autocopy_*. See the comment on the
     * `prefs` struct in xtext.c for the per-field semantics. */
    unsigned char autocopy_text;
    unsigned char autocopy_stamp;
    unsigned char autocopy_color;

    /* per-event desktop notification toggles. See the
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

    /* fogWraith chat-history extension — number of
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

    /* Phase 8.E: voice capture + playback device preferences.
     * Stable gst::Device::name() strings. Empty/NULL means
     * "system default" — the runtime falls back to autoaudiosrc /
     * autoaudiosink. Persisted as CFG_VOICE_INPUT_DEVICE /
     * CFG_VOICE_OUTPUT_DEVICE, pushed to the Rust runtime via
     * gtkhx_voice_set_input_device / _set_output_device whenever
     * the user saves a different pick in Settings → Voice. */
    char *voice_input_device;
    char *voice_output_device;
    /* Phase 8.E follow-up: push-to-talk.
     *
     *   voice_ptt_enabled — switch row in Settings → Voice.
     *     When FALSE, the runtime hook is dormant and the toolbar
     *     mute button is the only mute driver. When TRUE, the
     *     window-scoped key controller fires UNMUTE on key-press
     *     and MUTE on key-release for the configured key.
     *
     *   voice_ptt_key — canonical name of the captured key,
     *     e.g. "Pause", "F8", "<Control>F12". Empty string (or
     *     NULL — pref load defaults to empty) means the user
     *     enabled PTT but hasn't picked a key yet; the hook
     *     stays inactive in that combo, and the Settings row
     *     shows a "Click to set" subtitle. */
    /* Plain unsigned char (not :1 bitfield) so cfgvars[] can take
     * its address. The pref read/write path stores BOOLEANs into
     * the byte verbatim — 0 or 1. */
    unsigned char voice_ptt_enabled;
    char *voice_ptt_key;

    /* Theming: active theme name. Theme files live at
     * $CONFIG/themes/<name>.ini (or a bundle dir
     * $CONFIG/themes/<name>/ with an icons/ subdir); the built-in
     * default ships as a
     * GResource and loads when no on-disk file by that name is
     * found. Empty / NULL falls back to "default". All scale,
     * palette, and icon state lives in the theme — scales and
     * colors in the .ini, icons as PNGs under a bundle dir's
     * icons/ subdir. This string is
     * the *only* theming knob in gtkhxrc. See gtkhx_theme.{c,h},
     * docs/theming.md, docs/theming-file-format.md. */
    char *theme_name;

    /* GIF-icons extension (Phase 10.D): animate GIF avatars in the
     * user list. Default ON. When OFF, avatars render as the still
     * first frame. Per-user pause (click an animated avatar, or the
     * right-click menu) is a separate, transient override. Persisted as
     * CFG_ANIMATE_AVATARS. Plain unsigned char so cfgvars[] can take
     * its address. */
    unsigned char animate_avatars;
};

extern struct gtkhx_prefs gtkhx_prefs;

struct hx_sounds {
    unsigned char on;
    unsigned char invite, chat, error, file, join, login, msg, news, part;
    unsigned char voice_join, voice_leave;
};

#endif /* ndef GTKHX_PREFS_H */
