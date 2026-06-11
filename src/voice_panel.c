/* voice_panel.c — Phase 8.D voice toolbar widget.
 *
 * See voice_panel.h for the rules. This file is the implementation:
 * a GtkBox with two GtkToggleButtons (Join / Mute), the visibility
 * gating against HTLC_CAP_VOICE + HL_ACCESS_VOICE_CHAT, and the
 * click handlers that drive the Phase 8.A wire-out functions
 * (hx_send_voice_join / hx_send_voice_leave / hx_send_voice_mute).
 *
 * The internal "joined" / "muted" state lives in g_object data
 * keys on the panel widget. Phase 8.D's signal-driven update path
 * (voice-room-status from the runtime) will set those keys directly
 * once wired; for now the click handlers update them immediately
 * as a usability stopgap.
 */

#include "voice_panel.h"

#include "hl_access.h"
#include "hotline.h"
#include "voice.h"

/* Storage keys for per-panel state. */
#define KEY_SESS    "voice-panel-sess"
#define KEY_CID     "voice-panel-cid"
#define KEY_JOIN_BTN "voice-panel-join-btn"
#define KEY_MUTE_BTN "voice-panel-mute-btn"
#define KEY_JOINED  "voice-panel-joined"
#define KEY_MUTED   "voice-panel-muted"
/* Re-entrancy guard so a programmatic
 * gtk_toggle_button_set_active() inside our handler doesn't fire
 * the handler again. */
#define KEY_SUPPRESS "voice-panel-suppress"

static gboolean
panel_should_show (struct htlc_conn *htlc)
{
    return (htlc->caps & HTLC_CAP_VOICE) != 0;
}

static gboolean
panel_is_enabled (struct htlc_conn *htlc)
{
    return hl_access_has ((const guint8 *)&htlc->access,
                          HL_ACCESS_VOICE_CHAT);
}

static gboolean
panel_get_bool (GtkWidget *panel, const char *key)
{
    gpointer v = g_object_get_data (G_OBJECT (panel), key);
    return GPOINTER_TO_INT (v) != 0;
}

static void
panel_set_bool (GtkWidget *panel, const char *key, gboolean value)
{
    g_object_set_data (G_OBJECT (panel), key, GINT_TO_POINTER (value ? 1 : 0));
}

static void
update_button_labels (GtkWidget *panel)
{
    GtkToggleButton *join_btn = g_object_get_data (G_OBJECT (panel),
                                                   KEY_JOIN_BTN);
    GtkToggleButton *mute_btn = g_object_get_data (G_OBJECT (panel),
                                                   KEY_MUTE_BTN);
    gboolean joined = panel_get_bool (panel, KEY_JOINED);
    gboolean muted = panel_get_bool (panel, KEY_MUTED);

    /* Join Voice ↔ Leave Voice */
    if (join_btn) {
        gtk_button_set_label (GTK_BUTTON (join_btn),
                              joined ? _ ("Leave Voice") : _ ("Join Voice"));
        gtk_widget_set_tooltip_text (
            GTK_WIDGET (join_btn),
            joined ? _ ("Leave the voice room")
                   : _ ("Join the voice room for this chat"));
        panel_set_bool (panel, KEY_SUPPRESS, TRUE);
        gtk_toggle_button_set_active (join_btn, joined);
        panel_set_bool (panel, KEY_SUPPRESS, FALSE);
    }

    /* Mute is only relevant when joined. */
    if (mute_btn) {
        gtk_widget_set_sensitive (GTK_WIDGET (mute_btn), joined);
        gtk_button_set_label (GTK_BUTTON (mute_btn),
                              muted ? _ ("Unmute") : _ ("Mute"));
        gtk_widget_set_tooltip_text (
            GTK_WIDGET (mute_btn),
            muted ? _ ("Restore your microphone")
                  : _ ("Stop sending audio"));
        panel_set_bool (panel, KEY_SUPPRESS, TRUE);
        gtk_toggle_button_set_active (mute_btn, muted);
        panel_set_bool (panel, KEY_SUPPRESS, FALSE);
    }
}

static void
on_join_toggled (GtkToggleButton *btn, gpointer user_data)
{
    GtkWidget *panel = user_data;
    if (panel_get_bool (panel, KEY_SUPPRESS))
        return;

    session *sess = g_object_get_data (G_OBJECT (panel), KEY_SESS);
    guint32 cid = GPOINTER_TO_UINT (
        g_object_get_data (G_OBJECT (panel), KEY_CID));
    gboolean want_joined = gtk_toggle_button_get_active (btn);

    if (!sess)
        return;

    if (want_joined) {
        /* Spec recommends joining muted by default; the local mute
         * state is set to TRUE here so the subsequent state-machine
         * step has a consistent view. The actual wire MUTE is sent
         * only once we're actually in voice (the state machine emits
         * it from the appropriate transition). */
        panel_set_bool (panel, KEY_MUTED, TRUE);
        hx_send_voice_join (&sess->htlc, cid);
    } else {
        hx_send_voice_leave (&sess->htlc, cid);
    }

    /* Optimistic UI: reflect the requested state. The
     * voice-room-status signal will correct us if the wire op
     * fails. */
    panel_set_bool (panel, KEY_JOINED, want_joined);
    update_button_labels (panel);
}

