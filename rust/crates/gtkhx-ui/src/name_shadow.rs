//! Keeping a user's name readable over what is behind it in the user list.
//!
//! Over the list's own background, a name whose color is too close to it is
//! lightened or darkened until it reads, keeping its hue. The background is
//! the theme's, and a selected row's mixes in the accent; the name is made
//! to read on both, so its color doesn't shift as the selection moves.
//!
//! Over a wide banner icon, the art varies too much for one color to fix,
//! so the name keeps its color and gets a soft shadow, only where its
//! contrast with the art is poor, opposite the name so it lifts the name
//! off the art. An animated avatar's art changes frame to frame, so it gets
//! a dark shadow, as subtitles use.

use std::ffi::c_int;

use gtk::gdk;
use gtk::glib::ffi::gboolean;
use gtk::glib::translate::from_glib_none;
use gtk4 as gtk;
use libadwaita as adw;

/// Below this WCAG contrast ratio a name is hard to read: WCAG's floor for
/// large text and interface elements.
const MIN_CONTRAST: f64 = 3.0;

/// libadwaita's view background, light and dark, which a theme that leaves
/// lists to the system keeps.
const ADW_VIEW_BG: [[f64; 3]; 2] = [
    [1.0, 1.0, 1.0],
    [
        0x1d as f64 / 255.0,
        0x1d as f64 / 255.0,
        0x20 as f64 / 255.0,
    ],
];

/// How much of the accent libadwaita mixes into a selected row.
const SELECTED_ACCENT: f64 = 0.25;

// gtkhx_theme.h's GTKHX_PAL_BG, GTKHX_CHROME_VIEW and GTKHX_CHROME_ACCENT.
const PAL_BG: c_int = 1;
const CHROME_VIEW: c_int = 1;
const CHROME_ACCENT: c_int = 8;

extern "C" {
    fn gtkhx_prefs_get_bool(name: *const std::ffi::c_char) -> c_int;
    fn gtkhx_theme_get_color(role: c_int, dark: gboolean) -> gdk::ffi::GdkRGBA;
    fn gtkhx_theme_get_chrome_color(
        role: c_int,
        dark: gboolean,
        out: *mut gdk::ffi::GdkRGBA,
    ) -> gboolean;
}

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

fn contrast(a: f64, b: f64) -> f64 {
    (a.max(b) + 0.05) / (a.min(b) + 0.05)
}

/// sRGB in 0..1 to OKLab.
fn to_oklab(c: [f64; 3]) -> [f64; 3] {
    let lin = c.map(|v| {
        if v <= 0.04045 {
            v / 12.92
        } else {
            ((v + 0.055) / 1.055).powf(2.4)
        }
    });
    let lms = [
        0.4122214708 * lin[0] + 0.5363325363 * lin[1] + 0.0514459929 * lin[2],
        0.2119034982 * lin[0] + 0.6806995451 * lin[1] + 0.1073969566 * lin[2],
        0.0883024619 * lin[0] + 0.2817188376 * lin[1] + 0.6299787005 * lin[2],
    ]
    .map(f64::cbrt);
    [
        0.2104542553 * lms[0] + 0.7936177850 * lms[1] - 0.0040720468 * lms[2],
        1.9779984951 * lms[0] - 2.4285922050 * lms[1] + 0.4505937099 * lms[2],
        0.0259040371 * lms[0] + 0.7827717662 * lms[1] - 0.8086757660 * lms[2],
    ]
}

/// OKLab to sRGB in 0..1, or `None` outside the sRGB gamut.
fn from_oklab(lab: [f64; 3]) -> Option<[f64; 3]> {
    let lms = [
        lab[0] + 0.3963377774 * lab[1] + 0.2158037573 * lab[2],
        lab[0] - 0.1055613458 * lab[1] - 0.0638541728 * lab[2],
        lab[0] - 0.0894841775 * lab[1] - 1.2914855480 * lab[2],
    ]
    .map(|v| v * v * v);
    let lin = [
        4.0767416621 * lms[0] - 3.3077115913 * lms[1] + 0.2309699292 * lms[2],
        -1.2684380046 * lms[0] + 2.6097574011 * lms[1] - 0.3413193965 * lms[2],
        -0.0041960863 * lms[0] - 0.7034186147 * lms[1] + 1.7076127010 * lms[2],
    ];
    lin.iter()
        .all(|v| (-1e-6..=1.0 + 1e-6).contains(v))
        .then(|| {
            lin.map(|v| {
                let v = v.clamp(0.0, 1.0);
                if v <= 0.0031308 {
                    v * 12.92
                } else {
                    1.055 * v.powf(1.0 / 2.4) - 0.055
                }
            })
        })
}

/// The color at OKLab lightness `l` with `lab`'s hue, at as much of its
/// chroma as sRGB can show there.
fn at_lightness(lab: [f64; 3], l: f64) -> [f64; 3] {
    let (mut fits, mut over) = (0.0, 1.0);
    if let Some(c) = from_oklab([l, lab[1], lab[2]]) {
        return c;
    }
    for _ in 0..12 {
        let k = (fits + over) / 2.0;
        if from_oklab([l, lab[1] * k, lab[2] * k]).is_some() {
            fits = k;
        } else {
            over = k;
        }
    }
    from_oklab([l, lab[1] * fits, lab[2] * fits]).unwrap_or([l; 3])
}

