/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

/*
 * TLS Phase 3 trust DB — SHA-256 fingerprint cache in SSH
 * known_hosts shape. See tls_trust.h for the API rationale and
 * the GTKHX_KNOWN_HOSTS env-var override.
 *
 * File format (one entry per non-comment line):
 *
 *   <host>[:port] sha256:<64-hex-digits> # added <ISO-8601 date>
 *
 * Examples:
 *
 *   hotline.vespernet.net:5600 sha256:c4a02f...3f1d # added 2026-05-28
 *   localhost:5610 sha256:9b1de4...0a77 # added 2026-05-28
 *
 * Empty lines and lines starting with `#` are preserved on
 * round-trip so a hand-edit doesn't get clobbered by the next
 * pin. The port is optional in the lookup — if a hostname-only
 * line matches, every port reuses it. (We always WRITE host:port
 * so this only matters for hand-authored entries.)
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "tls_trust.h"
#include "host_port.h"

/* The TLS-trust dialog moved to Rust (gtkhx-ui tls_trust_dialog.rs) and
 * branches on the hx_tls_trust_status value it receives across the ABI as a
 * plain int, mirrored in rust/crates/gtkhx-ui/src/ffi.rs. Pin the C enum's
 * numeric values here so a reorder/extension breaks the build loudly instead
 * of silently diverging from the Rust side. */
_Static_assert (HX_TLS_TRUST_TRUSTED == 0 && HX_TLS_TRUST_UNKNOWN == 1
                    && HX_TLS_TRUST_MISMATCH == 2,
                "hx_tls_trust_status values are mirrored as HX_TLS_TRUST_* in "
                "rust/crates/gtkhx-ui/src/ffi.rs; keep them in sync");

/* Forward-declared in network.c / bookmarks_io.c — don't pull
 * the whole gtkhx.h pile in. The path resolver caches and lives
 * in gtkhx.c. */
extern const char *gtkhx_config_dir (void);

/* ---- Thread-safe test seams (see tls_trust.h) ------------------ */

/* tri-state: <0 unset (consult env), 0 force off, >0 force on. */
static gint hx_tls_test_auto_accept_ov = -1;
static gint hx_tls_test_force_tls_ov = -1;
/* prompt: 0 unset (consult env), 1 accept, 2 reject. */
static gint hx_tls_test_prompt_ov = 0;
/* GMutex may be statically zero-initialised and used without g_mutex_init. */
static GMutex hx_tls_test_known_hosts_mutex;
static char *hx_tls_test_known_hosts_ov = NULL; /* guarded by the mutex */

void
hx_tls_test_set_auto_accept (int tri)
{
    g_atomic_int_set (&hx_tls_test_auto_accept_ov, tri);
}

void
hx_tls_test_set_force_tls (int tri)
{
    g_atomic_int_set (&hx_tls_test_force_tls_ov, tri);
}

void
hx_tls_test_set_prompt_verdict (int verdict)
{
    g_atomic_int_set (&hx_tls_test_prompt_ov, verdict);
}

void
hx_tls_test_set_known_hosts (const char *path)
{
    g_mutex_lock (&hx_tls_test_known_hosts_mutex);
    g_free (hx_tls_test_known_hosts_ov);
    hx_tls_test_known_hosts_ov = path ? g_strdup (path) : NULL;
    g_mutex_unlock (&hx_tls_test_known_hosts_mutex);
}

gboolean
hx_tls_test_auto_accept (void)
{
    gint o = g_atomic_int_get (&hx_tls_test_auto_accept_ov);
    if (o >= 0) {
        return o != 0;
    }
    const char *e = g_getenv ("GTKHX_TLS_AUTO_ACCEPT");
    return e && *e;
}

gboolean
hx_tls_test_force_tls (void)
{
    gint o = g_atomic_int_get (&hx_tls_test_force_tls_ov);
    if (o >= 0) {
        return o != 0;
    }
    const char *e = g_getenv ("GTKHX_TLS");
    return e && *e;
}

