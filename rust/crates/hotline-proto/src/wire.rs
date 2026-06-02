//! Endianness-correct read/write primitives over byte slices, plus the
//! Hotline data-chunk framing.
//!
//! The Hotline wire format is big-endian on every multi-byte integer. The C
//! code spells this out with the `HN16` / `HN32` macros (`src/protocol.h`),
//! which are hand-rolled byte swaps — and the source of at least one
//! aliasing bug (`HN16(&x, &x)`, see `gtkhx_selfinfo_uid_bug.md`). Rust's
//! `u16::from_be_bytes` / `u32::from_be_bytes` are the safe equivalents;
//! [`Decoder`] wraps them with bounds checking so a short/truncated frame
//! returns `None` instead of reading past the buffer.

use crate::{HL_DATA_HDR_LEN, HL_HDR_LEN};

/// A cursor over a borrowed byte buffer that reads big-endian integers and
/// length-delimited byte runs, bounds-checking every read.
#[derive(Debug, Clone)]
pub struct Decoder<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> Decoder<'a> {
    /// Wrap a buffer with the cursor at offset 0.
    pub fn new(buf: &'a [u8]) -> Self {
        Decoder { buf, pos: 0 }
    }

    /// Wrap a buffer with the cursor at `pos` (used to skip a fixed header).
    pub fn at(buf: &'a [u8], pos: usize) -> Self {
        Decoder {
            buf,
            pos: pos.min(buf.len()),
        }
    }

    /// Current cursor offset.
    pub fn pos(&self) -> usize {
        self.pos
    }

    /// Bytes remaining after the cursor.
    pub fn remaining(&self) -> usize {
        self.buf.len() - self.pos
    }

    /// Read a big-endian `u16`, advancing the cursor. `None` if fewer than
    /// 2 bytes remain.
    pub fn u16(&mut self) -> Option<u16> {
        let end = self.pos.checked_add(2)?;
        let bytes = self.buf.get(self.pos..end)?;
        self.pos = end;
        Some(u16::from_be_bytes([bytes[0], bytes[1]]))
    }

    /// Read a big-endian `u32`, advancing the cursor. `None` if fewer than
    /// 4 bytes remain.
    pub fn u32(&mut self) -> Option<u32> {
        let end = self.pos.checked_add(4)?;
        let bytes = self.buf.get(self.pos..end)?;
        self.pos = end;
        Some(u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }

    /// Read a big-endian `u64`, advancing the cursor. `None` if fewer than
    /// 8 bytes remain.
    pub fn u64(&mut self) -> Option<u64> {
        let end = self.pos.checked_add(8)?;
        let bytes = self.buf.get(self.pos..end)?;
        self.pos = end;
        let mut a = [0u8; 8];
        a.copy_from_slice(bytes);
        Some(u64::from_be_bytes(a))
    }

    /// Borrow `n` bytes, advancing the cursor. `None` if fewer than `n`
    /// bytes remain.
    pub fn bytes(&mut self, n: usize) -> Option<&'a [u8]> {
        let end = self.pos.checked_add(n)?;
        let slice = self.buf.get(self.pos..end)?;
        self.pos = end;
        Some(slice)
    }
}

/// One TLV-style data chunk: a 16-bit type, a 16-bit length, and `len`
/// payload bytes. This is `struct hl_data_hdr` plus its trailing data.
#[derive(Debug, Clone, Copy)]
pub struct Chunk<'a> {
    /// The chunk type tag (`HTL[CS]_DATA_*` constant).
    pub tag: u16,
    /// The payload bytes (already length-bounded).
    pub data: &'a [u8],
}

impl<'a> Chunk<'a> {
    /// Interpret the payload as a big-endian integer, matching the C
    /// `dh_getint` macro: a 4-byte payload reads as `u32`, a 2-byte payload
    /// as `u16`, anything else as 0. (The C macro treats every non-4 length
    /// as a 2-byte read; we are stricter and return 0 for unexpected
    /// lengths, which is what every current caller wants.)
    pub fn as_uint(&self) -> u32 {
        match self.data.len() {
            4 => u32::from_be_bytes([self.data[0], self.data[1], self.data[2], self.data[3]]),
            2 => u16::from_be_bytes([self.data[0], self.data[1]]) as u32,
            _ => 0,
        }
    }
}

