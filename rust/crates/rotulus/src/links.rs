//! Autolinking message bodies.
//!
//! The detector lives in `rotulus_layout::linkify`, where it is tested
//! headless; this marks what it finds on a `ParsedText`, with each link
//! pointing at the form a launcher can open (`www.example.com` becomes
//! `https://www.example.com`).

use rotulus_layout::{Linkifier, ParsedText};

/// Find URLs in `p.text` and mark them as links.
///
/// Ranges that don't land on char boundaries are dropped by
/// `ParsedText::add_link`, so a detector disagreeing about byte offsets
/// degrades to "no link" rather than corrupting the spans.
pub fn autolink(p: &mut ParsedText, links: &Linkifier) {
    if p.text.is_empty() {
        return;
    }
    for range in links.scan(&p.text) {
        let href = links.normalize(&p.text[range.clone()]);
        p.add_link(range, href);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn linked(text: &str) -> ParsedText {
        let mut p = ParsedText::plain(text);
        autolink(&mut p, &Linkifier::default());
        p
    }

    #[test]
    fn autolink_marks_a_url() {
        let p = linked("see http://example.com/x now");
        assert_eq!(p.links.len(), 1);
        assert_eq!(p.links[0].href, "http://example.com/x");
        assert_eq!(&p.text[p.links[0].range.clone()], "http://example.com/x");
        assert_eq!(p.style_at(4).link, Some(0));
        assert_eq!(p.style_at(0).link, None);
        p.debug_assert_well_formed();
    }

    #[test]
    fn autolink_handles_several() {
        let p = linked("http://a.example and http://b.example");
        assert_eq!(p.links.len(), 2);
        p.debug_assert_well_formed();
    }

    #[test]
    fn autolink_points_a_bare_host_at_a_scheme() {
        let p = linked("try www.example.com");
        assert_eq!(p.links[0].href, "https://www.example.com");
        assert_eq!(&p.text[p.links[0].range.clone()], "www.example.com");
    }

    #[test]
    fn autolink_leaves_plain_text_alone() {
        let p = linked("nothing to see here");
        assert!(p.links.is_empty());
        assert!(p.spans.is_empty());
    }

    #[test]
    fn autolink_survives_multibyte_text() {
        let p = linked("héllo → http://example.com/ø done");
        p.debug_assert_well_formed();
        assert_eq!(p.links.len(), 1);
    }

    #[test]
    fn autolink_on_empty_text_is_a_noop() {
        assert!(linked("").links.is_empty());
    }
}
