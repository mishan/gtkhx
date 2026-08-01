/*
 * hxconn.h — field accessors for struct htlc_conn.
 *
 * The connection struct is Rust-owned (docs/rust/network-endgame.md; the
 * flip). Its storage and every accessor body live in the Rust `gtkhx-core::conn` module
 * (rust/crates/gtkhx-core/src/conn.rs); this header is the C ABI those accessors export. C
 * reaches every field through the getters/setters below — the migration onto
 * this seam ran group-by-group before the flip, so the call sites didn't change
 * when the bodies moved to Rust.
 *
 * `struct htlc_conn` is deliberately only forward-declared here: a consumer that
 * includes hxconn.h (and not protocol.h) sees the accessors but NOT the fields,
 * which is the end-state contract. Production allocates a connection with
 * hx_conn_new (a Rust Box, never freed — it lives for the process); the Tier-2/
 * Tier-3 tests stack-allocate the pinned C mirror in hxconn_layout.h. The
 * #[repr(C)] HtlcConn and the _Static_assert in hxconn_layout.h keep the two in
 * lockstep.
 */

#ifndef GTKHX_HXCONN_H
#define GTKHX_HXCONN_H

#include <glib.h>

struct htlc_conn;
typedef struct _session session;

/* ---- Lifecycle -----------------------------------------------------------
 *
 * hx_conn_new allocates a fresh, zeroed connection (the Rust owner Box-boxes
 * it); hx_conn_reset returns an existing one to the just-allocated state (the
 * reconnect path); hx_conn_free releases a hx_conn_new allocation. Production
 * keeps exactly one connection for the process lifetime and never frees it. */
extern struct htlc_conn *hx_conn_new (void);
extern void hx_conn_reset (struct htlc_conn *h);
extern void hx_conn_free (struct htlc_conn *h);

/* ---- Chat-history extension session state ---------------------------------
 *
 * history_max_msgs / history_max_days: server retention hints from the LOGIN
 * reply (0 = unlimited / undisclosed). chat_history_last_msgid: the newest
 * message_id rendered this run, used as the AFTER= reconnect cursor. All three
 * are wiped on disconnect. */
extern guint32 hx_conn_history_max_msgs (const struct htlc_conn *h);
extern void hx_conn_set_history_max_msgs (struct htlc_conn *h, guint32 v);
extern guint32 hx_conn_history_max_days (const struct htlc_conn *h);
extern void hx_conn_set_history_max_days (struct htlc_conn *h, guint32 v);
extern guint64 hx_conn_chat_history_last_msgid (const struct htlc_conn *h);
extern void hx_conn_set_chat_history_last_msgid (struct htlc_conn *h,
                                                 guint64 v);

/* Inline-media advisory limits — server hints from the LOGIN reply
 * (0 = "use the client default"). Set as a group by rcv_task_login,
 * reset as a group at connect, and read individually by the
 * inline_media.h ceiling helpers. Survive reconnect intentionally
 * (see the inline_media.h caps/limits note) until explicitly reset. */
extern guint32 hx_conn_media_max_bytes (const struct htlc_conn *h);
extern void hx_conn_set_media_max_bytes (struct htlc_conn *h, guint32 v);
extern guint32 hx_conn_media_max_dimension (const struct htlc_conn *h);
extern void hx_conn_set_media_max_dimension (struct htlc_conn *h, guint32 v);
extern guint32 hx_conn_media_max_pixels (const struct htlc_conn *h);
extern void hx_conn_set_media_max_pixels (struct htlc_conn *h, guint32 v);
extern guint32 hx_conn_media_chunk_size (const struct htlc_conn *h);
extern void hx_conn_set_media_chunk_size (struct htlc_conn *h, guint32 v);
extern guint32 hx_conn_media_max_frames (const struct htlc_conn *h);
extern void hx_conn_set_media_max_frames (struct htlc_conn *h, guint32 v);
extern guint32 hx_conn_media_max_duration_ms (const struct htlc_conn *h);
extern void hx_conn_set_media_max_duration_ms (struct htlc_conn *h, guint32 v);
/* Reset all six advisory limits to 0 ("use client defaults"). */
extern void hx_conn_reset_media_limits (struct htlc_conn *h);

/* ---- Server endpoint identity --------------------------------------------
 *
 * Populated at hx_connect time and read by the HTXF-subchannel setup, the TLS
 * TOFU verify, and the connection-event log lines. serverhost / ip_addr are
 * NUL-terminated strings; the getters return the internal buffer (valid for
 * the connection's lifetime) and the setters copy into it (truncating to the
 * field's capacity). `tls` is the per-connection separate-port TLS flag (0/1).
 * ip_addr is the resolved printable peer address; hx_conn_set_ip_addr with ""
 * clears it. */