/// Iterator over the data chunks in a Hotline message body, replacing the
/// `dh_start` / `dh_end` macro pair in `src/protocol.h`.
///
/// Construct it over the *whole* received buffer (header included) via
/// [`ChunkIter::over_message`]; it starts walking at [`HL_HDR_LEN`]. A chunk
/// whose declared length runs past the end of the buffer stops iteration
/// (matching the C macro's `break`), so a truncated frame never yields a
/// chunk that borrows out-of-bounds.
#[derive(Debug, Clone)]
pub struct ChunkIter<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> ChunkIter<'a> {
    /// Walk the chunks of a full message buffer (the 22-byte transaction
    /// header is skipped). `len` is the number of valid bytes in `buf`
    /// (the C side's `htlc->in.pos`); bytes beyond it are ignored even if
    /// the slice is longer.
    pub fn over_message(buf: &'a [u8], len: usize) -> Self {
        let end = len.min(buf.len());
        ChunkIter {
            buf: &buf[..end],
            pos: HL_HDR_LEN,
        }
    }

    /// Walk chunks starting at an arbitrary offset (used by message bodies
    /// that carry a count prefix before the chunk run).
    pub fn at(buf: &'a [u8], start: usize) -> Self {
        ChunkIter {
            buf,
            pos: start.min(buf.len()),
        }
    }
}

impl<'a> Iterator for ChunkIter<'a> {
    type Item = Chunk<'a>;

    fn next(&mut self) -> Option<Chunk<'a>> {
        // Need at least a 4-byte chunk header.
        if self.pos + HL_DATA_HDR_LEN > self.buf.len() {
            return None;
        }
        let tag = u16::from_be_bytes([self.buf[self.pos], self.buf[self.pos + 1]]);
        let len =
            u16::from_be_bytes([self.buf[self.pos + 2], self.buf[self.pos + 3]]) as usize;
        let data_start = self.pos + HL_DATA_HDR_LEN;
        // Declared length must fit in the remaining buffer; else stop
        // (matches the C macro's bounds `break`).
        if len > self.buf.len() - data_start {
            return None;
        }
        let data = &self.buf[data_start..data_start + len];
        self.pos = data_start + len;
        Some(Chunk { tag, data })
    }
}

/// A growable big-endian message builder. Phase R2's `commands.c` port
/// (`build_*` functions) writes into one of these; the C wrapper still calls
/// `hlwrite()` to push the bytes onto the socket.
#[derive(Debug, Default, Clone)]
pub struct Encoder {
    buf: Vec<u8>,
}

impl Encoder {
    /// A fresh, empty encoder.
    pub fn new() -> Self {
        Encoder { buf: Vec::new() }
    }

    /// Append a big-endian `u16`.
    pub fn put_u16(&mut self, v: u16) -> &mut Self {
        self.buf.extend_from_slice(&v.to_be_bytes());
        self
    }

    /// Append a big-endian `u32`.
    pub fn put_u32(&mut self, v: u32) -> &mut Self {
        self.buf.extend_from_slice(&v.to_be_bytes());
        self
    }

    /// Append raw bytes.
    pub fn put_bytes(&mut self, b: &[u8]) -> &mut Self {
        self.buf.extend_from_slice(b);
        self
    }

    /// Append a length-prefixed data chunk: `tag`(u16) + `len`(u16) + data.
    /// Panics if `data` exceeds `u16::MAX` (a wire-format impossibility —
    /// chunk lengths are 16-bit).
    pub fn put_chunk(&mut self, tag: u16, data: &[u8]) -> &mut Self {
        assert!(data.len() <= u16::MAX as usize, "chunk too long for u16 length");
        self.put_u16(tag);
        self.put_u16(data.len() as u16);
        self.put_bytes(data);
        self
    }

