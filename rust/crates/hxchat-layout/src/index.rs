//! The height index: pixel position ↔ row, in O(log n).
//!
//! This is the structure that makes variable-height rows possible.
//! xtext could skip it because every row was `fontsize × subline_count`,
//! so "what is at pixel Y" was a division (`gtk_xtext_find_char`,
//! xtext.c:1339) and the scroll adjustment's unit was fractional *lines*
//! (`page_size = height / fontsize`, xtext.c:919). Both assumptions have
//! to go for an image to be able to be 240 pixels tall.
//!
//! **Why chunked prefix sums and not a Fenwick tree.** A Fenwick tree is
//! the textbook answer for prefix sums with point updates, and it is the
//! wrong shape here: it is indexed from a fixed origin, and this buffer
//! grows at *both* ends (chat-history backfill prepends) and shrinks at
//! the front (scrollback trim). Every prepend would renumber the whole
//! tree.
//!
//! Instead: rows live in fixed-target-size chunks in a `VecDeque`, each
//! chunk caching its own summed height, with a lazily-repaired running
//! prefix over the chunks. Front insert pushes a chunk; trim pops one;
//! neither touches the rest. A lookup binary-searches the chunk prefixes
//! and then scans within one chunk, which is bounded by [`CHUNK_TARGET`].
//!
//! **Estimated heights.** A row that has never been laid out reports an
//! estimate rather than forcing a measure, so a resize costs O(visible)
//! instead of xtext's O(entire scrollback) (`gtk_xtext_calc_lines`,
//! xtext.c:4395, walks every entry on every width change). The honest
//! cost is that the scrollbar's extent is approximate until the estimates
//! are replaced. That is survivable precisely because scroll position is
//! anchored to a row rather than to a pixel value — see
//! [`crate::anchor`].

use std::collections::VecDeque;

/// Rows per chunk. Chunks are allowed to drift from this after middle
/// inserts; [`SPLIT_AT`] bounds the drift.
const CHUNK_TARGET: usize = 128;
/// A chunk this large is split on the next insert, so the within-chunk
/// scan stays bounded.
const SPLIT_AT: usize = CHUNK_TARGET * 2;

#[derive(Clone, Debug)]
struct Chunk {
    /// Per-row heights in pixels.
    heights: Vec<u32>,
    /// Whether each height came from a real layout pass or is an
    /// estimate. Parallel to `heights`.
    measured: Vec<bool>,
    /// Cached sum of `heights`.
    sum: u64,
}

impl Chunk {
    fn new() -> Chunk {
        Chunk {
            heights: Vec::with_capacity(CHUNK_TARGET),
            measured: Vec::with_capacity(CHUNK_TARGET),
            sum: 0,
        }
    }

    fn len(&self) -> usize {
        self.heights.len()
    }

    fn recompute(&mut self) {
        self.sum = self.heights.iter().map(|h| u64::from(*h)).sum();
    }
}

/// Where a pixel offset lands.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Hit {
    /// Row position (0-based, from the top of the buffer).
    pub row: usize,
    /// Pixels into that row.
    pub offset: u32,
}

/// Chunked prefix-sum index over row heights.
#[derive(Debug, Default)]
pub struct HeightIndex {
    chunks: VecDeque<Chunk>,
    /// `prefix[i]` is the total height of chunks `0..i`. Valid only up
    /// to `prefix_valid`; entries at or past it are stale.
    prefix: Vec<u64>,
    prefix_valid: usize,
    len: usize,
}

impl HeightIndex {
    pub fn new() -> HeightIndex {
        HeightIndex::default()
    }

    /// Number of rows.
    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Total height of every row, in pixels.
    pub fn total_height(&mut self) -> u64 {
        self.repair_prefix();
        self.prefix.last().copied().unwrap_or(0)
    }

    /// Height of one row.
    pub fn height_at(&self, row: usize) -> u32 {
        match self.locate_chunk(row) {
            Some((ci, within)) => self.chunks[ci].heights[within],
            None => 0,
        }
    }

    /// Whether row `row`'s height came from a real layout pass.
    pub fn is_measured(&self, row: usize) -> bool {
        match self.locate_chunk(row) {
            Some((ci, within)) => self.chunks[ci].measured[within],
            None => false,
        }
    }

