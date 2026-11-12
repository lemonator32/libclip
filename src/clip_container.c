/* CLIP STUDIO FORMAT container ("CSFCHUNK").
 *
 * A .clip file is a 24-byte header (magic, u64 file size, u64 header size)
 * followed by tagged chunks: "CHNK" + 4-character tag + u64 payload size.
 * See docs/FORMAT.md for the byte layout. */
#include <stdlib.h>
#include <string.h>

#include "clip_internal.h"

static const char kMagic[8] = {'C', 'S', 'F', 'C', 'H', 'U', 'N', 'K'};
#define K_HEADER_LEN 24

void clip_make_external_id(uint64_t seed, char out[CLIP_EXTERNAL_ID_LEN + 1]) {
    /* 32 hex digits from a splitmix64 sequence. */
    uint64_t s = seed;
    int k;
    memcpy(out, "extrnlid", 8);
    for (k = 0; k < 2; ++k) {
        uint64_t z;
        s += 0x9E3779B97F4A7C15ull;
        z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        snprintf(out + 8 + k * 16, 17, "%016llX", (unsigned long long)z);
    }
    out[CLIP_EXTERNAL_ID_LEN] = '\0';
}

void clip_container_free(clip_container *c) {
    if (!c) return;
    free(c->file);
    free(c->chunks);
    memset(c, 0, sizeof *c);
}

const clip_chunk *clip_container_find(const clip_container *c, const char *tag) {
    size_t i;
    for (i = 0; i < c->chunk_count; ++i)
        if (memcmp(c->chunks[i].tag, tag, 4) == 0) return &c->chunks[i];
    return NULL;
}

const clip_chunk *clip_container_find_external(const clip_container *c, const char *id) {
    size_t i;
    for (i = 0; i < c->chunk_count; ++i)
        if (memcmp(c->chunks[i].tag, "Exta", 4) == 0 && strcmp(c->chunks[i].external_id, id) == 0) return &c->chunks[i];
    return NULL;
}

const uint8_t *clip_container_sqlite(const clip_container *c, size_t *size) {
    const clip_chunk *ch = clip_container_find(c, "SQLi");
    if (size) *size = ch ? (size_t)ch->size : 0;
    return ch ? c->file + ch->data_offset : NULL;
}

const uint8_t *clip_container_external(const clip_container *c, const char *id, size_t *size) {
    const clip_chunk *ch = clip_container_find_external(c, id);
    if (size) *size = ch ? (size_t)ch->external_data_size : 0;
    return ch ? c->file + ch->external_data_offset : NULL;
}

static int add_chunk(clip_container *c, const clip_chunk *ch, size_t *cap) {
    if (c->chunk_count == *cap) {
        size_t ncap = *cap ? *cap * 2 : 16;
        clip_chunk *p = (clip_chunk *)clip__realloc(c->chunks, ncap * sizeof *p);
        if (!p) return 0;
        c->chunks = p;
        *cap = ncap;
    }
    c->chunks[c->chunk_count++] = *ch;
    return 1;
}

static int parse_owned(clip_container *out, clip_error *err) {
    const uint8_t *p = out->file;
    size_t n = out->file_size, pos = K_HEADER_LEN, cap = 0;
    if (n >= 100 && memcmp(p, "SQLite format 3", 16) == 0) {
        /* Legacy .lip: the whole file is the database, with tiles stored inline
         * in Offscreen.BlockData.  Present it as a single SQLi chunk. */
        clip_chunk c;
        memset(&c, 0, sizeof c);
        memcpy(c.tag, "SQLi", 4);
        c.size = n;
        out->bare_sqlite = 1;
        return add_chunk(out, &c, &cap) ? 1 : clip__fail(err, "out of memory");
    }
    if (n < K_HEADER_LEN || memcmp(p, kMagic, 8) != 0) return clip__fail(err, "not a CSFCHUNK (.clip) or SQLite (.lip) file");
    out->declared_file_size = clip__rd_u64be(p + 8);
    out->header_size = clip__rd_u64be(p + 16);
    while (pos + 16 <= n) {
        clip_chunk c;
        memset(&c, 0, sizeof c);
        if (memcmp(p + pos, "CHNK", 4) != 0) return clip__fail(err, "expected CHNK tag at offset %llu", (unsigned long long)pos);
        memcpy(c.tag, p + pos + 4, 4);
        c.offset = pos;
        c.size = clip__rd_u64be(p + pos + 8);
        c.data_offset = pos + 16;
        if (c.size > n - c.data_offset) return clip__fail(err, "chunk '%s' runs past end of file", c.tag);
        if (memcmp(c.tag, "Head", 4) == 0 && c.size >= 40) {
            const uint8_t *q = p + c.data_offset;
            out->head_version = clip__rd_u64be(q);
            out->head_sqlite_offset = clip__rd_u64be(q + 8);
            memcpy(out->document_guid, q + 24, 16);
            out->has_guid = 1;
        }
        if (memcmp(c.tag, "Exta", 4) == 0) {
            /* u64 id length, id bytes, u64 data length, data */
            const uint8_t *q = p + c.data_offset;
            uint64_t id_len;
            if (c.size < 16) return clip__fail(err, "Exta chunk too small");
            id_len = clip__rd_u64be(q);
            if (id_len != CLIP_EXTERNAL_ID_LEN || 8 + id_len + 8 > c.size) return clip__fail(err, "Exta chunk has unexpected id length");
            memcpy(c.external_id, q + 8, CLIP_EXTERNAL_ID_LEN);
            c.external_id[CLIP_EXTERNAL_ID_LEN] = '\0';
            c.external_data_size = clip__rd_u64be(q + 8 + id_len);
            c.external_data_offset = c.data_offset + 8 + id_len + 8;
            if (c.external_data_size > c.data_offset + c.size - c.external_data_offset)
                return clip__fail(err, "Exta payload runs past its chunk");
        }
        if (!add_chunk(out, &c, &cap)) return clip__fail(err, "out of memory");
        pos = (size_t)(c.data_offset + c.size);
        if (memcmp(c.tag, "Foot", 4) == 0) break;
    }
    if (!clip_container_find(out, "SQLi")) return clip__fail(err, "no CHNKSQLi chunk found");
    return 1;
}

