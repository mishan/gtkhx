/*
 * Copyright (C) 2000-2002 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <glib.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "macres.h"


static inline guint32 _e_int32(unsigned char **base)
{
	unsigned char *b = *base;
	guint32 ret = (*b++) << 24;
	ret |= (*b++) << 16;
	ret |= (*b++) << 8;
	ret |= *b++;
	*base = b;
	return ret;
}

static inline guint32 _e_int24(unsigned char **base)
{
	unsigned char *b = *base;
	guint32 ret = (*b++) << 16;
	ret |= (*b++) << 8;
	ret |= *b++;
	*base = b;
	return ret;
}

static inline guint16 _e_int16(unsigned char **base)
{
	unsigned char *b = *base;
	guint16 ret = (*b++) << 8;
	ret |= *b++;
	*base = b;
	return ret;
}

#define e_int32(foo) _e_int32(&foo)
#define e_int24(foo) _e_int24(&foo)
#define e_int16(foo) _e_int16(&foo)

macres_res_type_list *
macres_res_type_list_get (macres_res_type_list *typelist, guint32 ntypes, guint32 type)
{
	guint32 i;

	for (i = 0; i < ntypes; i++)
		if (typelist[i].res_type == type)
			return &typelist[i];

	return 0;
}

guint32
macres_file_num_res_of_type (macres_file *mrf, guint32 type)
{
	macres_res_type_list *rtl;

	rtl = macres_res_type_list_get(mrf->res_map.res_type_list, mrf->res_map.num_res_types, type);
	if (rtl)
		return rtl->num_res_of_type;

	return 0;
}

macres_res_type_list *
macres_file_read_res_type_list (macres_file *mrf)
{
	guint32 i;
	macres_res_type_list *rtl;
	/* Seek past the 2B type list count field */
	unsigned char *filepos = mrf->base + mrf->res_map_off + mrf->res_map.res_type_list_off + 2;

	rtl = g_malloc(sizeof(macres_res_type_list) * mrf->res_map.num_res_types);
	if (!rtl)
		return 0;
	for (i = 0; i < mrf->res_map.num_res_types; i++) {
		rtl[i].res_type = e_int32(filepos);
		rtl[i].num_res_of_type = e_int16(filepos) + 1;
		rtl[i].res_ref_list_off = e_int16(filepos);
		rtl[i].cached_ref_list = 0;
	}

	return rtl;
}

int
macres_file_read_res_map (macres_file *mrf)
{
	unsigned char *filepos = mrf->base + mrf->res_map_off + 24;

	mrf->res_map.res_type_list_off = e_int16(filepos);
	mrf->res_map.res_name_list_off = e_int16(filepos);
	mrf->res_map.num_res_types = e_int16(filepos) + 1;

	mrf->res_map.res_type_list = macres_file_read_res_type_list(mrf);

	return 0;
}

macres_res_ref_list *
macres_file_read_res_ref_list (macres_file *mrf, guint32 type);

macres_res_ref_list *
macres_res_ref_list_new (macres_file *mrf, guint32 type, macres_res_type_list *rtl)
{
	macres_res_ref_list *rl;

	rl = rtl->cached_ref_list;
	if (!rl)
		rl = rtl->cached_ref_list = macres_file_read_res_ref_list(mrf, type);
	rl = g_realloc(rl, rtl->num_res_of_type * sizeof(macres_res_ref_list));
	if (!rl)
		return 0;
	if (!rtl->cached_ref_list)
		memset(rl, 0, rtl->num_res_of_type*sizeof(macres_res_ref_list));
	rtl->cached_ref_list = rl;

	return rl;
}

macres_res *
macres_res_new (const guint8 *name, guint8 namelen, const guint8 *data, guint32 len)
{
	macres_res *mr;

	mr = g_malloc(sizeof(macres_res) + len);
	if (!mr)
		return 0;
	mr->namelen = namelen;
	if (namelen) {
		mr->name = g_malloc(namelen+1);
		if (mr->name) {
			memcpy(mr->name, name, namelen);
			mr->name[namelen] = 0;
		} else
			mr->name = 0;
	} else {
		mr->name = 0;
	}
	mr->datalen = len;
	memcpy(mr->data, data, len);

	return mr;
}

