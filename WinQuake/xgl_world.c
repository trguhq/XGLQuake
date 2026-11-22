/*
 * Bounded sampled-color BSP renderer for Sun XGL.
 * Derived from Quake, Copyright (C) 1996-1997 Id Software, Inc.
 * GNU GPL version 2 or (at your option) any later version.
 */
#include "quakedef.h"
#include "xgl_audio.h"
#include "xgl_profile.h"
#include "xgl_smp.h"

#define XGL_WORLD_CACHE_BYTES (8UL * 1024UL * 1024UL)
#define XGL_SURFACE_DETAIL_VERTS 12288
#define XGL_WATER_QUEUE 4096
#define XGL_WORLD_BATCH 384
#define XGL_TURB_SCALE (256.0 / (2 * M_PI))
/* External brush models (ammo and health boxes) keep their own small mesh
 * caches. Larger or more numerous ones stream their faces as before. */
#define XGL_BRUSH_CACHE_MODELS 16
#define XGL_BRUSH_CACHE_FACES 64
/* Detail candidates are ranked in quarter-octave steps of projected size. */
#define XGL_DETAIL_BUCKETS 128
#define XGL_DETAIL_KEEP 2
#define XGL_DETAIL_RETRY_FRAMES 64
/* Light is kept in units of five times (lightmap sample * style value), so
 * that full brightness and the .08 floor are both exact integers. */
#define XGL_LIGHT_FULL 163200U
#define XGL_LIGHT_FLOOR 13056U

static const float xgl_turb_sin[256] = {
#include "gl_warp_sin.h"
};

typedef struct {
    float st[2];
    byte material[3];
    byte samples[MAXLIGHTMAPS];
} xgl_cached_vertex_t;

typedef struct {
    xgl_native_vertex_t *vertices;
    xgl_cached_vertex_t *data;
    unsigned short *sample_vertex;
    size_t bytes;
    int count, texture, style[MAXLIGHTMAPS], fullbright, queued_frame;
    unsigned int texture_revision, color_revision;
} xgl_surface_mesh_t;

typedef struct {
    xgl_surface_mesh_t coarse, detail;
    /* detail_retry: frame at which a build that found no memory may be tried
     * again; zero when the last failure was permanent or there was none. */
    int coarse_attempted, detail_attempted, detail_retry;
    vec3_t center;
    /* detail_frame: frame in which the world pass selected the detail mesh.
     * detail_used: last frame the detail mesh was built or drawn. */
    int center_valid, detail_frame, detail_used, bucket;
    float area, radius;
} xgl_surface_cache_t;

typedef struct {
    model_t *model;
    msurface_t *surface;
    entity_t *entity;
    float distance;
} xgl_water_surface_t;

typedef struct {
    model_t *model;
    msurface_t *surfaces;
    xgl_surface_cache_t *faces;
    int count, used_frame;
} xgl_brush_cache_t;

static xgl_surface_cache_t *xgl_surfaces;
static model_t *xgl_cached_world;
static size_t xgl_cache_bytes;
static int xgl_surface_count, xgl_detail_used, xgl_detail_built, xgl_evict_hand;
static int *xgl_world_order, *xgl_detail_order, *xgl_detail_sorted, xgl_world_count;
static model_t *xgl_pvs_world;
static mleaf_t *xgl_pvs_leaf;
static int xgl_pvs_novis;
static float xgl_cache_step;
static xgl_water_surface_t xgl_water[XGL_WATER_QUEUE];
static int xgl_water_count;
static xgl_brush_cache_t xgl_brush_cache[XGL_BRUSH_CACHE_MODELS];
/* Building every coarse mesh and sampling its materials when the map loads
 * keeps that work out of the first frames. Host tests switch it off to
 * exercise the on-demand path, which remains the fallback. */
static int xgl_world_prebuild = 1;
/* Load-time building stops here, leaving the rest of the cache for detail
 * meshes and for the faces of a very large map, which then build on demand. */
static size_t xgl_prebuild_bytes = XGL_WORLD_CACHE_BYTES / 8 * 5;
/* The backend's gamma table, copied when its revision changes. */
static float xgl_gamma[256];
static unsigned int xgl_gamma_revision;
/* The rotation of the brush entity drawn last. Its faces arrive together. */
static struct {
    vec3_t angles, forward, right, up;
    int valid, rotated;
} xgl_basis;

cvar_t xgl_subdivide = {"xgl_subdivide", "128", false, false, 0, NULL};
cvar_t xgl_detailtris = {"xgl_detailtris", "1024", false, false, 0, NULL};
cvar_t xgl_detaildistance = {"xgl_detaildistance", "512", false, false, 0, NULL};
cvar_t xgl_detailbuild = {"xgl_detailbuild", "256", false, false, 0, NULL};

void XGL_SurfaceLightSamples(msurface_t *, const float *, byte[MAXLIGHTMAPS]);

void XGL_SubdivideSurface (msurface_t *surface)
{
    /* The loader's sky/water hook deliberately allocates no GL polygons.
     * Ordinary and turbulent surfaces share the bounded cache below. */
    (void)surface;
}

static void XGL_FreeSurfaceMesh (xgl_surface_mesh_t *mesh)
{
    free(mesh->vertices);
    free(mesh->sample_vertex);
    xgl_cache_bytes -= mesh->bytes;
    memset(mesh, 0, sizeof(*mesh));
}

static void XGL_FreeBrushCache (xgl_brush_cache_t *entry)
{
    int i;
    for (i = 0; i < entry->count; ++i) {
        XGL_FreeSurfaceMesh(&entry->faces[i].coarse);
        XGL_FreeSurfaceMesh(&entry->faces[i].detail);
    }
    if (entry->faces) xgl_cache_bytes -= (size_t)entry->count * sizeof(*entry->faces);
    free(entry->faces);
    memset(entry, 0, sizeof(*entry));
}

void XGL_WorldShutdown (void)
{
    int i;
    XGL_Flush(); /* Native batches may still borrow cached vertex arrays. */
    for (i = 0; i < XGL_BRUSH_CACHE_MODELS; ++i) XGL_FreeBrushCache(&xgl_brush_cache[i]);
    if (xgl_surfaces) {
        for (i = 0; i < xgl_surface_count; ++i) {
            XGL_FreeSurfaceMesh(&xgl_surfaces[i].coarse);
            XGL_FreeSurfaceMesh(&xgl_surfaces[i].detail);
        }
        free(xgl_surfaces);
    }
    free(xgl_world_order);
    xgl_world_order = xgl_detail_order = xgl_detail_sorted = NULL;
    xgl_world_count = 0;
    xgl_surfaces = NULL;
    xgl_cached_world = NULL;
    xgl_surface_count = 0;
    xgl_cache_bytes = 0;
    xgl_water_count = 0;
    xgl_evict_hand = 0;
    xgl_pvs_world = NULL;
    xgl_pvs_leaf = NULL;
    xgl_basis.valid = 0;
}

