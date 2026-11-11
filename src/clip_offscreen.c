/* Offscreen (tiled raster) Attribute blobs and block streams.
 *
 * Each raster layer references an "Offscreen" row.  Its Attribute blob
 * describes the bitmap size, block grid, pixel packing and the size of every
 * block; its BlockData names an external chunk holding the tiles.  Tiles
 * are 256x256 and individually zlib-compressed.  Verified against a
 * document written by Clip Studio Paint 5.1 (docs/FORMAT.md). */
#include <stdlib.h>
#include <string.h>

#include "clip_internal.h"

#define K_BLOCK 256u
static const char kBegin[] = "BlockDataBeginChunk";
static const char kEnd[] = "BlockDataEndChunk";
#define K_BEGIN_LEN (4 + 19 * 2)

/* Packing values written by CSP 5.1 for 8-bit RGBA ("channel order 33"). */
static const uint32_t kPackingRgba[16] = {33, 1, 4, 5, 65536, 4, 1024, 1, 256, 65536, 256, 256, 8, 8, 0, 0};

const char *clip_packing_name(size_t i) {
    static const char *names[16] = {"BlockDataChannelOrder",          "BlockDataChannelBytes",
                                    "BlockDataChannelImageCount",     "BlockDataPixelBytes",
                                    "BlockDataOffsetImageBlockBytes", "BlockDataOffsetImagePixelBytes",
                                    "BlockDataOffsetImageRowBytes",   "BlockDataOffsetAlphaPixelBytes",
                                    "BlockDataOffsetAlphaRowBytes",   "BlockDataOffsetImageBlock",
                                    "BlockDataHorizontalBlockWide",   "BlockDataVerticalBlockWide",
                                    "BlockDataHorizontalBlockShift",  "BlockDataVerticalBlockShift",
                                    "(reserved)",                     "(reserved)"};
    return i < 16 ? names[i] : "?";
}

const char *clip_pixel_layout_name(clip_pixel_layout l) {
    switch (l) {
        case CLIP_LAYOUT_ALPHA_ONLY: return "alpha-only (1 byte/pixel)";
        case CLIP_LAYOUT_GRAY_ALPHA: return "alpha plane + gray plane";
        case CLIP_LAYOUT_BGRX_ALPHA: return "alpha plane + BGRx plane";
        case CLIP_LAYOUT_BGRA: return "interleaved BGRA";
        case CLIP_LAYOUT_CMYK_ALPHA: return "alpha plane + CMYK plane";
        default: return "unknown";
    }
}

uint32_t clip_offscreen_block_width(const clip_offscreen_attribute *a) {
    return a->packing_count > 10 && a->packing[10] ? a->packing[10] : K_BLOCK;
}

uint32_t clip_offscreen_block_height(const clip_offscreen_attribute *a) {
    return a->packing_count > 11 && a->packing[11] ? a->packing[11] : K_BLOCK;
}

void clip_offscreen_attribute_free(clip_offscreen_attribute *a) {
    if (!a) return;
    free(a->block_sizes);
    memset(a, 0, sizeof *a);
}

static void set_note(char *note, size_t cap, const char *prefix, const char *name, const char *suffix) {
    if (!note[0]) snprintf(note, cap, "%s%s%s", prefix, name ? name : "", suffix);
}

