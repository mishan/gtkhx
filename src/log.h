#if 0 /* XXX */

#ifndef HX_LOG_H
#define HX_LOG_H

struct log {
	int fd;
	char *filename;
	struct log *next, *prev;
};

extern struct log *create_log(char *name);
extern void print_log(struct log *log, char *buf);
extern void close_log(struct log *log);
extern void close_logs(void);

#endif

#endif
