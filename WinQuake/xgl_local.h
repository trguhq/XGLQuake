/* XGLQuake renderer interface. No OpenGL or native XGL SDK types leak here. */
#ifndef XGL_LOCAL_H
#define XGL_LOCAL_H

#define BACKFACE_EPSILON 0.01

/* The view weapon is drawn shrunk toward the eye by this factor: its image
 * is unchanged, but its depth lies in front of the world, so it needs no
 * depth clear. The near plane moves in by the same factor, which clips the
 * weapon exactly where the original four-unit plane did. */
#define XGL_VIEWMODEL_SCALE 0.125f
#define XGL_NEAR_CLIP (4.0 * XGL_VIEWMODEL_SCALE)
/* Particles keep the original four-unit limit: one at the eye would
 * otherwise fill the view. */
#define XGL_PARTICLE_NEAR 4.0f
/* Models are lit by ambient light and one fixed directional fill from above.
 * With xgl_alias_shade the CPU evaluates that sum for every vertex and the
 * models are submitted unlit; otherwise the device evaluates it. Both use
 * these values. The axis is each component of the unit vector that points
 * toward the fill light. */
#define XGL_ALIAS_AMBIENT .5f
#define XGL_ALIAS_FILL .5f
#define XGL_ALIAS_FILL_AXIS .577350269f
#define XGL_MAX_POINT_LIGHTS 4
/* XGL_BrushEntityLights also sets this bit when reduced ambient light alone
 * requires illuminated submission. */
#define XGL_LIT_AMBIENT 256

typedef enum {
    pt_static, pt_grav, pt_slowgrav, pt_fire, pt_explode, pt_explode2, pt_blob, pt_blob2
} ptype_t;

typedef struct particle_s {
    vec3_t org;
    float color;
    struct particle_s *next;
    vec3_t vel;
    float ramp, die;
    ptype_t type;
} particle_t;

typedef struct {
    vec3_t position, normal;
    float color[3];
} xgl_vertex_t;

/* A dynamic light chosen for the scene. Geometry further than range from it
 * is not illuminated by it; a range of zero means no limit. */
typedef struct {
    vec3_t origin;
    float radius, range;
} xgl_point_light_t;

/* Matches XGL's RGB point/color/normal record without requiring the SDK in
 * CPU-only renderer files. The native backend checks size and field offsets.
 * XGL's color union includes an unsigned long, including on host-test builds. */
typedef struct {
    float position[3];
    union { float rgb[3]; unsigned long alignment; } color;
    float normal[3];
} xgl_native_vertex_t;

extern byte xgl_palette[768];
extern entity_t r_worldentity, *currententity;
extern vec3_t modelorg, r_entorigin;
extern int r_visframecount, r_framecount, c_brush_polys, c_alias_polys;
extern mplane_t frustum[4];
extern mleaf_t *r_viewleaf, *r_oldviewleaf;
extern int d_lightstylevalue[256];
extern float r_avertexnormals[162][3];
extern cvar_t r_norefresh, r_drawentities, r_drawviewmodel, r_speeds;
extern cvar_t r_fullbright, r_wateralpha, r_dynamic, r_novis;
extern cvar_t cl_crossx, cl_crossy;
extern float v_blend[4];
extern byte gammatable[256];

void XGL_BeginFrame(void);
/* After BeginFrame and before BeginScene, pass the opaque console height
 * in logical screen pixels. Zero leaves the complete game view visible. */
void XGL_SetConsoleClip(int lines);
void XGL_BeginScene(void);
/* Positions and unit normals are in Quake world coordinates. */
void XGL_DrawTriangles(const xgl_vertex_t *vertices, int count, int lit,
                      float opacity, int zwrite);
void XGL_DrawTrianglesStrided(const xgl_vertex_t *vertices, int count,
                             size_t stride, int lit, float opacity,
                             int zwrite, int prepared_color);
/* Prepared native colors are already gamma corrected. Borrowed vertices
 * must remain unchanged until XGL_Flush, which submits volatile XGL data. */
void XGL_DrawNativeTriangles(const xgl_native_vertex_t *vertices, int count,
                            int lit, float opacity, int zwrite);
/* Reserve up to *count vertices (a multiple of three) in the backend batch.
 * Fill them before any other draw or flush; color preparation is safe.
 * No separate commit is needed.
 * Returns NULL and sets *count to zero when no scene is active. */