int clip_offscreen_attribute_parse(const uint8_t *blob, size_t size, clip_offscreen_attribute *out, clip_error *err) {
    clip_reader r;
    size_t section_start, section_end;
    clip_offscreen_attribute_free(out);
    out->header_size = 16;
    out->init_unknown = 20;
    out->init_trailer = 4;
    clip__reader_init(&r, blob, size);
    out->header_size = clip__read_u32(&r);
    out->parameter_section_size = clip__read_u32(&r);
    out->init_color_section_size = clip__read_u32(&r);
    out->block_size_section_size = clip__read_u32(&r);
    if (!r.ok) return clip__fail(err, "attribute blob too short");
    if (out->header_size != 16) snprintf(out->note, sizeof out->note, "header size %u (expected 16)", out->header_size);

    /* ---- Parameter ---- */
    section_start = out->header_size;
    r.pos = section_start;
    if (!clip__read_ustr_eq(&r, "Parameter") || !r.ok) {
        clip_offscreen_attribute_free(out);
        return clip__fail(err, "no 'Parameter' section in offscreen attribute");
    }
    out->width = clip__read_u32(&r);
    out->height = clip__read_u32(&r);
    out->block_count_x = clip__read_u32(&r);
    out->block_count_y = clip__read_u32(&r);
    section_end = section_start + out->parameter_section_size;
    if (section_end > size) {
        clip_offscreen_attribute_free(out);
        return clip__fail(err, "Parameter section runs past the blob");
    }
    while (r.ok && r.pos + 4 <= section_end) {
        uint32_t v = clip__read_u32(&r);
        if (out->packing_count < 16) out->packing[out->packing_count++] = v;
    }
    if (!r.ok) {
        clip_offscreen_attribute_free(out);
        return clip__fail(err, "truncated Parameter section");
    }

    /* ---- InitColor ---- */
    section_start = section_end;
    section_end = section_start + out->init_color_section_size;
    if (out->init_color_section_size && section_end <= size) {
        char *sec;
        r.pos = section_start;
        sec = clip__read_ustr(&r);
        if (r.ok && sec && strcmp(sec, "InitColor") == 0) {
            uint32_t n, i;
            out->init_unknown = clip__read_u32(&r);
            out->init_do_initialize = clip__read_u32(&r);
            out->init_alpha_value = clip__read_u32(&r);
            n = clip__read_u32(&r);
            out->init_trailer = clip__read_u32(&r);
            for (i = 0; i < n && i < 16 && r.ok && r.pos + 4 <= section_end; ++i)
                out->init_image_values[out->init_image_value_count++] = clip__read_u32(&r);
        } else {
            set_note(out->note, sizeof out->note, "unexpected section '", sec, "' where InitColor was expected");
        }
        free(sec);
    }

    /* ---- BlockSize ---- */
    section_start = section_end;
    section_end = section_start + out->block_size_section_size;
    if (out->block_size_section_size && section_end <= size) {
        char *sec;
        r.pos = section_start;
        sec = clip__read_ustr(&r);
        if (r.ok && sec && strcmp(sec, "BlockSize") == 0) {
            uint32_t count, i;
            clip__read_u32(&r);         /* 12 (constant in CSP files) */
            count = clip__read_u32(&r); /* number of blocks */
            clip__read_u32(&r);         /* 4: bytes per entry */
            if (r.ok) {
                size_t fit = r.pos < section_end ? (section_end - r.pos) / 4 : 0;
                if (count > fit) count = (uint32_t)fit;
                out->block_sizes = (uint32_t *)clip__malloc(sizeof(uint32_t) * (count ? count : 1));
                if (!out->block_sizes) {
                    free(sec);
                    clip_offscreen_attribute_free(out);
                    return clip__fail(err, "out of memory");
                }
            }
            for (i = 0; out->block_sizes && i < count && r.ok && r.pos + 4 <= section_end; ++i)
                out->block_sizes[out->block_size_count++] = clip__read_u32(&r);
        } else {
            set_note(out->note, sizeof out->note, "unexpected section '", sec, "' where BlockSize was expected");
        }
        free(sec);
    }

    if (out->width == 0 || out->height == 0) {
        clip_offscreen_attribute_free(out);
        return clip__fail(err, "offscreen has zero size");
    }
    if (out->block_count_x == 0) out->block_count_x = (out->width + K_BLOCK - 1) / K_BLOCK;
    if (out->block_count_y == 0) out->block_count_y = (out->height + K_BLOCK - 1) / K_BLOCK;
    return 1;
}

void clip_offscreen_data_free(clip_offscreen_data *d) {
    if (!d) return;
    free(d->blocks);
    free(d->block_status);
    free(d->block_checksum);
    memset(d, 0, sizeof *d);
}

