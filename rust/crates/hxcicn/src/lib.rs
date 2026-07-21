//! `hxcicn` — a Macintosh `cicn` colour-icon decoder, ported from `src/cicn.c`.
//!
//! A `cicn` resource is a classic Mac colour icon: a `PixMap` header, a mask
//! `BitMap`, an icon `BitMap`, then the mask bits, the bitmap bits, a per-icon
//! `ColorTable`, and finally the indexed pixel data. GtkHx renders these as user
//! icons. This crate walks that structure and produces a packed **RGBA** buffer
//! (the Mac mask folds into the alpha channel), with no GTK dependency — the
//! caller wraps the RGBA into a `GdkPixbuf`.
//!
//! Unlike `cicn.c`, every read is bounds-checked, so a malformed / truncated
//! `cicn` (which turns up in user-supplied `icons.rsrc` files) yields `None`
//! rather than reading past the buffer.

pub mod ffi;
mod palette;

/// A decoded image: packed RGBA, `width * height * 4` bytes.
pub struct Rgba {
    pub width: u32,
    pub height: u32,
    pub pixels: Vec<u8>,
}

// ---- offsets into the cicn sub-structures (see cicn.h) ----
const PM_ROWBYTES: usize = 4;
const PM_BOUNDS_TOP: usize = 6;
const PM_BOUNDS_LEFT: usize = 8;
const PM_BOUNDS_BOTTOM: usize = 10;
const PM_BOUNDS_RIGHT: usize = 12;
const PM_PIXELSIZE: usize = 32;
const MBM: usize = 50; // mask BitMap
const BM: usize = 64; // icon BitMap
const BITMAP_ROWBYTES: usize = 4;
const BITMAP_BOUNDS_TOP: usize = 6;
const BITMAP_BOUNDS_BOTTOM: usize = 10;
const BITMAP_BOUNDS_RIGHT: usize = 12;
const MASK_DATA: usize = 82; // mask bitmap image data starts here

fn be16(b: &[u8], off: usize) -> Option<u16> {
    b.get(off..off + 2).map(|s| u16::from_be_bytes([s[0], s[1]]))
}

/// Pack a legacy 16-bit-per-channel `(r, g, b)` (high byte = the 8-bit value)
/// into `0x00RRGGBB`. `be16` already normalizes cicn ColorTable bytes to host
/// order, so the same `>> 8` extraction serves both the static palettes and the
/// file's ColorTable (this is why `cicn.c`'s separate `rgb_pack_net` isn't
/// needed here).
fn rgb_pack(c: (u16, u16, u16)) -> u32 {
    (((c.0 >> 8) as u32) << 16) | (((c.1 >> 8) as u32) << 8) | ((c.2 >> 8) as u32)
}

/// Build a 256-entry palette: the canonical Mac default for `bpp`, overlaid with
/// any entries the per-icon ColorTable at `ct_off` specifies.
fn build_palette(data: &[u8], ct_off: usize, bpp: u32) -> [u32; 256] {
    let mut out = [0u32; 256];
    let n = 1usize << bpp;
    let defpal: &[(u16, u16, u16)] = match bpp {
        8 => &palette::RGB_8,
        4 => &palette::RGB_4,
        2 => &palette::RGB_2,
        1 => &palette::RGB_1,
        _ => return out,
    };
    for i in 0..n {
        out[i] = rgb_pack(defpal[i]);
    }
    // ColorTable: ctSeed(4), ctFlags(2), ctSize(2), then ctSize+1 entries of
    // { value(2), rgb(6) }. Out-of-bounds (malformed CT) just stops the overlay.
    if let Some(ctsize) = be16(data, ct_off + 6) {
        for i in 0..(ctsize as usize + 1) {
            let e = ct_off + 8 + i * 8;
            let (Some(value), Some(r), Some(g), Some(b)) =
                (be16(data, e), be16(data, e + 2), be16(data, e + 4), be16(data, e + 6))
            else {
                break;
            };
            out[(value as usize) & (n - 1)] = rgb_pack((r, g, b));
        }
    }
    out
}

