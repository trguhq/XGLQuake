/* Portable geometry invariants. GPL-2.0-or-later; see ../COPYING. */
#include "../xglbench.h"

#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int same_point(const xb_point *a, const xb_point *b)
{
    return a->x == b->x && a->y == b->y && a->z == b->z;
}

static double twice_area(const xb_point *a, const xb_point *b,
                         const xb_point *c)
{
    return ((double)b->x - a->x) * ((double)c->y - a->y)
         - ((double)b->y - a->y) * ((double)c->x - a->x);
}

static void check_color(const xb_rgb *color)
{
    assert(color->r >= 0.0f && color->r <= 1.0f);
    assert(color->g >= 0.0f && color->g <= 1.0f);
    assert(color->b >= 0.0f && color->b <= 1.0f);
}

/* All submission paths must describe the same triangles with the same
 * orientation and facet order. Otherwise a throughput comparison is invalid.
 */
static void check_mesh(const xb_mesh *mesh, int base, int depth,
                       int require_xy_bounds, int connected)
{
    size_t i, j, first_point, first_facet, n, bytes, columns, rows, patches;
    const xb_strip *strip;
    const xb_point *a, *b, *c, *triangle;
    double area;

    n = (size_t)1 << depth;
    columns = n;
    rows = (size_t)(base / 2) * n;
    if (connected) {
        patches = (size_t)(base / 2);
        columns = (size_t)sqrt((double)patches);
        while (patches % columns != 0)
            --columns;
        rows = columns * n;
        columns = (patches / columns) * n;
    }
    assert(mesh->triangles == (size_t)base * n * n);
    assert(mesh->point_count == 3 * mesh->triangles);
    assert(mesh->strip_count == rows);
    assert(mesh->strip_point_count == mesh->strip_count * 2 * (columns + 1));
    assert(mesh->points != NULL && mesh->strip_points != NULL);
    assert(mesh->colors != NULL && mesh->base_colors != NULL);
    assert(mesh->strips != NULL);

    bytes = mesh->point_count * sizeof(*mesh->points)
          + mesh->strip_point_count * sizeof(*mesh->strip_points)
          + 2 * mesh->triangles * sizeof(*mesh->colors)
          + mesh->strip_count * sizeof(*mesh->strips);
    assert(mesh->bytes == bytes);

    first_point = 0;
    first_facet = 0;
    for (i = 0; i < mesh->strip_count; ++i) {
        strip = &mesh->strips[i];
        assert(strip->first_point == first_point);
        assert(strip->first_facet == first_facet);
        assert(strip->point_count == 2 * (columns + 1));
        assert(strip->first_point + strip->point_count
               <= mesh->strip_point_count);
        assert(strip->first_facet + strip->point_count - 2
               <= mesh->triangles);
        for (j = 0; j + 2 < strip->point_count; ++j) {
            a = &mesh->strip_points[strip->first_point + j];
            b = &mesh->strip_points[strip->first_point + j + 1];
            c = &mesh->strip_points[strip->first_point + j + 2];
            if (j & 1) {
                a = &mesh->strip_points[strip->first_point + j + 1];
                b = &mesh->strip_points[strip->first_point + j];
            }
            triangle = &mesh->points[(strip->first_facet + j) * 3];
            /* A cyclic rotation is equivalent; an orientation reversal is not. */
            assert((same_point(a, &triangle[0]) &&
                    same_point(b, &triangle[1]) &&
                    same_point(c, &triangle[2])) ||
                   (same_point(a, &triangle[1]) &&
                    same_point(b, &triangle[2]) &&
                    same_point(c, &triangle[0])) ||
                   (same_point(a, &triangle[2]) &&
                    same_point(b, &triangle[0]) &&
                    same_point(c, &triangle[1])));
            area = twice_area(&triangle[0], &triangle[1], &triangle[2]);
            assert(area > 0.0);
        }
        first_point += strip->point_count;
        first_facet += strip->point_count - 2;
    }
    assert(first_point == mesh->strip_point_count);
    assert(first_facet == mesh->triangles);

    for (i = 0; i < mesh->point_count; ++i) {
        a = &mesh->points[i];
        assert(fabs((double)a->x) <= FLT_MAX);
        assert(fabs((double)a->y) <= FLT_MAX);
        assert(fabs((double)a->z) <= FLT_MAX);
        if (require_xy_bounds) {
            assert(a->x >= -0.900001f && a->x <= 0.900001f);
            assert(a->y >= -0.900001f && a->y <= 0.900001f);
        }
    }
    for (i = 0; i < mesh->triangles; ++i) {
        check_color(&mesh->colors[i]);
        check_color(&mesh->base_colors[i]);
    }
}