static void
on_mute_toggled (GtkToggleButton *btn, gpointer user_data)
{
    GtkWidget *panel = user_data;
    if (panel_get_bool (panel, KEY_SUPPRESS))
        return;

    session *sess = g_object_get_data (G_OBJECT (panel), KEY_SESS);
    guint32 cid = GPOINTER_TO_UINT (
        g_object_get_data (G_OBJECT (panel), KEY_CID));
    gboolean want_muted = gtk_toggle_button_get_active (btn);

    if (!sess)
        return;

    hx_send_voice_mute (&sess->htlc, cid, want_muted);
    panel_set_bool (panel, KEY_MUTED, want_muted);
    update_button_labels (panel);
}

GtkWidget *
voice_panel_new (session *sess, guint32 cid)
{
    GtkWidget *panel = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class (panel, "toolbar");
    gtk_widget_set_margin_top (panel, 2);
    gtk_widget_set_margin_bottom (panel, 2);

    /* Stash session + cid so the click handlers can reach them
     * without needing a custom struct. */
    g_object_set_data (G_OBJECT (panel), KEY_SESS, sess);
    g_object_set_data (G_OBJECT (panel), KEY_CID,
                       GUINT_TO_POINTER (cid));

    GtkWidget *join_btn = gtk_toggle_button_new_with_label (_ ("Join Voice"));
    GtkWidget *mute_btn = gtk_toggle_button_new_with_label (_ ("Mute"));
    g_object_set_data (G_OBJECT (panel), KEY_JOIN_BTN, join_btn);
    g_object_set_data (G_OBJECT (panel), KEY_MUTE_BTN, mute_btn);

    g_signal_connect (join_btn, "toggled", G_CALLBACK (on_join_toggled),
                      panel);
    g_signal_connect (mute_btn, "toggled", G_CALLBACK (on_mute_toggled),
                      panel);

    gtk_box_append (GTK_BOX (panel), join_btn);
    gtk_box_append (GTK_BOX (panel), mute_btn);

    /* Initial join/mute state: not joined, not muted. */
    panel_set_bool (panel, KEY_JOINED, FALSE);
    panel_set_bool (panel, KEY_MUTED, FALSE);
    update_button_labels (panel);

    /* Apply visibility + enabled state for the current htlc. */
    voice_panel_refresh (panel, sess);

    return panel;
}

void
voice_panel_refresh (GtkWidget *panel, session *sess)
{
    if (!panel || !sess)
        return;

    gboolean show = panel_should_show (&sess->htlc);
    gtk_widget_set_visible (panel, show);
    if (!show)
        return;

    gboolean enabled = panel_is_enabled (&sess->htlc);
    GtkWidget *join_btn = g_object_get_data (G_OBJECT (panel), KEY_JOIN_BTN);
    GtkWidget *mute_btn = g_object_get_data (G_OBJECT (panel), KEY_MUTE_BTN);
    if (join_btn) {
        gtk_widget_set_sensitive (join_btn, enabled);
        if (!enabled) {
            gtk_widget_set_tooltip_text (
                join_btn, _ ("Voice chat requires permission"));
        }
    }
    if (mute_btn) {
        /* Mute is additionally gated on "currently joined" — see
         * update_button_labels. */
        if (!enabled)
            gtk_widget_set_sensitive (mute_btn, FALSE);
    }
}

void
voice_panel_set_joined (GtkWidget *panel, gboolean joined)
{
    if (!panel)
        return;
    panel_set_bool (panel, KEY_JOINED, joined);
    update_button_labels (panel);
}

void
voice_panel_set_muted (GtkWidget *panel, gboolean muted)
{
    if (!panel)
        return;
    panel_set_bool (panel, KEY_MUTED, muted);
    update_button_labels (panel);
}

void
voice_panel_refresh_all_chats (session *sess)
{
    if (!sess || !sess->gchats)
        return;
    GHashTableIter iter;
    gpointer val;
    g_hash_table_iter_init (&iter, sess->gchats);
    while (g_hash_table_iter_next (&iter, NULL, &val)) {
        struct gtkhx_chat *gchat = val;
        if (gchat && gchat->voice_panel) {
            voice_panel_refresh (gchat->voice_panel, sess);
        }
    }
}
