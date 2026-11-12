/* Gradient and Fill layers: the GradationFillInfo blob and the stop table it
 * shares with gradient-map correction layers.  Layout in clip.h. */
#include <stdlib.h>
#include <string.h>

#include "clip_internal.h"

#define STOP_HEADER 16u
#define STOP_SIZE 28u
#define POS_ONE 32768.0

/* 32-bit colour channel: a 16-bit value repeated (0xFFFFFFFF is 255). */
static uint8_t chan8(uint32_t v) { return (uint8_t)(((v >> 16) * 255u + 32767u) / 65535u); }
static uint32_t chan32(uint8_t v) { return (uint32_t)v * 0x01010101u; }

static int cmp_stop(const void *a, const void *b) {
    double x = ((const clip_gradient_stop *)a)->position, y = ((const clip_gradient_stop *)b)->position;
    return x < y ? -1 : x > y;
}

int clip_gradient_stops_parse(const uint8_t *p, size_t n, clip_gradient *out, clip_error *err) {
    uint32_t header, stride, count, i;
    if (!p || n < STOP_HEADER) return clip__fail(err, "gradient stop table too short");
    header = clip__rd_u32be(p);
    stride = clip__rd_u32be(p + 4);
    count = clip__rd_u32be(p + 8);
    if (header < STOP_HEADER || stride < 24 || count == 0 || header + (size_t)stride * count > n)
        return clip__fail(err, "bad gradient stop table (header %u, stop size %u, count %u)", header, stride, count);
    if (count > CLIP_GRADIENT_MAX_STOPS) count = CLIP_GRADIENT_MAX_STOPS;
    for (i = 0; i < count; ++i) {
        const uint8_t *s = p + header + (size_t)stride * i;
        clip_gradient_stop *d = &out->stops[i];
        int k;
        double pos = clip__rd_u32be(s + 20) / POS_ONE;
        for (k = 0; k < 4; ++k) d->rgba[k] = chan8(clip__rd_u32be(s + 4 * k));
        d->position = pos < 0 ? 0 : pos > 1 ? 1 : pos;
    }
    out->stop_count = count;
    /* Stable enough for stops at equal positions: CSP keeps them in order. */
    for (i = 1; i < count; ++i)
        if (out->stops[i].position < out->stops[i - 1].position) {
            qsort(out->stops, count, sizeof out->stops[0], cmp_stop);
            break;
        }
    return 1;
}

static double rd_f64(const uint8_t *p) {
    union {
        uint64_t u;
        double d;
    } v;
    v.u = clip__rd_u64be(p);
    return v.d;
}

static int buf_f64(clip_buffer *b, double d) {
    union {
        uint64_t u;
        double d;
    } v;
    v.d = d;
    return clip__buf_u64be(b, v.u);
}

/* Reads a section name; returns 1 and advances when it equals `ascii`. */
static int name_is(const uint8_t *p, size_t n, size_t *o, const char *ascii) {
    size_t len = strlen(ascii), k;
    uint32_t units;
    if (*o + 4 > n) return 0;
    units = clip__rd_u32be(p + *o);
    if (units != len || *o + 4 + 2 * (size_t)units > n) return 0;
    for (k = 0; k < len; ++k)
        if (p[*o + 4 + 2 * k] != 0 || p[*o + 5 + 2 * k] != (uint8_t)ascii[k]) return 0;
    *o += 4 + 2 * (size_t)units;
    return 1;
}

int clip_gradation_parse(const uint8_t *b, size_t n, clip_gradient *out, clip_error *err) {
    size_t o = 8;
    int have_stops = 0;
    memset(out, 0, sizeof *out);
    out->antialias = 1;
    out->ellipse[0] = out->ellipse[1] = 100.0;
    if (!b || n < 8) return clip__fail(err, "GradationFillInfo too short");
    if (clip__rd_u32be(b) > n) return clip__fail(err, "GradationFillInfo truncated");
    n = clip__rd_u32be(b);
    while (o + 4 <= n) {
        if (name_is(b, n, &o, "GradationData")) {
            uint32_t size;
            if (o + 4 > n) break;
            size = clip__rd_u32be(b + o);
            if (o + 4 + (size_t)size > n || !clip_gradient_stops_parse(b + o + 4, size, out, err)) return 0;
            have_stops = 1;
            o += 4 + size;
        } else if (name_is(b, n, &o, "GradationSetting")) {
            if (o + 68 > n) return clip__fail(err, "GradationSetting truncated");
            out->shape = (int32_t)clip__rd_u32be(b + o);
            out->repeat = (int32_t)clip__rd_u32be(b + o + 4);
            out->antialias = (int32_t)clip__rd_u32be(b + o + 8);
            out->ellipse[0] = rd_f64(b + o + 12);
            out->ellipse[1] = rd_f64(b + o + 20);
            out->ellipse[2] = rd_f64(b + o + 28);
            out->x0 = rd_f64(b + o + 36);
            out->y0 = rd_f64(b + o + 44);
            out->x1 = rd_f64(b + o + 52);
            out->y1 = rd_f64(b + o + 60);
            o += 68;
        } else {
            /* Any other section (GradationSettingAdd0001, ...) has a size word. */
            int add = name_is(b, n, &o, "GradationSettingAdd0001");
            uint32_t size, units;
            if (!add) {
                units = clip__rd_u32be(b + o);
                if (o + 4 + 2 * (size_t)units > n) break;
                o += 4 + 2 * (size_t)units;
            }
            if (o + 4 > n) break;
            size = clip__rd_u32be(b + o);
            if (o + 4 + (size_t)size > n) break;
            if (add && size >= 16) {
                int k;
                out->fill = clip__rd_u32be(b + o + 4) != 0;
                for (k = 0; k < 3; ++k) out->fill_rgb[k] = chan8(clip__rd_u32be(b + o + 8 + 4 * k));
            }
            o += 4 + size;
        }
    }
    if (!have_stops) return clip__fail(err, "GradationFillInfo has no GradationData");
    return 1;
}

