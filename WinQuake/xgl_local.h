/* What the original Quake sources need of the XGL renderer. Every source
 * includes this through quakedef.h, so it is kept small: the renderer's own
 * interface is in xgl_render.h, which only the xgl_ files include. */
#ifndef XGL_LOCAL_H
#define XGL_LOCAL_H

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

void XGL_Flush(void);
void XGL_DrawParticle(const particle_t *particle);
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
