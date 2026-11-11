/*
 * libclip - read and write CLIP STUDIO FORMAT (.clip) files.
 *
 * Plain C (C99), no dependencies besides the bundled SQLite amalgamation.
 * The byte layout handled here is documented in docs/FORMAT.md.
 *
 * Conventions
 *   - Functions returning `int` return nonzero on success and 0 on failure.
 *     When a `clip_error *` argument is given (it may be NULL), its message
 *     is filled on failure.
 *   - Paths are UTF-8 (converted to UTF-16 on Windows).
 *   - Images are straight-alpha RGBA, 8 bits per channel, rows of
 *     width * 4 bytes with no padding.
 *   - Anything the library allocates for the caller is released with the
 *     matching *_free / *_close function.  Output structs may be passed in
 *     zero-initialised or previously used; they are reset first.
 */
#ifndef LIBCLIP_CLIP_H
#define LIBCLIP_CLIP_H

#include <stddef.h>
#include <stdint.h>

#if defined(CLIP_SHARED)
#  if defined(_WIN32)
#    if defined(CLIP_BUILDING)
#      define CLIP_API __declspec(dllexport)
#    else
#      define CLIP_API __declspec(dllimport)
#    endif
#  else
#    define CLIP_API __attribute__((visibility("default")))
#  endif
#else
#  define CLIP_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CLIP_VERSION_MAJOR 1
#define CLIP_VERSION_MINOR 0
#define CLIP_VERSION_PATCH 0
CLIP_API const char *clip_version(void);

/* ------------------------------------------------------------------------ */
/* Basic types                                                               */
/* ------------------------------------------------------------------------ */

typedef struct clip_error {
    char message[256];
} clip_error;

/* Growable byte buffer. */
typedef struct clip_buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
} clip_buffer;
CLIP_API void clip_buffer_free(clip_buffer *b);

/* Straight-alpha RGBA8 image. */
typedef struct clip_image {
    int width;
    int height;
    uint8_t *pixels; /* width * height * 4 bytes */
} clip_image;
/* Allocate a zero-filled (transparent black) image. */
CLIP_API int clip_image_init(clip_image *img, int width, int height);
CLIP_API void clip_image_free(clip_image *img);

CLIP_API int clip_read_file(const char *path, clip_buffer *out, clip_error *err);
CLIP_API int clip_write_file(const char *path, const uint8_t *data, size_t size, clip_error *err);

/* ------------------------------------------------------------------------ */
/* zlib and PNG (used by the format; exposed for convenience)                */
/* ------------------------------------------------------------------------ */

/* CRC-32 (PNG) and Adler-32 (zlib).  Pass seed 0 / 1 for a fresh checksum,
 * or a previous result to continue it. */
CLIP_API uint32_t clip_crc32(const uint8_t *data, size_t len, uint32_t seed);
CLIP_API uint32_t clip_adler32(const uint8_t *data, size_t len, uint32_t seed);

/* Decode a zlib stream (RFC 1950: header, DEFLATE data, Adler-32 trailer)
 * into `out` (its contents are replaced). */
CLIP_API int clip_zlib_inflate(const uint8_t *data, size_t len, clip_buffer *out, clip_error *err);
/* Encode a zlib stream.  level 0 = stored blocks only, 1 = fastest .. 9 =
 * smallest; 6 is what libclip uses for tiles.  Output is deterministic. */
CLIP_API int clip_zlib_deflate(const uint8_t *data, size_t len, int level, clip_buffer *out);

/* PNG: 8-bit non-interlaced grey, grey+alpha, RGB, RGBA and palette images
 * are read; RGBA is written (filter 0, deflate level 6). */
CLIP_API int clip_png_decode(const uint8_t *data, size_t len, clip_image *out, clip_error *err);
CLIP_API int clip_png_encode(const clip_image *img, clip_buffer *out);

