#ifndef GTKHX_HXCONN_LAYOUT_H
#define GTKHX_HXCONN_LAYOUT_H 1

/*
 * hxconn_layout.h — the C mirror of struct htlc_conn.
 *
 * The connection struct is Rust-owned (gtkhx-core::conn):
 * production reaches it only through the opaque forward declaration in
 * protocol.h + the hx_conn_* accessors, and allocates it with hx_conn_new.
 *
 * The Tier-2/Tier-3 tests, however, stack-allocate a connection to drive the
 * accessors + parsers against. This header is the #[repr(C)]-matching C
 * definition they include for that. It is NOT part of the production contract
 * — production never sees the fields — but the layout must stay byte-identical
 * to Rust's HtlcConn, so the _Static_assert below (paired with the crate's
 * `assert!(size_of::<HtlcConn>() == HXCONN_SIZEOF)`) fails loudly on drift.
 *
 * Only test code (+ this header's own assert) should include it. Production
 * code includes protocol.h, which forward-declares struct htlc_conn opaquely.
 */

#include <glib.h>
#include "compat.h"  /* HOSTLEN */
#include "hotline.h" /* hl_access_bits */

/* Forward-declared so this header is self-contained (doesn't depend on
 * protocol.h / session.h having been included first for the `session` typedef).
 * The full type is `typedef struct _session session;` in session.h; gnu11
 * permits this redundant typedef of the same tag. */
typedef struct _session session;

struct htlc_conn {
    /* The session that owns this connection. Set once at allocation; read by
     * sess_from_htlc() to route a received event back to its session (replaces
     * the old container_of, which required htlc to be embedded in session).
     * The single-session world sets this to &the_session; the multi-conn seam
     * later sets it per connection. */
    session *sess;
    /* Server endpoint identification, populated at hx_connect time.
     * serverhost+serverport drive HTXF subchannel connects (rcv.c
     * stamps them onto each htxf_conn). ip_addr is the resolved
     * peer address as a printable string, used in connection-event
     * log lines (post-connect IP-then-status messages). */
    char serverhost[HOSTLEN];
    guint16 serverport;
    /* TLS mode for THIS connection (separate-port model — Mobius /
     * Janus). Set in hx_connect from the tls parameter (Phase 1).
     * HTXF subchannel connects in xfers.c / banner.c mirror this
     * flag so the data port runs over TLS too when the control
     * port does — the wire-level expectation on those servers is
     * that TLS-HTLS on port N pairs with TLS-HTXF on port N+1.
     * char (not gboolean) to match the hx_connect parameter type. */
    char tls;
    char ip_addr[HOSTLEN];
    int fd;
    guint32 trans;
    guint16 icon;
    guint16 uid;
    guint16 version;

    struct {
        guint32 visible : 1,
            /* set on the first HTLS_HDR_USER_SELFINFO. Used
         * by the agreement-window Agree button to decide whether
         * sending HTLC_HDR_AGREEMENTAGREE is appropriate.
         *
         * mhxd-style legacy flow: SELFINFO doesn't arrive until
         * AFTER AGREEMENTAGREE, so logged_in is still 0 when the
         * user clicks Agree → we send AGREEMENTAGREE.
         *
         * 1.9-style auto-accept flow (e.g. MacSecret.com): SELFINFO
         * arrives immediately after LOGIN — login is complete
         * before the agreement window opens. logged_in is 1 by
         * the time the user clicks Agree → we DO NOT send
         * AGREEMENTAGREE (some 1.9 servers disconnect when they
         * see one for an already-logged-in session).
         *
         * Reset to 0 in hx_htlc_close so reconnect starts fresh. */
            logged_in : 1,
            /* set when hx_post_login_fetches runs (either
         * via AGREEMENTAGREE-send for 1.5+ servers or the 1.0/1.2
         * fallback timer). This is the spec-correct "we're a fully
         * joined user" boundary — earlier than this, sending RPCs
         * like USER_GETLIST or FILE_LIST can land at the server
         * before our AGREEMENTAGREE and trip "action attributed
         * to not-yet-joined session" errors. The files browser's
         * remote provider gates on this flag so its initial
         * directory listing doesn't fire too early. Reset in
         * hx_htlc_close so reconnect starts fresh. */
            post_login_fetched : 1, reserved : 29;
    } flags;

    hl_access_bits access;
    /* Name/login on the wire are bytes, but the rest of GtkHx uses
     * them as C strings (passed to strcmp/strlen/strcpy throughout
     * rcv.c, users.c, network.c). Typing them as char* avoids a wave
     * of -Wpointer-sign warnings without changing storage layout —
     * char and guint8 are both 1 byte, just signed/unsigned. Same
     * reasoning applies to macalg / cipheralg / compressalg below
     * (those hold strings like "HMAC-SHA1", "BLOWFISH", "GZIP"). */
    char name[32];
    char login[32];