static double mesh_area(const xb_mesh *mesh)
{
    size_t i;
    double area;
    area = 0.0;
    for (i = 0; i < mesh->point_count; i += 3)
        area += twice_area(&mesh->points[i], &mesh->points[i + 1],
                           &mesh->points[i + 2]) * 0.5;
    return area;
}

static void test_geometry(void)
{
    int base, depth;
    xb_mesh mesh;
    double reference_area, area;

    memset(&mesh, 0, sizeof(mesh));
    for (base = 2; base <= 34; base += 2) {
        reference_area = 0.0;
        for (depth = 0; depth <= 2; ++depth) {
            assert(xb_mesh_build(&mesh, base, depth, 0.75f, 0));
            check_mesh(&mesh, base, depth, 1, 0);
            area = mesh_area(&mesh);
            if (depth == 0)
                reference_area = area;
            else
                assert(fabs(area - reference_area) < 0.000001);
        }
    }
    assert(xb_mesh_build(&mesh, 512, 2, 1.0f, 0));
    check_mesh(&mesh, 512, 2, 1, 0);
    assert(xb_mesh_build(&mesh, 32, 1, 4.0f, 1));
    check_mesh(&mesh, 32, 1, 0, 0);
    xb_mesh_free(&mesh);
    assert(mesh.points == NULL && mesh.strip_points == NULL);
    assert(mesh.colors == NULL && mesh.base_colors == NULL);
    assert(mesh.strips == NULL && mesh.bytes == 0);
    assert(mesh.triangles == 0 && mesh.point_count == 0);
    assert(mesh.strip_point_count == 0 && mesh.strip_count == 0);
    xb_mesh_free(&mesh);
}

static void test_color_updates(void)
{
    xb_mesh first, second;
    xb_rgb *snapshot, *original;
    size_t bytes, i;

    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    assert(xb_mesh_build(&first, 18, 2, 0.75f, 0));
    assert(xb_mesh_build(&second, 18, 2, 0.75f, 0));
    bytes = first.triangles * sizeof(*first.colors);
    assert(first.bytes == second.bytes);
    assert(memcmp(first.points, second.points,
                  first.point_count * sizeof(*first.points)) == 0);
    assert(memcmp(first.colors, second.colors, bytes) == 0);

    snapshot = (xb_rgb *)malloc(bytes);
    original = (xb_rgb *)malloc(bytes);
    assert(snapshot != NULL && original != NULL);
    memcpy(original, first.base_colors, bytes);
    xb_mesh_colors(&first, 17);
    memcpy(snapshot, first.colors, bytes);
    xb_mesh_colors(&first, 63);
    assert(memcmp(snapshot, first.colors, bytes) != 0);
    xb_mesh_colors(&first, 17);
    assert(memcmp(snapshot, first.colors, bytes) == 0);
    xb_mesh_colors(&second, 17);
    assert(memcmp(first.colors, second.colors, bytes) == 0);
    assert(memcmp(original, first.base_colors, bytes) == 0);
    for (i = 0; i < first.triangles; ++i)
        check_color(&first.colors[i]);
    free(snapshot);
    free(original);
    xb_mesh_free(&first);
    xb_mesh_free(&second);
}

