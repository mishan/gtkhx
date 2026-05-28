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
 * test_real_connect binary, which links production src/network.c
 * to exercise hx_connect's async state machine end-to-end against
 * the fake-server harness.
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

/* ---- TLS Phase 3 stubs --------------------------------------- */
/*
 * Production network.c::tls_accept_certificate calls into
 * src/tls_trust.c (pure GLib, gets linked in) and
 * src/tls_trust_dialog.c (Adwaita + nested GMainLoop — too heavy
 * for the test binary). The Tier 3 TLS tests set
 * GTKHX_TLS_AUTO_ACCEPT=1 so the dialog path is never taken;
 * the dialog symbol still has to resolve for the linker. Stub
 * with g_assert_not_reached so a future test that loses the env
 * var fails loudly instead of dead-locking on an invisible
 * dialog. Same trick for toolbar_window — never read on the
 * auto-accept path (the env-var check happens first). */
GtkWidget *toolbar_window = NULL;

/* src/tls_trust.c calls gtkhx_config_dir() to find
 * $CONFIG/known_hosts when the GTKHX_KNOWN_HOSTS env override
 * isn't set. The Tier 3 TLS test mains set the env override
 * unconditionally, so this stub is never consulted on the
 * happy path. Return NULL so a buggy test that loses the
 * env var fails the lookup loudly rather than touching the
 * developer's real $CONFIG. */
const char *
gtkhx_config_dir (void)
{
    return NULL;
}

#include "tls_trust.h"
gboolean
hx_tls_trust_dialog_run_sync (GtkWindow *parent G_GNUC_UNUSED,
                              const char *host G_GNUC_UNUSED,
                              guint16 port G_GNUC_UNUSED,
                              const char *fingerprint G_GNUC_UNUSED,
                              hx_tls_trust_status status G_GNUC_UNUSED)
{
    g_assert_not_reached ();
    return FALSE;
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

/* hx_rcv_hdr is the production receive callback — send_login
 * installs it on htlc->rcv after the LOGIN goes out. The test
 * never drives a receive (the fake server doesn't send LOGIN
 * replies), so this is a one-shot pointer the test never
 * dereferences. */
void
hx_rcv_hdr (struct htlc_conn *htlc)
{
    (void) htlc;
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
