/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/*
 * tests/integration/connect_test_stubs.c — minimal stubs for the
 * production-connect Tier 3 binaries (real_connect,
 * real_htxf_connect), which link production src/network.c to exercise
 * hx_connect / htxf_connect end-to-end against a real server.
 *
 * network.c references a handful of symbols that come from other
 * production translation units (gtkhx.c, chat.c, tasks.c, rcv.c,
 * gtkutil.c, banner.c). Linking those in would drag the GTK widget
 * tree, the xtext fork, the file table, the entire UI stack. This
 * file provides minimal stand-ins for each so the test binary can
 * link without that pile.
 *
 * The stubs are deliberately dumb:
 *
 *   - hxd_files[] is a static byte-allocated array sized for
 *     getdtablesize(), backing struct hxd_file pointers
 *     network.c::send_login writes into. The test doesn't read
 *     these back — it's purely write-only side state.
 *   - hxd_fd_set / hxd_fd_clr are no-ops. Production installs a
 *     GIOChannel watch; the test doesn't need byte routing past
 *     send_login because the assertions check the GtkhxSession
 *     connection-state signals, not the inbound rcv path.
 *   - the_session is a single zero-initialised session struct.
 *     network.c only touches a couple of fields (htlc, tasks)
 *     during the connect path; the rest stays untouched.
 *   - gtkhx_prefs is a zero-initialised prefs struct so the
 *     hx_htlc_close nick_color re-seed doesn't read uninitialised
 *     memory if a future test exercises the reconnect path.
 *   - hx_clear_chat, close_connected_windows, error_dialog,
 *     banner_clear, rcv_login_reset, hx_rcv_hdr, hx_post_login
 *     _fetches, task_new, task_update, task_delete — no-ops.
 *
 * The cumulative effect: the test exercises the REAL hx_connect
 * code path through TCP connect + magic write + magic read +
 * validation + send_login's preamble (including the LOGIN packet
 * build + hlwrite_chunks). What we lose is everything that
 * happens AFTER the LOGIN bytes hit the kernel buffer — there's
 * no real receive loop, no task dispatch, no chat rendering. For
 * the connect-state-machine test that's exactly the right
 * boundary.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>       /* FD_SETSIZE — see init_fd_table cap below */
#include <glib.h>
#include <gtk/gtk.h>          /* session.h drags this in */
#include "compat.h"
#include "hotline.h"            /* struct hl_hdr — Phase G replay recorder */
#include "protocol.h"
#include "session.h"
#include "prefs.h"
#include "tasks.h"
#include "rcv.h"
#include "banner.h"
#include "chat.h"
#include "gtkutil.h"
#include "users.h"

/* ---- hxd_files file table ----------------------------------- */

static struct hxd_file *the_hxd_files;
struct hxd_file *hxd_files = NULL;
int hxd_open_max = 0;

/* Called from the test's main() before hx_connect runs. Sizing
 * mirrors production gtkhx.c::main: query _SC_OPEN_MAX, then cap at
 * FD_SETSIZE. The cap matters here too — on Linux with a high
 * RLIMIT_NOFILE (containers, modern systemd) sysconf can return
 * 1 048 576, which would have us g_malloc0 ~80 MB of struct hxd_file
 * for a test binary that never opens more than a handful of fds. */
void connect_test_init_fd_table (void);
void
connect_test_init_fd_table (void)
{
    if (hxd_files) {
        return;
    }
    hxd_open_max = (int) sysconf (_SC_OPEN_MAX);
    if (hxd_open_max <= 0) {
        hxd_open_max = 1024;
    }
    if (hxd_open_max > FD_SETSIZE) {
        hxd_open_max = FD_SETSIZE;
    }
    the_hxd_files = g_new0 (struct hxd_file, hxd_open_max);
    hxd_files = the_hxd_files;
}

void hxd_fd_set (int fd, int rw) { (void) fd; (void) rw; }
void hxd_fd_clr (int fd, int rw) { (void) fd; (void) rw; }

