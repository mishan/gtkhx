/*
 * protocol.h — Hotline wire/protocol/network/task types.
 *
 * Pulls in <glib.h> for guint*_t and hotline.h for the on-the-wire
 * structs, but deliberately avoids <gtk/gtk.h>. Crypto and protocol
 * code (hmac.c, rand.c, cipher.c, network.c, ...) can include this
 * without dragging in widget definitions.
 *
 * If you're tempted to add a GtkWidget* field here, it belongs in
 * session.h instead.
 */

#ifndef __gtkhx_PROTOCOL_H
#define __gtkhx_PROTOCOL_H 1

#include <glib.h>
#include <sys/types.h> /* u_int8_t / u_int16_t / u_int32_t */
#include <sys/time.h>
#include <time.h>
#include <pthread.h>

#include "compat.h"
#include "hotline.h"

#ifdef CONFIG_COMPRESS
#include "compress.h"
#endif

#ifdef CONFIG_CIPHER
#include "cipher.h"
#endif

/* Phase 5+ (HTXF rewrite): the connection and transfer structs used
 * to carry addrinfo / sockaddr_in for the network stack. The new
 * GSocketClient-based connect path stores a plain host + port instead,
 * so neither header is needed here anymore. */

/* ---- Buffered byte queues ------------------------------------------- */

struct qbuf {
    guint32 pos, len;
    guint8 *buf;
};

extern void qbuf_set (struct qbuf *q, guint32 pos, guint32 len);
extern void qbuf_add (struct qbuf *q, void *buf, guint32 len);

/* ---- Connections ---------------------------------------------------- */

struct htlc_conn;

struct htxf_conn {
    /* All size / position fields are guint64 to support the
	 * Large-File extension (CAP_LARGE_FILES). The protocol still
	 * defaults to 32-bit on the wire; the upgrade only matters
	 * when both peers negotiate the capability. Internally we
	 * always operate in 64-bit so progress reporting, resume,
	 * and rename-on-collision logic do not have to branch on
	 * the negotiated mode. */
    guint64 data_size, data_pos, rsrc_size, rsrc_pos;
    guint64 total_size, total_pos;
    /* Server's data-fork size from the file listing, captured at
	 * xfer_new time. Used by xfer_go to choose between resume
	 * (local exists and is strictly smaller than server) and
	 * rename-on-collision (local is the same size or larger, or
	 * server size is unknown). 0 == unknown — listing wasn't
	 * available for this transfer. */
    guint64 srv_data_size;
    /* Lifecycle: htxf_conn is reference-counted to handle the
	 * cross-thread ownership knot between the xfers[] array, the
	 * per-xfer worker thread, and any pending main-thread idles
	 * the worker has queued (post_file_update / post_xfer_cleanup).
	 *
	 * Owners that increment refcount on acquire and decrement on
	 * release:
	 *   1  the xfers[] array (xfer_new → xfer_remove_from_list)
	 *   1  the worker thread (xfer_ready_write → cleanup_dispatch)
	 *   N  each pending post_* idle (post_*  → its dispatcher)
	 *
	 * xfer_delete (called from rcv.c when the server cancels, and
	 * from xfer_ready_write's err_fd path) sets `canceled` and
	 * removes the htxf from xfers[] (which drops the xfers[] ref).
	 * If the worker and queued idles still hold refs, the htxf
	 * stays alive, the worker exits cleanly, idles run with the
	 * canceled flag set and skip their work, and the last unref
	 * frees. No use-after-free; no race window.
	 *
	 * Mutate refcount only via g_atomic_int_*. Mutate `canceled`
	 * only on the main thread (so the worker reads a coherent
	 * value via volatile-equivalent access — gint reads are
	 * atomic on every architecture we run on). */
    gint refcount;
    gboolean canceled;
    guint32 ref; /* xfer id */
    guint8 gone;
    guint8 type;
    guint32 queue; /* position in server queue */
    int fd;
    pthread_t tid;

    /* HTXF subchannel target: same hostname as the main control
	 * channel, port + 1. Stored as plain strings so the worker
	 * thread can hand them straight to GSocketClient without any
	 * addrinfo dance. */
    char serverhost[HOSTLEN];
    guint16 serverport;
    struct htlc_conn *htlc;
    char path[MAXPATHLEN];
    char remotepath[MAXPATHLEN]; /* dir + name joined; display only */
    /* Structured remote location. The wire protocol identifies a
	 * file by a separate per-component DIR list plus a flat NAME
	 * chunk — names can contain any byte including `/`, which is
	 * otherwise dir_char. Storing the dir and name apart from the
	 * joined remotepath is what lets xfer_go correctly request
	 * files like "Cheeseman goes 56k/sec.pct" without the `/` in
	 * the name getting reinterpreted as a directory boundary. */
    char remotedir[MAXPATHLEN];
    char remotename[256];
    guint16 remotename_len;
    struct qbuf in;
    char **filter_argv;
    struct timeval start;