int
hx_tls_test_prompt_verdict (void)
{
    gint o = g_atomic_int_get (&hx_tls_test_prompt_ov);
    if (o != 0) {
        return o; /* 1 accept, 2 reject */
    }
    const char *e = g_getenv ("GTKHX_TLS_TEST_PROMPT");
    if (e && g_ascii_strcasecmp (e, "accept") == 0) {
        return 1;
    }
    if (e && g_ascii_strcasecmp (e, "reject") == 0) {
        return 2;
    }
    return 0;
}

/* Returns a g_strdup copy of the known-hosts path override, or NULL if
 * unset. Caller frees. */
static char *
hx_tls_test_known_hosts_override (void)
{
    g_mutex_lock (&hx_tls_test_known_hosts_mutex);
    char *p = hx_tls_test_known_hosts_ov
                  ? g_strdup (hx_tls_test_known_hosts_ov)
                  : NULL;
    g_mutex_unlock (&hx_tls_test_known_hosts_mutex);
    return p;
}

/* ---- Fingerprint compute --------------------------------------- */

gchar *
hx_tls_trust_fingerprint (GTlsCertificate *cert)
{
    g_return_val_if_fail (G_IS_TLS_CERTIFICATE (cert), NULL);

    /* GTlsCertificate carries the DER blob as the "certificate"
     * GByteArray property. SHA-256 over those bytes is the
     * fingerprint shape every other TOFU implementation uses
     * (OpenSSH "VisualHostKey", Mozilla NSS, etc.) so the hex
     * string is comparable across tools. */
    GByteArray *der = NULL;
    g_object_get (cert, "certificate", &der, NULL);
    if (!der) {
        return NULL;
    }

    GChecksum *sha = g_checksum_new (G_CHECKSUM_SHA256);
    g_checksum_update (sha, der->data, der->len);
    const gchar *hex = g_checksum_get_string (sha);
    /* g_checksum_get_string returns lowercase hex; SHA-256 is
     * 64 nibbles. Prefix "sha256:" so future algorithm rotations
     * (sha-512, blake2b) stay backwards-readable. */
    gchar *result = g_strdup_printf ("sha256:%s", hex);
    g_checksum_free (sha);
    g_byte_array_unref (der);
    return result;
}

/* ---- Path resolution ------------------------------------------- */

gchar *
hx_tls_trust_known_hosts_path (void)
{
    /* Test override — Tier 1 tests need a tmpdir path so they
     * don't poke the user's real $CONFIG/known_hosts. Same
     * pattern as the bookmarks tests' GTKHX_CONFIG_DIR override
     * (we don't reuse that one because it would also pull in
     * bookmarks / gtkhxrc which the tls_trust tests don't care
     * about). */
    /* Thread-safe test override wins over the env var (see the
     * test-seam note in tls_trust.h): the multi-threaded Tier 3
     * tests set it instead of g_setenv to avoid the environ race. */
    char *ov = hx_tls_test_known_hosts_override ();
    if (ov) {
        return ov; /* already a g_strdup copy */
    }

    const char *env = g_getenv ("GTKHX_KNOWN_HOSTS");
    if (env && *env) {
        return g_strdup (env);
    }

    const char *cfg = gtkhx_config_dir ();
    if (!cfg) {
        return NULL;
    }
    return g_build_filename (cfg, "known_hosts", NULL);
}

/* ---- File I/O -------------------------------------------------- */

/* Parse one host[:port] field from the start of a known-hosts
 * line. Returns the byte offset into `line` where the host token
 * ends (== whitespace start), or 0 on parse failure. On success
 * fills *host_out / *port_out with newly-allocated strings (host
 * is freed by caller; port may be 0 if line had no :port). */
