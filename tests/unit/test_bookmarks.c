/*
 * tests/unit/test_bookmarks.c — drive the bookmark CRUD module
 * (src/bookmarks_io.c) without involving GTK / libadwaita.
 *
 * Each test sets up a fresh GTKHX_PATH tmpdir and a fresh HOME pointing
 * at a tmpdir, so the primary bookmarks dir ($GTKHX_PATH/bookmarks/)
 * and the legacy fallback dir ($HOME/.hx/bookmarks/) are both empty
 * and independent. Tests that exercise legacy-only handling write
 * directly into the legacy dir.
 *
 * Locks in:
 *   - HTsc 460-byte on-disk format (header, padding, login/pass slots,
 *     server "host:port" + flags). The save→load roundtrip checks
 *     every field; a byte-layout check verifies the magic, length
 *     bytes, and flag positions independently.
 *   - hx_bookmark_list: dedup (primary masks legacy), alphabetical
 *     sort, dotfile filter.
 *   - hx_bookmark_rename: no-clobber refusal, legacy-only failure
 *     surfaces a user-actionable error.
 *   - hx_bookmark_delete: legacy-only failure surfaces the same kind
 *     of error.
 *   - hx_bookmark_safe_filename: '/' → '\\' canonicalization (the
 *     UI's defense against names that would otherwise create
 *     mismatched in-memory / on-disk state).
 *
 * The bookmarks module's only external dep is gtkhx_config_dir().
 * We provide a stub at the bottom of this file that reads the
 * GTKHX_PATH env var and caches the result — but invalidates the
 * cache whenever GTKHX_PATH changes, so each test's fresh tmpdir
 * is picked up cleanly. Production (src/gtkhx.c) caches forever
 * because a single-process app only sets its config dir once.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "bookmarks.h"

/* Test-local stub for gtkhx_config_dir — the real one lives in
 * gtkhx.c which pulls in GTK. Tests want a fresh-per-test value
 * driven by $GTKHX_PATH. Declared up front so -Wmissing-prototypes
 * doesn't complain about the definition at the bottom of the file. */
extern const char *gtkhx_config_dir (void);

/* ---------- Test fixture ---------- */

typedef struct {
    char *tmproot;
    char *primary_dir; /* $tmproot/config/gtkhx/bookmarks/ */
    char *legacy_dir;  /* $tmproot/home/.hx/bookmarks/    */
    char *saved_home;
    char *saved_gtkhx_path;
} Fixture;

static void
fixture_setup (Fixture *f, gconstpointer data)
{
    char *config_root;
    char *home_root;
    (void)data;

    f->tmproot = g_dir_make_tmp ("test_bookmarks_XXXXXX", NULL);
    g_assert_nonnull (f->tmproot);

    config_root = g_build_filename (f->tmproot, "config", "gtkhx", NULL);
    home_root = g_build_filename (f->tmproot, "home", NULL);

    f->primary_dir = g_build_filename (config_root, "bookmarks", NULL);
    f->legacy_dir = g_build_filename (home_root, ".hx", "bookmarks", NULL);

    g_assert_cmpint (g_mkdir_with_parents (f->primary_dir, 0700), ==, 0);
    g_assert_cmpint (g_mkdir_with_parents (f->legacy_dir, 0700), ==, 0);

    f->saved_home = g_strdup (g_getenv ("HOME") ? g_getenv ("HOME") : "");
    f->saved_gtkhx_path
        = g_strdup (g_getenv ("GTKHX_PATH") ? g_getenv ("GTKHX_PATH") : "");

    g_assert_true (g_setenv ("HOME", home_root, TRUE));
    g_assert_true (g_setenv ("GTKHX_PATH", config_root, TRUE));

    g_free (config_root);
    g_free (home_root);
}

static void
remove_dir_recursive (const char *path)
{
    GDir *d = g_dir_open (path, 0, NULL);
    const char *name;
    char *child;
    struct stat st;
    if (!d) {
        return;
    }
    while ((name = g_dir_read_name (d))) {
        child = g_build_filename (path, name, NULL);
        if (lstat (child, &st) == 0 && S_ISDIR (st.st_mode)) {
            remove_dir_recursive (child);
        } else {
            g_unlink (child);
        }
        g_free (child);
    }
    g_dir_close (d);
    g_rmdir (path);
}

