/* Reader for .clip documents: canvas properties, the layer tree and layer
 * pixels, via the SQLite database embedded in CHNKSQLi. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "clip_internal.h"
#include "sqlite3.h"

struct clip_document {
    clip_container container;
    uint8_t *db_image; /* SQLite deserialises from this; must outlive db */
    sqlite3 *db;
    clip_canvas_info canvas;
    clip_layer_info *layers;
    size_t layer_count;
};

/* ---- small statement helpers --------------------------------------------- */

static int col_index(sqlite3_stmt *s, const char *name) {
    int i, n = sqlite3_column_count(s);
    for (i = 0; i < n; ++i)
        if (strcmp(sqlite3_column_name(s, i), name) == 0) return i;
    return -1;
}

static int64_t col_i64(sqlite3_stmt *s, const char *name, int64_t def) {
    int c = col_index(s, name);
    return c < 0 ? def : sqlite3_column_int64(s, c);
}

static double col_f64(sqlite3_stmt *s, const char *name, double def) {
    int c = col_index(s, name);
    return c < 0 ? def : sqlite3_column_double(s, c);
}

static char *col_text(sqlite3_stmt *s, const char *name) {
    int c = col_index(s, name);
    const unsigned char *t = c < 0 ? NULL : sqlite3_column_text(s, c);
    return clip__strdup(t ? (const char *)t : "");
}

static const uint8_t *col_blob(sqlite3_stmt *s, const char *name, size_t *size) {
    int c = col_index(s, name);
    const void *p = c < 0 ? NULL : sqlite3_column_blob(s, c);
    *size = p ? (size_t)sqlite3_column_bytes(s, c) : 0;
    return (const uint8_t *)p;
}

static uint32_t rd_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Decodes the EffectEdge section of a LayerEffectInfo blob (docs/FORMAT.md
 * §4.4.1).  It is the first section in every observed blob; anything else
 * leaves the border off. */
static void parse_effect_info(const uint8_t *b, size_t n, clip_layer_effects *fx) {
    static const char name[] = "EffectEdge";
    const size_t len = sizeof name - 1, body = 8 + 4 + 2 * (sizeof name - 1);
    const uint8_t *e = b + body;
    union { double d; uint64_t u; } width;
    size_t k;
    if (!b || n < body + 24 || rd_be32(b) != n || rd_be32(b + 8) != len) return;
    for (k = 0; k < len; ++k)
        if (b[12 + 2 * k] != 0 || b[13 + 2 * k] != (uint8_t)name[k]) return;
    width.u = 0;
    for (k = 0; k < 8; ++k) width.u = width.u << 8 | e[4 + k];
    fx->border = rd_be32(e) != 0;
    fx->border_width = width.d;
    for (k = 0; k < 3; ++k) fx->border_color[k] = (uint8_t)rd_be32(e + 12 + 4 * k);
}

/* Prepares "SELECT * FROM <table> WHERE MainId=<id>" and steps to the row. */
static sqlite3_stmt *row_by_id(sqlite3 *db, const char *table, int64_t id) {
    char sql[128];
    sqlite3_stmt *s = NULL;
    snprintf(sql, sizeof sql, "SELECT * FROM %s WHERE MainId=%lld", table, (long long)id);
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return NULL;
    }
    return s;
}

/* ---- names --------------------------------------------------------------- */

const char *clip_layer_type_name(int32_t t) {
    if ((t & 0x630) == 0x630) return "Paper";
    if (t & 0x100) return "Folder";
    if (t == 1) return "Raster";
    return "Other";
}

int clip_layer_is_paper(const clip_layer_info *l) { return (l->type & 0x630) == 0x630; }
int clip_layer_is_folder(const clip_layer_info *l) { return l->folder || (l->type & 0x100) != 0; }
int clip_layer_mask_enabled(const clip_layer_info *l) { return l->mask_mipmap && (l->type & 0x2) && (l->masking & 0x1); }
int clip_layer_is_filter(const clip_layer_info *l) { return l->filter_info != NULL && l->filter_info_size >= 8; }

