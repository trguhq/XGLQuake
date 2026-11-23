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
#define XGL_WATER_QUEUE 4096
#define XGL_WORLD_BATCH 384
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
/* The most triangles in one mesh. A triangulated face has at most two more
 * distinct vertices than triangles. The edge table also keeps the edges that
 * refinement has split. */
#define XGL_MESH_TRIANGLES 1024
#define XGL_MESH_VERTICES (XGL_MESH_TRIANGLES + 2)
#define XGL_MESH_EDGES 4096
#define XGL_MESH_SLOTS 8192
#define XGL_MESH_POLYGON 64
/* Detail refinement. A vertex's texture sample is filtered over this
 * fraction of the spacing of the vertices around it. A build that finds
 * nothing to add still spends some of the frame's allowance. Differences are
 * judged relative to the local light, but never to less than the floor. An
 * edge that seems to need no vertex is probed at up to this many points. */
#define XGL_DETAIL_LEVELS 8
#define XGL_DETAIL_FILTER .75f
#define XGL_DETAIL_LIGHT_FLOOR .25f
#define XGL_DETAIL_PROBES 16
#define XGL_DETAIL_FORCED 1000000.0f
#define XGL_DETAIL_ATTEMPT 32
/* A face whose apparent size earns it fewer triangles than this keeps its
 * coarse mesh. One built for some apparent size is built again, larger,
 * when the viewer has come close enough to double what it may have. */
#define XGL_DETAIL_WANT_MIN 8
/* Sky and water are cut on regular world grids; water has a finer one for
 * faces near the viewer. Each 128-texel sky layer is reduced to cells. */
#define XGL_SKY_GRID 128.0f
#define XGL_WATER_GRID 128.0f
#define XGL_WATER_FINE_GRID 64.0f
#define XGL_SKY_CELLS 16
/* Water ripples: texels per cycle of the long wave, which runs along s+t,
 * and of the short one, along s-t, and their periods in seconds. */
#define XGL_WAVE_LONG 1024.0f
#define XGL_WAVE_SHORT 256.0f
#define XGL_WAVE_LONG_PERIOD 7.0
#define XGL_WAVE_SHORT_PERIOD 3.0
#define XGL_TONE_CACHE 16

static const float xgl_turb_sin[256] = {
#include "gl_warp_sin.h"
};

typedef struct {
    float st[2];
    /* Mean length, in texels, of the mesh edges that meet at the vertex.
     * Below zero for a vertex of a piece of a wall cut along the shapes of
     * its texture: the piece is then node -spacing - 1 of the division. */
    float spacing;
    byte material[3];
    byte samples[MAXLIGHTMAPS];
} xgl_cached_vertex_t;

/* One allocation holds three native vertices per triangle, which the backend
 * borrows, then one record per distinct vertex, then each corner's distinct
 * vertex. Colors are computed once per distinct vertex and copied out.
 * Walls are nearly always drawn unlit, so their vertices carry no normals.
 * `lit` is a second copy that does, made the first time a dynamic light
 * reaches the face and dropped again when none has for a while.
 * lit_current: its colors match; lit_frame: when it was last drawn. */
typedef struct {
    xgl_color_vertex_t *vertices;
    xgl_cached_vertex_t *data;
    unsigned short *corner;
    xgl_native_vertex_t *lit;
    size_t bytes, lit_bytes;
    int count, unique, texture, style[MAXLIGHTMAPS], fullbright, queued_frame;
    int lit_current, lit_frame;
    unsigned int texture_revision, color_revision;
} xgl_surface_mesh_t;

