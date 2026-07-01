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

#include "debug.h"
#include "hl_access.h"
#include "hotline.h"
#include "toolbar.h" /* toolbar_show_toast */
#include "voice.h"
#include "voice_model.h"
#include "voice_runtime.h"

/* Bridge from the Rust runtime's Action::SendWireFrame dispatch to
 * the C-side hx_send_voice_* helpers. The runtime invokes this on
 * the main thread for every outbound voice opcode the state
 * machine produces. user_data is the `htlc_conn *` the runtime was
 * constructed with (production: &sess->htlc).
 *
 * Body layout, per hxvoice::state::encode_*:
 *
 *   - 600 JOIN, 601 LEAVE:  4 bytes BE cid
 *   - 603 SDP_ANSWER:       4 bytes BE cid + SDP bytes (no NUL)
 *   - 604 ICE:              4 bytes BE cid + JSON bytes (no NUL)
 *   - 606 MUTE:             4 bytes BE cid + 2 bytes BE muted-flag
 *
 * 603 (SDP answer) and 604 (ICE) come from webrtcbin events that
 * only the runtime sees — those go on the wire via the bridge.
 *
 * 601 LEAVE: ALSO emitted from the runtime (state.rs's fail()
 * path on ICE failure / wedge timeout / server task error), so
 * the bridge is the wire-out path for LEAVE too. The UI click
 * handler is now runtime-driven: on_join_toggled fires
 * gtkhx_voice_runtime_leave, the state machine's LeaveRequested
 * arm emits Action::SendWireFrame { opcode: 601, ... }, and
 * this bridge calls hx_send_voice_leave. Pre-fix, the UI handler
 * called hx_send_voice_leave directly and the bridge skipped
 * LEAVE to avoid double-send — but Fix #3's fail()-path LEAVE
 * was silently dropped at the bridge because the bridge couldn't
 * tell "UI already sent it" from "runtime needs to send it."
 * Moving the wire-out responsibility to the runtime side closes
 * that gap.
 *
 * 600 JOIN and 606 MUTE remain UI-driven: JOIN's return value
 * gates runtime construction (we need to roll back with a wire
 * LEAVE if the runtime fails to come up), and MUTE fires often
 * enough during PTT that an extra runtime-dispatch hop would
 * add measurable jitter to the press/release edges. Both stay
 * in the skip list below; fail() doesn't emit either of them
 * today, so the divergence Fix #3 closed for LEAVE doesn't
 * apply. */
