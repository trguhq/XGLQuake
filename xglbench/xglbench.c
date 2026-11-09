/* XGLQuake standalone benchmark. GPL-2.0-or-later; see COPYING. */
#include "xglbench.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

#define XB_MAX_CASES 96
static int xb_quit;

static void usage(FILE *f)
{
    fprintf(f,
        "xglbench " XB_VERSION " - native Sun XGL flat-color benchmark\n"
        "Usage: xglbench [options]\n"
        "  -winsize W H       Run only this drawable size (default: 320x240, 640x480)\n"
        "  -seconds S         Minimum elapsed time per trial (default 1)\n"
        "  -warmup S          Warm-up per case, outside measurements (default .15)\n"
        "  -repeats N         Repeated trials, 1..31 (default 3)\n");
    fprintf(f,
        "  -triangles N       Base count, positive even number <=16384 (default 512)\n"
        "  -csv PATH          Write machine-readable per-trial and summary results\n"
        "  -test TEXT         Run case names containing TEXT\n"
        "  -display DISPLAY   X display (default DISPLAY environment variable)\n"
        "  -allow-fallback    Permit non-DGA/software/single-buffer investigation\n"
        "  -quick             Short smoke run (.05 seconds, .02 warm-up, 1 repeat)\n");
    fprintf(f,
        "                     Explicit timing/repeat options override -quick\n"
        "  -list              List matching cases without opening XGL\n"
        "  -help              Show this help\n"
        "Escape or closing the window stops the run. Keep the window unobscured.\n");
}

static int parse_long(const char *s, long lo, long hi, int *result)
{
    char *end;
    long value;
    if (!s || !*s) return 0;
    errno = 0;
    value = strtol(s, &end, 10);
    if (errno || *end || value < lo || value > hi) return 0;
    *result = (int)value;
    return 1;
}

static int parse_double(const char *s, double lo, double hi, double *result)
{
    char *end;
    double value;
    if (!s || !*s) return 0;
    errno = 0;
    value = strtod(s, &end);
    if (errno || *end || !(value >= lo && value <= hi)) return 0;
    *result = value;
    return 1;
}

static int options(int argc, char **argv, xb_options *o)
{
    int i;
    memset(o, 0, sizeof(*o));
    o->width = 320; o->height = 240;
    o->seconds = 1.0; o->warmup = .15; o->repeats = 3;
    o->base_triangles = 512;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-quick")) {
            o->quick = 1; o->seconds = .05; o->warmup = .02; o->repeats = 1;
        }
    }
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-help") || !strcmp(argv[i], "--help")) {
            usage(stdout); return 2;
        } else if (!strcmp(argv[i], "-quick")) {
            continue;
        } else if (!strcmp(argv[i], "-list")) {
            o->list_only = 1;
        } else if (!strcmp(argv[i], "-allow-fallback")) {
            o->allow_fallback = 1;
        } else if (!strcmp(argv[i], "-winsize")) {
            if (i + 2 >= argc || !parse_long(argv[i + 1], 128, 4096, &o->width) ||
                !parse_long(argv[i + 2], 64, 4096, &o->height)) goto invalid;
            i += 2; o->force_size = 1;
        } else if (!strcmp(argv[i], "-seconds")) {
            if (++i >= argc || !parse_double(argv[i], .001, 3600, &o->seconds)) goto invalid;
        } else if (!strcmp(argv[i], "-warmup")) {
            if (++i >= argc || !parse_double(argv[i], 0, 3600, &o->warmup)) goto invalid;
        } else if (!strcmp(argv[i], "-repeats")) {
            if (++i >= argc || !parse_long(argv[i], 1, XB_MAX_REPEATS, &o->repeats)) goto invalid;
        } else if (!strcmp(argv[i], "-triangles")) {
            if (++i >= argc || !parse_long(argv[i], 2, 16384, &o->base_triangles) ||
                (o->base_triangles & 1)) goto invalid;
        } else if (!strcmp(argv[i], "-csv")) {
            if (++i >= argc || !*argv[i]) goto invalid;
            o->csv_path = argv[i];
        } else if (!strcmp(argv[i], "-test")) {
            if (++i >= argc || !*argv[i]) goto invalid;
            o->filter = argv[i];
        } else if (!strcmp(argv[i], "-display")) {
            if (++i >= argc || !*argv[i]) goto invalid;
            o->display = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]); return 0;
        }
    }
    return 1;
