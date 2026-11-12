/* Writes a .clip file.
 *
 * The produced file follows the container layout and the table/column
 * vocabulary of documents written by Clip Studio Paint 5.1 (see
 * docs/FORMAT.md §3 and §6).  Clip Studio Paint 5.1 opens the
 * output.  Row order and values are kept stable on purpose: CSP compatibility
 * was established by bisecting exactly this output. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "clip_internal.h"
#include "csp_schema_data.h"
#include "sqlite3.h"

#define K_WHITE 4294967295ll /* CSP stores "white" colour channels as 0xFFFFFFFF */

/* ---- row values ---------------------------------------------------------------- */

enum { V_INT, V_REAL, V_TEXT, V_BLOB };

typedef struct col {
    const char *name;
    int kind;
    int64_t i;
    double d;
    const char *s;
    const uint8_t *b;
    size_t n;
} col;

static col ci(const char *name, int64_t v) {
    col c;
    memset(&c, 0, sizeof c);
    c.name = name;
    c.kind = V_INT;
    c.i = v;
    return c;
}
static col cr(const char *name, double v) {
    col c = ci(name, 0);
    c.kind = V_REAL;
    c.d = v;
    return c;
}
static col ct(const char *name, const char *v) {
    col c = ci(name, 0);
    c.kind = V_TEXT;
    c.s = v ? v : "";
    return c;
}
static col cb(const char *name, const uint8_t *p, size_t n) {
    col c = ci(name, 0);
    c.kind = V_BLOB;
    c.b = p;
    c.n = n;
    return c;
}

#define MAX_COLS 96

typedef struct row {
    col c[MAX_COLS];
    int n;
} row;

static void put(row *r, col c) {
    if (r->n < MAX_COLS) r->c[r->n++] = c;
}

/* ---- writer state ------------------------------------------------------------- */

typedef struct external {
    char id[CLIP_EXTERNAL_ID_LEN + 1];
    clip_buffer data;
} external;

typedef struct writer {
    sqlite3 *db;
    char err[256];
    external *ext;
    size_t ext_count, ext_cap;
    uint64_t next_offset; /* file offset of the next Exta chunk */
    int64_t canvas_id;
    int64_t next_layer, next_mipmap, next_mipmap_info, next_offscreen, next_thumb;
    int64_t next_extra; /* ids for a layer's second mipmap/thumbnail (its mask), above all layer ids */
    uint64_t seed;
    int canvas_w, canvas_h;
} writer;

static void set_err(writer *w, const char *msg) {
    if (!w->err[0]) snprintf(w->err, sizeof w->err, "%s", msg ? msg : "sql error");
}

static int exec_sql(writer *w, const char *sql) {
    char *msg = NULL;
    if (sqlite3_exec(w->db, sql, NULL, NULL, &msg) != SQLITE_OK) {
        set_err(w, msg);
        sqlite3_free(msg);
        return 0;
    }
    return 1;
}

static int insert(writer *w, const char *table, const row *r) {
    clip_buffer sql = {0};
    sqlite3_stmt *st = NULL;
    int i, ok;
    ok = clip__buf_append(&sql, "INSERT INTO \"", 13) && clip__buf_append(&sql, table, strlen(table)) && clip__buf_append(&sql, "\"(", 2);
    for (i = 0; ok && i < r->n; ++i)
        ok = clip__buf_append(&sql, i ? ",\"" : "\"", i ? 2 : 1) && clip__buf_append(&sql, r->c[i].name, strlen(r->c[i].name)) &&
             clip__buf_push(&sql, '"');
    ok = ok && clip__buf_append(&sql, ") VALUES(", 9);
    for (i = 0; ok && i < r->n; ++i) ok = clip__buf_append(&sql, i ? ",?" : "?", i ? 2 : 1);
    ok = ok && clip__buf_append(&sql, ")", 2); /* includes the terminating NUL */
    if (!ok) {
        clip_buffer_free(&sql);
        set_err(w, "out of memory");
        return 0;
    }
    if (sqlite3_prepare_v2(w->db, (const char *)sql.data, -1, &st, NULL) != SQLITE_OK) {
        set_err(w, sqlite3_errmsg(w->db));
        clip_buffer_free(&sql);
        return 0;
    }
    clip_buffer_free(&sql);
    for (i = 0; i < r->n; ++i) {
        const col *c = &r->c[i];
        switch (c->kind) {
            case V_INT: sqlite3_bind_int64(st, i + 1, c->i); break;
            case V_REAL: sqlite3_bind_double(st, i + 1, c->d); break;
            case V_TEXT: sqlite3_bind_text(st, i + 1, c->s, -1, SQLITE_TRANSIENT); break;
            case V_BLOB: sqlite3_bind_blob(st, i + 1, c->n ? (const void *)c->b : "", (int)c->n, SQLITE_TRANSIENT); break;
        }
    }
    ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) set_err(w, sqlite3_errmsg(w->db));
    sqlite3_finalize(st);
    return ok;
}

/* Takes ownership of `data`. */
static const char *add_external(writer *w, clip_buffer *data) {
    external *e;
    row r;
    if (w->ext_count == w->ext_cap) {
        size_t ncap = w->ext_cap ? w->ext_cap * 2 : 32;
        external *p = (external *)clip__realloc(w->ext, ncap * sizeof *p);
        if (!p) {
            set_err(w, "out of memory");
            clip_buffer_free(data);
            return NULL;
        }
        w->ext = p;
        w->ext_cap = ncap;
    }
    e = &w->ext[w->ext_count++];
    memset(e, 0, sizeof *e);
    clip_make_external_id(w->seed++, e->id);
    clip__buf_move(&e->data, data);
    /* ExternalID must be TEXT: CSP looks chunks up with
     * "SELECT Offset FROM ExternalChunk WHERE ExternalID='<id>'", and in SQLite
     * a BLOB never equals a text literal.  (Offscreen.BlockData is a BLOB.) */
    r.n = 0;
    put(&r, ct("ExternalID", e->id));
    put(&r, ci("Offset", (int64_t)w->next_offset));
    insert(w, "ExternalChunk", &r);
    w->next_offset += 16 + 8 + CLIP_EXTERNAL_ID_LEN + 8 + e->data.size;
    return e->id;
}

/* Offscreen row (+ external chunk when `store`).  Returns Offscreen.MainId. */
static int64_t add_offscreen(writer *w, int64_t layer_id, const clip_buffer *attr, clip_buffer *blocks, int store) {
    int64_t id = w->next_offscreen++;
    char local[CLIP_EXTERNAL_ID_LEN + 1];
    const char *ext_id;
    row r;
    if (store) {
        ext_id = add_external(w, blocks);
        if (!ext_id) return 0;
    } else {
        clip_make_external_id(w->seed++, local);
        ext_id = local;
    }
    r.n = 0;
    put(&r, ci("MainId", id));
    put(&r, ci("CanvasId", w->canvas_id));
    put(&r, ci("LayerId", layer_id));
    put(&r, cb("Attribute", attr->data, attr->size));
    put(&r, cb("BlockData", (const uint8_t *)ext_id, strlen(ext_id)));
    insert(w, "Offscreen", &r);
    return id;
}