static void
voice_runtime_send_wire_frame_cb (void *user_data, uint32_t opcode,
                                  const uint8_t *body, size_t body_len)
{
    struct htlc_conn *htlc = user_data;
    if (!htlc || !body || body_len < 4) {
        debug_log ("voice",
                   "bridge dropping opcode 0x%x: htlc=%p body=%p len=%zu",
                   opcode, (void *) htlc, (const void *) body, body_len);
        return;
    }

    /* All four encodings start with a 4-byte BE cid. */
    guint32 cid = ((guint32) body[0] << 24) | ((guint32) body[1] << 16) |
                  ((guint32) body[2] << 8)  | (guint32) body[3];
    const guint8 *payload = body + 4;
    gsize payload_len = body_len - 4;

    gboolean sent;
    switch (opcode) {
    case HTLC_HDR_VOICE_SDP_ANSWER:
        /* SDP payload is empty on a join-without-answer? Never —
         * encode_cid_plus_sdp only fires from Action paths that
         * carry a real SDP blob. Bail loudly if we ever see one. */
        if (payload_len == 0) {
            debug_log ("voice",
                       "bridge: empty SDP_ANSWER body for cid=%u (skipped)",
                       cid);
            return;
        }
        sent = hx_send_voice_sdp_answer (htlc, cid, payload, payload_len);
        if (!sent) {
            /* The wire helper skipped: CAP_VOICE cleared mid-
             * session, sdp_len > 65535, or the proto builder
             * rejected the input. Without this log, an SDP answer
             * silently dropping turns into "Janus never finishes
             * the handshake and the user has no idea why". */
            debug_log ("voice",
                       "bridge: hx_send_voice_sdp_answer FAILED cid=%u "
                       "sdp_len=%zu",
                       cid, payload_len);
        }
        return;
    case HTLC_HDR_VOICE_ICE:
        /* Empty ICE body is the end-of-candidates marker, which
         * the wire helper accepts as NULL/0. payload_len == 0
         * shouldn't happen from encode_cid_plus_ice (it always
         * includes at least the JSON braces), but handle it
         * defensively. */
        if (payload_len == 0) {
            sent = hx_send_voice_ice (htlc, cid, NULL, 0);
        } else {
            sent = hx_send_voice_ice (htlc, cid, payload, payload_len);
        }
        if (!sent) {
            /* Same triage rationale as SDP_ANSWER: losing ICE
             * candidates without a trace breaks ICE establishment
             * with zero diagnostics. */
            debug_log ("voice",
                       "bridge: hx_send_voice_ice FAILED cid=%u ice_len=%zu",
                       cid, payload_len);
        }
        return;
    case HTLC_HDR_VOICE_LEAVE:
        /* Runtime-driven: the state machine emits SendWireFrame
         * for LEAVE from BOTH the UI-click LeaveRequested arm AND
         * the failure-collapse fail() path. The bridge no longer
         * tries to disambiguate; it just ships the frame. The UI
         * handler in on_join_toggled used to call hx_send_voice_leave
         * directly + drive the runtime, with a skip-here for the
         * runtime's matching emit — Fix #3 made the runtime drive
         * the wire frame from fail() too, and the skip-here was
         * silently dropping that path's wire-out (the bug visible
         * in voice.log around line 467 of ninja/voice.log).
         *
         * payload_len is 0 for LEAVE (the body is just the cid,
         * which we already extracted). hx_send_voice_leave's
         * signature only takes htlc + cid. */
        sent = hx_send_voice_leave (htlc, cid);
        if (!sent) {
            debug_log ("voice",
                       "bridge: hx_send_voice_leave FAILED cid=%u",
                       cid);
        }
        return;
    case HTLC_HDR_VOICE_JOIN:
    case HTLC_HDR_VOICE_MUTE:
        /* Already sent by the UI click handler — skipping here
         * prevents double-send. The runtime's SendWireFrame
         * action is unconditional, so the bridge is the right
         * place to enforce the split. LEAVE used to be in this
         * skip group too; see the new dedicated arm above for the
         * rationale and the bug it closes. */
        debug_log ("voice",
                   "bridge: opcode 0x%x for cid=%u handled by UI (skipped)",
                   opcode, cid);
        return;
    default:
        debug_log ("voice",
                   "bridge: unknown opcode 0x%x for cid=%u (skipped)",
                   opcode, cid);
        return;
    }
}

/* Forward decl — defined below. Called from
 * ensure_voice_runtime's signal-callbacks registration; the
 * static-storage-class ordering needs the forward so the bridge
 * callback's address is taken at the same compilation point as
 * voice_runtime_send_wire_frame_cb above. */
static void voice_runtime_state_changed_cb (void *user_data,
                                            gtkhx_voice_state state);
static void voice_runtime_mute_changed_cb (void *user_data, int muted);
static void voice_runtime_speaker_changed_cb (void *user_data, uint16_t uid,
                                              int is_speaking);
static void voice_runtime_error_cb (void *user_data, const char *text);

/* Lazy-create the per-session voice runtime on first use. Returns
 * sess->voice_runtime, NULL on construction failure (GStreamer not
 * initialised, webrtcbin plugin missing). Idempotent: returns the
 * same handle on subsequent calls.
 *
 * Registers both the wire-out bridge (so the state machine's 603
 * SDP_ANSWER / 604 ICE actions reach hlwrite_chunks) and the
 * signal-in bridge (so the panel's joined / muted state reflects
 * authoritative state-machine state instead of optimistic UI).
 * user_data is &sess->htlc; from there the signal handlers reach
 * sess via hx_active_session (). Both stay valid until
 * network.c::hx_htlc_close frees the runtime on disconnect. */