static void XGL_SurfacePoint (model_t *model, msurface_t *surface, int index, vec3_t point)
{
    int edge;
    edge = model->surfedges[surface->firstedge + index];
    VectorCopy(model->vertexes[model->edges[edge >= 0 ? edge : -edge].v[edge < 0]].position, point);
}

/* Returns nonzero when the entity is rotated. Six trigonometric calls are
 * made once per orientation rather than for every face of the entity. An
 * unrotated basis is the identity, so callers may then simply translate. */
static int XGL_BrushBasis (entity_t *entity, vec3_t forward, vec3_t right, vec3_t up)
{
    if (!xgl_basis.valid || !VectorCompare(xgl_basis.angles, entity->angles)) {
        VectorCopy(entity->angles, xgl_basis.angles);
        AngleVectors(entity->angles, xgl_basis.forward, xgl_basis.right, xgl_basis.up);
        xgl_basis.rotated = entity->angles[0] || entity->angles[1] || entity->angles[2];
        xgl_basis.valid = 1;
    }
    VectorCopy(xgl_basis.forward, forward);
    VectorCopy(xgl_basis.right, right);
    VectorCopy(xgl_basis.up, up);
    return xgl_basis.rotated;
}

static void XGL_BrushPoint (const float *point, entity_t *entity,
                            const float *forward, const float *right,
                            const float *up, float *result)
{
    int i;
    for (i = 0; i < 3; ++i)
        result[i] = entity->origin[i] + point[0] * forward[i] - point[1] * right[i] + point[2] * up[i];
}

static texture_t *XGL_SurfaceTexture (texture_t *base, entity_t *entity)
{
    int tick, loops;
    if (entity && entity->frame && base->alternate_anims) base = base->alternate_anims;
    if (!base->anim_total) return base;
    tick = (int)(cl.time * 10) % base->anim_total;
    if (tick < 0) tick += base->anim_total;
    for (loops = 0; loops < 100; ++loops) {
        if (tick >= base->anim_min && tick < base->anim_max) return base;
        base = base->anim_next;
        if (!base) Host_Error("XGL: broken texture animation");
    }
    Host_Error("XGL: texture animation cycle");
    return base;
}

texture_t *R_TextureAnimation (texture_t *base)
{
    return XGL_SurfaceTexture(base, currententity);
}

static void XGL_InitCachedVertex (xgl_native_vertex_t *draw, xgl_cached_vertex_t *vertex,
                                  msurface_t *surface, const float *point)
{
    int i;
    VectorCopy(point, draw->position);
    for (i = 0; i < 3; ++i)
        draw->normal[i] = surface->plane->normal[i] * ((surface->flags & SURF_PLANEBACK) ? -1 : 1);
    for (i = 0; i < 2; ++i)
        vertex->st[i] = DotProduct(point, surface->texinfo->vecs[i]) + surface->texinfo->vecs[i][3];
    XGL_SurfaceLightSamples(surface, point, vertex->samples);
}

/* The same bounded recursion counts and emits. Texture-space edge span,
 * rather than world area alone, keeps detail useful on oblique materials. */
static int XGL_SubdivideTriangle (msurface_t *surface, const float *a, const float *b,
                                 const float *c, int depth, float step,
                                 xgl_native_vertex_t *output, xgl_cached_vertex_t *data)
{
    vec3_t ab, bc, ca;
    float sa, sb, sc, low, high;
    int axis, split, count, n;
    split = 0;
    if (depth > 0) {
        for (axis = 0; axis < 2; ++axis) {
            sa = DotProduct(a, surface->texinfo->vecs[axis]);
            sb = DotProduct(b, surface->texinfo->vecs[axis]);
            sc = DotProduct(c, surface->texinfo->vecs[axis]);
            low = sa < sb ? sa : sb; if (sc < low) low = sc;
            high = sa > sb ? sa : sb; if (sc > high) high = sc;
            if (high - low > step) split = 1;
        }
    }
    if (!split) {
        if (output) {
            XGL_InitCachedVertex(output, data, surface, a);
            XGL_InitCachedVertex(output + 1, data + 1, surface, b);
            XGL_InitCachedVertex(output + 2, data + 2, surface, c);
        }
        return 3;
    }
    for (axis = 0; axis < 3; ++axis) {
        ab[axis] = (a[axis] + b[axis]) * .5f;
        bc[axis] = (b[axis] + c[axis]) * .5f;
        ca[axis] = (c[axis] + a[axis]) * .5f;
    }
    count = XGL_SubdivideTriangle(surface, a, ab, ca, depth - 1, step, output, data);
    n = XGL_SubdivideTriangle(surface, ab, b, bc, depth - 1, step,
                              output ? output + count : NULL, data ? data + count : NULL);
    count += n;
    n = XGL_SubdivideTriangle(surface, ca, bc, c, depth - 1, step,
                              output ? output + count : NULL, data ? data + count : NULL);
    count += n;
    n = XGL_SubdivideTriangle(surface, ab, bc, ca, depth - 1, step,
                              output ? output + count : NULL, data ? data + count : NULL);
    count += n;
    return count;
}

/* Equal positions on one surface have equal UVs. Sort only an index list,
 * retaining the original triangle order and choosing the earliest corner
 * as the sample source so its color is ready before any duplicate uses it. */
static const xgl_native_vertex_t *xgl_sample_vertices;

static int XGL_SampleVertexCompare(const void *left, const void *right)
{
    int a, b, order;
    a = *(const unsigned short *)left;
    b = *(const unsigned short *)right;
    order = memcmp(xgl_sample_vertices[a].position,
                   xgl_sample_vertices[b].position, sizeof(vec3_t));
    return order ? order : a - b;
}

static void XGL_BuildSampleMap(xgl_surface_mesh_t *mesh)
{
    unsigned short *order;
    size_t bytes;
    int i, first, index;
    if (mesh->count <= 3) return;
    bytes = (size_t)mesh->count * sizeof(*order);
    /* Include temporary sorting storage in the same fixed cache budget. */
    if (bytes * 2 > XGL_WORLD_CACHE_BYTES - xgl_cache_bytes) return;
    mesh->sample_vertex = (unsigned short *)malloc(bytes);
    order = (unsigned short *)malloc(bytes);
    if (!mesh->sample_vertex || !order) {
        free(mesh->sample_vertex); free(order);
        mesh->sample_vertex = NULL;
        return;
    }
    xgl_cache_bytes += bytes * 2;
    for (i = 0; i < mesh->count; ++i) order[i] = (unsigned short)i;
    xgl_sample_vertices = mesh->vertices;
    qsort(order, mesh->count, sizeof(*order), XGL_SampleVertexCompare);
    xgl_sample_vertices = NULL;
    first = order[0];
    for (i = 0; i < mesh->count; ++i) {
        index = order[i];
        if (memcmp(mesh->vertices[index].position, mesh->vertices[first].position,
                   sizeof(vec3_t))) first = index;
        mesh->sample_vertex[index] = (unsigned short)first;
    }
    free(order);
    xgl_cache_bytes -= bytes;
    mesh->bytes += bytes;
}

