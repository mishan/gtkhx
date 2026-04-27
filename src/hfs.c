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

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <gtk/gtk.h>
#include <netinet/in.h>
#include "hfs.h"
#include "files.h"
#include "xfers.h"

#define DIRCHAR			'/'
#define UNKNOWN_TYPECREA	"TEXTR*ch"

struct hfs_config {
	long fork;
	long file_perm;
	long dir_perm;
	char *comment;
};

struct hfs_config cfg = {HFS_FORK_CAP, 0600, 0700, 0};

void
hfs_set_config (long fork, long file_perm, long dir_perm, char *comment)
{
	cfg.fork = fork;
	cfg.file_perm = file_perm;
	cfg.dir_perm = dir_perm;
	cfg.comment = comment;
}

int
finderinfo_path (char *infopath, const char *path, struct stat *statbuf)
{
	int i, len;
	char pathbuf[MAXPATHLEN];

	len = strlen(path);
	if (len + 16 >= MAXPATHLEN)
		return ENAMETOOLONG;
	strcpy(pathbuf, path);
	for (i = len - 1; i > 0; i--)
		if (pathbuf[i] == dir_char) {
			pathbuf[i++] = 0;
			break;
		}

	snprintf(infopath, MAXPATHLEN, "%s", i > 0 ? pathbuf : ".");
	strcat(infopath, "/");
	strcat(infopath, &pathbuf[i]);
	strcat(infopath, ".fndrinfo");
	if (statbuf && stat(infopath, statbuf))
		return errno;

	return 0;
}

int resource_path (char *rsrcpath, const char *path, struct stat *statbuf)
{
	int i;
	size_t len;
	char pathbuf[MAXPATHLEN];

	len = strlen(path);
	if (len + 16 >= MAXPATHLEN)
		return ENAMETOOLONG;
	strcpy(pathbuf, path);
	for (i = len - 1; i > 0; i--)
		if (pathbuf[i] == dir_char) {
			pathbuf[i++] = 0;
			break;
		}

	snprintf(rsrcpath, MAXPATHLEN, "%s", i > 0 ? pathbuf : ".");
	strcat(rsrcpath, "/");
	strcat(rsrcpath, &pathbuf[i]);
	strcat(rsrcpath, ".rsrc");
	
	if (statbuf && stat(rsrcpath, statbuf))
		return errno;

	return 0;
}


int
resource_open (const char *path, int mode, int perm)
{
	if (cfg.fork == HFS_FORK_CAP) {
		char rsrcpath[MAXPATHLEN];

		if (resource_path(rsrcpath, path, 0))
			return -1;
		return open(rsrcpath, mode, perm);
	} else { /* AppleDouble */
		char infopath[MAXPATHLEN];
		struct hfs_dbl_hdr dbl;
		struct stat sb;
		ssize_t r;
		u_int16_t i;
		int f;

		if (!finderinfo_path(infopath, path, &sb)) {
			f = open(infopath, mode);
			if (f < 0)
				return 0;
			r = read(f, &dbl, SIZEOF_HFS_DBL_HDR);
			if (r != SIZEOF_HFS_DBL_HDR)
				goto funkdat;
			if (ntohs(dbl.entries) > HFS_HDR_MAX)
				dbl.entries = htons(HFS_HDR_MAX);
			r = read(f, &dbl.descrs, SIZEOF_HFS_HDR_DESCR * 
					 ntohs(dbl.entries));
			if (r != SIZEOF_HFS_HDR_DESCR * ntohs(dbl.entries))
				goto funkdat;
			for (i = 0; i < ntohs(dbl.entries); i++) {
				struct hfs_hdr_descr *descr = (struct hfs_hdr_descr *)
					(&dbl.descrs[SIZEOF_HFS_HDR_DESCR * i]);
				if (ntohl(descr->id) == HFS_HDR_RSRC) {
					if (lseek(f, ntohl(descr->offset), SEEK_SET) != 
						(off_t)ntohl(descr->offset))
						goto funkdat;
					return f;
				}
			}
funkdat:
			close(f);
			return -1;
		}
	}

	return -1;
}