/* ---- open / close ---------------------------------------------------------- */

static void free_layer(clip_layer_info *l) {
    free(l->name);
    free(l->uuid);
    free(l->filter_info);
    free(l->gradation_info);
    l->name = l->uuid = NULL;
    l->filter_info = l->gradation_info = NULL;
    l->filter_info_size = l->gradation_info_size = 0;
}

void clip_document_close(clip_document *doc) {
    size_t i;
    if (!doc) return;
    if (doc->db) sqlite3_close(doc->db);
    free(doc->db_image);
    for (i = 0; i < doc->layer_count; ++i) free_layer(&doc->layers[i]);
    free(doc->layers);
    clip_container_free(&doc->container);
    free(doc);
}

static int load_db(clip_document *doc, clip_error *err) {
    size_t n;
    const uint8_t *img = clip_container_sqlite(&doc->container, &n);
    int rc;
    if (!img || n < 100 || memcmp(img, "SQLite format 3", 15) != 0) return clip__fail(err, "CHNKSQLi payload is not a SQLite database");
    /* sqlite3_deserialize wants a writable buffer even when read-only. */
    doc->db_image = (uint8_t *)clip__malloc(n);
    if (!doc->db_image) return clip__fail(err, "out of memory");
    memcpy(doc->db_image, img, n);
    if (sqlite3_open_v2(":memory:", &doc->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK)
        return clip__fail(err, "cannot open in-memory database");
    rc = sqlite3_deserialize(doc->db, "main", doc->db_image, (sqlite3_int64)n, (sqlite3_int64)n, SQLITE_DESERIALIZE_READONLY);
    if (rc != SQLITE_OK) return clip__fail(err, "sqlite3_deserialize failed: %s", sqlite3_errmsg(doc->db));
    return 1;
}

static int read_canvas(clip_document *doc, clip_error *err) {
    sqlite3_stmt *s = NULL;
    clip_canvas_info *c = &doc->canvas;
    if (sqlite3_prepare_v2(doc->db, "SELECT * FROM Canvas LIMIT 1", -1, &s, NULL) != SQLITE_OK)
        return clip__fail(err, "no Canvas table: %s", sqlite3_errmsg(doc->db));
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return clip__fail(err, "Canvas table is empty");
    }
    c->main_id = col_i64(s, "MainId", 0);
    c->unit_width = col_f64(s, "CanvasWidth", 0);
    c->unit_height = col_f64(s, "CanvasHeight", 0);
    c->resolution = col_f64(s, "CanvasResolution", 72);
    c->unit = (int32_t)col_i64(s, "CanvasUnit", 0);
    {
        /* Pages made from a print preset store their size in mm or inches.
         * CSP's pixel size is the rounded product with the resolution; this
         * matches the full-canvas offscreens of every sample file. */
        double per_inch;
        switch (c->unit) {
            case 1: per_inch = 2.54; break;
            case 2: per_inch = 25.4; break;
            case 3: per_inch = 1.0; break;
            case 4: per_inch = 72.0; break;
            default: per_inch = 0; break;
        }
        if (per_inch > 0 && c->resolution > 0) {
            c->width = floor(c->unit_width / per_inch * c->resolution + 0.5);
            c->height = floor(c->unit_height / per_inch * c->resolution + 0.5);
        } else {
            c->width = c->unit_width;
            c->height = c->unit_height;
        }
    }
    c->channel_bytes = (int32_t)col_i64(s, "CanvasChannelBytes", 1);
    c->default_channel_order = (int32_t)col_i64(s, "CanvasDefaultChannelOrder", 0);
    c->root_folder = col_i64(s, "CanvasRootFolder", 0);
    c->current_layer = col_i64(s, "CanvasCurrentLayer", 0);
    sqlite3_finalize(s);
    return 1;
}

/* CSP stores colour channels as 32 bits: a 16-bit value repeated twice
 * (0x983A983A is 0x983A / 65535 = 152 / 255; 0xFFFFFFFF is white).  Values
 * 0..255 are accepted as plain 8-bit channels, as older libclip wrote them. */
