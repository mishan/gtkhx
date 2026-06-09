#!/usr/bin/env python3
# cicnpack — rebuild a Mac resource fork containing 'cicn' resources,
# preserving all existing cicns (by ID and name) and appending a new
# cicn produced from each input PNG.
#
# Usage:
#   pack.py <in.rsrc> <out.rsrc> [<id>:<path.png> ...]
#
# Each <id>:<path.png> argument appends one new 'cicn' resource at the
# given resource ID. Output PNGs are encoded as 8-bpp indexed with a
# per-icon ColorTable, and the alpha channel becomes the Mac classic
# mask bitmap (1 = visible, 0 = transparent). PNGs must use no more
# than 256 distinct opaque RGB colors.
#
# This is a standalone, one-shot utility — not built by the main meson
# tree. The companion cicndump reads what this writes.
#
# License: GPL-2.0-or-later.

import struct
import sys
from PIL import Image


# ---------- cicn encoder -----------------------------------------------

def encode_cicn(png_path):
    """Encode a PNG as a Mac cicn resource. Returns the raw cicn bytes."""
    im = Image.open(png_path).convert('RGBA')
    w, h = im.size
    pixels = list(im.getdata())

    # Build palette from opaque colors. Transparent pixels get index 0
    # (which is also the first opaque color, but the mask hides them).
    palette = {}        # (r,g,b) -> idx
    indices = []        # one palette index per pixel
    mask_bits = []      # 1 = visible, 0 = transparent
    for r, g, b, a in pixels:
        visible = a >= 128
        mask_bits.append(1 if visible else 0)
        key = (r, g, b) if visible else None
        if key is None:
            indices.append(0)
        else:
            if key not in palette:
                palette[key] = len(palette)
            indices.append(palette[key])
    if len(palette) == 0:
        # All transparent — give it one dummy entry.
        palette[(0, 0, 0)] = 0
    if len(palette) > 256:
        raise ValueError(f'{png_path}: {len(palette)} colors (max 256 at 8bpp)')

    # 8-bpp pixmap row size = width bytes.
    pm_rowBytes = w

    # PixMap (50 bytes). Mac PixMap layout:
    #   baseAddr 4, rowBytes 2, bounds 8, pmVersion 2, packType 2,
    #   packSize 4, hRes 4, vRes 4, pixelType 2, pixelSize 2,
    #   cmpCount 2, cmpSize 2, planeBytes 4, pmTable 4, pmReserved 4
    pixmap = struct.pack('>I H HHHH H H I I I H H H H I I I',
        0,                  # baseAddr
        pm_rowBytes | 0x8000,  # rowBytes (top bit = pixmap)
        0, 0, h, w,         # bounds (top, left, bottom, right)
        0,                  # pmVersion
        0,                  # packType
        0,                  # packSize
        0x00480000,         # hRes (72 dpi)
        0x00480000,         # vRes (72 dpi)
        0,                  # pixelType (chunky)
        8,                  # pixelSize
        1,                  # cmpCount
        8,                  # cmpSize
        0, 0, 0)            # planeBytes, pmTable, pmReserved
    assert len(pixmap) == 50

    # Mask BitMap (14 bytes): baseAddr 4, rowBytes 2, bounds 8.
    mb_rowBytes = (w + 7) // 8
    mask_bm = struct.pack('>I H HHHH', 0, mb_rowBytes, 0, 0, h, w)
    assert len(mask_bm) == 14

    # 1bpp BitMap — empty. rowBytes=0, bounds=(0,0,0,0). The cicn
    # parsers in this tree (cicndump, src/cicn.c) only use this to
    # compute the ColorTable offset; zero is fine.
    bm = struct.pack('>I H HHHH', 0, 0, 0, 0, 0, 0)
    assert len(bm) == 14

    # Handle (4 bytes, ignored).
    handle = b'\x00\x00\x00\x00'

    # Mask bitmap data: mb_rowBytes * h bytes, MSB-first.
    mask_data = bytearray(mb_rowBytes * h)
    for y in range(h):
        for x in range(w):
            if mask_bits[y * w + x]:
                mask_data[y * mb_rowBytes + (x >> 3)] |= 1 << (7 - (x & 7))

    # No 1bpp bitmap data.

    # ColorTable: ctSeed 4, ctFlags 2, ctSize 2, then N * (value 2, RGB 6).
    # ctSize is N-1. Each RGBColor is 3 big-endian u16; high byte is
    # the 8-bit channel value (Mac classic 16-bit-per-channel form).
    N = len(palette)
    ct = bytearray()
    ct += struct.pack('>I H H', 0, 0, N - 1)
    # Stable order: by index.
    inv = [None] * N
    for rgb, idx in palette.items():
        inv[idx] = rgb
    for idx, (r, g, b) in enumerate(inv):
        ct += struct.pack('>H HHH', idx, (r << 8) | r, (g << 8) | g, (b << 8) | b)

    # Pixel data: pm_rowBytes * h bytes. One byte per pixel.
    pix_data = bytes(indices)
    assert len(pix_data) == pm_rowBytes * h

    return bytes(pixmap + mask_bm + bm + handle + mask_data + ct + pix_data)


