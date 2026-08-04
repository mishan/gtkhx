//! `hxsound` — cross-platform WAV notification-sound playback for GtkHx.
//!
//! Replaces the Linux-only GSound / libcanberra path in `src/sound.c`. The C
//! side still owns the event → filename mapping and the on-disk sound-file
//! search path; this crate is just the playback backend: given a path to a WAV
//! file, play it fire-and-forget.
//!
//! Design: a single lazily-spawned audio thread owns the (`!Send`) rodio
//! `OutputStream` and pulls play requests off an mpsc channel. It opens the
//! default output device at its own default config so we know the exact rate
//! the hardware runs at. Each request decodes the WAV, high-quality-resamples
//! it to that device rate (see below), and appends it to a **detached** `Sink`,
//! so the sound plays to completion on cpal's audio callback without blocking
//! the thread and overlapping notification sounds mix on the shared stream.
//!
//! Resampling: the notification WAVs are 8/11/22 kHz. rodio's built-in
//! sample-rate converter is plain linear interpolation, which leaves audible
//! aliasing when upsampling those to a 44.1/48 kHz output device. We instead
//! pre-resample each clip to the device's own rate with rubato's windowed-sinc
//! resampler, then hand rodio a buffer already at the device rate so rodio's
//! converter is a no-op. This matches the quality of the gsound/PipeWire path.
//!
//! [`hx_sound_play`] is the C entry point — thread-safe (it just sends on the
//! channel), and a graceful no-op when the path is bad or no audio device is
//! available (headless CI, no server), matching the old "sounds simply don't
//! play" behaviour on platforms without a backend.

use std::ffi::{c_char, CStr};
use std::fs::File;
use std::io::BufReader;
use std::path::PathBuf;
use std::sync::mpsc::{channel, Receiver, Sender};
use std::sync::OnceLock;
use std::thread;

use rodio::buffer::SamplesBuffer;
use rodio::cpal::traits::{DeviceTrait, HostTrait};
use rodio::{Decoder, OutputStream, OutputStreamHandle, Sink, Source};
use rubato::{
    Resampler, SincFixedIn, SincInterpolationParameters, SincInterpolationType, WindowFunction,
};

/// Lazily-spawned audio thread's request channel. `get_or_init` runs exactly
/// once; every caller thereafter clones the same `Sender`.
static PLAYER: OnceLock<Sender<PathBuf>> = OnceLock::new();

/// Honour the project-wide `GTKHX_DEBUG` category switch (see `src/debug.c`):
/// `GTKHX_DEBUG=sound` (or `all`) traces the output device rate and each clip's
/// resample decision, so the "is rodio's own converter really a no-op?"
/// assumption can be confirmed against real hardware.
fn sound_debug() -> bool {
    // Trim whitespace around each category to match the C side's debug_init()
    // (g_strstrip), so GTKHX_DEBUG="sound, login" behaves the same in both.
    std::env::var("GTKHX_DEBUG")
        .map(|v| v.split(',').any(|c| matches!(c.trim(), "sound" | "all")))
        .unwrap_or(false)
}

fn player() -> &'static Sender<PathBuf> {
    PLAYER.get_or_init(|| {
        let (tx, rx) = channel::<PathBuf>();
        // If the thread can't be spawned (effectively never), the returned
        // sender is simply never drained and every send is a dropped no-op.
        let _ = thread::Builder::new()
            .name("hxsound".into())
            .spawn(move || audio_loop(rx));
        tx
    })
}