int clip_offscreen_data_parse(const uint8_t *data, size_t size, clip_offscreen_data *out, clip_error *err) {
    clip_reader r;
    size_t cap = 0, i;
    clip_offscreen_data_free(out);
    clip__reader_init(&r, data, size);
    while (clip__reader_remaining(&r) >= 4 + K_BEGIN_LEN) {
        size_t block_start = r.pos;
        uint32_t block_size = clip__read_u32(&r); /* counts from the start of this size word */
        clip_offscreen_block b;
        uint32_t has_data;
        if (block_size == 11 || block_size == 13) {
            /* Not a block: the "BlockStatus" / "BlockCheckSum" trailer begins. */
            r.pos = block_start;
            break;
        }
        if (!clip__read_ustr_eq(&r, kBegin) || !r.ok) {
            clip_offscreen_data_free(out);
            return clip__fail(err, "expected BlockDataBeginChunk at offset %llu", (unsigned long long)block_start);
        }
        memset(&b, 0, sizeof b);
        b.index = clip__read_u32(&r);
        b.uncompressed_size = clip__read_u32(&r);
        b.width = clip__read_u32(&r);
        b.height = clip__read_u32(&r);
        has_data = clip__read_u32(&r);
        if (!r.ok) {
            clip_offscreen_data_free(out);
            return clip__fail(err, "truncated block fields");
        }
        b.has_data = has_data != 0;
        if (b.has_data) {
            uint32_t payload_len = clip__read_u32(&r); /* big-endian: 4 + zlib length */
            const uint8_t *len_le = clip__read_bytes(&r, 4);
            uint32_t zlen;
            if (!r.ok) {
                clip_offscreen_data_free(out);
                return clip__fail(err, "truncated block payload header");
            }
            zlen = (uint32_t)len_le[0] | ((uint32_t)len_le[1] << 8) | ((uint32_t)len_le[2] << 16) | ((uint32_t)len_le[3] << 24);
            if ((uint64_t)zlen + 4 != payload_len || zlen > clip__reader_remaining(&r)) {
                clip_offscreen_data_free(out);
                return clip__fail(err, "inconsistent block payload lengths at block %u", b.index);
            }
            b.compressed = clip__read_bytes(&r, zlen);
            b.compressed_size = zlen;
            b.payload_adler32 = clip_adler32(b.compressed, zlen, clip_adler32(len_le, 4, 1));
        }
        if (!clip__read_ustr_eq(&r, kEnd) || !r.ok) {
            clip_offscreen_data_free(out);
            return clip__fail(err, "expected BlockDataEndChunk after block %u", b.index);
        }
        if (block_size && block_start + block_size != r.pos) {
            if (block_start + block_size <= size) {
                r.pos = block_start + block_size;
            } else {
                clip_offscreen_data_free(out);
                return clip__fail(err, "block size word runs past the data");
            }
        }
        if (out->block_count == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            clip_offscreen_block *p = (clip_offscreen_block *)clip__realloc(out->blocks, ncap * sizeof *p);
            if (!p) {
                clip_offscreen_data_free(out);
                return clip__fail(err, "out of memory");
            }
            out->blocks = p;
            cap = ncap;
        }
        out->blocks[out->block_count++] = b;
    }
    /* Trailer: ustr name, u32 12, u32 count, u32 4, count x u32. */
    while (clip__reader_remaining(&r) >= 4) {
        size_t at = r.pos;
        char *name = clip__read_ustr(&r);
        uint32_t count, k;
        uint32_t **dst = NULL;
        size_t *dst_count = NULL;
        clip__read_u32(&r);
        count = clip__read_u32(&r);
        clip__read_u32(&r);
        if (!r.ok || !name || count > clip__reader_remaining(&r) / 4) {
            snprintf(out->note, sizeof out->note, "unreadable trailer at %llu", (unsigned long long)at);
            free(name);
            break;
        }
        if (strcmp(name, "BlockStatus") == 0) {
            dst = &out->block_status;
            dst_count = &out->block_status_count;
        } else if (strcmp(name, "BlockCheckSum") == 0) {
            dst = &out->block_checksum;
            dst_count = &out->block_checksum_count;
        }
        if (!dst) {
            snprintf(out->note, sizeof out->note, "unknown trailer section '%s'", name);
            free(name);
            break;
        }
        free(name);
        {
            uint32_t *p = (uint32_t *)clip__realloc(*dst, sizeof(uint32_t) * (*dst_count + count));
            if (!p) {
                clip_offscreen_data_free(out);
                return clip__fail(err, "out of memory");
            }
            *dst = p;
        }
        for (k = 0; k < count; ++k) (*dst)[(*dst_count)++] = clip__read_u32(&r);
    }
    for (i = 0; i < out->block_count && i < out->block_checksum_count; ++i)
        if (out->blocks[i].has_data && out->blocks[i].payload_adler32 != out->block_checksum[i]) ++out->checksum_mismatches;
    return 1;
}