/* Five-level mipmap chain (100, 50, 25, 12.5, 6.25 %).  Only the base level
 * carries pixels; CSP regenerates the rest.  `layout`: 0 RGBA, 1 gray, 2 mask.
 * Returns Mipmap.MainId (`id`). */
static int64_t add_mipmap(writer *w, int64_t id, int64_t layer_id, const clip_image *pixels, uint32_t width, uint32_t height,
                          int paper_fill, int layout) {
    /* CSP usually gives a layer's render Mipmap row the layer's own MainId;
     * a mask's chain gets an id of its own. */
    int64_t mipmap_id = id, info_ids[5];
    double scale = 100.0;
    int level;
    row r;
    if (w->next_mipmap < id + 1) w->next_mipmap = id + 1;
    for (level = 0; level < 5; ++level) info_ids[level] = w->next_mipmap_info++;
    for (level = 0; level < 5; ++level) {
        uint32_t lw = width >> level, lh = height >> level;
        clip_buffer attr = {0}, blocks = {0};
        clip_error e;
        int store = level == 0 && pixels != NULL, ok;
        int64_t off;
        if (lw < 1) lw = 1;
        if (lh < 1) lh = 1;
        /* A Gray layer stores every level as alpha + gray (observed); a mask as one plane. */
        ok = clip__offscreen_encode_ex(store ? pixels : NULL, store ? (uint32_t)pixels->width : lw, store ? (uint32_t)pixels->height : lh,
                                       paper_fill, layout, &attr, &blocks, &e);
        if (!ok) {
            set_err(w, e.message);
            return 0;
        }
        off = add_offscreen(w, layer_id, &attr, &blocks, store);
        clip_buffer_free(&attr);
        clip_buffer_free(&blocks);
        r.n = 0;
        put(&r, ci("MainId", info_ids[level]));
        put(&r, ci("CanvasId", w->canvas_id));
        put(&r, ci("LayerId", layer_id));
        put(&r, cr("ThisScale", scale));
        put(&r, ci("Offscreen", off));
        put(&r, ci("NextIndex", level < 4 ? info_ids[level + 1] : 0));
        insert(w, "MipmapInfo", &r);
        scale /= 2;
    }
    r.n = 0;
    put(&r, ci("MainId", mipmap_id));
    put(&r, ci("CanvasId", w->canvas_id));
    put(&r, ci("LayerId", layer_id));
    put(&r, ci("MipmapCount", 5));
    put(&r, ci("BaseMipmapInfo", info_ids[0]));
    insert(w, "Mipmap", &r);
    return mipmap_id;
}

static int64_t color_value(int paper, uint8_t c) { return paper ? (int64_t)((uint32_t)c * 0x01010101u) : 0; }

static int64_t add_thumbnail(writer *w, int64_t id, int64_t layer_id, const clip_image *pixels, int paper, const uint8_t *paper_rgb) {
    static const char *refresh[] = {"ThumbnailSmallerNeedRefresh",   "ThumbnailSmallNeedRefresh",    "ThumbnailMiddleNeedRefresh",
                                    "ThumbnailLargeNeedRefresh",     "ThumbnailLargerNeedRefresh",   "ThumbnailMiddle2xNeedRefresh",
                                    "ThumbnailLarger2xNeedRefresh",  "ThumbnailSmallerNeedRefresh1", "ThumbnailSmallNeedRefresh1",
                                    "ThumbnailMiddleNeedRefresh1",   "ThumbnailLargeNeedRefresh1",   "ThumbnailLargerNeedRefresh1",
                                    "ThumbnailMiddle2xNeedRefresh1", "ThumbnailLarger2xNeedRefresh1"};
    static const uint8_t black[3] = {0, 0, 0};
    /* Same ownership rule as Mipmap: usually LayerThumbnail.MainId == LayerId. */
    int64_t off;
    clip_buffer attr = {0}, blocks = {0};
    clip_error e;
    row r;
    size_t i;
    int ok;
    if (!paper_rgb) paper_rgb = black;
    if (w->next_thumb < id + 1) w->next_thumb = id + 1;
    if (pixels) {
        clip_image small = {0, 0, NULL};
        ok = clip__image_downscale(pixels, 512, 512, &small) && clip_offscreen_encode(&small, &attr, &blocks, &e);
        clip_image_free(&small);
    } else {
        ok = clip_offscreen_encode_empty(512, 512, 0, &attr, &blocks, &e);
    }
    if (!ok) {
        set_err(w, "cannot encode thumbnail");
        clip_buffer_free(&attr);
        clip_buffer_free(&blocks);
        return 0;
    }
    off = add_offscreen(w, layer_id, &attr, &blocks, 1);
    clip_buffer_free(&attr);
    clip_buffer_free(&blocks);

    r.n = 0;
    put(&r, ci("MainId", id));
    put(&r, ci("CanvasId", w->canvas_id));
    put(&r, ci("LayerId", layer_id));
    for (i = 0; i < sizeof refresh / sizeof refresh[0]; ++i) put(&r, ci(refresh[i], 1));
    put(&r, ci("ThumbnailDrewMode", 0));
    put(&r, ci("ThumbnailFixMode", paper ? 0 : -1));
    put(&r, ci("ThumbnailCanvasWidth", w->canvas_w));
    put(&r, ci("ThumbnailCanvasHeight", w->canvas_h));
    put(&r, ci("ThumbnailUseDrawColor", paper ? 1 : 0));
    put(&r, ci("ThumbnailMainColorRed", color_value(paper, paper_rgb[0])));
    put(&r, ci("ThumbnailMainColorGreen", color_value(paper, paper_rgb[1])));
    put(&r, ci("ThumbnailMainColorBlue", color_value(paper, paper_rgb[2])));
    put(&r, ci("ThumbnailSubColorRed", 0));
    put(&r, ci("ThumbnailSubColorGreen", 0));
    put(&r, ci("ThumbnailSubColorBlue", 0));
    put(&r, ci("ThumbnailOffscreen", off));
    put(&r, ci("ThumbnailColorTypeIndex", 0));
    put(&r, ci("ThumbnailColorTypeBlack", 1));
    put(&r, ci("ThumbnailColorTypeWhite", 1));
    put(&r, ci("ThumbnailPrewviewColorTypeIndex", 0));
    put(&r, ci("ThumbnailPrewviewColorTypeBlack", 1));
    put(&r, ci("ThumbnailPrewviewColorTypeWhite", 1));
    put(&r, ci("ThumbnailPrewviewColorTypeOpacity", 1));
    put(&r, ci("ThumbnailPrewviewColorTypeImage", 2147483648ll));
    put(&r, ci("ThumbnailPrewviewColorTypeAlpha", 2147483648ll));
    put(&r, ci("ThumbnailPrewviewMaskBinarize", 0));
    put(&r, ci("ThumbnailPrewviewMaskThreshold", 2147483648ll));
    put(&r, ci("ThumbnailDrewUseCanvasAspect0", 1));
    put(&r, ci("ThumbnailDrewUseCanvasAspect1", 1));
    insert(w, "LayerThumbnail", &r);
    return id;
}

