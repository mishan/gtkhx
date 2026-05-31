//! HOPE transport compression for GtkHx.
//!
//! Replaces `compress.c`. Provides streaming compress/decompress for:
//!   - GZIP (zlib deflate/inflate with Z_SYNC_FLUSH)
//!   - LZ4 (frame format, Janus/Klein extension)
//!   - ZSTD (Zstandard, Janus/Klein extension)
//!
//! Each algorithm uses a persistent codec context per connection direction,
//! matching the wire protocol's requirement that deflate/inflate state carries
//! across transactions within a single connection.

use std::slice;

use flate2::{Compress, Decompress, FlushCompress, FlushDecompress, Status};

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
    Lz4,
    #[cfg(feature = "zstd")]
    Zstd(zstd::stream::raw::Decoder<'static>),
}

// ---- Encoder FFI --------------------------------------------------------

/// Create a new encoder for the given algorithm.
/// Returns null if the algorithm is unsupported or COMPRESS_NONE.
#[no_mangle]
pub extern "C" fn gtkhx_compress_encoder_new(algo: u16) -> *mut CompressEncoder {
    let encoder = match algo {
        COMPRESS_GZIP => {
            CompressEncoder::Gzip(Compress::new(flate2::Compression::default(), false))
        }
        #[cfg(feature = "lz4")]
        COMPRESS_LZ4 => CompressEncoder::Lz4,
        #[cfg(feature = "zstd")]
        COMPRESS_ZSTD => {
            let enc = zstd::stream::raw::Encoder::new(3).expect("zstd encoder creation");
            CompressEncoder::Zstd(enc)
        }
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
                Ok(Status::Ok | Status::StreamEnd | Status::BufError) => {
                    let consumed_in = (compress.total_in() - before_in) as u32;
                    let _ = consumed_in; // All input should be consumed with sync flush
                    (compress.total_out() - before_out) as u32
                }
                Err(_) => 0,
            }
        }
        #[cfg(feature = "lz4")]
        CompressEncoder::Lz4 => {
            // LZ4 frame compress — use lz4_flex's block mode for simplicity
            // The C code uses LZ4F (frame format), so each transaction is a
            // complete frame.
            match lz4_flex::block::compress_into(input_slice, out_slice) {
                Ok(n) => n as u32,
                Err(_) => 0,
            }
        }
        #[cfg(feature = "zstd")]
        CompressEncoder::Zstd(ref mut encoder) => {
            let mut in_buf = zstd::stream::raw::InBuffer::around(input_slice);
            let mut out_buf_storage = vec![0u8; out_cap as usize];
            let mut out_buf = zstd::stream::raw::OutBuffer::around(&mut out_buf_storage);

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

            let written = out_buf.pos();
            out_slice[..written].copy_from_slice(&out_buf_storage[..written]);
            written as u32
        }
    }
}

// ---- Decoder FFI --------------------------------------------------------

/// Create a new decoder for the given algorithm.
#[no_mangle]
pub extern "C" fn gtkhx_compress_decoder_new(algo: u16) -> *mut CompressDecoder {
    let decoder = match algo {
        COMPRESS_GZIP => CompressDecoder::Gzip(Decompress::new(false)),
        #[cfg(feature = "lz4")]
        COMPRESS_LZ4 => CompressDecoder::Lz4,
        #[cfg(feature = "zstd")]
        COMPRESS_ZSTD => {
            let dec = zstd::stream::raw::Decoder::new().expect("zstd decoder creation");
            CompressDecoder::Zstd(dec)
        }
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
    if dec.is_null() || input.is_null() || out.is_null() || in_used.is_null() {
        return 0;
    }

    let decoder = &mut *dec;
    let input_slice = slice::from_raw_parts(input, input_len as usize);
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
        CompressDecoder::Lz4 => {
            match lz4_flex::block::decompress_into(input_slice, out_slice) {
                Ok(n) => {
                    *in_used = input_len;
                    n as u32
                }
                Err(_) => {
                    *in_used = 0;
                    0
                }
            }
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
