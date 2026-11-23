/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

/* The GL model loader's indexed skins and pose command lists remain CPU
 * data. XGL receives sampled RGB triangles, never texture objects. Models
 * are shaded here and submitted unlit unless xgl_alias_shade is 0, when
 * world-space normals are output and the device illuminates them. */
#include "quakedef.h"
#include "xgl_local.h"
#include "xgl_alias.h"
#include "xgl_audio.h"
#include "xgl_profile.h"
#include "xgl_smp.h"

#define XGL_VERTEX_NORMALS 162
#define XGL_ALIAS_CACHE_SLOTS 16
#define XGL_ALIAS_CACHE_BYTES (1024UL * 1024UL)
#define XGL_ALIAS_POSE_SLOTS 32
#define XGL_ALIAS_TOPOLOGY_SLOTS 16
#define XGL_ALIAS_POSE_BYTES (1024UL * 1024UL)
#define XGL_ALIAS_POSE_VERTICES 4096
#define XGL_ENTITY_BATCH_VERTICES (256 * 3)
/* The most points in one triangle strip of a model. */
#define XGL_ALIAS_STRIP_POINTS 128

/* Particle effects also use this original Quake normal table. */
float r_avertexnormals[XGL_VERTEX_NORMALS][3] = {
#include "anorms.h"
};

static cvar_t xgl_alias_subdivide = {"xgl_alias_subdivide", "1", true, false, 0, NULL};
static cvar_t xgl_sprite_cells = {"xgl_sprite_cells", "8", true, false, 0, NULL};
static cvar_t xgl_entity_triangles = {"xgl_entity_triangles", "512", true, false, 0, NULL};
static cvar_t xgl_playercolors = {"xgl_playercolors", "1", true, false, 0, NULL};
static cvar_t xgl_alias_shade = {"xgl_alias_shade", "1", false, false, 0, NULL};
/* The fewest triangles a run of a model's triangles must have to be sent as
 * a strip. Strips of eight and more cost the Sun ZX a third less for each
 * triangle than separate triangles; how short a strip still pays is not yet
 * measured there. Below 2, every triangle is sent separately. */
static cvar_t xgl_alias_strips = {"xgl_alias_strips", "4", false, false, 0, NULL};
static byte xgl_player_translation[MAX_SCOREBOARD][256];
static int xgl_player_color[MAX_SCOREBOARD];
static int xgl_entity_extra_used;

typedef struct {
    vec3_t position;
    vec3_t normal;
    float s, t;
} xgl_entity_vertex_t;

typedef struct {
    int index[3];
    int visible;
} xgl_alias_color_t;

typedef struct {
    int material, depth, command_words, triangle_count, translated, ready;
    int visible_triangles;
    /* generation: the loaded model these commands were last matched against;
     * zero when the model carries no identity record. */
    unsigned int revision, used, command_hash, generation;
    byte translation[256];
    int *commands;
    xgl_alias_color_t *colors;
    size_t bytes;
} xgl_alias_cache_t;

/* How one model's vertices are lit. */
#define XGL_SHADE_NATIVE 0 /* normals are output and the device illuminates */
#define XGL_SHADE_FLAT 1   /* sampled colors are final: torches, fullbright */
#define XGL_SHADE_CPU 2    /* ambient plus directional fill, per vertex, here */
typedef struct {
    int mode;
    float ambient;
    /* Toward the fill light, in model space, scaled by its strength. */
    vec3_t fill;
} xgl_alias_shade_t;

typedef struct {
    xgl_alias_cache_t *cache;
    /* NULL for sprites. The fallback's normals are in world space, where the
     * direction toward the fill light is the same along every axis. */
    const xgl_alias_shade_t *shade;
    int index, building, opaque, count;
    xgl_vertex_t vertices[XGL_ENTITY_BATCH_VERTICES];
} xgl_alias_draw_t;

typedef struct {
    vec3_t normals[XGL_VERTEX_NORMALS];
    byte valid[XGL_VERTEX_NORMALS];
} xgl_alias_normals_t;

typedef struct {
    unsigned short a, b;
} xgl_alias_parent_t;

typedef struct {
    int depth, command_words, base_count, vertex_count, triangle_count;
    unsigned int used, command_hash, generation;
    int *commands;
    xgl_alias_parent_t *parents;
    unsigned short *indices;
    /* The same triangles joined into strips, where the commands say which
     * pose vertices are one point of the mesh; none otherwise. strip_lengths
     * has the number of points of each strip, longest first, and
     * strip_vertices their points, one strip after another, as pose
     * vertices. point_corner says, for a pose vertex that is a strip point,
     * which corner of which triangle has its color: four times the
     * triangle, plus the corner. */
    int strip_count;
    unsigned short *strip_lengths, *strip_vertices, *point_corner;
    /* For the fewest triangles last asked of a strip: how many strips have
     * as many, and their points in all. */
    int split_shortest, split_strips, split_points;
    size_t bytes;
} xgl_alias_topology_t;

typedef struct {
    xgl_alias_topology_t *topology;
    vec3_t scale;
    unsigned int used, normal_hash;
    byte *normal_indices;
    vec3_t *normals;
    size_t bytes;
} xgl_alias_pose_cache_t;

typedef struct {
    vec3_t position;
    /* The world-space normal for native illumination or, with CPU shading,
     * the vertex's own light factor. */
    union { vec3_t normal; float shade; } light;
} xgl_alias_work_vertex_t;

/* Alias jobs are collected in visible-list order. Only pose bytes belong to
 * the movable model block. The XGL cache arrays are immutable from submission
 * until the main thread joins the jobs, before it draws any entities. */
#define XGL_ALIAS_FRAME_JOBS 32
#define XGL_ALIAS_FRAME_CORNERS 16380
#define XGL_ALIAS_FRAME_STRIPS 4096
typedef struct {
    int visible_index, lit, triangles, count;
    /* With strips above zero the output begins with that many strips, of
     * strip_points points in all and `lengths` each, and the rest of it is
     * separate triangles. The lengths are the frame's own copy: the caches
     * may change before the output is drawn. */
    int strips, strip_points;
    const unsigned short *lengths;
    float light;
    xgl_alias_shade_t shade;
    vec3_t scale, scale_origin, forward, right, up, origin;
    const trivertx_t *pose;
    const xgl_alias_pose_cache_t *cached_pose;
    const xgl_alias_cache_t *colors;
    const byte *palette;
    const float *gamma;
    xgl_alias_work_vertex_t *work;
    /* Records without normals unless the device is to illuminate them. */
    void *output;
} xgl_alias_job_t;

typedef struct {
    xgl_alias_job_t jobs[XGL_ALIAS_FRAME_JOBS];
    int job_count, pose_count, corner_count, length_count, pending;
    unsigned short lengths[XGL_ALIAS_FRAME_STRIPS];
    byte palette[768];
    float gamma[256];
    trivertx_t poses[XGL_ALIAS_FRAME_CORNERS];
    /* Sized for the larger record; a job without normals uses less of its
     * share. */
    xgl_native_vertex_t output[XGL_ALIAS_FRAME_CORNERS];
    /* The FIFO has one worker. Its temporary transforms can be reused after
     * each job; the assembled output is retained separately for each draw. */
    xgl_alias_work_vertex_t work[XGL_ALIAS_POSE_VERTICES];
} xgl_alias_frame_t;

static xgl_alias_frame_t *xgl_alias_frame;

/* One detail decision per visible entity, made in visible-list order so the
 * shared triangle allowance is spent identically whatever the drawing order
 * and whether or not a worker prepares the models. Negative: not drawn. */
#define XGL_ALIAS_UNPLANNED (-2)
#define XGL_ALIAS_FLAME 4
static signed char xgl_alias_plan[MAX_VISEDICTS];
static int xgl_alias_planned;

static void XGL_RunAliasJob(void *argument);
static void XGL_JoinAliasJobs(void);
static int XGL_EntitySetting(float value, int low, int high);

void XGL_AliasFinishFrame(void)
{
    XGL_JoinAliasJobs();
    xgl_alias_planned = 0;
    if (xgl_alias_frame) {
        xgl_alias_frame->job_count = 0;
        xgl_alias_frame->pose_count = 0;
        xgl_alias_frame->corner_count = 0;
        xgl_alias_frame->length_count = 0;
    }
}

/* Poses share immutable subdivision data and own only their normal data.
 * Positions always come from the live pose. No pointer into Quake's movable
 * model cache survives the draw; all tables and work storage count in the cap. */
static xgl_alias_topology_t xgl_alias_topologies[XGL_ALIAS_TOPOLOGY_SLOTS];
static xgl_alias_pose_cache_t xgl_alias_poses[XGL_ALIAS_POSE_SLOTS];
static xgl_alias_work_vertex_t xgl_alias_work[XGL_ALIAS_POSE_VERTICES];
#define XGL_ALIAS_FIXED_BYTES (sizeof(xgl_alias_topologies) + sizeof(xgl_alias_poses) + sizeof(xgl_alias_work))
static size_t xgl_alias_pose_bytes = XGL_ALIAS_FIXED_BYTES;
static unsigned int xgl_alias_pose_used;

/* Rather than idle, the main thread runs the jobs the worker has not reached,
 * on the serial path's own scratch buffer, then waits for the one in flight. */
static void XGL_JoinAliasJobs(void)
{
    void (*function)(void *);
    void *argument;
    double start;
    unsigned long helped;
    if (!xgl_alias_frame || !xgl_alias_frame->pending) return;
    start = XGL_ProfileDetailNow();
    helped = 0;
    while (XGL_SMPTake(&function, &argument)) {
        if (function == XGL_RunAliasJob)
            ((xgl_alias_job_t *)argument)->work = xgl_alias_work;
        function(argument);
        ++helped;
    }
    if (helped) {
        XGL_ProfileAdd(XGL_PROFILE_SMP_WAIT, start);
        XGL_ProfileCount(XGL_PROFILE_SMP_HELPED, helped);
    }
    XGL_SMPWait();
    xgl_alias_frame->pending = 0;
}

static void XGL_FreeAliasPose(xgl_alias_pose_cache_t *cache)
{
    free(cache->normal_indices);
    free(cache->normals);
    xgl_alias_pose_bytes -= cache->bytes;
    memset(cache, 0, sizeof(*cache));
}

static void XGL_EvictAliasPose(xgl_alias_pose_cache_t *cache,
                              xgl_profile_counter_t reason)
{
    XGL_ProfileCount(XGL_PROFILE_ALIAS_POSE_EVICTIONS, 1);
    XGL_ProfileCount(reason, 1);
    XGL_FreeAliasPose(cache);
}

static void XGL_FreeAliasTopology(xgl_alias_topology_t *topology,
                                  xgl_profile_counter_t reason)
{
    int i;
    for (i = 0; i < XGL_ALIAS_POSE_SLOTS; ++i)
        if (xgl_alias_poses[i].topology == topology)
            XGL_EvictAliasPose(&xgl_alias_poses[i], reason);
    free(topology->commands);
    free(topology->parents);
    free(topology->indices);
    free(topology->strip_lengths);
    free(topology->strip_vertices);
    free(topology->point_corner);
    xgl_alias_pose_bytes -= topology->bytes;
    memset(topology, 0, sizeof(*topology));
}

/* Keep only owned command bytes and sampled colors, never pointers into the
 * engine's movable alias cache. The cap includes the fixed entry table. */
static xgl_alias_cache_t xgl_alias_cache[XGL_ALIAS_CACHE_SLOTS];
static size_t xgl_alias_cache_bytes = sizeof(xgl_alias_cache);
static unsigned int xgl_alias_cache_used;