int clip_offscreen_decode(const clip_offscreen_attribute *attr, const clip_offscreen_data *data, clip_image *out,
                          clip_pixel_layout *layout_used, clip_error *err) {
    clip_pixel_layout layout = CLIP_LAYOUT_UNKNOWN;
    const uint32_t bw = clip_offscreen_block_width(attr), bh = clip_offscreen_block_height(attr);
    const size_t px = (size_t)bw * bh;
    clip_buffer tile = {0};
    size_t bi;
    clip_image_free(out);
    if (!clip_image_init(out, (int)attr->width, (int)attr->height)) return clip__fail(err, "out of memory");
    for (bi = 0; bi < data->block_count; ++bi) {
        const clip_offscreen_block *b = &data->blocks[bi];
        clip_pixel_layout l;
        uint32_t bx, by, x, y;
        int ox, oy;
        if (!b->has_data) continue;
        if (!clip_zlib_inflate(b->compressed, b->compressed_size, &tile, err)) {
            char inner[sizeof err->message];
            if (err) {
                memcpy(inner, err->message, sizeof inner);
                clip__fail(err, "block %u: %s", b->index, inner);
            }
            goto fail;
        }
        if (tile.size == px * 5) l = CLIP_LAYOUT_BGRX_ALPHA;
        else if (tile.size == px * 4) l = CLIP_LAYOUT_BGRA;
        else if (tile.size == px * 2) l = CLIP_LAYOUT_GRAY_ALPHA;
        else if (tile.size == px) l = CLIP_LAYOUT_ALPHA_ONLY;
        else {
            clip__fail(err, "block %u inflates to unexpected size %llu", b->index, (unsigned long long)tile.size);
            goto fail;
        }
        if (layout == CLIP_LAYOUT_UNKNOWN) layout = l;
        bx = b->index % attr->block_count_x;
        by = b->index / attr->block_count_x;
        ox = (int)(bx * bw);
        oy = (int)(by * bh);
        for (y = 0; y < bh; ++y) {
            int cy = oy + (int)y;
            uint8_t *dst;
            if (cy >= out->height) break;
            dst = out->pixels + (size_t)cy * out->width * 4;
            for (x = 0; x < bw; ++x) {
                int cx = ox + (int)x;
                size_t i = (size_t)y * bw + x;
                const uint8_t *t = tile.data;
                uint8_t *o;
                if (cx >= out->width) break;
                o = dst + (size_t)cx * 4;
                switch (l) {
                    case CLIP_LAYOUT_BGRX_ALPHA: {
                        const uint8_t *img = t + px + i * 4;
                        o[0] = img[2]; o[1] = img[1]; o[2] = img[0]; o[3] = t[i];
                        break;
                    }
                    case CLIP_LAYOUT_BGRA: {
                        const uint8_t *img = t + i * 4;
                        o[0] = img[2]; o[1] = img[1]; o[2] = img[0]; o[3] = img[3];
                        break;
                    }
                    case CLIP_LAYOUT_GRAY_ALPHA:
                        o[0] = o[1] = o[2] = t[px + i];
                        o[3] = t[i];
                        break;
                    case CLIP_LAYOUT_ALPHA_ONLY: o[0] = o[1] = o[2] = o[3] = t[i]; break;
                    default: break;
                }
            }
        }
    }
    clip_buffer_free(&tile);
    if (layout_used) *layout_used = layout;
    return 1;
fail:
    clip_buffer_free(&tile);
    clip_image_free(out);
    return 0;
}