static gsize
parse_host_field (const char *line, gchar **host_out, guint16 *port_out)
{
    *host_out = NULL;
    *port_out = 0;

    /* Skip leading whitespace defensively (the writer doesn't
     * emit any, but a hand-edit might). `start` advances through
     * the optional whitespace; `line` stays at the caller's
     * pointer so the return value is an offset relative to it —
     * locate_fingerprint then walks past the host token starting
     * from the same base. Pre-fix this returned only the host
     * token length, which made any hand-edited entry with
     * leading whitespace silently mismatch its own fingerprint. */
    const char *start = line;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (*start == '\0' || *start == '#' || *start == '\n') {
        return 0;
    }

    const char *p = start;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        p++;
    }
    gsize fieldlen = p - start;
    if (fieldlen == 0) {
        return 0;
    }

    /* Split host[:port] via the shared IPv6-aware helper. A hostname-only
     * entry (no port) leaves *port_out = 0, which host_port_match treats as
     * the "any port" wildcard. A present-but-malformed port (non-numeric,
     * trailing garbage like "5500garbage", or out of 1..65535) makes the
     * helper fail and we skip the whole entry — never widening a typo into
     * the any-port wildcard. Bracketed IPv6 ([::1]:5600) is handled too;
     * an unbracketed IPv6 literal is taken as host-only (no port). */
    gchar *field = g_strndup (start, fieldlen);
    char *host = NULL;
    guint16 port = 0;
    gboolean ok = gtkhx_parse_host_port (field, 0, &host, &port, NULL);
    g_free (field);
    if (!ok) {
        return 0;
    }
    *host_out = host;
    *port_out = port;
    /* Return the offset from `line` (not `start`) so the caller's
     * subsequent locate_fingerprint(line, host_end) walks past
     * both the leading whitespace and the host token. */
    return (gsize) (p - line);
}

/* Match a parsed (line_host, line_port) entry against the
 * lookup (host, port). line_port == 0 means hostname-only entry
 * — matches every port for that host (legacy SSH known_hosts
 * convention; we never write this shape, but hand-edits may). */
static gboolean
host_port_match (const char *line_host, guint16 line_port,
                 const char *host, guint16 port)
{
    if (g_ascii_strcasecmp (line_host, host) != 0) {
        return FALSE;
    }
    if (line_port == 0) {
        return TRUE;
    }
    return line_port == port;
}

/* Locate the fingerprint field on a line — scans past the host
 * token and any inter-field whitespace. Returns a borrowed
 * pointer into `line`, or NULL if the line has no fingerprint
 * (malformed or comment). */
static const char *
locate_fingerprint (const char *line, gsize host_end)
{
    const char *p = line + host_end;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (g_str_has_prefix (p, "sha256:")) {
        return p;
    }
    return NULL;
}

hx_tls_trust_status
hx_tls_trust_lookup (const char *host, guint16 port, const char *fingerprint)
{
    g_return_val_if_fail (host != NULL, HX_TLS_TRUST_UNKNOWN);
    g_return_val_if_fail (fingerprint != NULL, HX_TLS_TRUST_UNKNOWN);

    g_autofree gchar *path = hx_tls_trust_known_hosts_path ();
    if (!path) {
        return HX_TLS_TRUST_UNKNOWN;
    }

    gchar *contents = NULL;
    gsize contents_len = 0;
    GError *err = NULL;
    if (!g_file_get_contents (path, &contents, &contents_len, &err)) {
        /* Missing file = no pins yet = every cert is UNKNOWN.
         * Don't log — first-run is the common case. */
        g_clear_error (&err);
        return HX_TLS_TRUST_UNKNOWN;
    }

    hx_tls_trust_status result = HX_TLS_TRUST_UNKNOWN;

    gchar **lines = g_strsplit (contents, "\n", -1);
    for (gchar **lp = lines; *lp; lp++) {
        const char *line = *lp;
        if (*line == '#' || *line == '\0') {
            continue;
        }
        g_autofree gchar *line_host = NULL;
        guint16 line_port = 0;
        gsize host_end = parse_host_field (line, &line_host, &line_port);
        if (host_end == 0) {
            continue;
        }
        if (!host_port_match (line_host, line_port, host, port)) {
            continue;
        }

        /* This entry's host:port matches the lookup. Now does
         * the fingerprint match? If yes → TRUSTED. If no →
         * MISMATCH (and keep walking; could be that an older
         * fingerprint is followed by a newer one written by a
         * subsequent pin — but our writer scrubs old entries
         * on pin so duplicate host:port lines should be rare).
         */
        const char *fp_in_line = locate_fingerprint (line, host_end);
        if (!fp_in_line) {
            /* Malformed entry — skip. */
            continue;
        }

        /* Compare just the sha256:... token, not any trailing
         * comment / whitespace. */
        gsize fp_len = strlen (fingerprint);
        if (strncmp (fp_in_line, fingerprint, fp_len) == 0
            && (fp_in_line[fp_len] == '\0'
                || fp_in_line[fp_len] == ' '
                || fp_in_line[fp_len] == '\t'
                || fp_in_line[fp_len] == '\n'
                || fp_in_line[fp_len] == '\r'
                || fp_in_line[fp_len] == '#')) {
            result = HX_TLS_TRUST_TRUSTED;
            break;
        }
        /* Same host:port, different fingerprint — at least
         * MISMATCH. Don't break; if a later line has the right
         * fingerprint (hand-edit with multiple pins), prefer
         * TRUSTED. Walking the whole file is cheap and the
         * semantics are right. */
        result = HX_TLS_TRUST_MISMATCH;
    }

    g_strfreev (lines);
    g_free (contents);
    return result;
}