invalid:
    fprintf(stderr, "Invalid or missing argument near %s\n", argv[i < argc ? i : argc - 1]);
    return 0;
}

static xb_case *add_case(xb_case *cases, int *count, const char *name)
{
    xb_case *c;
    if (*count >= XB_MAX_CASES) return NULL;
    c = &cases[(*count)++];
    memset(c, 0, sizeof(*c));
    strcpy(c->name, name);
    c->method = XB_MULTI; c->work = XB_DRAW;
    c->batch = 128; c->zbuffer = 1; c->size = .85f;
    return c;
}

static int make_cases(xb_case *cases)
{
    int n, m, b, d, z, i;
    int batches[4];
    char name[80];
    xb_case *c;
    const char *sizes[3];
    float scale[3];
    n = 0;
    batches[0] = 8; batches[1] = 32; batches[2] = 128; batches[3] = 512;
    c = add_case(cases, &n, "submit/polygon/batch-1");
    c->method = XB_POLYGON; c->batch = 1;
    for (m = XB_MULTI; m <= XB_TLIST; ++m) {
        for (b = 0; b < 4; ++b) {
            sprintf(name, "submit/%s/batch-%d", xb_method_name((enum xb_method)m), batches[b]);
            c = add_case(cases, &n, name);
            c->method = (enum xb_method)m; c->batch = batches[b];
        }
    }
    c = add_case(cases, &n, "submit/strip/natural-length");
    c->method = XB_STRIP;
    c = add_case(cases, &n, "storage/multi_polygon/retained-arrays");
    c->retained = 1;
    for (m = XB_POLYGON; m <= XB_STRIP; ++m) {
        sprintf(name, "connected/%s/depth-1", xb_method_name((enum xb_method)m));
        c = add_case(cases, &n, name);
        c->method = (enum xb_method)m; c->connected = 1; c->depth = 1;
        if (m == XB_POLYGON) c->batch = 1;
    }
    sizes[0] = "small"; sizes[1] = "medium"; sizes[2] = "large";
    scale[0] = .25f; scale[1] = .85f; scale[2] = 3.0f;
    for (i = 0; i < 3; ++i) {
        for (z = 0; z <= 1; ++z) {
            sprintf(name, "fill/%s/z-%s", sizes[i], z ? "on" : "off");
            c = add_case(cases, &n, name); c->size = scale[i]; c->zbuffer = z;
        }
    }
    for (z = 0; z <= 1; ++z) {
        sprintf(name, "overdraw/z-%s", z ? "on" : "off");
        c = add_case(cases, &n, name); c->overdraw = 1; c->zbuffer = z;
    }
    c = add_case(cases, &n, "overdraw/far-to-near/z-on");
    c->overdraw = 1; c->reverse = 1;
    c = add_case(cases, &n, "transform/changing-model"); c->update = XB_TRANSFORM;
    c = add_case(cases, &n, "transform/perspective"); c->perspective = 1;
    c = add_case(cases, &n, "transform/perspective-changing");
    c->perspective = 1; c->update = XB_TRANSFORM;
    c = add_case(cases, &n, "clip/near-crossing"); c->clipping = 1;
    c = add_case(cases, &n, "clip/near-crossing-perspective");
    c->clipping = 1; c->perspective = 1;
    c = add_case(cases, &n, "colors/changing-light"); c->update = XB_COLORS;
    for (d = 0; d <= 2; ++d) {
        sprintf(name, "subdivide/cached/depth-%d", d);
        c = add_case(cases, &n, name); c->depth = d;
        sprintf(name, "subdivide/relight/depth-%d", d);
        c = add_case(cases, &n, name); c->depth = d; c->update = XB_COLORS;
        sprintf(name, "subdivide/rebuild/depth-%d", d);
        c = add_case(cases, &n, name); c->depth = d; c->update = XB_REBUILD;
        sprintf(name, "frame/cached/depth-%d", d);
        c = add_case(cases, &n, name); c->depth = d; c->work = XB_FRAME;
        sprintf(name, "frame/relight/depth-%d", d);
        c = add_case(cases, &n, name); c->depth = d; c->work = XB_FRAME; c->update = XB_COLORS;
    }
    c = add_case(cases, &n, "overhead/color-clear"); c->work = XB_COLOR_CLEAR;
    c = add_case(cases, &n, "overhead/depth-clear"); c->work = XB_DEPTH_CLEAR;
    c = add_case(cases, &n, "overhead/swap"); c->work = XB_SWAP;
    c = add_case(cases, &n, "hud/opaque-copy"); c->work = XB_RASTER; c->zbuffer = 0;
    c = add_case(cases, &n, "hud/masked-copy"); c->work = XB_MASKED_RASTER; c->zbuffer = 0;
    return n;
}

