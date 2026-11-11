/* clipnew - create a .clip document from scratch.  Example program for
 * libclip.
 *
 *   clipnew <out.clip>
 *
 * Writes a 512 x 512 document with two raster layers over a white Paper
 * layer: a colour gradient and, above it, a dark disc in Multiply mode at
 * 75 % opacity.  The result opens in Clip Studio Paint. */
#include <stdio.h>
#include <string.h>

#include <libclip/clip.h>

#define SIZE 512

static void put(clip_image *img, int x, int y, int r, int g, int b, int a) {
    uint8_t *p = img->pixels + ((size_t)y * (size_t)img->width + (size_t)x) * 4;
    p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b; p[3] = (uint8_t)a;
}

int main(int argc, char **argv) {
    clip_image gradient = {0}, disc = {0}, preview = {0};
    clip_write_layer layers[2];
    clip_write_document doc;
    clip_error err;
    int x, y, ok;

    if (argc < 2) {
        fprintf(stderr, "usage: clipnew <out.clip>\n");
        return 2;
    }
    if (!clip_image_init(&gradient, SIZE, SIZE) || !clip_image_init(&disc, SIZE, SIZE) ||
        !clip_image_init(&preview, SIZE, SIZE)) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    for (y = 0; y < SIZE; ++y)
        for (x = 0; x < SIZE; ++x) {
            int dx = x - SIZE / 2, dy = y - SIZE / 2;
            int r = x / 2, g = y / 2, b = 255 - x / 2;
            int inside = dx * dx + dy * dy < (SIZE / 3) * (SIZE / 3);
            put(&gradient, x, y, r, g, b, 255);
            if (inside) put(&disc, x, y, 40, 20, 80, 255);
            /* Flattened image for the embedded preview.  libclip does not
             * composite; a real application passes its own rendering. */
            if (inside) {
                /* multiply at 75 %: c * (1 - 0.75) + c * d / 255 * 0.75 */
                r = (r * 64 + r * 40 / 255 * 192) / 256;
                g = (g * 64 + g * 20 / 255 * 192) / 256;
                b = (b * 64 + b * 80 / 255 * 192) / 256;
            }
            put(&preview, x, y, r, g, b, 255);
        }

    memset(layers, 0, sizeof layers);
    layers[0].name = "Gradient";
    layers[0].pixels = gradient;
    layers[0].visible = 1;
    layers[0].opacity = 256;
    layers[0].composite = CLIP_COMPOSITE_NORMAL;
    layers[1].name = "Disc";
    layers[1].pixels = disc;
    layers[1].visible = 1;
    layers[1].opacity = 192; /* 75 % */
    layers[1].composite = CLIP_COMPOSITE_MULTIPLY;

    memset(&doc, 0, sizeof doc);
    doc.width = SIZE;
    doc.height = SIZE;
    doc.dpi = 72;
    doc.paper_visible = 1;
    doc.paper_color[0] = doc.paper_color[1] = doc.paper_color[2] = 255;
    doc.layers = layers; /* bottom to top */
    doc.layer_count = 2;
    doc.active_layer = 1;
    doc.preview = &preview;

    ok = clip_write_path(&doc, argv[1], &err);
    if (!ok) fprintf(stderr, "%s: %s\n", argv[1], err.message);

    clip_image_free(&gradient);
    clip_image_free(&disc);
    clip_image_free(&preview);
    return ok ? 0 : 1;
}
