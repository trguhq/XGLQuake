/* Host renderer contracts; these tests do not simulate XGL or GPU timing. */
#include <assert.h>
#include <float.h>
#include <stdarg.h>
#include <stdlib.h>
static int fail_alloc_after = -1;
static void *test_malloc(size_t bytes)
{
    if (fail_alloc_after == 0) return NULL;
    if (fail_alloc_after > 0) --fail_alloc_after;
    return malloc(bytes);
}
/* Count frustum tests without changing their result. The macro also
 * renames mathlib.h's prototype, which then declares the wrapper. */
static int box_tests, angle_calls;
#define malloc test_malloc
#define BoxOnPlaneSide test_box_side
#define AngleVectors test_angle_vectors
#include "../xgl_world.c"
#undef AngleVectors
#undef BoxOnPlaneSide
#undef malloc
int BoxOnPlaneSide(vec3_t emins, vec3_t emaxs, struct mplane_s *plane);
int test_box_side(vec3_t emins, vec3_t emaxs, struct mplane_s *plane)
{
    ++box_tests;
    return BoxOnPlaneSide(emins, emaxs, plane);
}
void AngleVectors(vec3_t angles, vec3_t forward, vec3_t right, vec3_t up);
void test_angle_vectors(vec3_t angles, vec3_t forward, vec3_t right, vec3_t up)
{
    ++angle_calls;
    AngleVectors(angles, forward, right, up);
}

client_state_t cl;
lightstyle_t cl_lightstyle[MAX_LIGHTSTYLES];
entity_t r_worldentity, *currententity;
vec3_t r_origin, vpn;
mleaf_t *r_viewleaf;
int r_framecount, r_visframecount, c_brush_polys;
cvar_t r_fullbright, r_wateralpha, r_novis, r_drawentities;
entity_t *cl_visedicts[MAX_VISEDICTS];
int cl_numvisedicts;
mplane_t frustum[4];
double realtime;
byte xgl_palette[768];

static model_t test_model;
static msurface_t test_surface;
static mplane_t test_plane;
static mtexinfo_t test_texinfo;
static texture_t test_texture;
static mvertex_t test_vertices[4];
static medge_t test_edges[4];
static int test_surfedges[4];
static mnode_t test_node;
static mleaf_t test_leaves[3];
static msurface_t *test_marks[1];
static byte test_samples[81];
static efrag_t test_efrag;
static byte test_vis;
static int pvs_calls, sample_calls, efrag_calls, reject_box;
static int smp_active, alias_prepasses, gamma_copies;
static xgl_point_light_t test_lights[XGL_MAX_POINT_LIGHTS];
static int test_light_count, used_lights, use_calls;
static float test_ambient;
/* Everything submitted, as records with normals. One that was submitted
 * without a normal has a zero normal here and is flagged in captured_plain.
 * A caller fills reserved records only after the mock has returned, so
 * those without normals are copied in when the capture is next looked at. */
static xgl_native_vertex_t captured_store[4096];
static xgl_color_vertex_t reserved_plain[12];
static byte captured_plain[4096];
static int pending_first, pending_count;
static xgl_native_vertex_t *settle_captured(void);
#define captured (settle_captured())
static const void *queued[4096];
static size_t queued_size[4096];
static xgl_native_vertex_t queued_copy[4096];
static int queued_count, flush_calls, palette_calls, failed_sample;
static int captured_count, captured_lit, captured_zwrite;
static int captured_calls, varying_material, prepare_calls, filter_calls;
/* 1: a dark disc in the middle of every texture repeat. 2: a dark left half.
 * 3: sixteen-texel blocks of unrelated shades. 4: one small dark square,
 * away from every corner, edge middle and center of the test face. */
static int pattern_material, box_calls;
static unsigned int color_revision, texture_revision;
static float gamma_scale;
static float captured_opacity;
static unsigned long profile_counts[XGL_PROFILE_COUNTER_COUNT];
static int profile_sections[XGL_PROFILE_SECTION_COUNT];

double XGL_ProfileDetailNow(void) { return 1; }
void XGL_ProfileAdd(xgl_profile_section_t section, double start)
{
    assert(section >= XGL_PROFILE_WORLD_VISIBILITY && section < XGL_PROFILE_SECTION_COUNT);
    assert(start == 1);
    ++profile_sections[section];
}
void XGL_ProfileCount(xgl_profile_counter_t counter, unsigned long count)
{
    assert((unsigned int)counter < XGL_PROFILE_COUNTER_COUNT);
    profile_counts[counter] += count;
}

void Con_Printf(char *format, ...) { (void)format; }
int XGL_SMPActive(void) { return smp_active; }
void XGL_AliasPrepareFrame(void)
{
    assert(efrag_calls == 1); /* All visible entities are known. */
    assert(!captured_calls);  /* World surface drawing can overlap the worker. */
    ++alias_prepasses;
}
void XGL_AudioUpdate(void) {}
unsigned int XGL_TextureRevision(int id) { assert(id == 1 || id == 2); return texture_revision; }
unsigned int XGL_ColorRevision(void) { return color_revision; }
int XGL_PointLights(const xgl_point_light_t **lights)
{
    if (lights) *lights = test_lights;
    return test_light_count;
}
void XGL_UsePointLights(int mask) { used_lights = mask; ++use_calls; }
float XGL_AmbientLight(void) { return test_ambient; }
/* The backend's 256-entry gamma output table. World colors index it with an
 * integer level; the volatile store keeps fast math from reassociating. */
void XGL_CopyGammaOutput(float output[256])
{
    volatile float level;
    int i;
    ++gamma_copies;
    for (i = 0; i < 256; ++i) {
        level = (float)i / 255.0f;
        output[i] = level * gamma_scale;
    }
}
void XGL_PrepareColor(float rgb[3])
{
    int i;
    ++prepare_calls;
    for (i = 0; i < 3; ++i) rgb[i] *= gamma_scale;
}
void XGL_PreparePaletteColor(int index, float rgb[3])
{
    int i;
    assert(index >= 0 && index <= 255);
    ++palette_calls;
    for (i = 0; i < 3; ++i) rgb[i] = xgl_palette[index * 3 + i] / 255.0f * gamma_scale;
}
void XGL_Flush(void)
{
    int i;
    ++flush_calls;
    for (i = 0; i < queued_count; ++i)
        assert(!memcmp(queued[i], &queued_copy[i], queued_size[i]));
    queued_count = 0;
}
void Sys_Error(char *format, ...)
{
    va_list args;
    va_start(args, format); vfprintf(stderr, format, args); va_end(args);
    abort();
}
void Host_Error(char *format, ...)
{
    va_list args;
    va_start(args, format); vfprintf(stderr, format, args); va_end(args);
    abort();
}
qboolean R_CullBox(vec3_t mins, vec3_t maxs)
{
    (void)mins; (void)maxs;
    return reject_box;
}
byte *Mod_LeafPVS(mleaf_t *leaf, model_t *model)
{
    (void)leaf; (void)model;
    ++pvs_calls;
    return &test_vis;
}
void R_StoreEfrags(efrag_t **efrags)
{
    assert(*efrags == &test_efrag);
    ++efrag_calls;
}
qboolean XGL_SampleTextureIndex(int texture, float s, float t, int *index)
{
    assert(texture == 1 || texture == 2);
    ++sample_calls;
    if (failed_sample) return false;
    s -= (float)floor(s); t -= (float)floor(t);
    *index = ((int)(s * 255) + (int)(t * 127)) & 255;
    return true;
}
qboolean XGL_SampleTexture(int texture, float s, float t, float rgb[3], const byte *translation)
{
    int i, index;
    assert(texture == 1 || texture == 2);
    assert(translation == NULL);
    if (varying_material) {
        if (!XGL_SampleTextureIndex(texture, s, t, &index)) return false;
        for (i = 0; i < 3; ++i) rgb[i] = xgl_palette[index * 3 + i] / 255.0f;
    } else {
        ++sample_calls;
        rgb[0] = .8f; rgb[1] = .4f; rgb[2] = .2f;
    }
    return true;
}

/* The water tones still use the interpolating sampler. */
qboolean XGL_SampleTextureFiltered(int texture, float s, float t, float fs, float ft,
                                  float rgb[3], const byte *translation)
{
    assert(fs >= 0 && ft >= 0);
    ++filter_calls;
    return XGL_SampleTexture(texture, s, t, rgb, translation);
}

/* Wall meshes use the box sampler, in texels of the 128-texel test texture. */
qboolean XGL_SampleTextureBox(int texture, float s, float t, float footprint, byte rgb[3])
{
    float color[3], x, y, value;
    int i;
    assert(footprint >= 0);
    ++box_calls;
    if (!pattern_material) {
        if (!XGL_SampleTexture(texture, s / 128, t / 128, color, NULL)) return false;
        for (i = 0; i < 3; ++i) rgb[i] = (byte)(color[i] * 255 + .5f);
        return true;
    }
    ++sample_calls;
    x = s / 128 - (float)floor(s / 128) - .5f; y = t / 128 - (float)floor(t / 128) - .5f;
    if (pattern_material == 1) value = x * x + y * y < .1f ? .2f : .8f;
    else if (pattern_material == 2) value = x < 0 ? .2f : .8f;
    else if (pattern_material == 4)
        value = x >= -.375f && x < -.25f && y >= -.375f && y < -.25f ? .2f : .8f;
    else {
        i = ((int)floor(s / 16) * 73856093) ^ ((int)floor(t / 16) * 19349663);
        value = (float)((i >> 3) & 255) / 255;
    }
    rgb[0] = rgb[1] = rgb[2] = (byte)(value * 255 + .5f);
    return true;
}

static xgl_native_vertex_t *settle_captured(void)
{
    xgl_native_vertex_t *out;
    int i;
    for (i = 0; i < pending_count; ++i) {
        out = &captured_store[pending_first + i];
        memset(out, 0, sizeof(*out));
        VectorCopy(reserved_plain[i].position, out->position);
        VectorCopy(reserved_plain[i].color.rgb, out->color.rgb);
    }
    pending_count = 0;
    return captured_store;
}

/* How many of the captured vertices were submitted without a normal. */
static int plain_count(void)
{
    int i, count;
    for (i = count = 0; i < captured_count; ++i) count += captured_plain[i];
    return count;
}

void XGL_DrawNativeTriangles(const xgl_native_vertex_t *vertices, int count,
                             int lit, float opacity, int zwrite)
{
    xgl_native_vertex_t *out;
    int i;
    out = settle_captured();
    assert(count > 0 && count % 3 == 0 && captured_count + count <= 4096);
    assert(queued_count + count <= 4096);
    for (i = 0; i < count; ++i) {
        assert(fabs(DotProduct(vertices[i].normal, vertices[i].normal) - 1) < .0001);
        captured_plain[captured_count] = 0;
        out[captured_count++] = vertices[i];
        queued[queued_count] = vertices + i;
        queued_size[queued_count] = sizeof(vertices[i]);
        memcpy(&queued_copy[queued_count++], vertices + i, sizeof(vertices[i]));
    }
    captured_lit = lit; captured_opacity = opacity; captured_zwrite = zwrite;
    ++captured_calls;
}

void XGL_DrawColorTriangles(const xgl_color_vertex_t *vertices, int count,
                            float opacity, int zwrite)
{
    xgl_native_vertex_t *out;
    int i;
    out = settle_captured();
    assert(count > 0 && count % 3 == 0 && captured_count + count <= 4096);
    assert(queued_count + count <= 4096);
    for (i = 0; i < count; ++i) {
        memset(&out[captured_count], 0, sizeof(out[0]));
        VectorCopy(vertices[i].position, out[captured_count].position);
        VectorCopy(vertices[i].color.rgb, out[captured_count].color.rgb);
        captured_plain[captured_count++] = 1;
        queued[queued_count] = vertices + i;
        queued_size[queued_count] = sizeof(vertices[i]);
        memcpy(&queued_copy[queued_count++], vertices + i, sizeof(vertices[i]));
    }
    captured_lit = 0; captured_opacity = opacity; captured_zwrite = zwrite;
    ++captured_calls;
}

xgl_native_vertex_t *XGL_ReserveTriangles(int *count, int lit, float opacity, int zwrite)
{
    xgl_native_vertex_t *vertices;
    vertices = settle_captured() + captured_count;
    assert(*count > 0 && *count % 3 == 0);
    if (*count > 12) *count = 12; /* Exercise callers handling partial space. */
    assert(captured_count + *count <= 4096);
    memset(captured_plain + captured_count, 0, (size_t)*count);
    captured_count += *count;
    captured_lit = lit; captured_opacity = opacity; captured_zwrite = zwrite;
    ++captured_calls;
    return vertices;
}