    /// How many rows still carry an estimated height.
    ///
    /// The view uses this to decide whether an idle-time measure backfill
    /// is worth scheduling.
    pub fn unmeasured_count(&self) -> usize {
        self.chunks
            .iter()
            .map(|c| c.measured.iter().filter(|m| !**m).count())
            .sum()
    }

    /// Append a row.
    pub fn push_back(&mut self, height: u32, measured: bool) {
        let need_new = match self.chunks.back() {
            None => true,
            Some(c) => c.len() >= CHUNK_TARGET,
        };
        if need_new {
            self.chunks.push_back(Chunk::new());
        }
        let ci = self.chunks.len() - 1;
        let c = &mut self.chunks[ci];
        c.heights.push(height);
        c.measured.push(measured);
        c.sum += u64::from(height);
        self.len += 1;
        self.invalidate_from(ci);
    }

    /// Prepend a row — the chat-history backfill path.
    pub fn push_front(&mut self, height: u32, measured: bool) {
        let need_new = match self.chunks.front() {
            None => true,
            Some(c) => c.len() >= CHUNK_TARGET,
        };
        if need_new {
            self.chunks.push_front(Chunk::new());
        }
        let c = &mut self.chunks[0];
        c.heights.insert(0, height);
        c.measured.insert(0, true & measured);
        c.sum += u64::from(height);
        self.len += 1;
        self.invalidate_from(0);
    }

    /// Insert at an arbitrary row position.
    ///
    /// Used by the Load-Older path, which inserts before a saved divider
    /// that is generally *not* at the front (server notices precede it,
    /// live messages follow it). O(chunk length) rather than O(n): only
    /// the containing chunk shifts, and the chunk prefixes past it are
    /// marked stale rather than recomputed.
    pub fn insert(&mut self, row: usize, height: u32, measured: bool) {
        if row >= self.len {
            self.push_back(height, measured);
            return;
        }
        let (ci, within) = match self.locate_chunk(row) {
            Some(x) => x,
            None => {
                self.push_back(height, measured);
                return;
            }
        };
        {
            let c = &mut self.chunks[ci];
            c.heights.insert(within, height);
            c.measured.insert(within, measured);
            c.sum += u64::from(height);
        }
        self.len += 1;
        if self.chunks[ci].len() >= SPLIT_AT {
            self.split_chunk(ci);
        }
        self.invalidate_from(ci);
    }

    /// Remove a row, returning its height.
    pub fn remove(&mut self, row: usize) -> Option<u32> {
        let (ci, within) = self.locate_chunk(row)?;
        let h = {
            let c = &mut self.chunks[ci];
            let h = c.heights.remove(within);
            c.measured.remove(within);
            c.sum -= u64::from(h);
            h
        };
        self.len -= 1;
        if self.chunks[ci].len() == 0 && self.chunks.len() > 1 {
            self.chunks.remove(ci);
        }
        self.invalidate_from(ci.min(self.chunks.len().saturating_sub(1)));
        Some(h)
    }

    /// Drop the first `n` rows — the scrollback trim.
    pub fn drain_front(&mut self, n: usize) {
        let mut left = n.min(self.len);
        while left > 0 {
            let front_len = match self.chunks.front() {
                Some(c) => c.len(),
                None => break,
            };
            if front_len <= left {
                let c = self.chunks.pop_front().expect("front exists");
                left -= front_len;
                self.len -= front_len;
                let _ = c;
            } else {
                let c = self.chunks.front_mut().expect("front exists");
                c.heights.drain(..left);
                c.measured.drain(..left);
                c.recompute();
                self.len -= left;
                left = 0;
            }
        }
        self.invalidate_from(0);
    }

    /// Replace a row's height, e.g. when a real layout pass supersedes an
    /// estimate or an image texture finally lands.
    pub fn set_height(&mut self, row: usize, height: u32, measured: bool) {
        let Some((ci, within)) = self.locate_chunk(row) else {
            return;
        };
        let c = &mut self.chunks[ci];
        let old = c.heights[within];
        if old == height && c.measured[within] == measured {
            return;
        }
        c.sum = c.sum - u64::from(old) + u64::from(height);
        c.heights[within] = height;
        c.measured[within] = measured;
        self.invalidate_from(ci);
    }