/* ------------------------------------------------------------------------ */
/* Container ("CSFCHUNK" framing)                                            */
/* ------------------------------------------------------------------------ */

#define CLIP_EXTERNAL_ID_LEN 40 /* "extrnlid" + 32 hex digits */

typedef struct clip_chunk {
    char tag[5];                   /* "Head", "Exta", "SQLi", "Foot" (NUL terminated) */
    uint64_t offset;               /* file offset of the "CHNK" tag */
    uint64_t size;                 /* payload size from the chunk header */
    uint64_t data_offset;          /* file offset of the payload */
    char external_id[CLIP_EXTERNAL_ID_LEN + 1]; /* Exta chunks only */
    uint64_t external_data_offset; /* Exta chunks: offset of the raw data */
    uint64_t external_data_size;
} clip_chunk;

typedef struct clip_container {
    uint8_t *file;                 /* the whole file image (owned) */
    size_t file_size;
    uint64_t declared_file_size;   /* first u64 after the magic */
    uint64_t header_size;          /* second u64 after the magic: 24 */
    uint64_t head_version;         /* CHNKHead: format version (0x100) */
    uint64_t head_sqlite_offset;   /* CHNKHead: file offset of the CHNKSQLi chunk */
    uint8_t document_guid[16];     /* CHNKHead */
    int has_guid;
    clip_chunk *chunks;
    size_t chunk_count;
} clip_container;

/* Parses a copy of `data`. */
CLIP_API int clip_container_parse(clip_container *out, const uint8_t *data, size_t size, clip_error *err);
CLIP_API int clip_container_read(clip_container *out, const char *path, clip_error *err);
CLIP_API void clip_container_free(clip_container *c);

/* `tag` is the 4 characters after "CHNK". */
CLIP_API const clip_chunk *clip_container_find(const clip_container *c, const char *tag);
CLIP_API const clip_chunk *clip_container_find_external(const clip_container *c, const char *id);
/* Pointers into c->file; NULL when absent. */
CLIP_API const uint8_t *clip_container_sqlite(const clip_container *c, size_t *size);
CLIP_API const uint8_t *clip_container_external(const clip_container *c, const char *id, size_t *size);

typedef struct clip_external_blob {
    char id[CLIP_EXTERNAL_ID_LEN + 1];
    const uint8_t *data;
    size_t size;
} clip_external_blob;

/* Builds a file image: header, CHNKHead, one CHNKExta per blob (in order),
 * CHNKSQLi, CHNKFoot.  The first Exta chunk is at offset 80. */
CLIP_API int clip_container_build(const uint8_t *sqlite_image, size_t sqlite_size, const clip_external_blob *externals,
                                  size_t external_count, clip_buffer *out, clip_error *err);

/* "extrnlid" + 32 uppercase hex digits derived deterministically from `seed`. */
CLIP_API void clip_make_external_id(uint64_t seed, char out[CLIP_EXTERNAL_ID_LEN + 1]);

/* ------------------------------------------------------------------------ */
/* Offscreens (tiled rasters)                                                */
/* ------------------------------------------------------------------------ */

typedef struct clip_offscreen_attribute {
    uint32_t header_size;               /* 16 */
    uint32_t parameter_section_size;
    uint32_t init_color_section_size;
    uint32_t block_size_section_size;
    /* "Parameter" */
    uint32_t width, height;
    uint32_t block_count_x, block_count_y;
    uint32_t packing[16];               /* BlockData* pixel packing values */
    size_t packing_count;
    /* "InitColor" */
    uint32_t init_unknown;
    uint32_t init_do_initialize;
    uint32_t init_alpha_value;
    uint32_t init_image_values[16];
    size_t init_image_value_count;
    uint32_t init_trailer;
    /* "BlockSize": byte size of every block in the external stream */
    uint32_t *block_sizes;
    size_t block_size_count;
    char note[160];                     /* set when the layout deviated from expectations */
} clip_offscreen_attribute;