static struct gtkhx_voice_runtime *
ensure_voice_runtime (session *sess)
{
    if (!sess)
        return NULL;
    if (!sess->voice_runtime) {
        gtkhx_voice_runtime_signal_callbacks signals = {
            .state_changed   = voice_runtime_state_changed_cb,
            .mute_changed    = voice_runtime_mute_changed_cb,
            .speaker_changed = voice_runtime_speaker_changed_cb,
            .error           = voice_runtime_error_cb,
        };
        sess->voice_runtime = gtkhx_voice_runtime_new_v2 (
            &sess->htlc, voice_runtime_send_wire_frame_cb, &signals);
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

/* Registry of every live voice panel (borrowed pointers). The
 * runtime state/mute signal callbacks used to find panels by walking
 * sess->gchats for gchat->voice_panel, which only works for panels
 * that hang off a gtkhx_chat. Since the controls now live in the
 * user-list surfaces — the public room's controls sit in the
 * standalone Users window, which is NOT a gchat sidebar — that walk
 * can't reach them. This flat registry (keyed at update time on each
 * panel's KEY_CID) covers both surfaces uniformly. Panels add
 * themselves in voice_panel_new and remove themselves on
 * GtkWidget::destroy. */
static GPtrArray *voice_panels;

static void
on_panel_destroy (GtkWidget *panel, gpointer user_data)
{
    (void)user_data;
    if (voice_panels) {
        g_ptr_array_remove_fast (voice_panels, panel);
    }
}

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

    /* Join Voice ↔ Leave Voice. Icon-only now that the controls live
     * in the user-list button bar; the tooltip carries the words.
     * call-start/call-stop are stock symbolic icons (adwaita-icon-
     * theme), so no bundled art is needed. */
    if (join_btn) {
        gtk_button_set_icon_name (GTK_BUTTON (join_btn),
                                  joined ? "call-stop-symbolic"
                                         : "call-start-symbolic");
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
        gtk_button_set_icon_name (
            GTK_BUTTON (mute_btn),
            muted ? "microphone-sensitivity-muted-symbolic"
                  : "audio-input-microphone-symbolic");
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
        sent = hx_send_voice_join (&sess->htlc, cid);
        if (sent) {
            /* Drive the runtime state machine. handle_event runs
             * synchronously: by the time gtkhx_voice_runtime_join
             * returns, the state machine has transitioned
             * Idle→JoinSent and the StateChanged signal has fired
             * — voice_runtime_state_changed_cb above has already
             * updated KEY_JOINED on this panel. */
            struct gtkhx_voice_runtime *rt =
                ensure_voice_runtime (sess);
            if (rt) {
                gtkhx_voice_runtime_join (rt, cid);
                /* Spec recommends joining muted by default. Two
                 * steps, IN THIS ORDER:
                 *
                 *   1. Send the VOICE_MUTE (606) wire frame so the
                 *      server actually learns we joined muted. This
                 *      MUST be the UI's job: the bridge
                 *      (voice_runtime_send_wire_frame_cb) deliberately
                 *      SKIPS runtime-emitted MUTE frames on the
                 *      assumption the UI already sent them (kept
                 *      UI-driven for PTT latency). So the matching
                 *      606 the state machine emits in step 2 is
                 *      swallowed there — without this explicit send,
                 *      the start-muted intent never reaches the wire.
                 *      (Same defect class as the LEAVE "Fix #3" the
                 *      bridge comment describes.)
                 *
                 *   2. Drive the state machine's self.muted to TRUE so
                 *      it stays in sync with what the toolbar shows;
                 *      the MuteChanged signal that this fires updates
                 *      KEY_MUTED.
                 *
                 * Without step 2, the optimistic KEY_MUTED=TRUE would
                 * diverge from the state machine's default
                 * (self.muted=false), and the user's first Unmute
                 * click would hit the redundant-mute no-op branch —
                 * no MuteChanged signal fires, so the label stays
                 * stuck on 'Unmute' until a second click actually
                 * transitions the machine. */
                (void) hx_send_voice_mute (&sess->htlc, cid, TRUE);
                gtkhx_voice_runtime_mute (rt, 1);
            } else {
                /* Runtime construction failed (GStreamer not
                 * initialised, webrtcbin missing). Roll back the
                 * wire-side join so the user isn't left in a
                 * room that never finishes the handshake. */
                (void) hx_send_voice_leave (&sess->htlc, cid);
                sent = FALSE;
            }
        }
    } else {
        /* Runtime-driven LEAVE. We don't call hx_send_voice_leave
         * here: the state machine's LeaveRequested arm emits
         * Action::SendWireFrame(601), and the bridge above (the
         * HTLC_HDR_VOICE_LEAVE case in voice_runtime_send_wire_frame_cb)
         * is what calls hx_send_voice_leave on the wire.
         *
         * Pre-Fix-#3 this handler called hx_send_voice_leave directly
         * and the bridge skipped LEAVE to avoid a double-send. That
         * was fine for the UI path, but it meant Fix #3's fail()-
         * emitted LEAVE was also dropped at the bridge — the bridge
         * couldn't tell which path the LEAVE came from. Moving the
         * single wire-out path into the bridge closes that gap.
         *
         * Side-effects via the StateChanged signal: the runtime
         * walks the state machine to Leaving (LeaveRequested arm)
         * and our signal handler picks up the transition to clear
         * KEY_JOINED + KEY_MUTED on this panel.
         *
         * If sess->voice_runtime is NULL (lazy construction never
         * fired — shouldn't happen with the button enabled in the
         * "leave" state, but defensive), drop silently. */
        if (sess->voice_runtime) {
            gtkhx_voice_runtime_leave (sess->voice_runtime, cid);
        }
        sent = TRUE;
    }

    if (!sent) {
        /* Wire-out was skipped — revert the toggle so the
         * button visually matches the underlying state. The
         * KEY_SUPPRESS guard at the top of this handler keeps
         * the set_active call from re-firing us.
         *
         * Sent path: the state_changed signal handler already
         * called update_button_labels with the authoritative
         * KEY_JOINED value, so no extra UI refresh is needed.
         * Skipped path: KEY_JOINED is still its pre-click value,
         * so update_button_labels below restores the label. */
        panel_set_bool (panel, KEY_SUPPRESS, TRUE);
        gtk_toggle_button_set_active (btn, !want_joined);
        panel_set_bool (panel, KEY_SUPPRESS, FALSE);
        update_button_labels (panel);
    }
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
        /* gtkhx_voice_runtime_mute fires MuteToggleRequested which
         * the state machine handles synchronously: by the time it
         * returns, the MuteChanged signal has already updated
         * KEY_MUTED on this panel via voice_runtime_mute_changed_cb. */
        gtkhx_voice_runtime_mute (sess->voice_runtime,
                                  want_muted ? 1 : 0);
    } else {
        /* Revert the toggle so the button matches the unchanged
         * underlying state. The KEY_MUTED model hasn't moved, so
         * update_button_labels at the bottom restores the label
         * to its prior value. */
        panel_set_bool (panel, KEY_SUPPRESS, TRUE);
        gtk_toggle_button_set_active (btn, !want_muted);
        panel_set_bool (panel, KEY_SUPPRESS, FALSE);
        update_button_labels (panel);
    }
}

