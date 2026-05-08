/*
 * cicndump — dump every cicn (color icon) resource in a Mac resource
 *            fork to PNG.
 *
 * Standalone utility. Builds against libpng. No GLib, no GTK, no
 * GdkPixbuf — just stdio/mmap/inttypes and libpng.
 *
 * Usage:
 *   cicndump <input.rsrc> [output_dir]
 *
 * Each cicn resource is written to <output_dir>/cicn_<resid>.png.
 * Default output_dir is "./out".
 *
 * Format references:
 *   - "Inside Macintosh: More Macintosh Toolbox", Resource Manager
 *   - "Inside Macintosh: Imaging With QuickDraw", Color Icons (cicn)
 *
 * License: GPL-2.0-or-later.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <png.h>

/* ---------- big-endian readers ------------------------------------- */

static inline uint16_t be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}
static inline uint32_t be24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}
static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/* ---------- resource fork walker ----------------------------------- */

#define TYPE_cicn 0x6369636eu  /* 'cicn' */

typedef struct {
    int16_t   resid;
    uint8_t   attrs;
    uint32_t  data_off;     /* relative to first_res_off                */
    uint16_t  name_off;     /* 0xFFFF if no name                        */
} res_ref;

typedef struct {
    uint32_t  type;
    uint32_t  count;
    uint16_t  ref_list_off; /* relative to type_list base               */
    res_ref  *refs;         /* count entries                            */
} res_type;

typedef struct {
    const uint8_t *base;
    size_t         len;
    uint32_t       first_res_off;
    uint32_t       res_map_off;
    uint16_t       type_list_off; /* relative to res_map_off            */
    uint16_t       name_list_off;
    uint32_t       num_types;
    res_type      *types;
} rsrc_file;

static int rsrc_open(rsrc_file *r, const uint8_t *base, size_t len) {
    if (len < 256) {
        fprintf(stderr, "file too short to be a Mac resource fork\n");
        return -1;
    }
    r->base = base;
    r->len  = len;

    r->first_res_off = be32(base + 0);
    r->res_map_off   = be32(base + 4);
    /* res_data_len  = be32(base + 8);  */
    /* res_map_len   = be32(base + 12); */

    if ((size_t)r->res_map_off + 30 > len) {
        fprintf(stderr, "res_map_off %u out of range\n", r->res_map_off);
        return -1;
    }

    /* Map header layout: 16B reserved (header copy), 4B next-handle,
     * 2B file ref, 2B attrs, 2B type-list-off, 2B name-list-off,
     * 2B num_types-1, then the type list. */
    const uint8_t *map = base + r->res_map_off;
    r->type_list_off = be16(map + 24);
    r->name_list_off = be16(map + 26);
    r->num_types     = (uint32_t)be16(map + 28) + 1u;

    const uint8_t *type_list = map + r->type_list_off + 2;
    if ((size_t)(type_list - base) + 8u * r->num_types > len) {
        fprintf(stderr, "type list out of range\n");
        return -1;
    }

    r->types = calloc(r->num_types, sizeof *r->types);
    if (!r->types) return -1;

    for (uint32_t i = 0; i < r->num_types; i++) {
        const uint8_t *t = type_list + 8 * i;
        r->types[i].type         = be32(t + 0);
        r->types[i].count        = (uint32_t)be16(t + 4) + 1u;
        r->types[i].ref_list_off = be16(t + 6);

        const uint8_t *rl = map + r->type_list_off + r->types[i].ref_list_off;
        if ((size_t)(rl - base) + 12u * r->types[i].count > len) {
            fprintf(stderr, "ref list out of range for type 0x%08x\n",
                    r->types[i].type);
            free(r->types);
            return -1;
        }

        r->types[i].refs = calloc(r->types[i].count, sizeof(res_ref));
        if (!r->types[i].refs) return -1;

        for (uint32_t j = 0; j < r->types[i].count; j++) {
            const uint8_t *e = rl + 12 * j;
            r->types[i].refs[j].resid    = (int16_t)be16(e + 0);
            r->types[i].refs[j].name_off = be16(e + 2);
            r->types[i].refs[j].attrs    = e[4];
            r->types[i].refs[j].data_off = be24(e + 5);
        }
    }
    return 0;
}