static uint8_t chan_value(int64_t v) {
    if (v < 0) return 255;
    if (v <= 255) return (uint8_t)v;
    return (uint8_t)((((uint64_t)v >> 16 & 0xFFFF) * 255 + 32767) / 65535);
}

static uint8_t chan(sqlite3_stmt *s, const char *name) { return chan_value(col_i64(s, name, 255)); }

static int cmp_layer_id(const void *a, const void *b) {
    int64_t x = ((const clip_layer_info *)a)->main_id, y = ((const clip_layer_info *)b)->main_id;
    return x < y ? -1 : x > y;
}

typedef struct layer_set {
    clip_layer_info *all; /* sorted by main_id, unique */
    size_t count;
} layer_set;

static const clip_layer_info *find_layer(const layer_set *set, int64_t id) {
    clip_layer_info key;
    key.main_id = id;
    return (const clip_layer_info *)bsearch(&key, set->all, set->count, sizeof key, cmp_layer_id);
}

static int emit_layer(clip_document *doc, const clip_layer_info *src, int depth, size_t *cap) {
    clip_layer_info *l;
    if (doc->layer_count == *cap) {
        size_t ncap = *cap ? *cap * 2 : 16;
        clip_layer_info *p = (clip_layer_info *)clip__realloc(doc->layers, ncap * sizeof *p);
        if (!p) return 0;
        doc->layers = p;
        *cap = ncap;
    }
    l = &doc->layers[doc->layer_count];
    *l = *src;
    l->depth = depth;
    l->name = clip__strdup(src->name);
    l->uuid = clip__strdup(src->uuid);
    l->filter_info = NULL;
    if (src->filter_info) {
        l->filter_info = (uint8_t *)clip__malloc(src->filter_info_size);
        if (l->filter_info) memcpy(l->filter_info, src->filter_info, src->filter_info_size);
        else l->filter_info_size = 0;
    }
    l->gradation_info = NULL;
    if (src->gradation_info) {
        l->gradation_info = (uint8_t *)clip__malloc(src->gradation_info_size);
        if (l->gradation_info) memcpy(l->gradation_info, src->gradation_info, src->gradation_info_size);
        else l->gradation_info_size = 0;
    }
    if (!l->name || !l->uuid) {
        free_layer(l);
        return 0;
    }
    doc->layer_count++;
    return 1;
}

/* In the file the first child is the BOTTOM-most layer and NextIndex links
 * to the sibling above it, so each sibling list is emitted in reverse. */
static int walk(clip_document *doc, const layer_set *set, int64_t id, int depth, size_t *cap) {
    const clip_layer_info **sib = NULL;
    size_t n = 0, sib_cap = 0, i;
    int guard = 0, ok = 1;
    if (depth > 512) return 1; /* corrupt tree */
    while (id != 0 && guard++ < 100000) {
        const clip_layer_info *l = find_layer(set, id);
        if (!l) break;
        if (n == sib_cap) {
            size_t ncap = sib_cap ? sib_cap * 2 : 16;
            const clip_layer_info **p = (const clip_layer_info **)clip__realloc((void *)sib, ncap * sizeof *p);
            if (!p) {
                free((void *)sib);
                return 0;
            }
            sib = p;
            sib_cap = ncap;
        }
        sib[n++] = l;
        id = l->next_index;
    }
    for (i = n; ok && i-- > 0;) {
        ok = emit_layer(doc, sib[i], depth, cap);
        if (ok && sib[i]->first_child_index) ok = walk(doc, set, sib[i]->first_child_index, depth + 1, cap);
    }
    free((void *)sib);
    return ok;
}