GtkWidget *
voice_panel_new (session *sess, guint32 cid)
{
    /* A tight two-icon-button box designed to sit inline with the
     * other icon buttons in the user-list button bar (msg / chat /
     * kick / ban / …). No "toolbar" chrome or margins — it blends
     * into the surrounding bar. */
    GtkWidget *panel = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);

    /* Stash session + cid so the click handlers can reach them
     * without needing a custom struct. */
    g_object_set_data (G_OBJECT (panel), KEY_SESS, sess);
    g_object_set_data (G_OBJECT (panel), KEY_CID,
                       GUINT_TO_POINTER (cid));

    /* Icon-only toggle buttons; update_button_labels sets the actual
     * icon per state (call-start/stop, mic/mic-muted) + the tooltip. */
    GtkWidget *join_btn = gtk_toggle_button_new ();
    GtkWidget *mute_btn = gtk_toggle_button_new ();
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

    /* Register for signal-driven state updates; auto-deregister when
     * the widget is destroyed (tab close / session teardown). */
    if (!voice_panels) {
        voice_panels = g_ptr_array_new ();
    }
    g_ptr_array_add (voice_panels, panel);
    g_signal_connect (panel, "destroy", G_CALLBACK (on_panel_destroy), NULL);

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

/* Map FFI gtkhx_voice_state → "in voice or not". From the user's
 * perspective the toolbar should flip to "Leave Voice" as soon as
 * the JOIN goes on the wire so they can cancel the attempt — they
 * don't care that we're still mid-SDP-offer or ICE-establishing.
 * Treat anything past Idle as joined, EXCEPT Leaving (the
 * terminal post-tear-down state, where the panel should read
 * "Join Voice" again so the user can rejoin). */