typedef struct {
    xgl_surface_mesh_t coarse, detail;
    /* detail_retry: frame at which a build that found no memory may be tried
     * again; zero when the last failure was permanent or there was none.
     * detail_limit: the triangle limit of the last detail build if that
     * limit, rather than the face, ended it; else zero.
     * detail_want: the limit this frame's apparent size allows. */
    int coarse_attempted, detail_attempted, detail_retry, detail_limit, detail_want;
    vec3_t center;
    /* detail_frame: frame in which the world pass selected the detail mesh.
     * detail_used: last frame the detail mesh was built or drawn.
     * light_frame: frame in which a dynamic light reached this wall. */
    int center_valid, detail_frame, detail_used, bucket, light_frame;
    float area, radius;
    /* For a wall cut along the shapes of its texture. shape_step: the step
     * of detail its detailed mesh was last built for; zero while its plain
     * mesh serves. shape_need: the step this frame's distance asks for; -1
     * for a wall drawn the ordinary way. shape_most: one more than the
     * finest step that fits one mesh, once a finer has been found not to;
     * zero until then. */
    short shape_step, shape_need, shape_most;
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

/* A liquid texture's darker and lighter halves, each averaged. */
typedef struct {
    int texture;
    unsigned int revision;
    byte tone[2][3];
} xgl_tone_t;

/* Both sky layers reduced to cells: the cloud layer as color already
 * multiplied by its coverage, with that coverage. */
typedef struct {
    const texture_t *texture;
    unsigned int revision;
    int valid;
    byte front[XGL_SKY_CELLS * XGL_SKY_CELLS][4];
    byte back[XGL_SKY_CELLS * XGL_SKY_CELLS][3];
} xgl_sky_filter_t;

static xgl_surface_cache_t *xgl_surfaces;
static model_t *xgl_cached_world;
static size_t xgl_cache_bytes;
static int xgl_surface_count, xgl_detail_used, xgl_detail_built, xgl_evict_hand;
/* Frame before which a full cache is not searched again for room for an
 * illuminated copy: the search visits every face. */
static int xgl_lit_retry;
static int *xgl_world_order, *xgl_detail_order, *xgl_detail_sorted, xgl_world_count;
static model_t *xgl_pvs_world;
static mleaf_t *xgl_pvs_leaf;
static int xgl_pvs_novis;
/* The settings the cached meshes were built with. */
static float xgl_cache_step, xgl_cache_error, xgl_cache_face, xgl_cache_edge, xgl_cache_density;
static float xgl_cache_shapes;
static xgl_water_surface_t xgl_water[XGL_WATER_QUEUE];
static int xgl_water_count;
static xgl_brush_cache_t xgl_brush_cache[XGL_BRUSH_CACHE_MODELS];
static xgl_tone_t xgl_tones[XGL_TONE_CACHE];
static int xgl_tone_next;
static xgl_sky_filter_t xgl_sky;
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
/* One color for each distinct vertex of the mesh being colored. */
static float xgl_vertex_colors[XGL_MESH_VERTICES][3];
/* The scene's dynamic lights, fetched once per frame by XGL_DrawWorld.
 * xgl_light_all: reduced ambient light makes every wall illuminated.
 * xgl_light_split: some visible walls are illuminated and others are not, so
 * they are drawn in two passes. xgl_entity_lit: the brush entity in hand. */
static const xgl_point_light_t *xgl_lights;
static int xgl_light_count, xgl_light_all, xgl_light_split, xgl_entity_lit;
/* The rotation of the brush entity drawn last. Its faces arrive together. */
static struct {
    vec3_t angles, forward, right, up;
    int valid, rotated;
} xgl_basis;

cvar_t xgl_subdivide = {"xgl_subdivide", "128", false, false, 0, NULL};
cvar_t xgl_detailtris = {"xgl_detailtris", "2048", false, false, 0, NULL};
cvar_t xgl_detaildistance = {"xgl_detaildistance", "512", false, false, 0, NULL};
cvar_t xgl_detailbuild = {"xgl_detailbuild", "128", false, false, 0, NULL};
cvar_t xgl_detailerror = {"xgl_detailerror", "16", false, false, 0, NULL};
cvar_t xgl_detailface = {"xgl_detailface", "192", false, false, 0, NULL};
cvar_t xgl_detailedge = {"xgl_detailedge", "16", false, false, 0, NULL};
cvar_t xgl_detaildensity = {"xgl_detaildensity", "160", false, false, 0, NULL};
cvar_t xgl_shapes = {"xgl_shapes", "1", false, false, 0, NULL};

void XGL_SurfaceLightSamples(msurface_t *, const float *, byte[MAXLIGHTMAPS]);
void XGL_LightSamplesAt(msurface_t *, float, float, byte[MAXLIGHTMAPS]);

void XGL_SubdivideSurface (msurface_t *surface)
{
    /* The loader's sky/water hook deliberately allocates no GL polygons.
     * Ordinary and turbulent surfaces share the bounded cache below. */
    (void)surface;
}

static void XGL_FreeSurfaceMesh (xgl_surface_mesh_t *mesh)
{
    free(mesh->vertices);
    free(mesh->lit);
    xgl_cache_bytes -= mesh->bytes + mesh->lit_bytes;
    memset(mesh, 0, sizeof(*mesh));
}

/* Release a mesh's illuminated copy if no light has reached the face for
 * `age` frames. One drawn in this frame may still be borrowed by a batch. */
static void XGL_DropLitCopy (xgl_surface_mesh_t *mesh, int age)
{
    if (!mesh->lit || r_framecount - mesh->lit_frame < age) return;
    free(mesh->lit);
    xgl_cache_bytes -= mesh->lit_bytes;
    mesh->lit = NULL;
    mesh->lit_bytes = 0;
    mesh->lit_current = 0;
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

/* Release every cached mesh. What is known of the textures' shapes stays:
 * a change of settings builds the meshes again from it. */
static void XGL_WorldRelease (void)
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
    xgl_lit_retry = 0;
    xgl_pvs_world = NULL;
    xgl_pvs_leaf = NULL;
    xgl_basis.valid = 0;
    xgl_light_count = xgl_light_all = xgl_light_split = xgl_entity_lit = 0;
    /* Textures belong to the map: a new one may reuse an address or an id. */
    memset(&xgl_sky, 0, sizeof(xgl_sky));
    memset(xgl_tones, 0, sizeof(xgl_tones));
    xgl_tone_next = 0;
}

void XGL_WorldShutdown (void)
{
    XGL_WorldRelease();
    XGL_ShapesClear();
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
    XGL_DropLitCopy(&cache->coarse, age);
    XGL_DropLitCopy(&cache->detail, age);
    if (!cache->detail.vertices || r_framecount - cache->detail_used < age) return;
    XGL_FreeSurfaceMesh(&cache->detail);
    cache->detail_attempted = cache->detail_retry = cache->detail_limit = 0;
    cache->shape_step = 0;
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

static void XGL_WorldGamma (void)
{
    unsigned int revision;
    revision = XGL_ColorRevision();
    if (xgl_gamma_revision == revision) return;
    XGL_CopyGammaOutput(xgl_gamma);
    xgl_gamma_revision = revision;
}

/* Mesh building. One face at a time is assembled in this scratch space and
 * then copied into a single cache allocation. */
typedef struct {
    vec3_t position;
    float st[2];
    /* spacing: the texels the texture sample is filtered over. color: what
     * the vertex would show, 0..255: material times baked light. light:
     * that baked light alone, 0..1. */
    float spacing, color[3], light;
    byte samples[MAXLIGHTMAPS];
} xgl_build_vertex_t;

typedef struct {
    unsigned int key;
    unsigned short a, b;
    /* uses: triangles of the current level that have this edge. vertex: its
     * midpoint once it has been split. tested: its middle was evaluated; if
     * splittable, mid then holds the sample a midpoint vertex would have.
     * probed: it passed that and was walked along its length as well. */
    short uses, vertex;
    byte tested, splittable, probed;
    float length, error;
    xgl_build_vertex_t mid;
} xgl_build_edge_t;

static struct {
    xgl_build_vertex_t vertices[XGL_MESH_VERTICES];
    unsigned short triangles[2][XGL_MESH_TRIANGLES][3];
    /* tested: the triangle's interior was checked and it has not split since. */
    byte tested[2][XGL_MESH_TRIANGLES];
    unsigned short triangle_edges[XGL_MESH_TRIANGLES][3];
    xgl_build_edge_t edges[XGL_MESH_EDGES];
    unsigned short slots[XGL_MESH_SLOTS];
    unsigned short order[XGL_MESH_EDGES];
    int vertex_count, triangle_count, edge_count, current;
    /* capped: the triangle limit, not the face, ended the refinement. */
    int capped;
    /* Edges no longer than this, in texels, are never split. */
    float minimum;
    /* After a mesh cut along shapes was asked for: the step that fit one
     * mesh when the step asked for did not, zero if none did; -1 when the
     * step asked for was not too much. */
    int shape_fit;
} xgl_build;

/* Baked light with every style at its nominal level, as a factor that takes
 * a 0..1 material to 0..255. Mesh shape must not follow a flickering style. */
static float XGL_StaticLight (const msurface_t *surface, const byte *samples)
{
    unsigned int level;
    int layer, value;
    if (!cl.worldmodel->lightdata ||
        (surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWTILED))) return 255;
    value = 0;
    for (layer = 0; layer < MAXLIGHTMAPS && surface->styles[layer] != 255; ++layer)
        value += samples[layer] * 256;
    level = 5U * (unsigned int)value;
    if (level < XGL_LIGHT_FLOOR) level = XGL_LIGHT_FLOOR;
    if (level > XGL_LIGHT_FULL) level = XGL_LIGHT_FULL;
    return (float)level * (255.0f / XGL_LIGHT_FULL);
}

static void XGL_BuildCoordinates (const msurface_t *surface, xgl_build_vertex_t *vertex)
{
    int i;
    for (i = 0; i < 2; ++i)
        vertex->st[i] = DotProduct(vertex->position, surface->texinfo->vecs[i]) +
            surface->texinfo->vecs[i][3];
}

/* What a vertex with these texture coordinates would show if its texture
 * sample were filtered for vertices `spacing` texels apart. Refinement takes
 * several of these for every triangle it produces, so both lookups work
 * from the coordinates alone. */
static void XGL_BuildSample (msurface_t *surface, texture_t *texture,
                             xgl_build_vertex_t *vertex, float spacing)
{
    byte rgb[3];
    float light;
    int i;
    vertex->spacing = spacing;
    XGL_LightSamplesAt(surface, vertex->st[0], vertex->st[1], vertex->samples);
    rgb[0] = rgb[1] = rgb[2] = 128;
    if (texture)
        XGL_SampleTextureBox(texture->gl_texturenum, vertex->st[0], vertex->st[1],
                             spacing * XGL_DETAIL_FILTER, rgb);
    light = XGL_StaticLight(surface, vertex->samples) * (1.0f / 255);
    vertex->light = light;
    for (i = 0; i < 3; ++i) vertex->color[i] = rgb[i] * light;
}

/* Give every vertex the mean texel length of the edges that meet at it. */
static void XGL_BuildSpacing (void)
{
    static float sum[XGL_MESH_VERTICES];
    static unsigned short edges[XGL_MESH_VERTICES];
    const unsigned short *triangle;
    const xgl_build_vertex_t *a, *b;
    float ds, dt, length;
    int i, j;
    memset(sum, 0, (size_t)xgl_build.vertex_count * sizeof(sum[0]));
    memset(edges, 0, (size_t)xgl_build.vertex_count * sizeof(edges[0]));
    for (i = 0; i < xgl_build.triangle_count; ++i) {
        triangle = xgl_build.triangles[xgl_build.current][i];
        for (j = 0; j < 3; ++j) {
            a = &xgl_build.vertices[triangle[j]];
            b = &xgl_build.vertices[triangle[(j + 1) % 3]];
            ds = a->st[0] - b->st[0]; dt = a->st[1] - b->st[1];
            length = (float)sqrt(ds * ds + dt * dt);
            sum[triangle[j]] += length; ++edges[triangle[j]];
            sum[triangle[(j + 1) % 3]] += length; ++edges[triangle[(j + 1) % 3]];
        }
    }
    for (i = 0; i < xgl_build.vertex_count; ++i)
        xgl_build.vertices[i].spacing = edges[i] ? sum[i] / edges[i] : 0;
}

/* Triangulate the convex polygon whose scratch vertices are listed in index,
 * working inward from both ends in turn. That keeps the triangles better
 * proportioned than a fan from one corner. Returns 0 when out of room. */
static int XGL_BuildPolygon (const int *index, int count)
{
    unsigned short *triangle;
    int low, high, turn;
    low = 0; high = count - 1; turn = 0;
    while (high - low >= 2) {
        if (xgl_build.triangle_count == XGL_MESH_TRIANGLES) return 0;
        triangle = xgl_build.triangles[0][xgl_build.triangle_count];
        xgl_build.tested[0][xgl_build.triangle_count++] = 0;
        triangle[0] = (unsigned short)index[low];
        if (turn) {
            triangle[1] = (unsigned short)index[high - 1];
            triangle[2] = (unsigned short)index[high];
            --high;
        } else {
            triangle[1] = (unsigned short)index[low + 1];
            triangle[2] = (unsigned short)index[high];
            ++low;
        }
        turn ^= 1;
    }
    return 1;
}

/* A wall's own polygon. With colors, each corner is also sampled as the
 * refinement tests need; the coarse mesh is sampled later, once. */
static int XGL_BuildBase (model_t *model, msurface_t *surface, texture_t *texture, int colors)
{
    xgl_build_vertex_t *vertex;
    int index[XGL_MESH_POLYGON], i, count;
    count = surface->numedges;
    if (count < 3 || count > XGL_MESH_POLYGON) return 0;
    xgl_build.current = 0;
    xgl_build.vertex_count = count;
    xgl_build.triangle_count = xgl_build.edge_count = 0;
    for (i = 0; i < count; ++i) {
        vertex = &xgl_build.vertices[i];
        XGL_SurfacePoint(model, surface, i, vertex->position);
        XGL_BuildCoordinates(surface, vertex);
        index[i] = i;
    }
    XGL_BuildPolygon(index, count);
    XGL_BuildSpacing();
    for (i = 0; i < count; ++i) {
        vertex = &xgl_build.vertices[i];
        if (colors) XGL_BuildSample(surface, texture, vertex, vertex->spacing);
        else XGL_LightSamplesAt(surface, vertex->st[0], vertex->st[1], vertex->samples);
    }
    return 1;
}

/* The table index of the edge between two scratch vertices, added if new;
 * -1 when the table is full. */
static int XGL_BuildEdge (int a, int b)
{
    xgl_build_edge_t *edge;
    unsigned int key, slot;
    int swap;
    if (a > b) { swap = a; a = b; b = swap; }
    key = ((unsigned int)a << 16) | (unsigned int)b;
    slot = ((key * 2654435761U) & 0xffffffffU) >> 19;
    while (xgl_build.slots[slot]) {
        if (xgl_build.edges[xgl_build.slots[slot] - 1].key == key)
            return xgl_build.slots[slot] - 1;
        slot = (slot + 1) & (XGL_MESH_SLOTS - 1);
    }
    if (xgl_build.edge_count == XGL_MESH_EDGES) return -1;
    edge = &xgl_build.edges[xgl_build.edge_count];
    edge->key = key;
    edge->a = (unsigned short)a; edge->b = (unsigned short)b;
    edge->uses = 0; edge->vertex = -1;
    edge->tested = edge->splittable = edge->probed = 0;
    edge->length = edge->error = 0;
    xgl_build.slots[slot] = (unsigned short)++xgl_build.edge_count;
    return xgl_build.edge_count - 1;
}

/* How far a sample is from the color the mesh would interpolate at the same
 * point: the largest channel difference, relative to the brighter of the two
 * light levels. The eye judges contrast against its surroundings, so a
 * texture in shadow has to count as much as the same texture in full light;
 * measured in final color alone, dim walls would rarely earn a triangle. */
static float XGL_BuildError (const xgl_build_vertex_t *sample, const float *color, float light)
{
    float error, difference;
    int i;
    error = 0;
    for (i = 0; i < 3; ++i) {
        difference = sample->color[i] - color[i];
        if (difference < 0) difference = -difference;
        if (difference > error) error = difference;
    }
    if (sample->light > light) light = sample->light;
    if (light < XGL_DETAIL_LIGHT_FLOOR) light = XGL_DETAIL_LIGHT_FLOOR;
    return error / light;
}

/* The same, against the point `weight` of the way from a to b. */
static float XGL_BuildEdgeError (const xgl_build_vertex_t *sample, const xgl_build_vertex_t *a,
                                 const xgl_build_vertex_t *b, float weight)
{
    float color[3];
    int i;
    for (i = 0; i < 3; ++i) color[i] = a->color[i] + (b->color[i] - a->color[i]) * weight;
    return XGL_BuildError(sample, color, a->light + (b->light - a->light) * weight);
}

/* Decide whether a vertex at the middle of an edge would show something
 * that interpolating between its ends does not. Edges longer than `step`
 * texels are always split. */
static void XGL_BuildTestEdge (msurface_t *surface, texture_t *texture,
                               xgl_build_edge_t *edge, float step, float threshold)
{
    const xgl_build_vertex_t *a, *b;
    float ds, dt, error;
    int i;
    a = &xgl_build.vertices[edge->a]; b = &xgl_build.vertices[edge->b];
    ds = a->st[0] - b->st[0]; dt = a->st[1] - b->st[1];
    edge->length = (float)sqrt(ds * ds + dt * dt);
    edge->tested = 1;
    edge->error = 0;
    edge->splittable = edge->length > xgl_build.minimum;
    if (!edge->splittable) return;
    for (i = 0; i < 3; ++i)
        edge->mid.position[i] = (a->position[i] + b->position[i]) * .5f;
    edge->mid.st[0] = (a->st[0] + b->st[0]) * .5f;
    edge->mid.st[1] = (a->st[1] + b->st[1]) * .5f;
    XGL_BuildSample(surface, texture, &edge->mid, edge->length * .5f);
    if (edge->length > step) {
        edge->error = XGL_DETAIL_FORCED + edge->length;
        return;
    }
    error = XGL_BuildEdgeError(&edge->mid, a, b, .5f);
    if (error > threshold) edge->error = error;
}

/* The middle of an edge can agree by chance while something lies to either
 * side of it. Before a long edge is let pass, it is walked at the finest
 * spacing a mesh may reach; whatever that finds, further splits close in
 * on. Otherwise the same texture is caught on one wall and missed on the
 * next, according to where their corners happen to fall. */
static void XGL_BuildProbeEdge (msurface_t *surface, texture_t *texture,
                                xgl_build_edge_t *edge, float threshold)
{
    const xgl_build_vertex_t *a, *b;
    xgl_build_vertex_t probe;
    float error, weight;
    int k, steps;
    edge->probed = 1;
    memset(&probe, 0, sizeof(probe));
    a = &xgl_build.vertices[edge->a]; b = &xgl_build.vertices[edge->b];
    steps = (int)(edge->length / xgl_build.minimum);
    if (steps > XGL_DETAIL_PROBES) steps = XGL_DETAIL_PROBES;
    for (k = 1; k < steps; ++k) {
        if (k * 2 == steps) continue; /* The middle has been looked at. */
        weight = (float)k / steps;
        probe.st[0] = a->st[0] + (b->st[0] - a->st[0]) * weight;
        probe.st[1] = a->st[1] + (b->st[1] - a->st[1]) * weight;
        XGL_BuildSample(surface, texture, &probe, edge->length / steps);
        error = XGL_BuildEdgeError(&probe, a, b, weight);
        if (error > threshold) {
            edge->error = error;
            return;
        }
    }
}

static int XGL_BuildEdgeCompare (const void *left, const void *right)
{
    const xgl_build_edge_t *a, *b;
    a = &xgl_build.edges[*(const unsigned short *)left];
    b = &xgl_build.edges[*(const unsigned short *)right];
    if (a->error != b->error) return a->error > b->error ? -1 : 1;
    return (int)*(const unsigned short *)left - (int)*(const unsigned short *)right;
}

static void XGL_BuildTriangle (unsigned short (*list)[3], byte *tested, int *count,
                               int a, int b, int c, int checked)
{
    list[*count][0] = (unsigned short)a;
    list[*count][1] = (unsigned short)b;
    list[*count][2] = (unsigned short)c;
    tested[(*count)++] = (byte)checked;
}

/* One level of refinement. Mark the edges whose midpoint would show
 * something the mesh does not; split as many as the triangle limit allows,
 * largest error first; rebuild the list. Every triangle that has a
 * split edge uses the same new vertex, so no seam can open inside the face.
 * Edges and list that passed are not examined again at later levels.
 * Returns the number of edges split. */
static int XGL_BuildRefine (msurface_t *surface, texture_t *texture,
                            float step, float threshold, int limit)
{
    xgl_build_edge_t *edge, *sides[3];
    xgl_build_vertex_t centroid;
    const xgl_build_vertex_t *corner[3];
    const float *pa, *pc, *pm, *pn;
    unsigned short (*list)[3], (*next)[3];
    byte *tested, *next_tested;
    vec3_t first, second;
    float error, worst, difference, share[3], expected[3];
    int i, j, k, index, count, marked, applied, total, longest, a, b, c, ab, bc, ca, swap, n;
    int probes, room, wanted;
    list = xgl_build.triangles[xgl_build.current];
    tested = xgl_build.tested[xgl_build.current];
    count = xgl_build.triangle_count;
    /* A full mesh needs no further looking at. */
    room = limit - count;
    if (room <= 0) { xgl_build.capped = 1; return 0; }
    for (i = 0; i < xgl_build.edge_count; ++i) xgl_build.edges[i].uses = 0;
    for (i = 0; i < count; ++i)
        for (j = 0; j < 3; ++j) {
            index = XGL_BuildEdge(list[i][j], list[i][(j + 1) % 3]);
            if (index < 0) return 0;
            xgl_build.triangle_edges[i][j] = (unsigned short)index;
            ++xgl_build.edges[index].uses;
        }
    /* wanted: the triangles the edges marked so far would add. Once that
     * exceeds the room left, the costlier second looks cannot add a split
     * that would be made, and are skipped. */
    wanted = 0;
    for (i = 0; i < xgl_build.edge_count; ++i) {
        edge = &xgl_build.edges[i];
        if ((i & 15) == 0) XGL_AudioUpdate();
        if (!edge->uses) continue;
        if (!edge->tested) XGL_BuildTestEdge(surface, texture, edge, step, threshold);
        if (edge->error > 0 && edge->vertex < 0) wanted += edge->uses;
    }
    for (i = 0; i < xgl_build.edge_count && wanted <= room; ++i) {
        edge = &xgl_build.edges[i];
        if (!edge->uses || edge->probed || edge->error > 0 || !edge->splittable) continue;
        if ((i & 7) == 0) XGL_AudioUpdate();
        XGL_BuildProbeEdge(surface, texture, edge, threshold);
        if (edge->error > 0) wanted += edge->uses;
    }
    for (i = 0; i < count; ++i) {
        worst = 0; longest = 0;
        for (j = 0; j < 3; ++j) {
            sides[j] = &xgl_build.edges[xgl_build.triangle_edges[i][j]];
            if (sides[j]->error > worst) worst = sides[j]->error;
            if (sides[j]->length > sides[longest]->length) longest = j;
        }
        /* Something inside a large triangle may touch none of the points
         * its edges were tested at. Look at its middle and, if it is large
         * enough to be split twice, at the middles of the corner triangles
         * a split would make. */
        if (worst <= 0 && !tested[i] && wanted <= room &&
            sides[longest]->length > 2 * xgl_build.minimum) {
            if ((i & 15) == 0) XGL_AudioUpdate();
            for (j = 0; j < 3; ++j) corner[j] = &xgl_build.vertices[list[i][j]];
            error = 0;
            probes = sides[longest]->length > 4 * xgl_build.minimum ? 4 : 1;
            for (k = 0; k < probes && error <= threshold; ++k) {
                for (j = 0; j < 3; ++j)
                    share[j] = !k ? 1.0f / 3 : (j == k - 1 ? 2.0f / 3 : 1.0f / 6);
                for (j = 0; j < 2; ++j)
                    centroid.st[j] = corner[0]->st[j] * share[0] + corner[1]->st[j] * share[1] +
                                     corner[2]->st[j] * share[2];
                XGL_BuildSample(surface, texture, &centroid,
                                sides[longest]->length * (k ? .25f : 1.0f / 3));
                for (j = 0; j < 3; ++j)
                    expected[j] = corner[0]->color[j] * share[0] + corner[1]->color[j] * share[1] +
                                  corner[2]->color[j] * share[2];
                difference = XGL_BuildError(&centroid, expected,
                    corner[0]->light * share[0] + corner[1]->light * share[1] +
                    corner[2]->light * share[2]);
                if (difference > error) error = difference;
            }
            if (error > threshold) {
                worst = error;
                for (j = 0; j < 3; ++j)
                    if (sides[j]->splittable && sides[j]->error < error) {
                        if (sides[j]->error <= 0) wanted += sides[j]->uses;
                        sides[j]->error = error;
                    }
            }
            tested[i] = 1;
        }
        /* Splitting only its shorter edges would leave a sliver. */
        if (worst > 0 && sides[longest]->error <= 0 && sides[longest]->splittable)
            sides[longest]->error = worst;
    }
    marked = 0;
    for (i = 0; i < xgl_build.edge_count; ++i) {
        edge = &xgl_build.edges[i];
        if (edge->uses && edge->error > 0 && edge->vertex < 0)
            xgl_build.order[marked++] = (unsigned short)i;
    }
    if (!marked) return 0;
    qsort(xgl_build.order, (size_t)marked, sizeof(xgl_build.order[0]), XGL_BuildEdgeCompare);
    applied = 0; total = count;
    for (i = 0; i < marked; ++i) {
        edge = &xgl_build.edges[xgl_build.order[i]];
        if (total + edge->uses > limit || xgl_build.vertex_count == XGL_MESH_VERTICES) {
            xgl_build.capped = 1;
            continue;
        }
        xgl_build.vertices[xgl_build.vertex_count] = edge->mid;
        edge->vertex = (short)xgl_build.vertex_count++;
        total += edge->uses; ++applied;
    }
    if (!applied) return 0;
    next = xgl_build.triangles[!xgl_build.current];
    next_tested = xgl_build.tested[!xgl_build.current];
    n = 0;
    for (i = 0; i < count; ++i) {
        a = list[i][0]; b = list[i][1]; c = list[i][2];
        ab = xgl_build.edges[xgl_build.triangle_edges[i][0]].vertex;
        bc = xgl_build.edges[xgl_build.triangle_edges[i][1]].vertex;
        ca = xgl_build.edges[xgl_build.triangle_edges[i][2]].vertex;
        switch ((ab >= 0) + (bc >= 0) + (ca >= 0)) {
        case 0:
            XGL_BuildTriangle(next, next_tested, &n, a, b, c, 1);
            break;
        case 1:
            /* Turn the triangle so that a-b is the split edge. */
            if (bc >= 0) { swap = a; a = b; b = c; c = swap; ab = bc; }
            else if (ca >= 0) { swap = c; c = b; b = a; a = swap; ab = ca; }
            XGL_BuildTriangle(next, next_tested, &n, a, ab, c, 0);
            XGL_BuildTriangle(next, next_tested, &n, ab, b, c, 0);
            break;
        case 2:
            /* Turn it so that c-a is the whole edge. */
            if (ab < 0) { swap = a; a = b; b = c; c = swap; ab = bc; bc = ca; }
            else if (bc < 0) { swap = c; c = b; b = a; a = swap; swap = ab; ab = ca; bc = swap; }
            XGL_BuildTriangle(next, next_tested, &n, ab, b, bc, 0);
            /* The remaining quadrilateral is cut along its shorter diagonal. */
            pa = xgl_build.vertices[a].position; pc = xgl_build.vertices[c].position;
            pm = xgl_build.vertices[ab].position; pn = xgl_build.vertices[bc].position;
            VectorSubtract(pa, pn, first); VectorSubtract(pm, pc, second);
            if (DotProduct(first, first) <= DotProduct(second, second)) {
                XGL_BuildTriangle(next, next_tested, &n, a, ab, bc, 0);
                XGL_BuildTriangle(next, next_tested, &n, a, bc, c, 0);
            } else {
                XGL_BuildTriangle(next, next_tested, &n, a, ab, c, 0);
                XGL_BuildTriangle(next, next_tested, &n, ab, bc, c, 0);
            }
            break;
        default:
            XGL_BuildTriangle(next, next_tested, &n, a, ab, ca, 0);
            XGL_BuildTriangle(next, next_tested, &n, ab, b, bc, 0);
            XGL_BuildTriangle(next, next_tested, &n, ca, bc, c, 0);
            XGL_BuildTriangle(next, next_tested, &n, ab, bc, ca, 0);
            break;
        }
    }
    xgl_build.current ^= 1;
    xgl_build.triangle_count = n;
    return applied;
}

/* The scratch vertex at a position, added if new; -1 when out of room.
 * Equal positions are found whatever the sign of a zero. */
static int XGL_GridVertex (const float *position)
{
    unsigned int slot;
    int index;
    slot = ((unsigned int)(int)(position[0] * 4) * 73856093U ^
            (unsigned int)(int)(position[1] * 4) * 19349663U ^
            (unsigned int)(int)(position[2] * 4) * 83492791U) & (XGL_MESH_SLOTS - 1);
    while (xgl_build.slots[slot]) {
        index = xgl_build.slots[slot] - 1;
        if (xgl_build.vertices[index].position[0] == position[0] &&
            xgl_build.vertices[index].position[1] == position[1] &&
            xgl_build.vertices[index].position[2] == position[2]) return index;
        slot = (slot + 1) & (XGL_MESH_SLOTS - 1);
    }
    if (xgl_build.vertex_count == XGL_MESH_VERTICES) return -1;
    VectorCopy(position, xgl_build.vertices[xgl_build.vertex_count].position);
    xgl_build.slots[slot] = (unsigned short)++xgl_build.vertex_count;
    return xgl_build.vertex_count - 1;
}

static int XGL_GridEmit (vec3_t *points, int count)
{
    int index[XGL_MESH_POLYGON], i, n, vertex;
    n = 0;
    for (i = 0; i < count; ++i) {
        vertex = XGL_GridVertex(points[i]);
        if (vertex < 0) return 0;
        if (n && index[n - 1] == vertex) continue;
        index[n++] = vertex;
    }
    while (n > 1 && index[n - 1] == index[0]) --n;
    return XGL_BuildPolygon(index, n);
}

/* Cut a polygon along world-axis planes at multiples of the grid, as
 * GL_SubdivideSurface cuts sky and water, until no piece spans a plane by
 * more than eight units. Returns 0 when the pieces do not fit the scratch. */
static int XGL_GridSplit (vec3_t *points, int count, float grid)
{
    vec3_t front[XGL_MESH_POLYGON], back[XGL_MESH_POLYGON];
    const float *p, *q, *a, *c;
    float low, high, middle, dp, dq, fraction;
    int axis, i, j, f, b;
    if (count < 3) return 1;
    for (axis = 0; axis < 3; ++axis) {
        low = high = points[0][axis];
        for (i = 1; i < count; ++i) {
            if (points[i][axis] < low) low = points[i][axis];
            if (points[i][axis] > high) high = points[i][axis];
        }
        middle = grid * (float)floor((low + high) * .5f / grid + .5f);
        if (high - middle < 8 || middle - low < 8) continue;
        f = b = 0;
        for (i = 0; i < count; ++i) {
            if (f > XGL_MESH_POLYGON - 2 || b > XGL_MESH_POLYGON - 2) return 0;
            p = points[i]; q = points[(i + 1) % count];
            dp = p[axis] - middle; dq = q[axis] - middle;
            if (dp >= 0) { VectorCopy(p, front[f]); ++f; }
            if (dp <= 0) { VectorCopy(p, back[b]); ++b; }
            if (dp == 0 || dq == 0 || (dp > 0) == (dq > 0)) continue;
            /* Faces that share this edge cut it from the same end, so both
             * arrive at exactly the same point. */
            a = p; c = q;
            for (j = 0; j < 3; ++j) {
                if (p[j] == q[j]) continue;
                if (p[j] > q[j]) { a = q; c = p; }
                break;
            }
            fraction = (middle - a[axis]) / (c[axis] - a[axis]);
            for (j = 0; j < 3; ++j) front[f][j] = a[j] + fraction * (c[j] - a[j]);
            front[f][axis] = middle;
            VectorCopy(front[f], back[b]);
            ++f; ++b;
        }
        return XGL_GridSplit(front, f, grid) && XGL_GridSplit(back, b, grid);
    }
    return XGL_GridEmit(points, count);
}

/* Sky and water: the face cut on a regular grid. Neighboring faces then
 * share every vertex along their common edges. */
static int XGL_BuildGrid (model_t *model, msurface_t *surface, float grid)
{
    vec3_t points[XGL_MESH_POLYGON];
    int i, count;
    count = surface->numedges;
    if (count < 3 || count > XGL_MESH_POLYGON - 2) return 0;
    for (i = 0; i < count; ++i) XGL_SurfacePoint(model, surface, i, points[i]);
    xgl_build.current = 0;
    xgl_build.vertex_count = xgl_build.triangle_count = xgl_build.edge_count = 0;
    memset(xgl_build.slots, 0, sizeof(xgl_build.slots));
    if (!XGL_GridSplit(points, count, grid) || !xgl_build.triangle_count) return 0;
    for (i = 0; i < xgl_build.vertex_count; ++i) {
        XGL_BuildCoordinates(surface, &xgl_build.vertices[i]);
        xgl_build.vertices[i].spacing = 0;
        memset(xgl_build.vertices[i].samples, 0, MAXLIGHTMAPS);
    }
    return 1;
}

/* Copy the scratch mesh into one cache allocation. Returns 1, or -1 when
 * the cache has no room even after evicting idle detail. */
static int XGL_BuildFinish (msurface_t *surface, xgl_surface_mesh_t *mesh)
{
    const xgl_build_vertex_t *source;
    xgl_color_vertex_t *draw;
    xgl_cached_vertex_t *data;
    const unsigned short *triangle;
    size_t bytes;
    int i, j, count, unique;
    (void)surface;
    count = xgl_build.triangle_count * 3; unique = xgl_build.vertex_count;
    bytes = (size_t)count * (sizeof(*mesh->vertices) + sizeof(*mesh->corner)) +
            (size_t)unique * sizeof(*mesh->data);
    bytes = (bytes + 7) & ~(size_t)7;
    if (!XGL_CacheFits(bytes)) {
        XGL_EvictDetail(bytes);
        if (!XGL_CacheFits(bytes)) return -1;
    }
    mesh->vertices = (xgl_color_vertex_t *)malloc(bytes);
    if (!mesh->vertices) return -1;
    memset(mesh->vertices, 0, bytes);
    xgl_cache_bytes += bytes;
    mesh->bytes = bytes;
    mesh->data = (xgl_cached_vertex_t *)(mesh->vertices + count);
    mesh->corner = (unsigned short *)(mesh->data + unique);
    mesh->count = count; mesh->unique = unique;
    for (i = 0, source = xgl_build.vertices, data = mesh->data; i < unique; ++i, ++source, ++data) {
        data->st[0] = source->st[0]; data->st[1] = source->st[1];
        data->spacing = source->spacing;
        memcpy(data->samples, source->samples, MAXLIGHTMAPS);
    }
    draw = mesh->vertices;
    for (i = 0; i < xgl_build.triangle_count; ++i) {
        triangle = xgl_build.triangles[xgl_build.current][i];
        for (j = 0; j < 3; ++j, ++draw) {
            mesh->corner[i * 3 + j] = triangle[j];
            VectorCopy(xgl_build.vertices[triangle[j]].position, draw->position);
        }
    }
    mesh->texture = -1;
    mesh->fullbright = -1;
    mesh->queued_frame = -1;
    mesh->texture_revision = mesh->color_revision = 0;
    for (i = 0; i < MAXLIGHTMAPS; ++i) mesh->style[i] = -1;
    return 1;
}

static float XGL_Setting (float value, float low, float high)
{
    if (!(value >= low)) return low;
    return value > high ? high : value;
}

/* ---- Walls cut along the shapes in their textures -----------------------
 * xgl_shapes.c divides a texture into pieces of nearly one color and cuts a
 * face along that division. A wall of such a texture is drawn as those
 * pieces, each in its color under the baked light at its own corners.
 *
 * How finely depends only on the texture and on a step of detail that
 * follows the distance, never on the face: every face of a wall is then cut
 * alike and the pattern runs unbroken across them. */

/* Pieces to a repeat of the texture: the triangles the frame's density
 * allows that much wall, over this. Set so that walls cut this way cost
 * about what they cost as smooth meshes. A cut is judged worth making as if
 * a piece cost somewhat more. */
#define XGL_SHAPE_PER_PIECE 2.2f
#define XGL_SHAPE_PIECE_COST 3.3f
/* A step serves a range of distances, and is judged as from the middle of
 * it: the squared distance at which it begins, over this. */
#define XGL_SHAPE_STEP_MIDDLE 1.2f
/* At this distance a texel is about a pixel wide. Nearer, a cut need be
 * worth less, down to this share; and no part is cut off that would be
 * narrower than this many pixels, which would only flicker. */
#define XGL_SHAPE_PIXEL_DISTANCE 160.0f
#define XGL_SHAPE_NEAR .25f
#define XGL_SHAPE_THIN 1.5f
/* Walls cut along shapes may run this far over the frame's allowance before
 * one of their faces is left plain, and rank among the other walls as if
 * each face were this large. */
#define XGL_SHAPE_OVER 1.25f
#define XGL_SHAPE_RANK_AREA 16384.0f
/* Frames between two meshes made coarser because the viewer moved away. */
#define XGL_SHAPE_COARSEN 4

/* The pieces a repeat of the texture may have at each step: steps, so that
 * faces a little nearer or farther agree. Past the most pieces a division
 * has, the steps only ask less of a cut. */
static const short xgl_shape_levels[XGL_SHAPE_STEPS] = {
    1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64,
    96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096
};
/* Changes whenever the settings the steps depend on may have. */
static int xgl_shape_key = 1;
static int xgl_coarsen_frame = -1;

/* The division of a wall's texture, or NULL for a wall drawn the ordinary
 * way. An animated texture has one division for all its frames. */
static xgl_shape_tree_t *XGL_SurfaceShapes (const msurface_t *surface)
{
    if (!xgl_shapes.value || (surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB))) return NULL;
    return XGL_TextureShapes(surface->texinfo->texture);
}