size_t
resource_len (const char *path)
{
	size_t len = 0;

	if (cfg.fork == HFS_FORK_CAP) {
		char rsrcpath[MAXPATHLEN];
		struct stat sb;

		if (!resource_path(rsrcpath, path, &sb))
			len = sb.st_size;
	} else { /* AppleDouble */
		char infopath[MAXPATHLEN];
		struct hfs_dbl_hdr dbl;
		struct stat sb;
		ssize_t r;
		u_int16_t i;
		int f;

		if (!finderinfo_path(infopath, path, &sb)) {
			f = open(infopath, O_RDONLY);
			if (f < 0)
				return 0;
			r = read(f, &dbl, SIZEOF_HFS_DBL_HDR);
			if (r != SIZEOF_HFS_DBL_HDR)
				goto funkdat;
			if (ntohs(dbl.entries) > HFS_HDR_MAX)
				dbl.entries = htons(HFS_HDR_MAX);
			r = read(f, &dbl.descrs, SIZEOF_HFS_HDR_DESCR * 
					 ntohs(dbl.entries));
			if (r != SIZEOF_HFS_HDR_DESCR * ntohs(dbl.entries))
				goto funkdat;
			for (i = 0; i < ntohs(dbl.entries); i++) {
				struct hfs_hdr_descr *descr = (struct hfs_hdr_descr *)
					(&dbl.descrs[SIZEOF_HFS_HDR_DESCR * i]);
				if (ntohl(descr->id) == HFS_HDR_RSRC) {
					len = ntohl(descr->length);
					break;
				}
			}
funkdat:
			close(f);
		}
	}

	return len;
}

static inline char *
suffix (const char *path)
{
	char *p = (char *)path + strlen(path);

	while (p-- > path)
		if (*p == '.')
			return p + 1;
	return 0;
}

static void
suffix_type_creator (char *buf, const char *path)
{
	u_int8_t *tc = UNKNOWN_TYPECREA;
	char *suff;

	if (!(suff = suffix(path)))
		goto cpy;

	if (!strcmp(suff, "jpg") || !strcmp(suff, "jpeg"))
		tc = "JPEGGKON";
	else if (!strcmp(suff, "png"))
		tc = "PNGfGKON";
	else if (!strcmp(suff, "gif"))
		tc = "GIFfGKON";
	else if (!strcmp(suff, "mp3") || !strcmp(suff, "mp2"))
		tc = "MP3 MAmp";
	else if (!strcmp(suff, "mpg") || !strcmp(suff, "mpeg"))
		tc = "MPEGTVOD";
	else if (!strcmp(suff, "mov"))
		tc = "MooVTVOD";
	else if (!strcmp(suff, "sit"))
		tc = "SITDSIT!";
	else if (!strcmp(suff, "zip") || !strcmp(suff, "pk3"))
		tc = "ZIP ZIP ";
	else if (!strcmp(suff, "app") || !strcmp(suff, "sea"))
		tc = "APPLpeff";
	else if (!strcmp(suff, "img"))
		tc = "rohdWrap";
	else if (!strcmp(suff, "pict"))
		tc = "PICTGKON";

cpy:
	memcpy(buf, tc, 8);
}

void
type_creator (u_int8_t *buf, const char *path)
{
	char infopath[MAXPATHLEN];
	struct stat sb;

	if (!finderinfo_path(infopath, path, &sb)) {
		int r, f;

		f = open(infopath, O_RDONLY);
		if (f < 0)
			goto use_suffix;
		switch (cfg.fork) {
			default:
			case HFS_FORK_CAP:
				r = read(f, buf, 8);
				if (r != 8 || !(*(u_int32_t *)(&buf[0])) || !(*(u_int32_t *)
															  (&buf[4])))
					r = 0;
				break;
			case HFS_FORK_NETATALK:
			case HFS_FORK_DOUBLE:
				{
					struct hfs_dbl_hdr dbl;
					int i;

					r = read(f, &dbl, SIZEOF_HFS_DBL_HDR);
					if (r != SIZEOF_HFS_DBL_HDR) {
						r = -1;
						break;
					}
					if (ntohs(dbl.entries) > HFS_HDR_MAX)
						dbl.entries = htons(HFS_HDR_MAX);
					r = read(f, &dbl.descrs, SIZEOF_HFS_HDR_DESCR * 
							 ntohs(dbl.entries));
					if (r != SIZEOF_HFS_HDR_DESCR * ntohs(dbl.entries)) {
						r = -1;
						break;
					}
					for (i = 0; i < ntohs(dbl.entries); i++) {
						struct hfs_hdr_descr *descr = (struct hfs_hdr_descr *)
							(&dbl.descrs[SIZEOF_HFS_HDR_DESCR * i]);
						if (ntohl(descr->id) == HFS_HDR_FINFO) {
							if (lseek(f, ntohl(descr->offset), SEEK_SET) != 
								(off_t)ntohl(descr->offset))
								continue;
							r = 8;
							r = read(f, buf, 8);
							break;
						}
					}
				}
				break;
		}
		close(f);
		if (r == 8)
			return;
	}

use_suffix:
	suffix_type_creator(buf, path);
}