CLIP_API int clip_offscreen_attribute_parse(const uint8_t *blob, size_t size, clip_offscreen_attribute *out, clip_error *err);
CLIP_API void clip_offscreen_attribute_free(clip_offscreen_attribute *a);
CLIP_API uint32_t clip_offscreen_block_width(const clip_offscreen_attribute *a);
CLIP_API uint32_t clip_offscreen_block_height(const clip_offscreen_attribute *a);
CLIP_API const char *clip_packing_name(size_t index);

typedef enum clip_pixel_layout {
    CLIP_LAYOUT_UNKNOWN = 0,
    CLIP_LAYOUT_ALPHA_ONLY,
    CLIP_LAYOUT_GRAY_ALPHA,
    CLIP_LAYOUT_BGRX_ALPHA,
    CLIP_LAYOUT_BGRA,
    CLIP_LAYOUT_CMYK_ALPHA
} clip_pixel_layout;
CLIP_API const char *clip_pixel_layout_name(clip_pixel_layout layout);

typedef struct clip_offscreen_block {
    uint32_t index;              /* row-major block index */
    uint32_t uncompressed_size;
    uint32_t width, height;
    int has_data;
    const uint8_t *compressed;   /* zlib stream; points into the parsed data */
    size_t compressed_size;
    uint32_t payload_adler32;    /* Adler-32 of LE length + zlib stream */
} clip_offscreen_block;

typedef struct clip_offscreen_data {
    clip_offscreen_block *blocks;
    size_t block_count;
    uint32_t *block_status;
    size_t block_status_count;
    uint32_t *block_checksum;
    size_t block_checksum_count;
    size_t checksum_mismatches;  /* stored blocks whose BlockCheckSum does not match */
    char note[160];
} clip_offscreen_data;

/* `data` must outlive `out` (blocks point into it). */
CLIP_API int clip_offscreen_data_parse(const uint8_t *data, size_t size, clip_offscreen_data *out, clip_error *err);
CLIP_API void clip_offscreen_data_free(clip_offscreen_data *d);

/* Decode all blocks into an image of attr->width x attr->height. */
CLIP_API int clip_offscreen_decode(const clip_offscreen_attribute *attr, const clip_offscreen_data *data, clip_image *out,
                                   clip_pixel_layout *layout_used, clip_error *err);

/* Encode an image as CSP-style tiles (alpha plane + BGRx plane).  Fills both
 * the Attribute blob and the external block stream. */
CLIP_API int clip_offscreen_encode(const clip_image *img, clip_buffer *attribute, clip_buffer *blocks, clip_error *err);
/* Attribute + stream for an empty offscreen.  `paper_fill` writes the
 * InitColor section the way CSP does for its Paper layer. */
CLIP_API int clip_offscreen_encode_empty(uint32_t width, uint32_t height, int paper_fill, clip_buffer *attribute,
                                         clip_buffer *blocks, clip_error *err);

/* ------------------------------------------------------------------------ */
/* Documents: reading                                                        */
/* ------------------------------------------------------------------------ */

/* Layer.LayerType bits observed in CSP 5.1 documents. */
#define CLIP_LAYER_TYPE_RASTER 0x001
#define CLIP_LAYER_TYPE_FOLDER 0x100
#define CLIP_LAYER_TYPE_PAPER  0x630
CLIP_API const char *clip_layer_type_name(int32_t type);

/* LayerComposite codes (blend modes) as used by libclip's reader and
 * writer.  Codes 4, 9 and 10 are the least certain of the mapping. */
