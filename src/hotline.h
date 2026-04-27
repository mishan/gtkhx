#ifndef _HOTLINE_H
#define _HOTLINE_H

#if defined(__GNUC__) && !defined(__STRICT_ANSI__)
#define ZERO_SIZE_ARRAY_SIZE	0
#else
#define ZERO_SIZE_ARRAY_SIZE	1
#endif

struct hl_hdr {
	guint32	type PACKED,
			trans PACKED;
	guint32 flag PACKED;
	guint32	len PACKED,
			len2 PACKED;
	guint16	hc PACKED;
	guint8	data[ZERO_SIZE_ARRAY_SIZE] PACKED;
};

struct hl_data_hdr {
	guint16	type PACKED,
			len PACKED;
	guint8	data[ZERO_SIZE_ARRAY_SIZE] PACKED;
};

struct htxf_hdr {
	guint32	magic,
			ref,
			len,
			unknown;
};

struct hl_filelist_hdr {
	guint16	type PACKED,
			len PACKED;
	guint32	ftype PACKED,
			fcreator PACKED;
	guint32	fsize PACKED,
			unknown PACKED,
			fnlen PACKED;
	guint8	fname[ZERO_SIZE_ARRAY_SIZE] PACKED;
};

struct hl_userlist_hdr {
	guint16	type PACKED,
			len PACKED;
	guint16	uid PACKED,
			icon PACKED,
			color PACKED,
			nlen PACKED;
	guint8	name[ZERO_SIZE_ARRAY_SIZE] PACKED;
};

struct htrk_hdr {
	guint16 version;
	guint16 port;
	guint16 nusers;
	guint16 __reserved0;
	guint32 id;
};

typedef guint64 hl_access_bits;

struct hl_user_data {
	guint32 magic;
	hl_access_bits access;
	guint8 pad[516];
	guint16 nlen;
	guint8 name[134];
	guint16 llen;
	guint8 login[34];
	guint16 plen;
	guint8 password[32];
};

#define HTLC_MAGIC	"TRTPHOTL\0\1\0\2"
#define HTLC_MAGIC_LEN	12
#define HTLS_MAGIC	"TRTP\0\0\0\0"
#define HTLS_MAGIC_LEN	8
#define HTRK_MAGIC	"HTRK\0\1"
#define HTRK_MAGIC_LEN	6
#define HTXF_MAGIC	"HTXF"
#define HTXF_MAGIC_LEN	4
#define HTXF_MAGIC_INT	0x48545846

#define HTRK_TCPPORT	5498
#define HTRK_UDPPORT	5499
#define HTLS_TCPPORT	5500
#define HTXF_TCPPORT	5501

#define HTLC_HDR_NEWS_GETFILE		((guint32) 0x00000065)
#define HTLC_HDR_NEWS_POST		((guint32) 0x00000067)
#define HTLC_HDR_CHAT			((guint32) 0x00000069)
#define HTLC_HDR_LOGIN			((guint32) 0x0000006b)
#define HTLC_HDR_MSG			((guint32) 0x0000006c)
#define HTLC_HDR_USER_KICK		((guint32) 0x0000006e)
#define HTLC_HDR_CHAT_CREATE		((guint32) 0x00000070)
#define HTLC_HDR_CHAT_INVITE		((guint32) 0x00000071)
#define HTLC_HDR_CHAT_DECLINE		((guint32) 0x00000072)
#define HTLC_HDR_CHAT_JOIN		((guint32) 0x00000073)
#define HTLC_HDR_CHAT_PART		((guint32) 0x00000074)
#define HTLC_HDR_CHAT_SUBJECT		((guint32) 0x00000078)
#define HTLC_HDR_FILE_LIST		((guint32) 0x000000c8)
#define HTLC_HDR_FILE_GET		((guint32) 0x000000ca)
#define HTLC_HDR_FILE_PUT		((guint32) 0x000000cb)
#define HTLC_HDR_FILE_DELETE		((guint32) 0x000000cc)
#define HTLC_HDR_FILE_MKDIR		((guint32) 0x000000cd)
#define HTLC_HDR_FILE_GETINFO		((guint32) 0x000000ce)
#define HTLC_HDR_FILE_SETINFO		((guint32) 0x000000cf)
#define HTLC_HDR_FILE_MOVE		((guint32) 0x000000d0)
#define HTLC_HDR_FILE_SYMLINK		((guint32) 0x000000d1)
#define HTLC_HDR_USER_GETLIST		((guint32) 0x0000012c)
#define HTLC_HDR_USER_GETINFO		((guint32) 0x0000012f)
#define HTLC_HDR_USER_CHANGE		((guint32) 0x00000130)
#define HTLC_HDR_ACCOUNT_CREATE		((guint32) 0x0000015e)
#define HTLC_HDR_ACCOUNT_DELETE		((guint32) 0x0000015f)
#define HTLC_HDR_ACCOUNT_READ		((guint32) 0x00000160)
#define HTLC_HDR_ACCOUNT_MODIFY		((guint32) 0x00000161)
#define HTLC_HDR_MSG_BROADCAST		((guint32) 0x00000163)