static int XGL_CacheFits (size_t bytes)
{
    return xgl_cache_bytes <= XGL_WORLD_CACHE_BYTES &&
           bytes <= XGL_WORLD_CACHE_BYTES - xgl_cache_bytes;
}

static void XGL_EvictDetailFrom (xgl_surface_cache_t *cache, int age)
{
    /* A mesh drawn in this frame may still be borrowed by a native batch,
     * and one drawn in the previous frame is probably wanted again: when
     * everything is that recent the build fails instead and is retried. */
    if (!cache->detail.vertices || r_framecount - cache->detail_used < age) return;
    XGL_FreeSurfaceMesh(&cache->detail);
    cache->detail_attempted = cache->detail_retry = 0;
    XGL_ProfileCount(XGL_PROFILE_WORLD_EVICTIONS, 1);
}

/* Make room by freeing detail meshes that have not been drawn for a while,
 * longest-idle classes first. Coarse meshes stay: every face needs one. A
 * rotating start position spreads the cost across the map. */
static void XGL_EvictDetail (size_t bytes)
{
    static const int ages[3] = {120, 8, 2};
    xgl_brush_cache_t *entry;
    int pass, i, j;
    for (pass = 0; pass < 3; ++pass) {
        for (i = 0; i < xgl_surface_count; ++i) {
            if (XGL_CacheFits(bytes)) return;
            if (++xgl_evict_hand >= xgl_surface_count) xgl_evict_hand = 0;
            XGL_EvictDetailFrom(&xgl_surfaces[xgl_evict_hand], ages[pass]);
        }
        for (i = 0; i < XGL_BRUSH_CACHE_MODELS; ++i) {
            entry = &xgl_brush_cache[i];
            for (j = 0; j < entry->count; ++j) {
                if (XGL_CacheFits(bytes)) return;
                XGL_EvictDetailFrom(&entry->faces[j], ages[pass]);
            }
        }
    }
}

/* Returns 1 when built, 0 when this face can never have such a mesh, and -1
 * when the cache had no room even after evicting idle detail. */
static int XGL_BuildSurfaceMeshData (model_t *model, msurface_t *surface,
                                xgl_surface_mesh_t *mesh, int depth)
{
    vec3_t a, b, c;
    int i, count;
    size_t bytes;
    float step;
    if (surface->numedges < 3) return 0;
    step = xgl_subdivide.value;
    if (step < 8) step = 8;
    if (step > 256) step = 256;
    XGL_SurfacePoint(model, surface, 0, a);
    count = 0;
    for (i = 1; i + 1 < surface->numedges; ++i) {
        if ((i & 15) == 1) XGL_AudioUpdate();
        XGL_SurfacePoint(model, surface, i, b);
        XGL_SurfacePoint(model, surface, i + 1, c);
        count += XGL_SubdivideTriangle(surface, a, b, c, depth, step, NULL, NULL);
        if (count > XGL_SURFACE_DETAIL_VERTS) return 0;
    }
    bytes = (size_t)count * (sizeof(*mesh->vertices) + sizeof(*mesh->data));
    if (!XGL_CacheFits(bytes)) {
        XGL_EvictDetail(bytes);
        if (!XGL_CacheFits(bytes)) return -1;
    }
    mesh->vertices = (xgl_native_vertex_t *)malloc(bytes);
    if (!mesh->vertices) return -1;
    xgl_cache_bytes += bytes;
    mesh->bytes = bytes;
    mesh->sample_vertex = NULL;
    mesh->data = (xgl_cached_vertex_t *)(mesh->vertices + count);
    count = 0;
    for (i = 1; i + 1 < surface->numedges; ++i) {
        if ((i & 15) == 1) XGL_AudioUpdate();
        XGL_SurfacePoint(model, surface, i, b);
        XGL_SurfacePoint(model, surface, i + 1, c);
        count += XGL_SubdivideTriangle(surface, a, b, c, depth, step,
                                       mesh->vertices + count, mesh->data + count);
    }
    mesh->count = count;
    if (surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) XGL_BuildSampleMap(mesh);
    mesh->texture = -1;
    mesh->fullbright = -1;
    mesh->queued_frame = -1;
    mesh->texture_revision = mesh->color_revision = 0;
    for (i = 0; i < MAXLIGHTMAPS; ++i) mesh->style[i] = -1;
    return 1;
}

static int XGL_BuildSurfaceMesh (model_t *model, msurface_t *surface,
                                xgl_surface_mesh_t *mesh, int depth)
{
    double start;
    int built;
    start = XGL_ProfileDetailNow();
    built = XGL_BuildSurfaceMeshData(model, surface, mesh, depth);
    XGL_ProfileAdd(XGL_PROFILE_WORLD_BUILD, start);
    if (built > 0) {
        XGL_ProfileCount(XGL_PROFILE_WORLD_MESH_BUILDS, 1);
        XGL_ProfileCount(XGL_PROFILE_WORLD_BUILD_VERTICES, (unsigned long)mesh->count);
    } else XGL_ProfileCount(XGL_PROFILE_WORLD_MESH_FAILURES, 1);
    return built;
}

/* Build a face's detailed mesh if this frame's build allowance permits.
 * A capped or memory-starved build is simply tried again in a later frame;
 * the face draws its coarse mesh meanwhile. */
static void XGL_TryBuildDetail (model_t *model, msurface_t *surface, xgl_surface_cache_t *cache)
{
    int built;
    if (cache->detail.vertices) return;
    if (cache->detail_attempted &&
        !(cache->detail_retry && r_framecount >= cache->detail_retry)) return;
    if (xgl_detailbuild.value > 0 && xgl_detail_built >= xgl_detailbuild.value) return;
    cache->detail_attempted = 1;
    cache->detail_retry = 0;
    built = XGL_BuildSurfaceMesh(model, surface, &cache->detail, 2);
    if (built < 0) cache->detail_retry = r_framecount + XGL_DETAIL_RETRY_FRAMES;
    if (built <= 0) return;
    xgl_detail_built += cache->detail.count;
    cache->detail_used = r_framecount;
    /* A face too small to subdivide gains nothing from a second copy. */
    if (cache->coarse.vertices && cache->detail.count <= cache->coarse.count)
        XGL_FreeSurfaceMesh(&cache->detail);
}

static int XGL_RepeatCoordinate (float coordinate, int size)
{
    int index;
    coordinate -= (float)(int)coordinate;
    if (coordinate < 0) coordinate += 1;
    index = (int)(coordinate * size);
    return index < size ? index : size - 1;
}

/* GLQuake draws the background and cutout foreground as separate passes.
 * Sampling the indexed source lets us compose the two scrolling layers into
 * one RGB sample before the single XGL submission. Index zero is transparent
 * only in the left/foreground half; it is an ordinary background color. */