/* What a step of detail asks of a cut. */
static void XGL_ShapeTerms (const xgl_shape_tree_t *tree, int step, xgl_shape_terms_t *terms)
{
    float threshold, edge, distance, nearness;
    int level;
    level = xgl_shape_levels[step];
    terms->most = 2 * ((level < XGL_SHAPE_PIECES ? level : XGL_SHAPE_PIECES) - 1);
    /* The squared distance the step is judged from. With no density every
     * wall in range is as if at the viewer's feet. */
    distance = 0;
    if (xgl_detaildensity.value > 0)
        distance = (float)tree->width * tree->height * xgl_detaildensity.value /
                   (XGL_SHAPE_PER_PIECE * XGL_SHAPE_STEP_MIDDLE * level) - 64;
    if (distance < 0) distance = 0;
    /* A cut must be worth what the ordinary builder asks of a split: a step
     * of xgl_detailerror levels in every channel across a piece
     * xgl_detailedge texels square, for each triangle it adds. What it
     * removes counts by the screen it covers, so near the viewer a small
     * feature is worth more; farther off, the count of pieces already holds
     * the cuts back. */
    threshold = XGL_Setting(xgl_detailerror.value, 1, 255);
    edge = XGL_Setting(xgl_detailedge.value, 4, 64);
    nearness = (distance + 64) / (XGL_SHAPE_PIXEL_DISTANCE * XGL_SHAPE_PIXEL_DISTANCE);
    if (nearness < XGL_SHAPE_NEAR) nearness = XGL_SHAPE_NEAR;
    if (nearness > 1) nearness = 1;
    terms->worth = .375f * edge * edge * threshold * threshold * XGL_SHAPE_PIECE_COST * nearness;
    terms->thin = XGL_SHAPE_THIN * (float)sqrt(distance) / XGL_SHAPE_PIXEL_DISTANCE;
}