static void XGL_FreeAliasColors(xgl_alias_cache_t *cache)
{
    free(cache->commands);
    free(cache->colors);
    xgl_alias_cache_bytes -= cache->bytes;
    memset(cache, 0, sizeof(*cache));
}

void XGL_EntitiesShutdown(void)
{
    int i;
    XGL_AliasFinishFrame();
    free(xgl_alias_frame);
    xgl_alias_frame = NULL;
    for (i = 0; i < XGL_ALIAS_CACHE_SLOTS; ++i)
        XGL_FreeAliasColors(&xgl_alias_cache[i]);
    xgl_alias_cache_used = 0;
    for (i = 0; i < XGL_ALIAS_POSE_SLOTS; ++i)
        XGL_FreeAliasPose(&xgl_alias_poses[i]);
    for (i = 0; i < XGL_ALIAS_TOPOLOGY_SLOTS; ++i)
        XGL_FreeAliasTopology(&xgl_alias_topologies[i], XGL_PROFILE_ALIAS_POSE_SLOT_EVICTIONS);
    xgl_alias_pose_used = 0;
}

static int XGL_OldestAliasColors(void)
{
    int i, oldest;
    oldest = -1;
    for (i = 0; i < XGL_ALIAS_CACHE_SLOTS; ++i) {
        if (!xgl_alias_cache[i].colors) continue;
        if (oldest < 0 || xgl_alias_cache[i].used < xgl_alias_cache[oldest].used)
            oldest = i;
    }
    return oldest;
}

static xgl_alias_cache_t *XGL_FindAliasColors(const aliashdr_t *header,
                                       int material, int depth,
                                       const byte *translation, int create)
{
    const int *commands, *cursor;
    const xgl_alias_identity_t *identity;
    xgl_alias_cache_t *cache;
    unsigned int revision;
    size_t bytes;
    int count, consumed, triangles, words, i, slot;
    revision = XGL_TextureRevision(material);
    if (!revision || depth < 0 || depth > 2 || header->poseverts < 3 ||
        header->poseverts > MAXALIASTRIS * 3 ||
        header->numtris < 1 || header->numtris > MAXALIASTRIS) return NULL;
    commands = (const int *)((const byte *)header + header->commands);
    identity = XGL_AliasIdentity(header);
    if (identity) {
        /* The loader generated and measured these commands itself. */
        words = identity->words;
        triangles = header->numtris;
    } else {
        cursor = commands;
        consumed = triangles = 0;
        while ((count = *cursor++) != 0) {
            if (count > header->poseverts - consumed || count < -(header->poseverts - consumed))
                return NULL;
            if (count < 0) count = -count;
            if (count < 3) return NULL;
            consumed += count; triangles += count - 2;
            cursor += count * 2;
        }
        if (triangles != header->numtris) return NULL;
        words = (int)(cursor - commands);
    }
    triangles *= 1 << (2 * depth);
    if (++xgl_alias_cache_used == 0) {
        for (i = 0; i < XGL_ALIAS_CACHE_SLOTS; ++i) xgl_alias_cache[i].used = 0;
        xgl_alias_cache_used = 1;
    }
    slot = -1;
    for (i = 0; i < XGL_ALIAS_CACHE_SLOTS; ++i) {
        cache = &xgl_alias_cache[i];
        if (!cache->colors) { if (slot < 0) slot = i; continue; }
        if (cache->material != material || cache->revision != revision ||
            cache->depth != depth || cache->command_words != words ||
            cache->translated != (translation != NULL) ||
            cache->triangle_count != triangles) continue;
        if (translation && memcmp(cache->translation, translation, 256)) continue;
        /* The same loaded model needs no comparison. Another load of equal
         * commands (a reloaded or duplicate model) is compared once, exactly,
         * and then remembered by its generation. */
        if (!identity || cache->generation != identity->generation) {
            if (identity && cache->command_hash != identity->hash) continue;
            if (memcmp(cache->commands, commands, (size_t)words * sizeof(int))) continue;
            cache->generation = identity ? identity->generation : 0;
        }
        cache->used = xgl_alias_cache_used;
        if (create) XGL_ProfileCount(XGL_PROFILE_ALIAS_COLOR_HITS, 1);
        return cache;
    }
    if (!create) return NULL;
    XGL_ProfileCount(XGL_PROFILE_ALIAS_COLOR_MISSES, 1);
    bytes = (size_t)words * sizeof(int) + (size_t)triangles * sizeof(xgl_alias_color_t);
    if (bytes > XGL_ALIAS_CACHE_BYTES - sizeof(xgl_alias_cache)) return NULL;
    if (slot < 0) slot = XGL_OldestAliasColors();
    if (slot < 0) return NULL;
    if (xgl_alias_cache[slot].colors)
        XGL_ProfileCount(XGL_PROFILE_ALIAS_COLOR_EVICTIONS, 1);
    XGL_FreeAliasColors(&xgl_alias_cache[slot]);
    while (bytes > XGL_ALIAS_CACHE_BYTES - xgl_alias_cache_bytes) {
        i = XGL_OldestAliasColors();
        if (i < 0) return NULL;
        XGL_ProfileCount(XGL_PROFILE_ALIAS_COLOR_EVICTIONS, 1);
        XGL_FreeAliasColors(&xgl_alias_cache[i]);
    }
    cache = &xgl_alias_cache[slot];
    cache->commands = (int *)malloc((size_t)words * sizeof(int));
    cache->colors = (xgl_alias_color_t *)malloc((size_t)triangles * sizeof(xgl_alias_color_t));
    if (!cache->commands || !cache->colors) {
        XGL_FreeAliasColors(cache);
        return NULL;
    }
    memcpy(cache->commands, commands, (size_t)words * sizeof(int));
    cache->command_hash = identity ? identity->hash : XGL_AliasCommandHash(commands, words);
    cache->generation = identity ? identity->generation : 0;
    if (translation) memcpy(cache->translation, translation, 256);
    cache->translated = translation != NULL;
    cache->material = material; cache->revision = revision; cache->depth = depth;
    cache->command_words = words; cache->triangle_count = triangles;
    cache->used = xgl_alias_cache_used; cache->bytes = bytes;
    xgl_alias_cache_bytes += bytes;
    return cache;
}

static xgl_alias_cache_t *XGL_AliasColors(const aliashdr_t *header, int material,
                                         int depth, const byte *translation)
{
    return XGL_FindAliasColors(header, material, depth, translation, 1);
}

/* The parent and triangle order exactly follows the recursive fallback.
 * Build it once for each command stream/detail level, independent of poses. */
static int XGL_PoseMidpoint(xgl_alias_topology_t *topology, int a, int b,
                           int *vertex_count)
{
    int index;
    index = (*vertex_count)++;
    topology->parents[index].a = (unsigned short)a;
    topology->parents[index].b = (unsigned short)b;
    return index;
}

static void XGL_PoseTriangle(xgl_alias_topology_t *topology, int a, int b, int c,
                            int depth, int *vertex_count, int *index_count)
{
    int ab, bc, ca;
    if (depth) {
        ab = XGL_PoseMidpoint(topology, a, b, vertex_count);
        bc = XGL_PoseMidpoint(topology, b, c, vertex_count);
        ca = XGL_PoseMidpoint(topology, c, a, vertex_count);
        XGL_PoseTriangle(topology, a, ab, ca, depth - 1, vertex_count, index_count);
        XGL_PoseTriangle(topology, ab, b, bc, depth - 1, vertex_count, index_count);
        XGL_PoseTriangle(topology, ca, bc, c, depth - 1, vertex_count, index_count);
        XGL_PoseTriangle(topology, ab, bc, ca, depth - 1, vertex_count, index_count);
    } else {
        topology->indices[(*index_count)++] = (unsigned short)a;
        topology->indices[(*index_count)++] = (unsigned short)b;
        topology->indices[(*index_count)++] = (unsigned short)c;
    }
}

static int XGL_OldestAliasPose(void)
{
    int i, oldest;
    oldest = -1;
    for (i = 0; i < XGL_ALIAS_POSE_SLOTS; ++i) {
        if (!xgl_alias_poses[i].normals) continue;
        if (oldest < 0 || xgl_alias_poses[i].used < xgl_alias_poses[oldest].used)
            oldest = i;
    }
    return oldest;
}

static int XGL_OldestAliasTopology(const xgl_alias_topology_t *keep)
{
    int i, oldest;
    oldest = -1;
    for (i = 0; i < XGL_ALIAS_TOPOLOGY_SLOTS; ++i) {
        if (!xgl_alias_topologies[i].indices || &xgl_alias_topologies[i] == keep) continue;
        if (oldest < 0 || xgl_alias_topologies[i].used < xgl_alias_topologies[oldest].used)
            oldest = i;
    }
    return oldest;
}

/* Evict old poses first, then unused topologies. The topology being used by
 * this draw remains pinned while allocating its new normal data. */
static int XGL_AliasPoseRoom(size_t bytes, const xgl_alias_topology_t *keep)
{
    int i;
    if (bytes > XGL_ALIAS_POSE_BYTES - XGL_ALIAS_FIXED_BYTES - (keep ? keep->bytes : 0))
        return 0;
    while (bytes > XGL_ALIAS_POSE_BYTES - xgl_alias_pose_bytes) {
        i = XGL_OldestAliasPose();
        if (i >= 0) {
            XGL_EvictAliasPose(&xgl_alias_poses[i], XGL_PROFILE_ALIAS_POSE_BYTE_EVICTIONS);
        } else {
            i = XGL_OldestAliasTopology(keep);
            if (i < 0) return 0;
            XGL_FreeAliasTopology(&xgl_alias_topologies[i], XGL_PROFILE_ALIAS_POSE_BYTE_EVICTIONS);
        }
    }
    return 1;
}

/* Join a topology's triangles into strips, once, when it is made. Two
 * corners are the same point of a strip when they are the same point of the
 * mesh: pose vertices that `welds` says are one, or middles of edges between
 * the same two points. Such corners are at one position, have one normal and
 * one place on the skin, so one color, whatever the pose and the skin.
 *
 * Each triangle in turn, unless a strip already has it, begins a strip that
 * is carried through its neighbors both ways until none is free; of the
 * three ways the triangle can lie in a strip, the longest is kept. The
 * device does not cull, so a strip may turn either way. Without memory, or
 * with welds that make no sense, the topology is left without strips. */
