/* zlib (RFC 1950) / DEFLATE (RFC 1951) decoder and encoder.
 *
 * Original work for libclip.  It is not derived from zlib's source.
 *
 * Encoder pipeline:
 *   1. LZ77: a hash of the next three bytes indexes chains of earlier
 *      positions inside the 32 KiB window; the longest match within a
 *      level-dependent chain budget is taken, with one step of lazy
 *      evaluation.
 *   2. Symbols are buffered per block; each block is emitted with the
 *      cheapest of dynamic Huffman, fixed Huffman or stored.
 *   3. Huffman lengths come from a binary heap over (weight, node index) and
 *      are limited by halving frequencies until the limit holds.
 */
#include <stdlib.h>
#include <string.h>

#include "clip_internal.h"

static const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t kCodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/* ========================================================================== */
/* Checksums                                                                  */
/* ========================================================================== */

uint32_t clip_crc32(const uint8_t *data, size_t len, uint32_t seed) {
    static uint32_t table[256];
    static int built = 0;
    uint32_t c;
    size_t i;
    if (!built) {
        uint32_t n;
        for (n = 0; n < 256; ++n) {
            uint32_t v = n;
            int k;
            for (k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[n] = v;
        }
        built = 1;
    }
    c = seed ^ 0xFFFFFFFFu;
    for (i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t clip_adler32(const uint8_t *data, size_t len, uint32_t seed) {
    uint32_t a = seed & 0xFFFF, b = (seed >> 16) & 0xFFFF;
    while (len > 0) {
        size_t n = len < 5552 ? len : 5552;
        len -= n;
        while (n--) {
            a += *data++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

/* ========================================================================== */
/* Inflate                                                                    */
/* ========================================================================== */

typedef struct bit_reader {
    const uint8_t *data;
    size_t len, pos;
    uint32_t buf;
    int nbits;
} bit_reader;

static int br_bits(bit_reader *br, int n, uint32_t *v) {
    if (n == 0) {
        *v = 0;
        return 1;
    }
    while (br->nbits < n) {
        if (br->pos >= br->len) return 0;
        br->buf |= (uint32_t)br->data[br->pos++] << br->nbits;
        br->nbits += 8;
    }
    *v = br->buf & ((1u << n) - 1);
    br->buf >>= n;
    br->nbits -= n;
    return 1;
}

static void br_align(bit_reader *br) {
    br->buf = 0;
    br->nbits = 0;
}

static const uint8_t *br_read_bytes(bit_reader *br, size_t n) {
    const uint8_t *p;
    if (br->pos + n > br->len) return NULL;
    p = br->data + br->pos;
    br->pos += n;
    return p;
}

/* Canonical Huffman decoding table. */
typedef struct huffman {
    uint16_t count[16];
    uint16_t symbol[320];
} huffman;

static int huff_build(huffman *h, const uint8_t *lengths, int n) {
    uint16_t offs[16];
    int i, len, left = 1;
    memset(h->count, 0, sizeof h->count);
    memset(h->symbol, 0, sizeof h->symbol);
    for (i = 0; i < n; ++i) h->count[lengths[i]]++;
    h->count[0] = 0;
    for (len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return 0;
    }
    memset(offs, 0, sizeof offs);
    for (len = 1; len < 15; ++len) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (i = 0; i < n; ++i)
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = (uint16_t)i;
    return 1;
}

static int huff_decode(const huffman *h, bit_reader *br, int *sym) {
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len < 16; ++len) {
        uint32_t b;
        int c;
        if (!br_bits(br, 1, &b)) return 0;
        code |= (int)b;
        c = h->count[len];
        if (code - c < first) {
            *sym = h->symbol[index + (code - first)];
            return 1;
        }
        index += c;
        first += c;
        first <<= 1;
        code <<= 1;
    }
    return 0;
}

static int inflate_codes(bit_reader *br, clip_buffer *out, const huffman *lit, const huffman *dist, clip_error *err) {
    for (;;) {
        int sym;
        if (!huff_decode(lit, br, &sym)) return clip__fail(err, "truncated literal/length code");
        if (sym < 256) {
            if (!clip__buf_push(out, (uint8_t)sym)) return clip__fail(err, "out of memory");
        } else if (sym == 256) {
            return 1;
        } else {
            uint32_t extra;
            size_t len, d, from, i;
            int dsym;
            sym -= 257;
            if (sym >= 29) return clip__fail(err, "invalid length symbol");
            if (!br_bits(br, kLenExtra[sym], &extra)) return clip__fail(err, "truncated length extra bits");
            len = kLenBase[sym] + extra;
            if (!huff_decode(dist, br, &dsym)) return clip__fail(err, "truncated distance code");
            if (dsym >= 30) return clip__fail(err, "invalid distance symbol");
            if (!br_bits(br, kDistExtra[dsym], &extra)) return clip__fail(err, "truncated distance extra bits");
            d = kDistBase[dsym] + extra;
            if (d > out->size) return clip__fail(err, "distance beyond start of output");
            if (!clip__buf_reserve(out, len)) return clip__fail(err, "out of memory");
            from = out->size - d;
            for (i = 0; i < len; ++i) out->data[out->size++] = out->data[from + i];
        }
    }
}

static int inflate_stored(bit_reader *br, clip_buffer *out, clip_error *err) {
    const uint8_t *p;
    uint16_t len, nlen;
    br_align(br);
    if (!(p = br_read_bytes(br, 4))) return clip__fail(err, "truncated stored block header");
    len = (uint16_t)(p[0] | (p[1] << 8));
    nlen = (uint16_t)(p[2] | (p[3] << 8));
    if ((uint16_t)~len != nlen) return clip__fail(err, "stored block length check failed");
    if (!(p = br_read_bytes(br, len))) return clip__fail(err, "truncated stored block data");
    return clip__buf_append(out, p, len) ? 1 : clip__fail(err, "out of memory");
}

static int inflate_fixed(bit_reader *br, clip_buffer *out, clip_error *err) {
    huffman lit, dist;
    uint8_t l[288], d[30];
    int i = 0;
    for (; i < 144; ++i) l[i] = 8;
    for (; i < 256; ++i) l[i] = 9;
    for (; i < 280; ++i) l[i] = 7;
    for (; i < 288; ++i) l[i] = 8;
    huff_build(&lit, l, 288);
    for (i = 0; i < 30; ++i) d[i] = 5;
    huff_build(&dist, d, 30);
    return inflate_codes(br, out, &lit, &dist, err);
}

static int inflate_dynamic(bit_reader *br, clip_buffer *out, clip_error *err) {
    uint32_t hlit, hdist, hclen;
    int nlen, ndist, ncode, i, idx = 0;
    uint8_t lengths[320];
    huffman lencode, lit, dist;
    if (!br_bits(br, 5, &hlit) || !br_bits(br, 5, &hdist) || !br_bits(br, 4, &hclen)) return clip__fail(err, "truncated dynamic header");
    nlen = (int)hlit + 257;
    ndist = (int)hdist + 1;
    ncode = (int)hclen + 4;
    if (nlen > 286 || ndist > 30) return clip__fail(err, "bad dynamic code counts");
    memset(lengths, 0, sizeof lengths);
    for (i = 0; i < ncode; ++i) {
        uint32_t v;
        if (!br_bits(br, 3, &v)) return clip__fail(err, "truncated code length codes");
        lengths[kCodeLengthOrder[i]] = (uint8_t)v;
    }
    if (!huff_build(&lencode, lengths, 19)) return clip__fail(err, "bad code length code");
    while (idx < nlen + ndist) {
        int sym;
        if (!huff_decode(&lencode, br, &sym)) return clip__fail(err, "truncated code lengths");
        if (sym < 16) {
            lengths[idx++] = (uint8_t)sym;
        } else {
            uint8_t rep_len = 0;
            uint32_t rep;
            if (sym == 16) {
                if (idx == 0) return clip__fail(err, "repeat with no previous length");
                rep_len = lengths[idx - 1];
                if (!br_bits(br, 2, &rep)) return clip__fail(err, "truncated repeat");
                rep += 3;
            } else if (sym == 17) {
                if (!br_bits(br, 3, &rep)) return clip__fail(err, "truncated repeat");
                rep += 3;
            } else {
                if (!br_bits(br, 7, &rep)) return clip__fail(err, "truncated repeat");
                rep += 11;
            }
            if (idx + (int)rep > nlen + ndist) return clip__fail(err, "too many code lengths");
            while (rep--) lengths[idx++] = rep_len;
        }
    }
    if (lengths[256] == 0) return clip__fail(err, "no end-of-block code");
    if (!huff_build(&lit, lengths, nlen)) return clip__fail(err, "bad literal/length code");
    if (!huff_build(&dist, lengths + nlen, ndist)) return clip__fail(err, "bad distance code");
    return inflate_codes(br, out, &lit, &dist, err);
}

int clip_zlib_inflate(const uint8_t *data, size_t len, clip_buffer *out, clip_error *err) {
    bit_reader br;
    const uint8_t *p;
    uint8_t cmf, flg;
    out->size = 0;
    if (len < 6) return clip__fail(err, "zlib stream too short");
    cmf = data[0];
    flg = data[1];
    if ((cmf & 0x0F) != 8) return clip__fail(err, "unsupported zlib compression method");
    if (((cmf << 8) | flg) % 31 != 0) return clip__fail(err, "bad zlib header check");
    if (flg & 0x20) return clip__fail(err, "preset dictionary not supported");
    memset(&br, 0, sizeof br);
    br.data = data + 2;
    br.len = len - 2;
    for (;;) {
        uint32_t final_block, type;
        int ok;
        if (!br_bits(&br, 1, &final_block) || !br_bits(&br, 2, &type)) return clip__fail(err, "truncated block header");
        switch (type) {
            case 0: ok = inflate_stored(&br, out, err); break;
            case 1: ok = inflate_fixed(&br, out, err); break;
            case 2: ok = inflate_dynamic(&br, out, err); break;
            default: return clip__fail(err, "reserved block type");
        }
        if (!ok) return 0;
        if (final_block) break;
    }
    br_align(&br);
    if ((p = br_read_bytes(&br, 4)) != NULL) { /* trailer optional, verified when present */
        uint32_t expect = clip__rd_u32be(p);
        if (expect != clip_adler32(out->data, out->size, 1)) return clip__fail(err, "adler32 mismatch");
    }
    return 1;
}

/* ========================================================================== */
/* Deflate                                                                    */
/* ========================================================================== */

#define K_WINDOW 32768
#define K_MIN_MATCH 3
#define K_MAX_MATCH 258
#define K_HASH_BITS 15
#define K_HASH_SIZE (1 << K_HASH_BITS)
#define K_BLOCK_SYMBOLS (1 << 15)

typedef struct sym {
    uint16_t value; /* literal byte or match length 3..258 */
    uint16_t dist;  /* 0 for literals */
} sym;

typedef struct bit_writer {
    clip_buffer *out;
    uint64_t acc;
    int n;
    int ok;
} bit_writer;

static void bw_put(bit_writer *bw, uint32_t value, int bits) {
    bw->acc |= (uint64_t)value << bw->n;
    bw->n += bits;
    while (bw->n >= 8) {
        if (!clip__buf_push(bw->out, (uint8_t)bw->acc)) bw->ok = 0;
        bw->acc >>= 8;
        bw->n -= 8;
    }
}

static void bw_align(bit_writer *bw) {
    if (bw->n > 0) bw_put(bw, 0, 8 - bw->n);
}

/* ---- Huffman code construction ------------------------------------------- */

typedef struct hnode {
    uint64_t weight;
    int left, right; /* leaf: left < 0, right = symbol */
} hnode;

typedef struct hitem {
    uint64_t weight;
    int node;
} hitem;

static int hitem_less(hitem a, hitem b) { return a.weight < b.weight || (a.weight == b.weight && a.node < b.node); }

static void heap_push(hitem *heap, int *size, hitem v) {
    int i = (*size)++;
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (!hitem_less(v, heap[parent])) break;
        heap[i] = heap[parent];
        i = parent;
    }
    heap[i] = v;
}

static hitem heap_pop(hitem *heap, int *size) {
    hitem top = heap[0], last = heap[--(*size)];
    int i = 0, n = *size;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && hitem_less(heap[c + 1], heap[c])) ++c;
        if (!hitem_less(heap[c], last)) break;
        heap[i] = heap[c];
        i = c;
    }
    if (n > 0) heap[i] = last;
    return top;
}

/* Code lengths for freq[0..n) no longer than `limit`; n <= 288. */
static void build_lengths(const uint32_t *freq_in, int n, int limit, uint8_t *len) {
    uint32_t freq[288];
    hnode nodes[2 * 288];
    hitem heap[288];
    int stack_node[2 * 288], stack_depth[2 * 288];
    int used = 0, i;
    memcpy(freq, freq_in, sizeof(uint32_t) * (size_t)n);
    for (i = 0; i < n; ++i) used += freq[i] != 0;
    for (i = 0; used < 2 && i < n; ++i)
        if (freq[i] == 0) {
            freq[i] = 1;
            ++used;
        }
    for (;;) {
        int node_count = 0, heap_size = 0, sp = 0, max_depth = 0;
        for (i = 0; i < n; ++i) {
            hitem it;
            if (!freq[i]) continue;
            nodes[node_count].weight = freq[i];
            nodes[node_count].left = -1;
            nodes[node_count].right = i;
            it.weight = freq[i];
            it.node = node_count++;
            heap_push(heap, &heap_size, it);
        }
        while (heap_size > 1) {
            hitem a = heap_pop(heap, &heap_size);
            hitem b = heap_pop(heap, &heap_size);
            hitem it;
            nodes[node_count].weight = a.weight + b.weight;
            nodes[node_count].left = a.node;
            nodes[node_count].right = b.node;
            it.weight = a.weight + b.weight;
            it.node = node_count++;
            heap_push(heap, &heap_size, it);
        }
        memset(len, 0, (size_t)n);
        stack_node[sp] = heap[0].node;
        stack_depth[sp++] = 0;
        while (sp > 0) {
            int idx = stack_node[--sp], depth = stack_depth[sp];
            const hnode *node = &nodes[idx];
            if (node->left < 0) {
                len[node->right] = (uint8_t)(depth > 1 ? depth : 1);
                if (depth > max_depth) max_depth = depth;
            } else {
                stack_node[sp] = node->left;
                stack_depth[sp++] = depth + 1;
                stack_node[sp] = node->right;
                stack_depth[sp++] = depth + 1;
            }
        }
        if (max_depth <= limit) return;
        for (i = 0; i < n; ++i)
            if (freq[i]) freq[i] = (freq[i] + 1) / 2;
    }
}

/* Canonical codes, bit-reversed for LSB-first output. */
static void build_codes(const uint8_t *len, int n, uint16_t *codes) {
    uint16_t count[16], next[16], code = 0;
    int bits, i;
    memset(count, 0, sizeof count);
    memset(next, 0, sizeof next);
    for (i = 0; i < n; ++i)
        if (len[i]) count[len[i]]++;
    for (bits = 1; bits < 16; ++bits) {
        code = (uint16_t)((code + count[bits - 1]) << 1);
        next[bits] = code;
    }
    for (i = 0; i < n; ++i) {
        int l = len[i], b;
        uint16_t c, r = 0;
        codes[i] = 0;
        if (!l) continue;
        c = next[l]++;
        for (b = 0; b < l; ++b) r = (uint16_t)((r << 1) | ((c >> b) & 1));
        codes[i] = r;
    }
}

typedef struct cl_sym {
    uint8_t sym, extra;
} cl_sym;

/* RLE of code lengths with the code-length alphabet; returns the count. */
static int encode_lengths(const uint8_t *lens, int n, cl_sym *out) {
    int i = 0, o = 0;
    while (i < n) {
        uint8_t v = lens[i];
        int run = 1, left;
        while (i + run < n && lens[i + run] == v) ++run;
        left = run;
        if (v == 0) {
            while (left >= 11) {
                int k = left < 138 ? left : 138;
                out[o].sym = 18;
                out[o++].extra = (uint8_t)(k - 11);
                left -= k;
            }
            if (left >= 3) {
                out[o].sym = 17;
                out[o++].extra = (uint8_t)(left - 3);
                left = 0;
            }
            while (left--) {
                out[o].sym = 0;
                out[o++].extra = 0;
            }
        } else {
            out[o].sym = v;
            out[o++].extra = 0;
            --left;
            while (left >= 3) {
                int k = left < 6 ? left : 6;
                out[o].sym = 16;
                out[o++].extra = (uint8_t)(k - 3);
                left -= k;
            }
            while (left--) {
                out[o].sym = v;
                out[o++].extra = 0;
            }
        }
        i += run;
    }
    return o;
}

static int extra_bits_of(uint8_t cl) { return cl == 16 ? 2 : cl == 17 ? 3 : cl == 18 ? 7 : 0; }

typedef struct deflate_state {
    bit_writer bw;
    const uint8_t *data;
    uint8_t len_code[259];
    uint8_t dist_code[32769];
} deflate_state;

static void init_tables(deflate_state *s) {
    int i;
    for (i = 0; i < 29; ++i) {
        int end = i + 1 < 29 ? kLenBase[i + 1] : 259, l;
        for (l = kLenBase[i]; l < end && l <= 258; ++l) s->len_code[l] = (uint8_t)i;
    }
    s->len_code[258] = 28;
    for (i = 0; i < 30; ++i) {
        int end = i + 1 < 30 ? kDistBase[i + 1] : 32769, d;
        for (d = kDistBase[i]; d < end; ++d) s->dist_code[d] = (uint8_t)i;
    }
}

static void emit_symbols(deflate_state *s, const sym *syms, size_t nsyms, const uint16_t *lit_codes, const uint8_t *lit_len,
                         const uint16_t *dist_codes, const uint8_t *dist_len) {
    size_t i;
    for (i = 0; i < nsyms; ++i) {
        const sym *y = &syms[i];
        int lc, dc;
        if (y->dist == 0) {
            bw_put(&s->bw, lit_codes[y->value], lit_len[y->value]);
            continue;
        }
        lc = s->len_code[y->value];
        dc = s->dist_code[y->dist];
        bw_put(&s->bw, lit_codes[257 + lc], lit_len[257 + lc]);
        if (kLenExtra[lc]) bw_put(&s->bw, (uint32_t)(y->value - kLenBase[lc]), kLenExtra[lc]);
        bw_put(&s->bw, dist_codes[dc], dist_len[dc]);
        if (kDistExtra[dc]) bw_put(&s->bw, (uint32_t)(y->dist - kDistBase[dc]), kDistExtra[dc]);
    }
    bw_put(&s->bw, lit_codes[256], lit_len[256]);
}

static void emit_stored(deflate_state *s, size_t start, size_t end, int final) {
    do {
        size_t n = end - start < 65535 ? end - start : 65535, i;
        int last = final && start + n == end;
        bw_put(&s->bw, last ? 1 : 0, 1);
        bw_put(&s->bw, 0, 2);
        bw_align(&s->bw);
        bw_put(&s->bw, (uint32_t)n, 16);
        bw_put(&s->bw, (uint32_t)(~n & 0xFFFF), 16);
        for (i = 0; i < n; ++i) bw_put(&s->bw, s->data[start + i], 8);
        start += n;
    } while (start < end);
}

static void emit_block(deflate_state *s, const sym *syms, size_t nsyms, size_t raw_start, size_t raw_end, int final) {
    uint32_t lit_freq[286], dist_freq[30], cl_freq[19];
    uint8_t lit_len[286], dist_len[30], cl_len[19], all[316], fix_lit[288], fix_dist[30];
    uint16_t lit_codes[288], dist_codes[30], cl_codes[19];
    cl_sym cl[316];
    uint64_t extra_bits = 0, dyn_bits, fix_bits, stored_bits;
    int hlit = 286, hdist = 30, hclen = 19, ncl, i;
    size_t k, raw;

    memset(lit_freq, 0, sizeof lit_freq);
    memset(dist_freq, 0, sizeof dist_freq);
    for (k = 0; k < nsyms; ++k) {
        if (syms[k].dist == 0) {
            lit_freq[syms[k].value]++;
        } else {
            int lc = s->len_code[syms[k].value], dc = s->dist_code[syms[k].dist];
            lit_freq[257 + lc]++;
            dist_freq[dc]++;
            extra_bits += (uint64_t)kLenExtra[lc] + kDistExtra[dc];
        }
    }
    lit_freq[256]++;

    /* dynamic */
    build_lengths(lit_freq, 286, 15, lit_len);
    build_lengths(dist_freq, 30, 15, dist_len);
    while (hlit > 257 && lit_len[hlit - 1] == 0) --hlit;
    while (hdist > 1 && dist_len[hdist - 1] == 0) --hdist;
    memcpy(all, lit_len, (size_t)hlit);
    memcpy(all + hlit, dist_len, (size_t)hdist);
    ncl = encode_lengths(all, hlit + hdist, cl);
    memset(cl_freq, 0, sizeof cl_freq);
    for (i = 0; i < ncl; ++i) cl_freq[cl[i].sym]++;
    build_lengths(cl_freq, 19, 7, cl_len);
    while (hclen > 4 && cl_len[kCodeLengthOrder[hclen - 1]] == 0) --hclen;

    dyn_bits = 3 + 5 + 5 + 4 + 3 * (uint64_t)hclen + extra_bits;
    for (i = 0; i < ncl; ++i) dyn_bits += (uint64_t)cl_len[cl[i].sym] + extra_bits_of(cl[i].sym);
    for (i = 0; i < 286; ++i) dyn_bits += (uint64_t)lit_freq[i] * lit_len[i];
    for (i = 0; i < 30; ++i) dyn_bits += (uint64_t)dist_freq[i] * dist_len[i];

    /* fixed */
    for (i = 0; i < 288; ++i) fix_lit[i] = (uint8_t)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
    for (i = 0; i < 30; ++i) fix_dist[i] = 5;
    fix_bits = 3 + extra_bits;
    for (i = 0; i < 286; ++i) fix_bits += (uint64_t)lit_freq[i] * fix_lit[i];
    for (i = 0; i < 30; ++i) fix_bits += (uint64_t)dist_freq[i] * 5;

    /* stored */
    raw = raw_end - raw_start;
    stored_bits = (raw + 5 * ((raw + 65534) / 65535 + (raw == 0))) * 8 + 7;

    if (stored_bits <= dyn_bits && stored_bits <= fix_bits) {
        emit_stored(s, raw_start, raw_end, final);
    } else if (fix_bits <= dyn_bits) {
        bw_put(&s->bw, final ? 1 : 0, 1);
        bw_put(&s->bw, 1, 2);
        build_codes(fix_lit, 288, lit_codes);
        build_codes(fix_dist, 30, dist_codes);
        emit_symbols(s, syms, nsyms, lit_codes, fix_lit, dist_codes, fix_dist);
    } else {
        bw_put(&s->bw, final ? 1 : 0, 1);
        bw_put(&s->bw, 2, 2);
        bw_put(&s->bw, (uint32_t)(hlit - 257), 5);
        bw_put(&s->bw, (uint32_t)(hdist - 1), 5);
        bw_put(&s->bw, (uint32_t)(hclen - 4), 4);
        for (i = 0; i < hclen; ++i) bw_put(&s->bw, cl_len[kCodeLengthOrder[i]], 3);
        build_codes(cl_len, 19, cl_codes);
        for (i = 0; i < ncl; ++i) {
            int e = extra_bits_of(cl[i].sym);
            bw_put(&s->bw, cl_codes[cl[i].sym], cl_len[cl[i].sym]);
            if (e) bw_put(&s->bw, cl[i].extra, e);
        }
        build_codes(lit_len, 286, lit_codes);
        build_codes(dist_len, 30, dist_codes);
        emit_symbols(s, syms, nsyms, lit_codes, lit_len, dist_codes, dist_len);
    }
}

static uint32_t hash3(const uint8_t *p) {
    return (((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ p[2]) & (K_HASH_SIZE - 1);
}

typedef struct match_ctx {
    const uint8_t *data;
    size_t len;
    int32_t *head, *prev;
    int max_chain, nice_len;
} match_ctx;

static void mc_insert(match_ctx *m, size_t pos) {
    uint32_t h;
    if (pos + K_MIN_MATCH > m->len) return;
    h = hash3(m->data + pos);
    m->prev[pos & (K_WINDOW - 1)] = m->head[h];
    m->head[h] = (int32_t)pos;
}

static int mc_longest(const match_ctx *m, size_t pos, int *best_dist) {
    int best = 0, limit, chain;
    int32_t cand;
    *best_dist = 0;
    if (pos + K_MIN_MATCH > m->len) return 0;
    limit = (int)(m->len - pos < K_MAX_MATCH ? m->len - pos : K_MAX_MATCH);
    cand = m->head[hash3(m->data + pos)];
    for (chain = 0; cand >= 0 && chain < m->max_chain; ++chain) {
        size_t dist = pos - (size_t)cand;
        const uint8_t *a, *b;
        int32_t nxt;
        if (dist == 0 || dist >= (size_t)K_WINDOW) break;
        a = m->data + cand;
        b = m->data + pos;
        if (a[best] == b[best]) {
            int l = 0;
            while (l < limit && a[l] == b[l]) ++l;
            if (l > best) {
                best = l;
                *best_dist = (int)dist;
                if (l >= m->nice_len || l == limit) break;
            }
        }
        nxt = m->prev[cand & (K_WINDOW - 1)];
        if (nxt >= cand) break;
        cand = nxt;
    }
    return best >= K_MIN_MATCH ? best : 0;
}

static int deflate_stored(const uint8_t *data, size_t len, clip_buffer *out) {
    size_t pos = 0;
    if (!clip__buf_push(out, 0x78) || !clip__buf_push(out, 0x01)) return 0;
    do {
        size_t n = len - pos > 65535 ? 65535 : len - pos;
        uint8_t hdr[5];
        hdr[0] = (uint8_t)(pos + n == len ? 1 : 0);
        hdr[1] = (uint8_t)(n & 0xFF);
        hdr[2] = (uint8_t)(n >> 8);
        hdr[3] = (uint8_t)(~n & 0xFF);
        hdr[4] = (uint8_t)((~n >> 8) & 0xFF);
        if (!clip__buf_append(out, hdr, 5) || !clip__buf_append(out, data + pos, n)) return 0;
        pos += n;
    } while (pos < len);
    return clip__buf_u32be(out, clip_adler32(data, len, 1));
}

int clip_zlib_deflate(const uint8_t *data, size_t len, int level, clip_buffer *out) {
    static const int kChain[10] = {0, 4, 8, 16, 24, 32, 48, 128, 512, 2048};
    static const int kNice[10] = {0, 16, 32, 64, 96, 128, 160, 258, 258, 258};
    deflate_state *s;
    match_ctx m;
    sym *syms;
    size_t nsyms = 0, block_start = 0, pos = 0;
    int lazy, i, ok;

    out->size = 0;
    if (level <= 0) return deflate_stored(data, len, out);
    if (level > 9) level = 9;
    lazy = level >= 4;

    s = (deflate_state *)clip__calloc(1, sizeof *s);
    m.head = (int32_t *)clip__malloc(sizeof(int32_t) * K_HASH_SIZE);
    m.prev = (int32_t *)clip__malloc(sizeof(int32_t) * K_WINDOW);
    syms = (sym *)clip__malloc(sizeof(sym) * (K_BLOCK_SYMBOLS + 2));
    if (!s || !m.head || !m.prev || !syms || !clip__buf_reserve(out, len / 2 + 64)) {
        free(s);
        free(m.head);
        free(m.prev);
        free(syms);
        return 0;
    }
    for (i = 0; i < K_HASH_SIZE; ++i) m.head[i] = -1;
    for (i = 0; i < K_WINDOW; ++i) m.prev[i] = -1;
    m.data = data;
    m.len = len;
    m.max_chain = kChain[level];
    m.nice_len = kNice[level];
    init_tables(s);
    s->data = data;
    s->bw.out = out;
    s->bw.ok = 1;

    clip__buf_push(out, 0x78);
    clip__buf_push(out, 0x9C);

#define FLUSH(raw_end, final)                                             \
    do {                                                                  \
        emit_block(s, syms, nsyms, block_start, (raw_end), (final));      \
        nsyms = 0;                                                        \
        block_start = (raw_end);                                          \
    } while (0)

    while (pos < len) {
        int dist, match_len = mc_longest(&m, pos, &dist);
        if (match_len && lazy && match_len < m.nice_len && pos + 1 < len) {
            int dist2, next;
            mc_insert(&m, pos);
            next = mc_longest(&m, pos + 1, &dist2);
            if (next > match_len) { /* defer: literal now, longer match next */
                syms[nsyms].value = data[pos];
                syms[nsyms++].dist = 0;
                ++pos;
                match_len = next;
                dist = dist2;
            } else {
                int k;
                syms[nsyms].value = (uint16_t)match_len;
                syms[nsyms++].dist = (uint16_t)dist;
                for (k = 1; k < match_len; ++k) mc_insert(&m, pos + (size_t)k);
                pos += (size_t)match_len;
                if (nsyms >= K_BLOCK_SYMBOLS) FLUSH(pos, 0);
                continue;
            }
        }
        if (match_len) {
            int k;
            syms[nsyms].value = (uint16_t)match_len;
            syms[nsyms++].dist = (uint16_t)dist;
            for (k = 0; k < match_len; ++k) mc_insert(&m, pos + (size_t)k);
            pos += (size_t)match_len;
        } else {
            mc_insert(&m, pos);
            syms[nsyms].value = data[pos];
            syms[nsyms++].dist = 0;
            ++pos;
        }
        if (nsyms >= K_BLOCK_SYMBOLS) FLUSH(pos, 0);
    }
    FLUSH(len, 1);
#undef FLUSH
    bw_align(&s->bw);
    ok = s->bw.ok && clip__buf_u32be(out, clip_adler32(data, len, 1));
    free(s);
    free(m.head);
    free(m.prev);
    free(syms);
    return ok;
}
