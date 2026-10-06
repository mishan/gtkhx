//! `hxrequest` — the requests GtkHx sends, built as values.
//!
//! Each builder takes what the user asked for (a path, a name, whether the
//! connection negotiated UTF-8) and returns the [`Request`] the client puts on
//! the wire: its opcode and its chunks, byte for byte. The production senders
//! wrap them with the task registration and the send; the end-to-end suite
//! sends the same values to real servers. Neither the builders nor anything
//! they call reaches into C, which is what lets that suite link them.

pub mod files;
pub mod icon;
pub mod media;
pub mod news;
pub mod path;
pub mod user;

use hxproto::build::{HxChunk, PackChunk};

/// One request: the transaction type and its data chunks, in order.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Request {
    pub opcode: u32,
    pub chunks: Vec<(u16, Vec<u8>)>,
}

impl Request {
    /// Copy what an `hxproto::build` builder filled into an owned request.
    /// `hc` is the builder's return: the number of chunks it filled, or 0 when
    /// it refused the input.
    pub(crate) fn from_built(opcode: u32, chunks: &[HxChunk], hc: usize) -> Option<Self> {
        if hc == 0 {
            return None;
        }
        let chunks = chunks[..hc]
            .iter()
            .map(|c| {
                let data = if c.data.is_null() || c.len == 0 {
                    Vec::new()
                } else {
                    // SAFETY: the builder pointed `data` at `len` bytes of an
                    // input that outlives this call.
                    unsafe { std::slice::from_raw_parts(c.data, c.len as usize) }.to_vec()
                };
                (c.tag, data)
            })
            .collect();
        Some(Request { opcode, chunks })
    }

    /// Run `f` over the chunks as the `HxChunk` array the C send primitive
    /// takes. The array borrows this request, so `f` must not keep it.
    pub fn with_hx_chunks<R>(&self, f: impl FnOnce(&[HxChunk]) -> R) -> R {
        let chunks: Vec<HxChunk> = self
            .chunks
            .iter()
            .map(|(tag, data)| HxChunk {
                tag: *tag,
                len: data.len() as u16,
                data: data.as_ptr(),
            })
            .collect();
        f(&chunks)
    }

    /// The whole frame, header included, for transaction `trans`.
    pub fn pack(&self, trans: u32) -> Vec<u8> {
        let chunks: Vec<PackChunk<'_>> = self
            .chunks
            .iter()
            .map(|(tag, data)| PackChunk { tag: *tag, data })
            .collect();
        let mut out = vec![0u8; hxproto::build::pack_message_size(&chunks)];
        let n = hxproto::build::pack_message(&mut out, self.opcode, trans, 0, &chunks)
            .expect("a built request always packs");
        out.truncate(n);
        out
    }

    /// The data of the first chunk tagged `tag`.
    pub fn chunk(&self, tag: u16) -> Option<&[u8]> {
        self.chunks
            .iter()
            .find(|(t, _)| *t == tag)
            .map(|(_, d)| d.as_slice())
    }
}
