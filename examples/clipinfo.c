/* clipinfo - print the structure of a .clip file and optionally export its
 * layers as PNG.  Example program for libclip.
 *
 *   clipinfo <file.clip>                 canvas, layer tree, chunks
 *   clipinfo <file.clip> <out_dir>       ... and write preview.png plus
 *                                        layer_<n>.png for each raster layer
 *
 * Only the public header is used. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libclip/clip.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

/* libclip takes UTF-8 paths.  The `argv` passed to main() on Windows is in
 * the ANSI code page, which turns characters outside it into '?', so read
 * the UTF-16 command line and convert it instead. */
static char **utf8_argv(int *argc) {
    int i, n = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    char **out;
    if (!wargv) return NULL;
    out = (char **)calloc((size_t)n + 1, sizeof *out);
    for (i = 0; out && i < n; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        out[i] = (char *)malloc((size_t)len);
        if (out[i]) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, out[i], len, NULL, NULL);
    }
    LocalFree(wargv);
    *argc = n;
    return out;
}
#endif

static int save_png(const clip_image *img, const char *dir, const char *name) {
    clip_buffer png = {0};
    clip_error err;
    char path[4096];
    int ok;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    ok = clip_png_encode(img, &png) && clip_write_file(path, png.data, png.size, &err);
    if (ok)
        printf("  wrote %s\n", path);
    else
        fprintf(stderr, "  could not write %s\n", path);
    clip_buffer_free(&png);
    return ok;
}

int main(int argc, char **argv) {
    clip_document *doc;
    clip_error err;
    const clip_canvas_info *canvas;
    const clip_container *container;
    const char *out_dir;
    size_t i;

#ifdef _WIN32
    char **wide = utf8_argv(&argc);
    if (wide) argv = wide;
#endif
    if (argc < 2) {
        fprintf(stderr, "usage: clipinfo <file.clip> [out_dir]\n");
        return 2;
    }
    out_dir = argc > 2 ? argv[2] : NULL;

    if (!clip_document_open(&doc, argv[1], &err)) {
        fprintf(stderr, "%s: %s\n", argv[1], err.message);
        return 1;
    }

    canvas = clip_document_canvas(doc);
    printf("canvas  %.0f x %.0f px, %.0f dpi, %d byte(s) per channel\n", canvas->width, canvas->height,
           canvas->resolution, (int)canvas->channel_bytes);

    printf("layers  (top to bottom)\n");
    for (i = 0; i < clip_document_layer_count(doc); ++i) {
        const clip_layer_info *l = clip_document_layer(doc, i);
        printf("  %2u %*s%-24s %-8s %s opacity %3d%% blend %d%s\n", (unsigned)i, l->depth * 2, "", l->name,
               clip_layer_type_name(l->type), l->visible ? "visible" : "hidden ", l->opacity * 100 / 256,
               (int)l->composite, l->clip ? " clipped" : "");
    }

    container = clip_document_container(doc);
    printf("chunks\n");
    for (i = 0; i < container->chunk_count; ++i) {
        const clip_chunk *c = &container->chunks[i];
        printf("  %-4s at %10llu, %10llu bytes %s\n", c->tag, (unsigned long long)c->offset,
               (unsigned long long)c->size, c->external_id);
    }

    if (out_dir) {
        clip_image img = {0};
        if (clip_document_canvas_preview(doc, &img, &err)) save_png(&img, out_dir, "preview.png");
        for (i = 0; i < clip_document_layer_count(doc); ++i) {
            const clip_layer_info *l = clip_document_layer(doc, i);
            char name[64];
            if (clip_layer_is_folder(l) || clip_layer_is_paper(l)) continue;
            if (!clip_document_render_layer(doc, l, &img, &err)) {
                fprintf(stderr, "  layer %u: %s\n", (unsigned)i, err.message);
                continue;
            }
            snprintf(name, sizeof name, "layer_%u.png", (unsigned)i);
            save_png(&img, out_dir, name);
        }
        clip_image_free(&img);
    }

    clip_document_close(doc);
    return 0;
}
