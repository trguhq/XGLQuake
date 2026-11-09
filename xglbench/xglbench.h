/* XGLQuake standalone benchmark. GPL-2.0-or-later; see COPYING. */
#ifndef XGLBENCH_H
#define XGLBENCH_H

#include <stddef.h>
#include <stdio.h>

#define XB_VERSION "0.1"
#define XB_MAX_RESULTS 512
#define XB_MAX_REPEATS 31
#define XB_METHODS 4

typedef struct { float x, y, z; } xb_point;
typedef struct { float r, g, b; } xb_rgb;
typedef struct { size_t first_point, point_count, first_facet; } xb_strip;
typedef struct {
    size_t triangles, point_count, strip_point_count, strip_count;
    xb_point *points;                 /* independent triangle corners */
    xb_point *strip_points;           /* equivalent strip geometry */
    xb_rgb *colors;                   /* one color per triangle */
    xb_rgb *base_colors;
    xb_strip *strips;
    size_t bytes;
} xb_mesh;

enum xb_method { XB_POLYGON, XB_MULTI, XB_TLIST, XB_STRIP };
enum xb_work { XB_DRAW, XB_COLOR_CLEAR, XB_DEPTH_CLEAR, XB_SWAP,
               XB_RASTER, XB_MASKED_RASTER, XB_FRAME };
enum xb_update { XB_STATIC, XB_TRANSFORM, XB_COLORS, XB_REBUILD };

typedef struct {
    char name[80];
    enum xb_work work;
    enum xb_method method;
    enum xb_update update;
    int batch, depth, zbuffer, overdraw, perspective, connected, retained, clipping, reverse;
    float size;
} xb_case;

typedef struct {
    int width, height, force_size;
    int repeats, base_triangles, quick, allow_fallback, list_only;
    double seconds, warmup;
    const char *csv_path, *filter, *display;
} xb_options;

typedef struct {
    xb_case test;
    int width, height, passed, samples;
    unsigned long frames;
    size_t triangles, calls, bytes;
    double total[XB_MAX_REPEATS];
    double prepare[XB_MAX_REPEATS];
    double render[XB_MAX_REPEATS];
    double present[XB_MAX_REPEATS];
    double cpu[XB_MAX_REPEATS];
    double median, low, high, prep_median, render_median, present_median;
    char reason[160];
} xb_result;

/* Portable geometry: no XGL/X11 dependency and no game assets. */
int xb_mesh_build(xb_mesh *mesh, int base_triangles, int depth,
                  float size, int overdraw);
int xb_mesh_build_connected(xb_mesh *mesh, int base_triangles, int depth);
int xb_mesh_reverse_draw_order(xb_mesh *mesh);
void xb_mesh_free(xb_mesh *mesh);
void xb_mesh_colors(xb_mesh *mesh, unsigned long frame);
size_t xb_mesh_expected_triangles(int base_triangles, int depth);
const char *xb_method_name(enum xb_method method);

/* Time is elapsed wall time; gethrtime() on Solaris. CPU includes user+system. */
double xb_now(void);
double xb_cpu_now(void);
double xb_median(const double *values, int count);

/* Backend owns all native arrays; these functions never retain mesh pointers. */
int xb_backend_open(const xb_options *options, int width, int height);
void xb_backend_close(void);
int xb_backend_resize(int width, int height);
void xb_backend_dimensions(int *width, int *height);
void xb_backend_info(FILE *out);
int xb_backend_validate(enum xb_method method, char *reason, size_t size);
int xb_backend_configure(const xb_case *test, const xb_mesh *mesh,
                         char *reason, size_t size);
int xb_backend_upload(const xb_mesh *mesh);
void xb_backend_colors(const xb_mesh *mesh);
void xb_backend_transform(unsigned long frame);
void xb_backend_clear(int color, int depth);
size_t xb_backend_draw(enum xb_method method, int batch);
void xb_backend_sync(void);
void xb_backend_swap(void);
void xb_backend_raster(int masked);
int xb_backend_events(void); /* 0 requests exit; -1 means resized/exposed */
void xb_backend_label(const char *name);
int xb_backend_has_double_buffer(void);
int xb_backend_has_hardware(void);
const char *xb_backend_error(void);

#endif