static int build_mesh(xb_mesh *mesh, const xb_options *o, const xb_case *c)
{
    int ok;
    size_t i;
    if (c->connected) ok = xb_mesh_build_connected(mesh, o->base_triangles, c->depth);
    else ok = xb_mesh_build(mesh, o->base_triangles, c->depth, c->size, c->overdraw);
    if (ok && c->reverse) ok = xb_mesh_reverse_draw_order(mesh);
    if (ok && c->clipping) {
        for (i = 0; i < mesh->point_count; ++i)
            mesh->points[i].z = (mesh->points[i].x + .2f) * .8f;
        for (i = 0; i < mesh->strip_point_count; ++i)
            mesh->strip_points[i].z = (mesh->strip_points[i].x + .2f) * .8f;
    }
    return ok;
}

typedef struct { double total, prepare, render, present; size_t calls; } xb_frame;

static int one_frame(const xb_options *o, const xb_case *c, xb_mesh *mesh,
                     unsigned long sequence, xb_frame *f)
{
    double start, prepared, render_start, rendered, end;
    memset(f, 0, sizeof(*f));
    start = xb_now();
    if (c->update == XB_REBUILD) {
        if (!build_mesh(mesh, o, c) || !xb_backend_upload(mesh)) return 0;
    } else if (c->update == XB_COLORS) {
        xb_mesh_colors(mesh, sequence);
        xb_backend_colors(mesh);
    } else if (c->update == XB_TRANSFORM) {
        xb_backend_transform(sequence);
    }
    prepared = xb_now();
    if (c->work == XB_DRAW || c->work == XB_RASTER || c->work == XB_MASKED_RASTER) {
        /* Identical depth history for every frame. Exclude this reset from render_ms. */
        xb_backend_clear(1, c->zbuffer);
        xb_backend_sync();
    }
    render_start = xb_now();
    switch (c->work) {
    case XB_DRAW:
        f->calls = xb_backend_draw(c->method, c->batch); break;
    case XB_FRAME:
        xb_backend_clear(1, c->zbuffer);
        f->calls = xb_backend_draw(c->method, c->batch);
        xb_backend_raster(1);
        break;
    case XB_COLOR_CLEAR: xb_backend_clear(1, 0); break;
    case XB_DEPTH_CLEAR: xb_backend_clear(0, 1); break;
    case XB_RASTER: xb_backend_raster(0); break;
    case XB_MASKED_RASTER: xb_backend_raster(1); break;
    case XB_SWAP: break;
    }
    if (c->work != XB_SWAP) xb_backend_sync();
    rendered = xb_now();
    if (c->work == XB_SWAP || c->work == XB_FRAME) {
        xb_backend_swap();
        xb_backend_sync();
    }
    end = xb_now();
    f->prepare = prepared - start;
    f->render = c->work == XB_SWAP ? 0 : rendered - render_start;
    f->present = (c->work == XB_SWAP || c->work == XB_FRAME) ? end - rendered : 0;
    f->total = end - start;
    return 1;
}

static int events(xb_result *r)
{
    int result, width, height;
    result = xb_backend_events();
    if (!result) {
        xb_quit = 1;
        if (*xb_backend_error()) {
            strncpy(r->reason, xb_backend_error(), sizeof(r->reason) - 1);
            r->reason[sizeof(r->reason) - 1] = '\0';
        } else strcpy(r->reason, "interrupted by user");
        return 0;
    }
    if (result < 0) {
        xb_backend_dimensions(&width, &height);
        if (width != r->width || height != r->height) {
            strcpy(r->reason, "drawable changed size during trial; run stopped to preserve size labels");
            xb_quit = 1;
        } else if (*xb_backend_error()) {
            strncpy(r->reason, xb_backend_error(), sizeof(r->reason) - 1);
            r->reason[sizeof(r->reason) - 1] = '\0';
        } else strcpy(r->reason, "window exposed/obscured during trial; measurement discarded");
        return 0;
    }
    return 1;
}