static void test_invalid_input(void)
{
    xb_mesh mesh, before;
    memset(&mesh, 0, sizeof(mesh));
    assert(xb_mesh_expected_triangles(2, 0) == 2);
    assert(xb_mesh_expected_triangles(512, 1) == 2048);
    assert(xb_mesh_expected_triangles(512, 2) == 8192);
    assert(xb_mesh_expected_triangles(0, 0) == 0);
    assert(xb_mesh_expected_triangles(-2, 0) == 0);
    assert(xb_mesh_expected_triangles(3, 0) == 0);
    assert(xb_mesh_expected_triangles(2, -1) == 0);
    assert(xb_mesh_expected_triangles(2, 3) == 0);
    assert(xb_mesh_build(&mesh, 8, 1, 0.75f, 0));
    before = mesh;
    assert(!xb_mesh_build(&mesh, 0, 0, 0.75f, 0));
    assert(!xb_mesh_build(&mesh, -2, 0, 0.75f, 0));
    assert(!xb_mesh_build(&mesh, 3, 0, 0.75f, 0));
    assert(!xb_mesh_build(&mesh, 8, -1, 0.75f, 0));
    assert(!xb_mesh_build(&mesh, 8, 3, 0.75f, 0));
    assert(!xb_mesh_build(&mesh, 8, 0, 0.0f, 0));
    assert(!xb_mesh_build(&mesh, 8, 0, -1.0f, 0));
    assert(!xb_mesh_build(&mesh, 8, 0, 17.0f, 0));
    assert(!xb_mesh_build(&mesh, INT_MAX - 1, 2, 0.75f, 0));
    assert(memcmp(&mesh, &before, sizeof(mesh)) == 0);
    check_mesh(&mesh, 8, 1, 1, 0);
    xb_mesh_free(&mesh);
}

static void test_connected_geometry(void)
{
    const int bases[] = { 2, 6, 14, 128 };
    xb_mesh mesh, before, duplicate;
    size_t i, j;
    int depth;

    memset(&mesh, 0, sizeof(mesh));
    memset(&duplicate, 0, sizeof(duplicate));
    /* Start with a disconnected mesh to exercise replacement across layouts. */
    assert(xb_mesh_build(&mesh, 8, 1, 0.75f, 0));
    for (i = 0; i < sizeof(bases) / sizeof(bases[0]); ++i) {
        for (depth = 0; depth <= 2; ++depth) {
            assert(xb_mesh_build_connected(&mesh, bases[i], depth));
            check_mesh(&mesh, bases[i], depth, 1, 1);
            assert(fabs(mesh_area(&mesh) - 3.24) < 0.000001);
            for (j = 0; j < mesh.point_count; ++j)
                assert(mesh.points[j].z == 0.5f);
        }
    }
    /* The 128-base-triangle surface has 32 strips of 64 triangles at depth 2. */
    assert(mesh.strip_count == 32);
    assert(mesh.strips[0].point_count == 66);
    assert(xb_mesh_build_connected(&duplicate, 128, 2));
    assert(mesh.bytes == duplicate.bytes);
    assert(memcmp(mesh.points, duplicate.points,
                  mesh.point_count * sizeof(*mesh.points)) == 0);
    assert(memcmp(mesh.base_colors, duplicate.base_colors,
                  mesh.triangles * sizeof(*mesh.base_colors)) == 0);

    before = mesh;
    assert(!xb_mesh_build_connected(&mesh, 0, 0));
    assert(!xb_mesh_build_connected(&mesh, -2, 0));
    assert(!xb_mesh_build_connected(&mesh, 3, 0));
    assert(!xb_mesh_build_connected(&mesh, 8, -1));
    assert(!xb_mesh_build_connected(&mesh, 8, 3));
    assert(!xb_mesh_build_connected(&mesh, INT_MAX - 1, 2));
    assert(memcmp(&mesh, &before, sizeof(mesh)) == 0);
    check_mesh(&mesh, 128, 2, 1, 1);
    xb_mesh_free(&mesh);
    xb_mesh_free(&duplicate);
}

/* The scenes use coplanar triangles. A small CPU visibility probe verifies
 * that reversal preserves the nearest visible facet, not just its vertices.
 */