void
hfsinfo_read (const char *path, struct hfsinfo *fi)
{
	char infopath[MAXPATHLEN];
	int f, i, r;
	struct stat sb;
	union {
		struct hfs_cap_info cap;
		struct hfs_dbl_hdr dbl;
	} hdr;

	memset(fi, 0, sizeof(struct hfsinfo));
	if (!finderinfo_path(infopath, path, &sb)) {
		f = open(infopath, O_RDONLY);
		if (f >= 0) {
			switch (cfg.fork) {
				case HFS_FORK_CAP:
					r = read(f, &hdr.cap, SIZEOF_HFS_CAP_INFO);
					if (r != SIZEOF_HFS_CAP_INFO)
						break;
					memcpy(fi->type, hdr.cap.fi_fndr, 8);
					if (hdr.cap.fi_datevalid & HFS_CAP_CDATE)
						memcpy(&fi->create_time, hdr.cap.fi_ctime, 4);
					else
						fi->create_time = 0;
					if (hdr.cap.fi_datevalid & HFS_CAP_MDATE)
						memcpy(&fi->modify_time, hdr.cap.fi_mtime, 4);
					else
						fi->modify_time = 0;
					fi->comlen = hdr.cap.fi_comln > 200 ? 200 : 
						hdr.cap.fi_comln;
					memcpy(fi->comment, hdr.cap.fi_comnt, fi->comlen);
					break;
				case HFS_FORK_NETATALK:
				case HFS_FORK_DOUBLE:
					r = read(f, &hdr.dbl, SIZEOF_HFS_DBL_HDR);
					if (r != SIZEOF_HFS_DBL_HDR)
						break;
					if (ntohs(hdr.dbl.entries) > HFS_HDR_MAX)
						hdr.dbl.entries = htons(HFS_HDR_MAX);
					r = read(f, &hdr.dbl.descrs, SIZEOF_HFS_HDR_DESCR * 
							 ntohs(hdr.dbl.entries));
					if (r != SIZEOF_HFS_HDR_DESCR * ntohs(hdr.dbl.entries))
						break;
					for (i = 0; i < ntohs(hdr.dbl.entries); i++) {
						struct hfs_hdr_descr *descr = (struct hfs_hdr_descr *)
							(&hdr.dbl.descrs[SIZEOF_HFS_HDR_DESCR * i]);
						if (lseek(f, ntohl(descr->offset), SEEK_SET) != 
							(off_t)ntohl(descr->offset))
							continue;
						switch (ntohl(descr->id)) {
							case HFS_HDR_COMNT:
								fi->comlen = ntohl(descr->length) > 200 ? 200
									: ntohl(descr->length);
								r = read(f, fi->comment, fi->comlen);
								if (r != (int)fi->comlen)
									fi->comlen = 0;
								break;
							case HFS_HDR_OLDI:
							case HFS_HDR_DATES:
								r = read(f, &fi->create_time, 8);
								if (r != 8)
									fi->create_time = fi->modify_time = 0;
								break;
							case HFS_HDR_FINFO:
								r = read(f, fi->type, 8);
								if (r != 8)
									memset(fi->type, 0, 8);
								break;
							case HFS_HDR_RSRC:
								fi->rsrclen = ntohl(descr->length);
								break;
						}
					}
					break;
			}
			close(f);
		}
	}

	if (!(*(u_int32_t *)(&fi->type[0])) || !(*(u_int32_t *)(&fi->type[4])))
		suffix_type_creator(fi->type, path);
	if (!fi->create_time || !fi->modify_time) {
		if (!stat(path, &sb)) {
			u_int32_t htime = hfs_u_to_htime(sb.st_mtime);
			if (!fi->create_time)
				fi->create_time = htime;
			if (!fi->modify_time)
				fi->modify_time = htime;
		}
	}
	if (!fi->comlen && cfg.comment) {
		fi->comlen = strlen(cfg.comment);
		if (fi->comlen > 200)
			fi->comlen = 200;
		memcpy(fi->comment, cfg.comment, fi->comlen);
	}
}

