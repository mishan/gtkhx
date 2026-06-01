//! HOPE transport compression for GtkHx.
//!
//! Replaces `compress.c`. Provides streaming compress/decompress for:
//!   - GZIP (RFC 1950 zlib deflate/inflate with Z_SYNC_FLUSH)
//!   - LZ4 (LZ4F frame format, Janus/Klein extension; spec v1.6)
//!   - ZSTD (Zstandard, RFC 8878; Janus/Klein extension)
//!
//! The HOPE wire format names the algorithm "GZIP" but the bytes are RFC
//! 1950 zlib (with the 2-byte zlib header and 4-byte trailing Adler-32),
//! NOT the RFC 1952 gzip wire format. `flate2::Compress` /
//! `flate2::Decompress` are constructed with `zlib_header=true` so the
//! output is interoperable with every hx-family server's deflateInit/
//! inflateInit. "LZ4" on the HOPE wire means LZ4F frames (one complete
//! frame per transaction); raw LZ4 blocks would not be interoperable.
//!
//! GZIP uses a persistent codec context per connection direction, matching
//! the wire protocol's requirement that deflate/inflate state carries
//! across transactions within a single connection. LZ4F and ZSTD treat
//! each transaction as a self-contained frame (the C-side dispatcher used
//! LZ4F_compressBegin/Update/End per call, and ZSTD_e_flush per call).

#![allow(unsafe_op_in_unsafe_fn)]

use std::io::{Cursor, Write};
use std::slice;

use flate2::{Compress, Decompress, FlushCompress, FlushDecompress, Status};

// zstd::stream::raw::Operation is the trait that provides `run()` /
// `flush()` on Encoder / Decoder; without it in scope the method
// calls below don't compile. (Looks unused at a glance — a linter
// might flag it — but it's load-bearing for trait-method dispatch.)
#[cfg(feature = "zstd")]
use zstd::stream::raw::Operation;

/// Compression algorithm IDs matching the C defines.
pub const COMPRESS_NONE: u16 = 0;
pub const COMPRESS_GZIP: u16 = 1;
pub const COMPRESS_LZ4: u16 = 2;
pub const COMPRESS_ZSTD: u16 = 3;