# ---------- resource fork reader ---------------------------------------

TYPE_cicn = 0x6369636e


def read_rsrc(path):
    """Parse a Mac resource fork. Returns list of (resid, name_bytes_or_None, data)
    tuples for the 'cicn' type, preserving original order."""
    raw = open(path, 'rb').read()
    data_off, map_off, _data_len, _map_len = struct.unpack('>IIII', raw[:16])
    m = raw[map_off:]
    type_list_off = struct.unpack('>H', m[24:26])[0]
    name_list_off = struct.unpack('>H', m[26:28])[0]
    num_types = struct.unpack('>H', m[28:30])[0] + 1

    cicns = []
    for i in range(num_types):
        t = m[type_list_off + 2 + 8*i : type_list_off + 2 + 8*(i+1)]
        typ, cnt_m1, rl_off = struct.unpack('>IHH', t)
        cnt = cnt_m1 + 1
        if typ != TYPE_cicn:
            continue
        for j in range(cnt):
            e = m[type_list_off + rl_off + 12*j : type_list_off + rl_off + 12*(j+1)]
            resid = struct.unpack('>h', e[0:2])[0]
            name_off = struct.unpack('>h', e[2:4])[0]   # -1 if no name
            # attrs is e[4]; data_off is the next 3 bytes (big-endian).
            data_off_rel = (e[5] << 16) | (e[6] << 8) | e[7]
            doff = data_off + data_off_rel
            dlen = struct.unpack('>I', raw[doff:doff+4])[0]
            blob = raw[doff+4 : doff+4+dlen]
            name = None
            if name_off >= 0:
                nstart = map_off + name_list_off + name_off
                nlen = raw[nstart]
                name = raw[nstart+1 : nstart+1+nlen]
            cicns.append((resid, name, blob))
    return cicns


# ---------- resource fork writer ---------------------------------------