static int nearest_color(const xb_mesh *mesh, float x, float y,
                         xb_rgb *color, float *depth)
{
    xb_point probe;
    const xb_point *triangle;
    size_t i;
    int found;

    probe.x = x;
    probe.y = y;
    probe.z = 0.0f;
    found = 0;
    *depth = FLT_MAX;
    for (i = 0; i < mesh->triangles; ++i) {
        triangle = &mesh->points[i * 3];
        if (twice_area(&triangle[0], &triangle[1], &probe) >= 0.0 &&
            twice_area(&triangle[1], &triangle[2], &probe) >= 0.0 &&
            twice_area(&triangle[2], &triangle[0], &probe) >= 0.0 &&
            triangle[0].z < *depth) {
            found = 1;
            *depth = triangle[0].z;
            *color = mesh->colors[i];
        }
    }
    return found;
}

static void check_reversed_groups(const xb_mesh *original,
                                  const xb_mesh *reversed)
{
    const xb_strip *source, *destination;
    size_t i, facets;

    assert(original->triangles == reversed->triangles);
    assert(original->strip_count == reversed->strip_count);
    assert(original->point_count == reversed->point_count);
    assert(original->strip_point_count == reversed->strip_point_count);
    assert(original->bytes == reversed->bytes);
    for (i = 0; i < original->strip_count; ++i) {
        source = &original->strips[original->strip_count - 1 - i];
        destination = &reversed->strips[i];
        assert(source->point_count == destination->point_count);
        facets = source->point_count - 2;
        assert(memcmp(original->strip_points + source->first_point,
                      reversed->strip_points + destination->first_point,
                      source->point_count * sizeof(*original->strip_points))
               == 0);
        assert(memcmp(original->points + source->first_facet * 3,
                      reversed->points + destination->first_facet * 3,
                      facets * 3 * sizeof(*original->points)) == 0);
        assert(memcmp(original->colors + source->first_facet,
                      reversed->colors + destination->first_facet,
                      facets * sizeof(*original->colors)) == 0);
        assert(memcmp(original->base_colors + source->first_facet,
                      reversed->base_colors + destination->first_facet,
                      facets * sizeof(*original->base_colors)) == 0);
    }
}

static void check_identical_mesh(const xb_mesh *a, const xb_mesh *b)
{
    assert(a->triangles == b->triangles && a->point_count == b->point_count);
    assert(a->strip_count == b->strip_count);
    assert(a->strip_point_count == b->strip_point_count);
    assert(a->bytes == b->bytes);
    assert(memcmp(a->points, b->points,
                  a->point_count * sizeof(*a->points)) == 0);
    assert(memcmp(a->strip_points, b->strip_points,
                  a->strip_point_count * sizeof(*a->strip_points)) == 0);
    assert(memcmp(a->colors, b->colors,
                  a->triangles * sizeof(*a->colors)) == 0);
    assert(memcmp(a->base_colors, b->base_colors,
                  a->triangles * sizeof(*a->base_colors)) == 0);
    assert(memcmp(a->strips, b->strips,
                  a->strip_count * sizeof(*a->strips)) == 0);
}