static void XGL_SampleSky (texture_t *texture, const float *position,
                          float slow, float fast, float rgb[3])
{
    vec3_t direction;
    float scale, s, t;
    int half, x, y, index;
    const byte *pixels;
    VectorSubtract(position, r_origin, direction);
    direction[2] *= 3;
    scale = DotProduct(direction, direction);
    scale = scale > .0001f ? 378.0f / (float)sqrt(scale) : 0;
    s = direction[0] * scale;
    t = direction[1] * scale;
    half = texture->width / 2;
    if (!half || texture->offsets[0] < sizeof(texture_t)) {
        /* A malformed/placeholder sky has no addressable split image. */
        rgb[0] = rgb[1] = rgb[2] = .5f;
        XGL_SampleTexture(texture->gl_texturenum, (s + slow) / 128,
                          (t + slow) / 128, rgb, NULL);
        XGL_PrepareColor(rgb);
        return;
    }
    pixels = (const byte *)texture + texture->offsets[0];
    x = XGL_RepeatCoordinate((s + fast) / 128, half);
    y = XGL_RepeatCoordinate((t + fast) / 128, texture->height);
    index = pixels[y * texture->width + x];
    if (!index) {
        x = XGL_RepeatCoordinate((s + slow) / 128, half);
        y = XGL_RepeatCoordinate((t + slow) / 128, texture->height);
        index = pixels[y * texture->width + half + x];
    }
    XGL_PreparePaletteColor(index, rgb);
}

static void XGL_UpdateSpecialSurface (xgl_surface_mesh_t *mesh, msurface_t *surface,
                                      texture_t *texture, entity_t *entity)
{
    xgl_cached_vertex_t *vertex;
    xgl_native_vertex_t *draw;
    vec3_t forward, right, up, position;
    float os, ot, s, t, phase, slow, fast;
    int i, index, transformed, sky, samples;
    sky = (surface->flags & SURF_DRAWSKY) != 0;
    transformed = sky && entity && entity != &r_worldentity;
    if (transformed) XGL_BrushBasis(entity, forward, right, up);
    /* Bound time before integer lookup conversion, including long sessions. */
    phase = slow = fast = 0;
    if (sky) {
        slow = (float)fmod(realtime * 8, 128);
        fast = (float)fmod(realtime * 16, 128);
    } else phase = (float)fmod(realtime, 2 * M_PI);
    samples = 0;
    for (i = 0; i < mesh->count; ++i) {
        draw = mesh->vertices + i;
        if (mesh->sample_vertex && mesh->sample_vertex[i] != i) {
            VectorCopy(mesh->vertices[mesh->sample_vertex[i]].color.rgb, draw->color.rgb);
            continue;
        }
        ++samples;
        vertex = mesh->data + i;
        if (sky) {
            if (transformed)
                XGL_BrushPoint(draw->position, entity, forward, right, up, position);
            else VectorCopy(draw->position, position);
            XGL_SampleSky(texture, position, slow, fast, draw->color.rgb);
        } else {
            /* Original turbulent UVs omit texture-vector translation and
             * repeat every 64 world texture units, just like EmitWaterPolys. */
            os = vertex->st[0] - surface->texinfo->vecs[0][3];
            ot = vertex->st[1] - surface->texinfo->vecs[1][3];
            s = (os + xgl_turb_sin[(int)((ot * .125f + phase) * XGL_TURB_SCALE) & 255]) / 64;
            t = (ot + xgl_turb_sin[(int)((os * .125f + phase) * XGL_TURB_SCALE) & 255]) / 64;
            if (XGL_SampleTextureIndex(texture->gl_texturenum, s, t, &index))
                XGL_PreparePaletteColor(index, draw->color.rgb);
            else {
                draw->color.rgb[0] = draw->color.rgb[1] = draw->color.rgb[2] = .5f;
                XGL_PrepareColor(draw->color.rgb);
            }
        }
    }
    XGL_ProfileCount(XGL_PROFILE_WORLD_ANIMATED_SAMPLES, (unsigned long)samples);
}

/* A mesh can be reused by more than one brush entity in the same frame.
 * Finish any earlier borrowed submission before changing its colors. The
 * previous frame has already completed, so normal first draws need no flush. */
static void XGL_SurfaceWillChange(xgl_surface_mesh_t *mesh)
{
    if (mesh->queued_frame == r_framecount) XGL_Flush();
    mesh->queued_frame = -1;
}

/* Filtered texture colors are sampled once per vertex and kept as bytes;
 * later light, style and gamma changes never touch the texture again. */
static void XGL_SampleMeshMaterials (xgl_surface_mesh_t *mesh, texture_t *texture,
                                     unsigned int texture_revision)
{
    xgl_cached_vertex_t *vertex, *triangle;
    float rgb[3], footprint[2], low, high, value;
    int i, j, axis, corner;
    footprint[0] = footprint[1] = 0;
    for (i = 0, vertex = mesh->data; i < mesh->count; ++i, ++vertex) {
        if ((i & 255) == 0) XGL_AudioUpdate();
        if (i % 3 == 0) {
            triangle = vertex;
            for (axis = 0; axis < 2; ++axis) {
                low = high = triangle[0].st[axis];
                for (corner = 1; corner < 3; ++corner) {
                    value = triangle[corner].st[axis];
                    if (value < low) low = value;
                    if (value > high) high = value;
                }
                footprint[axis] = (high - low) / (3 * (float)(axis ? texture->height : texture->width));
            }
        }
        rgb[0] = rgb[1] = rgb[2] = .5f;
        XGL_SampleTextureFiltered(texture->gl_texturenum, vertex->st[0] / texture->width,
                          vertex->st[1] / texture->height, footprint[0], footprint[1], rgb, NULL);
        for (j = 0; j < 3; ++j) vertex->material[j] = (byte)(rgb[j] * 255 + .5f);
    }
    mesh->texture = texture->gl_texturenum;
    mesh->texture_revision = texture_revision;
}

/* Material byte times light, rounded to the nearest of 256 levels and passed
 * through the gamma table, all in integers: this is the only per-vertex work
 * when a light style flickers. GLQuake's lightmap scale applies: a sample of
 * 128 at style value 256 is full brightness. The small floor lets native
 * diffuse lights, which multiply the vertex color, still show on dark
 * materials; that is an intentional approximation of additive dlights. */
static void XGL_ColorMesh (xgl_surface_mesh_t *mesh, msurface_t *surface, int fullbright)
{
    const xgl_cached_vertex_t *vertex;
    xgl_native_vertex_t *draw;
    unsigned int level;
    int weight[MAXLIGHTMAPS], layers, layer, value, i, unlit;
    unlit = fullbright || !cl.worldmodel->lightdata ||
        (surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWTILED));
    layers = 0;
    if (!unlit)
        for (; layers < MAXLIGHTMAPS && surface->styles[layers] != 255; ++layers)
            weight[layers] = d_lightstylevalue[surface->styles[layers]];
    level = XGL_LIGHT_FULL;
    for (i = 0, vertex = mesh->data, draw = mesh->vertices; i < mesh->count; ++i, ++vertex, ++draw) {
        if ((i & 255) == 0) XGL_AudioUpdate();
        if (!unlit) {
            value = 0;
            for (layer = 0; layer < layers; ++layer)
                value += vertex->samples[layer] * weight[layer];
            level = value > 0 ? 5U * (unsigned int)value : 0;
            if (level < XGL_LIGHT_FLOOR) level = XGL_LIGHT_FLOOR;
            if (level > XGL_LIGHT_FULL) level = XGL_LIGHT_FULL;
        }
        draw->color.rgb[0] = xgl_gamma[(vertex->material[0] * level + XGL_LIGHT_FULL / 2) / XGL_LIGHT_FULL];
        draw->color.rgb[1] = xgl_gamma[(vertex->material[1] * level + XGL_LIGHT_FULL / 2) / XGL_LIGHT_FULL];
        draw->color.rgb[2] = xgl_gamma[(vertex->material[2] * level + XGL_LIGHT_FULL / 2) / XGL_LIGHT_FULL];
    }
}