/// Pick the output stream config to open the device with.
///
/// We build clips at whatever rate this returns so that rodio's own converter
/// (and, more importantly, the audio server's) does no extra resampling. The
/// obvious choice is `default_output_config()`, but cpal's ALSA backend
/// hardcodes a 44.1 kHz preference there: whenever the device advertises 44100
/// it reports 44100 as the default, *regardless of the rate the audio server's
/// graph is actually running at*. On modern Linux that graph is almost always
/// PipeWire (or PulseAudio) at 48 kHz, so taking cpal's 44100 at face value
/// makes us hand the server a 44.1 kHz stream, which it then resamples up to
/// 48 kHz — a second conversion stacked on ours, audibly duller than the single
/// high-quality resample `aplay` does when it feeds the server directly.
///
/// So on Linux, if the device can do 48 kHz with the same channels and sample
/// format as the default, prefer that: it matches the common graph rate, so the
/// server passes our audio through untouched and there is exactly one resample.
/// Other hosts (CoreAudio, WASAPI) report their device's true current rate as
/// the default and don't have this quirk, so we trust the default there and
/// leave this override Linux-only to avoid forcing a needless conversion on a
/// device whose real rate is 44.1 kHz.
fn choose_output_config(
    device: &rodio::cpal::Device,
) -> Result<rodio::cpal::SupportedStreamConfig, rodio::cpal::DefaultStreamConfigError> {
    let default = device.default_output_config()?;

    #[cfg(target_os = "linux")]
    {
        use rodio::cpal::SampleRate;
        const PREFERRED: SampleRate = SampleRate(48_000);
        if default.sample_rate() != PREFERRED {
            if let Ok(ranges) = device.supported_output_configs() {
                for r in ranges {
                    if r.channels() == default.channels()
                        && r.sample_format() == default.sample_format()
                        && r.min_sample_rate() <= PREFERRED
                        && PREFERRED <= r.max_sample_rate()
                    {
                        return Ok(r.with_sample_rate(PREFERRED));
                    }
                }
            }
        }
    }

    Ok(default)
}

fn audio_loop(rx: Receiver<PathBuf>) {
    // Open the default output device at its own default config so we know the
    // exact sample rate the stream runs at and can resample clips to match.
    // With no audio device (headless CI, no sound server) there's nothing to
    // play — return, dropping the receiver; subsequent sends fail silently, so
    // sounds are a no-op rather than a crash. Logged once so the "why is there
    // no sound" case is diagnosable.
    let host = rodio::cpal::default_host();
    let device = match host.default_output_device() {
        Some(d) => d,
        None => {
            eprintln!("hxsound: no default output device; sounds disabled");
            return;
        }
    };
    let config = match choose_output_config(&device) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("hxsound: no default output config ({e}); sounds disabled");
            return;
        }
    };
    let device_rate = config.sample_rate().0;
    if sound_debug() {
        let name = device.name().unwrap_or_else(|_| "<unknown>".into());
        eprintln!(
            "hxsound: cpal output device {name:?}; negotiated config: \
             {} ch, {:?}, {} Hz, buffer {:?}",
            config.channels(),
            config.sample_format(),
            device_rate,
            config.buffer_size(),
        );
    }
    let (_stream, handle) = match OutputStream::try_from_device_config(&device, config) {
        Ok(pair) => pair,
        Err(e) => {
            eprintln!("hxsound: cannot open output stream ({e}); sounds disabled");
            return;
        }
    };
    if sound_debug() {
        eprintln!(
            "hxsound: opened output stream at {device_rate} Hz; \
             clips are pre-resampled to this where possible so neither rodio nor \
             the audio server resamples again (per-clip trace below shows the path)"
        );
    }
    for path in rx {
        play_one(&handle, device_rate, &path);
    }
}

