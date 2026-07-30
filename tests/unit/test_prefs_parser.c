/*
 * tests/unit/test_prefs_parser.c — verify prefs_parse_boolean()
 * accepts every spelling that GKeyFile and the legacy gtkhxrc
 * format can produce.
 *
 * Why this test exists: we already shipped a fix once for this
 * code (commit 6bb3928 — the parser was accepting only '0'/'1'
 * and silently falling through on GKeyFile's "true"/"false",
 * which made every BOOLEAN pref revert to its struct-init default
 * on every startup). Six lines, one regex, one obvious unit test
 * would have caught it. Tests now serve as the regression net.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
#include "prefs_parser.h"

/* ---------- TRUE-ish spellings ---------- */

static void
test_bool_one (void)
{
    unsigned char out = 99;
    g_assert_true (prefs_parse_boolean ("1", &out));
    g_assert_cmpuint (out, ==, 1);
}

static void
test_bool_true_lowercase (void)
{
    unsigned char out = 99;
    g_assert_true (prefs_parse_boolean ("true", &out));
    g_assert_cmpuint (out, ==, 1);
}

static void
test_bool_True_capital (void)
{
    unsigned char out = 99;
    g_assert_true (prefs_parse_boolean ("True", &out));
    g_assert_cmpuint (out, ==, 1);
}

static void
test_bool_TRUE_uppercase (void)
{
    unsigned char out = 99;
    g_assert_true (prefs_parse_boolean ("TRUE", &out));
    g_assert_cmpuint (out, ==, 1);
}

static void
test_bool_yes (void)
{
    unsigned char out = 99;
    g_assert_true (prefs_parse_boolean ("yes", &out));
    g_assert_cmpuint (out, ==, 1);
    g_assert_true (prefs_parse_boolean ("Yes", &out));
    g_assert_cmpuint (out, ==, 1);
    g_assert_true (prefs_parse_boolean ("YES", &out));
    g_assert_cmpuint (out, ==, 1);
}

static void
test_bool_single_letter_t (void)
{
    unsigned char out = 99;
    g_assert_true (prefs_parse_boolean ("t", &out));
    g_assert_cmpuint (out, ==, 1);
    g_assert_true (prefs_parse_boolean ("T", &out));
    g_assert_cmpuint (out, ==, 1);
    g_assert_true (prefs_parse_boolean ("y", &out));
    g_assert_cmpuint (out, ==, 1);
    g_assert_true (prefs_parse_boolean ("Y", &out));
    g_assert_cmpuint (out, ==, 1);
}

/* ---------- FALSE-ish spellings ---------- */

static void
test_bool_zero (void)
{
    unsigned char out = 99;
    g_assert_true (prefs_parse_boolean ("0", &out));
    g_assert_cmpuint (out, ==, 0);
}

static void
test_bool_false_lowercase (void)
{
    unsigned char out = 99;
    g_assert_true (prefs_parse_boolean ("false", &out));
    g_assert_cmpuint (out, ==, 0);
}

static void
test_bool_False_capital (void)
{
    unsigned char out = 99;
    g_assert_true (prefs_parse_boolean ("False", &out));
    g_assert_cmpuint (out, ==, 0);
}

static void
test_bool_FALSE_uppercase (void)
{
    unsigned char out = 99;
    g_assert_true (prefs_parse_boolean ("FALSE", &out));
    g_assert_cmpuint (out, ==, 0);
}

static void
test_bool_no (void)
{
    unsigned char out = 99;
    g_assert_true (prefs_parse_boolean ("no", &out));
    g_assert_cmpuint (out, ==, 0);
    g_assert_true (prefs_parse_boolean ("No", &out));
    g_assert_cmpuint (out, ==, 0);
    g_assert_true (prefs_parse_boolean ("NO", &out));
    g_assert_cmpuint (out, ==, 0);
}

static void
test_bool_single_letter_f (void)
{
    unsigned char out = 99;
    g_assert_true (prefs_parse_boolean ("f", &out));
    g_assert_cmpuint (out, ==, 0);
    g_assert_true (prefs_parse_boolean ("F", &out));
    g_assert_cmpuint (out, ==, 0);
    g_assert_true (prefs_parse_boolean ("n", &out));
    g_assert_cmpuint (out, ==, 0);
    g_assert_true (prefs_parse_boolean ("N", &out));
    g_assert_cmpuint (out, ==, 0);
}

/* ---------- The shipped regression: commit 6bb3928 ----------
 *
 * GKeyFile's get_boolean writes the literal "true" / "false" — those
 * are the actual strings prefs_write puts on disk. Pre-fix, the
 * parser fell through to "return without writing" for both, which
 * is what made SOUNDSON / TIMESTAMP / FILESAMEWIN revert to defaults
 * on every startup. Spell out the exact regression as a test so a
 * future "simplify" pass that reverts to '0'/'1'-only will fail
 * loudly. */
static void
test_bool_shipped_regression_gkeyfile_true_false (void)
{
    unsigned char out = 99;
    g_assert_true (prefs_parse_boolean ("true", &out));
    g_assert_cmpuint (out, ==, 1);
    g_assert_true (prefs_parse_boolean ("false", &out));
    g_assert_cmpuint (out, ==, 0);
}

/* ---------- First-character semantics ---------- */