void
hfsinfo_write (const char *path, struct hfsinfo *fi)
{
	char infopath[MAXPATHLEN];
	int f, i, r;
	union {
		struct hfs_cap_info cap;
		struct hfs_dbl_hdr dbl;
	} hdr;

	if (!finderinfo_path(infopath, path, 0)) {
		f = open(infopath, O_RDWR|O_CREAT, cfg.file_perm);
		if (f >= 0) {
			switch (cfg.fork) {
				case HFS_FORK_CAP:
					memset(&hdr.cap, 0, sizeof(struct hfs_cap_info));
					hdr.cap.fi_magic1 = HFS_CAP_MAGIC1;
					hdr.cap.fi_version = HFS_CAP_VERSION;
					hdr.cap.fi_magic = HFS_CAP_MAGIC;
					hdr.cap.fi_datemagic = HFS_CAP_DMAGIC;
					hdr.cap.fi_datevalid = HFS_CAP_MDATE|HFS_CAP_CDATE;
					memcpy(hdr.cap.fi_fndr, fi->type, 8);
					memcpy(hdr.cap.fi_ctime, &fi->create_time, 8);
					hdr.cap.fi_comln = fi->comlen > 200 ? 200 : fi->comlen;
					memcpy(hdr.cap.fi_comnt, fi->comment, hdr.cap.fi_comln);
					write(f, &hdr.cap, SIZEOF_HFS_CAP_INFO);
					break;
				case HFS_FORK_NETATALK:
				case HFS_FORK_DOUBLE:
					r = read(f, &hdr.dbl, SIZEOF_HFS_DBL_HDR);
					if (r != SIZEOF_HFS_DBL_HDR) {
						struct hfs_hdr_descr *descr;

#define NENTRIES 4
						hdr.dbl.entries = htons(NENTRIES);
						descr = (struct hfs_hdr_descr *)
							(&hdr.dbl.descrs[SIZEOF_HFS_HDR_DESCR * 0]);
						descr->id = htonl(HFS_HDR_COMNT);
						descr->offset = htonl(SIZEOF_HFS_DBL_HDR + 
											  SIZEOF_HFS_HDR_DESCR * NENTRIES);
						descr->length = htonl(fi->comlen);
						descr = (struct hfs_hdr_descr *)
							(&hdr.dbl.descrs[SIZEOF_HFS_HDR_DESCR * 1]);
						descr->id = htonl(HFS_HDR_DATES);
						descr->offset = htonl(SIZEOF_HFS_DBL_HDR + 
											  SIZEOF_HFS_HDR_DESCR * NENTRIES 
											  + fi->comlen);
						descr->length = htonl(8);
						descr = (struct hfs_hdr_descr *)
							(&hdr.dbl.descrs[SIZEOF_HFS_HDR_DESCR * 2]);
						descr->id = htonl(HFS_HDR_FINFO);
						descr->offset = htonl(SIZEOF_HFS_DBL_HDR + 
											  SIZEOF_HFS_HDR_DESCR * NENTRIES
											  + fi->comlen + 8);
						descr->length = htonl(8);
						descr = (struct hfs_hdr_descr *)
							(&hdr.dbl.descrs[SIZEOF_HFS_HDR_DESCR * 3]);
						descr->id = htonl(HFS_HDR_RSRC);
						descr->offset = htonl(SIZEOF_HFS_DBL_HDR + 
											  SIZEOF_HFS_HDR_DESCR * NENTRIES
											  + fi->comlen + 8 + 8);
						descr->length = htonl(0);
					} else {
						if (ntohs(hdr.dbl.entries) > HFS_HDR_MAX)
							hdr.dbl.entries = htons(HFS_HDR_MAX);
						r = read(f, &hdr.dbl.descrs, SIZEOF_HFS_HDR_DESCR *
								 ntohs(hdr.dbl.entries));
						if (r != SIZEOF_HFS_HDR_DESCR * ntohs(hdr.dbl.entries))
							break;
					}
					for (i = 0; i < ntohs(hdr.dbl.entries); i++) {
						struct hfs_hdr_descr *descr = (struct hfs_hdr_descr *)
							(&hdr.dbl.descrs[SIZEOF_HFS_HDR_DESCR * i]);
						if (lseek(f, ntohl(descr->offset), SEEK_SET) != 
							(off_t)ntohl(descr->offset))
							continue;
						switch (ntohl(descr->id)) {
							case HFS_HDR_COMNT:
								if (r == SIZEOF_HFS_DBL_HDR)
									break;
								descr->length = htonl(fi->comlen);
								write(f, fi->comment, fi->comlen);
								break;
							case HFS_HDR_OLDI:
							case HFS_HDR_DATES:
								if (descr->length < 8)
									descr->length = 8;
								write(f, &fi->create_time, 8);
								break;
							case HFS_HDR_FINFO:
								if (descr->length < 8)
									descr->length = 8;
								write(f, fi->type, 8);
								break;
							case HFS_HDR_RSRC:
								descr->length = htonl(fi->rsrclen);
								break;
						}
					}
					hdr.dbl.magic = htonl(HFS_DBL_MAGIC);
					if (cfg.fork == HFS_FORK_NETATALK)
						hdr.dbl.version = htonl(HFS_HDR_VERSION_1);
					else
						hdr.dbl.version = htonl(HFS_HDR_VERSION_2);
					lseek(f, 0, SEEK_SET);
					write(f, &hdr.dbl, SIZEOF_HFS_DBL_HDR + 
						  SIZEOF_HFS_HDR_DESCR * ntohs(hdr.dbl.entries));
					break;
			}
			fsync(f);
			close(f);
		}
	}
}

