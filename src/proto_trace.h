#ifndef HX_PROTO_TRACE_H
#define HX_PROTO_TRACE_H 1

/*
 * Hotline wire-protocol trace logging.
 *
 * Built on top of the categorised debug logger (debug.h). Activate
 * with:
 *
 *     GTKHX_DEBUG=proto ./gtkhx
 *
 * Output is written to stderr, one line per message header plus
 * one line per data chunk. Outgoing messages are prefixed "→",
 * incoming "←", and trans IDs let you correlate a request with its
 * task reply.
 *
 * Example output during a server-rejected news fetch:
 *
 *     [proto] → trans=12 type=HTLC_HDR_NEWS_GETFILE (0x000065) hc=0
 *     [proto] ← trans=12 type=HTLS_HDR_TASK (0x010000) flag=1 len=12
 *     [proto]   chunk type=HTLS_DATA_TASKERROR (0x0064) len=8 "Uh, no."
 *
 * The flag=1 on the reply is the task-error bit; matching it back
 * to the outgoing trans=12 tells you exactly which command was
 * rejected.
 */

#include <glib.h>

struct htlc_conn;

/* Type-name lookups. Return a literal string for known opcodes
 * ("HTLC_HDR_LOGIN", "HTLS_DATA_TASKERROR"), and a static "0x????"
 * fallback (held in a per-call static buffer; not thread-safe — only
 * call from the trace logger, which is single-threaded by virtue of
 * being on the main thread). */
extern const char *proto_hdr_name (guint32 type);
extern const char *proto_data_name (guint16 type);

/* Outgoing — called from hlwrite. proto_trace_send_chunk fires once
 * per data chunk between begin and end. begin/end are no-ops when
 * the "proto" debug category is disabled, so call sites don't need
 * an explicit gate. */
extern void proto_trace_send_begin (guint32 type, guint32 trans, guint16 hc);
extern void proto_trace_send_chunk (guint16 type, guint16 len,
                                    const guint8 *data);
extern void proto_trace_send_end (void);

/* Incoming — called from hx_rcv_hdr after the header is decoded but
 * before dispatch. Logs the header line. */
extern void proto_trace_recv_hdr (guint32 type, guint32 trans, guint32 flag,
                                  guint32 len);

/* Incoming — walks the data chunks already buffered in htlc->in
 * (between SIZEOF_HL_HDR and htlc->in.pos) and logs each one.
 * Independent of dh_start / dh_end so we don't have to thread a
 * trace call into every consumer of those macros. */
extern void proto_trace_recv_chunks (struct htlc_conn *htlc);

#endif /* HX_PROTO_TRACE_H */
