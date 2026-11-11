/* Buffers, big-endian I/O, UTF-16BE strings, errors, files and images. */
#include <stdlib.h>
#include <string.h>

#include "clip_internal.h"

#ifdef _WIN32
#include <wchar.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

const char *clip_version(void) { return "1.0.0"; }

/* ---- errors ---------------------------------------------------------------- */

int clip__fail(clip_error *err, const char *fmt, ...) {
    if (err) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err->message, sizeof err->message, fmt, ap);
        va_end(ap);
    }
    return 0;
}

/* ---- memory ---------------------------------------------------------------- */

void *clip__malloc(size_t n) { return malloc(n ? n : 1); }
void *clip__calloc(size_t count, size_t size) { return calloc(count ? count : 1, size ? size : 1); }
void *clip__realloc(void *p, size_t n) { return realloc(p, n ? n : 1); }

char *clip__strndup(const char *s, size_t n) {
    char *r = (char *)clip__malloc(n + 1);
    if (!r) return NULL;
    if (n) memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

char *clip__strdup(const char *s) { return clip__strndup(s ? s : "", s ? strlen(s) : 0); }

/* ---- buffer ---------------------------------------------------------------- */

void clip_buffer_free(clip_buffer *b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->size = b->capacity = 0;
}

int clip__buf_reserve(clip_buffer *b, size_t extra) {
    size_t need = b->size + extra;
    size_t cap;
    uint8_t *p;
    if (need < b->size) return 0; /* overflow */
    if (need <= b->capacity) return 1;
    cap = b->capacity ? b->capacity : 64;
    while (cap < need) cap = cap > ((size_t)-1) / 2 ? need : cap * 2;
    p = (uint8_t *)realloc(b->data, cap);
    if (!p) return 0;
    b->data = p;
    b->capacity = cap;
    return 1;
}

int clip__buf_append(clip_buffer *b, const void *data, size_t n) {
    if (!n) return 1;
    if (!clip__buf_reserve(b, n)) return 0;
    memcpy(b->data + b->size, data, n);
    b->size += n;
    return 1;
}

int clip__buf_push(clip_buffer *b, uint8_t v) {
    if (b->size == b->capacity && !clip__buf_reserve(b, 1)) return 0;
    b->data[b->size++] = v;
    return 1;
}

int clip__buf_fill(clip_buffer *b, uint8_t v, size_t n) {
    if (!clip__buf_reserve(b, n)) return 0;
    memset(b->data + b->size, v, n);
    b->size += n;
    return 1;
}

int clip__buf_u32be(clip_buffer *b, uint32_t v) {
    uint8_t t[4];
    t[0] = (uint8_t)(v >> 24);
    t[1] = (uint8_t)(v >> 16);
    t[2] = (uint8_t)(v >> 8);
    t[3] = (uint8_t)v;
    return clip__buf_append(b, t, 4);
}

int clip__buf_u64be(clip_buffer *b, uint64_t v) {
    uint8_t t[8];
    clip__wr_u64be(t, v);
    return clip__buf_append(b, t, 8);
}

int clip__buf_u32le(clip_buffer *b, uint32_t v) {
    uint8_t t[4];
    t[0] = (uint8_t)v;
    t[1] = (uint8_t)(v >> 8);
    t[2] = (uint8_t)(v >> 16);
    t[3] = (uint8_t)(v >> 24);
    return clip__buf_append(b, t, 4);
}

int clip__buf_ustr(clip_buffer *b, const char *utf8) {
    clip_buffer u = {0};
    int ok = clip__utf8_to_utf16be(utf8, &u) && clip__buf_u32be(b, (uint32_t)(u.size / 2)) && clip__buf_append(b, u.data, u.size);
    clip_buffer_free(&u);
    return ok;
}

void clip__buf_move(clip_buffer *dst, clip_buffer *src) {
    clip_buffer_free(dst);
    *dst = *src;
    src->data = NULL;
    src->size = src->capacity = 0;
}

uint32_t clip__rd_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

uint64_t clip__rd_u64be(const uint8_t *p) {
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

void clip__wr_u64be(uint8_t *p, uint64_t v) {
    int i;
    for (i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> ((7 - i) * 8));
}

/* ---- reader ---------------------------------------------------------------- */

void clip__reader_init(clip_reader *r, const uint8_t *p, size_t n) {
    r->p = p;
    r->n = n;
    r->pos = 0;
    r->ok = 1;
}

size_t clip__reader_remaining(const clip_reader *r) { return r->pos <= r->n ? r->n - r->pos : 0; }

uint32_t clip__read_u32(clip_reader *r) {
    uint32_t v;
    if (r->pos + 4 > r->n) {
        r->ok = 0;
        return 0;
    }
    v = clip__rd_u32be(r->p + r->pos);
    r->pos += 4;
    return v;
}

const uint8_t *clip__read_bytes(clip_reader *r, size_t n) {
    const uint8_t *q;
    if (r->pos + n > r->n || r->pos + n < r->pos) {
        r->ok = 0;
        return NULL;
    }
    q = r->p + r->pos;
    r->pos += n;
    return q;
}

char *clip__read_ustr(clip_reader *r) {
    uint32_t count = clip__read_u32(r);
    char *s;
    if (!r->ok || count > clip__reader_remaining(r) / 2) {
        r->ok = 0;
        return NULL;
    }
    s = clip__utf16be_to_utf8(r->p + r->pos, count);
    if (!s) {
        r->ok = 0;
        return NULL;
    }
    r->pos += (size_t)count * 2;
    return s;
}

int clip__read_ustr_eq(clip_reader *r, const char *ascii) {
    char *s = clip__read_ustr(r);
    int eq = s && strcmp(s, ascii) == 0;
    free(s);
    return eq;
}

/* ---- UTF conversion ---------------------------------------------------------- */

char *clip__utf16be_to_utf8(const uint8_t *p, size_t units) {
    /* At most 3 UTF-8 bytes per UTF-16 unit (a surrogate pair is 2 units -> 4 bytes). */
    char *out = (char *)clip__malloc(units * 3 + 1);
    size_t i, o = 0;
    if (!out) return NULL;
    for (i = 0; i < units; ++i) {
        uint32_t c = ((uint32_t)p[i * 2] << 8) | p[i * 2 + 1];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < units) {
            uint32_t lo = ((uint32_t)p[(i + 1) * 2] << 8) | p[(i + 1) * 2 + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (c >> 18));
            out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = '\0';
    return out;
}

static int put16(clip_buffer *out, uint32_t u) { return clip__buf_push(out, (uint8_t)(u >> 8)) && clip__buf_push(out, (uint8_t)u); }

int clip__utf8_to_utf16be(const char *s, clip_buffer *out) {
    size_t i = 0, n = s ? strlen(s) : 0;
    out->size = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        int extra, k;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else { cp = c & 0x07; extra = 3; }
        ++i;
        for (k = 0; k < extra && i < n; ++k, ++i) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            if (!put16(out, 0xD800 + (cp >> 10)) || !put16(out, 0xDC00 + (cp & 0x3FF))) return 0;
        } else if (!put16(out, cp)) {
            return 0;
        }
    }
    return 1;
}

/* ---- files ------------------------------------------------------------------- */

FILE *clip__fopen(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wmode[8];
    wchar_t *wpath;
    FILE *f = NULL;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    size_t i;
    if (n <= 0) return fopen(path, mode); /* not valid UTF-8: try the ANSI code page */
    wpath = (wchar_t *)clip__malloc((size_t)n * sizeof(wchar_t));
    if (!wpath) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, n);
    for (i = 0; mode[i] && i < 7; ++i) wmode[i] = (wchar_t)mode[i];
    wmode[i] = 0;
    f = _wfopen(wpath, wmode);
    free(wpath);
    return f;
#else
    return fopen(path, mode);
#endif
}