/* CSP writes layer UUIDs as 32 lowercase hex digits grouped 10-4-4-4-10. */
static void layer_uuid(uint64_t seed, char out[37]) {
    char id[CLIP_EXTERNAL_ID_LEN + 1];
    const char *hex = id + 8;
    static const int groups[5] = {10, 4, 4, 4, 10};
    int g, k, o = 0, h = 0;
    clip_make_external_id(seed, id);
    for (g = 0; g < 5; ++g) {
        if (g) out[o++] = '-';
        for (k = 0; k < groups[g]; ++k) out[o++] = (char)tolower((unsigned char)hex[h++]);
    }
    out[o] = '\0';
}

typedef struct layer_fields {
    int64_t id;
    const char *name;
    int64_t type;
    int folder, visible, locked, clip;
    int64_t opacity256, composite, next, first_child, mipmap, thumb;
} layer_fields;

static void layer_common(writer *w, row *r, const layer_fields *f, char uuid[37]) {
    r->n = 0;
    put(r, ci("MainId", f->id));
    put(r, ci("CanvasId", w->canvas_id));
    put(r, ct("LayerName", f->name));
    put(r, ci("LayerType", f->type));
    put(r, ci("LayerLock", f->locked ? 1 : 0));
    put(r, ci("LayerClip", f->clip ? 1 : 0));
    put(r, ci("LayerMasking", f->folder ? 32 : 0));
    put(r, ci("LayerOffsetX", 0));
    put(r, ci("LayerOffsetY", 0));
    put(r, ci("LayerRenderOffscrOffsetX", 0));
    put(r, ci("LayerRenderOffscrOffsetY", 0));
    put(r, ci("LayerMaskOffsetX", 0));
    put(r, ci("LayerMaskOffsetY", 0));
    put(r, ci("LayerMaskOffscrOffsetX", 0));
    put(r, ci("LayerMaskOffscrOffsetY", 0));
    put(r, ci("LayerOpacity", f->opacity256));
    put(r, ci("LayerComposite", f->composite));
    put(r, ci("LayerUsePaletteColor", 0));
    put(r, ci("LayerNoticeablePaletteColor", 0));
    put(r, ci("LayerPaletteRed", 0));
    put(r, ci("LayerPaletteGreen", 0));
    put(r, ci("LayerPaletteBlue", 0));
    put(r, ci("LayerFolder", f->folder ? 1 : 0));
    put(r, ci("LayerVisibility", f->visible ? 1 : 0));
    put(r, ci("LayerSelect", 0));
    put(r, ci("LayerNextIndex", f->next));
    put(r, ci("LayerFirstChildIndex", f->first_child));
    layer_uuid(w->seed++, uuid);
    put(r, ct("LayerUuid", uuid));
    put(r, ci("LayerRenderMipmap", f->mipmap));
    put(r, ci("LayerLayerMaskMipmap", 0));
    put(r, ci("LayerRenderThumbnail", f->thumb));
    put(r, ci("LayerLayerMaskThumbnail", 0));
}

/* Replaces the value of an integer column already in the row. */
static void set_col(row *r, const char *name, int64_t v) {
    int k;
    for (k = 0; k < r->n; ++k)
        if (strcmp(r->c[k].name, name) == 0) r->c[k].i = v;
}

/* The mask as a canvas offscreen padded to whole blocks: outside the image it shows. */
static int padded_mask(const clip_image *mask, uint32_t pw, uint32_t ph, clip_image *out) {
    size_t i, n;
    if (!clip_image_init(out, (int)pw, (int)ph)) return 0;
    n = (size_t)pw * ph;
    for (i = 0; i < n; ++i) out->pixels[i * 4 + 3] = 255;
    clip__image_blit(out, mask, 0, 0);
    return 1;
}

/* MonochromeFillInfo blob of the Paper layer, as observed. */
static int monochrome_fill_info(clip_buffer *b) {
    return clip__buf_u32be(b, 62) && clip__buf_u32be(b, 1) && clip__buf_ustr(b, "MonochromeSetting") && clip__buf_u32be(b, 0) &&
           clip__buf_u32be(b, 0) && clip__buf_u32be(b, 0) && clip__buf_u32be(b, 1);
}

/* LayerEffectInfo (docs/FORMAT.md §4.4.1).  u32 total size, u32 version 2,
 * then named sections (u32 length in UTF-16 units, UTF-16BE name, payload) in
 * a fixed order: EffectEdge, EffectTone, EffectTextureMap, EffectApplyOpacity,
 * EffectToneAreaColor, EffectTonePosterize, EffectWaterEdge, EffectLine.
 * The template is the blob CSP 5.1 writes most often for a layer with a border
 * (16 layers in 9 files); only EffectEdge is patched.  Its 24-byte payload is
 * u32 enabled, f64 width in px, then u32 red/green/blue with the 8-bit value
 * replicated into all four bytes. */
