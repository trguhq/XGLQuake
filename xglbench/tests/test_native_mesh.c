/* Mesh probe control-flow tests, GPL-2.0-or-later.
 * A model device keeps the triangles it is given and answers pixel readback
 * by interpolating their corner colors. It is not an XGL implementation,
 * rasterizer, ABI test, or hardware result: it only shows that the probe
 * submits what it says and rejects a device that draws something else.
 */
#include <assert.h>
#include <stdarg.h>
#include "../xglbench_xgl.c"

enum mock_fault {
    MOCK_CORRECT, MOCK_STRIP_AS_LIST, MOCK_LIST_AS_STRIP, MOCK_FLAT_COLOR,
    MOCK_STRIP_NOT_DRAWN, MOCK_NOTHING_DRAWN
};

typedef struct { float x, y; Xgl_color color; } mock_corner;
typedef struct { mock_corner corner[3]; } mock_triangle;

static int system_token, context_token, model_token, view_token;
static enum mock_fault fault;
static int actions;
static mock_triangle drawn[4096];
static int drawn_count;
static unsigned int polygon_calls, strip_calls, list_calls, clears, readbacks;
static unsigned long strip_points, list_points;

int xb_cancel_requested(void) { return 0; }
/* Reached only by the buffer-switch probe, which these tests do not run. */
double xb_now(void) { assert(!"no clock is read here"); return 0; }
/* Only native lighting, which these draws do not use, would ask for this. */
xb_rgb xb_lighting_evaluate(const xb_light_state *state, const xb_point *position,
                            const xb_point *normal, const xb_rgb *material)
{
    (void)state; (void)position; (void)normal;
    assert(!"lighting was not expected");
    return *material;
}
const char *xb_method_name(enum xb_method method)
{
    return method == XB_STRIP ? "triangle_strip" :
           (method == XB_TLIST ? "triangle_list" : "multi_polygon");
}

void xgl_object_set(Xgl_object object, ...)
{
    va_list args;
    int attribute;
    assert(object == b.ctx);
    va_start(args, object);
    while ((attribute = va_arg(args, int)) != 0) {
        switch (attribute) {
        case XGL_CTX_NEW_FRAME_ACTION: actions = va_arg(args, int); break;
        case XGL_3D_CTX_HLHSR_MODE: (void)va_arg(args, int); break;
        default: assert(!"unmodeled mesh probe attribute");
        }
    }
    va_end(args);
}

void xgl_object_get(Xgl_object object, int attribute, void *value)
{
    assert(object == b.system && attribute == XGL_SYS_ST_ERROR_INFO);
    memset(value, 0, sizeof(Xgl_error_info));
}

void xgl_transform_write(Xgl_trans transform, void *matrix)
{
    Xgl_matrix_f3d *values;
    int i, j;
    assert(transform == b.model || transform == b.view);
    values = (Xgl_matrix_f3d *)matrix;
    for (i = 0; i < 4; ++i)
        for (j = 0; j < 4; ++j)
            assert((*values)[i][j] == (i == j ? 1.0f : 0.0f));
}

void xgl_context_new_frame(Xgl_ctx context)
{
    assert(context == b.ctx);
    ++clears;
    if (actions & XGL_CTX_NEW_FRAME_CLEAR) drawn_count = 0;
}

void xgl_context_flush(Xgl_ctx context, Xgl_flush_action action)
{
    assert(context == b.ctx && action == XGL_FLUSH_SYNCHRONIZE);
}

static void keep(const Xgl_pt_color_f3d *a, const Xgl_pt_color_f3d *c1,
                 const Xgl_pt_color_f3d *c2)
{
    const Xgl_pt_color_f3d *points[3];
    mock_triangle *triangle;
    int i;
    assert(drawn_count < (int)(sizeof(drawn) / sizeof(drawn[0])));
    points[0] = a; points[1] = c1; points[2] = c2;
    triangle = &drawn[drawn_count++];
    for (i = 0; i < 3; ++i) {
        assert(points[i]->z == .5f);
        triangle->corner[i].x = points[i]->x;
        triangle->corner[i].y = points[i]->y;
        triangle->corner[i].color = fault == MOCK_FLAT_COLOR ? a->color : points[i]->color;
    }
}