static gboolean
state_is_joined (gtkhx_voice_state state)
{
    return state == GTKHX_VOICE_STATE_JOIN_SENT ||
           state == GTKHX_VOICE_STATE_OFFER_PENDING ||
           state == GTKHX_VOICE_STATE_CONNECTING ||
           state == GTKHX_VOICE_STATE_CONNECTED;
}

/* Signal handler: voice_runtime emitted SignalKind::StateChanged.
 * user_data is &htlc; we reach the runtime + voice model via
 * hx_active_session () (the focused session — single-session today)
 * and iterate the voice_panels registry. Updates KEY_JOINED on every
 * panel: TRUE only on the panel matching the runtime's active cid,
 * FALSE on all others. */
static void
voice_runtime_state_changed_cb (void *user_data, gtkhx_voice_state state)
{
    (void) user_data;
    session *sess = hx_active_session ();

    gboolean joined_now = state_is_joined (state);

    /* Figure out which cid (if any) the runtime considers active.
     * Idle / Leaving paths return 0, which we treat as "no active
     * room" so every panel reads as not joined. */
    uint32_t active_cid = 0;
    int has_active = 0;
    if (sess->voice_runtime) {
        has_active = gtkhx_voice_runtime_active_cid (sess->voice_runtime,
                                                    &active_cid);
    }

    debug_log ("voice",
               "panel: state=%u joined=%d active_cid=%s%u",
               /* enum → unsigned int through varargs:
                * gtkhx_voice_state is an enum that promotes to
                * int; passing it directly to %u is UB. Cast to
                * unsigned so the varargs type matches the format
                * specifier. */
               (unsigned int) state,
               joined_now ? 1 : 0,
               has_active ? "" : "(none) ",
               (unsigned int) active_cid);

    if (voice_panels) {
        for (guint i = 0; i < voice_panels->len; i++) {
            GtkWidget *panel = g_ptr_array_index (voice_panels, i);
            guint32 cid = GPOINTER_TO_UINT (
                g_object_get_data (G_OBJECT (panel), KEY_CID));
            gboolean is_active = has_active && cid == active_cid;
            panel_set_bool (panel, KEY_JOINED, joined_now && is_active);
            /* Leaving / Idle on a previously-joined panel needs muted
             * cleared too — otherwise a subsequent Join sees a stale
             * muted icon, which update_button_labels' disabled-state
             * gate would otherwise paper over. */
            if (!joined_now || !is_active) {
                panel_set_bool (panel, KEY_MUTED, FALSE);
            }
            update_button_labels (panel);
        }
    }

    /* When we transition to a non-joined state (LEAVING terminal
     * collapse, or IDLE if a future state-machine revision adds
     * Leaving → Idle), every speaker indicator we've been showing
     * is now lying — we're no longer in the room, so we won't
     * receive the 605 updates that would otherwise show users
     * leaving / muting. Clear the model so the column blanks
     * synchronously. Re-population happens on the next JOIN
     * reply / 605, exactly like a fresh connect would. */
    if (!joined_now && sess->voice_model) {
        hx_voice_model_clear (sess->voice_model);
    }
}