static int read_layers(clip_document *doc, clip_error *err) {
    sqlite3_stmt *s = NULL;
    layer_set set = {NULL, 0};
    size_t cap = 0, set_cap = 0, i, w;
    int ok = 1;
    const clip_layer_info *root;

    if (sqlite3_prepare_v2(doc->db, "SELECT * FROM Layer", -1, &s, NULL) != SQLITE_OK)
        return clip__fail(err, "no Layer table: %s", sqlite3_errmsg(doc->db));
    while (ok && sqlite3_step(s) == SQLITE_ROW) {
        clip_layer_info l;
        memset(&l, 0, sizeof l);
        l.main_id = col_i64(s, "MainId", 0);
        l.canvas_id = col_i64(s, "CanvasId", 0);
        l.name = col_text(s, "LayerName");
        l.type = (int32_t)col_i64(s, "LayerType", 0);
        /* Bit 0 is the layer's eye; bit 1 belongs to its mask (2 = hidden, masked). */
        l.visible = (col_i64(s, "LayerVisibility", 1) & 1) != 0;
        l.locked = col_i64(s, "LayerLock", 0) != 0;
        l.clip = col_i64(s, "LayerClip", 0) != 0;
        l.masking = (int32_t)col_i64(s, "LayerMasking", 0);
        l.offset_x = (int32_t)col_i64(s, "LayerOffsetX", 0);
        l.offset_y = (int32_t)col_i64(s, "LayerOffsetY", 0);
        l.render_offset_x = (int32_t)col_i64(s, "LayerRenderOffscrOffsetX", 0);
        l.render_offset_y = (int32_t)col_i64(s, "LayerRenderOffscrOffsetY", 0);
        l.opacity = (int32_t)col_i64(s, "LayerOpacity", 256);
        l.composite = (int32_t)col_i64(s, "LayerComposite", 0);
        l.folder = (int)col_i64(s, "LayerFolder", 0); /* 1 = folder; bit 0x10 = shown open */
        l.next_index = col_i64(s, "LayerNextIndex", 0);
        l.first_child_index = col_i64(s, "LayerFirstChildIndex", 0);
        l.render_mipmap = col_i64(s, "LayerRenderMipmap", 0);
        l.mask_mipmap = col_i64(s, "LayerLayerMaskMipmap", 0);
        l.render_thumbnail = col_i64(s, "LayerRenderThumbnail", 0);
        l.uuid = col_text(s, "LayerUuid");
        l.draw_color_enable = col_i64(s, "DrawColorEnable", 0) != 0;
        l.draw_color[0] = chan(s, "DrawColorMainRed");
        l.draw_color[1] = chan(s, "DrawColorMainGreen");
        l.draw_color[2] = chan(s, "DrawColorMainBlue");
        l.draw_color[3] = 255;
        l.effects.expression_color = (int32_t)col_i64(s, "LayerColorTypeIndex", 0);
        l.mono_black = col_i64(s, "LayerColorTypeBlackChecked", 1) != 0;
        l.mono_white = col_i64(s, "LayerColorTypeWhiteChecked", 1) != 0;
        {
            size_t gi_size;
            const uint8_t *gi = col_blob(s, "GradationFillInfo", &gi_size);
            if (gi && gi_size >= 8 && (l.gradation_info = (uint8_t *)clip__malloc(gi_size)) != NULL) {
                memcpy(l.gradation_info, gi, gi_size);
                l.gradation_info_size = gi_size;
            }
        }
        l.effects.layer_color = col_i64(s, "LayerUsePaletteColor", 0) != 0;
        l.effects.layer_color_rgb[0] = chan_value(col_i64(s, "LayerPaletteRed", 0));
        l.effects.layer_color_rgb[1] = chan_value(col_i64(s, "LayerPaletteGreen", 0));
        l.effects.layer_color_rgb[2] = chan_value(col_i64(s, "LayerPaletteBlue", 0));
        {
            size_t fx_size;
            const uint8_t *fx = col_blob(s, "LayerEffectInfo", &fx_size);
            parse_effect_info(fx, fx_size, &l.effects);
        }
        l.mask_offset_x = (int32_t)(col_i64(s, "LayerMaskOffsetX", 0) + col_i64(s, "LayerMaskOffscrOffsetX", 0));
        l.mask_offset_y = (int32_t)(col_i64(s, "LayerMaskOffsetY", 0) + col_i64(s, "LayerMaskOffscrOffsetY", 0));
        if (l.type & 0x1000) {
            size_t fi_size;
            const uint8_t *fi = col_blob(s, "FilterLayerInfo", &fi_size);
            if (fi && fi_size >= 8) {
                l.filter_info = (uint8_t *)clip__malloc(fi_size);
                if (l.filter_info) {
                    memcpy(l.filter_info, fi, fi_size);
                    l.filter_info_size = fi_size;
                }
            }
        }
        if (!l.name || !l.uuid) {
            free_layer(&l);
            ok = 0;
            break;
        }
        if (set.count == set_cap) {
            size_t ncap = set_cap ? set_cap * 2 : 16;
            clip_layer_info *p = (clip_layer_info *)clip__realloc(set.all, ncap * sizeof *p);
            if (!p) {
                free_layer(&l);
                ok = 0;
                break;
            }
            set.all = p;
            set_cap = ncap;
        }
        set.all[set.count++] = l;
    }
    sqlite3_finalize(s);

    /* Sort by id; a later row with the same MainId replaces an earlier one. */
    if (ok && set.count) {
        for (i = 0; i < set.count; ++i) set.all[i].depth = (int)i; /* remember row order for a stable sort */
        qsort(set.all, set.count, sizeof *set.all, cmp_layer_id);
        for (i = 0, w = 0; i < set.count; ++i) {
            if (w > 0 && set.all[w - 1].main_id == set.all[i].main_id) {
                clip_layer_info *keep = set.all[w - 1].depth > set.all[i].depth ? &set.all[w - 1] : &set.all[i];
                clip_layer_info *drop = keep == &set.all[i] ? &set.all[w - 1] : &set.all[i];
                clip_layer_info tmp = *keep;
                free_layer(drop);
                set.all[w - 1] = tmp;
            } else {
                set.all[w++] = set.all[i];
            }
        }
        set.count = w;
        for (i = 0; i < set.count; ++i) set.all[i].depth = 0;
    }

    if (ok) {
        root = doc->canvas.root_folder ? find_layer(&set, doc->canvas.root_folder) : NULL;
        if (root) {
            ok = walk(doc, &set, root->first_child_index, 0, &cap);
        } else {
            for (i = 0; ok && i < set.count; ++i) ok = emit_layer(doc, &set.all[i], 0, &cap);
        }
    }
    for (i = 0; i < set.count; ++i) free_layer(&set.all[i]);
    free(set.all);
    return ok ? 1 : clip__fail(err, "out of memory reading layers");
}

