/* libclip unit tests.  usage: libclip_tests [reference.clip] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libclip/clip.h"
#include "sqlite3.h"

static int g_failures = 0;
static const char *g_reference = NULL;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

#define CHECK_EQ_U(a, b)                                                                                   \
    do {                                                                                                   \
        unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b);                   \
        if (va_ != vb_) {                                                                                  \
            printf("  FAIL %s:%d: %s == %s (%llu vs %llu)\n", __FILE__, __LINE__, #a, #b, va_, vb_);       \
            ++g_failures;                                                                                  \
        }                                                                                                  \
    } while (0)

static void px(clip_image *img, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t *p = img->pixels + ((size_t)y * img->width + x) * 4;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

static const uint8_t *at(const clip_image *img, int x, int y) { return img->pixels + ((size_t)y * img->width + x) * 4; }

static void test_zlib_and_png(void) {
    static const char *digits = "123456789";
    /* zlib stream of "hello hello hello hello" from CPython (fixed Huffman). */
    static const uint8_t fixed[] = {0x78, 0xda, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x27, 0x01, 0x68, 0x03, 0x08, 0xb1};
    clip_buffer z = {0}, back = {0}, png = {0};
    clip_image img, decoded = {0, 0, NULL};
    clip_error err;
    uint8_t *data = (uint8_t *)malloc(100000);
    int level, x, y;
    size_t i;
    CHECK_EQ_U(clip_crc32((const uint8_t *)digits, 9, 0), 0xCBF43926u);
    CHECK_EQ_U(clip_adler32((const uint8_t *)digits, 9, 1), 0x091E01DEu);
    CHECK(clip_zlib_inflate(fixed, sizeof fixed, &back, &err));
    CHECK(back.size == 23 && memcmp(back.data, "hello hello hello hello", 23) == 0);
    for (i = 0; i < 100000; ++i) data[i] = (uint8_t)((i / 37) % 7 + (i % 1000 == 0 ? i : 0));
    for (level = 0; level <= 9; level += 3) {
        CHECK(clip_zlib_deflate(data, 100000, level, &z));
        CHECK(clip_zlib_inflate(z.data, z.size, &back, &err));
        CHECK(back.size == 100000 && memcmp(back.data, data, 100000) == 0);
    }
    z.data[z.size - 1] ^= 1; /* corrupt the Adler-32 trailer */
    CHECK(!clip_zlib_inflate(z.data, z.size, &back, &err));

    CHECK(clip_image_init(&img, 37, 19));
    for (y = 0; y < 19; ++y)
        for (x = 0; x < 37; ++x) px(&img, x, y, (uint8_t)(x * 7), (uint8_t)(y * 13), (uint8_t)(x ^ y), (uint8_t)(x + y * 3));
    CHECK(clip_png_encode(&img, &png));
    CHECK(clip_png_decode(png.data, png.size, &decoded, &err));
    CHECK(decoded.width == 37 && decoded.height == 19 && memcmp(decoded.pixels, img.pixels, 37 * 19 * 4) == 0);
    CHECK(!clip_png_decode(png.data, 20, &decoded, &err));

    free(data);
    clip_buffer_free(&z);
    clip_buffer_free(&back);
    clip_buffer_free(&png);
    clip_image_free(&img);
    clip_image_free(&decoded);
}