    struct {
        guint32 retry : 1, preview : 1,
            /* When set, the worker uses folder_{get,put}_thread
		 * instead of {get,put}_thread, and the wire format is
		 * the HTXF_TYPE_FOLDER stream (FILE_NEXT/FILE_SEND/
		 * FILE_RESUME commands, per-file size headers, nested
		 * file_send_one/file_recv_one calls per leaf). Set on
		 * htxf right after xfer_new for folder transfers. */
            folder : 1,
            /* When set, this transfer uses the Large-File extension
		 * wire shape: HTXF_FLAG_LARGE_FILE in the handshake; FFO
		 * fork headers use the high/low 32-bit split encoding on
		 * the wire (the Compression field at offset 4-7 carries
		 * the high 32 bits of the fork length, DataSize at 12-15
		 * carries the low 32 bits); large-file uploads (>4 GiB)
		 * send raw data only, no FFO wrapper.
		 * Decided once by htxf_connect based on CAP_LARGE_FILES
		 * plus the actual transfer size. */
            large : 1, reserved : 28;
    } opt;

    /* Phase 5: when opt.preview is set, the preview window is created
	 * on the main thread (in rcv_task_file_get) and stashed here so
	 * the download worker thread doesn't have to construct GTK widgets
	 * itself. The worker only feeds bytes through preview->output()
	 * (which g_idle_add's them onto the main thread's queue). NULL
	 * for non-preview transfers. */
    void *preview;
};

struct htlc_conn {
    struct htlc_conn *next, *prev;
    void (*rcv) (struct htlc_conn *);
    void (*real_rcv) (struct htlc_conn *);
    struct qbuf in, out;
    struct qbuf read_in;
    /* Server endpoint identification, populated at hx_connect time.
	 * serverhost+serverport drive HTXF subchannel connects (rcv.c
	 * stamps them onto each htxf_conn). ip_addr is the resolved
	 * peer address as a printable string, used in connection-event
	 * log lines (post-connect IP-then-status messages). */
    char serverhost[HOSTLEN];
    guint16 serverport;
    char ip_addr[HOSTLEN];
    int fd;
    guint32 trans;
    guint32 chattrans;
    guint16 icon;
    guint16 uid;
    guint16 version;

    struct {
        guint32 visible : 1,
            /* Phase 5: set on the first HTLS_HDR_USER_SELFINFO. Used
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
            logged_in : 1, reserved : 30;
    } flags;

    hl_access_bits access;
    /* Name/login on the wire are bytes, but the rest of GtkHx uses
	 * them as C strings (passed to strcmp/strlen/strcpy throughout
	 * rcv.c, users.c, network.c). Typing them as char* avoids a wave
	 * of -Wpointer-sign warnings without changing storage layout —
	 * char and guint8 are both 1 byte, just signed/unsigned. Same
	 * reasoning applies to macalg / cipheralg / compressalg below
	 * (those hold strings like "HMAC-SHA1", "RC4", "GZIP"). */
    char name[32];
    char login[32];

    unsigned int gdk_input : 1;

    guint16 color;

