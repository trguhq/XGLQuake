/* Host-only entity rendering contract tests. No XGL runtime is simulated. */
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
static void *entity_test_malloc(size_t bytes);
static float entity_test_normalize(float vector[3]);
#define VectorNormalize entity_test_normalize
#define malloc entity_test_malloc
#include "../xgl_entities.c"
#undef malloc
#undef VectorNormalize
extern float VectorNormalize(float vector[3]);

client_state_t cl;
entity_t cl_entities[MAX_EDICTS];
entity_t *cl_visedicts[MAX_VISEDICTS];
int cl_numvisedicts;
viddef_t vid;
entity_t *currententity;
vec3_t r_origin, vpn, vright, vup;
int c_alias_polys;
cvar_t r_fullbright, r_drawentities, r_drawviewmodel, chase_active, r_dynamic;
dlight_t cl_dlights[MAX_DLIGHTS];
byte xgl_palette[768];

static xgl_vertex_t captured[4096];
static int captured_count, native_lit, material_seen, coverage_mode;
static int sample_calls, test_light, draw_calls, normalizations;
static int draw_log_lit[64], draw_log_count[64];
static int prepared_draws, brush_lights, brush_draws;
static float test_ambient;
static byte material_pixels[16 * 16];
static xgl_native_vertex_t reserved_vertices[XGL_ENTITY_BATCH_VERTICES];
static int reserved_count, reserved_lit, reserve_calls, prepare_calls;
/* Records submitted without normals are captured with a zero normal and
 * flagged; plain_capture marks the capture in progress as such. */
static xgl_color_vertex_t reserved_plain[XGL_ENTITY_BATCH_VERTICES];
static byte captured_plain[4096];
static int reserved_plain_count, plain_capture, plain_submissions;
/* Triangle strips as submitted: how many, their points, and each one's. */
static int strip_calls, strip_points, strip_log[64];
static int reserve_limit = XGL_ENTITY_BATCH_VERTICES;
static void capture_reserved(void);
static unsigned long profile_counts[XGL_PROFILE_COUNTER_COUNT];
static xgl_profile_draw_scope_t *profile_draw_active;
static xgl_profile_draw_scope_t *profile_draw_stack[4];
static int profile_draw_depth, profile_draw_max_depth;
static int profile_draw_calls[XGL_PROFILE_DRAW_COUNT];
static int profile_sections[XGL_PROFILE_SECTION_COUNT];
static int light_calls;
static vec3_t light_origin;
static unsigned int material_revision;
static int allocation_fail_after = -1;
static int audio_lock_depth, audio_locks, audio_unlocks, expect_alias_lock;
static int test_cull;
static scoreboard_t scores[MAX_SCOREBOARD];
static byte base_colormap[256], player_colormap[256];
static union { double alignment; byte bytes[32768]; } alias_storage;
static model_t alias_model, *evict_on_audio_lock;
static void *evicted_data;

/* Defer work until the renderer waits. This exercises the production worker
 * computation after the main thread has released Quake's movable model. */
static int smp_active, smp_limit, pending_jobs, executed_jobs, native_submissions;
static int worker_running, test_gamma, take_jobs, taken_jobs;
static float gamma_output[256];
static struct {
    void (*run)(void *);
    void *arg;
} deferred[8];

int XGL_SMPActive(void) { return smp_active; }

int XGL_SMPSubmit(void (*run)(void *), void *arg)
{
    assert(!worker_running && run);
    if (!smp_active || pending_jobs >= smp_limit) return 0;
    deferred[pending_jobs].run = run;
    deferred[pending_jobs++].arg = arg;
    return 1;
}

/* The joining thread may reclaim the newest jobs the worker has not begun. */
int XGL_SMPTake(void (**run)(void *), void **arg)
{
    assert(!worker_running && run && arg);
    if (!take_jobs || !pending_jobs) return 0;
    --pending_jobs;
    *run = deferred[pending_jobs].run;
    *arg = deferred[pending_jobs].arg;
    ++taken_jobs;
    return 1;
}

void XGL_SMPWait(void)
{
    int i;
    assert(!worker_running);
    for (i = 0; i < pending_jobs; ++i) {
        worker_running = 1;
        deferred[i].run(deferred[i].arg);
        worker_running = 0;
        ++executed_jobs;
    }
    pending_jobs = 0;
}

void XGL_CopyGammaOutput(float out[256])
{
    assert(!worker_running);
    memcpy(out, gamma_output, sizeof(gamma_output));
}

static void *entity_test_malloc(size_t bytes)
{
    if (allocation_fail_after == 0) return NULL;
    if (allocation_fail_after > 0) --allocation_fail_after;
    return malloc(bytes);
}

static float entity_test_normalize(float vector[3])
{
    ++normalizations;
    return VectorNormalize(vector);
}

double XGL_ProfileDetailNow(void) { return 0; }
void XGL_ProfileAdd(xgl_profile_section_t section, double start)
{
    assert((unsigned int)section < XGL_PROFILE_SECTION_COUNT);
    assert(start == 0);
    ++profile_sections[section];
}
void XGL_ProfileCount(xgl_profile_counter_t counter, unsigned long count)
{
    assert((unsigned int)counter < XGL_PROFILE_COUNTER_COUNT);
    profile_counts[counter] += count;
}
void XGL_ProfileDrawBegin(xgl_profile_draw_scope_t *scope)
{
    assert(profile_draw_depth < 4);
    profile_draw_stack[profile_draw_depth++] = scope;
    profile_draw_active = scope;
    if (profile_draw_depth > profile_draw_max_depth)
        profile_draw_max_depth = profile_draw_depth;
}
void XGL_ProfileDrawEnd(xgl_profile_draw_scope_t *scope, xgl_profile_draw_t draw)
{
    assert(profile_draw_depth > 0 && profile_draw_active == scope);
    assert(profile_draw_stack[profile_draw_depth - 1] == scope);
    assert((unsigned int)draw < XGL_PROFILE_DRAW_COUNT);
    ++profile_draw_calls[draw];
    --profile_draw_depth;
    profile_draw_active = profile_draw_depth ? profile_draw_stack[profile_draw_depth - 1] : NULL;
}
void Cvar_RegisterVariable(cvar_t *variable) { (void)variable; }
void Con_DPrintf(char *format, ...) { (void)format; }
void Sys_Error(char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    abort();
}
void XGL_AudioLock(void)
{
    ++audio_lock_depth;
    ++audio_locks;
    if (evict_on_audio_lock && !evicted_data) {
        evicted_data = evict_on_audio_lock->cache.data;
        assert(evicted_data);
        evict_on_audio_lock->cache.data = NULL;
    }
}
void XGL_AudioUnlock(void)
{
    assert(audio_lock_depth > 0);
    capture_reserved();
    --audio_lock_depth; ++audio_unlocks;
}
void *Mod_Extradata(model_t *model)
{
    assert(audio_lock_depth > 0);
    expect_alias_lock = 1;
    if (model == evict_on_audio_lock && evicted_data) {
        model->cache.data = evicted_data;
        evicted_data = NULL;
        evict_on_audio_lock = NULL;
    }
    return model->cache.data;
}
qboolean R_CullBox(vec3_t mins, vec3_t maxs) { (void)mins; (void)maxs; return test_cull; }
int R_LightPoint(vec3_t point)
{
    ++light_calls;
    VectorCopy(point, light_origin);
    return test_light;
}
unsigned int XGL_TextureRevision(int material) { return material > 0 ? material_revision : 0; }
qboolean XGL_TextureHasAlpha(int material) { (void)material; return coverage_mode != 0; }
void XGL_DrawBrushModel(entity_t *entity) { (void)entity; ++brush_draws; }
float XGL_AmbientLight(void) { return test_ambient; }
int XGL_BrushEntityLights(entity_t *entity) { (void)entity; return brush_lights; }

/* The frame's entity order: brush models in reach of a dynamic light, the
 * other brush models, sprites, then alias models. */
static void draw_entities(void)
{
    XGL_DrawBrushEntities(1);
    XGL_DrawBrushEntities(0);
    XGL_DrawSprites();
    XGL_DrawAliasEntities();
}
void XGL_DrawTriangles(const xgl_vertex_t *vertices, int count, int lit,
                       float opacity, int zwrite)
{
    int i;
    if (expect_alias_lock) assert(audio_lock_depth > 0);
    assert(count > 0 && count % 3 == 0);
    assert(captured_count + count <= 4096);
    assert(opacity == 1 && zwrite == 1);
    native_lit = lit;
    if (draw_calls < 64) {
        draw_log_lit[draw_calls] = lit;
        draw_log_count[draw_calls] = count;
    }
    ++draw_calls;
    for (i = 0; i < count; ++i) {
        if (plain_capture) assert(!lit && !DotProduct(vertices[i].normal, vertices[i].normal));
        else assert(fabs(DotProduct(vertices[i].normal, vertices[i].normal) - 1) < .0001);
        captured_plain[captured_count] = (byte)plain_capture;
        captured[captured_count++] = vertices[i];
    }
}
/* CPU-shaded fallback triangles carry final colors and are always unlit. */
void XGL_DrawTrianglesStrided(const xgl_vertex_t *vertices, int count, size_t stride,
                              int lit, float opacity, int zwrite, int prepared_color)
{
    assert(stride == sizeof(*vertices) && prepared_color == 1 && lit == 0);
    ++prepared_draws;
    XGL_DrawTriangles(vertices, count, lit, opacity, zwrite);
}
static void capture_reserved(void)
{
    xgl_vertex_t vertices[XGL_ENTITY_BATCH_VERTICES];
    int i, count;
    count = reserved_plain_count;
    if (count) {
        assert(!reserved_count);
        reserved_plain_count = 0;
        memset(vertices, 0, sizeof(vertices));
        for (i = 0; i < count; ++i) {
            VectorCopy(reserved_plain[i].position, vertices[i].position);
            VectorCopy(reserved_plain[i].color.rgb, vertices[i].color);
        }
        plain_capture = 1;
        XGL_DrawTriangles(vertices, count, 0, 1, 1);
        plain_capture = 0;
        return;
    }
    count = reserved_count;
    if (!count) return;
    reserved_count = 0;
    for (i = 0; i < count; ++i) {
        VectorCopy(reserved_vertices[i].position, vertices[i].position);
        VectorCopy(reserved_vertices[i].normal, vertices[i].normal);
        VectorCopy(reserved_vertices[i].color.rgb, vertices[i].color);
    }
    XGL_DrawTriangles(vertices, count, reserved_lit, 1, 1);
}

xgl_color_vertex_t *XGL_ReserveColorTriangles(int *count, float opacity, int zwrite)
{
    capture_reserved();
    assert(*count > 0 && *count % 3 == 0 && opacity == 1 && zwrite == 1);
    if (expect_alias_lock) assert(audio_lock_depth > 0);
    if (*count > reserve_limit) *count = reserve_limit;
    reserved_plain_count = *count;
    ++reserve_calls;
    return reserved_plain;
}

xgl_native_vertex_t *XGL_ReserveTriangles(int *count, int lit, float opacity, int zwrite)
{
    capture_reserved();
    assert(*count > 0 && *count % 3 == 0 && opacity == 1 && zwrite == 1);
    if (expect_alias_lock) assert(audio_lock_depth > 0);
    if (*count > reserve_limit) *count = reserve_limit;
    reserved_count = *count; reserved_lit = lit;
    ++reserve_calls;
    return reserved_vertices;
}

void XGL_PrepareColor(float rgb[3])
{
    int i;
    assert(!worker_running);
    if (test_gamma)
        for (i = 0; i < 3; ++i)
            rgb[i] = gamma_output[(int)(rgb[i] * 255.0f + .5f)];
    ++prepare_calls;
}

qboolean XGL_SampleTextureIndex(int material, float s, float t, int *result)
{
    int x, y, index;
    ++sample_calls;
    material_seen = material;
    if (!(s > -1000000000.0f && s < 1000000000.0f &&
          t > -1000000000.0f && t < 1000000000.0f)) return false;
    s -= (float)(int)s; t -= (float)(int)t;
    if (s < 0) s += 1;
    if (t < 0) t += 1;
    x = (int)(s * 16); y = (int)(t * 16);
    if (x > 15) x = 15;
    if (y > 15) y = 15;
    index = material_pixels[y * 16 + x];
    if (coverage_mode == 1 || (coverage_mode == 2 && s < .5f)) index = 255;
    if (coverage_mode && index == 255) return false;
    *result = index;
    return true;
}

