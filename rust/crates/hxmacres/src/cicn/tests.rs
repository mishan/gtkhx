//! Tests over hand-built `cicn` resources + the halo.

use super::*;

fn put16(buf: &mut [u8], off: usize, v: u16) {
    buf[off..off + 2].copy_from_slice(&v.to_be_bytes());
}

/// A 2x2 1-bpp cicn with a red/blue ColorTable and a fully-opaque mask, laid
/// out the way `decode` walks it:
///   PixMap[50] · maskBitMap[14] · iconBitMap[14] · iconData[4] ·
///   maskdata[2] · bitmapdata[2] · ColorTable[24] · pixeldata[2]  = 112 bytes
fn checkerboard_cicn() -> Vec<u8> {
    let mut b = vec![0u8; 112];

    // PixMap.
    put16(&mut b, 4, 1); // rowBytes
    put16(&mut b, 6, 0); // bounds top
    put16(&mut b, 8, 0); // left
    put16(&mut b, 10, 2); // bottom
    put16(&mut b, 12, 2); // right
    put16(&mut b, 32, 1); // pixelSize (bpp)

    // Mask BitMap @50.
    put16(&mut b, 50 + 4, 1); // rowBytes
    put16(&mut b, 50 + 10, 2); // bounds bottom
    put16(&mut b, 50 + 12, 2); // bounds right (non-zero → have_mask)

    // Icon BitMap @64.
    put16(&mut b, 64 + 4, 1); // rowBytes
    put16(&mut b, 64 + 10, 2); // bounds bottom

    // Mask data @82: both rows opaque (x=0,1 → bits 7,6).
    b[82] = 0b1100_0000;
    b[83] = 0b1100_0000;

    // Bitmap data @84: unused by the decoder (zeros).

    // ColorTable @86: ctSeed(4), ctFlags(2), ctSize(2)=1, then 2 entries.
    put16(&mut b, 92, 1); // ctSize (→ 2 entries)
    // entry 0 @94: value=0, rgb = red (0xffff, 0, 0)
    put16(&mut b, 94, 0);
    put16(&mut b, 96, 0xffff);
    // entry 1 @102: value=1, rgb = blue (0, 0, 0xffff)
    put16(&mut b, 102, 1);
    put16(&mut b, 108, 0xffff);

    // Pixel data @110: checkerboard [0,1 / 1,0].
    b[110] = 0b0100_0000; // row0: x0=0 (red), x1=1 (blue)
    b[111] = 0b1000_0000; // row1: x0=1 (blue), x1=0 (red)
    b
}

fn px(img: &Rgba, x: u32, y: u32) -> [u8; 4] {
    let o = ((y * img.width + x) * 4) as usize;
    img.pixels[o..o + 4].try_into().unwrap()
}

#[test]
fn decode_checkerboard() {
    let img = decode(&checkerboard_cicn()).expect("decode");
    // 2x2 icon + 1px halo on every side → 4x4.
    assert_eq!((img.width, img.height), (4, 4));

    // The icon sits at the (1,1) inset. Red/blue checkerboard, all opaque.
    assert_eq!(px(&img, 1, 1), [0xff, 0x00, 0x00, 0xff]); // (0,0) red
    assert_eq!(px(&img, 2, 1), [0x00, 0x00, 0xff, 0xff]); // (1,0) blue
    assert_eq!(px(&img, 1, 2), [0x00, 0x00, 0xff, 0xff]); // (0,1) blue
    assert_eq!(px(&img, 2, 2), [0xff, 0x00, 0x00, 0xff]); // (1,1) red
}

#[test]
fn default_palette_when_no_ct_overlay() {
    // Blank the ColorTable's entries (ctSize entries all value 0/rgb 0) and
    // check the 1-bpp default palette (white / black) drives index 1 → black.
    let mut b = checkerboard_cicn();
    put16(&mut b, 92, 0); // ctSize=0 → one entry {value 0, rgb 0} overrides idx 0 only
    b[94..110].fill(0);
    let img = decode(&b).unwrap();
    // idx-1 pixels keep the default black (RGB_1[1]); the (0,0) entry got
    // overridden to rgb=0 (black) too, so idx-0 is black as well here.
    assert_eq!(px(&img, 2, 1), [0x00, 0x00, 0x00, 0xff]); // idx 1 → default black
}

#[test]
fn malformed_is_none() {
    assert!(decode(&[]).is_none());
    assert!(decode(&[0u8; 10]).is_none()); // too short for the PixMap fields
    let mut b = checkerboard_cicn();
    put16(&mut b, 32, 3); // bpp = 3 (invalid)
    assert!(decode(&b).is_none());
    let mut b = checkerboard_cicn();
    put16(&mut b, 12, 0); // right = 0 <= left → inverted Rect
    assert!(decode(&b).is_none());
}

#[test]
fn halo_around_single_pixel() {
    // 1x1 opaque red → 3x3 with a grey halo on the 4 edge-adjacent cells.
    let src = Rgba {
        width: 1,
        height: 1,
        pixels: vec![0xff, 0x00, 0x00, 0xff],
    };
    let out = add_halo(&src);
    assert_eq!((out.width, out.height), (3, 3));
    assert_eq!(px(&out, 1, 1), [0xff, 0x00, 0x00, 0xff]); // the pixel
    let halo = [0x80, 0x80, 0x80, 0xa0];
    assert_eq!(px(&out, 0, 1), halo); // left
    assert_eq!(px(&out, 2, 1), halo); // right
    assert_eq!(px(&out, 1, 0), halo); // top
    assert_eq!(px(&out, 1, 2), halo); // bottom
    assert_eq!(px(&out, 0, 0), [0, 0, 0, 0]); // corner — no opaque neighbour
}