static void XGL_UpdateSurfaceMesh (xgl_surface_mesh_t *mesh, msurface_t *surface,
                                   texture_t *texture, entity_t *entity)
{
    int i, changed, fullbright, texture_changed, value;
    unsigned int texture_revision, color_revision;
    double start;
    if (surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) {
        XGL_SurfaceWillChange(mesh);
        start = XGL_ProfileDetailNow();
        XGL_UpdateSpecialSurface(mesh, surface, texture, entity);
        XGL_ProfileAdd(XGL_PROFILE_WORLD_ANIMATED, start);
        XGL_ProfileAdd((surface->flags & SURF_DRAWSKY) ?
                       XGL_PROFILE_WORLD_SKY : XGL_PROFILE_WORLD_WATER, start);
        XGL_ProfileCount(XGL_PROFILE_WORLD_ANIMATED_VERTICES, (unsigned long)mesh->count);
        return;
    }
    start = XGL_ProfileDetailNow();
    fullbright = r_fullbright.value != 0;
    texture_revision = XGL_TextureRevision(texture->gl_texturenum);
    color_revision = XGL_ColorRevision();
    texture_changed = mesh->texture != texture->gl_texturenum || mesh->texture_revision != texture_revision;
    changed = texture_changed || mesh->fullbright != fullbright || mesh->color_revision != color_revision;
    for (i = 0; i < MAXLIGHTMAPS; ++i) {
        value = surface->styles[i] == 255 ? 0 : d_lightstylevalue[surface->styles[i]];
        if (mesh->style[i] != value) changed = 1;
        mesh->style[i] = value;
    }
    if (!changed) {
        XGL_ProfileAdd(XGL_PROFILE_WORLD_COLORS, start);
        XGL_ProfileCount(XGL_PROFILE_WORLD_COLOR_HITS, 1);
        return;
    }
    XGL_SurfaceWillChange(mesh);
    if (texture_changed) XGL_SampleMeshMaterials(mesh, texture, texture_revision);
    if (xgl_gamma_revision != color_revision) {
        XGL_CopyGammaOutput(xgl_gamma);
        xgl_gamma_revision = color_revision;
    }
    XGL_ColorMesh(mesh, surface, fullbright);
    mesh->color_revision = color_revision;
    mesh->fullbright = fullbright;
    XGL_ProfileAdd(XGL_PROFILE_WORLD_COLORS, start);
    XGL_ProfileCount(XGL_PROFILE_WORLD_COLOR_UPDATES, 1);
    XGL_ProfileCount(XGL_PROFILE_WORLD_COLOR_VERTICES, (unsigned long)mesh->count);
}

static void XGL_EmitSurfaceVertices (xgl_surface_mesh_t *mesh,
                                    entity_t *entity, int lit, float opacity,
                                    int persistent)
{
    const xgl_native_vertex_t *source;
    xgl_native_vertex_t *vertices;
    vec3_t forward, right, up;
    int i, j, n, transformed, rotated, count;
    source = mesh->vertices; count = mesh->count;
    transformed = entity && entity != &r_worldentity;
    if (!transformed && persistent) {
        XGL_DrawNativeTriangles(source, count, lit, opacity, opacity >= 1);
        mesh->queued_frame = r_framecount;
        c_brush_polys += count / 3;
        return;
    }
    rotated = transformed ? XGL_BrushBasis(entity, forward, right, up) : 0;
    while (count > 0) {
        n = count > XGL_WORLD_BATCH ? XGL_WORLD_BATCH : count;
        vertices = XGL_ReserveTriangles(&n, lit, opacity, opacity >= 1);
        if (!vertices) return;
        for (i = 0; i < n; ++i) {
            vertices[i] = source[i];
            if (rotated) {
                XGL_BrushPoint(source[i].position, entity, forward, right, up, vertices[i].position);
                for (j = 0; j < 3; ++j)
                    vertices[i].normal[j] = source[i].normal[0] * forward[j] -
                        source[i].normal[1] * right[j] + source[i].normal[2] * up[j];
            } else if (transformed) {
                /* Doors, platforms and item boxes: translation only. */
                VectorAdd(source[i].position, entity->origin, vertices[i].position);
            }
        }
        c_brush_polys += n / 3;
        source += n; count -= n;
    }
}

static void XGL_SurfaceBounds(model_t *model, msurface_t *surface, xgl_surface_cache_t *cache)
{
    vec3_t point, origin, previous, a, b, cross;
    float radius_squared, length_squared;
    int i;
    if (cache->center_valid) return;
    VectorCopy(vec3_origin, cache->center);
    cache->area = cache->radius = 0;
    for (i = 0; i < surface->numedges; ++i) {
        XGL_SurfacePoint(model, surface, i, point);
        VectorAdd(cache->center, point, cache->center);
    }
    if (surface->numedges) { VectorScale(cache->center, 1.0f / surface->numedges, cache->center); }
    radius_squared = 0;
    VectorCopy(vec3_origin, origin); VectorCopy(vec3_origin, previous);
    for (i = 0; i < surface->numedges; ++i) {
        XGL_SurfacePoint(model, surface, i, point);
        VectorSubtract(point, cache->center, a);
        length_squared = DotProduct(a, a);
        if (length_squared > radius_squared) radius_squared = length_squared;
        if (i == 0) { VectorCopy(point, origin); }
        else if (i > 1) {
            VectorSubtract(previous, origin, a); VectorSubtract(point, origin, b);
            CrossProduct(a, b, cross);
            cache->area += (float)fabs(DotProduct(cross, surface->plane->normal)) * .5f;
        }
        VectorCopy(point, previous);
    }
    cache->radius = (float)sqrt(radius_squared);
    cache->center_valid = 1;
}