xgl_color_vertex_t *XGL_ReserveColorTriangles(int *count, float opacity, int zwrite)
{
    settle_captured();
    assert(*count > 0 && *count % 3 == 0);
    if (*count > 12) *count = 12;
    assert(captured_count + *count <= 4096);
    memset(captured_plain + captured_count, 1, (size_t)*count);
    pending_first = captured_count; pending_count = *count;
    captured_count += *count;
    captured_lit = 0; captured_opacity = opacity; captured_zwrite = zwrite;
    ++captured_calls;
    return reserved_plain;
}

/* Four planes that either contain every box or exclude every box. */
static void set_frustum(int reject)
{
    int i;
    memset(frustum, 0, sizeof(frustum));
    for (i = 0; i < 4; ++i) {
        frustum[i].normal[0] = 1;
        frustum[i].dist = reject ? 1000000.0f : -1000000.0f;
        frustum[i].type = PLANE_ANYZ;
    }
}

static void begin_frame(void)
{
    XGL_Flush(); /* The preceding frame has completed. */
    ++r_framecount;
    captured_count = captured_calls = 0;
    memset(profile_counts, 0, sizeof(profile_counts));
    memset(profile_sections, 0, sizeof(profile_sections));
    set_frustum(reject_box);
    XGL_DrawWorld();
    XGL_DrawWorldUnlit();
    XGL_DrawWorldSpecial(); /* Sky and water follow the walls in a frame. */
}

static float captured_area(void)
{
    int i;
    float area;
    vec3_t a, b, cross;
    area = 0;
    for (i = 0; i < captured_count; i += 3) {
        VectorSubtract(captured[i+1].position, captured[i].position, a);
        VectorSubtract(captured[i+2].position, captured[i].position, b);
        CrossProduct(a, b, cross);
        area += Length(cross) * .5f;
    }
    return area;
}

static void setup_world(void)
{
    int i;
    XGL_WorldShutdown();
    fail_alloc_after = -1;
    memset(&test_model, 0, sizeof(test_model));
    memset(&test_surface, 0, sizeof(test_surface));
    memset(&test_texinfo, 0, sizeof(test_texinfo));
    memset(&test_texture, 0, sizeof(test_texture));
    memset(test_vertices, 0, sizeof(test_vertices));
    memset(test_edges, 0, sizeof(test_edges));
    memset(&test_plane, 0, sizeof(test_plane));
    memset(&test_node, 0, sizeof(test_node));
    memset(test_leaves, 0, sizeof(test_leaves));
    memset(&r_worldentity, 0, sizeof(r_worldentity));
    memset(&cl, 0, sizeof(cl));
    test_vertices[1].position[0] = 128;
    test_vertices[2].position[0] = test_vertices[2].position[1] = 128;
    test_vertices[3].position[1] = 128;
    for (i = 0; i < 4; ++i) {
        test_edges[i].v[0] = i; test_edges[i].v[1] = (i + 1) % 4;
        test_surfedges[i] = i;
    }
    test_plane.normal[2] = 1;
    test_plane.type = PLANE_Z;
    test_texinfo.vecs[0][0] = test_texinfo.vecs[1][1] = 1;
    test_texture.width = test_texture.height = 128;
    test_texture.gl_texturenum = 1;
    test_texinfo.texture = &test_texture;
    test_surface.plane = &test_plane;
    test_surface.texinfo = &test_texinfo;
    test_surface.numedges = 4;
    test_surface.extents[0] = test_surface.extents[1] = 128;
    memset(test_samples, 64, sizeof(test_samples));
    test_surface.samples = test_samples;
    memset(test_surface.styles, 255, sizeof(test_surface.styles));
    test_surface.styles[0] = 0;
    test_node.plane = &test_plane;
    test_node.numsurfaces = 1;
    test_node.children[0] = (mnode_t *)&test_leaves[1];
    test_node.children[1] = (mnode_t *)&test_leaves[2];
    test_leaves[0].contents = CONTENTS_SOLID;
    for (i = 1; i < 3; ++i) {
        test_leaves[i].contents = CONTENTS_EMPTY;
        test_leaves[i].parent = &test_node;
    }
    test_marks[0] = &test_surface;
    test_leaves[1].firstmarksurface = test_marks;
    test_leaves[1].nummarksurfaces = 1;
    test_leaves[1].efrags = &test_efrag;
    test_model.numleafs = 2;
    test_model.leafs = test_leaves;
    test_model.nodes = &test_node;
    test_model.surfaces = &test_surface;
    test_model.numsurfaces = test_model.nummodelsurfaces = 1;
    test_model.vertexes = test_vertices;
    test_model.edges = test_edges;
    test_model.surfedges = test_surfedges;
    test_model.lightdata = test_samples;
    cl.worldmodel = r_worldentity.model = &test_model;
    r_origin[0] = r_origin[1] = 64; r_origin[2] = 32;
    vpn[0] = 1; vpn[1] = vpn[2] = 0;
    r_viewleaf = &test_leaves[1];
    r_fullbright.value = r_novis.value = 0;
    r_wateralpha.value = 1;
    xgl_subdivide.value = 32;
    xgl_detailtris.value = 4096;
    xgl_detaildistance.value = 768;
    xgl_detailerror.value = 16;
    xgl_detailface.value = 192;
    xgl_detailedge.value = 16;
    /* Most contracts give every face the same limit; sharing by apparent
     * size has a test of its own. */
    xgl_detaildensity.value = 0;
    pattern_material = box_calls = 0;
    /* Most contracts exercise on-demand building without a per-frame limit;
     * map-load building and the limit have tests of their own. */
    xgl_world_prebuild = 0;
    xgl_detailbuild.value = 0;
    /* One dynamic light above the test face, well within its range. */
    memset(test_lights, 0, sizeof(test_lights));
    test_lights[0].origin[0] = test_lights[0].origin[1] = 64;
    test_lights[0].origin[2] = 32;
    test_lights[0].radius = 100; test_lights[0].range = 200;
    test_light_count = 1; test_ambient = 1;
    used_lights = use_calls = 0;
    cl_numvisedicts = 0; r_drawentities.value = 1;
    xgl_gamma_revision = 0;
    for (i = 0; i < 256; ++i) d_lightstylevalue[i] = 256;
    pvs_calls = sample_calls = efrag_calls = reject_box = captured_count = 0;
    captured_calls = varying_material = prepare_calls = filter_calls = 0;
    gamma_copies = box_tests = angle_calls = 0;
    color_revision = texture_revision = 1; gamma_scale = 1;
    palette_calls = failed_sample = flush_calls = 0;
    for (i = 0; i < 256; ++i)
        xgl_palette[i * 3] = xgl_palette[i * 3 + 1] = xgl_palette[i * 3 + 2] = (byte)i;
    realtime = 0;
    test_vis = 1;
    XGL_WorldNewMap();
}

/* A sky texture whose layers are each one palette index; 0 is a clear cloud
 * layer. Texels can then be changed to make patterns. */
static texture_t *make_sky(int front, int back)
{
    texture_t *sky;
    byte *pixels;
    int x, y;
    sky = (texture_t *)calloc(1, sizeof(texture_t) + 256 * 128);
    assert(sky != NULL);
    sky->width = 256; sky->height = 128;
    sky->offsets[0] = sizeof(texture_t);
    sky->gl_texturenum = 1;
    pixels = (byte *)sky + sky->offsets[0];
    for (y = 0; y < 128; ++y)
        for (x = 0; x < 128; ++x) {
            pixels[y * 256 + x] = (byte)front;
            pixels[y * 256 + 128 + x] = (byte)back;
        }
    test_texinfo.texture = sky;
    return sky;
}

static void test_animated_water_and_sky(void)
{
    texture_t *sky;
    byte *pixels;
    float first_color, moving;
    vec3_t first_position;
    int i, x, y, samples;
    setup_world();
    xgl_detaildistance.value = 0; /* The coarser grid: the face itself. */
    test_surface.flags = SURF_DRAWTURB;
    begin_frame();
    assert(captured_count == 6 && captured_calls == 1 && captured_lit == 0);
    /* One flat tone at the wave's zero crossing: the texture's average,
     * not whichever single texel a warped coordinate happened to land on. */
    first_color = captured[0].color.rgb[0];
    assert(fabs(first_color - .8f) < .0001);
    VectorCopy(captured[0].position, first_position);
    samples = sample_calls;
    realtime = XGL_WAVE_LONG_PERIOD / 4;
    begin_frame();
    /* The texture is not sampled again; only the wave has moved: here to
     * its crest, a 6% swell of the tone. */
    assert(captured_count == 6 && captured_calls == 1 && sample_calls == samples);
    assert(fabs(captured[0].color.rgb[0] - 216.0f / 255) < .0001);
    assert(VectorCompare(captured[0].position, first_position));
    /* Dynamic lighting never changes the water or sky's unlit material path. */
    assert(captured_lit == 0 && captured_zwrite == 1);
    reject_box = 1;
    realtime = .5;
    begin_frame();
    assert(captured_count == 0 && sample_calls == samples);

    setup_world();
    test_surface.flags = SURF_DRAWSKY;
    for (i = 0; i < 256; ++i)
        xgl_palette[i * 3] = xgl_palette[i * 3 + 1] = xgl_palette[i * 3 + 2] = i;
    sky = make_sky(0, 100);
    pixels = (byte *)sky + sky->offsets[0];
    r_origin[0] = r_origin[1] = 0;
    begin_frame();
    /* The sky has one grid at every distance, and reads its own layers. */
    assert(captured_count == 6 && captured_calls == 1 && captured_lit == 0);
    assert(sample_calls == 0 && !xgl_surfaces[0].detail.vertices);
    for (i = 0; i < 6; ++i) assert(fabs(captured[i].color.rgb[0] - 100.0f / 255) < .0001);
    /* Opaque cloud covers the back layer completely. */
    for (y = 0; y < 128; ++y) for (x = 0; x < 128; ++x) pixels[y * 256 + x] = 200;
    ++texture_revision;
    begin_frame();
    for (i = 0; i < 6; ++i) assert(fabs(captured[i].color.rgb[0] - 200.0f / 255) < .0001);
    /* Cloud over half of every cell: its color is weighted by its coverage
     * and the back layer shows through the rest. No black from the holes. */
    for (y = 0; y < 128; ++y) for (x = 0; x < 128; ++x) pixels[y * 256 + x] = (x & 1) ? 200 : 0;
    ++texture_revision;
    begin_frame();
    for (i = 0; i < 6; ++i) assert(fabs(captured[i].color.rgb[0] - 150.0f / 255) < .004);
    /* A back layer that varies across the texture scrolls with time. */
    for (y = 0; y < 128; ++y) for (x = 0; x < 128; ++x) {
        pixels[y * 256 + x] = 0;
        pixels[y * 256 + 128 + x] = (byte)(x < 64 ? 40 : 220);
    }
    ++texture_revision;
    realtime = 0;
    begin_frame();
    moving = captured[0].color.rgb[0];
    assert(moving >= 40.0f / 255 - .0001 && moving <= 220.0f / 255 + .0001);
    realtime = 3;
    begin_frame();
    assert(captured_calls == 1 && fabs(captured[0].color.rgb[0] - moving) > .01);
    /* So does moving the viewer under it. */
    moving = captured[0].color.rgb[0];
    r_origin[0] = 16;
    begin_frame();
    assert(captured_calls == 1 && captured_lit == 0);
    assert(fabs(captured[0].color.rgb[0] - moving) > .01);
    XGL_WorldShutdown();
    free(sky);

    /* A sky too small to reduce to cells falls back to one plain sample. */
    setup_world();
    test_surface.flags = SURF_DRAWSKY;
    test_texture.width = test_texture.height = 16;
    begin_frame();
    assert(captured_count == 6 && sample_calls == 4);
    assert(fabs(captured[0].color.rgb[0] - .8f) < .0001);
}