int clip_gradient_stops_encode(const clip_gradient *g, clip_buffer *out) {
    size_t i;
    int ok = g->stop_count > 0 && g->stop_count <= CLIP_GRADIENT_MAX_STOPS;
    ok = ok && clip__buf_u32be(out, STOP_HEADER) && clip__buf_u32be(out, STOP_SIZE) &&
         clip__buf_u32be(out, (uint32_t)g->stop_count) && clip__buf_u32be(out, 16);
    for (i = 0; ok && i < g->stop_count; ++i) {
        const clip_gradient_stop *s = &g->stops[i];
        double pos = s->position < 0 ? 0 : s->position > 1 ? 1 : s->position;
        int k;
        for (k = 0; ok && k < 4; ++k) ok = clip__buf_u32be(out, chan32(s->rgba[k]));
        ok = ok && clip__buf_u32be(out, 0) /* flags: plain stop */ &&
             clip__buf_u32be(out, (uint32_t)(pos * POS_ONE + 0.5)) && clip__buf_u32be(out, 0);
    }
    return ok;
}

int clip_gradation_encode(const clip_gradient *g, clip_buffer *out) {
    clip_buffer stops = {0}, b = {0};
    int k, ok = clip_gradient_stops_encode(g, &stops);
    /* CSP marks the end stops of a layer gradient 1 and 2 (observed on all
     * 380 surveyed layers); the flag word is at +16 in each 28-byte stop. */
    if (ok) {
        uint8_t *first = stops.data + STOP_HEADER + 16, *last = stops.data + STOP_HEADER + STOP_SIZE * (g->stop_count - 1) + 16;
        first[3] = 1;
        if (g->stop_count > 1) last[3] = 2;
    }
    ok = ok && clip__buf_u32be(&b, 0) /* total size, patched below */ && clip__buf_u32be(&b, 2) &&
         clip__buf_ustr(&b, "GradationData") && clip__buf_u32be(&b, (uint32_t)stops.size) &&
         clip__buf_append(&b, stops.data, stops.size) && clip__buf_ustr(&b, "GradationSetting") &&
         clip__buf_u32be(&b, (uint32_t)g->shape) && clip__buf_u32be(&b, (uint32_t)g->repeat) &&
         clip__buf_u32be(&b, (uint32_t)g->antialias) && buf_f64(&b, g->ellipse[0]) && buf_f64(&b, g->ellipse[1]) &&
         buf_f64(&b, g->ellipse[2]) && buf_f64(&b, g->x0) && buf_f64(&b, g->y0) && buf_f64(&b, g->x1) && buf_f64(&b, g->y1) &&
         clip__buf_ustr(&b, "GradationSettingAdd0001") && clip__buf_u32be(&b, 20) && clip__buf_u32be(&b, g->fill ? 1 : 0);
    for (k = 0; ok && k < 3; ++k) ok = clip__buf_u32be(&b, chan32(g->fill ? g->fill_rgb[k] : 0));
    ok = ok && clip__buf_u32be(&b, 1);
    if (ok) {
        uint32_t total = (uint32_t)b.size;
        b.data[0] = (uint8_t)(total >> 24);
        b.data[1] = (uint8_t)(total >> 16);
        b.data[2] = (uint8_t)(total >> 8);
        b.data[3] = (uint8_t)total;
        clip__buf_move(out, &b);
    }
    clip_buffer_free(&stops);
    clip_buffer_free(&b);
    return ok;
}
