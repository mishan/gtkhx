#ifndef _MACRES_H
#define _MACRES_H

#include <unistd.h>
#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif

struct macres_res;

struct macres_res_ref_list {
	gint16 resid;
	guint16 res_map_name_list_name_off;
	guint8 res_attrs;
	guint32 res_data_off;
	struct macres_res *mr;
};

typedef struct macres_res_ref_list macres_res_ref_list;

struct macres_res_type_list {
	guint32 res_type;
	guint32 num_res_of_type;
	guint16 res_ref_list_off;
	macres_res_ref_list *cached_ref_list;
};

typedef struct macres_res_type_list macres_res_type_list;

struct macres_res_map {
	guint16 res_type_list_off;
	guint16 res_name_list_off;
	guint32 num_res_types;
	macres_res_type_list *res_type_list;
};

typedef struct macres_res_map macres_res_map;

struct macres_file {
	int fd;
	unsigned char *base;
	size_t len;
	guint32 first_res_off;
	guint32 res_data_len;
	guint32 res_map_off;
	guint32 res_map_len;
	macres_res_map res_map;
};

typedef struct macres_file macres_file;

struct macres_res {
	guint32 datalen;
	guint16 resid;
	guint16 namelen;
	guint8 *name;
	void *data;
};

typedef struct macres_res macres_res;

extern macres_file *macres_file_new (void);
extern macres_file *macres_file_open (int fd);
extern macres_res *macres_res_read (unsigned char *);
extern guint32 macres_file_num_res_of_type (macres_file *mrf, guint32 type);
extern macres_res *macres_file_get_nth_res_of_type (macres_file *mrf, guint32 type, guint32 n);
extern macres_res *macres_file_get_resid_of_type (macres_file *mrf, guint32 type, gint16 resid);
extern void macres_file_print (macres_file *mrf);
extern void macres_file_delete (macres_file *mrf);
extern int macres_add_resource (macres_file *mrf, guint32 type, gint16 resid, const guint8 *name, guint8 namelen, const guint8 *data, guint32 datalen);

#endif /* _MACRES_H */