static void test_cache_and_styles(void)
{
    float original;
    int count, samples, prepares, unique;
    setup_world();
    begin_frame();
    /* Edges longer than xgl_subdivide are always split: 64 triangles here. */
    assert(captured_count == 192 && captured_lit == 1);
    assert(captured_zwrite == 1 && captured_opacity == 1);
    assert(fabs(captured_area() - 128 * 128) < .01);
    assert(xgl_cache_bytes <= XGL_WORLD_CACHE_BYTES);
    unique = xgl_surfaces[0].detail.unique;
    assert(unique == 41 && xgl_surfaces[0].detail.count == 192);
    original = captured[0].color.rgb[0];
    /* Material .8 is byte 204; a lightmap sample of 64 is half brightness:
     * level 102 of 255, taken through the gamma table. */
    assert(fabs(original - 102.0f / 255) < .0001);
    samples = sample_calls; count = captured_count; prepares = gamma_copies;
    assert(prepares == 1 && !prepare_calls);
    begin_frame();
    assert(captured_count == count && sample_calls == samples && pvs_calls == 1);
    assert(efrag_calls == 2 && gamma_copies == prepares);
    gamma_scale = .5f; ++color_revision;
    begin_frame();
    /* A gamma change refetches the table once; textures are not resampled. */
    assert(sample_calls == samples && gamma_copies == prepares + 1);
    assert(fabs(captured[0].color.rgb[0] - original * .5f) < .0001);
    gamma_scale = 1; ++color_revision;
    begin_frame();
    assert(sample_calls == samples);
    assert(fabs(captured[0].color.rgb[0] - original) < .0001);
    d_lightstylevalue[0] = 128;
    begin_frame();
    assert(sample_calls == samples);
    assert(fabs(captured[0].color.rgb[0] - original * .5f) < .0001);
    r_fullbright.value = 1;
    begin_frame();
    assert(captured_lit == 0 && sample_calls == samples);
    assert(fabs(captured[0].color.rgb[0] - .8f) < .0001);
    /* A changed texture is sampled again once for each distinct vertex,
     * not once for each of the 192 triangle corners. */
    ++texture_revision;
    begin_frame();
    assert(sample_calls == samples + unique);
    test_texture.gl_texturenum = 2;
    begin_frame();
    assert(sample_calls == samples + unique * 2);
}

static void test_pvs_and_fallback(void)
{
    setup_world();
    xgl_detailtris.value = 0;
    begin_frame();
    assert(captured_count == 6 && pvs_calls == 1);
    assert(alias_prepasses == 0);
    efrag_calls = 0;
    smp_active = 1;
    begin_frame();
    assert(alias_prepasses == 1 && captured_count == 6);
    smp_active = 0;
    test_vis = 2;
    r_viewleaf = &test_leaves[2];
    begin_frame();
    assert(captured_count == 0 && pvs_calls == 2);
    r_novis.value = 1;
    begin_frame();
    assert(captured_count == 6 && pvs_calls == 2);
    r_novis.value = 0;
    begin_frame();
    assert(captured_count == 0 && pvs_calls == 3);
    setup_world();
    xgl_cache_bytes = XGL_WORLD_CACHE_BYTES;
    begin_frame();
    assert(captured_count == 6 && xgl_surfaces[0].coarse.vertices == NULL);
    assert(fabs(captured_area() - 128 * 128) < .01);
    reject_box = 1;
    begin_frame();
    assert(captured_count == 0);
}

static void test_profile_cache_events(void)
{
    setup_world();
    begin_frame();
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 2);
    /* Vertices are counted once each, however many triangles share them. */
    assert(profile_counts[XGL_PROFILE_WORLD_BUILD_VERTICES] == 4 + 41);
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_FAILURES] == 0);
    assert(profile_counts[XGL_PROFILE_WORLD_CACHED_SURFACES] == 1);
    assert(profile_counts[XGL_PROFILE_WORLD_COLOR_UPDATES] == 1);
    assert(profile_counts[XGL_PROFILE_WORLD_COLOR_VERTICES] == 41);
    assert(profile_counts[XGL_PROFILE_WORLD_DETAIL_SURFACES] == 1);
    assert(profile_counts[XGL_PROFILE_WORLD_DETAIL_TRIANGLES] == 64);
    assert(profile_counts[XGL_PROFILE_WORLD_COLOR_HITS] == 0);
    assert(profile_sections[XGL_PROFILE_WORLD_VISIBILITY] == 1);
    assert(profile_sections[XGL_PROFILE_WORLD_DETAIL_SELECT] == 1);
    assert(profile_sections[XGL_PROFILE_WORLD_BUILD] == 2);
    assert(profile_sections[XGL_PROFILE_WORLD_COLORS] == 1);

    begin_frame();
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 0);
    assert(profile_counts[XGL_PROFILE_WORLD_CACHED_SURFACES] == 1);
    assert(profile_counts[XGL_PROFILE_WORLD_COLOR_HITS] == 1);
    assert(profile_counts[XGL_PROFILE_WORLD_COLOR_UPDATES] == 0);
    assert(profile_counts[XGL_PROFILE_WORLD_COLOR_VERTICES] == 0);
    assert(profile_sections[XGL_PROFILE_WORLD_BUILD] == 0);

    ++color_revision;
    begin_frame();
    assert(profile_counts[XGL_PROFILE_WORLD_COLOR_UPDATES] == 1);
    assert(profile_counts[XGL_PROFILE_WORLD_COLOR_VERTICES] == 41);
    assert(profile_counts[XGL_PROFILE_WORLD_COLOR_HITS] == 0);
    test_surface.flags = SURF_DRAWTURB;
    XGL_WorldNewMap();
    begin_frame();
    /* Water in range: the finer grid's 8 triangles and 9 distinct vertices. */
    assert(profile_counts[XGL_PROFILE_WORLD_ANIMATED_VERTICES] == 24);
    assert(profile_counts[XGL_PROFILE_WORLD_ANIMATED_SAMPLES] == 9);
    assert(profile_counts[XGL_PROFILE_WORLD_COLOR_UPDATES] == 0);
    assert(profile_sections[XGL_PROFILE_WORLD_ANIMATED] == 1);
    assert(profile_sections[XGL_PROFILE_WORLD_COLORS] == 0);

    setup_world();
    xgl_cache_bytes = XGL_WORLD_CACHE_BYTES;
    begin_frame();
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 0);
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_FAILURES] == 2);
    assert(profile_counts[XGL_PROFILE_WORLD_CACHED_SURFACES] == 0);
    assert(profile_counts[XGL_PROFILE_WORLD_FALLBACK_SURFACES] == 1);
    assert(profile_counts[XGL_PROFILE_WORLD_COLOR_UPDATES] == 2);
    assert(profile_counts[XGL_PROFILE_WORLD_COLOR_VERTICES] == 6);
    assert(captured_count == 6);
    begin_frame();
    /* A failed persistent allocation is not retried every frame. The
     * complete streamed fallback remains visible and counted each time. */
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_FAILURES] == 0);
    assert(profile_counts[XGL_PROFILE_WORLD_FALLBACK_SURFACES] == 1);
    assert(captured_count == 6);
}

static void test_brush_water_and_point_light(void)
{
    entity_t entity;
    byte special_samples[MAXLIGHTMAPS];
    setup_world();
    assert(R_LightPoint(r_origin) == 64);
    xgl_detailtris.value = 0;
    begin_frame();
    memset(&entity, 0, sizeof(entity));
    entity.model = &test_model;
    entity.origin[0] = 10;
    entity.angles[1] = 90;
    captured_count = 0;
    XGL_DrawBrushModel(&entity);
    assert(captured_count == 6);
    assert(fabs(captured[0].position[0] - 10) < .001);
    assert(fabs(captured[1].position[0] - 10) < .001);
    assert(fabs(captured[1].position[1] - 128) < .001);
    assert(fabs(captured_area() - 128 * 128) < .01);
    test_surface.flags = SURF_DRAWTURB;
    /* The loader expands water extents beyond its on-disk lightmap. Special
     * surfaces must never index the tiny original sample block with them. */
    test_surface.extents[0] = test_surface.extents[1] = 16384;
    XGL_SurfaceLightSamples(&test_surface, test_vertices[2].position, special_samples);
    assert(special_samples[0] == 0);
    r_wateralpha.value = .5f;
    XGL_WorldNewMap();
    begin_frame();
    assert(captured_count == 0 && xgl_water_count == 1);
    XGL_DrawWater();
    assert(captured_count == 24 && captured_lit == 0);
    assert(captured_opacity == .5f && captured_zwrite == 0);
    assert(xgl_water_count == 0);
}

static void test_projected_detail_priority(void)
{
    msurface_t surfaces[2], *marks[2];
    mvertex_t vertices[8];
    medge_t edges[8];
    int surfedges[8], i, j;
    float minimum, maximum;
    setup_world();
    surfaces[0] = surfaces[1] = test_surface;
    memset(vertices, 0, sizeof(vertices));
    memset(edges, 0, sizeof(edges));
    for (i = 0; i < 2; ++i) {
        minimum = i ? 0 : 32; maximum = i ? 128 : 96;
        vertices[i * 4].position[0] = vertices[i * 4].position[1] = minimum;
        vertices[i * 4 + 1].position[0] = maximum;
        vertices[i * 4 + 1].position[1] = minimum;
        vertices[i * 4 + 2].position[0] = vertices[i * 4 + 2].position[1] = maximum;
        vertices[i * 4 + 3].position[0] = minimum;
        vertices[i * 4 + 3].position[1] = maximum;
        for (j = 0; j < 4; ++j) {
            edges[i * 4 + j].v[0] = i * 4 + j;
            edges[i * 4 + j].v[1] = i * 4 + (j + 1) % 4;
            surfedges[i * 4 + j] = i * 4 + j;
        }
        surfaces[i].firstedge = i * 4;
        marks[i] = &surfaces[i];
    }
    test_model.surfaces = surfaces;
    test_model.numsurfaces = test_model.nummodelsurfaces = 2;
    test_model.vertexes = vertices; test_model.edges = edges; test_model.surfedges = surfedges;
    test_node.numsurfaces = 2;
    test_leaves[1].firstmarksurface = marks; test_leaves[1].nummarksurfaces = 2;
    xgl_detailtris.value = 64;
    XGL_WorldNewMap();
    begin_frame();
    /* BSP order reaches the smaller face first, but the larger screen-area
     * estimate receives the detail budget. Both faces still draw, in order. */
    assert(xgl_detail_used == 64 && captured_count == 6 + 192);
    assert(xgl_surfaces[0].detail_frame != r_framecount);
    assert(xgl_surfaces[1].detail_frame == r_framecount);
    for (i = 0; i < 6; ++i) assert(captured[i].position[0] >= 32 && captured[i].position[0] <= 96);
    for (j = 0, i = 6; i < captured_count; ++i) if (captured[i].position[0] == 0) ++j;
    assert(j > 0);
    assert(fabs(captured_area() - (64 * 64 + 128 * 128)) < .01);
    assert(xgl_cache_bytes <= XGL_WORLD_CACHE_BYTES);
    xgl_detailtris.value = 0;
    begin_frame();
    assert(captured_count == 12 && xgl_detail_used == 0);
    assert(fabs(captured_area() - (64 * 64 + 128 * 128)) < .01);
    xgl_detailtris.value = 64;
    surfaces[1].flags = SURF_DRAWTURB; r_wateralpha.value = 0;
    begin_frame();
    assert(captured_count == 48 && xgl_detail_used == 16 && !xgl_water_count);
    assert(xgl_surfaces[0].detail_frame == r_framecount);
    assert(xgl_surfaces[1].detail_frame != r_framecount);
    XGL_WorldShutdown();
}

/* Two faces side by side; defined with the tests that first used them. */
static msurface_t pair_surfaces[2];
static void setup_pair(float x0, float size0, float x1, float size1);

/* Every edge inside a mesh must be used by exactly two triangles, in
 * opposite directions, and every other edge must lie on the face's own
 * boundary: then no vertex sits in the middle of a neighbor's edge and no
 * seam can open. Corners at one position must be one distinct vertex. */
static void check_mesh(const xgl_surface_mesh_t *mesh, float low, float high, float wide)
{
    static unsigned char used[XGL_MESH_VERTICES][XGL_MESH_VERTICES];
    const float *pa, *pb;
    int i, j, a, b, boundary;
    assert(mesh->vertices && mesh->count % 3 == 0 && mesh->unique <= XGL_MESH_VERTICES);
    memset(used, 0, sizeof(used));
    for (i = 0; i < mesh->count; ++i) {
        assert(mesh->corner[i] < mesh->unique);
        for (j = 0; j < i; ++j)
            assert((mesh->corner[i] == mesh->corner[j]) ==
                   (VectorCompare(mesh->vertices[i].position, mesh->vertices[j].position) != 0));
    }
    for (i = 0; i < mesh->count; i += 3)
        for (j = 0; j < 3; ++j) {
            a = mesh->corner[i + j]; b = mesh->corner[i + (j + 1) % 3];
            assert(a != b && !used[a][b]);
            used[a][b] = 1;
        }
    for (i = 0; i < mesh->count; i += 3)
        for (j = 0; j < 3; ++j) {
            a = mesh->corner[i + j]; b = mesh->corner[i + (j + 1) % 3];
            if (used[b][a]) continue;
            pa = mesh->vertices[i + j].position; pb = mesh->vertices[i + (j + 1) % 3].position;
            boundary = 0;
            /* The face spans low..high in x and 0..wide in y. */
            if (pa[0] == pb[0] && (pa[0] == low || pa[0] == high)) boundary = 1;
            if (pa[1] == pb[1] && (pa[1] == 0 || pa[1] == wide)) boundary = 1;
            assert(boundary);
        }
}