static void rsrc_close(rsrc_file *r) {
    if (!r->types) return;
    for (uint32_t i = 0; i < r->num_types; i++)
        free(r->types[i].refs);
    free(r->types);
    r->types = NULL;
}

/* Resolve a ref to (data ptr, length). Each resource in the data area
 * is preceded by a 4-byte big-endian length. */
static int rsrc_get_data(const rsrc_file *r, const res_ref *ref,
                         const uint8_t **out_data, uint32_t *out_len) {
    size_t off = (size_t)r->first_res_off + ref->data_off;
    if (off + 4 > r->len) return -1;
    uint32_t len = be32(r->base + off);
    if (off + 4 + len > r->len) return -1;
    *out_data = r->base + off + 4;
    *out_len  = len;
    return 0;
}

/* ---------- canonical Mac palettes --------------------------------- */
/* Channels stored as 0xVV (8-bit values, not Mac classic 16-bit).    */

typedef struct { uint8_t r, g, b; } rgb;

/* The full Mac classic 8-bit palette is ported verbatim from
 * src/cicn.c (rgb_8[]). Each entry there stored channels as
 * 0xVV00 (high byte = the 8-bit value), so we take the high byte. */
static const rgb pal_8[256] = {
    {0xff,0xff,0xff},{0xff,0xfe,0xcb},{0xff,0xfe,0x9a},{0xff,0xfe,0x66},
    {0xff,0xfe,0x33},{0xfe,0xfe,0x00},{0xff,0xcb,0xfe},{0xff,0xcb,0xcb},
    {0xff,0xcc,0x9a},{0xff,0xcc,0x66},{0xff,0xcc,0x33},{0xfe,0xcb,0x00},
    {0xff,0x9a,0xfe},{0xff,0x9a,0xcc},{0xff,0x9a,0x9a},{0xff,0x99,0x66},
    {0xff,0x99,0x33},{0xfe,0x98,0x00},{0xff,0x66,0xfe},{0xff,0x66,0xcc},
    {0xff,0x66,0x99},{0xff,0x66,0x66},{0xff,0x66,0x33},{0xfe,0x65,0x00},
    {0xff,0x33,0xfe},{0xff,0x33,0xcc},{0xff,0x33,0x99},{0xff,0x33,0x66},
    {0xff,0x33,0x33},{0xfe,0x32,0x00},{0xfe,0x00,0xfe},{0xfe,0x00,0xcb},
    {0xfe,0x00,0x98},{0xfe,0x00,0x65},{0xfe,0x00,0x32},{0xfe,0x00,0x00},
    {0xcb,0xff,0xff},{0xcb,0xff,0xcb},{0xcc,0xff,0x9a},{0xcc,0xff,0x66},
    {0xcc,0xff,0x33},{0xcb,0xfe,0x00},{0xcb,0xcb,0xff},{0xcc,0xcc,0xcc},
    {0xcc,0xcc,0x99},{0xcc,0xcc,0x66},{0xcb,0xcb,0x32},{0xcd,0xcd,0x00},
    {0xcc,0x9a,0xff},{0xcc,0x99,0xcc},{0xcc,0x99,0x99},{0xcc,0x99,0x66},
    {0xcb,0x98,0x32},{0xcd,0x9a,0x00},{0xcc,0x66,0xff},{0xcc,0x66,0xcc},
    {0xcc,0x66,0x99},{0xcc,0x66,0x66},{0xcb,0x65,0x32},{0xcd,0x66,0x00},
    {0xcc,0x33,0xff},{0xcb,0x32,0xcb},{0xcb,0x32,0x98},{0xcb,0x32,0x65},
    {0xcb,0x32,0x32},{0xcd,0x33,0x00},{0xcb,0x00,0xfe},{0xcd,0x00,0xcd},
    {0xcd,0x00,0x9a},{0xcd,0x00,0x66},{0xcd,0x00,0x33},{0xcd,0x00,0x00},
    {0x9a,0xff,0xff},{0x9a,0xff,0xcc},{0x9a,0xff,0x9a},{0x99,0xff,0x66},
    {0x99,0xff,0x33},{0x99,0xfe,0x00},{0x9a,0xcc,0xff},{0x99,0xcc,0xcc},
    {0x00,0x98,0x65},{0x99,0xcc,0x66},{0x99,0xcb,0x32},{0x9a,0xcd,0x00},
    {0x9a,0x9a,0xff},{0x99,0x99,0xcc},{0x99,0x99,0x99},{0x98,0x98,0x65},
    {0x9a,0x9a,0x33},{0x98,0x98,0x00},{0x99,0x66,0xff},{0x99,0x66,0xcc},
    {0x98,0x65,0x98},{0x98,0x65,0x65},{0x9a,0x66,0x33},{0x98,0x65,0x00},
    {0x99,0x33,0xff},{0x98,0x32,0xcb},{0x9a,0x33,0x9a},{0x9a,0x33,0x66},
    {0x9a,0x33,0x33},{0x98,0x32,0x00},{0x98,0x00,0xfe},{0x9a,0x00,0xcd},
    {0x98,0x00,0x98},{0x98,0x00,0x65},{0x98,0x00,0x32},{0x98,0x00,0x00},
    {0x66,0xff,0xff},{0x66,0xff,0xcc},{0x66,0xff,0x99},{0x66,0xff,0x66},
    {0x66,0xff,0x33},{0x66,0xfe,0x00},{0x66,0xcc,0xff},{0x66,0xcc,0xcc},
    {0x66,0xcc,0x99},{0x66,0xcc,0x66},{0x66,0xcb,0x32},{0x66,0xcd,0x00},
    {0x66,0x99,0xff},{0x66,0x99,0xcc},{0x65,0x98,0x98},{0x65,0x98,0x65},
    {0x66,0x9a,0x33},{0x65,0x98,0x00},{0x66,0x66,0xff},{0x66,0x66,0xcc},
    {0x65,0x65,0x98},{0x66,0x66,0x66},{0x65,0x65,0x32},{0x66,0x66,0x00},
    {0x66,0x33,0xff},{0x65,0x32,0xcb},{0x66,0x33,0x9a},{0x65,0x32,0x65},
    {0x65,0x32,0x32},{0x66,0x33,0x00},{0x65,0x00,0xfe},{0x66,0x00,0xcd},
    {0x65,0x00,0x98},{0x66,0x00,0x66},{0x66,0x00,0x33},{0x66,0x00,0x00},
    {0x33,0xff,0xff},{0x33,0xff,0xcc},{0x33,0xff,0x99},{0x33,0xff,0x66},
    {0x33,0xff,0x33},{0x33,0xfe,0x00},{0x33,0xcc,0xff},{0x32,0xcb,0xcb},
    {0x32,0xcb,0x98},{0x32,0xcb,0x65},{0x33,0xcb,0x32},{0x33,0xcd,0x00},
    {0x33,0x99,0xff},{0x32,0x99,0xcb},{0x33,0x9a,0x9a},{0x33,0x9a,0x66},
    {0x33,0x9a,0x33},{0x32,0x98,0x00},{0x33,0x66,0xff},{0x32,0x66,0xcb},
    {0x33,0x66,0x9a},{0x32,0x65,0x65},{0x32,0x65,0x32},{0x33,0x66,0x00},
    {0x33,0x33,0xff},{0x32,0x33,0xcb},{0x33,0x33,0x9a},{0x32,0x32,0x65},
    {0x33,0x33,0x33},{0x33,0x33,0x00},{0x32,0x00,0xfe},{0x33,0x00,0xcd},
    {0x32,0x00,0x98},{0x33,0x00,0x66},{0x33,0x00,0x33},{0x33,0x00,0x00},
    {0x00,0xfe,0xfe},{0x00,0xfe,0xcb},{0x00,0xfe,0x98},{0x00,0xfe,0x65},
    {0x00,0xfe,0x32},{0x00,0xfe,0x00},{0x00,0xcb,0xfe},{0x00,0xcd,0xcd},
    {0x00,0xcd,0x9a},{0x00,0xcd,0x66},{0x00,0xcd,0x33},{0x00,0xcd,0x00},
    {0x00,0x98,0xfe},{0x00,0x9a,0xcd},{0x00,0x98,0x98},{0x00,0x98,0x65},
    {0x00,0x98,0x32},{0x00,0x98,0x00},{0x00,0x66,0xfe},{0x00,0x66,0xcd},
    {0x00,0x65,0x98},{0x00,0x66,0x66},{0x00,0x66,0x33},{0x00,0x66,0x00},
    {0x00,0x33,0xfe},{0x00,0x33,0xcd},{0x00,0x32,0x98},{0x00,0x33,0x66},
    {0x00,0x33,0x33},{0x00,0x33,0x00},{0x00,0x00,0xfe},{0x00,0x00,0xcd},
    {0x00,0x00,0x98},{0x00,0x00,0x66},{0x00,0x00,0x33},{0xef,0x00,0x00},
    {0xdc,0x00,0x00},{0xba,0x00,0x00},{0xab,0x00,0x00},{0x89,0x00,0x00},
    {0x77,0x00,0x00},{0x55,0x00,0x00},{0x44,0x00,0x00},{0x22,0x00,0x00},
    {0x11,0x00,0x00},{0x00,0xef,0x00},{0x00,0xdc,0x00},{0x00,0xba,0x00},
    {0x00,0xab,0x00},{0x00,0x89,0x00},{0x00,0x77,0x00},{0x00,0x55,0x00},
    {0x00,0x44,0x00},{0x00,0x22,0x00},{0x00,0x11,0x00},{0x00,0x00,0xef},
    {0x00,0x00,0xdc},{0x00,0x00,0xba},{0x00,0x00,0xab},{0x00,0x00,0x89},
    {0x00,0x00,0x77},{0x00,0x00,0x55},{0x00,0x00,0x44},{0x00,0x00,0x22},
    {0x00,0x00,0x11},{0xee,0xee,0xee},{0xdd,0xdd,0xdd},{0xbb,0xbb,0xbb},
    {0xaa,0xaa,0xaa},{0x88,0x88,0x88},{0x77,0x77,0x77},{0x55,0x55,0x55},
    {0x44,0x44,0x44},{0x22,0x22,0x22},{0x11,0x11,0x11},{0x00,0x00,0x00}
};