static void test_container_roundtrip(void) {
    uint8_t db[4096], small[5] = {1, 2, 3, 4, 5}, *big = (uint8_t *)malloc(70000);
    clip_external_blob ext[2];
    clip_buffer file = {0};
    clip_container c;
    clip_error err;
    const uint8_t *p;
    size_t n;
    memset(&c, 0, sizeof c);
    memset(db, 0x5A, sizeof db);
    memcpy(db, "SQLite format 3", 16);
    memset(big, 7, 70000);
    clip_make_external_id(1, ext[0].id);
    ext[0].data = small;
    ext[0].size = sizeof small;
    clip_make_external_id(2, ext[1].id);
    ext[1].data = big;
    ext[1].size = 70000;
    CHECK_EQ_U(strlen(ext[0].id), 40);
    CHECK(strncmp(ext[0].id, "extrnlid", 8) == 0);
    CHECK(strcmp(ext[0].id, ext[1].id) != 0);

    CHECK(clip_container_build(db, sizeof db, ext, 2, &file, &err));
    CHECK(memcmp(file.data, "CSFCHUNK", 8) == 0);
    CHECK(clip_container_parse(&c, file.data, file.size, &err));
    CHECK_EQ_U(c.declared_file_size, file.size);
    CHECK_EQ_U(c.header_size, 24);
    CHECK_EQ_U(c.head_version, 0x100);
    CHECK(c.has_guid);
    CHECK_EQ_U(c.chunk_count, 5); /* Head, Exta, Exta, SQLi, Foot */
    CHECK(clip_container_find(&c, "Head") && clip_container_find(&c, "SQLi") && clip_container_find(&c, "Foot"));
    CHECK_EQ_U(c.head_sqlite_offset, clip_container_find(&c, "SQLi")->offset);
    p = clip_container_sqlite(&c, &n);
    CHECK(p && n == sizeof db && memcmp(p, db, n) == 0);
    p = clip_container_external(&c, ext[1].id, &n);
    CHECK(p && n == 70000 && memcmp(p, big, n) == 0);
    p = clip_container_external(&c, ext[0].id, &n);
    CHECK(p && n == 5 && memcmp(p, small, n) == 0);
    CHECK(clip_container_external(&c, "extrnlid00000000000000000000000000000000", &n) == NULL);
    /* First external chunk sits right after the header and Head chunk. */
    CHECK_EQ_U(c.chunks[1].offset, 80);

    /* A missing (empty) CHNKFoot is tolerated; a truncated payload is not. */
    CHECK(clip_container_parse(&c, file.data, file.size - 16, &err));
    CHECK(!clip_container_parse(&c, file.data, file.size - 100, &err));
    CHECK(!clip_container_parse(&c, (const uint8_t *)"not a clip file at all..", 24, &err));

    clip_container_free(&c);
    clip_buffer_free(&file);
    free(big);
}

static void test_offscreen_encode_decode(void) {
    clip_image img, back = {0, 0, NULL};
    clip_buffer attr_blob = {0}, blocks = {0};
    clip_offscreen_attribute attr;
    clip_offscreen_data data;
    clip_pixel_layout layout = CLIP_LAYOUT_UNKNOWN;
    clip_error err;
    int x, y, same = 1;
    memset(&attr, 0, sizeof attr);
    memset(&data, 0, sizeof data);
    CHECK(clip_image_init(&img, 300, 270));
    for (y = 0; y < img.height; ++y)
        for (x = 0; x < img.width; ++x)
            if ((x / 20 + y / 20) % 3 != 0) px(&img, x, y, (uint8_t)x, (uint8_t)y, (uint8_t)(x + y), (uint8_t)(255 - (x % 7)));
    CHECK(clip_offscreen_encode(&img, &attr_blob, &blocks, &err));
    CHECK(clip_offscreen_attribute_parse(attr_blob.data, attr_blob.size, &attr, &err));
    CHECK_EQ_U(attr.width, 300);
    CHECK_EQ_U(attr.height, 270);
    CHECK_EQ_U(attr.block_count_x, 2);
    CHECK_EQ_U(attr.block_count_y, 2);
    CHECK_EQ_U(attr.packing_count, 16);
    CHECK_EQ_U(attr.block_size_count, 4);
    CHECK(attr.note[0] == '\0');
    CHECK(clip_offscreen_data_parse(blocks.data, blocks.size, &data, &err));
    CHECK_EQ_U(data.block_count, 4);
    CHECK_EQ_U(data.block_status_count, 4);
    CHECK_EQ_U(data.block_checksum_count, 4);
    CHECK_EQ_U(data.checksum_mismatches, 0);
    CHECK(data.block_checksum[0] != 0);
    CHECK(data.note[0] == '\0');
    CHECK(clip_offscreen_decode(&attr, &data, &back, &layout, &err));
    CHECK(layout == CLIP_LAYOUT_BGRX_ALPHA);
    same = back.width == 300 && back.height == 270;
    for (y = 0; same && y < 270; ++y)
        for (x = 0; x < 300; ++x) {
            const uint8_t *a = at(&img, x, y), *b = at(&back, x, y);
            if (a[3] == 0 ? b[3] != 0 : memcmp(a, b, 4) != 0) {
                same = 0;
                break;
            }
        }
    CHECK(same);

    /* Empty offscreen: no stored blocks, all checksums zero. */
    clip_offscreen_data_free(&data);
    CHECK(clip_offscreen_encode_empty(600, 300, 1, &attr_blob, &blocks, &err));
    CHECK(clip_offscreen_attribute_parse(attr_blob.data, attr_blob.size, &attr, &err));
    CHECK_EQ_U(attr.init_do_initialize, 1);
    CHECK_EQ_U(attr.init_alpha_value, 0xFFFFFFFFu);
    CHECK_EQ_U(attr.init_image_value_count, 4);
    CHECK(clip_offscreen_data_parse(blocks.data, blocks.size, &data, &err));
    CHECK_EQ_U(data.block_count, 6);
    CHECK(!data.blocks[0].has_data && data.block_checksum[5] == 0);

    clip_offscreen_data_free(&data);
    clip_offscreen_attribute_free(&attr);
    clip_buffer_free(&attr_blob);
    clip_buffer_free(&blocks);
    clip_image_free(&img);
    clip_image_free(&back);
}