/* 1.5+ Only */
#define HTLC_HDR_NEWSDIRLIST   ((guint32) 0x00000172)
#define HTLC_HDR_NEWSCATLIST   ((guint32) 0x00000173)
#define HTLC_HDR_DELNEWSDIRCAT ((guint32) 0x0000017c)
#define HTLC_HDR_MAKENEWSDIR   ((guint32) 0x0000017d)
#define HTLC_HDR_MAKECATEGORY  ((guint32) 0x0000017e)
#define HTLC_HDR_GETTHREAD     ((guint32) 0x00000190)
#define HTLC_HDR_POSTTHREAD    ((guint32) 0x0000019a)
#define HTLC_HDR_DELETETHREAD  ((guint32) 0x0000019b)

#define HTLC_DATA_CHAT			((guint16) 0x0065)
#define HTLC_DATA_MSG			((guint16) 0x0065)
#define HTLC_DATA_NEWS_POST		((guint16) 0x0065)
#define HTLC_DATA_NAME			((guint16) 0x0066)
#define HTLC_DATA_UID			((guint16) 0x0067)
#define HTLC_DATA_ICON			((guint16) 0x0068)
#define HTLC_DATA_LOGIN			((guint16) 0x0069)
#define HTLC_DATA_PASSWORD		((guint16) 0x006a)
#define HTLC_DATA_HTXF_SIZE		((guint16) 0x006c)
#define HTLC_DATA_STYLE			((guint16) 0x006d)
#define HTLC_DATA_ACCESS		((guint16) 0x006e)
#define HTLC_DATA_BAN			((guint16) 0x0071)
#define HTLC_DATA_CHAT_ID		((guint16) 0x0072)
#define HTLC_DATA_CHAT_SUBJECT		((guint16) 0x0073)
#define HTLC_DATA_FILE_NAME		((guint16) 0x00c9)
#define HTLC_DATA_DIR			((guint16) 0x00ca)
#define HTLC_DATA_RFLT			((guint16) 0x00cb)
#define HTLC_DATA_FILE_PREVIEW		((guint16) 0x00cc)
#define HTLC_DATA_FILE_COMMENT		((guint16) 0x00d2)
#define HTLC_DATA_FILE_RENAME		((guint16) 0x00d3)
#define HTLC_DATA_DIR_RENAME		((guint16) 0x00d4)


#define HTLC_DATA_NEWSFOLDERITEM ((guint16) 0x0140)
#define HTLC_DATA_CATLIST        ((guint16) 0x0141)
#define HTLC_DATA_CATEGORY       ((guint16) 0x0142)
#define HTLC_DATA_CATEGORYITEM   ((guint16) 0x0143)
#define HTLC_DATA_NEWSPATH       ((guint16) 0x0145)
#define HTLC_DATA_THREADID       ((guint16) 0x0146)
#define HTLC_DATA_NEWSTYPE       ((guint16) 0x0147)
#define HTLC_DATA_NEWSSUBJECT    ((guint16) 0x0148)
#define HTLC_DATA_NEWSAUTHOR     ((guint16) 0x0149)
#define HTLC_DATA_NEWSDATE       ((guint16) 0x014a)
#define HTLC_DATA_NEWSDIR        ((guint16) 0x01c8) 

#define HTLC_DATA_PREVTHREAD     ((guint16) 0x014b)
#define HTLC_DATA_NEXTTHREAD     ((guint16) 0x014c)
#define HTLC_DATA_NEWSDATA       ((guint16) 0x014d)
#define HTLC_DATA_PARENTTHREAD   ((guint16) 0x014e)
#define HTLC_DATA_PARENT_POST    ((guint16) 0x014f)
#define HTLC_DATA_CHILD_POST     ((guint16)  0x0150)


#define HTLS_HDR_NEWS_POST		((guint32) 0x00000066)
#define HTLS_HDR_MSG			((guint32) 0x00000068)
#define HTLS_HDR_CHAT			((guint32) 0x0000006a)
#define HTLS_HDR_AGREEMENT		((guint32) 0x0000006d)
#define HTLS_HDR_POLITEQUIT		((guint32) 0x0000006f)
#define HTLS_HDR_CHAT_INVITE		((guint32) 0x00000071)
#define HTLS_HDR_CHAT_USER_CHANGE	((guint32) 0x00000075)
#define HTLS_HDR_CHAT_USER_PART		((guint32) 0x00000076)
#define HTLS_HDR_CHAT_SUBJECT		((guint32) 0x00000077)
#define HTLS_HDR_USER_CHANGE		((guint32) 0x0000012d)
#define HTLS_HDR_USER_PART		((guint32) 0x0000012e)
#define HTLS_HDR_USER_SELFINFO		((guint32) 0x00000162)
#define HTLS_HDR_MSG_BROADCAST		((guint32) 0x00000163)
#define HTLS_HDR_TASK			((guint32) 0x00010000)
#define HTLS_HDR_QUEUE			((guint32) 0x000000d3)