    char macalg[32];
    u_int8_t sessionkey[64];
    u_int16_t sklen;

#if defined(CONFIG_CIPHER)
    char cipheralg[32];
    union cipher_state cipher_encode_state;
    union cipher_state cipher_decode_state;
    u_int8_t cipher_encode_key[32];
    u_int8_t cipher_decode_key[32];
    /* keylen in bytes */
    u_int8_t cipher_encode_keylen, cipher_decode_keylen;
    u_int8_t cipher_encode_type, cipher_decode_type;
#if defined(CONFIG_COMPRESS)
    u_int8_t zc_hdrlen;
    u_int8_t zc_ran;
#endif
#endif
#if defined(CONFIG_COMPRESS)
    char compressalg[32];
    union compress_state compress_encode_state;
    union compress_state compress_decode_state;
    u_int16_t compress_encode_type, compress_decode_type;
    unsigned long gzip_inflate_total_in, gzip_inflate_total_out;
    unsigned long gzip_deflate_total_in, gzip_deflate_total_out;
#endif
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
};

/* Phase 5: LOCK_HTXF / UNLOCK_HTXF / INITLOCK_HTXF used to serialize
 * cross-thread access to the global xfers[] array between worker
 * threads (get_thread, put_thread, the connect worker) and main.
 * After every xfer-related mutator was moved onto the main thread
 * (via gtkhx_post_to_main / gtkhx_invoke_sync), the mutex had no
 * job left and was removed. The fields-of-htlc_conn entry for
 * htxf_mutex is gone above, so don't reintroduce these macros
 * without also adding back the field. */

extern void htlc_close (struct htlc_conn *htlc);
extern void hlwrite (struct htlc_conn *htlc, guint32 type, guint32 flag, int hc,
                     ...);
extern void hl_code (void *__dst, const void *__src, size_t len);

#define hl_decode(d, s, l) hl_code (d, s, l)
#define hl_encode(d, s, l) hl_code (d, s, l)

/* ---- File-descriptor / event-loop plumbing ------------------------- */

struct hxd_file {
    union {
        void *ptr;
        struct htlc_conn *htlc;
        struct htrk_conn *htrk;
        struct htxf_conn *htxf;
    } conn;
    guint32 cid;
    int fd;
    void (*ready_read) (int fd);
    void (*ready_write) (int fd);
};

extern struct hxd_file *hxd_files;
extern int hxd_open_max;

extern void hxd_fd_set (int fd, int rw);
extern void hxd_fd_clr (int fd, int rw);

#define FDR 1
#define FDW 2

extern char **hxd_environ;

extern int fd_closeonexec (int fd, int on);
extern int fd_lock_write (int fd);

/* ---- Tasks (in-flight protocol transactions) ----------------------- */

/* Type-erased per-task callback. The dispatcher in rcv.c calls it as
 * (htlc, ptr, data) when a TASK reply arrives, but the rcv_task_*
 * implementations have heterogeneous argument lists (some 1, some 2,
 * some 3 args). Callers cast their function pointer to rcv_task_fn at
 * task_new() time; extras are silently ignored on the register-passing
 * ABIs we run on. This typedef replaces the historic K&R-style
 * `void (*)()` so -Wstrict-prototypes doesn't trip on every consumer
 * of this header. */
struct htlc_conn;
typedef void (*rcv_task_fn) (struct htlc_conn *htlc, void *ptr, void *data);

/* Cast a heterogeneous rcv_task_* implementation to the canonical
 * 3-arg rcv_task_fn shape. The intermediate (void(*)(void)) cast is
 * GCC's documented escape hatch for -Wcast-function-type when the
 * type-erasure is intentional (see GCC manual §6.45). */
#define RCV_TASK_FN(f) ((rcv_task_fn)(void (*) (void)) (f))

struct task {
    /* Phase 5+: no next/prev — tasks live in session->tasks, a
	 * GHashTable<u32 trans, struct task*>. Lookup by trans goes
	 * through task_with_trans (now an O(1) wrapper around
	 * g_hash_table_lookup); iteration goes through GHashTableIter
	 * at the very small number of call sites that need it. */
    guint32 trans;
    guint32 pos, len;
    void *data;