static void test_reverse_draw_order(void)
{
    xb_mesh original, reversed, empty, saved;
    xb_rgb first_color, second_color;
    float first_depth, second_depth;
    size_t i;
    int connected, depth;

    memset(&original, 0, sizeof(original));
    memset(&reversed, 0, sizeof(reversed));
    memset(&empty, 0, sizeof(empty));
    saved = empty;
    assert(!xb_mesh_reverse_draw_order(NULL));
    assert(!xb_mesh_reverse_draw_order(&empty));
    assert(memcmp(&empty, &saved, sizeof(empty)) == 0);
    for (connected = 0; connected <= 1; ++connected) {
        for (depth = 0; depth <= 2; ++depth) {
            if (connected) {
                assert(xb_mesh_build_connected(&original, 128, depth));
                assert(xb_mesh_build_connected(&reversed, 128, depth));
            } else {
                assert(xb_mesh_build(&original, 128, depth, 0.75f, 1));
                assert(xb_mesh_build(&reversed, 128, depth, 0.75f, 1));
            }
            xb_mesh_colors(&original, 17);
            xb_mesh_colors(&reversed, 17);
            assert(xb_mesh_reverse_draw_order(&reversed));
            check_mesh(&reversed, 128, depth, 1, connected);
            check_reversed_groups(&original, &reversed);
            if (!connected) {
                for (i = 3; i < original.point_count; i += 3) {
                    assert(original.points[i - 3].z <= original.points[i].z);
                    assert(reversed.points[i - 3].z >= reversed.points[i].z);
                }
                assert(nearest_color(&original, -0.31f, 0.13f,
                                     &first_color, &first_depth));
                assert(nearest_color(&reversed, -0.31f, 0.13f,
                                     &second_color, &second_depth));
                assert(first_depth == second_depth);
                assert(memcmp(&first_color, &second_color,
                              sizeof(first_color)) == 0);
            }
            assert(xb_mesh_reverse_draw_order(&reversed));
            check_identical_mesh(&original, &reversed);
        }
    }
    /* A rejected strip descriptor must not free or partially reorder a mesh. */
    i = reversed.strips[0].first_facet;
    reversed.strips[0].first_facet = reversed.triangles + 1;
    saved = reversed;
    assert(!xb_mesh_reverse_draw_order(&reversed));
    assert(memcmp(&reversed, &saved, sizeof(reversed)) == 0);
    assert(reversed.strips[0].first_facet == reversed.triangles + 1);
    reversed.strips[0].first_facet = i;
    check_identical_mesh(&original, &reversed);
    xb_mesh_free(&original);
    xb_mesh_free(&reversed);
}

