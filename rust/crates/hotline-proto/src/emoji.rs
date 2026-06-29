//! Emoji <-> `:shortcode:` conversion.
//!
//! Hotline servers that don't negotiate `CAP_TEXT_ENCODING` carry text as
//! Mac Roman, which has no emoji. Rather than let `gtkhx_text_for_wire`
//! turn every emoji into `?`, we rewrite emoji to their Slack/Discord-style
//! `:joy:` shortcodes on the way out (pure ASCII, survives Mac Roman) and
//! rewrite known `:shortcode:` tokens back to emoji at display time.
//!
//! Two directions, both pure string transforms over the tables in
//! [`crate::emoji_table`]:
//!
//! - [`emoji_to_shortcodes`] — encode, used in the legacy send path. Walks
//!   the text and replaces the longest emoji cluster it can match at each
//!   position with `:canonical:`. Only fully-qualified emoji are encode
//!   sources (see the table generator).
//! - [`shortcodes_to_emoji`] — decode, used at chat display time. Replaces
//!   every `:name:` token whose `name` is a known shortcode with the emoji;
//!   leaves all other text — including unmatched `:tokens:` and stray
//!   colons — untouched.
//!
//! The shortcode grammar is `[a-z0-9_+-]+` between two colons, matching the
//! generator's filter. Decode skips over mIRC colour-code runs (`\x03` plus
//! its numeric spec) so a colour code can never be mistaken for shortcode
//! text or split a token.
//!
//! See `docs/emoji-shortcodes-plan.md` (phase E1).

use crate::emoji_table::{DECODE, ENCODE, MAX_ENCODE_CHARS};

/// mIRC colour control byte (ETX). HexChat's xtext / our chat path use the
/// `\x03NN[,NN]` convention for coloured runs.
const MIRC_COLOR: char = '\u{0003}';

/// True for characters allowed inside a `:shortcode:` token.
#[inline]
fn is_shortcode_char(c: char) -> bool {
    c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_' || c == '+' || c == '-'
}

/// Could `c` begin an emoji cluster in [`ENCODE`]? Every encode key starts
/// with a non-ASCII byte except the keycap emoji, which start with `#`, `*`,
/// or a digit. Skipping the binary-search probe for ordinary ASCII keeps
/// encode linear over plain text.
#[inline]
fn could_start_emoji(c: char) -> bool {
    !c.is_ascii() || c == '#' || c == '*' || c.is_ascii_digit()
}

fn shortcode_for_emoji(cluster: &str) -> Option<&'static str> {
    ENCODE
        .binary_search_by(|(k, _)| (*k).cmp(cluster))
        .ok()
        .map(|i| ENCODE[i].1)
}

fn emoji_for_shortcode(name: &str) -> Option<&'static str> {
    DECODE
        .binary_search_by(|(k, _)| (*k).cmp(name))
        .ok()
        .map(|i| DECODE[i].1)
}

/// Encode: replace emoji clusters with `:shortcode:`. Unmatched text
/// (including emoji with no canonical shortcode) is passed through
/// verbatim. Used by the legacy (non-UTF-8) send path.
pub fn emoji_to_shortcodes(input: &str) -> String {
    let chars: Vec<(usize, char)> = input.char_indices().collect();
    let n = chars.len();
    let mut out = String::with_capacity(input.len());
    let mut i = 0;

    while i < n {
        let (start, c) = chars[i];
        if could_start_emoji(c) {
            // Longest-match: try the longest cluster first so multi-codepoint
            // sequences (ZWJ families, keycaps, skin-tone combos) win over
            // their leading codepoint.
            let max_l = MAX_ENCODE_CHARS.min(n - i);
            let mut matched = false;
            for l in (1..=max_l).rev() {
                let end = if i + l < n { chars[i + l].0 } else { input.len() };
                let sub = &input[start..end];
                if let Some(sc) = shortcode_for_emoji(sub) {
                    out.push(':');
                    out.push_str(sc);
                    out.push(':');
                    i += l;
                    matched = true;
                    break;
                }
            }
            if matched {
                continue;
            }
        }
        out.push(c);
        i += 1;
    }

    out
}