/// Opaque compression encoder state.
pub enum CompressEncoder {
    Gzip(Compress),
    #[cfg(feature = "lz4")]
    Lz4,
    #[cfg(feature = "zstd")]
    Zstd(zstd::stream::raw::Encoder<'static>),
}

/// Opaque compression decoder state.
pub enum CompressDecoder {
    Gzip(Decompress),
    #[cfg(feature = "lz4")]
    Lz4(Lz4StreamDecoder),
    #[cfg(feature = "zstd")]
    Zstd(zstd::stream::raw::Decoder<'static>),
}

/// Persistent state for the LZ4F decode path. The Hotline wire
/// protocol stores compression context across transactions inside
/// a single connection — the production `compress_decode` caller
/// passes a small `out_cap` (often `htlc->in.len`, ~22 bytes for
/// a header) on each call, and the codec is expected to remember
/// where it is in the frame so multiple calls can drain one frame.
/// `lz4_flex::frame::FrameDecoder` wraps a `Read` and maintains its
/// own internal state across `read()` calls, so we own one of those
/// across the whole connection (per-direction) and feed it via a
/// `Lz4InputBuf` we extend between calls.
#[cfg(feature = "lz4")]
pub struct Lz4StreamDecoder {
    decoder: lz4_flex::frame::FrameDecoder<Lz4InputBuf>,
    /// Plaintext bytes produced but not yet handed to the C caller —
    /// drained in `out_cap`-sized pieces across multiple
    /// `gtkhx_compress_decode` calls when the caller-provided
    /// output buffer is smaller than the frame's decompressed size.
    output_staging: Vec<u8>,
}

/// `Read` implementation backed by a Vec the outer
/// `Lz4StreamDecoder` extends between calls. Returns `WouldBlock`
/// (not `Ok(0)`) on exhaustion so the wrapping `FrameDecoder`
/// doesn't treat the temporary empty state as end-of-frame.
#[cfg(feature = "lz4")]
struct Lz4InputBuf {
    bytes: Vec<u8>,
    pos: usize,
}

#[cfg(feature = "lz4")]
impl std::io::Read for Lz4InputBuf {
    fn read(&mut self, dst: &mut [u8]) -> std::io::Result<usize> {
        let avail = self.bytes.len() - self.pos;
        if avail == 0 {
            return Err(std::io::Error::new(
                std::io::ErrorKind::WouldBlock,
                "no LZ4 input buffered — feed more or call again later",
            ));
        }
        let n = avail.min(dst.len());
        dst[..n].copy_from_slice(&self.bytes[self.pos..self.pos + n]);
        self.pos += n;
        Ok(n)
    }
}

// ---- Encoder FFI --------------------------------------------------------

/// Create a new encoder for the given algorithm.
/// Returns null if the algorithm is unsupported, COMPRESS_NONE, or
/// the underlying codec failed to initialise. The C dispatcher in
/// compress.c maps a null return to "no compression" rather than
/// driving an uninitialised state forward.
#[no_mangle]
pub extern "C" fn gtkhx_compress_encoder_new(algo: u16) -> *mut CompressEncoder {
    let encoder = match algo {
        // zlib_header=true selects RFC 1950 (zlib wrapper), which is
        // what the HOPE "GZIP" algorithm actually wires. Despite the
        // protocol name, this is NOT RFC 1952 gzip. Wire-compat with
        // every hx-family server's deflateInit/inflateInit.
        COMPRESS_GZIP => {
            CompressEncoder::Gzip(Compress::new(flate2::Compression::default(), true))
        }
        #[cfg(feature = "lz4")]
        COMPRESS_LZ4 => CompressEncoder::Lz4,
        #[cfg(feature = "zstd")]
        COMPRESS_ZSTD => match zstd::stream::raw::Encoder::new(3) {
            Ok(enc) => CompressEncoder::Zstd(enc),
            Err(_) => return std::ptr::null_mut(),
        },
        _ => return std::ptr::null_mut(),
    };
    Box::into_raw(Box::new(encoder))
}

/// Free an encoder.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_compress_encoder_free(enc: *mut CompressEncoder) {
    if !enc.is_null() {
        drop(Box::from_raw(enc));
    }
}

/// Compress data. Returns the number of compressed bytes written to `out`,
/// or 0 on error. The `out` buffer should be large enough (suggest 2x input + 64).
///
/// # Safety
/// `input` must be valid for `input_len` bytes. `out` must be valid for `out_cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_compress_encode(
    enc: *mut CompressEncoder,
    input: *const u8,
    input_len: u32,
    out: *mut u8,
    out_cap: u32,
) -> u32 {
    if enc.is_null() || input.is_null() || out.is_null() {
        return 0;
    }

    let encoder = &mut *enc;
    let input_slice = slice::from_raw_parts(input, input_len as usize);
    let out_slice = slice::from_raw_parts_mut(out, out_cap as usize);

    match encoder {
        CompressEncoder::Gzip(ref mut compress) => {
            let before_in = compress.total_in();
            let before_out = compress.total_out();
            match compress.compress(input_slice, out_slice, FlushCompress::Sync) {
                // BufError specifically means "output buffer too small
                // to hold the full sync-flushed payload" — what we have
                // in out_slice is a truncated frame, so return 0 and
                // let the C dispatcher surface the failure rather than
                // send a malformed compressed transaction.
                //
                // Status::Ok with consumed_in != input_len would also
                // be a truncation; belt-and-suspenders check after the
                // status-level filter.
                Ok(Status::Ok | Status::StreamEnd) => {
                    let consumed_in = compress.total_in() - before_in;
                    if consumed_in != input_len as u64 {
                        return 0;
                    }
                    (compress.total_out() - before_out) as u32
                }
                Ok(Status::BufError) | Err(_) => 0,
            }
        }
        #[cfg(feature = "lz4")]
        CompressEncoder::Lz4 => {
            // HOPE LZ4 is LZ4F frame format — one self-contained
            // frame per transaction. lz4_flex::frame::FrameEncoder
            // wraps a Writer; we wrap a Cursor over the output slice
            // so position() gives us byte count after finish().
            let out_cursor = Cursor::new(out_slice);
            let mut frame_enc = lz4_flex::frame::FrameEncoder::new(out_cursor);
            if frame_enc.write_all(input_slice).is_err() {
                return 0;
            }
            // finish() flushes the frame trailer and gives the wrapped
            // Cursor back. position() is the total frame size we
            // produced.
            match frame_enc.finish() {
                Ok(final_cursor) => final_cursor.position() as u32,
                Err(_) => 0,
            }
        }
        #[cfg(feature = "zstd")]
        CompressEncoder::Zstd(ref mut encoder) => {
            // Wrap the caller's out_slice directly in OutBuffer so
            // the encoder writes straight into it — no temporary Vec
            // + memcpy per transaction.
            let mut in_buf = zstd::stream::raw::InBuffer::around(input_slice);
            let mut out_buf = zstd::stream::raw::OutBuffer::around(out_slice);

            loop {
                match encoder.run(&mut in_buf, &mut out_buf) {
                    Ok(0) if in_buf.pos() >= in_buf.src.len() => break,
                    Ok(_) if in_buf.pos() >= in_buf.src.len() => break,
                    Ok(_) => continue,
                    Err(_) => return 0,
                }
            }

            // Flush
            loop {
                match encoder.flush(&mut out_buf) {
                    Ok(0) => break,
                    Ok(_) => continue,
                    Err(_) => return 0,
                }
            }

            out_buf.pos() as u32
        }
    }
}

