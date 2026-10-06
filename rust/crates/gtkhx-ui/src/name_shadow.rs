//! Whether, and in what shade, the Users window shadows a name drawn over a
//! wide banner icon. Only a banner puts art behind the name; elsewhere a
//! halo just muddies the user's chosen color. Over a still banner the
//! shadow goes only where the name's contrast with the art is poor, and is
//! the opposite of the name so it lifts the name off the art. An animated
//! avatar's art changes frame to frame, so it gets a dark shadow, as
//! subtitles use.

use gtk::glib::translate::from_glib_none;
use gtk4 as gtk;

/// Below this WCAG contrast ratio a name is hard to read over the art.
const MIN_CONTRAST: f64 = 2.5;

/// The luminance at which black and white contrast equally with a color.
const MID_LUMINANCE: f64 = 0.179;

/// What a wide icon puts behind the name.
#[derive(Clone, Copy, Debug)]
enum Behind {
    /// An animated avatar, whose art changes frame to frame.
    Animated,
    /// Nothing opaque: the art lies past the name or is transparent there.
    Nothing,
    /// Still art, as its 10th- and 90th-percentile luminance, so busy art
    /// with both light and dark in it counts as both.
    Art(f64, f64),
}

/// WCAG relative luminance of sRGB components in 0..1.
fn rel_luminance(r: f64, g: f64, b: f64) -> f64 {
    let lin = |c: f64| {
        if c <= 0.04045 {
            c / 12.92
        } else {
            ((c + 0.055) / 1.055).powf(2.4)
        }
    };
    0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)
}

/// What the columns from `x0` across a typical name's width hold, in 8-bit
/// RGB(A) pixels.
fn art_behind(
    pixels: &[u8],
    width: usize,
    height: usize,
    stride: usize,
    channels: usize,
    x0: usize,
) -> Behind {
    let mut lums = Vec::new();
    for y in 0..height {
        for x in x0..width.min(x0 + 120) {
            let p = &pixels[y * stride + x * channels..][..channels];
            if channels < 4 || p[3] >= 128 {
                let c = |i: usize| f64::from(p[i]) / 255.0;
                lums.push(rel_luminance(c(0), c(1), c(2)));
            }
        }
    }
    if lums.is_empty() {
        return Behind::Nothing;
    }
    lums.sort_by(f64::total_cmp);
    Behind::Art(lums[lums.len() / 10], lums[lums.len() * 9 / 10])
}

/// The gray level of the shadow for a name in `fg` over `behind`, or `None`
/// for no shadow.
fn shadow_gray(fg: (f64, f64, f64), behind: Behind) -> Option<f32> {
    let lum = rel_luminance(fg.0, fg.1, fg.2);
    let nearest = match behind {
        Behind::Animated => return Some(0.0),
        Behind::Nothing => return None,
        Behind::Art(lo, hi) => lum.clamp(lo, hi),
    };
    let contrast = (lum.max(nearest) + 0.05) / (lum.min(nearest) + 0.05);
    (contrast < MIN_CONTRAST).then_some(if lum > MID_LUMINANCE { 0.0 } else { 1.0 })
}

/// [`art_behind`] a GdkPixbuf, into `out` as its two percentiles, or NaN
/// for nothing.
///
/// # Safety
/// `pixbuf` must be a valid GdkPixbuf and `out` valid for two writes.
#[no_mangle]
pub unsafe extern "C" fn hx_user_name_art_luminance(
    pixbuf: *mut gtk::gdk_pixbuf::ffi::GdkPixbuf,
    x0: i32,
    out: *mut f64,
) {
    let pb: gtk::gdk_pixbuf::Pixbuf = unsafe { from_glib_none(pixbuf) };
    let dim = |v: i32| usize::try_from(v).unwrap_or(0);
    let bytes = pb.read_pixel_bytes();
    let (lo, hi) = match art_behind(
        &bytes,
        dim(pb.width()),
        dim(pb.height()),
        dim(pb.rowstride()),
        dim(pb.n_channels()),
        dim(x0),
    ) {
        Behind::Art(lo, hi) => (lo, hi),
        _ => (f64::NAN, f64::NAN),
    };
    unsafe { out.write(lo) };
    unsafe { out.add(1).write(hi) };
}

/// The shadow's gray level for a name in `r`, `g`, `b` over the two
/// luminances [`hx_user_name_art_luminance`] gave, or NULL for an animated
/// avatar; negative for no shadow.
///
/// # Safety
/// `art` must be NULL or valid for two reads.
#[no_mangle]
pub unsafe extern "C" fn hx_user_name_shadow(r: f64, g: f64, b: f64, art: *const f64) -> f32 {
    let behind = if art.is_null() {
        Behind::Animated
    } else {
        match unsafe { (art.read(), art.add(1).read()) } {
            (lo, hi) if lo.is_nan() || hi.is_nan() => Behind::Nothing,
            (lo, hi) => Behind::Art(lo, hi),
        }
    };
    shadow_gray((r, g, b), behind).unwrap_or(-1.0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shadow_only_where_contrast_is_poor() {
        let white = (1.0, 1.0, 1.0);
        let purple = (
            0x90 as f64 / 255.0,
            0x50 as f64 / 255.0,
            0xD0 as f64 / 255.0,
        );
        let light = rel_luminance(0.85, 0.85, 0.85);
        let dark = rel_luminance(0.1, 0.1, 0.1);
        let mid = rel_luminance(0.4, 0.4, 0.4);
        for (fg, behind, want) in [
            (white, Behind::Art(light, light), Some(0.0)),
            (white, Behind::Art(dark, dark), None),
            (white, Behind::Art(dark, light), Some(0.0)),
            (purple, Behind::Art(light, light), None),
            (purple, Behind::Art(dark, dark), None),
            (purple, Behind::Art(mid, mid), Some(1.0)),
            (purple, Behind::Animated, Some(0.0)),
            (white, Behind::Nothing, None),
        ] {
            assert_eq!(shadow_gray(fg, behind), want, "fg {fg:?} behind {behind:?}");
        }
    }

    #[test]
    fn art_behind_reads_opaque_pixels_in_the_name_columns() {
        // 200 px wide, 3 rows padded to a longer stride: black where the
        // name sits, white past it, and a transparent white column to skip.
        let (w, h, stride) = (200, 3, 200 * 4 + 8);
        let mut px = vec![0u8; stride * h];
        for y in 0..h {
            for x in 0..w {
                let v = if x >= 130 || x == 20 { 255 } else { 0 };
                let a = if x == 20 { 0 } else { 255 };
                px[y * stride + x * 4..][..4].copy_from_slice(&[v, v, v, a]);
            }
        }
        assert!(
            matches!(art_behind(&px, w, h, stride, 4, 10), Behind::Art(lo, hi) if lo == 0.0 && hi == 0.0)
        );
        assert!(
            matches!(art_behind(&px, w, h, stride, 4, 140), Behind::Art(lo, hi) if lo == 1.0 && hi == 1.0)
        );
        assert!(matches!(
            art_behind(&px, w, h, stride, 4, 236),
            Behind::Nothing
        ));
        assert!(matches!(
            art_behind(&vec![0u8; stride * h], w, h, stride, 4, 10),
            Behind::Nothing
        ));
        let rgb = [255u8; 60 * 3];
        assert!(matches!(art_behind(&rgb, 60, 1, 60 * 3, 3, 10), Behind::Art(lo, _) if lo == 1.0));
    }
}