void XGL_DrawNativeTriangles(const xgl_native_vertex_t *vertices, int count,
                             int lit, float opacity, int zwrite)
{
    int i;
    assert(!worker_running && count > 0 && count % 3 == 0);
    assert(opacity == 1 && zwrite == 1);
    capture_reserved();
    assert(captured_count + count <= 4096);
    for (i = 0; i < count; ++i) {
        VectorCopy(vertices[i].position, captured[captured_count].position);
        VectorCopy(vertices[i].normal, captured[captured_count].normal);
        VectorCopy(vertices[i].color.rgb, captured[captured_count].color);
        ++captured_count;
    }
    native_lit = lit;
    ++native_submissions;
}

void XGL_DrawColorTriangles(const xgl_color_vertex_t *vertices, int count,
                            float opacity, int zwrite)
{
    int i;
    assert(!worker_running && count > 0 && count % 3 == 0);
    assert(opacity == 1 && zwrite == 1);
    capture_reserved();
    assert(captured_count + count <= 4096);
    for (i = 0; i < count; ++i) {
        memset(&captured[captured_count], 0, sizeof(captured[0]));
        VectorCopy(vertices[i].position, captured[captured_count].position);
        VectorCopy(vertices[i].color.rgb, captured[captured_count].color);
        captured_plain[captured_count++] = 1;
    }
    native_lit = 0;
    ++native_submissions; ++plain_submissions;
}

/* A strip is captured as the triangles it draws: each point after the
 * second with the two before it. */
void XGL_DrawColorStrip(const xgl_color_vertex_t *points, int count,
                        float opacity, int zwrite)
{
    int i, j;
    assert(!worker_running && count >= 3 && count <= XGL_ALIAS_STRIP_POINTS);
    assert(opacity == 1 && zwrite == 1);
    capture_reserved();
    assert(captured_count + 3 * (count - 2) <= 4096);
    for (i = 0; i + 2 < count; ++i)
        for (j = 0; j < 3; ++j) {
            memset(&captured[captured_count], 0, sizeof(captured[0]));
            VectorCopy(points[i + j].position, captured[captured_count].position);
            VectorCopy(points[i + j].color.rgb, captured[captured_count].color);
            captured_plain[captured_count++] = 1;
        }
    if (strip_calls < 64) strip_log[strip_calls] = count;
    ++strip_calls; strip_points += count;
    native_lit = 0;
}

static void reset_test(void)
{
    int i;
    assert(!audio_lock_depth && audio_locks == audio_unlocks);
    assert(!pending_jobs && !worker_running);
    test_gamma = smp_active = executed_jobs = native_submissions = 0;
    take_jobs = taken_jobs = 0;
    smp_limit = sizeof(deferred) / sizeof(deferred[0]);
    memset(&cl, 0, sizeof(cl));
    memset(cl_entities, 0, sizeof(cl_entities));
    memset(&vid, 0, sizeof(vid));
    memset(scores, 0, sizeof(scores));
    memset(r_origin, 0, sizeof(r_origin));
    memset(vpn, 0, sizeof(vpn)); vpn[0] = 1;
    memset(vright, 0, sizeof(vright)); vright[1] = -1;
    memset(vup, 0, sizeof(vup)); vup[2] = 1;
    cl.scores = scores;
    cl.maxclients = 1;
    vid.colormap = base_colormap;
    r_fullbright.value = chase_active.value = 0;
    r_drawentities.value = r_drawviewmodel.value = 1;
    xgl_alias_subdivide.value = 0;
    xgl_sprite_cells.value = 2;
    xgl_entity_triangles.value = 2048;
    xgl_playercolors.value = 1;
    /* Most contracts were written against native illumination, which also
     * outputs the world normals they check. CPU shading has its own test. */
    xgl_alias_shade.value = 0;
    /* Models go out as separate triangles except in the tests of strips. */
    xgl_alias_strips.value = 0;
    strip_calls = strip_points = 0;
    test_ambient = 1; r_dynamic.value = 1;
    memset(cl_dlights, 0, sizeof(cl_dlights));
    prepared_draws = brush_lights = brush_draws = 0;
    /* The backend starts every batch record with a valid normal. */
    for (i = 0; i < XGL_ENTITY_BATCH_VERTICES; ++i) {
        reserved_vertices[i].normal[0] = reserved_vertices[i].normal[1] = 0;
        reserved_vertices[i].normal[2] = 1;
    }
    xgl_entity_extra_used = 0;
    captured_count = native_lit = material_seen = coverage_mode = 0;
    sample_calls = draw_calls = normalizations = 0; test_light = 128; material_revision = 1;
    assert(!reserved_count && !reserved_plain_count);
    reserved_lit = reserve_calls = prepare_calls = plain_submissions = 0;
    reserve_limit = XGL_ENTITY_BATCH_VERTICES;
    memset(profile_counts, 0, sizeof(profile_counts));
    assert(!profile_draw_active && !profile_draw_depth);
    profile_draw_max_depth = 0;
    memset(profile_draw_calls, 0, sizeof(profile_draw_calls));
    memset(profile_sections, 0, sizeof(profile_sections));
    light_calls = 0;
    memset(light_origin, 0, sizeof(light_origin));
    /* A repeating 16x16 indexed fixture: UV (.1,.2) samples TOP_RANGE.
     * Distinct palette channels expose player translation and UV changes. */
    for (i = 0; i < 256; ++i) {
        material_pixels[i] = (byte)(i + 223);
        xgl_palette[i * 3] = (byte)(i * 37 + 31);
        xgl_palette[i * 3 + 1] = (byte)(255 - i);
        xgl_palette[i * 3 + 2] = (byte)i;
    }
    allocation_fail_after = -1;
    audio_locks = audio_unlocks = expect_alias_lock = test_cull = 0;
    XGL_EntitiesInit();
}

static aliashdr_t *make_alias(int count)
{
    aliashdr_t *header;
    trivertx_t *poses;
    int *commands;
    int i, vertices;
    float uv[2];
    memset(&alias_storage, 0, sizeof(alias_storage));
    memset(&alias_model, 0, sizeof(alias_model));
    header = (aliashdr_t *)alias_storage.bytes;
    header->scale[0] = header->scale[1] = header->scale[2] = 1;
    vertices = count < 0 ? -count : count;
    header->numposes = 2;
    header->poseverts = vertices;
    header->numtris = vertices - 2;
    header->numframes = header->numskins = 1;
    header->frames[0].firstpose = 0;
    header->frames[0].numposes = 2;
    header->frames[0].interval = .1f;
    header->gl_texturenum[0][0] = 10;
    header->gl_texturenum[0][1] = 11;
    header->gl_texturenum[0][2] = 12;
    header->gl_texturenum[0][3] = 13;
    header->commands = sizeof(*header);
    commands = (int *)((byte *)header + header->commands);
    commands[0] = count;
    for (i = 0; i < vertices; ++i) {
        uv[0] = .1f * (i + 1); uv[1] = .2f * (i + 1);
        memcpy(&commands[1 + 2 * i], uv, sizeof(uv));
    }
    commands[1 + 2 * vertices] = 0;
    header->posedata = header->commands + (2 + 2 * vertices) * sizeof(int);
    poses = (trivertx_t *)((byte *)header + header->posedata);
    for (i = 0; i < 2 * vertices; ++i) poses[i].lightnormalindex = 5;
    strcpy(alias_model.name, "progs/player.mdl");
    alias_model.type = mod_alias;
    alias_model.radius = 32;
    alias_model.cache.data = header;
    cl_entities[1].model = &alias_model;
    cl_entities[1].colormap = player_colormap;
    return header;
}

static void test_alias_pose_transform_translation(void)
{
    aliashdr_t *header;
    trivertx_t *poses;
    entity_t *entity;
    reset_test();
    header = make_alias(3);
    poses = (trivertx_t *)((byte *)header + header->posedata);
    header->scale[0] = 1; header->scale[1] = 2; header->scale[2] = 3;
    header->scale_origin[0] = 5; header->scale_origin[1] = 6; header->scale_origin[2] = 7;
    poses[3].v[0] = 10;
    poses[4].v[0] = 11;
    poses[5].v[0] = 10; poses[5].v[1] = 1;
    entity = &cl_entities[1];
    entity->origin[0] = 100; entity->origin[1] = 200; entity->origin[2] = 300;
    entity->angles[YAW] = 90;
    cl.time = .15;
    scores[0].colors = 0xe1;
    XGL_DrawAlias(entity);
    assert(captured_count == 3 && native_lit == 2 && material_seen == 11);
    assert(fabs(captured[0].position[0] - 94) < .0001);
    assert(fabs(captured[0].position[1] - 215) < .0001);
    assert(fabs(captured[0].position[2] - 307) < .0001);
    assert(fabs(captured[0].color[0] - xgl_palette[239 * 3] / 255.0f) < .0001);
    assert(fabs(captured[0].color[2] - 239.0 / 255) < .0001);
    assert(xgl_player_translation[0][TOP_RANGE + 15] == 224);
    assert(xgl_player_translation[0][BOTTOM_RANGE] == 16);
    assert(xgl_player_translation[0][BOTTOM_RANGE + 15] == 31);
    assert(xgl_player_translation[0][255] == 255);
    assert(entity->angles[PITCH] == 0 && entity->angles[YAW] == 90);
    captured_count = 0;
    xgl_alias_subdivide.value = 2;
    xgl_entity_triangles.value = 0;
    XGL_DrawAlias(entity);
    assert(captured_count == 3); /* exhausted detail budget retains base mesh */
    captured_count = 0;
    xgl_alias_subdivide.value = 1;
    xgl_entity_triangles.value = 2048;
    XGL_DrawAlias(entity);
    assert(captured_count == 12 && xgl_entity_extra_used == 3);
}

static void test_alias_primitive_winding(void)
{
    aliashdr_t *header;
    trivertx_t *poses;
    vec3_t ab, ac, normal;
    int fan, triangle;
    for (fan = 0; fan < 2; ++fan) {
        reset_test();
        header = make_alias(fan ? -4 : 4);
        header->frames[0].numposes = 1;
        poses = (trivertx_t *)((byte *)header + header->posedata);
        poses[1].v[0] = 1;
        poses[2].v[0] = fan ? 1 : 0; poses[2].v[1] = 1;
        poses[3].v[0] = fan ? 0 : 1; poses[3].v[1] = 1;
        XGL_DrawAlias(&cl_entities[1]);
        assert(captured_count == 6);
        for (triangle = 0; triangle < 2; ++triangle) {
            VectorSubtract(captured[triangle * 3 + 1].position, captured[triangle * 3].position, ab);
            VectorSubtract(captured[triangle * 3 + 2].position, captured[triangle * 3].position, ac);
            CrossProduct(ab, ac, normal);
            assert(normal[2] > .99f);
        }
    }
}

static void test_sprite_groups_coverage_axes(void)
{
    msprite_t sprite;
    mspriteframe_t frames[2];
    struct { int numframes; float *intervals; mspriteframe_t *frames[2]; } group;
    float intervals[2];
    entity_t entity;
    model_t model;
    int i, type, cells, coverage, triangles;
    reset_test();
    memset(&sprite, 0, sizeof(sprite));
    memset(frames, 0, sizeof(frames));
    memset(&entity, 0, sizeof(entity));
    memset(&model, 0, sizeof(model));
    for (i = 0; i < 2; ++i) {
        frames[i].width = frames[i].height = 8;
        frames[i].left = frames[i].down = -4;
        frames[i].right = frames[i].up = 4;
        frames[i].gl_texturenum = 20 + i;
        group.frames[i] = &frames[i];
    }
    intervals[0] = .1f; intervals[1] = .3f;
    group.numframes = 2; group.intervals = intervals;
    sprite.numframes = 1;
    sprite.frames[0].type = SPR_GROUP;
    sprite.frames[0].frameptr = (mspriteframe_t *)&group;
    model.cache.data = &sprite; model.type = mod_sprite;
    entity.model = &model; entity.origin[0] = 10;
    cl.time = .2;
    for (type = SPR_VP_PARALLEL_UPRIGHT; type <= SPR_VP_PARALLEL_ORIENTED; ++type) {
        sprite.type = type;
        captured_count = draw_calls = 0;
        XGL_DrawSprite(&entity);
        assert(captured_count == 24 && native_lit == 0 && material_seen == 21);
        assert(draw_calls == 1);
        for (i = 0; i < captured_count; ++i) {
            assert(fabs(captured[i].position[0] - 10) < .0001);
            assert(fabs(captured[i].position[1]) <= 4 && fabs(captured[i].position[2]) <= 4);
        }
    }
    coverage_mode = 1; captured_count = 0;
    XGL_DrawSprite(&entity);
    assert(captured_count == 0);
    coverage_mode = 2; captured_count = 0;
    XGL_DrawSprite(&entity);
    assert(captured_count == 12);
    for (cells = 8; cells <= 16; cells += 8) {
        xgl_sprite_cells.value = (float)cells;
        triangles = cells * cells * 2;
        for (coverage = 0; coverage <= 2; ++coverage) {
            coverage_mode = coverage;
            captured_count = draw_calls = sample_calls = 0;
            XGL_DrawSprite(&entity);
            if (coverage == 1) {
                assert(!captured_count && !draw_calls);
                assert(sample_calls == triangles);
            } else {
                assert(captured_count == triangles * 3 / (coverage ? 2 : 1));
                assert(draw_calls == (captured_count + XGL_ENTITY_BATCH_VERTICES - 1) /
                                     XGL_ENTITY_BATCH_VERTICES);
                assert(sample_calls == triangles + captured_count);
            }
        }
    }
}