static int finish_open(clip_document *doc, clip_document **out, clip_error *err) {
    if (!load_db(doc, err) || !read_canvas(doc, err) || !read_layers(doc, err)) {
        clip_document_close(doc);
        return 0;
    }
    *out = doc;
    return 1;
}

int clip_document_open_memory(clip_document **out, const uint8_t *data, size_t size, clip_error *err) {
    clip_document *doc = (clip_document *)clip__calloc(1, sizeof *doc);
    *out = NULL;
    if (!doc) return clip__fail(err, "out of memory");
    if (!clip_container_parse(&doc->container, data, size, err)) {
        clip_document_close(doc);
        return 0;
    }
    return finish_open(doc, out, err);
}

int clip_document_open(clip_document **out, const char *path, clip_error *err) {
    clip_document *doc = (clip_document *)clip__calloc(1, sizeof *doc);
    *out = NULL;
    if (!doc) return clip__fail(err, "out of memory");
    if (!clip_container_read(&doc->container, path, err)) {
        clip_document_close(doc);
        return 0;
    }
    return finish_open(doc, out, err);
}

/* ---- accessors ------------------------------------------------------------- */

const clip_container *clip_document_container(const clip_document *doc) { return &doc->container; }
const clip_canvas_info *clip_document_canvas(const clip_document *doc) { return &doc->canvas; }
size_t clip_document_layer_count(const clip_document *doc) { return doc->layer_count; }
const clip_layer_info *clip_document_layer(const clip_document *doc, size_t i) {
    return i < doc->layer_count ? &doc->layers[i] : NULL;
}
struct sqlite3 *clip_document_db(const clip_document *doc) { return doc->db; }

