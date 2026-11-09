/* Native Sun XGL 3.x backend, GPL-2.0-or-later. See COPYING.
 * API references: Sun XGL Reference Manual 802-5864, printed pp. 37-42,
 * 79-80, 101, 174, 204-205, 228-233, 274-277, 333-334, 387-400,
 * 417-424, 457-459, 467-470, 515, 552-567. No OpenGL is used.
 */
#include "xglbench.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <xgl/xgl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

#define XB_RASTER_W 128
#define XB_RASTER_H 64

/* Every pointer reachable by a retained draw remains stable until completion.
 * Keeping separate native copies also prevents CPU color preparation from
 * overwriting geometry still being consumed by the accelerator.
 */
typedef struct {
    Xgl_pt_f3d *points;
    Xgl_pt_list *lists;
    Xgl_color_facet *facets;
    Xgl_facet_list *batch_facets;
    size_t triangles, point_count;
    int pending, prepared;
} xb_native_slot;

typedef struct {
    Display *display;
    int screen, width, height, depth, quit, hardware, current_z, obscured;
    Window window;
    Colormap colormap;
    Atom wm_delete;
    VisualID visual;
    unsigned long red_mask, green_mask, blue_mask;
    Xgl_sys_state system;
    Xgl_win_ras raster;
    Xgl_ctx ctx;
    Xgl_trans model, view;
    Xgl_mem_ras hud, mask;
    Xgl_inquire inquiry;
    Xgl_usgn32 buffers;
    char device[160], error[256], version[160];
    Xgl_pt_f3d *points, *strip_points;
    Xgl_pt_list *lists;
    Xgl_color_facet *facets, *restart_facets;
    Xgl_facet_list *batch_facets;
    xb_strip *strips;
    size_t triangles, point_count, strip_point_count, strip_count;
    enum xb_method method;
    int batch, perspective, errors, fatal;
    int raster_checked, raster_ok, mask_checked, mask_ok;
    xb_native_slot stream[XB_MAX_QUEUE];
    int stream_active, stream_slots, stream_batch;
    Xgl_boolean stream_previous_volatile;
} xb_native;

static xb_native b;
static int validate_raster(int masked);
static int validate_stream(void);

static void free_stream_slots(void)
{
    int i;
    for (i = 0; i < XB_MAX_QUEUE; ++i) {
        free(b.stream[i].points);
        free(b.stream[i].lists);
        free(b.stream[i].facets);
        free(b.stream[i].batch_facets);
        memset(&b.stream[i], 0, sizeof(b.stream[i]));
    }
    b.stream_slots = 0;
}

static void copy_text(char *dest, size_t size, const char *src)
{
    if (!size) return;
    if (!src) src = "unknown";
    strncpy(dest, src, size - 1);
    dest[size - 1] = '\0';
}

static void fail(const char *message)
{
    copy_text(b.error, sizeof(b.error), message);
}

static Xgl_sgn32 xgl_error(Xgl_sys_state system)
{
    Xgl_error_info info;
    memset(&info, 0, sizeof(info));
    xgl_object_get(system, XGL_SYS_ST_ERROR_INFO, &info);
    ++b.errors;
    if (info.type == XGL_ERROR_NONRECOVERABLE) b.fatal = 1;
    copy_text(b.error, sizeof(b.error), info.msg ? info.msg : "XGL operation failed");
    fprintf(stderr, "XGL error %s in %s: %s\n", info.id ? info.id : "?",
            info.cur_op ? info.cur_op : "?", b.error);
    return 1;
}

static const char *support(Xgl_inq_support_level value)
{
    if (value == XGL_INQ_HARDWARE) return "hardware";
    if (value == XGL_INQ_SOFTWARE) return "software";
    if (value == XGL_INQ_NOT_SUPPORTED) return "unsupported";
    return "unknown";
}

static void identity(Xgl_trans transform)
{
    Xgl_matrix_f3d matrix;
    int i;
    if (b.errors) return;
    memset(matrix, 0, sizeof(matrix));
    for (i = 0; i < 4; ++i) matrix[i][i] = 1.0f;
    xgl_transform_write(transform, matrix);
}

static void view_matrix(int perspective)
{
    Xgl_matrix_f3d matrix;
    int i;
    if (b.errors) return;
    memset(matrix, 0, sizeof(matrix));
    for (i = 0; i < 4; ++i) matrix[i][i] = 1.0f;
    /* Row-vector homogeneous projection: w = 1 + 0.75*z.
     * This exercises native clipping/divide without pre-dividing vertices.
     * Scene z stays positive; near is smaller z in Y_DOWN_Z_AWAY.
     */
    if (perspective) matrix[2][3] = 0.75f;
    xgl_transform_write(b.view, matrix);
}

static void setup_view(void)
{
    Xgl_bounds_d3d bounds;
    Xgl_hlhsr_data clear;
    Xgl_pt_d3d maximum;
    if (b.errors) return;
    memset(&bounds, 0, sizeof(bounds));
    bounds.xmin = bounds.ymin = -1.0;
    bounds.xmax = bounds.ymax = 1.0;
    bounds.zmin = 0.0;
    bounds.zmax = 1.0;
    xgl_object_set(b.ctx, XGL_CTX_VDC_MAP, XGL_VDC_MAP_ALL,
                   XGL_CTX_VDC_ORIENTATION, XGL_Y_DOWN_Z_AWAY,
                   XGL_CTX_VDC_WINDOW, &bounds,
                   XGL_CTX_VIEW_CLIP_BOUNDS, &bounds,
                   XGL_CTX_CLIP_PLANES, XGL_CLIP_XMIN | XGL_CLIP_XMAX |
                       XGL_CLIP_YMIN | XGL_CLIP_YMAX |
                       XGL_CLIP_ZMIN | XGL_CLIP_ZMAX, 0);
    if (b.errors) return;
    xgl_object_get(b.raster, XGL_DEV_MAXIMUM_COORDINATES, &maximum);
    if (b.errors) return;
    clear.z_buffer.z_value = (float)maximum.z;
    xgl_object_set(b.ctx, XGL_3D_CTX_HLHSR_DATA, &clear, 0);
}

static int make_hud(void)
{
    Xgl_usgn32 *pixels, *mask_address;
    Xgl_usgn16 *mask_words;
    int x, y;
    pixels = mask_address = NULL;
    b.hud = xgl_object_create(b.system, XGL_MEM_RAS, NULL,
                XGL_RAS_DEPTH, 32, XGL_RAS_WIDTH, XB_RASTER_W,
                XGL_RAS_HEIGHT, XB_RASTER_H,
                XGL_DEV_COLOR_TYPE, XGL_COLOR_RGB, 0);
    if (!b.hud || b.errors) return 0;
    b.mask = xgl_object_create(b.system, XGL_MEM_RAS, NULL,
                XGL_RAS_DEPTH, 1, XGL_RAS_WIDTH, XB_RASTER_W,
                XGL_RAS_HEIGHT, XB_RASTER_H, 0);
    if (!b.hud || !b.mask || b.errors) return 0;
    xgl_object_get(b.hud, XGL_MEM_RAS_IMAGE_BUFFER_ADDR, &pixels);
    if (b.errors) return 0;
    xgl_object_get(b.mask, XGL_MEM_RAS_IMAGE_BUFFER_ADDR, &mask_address);
    if (b.errors) return 0;
    if (!pixels || !mask_address) return 0;
    mask_words = (Xgl_usgn16 *)mask_address;
    memset(mask_words, 0, (size_t)XB_RASTER_W * XB_RASTER_H / 8);
    for (y = 0; y < XB_RASTER_H; ++y) {
        for (x = 0; x < XB_RASTER_W; ++x) {
            /* Canonical XGL native word, independent of host byte order. */
            pixels[y * XB_RASTER_W + x] =
                ((Xgl_usgn32)(64 + (y * 3) % 192) << 16) |
                ((Xgl_usgn32)(32 + (x * 5) % 224) << 8) | 220;
            if (((x / 8) + (y / 8)) % 2 == 0)
                mask_words[y * (XB_RASTER_W / 16) + x / 16] |=
                    (Xgl_usgn16)(0x8000U >> (x & 15));
        }
    }
    xgl_object_set(b.ctx, XGL_CTX_RASTER_FPAT, b.mask,
                   XGL_CTX_ROP, XGL_ROP_COPY, 0);
    return !b.errors;
}

