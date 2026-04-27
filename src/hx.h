#ifndef __hxd_HX_H
#define __hxd_HX_H 1

#ifdef __GNUC__
#define PACKED __attribute__((__packed__))
#else
#define PACKED
#endif

#include "hotline.h"
#include <time.h>
#include <sys/time.h>
#include <pthread.h>

#if !defined(__GNUC__) || defined(__STRICT_ANSI__) || defined(__APPLE_CC__)
#define __attribute__(x)
#endif


#ifdef CONFIG_COMPRESS
#include "compress.h"
#endif


#ifdef CONFIG_CIPHER
#include "cipher.h"
#endif

#define HOSTLEN 256

#define MAX_HOTLINE_PACKET_LEN 0x100000
#define UNKNOWN_TYPECREA	"TEXTR*ch"

#ifndef MAXPATHLEN
#ifdef PATH_MAX
#define MAXPATHLEN PATH_MAX
#else
#define MAXPATHLEN 4095
#endif
#endif

#if MAXPATHLEN > 4095
#undef MAXPATHLEN
#define MAXPATHLEN 4095
#endif

struct qbuf {
	guint32 pos, len;
	guint8 *buf;
};

struct htlc_conn;

struct htxf_conn {
	guint32 data_size, data_pos, rsrc_size, rsrc_pos;
	guint32 total_size, total_pos;
	guint32 ref;   /* xfer id */
	guint8 gone;
	guint8 type;
	guint32 queue; /* position in server queue */
	int fd;
#ifdef WIN32
	int pid;
#else
	pthread_t tid;
#endif
	
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
	pthread_mutex_t htxf_mutex;

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

#define LOCK_HTXF(htlc) pthread_mutex_lock(&(htlc->htxf_mutex));
#define UNLOCK_HTXF(htlc) pthread_mutex_unlock(&(htlc->htxf_mutex));
#define INITLOCK_HTXF(htlc) pthread_mutex_init(&(htlc->htxf_mutex), 0);



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

#if !defined(HAVE_INET_NTOA_R)
extern int inet_ntoa_r (struct in_addr in, char *buf, size_t buflen);
#endif

#ifndef HAVE_LOCALTIME_R
extern struct tm *localtime_r (const time_t *t, struct tm *tm);
#endif

#if !defined(HAVE_SNPRINTF) || defined(__hpux__)
extern int snprintf (char *str, size_t count, const char *fmt, ...);
#endif

#if !defined(HAVE_VSNPRINTF) || defined(__hpux__)
extern int vsnprintf (char *str, size_t count, const char *fmt, va_list ap);
#endif

#ifndef HAVE_BASENAME
extern char *basename (const char *path);
#endif

#ifndef RETSIGTYPE
#define RETSIGTYPE void
#endif

extern int fd_closeonexec (int fd, int on);
extern int fd_lock_write (int fd);
extern void timer_add (struct timeval *tv, void (*fn)(), void *ptr);

extern void qbuf_set (struct qbuf *q, guint32 pos, guint32 len);
extern void qbuf_add (struct qbuf *q, void *buf, guint32 len);

extern void htlc_close (struct htlc_conn *htlc);

extern void hlwrite (struct htlc_conn *htlc, guint32 type, guint32 flag, int hc, ...);
extern void hl_code (void *__dst, const void *__src, size_t len);

#define hl_decode(d,s,l) hl_code(d,s,l)
#define hl_encode(d,s,l) hl_code(d,s,l)

extern u_int16_t hmac_xxx (u_int8_t *md, u_int8_t *key, u_int32_t keylen,
			   u_int8_t *text, u_int32_t textlen, u_int8_t *macalg);

#if defined(CONFIG_CIPHER)
extern unsigned int random_bytes (u_int8_t *buf, unsigned int nbytes);
#endif

#define atou32(_str) ((guint32)strtoul(_str, 0, 0))
#define atou16(_str) ((guint16)strtoul(_str, 0, 0))

#if (G_BYTE_ORDER==G_LITTLE_ENDIAN)
#define HN32(_to,_from) \
	do {*((unsigned char*)_to)     = *(((unsigned char*)_from)+3); \
		*(((unsigned char*)_to)+1) = *(((unsigned char*)_from)+2); \
		*(((unsigned char*)_to)+2) = *(((unsigned char*)_from)+1); \
		*(((unsigned char*)_to)+3) = *((unsigned char* )_from); \
	} while (0)
#define HN16(_to,_from) \
	do {*((unsigned char*)_to)     = *(((unsigned char*)_from)+1); \
		*(((unsigned char*)_to)+1) = *((unsigned char* )_from); \
	} while (0)
#else
#define HN32(_to,_from) \
	do {*((unsigned char*)_to)     = *((unsigned char* )_from); \
		*(((unsigned char*)_to)+1) = *(((unsigned char*)_from)+1); \
		*(((unsigned char*)_to)+2) = *(((unsigned char*)_from)+2); \
		*(((unsigned char*)_to)+3) = *(((unsigned char*)_from)+3); \
	} while (0)
#define HN16(_to,_from) \
	do {*((unsigned char*)_to)     = *((unsigned char* )_from); \
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

#define dh_start(_htlc)													\
{															\
	struct hl_data_hdr *dh = (struct hl_data_hdr *)(&((_htlc)->in.buf[SIZEOF_HL_HDR]));				\
	guint32 _pos, _max; \
	guint16 _len, _type;												\
	_pos = SIZEOF_HL_HDR; \
	_max = _htlc->in.pos; \
	while (1) { \
		if(_pos + SIZEOF_HL_DATA_HDR >= _max) break; \
		HN16(&_len, &dh->len); \
		if(_len > (_max - _pos) - SIZEOF_HL_DATA_HDR) break; \
		HN16(&_type, &dh->type);

#define dh_getint(_word) \
do {if (_len == 4) \
		HN32(&_word, dh->data); \
	else /* if (ntohs(dh->len) == 2) */ \
		HN16(&_word, dh->data); \
} while (0)

#define dh_end()	\
		_pos += SIZEOF_HL_DATA_HDR + _len,								\
		dh = (struct hl_data_hdr *)(((guint8 *)dh) + SIZEOF_HL_DATA_HDR + _len); \
	} \
}

#define X2X(_ptr, _len, _x1, _x2) \
do {						\
	char *_p = _ptr, *_end = _ptr + _len;	\
	for ( ; _p < _end; _p++)		\
		if (*_p == _x1)			\
			*_p = _x2;		\
} while (0)

#define CR2LF(_ptr, _len)	X2X(_ptr, _len, '\r', '\n')
#define LF2CR(_ptr, _len)	X2X(_ptr, _len, '\n', '\r')

#define WHITE           "\033[0;37m"
#define WHITE_BOLD      "\033[1;37m"
#define RED             "\033[0;31m"
#define RED_BOLD        "\033[1;31m"

extern int task_inerror (struct htlc_conn *htlc);

#define XFER_GET	0
#define XFER_PUT	1
#define COMPLETE_NONE	0
#define COMPLETE_EXPAND	1
#define COMPLETE_LS_R	2
#define COMPLETE_GET_R	3

#define MAX_CONN 1

#ifdef HAVE_DCGETTEXT
#include <libintl.h>
#define _(string) dgettext (PACKAGE, string)
#else
#define _(string) (string)
#endif

typedef struct {
	int xsize, ysize;
	int xpos, ypos;
	unsigned int open:1;
	unsigned char init;
} Window_Geo;

struct gtkhx_prefs {
	int num_tracker;
	int num_icons;
	int trans_xtext;
#ifdef USE_GDK_PIXBUF
	int tint_red, tint_blue, tint_green;
#endif
	char *sound_path;
	char *auto_reply_msg;
	char *font;
	char *download_path;
	char **tracker;
	char *tracker_str;
	char **icon;
	char *icon_str;
	char *snd_cmd;
	int xbuf_max;

	struct {
		Window_Geo chat;
		Window_Geo news;
		Window_Geo tool;
		Window_Geo tasks;
		Window_Geo users;
	} geo;

	unsigned char queuedl;
	unsigned char showjoin;
	unsigned char showback;
	unsigned char auto_reply;
	unsigned char use_fontset;
	unsigned char timestamp;
	unsigned char word_wrap;
	unsigned char file_samewin;
	unsigned char news_samewin;
	unsigned char track_case;
	unsigned char old_nickcompletion;
	unsigned char outrate_limit;
	unsigned char inrate_limit;
	unsigned char logging;

	int out_bps;
	int in_bps;
};

extern struct gtkhx_prefs gtkhx_prefs;

struct msgwin {
	struct msgwin *next, *prev;
	guint16 *uid;
	char *name;
	GtkWidget *outputbuf;
	GtkWidget *inputbuf;
	GtkWidget *vscroll;
	GtkWidget *window;
	void *history;
};

struct timer {
	struct timer *next, *prev;
	guint id;
	int (*fn)();
	void *ptr;
};

#include "macres.h"

struct ifn {
	char **files;
	macres_file **cicns;
	unsigned int n;
};

struct gtkhx_chat {
	struct gtkhx_chat *next, *prev;
	GtkWidget *window;
	GtkWidget *vscroll;
	GtkWidget *output;
	GtkWidget *input;
	GtkWidget *subject;
	GtkWidget *userlist;
	guint32 cid;
	struct chat *chat;
	void *chat_history;
};

struct hx_sounds {
	unsigned char on;
	unsigned char invite, chat, error, file, join, login, msg, news, part;
};

struct date_time {
  guint16 base_year;
  guint16 pad;
  guint32 seconds;
};

struct news_post {
	char *buf;
	struct news_item *item;
};

struct news_parts {
	int size;
	char *mime_type;
};

struct news_item {
	guint32 postid,parentid;
	char *sender;
	char *subject;
	struct date_time date;
	guint16 partcount;
	guint16 size;
	struct news_parts *parts;
	GtkCTreeNode *node;
	struct news_group *group;
};

struct news_group {
	int post_count;
	struct news_item *posts;
	char *path;
};

struct gnews_catalog {
	struct news_group *group;
	struct gnews_catalog *next, *prev;
	char *path;
	GtkWidget *window;
	GtkWidget *news_tree;
	GtkWidget *news_text;
	GtkWidget *authorlbl, *subjectlbl, *datelbl;
	GtkCTreeNode *row;
	char listing;
};

struct path_hist {
	char path[4096];
	struct path_hist *prev;
};

struct gnews_folder {
	GtkWidget *window;
	GtkWidget *news_list;
	gint row, col;
	struct news_folder *news;
	struct gnews_folder *next, *prev;
	char *path;
	char listing;
	GtkWidget *up_btn;
	struct path_hist *path_list;
};


struct folder_item {
	char *name;
/*	guint16 icon; */
	int type;
};

struct news_folder {
	struct folder_item **entry;
	char *path;
	guint32 num_entries;
};

struct uesp_fn {
	void *uesp;
	void (*fn)(void *, const char *, const char *, const char *, const hl_access_bits);
};

struct task {
	struct task *next, *prev;
	guint32 trans;
	guint32 pos, len;
	void *data;

	char *str;
	void *ptr;
	void (*rcv)();
};

struct hx_user {
	struct hx_user *next, *prev;
	guint16 uid;
	guint16 icon;
	guint16 color;
	unsigned char name[32];
	unsigned int ignore:1;
};

struct chat {
	struct chat *next, *prev;
	guint32 cid;
	guint32 nusers;
	struct hx_user __user_list;
	struct hx_user *user_list;
	struct hx_user *user_tail;
	char subject[256];
};


typedef struct _session {
	GtkWidget *toolbar_window;
	GtkWidget *news_window;
	GtkWidget *chat_window;
	GtkWidget *tasks_window;
	GtkWidget *users_window;

	GtkWidget *users_list;

	GtkWidget *news_text;
	GtkWidget *postButton;
	GtkWidget *reloadButton;

	GtkWidget *agreementwin;

	struct gtkhx_chat *gchat_list;

	struct gtask *gtask_list;
	GtkWidget *gtklist, *gtask_scroll;

	struct gnews_folder *gfnews_list;

	struct gfile_list *gfile_list;

	struct msgwin *msg_list;

	struct gnews_catalog *gcnews_list;

	struct task __task_list;
	struct task *task_list, *task_tail;

	struct hx_user __user_list;
	struct hx_user *user_list, *user_tail;

	struct chat *chat_front, *chat_tail, *chat_list;
	struct chat __chat_list;

	struct htlc_conn htlc;

	unsigned int connected:1;
} session;

extern session sessions[MAX_CONN];

extern session *sess_from_htlc(struct htlc_conn *htlc);

extern char last_msg_nick[32];
extern const char *INFOPREFIX;
extern char *g_user_colors[4];

extern char *colorstr (guint16 color);

extern void hotline_client_input (struct htlc_conn *htlc, char *str, guint32 cid, guint16 style);

extern void hx_connect (struct htlc_conn *htlc, const char *serverstr, 
						guint16 port, const char *login, const char *pass,
						char secure);
extern void hx_tracker_list (session *sess, char *addrstr, guint16 port);
extern void hx_quit (void);

#ifdef USE_DEBUG
#define debug(fmt,args...) {printf("%s:%d: ",__FILE__,__LINE__);printf(fmt,##args);fflush(stdout);}
#else
#define debug(args...)
#endif

extern void chrexpand (char *str, int len);

#if !defined(__va_copy)
#define __va_copy(_dst, _src) ((_dst) = (_src))
#endif

static inline void
strip_ansi (char *buf, int len)
{
	register char *p, *end = buf + len;

	for (p = buf; p < end; p++)
		if (*p < 31 && *p > 13 && *p != 15 && *p != 22)
				*p = (*p & 127) | 64;
}


struct cached_filelist {
	struct cached_filelist *next, *prev;
	char *path;
	struct hl_filelist_hdr *fh;
	guint32 fhlen;
	unsigned int completing:2;
	char **filter_argv;
};

struct output_functions {
	void (*init)(int argc, char **argv);
	void (*loop)(void);
	void (*clear)(struct htlc_conn *htlc, guint32 cid, int subj);
	void (*chat)(struct htlc_conn *htlc, guint32 cid, char *chat, guint16 len);
	void (*msg)(char *name, guint16 uid, char *buf);
	void (*agreement)(session *sess, const char *agreement, guint16 len);
	void (*news_file)(struct htlc_conn *htlc, char *news, guint16 len);
	void (*news_post)(struct htlc_conn *htlc, char *news, guint16 len);
	void (*user_info)(guint16 uid, const char *nam, const char *info, guint16 len);
	void (*file_info) (char *path, char *name, char *creator, char *type, char *comments, char *modified, char *created, guint32 size);
	void (*user_create)(struct htlc_conn *htlc, struct chat *chat, struct hx_user *user, const char *nam, guint16 icon, guint16 color);
	void (*user_delete)(struct htlc_conn *htlc, struct chat *chat, struct hx_user *user);
	void (*user_change)(struct htlc_conn *htlc, struct chat *chat, struct hx_user *user, const char *nam, guint16 icon, guint16 color);
	void (*user_list)(struct htlc_conn *htlc);
	void (*users_clear)(struct htlc_conn *htlc, struct chat *chat);
	void (*file_list)(struct cached_filelist *cfl, struct hl_filelist_hdr *fh, void *data);
	void (*file_update)(session *sess, struct htxf_conn *htxf);
	void (*tracker_server_create)(struct in_addr addr, guint16 port, guint16 nusers, const char *nam, const char *desc, int total);
	void (*task_update)(session *sess, struct task *tsk);
	void (*news_folder)(struct gnews_folder *gfnews);
	void (*news_catalog)(struct gnews_catalog *gcnews);
	void (*news_thread)(struct news_post *post);
	void (*chat_subject)(struct htlc_conn *htlc, guint32 cid, char *buf);
	void (*chat_invitation)(struct htlc_conn *htlc, guint32 cid, char *name);
	void (*xfer_queue)(session *sess, struct htxf_conn *htxf);
	void (*tracker_clear)(void);
};

extern struct output_functions hx_output;
extern void timer_add_secs (time_t secs, int (*fn)(), void *ptr);
extern void timer_delete_ptr (void *ptr);

#endif /* ndef __hxd_HX_H */