/* ---- raster access ----------------------------------------------------------- */

static int offscreen_for_mipmap(const clip_document *doc, int64_t mipmap_id, int64_t *offscreen_id, clip_error *err) {
    sqlite3_stmt *s;
    int64_t info;
    if (!mipmap_id) return clip__fail(err, "layer has no mipmap");
    if (!(s = row_by_id(doc->db, "Mipmap", mipmap_id))) return clip__fail(err, "Mipmap row not found");
    info = col_i64(s, "BaseMipmapInfo", 0);
    sqlite3_finalize(s);
    if (!(s = row_by_id(doc->db, "MipmapInfo", info))) return clip__fail(err, "MipmapInfo row not found");
    *offscreen_id = col_i64(s, "Offscreen", 0);
    sqlite3_finalize(s);
    if (!*offscreen_id) return clip__fail(err, "MipmapInfo has no Offscreen");
    return 1;
}

static int render_offscreen_row(const clip_document *doc, int64_t offscreen_id, clip_image *out, clip_error *err) {
    sqlite3_stmt *s = row_by_id(doc->db, "Offscreen", offscreen_id);
    clip_offscreen_attribute attr;
    clip_offscreen_data data;
    const uint8_t *blob, *ref, *raw;
    size_t blob_size, ref_size, raw_size;
    char id[CLIP_EXTERNAL_ID_LEN + 1];
    int ok;
    if (!s) return clip__fail(err, "Offscreen row not found");
    memset(&attr, 0, sizeof attr);
    memset(&data, 0, sizeof data);
    blob = col_blob(s, "Attribute", &blob_size);
    if (!clip_offscreen_attribute_parse(blob, blob_size, &attr, err)) {
        sqlite3_finalize(s);
        return 0;
    }
    ref = col_blob(s, "BlockData", &ref_size);
    if (ref_size > CLIP_EXTERNAL_ID_LEN + 1 || (ref_size >= 8 && memcmp(ref, "extrnlid", 8) != 0)) {
        /* Legacy .lip: BlockData holds the tile stream itself. */
        ok = clip_offscreen_data_parse(ref, ref_size, &data, err) && clip_offscreen_decode(&attr, &data, out, NULL, err);
        sqlite3_finalize(s);
        clip_offscreen_data_free(&data);
        clip_offscreen_attribute_free(&attr);
        return ok;
    }
    /* .clip: BlockData holds the external chunk id as text. */
    while (ref_size && ref[ref_size - 1] == '\0') --ref_size;
    if (ref_size > CLIP_EXTERNAL_ID_LEN) ref_size = CLIP_EXTERNAL_ID_LEN;
    if (ref_size) memcpy(id, ref, ref_size);
    id[ref_size] = '\0';
    sqlite3_finalize(s);
    raw = clip_container_external(&doc->container, id, &raw_size);
    if (!raw || !raw_size) {
        clip_offscreen_attribute_free(&attr);
        return clip__fail(err, "external chunk '%s' not found", id);
    }
    ok = clip_offscreen_data_parse(raw, raw_size, &data, err) && clip_offscreen_decode(&attr, &data, out, NULL, err);
    clip_offscreen_data_free(&data);
    clip_offscreen_attribute_free(&attr);
    return ok;
}

int clip_document_render_layer(const clip_document *doc, const clip_layer_info *layer, clip_image *out, clip_error *err) {
    int64_t off;
    clip_image tile = {0, 0, NULL};
    clip_image_free(out);
    if (!offscreen_for_mipmap(doc, layer->render_mipmap, &off, err)) return 0;
    if (!render_offscreen_row(doc, off, &tile, err)) return 0;
    if (!clip_image_init(out, (int)doc->canvas.width, (int)doc->canvas.height)) {
        clip_image_free(&tile);
        return clip__fail(err, "out of memory");
    }
    clip__image_blit(out, &tile, layer->offset_x + layer->render_offset_x, layer->offset_y + layer->render_offset_y);
    clip_image_free(&tile);
    return 1;
}