extern const char *hx_conn_serverhost (const struct htlc_conn *h);
extern void hx_conn_set_serverhost (struct htlc_conn *h, const char *v);
extern guint16 hx_conn_serverport (const struct htlc_conn *h);
extern void hx_conn_set_serverport (struct htlc_conn *h, guint16 v);
extern const char *hx_conn_ip_addr (const struct htlc_conn *h);
extern void hx_conn_set_ip_addr (struct htlc_conn *h, const char *v);
extern char hx_conn_tls (const struct htlc_conn *h);
extern void hx_conn_set_tls (struct htlc_conn *h, char v);

/* ---- Negotiated protocol identity ----------------------------------------
 *
 * version: the server's HTLS_DATA_VERSION from the LOGIN reply (0 on 1.0/1.2
 * servers that don't advertise one — the gate for PING, the 1.5 news button,
 * etc.). caps: the DATA_CAPABILITIES bitmask the server confirmed for this
 * session (0 on legacy servers). Both are wiped on disconnect. hx_conn_has_cap
 * is the predicate every `htlc->caps & HTLC_CAP_*` bit-test now goes through;
 * hx_conn_caps exposes the raw bitmask for the rare whole-value consumer. */
extern guint16 hx_conn_version (const struct htlc_conn *h);
extern void hx_conn_set_version (struct htlc_conn *h, guint16 v);
extern guint64 hx_conn_caps (const struct htlc_conn *h);
extern void hx_conn_set_caps (struct htlc_conn *h, guint64 v);
extern gboolean hx_conn_has_cap (const struct htlc_conn *h, guint64 cap);

/* ---- Our own user identity -----------------------------------------------
 *
 * uid: our session user-id (assigned by the server). icon: our chat-list icon
 * id. nick_color: the Colored-Nicknames RGB (0x00RRGGBB, or
 * HX_NICK_COLOR_NONE). Seeded from prefs at connect and updated from SELFINFO /
 * USER_CHANGE echoes. (The old status-colour field `color` was write-only and
 * has been removed.)
 *
 * Both are plain value accessors. The raw-address escape hatches that used to
 * sit beside them existed for one consumer — the settings binder that aliased
 * the nickname and icon preferences onto this struct's storage — and went with
 * it. Identity is copied into the connection now, never aliased. */
extern guint16 hx_conn_uid (const struct htlc_conn *h);
extern void hx_conn_set_uid (struct htlc_conn *h, guint16 v);
extern guint16 hx_conn_icon (const struct htlc_conn *h);
extern void hx_conn_set_icon (struct htlc_conn *h, guint16 v);
extern guint32 hx_conn_nick_color (const struct htlc_conn *h);
extern void hx_conn_set_nick_color (struct htlc_conn *h, guint32 v);

/* name: our display nick (NUL-terminated, <= 31 chars). login: our account
 * login (set at connect, not read back in production — kept as a field only so
 * the SELFINFO no-overflow test can sentinel the buffer adjacent to name).
 * Read via hx_conn_name, write via hx_conn_set_name (which truncates to the
 * field capacity). */
extern const char *hx_conn_name (const struct htlc_conn *h);
extern void hx_conn_set_name (struct htlc_conn *h, const char *v);
extern void hx_conn_set_login (struct htlc_conn *h, const char *v);

/* ---- Login lifecycle flags -----------------------------------------------
 *
 * logged_in: set on the first SELFINFO; the agreement Agree button reads it to
 * decide whether AGREEMENTAGREE is appropriate. post_login_fetched: the
 * spec-correct "we're a fully-joined user" boundary (USER_GETLIST / news / file
 * listing are safe only after it). Both are 1-bit flags, cleared on disconnect. */
extern gboolean hx_conn_logged_in (const struct htlc_conn *h);
extern void hx_conn_set_logged_in (struct htlc_conn *h, gboolean v);
extern gboolean hx_conn_post_login_fetched (const struct htlc_conn *h);
extern void hx_conn_set_post_login_fetched (struct htlc_conn *h, gboolean v);

/* ---- GIF-icons capability probe ------------------------------------------
 *
 * gif_icons_state is the GIF_ICONS_* tri-state (UNKNOWN / SUPPORTED /
 * UNSUPPORTED) tracking whether the server accepts the animated-icon opcodes
 * rather than task-erroring. gif_icons_probe_timer is the watchdog source id
 * that resolves the probe if no reply arrives (0 = disarmed). Both reset on
 * connect. The state getter/setter keep the int wire vocabulary of the
 * GIF_ICONS_* enum; the timer setter takes 0 to record "disarmed" after the
 * caller has removed the source. */
