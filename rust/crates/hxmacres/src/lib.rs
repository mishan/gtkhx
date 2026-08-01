//! `hxmacres` — a Macintosh resource-fork parser, ported from `src/macres.c`.
//!
//! Classic Mac files stored structured data ("resources") in a resource fork: a
//! header pointing at a resource-data blob and a resource map. The map holds a
//! type list (each a 4-char OSType like `cicn`) and, per type, a reference list
//! naming each resource's 16-bit id and the byte offset of its data.
//!
//! GtkHx only reads resources — it walks the map to pull `cicn` colour-icon
//! resources out of the bundled `icons.rsrc` files for the user-icon picker.
//! This crate is that read-only walker.
//!
//! The [native API](ResourceFork) owns the fork bytes and hands out borrowed
//! [`Resource`] slices; it's pure std (bounds-checked big-endian reads, no
//! mmap — unlike `macres.c`, which mmap'd and never bounds-checked, so a
//! malformed `.rsrc` could read wild memory). The [`ffi`] module preserves the
//! exact `macres.h` C ABI so `macres.c` can be deleted and the C consumers
//! (`gtkhx.c` / `options.c`) link this instead.
//!
//! The [`cicn`] module decodes the one resource type GtkHx actually renders —
//! Mac colour icons — from the bytes this walker hands back. It was the
//! separate `hxmacres::cicn` module until the step-3 consolidation; a `cicn` is a
//! resource-fork resource, so decoding it belongs here rather than in a crate
//! of its own. Its C ABI (`hxcicn_decode`) is unchanged.

pub mod cicn;
pub mod ffi;

/// A parsed Mac resource fork. Owns the fork bytes; [`Resource`]s borrow into
/// them.
pub struct ResourceFork {
    data: Vec<u8>,
    first_res_off: u32,
    res_map_off: u32,
    res_type_list_off: u16,
    types: Vec<TypeEntry>,
}

struct TypeEntry {
    res_type: u32,
    /// Number of resources of this type (the on-disk count is stored minus one).
    count: u32,
    /// Offset of this type's reference list, relative to the type-list start.
    ref_list_off: u16,
}

/// One resource: its 16-bit id and a borrowed view of its data bytes.
pub struct Resource<'a> {
    pub resid: i16,
    pub data: &'a [u8],
}

impl ResourceFork {
    /// Parse a resource fork's bytes. Returns `None` if the header / map can't
    /// be walked within bounds (a malformed or truncated file).
    pub fn parse(data: Vec<u8>) -> Option<ResourceFork> {
        // Header: first_res_off, res_map_off, res_data_len, res_map_len (u32 BE).
        let first_res_off = be32(&data, 0)?;
        let res_map_off = be32(&data, 4)?;
        let res_data_len = be32(&data, 8)?;
        let res_map_len = be32(&data, 12)?;

        // The declared resource-data + resource-map sections must fit inside the
        // file; a header that points past EOF is malformed — fail fast rather
        // than hand back a handle that only ever yields None.
        let len = data.len();
        if (first_res_off as usize).checked_add(res_data_len as usize)? > len
            || (res_map_off as usize).checked_add(res_map_len as usize)? > len
        {
            return None;
        }

        // Resource map: the type-list / name-list offsets + type count live at a
        // fixed spot inside the map header (matching macres.c's `+24` reads).
        let mo = res_map_off as usize;
        let res_type_list_off = be16(&data, mo.checked_add(24)?)?;
        // On-disk counts are "value minus one"; checked_add so a 0xffff count
        // rejects (→ None) rather than wrapping to 0 and treating the file as
        // empty.
        let num_res_types = be16(&data, mo.checked_add(28)?)?.checked_add(1)?;

        // Type list begins 2 bytes past the type-list offset (past its own count).
        let tl_base = mo.checked_add(res_type_list_off as usize)?.checked_add(2)?;
        let mut types = Vec::with_capacity(num_res_types as usize);
        for i in 0..num_res_types as usize {
            let o = tl_base.checked_add(i.checked_mul(8)?)?;
            types.push(TypeEntry {
                res_type: be32(&data, o)?,
                count: be16(&data, o.checked_add(4)?)?.checked_add(1)? as u32,
                ref_list_off: be16(&data, o.checked_add(6)?)?,
            });
        }

        Some(ResourceFork {
            data,
            first_res_off,
            res_map_off,
            res_type_list_off,
            types,
        })
    }