static void test_calibrated_geometry(void)
{
    const double areas[] = { 4.0, 16.0, 64.0, 100.0, 256.0, 1024.0 };
    const double aspects[] = { 1.0, 16.0, 1.0 / 16.0 };
    const int widths[] = { 320, 640 };
    const int heights[] = { 240, 480 };
    xb_mesh mesh, duplicate, before;
    const xb_point *points;
    size_t i, j, k, p;
    double mean, minimum, maximum, span_x, span_y, box_width, box_height;
    double nonfinite;

    memset(&mesh, 0, sizeof(mesh));
    memset(&duplicate, 0, sizeof(duplicate));
    for (i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i) {
        span_x = (widths[i] - 1) * 0.5;
        span_y = (heights[i] - 1) * 0.5;
        for (j = 0; j < sizeof(areas) / sizeof(areas[0]); ++j) {
            for (k = 0; k < sizeof(aspects) / sizeof(aspects[0]); ++k) {
                assert(xb_mesh_build_calibrated(&mesh, 512, widths[i], heights[i],
                                                areas[j], aspects[k]));
                check_mesh(&mesh, 512, 0, 0, 0);
                assert(xb_mesh_projected_area(&mesh, widths[i], heights[i],
                                              &mean, &minimum, &maximum));
                assert(fabs(mean - areas[j]) < areas[j] * 0.0001);
                assert(fabs(minimum - areas[j]) < areas[j] * 0.0001);
                assert(fabs(maximum - areas[j]) < areas[j] * 0.0001);
                for (p = 0; p < mesh.point_count; ++p) {
                    assert(mesh.points[p].x >= -1.0f && mesh.points[p].x <= 1.0f);
                    assert(mesh.points[p].y >= -1.0f && mesh.points[p].y <= 1.0f);
                    assert(mesh.points[p].z == 0.5f);
                }
                for (p = 0; p < mesh.strip_count; ++p) {
                    points = mesh.strip_points + mesh.strips[p].first_point;
                    box_width = ((double)points[2].x - points[0].x) * span_x;
                    box_height = ((double)points[0].y - points[1].y) * span_y;
                    assert(fabs(box_width / box_height - aspects[k]) <
                           aspects[k] * 0.0001);
                }
                assert(xb_mesh_build_calibrated(&duplicate, 512, widths[i],
                                                heights[i], areas[j], aspects[k]));
                check_identical_mesh(&mesh, &duplicate);
            }
        }
    }
    /* This area fills the tiny viewport with one quad; repeated patches must
     * retain their area, count, winding and depth instead of being shrunk. */
    assert(xb_mesh_build_calibrated(&mesh, 514, 21, 11, 100.0, 2.0));
    check_mesh(&mesh, 514, 0, 0, 0);
    assert(xb_mesh_projected_area(&mesh, 21, 11, &mean, &minimum, &maximum));
    assert(mean == 100.0 && minimum == 100.0 && maximum == 100.0);
    for (p = 4; p < mesh.strip_point_count; ++p)
        assert(same_point(&mesh.strip_points[p], &mesh.strip_points[p % 4]));

    assert(xb_mesh_build_calibrated(&mesh, 512, 320, 240, 100.0, 1.0));
    assert(xb_mesh_build_calibrated(&duplicate, 512, 640, 480, 100.0, 1.0));
    /* A resize changes positions, never the workload's facet-color stream. */
    assert(memcmp(mesh.base_colors, duplicate.base_colors,
                  mesh.triangles * sizeof(*mesh.base_colors)) == 0);
    assert(xb_mesh_build_calibrated(&duplicate, 512, 320, 240, 100.0, 1.0));
    before = mesh;
    assert(!xb_mesh_build_calibrated(NULL, 512, 320, 240, 100.0, 1.0));
    assert(!xb_mesh_build_calibrated(&mesh, 0, 320, 240, 100.0, 1.0));
    assert(!xb_mesh_build_calibrated(&mesh, -2, 320, 240, 100.0, 1.0));
    assert(!xb_mesh_build_calibrated(&mesh, 3, 320, 240, 100.0, 1.0));
    assert(!xb_mesh_build_calibrated(&mesh, INT_MAX - 1, 320, 240, 100.0, 1.0));
    assert(!xb_mesh_build_calibrated(&mesh, 512, 1, 240, 100.0, 1.0));
    assert(!xb_mesh_build_calibrated(&mesh, 512, 320, 0, 100.0, 1.0));
    assert(!xb_mesh_build_calibrated(&mesh, 512, 320, 240, 0.0, 1.0));
    assert(!xb_mesh_build_calibrated(&mesh, 512, 320, 240, -1.0, 1.0));
    assert(!xb_mesh_build_calibrated(&mesh, 512, 320, 240, 100.0, 0.0));
    assert(!xb_mesh_build_calibrated(&mesh, 512, 320, 240, 100.0, -1.0));
    assert(!xb_mesh_build_calibrated(&mesh, 512, 320, 240, DBL_MAX, 1.0));
    assert(!xb_mesh_build_calibrated(&mesh, 512, 320, 240, 100.0, DBL_MAX));
    assert(!xb_mesh_build_calibrated(&mesh, 512, 320, 240, 100.0, DBL_MIN));
    assert(!xb_mesh_build_calibrated(&mesh, 512, 20, 20, 100.0, 16.0));
    assert(!xb_mesh_build_calibrated(&mesh, 512, 20, 20, 100.0, 1.0 / 16.0));
    nonfinite = strtod("nan", NULL);
    if (nonfinite != nonfinite) {
        assert(!xb_mesh_build_calibrated(&mesh, 512, 320, 240, nonfinite, 1.0));
        assert(!xb_mesh_build_calibrated(&mesh, 512, 320, 240, 100.0, nonfinite));
    }
    nonfinite = strtod("inf", NULL);
    if (nonfinite > DBL_MAX) {
        assert(!xb_mesh_build_calibrated(&mesh, 512, 320, 240, nonfinite, 1.0));
        assert(!xb_mesh_build_calibrated(&mesh, 512, 320, 240, 100.0, nonfinite));
    }
    assert(memcmp(&mesh, &before, sizeof(mesh)) == 0);
    check_identical_mesh(&mesh, &duplicate);
    xb_mesh_free(&mesh);
    xb_mesh_free(&duplicate);
}