static float mesh_area(const xgl_surface_mesh_t *mesh)
{
    vec3_t a, b, cross;
    float area;
    int i;
    area = 0;
    for (i = 0; i < mesh->count; i += 3) {
        VectorSubtract(mesh->vertices[i+1].position, mesh->vertices[i].position, a);
        VectorSubtract(mesh->vertices[i+2].position, mesh->vertices[i].position, b);
        CrossProduct(a, b, cross);
        /* Every triangle keeps the polygon's own winding. */
        assert(cross[2] > 0);
        area += cross[2] * .5f;
    }
    return area;
}

/* Triangles go where the filtered texture or the baked light differs from
 * what the mesh already interpolates, and nowhere else. */
static void test_adaptive_detail(void)
{
    xgl_surface_mesh_t *mesh;
    int i, inside, outside, plain, fine, x, y;
    float dx, dy;
    setup_world();
    xgl_subdivide.value = 256; /* Nothing is split for its size alone. */
    begin_frame();
    /* One flat color under even light: the coarse mesh already shows all
     * there is. The face is examined once and keeps no second mesh. */
    assert(captured_count == 6 && !xgl_surfaces[0].detail.vertices);
    assert(xgl_surfaces[0].detail_attempted && !xgl_detail_used);
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 1);
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_FAILURES] == 0);
    i = box_calls;
    begin_frame();
    assert(box_calls == i && captured_count == 6);

    /* A dark disc in the middle of the texture. */
    setup_world();
    xgl_subdivide.value = 256;
    pattern_material = 1;
    begin_frame();
    mesh = &xgl_surfaces[0].detail;
    assert(mesh->vertices && captured_count == mesh->count);
    assert(mesh->count / 3 > 40 && mesh->count / 3 <= 192);
    check_mesh(mesh, 0, 128, 128);
    assert(fabs(mesh_area(mesh) - 128 * 128) < .01);
    /* Small triangles crowd the disc's rim; the plain corners stay large. */
    inside = outside = 0;
    for (i = 0; i < mesh->count; i += 3) {
        dx = (mesh->vertices[i].position[0] + mesh->vertices[i+1].position[0] +
              mesh->vertices[i+2].position[0]) / 3 - 64;
        dy = (mesh->vertices[i].position[1] + mesh->vertices[i+1].position[1] +
              mesh->vertices[i+2].position[1]) / 3 - 64;
        if (dx * dx + dy * dy < 52 * 52) ++inside; else ++outside;
    }
    assert(inside > outside);
    /* The vertices carry the texture: dark within the disc, light outside. */
    for (i = 0; i < mesh->count; ++i) {
        dx = mesh->vertices[i].position[0] - 64; dy = mesh->vertices[i].position[1] - 64;
        if (dx * dx + dy * dy < 40 * 40) assert(mesh->vertices[i].color.rgb[0] < .2f);
        if (dx * dx + dy * dy > 41 * 41) assert(mesh->vertices[i].color.rgb[0] > .35f);
    }
    fine = mesh->count / 3;
    /* A larger tolerated error means fewer triangles; the cache is rebuilt. */
    xgl_detailerror.value = 100;
    begin_frame();
    mesh = &xgl_surfaces[0].detail;
    assert(mesh->vertices && mesh->count / 3 < fine);
    check_mesh(mesh, 0, 128, 128);
    /* The limit for one face is honored, largest errors first, and the mesh
     * stays whole. */
    xgl_detailerror.value = 16; xgl_detailface.value = 24;
    begin_frame();
    mesh = &xgl_surfaces[0].detail;
    assert(mesh->vertices && mesh->count / 3 <= 24 && mesh->count / 3 > 12);
    check_mesh(mesh, 0, 128, 128);
    assert(fabs(mesh_area(mesh) - 128 * 128) < .01);
    xgl_detailface.value = 100000; /* Clamped to the mesh's own capacity. */
    begin_frame();
    mesh = &xgl_surfaces[0].detail;
    assert(mesh->count / 3 >= fine && mesh->count / 3 <= XGL_MESH_TRIANGLES);
    check_mesh(mesh, 0, 128, 128);
    /* Edges no longer than xgl_detailedge texels are never split, so a
     * larger value gives a coarser mesh and a smaller one a finer. */
    xgl_detailface.value = 1024; xgl_detailedge.value = 40;
    begin_frame();
    mesh = &xgl_surfaces[0].detail;
    plain = mesh->count / 3;
    assert(mesh->vertices && plain < fine);
    check_mesh(mesh, 0, 128, 128);
    xgl_detailedge.value = 8;
    begin_frame();
    mesh = &xgl_surfaces[0].detail;
    assert(mesh->count / 3 > fine);
    check_mesh(mesh, 0, 128, 128);
    assert(fabs(mesh_area(mesh) - 128 * 128) < .01);

    /* A shadow edge in the lightmap does the same with a plain texture.
     * The 9x9 lightmap covers the face at sixteen units a sample. */
    setup_world();
    xgl_subdivide.value = 256;
    for (y = 0; y < 9; ++y) for (x = 0; x < 9; ++x) test_samples[y * 9 + x] = x < 4 ? 16 : 120;
    begin_frame();
    mesh = &xgl_surfaces[0].detail;
    assert(mesh->vertices);
    check_mesh(mesh, 0, 128, 128);
    plain = 0;
    for (i = 0; i < mesh->count; i += 3) {
        dx = (mesh->vertices[i].position[0] + mesh->vertices[i+1].position[0] +
              mesh->vertices[i+2].position[0]) / 3;
        if (dx > 96) ++plain;
    }
    /* Few triangles in the evenly lit quarter, many along the edge. */
    assert(plain * 4 < mesh->count / 3);
    /* Light styles flicker; the mesh is shaped by their nominal levels and
     * is neither rebuilt nor resampled when they change. */
    i = box_calls; fine = mesh->count;
    d_lightstylevalue[0] = 40;
    begin_frame();
    assert(box_calls == i && xgl_surfaces[0].detail.count == fine);
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 0);
    memset(test_samples, 64, sizeof(test_samples));
    XGL_WorldShutdown();
}

/* The same texture must be found on every wall, bright or dim, wherever its
 * features fall between the wall's corners. */
static void test_uniform_detail(void)
{
    xgl_surface_mesh_t *mesh;
    int i, bright, dark, x, y;
    float dx, dy;
    /* A dark disc under even light, bright and then dim. Measured in final
     * color the dim wall's contrast is a quarter of the bright one's; judged
     * against its own light it is the same, and so is the mesh. */
    setup_world();
    xgl_subdivide.value = 256;
    pattern_material = 1;
    memset(test_samples, 128, sizeof(test_samples));
    begin_frame();
    mesh = &xgl_surfaces[0].detail;
    assert(mesh->vertices);
    bright = mesh->count;
    setup_world();
    xgl_subdivide.value = 256;
    pattern_material = 1;
    memset(test_samples, 32, sizeof(test_samples));
    begin_frame();
    mesh = &xgl_surfaces[0].detail;
    assert(mesh->vertices && mesh->count == bright);
    check_mesh(mesh, 0, 128, 128);
    /* Even near black it is still found: differences are never judged
     * against less than a quarter of full light. */
    setup_world();
    xgl_subdivide.value = 256;
    pattern_material = 1;
    memset(test_samples, 6, sizeof(test_samples));
    begin_frame();
    assert(xgl_surfaces[0].detail.vertices);
    /* Under a hard shadow, both the lit and the shadowed half show it. */
    setup_world();
    xgl_subdivide.value = 256;
    pattern_material = 1;
    for (y = 0; y < 9; ++y) for (x = 0; x < 9; ++x) test_samples[y * 9 + x] = y < 4 ? 20 : 128;
    begin_frame();
    mesh = &xgl_surfaces[0].detail;
    assert(mesh->vertices);
    check_mesh(mesh, 0, 128, 128);
    bright = dark = 0;
    for (i = 0; i < mesh->count; i += 3) {
        dx = (mesh->vertices[i].position[0] + mesh->vertices[i+1].position[0] +
              mesh->vertices[i+2].position[0]) / 3 - 64;
        dy = (mesh->vertices[i].position[1] + mesh->vertices[i+1].position[1] +
              mesh->vertices[i+2].position[1]) / 3 - 64;
        if (dx * dx + dy * dy > 30 * 30 && dx * dx + dy * dy < 52 * 52) {
            if (dy < -10) ++dark;
            if (dy > 10) ++bright;
        }
    }
    assert(dark >= 6 && bright >= 6 && dark * 2 > bright && bright * 2 > dark);
    memset(test_samples, 64, sizeof(test_samples));

    /* One small dark square that lies on no corner, edge middle, quarter
     * point or center of the face. Walking the edges and the insides of the
     * large triangles finds it, and further splits close in on it. */
    setup_world();
    xgl_subdivide.value = 256;
    pattern_material = 4;
    begin_frame();
    mesh = &xgl_surfaces[0].detail;
    assert(mesh->vertices);
    check_mesh(mesh, 0, 128, 128);
    assert(fabs(mesh_area(mesh) - 128 * 128) < .01);
    dark = bright = 0;
    for (i = 0; i < mesh->count; ++i) {
        dx = mesh->vertices[i].position[0]; dy = mesh->vertices[i].position[1];
        /* The square covers 16..32 in both directions. */
        if (dx > 16 && dx < 32 && dy > 16 && dy < 32) {
            assert(mesh->vertices[i].color.rgb[0] < .2f);
            ++dark;
        }
        if (dx > 64 || dy > 64) {
            assert(mesh->vertices[i].color.rgb[0] > .35f);
            ++bright;
        }
    }
    assert(dark > 0 && bright > 0);
    /* The far half of the face has nothing to show and stays coarse. */
    dark = bright = 0;
    for (i = 0; i < mesh->count; i += 3) {
        dx = (mesh->vertices[i].position[0] + mesh->vertices[i+1].position[0] +
              mesh->vertices[i+2].position[0]) / 3;
        dy = (mesh->vertices[i].position[1] + mesh->vertices[i+1].position[1] +
              mesh->vertices[i+2].position[1]) / 3;
        if (dx < 56 && dy < 56) ++dark; else ++bright;
    }
    assert(dark > bright);
    XGL_WorldShutdown();
}

/* With a density, a face may have triangles in proportion to its apparent
 * size, so that the frame's allowance reaches every wall in range. */