static void
fixture_teardown (Fixture *f, gconstpointer data)
{
    (void)data;
    if (*f->saved_home) {
        g_setenv ("HOME", f->saved_home, TRUE);
    } else {
        g_unsetenv ("HOME");
    }
    if (*f->saved_gtkhx_path) {
        g_setenv ("GTKHX_PATH", f->saved_gtkhx_path, TRUE);
    } else {
        g_unsetenv ("GTKHX_PATH");
    }
    g_free (f->saved_home);
    g_free (f->saved_gtkhx_path);

    remove_dir_recursive (f->tmproot);

    g_free (f->tmproot);
    g_free (f->primary_dir);
    g_free (f->legacy_dir);
}

/* ---------- Helpers ---------- */

/* Write a minimal valid HTsc-formatted file directly into the legacy
 * dir, so tests can exercise the legacy-only delete/rename paths
 * without involving hx_bookmark_save (which always writes to the
 * primary dir). The blob is the 460-byte fixed layout: header + pad
 * + login/pass slots + server "host:port" + 3 flag bytes + trailing
 * zero pad. */
static void
write_legacy_bookmark (const Fixture *f, const char *name, const char *server)
{
    char *path;
    FILE *fp;
    unsigned char blob[460];
    size_t srvlen = strlen (server);

    memset (blob, 0, sizeof (blob));
    memcpy (blob, "HTsc\0\1", 6);
    /* Login length byte at offset 135, blank login follows. */
    /* Pass length byte at offset 169, blank pass follows. */
    /* Server length byte at offset 203, server bytes follow. */
    blob[203] = (unsigned char)srvlen;
    memcpy (blob + 204, server, srvlen);
    /* secure / compress / cipher flags after server. */
    blob[204 + srvlen + 0] = 1; /* secure */
    blob[204 + srvlen + 1] = 0; /* compress */
    blob[204 + srvlen + 2] = 0; /* cipher */

    path = g_build_filename (f->legacy_dir, name, NULL);
    fp = fopen (path, "wb");
    g_assert_nonnull (fp);
    g_assert_cmpuint (fwrite (blob, 1, sizeof (blob), fp), ==, sizeof (blob));
    g_assert_cmpint (fclose (fp), ==, 0);
    g_free (path);
}

static gboolean
file_exists (const char *dir, const char *name)
{
    char *p = g_build_filename (dir, name, NULL);
    gboolean ok = g_file_test (p, G_FILE_TEST_EXISTS);
    g_free (p);
    return ok;
}

static HxBookmark *
make_bookmark (const char *name, const char *server, const char *port,
               const char *login, const char *pass)
{
    HxBookmark *bm = hx_bookmark_new ();
    bm->name = g_strdup (name);
    g_strlcpy (bm->server, server, sizeof (bm->server));
    g_strlcpy (bm->port, port, sizeof (bm->port));
    g_strlcpy (bm->login, login, sizeof (bm->login));
    g_strlcpy (bm->pass, pass, sizeof (bm->pass));
    bm->secure = 1;
    bm->compress = 0;
    bm->cipher = 0;
    return bm;
}

/* ---------- Tests ---------- */

/* save → load roundtrip preserves every user-visible field. The
 * bookmark is bytewise compatible with the connect.c writer; this
 * is the load half of that contract. */
static void
test_save_load_roundtrip (Fixture *f, gconstpointer data)
{
    g_autoptr (HxBookmark) bm = NULL;
    g_autoptr (HxBookmark) out = NULL;
    GError *err = NULL;
    (void)f;
    (void)data;

    bm = make_bookmark ("mhxd", "hotline.example.org", "5500", "admin",
                        "secret");
    bm->compress = 2;
    bm->cipher = 1;
    g_assert_true (hx_bookmark_save (bm, &err));
    g_assert_no_error (err);

    out = hx_bookmark_load ("mhxd");
    g_assert_nonnull (out);
    g_assert_cmpstr (out->name, ==, "mhxd");
    g_assert_cmpstr (out->server, ==, "hotline.example.org");
    g_assert_cmpstr (out->port, ==, "5500");
    g_assert_cmpstr (out->login, ==, "admin");
    g_assert_cmpstr (out->pass, ==, "secret");
    g_assert_cmpint (out->secure, ==, 1);
    g_assert_cmpint (out->compress, ==, 2);
    g_assert_cmpint (out->cipher, ==, 1);
    /* Phase 4: TLS defaults off on a fresh make_bookmark; this
     * subtest doesn't set it, so the load value should be 0. The
     * dedicated TLS round-trip test below covers the on-path. */
    g_assert_cmpint (out->tls, ==, 0);
}