typedef enum clip_composite {
    CLIP_COMPOSITE_NORMAL = 0,
    CLIP_COMPOSITE_DARKEN = 1,
    CLIP_COMPOSITE_MULTIPLY = 2,
    CLIP_COMPOSITE_COLOR_BURN = 3,
    CLIP_COMPOSITE_LINEAR_BURN = 4,
    CLIP_COMPOSITE_SUBTRACT = 5,
    CLIP_COMPOSITE_LIGHTEN = 6,
    CLIP_COMPOSITE_SCREEN = 7,
    CLIP_COMPOSITE_COLOR_DODGE = 8,
    CLIP_COMPOSITE_ADD = 9,
    CLIP_COMPOSITE_ADD_GLOW = 10,
    CLIP_COMPOSITE_OVERLAY = 11,
    CLIP_COMPOSITE_SOFT_LIGHT = 12,
    CLIP_COMPOSITE_HARD_LIGHT = 13,
    CLIP_COMPOSITE_DIFFERENCE = 14,
    CLIP_COMPOSITE_EXCLUSION = 15,
    CLIP_COMPOSITE_HUE = 16,
    CLIP_COMPOSITE_SATURATION = 17,
    CLIP_COMPOSITE_COLOR = 18,
    CLIP_COMPOSITE_LUMINOSITY = 19
} clip_composite;

/* Layer.LayerColorTypeIndex: the layer's "Expression color".  It also
 * decides how the layer's pixels are stored (BGRx, gray, or 1-bit). */
typedef enum clip_expression_color {
    CLIP_EXPRESSION_COLOR = 0,
    CLIP_EXPRESSION_GRAY = 1,
    CLIP_EXPRESSION_MONOCHROME = 2   /* read only; the writer rejects it */
} clip_expression_color;

/* Layer Property effects.  Zero-initialised means "no effects, color". */
typedef struct clip_layer_effects {
    int border;                  /* "Border effect": LayerEffectInfo / EffectEdge */
    double border_width;         /* pixels (CSP's UI may show mm; the file stores px) */
    uint8_t border_color[3];     /* RGB */
    int layer_color;             /* "Layer color": LayerUsePaletteColor */
    uint8_t layer_color_rgb[3];  /* LayerPaletteRed/Green/Blue */
    int32_t expression_color;    /* clip_expression_color */
} clip_layer_effects;

typedef struct clip_layer_info {
    int64_t main_id;             /* Layer.MainId (used by tree links) */
    int64_t canvas_id;
    char *name;                  /* UTF-8, never NULL */
    int32_t type;
    int visible, locked;
    int clip;                    /* clipping layer */
    int32_t masking;
    int32_t offset_x, offset_y;                 /* LayerOffsetX/Y */
    int32_t render_offset_x, render_offset_y;   /* LayerRenderOffscrOffsetX/Y */
    int32_t opacity;             /* 0..256 (256 == 100%) */
    int32_t composite;           /* LayerComposite, see clip_composite */
    int folder;                  /* LayerFolder */
    int64_t next_index;          /* sibling link, 0 = none */
    int64_t first_child_index;   /* child link, 0 = none */
    int64_t render_mipmap;
    int64_t mask_mipmap;
    int64_t render_thumbnail;
    char *uuid;                  /* never NULL */
    int depth;                   /* nesting depth computed from the tree */
    int draw_color_enable;       /* fill layers ("Paper") */
    uint8_t draw_color[4];       /* RGBA */
    clip_layer_effects effects;
} clip_layer_info;

CLIP_API int clip_layer_is_paper(const clip_layer_info *l);
CLIP_API int clip_layer_is_folder(const clip_layer_info *l);

typedef struct clip_canvas_info {
    int64_t main_id;
    double width, height;        /* pixels (unit 0) */
    double resolution;
    int32_t unit;
    int32_t channel_bytes;
    int32_t default_channel_order;
    int64_t root_folder;         /* Layer.MainId of the invisible root folder */
    int64_t current_layer;
} clip_canvas_info;

typedef struct clip_document clip_document;

CLIP_API int clip_document_open(clip_document **out, const char *path, clip_error *err);
CLIP_API int clip_document_open_memory(clip_document **out, const uint8_t *data, size_t size, clip_error *err);
CLIP_API void clip_document_close(clip_document *doc);