static void
test_bool_first_char_drives_decision (void)
{
    /* Documented behaviour: only the first character matters.
     * "tarantino" starts with 't' so it parses as TRUE; "facetious"
     * with 'f' parses as FALSE. Same as the historical options.c
     * parser. Test catches anyone who tries to "tighten" the parser
     * into a strict word-list match without thinking through the
     * implications. */
    unsigned char out = 99;
    g_assert_true (prefs_parse_boolean ("tarantino", &out));
    g_assert_cmpuint (out, ==, 1);
    g_assert_true (prefs_parse_boolean ("facetious", &out));
    g_assert_cmpuint (out, ==, 0);
    g_assert_true (prefs_parse_boolean ("yellow", &out));
    g_assert_cmpuint (out, ==, 1);
    g_assert_true (prefs_parse_boolean ("nautical", &out));
    g_assert_cmpuint (out, ==, 0);
}

/* ---------- Unrecognised inputs ---------- */

static void
test_bool_empty_string_returns_false (void)
{
    unsigned char out = 7;
    g_assert_false (prefs_parse_boolean ("", &out));
    /* out is left unchanged. */
    g_assert_cmpuint (out, ==, 7);
}

static void
test_bool_null_input_returns_false (void)
{
    unsigned char out = 5;
    g_assert_false (prefs_parse_boolean (NULL, &out));
    g_assert_cmpuint (out, ==, 5);
}

static void
test_bool_garbage_returns_false (void)
{
    unsigned char out = 3;
    g_assert_false (prefs_parse_boolean ("xyz", &out));
    g_assert_cmpuint (out, ==, 3);
    g_assert_false (prefs_parse_boolean ("?", &out));
    g_assert_cmpuint (out, ==, 3);
    g_assert_false (prefs_parse_boolean ("2", &out));
    g_assert_cmpuint (out, ==, 3);
    g_assert_false (prefs_parse_boolean (" ", &out));
    g_assert_cmpuint (out, ==, 3);
}

static void
test_bool_leading_whitespace_not_tolerated (void)
{
    /* Leading whitespace is not a recognised prefix; first char is
     * ' ' which doesn't match. This documents the historical
     * behaviour — if we ever decide to whitespace-trim we'll need
     * to update the test together with the implementation. */
    unsigned char out = 4;
    g_assert_false (prefs_parse_boolean (" true", &out));
    g_assert_cmpuint (out, ==, 4);
}

/* ---------- NULL out pointer is allowed ---------- */

static void
test_bool_null_out_pointer_just_returns_recognition (void)
{
    /* NULL out is allowed; the function still does the recognition
     * check and returns TRUE / FALSE accordingly. */
    g_assert_true (prefs_parse_boolean ("true", NULL));
    g_assert_true (prefs_parse_boolean ("false", NULL));
    g_assert_true (prefs_parse_boolean ("0", NULL));
    g_assert_true (prefs_parse_boolean ("1", NULL));
    g_assert_false (prefs_parse_boolean ("", NULL));
    g_assert_false (prefs_parse_boolean ("xyz", NULL));
    g_assert_false (prefs_parse_boolean (NULL, NULL));
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/prefs_parser/bool_one", test_bool_one);
    g_test_add_func ("/prefs_parser/bool_true_lowercase",
                     test_bool_true_lowercase);
    g_test_add_func ("/prefs_parser/bool_True_capital", test_bool_True_capital);
    g_test_add_func ("/prefs_parser/bool_TRUE_uppercase",
                     test_bool_TRUE_uppercase);
    g_test_add_func ("/prefs_parser/bool_yes", test_bool_yes);
    g_test_add_func ("/prefs_parser/bool_single_letter_t",
                     test_bool_single_letter_t);

    g_test_add_func ("/prefs_parser/bool_zero", test_bool_zero);
    g_test_add_func ("/prefs_parser/bool_false_lowercase",
                     test_bool_false_lowercase);
    g_test_add_func ("/prefs_parser/bool_False_capital",
                     test_bool_False_capital);
    g_test_add_func ("/prefs_parser/bool_FALSE_uppercase",
                     test_bool_FALSE_uppercase);
    g_test_add_func ("/prefs_parser/bool_no", test_bool_no);
    g_test_add_func ("/prefs_parser/bool_single_letter_f",
                     test_bool_single_letter_f);

    g_test_add_func (
        "/prefs_parser/bool_shipped_regression_gkeyfile_true_false",
        test_bool_shipped_regression_gkeyfile_true_false);
    g_test_add_func ("/prefs_parser/bool_first_char_drives_decision",
                     test_bool_first_char_drives_decision);

    g_test_add_func ("/prefs_parser/bool_empty_string_returns_false",
                     test_bool_empty_string_returns_false);
    g_test_add_func ("/prefs_parser/bool_null_input_returns_false",
                     test_bool_null_input_returns_false);
    g_test_add_func ("/prefs_parser/bool_garbage_returns_false",
                     test_bool_garbage_returns_false);
    g_test_add_func ("/prefs_parser/bool_leading_whitespace_not_tolerated",
                     test_bool_leading_whitespace_not_tolerated);

    g_test_add_func (
        "/prefs_parser/bool_null_out_pointer_just_returns_recognition",
        test_bool_null_out_pointer_just_returns_recognition);

    return g_test_run ();
}