/* Same fingerprint-token comparison as the strict lookup —
 * factored as a small helper so any-port-search keeps identical
 * delimiter semantics (trailing whitespace, comment, EOL). */
static gboolean
fingerprint_field_matches (const char *fp_in_line, const char *fingerprint)
{
    gsize fp_len = strlen (fingerprint);
    if (strncmp (fp_in_line, fingerprint, fp_len) != 0) {
        return FALSE;
    }
    char delim = fp_in_line[fp_len];
    return delim == '\0' || delim == ' ' || delim == '\t'
        || delim == '\n' || delim == '\r' || delim == '#';
}

gboolean
hx_tls_trust_host_has_fingerprint (const char *host, const char *fingerprint)
{
    g_return_val_if_fail (host != NULL, FALSE);
    g_return_val_if_fail (fingerprint != NULL, FALSE);

    g_autofree gchar *path = hx_tls_trust_known_hosts_path ();
    if (!path) {
        return FALSE;
    }

    gchar *contents = NULL;
    gsize contents_len = 0;
    GError *err = NULL;
    if (!g_file_get_contents (path, &contents, &contents_len, &err)) {
        g_clear_error (&err);
        return FALSE;
    }

    gboolean found = FALSE;
    gchar **lines = g_strsplit (contents, "\n", -1);
    for (gchar **lp = lines; *lp; lp++) {
        const char *line = *lp;
        if (*line == '#' || *line == '\0') {
            continue;
        }
        g_autofree gchar *line_host = NULL;
        guint16 line_port = 0;
        gsize host_end = parse_host_field (line, &line_host, &line_port);
        if (host_end == 0) {
            continue;
        }
        /* Match host case-insensitively but ignore line_port —
         * the entry's port could be anything (including 0, the
         * hostname-only shape). */
        if (g_ascii_strcasecmp (line_host, host) != 0) {
            continue;
        }
        const char *fp_in_line = locate_fingerprint (line, host_end);
        if (!fp_in_line) {
            continue;
        }
        if (fingerprint_field_matches (fp_in_line, fingerprint)) {
            found = TRUE;
            break;
        }
    }

    g_strfreev (lines);
    g_free (contents);
    return found;
}

/* Format one host:port + fingerprint into a known-hosts line
 * with an ISO-date trailing comment. Returns a newly-allocated
 * string ending in '\n'. */
static gchar *
format_entry_line (const char *host, guint16 port, const char *fingerprint)
{
    GDateTime *now = g_date_time_new_now_utc ();
    g_autofree gchar *date = g_date_time_format (now, "%Y-%m-%d");
    g_date_time_unref (now);

    return g_strdup_printf ("%s:%u %s # added %s\n",
                            host, (unsigned) port, fingerprint, date);
}