static void test_view_weapon_scope(void)
{
    xgl_vertex_t world[3];
    aliashdr_t *header;
    trivertx_t *poses;
    int i, j, pass;
    reset_test();
    header = make_alias(3);
    header->frames[0].numposes = 1;
    poses = (trivertx_t *)((byte *)header + header->posedata);
    poses[0].v[0] = 40; poses[1].v[0] = 40; poses[1].v[1] = 16;
    poses[2].v[0] = 24; poses[2].v[2] = 8;
    /* The same model as an ordinary entity gives the unshrunk positions. */
    cl_entities[1].origin[0] = 100; cl_entities[1].origin[1] = -50;
    cl_entities[1].origin[2] = 20; cl_entities[1].angles[YAW] = 30;
    XGL_DrawAlias(&cl_entities[1]);
    assert(captured_count == 3);
    memcpy(world, captured, sizeof(world));
    cl.viewent = cl_entities[1];
    cl.viewent.colormap = vid.colormap;
    r_origin[0] = 96; r_origin[1] = -48; r_origin[2] = 42;
    cl.stats[STAT_HEALTH] = 100;
    /* Fallback, then cached: both draw the weapon shrunk toward the eye, so
     * every vertex stays on its original line of sight (the same image) at
     * one eighth of the distance, in front of the world by depth alone. */
    for (pass = 0; pass < 2; ++pass) {
        captured_count = 0;
        XGL_DrawViewModel();
        assert(captured_count == 3 && native_lit == 2);
        for (i = 0; i < 3; ++i) {
            for (j = 0; j < 3; ++j) {
                assert(fabs(captured[i].position[j] - (r_origin[j] +
                    (world[i].position[j] - r_origin[j]) * XGL_VIEWMODEL_SCALE)) < .0001);
                assert(fabs(captured[i].normal[j] - world[i].normal[j]) < .0001);
            }
        }
    }
    assert(profile_draw_calls[XGL_PROFILE_DRAW_WEAPON_FALLBACK] == 1);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_WEAPON_OUTPUT] == 1);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_WEAPON_DRAW] == 2);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_ALIAS] == 0 && !profile_draw_active);
    captured_count = 0;
    cl.items = IT_INVISIBILITY;
    XGL_DrawViewModel();
    assert(!captured_count);
    cl.items = 0; cl.stats[STAT_HEALTH] = 0;
    XGL_DrawViewModel();
    assert(!captured_count);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_WEAPON_DRAW] == 2);
    assert(!profile_draw_active);
}

static void test_entity_profile_categories(void)
{
    model_t brush, sprite;
    reset_test();
    make_alias(3);
    memset(&brush, 0, sizeof(brush)); brush.type = mod_brush;
    memset(&sprite, 0, sizeof(sprite)); sprite.type = mod_sprite;
    cl_entities[2].model = &brush;
    cl_entities[3].model = &sprite;
    cl_visedicts[0] = &cl_entities[1];
    cl_visedicts[1] = &cl_entities[2];
    cl_visedicts[2] = &cl_entities[3];
    cl_visedicts[3] = NULL;
    cl_visedicts[4] = &cl_entities[4];
    cl_numvisedicts = 5;
    draw_entities();
    assert(profile_draw_calls[XGL_PROFILE_DRAW_ALIAS] == 1);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_BRUSH] == 1);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_SPRITE] == 1);
    assert(!profile_draw_calls[XGL_PROFILE_DRAW_WEAPON_DRAW] && !profile_draw_active);
    /* Call counts include dispatches culled or rejected by the renderer. */
    test_cull = 1;
    draw_entities();
    assert(profile_draw_calls[XGL_PROFILE_DRAW_ALIAS] == 2);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_BRUSH] == 2);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_SPRITE] == 2);
    r_drawentities.value = 0;
    draw_entities();
    assert(profile_draw_calls[XGL_PROFILE_DRAW_ALIAS] == 2 && !profile_draw_active);
    cl_numvisedicts = 0;
}

static void test_alias_lighting_selection(void)
{
    aliashdr_t *header;
    int mode, pass, lit;
    float expected;
    for (mode = 0; mode < 4; ++mode) {
        reset_test();
        header = make_alias(3);
        header->frames[0].numposes = 1;
        if (mode == 1) strcpy(alias_model.name, "progs/flame.mdl");
        else if (mode == 2) strcpy(alias_model.name, "progs/flame2.mdl");
        else strcpy(alias_model.name, "progs/test.mdl");
        r_fullbright.value = mode == 3;
        test_light = 64;
        lit = mode == 0;
        expected = xgl_palette[0] * (1.0f / 255.0f) * (lit ? .5f : 1);
        for (pass = 0; pass < 2; ++pass) {
            captured_count = 0;
            XGL_DrawAlias(&cl_entities[1]);
            assert(captured_count == 3 && native_lit == (lit ? 2 : 0));
            assert(fabs(captured[0].color[0] - expected) < .0001);
            assert(light_calls == (lit ? pass + 1 : 0));
            assert(profile_sections[XGL_PROFILE_ALIAS_LIGHT] == light_calls);
            assert(!profile_sections[XGL_PROFILE_WEAPON_LIGHT]);
            assert(!profile_draw_depth);
        }
        assert(profile_draw_calls[XGL_PROFILE_DRAW_ALIAS_FALLBACK] == 1);
        assert(profile_draw_calls[XGL_PROFILE_DRAW_ALIAS_OUTPUT] == 1);
    }
    /* Ordinary and translated player lighting keep their dark floors. */
    reset_test();
    make_alias(3);
    cl_entities[1].colormap = vid.colormap;
    test_light = 0;
    XGL_DrawAlias(&cl_entities[1]);
    assert(light_calls == 1 && native_lit == 2);
    assert(fabs(captured[0].color[0] - xgl_palette[TOP_RANGE * 3] * (1.0f / 255.0f) * .04f) < .0001);
    cl_entities[1].colormap = player_colormap;
    captured_count = 0;
    XGL_DrawAlias(&cl_entities[1]);
    assert(light_calls == 2 && fabs(captured[0].color[0] - xgl_palette[0] * (1.0f / 255.0f) * .08f) < .0001);
}

static void test_nested_alias_profile_paths(void)
{
    aliashdr_t *header;
    int *commands;
    reset_test();
    header = make_alias(3);
    cl_visedicts[0] = &cl_entities[1];
    cl_numvisedicts = 1;
    draw_entities();
    assert(profile_draw_calls[XGL_PROFILE_DRAW_ALIAS] == 1);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_ALIAS_FALLBACK] == 1);
    assert(!profile_draw_calls[XGL_PROFILE_DRAW_ALIAS_OUTPUT]);
    captured_count = 0;
    draw_entities();
    assert(profile_draw_calls[XGL_PROFILE_DRAW_ALIAS] == 2);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_ALIAS_FALLBACK] == 1);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_ALIAS_OUTPUT] == 1);
    commands = (int *)((byte *)header + header->commands);
    commands[0] = 4;
    captured_count = 0;
    draw_entities();
    assert(!captured_count && !audio_lock_depth);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_ALIAS] == 3);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_ALIAS_FALLBACK] == 2);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_ALIAS_OUTPUT] == 1);
    assert(profile_draw_max_depth == 2 && !profile_draw_depth);
    cl_numvisedicts = 0;
}

static void test_nested_weapon_profile_paths(void)
{
    aliashdr_t *header;
    int *commands;
    reset_test();
    header = make_alias(3);
    cl.viewent.model = &alias_model;
    cl.viewent.origin[0] = 7; cl.viewent.origin[1] = 11; cl.viewent.origin[2] = 13;
    cl.stats[STAT_HEALTH] = 100;
    test_light = 0;
    XGL_DrawViewModel();
    assert(profile_draw_calls[XGL_PROFILE_DRAW_WEAPON_FALLBACK] == 1);
    assert(!profile_draw_calls[XGL_PROFILE_DRAW_WEAPON_OUTPUT]);
    assert(light_calls == 1 && native_lit == 2);
    assert(fabs(captured[0].color[0] - xgl_palette[TOP_RANGE * 3] * (1.0f / 255.0f) * .20f) < .0001);
    assert(light_origin[0] == 7 && light_origin[1] == 11 && light_origin[2] == 13);
    captured_count = 0;
    XGL_DrawViewModel();
    assert(captured_count == 3 && fabs(captured[0].color[0] - xgl_palette[TOP_RANGE * 3] * (1.0f / 255.0f) * .20f) < .0001);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_WEAPON_FALLBACK] == 1);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_WEAPON_OUTPUT] == 1);
    commands = (int *)((byte *)header + header->commands);
    commands[0] = 4;
    captured_count = 0;
    XGL_DrawViewModel();
    assert(!captured_count && !audio_lock_depth);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_WEAPON_FALLBACK] == 2);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_WEAPON_OUTPUT] == 1);
    assert(profile_draw_calls[XGL_PROFILE_DRAW_WEAPON_DRAW] == 3);
    assert(!profile_draw_calls[XGL_PROFILE_DRAW_ALIAS_OUTPUT]);
    assert(!profile_draw_calls[XGL_PROFILE_DRAW_ALIAS_FALLBACK]);
    assert(profile_sections[XGL_PROFILE_WEAPON_LIGHT] == 3 && light_calls == 3);
    assert(!profile_sections[XGL_PROFILE_ALIAS_LIGHT]);
    assert(profile_draw_max_depth == 2 && !profile_draw_depth);
}

static void test_alias_color_cache(void)
{
    aliashdr_t *header, *moved;
    trivertx_t *poses;
    union { double alignment; byte bytes[32768]; } moved_storage;
    xgl_vertex_t previous;
    float changed_uv;
    int *commands;
    int i, sampled;
    reset_test();
    header = make_alias(3);
    for (i = 0; i < 4; ++i) header->gl_texturenum[0][i] = 10;
    poses = (trivertx_t *)((byte *)header + header->posedata);
    poses[3].v[0] = 10;
    scores[0].colors = 0x21;
    XGL_DrawAlias(&cl_entities[1]);
    assert(sample_calls == 3); /* no centroid test for an opaque alias skin */
    previous = captured[0];
    captured_count = 0; sample_calls = 0;
    cl.time = .15;
    XGL_DrawAlias(&cl_entities[1]);
    assert(sample_calls == 0 && captured_count == 3);
    assert(captured[0].position[0] == previous.position[0] + 10);
    assert(captured[0].color[0] == previous.color[0]);

    /* Quake may move a cached model. No retained header/command pointers
     * may be required to reuse skin colors or animate the new allocation. */
    memcpy(moved_storage.bytes, alias_storage.bytes, sizeof(alias_storage.bytes));
    moved = (aliashdr_t *)moved_storage.bytes;
    alias_model.cache.data = moved;
    captured_count = 0; sample_calls = 0; test_light = 64;
    XGL_DrawAlias(&cl_entities[1]);
    assert(sample_calls == 0 && captured_count == 3);
    assert(fabs(captured[0].color[0] - previous.color[0] * .5f) < .0001);
    assert(captured[0].position[0] == previous.position[0] + 10);

    captured_count = 0; sample_calls = 0;
    moved->gl_texturenum[0][1] = 11;
    XGL_DrawAlias(&cl_entities[1]);
    assert(sample_calls == 3 && material_seen == 11);
    captured_count = 0; sample_calls = 0;
    moved->gl_texturenum[0][1] = 10;
    XGL_DrawAlias(&cl_entities[1]);
    assert(sample_calls == 0 && captured_count == 3);

    captured_count = 0; sample_calls = 0; test_light = 128;
    scores[0].colors = 0x41;
    XGL_DrawAlias(&cl_entities[1]);
    assert(sample_calls == 3);
    assert(fabs(captured[0].color[2] - 64.0 / 255) < .0001);
    captured_count = 0; sample_calls = 0; xgl_playercolors.value = 0;
    XGL_DrawAlias(&cl_entities[1]);
    assert(sample_calls == 3 && fabs(captured[0].color[2] - TOP_RANGE / 255.0f) < .0001);
    captured_count = 0; sample_calls = 0; xgl_playercolors.value = 1;
    XGL_DrawAlias(&cl_entities[1]);
    assert(sample_calls == 0 && fabs(captured[0].color[2] - 64.0 / 255) < .0001);
    captured_count = 0; sample_calls = 0; ++material_revision;
    XGL_DrawAlias(&cl_entities[1]);
    assert(sample_calls == 3);

    /* A recycled model slot can have different UV commands with the same
     * material ID and revision. Owned command bytes distinguish that case. */
    commands = (int *)((byte *)moved + moved->commands);
    changed_uv = .75f;
    memcpy(commands + 1, &changed_uv, sizeof(changed_uv));
    captured_count = 0; sample_calls = 0;
    XGL_DrawAlias(&cl_entities[1]);
    assert(sample_calls == 3 && fabs(captured[0].color[0] - xgl_palette[75 * 3] / 255.0f) < .0001);

    captured_count = 0; sample_calls = 0;
    xgl_alias_subdivide.value = 1;
    XGL_DrawAlias(&cl_entities[1]);
    sampled = sample_calls;
    assert(sampled == 12 && captured_count == 12);
    captured_count = 0; sample_calls = 0;
    XGL_DrawAlias(&cl_entities[1]);
    assert(!sample_calls && captured_count == 12);
    assert(xgl_alias_cache_bytes > sizeof(xgl_alias_cache));
    assert(xgl_alias_cache_bytes <= XGL_ALIAS_CACHE_BYTES);
    XGL_EntitiesShutdown();
    assert(xgl_alias_cache_bytes == sizeof(xgl_alias_cache));
}

