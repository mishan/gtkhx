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
#include <sys/types.h>		/* u_int8_t / u_int16_t / u_int32_t */
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

#ifdef USE_IPV6
#include <netdb.h>		/* struct addrinfo */
#else
#include <netinet/in.h>		/* struct sockaddr_in */
#endif

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
	guint32 data_size, data_pos, rsrc_size, rsrc_pos;
	guint32 total_size, total_pos;
	/* Server's data-fork size from the file listing, captured at
	 * xfer_new time. Used by xfer_go to choose between resume
	 * (local exists and is strictly smaller than server) and
	 * rename-on-collision (local is the same size or larger, or
	 * server size is unknown). 0 == unknown — listing wasn't
	 * available for this transfer. */
	guint32 srv_data_size;
	guint32 ref;	/* xfer id */
	guint8 gone;
	guint8 type;
	guint32 queue;	/* position in server queue */
	int fd;
	pthread_t tid;

#ifdef USE_IPV6
	struct addrinfo *listen_addr;
#else
	struct sockaddr_in listen_addr;
#endif
	struct htlc_conn *htlc;
	char path[MAXPATHLEN];
	char remotepath[MAXPATHLEN];
	struct qbuf in;
	char **filter_argv;
	struct timeval start;

	struct {
		guint32 retry:1, preview:1, reserved:30;
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
	void (*rcv)(struct htlc_conn *);
	void (*real_rcv)(struct htlc_conn *);
	struct qbuf in, out;
	struct qbuf read_in;
#ifdef USE_IPV6
	struct addrinfo *addr;
#else
	struct sockaddr_in addr;
#endif
	int fd;
	guint32 trans;
	guint32 chattrans;
	guint16 icon;
	guint16 uid;
	guint16 version;

	struct {
		guint32 visible:1, reserved:31;
	} flags;

	hl_access_bits access;
	guint8 name[32];
	guint8 login[32];

	unsigned int gdk_input:1;

	guint16 color;

	u_int8_t macalg[32];
	u_int8_t sessionkey[64];
	u_int16_t sklen;

#if defined(CONFIG_CIPHER)
	u_int8_t cipheralg[32];
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
	u_int8_t compressalg[32];
	union compress_state compress_encode_state;
	union compress_state compress_decode_state;
	u_int16_t compress_encode_type, compress_decode_type;
	unsigned long gzip_inflate_total_in, gzip_inflate_total_out;
	unsigned long gzip_deflate_total_in, gzip_deflate_total_out;
#endif
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
extern void hlwrite (struct htlc_conn *htlc, guint32 type, guint32 flag, int hc, ...);
extern void hl_code (void *__dst, const void *__src, size_t len);

#define hl_decode(d,s,l) hl_code(d,s,l)
#define hl_encode(d,s,l) hl_code(d,s,l)

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
	void (*ready_read)(int fd);
	void (*ready_write)(int fd);
};

extern struct hxd_file *hxd_files;
extern int hxd_open_max;

extern void hxd_fd_set (int fd, int rw);
extern void hxd_fd_clr (int fd, int rw);

#define FDR	1
#define FDW	2

extern char **hxd_environ;

extern int  fd_closeonexec (int fd, int on);
extern int  fd_lock_write  (int fd);
extern void timer_add      (struct timeval *tv, void (*fn)(), void *ptr);

/* ---- Tasks (in-flight protocol transactions) ----------------------- */

struct task {
	struct task *next, *prev;
	guint32 trans;
	guint32 pos, len;
	void *data;

	char *str;
	void *ptr;
	void (*rcv)();
};

extern int task_inerror (struct htlc_conn *htlc);

#define XFER_GET	0
#define XFER_PUT	1
#define COMPLETE_NONE	0
#define COMPLETE_EXPAND	1
#define COMPLETE_LS_R	2
#define COMPLETE_GET_R	3

/* ---- Crypto helpers (implementations in hmac.c / rand.c) ----------- */

extern u_int16_t hmac_xxx (u_int8_t *md, u_int8_t *key, u_int32_t keylen,
			   u_int8_t *text, u_int32_t textlen, u_int8_t *macalg);

#if defined(CONFIG_CIPHER)
extern unsigned int random_bytes (u_int8_t *buf, unsigned int nbytes);
#endif

/* ---- Byte-order helpers used by the protocol parser ---------------- */

#if (G_BYTE_ORDER==G_LITTLE_ENDIAN)
#define HN32(_to,_from)							\
	do {*((unsigned char*)_to)     = *(((unsigned char*)_from)+3);	\
		*(((unsigned char*)_to)+1) = *(((unsigned char*)_from)+2); \
		*(((unsigned char*)_to)+2) = *(((unsigned char*)_from)+1); \
		*(((unsigned char*)_to)+3) = *((unsigned char* )_from);	\
	} while (0)
#define HN16(_to,_from)							\
	do {*((unsigned char*)_to)     = *(((unsigned char*)_from)+1);	\
		*(((unsigned char*)_to)+1) = *((unsigned char* )_from);	\
	} while (0)
#else
#define HN32(_to,_from)							\
	do {*((unsigned char*)_to)     = *((unsigned char* )_from);	\
		*(((unsigned char*)_to)+1) = *(((unsigned char*)_from)+1); \
		*(((unsigned char*)_to)+2) = *(((unsigned char*)_from)+2); \
		*(((unsigned char*)_to)+3) = *(((unsigned char*)_from)+3); \
	} while (0)
#define HN16(_to,_from)							\
	do {*((unsigned char*)_to)     = *((unsigned char* )_from);	\
		*(((unsigned char*)_to)+1) = *(((unsigned char*)_from)+1); \
	} while (0)
#endif

static inline void
memory_copy (void *__dst, void *__src, unsigned int len)
{
	u_int8_t *dst = __dst, *src = __src;

	for (; len; len--)
		*dst++ = *src++;
}

#define S32HTON(_word, _addr) \
	do { u_int32_t _x; _x = htonl(_word); memory_copy((_addr), &_x, 4); } while (0)

/* ---- Walking data-header lists in incoming packets ----------------- */

#define dh_start(_htlc)								\
{										\
	struct hl_data_hdr *dh = (struct hl_data_hdr *)(&((_htlc)->in.buf[SIZEOF_HL_HDR])); \
	guint32 _pos, _max;							\
	guint16 _len, _type;							\
	_pos = SIZEOF_HL_HDR;							\
	_max = _htlc->in.pos;							\
	while (1) {								\
		if(_pos + SIZEOF_HL_DATA_HDR >= _max) break;			\
		HN16(&_len, &dh->len);						\
		if(_len > (_max - _pos) - SIZEOF_HL_DATA_HDR) break;		\
		HN16(&_type, &dh->type);

#define dh_getint(_word)						\
do {if (_len == 4)							\
		HN32(&_word, dh->data);					\
	else /* if (ntohs(dh->len) == 2) */				\
		HN16(&_word, dh->data);					\
} while (0)

#define dh_end()							\
		_pos += SIZEOF_HL_DATA_HDR + _len,			\
		dh = (struct hl_data_hdr *)(((guint8 *)dh) + SIZEOF_HL_DATA_HDR + _len); \
	}								\
}

/* ---- Output sanitization helpers ---------------------------------- */

extern void chrexpand (char *str, int len);

static inline void
strip_ansi (char *buf, int len)
{
	register char *p, *end = buf + len;

	for (p = buf; p < end; p++)
		if (*p < 31 && *p > 13 && *p != 15 && *p != 22)
				*p = (*p & 127) | 64;
}

#endif /* ndef __gtkhx_PROTOCOL_H */
