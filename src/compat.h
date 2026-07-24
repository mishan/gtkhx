/*
 * compat.h — portability shims and build-time macros.
 *
 * No struct definitions and no project-internal includes; this header is
 * pulled in by everything else (including protocol.h and prefs.h), so it
 * needs to stay leaf-level. Anything that depends on GLib types lives in
 * protocol.h instead.
 */

#ifndef GTKHX_COMPAT_H
#define GTKHX_COMPAT_H 1

#include <stdlib.h> /* strtoul for atou32/atou16 */

#ifdef __GNUC__
#define PACKED __attribute__ ((__packed__))
#else
#define PACKED
#endif

/*
 * Never define away `__attribute__` for "compatibility": every compiler
 * we build with (GCC, Clang, Apple Clang) supports it, the identifier is
 * reserved so redefining it is undefined behavior, and because this
 * header is included before system headers the empty definition silently
 * corrupts THEIR attributes too. On macOS it stripped
 * neon_vector_type(N) from arm_neon.h (breaking every GTK-including
 * compile via graphene) and __packed__ from our own wire structs.
 */

#if !defined(__va_copy)
#define __va_copy(_dst, _src) ((_dst) = (_src))
#endif

#ifndef RETSIGTYPE
#define RETSIGTYPE void
#endif

/* fsync(2): flush a file's data to disk. The Windows CRT spells it
 * _commit(); everywhere else it's POSIX fsync. Takes an OS file
 * descriptor on both. */
#ifdef _WIN32
#include <io.h>
static inline int hx_fsync (int fd) { return _commit (fd); }
#else
#include <unistd.h>
static inline int hx_fsync (int fd) { return fsync (fd); }
#endif

#define HOSTLEN 256
#define MAX_HOTLINE_PACKET_LEN 0x100000
#define UNKNOWN_TYPECREA "TEXTR*ch"

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

/* libintl provides dgettext on every platform we care about. Older
 * autotools layouts checked HAVE_DCGETTEXT (set by AM_GNU_GETTEXT)
 * before pulling libintl in, but meson detects libintl directly via
 * intl_dep + HAVE_LIBINTL_H, so we gate on the header check that's
 * actually performed at configure time. Identity fallback is kept
 * for the unusual case where libintl isn't present at all. */
#ifdef HAVE_LIBINTL_H
#include <libintl.h>
#define _(string) dgettext (PACKAGE, string)
#else
#define _(string) (string)
#endif

#define atou32(_str) ((guint32)strtoul ((_str), 0, 0))
#define atou16(_str) ((guint16)strtoul ((_str), 0, 0))

/* In-place character substitution. Used for CR<->LF conversion at the
 * Hotline wire boundary (the protocol uses CR; we use LF internally). */
#define X2X(_ptr, _len, _x1, _x2)                                              \
    do {                                                                       \
        char *_p = (_ptr), *_end = (_ptr) + (_len);                            \
        for (; _p < _end; _p++)                                                \
            if (*_p == (_x1))                                                  \
                *_p = (_x2);                                                   \
    } while (0)

#define CR2LF(_ptr, _len) X2X (_ptr, _len, '\r', '\n')
#define LF2CR(_ptr, _len) X2X (_ptr, _len, '\n', '\r')

/* ANSI color escapes (used in console-style output paths). */
#define WHITE "\033[0;37m"
#define WHITE_BOLD "\033[1;37m"
#define RED "\033[0;31m"
#define RED_BOLD "\033[1;31m"

/* GdkRGBA initializer from a 16-bit-per-channel literal.
 * Used to express the historic Mac/IRC color values (which were
 * naturally 16-bit per channel under GdkColor) without manually
 * re-computing each fraction. Always opaque (alpha=1). The divisions
 * are constant expressions and fold at compile time. */
#define RGB16(r, g, b) { (r) / 65535.0, (g) / 65535.0, (b) / 65535.0, 1.0 }

#ifdef USE_DEBUG
#define debug(fmt, args...)                                                    \
    {                                                                          \
        printf ("%s:%d: ", __FILE__, __LINE__);                                \
        printf (fmt, ##args);                                                  \
        fflush (stdout);                                                       \
    }
#else
#define debug(args...)
#endif

#endif /* ndef GTKHX_COMPAT_H */
