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
 * nothing to add still spends some of the frame's allowance. */
#define XGL_DETAIL_LEVELS 8
#define XGL_DETAIL_FILTER .75f
#define XGL_DETAIL_FORCED 1000000.0f
#define XGL_DETAIL_ATTEMPT 32
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
    /* Mean length, in texels, of the mesh edges that meet at the vertex. */
    float spacing;
    byte material[3];
    byte samples[MAXLIGHTMAPS];
} xgl_cached_vertex_t;

/* One allocation holds three native vertices per triangle, which the backend
 * borrows, then one record per distinct vertex, then each corner's distinct
 * vertex. Colors are computed once per distinct vertex and copied out. */
typedef struct {
    xgl_native_vertex_t *vertices;
    xgl_cached_vertex_t *data;
    unsigned short *corner;
    size_t bytes;
    int count, unique, texture, style[MAXLIGHTMAPS], fullbright, queued_frame;
    unsigned int texture_revision, color_revision;
} xgl_surface_mesh_t;

typedef struct {
    xgl_surface_mesh_t coarse, detail;
    /* detail_retry: frame at which a build that found no memory may be tried
     * again; zero when the last failure was permanent or there was none. */
    int coarse_attempted, detail_attempted, detail_retry;
    vec3_t center;
    /* detail_frame: frame in which the world pass selected the detail mesh.
     * detail_used: last frame the detail mesh was built or drawn.
     * light_frame: frame in which a dynamic light reached this wall. */
    int center_valid, detail_frame, detail_used, bucket, light_frame;
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
static int *xgl_world_order, *xgl_detail_order, *xgl_detail_sorted, xgl_world_count;
static model_t *xgl_pvs_world;
static mleaf_t *xgl_pvs_leaf;
static int xgl_pvs_novis;
/* The settings the cached meshes were built with. */
static float xgl_cache_step, xgl_cache_error, xgl_cache_face, xgl_cache_edge;
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
    xgl_light_count = xgl_light_all = xgl_light_split = xgl_entity_lit = 0;
    /* Textures belong to the map: a new one may reuse an address or an id. */
    memset(&xgl_sky, 0, sizeof(xgl_sky));
    memset(xgl_tones, 0, sizeof(xgl_tones));
    xgl_tone_next = 0;
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
     * the vertex would show, 0..255: material times baked light. */
    float spacing, color[3];
    byte samples[MAXLIGHTMAPS];
} xgl_build_vertex_t;

typedef struct {
    unsigned int key;
    unsigned short a, b;
    /* uses: triangles of the current level that have this edge. vertex: its
     * midpoint once it has been split. tested: evaluated; if splittable,
     * mid then holds the sample a midpoint vertex would have. */
    short uses, vertex;
    byte tested, splittable;
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
    /* Edges no longer than this, in texels, are never split. */
    float minimum;
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
    edge->tested = edge->splittable = 0;
    edge->length = edge->error = 0;
    xgl_build.slots[slot] = (unsigned short)++xgl_build.edge_count;
    return xgl_build.edge_count - 1;
}

/* The largest channel difference between a sample and the color found
 * `weight` of the way from a to b by interpolation. */
static float XGL_BuildError (const float *color, const float *a, const float *b, float weight)
{
    float error, difference;
    int i;
    error = 0;
    for (i = 0; i < 3; ++i) {
        difference = color[i] - (a[i] + (b[i] - a[i]) * weight);
        if (difference < 0) difference = -difference;
        if (difference > error) error = difference;
    }
    return error;
}

/* Decide whether a vertex at the middle of an edge would show something
 * that interpolating between its ends does not. Edges longer than `step`
 * texels are always split. */