fn play_one(handle: &OutputStreamHandle, device_rate: u32, path: &PathBuf) {
    let file = match File::open(path) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("hxsound: open {}: {e}", path.display());
            return;
        }
    };
    let decoder = match Decoder::new(BufReader::new(file)) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("hxsound: decode {}: {e}", path.display());
            return;
        }
    };

    let src_channels = decoder.channels();
    let src_rate = decoder.sample_rate();
    // rodio decoders yield i16 samples; normalise to f32 in [-1, 1].
    let decoded: Vec<f32> = decoder.map(|s| s as f32 / 32768.0).collect();
    if decoded.is_empty() || src_channels == 0 {
        return;
    }

    // Downmix to mono before resampling — see downmix_to_mono for why this is
    // load-bearing (it dodges a rodio stereo-playback hazard), not just tidiness.
    let interleaved = downmix_to_mono(&decoded, src_channels);
    let channels = 1u16;

    // Resample to the device rate so rodio's own converter is a pass-through.
    // If the resampler can't be built we fall back to the original samples at
    // their *source* rate and let rodio's (lower-quality) converter handle it —
    // tagging the buffer with device_rate there would make rodio treat
    // unresampled audio as already-correct and play it at the wrong speed/pitch.
    let (samples, rate) = if src_rate == device_rate {
        (interleaved, device_rate)
    } else {
        match resample_to(&interleaved, channels, src_rate, device_rate) {
            Some(resampled) => (resampled, device_rate),
            None => (interleaved, src_rate),
        }
    };

    if sound_debug() {
        let mode = if src_rate == device_rate {
            "passthrough (already at device rate)"
        } else if rate == device_rate {
            "pre-resampled to device rate"
        } else {
            "resample FAILED -> rodio's linear converter runs (lower quality)"
        };
        eprintln!(
            "hxsound: {} -- {} ch (mono-downmixed), {} Hz -> {} Hz [{}]",
            path.display(),
            src_channels,
            src_rate,
            rate,
            mode
        );
    }

    // When rate == device_rate rodio's sample-rate converter is a pass-through;
    // either way it up/down-mixes channels to match the output (e.g. mono →
    // stereo), which is cheap and artefact-free.
    let buffer = SamplesBuffer::new(channels, rate, samples);
    match Sink::try_new(handle) {
        Ok(sink) => {
            sink.append(buffer);
            // Detach so playback runs to completion on cpal's callback thread
            // without us holding the Sink; the loop is free to serve the next
            // request and concurrent sounds mix on the shared stream.
            sink.detach();
        }
        Err(e) => eprintln!("hxsound: sink: {e}"),
    }
}

/// Average `channels`-interleaved samples down to a single mono channel. Mono
/// in, mono out. A short fire-and-forget notification blip gains nothing from
/// stereo, and mixing down sidesteps a rodio playback hazard: its mixer/queue
/// idle-filler is a *mono* silence, and a multi-channel buffer handed in can end
/// up read against that mono frame, clocking the interleaved L,R,L,R samples out
/// sequentially — the clip then plays at half speed (an octave down) with heavy
/// near-Nyquist buzz. A mono source matches the idle format and always plays
/// correctly. Averaging (rather than dropping a channel) keeps hard-panned
/// content audible.
fn downmix_to_mono(interleaved: &[f32], channels: u16) -> Vec<f32> {
    let ch = channels.max(1) as usize;
    if ch == 1 {
        return interleaved.to_vec();
    }
    let frames = interleaved.len() / ch;
    let mut mono = Vec::with_capacity(frames);
    for f in 0..frames {
        let sum: f32 = interleaved[f * ch..f * ch + ch].iter().sum();
        mono.push(sum / ch as f32);
    }
    mono
}