static void test_alias_cache_coverage_and_capacity(void)
{
    aliashdr_t *header, *large;
    xgl_alias_cache_t *cache;
    int *commands;
    int i, sampled, entries;
    size_t old_bytes;
    reset_test();
    header = make_alias(3);
    coverage_mode = 1;
    XGL_DrawAlias(&cl_entities[1]);
    assert(captured_count == 0 && sample_calls == 1);
    sample_calls = 0;
    XGL_DrawAlias(&cl_entities[1]);
    assert(captured_count == 0 && sample_calls == 0);
    ++material_revision; coverage_mode = 0;
    XGL_DrawAlias(&cl_entities[1]);
    assert(captured_count == 3 && sample_calls == 3);

    /* Exhaust entry slots using different materials; eviction must remain
     * bounded, and evicted colors must be sampled again when revisited. */
    for (i = 0; i < XGL_ALIAS_CACHE_SLOTS + 2; ++i) {
        captured_count = 0;
        header->gl_texturenum[0][0] = 100 + i;
        XGL_DrawAlias(&cl_entities[1]);
        assert(captured_count == 3 && xgl_alias_cache_bytes <= XGL_ALIAS_CACHE_BYTES);
    }
    sampled = sample_calls;
    captured_count = 0;
    header->gl_texturenum[0][0] = 10;
    XGL_DrawAlias(&cl_entities[1]);
    assert(sample_calls == sampled + 3 && captured_count == 3);

    /* Large valid models still force byte-budget eviction before slots
     * fill. Compact indices also allow the largest valid model to fit. */
    large = (aliashdr_t *)calloc(1, sizeof(*large) +
                                (2 + (MAXALIASTRIS + 2) * 2) * sizeof(int));
    assert(large);
    large->commands = sizeof(*large);
    large->poseverts = 514; large->numtris = 512;
    commands = (int *)((byte *)large + large->commands);
    commands[0] = large->poseverts;
    XGL_EntitiesShutdown();
    for (i = 0; i < 10; ++i) {
        cache = XGL_AliasColors(large, 200 + i, 2, NULL);
        assert(cache && xgl_alias_cache_bytes <= XGL_ALIAS_CACHE_BYTES);
    }
    entries = 0;
    for (i = 0; i < XGL_ALIAS_CACHE_SLOTS; ++i)
        if (xgl_alias_cache[i].colors) ++entries;
    assert(entries > 0 && entries < 10);
    large->poseverts = MAXALIASTRIS + 2; large->numtris = MAXALIASTRIS;
    commands[0] = large->poseverts;
    cache = XGL_AliasColors(large, 10, 2, NULL);
    assert(cache && xgl_alias_cache_bytes <= XGL_ALIAS_CACHE_BYTES);
    large->numtris = MAXALIASTRIS + 1;
    old_bytes = xgl_alias_cache_bytes;
    assert(!XGL_AliasColors(large, 10, 2, NULL));
    assert(xgl_alias_cache_bytes == old_bytes);
    free(large);

    /* The ordinary path still supplies complete geometry without a usable
     * material revision/cache, and preserves its original sampling rules. */
    material_revision = 0; sample_calls = 0; captured_count = 0;
    XGL_DrawAlias(&cl_entities[1]);
    assert(sample_calls == 4 && captured_count == 3);
    XGL_EntitiesShutdown();
}

static void test_alias_cache_allocation_failure(void)
{
    int failure;
    for (failure = 0; failure < 2; ++failure) {
        reset_test();
        make_alias(3);
        allocation_fail_after = failure;
        XGL_DrawAlias(&cl_entities[1]);
        assert(captured_count == 3 && sample_calls == 3);
        assert(xgl_alias_cache_bytes == sizeof(xgl_alias_cache));
        allocation_fail_after = -1; captured_count = 0; sample_calls = 0;
        XGL_DrawAlias(&cl_entities[1]);
        assert(captured_count == 3 && sample_calls == 3);
        captured_count = 0; sample_calls = 0;
        XGL_DrawAlias(&cl_entities[1]);
        assert(captured_count == 3 && sample_calls == 0);
        XGL_EntitiesShutdown();
    }
}

static void compare_capture(const xgl_vertex_t *expected, int count)
{
    int i, j;
    assert(captured_count == count);
    for (i = 0; i < count; ++i) {
        for (j = 0; j < 3; ++j) {
            assert(fabs(captured[i].position[j] - expected[i].position[j]) < .0001);
            assert(fabs(captured[i].normal[j] - expected[i].normal[j]) < .0001);
            assert(fabs(captured[i].color[j] - expected[i].color[j]) < .0001);
        }
    }
}

/* The one topology a test has made, if it has made one. */
static xgl_alias_topology_t *only_topology(void)
{
    xgl_alias_topology_t *found;
    int i;
    found = NULL;
    for (i = 0; i < XGL_ALIAS_TOPOLOGY_SLOTS; ++i) {
        if (!xgl_alias_topologies[i].indices) continue;
        assert(!found);
        found = &xgl_alias_topologies[i];
    }
    return found;
}

static void test_alias_cached_mesh_equivalence(void)
{
    xgl_vertex_t expected[192];
    aliashdr_t *header;
    trivertx_t *pose;
    entity_t *entity;
    xgl_alias_topology_t *topology;
    unsigned long hits;
    int i, depth, fan, count, middles;
    for (fan = 0; fan < 2; ++fan) {
        for (depth = 0; depth <= 2; ++depth) {
            reset_test();
            header = make_alias(fan ? -6 : 6);
            header->frames[0].numposes = 1;
            header->scale[0] = .3f; header->scale[1] = 2; header->scale[2] = -.75f;
            header->scale_origin[0] = 5; header->scale_origin[1] = -7;
            pose = (trivertx_t *)((byte *)header + header->posedata);
            for (i = 0; i < header->poseverts; ++i) {
                pose[i].v[0] = (byte)(i * 4 + 3);
                pose[i].v[1] = (byte)(i * 2 + 1);
                pose[i].v[2] = (byte)(i * 3 + 1);
                pose[i].lightnormalindex = (byte)(i == 5 ? 255 : i * 31);
            }
            entity = &cl_entities[1];
            entity->angles[0] = 23; entity->angles[1] = 19; entity->angles[2] = 137;
            entity->origin[0] = 31; entity->origin[1] = -17; entity->origin[2] = 64;
            xgl_alias_subdivide.value = (float)depth;
            XGL_DrawAlias(entity); /* cold sampled-color fallback */
            count = captured_count;
            assert(count == 12 * (1 << (2 * depth)));
            memcpy(expected, captured, (size_t)count * sizeof(*expected));
            captured_count = 0; xgl_entity_extra_used = 0;
            XGL_DrawAlias(entity); /* build the topology and the normals for this scale */
            compare_capture(expected, count);
            topology = only_topology();
            assert(topology && profile_counts[XGL_PROFILE_ALIAS_TOPOLOGY_MISSES] == 1);
            /* The commands of a strip or fan name every vertex once; each
             * edge has one middle, whichever of its two triangles asks. */
            assert(topology->base_points == 6 && topology->point_count <= topology->vertex_count);
            middles = topology->point_count - topology->base_points;
            if (depth == 0) assert(middles == 0);
            if (depth == 1) assert(middles == 9);
            captured_count = 0; normalizations = 0; xgl_entity_extra_used = 0;
            XGL_DrawAlias(entity);
            compare_capture(expected, count);
            /* Nothing of a pose is kept. The device is given unit normals,
             * so each middle's is brought to length; the model's own come
             * from the table made for its scale. */
            assert(normalizations == middles);
            assert(profile_counts[XGL_PROFILE_ALIAS_TOPOLOGY_HITS] == 1);
            assert(profile_counts[XGL_PROFILE_ALIAS_POINTS] == 2 * (unsigned long)topology->point_count);

            /* The same mesh for any position bytes, normals, origin, rotation
             * and model offset: it owns none of them. */
            hits = profile_counts[XGL_PROFILE_ALIAS_TOPOLOGY_HITS];
            pose[0].v[0] += 17;
            pose[2].lightnormalindex = 77;
            header->scale_origin[2] += 9;
            entity->origin[1] += 33; entity->angles[1] += 81;
            captured_count = 0; normalizations = 0; xgl_entity_extra_used = 0;
            test_light = 73;
            XGL_DrawAlias(entity);
            assert(normalizations == middles && only_topology() == topology);
            assert(profile_counts[XGL_PROFILE_ALIAS_TOPOLOGY_HITS] == hits + 1);
            assert(profile_counts[XGL_PROFILE_ALIAS_TOPOLOGY_MISSES] == 1);
            memcpy(expected, captured, (size_t)count * sizeof(*expected));
            captured_count = 0; xgl_entity_extra_used = 0; material_revision = 0;
            XGL_DrawAlias(entity); /* independent recursive reference */
            compare_capture(expected, count);
            material_revision = 1;

            /* Another scale has other normals, made once; zero scale keeps
             * the fallback's unscaled-normal convention. */
            header->scale[0] = 0; pose[1].lightnormalindex = 4;
            captured_count = 0; normalizations = 0; xgl_entity_extra_used = 0;
            XGL_DrawAlias(entity);
            assert(normalizations == XGL_VERTEX_NORMALS + middles);
            captured_count = 0; normalizations = 0; xgl_entity_extra_used = 0;
            XGL_DrawAlias(entity);
            assert(normalizations == middles);
            memcpy(expected, captured, (size_t)count * sizeof(*expected));
            captured_count = 0; xgl_entity_extra_used = 0; material_revision = 0;
            XGL_DrawAlias(entity);
            compare_capture(expected, count);
            XGL_EntitiesShutdown();
        }
    }
}

static void test_alias_pose_canceling_normals(void)
{
    xgl_vertex_t expected[48];
    aliashdr_t *header;
    trivertx_t *pose;
    int i, opposite;
    reset_test();
    header = make_alias(3);
    header->frames[0].numposes = 1;
    header->scale[0] = .5f; header->scale[1] = 2; header->scale[2] = -3;
    pose = (trivertx_t *)((byte *)header + header->posedata);
    opposite = -1;
    for (i = 0; i < XGL_VERTEX_NORMALS; ++i) {
        if (r_avertexnormals[i][0] == -r_avertexnormals[0][0] &&
            r_avertexnormals[i][1] == -r_avertexnormals[0][1] &&
            r_avertexnormals[i][2] == -r_avertexnormals[0][2]) opposite = i;
    }
    assert(opposite >= 0);
    pose[0].lightnormalindex = pose[2].lightnormalindex = 0;
    pose[1].lightnormalindex = (byte)opposite;
    cl_entities[1].angles[0] = 71; cl_entities[1].angles[1] = 32;
    xgl_alias_subdivide.value = 2;
    XGL_DrawAlias(&cl_entities[1]);
    assert(captured_count == 48);
    memcpy(expected, captured, sizeof(expected));
    captured_count = 0; xgl_entity_extra_used = 0;
    XGL_DrawAlias(&cl_entities[1]);
    compare_capture(expected, 48);
    captured_count = 0; xgl_entity_extra_used = 0;
    XGL_DrawAlias(&cl_entities[1]);
    compare_capture(expected, 48);
    XGL_EntitiesShutdown();
}