/* Build every coarse mesh and sample its materials while the map loads. */
static void XGL_PrebuildWorld (void)
{
    xgl_surface_cache_t *cache;
    msurface_t *surface;
    texture_t *texture;
    int i;
    for (i = 0; i < xgl_surface_count && xgl_cache_bytes < xgl_prebuild_bytes; ++i) {
        if ((i & 31) == 0) XGL_AudioUpdate();
        surface = xgl_cached_world->surfaces + i;
        cache = xgl_surfaces + i;
        cache->coarse_attempted = 1;
        if (XGL_BuildSurfaceMesh(xgl_cached_world, surface, &cache->coarse, 0) <= 0) continue;
        XGL_SurfaceBounds(xgl_cached_world, surface, cache);
        if (surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) continue;
        texture = XGL_SurfaceTexture(surface->texinfo->texture, NULL);
        if (texture && texture->width && texture->height)
            XGL_SampleMeshMaterials(&cache->coarse, texture,
                                    XGL_TextureRevision(texture->gl_texturenum));
    }
}

void XGL_WorldNewMap (void)
{
    size_t bytes, each;
    XGL_WorldShutdown();
    xgl_cached_world = cl.worldmodel;
    xgl_cache_step = xgl_subdivide.value;
    if (!xgl_cached_world || xgl_cached_world->numsurfaces <= 0) return;
    each = sizeof(*xgl_surfaces) + 3 * sizeof(int);
    if ((size_t)xgl_cached_world->numsurfaces > XGL_WORLD_CACHE_BYTES / each) return;
    bytes = (size_t)xgl_cached_world->numsurfaces * each;
    if (bytes <= XGL_WORLD_CACHE_BYTES) {
        xgl_surfaces = (xgl_surface_cache_t *)calloc((size_t)xgl_cached_world->numsurfaces, sizeof(*xgl_surfaces));
        xgl_world_order = (int *)malloc((size_t)xgl_cached_world->numsurfaces * 3 * sizeof(int));
        if (xgl_surfaces && xgl_world_order) {
            xgl_detail_order = xgl_world_order + xgl_cached_world->numsurfaces;
            xgl_detail_sorted = xgl_detail_order + xgl_cached_world->numsurfaces;
            xgl_surface_count = xgl_cached_world->numsurfaces;
            xgl_cache_bytes = bytes;
        } else {
            free(xgl_surfaces); free(xgl_world_order);
            xgl_surfaces = NULL; xgl_world_order = NULL;
        }
    }
    if (xgl_surfaces && xgl_world_prebuild) XGL_PrebuildWorld();
    Con_Printf("XGL: sampled BSP cache limited to 8 MiB; detail depth <= 2; projected-size detail priority; complete coarse fallback\n");
}

/* Quarter-octave rank of a positive projected-size estimate, read from the
 * float's exponent and leading mantissa bits; no logarithm is needed. */
static int XGL_DetailBucket (float priority)
{
    union { float value; unsigned int bits; } key;
    int bucket;
    if (!(priority > 0)) return 0;
    key.value = priority;
    bucket = (int)(key.bits >> 21) - ((127 - 14) << 2);
    if (bucket < 0) return 0;
    if (bucket > XGL_DETAIL_BUCKETS - 1 - XGL_DETAIL_KEEP)
        bucket = XGL_DETAIL_BUCKETS - 1 - XGL_DETAIL_KEEP;
    return bucket;
}

/* Reserve the fixed detail allowance for the largest visible projected-size
 * estimates, then submit in the original BSP order. This avoids spending the
 * whole allowance on small faces encountered early in the traversal. A face
 * detailed in the previous frame ranks two steps higher, so faces near the
 * cut-off do not flicker between their coarse and detailed colors. */
static void XGL_SelectWorldDetail(void)
{
    static int counts[XGL_DETAIL_BUCKETS];
    xgl_surface_cache_t *cache;
    msurface_t *surface;
    vec3_t delta;
    float distance, range;
    int i, index, candidates, bucket, start;
    candidates = 0;
    memset(counts, 0, sizeof(counts));
    for (i = 0; i < xgl_world_count; ++i) {
        if ((i & 63) == 0) XGL_AudioUpdate();
        index = xgl_world_order[i];
        cache = xgl_surfaces + index;
        surface = cl.worldmodel->surfaces + index;
        bucket = cache->detail_frame == r_framecount - 1 ? XGL_DETAIL_KEEP : 0;
        cache->detail_frame = -1;
        if ((surface->flags & SURF_DRAWTURB) && r_wateralpha.value <= 0) continue;
        /* Coarse meshes get memory first, preserving cheap complete fallback. */
        if (!cache->coarse_attempted) {
            cache->coarse_attempted = 1;
            XGL_BuildSurfaceMesh(cl.worldmodel, surface, &cache->coarse, 0);
        }
        if (!(xgl_detailtris.value > 0 && xgl_detaildistance.value > 0)) continue;
        XGL_SurfaceBounds(cl.worldmodel, surface, cache);
        VectorSubtract(cache->center, r_origin, delta);
        distance = DotProduct(delta, delta);
        range = xgl_detaildistance.value + cache->radius;
        if (distance >= range * range) continue;
        bucket += XGL_DetailBucket(cache->area / (distance + 64));
        cache->bucket = bucket;
        ++counts[bucket];
        xgl_detail_order[candidates++] = index;
    }
    /* Counting sort, largest first; equal ranks keep their BSP order. */
    start = 0;
    for (bucket = XGL_DETAIL_BUCKETS - 1; bucket >= 0; --bucket) {
        i = counts[bucket];
        counts[bucket] = start;
        start += i;
    }
    for (i = 0; i < candidates; ++i) {
        index = xgl_detail_order[i];
        xgl_detail_sorted[counts[xgl_surfaces[index].bucket]++] = index;
    }
    for (i = 0; i < candidates && xgl_detail_used < xgl_detailtris.value; ++i) {
        if ((i & 63) == 0) XGL_AudioUpdate();
        index = xgl_detail_sorted[i]; cache = xgl_surfaces + index;
        XGL_TryBuildDetail(cl.worldmodel, cl.worldmodel->surfaces + index, cache);
        if (cache->detail.vertices && cache->detail.count > cache->coarse.count &&
            xgl_detail_used + cache->detail.count / 3 <= xgl_detailtris.value) {
            cache->detail_frame = cache->detail_used = r_framecount;
            xgl_detail_used += cache->detail.count / 3;
        }
    }
}

/* The mesh cache of one face of an external brush model, or NULL when the
 * model is too large, every slot is in use this frame, or memory is short.
 * Model storage is reloaded with each map, when all of this is released. */