static void measure(const xb_options *o, xb_result *r, const int *valid,
                    char validation[XB_METHODS][160])
{
    xb_mesh mesh;
    xb_frame f;
    double start, cpu, sum, prep, render, present;
    unsigned long frames, sequence;
    int trial, uses_mesh, preview;
    const xb_case *c;
    c = &r->test;
    memset(&mesh, 0, sizeof(mesh));
    uses_mesh = c->work == XB_DRAW || c->work == XB_FRAME;
    if (uses_mesh && !valid[c->method]) {
        strcpy(r->reason, validation[c->method]); return;
    }
    if ((c->work == XB_SWAP || c->work == XB_FRAME) && !xb_backend_has_double_buffer()) {
        strcpy(r->reason, "two hardware/window buffers not allocated"); return;
    }
    if (uses_mesh && !build_mesh(&mesh, o, c)) {
        strcpy(r->reason, "geometry allocation or input limit failure"); return;
    }
    if (!xb_backend_configure(c, &mesh, r->reason, sizeof(r->reason))) goto done;
    /* configure() owns the initial upload; updates upload inside one_frame(). */
    r->triangles = mesh.triangles;
    r->bytes = mesh.bytes;
    xb_backend_label(c->name);
    /* Show a representative image even for tests which exclude presentation. */
    xb_backend_clear(1, c->zbuffer);
    if (uses_mesh) xb_backend_draw(c->method, c->batch);
    if (c->work == XB_RASTER || c->work == XB_MASKED_RASTER)
        xb_backend_raster(c->work == XB_MASKED_RASTER);
    xb_backend_sync();
    if (xb_backend_has_double_buffer()) xb_backend_swap();
    xb_backend_sync();
    preview = xb_backend_events();
    if (!preview) {
        xb_quit = 1;
        if (*xb_backend_error()) {
            strncpy(r->reason, xb_backend_error(), sizeof(r->reason) - 1);
            r->reason[sizeof(r->reason) - 1] = '\0';
        } else strcpy(r->reason, "interrupted by user");
        goto done;
    }
    /* Resize/expose from setup are consumed before warm-up; later events invalidate. */
    sequence = 0;
    start = xb_now();
    do {
        if (!events(r)) goto done;
        if (!one_frame(o, c, &mesh, sequence++, &f)) {
            strcpy(r->reason, "warm-up geometry/native buffer allocation failed"); goto done;
        }
    } while (xb_now() - start < o->warmup);
    for (trial = 0; trial < o->repeats; ++trial) {
        frames = 0; sum = 0; prep = 0; render = 0; present = 0;
        cpu = xb_cpu_now(); start = xb_now();
        do {
            if (!events(r)) goto done;
            if (!one_frame(o, c, &mesh, sequence++, &f)) {
                strcpy(r->reason, "geometry/native buffer allocation failed"); goto done;
            }
            ++frames;
            sum += f.total; prep += f.prepare; render += f.render; present += f.present;
            r->calls = f.calls;
        } while (frames < 3 || xb_now() - start < o->seconds);
        /* A fault in the final draw/flush must not turn into a successful trial. */
        if (!events(r)) goto done;
        r->cpu[trial] = (xb_cpu_now() - cpu) / (double)frames;
        r->total[trial] = sum / (double)frames;
        r->prepare[trial] = prep / (double)frames;
        r->render[trial] = render / (double)frames;
        r->present[trial] = present / (double)frames;
        r->frames += frames; ++r->samples;
    }
    r->median = xb_median(r->total, r->samples);
    r->prep_median = xb_median(r->prepare, r->samples);
    r->render_median = xb_median(r->render, r->samples);
    r->present_median = xb_median(r->present, r->samples);
    r->low = r->high = r->total[0];
    for (trial = 1; trial < r->samples; ++trial) {
        if (r->total[trial] < r->low) r->low = r->total[trial];
        if (r->total[trial] > r->high) r->high = r->total[trial];
    }
    r->passed = 1;
done:
    xb_mesh_free(&mesh);
}

static void csv_text(FILE *f, const char *s)
{
    fputc('"', f);
    while (*s) { if (*s == '"') fputc('"', f); fputc(*s++, f); }
    fputc('"', f);
}

