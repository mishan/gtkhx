/* voice_panel.h — Phase 8.D voice toolbar for the chat tab.
 *
 * One toolbar per chat tab: Join Voice / Leave Voice (toggle button)
 * and Mute / Unmute (toggle button). Sits above the chat output, below
 * the subject bar.
 *
 * Visibility / state rules (see docs/voice.md "Voice indicators in the user list"):
 *
 *   - htlc->caps & HTLC_CAP_VOICE unset → toolbar is hidden entirely.
 *     Most servers won't echo the cap; don't clutter the tab with a
 *     button the user can't use.
 *   - Cap echoed but HL_ACCESS_VOICE_CHAT bit not set → toolbar is
 *     visible but disabled, with tooltip "Voice chat requires
 *     permission". Spec calls out keeping the UI visible so users
 *     know to ask an admin for access.
 *   - Cap echoed and access bit set → fully interactive.
 *
 * The widget tracks join + mute state internally. Phase 8.D drives
 * those flags from the GtkhxSession `voice-room-status` signal once
 * we wire it; the initial commit just exposes the buttons + the
 * call into the Phase 8.A wire layer (`hx_send_voice_join` /
 * `hx_send_voice_leave` / `hx_send_voice_mute`).
 */

#ifndef GTKHX_VOICE_PANEL_H
#define GTKHX_VOICE_PANEL_H

#include <gtk/gtk.h>

#include "hx.h"

/* Forward decl — we don't include htlc internals here. */
struct htlc_conn;

/* Create the voice panel toolbar for `cid`. Returns a GtkWidget*
 * (currently a GtkBox holding the buttons). Caller is responsible
 * for inserting it into the chat tab layout. The toolbar reads
 * `htlc->caps` + `htlc->access` at construction to set its
 * initial visibility / enabled state.
 *
 * The widget lives as long as the chat tab; destroyed by the
 * normal GTK parent-destroys-children walk when the tab closes.
 */
extern GtkWidget *voice_panel_new (session *sess, guint32 cid);

/* Refresh the panel's visibility + enabled state after a
 * `htlc->caps` / `htlc->access` change (typically: post-login,
 * once SELFINFO has populated the access bitmap). Safe to call
 * with `panel == NULL` (no-op). */
extern void voice_panel_refresh (GtkWidget *panel, session *sess);

/* Walk every live chat tab in `sess->gchats` and call
 * `voice_panel_refresh` on its voice panel. Called from the
 * post-login UI refresh path (`setbtns`) so the panels pick up
 * `htlc->caps` and `htlc->access` after they land — the chat
 * windows open BEFORE the LOGIN reply finishes, so the
 * construction-time refresh runs against zeroed caps and the
 * panel stays hidden until this is called. */
extern void voice_panel_refresh_all_chats (session *sess);

/* Update the panel's "joined to voice" indicator. Driven by the
 * GtkhxSession `voice-room-status` signal in production. The Phase
 * 8.D initial commit wires the buttons to call this directly from
 * their click handlers as immediate UI feedback; the signal-driven
 * truth-of-record lands when we hook the signal. */
extern void voice_panel_set_joined (GtkWidget *panel, gboolean joined);

/* Update the panel's mute indicator. Same caveat as
 * voice_panel_set_joined. */
extern void voice_panel_set_muted (GtkWidget *panel, gboolean muted);

#endif /* GTKHX_VOICE_PANEL_H */