static const uint8_t kEffectInfoTemplate[610] = {
    0x00, 0x00, 0x02, 0x62, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x45, 0x00, 0x66,
    0x00, 0x66, 0x00, 0x65, 0x00, 0x63, 0x00, 0x74, 0x00, 0x45, 0x00, 0x64, 0x00, 0x67, 0x00, 0x65,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x45, 0x00, 0x66,
    0x00, 0x66, 0x00, 0x65, 0x00, 0x63, 0x00, 0x74, 0x00, 0x54, 0x00, 0x6f, 0x00, 0x6e, 0x00, 0x65,
    0x00, 0x00, 0x00, 0x00, 0x40, 0x72, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x40, 0x4e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2d, 0x00, 0x00, 0x00, 0x1e,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
    0x00, 0x45, 0x00, 0x66, 0x00, 0x66, 0x00, 0x65, 0x00, 0x63, 0x00, 0x74, 0x00, 0x54, 0x00, 0x65,
    0x00, 0x78, 0x00, 0x74, 0x00, 0x75, 0x00, 0x72, 0x00, 0x65, 0x00, 0x4d, 0x00, 0x61, 0x00, 0x70,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e, 0x80, 0x80, 0x80, 0x80,
    0x00, 0x00, 0x00, 0x12, 0x00, 0x45, 0x00, 0x66, 0x00, 0x66, 0x00, 0x65, 0x00, 0x63, 0x00, 0x74,
    0x00, 0x41, 0x00, 0x70, 0x00, 0x70, 0x00, 0x6c, 0x00, 0x79, 0x00, 0x4f, 0x00, 0x70, 0x00, 0x61,
    0x00, 0x63, 0x00, 0x69, 0x00, 0x74, 0x00, 0x79, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x13, 0x00, 0x45, 0x00, 0x66, 0x00, 0x66, 0x00, 0x65, 0x00, 0x63, 0x00, 0x74,
    0x00, 0x54, 0x00, 0x6f, 0x00, 0x6e, 0x00, 0x65, 0x00, 0x41, 0x00, 0x72, 0x00, 0x65, 0x00, 0x61,
    0x00, 0x43, 0x00, 0x6f, 0x00, 0x6c, 0x00, 0x6f, 0x00, 0x72, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x6f, 0x00, 0x00, 0x00, 0xd7, 0x00, 0x00, 0x00, 0xf1, 0x00, 0x00, 0x00, 0x13, 0x00, 0x45,
    0x00, 0x66, 0x00, 0x66, 0x00, 0x65, 0x00, 0x63, 0x00, 0x74, 0x00, 0x54, 0x00, 0x6f, 0x00, 0x6e,
    0x00, 0x65, 0x00, 0x50, 0x00, 0x6f, 0x00, 0x73, 0x00, 0x74, 0x00, 0x65, 0x00, 0x72, 0x00, 0x69,
    0x00, 0x7a, 0x00, 0x65, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x78, 0x00, 0x00, 0x00, 0x1e,
    0x00, 0x00, 0x00, 0x0f, 0x00, 0x45, 0x00, 0x66, 0x00, 0x66, 0x00, 0x65, 0x00, 0x63, 0x00, 0x74,
    0x00, 0x57, 0x00, 0x61, 0x00, 0x74, 0x00, 0x65, 0x00, 0x72, 0x00, 0x45, 0x00, 0x64, 0x00, 0x67,
    0x00, 0x65, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x40, 0x14, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x40, 0x49, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x34, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x40, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x45,
    0x00, 0x66, 0x00, 0x66, 0x00, 0x65, 0x00, 0x63, 0x00, 0x74, 0x00, 0x4c, 0x00, 0x69, 0x00, 0x6e,
    0x00, 0x65, 0x00, 0x00, 0x00, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xf0, 0xf0,
    0xf0, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x40, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x00, 0x40, 0x00, 0x00, 0x00, 0x4b, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00,
    0x00, 0xc0, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0xc8,
};
#define EFFECT_EDGE_PAYLOAD 32 /* offset of the EffectEdge payload in the template */

static uint32_t rep8(uint8_t v) { return (uint32_t)v * 0x01010101u; }

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void layer_effect_info(const clip_layer_effects *fx, uint8_t out[sizeof kEffectInfoTemplate]) {
    uint8_t *e = out + EFFECT_EDGE_PAYLOAD;
    union { double d; uint64_t u; } width;
    int k;
    memcpy(out, kEffectInfoTemplate, sizeof kEffectInfoTemplate);
    width.d = fx->border_width;
    put_be32(e, fx->border ? 1 : 0);
    for (k = 0; k < 8; ++k) e[4 + k] = (uint8_t)(width.u >> (56 - 8 * k));
    for (k = 0; k < 3; ++k) put_be32(e + 12 + 4 * k, rep8(fx->border_color[k]));
}

/* Comic-story columns shared by the Canvas and Project rows. */
static void put_comic_story(row *r) {
    put(r, ct("ComicStoryStoryName", ""));
    put(r, ci("ComicStoryUseStoryIndex", 0));
    put(r, ci("ComicStoryStoryIndex", 1));
    put(r, ct("ComicStorySubTitle", ""));
    put(r, ci("ComicStoryStoryPosition", 1));
    put(r, ct("ComicStoryAuthorName", ""));
    put(r, ci("ComicStoryAuthorPosition", 2));
    put(r, ci("ComicStoryUsePageNumber", 0));
    put(r, ci("ComicStoryPageNumberStart", 1));
    put(r, ci("ComicStoryPageNumberPosition", 3));
    put(r, ci("ComicStoryNombreStart", 1));
    put(r, ct("ComicStoryNombreFont", "Tahoma"));
    put(r, cr("ComicStoryNombreFontSize", 9.0));
    put(r, ct("ComicStoryNombreFontPostScriptName", "Tahoma"));
    put(r, ci("ComicStoryUseShownNombre", 0));
    put(r, ci("ComicStoryNombrePosition", 5));
    put(r, ct("ComicStoryNombrePrefix", ""));
    put(r, ct("ComicStoryNombreSuffix", ""));
    put(r, ci("ComicStoryUseHiddenNombre", 0));
    put(r, ci("ComicStoryNombreColor", 0));
    put(r, ci("ComicStoryNombreUseEdge", 0));
    put(r, cr("ComicStoryNombreEdgeWidth", 0.5));
    put(r, ci("ComicStoryNombreEdgeUnit", 2));
    put(r, cr("ComicStoryYPageNombreOffset", 5.0));
    put(r, ci("ComicStoryYPageNombreOffsetUnit", 2));
    put(r, cr("ComicStoryXPageNombreOffset", 0.0));
    put(r, ci("ComicStoryXPageNombreOffsetUnit", 2));
}

static void write_schema(writer *w) {
    static const char *ext_tables[10] = {"Offscreen", "VectorObjectList", "Canvas3DModelBank", "Canvas3DModelLoader", "Track", "Track",
                                         "CanvasItemBinary", "Manager3DOd", "ModelData3D", "TimeLapseBlob"};
    static const char *ext_cols[10] = {"BlockData", "VectorData", "BankData", "ModelData", "TrackActionMixer", "TrackActionMixer2",
                                       "ItemBinaryData", "SceneData", "Layer3DModelData", "BlobData"};
    size_t i;
    row r;
    for (i = 0; i < CSP_SCHEMA_TABLE_COUNT; ++i)
        if (!exec_sql(w, csp_schema_tables[i].sql)) return;
    for (i = 0; i < CSP_SCHEMA_INDEX_COUNT; ++i)
        if (!exec_sql(w, csp_schema_indexes[i])) return;
    /* Garbage-collection triggers CSP keeps on every external-id column (observed). */
    if (!exec_sql(w, "CREATE TRIGGER IF NOT EXISTS ExternalUpdateOfBlockDataOnOffscreen UPDATE OF BlockData ON Offscreen BEGIN INSERT INTO RemovedExternal(ExternalID) VALUES(OLD.BlockData); END;") ||
        !exec_sql(w, "CREATE TRIGGER IF NOT EXISTS ExternalDeleteOfBlockDataOnOffscreen DELETE ON Offscreen BEGIN INSERT INTO RemovedExternal(ExternalID) VALUES(OLD.BlockData); END;"))
        return;
    for (i = 0; i < CSP_SCHEMA_PARAM_SCHEME_COUNT; ++i) {
        const csp_param_scheme_row *p = &csp_schema_param_scheme[i];
        r.n = 0;
        put(&r, ct("TableName", p->table));
        put(&r, ct("LabelName", p->label));
        put(&r, ci("DataType", p->data_type));
        put(&r, ci("Flag", p->flag));
        put(&r, ci("OwnerType", p->owner_type));
        put(&r, ci("LockType", p->lock_type));
        put(&r, ci("LockSpecified", p->lock_specified));
        put(&r, ct("LinkTable", p->link_table));
        insert(w, "ParamScheme", &r);
    }
    for (i = 0; i < 10; ++i) {
        r.n = 0;
        put(&r, ct("TableName", ext_tables[i]));
        put(&r, ct("ColumnName", ext_cols[i]));
        insert(w, "ExternalTableAndColumnName", &r);
    }
}