static void csv_row(FILE *f, const xb_result *r, const char *record, int trial,
                    double total, double prep, double render, double present, double cpu)
{
    double rate;
    if (!f) return;
    rate = r->triangles && render > 0 ? (double)r->triangles / render : 0;
    fprintf(f, "%s,%d,%d,", record, r->width, r->height);
    csv_text(f, r->test.name);
    fprintf(f, ",%s,%d,%s,%d,%d,%d,%d,%d,%lu,%lu,%lu,%lu,%.9f,%.9f,%.9f,%.9f,%.9f,%.3f,",
            r->passed ? "ok" : "skipped", trial, xb_method_name(r->test.method),
            r->test.batch, r->test.depth, r->test.zbuffer, r->test.connected, r->test.retained,
            (unsigned long)r->triangles, (unsigned long)r->calls,
            (unsigned long)r->bytes, r->frames,
            total * 1000, prep * 1000, render * 1000, present * 1000, cpu * 1000, rate);
    csv_text(f, r->reason);
    fputc('\n', f);
}

static void print_result(FILE *csv, const xb_result *r)
{
    int i;
    double rate;
    if (!r->passed) {
        printf("%-38s SKIPPED: %s\n", r->test.name, r->reason);
        csv_row(csv, r, "summary", -1, 0, 0, 0, 0, 0);
        return;
    }
    rate = r->triangles && r->render_median > 0 ? (double)r->triangles / r->render_median : 0;
    printf("%-38s %9.3f %9.3f %9.3f %9.3f %10.0f %6lu\n",
        r->test.name, r->median * 1000, r->prep_median * 1000,
        r->render_median * 1000, r->present_median * 1000,
        rate, (unsigned long)r->calls);
    printf("  trial-mean range %.3f..%.3f ms; %lu frames, %lu facets/frame, "
           "%.1f KiB portable mesh, CPU %.3f ms/frame\n",
        r->low * 1000, r->high * 1000, r->frames, (unsigned long)r->triangles,
        (double)r->bytes / 1024, xb_median(r->cpu, r->samples) * 1000);
    for (i = 0; i < r->samples; ++i)
        csv_row(csv, r, "trial", i + 1, r->total[i], r->prepare[i],
                r->render[i], r->present[i], r->cpu[i]);
    csv_row(csv, r, "summary", -1, r->median, r->prep_median,
            r->render_median, r->present_median, xb_median(r->cpu, r->samples));
}

static const xb_result *find_result(const xb_result *r, int count,
                                   int width, int height, const char *name)
{
    int i;
    for (i = 0; i < count; ++i)
        if (r[i].passed && r[i].width == width && r[i].height == height &&
            !strcmp(r[i].test.name, name)) return &r[i];
    return NULL;
}