/* Two triangles on one edge of the model, which the skin has at two places:
 * the commands name its ends twice, and the second time they lie the other
 * way round on the skin. */
static aliashdr_t *make_seam(void)
{
    static const float uv[6][2] = {{.1f, .5f}, {.3f, .5f}, {.2f, .8f}, {.7f, .5f}, {.9f, .5f}, {.8f, .2f}};
    static const int model_vertex[6] = {0, 1, 2, 1, 0, 3};
    static const byte place[4][3] = {{0, 0, 0}, {40, 0, 0}, {20, 30, 0}, {20, 0, 30}};
    xgl_alias_identity_t *identity;
    aliashdr_t *header;
    trivertx_t *poses;
    int *commands, *weld, *same;
    int i, j;
    memset(&alias_storage, 0, sizeof(alias_storage));
    memset(&alias_model, 0, sizeof(alias_model));
    header = (aliashdr_t *)alias_storage.bytes;
    header->scale[0] = header->scale[1] = header->scale[2] = 1;
    header->numposes = 1;
    header->poseverts = 6;
    header->numtris = 2;
    header->numframes = header->numskins = 1;
    header->frames[0].firstpose = 0;
    header->frames[0].numposes = 1;
    header->gl_texturenum[0][0] = header->gl_texturenum[0][1] = 10;
    header->gl_texturenum[0][2] = header->gl_texturenum[0][3] = 10;
    identity = (xgl_alias_identity_t *)((byte *)header + sizeof(*header));
    header->commands = sizeof(*header) + sizeof(*identity);
    commands = (int *)(identity + 1);
    weld = commands + 15;
    same = weld + 6;
    header->posedata = header->commands + (15 + 12) * (int)sizeof(int);
    poses = (trivertx_t *)((byte *)header + header->posedata);
    commands[0] = commands[7] = 3;
    commands[14] = 0;
    for (i = 0; i < 6; ++i) {
        memcpy(&commands[(i < 3 ? 1 : 8) + 2 * (i % 3)], uv[i], sizeof(uv[i]));
        memcpy(poses[i].v, place[model_vertex[i]], 3);
        poses[i].lightnormalindex = 5;
        /* No two are at one place on the skin; four are two of the model's. */
        weld[i] = i;
        for (j = 0; model_vertex[j] != model_vertex[i]; ++j) {}
        same[i] = j;
    }
    identity->magic = XGL_ALIAS_IDENTITY_MAGIC;
    identity->generation = 78;
    identity->words = 15 + 12;
    identity->hash = XGL_AliasCommandHash(commands, identity->words);
    assert(XGL_AliasIdentity(header) == identity);
    strcpy(alias_model.name, "progs/soldier.mdl");
    alias_model.type = mod_alias;
    alias_model.radius = 400;
    alias_model.cache.data = header;
    cl_entities[1].model = &alias_model;
    return header;
}

/* Where the normals of an edge's two ends cancel, its middle takes the
 * normal of the end that is the earlier vertex of the model, from whichever
 * triangle and by whichever way of drawing: here the two triangles on the
 * edge have its ends in opposite orders on the skin. */
static void test_alias_seam_canceling_normals(void)
{
    xgl_vertex_t expected[96];
    aliashdr_t *header;
    trivertx_t *pose;
    xgl_alias_topology_t *topology;
    vec3_t first, middle, delta;
    int i, j, depth, opposite, count, found;
    opposite = -1;
    for (i = 0; i < XGL_VERTEX_NORMALS; ++i) {
        if (r_avertexnormals[i][0] == -r_avertexnormals[0][0] &&
            r_avertexnormals[i][1] == -r_avertexnormals[0][1] &&
            r_avertexnormals[i][2] == -r_avertexnormals[0][2]) opposite = i;
    }
    assert(opposite >= 0);
    for (depth = 1; depth <= 2; ++depth) {
        reset_test();
        header = make_seam();
        pose = (trivertx_t *)((byte *)header + header->posedata);
        pose[0].lightnormalindex = pose[4].lightnormalindex = 0;
        pose[1].lightnormalindex = pose[3].lightnormalindex = (byte)opposite;
        cl_entities[1].angles[0] = 71; cl_entities[1].angles[1] = 32;
        xgl_alias_subdivide.value = (float)depth;
        XGL_DrawAlias(&cl_entities[1]); /* by recursion */
        count = 6 * (1 << (2 * depth));
        assert(captured_count == count);
        memcpy(expected, captured, (size_t)count * sizeof(expected[0]));
        /* The first corner drawn is the model's first vertex, and the
         * second lies along the edge: at its middle, or at depth 2 half way
         * there. Every corner at the middle has the first vertex's normal:
         * three from each triangle. */
        VectorCopy(expected[0].normal, first);
        for (j = 0; j < 3; ++j)
            middle[j] = expected[0].position[j] + (expected[1].position[j] - expected[0].position[j]) * depth;
        found = 0;
        for (i = 0; i < count; ++i) {
            VectorSubtract(expected[i].position, middle, delta);
            if (DotProduct(delta, delta) > 1e-6) continue;
            ++found;
            for (j = 0; j < 3; ++j) assert(fabs(expected[i].normal[j] - first[j]) < 1e-5);
        }
        assert(found == 6);
        for (i = 0; i < 2; ++i) {
            captured_count = 0; xgl_entity_extra_used = 0;
            XGL_DrawAlias(&cl_entities[1]); /* from the mesh */
            compare_capture(expected, count);
        }
        /* Four vertices of the model, and the five edges between them. */
        topology = only_topology();
        assert(topology && topology->base_points == 4);
        if (depth == 1) assert(topology->point_count == 9);
        XGL_EntitiesShutdown();
    }
}

static void test_alias_mesh_batching_and_limits(void)
{
    aliashdr_t *header;
    trivertx_t *pose;
    xgl_alias_topology_t *topology;
    xgl_alias_cache_t *colors;
    size_t bytes;
    int i, calls, count, middles;
    reset_test();
    header = make_alias(320);
    header->frames[0].numposes = 1;
    xgl_alias_subdivide.value = 1;
    XGL_DrawAlias(&cl_entities[1]);
    count = captured_count;
    assert(count == 318 * 12);
    calls = (count + XGL_ENTITY_BATCH_VERTICES - 1) / XGL_ENTITY_BATCH_VERTICES;
    assert(draw_calls == calls && calls == 5);
    assert(normalizations == 318 * 3 + 1);
    captured_count = draw_calls = 0; xgl_entity_extra_used = 0;
    XGL_DrawAlias(&cl_entities[1]);
    assert(captured_count == count && draw_calls == calls);
    topology = only_topology();
    /* A strip of 318 triangles has 637 edges, each with one middle, where
     * the triangles between them name 954. */
    assert(topology && topology->base_points == 320 && topology->vertex_count == 320 + 954);
    middles = topology->point_count - topology->base_points;
    assert(middles == 637);
    captured_count = draw_calls = normalizations = 0; xgl_entity_extra_used = 0;
    XGL_DrawAlias(&cl_entities[1]);
    assert(captured_count == count && draw_calls == calls && normalizations == middles);

    /* No pose adds anything to what is kept. */
    bytes = xgl_alias_topology_bytes;
    pose = (trivertx_t *)((byte *)header + header->posedata);
    for (i = 0; i < 40; ++i) {
        pose[0].lightnormalindex = (byte)i;
        pose[i].v[1] = (byte)(i * 5);
        captured_count = 0; xgl_entity_extra_used = 0;
        XGL_DrawAlias(&cl_entities[1]);
        assert(captured_count == count && xgl_alias_topology_bytes == bytes);
    }
    assert(only_topology() == topology);
    assert(profile_counts[XGL_PROFILE_ALIAS_TOPOLOGY_MISSES] == 1);
    assert(!profile_counts[XGL_PROFILE_ALIAS_TOPOLOGY_EVICTIONS]);

    /* A valid larger subdivision exceeds the fixed workspace. Decline it
     * without touching old cache storage; rendering uses its batched fallback. */
    colors = XGL_AliasColors(header, 10, 2, NULL);
    assert(colors);
    assert(!XGL_AliasTopology(header, colors, 1));
    assert(only_topology() == topology && xgl_alias_topology_bytes == bytes);
    XGL_EntitiesShutdown();
    assert(xgl_alias_topology_bytes == XGL_ALIAS_FIXED_BYTES);
}

/* A topology is all there or not there at all, whichever of its requests
 * for memory is refused; the model is drawn the long way meanwhile. */
static void test_alias_mesh_allocation_failure(void)
{
    int failure, refused, built;
    refused = built = 0;
    for (failure = 0; failure < 14; ++failure) {
        reset_test();
        make_alias(3);
        xgl_alias_subdivide.value = 1;
        XGL_DrawAlias(&cl_entities[1]); /* first finish the independent color cache */
        allocation_fail_after = failure;
        captured_count = normalizations = sample_calls = 0; xgl_entity_extra_used = 0;
        XGL_DrawAlias(&cl_entities[1]);
        assert(captured_count == 12 && sample_calls == 0 && normalizations > 0);
        assert(xgl_alias_topology_bytes <= XGL_ALIAS_TOPOLOGY_BYTES);
        if (xgl_alias_topology_bytes == XGL_ALIAS_FIXED_BYTES) {
            assert(!only_topology() && !built);
            ++refused;
        } else {
            assert(only_topology() && only_topology()->point_vertex && only_topology()->point_parents);
            ++built;
        }
        allocation_fail_after = -1;
        captured_count = 0; xgl_entity_extra_used = 0;
        XGL_DrawAlias(&cl_entities[1]);
        captured_count = normalizations = 0; xgl_entity_extra_used = 0;
        XGL_DrawAlias(&cl_entities[1]);
        /* Warm: only the three middles' normals are brought to length. */
        assert(captured_count == 12 && normalizations == 3 && only_topology());
        XGL_EntitiesShutdown();
        assert(xgl_alias_topology_bytes == XGL_ALIAS_FIXED_BYTES);
    }
    assert(refused >= 9 && built >= 1);
}

static void test_alias_shared_topology(void)
{
    aliashdr_t *header;
    trivertx_t *pose;
    xgl_alias_cache_t *colors;
    xgl_alias_topology_t *topology, *second;
    size_t first_bytes;
    int *commands;
    int i, entries;
    reset_test();
    header = make_alias(6);
    pose = (trivertx_t *)((byte *)header + header->posedata);
    colors = XGL_AliasColors(header, 10, 1, NULL);
    assert(colors);
    topology = XGL_AliasTopology(header, colors, 1);
    assert(topology);
    first_bytes = xgl_alias_topology_bytes;
    /* Every pose of the model has the one topology. */
    pose[0].lightnormalindex = 31;
    assert(XGL_AliasTopology(header, colors, 1) == topology);
    assert(xgl_alias_topology_bytes == first_bytes);
    assert(profile_counts[XGL_PROFILE_ALIAS_TOPOLOGY_MISSES] == 1);
    assert(profile_counts[XGL_PROFILE_ALIAS_TOPOLOGY_HITS] == 1);
    /* Asking without leave to build finds it, and counts nothing. */
    assert(XGL_AliasTopology(header, colors, 0) == topology);
    assert(profile_counts[XGL_PROFILE_ALIAS_TOPOLOGY_HITS] == 1);

    /* Hashes only reject mismatches: equal hashes still require exact data. */
    commands = (int *)((byte *)header + header->commands);
    ++commands[1];
    colors = XGL_AliasColors(header, 10, 1, NULL);
    assert(colors);
    assert(!XGL_AliasTopology(header, colors, 0));
    topology->command_hash = colors->command_hash;
    assert(!XGL_AliasTopology(header, colors, 0));
    second = XGL_AliasTopology(header, colors, 1);
    assert(second && second != topology);

    /* More command streams than there are places: the least lately used
     * make way, and the cap holds. */
    for (i = 0; i < XGL_ALIAS_TOPOLOGY_SLOTS + 4; ++i) {
        ++commands[1];
        colors = XGL_AliasColors(header, 10, 1, NULL);
        assert(colors && XGL_AliasTopology(header, colors, 1));
        assert(xgl_alias_topology_bytes <= XGL_ALIAS_TOPOLOGY_BYTES);
    }
    entries = 0;
    for (i = 0; i < XGL_ALIAS_TOPOLOGY_SLOTS; ++i)
        if (xgl_alias_topologies[i].indices) {
            ++entries;
            assert(xgl_alias_topologies[i].parents && xgl_alias_topologies[i].point_vertex);
        }
    assert(entries == XGL_ALIAS_TOPOLOGY_SLOTS);
    assert(profile_counts[XGL_PROFILE_ALIAS_TOPOLOGY_EVICTIONS] == 6);
    XGL_EntitiesShutdown();
    assert(xgl_alias_topology_bytes == XGL_ALIAS_FIXED_BYTES);
}