    char *str;
    void *ptr;
    rcv_task_fn rcv;
};

extern int task_inerror (struct htlc_conn *htlc);

#define XFER_GET 0
#define XFER_PUT 1
#define COMPLETE_NONE 0
#define COMPLETE_EXPAND 1
#define COMPLETE_LS_R 2
#define COMPLETE_GET_R 3

/* ---- Crypto helpers (implementations in hmac.c / rand.c) ----------- */

extern u_int16_t hmac_xxx (u_int8_t *md, const void *key, u_int32_t keylen,
                           const void *text, u_int32_t textlen,
                           const char *macalg);

#if defined(CONFIG_CIPHER)
extern unsigned int random_bytes (u_int8_t *buf, unsigned int nbytes);
#endif

/* ---- Byte-order helpers used by the protocol parser ---------------- */

#if (G_BYTE_ORDER == G_LITTLE_ENDIAN)
#define HN32(_to, _from)                                                       \
    do {                                                                       \
        *((unsigned char *)_to) = *(((unsigned char *)_from) + 3);             \
        *(((unsigned char *)_to) + 1) = *(((unsigned char *)_from) + 2);       \
        *(((unsigned char *)_to) + 2) = *(((unsigned char *)_from) + 1);       \
        *(((unsigned char *)_to) + 3) = *((unsigned char *)_from);             \
    } while (0)
#define HN16(_to, _from)                                                       \
    do {                                                                       \
        *((unsigned char *)_to) = *(((unsigned char *)_from) + 1);             \
        *(((unsigned char *)_to) + 1) = *((unsigned char *)_from);             \
    } while (0)
#else
#define HN32(_to, _from)                                                       \
    do {                                                                       \
        *((unsigned char *)_to) = *((unsigned char *)_from);                   \
        *(((unsigned char *)_to) + 1) = *(((unsigned char *)_from) + 1);       \
        *(((unsigned char *)_to) + 2) = *(((unsigned char *)_from) + 2);       \
        *(((unsigned char *)_to) + 3) = *(((unsigned char *)_from) + 3);       \
    } while (0)
#define HN16(_to, _from)                                                       \
    do {                                                                       \
        *((unsigned char *)_to) = *((unsigned char *)_from);                   \
        *(((unsigned char *)_to) + 1) = *(((unsigned char *)_from) + 1);       \
    } while (0)
#endif

static inline void
memory_copy (void *__dst, void *__src, unsigned int len)
{
    u_int8_t *dst = __dst, *src = __src;

    for (; len; len--) {
        *dst++ = *src++;
    }
}

#define S32HTON(_word, _addr)                                                  \
    do {                                                                       \
        u_int32_t _x;                                                          \
        _x = htonl (_word);                                                    \
        memory_copy ((_addr), &_x, 4);                                         \
    } while (0)

/* ---- Walking data-header lists in incoming packets ----------------- */

/*
 * Phase 5: dh_start moved from while-with-tail-increment to a for
 * loop with the position increment in the third clause. The two
 * shapes are equivalent for normal break/iteration, but they
 * differ on `continue`:
 *
 *   while-tail-increment: continue jumps over the increment →
 *                         infinite loop on any non-matching chunk.
 *   for-with-increment:   continue runs the increment, then re-
 *                         evaluates the condition → next chunk.
 *
 * The whole tree had three `continue` sites inside dh_start
 * (hx_rcv_news_post, hx_rcv_agreement_file, and rcv_task_news_file's
 * subroutine). They never hung in the wild because real wire
 * messages of those types only carried matching chunks — a server
 * that mixed e.g. an HTLS_DATA_UID into HTLS_HDR_NEWS_POST would
 * have hung us. Caught by the Tier 2 hx_news_post_walk test that
 * deliberately inserts a non-NEWS chunk in the middle of a NEWS_POST
 * fixture.
 *
 * The condition is a comma-expression that runs HN16 as a side
 * effect and ANDs the bounds checks. Reads dh->len, then bounds-
 * checks; reads dh->type only after we've confirmed the chunk fits.
 */
#define dh_start(_htlc)                                                        \
    {                                                                          \
        struct hl_data_hdr *dh                                                 \
            = (struct hl_data_hdr *)(&((_htlc)->in.buf[SIZEOF_HL_HDR]));       \
        guint32 _pos = SIZEOF_HL_HDR;                                          \
        guint32 _max = (_htlc)->in.pos;                                        \
        guint16 _len = 0, _type = 0;                                           \
        for (; _pos + SIZEOF_HL_DATA_HDR <= _max;                              \
             _pos += SIZEOF_HL_DATA_HDR + _len,                                \
             dh = (struct hl_data_hdr *)(((guint8 *)dh) + SIZEOF_HL_DATA_HDR   \
                                         + _len)) {                            \
            HN16 (&_len, &dh->len);                                            \
            if (_len > (_max - _pos) - SIZEOF_HL_DATA_HDR)                     \
                break;                                                         \
            HN16 (&_type, &dh->type);

#define dh_getint(_word)                                                       \
    do {                                                                       \
        if (_len == 4)                                                         \
            HN32 (&_word, dh->data);                                           \
        else /* if (ntohs(dh->len) == 2) */                                    \
            HN16 (&_word, dh->data);                                           \
    } while (0)

#define dh_end()                                                               \
    }                                                                          \
    }

/* ---- Output sanitization helpers ---------------------------------- */

extern void chrexpand (char *str, int len);

static inline void
strip_ansi (char *buf, int len)
{
    register char *p, *end = buf + len;

    for (p = buf; p < end; p++) {
        if (*p < 31 && *p > 13 && *p != 15 && *p != 22) {
            *p = (*p & 127) | 64;
        }
    }
}

#endif /* ndef __gtkhx_PROTOCOL_H */