static void XGL_BuildAliasStrips(xgl_alias_topology_t *topology, const int *welds)
{
    unsigned short strip[XGL_ALIAS_STRIP_POINTS], best[XGL_ALIAS_STRIP_POINTS];
    unsigned short backward[XGL_ALIAS_STRIP_POINTS];
    short members[XGL_ALIAS_STRIP_POINTS], best_members[XGL_ALIAS_STRIP_POINTS];
    int counts[XGL_ALIAS_STRIP_POINTS + 1];
    void *memory;
    int *edge_slots;
    unsigned short *canon, *middle_slots, *points, *lengths, *corner;
    short (*neighbor)[3];
    byte *used;
    size_t bytes, extra;
    unsigned int hash;
    int triangles, vertices, base, edge_size, middle_size, i, j, k, e, t, n, a, b, x, y, z;
    int low, high, other, other_edge, slot, seed, way, best_n, forward, back, total, strips;
    int most, length, offset;
    triangles = topology->triangle_count; vertices = topology->vertex_count;
    base = topology->base_count;
    if (triangles < 2 || triangles > 32767 || vertices > 65535) return;
    for (edge_size = 16; edge_size < 6 * triangles; edge_size <<= 1) {}
    for (middle_size = 16; middle_size < 2 * (vertices - base); middle_size <<= 1) {}
    bytes = (size_t)edge_size * sizeof(*edge_slots) +
            ((size_t)vertices + middle_size + 3 * (size_t)triangles + triangles) * sizeof(unsigned short) +
            (size_t)triangles * sizeof(*neighbor) + (size_t)triangles;
    memory = malloc(bytes);
    if (!memory) return;
    memset(memory, 0, bytes);
    edge_slots = (int *)memory;
    canon = (unsigned short *)(edge_slots + edge_size);
    middle_slots = canon + vertices;
    points = middle_slots + middle_size;
    lengths = points + 3 * triangles;
    neighbor = (short (*)[3])(lengths + triangles);
    used = (byte *)(neighbor + triangles);
    /* Which vertex stands for each: the first that is the same point. */
    for (i = 0; i < base; ++i) {
        if (welds[i] < 0 || welds[i] > i || welds[welds[i]] != welds[i]) { free(memory); return; }
        canon[i] = (unsigned short)welds[i];
    }
    for (; i < vertices; ++i) {
        a = canon[topology->parents[i].a]; b = canon[topology->parents[i].b];
        low = a < b ? a : b; high = a < b ? b : a;
        hash = ((unsigned int)low * 2654435761U ^ (unsigned int)high * 40503U) & (unsigned int)(middle_size - 1);
        for (;; hash = (hash + 1) & (unsigned int)(middle_size - 1)) {
            if (!middle_slots[hash]) {
                middle_slots[hash] = (unsigned short)(i - base + 1);
                canon[i] = (unsigned short)i;
                break;
            }
            j = middle_slots[hash] - 1 + base;
            a = canon[topology->parents[j].a]; b = canon[topology->parents[j].b];
            if ((a < b ? a : b) == low && (a < b ? b : a) == high) { canon[i] = (unsigned short)j; break; }
        }
    }
    /* Which triangle lies across each edge of each. An edge with more than
     * two triangles on it joins the first two; a triangle with two corners
     * at one point joins nothing. */
    for (t = 0; t < triangles; ++t) neighbor[t][0] = neighbor[t][1] = neighbor[t][2] = -1;
    for (t = 0; t < triangles; ++t) {
        corner = topology->indices + 3 * t;
        if (canon[corner[0]] == canon[corner[1]] || canon[corner[1]] == canon[corner[2]] ||
            canon[corner[2]] == canon[corner[0]]) continue;
        for (e = 0; e < 3; ++e) {
            a = canon[corner[e]]; b = canon[corner[e == 2 ? 0 : e + 1]];
            low = a < b ? a : b; high = a < b ? b : a;
            hash = ((unsigned int)low * 2654435761U ^ (unsigned int)high * 40503U) & (unsigned int)(edge_size - 1);
            for (;; hash = (hash + 1) & (unsigned int)(edge_size - 1)) {
                slot = edge_slots[hash];
                if (!slot) { edge_slots[hash] = 3 * t + e + 1; break; }
                other = (slot - 1) / 3; other_edge = (slot - 1) % 3;
                a = canon[topology->indices[3 * other + other_edge]];
                b = canon[topology->indices[3 * other + (other_edge == 2 ? 0 : other_edge + 1)]];
                if ((a < b ? a : b) != low || (a < b ? b : a) != high) continue;
                if (neighbor[other][other_edge] < 0 && other != t) {
                    neighbor[other][other_edge] = (short)t;
                    neighbor[t][e] = (short)other;
                }
                break;
            }
        }
    }
    total = strips = 0;
    for (seed = 0; seed < triangles; ++seed) {
        if (used[seed]) continue;
        corner = topology->indices + 3 * seed;
        best_n = 0;
        for (way = 0; way < 3; ++way) {
            /* The seed's corner `way` is the middle one of its three
             * points; the strip leaves through the edges either side. */
            strip[0] = canon[corner[way == 0 ? 2 : way - 1]];
            strip[1] = canon[corner[way]];
            strip[2] = canon[corner[way == 2 ? 0 : way + 1]];
            members[0] = (short)seed; used[seed] = 2;
            n = 1; forward = 3; back = 0;
            for (k = 0; k < 2; ++k) {
                /* Onward from the last triangle, then back from the first. */
                t = seed;
                x = k ? strip[1] : strip[1]; y = k ? strip[0] : strip[2];
                while (n < XGL_ALIAS_STRIP_POINTS - 2) {
                    const unsigned short *at = topology->indices + 3 * t;
                    for (e = 0; e < 3; ++e) {
                        a = canon[at[e]]; b = canon[at[e == 2 ? 0 : e + 1]];
                        if ((a == x && b == y) || (a == y && b == x)) break;
                    }
                    if (e == 3 || neighbor[t][e] < 0 || used[neighbor[t][e]]) break;
                    t = neighbor[t][e];
                    at = topology->indices + 3 * t;
                    z = canon[at[0]];
                    if (z == x || z == y) z = canon[at[1]];
                    if (z == x || z == y) z = canon[at[2]];
                    if (k) backward[back++] = (unsigned short)z;
                    else strip[forward++] = (unsigned short)z;
                    members[n++] = (short)t; used[t] = 2;
                    x = y; y = z;
                }
            }
            if (n > best_n) {
                best_n = n;
                for (i = 0; i < back; ++i) best[i] = backward[back - 1 - i];
                memcpy(best + back, strip, (size_t)forward * sizeof(strip[0]));
                memcpy(best_members, members, (size_t)n * sizeof(members[0]));
            }
            for (i = 0; i < n; ++i) used[members[i]] = 0;
        }
        for (i = 0; i < best_n; ++i) used[best_members[i]] = 1;
        memcpy(points + total, best, (size_t)(best_n + 2) * sizeof(best[0]));
        lengths[strips++] = (unsigned short)(best_n + 2);
        total += best_n + 2;
    }
    /* Every triangle is in exactly one strip. */
    if (total - 2 * strips != triangles) { free(memory); return; }
    extra = ((size_t)strips + total + vertices) * sizeof(unsigned short);
    if (!XGL_AliasPoseRoom(extra, topology)) { free(memory); return; }
    topology->strip_lengths = (unsigned short *)malloc((size_t)strips * sizeof(unsigned short));
    topology->strip_vertices = (unsigned short *)malloc((size_t)total * sizeof(unsigned short));
    topology->point_corner = (unsigned short *)malloc((size_t)vertices * sizeof(unsigned short));
    if (!topology->strip_lengths || !topology->strip_vertices || !topology->point_corner) {
        free(topology->strip_lengths); free(topology->strip_vertices); free(topology->point_corner);
        topology->strip_lengths = topology->strip_vertices = topology->point_corner = NULL;
        free(memory);
        return;
    }
    /* Longest first: those long enough to be sent as strips then come
     * before the rest, whatever length is asked. */
    memset(counts, 0, sizeof(counts));
    for (i = 0; i < strips; ++i) ++counts[lengths[i]];
    most = offset = 0;
    for (length = XGL_ALIAS_STRIP_POINTS; length >= 3; --length) {
        n = counts[length];
        counts[length] = most;
        most += n;
    }
    /* counts[length] is now the place of the next strip of that length;
     * where its points go follows from the strips before it. */
    for (i = 0; i < strips; ++i) topology->strip_lengths[counts[lengths[i]]++] = lengths[i];
    for (length = XGL_ALIAS_STRIP_POINTS; length >= 3; --length) counts[length] = 0;
    for (i = 0; i < strips; ++i) {
        /* The points before the first strip of each length. */
        if (!i || topology->strip_lengths[i] != topology->strip_lengths[i - 1])
            counts[topology->strip_lengths[i]] = offset;
        offset += topology->strip_lengths[i];
    }
    offset = 0;
    for (i = 0; i < strips; ++i) {
        memcpy(topology->strip_vertices + counts[lengths[i]], points + offset,
               (size_t)lengths[i] * sizeof(unsigned short));
        counts[lengths[i]] += lengths[i];
        offset += lengths[i];
    }
    memset(topology->point_corner, 0xff, (size_t)vertices * sizeof(unsigned short));
    for (t = 0; t < triangles; ++t)
        for (j = 0; j < 3; ++j) {
            i = canon[topology->indices[3 * t + j]];
            if (topology->point_corner[i] == 0xffff) topology->point_corner[i] = (unsigned short)(4 * t + j);
        }
    topology->strip_count = strips;
    topology->split_shortest = -1;
    topology->bytes += extra;
    xgl_alias_pose_bytes += extra;
    free(memory);
}

/* How many of a topology's strips have at least `shortest` triangles, and
 * how many points those have. */
static void XGL_AliasStripSplit(xgl_alias_topology_t *topology, int shortest)
{
    int strips, points;
    if (topology->split_shortest == shortest) return;
    strips = points = 0;
    while (strips < topology->strip_count && topology->strip_lengths[strips] - 2 >= shortest)
        points += topology->strip_lengths[strips++];
    topology->split_shortest = shortest;
    topology->split_strips = strips;
    topology->split_points = points;
}

static xgl_alias_topology_t *XGL_AliasTopology(const aliashdr_t *header,
                                               const xgl_alias_cache_t *colors, int create)
{
    xgl_alias_topology_t *topology;
    const int *order;
    size_t bytes;
    int vertices, slot, i, index, count, consumed, fan, a, b;
    int vertex_count, index_count;
    vertices = header->poseverts + header->numtris * ((1 << (2 * colors->depth)) - 1);
    if (vertices > XGL_ALIAS_POSE_VERTICES) return NULL;
    slot = -1;
    for (i = 0; i < XGL_ALIAS_TOPOLOGY_SLOTS; ++i) {
        topology = &xgl_alias_topologies[i];
        if (!topology->indices) { if (slot < 0) slot = i; continue; }
        if (topology->depth != colors->depth || topology->command_hash != colors->command_hash ||
            topology->command_words != colors->command_words || topology->base_count != header->poseverts ||
            topology->vertex_count != vertices || topology->triangle_count != colors->triangle_count)
            continue;
        if (!colors->generation || topology->generation != colors->generation) {
            if (memcmp(topology->commands, colors->commands,
                       (size_t)colors->command_words * sizeof(int))) continue;
            topology->generation = colors->generation;
        }
        topology->used = xgl_alias_pose_used;
        if (create) XGL_ProfileCount(XGL_PROFILE_ALIAS_TOPOLOGY_HITS, 1);
        return topology;
    }
    if (!create) return NULL;
    XGL_ProfileCount(XGL_PROFILE_ALIAS_TOPOLOGY_MISSES, 1);
    bytes = (size_t)colors->command_words * sizeof(int) +
        (size_t)vertices * sizeof(xgl_alias_parent_t) +
        (size_t)colors->triangle_count * 3 * sizeof(unsigned short);
    if (slot < 0) slot = XGL_OldestAliasTopology(NULL);
    if (slot < 0) return NULL;
    topology = &xgl_alias_topologies[slot];
    XGL_FreeAliasTopology(topology, XGL_PROFILE_ALIAS_POSE_SLOT_EVICTIONS);
    if (!XGL_AliasPoseRoom(bytes, NULL)) return NULL;
    topology->commands = (int *)malloc((size_t)colors->command_words * sizeof(int));
    topology->parents = (xgl_alias_parent_t *)malloc((size_t)vertices * sizeof(*topology->parents));
    topology->indices = (unsigned short *)malloc((size_t)colors->triangle_count * 3 * sizeof(*topology->indices));
    if (!topology->commands || !topology->parents || !topology->indices) {
        XGL_FreeAliasTopology(topology, XGL_PROFILE_ALIAS_POSE_SLOT_EVICTIONS);
        return NULL;
    }
    topology->depth = colors->depth; topology->command_words = colors->command_words;
    topology->command_hash = colors->command_hash;
    topology->generation = colors->generation;
    topology->base_count = header->poseverts; topology->vertex_count = vertices;
    topology->triangle_count = colors->triangle_count;
    topology->used = xgl_alias_pose_used; topology->bytes = bytes;
    xgl_alias_pose_bytes += bytes;
    memcpy(topology->commands, colors->commands, (size_t)colors->command_words * sizeof(int));
    consumed = index_count = 0;
    vertex_count = header->poseverts;
    order = colors->commands;
    while ((count = *order++) != 0) {
        fan = count < 0;
        if (fan) count = -count;
        a = consumed; b = consumed + 1;
        for (i = 2; i < count; ++i) {
            index = consumed + i;
            if (!fan && (i & 1))
                XGL_PoseTriangle(topology, b, a, index, colors->depth, &vertex_count, &index_count);
            else XGL_PoseTriangle(topology, a, b, index, colors->depth, &vertex_count, &index_count);
            if (!fan) a = b;
            b = index;
        }
        consumed += count;
        order += count * 2;
    }
    if (vertex_count != vertices || index_count != colors->triangle_count * 3) {
        XGL_FreeAliasTopology(topology, XGL_PROFILE_ALIAS_POSE_SLOT_EVICTIONS);
        return NULL;
    }
    /* A model the loader built says, after its commands, which pose
     * vertices are one point of the mesh. */
    if ((int)(order - colors->commands) + header->poseverts == colors->command_words)
        XGL_BuildAliasStrips(topology, order);
    return topology;
}