int clip_read_file(const char *path, clip_buffer *out, clip_error *err) {
    FILE *f;
    uint8_t chunk[1 << 16];
    size_t n;
    clip_buffer_free(out);
    f = clip__fopen(path, "rb");
    if (!f) return clip__fail(err, "cannot read %s", path);
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (!clip__buf_append(out, chunk, n)) {
            fclose(f);
            clip_buffer_free(out);
            return clip__fail(err, "out of memory reading %s", path);
        }
    }
    fclose(f);
    return 1;
}

int clip_write_file(const char *path, const uint8_t *data, size_t size, clip_error *err) {
    FILE *f = clip__fopen(path, "wb");
    int ok;
    if (!f) return clip__fail(err, "cannot write %s", path);
    ok = fwrite(data, 1, size, f) == size;
    ok = (fclose(f) == 0) && ok;
    return ok ? 1 : clip__fail(err, "cannot write %s", path);
}

/* ---- images ------------------------------------------------------------------ */

int clip_image_init(clip_image *img, int width, int height) {
    size_t bytes;
    img->pixels = NULL;
    img->width = width > 0 ? width : 0;
    img->height = height > 0 ? height : 0;
    bytes = (size_t)img->width * (size_t)img->height * 4;
    img->pixels = (uint8_t *)clip__calloc(bytes ? bytes : 1, 1);
    if (!img->pixels) {
        img->width = img->height = 0;
        return 0;
    }
    return 1;
}

void clip_image_free(clip_image *img) {
    if (!img) return;
    free(img->pixels);
    img->pixels = NULL;
    img->width = img->height = 0;
}

void clip__image_blit(clip_image *dst, const clip_image *src, int dx, int dy) {
    int x0 = dx > 0 ? dx : 0, y0 = dy > 0 ? dy : 0;
    int x1 = dx + src->width < dst->width ? dx + src->width : dst->width;
    int y1 = dy + src->height < dst->height ? dy + src->height : dst->height;
    int y;
    if (x1 <= x0 || y1 <= y0) return;
    for (y = y0; y < y1; ++y)
        memcpy(dst->pixels + ((size_t)y * dst->width + x0) * 4, src->pixels + ((size_t)(y - dy) * src->width + (x0 - dx)) * 4,
               (size_t)(x1 - x0) * 4);
}

int clip__image_downscale(const clip_image *src, int pw, int ph, clip_image *dst) {
    int w = src->width, h = src->height, x, y;
    if (!clip_image_init(dst, pw, ph)) return 0;
    for (y = 0; y < ph; ++y)
        for (x = 0; x < pw; ++x) {
            int sx0 = x * w / pw, sx1 = (x + 1) * w / pw;
            int sy0 = y * h / ph, sy1 = (y + 1) * h / ph;
            uint32_t sum[4] = {0, 0, 0, 0}, n = 0;
            int sx, sy, k;
            uint8_t *o = dst->pixels + ((size_t)y * pw + x) * 4;
            if (sx1 < sx0 + 1) sx1 = sx0 + 1;
            if (sy1 < sy0 + 1) sy1 = sy0 + 1;
            for (sy = sy0; sy < sy1; ++sy)
                for (sx = sx0; sx < sx1; ++sx) {
                    const uint8_t *p = src->pixels + ((size_t)sy * w + sx) * 4;
                    for (k = 0; k < 4; ++k) sum[k] += p[k];
                    ++n;
                }
            for (k = 0; k < 4; ++k) o[k] = (uint8_t)(sum[k] / n);
        }
    return 1;
}