void xgl_polygon(Xgl_ctx context, Xgl_facet_type type, Xgl_facet *facet,
                 Xgl_bbox *bounds, Xgl_usgn32 count, Xgl_pt_list *lists)
{
    (void)context; (void)type; (void)facet; (void)bounds; (void)count; (void)lists;
    assert(!"single polygons are not part of the mesh probes");
}

void xgl_multi_simple_polygon(Xgl_ctx context, Xgl_facet_flags flags,
                              Xgl_facet_list *facets, Xgl_bbox *bounds,
                              Xgl_usgn32 count, Xgl_pt_list *lists)
{
    Xgl_usgn32 i;
    (void)flags;
    assert(context == b.ctx && !bounds && !facets && count > 0);
    ++polygon_calls;
    if (fault == MOCK_NOTHING_DRAWN) return;
    for (i = 0; i < count; ++i) {
        assert(lists[i].pt_type == XGL_PT_COLOR_F3D && lists[i].num_pts == 3);
        /* The reference path is the one the device already draws correctly:
         * no fault is modeled in it. */
        assert(drawn_count < (int)(sizeof(drawn) / sizeof(drawn[0])));
        drawn[drawn_count].corner[0].x = lists[i].pts.color_f3d[0].x;
        drawn[drawn_count].corner[0].y = lists[i].pts.color_f3d[0].y;
        drawn[drawn_count].corner[0].color = lists[i].pts.color_f3d[0].color;
        drawn[drawn_count].corner[1].x = lists[i].pts.color_f3d[1].x;
        drawn[drawn_count].corner[1].y = lists[i].pts.color_f3d[1].y;
        drawn[drawn_count].corner[1].color = lists[i].pts.color_f3d[1].color;
        drawn[drawn_count].corner[2].x = lists[i].pts.color_f3d[2].x;
        drawn[drawn_count].corner[2].y = lists[i].pts.color_f3d[2].y;
        drawn[drawn_count].corner[2].color = lists[i].pts.color_f3d[2].color;
        ++drawn_count;
    }
}

void xgl_triangle_strip(Xgl_ctx context, Xgl_facet_list *facets, Xgl_pt_list *points)
{
    const Xgl_pt_color_f3d *p;
    Xgl_usgn32 i;
    assert(context == b.ctx && !facets);
    assert(points->pt_type == XGL_PT_COLOR_F3D && points->num_pts >= 3);
    ++strip_calls;
    strip_points += points->num_pts;
    p = points->pts.color_f3d;
    if (fault == MOCK_STRIP_NOT_DRAWN || fault == MOCK_NOTHING_DRAWN) return;
    if (fault == MOCK_STRIP_AS_LIST) {
        for (i = 0; i + 2 < points->num_pts; i += 3) keep(p + i, p + i + 1, p + i + 2);
        return;
    }
    for (i = 0; i + 2 < points->num_pts; ++i) keep(p + i, p + i + 1, p + i + 2);
}

void xgl_triangle_list(Xgl_ctx context, Xgl_facet_list *facets, Xgl_pt_list *points,
                       Xgl_tlist_flags flags)
{
    const Xgl_pt_color_f3d *p;
    Xgl_usgn32 i;
    assert(context == b.ctx && !facets && flags == XGL_TLIST_FLAG_TRI_RESTART);
    assert(points->pt_type == XGL_PT_COLOR_F3D && points->num_pts >= 3);
    assert(points->num_pts % 3 == 0);
    ++list_calls;
    list_points += points->num_pts;
    p = points->pts.color_f3d;
    if (fault == MOCK_NOTHING_DRAWN) return;
    if (fault == MOCK_LIST_AS_STRIP) {
        for (i = 0; i + 2 < points->num_pts; ++i) keep(p + i, p + i + 1, p + i + 2);
        return;
    }
    for (i = 0; i + 2 < points->num_pts; i += 3) keep(p + i, p + i + 1, p + i + 2);
}

