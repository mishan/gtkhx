/*
 * cfgkeys.h — config-key string constants.
 *
 * The cfgvars[] table in options.c is keyed by short upper-case strings
 * ("FONT", "ICON", "THEME", ...). The same strings appear at the call
 * sites where the dialog rows are built (pref_*_row("FONT", ...)) and
 * where code reaches into the table by name (cfgvar_for_name("FONT")).
 * Repeating the literal at every site is a typo magnet and makes
 * grep-based refactors brittle: misspelling "FONT" as "FNT" compiles
 * cleanly, runs, and silently drops the pref.
 *
 * Centralize the keys here as #defines so:
 *   1. The compiler still folds them into a single .rodata copy.
 *   2. A typo at a call site is now a "use of undeclared identifier"
 *      compile error instead of a runtime drop.
 *   3. The full set of config keys is discoverable in one place,
 *      grouped by what they configure.
 *
 * Anything that touches a cfgvar by string name should use these
 * constants rather than hard-coding the literal. The cfgvars[] table
 * uses them as initializer values, so its alphabetic-sort invariant is
 * preserved (the literals collapse to the same bytes either way).
 *
 * Adding a new pref: define the key here, add the cfgvars[] entry, and
 * the cfgvars_assert_sorted() check at startup will catch a misordered
 * insert before bsearch does anything subtle.
 */

#ifndef GTKHX_CFGKEYS_H
#define GTKHX_CFGKEYS_H 1

/* Identity */
#define CFG_NICK "NICK"
#define CFG_ICON "ICON"
/* Colored-Nicknames extension — 32-bit 0x00RRGGBB stored
 * as int (-1 = HX_NICK_COLOR_NONE = "no color, use theme default").
 * Persisted as a plain decimal integer in gtkhxrc. */
#define CFG_NICK_COLOR "NICKCOLOR"

/* Behavior toggles */
#define CFG_QUEUEDL "QUEUEDL"
#define CFG_SHOWJOIN "SHOWJOIN"
#define CFG_TRACKER_CASE "TRACKER_CASE"
#define CFG_OLD_NICKCOMP "OLD_NICKCOMPLETION"
#define CFG_TRAY "TRAY"

/* Chat output */
#define CFG_TIMESTAMP "TIMESTAMP"
#define CFG_CHAT_AVATARS "CHATAVATARS"
#define CFG_MARKDOWN "MARKDOWN"
#define CFG_WORDWRAP "WORDWRAP"
#define CFG_XBUF_MAX "XBUF_MAX"
#define CFG_FONT "FONT"

/* fogWraith chat-history extension — number of entries to
 * pull on the initial post-login fetch. 0 disables the initial pull
 * (server still advertises CAP_CHAT_HISTORY; user has to click Load
 * older to see anything). Subsequent Load-older clicks each request
 * the same count. Default 50 — matches the recommended default in
 * the spec, and is what Phase 1/2/3 shipped with the value
 * hard-coded. */
#define CFG_CHAT_HISTORY_INITIAL "CHATHISTORYINITIAL"

/* Emoji shortcodes (phase E6). EMOJISHORTCODES gates the emoji ↔
 * :shortcode: conversion (legacy-server send encode + always-on receive
 * decode); EMOJITYPEAHEAD gates the inline `:prefix` suggestion popup.
 * Both BOOLEAN, both default ON. */
#define CFG_EMOJI_SHORTCODES "EMOJISHORTCODES"
#define CFG_EMOJI_TYPEAHEAD "EMOJITYPEAHEAD"

/* GIF-icons extension (Phase 10.D): animate GIF avatars in the user
 * list. BOOLEAN, default ON. */
#define CFG_ANIMATE_AVATARS "ANIMATEAVATARS"

/* Same-window navigation for the files browser. The matching
 * NEWS_SAMEWINDOW key was retired in Phase 6 when the legacy
 * two-window news UI was removed; an old gtkhxrc that still has
 * the key just gets ignored. Same applies to FILE_SAMEWINDOW,
 * retired in Phase 5 with the legacy single-pane files browser. */

/* Desktop notifications (Phase 5+). One BOOLEAN per event class so
 * users can toggle each independently. NOTIFY_OMIT_FOCUSED gates
 * the suppress-when-relevant-window-has-focus behaviour. The
 * "mention" classes (CHAT_HIGHLIGHT / PCHAT_HIGHLIGHT) match
 * against the same word list as the chat highlight colouring
 * (own nick + CFG_HIGHLIGHT_WORDS, comma-separated). */
#define CFG_NOTIFY_CHAT "NOTIFYCHAT"
#define CFG_NOTIFY_CHAT_HIGHLIGHT "NOTIFYCHATHIGHLIGHT"
#define CFG_NOTIFY_MSG "NOTIFYMSG"
#define CFG_NOTIFY_PCHAT "NOTIFYPCHAT"
#define CFG_NOTIFY_PCHAT_HIGHLIGHT "NOTIFYPCHATHIGHLIGHT"
#define CFG_NOTIFY_PCHAT_INVITE "NOTIFYPCHATINVITE"
#define CFG_NOTIFY_NEWS "NOTIFYNEWS"
#define CFG_NOTIFY_XFER "NOTIFYXFER"
#define CFG_NOTIFY_BROADCAST "NOTIFYBROADCAST"
#define CFG_NOTIFY_OMIT_FOCUSED "NOTIFYOMITFOCUSED"

