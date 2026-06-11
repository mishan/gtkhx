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

/* config.h first so PACKAGE / HAVE_LIBINTL_H / GETTEXT_PACKAGE
 * are visible to the gettext _() macro (and any other feature
 * test macros the rest of the headers reach for). Without this,
 * _("Join Voice") would fall back to the identity macro and the
 * toolbar labels wouldn't translate. */
#include "config.h"

#include "voice_panel.h"

#include "hl_access.h"
#include "hotline.h"
#include "voice.h"
#include "voice_runtime.h"

/* Lazy-create the per-session voice runtime on first use. Returns
 * sess->voice_runtime, NULL on construction failure (GStreamer not
 * initialised, webrtcbin plugin missing). Idempotent: returns the
 * same handle on subsequent calls. */
static struct gtkhx_voice_runtime *
ensure_voice_runtime (session *sess)
{
    if (!sess)
        return NULL;
    if (!sess->voice_runtime) {
        sess->voice_runtime = gtkhx_voice_runtime_new ();
    }
    return sess->voice_runtime;
}

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
    /* Read access from the session so update_button_labels can
     * apply the "disabled" tooltip and sensitivity itself —
     * otherwise voice_panel_refresh sets them and we'd
     * immediately overwrite. */
    session *sess = g_object_get_data (G_OBJECT (panel), KEY_SESS);
    gboolean access_ok = sess && panel_is_enabled (&sess->htlc);

    /* Join Voice ↔ Leave Voice */
    if (join_btn) {
        gtk_button_set_label (GTK_BUTTON (join_btn),
                              joined ? _ ("Leave Voice") : _ ("Join Voice"));
        if (!access_ok) {
            /* Access bit missing — spec-mandated permission
             * tooltip. Keep over the normal one when the panel
             * is visible-but-disabled (CAP_VOICE echoed but
             * HL_ACCESS_VOICE_CHAT cleared). */
            gtk_widget_set_tooltip_text (
                GTK_WIDGET (join_btn),
                _ ("Voice chat requires permission"));
        } else {
            gtk_widget_set_tooltip_text (
                GTK_WIDGET (join_btn),
                joined ? _ ("Leave the voice room")
                       : _ ("Join the voice room for this chat"));
        }
        panel_set_bool (panel, KEY_SUPPRESS, TRUE);
        gtk_toggle_button_set_active (join_btn, joined);
        panel_set_bool (panel, KEY_SUPPRESS, FALSE);
    }

    /* Mute is only relevant when joined AND access is granted.
     * Without the access gate, update_button_labels would re-
     * enable Mute after voice_panel_refresh disabled it
     * (the access bit was cleared between refresh and labels). */
    if (mute_btn) {
        gtk_widget_set_sensitive (GTK_WIDGET (mute_btn),
                                  joined && access_ok);
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

    gboolean sent;
    if (want_joined) {
        /* Spec recommends joining muted by default. Set the local
         * muted bit BEFORE issuing VOICE_JOIN so any downstream
         * code reading panel state during the send sees a
         * consistent "joining muted" view. Roll it back if the
         * wire-out skipped — a join that didn't ship shouldn't
         * pretend we're in voice. */
        gboolean prev_muted = panel_get_bool (panel, KEY_MUTED);
        panel_set_bool (panel, KEY_MUTED, TRUE);
        sent = hx_send_voice_join (&sess->htlc, cid);
        if (sent) {
            /* Drive the runtime state machine in parallel.
             * The C side still owns the wire-out (NoopBackend
             * on the runtime side), so this doesn't
             * double-send — it just feeds the state machine
             * the matching JoinRequested event so SDP /
             * ICE / pad dispatch on the inbound side has the
             * right active_cid + pipeline state. */
            struct gtkhx_voice_runtime *rt =
                ensure_voice_runtime (sess);
            if (rt) {
                gtkhx_voice_runtime_join (rt, cid);
            } else {
                /* Runtime construction failed (GStreamer not
                 * initialised, webrtcbin missing). Roll back
                 * the wire-side join so the UI doesn't pretend
                 * we're joined while no local WebRTC plumbing
                 * exists — without it, no SDP answer ever
                 * fires and the server times us out anyway. */
                (void) hx_send_voice_leave (&sess->htlc, cid);
                panel_set_bool (panel, KEY_MUTED, prev_muted);
                sent = FALSE;
            }
        } else {
            panel_set_bool (panel, KEY_MUTED, prev_muted);
        }
    } else {
        sent = hx_send_voice_leave (&sess->htlc, cid);
        if (sent) {
            gtkhx_voice_runtime_leave (sess->voice_runtime, cid);
            /* On a successful leave, clear muted so a
             * subsequent Join doesn't restore a stale Unmute
             * label. The disabled mute button (mute is gated on
             * "currently joined" in update_button_labels) would
             * otherwise show "Unmute" while not joined. */
            panel_set_bool (panel, KEY_MUTED, FALSE);
        }
    }

    if (sent) {
        /* Optimistic UI: reflect the requested state. The
         * voice-room-status signal will correct us if the wire
         * op fails after this point. */
        panel_set_bool (panel, KEY_JOINED, want_joined);
    } else {
        /* Wire-out was skipped — revert the toggle so the
         * button visually matches the underlying state. The
         * KEY_SUPPRESS guard at the top of this handler keeps
         * the set_active call from re-firing us. */
        panel_set_bool (panel, KEY_SUPPRESS, TRUE);
        gtk_toggle_button_set_active (btn, !want_joined);
        panel_set_bool (panel, KEY_SUPPRESS, FALSE);
    }
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

    gboolean sent = hx_send_voice_mute (&sess->htlc, cid, want_muted);
    if (sent) {
        gtkhx_voice_runtime_mute (sess->voice_runtime,
                                  want_muted ? 1 : 0);
        panel_set_bool (panel, KEY_MUTED, want_muted);
    } else {
        /* Revert the toggle so the button matches the
         * unchanged underlying state. */
        panel_set_bool (panel, KEY_SUPPRESS, TRUE);
        gtk_toggle_button_set_active (btn, !want_muted);
        panel_set_bool (panel, KEY_SUPPRESS, FALSE);
    }
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
    if (!show) {
        /* Hiding the panel means the server didn't echo
         * HTLC_CAP_VOICE (or the session disconnected and the
         * cap mask was cleared in network.c). Reset
         * joined/muted state so a reconnect to a server with
         * different voice support doesn't carry stale UI —
         * the chat tab persists across disconnect/reconnect,
         * so 'Leave Voice' / 'Unmute' labels would otherwise
         * linger from the previous session. */
        panel_set_bool (panel, KEY_JOINED, FALSE);
        panel_set_bool (panel, KEY_MUTED, FALSE);
        update_button_labels (panel);
        gtk_widget_set_visible (panel, FALSE);
        return;
    }
    gtk_widget_set_visible (panel, TRUE);

    /* Sensitivity: only the Join button's enabled/disabled state
     * lives here. The Mute button's sensitivity is computed from
     * (joined && access_ok) inside update_button_labels — keeping
     * it here would race with update_button_labels and overwrite.
     * Tooltips also live in update_button_labels for the same
     * reason: the disabled-state "Voice chat requires permission"
     * message would otherwise be immediately overwritten by the
     * normal Join/Leave tooltip on every refresh. */
    gboolean enabled = panel_is_enabled (&sess->htlc);
    GtkWidget *join_btn = g_object_get_data (G_OBJECT (panel), KEY_JOIN_BTN);
    if (join_btn) {
        gtk_widget_set_sensitive (join_btn, enabled);
    }
    update_button_labels (panel);
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