size_t comment_len (const char *path)
{
	char infopath[MAXPATHLEN];
	int f, i, r, len = 0;
	struct stat sb;
	union {
		struct hfs_cap_info cap;
		struct hfs_dbl_hdr dbl;
	} hdr;

	if (!finderinfo_path(infopath, path, &sb)) {
		f = open(infopath, O_RDONLY);
		if (f >= 0) {
			switch (cfg.fork) {
				case HFS_FORK_CAP:
					if (read(f, &hdr.cap, SIZEOF_HFS_CAP_INFO) == 
						SIZEOF_HFS_CAP_INFO)
						len = hdr.cap.fi_comln > 200 ? 200 : hdr.cap.fi_comln;
					break;
				case HFS_FORK_NETATALK:
				case HFS_FORK_DOUBLE:
					r = read(f, &hdr.dbl, SIZEOF_HFS_DBL_HDR);
					if (r != SIZEOF_HFS_DBL_HDR)
						break;
					if (ntohs(hdr.dbl.entries) > HFS_HDR_MAX)
						hdr.dbl.entries = htons(HFS_HDR_MAX);
					r = read(f, &hdr.dbl.descrs, SIZEOF_HFS_HDR_DESCR * 
							 ntohs(hdr.dbl.entries));
					if (r != SIZEOF_HFS_HDR_DESCR * ntohs(hdr.dbl.entries))
						break;
					for (i = 0; i < ntohs(hdr.dbl.entries); i++) {
						struct hfs_hdr_descr *descr = (struct hfs_hdr_descr *)
							(&hdr.dbl.descrs[SIZEOF_HFS_HDR_DESCR * i]);
						if (ntohl(descr->id) == HFS_HDR_COMNT)
							len = ntohl(descr->length) > 200 ? 200 : 
								ntohl(descr->length);
					}
					break;
			}
			close(f);
		}
	}
	if (!len && cfg.comment) {
		len = strlen(cfg.comment);
		if (len > 200)
			len = 200;
	}

	return len;
}

void
comment_write (const char *path, char *comment, int comlen)
{
	char infopath[MAXPATHLEN];
	int f;
	union {
		struct hfs_cap_info cap;
		struct hfs_dbl_hdr dbl;
	} hdr;
	if (!finderinfo_path(infopath, path, 0)) {
		f = open(infopath, O_RDWR|O_CREAT, cfg.file_perm);
		if (f >= 0) {
			if (comlen > 200)
				comlen = 200;
			switch (cfg.fork) {
				case HFS_FORK_CAP:
					if (read(f, &hdr.cap, SIZEOF_HFS_CAP_INFO) != 
						SIZEOF_HFS_CAP_INFO) {
						
						memset(&hdr.cap, 0, sizeof(struct hfs_cap_info));
						hdr.cap.fi_magic1 = HFS_CAP_MAGIC1;
						hdr.cap.fi_version = HFS_CAP_VERSION;
						hdr.cap.fi_magic = HFS_CAP_MAGIC;
						hdr.cap.fi_datemagic = HFS_CAP_DMAGIC;
						suffix_type_creator(hdr.cap.fi_fndr, path);
					}
					hdr.cap.fi_comln = comlen;
					memcpy(hdr.cap.fi_comnt, comment, comlen);
					lseek(f, 0, SEEK_SET);
					write(f, &hdr.cap, SIZEOF_HFS_CAP_INFO);
					break;			}
			fsync(f);
			close(f);
		}
	}
}