/* The window shows VDC -1..1 on pixels 0..size-1, as the probes assume. */
Xgl_boolean xgl_context_get_pixel(Xgl_ctx context, Xgl_pt_i2d *point, Xgl_color *color)
{
    const mock_triangle *t;
    double x, y, area, w0, w1, w2;
    int i;
    assert(context == b.ctx);
    ++readbacks;
    memset(color, 0, sizeof(*color));
    x = 2.0 * point->x / (b.width - 1) - 1.0;
    y = 2.0 * point->y / (b.height - 1) - 1.0;
    for (i = drawn_count - 1; i >= 0; --i) {
        t = &drawn[i];
        area = (t->corner[1].x - t->corner[0].x) * (double)(t->corner[2].y - t->corner[0].y) -
               (t->corner[2].x - t->corner[0].x) * (double)(t->corner[1].y - t->corner[0].y);
        if (area == 0) continue;
        w1 = ((x - t->corner[0].x) * (t->corner[2].y - t->corner[0].y) -
              (t->corner[2].x - t->corner[0].x) * (y - t->corner[0].y)) / area;
        w2 = ((t->corner[1].x - t->corner[0].x) * (y - t->corner[0].y) -
              (x - t->corner[0].x) * (t->corner[1].y - t->corner[0].y)) / area;
        w0 = 1.0 - w1 - w2;
        if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
        color->rgb.r = (float)(w0 * t->corner[0].color.rgb.r + w1 * t->corner[1].color.rgb.r +
                               w2 * t->corner[2].color.rgb.r);
        color->rgb.g = (float)(w0 * t->corner[0].color.rgb.g + w1 * t->corner[1].color.rgb.g +
                               w2 * t->corner[2].color.rgb.g);
        color->rgb.b = (float)(w0 * t->corner[0].color.rgb.b + w1 * t->corner[1].color.rgb.b +
                               w2 * t->corner[2].color.rgb.b);
        break;
    }
    return FALSE;
}

/* Rows of cells across the middle nine tenths of the view, as the bench's
 * connected mesh is laid out: each row is one strip, and the same triangles
 * are also stored corner by corner. Colors are the sharpest pattern smooth
 * shading can carry: each cell corner's color is unrelated to the next. */
#define ROWS 6
#define COLUMNS 16
#define TRIANGLES (ROWS * COLUMNS * 2)
static xb_point mesh_points[TRIANGLES * 3], mesh_strip_points[ROWS * 2 * (COLUMNS + 1)];
static xb_rgb mesh_colors[TRIANGLES * 3], mesh_facet_colors[TRIANGLES];
static xb_strip mesh_strips[ROWS];
static xb_mesh mesh;

static xb_rgb corner_color(const xb_point *point)
{
    xb_rgb color;
    int column, row;
    column = (int)floor((point->x + .9) / 1.8 * COLUMNS + .5);
    row = (int)floor((point->y + .9) / 1.8 * ROWS + .5);
    color.r = .15f + .8f * (float)((column * 5 + row * 3) % 7) / 6;
    color.g = .15f + .8f * (float)((column * 2 + row * 5) % 5) / 4;
    color.b = .15f + .8f * (float)((column + row * 2) % 3) / 2;
    return color;
}