int clip_document_render_layer_mask(const clip_document *doc, const clip_layer_info *layer, clip_image *out, clip_error *err) {
    int64_t off;
    clip_image tile = {0, 0, NULL};
    uint8_t outside[4] = {0, 0, 0, 0};
    clip_offscreen_attribute attr;
    clip_image_free(out);
    if (!offscreen_for_mipmap(doc, layer->mask_mipmap, &off, err)) return 0;
    if (!render_offscreen_row(doc, off, &tile, err)) return 0;
    /* Canvas-sized, placed like the layer pixels.  Outside the offscreen the
     * mask has its InitColor (usually "show"). */
    memset(&attr, 0, sizeof attr);
    {
        sqlite3_stmt *s = row_by_id(doc->db, "Offscreen", off);
        size_t n;
        const uint8_t *blob = s ? col_blob(s, "Attribute", &n) : NULL;
        if (blob && clip_offscreen_attribute_parse(blob, n, &attr, NULL)) clip_offscreen_init_color(&attr, outside);
        clip_offscreen_attribute_free(&attr);
        if (s) sqlite3_finalize(s);
    }
    if (!clip_image_init(out, (int)doc->canvas.width, (int)doc->canvas.height)) {
        clip_image_free(&tile);
        return clip__fail(err, "out of memory");
    }
    if (outside[3]) {
        size_t i, n = (size_t)out->width * out->height;
        for (i = 0; i < n; ++i) memcpy(out->pixels + i * 4, outside, 4);
    }
    clip__image_blit(out, &tile, layer->mask_offset_x, layer->mask_offset_y);
    clip_image_free(&tile);
    return 1;
}

int clip_document_layer_attribute(const clip_document *doc, const clip_layer_info *layer, clip_offscreen_attribute *out,
                                  clip_error *err) {
    int64_t off;
    sqlite3_stmt *s;
    const uint8_t *blob;
    size_t n;
    int ok;
    if (!offscreen_for_mipmap(doc, layer->render_mipmap, &off, err)) return 0;
    if (!(s = row_by_id(doc->db, "Offscreen", off))) return clip__fail(err, "Offscreen row not found");
    blob = col_blob(s, "Attribute", &n);
    ok = clip_offscreen_attribute_parse(blob, n, out, err);
    sqlite3_finalize(s);
    return ok;
}

int clip_document_canvas_preview(const clip_document *doc, clip_image *out, clip_error *err) {
    sqlite3_stmt *s = NULL;
    const uint8_t *data;
    size_t n;
    int w, h, x, y;
    clip_image_free(out);
    if (sqlite3_prepare_v2(doc->db, "SELECT * FROM CanvasPreview LIMIT 1", -1, &s, NULL) != SQLITE_OK || sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return clip__fail(err, "no CanvasPreview in file");
    }
    data = col_blob(s, "ImageData", &n);
    if (!data || !n) {
        sqlite3_finalize(s);
        return clip__fail(err, "CanvasPreview has no image data");
    }
    if (clip_png_decode(data, n, out, NULL)) { /* ImageType 1 = PNG (observed) */
        sqlite3_finalize(s);
        return 1;
    }
    /* Fall back to raw BGRA at ImageWidth x ImageHeight. */
    w = (int)col_i64(s, "ImageWidth", 0);
    h = (int)col_i64(s, "ImageHeight", 0);
    if (w <= 0 || h <= 0 || n < (size_t)w * h * 4 || !clip_image_init(out, w, h)) {
        sqlite3_finalize(s);
        return clip__fail(err, "CanvasPreview image data not recognised");
    }
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x) {
            const uint8_t *p = data + ((size_t)y * w + x) * 4;
            uint8_t *o = out->pixels + ((size_t)y * w + x) * 4;
            o[0] = p[2];
            o[1] = p[1];
            o[2] = p[0];
            o[3] = p[3];
        }
    sqlite3_finalize(s);
    return 1;
}

/* ---- SQL tooling ----------------------------------------------------------------- */