static void test_alias_topology_byte_limit(void)
{
    aliashdr_t *header;
    xgl_alias_cache_t *colors;
    int *commands;
    int i, entries;
    reset_test();
    header = make_alias(1000);
    commands = (int *)((byte *)header + header->commands);
    /* Each of these is some sixty kilobytes: the cap is reached before
     * the places run out. */
    for (i = 0; i < XGL_ALIAS_TOPOLOGY_SLOTS; ++i) {
        ++commands[1];
        colors = XGL_AliasColors(header, 10, 1, NULL);
        assert(colors && XGL_AliasTopology(header, colors, 1));
        assert(xgl_alias_topology_bytes <= XGL_ALIAS_TOPOLOGY_BYTES);
    }
    entries = 0;
    for (i = 0; i < XGL_ALIAS_TOPOLOGY_SLOTS; ++i)
        if (xgl_alias_topologies[i].indices) ++entries;
    assert(entries > 0 && entries < XGL_ALIAS_TOPOLOGY_SLOTS);
    assert(profile_counts[XGL_PROFILE_ALIAS_TOPOLOGY_EVICTIONS] > 0);
    assert(profile_counts[XGL_PROFILE_ALIAS_TOPOLOGY_MISSES] == XGL_ALIAS_TOPOLOGY_SLOTS);
    XGL_EntitiesShutdown();
    assert(xgl_alias_topology_bytes == XGL_ALIAS_FIXED_BYTES);
}

static void test_alias_direct_partial_batches(void)
{
    xgl_vertex_t expected[192];
    aliashdr_t *header;
    int count, i, j, distinct;
    reset_test();
    header = make_alias(6);
    header->frames[0].numposes = 1;
    xgl_alias_subdivide.value = 2;
    coverage_mode = 2;
    XGL_DrawAlias(&cl_entities[1]);
    count = captured_count;
    assert(count > 0 && count < 192);
    memcpy(expected, captured, (size_t)count * sizeof(*expected));
    captured_count = 0; xgl_entity_extra_used = 0;
    reserve_limit = 9;
    XGL_DrawAlias(&cl_entities[1]);
    compare_capture(expected, count);
    assert(reserve_calls == (count + reserve_limit - 1) / reserve_limit);
    distinct = 0;
    for (i = 0; i < count; ++i) {
        for (j = 0; j < i; ++j)
            if (!memcmp(expected[i].color, expected[j].color, sizeof(expected[i].color))) break;
        if (j == i) ++distinct;
    }
    assert(prepare_calls == distinct && distinct < count);
    assert(audio_locks == audio_unlocks && !audio_lock_depth);
    /* Rebuilding material colors must also replace the cached visible count. */
    ++material_revision; coverage_mode = 1;
    captured_count = reserve_calls = 0; xgl_entity_extra_used = 0;
    XGL_DrawAlias(&cl_entities[1]);
    assert(!captured_count);
    xgl_entity_extra_used = 0; sample_calls = 0;
    XGL_DrawAlias(&cl_entities[1]);
    assert(!captured_count && !reserve_calls && !sample_calls);
    ++material_revision; coverage_mode = 0;
    xgl_entity_extra_used = 0;
    XGL_DrawAlias(&cl_entities[1]);
    assert(captured_count == 192);
    captured_count = 0; xgl_entity_extra_used = 0;
    XGL_DrawAlias(&cl_entities[1]);
    assert(captured_count == 192);
    XGL_EntitiesShutdown();
}

static void test_alias_audio_lock_cleanup(void)
{
    aliashdr_t *header;
    int *commands;
    reset_test();
    header = make_alias(3);
    test_cull = 1;
    XGL_DrawAlias(&cl_entities[1]);
    assert(!audio_lock_depth && !audio_locks && !audio_unlocks);
    test_cull = 0; alias_model.cache.data = NULL;
    XGL_DrawAlias(&cl_entities[1]);
    assert(!audio_lock_depth && audio_locks == 1 && audio_unlocks == 1);
    alias_model.cache.data = header; header->poseverts = 0;
    XGL_DrawAlias(&cl_entities[1]);
    assert(!audio_lock_depth && audio_locks == 2 && audio_unlocks == 2);
    header->poseverts = 3;
    commands = (int *)((byte *)header + header->commands);
    commands[0] = 4;
    XGL_DrawAlias(&cl_entities[1]);
    assert(!audio_lock_depth && audio_locks == 3 && audio_unlocks == 3);
    assert(!captured_count);
    commands[0] = 3;
    XGL_DrawAlias(&cl_entities[1]);
    assert(!audio_lock_depth && audio_locks == 4 && audio_unlocks == 4);
    assert(captured_count == 3);
    XGL_EntitiesShutdown();
}

/* Give a fixture the record the real loader stores before its commands. */
static xgl_alias_identity_t *add_identity(aliashdr_t *header, unsigned int generation)
{
    xgl_alias_identity_t *identity;
    byte *commands;
    int words;
    words = 2 + 2 * header->poseverts;
    commands = (byte *)header + header->commands;
    memmove(commands + sizeof(*identity), commands,
            (size_t)words * sizeof(int) +
            (size_t)header->numposes * header->poseverts * sizeof(trivertx_t));
    header->commands += sizeof(*identity);
    header->posedata += sizeof(*identity);
    identity = (xgl_alias_identity_t *)commands;
    identity->magic = XGL_ALIAS_IDENTITY_MAGIC;
    identity->generation = generation;
    identity->words = words;
    identity->hash = XGL_AliasCommandHash((const int *)(identity + 1), words);
    assert(XGL_AliasIdentity(header) == identity);
    return identity;
}

static xgl_alias_cache_t *only_color_entry(void)
{
    xgl_alias_cache_t *found;
    int i;
    found = NULL;
    for (i = 0; i < XGL_ALIAS_CACHE_SLOTS; ++i) {
        if (!xgl_alias_cache[i].colors) continue;
        assert(!found);
        found = &xgl_alias_cache[i];
    }
    assert(found);
    return found;
}

static void test_alias_identity_fast_path(void)
{
    xgl_alias_identity_t *identity;
    aliashdr_t *header;
    float uv, saved_uv;
    int *commands;
    reset_test();
    header = make_alias(6);
    assert(!XGL_AliasIdentity(header)); /* Plain fixtures take the exact path. */
    header->frames[0].numposes = 1;
    identity = add_identity(header, 7);
    commands = (int *)((byte *)header + header->commands);
    xgl_alias_subdivide.value = 1;
    XGL_DrawAlias(&cl_entities[1]);
    assert(sample_calls == 48 && captured_count == 48);
    assert(only_color_entry()->generation == 7);
    captured_count = sample_calls = 0; xgl_entity_extra_used = 0;
    XGL_DrawAlias(&cl_entities[1]); /* builds the shared topology */
    assert(!sample_calls && captured_count == 48);

    /* The same loaded model is recognised by its generation alone. Editing
     * a coordinate in place (which a real loaded model never does) shows
     * that neither cache compared the command list again. */
    memcpy(&saved_uv, commands + 1, sizeof(saved_uv));
    uv = .75f; memcpy(commands + 1, &uv, sizeof(uv));
    captured_count = sample_calls = normalizations = 0; xgl_entity_extra_used = 0;
    memset(profile_counts, 0, sizeof(profile_counts));
    XGL_DrawAlias(&cl_entities[1]);
    /* Only the nine middles' normals, for the device, are brought to length. */
    assert(!sample_calls && normalizations == 9 && captured_count == 48);
    assert(profile_counts[XGL_PROFILE_ALIAS_COLOR_HITS] == 1);
    assert(profile_counts[XGL_PROFILE_ALIAS_TOPOLOGY_HITS] == 1);
    memcpy(commands + 1, &saved_uv, sizeof(saved_uv));

    /* Quake evicted and reloaded the model: a new generation with equal
     * commands. One exact comparison adopts it; nothing is resampled. */
    identity->generation = 8;
    captured_count = sample_calls = normalizations = 0; xgl_entity_extra_used = 0;
    XGL_DrawAlias(&cl_entities[1]);
    assert(!sample_calls && normalizations == 9 && captured_count == 48);
    assert(only_color_entry()->generation == 8);

    /* A different model in the same slot: new generation, new commands and
     * hash. Its colours are sampled afresh. */
    memcpy(commands + 1, &uv, sizeof(uv));
    identity->generation = 9;
    identity->hash = XGL_AliasCommandHash(commands, identity->words);
    captured_count = sample_calls = 0; xgl_entity_extra_used = 0;
    XGL_DrawAlias(&cl_entities[1]);
    assert(sample_calls == 48 && captured_count == 48);

    /* A damaged record is ignored rather than trusted. */
    identity->magic = 0;
    assert(!XGL_AliasIdentity(header));
    identity->magic = XGL_ALIAS_IDENTITY_MAGIC; identity->generation = 0;
    assert(!XGL_AliasIdentity(header));
    XGL_EntitiesShutdown();
}

static void test_alias_state_order(void)
{
    static model_t flame_model;
    xgl_vertex_t expected[64];
    aliashdr_t *header;
    int i, count, frame;
    reset_test();
    header = make_alias(3);
    header->frames[0].numposes = 1;
    strcpy(alias_model.name, "progs/soldier.mdl");
    flame_model = alias_model;
    strcpy(flame_model.name, "progs/flame.mdl");
    for (i = 2; i <= 4; ++i) {
        cl_entities[i] = cl_entities[1];
        cl_entities[i].origin[1] = 40.0f * i;
    }
    cl_entities[2].model = cl_entities[4].model = &flame_model;
    /* Visible-list order: lit, flame, lit, flame. */
    for (i = 0; i < 4; ++i) cl_visedicts[i] = &cl_entities[i + 1];
    cl_numvisedicts = 4;
    xgl_alias_subdivide.value = 1;
    xgl_entity_triangles.value = 3; /* Exactly one refined one-triangle model. */
    test_gamma = 1; /* Serial and worker paths then share one gamma table. */
    for (i = 0; i < 256; ++i) gamma_output[i] = (float)(255 - i) / 255.0f;
    for (frame = 0; frame < 4; ++frame) {
        captured_count = draw_calls = 0;
        draw_entities();
        /* Both unlit flames are drawn first, then both lit models: one
         * illumination change, not three. The detail allowance still goes
         * to the first model in the visible list, although it now draws third. */
        assert(draw_calls == 4 && captured_count == 21);
        assert(draw_log_lit[0] == 0 && draw_log_lit[1] == 0);
        assert(draw_log_lit[2] == 2 && draw_log_lit[3] == 2);
        assert(draw_log_count[0] == 3 && draw_log_count[1] == 3);
        assert(draw_log_count[2] == 12 && draw_log_count[3] == 3);
        assert(captured[0].position[1] == 80 && captured[3].position[1] == 160);
        assert(captured[18].position[1] == 120);
        assert(xgl_entity_extra_used == 3);
        assert(profile_draw_calls[XGL_PROFILE_DRAW_ALIAS] == 4 * (frame + 1));
    }
    count = captured_count;
    memcpy(expected, captured, (size_t)count * sizeof(*expected));

    /* The worker's jobs are queued in that drawing order, and whichever
     * thread runs them the frame is identical to the serial one. */
    smp_active = 1;
    for (frame = 0; frame < 2; ++frame) {
        take_jobs = frame;
        captured_count = executed_jobs = taken_jobs = 0;
        XGL_AliasPrepareFrame();
        assert(pending_jobs == 4 && xgl_alias_frame->job_count == 4);
        assert(xgl_alias_frame->jobs[0].visible_index == 1);
        assert(xgl_alias_frame->jobs[1].visible_index == 3);
        assert(xgl_alias_frame->jobs[2].visible_index == 0);
        assert(xgl_alias_frame->jobs[3].visible_index == 2);
        assert(xgl_entity_extra_used == 3);
        draw_entities();
        compare_capture(expected, count);
        if (frame) {
            /* Reclaimed jobs ran here, on the serial path's scratch buffer. */
            assert(taken_jobs == 4 && !executed_jobs);
            assert(profile_counts[XGL_PROFILE_SMP_HELPED] == 4);
            for (i = 0; i < 4; ++i)
                assert(xgl_alias_frame->jobs[i].work == xgl_alias_work);
        } else {
            assert(executed_jobs == 4 && !taken_jobs);
            for (i = 0; i < 4; ++i)
                assert(xgl_alias_frame->jobs[i].work == xgl_alias_frame->work);
        }
        XGL_AliasFinishFrame();
    }
    smp_active = take_jobs = 0;
    cl_numvisedicts = 0;
    XGL_EntitiesShutdown();
}