/* Where on the skin a pose vertex is: a command's own coordinates, or for
 * the middle of an edge the mean of its two ends', as the fallback has it. */
static void XGL_AliasSkinPlace(const xgl_alias_topology_t *topology, int vertex, float *place)
{
    const int *order;
    float a[2], b[2];
    int count, consumed;
    if (vertex >= topology->base_count) {
        XGL_AliasSkinPlace(topology, topology->parents[vertex].a, a);
        XGL_AliasSkinPlace(topology, topology->parents[vertex].b, b);
        place[0] = (a[0] + b[0]) * .5f;
        place[1] = (a[1] + b[1]) * .5f;
        return;
    }
    place[0] = place[1] = 0;
    order = topology->commands;
    consumed = 0;
    while ((count = *order++) != 0) {
        if (count < 0) count = -count;
        if (vertex < consumed + count) {
            memcpy(place, order + 2 * (vertex - consumed), 2 * sizeof(float));
            return;
        }
        consumed += count;
        order += count * 2;
    }
}

/* Two normals that cancel leave the point between them no direction of its
 * own, and it takes one of theirs. Which one must not depend on which end
 * is named first: the triangles either side of the edge name them in
 * opposite orders, and both must light that point alike, the more so when
 * it is one point of a strip. The end that comes first on the skin has it. */
static int XGL_AliasSkinFirst(const float *a, const float *b)
{
    return a[0] < b[0] || (a[0] == b[0] && a[1] <= b[1]);
}

static xgl_alias_pose_cache_t *XGL_FindAliasPose(const aliashdr_t *header,
                                  const trivertx_t *pose,
                                  const xgl_alias_cache_t *colors, int create)
{
    xgl_alias_pose_cache_t *cache;
    xgl_alias_topology_t *topology;
    xgl_alias_normals_t normals;
    float place_a[2], place_b[2];
    unsigned int hash;
    size_t bytes;
    int slot, i, j, index, a, b;
    if (++xgl_alias_pose_used == 0) {
        for (i = 0; i < XGL_ALIAS_POSE_SLOTS; ++i) xgl_alias_poses[i].used = 0;
        for (i = 0; i < XGL_ALIAS_TOPOLOGY_SLOTS; ++i) xgl_alias_topologies[i].used = 0;
        xgl_alias_pose_used = 1;
    }
    topology = XGL_AliasTopology(header, colors, create);
    if (!topology) return NULL;
    hash = 2166136261U;
    for (i = 0; i < header->poseverts; ++i)
        hash = (hash ^ pose[i].lightnormalindex) * 16777619U;
    slot = -1;
    for (i = 0; i < XGL_ALIAS_POSE_SLOTS; ++i) {
        cache = &xgl_alias_poses[i];
        if (!cache->normals) { if (slot < 0) slot = i; continue; }
        if (cache->topology != topology || cache->normal_hash != hash ||
            memcmp(cache->scale, header->scale, sizeof(vec3_t))) continue;
        for (j = 0; j < header->poseverts; ++j)
            if (cache->normal_indices[j] != pose[j].lightnormalindex) break;
        if (j != header->poseverts) continue;
        cache->used = xgl_alias_pose_used;
        if (create) XGL_ProfileCount(XGL_PROFILE_ALIAS_POSE_HITS, 1);
        return cache;
    }
    if (!create) return NULL;
    XGL_ProfileCount(XGL_PROFILE_ALIAS_POSE_MISSES, 1);
    bytes = (size_t)header->poseverts + (size_t)topology->vertex_count * sizeof(vec3_t);
    if (slot < 0) {
        slot = XGL_OldestAliasPose();
        if (slot < 0) return NULL;
        XGL_EvictAliasPose(&xgl_alias_poses[slot], XGL_PROFILE_ALIAS_POSE_SLOT_EVICTIONS);
    }
    if (!XGL_AliasPoseRoom(bytes, topology)) return NULL;
    cache = &xgl_alias_poses[slot];
    cache->normal_indices = (byte *)malloc((size_t)header->poseverts);
    cache->normals = (vec3_t *)malloc((size_t)topology->vertex_count * sizeof(vec3_t));
    if (!cache->normal_indices || !cache->normals) {
        XGL_FreeAliasPose(cache);
        return NULL;
    }
    cache->topology = topology; cache->normal_hash = hash;
    cache->used = xgl_alias_pose_used; cache->bytes = bytes;
    xgl_alias_pose_bytes += bytes;
    VectorCopy(header->scale, cache->scale);
    memset(normals.valid, 0, sizeof(normals.valid));
    for (i = 0; i < header->poseverts; ++i) {
        index = pose[i].lightnormalindex;
        cache->normal_indices[i] = (byte)index;
        if (index >= XGL_VERTEX_NORMALS) index = 0;
        if (!normals.valid[index]) {
            for (j = 0; j < 3; ++j)
                normals.normals[index][j] = header->scale[j] != 0 ?
                    r_avertexnormals[index][j] / header->scale[j] : r_avertexnormals[index][j];
            VectorNormalize(normals.normals[index]);
            normals.valid[index] = 1;
        }
        VectorCopy(normals.normals[index], cache->normals[i]);
    }
    for (; i < topology->vertex_count; ++i) {
        a = topology->parents[i].a; b = topology->parents[i].b;
        for (j = 0; j < 3; ++j)
            cache->normals[i][j] = (cache->normals[a][j] + cache->normals[b][j]) * .5f;
        if (VectorNormalize(cache->normals[i]) == 0) {
            XGL_AliasSkinPlace(topology, a, place_a);
            XGL_AliasSkinPlace(topology, b, place_b);
            VectorCopy(cache->normals[XGL_AliasSkinFirst(place_a, place_b) ? a : b], cache->normals[i]);
        }
    }
    return cache;
}

static xgl_alias_pose_cache_t *XGL_AliasPose(const aliashdr_t *header,
                                           const trivertx_t *pose,
                                           const xgl_alias_cache_t *colors)
{
    return XGL_FindAliasPose(header, pose, colors, 1);
}

/* The device's former ambient-plus-fill sum for one model-space normal. */
static float XGL_AliasShade(const xgl_alias_shade_t *shade, const float *normal)
{
    float fill;
    fill = DotProduct(normal, shade->fill);
    return fill > 0 ? shade->ambient + fill : shade->ambient;
}

/* A world normal is forward*n0 - right*n1 + up*n2, so the fixed world
 * direction toward the fill light is turned into model space once for the
 * model rather than every normal being turned into world space. */
static void XGL_AliasShadeSetup(xgl_alias_shade_t *shade, int lit, const vec3_t forward,
                                const vec3_t right, const vec3_t up)
{
    float axis;
    memset(shade, 0, sizeof(*shade));
    if (!lit) { shade->mode = XGL_SHADE_FLAT; return; }
    if (!xgl_alias_shade.value) { shade->mode = XGL_SHADE_NATIVE; return; }
    shade->mode = XGL_SHADE_CPU;
    shade->ambient = XGL_ALIAS_AMBIENT * XGL_AmbientLight();
    axis = XGL_ALIAS_FILL * XGL_ALIAS_FILL_AXIS;
    shade->fill[0] = (forward[0] + forward[1] + forward[2]) * axis;
    shade->fill[1] = -(right[0] + right[1] + right[2]) * axis;
    shade->fill[2] = (up[0] + up[1] + up[2]) * axis;
}

/* Quake adds every dynamic light within reach to a model's own light level.
 * With CPU shading no native light reaches the models to do that instead. */
static int XGL_AliasDynamicLight(const entity_t *entity)
{
    vec3_t delta;
    float total, distance;
    int i;
    if (!r_dynamic.value) return 0;
    total = 0;
    for (i = 0; i < MAX_DLIGHTS; ++i) {
        if (cl_dlights[i].die < cl.time || cl_dlights[i].radius <= 0) continue;
        VectorSubtract(entity->origin, cl_dlights[i].origin, delta);
        distance = DotProduct(delta, delta);
        if (distance >= cl_dlights[i].radius * cl_dlights[i].radius) continue;
        total += cl_dlights[i].radius - (float)sqrt(distance);
    }
    return (int)total;
}

static void XGL_PrepareAliasPose(const vec3_t scale, const vec3_t scale_origin,
                                const trivertx_t *pose,
                                const xgl_alias_pose_cache_t *cache,
                                const vec3_t forward, const vec3_t right,
                                const vec3_t up, const vec3_t origin,
                                const xgl_alias_shade_t *shade,
                                xgl_alias_work_vertex_t *work)
{
    const xgl_alias_topology_t *topology;
    xgl_alias_work_vertex_t *out;
    const float *normal;
    xgl_alias_normals_t normals;
    vec3_t local;
    int i, j, normal_index, a, b;
    topology = cache->topology;
    memset(normals.valid, 0, sizeof(normals.valid));
    for (i = 0; i < topology->vertex_count; ++i) {
        out = &work[i]; normal = cache->normals[i];
        if (i < topology->base_count) {
            for (j = 0; j < 3; ++j)
                local[j] = pose[i].v[j] * scale[j] + scale_origin[j];
            for (j = 0; j < 3; ++j)
                out->position[j] = origin[j] + forward[j] * local[0] -
                    right[j] * local[1] + up[j] * local[2];
        } else {
            a = topology->parents[i].a; b = topology->parents[i].b;
            for (j = 0; j < 3; ++j)
                out->position[j] = (work[a].position[j] +
                                    work[b].position[j]) * .5f;
        }
        if (shade->mode == XGL_SHADE_FLAT) continue;
        if (shade->mode == XGL_SHADE_CPU) {
            /* Vertices with the same normal index share one evaluation. */
            if (i >= topology->base_count) {
                out->light.shade = XGL_AliasShade(shade, normal);
                continue;
            }
            normal_index = pose[i].lightnormalindex;
            if (normal_index >= XGL_VERTEX_NORMALS) normal_index = 0;
            if (!normals.valid[normal_index]) {
                normals.normals[normal_index][0] = XGL_AliasShade(shade, normal);
                normals.valid[normal_index] = 1;
            }
            out->light.shade = normals.normals[normal_index][0];
            continue;
        }
        if (i < topology->base_count) {
            normal_index = pose[i].lightnormalindex;
            if (normal_index >= XGL_VERTEX_NORMALS) normal_index = 0;
            if (!normals.valid[normal_index]) {
                for (j = 0; j < 3; ++j)
                    normals.normals[normal_index][j] = forward[j] * normal[0] -
                        right[j] * normal[1] + up[j] * normal[2];
                normals.valid[normal_index] = 1;
            }
            VectorCopy(normals.normals[normal_index], out->light.normal);
        } else {
            for (j = 0; j < 3; ++j)
                out->light.normal[j] = forward[j] * normal[0] -
                    right[j] * normal[1] + up[j] * normal[2];
        }
    }
}

