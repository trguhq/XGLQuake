/* XGLQuake renderer interface, for the xgl_ files only. Include it after
 * quakedef.h. No OpenGL or native XGL SDK types leak here. */
#ifndef XGL_RENDER_H
#define XGL_RENDER_H

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

/* Matches XGL's RGB point/color record. Unlit geometry, which is nearly all
 * of it, is submitted without normals: the device draws such points about
 * a tenth faster and the cached meshes are a third smaller. */
typedef struct {
    float position[3];
    union { float rgb[3]; unsigned long alignment; } color;
} xgl_color_vertex_t;

extern byte xgl_palette[768];

void XGL_BeginFrame(void);
/* After BeginFrame and before BeginScene, pass the opaque console height
 * in logical screen pixels. Zero leaves the complete game view visible. */
/* Ask the next frame's clear to leave the bottom `lines` rows of the 2D
 * screen as they are, for a status bar that has not changed. The request
 * is for that one frame, and the backend may decline it. */
void XGL_SetFrameKeep(int lines);
/* For the frame begun last: 0 if everything was cleared; else the number
 * of frames running, this one included, whose clears left the same rows. */
int XGL_FrameKeepStreak(void);
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
/* The same two services for unlit geometry without normals. Colors are
 * prepared; borrowed vertices must stay unchanged until XGL_Flush. */
void XGL_DrawColorTriangles(const xgl_color_vertex_t *vertices, int count,
                           float opacity, int zwrite);
xgl_color_vertex_t *XGL_ReserveColorTriangles(int *count, float opacity, int zwrite);
/* Submit `count` points without normals, three or more, as one triangle
 * strip: each point after the second makes a triangle with the two before
 * it. The strip goes to the device before this returns, so the points need
 * not be kept. Colors are prepared. */
void XGL_DrawColorStrip(const xgl_color_vertex_t *points, int count,
                        float opacity, int zwrite);
unsigned int XGL_ColorRevision(void);
void XGL_CopyGammaOutput(float output[256]);
void XGL_PrepareColor(float rgb[3]);
void XGL_PreparePaletteColor(int index, float rgb[3]);
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
const byte *XGL_TextureTexels(int id, int *width, int *height);
/* xgl_shapes.c: a wall texture divided into pieces of nearly one color, for
 * walls drawn as those pieces. The division is a tree: node 0 is the whole
 * repeat of the texture, and every node that is not a leaf has one straight
 * cut and a child either side of it. Nodes are in the order their cuts were
 * found, most telling first, so the first n - 1 cuts give n pieces. */
#define XGL_SHAPE_PIECES 48
#define XGL_SHAPE_AXIS 3      /* flags & this: 0 a cut at s = d, 1 at t = d, 2 tilted */
#define XGL_SHAPE_PAIRED 4    /* this cut and child[1]'s take out a thin band: both or neither */
#define XGL_SHAPE_SECOND 8    /* the other cut of such a pair */
#define XGL_SHAPE_THIN_FAR 16 /* child[1] is the thinner of the two parts */
typedef struct {
    /* The cut a*s + b*t = d, in texels from the tree's origin and measured
     * at texel centers. child[1] takes the side where a*s + b*t >= d.
     * A leaf has child[0] < 0. */
    float a, b, d;
    /* gain: the squared color error the cut removes from one repeat of the
     * texture, for each piece it adds. width: about how many texels across
     * the thinner of its two parts is. */
    float gain, width;
    short child[2];
    byte flags;
} xgl_shape_node_t;
/* The steps of detail a wall cut along such a division can have. */
#define XGL_SHAPE_STEPS 24
typedef struct {
    int width, height;
    /* The texel the repeat is taken to begin at, so that its border falls
     * on an edge in the picture where there is one. */
    int origin[2];
    int count;
    xgl_shape_node_t *nodes;
    /* What each node's part of the texture is drawn in when it is a piece:
     * three bytes for each node. */
    byte *colors;
    /* Not the division's own: what the code that cuts walls along it has
     * worked out about its steps of detail, kept here to be found again.
     * steps_key is zero when the division is made. */
    int steps_key;
    byte steps[XGL_SHAPE_STEPS];
} xgl_shape_tree_t;
/* The division that walls of a texture are cut along, worked out the first
 * time it is asked for; NULL for a texture with no shapes worth following.
 * Every frame of an animated texture has the same one: that of the frame
 * with the most pieces. */
xgl_shape_tree_t *XGL_TextureShapes(texture_t *texture);
/* The color of each node of a division, three bytes for each, as a texture
 * shows it: the texture the division was made from, or another of the same
 * size laid over it, as the other frames of an animated texture are. NULL
 * when the texture does not fit the division or there is no memory. */
const byte *XGL_ShapeColors(const xgl_shape_tree_t *tree, texture_t *texture);
/* Forget every division: the textures are about to change. */
void XGL_ShapesClear(void);
/* How many textures have been examined since the last clear, and how many
 * of them have shapes. */
void XGL_ShapesCount(int *examined, int *shaped);
/* What a wall asks of a cut before it is made: that it be among the first
 * found, `most` being the last node those can have made; that it gain at
 * least `worth`; and that neither of its parts be narrower than `thin`
 * texels. A piece whose cut fails only the last goes on as the wider part,
 * the narrow one being too small to see. */
typedef struct {
    int most;
    float worth, thin;
} xgl_shape_terms_t;
/* How many of a division's cuts such terms make. Terms that ask no more
 * make every cut that terms asking more do. */
int XGL_ShapeCuts(const xgl_shape_tree_t *tree, const xgl_shape_terms_t *terms);
/* The most corners a face or a piece of one may have, and the most
 * triangles the pieces of one face may come to. */
#define XGL_SHAPE_CORNERS 96
#define XGL_SHAPE_TRIANGLES 256
/* Cut a face, given as the texture coordinates of its corners in order,
 * into pieces along a division. Pieces longer than `longest` texels are
 * halved. Returns the number of pieces; 0 when the face shows nothing that
 * the terms cut; -1 when the pieces are too many for one face or come to
 * more than XGL_SHAPE_TRIANGLES. */
int XGL_ShapeCutFace(const xgl_shape_tree_t *tree, const xgl_shape_terms_t *terms,
                     float (*st)[2], int count, float longest);
/* One piece of the face cut last. Its corners, in texture coordinates,
 * include those of its neighbors that lie along its sides. It is drawn as a
 * fan of triangles from its first corner or, with `center`, from its
 * middle around all its corners. `width` is about how many texels across
 * it is. */
typedef struct {
    float corner[XGL_SHAPE_CORNERS][2];
    float middle[2], width;
    int count, center;
    /* The node of the division it is: its color is that node's. */
    int node;
} xgl_shape_piece_t;
/* Returns 0 when there is no such piece. */
int XGL_ShapePiece(int index, xgl_shape_piece_t *piece);
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

#endif