static const rgb pal_4[16] = {
    {0xff,0xff,0xff},{0xff,0xff,0x00},{0xff,0xa0,0x7a},{0xff,0x00,0x00},
    {0xff,0x14,0x93},{0x8a,0x2b,0xe2},{0x00,0x00,0x80},{0x64,0x95,0xed},
    {0x22,0x8b,0x22},{0x00,0x64,0x00},{0x8b,0x45,0x13},{0xd2,0xb4,0x8c},
    {0xd3,0xd3,0xd3},{0xbe,0xbe,0xbe},{0x69,0x69,0x69},{0x00,0x00,0x00}
};

static const rgb pal_2[4] = {
    {0xff,0xff,0xff},{0xff,0xff,0x00},{0x00,0xff,0xff},{0x00,0x00,0x00}
};

static const rgb pal_1[2] = {
    {0xff,0xff,0xff},{0x00,0x00,0x00}
};

/* ---------- cicn decoder ------------------------------------------- */

/* On-disk cicn layout (from src/cicn.h and Inside Macintosh):
 *   0   PixMap     50 bytes  (icon's pixel map)
 *   50  BitMap     14 bytes  (icon's mask)
 *   64  BitMap     14 bytes  (icon's bitmap)
 *   78  Handle      4 bytes  (icon's data handle, ignored)
 *   82+ mask bitmap data, then bitmap data, then ColorTable, then
 *       pixel data (rowBytes*height bytes at the very end of the
 *       resource).
 *
 * PixMap fields we care about (all big-endian):
 *   off  0   Ptr     baseAddr
 *   off  4   u16     rowBytes (top bit means "pixmap, not bitmap")
 *   off  6   Rect    bounds (top, left, bottom, right) — 8 bytes
 *   off 14   u16     pmVersion
 *   off 16   u16     packType
 *   off 18   u32     packSize
 *   off 22   u32     hRes
 *   off 26   u32     vRes
 *   off 30   u16     pixelType
 *   off 32   u16     pixelSize (1, 2, 4, or 8)
 *
 * BitMap fields:
 *   off  4   u16     rowBytes
 *   off  6   Rect    bounds (8 bytes)
 *
 * Pixel access: rows are rowBytes long, MSB-first within each byte.
 */