static void findings(const xb_result *r, int count, int width, int height)
{
    int i, d;
    const xb_result *best, *poly, *connected, *a, *b;
    char name[80];
    double ratio;
    best = NULL; connected = NULL;
    printf("\nFindings for %dx%d (synthetic workload, not Quake FPS):\n", width, height);
    for (i = 0; i < count; ++i) {
        if (!r[i].passed || r[i].width != width || r[i].height != height) continue;
        if (!strncmp(r[i].test.name, "submit/", 7) && r[i].render_median > 0 &&
            (!best || r[i].render_median < best->render_median)) best = &r[i];
        if (!strncmp(r[i].test.name, "connected/", 10) && r[i].render_median > 0 &&
            (!connected || r[i].render_median < connected->render_median)) connected = &r[i];
        if (r[i].samples > 1 && r[i].median > 0 && (r[i].high - r[i].low) / r[i].median > .15)
            printf("  Variable timing: %s (trial-mean range exceeds 15%% of median).\n", r[i].test.name);
    }
    if (best) {
        printf("  Lowest measured independent-mesh render time: %s, %.3f ms.\n",
            best->test.name, best->render_median * 1000);
        poly = find_result(r, count, width, height, "submit/polygon/batch-1");
        if (poly && best->render_median > 0)
            printf("  %.2fx throughput relative to individual polygon calls.\n",
                poly->render_median / best->render_median);
        printf("  This is a candidate, not a statistically established universal winner.\n");
    }
    if (connected)
        printf("  Lowest measured connected-grid render time: %s, %.3f ms.\n",
            connected->test.name, connected->render_median * 1000);
    a = find_result(r, count, width, height, "submit/multi_polygon/batch-128");
    b = find_result(r, count, width, height, "storage/multi_polygon/retained-arrays");
    if (a && b && a->render_median > 0)
        printf("  Retained/volatile native-array render-time ratio: %.2f (same mesh/batch).\n",
            b->render_median / a->render_median);
    a = find_result(r, count, width, height, "subdivide/cached/depth-0");
    if (a) {
        for (d = 1; d <= 2; ++d) {
            sprintf(name, "subdivide/cached/depth-%d", d);
            b = find_result(r, count, width, height, name);
            if (b && a->render_median > 0)
                printf("  Depth %d: %lu facets, %.2fx baseline render time.\n",
                    d, (unsigned long)b->triangles, b->render_median / a->render_median);
        }
    }
    a = find_result(r, count, width, height, "subdivide/cached/depth-1");
    b = find_result(r, count, width, height, "subdivide/rebuild/depth-1");
    if (a && b && a->median > 0)
        printf("  Rebuilding depth-1 geometry each frame costs %.2fx the cached total; "
               "rebuild preparation %.3f ms/frame.\n", b->median / a->median, b->prep_median * 1000);
    a = find_result(r, count, width, height, "hud/opaque-copy");
    b = find_result(r, count, width, height, "hud/masked-copy");
    if (a && b && a->render_median > 0)
        printf("  Masked/opaque HUD copy time ratio: %.2f.\n", b->render_median / a->render_median);
    a = find_result(r, count, width, height, "overdraw/z-on");
    b = find_result(r, count, width, height, "overdraw/far-to-near/z-on");
    if (a && b && a->render_median > 0)
        printf("  Far-to-near/near-to-far overdraw time ratio: %.2f (same geometry).\n",
            b->render_median / a->render_median);
    for (d = 0; d <= 2; ++d) {
        sprintf(name, "frame/relight/depth-%d", d);
        a = find_result(r, count, width, height, name);
        if (a && a->median > 0) {
            ratio = a->present_median / a->median;
            printf("  Relit depth-%d synthetic frame: %.3f ms; presentation %.0f%%.\n",
                d, a->median * 1000, ratio * 100);
            printf("    Leaves %.3f ms of a 15 FPS budget for game/visibility/audio "
                   "(this scene only).\n", 1000.0 / 15.0 - a->median * 1000);
        }
    }
    printf("  Choose depth 1 only if its measured frame budget leaves enough CPU time;\n"
           "  reserve depth 2 for selected surfaces and retain a complete coarse mesh.\n");
}

static void compare_sizes(const xb_result *r, int count)
{
    int i;
    const xb_result *large;
    printf("\nResolution comparison: 640x480 / 320x240 (4x pixel area)\n");
    for (i = 0; i < count; ++i) {
        if (!r[i].passed || r[i].width != 320 || r[i].height != 240) continue;
        large = find_result(r, count, 640, 480, r[i].test.name);
        if (!large || r[i].render_median <= 0 || r[i].median <= 0) continue;
        printf("  %-38s render %6.2fx; total %6.2fx\n", r[i].test.name,
            large->render_median / r[i].render_median, large->median / r[i].median);
    }
    printf("  Weak render-time scaling is consistent with geometry/CPU/dispatch limits;\n"
           "  stronger scaling indicates pixel-work sensitivity. Neither identifies a\n"
           "  unique bottleneck. Swap timing can be quantized by display refresh.\n"
           "  HUD copies remain 128x64 pixels at both sizes; their work does not quadruple.\n");
}