static char *quote_ident(const char *ident) {
    size_t n = strlen(ident), i, o = 0;
    char *s = (char *)clip__malloc(n * 2 + 3);
    if (!s) return NULL;
    s[o++] = '"';
    for (i = 0; i < n; ++i) {
        if (ident[i] == '"') s[o++] = '"';
        s[o++] = ident[i];
    }
    s[o++] = '"';
    s[o] = '\0';
    return s;
}

static char *cell_text(sqlite3_stmt *s, int i) {
    switch (sqlite3_column_type(s, i)) {
        case SQLITE_NULL: return clip__strdup("NULL");
        case SQLITE_BLOB: {
            int nb = sqlite3_column_bytes(s, i), k, printable = nb > 0 && nb <= 64;
            const uint8_t *p = (const uint8_t *)sqlite3_column_blob(s, i);
            char *v = (char *)clip__malloc(96 + (size_t)(nb > 0 ? nb : 0));
            size_t o;
            if (!v) return NULL;
            for (k = 0; k < nb && printable; ++k) printable = p[k] >= 32 && p[k] < 127;
            o = (size_t)snprintf(v, 32, "<blob %dB", nb);
            if (printable) {
                v[o++] = ' ';
                v[o++] = '\'';
                memcpy(v + o, p, (size_t)nb);
                o += (size_t)nb;
                v[o++] = '\'';
            }
            v[o++] = '>';
            v[o] = '\0';
            return v;
        }
        default: {
            const unsigned char *t = sqlite3_column_text(s, i);
            return clip__strdup(t ? (const char *)t : "");
        }
    }
}

int clip_document_query(const clip_document *doc, const char *sql, int limit, clip_row_callback cb, void *user, clip_error *err) {
    sqlite3_stmt *s = NULL;
    int n, rows = 0, i, stop = 0, rc;
    char **cells;
    if (sqlite3_prepare_v2(doc->db, sql, -1, &s, NULL) != SQLITE_OK) return clip__fail(err, "%s", sqlite3_errmsg(doc->db));
    n = sqlite3_column_count(s);
    cells = (char **)clip__calloc((size_t)(n > 0 ? n : 1), sizeof(char *));
    if (!cells) {
        sqlite3_finalize(s);
        return clip__fail(err, "out of memory");
    }
    for (i = 0; i < n; ++i) cells[i] = (char *)sqlite3_column_name(s, i);
    stop = cb(user, 1, n, (const char *const *)cells);
    while (!stop && (limit < 0 || rows < limit) && (rc = sqlite3_step(s)) == SQLITE_ROW) {
        int bad = 0;
        for (i = 0; i < n; ++i) bad |= (cells[i] = cell_text(s, i)) == NULL;
        if (!bad) stop = cb(user, 0, n, (const char *const *)cells);
        for (i = 0; i < n; ++i) free(cells[i]);
        if (bad) {
            free(cells);
            sqlite3_finalize(s);
            return clip__fail(err, "out of memory");
        }
        ++rows;
    }
    free(cells);
    sqlite3_finalize(s);
    return 1;
}

int clip_document_dump_table(const clip_document *doc, const char *table, int limit, clip_row_callback cb, void *user,
                             clip_error *err) {
    char *q = quote_ident(table), *sql;
    int ok;
    if (!q) return clip__fail(err, "out of memory");
    sql = (char *)clip__malloc(strlen(q) + 32);
    if (!sql) {
        free(q);
        return clip__fail(err, "out of memory");
    }
    sprintf(sql, "SELECT * FROM %s", q);
    free(q);
    ok = clip_document_query(doc, sql, limit, cb, user, err);
    free(sql);
    return ok;
}

int64_t clip_document_row_count(const clip_document *doc, const char *table) {
    char *q = quote_ident(table), *sql;
    sqlite3_stmt *s = NULL;
    int64_t n = -1;
    if (!q) return -1;
    sql = (char *)clip__malloc(strlen(q) + 32);
    if (sql) {
        sprintf(sql, "SELECT COUNT(*) FROM %s", q);
        if (sqlite3_prepare_v2(doc->db, sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    free(sql);
    free(q);
    return n;
}