static void test_detail_by_apparent_size(void)
{
    const xgl_color_vertex_t *first;
    int count, builds;
    setup_world();
    xgl_subdivide.value = 256;
    xgl_detaildistance.value = 2000;
    xgl_detaildensity.value = 160;
    pattern_material = 3; /* Busy enough to use any number of triangles. */
    XGL_WorldNewMap();
    /* 400 units above the middle of the 128-unit face: 16 triangles' worth,
     * built with half as many again in hand. */
    r_origin[2] = 400;
    begin_frame();
    assert(XGL_DetailWant(xgl_surfaces[0].area, 400.0f * 400) == 16);
    assert(xgl_surfaces[0].detail.vertices && xgl_surfaces[0].detail_limit == 24);
    count = xgl_surfaces[0].detail.count / 3;
    assert(count > 16 && count <= 24 && captured_count == count * 3);
    check_mesh(&xgl_surfaces[0].detail, 0, 128, 128);
    first = xgl_surfaces[0].detail.vertices;
    /* A little closer: the mesh it has will do. */
    r_origin[2] = 300;
    begin_frame();
    assert(xgl_surfaces[0].detail.vertices == first);
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 0);
    /* Close enough to double it: built again, larger, in one go. */
    r_origin[2] = 200;
    begin_frame();
    assert(XGL_DetailWant(xgl_surfaces[0].area, 200.0f * 200) == 65);
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 1);
    assert(xgl_surfaces[0].detail_limit == 97);
    assert(xgl_surfaces[0].detail.count / 3 > count * 2 && xgl_surfaces[0].detail.count / 3 <= 97);
    assert(captured_count == xgl_surfaces[0].detail.count);
    check_mesh(&xgl_surfaces[0].detail, 0, 128, 128);
    /* Next to it: the most one face may have. */
    r_origin[2] = 40;
    begin_frame();
    assert(XGL_DetailWant(xgl_surfaces[0].area, 40.0f * 40) == 192);
    assert(xgl_surfaces[0].detail.count / 3 > 97 && xgl_surfaces[0].detail.count / 3 <= 192);
    builds = xgl_surfaces[0].detail.count;
    first = xgl_surfaces[0].detail.vertices;
    /* Walking away again keeps the finer mesh; nothing is rebuilt. */
    r_origin[2] = 400;
    begin_frame();
    assert(xgl_surfaces[0].detail.vertices == first && captured_count == builds);
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 0);
    /* Too small on screen to be worth any detail: the coarse mesh, and no
     * build is attempted. */
    setup_world();
    xgl_subdivide.value = 256;
    xgl_detaildistance.value = 2000;
    xgl_detaildensity.value = 160;
    pattern_material = 3;
    XGL_WorldNewMap();
    r_origin[2] = 700;
    begin_frame();
    assert(XGL_DetailWant(xgl_surfaces[0].area, 700.0f * 700) < XGL_DETAIL_WANT_MIN);
    assert(captured_count == 6 && !xgl_surfaces[0].detail_attempted);
    /* A face that shows all it has within its limit is complete and is not
     * built again however close the viewer comes. */
    setup_world();
    xgl_subdivide.value = 256;
    xgl_detaildistance.value = 2000;
    xgl_detaildensity.value = 160;
    pattern_material = 4; /* One small square. */
    XGL_WorldNewMap();
    r_origin[2] = 110;
    begin_frame();
    assert(xgl_surfaces[0].detail.vertices && !xgl_surfaces[0].detail_limit);
    first = xgl_surfaces[0].detail.vertices;
    r_origin[2] = 30;
    begin_frame();
    assert(xgl_surfaces[0].detail.vertices == first);
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 0);
    /* No density: every face may have the per-face limit at any distance. */
    xgl_detaildensity.value = 0;
    assert(XGL_DetailWant(100, 1e9f) == 192);
    XGL_WorldShutdown();
}

/* Many convex faces of arbitrary shape under a busy texture: whatever the
 * refinement does, the mesh must cover the face exactly and stay whole. */
static void test_refinement_stress(void)
{
    static mvertex_t vertices[12];
    static medge_t edges[12];
    static int surfedges[12];
    const xgl_surface_mesh_t *mesh;
    static unsigned char used[XGL_MESH_VERTICES][XGL_MESH_VERTICES];
    unsigned int seed;
    float angle, radius, area, expected, cross, dx, dy, ex, ey;
    const float *pa, *pb;
    int round, i, j, k, count, a, b, on_side, detailed, valid;
    seed = 12345; detailed = valid = 0;
    for (round = 0; round < 60; ++round) {
        setup_world();
        pattern_material = 3;
        xgl_subdivide.value = 64 + (float)(round % 3) * 64;
        xgl_detailface.value = 40 + (float)(round % 7) * 60;
        xgl_detailedge.value = 8 + (float)(round % 4) * 8;
        xgl_detailerror.value = 6 + (float)(round % 5) * 8;
        count = 3 + round % 9;
        expected = 0;
        for (i = 0; i < count; ++i) {
            seed = seed * 1103515245U + 12345U;
            angle = (i + (float)((seed >> 16) & 255) / 512) * (float)(2 * M_PI) / count;
            seed = seed * 1103515245U + 12345U;
            radius = 60 + (float)((seed >> 16) & 127);
            /* Whole-unit corners, as in a map, keep every midpoint exact. */
            vertices[i].position[0] = (float)floor(200 + radius * cos(angle));
            vertices[i].position[1] = (float)floor(200 + radius * sin(angle));
            vertices[i].position[2] = 0;
            edges[i].v[0] = i; edges[i].v[1] = (i + 1) % count;
            surfedges[i] = i;
        }
        /* Keep only convex polygons: every turn must be to the left. */
        for (i = 0; i < count; ++i) {
            pa = vertices[i].position; pb = vertices[(i + 1) % count].position;
            dx = pb[0] - pa[0]; dy = pb[1] - pa[1];
            ex = vertices[(i + 2) % count].position[0] - pb[0];
            ey = vertices[(i + 2) % count].position[1] - pb[1];
            if (dx * ey - dy * ex <= 0) break;
            expected += (pa[0] * pb[1] - pb[0] * pa[1]) * .5f;
        }
        if (i < count) continue;
        ++valid;
        test_model.vertexes = vertices; test_model.edges = edges; test_model.surfedges = surfedges;
        test_surface.numedges = count;
        test_surface.extents[0] = test_surface.extents[1] = 512;
        /* An unlit map: materials at full brightness, for strong contrast. */
        test_surface.samples = NULL; test_model.lightdata = NULL;
        r_origin[0] = r_origin[1] = 200;
        XGL_WorldNewMap();
        begin_frame();
        mesh = xgl_surfaces[0].detail.vertices ? &xgl_surfaces[0].detail : &xgl_surfaces[0].coarse;
        if (mesh == &xgl_surfaces[0].detail) ++detailed;
        assert(mesh->count / 3 <= (int)xgl_detailface.value || mesh == &xgl_surfaces[0].coarse);
        assert(captured_count == mesh->count);
        area = 0;
        memset(used, 0, sizeof(used));
        for (i = 0; i < mesh->count; i += 3) {
            pa = mesh->vertices[i].position;
            dx = mesh->vertices[i+1].position[0] - pa[0]; dy = mesh->vertices[i+1].position[1] - pa[1];
            ex = mesh->vertices[i+2].position[0] - pa[0]; ey = mesh->vertices[i+2].position[1] - pa[1];
            cross = dx * ey - dy * ex;
            assert(cross > 0);
            area += cross * .5f;
            for (j = 0; j < 3; ++j) {
                a = mesh->corner[i + j]; b = mesh->corner[i + (j + 1) % 3];
                assert(a != b && a < mesh->unique && b < mesh->unique && !used[a][b]);
                used[a][b] = 1;
            }
        }
        assert(fabs(area - expected) < expected * .0001f);
        for (i = 0; i < mesh->count; i += 3)
            for (j = 0; j < 3; ++j) {
                a = mesh->corner[i + j]; b = mesh->corner[i + (j + 1) % 3];
                if (used[b][a]) continue;
                /* An edge with one triangle must lie along a side of the face. */
                pa = mesh->vertices[i + j].position; pb = mesh->vertices[i + (j + 1) % 3].position;
                on_side = 0;
                for (k = 0; k < count; ++k) {
                    dx = vertices[(k + 1) % count].position[0] - vertices[k].position[0];
                    dy = vertices[(k + 1) % count].position[1] - vertices[k].position[1];
                    if (fabs(dx * (pa[1] - vertices[k].position[1]) - dy * (pa[0] - vertices[k].position[0])) < .01 &&
                        fabs(dx * (pb[1] - vertices[k].position[1]) - dy * (pb[0] - vertices[k].position[0])) < .01)
                        on_side = 1;
                }
                assert(on_side);
            }
    }
    /* Most of the shapes drawn were convex, and all but the smallest of
     * those found something in the texture worth more triangles. */
    assert(valid >= 20 && detailed >= valid * 3 / 4);
    XGL_WorldShutdown();
    test_surface.numedges = 4;
    test_surface.extents[0] = test_surface.extents[1] = 128;
    test_surface.samples = test_samples;
}

/* A face with many corners is cut into a zig-zag of triangles rather than a
 * fan of slivers from its first corner. */
static void test_zigzag_base(void)
{
    static mvertex_t vertices[8];
    static medge_t edges[8];
    static int surfedges[8];
    const xgl_surface_mesh_t *mesh;
    int i, first;
    setup_world();
    for (i = 0; i < 8; ++i) {
        vertices[i].position[0] = 64 + 60 * (float)cos(i * M_PI / 4);
        vertices[i].position[1] = 64 + 60 * (float)sin(i * M_PI / 4);
        vertices[i].position[2] = 0;
        edges[i].v[0] = i; edges[i].v[1] = (i + 1) % 8;
        surfedges[i] = i;
    }
    test_model.vertexes = vertices; test_model.edges = edges; test_model.surfedges = surfedges;
    test_surface.numedges = 8;
    xgl_detailtris.value = 0;
    XGL_WorldNewMap();
    begin_frame();
    mesh = &xgl_surfaces[0].coarse;
    assert(mesh->count == 18 && mesh->unique == 8 && captured_count == 18);
    first = 0;
    for (i = 0; i < mesh->count; ++i) if (mesh->corner[i] == 0) ++first;
    assert(first == 1); /* A fan would use corner 0 in all six triangles. */
    assert(fabs(mesh_area(mesh) - 2 * 60 * 60 * sqrt(2.0)) < .1);
    /* Each corner's texture filter is as wide as the edges around it. */
    for (i = 0; i < mesh->unique; ++i) assert(mesh->data[i].spacing > 40 && mesh->data[i].spacing < 125);
    XGL_WorldShutdown();
    test_surface.numedges = 4;
}

/* Sky and water are cut on world grids, so that neighboring faces meet at
 * the same vertices, and each distinct vertex is colored once per frame. */
static void test_grid_meshes_and_ripples(void)
{
    xgl_surface_mesh_t *mesh, *other;
    float colors[24][3], phase;
    int i, j, on_edge, matched, differ;
    /* Two water faces side by side, 128 units square. */
    setup_pair(0, 128, 128, 128);
    pair_surfaces[0].flags = pair_surfaces[1].flags = SURF_DRAWTURB;
    varying_material = 1; /* A texture with darker and lighter parts. */
    r_origin[0] = 128;
    XGL_WorldNewMap();
    begin_frame();
    mesh = &xgl_surfaces[0].detail; other = &xgl_surfaces[1].detail;
    /* In range, each uses the 64-unit grid: 8 triangles, 9 vertices. */
    assert(captured_count == 48 && mesh->count == 24 && mesh->unique == 9);
    assert(xgl_surfaces[0].coarse.count == 6 && xgl_surfaces[0].coarse.unique == 4);
    assert(profile_counts[XGL_PROFILE_WORLD_ANIMATED_VERTICES] == 48);
    assert(profile_counts[XGL_PROFILE_WORLD_ANIMATED_SAMPLES] == 18);
    assert(!xgl_detail_used); /* Not charged to the walls' allowance. */
    check_mesh(mesh, 0, 128, 128);
    for (i = 0; i < mesh->count; ++i)
        for (j = 0; j < 2; ++j)
            assert(fmod(mesh->vertices[i].position[j], 64) == 0);
    /* Along the shared edge both faces have the same three vertices, and
     * give them the same colors: no seam between them. */
    on_edge = matched = 0;
    for (i = 0; i < mesh->count; ++i) {
        if (mesh->vertices[i].position[0] != 128) continue;
        ++on_edge;
        for (j = 0; j < other->count; ++j)
            if (VectorCompare(mesh->vertices[i].position, other->vertices[j].position)) {
                assert(VectorCompare(mesh->vertices[i].color.rgb, other->vertices[j].color.rgb));
                ++matched;
                break;
            }
    }
    assert(on_edge >= 3 && matched == on_edge);
    /* The texture was sampled once to find its two tones, not per vertex. */
    assert(sample_calls == 64 && filter_calls == 64);
    realtime = 1.25;
    begin_frame();
    assert(sample_calls == 64);
    /* The short ripple belongs to the finer grid alone and fades out with
     * distance, so a far face on the coarse grid joins it without a step. */
    for (i = 0; i < 24; ++i) VectorCopy(mesh->vertices[i].color.rgb, colors[i]);
    XGL_UpdateSurfaceMesh(mesh, &pair_surfaces[0], &test_texture, &r_worldentity, 0);
    differ = 0;
    for (i = 0; i < 24; ++i)
        if (!VectorCompare(mesh->vertices[i].color.rgb, colors[i])) ++differ;
    assert(differ > 0);
    for (i = 0; i < 24; ++i) VectorCopy(mesh->vertices[i].color.rgb, colors[i]);
    r_origin[0] = 5000; /* Every vertex is beyond the detail distance. */
    XGL_UpdateSurfaceMesh(mesh, &pair_surfaces[0], &test_texture, &r_worldentity, 1);
    for (i = 0; i < 24; ++i) assert(VectorCompare(mesh->vertices[i].color.rgb, colors[i]));
    /* Out of range the face is drawn on the coarse grid. */
    begin_frame();
    assert(captured_count == 12);
    /* Colors stay between the texture's two tones (and the swell), and the
     * wave repeats exactly after its period, however long the session. */
    r_origin[0] = 128;
    for (phase = 0; phase < 3; phase += .37f) {
        realtime = phase;
        XGL_UpdateSurfaceMesh(mesh, &pair_surfaces[0], &test_texture, &r_worldentity, 1);
        for (i = 0; i < 24; ++i) {
            VectorCopy(mesh->vertices[i].color.rgb, colors[i]);
            assert(colors[i][0] > 0 && colors[i][0] < 1);
        }
        realtime = phase + 21.0 * 1000000; /* A multiple of both periods. */
        XGL_UpdateSurfaceMesh(mesh, &pair_surfaces[0], &test_texture, &r_worldentity, 1);
        for (i = 0; i < 24; ++i)
            assert(fabs(mesh->vertices[i].color.rgb[0] - colors[i][0]) < .02);
    }
    /* A failed texture sample leaves a neutral tone, never garbage. */
    failed_sample = 1; ++texture_revision;
    XGL_UpdateSurfaceMesh(mesh, &pair_surfaces[0], &test_texture, &r_worldentity, 0);
    for (i = 0; i < 24; ++i)
        assert(mesh->vertices[i].color.rgb[0] > .45f && mesh->vertices[i].color.rgb[0] < .56f);
    failed_sample = 0;

    /* A face that does not start on a grid line is still cut on the world's
     * lines, and pieces narrower than eight units are not cut off. */
    setup_pair(-70, 260, 2000, 16);
    pair_surfaces[0].flags = SURF_DRAWSKY;
    xgl_detailtris.value = 0;
    XGL_WorldNewMap();
    begin_frame();
    mesh = &xgl_surfaces[0].coarse;
    check_mesh(mesh, -70, 190, 260);
    for (i = 0; i < mesh->count; ++i) {
        assert(mesh->vertices[i].position[0] == -70 || mesh->vertices[i].position[0] == 190 ||
               fmod(mesh->vertices[i].position[0], 128) == 0);
        assert(mesh->vertices[i].position[1] == 260 || fmod(mesh->vertices[i].position[1], 128) == 0);
    }
    /* x: -70, 0, 128, 190; y: 0, 128, 256 with the last four units left on. */
    assert(mesh->unique == 4 * 3 && mesh->count == 3 * 2 * 6);
    XGL_WorldShutdown();
}