typedef struct {
    uint32_t  width, height;
    uint8_t  *rgba;          /* width*height*4 bytes, top-to-bottom    */
} image;

static void image_free(image *im) {
    free(im->rgba);
    im->rgba = NULL;
}

static int decode_cicn(const uint8_t *r, uint32_t len, image *out) {
    if (len < 82) return -1;

    /* PixMap */
    uint32_t pm_rowBytes = be16(r + 4) & 0x7fff;
    uint16_t pm_top      = be16(r + 6);
    uint16_t pm_left     = be16(r + 8);
    uint16_t pm_bottom   = be16(r + 10);
    uint16_t pm_right    = be16(r + 12);
    uint16_t pm_bpp      = be16(r + 32);

    /* mask BitMap */
    uint16_t mb_rowBytes = be16(r + 50 + 4);
    uint16_t mb_top      = be16(r + 50 + 6);
    uint16_t mb_bottom   = be16(r + 50 + 10);
    uint16_t mb_right    = be16(r + 50 + 12);

    /* bitmap (1bpp) BitMap */
    uint16_t bm_rowBytes = be16(r + 64 + 4);
    uint16_t bm_top      = be16(r + 64 + 6);
    uint16_t bm_bottom   = be16(r + 64 + 10);

    if (pm_bpp != 1 && pm_bpp != 2 && pm_bpp != 4 && pm_bpp != 8) {
        fprintf(stderr, "  skip: unsupported bpp=%u\n", pm_bpp);
        return -1;
    }
    if (pm_right <= pm_left || pm_bottom <= pm_top) return -1;
    if (mb_bottom < mb_top || bm_bottom < bm_top)   return -1;

    uint32_t width  = pm_right  - pm_left;
    uint32_t height = pm_bottom - pm_top;
    uint32_t mbm_h  = mb_bottom - mb_top;
    uint32_t bm_h   = bm_bottom - bm_top;

    if (width > 4096 || height > 4096) return -1;
    if (pm_rowBytes < (width * pm_bpp + 7) / 8) return -1;

    /* ColorTable lives after both bitmap data blocks. */
    size_t ct_off = 50 + 14 + 14 + 4
                  + (size_t)mb_rowBytes * mbm_h
                  + (size_t)bm_rowBytes * bm_h;
    if (ct_off + 8 > len) return -1;

    const uint8_t *ct = r + ct_off;
    /* uint32_t ctSeed  = be32(ct + 0); */
    /* uint16_t ctFlags = be16(ct + 4); */
    uint16_t ctSize  = be16(ct + 6);   /* number of entries - 1 */
    uint32_t ctn     = (uint32_t)ctSize + 1u;
    if (ct_off + 8 + (size_t)ctn * 8 > len) return -1;

    /* Pixel data is the trailing rowBytes*height bytes of the
     * resource (per Inside Macintosh and src/cicn.c). */
    size_t pix_size = (size_t)pm_rowBytes * height;
    if (pix_size > len) return -1;
    const uint8_t *pixdata = r + len - pix_size;
    const uint8_t *maskdata = r + 82;
    int have_mask = (mb_right != 0 && mb_bottom != 0);

    /* Build the palette table. */
    uint32_t n = 1u << pm_bpp;
    rgb palette[256];
    const rgb *defpal;
    switch (pm_bpp) {
        case 8: defpal = pal_8; break;
        case 4: defpal = pal_4; break;
        case 2: defpal = pal_2; break;
        case 1: defpal = pal_1; break;
        default: return -1;
    }
    for (uint32_t i = 0; i < n; i++) palette[i] = defpal[i];
    /* Overlay any per-icon ColorTable entries (each 8 bytes:
     * u16 value, then RGBColor with three u16 channels — high byte
     * is the 8-bit value). */
    for (uint32_t i = 0; i < ctn; i++) {
        const uint8_t *e = ct + 8 + i * 8;
        uint16_t v = be16(e + 0) & (n - 1);
        palette[v].r = e[2];
        palette[v].g = e[4];
        palette[v].b = e[6];
    }

    out->width  = width;
    out->height = height;
    out->rgba   = malloc((size_t)width * height * 4);
    if (!out->rgba) return -1;

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *id = pixdata + (size_t)pm_rowBytes * y;
        const uint8_t *mp = have_mask
            ? maskdata + (size_t)mb_rowBytes * y : NULL;
        uint8_t *prow = out->rgba + (size_t)y * width * 4;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t idx;
            switch (pm_bpp) {
                case 1: idx = (id[x>>3] >> (7 - (x & 7))) & 0x01; break;
                case 2: idx = (id[x>>2] >> ((3 - (x & 3))*2)) & 0x03; break;
                case 4: idx = (id[x>>1] >> ((1 - (x & 1))*4)) & 0x0f; break;
                case 8: default: idx = id[x]; break;
            }
            const rgb *c = &palette[idx];
            uint8_t a = mp
                ? (((mp[x>>3] >> (7 - (x & 7))) & 0x01) ? 0xff : 0x00)
                : 0xff;
            prow[0] = c->r;
            prow[1] = c->g;
            prow[2] = c->b;
            prow[3] = a;
            prow += 4;
        }
    }
    return 0;
}