/// High-quality (windowed-sinc) resample of interleaved f32 audio from `from`
/// to `to` Hz, returning the resampled interleaved samples. Returns `None` if
/// the resampler can't be constructed, so the caller can fall back to the
/// original samples (at their original rate) and let rodio resample instead.
fn resample_to(interleaved: &[f32], channels: u16, from: u32, to: u32) -> Option<Vec<f32>> {
    let ch = channels as usize;
    let in_frames = interleaved.len() / ch;
    if in_frames == 0 {
        return Some(Vec::new());
    }

    // Deinterleave into rubato's planar (per-channel) layout.
    let mut planar: Vec<Vec<f32>> = vec![Vec::with_capacity(in_frames); ch];
    for (i, s) in interleaved.iter().enumerate() {
        planar[i % ch].push(*s);
    }

    let ratio = to as f64 / from as f64;
    let params = SincInterpolationParameters {
        sinc_len: 256,
        f_cutoff: 0.95,
        interpolation: SincInterpolationType::Linear,
        oversampling_factor: 256,
        window: WindowFunction::BlackmanHarris2,
    };
    const CHUNK: usize = 1024;
    let mut resampler = match SincFixedIn::<f32>::new(ratio, 2.0, params, CHUNK, ch) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("hxsound: resampler init {from}->{to}: {e}");
            return None;
        }
    };

    // SincFixedIn consumes exactly CHUNK input frames per call; feed full chunks
    // and zero-pad the final short one.
    let mut out_planar: Vec<Vec<f32>> = vec![Vec::new(); ch];
    let mut inbuf: Vec<Vec<f32>> = vec![vec![0.0f32; CHUNK]; ch];
    let mut pos = 0usize;
    while pos < in_frames {
        let n = (in_frames - pos).min(CHUNK);
        for c in 0..ch {
            for i in 0..CHUNK {
                inbuf[c][i] = if i < n { planar[c][pos + i] } else { 0.0 };
            }
        }
        if let Ok(out) = resampler.process(&inbuf, None) {
            for c in 0..ch {
                out_planar[c].extend_from_slice(&out[c]);
            }
        }
        pos += CHUNK;
    }

    // Keep the whole resampled clip from frame 0 — we deliberately do NOT discard
    // `output_delay()` leading frames the way a latency-compensating stream would.
    // For these very short notification clips rubato's reported delay (hundreds of
    // output frames for a 256-tap sinc) massively overstates the true group delay,
    // so chopping it discards the clip's onset outright. That was a real bug: a
    // clip whose whole sound is a tick in the first few milliseconds came out
    // ~15 dB quieter with no attack, because the trim ate the onset. The true
    // leading latency here is sub-millisecond and inaudible for fire-and-forget
    // playback, so starting at frame 0 is both onset-safe and correct-sounding.
    // Capping the length at the ideal frame count also trims the silent tail left
    // by zero-padding the last input chunk.
    // Take the shortest channel so the interleave loop can never index past a
    // shorter slice, even if per-channel output lengths ever diverge.
    let produced = out_planar.iter().map(|c| c.len()).min().unwrap_or(0);
    let expected = (in_frames as f64 * ratio).round() as usize;
    let out_frames = expected.min(produced);

    let mut out = Vec::with_capacity(out_frames * ch);
    for f in 0..out_frames {
        for plane in &out_planar {
            out.push(plane[f]);
        }
    }
    Some(out)
}

/// Decode a C path string into a `PathBuf`. On Unix the raw bytes are the
/// path; elsewhere (Windows) GLib hands us UTF-8, which std maps to the OS's
/// wide encoding at the filesystem boundary.
fn cpath_to_pathbuf(cstr: &CStr) -> Option<PathBuf> {
    #[cfg(unix)]
    {
        use std::ffi::OsStr;
        use std::os::unix::ffi::OsStrExt;
        Some(PathBuf::from(OsStr::from_bytes(cstr.to_bytes())))
    }
    #[cfg(not(unix))]
    {
        cstr.to_str().ok().map(PathBuf::from)
    }
}

/// `void hx_sound_play (const char *path)` — play the WAV at `path`,
/// fire-and-forget. Thread-safe (callable from any thread); a no-op on a NULL
/// path, an unreadable / non-WAV file, or when no audio device is available.
///
/// # Safety
/// `path` must be NULL or a valid NUL-terminated C string, valid for the
/// duration of the call.
#[no_mangle]
pub unsafe extern "C" fn hx_sound_play(path: *const c_char) {
    if path.is_null() {
        return;
    }
    if let Some(pb) = cpath_to_pathbuf(CStr::from_ptr(path)) {
        // Best-effort: a full/closed channel just means the sound is skipped.
        let _ = player().send(pb);
    }
}

#[cfg(test)]
mod tests {
    use super::resample_to;

    /// A 6× upsample (8 k → 48 k) yields roughly 6× the frames, in range, and
    /// finite — the core of the quality fix. Approximate because the resampler
    /// trims its own leading delay.
    #[test]
    fn upsample_mono_length_and_finiteness() {
        let in_frames = 4000;
        let input: Vec<f32> = (0..in_frames)
            .map(|i| (i as f32 * 0.05).sin() * 0.5)
            .collect();
        let out = resample_to(&input, 1, 8000, 48000).expect("resampler builds");
        let expected = in_frames * 6;
        // Within a few percent of the ideal 6× ratio.
        assert!(
            out.len() as f64 > expected as f64 * 0.95 && out.len() as f64 <= expected as f64 * 1.02,
            "got {} frames, expected ~{expected}",
            out.len()
        );
        assert!(out.iter().all(|s| s.is_finite() && s.abs() <= 1.5));
    }