static void test_projected_area(void)
{
    xb_mesh mesh;
    xb_point points[6], temporary;
    double mean, minimum, maximum, nonfinite;
    size_t i;

    memset(&mesh, 0, sizeof(mesh));
    memset(points, 0, sizeof(points));
    mesh.triangles = 2;
    mesh.point_count = 6;
    mesh.points = points;
    points[0].x = points[0].y = -2.0f;
    points[1].x = 2.0f;
    points[1].y = -2.0f;
    points[2].x = -2.0f;
    points[2].y = 2.0f;
    /* One triangle extends beyond the viewport, the other is degenerate.
     * A summary describes raw projected area, without clipping or culling. */
    assert(xb_mesh_projected_area(&mesh, 5, 3, &mean, &minimum, &maximum));
    assert(mean == 8.0 && minimum == 0.0 && maximum == 16.0);
    temporary = points[1];
    points[1] = points[2];
    points[2] = temporary;
    assert(xb_mesh_projected_area(&mesh, 5, 3, &mean, &minimum, &maximum));
    assert(mean == 8.0 && minimum == 0.0 && maximum == 16.0);
    assert(!xb_mesh_projected_area(NULL, 5, 3, &mean, &minimum, &maximum));
    assert(!xb_mesh_projected_area(&mesh, 1, 3, &mean, &minimum, &maximum));
    assert(!xb_mesh_projected_area(&mesh, 5, 1, &mean, &minimum, &maximum));
    assert(!xb_mesh_projected_area(&mesh, 5, 3, NULL, &minimum, &maximum));
    assert(!xb_mesh_projected_area(&mesh, 5, 3, &mean, NULL, &maximum));
    assert(!xb_mesh_projected_area(&mesh, 5, 3, &mean, &minimum, NULL));
    mesh.point_count = 5;
    assert(!xb_mesh_projected_area(&mesh, 5, 3, &mean, &minimum, &maximum));
    mesh.point_count = 6;
    mesh.triangles = 0;
    assert(!xb_mesh_projected_area(&mesh, 5, 3, &mean, &minimum, &maximum));
    mesh.triangles = (size_t)-1;
    assert(!xb_mesh_projected_area(&mesh, 5, 3, &mean, &minimum, &maximum));
    mesh.triangles = 2;
    mesh.points = NULL;
    assert(!xb_mesh_projected_area(&mesh, 5, 3, &mean, &minimum, &maximum));
    mesh.points = points;
    for (i = 0; i < 2; ++i) {
        nonfinite = strtod(i ? "inf" : "nan", NULL);
        if (nonfinite != nonfinite || nonfinite > DBL_MAX) {
            temporary = points[5];
            points[5].x = (float)nonfinite;
            assert(!xb_mesh_projected_area(&mesh, 5, 3, &mean, &minimum, &maximum));
            points[5] = temporary;
            points[5].y = (float)nonfinite;
            assert(!xb_mesh_projected_area(&mesh, 5, 3, &mean, &minimum, &maximum));
            points[5] = temporary;
            points[5].z = (float)nonfinite;
            assert(!xb_mesh_projected_area(&mesh, 5, 3, &mean, &minimum, &maximum));
            points[5] = temporary;
        }
    }
    /* Failures leave all three outputs unchanged. */
    assert(mean == 8.0 && minimum == 0.0 && maximum == 16.0);
}

int main(void)
{
    int method;
    test_geometry();
    test_color_updates();
    test_invalid_input();
    test_connected_geometry();
    test_reverse_draw_order();
    test_calibrated_geometry();
    test_projected_area();
    for (method = 0; method < XB_METHODS; ++method)
        assert(xb_method_name((enum xb_method)method)[0] != '\0');
    puts("scene tests: geometry equivalence, subdivision coverage, color caches,"
         " calibrated pixel areas, allocation accounting and invalid inputs passed");
    return 0;
}