/* For each step, the lowest step that makes the same cuts: a later step
 * makes every cut an earlier one does, so two that make as many make the
 * same. A wall is then not built again for a step that would not change it,
 * and step zero stands for every step that leaves the plain mesh. */
static const byte *XGL_ShapeSteps (xgl_shape_tree_t *tree)
{
    xgl_shape_terms_t terms;
    int i, cuts, previous;
    if (tree->steps_key == xgl_shape_key) return tree->steps;
    tree->steps[0] = 0;
    previous = 0;
    for (i = 1; i < XGL_SHAPE_STEPS; ++i) {
        XGL_ShapeTerms(tree, i, &terms);
        cuts = XGL_ShapeCuts(tree, &terms);
        tree->steps[i] = cuts == previous ? tree->steps[i - 1] : (byte)i;
        previous = cuts;
    }
    tree->steps_key = xgl_shape_key;
    return tree->steps;
}

/* The step of detail a wall of this texture has at a squared distance: the
 * pieces a repeat gets are what the frame's density of triangles pays for.
 * Zero when its plain mesh shows all it can be given there. */
static int XGL_ShapeStep (xgl_shape_tree_t *tree, float distance_squared)
{
    float pieces;
    int step;
    step = XGL_SHAPE_STEPS - 1;
    if (xgl_detaildensity.value > 0) {
        pieces = (float)tree->width * tree->height * xgl_detaildensity.value /
                 ((distance_squared + 64) * XGL_SHAPE_PER_PIECE);
        for (step = 0; step + 1 < XGL_SHAPE_STEPS && xgl_shape_levels[step + 1] <= pieces; ++step) {}
    }
    return XGL_ShapeSteps(tree)[step];
}

/* That step for a wall; -1 for one not cut along shapes. */
static int XGL_SurfaceShapeStep (const msurface_t *surface, float distance_squared)
{
    xgl_shape_tree_t *tree = XGL_SurfaceShapes(surface);
    return tree ? XGL_ShapeStep(tree, distance_squared) : -1;
}

/* A scratch vertex of a piece. Its recorded coordinates are the piece's
 * middle, and in place of a spacing it records which node of the division
 * the piece is: the piece takes its color from that. */
static int XGL_ShapeVertex (msurface_t *surface, const float *where, const float *anchor,
                            const float *map, const xgl_shape_piece_t *piece)
{
    xgl_build_vertex_t *vertex;
    int j;
    if (xgl_build.vertex_count == XGL_MESH_VERTICES) return -1;
    vertex = &xgl_build.vertices[xgl_build.vertex_count];
    for (j = 0; j < 3; ++j)
        vertex->position[j] = anchor[j] + (where[0] - anchor[3]) * map[j] +
                              (where[1] - anchor[4]) * map[3 + j];
    XGL_LightSamplesAt(surface, where[0], where[1], vertex->samples);
    vertex->st[0] = piece->middle[0]; vertex->st[1] = piece->middle[1];
    vertex->spacing = -(float)(piece->node + 1);
    return xgl_build.vertex_count++;
}

/* Lay the pieces of the face cut last into the scratch mesh. Returns 0 when
 * they come to more than one mesh may hold. */
static int XGL_ShapeEmit (msurface_t *surface, int pieces, const float *anchor, const float *map)
{
    static xgl_shape_piece_t piece;
    static int index[XGL_SHAPE_CORNERS];
    unsigned short *triangle;
    int p, i, center, count;
    xgl_build.current = 0;
    xgl_build.vertex_count = xgl_build.triangle_count = xgl_build.edge_count = 0;
    for (p = 0; p < pieces; ++p) {
        if ((p & 15) == 15) XGL_AudioUpdate();
        if (!XGL_ShapePiece(p, &piece)) return 0;
        count = piece.center ? piece.count : piece.count - 2;
        if (xgl_build.triangle_count + count > XGL_SHAPE_TRIANGLES) return 0;
        for (i = 0; i < piece.count; ++i) {
            index[i] = XGL_ShapeVertex(surface, piece.corner[i], anchor, map, &piece);
            if (index[i] < 0) return 0;
        }
        center = index[0];
        if (piece.center) {
            center = XGL_ShapeVertex(surface, piece.middle, anchor, map, &piece);
            if (center < 0) return 0;
        }
        for (i = 0; i < count; ++i) {
            triangle = xgl_build.triangles[0][xgl_build.triangle_count];
            xgl_build.tested[0][xgl_build.triangle_count++] = 0;
            triangle[0] = (unsigned short)center;
            if (piece.center) {
                triangle[1] = (unsigned short)index[i];
                triangle[2] = (unsigned short)index[(i + 1) % piece.count];
            } else {
                triangle[1] = (unsigned short)index[i + 1];
                triangle[2] = (unsigned short)index[i + 2];
            }
        }
    }
    return 1;
}

/* Build, in the scratch, a face's mesh cut along the shapes of its texture
 * at a step of detail. A face too large for that step to fit one mesh is
 * cut at the finest step that does, and xgl_build.shape_fit says which.
 * Returns 0, with nothing built, when the face shows no part of the texture
 * that the step cuts or no step fits: its plain mesh then serves. */