static void test_borrowed_mesh_lifetime(void)
{
    int before;
    float old_color;
    setup_world();
    xgl_detailtris.value = 0;
    begin_frame();
    assert(queued_count == 6);
    before = flush_calls;
    XGL_DrawSurface(&test_model, &test_surface, &r_worldentity, 1);
    assert(flush_calls == before && queued_count == 12); /* Unchanged cache batches. */
    old_color = captured[0].color.rgb[0];
    gamma_scale = .5f; ++color_revision;
    XGL_DrawSurface(&test_model, &test_surface, &r_worldentity, 1);
    assert(flush_calls == before + 1 && queued_count == 6);
    assert(captured[12].color.rgb[0] == old_color * .5f);
    XGL_WorldShutdown(); /* The mock checks pointers before the arrays are freed. */
    assert(!queued_count);

    setup_world();
    test_surface.flags = SURF_DRAWTURB; xgl_detaildistance.value = 0;
    begin_frame();
    before = flush_calls; old_color = captured[0].color.rgb[0];
    realtime = .25;
    XGL_DrawSurface(&test_model, &test_surface, &r_worldentity, 1);
    assert(flush_calls == before + 1 && queued_count == 6);
    assert(captured[6].color.rgb[0] != old_color);
    XGL_WorldShutdown();
}

/* Two square faces on the z=0 plane, in BSP order, spanning the given x
 * ranges and 0..size in y. Storage is static: the world keeps pointers. */
static msurface_t pair_surfaces[2], *pair_marks[2];
static mvertex_t pair_vertices[8];
static medge_t pair_edges[8];
static int pair_surfedges[8];

static void setup_pair(float x0, float size0, float x1, float size1)
{
    float minimum, size;
    int i, j;
    setup_world();
    pair_surfaces[0] = pair_surfaces[1] = test_surface;
    memset(pair_vertices, 0, sizeof(pair_vertices));
    memset(pair_edges, 0, sizeof(pair_edges));
    for (i = 0; i < 2; ++i) {
        minimum = i ? x1 : x0; size = i ? size1 : size0;
        pair_vertices[i * 4].position[0] = minimum;
        pair_vertices[i * 4 + 1].position[0] = minimum + size;
        pair_vertices[i * 4 + 2].position[0] = minimum + size;
        pair_vertices[i * 4 + 2].position[1] = size;
        pair_vertices[i * 4 + 3].position[0] = minimum;
        pair_vertices[i * 4 + 3].position[1] = size;
        for (j = 0; j < 4; ++j) {
            pair_edges[i * 4 + j].v[0] = i * 4 + j;
            pair_edges[i * 4 + j].v[1] = i * 4 + (j + 1) % 4;
            pair_surfedges[i * 4 + j] = i * 4 + j;
        }
        pair_surfaces[i].firstedge = i * 4;
        pair_marks[i] = &pair_surfaces[i];
    }
    test_model.surfaces = pair_surfaces;
    test_model.numsurfaces = test_model.nummodelsurfaces = 2;
    test_model.vertexes = pair_vertices; test_model.edges = pair_edges;
    test_model.surfedges = pair_surfedges;
    test_node.numsurfaces = 2;
    test_leaves[1].firstmarksurface = pair_marks; test_leaves[1].nummarksurfaces = 2;
    XGL_WorldNewMap();
}

static void test_unlit_without_dynamic_light(void)
{
    entity_t entity;
    float lit_color;
    setup_world();
    xgl_detailtris.value = 0;
    begin_frame();
    assert(captured_count == 6 && captured_lit == 1);
    lit_color = captured[0].color.rgb[0];
    /* Baked light is in the vertex colors. With no dynamic light and full
     * ambient, illumination would return those colors unchanged, so walls
     * and brush entities are submitted unlit, with exactly the same colors. */
    test_light_count = 0;
    begin_frame();
    assert(captured_count == 6 && captured_lit == 0 && captured_zwrite == 1);
    assert(captured[0].color.rgb[0] == lit_color);
    assert(profile_counts[XGL_PROFILE_WORLD_COLOR_HITS] == 1);
    assert(!used_lights && !profile_counts[XGL_PROFILE_WORLD_LIT_SURFACES]);
    memset(&entity, 0, sizeof(entity));
    entity.model = &test_model;
    captured_count = 0;
    XGL_DrawBrushModel(&entity);
    /* Unlit geometry carries no normals, whichever way it is submitted. */
    assert(captured_count == 6 && captured_lit == 0 && plain_count() == 6);
    test_light_count = 1;
    begin_frame();
    captured_count = 0;
    XGL_DrawBrushModel(&entity);
    assert(captured_count == 6 && captured_lit == 1 && plain_count() == 0);
    assert(captured[0].normal[2] == 1);
    /* The streamed fallback follows the same rule. */
    setup_world();
    xgl_cache_bytes = XGL_WORLD_CACHE_BYTES;
    test_light_count = 0;
    begin_frame();
    assert(captured_count == 6 && captured_lit == 0 && plain_count() == 6);
    assert(profile_counts[XGL_PROFILE_WORLD_FALLBACK_SURFACES] == 1);
    test_light_count = 1;
    begin_frame();
    assert(captured_count == 6 && captured_lit == 1 && plain_count() == 0);
    assert(captured[0].normal[2] == 1);
}

/* Walls and brush entities are illuminated only where a dynamic light can
 * reach them; everything else stays on the cheaper unlit path. */
static void test_dynamic_light_range(void)
{
    static model_t box;
    entity_t entity;
    int frame;
    /* Two walls 1024 units apart, with the light above the first. */
    setup_pair(0, 128, 1024, 128);
    xgl_detailtris.value = 0;
    for (frame = 0; frame < 2; ++frame) {
        XGL_Flush(); ++r_framecount; captured_count = captured_calls = 0;
        memset(profile_counts, 0, sizeof(profile_counts));
        set_frustum(0);
        XGL_DrawWorld();
        /* The wall in reach is drawn first, illuminated... */
        assert(captured_count == 6 && captured_lit == 1 && captured[0].position[0] == 0);
        assert(used_lights == 1 && xgl_light_split);
        assert(profile_counts[XGL_PROFILE_WORLD_LIT_SURFACES] == 1);
        assert(profile_counts[XGL_PROFILE_POINT_LIGHTS] == 1);
        /* ...and the other one afterwards, unlit: one state change. */
        XGL_DrawWorldUnlit();
        assert(captured_count == 12 && captured_lit == 0 && captured[6].position[0] == 1024);
        /* A second light that reaches nothing visible is switched off. */
        test_light_count = 2;
        test_lights[1] = test_lights[0];
        test_lights[1].origin[1] = 5000;
    }
    /* Carry the first light to the far wall: the walls swap passes. */
    test_lights[0].origin[0] = 1088;
    begin_frame();
    assert(captured_count == 12 && captured[0].position[0] == 1024 && captured[6].position[0] == 0);
    assert(used_lights == 1);
    /* Between the walls it reaches both: one illuminated pass. */
    test_lights[0].origin[0] = 576; test_lights[0].range = 520;
    XGL_Flush(); ++r_framecount; captured_count = 0; set_frustum(0);
    XGL_DrawWorld();
    assert(captured_count == 12 && captured_lit == 1 && !xgl_light_split);
    XGL_DrawWorldUnlit();
    assert(captured_count == 12);
    /* A light behind a wall's plane adds nothing to it. */
    test_lights[0].origin[2] = -32;
    XGL_Flush(); ++r_framecount; captured_count = 0;
    memset(profile_counts, 0, sizeof(profile_counts));
    XGL_DrawWorld();
    assert(captured_count == 12 && captured_lit == 0 && !used_lights && !xgl_light_split);
    assert(!profile_counts[XGL_PROFILE_WORLD_LIT_SURFACES]);
    /* Nor does one whose range ends above the plane. */
    test_lights[0].origin[2] = 521;
    begin_frame();
    assert(captured_count == 12 && captured_lit == 0 && !used_lights);
    test_lights[0].origin[2] = 519; test_lights[0].origin[0] = 64;
    begin_frame();
    assert(captured_count == 12 && used_lights == 1 && captured[0].position[0] == 0);
    assert(xgl_light_split);
    /* A range of zero is no limit: every wall facing a light is lit. */
    test_lights[0].range = 0; test_lights[0].origin[2] = 4000;
    begin_frame();
    assert(captured_count == 12 && captured_lit == 1 && !xgl_light_split && used_lights == 1);
    /* Reduced ambient light illuminates every wall, with or without lights. */
    test_light_count = 0; test_ambient = .5f;
    memset(profile_counts, 0, sizeof(profile_counts));
    begin_frame();
    assert(captured_count == 12 && captured_lit == 1 && !used_lights && !xgl_light_split);
    assert(profile_counts[XGL_PROFILE_WORLD_LIT_SURFACES] == 2);
    assert(!profile_counts[XGL_PROFILE_POINT_LIGHTS]);
    r_fullbright.value = 1;
    begin_frame();
    assert(captured_count == 12 && captured_lit == 0);
    r_fullbright.value = 0; test_ambient = 1;

    /* A brush entity is illuminated as a whole when its bounds are in reach
     * of a light, and its light counts toward those the frame enables. */
    box = test_model;
    box.mins[0] = box.mins[1] = 0; box.maxs[0] = box.maxs[1] = 128;
    memset(&entity, 0, sizeof(entity));
    entity.model = &box;
    entity.origin[1] = 4936; /* Centered under the second light only. */
    test_light_count = 2;
    test_lights[0].origin[0] = 64; test_lights[0].origin[2] = 32; test_lights[0].range = 200;
    test_lights[1].origin[0] = 64; test_lights[1].range = 200;
    cl_visedicts[0] = &entity; cl_numvisedicts = 1;
    box.type = mod_brush;
    begin_frame();
    assert(used_lights == 3 && XGL_BrushEntityLights(&entity) == 2);
    captured_count = 0;
    XGL_DrawBrushModel(&entity);
    assert(captured_count == 12 && captured_lit == 1);
    entity.origin[1] = 2000;
    begin_frame();
    assert(used_lights == 1 && !XGL_BrushEntityLights(&entity));
    captured_count = 0;
    XGL_DrawBrushModel(&entity);
    assert(captured_count == 12 && captured_lit == 0);
    /* A rotated entity is bounded by its radius about its origin. */
    entity.angles[1] = 45; box.radius = 64;
    entity.origin[0] = 64; entity.origin[2] = 32;
    entity.origin[1] = 5000 + 263;
    assert(XGL_BrushEntityLights(&entity) == 2);
    entity.origin[1] = 5000 + 265;
    assert(!XGL_BrushEntityLights(&entity));
    r_drawentities.value = 0; entity.origin[1] = 5000;
    begin_frame();
    assert(used_lights == 1); /* Hidden entities need no light. */
    r_drawentities.value = 1;
    test_ambient = .5f;
    begin_frame();
    assert(XGL_BrushEntityLights(&entity) == (XGL_LIT_AMBIENT | 2));
    r_fullbright.value = 1;
    assert(!XGL_BrushEntityLights(&entity));
    r_fullbright.value = 0; test_ambient = 1;
    cl_numvisedicts = 0;
    XGL_WorldShutdown();
}