// ---- Decoder FFI --------------------------------------------------------

/// Create a new decoder for the given algorithm. Returns null on
/// unsupported algorithm, COMPRESS_NONE, or codec init failure;
/// same fail-closed contract as `gtkhx_compress_encoder_new`.
#[no_mangle]
pub extern "C" fn gtkhx_compress_decoder_new(algo: u16) -> *mut CompressDecoder {
    let decoder = match algo {
        // zlib_header=true matches the encoder side; see the comment
        // on gtkhx_compress_encoder_new for the rationale.
        COMPRESS_GZIP => CompressDecoder::Gzip(Decompress::new(true)),
        #[cfg(feature = "lz4")]
        COMPRESS_LZ4 => CompressDecoder::Lz4(Lz4StreamDecoder {
            decoder: lz4_flex::frame::FrameDecoder::new(Lz4InputBuf {
                bytes: Vec::new(),
                pos: 0,
            }),
            output_staging: Vec::new(),
        }),
        #[cfg(feature = "zstd")]
        COMPRESS_ZSTD => match zstd::stream::raw::Decoder::new() {
            Ok(dec) => CompressDecoder::Zstd(dec),
            Err(_) => return std::ptr::null_mut(),
        },
        _ => return std::ptr::null_mut(),
    };
    Box::into_raw(Box::new(decoder))
}

/// Free a decoder.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_compress_decoder_free(dec: *mut CompressDecoder) {
    if !dec.is_null() {
        drop(Box::from_raw(dec));
    }
}