    /* Colored-Nicknames extension — our own 32-bit
     * 0x00RRGGBB nickname color. HX_NICK_COLOR_NONE means "no
     * color set"; in that case hx_change_name_icon omits the
     * HTLC_DATA_COLOR chunk entirely and the spec's auto-opt-in
     * doesn't fire (server keeps us in "no-color" mode and won't
     * decorate USER_CHANGE pushes with DATA_COLOR for us). Set
     * from gtkhx_prefs.nick_color at startup and on Settings
     * apply; sent on USER_CHANGE via hx_change_name_icon. */
    guint32 nick_color;

    /* HOPE cipher / compression names handed to the orchestrated connect
     * (hxnet owns the actual handshake, ciphers, and compression). Empty
     * strings select the orchestrator's defaults; set from the Connect dialog
     * and read at hx_connect time. */
    char cipheralg[32];
    char compressalg[32];
    /* Opaque HOPE control-channel AEAD material handle (Rust HxnetHopeAead*),
     * or NULL. Set after login when the orchestrated HOPE handshake negotiated
     * ChaCha20-Poly1305; lets an HTXF subchannel derive its per-transfer keys
     * in-process via hxnet_htxf_connect without the session key crossing back
     * to C. Freed with hxnet_hope_aead_free on connection teardown. */
    void *hope_aead;
    /* The legacy per-direction C cipher and compression state (session key,
     * the cipher and compress union members, their keys, type and keylen
     * fields, cipher_mode, the AEAD plaintext accumulator, and the gzip
     * counters) is gone: hxnet plus the hxcrypto and hxcompress crates own all
     * control-channel crypto and compression now, so none of it was ever
     * populated on htlc. */
    /* DATA_CAPABILITIES bitmask negotiated for this session, as
     * confirmed by the server in the LOGIN reply. Zero on legacy
     * servers (or on connections where neither side speaks the
     * extension). Tested with HTLC_CAP_TEXT_ENCODING and friends
     * — see hotline.h. */
    guint64 caps;
    /* Chat-history extension: server retention hints from the
     * LOGIN reply (DATA_HISTORY_MAX_MSGS / _DAYS, fogWraith
     * Capabilities-Chat-History.md). 0 means unlimited or
     * undisclosed — the spec says the authoritative signal for
     * "no more messages" is DATA_HISTORY_HAS_MORE = 0 in the
     * GET_CHAT_HISTORY reply, so these are UI hints only.
     * Populated only when the server echoes CAP_CHAT_HISTORY. */
    guint32 history_max_msgs;
    guint32 history_max_days;
    /* Inline-media extension: server-advertised advisory limits
     * from the LOGIN reply (DATA_CHAT_MEDIA_MAX_*, fogWraith
     * Capabilities-Inline-Media.md). Populated only when the
     * server echoes CAP_INLINE_MEDIA. Unset fields land as 0;
     * clients SHOULD treat 0 as "use spec recommended default"
     * (HX_MEDIA_DEFAULT_*). Pre-flight UI consults these before
     * round-tripping a known-bad upload. */
    guint32 media_max_bytes;
    guint32 media_max_dimension;
    guint32 media_max_pixels;
    guint32 media_chunk_size;
    guint32 media_max_frames;
    guint32 media_max_duration_ms;
    /* Chat-history extension Phase 4 (in-session reconnect
     * catch-up): newest message_id we've ever rendered for this
     * htlc — across all chats and all history batches received
     * during this gtkhx run. The post-login fetch in
     * hx_post_login_fetches uses this as an AFTER= cursor on
     * reconnect, so the server only sends entries that arrived
     * (or were stored) after our last view of the chat. Zero on
     * first-ever connect to a server, after which the first
     * batch's newest entry seeds it. Reset to zero when the user
     * connects to a DIFFERENT server (different host:port).
     *
     * KNOWN LIMITATION: this cursor only advances on entries
     * we receive through the chat-history extension (which carry
     * message_ids). Live TRAN_CHAT_MSG broadcasts don't carry a
     * message_id, so the cursor doesn't advance on them. Net
     * effect: reconnecting via AFTER=cursor will replay live
     * messages received during the previous session (between
     * the initial history fetch and disconnect). Acceptable
     * per the locked UX decisions — duplicates are preferable
     * to silent gaps. */
    guint64 chat_history_last_msgid;
    /* GIF-icons extension (fogWraith GIF-Icons.md). No capability
     * bit — support is discovered by probing ICON_GETLIST after login
     * and watching for a reply, since servers silently drop unknown
     * opcodes rather than returning a task error. gif_icons_state is
     * one of the GIF_ICONS_* values in gif_icons.h (UNKNOWN until the
     * probe resolves). gif_icons_probe_timer is the watchdog
     * g_timeout source id (0 when inactive). */
    int gif_icons_state;
    guint gif_icons_probe_timer;
    /* trans id of the post-login ICON_GETLIST probe, so the watchdog
     * can dismiss its Tasks-window row if no reply ever arrives (a
     * legacy server silently drops the unknown opcode, so the task
     * would otherwise linger forever). */
    guint32 gif_icons_probe_trans;
};

_Static_assert (sizeof (struct htlc_conn) == 760,
                "struct htlc_conn layout drifted from Rust HtlcConn "
                "(rust/crates/hxconn/src/lib.rs)");

#endif /* GTKHX_HXCONN_LAYOUT_H */