/* With xgl_alias_shade the CPU computes what the device used to: the
 * prepared color times (ambient/2 + max(0, N.L)/2), from the same normals,
 * and every model is submitted unlit. That is at the model's own vertices;
 * the middle of an edge has the mean of its two ends' light, which is what
 * the device shows there when the edge is not divided. */
static void test_alias_cpu_shading(void)
{
    static model_t flame_model;
    /* Where the three corners of each triangle and the middles of its
     * edges ab, bc and ca come among the twelve corners it is divided into. */
    static const int ends[12][2] = {{0, 0}, {0, 4}, {8, 0}, {0, 4}, {4, 4}, {4, 8},
                                    {8, 0}, {4, 8}, {8, 8}, {0, 4}, {4, 8}, {8, 0}};
    xgl_vertex_t native[192], shaded[192];
    float lights[192];
    aliashdr_t *header;
    trivertx_t *pose;
    entity_t *entity;
    float dot, shade, dim;
    int i, j, depth, count, stage, lit_corners, group;
    for (depth = 0; depth <= 1; ++depth) {
        reset_test();
        header = make_alias(6);
        header->frames[0].numposes = 1;
        header->scale[0] = .3f; header->scale[1] = 2; header->scale[2] = -.75f;
        pose = (trivertx_t *)((byte *)header + header->posedata);
        for (i = 0; i < header->poseverts; ++i) {
            pose[i].v[0] = (byte)(i * 4 + 3);
            pose[i].v[1] = (byte)(i * 2 + 1);
            pose[i].v[2] = (byte)(i * 3 + 1);
            pose[i].lightnormalindex = (byte)(i * 31);
        }
        strcpy(alias_model.name, "progs/soldier.mdl");
        entity = &cl_entities[1];
        entity->angles[0] = 23; entity->angles[1] = 19; entity->angles[2] = 137;
        entity->origin[0] = 31; entity->origin[1] = -17; entity->origin[2] = 64;
        xgl_alias_subdivide.value = (float)depth;
        test_light = 96;
        test_ambient = depth ? .5f : 1;
        /* A gamma curve shows that the shade is applied after gamma, as the
         * device applied its illumination to the prepared color. */
        test_gamma = 1;
        for (i = 0; i < 256; ++i) gamma_output[i] = (float)sqrt(i / 255.0);
        for (stage = 0; stage < 3; ++stage) {
            captured_count = 0; xgl_entity_extra_used = 0;
            XGL_DrawAlias(entity);
        }
        count = captured_count;
        assert(count == 12 * (1 << (2 * depth)) && native_lit == 2 && !prepared_draws);
        memcpy(native, captured, (size_t)count * sizeof(*native));
        XGL_EntitiesShutdown();
        xgl_alias_shade.value = 1;
        /* Cold fallback, cache build, then the kept mesh: all the same. */
        for (stage = 0; stage < 3; ++stage) {
            captured_count = 0; xgl_entity_extra_used = 0; normalizations = 0;
            XGL_DrawAlias(entity);
            assert(captured_count == count && native_lit == 0);
            /* Only the cold draw streams; the next builds and uses the cache. */
            assert(prepared_draws == 1);
            if (stage == 2) assert(!normalizations);
            lit_corners = 0;
            for (i = 0; i < count; ++i) {
                dot = (native[i].normal[0] + native[i].normal[1] + native[i].normal[2]) * .577350269f;
                lights[i] = .5f * test_ambient + (dot > 0 ? .5f * dot : 0);
                if (dot > 0) ++lit_corners;
            }
            for (i = 0; i < count; ++i) {
                shade = lights[i];
                if (depth) {
                    group = i - i % 12;
                    shade = (lights[group + ends[i % 12][0]] + lights[group + ends[i % 12][1]]) * .5f;
                }
                for (j = 0; j < 3; ++j) {
                    assert(fabs(captured[i].position[j] - native[i].position[j]) < .0001);
                    assert(fabs(captured[i].color[j] - native[i].color[j] * shade) < .0001);
                }
                /* Only the cold draw, which streams the model's own
                 * records, still passes normals; they are not read. */
                assert(captured_plain[i] == (stage != 0));
            }
            assert(lit_corners > 0 && lit_corners < count);
        }
        memcpy(shaded, captured, (size_t)count * sizeof(*shaded));
        /* A worker prepares the same colors, in records without normals. */
        cl_visedicts[0] = entity; cl_numvisedicts = 1;
        smp_active = 1;
        captured_count = executed_jobs = 0; xgl_entity_extra_used = 0;
        XGL_AliasPrepareFrame();
        assert(pending_jobs == 1 && xgl_alias_frame->jobs[0].lit == 0);
        draw_entities();
        assert(executed_jobs == 1 && captured_count == count && native_lit == 0);
        assert(plain_submissions == 1);
        for (i = 0; i < count; ++i) {
            assert(captured_plain[i]);
            for (j = 0; j < 3; ++j) {
                assert(captured[i].position[j] == shaded[i].position[j]);
                assert(captured[i].color[j] == shaded[i].color[j]);
            }
        }
        XGL_AliasFinishFrame();
        smp_active = 0; cl_numvisedicts = 0;

        /* Quake adds each dynamic light within reach to the model's light.
         * Native illumination leaves that to the device's point lights. */
        test_gamma = 0; test_light = 16;
        captured_count = 0; xgl_entity_extra_used = 0;
        XGL_DrawAlias(entity);
        dim = captured[0].color[1];
        assert(dim > 0);
        VectorCopy(entity->origin, cl_dlights[3].origin);
        cl_dlights[3].origin[2] += 60;
        cl_dlights[3].radius = 100; cl_dlights[3].die = 1;
        captured_count = 0; xgl_entity_extra_used = 0;
        XGL_DrawAlias(entity);
        assert(fabs(captured[0].color[1] - dim * (16 + 40) / 16) < .0001);
        cl_dlights[3].radius = 60; /* out of reach */
        captured_count = 0; xgl_entity_extra_used = 0;
        XGL_DrawAlias(entity);
        assert(captured[0].color[1] == dim);
        cl_dlights[3].radius = 100; r_dynamic.value = 0;
        captured_count = 0; xgl_entity_extra_used = 0;
        XGL_DrawAlias(entity);
        assert(captured[0].color[1] == dim);
        r_dynamic.value = 1; cl.time = 2; /* expired */
        captured_count = 0; xgl_entity_extra_used = 0;
        XGL_DrawAlias(entity);
        assert(captured[0].color[1] == dim);
        cl.time = 0;
        xgl_alias_shade.value = 0;
        captured_count = 0; xgl_entity_extra_used = 0;
        XGL_DrawAlias(entity);
        assert(native_lit == 2 && fabs(captured[0].color[1] * .5f * test_ambient - dim) < .0001 * 2);
        assert(!captured_plain[0] && !captured_plain[count - 1]);

        /* Torches are final as sampled in either mode: no shade, no light. */
        flame_model = alias_model;
        strcpy(flame_model.name, "progs/flame.mdl");
        entity->model = &flame_model;
        for (stage = 0; stage < 2; ++stage) {
            xgl_alias_shade.value = (float)stage;
            captured_count = 0; xgl_entity_extra_used = 0;
            XGL_DrawAlias(entity);
            assert(captured_count == count && native_lit == 0);
            assert(captured_plain[0] && captured_plain[count - 1]);
            if (stage) for (i = 0; i < count; ++i)
                for (j = 0; j < 3; ++j) assert(captured[i].color[j] == shaded[i].color[j]);
            memcpy(shaded, captured, (size_t)count * sizeof(*shaded));
        }
        XGL_EntitiesShutdown();
    }
}

static void test_brush_entity_passes(void)
{
    static model_t brush;
    reset_test();
    brush.type = mod_brush;
    cl_entities[1].model = &brush;
    cl_visedicts[0] = &cl_entities[1]; cl_numvisedicts = 1;
    /* Each brush entity is drawn by exactly one of a frame's two calls. */
    XGL_DrawBrushEntities(1);
    assert(!brush_draws);
    XGL_DrawBrushEntities(0);
    assert(brush_draws == 1 && profile_draw_calls[XGL_PROFILE_DRAW_BRUSH] == 1);
    brush_lights = XGL_LIT_AMBIENT;
    XGL_DrawBrushEntities(0);
    assert(brush_draws == 1);
    XGL_DrawBrushEntities(1);
    assert(brush_draws == 2 && profile_draw_calls[XGL_PROFILE_DRAW_BRUSH] == 2);
    cl_numvisedicts = 0;
}

static void test_smp_alias_frame(void)
{
    static union { double alignment; byte bytes[sizeof(alias_storage)]; } relocated;
    model_t second_model;
    xgl_vertex_t expected[192];
    aliashdr_t *header;
    trivertx_t *pose;
    int i, count, pass, previous_submits;

    reset_test();
    header = make_alias(6);
    header->frames[0].numposes = 1;
    header->scale[0] = .3f;
    header->scale[1] = 2;
    header->scale_origin[0] = 5;
    pose = (trivertx_t *)((byte *)header + header->posedata);
    for (i = 0; i < header->poseverts; ++i) {
        pose[i].v[0] = (byte)(2 + 3 * i);
        pose[i].v[1] = (byte)(i * i);
        pose[i].lightnormalindex = (byte)(i * 23);
    }
    cl_entities[2] = cl_entities[1];
    cl_entities[2].origin[1] = 65;
    cl_entities[1].angles[YAW] = 17;
    cl_entities[2].angles[PITCH] = -39;
    cl_visedicts[0] = &cl_entities[1];
    cl_visedicts[1] = &cl_entities[2];
    cl_numvisedicts = 2;
    xgl_alias_subdivide.value = 1;
    xgl_entity_triangles.value = 12; /* Exactly one refined four-triangle draw. */
    scores[0].colors = 0xe1;
    test_light = 83;
    test_gamma = 1;
    for (i = 0; i < 256; ++i) gamma_output[i] = (float)(255 - i) / 255.0f;

    /* Warm both depth variants before capturing the serial reference. */
    for (i = 0; i < 3; ++i) {
        captured_count = 0;
        draw_entities();
    }
    count = captured_count;
    assert(count == 60 && xgl_entity_extra_used == 12);
    memcpy(expected, captured, count * sizeof(*expected));
    smp_active = 1;
    for (pass = 0; pass <= 2; ++pass) {
        /* Simulate an unavailable queue, one accepted job, then both. */
        smp_limit = pass;
        captured_count = executed_jobs = 0;
        previous_submits = native_submissions;
        XGL_AliasPrepareFrame();
        assert(pending_jobs == pass);
        if (pass == 2) {
            memcpy(relocated.bytes, &alias_storage, sizeof(alias_storage));
            alias_model.cache.data = relocated.bytes;
            /* A real cache move invalidates the entire old allocation. */
            memset(&alias_storage, 0, sizeof(alias_storage));
        }
        draw_entities();
        XGL_AliasFinishFrame();
        compare_capture(expected, count);
        assert(xgl_entity_extra_used == 12);
        assert(executed_jobs == pass && native_submissions - previous_submits == pass);
    }
    /* Starting an audio lock can evict the first model. The regular draw
     * reloads it and spends the detail budget, even if the prepass couldn't
     * see its header. Later models must retain their original lower LOD. */
    second_model = alias_model;
    cl_entities[2].model = &second_model;
    evict_on_audio_lock = &alias_model;
    captured_count = 0;
    XGL_AliasPrepareFrame();
    draw_entities();
    XGL_AliasFinishFrame();
    compare_capture(expected, count);
    assert(!evict_on_audio_lock && !evicted_data);
    assert(xgl_entity_extra_used == 12);

    /* A map change or shutdown can abandon a prepared frame. Drain before
     * releasing any cache storage still referenced by a worker. */
    XGL_AliasPrepareFrame();
    assert(pending_jobs == 2);
    XGL_EntitiesShutdown();
    assert(!pending_jobs);
    smp_active = 0;
    cl_numvisedicts = 0;
}