/* ---- query helpers ---- */
typedef struct rows {
    int count;      /* data rows */
    char first[2][128];
    int all_text;
} rows;

static int collect(void *user, int is_header, int n, const char *const *v) {
    rows *r = (rows *)user;
    if (is_header) return 0;
    if (r->count == 0) {
        int i;
        for (i = 0; i < n && i < 2; ++i) snprintf(r->first[i], sizeof r->first[i], "%s", v[i]);
    }
    if (n > 0 && strcmp(v[0], "text") != 0) r->all_text = 0;
    r->count++;
    return 0;
}

typedef struct offsets {
    const uint8_t *file;
    int count, bad;
} offsets;

static int check_offsets(void *user, int is_header, int n, const char *const *v) {
    offsets *o = (offsets *)user;
    if (is_header || n < 2) return 0;
    if (memcmp(o->file + strtoull(v[1], NULL, 10), "CHNKExta", 8) != 0) o->bad++;
    o->count++;
    return 0;
}

static void test_write_then_read(void) {
    static const char *ink_name = "Ink \xE3\x83\xAC\xE3\x82\xA4\xE3\x83\xA4\xE3\x83\xBC \xF0\x9F\x8E\xA8"; /* Japanese + emoji */
    clip_image bg, ink, hidden, preview, out = {0, 0, NULL};
    clip_write_layer layers[3];
    clip_write_document doc;
    clip_buffer file = {0};
    clip_document *cd = NULL;
    clip_error err;
    const clip_canvas_info *cv;
    const clip_layer_info *l;
    int x, y;
    char sql[160];
    rows r;
    offsets o;

    CHECK(clip_image_init(&bg, 320, 200));
    CHECK(clip_image_init(&ink, 320, 200));
    CHECK(clip_image_init(&hidden, 100, 50)); /* smaller than the canvas: placed at (0,0) */
    CHECK(clip_image_init(&preview, 320, 200));
    for (y = 0; y < 200; ++y)
        for (x = 0; x < 320; ++x) {
            px(&bg, x, y, 200, 220, 240, 255);
            px(&preview, x, y, 100, 120, 140, 255);
            if (x >= 10 && x < 110 && y >= 10 && y < 60) px(&ink, x, y, 10, 20, 30, 255);
        }
    px(&hidden, 5, 5, 1, 2, 3, 255);
    memset(layers, 0, sizeof layers);
    layers[0].name = "Background"; layers[0].pixels = bg; layers[0].visible = 1; layers[0].opacity = 256;
    layers[1].name = ink_name; layers[1].pixels = ink; layers[1].visible = 1; layers[1].opacity = 128;
    layers[1].composite = CLIP_COMPOSITE_MULTIPLY;
    layers[2].name = "Hidden"; layers[2].pixels = hidden; layers[2].visible = 0; layers[2].opacity = 256; layers[2].locked = 1;
    layers[2].clip = 1;
    memset(&doc, 0, sizeof doc);
    doc.width = 320;
    doc.height = 200;
    doc.dpi = 300;
    doc.paper_visible = 1;
    doc.paper_color[0] = doc.paper_color[1] = doc.paper_color[2] = 255;
    doc.layers = layers;
    doc.layer_count = 3;
    doc.active_layer = 1;
    doc.preview = &preview;

    CHECK(clip_write_memory(&doc, &file, &err));
    if (!file.size) {
        printf("  %s\n", err.message);
        return;
    }
    CHECK(clip_document_open_memory(&cd, file.data, file.size, &err));
    if (!cd) {
        printf("  %s\n", err.message);
        clip_buffer_free(&file);
        return;
    }
    cv = clip_document_canvas(cd);
    CHECK(cv->width == 320.0 && cv->height == 200.0 && cv->resolution == 300.0);
    /* Three raster layers plus the Paper fill layer at the bottom. */
    CHECK_EQ_U(clip_document_layer_count(cd), 4);
    CHECK(strcmp(clip_document_layer(cd, 0)->name, "Hidden") == 0);
    CHECK(strcmp(clip_document_layer(cd, 1)->name, ink_name) == 0);
    CHECK(strcmp(clip_document_layer(cd, 2)->name, "Background") == 0);
    CHECK(clip_layer_is_paper(clip_document_layer(cd, 3)));
    l = clip_document_layer(cd, 0);
    CHECK(!l->visible && l->locked && l->clip);
    l = clip_document_layer(cd, 1);
    CHECK_EQ_U(l->opacity, 128);
    CHECK_EQ_U(l->composite, CLIP_COMPOSITE_MULTIPLY);
    CHECK_EQ_U(strlen(l->uuid), 36);
    CHECK(cv->current_layer == l->main_id);
    l = clip_document_layer(cd, 3);
    CHECK(l->draw_color_enable && l->draw_color[0] == 255 && l->draw_color[2] == 255);

    CHECK(clip_document_render_layer(cd, clip_document_layer(cd, 1), &out, &err));
    CHECK(out.width == 320 && out.height == 200);
    CHECK(memcmp(at(&out, 50, 30), "\x0a\x14\x1e\xff", 4) == 0);
    CHECK(at(&out, 200, 150)[3] == 0);
    CHECK(clip_document_render_layer(cd, clip_document_layer(cd, 0), &out, &err));
    CHECK(at(&out, 5, 5)[3] == 255 && at(&out, 200, 100)[3] == 0);

    CHECK(clip_document_canvas_preview(cd, &out, &err));
    CHECK(out.width == 320 && out.height == 200 && memcmp(at(&out, 3, 3), "\x64\x78\x8c\xff", 4) == 0);

    /* ExternalChunk ids are TEXT, and CSP's lookup query finds each chunk. */
    memset(&r, 0, sizeof r);
    r.all_text = 1;
    CHECK(clip_document_query(cd, "SELECT typeof(ExternalID) FROM ExternalChunk", -1, collect, &r, &err));
    /* 3 layer tile streams + 5 thumbnails (3 layers, paper, root). */
    CHECK_EQ_U(r.count, 8);
    CHECK(r.all_text);
    snprintf(sql, sizeof sql, "SELECT Offset FROM ExternalChunk WHERE ExternalID='%s'", clip_document_container(cd)->chunks[1].external_id);
    memset(&r, 0, sizeof r);
    CHECK(clip_document_query(cd, sql, -1, collect, &r, &err));
    CHECK_EQ_U(r.count, 1);
    CHECK_EQ_U(strtoull(r.first[0], NULL, 10), clip_document_container(cd)->chunks[1].offset);
    memset(&o, 0, sizeof o);
    o.file = file.data;
    CHECK(clip_document_dump_table(cd, "ExternalChunk", 20, check_offsets, &o, &err));
    CHECK_EQ_U(o.count, 8);
    CHECK_EQ_U(o.bad, 0);
    CHECK_EQ_U(clip_document_row_count(cd, "Layer"), 5);
    CHECK(clip_document_row_count(cd, "NoSuchTable") == -1);

    /* No preview requested: no CanvasPreview row. */
    doc.preview = NULL;
    clip_document_close(cd);
    CHECK(clip_write_memory(&doc, &file, &err));
    CHECK(clip_document_open_memory(&cd, file.data, file.size, &err));
    CHECK(cd && clip_document_row_count(cd, "CanvasPreview") == 0);

    /* Deterministic output. */
    {
        clip_buffer again = {0};
        CHECK(clip_write_memory(&doc, &again, &err));
        CHECK(again.size == file.size && memcmp(again.data, file.data, file.size) == 0);
        clip_buffer_free(&again);
    }

    clip_document_close(cd);
    clip_buffer_free(&file);
    clip_image_free(&bg);
    clip_image_free(&ink);
    clip_image_free(&hidden);
    clip_image_free(&preview);
    clip_image_free(&out);
}