/* ---- the_session + gtkhx_prefs globals ---------------------- */

session the_session;
struct gtkhx_prefs gtkhx_prefs;
char last_msg_nick[32];
char *g_user_colors[4];
int hxd_open_max_placeholder; /* never read; just defined to make sure */

/* ---- INFOPREFIX ---------------------------------------------- */

const char *INFOPREFIX = "[hx] ";

/* ---- TLS TOFU stubs ------------------------------------------ */
/*
 * The TLS trust brain (classify / decide / pin / seams) is the Rust
 * hxtls-trust crate, linked into these test binaries via
 * rust_hxtls_trust_dep. The Adwaita prompt lives in gtkhx-ui, which
 * we deliberately DON'T link (too heavy, and it drags a huge C-symbol
 * surface). The trust brain only reaches the prompt through a
 * callback registered at UI init (gtkhx_tls_prompt_install), which
 * these headless tests never call — so with no prompt registered the
 * decide path rejects. The Tier 3 TLS tests set a seam
 * (GTKHX_TLS_AUTO_ACCEPT / a prompt verdict) up front, so the prompt
 * is never needed anyway. toolbar_window is stubbed because other
 * production sources linked here reference it. */
GtkWidget *toolbar_window = NULL;

/* hxtls-trust's known_hosts path resolver calls gtkhx_config_dir() to
 * find $CONFIG/known_hosts when the GTKHX_KNOWN_HOSTS env override /
 * hx_tls_test_set_known_hosts seam isn't set. The Tier 3 TLS test
 * mains set the override unconditionally, so this stub is never
 * consulted on the happy path. Return NULL so a buggy test that loses
 * the override fails the lookup loudly rather than touching the
 * developer's real $CONFIG.
 *
 * Forward-declared inline (rather than via #include "gtkhx.h", which
 * would drag in the GTK / Adwaita header pile) so -Wmissing-prototypes
 * stays happy. */
extern const char *gtkhx_config_dir (void);
const char *
gtkhx_config_dir (void)
{
    return NULL;
}

/* ---- Stubs that network.c calls during hx_connect / close --- */

void
hx_clear_chat (struct htlc_conn *htlc, guint32 cid, int subj)
{
    (void) htlc; (void) cid; (void) subj;
}

void
close_connected_windows (session *sess)
{
    (void) sess;
}

void
error_dialog (char *title, char *msg)
{
    (void) title; (void) msg;
}

void
banner_clear (void)
{
}

void
rcv_login_reset (void)
{
}

/* hx_rcv_hdr is the production receive callback. In the Phase G
 * orchestrator path, hx_bridge_dispatch_frame stages the replayed
 * LOGIN-reply header into htlc->in.buf and calls the body-handler
 * dispatch (== this stub) — so we record the dispatched frame's header
 * fields here. The
 * real
 * production hx_rcv_hdr lives in rcv.c and drags the whole UI
 * stack; the orchestrator test only needs to prove the reply was
 * replayed to the C dispatch with the pinned trans / TASK opcode /
 * success flag, which the header alone carries. */
/* "first_*" captures the FIRST dispatched frame, "last_*" the most
 * recent. The Phase G test asserts on first_*: the orchestrator
 * replays the LOGIN reply as a synthetic frame before HandshakeDone,
 * so it's guaranteed to be the first frame dispatched. After
 * HandshakeDone the actor starts reading real server pushes (mhxd
 * sends SELFINFO / user-list / etc.), which also dispatch through
 * here — so "last" would be one of those, not the login reply. */
guint32 connect_test_first_rcv_type = 0;
guint32 connect_test_first_rcv_trans = 0;
guint32 connect_test_first_rcv_flag = 0;
guint32 connect_test_last_rcv_type = 0;
guint32 connect_test_last_rcv_trans = 0;
guint32 connect_test_last_rcv_flag = 0;
guint connect_test_rcv_count = 0;
/* Capabilities echo from the FIRST dispatched frame's body (the
 * replayed LOGIN reply). A capability-aware server (Janus) echoes
 * the HTLC_DATA_CAPABILITIES bits we advertised back in the LOGIN
 * reply; a cap-unaware server (mhxd) omits the chunk per spec. Lets
 * the Phase G Tier 3 test prove the orchestrator advertised caps
 * end-to-end against a real cap-aware server. */