static void XGL_AliasFlush(xgl_alias_draw_t *draw, int lit)
{
    if (!draw->count) return;
    /* CPU-shaded colors are final: gamma was applied before the shade. */
    if (draw->shade && draw->shade->mode == XGL_SHADE_CPU)
        XGL_DrawTrianglesStrided(draw->vertices, draw->count, sizeof(draw->vertices[0]),
                                 0, 1, 1, 1);
    else XGL_DrawTriangles(draw->vertices, draw->count, lit, 1, 1);
    draw->count = 0;
}

static void XGL_AliasRawColor(const byte *palette, int index, float rgb[3])
{
    volatile float normalized;
    int i;
    for (i = 0; i < 3; ++i) {
        /* Keep the old sampler's stored float rounding before lighting,
         * including with -ffast-math. -1 is black, not palette entry zero. */
        normalized = index < 0 ? 0 : palette[index * 3 + i] * (1.0f / 255.0f);
        rgb[i] = normalized;
    }
}

typedef struct {
    int triangle;
    float prepared[257][3];
    byte valid[257];
} xgl_alias_output_cursor_t;

/* A palette color under the model's light, ready for the device: worked
 * out once for each color a model uses. Providing a gamma snapshot makes
 * this a pure worker operation. */
static const float *XGL_AliasColor(const xgl_alias_job_t *job, xgl_alias_output_cursor_t *cursor,
                                   int index)
{
    float *rgb;
    int k, slot;
    slot = index + 1;
    rgb = cursor->prepared[slot];
    if (!cursor->valid[slot]) {
        XGL_AliasRawColor(job->palette, index, rgb);
        for (k = 0; k < 3; ++k) rgb[k] *= job->light;
        if (job->gamma) {
            /* Palette and clamped entity light are both in [0,1]. */
            for (k = 0; k < 3; ++k)
                rgb[k] = job->gamma[(int)(rgb[k] * 255.0f + .5f)];
        } else XGL_PrepareColor(rgb);
        cursor->valid[slot] = 1;
    }
    return rgb;
}

/* Resume at the next visible triangle when a native batch was split.
 * Models the device illuminates are written as points with normals; all
 * others, which are submitted unlit, as points without. */
static void XGL_CopyAliasOutput(const xgl_alias_job_t *job, xgl_alias_output_cursor_t *cursor,
                                void *output, int count)
{
    const xgl_alias_topology_t *topology;
    const xgl_alias_color_t *colors;
    const xgl_alias_work_vertex_t *source;
    xgl_native_vertex_t *out;
    xgl_color_vertex_t *plain;
    const float *rgb;
    float shade;
    int i, j, mode;
    topology = job->cached_pose->topology;
    mode = job->shade.mode;
    out = (xgl_native_vertex_t *)output;
    plain = (xgl_color_vertex_t *)output;
    while (count > 0) {
        i = cursor->triangle++;
        colors = &job->colors->colors[i];
        if (!colors->visible) continue;
        for (j = 0; j < 3; ++j) {
            rgb = XGL_AliasColor(job, cursor, colors->index[j]);
            source = &job->work[topology->indices[i * 3 + j]];
            if (mode == XGL_SHADE_NATIVE) {
                VectorCopy(source->position, out->position);
                VectorCopy(source->light.normal, out->normal);
                VectorCopy(rgb, out->color.rgb);
                ++out;
                continue;
            }
            VectorCopy(source->position, plain->position);
            if (mode == XGL_SHADE_CPU) {
                /* The device multiplied the prepared color by the same sum. */
                shade = source->light.shade;
                plain->color.rgb[0] = rgb[0] * shade;
                plain->color.rgb[1] = rgb[1] * shade;
                plain->color.rgb[2] = rgb[2] * shade;
            } else { VectorCopy(rgb, plain->color.rgb); }
            ++plain;
        }
        count -= 3;
    }
}

/* The points of strips, `count` of them, as records without normals: each
 * where its pose vertex is, in the color that vertex's place on the skin
 * has under the model's light. */
static void XGL_CopyAliasPoints(const xgl_alias_job_t *job, xgl_alias_output_cursor_t *cursor,
                                const unsigned short *vertices, int count,
                                xgl_color_vertex_t *out)
{
    const xgl_alias_topology_t *topology;
    const xgl_alias_color_t *colors;
    const xgl_alias_work_vertex_t *source;
    const float *rgb;
    float shade;
    int i, corner, shaded;
    topology = job->cached_pose->topology;
    colors = job->colors->colors;
    shaded = job->shade.mode == XGL_SHADE_CPU;
    for (i = 0; i < count; ++i, ++out) {
        corner = topology->point_corner[vertices[i]];
        rgb = XGL_AliasColor(job, cursor, colors[corner >> 2].index[corner & 3]);
        source = &job->work[vertices[i]];
        VectorCopy(source->position, out->position);
        if (shaded) {
            shade = source->light.shade;
            out->color.rgb[0] = rgb[0] * shade;
            out->color.rgb[1] = rgb[1] * shade;
            out->color.rgb[2] = rgb[2] * shade;
        } else { VectorCopy(rgb, out->color.rgb); }
    }
}

/* Where the writing of the strips too short to send as strips has got to:
 * they go out as separate triangles, as the native batch has room. */
typedef struct {
    int strip, triangle;
    const unsigned short *vertices;
} xgl_alias_rest_t;

static void XGL_CopyAliasRest(const xgl_alias_job_t *job, xgl_alias_output_cursor_t *cursor,
                              xgl_alias_rest_t *rest, xgl_color_vertex_t *out, int count)
{
    const xgl_alias_topology_t *topology;
    topology = job->cached_pose->topology;
    for (; count > 0 && rest->strip < topology->strip_count; count -= 3, out += 3) {
        XGL_CopyAliasPoints(job, cursor, rest->vertices + rest->triangle, 3, out);
        if (++rest->triangle == topology->strip_lengths[rest->strip] - 2) {
            rest->vertices += topology->strip_lengths[rest->strip];
            rest->triangle = 0;
            ++rest->strip;
        }
    }
}

/* Whether a model goes out as strips, and if so with the topology's count
 * of those long enough up to date. Strips are points without normals, so a
 * model the device illuminates has none; nor has one with holes in it. */
static int XGL_AliasUsesStrips(xgl_alias_topology_t *topology, const xgl_alias_cache_t *colors,
                               const xgl_alias_shade_t *shade)
{
    int shortest;
    shortest = XGL_EntitySetting(xgl_alias_strips.value, 0, XGL_ALIAS_STRIP_POINTS);
    if (shortest < 2 || shade->mode == XGL_SHADE_NATIVE || !topology->strip_count ||
        colors->visible_triangles != colors->triangle_count) return 0;
    XGL_AliasStripSplit(topology, shortest);
    return topology->split_strips > 0;
}

/* The records a model comes to as strips and the triangles left over. */
static int XGL_AliasStripRecords(const xgl_alias_topology_t *topology)
{
    return topology->split_points +
        3 * (topology->triangle_count - (topology->split_points - 2 * topology->split_strips));
}

static void XGL_EmitAliasPose(const xgl_alias_pose_cache_t *cache,
                             xgl_alias_draw_t *draw, float light, int lit)
{
    static xgl_color_vertex_t points[XGL_ALIAS_STRIP_POINTS];
    xgl_alias_output_cursor_t cursor;
    xgl_alias_job_t job;
    xgl_alias_rest_t rest;
    xgl_alias_topology_t *topology;
    xgl_color_vertex_t *plain;
    void *vertices;
    int count, remaining, i;
    cursor.triangle = 0;
    memset(cursor.valid, 0, sizeof(cursor.valid));
    job.work = xgl_alias_work; job.colors = draw->cache; job.cached_pose = cache;
    job.palette = xgl_palette; job.light = light; job.gamma = NULL;
    job.shade = *draw->shade;
    topology = cache->topology;
    if (XGL_AliasUsesStrips(topology, draw->cache, draw->shade)) {
        /* Each strip is a native call of its own; what is left joins the
         * batch of separate triangles. */
        rest.vertices = topology->strip_vertices;
        for (i = 0; i < topology->split_strips; ++i) {
            count = topology->strip_lengths[i];
            XGL_CopyAliasPoints(&job, &cursor, rest.vertices, count, points);
            XGL_DrawColorStrip(points, count, 1, 1);
            rest.vertices += count;
        }
        rest.strip = topology->split_strips; rest.triangle = 0;
        remaining = XGL_AliasStripRecords(topology) - topology->split_points;
        while (remaining) {
            count = remaining;
            plain = XGL_ReserveColorTriangles(&count, 1, 1);
            if (!plain) return;
            XGL_CopyAliasRest(&job, &cursor, &rest, plain, count);
            remaining -= count;
        }
        return;
    }
    remaining = draw->cache->visible_triangles * 3;
    while (remaining) {
        count = remaining;
        if (job.shade.mode == XGL_SHADE_NATIVE)
            vertices = XGL_ReserveTriangles(&count, lit, 1, 1);
        else vertices = XGL_ReserveColorTriangles(&count, 1, 1);
        if (!vertices) return;
        XGL_CopyAliasOutput(&job, &cursor, vertices, count);
        remaining -= count;
    }
}

static void XGL_RunAliasJob(void *argument)
{
    const xgl_alias_topology_t *topology;
    xgl_alias_job_t *job;
    xgl_alias_output_cursor_t cursor;
    xgl_alias_rest_t rest;
    job = (xgl_alias_job_t *)argument;
    cursor.triangle = 0;
    memset(cursor.valid, 0, sizeof(cursor.valid));
    XGL_PrepareAliasPose(job->scale, job->scale_origin, job->pose, job->cached_pose,
                         job->forward, job->right, job->up, job->origin, &job->shade, job->work);
    if (job->strips > 0) {
        /* The strips one after another, then the triangles left over. */
        topology = job->cached_pose->topology;
        XGL_CopyAliasPoints(job, &cursor, topology->strip_vertices, job->strip_points,
                            (xgl_color_vertex_t *)job->output);
        rest.strip = job->strips; rest.triangle = 0;
        rest.vertices = topology->strip_vertices + job->strip_points;
        XGL_CopyAliasRest(job, &cursor, &rest,
                          (xgl_color_vertex_t *)job->output + job->strip_points,
                          job->count - job->strip_points);
        return;
    }
    XGL_CopyAliasOutput(job, &cursor, job->output, job->count);
}

static int XGL_EntitySetting(float value, int low, int high)
{
    if (!(value >= low)) return low;
    if (value > high) return high;
    return (int)value;
}

void XGL_EntitiesInit(void)
{
    int i;
    XGL_EntitiesShutdown();
    Cvar_RegisterVariable(&xgl_alias_subdivide);
    Cvar_RegisterVariable(&xgl_sprite_cells);
    Cvar_RegisterVariable(&xgl_entity_triangles);
    Cvar_RegisterVariable(&xgl_playercolors);
    Cvar_RegisterVariable(&xgl_alias_shade);
    Cvar_RegisterVariable(&xgl_alias_strips);
    for (i = 0; i < MAX_SCOREBOARD; ++i) xgl_player_color[i] = -1;
}