static void test_layer_effects(void) {
    clip_image ink, out = {0, 0, NULL};
    clip_write_layer layers[2];
    clip_write_document doc;
    clip_buffer file = {0};
    clip_document *cd = NULL;
    clip_error err;
    clip_offscreen_attribute attr;
    const clip_layer_info *l;
    int x, y;

    CHECK(clip_image_init(&ink, 300, 300));
    for (y = 20; y < 80; ++y)
        for (x = 20; x < 280; ++x) px(&ink, x, y, 200, 40, 10, 255);
    memset(layers, 0, sizeof layers);
    memset(&attr, 0, sizeof attr);
    layers[0].name = "Bordered"; layers[0].pixels = ink; layers[0].visible = 1; layers[0].opacity = 256;
    layers[0].effects.border = 1;
    layers[0].effects.border_width = 3.5;
    layers[0].effects.border_color[0] = 0x12; layers[0].effects.border_color[1] = 0x34; layers[0].effects.border_color[2] = 0x56;
    layers[0].effects.layer_color = 1;
    layers[0].effects.layer_color_rgb[0] = 40; layers[0].effects.layer_color_rgb[1] = 120; layers[0].effects.layer_color_rgb[2] = 230;
    layers[1].name = "Gray"; layers[1].pixels = ink; layers[1].visible = 1; layers[1].opacity = 256;
    layers[1].effects.expression_color = CLIP_EXPRESSION_GRAY;
    memset(&doc, 0, sizeof doc);
    doc.width = doc.height = 300;
    doc.dpi = 72;
    doc.layers = layers;
    doc.layer_count = 2;

    if (!clip_write_memory(&doc, &file, &err)) {
        printf("  write: %s\n", err.message);
        CHECK(0);
    }
    CHECK(clip_document_open_memory(&cd, file.data, file.size, &err));
    if (!cd) {
        printf("  %s\n", err.message);
        clip_image_free(&ink);
        clip_buffer_free(&file);
        return;
    }
    l = clip_document_layer(cd, 1); /* top-down: Gray, Bordered */
    CHECK(strcmp(l->name, "Bordered") == 0);
    CHECK(l->effects.border && l->effects.border_width == 3.5);
    CHECK(l->effects.border_color[0] == 0x12 && l->effects.border_color[1] == 0x34 && l->effects.border_color[2] == 0x56);
    CHECK(l->effects.layer_color && l->effects.layer_color_rgb[0] == 40 && l->effects.layer_color_rgb[2] == 230);
    CHECK_EQ_U(l->effects.expression_color, CLIP_EXPRESSION_COLOR);
    l = clip_document_layer(cd, 0);
    CHECK(strcmp(l->name, "Gray") == 0);
    CHECK(!l->effects.border && !l->effects.layer_color);
    CHECK_EQ_U(l->effects.expression_color, CLIP_EXPRESSION_GRAY);
    /* The gray layer is stored as alpha + gray, and decodes to its luma. */
    CHECK(clip_document_layer_attribute(cd, l, &attr, &err));
    CHECK_EQ_U(attr.packing[0], 17);
    CHECK(clip_document_render_layer(cd, l, &out, &err));
    CHECK(at(&out, 50, 50)[0] == 84 && at(&out, 50, 50)[1] == 84 && at(&out, 50, 50)[3] == 255); /* .299*200+.587*40+.114*10 */
    CHECK(at(&out, 150, 150)[3] == 0);
    CHECK_EQ_U(clip_document_row_count(cd, "Layer"), 3);
    {
        rows r;
        memset(&r, 0, sizeof r);
        CHECK(clip_document_query(cd, "SELECT length(LayerEffectInfo) FROM Layer WHERE LayerEffectInfo IS NOT NULL", -1, collect, &r, &err));
        CHECK_EQ_U(r.count, 1);
        CHECK(strcmp(r.first[0], "610") == 0);
    }

    /* Monochrome is not writable. */
    layers[1].effects.expression_color = CLIP_EXPRESSION_MONOCHROME;
    CHECK(!clip_write_memory(&doc, &file, &err));

    clip_offscreen_attribute_free(&attr);
    clip_document_close(cd);
    clip_buffer_free(&file);
    clip_image_free(&ink);
    clip_image_free(&out);
}