gboolean connect_test_first_rcv_caps_present = FALSE;
guint16  connect_test_first_rcv_caps_value = 0;

void connect_test_reset_rcv_record (void);
void
connect_test_reset_rcv_record (void)
{
    connect_test_first_rcv_type = 0;
    connect_test_first_rcv_trans = 0;
    connect_test_first_rcv_flag = 0;
    connect_test_last_rcv_type = 0;
    connect_test_last_rcv_trans = 0;
    connect_test_last_rcv_flag = 0;
    connect_test_rcv_count = 0;
    connect_test_first_rcv_caps_present = FALSE;
    connect_test_first_rcv_caps_value = 0;
}

/* HTLS_DATA_CAPABILITIES wire tag (mirror of HTLC_DATA_CAPABILITIES
 * 0x01f0 — same tag is reused server→client for the echo). */
#define CONNECT_TEST_TAG_CAPABILITIES 0x01f0

/* Body handler the recording hx_rcv_hdr installs for the FIRST
 * dispatched frame. By the time hx_bridge_dispatch_frame calls this,
 * the full frame (22-byte header + body) is staged in htlc->in.buf;
 * walk the chunk list for the capabilities echo. */
static void
connect_test_rcv_body (struct htlc_conn *htlc)
{
    guint16 hc_be;
    guint32 len_be;
    memcpy (&hc_be, htlc->in.buf + 20, 2);    /* hl_hdr.hc  @ offset 20 */
    memcpy (&len_be, htlc->in.buf + 12, 4);   /* hl_hdr.len @ offset 12 */
    guint16 hc = GUINT16_FROM_BE (hc_be);
    guint32 wire_len = GUINT32_FROM_BE (len_be);
    gsize body_len = wire_len >= 2 ? (gsize) (wire_len - 2) : 0;
    gsize off = SIZEOF_HL_HDR;
    gsize end = SIZEOF_HL_HDR + body_len;

    for (guint16 i = 0; i < hc && off + 4 <= end; i++) {
        guint16 tag_be, dlen_be;
        memcpy (&tag_be, htlc->in.buf + off, 2);
        memcpy (&dlen_be, htlc->in.buf + off + 2, 2);
        guint16 tag = GUINT16_FROM_BE (tag_be);
        guint16 dlen = GUINT16_FROM_BE (dlen_be);
        off += 4;
        if (off + dlen > end) {
            break;
        }
        if (tag == CONNECT_TEST_TAG_CAPABILITIES) {
            connect_test_first_rcv_caps_present = TRUE;
            /* HTLS_DATA_CAPABILITIES is a variable-width big-endian
             * integer (1..8 bytes on the wire); a server may echo our
             * advertised bits in as few bytes as fit (e.g. 0x1F in a
             * single byte). Accumulate all dlen bytes big-endian
             * rather than assuming a fixed 2-byte width — the old
             * `dlen >= 2` path left the value at 0 for a 1-byte echo
             * and made the negotiation assertion spuriously fail. */
            guint64 caps = 0;
            for (guint16 b = 0; b < dlen; b++) {
                caps = (caps << 8) | htlc->in.buf[off + b];
            }
            connect_test_first_rcv_caps_value = (guint16) caps;
        }
        off += dlen;
    }
}