static void build_mesh(void)
{
    size_t row, column, side, point, facet, triangle, start, a, b2, c, i;
    memset(&mesh, 0, sizeof(mesh));
    mesh.triangles = TRIANGLES; mesh.point_count = TRIANGLES * 3;
    mesh.strip_count = ROWS; mesh.strip_point_count = ROWS * 2 * (COLUMNS + 1);
    mesh.points = mesh_points; mesh.strip_points = mesh_strip_points;
    mesh.vertex_colors = mesh_colors; mesh.colors = mesh_facet_colors;
    mesh.strips = mesh_strips;
    point = facet = 0;
    for (row = 0; row < ROWS; ++row) {
        mesh_strips[row].first_point = start = point;
        mesh_strips[row].point_count = 2 * (COLUMNS + 1);
        mesh_strips[row].first_facet = facet;
        for (column = 0; column <= COLUMNS; ++column) {
            for (side = 0; side < 2; ++side, ++point) {
                mesh_strip_points[point].x = (float)(-.9 + 1.8 * column / COLUMNS);
                mesh_strip_points[point].y = (float)(-.9 + 1.8 * (row + 1 - side) / ROWS);
                mesh_strip_points[point].z = .5f;
            }
        }
        for (triangle = 0; triangle < 2 * COLUMNS; ++triangle, ++facet) {
            a = start + triangle; b2 = a + 1; c = a + 2;
            if (triangle & 1) { a = start + triangle + 1; b2 = a - 1; }
            mesh_points[facet * 3] = mesh_strip_points[a];
            mesh_points[facet * 3 + 1] = mesh_strip_points[b2];
            mesh_points[facet * 3 + 2] = mesh_strip_points[c];
        }
    }
    for (i = 0; i < TRIANGLES * 3; ++i) mesh_colors[i] = corner_color(&mesh_points[i]);
}

static void reset_mock(enum mock_fault injected, enum xb_method method, int batch)
{
    free_geometry();
    memset(&b, 0, sizeof(b));
    b.system = &system_token;
    b.ctx = &context_token;
    b.model = &model_token;
    b.view = &view_token;
    b.width = 320; b.height = 240;
    b.current_z = 1;
    b.shading = XB_SMOOTH;
    b.method = method; b.batch = batch;
    fault = injected;
    actions = 0;
    drawn_count = 0;
    polygon_calls = strip_calls = list_calls = clears = readbacks = 0;
    strip_points = list_points = 0;
}

static int run_probe(enum mock_fault injected, enum xb_method method, int batch, char *note)
{
    xb_case test;
    reset_mock(injected, method, batch);
    memset(&test, 0, sizeof(test));
    test.method = method; test.batch = batch; test.shade = XB_SMOOTH;
    test.work = XB_DRAW; test.update = XB_STATIC; test.probe = XB_PROBE_MESH;
    assert(xb_backend_upload(&mesh));
    return apply_probe(&test, note, 240);
}

static void test_correct_device(void)
{
    char note[240];
    /* The polygons the renderer submits now, in its batches of 256. */
    assert(run_probe(MOCK_CORRECT, XB_MULTI, 256, note));
    assert(strstr(note, "192 smooth triangles as 576 points in 1 multi_polygon call(s)"));
    assert(strstr(note, "picture matches independent polygons at 48 pixels"));
    /* Reference in chunks of 64, then the timed path once. */
    assert(polygon_calls == 3 + 1 && readbacks == 96 && clears == 3);
    assert(xb_backend_draw(XB_MULTI, 256) == 1 && drawn_count == TRIANGLES);
    /* One list of independent triangles for each batch. */
    assert(run_probe(MOCK_CORRECT, XB_TLIST, 64, note));
    assert(strstr(note, "192 smooth triangles as 576 points in 3 triangle_list call(s)"));
    assert(list_calls == 3 && list_points == 576 && !strip_calls);
    assert(xb_backend_input_bytes(XB_TLIST, 64) == 576 * sizeof(Xgl_pt_color_f3d));
    /* Strips of at most 8 triangles: a row of 32 is four calls of 10 points. */
    assert(run_probe(MOCK_CORRECT, XB_STRIP, 8, note));
    assert(strstr(note, "192 smooth triangles as 240 points in 24 triangle_strip call(s)"));
    assert(strip_calls == 24 && strip_points == 240 && !list_calls);
    assert(xb_backend_input_bytes(XB_STRIP, 8) == 240 * sizeof(Xgl_pt_color_f3d));
    assert(xb_backend_draw(XB_STRIP, 8) == 24 && drawn_count == TRIANGLES);
    /* A whole row at once. */
    assert(run_probe(MOCK_CORRECT, XB_STRIP, 32, note));
    assert(strstr(note, "192 smooth triangles as 204 points in 6 triangle_strip call(s)"));
    assert(strip_calls == 6 && strip_points == 204);
    /* Every strip point carries the color of the corner it is. */
    assert(b.strip_color_points && b.strip_corners && !b.strip_points);
    assert(b.strip_color_points[5].color.rgb.r == mesh_colors[b.strip_corners[5]].r);
    assert(b.strip_color_points[5].x == mesh_points[b.strip_corners[5]].x);
    /* A color update reaches the strip points as well as the corners. */
    mesh_colors[b.strip_corners[5]].r = .123f;
    xb_backend_colors(&mesh);
    assert(b.strip_color_points[5].color.rgb.r == .123f);
    build_mesh();
}