static void XGL_BuildTestEdge (msurface_t *surface, texture_t *texture,
                               xgl_build_edge_t *edge, float step, float threshold)
{
    const xgl_build_vertex_t *a, *b;
    xgl_build_vertex_t quarter;
    float ds, dt, error, other, weight;
    int i, k;
    memset(&quarter, 0, sizeof(quarter));
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
    error = XGL_BuildError(edge->mid.color, a->color, b->color, .5f);
    /* The middle can agree by chance where the quarters do not. Look there
     * only when the halves could be split again to show it. */
    if (error <= threshold && edge->length > 2 * xgl_build.minimum) {
        for (k = 0; k < 2; ++k) {
            weight = k ? .75f : .25f;
            quarter.st[0] = a->st[0] + (b->st[0] - a->st[0]) * weight;
            quarter.st[1] = a->st[1] + (b->st[1] - a->st[1]) * weight;
            XGL_BuildSample(surface, texture, &quarter, edge->length * .25f);
            other = XGL_BuildError(quarter.color, a->color, b->color, weight);
            if (other > error) error = other;
        }
    }
    if (error > threshold) edge->error = error;
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
    float error, worst, difference;
    int i, j, index, count, marked, applied, total, longest, a, b, c, ab, bc, ca, swap, n;
    list = xgl_build.triangles[xgl_build.current];
    tested = xgl_build.tested[xgl_build.current];
    count = xgl_build.triangle_count;
    for (i = 0; i < xgl_build.edge_count; ++i) xgl_build.edges[i].uses = 0;
    for (i = 0; i < count; ++i)
        for (j = 0; j < 3; ++j) {
            index = XGL_BuildEdge(list[i][j], list[i][(j + 1) % 3]);
            if (index < 0) return 0;
            xgl_build.triangle_edges[i][j] = (unsigned short)index;
            ++xgl_build.edges[index].uses;
        }
    for (i = 0; i < xgl_build.edge_count; ++i) {
        edge = &xgl_build.edges[i];
        if ((i & 15) == 0) XGL_AudioUpdate();
        if (edge->uses && !edge->tested) XGL_BuildTestEdge(surface, texture, edge, step, threshold);
    }
    for (i = 0; i < count; ++i) {
        worst = 0; longest = 0;
        for (j = 0; j < 3; ++j) {
            sides[j] = &xgl_build.edges[xgl_build.triangle_edges[i][j]];
            if (sides[j]->error > worst) worst = sides[j]->error;
            if (sides[j]->length > sides[longest]->length) longest = j;
        }
        /* Something inside a large triangle may touch none of the points
         * its edges were tested at. */
        if (worst <= 0 && !tested[i] && sides[longest]->length > 2 * xgl_build.minimum) {
            if ((i & 15) == 0) XGL_AudioUpdate();
            for (j = 0; j < 3; ++j) corner[j] = &xgl_build.vertices[list[i][j]];
            for (j = 0; j < 2; ++j)
                centroid.st[j] = (corner[0]->st[j] + corner[1]->st[j] + corner[2]->st[j]) * (1.0f / 3);
            XGL_BuildSample(surface, texture, &centroid, sides[longest]->length * (1.0f / 3));
            error = 0;
            for (j = 0; j < 3; ++j) {
                difference = centroid.color[j] - (corner[0]->color[j] + corner[1]->color[j] +
                                                  corner[2]->color[j]) * (1.0f / 3);
                if (difference < 0) difference = -difference;
                if (difference > error) error = difference;
            }
            if (error > threshold) {
                worst = error;
                for (j = 0; j < 3; ++j)
                    if (sides[j]->splittable && sides[j]->error < error) sides[j]->error = error;
            }
        }
        tested[i] = 1;
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
        if (total + edge->uses > limit || xgl_build.vertex_count == XGL_MESH_VERTICES) continue;
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
    xgl_native_vertex_t *draw;
    xgl_cached_vertex_t *data;
    const unsigned short *triangle;
    size_t bytes;
    float sign;
    int i, j, k, count, unique;
    count = xgl_build.triangle_count * 3; unique = xgl_build.vertex_count;
    bytes = (size_t)count * (sizeof(*mesh->vertices) + sizeof(*mesh->corner)) +
            (size_t)unique * sizeof(*mesh->data);
    bytes = (bytes + 7) & ~(size_t)7;
    if (!XGL_CacheFits(bytes)) {
        XGL_EvictDetail(bytes);
        if (!XGL_CacheFits(bytes)) return -1;
    }
    mesh->vertices = (xgl_native_vertex_t *)malloc(bytes);
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
    sign = (surface->flags & SURF_PLANEBACK) ? -1.0f : 1.0f;
    draw = mesh->vertices;
    for (i = 0; i < xgl_build.triangle_count; ++i) {
        triangle = xgl_build.triangles[xgl_build.current][i];
        for (j = 0; j < 3; ++j, ++draw) {
            mesh->corner[i * 3 + j] = triangle[j];
            VectorCopy(xgl_build.vertices[triangle[j]].position, draw->position);
            for (k = 0; k < 3; ++k) draw->normal[k] = surface->plane->normal[k] * sign;
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

/* Returns 1 when built, 0 when this face can have no such mesh (a detailed
 * one that would be no finer than the coarse one included), and -1 when the
 * cache had no room even after evicting idle detail. */
static int XGL_BuildSurfaceMeshData (model_t *model, msurface_t *surface,
                                xgl_surface_mesh_t *mesh, int detail)
{
    texture_t *texture;
    float grid, step, threshold;
    int level, base, limit;
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
    texture = detail ? XGL_SurfaceTexture(surface->texinfo->texture, NULL) : NULL;
    if (texture && (!texture->width || !texture->height)) texture = NULL;
    if (!XGL_BuildBase(model, surface, texture, detail)) return 0;
    if (detail) {
        step = XGL_Setting(xgl_subdivide.value, 8, 256);
        threshold = XGL_Setting(xgl_detailerror.value, 1, 255);
        limit = (int)XGL_Setting(xgl_detailface.value, 4, XGL_MESH_TRIANGLES);
        xgl_build.minimum = XGL_Setting(xgl_detailedge.value, 4, 64);
        base = xgl_build.triangle_count;
        memset(xgl_build.slots, 0, sizeof(xgl_build.slots));
        for (level = 0; level < XGL_DETAIL_LEVELS; ++level)
            if (!XGL_BuildRefine(surface, texture, step, threshold, limit)) break;
        if (xgl_build.triangle_count == base) return 0;
        XGL_BuildSpacing();
    }
    return XGL_BuildFinish(surface, mesh);
}

static int XGL_BuildSurfaceMesh (model_t *model, msurface_t *surface,
                                xgl_surface_mesh_t *mesh, int detail)
{
    double start;
    int built;
    start = XGL_ProfileDetailNow();
    built = XGL_BuildSurfaceMeshData(model, surface, mesh, detail);
    XGL_ProfileAdd(XGL_PROFILE_WORLD_BUILD, start);
    if (built > 0) {
        XGL_ProfileCount(XGL_PROFILE_WORLD_MESH_BUILDS, 1);
        XGL_ProfileCount(XGL_PROFILE_WORLD_BUILD_VERTICES, (unsigned long)mesh->unique);
    } else if (built < 0 || !detail) XGL_ProfileCount(XGL_PROFILE_WORLD_MESH_FAILURES, 1);
    return built;
}

/* Build a face's detailed mesh if this frame's build allowance permits.
 * A capped or memory-starved build is simply tried again in a later frame;
 * the face draws its coarse mesh meanwhile. A face that has nothing to show
 * in more detail is examined once and never again. */
static void XGL_TryBuildDetail (model_t *model, msurface_t *surface, xgl_surface_cache_t *cache)
{
    int built, built_triangles;
    if (cache->detail.vertices) return;
    if (cache->detail_attempted &&
        !(cache->detail_retry && r_framecount >= cache->detail_retry)) return;
    if (xgl_detailbuild.value > 0 && xgl_detail_built >= xgl_detailbuild.value) return;
    cache->detail_attempted = 1;
    cache->detail_retry = 0;
    built = XGL_BuildSurfaceMesh(model, surface, &cache->detail, 1);
    built_triangles = built > 0 ? cache->detail.count / 3 : 0;
    xgl_detail_built += built_triangles > XGL_DETAIL_ATTEMPT ? built_triangles : XGL_DETAIL_ATTEMPT;
    if (built < 0) cache->detail_retry = r_framecount + XGL_DETAIL_RETRY_FRAMES;
    if (built > 0) cache->detail_used = r_framecount;
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
    xgl_native_vertex_t *draw;
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
 * and gamma changes never touch the texture again. */
static void XGL_SampleMeshMaterials (xgl_surface_mesh_t *mesh, texture_t *texture,
                                     unsigned int texture_revision)
{
    xgl_cached_vertex_t *vertex;
    int i;
    for (i = 0, vertex = mesh->data; i < mesh->unique; ++i, ++vertex) {
        if ((i & 255) == 255) XGL_AudioUpdate();
        vertex->material[0] = vertex->material[1] = vertex->material[2] = 128;
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
    xgl_native_vertex_t *draw;
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
    if (texture_changed) XGL_SampleMeshMaterials(mesh, texture, texture_revision);
    XGL_WorldGamma();
    XGL_ColorMesh(mesh, surface, fullbright);
    mesh->color_revision = color_revision;
    mesh->fullbright = fullbright;
    XGL_ProfileAdd(XGL_PROFILE_WORLD_COLORS, start);
    XGL_ProfileCount(XGL_PROFILE_WORLD_COLOR_UPDATES, 1);
    XGL_ProfileCount(XGL_PROFILE_WORLD_COLOR_VERTICES, (unsigned long)mesh->unique);
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
        if (XGL_BuildSurfaceMesh(xgl_cached_world, surface, &cache->coarse, 0) <= 0) continue;
        XGL_SurfaceBounds(xgl_cached_world, surface, cache);
        if (surface->flags & (SURF_DRAWSKY | SURF_DRAWTURB)) continue;
        texture = XGL_SurfaceTexture(surface->texinfo->texture, NULL);
        if (texture && texture->width && texture->height)
            XGL_SampleMeshMaterials(&cache->coarse, texture,
                                    XGL_TextureRevision(texture->gl_texturenum));
    }
}

/* Cached meshes depend on these settings; a change rebuilds the cache. */
static int XGL_BuildSettingsChanged (void)
{
    return xgl_cache_step != xgl_subdivide.value ||
           xgl_cache_error != xgl_detailerror.value ||
           xgl_cache_face != xgl_detailface.value ||
           xgl_cache_edge != xgl_detailedge.value;
}

void XGL_WorldNewMap (void)
{
    size_t bytes, each;
    XGL_WorldShutdown();
    xgl_cached_world = cl.worldmodel;
    xgl_cache_step = xgl_subdivide.value;
    xgl_cache_error = xgl_detailerror.value;
    xgl_cache_face = xgl_detailface.value;
    xgl_cache_edge = xgl_detailedge.value;
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
            XGL_TryBuildDetail(cl.worldmodel, surface, cache);
            if (cache->detail.vertices)
                cache->detail_frame = cache->detail_used = r_framecount;
            continue;
        }
        if (!(xgl_detailtris.value > 0)) continue;
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
        if (cache->detail.vertices &&
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

static void XGL_DrawSurface (model_t *model, msurface_t *surface, entity_t *entity, float opacity)
{
    static unsigned short corners[3] = {0, 1, 2};
    xgl_surface_cache_t *cache;
    xgl_surface_mesh_t *mesh;
    xgl_surface_mesh_t fallback;
    xgl_native_vertex_t triangle[3];
    xgl_cached_vertex_t triangle_data[3];
    texture_t *texture;
    vec3_t a, b, c, center, forward, right, up, worldcenter, delta;
    float distance, ds, dt, spacing;
    int index, i, j, axis, lit, detail, special;
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
            XGL_BuildSurfaceMesh(model, surface, &cache->coarse, 0);
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
            if (detail && xgl_detail_used < xgl_detailtris.value)
                XGL_TryBuildDetail(model, surface, cache);
            if (detail && cache->detail.vertices &&
                xgl_detail_used + cache->detail.count / 3 <= xgl_detailtris.value) {
                mesh = &cache->detail;
                xgl_detail_used += mesh->count / 3;
            }
        }
        if (mesh == &cache->detail) {
            cache->detail_used = r_framecount;
            XGL_ProfileCount(XGL_PROFILE_WORLD_DETAIL_SURFACES, 1);
            XGL_ProfileCount(XGL_PROFILE_WORLD_DETAIL_TRIANGLES, (unsigned long)(mesh->count / 3));
        }
        if (mesh->vertices) {
            XGL_ProfileCount(XGL_PROFILE_WORLD_CACHED_SURFACES, 1);
            XGL_UpdateSurfaceMesh(mesh, surface, texture, entity, mesh == &cache->detail);
            XGL_EmitSurfaceVertices(mesh, entity, lit, opacity, 1);
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

/* Finds the visible faces and draws walls. While a dynamic light reaches
 * only some of them, those are drawn here, illuminated, and the rest by
 * XGL_DrawWorldUnlit, so that each native state is entered once. Sky and
 * water wait for XGL_DrawWorldSpecial. */
void XGL_DrawWorld (void)
{
    msurface_t *surface;
    int i;
    double start;
    if (XGL_BuildSettingsChanged()) XGL_WorldNewMap();
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