extern int hx_conn_gif_icons_state (const struct htlc_conn *h);
extern void hx_conn_set_gif_icons_state (struct htlc_conn *h, int v);
extern guint hx_conn_gif_icons_probe_timer (const struct htlc_conn *h);
extern void hx_conn_set_gif_icons_probe_timer (struct htlc_conn *h, guint v);
/* gif_icons_probe_trans: the trans of the probe's task, stashed so the
 * watchdog can dismiss the orphaned Tasks-window row when a legacy server
 * silently drops ICON_GETLIST. */
extern guint32 hx_conn_gif_icons_probe_trans (const struct htlc_conn *h);
extern void hx_conn_set_gif_icons_probe_trans (struct htlc_conn *h, guint32 v);

/* ---- Account access bitmap -----------------------------------------------
 *
 * The 8-byte access bitmap the server sends in SELFINFO. Every UI gate reads
 * it through one of these two predicates — the byte-pointer-into-the-struct
 * form the callers used before is exactly what these hide. hx_conn_access_has
 * is the strict bit test; hx_conn_access_permits additionally treats an
 * all-zero bitmap as "permitted", the rule the legacy-server news gate needs.
 * hx_conn_set_access copies the 8 raw bitmap bytes in (the SELFINFO parse's
 * sole writer). */
extern gboolean hx_conn_access_has (const struct htlc_conn *h, int bit);
extern gboolean hx_conn_access_permits (const struct htlc_conn *h, int bit);
extern void hx_conn_set_access (struct htlc_conn *h, const guint8 *bytes);

/* ---- Control-channel socket descriptor -----------------------------------
 *
 * The main control-channel fd, doubling as the connection-liveness flag: 0
 * means "no socket" (disconnected), and every `if (hx_conn_fd (h))` gate reads
 * it that way. A non-zero value is either a live descriptor or the -1 sentinel
 * network.c parks it at during teardown so the close-time guards still fire —
 * so the truthiness (!= 0), not fd > 0, is the connected test. The raw value
 * is also formatted into the disconnect-diagnostics log lines. The writers all
 * live in network.c's connect/close paths. */
extern int hx_conn_fd (const struct htlc_conn *h);
extern void hx_conn_set_fd (struct htlc_conn *h, int v);

/* ---- Owning-session back-pointer -----------------------------------------
 *
 * The session that owns this connection, set once at allocation. The read side
 * already has a chokepoint accessor — sess_from_htlc() in session.h — so only
 * the write needs a seam here; the single-session world sets it to
 * &the_session, the multi-conn seam later sets it per connection. */
extern session *hx_conn_sess (const struct htlc_conn *h);
extern void hx_conn_set_sess (struct htlc_conn *h, session *s);

/* ---- Connect-time HOPE algorithm selections ------------------------------
 *
 * The cipher / compression names handed to the orchestrated connect (hxnet
 * owns the actual handshake). Empty selects the orchestrator's default. Set
 * from the Connect dialog, read at hx_connect time. hx_conn_set_cipheralg /
 * _compressalg copy into the fixed buffer (truncating), and clear it when
 * passed NULL or "". */
extern const char *hx_conn_cipheralg (const struct htlc_conn *h);
extern void hx_conn_set_cipheralg (struct htlc_conn *h, const char *v);
extern const char *hx_conn_compressalg (const struct htlc_conn *h);
extern void hx_conn_set_compressalg (struct htlc_conn *h, const char *v);

/* ---- HOPE control-channel AEAD material handle ---------------------------
 *
 * Opaque Rust HxnetHopeAead* (or NULL). Seeded after login when the
 * orchestrated HOPE handshake negotiated ChaCha20-Poly1305, so an HTXF
 * subchannel can derive its per-transfer keys in-process without the session
 * key crossing back to C. The caller owns the lifecycle (clone / free via the
 * hxnet_hope_aead_* API); this seam only stores and returns the pointer. */
extern void *hx_conn_hope_aead (const struct htlc_conn *h);
extern void hx_conn_set_hope_aead (struct htlc_conn *h, void *p);

/* ---- Outgoing transaction counter ----------------------------------------
 *
 * The monotonically-increasing trans id stamped on each outgoing request. A
 * task is keyed on the value current at send time (task_new snapshots it
 * before hlpack_chunks bumps it), so the packer reads-then-increments via
 * hx_conn_trans_post_inc (returns the pre-increment value). hx_conn_trans /
 * hx_conn_set_trans cover the login-replay save/restore in network.c and the
 * trace/task snapshots that just read it. */
extern guint32 hx_conn_trans (const struct htlc_conn *h);
extern void hx_conn_set_trans (struct htlc_conn *h, guint32 v);
extern guint32 hx_conn_trans_post_inc (struct htlc_conn *h);

#endif /* GTKHX_HXCONN_H */