#define HTLS_DATA_TASKERROR		((guint16) 0x0064)
#define HTLS_DATA_NEWS			((guint16) 0x0065)
#define HTLS_DATA_AGREEMENT		((guint16) 0x0065)
#define HTLS_DATA_USER_INFO		((guint16) 0x0065)
#define HTLS_DATA_CHAT			((guint16) 0x0065)
#define HTLS_DATA_MSG			((guint16) 0x0065)
#define HTLS_DATA_NAME			((guint16) 0x0066)
#define HTLS_DATA_UID			((guint16) 0x0067)
#define HTLS_DATA_ICON			((guint16) 0x0068)
#define HTLS_DATA_LOGIN			((guint16) 0x0069)
#define HTLS_DATA_PASSWORD		((guint16) 0x006a)
#define HTLS_DATA_HTXF_REF		((guint16) 0x006b)
#define HTLS_DATA_HTXF_SIZE		((guint16) 0x006c)
#define HTLS_DATA_STYLE			((guint16) 0x006d)
#define HTLS_DATA_ACCESS		((guint16) 0x006e)
#define HTLS_DATA_COLOUR		((guint16) 0x0070)
#define HTLS_DATA_CHAT_ID		((guint16) 0x0072)
#define HTLS_DATA_CHAT_SUBJECT		((guint16) 0x0073)
#define HTLS_DATA_FILE_LIST		((guint16) 0x00c8)
#define HTLS_DATA_FILE_NAME		((guint16) 0x00c9)
#define HTLS_DATA_RFLT			((guint16) 0x00cb)
#define HTLS_DATA_FILE_TYPE		((guint16) 0x00cd)
#define HTLS_DATA_FILE_CREATOR		((guint16) 0x00ce)
#define HTLS_DATA_FILE_SIZE		((guint16) 0x00cf)
#define HTLS_DATA_FILE_DATE_CREATE	((guint16) 0x00d0)
#define HTLS_DATA_FILE_DATE_MODIFY	((guint16) 0x00d1)
#define HTLS_DATA_FILE_COMMENT		((guint16) 0x00d2)
#define HTLS_DATA_FILE_ICON		((guint16) 0x00d5)
#define HTLS_DATA_FILE_NFILES		((guint16) 0x00dc)
#define HTLS_DATA_USER_LIST		((guint16) 0x012c)

/* 1.5+ only */
#define HTLS_DATA_QUEUE			((guint16) 0x0074)
#define HTLS_DATA_VERSION ((guint16) 0x00a0)
#define HTLS_DATA_SERVERNAME ((guint16) 0x00a2)
#define HTLS_DATA_NOAGREEMENT    ((guint16) 0x009a)

#define HTLC_HDR_ICON_GET		((guint32) 0x00000e90)
#define HTLC_HDR_FILE_HASH		((guint32) 0x00000ee0)

#define HTLC_DATA_HASH_MD5		((guint16) 0x0e80)
#define HTLC_DATA_HASH_HAVAL		((guint16) 0x0e81)
#define HTLC_DATA_HASH_SHA1		((guint16) 0x0e82)
#define HTLC_DATA_CHAT_AWAY		((guint16) 0x0ea1)

#define HTLS_DATA_HASH_MD5		((guint16) 0x0e80)
#define HTLS_DATA_HASH_HAVAL		((guint16) 0x0e81)
#define HTLS_DATA_HASH_SHA1		((guint16) 0x0e82)
#define HTLS_DATA_ICON_CICN		((guint16) 0x0e90)

/* HOPE */
#define HTLS_DATA_SESSIONKEY		((guint16) 0x0e03)
#define HTLC_DATA_SESSIONKEY		((guint16) 0x0e03)
#define HTLS_DATA_MAC_ALG		((guint16) 0x0e04)
#define HTLC_DATA_MAC_ALG		((guint16) 0x0e04)

/* cipher */
#define HTLS_DATA_CIPHER_ALG		((u_int16_t) 0x0ec1)
#define HTLC_DATA_CIPHER_ALG		((u_int16_t) 0x0ec2)
#define HTLS_DATA_CIPHER_MODE		((u_int16_t) 0x0ec3)
#define HTLC_DATA_CIPHER_MODE		((u_int16_t) 0x0ec4)
#define HTLS_DATA_CIPHER_IVEC		((u_int16_t) 0x0ec5)
#define HTLC_DATA_CIPHER_IVEC		((u_int16_t) 0x0ec6)

/* compress */
#define HTLS_DATA_CHECKSUM_ALG		((guint16) 0x0ec7)
#define HTLC_DATA_CHECKSUM_ALG		((guint16) 0x0ec8)
#define HTLS_DATA_COMPRESS_ALG		((guint16) 0x0ec9)
#define HTLC_DATA_COMPRESS_ALG		((guint16) 0x0eca)

#define SIZEOF_HL_HDR		(22)
#define SIZEOF_HL_DATA_HDR	(4)
#define SIZEOF_HL_FILELIST_HDR	(24)
#define SIZEOF_HL_USERLIST_HDR	(12)
#define SIZEOF_HTXF_HDR		(16)
#define SIZEOF_HTRK_HDR		(12)

#endif /* ndef _HOTLINE_H */