xgl_native_vertex_t *XGL_ReserveTriangles(int *count, int lit,
                                         float opacity, int zwrite);
unsigned int XGL_ColorRevision(void);
void XGL_CopyGammaOutput(float output[256]);
void XGL_PrepareColor(float rgb[3]);
void XGL_PreparePaletteColor(int index, float rgb[3]);
void XGL_Flush(void);
void XGL_EndScene(void);
void XGL_EndFrame(void);
/* The scene's dynamic lights, nearest the viewer first; at most
 * XGL_MAX_POINT_LIGHTS. Valid after XGL_BeginScene. */
int XGL_PointLights(const xgl_point_light_t **lights);
/* Bit i set: light i reaches some geometry that will be drawn illuminated.
 * The others are switched off for it. BeginScene selects them all. */
void XGL_UsePointLights(int mask);
/* xgl_ambient, bounded to 0..1. Below 1, unlit submission would differ from
 * illuminated submission even with no dynamic light. */
float XGL_AmbientLight(void);
void XGL_UIText(float x, float y, const char *text, int color,
                float cell_width, float height);
void XGL_UIFill(float x, float y, float width, float height,
                int color, float opacity);
void XGL_UIFillRGB(float x, float y, float width, float height,
                  const float rgb[3], float opacity);
void XGL_UIFlush(void);
void XGL_DrawFlush(void);
int XGL_ReadScreen(byte *rgb, int width, int height);
void XGL_DrawParticle(const particle_t *particle);
void XGL_PolyBlend(void);

int XGL_LoadTexture(char *identifier, int width, int height, byte *data,
                    qboolean mipmap, qboolean alpha);
qboolean XGL_SampleTexture(int id, float s, float t, float rgb[3],
                          const byte *translation);
qboolean XGL_SampleTextureIndex(int id, float s, float t, int *index);
unsigned int XGL_TextureRevision(int id);
qboolean XGL_TextureHasAlpha(int id);
qboolean XGL_SampleTextureFiltered(int id, float s, float t,
                                  float footprint_s, float footprint_t,
                                  float rgb[3], const byte *translation);
/* The mean color of a box of texels about footprint wide around (s, t),
 * all three in texels. Integer averaging of reduced images; no translation. */
qboolean XGL_SampleTextureBox(int id, float s, float t, float footprint, byte rgb[3]);
void XGL_TextureInit(void);
void XGL_TextureShutdown(void);
void XGL_MakeAliasModelDisplayLists(model_t *model, aliashdr_t *header);
void XGL_SubdivideSurface(msurface_t *surface);

void XGL_WorldNewMap(void);
void XGL_WorldShutdown(void);
/* XGL_DrawWorld finds the visible faces and draws the walls a dynamic light
 * reaches, illuminated; XGL_DrawWorldUnlit draws the others. With no such
 * light XGL_DrawWorld draws every wall unlit. */
void XGL_DrawWorld(void);
void XGL_DrawWorldUnlit(void);
void XGL_DrawWorldSpecial(void);
void XGL_DrawBrushModel(entity_t *entity);
/* Nonzero when the brush entity must be submitted illuminated: the scene
 * lights that reach it, as XGL_UsePointLights bits, plus XGL_LIT_AMBIENT. */
int XGL_BrushEntityLights(entity_t *entity);
void XGL_DrawWater(void);
void XGL_AliasPrepareFrame(void);
void XGL_AliasFinishFrame(void);
/* Entity drawing is split by native state so that R_RenderView can order
 * the frame with as few lit/unlit changes as possible. */
void XGL_DrawBrushEntities(int lit);
void XGL_DrawSprites(void);
void XGL_DrawAliasEntities(void);
void XGL_DrawViewModel(void);
void XGL_EntitiesInit(void);
void XGL_EntitiesShutdown(void);
void R_TranslatePlayerSkin(int playernum);
int R_LightPoint(vec3_t point);
void R_AnimateLight(void);
qboolean R_CullBox(vec3_t mins, vec3_t maxs);
texture_t *R_TextureAnimation(texture_t *base);
void R_InitParticles(void);
void R_ClearParticles(void);
void R_DrawParticles(void);
void R_ReadPointFile_f(void);
void R_StoreEfrags(efrag_t **ppefrag);
void V_CalcBlend(void);
int XGL_CheckPackDirectory(const char *path, int count, unsigned short crc);
void XGL_SharewareDiagnostics(void);

#endif