/* Legacy .lip files (CSP 1.x) are a bare SQLite database: no CSFCHUNK framing,
 * each Offscreen.BlockData holds its tile stream inline, and print-preset pages
 * store the canvas size in mm.  Build one from a libclip-written .clip. */
static void test_legacy_lip(void) {
    clip_image ink, out = {0, 0, NULL};
    clip_write_layer layer;
    clip_write_document doc;
    clip_buffer file = {0};
    clip_container c;
    clip_document *cd = NULL;
    clip_error err;
    sqlite3 *db = NULL;
    sqlite3_stmt *sel = NULL, *upd = NULL;
    unsigned char *image, *lip;
    const uint8_t *sq;
    size_t sq_size;
    sqlite3_int64 lip_size;
    size_t i;
    int inlined = 0;
    /* 25.4 mm x 12.7 mm at 300 dpi = 300 x 150 px. */
    const int w = 300, h = 150;

    memset(&c, 0, sizeof c);
    CHECK(clip_image_init(&ink, w, h));
    px(&ink, 3, 4, 10, 20, 30, 255);
    px(&ink, w - 1, h - 1, 200, 100, 50, 255);
    memset(&layer, 0, sizeof layer);
    layer.name = "Ink"; layer.pixels = ink; layer.visible = 1; layer.opacity = 256;
    memset(&doc, 0, sizeof doc);
    doc.width = w; doc.height = h; doc.dpi = 300;
    doc.layers = &layer; doc.layer_count = 1;
    CHECK(clip_write_memory(&doc, &file, &err));
    CHECK(clip_container_parse(&c, file.data, file.size, &err));
    sq = clip_container_sqlite(&c, &sq_size);
    CHECK(sq != NULL);
    if (!sq) goto done;

    /* Inline every external tile stream and switch the canvas to millimetres. */
    image = (unsigned char *)sqlite3_malloc64(sq_size);
    memcpy(image, sq, sq_size);
    CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    CHECK(sqlite3_deserialize(db, "main", image, (sqlite3_int64)sq_size, (sqlite3_int64)sq_size,
                              SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "DROP TRIGGER IF EXISTS ExternalUpdateOfBlockDataOnOffscreen;"
                           "UPDATE Canvas SET CanvasUnit=2, CanvasWidth=25.4, CanvasHeight=12.7;", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, "SELECT _PW_ID, BlockData FROM Offscreen", -1, &sel, NULL) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, "UPDATE Offscreen SET BlockData=? WHERE _PW_ID=?", -1, &upd, NULL) == SQLITE_OK);
    while (sel && upd && sqlite3_step(sel) == SQLITE_ROW) {
        char id[CLIP_EXTERNAL_ID_LEN + 1] = {0};
        int n = sqlite3_column_bytes(sel, 1);
        const uint8_t *data;
        size_t data_size;
        /* Updating while stepping can revisit rows that are already inlined. */
        if (n < 8 || memcmp(sqlite3_column_blob(sel, 1), "extrnlid", 8) != 0) continue;
        memcpy(id, sqlite3_column_blob(sel, 1), n > CLIP_EXTERNAL_ID_LEN ? CLIP_EXTERNAL_ID_LEN : (size_t)n);
        data = clip_container_external(&c, id, &data_size);
        /* Offscreens the writer left without data (no chunk) stay as they are. */
        if (!data) continue;
        ++inlined;
        sqlite3_bind_blob(upd, 1, data, (int)data_size, SQLITE_TRANSIENT);
        sqlite3_bind_int64(upd, 2, sqlite3_column_int64(sel, 0));
        CHECK(sqlite3_step(upd) == SQLITE_DONE);
        sqlite3_reset(upd);
    }
    sqlite3_finalize(sel);
    sqlite3_finalize(upd);
    CHECK(inlined > 0);
    lip =sqlite3_serialize(db, "main", &lip_size, 0);
    CHECK(lip != NULL && memcmp(lip, "SQLite format 3", 16) == 0);
    if (!lip) goto done;

    CHECK(clip_document_open_memory(&cd, lip, (size_t)lip_size, &err));
    sqlite3_free(lip);
    if (!cd) {
        printf("  %s\n", err.message);
        goto done;
    }
    CHECK(clip_document_container(cd)->bare_sqlite);
    CHECK(clip_document_canvas(cd)->unit == 2);
    CHECK(clip_document_canvas(cd)->width == w && clip_document_canvas(cd)->height == h);
    for (i = 0; i < clip_document_layer_count(cd); ++i) {
        const clip_layer_info *l = clip_document_layer(cd, i);
        if (strcmp(l->name, "Ink") != 0) continue;
        CHECK(clip_document_render_layer(cd, l, &out, &err));
        CHECK(out.width == w && out.height == h);
        if (out.pixels) {
            CHECK(memcmp(at(&out, 3, 4), at(&ink, 3, 4), 4) == 0);
            CHECK(memcmp(at(&out, w - 1, h - 1), at(&ink, w - 1, h - 1), 4) == 0);
        }
    }
    CHECK(out.pixels != NULL);
    clip_document_close(cd);