static void test_map_load_prebuild(void)
{
    int samples;
    setup_world();
    xgl_world_prebuild = 1;
    XGL_WorldNewMap();
    /* The coarse mesh, its bounds and its filtered material samples exist
     * before the first frame; no light or gamma work was needed for them. */
    assert(xgl_surfaces[0].coarse.vertices && xgl_surfaces[0].coarse.count == 6);
    assert(xgl_surfaces[0].coarse_attempted && xgl_surfaces[0].center_valid);
    /* One filtered sample for each of the face's four corners. */
    assert(box_calls == 4 && !filter_calls && !gamma_copies && !captured_calls);
    assert(xgl_surfaces[0].coarse.texture == 1 && xgl_surfaces[0].coarse.unique == 4);
    samples = sample_calls;
    xgl_detailtris.value = 0;
    begin_frame();
    /* The first frame only applies light: nothing is built or sampled. */
    assert(captured_count == 6 && sample_calls == samples && box_calls == 4);
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 0);
    assert(profile_counts[XGL_PROFILE_WORLD_COLOR_UPDATES] == 1);
    assert(profile_sections[XGL_PROFILE_WORLD_BUILD] == 0);
    assert(fabs(captured[0].color.rgb[0] - 102.0f / 255) < .0001);
    /* Detail is still built on demand. */
    xgl_detailtris.value = 4096;
    begin_frame();
    assert(captured_count == 192 && profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 1);
    assert(box_calls > 4 + 41);
    /* Sky and water are prebuilt as geometry only; their colors are animated. */
    setup_world();
    test_surface.flags = SURF_DRAWTURB;
    xgl_world_prebuild = 1;
    XGL_WorldNewMap();
    assert(xgl_surfaces[0].coarse.vertices && !filter_calls && !box_calls && !sample_calls);
    /* Beyond its share of the cache, load-time building leaves the remaining
     * faces to be built when they are first seen. */
    setup_world();
    xgl_world_prebuild = 1;
    xgl_prebuild_bytes = 0;
    XGL_WorldNewMap();
    xgl_prebuild_bytes = XGL_WORLD_CACHE_BYTES / 8 * 5;
    assert(!xgl_surfaces[0].coarse.vertices && !xgl_surfaces[0].coarse_attempted);
    xgl_detailtris.value = 0;
    begin_frame();
    assert(captured_count == 6 && profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 1);
    /* A failed load-time build leaves the complete streamed fallback. */
    setup_world();
    xgl_world_prebuild = 1;
    fail_alloc_after = 1; /* The cache index succeeds; the mesh does not. */
    XGL_WorldNewMap();
    fail_alloc_after = -1;
    assert(xgl_surfaces && !xgl_surfaces[0].coarse.vertices);
    xgl_detailtris.value = 0;
    begin_frame();
    assert(captured_count == 6 && profile_counts[XGL_PROFILE_WORLD_FALLBACK_SURFACES] == 1);
}

static void test_detail_build_limit(void)
{
    size_t coarse_bytes;
    setup_pair(32, 64, 0, 128);
    /* The limit counts triangles built per frame. One mesh is always
     * allowed, so the larger-ranked face is detailed now and the other in
     * the next frame. */
    xgl_detailbuild.value = 10;
    begin_frame();
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 3); /* 2 coarse + 1 detail */
    assert(xgl_surfaces[1].detail.vertices && !xgl_surfaces[0].detail.vertices);
    assert(!xgl_surfaces[0].detail_attempted && captured_count == 6 + 192);
    assert(xgl_detail_built == 64);
    begin_frame();
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 1);
    assert(xgl_surfaces[0].detail.vertices && captured_count == 48 + 192);
    begin_frame();
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 0 && captured_count == 48 + 192);
    /* Zero removes the limit. */
    setup_pair(32, 64, 0, 128);
    begin_frame();
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 4 && captured_count == 48 + 192);
    /* A face too small to subdivide keeps no second mesh, and examining it
     * still spends some of the frame's allowance: here, all of it. */
    setup_pair(0, 16, 64, 16);
    xgl_detailbuild.value = XGL_DETAIL_ATTEMPT;
    begin_frame();
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 2 && captured_count == 12);
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_FAILURES] == 0);
    assert(xgl_surfaces[0].detail_attempted + xgl_surfaces[1].detail_attempted == 1);
    assert(!xgl_surfaces[0].detail.vertices && !xgl_surfaces[1].detail.vertices);
    coarse_bytes = (6 * (sizeof(xgl_color_vertex_t) + sizeof(unsigned short)) +
                    4 * sizeof(xgl_cached_vertex_t) + 7) & ~(size_t)7;
    /* The test's light reaches both faces: each has an illuminated copy. */
    assert(xgl_surfaces[0].coarse.lit && xgl_surfaces[1].coarse.lit);
    assert(xgl_cache_bytes == 2 * (sizeof(*xgl_surfaces) + 3 * sizeof(int)) + 2 * coarse_bytes +
                              2 * 6 * sizeof(xgl_native_vertex_t));
    begin_frame();
    assert(xgl_surfaces[0].detail_attempted && xgl_surfaces[1].detail_attempted);
    begin_frame();
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 0 && captured_count == 12);
    assert(!xgl_detail_built);
    XGL_WorldShutdown();
}

static int pair_bucket(int face)
{
    vec3_t delta;
    VectorSubtract(xgl_surfaces[face].center, r_origin, delta);
    return XGL_DetailBucket(xgl_surfaces[face].area / (DotProduct(delta, delta) + 64));
}

static void test_detail_hysteresis(void)
{
    /* Equal faces side by side; the allowance covers the detail of one. */
    setup_pair(0, 128, 128, 128);
    xgl_detailtris.value = 64;
    r_origin[0] = 64;
    begin_frame();
    assert(pair_bucket(0) > pair_bucket(1) + XGL_DETAIL_KEEP);
    assert(xgl_surfaces[0].detail_frame == r_framecount && xgl_detail_used == 64);
    assert(xgl_surfaces[1].detail_frame != r_framecount);
    /* The other face now ranks one step higher. Without the preference for
     * last frame's choice the detail would hop across; it stays put. */
    r_origin[0] = 132;
    begin_frame();
    assert(pair_bucket(1) == pair_bucket(0) + 1);
    assert(xgl_surfaces[0].detail_frame == r_framecount);
    assert(xgl_surfaces[1].detail_frame != r_framecount);
    begin_frame();
    assert(xgl_surfaces[0].detail_frame == r_framecount);
    /* A clearly larger estimate still takes the allowance. */
    r_origin[0] = 150;
    begin_frame();
    assert(pair_bucket(1) > pair_bucket(0) + XGL_DETAIL_KEEP);
    assert(xgl_surfaces[1].detail_frame == r_framecount);
    assert(xgl_surfaces[0].detail_frame != r_framecount);
    /* And then it is the new holder that is kept. */
    r_origin[0] = 132;
    begin_frame();
    assert(xgl_surfaces[1].detail_frame == r_framecount);
    r_origin[0] = 124;
    begin_frame();
    assert(pair_bucket(0) == pair_bucket(1) + 1);
    assert(xgl_surfaces[1].detail_frame == r_framecount);
    /* The estimate's rank grows with size and is bounded at both ends. */
    assert(XGL_DetailBucket(0) == 0 && XGL_DetailBucket(-1) == 0);
    assert(XGL_DetailBucket(1e-9f) == 0);
    assert(XGL_DetailBucket(1e9f) == XGL_DETAIL_BUCKETS - 1 - XGL_DETAIL_KEEP);
    assert(XGL_DetailBucket(2) == XGL_DetailBucket(1) + 4);
    assert(XGL_DetailBucket(1.5f) == XGL_DetailBucket(1) + 2);
    XGL_WorldShutdown();
}

static void test_detail_eviction(void)
{
    size_t detail_bytes, full;
    detail_bytes = (192 * (sizeof(xgl_color_vertex_t) + sizeof(unsigned short)) +
                    41 * sizeof(xgl_cached_vertex_t) + 7) & ~(size_t)7;
    setup_pair(0, 128, 1024, 128);
    test_light_count = 0; /* Illuminated copies have a test of their own. */
    xgl_detaildistance.value = 200; /* Only the face near the viewer. */
    r_origin[0] = 64;
    begin_frame();
    assert(xgl_surfaces[0].detail.vertices && !xgl_surfaces[1].detail.vertices);
    assert(xgl_surfaces[0].detail.bytes == detail_bytes);
    assert(!xgl_surfaces[0].detail.lit && plain_count() == captured_count);
    /* Pretend the rest of the 8 MiB is in use, then walk to the other face.
     * In the next frame the first detail mesh is still too recent to free:
     * the new build is refused and counted, and the face stays coarse. */
    full = xgl_cache_bytes;
    xgl_cache_bytes = XGL_WORLD_CACHE_BYTES;
    r_origin[0] = 1088;
    begin_frame();
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_FAILURES] == 1);
    assert(profile_counts[XGL_PROFILE_WORLD_EVICTIONS] == 0);
    assert(xgl_surfaces[0].detail.vertices && !xgl_surfaces[1].detail.vertices);
    assert(xgl_surfaces[1].detail_retry == r_framecount + XGL_DETAIL_RETRY_FRAMES);
    assert(captured_count == 12);
    begin_frame(); /* No retry yet: nothing is built or freed. */
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_FAILURES] == 0 && captured_count == 12);
    /* When the retry comes due, the idle mesh is evicted to make room. */
    r_framecount = xgl_surfaces[1].detail_retry - 1;
    begin_frame();
    assert(profile_counts[XGL_PROFILE_WORLD_EVICTIONS] == 1);
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 1);
    assert(!xgl_surfaces[0].detail.vertices && !xgl_surfaces[0].detail_attempted);
    assert(xgl_surfaces[1].detail.vertices && captured_count == 6 + 192);
    assert(xgl_cache_bytes == XGL_WORLD_CACHE_BYTES);
    /* Walking back rebuilds the first face's detail the same way. */
    r_origin[0] = 64;
    r_framecount += 2;
    begin_frame();
    assert(profile_counts[XGL_PROFILE_WORLD_EVICTIONS] == 1);
    assert(xgl_surfaces[0].detail.vertices && !xgl_surfaces[1].detail.vertices);
    assert(captured_count == 6 + 192);
    xgl_cache_bytes = full;
    XGL_WorldShutdown();
}

static void test_state_order_and_clip_flags(void)
{
    /* BSP order reaches the sky face first; it is still drawn after the wall. */
    setup_pair(0, 128, 128, 128);
    pair_surfaces[0].flags = SURF_DRAWSKY;
    xgl_detailtris.value = 0;
    XGL_WorldNewMap();
    XGL_Flush(); ++r_framecount; captured_count = captured_calls = 0;
    set_frustum(0);
    box_tests = 0;
    XGL_DrawWorld();
    assert(captured_count == 6 && captured_lit == 1 && captured[0].position[0] == 128);
    /* Every box is inside all four planes: the root's four tests settle it
     * for the whole tree. */
    assert(box_tests == 4);
    XGL_DrawWorldSpecial();
    assert(captured_count == 12 && captured_lit == 0 && captured[6].position[0] == 0);
    XGL_DrawWorldSpecial(); /* The list is the frame's; nothing accumulates. */
    assert(captured_count == 18);

    /* One plane crosses the root box: only that plane is tested again below.
     * The visible leaf lies inside it, so its faces are drawn. */
    setup_world();
    xgl_detailtris.value = 0;
    test_node.minmaxs[3] = test_node.minmaxs[4] = 128;
    test_leaves[1].minmaxs[0] = 64;
    test_leaves[1].minmaxs[3] = test_leaves[1].minmaxs[4] = 128;
    test_leaves[1].minmaxs[5] = 64;
    XGL_Flush(); ++r_framecount; captured_count = 0;
    set_frustum(0);
    frustum[2].dist = 64;
    box_tests = 0;
    XGL_DrawWorld();
    assert(captured_count == 6 && box_tests == 5);
    /* The same plane moved beyond the leaf rejects it. */
    XGL_Flush(); ++r_framecount; captured_count = 0;
    frustum[2].dist = 130;
    box_tests = 0;
    XGL_DrawWorld();
    assert(captured_count == 0 && box_tests <= 4);
}

