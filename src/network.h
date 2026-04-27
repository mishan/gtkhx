#ifndef HX_NETWORK_H
#define HX_NETWORK_H

/* we only want pthread.h if we're not in debug mode */
#ifndef DEBUG
#include <pthread.h>

extern pthread_t conn_tid;
#endif

extern char *server_addr;

#ifdef USE_IPV6
extern guint16 server_port;
#endif
extern struct log *server_log;

extern int connected;

extern int fd_closeonexec (int fd, int on);
extern int fd_lock_write(int fd);

extern void hx_htlc_close(struct htlc_conn *htlc, int expected);
extern void hx_connect(struct htlc_conn *htlc, const char *serverstr, 
					   guint16 port, const char *login, const char *pass, 
					   char secure);

extern void kill_threads(void);

extern int htxf_connect (struct htxf_conn *htxf);

extern void hlwrite (struct htlc_conn *htlc, guint32 type, guint32 flag, 
					 int hc, ...);
extern void hl_code (void *__dst, const void *__src, size_t len);

#endif /* HX_NETWORK_H */