/* ---- encoding -------------------------------------------------------------- */

static int write_attribute(uint32_t w, uint32_t h, uint32_t bcx, uint32_t bcy, const uint32_t *block_sizes, size_t nblocks,
                           int paper_fill, clip_buffer *attribute) {
    clip_buffer param = {0}, init = {0}, bs = {0}, attr = {0};
    size_t i;
    int ok = clip__buf_ustr(&param, "Parameter") && clip__buf_u32be(&param, w) && clip__buf_u32be(&param, h) &&
             clip__buf_u32be(&param, bcx) && clip__buf_u32be(&param, bcy);
    for (i = 0; ok && i < 16; ++i) ok = clip__buf_u32be(&param, kPackingRgba[i]);
    ok = ok && clip__buf_ustr(&init, "InitColor") && clip__buf_u32be(&init, 20) &&
         clip__buf_u32be(&init, paper_fill ? 1 : 0) &&            /* DoInitialize */
         clip__buf_u32be(&init, paper_fill ? 0xFFFFFFFFu : 0) &&  /* AlphaValue */
         clip__buf_u32be(&init, paper_fill ? 4 : 0) &&            /* ImageValueCount */
         clip__buf_u32be(&init, 4);
    for (i = 0; ok && paper_fill && i < 4; ++i) ok = clip__buf_u32be(&init, 0);
    ok = ok && clip__buf_ustr(&bs, "BlockSize") && clip__buf_u32be(&bs, 12) && clip__buf_u32be(&bs, (uint32_t)nblocks) &&
         clip__buf_u32be(&bs, 4);
    for (i = 0; ok && i < nblocks; ++i) ok = clip__buf_u32be(&bs, block_sizes[i]);
    ok = ok && clip__buf_u32be(&attr, 16) && clip__buf_u32be(&attr, (uint32_t)param.size) &&
         clip__buf_u32be(&attr, (uint32_t)init.size) && clip__buf_u32be(&attr, (uint32_t)bs.size) &&
         clip__buf_append(&attr, param.data, param.size) && clip__buf_append(&attr, init.data, init.size) &&
         clip__buf_append(&attr, bs.data, bs.size);
    if (ok) clip__buf_move(attribute, &attr);
    clip_buffer_free(&param);
    clip_buffer_free(&init);
    clip_buffer_free(&bs);
    clip_buffer_free(&attr);
    return ok;
}