/* Walls are stored and drawn without normals. One that a dynamic light
 * reaches is drawn from a second copy that has them, made when first
 * needed and given up again when memory is short and no light has come. */
static void test_illuminated_copies(void)
{
    xgl_surface_mesh_t *mesh;
    size_t before, lit_bytes, full, detail_bytes;
    float color;
    int i, retry;
    setup_world();
    xgl_detailtris.value = 0; /* The two-triangle coarse mesh. */
    test_light_count = 0;
    begin_frame();
    mesh = &xgl_surfaces[0].coarse;
    /* No light: the stored vertices themselves are lent, without normals. */
    assert(captured_count == 6 && captured_lit == 0 && plain_count() == 6);
    assert(queued_count == 6 && queued[0] == (const void *)mesh->vertices);
    assert(queued_size[0] == sizeof(xgl_color_vertex_t));
    assert(!mesh->lit && !mesh->lit_bytes);
    before = xgl_cache_bytes;
    /* A light arrives: a copy with the face's normal is made and lent. */
    test_light_count = 1;
    begin_frame();
    lit_bytes = 6 * sizeof(xgl_native_vertex_t);
    assert(captured_count == 6 && captured_lit == 1 && plain_count() == 0);
    assert(mesh->lit && mesh->lit_bytes == lit_bytes && xgl_cache_bytes == before + lit_bytes);
    assert(queued_count == 6 && queued[0] == (const void *)mesh->lit);
    assert(queued_size[0] == sizeof(xgl_native_vertex_t));
    for (i = 0; i < 6; ++i) {
        assert(captured[i].normal[0] == 0 && captured[i].normal[1] == 0 && captured[i].normal[2] == 1);
        assert(!memcmp(captured[i].position, mesh->vertices[i].position, sizeof(vec3_t)));
        assert(!memcmp(captured[i].color.rgb, mesh->vertices[i].color.rgb, sizeof(vec3_t)));
    }
    color = captured[0].color.rgb[0];
    /* It is kept from frame to frame... */
    begin_frame();
    assert(xgl_cache_bytes == before + lit_bytes && queued[0] == (const void *)mesh->lit);
    /* ...and follows the wall's colors. Here a light style dims while the
     * light is away; the copy is brought up to date when the light returns. */
    test_light_count = 0;
    d_lightstylevalue[0] = 128;
    begin_frame();
    assert(plain_count() == 6 && fabs(captured[0].color.rgb[0] - color * .5f) < .0001);
    assert(mesh->lit && !mesh->lit_current);
    test_light_count = 1;
    begin_frame();
    assert(plain_count() == 0 && mesh->lit_current);
    assert(fabs(captured[0].color.rgb[0] - color * .5f) < .0001);
    /* While the wall is illuminated a style change reaches both. */
    d_lightstylevalue[0] = 256;
    begin_frame();
    assert(plain_count() == 0 && fabs(captured[0].color.rgb[0] - color) < .0001);
    assert(fabs(mesh->vertices[0].color.rgb[0] - color) < .0001);
    /* A copy drawn in this frame is still lent out: nothing may free it. */
    full = xgl_cache_bytes;
    xgl_cache_bytes = XGL_WORLD_CACHE_BYTES;
    XGL_EvictDetail(1 << 20);
    assert(mesh->lit && xgl_cache_bytes == XGL_WORLD_CACHE_BYTES);
    /* Long after the light has gone, the copy is the first thing to go. */
    test_light_count = 0;
    begin_frame();
    r_framecount += 120;
    XGL_EvictDetail(16);
    assert(!mesh->lit && !mesh->lit_bytes);
    assert(xgl_cache_bytes == XGL_WORLD_CACHE_BYTES - lit_bytes);
    xgl_cache_bytes = full - lit_bytes;
    XGL_WorldShutdown();
    assert(!xgl_cache_bytes);

    /* A face seen from behind its plane is lit on that side. */
    setup_world();
    xgl_detailtris.value = 0;
    test_surface.flags = SURF_PLANEBACK;
    /* Such a face belongs to the leaf on that side; so do the viewer and
     * the light. */
    test_leaves[1].nummarksurfaces = 0;
    test_leaves[2].firstmarksurface = test_marks;
    test_leaves[2].nummarksurfaces = 1;
    r_viewleaf = &test_leaves[2];
    test_vis = 3;
    r_origin[2] = -32; test_lights[0].origin[2] = -32;
    begin_frame();
    assert(captured_count == 6 && captured_lit == 1 && plain_count() == 0);
    for (i = 0; i < 6; ++i) assert(captured[i].normal[2] == -1);
    XGL_WorldShutdown();

    /* Without room for a copy the wall is illuminated all the same: its
     * vertices are copied out with the normal, frame by frame, and the full
     * cache is not searched for room again until some time has passed. */
    setup_world();
    test_light_count = 0;
    begin_frame(); /* Builds the detail mesh; the coarse one is idle. */
    assert(xgl_surfaces[0].detail.vertices && captured_count == 192);
    detail_bytes = xgl_surfaces[0].detail.bytes;
    xgl_detailtris.value = 0; /* From here on the coarse mesh is drawn. */
    mesh = &xgl_surfaces[0].coarse;
    full = xgl_cache_bytes;
    xgl_cache_bytes = XGL_WORLD_CACHE_BYTES;
    test_light_count = 1;
    begin_frame();
    /* The idle detail mesh is too recent to give up. */
    assert(captured_count == 6 && captured_lit == 1 && plain_count() == 0 && !queued_count);
    for (i = 0; i < 6; ++i) assert(captured[i].normal[2] == 1);
    assert(!mesh->lit && xgl_surfaces[0].detail.vertices);
    assert(profile_counts[XGL_PROFILE_WORLD_EVICTIONS] == 0);
    retry = xgl_lit_retry;
    assert(retry == r_framecount + XGL_DETAIL_RETRY_FRAMES);
    r_framecount += 8;
    begin_frame(); /* It is old enough now, but it is not yet time to look. */
    assert(!mesh->lit && xgl_surfaces[0].detail.vertices && !queued_count);
    assert(xgl_lit_retry == retry && captured_lit == 1);
    r_framecount = retry - 1;
    begin_frame();
    assert(profile_counts[XGL_PROFILE_WORLD_EVICTIONS] == 1);
    assert(mesh->lit && !xgl_surfaces[0].detail.vertices && queued_count == 6);
    assert(xgl_cache_bytes == XGL_WORLD_CACHE_BYTES - detail_bytes + 6 * sizeof(xgl_native_vertex_t));
    xgl_cache_bytes = full - detail_bytes + 6 * sizeof(xgl_native_vertex_t);
    /* With room, no search is needed and none is waited for. */
    XGL_WorldShutdown();
    assert(!xgl_cache_bytes);
}

static void test_brush_entity_basis_and_external_cache(void)
{
    static model_t external;
    static msurface_t external_surfaces[XGL_BRUSH_CACHE_FACES + 1];
    entity_t entity, second;
    int i, filters;
    setup_world();
    xgl_detailtris.value = 0;
    begin_frame();
    /* An unrotated entity is translated exactly; no trigonometry per face. */
    memset(&entity, 0, sizeof(entity));
    entity.model = &test_model;
    entity.origin[0] = 10; entity.origin[1] = -20; entity.origin[2] = 30;
    captured_count = 0; angle_calls = 0;
    XGL_DrawBrushModel(&entity);
    assert(captured_count == 6 && angle_calls == 1);
    for (i = 0; i < 6; ++i) {
        assert(captured[i].position[0] == xgl_surfaces[0].coarse.vertices[i].position[0] + 10);
        assert(captured[i].position[1] == xgl_surfaces[0].coarse.vertices[i].position[1] - 20);
        assert(captured[i].position[2] == 30 && captured[i].normal[2] == 1);
    }
    /* The same orientation is reused, also by another entity. */
    second = entity; second.origin[0] = 300;
    XGL_DrawBrushModel(&second);
    XGL_DrawBrushModel(&entity);
    assert(angle_calls == 1);
    entity.angles[1] = 90;
    captured_count = 0;
    XGL_DrawBrushModel(&entity);
    assert(angle_calls == 2 && captured_count == 6);
    assert(fabs(captured[1].position[0] - 10) < .001 && fabs(captured[1].position[1] - 108) < .001);
    XGL_DrawBrushModel(&entity);
    assert(angle_calls == 2);

    /* An external brush model (an ammo box) has its own surfaces. Its meshes
     * are now built and sampled once, not streamed and resampled per frame. */
    external = test_model;
    for (i = 0; i <= XGL_BRUSH_CACHE_FACES; ++i) external_surfaces[i] = test_surface;
    external.surfaces = external_surfaces;
    external.numsurfaces = external.nummodelsurfaces = 1;
    memset(&entity, 0, sizeof(entity));
    entity.model = &external;
    entity.origin[2] = 5;
    memset(profile_counts, 0, sizeof(profile_counts));
    captured_count = 0; filters = box_calls;
    XGL_DrawBrushModel(&entity);
    assert(captured_count == 6 && captured[0].position[2] == 5);
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 1);
    assert(profile_counts[XGL_PROFILE_WORLD_FALLBACK_SURFACES] == 0);
    assert(profile_counts[XGL_PROFILE_WORLD_CACHED_SURFACES] == 1);
    assert(box_calls == filters + 4);
    begin_frame();
    memset(profile_counts, 0, sizeof(profile_counts));
    captured_count = 0;
    XGL_DrawBrushModel(&entity);
    assert(captured_count == 6 && box_calls == filters + 4);
    assert(profile_counts[XGL_PROFILE_WORLD_MESH_BUILDS] == 0);
    assert(profile_counts[XGL_PROFILE_WORLD_COLOR_HITS] == 1);
    assert(xgl_brush_cache[0].model == &external && xgl_brush_cache[0].count == 1);
    /* A model with too many faces for a slot streams, complete, as before. */
    external.numsurfaces = XGL_BRUSH_CACHE_FACES + 1;
    captured_count = 0;
    memset(profile_counts, 0, sizeof(profile_counts));
    XGL_DrawBrushModel(&entity);
    assert(captured_count == 6);
    assert(profile_counts[XGL_PROFILE_WORLD_FALLBACK_SURFACES] == 1);
    /* More models than slots: the least recently drawn one is replaced, but
     * never one drawn in the current frame. */
    external.numsurfaces = 1;
    for (i = 0; i < XGL_BRUSH_CACHE_MODELS; ++i) {
        external.surfaces = external_surfaces + i + 1;
        captured_count = 0;
        memset(profile_counts, 0, sizeof(profile_counts));
        XGL_DrawBrushModel(&entity);
        assert(captured_count == 6);
        /* Slot 0 is from an earlier frame and is reused first; when all the
         * others hold this frame's models, the next one streams. */
        assert(profile_counts[XGL_PROFILE_WORLD_FALLBACK_SURFACES] ==
               (unsigned long)(i == XGL_BRUSH_CACHE_MODELS - 1));
    }
    /* A new map releases every external cache with the world's. */
    XGL_WorldShutdown();
    assert(!xgl_cache_bytes && !xgl_brush_cache[0].faces && !xgl_brush_cache[1].faces);
}

int main(void)
{
    test_adaptive_detail();
    test_zigzag_base();
    test_uniform_detail();
    test_detail_by_apparent_size();
    test_refinement_stress();
    test_grid_meshes_and_ripples();
    test_borrowed_mesh_lifetime();
    test_cache_and_styles();
    test_projected_detail_priority();
    test_pvs_and_fallback();
    test_profile_cache_events();
    test_brush_water_and_point_light();
    test_animated_water_and_sky();
    test_unlit_without_dynamic_light();
    test_dynamic_light_range();
    test_illuminated_copies();
    test_map_load_prebuild();
    test_detail_build_limit();
    test_detail_hysteresis();
    test_detail_eviction();
    test_state_order_and_clip_flags();
    test_brush_entity_basis_and_external_cache();
    XGL_WorldShutdown();
    assert(xgl_cache_bytes == 0 && xgl_surfaces == NULL);
    puts("XGL world/light contracts passed");
    return 0;
}