static xgl_surface_cache_t *XGL_BrushModelFace (model_t *model, msurface_t *surface)
{
    xgl_brush_cache_t *entry, *oldest;
    size_t bytes;
    int i, index;
    index = (int)(surface - model->surfaces);
    if (!xgl_cached_world || index < 0 || index >= model->numsurfaces ||
        model->numsurfaces > XGL_BRUSH_CACHE_FACES) return NULL;
    oldest = NULL;
    for (i = 0; i < XGL_BRUSH_CACHE_MODELS; ++i) {
        entry = &xgl_brush_cache[i];
        if (entry->faces && entry->model == model && entry->surfaces == model->surfaces &&
            entry->count == model->numsurfaces) {
            entry->used_frame = r_framecount;
            return &entry->faces[index];
        }
        if (!entry->faces) { if (!oldest || oldest->faces) oldest = entry; }
        else if (!oldest || (oldest->faces && entry->used_frame < oldest->used_frame)) oldest = entry;
    }
    /* Never release a model drawn in this frame: a batch may borrow it. */
    if (oldest->faces && oldest->used_frame == r_framecount) return NULL;
    XGL_FreeBrushCache(oldest);
    bytes = (size_t)model->numsurfaces * sizeof(*oldest->faces);
    if (!XGL_CacheFits(bytes)) {
        XGL_EvictDetail(bytes);
        if (!XGL_CacheFits(bytes)) return NULL;
    }
    oldest->faces = (xgl_surface_cache_t *)calloc((size_t)model->numsurfaces, sizeof(*oldest->faces));
    if (!oldest->faces) return NULL;
    xgl_cache_bytes += bytes;
    oldest->model = model; oldest->surfaces = model->surfaces;
    oldest->count = model->numsurfaces; oldest->used_frame = r_framecount;
    return &oldest->faces[index];
}

static void XGL_DrawSurface (model_t *model, msurface_t *surface, entity_t *entity, float opacity)
{
    xgl_surface_cache_t *cache;
    xgl_surface_mesh_t *mesh;
    xgl_surface_mesh_t fallback;
    xgl_native_vertex_t triangle[3];
    xgl_cached_vertex_t triangle_data[3];
    texture_t *texture;
    vec3_t a, b, c, center, forward, right, up, worldcenter, delta;
    float distance;
    int index, i, axis, lit, detail;
    texture = XGL_SurfaceTexture(surface->texinfo->texture, entity);
    if (!texture || !texture->width || !texture->height) return;
    /* Baked light is already in the vertex colors. Native illumination is
     * only worth its cost while a dynamic light can change the result. */
    lit = !r_fullbright.value && !(surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) &&
          XGL_LightsActive();
    cache = NULL; mesh = NULL;
    if (xgl_surfaces && model->surfaces == xgl_cached_world->surfaces) {
        index = (int)(surface - xgl_cached_world->surfaces);
        if (index >= 0 && index < xgl_surface_count) cache = &xgl_surfaces[index];
    } else cache = XGL_BrushModelFace(model, surface);
    if (cache) {
        if (!cache->coarse_attempted) {
            cache->coarse_attempted = 1;
            XGL_BuildSurfaceMesh(model, surface, &cache->coarse, 0);
        }
        mesh = &cache->coarse;
        if (entity == &r_worldentity && xgl_world_order) {
            if (cache->detail_frame == r_framecount && cache->detail.vertices)
                mesh = &cache->detail;
        } else {
            XGL_SurfaceBounds(model, surface, cache);
            VectorCopy(cache->center, center);
            if (entity && entity != &r_worldentity) {
                if (XGL_BrushBasis(entity, forward, right, up))
                    XGL_BrushPoint(center, entity, forward, right, up, worldcenter);
                else VectorAdd(center, entity->origin, worldcenter);
            } else VectorCopy(center, worldcenter);
            VectorSubtract(worldcenter, r_origin, delta);
            distance = DotProduct(delta, delta);
            detail = xgl_detaildistance.value > 0 && distance < xgl_detaildistance.value * xgl_detaildistance.value;
            if (detail && xgl_detail_used < xgl_detailtris.value)
                XGL_TryBuildDetail(model, surface, cache);
            if (detail && cache->detail.vertices && cache->detail.count > cache->coarse.count &&
                xgl_detail_used + cache->detail.count / 3 <= xgl_detailtris.value) {
                mesh = &cache->detail;
                xgl_detail_used += mesh->count / 3;
            }
        }
        if (mesh == &cache->detail) cache->detail_used = r_framecount;
        if (mesh->vertices) {
            XGL_ProfileCount(XGL_PROFILE_WORLD_CACHED_SURFACES, 1);
            XGL_UpdateSurfaceMesh(mesh, surface, texture, entity);
            XGL_EmitSurfaceVertices(mesh, entity, lit, opacity, 1);
            return;
        }
    }
    /* Allocation/budget exhaustion never discards a face. Stream its whole
     * coarse fan without persistent allocation, even for external bmodels. */
    if (surface->numedges < 3) return;
    XGL_ProfileCount(XGL_PROFILE_WORLD_FALLBACK_SURFACES, 1);
    XGL_SurfacePoint(model, surface, 0, a);
    memset(&fallback, 0, sizeof(fallback));
    fallback.vertices = triangle; fallback.data = triangle_data; fallback.count = 3;
    fallback.queued_frame = -1;
    for (i = 1; i + 1 < surface->numedges; ++i) {
        if ((i & 15) == 1) XGL_AudioUpdate();
        XGL_SurfacePoint(model, surface, i, b);
        XGL_SurfacePoint(model, surface, i + 1, c);
        XGL_InitCachedVertex(triangle, triangle_data, surface, a);
        XGL_InitCachedVertex(triangle + 1, triangle_data + 1, surface, b);
        XGL_InitCachedVertex(triangle + 2, triangle_data + 2, surface, c);
        fallback.texture = -1; fallback.fullbright = -1;
        for (axis = 0; axis < MAXLIGHTMAPS; ++axis) fallback.style[axis] = -1;
        XGL_UpdateSurfaceMesh(&fallback, surface, texture, entity);
        XGL_EmitSurfaceVertices(&fallback, entity, lit, opacity, 0);
    }
}

static void XGL_QueueSurface (model_t *model, msurface_t *surface, entity_t *entity)
{
    vec3_t point, delta, forward, right, up, transformed;
    float opacity;
    opacity = r_wateralpha.value;
    if (opacity < 0) opacity = 0;
    if (opacity > 1) opacity = 1;
    if (!(surface->flags & SURF_DRAWTURB) || opacity >= 1) {
        XGL_DrawSurface(model, surface, entity, 1);
        return;
    }
    if (opacity <= 0) return;
    if (xgl_water_count == XGL_WATER_QUEUE) {
        /* Preserve complete visibility even if an extreme map exhausts the
         * bounded sort queue; this overflow alone is drawn opaque. */
        XGL_DrawSurface(model, surface, entity, 1);
        return;
    }
    XGL_SurfacePoint(model, surface, 0, point);
    if (entity != &r_worldentity) {
        XGL_BrushBasis(entity, forward, right, up);
        XGL_BrushPoint(point, entity, forward, right, up, transformed);
    } else VectorCopy(point, transformed);
    VectorSubtract(transformed, r_origin, delta);
    xgl_water[xgl_water_count].model = model;
    xgl_water[xgl_water_count].surface = surface;
    xgl_water[xgl_water_count].entity = entity;
    xgl_water[xgl_water_count].distance = DotProduct(delta, vpn);
    ++xgl_water_count;
}

static int XGL_WaterCompare (const void *a, const void *b)
{
    float difference;
    difference = ((const xgl_water_surface_t *)b)->distance - ((const xgl_water_surface_t *)a)->distance;
    return difference > 0 ? 1 : (difference < 0 ? -1 : 0);
}

