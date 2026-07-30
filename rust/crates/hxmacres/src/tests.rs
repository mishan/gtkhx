//! Tests over synthetic Mac resource forks.

use super::*;

/// Build a minimal valid resource fork holding one type with the given
/// (resid, data) resources, laid out the way `macres.c` expects to read it.
fn build_resfork(res_type: u32, resources: &[(i16, &[u8])]) -> Vec<u8> {
    let first_res_off: u32 = 16;

    // Resource-data section: each resource is a u32 length prefix + bytes.
    let mut data_section = Vec::new();
    let mut offsets = Vec::new();
    for (_, d) in resources {
        offsets.push(data_section.len() as u32);
        data_section.extend_from_slice(&(d.len() as u32).to_be_bytes());
        data_section.extend_from_slice(d);
    }
    let res_map_off = first_res_off + data_section.len() as u32;

    // Resource map.
    let mut map = Vec::new();
    map.extend_from_slice(&[0u8; 24]); // reserved header
    map.extend_from_slice(&28u16.to_be_bytes()); // @24 type_list_off
    map.extend_from_slice(&0u16.to_be_bytes()); // @26 name_list_off (unused)
    map.extend_from_slice(&0u16.to_be_bytes()); // @28 num_types-1 (one type here)
                                                // Type entry @30: res_type, num_res-1, ref_list_off.
    map.extend_from_slice(&res_type.to_be_bytes());
    map.extend_from_slice(&((resources.len() as u16).wrapping_sub(1)).to_be_bytes());
    map.extend_from_slice(&10u16.to_be_bytes()); // ref_list_off (38 - 28)
                                                 // Reference entries @38: resid, name_off, attrs, data_off(u24), reserved.
    for (i, (resid, _)) in resources.iter().enumerate() {
        map.extend_from_slice(&resid.to_be_bytes());
        map.extend_from_slice(&0xffffu16.to_be_bytes()); // name_off (none)
        map.push(0); // attrs
        let off = offsets[i];
        map.push((off >> 16) as u8);
        map.push((off >> 8) as u8);
        map.push(off as u8);
        map.extend_from_slice(&0u32.to_be_bytes()); // reserved handle
    }

    let mut out = Vec::new();
    out.extend_from_slice(&first_res_off.to_be_bytes());
    out.extend_from_slice(&res_map_off.to_be_bytes());
    out.extend_from_slice(&(data_section.len() as u32).to_be_bytes());
    out.extend_from_slice(&(map.len() as u32).to_be_bytes());
    out.extend_from_slice(&data_section);
    out.extend_from_slice(&map);
    out
}

const CICN: u32 = 0x6369_636e; // 'cicn'

#[test]
fn parse_and_lookup() {
    let fork = build_resfork(
        CICN,
        &[(128, b"first"), (135, b"second-icon"), (200, b"third")],
    );
    let rf = ResourceFork::parse(fork).expect("parse");

    assert_eq!(rf.num_res_of_type(CICN), 3);
    assert_eq!(rf.num_res_of_type(0x74657374), 0); // 'test' — absent

    assert_eq!(rf.nth_res_of_type(CICN, 0).unwrap().data, b"first");
    assert_eq!(rf.nth_res_of_type(CICN, 1).unwrap().resid, 135);
    assert_eq!(rf.nth_res_of_type(CICN, 1).unwrap().data, b"second-icon");
    assert!(rf.nth_res_of_type(CICN, 3).is_none()); // out of range

    assert_eq!(rf.res_of_id(CICN, 200).unwrap().data, b"third");
    assert!(rf.res_of_id(CICN, 999).is_none());
}

#[test]
fn negative_resid_roundtrips() {
    let fork = build_resfork(0x4142_4344, &[(-2, b"neg")]);
    let rf = ResourceFork::parse(fork).unwrap();
    assert_eq!(rf.res_of_id(0x4142_4344, -2).unwrap().data, b"neg");
}

#[test]
fn malformed_is_none_not_panic() {
    assert!(ResourceFork::parse(vec![0u8; 4]).is_none()); // too short for header
    let mut bad = vec![0u8; 16];
    bad[4..8].copy_from_slice(&9999u32.to_be_bytes()); // res_map_off past EOF
    assert!(ResourceFork::parse(bad).is_none());
    assert!(ResourceFork::parse(Vec::new()).is_none());
}

#[test]
fn empty_data_resource() {
    let fork = build_resfork(CICN, &[(1, b"")]);
    let rf = ResourceFork::parse(fork).unwrap();
    assert_eq!(rf.res_of_id(CICN, 1).unwrap().data, b"");
}
