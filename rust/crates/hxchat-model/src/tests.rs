//! Unit tests for the conversation model + nick completion. The completion
//! rules here define the *corrected* behaviour (working cycle, caret right
//! after the inserted nick) — see the divergence note in `lib.rs`.

use super::*;

// ---- MemberList ----------------------------------------------------------

#[test]
fn upsert_inserts_then_replaces_in_place() {
    let mut ml = MemberList::new();
    ml.upsert(Member::new(1, "alice"));
    ml.upsert(Member::new(2, "bob"));
    assert_eq!(ml.len(), 2);
    // Replace uid 1 (rename) — must keep position 0, not append.
    ml.upsert(Member::new(1, "alicia"));
    assert_eq!(ml.len(), 2);
    let names: Vec<&str> = ml.iter().map(|m| m.name.as_str()).collect();
    assert_eq!(names, vec!["alicia", "bob"]);
    assert_eq!(ml.get(1).unwrap().name, "alicia");
}

#[test]
fn remove_reindexes_remaining() {
    let mut ml = MemberList::new();
    ml.upsert(Member::new(10, "a"));
    ml.upsert(Member::new(20, "b"));
    ml.upsert(Member::new(30, "c"));
    let gone = ml.remove(20).unwrap();
    assert_eq!(gone.name, "b");
    assert_eq!(ml.len(), 2);
    // The uid→index map must still resolve the survivors correctly.
    assert_eq!(ml.get(10).unwrap().name, "a");
    assert_eq!(ml.get(30).unwrap().name, "c");
    assert!(ml.get(20).is_none());
    let names: Vec<&str> = ml.iter().map(|m| m.name.as_str()).collect();
    assert_eq!(names, vec!["a", "c"]);
}

#[test]
fn get_mut_edits_in_place() {
    let mut ml = MemberList::new();
    ml.upsert(Member::new(1, "x"));
    ml.get_mut(1).unwrap().nick_color = Some(0x00ff0000);
    assert_eq!(ml.get(1).unwrap().nick_color, Some(0x00ff0000));
}

#[test]
fn names_sorted_is_case_insensitive() {
    let mut ml = MemberList::new();
    for (i, n) in ["Zoe", "alice", "Bob"].iter().enumerate() {
        ml.upsert(Member::new(i as u16, *n));
    }
    assert_eq!(ml.names_sorted(), vec!["alice", "Bob", "Zoe"]);
}

// ---- completion: address form (whole buffer is a bare nick) --------------

#[test]
fn address_form_single_match_adds_suffix() {
    let c = complete(&["alice"], "ali", 3, false, ':').unwrap();
    assert_eq!(c.text, "alice: ");
    assert_eq!(c.cursor, None);
    assert!(c.info.is_empty());
}

#[test]
fn address_form_no_match_is_none() {
    assert!(complete(&["alice"], "zzz", 3, false, ':').is_none());
}

#[test]
fn address_form_ambiguous_extends_to_common_prefix() {
    // sorted: alan, alice → common "al", longer than "a".
    let c = complete(&["alan", "alice"], "a", 1, false, ':').unwrap();
    assert_eq!(c.text, "al");
    assert_eq!(c.cursor, None);
    assert_eq!(c.info, vec!["alan".to_string(), "alice".to_string()]);
}

#[test]
fn old_style_ambiguous_completes_to_first_match() {
    // Old-style: an ambiguous prefix completes to the first candidate fully,
    // no common-prefix extension or candidate list (vs. the new-style default).
    let c = complete_styled(&["alan", "alice"], "a", 1, false, ':', true).unwrap();
    assert_eq!(c.text, "alan: "); // first sorted candidate, fully
    assert!(c.info.is_empty());
    // New-style over the same input only extends to the common prefix.
    let n = complete(&["alan", "alice"], "a", 1, false, ':').unwrap();
    assert_eq!(n.text, "al");
    assert_eq!(n.info.len(), 2);
}

#[test]
fn address_form_ambiguous_no_further_prefix_reports_only() {
    // "al" is already the common prefix of alan/alice → no extension.
    let c = complete(&["alan", "alice"], "al", 2, false, ':').unwrap();
    assert_eq!(c.text, "al");
    assert_eq!(c.cursor, Some(2));
    assert_eq!(c.info.len(), 2);
}