void XGL_DrawWater (void)
{
    int i;
    float opacity;
    opacity = r_wateralpha.value;
    if (opacity < 0) opacity = 0;
    if (opacity > 1) opacity = 1;
    qsort(xgl_water, xgl_water_count, sizeof(xgl_water[0]), XGL_WaterCompare);
    for (i = 0; i < xgl_water_count; ++i)
        XGL_DrawSurface(xgl_water[i].model, xgl_water[i].surface, xgl_water[i].entity, opacity);
    xgl_water_count = 0;
}

static void XGL_MarkLeaves (void)
{
    byte *vis;
    mnode_t *node;
    int i;
    /* Retain the PVS inside a leaf, but invalidate it on r_novis changes and
     * map/cache transitions. Surface visibility is still marked each frame. */
    if (xgl_pvs_world == cl.worldmodel && xgl_pvs_leaf == r_viewleaf &&
        xgl_pvs_novis == (r_novis.value != 0)) return;
    xgl_pvs_world = cl.worldmodel;
    xgl_pvs_leaf = r_viewleaf;
    xgl_pvs_novis = r_novis.value != 0;
    ++r_visframecount;
    vis = r_novis.value ? NULL : Mod_LeafPVS(r_viewleaf, cl.worldmodel);
    for (i = 0; i < cl.worldmodel->numleafs; ++i) {
        if (vis && !(vis[i >> 3] & (1 << (i & 7)))) continue;
        node = (mnode_t *)&cl.worldmodel->leafs[i + 1];
        while (node && node->visframe != r_visframecount) {
            node->visframe = r_visframecount;
            node = node->parent;
        }
    }
}

/* clipflags has one bit for each frustum plane this node may still cross.
 * A box wholly inside a plane keeps every descendant inside it too, so that
 * plane is never tested again below; the culling result is unchanged. */
static void XGL_WorldNode (mnode_t *node, int clipflags)
{
    mleaf_t *leaf;
    msurface_t *surface;
    float distance;
    int i, side;
    if (node->contents == CONTENTS_SOLID || node->visframe != r_visframecount) return;
    for (i = 0; clipflags && i < 4; ++i) {
        if (!(clipflags & (1 << i))) continue;
        side = BoxOnPlaneSide(node->minmaxs, node->minmaxs + 3, &frustum[i]);
        if (side == 2) return;
        if (side == 1) clipflags &= ~(1 << i);
    }
    if (node->contents < 0) {
        leaf = (mleaf_t *)node;
        for (i = 0; i < leaf->nummarksurfaces; ++i) leaf->firstmarksurface[i]->visframe = r_framecount;
        if (leaf->efrags) R_StoreEfrags(&leaf->efrags);
        return;
    }
    distance = DotProduct(r_origin, node->plane->normal) - node->plane->dist;
    side = distance < 0;
    XGL_WorldNode(node->children[side], clipflags);
    surface = cl.worldmodel->surfaces + node->firstsurface;
    for (i = 0; i < node->numsurfaces; ++i, ++surface) {
        if (surface->visframe != r_framecount) continue;
        if (!(surface->flags & SURF_UNDERWATER) &&
            ((distance < 0) != ((surface->flags & SURF_PLANEBACK) != 0))) continue;
        if (xgl_world_order)
            xgl_world_order[xgl_world_count++] = (int)(surface - cl.worldmodel->surfaces);
        else XGL_QueueSurface(cl.worldmodel, surface, &r_worldentity);
    }
    XGL_WorldNode(node->children[!side], clipflags);
}

/* Draws the walls. Sky and water wait for XGL_DrawWorldSpecial, so that the
 * frame can keep geometry of one native state together. */
void XGL_DrawWorld (void)
{
    msurface_t *surface;
    int i;
    double start;
    if (xgl_cache_step != xgl_subdivide.value) XGL_WorldNewMap();
    xgl_detail_used = xgl_detail_built = 0;
    xgl_water_count = 0;
    xgl_world_count = 0;
    currententity = &r_worldentity;
    start = XGL_ProfileDetailNow();
    XGL_MarkLeaves();
    XGL_WorldNode(cl.worldmodel->nodes, 15);
    /* Without the cache index, traversal also emits coarse fallback faces.
     * The fallback counter identifies that case in a detailed report. */
    XGL_ProfileAdd(XGL_PROFILE_WORLD_VISIBILITY, start);
    if (xgl_world_order) {
        /* Leaf traversal has now appended the visible static entities.
         * Prepare models while the main thread selects and submits surfaces. */
        if (XGL_SMPActive()) {
            start = XGL_ProfileDetailNow();
            XGL_AliasPrepareFrame();
            XGL_ProfileAdd(XGL_PROFILE_SMP_PREPASS, start);
        }
        start = XGL_ProfileDetailNow();
        XGL_SelectWorldDetail();
        XGL_ProfileAdd(XGL_PROFILE_WORLD_DETAIL_SELECT, start);
        for (i = 0; i < xgl_world_count; ++i) {
            surface = cl.worldmodel->surfaces + xgl_world_order[i];
            if (!(surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB)))
                XGL_QueueSurface(cl.worldmodel, surface, &r_worldentity);
        }
    }
}

/* The visible sky and water faces found by XGL_DrawWorld. They are never
 * illuminated; the caller draws them with the frame's other unlit geometry.
 * Translucent water is only queued here, for XGL_DrawWater. */
void XGL_DrawWorldSpecial (void)
{
    msurface_t *surface;
    int i;
    if (!xgl_world_order) return;
    currententity = &r_worldentity;
    for (i = 0; i < xgl_world_count; ++i) {
        surface = cl.worldmodel->surfaces + xgl_world_order[i];
        if (surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
            XGL_QueueSurface(cl.worldmodel, surface, &r_worldentity);
    }
}

void XGL_DrawBrushModel (entity_t *entity)
{
    model_t *model;
    msurface_t *surface;
    vec3_t mins, maxs, delta, local, forward, right, up;
    float distance;
    int i, rotated;
    model = entity->model;
    currententity = entity;
    rotated = entity->angles[0] || entity->angles[1] || entity->angles[2];
    for (i = 0; i < 3; ++i) {
        mins[i] = entity->origin[i] + (rotated ? -model->radius : model->mins[i]);
        maxs[i] = entity->origin[i] + (rotated ? model->radius : model->maxs[i]);
    }
    if (R_CullBox(mins, maxs)) return;
    VectorSubtract(r_origin, entity->origin, delta);
    if (XGL_BrushBasis(entity, forward, right, up)) {
        local[0] = DotProduct(delta, forward);
        local[1] = -DotProduct(delta, right);
        local[2] = DotProduct(delta, up);
    } else VectorCopy(delta, local);
    surface = model->surfaces + model->firstmodelsurface;
    for (i = 0; i < model->nummodelsurfaces; ++i, ++surface) {
        distance = DotProduct(local, surface->plane->normal) - surface->plane->dist;
        if ((surface->flags & SURF_PLANEBACK) ? distance < -.01f : distance > .01f)
            XGL_QueueSurface(model, surface, entity);
    }
}
