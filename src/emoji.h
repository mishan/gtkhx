#ifndef HX_EMOJI_H
#define HX_EMOJI_H

#include <gtk/gtk.h>

/*
 * emoji — shared emoji-picker button across GtkHx's chat-style inputs.
 *
 * Used by the public chat, private chat, and private-message windows.
 * Each of those windows has a GtkTextView as its outgoing-text input;
 * this module produces a single button that, when clicked, pops up a
 * GtkEmojiChooser anchored to the button and inserts the picked
 * codepoint at the input's cursor position.
 *
 * GtkEmojiChooser is GTK's built-in emoji picker — it ships with
 * category tabs, search, skin-tone variant selection, and keyboard
 * navigation; no point rolling our own.
 *
 * Layout convention: the chat/PM input is wrapped in a horizontal box
 * with the input scrolled-window on the left; the caller packs the
 * returned button to the right of it with valign=GTK_ALIGN_END so the
 * button sits next to the bottom line of the input regardless of how
 * much the input has auto-grown.
 */

/* Create the emoji-picker button bound to `target_text_view`. The
 * button owns its popover; the returned widget can be packed into any
 * container and destroyed normally. `target_text_view` must outlive
 * the button (the on-click handler dereferences it), which is the
 * trivial case in our codebase since both live in the same window. */
extern GtkWidget *hx_emoji_button_new (GtkWidget *target_text_view);

#endif /* HX_EMOJI_H */