/* Phase 4: TLS flag round-trips through save/load. Set tls=1 (and
 * everything else off — TLS + HOPE is a meaningless combination at
 * the protocol layer; connect_with_args refuses it). Reload and
 * confirm the new flag byte survives. */
static void
test_save_load_tls (Fixture *f, gconstpointer data)
{
    g_autoptr (HxBookmark) bm = NULL;
    g_autoptr (HxBookmark) out = NULL;
    (void)f;
    (void)data;

    bm = make_bookmark ("tls-srv", "tls.example.org", "5600", "u", "p");
    bm->secure = 0;
    bm->compress = 0;
    bm->cipher = 0;
    bm->tls = 1;
    g_assert_true (hx_bookmark_save (bm, NULL));

    out = hx_bookmark_load ("tls-srv");
    g_assert_nonnull (out);
    g_assert_cmpint (out->tls, ==, 1);
    g_assert_cmpint (out->secure, ==, 0);
    g_assert_cmpstr (out->port, ==, "5600");
}

/* Phase 4: a pre-TLS bookmark on disk (the 3-flag layout the
 * legacy writer emitted) must load with tls=0. The format
 * extension trick is that pre-TLS files wrote a zero byte at
 * the slot where TLS now lives — naturally readable as off. */
static void
test_load_pre_tls_bookmark (Fixture *f, gconstpointer data)
{
    g_autoptr (HxBookmark) out = NULL;
    (void)data;

    /* write_legacy_bookmark uses the pre-TLS shape — secure=1,
     * compress=0, cipher=0, and the bytes past those three are
     * zero-padded. Drop it into the LEGACY dir so hx_bookmark_load
     * picks it up via the legacy fallback path. */
    write_legacy_bookmark (f, "pre-tls", "old.example.org:5500");

    out = hx_bookmark_load ("pre-tls");
    g_assert_nonnull (out);
    g_assert_cmpstr (out->server, ==, "old.example.org");
    g_assert_cmpstr (out->port, ==, "5500");
    g_assert_cmpint (out->secure, ==, 1);
    g_assert_cmpint (out->compress, ==, 0);
    g_assert_cmpint (out->cipher, ==, 0);
    g_assert_cmpint (out->tls, ==, 0);
}

/* Roundtrip with a blank port — the writer omits the ':port' suffix
 * entirely (rather than emitting 'host:'), and the loader sees an
 * empty port string. */
static void
test_save_load_blank_port (Fixture *f, gconstpointer data)
{
    g_autoptr (HxBookmark) bm = NULL;
    g_autoptr (HxBookmark) out = NULL;
    (void)f;
    (void)data;

    bm = make_bookmark ("default-port", "host.example", "", "u", "p");
    g_assert_true (hx_bookmark_save (bm, NULL));

    out = hx_bookmark_load ("default-port");
    g_assert_nonnull (out);
    g_assert_cmpstr (out->server, ==, "host.example");
    g_assert_cmpstr (out->port, ==, "");
}

/* Byte-layout check: the first 6 bytes are the "HTsc\0\1" magic, and
 * the saved file is exactly 460 bytes. Locks in cross-source
 * compatibility with connect.c::save_bookmark_response. */