/// Decompress data. Returns the number of decompressed bytes written to `out`,
/// or 0 on error. Writes the number of input bytes consumed to `*in_used`.
///
/// # Safety
/// All pointers must be valid for their respective lengths.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_compress_decode(
    dec: *mut CompressDecoder,
    input: *const u8,
    input_len: u32,
    out: *mut u8,
    out_cap: u32,
    in_used: *mut u32,
) -> u32 {
    if dec.is_null() || out.is_null() || in_used.is_null() {
        return 0;
    }
    /* NULL input with input_len > 0 is a caller bug; NULL with
     * input_len == 0 is the legitimate "drain only" case for the
     * streaming LZ4 path — when production's rcv loop calls
     * compress_decode without any new bytes (just to pull more out
     * of internal staging), the test fixtures pass NULL/0 here
     * rather than synthesising a dangling-but-non-null pointer. */
    if input.is_null() && input_len != 0 {
        return 0;
    }

    let decoder = &mut *dec;
    let input_slice: &[u8] = if input_len == 0 {
        &[]
    } else {
        slice::from_raw_parts(input, input_len as usize)
    };
    let out_slice = slice::from_raw_parts_mut(out, out_cap as usize);

    match decoder {
        CompressDecoder::Gzip(ref mut decompress) => {
            let before_in = decompress.total_in();
            let before_out = decompress.total_out();
            match decompress.decompress(input_slice, out_slice, FlushDecompress::Sync) {
                Ok(Status::Ok | Status::StreamEnd | Status::BufError) => {
                    *in_used = (decompress.total_in() - before_in) as u32;
                    (decompress.total_out() - before_out) as u32
                }
                Err(_) => {
                    *in_used = 0;
                    0
                }
            }
        }
        #[cfg(feature = "lz4")]
        CompressDecoder::Lz4(ref mut state) => {
            // Streaming LZ4F decode. Production calls compress_decode
            // with out_cap == htlc->in.len (e.g. 22 bytes for a
            // header), but a single LZ4F frame can decompress to many
            // KB — so the decoder needs persistent state across
            // calls and an output staging buffer that drains in
            // out_cap-sized pieces.
            //
            // The shape:
            //   - Append new input bytes to the persistent
            //     FrameDecoder's input buffer (Lz4InputBuf).
            //   - Pump the FrameDecoder until WouldBlock (out of
            //     input) or until we've staged at least out_cap
            //     output bytes.
            //   - Hand the caller up to out_cap bytes from staging;
            //     keep the rest for the next call.
            //   - Always report in_used == input_len (the caller's
            //     bytes are now in our buffer; we'll process them
            //     on subsequent calls if necessary). The upstream
            //     rcv loop iterates and feeds more network bytes
            //     when our output is short; forward progress is
            //     guaranteed once a complete frame has been buffered.
            {
                let inner = state.decoder.get_mut();
                // Compact the consumed prefix so the buffer doesn't
                // grow unboundedly across many calls. Position
                // resets to 0 after the drain.
                if inner.pos > 0 {
                    inner.bytes.drain(..inner.pos);
                    inner.pos = 0;
                }
                inner.bytes.extend_from_slice(input_slice);
            }

            use std::io::Read;
            loop {
                if state.output_staging.len() >= out_slice.len() {
                    break;
                }
                let mut tmp = [0u8; 4096];
                match state.decoder.read(&mut tmp) {
                    Ok(0) => break, // frame ended; future reads stay at 0
                    Ok(n) => state.output_staging.extend_from_slice(&tmp[..n]),
                    Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                        // Out of buffered input — caller needs to
                        // hand us more bytes on a subsequent call.
                        break;
                    }
                    Err(_) => {
                        // Genuine decode error — corrupted frame or
                        // similar. Fail closed.
                        *in_used = 0;
                        return 0;
                    }
                }
            }

            let to_caller = state.output_staging.len().min(out_slice.len());
            out_slice[..to_caller]
                .copy_from_slice(&state.output_staging[..to_caller]);
            state.output_staging.drain(..to_caller);

            *in_used = input_len;
            to_caller as u32
        }
        #[cfg(feature = "zstd")]
        CompressDecoder::Zstd(ref mut decoder) => {
            let mut in_buf = zstd::stream::raw::InBuffer::around(input_slice);
            let mut out_buf = zstd::stream::raw::OutBuffer::around(out_slice);

            loop {
                match decoder.run(&mut in_buf, &mut out_buf) {
                    Ok(0) => break,
                    Ok(_) if in_buf.pos() >= in_buf.src.len() => break,
                    Ok(_) => continue,
                    Err(_) => {
                        *in_used = 0;
                        return 0;
                    }
                }
            }

            *in_used = in_buf.pos() as u32;
            out_buf.pos() as u32
        }
    }
}