static int XGL_BuildShapes (model_t *model, msurface_t *surface, float longest, int step)
{
    xgl_shape_tree_t *tree;
    const byte *steps;
    xgl_shape_terms_t terms;
    vec3_t points[XGL_MESH_POLYGON], first, second;
    float st[XGL_MESH_POLYGON][2], anchor[5], map[6], e1[2], e2[2], determinant, best;
    int i, j, k, count, bi, bj, pieces, asked, built;
    xgl_build.shape_fit = -1;
    tree = XGL_SurfaceShapes(surface);
    count = surface->numedges;
    if (!tree || count < 3 || count > XGL_MESH_POLYGON) return 0;
    steps = XGL_ShapeSteps(tree);
    for (i = 0; i < count; ++i) {
        XGL_SurfacePoint(model, surface, i, points[i]);
        for (j = 0; j < 2; ++j)
            st[i][j] = DotProduct(points[i], surface->texinfo->vecs[j]) + surface->texinfo->vecs[j][3];
    }
    /* Texture coordinates back to positions, from the two corners that
     * spread widest about the first. */
    best = 0; bi = 1; bj = 2;
    for (i = 1; i < count; ++i)
        for (j = i + 1; j < count; ++j) {
            determinant = (st[i][0] - st[0][0]) * (st[j][1] - st[0][1]) -
                          (st[j][0] - st[0][0]) * (st[i][1] - st[0][1]);
            if (fabs(determinant) > fabs(best)) { best = determinant; bi = i; bj = j; }
        }
    if (fabs(best) < .001f) return 0;
    e1[0] = st[bi][0] - st[0][0]; e1[1] = st[bi][1] - st[0][1];
    e2[0] = st[bj][0] - st[0][0]; e2[1] = st[bj][1] - st[0][1];
    VectorSubtract(points[bi], points[0], first); VectorSubtract(points[bj], points[0], second);
    for (k = 0; k < 3; ++k) {
        map[k] = (first[k] * e2[1] - second[k] * e1[1]) / best;
        map[3 + k] = (second[k] * e1[0] - first[k] * e2[0]) / best;
        anchor[k] = points[0][k];
    }
    anchor[3] = st[0][0]; anchor[4] = st[0][1];
    asked = step; built = 0;
    /* Too much for one mesh: the next step down that cuts less. A step
     * that cuts nothing this face shows ends it: no lower one will. */
    for (; step > 0; step = steps[step - 1]) {
        XGL_ShapeTerms(tree, step, &terms);
        pieces = XGL_ShapeCutFace(tree, &terms, st, count, longest);
        if (!pieces) break;
        if (pieces > 0 && XGL_ShapeEmit(surface, pieces, anchor, map)) {
            built = 1;
            break;
        }
    }
    if (step != asked) xgl_build.shape_fit = built ? step : 0;
    return built;
}

/* detail is zero for the coarse mesh, else the most triangles the detailed
 * one may have. shape_step, when above zero, has the detailed mesh cut
 * along the shapes of the face's texture at that step of detail instead.
 * Returns 1 when built, 0 when this face can have no such mesh (a detailed
 * one that would be no finer than the coarse one included), and -1 when the
 * cache had no room even after evicting idle detail. */
static int XGL_BuildSurfaceMeshData (model_t *model, msurface_t *surface,
                                xgl_surface_mesh_t *mesh, int detail, int shape_step)
{
    texture_t *texture;
    float grid, step, threshold;
    int level, base, limit;
    xgl_build.capped = 0;
    xgl_build.shape_fit = -1;
    if (surface->numedges < 3) return 0;
    if (surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) {
        if (surface->flags & SURF_DRAWSKY) {
            /* One grid serves the sky at every distance. */
            if (detail) return 0;
            grid = XGL_SKY_GRID;
        } else grid = detail ? XGL_WATER_FINE_GRID : XGL_WATER_GRID;
        /* An enormous face still gets a coarse mesh, on a wider grid. */
        while (!XGL_BuildGrid(model, surface, grid)) {
            if (detail || grid >= 4096) return 0;
            grid *= 2;
        }
        return XGL_BuildFinish(surface, mesh);
    }
    if (detail && shape_step > 0) {
        if (!XGL_BuildShapes(model, surface, XGL_Setting(xgl_subdivide.value, 8, 256), shape_step))
            return 0;
        return XGL_BuildFinish(surface, mesh);
    }
    texture = detail ? XGL_SurfaceTexture(surface->texinfo->texture, NULL) : NULL;
    if (texture && (!texture->width || !texture->height)) texture = NULL;
    if (!XGL_BuildBase(model, surface, texture, detail)) return 0;
    if (detail) {
        step = XGL_Setting(xgl_subdivide.value, 8, 256);
        threshold = XGL_Setting(xgl_detailerror.value, 1, 255);
        limit = detail < 4 ? 4 : (detail > XGL_MESH_TRIANGLES ? XGL_MESH_TRIANGLES : detail);
        xgl_build.minimum = XGL_Setting(xgl_detailedge.value, 4, 64);
        base = xgl_build.triangle_count;
        xgl_build.capped = 0;
        memset(xgl_build.slots, 0, sizeof(xgl_build.slots));
        for (level = 0; level < XGL_DETAIL_LEVELS; ++level)
            if (!XGL_BuildRefine(surface, texture, step, threshold, limit)) break;
        if (xgl_build.triangle_count == base) return 0;
        XGL_BuildSpacing();
    }
    return XGL_BuildFinish(surface, mesh);
}

static int XGL_BuildSurfaceMesh (model_t *model, msurface_t *surface,
                                xgl_surface_mesh_t *mesh, int detail, int shape_step)
{
    double start;
    int built;
    start = XGL_ProfileDetailNow();
    built = XGL_BuildSurfaceMeshData(model, surface, mesh, detail, shape_step);
    XGL_ProfileAdd(XGL_PROFILE_WORLD_BUILD, start);
    if (built > 0) {
        XGL_ProfileCount(XGL_PROFILE_WORLD_MESH_BUILDS, 1);
        XGL_ProfileCount(XGL_PROFILE_WORLD_BUILD_VERTICES, (unsigned long)mesh->unique);
    } else if (built < 0 || !detail) XGL_ProfileCount(XGL_PROFILE_WORLD_MESH_FAILURES, 1);
    return built;
}

/* The most triangles a face may be given at a squared distance: in
 * proportion to its apparent size, its area over that distance, so that
 * triangles come out about the same size on screen near and far and the
 * frame's allowance is spread over every wall in range instead of going to
 * the first few. Without a density every face may have the per-face limit. */
static int XGL_DetailWant (float area, float distance_squared)
{
    float limit, want;
    limit = XGL_Setting(xgl_detailface.value, 4, XGL_MESH_TRIANGLES);
    if (!(xgl_detaildensity.value > 0)) return (int)limit;
    want = xgl_detaildensity.value * area / (distance_squared + 64);
    return (int)(want < limit ? want : limit);
}

/* Build a face's detailed mesh if this frame's build allowance permits.
 * A capped or memory-starved build is simply tried again in a later frame;
 * the face draws its coarse mesh meanwhile. A face that has nothing to show
 * in more detail is examined once and never again. A mesh that its triangle
 * limit cut short is built again when `want`, which grows as the viewer
 * approaches, reaches twice that limit; the old one serves until then.
 *
 * shape_step is -1 for such a face. For a wall cut along the shapes of its
 * texture it is the step of detail the distance asks for, and the mesh
 * follows it both ways: finer at once, coarser only when the step asks for
 * half the pieces or fewer, so that a wall does not change with every pace
 * back and forth, and then without hurry. */
static void XGL_TryBuildDetail (model_t *model, msurface_t *surface,
                                xgl_surface_cache_t *cache, int want, int shape_step)
{
    xgl_surface_mesh_t mesh;
    int built, built_triangles, limit, most, larger, change, coarser;
    /* Build somewhat ahead of the present need, to rebuild less often. */
    most = limit = (int)XGL_Setting(xgl_detailface.value, 4, XGL_MESH_TRIANGLES);
    if (xgl_detaildensity.value > 0 && want + want / 2 < limit) limit = want + want / 2;
    coarser = 0;
    if (shape_step >= 0) {
        /* A face too large for the finer steps was found out when it was
         * first asked for one: it is not tried again. */
        if (cache->shape_most && shape_step >= cache->shape_most) shape_step = cache->shape_most - 1;
        /* With no limit on building a wall always has just what is asked. */
        if (!(xgl_detailbuild.value > 0)) change = shape_step != cache->shape_step;
        else change = shape_step > cache->shape_step ||
                      xgl_shape_levels[shape_step] * 2 <= xgl_shape_levels[cache->shape_step];
        coarser = change && shape_step < cache->shape_step;
        /* Making a mesh coarser is never urgent: one in every few frames. */
        if (coarser && xgl_detailbuild.value > 0 &&
            (r_framecount % XGL_SHAPE_COARSEN || xgl_coarsen_frame == r_framecount)) change = 0;
        /* A build that found no memory waits its turn like any other. */
        if (!change || (cache->detail_retry && r_framecount < cache->detail_retry)) return;
        if (!shape_step) {
            /* The plain mesh shows all the wall can be given from here. */
            if (cache->detail.vertices) {
                if (cache->detail.queued_frame == r_framecount) XGL_Flush();
                XGL_FreeSurfaceMesh(&cache->detail);
                xgl_coarsen_frame = r_framecount;
            }
            cache->shape_step = 0;
            cache->detail_retry = 0;
            return;
        }
    } else {
        /* Worth building again at twice the size, or to reach the most a
         * face may have from anything up to three quarters of it. */
        larger = cache->detail_limit && (limit >= cache->detail_limit * 2 ||
            (limit == most && cache->detail_limit * 4 <= most * 3));
        if (cache->detail.vertices && !larger) return;
        if (cache->detail_attempted && !larger &&
            !(cache->detail_retry && r_framecount >= cache->detail_retry)) return;
    }
    if (xgl_detailbuild.value > 0 && xgl_detail_built >= xgl_detailbuild.value) return;
    cache->detail_attempted = 1;
    cache->detail_retry = 0;
    memset(&mesh, 0, sizeof(mesh));
    built = XGL_BuildSurfaceMesh(model, surface, &mesh, limit, shape_step);
    built_triangles = built > 0 ? mesh.count / 3 : 0;
    xgl_detail_built += built_triangles > XGL_DETAIL_ATTEMPT ? built_triangles : XGL_DETAIL_ATTEMPT;
    if (built < 0) {
        /* No memory: whatever mesh the face has stays in use. */
        cache->detail_retry = r_framecount + XGL_DETAIL_RETRY_FRAMES;
        return;
    }
    cache->detail_limit = xgl_build.capped ? limit : 0;
    if (shape_step >= 0) {
        cache->shape_step = (short)shape_step;
        if (xgl_build.shape_fit >= 0) {
            cache->shape_most = (short)(xgl_build.shape_fit + 1);
            cache->shape_step = (short)xgl_build.shape_fit;
        }
        if (coarser) xgl_coarsen_frame = r_framecount;
    }
    if (cache->detail.vertices && (built || shape_step >= 0)) {
        /* A brush entity may have queued the old mesh in this frame. A wall
         * cut along shapes that now shows nothing to cut goes plain. */
        if (cache->detail.queued_frame == r_framecount) XGL_Flush();
        XGL_FreeSurfaceMesh(&cache->detail);
    }
    if (!built) return;
    cache->detail = mesh;
    cache->detail_used = r_framecount;
}

/* GLQuake's sky projection: the layers repeat every 128 units of s and t. */
static void XGL_SkyCoordinates (const float *position, float *s, float *t)
{
    vec3_t direction;
    float scale;
    VectorSubtract(position, r_origin, direction);
    direction[2] *= 3;
    scale = DotProduct(direction, direction);
    scale = scale > .0001f ? 378.0f / (float)sqrt(scale) : 0;
    *s = direction[0] * scale;
    *t = direction[1] * scale;
}

/* Reduce both sky layers to cells. In the cloud layer index zero is
 * transparent, so its color is stored already multiplied by its coverage:
 * interpolating that and laying it over the back layer cannot bring in the
 * color of the holes. Returns 0 for a sky too small or malformed to reduce. */
static int XGL_SkyFilter (texture_t *texture)
{
    const byte *pixels;
    unsigned int revision, sum[2][3], covered;
    int half, cx, cy, x, y, x0, x1, y0, y1, c, index, count, cell;
    revision = XGL_TextureRevision(texture->gl_texturenum);
    if (xgl_sky.texture == texture && xgl_sky.revision == revision) return xgl_sky.valid;
    xgl_sky.texture = texture;
    xgl_sky.revision = revision;
    xgl_sky.valid = 0;
    half = texture->width / 2;
    if (half < XGL_SKY_CELLS || texture->height < XGL_SKY_CELLS ||
        texture->offsets[0] < sizeof(texture_t)) return 0;
    pixels = (const byte *)texture + texture->offsets[0];
    for (cy = 0; cy < XGL_SKY_CELLS; ++cy) {
        XGL_AudioUpdate();
        y0 = cy * (int)texture->height / XGL_SKY_CELLS;
        y1 = (cy + 1) * (int)texture->height / XGL_SKY_CELLS;
        for (cx = 0; cx < XGL_SKY_CELLS; ++cx) {
            x0 = cx * half / XGL_SKY_CELLS; x1 = (cx + 1) * half / XGL_SKY_CELLS;
            memset(sum, 0, sizeof(sum));
            covered = 0;
            for (y = y0; y < y1; ++y)
                for (x = x0; x < x1; ++x) {
                    index = pixels[y * (int)texture->width + x];
                    if (index) {
                        ++covered;
                        for (c = 0; c < 3; ++c) sum[0][c] += xgl_palette[index * 3 + c];
                    }
                    index = pixels[y * (int)texture->width + half + x];
                    for (c = 0; c < 3; ++c) sum[1][c] += xgl_palette[index * 3 + c];
                }
            count = (x1 - x0) * (y1 - y0);
            cell = cy * XGL_SKY_CELLS + cx;
            for (c = 0; c < 3; ++c) {
                xgl_sky.front[cell][c] = (byte)((sum[0][c] + count / 2) / count);
                xgl_sky.back[cell][c] = (byte)((sum[1][c] + count / 2) / count);
            }
            xgl_sky.front[cell][3] = (byte)((covered * 255 + count / 2) / count);
        }
    }
    xgl_sky.valid = 1;
    return 1;
}