void R_TranslatePlayerSkin(int playernum)
{
    int i, top, bottom;
    byte *table;
    if (playernum < 0 || playernum >= cl.maxclients ||
        playernum >= MAX_SCOREBOARD || !cl.scores) return;
    table = xgl_player_translation[playernum];
    top = cl.scores[playernum].colors & 0xf0;
    bottom = (cl.scores[playernum].colors & 15) << 4;
    for (i = 0; i < 256; ++i) table[i] = (byte)i;
    for (i = 0; i < 16; ++i) {
        table[TOP_RANGE + i] = (byte)(top < 128 ? top + i : top + 15 - i);
        table[BOTTOM_RANGE + i] = (byte)(bottom < 128 ? bottom + i : bottom + 15 - i);
    }
    xgl_player_color[playernum] = cl.scores[playernum].colors;
}

static const byte *XGL_EntityTranslation(entity_t *entity)
{
    int i;
    if (!xgl_playercolors.value || !cl.scores || entity->colormap == vid.colormap)
        return NULL;
    /* Pointer equality avoids subtracting unrelated entity pointers, as the
     * view weapon and temporary entities are not members of cl_entities. */
    for (i = 0; i < cl.maxclients && i < MAX_SCOREBOARD; ++i) {
        if (entity != &cl_entities[i + 1]) continue;
        if (xgl_player_color[i] != cl.scores[i].colors)
            R_TranslatePlayerSkin(i);
        return xgl_player_translation[i];
    }
    return NULL;
}

static void XGL_EntityMidpoint(const xgl_entity_vertex_t *a,
                              const xgl_entity_vertex_t *b,
                              xgl_entity_vertex_t *out)
{
    const xgl_entity_vertex_t *first;
    float place_a[2], place_b[2];
    int i;
    for (i = 0; i < 3; ++i) {
        out->position[i] = (a->position[i] + b->position[i]) * .5f;
        out->normal[i] = (a->normal[i] + b->normal[i]) * .5f;
    }
    if (VectorNormalize(out->normal) == 0) {
        /* The same choice as the cached pose makes. */
        place_a[0] = a->s; place_a[1] = a->t;
        place_b[0] = b->s; place_b[1] = b->t;
        first = XGL_AliasSkinFirst(place_a, place_b) ? a : b;
        VectorCopy(first->normal, out->normal);
    }
    out->s = (a->s + b->s) * .5f;
    out->t = (a->t + b->t) * .5f;
}

static void XGL_EntityTriangleCached(const xgl_entity_vertex_t *a,
                              const xgl_entity_vertex_t *b,
                              const xgl_entity_vertex_t *c,
                              int material, const byte *translation,
                              float light, int lit, int depth,
                              xgl_alias_draw_t *draw)
{
    const xgl_entity_vertex_t *source[3];
    xgl_entity_vertex_t ab, bc, ca;
    xgl_vertex_t *vertices;
    xgl_alias_color_t sampled, *colors;
    float shade;
    int i, j, center, index, shaded;
    if (depth > 0) {
        XGL_EntityMidpoint(a, b, &ab);
        XGL_EntityMidpoint(b, c, &bc);
        XGL_EntityMidpoint(c, a, &ca);
        XGL_EntityTriangleCached(a, &ab, &ca, material, translation, light, lit, depth - 1, draw);
        XGL_EntityTriangleCached(&ab, b, &bc, material, translation, light, lit, depth - 1, draw);
        XGL_EntityTriangleCached(&ca, &bc, c, material, translation, light, lit, depth - 1, draw);
        XGL_EntityTriangleCached(&ab, &bc, &ca, material, translation, light, lit, depth - 1, draw);
        return;
    }
    colors = &sampled;
    if (draw->cache && draw->index < draw->cache->triangle_count)
        colors = &draw->cache->colors[draw->index++];
    /* Alpha index 255 is coverage, not a black vertex. The centroid decides
     * coverage of each small sprite triangle. Transparent corners inherit
     * its opaque color so that interpolation cannot introduce black fringes. */
    source[0] = a; source[1] = b; source[2] = c;
    if (colors == &sampled || draw->building) {
        colors->visible = 0;
        center = -1;
        /* Opaque alias skins need no coverage sample. Sprites retain their
         * centroid coverage rule; cached aliases also retain alpha holes. */
        if (!draw->opaque) {
            if (!XGL_SampleTextureIndex(material, (a->s + b->s + c->s) / 3,
                                        (a->t + b->t + c->t) / 3, &center)) return;
            if (translation) center = translation[center];
        }
        for (i = 0; i < 3; ++i) {
            /* Coverage is decided by the source index, before player
             * translation: a translated index 255 is still an opaque color. */
            if (XGL_SampleTextureIndex(material, source[i]->s, source[i]->t, &index))
                colors->index[i] = translation ? translation[index] : index;
            else colors->index[i] = center;
        }
        colors->visible = 1;
    }
    if (!colors->visible) return;
    vertices = &draw->vertices[draw->count];
    shaded = draw->shade && draw->shade->mode == XGL_SHADE_CPU;
    for (i = 0; i < 3; ++i) {
        VectorCopy(source[i]->position, vertices[i].position);
        VectorCopy(source[i]->normal, vertices[i].normal);
        XGL_AliasRawColor(xgl_palette, colors->index[i], vertices[i].color);
        for (j = 0; j < 3; ++j) vertices[i].color[j] *= light;
        if (shaded) {
            /* The same order as the cached path: gamma, then the shade. */
            XGL_PrepareColor(vertices[i].color);
            shade = (source[i]->normal[0] + source[i]->normal[1] + source[i]->normal[2]) *
                (XGL_ALIAS_FILL * XGL_ALIAS_FILL_AXIS);
            shade = shade > 0 ? draw->shade->ambient + shade : draw->shade->ambient;
            for (j = 0; j < 3; ++j) vertices[i].color[j] *= shade;
        }
    }
    draw->count += 3;
    if (draw->count == XGL_ENTITY_BATCH_VERTICES) XGL_AliasFlush(draw, lit);
}

static int XGL_AliasDepth(entity_t *entity, int triangles)
{
    vec3_t distance;
    int depth, extra, budget;
    depth = XGL_EntitySetting(xgl_alias_subdivide.value, 0, 2);
    VectorSubtract(entity->origin, r_origin, distance);
    if (entity != &cl.viewent) {
        if (DotProduct(distance, distance) > 512.0f * 512.0f) depth = 0;
        else if (depth > 1 && DotProduct(distance, distance) > 192.0f * 192.0f) depth = 1;
    }
    budget = XGL_EntitySetting(xgl_entity_triangles.value, 0, 32768);
    while (depth > 0) {
        extra = triangles * ((1 << (2 * depth)) - 1);
        if (extra <= budget - xgl_entity_extra_used) {
            xgl_entity_extra_used += extra;
            break;
        }
        --depth;
    }
    return depth;
}

static void XGL_AliasVertex(const trivertx_t *pose, const aliashdr_t *header,
                           const vec3_t forward, const vec3_t right,
                           const vec3_t up, const vec3_t origin,
                           const int *uv, xgl_entity_vertex_t *out,
                           xgl_alias_normals_t *normals)
{
    vec3_t local, normal;
    int i, index;
    /* Read the embedded floats without strict-aliasing or SPARC alignment
     * assumptions beyond the loader's aligned int command buffer. */
    memcpy(&out->s, uv, sizeof(float));
    memcpy(&out->t, uv + 1, sizeof(float));
    index = pose->lightnormalindex;
    if (index >= XGL_VERTEX_NORMALS) index = 0;
    for (i = 0; i < 3; ++i) {
        local[i] = pose->v[i] * header->scale[i] + header->scale_origin[i];
    }
    if (!normals->valid[index]) {
        for (i = 0; i < 3; ++i)
            normal[i] = header->scale[i] != 0 ?
                r_avertexnormals[index][i] / header->scale[i] : r_avertexnormals[index][i];
        for (i = 0; i < 3; ++i)
            normals->normals[index][i] = forward[i] * normal[0] -
                right[i] * normal[1] + up[i] * normal[2];
        VectorNormalize(normals->normals[index]);
        normals->valid[index] = 1;
    }
    for (i = 0; i < 3; ++i) {
        out->position[i] = origin[i] + forward[i] * local[0] -
            right[i] * local[1] + up[i] * local[2];
        out->normal[i] = normals->normals[index][i];
    }
}

/* The source pose is locked by the caller until its last byte is copied. */
static void XGL_SubmitAliasJob(int visible_index, const entity_t *entity, const aliashdr_t *header,
                               const trivertx_t *pose, const xgl_alias_pose_cache_t *cached_pose,
                               const xgl_alias_cache_t *colors, const vec3_t forward,
                               const vec3_t right, const vec3_t up, float light, int lit,
                               const xgl_alias_shade_t *shade)
{
    xgl_alias_topology_t *topology;
    xgl_alias_job_t *job;
    int corners;
    job = &xgl_alias_frame->jobs[xgl_alias_frame->job_count];
    job->visible_index = visible_index;
    job->triangles = header->numtris;
    job->light = light; job->lit = lit;
    job->shade = *shade;
    VectorCopy(header->scale, job->scale);
    VectorCopy(header->scale_origin, job->scale_origin);
    VectorCopy(entity->origin, job->origin);
    VectorCopy(forward, job->forward);
    VectorCopy(right, job->right);
    VectorCopy(up, job->up);
    memcpy(xgl_alias_frame->poses + xgl_alias_frame->pose_count, pose,
           header->poseverts * sizeof(*pose));
    job->pose = xgl_alias_frame->poses + xgl_alias_frame->pose_count;
    job->work = xgl_alias_frame->work;
    job->cached_pose = cached_pose;
    job->colors = colors;
    job->palette = xgl_alias_frame->palette;
    job->gamma = xgl_alias_frame->gamma;
    corners = colors->visible_triangles * 3;
    job->strips = job->strip_points = 0;
    job->lengths = NULL;
    topology = cached_pose->topology;
    if (XGL_AliasUsesStrips(topology, colors, shade) &&
        topology->split_strips <= XGL_ALIAS_FRAME_STRIPS - xgl_alias_frame->length_count) {
        /* Never more records than as separate triangles. */
        job->strips = topology->split_strips;
        job->strip_points = topology->split_points;
        job->lengths = xgl_alias_frame->lengths + xgl_alias_frame->length_count;
        memcpy(xgl_alias_frame->lengths + xgl_alias_frame->length_count, topology->strip_lengths,
               (size_t)job->strips * sizeof(topology->strip_lengths[0]));
        corners = XGL_AliasStripRecords(topology);
    }
    job->count = corners;
    job->output = xgl_alias_frame->output + xgl_alias_frame->corner_count;
    if (XGL_SMPSubmit(XGL_RunAliasJob, job)) {
        ++xgl_alias_frame->job_count;
        xgl_alias_frame->length_count += job->strips;
        xgl_alias_frame->corner_count += corners;
        xgl_alias_frame->pose_count += header->poseverts;
        xgl_alias_frame->pending = 1;
        XGL_ProfileCount(XGL_PROFILE_ALIAS_COLOR_HITS, 1);
        XGL_ProfileCount(XGL_PROFILE_ALIAS_TOPOLOGY_HITS, 1);
        XGL_ProfileCount(XGL_PROFILE_ALIAS_POSE_HITS, 1);
    }
}

static int XGL_AliasIsFlame(const model_t *model)
{
    return !strcmp(model->name, "progs/flame.mdl") ||
           !strcmp(model->name, "progs/flame2.mdl");
}