#[test]
fn ambiguous_non_ascii_common_prefix_stays_on_char_boundary() {
    // "aém" and "aèn" share the leading byte of é/è (0xc3) but diverge in the
    // continuation byte, so the raw byte-common-prefix length lands inside a
    // multi-byte codepoint. Without the char-boundary clamp this panics; with
    // it, the common prefix backs off to "a".
    let c = complete(&["aém", "aèn"], "a", 1, false, ':').unwrap();
    assert_eq!(c.text, "a");
    assert_eq!(c.info, vec!["aém".to_string(), "aèn".to_string()]);
}

#[test]
fn suffix_is_configurable_not_hardcoded_colon() {
    // A non-':' suffix flows through to the address-form output...
    let c = complete(&["alice"], "ali", 3, false, ',').unwrap();
    assert_eq!(c.text, "alice, ");
    // ...and ':' is no longer a hardcoded delimiter: with suffix ',', a buffer
    // whose only "delimiter-ish" char is ':' stays address form, so "alice"
    // (already an exact nick, no space/./?/,) has nothing to cycle to → None,
    // whereas the same buffer with a space would be word-mode. Here we just
    // confirm ':' doesn't force word-mode: "ali:" completes as a bare token.
    let c2 = complete(&["ali:x"], "ali:", 4, false, ',');
    // "ali:" is address form (no space/./?/,) and prefix-matches "ali:x".
    assert_eq!(c2.unwrap().text, "ali:x, ");
}

// ---- completion: mid-buffer word ----------------------------------------

#[test]
fn midbuffer_single_match_places_caret_after_name() {
    // "hi ali" — caret at end; complete the "ali" token.
    let c = complete(&["alice"], "hi ali", 6, false, ':').unwrap();
    assert_eq!(c.text, "hi alice");
    assert_eq!(c.cursor, Some(8)); // right after "alice"
}

#[test]
fn midbuffer_preserves_text_after_caret() {
    let c = complete(&["alice"], "hi ali there", 6, false, ':').unwrap();
    assert_eq!(c.text, "hi alice there");
    assert_eq!(c.cursor, Some(8));
}

#[test]
fn word_at_start_with_trailing_delim_trims_leading_space() {
    // "ali.foo": '.' is a delimiter so not address form; word "ali" at start,
    // after = ".foo". Rebuild trims the synthetic leading space; caret lands
    // right after "alice".
    let c = complete(&["alice"], "ali.foo", 3, false, ':').unwrap();
    assert_eq!(c.text, "alice.foo");
    assert_eq!(c.cursor, Some(5));
}

#[test]
fn empty_token_after_space_is_none() {
    assert!(complete(&["alice"], "hi ", 3, false, ':').is_none());
}

// ---- completion: cycling (the C computed-then-discarded path) ------------

#[test]
fn exact_nick_cycles_forward() {
    // "x alice" — "alice" is an exact nick; Tab cycles to the next candidate.
    let c = complete(&["alice", "bob"], "x alice", 7, false, ':').unwrap();
    assert_eq!(c.text, "x bob");
    assert_eq!(c.cursor, Some(5)); // after "bob"
}

#[test]
fn exact_nick_cycles_backward_with_reverse() {
    let c = complete(&["alice", "bob"], "x bob", 5, true, ':').unwrap();
    assert_eq!(c.text, "x alice");
    assert_eq!(c.cursor, Some(7));
}

#[test]
fn exact_nick_wraps_around() {
    // Forward from the last candidate wraps to the first.
    let c = complete(&["alice", "bob"], "x bob", 5, false, ':').unwrap();
    assert_eq!(c.text, "x alice");
}

#[test]
fn sole_member_equal_to_word_does_not_cycle() {
    assert!(complete(&["alice"], "x alice", 7, false, ':').is_none());
}

// ---- Conversation wrapper ------------------------------------------------

#[test]
fn conversation_complete_uses_sorted_members() {
    let mut convo = Conversation::new(0);
    convo.members.upsert(Member::new(1, "Bob"));
    convo.members.upsert(Member::new(2, "alice"));
    // names_sorted → [alice, Bob]; "al" completes to "alice: " (address form).
    let c = convo.complete("al", 2, false, ':').unwrap();
    assert_eq!(c.text, "alice: ");
}