    /// Mark every row unmeasured, keeping the current heights as
    /// estimates.
    ///
    /// This is a width or font change. Note what it does *not* do:
    /// re-measure anything. The old heights stay as the estimate so the
    /// scrollbar remains plausible, and rows are re-laid-out lazily as
    /// they become visible. That is the whole reason resize is O(visible)
    /// here and O(scrollback) in xtext.
    pub fn invalidate_all_measurements(&mut self) {
        for c in &mut self.chunks {
            for m in &mut c.measured {
                *m = false;
            }
        }
    }

    /// Pixel offset of the top of row `row`.
    pub fn offset_of(&mut self, row: usize) -> u64 {
        if row == 0 || self.len == 0 {
            return 0;
        }
        self.repair_prefix();
        let mut remaining = row.min(self.len);
        let mut acc = 0u64;
        for (i, c) in self.chunks.iter().enumerate() {
            if remaining >= c.len() {
                acc = self.prefix[i + 1];
                remaining -= c.len();
                if remaining == 0 {
                    return acc;
                }
            } else {
                for h in &c.heights[..remaining] {
                    acc += u64::from(*h);
                }
                return acc;
            }
        }
        acc
    }

    /// Which row contains pixel offset `y`, and how far into it.
    ///
    /// `y` past the end clamps to the last row's bottom edge, so a caller
    /// that over-scrolls gets a sane answer rather than `None`.
    pub fn locate(&mut self, y: u64) -> Option<Hit> {
        if self.len == 0 {
            return None;
        }
        self.repair_prefix();
        let total = self.prefix.last().copied().unwrap_or(0);
        if y >= total {
            let row = self.len - 1;
            return Some(Hit {
                row,
                offset: self.height_at(row),
            });
        }
        // Binary search for the chunk whose span contains y.
        let mut lo = 0usize;
        let mut hi = self.chunks.len();
        while lo + 1 < hi {
            let mid = lo + (hi - lo) / 2;
            if self.prefix[mid] <= y {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        let ci = lo;
        let mut acc = self.prefix[ci];
        let mut row = self.chunk_start_row(ci);
        for h in &self.chunks[ci].heights {
            let h64 = u64::from(*h);
            if acc + h64 > y {
                return Some(Hit {
                    row,
                    offset: (y - acc) as u32,
                });
            }
            acc += h64;
            row += 1;
        }
        // Only reachable on a zero-height tail; clamp.
        Some(Hit {
            row: self.len - 1,
            offset: 0,
        })
    }

    // ---- internals ------------------------------------------------

    fn chunk_start_row(&self, ci: usize) -> usize {
        self.chunks.iter().take(ci).map(|c| c.len()).sum()
    }

    fn locate_chunk(&self, row: usize) -> Option<(usize, usize)> {
        if row >= self.len {
            return None;
        }
        let mut acc = 0usize;
        for (i, c) in self.chunks.iter().enumerate() {
            if row < acc + c.len() {
                return Some((i, row - acc));
            }
            acc += c.len();
        }
        None
    }

    fn split_chunk(&mut self, ci: usize) {
        let at = self.chunks[ci].len() / 2;
        let tail_h = self.chunks[ci].heights.split_off(at);
        let tail_m = self.chunks[ci].measured.split_off(at);
        self.chunks[ci].recompute();
        let mut tail = Chunk {
            heights: tail_h,
            measured: tail_m,
            sum: 0,
        };
        tail.recompute();
        self.chunks.insert(ci + 1, tail);
    }

    fn invalidate_from(&mut self, ci: usize) {
        self.prefix_valid = self.prefix_valid.min(ci);
    }

    /// Rebuild the stale tail of the chunk prefix sums.
    ///
    /// Amortised: a batch of appends invalidates once and repairs once,
    /// on the next query, rather than repairing per append.
    fn repair_prefix(&mut self) {
        let n = self.chunks.len();
        if self.prefix.len() != n + 1 {
            self.prefix.resize(n + 1, 0);
            self.prefix_valid = self.prefix_valid.min(n);
        }
        if self.prefix_valid >= n {
            return;
        }
        let start = self.prefix_valid;
        let mut acc = self.prefix[start];
        for i in start..n {
            acc += self.chunks[i].sum;
            self.prefix[i + 1] = acc;
        }
        self.prefix_valid = n;
    }
}