/// Decode a `cicn` resource into an RGBA image (with the 1px halo applied, so
/// the result is 2px larger on each axis). `None` on malformed input.
pub fn decode(rsrc: &[u8]) -> Option<Rgba> {
    let len = rsrc.len();

    let bpp = be16(rsrc, PM_PIXELSIZE)? as u32;
    if !matches!(bpp, 1 | 2 | 4 | 8) {
        return None;
    }

    let b_top = be16(rsrc, PM_BOUNDS_TOP)?;
    let b_left = be16(rsrc, PM_BOUNDS_LEFT)?;
    let b_bottom = be16(rsrc, PM_BOUNDS_BOTTOM)?;
    let b_right = be16(rsrc, PM_BOUNDS_RIGHT)?;
    let mb_top = be16(rsrc, MBM + BITMAP_BOUNDS_TOP)?;
    let mb_bottom = be16(rsrc, MBM + BITMAP_BOUNDS_BOTTOM)?;
    let mb_right = be16(rsrc, MBM + BITMAP_BOUNDS_RIGHT)?;
    let bb_top = be16(rsrc, BM + BITMAP_BOUNDS_TOP)?;
    let bb_bottom = be16(rsrc, BM + BITMAP_BOUNDS_BOTTOM)?;

    // Validate Rect ordering before subtracting (an inverted Rect would wrap).
    if b_right <= b_left || b_bottom <= b_top || mb_bottom < mb_top || bb_bottom < bb_top {
        return None;
    }
    let width = (b_right - b_left) as usize;
    let height = (b_bottom - b_top) as usize;
    let mbm_h = (mb_bottom - mb_top) as usize;
    let bm_h = (bb_bottom - bb_top) as usize;
    if width > 4096 || height > 4096 {
        return None;
    }

    let mbm_rb = be16(rsrc, MBM + BITMAP_ROWBYTES)? as usize;
    let bm_rb = be16(rsrc, BM + BITMAP_ROWBYTES)? as usize;
    let row_bytes = (be16(rsrc, PM_ROWBYTES)? & 0x7fff) as usize;

    // ColorTable sits after both bitmap data blocks; pixel data is the trailing
    // row_bytes*height bytes of the resource.
    let ct_off = MASK_DATA
        .checked_add(mbm_rb.checked_mul(mbm_h)?)?
        .checked_add(bm_rb.checked_mul(bm_h)?)?;
    let pix_off = len.checked_sub(row_bytes.checked_mul(height)?)?;
    let have_mask = mb_right != 0 && mb_bottom != 0;

    let palette = build_palette(rsrc, ct_off, bpp);

    let mut pixels = vec![0u8; width * height * 4];
    for y in 0..height {
        let id_row = pix_off + row_bytes * y;
        let mp_row = MASK_DATA + mbm_rb * y;
        for x in 0..width {
            let idx = match bpp {
                1 => (rsrc.get(id_row + (x >> 3)).copied()? >> ((7 - (x & 7)) as u32)) & 0x01,
                2 => (rsrc.get(id_row + (x >> 2)).copied()? >> (((3 - (x & 3)) * 2) as u32)) & 0x03,
                4 => (rsrc.get(id_row + (x >> 1)).copied()? >> (((1 - (x & 1)) * 4) as u32)) & 0x0f,
                _ => rsrc.get(id_row + x).copied()?,
            } as usize;
            let rgb = palette[idx];
            let a = if have_mask {
                match rsrc.get(mp_row + (x >> 3)) {
                    // Mac classic mask: 1 = visible, 0 = transparent.
                    Some(&mb) => {
                        if (mb >> ((7 - (x & 7)) as u32)) & 1 != 0 {
                            0xff
                        } else {
                            0x00
                        }
                    }
                    // Mask shorter than the icon → treat as opaque.
                    None => 0xff,
                }
            } else {
                0xff
            };
            let o = (y * width + x) * 4;
            pixels[o] = (rgb >> 16) as u8;
            pixels[o + 1] = (rgb >> 8) as u8;
            pixels[o + 2] = rgb as u8;
            pixels[o + 3] = a;
        }
    }

    Some(add_halo(&Rgba {
        width: width as u32,
        height: height as u32,
        pixels,
    }))
}

/// Add a 1px semi-transparent grey halo around the icon's opaque silhouette, so
/// it reads against both light and dark theme backgrounds. Grows the image by
/// 2px on each axis.
pub fn add_halo(src: &Rgba) -> Rgba {
    let (sw, sh) = (src.width as usize, src.height as usize);
    let (dw, dh) = (sw + 2, sh + 2);
    let mut pixels = vec![0u8; dw * dh * 4];

    // Copy the source into the (1, 1) inset.
    for y in 0..sh {
        for x in 0..sw {
            let s = (y * sw + x) * 4;
            let d = ((y + 1) * dw + (x + 1)) * 4;
            pixels[d..d + 4].copy_from_slice(&src.pixels[s..s + 4]);
        }
    }

    // Snapshot the alpha plane so freshly-painted halo pixels don't seed later
    // neighbour checks.
    let alpha: Vec<u8> = (0..dw * dh).map(|i| pixels[i * 4 + 3]).collect();
    for y in 0..dh {
        for x in 0..dw {
            if alpha[y * dw + x] != 0 {
                continue; // already part of the icon
            }
            let neighbour = (x > 0 && alpha[y * dw + x - 1] != 0)
                || (x < dw - 1 && alpha[y * dw + x + 1] != 0)
                || (y > 0 && alpha[(y - 1) * dw + x] != 0)
                || (y < dh - 1 && alpha[(y + 1) * dw + x] != 0);
            if !neighbour {
                continue;
            }
            let p = (y * dw + x) * 4;
            pixels[p] = 0x80; // medium grey reads on both light + dark themes
            pixels[p + 1] = 0x80;
            pixels[p + 2] = 0x80;
            pixels[p + 3] = 0xa0; // ~63% — definition without a heavy border
        }
    }

    Rgba {
        width: dw as u32,
        height: dh as u32,
        pixels,
    }
}

#[cfg(test)]
mod tests;