/// Decode: replace every known `:shortcode:` token with its emoji. Unknown
/// tokens and stray colons are left exactly as they were. mIRC colour runs
/// are copied through untouched. Used at chat display time on every server.
pub fn shortcodes_to_emoji(input: &str) -> String {
    let chars: Vec<(usize, char)> = input.char_indices().collect();
    let n = chars.len();
    let mut out = String::with_capacity(input.len());
    let mut i = 0;

    while i < n {
        let c = chars[i].1;

        // Copy a mIRC colour code verbatim: ETX, up to 2 fg digits, then an
        // optional ",NN" bg part. Colour specs hold no colons, so this only
        // matters to keep a colon-adjacent code from being mis-scanned.
        if c == MIRC_COLOR {
            out.push(c);
            i += 1;
            let mut d = 0;
            while i < n && d < 2 && chars[i].1.is_ascii_digit() {
                out.push(chars[i].1);
                i += 1;
                d += 1;
            }
            if i + 1 < n && chars[i].1 == ',' && chars[i + 1].1.is_ascii_digit() {
                out.push(',');
                i += 1;
                let mut d2 = 0;
                while i < n && d2 < 2 && chars[i].1.is_ascii_digit() {
                    out.push(chars[i].1);
                    i += 1;
                    d2 += 1;
                }
            }
            continue;
        }

        if c == ':' {
            // Read a candidate name: one or more shortcode chars, then ':'.
            let mut j = i + 1;
            while j < n && is_shortcode_char(chars[j].1) {
                j += 1;
            }
            if j < n && j > i + 1 && chars[j].1 == ':' {
                let name = &input[chars[i + 1].0..chars[j].0];
                if let Some(em) = emoji_for_shortcode(name) {
                    out.push_str(em);
                    i = j + 1;
                    continue;
                }
            }
            // Not a known shortcode — emit just the opening colon and let the
            // scan resume at the next char (so the closing colon of a failed
            // token can still open the next one).
            out.push(':');
            i += 1;
            continue;
        }

        out.push(c);
        i += 1;
    }

    out
}

/// Prefix query for the typeahead popup. Given the partial name the user
/// has typed after an opening colon (no colons, e.g. "jo"), return up to
/// `max` `(shortcode, emoji)` matches, ranked: an exact name match first,
/// then by ascending name length, then alphabetically — so the shortest,
/// most likely-intended shortcodes surface at the top. Searches every
/// DECODE name (aliases + CLDR), so `:+1` finds 👍 and `:thu` finds
/// `thumbsup`/`thumbsdown`/etc. An empty prefix returns nothing (the popup
/// only opens once there's at least one character to match).
pub fn shortcode_matches(prefix: &str, max: usize) -> Vec<(&'static str, &'static str)> {
    if prefix.is_empty() || max == 0 {
        return Vec::new();
    }
    let mut hits: Vec<&'static (&'static str, &'static str)> = DECODE
        .iter()
        .filter(|(name, _)| name.starts_with(prefix))
        .collect();
    hits.sort_by(|a, b| {
        // false (exact) sorts before true (non-exact); then short before
        // long; then alphabetical for a stable, deterministic order.
        let ka = (a.0 != prefix, a.0.len(), a.0);
        let kb = (b.0 != prefix, b.0.len(), b.0);
        ka.cmp(&kb)
    });
    hits.truncate(max);
    hits.iter().map(|(n, e)| (*n, *e)).collect()
}

/// Copy `s` into `dst`, truncating at the last UTF-8 char boundary that
/// fits, and return the **full** byte length `s` needs (snprintf-style).
/// When the return value exceeds `dst.len()` the output was truncated; the
/// caller re-calls with a buffer of at least the returned size. Writing a
/// truncated prefix in that case is harmless — the caller discards it.
fn emit(s: &str, dst: &mut [u8]) -> usize {
    let bytes = s.as_bytes();
    if !dst.is_empty() {
        let mut n = bytes.len().min(dst.len());
        if n < bytes.len() {
            while n > 0 && (bytes[n] & 0b1100_0000) == 0b1000_0000 {
                n -= 1;
            }
        }
        if n > 0 {
            dst[..n].copy_from_slice(&bytes[..n]);
        }
    }
    bytes.len()
}

/// Buffer-fill encode for the FFI. `input` is interpreted as UTF-8
/// (lossily — the send path feeds valid UTF-8, but be defensive). Writes
/// the `:shortcode:`-rewritten text into `dst` and returns the full
/// required length per [`emit`].
pub fn emoji_to_shortcodes_into(input: &[u8], dst: &mut [u8]) -> usize {
    emit(&emoji_to_shortcodes(&String::from_utf8_lossy(input)), dst)
}