static void
test_htsc_byte_layout (Fixture *f, gconstpointer data)
{
    HxBookmark *bm;
    char *path;
    FILE *fp;
    unsigned char buf[460];
    struct stat st;
    (void)data;

    bm = make_bookmark ("byte-check", "h", "5500", "lo", "pa");
    bm->tls = 1;
    g_assert_true (hx_bookmark_save (bm, NULL));

    path = g_build_filename (f->primary_dir, "byte-check", NULL);
    g_assert_cmpint (stat (path, &st), ==, 0);
    g_assert_cmpint (st.st_size, ==, 460);

    fp = fopen (path, "rb");
    g_assert_nonnull (fp);
    g_assert_cmpuint (fread (buf, 1, sizeof (buf), fp), ==, sizeof (buf));
    fclose (fp);
    g_free (path);

    /* "HTsc" magic + version 1. */
    g_assert_cmpmem (buf, 6, "HTsc\0\1", 6);
    /* Login length byte at offset 135, value is strlen("lo") = 2. */
    g_assert_cmpint (buf[135], ==, 2);
    g_assert_cmpmem (buf + 136, 2, "lo", 2);
    /* Pass length byte at offset 169. */
    g_assert_cmpint (buf[169], ==, 2);
    g_assert_cmpmem (buf + 170, 2, "pa", 2);
    /* Server length byte at offset 203, "h:5500" = 6 chars. */
    g_assert_cmpint (buf[203], ==, 6);
    g_assert_cmpmem (buf + 204, 6, "h:5500", 6);
    /* Flags follow the server bytes: secure=1, compress=0,
     * cipher=0, tls=1 (Phase 4: 4th flag byte added on disk;
     * pre-TLS files have a zero here naturally via padding). */
    g_assert_cmpint (buf[210], ==, 1);
    g_assert_cmpint (buf[211], ==, 0);
    g_assert_cmpint (buf[212], ==, 0);
    g_assert_cmpint (buf[213], ==, 1);

    hx_bookmark_free (bm);
}

/* Loading a name that isn't on disk anywhere yields NULL — neither
 * the primary nor the legacy dir has it. */
static void
test_load_missing (Fixture *f, gconstpointer data)
{
    g_autoptr (HxBookmark) out = NULL;
    (void)f;
    (void)data;
    out = hx_bookmark_load ("not-there");
    g_assert_null (out);
}

/* List returns entries from both dirs, deduplicated (primary wins),
 * collated alphabetically, and with dotfiles filtered out. */
static void
test_list_dedup_sort_and_dotfiles (Fixture *f, gconstpointer data)
{
    HxBookmark *a, *b;
    GList *list;
    char *dot_path;
    FILE *fp;
    (void)data;

    /* primary: alpha, dup. legacy: beta, dup, .hidden. */
    a = make_bookmark ("alpha", "h1", "", "", "");
    b = make_bookmark ("dup", "primary-version", "", "", "");
    g_assert_true (hx_bookmark_save (a, NULL));
    g_assert_true (hx_bookmark_save (b, NULL));
    hx_bookmark_free (a);
    hx_bookmark_free (b);
    write_legacy_bookmark (f, "beta", "legacy-host");
    write_legacy_bookmark (f, "dup", "legacy-shadowed");
    dot_path = g_build_filename (f->legacy_dir, ".hidden", NULL);
    fp = fopen (dot_path, "w");
    g_assert_nonnull (fp);
    g_assert_cmpint (fclose (fp), ==, 0);
    g_free (dot_path);

    list = hx_bookmark_list ();
    g_assert_cmpuint (g_list_length (list), ==, 3);
    g_assert_cmpstr (g_list_nth_data (list, 0), ==, "alpha");
    g_assert_cmpstr (g_list_nth_data (list, 1), ==, "beta");
    g_assert_cmpstr (g_list_nth_data (list, 2), ==, "dup");

    /* The "dup" entry has to be the primary version — listing must
	 * not shadow the user's primary save with the legacy file. */
    {
        g_autoptr (HxBookmark) dup = hx_bookmark_load ("dup");
        g_assert_nonnull (dup);
        g_assert_cmpstr (dup->server, ==, "primary-version");
    }

    g_list_free_full (list, g_free);
}

/* Rename happy path: source file goes away, target file appears,
 * loading the new name works. */
static void
test_rename_succeeds (Fixture *f, gconstpointer data)
{
    g_autoptr (HxBookmark) bm = NULL;
    g_autoptr (HxBookmark) out = NULL;
    (void)data;
    bm = make_bookmark ("oldname", "h", "", "", "");
    g_assert_true (hx_bookmark_save (bm, NULL));

    g_assert_true (hx_bookmark_rename ("oldname", "newname", NULL));
    g_assert_false (file_exists (f->primary_dir, "oldname"));
    g_assert_true (file_exists (f->primary_dir, "newname"));

    out = hx_bookmark_load ("newname");
    g_assert_nonnull (out);
    g_assert_cmpstr (out->name, ==, "newname");
}