/* A sheet of quads, every quad a command of its own: a fan of two
 * triangles. As loaded models do, it can say after its commands which
 * command vertices are one model vertex, in two lists: by place on the skin
 * and by place in space, which for a sheet are the same. welds: 0 for
 * neither list, 1 for the true ones, 2 for a first list that makes no
 * sense, 3 for a second that makes none. */
static aliashdr_t *make_sheet(int columns, int rows, int welds)
{
    static const int corner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    xgl_alias_identity_t *identity;
    aliashdr_t *header;
    trivertx_t *poses;
    int *commands, *weld, *same;
    int first[32][32];
    int quads, vertices, words, q, c, i, j, k;
    float uv[2];
    assert(columns < 31 && rows < 31);
    memset(&alias_storage, 0, sizeof(alias_storage));
    memset(&alias_model, 0, sizeof(alias_model));
    header = (aliashdr_t *)alias_storage.bytes;
    header->scale[0] = header->scale[1] = header->scale[2] = 1;
    quads = columns * rows; vertices = 4 * quads;
    header->numposes = 1;
    header->poseverts = vertices;
    header->numtris = 2 * quads;
    header->numframes = header->numskins = 1;
    header->frames[0].firstpose = 0;
    header->frames[0].numposes = 1;
    header->gl_texturenum[0][0] = header->gl_texturenum[0][1] = 10;
    header->gl_texturenum[0][2] = header->gl_texturenum[0][3] = 10;
    identity = (xgl_alias_identity_t *)((byte *)header + sizeof(*header));
    header->commands = sizeof(*header) + sizeof(*identity);
    commands = (int *)(identity + 1);
    words = 9 * quads + 1;
    weld = commands + words;
    same = weld + vertices;
    header->posedata = header->commands + (words + (welds ? 2 * vertices : 0)) * (int)sizeof(int);
    poses = (trivertx_t *)((byte *)header + header->posedata);
    assert((byte *)(poses + vertices) <= alias_storage.bytes + sizeof(alias_storage.bytes));
    memset(first, 0xff, sizeof(first));
    for (q = 0; q < quads; ++q) {
        commands[9 * q] = -4;
        for (c = 0; c < 4; ++c) {
            i = q % columns + corner[c][0]; j = q / columns + corner[c][1];
            k = 4 * q + c;
            uv[0] = (i + .5f) / (columns + 1); uv[1] = (j + .5f) / (rows + 1);
            memcpy(&commands[9 * q + 1 + 2 * c], uv, sizeof(uv));
            poses[k].v[0] = (byte)(10 * i); poses[k].v[1] = (byte)(7 * j);
            poses[k].v[2] = (byte)(i * j);
            poses[k].lightnormalindex = (byte)((i * 37 + j * 11) % 162);
            if (first[i][j] < 0) first[i][j] = k;
            if (welds) {
                weld[k] = welds == 2 && k == 5 ? 6 : first[i][j];
                same[k] = welds == 3 && k == 5 ? 6 : first[i][j];
            }
        }
    }
    commands[9 * quads] = 0;
    identity->magic = XGL_ALIAS_IDENTITY_MAGIC;
    identity->generation = 77;
    identity->words = words + (welds ? 2 * vertices : 0);
    identity->hash = XGL_AliasCommandHash(commands, identity->words);
    assert(XGL_AliasIdentity(header) == identity);
    strcpy(alias_model.name, "progs/soldier.mdl");
    alias_model.type = mod_alias;
    alias_model.radius = 400;
    alias_model.cache.data = header;
    cl_entities[1].model = &alias_model;
    return header;
}

/* Put the corners of every captured triangle, and then the triangles, in
 * one fixed order: what was drawn, however it was sent. */
static int vertex_order(const void *a, const void *b)
{
    return memcmp(a, b, sizeof(xgl_vertex_t));
}
static int triangle_order(const void *a, const void *b)
{
    return memcmp(a, b, 3 * sizeof(xgl_vertex_t));
}
static void sort_capture(void)
{
    int i;
    assert(captured_count % 3 == 0);
    for (i = 0; i < captured_count; i += 3) qsort(&captured[i], 3, sizeof(captured[0]), vertex_order);
    qsort(captured, (size_t)(captured_count / 3), 3 * sizeof(captured[0]), triangle_order);
}

static void test_alias_strips(void)
{
    static xgl_vertex_t expected[2304];
    xgl_alias_topology_t *topology;
    aliashdr_t *header;
    entity_t *entity;
    int depth, i, count, triangles, failure, calls, points;
    for (depth = 0; depth <= 1; ++depth) {
        reset_test();
        header = make_sheet(3, 2, 1);
        triangles = header->numtris << (2 * depth);
        entity = &cl_entities[1];
        entity->angles[0] = 11; entity->angles[1] = 47; entity->angles[2] = -5;
        entity->origin[0] = 100; entity->origin[1] = -20; entity->origin[2] = 30;
        xgl_alias_shade.value = 1;
        xgl_alias_subdivide.value = (float)depth;
        test_light = 100;
        test_gamma = 1;
        for (i = 0; i < 256; ++i) gamma_output[i] = (float)sqrt(i / 255.0);
        /* As separate triangles: what the model is. */
        for (i = 0; i < 3; ++i) {
            captured_count = 0; xgl_entity_extra_used = 0;
            XGL_DrawAlias(entity);
        }
        capture_reserved();
        count = captured_count;
        assert(count == 3 * triangles && !strip_calls);
        sort_capture();
        memcpy(expected, captured, (size_t)count * sizeof(expected[0]));
        /* Its triangles were joined into strips when the mesh was made:
         * each row of quads is one. Every triangle is in exactly one. */
        topology = &xgl_alias_topologies[0];
        assert(topology->indices && topology->triangle_count == triangles);
        /* Twenty-four vertices in the commands are the twelve of the
         * sheet, and its twenty-three edges have a middle each. */
        assert(topology->base_count == 24 && topology->base_points == 12);
        assert(topology->point_count == (depth ? 12 + 23 : 12));
        assert(topology->strip_count > 0 && topology->strip_count < triangles / 2);
        points = 0;
        for (i = 0; i < topology->strip_count; ++i) {
            assert(topology->strip_lengths[i] >= 3 && topology->strip_lengths[i] <= XGL_ALIAS_STRIP_POINTS);
            if (i) assert(topology->strip_lengths[i] <= topology->strip_lengths[i - 1]);
            points += topology->strip_lengths[i];
        }
        assert(points - 2 * topology->strip_count == triangles && points < 2 * triangles);
        if (!depth) assert(topology->strip_count == 2 && topology->strip_lengths[1] == 8);
        /* Sent as strips, it is the same triangles in fewer points. */
        xgl_alias_strips.value = 2;
        captured_count = 0; xgl_entity_extra_used = 0;
        XGL_DrawAlias(entity);
        capture_reserved();
        assert(captured_count == count && strip_calls > 0);
        calls = strip_calls; points = strip_points;
        assert(points - 2 * calls <= triangles && points < 2 * triangles);
        sort_capture();
        assert(!memcmp(captured, expected, (size_t)count * sizeof(expected[0])));
        /* Runs shorter than asked go out as separate triangles; when every
         * run is shorter, the model is sent as it was before. */
        xgl_alias_strips.value = (float)(strip_log[0] - 2);
        strip_calls = strip_points = 0;
        captured_count = 0; xgl_entity_extra_used = 0;
        XGL_DrawAlias(entity);
        capture_reserved();
        assert(strip_calls > 0 && strip_calls <= calls && captured_count == count);
        for (i = 0; i < strip_calls && i < 64; ++i) assert(strip_log[i] == strip_log[0]);
        sort_capture();
        assert(!memcmp(captured, expected, (size_t)count * sizeof(expected[0])));
        xgl_alias_strips.value = (float)(strip_log[0] - 1);
        strip_calls = strip_points = 0;
        captured_count = 0; xgl_entity_extra_used = 0;
        XGL_DrawAlias(entity);
        capture_reserved();
        assert(!strip_calls && captured_count == count);
        sort_capture();
        assert(!memcmp(captured, expected, (size_t)count * sizeof(expected[0])));
        /* A full native batch splits the leftover triangles, not a strip. */
        xgl_alias_strips.value = depth ? 9 : 2;
        reserve_limit = 9;
        strip_calls = strip_points = 0;
        captured_count = 0; xgl_entity_extra_used = 0;
        XGL_DrawAlias(entity);
        capture_reserved();
        reserve_limit = XGL_ENTITY_BATCH_VERTICES;
        assert(captured_count == count);
        sort_capture();
        assert(!memcmp(captured, expected, (size_t)count * sizeof(expected[0])));
        /* The worker writes the same strips; they are sent from its output. */
        xgl_alias_strips.value = 2;
        cl_visedicts[0] = entity; cl_numvisedicts = 1;
        smp_active = 1;
        strip_calls = strip_points = 0;
        captured_count = executed_jobs = 0; xgl_entity_extra_used = 0;
        XGL_AliasPrepareFrame();
        assert(pending_jobs == 1);
        draw_entities();
        XGL_AliasFinishFrame();
        assert(executed_jobs == 1 && strip_calls == calls && strip_points == points);
        assert(captured_count == count);
        sort_capture();
        assert(!memcmp(captured, expected, (size_t)count * sizeof(expected[0])));
        smp_active = 0; cl_numvisedicts = 0;
        /* A model the device illuminates needs normals: no strips. */
        xgl_alias_shade.value = 0;
        strip_calls = 0;
        for (i = 0; i < 2; ++i) {
            captured_count = 0; xgl_entity_extra_used = 0;
            XGL_DrawAlias(entity);
        }
        capture_reserved();
        assert(!strip_calls && captured_count == count);
        XGL_EntitiesShutdown();
        assert(xgl_alias_topology_bytes == XGL_ALIAS_FIXED_BYTES);
        assert(xgl_alias_cache_bytes == sizeof(xgl_alias_cache));
    }
    /* Without the loader's lists, or with a first one that makes no sense,
     * a model has no strips and is drawn all the same. So it is when there
     * is no memory to join them with. A second list that makes no sense
     * costs it only the sharing of points. */
    for (failure = -3; failure < 16; ++failure) {
        reset_test();
        header = make_sheet(3, 2, failure == -3 ? 0 : (failure == -2 ? 2 : (failure == -1 ? 3 : 1)));
        entity = &cl_entities[1];
        xgl_alias_shade.value = 1;
        xgl_alias_strips.value = 2;
        XGL_DrawAlias(entity); /* The colors first. */
        if (failure >= 0) allocation_fail_after = failure;
        captured_count = 0;
        XGL_DrawAlias(entity);
        capture_reserved();
        allocation_fail_after = -1;
        assert(captured_count == 3 * header->numtris);
        topology = &xgl_alias_topologies[0];
        if (failure == -3) assert(!strip_calls && !topology->strip_count && topology->base_points == 24);
        if (failure == -2) assert(!strip_calls && !topology->strip_count && topology->base_points == 12);
        if (failure == -1) assert(strip_calls == 2 && topology->base_points == 13);
        /* Four allocations make the mesh and five its points; one is
         * scratch for the strips and four more keep them. */
        if (failure >= 0 && failure < 9) assert(!strip_calls && !topology->indices);
        if (failure >= 9 && failure < 14) assert(!strip_calls && topology->indices && !topology->strip_count);
        if (failure >= 14) assert(strip_calls == 2 && topology->base_points == 12);
        assert(xgl_alias_topology_bytes <= XGL_ALIAS_TOPOLOGY_BYTES);
        XGL_EntitiesShutdown();
        assert(xgl_alias_topology_bytes == XGL_ALIAS_FIXED_BYTES);
    }
}

int main(void)
{
    test_alias_pose_transform_translation();
    test_alias_primitive_winding();
    test_sprite_groups_coverage_axes();
    test_view_weapon_scope();
    test_entity_profile_categories();
    test_alias_lighting_selection();
    test_nested_alias_profile_paths();
    test_nested_weapon_profile_paths();
    test_alias_color_cache();
    test_alias_cache_coverage_and_capacity();
    test_alias_cache_allocation_failure();
    test_alias_shared_topology();
    test_alias_topology_byte_limit();
    test_alias_direct_partial_batches();
    test_alias_audio_lock_cleanup();
    test_smp_alias_frame();
    test_alias_identity_fast_path();
    test_alias_state_order();
    test_alias_cached_mesh_equivalence();
    test_alias_pose_canceling_normals();
    test_alias_seam_canceling_normals();
    test_alias_mesh_batching_and_limits();
    test_alias_mesh_allocation_failure();
    test_alias_cpu_shading();
    test_alias_strips();
    test_brush_entity_passes();
    puts("XGL entity contracts passed (CPU geometry; no native rendering)");
    return 0;
}