void
hx_dispatch_frame (struct htlc_conn *htlc, guint32 type, guint32 trans,
                   guint32 flag, guint32 body_len)
{
    (void) body_len;
    gboolean is_first = (connect_test_rcv_count == 0);
    if (is_first) {
        connect_test_first_rcv_type = type;
        connect_test_first_rcv_trans = trans;
        connect_test_first_rcv_flag = flag;
    }
    connect_test_last_rcv_type = type;
    connect_test_last_rcv_trans = trans;
    connect_test_last_rcv_flag = flag;
    connect_test_rcv_count++;

    /* hx_bridge_dispatch_frame has already staged the full frame (22-byte
     * header + body) into htlc->in.buf, so — unlike the old two-phase
     * hx_rcv_hdr handoff — there's no qbuf/two-phase dance to mirror here.
     * Inspect the first frame (the replayed LOGIN reply) directly for the
     * capabilities echo. */
    if (is_first && htlc && htlc->in.buf) {
        connect_test_rcv_body (htlc);
    }
}

void
hx_post_login_fetches (struct htlc_conn *htlc)
{
    (void) htlc;
}

/* tasks.c stubs — production task_new allocates a struct task,
 * registers it in the_session.tasks hashtable, fires a
 * GtkhxSession::task-update signal. The test doesn't read tasks
 * back; a NULL return is fine and matches what network.c::
 * send_login does with the result (it doesn't use it). */
struct task;
struct task *
task_new (struct htlc_conn *htlc, rcv_task_fn rcv, void *ptr, void *data,
          const char *str)
{
    (void) htlc; (void) rcv; (void) ptr; (void) data; (void) str;
    return NULL;
}

void task_update (session *sess, struct task *tsk) { (void) sess; (void) tsk; }
void task_delete (session *sess, struct task *tsk) { (void) sess; (void) tsk; }

/* network.c::update_task looks up a task by trans on incoming
 * HTLS_HDR_TASK frames. The real test path doesn't trigger any
 * task responses (the fake server never replies after LOGIN), but
 * the symbol still needs to resolve. */
struct task *
task_with_trans (session *sess, guint32 trans)
{
    (void) sess; (void) trans;
    return NULL;
}

void
gtask_delete_tsk (session *sess, guint32 trans)
{
    (void) sess; (void) trans;
}

/* tracker.c progress hooks — referenced by tracker fetch state
 * machine in network.c. The connect path doesn't fire the tracker
 * fetch, but the symbols need to link. */
void
track_prog_update (session *sess, char *str, int num, int total)
{
    (void) sess; (void) str; (void) num; (void) total;
}

void
trackconn_prog_update (session *sess, char *str, int num, int total)
{
    (void) sess; (void) str; (void) num; (void) total;
}

/* users.c — referenced by hx_htlc_close. The test path with
 * fd=0 avoids it, but the symbol must resolve. */
void
hx_change_name_icon (struct htlc_conn *htlc)
{
    (void) htlc;
}

/* rcv_task_login is referenced by network.c via the RCV_TASK_FN
 * macro inside task_new()'s argument list — task_new takes the
 * function pointer but doesn't call it (the test's task_new is a
 * no-op anyway). Provide a definition so the symbol resolves. */
void
rcv_task_login (struct htlc_conn *htlc, char *pass)
{
    (void) htlc; (void) pass;
}

/* Phase 8.D runtime-wire stub. network.c::hx_htlc_close calls
 * gtkhx_voice_runtime_free on disconnect; the integration test
 * binaries don't link hxvoice-runtime, so provide a no-op shim.
 * the_session.voice_runtime stays NULL in the test path anyway. */
typedef struct gtkhx_voice_runtime gtkhx_voice_runtime;
extern void gtkhx_voice_runtime_free (gtkhx_voice_runtime *rt);
void
gtkhx_voice_runtime_free (gtkhx_voice_runtime *rt)
{
    (void) rt;
}

/* Speaker-indicator model stub. Same rationale as the runtime stub
 * above: network.c::hx_htlc_close calls hx_voice_model_clear on
 * disconnect to drop stale presence state. the_session.voice_model
 * stays NULL in the test path, so the production null-check above
 * skips the call — but the link still needs the symbol resolved. */
typedef struct _HxVoiceModel HxVoiceModel;
extern void hx_voice_model_clear (HxVoiceModel *self);
void
hx_voice_model_clear (HxVoiceModel *self)
{
    (void) self;
}