/* Canvas-sized copy of a layer image (or the image itself when it already is). */
static int canvas_image(const clip_image *src, int w, int h, clip_image *tmp, const clip_image **out) {
    if (src->width == w && src->height == h && src->pixels) {
        *out = src;
        return 1;
    }
    if (!clip_image_init(tmp, w, h)) return 0;
    if (src->pixels) clip__image_blit(tmp, src, 0, 0);
    *out = tmp;
    return 1;
}

static int build(writer *w, const clip_write_document *doc) {
    const int64_t root_id = w->next_layer++;
    const int64_t paper_id = doc->paper_visible ? w->next_layer++ : 0;
    const size_t n = doc->layer_count;
    const uint32_t pad_w = ((uint32_t)doc->width + 255) / 256 * 256, pad_h = ((uint32_t)doc->height + 255) / 256 * 256;
    int64_t *ids = (int64_t *)clip__malloc(sizeof(int64_t) * (n ? n : 1));
    int64_t *next = NULL, *child = NULL, top_first = 0;
    int64_t current_layer;
    char uuid[37];
    uint8_t effect_blob[sizeof kEffectInfoTemplate];
    row r;
    size_t i;

    if (!ids) {
        set_err(w, "out of memory");
        return 0;
    }
    for (i = 0; i < n; ++i) ids[i] = w->next_layer++;
    /* Tree links.  next[i]: the sibling above layer i (0 at the top of its
     * folder); child[i]: a folder's bottom-most child; top_first: the
     * bottom-most top-level layer. */
    next = (int64_t *)clip__calloc(n ? n : 1, sizeof(int64_t));
    child = (int64_t *)clip__calloc(n ? n : 1, sizeof(int64_t));
    if (!next || !child) {
        free(ids);
        free(next);
        free(child);
        set_err(w, "out of memory");
        return 0;
    }
    for (i = 0; i < n; ++i) {
        size_t j;
        for (j = i + 1; j < n && doc->layers[j].depth > doc->layers[i].depth; ++j) {}
        if (j < n && doc->layers[j].depth == doc->layers[i].depth) next[i] = ids[j];
        if (doc->layers[i].kind == CLIP_WRITE_FOLDER) {
            /* Its contents are the run below it deeper than it; the first child is the lowest of them at depth + 1. */
            size_t k = i;
            while (k > 0 && doc->layers[k - 1].depth > doc->layers[i].depth) --k;
            for (; k < i; ++k)
                if (doc->layers[k].depth == doc->layers[i].depth + 1) {
                    child[i] = ids[k];
                    break;
                }
        }
        if (!top_first && doc->layers[i].depth == 0) top_first = ids[i];
    }

    write_schema(w);
    /* CSP adds the effect columns to Layer only once a layer uses an effect
     * (70 of 639 surveyed files have them), always as this pair at the end. */
    for (i = 0; i < n; ++i)
        if (doc->layers[i].effects.border) {
            exec_sql(w, "ALTER TABLE Layer ADD COLUMN LayerEffectAttached INTEGER DEFAULT NULL");
            exec_sql(w, "ALTER TABLE Layer ADD COLUMN LayerEffectInfo BLOB DEFAULT NULL");
            break;
        }

    /* Correction and gradient layers carry their settings in columns CSP adds
     * to Layer on first use (as BLOB, like LayerEffectInfo). */
    for (i = 0; i < n; ++i)
        if (doc->layers[i].kind == CLIP_WRITE_FILTER) {
            exec_sql(w, "ALTER TABLE Layer ADD COLUMN FilterLayerInfo BLOB DEFAULT NULL");
            break;
        }
    for (i = 0; i < n; ++i)
        if (doc->layers[i].kind == CLIP_WRITE_GRADIENT) {
            exec_sql(w, "ALTER TABLE Layer ADD COLUMN GradationFillInfo BLOB DEFAULT NULL");
            break;
        }
    /* Mask mipmaps and thumbnails take ids above every layer's. */
    w->next_extra = w->next_layer;

    /* ---- layers ----
     * Bottom-up order in the file: the root's first child is the bottom-most
     * layer (the Paper, when present) and NextIndex walks upwards. */
    for (i = 0; i < n && !w->err[0]; ++i) {
        const clip_write_layer *l = &doc->layers[i];
        clip_image tmp = {0, 0, NULL}, padded = {0, 0, NULL};
        const clip_image *px = NULL;
        layer_fields f;
        int64_t mip, thumb, mask_mip, mask_thumb;
        const int masked = l->mask.pixels != NULL;
        /* Layer offscreens are padded to whole blocks, as CSP does. */
        if (!canvas_image(&l->pixels, doc->width, doc->height, &tmp, &px) || !clip_image_init(&padded, (int)pad_w, (int)pad_h)) {
            clip_image_free(&tmp);
            set_err(w, "out of memory");
            break;
        }
        clip__image_blit(&padded, px, 0, 0);
        mip = add_mipmap(w, ids[i], ids[i], l->kind == CLIP_WRITE_FOLDER ? NULL : &padded, pad_w, pad_h, 0,
                         l->kind == CLIP_WRITE_RASTER && l->effects.expression_color == CLIP_EXPRESSION_GRAY);
        thumb = add_thumbnail(w, ids[i], ids[i], l->kind == CLIP_WRITE_FOLDER ? NULL : px, 0, NULL);
        clip_image_free(&padded);
        clip_image_free(&tmp);
        mask_mip = mask_thumb = 0;
        if (l->mask.pixels && !w->err[0]) {
            clip_image pm = {0, 0, NULL};
            if (!padded_mask(&l->mask, pad_w, pad_h, &pm)) {
                set_err(w, "out of memory");
                break;
            }
            mask_mip = add_mipmap(w, w->next_extra++, ids[i], &pm, pad_w, pad_h, 0, 2);
            mask_thumb = add_thumbnail(w, w->next_extra++, ids[i], NULL, 0, NULL);
            clip_image_free(&pm);
        }
        memset(&f, 0, sizeof f);
        f.id = ids[i];
        f.name = l->name ? l->name : "";
        f.type = (l->kind == CLIP_WRITE_FILTER ? 0x1000
                  : l->kind == CLIP_WRITE_GRADIENT || l->kind == CLIP_WRITE_FOLDER ? 0
                                                                                   : CLIP_LAYER_TYPE_RASTER) |
                 (masked ? 2 : 0);
        f.visible = l->visible;
        f.locked = l->locked;
        f.clip = l->clip;
        f.opacity256 = l->opacity;
        f.composite = l->composite;
        f.next = next[i];
        f.first_child = child[i];
        f.folder = l->kind == CLIP_WRITE_FOLDER;
        f.mipmap = mip;
        f.thumb = thumb;
        layer_common(w, &r, &f, uuid);
        if (masked) {
            /* LayerMasking bit 0 enables the mask; LayerVisibility bit 1 is set on
             * masked layers (315 of 335 masked raster layers). */
            set_col(&r, "LayerMasking", (l->kind == CLIP_WRITE_FILTER || l->kind == CLIP_WRITE_FOLDER ? 32 : 0) | (l->mask_enabled ? 1 : 0));
            set_col(&r, "LayerVisibility", (l->visible ? 1 : 0) | 2);
            set_col(&r, "LayerLayerMaskMipmap", mask_mip);
            set_col(&r, "LayerLayerMaskThumbnail", mask_thumb);
        } else if (l->kind == CLIP_WRITE_FILTER) {
            set_col(&r, "LayerMasking", 32);
        }
        if (l->kind == CLIP_WRITE_FOLDER) {
            /* LayerFolder 17 (924 folders) is an open folder, 1 (999) a closed one. */
            set_col(&r, "LayerFolder", l->expanded ? 17 : 1);
            set_col(&r, "LayerSelect", i == doc->active_layer ? 1 : 0);
            insert(w, "Layer", &r);
            continue;
        }
        if (l->kind == CLIP_WRITE_FILTER) {
            /* As on all 116 surveyed correction layers. */
            put(&r, ci("SpecialRenderType", 13));
            put(&r, ci("SetRenderThumbnailInfoType", 20));
            put(&r, ci("DrawRenderThumbnailType", 20));
            put(&r, cb("FilterLayerInfo", l->filter_info, l->filter_info_size));
            if (clip__rd_u32be(l->filter_info) == CLIP_FILTER_GRADIENT_MAP) put(&r, ci("FilterLayerV132", 1));
            set_col(&r, "LayerSelect", i == doc->active_layer ? 1 : 0);
            insert(w, "Layer", &r);
            continue;
        }
        if (l->kind == CLIP_WRITE_GRADIENT) {
            /* As on all 380 surveyed gradient / fill layers. */
            clip_buffer gi = {0};
            if (!clip_gradation_encode(l->gradient, &gi)) {
                set_err(w, "cannot encode gradient");
                break;
            }
            put(&r, ci("DrawToRenderOffscreenType", 60));
            put(&r, ci("DrawToRenderMipmapType", 32));
            put(&r, ci("MoveOffsetAndExpandType", 30));
            put(&r, ci("FixOffsetAndExpandType", 30));
            put(&r, ci("RenderBoundForLayerMoveType", 30));
            put(&r, ci("SetRenderThumbnailInfoType", 30));
            put(&r, ci("DrawRenderThumbnailType", 30));
            put(&r, cb("GradationFillInfo", gi.data, gi.size));
            set_col(&r, "LayerSelect", i == doc->active_layer ? 1 : 0);
            insert(w, "Layer", &r);
            clip_buffer_free(&gi);
            continue;
        }
        {
            const clip_layer_effects *fx = &l->effects;
            int k;
            for (k = 0; k < r.n; ++k) {
                if (strcmp(r.c[k].name, "LayerSelect") == 0) r.c[k].i = i == doc->active_layer ? 1 : 0;
                /* "Layer color": CSP stores each channel replicated into 4 bytes. */
                else if (strcmp(r.c[k].name, "LayerUsePaletteColor") == 0) r.c[k].i = fx->layer_color ? 1 : 0;
                else if (strcmp(r.c[k].name, "LayerPaletteRed") == 0) r.c[k].i = fx->layer_color ? rep8(fx->layer_color_rgb[0]) : 0;
                else if (strcmp(r.c[k].name, "LayerPaletteGreen") == 0) r.c[k].i = fx->layer_color ? rep8(fx->layer_color_rgb[1]) : 0;
                else if (strcmp(r.c[k].name, "LayerPaletteBlue") == 0) r.c[k].i = fx->layer_color ? rep8(fx->layer_color_rgb[2]) : 0;
            }
            /* Expression color; "BlackChecked/WhiteChecked" are 1/1 for Color and Gray. */
            put(&r, ci("LayerColorTypeIndex", fx->expression_color == CLIP_EXPRESSION_GRAY ? 1 : 0));
            put(&r, ci("LayerColorTypeBlackChecked", 1));
            put(&r, ci("LayerColorTypeWhiteChecked", 1));
            /* Only written when a border is set, like CSP: a NULL column means "never touched". */
            if (fx->border) {
                layer_effect_info(fx, effect_blob);
                put(&r, ci("LayerEffectAttached", 1));
                put(&r, cb("LayerEffectInfo", effect_blob, sizeof effect_blob));
            }
        }
        insert(w, "Layer", &r);
    }
    if (paper_id && !w->err[0]) {
        const uint8_t *pc = doc->paper_color;
        int white = pc[0] == 255 && pc[1] == 255 && pc[2] == 255;
        clip_buffer mono = {0};
        layer_fields f;
        int64_t mip = add_mipmap(w, paper_id, paper_id, NULL, (uint32_t)doc->width, (uint32_t)doc->height, 1, 0);
        int64_t thumb = add_thumbnail(w, paper_id, paper_id, NULL, 1, pc);
        memset(&f, 0, sizeof f);
        f.id = paper_id;
        f.name = "Paper";
        f.type = CLIP_LAYER_TYPE_PAPER;
        f.visible = 1;
        f.opacity256 = 256;
        f.next = top_first;
        f.mipmap = mip;
        f.thumb = thumb;
        layer_common(w, &r, &f, uuid);
        /* 32-bit channels as CSP writes them (see chan_value in clip_document.c). */
        put(&r, ci("DrawColorMainRed", white ? K_WHITE : (int64_t)rep8(pc[0])));
        put(&r, ci("DrawColorMainGreen", white ? K_WHITE : (int64_t)rep8(pc[1])));
        put(&r, ci("DrawColorMainBlue", white ? K_WHITE : (int64_t)rep8(pc[2])));
        put(&r, ci("DrawColorEnable", 1));
        put(&r, ci("DrawToRenderOffscreenType", 61));
        put(&r, ci("SpecialRenderType", 20));
        put(&r, ci("DrawToRenderMipmapType", 31));
        put(&r, ci("MoveOffsetAndExpandType", 31));
        put(&r, ci("FixOffsetAndExpandType", 31));
        put(&r, ci("RenderBoundForLayerMoveType", 31));
        put(&r, ci("SetRenderThumbnailInfoType", 31));
        put(&r, ci("DrawRenderThumbnailType", 31));
        if (!monochrome_fill_info(&mono)) set_err(w, "out of memory");
        put(&r, cb("MonochromeFillInfo", mono.data, mono.size));
        insert(w, "Layer", &r);
        clip_buffer_free(&mono);
    }
    if (!w->err[0]) {
        layer_fields f;
        int64_t mip = add_mipmap(w, root_id, root_id, NULL, (uint32_t)doc->width, (uint32_t)doc->height, 0, 0);
        int64_t thumb = add_thumbnail(w, root_id, root_id, NULL, 0, NULL);
        memset(&f, 0, sizeof f);
        f.id = root_id;
        f.name = "";
        f.type = CLIP_LAYER_TYPE_FOLDER;
        f.folder = 1;
        f.visible = 1;
        f.opacity256 = 256;
        f.first_child = paper_id ? paper_id : top_first;
        f.mipmap = mip;
        f.thumb = thumb;
        layer_common(w, &r, &f, uuid);
        insert(w, "Layer", &r);
    }

    /* ---- canvas / project / banks ---- */
    current_layer = n ? ids[doc->active_layer < n ? doc->active_layer : n - 1] : (paper_id ? paper_id : root_id);
    free(ids);
    free(next);
    free(child);
    if (w->err[0]) return 0;

    r.n = 0;
    put(&r, ci("MainId", w->canvas_id));
    put(&r, ci("CanvasUnit", 0));
    put(&r, cr("CanvasWidth", doc->width));
    put(&r, cr("CanvasHeight", doc->height));
    put(&r, cr("CanvasResolution", doc->dpi));
    put(&r, ci("CanvasChannelBytes", 1));
    put(&r, ci("CanvasDefaultChannelOrder", 33));
    put(&r, ci("CanvasRootFolder", root_id));
    put(&r, ci("CanvasCurrentLayer", current_layer));
    put(&r, ci("CanvasDoSimulateColor", 0));
    put(&r, ci("CanvasDefaultColorTypeIndex", 0));
    put(&r, ci("CanvasDefaultColorBlackChecked", 1));
    put(&r, ci("CanvasDefaultColorWhiteChecked", 1));
    put(&r, cr("CanvasDefaultToneLine", 60.0));
    put(&r, ci("CanvasDoublePage", 0));
    put(&r, ci("CanvasRenderMipmapForceSaved", 0));
    put(&r, ci("Canvas3DModelDataLoaderIndex", 2));
    put_comic_story(&r);
    put(&r, ci("ComicBindPosition", 1));
    put(&r, ci("ComicHasComicCover", 0));
    put(&r, ci("ComicHasDoubleCover", 0));
    put(&r, ci("ComicPageIndex", 0));
    put(&r, ci("ComicIsLeftPage", 1));
    put(&r, ci("CropFrameShow", 0));
    put(&r, ci("SafeFrameShow", 0));
    put(&r, ci("ComicStoryLayerCreated", 0));
    put(&r, ci("ComicNombreLayerCreated", 0));
    insert(w, "Canvas", &r);

    r.n = 0;
    put(&r, ci("MainId", 1));
    put(&r, ci("ModelBankMainIndex", 0));
    put(&r, ci("BankRootItemMainIndex", 0));
    insert(w, "CanvasItemBank", &r);

    r.n = 0;
    put(&r, ci("MainId", 2));
    put(&r, ci("FirstTimeLine", 0));
    put(&r, ci("FirstScenario", 0));
    put(&r, ci("CurrentIndex", 0));
    put(&r, ci("Enable", 0));
    insert(w, "AnimationCutBank", &r);

    r.n = 0;
    put(&r, ct("ProjectInternalVersion", "1.1.0"));
    put(&r, ct("ProjectName", "3"));
    put(&r, ci("ProjectCanvas", w->canvas_id));
    put(&r, ci("ProjectItemBank", 1));
    put(&r, ci("ProjectCutBank", 2));
    put(&r, ci("ProjectRootCanvasNode", 0));
    put_comic_story(&r);
    put(&r, ci("DefaultPageUnit", 0));
    put(&r, cr("DefaultPageWidth", doc->width));
    put(&r, cr("DefaultPageHeight", doc->height));
    put(&r, cr("DefaultPageResolution", doc->dpi));
    put(&r, ci("DefaultPageChannelBytes", 1));
    put(&r, ci("DefaultPageChannelOrder", 33));
    put(&r, ci("DefaultPageColorType", 0));
    put(&r, ci("DefaultPageBlackChecked", 1));
    put(&r, ci("DefaultPageWhiteChecked", 1));
    put(&r, cr("DefaultPageToneLine", 60.0));
    put(&r, ci("DefaultPageUsePaper", doc->paper_visible ? 1 : 0));
    put(&r, ci("DefaultPagePaperRed", K_WHITE));
    put(&r, ci("DefaultPagePaperGreen", K_WHITE));
    put(&r, ci("DefaultPagePaperBlue", K_WHITE));
    put(&r, ci("DefaultPageUseTemplate", 0));
    put(&r, ct("DefaultPageTemplatePath", ""));
    put(&r, ct("DefaultPageTemplatePath2", ""));
    put(&r, ct("DefaultPageTemplateName", ""));
    put(&r, ci("DefaultPageIsUserTemplate", 0));
    put(&r, ci("DefaultPageDoublePage", 0));
    put(&r, ct("DefaultPagePresetID", ""));
    put(&r, ci("DefaultPageCheckBookBinding", 0));
    put(&r, ci("DefaultPageCelUseTemplate", 0));
    put(&r, ct("DefaultPageCelTemplatePath", ""));
    put(&r, ct("DefaultPageCelTemplatePath2", ""));
    put(&r, ct("DefaultPageCelTemplateName", ""));
    put(&r, ct("DefaultPageCelTemplateUUID", ""));
    put(&r, ci("DefaultPageCelIsUserTemplate", 0));
    put(&r, ci("DefaultPageUseCropFrame", 0));
    put(&r, ct("DefaultPageTemplateUUID", ""));
    put(&r, ci("DefaultPageUseDefaultCoverInfo", 0));
    put(&r, ct("DefaultPageSettingType", "Illust"));
    put(&r, ci("DefaultPageRecordTimeLapse", 0));
    insert(w, "Project", &r);

    if (doc->preview && doc->preview->pixels && doc->preview->width > 0 && doc->preview->height > 0) {
        const clip_image *full = doc->preview;
        clip_image small = {0, 0, NULL};
        clip_buffer png = {0};
        int pw = full->width, ph = full->height, m = doc->preview_max_size;
        if (m > 0 && (pw > m || ph > m)) {
            if (pw >= ph) {
                ph = ph * m / pw > 1 ? ph * m / pw : 1;
                pw = m;
            } else {
                pw = pw * m / ph > 1 ? pw * m / ph : 1;
                ph = m;
            }
            if (!clip__image_downscale(full, pw, ph, &small)) set_err(w, "out of memory");
            full = &small;
        }
        if (!w->err[0] && !clip_png_encode(full, &png)) set_err(w, "cannot encode preview");
        if (!w->err[0]) {
            r.n = 0;
            put(&r, ci("MainId", 1));
            put(&r, ci("CanvasId", w->canvas_id));
            put(&r, ci("ImageType", 1));
            put(&r, ci("ImageWidth", pw));
            put(&r, ci("ImageHeight", ph));
            put(&r, cb("ImageData", png.data, png.size));
            insert(w, "CanvasPreview", &r);
        }
        clip_buffer_free(&png);
        clip_image_free(&small);
    }

    /* ---- ElemScheme: CSP's per-table id high-water marks ---- */
    for (i = 0; i < CSP_SCHEMA_ELEM_SCHEME_COUNT; ++i) {
        const csp_elem_scheme_row *e = &csp_schema_elem_scheme[i];
        int64_t max_index = e->max_index;
        if (strcmp(e->table, "Offscreen") == 0) max_index = w->next_offscreen - 1;
        else if (strcmp(e->table, "MipmapInfo") == 0) max_index = w->next_mipmap_info - 1;
        else if (strcmp(e->table, "Mipmap") == 0) max_index = w->next_mipmap - 1;
        else if (strcmp(e->table, "LayerThumbnail") == 0) max_index = w->next_thumb - 1;
        else if (strcmp(e->table, "Layer") == 0) max_index = w->next_layer - 1;
        else if (strcmp(e->table, "Canvas") == 0) max_index = 1;
        r.n = 0;
        put(&r, ct("TableName", e->table));
        put(&r, ci("ElemType", e->elem_type));
        put(&r, ci("MaxIndex", max_index));
        insert(w, "ElemScheme", &r);
    }
    return !w->err[0];
}