/* Rename refuses to clobber an existing target — both files survive. */
static void
test_rename_refuses_clobber (Fixture *f, gconstpointer data)
{
    HxBookmark *a, *b;
    GError *err = NULL;
    (void)data;

    a = make_bookmark ("a", "h1", "", "", "");
    b = make_bookmark ("b", "h2", "", "", "");
    g_assert_true (hx_bookmark_save (a, NULL));
    g_assert_true (hx_bookmark_save (b, NULL));
    hx_bookmark_free (a);
    hx_bookmark_free (b);

    g_assert_false (hx_bookmark_rename ("a", "b", &err));
    g_assert_error (err, G_FILE_ERROR, G_FILE_ERROR_EXIST);
    g_error_free (err);

    /* Both source and target survive. */
    g_assert_true (file_exists (f->primary_dir, "a"));
    g_assert_true (file_exists (f->primary_dir, "b"));
}

/* Renaming a legacy-only entry surfaces a user-friendly error
 * (G_FILE_ERROR_PERM with a "convert via Connect dialog" message),
 * not a raw "rename: No such file or directory". */
static void
test_rename_legacy_only_clear_error (Fixture *f, gconstpointer data)
{
    GError *err = NULL;
    (void)data;

    write_legacy_bookmark (f, "legacy-only", "old.example");

    g_assert_false (hx_bookmark_rename ("legacy-only", "renamed", &err));
    g_assert_error (err, G_FILE_ERROR, G_FILE_ERROR_PERM);
    /* The error message has to mention the legacy directory so the
	 * user knows why this failed and how to migrate. */
    g_assert_nonnull (strstr (err->message, "legacy"));
    g_error_free (err);
}

/* Deleting a legacy-only entry surfaces the same kind of clear
 * error — and crucially does NOT remove the legacy file (we don't
 * touch the legacy dir from this path). */
static void
test_delete_legacy_only_clear_error (Fixture *f, gconstpointer data)
{
    GError *err = NULL;
    (void)data;

    write_legacy_bookmark (f, "legacy-del", "host.example");

    g_assert_false (hx_bookmark_delete ("legacy-del", &err));
    g_assert_error (err, G_FILE_ERROR, G_FILE_ERROR_PERM);
    g_assert_nonnull (strstr (err->message, "legacy"));
    g_error_free (err);

    /* Legacy file is untouched. */
    g_assert_true (file_exists (f->legacy_dir, "legacy-del"));
}

/* Deleting a file that exists nowhere (not primary, not legacy)
 * surfaces an ordinary ENOENT, not the legacy-only message — the
 * user might genuinely be dealing with a stale list row, and the
 * standard "no such file" is the right thing to tell them. */
static void
test_delete_truly_missing (Fixture *f, gconstpointer data)
{
    GError *err = NULL;
    (void)f;
    (void)data;

    g_assert_false (hx_bookmark_delete ("never-existed", &err));
    g_assert_error (err, G_FILE_ERROR, G_FILE_ERROR_NOENT);
    /* No "legacy" mention — this isn't the legacy-location path. */
    g_assert_null (strstr (err->message, "legacy"));
    g_error_free (err);
}

/* Happy-path delete: file goes away. */
static void
test_delete_succeeds (Fixture *f, gconstpointer data)
{
    HxBookmark *bm;
    (void)data;
    bm = make_bookmark ("doomed", "h", "", "", "");
    g_assert_true (hx_bookmark_save (bm, NULL));
    hx_bookmark_free (bm);

    g_assert_true (file_exists (f->primary_dir, "doomed"));
    g_assert_true (hx_bookmark_delete ("doomed", NULL));
    g_assert_false (file_exists (f->primary_dir, "doomed"));
}

/* hx_bookmark_safe_filename canonicalizes '/' → '\\'. The defense
 * the UI relies on so the in-memory HxBookmark.name matches the
 * actual on-disk filename. */