done:
    if (db) sqlite3_close(db);
    clip_container_free(&c);
    clip_buffer_free(&file);
    clip_image_free(&ink);
    clip_image_free(&out);
}

static void test_reference_document(void) {
    clip_document *cd = NULL;
    clip_error err;
    clip_image img = {0, 0, NULL};
    clip_offscreen_attribute attr;
    size_t i, raster = 0;
    FILE *f = g_reference ? fopen(g_reference, "rb") : NULL;
    if (!f) {
        printf("  (skipped: reference document not found)\n");
        return;
    }
    fclose(f);
    memset(&attr, 0, sizeof attr);
    CHECK(clip_document_open(&cd, g_reference, &err));
    if (!cd) {
        printf("  %s\n", err.message);
        return;
    }
    CHECK(clip_document_canvas(cd)->width > 0);
    for (i = 0; i < clip_document_layer_count(cd); ++i) {
        const clip_layer_info *l = clip_document_layer(cd, i);
        if (l->type != CLIP_LAYER_TYPE_RASTER) continue;
        ++raster;
        CHECK(clip_document_render_layer(cd, l, &img, &err));
        CHECK(clip_document_layer_attribute(cd, l, &attr, &err));
        CHECK(attr.note[0] == '\0');
    }
    CHECK(raster > 0);
    CHECK(clip_document_canvas_preview(cd, &img, &err));
    clip_offscreen_attribute_free(&attr);
    clip_image_free(&img);
    clip_document_close(cd);
}

typedef struct test_case {
    const char *name;
    void (*fn)(void);
} test_case;

int main(int argc, char **argv) {
    static const test_case tests[] = {
        {"zlib_and_png", test_zlib_and_png},
        {"container_roundtrip", test_container_roundtrip},
        {"offscreen_encode_decode", test_offscreen_encode_decode},
        {"write_then_read", test_write_then_read},
        {"layer_effects", test_layer_effects},
        {"legacy_lip", test_legacy_lip},
        {"reference_document", test_reference_document},
    };
    size_t i;
    g_reference = argc > 1 ? argv[1] : NULL;
    printf("libclip %s\n", clip_version());
    for (i = 0; i < sizeof tests / sizeof tests[0]; ++i) {
        int before = g_failures;
        printf("[ RUN  ] %s\n", tests[i].name);
        tests[i].fn();
        printf("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", tests[i].name);
    }
    printf("%d tests, %d failures\n", (int)(sizeof tests / sizeof tests[0]), g_failures);
    return g_failures == 0 ? 0 : 1;
}
