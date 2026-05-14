/*
 * files_complete.h — path completion for the local files panel's
 * path entry. Pops a small list of matching subdirectories under
 * the entry as the user types.
 *
 * Behaviour:
 *
 *   • Local filesystem only. The remote provider can't synchronously
 *     enumerate without an RPC round-trip, so we don't try.
 *   • Directories only (files don't make sense as navigation
 *     targets — Enter on a file in the listing already routes to
 *     activate_entry).
 *   • Smart case: case-insensitive by default; switches to
 *     case-sensitive once the prefix contains an uppercase
 *     character. Same heuristic as Vim's smartcase / fzf.
 *   • Hidden files (leading-dot names) are suppressed unless the
 *     prefix the user is typing also starts with a dot.
 *
 * Keyboard:
 *
 *   ↓ / ↑     navigate the suggestion list
 *   Enter     insert the highlighted suggestion (or fall through
 *             to the entry's own activate handler if none is
 *             highlighted)
 *   Tab       insert the highlighted suggestion (or the first one
 *             if nothing is highlighted yet)
 *   Esc       close the popover, leave the entry alone
 *
 * Lifecycle:
 *
 *   The completion object owns the popover (parented to the entry,
 *   so it's destroyed with the entry) and a couple of signal
 *   handlers. Caller must call hx_path_complete_free during panel
 *   teardown to disconnect those handlers before the entry itself
 *   goes away.
 */

#ifndef HX_FILES_COMPLETE_H
#define HX_FILES_COMPLETE_H 1

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _hx_path_complete hx_path_complete;

extern hx_path_complete *hx_path_complete_attach (GtkEntry *entry);
extern void hx_path_complete_free (hx_path_complete *c);

G_END_DECLS

#endif /* HX_FILES_COMPLETE_H */