gboolean
hx_tls_trust_pin (const char *host, guint16 port, const char *fingerprint)
{
    g_return_val_if_fail (host != NULL, FALSE);
    g_return_val_if_fail (fingerprint != NULL, FALSE);

    g_autofree gchar *path = hx_tls_trust_known_hosts_path ();
    if (!path) {
        return FALSE;
    }

    /* Read current contents (file may not exist yet). */
    gchar *existing = NULL;
    gsize existing_len = 0;
    GError *err = NULL;
    if (!g_file_get_contents (path, &existing, &existing_len, &err)) {
        /* Missing is fine; other errors fall through and we'll
         * attempt to create the file. */
        g_clear_error (&err);
        existing = g_strdup ("");
        existing_len = 0;
    }

    /* Walk existing lines: KEEP everything except entries that
     * match (host, port) — those are the old fingerprint we're
     * replacing, drop them. Preserves comments, blank lines, and
     * unrelated host:port entries verbatim. */
    GString *out = g_string_new (NULL);
    gchar **lines = g_strsplit (existing, "\n", -1);
    guint n = g_strv_length (lines);
    /* g_strsplit on a string ending in "\n" produces a trailing
     * "" element. Skip that one when re-emitting so we don't
     * grow the file by a stray blank line per pin. */
    for (guint i = 0; i < n; i++) {
        const char *line = lines[i];
        if (i == n - 1 && *line == '\0') {
            break;
        }
        if (*line == '#' || *line == '\0') {
            g_string_append (out, line);
            g_string_append_c (out, '\n');
            continue;
        }
        g_autofree gchar *line_host = NULL;
        guint16 line_port = 0;
        gsize host_end = parse_host_field (line, &line_host, &line_port);
        if (host_end > 0 && host_port_match (line_host, line_port, host, port)) {
            /* Drop — this is the entry we're replacing. */
            continue;
        }
        g_string_append (out, line);
        g_string_append_c (out, '\n');
    }
    g_strfreev (lines);
    g_free (existing);

    g_autofree gchar *new_line = format_entry_line (host, port, fingerprint);
    g_string_append (out, new_line);

    /* Atomic write: temp file in same dir + rename. Mode 0600 —
     * known_hosts is per-user trust state.
     *
     * NOTE: we deliberately do NOT call g_mkdir_with_parents on
     * the destination directory here. This dates to when the TLS
     * trust decision ran inside glib-networking's GSocketClient
     * accept-certificate handler: any g_mkdir_with_parents call
     * wedged the GnuTLS handshake (it never completed; Janus
     * eventually logged "perform handshake: read handshake: EOF").
     * The bisect was tight — every other GLib + file op in this
     * function passed; only adding g_mkdir_with_parents back
     * triggered the hang (suspected internal GLib lock or a GIO
     * file-monitor side effect on /tmp). The glib-networking TLS
     * path is gone now (both the control channel and the HTXF
     * subchannel handshake through hxnet's rustls and call here
     * via tls_trust_decide), but the mkdir stays dropped anyway:
     * it was never needed. The config directory is
     * created by gtkhx_config_dir() at app startup, and the
     * test harness sets GTKHX_KNOWN_HOSTS to a path under /tmp
     * which always exists, so dropping the mkdir is safe in
     * both contexts. */
    g_autofree gchar *tmp_path = g_strdup_printf ("%s.tmp.XXXXXX", path);
    int fd = g_mkstemp_full (tmp_path, O_RDWR, 0600);
    if (fd < 0) {
        g_string_free (out, TRUE);
        return FALSE;
    }

    gsize written = 0;
    while (written < out->len) {
        ssize_t wn = write (fd, out->str + written, out->len - written);
        if (wn < 0) {
            if (errno == EINTR) {
                continue;
            }
            close (fd);
            unlink (tmp_path);
            g_string_free (out, TRUE);
            return FALSE;
        }
        written += (gsize) wn;
    }
    close (fd);

    /* Atomic swap. POSIX rename rather than g_rename to keep the
     * call deterministic — see the g_mkdir_with_parents note
     * above; we're being deliberately surgical about which GLib
     * file primitives this function touches because it can run on
     * glib-networking's TLS worker thread via the accept-certificate
     * signal handler. */
    if (rename (tmp_path, path) != 0) {
        unlink (tmp_path);
        g_string_free (out, TRUE);
        return FALSE;
    }

    g_string_free (out, TRUE);
    return TRUE;
}