/// Map a HOPE algorithm name to its numeric ID.
/// Returns COMPRESS_NONE for unrecognized or empty names.
///
/// # Safety
/// `name` must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn gtkhx_compress_id_from_name(name: *const std::ffi::c_char) -> u16 {
    if name.is_null() {
        return COMPRESS_NONE;
    }
    let cstr = match std::ffi::CStr::from_ptr(name).to_str() {
        Ok(s) => s,
        Err(_) => return COMPRESS_NONE,
    };
    match cstr {
        "NONE" | "" => COMPRESS_NONE,
        "GZIP" => COMPRESS_GZIP,
        #[cfg(feature = "lz4")]
        "LZ4" => COMPRESS_LZ4,
        #[cfg(feature = "zstd")]
        "ZSTD" => COMPRESS_ZSTD,
        _ => COMPRESS_NONE,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gzip_roundtrip() {
        let input = b"Hello, Hotline compression! This is a test of the GZIP codec.";
        let mut compressed = vec![0u8; input.len() * 2 + 64];
        let mut decompressed = vec![0u8; input.len() * 2];

        unsafe {
            let enc = gtkhx_compress_encoder_new(COMPRESS_GZIP);
            assert!(!enc.is_null());

            let comp_len = gtkhx_compress_encode(
                enc,
                input.as_ptr(),
                input.len() as u32,
                compressed.as_mut_ptr(),
                compressed.len() as u32,
            );
            assert!(comp_len > 0);
            // RFC 1950 zlib stream starts with two bytes: CMF (typically
            // 0x78 for deflate, 32K window) followed by FLG. Pinning the
            // first byte catches a regression where Compress::new is
            // called with zlib_header=false (raw DEFLATE, which would
            // produce different leading bytes and break wire-compat
            // with hx-family servers).
            assert_eq!(compressed[0], 0x78,
                       "GZIP output should start with zlib's CMF byte (0x78), not raw DEFLATE");
            gtkhx_compress_encoder_free(enc);

            let dec = gtkhx_compress_decoder_new(COMPRESS_GZIP);
            assert!(!dec.is_null());

            let mut in_used = 0u32;
            let decomp_len = gtkhx_compress_decode(
                dec,
                compressed.as_ptr(),
                comp_len,
                decompressed.as_mut_ptr(),
                decompressed.len() as u32,
                &mut in_used,
            );
            assert_eq!(decomp_len, input.len() as u32);
            assert_eq!(&decompressed[..input.len()], input);
            gtkhx_compress_decoder_free(dec);
        }
    }

    #[test]
    fn gzip_encoder_buferror_is_a_failure_not_a_truncation() {
        // BufError from flate2 means the output buffer couldn't hold
        // the full sync-flushed payload. Verify we surface that as a
        // failure (return 0) rather than returning a truncated frame.
        // We trip BufError by passing out_cap == 0 — flate2 can't
        // even write the zlib header (CMF + FLG = 2 bytes), so this
        // exercises the BufError path deterministically.
        //
        // Note: with a SLIGHTLY-too-small out_cap, flate2 can return
        // Status::Ok with total_in == input_len AND a truncated
        // output — total_in is advanced as flate2 reads through the
        // input window even when the output stream isn't faithfully
        // representing those bytes. That looks like success at the
        // total_in level but isn't. The C dispatcher (src/compress.c
        // :: encode_bufsize) always allocates `2 * len + 1024`,
        // which is comfortably above zlib's worst-case expansion
        // for the Hotline-transaction sizes we ever encode, so the
        // truncation corner is unreachable through the production
        // call site. This test pins only the BufError edge — the
        // one Copilot flagged as the actually-observable regression.
        let input = vec![0xabu8; 4096];
        let mut zero: [u8; 1] = [0]; // Vec::new().as_mut_ptr() is non-null but out_cap=0
        unsafe {
            let enc = gtkhx_compress_encoder_new(COMPRESS_GZIP);
            assert!(!enc.is_null());
            let comp_len = gtkhx_compress_encode(
                enc,
                input.as_ptr(),
                input.len() as u32,
                zero.as_mut_ptr(),
                0,
            );
            assert_eq!(comp_len, 0,
                       "encoder must surface BufError as failure, not return a truncated frame");
            gtkhx_compress_encoder_free(enc);
        }
    }

    #[cfg(feature = "zstd")]
    #[test]
    fn zstd_roundtrip_writes_directly_into_out_slice() {
        // After the round-3 optimization the ZSTD encoder writes
        // straight into the caller's out_slice (was: temporary Vec
        // + memcpy). This test pins (a) the round-trip still works
        // — encode/decode preserve the plaintext — and (b) the
        // wire bytes start with the ZSTD frame magic (0xFD2FB528
        // little-endian → 0x28, 0xb5, 0x2f, 0xfd) so a regression
        // that produced raw blocks or another format would trip
        // here.
        let input = b"Hello, Hotline ZSTD compression! Round-tripping cleanly is the contract.";
        let mut compressed = vec![0u8; input.len() * 2 + 1024];
        let mut decompressed = vec![0u8; input.len() * 2];

        unsafe {
            let enc = gtkhx_compress_encoder_new(COMPRESS_ZSTD);
            assert!(!enc.is_null());
            let comp_len = gtkhx_compress_encode(
                enc, input.as_ptr(), input.len() as u32,
                compressed.as_mut_ptr(), compressed.len() as u32,
            );
            assert!(comp_len > 4, "ZSTD frame is at least 4 magic bytes + body");
            assert_eq!(&compressed[..4], &[0x28, 0xb5, 0x2f, 0xfd],
                       "ZSTD output should start with the ZSTD frame magic (0xFD2FB528 LE)");
            gtkhx_compress_encoder_free(enc);

            let dec = gtkhx_compress_decoder_new(COMPRESS_ZSTD);
            assert!(!dec.is_null());
            let mut in_used = 0u32;
            let decomp_len = gtkhx_compress_decode(
                dec, compressed.as_ptr(), comp_len,
                decompressed.as_mut_ptr(), decompressed.len() as u32,
                &mut in_used,
            );
            assert_eq!(decomp_len, input.len() as u32);
            assert_eq!(&decompressed[..input.len()], input);
            gtkhx_compress_decoder_free(dec);
        }
    }

    #[cfg(feature = "lz4")]
    #[test]
    fn lz4_decoder_supports_partial_output() {
        // Production calls compress_decode with out_cap == htlc->in.len
        // (often 22 bytes for a header), but a single LZ4F frame can
        // decompress to many KB. Pins the streaming-partial contract:
        // encode a long plaintext into one frame, then decode it via
        // multiple compress_decode calls with a small out_cap, and
        // verify the bytes concatenate back to the original.
        let plaintext: Vec<u8> = (0..2048).map(|i| (i & 0xff) as u8).collect();

        // Encode the whole thing into one frame.
        let mut framed = vec![0u8; plaintext.len() * 2 + 64];
        let comp_len = unsafe {
            let enc = gtkhx_compress_encoder_new(COMPRESS_LZ4);
            let n = gtkhx_compress_encode(
                enc, plaintext.as_ptr(), plaintext.len() as u32,
                framed.as_mut_ptr(), framed.len() as u32,
            );
            gtkhx_compress_encoder_free(enc);
            n as usize
        };
        assert!(comp_len > 0);

        // Decode in 64-byte output chunks. Feed the full framed input
        // on the first call only; subsequent calls feed 0 bytes (the
        // production rcv loop would feed more on each call when
        // network data is available, but with a complete frame
        // already buffered the decoder must drain its staging on
        // subsequent calls without any new input).
        let mut decoded = Vec::with_capacity(plaintext.len());
        let mut out_chunk = [0u8; 64];
        let dec = unsafe { gtkhx_compress_decoder_new(COMPRESS_LZ4) };
        assert!(!dec.is_null());

        // First call: feed all the framed bytes.
        let mut in_used = 0u32;
        let mut produced = unsafe {
            gtkhx_compress_decode(
                dec, framed.as_ptr(), comp_len as u32,
                out_chunk.as_mut_ptr(), out_chunk.len() as u32,
                &mut in_used,
            )
        };
        assert_eq!(in_used, comp_len as u32,
                   "first call should consume the entire framed input into its buffer");
        decoded.extend_from_slice(&out_chunk[..produced as usize]);

        // Subsequent calls: zero new input, just drain.
        while decoded.len() < plaintext.len() {
            in_used = 0;
            produced = unsafe {
                gtkhx_compress_decode(
                    dec, std::ptr::null(), 0,
                    out_chunk.as_mut_ptr(), out_chunk.len() as u32,
                    &mut in_used,
                )
            };
            if produced == 0 {
                break; // no progress — bug
            }
            decoded.extend_from_slice(&out_chunk[..produced as usize]);
        }
        unsafe { gtkhx_compress_decoder_free(dec) };

        assert_eq!(decoded.len(), plaintext.len(),
                   "should drain the whole frame across multiple small-out_cap calls");
        assert_eq!(decoded, plaintext);
    }

    #[cfg(feature = "lz4")]
    #[test]
    fn lz4_frame_roundtrip() {
        // Pins the LZ4F frame format: every valid LZ4F frame starts with
        // the 32-bit magic number 0x184D2204 (little-endian → bytes
        // 0x04, 0x22, 0x4d, 0x18). A regression to raw LZ4 block
        // compression (lz4_flex::block::*) would produce different
        // leading bytes and break wire-compat with HOPE-negotiated
        // "LZ4".
        let input = b"Hello, Hotline LZ4 compression! This needs to round-trip cleanly.";
        let mut compressed = vec![0u8; input.len() * 2 + 64];
        let mut decompressed = vec![0u8; input.len() * 2];

        unsafe {
            let enc = gtkhx_compress_encoder_new(COMPRESS_LZ4);
            assert!(!enc.is_null());

            let comp_len = gtkhx_compress_encode(
                enc,
                input.as_ptr(),
                input.len() as u32,
                compressed.as_mut_ptr(),
                compressed.len() as u32,
            );
            assert!(comp_len > 4, "LZ4F frame is at least the 4-byte magic + body");
            assert_eq!(&compressed[..4], &[0x04, 0x22, 0x4d, 0x18],
                       "LZ4 output should start with the LZ4F frame magic (0x184D2204 LE)");
            gtkhx_compress_encoder_free(enc);

            let dec = gtkhx_compress_decoder_new(COMPRESS_LZ4);
            assert!(!dec.is_null());

            let mut in_used = 0u32;
            let decomp_len = gtkhx_compress_decode(
                dec,
                compressed.as_ptr(),
                comp_len,
                decompressed.as_mut_ptr(),
                decompressed.len() as u32,
                &mut in_used,
            );
            assert_eq!(decomp_len, input.len() as u32);
            assert_eq!(&decompressed[..input.len()], input);
            assert_eq!(in_used, comp_len,
                       "LZ4F decoder should consume the entire frame");
            gtkhx_compress_decoder_free(dec);
        }
    }

    #[test]
    fn compress_id_from_name_mapping() {
        use std::ffi::CString;
        let test = |name: &str, expected: u16| {
            let cstr = CString::new(name).unwrap();
            unsafe {
                assert_eq!(gtkhx_compress_id_from_name(cstr.as_ptr()), expected);
            }
        };

        test("NONE", COMPRESS_NONE);
        test("GZIP", COMPRESS_GZIP);
        test("", COMPRESS_NONE);
        test("UNKNOWN", COMPRESS_NONE);

        #[cfg(feature = "lz4")]
        test("LZ4", COMPRESS_LZ4);
        #[cfg(feature = "zstd")]
        test("ZSTD", COMPRESS_ZSTD);
    }

    #[test]
    fn null_encoder_returns_zero() {
        unsafe {
            let result = gtkhx_compress_encode(
                std::ptr::null_mut(),
                b"test".as_ptr(),
                4,
                [0u8; 64].as_mut_ptr(),
                64,
            );
            assert_eq!(result, 0);
        }
    }
}
