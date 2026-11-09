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
static int test_batch = 128;
static int test_zbuffer;

typedef struct {
    xb_rgb *colors;
    unsigned long checksum;
    int pending;
} test_slot;

static test_slot test_slots[XB_MAX_QUEUE];
static int test_slot_count;
static int test_outstanding;
static int test_peak;
static size_t test_stream_triangles;
static unsigned long test_submitted;
static unsigned long test_completed;
static unsigned long test_nonempty_drains;
static unsigned long test_fail_drain;
static double test_drain_delay;
static int test_stream_bad;
static int test_stream_fail(const char *error);

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
    const char *draw, *delay, *drain;
    (void)options;
    test_width = width;
    test_height = height;
    test_draws = 0;
    test_fault_pending = test_fault_fired = 0;
    test_error = "";
    test_stream_bad = 0;
    test_slot_count = test_outstanding = test_peak = 0;
    test_submitted = test_completed = test_nonempty_drains = 0;
    memset(test_slots, 0, sizeof(test_slots));
    delay = getenv("XB_TEST_DRAIN_DELAY_MS");
    test_drain_delay = delay ? atof(delay) * .001 : 0;
    drain = getenv("XB_TEST_FAIL_DRAIN");
    test_fail_drain = drain ? strtoul(drain, NULL, 10) : 0;
    test_fault = getenv("XB_TEST_FAULT");
    draw = getenv("XB_TEST_FAULT_DRAW");
    test_fault_draw = draw ? strtoul(draw, NULL, 10) : 5;
    test_log("open", width, height);
    return 1;
}

