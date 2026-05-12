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

typedef struct {
	int xsize, ysize;
	int xpos, ypos;
	unsigned int open:1;
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
	unsigned char auto_reply;
	unsigned char timestamp;
	unsigned char word_wrap;
	unsigned char file_samewin;
	unsigned char news_samewin;
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

	int out_bps;
	int in_bps;
};

extern struct gtkhx_prefs gtkhx_prefs;

struct hx_sounds {
	unsigned char on;
	unsigned char invite, chat, error, file, join, login, msg, news, part;
};

#endif /* ndef __gtkhx_PREFS_H */