static void
test_safe_filename_slash (Fixture *f, gconstpointer data)
{
    (void)f;
    (void)data;

    {
        g_autofree char *out = hx_bookmark_safe_filename ("foo/bar/baz");
        g_assert_cmpstr (out, ==, "foo\\bar\\baz");
    }

    {
        g_autofree char *out = hx_bookmark_safe_filename ("no-slashes-here");
        g_assert_cmpstr (out, ==, "no-slashes-here");
    }

    g_assert_null (hx_bookmark_safe_filename (NULL));
    g_assert_null (hx_bookmark_safe_filename (""));
}

/* Save into a name that holds a '/' lands the file on disk under the
 * canonicalized name, AND load with the canonicalized name works. The
 * UI side calls safe_filename before stashing the name in bm->name; this
 * test exercises the load half via the actually-stored filename. */
static void
test_save_with_slash_canonicalizes (Fixture *f, gconstpointer data)
{
    g_autoptr (HxBookmark) bm = NULL;
    g_autoptr (HxBookmark) out = NULL;
    g_autofree char *safe = NULL;
    (void)data;

    bm = make_bookmark ("group/server", "h", "", "", "");
    g_assert_true (hx_bookmark_save (bm, NULL));

    /* On-disk file is the canonicalized form. */
    g_assert_true (file_exists (f->primary_dir, "group\\server"));
    g_assert_false (file_exists (f->primary_dir, "group/server"));

    safe = hx_bookmark_safe_filename ("group/server");
    out = hx_bookmark_load (safe);
    g_assert_nonnull (out);
}

/* bookmark_resolve_path rejects '/' in names — direct load with a
 * '/' must fail. (The UI calls safe_filename first; this defense is
 * just-in-case for any path that doesn't.) */
static void
test_load_with_slash_rejected (Fixture *f, gconstpointer data)
{
    (void)f;
    (void)data;
    /* Even if we created group/server on disk somehow, hx_bookmark_load
	 * would refuse the '/'-bearing name and return NULL. We don't
	 * actually need a file on disk to test this — the rejection
	 * happens before stat. */
    g_assert_null (hx_bookmark_load ("../etc/passwd"));
    g_assert_null (hx_bookmark_load ("foo/bar"));
}

/* ---------- gtkhx_config_dir stub ----------
 *
 * Production links src/gtkhx.c which caches its result on first
 * call. Tests can't have that — each test sets GTKHX_PATH to a
 * fresh tmpdir, so the env var has to be re-read every call. */
const char *
gtkhx_config_dir (void)
{
    static char *cached;
    const char *p = g_getenv ("GTKHX_PATH");
    if (!p || !*p) {
        return "/tmp/no-gtkhx-path-set"; /* tests should always set it */
    }
    if (cached && strcmp (cached, p) == 0) {
        return cached;
    }
    g_free (cached);
    cached = g_strdup (p);
    return cached;
}

/* ---------- Driver ---------- */

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

#define ADD(name, fn) \
    g_test_add ("/bookmarks/" name, Fixture, NULL, fixture_setup, fn, \
                fixture_teardown)

    ADD ("save_load_roundtrip", test_save_load_roundtrip);
    ADD ("save_load_blank_port", test_save_load_blank_port);
    ADD ("save_load_tls", test_save_load_tls);
    ADD ("load_pre_tls_bookmark", test_load_pre_tls_bookmark);
    ADD ("htsc_byte_layout", test_htsc_byte_layout);
    ADD ("load_missing", test_load_missing);
    ADD ("list_dedup_sort_and_dotfiles", test_list_dedup_sort_and_dotfiles);
    ADD ("rename_succeeds", test_rename_succeeds);
    ADD ("rename_refuses_clobber", test_rename_refuses_clobber);
    ADD ("rename_legacy_only_clear_error",
         test_rename_legacy_only_clear_error);
    ADD ("delete_succeeds", test_delete_succeeds);
    ADD ("delete_legacy_only_clear_error",
         test_delete_legacy_only_clear_error);
    ADD ("delete_truly_missing", test_delete_truly_missing);
    ADD ("safe_filename_slash", test_safe_filename_slash);
    ADD ("save_with_slash_canonicalizes", test_save_with_slash_canonicalizes);
    ADD ("load_with_slash_rejected", test_load_with_slash_rejected);

#undef ADD

    return g_test_run ();
}