void xb_backend_close(void)
{
    int i;
    for (i = 0; i < test_slot_count; ++i) free(test_slots[i].colors);
    memset(test_slots, 0, sizeof(test_slots));
    test_slot_count = 0;
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
    test_batch = test->batch > 0 ? test->batch : 128;
    test_zbuffer = test->zbuffer;
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
void xb_backend_clear(int color, int depth)
{
    if (test_slot_count) test_log("queue-clear", color, depth);
}

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

void xb_backend_sync(void)
{
    if (test_outstanding)
        test_stream_fail("stream work synchronized outside its completion drain");
}
void xb_backend_swap(void)
{
    if (test_slot_count) test_stream_fail("streaming test unexpectedly swapped");
}
void xb_backend_raster(int masked)
{
    (void)masked;
    if (test_slot_count) test_stream_fail("streaming test unexpectedly drew a HUD");
}
int xb_backend_events(void)
{
    if (test_stream_bad) return 0;
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

size_t xb_backend_input_bytes(enum xb_method method, int batch)
{
    size_t points, facets;
    (void)batch;
    /* A test-double payload estimate only, never native transfer bandwidth. */
    points = method == XB_STRIP ? test_triangles + test_strips * 2 : test_triangles * 3;
    facets = method == XB_TLIST ? test_triangles * 3 : test_triangles;
    return points * sizeof(xb_point) + facets * sizeof(xb_rgb);
}

static int test_stream_fail(const char *error)
{
    test_error = error;
    test_stream_bad = 1;
    test_log("queue-error", test_outstanding, test_slot_count);
    return 0;
}

static unsigned long test_checksum(const xb_rgb *colors, size_t count)
{
    const unsigned char *bytes;
    size_t i;
    unsigned long checksum;
    bytes = (const unsigned char *)colors;
    checksum = 0;
    for (i = 0; i < count * sizeof(*colors); ++i)
        checksum = checksum * 33UL + bytes[i];
    return checksum;
}

int xb_backend_stream_begin(const xb_mesh *mesh, int slots, char *reason, size_t size)
{
    int i;
    if (size) reason[0] = '\0';
    if (!mesh || !mesh->triangles || !mesh->colors || slots < 1 || slots > XB_MAX_QUEUE)
        return test_stream_fail("invalid mock stream setup");
    if (test_slot_count || test_outstanding)
        return test_stream_fail("previous mock stream was not released");
    test_stream_triangles = mesh->triangles;
    test_outstanding = test_peak = 0;
    test_submitted = test_completed = test_nonempty_drains = 0;
    for (i = 0; i < slots; ++i) {
        test_slots[i].colors = (xb_rgb *)malloc(mesh->triangles * sizeof(xb_rgb));
        if (!test_slots[i].colors) {
            test_slot_count = i;
            xb_backend_stream_end();
            return test_stream_fail("mock stream allocation failed");
        }
        memcpy(test_slots[i].colors, mesh->colors, mesh->triangles * sizeof(xb_rgb));
        test_slots[i].pending = 0;
    }
    test_slot_count = slots;
    test_log("queue-begin", slots, (int)mesh->triangles);
    return 1;
}

int xb_backend_stream_prepare(int slot, const xb_mesh *mesh)
{
    if (slot < 0 || slot >= test_slot_count || !mesh ||
        mesh->triangles != test_stream_triangles || !mesh->colors)
        return test_stream_fail("invalid mock stream slot");
    if (test_slots[slot].pending)
        return test_stream_fail("in-flight stream slot overwritten before drain");
    /* The source may be reused immediately. No allocation occurs here. */
    memcpy(test_slots[slot].colors, mesh->colors, mesh->triangles * sizeof(xb_rgb));
    test_log("queue-prepare", slot, test_outstanding);
    return 1;
}

size_t xb_backend_stream_draw(int slot)
{
    if (slot < 0 || slot >= test_slot_count || test_slots[slot].pending) {
        test_stream_fail("invalid or still-pending mock stream draw slot");
        return 0;
    }
    xb_backend_clear(1, test_zbuffer);
    test_slots[slot].pending = 1;
    test_slots[slot].checksum = test_checksum(test_slots[slot].colors, test_stream_triangles);
    ++test_outstanding;
    ++test_submitted;
    if (test_outstanding > test_peak) test_peak = test_outstanding;
    test_log("queue-draw", slot, test_outstanding);
    if (test_outstanding > test_slot_count) {
        test_stream_fail("mock stream queue exceeded its limit");
        return 0;
    }
    /* Reuse the existing final-draw fault injection without completing work. */
    return xb_backend_draw(XB_MULTI, test_batch);
}

void xb_backend_post(void)
{
    test_log("queue-post", test_outstanding, (int)test_completed);
}

int xb_backend_stream_drain(void)
{
    double until;
    int i;
    if (test_outstanding) {
        ++test_nonempty_drains;
        if (test_fail_drain && test_nonempty_drains == test_fail_drain) {
            test_log("queue-drain-failed", test_outstanding, (int)test_nonempty_drains);
            return test_stream_fail("injected stream completion failure");
        }
        /* A controlled delay detects timers stopped before completion. It is
         * deliberately not a device performance simulation. */
        until = xb_now() + test_drain_delay;
        while (xb_now() < until) { }
    }
    for (i = 0; i < test_slot_count; ++i) {
        if (!test_slots[i].pending) continue;
        if (test_slots[i].checksum != test_checksum(test_slots[i].colors, test_stream_triangles))
            return test_stream_fail("native colors changed while mock queue retained them");
        test_slots[i].pending = 0;
        ++test_completed;
    }
    test_log("queue-drain", test_outstanding, (int)test_completed);
    test_outstanding = 0;
    return 1;
}

void xb_backend_stream_end(void)
{
    int i;
    if (!test_slot_count) return;
    /* Mirrors native cleanup: finish retained data before releasing it. */
    if (test_outstanding && !xb_backend_stream_drain()) return;
    test_log("queue-end", (int)test_submitted, (int)test_completed);
    for (i = 0; i < test_slot_count; ++i) free(test_slots[i].colors);
    memset(test_slots, 0, sizeof(test_slots));
    test_slot_count = 0;
}
