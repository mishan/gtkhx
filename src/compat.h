/*
 * compat.h — portability shims and build-time macros.
 *
 * No struct definitions and no project-internal includes; this header is
 * pulled in by everything else (including protocol.h and prefs.h), so it
 * needs to stay leaf-level. Anything that depends on GLib types lives in
 * protocol.h instead.
 */

#ifndef __gtkhx_COMPAT_H
#define __gtkhx_COMPAT_H 1

#include <stdlib.h>		/* strtoul for atou32/atou16 */

#ifdef __GNUC__
#define PACKED __attribute__((__packed__))
#else
#define PACKED
#endif

#if !defined(__GNUC__) || defined(__STRICT_ANSI__) || defined(__APPLE_CC__)
#define __attribute__(x)
#endif

#if !defined(__va_copy)
#define __va_copy(_dst, _src) ((_dst) = (_src))
#endif

#ifndef RETSIGTYPE
#define RETSIGTYPE void
#endif

#define HOSTLEN			256
#define MAX_HOTLINE_PACKET_LEN	0x100000
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

#if !defined(HAVE_INET_NTOA_R)
#include <sys/types.h>
#include <netinet/in.h>		/* struct in_addr */
extern int inet_ntoa_r (struct in_addr in, char *buf, size_t buflen);
#endif

#ifdef HAVE_DCGETTEXT
#include <libintl.h>
#define _(string) dgettext (PACKAGE, string)
#else
#define _(string) (string)
#endif

#define atou32(_str) ((guint32)strtoul(_str, 0, 0))
#define atou16(_str) ((guint16)strtoul(_str, 0, 0))

/* In-place character substitution. Used for CR<->LF conversion at the
 * Hotline wire boundary (the protocol uses CR; we use LF internally). */
#define X2X(_ptr, _len, _x1, _x2)		\
do {						\
	char *_p = _ptr, *_end = _ptr + _len;	\
	for ( ; _p < _end; _p++)		\
		if (*_p == _x1)			\
			*_p = _x2;		\
} while (0)

#define CR2LF(_ptr, _len)	X2X(_ptr, _len, '\r', '\n')
#define LF2CR(_ptr, _len)	X2X(_ptr, _len, '\n', '\r')

/* ANSI color escapes (used in console-style output paths). */
#define WHITE		"\033[0;37m"
#define WHITE_BOLD	"\033[1;37m"
#define RED		"\033[0;31m"
#define RED_BOLD	"\033[1;31m"

/* Phase 3.10: GdkRGBA initializer from a 16-bit-per-channel literal.
 * Used to express the historic Mac/IRC color values (which were
 * naturally 16-bit per channel under GdkColor) without manually
 * re-computing each fraction. Always opaque (alpha=1). The divisions
 * are constant expressions and fold at compile time. */
#define RGB16(r, g, b) { (r) / 65535.0, (g) / 65535.0, (b) / 65535.0, 1.0 }

#ifdef USE_DEBUG
#define debug(fmt,args...) {printf("%s:%d: ",__FILE__,__LINE__);printf(fmt,##args);fflush(stdout);}
#else
#define debug(args...)
#endif

#endif /* ndef __gtkhx_COMPAT_H */