static int XGL_AliasCulled(entity_t *entity)
{
    vec3_t mins, maxs;
    float radius;
    int i;
    if (entity == &cl.viewent) return 0;
    radius = entity->model->radius;
    for (i = 0; i < 3; ++i) {
        mins[i] = entity->origin[i] - radius;
        maxs[i] = entity->origin[i] + radius;
    }
    /* A loader without a valid radius must not reduce an animated model
     * to a point for culling. The XGL loader computes pose-wide bounds. */
    return radius > 0 && R_CullBox(mins, maxs);
}

/* A nonnegative visible index selects an early, hit-only preparation pass.
 * Cold/oversized entries run the ordinary draw path later. A planned depth
 * comes from XGL_PlanAliasEntities, which has already culled the entity;
 * XGL_ALIAS_UNPLANNED culls and spends the detail allowance here. */
static int XGL_ProcessAlias(entity_t *entity, int visible_index, int planned)
{
    aliashdr_t *header;
    const trivertx_t *pose;
    const byte *translation;
    const int *order;
    xgl_entity_vertex_t a, b, v;
    xgl_alias_draw_t draw;
    xgl_alias_pose_cache_t *cached_pose = NULL;
    xgl_alias_normals_t normals;
    xgl_alias_shade_t shade;
    xgl_profile_draw_scope_t output_scope;
    vec3_t angles, forward, right, up;
    float light, shrink;
    double time, prepare_start, cache_start, transform_start, light_start;
    int i, j, frame, posenum, animation, skin, material, count, fan, consumed, depth, lit;
    int light_value;
    if (planned == XGL_ALIAS_UNPLANNED && XGL_AliasCulled(entity)) return 1;
    /* The view weapon is shrunk toward the eye: the same image, but nearer
     * than any world geometry, so it needs no depth clear of its own. */
    shrink = entity == &cl.viewent ? XGL_VIEWMODEL_SCALE : 1;
    /* A sound cache miss can move or evict Quake alias-cache allocations.
     * Service audio before acquiring pointers, then defer mixer calls until
     * the last header/pose/command access. Native batches still submit as
     * usual; their audio hooks respect this short, nestable cache lock. */
    XGL_AudioLock();
    /* Lock may itself service audio and evict a model. The early pass never
     * reloads one; the ordinary draw does that later.
     * Mod_Extradata touches the resident block on Quake's LRU as usual. */
    if (visible_index >= 0 && !entity->model->cache.data) {
        XGL_AudioUnlock();
        return 0;
    }
    header = (aliashdr_t *)Mod_Extradata(entity->model);
    if (!header || header->poseverts <= 0 || header->numposes <= 0 || header->numskins <= 0) {
        XGL_AudioUnlock();
        return 1;
    }
    frame = entity->frame;
    if (frame < 0 || frame >= header->numframes) frame = 0;
    posenum = header->frames[frame].firstpose;
    time = cl.time + entity->syncbase;
    if (header->frames[frame].numposes > 1 && header->frames[frame].interval > 0) {
        animation = (int)(time / header->frames[frame].interval) % header->frames[frame].numposes;
        if (animation < 0) animation += header->frames[frame].numposes;
        posenum += animation;
    }
    if (posenum < 0 || posenum >= header->numposes) posenum = 0;
    skin = entity->skinnum;
    if (skin < 0 || skin >= header->numskins) skin = 0;
    animation = ((int)(time * 10)) & 3;
    material = header->gl_texturenum[skin][animation];
    translation = XGL_EntityTranslation(entity);
    pose = (const trivertx_t *)((const byte *)header + header->posedata) + posenum * header->poseverts;
    order = (const int *)((const byte *)header + header->commands);
    depth = planned == XGL_ALIAS_UNPLANNED ? XGL_AliasDepth(entity, header->numtris) : planned;
    /* Detailed preparation covers cache lookup/build and cached live-pose
     * transforms. Fallback recursion and emission stay in the entity stage. */
    if (visible_index >= 0) {
        draw.cache = XGL_FindAliasColors(header, material, depth, translation, 0);
        cached_pose = draw.cache && draw.cache->ready ?
            XGL_FindAliasPose(header, pose, draw.cache, 0) : NULL;
        if (!cached_pose || !draw.cache->visible_triangles ||
            header->poseverts > XGL_ALIAS_FRAME_CORNERS - xgl_alias_frame->pose_count ||
            draw.cache->visible_triangles > (XGL_ALIAS_FRAME_CORNERS - xgl_alias_frame->corner_count) / 3) {
            XGL_AudioUnlock();
            return 1;
        }
    }
    lit = r_fullbright.value || XGL_AliasIsFlame(entity->model) ? 0 : 2;
    if (lit) {
        light_start = XGL_ProfileDetailNow();
        light_value = R_LightPoint(entity->origin);
        if (xgl_alias_shade.value) light_value += XGL_AliasDynamicLight(entity);
        XGL_ProfileAdd(entity == &cl.viewent ? XGL_PROFILE_WEAPON_LIGHT :
                       XGL_PROFILE_ALIAS_LIGHT, light_start);
        light = light_value / 128.0f;
    } else light = 1;
    if (entity == &cl.viewent && light < .20f) light = .20f;
    if (translation && light < .08f) light = .08f;
    if (light < .04f) light = .04f;
    if (light > 1) light = 1;
    VectorCopy(entity->angles, angles);
    /* The original alias path rotates -pitch; brush entities use the other
     * convention. Negating only this local copy preserves entity state. */
    angles[PITCH] = -angles[PITCH];
    AngleVectors(angles, forward, right, up);
    XGL_AliasShadeSetup(&shade, lit, forward, right, up);
    /* Only natively lit models are submitted illuminated. */
    if (shade.mode != XGL_SHADE_NATIVE) lit = 0;
    if (visible_index >= 0) {
        XGL_SubmitAliasJob(visible_index, entity, header, pose, cached_pose, draw.cache,
                           forward, right, up, light, lit, &shade);
        XGL_AudioUnlock();
        return 1;
    }
    prepare_start = cache_start = XGL_ProfileDetailNow();
    /* Avoid clearing the large output buffer; count guards every access. */
    draw.count = draw.index = 0;
    draw.shade = &shade;
    memset(normals.valid, 0, sizeof(normals.valid));
    draw.cache = XGL_AliasColors(header, material, depth, translation);
    draw.building = draw.cache && !draw.cache->ready;
    draw.opaque = XGL_TextureRevision(material) != 0 && !XGL_TextureHasAlpha(material);
    cached_pose = draw.cache && draw.cache->ready ? XGL_AliasPose(header, pose, draw.cache) : NULL;
    XGL_ProfileAdd(XGL_PROFILE_ALIAS_CACHE, cache_start);
    if (cached_pose) {
        transform_start = XGL_ProfileDetailNow();
        XGL_PrepareAliasPose(header->scale, header->scale_origin, pose, cached_pose,
                             forward, right, up, entity->origin, &shade, xgl_alias_work);
        if (shrink != 1)
            for (i = 0; i < cached_pose->topology->vertex_count; ++i)
                for (j = 0; j < 3; ++j)
                    xgl_alias_work[i].position[j] = r_origin[j] +
                        (xgl_alias_work[i].position[j] - r_origin[j]) * shrink;
        XGL_ProfileAdd(XGL_PROFILE_ALIAS_TRANSFORM, transform_start);
    }
    XGL_ProfileAdd(XGL_PROFILE_ALIAS_PREPARE, prepare_start);
    consumed = 0;
    c_alias_polys += header->numtris;
    if (cached_pose) {
        XGL_ProfileDrawBegin(&output_scope);
        XGL_EmitAliasPose(cached_pose, &draw, light, lit);
        XGL_ProfileDrawEnd(&output_scope, entity == &cl.viewent ?
                          XGL_PROFILE_DRAW_WEAPON_OUTPUT : XGL_PROFILE_DRAW_ALIAS_OUTPUT);
        XGL_AudioUnlock();
        return 1;
    }
    XGL_ProfileCount(XGL_PROFILE_ALIAS_FALLBACKS, 1);
    XGL_ProfileDrawBegin(&output_scope);
    while ((count = *order++) != 0) {
        if (count > header->poseverts - consumed || count < -(header->poseverts - consumed)) {
            XGL_AliasFlush(&draw, lit);
            XGL_ProfileDrawEnd(&output_scope, entity == &cl.viewent ?
                              XGL_PROFILE_DRAW_WEAPON_FALLBACK : XGL_PROFILE_DRAW_ALIAS_FALLBACK);
            XGL_AudioUnlock();
            Con_DPrintf("XGL: invalid alias commands in %s\n", entity->model->name);
            return 1;
        }
        fan = count < 0;
        if (fan) count = -count;
        for (i = 0; i < count; ++i) {
            XGL_AliasVertex(pose++, header, forward, right, up, entity->origin, order, &v, &normals);
            if (shrink != 1)
                for (j = 0; j < 3; ++j)
                    v.position[j] = r_origin[j] + (v.position[j] - r_origin[j]) * shrink;
            order += 2;
            if (i == 0) a = v;
            else if (i == 1) b = v;
            else {
                if (!fan && (i & 1))
                    XGL_EntityTriangleCached(&b, &a, &v, material, translation, light, lit, depth, &draw);
                else XGL_EntityTriangleCached(&a, &b, &v, material, translation, light, lit, depth, &draw);
                if (!fan) a = b;
                b = v;
            }
        }
        consumed += count;
    }
    if (draw.building && draw.index == draw.cache->triangle_count) {
        /* Coverage changes only when the sampled-color cache is rebuilt. */
        draw.cache->visible_triangles = 0;
        for (i = 0; i < draw.cache->triangle_count; ++i)
            if (draw.cache->colors[i].visible) ++draw.cache->visible_triangles;
        draw.cache->ready = 1;
    }
    XGL_AliasFlush(&draw, lit);
    XGL_ProfileDrawEnd(&output_scope, entity == &cl.viewent ?
                      XGL_PROFILE_DRAW_WEAPON_FALLBACK : XGL_PROFILE_DRAW_ALIAS_FALLBACK);
    XGL_AudioUnlock();
    return 1;
}

static void XGL_DrawAlias(entity_t *entity)
{
    XGL_ProcessAlias(entity, -1, XGL_ALIAS_UNPLANNED);
}

/* Decide every visible model's subdivision before any is prepared or drawn. */
static void XGL_PlanAliasEntities(void)
{
    entity_t *entity;
    aliashdr_t *header;
    int i, base;
    xgl_entity_extra_used = 0;
    for (i = 0; i < cl_numvisedicts; ++i) {
        xgl_alias_plan[i] = -1;
        entity = cl_visedicts[i];
        if (!entity || !entity->model || entity->model->type != mod_alias) continue;
        if (XGL_AliasCulled(entity)) continue;
        /* Only the triangle count is read; no pointer outlives the lock. */
        XGL_AudioLock();
        header = (aliashdr_t *)Mod_Extradata(entity->model);
        base = header && header->poseverts > 0 && header->numposes > 0 &&
               header->numskins > 0 ? header->numtris : -1;
        XGL_AudioUnlock();
        if (base < 0) continue;
        xgl_alias_plan[i] = (signed char)(XGL_AliasDepth(entity, base) |
            (XGL_AliasIsFlame(entity->model) ? XGL_ALIAS_FLAME : 0));
    }
    xgl_alias_planned = 1;
}

/* Under native illumination (xgl_alias_shade 0) the unlit flames are drawn
 * first, straight after the other unlit geometry, and the lit models after
 * them: one illumination change instead of one per torch. With CPU shading
 * every model is unlit and the two passes differ in nothing. */
static int XGL_AliasPassEntity(int pass, int index)
{
    entity_t *entity;
    int flame;
    entity = cl_visedicts[index];
    if (!entity || !entity->model || entity->model->type != mod_alias) return 0;
    flame = xgl_alias_plan[index] >= 0 && (xgl_alias_plan[index] & XGL_ALIAS_FLAME);
    return (pass == 0) == flame;
}