macres_file *
macres_file_open (int fd)
{
	macres_file *mrf;
	struct stat bah;
	unsigned char *ptr;

	mrf = g_malloc(sizeof(macres_file));
	if (!mrf)
		return 0;

	fstat (fd, &bah);

	mrf->fd = fd;
	mrf->len = bah.st_size;
	mrf->base = mmap (0, bah.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (!mrf->base) return 0;
	ptr = mrf->base;


	mrf->first_res_off = e_int32(ptr);
	mrf->res_map_off = e_int32(ptr);
	mrf->res_data_len = e_int32(ptr);
	mrf->res_map_len = e_int32(ptr);

	macres_file_read_res_map(mrf);

	return mrf;
}

void
macres_file_delete (macres_file *mrf)
{
	int i;
	munmap (mrf->base, mrf->len);


	for (i = 0; i < mrf->res_map.num_res_types; i++)
		g_free(mrf->res_map.res_type_list[i].cached_ref_list);
	g_free(mrf->res_map.res_type_list);
	g_free(mrf);
}

macres_res *
macres_res_read (unsigned char *base)
{
	guint32 len;
	macres_res *mr;

	len = e_int32(base);
	mr = g_malloc(sizeof(macres_res));
	if (!mr)
		return 0;
	mr->datalen = len;
	mr->data = base;

	return mr;
}

macres_res_ref_list *
macres_file_read_res_ref_list (macres_file *mrf, guint32 type)
{
	guint32 i, found;
	macres_res_ref_list *rl;
	macres_res_type_list *rtl = 0;
	unsigned char *filepos;

	for (i = 0, found = 0; i < mrf->res_map.num_res_types && !found; i++) {
		rtl = &mrf->res_map.res_type_list[i];
		found = (rtl->res_type == type);
	}
	if (!found)
		return 0;

	rl = g_malloc(rtl->num_res_of_type * sizeof(macres_res_ref_list));
	if (!rl)
		return 0;

	filepos = mrf->base + mrf->res_map_off + mrf->res_map.res_type_list_off + rtl->res_ref_list_off;

	for (i = 0; i < rtl->num_res_of_type; i++)
	{
		rl[i].resid = e_int16(filepos);
		rl[i].res_map_name_list_name_off = e_int16(filepos);
		rl[i].res_attrs = *filepos++;
		rl[i].res_data_off = e_int24(filepos);
		filepos += 4; /* Skip the reserved handle field */
		rl[i].mr = 0;
	}

	return rl;
}

macres_res *
macres_file_get_nth_res_of_type (macres_file *mrf, guint32 type, guint32 n)
{
	macres_res *mr = 0;
	macres_res_type_list *rtl;
	macres_res_ref_list *rl;

	rtl = macres_res_type_list_get(mrf->res_map.res_type_list, mrf->res_map.num_res_types, type);
	if (rtl) {
		rl = rtl->cached_ref_list;
		if (!rl)
			rl = rtl->cached_ref_list = macres_file_read_res_ref_list(mrf, type);
		if (rl) {
			int16_t resid = rl[n].resid;

			mr = macres_res_read(mrf->base + mrf->first_res_off + rl[n].res_data_off);
			mr->name = 0;
			mr->namelen = 0;
			rl[n].mr = mr;
			if (mr) {
				mr->resid = resid;
/*				if (rl[n].res_map_name_list_name_off != 0xffff) {
					unsigned char *filepos = mrf->base + mrf->res_map_off + mrf->res_map.res_name_list_off + rl[n].res_map_name_list_name_off;
					mr->namelen = *filepos++;
					if (mr->namelen) {
						mr->name = g_malloc(mr->namelen+1);
						if (mr->name) {
							memcpy(mr->name, filepos, mr->namelen);
							mr->name[mr->namelen] = 0;
						}
					}
				} */
			}
		}
	}

	return mr;
}

macres_res *
macres_file_get_resid_of_type (macres_file *mrf, guint32 type, int16_t resid)
{
	guint32 i;
	macres_res *mr = 0;
	macres_res_type_list *rtl;
	macres_res_ref_list *rl;

	rtl = macres_res_type_list_get(mrf->res_map.res_type_list, mrf->res_map.num_res_types, type);
	if (rtl) {
		rl = rtl->cached_ref_list;
		if (!rl)
			rl = rtl->cached_ref_list = macres_file_read_res_ref_list(mrf, type);
		if (!rl)
			return 0;
		for (i = 0; i < rtl->num_res_of_type; i++) {
			if (rl[i].resid != resid)
				continue;
			mr = macres_res_read(mrf->base + mrf->first_res_off + rl[i].res_data_off);
			mr->name = 0;
			mr->namelen = 0;
			rl[i].mr = mr;
			if (mr) {
				mr->resid = resid;;
				/*
				if (rl[i].res_map_name_list_name_off != 0xffff) {
					unsigned char *filepos = mrf->base + mrf->res_map_off +
							mrf->res_map.res_name_list_off +
							rl[i].res_map_name_list_name_off;
					guint8 namelen = *filepos++;

					if (namelen) {
						mr->name = g_malloc(namelen + 1);
						if (mr->name) {
							memcpy(mr->name, filepos, namelen);
							mr->name[namelen] = 0;
						}
					}
					mr->namelen = namelen;
				} */
			}
			break;
		}
	}

	return mr;
}