/* Signal handler: voice_runtime emitted SignalKind::MuteChanged.
 * Updates KEY_MUTED on the panel matching active_cid. */
static void
voice_runtime_mute_changed_cb (void *user_data, int muted)
{
    (void) user_data;
    session *sess = hx_active_session ();
    if (!sess->voice_runtime)
        return;

    uint32_t active_cid = 0;
    if (!gtkhx_voice_runtime_active_cid (sess->voice_runtime, &active_cid)) {
        debug_log ("voice",
                   "panel: mute_changed muted=%d but no active cid (dropped)",
                   muted);
        return;
    }
    if (!voice_panels)
        return;
    for (guint i = 0; i < voice_panels->len; i++) {
        GtkWidget *panel = g_ptr_array_index (voice_panels, i);
        guint32 cid = GPOINTER_TO_UINT (
            g_object_get_data (G_OBJECT (panel), KEY_CID));
        if (cid == active_cid) {
            panel_set_bool (panel, KEY_MUTED, muted ? TRUE : FALSE);
            update_button_labels (panel);
        }
    }
}

/* Signal handler: voice_runtime emitted SignalKind::Error.
 * Routes the message into the application toast overlay so the
 * user sees soft-failure notices (Media-timeout soft toast,
 * server task-error replies, DTLS / ICE connectivity warnings)
 * instead of silent state churn.
 *
 * Pairs with the (Connected, Timeout::Media) softening in
 * hxvoice::state — the state machine emits Error+keeps-connected
 * for the post-Phase-8.G softened timeouts (DTLS / ICE / Media)
 * and for ServerTaskError replies on 600/601/603/606. Without
 * this slot wired, those events would be visible only in the
 * voice debug log and the session would appear to stall
 * silently. */
static void
voice_runtime_error_cb (void *user_data, const char *text)
{
    (void) user_data;
    if (!text) {
        return;
    }
    debug_log ("voice", "error: %s", text);
    toolbar_show_toast (text);
}

/* Signal handler: voice_runtime emitted SignalKind::SpeakerChanged
 * from the GStreamer `level` voice-activity evaluator (every ~200 ms
 * when a uid's RMS crosses the speaking threshold). Forwards the
 * flip into the canonical voice model; the user list view subscribes
 * to the model and repaints.
 *
 * Decoupled from the panel UI deliberately — the speaker indicator
 * lives in the user list column, not on the voice panel. The
 * runtime → model → users_view chain keeps the indicator policy
 * (NONE / IN_VOICE / SPEAKING / MUTED) in one place. */
static void
voice_runtime_speaker_changed_cb (void *user_data, uint16_t uid,
                                  int is_speaking)
{
    (void) user_data;
    session *sess = hx_active_session ();
    if (!sess->voice_model) {
        return;
    }
    hx_voice_model_set_speaking (sess->voice_model, uid,
                                 is_speaking ? TRUE : FALSE);
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
    if (!sess || !voice_panels)
        return;
    /* Refresh every registered panel bound to this session — covers
     * the public room's controls in the Users window plus every
     * pchat sidebar's controls, regardless of which surface hosts
     * them. */
    for (guint i = 0; i < voice_panels->len; i++) {
        GtkWidget *panel = g_ptr_array_index (voice_panels, i);
        if (g_object_get_data (G_OBJECT (panel), KEY_SESS) == sess) {
            voice_panel_refresh (panel, sess);
        }
    }
}