/* Sounds */
#define CFG_SOUNDS_ON "SOUNDSON"
#define CFG_SND_INVITE "SOUNDINVITE"
#define CFG_SND_CHAT "SOUNDCHAT"
#define CFG_SND_ERROR "SOUNDERROR"
#define CFG_SND_FILE "SOUNDFILE"
#define CFG_SND_JOIN "SOUNDJOIN"
#define CFG_SND_LOGIN "SOUNDLOGIN"
#define CFG_SND_MSG "SOUNDMSG"
#define CFG_SND_NEWS "SOUNDNEWS"
#define CFG_SND_PART "SOUNDPART"
#define CFG_SND_VOICE_JOIN "SOUNDVOICEJOIN"
#define CFG_SND_VOICE_LEAVE "SOUNDVOICELEAVE"

/* Paths */
#define CFG_DOWNLOAD "DOWNLOAD"

/* Tracker list (single comma-separated string) */
#define CFG_TRACKER "TRACKER"

/* Appearance */
#define CFG_THEME "THEME"

/* THEME string values (case-sensitive — must match changed_theme()). */
#define CFG_THEME_SYSTEM "system"
#define CFG_THEME_LIGHT "light"
#define CFG_THEME_DARK "dark"

/* xtext autocopy controls — Settings → Advanced → Auto Copy
 * Behavior. Three independent toggles for the drag-end clipboard
 * behaviour. See chat_view.h:hx_chat_view_set_autocopy_* for the per-field
 * semantics. */
#define CFG_AUTOCOPY_TEXT "AUTOCOPYTEXT"
#define CFG_AUTOCOPY_STAMP "AUTOCOPYSTAMP"
#define CFG_AUTOCOPY_COLOR "AUTOCOPYCOLOR"

/* timestamp format. strftime(3) format string used by xtext
 * for the per-line stamp column and the autocopy_stamp clipboard
 * prefix. Default '[%H:%M:%S] ' matches HexChat's default and the
 * bracketed form users are used to from GtkHx pre-Phase-5. */
#define CFG_STAMP_FORMAT "TIMESTAMPFORMAT"

/* chat-highlight word list (Settings → Chat → Highlight).
 * Comma-separated; each word matches case-insensitively at word
 * boundaries against incoming chat bodies. Our own nick is always
 * implicitly matched so this list is purely additional. */
#define CFG_HIGHLIGHT_WORDS "HIGHLIGHTWORDS"

/* Window geometry — four windows × (w, h). Names match the historic
 * gtkhxrc keys.
 *
 * The matching OPENCHAT / OPENNEWS / OPENTASKS / OPENUSERS keys are
 * gone. They looked like user intent and were not: each defaulted to 1,
 * each panel set its own the first time it was constructed, and nothing
 * ever cleared one, so after first run they were 1 for everybody. No
 * setting exposed them either. The runtime "has this panel been built"
 * flag they shadowed now lives with the panels, in panel_registry.h. */
#define CFG_CHAT_XSIZE "CHATXSIZE"
#define CFG_CHAT_YSIZE "CHATYSIZE"

#define CFG_NEWS_XSIZE "NEWSXSIZE"
#define CFG_NEWS_YSIZE "NEWSYSIZE"

#define CFG_TASK_XSIZE "TASKXSIZE"
#define CFG_TASK_YSIZE "TASKYSIZE"

#define CFG_USER_XSIZE "USERXSIZE"
#define CFG_USER_YSIZE "USERYSIZE"

/* Toolbar window. gtkhx_prefs.geo.tool has always been written
 * via gtkhx_save_window_positions, but for years no key existed
 * in the prefs file so the value never actually survived the
 * write → load round-trip. */
#define CFG_TOOL_XSIZE "TOOLXSIZE"
#define CFG_TOOL_YSIZE "TOOLYSIZE"

/* Phase 8.E: voice device preferences. Empty string means "system
 * default" (autoaudiosrc / autoaudiosink); a non-empty value is the
 * stable gst::Device::name() that the runtime resolves via
 * DeviceMonitor at the next VoiceRuntime construction. Missing
 * devices fall back to the auto-* source / sink rather than failing
 * loudly. */
#define CFG_VOICE_INPUT_DEVICE "VOICEINPUTDEVICE"
#define CFG_VOICE_OUTPUT_DEVICE "VOICEOUTPUTDEVICE"

/* Phase 8.E follow-up: push-to-talk. CFG_VOICE_PTT_ENABLED is a
 * BOOLEAN toggle. CFG_VOICE_PTT_KEY is the canonical key spec
 * (gdk_keyval_name output, plus optional `<Control>` / `<Alt>` /
 * `<Shift>` prefix for modifier-augmented binds); empty string
 * means "no key captured yet" and the runtime hook stays inactive
 * even when the enabled toggle is on. Window-scoped key
 * controller; vocabulary is restricted to non-typing keys
 * (function keys, Pause, Insert, ScrollLock, and modifier+ combos)
 * so PTT can never eat keystrokes meant for chat-input typing. */
#define CFG_VOICE_PTT_ENABLED "VOICEPTTENABLED"
#define CFG_VOICE_PTT_KEY "VOICEPTTKEY"

/* Theming: name of the active theme. Picks $CONFIG/themes/<name>.ini
 * (with the built-in default GResource as the fallback). All scale,
 * palette, and icon-pack state lives in the theme file — this is
 * the only theming key in gtkhxrc. Empty / unset → "default". See
 * gtkhx_theme.{c,h}, docs/theming.md,
 * docs/theming-file-format.md. */
#define CFG_THEME_NAME "THEMENAME"

/* GKeyFile section name for the prefs file. */
#define CFG_KEYFILE_GROUP "gtkhx"

#endif /* ndef GTKHX_CFGKEYS_H */
