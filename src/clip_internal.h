/* Internal helpers shared by the libclip modules.  Not part of the API. */
#ifndef LIBCLIP_INTERNAL_H
#define LIBCLIP_INTERNAL_H

#include <stdarg.h>
#include <stdio.h>

#include "libclip/clip.h"

/* ---- errors -------------------------------------------------------------- */
/* Formats into err (if any) and returns 0. */
int clip__fail(clip_error *err, const char *fmt, ...);

/* ---- memory -------------------------------------------------------------- */
void *clip__malloc(size_t n);
void *clip__calloc(size_t count, size_t size);
void *clip__realloc(void *p, size_t n);
char *clip__strdup(const char *s);
char *clip__strndup(const char *s, size_t n);

/* ---- byte buffer ----------------------------------------------------------- */
/* All append functions return 0 on allocation failure and leave b->size alone. */
int clip__buf_reserve(clip_buffer *b, size_t extra);
int clip__buf_append(clip_buffer *b, const void *data, size_t n);
int clip__buf_push(clip_buffer *b, uint8_t v);
int clip__buf_fill(clip_buffer *b, uint8_t v, size_t n);
int clip__buf_u32be(clip_buffer *b, uint32_t v);
int clip__buf_u64be(clip_buffer *b, uint64_t v);
int clip__buf_u32le(clip_buffer *b, uint32_t v);
/* u32 count + UTF-16BE code units. */
int clip__buf_ustr(clip_buffer *b, const char *utf8);
void clip__buf_move(clip_buffer *dst, clip_buffer *src);

uint32_t clip__rd_u32be(const uint8_t *p);
uint64_t clip__rd_u64be(const uint8_t *p);
void clip__wr_u64be(uint8_t *p, uint64_t v);

/* ---- big-endian reader ---------------------------------------------------- */
typedef struct clip_reader {
    const uint8_t *p;
    size_t n;
    size_t pos;
    int ok;
} clip_reader;

void clip__reader_init(clip_reader *r, const uint8_t *p, size_t n);
size_t clip__reader_remaining(const clip_reader *r);
uint32_t clip__read_u32(clip_reader *r);
const uint8_t *clip__read_bytes(clip_reader *r, size_t n);
/* Reads u32 count + UTF-16BE, returns a malloc'd UTF-8 string (or NULL with r->ok = 0). */
char *clip__read_ustr(clip_reader *r);
/* Reads a ustr and compares it with an ASCII literal without allocating. */
int clip__read_ustr_eq(clip_reader *r, const char *ascii);

/* ---- UTF-8 / UTF-16BE ------------------------------------------------------ */
char *clip__utf16be_to_utf8(const uint8_t *p, size_t units);
int clip__utf8_to_utf16be(const char *s, clip_buffer *out);

/* ---- files ---------------------------------------------------------------- */
FILE *clip__fopen(const char *utf8_path, const char *mode);

/* ---- image helpers ---------------------------------------------------------- */
/* Copy src into dst with src's origin at (dx, dy), clipped, no blending. */
void clip__image_blit(clip_image *dst, const clip_image *src, int dx, int dy);
/* Box-filter downscale; dst must be freed by the caller. */
int clip__image_downscale(const clip_image *src, int w, int h, clip_image *dst);

/* Offscreen encoder.  img == NULL writes an empty offscreen of w x h; `gray`
 * stores an alpha plane + gray plane (channel order 17) instead of BGRx. */
int clip__offscreen_encode_ex(const clip_image *img, uint32_t w, uint32_t h, int paper_fill, int gray, clip_buffer *attribute,
                              clip_buffer *blocks, clip_error *err);

#endif