int main(int argc, char **argv)
{
    xb_options o;
    xb_case cases[XB_MAX_CASES];
    xb_result *results, *r;
    FILE *csv;
    int parsed, count, selected, i, s, m, nresult, w, h, successes, sizes;
    int valid[XB_METHODS];
    char validation[XB_METHODS][160];
    struct utsname host;
    parsed = options(argc, argv, &o);
    if (parsed == 2) return 0;
    if (!parsed) { usage(stderr); return 2; }
    count = make_cases(cases);
    selected = 0;
    for (i = 0; i < count; ++i) {
        if (o.filter && !strstr(cases[i].name, o.filter)) continue;
        ++selected;
        if (o.list_only) puts(cases[i].name);
    }
    if (!selected) { fprintf(stderr, "No test names match the filter.\n"); return 2; }
    if (o.list_only) return 0;
    csv = NULL;
    if (o.csv_path) {
        csv = fopen(o.csv_path, "w");
        if (!csv) { perror(o.csv_path); return 1; }
        fprintf(csv, "record,width,height,test,status,trial,method,batch,depth,zbuffer,connected,retained,"
                     "triangles,calls,mesh_bytes,frames_all_trials,total_ms,prepare_ms,render_ms,"
                     "present_ms,cpu_ms,triangles_per_second,reason\n");
    }
    results = (xb_result *)calloc(XB_MAX_RESULTS, sizeof(*results));
    if (!results) { if (csv) fclose(csv); fprintf(stderr, "Out of memory.\n"); return 1; }
    if (!xb_backend_open(&o, o.width, o.height)) {
        fprintf(stderr, "XGL setup failed: %s\n", xb_backend_error());
        xb_backend_close(); free(results); if (csv) fclose(csv); return 1;
    }
    printf("xglbench " XB_VERSION " - native XGL flat-color pipeline investigation\n");
    if (uname(&host) == 0)
        printf("Host: %s %s %s %s\n", host.nodename, host.sysname, host.release, host.machine);
    printf("Build: %s %s; pointer %lu bytes; float %lu bytes\n", __DATE__, __TIME__,
        (unsigned long)sizeof(void *), (unsigned long)sizeof(float));
    xb_backend_info(stdout);
    printf("Settings: %d base triangles; %.3f seconds/trial; %d repeats; %.3f seconds warm-up\n",
        o.base_triangles, o.seconds, o.repeats, o.warmup);
    printf("Texture sampling is CPU-only; XGL receives positions and flat RGB facet colors.\n"
           "All timed rendering waits for completion. No result is a GPU-only timer.\n"
           "Totals include reset clears; draw render_ms excludes the reset. frame/* includes\n"
           "clear+draw+masked HUD in render_ms and swap in present_ms. Event polling is outside totals.\n");
    printf(
           "Preparation includes native-array conversion/allocation where applicable.\n"
           "Rebuild cases also recreate portable list AND strip representations; this\n"
           "is harness preparation cost, not an optimized engine implementation.\n"
           "Statistics are medians/ranges of per-trial means, not frame-latency percentiles.\n"
           "Mesh bytes exclude XGL/native buffers and driver allocations.\n");
    if (!xb_backend_has_hardware())
        printf("FALLBACK/TEST RUN: results do not establish the accelerated ZX performance path.\n");
    nresult = 0; successes = 0;
    sizes = o.force_size ? 1 : 2;
    for (s = 0; s < sizes && !xb_quit; ++s) {
        w = o.force_size ? o.width : (s ? 640 : 320);
        h = o.force_size ? o.height : (s ? 480 : 240);
        if (s && !xb_backend_resize(w, h)) {
            fprintf(stderr, "Resize failed: %s\n", xb_backend_error()); xb_quit = 1; break;
        }
        xb_backend_dimensions(&w, &h);
        printf("\nDrawable: %dx%d\n", w, h);
        for (m = 0; m < XB_METHODS; ++m) {
            validation[m][0] = '\0';
            valid[m] = xb_backend_validate((enum xb_method)m, validation[m], sizeof(validation[m]));
            printf("Validation %-8s: %s %s\n", xb_method_name((enum xb_method)m),
                valid[m] ? "PASS" : "FAIL", validation[m]);
        }
        printf("%-38s %9s %9s %9s %9s %10s %6s\n", "Test", "total_ms", "prep_ms",
            "render_ms", "swap_ms", "facets/sec", "calls");
        for (i = 0; i < count && !xb_quit; ++i) {
            if (o.filter && !strstr(cases[i].name, o.filter)) continue;
            r = &results[nresult++]; r->test = cases[i]; r->width = w; r->height = h;
            fprintf(stderr, "[%dx%d] %s\n", w, h, cases[i].name);
            measure(&o, r, valid, validation);
            print_result(csv, r);
            if (r->passed) ++successes;
            fflush(stdout);
            if (csv && fflush(csv) != 0) {
                perror("writing CSV"); xb_quit = 1;
            }
        }
        findings(results, nresult, w, h);
    }
    if (!o.force_size && !xb_quit) compare_sizes(results, nresult);
    printf("\nCompleted %d cases; %d skipped/failed. %s\n", successes, nresult - successes,
        xb_quit ? "Run interrupted or output/resize failed." : "Run finished.");
    xb_backend_close();
    free(results);
    if (csv && fclose(csv) != 0) { perror("closing CSV"); return 1; }
    return xb_quit || !successes ? 1 : 0;
}