    /// How many resources of `res_type` (a 4-char OSType, e.g. `0x6369636e` =
    /// `cicn`) the fork holds.
    pub fn num_res_of_type(&self, res_type: u32) -> u32 {
        self.type_entry(res_type).map(|t| t.count).unwrap_or(0)
    }

    /// The `n`-th resource of `res_type` (0-based), or `None` if out of range /
    /// malformed.
    pub fn nth_res_of_type(&self, res_type: u32, n: u32) -> Option<Resource<'_>> {
        let t = self.type_entry(res_type)?;
        if n >= t.count {
            return None;
        }
        let (resid, data_off) = self.ref_entry(t, n)?;
        self.resource_at(resid, data_off)
    }

    /// The resource of `res_type` with id `resid`, or `None`.
    pub fn res_of_id(&self, res_type: u32, resid: i16) -> Option<Resource<'_>> {
        let t = self.type_entry(res_type)?;
        for n in 0..t.count {
            let (rid, data_off) = self.ref_entry(t, n)?;
            if rid == resid {
                return self.resource_at(resid, data_off);
            }
        }
        None
    }

    fn type_entry(&self, res_type: u32) -> Option<&TypeEntry> {
        self.types.iter().find(|t| t.res_type == res_type)
    }

    /// Read the `n`-th reference-list entry for `t`: returns `(resid, data_off)`.
    /// Each entry is 12 bytes: resid(i16), name_off(u16), attrs(u8),
    /// data_off(u24), reserved(u32).
    fn ref_entry(&self, t: &TypeEntry, n: u32) -> Option<(i16, u32)> {
        let base = (self.res_map_off as usize)
            .checked_add(self.res_type_list_off as usize)?
            .checked_add(t.ref_list_off as usize)?;
        let o = base.checked_add((n as usize).checked_mul(12)?)?;
        let resid = be16(&self.data, o)? as i16;
        let data_off = be24(&self.data, o.checked_add(5)?)?;
        Some((resid, data_off))
    }

    /// Slice out a resource's data: a u32 length prefix at
    /// `first_res_off + data_off`, then that many bytes.
    fn resource_at(&self, resid: i16, data_off: u32) -> Option<Resource<'_>> {
        let base = (self.first_res_off as usize).checked_add(data_off as usize)?;
        let len = be32(&self.data, base)? as usize;
        let start = base.checked_add(4)?;
        let end = start.checked_add(len)?;
        let data = self.data.get(start..end)?;
        Some(Resource { resid, data })
    }
}

// ---- bounds-checked big-endian reads ----

// The end index is computed with `checked_add` so an `off` near `usize::MAX`
// returns `None` rather than overflow-panicking before `get` can bail.
fn be16(b: &[u8], off: usize) -> Option<u16> {
    let end = off.checked_add(2)?;
    b.get(off..end).map(|s| u16::from_be_bytes([s[0], s[1]]))
}

fn be24(b: &[u8], off: usize) -> Option<u32> {
    let end = off.checked_add(3)?;
    b.get(off..end)
        .map(|s| ((s[0] as u32) << 16) | ((s[1] as u32) << 8) | s[2] as u32)
}

fn be32(b: &[u8], off: usize) -> Option<u32> {
    let end = off.checked_add(4)?;
    b.get(off..end)
        .map(|s| u32::from_be_bytes([s[0], s[1], s[2], s[3]]))
}

#[cfg(test)]
mod tests;
