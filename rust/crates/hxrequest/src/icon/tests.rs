//! The icon requests, pinned against hand-written wire bytes.

use super::*;

#[test]
fn each_request_goes_as_gtkhx_has_always_sent_it() {
    let req = |opcode: u32, chunks: &[(u16, &[u8])]| Request {
        opcode,
        chunks: chunks.iter().map(|(t, d)| (*t, d.to_vec())).collect(),
    };
    for (got, want) in [
        (list(), req(1861, &[])),
        (get(0x1234).unwrap(), req(1863, &[(0x0067, &[0x12, 0x34])])),
        (set(b"GIF89a").unwrap(), req(1862, &[(0x0300, b"GIF89a")])),
        // Clearing sends the field empty.
        (set(b"").unwrap(), req(1862, &[(0x0300, b"")])),
    ] {
        assert_eq!(got, want);
    }
    assert_eq!(set(&vec![0; 65536]), None);
}