/* Interpolate between the four cells around a point given in layer periods. */
static void XGL_SkyCells (const byte *cells, int width, float u, float v, float *out)
{
    const byte *p00, *p10, *p01, *p11;
    float fx, fy, top, bottom;
    int x0, x1, y0, y1, c;
    u -= (float)(int)u; v -= (float)(int)v;
    if (u < 0) u += 1;
    if (v < 0) v += 1;
    fx = u * XGL_SKY_CELLS - .5f; fy = v * XGL_SKY_CELLS - .5f;
    if (fx < 0) fx += XGL_SKY_CELLS;
    if (fy < 0) fy += XGL_SKY_CELLS;
    x0 = (int)fx; y0 = (int)fy;
    fx -= x0; fy -= y0;
    if (x0 >= XGL_SKY_CELLS) x0 = 0;
    if (y0 >= XGL_SKY_CELLS) y0 = 0;
    x1 = x0 + 1 == XGL_SKY_CELLS ? 0 : x0 + 1;
    y1 = y0 + 1 == XGL_SKY_CELLS ? 0 : y0 + 1;
    p00 = cells + (y0 * XGL_SKY_CELLS + x0) * width;
    p10 = cells + (y0 * XGL_SKY_CELLS + x1) * width;
    p01 = cells + (y1 * XGL_SKY_CELLS + x0) * width;
    p11 = cells + (y1 * XGL_SKY_CELLS + x1) * width;
    for (c = 0; c < width; ++c) {
        top = p00[c] + (p10[c] - p00[c]) * fx;
        bottom = p01[c] + (p11[c] - p01[c]) * fx;
        out[c] = top + (bottom - top) * fy;
    }
}

/* GLQuake draws the background and the cutout clouds as separate passes.
 * Here the two scrolling layers are composed into one color, 0..255. */
static void XGL_SampleSky (texture_t *texture, int filtered, const float *position,
                          float slow, float fast, float rgb[3])
{
    float s, t, front[4], back[3], clear;
    int i;
    XGL_SkyCoordinates(position, &s, &t);
    if (!filtered) {
        /* A malformed or placeholder sky has no addressable split image. */
        rgb[0] = rgb[1] = rgb[2] = .5f;
        XGL_SampleTexture(texture->gl_texturenum, (s + slow) / 128, (t + slow) / 128, rgb, NULL);
        for (i = 0; i < 3; ++i) rgb[i] *= 255;
        return;
    }
    XGL_SkyCells(xgl_sky.front[0], 4, (s + fast) / 128, (t + fast) / 128, front);
    XGL_SkyCells(xgl_sky.back[0], 3, (s + slow) / 128, (t + slow) / 128, back);
    clear = 1 - front[3] * (1.0f / 255);
    for (i = 0; i < 3; ++i) rgb[i] = front[i] + back[i] * clear;
}

/* The darker and the lighter half of a liquid texture, each averaged: the
 * two tones its ripples move between. A 64-unit grid cannot show the
 * texture's own 64-texel pattern, whose true average is one flat color. */
static const xgl_tone_t *XGL_WaterTones (texture_t *texture)
{
    xgl_tone_t *entry;
    float samples[64][3], luminance[64], sum[2][3], mean;
    unsigned int revision;
    int i, j, count[2], side;
    revision = XGL_TextureRevision(texture->gl_texturenum);
    for (i = 0; revision && i < XGL_TONE_CACHE; ++i)
        if (xgl_tones[i].texture == texture->gl_texturenum && xgl_tones[i].revision == revision)
            return &xgl_tones[i];
    entry = &xgl_tones[xgl_tone_next];
    xgl_tone_next = (xgl_tone_next + 1) % XGL_TONE_CACHE;
    mean = 0;
    for (i = 0; i < 64; ++i) {
        samples[i][0] = samples[i][1] = samples[i][2] = .5f;
        XGL_SampleTextureFiltered(texture->gl_texturenum, ((i & 7) + .5f) / 8,
                                  ((i >> 3) + .5f) / 8, .125f, .125f, samples[i], NULL);
        luminance[i] = samples[i][0] * .3f + samples[i][1] * .59f + samples[i][2] * .11f;
        mean += luminance[i];
    }
    mean /= 64;
    memset(sum, 0, sizeof(sum));
    count[0] = count[1] = 0;
    for (i = 0; i < 64; ++i) {
        side = luminance[i] > mean;
        for (j = 0; j < 3; ++j) sum[side][j] += samples[i][j];
        ++count[side];
    }
    /* A texture of one color has only one half, whichever way rounding
     * puts its samples against their own mean. */
    if (!count[0]) { count[0] = count[1]; VectorCopy(sum[1], sum[0]); }
    if (!count[1]) { count[1] = count[0]; VectorCopy(sum[0], sum[1]); }
    for (side = 0; side < 2; ++side)
        for (j = 0; j < 3; ++j)
            entry->tone[side][j] = (byte)(sum[side][j] / count[side] * 255 + .5f);
    entry->texture = texture->gl_texturenum;
    entry->revision = revision;
    return entry;
}

/* Sky and water are colored every frame, once per distinct vertex. Water
 * moves between its texture's two tones on a long wave and, on the finer
 * grid, a shorter crossing one. */