static int wait_window(int width, int height)
{
    double deadline;
    XWindowAttributes attributes;
    XEvent event;
    fd_set readfds;
    struct timeval timeout;
    int fd;
    deadline = xb_now() + 5.0;
    fd = ConnectionNumber(b.display);
    do {
        while (XPending(b.display)) {
            XNextEvent(b.display, &event);
            if (event.type == VisibilityNotify)
                b.obscured = event.xvisibility.state != VisibilityUnobscured;
            if (event.type == DestroyNotify) {
                b.window = 0;
                b.quit = 1;
                fail("window destroyed while waiting for resize");
                return 0;
            }
            if (event.type == ClientMessage &&
                (Atom)event.xclient.data.l[0] == b.wm_delete) {
                b.quit = 1;
                fail("window closed while waiting for resize");
                return 0;
            }
        }
        XGetWindowAttributes(b.display, b.window, &attributes);
        if (attributes.map_state == IsViewable && !b.obscured &&
            attributes.width == width && attributes.height == height) {
            b.width = attributes.width;
            b.height = attributes.height;
            XSync(b.display, False);
            return 1;
        }
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 20000;
        (void)select(fd + 1, &readfds, NULL, NULL, &timeout);
    } while (xb_now() < deadline);
    fail("requested drawable size was not visible and unobscured within 5 seconds");
    return 0;
}

static int choose_visual(int width, int height, int allow_fallback)
{
    XVisualInfo query, *visuals;
    XSetWindowAttributes attributes;
    Xgl_X_window xwindow;
    Xgl_obj_desc descriptor;
    Xgl_inquire *info;
    int count, i, best, score;
    Window window;
    Colormap colormap;
    memset(&query, 0, sizeof(query));
    query.screen = b.screen;
    query.class = TrueColor;
    visuals = XGetVisualInfo(b.display, VisualScreenMask | VisualClassMask,
                            &query, &count);
    if (!visuals) {
        fail("no TrueColor visual is available on this X screen");
        return 0;
    }
    best = -1;
    for (i = 0; i < count; ++i) {
        if (visuals[i].depth < 24 && !allow_fallback) continue;
        colormap = XCreateColormap(b.display, RootWindow(b.display, b.screen),
                                   visuals[i].visual, AllocNone);
        memset(&attributes, 0, sizeof(attributes));
        attributes.colormap = colormap;
        attributes.background_pixel = 0;
        attributes.border_pixel = 0;
        attributes.backing_store = NotUseful;
        attributes.event_mask = ExposureMask | StructureNotifyMask |
                                KeyPressMask | VisibilityChangeMask;
        window = XCreateWindow(b.display, RootWindow(b.display, b.screen),
                    0, 0, (unsigned int)width, (unsigned int)height, 0,
                    visuals[i].depth, InputOutput, visuals[i].visual,
                    CWColormap | CWBackPixel | CWBorderPixel |
                    CWBackingStore | CWEventMask, &attributes);
        XSync(b.display, False);
        memset(&xwindow, 0, sizeof(xwindow));
        xwindow.X_display = b.display;
        xwindow.X_screen = b.screen;
        xwindow.X_window = window;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.win_ras.type = XGL_WIN_X;
        descriptor.win_ras.desc = &xwindow;
        info = xgl_inquire(b.system, &descriptor);
        score = -1;
        if (info && !b.errors) {
            score = (info->dga_flag ? 1000 : 0) +
                    (info->true_color == XGL_INQ_HARDWARE ? 100 : 0) +
                    (info->hlhsr == XGL_INQ_HARDWARE ? 100 : 0) +
                    (info->shading == XGL_INQ_HARDWARE ? 50 : 0) +
                    (info->maximum_buffer >= 2 ? 20 : 0) +
                    (visuals[i].depth == 24 ? 2 : 0);
        }
        if (score > best) {
            if (b.window) XDestroyWindow(b.display, b.window);
            if (b.colormap) XFreeColormap(b.display, b.colormap);
            b.window = window;
            b.colormap = colormap;
            b.depth = visuals[i].depth;
            b.visual = visuals[i].visualid;
            b.red_mask = visuals[i].red_mask;
            b.green_mask = visuals[i].green_mask;
            b.blue_mask = visuals[i].blue_mask;
            b.inquiry = *info;
            copy_text(b.device, sizeof(b.device), info->name);
            b.inquiry.name = b.device;
            best = score;
        } else {
            XDestroyWindow(b.display, window);
            XFreeColormap(b.display, colormap);
        }
        if (info) free(info);
        if (b.errors) break;
    }
    XFree(visuals);
    if (best < 0 || b.errors) {
        if (!b.errors) fail("no 24/32-bit TrueColor visual could be queried by XGL");
        return 0;
    }
    return 1;
}