static void test_rejected_devices(void)
{
    static const struct { enum mock_fault fault; enum xb_method method; int batch; } wrong[] = {
        {MOCK_STRIP_AS_LIST, XB_STRIP, 8}, {MOCK_STRIP_AS_LIST, XB_STRIP, 32},
        {MOCK_STRIP_NOT_DRAWN, XB_STRIP, 8},
        {MOCK_LIST_AS_STRIP, XB_TLIST, 64},
        {MOCK_FLAT_COLOR, XB_STRIP, 8}, {MOCK_FLAT_COLOR, XB_TLIST, 64}
    };
    char note[240];
    size_t i;
    for (i = 0; i < sizeof(wrong) / sizeof(wrong[0]); ++i) {
        assert(!run_probe(wrong[i].fault, wrong[i].method, wrong[i].batch, note));
        assert(xb_backend_failure() == XB_FAILURE_VALIDATION);
        assert(strstr(b.error, "differs from independent polygons at pixel"));
    }
    /* A device that draws nothing cannot pass by matching an empty picture. */
    assert(!run_probe(MOCK_NOTHING_DRAWN, XB_STRIP, 8, note));
    assert(xb_backend_failure() == XB_FAILURE_VALIDATION);
    assert(strstr(b.error, "reference picture is mostly empty"));
    /* The probe is for unlit smooth colors only. */
    reset_mock(MOCK_CORRECT, XB_STRIP, 8);
    b.shading = XB_FLAT;
    assert(xb_backend_upload(&mesh));
    {
        xb_case test;
        memset(&test, 0, sizeof(test));
        test.method = XB_STRIP; test.probe = XB_PROBE_MESH; test.work = XB_DRAW;
        assert(!apply_probe(&test, note, sizeof(note)));
        assert(xb_backend_failure() == XB_FAILURE_UNSUPPORTED);
    }
    /* Strips that are not the mesh's triangles are refused at upload. */
    reset_mock(MOCK_CORRECT, XB_STRIP, 8);
    mesh_strip_points[3].x += .01f;
    assert(!xb_backend_upload(&mesh));
    assert(strstr(b.error, "strip points do not match"));
    build_mesh();
    reset_mock(MOCK_CORRECT, XB_STRIP, 8);
    mesh_strips[ROWS - 1].point_count += 2;
    assert(!xb_backend_upload(&mesh));
    assert(strstr(b.error, "strip description exceeds the mesh"));
    build_mesh();
}

/* The single-triangle gate every smooth case passes first reads a mixture
 * of three primary corners; a strip of three points must show the same. */
static void test_interpolation_gate(void)
{
    reset_mock(MOCK_CORRECT, XB_STRIP, 8);
    assert(validate_lighting());
    assert(strip_calls == 2 && readbacks == 2);
    reset_mock(MOCK_CORRECT, XB_TLIST, 64);
    assert(validate_lighting());
    assert(list_calls == 2);
    reset_mock(MOCK_FLAT_COLOR, XB_STRIP, 8);
    assert(!validate_lighting());
    assert(xb_backend_failure() == XB_FAILURE_VALIDATION);
}

int main(void)
{
    build_mesh();
    test_correct_device();
    test_rejected_devices();
    test_interpolation_gate();
    free_geometry();
    puts("native mesh probe tests passed (model device; no graphics/SDK validation)");
    return 0;
}