void XGL_AliasPrepareFrame(void)
{
    int i, pass;
    if (!XGL_SMPActive() || !r_drawentities.value) return;
    if (!xgl_alias_frame) {
        xgl_alias_frame = (xgl_alias_frame_t *)malloc(sizeof(*xgl_alias_frame));
        if (!xgl_alias_frame) return;
        xgl_alias_frame->pending = xgl_alias_frame->job_count = 0;
        xgl_alias_frame->pose_count = xgl_alias_frame->corner_count = 0;
        xgl_alias_frame->length_count = 0;
        /* Every record handed to the device has been written in full; this
         * only keeps the first frame's untouched tail well defined. */
        memset(xgl_alias_frame->output, 0, sizeof(xgl_alias_frame->output));
    }
    /* A repeated call cannot reuse output still borrowed by native batches. */
    if (xgl_alias_frame->job_count) return;
    memcpy(xgl_alias_frame->palette, xgl_palette, sizeof(xgl_alias_frame->palette));
    XGL_CopyGammaOutput(xgl_alias_frame->gamma);
    XGL_PlanAliasEntities();
    /* Jobs are queued in the order XGL_DrawAliasEntities consumes them. */
    for (pass = 0; pass < 2; ++pass)
        for (i = 0; i < cl_numvisedicts && xgl_alias_frame->job_count < XGL_ALIAS_FRAME_JOBS; ++i)
            if (XGL_AliasPassEntity(pass, i) && xgl_alias_plan[i] >= 0)
                XGL_ProcessAlias(cl_visedicts[i], i, xgl_alias_plan[i] & 3);
}

static mspriteframe_t *XGL_SpriteFrame(entity_t *entity, msprite_t *sprite)
{
    mspritegroup_t *group;
    double time, cycle, target;
    int frame, i;
    if (sprite->numframes <= 0) return NULL;
    frame = entity->frame;
    if (frame < 0 || frame >= sprite->numframes) frame = 0;
    if (sprite->frames[frame].type == SPR_SINGLE) return sprite->frames[frame].frameptr;
    group = (mspritegroup_t *)sprite->frames[frame].frameptr;
    if (!group || group->numframes <= 0) return NULL;
    cycle = group->intervals[group->numframes - 1];
    if (!(cycle > 0)) return group->frames[0];
    time = cl.time + entity->syncbase;
    target = time - floor(time / cycle) * cycle;
    for (i = 0; i < group->numframes - 1; ++i)
        if (target < group->intervals[i]) break;
    return group->frames[i];
}

static int XGL_SpriteAxes(entity_t *entity, int type, vec3_t right, vec3_t up, vec3_t normal)
{
    vec3_t direction;
    float angle, sr, cr;
    int i;
    if (type == SPR_ORIENTED) {
        AngleVectors(entity->angles, normal, right, up);
        VectorScale(normal, -1, normal);
    } else if (type == SPR_VP_PARALLEL_UPRIGHT || type == SPR_FACING_UPRIGHT) {
        if (type == SPR_FACING_UPRIGHT) {
            VectorSubtract(entity->origin, r_origin, direction);
        } else {
            VectorCopy(vpn, direction);
        }
        direction[2] = 0;
        if (VectorNormalize(direction) < .001f) return 0;
        up[0] = up[1] = 0; up[2] = 1;
        right[0] = direction[1]; right[1] = -direction[0]; right[2] = 0;
        VectorScale(direction, -1, normal);
    } else if (type == SPR_VP_PARALLEL_ORIENTED) {
        angle = entity->angles[ROLL] * (M_PI / 180);
        sr = sin(angle); cr = cos(angle);
        for (i = 0; i < 3; ++i) {
            right[i] = vright[i] * cr + vup[i] * sr;
            up[i] = vright[i] * -sr + vup[i] * cr;
            normal[i] = -vpn[i];
        }
    } else {
        VectorCopy(vright, right);
        VectorCopy(vup, up);
        VectorScale(vpn, -1, normal);
    }
    return 1;
}

static void XGL_SpriteVertex(const mspriteframe_t *frame, const vec3_t origin,
                            const vec3_t right, const vec3_t up, const vec3_t normal,
                            float s, float t, xgl_entity_vertex_t *vertex)
{
    float x, y;
    int i;
    x = frame->left + (frame->right - frame->left) * s;
    y = frame->up + (frame->down - frame->up) * t;
    for (i = 0; i < 3; ++i) {
        vertex->position[i] = origin[i] + right[i] * x + up[i] * y;
        vertex->normal[i] = normal[i];
    }
    /* Keep endpoint samples on the outermost texel rather than wrapping
     * exactly 1.0 back to the opposite edge of a sprite image. */
    vertex->s = (.5f + s * (frame->width - 1)) / frame->width;
    vertex->t = (.5f + t * (frame->height - 1)) / frame->height;
}

static void XGL_DrawSprite(entity_t *entity)
{
    msprite_t *sprite;
    mspriteframe_t *frame;
    xgl_entity_vertex_t corners[4];
    xgl_alias_draw_t draw;
    vec3_t right, up, normal, origin, mins, maxs;
    int x, y, cells, i;
    float s0, s1, t0, t1, radius;
    sprite = (msprite_t *)entity->model->cache.data;
    if (!sprite) return;
    frame = XGL_SpriteFrame(entity, sprite);
    if (!frame || frame->width <= 0 || frame->height <= 0) return;
    if (!XGL_SpriteAxes(entity, sprite->type, right, up, normal)) return;
    VectorMA(entity->origin, sprite->beamlength, normal, origin);
    radius = entity->model->radius;
    if (radius > 0) {
        for (i = 0; i < 3; ++i) {
            mins[i] = origin[i] - radius;
            maxs[i] = origin[i] + radius;
        }
        if (R_CullBox(mins, maxs)) return;
    }
    cells = XGL_EntitySetting(xgl_sprite_cells.value, 2, 16);
    /* Reuse the bounded fallback batch while keeping per-triangle cutout
     * sampling. No sampled colors or geometry survive this sprite draw. */
    draw.cache = NULL; draw.shade = NULL;
    draw.count = draw.index = draw.building = draw.opaque = 0;
    for (y = 0; y < cells; ++y) {
        t0 = (float)y / cells; t1 = (float)(y + 1) / cells;
        for (x = 0; x < cells; ++x) {
            s0 = (float)x / cells; s1 = (float)(x + 1) / cells;
            XGL_SpriteVertex(frame, origin, right, up, normal, s0, t0, &corners[0]);
            XGL_SpriteVertex(frame, origin, right, up, normal, s1, t0, &corners[1]);
            XGL_SpriteVertex(frame, origin, right, up, normal, s1, t1, &corners[2]);
            XGL_SpriteVertex(frame, origin, right, up, normal, s0, t1, &corners[3]);
            XGL_EntityTriangleCached(&corners[0], &corners[1], &corners[2],
                               frame->gl_texturenum, NULL, 1, 0, 0, &draw);
            XGL_EntityTriangleCached(&corners[0], &corners[2], &corners[3],
                               frame->gl_texturenum, NULL, 1, 0, 0, &draw);
        }
    }
    XGL_AliasFlush(&draw, 0);
}

/* Brush entities that a dynamic light reaches are drawn illuminated, with
 * the walls it reaches; the others with the unlit walls. Each entity is
 * drawn by exactly one of the two calls a frame makes. */
void XGL_DrawBrushEntities(int lit)
{
    xgl_profile_draw_scope_t profile;
    int i;
    if (!r_drawentities.value) return;
    for (i = 0; i < cl_numvisedicts; ++i) {
        currententity = cl_visedicts[i];
        if (!currententity || !currententity->model ||
            currententity->model->type != mod_brush) continue;
        if ((XGL_BrushEntityLights(currententity) != 0) != (lit != 0)) continue;
        XGL_ProfileDrawBegin(&profile);
        XGL_DrawBrushModel(currententity);
        XGL_ProfileDrawEnd(&profile, XGL_PROFILE_DRAW_BRUSH);
    }
}

void XGL_DrawSprites(void)
{
    xgl_profile_draw_scope_t profile;
    int i;
    if (!r_drawentities.value) return;
    for (i = 0; i < cl_numvisedicts; ++i) {
        currententity = cl_visedicts[i];
        if (!currententity || !currententity->model ||
            currententity->model->type != mod_sprite) continue;
        XGL_ProfileDrawBegin(&profile);
        XGL_DrawSprite(currententity);
        XGL_ProfileDrawEnd(&profile, XGL_PROFILE_DRAW_SPRITE);
    }
}

void XGL_DrawAliasEntities(void)
{
    xgl_profile_draw_scope_t profile;
    xgl_alias_job_t *job;
    const xgl_color_vertex_t *points;
    int i, j, pass, next_job, plan;
    /* Hit probes never allocate or evict. Fence before normal serial cache
     * accesses by later/fallback draws and before reading worker output. */
    XGL_JoinAliasJobs();
    if (!r_drawentities.value) {
        xgl_alias_planned = xgl_entity_extra_used = 0;
        return;
    }
    if (!xgl_alias_planned) XGL_PlanAliasEntities();
    next_job = 0;
    for (pass = 0; pass < 2; ++pass) {
        for (i = 0; i < cl_numvisedicts; ++i) {
            if (!XGL_AliasPassEntity(pass, i)) continue;
            currententity = cl_visedicts[i];
            plan = xgl_alias_plan[i];
            /* Culled models still count as a visit, as they always have. */
            XGL_ProfileDrawBegin(&profile);
            job = xgl_alias_frame && next_job < xgl_alias_frame->job_count ?
                &xgl_alias_frame->jobs[next_job] : NULL;
            if (plan >= 0 && job && job->visible_index == i) {
                if (job->strips > 0) {
                    points = (const xgl_color_vertex_t *)job->output;
                    for (j = 0; j < job->strips; ++j) {
                        XGL_DrawColorStrip(points, job->lengths[j], 1, 1);
                        points += job->lengths[j];
                    }
                    if (job->count > job->strip_points)
                        XGL_DrawColorTriangles(points, job->count - job->strip_points, 1, 1);
                } else if (job->shade.mode == XGL_SHADE_NATIVE)
                    XGL_DrawNativeTriangles((const xgl_native_vertex_t *)job->output,
                                            job->count, job->lit, 1, 1);
                else XGL_DrawColorTriangles((const xgl_color_vertex_t *)job->output,
                                            job->count, 1, 1);
                c_alias_polys += job->triangles;
                ++next_job;
            } else if (plan >= 0) XGL_ProcessAlias(currententity, -1, plan & 3);
            XGL_ProfileDrawEnd(&profile, XGL_PROFILE_DRAW_ALIAS);
        }
    }
    xgl_alias_planned = 0;
}

void XGL_DrawViewModel(void)
{
    xgl_profile_draw_scope_t profile;
    if (!r_drawviewmodel.value || !r_drawentities.value || chase_active.value ||
        (cl.items & IT_INVISIBILITY) || cl.stats[STAT_HEALTH] <= 0 ||
        !cl.viewent.model || cl.viewent.model->type != mod_alias) return;
    currententity = &cl.viewent;
    /* The weapon joins the other models' batch. XGL_ProcessAlias draws it
     * shrunk toward the eye, in front of the world by depth alone, so the
     * frame needs no second depth clear or completion wait. */
    XGL_ProfileDrawBegin(&profile);
    XGL_DrawAlias(currententity);
    XGL_ProfileDrawEnd(&profile, XGL_PROFILE_DRAW_WEAPON_DRAW);
}