    /// Stereo stays interleaved: output length is a whole number of frames.
    #[test]
    fn upsample_stereo_stays_interleaved() {
        let frames = 2000;
        let mut input = Vec::with_capacity(frames * 2);
        for i in 0..frames {
            input.push((i as f32 * 0.03).sin()); // L
            input.push((i as f32 * 0.07).cos()); // R
        }
        let out = resample_to(&input, 2, 22050, 44100).expect("resampler builds");
        assert_eq!(out.len() % 2, 0, "stereo output must stay frame-aligned");
        assert!(
            out.len() >= frames * 2,
            "44.1k from 22.05k should ~2× frames"
        );
        assert!(out.iter().all(|s| s.is_finite()));
    }

    /// A short clip whose energy is a transient right at the start — the failure
    /// class where the whole sound lives in the first few milliseconds. Regression
    /// guard: the resampler must keep the onset, not discard it as "algorithmic
    /// delay". The pre-fix behaviour trimmed `output_delay()` leading frames, which
    /// for a clip like this ate the entire attack and left it ~15 dB quieter.
    #[test]
    fn short_clip_onset_survives() {
        let ch = 2u16;
        let frames = 1000; // ~45 ms at 22.05 kHz, like the stereo notification clip
        let mut input = vec![0.0f32; frames * ch as usize];
        // A short decaying tick in the first ~40 frames, on both channels.
        for i in 0..40 {
            let v = 0.5 * (1.0 - i as f32 / 40.0);
            input[i * 2] = v;
            input[i * 2 + 1] = v;
        }
        let in_peak = input.iter().cloned().fold(0.0f32, |m, x| m.max(x.abs()));
        let out = resample_to(&input, ch, 22050, 48000).expect("resampler builds");
        let out_peak = out.iter().cloned().fold(0.0f32, |m, x| m.max(x.abs()));
        // Onset must survive: allow modest sinc overshoot, but the pre-fix collapse
        // (peak dropping to ~0.18x input) must never recur.
        assert!(
            out_peak > in_peak * 0.8,
            "onset lost: in_peak {in_peak}, out_peak {out_peak}"
        );
        assert!(out_peak < in_peak * 1.4, "unexpected overshoot: {out_peak}");
    }

    /// Stereo downmix averages channels and halves the sample count, so the
    /// buffer handed to rodio is genuinely mono. Guards the fix for the stereo
    /// clip that played an octave low with near-Nyquist buzz when a 2-channel
    /// buffer was handed to rodio's mono-idle mixer.
    #[test]
    fn downmix_stereo_to_mono() {
        use super::downmix_to_mono;
        // interleaved L,R: (1,-1),(0.5,0.5),(0.2,0.4)
        let stereo = [1.0, -1.0, 0.5, 0.5, 0.2, 0.4];
        let mono = downmix_to_mono(&stereo, 2);
        assert_eq!(mono.len(), 3, "stereo->mono must halve the sample count");
        assert!((mono[0] - 0.0).abs() < 1e-6); // (1 + -1)/2
        assert!((mono[1] - 0.5).abs() < 1e-6); // (0.5 + 0.5)/2
        assert!((mono[2] - 0.3).abs() < 1e-6); // (0.2 + 0.4)/2
        // Mono input passes through untouched.
        let m = downmix_to_mono(&[0.1, -0.2, 0.3], 1);
        assert_eq!(m, vec![0.1, -0.2, 0.3]);
    }

    /// Empty input yields an empty result rather than panicking.
    #[test]
    fn empty_input_is_noop() {
        let empty: [f32; 0] = [];
        assert!(resample_to(&empty, 1, 8000, 48000).unwrap().is_empty());
    }
}