int xb_backend_open(const xb_options *options, int width, int height)
{
    Xgl_X_window xwindow;
    Xgl_obj_desc descriptor;
    Xgl_color black;
    XSizeHints hints;
    char *version;
    memset(&b, 0, sizeof(b));
    b.display = XOpenDisplay(options->display);
    if (!b.display) {
        fail("cannot open X display; run in the local OpenWindows desktop");
        return 0;
    }
    b.screen = DefaultScreen(b.display);
    b.system = xgl_open(NULL);
    if (!b.system) { fail("xgl_open failed"); return 0; }
    xgl_object_set(b.system, XGL_SYS_ST_ERROR_NOTIFICATION_FUNCTION, xgl_error,
                   XGL_SYS_ST_ERROR_DETECTION, TRUE, 0);
    if (b.errors) return 0;
    version = NULL;
    xgl_object_get(b.system, XGL_SYS_ST_VERSION, &version);
    if (b.errors) return 0;
    copy_text(b.version, sizeof(b.version), version);
    if (!choose_visual(width, height, options->allow_fallback)) return 0;
    b.hardware = b.inquiry.dga_flag &&
        b.inquiry.true_color == XGL_INQ_HARDWARE &&
        b.inquiry.hlhsr == XGL_INQ_HARDWARE &&
        b.inquiry.shading == XGL_INQ_HARDWARE;
    if (!options->allow_fallback && (!b.hardware ||
        b.inquiry.double_buffer != XGL_INQ_HARDWARE ||
        b.inquiry.maximum_buffer < 2)) {
        fail("selected visual lacks local DGA/hardware RGB, shading, Z, or double buffering; use -allow-fallback only for diagnostic runs");
        return 0;
    }
    b.wm_delete = XInternAtom(b.display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(b.display, b.window, &b.wm_delete, 1);
    memset(&hints, 0, sizeof(hints));
    hints.flags = PSize;
    hints.width = width;
    hints.height = height;
    XSetWMNormalHints(b.display, b.window, &hints);
    XStoreName(b.display, b.window, "xglbench: initializing (Escape to stop)");
    XMapRaised(b.display, b.window);
    XSync(b.display, False);
    if (!wait_window(width, height)) return 0;
    memset(&xwindow, 0, sizeof(xwindow));
    xwindow.X_display = b.display;
    xwindow.X_screen = b.screen;
    xwindow.X_window = b.window;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.win_ras.type = options->allow_fallback ? XGL_WIN_X :
                             (XGL_WIN_X | XGL_WIN_X_PROTO_DGA);
    descriptor.win_ras.desc = &xwindow;
    b.raster = xgl_object_create(b.system, XGL_WIN_RAS, &descriptor,
                XGL_DEV_COLOR_TYPE, XGL_COLOR_RGB,
                XGL_WIN_RAS_BACKING_STORE, FALSE,
                XGL_WIN_RAS_BUFFERS_REQUESTED, 2, 0);
    if (!b.raster || b.errors) return 0;
    xgl_object_get(b.raster, XGL_WIN_RAS_BUFFERS_ALLOCATED, &b.buffers);
    if (b.errors) return 0;
    if (b.buffers < 2 && !options->allow_fallback) {
        fail("XGL allocated fewer than two window buffers");
        return 0;
    }
    xgl_object_set(b.raster, XGL_WIN_RAS_BUF_DISPLAY, 0,
                   XGL_WIN_RAS_BUF_DRAW, b.buffers >= 2 ? 1 : 0,
                   XGL_RAS_SOURCE_BUFFER, XGL_BUFFER_SEL_DRAW, 0);
    if (b.errors) return 0;
    memset(&black, 0, sizeof(black));
    b.ctx = xgl_object_create(b.system, XGL_3D_CTX, NULL,
                XGL_CTX_DEVICE, b.raster,
                XGL_CTX_BACKGROUND_COLOR, &black,
                XGL_CTX_DEFERRAL_MODE, XGL_DEFER_ASTI,
                XGL_CTX_GEOM_DATA_IS_VOLATILE, TRUE,
                XGL_CTX_SURF_FRONT_COLOR_SELECTOR, XGL_SURF_COLOR_FACET,
                XGL_3D_CTX_SURF_FRONT_ILLUMINATION, XGL_ILLUM_NONE,
                XGL_CTX_SURF_FRONT_FILL_STYLE, XGL_SURF_FILL_SOLID,
                XGL_CTX_SURF_EDGE_FLAG, FALSE,
                XGL_3D_CTX_SURF_FACE_CULL, XGL_CULL_OFF,
                XGL_3D_CTX_SURF_FACE_DISTINGUISH, FALSE,
                XGL_3D_CTX_Z_BUFFER_COMP_METHOD, XGL_Z_COMP_LESS_THAN_OR_EQUAL,
                XGL_3D_CTX_Z_BUFFER_WRITE_MASK, (Xgl_usgn32)0xffffffffUL,
                XGL_3D_CTX_HLHSR_MODE, XGL_HLHSR_Z_BUFFER, 0);
    if (!b.ctx || b.errors) return 0;
    b.current_z = 1;
    xgl_object_get(b.ctx, XGL_CTX_LOCAL_MODEL_TRANS, &b.model);
    if (b.errors) return 0;
    xgl_object_get(b.ctx, XGL_CTX_VIEW_TRANS, &b.view);
    if (b.errors) return 0;
    xgl_object_set(b.model, XGL_TRANS_DATA_TYPE, XGL_DATA_FLT,
                   XGL_TRANS_DIMENSION, XGL_TRANS_3D, 0);
    if (b.errors) return 0;
    xgl_object_set(b.view, XGL_TRANS_DATA_TYPE, XGL_DATA_FLT,
                   XGL_TRANS_DIMENSION, XGL_TRANS_3D, 0);
    if (b.errors) return 0;
    identity(b.model);
    view_matrix(0);
    setup_view();
    xb_backend_clear(1, 1);
    xb_backend_sync();
    if (b.buffers >= 2) {
        xb_backend_swap();
        xb_backend_clear(1, 1);
        xb_backend_sync();
    }
    (void)xb_backend_events();
    return !b.errors;
}

static void free_geometry(void)
{
    free(b.points); free(b.strip_points); free(b.lists);
    free(b.facets); free(b.restart_facets); free(b.strips);
    free(b.batch_facets);
    b.points = b.strip_points = NULL;
    b.lists = NULL;
    b.facets = b.restart_facets = NULL;
    b.batch_facets = NULL;
    b.strips = NULL;
    b.triangles = b.point_count = b.strip_point_count = b.strip_count = 0;
}

void xb_backend_close(void)
{
    if (b.ctx && !b.errors) xb_backend_sync();
    if (b.system) xgl_close(b.system); /* also destroys every XGL object */
    /* Even after an error, xgl_close ends all native references. */
    free_stream_slots();
    b.stream_active = 0;
    free_geometry();
    if (b.display) {
        if (b.window) XDestroyWindow(b.display, b.window);
        if (b.colormap) XFreeColormap(b.display, b.colormap);
        XCloseDisplay(b.display);
    }
    b.system = NULL;
    b.display = NULL;
    b.ctx = NULL;
}

int xb_backend_resize(int width, int height)
{
    if (b.errors || b.quit) return 0;
    xb_backend_sync();
    XResizeWindow(b.display, b.window, (unsigned int)width, (unsigned int)height);
    XSync(b.display, False);
    if (!wait_window(width, height)) return 0;
    xgl_window_raster_resize(b.raster);
    b.raster_checked = b.mask_checked = 0;
    setup_view();
    xb_backend_clear(1, 1);
    xb_backend_sync();
    if (b.buffers >= 2) {
        xb_backend_swap();
        xb_backend_clear(1, 1);
        xb_backend_sync();
    }
    (void)xb_backend_events();
    return !b.errors && !b.quit;
}

void xb_backend_dimensions(int *width, int *height)
{
    *width = b.width;
    *height = b.height;
}

void xb_backend_info(FILE *out)
{
    fprintf(out, "XGL version: %s\nDevice: %s\n", b.version, b.device);
    fprintf(out, "Display: %s; screen %d; visual 0x%lx TrueColor depth %d\n",
            b.display ? DisplayString(b.display) : "unopened", b.screen,
            (unsigned long)b.visual, b.depth);
    fprintf(out, "Visual RGB masks: 0x%lx / 0x%lx / 0x%lx; HUD memory raster: "
                 "32-bit native B:G:R words (24 color bits).\n",
            b.red_mask, b.green_mask, b.blue_mask);
    fprintf(out, "Drawable: %dx%d; DGA: %s; buffers allocated: %lu (maximum %ld)\n",
            b.width, b.height, b.inquiry.dga_flag ? "yes" : "no",
            (unsigned long)b.buffers, (long)b.inquiry.maximum_buffer);
    fprintf(out, "Reported acceleration: RGB=%s shading=%s Z=%s double-buffer=%s\n",
            support(b.inquiry.true_color), support(b.inquiry.shading),
            support(b.inquiry.hlhsr), support(b.inquiry.double_buffer));
    fprintf(out, "Swap implementation: %s; transparency=%s; lighting=%s\n",
            b.inquiry.db_buffer_is_copy ? "copy" : "buffer switch",
            support(b.inquiry.transparency), support(b.inquiry.lighting));
    fprintf(out, "State: flat facet RGB; no XGL illumination/textures; culling off; "
                 "six clipping planes; deferred submission; volatile geometry "
                 "except retained-array and pipeline cases.\n");
    fprintf(out, "Raster copies share the 3D context to preserve draw order "
                 "without inter-context synchronization.\n");
    fprintf(out, "Triangle-list facets are padded per batch to num_points-2 as "
                 "documented; readback must pass. No IS_PLANAR flag is used.\n");
    fprintf(out, "Strip inputs share vertex records; batch caps split strips "
                 "with two overlapping vertices. Raster copies: %dx%d.\n",
            XB_RASTER_W, XB_RASTER_H);
    fprintf(out, "Pipeline: one thread/context; stable native slots; BUFFERS posts, "
                 "SYNCHRONIZE drains before slot reuse; no presentation.\n");
    fprintf(out, "Input bytes count referenced native points/facet colors, including "
                 "strip overlaps and list padding, excluding descriptors; "
                 "not measured SBus/DMA traffic.\n");
    if (!b.hardware || b.buffers < 2)
        fprintf(out, "DIAGNOSTIC FALLBACK: these results do not establish ZX hardware performance.\n");
}

int xb_backend_upload(const xb_mesh *mesh)
{
    size_t i, start, count, batch_count;
    int needs_points, needs_lists;
    if (b.stream_active) {
        fail("end the active stream before uploading ordinary native geometry");
        return 0;
    }
    if (!mesh || !mesh->triangles || mesh->point_count != mesh->triangles * 3)
        return 0;
    if (b.batch <= 0) b.batch = 128;
    /* Full rebuild is intentionally measurable; static workloads upload once. */
    free_geometry();
    needs_points = b.method != XB_STRIP;
    needs_lists = b.method == XB_POLYGON || b.method == XB_MULTI;
    if (needs_points)
        b.points = (Xgl_pt_f3d *)calloc(mesh->point_count, sizeof(*b.points));
    if (b.method == XB_STRIP) {
        b.strip_points = (Xgl_pt_f3d *)calloc(mesh->strip_point_count,
                                            sizeof(*b.strip_points));
        b.strips = (xb_strip *)calloc(mesh->strip_count, sizeof(*b.strips));
    }
    if (needs_lists)
        b.lists = (Xgl_pt_list *)calloc(mesh->triangles, sizeof(*b.lists));
    b.facets = (Xgl_color_facet *)calloc(mesh->triangles, sizeof(*b.facets));
    if (b.method == XB_TLIST)
        b.restart_facets = (Xgl_color_facet *)calloc(mesh->triangles * 3,
                                                   sizeof(*b.restart_facets));
    batch_count = (mesh->triangles + (size_t)b.batch - 1) / (size_t)b.batch;
    if (b.method == XB_MULTI)
        b.batch_facets = (Xgl_facet_list *)calloc(batch_count, sizeof(*b.batch_facets));
    if ((needs_points && !b.points) || (needs_lists && !b.lists) || !b.facets ||
        (b.method == XB_STRIP && (!b.strips || !b.strip_points)) ||
        (b.method == XB_TLIST && !b.restart_facets) ||
        (b.method == XB_MULTI && !b.batch_facets)) {
        fail("out of memory building native XGL submission arrays");
        free_geometry();
        return 0;
    }
    b.triangles = mesh->triangles;
    b.point_count = mesh->point_count;
    b.strip_point_count = mesh->strip_point_count;
    b.strip_count = mesh->strip_count;
    if (b.strips)
        memcpy(b.strips, mesh->strips, mesh->strip_count * sizeof(*b.strips));
    for (i = 0; b.points && i < mesh->point_count; ++i) {
        b.points[i].x = mesh->points[i].x;
        b.points[i].y = mesh->points[i].y;
        b.points[i].z = mesh->points[i].z;
    }
    for (i = 0; b.strip_points && i < mesh->strip_point_count; ++i) {
        b.strip_points[i].x = mesh->strip_points[i].x;
        b.strip_points[i].y = mesh->strip_points[i].y;
        b.strip_points[i].z = mesh->strip_points[i].z;
    }
    for (i = 0; b.lists && i < mesh->triangles; ++i) {
        b.lists[i].pt_type = XGL_PT_F3D;
        b.lists[i].num_pts = 3;
        b.lists[i].num_data_values = 0;
        b.lists[i].pts.f3d = &b.points[i * 3];
    }
    for (i = 0, start = 0; b.batch_facets && start < mesh->triangles;
         ++i, start += count) {
        count = mesh->triangles - start;
        if (count > (size_t)b.batch) count = (size_t)b.batch;
        b.batch_facets[i].facet_type = XGL_FACET_COLOR;
        b.batch_facets[i].num_facets = (Xgl_usgn32)count;
        b.batch_facets[i].facets.color_facets = b.facets + start;
    }
    xb_backend_colors(mesh);
    return 1;
}

void xb_backend_colors(const xb_mesh *mesh)
{
    size_t i, start, count;
    int batch;
    if (!mesh || mesh->triangles != b.triangles) return;
    for (i = 0; i < b.triangles; ++i) {
        b.facets[i].color.rgb.r = mesh->colors[i].r;
        b.facets[i].color.rgb.g = mesh->colors[i].g;
        b.facets[i].color.rgb.b = mesh->colors[i].b;
    }
    if (b.method != XB_TLIST) return;
    batch = b.batch > 0 ? b.batch : 128;
    for (start = 0; start < b.triangles; start += count) {
        count = b.triangles - start;
        if (count > (size_t)batch) count = (size_t)batch;
        /* First count records carry actual per-triangle colors. Remaining
         * records satisfy the documented num_points-2 minimum. Validation
         * rejects drivers that interpret restart facet indexing differently.
         */
        for (i = 0; i < count * 3; ++i)
            b.restart_facets[start * 3 + i] = b.facets[start +
                                                (i < count ? i : count - 1)];
    }
}

size_t xb_backend_input_bytes(enum xb_method method, int batch)
{
    size_t i, count, batches, points, facets;
    if (method != b.method) return 0;
    if (batch <= 0) batch = 128;
    points = b.point_count;
    facets = b.triangles;
    if (method == XB_STRIP) {
        points = facets = 0;
        for (i = 0; i < b.strip_count; ++i) {
            count = b.strips[i].point_count - 2;
            batches = (count + (size_t)batch - 1) / (size_t)batch;
            points += count + 2 * batches;
            facets += count;
        }
    } else if (method == XB_TLIST) {
        batches = (b.triangles + (size_t)batch - 1) / (size_t)batch;
        facets = 3 * b.triangles - 2 * batches;
    }
    return points * sizeof(Xgl_pt_f3d) + facets * sizeof(Xgl_color_facet);
}

static int allocate_stream_slot(int index, const xb_mesh *mesh)
{
    xb_native_slot *slot;
    size_t i, start, count, batches;
    slot = &b.stream[index];
    slot->triangles = mesh->triangles;
    slot->point_count = mesh->point_count;
    batches = (mesh->triangles + (size_t)b.stream_batch - 1) /
              (size_t)b.stream_batch;
    slot->points = (Xgl_pt_f3d *)calloc(mesh->point_count, sizeof(*slot->points));
    slot->lists = (Xgl_pt_list *)calloc(mesh->triangles, sizeof(*slot->lists));
    slot->facets = (Xgl_color_facet *)calloc(mesh->triangles, sizeof(*slot->facets));
    slot->batch_facets = (Xgl_facet_list *)calloc(batches, sizeof(*slot->batch_facets));
    if (!slot->points || !slot->lists || !slot->facets || !slot->batch_facets) {
        fail("out of memory allocating stable native stream slots");
        return 0;
    }
    for (i = 0; i < mesh->point_count; ++i) {
        slot->points[i].x = mesh->points[i].x;
        slot->points[i].y = mesh->points[i].y;
        slot->points[i].z = mesh->points[i].z;
    }
    for (i = 0; i < mesh->triangles; ++i) {
        slot->lists[i].pt_type = XGL_PT_F3D;
        slot->lists[i].num_pts = 3;
        slot->lists[i].pts.f3d = slot->points + i * 3;
        slot->facets[i].color.rgb.r = mesh->colors[i].r;
        slot->facets[i].color.rgb.g = mesh->colors[i].g;
        slot->facets[i].color.rgb.b = mesh->colors[i].b;
    }
    for (i = 0, start = 0; start < mesh->triangles; ++i, start += count) {
        count = mesh->triangles - start;
        if (count > (size_t)b.stream_batch) count = (size_t)b.stream_batch;
        slot->batch_facets[i].facet_type = XGL_FACET_COLOR;
        slot->batch_facets[i].num_facets = (Xgl_usgn32)count;
        slot->batch_facets[i].facets.color_facets = slot->facets + start;
    }
    slot->prepared = 1;
    return 1;
}

int xb_backend_stream_prepare(int index, const xb_mesh *mesh)
{
    xb_native_slot *slot;
    size_t i;
    if (b.errors || b.fatal) return 0;
    if (!b.stream_active || index < 0 || index >= b.stream_slots) {
        fail("invalid native stream slot");
        return 0;
    }
    slot = &b.stream[index];
    if (slot->pending) {
        fail("stream slot is pending; synchronize before replacing its colors");
        return 0;
    }
    if (!mesh || !mesh->colors || mesh->triangles != slot->triangles ||
        mesh->point_count != slot->point_count) {
        fail("stream preparation requires the original static geometry dimensions");
        return 0;
    }
    /* No allocation or source-mesh reference survives this call. */
    for (i = 0; i < slot->triangles; ++i) {
        slot->facets[i].color.rgb.r = mesh->colors[i].r;
        slot->facets[i].color.rgb.g = mesh->colors[i].g;
        slot->facets[i].color.rgb.b = mesh->colors[i].b;
    }
    slot->prepared = 1;
    return 1;
}

static size_t draw_stream_slot(int index, int clear)
{
    xb_native_slot *slot;
    size_t start, count, calls;
    if (b.errors || b.fatal) return 0;
    if (!b.stream_active || index < 0 || index >= b.stream_slots) {
        fail("invalid native stream slot");
        return 0;
    }
    slot = &b.stream[index];
    if (slot->pending || !slot->prepared) {
        fail("stream slot must be initialized and completed before submission");
        return 0;
    }
    /* Mark before any native operation: failure cannot make its references
     * safe to overwrite. Only a successful completion clears this flag.
     */
    slot->pending = 1;
    if (clear) xb_backend_clear(1, 1);
    calls = 0;
    for (start = 0; start < slot->triangles && !b.errors; start += count) {
        count = slot->triangles - start;
        if (count > (size_t)b.stream_batch) count = (size_t)b.stream_batch;
        xgl_multi_simple_polygon(b.ctx,
                XGL_FACET_FLAG_SIDES_ARE_3 | XGL_FACET_FLAG_SHAPE_CONVEX,
                &slot->batch_facets[calls], NULL, (Xgl_usgn32)count,
                slot->lists + start);
        ++calls;
    }
    return b.errors ? 0 : calls;
}

size_t xb_backend_stream_draw(int index)
{
    return draw_stream_slot(index, 1);
}

void xb_backend_post(void)
{
    if (b.ctx && !b.errors) xgl_context_flush(b.ctx, XGL_FLUSH_BUFFERS);
}

int xb_backend_stream_drain(void)
{
    int i;
    if (!b.ctx || b.errors || b.fatal) return 0;
    xgl_context_flush(b.ctx, XGL_FLUSH_SYNCHRONIZE);
    if (b.errors) return 0;
    for (i = 0; i < b.stream_slots; ++i) b.stream[i].pending = 0;
    return 1;
}

void xb_backend_stream_end(void)
{
    if (!b.stream_active) return;
    if (!xb_backend_stream_drain()) return;
    xgl_object_set(b.ctx, XGL_CTX_GEOM_DATA_IS_VOLATILE,
                   b.stream_previous_volatile, 0);
    if (b.errors) return;
    free_stream_slots();
    b.stream_active = 0;
}

int xb_backend_stream_begin(const xb_mesh *mesh, int slots,
                            char *reason, size_t size)
{
    int i;
    if (b.errors || b.fatal || b.quit) goto bad;
    xb_backend_stream_end();
    if (b.stream_active || b.errors) goto bad;
    if (b.method != XB_MULTI || slots < 1 || slots > XB_MAX_QUEUE || !mesh ||
        !mesh->triangles || mesh->point_count != mesh->triangles * 3 ||
        !mesh->points || !mesh->colors) {
        fail("streaming requires multi-polygon geometry and 1..8 stable slots");
        goto bad;
    }
    xb_backend_sync();
    if (b.errors) goto bad;
    xgl_object_get(b.ctx, XGL_CTX_GEOM_DATA_IS_VOLATILE,
                   &b.stream_previous_volatile);
    if (b.errors) goto bad;
    /* Record ownership before changing state so error recovery restores it. */
    b.stream_active = 1;
    b.stream_batch = b.batch;
    xgl_object_set(b.ctx, XGL_CTX_GEOM_DATA_IS_VOLATILE, FALSE, 0);
    if (b.errors || !validate_stream()) goto bad;
    free_stream_slots(); /* probe has completed successfully */
    b.stream_slots = slots;
    for (i = 0; i < slots; ++i)
        if (!allocate_stream_slot(i, mesh)) goto bad;
    copy_text(reason, size, "independent queued slot colors and per-item color/Z clear readback passed");
    return 1;
bad:
    copy_text(reason, size, b.error);
    /* On a native failure end deliberately keeps arrays alive for a later
     * reconfiguration recovery or, ultimately, xgl_close.
     */
    xb_backend_stream_end();
    return 0;
}

int xb_backend_configure(const xb_case *test, const xb_mesh *mesh,
                         char *reason, size_t size)
{
    if (b.fatal || b.quit) { copy_text(reason, size, b.error); return 0; }
    /* A recoverable API error aborts its case. Reset only at an explicit
     * reconfiguration boundary; nonrecoverable errors never resume drawing.
     */
    b.errors = 0;
    b.error[0] = '\0';
    /* Complete any retained data from a preceding recoverable error before
     * changing attributes or releasing the previous native mesh arrays.
     */
    xb_backend_sync();
    if (b.errors) { copy_text(reason, size, b.error); return 0; }
    xb_backend_stream_end();
    if (b.stream_active || b.errors) {
        copy_text(reason, size, b.error);
        return 0;
    }
    b.batch = test->batch > 0 ? test->batch : 128;
    b.method = test->method;
    b.perspective = test->perspective;
    if (test->retained && (test->method != XB_MULTI || test->update != XB_STATIC)) {
        copy_text(reason, size, "retained-array test requires static multi-polygon geometry");
        return 0;
    }
    if ((test->work == XB_SWAP || test->work == XB_FRAME) && b.buffers < 2) {
        copy_text(reason, size, "two window buffers were not allocated");
        return 0;
    }
    b.current_z = test->zbuffer;
    xgl_object_set(b.ctx, XGL_CTX_GEOM_DATA_IS_VOLATILE,
                   test->retained ? FALSE : TRUE,
                   XGL_3D_CTX_HLHSR_MODE,
                   b.current_z ? XGL_HLHSR_Z_BUFFER : XGL_HLHSR_NONE, 0);
    identity(b.model);
    view_matrix(test->perspective);
    if (mesh && mesh->triangles && !xb_backend_upload(mesh)) {
        copy_text(reason, size, b.error);
        return 0;
    }
    xb_backend_clear(1, 1);
    xb_backend_sync();
    if (b.errors) { copy_text(reason, size, b.error); return 0; }
    if (test->work == XB_RASTER || test->work == XB_MASKED_RASTER ||
        test->work == XB_FRAME) {
        if (!validate_raster(0) ||
            ((test->work == XB_MASKED_RASTER || test->work == XB_FRAME) &&
             !validate_raster(1))) {
            copy_text(reason, size, b.error);
            return 0;
        }
        xb_backend_clear(1, 1);
        xb_backend_sync();
    }
    return 1;
}

void xb_backend_transform(unsigned long frame)
{
    Xgl_matrix_f3d matrix;
    double angle, c, s;
    if (b.errors) return;
    angle = (double)(frame % 360) * 0.017453292519943295;
    c = cos(angle); s = sin(angle);
    memset(matrix, 0, sizeof(matrix));
    matrix[0][0] = (float)c;
    matrix[0][1] = (float)s;
    matrix[1][0] = (float)-s;
    matrix[1][1] = (float)c;
    matrix[2][2] = matrix[3][3] = 1.0f;
    xgl_transform_write(b.model, matrix);
}

void xb_backend_clear(int color, int depth)
{
    int actions;
    if (b.errors) return;
    actions = 0;
    if (color) actions |= XGL_CTX_NEW_FRAME_CLEAR;
    if (depth) actions |= XGL_CTX_NEW_FRAME_HLHSR_ACTION;
    if (depth && !b.current_z)
        xgl_object_set(b.ctx, XGL_3D_CTX_HLHSR_MODE, XGL_HLHSR_Z_BUFFER, 0);
    if (b.errors) return;
    xgl_object_set(b.ctx, XGL_CTX_NEW_FRAME_ACTION, actions, 0);
    if (b.errors) return;
    xgl_context_new_frame(b.ctx);
    if (b.errors) return;
    if (depth && !b.current_z)
        xgl_object_set(b.ctx, XGL_3D_CTX_HLHSR_MODE, XGL_HLHSR_NONE, 0);
}

size_t xb_backend_draw(enum xb_method method, int batch)
{
    size_t start, count, calls, i, offset, available;
    Xgl_facet facet;
    Xgl_facet_list facets;
    Xgl_pt_list points;
    if (b.errors) return 0;
    if (method != b.method) {
        fail("draw method differs from configured native geometry representation");
        ++b.errors;
        return 0;
    }
    if (batch <= 0) batch = 128;
    calls = 0;
    memset(&facets, 0, sizeof(facets));
    memset(&points, 0, sizeof(points));
    facets.facet_type = XGL_FACET_COLOR;
    points.pt_type = XGL_PT_F3D;
    if (method == XB_POLYGON) {
        for (i = 0; i < b.triangles && !b.errors; ++i) {
            facet.color_facet = b.facets[i];
            xgl_polygon(b.ctx, XGL_FACET_COLOR, &facet, NULL, 1, &b.lists[i]);
        }
        return i;
    }
    if (method == XB_STRIP) {
        for (i = 0; i < b.strip_count && !b.errors; ++i) {
            available = b.strips[i].point_count - 2;
            for (offset = 0; offset < available && !b.errors; offset += count) {
                count = available - offset;
                if (count > (size_t)batch) count = (size_t)batch;
                points.num_pts = (Xgl_usgn32)(count + 2);
                points.pts.f3d = b.strip_points + b.strips[i].first_point + offset;
                facets.num_facets = (Xgl_usgn32)count;
                facets.facets.color_facets = b.facets +
                                            b.strips[i].first_facet + offset;
                xgl_triangle_strip(b.ctx, &facets, &points);
                ++calls;
            }
        }
        return calls;
    }
    for (start = 0; start < b.triangles && !b.errors; start += count) {
        count = b.triangles - start;
        if (count > (size_t)batch) count = (size_t)batch;
        facets.num_facets = (Xgl_usgn32)count;
        facets.facets.color_facets = b.facets + start;
        if (method == XB_MULTI) {
            xgl_multi_simple_polygon(b.ctx,
                XGL_FACET_FLAG_SIDES_ARE_3 | XGL_FACET_FLAG_SHAPE_CONVEX,
                &b.batch_facets[calls], NULL, (Xgl_usgn32)count, b.lists + start);
        } else {
            points.num_pts = (Xgl_usgn32)(count * 3);
            points.pts.f3d = b.points + start * 3;
            facets.num_facets = (Xgl_usgn32)(count * 3 - 2);
            facets.facets.color_facets = b.restart_facets + start * 3;
            xgl_triangle_list(b.ctx, &facets, &points,
                              XGL_TLIST_FLAG_TRI_RESTART);
        }
        ++calls;
    }
    return calls;
}

void xb_backend_sync(void)
{
    if (b.ctx && !b.errors) xgl_context_flush(b.ctx, XGL_FLUSH_SYNCHRONIZE);
}

void xb_backend_swap(void)
{
    if (b.buffers < 2 || b.errors) return;
    /* Complete queued geometry and raster copies before presentation. */
    xb_backend_sync();
    if (b.errors) return;
    xgl_object_set(b.ctx, XGL_CTX_NEW_FRAME_ACTION,
                   XGL_CTX_NEW_FRAME_SWITCH_BUFFER, 0);
    if (b.errors) return;
    xgl_context_new_frame(b.ctx);
    if (!b.errors) xgl_context_flush(b.ctx, XGL_FLUSH_SYNCHRONIZE);
}

void xb_backend_raster(int masked)
{
    Xgl_pt_i2d position;
    if (b.errors) return;
    position.x = (b.width - XB_RASTER_W) / 2;
    position.y = b.height - XB_RASTER_H;
    xgl_object_set(b.ctx, XGL_CTX_RASTER_FILL_STYLE,
                   masked ? XGL_RAS_FILL_STIPPLE : XGL_RAS_FILL_COPY,
                   XGL_CTX_RASTER_FPAT_POSITION, &position, 0);
    if (!b.errors) xgl_context_copy_buffer(b.ctx, NULL, &position, b.hud);
}

int xb_backend_events(void)
{
    XEvent event;
    int changed;
    changed = 0;
    while (b.display && XPending(b.display)) {
        XNextEvent(b.display, &event);
        if (event.type == ClientMessage &&
            (Atom)event.xclient.data.l[0] == b.wm_delete) {
            b.quit = 1;
            if (!b.errors) b.error[0] = '\0';
        }
        if (event.type == DestroyNotify) {
            b.quit = 1; b.window = 0;
            if (!b.errors) b.error[0] = '\0';
        }
        if (event.type == KeyPress &&
            XLookupKeysym(&event.xkey, 0) == XK_Escape) {
            b.quit = 1;
            if (!b.errors) b.error[0] = '\0';
        }
        if (b.quit) break;
        if (event.type == ConfigureNotify &&
            (event.xconfigure.width != b.width || event.xconfigure.height != b.height)) {
            xb_backend_sync();
            b.width = event.xconfigure.width;
            b.height = event.xconfigure.height;
            xgl_window_raster_resize(b.raster);
            setup_view();
            changed = 1;
        }
        if (event.type == Expose) changed = 1;
        if (event.type == VisibilityNotify) {
            b.obscured = event.xvisibility.state != VisibilityUnobscured;
            changed = 1;
        }
    }
    if (b.quit || b.fatal) return 0;
    if (b.errors) return -1;
    if (b.obscured)
        fail("window is obscured; keep the entire drawable visible for valid timing");
    return changed || b.obscured ? -1 : 1;
}

void xb_backend_label(const char *name)
{
    char title[160];
    xb_backend_sync();
    sprintf(title, "xglbench %dx%d: %.90s (Escape stops)", b.width, b.height, name);
    XStoreName(b.display, b.window, title);
    XSync(b.display, False);
}

int xb_backend_has_double_buffer(void) { return b.buffers >= 2; }
int xb_backend_has_hardware(void) { return b.hardware; }
const char *xb_backend_error(void) { return b.error; }

static int pixel_matches(int x, int y, const xb_rgb *expected)
{
    Xgl_pt_i2d point;
    Xgl_color color;
    if (b.errors) return 0;
    point.x = x; point.y = y;
    memset(&color, 0, sizeof(color));
    if (xgl_context_get_pixel(b.ctx, &point, &color)) {
        fail("validation pixel is obscured; keep the entire benchmark window visible");
        return 0;
    }
    if (fabs(color.rgb.r - expected->r) > 0.065 ||
        fabs(color.rgb.g - expected->g) > 0.065 ||
        fabs(color.rgb.b - expected->b) > 0.065) {
        fail("RGB pixel readback disagrees with expected facet/depth/mask result");
        return 0;
    }
    return !b.errors;
}

static int validate_stream(void)
{
    xb_mesh mesh;
    xb_point points[6];
    xb_rgb colors[2], expected[2], black;
    float x;
    int i, j, px, py, saved_z, result;
    memset(&mesh, 0, sizeof(mesh));
    mesh.triangles = 2; mesh.point_count = 6;
    mesh.points = points; mesh.colors = colors;
    expected[0].r = 1; expected[0].g = expected[0].b = 0;
    expected[1].r = 0; expected[1].g = expected[1].b = 1;
    black.r = black.g = black.b = 0;
    saved_z = b.current_z;
    result = 0;
    b.current_z = 1;
    xgl_object_set(b.ctx, XGL_3D_CTX_HLHSR_MODE, XGL_HLHSR_Z_BUFFER, 0);
    identity(b.model);
    view_matrix(0);
    /* Even the depth-one control exercises two independent native slots in
     * this untimed gate. Disjoint patches survive until the joint readback;
     * normal per-item clears would hide mistakes in earlier queued draws.
     */
    b.stream_slots = 2;
    for (i = 0; i < 2; ++i) {
        x = i ? 0.3f : -0.9f;
        points[0].x = x;        points[0].y = -0.85f;
        points[1].x = x + 0.6f; points[1].y = -0.85f;
        points[2].x = x;        points[2].y = -0.5f;
        points[3].x = -0.3f; points[3].y = -0.2f;
        points[4].x = 0.3f;  points[4].y = -0.2f;
        points[5].x = 0;     points[5].y = 0.4f;
        for (j = 0; j < 6; ++j) points[j].z = i ? 0.8f : 0.2f;
        colors[0] = colors[1] = black;
        if (!allocate_stream_slot(i, &mesh)) goto done;
    }
    xb_backend_clear(1, 1);
    for (i = 0; i < 2; ++i) {
        colors[0] = colors[1] = expected[i];
        if (!xb_backend_stream_prepare(i, &mesh)) goto done;
        colors[0] = colors[1] = black; /* CPU source must not be retained. */
        if (!draw_stream_slot(i, 0)) goto done;
        xb_backend_post();
    }
    if (!xb_backend_stream_drain()) goto done;
    py = (int)((-0.733333333 + 1.0) * 0.5 * (b.height - 1));
    for (i = 0; i < 2; ++i) {
        x = i ? 0.5f : -0.7f;
        px = (int)((x + 1.0) * 0.5 * (b.width - 1));
        if (!pixel_matches(px, py, &expected[i])) goto done;
    }
    if (!pixel_matches(b.width / 2, b.height / 2, &expected[0])) goto done;
    /* Reuse only after completion. The second far central triangle must
     * replace the first near triangle, proving the queued Z clear as well
     * as the color clear (the old left patch must disappear).
     */
    for (i = 0; i < 2; ++i) {
        if (!xb_backend_stream_draw(i)) goto done;
        xb_backend_post();
    }
    if (!xb_backend_stream_drain()) goto done;
    px = (int)(0.3 * 0.5 * (b.width - 1));
    if (!pixel_matches(px, py, &black)) goto done;
    px = (int)(1.5 * 0.5 * (b.width - 1));
    if (!pixel_matches(px, py, &expected[1])) goto done;
    if (!pixel_matches(b.width / 2, b.height / 2, &expected[1])) goto done;
    result = 1;
done:
    /* Native failures preserve all queued references until recovery/close. */
    if (!b.errors) {
        b.current_z = saved_z;
        xgl_object_set(b.ctx, XGL_3D_CTX_HLHSR_MODE,
                       saved_z ? XGL_HLHSR_Z_BUFFER : XGL_HLHSR_NONE, 0);
        view_matrix(b.perspective);
    }
    return result && !b.errors;
}

static int validate_raster(int masked)
{
    xb_rgb black, hud;
    int px, py, result;
    if ((masked && b.mask_checked) || (!masked && b.raster_checked)) {
        result = masked ? b.mask_ok : b.raster_ok;
        if (!result) fail("raster readback validation failed earlier at this resolution");
        return result;
    }
    if (b.width < XB_RASTER_W || b.height < XB_RASTER_H) {
        fail("raster benchmark requires at least a 128x64 drawable");
        return 0;
    }
    if ((!b.hud || !b.mask) && !make_hud()) return 0;
    xb_backend_clear(1, 1);
    xb_backend_raster(masked);
    xb_backend_sync();
    px = (b.width - XB_RASTER_W) / 2;
    py = b.height - XB_RASTER_H;
    black.r = black.g = black.b = 0;
    hud.r = 220.0f / 255; hud.g = 32.0f / 255; hud.b = 64.0f / 255;
    result = pixel_matches(px, py, &hud);
    if (result && masked) result = pixel_matches(px + 9, py, &black);
    if (masked) { b.mask_checked = 1; b.mask_ok = result; }
    else { b.raster_checked = 1; b.raster_ok = result; }
    return result;
}

int xb_backend_validate(enum xb_method method, char *reason, size_t size)
{
    xb_mesh mesh;
    xb_case test;
    xb_point points[24], strip_points[16];
    xb_rgb colors[8], black;
    xb_strip strips[4];
    float x, y, dx, dy;
    size_t i, j, base;
    int z, px, py;
    static const float rgb[8][3] = {
        {1,0,0},{0,1,0},{0,0,1},{1,1,0},
        {1,0,1},{0,1,1},{0.5f,0.25f,0},{0.25f,0.5f,1}
    };
    memset(&mesh, 0, sizeof(mesh));
    memset(&test, 0, sizeof(test));
    test.method = method; test.batch = 3; test.zbuffer = 1;
    test.work = XB_DRAW;
    mesh.triangles = 8; mesh.point_count = 24;
    mesh.strip_point_count = 16; mesh.strip_count = 4;
    mesh.points = points; mesh.strip_points = strip_points;
    mesh.colors = colors; mesh.strips = strips;
    for (i = 0; i < 4; ++i) {
        x = (i & 1) ? 0.1f : -0.9f;
        y = (i & 2) ? 0.1f : -0.9f;
        base = i * 4;
        strip_points[base].x = x; strip_points[base].y = y;
        strip_points[base+1].x = x; strip_points[base+1].y = y + 0.8f;
        strip_points[base+2].x = x + 0.8f; strip_points[base+2].y = y;
        strip_points[base+3].x = x + 0.8f; strip_points[base+3].y = y + 0.8f;
        for (j = 0; j < 4; ++j) strip_points[base+j].z = 0.5f;
        points[i*6] = strip_points[base];
        points[i*6+1] = strip_points[base+1];
        points[i*6+2] = strip_points[base+2];
        points[i*6+3] = strip_points[base+2];
        points[i*6+4] = strip_points[base+1];
        points[i*6+5] = strip_points[base+3];
        strips[i].first_point = base;
        strips[i].point_count = 4;
        strips[i].first_facet = i*2;
    }
    for (i = 0; i < 8; ++i) {
        colors[i].r = rgb[i][0]; colors[i].g = rgb[i][1]; colors[i].b = rgb[i][2];
    }
    for (z = 0; z <= 1; ++z) {
        test.zbuffer = z;
        if (!xb_backend_configure(&test, &mesh, reason, size)) return 0;
        xb_backend_draw(method, 3);
        xb_backend_sync();
        for (i = 0; i < 8; ++i) {
            dx = dy = 0;
            for (j = 0; j < 3; ++j) {
                dx += points[i*3+j].x / 3.0f;
                dy += points[i*3+j].y / 3.0f;
            }
            px = (int)((dx + 1.0) * 0.5 * (b.width - 1));
            py = (int)((dy + 1.0) * 0.5 * (b.height - 1));
            if (!pixel_matches(px, py, &colors[i])) goto bad;
        }
    }
    if (method == XB_MULTI) {
        test.retained = 1;
        if (!xb_backend_configure(&test, &mesh, reason, size)) return 0;
        xb_backend_draw(method, 3); xb_backend_sync();
        for (i = 0; i < 8; ++i) {
            dx = dy = 0;
            for (j = 0; j < 3; ++j) {
                dx += points[i*3+j].x / 3.0f;
                dy += points[i*3+j].y / 3.0f;
            }
            px = (int)((dx + 1.0) * 0.5 * (b.width - 1));
            py = (int)((dy + 1.0) * 0.5 * (b.height - 1));
            if (!pixel_matches(px, py, &colors[i])) goto bad;
        }
        test.retained = 0;
    }
    /* Check row-vector model rotation and the homogeneous W divide together.
     * Every facet remains at z=.5, so centroid projection is unambiguous.
     */
    test.perspective = 1;
    if (!xb_backend_configure(&test, &mesh, reason, size)) return 0;
    xb_backend_transform(90);
    xb_backend_draw(method, 3); xb_backend_sync();
    for (i = 0; i < 8; ++i) {
        dx = dy = 0;
        for (j = 0; j < 3; ++j) {
            dx += -points[i*3+j].y / (3.0f * 1.375f);
            dy += points[i*3+j].x / (3.0f * 1.375f);
        }
        px = (int)((dx + 1.0) * 0.5 * (b.width - 1));
        py = (int)((dy + 1.0) * 0.5 * (b.height - 1));
        if (!pixel_matches(px, py, &colors[i])) goto bad;
    }
    test.perspective = 0;
    /* A near red facet followed by a farther blue facet tests real depth. */
    for (i = 0; i < 2; ++i) {
        base = i*3;
        points[base].x = -0.8f; points[base].y = -0.8f;
        points[base+1].x = 0.8f; points[base+1].y = -0.8f;
        points[base+2].x = 0.0f; points[base+2].y = 0.8f;
        for (j = 0; j < 3; ++j) {
            points[base+j].z = i ? 0.8f : 0.2f;
            strip_points[base+j] = points[base+j];
        }
        strips[i].first_point = base; strips[i].point_count = 3;
        strips[i].first_facet = i;
    }
    colors[0].r = 1; colors[0].g = colors[0].b = 0;
    colors[1].b = 1; colors[1].r = colors[1].g = 0;
    mesh.triangles = 2; mesh.point_count = 6;
    mesh.strip_count = 2; mesh.strip_point_count = 6;
    test.zbuffer = 1;
    if (!xb_backend_configure(&test, &mesh, reason, size)) return 0;
    xb_backend_draw(method, 3); xb_backend_sync();
    if (!pixel_matches(b.width / 2, b.height / 2, &colors[0])) goto bad;
    /* Reversing depths after a clear must expose blue, not retain stale red. */
    for (i = 0; i < 6; ++i) {
        points[i].z = strip_points[i].z = i < 3 ? 0.8f : 0.2f;
    }
    if (!xb_backend_upload(&mesh)) goto bad;
    xb_backend_clear(1, 1); xb_backend_draw(method, 3); xb_backend_sync();
    if (!pixel_matches(b.width / 2, b.height / 2, &colors[1])) goto bad;
    /* Both triangles in front of the near plane must be clipped. */
    for (i = 0; i < 6; ++i) points[i].z = strip_points[i].z = -0.5f;
    if (!xb_backend_upload(&mesh)) goto bad;
    xb_backend_clear(1, 1); xb_backend_draw(method, 3); xb_backend_sync();
    black.r = black.g = black.b = 0;
    if (!pixel_matches(b.width / 2, b.height / 2, &black)) goto bad;
    if (b.buffers >= 2) xb_backend_swap();
    copy_text(reason, size, "facet colors, Z/clear, near clipping and transformed perspective readback passed");
    return 1;
bad:
    copy_text(reason, size, b.error);
    return 0;
}