static void XGL_UpdateSpecialSurface (xgl_surface_mesh_t *mesh, msurface_t *surface,
                                      texture_t *texture, entity_t *entity, int fine)
{
    static byte done[XGL_MESH_VERTICES];
    const xgl_cached_vertex_t *vertex;
    const xgl_tone_t *tones;
    xgl_color_vertex_t *draw;
    vec3_t forward, right, up, position, delta;
    float rgb[3], *color, os, ot, slow, fast, long_phase, short_phase, wave, weight, swell;
    float fade, reach, near_squared, far_squared, value;
    int i, j, index, transformed, sky, filtered, samples;
    sky = (surface->flags & SURF_DRAWSKY) != 0;
    transformed = sky && entity && entity != &r_worldentity;
    if (transformed) XGL_BrushBasis(entity, forward, right, up);
    XGL_WorldGamma();
    slow = fast = long_phase = short_phase = near_squared = far_squared = 0;
    filtered = 0; tones = NULL;
    /* Bound time before the table lookups, including in long sessions. */
    if (sky) {
        slow = (float)fmod(realtime * 8, 128);
        fast = (float)fmod(realtime * 16, 128);
        filtered = XGL_SkyFilter(texture);
    } else {
        tones = XGL_WaterTones(texture);
        /* Whole cycles are added so that no table index goes negative. */
        long_phase = (float)fmod(realtime / XGL_WAVE_LONG_PERIOD, 1) + 64;
        short_phase = (float)fmod(realtime / XGL_WAVE_SHORT_PERIOD, 1) + 256;
        /* The short ripple needs the finer grid. It fades out toward the
         * distance at which faces fall back to the coarser one, so no step
         * shows where the two grids meet. */
        reach = fine && entity == &r_worldentity ? xgl_detaildistance.value : 0;
        far_squared = reach * reach;
        near_squared = far_squared * .25f;
    }
    samples = 0;
    memset(done, 0, (size_t)mesh->unique);
    for (i = 0, draw = mesh->vertices; i < mesh->count; ++i, ++draw) {
        index = mesh->corner[i];
        color = xgl_vertex_colors[index];
        if (!done[index]) {
            done[index] = 1;
            ++samples;
            if (sky) {
                if (transformed)
                    XGL_BrushPoint(draw->position, entity, forward, right, up, position);
                else VectorCopy(draw->position, position);
                XGL_SampleSky(texture, filtered, position, slow, fast, rgb);
            } else {
                /* Like the original turbulence, the waves ignore the
                 * texture vectors' translation. */
                vertex = mesh->data + index;
                os = vertex->st[0] - surface->texinfo->vecs[0][3];
                ot = vertex->st[1] - surface->texinfo->vecs[1][3];
                wave = xgl_turb_sin[(int)(((os + ot) * (1.0f / XGL_WAVE_LONG) + long_phase) * 256) & 255] * .125f;
                weight = .5f + .35f * wave;
                swell = 1 + .06f * wave;
                if (far_squared > 0) {
                    VectorSubtract(draw->position, r_origin, delta);
                    fade = (far_squared - DotProduct(delta, delta)) / (far_squared - near_squared);
                    if (fade > 0) {
                        if (fade > 1) fade = 1;
                        wave = xgl_turb_sin[(int)(((os - ot) * (1.0f / XGL_WAVE_SHORT) + short_phase) * 256) & 255] * .125f;
                        weight += .25f * fade * wave;
                    }
                }
                if (weight < 0) weight = 0;
                if (weight > 1) weight = 1;
                for (j = 0; j < 3; ++j)
                    rgb[j] = (tones->tone[0][j] + (tones->tone[1][j] - tones->tone[0][j]) * weight) * swell;
            }
            for (j = 0; j < 3; ++j) {
                value = rgb[j];
                if (!(value >= 0)) value = 0;
                if (value > 255) value = 255;
                color[j] = xgl_gamma[(int)(value + .5f)];
            }
        }
        draw->color.rgb[0] = color[0];
        draw->color.rgb[1] = color[1];
        draw->color.rgb[2] = color[2];
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

/* Filtered texture colors are sampled once per distinct vertex, over the
 * spacing of the vertices around it, and kept as bytes; later light, style
 * and gamma changes never touch the texture again. The pieces of a wall cut
 * along its texture's shapes are not sampled: each has the color the
 * division gives its node in this texture, which for an animated texture
 * is the frame now showing. */
static void XGL_SampleMeshMaterials (xgl_surface_mesh_t *mesh, const msurface_t *surface,
                                     texture_t *texture, unsigned int texture_revision)
{
    xgl_cached_vertex_t *vertex;
    const xgl_shape_tree_t *tree;
    const byte *colors;
    int i, node, looked;
    tree = NULL; colors = NULL; looked = 0;
    for (i = 0, vertex = mesh->data; i < mesh->unique; ++i, ++vertex) {
        if ((i & 255) == 255) XGL_AudioUpdate();
        vertex->material[0] = vertex->material[1] = vertex->material[2] = 128;
        if (vertex->spacing < 0) {
            if (!looked) {
                looked = 1;
                tree = XGL_SurfaceShapes(surface);
                colors = XGL_ShapeColors(tree, texture);
            }
            node = (int)(-vertex->spacing) - 1;
            if (colors && node < tree->count) memcpy(vertex->material, colors + 3 * node, 3);
            /* The division is not to be had: the piece's middle, then. */
            else XGL_SampleTextureBox(texture->gl_texturenum, vertex->st[0], vertex->st[1], 2, vertex->material);
            continue;
        }
        XGL_SampleTextureBox(texture->gl_texturenum, vertex->st[0], vertex->st[1],
                             vertex->spacing * XGL_DETAIL_FILTER, vertex->material);
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
    xgl_color_vertex_t *draw;
    const float *color;
    float *out;
    unsigned int level;
    int weight[MAXLIGHTMAPS], layers, layer, value, i, unlit;
    unlit = fullbright || !cl.worldmodel->lightdata ||
        (surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWTILED));
    layers = 0;
    if (!unlit)
        for (; layers < MAXLIGHTMAPS && surface->styles[layers] != 255; ++layers)
            weight[layers] = d_lightstylevalue[surface->styles[layers]];
    level = XGL_LIGHT_FULL;
    out = xgl_vertex_colors[0];
    for (i = 0, vertex = mesh->data; i < mesh->unique; ++i, ++vertex, out += 3) {
        if ((i & 255) == 255) XGL_AudioUpdate();
        if (!unlit) {
            value = 0;
            for (layer = 0; layer < layers; ++layer)
                value += vertex->samples[layer] * weight[layer];
            level = value > 0 ? 5U * (unsigned int)value : 0;
            if (level < XGL_LIGHT_FLOOR) level = XGL_LIGHT_FLOOR;
            if (level > XGL_LIGHT_FULL) level = XGL_LIGHT_FULL;
        }
        out[0] = xgl_gamma[(vertex->material[0] * level + XGL_LIGHT_FULL / 2) / XGL_LIGHT_FULL];
        out[1] = xgl_gamma[(vertex->material[1] * level + XGL_LIGHT_FULL / 2) / XGL_LIGHT_FULL];
        out[2] = xgl_gamma[(vertex->material[2] * level + XGL_LIGHT_FULL / 2) / XGL_LIGHT_FULL];
    }
    for (i = 0, draw = mesh->vertices; i < mesh->count; ++i, ++draw) {
        color = xgl_vertex_colors[mesh->corner[i]];
        draw->color.rgb[0] = color[0];
        draw->color.rgb[1] = color[1];
        draw->color.rgb[2] = color[2];
    }
}

static void XGL_UpdateSurfaceMesh (xgl_surface_mesh_t *mesh, msurface_t *surface,
                                   texture_t *texture, entity_t *entity, int fine)
{
    int i, changed, fullbright, texture_changed, value;
    unsigned int texture_revision, color_revision;
    double start;
    if (surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) {
        XGL_SurfaceWillChange(mesh);
        start = XGL_ProfileDetailNow();
        XGL_UpdateSpecialSurface(mesh, surface, texture, entity, fine);
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
    if (texture_changed) XGL_SampleMeshMaterials(mesh, surface, texture, texture_revision);
    XGL_WorldGamma();
    XGL_ColorMesh(mesh, surface, fullbright);
    mesh->lit_current = 0;
    mesh->color_revision = color_revision;
    mesh->fullbright = fullbright;
    XGL_ProfileAdd(XGL_PROFILE_WORLD_COLORS, start);
    XGL_ProfileCount(XGL_PROFILE_WORLD_COLOR_UPDATES, 1);
    XGL_ProfileCount(XGL_PROFILE_WORLD_COLOR_VERTICES, (unsigned long)mesh->unique);
}

/* A face's normal as illumination wants it: out of its visible side. */
static void XGL_SurfaceNormal (const msurface_t *surface, vec3_t normal)
{
    float sign;
    sign = (surface->flags & SURF_PLANEBACK) ? -1.0f : 1.0f;
    normal[0] = surface->plane->normal[0] * sign;
    normal[1] = surface->plane->normal[1] * sign;
    normal[2] = surface->plane->normal[2] * sign;
}

/* Make sure a mesh has its illuminated copy and that the copy's colors are
 * the mesh's. Returns 0 when the cache has no room for one; the face is
 * then copied out vertex by vertex instead. */
static int XGL_LitCopy (xgl_surface_mesh_t *mesh, const msurface_t *surface)
{
    xgl_native_vertex_t *lit;
    const xgl_color_vertex_t *source;
    vec3_t normal;
    size_t bytes;
    int i;
    if (!mesh->lit) {
        bytes = (size_t)mesh->count * sizeof(*mesh->lit);
        if (!XGL_CacheFits(bytes)) {
            if (r_framecount < xgl_lit_retry) return 0;
            XGL_EvictDetail(bytes);
            if (!XGL_CacheFits(bytes)) {
                xgl_lit_retry = r_framecount + XGL_DETAIL_RETRY_FRAMES;
                return 0;
            }
        }
        mesh->lit = (xgl_native_vertex_t *)malloc(bytes);
        if (!mesh->lit) return 0;
        memset(mesh->lit, 0, bytes);
        xgl_cache_bytes += bytes;
        mesh->lit_bytes = bytes;
        XGL_SurfaceNormal(surface, normal);
        for (i = 0, lit = mesh->lit, source = mesh->vertices; i < mesh->count; ++i, ++lit, ++source) {
            VectorCopy(source->position, lit->position);
            VectorCopy(normal, lit->normal);
        }
        mesh->lit_current = 0;
    }
    if (!mesh->lit_current) {
        for (i = 0, lit = mesh->lit, source = mesh->vertices; i < mesh->count; ++i, ++lit, ++source) {
            lit->color.rgb[0] = source->color.rgb[0];
            lit->color.rgb[1] = source->color.rgb[1];
            lit->color.rgb[2] = source->color.rgb[2];
        }
        mesh->lit_current = 1;
    }
    mesh->lit_frame = r_framecount;
    return 1;
}

/* Unlit faces go out as they are stored, without normals. A face that a
 * dynamic light reaches goes out from its illuminated copy. Brush entities
 * and streamed faces are copied into the backend's own batch. */
static void XGL_EmitSurfaceVertices (xgl_surface_mesh_t *mesh, msurface_t *surface,
                                    entity_t *entity, int lit, float opacity,
                                    int persistent)
{
    const xgl_color_vertex_t *source;
    xgl_color_vertex_t *plain;
    xgl_native_vertex_t *vertices;
    vec3_t forward, right, up, normal, turned;
    float *position;
    int i, n, transformed, rotated, count;
    source = mesh->vertices; count = mesh->count;
    VectorCopy(vec3_origin, normal);
    transformed = entity && entity != &r_worldentity;
    if (!transformed && persistent && (!lit || XGL_LitCopy(mesh, surface))) {
        if (lit) XGL_DrawNativeTriangles(mesh->lit, count, lit, opacity, opacity >= 1);
        else XGL_DrawColorTriangles(source, count, opacity, opacity >= 1);
        mesh->queued_frame = r_framecount;
        c_brush_polys += count / 3;
        return;
    }
    rotated = transformed ? XGL_BrushBasis(entity, forward, right, up) : 0;
    if (lit) {
        XGL_SurfaceNormal(surface, normal);
        if (rotated) {
            for (i = 0; i < 3; ++i)
                turned[i] = normal[0] * forward[i] - normal[1] * right[i] + normal[2] * up[i];
            VectorCopy(turned, normal);
        }
    }
    plain = NULL; vertices = NULL;
    while (count > 0) {
        n = count > XGL_WORLD_BATCH ? XGL_WORLD_BATCH : count;
        if (lit) vertices = XGL_ReserveTriangles(&n, lit, opacity, opacity >= 1);
        else plain = XGL_ReserveColorTriangles(&n, opacity, opacity >= 1);
        if (!vertices && !plain) return;
        for (i = 0; i < n; ++i) {
            if (lit) {
                position = vertices[i].position;
                VectorCopy(source[i].color.rgb, vertices[i].color.rgb);
                VectorCopy(normal, vertices[i].normal);
            } else {
                position = plain[i].position;
                VectorCopy(source[i].color.rgb, plain[i].color.rgb);
            }
            if (rotated) XGL_BrushPoint(source[i].position, entity, forward, right, up, position);
            else if (transformed) {
                /* Doors, platforms and item boxes: translation only. */
                VectorAdd(source[i].position, entity->origin, position);
            } else { VectorCopy(source[i].position, position); }
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

/* The scene lights that reach a sphere. For a face, a light behind its plane
 * is left out: it adds nothing to any of the face's vertices. */
static int XGL_LightsReaching (const float *center, float radius, const msurface_t *surface)
{
    const xgl_point_light_t *light;
    vec3_t delta;
    float height, reach;
    int i, mask;
    mask = 0;
    for (i = 0, light = xgl_lights; i < xgl_light_count; ++i, ++light) {
        if (surface) {
            height = DotProduct(light->origin, surface->plane->normal) - surface->plane->dist;
            if (surface->flags & SURF_PLANEBACK) height = -height;
            if (height <= 0 || (light->range > 0 && height >= light->range)) continue;
        }
        if (light->range > 0) {
            VectorSubtract(light->origin, center, delta);
            reach = light->range + radius;
            if (DotProduct(delta, delta) >= reach * reach) continue;
        }
        mask |= 1 << i;
    }
    return mask;
}

int XGL_BrushEntityLights (entity_t *entity)
{
    model_t *model;
    vec3_t center;
    float radius, half;
    int i, mask;
    if (r_fullbright.value) return 0;
    mask = xgl_light_all ? XGL_LIT_AMBIENT : 0;
    if (!xgl_light_count) return mask;
    model = entity->model;
    if (entity->angles[0] || entity->angles[1] || entity->angles[2]) {
        /* A rotated entity is bounded by a sphere about its origin. */
        VectorCopy(entity->origin, center);
        radius = model->radius;
    } else {
        radius = 0;
        for (i = 0; i < 3; ++i) {
            center[i] = entity->origin[i] + (model->mins[i] + model->maxs[i]) * .5f;
            half = (model->maxs[i] - model->mins[i]) * .5f;
            radius += half * half;
        }
        radius = (float)sqrt(radius);
    }
    return mask | XGL_LightsReaching(center, radius, NULL);
}

/* Decide which visible walls are drawn illuminated, and tell the backend
 * which lights that geometry needs: each enabled light costs time on every
 * illuminated triangle, whether or not it reaches it. */
static void XGL_MarkLitSurfaces (void)
{
    xgl_surface_cache_t *cache;
    msurface_t *surface;
    entity_t *entity;
    int i, index, mask, reached, walls, lit, lights;
    xgl_light_count = r_fullbright.value ? 0 : XGL_PointLights(&xgl_lights);
    xgl_light_all = !r_fullbright.value && XGL_AmbientLight() < 1;
    xgl_light_split = 0;
    mask = walls = lit = 0;
    if (xgl_light_count && !xgl_world_order) {
        /* Without the cache index the traversal has already drawn the walls,
         * all of them illuminated. */
        mask = (1 << xgl_light_count) - 1;
    } else if ((xgl_light_count || xgl_light_all) && xgl_world_order) {
        for (i = 0; i < xgl_world_count; ++i) {
            index = xgl_world_order[i];
            surface = cl.worldmodel->surfaces + index;
            if (surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) continue;
            ++walls;
            if (!xgl_light_count) continue;
            cache = xgl_surfaces + index;
            XGL_SurfaceBounds(cl.worldmodel, surface, cache);
            reached = XGL_LightsReaching(cache->center, cache->radius, surface);
            if (!reached) continue;
            cache->light_frame = r_framecount;
            mask |= reached; ++lit;
        }
        if (xgl_light_count && r_drawentities.value)
            for (i = 0; i < cl_numvisedicts; ++i) {
                entity = cl_visedicts[i];
                if (entity && entity->model && entity->model->type == mod_brush)
                    mask |= XGL_BrushEntityLights(entity) & ((1 << XGL_MAX_POINT_LIGHTS) - 1);
            }
        xgl_light_split = !xgl_light_all && lit > 0 && lit < walls;
    }
    XGL_UsePointLights(mask);
    for (lights = 0; mask; mask >>= 1) lights += mask & 1;
    XGL_ProfileCount(XGL_PROFILE_POINT_LIGHTS, (unsigned long)lights);
    XGL_ProfileCount(XGL_PROFILE_WORLD_LIT_SURFACES, (unsigned long)(xgl_light_all ? walls : lit));
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
        if (XGL_BuildSurfaceMesh(xgl_cached_world, surface, &cache->coarse, 0, -1) <= 0) continue;
        XGL_SurfaceBounds(xgl_cached_world, surface, cache);
        if (surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) continue;
        texture = XGL_SurfaceTexture(surface->texinfo->texture, NULL);
        if (texture && texture->width && texture->height)
            XGL_SampleMeshMaterials(&cache->coarse, surface, texture,
                                    XGL_TextureRevision(texture->gl_texturenum));
    }
}

/* Cached meshes depend on these settings; a change rebuilds the cache. */
static int XGL_BuildSettingsChanged (void)
{
    return xgl_cache_step != xgl_subdivide.value ||
           xgl_cache_error != xgl_detailerror.value ||
           xgl_cache_face != xgl_detailface.value ||
           xgl_cache_edge != xgl_detailedge.value ||
           xgl_cache_density != xgl_detaildensity.value ||
           xgl_cache_shapes != xgl_shapes.value;
}

/* Examine, while the map loads, the texture of every wall that may be
 * drawn, for the shapes it is to be cut along: that work is then out of
 * the frames. A texture already examined costs nothing. */
static void XGL_ExamineModelShapes (model_t *model)
{
    msurface_t *surface;
    int i;
    for (i = 0, surface = model->surfaces; i < model->numsurfaces; ++i, ++surface) {
        if ((i & 63) == 63) XGL_AudioUpdate();
        if (surface->texinfo && surface->texinfo->texture) XGL_SurfaceShapes(surface);
    }
}

static void XGL_ExamineShapes (void)
{
    model_t *model;
    double start;
    int i, before, examined, shaped;
    if (!xgl_shapes.value) return;
    XGL_ShapesCount(&before, NULL);
    start = Sys_FloatTime();
    XGL_ExamineModelShapes(xgl_cached_world);
    /* The boxes and other models built as maps are. The map's own doors and
     * platforms are among its faces. */
    for (i = 1; i < MAX_MODELS && (model = cl.model_precache[i]) != NULL; ++i)
        if (model != xgl_cached_world && model->type == mod_brush && model->name[0] != '*')
            XGL_ExamineModelShapes(model);
    XGL_ShapesCount(&examined, &shaped);
    if (examined != before)
        Con_Printf("XGL: wall textures examined for shapes: %d in %.1f s, %d with shapes\n",
                   examined, Sys_FloatTime() - start, shaped);
}

/* Start the cache afresh for the map in hand, with the settings as they
 * stand. */
static void XGL_WorldSetup (void)
{
    size_t bytes, each;
    XGL_WorldRelease();
    xgl_cached_world = cl.worldmodel;
    xgl_cache_step = xgl_subdivide.value;
    xgl_cache_error = xgl_detailerror.value;
    xgl_cache_face = xgl_detailface.value;
    xgl_cache_edge = xgl_detailedge.value;
    xgl_cache_density = xgl_detaildensity.value;
    xgl_cache_shapes = xgl_shapes.value;
    /* The steps of detail worked out for the old settings no longer hold. */
    ++xgl_shape_key;
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
    Con_Printf("XGL: sampled BSP cache limited to 8 MiB; detail follows texture and light contrast; complete coarse fallback\n");
    XGL_ExamineShapes();
}

void XGL_WorldNewMap (void)
{
    /* The textures are the new map's: what was known of the old ones goes. */
    XGL_ShapesClear();
    XGL_WorldSetup();
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
 * cut-off do not flicker between their coarse and detailed colors.
 *
 * A wall cut along the shapes of its texture looks wrong with one face left
 * plain. Its faces are therefore taken however small, rank by nearness
 * alone, so that if the allowance runs out it is the far walls that go
 * plain and not the small faces of a near one, and may run somewhat over
 * the allowance before any is dropped. */
static void XGL_SelectWorldDetail(void)
{
    static int counts[XGL_DETAIL_BUCKETS];
    xgl_surface_cache_t *cache;
    msurface_t *surface;
    vec3_t delta;
    float distance, range, allowance, most;
    int i, index, candidates, bucket, start, shaped;
    candidates = shaped = 0;
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
            XGL_BuildSurfaceMesh(cl.worldmodel, surface, &cache->coarse, 0, -1);
        }
        if ((surface->flags & SURF_DRAWSKY) || !(xgl_detaildistance.value > 0)) continue;
        XGL_SurfaceBounds(cl.worldmodel, surface, cache);
        VectorSubtract(cache->center, r_origin, delta);
        distance = DotProduct(delta, delta);
        range = xgl_detaildistance.value + cache->radius;
        if (distance >= range * range) continue;
        if (surface->flags & SURF_DRAWTURB) {
            /* Water in range always uses its finer grid, outside the wall
             * allowance: the ripple that needs it fades with distance, so
             * the change of grid at the range cannot be seen. */
            XGL_TryBuildDetail(cl.worldmodel, surface, cache, 1, -1);
            if (cache->detail.vertices)
                cache->detail_frame = cache->detail_used = r_framecount;
            continue;
        }
        if (!(xgl_detailtris.value > 0)) continue;
        cache->detail_want = XGL_DetailWant(cache->area, distance);
        cache->shape_need = (short)XGL_SurfaceShapeStep(surface, distance);
        if (cache->shape_need > 0) {
            ++shaped;
            bucket += XGL_DetailBucket(XGL_SHAPE_RANK_AREA / (distance + 64));
        } else {
            if (cache->detail_want < XGL_DETAIL_WANT_MIN) continue;
            bucket += XGL_DetailBucket(cache->area / (distance + 64));
        }
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
    most = xgl_detailtris.value;
    if (shaped) most *= XGL_SHAPE_OVER;
    for (i = 0; i < candidates && xgl_detail_used < most; ++i) {
        if ((i & 63) == 0) XGL_AudioUpdate();
        index = xgl_detail_sorted[i]; cache = xgl_surfaces + index;
        allowance = cache->shape_need > 0 ? most : xgl_detailtris.value;
        if (xgl_detail_used >= allowance) continue;
        XGL_TryBuildDetail(cl.worldmodel, cl.worldmodel->surfaces + index, cache,
                           cache->detail_want, cache->shape_need);
        if (cache->detail.vertices &&
            xgl_detail_used + cache->detail.count / 3 <= allowance) {
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

static void XGL_InitCachedVertex (xgl_color_vertex_t *draw, xgl_cached_vertex_t *vertex,
                                  msurface_t *surface, const float *point)
{
    int i;
    VectorCopy(point, draw->position);
    for (i = 0; i < 2; ++i)
        vertex->st[i] = DotProduct(point, surface->texinfo->vecs[i]) + surface->texinfo->vecs[i][3];
    XGL_SurfaceLightSamples(surface, point, vertex->samples);
}

static void XGL_DrawSurface (model_t *model, msurface_t *surface, entity_t *entity, float opacity)
{
    static unsigned short corners[3] = {0, 1, 2};
    xgl_surface_cache_t *cache;
    xgl_surface_mesh_t *mesh;
    xgl_surface_mesh_t fallback;
    xgl_color_vertex_t triangle[3];
    xgl_cached_vertex_t triangle_data[3];
    texture_t *texture;
    vec3_t a, b, c, center, forward, right, up, worldcenter, delta;
    float distance, ds, dt, spacing, allowance;
    int index, i, j, axis, lit, detail, special, step;
    texture = XGL_SurfaceTexture(surface->texinfo->texture, entity);
    if (!texture || !texture->width || !texture->height) return;
    special = (surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) != 0;
    cache = NULL; mesh = NULL;
    if (xgl_surfaces && model->surfaces == xgl_cached_world->surfaces) {
        index = (int)(surface - xgl_cached_world->surfaces);
        if (index >= 0 && index < xgl_surface_count) cache = &xgl_surfaces[index];
    } else cache = XGL_BrushModelFace(model, surface);
    /* Baked light is already in the vertex colors. Native illumination is
     * only worth its cost where a dynamic light can change the result: on
     * the walls XGL_MarkLitSurfaces chose and the brush entities in reach. */
    if (r_fullbright.value || special) lit = 0;
    else if (entity && entity != &r_worldentity) lit = xgl_entity_lit;
    else lit = xgl_light_all || (xgl_light_count &&
        (!xgl_world_order || !cache || cache->light_frame == r_framecount));
    if (cache) {
        if (!cache->coarse_attempted) {
            cache->coarse_attempted = 1;
            XGL_BuildSurfaceMesh(model, surface, &cache->coarse, 0, -1);
        }
        mesh = &cache->coarse;
        if (entity == &r_worldentity && xgl_world_order) {
            if (cache->detail_frame == r_framecount && cache->detail.vertices)
                mesh = &cache->detail;
        } else if (!special) {
            /* Brush entities are not in the world's ranking: theirs take
             * what the walls left of the allowance, nearest in entity order. */
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
            step = -1;
            allowance = xgl_detailtris.value;
            if (detail) {
                cache->detail_want = XGL_DetailWant(cache->area, distance);
                /* One cut along shapes is taken however small, as a wall's
                 * faces are, and may run as far over the allowance. */
                step = XGL_SurfaceShapeStep(surface, distance);
                if (step > 0) allowance *= XGL_SHAPE_OVER;
                else if (cache->detail_want < XGL_DETAIL_WANT_MIN) detail = 0;
            }
            if (detail && xgl_detail_used < allowance)
                XGL_TryBuildDetail(model, surface, cache, cache->detail_want, step);
            if (detail && cache->detail.vertices &&
                xgl_detail_used + cache->detail.count / 3 <= allowance) {
                mesh = &cache->detail;
                xgl_detail_used += mesh->count / 3;
            }
        }
        if (mesh == &cache->detail) {
            cache->detail_used = r_framecount;
            XGL_ProfileCount(XGL_PROFILE_WORLD_DETAIL_SURFACES, 1);
            XGL_ProfileCount(XGL_PROFILE_WORLD_DETAIL_TRIANGLES, (unsigned long)(mesh->count / 3));
            if (cache->shape_step > 0) {
                XGL_ProfileCount(XGL_PROFILE_WORLD_SHAPE_SURFACES, 1);
                XGL_ProfileCount(XGL_PROFILE_WORLD_SHAPE_TRIANGLES, (unsigned long)(mesh->count / 3));
            }
        }
        if (mesh->vertices) {
            XGL_ProfileCount(XGL_PROFILE_WORLD_CACHED_SURFACES, 1);
            XGL_UpdateSurfaceMesh(mesh, surface, texture, entity, mesh == &cache->detail);
            XGL_EmitSurfaceVertices(mesh, surface, entity, lit, opacity, 1);
            return;
        }
    }
    /* Allocation/budget exhaustion never discards a face. Stream its whole
     * polygon as a fan without persistent allocation, even for external
     * bmodels. */
    if (surface->numedges < 3) return;
    XGL_ProfileCount(XGL_PROFILE_WORLD_FALLBACK_SURFACES, 1);
    XGL_SurfacePoint(model, surface, 0, a);
    memset(&fallback, 0, sizeof(fallback));
    fallback.vertices = triangle; fallback.data = triangle_data; fallback.corner = corners;
    fallback.count = fallback.unique = 3;
    fallback.queued_frame = -1;
    for (i = 1; i + 1 < surface->numedges; ++i) {
        if ((i & 15) == 1) XGL_AudioUpdate();
        XGL_SurfacePoint(model, surface, i, b);
        XGL_SurfacePoint(model, surface, i + 1, c);
        XGL_InitCachedVertex(triangle, triangle_data, surface, a);
        XGL_InitCachedVertex(triangle + 1, triangle_data + 1, surface, b);
        XGL_InitCachedVertex(triangle + 2, triangle_data + 2, surface, c);
        spacing = 0;
        for (j = 0; j < 3; ++j) {
            ds = triangle_data[j].st[0] - triangle_data[(j + 1) % 3].st[0];
            dt = triangle_data[j].st[1] - triangle_data[(j + 1) % 3].st[1];
            spacing += (float)sqrt(ds * ds + dt * dt) * (1.0f / 3);
        }
        for (j = 0; j < 3; ++j) triangle_data[j].spacing = spacing;
        fallback.texture = -1; fallback.fullbright = -1;
        for (axis = 0; axis < MAXLIGHTMAPS; ++axis) fallback.style[axis] = -1;
        XGL_UpdateSurfaceMesh(&fallback, surface, texture, entity, 0);
        XGL_EmitSurfaceVertices(&fallback, surface, entity, lit, opacity, 0);
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

/* Finds the visible faces and draws walls. While a dynamic light reaches
 * only some of them, those are drawn here, illuminated, and the rest by
 * XGL_DrawWorldUnlit, so that each native state is entered once. Sky and
 * water wait for XGL_DrawWorldSpecial. */
void XGL_DrawWorld (void)
{
    msurface_t *surface;
    int i;
    double start;
    if (XGL_BuildSettingsChanged()) XGL_WorldSetup();
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
    }
    XGL_MarkLitSurfaces();
    if (xgl_world_order) {
        for (i = 0; i < xgl_world_count; ++i) {
            surface = cl.worldmodel->surfaces + xgl_world_order[i];
            if (surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) continue;
            if (xgl_light_split && xgl_surfaces[xgl_world_order[i]].light_frame != r_framecount)
                continue;
            XGL_QueueSurface(cl.worldmodel, surface, &r_worldentity);
        }
    }
}

/* The walls no dynamic light reaches, when XGL_DrawWorld drew only the others. */
void XGL_DrawWorldUnlit (void)
{
    msurface_t *surface;
    int i;
    if (!xgl_light_split || !xgl_world_order) return;
    currententity = &r_worldentity;
    for (i = 0; i < xgl_world_count; ++i) {
        surface = cl.worldmodel->surfaces + xgl_world_order[i];
        if (surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) continue;
        if (xgl_surfaces[xgl_world_order[i]].light_frame == r_framecount) continue;
        XGL_QueueSurface(cl.worldmodel, surface, &r_worldentity);
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
    xgl_entity_lit = XGL_BrushEntityLights(entity) != 0;
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
