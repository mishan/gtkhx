#ifndef HX_PREFS_PARSER_H
#define HX_PREFS_PARSER_H 1

/*
 * Standalone helpers for parsing prefs values (as written by
 * GKeyFile and historically by the old plain-text gtkhxrc format).
 * Lives in its own translation unit so the unit tests can compile
 * the parser without dragging in options.c's mountain of GTK +
 * Adwaita widget code.
 *
 * Phase 5 reminder: GKeyFile writes booleans as the literal strings
 * "true" / "false". The original gtkhxrc parser only accepted
 * '0'/'1' and silently fell through on anything else, which made
 * every BOOLEAN pref revert to its struct-init default on every
 * startup. The fix that shipped accepts both spellings,
 * case-insensitively, plus "yes"/"no" since GKeyFile's own
 * get_boolean tolerates them.
 */

#include <glib.h>

/*
 * Parse a boolean string. Recognises (case-insensitively) all of:
 *   - "0"     / "1"
 *   - "false" / "true"
 *   - "no"    / "yes"
 *   - "f"     / "t"      (single-letter shorthand)
 *   - "n"     / "y"      (single-letter shorthand)
 *
 * Decision is based on the FIRST character only — the parser does
 * not require the rest of the word to match. So "tarantino" parses
 * as TRUE, "facetious" as FALSE. That matches the historical
 * options.c behaviour and keeps GKeyFile's "true" / "false" both
 * working without a string-table lookup.
 *
 * On a recognised input, *out is set to 0 or 1 and the function
 * returns TRUE. On any unrecognised input *out is left alone and
 * FALSE is returned. NULL `s` returns FALSE.
 */
extern gboolean prefs_parse_boolean (const char *s, unsigned char *out);

#endif /* HX_PREFS_PARSER_H */
