/* Test double for portable CLI/report tests, NOT a graphics implementation.
 * GPL-2.0-or-later; see COPYING.
 */
#include "xglbench.h"
#include <stdlib.h>
#include <string.h>

static int test_width;
static int test_height;
static size_t test_triangles;
static size_t test_strips;
static unsigned long test_draws;
static unsigned long test_fault_draw;
static int test_fault_pending;
static int test_fault_fired;
static const char *test_fault;
static const char *test_error = "";

static void test_log(const char *operation, int width, int height)
{
    const char *path;
    FILE *file;
    path = getenv("XB_TEST_LOG");
    if (!path) return;
    file = fopen(path, "a");
    if (!file) return;
    fprintf(file, "%s %d %d\n", operation, width, height);
    fclose(file);
}

int xb_backend_open(const xb_options *options, int width, int height)
{
    const char *draw;
    (void)options;
    test_width = width;
    test_height = height;
    test_draws = 0;
    test_fault_pending = test_fault_fired = 0;
    test_error = "";
    test_fault = getenv("XB_TEST_FAULT");
    draw = getenv("XB_TEST_FAULT_DRAW");
    test_fault_draw = draw ? strtoul(draw, NULL, 10) : 5;
    test_log("open", width, height);
    return 1;
}

void xb_backend_close(void)
{
    test_log("close", test_width, test_height);
}

int xb_backend_resize(int width, int height)
{
    if (test_width == width && test_height == height) return 1;
    test_width = width;
    test_height = height;
    test_log("resize", width, height);
    return 1;
}

void xb_backend_dimensions(int *width, int *height)
{
    *width = test_width;
    *height = test_height;
}

void xb_backend_info(FILE *out)
{
    fprintf(out, "TEST DOUBLE: NO GRAPHICS, NO VALID HARDWARE PERFORMANCE DATA\n");
    fprintf(out, "Portable scheduling/report tests only; window=%dx%d\n",
            test_width, test_height);
}

int xb_backend_validate(enum xb_method method, char *reason, size_t size)
{
    const char *failed;
    failed = getenv("XB_TEST_FAIL_METHOD");
    if (failed && !strcmp(failed, xb_method_name(method))) {
        if (size) {
            strncpy(reason, "injected test-only validation failure", size - 1);
            reason[size - 1] = '\0';
        }
        return 0;
    }
    if (size) reason[0] = '\0';
    return 1;
}

int xb_backend_configure(const xb_case *test, const xb_mesh *mesh,
                         char *reason, size_t size)
{
    (void)test;
    if (size) reason[0] = '\0';
    return xb_backend_upload(mesh);
}

int xb_backend_upload(const xb_mesh *mesh)
{
    test_triangles = mesh->triangles;
    test_strips = mesh->strip_count;
    return 1;
}

void xb_backend_colors(const xb_mesh *mesh) { (void)mesh; }
void xb_backend_transform(unsigned long frame) { (void)frame; }
void xb_backend_clear(int color, int depth) { (void)color; (void)depth; }

size_t xb_backend_draw(enum xb_method method, int batch)
{
    double until;
    ++test_draws;
    if (test_fault && !test_fault_fired) {
        /* Force the .001-second regression trial to end at its minimum
         * frame count. This is test scheduling, not simulated GPU work. */
        until = xb_now() + .002;
        while (xb_now() < until) { }
        if (test_draws >= test_fault_draw) {
            test_fault_pending = test_fault_fired = 1;
            test_log(test_fault, test_width, test_height);
        }
    }
    if (method == XB_POLYGON) return test_triangles;
    if (method == XB_STRIP) return test_strips;
    if (batch < 1) batch = 1;
    return (test_triangles + (size_t)batch - 1) / (size_t)batch;
}

void xb_backend_sync(void) { }
void xb_backend_swap(void) { }
void xb_backend_raster(int masked) { (void)masked; }
int xb_backend_events(void)
{
    if (!test_fault_pending) return 1;
    test_fault_pending = 0;
    if (!strcmp(test_fault, "native-error")) {
        test_error = "injected native completion failure";
        return 0;
    }
    if (!strcmp(test_fault, "resize")) {
        test_width += 16;
        return -1;
    }
    if (!strcmp(test_fault, "expose")) return -1;
    if (!strcmp(test_fault, "interrupt")) return 0;
    return 1;
}
void xb_backend_label(const char *name) { (void)name; }
int xb_backend_has_double_buffer(void) { return 1; }
int xb_backend_has_hardware(void) { return 0; }
const char *xb_backend_error(void) { return test_error; }
