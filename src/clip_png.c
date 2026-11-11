/* Minimal PNG reader/writer for CanvasPreview: 8-bit, non-interlaced; grey,
 * grey+alpha, RGB, RGBA and palette images are read; RGBA is written. */
#include <stdlib.h>
#include <string.h>

#include "clip_internal.h"

static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

static int png_chunk(clip_buffer *out, const char *type, const uint8_t *data, size_t len) {
    size_t start;
    if (!clip__buf_u32be(out, (uint32_t)len)) return 0;
    start = out->size;
    if (!clip__buf_append(out, type, 4) || !clip__buf_append(out, data, len)) return 0;
    return clip__buf_u32be(out, clip_crc32(out->data + start, len + 4, 0));
}

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

int clip_png_decode(const uint8_t *data, size_t len, clip_image *out, clip_error *err) {
    size_t pos = 8, stride, plen = 0, tlen = 0;
    uint32_t w = 0, h = 0, y, x;
    int depth = 0, ctype = 0, interlace = 0, channels, seen_ihdr = 0, ok = 0;
    const uint8_t *palette = NULL, *trns = NULL;
    clip_buffer idat = {0}, raw = {0};
    uint8_t *prev = NULL, *cur = NULL;

    clip_image_free(out);
    if (len < 8 || memcmp(data, kSig, 8) != 0) return clip__fail(err, "not a PNG file");
    while (pos + 8 <= len) {
        uint32_t clen = clip__rd_u32be(data + pos);
        const uint8_t *type = data + pos + 4, *body = data + pos + 8;
        if (pos + 12 + (size_t)clen > len) {
            clip__fail(err, "truncated chunk");
            goto done;
        }
        if (!memcmp(type, "IHDR", 4)) {
            if (clen != 13) {
                clip__fail(err, "bad IHDR");
                goto done;
            }
            w = clip__rd_u32be(body);
            h = clip__rd_u32be(body + 4);
            depth = body[8];
            ctype = body[9];
            interlace = body[12];
            seen_ihdr = 1;
        } else if (!memcmp(type, "PLTE", 4)) {
            palette = body;
            plen = clen;
        } else if (!memcmp(type, "tRNS", 4)) {
            trns = body;
            tlen = clen;
        } else if (!memcmp(type, "IDAT", 4)) {
            if (!clip__buf_append(&idat, body, clen)) {
                clip__fail(err, "out of memory");
                goto done;
            }
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + (size_t)clen;
    }
    if (!seen_ihdr) { clip__fail(err, "missing IHDR"); goto done; }
    if (depth != 8) { clip__fail(err, "only 8-bit PNGs are supported"); goto done; }
    if (interlace) { clip__fail(err, "interlaced PNGs are not supported"); goto done; }
    switch (ctype) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: clip__fail(err, "unsupported colour type"); goto done;
    }
    if (w == 0 || h == 0 || w > 65535 || h > 65535) { clip__fail(err, "bad image size"); goto done; }
    if (!clip_zlib_inflate(idat.data, idat.size, &raw, err)) goto done;
    stride = (size_t)w * channels;
    if (raw.size < (stride + 1) * h) { clip__fail(err, "image data too short"); goto done; }

    prev = (uint8_t *)clip__calloc(stride, 1);
    cur = (uint8_t *)clip__malloc(stride);
    if (!prev || !cur || !clip_image_init(out, (int)w, (int)h)) { clip__fail(err, "out of memory"); goto done; }
    for (y = 0; y < h; ++y) {
        const uint8_t *line = raw.data + (size_t)y * (stride + 1);
        uint8_t filter = line[0], *tmp;
        uint8_t *dst = out->pixels + (size_t)y * w * 4;
        size_t i;
        memcpy(cur, line + 1, stride);
        for (i = 0; i < stride; ++i) {
            int a = i >= (size_t)channels ? cur[i - channels] : 0;
            int b = prev[i];
            int c = i >= (size_t)channels ? prev[i - channels] : 0;
            switch (filter) {
                case 0: break;
                case 1: cur[i] = (uint8_t)(cur[i] + a); break;
                case 2: cur[i] = (uint8_t)(cur[i] + b); break;
                case 3: cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1)); break;
                case 4: cur[i] = (uint8_t)(cur[i] + paeth(a, b, c)); break;
                default: clip_image_free(out); clip__fail(err, "bad filter type"); goto done;
            }
        }
        for (x = 0; x < w; ++x) {
            const uint8_t *p = cur + (size_t)x * channels;
            uint8_t *o = dst + (size_t)x * 4;
            switch (ctype) {
                case 0: o[0] = o[1] = o[2] = p[0]; o[3] = 255; break;
                case 2: o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = 255; break;
                case 3: {
                    size_t k = (size_t)p[0] * 3;
                    if (k + 2 < plen) {
                        o[0] = palette[k]; o[1] = palette[k + 1]; o[2] = palette[k + 2];
                        o[3] = p[0] < tlen ? trns[p[0]] : 255;
                    } else {
                        o[0] = o[1] = o[2] = o[3] = 0;
                    }
                    break;
                }
                case 4: o[0] = o[1] = o[2] = p[0]; o[3] = p[1]; break;
                case 6: memcpy(o, p, 4); break;
            }
        }
        tmp = prev;
        prev = cur;
        cur = tmp;
    }
    ok = 1;
done:
    free(prev);
    free(cur);
    clip_buffer_free(&idat);
    clip_buffer_free(&raw);
    return ok;
}

int clip_png_encode(const clip_image *img, clip_buffer *out) {
    uint8_t ihdr[13];
    uint32_t w = (uint32_t)img->width, h = (uint32_t)img->height, y;
    size_t stride = (size_t)w * 4;
    clip_buffer raw = {0}, z = {0};
    int ok;
    out->size = 0;
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16); ihdr[2] = (uint8_t)(w >> 8); ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16); ihdr[6] = (uint8_t)(h >> 8); ihdr[7] = (uint8_t)h;
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    if (!clip__buf_append(out, kSig, 8) || !png_chunk(out, "IHDR", ihdr, 13)) return 0;
    /* Filter type 0 on every row; the deflate stage compresses. */
    ok = clip__buf_reserve(&raw, (stride + 1) * h);
    for (y = 0; ok && y < h; ++y) ok = clip__buf_push(&raw, 0) && clip__buf_append(&raw, img->pixels + (size_t)y * stride, stride);
    ok = ok && clip_zlib_deflate(raw.data, raw.size, 6, &z) && png_chunk(out, "IDAT", z.data, z.size) && png_chunk(out, "IEND", NULL, 0);
    clip_buffer_free(&raw);
    clip_buffer_free(&z);
    return ok;
}