/// `fg`, lightened or darkened until it reads over backgrounds of luminance
/// `lo` to `hi`, keeping its hue and as much of its chroma as it can.
fn legible(fg: [f64; 3], lo: f64, hi: f64) -> [f64; 3] {
    let worst = |c: [f64; 3]| {
        let lum = rel_luminance(c[0], c[1], c[2]);
        contrast(lum, lum.clamp(lo, hi))
    };
    let reads = |c: [f64; 3]| worst(c) >= MIN_CONTRAST;
    if reads(fg) {
        return fg;
    }
    let lab = to_oklab(fg);
    // Toward whichever end contrasts more with both backgrounds: the only
    // one that can reach the target when just one can.
    let away = if worst(at_lightness(lab, 1.0)) >= worst(at_lightness(lab, 0.0)) {
        1.0
    } else {
        0.0
    };
    let (mut short, mut far) = (lab[0], away);
    for _ in 0..16 {
        let l = (short + far) / 2.0;
        if reads(at_lightness(lab, l)) {
            far = l;
        } else {
            short = l;
        }
    }
    at_lightness(lab, far)
}

/// The luminance of the user list's background, unselected and selected,
/// as gtkhx_refresh_css and gtkhx_refresh_userlist_css paint it: the theme's
/// chrome reaches libadwaita's named colors only while the window is
/// tinted, but a theme that colors content views paints the list in its
/// chat background either way.
fn list_backgrounds() -> (f64, f64) {
    let style = adw::StyleManager::default();
    let dark = gboolean::from(style.is_dark());
    let tint = unsafe { gtkhx_prefs_get_bool(crate::cs("TINTWINDOW").as_ptr()) } != 0;
    let chrome = |role| {
        let mut c = gdk::ffi::GdkRGBA {
            red: 0.0,
            green: 0.0,
            blue: 0.0,
            alpha: 0.0,
        };
        (unsafe { gtkhx_theme_get_chrome_color(role, dark, &mut c) } != 0)
            .then(|| [c.red, c.green, c.blue].map(f64::from))
    };
    let chat_bg = unsafe { gtkhx_theme_get_color(PAL_BG, dark) };
    let bg = match chrome(CHROME_VIEW) {
        Some(_) if chat_bg.alpha > 0.0 => [chat_bg.red, chat_bg.green, chat_bg.blue].map(f64::from),
        Some(view) if tint => view,
        _ => ADW_VIEW_BG[dark as usize],
    };
    let accent = chrome(CHROME_ACCENT).filter(|_| tint).unwrap_or_else(|| {
        let a = style.accent_color_rgba();
        [a.red(), a.green(), a.blue()].map(f64::from)
    });
    let selected = [0, 1, 2].map(|i| bg[i] + (accent[i] - bg[i]) * SELECTED_ACCENT);
    let lum = |c: [f64; 3]| rel_luminance(c[0], c[1], c[2]);
    (lum(bg), lum(selected))
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
    (contrast(lum, nearest) < MIN_CONTRAST).then_some(if lum > MID_LUMINANCE { 0.0 } else { 1.0 })
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

/// Readies a name in `fg` to draw. Off a banner, adjusts `fg` to read over
/// the list and returns -1. Over one, returns the shadow's gray level, or -1
/// for none, for the two luminances [`hx_user_name_art_luminance`] gave, or
/// NULL `art` for an animated avatar.
///
/// # Safety
/// `fg` must be valid for a read and a write, and `art` NULL or valid for
/// two reads.
#[no_mangle]
pub unsafe extern "C" fn hx_user_name_style(
    fg: *mut gdk::ffi::GdkRGBA,
    banner: gboolean,
    art: *const f64,
) -> f32 {
    let fg = unsafe { &mut *fg };
    let rgb = [fg.red, fg.green, fg.blue].map(f64::from);
    if banner == 0 {
        let (bg, selected) = list_backgrounds();
        [fg.red, fg.green, fg.blue] =
            legible(rgb, bg.min(selected), bg.max(selected)).map(|c| c as f32);
        return -1.0;
    }
    let behind = if art.is_null() {
        Behind::Animated
    } else {
        match unsafe { (art.read(), art.add(1).read()) } {
            (lo, hi) if lo.is_nan() || hi.is_nan() => Behind::Nothing,
            (lo, hi) => Behind::Art(lo, hi),
        }
    };
    shadow_gray((rgb[0], rgb[1], rgb[2]), behind).unwrap_or(-1.0)
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
    fn legible_lifts_only_names_too_close_to_the_list() {
        let page = rel_luminance(
            0x0f as f64 / 255.0,
            0x0d as f64 / 255.0,
            0x14 as f64 / 255.0,
        );
        let paper = rel_luminance(1.0, 1.0, 1.0);
        let dark_red = [0.5, 0.0, 0.0];
        let pale_yellow = [1.0, 1.0, 0.6];
        let lavender = [0.92, 0.9, 0.94];
        for (fg, lo, hi) in [
            (dark_red, page, 0.04),
            (pale_yellow, paper, paper),
            (lavender, page, 0.04),
            // Mid backgrounds whose average says darken, though only white
            // gets clear of the darker one.
            ([0.5, 0.3, 0.3], 0.09, 0.28),
        ] {
            let out = legible(fg, lo, hi);
            let lum = rel_luminance(out[0], out[1], out[2]);
            assert!(
                contrast(lum, lum.clamp(lo, hi)) >= MIN_CONTRAST - 0.01,
                "{fg:?} -> {out:?}"
            );
            if fg == lavender {
                assert_eq!(out, fg, "a name that already reads keeps its color");
            } else {
                let hue = |c: [f64; 3]| {
                    let lab = to_oklab(c);
                    lab[2].atan2(lab[1])
                };
                assert_ne!(out, fg);
                assert!(
                    (hue(out) - hue(fg)).abs() < 0.02,
                    "hue drifted: {fg:?} -> {out:?}"
                );
            }
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