int clip_write_memory(const clip_write_document *doc, clip_buffer *out, clip_error *err) {
    writer w;
    clip_external_blob *blobs = NULL;
    unsigned char *image;
    sqlite3_int64 image_size = 0;
    size_t i;
    int ok = 0;

    out->size = 0;
    if (!doc || doc->width <= 0 || doc->height <= 0) return clip__fail(err, "document has no size");
    if (doc->layer_count && !doc->layers) return clip__fail(err, "layer_count set without layers");
    for (i = 0; i < doc->layer_count; ++i) {
        const clip_layer_effects *fx = &doc->layers[i].effects;
        if (fx->expression_color != CLIP_EXPRESSION_COLOR && fx->expression_color != CLIP_EXPRESSION_GRAY)
            return clip__fail(err, "layer %u: only Color and Gray expression colors can be written", (unsigned)i);
        if (fx->border && !(fx->border_width > 0.0 && fx->border_width <= 1000.0))
            return clip__fail(err, "layer %u: border width out of range", (unsigned)i);
        {
            const clip_write_layer *l = &doc->layers[i];
            if (l->kind == CLIP_WRITE_FILTER && (!l->filter_info || l->filter_info_size < 8 ||
                                                 8 + (size_t)clip__rd_u32be(l->filter_info + 4) > l->filter_info_size))
                return clip__fail(err, "layer %u: correction layer without valid FilterLayerInfo", (unsigned)i);
            if (l->kind == CLIP_WRITE_GRADIENT && (!l->gradient || l->gradient->stop_count == 0 ||
                                                   l->gradient->stop_count > CLIP_GRADIENT_MAX_STOPS))
                return clip__fail(err, "layer %u: gradient layer without stops", (unsigned)i);
            if (l->kind < CLIP_WRITE_RASTER) return clip__fail(err, "layer %u: unknown layer kind %d", (unsigned)i, (int)l->kind);
            if (l->kind > CLIP_WRITE_FOLDER) return clip__fail(err, "layer %u: unknown layer kind %d", (unsigned)i, (int)l->kind);
            /* Nesting: a layer deeper than the one above it must be closed by a
             * folder exactly one level up; the top layer is not in a folder. */
            if (l->depth < 0 || (i + 1 == doc->layer_count && l->depth != 0))
                return clip__fail(err, "layer %u: bad folder depth %d", (unsigned)i, l->depth);
            if (i + 1 < doc->layer_count && doc->layers[i + 1].depth < l->depth &&
                (doc->layers[i + 1].kind != CLIP_WRITE_FOLDER || doc->layers[i + 1].depth != l->depth - 1))
                return clip__fail(err, "layer %u: not closed by its folder", (unsigned)i);
            if (l->mask.pixels && (l->mask.width != doc->width || l->mask.height != doc->height))
                return clip__fail(err, "layer %u: the mask must be canvas-sized", (unsigned)i);
        }
    }

    memset(&w, 0, sizeof w);
    w.next_offset = 24 + 16 + 40; /* header + CHNKHead chunk */
    w.canvas_id = 1;
    w.next_layer = w.next_mipmap = w.next_mipmap_info = w.next_offscreen = w.next_thumb = 2;
    w.seed = 0x5EED0000ull;
    w.canvas_w = doc->width;
    w.canvas_h = doc->height;
    if (sqlite3_open(":memory:", &w.db) != SQLITE_OK) {
        sqlite3_close(w.db);
        return clip__fail(err, "cannot create database");
    }

    if (!build(&w, doc)) {
        clip__fail(err, "%s", w.err[0] ? w.err : "write failed");
        goto done;
    }
    image = sqlite3_serialize(w.db, "main", &image_size, 0);
    if (!image) {
        clip__fail(err, "could not serialise database");
        goto done;
    }
    blobs = (clip_external_blob *)clip__calloc(w.ext_count ? w.ext_count : 1, sizeof *blobs);
    if (!blobs) {
        sqlite3_free(image);
        clip__fail(err, "out of memory");
        goto done;
    }
    for (i = 0; i < w.ext_count; ++i) {
        memcpy(blobs[i].id, w.ext[i].id, sizeof blobs[i].id);
        blobs[i].data = w.ext[i].data.data;
        blobs[i].size = w.ext[i].data.size;
    }
    ok = clip_container_build(image, (size_t)image_size, blobs, w.ext_count, out, err);
    sqlite3_free(image);

done:
    free(blobs);
    for (i = 0; i < w.ext_count; ++i) clip_buffer_free(&w.ext[i].data);
    free(w.ext);
    sqlite3_close(w.db);
    return ok;
}

int clip_write_path(const clip_write_document *doc, const char *path, clip_error *err) {
    clip_buffer b = {0};
    int ok = clip_write_memory(doc, &b, err) && clip_write_file(path, b.data, b.size, err);
    clip_buffer_free(&b);
    return ok;
}