    /// Consume the encoder, yielding the assembled bytes.
    pub fn into_vec(self) -> Vec<u8> {
        self.buf
    }

    /// Borrow the assembled bytes.
    pub fn as_slice(&self) -> &[u8] {
        &self.buf
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decoder_reads_big_endian() {
        let buf = [0x12, 0x34, 0x00, 0x00, 0x00, 0x01];
        let mut d = Decoder::new(&buf);
        assert_eq!(d.u16(), Some(0x1234));
        assert_eq!(d.u32(), Some(0x0000_0001));
        assert_eq!(d.u16(), None); // exhausted
    }

    #[test]
    fn decoder_short_read_is_none_not_panic() {
        let buf = [0x01];
        let mut d = Decoder::new(&buf);
        assert_eq!(d.u16(), None);
        assert_eq!(d.pos(), 0); // cursor unmoved on failure
    }

    #[test]
    fn chunk_as_uint_matches_dh_getint() {
        assert_eq!(Chunk { tag: 1, data: &[0x00, 0x00, 0x01, 0x02] }.as_uint(), 0x102);
        assert_eq!(Chunk { tag: 1, data: &[0x01, 0x02] }.as_uint(), 0x102);
        assert_eq!(Chunk { tag: 1, data: &[0x09] }.as_uint(), 0);
    }

    #[test]
    fn chunk_iter_walks_body() {
        // 22-byte header (zeroed) + two chunks.
        let mut buf = vec![0u8; HL_HDR_LEN];
        // chunk tag=0x0067 len=2 data=0xABCD
        buf.extend_from_slice(&[0x00, 0x67, 0x00, 0x02, 0xAB, 0xCD]);
        // chunk tag=0x006e len=4 data=0x00000001
        buf.extend_from_slice(&[0x00, 0x6e, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01]);
        let chunks: Vec<_> = ChunkIter::over_message(&buf, buf.len()).collect();
        assert_eq!(chunks.len(), 2);
        assert_eq!(chunks[0].tag, 0x0067);
        assert_eq!(chunks[0].as_uint(), 0xABCD);
        assert_eq!(chunks[1].tag, 0x006e);
        assert_eq!(chunks[1].as_uint(), 1);
    }

    #[test]
    fn chunk_iter_stops_on_overlong_chunk() {
        // A chunk that claims len=99 but only has 2 payload bytes: the
        // iterator must stop rather than borrow out of bounds.
        let mut buf = vec![0u8; HL_HDR_LEN];
        buf.extend_from_slice(&[0x00, 0x67, 0x00, 0x63, 0xAB, 0xCD]);
        let chunks: Vec<_> = ChunkIter::over_message(&buf, buf.len()).collect();
        assert_eq!(chunks.len(), 0);
    }

    #[test]
    fn chunk_iter_respects_len_shorter_than_slice() {
        let mut buf = vec![0u8; HL_HDR_LEN];
        buf.extend_from_slice(&[0x00, 0x67, 0x00, 0x02, 0xAB, 0xCD]);
        // Pretend only the header arrived: no chunks should be yielded.
        let chunks: Vec<_> = ChunkIter::over_message(&buf, HL_HDR_LEN).collect();
        assert_eq!(chunks.len(), 0);
    }

    #[test]
    fn encoder_roundtrips_through_decoder() {
        let mut e = Encoder::new();
        e.put_u32(0x0000_0069).put_u16(2).put_chunk(0x0065, b"hi");
        let v = e.into_vec();
        let mut d = Decoder::new(&v);
        assert_eq!(d.u32(), Some(0x69));
        assert_eq!(d.u16(), Some(2));
        assert_eq!(d.u16(), Some(0x0065)); // chunk tag
        assert_eq!(d.u16(), Some(2)); // chunk len
        assert_eq!(d.bytes(2), Some(&b"hi"[..]));
    }
}