/// Buffer-fill decode for the FFI. Counterpart of
/// [`emoji_to_shortcodes_into`]; replaces known `:shortcode:` tokens with
/// emoji. Returns the full required length per [`emit`].
pub fn shortcodes_to_emoji_into(input: &[u8], dst: &mut [u8]) -> usize {
    emit(&shortcodes_to_emoji(&String::from_utf8_lossy(input)), dst)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn table_sorted_and_searchable() {
        // Binary search relies on both tables being sorted by key.
        assert!(ENCODE.windows(2).all(|w| w[0].0 < w[1].0), "ENCODE unsorted");
        assert!(DECODE.windows(2).all(|w| w[0].0 < w[1].0), "DECODE unsorted");
        assert!(!ENCODE.is_empty() && !DECODE.is_empty());
    }

    #[test]
    fn every_canonical_round_trips() {
        // Collision guard: every ENCODE canonical shortcode must decode back
        // to the exact emoji it encodes from. If a future dataset change made
        // two emoji share a canonical shortcode, DECODE (first-wins) would
        // map that shortcode to only one of them and the other would silently
        // round-trip to the wrong emoji — this catches it across the whole
        // table, not just the handful of spot-checks above.
        for (emoji, sc) in ENCODE {
            assert_eq!(
                emoji_for_shortcode(sc),
                Some(*emoji),
                "canonical :{sc}: does not decode back to {emoji:?}"
            );
        }
    }

    #[test]
    fn encode_basic() {
        assert_eq!(emoji_to_shortcodes("😂"), ":joy:");
        assert_eq!(emoji_to_shortcodes("hi 😂!"), "hi :joy:!");
        // Canonical is the shortest grammar-valid name: 👍 → "+1" (not the
        // longer "thumbsup"), ❤️ → "heart".
        assert_eq!(emoji_to_shortcodes("👍 ❤️"), ":+1: :heart:");
    }

    #[test]
    fn encode_passthrough_plain_text() {
        let s = "no emoji here, just text 123 #hash *star C:\\path";
        assert_eq!(emoji_to_shortcodes(s), s);
    }

    #[test]
    fn decode_basic() {
        assert_eq!(shortcodes_to_emoji(":joy:"), "😂");
        assert_eq!(shortcodes_to_emoji("hi :joy:!"), "hi 😂!");
        assert_eq!(shortcodes_to_emoji(":thumbsup: :heart:"), "👍 ❤️");
    }

    #[test]
    fn decode_accepts_aliases_and_cldr() {
        // gemoji alias, Slack alias, and CLDR English name all decode.
        let joy = shortcodes_to_emoji(":joy:");
        assert_eq!(shortcodes_to_emoji(":face_with_tears_of_joy:"), joy);
        let up = shortcodes_to_emoji(":thumbsup:");
        assert_eq!(shortcodes_to_emoji(":+1:"), up);
    }

    #[test]
    fn round_trip_common_set() {
        for e in ["😂", "👍", "🎉", "🔥", "🚀", "😀", "💯"] {
            let sc = emoji_to_shortcodes(e);
            assert_ne!(sc, e, "{e} did not encode");
            assert_eq!(shortcodes_to_emoji(&sc), e, "round-trip failed for {e}");
        }
    }

    #[test]
    fn decode_leaves_unknown_and_stray_colons() {
        assert_eq!(shortcodes_to_emoji(":notareal_shortcode:"), ":notareal_shortcode:");
        assert_eq!(shortcodes_to_emoji("10:30:00"), "10:30:00");
        assert_eq!(shortcodes_to_emoji("C:\\path"), "C:\\path");
        assert_eq!(shortcodes_to_emoji("ratio a:b"), "ratio a:b");
        assert_eq!(shortcodes_to_emoji("http://x"), "http://x");
        assert_eq!(shortcodes_to_emoji("::"), "::");
        assert_eq!(shortcodes_to_emoji(""), "");
    }

    #[test]
    fn decode_adjacent_and_recoverable_colons() {
        // ":foo:joy:" — :foo: is unknown, but :joy: inside still resolves.
        assert_eq!(shortcodes_to_emoji(":zzznope:joy:"), ":zzznope😂");
        // Two valid tokens back to back.
        assert_eq!(shortcodes_to_emoji(":joy::fire:"), "😂🔥");
        // Leading extra colon before a valid token.
        assert_eq!(shortcodes_to_emoji(":::joy:"), "::😂");
    }

    #[test]
    fn decode_skips_mirc_colour() {
        // ETX + "04" colour spec, then a real token — colour copied verbatim.
        let input = "\u{3}04:joy:\u{3}";
        assert_eq!(shortcodes_to_emoji(input), "\u{3}04😂\u{3}");
        // fg,bg spec.
        let input2 = "\u{3}04,01 hi :fire:";
        assert_eq!(shortcodes_to_emoji(input2), "\u{3}04,01 hi 🔥");
    }

    #[test]
    fn encode_longest_match_wins() {
        // A keycap (#-VS16-keycap) must beat a bare '#'. The cluster encodes
        // to a single shortcode rather than leaving '#' plus combining marks.
        let keycap = "#\u{fe0f}\u{20e3}";
        let sc = emoji_to_shortcodes(keycap);
        assert!(sc.starts_with(':') && sc.ends_with(':'), "got {sc:?}");
        assert_eq!(shortcodes_to_emoji(&sc), keycap);
    }

    #[test]
    fn into_reports_full_length_and_truncates_at_boundary() {
        // Ample buffer: full write, return == byte length.
        let mut big = [0u8; 64];
        let n = emoji_to_shortcodes_into("😂".as_bytes(), &mut big);
        assert_eq!(n, ":joy:".len());
        assert_eq!(&big[..n], b":joy:");

        // Too-small buffer: return is the *required* length (> cap), and
        // the prefix written never splits a codepoint.
        let mut small = [0u8; 3];
        let need = shortcodes_to_emoji_into(":joy:".as_bytes(), &mut small);
        assert_eq!(need, "😂".len()); // 4 bytes needed
        assert!(need > small.len());
        // "😂" is 4 bytes; nothing fits in 3 without splitting → 0 written.
        assert_eq!(&small, &[0u8; 3]);

        // Empty buffer is allowed and just reports the needed size.
        // 👍 canonical is the shortest name, "+1" → ":+1:".
        let need2 = emoji_to_shortcodes_into("👍".as_bytes(), &mut []);
        assert_eq!(need2, ":+1:".len());
    }

    #[test]
    fn into_handles_invalid_utf8_lossily() {
        // Lone 0xFF is not valid UTF-8; must not panic.
        let mut buf = [0u8; 16];
        let n = emoji_to_shortcodes_into(&[0xFF, b'h', b'i'], &mut buf);
        assert!(n > 0);
    }

    #[test]
    fn matches_basic_and_ranked() {
        // Exact name ranks first even though longer names also start with it.
        let m = shortcode_matches("joy", 8);
        assert_eq!(m[0].0, "joy");
        assert_eq!(m[0].1, "😂");
        // Every result actually starts with the prefix.
        assert!(m.iter().all(|(n, _)| n.starts_with("joy")));

        // Prefix that hits several: results are capped and prefix-filtered.
        let m2 = shortcode_matches("thumb", 3);
        assert!(m2.len() <= 3 && !m2.is_empty());
        assert!(m2.iter().all(|(n, _)| n.starts_with("thumb")));

        // Shorter names rank before longer ones for the same prefix.
        let m3 = shortcode_matches("fire", 8);
        assert_eq!(m3[0].0, "fire");
        assert!(m3.windows(2).all(|w| w[0].0.len() <= w[1].0.len()));
    }

    #[test]
    fn matches_empty_and_unknown() {
        assert!(shortcode_matches("", 8).is_empty());
        assert!(shortcode_matches("joy", 0).is_empty());
        assert!(shortcode_matches("zzzznotaprefix", 8).is_empty());
    }

    #[test]
    fn matches_finds_punctuation_aliases() {
        // "+1" is a real alias for 👍; a "+" prefix must reach it.
        let m = shortcode_matches("+", 8);
        assert!(m.iter().any(|(n, e)| *n == "+1" && *e == "👍"));
    }

    #[test]
    fn encode_then_decode_mixed_message() {
        let msg = "great work 🎉 ship it 🚀!";
        let wire = emoji_to_shortcodes(msg);
        assert!(!wire.contains('🎉') && !wire.contains('🚀'));
        assert!(wire.is_ascii(), "wire form must be ASCII: {wire:?}");
        assert_eq!(shortcodes_to_emoji(&wire), msg);
    }
}