CLIP_API const clip_container *clip_document_container(const clip_document *doc);
CLIP_API const clip_canvas_info *clip_document_canvas(const clip_document *doc);
/* Layers in top-to-bottom palette order (folders followed by their contents).
 * In the file the first child is the BOTTOM-most layer; this list is
 * already reversed. */
CLIP_API size_t clip_document_layer_count(const clip_document *doc);
CLIP_API const clip_layer_info *clip_document_layer(const clip_document *doc, size_t index);

/* Full-resolution raster of a layer, positioned on a canvas-sized image. */
CLIP_API int clip_document_render_layer(const clip_document *doc, const clip_layer_info *layer, clip_image *out,
                                        clip_error *err);
/* The layer's mask offscreen (alpha in every channel), uncropped. */
CLIP_API int clip_document_render_layer_mask(const clip_document *doc, const clip_layer_info *layer, clip_image *out,
                                             clip_error *err);
/* Attribute blob of the layer's base offscreen. */
CLIP_API int clip_document_layer_attribute(const clip_document *doc, const clip_layer_info *layer,
                                           clip_offscreen_attribute *out, clip_error *err);
/* Embedded CanvasPreview thumbnail. */
CLIP_API int clip_document_canvas_preview(const clip_document *doc, clip_image *out, clip_error *err);

/* Read-only SQL access to the embedded database.  The callback receives the
 * column names first (is_header = 1), then each row as text (NULL values as
 * "NULL", blobs summarised as "<blob N B>").  Return nonzero from the
 * callback to stop early.  `limit` < 0 means no limit. */
typedef int (*clip_row_callback)(void *user, int is_header, int column_count, const char *const *values);
CLIP_API int clip_document_query(const clip_document *doc, const char *sql, int limit, clip_row_callback cb, void *user,
                                 clip_error *err);
CLIP_API int clip_document_dump_table(const clip_document *doc, const char *table, int limit, clip_row_callback cb,
                                      void *user, clip_error *err);
/* -1 when the table does not exist. */
CLIP_API int64_t clip_document_row_count(const clip_document *doc, const char *table);

/* The underlying sqlite3 handle (read-only).  Only use it with the same
 * SQLite build libclip links against. */
struct sqlite3;
CLIP_API struct sqlite3 *clip_document_db(const clip_document *doc);

/* ------------------------------------------------------------------------ */
/* Documents: writing                                                        */
/* ------------------------------------------------------------------------ */

typedef struct clip_write_layer {
    const char *name;            /* UTF-8; NULL is written as "" */
    clip_image pixels;           /* placed at (0, 0); cropped to the canvas */
    int visible, locked, clip;
    int32_t opacity;             /* 0..256 */
    int32_t composite;           /* clip_composite */
    /* Border, layer color and expression color.  A GRAY layer is stored as
     * gray (luma of `pixels`), as CSP does; COLOR and GRAY are writable. */
    clip_layer_effects effects;
} clip_write_layer;

typedef struct clip_write_document {
    int width, height;           /* canvas size in pixels */
    double dpi;
    int paper_visible;           /* writes CSP's "Paper" fill layer */
    uint8_t paper_color[3];      /* RGB */
    const clip_write_layer *layers;  /* bottom-to-top */
    size_t layer_count;
    size_t active_layer;         /* index into layers */
    const clip_image *preview;   /* flattened image for CanvasPreview; NULL = none */
    int preview_max_size;        /* 0 = store the preview at full size (as CSP does) */
} clip_write_document;

CLIP_API int clip_write_memory(const clip_write_document *doc, clip_buffer *out, clip_error *err);
CLIP_API int clip_write_path(const clip_write_document *doc, const char *path, clip_error *err);

#ifdef __cplusplus
}
#endif

#endif /* LIBCLIP_CLIP_H */