def write_rsrc(path, cicns):
    """Write a single-type ('cicn') resource fork. cicns is a list of
    (resid, name_bytes_or_None, data) tuples."""
    data_off = 0x100  # fixed 256-byte header

    # Build the resource data section (concatenated 4-byte length + blob).
    rdata = bytearray()
    blob_offsets = []  # parallel to cicns, relative to data section start
    for _resid, _name, blob in cicns:
        blob_offsets.append(len(rdata))
        rdata += struct.pack('>I', len(blob)) + blob

    # Build the name list (concatenated pascal-strings) and per-resource
    # name-offsets (or -1 for nameless). Order matches cicns.
    name_list = bytearray()
    name_offsets = []
    for _resid, name, _blob in cicns:
        if name is None:
            name_offsets.append(-1)
        else:
            name_offsets.append(len(name_list))
            name_list += bytes([len(name)]) + name

    # Map layout (in order):
    #   [0..15]   header copy (zero)
    #   [16..19]  next handle (0)
    #   [20..21]  file ref (0)
    #   [22..23]  attrs (0)
    #   [24..25]  type_list_off (= 28)
    #   [26..27]  name_list_off (set below)
    #   [28..29]  num_types - 1 (= 0)
    #   [30..37]  type entry for 'cicn'
    #   [38..]    ref list (12 bytes per entry)
    #   [...]     name list
    type_list_off = 28
    num_types = 1
    type_entries_size = 8 * num_types
    # Type entry: type(4) + count-1(2) + ref_list_off(2). ref_list_off
    # is relative to map+type_list_off (so the very first byte of the
    # type-list region, which is the num_types field). The ref list
    # begins right after all the type entries:
    #   absolute map offset = type_list_off + 2 + type_entries_size
    #   relative to map+type_list_off = 2 + type_entries_size
    ref_list_off = 2 + type_entries_size
    ref_list_size = 12 * len(cicns)
    name_list_off = type_list_off + ref_list_off + ref_list_size

    map_size = name_list_off + len(name_list)

    m = bytearray(map_size)
    # 0..15 reserved (zeros)
    # 16..19 next handle, 20..21 file ref, 22..23 attrs — all zero
    struct.pack_into('>H H H',  m, 24, type_list_off, name_list_off,
                     num_types - 1)
    # type entry at offset 30
    struct.pack_into('>I H H', m, 30, TYPE_cicn, len(cicns) - 1, ref_list_off)
    # ref entries
    rl_base = type_list_off + ref_list_off
    for k, (resid, _name, _blob) in enumerate(cicns):
        e_off = rl_base + 12 * k
        # Resource IDs are 16-bit. Pack as unsigned so callers can pass
        # IDs in the 32768..65535 range (gtkhx treats icon IDs as
        # guint16; cicndump casts to int16 on read and so prints them
        # as negative, but the on-disk byte pattern is identical to
        # what a guint16-using reader expects).
        struct.pack_into('>H', m, e_off, resid & 0xffff)
        struct.pack_into('>h', m, e_off + 2, name_offsets[k])
        # attrs (1 byte) + data_off (3 bytes)
        off = blob_offsets[k]
        m[e_off + 4] = 0
        m[e_off + 5] = (off >> 16) & 0xff
        m[e_off + 6] = (off >> 8) & 0xff
        m[e_off + 7] = off & 0xff
        # reserved 4 bytes already zero
    # name list
    m[name_list_off:name_list_off + len(name_list)] = name_list

    # Assemble final file. Header is 256 bytes.
    map_off = data_off + len(rdata)
    data_len = len(rdata)
    map_len = len(m)

    header = bytearray(256)
    struct.pack_into('>IIII', header, 0, data_off, map_off, data_len, map_len)
    # The first 16 bytes of the map are a copy of the header per spec.
    m[0:16] = header[0:16]

    with open(path, 'wb') as f:
        f.write(header)
        f.write(rdata)
        f.write(m)


# ---------- driver -----------------------------------------------------

def main():
    if len(sys.argv) < 3:
        sys.stderr.write('usage: pack.py <in.rsrc> <out.rsrc> '
                         '[<id>:<path.png> ...]\n')
        return 1
    in_path, out_path = sys.argv[1], sys.argv[2]
    additions = []
    for arg in sys.argv[3:]:
        rid_s, _, png = arg.partition(':')
        if not png:
            sys.stderr.write(f'bad arg "{arg}", want <id>:<path.png>\n')
            return 1
        additions.append((int(rid_s), png))

    cicns = read_rsrc(in_path)
    print(f'read {len(cicns)} cicn resources from {in_path}')

    seen = {rid for rid, _n, _b in cicns}
    for rid, png in additions:
        if rid in seen:
            sys.stderr.write(f'ID {rid} already present in {in_path}\n')
            return 1
        blob = encode_cicn(png)
        cicns.append((rid, None, blob))
        seen.add(rid)
        print(f'  appended id={rid} from {png} ({len(blob)} bytes)')

    write_rsrc(out_path, cicns)
    print(f'wrote {len(cicns)} cicn resources to {out_path}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