/* ---------- PNG writer --------------------------------------------- */

static int write_png(const char *path, const image *im) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "  fopen %s: %s\n", path, strerror(errno));
        return -1;
    }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                              NULL, NULL, NULL);
    if (!png) { fclose(fp); return -1; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(fp); return -1; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return -1;
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, im->width, im->height, 8,
                 PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    png_bytep *rows = malloc(sizeof(png_bytep) * im->height);
    if (!rows) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return -1;
    }
    for (uint32_t y = 0; y < im->height; y++)
        rows[y] = im->rgba + (size_t)y * im->width * 4;

    png_write_image(png, rows);
    png_write_end(png, NULL);
    free(rows);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 0;
}

/* ---------- driver ------------------------------------------------- */

static int ensure_dir(const char *dir) {
    struct stat st;
    if (stat(dir, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        fprintf(stderr, "%s exists but is not a directory\n", dir);
        return -1;
    }
    if (mkdir(dir, 0755) != 0) {
        fprintf(stderr, "mkdir %s: %s\n", dir, strerror(errno));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <input.rsrc> [output_dir]\n", argv[0]);
        return 1;
    }
    const char *in_path = argv[1];
    const char *out_dir = (argc == 3) ? argv[2] : "./out";

    if (ensure_dir(out_dir) != 0) return 1;

    int fd = open(in_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", in_path, strerror(errno));
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return 1; }
    void *base = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        fprintf(stderr, "mmap: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    rsrc_file r = {0};
    if (rsrc_open(&r, (const uint8_t *)base, st.st_size) != 0) {
        munmap(base, st.st_size);
        close(fd);
        return 1;
    }

    int written = 0, skipped = 0;
    for (uint32_t i = 0; i < r.num_types; i++) {
        if (r.types[i].type != TYPE_cicn) {
            fprintf(stderr, "skipping %u resources of type 0x%08x\n",
                    r.types[i].count, r.types[i].type);
            skipped += r.types[i].count;
            continue;
        }
        for (uint32_t j = 0; j < r.types[i].count; j++) {
            res_ref *ref = &r.types[i].refs[j];
            const uint8_t *data; uint32_t dlen;
            if (rsrc_get_data(&r, ref, &data, &dlen) != 0) {
                fprintf(stderr, "  cicn %d: bad data offset\n", ref->resid);
                skipped++;
                continue;
            }
            image im = {0};
            if (decode_cicn(data, dlen, &im) != 0) {
                fprintf(stderr, "  cicn %d: decode failed (%u bytes)\n",
                        ref->resid, dlen);
                skipped++;
                continue;
            }
            char path[512];
            snprintf(path, sizeof path, "%s/cicn_%d.png", out_dir, ref->resid);
            if (write_png(path, &im) == 0) {
                printf("  wrote %s (%ux%u)\n", path, im.width, im.height);
                written++;
            } else {
                skipped++;
            }
            image_free(&im);
        }
    }

    fprintf(stderr, "done: %d written, %d skipped\n", written, skipped);

    rsrc_close(&r);
    munmap(base, st.st_size);
    close(fd);
    return (written > 0) ? 0 : 2;
}