int clip_container_parse(clip_container *out, const uint8_t *data, size_t size, clip_error *err) {
    clip_container_free(out);
    out->file = (uint8_t *)clip__malloc(size);
    if (!out->file) return clip__fail(err, "out of memory");
    if (size) memcpy(out->file, data, size);
    out->file_size = size;
    if (!parse_owned(out, err)) {
        clip_container_free(out);
        return 0;
    }
    return 1;
}

int clip_container_read(clip_container *out, const char *path, clip_error *err) {
    clip_buffer b = {0};
    clip_container_free(out);
    if (!clip_read_file(path, &b, err)) return 0;
    out->file = b.data; /* take ownership, no copy */
    out->file_size = b.size;
    if (!parse_owned(out, err)) {
        clip_container_free(out);
        return 0;
    }
    return 1;
}

static int put_chunk(clip_buffer *out, const char *tag, const uint8_t *payload, size_t n) {
    return clip__buf_append(out, "CHNK", 4) && clip__buf_append(out, tag, 4) && clip__buf_u64be(out, n) &&
           clip__buf_append(out, payload, n);
}

int clip_container_build(const uint8_t *sqlite_image, size_t sqlite_size, const clip_external_blob *externals,
                         size_t external_count, clip_buffer *out, clip_error *err) {
    uint8_t head[40];
    char guid[CLIP_EXTERNAL_ID_LEN + 1];
    size_t head_payload, i;
    uint64_t sqli_offset;
    int ok;

    out->size = 0;
    ok = clip__buf_append(out, kMagic, 8) && clip__buf_u64be(out, 0) /* file size, patched below */ &&
         clip__buf_u64be(out, K_HEADER_LEN);

    /* CHNKHead (40 bytes): u64 version 0x100, u64 offset of the CHNKSQLi
     * chunk (patched below; CSP seeks there and expects "CHNKSQLi"),
     * u64 16 = GUID length, 16 GUID bytes. */
    head_payload = out->size + 16;
    clip__wr_u64be(head, 0x100);
    clip__wr_u64be(head + 8, 0);
    clip__wr_u64be(head + 16, 16);
    clip_make_external_id((uint64_t)sqlite_size * 2654435761ull + external_count, guid);
    for (i = 0; i < 16; ++i) {
        unsigned v = 0;
        sscanf(guid + 8 + i * 2, "%2x", &v);
        head[24 + i] = (uint8_t)v;
    }
    ok = ok && put_chunk(out, "Head", head, sizeof head);

    for (i = 0; ok && i < external_count; ++i) {
        const clip_external_blob *e = &externals[i];
        size_t id_len = strlen(e->id);
        ok = clip__buf_append(out, "CHNKExta", 8) && clip__buf_u64be(out, 8 + id_len + 8 + e->size) && clip__buf_u64be(out, id_len) &&
             clip__buf_append(out, e->id, id_len) && clip__buf_u64be(out, e->size) && clip__buf_append(out, e->data, e->size);
    }
    sqli_offset = out->size;
    ok = ok && put_chunk(out, "SQLi", sqlite_image, sqlite_size) && put_chunk(out, "Foot", NULL, 0);
    if (!ok) {
        clip_buffer_free(out);
        return clip__fail(err, "out of memory building container");
    }
    clip__wr_u64be(out->data + head_payload + 8, sqli_offset);
    clip__wr_u64be(out->data + 8, (uint64_t)out->size);
    return 1;
}