static int encode_impl(const clip_image *img, uint32_t w, uint32_t h, int store_data, int paper_fill, clip_buffer *attribute,
                       clip_buffer *blocks, clip_error *err) {
    const uint32_t bcx = (w + K_BLOCK - 1) / K_BLOCK, bcy = (h + K_BLOCK - 1) / K_BLOCK;
    const size_t px = (size_t)K_BLOCK * K_BLOCK, nblocks = (size_t)bcx * bcy;
    uint8_t *tile = NULL;
    uint32_t *block_sizes = NULL, *checksums = NULL;
    clip_buffer bd = {0}, blk = {0}, z = {0};
    uint32_t bx, by;
    size_t i, nb = 0;
    int ok = 1;

    tile = (uint8_t *)clip__malloc(px * 5);
    block_sizes = (uint32_t *)clip__malloc(sizeof(uint32_t) * (nblocks ? nblocks : 1));
    checksums = (uint32_t *)clip__malloc(sizeof(uint32_t) * (nblocks ? nblocks : 1));
    if (!tile || !block_sizes || !checksums) ok = 0;

    for (by = 0; ok && by < bcy; ++by) {
        for (bx = 0; ok && bx < bcx; ++bx) {
            uint32_t index = by * bcx + bx, x, y, size;
            int any = 0;
            if (store_data) {
                /* Alpha plane transparent, image plane white with 4th byte 255 (as CSP writes it). */
                memset(tile, 0, px);
                memset(tile + px, 255, px * 4);
                for (y = 0; y < K_BLOCK; ++y) {
                    int cy = (int)(by * K_BLOCK + y);
                    const uint8_t *src;
                    if (cy >= img->height) break;
                    src = img->pixels + (size_t)cy * img->width * 4;
                    for (x = 0; x < K_BLOCK; ++x) {
                        int cx = (int)(bx * K_BLOCK + x);
                        const uint8_t *c;
                        uint8_t *o;
                        size_t k = (size_t)y * K_BLOCK + x;
                        if (cx >= img->width) break;
                        c = src + (size_t)cx * 4;
                        tile[k] = c[3];
                        o = tile + px + k * 4;
                        /* CSP writes the 4th image byte as 255 and stores white under
                         * fully transparent pixels; files that deviate are reported
                         * as "layer image corrupted". */
                        if (c[3] == 0) {
                            o[0] = o[1] = o[2] = 255;
                        } else {
                            o[0] = c[2];
                            o[1] = c[1];
                            o[2] = c[0];
                        }
                        o[3] = 255;
                        any = any || c[3] != 0;
                    }
                }
            }
            blk.size = 0;
            ok = clip__buf_ustr(&blk, kBegin) && clip__buf_u32be(&blk, index) && clip__buf_u32be(&blk, (uint32_t)(px * 5)) &&
                 clip__buf_u32be(&blk, K_BLOCK) && clip__buf_u32be(&blk, K_BLOCK);
            if (ok && any) {
                size_t payload_at;
                ok = clip_zlib_deflate(tile, px * 5, 6, &z) && clip__buf_u32be(&blk, 1) &&
                     clip__buf_u32be(&blk, (uint32_t)(z.size + 4)) /* big-endian payload length */;
                payload_at = blk.size;
                ok = ok && clip__buf_u32le(&blk, (uint32_t)z.size) /* little-endian zlib length */ &&
                     clip__buf_append(&blk, z.data, z.size);
                /* BlockCheckSum = Adler-32 of the stored payload (LE length + zlib
                 * stream), as CSP computes it (verified on all blocks of a CSP file). */
                if (ok) checksums[nb] = clip_adler32(blk.data + payload_at, z.size + 4, 1);
            } else if (ok) {
                ok = clip__buf_u32be(&blk, 0);
                checksums[nb] = 0;
            }
            ok = ok && clip__buf_ustr(&blk, kEnd);
            size = (uint32_t)(blk.size + 4); /* includes the size word itself */
            ok = ok && clip__buf_u32be(&bd, size) && clip__buf_append(&bd, blk.data, blk.size);
            block_sizes[nb++] = size;
        }
    }
    /* Trailer: BlockStatus (all 0), then BlockCheckSum. */
    ok = ok && clip__buf_ustr(&bd, "BlockStatus") && clip__buf_u32be(&bd, 12) && clip__buf_u32be(&bd, (uint32_t)nb) &&
         clip__buf_u32be(&bd, 4);
    for (i = 0; ok && i < nb; ++i) ok = clip__buf_u32be(&bd, 0);
    ok = ok && clip__buf_ustr(&bd, "BlockCheckSum") && clip__buf_u32be(&bd, 12) && clip__buf_u32be(&bd, (uint32_t)nb) &&
         clip__buf_u32be(&bd, 4);
    for (i = 0; ok && i < nb; ++i) ok = clip__buf_u32be(&bd, checksums[i]);
    ok = ok && write_attribute(w, h, bcx, bcy, block_sizes, nb, paper_fill, attribute);
    if (ok) clip__buf_move(blocks, &bd);

    free(tile);
    free(block_sizes);
    free(checksums);
    clip_buffer_free(&bd);
    clip_buffer_free(&blk);
    clip_buffer_free(&z);
    return ok ? 1 : clip__fail(err, "out of memory encoding offscreen");
}

int clip_offscreen_encode(const clip_image *img, clip_buffer *attribute, clip_buffer *blocks, clip_error *err) {
    return encode_impl(img, (uint32_t)img->width, (uint32_t)img->height, 1, 0, attribute, blocks, err);
}

int clip_offscreen_encode_empty(uint32_t width, uint32_t height, int paper_fill, clip_buffer *attribute, clip_buffer *blocks,
                                clip_error *err) {
    return encode_impl(NULL, width, height, 0, paper_fill, attribute, blocks, err);
}
