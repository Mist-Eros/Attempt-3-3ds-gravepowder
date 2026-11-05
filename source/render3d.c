/*
 * Gravepowder - citro3d 3D front end
 *
 * Owns: gfx/C3D init, the two render targets, the shader program, every mesh
 * (procedural, baked once into linearAlloc memory) and the world drawing for
 * the top screen.  Reads GraveGame and the camera fields filled by the core;
 * never mutates game state.
 *
 * Rendering notes:
 *  - citro2d (the bottom HUD) pollutes the GPU state, so the 3D pass
 *    re-establishes program/attributes/texenv/depth/cull at its start every
 *    frame (see bind_pass_state).
 *  - One vertex shader does flat directional+ambient lighting and per-vertex
 *    fog; no fragment shaders are needed (Makefile only builds .v.pica).
 *  - Zombies use one baked pose mesh -> one draw call each.
 *  - Effects (tracers, sparks, muzzle flash, smoke) are batched into one
 *    dynamic VBO and drawn additively.
 */
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "render3d.h"
#include "gpm.h"
#include "mesh.h"
#include "skin.h"
#include "vshader_shbin.h"

/* ------------------------------------------------------------------ */
/* embedded default models (data/models/<name>.gpm, converted at build) */
/*                                                                     */
/* Missing headers keep the build green before the converter has run;  */
/* the runtime then falls back to the procedural meshes.  SD-card       */
/* overrides (sdmc:/3ds/Gravepowder/models/<name>.gpm) win over these.  */
/* ------------------------------------------------------------------ */

#if defined(__has_include)
#  if __has_include("hero_gpm.h")
#    include "hero_gpm.h"
#    define R3D_HAVE_EMBEDDED_HERO 1
#  endif
#  if __has_include("zombie_gpm.h")
#    include "zombie_gpm.h"
#    define R3D_HAVE_EMBEDDED_ZOMBIE 1
#  endif
#  if __has_include("gun_flintlock_gpm.h")
#    include "gun_flintlock_gpm.h"
#    define R3D_HAVE_EMBEDDED_GUN_FLINTLOCK 1
#  endif
#  if __has_include("gun_revolver_gpm.h")
#    include "gun_revolver_gpm.h"
#    define R3D_HAVE_EMBEDDED_GUN_REVOLVER 1
#  endif
#  if __has_include("gun_musket_gpm.h")
#    include "gun_musket_gpm.h"
#    define R3D_HAVE_EMBEDDED_GUN_MUSKET 1
#  endif
#endif

#ifndef R3D_HAVE_EMBEDDED_HERO
#define R3D_HAVE_EMBEDDED_HERO 0
#endif
#ifndef R3D_HAVE_EMBEDDED_ZOMBIE
#define R3D_HAVE_EMBEDDED_ZOMBIE 0
#endif
#ifndef R3D_HAVE_EMBEDDED_GUN_FLINTLOCK
#define R3D_HAVE_EMBEDDED_GUN_FLINTLOCK 0
#endif
#ifndef R3D_HAVE_EMBEDDED_GUN_REVOLVER
#define R3D_HAVE_EMBEDDED_GUN_REVOLVER 0
#endif
#ifndef R3D_HAVE_EMBEDDED_GUN_MUSKET
#define R3D_HAVE_EMBEDDED_GUN_MUSKET 0
#endif

#define PI_F 3.14159265f

/* ------------------------------------------------------------------ */
/* constants                                                          */
/* ------------------------------------------------------------------ */

#define R3D_TOP_W 240
#define R3D_TOP_H 400
#define R3D_BOT_W 240
#define R3D_BOT_H 320

/* dusty overcast sky; the fog fades into exactly this colour */
#define R3D_CLEAR_COLOR 0x7A6E58FFu

#define R3D_NEAR      0.10f
#define R3D_FAR       160.0f
#define R3D_FOG_NEAR  8.0f
#define R3D_FOG_FAR   58.0f
#define R3D_FOG_INV   (1.0f / (R3D_FOG_FAR - R3D_FOG_NEAR))

#define R3D_SKY_R 0.4784f
#define R3D_SKY_G 0.4314f
#define R3D_SKY_B 0.3451f

#define R3D_LIGHT_R 0.86f
#define R3D_LIGHT_G 0.72f
#define R3D_LIGHT_B 0.47f

#define R3D_AMBIENT_R 0.42f
#define R3D_AMBIENT_G 0.42f
#define R3D_AMBIENT_B 0.46f

#define R3D_FILL_R 0.30f
#define R3D_FILL_G 0.26f
#define R3D_FILL_B 0.22f
/* ------------------------------------------------------------------ */
/* per-zone atmosphere palettes (v0.3)                                */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned clear_color;              /* 0xRRGGBBAA                        */
    float    fog_r, fog_g, fog_b;      /* fog + horizon colour              */
    float    fog_near, fog_far;
    float    light_r, light_g, light_b;
    float    ambient_r, ambient_g, ambient_b;
    float    fill_r, fill_g, fill_b;
    float    ground_r, ground_g, ground_b;  /* tint on the baked ground mesh */
    float    hills_r, hills_g, hills_b;     /* distant hills tint            */
    float    dir_x, dir_y, dir_z;      /* world-space sun direction         */
    float    glow;                     /* 0..1 lantern/ore light pool gain  */
} R3dZonePalette;

/* Ashen Mill keeps the v0.2 dusty tan exactly; Bone Orchard is a colder
 * grey-green night; Old Mine is dark brown-grey with low warm amber fog. */
static const R3dZonePalette s_zone_palettes[ZONE_COUNT] = {
    /* ZONE_ASHEN_MILL */
    { 0x7A6E58FFu, 0.4784f, 0.4314f, 0.3451f, 8.0f, 58.0f,
      0.86f, 0.72f, 0.47f, 0.42f, 0.42f, 0.46f, 0.30f, 0.26f, 0.22f,
      1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, -0.42f, 0.86f, 0.30f, 0.0f },
    /* ZONE_BONE_ORCHARD */
    { 0x596257FFu, 0.34f, 0.38f, 0.34f, 7.0f, 50.0f,
      0.60f, 0.72f, 0.60f, 0.32f, 0.36f, 0.34f, 0.22f, 0.26f, 0.23f,
      0.90f, 0.96f, 0.90f, 0.80f, 0.86f, 0.80f, -0.30f, 0.80f, 0.40f, 0.15f },
    /* ZONE_OLD_MINE */
    { 0x2B2118FFu, 0.23f, 0.175f, 0.125f, 4.0f, 34.0f,
      0.98f, 0.64f, 0.28f, 0.21f, 0.175f, 0.19f, 0.24f, 0.16f, 0.10f,
      0.68f, 0.60f, 0.53f, 0.36f, 0.28f, 0.21f, -0.30f, 0.75f, 0.45f, 1.0f },
};

static const R3dZonePalette *s_zone_pal;

static const R3dZonePalette *r3d_zone_palette(ZoneId zone)
{
    if ((int)zone < 0 || (int)zone >= ZONE_COUNT)
        return &s_zone_palettes[ZONE_ASHEN_MILL];
    return &s_zone_palettes[(int)zone];
}

static void r3d_zone_select(ZoneId zone)
{
    s_zone_pal = r3d_zone_palette(zone);
}

static const R3dZonePalette *r3d_palette(void)
{
    if (s_zone_pal == NULL)
        s_zone_pal = &s_zone_palettes[ZONE_ASHEN_MILL];
    return s_zone_pal;
}

#define R3D_MESH_ARENA_BYTES (2u * 1024u * 1024u)
#define R3D_SCRATCH_VERTS    16384
#define R3D_DYN_VERTS        2048
#define R3D_MAX_PUFFS        24
#define R3D_DUST_COUNT      16

/* imported-model runtime (GPM v1) */
#define R3D_HERO_HEIGHT        1.88f  /* hat-top height in metres (Lead: 1.85-1.90) */
#define R3D_HERO_HEAD_TOP      0.417f /* procedural hat top above the Head bone */
#define R3D_NEAR_ZOMBIES       3      /* fully skinned zombies (27k verts cap) */
#define R3D_SKIN_BUDGET        27000  /* max skinned verts/frame total (tuned) */
#define R3D_ZOMBIE_LOD_DIST    14.0f  /* further zombies reuse a shared pose  */
#define R3D_MODEL_DIR          "sdmc:/3ds/Gravepowder/models/"
#define R3D_MAX_MODEL_VERTS    12000  /* dynamic VBO capacity per instance    */
#define R3D_FOREMAN_HEIGHT     2.80f  /* v0.3 boss body height (~1.65x shambler) */

/* core's muzzle_flash timer counts down from 0.07 s */
#define R3D_MUZZLE_FLASH_TIME 0.07f

#define R3D_GROUND_HALF_EXTENT 80.0f
#define R3D_GROUND_CELLS       40
#define R3D_HILL_RADIUS        52.0f

#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

/* ------------------------------------------------------------------ */
/* state                                                              */
/* ------------------------------------------------------------------ */

static bool s_initialized;

static DVLB_s *s_dvlb;
static shaderProgram_s s_program;
static int s_u_projection;
static int s_u_model_view;
static int s_u_tint;
static int s_u_light_dir;
static int s_u_light_color;
static int s_u_ambient;
static int s_u_fill_color;
static int s_u_fog_color;
static int s_u_fog_params;

static C3D_AttrInfo s_attr;

static C3D_RenderTarget *s_target_top;
static C3D_RenderTarget *s_target_top_right;
static C3D_RenderTarget *s_target_bottom;
static bool s_display_3d;

static void *s_arena_mem;
static MeshArena s_arena;
static void *s_dyn_mem;
static MeshVertex *s_dyn_verts;
static int s_dyn_count;

static Mesh s_ground;
static Mesh s_hills;
static Mesh s_shadow;
static Mesh s_unit_cube;
static Mesh s_props[PROP_KIND_COUNT];
static Mesh s_pickups[DROP_COUNT];
static Mesh s_weapons[WEAPON_COUNT];
static Mesh s_zombie_poses[ZOMBIE_COUNT][ZOMBIE_POSE_COUNT];

static HeroRig s_hero_builtin;
static const HeroRig *s_hero_rig;
static bool s_hero_rig_override;   /* user installed a procedural rig */

/* ------------------------------------------------------------------ */
/* imported model state (GPM v1 + CPU skinning)                       */
/* ------------------------------------------------------------------ */

static GpmModel s_gpm_hero;
static bool s_gpm_hero_ok;
static GpmModel s_gpm_zombie;
static bool s_gpm_zombie_ok;
static GpmModel s_gpm_gun[WEAPON_COUNT];
static bool s_gpm_gun_ok[WEAPON_COUNT];

/* Imported static gun mesh: GPM vertex data + indices uploaded once into
 * linearAlloc memory and drawn indexed (smaller and fully reloadable, unlike
 * baking expanded meshes into the static arena). */
typedef struct {
    MeshVertex *verts;      /* linearAlloc'd, MESH_VERTEX_STRIDE layout     */
    uint16_t   *ibo;        /* linearAlloc'd index buffer                   */
    int         vert_count;
    int         index_count;
} R3dGunMesh;

static R3dGunMesh s_gpm_gun_mesh[WEAPON_COUNT];
static float s_gpm_gun_scale[WEAPON_COUNT];
static Vec3 s_gpm_gun_muzzle_local[WEAPON_COUNT]; /* +Z barrel tip, model space */

/* Where each model slot came from, for the wardrobe page. */
typedef enum {
    R3D_SRC_NONE = 0,
    R3D_SRC_SD,
    R3D_SRC_EMBEDDED,
    R3D_SRC_PROCEDURAL,
    R3D_SRC_CUSTOM
} R3dModelSrcKind;

static R3dModelSrcKind s_src_hero;
static R3dModelSrcKind s_src_zombie;
static R3dModelSrcKind s_src_gun[WEAPON_COUNT];
static char s_model_source_text[R3D_MODEL_COUNT][64];

/* hero runtime */
static SkinInstance s_hero_skin;
static bool s_gpm_hero_ready;
static MeshVertex *s_hero_vbo;      /* linearAlloc'd dynamic skinned verts  */
static uint16_t *s_hero_ibo;        /* linearAlloc'd static index buffer    */
static int s_hero_vert_cap;
static int s_hero_index_count;
static Mesh s_wolf_head;            /* procedural wolf head+ears+hat        */
static Mesh s_wolf_tail[3];         /* procedural bushy tail segments       */
static int s_hero_bone_head = -1;
static int s_hero_bone_hips = -1;
static int s_hero_bone_wrist = -1;
static Vec3 s_hero_head_rest;
static Vec3 s_hero_hips_rest;
static Vec3 s_hero_wrist_rest;
static float s_hero_model_scale = 1.0f;
static float s_hero_bbox_min_y;
static int s_hero_clip_idle = -1, s_hero_clip_walk = -1, s_hero_clip_run = -1;
static int s_hero_clip_shoot = -1, s_hero_clip_hit = -1, s_hero_clip_death = -1;
static float s_hero_shoot_clock;
static float s_hero_death_clock;
static bool s_hero_was_firing;
static bool s_hero_was_alive = true;

/* zombie runtime (nearest N skinned per frame + one shared far pose) */
static SkinInstance s_zombie_skin[R3D_NEAR_ZOMBIES];
static MeshVertex *s_zombie_vbo[R3D_NEAR_ZOMBIES];
static bool s_zombie_slot_inited[R3D_NEAR_ZOMBIES];
static uint16_t *s_zombie_ibo;
static int s_zombie_index_count;
static int s_zombie_vert_count;
static int s_zombie_vert_cap;
static SkinInstance s_zombie_far_skin;
static MeshVertex *s_zombie_far_vbo;
static bool s_zombie_far_inited;
static float s_zombie_far_clock;
static float s_zombie_model_scale = 0.93f;
static float s_zombie_bbox_min_y;
static int s_zombie_bone_head = -1;   /* Head bone on the zombie model (boss eye anchor) */
static Vec3 s_boss_head_pos;          /* skinned head world position, refreshed per frame */
static bool s_boss_head_valid;
static int s_zombie_clip_idle = -1, s_zombie_clip_walk = -1, s_zombie_clip_run = -1;
static int s_zombie_clip_attack = -1, s_zombie_clip_hit = -1, s_zombie_clip_death = -1;

/* frame delta for skinned animation (set once per r3d_draw_world) */
static float s_draw_dt;

static C3D_Mtx s_ortho;
static C3D_Mtx s_last_projection;

static bool s_stereo_enabled;
static float s_stereo_iod;

/* frame stats */
static unsigned s_draw_calls;
static unsigned s_draw_verts;
static unsigned s_mesh_verts;
static unsigned s_mesh_bytes;
static unsigned s_skin_verts_frame;    /* verts skinned this frame             */
static unsigned s_skin_bones_frame;    /* bone poses evaluated this frame      */
static unsigned s_skin_instances_frame;/* skinned instances updated this frame */
static unsigned s_dynamic_bytes;      /* linearAlloc'd skinned/dynamic memory */

/* smoke puffs + muzzle tracking */
typedef struct {
    Vec3  pos;
    Vec3  vel;
    float age;
    float life;
    float size;
    bool  active;
} SmokePuff;

static SmokePuff s_puffs[R3D_MAX_PUFFS];
static int s_puff_next;
static Vec3 s_dust[R3D_DUST_COUNT];
static bool s_dust_ready;
static float s_elapsed;
static float s_prev_muzzle;
static Vec3 s_last_muzzle;
static bool s_have_last_muzzle;

/* title screen diorama clock */
static float s_title_clock;

/* ------------------------------------------------------------------ */
/* small maths                                                        */
/* ------------------------------------------------------------------ */

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static Vec3 v3(float x, float y, float z)
{
    return vec3_make(x, y, z);
}

static Vec3 v3_add(Vec3 a, Vec3 b)
{
    return v3(a.x + b.x, a.y + b.y, a.z + b.z);
}

static Vec3 v3_sub(Vec3 a, Vec3 b)
{
    return v3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static Vec3 v3_scale(Vec3 a, float s)
{
    return v3(a.x * s, a.y * s, a.z * s);
}

static float v3_dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vec3 v3_cross(Vec3 a, Vec3 b)
{
    return v3(a.y * b.z - a.z * b.y,
              a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x);
}

static Vec3 v3_norm(Vec3 a)
{
    float len = sqrtf(v3_dot(a, a));
    if (len < 1e-6f)
        return v3(0.0f, 1.0f, 0.0f);
    return v3_scale(a, 1.0f / len);
}

static float hash01(unsigned x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return (float)(x & 0xFFFFu) / 65535.0f;
}

/* ------------------------------------------------------------------ */
/* GPU state                                                          */
/* ------------------------------------------------------------------ */

static void bind_pass_state(const C3D_Mtx *projection, const C3D_Mtx *view)
{
    int i;
    const R3dZonePalette *pal = r3d_palette();
    Vec3 light = v3_norm(v3(pal->dir_x, pal->dir_y, pal->dir_z));
    float lx = view->r[0].x * light.x + view->r[0].y * light.y + view->r[0].z * light.z;
    float ly = view->r[1].x * light.x + view->r[1].y * light.y + view->r[1].z * light.z;
    float lz = view->r[2].x * light.x + view->r[2].y * light.y + view->r[2].z * light.z;
    float fog_inv = 1.0f / (pal->fog_far - pal->fog_near);

    C3D_BindProgram(&s_program);
    C3D_SetAttrInfo(&s_attr);

    for (i = 0; i < 6; i++)
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    {
        C3D_TexEnv *env = C3D_GetTexEnv(0);
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    }

    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_BACK_CCW);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0xFF);
    C3D_ColorLogicOp(GPU_LOGICOP_COPY);

    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_u_projection, projection);
    s_last_projection = *projection;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u_light_dir, lx, ly, lz, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u_light_color,
                  pal->light_r, pal->light_g, pal->light_b, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u_ambient,
                  pal->ambient_r, pal->ambient_g, pal->ambient_b, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u_fill_color,
                  pal->fill_r, pal->fill_g, pal->fill_b, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u_fog_color,
                  pal->fog_r, pal->fog_g, pal->fog_b, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u_fog_params,
                  -pal->fog_near * fog_inv, -fog_inv, 0.0f, 0.0f);
}

/* ------------------------------------------------------------------ */
/* mesh drawing                                                       */
/* ------------------------------------------------------------------ */

static void draw_mesh_mv(const C3D_Mtx *model_view, const Mesh *mesh,
                         float r, float g, float b, float unlit)
{
    C3D_BufInfo *buf;

    if (mesh == NULL || mesh->vbo == NULL || mesh->vertex_count == 0)
        return;

    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_u_model_view, model_view);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u_tint, r, g, b, unlit);

    buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, mesh->vbo, MESH_VERTEX_STRIDE, MESH_ATTR_COUNT,
                MESH_ATTR_PERMUTATION);
    C3D_DrawArrays(GPU_TRIANGLES, 0, (int)mesh->vertex_count);

    s_draw_calls++;
    s_draw_verts += mesh->vertex_count;
}

static void model_transform(C3D_Mtx *m, Vec3 pos, float yaw, Vec3 scale)
{
    Mtx_Identity(m);
    Mtx_Translate(m, pos.x, pos.y, pos.z, true);
    Mtx_RotateY(m, yaw, true);
    Mtx_Scale(m, scale.x, scale.y, scale.z);
}

static void draw_model(const C3D_Mtx *view, const Mesh *mesh, const C3D_Mtx *model,
                       float r, float g, float b, float unlit)
{
    C3D_Mtx mv;
    Mtx_Multiply(&mv, view, model);
    draw_mesh_mv(&mv, mesh, r, g, b, unlit);
}

/* Cheap blob shadow to ground characters/props on the dirt. */
static void draw_shadow(const C3D_Mtx *view, Vec3 pos, float radius_x, float radius_z)
{
    C3D_Mtx model;

    if (s_shadow.vbo == NULL)
        return;
    if (radius_x < 0.05f)
        radius_x = 0.05f;
    if (radius_z < 0.05f)
        radius_z = 0.05f;

    Mtx_Identity(&model);
    Mtx_Translate(&model, pos.x, 0.025f, pos.z, true);
    Mtx_Scale(&model, radius_x * 2.0f, 1.0f, radius_z * 2.0f);
    draw_model(view, &s_shadow, &model, 0.10f, 0.09f, 0.08f, 1.0f);
}

/* Cheap frustum-ish visibility test: rejects behind-camera and far geometry. */
static bool world_visible(const C3D_Mtx *view, Vec3 c, float radius)
{
    float vx = view->r[0].x * c.x + view->r[0].y * c.y + view->r[0].z * c.z + view->r[0].w;
    float vy = view->r[1].x * c.x + view->r[1].y * c.y + view->r[1].z * c.z + view->r[1].w;
    float vz = view->r[2].x * c.x + view->r[2].y * c.y + view->r[2].z * c.z + view->r[2].w;
    float depth;

    if (vz > radius)
        return false;
    depth = -vz;
    if (depth > r3d_palette()->fog_far + radius)
        return false;
    if (fabsf(vx) > depth * 1.8f + radius)
        return false;
    if (fabsf(vy) > depth * 1.2f + radius)
        return false;
    return true;
}

/* ------------------------------------------------------------------ */
/* dynamic effect geometry                                            */
/* ------------------------------------------------------------------ */

static void dyn_reset(void)
{
    s_dyn_count = 0;
}

static MeshVertex *dyn_push(int count)
{
    MeshVertex *p;

    if (s_dyn_verts == NULL)
        return NULL;
    if (s_dyn_count + count > R3D_DYN_VERTS)
        return NULL;
    p = &s_dyn_verts[s_dyn_count];
    s_dyn_count += count;
    return p;
}

static void dyn_vertex(MeshVertex *v, Vec3 p, Vec3 n, float r, float g, float b)
{
    v->pos[0] = p.x;
    v->pos[1] = p.y;
    v->pos[2] = p.z;
    v->nrm[0] = n.x;
    v->nrm[1] = n.y;
    v->nrm[2] = n.z;
    v->col[0] = r;
    v->col[1] = g;
    v->col[2] = b;
}

static void dyn_quad(Vec3 centre, Vec3 right, Vec3 up, float hw, float hh,
                     float r, float g, float b)
{
    MeshVertex *v = dyn_push(6);
    Vec3 p0, p1, p2, p3, n;

    if (v == NULL)
        return;
    p0 = v3_add(v3_sub(centre, v3_scale(right, hw)), v3_scale(up, -hh));
    p1 = v3_add(v3_add(centre, v3_scale(right, hw)), v3_scale(up, -hh));
    p2 = v3_add(v3_add(centre, v3_scale(right, hw)), v3_scale(up, hh));
    p3 = v3_add(v3_sub(centre, v3_scale(right, hw)), v3_scale(up, hh));
    n = v3_norm(v3_cross(right, up));

    dyn_vertex(&v[0], p0, n, r, g, b);
    dyn_vertex(&v[1], p1, n, r, g, b);
    dyn_vertex(&v[2], p2, n, r, g, b);
    dyn_vertex(&v[3], p0, n, r, g, b);
    dyn_vertex(&v[4], p2, n, r, g, b);
    dyn_vertex(&v[5], p3, n, r, g, b);
}

static void dyn_ribbon(Vec3 a, Vec3 b, Vec3 side, float half_w,
                       float r, float g, float bl)
{
    MeshVertex *v = dyn_push(6);
    Vec3 p0, p1, p2, p3, n, dir;

    if (v == NULL)
        return;
    dir = v3_sub(b, a);
    n = v3_norm(v3_cross(dir, side));
    p0 = v3_sub(a, v3_scale(side, half_w));
    p1 = v3_add(a, v3_scale(side, half_w));
    p2 = v3_add(b, v3_scale(side, half_w));
    p3 = v3_sub(b, v3_scale(side, half_w));

    dyn_vertex(&v[0], p0, n, r, g, bl);
    dyn_vertex(&v[1], p1, n, r, g, bl);
    dyn_vertex(&v[2], p2, n, r, g, bl);
    dyn_vertex(&v[3], p0, n, r, g, bl);
    dyn_vertex(&v[4], p2, n, r, g, bl);
    dyn_vertex(&v[5], p3, n, r, g, bl);
}

static void dyn_flush(const C3D_Mtx *model_view, float r, float g, float b, float unlit)
{
    C3D_BufInfo *buf;

    if (s_dyn_verts == NULL || s_dyn_count <= 0)
        return;

    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_u_model_view, model_view);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u_tint, r, g, b, unlit);
    buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, s_dyn_verts, MESH_VERTEX_STRIDE, MESH_ATTR_COUNT,
                MESH_ATTR_PERMUTATION);
    C3D_DrawArrays(GPU_TRIANGLES, 0, s_dyn_count);

    s_draw_calls++;
    s_draw_verts += (unsigned)s_dyn_count;
}

/* ------------------------------------------------------------------ */
/* camera                                                             */
/* ------------------------------------------------------------------ */

static void camera_from_game(const GraveGame *game, Vec3 *pos, Vec3 *look, float *fov)
{
    bool valid = false;

    *pos = v3(0.0f, 2.6f, -5.8f);
    *look = v3(0.0f, 1.25f, 0.6f);
    *fov = 62.0f;

    if (game != NULL) {
        Vec3 p = game->camera_pos;
        Vec3 l = game->camera_look;
        float d = sqrtf(v3_dot(v3_sub(l, p), v3_sub(l, p)));

        if (d > 0.05f && fabsf(p.x) < 500.0f && fabsf(p.y) < 500.0f &&
            fabsf(p.z) < 500.0f && fabsf(l.x) < 500.0f && fabsf(l.y) < 500.0f &&
            fabsf(l.z) < 500.0f) {
            *pos = p;
            *look = l;
            valid = true;
        }
        if (valid && game->camera_fov >= 5.0f && game->camera_fov <= 120.0f)
            *fov = game->camera_fov;
    }
}

static void build_view(const Vec3 *pos, const Vec3 *look, C3D_Mtx *view)
{
    Mtx_LookAt(view,
               FVec3_New(pos->x, pos->y, pos->z),
               FVec3_New(look->x, look->y, look->z),
               FVec3_New(0.0f, 1.0f, 0.0f),
               false);
}

static void camera_basis(const Vec3 *pos, const Vec3 *look,
                         Vec3 *fwd, Vec3 *right, Vec3 *up)
{
    *fwd = v3_norm(v3_sub(*look, *pos));
    *right = v3_norm(v3_cross(*fwd, v3(0.0f, 1.0f, 0.0f)));
    *up = v3_cross(*right, *fwd);
}

/* ------------------------------------------------------------------ */
/* weapon helpers                                                     */
/* ------------------------------------------------------------------ */

/* barrel reach in weapon-local +Z, metres */
static const float s_muzzle_z[WEAPON_COUNT] = { 0.21f, 0.21f, 0.60f };

/* first person viewmodel placement (camera space) */
static const float s_vm_x[WEAPON_COUNT] = { 0.200f, 0.200f, 0.210f };
static const float s_vm_y[WEAPON_COUNT] = { -0.145f, -0.140f, -0.165f };
static const float s_vm_z[WEAPON_COUNT] = { -0.36f, -0.34f, -0.46f };

static WeaponId clamp_weapon(WeaponId w)
{
    if ((int)w < 0 || (int)w >= WEAPON_COUNT)
        return WEAPON_FLINTLOCK;
    return w;
}

/* ------------------------------------------------------------------ */
/* hero                                                               */
/* ------------------------------------------------------------------ */

static void hero_pose_angles(const Player *player, float t, bool idle_only,
                             Vec3 angles[RIG_CH_COUNT], float *bob)
{
    float speed = idle_only ? 0.0f : clampf(player->anim_speed, 0.0f, 1.0f);
    float phase = player->anim_time;
    float sw = sinf(phase) * speed;
    int i;

    for (i = 0; i < RIG_CH_COUNT; i++)
        angles[i] = v3(0.0f, 0.0f, 0.0f);

    *bob = speed * 0.028f * fabsf(sinf(phase)) +
           0.008f * sinf(t * 2.1f) * (1.0f - speed);

    angles[RIG_CH_TORSO].x = 0.04f + 0.07f * speed - clampf(player->recoil, 0.0f, 0.2f) * 1.5f;
    angles[RIG_CH_HEAD].x = -clampf(player->transform.pitch, -1.2f, 1.2f) * 0.55f +
                            0.02f * sinf(t * 1.7f);
    angles[RIG_CH_HEAD].y = 0.04f * sinf(t * 0.9f);

    angles[RIG_CH_LEG_A].x = -0.55f * sw;
    angles[RIG_CH_LEG_B].x = 0.55f * sw;
    angles[RIG_CH_KNEE_A].x = 0.22f * speed +
        0.42f * speed * (0.5f + 0.5f * sinf(phase + 1.6f));
    angles[RIG_CH_KNEE_B].x = 0.22f * speed +
        0.42f * speed * (0.5f + 0.5f * sinf(phase + 1.6f + PI_F));

    angles[RIG_CH_ARM_A].x = 0.45f * sw;
    angles[RIG_CH_ELBOW_A].x = -0.20f - 0.15f * speed;
    angles[RIG_CH_ARM_B].x = -0.18f + 0.10f * sw;
    angles[RIG_CH_ELBOW_B].x = -0.62f;
    angles[RIG_CH_ARM_B].z = -0.06f;

    angles[RIG_CH_COAT_A].x = 0.12f + 0.28f * speed * sinf(phase + PI_F);
    angles[RIG_CH_COAT_B].x = 0.12f + 0.28f * speed * sinf(phase + PI_F + 0.5f);

    /* tail hangs down behind the coat (local -Z) and sways */
    angles[RIG_CH_TAIL_A].x = -0.55f + 0.07f * sinf(t * 2.2f);
    angles[RIG_CH_TAIL_A].y = 0.22f * sinf(t * 1.4f);
    angles[RIG_CH_TAIL_B].x = 0.18f + 0.05f * sinf(t * 1.9f + 0.7f);
    angles[RIG_CH_TAIL_B].y = 0.26f * sinf(t * 1.9f + 0.7f);
}

/* Draws the hero rig and its third-person weapon.  Writes the world-space
 * muzzle position when `muzzle` is non-NULL. */
static void hero_draw(const Player *player, const C3D_Mtx *view, float t,
                      bool idle_only, WeaponId weapon, Vec3 *muzzle)
{
    const HeroRig *rig = s_hero_rig;
    Vec3 angles[RIG_CH_COUNT];
    C3D_Mtx world[HERO_PART_COUNT];
    C3D_Mtx root;
    float bob;
    Vec3 pos = player->transform.pos;
    float yaw = player->transform.yaw;
    float scale = rig->scale > 0.01f ? rig->scale : 1.0f;
    WeaponId w = clamp_weapon(weapon);
    int i;

    hero_pose_angles(player, t, idle_only, angles, &bob);

    Mtx_Identity(&root);
    Mtx_Translate(&root, pos.x, pos.y + bob, pos.z, true);
    Mtx_RotateY(&root, yaw, true);
    Mtx_Scale(&root, scale, scale, scale);

    for (i = 0; i < rig->part_count; i++) {
        const RigPart *part = &rig->part[i];
        C3D_Mtx local;
        Vec3 a = (part->channel < RIG_CH_COUNT) ? angles[part->channel] : v3(0.0f, 0.0f, 0.0f);

        Mtx_Identity(&local);
        Mtx_Translate(&local, part->pivot.x, part->pivot.y, part->pivot.z, true);
        Mtx_RotateX(&local, a.x, true);
        Mtx_RotateY(&local, a.y, true);
        Mtx_RotateZ(&local, a.z, true);

        if (part->parent >= 0 && part->parent < i) {
            Mtx_Multiply(&world[i], &world[part->parent], &local);
        } else {
            /* root part: its parent space is hero-local, so apply the hero root */
            Mtx_Multiply(&world[i], &root, &local);
        }
    }

    for (i = 0; i < rig->part_count; i++) {
        draw_model(view, &rig->part[i].mesh, &world[i], 1.0f, 1.0f, 1.0f, 0.0f);
    }

    /* third-person weapon: held at the right hip, barrel towards hero-forward.
     * Rig parts are in hero-local metres, so mount from the root instead of the
     * animated hand chain to keep the silhouette readable and the barrel level. */
    {
        C3D_Mtx local, gun_world;

        Mtx_Identity(&local);
        Mtx_Translate(&local, 0.33f, 0.99f, 0.06f, true);
        Mtx_RotateY(&local, 0.55f, true);
        Mtx_RotateX(&local, -0.03f, true);
        Mtx_Multiply(&gun_world, &root, &local);
        Mtx_Scale(&gun_world, 1.25f, 1.25f, 1.25f);
        draw_model(view, &s_weapons[w], &gun_world, 1.0f, 1.0f, 1.0f, 0.10f);

        if (muzzle != NULL) {
            C3D_FVec mp = Mtx_MultiplyFVecH(&gun_world,
                                            FVec3_New(0.0f, 0.028f, s_muzzle_z[w]));
            *muzzle = v3(mp.x, mp.y, mp.z);
        }
    }
}

static void draw_first_person_gun(const GraveGame *game, WeaponId weapon)
{
    const Player *p = &game->player;
    C3D_Mtx model;
    float t = game->zone_time;
    float sway_x = sinf(t * 1.9f) * 0.005f +
                   clampf(p->anim_speed, 0.0f, 1.0f) * sinf(p->anim_time * 2.0f) * 0.006f;
    float sway_y = cosf(t * 1.3f) * 0.004f +
                   clampf(p->anim_speed, 0.0f, 1.0f) * fabsf(cosf(p->anim_time)) * 0.006f;
    float recoil = clampf(p->recoil, 0.0f, 0.25f);
    float kick = clampf(p->muzzle_flash / R3D_MUZZLE_FLASH_TIME, 0.0f, 1.0f);

    Mtx_Identity(&model);
    Mtx_Translate(&model,
                  s_vm_x[weapon] + sway_x,
                  s_vm_y[weapon] + sway_y,
                  s_vm_z[weapon] + recoil * 0.55f + kick * 0.02f, true);
    Mtx_RotateY(&model, PI_F, true);
    Mtx_RotateX(&model, recoil * 1.8f + kick * 0.12f, true);
    Mtx_RotateZ(&model, sway_x * 0.6f, true);

    Mtx_Scale(&model, 1.25f, 1.25f, 1.25f);
    draw_mesh_mv(&model, &s_weapons[weapon], 1.0f, 1.0f, 1.0f, 0.18f);
}

/* ------------------------------------------------------------------ */
/* zombies                                                            */
/* ------------------------------------------------------------------ */

static float zombie_scale(ZombieType type)
{
    /* core heights: shambler 1.70 m, runner 1.60 m, brute 2.30 m; the baked
     * pose meshes are ~1.83 m tall, so these are height/1.83. */
    switch (type) {
    case ZOMBIE_RUNNER:  return 0.87f;
    case ZOMBIE_BRUTE:   return 1.26f;
    case ZOMBIE_FOREMAN: return R3D_FOREMAN_HEIGHT / 1.83f;
    default:             return 0.93f;
    }
}

static int zombie_pose_index(const Zombie *z, int index)
{
    /* core's zombie anim_time is already an animation phase that advances at
     * spec->anim_rate * (2.2 + speed*0.8) per second, so one multiplier is
     * enough to turn it into an 8-frame walk cycle. */
    switch (z->state) {
    case ZOMBIE_ATTACK:  return ZOMBIE_POSE_LUNGE;
    case ZOMBIE_STAGGER: return ZOMBIE_POSE_STAGGER;
    case ZOMBIE_DYING:   return ZOMBIE_POSE_DEATH;
    default:             break;
    }

    {
        float ph = z->anim_time * 1.7f + (float)index * 1.7f;
        int p = (int)floorf(ph) % ZOMBIE_WALK_FRAMES;
        if (p < 0)
            p += ZOMBIE_WALK_FRAMES;
        return p;
    }
}

static void draw_zombie(const Zombie *z, int index, const C3D_Mtx *view)
{
    ZombieType type = z->type;
    float scale = zombie_scale(type);
    C3D_Mtx model;
    Vec3 centre;
    const Mesh *mesh;
    float tint_r = 1.0f, tint_g = 1.0f, tint_b = 1.0f;

    if ((int)type < 0 || (int)type >= ZOMBIE_COUNT)
        type = ZOMBIE_SHAMBLER;
    if (z->state == ZOMBIE_DEAD)
        return;

    centre = v3_add(z->pos, v3(0.0f, 0.95f * scale, 0.0f));
    if (!world_visible(view, centre, 1.4f * scale))
        return;

    mesh = &s_zombie_poses[type][zombie_pose_index(z, index)];

    Mtx_Identity(&model);
    Mtx_Translate(&model, z->pos.x, z->pos.y, z->pos.z, true);
    Mtx_RotateY(&model, z->yaw, true);
    if (z->state == ZOMBIE_DYING) {
        float k = clampf(z->death_time, 0.0f, 1.0f);
        Mtx_Translate(&model, 0.0f, -0.10f * k, 0.0f, true);
        Mtx_RotateX(&model, 1.40f * k, true);
        tint_r = 1.0f - 0.45f * k;
        tint_g = 1.0f - 0.45f * k;
        tint_b = 1.0f - 0.40f * k;
    } else if (z->state == ZOMBIE_STAGGER) {
        tint_r = 1.55f;
        tint_g = 1.20f;
        tint_b = 1.10f;
    }
    if (type == ZOMBIE_FOREMAN) {
        /* charred, soot-blackened hide (eyes glow additively in zone_glow_build) */
        tint_r = 0.40f;
        tint_g = 0.35f;
        tint_b = 0.34f;
    }
    Mtx_Scale(&model, scale, scale, scale);

    draw_model(view, mesh, &model, tint_r, tint_g, tint_b, 0.0f);
}

static void draw_zombies(const GraveGame *game, const C3D_Mtx *view)
{
    int i;

    for (i = 0; i < MAX_ZOMBIES; i++) {
        const Zombie *z = &game->zombies[i];
        if (!z->active)
            continue;
        draw_zombie(z, i, view);
    }
}

/* ------------------------------------------------------------------ */
/* world props / pickups                                              */
/* ------------------------------------------------------------------ */

static const float s_prop_palette[PROP_KIND_COUNT][3] = {
    { 0.62f, 0.45f, 0.27f },   /* crate      */
    { 0.55f, 0.39f, 0.23f },   /* barrel     */
    { 0.60f, 0.50f, 0.36f },   /* fence      */
    { 0.56f, 0.56f, 0.55f },   /* tombstone  */
    { 0.42f, 0.33f, 0.24f },   /* deadtree   */
    { 0.52f, 0.51f, 0.48f },   /* well       */
    { 0.48f, 0.43f, 0.36f },   /* gate       */
    { 0.56f, 0.44f, 0.30f },   /* timber     */
    { 0.52f, 0.40f, 0.27f },   /* cart       */
    { 1.12f, 1.02f, 0.90f },   /* ore (bright, glows via additive halo) */
    { 1.15f, 0.95f, 0.68f },   /* lantern (warm, glows) */
};

static void draw_prop_one(const Prop *p, int index, const C3D_Mtx *view)
{
    C3D_Mtx model;
    Vec3 scale, centre;
    const float *pal;
    float shade, radius;
    int pal_index;

    if ((int)p->kind < 0 || (int)p->kind >= PROP_KIND_COUNT)
        return;

    scale = v3(2.0f * p->half.x, 2.0f * p->half.y, 2.0f * p->half.z);
    if (scale.x < 0.05f) scale.x = 0.05f;
    if (scale.y < 0.05f) scale.y = 0.05f;
    if (scale.z < 0.05f) scale.z = 0.05f;

    centre = v3_add(p->pos, v3(0.0f, p->half.y, 0.0f));
    radius = fmaxf(scale.x, fmaxf(scale.y, scale.z)) * 1.2f;
    if (!world_visible(view, centre, radius))
        return;

    pal_index = (int)p->color;
    if (pal_index < 0 || pal_index >= PROP_KIND_COUNT)
        pal_index = (int)p->kind;
    pal = s_prop_palette[pal_index];

    shade = 0.88f + 0.24f * hash01((unsigned)index * 2654435761u + 17u);

    model_transform(&model, p->pos, p->yaw, scale);
    draw_model(view, &s_props[p->kind], &model,
               pal[0] * shade, pal[1] * shade, pal[2] * shade, 0.0f);
}

static void draw_props(const GraveGame *game, const C3D_Mtx *view)
{
    int i, count = game->prop_count;

    if (count > MAX_PROPS)
        count = MAX_PROPS;
    for (i = 0; i < count; i++)
        draw_prop_one(&game->props[i], i, view);
}

static void draw_pickups(const GraveGame *game, const C3D_Mtx *view)
{
    int i;

    for (i = 0; i < MAX_PICKUPS; i++) {
        const Pickup *p = &game->pickups[i];
        C3D_Mtx model;
        Vec3 centre;
        float spin, bob;

        if (!p->active)
            continue;
        if ((int)p->kind <= DROP_NONE || (int)p->kind >= DROP_COUNT)
            continue;

        centre = v3_add(p->pos, v3(0.0f, 0.18f, 0.0f));
        if (!world_visible(view, centre, 0.35f))
            continue;

        spin = p->bob_time * 2.6f;
        bob = 0.05f * sinf(p->bob_time * 3.0f);

        Mtx_Identity(&model);
        Mtx_Translate(&model, p->pos.x, p->pos.y + bob, p->pos.z, true);
        Mtx_RotateY(&model, spin, true);

        draw_model(view, &s_pickups[p->kind], &model, 1.0f, 1.0f, 1.0f, 0.35f);
    }
}

/* Shadows for props + characters; drawn right after the ground. */
static void draw_shadows(const GraveGame *game, const C3D_Mtx *view, bool first_person)
{
    int i, count = game->prop_count;

    if (count > MAX_PROPS)
        count = MAX_PROPS;
    for (i = 0; i < count; i++) {
        const Prop *p = &game->props[i];
        if ((int)p->kind < 0 || (int)p->kind >= PROP_KIND_COUNT)
            continue;
        if (!world_visible(view, p->pos, fmaxf(p->half.x, p->half.z) * 2.0f))
            continue;
        draw_shadow(view, p->pos, p->half.x * 1.05f, p->half.z * 1.05f);
    }

    for (i = 0; i < MAX_ZOMBIES; i++) {
        const Zombie *z = &game->zombies[i];
        float s;
        if (!z->active || z->state == ZOMBIE_DEAD)
            continue;
        if (!world_visible(view, z->pos, 1.5f))
            continue;
        s = zombie_scale((ZombieType)z->type);
        draw_shadow(view, z->pos, 0.42f * s, 0.36f * s);
    }

    if (!first_person && game->player.alive) {
        draw_shadow(view, game->player.transform.pos, 0.36f, 0.30f);
    }
}

/* ------------------------------------------------------------------ */
/* effects                                                            */
/* ------------------------------------------------------------------ */

static void update_smoke(const GraveGame *game, float dt, Vec3 muzzle, bool muzzle_valid)
{
    float mf = game != NULL ? game->player.muzzle_flash : 0.0f;
    int i;

    s_last_muzzle = muzzle;
    s_have_last_muzzle = muzzle_valid;

    if (mf > 0.001f && s_prev_muzzle <= 0.001f) {
        for (i = 0; i < 2; i++) {
            SmokePuff *p = &s_puffs[s_puff_next];
            float r1 = hash01((unsigned)s_puff_next * 7919u + 3u);
            float r2 = hash01((unsigned)s_puff_next * 104729u + 11u);
            s_puff_next = (s_puff_next + 1) % R3D_MAX_PUFFS;
            p->active = true;
            p->age = 0.0f;
            p->life = 0.45f + 0.25f * r1;
            p->size = 0.045f + 0.02f * r2;
            p->pos = v3_add(muzzle, v3((r1 - 0.5f) * 0.05f,
                                       (r2 - 0.5f) * 0.05f,
                                       (r1 - 0.5f) * 0.05f));
            p->vel = v3((r1 - 0.5f) * 0.25f, 0.30f + 0.25f * r2, (r2 - 0.5f) * 0.25f);
        }
    }
    s_prev_muzzle = mf;

    for (i = 0; i < R3D_MAX_PUFFS; i++) {
        SmokePuff *p = &s_puffs[i];
        if (!p->active)
            continue;
        p->age += dt;
        if (p->age >= p->life) {
            p->active = false;
            continue;
        }
        p->pos = v3_add(p->pos, v3_scale(p->vel, dt));
        p->vel.y += 0.55f * dt;
        p->vel = v3_scale(p->vel, clampf(1.0f - 1.6f * dt, 0.0f, 1.0f));
    }
}

/* ------------------------------------------------------------------ */
/* v0.3 zone effects: lantern/ore glow, Foreman presence and stomps   */
/* ------------------------------------------------------------------ */

#define R3D_MAX_BURSTS     6

typedef struct {
    Vec3  pos;
    float age, life, radius, strength;
    bool  active;
} R3dGroundBurst;

static R3dGroundBurst s_bursts[R3D_MAX_BURSTS];
static int s_burst_next;
static bool s_boss_was_alive;
static float s_boss_prev_atk;

static float zombie_type_height(ZombieType type)
{
    switch (type) {
    case ZOMBIE_RUNNER:  return 1.60f;
    case ZOMBIE_BRUTE:   return 2.30f;
    case ZOMBIE_FOREMAN: return R3D_FOREMAN_HEIGHT;
    default:             return 1.70f;
    }
}

static const Zombie *find_boss(const GraveGame *game)
{
    int i;
    if (game == NULL)
        return NULL;
    for (i = 0; i < MAX_ZOMBIES; i++) {
        const Zombie *z = &game->zombies[i];
        if (z->active && z->type == ZOMBIE_FOREMAN && z->state != ZOMBIE_DEAD)
            return z;
    }
    return NULL;
}

static void burst_spawn(Vec3 pos, float radius, float life, float strength)
{
    R3dGroundBurst *b = &s_bursts[s_burst_next];
    s_burst_next = (s_burst_next + 1) % R3D_MAX_BURSTS;
    b->pos = pos;
    b->age = 0.0f;
    b->life = life;
    b->radius = radius;
    b->strength = strength;
    b->active = true;
}

static void dust_puff_spawn(Vec3 pos, float spread, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        SmokePuff *p = &s_puffs[s_puff_next];
        float r1 = hash01((unsigned)s_puff_next * 7919u + 3u);
        float r2 = hash01((unsigned)s_puff_next * 104729u + 11u);
        s_puff_next = (s_puff_next + 1) % R3D_MAX_PUFFS;
        p->active = true;
        p->age = 0.0f;
        p->life = 0.55f + 0.45f * r1;
        p->size = 0.10f + 0.08f * r2;
        p->pos = v3_add(pos, v3((r1 - 0.5f) * spread,
                                0.05f + 0.10f * r2,
                                (r2 - 0.5f) * spread));
        p->vel = v3((r1 - 0.5f) * 0.9f, 0.45f + 0.4f * r2,
                    (r2 - 0.5f) * 0.9f);
    }
}

/* Called once per frame (not per eye) so effects do not spawn twice in 3D. */
static void boss_effects_update(const GraveGame *game, float dt)
{
    const Zombie *boss = find_boss(game);
    bool alive = (boss != NULL) || (game != NULL && game->boss_alive);
    int i;

    if (alive && !s_boss_was_alive && boss != NULL) {
        /* Foreman rises: dust ring + a burst of smoke at his feet */
        burst_spawn(v3(boss->pos.x, 0.05f, boss->pos.z), 2.4f, 1.2f, 1.3f);
        dust_puff_spawn(v3(boss->pos.x, 0.20f, boss->pos.z), 1.4f, 6);
    }

    if (boss != NULL) {
        float atk = boss->attack_timer;
        if (atk > s_boss_prev_atk + 0.05f) {
            burst_spawn(v3(boss->pos.x, 0.05f, boss->pos.z), 1.7f, 0.8f, 1.1f);
            dust_puff_spawn(v3(boss->pos.x, 0.15f, boss->pos.z), 1.1f, 3);
        }
        s_boss_prev_atk = atk;
    } else {
        s_boss_prev_atk = 0.0f;
    }
    s_boss_was_alive = alive;

    for (i = 0; i < R3D_MAX_BURSTS; i++) {
        if (!s_bursts[i].active)
            continue;
        s_bursts[i].age += dt;
        if (s_bursts[i].age >= s_bursts[i].life)
            s_bursts[i].active = false;
    }
}

/* Appends additive glow quads for mine lights, ore and the Foreman's eyes.
 * Must be called after effects_build() reset the dynamic buffer. */
static void zone_glow_build(const GraveGame *game, const C3D_Mtx *view,
                            const Vec3 *right, const Vec3 *up)
{
    const R3dZonePalette *pal = r3d_palette();
    Vec3 ax = v3(1.0f, 0.0f, 0.0f);
    Vec3 az = v3(0.0f, 0.0f, 1.0f);
    const Zombie *boss;
    float t;
    int i;

    if (game == NULL)
        return;
    t = game->zone_time;

    if (pal->glow > 0.01f) {
        int count = game->prop_count;
        if (count > MAX_PROPS)
            count = MAX_PROPS;
        for (i = 0; i < count; i++) {
            const Prop *p = &game->props[i];
            float sx = 2.0f * p->half.x;
            float sy = 2.0f * p->half.y;
            float sz = 2.0f * p->half.z;
            float lx, ly, wx, wy, wz, size, flick;

            if (p->kind != PROP_LANTERN && p->kind != PROP_ORE)
                continue;
            if (!world_visible(view, v3(p->pos.x, p->pos.y + p->half.y, p->pos.z),
                                fmaxf(fmaxf(sx, sy), 1.2f)))
                continue;

            if (p->kind == PROP_LANTERN) {
                lx = 0.305f;
                ly = 0.725f;
            } else {
                lx = 0.0f;
                ly = 0.34f;
            }
            wx = p->pos.x + cosf(p->yaw) * lx * sx;
            wz = p->pos.z - sinf(p->yaw) * lx * sz;
            wy = p->pos.y + ly * sy;

            flick = 0.78f + 0.16f * sinf(t * 7.3f + (float)i * 1.7f) +
                    0.06f * hash01((unsigned)i * 2654435761u + 7u);
            flick *= pal->glow;

            if (p->kind == PROP_LANTERN) {
                size = 0.13f * fmaxf(fmaxf(sx, sy), 0.4f) + 0.05f;
                /* warm halo at the lantern head */
                dyn_quad(v3(wx, wy, wz), *right, *up, size, size,
                         0.95f * flick, 0.58f * flick, 0.22f * flick);
                dyn_quad(v3(wx, wy, wz), *right, *up, size * 0.45f, size * 0.45f,
                         1.10f * flick, 0.85f * flick, 0.45f * flick);
                /* light pool on the ground below */
                dyn_quad(v3(p->pos.x, 0.035f, p->pos.z), ax, az,
                         0.75f * sx, 0.75f * sz,
                         0.30f * flick, 0.18f * flick, 0.06f * flick);
            } else {
                size = 0.16f * fmaxf(sx, sy) + 0.05f;
                dyn_quad(v3(wx, wy + 0.15f * sy, wz), *right, *up, size, size,
                         0.55f * flick, 0.30f * flick, 0.08f * flick);
                dyn_quad(v3(wx, wy + 0.10f * sy, wz), *right, *up,
                         size * 0.5f, size * 0.5f,
                         0.30f * flick, 0.70f * flick, 0.62f * flick);
            }
        }

        /* expanding dust rings (boss spawn / stomps) */
        for (i = 0; i < R3D_MAX_BURSTS; i++) {
            const R3dGroundBurst *b = &s_bursts[i];
            float k, fade, rad;
            int seg;
            if (!b->active)
                continue;
            k = b->age / b->life;
            fade = (1.0f - k) * (1.0f - k) * b->strength;
            rad = b->radius * (0.15f + 0.85f * k);
            for (seg = 0; seg < 10; seg++) {
                float a = (float)seg * (6.2831853f / 10.0f) + b->age * 1.2f;
                Vec3 p = v3(b->pos.x + cosf(a) * rad, 0.045f,
                            b->pos.z + sinf(a) * rad);
                dyn_quad(p, ax, az, 0.20f * rad + 0.04f, 0.20f * rad + 0.04f,
                         0.42f * fade, 0.35f * fade, 0.26f * fade);
            }
        }
    }

    /* Foreman: red eye glow + charred halo */
    boss = find_boss(game);
    if (boss != NULL) {
        float height = zombie_type_height(boss->type);
        float scale = height / 1.70f;
        float pulse = 0.80f + 0.20f * sinf(t * 4.1f);
        Vec3 anchor;
        float anchor_fwd_x = sinf(boss->yaw);
        float anchor_fwd_z = cosf(boss->yaw);
        int e;

        if (s_boss_head_valid) {
            anchor = s_boss_head_pos;
        } else {
            anchor = v3(boss->pos.x + anchor_fwd_x * 0.16f * scale,
                        boss->pos.y + 0.92f * height,
                        boss->pos.z + anchor_fwd_z * 0.16f * scale);
        }
        for (e = -1; e <= 1; e += 2) {
            Vec3 eye = v3(anchor.x - anchor_fwd_z * 0.085f * scale * (float)e,
                          anchor.y + 0.02f * scale,
                          anchor.z + anchor_fwd_x * 0.085f * scale * (float)e);
            dyn_quad(eye, *right, *up, 0.045f * scale, 0.032f * scale,
                     1.10f * pulse, 0.10f, 0.06f);
            dyn_quad(eye, *right, *up, 0.10f * scale, 0.075f * scale,
                     0.35f * pulse, 0.03f, 0.02f);
        }
        dyn_quad(v3(boss->pos.x, boss->pos.y + 0.35f, boss->pos.z),
                 ax, az, 0.9f * scale, 0.9f * scale,
                 0.14f * pulse, 0.04f, 0.02f);
    }
}
/* slow drifting dust motes: cheap atmosphere, 16 additive quads */
static void dust_update(float dt, Vec3 cam)
{
    int i;

    if (!s_dust_ready) {
        for (i = 0; i < R3D_DUST_COUNT; i++) {
            float a = hash01((unsigned)i * 7u + 1u) * 2.0f * PI_F;
            float rr = 1.5f + hash01((unsigned)i * 13u + 3u) * 7.5f;
            s_dust[i] = v3(cam.x + cosf(a) * rr,
                           0.15f + hash01((unsigned)i * 17u + 5u) * 2.9f,
                           cam.z + sinf(a) * rr);
        }
        s_dust_ready = true;
    }

    for (i = 0; i < R3D_DUST_COUNT; i++) {
        Vec3 p = s_dust[i];
        float spin = s_elapsed * 0.55f + (float)i * 1.37f;

        p.y += dt * (0.10f + 0.10f * hash01((unsigned)i * 23u + 9u));
        p.x += dt * 0.12f * sinf(spin);
        p.z += dt * 0.10f * cosf(spin * 0.83f);
        if (p.y > 3.8f)
            p.y = 0.12f;
        if (fabsf(p.x - cam.x) > 11.0f)
            p.x = cam.x + (hash01((unsigned)i * 31u + 11u) * 2.0f - 1.0f) * 11.0f;
        if (fabsf(p.z - cam.z) > 11.0f)
            p.z = cam.z + (hash01((unsigned)i * 37u + 7u) * 2.0f - 1.0f) * 11.0f;
        s_dust[i] = p;
    }
}

static void dust_build(const Vec3 *right, const Vec3 *up)
{
    int i;

    for (i = 0; i < R3D_DUST_COUNT; i++) {
        float size = 0.014f + 0.024f * hash01((unsigned)i * 41u + 2u);
        float b = 0.16f + 0.22f * (0.5f + 0.5f * sinf(s_elapsed * 1.7f + (float)i));
        dyn_quad(s_dust[i], *right, *up, size, size,
                 0.58f * b, 0.50f * b, 0.38f * b);
    }
}
static void effects_build(const GraveGame *game, const Vec3 *cam_pos,
                          const Vec3 *fwd, const Vec3 *right, const Vec3 *up,
                          Vec3 muzzle, bool muzzle_visible, float muzzle_flash,
                          float fx_scale)
{
    int i;

    dyn_reset();

    /* shot tracers / impact sparks */
    for (i = 0; i < MAX_SHOT_TRACES; i++) {
        const ShotTrace *tr = &game->shot_traces[i];
        Vec3 mid;
        float f;

        if (tr->life <= 0.0f)
            continue;

        if (tr->kind == 0) {
            Vec3 dir = v3_sub(tr->end, tr->start);
            float len = sqrtf(v3_dot(dir, dir));
            if (len < 0.05f)
                continue;
            mid = v3_scale(v3_add(tr->start, tr->end), 0.5f);
            f = clampf(tr->life / 0.06f, 0.0f, 1.0f);
            {
                Vec3 side = v3_cross(v3_norm(dir), v3_norm(v3_sub(mid, *cam_pos)));
                if (sqrtf(v3_dot(side, side)) < 1e-4f)
                    side = *right;
                side = v3_norm(side);
                dyn_ribbon(tr->start, tr->end, side,
                           0.016f + 0.020f * f,
                           1.00f * f, 0.86f * f, 0.46f * f);
            }
        } else {
            unsigned h = (unsigned)i * 2654435761u + 101u;
            int k;
            f = clampf(tr->life / 0.22f, 0.0f, 1.0f);
            for (k = 0; k < 3; k++) {
                float rx = hash01(h + (unsigned)k * 3u) - 0.5f;
                float ry = hash01(h + (unsigned)k * 7u + 1u) - 0.5f;
                float rz = hash01(h + (unsigned)k * 11u + 2u) - 0.5f;
                Vec3 centre = v3_add(tr->end, v3(rx * 0.12f, ry * 0.12f, rz * 0.12f));
                float size = (0.030f + 0.035f * hash01(h + (unsigned)k)) * f;
                dyn_quad(centre, *right, *up, size, size,
                         1.0f * f, 0.68f * f, 0.25f * f);
            }
        }
    }

    /* muzzle smoke */
    for (i = 0; i < R3D_MAX_PUFFS; i++) {
        const SmokePuff *p = &s_puffs[i];
        float k, size, fade;
        if (!p->active)
            continue;
        k = p->age / p->life;
        size = (p->size + 0.20f * k) * fx_scale;
        fade = (1.0f - k) * (1.0f - k) * 0.30f;
        dyn_quad(p->pos, *right, *up, size, size,
                 fade * 0.85f, fade * 0.82f, fade * 0.75f);
    }

    /* drifting dust */
    dust_build(right, up);

    /* muzzle flash: two crossed quads at the muzzle */
    if (muzzle_visible && muzzle_flash > 0.001f) {
        float f = clampf(muzzle_flash, 0.0f, 1.0f);
        float s1 = (0.055f + 0.13f * f) * fx_scale;
        Vec3 r2 = v3_norm(v3_add(v3_scale(*right, 0.7071f), v3_scale(*up, 0.7071f)));
        Vec3 u2 = v3_norm(v3_sub(v3_scale(*up, 0.7071f), v3_scale(*right, 0.7071f)));

        dyn_quad(muzzle, *right, *up, s1, s1, 1.0f * f, 0.80f * f, 0.38f * f);
        dyn_quad(muzzle, r2, u2, s1 * 0.72f, s1 * 0.72f,
                 1.0f * f, 0.92f * f, 0.62f * f);
        dyn_quad(muzzle, *right, *up, s1 * 0.28f, s1 * 0.28f,
                 1.0f * f, 1.0f * f, 0.95f * f);
    }
}

/* blend/depth state for additive effects */
static void begin_additive(void)
{
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
}

static void end_additive(void)
{
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_BACK_CCW);
}

/* ------------------------------------------------------------------ */
/* crosshair (first person)                                           */
/* ------------------------------------------------------------------ */

static void draw_crosshair(void)
{
    /* pixel space: logical top screen is 400 x 240 */
    const float cx = 200.0f, cy = 120.0f;
    const float gap = 5.0f, len = 9.0f, thick = 1.7f;
    Vec3 r = v3(1.0f, 0.0f, 0.0f);
    Vec3 u = v3(0.0f, 1.0f, 0.0f);
    C3D_Mtx identity;

    dyn_reset();
    dyn_quad(v3(cx - gap - len * 0.5f, cy, 0.5f), r, u, len * 0.5f, thick * 0.5f,
             0.94f, 0.90f, 0.78f);
    dyn_quad(v3(cx + gap + len * 0.5f, cy, 0.5f), r, u, len * 0.5f, thick * 0.5f,
             0.94f, 0.90f, 0.78f);
    dyn_quad(v3(cx, cy - gap - len * 0.5f, 0.5f), r, u, thick * 0.5f, len * 0.5f,
             0.94f, 0.90f, 0.78f);
    dyn_quad(v3(cx, cy + gap + len * 0.5f, 0.5f), r, u, thick * 0.5f, len * 0.5f,
             0.94f, 0.90f, 0.78f);
    dyn_quad(v3(cx, cy, 0.5f), r, u, 1.4f, 1.4f, 0.95f, 0.55f, 0.35f);

    Mtx_Identity(&identity);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_u_projection, &s_ortho);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    dyn_flush(&identity, 1.0f, 1.0f, 1.0f, 1.0f);
}

/* ------------------------------------------------------------------ */
/* imported models: GPM v1 loading + CPU skinning                    */
/* ------------------------------------------------------------------ */

/* Small trace file so model swaps on SD can be debugged from a PC.  Written
 * only during init / explicit reload, never per frame. */
static void r3d_asset_log(const char *fmt, ...)
{
    FILE *f = fopen("sdmc:/gp_assets.txt", "a");
    va_list ap;
    if (f == NULL)
        return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* Converts a row-major float[16] (x,y,z,w per row) into a C3D_Mtx.  C3D_FVec
 * uses the PICA layout (w,z,y,x per row), so a plain memcpy would scramble the
 * matrix - every column/translation would land in the wrong place. */
static void mtx_from_floats(C3D_Mtx *out, const float *f)
{
    int r;
    for (r = 0; r < 4; r++) {
        out->r[r] = FVec4_New(f[r * 4 + 0], f[r * 4 + 1],
                              f[r * 4 + 2], f[r * 4 + 3]);
    }
}

static Vec3 mtx_translation(const float *f)
{
    return v3(f[3], f[7], f[11]);
}

static float deg2rad(float d)
{
    return d * (PI_F / 180.0f);
}

/* Builds T(rest + offset) * R(euler) * S(scale) into `local`. */
static void attach_local(C3D_Mtx *local, Vec3 rest, Vec3 offset,
                         Vec3 euler_deg, float scale)
{
    Mtx_Identity(local);
    Mtx_Translate(local, rest.x + offset.x, rest.y + offset.y, rest.z + offset.z, true);
    Mtx_RotateX(local, deg2rad(euler_deg.x), true);
    Mtx_RotateY(local, deg2rad(euler_deg.y), true);
    Mtx_RotateZ(local, deg2rad(euler_deg.z), true);
    Mtx_Scale(local, scale, scale, scale);
}

/* Case-insensitive substring bone lookup with exact-name fast path. */
static int r3d_find_bone_flexible(const GpmModel *m, const char *exact,
                                  const char *substr)
{
    uint32_t i;
    int idx;

    if (m == NULL || m->bones == NULL)
        return -1;
    idx = gpm_find_bone(m, exact);
    if (idx >= 0)
        return idx;
    if (substr == NULL)
        return -1;
    for (i = 0; i < m->bone_count; i++) {
        const char *name = gpm_name(m, m->bones[i].name_off);
        const char *a, *b;
        if (name == NULL)
            continue;
        /* substring search, ASCII case-insensitive */
        for (a = name; *a != '\0'; a++) {
            const char *p = a;
            b = substr;
            while (*p != '\0' && *b != '\0') {
                char ca = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : *p;
                char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
                if (ca != cb)
                    break;
                p++;
                b++;
            }
            if (*b == '\0')
                return (int)i;
        }
    }
    return -1;
}


typedef struct {
    const char *name;
    const void *data;
    size_t      size;
} R3dEmbeddedModel;

static bool r3d_load_one_model(GpmModel *model, bool *ok, const char *name,
                               const R3dEmbeddedModel *embedded, bool need_skinned,
                               R3dModelSrcKind *src_out)
{
    char path[160];
    const char *err;

    *ok = false;
    if (src_out != NULL)
        *src_out = R3D_SRC_PROCEDURAL;
    memset(model, 0, sizeof(*model));

    snprintf(path, sizeof(path), R3D_MODEL_DIR "%s.gpm", name);
    err = gpm_load_file(model, path);
    if (err == NULL) {
        if (src_out != NULL)
            *src_out = R3D_SRC_SD;
        r3d_asset_log("%-14s SD        %s verts=%u idx=%u bones=%u anims=%u",
                      name, path, model->vertex_count, model->index_count,
                      model->bone_count, model->anim_count);
    } else {
        r3d_asset_log("%-14s SD miss   %s (%s)", name, path, err);
        if (embedded != NULL && embedded->data != NULL && embedded->size > 0) {
            err = gpm_parse_memory(model, embedded->data, embedded->size);
            if (err == NULL) {
                if (src_out != NULL)
                    *src_out = R3D_SRC_EMBEDDED;
                r3d_asset_log("%-14s embedded  verts=%u idx=%u bones=%u anims=%u",
                              name, model->vertex_count, model->index_count,
                              model->bone_count, model->anim_count);
            } else {
                r3d_asset_log("%-14s embedded BAD (%s)", name, err);
            }
        } else {
            r3d_asset_log("%-14s no embedded copy", name);
        }
    }

    if (model->data == NULL)
        return false;
    if (need_skinned && !gpm_is_skinned(model)) {
        r3d_asset_log("%-14s rejected: not skinned", name);
        gpm_destroy(model);
        if (src_out != NULL)
            *src_out = R3D_SRC_PROCEDURAL;
        return false;
    }
    *ok = true;
    return true;
}

/* Fills the per-slot provenance strings printed by the wardrobe page. */
static void r3d_update_model_sources(void)
{
    static const char *const gun_names[WEAPON_COUNT] = {
        "gun_flintlock.gpm", "gun_revolver.gpm", "gun_musket.gpm"
    };
    int w;
    bool all_sd = true, all_embedded = true, any_proc = false;
    const char *src = "not loaded";

    switch (s_src_hero) {
    case R3D_SRC_SD:         src = "SD: hero.gpm"; break;
    case R3D_SRC_EMBEDDED:   src = "embedded"; break;
    case R3D_SRC_PROCEDURAL: src = "procedural fallback"; break;
    case R3D_SRC_CUSTOM:     src = "custom model"; break;
    default:                 break;
    }
    snprintf(s_model_source_text[R3D_MODEL_HERO], 64, "%s", src);

    switch (s_src_zombie) {
    case R3D_SRC_SD:         src = "SD: zombie.gpm"; break;
    case R3D_SRC_EMBEDDED:   src = "embedded"; break;
    case R3D_SRC_PROCEDURAL: src = "procedural fallback"; break;
    default:                 src = "not loaded"; break;
    }
    snprintf(s_model_source_text[R3D_MODEL_ZOMBIE], 64, "%s", src);

    for (w = 0; w < WEAPON_COUNT; w++) {
        if (s_src_gun[w] != R3D_SRC_SD)
            all_sd = false;
        if (s_src_gun[w] != R3D_SRC_EMBEDDED)
            all_embedded = false;
        if (s_src_gun[w] == R3D_SRC_PROCEDURAL)
            any_proc = true;
    }
    if (all_sd)
        src = "SD: gun_*.gpm";
    else if (all_embedded)
        src = "embedded";
    else if (any_proc)
        src = "procedural fallback";
    else
        src = "mixed SD/embedded";
    snprintf(s_model_source_text[R3D_MODEL_GUNS], 64, "%s", src);
    (void)gun_names;
}

/* Resolves standard clip roles into model clip indices (with fallbacks). */
static int clip_chain(const GpmModel *m, const char *const *roles, int count)
{
    if (m == NULL || m->data == NULL)
        return -1;
    return gpm_find_anim_chain(m, roles, count);
}

static void r3d_resolve_hero_clips(const GpmModel *m)
{
    static const char *const idle[]  = { "idle" };
    static const char *const walk[]  = { "walk", "idle" };
    static const char *const run[]   = { "run", "walk", "idle" };
    static const char *const shoot[] = { "shoot", "attack", "reload", "idle" };
    static const char *const hit[]   = { "hit", "idle" };
    static const char *const death[] = { "death", "hit", "idle" };

    s_hero_clip_idle = clip_chain(m, idle, 1);
    s_hero_clip_walk = clip_chain(m, walk, 2);
    s_hero_clip_run = clip_chain(m, run, 3);
    s_hero_clip_shoot = clip_chain(m, shoot, 4);
    s_hero_clip_hit = clip_chain(m, hit, 2);
    s_hero_clip_death = clip_chain(m, death, 3);
}

static void r3d_resolve_zombie_clips(void)
{
    static const char *const idle[]   = { "idle", "walk" };
    static const char *const walk[]   = { "walk", "idle" };
    static const char *const run[]    = { "run", "walk", "idle" };
    static const char *const attack[] = { "attack", "shoot", "walk", "idle" };
    static const char *const hit[]    = { "hit", "idle" };
    static const char *const death[]  = { "death", "hit", "idle" };

    s_zombie_clip_idle = clip_chain(&s_gpm_zombie, idle, 2);
    s_zombie_clip_walk = clip_chain(&s_gpm_zombie, walk, 2);
    s_zombie_clip_run = clip_chain(&s_gpm_zombie, run, 3);
    s_zombie_clip_attack = clip_chain(&s_gpm_zombie, attack, 4);
    s_zombie_clip_hit = clip_chain(&s_gpm_zombie, hit, 2);
    s_zombie_clip_death = clip_chain(&s_gpm_zombie, death, 3);
    s_zombie_bone_head = r3d_find_bone_flexible(&s_gpm_zombie, "Head", "head");
}

/* ------------------------------------------------------------------ */
/* dynamic skinning buffers (linearAlloc, GPU visible)                 */
/* ------------------------------------------------------------------ */

/* (Re)allocates the hero dynamic VBO + static IBO for a model.  Old buffers
 * are released first, so this doubles as the hot-swap path. */
static bool r3d_hero_buffers_alloc(const GpmModel *m)
{
    int verts;

    if (s_hero_vbo != NULL) {
        linearFree(s_hero_vbo);
        s_hero_vbo = NULL;
    }
    if (s_hero_ibo != NULL) {
        linearFree(s_hero_ibo);
        s_hero_ibo = NULL;
    }
    s_hero_vert_cap = 0;
    s_hero_index_count = 0;
    if (m == NULL || m->data == NULL || m->vertex_count == 0 ||
        m->index_count == 0 || m->vertex_count > R3D_MAX_MODEL_VERTS)
        return false;
    verts = (int)m->vertex_count;
    s_hero_vbo = (MeshVertex *)linearAlloc((size_t)verts * sizeof(MeshVertex));
    s_hero_ibo = (uint16_t *)linearAlloc((size_t)m->index_count * sizeof(uint16_t));
    if (s_hero_vbo == NULL || s_hero_ibo == NULL) {
        r3d_asset_log("hero: skin buffer alloc failed (verts=%d)", verts);
        if (s_hero_vbo != NULL) {
            linearFree(s_hero_vbo);
            s_hero_vbo = NULL;
        }
        if (s_hero_ibo != NULL) {
            linearFree(s_hero_ibo);
            s_hero_ibo = NULL;
        }
        s_hero_vert_cap = 0;
        s_hero_index_count = 0;
        return false;
    }
    memcpy(s_hero_ibo, m->indices, (size_t)m->index_count * sizeof(uint16_t));
    s_hero_vert_cap = verts;
    s_hero_index_count = (int)m->index_count;
    return true;
}

/* Fills all hero runtime state (bones, rest pivots, scale, clips, skinning
 * instance) from a model.  Used by init, reload and r3d_set_hero_model. */
static void r3d_hero_configure(const GpmModel *m)
{
    float height = m->bbox_max.y - m->bbox_min.y;

    s_hero_bbox_min_y = m->bbox_min.y;
    s_hero_bone_head = r3d_find_bone_flexible(m, "Head", "head");
    s_hero_bone_hips = r3d_find_bone_flexible(m, "Hips", "hip");
    s_hero_bone_wrist = r3d_find_bone_flexible(m, "Wrist.R", "wrist");
    if (s_hero_bone_wrist < 0)
        s_hero_bone_wrist = r3d_find_bone_flexible(m, "Hand.R", "hand");
    s_hero_head_rest = (s_hero_bone_head >= 0)
        ? mtx_translation(&m->rest_world[s_hero_bone_head * 16]) : v3(0, 1.5f, 0);
    /* The imported hero has no head mesh: the procedural wolf head rides the
     * Head bone.  Scale the body so that (body head bone + wolf hat top) comes
     * out at R3D_HERO_HEIGHT, which also survives re-exports that change the
     * source scaling. */
    if (s_hero_bone_head >= 0 && s_hero_head_rest.y > 0.6f) {
        float above = R3D_HERO_HEIGHT - R3D_HERO_HEAD_TOP;
        s_hero_model_scale = above / s_hero_head_rest.y;
    } else {
        s_hero_model_scale = (height > 0.1f) ? (R3D_HERO_HEIGHT / height) : 1.0f;
    }
    if (s_hero_model_scale < 0.4f) s_hero_model_scale = 0.4f;
    if (s_hero_model_scale > 2.5f) s_hero_model_scale = 2.5f;
    s_hero_hips_rest = (s_hero_bone_hips >= 0)
        ? mtx_translation(&m->rest_world[s_hero_bone_hips * 16]) : v3(0, 0.95f, 0);
    s_hero_wrist_rest = (s_hero_bone_wrist >= 0)
        ? mtx_translation(&m->rest_world[s_hero_bone_wrist * 16]) : v3(0.2f, 1.0f, 0);
    r3d_resolve_hero_clips(m);
    skin_init(&s_hero_skin, m);
    s_hero_was_firing = false;
    s_hero_was_alive = true;
    s_hero_shoot_clock = 0.0f;
    s_hero_death_clock = 0.0f;
    r3d_asset_log("hero:  height=%.2f scale=%.3f bones head=%d hips=%d wrist=%d",
                  height, s_hero_model_scale, s_hero_bone_head,
                  s_hero_bone_hips, s_hero_bone_wrist);
}

static R3dEmbeddedModel r3d_hero_embedded_model(void)
{
    R3dEmbeddedModel emb;
    emb.name = "hero";
#if R3D_HAVE_EMBEDDED_HERO
    emb.data = hero_gpm;
    emb.size = (size_t)hero_gpm_size;
#else
    emb.data = NULL;
    emb.size = 0;
#endif
    return emb;
}

static bool r3d_alloc_skin_zombie(void)
{
    int i;
    int verts;

    if (!s_gpm_zombie_ok)
        return false;
    verts = (int)s_gpm_zombie.vertex_count;
    if (verts <= 0 || verts > R3D_MAX_MODEL_VERTS)
        return false;
    s_zombie_ibo = (uint16_t *)linearAlloc((size_t)s_gpm_zombie.index_count * sizeof(uint16_t));
    s_zombie_far_vbo = (MeshVertex *)linearAlloc((size_t)verts * sizeof(MeshVertex));
    if (s_zombie_ibo == NULL || s_zombie_far_vbo == NULL) {
        r3d_asset_log("zombie: skin buffer alloc failed (verts=%d)", verts);
        return false;
    }
    for (i = 0; i < R3D_NEAR_ZOMBIES; i++) {
        s_zombie_vbo[i] = (MeshVertex *)linearAlloc((size_t)verts * sizeof(MeshVertex));
        if (s_zombie_vbo[i] == NULL) {
            r3d_asset_log("zombie: near slot alloc failed (slot=%d)", i);
            return false;
        }
    }
    memcpy(s_zombie_ibo, s_gpm_zombie.indices,
           (size_t)s_gpm_zombie.index_count * sizeof(uint16_t));
    s_zombie_vert_cap = verts;
    s_zombie_index_count = (int)s_gpm_zombie.index_count;
    s_zombie_vert_count = verts;
    return true;
}

/* Loads every optional model, then prepares the skinning runtime.  Called
 * once from r3d_init (and again from the reload API). */
static void r3d_assets_init(void)
{
    R3dEmbeddedModel zombie_emb;
    R3dEmbeddedModel gun_emb[WEAPON_COUNT];
    const char *gun_names[WEAPON_COUNT] = { "gun_flintlock", "gun_revolver", "gun_musket" };
    /* barrel length used to auto-scale the imported gun to fit the hand */
    static const float gun_len[WEAPON_COUNT] = { 0.42f, 0.34f, 1.05f };
    int w;

    {
        FILE *f = fopen("sdmc:/gp_assets.txt", "w");
        if (f != NULL) {
            fprintf(f, "Gravepowder model loader\n");
            fclose(f);
        }
    }


#if R3D_HAVE_EMBEDDED_ZOMBIE
    zombie_emb.name = "zombie";
    zombie_emb.data = zombie_gpm;
    zombie_emb.size = (size_t)zombie_gpm_size;
#else
    zombie_emb.name = "zombie";
    zombie_emb.data = NULL;
    zombie_emb.size = 0;
#endif
#if R3D_HAVE_EMBEDDED_GUN_FLINTLOCK
    gun_emb[WEAPON_FLINTLOCK].name = "gun_flintlock";
    gun_emb[WEAPON_FLINTLOCK].data = gun_flintlock_gpm;
    gun_emb[WEAPON_FLINTLOCK].size = (size_t)gun_flintlock_gpm_size;
#else
    gun_emb[WEAPON_FLINTLOCK].name = "gun_flintlock";
    gun_emb[WEAPON_FLINTLOCK].data = NULL;
    gun_emb[WEAPON_FLINTLOCK].size = 0;
#endif
#if R3D_HAVE_EMBEDDED_GUN_REVOLVER
    gun_emb[WEAPON_REVOLVER].name = "gun_revolver";
    gun_emb[WEAPON_REVOLVER].data = gun_revolver_gpm;
    gun_emb[WEAPON_REVOLVER].size = (size_t)gun_revolver_gpm_size;
#else
    gun_emb[WEAPON_REVOLVER].name = "gun_revolver";
    gun_emb[WEAPON_REVOLVER].data = NULL;
    gun_emb[WEAPON_REVOLVER].size = 0;
#endif
#if R3D_HAVE_EMBEDDED_GUN_MUSKET
    gun_emb[WEAPON_MUSKET].name = "gun_musket";
    gun_emb[WEAPON_MUSKET].data = gun_musket_gpm;
    gun_emb[WEAPON_MUSKET].size = (size_t)gun_musket_gpm_size;
#else
    gun_emb[WEAPON_MUSKET].name = "gun_musket";
    gun_emb[WEAPON_MUSKET].data = NULL;
    gun_emb[WEAPON_MUSKET].size = 0;
#endif

    /* hero */
    {
        R3dEmbeddedModel emb = r3d_hero_embedded_model();
        if (r3d_load_one_model(&s_gpm_hero, &s_gpm_hero_ok, "hero", &emb, true,
                               &s_src_hero))
            r3d_hero_configure(&s_gpm_hero);
    }

    /* zombie */
    if (r3d_load_one_model(&s_gpm_zombie, &s_gpm_zombie_ok, "zombie",
                           &zombie_emb, true, &s_src_zombie)) {
        float height = s_gpm_zombie.bbox_max.y - s_gpm_zombie.bbox_min.y;
        s_zombie_model_scale = (height > 0.1f) ? (1.70f / height) : 0.93f;
        s_zombie_bbox_min_y = s_gpm_zombie.bbox_min.y;
        r3d_resolve_zombie_clips();
        r3d_asset_log("zombie: height=%.2f scale=%.3f clips idle=%d walk=%d run=%d attack=%d hit=%d death=%d",
                      height, s_zombie_model_scale, s_zombie_clip_idle,
                      s_zombie_clip_walk, s_zombie_clip_run, s_zombie_clip_attack,
                      s_zombie_clip_hit, s_zombie_clip_death);
    }

    /* guns (static).  The importer already scales them to real-world length
     * (`scale` in assets/import/gun_*.json) and points the barrel along +Z, so
     * the renderer only places/orients them. */
    (void)gun_len;
    for (w = 0; w < WEAPON_COUNT; w++) {
        GpmModel *gm = &s_gpm_gun[w];
        if (r3d_load_one_model(gm, &s_gpm_gun_ok[w], gun_names[w],
                               &gun_emb[w], false, &s_src_gun[w])) {
            s_gpm_gun_scale[w] = 1.0f;
            s_gpm_gun_muzzle_local[w] = v3(
                (gm->bbox_min.x + gm->bbox_max.x) * 0.5f,
                (gm->bbox_min.y + gm->bbox_max.y) * 0.5f,
                gm->bbox_max.z);
            r3d_asset_log("gun[%d]:   bbox z=%.3f muzzle_z=%.3f", w,
                          gm->bbox_max.z - gm->bbox_min.z,
                          s_gpm_gun_muzzle_local[w].z);
        }
    }
    r3d_update_model_sources();
}

/* Frees model storage (used by r3d_fini and the reload API). */
static void r3d_assets_free(void)
{
    int w;
    gpm_destroy(&s_gpm_hero);
    gpm_destroy(&s_gpm_zombie);
    for (w = 0; w < WEAPON_COUNT; w++)
        gpm_destroy(&s_gpm_gun[w]);
    s_gpm_hero_ok = false;
    s_gpm_zombie_ok = false;
    for (w = 0; w < WEAPON_COUNT; w++)
        s_gpm_gun_ok[w] = false;
}

/* ------------------------------------------------------------------ */
/* imported static gun meshes (indexed, linearAlloc, reloadable)      */
/* ------------------------------------------------------------------ */

static void r3d_gun_mesh_free(int w)
{
    R3dGunMesh *gm = &s_gpm_gun_mesh[w];

    if (gm->verts != NULL)
        linearFree(gm->verts);
    if (gm->ibo != NULL)
        linearFree(gm->ibo);
    memset(gm, 0, sizeof(*gm));
}

static void r3d_gun_meshes_free(void)
{
    int w;
    for (w = 0; w < WEAPON_COUNT; w++)
        r3d_gun_mesh_free(w);
}

/* Uploads the GPM gun vertices + indices into linearAlloc buffers. */
static bool r3d_gun_mesh_build(int w)
{
    const GpmModel *m = &s_gpm_gun[w];
    R3dGunMesh *gm = &s_gpm_gun_mesh[w];
    uint32_t i;

    r3d_gun_mesh_free(w);
    if (!s_gpm_gun_ok[w] || m->data == NULL || m->vertex_count == 0 ||
        m->index_count == 0)
        return false;

    gm->verts = (MeshVertex *)linearAlloc((size_t)m->vertex_count * sizeof(MeshVertex));
    gm->ibo = (uint16_t *)linearAlloc((size_t)m->index_count * sizeof(uint16_t));
    if (gm->verts == NULL || gm->ibo == NULL) {
        r3d_asset_log("gun[%d]: indexed mesh alloc failed (verts=%u)", w,
                      m->vertex_count);
        r3d_gun_mesh_free(w);
        return false;
    }
    for (i = 0; i < m->vertex_count; i++) {
        const GpmVertex *v = &m->vertices[i];
        MeshVertex *o = &gm->verts[i];
        o->pos[0] = v->pos[0];
        o->pos[1] = v->pos[1];
        o->pos[2] = v->pos[2];
        o->nrm[0] = v->nrm[0];
        o->nrm[1] = v->nrm[1];
        o->nrm[2] = v->nrm[2];
        o->col[0] = (float)v->col[0] * (1.0f / 255.0f);
        o->col[1] = (float)v->col[1] * (1.0f / 255.0f);
        o->col[2] = (float)v->col[2] * (1.0f / 255.0f);
    }
    memcpy(gm->ibo, m->indices, (size_t)m->index_count * sizeof(uint16_t));
    gm->vert_count = (int)m->vertex_count;
    gm->index_count = (int)m->index_count;
    return true;
}

static void r3d_gun_meshes_build(void)
{
    int w;
    for (w = 0; w < WEAPON_COUNT; w++)
        (void)r3d_gun_mesh_build(w);
}

/* Draws one imported gun with the current model-view matrix. */
static void draw_gpm_gun(const C3D_Mtx *model_view, WeaponId weapon, float unlit)
{
    R3dGunMesh *gm;

    if ((int)weapon < 0 || (int)weapon >= WEAPON_COUNT)
        return;
    gm = &s_gpm_gun_mesh[weapon];
    if (gm->verts == NULL || gm->ibo == NULL)
        return;

    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_u_model_view, model_view);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u_tint, 1.0f, 1.0f, 1.0f, unlit);
    {
        C3D_BufInfo *buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, gm->verts, MESH_VERTEX_STRIDE, MESH_ATTR_COUNT,
                    MESH_ATTR_PERMUTATION);
    }
    C3D_DrawElements(GPU_TRIANGLES, gm->index_count, C3D_UNSIGNED_SHORT, gm->ibo);
    s_draw_calls++;
    s_draw_verts += (unsigned)gm->vert_count;
}


/* ------------------------------------------------------------------ */
/* imported model runtime lifecycle                                   */
/* (shared by r3d_init, hero hot-swap and r3d_reload_all_models)      */
/* ------------------------------------------------------------------ */

static void r3d_skin_buffers_free(void)
{
    int i;

    if (s_hero_vbo != NULL)
        linearFree(s_hero_vbo);
    s_hero_vbo = NULL;
    if (s_hero_ibo != NULL)
        linearFree(s_hero_ibo);
    s_hero_ibo = NULL;
    s_hero_vert_cap = 0;
    s_hero_index_count = 0;

    for (i = 0; i < R3D_NEAR_ZOMBIES; i++) {
        if (s_zombie_vbo[i] != NULL)
            linearFree(s_zombie_vbo[i]);
        s_zombie_vbo[i] = NULL;
        s_zombie_slot_inited[i] = false;
    }
    if (s_zombie_far_vbo != NULL)
        linearFree(s_zombie_far_vbo);
    s_zombie_far_vbo = NULL;
    s_zombie_far_inited = false;
    if (s_zombie_ibo != NULL)
        linearFree(s_zombie_ibo);
    s_zombie_ibo = NULL;
    s_zombie_index_count = 0;
    s_zombie_vert_count = 0;
    s_zombie_vert_cap = 0;
}

static void r3d_dynamic_bytes_update(void)
{
    int i;

    s_dynamic_bytes = 0;
    if (s_hero_vbo != NULL)
        s_dynamic_bytes += (unsigned)s_hero_vert_cap * (unsigned)sizeof(MeshVertex);
    if (s_hero_ibo != NULL)
        s_dynamic_bytes += (unsigned)s_hero_index_count * 2u;
    if (s_zombie_ibo != NULL)
        s_dynamic_bytes += (unsigned)s_zombie_index_count * 2u;
    for (i = 0; i < R3D_NEAR_ZOMBIES; i++)
        if (s_zombie_vbo[i] != NULL)
            s_dynamic_bytes += (unsigned)s_zombie_vert_cap * (unsigned)sizeof(MeshVertex);
    if (s_zombie_far_vbo != NULL)
        s_dynamic_bytes += (unsigned)s_zombie_vert_cap * (unsigned)sizeof(MeshVertex);
    for (i = 0; i < WEAPON_COUNT; i++) {
        if (s_gpm_gun_mesh[i].verts != NULL)
            s_dynamic_bytes += (unsigned)s_gpm_gun_mesh[i].vert_count *
                               (unsigned)sizeof(MeshVertex);
        if (s_gpm_gun_mesh[i].ibo != NULL)
            s_dynamic_bytes += (unsigned)s_gpm_gun_mesh[i].index_count * 2u;
    }
}

/* Releases every GPU buffer derived from imported GPM data (but not the
 * GpmModel structs themselves, which r3d_assets_free owns). */
static void r3d_model_runtime_free(void)
{
    r3d_skin_buffers_free();
    r3d_gun_meshes_free();
    s_gpm_hero_ready = false;
}

/* (Re)builds gun meshes and hero/zombie skinning buffers from whatever the
 * current s_gpm_* models are.  Safe after a reload; returns false if any layer
 * could not be prepared (rendering then uses that layer's fallback). */
static bool r3d_model_runtime_build(void)
{
    bool ok = true;
    int w, z;

    r3d_gun_meshes_build();
    for (w = 0; w < WEAPON_COUNT; w++) {
        if (s_gpm_gun_ok[w] && s_gpm_gun_mesh[w].verts == NULL)
            ok = false;
    }

    s_gpm_hero_ready = false;
    if (s_gpm_hero_ok) {
        if (r3d_hero_buffers_alloc(&s_gpm_hero)) {
            r3d_hero_configure(&s_gpm_hero);
            s_gpm_hero_ready = true;
        } else {
            ok = false;
        }
    }

    for (z = 0; z < R3D_NEAR_ZOMBIES; z++)
        s_zombie_slot_inited[z] = false;
    s_zombie_far_inited = false;
    if (s_gpm_zombie_ok) {
        if (r3d_alloc_skin_zombie()) {
            skin_init(&s_zombie_far_skin, &s_gpm_zombie);
            s_zombie_far_inited = true;
        } else {
            ok = false;
        }
    }

    r3d_dynamic_bytes_update();
    r3d_update_model_sources();
    return ok;
}
/* ------------------------------------------------------------------ */
/* imported hero                                                      */
/* ------------------------------------------------------------------ */

/* Calls the loader for procedural attachment meshes once the imported hero
 * skeleton is known (they are built in model space around their bones). */
static void r3d_build_wolf_attachments(MeshVertex *scratch, int scratch_cap)
{
    if (s_hero_bone_head < 0 && s_hero_bone_hips < 0)
        return;
    mesh_build_wolf_head(&s_wolf_head, &s_arena, scratch, scratch_cap);
    mesh_build_wolf_tail(s_wolf_tail, &s_arena, scratch, scratch_cap);
    r3d_asset_log("attach: head=%u verts tail=%u/%u/%u verts",
                  s_wolf_head.vertex_count, s_wolf_tail[0].vertex_count,
                  s_wolf_tail[1].vertex_count, s_wolf_tail[2].vertex_count);
}

/* Decides the hero clip from core state and advances the clocks. */
static void hero_gpm_update_anim(const Player *player, bool idle_only)
{
    int clip;
    float clip_time;

    if (idle_only) {
        clip = s_hero_clip_idle;
        clip_time = s_title_clock * 0.35f;
    } else if (!player->alive) {
        clip = s_hero_clip_death;
        s_hero_death_clock += s_draw_dt;
        clip_time = s_hero_death_clock;
    } else {
        bool firing = player->muzzle_flash > 0.001f;
        if (firing && !s_hero_was_firing)
            s_hero_shoot_clock = 0.0f;
        if (firing)
            s_hero_shoot_clock += s_draw_dt;
        s_hero_was_firing = firing;

        if (firing && s_hero_clip_shoot >= 0) {
            clip = s_hero_clip_shoot;
            clip_time = s_hero_shoot_clock;
        } else if (player->anim_speed > 1.15f && s_hero_clip_run >= 0) {
            clip = s_hero_clip_run;
            clip_time = player->anim_time;
        } else if (player->anim_speed > 0.05f && s_hero_clip_walk >= 0) {
            clip = s_hero_clip_walk;
            clip_time = player->anim_time;
        } else {
            clip = s_hero_clip_idle;
            clip_time = player->anim_time;
        }
    }

    if (player->alive) {
        s_hero_death_clock = 0.0f;
        s_hero_was_alive = true;
    } else if (s_hero_was_alive) {
        s_hero_death_clock = 0.0f;
        s_hero_was_alive = false;
    }

    skin_set_clip(&s_hero_skin, clip, clip_time);
    skin_update(&s_hero_skin, s_draw_dt);
}

/* Wolf head + bushy tail, attached to the imported skeleton bones. */
static void hero_gpm_draw_attachments(const C3D_Mtx *root_view, const C3D_Mtx *root)
{
    C3D_Mtx pose, attach, local, mv;
    /* procedural part scale in model units, so it stays 1.8-m-relative */
    float attach_scale = 1.0f / (s_hero_model_scale > 0.01f ? s_hero_model_scale : 1.0f);
    int i;

    /* --- head (wolf identity) ------------------------------------- */
    if (s_wolf_head.vbo != NULL && s_hero_bone_head >= 0) {
        mtx_from_floats(&pose, &s_hero_skin.pose[s_hero_bone_head][0]);
        attach_local(&local, s_hero_head_rest,
                     v3(0.0f, 0.100f, 0.020f), v3(0, 0, 0),
                     attach_scale * 1.08f);
        Mtx_Multiply(&attach, &pose, &local);
        Mtx_Multiply(&mv, root_view, &attach);
        draw_mesh_mv(&mv, &s_wolf_head, 1.0f, 1.0f, 1.0f, 0.0f);
    }

    /* --- tail: three segments hanging off the hips bone ------------ */
    if (s_wolf_tail[0].vbo != NULL && s_hero_bone_hips >= 0) {
        C3D_Mtx chain;
        float sway = sinf(s_title_clock * 2.0f + (float)(s_hero_skin.model ? s_hero_skin.model->anim_count : 0)) * 0.15f;
        float sway2 = sinf(s_title_clock * 1.7f + 1.3f) * 0.22f;

        mtx_from_floats(&pose, &s_hero_skin.pose[s_hero_bone_hips][0]);
        /* base: behind/down from the hips at ~52 degrees, clear of the coat */
        attach_local(&chain, s_hero_hips_rest,
                     v3(0.0f, -0.02f, -0.10f),
                     v3(-52.0f, sway * 40.0f, 0.0f),
                     attach_scale);
        Mtx_Multiply(&attach, &pose, &chain);
        Mtx_Multiply(&mv, root_view, &attach);
        draw_mesh_mv(&mv, &s_wolf_tail[0], 1.0f, 1.0f, 1.0f, 0.0f);

        for (i = 1; i < 3; i++) {
            C3D_Mtx step;
            float len = (i == 1) ? 0.20f : 0.18f;
            float s = (i == 1) ? sway2 : -sway2 * 0.6f;
            float kink = (i == 1) ? 9.0f : -11.0f;
            attach_local(&step, v3(0.0f, 0.0f, -len), v3(0, 0, 0),
                         v3(-9.0f + s * 26.0f, s * 12.0f, kink), 1.0f);
            {
                C3D_Mtx tmp;
                Mtx_Multiply(&tmp, &attach, &step);
                attach = tmp;
            }
            Mtx_Multiply(&mv, root_view, &attach);
            draw_mesh_mv(&mv, &s_wolf_tail[i], 1.0f, 1.0f, 1.0f, 0.0f);
        }
    }
    (void)root;
}

/* Full imported hero: skinned body + procedural wolf head/tail + gun. */
static void hero_draw_gpm(const Player *player, const C3D_Mtx *view,
                          bool idle_only, WeaponId weapon, Vec3 *muzzle)
{
    C3D_Mtx root, root_view, local, attach, mv, pose;
    Vec3 pos = player->transform.pos;
    float bob;
    int count;

    hero_gpm_update_anim(player, idle_only);

    /* root: feet on the ground, model yaw ~ player yaw, scaled to 1.8 m */
    bob = idle_only ? 0.0f : (clampf(player->anim_speed, 0.0f, 1.0f) * 0.0f);
    Mtx_Identity(&root);
    Mtx_Translate(&root, pos.x, pos.y + bob, pos.z, true);
    Mtx_RotateY(&root, player->transform.yaw, true);
    Mtx_Scale(&root, s_hero_model_scale, s_hero_model_scale, s_hero_model_scale);
    /* lift so the model's lowest vertex sits at y=0 */
    Mtx_Translate(&root, 0.0f, -s_hero_bbox_min_y, 0.0f, true);
    Mtx_Multiply(&root_view, view, &root);

    count = skin_vertices(&s_hero_skin, (SkinVertex *)s_hero_vbo, s_hero_vert_cap);
    if (count > 0) {
        C3D_CullFace(GPU_CULL_NONE);
        s_skin_verts_frame += (unsigned)count;
        s_skin_bones_frame += s_hero_skin.model ? s_hero_skin.model->bone_count : 0u;
        s_skin_instances_frame++;
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_u_model_view, &root_view);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u_tint, 1.0f, 1.0f, 1.0f, 0.0f);
        {
            C3D_BufInfo *buf = C3D_GetBufInfo();
            BufInfo_Init(buf);
            BufInfo_Add(buf, s_hero_vbo, MESH_VERTEX_STRIDE, MESH_ATTR_COUNT,
                        MESH_ATTR_PERMUTATION);
        }
        C3D_DrawElements(GPU_TRIANGLES, s_hero_index_count, C3D_UNSIGNED_SHORT,
                         s_hero_ibo);
        C3D_CullFace(GPU_CULL_BACK_CCW);
        s_draw_calls++;
        s_draw_verts += (unsigned)count;
    }

    hero_gpm_draw_attachments(&root_view, &root);

    /* --- third-person gun on the wrist/hand bone ------------------ */
    if (s_gpm_gun_ok[weapon] && s_gpm_gun_mesh[weapon].verts != NULL) {
        if (s_hero_bone_wrist >= 0) {
            static const Vec3 tp_pos[WEAPON_COUNT] = {
                { 0.0f, -0.010f, 0.080f },  /* flintlock */
                { 0.0f, -0.010f, 0.070f },  /* revolver  */
                { 0.0f, -0.020f, 0.280f },  /* musket    */
            };
            static const Vec3 tp_rot[WEAPON_COUNT] = {
                { -28.0f, 0.0f, 0.0f },     /* carried low, barrel down-forward */
                { -28.0f, 0.0f, 0.0f },
                { -14.0f, 0.0f, 0.0f },
            };
            float gs = s_gpm_gun_scale[weapon];
            mtx_from_floats(&pose, &s_hero_skin.pose[s_hero_bone_wrist][0]);
            attach_local(&local, s_hero_wrist_rest,
                         v3(tp_pos[weapon].x, tp_pos[weapon].y, tp_pos[weapon].z),
                         tp_rot[weapon], gs);
            Mtx_Multiply(&attach, &pose, &local);
            Mtx_Multiply(&mv, &root_view, &attach);
            C3D_CullFace(GPU_CULL_NONE);
            draw_gpm_gun(&mv, weapon, 0.10f);
            C3D_CullFace(GPU_CULL_BACK_CCW);
            if (muzzle != NULL) {
                Vec3 ml = s_gpm_gun_muzzle_local[weapon];
                C3D_FVec mp = Mtx_MultiplyFVecH(&attach,
                                                FVec4_New(ml.x, ml.y, ml.z, 1.0f));
                mp = Mtx_MultiplyFVecH(&root, FVec4_New(mp.x, mp.y, mp.z, 1.0f));
                *muzzle = v3(mp.x, mp.y, mp.z);
            }
        } else {
            /* no wrist bone: hang it at the right hip like the procedural rig */
            attach_local(&local, v3(0.24f, 0.97f, 0.10f), v3(0, 0, 0),
                         v3(0.0f, -9.0f, 0.0f), s_gpm_gun_scale[weapon]);
            Mtx_Multiply(&mv, &root_view, &local);
            C3D_CullFace(GPU_CULL_NONE);
            draw_gpm_gun(&mv, weapon, 0.10f);
            C3D_CullFace(GPU_CULL_BACK_CCW);
            if (muzzle != NULL) {
                C3D_FVec mp = Mtx_MultiplyFVecH(&mv, FVec4_New(0.0f, 0.02f, 0.3f, 1.0f));
                *muzzle = v3(mp.x, mp.y, mp.z);
            }
        }
    } else if (muzzle != NULL) {
        /* procedural gun fallback: right hip, hero-space */
        C3D_Mtx gun_world;
        attach_local(&local, v3(0.33f, 0.99f, 0.06f), v3(0, 0, 0),
                     v3(0.0f, 31.5f, -2.0f), 1.25f);
        Mtx_Multiply(&gun_world, &root, &local);
        Mtx_Multiply(&mv, view, &gun_world);
        draw_mesh_mv(&mv, &s_weapons[weapon], 1.0f, 1.0f, 1.0f, 0.10f);
        {
            C3D_FVec mp = Mtx_MultiplyFVecH(&gun_world,
                                            FVec4_New(0.0f, 0.028f, s_muzzle_z[weapon], 1.0f));
            *muzzle = v3(mp.x, mp.y, mp.z);
        }
    }
}

/* ------------------------------------------------------------------ */
/* imported zombies                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int   index;
    float dist2;
} ZDrawEntry;

#define R3D_MAX_ZDRAWS MAX_ZOMBIES

/* Picks the clip for a zombie state. */
static void zombie_gpm_clip(const Zombie *z, int *clip, float *time)
{
    /* Core's anim_time already advances at a speed/type-dependent rate, but the
     * imported clips are long (walk 4 s), so apply a small per-type gain for
     * readability: shamblers/brutes read as deliberate, runners already fast. */
    float gain = 1.15f;

    switch (z->type) {
    case ZOMBIE_RUNNER: gain = 1.0f; break;
    case ZOMBIE_BRUTE:  gain = 1.05f; break;
    default:            gain = 1.25f; break;
    }

    switch (z->state) {
    case ZOMBIE_ATTACK:
        *clip = s_zombie_clip_attack;
        /* attack_timer runs down through the windup; anim_time also advances
         * during the swing, so speed it up to land the strike in-step */
        *time = z->anim_time * 1.8f;
        break;
    case ZOMBIE_STAGGER:
        *clip = s_zombie_clip_hit;
        *time = z->anim_time * 1.2f;
        break;
    case ZOMBIE_DYING:
        *clip = s_zombie_clip_death;
        *time = clampf(z->death_time, 0.0f, 1.0f) *
                gpm_anim_duration(&s_gpm_zombie, s_zombie_clip_death);
        break;
    case ZOMBIE_IDLE:
        *clip = s_zombie_clip_idle;
        *time = z->anim_time * gain;
        break;
    default:
        if (z->speed > 2.0f && s_zombie_clip_run >= 0) {
            *clip = s_zombie_clip_run;
        } else {
            *clip = s_zombie_clip_walk;
        }
        *time = z->anim_time * gain;
        break;
    }
}

/* Builds the model matrix for one zombie (feet at z->pos, scaled per type). */
static void zombie_gpm_model(C3D_Mtx *model, const Zombie *z)
{
    float height;
    float scale;

    height = zombie_type_height(z->type);
    scale = height / (s_gpm_zombie.bbox_max.y - s_gpm_zombie.bbox_min.y);
    Mtx_Identity(model);
    Mtx_Translate(model, z->pos.x, z->pos.y, z->pos.z, true);
    Mtx_RotateY(model, z->yaw, true);
    Mtx_Scale(model, scale, scale, scale);
    Mtx_Translate(model, 0.0f, -s_zombie_bbox_min_y, 0.0f, true);
    if (z->state == ZOMBIE_DYING) {
        float k = clampf(z->death_time, 0.0f, 1.0f);
        Mtx_Translate(model, 0.0f, -0.10f * k, 0.0f, true);
        Mtx_RotateX(model, 1.40f * k, true);
    }
    (void)scale;
}

/* Tint per zombie type (the base GPM is shared until per-type models exist). */
static void zombie_gpm_tint(const Zombie *z, float *r, float *g, float *b)
{
    *r = 1.0f;
    *g = 1.0f;
    *b = 1.0f;
    switch (z->type) {
    case ZOMBIE_RUNNER:
        *r = 0.95f; *g = 1.02f; *b = 1.04f;
        break;
    case ZOMBIE_BRUTE:
        *r = 1.18f; *g = 0.97f; *b = 0.88f;
        break;
    case ZOMBIE_FOREMAN:
        /* charred hide; the additive red eyes sell the boss */
        *r = 0.40f; *g = 0.35f; *b = 0.34f;
        break;
    default:
        break;
    }
    if (z->state == ZOMBIE_STAGGER) {
        *r *= 1.5f;
        *g *= 1.2f;
        *b *= 1.1f;
    } else if (z->state == ZOMBIE_DYING) {
        float k = clampf(z->death_time, 0.0f, 1.0f);
        *r *= 1.0f - 0.45f * k;
        *g *= 1.0f - 0.45f * k;
        *b *= 1.0f - 0.40f * k;
    }
}


/* Draws one zombie from the shared far pose (no per-zombie skinning). */
static void zombie_gpm_draw_far(const Zombie *z, const C3D_Mtx *view)
{
    C3D_Mtx model, mv;
    float r, g, b;

    zombie_gpm_model(&model, z);
    zombie_gpm_tint(z, &r, &g, &b);
    Mtx_Multiply(&mv, view, &model);
    draw_mesh_mv(&mv, &(Mesh){ .vbo = s_zombie_far_vbo,
                              .vertex_count = (uint32_t)s_zombie_vert_count },
                 r, g, b, 0.0f);
}

/* Draws every zombie: nearest N fully skinned, the rest on one shared pose. */
static void draw_zombies_gpm(const GraveGame *game, const C3D_Mtx *view,
                             bool hero_visible)
{
    ZDrawEntry list[R3D_MAX_ZDRAWS];
    int n = 0, i, j;
    int budget = R3D_SKIN_BUDGET - (hero_visible ? s_hero_vert_cap : 0);
    int near_cap = R3D_NEAR_ZOMBIES;
    int slot = 0;
    bool far_ready = false;
    C3D_Mtx mv;
    float cam_x = game->camera_pos.x;
    s_zombie_far_clock += s_draw_dt;
    float cam_z = game->camera_pos.z;

    for (i = 0; i < MAX_ZOMBIES && n < R3D_MAX_ZDRAWS; i++) {
        const Zombie *z = &game->zombies[i];
        float dx, dz;
        if (!z->active || z->state == ZOMBIE_DEAD)
            continue;
        if (!world_visible(view, z->pos, 2.0f))
            continue;
        dx = z->pos.x - cam_x;
        dz = z->pos.z - cam_z;
        list[n].index = i;
        list[n].dist2 = dx * dx + dz * dz;
        n++;
    }

    /* Crowded waves: one fewer fully-skinned zombie keeps the frame under the
     * vsync budget (the extra instance costs ~4.5k skinned verts). */
    if (n > 8)
        near_cap = 3;

    /* nearest first (insertion sort: n is small) */
    for (i = 1; i < n; i++) {
        ZDrawEntry e = list[i];
        j = i - 1;
        while (j >= 0 && list[j].dist2 > e.dist2) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = e;
    }

    for (i = 0; i < n; i++) {
        const Zombie *z = &game->zombies[list[i].index];
        bool can_skin = (list[i].dist2 <= R3D_ZOMBIE_LOD_DIST * R3D_ZOMBIE_LOD_DIST) &&
                        (slot < near_cap) &&
                        (budget >= s_zombie_vert_count);
        if (can_skin) {
            int clip;
            float clip_time;
            int count;
            SkinInstance *inst = &s_zombie_skin[slot];

            if (!s_zombie_slot_inited[slot]) {
                skin_init(inst, &s_gpm_zombie);
                s_zombie_slot_inited[slot] = true;
            }
            zombie_gpm_clip(z, &clip, &clip_time);
            skin_set_clip(inst, clip, clip_time);
            skin_update(inst, s_draw_dt);
            count = skin_vertices(inst, (SkinVertex *)s_zombie_vbo[slot],
                                  s_zombie_vert_cap);
            if (count > 0) {
                if (z->type == ZOMBIE_FOREMAN && s_zombie_bone_head >= 0) {
                    C3D_Mtx model;
                    const float *bw = inst->world[s_zombie_bone_head];
                    C3D_FVec hp;
                    zombie_gpm_model(&model, z);
                    hp = Mtx_MultiplyFVecH(&model,
                                           FVec4_New(bw[3], bw[7], bw[11], 1.0f));
                    s_boss_head_pos = v3(hp.x, hp.y, hp.z);
                    s_boss_head_valid = true;
                }
                s_skin_verts_frame += (unsigned)count;
                s_skin_bones_frame += s_gpm_zombie.bone_count;
                s_skin_instances_frame++;
                float r, g, b;
                C3D_Mtx model;
                zombie_gpm_model(&model, z);
                Mtx_Multiply(&mv, view, &model);
                zombie_gpm_tint(z, &r, &g, &b);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_u_model_view, &mv);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u_tint, r, g, b, 0.0f);
                {
                    C3D_BufInfo *buf = C3D_GetBufInfo();
                    BufInfo_Init(buf);
                    BufInfo_Add(buf, s_zombie_vbo[slot], MESH_VERTEX_STRIDE,
                                MESH_ATTR_COUNT, MESH_ATTR_PERMUTATION);
                }
                C3D_CullFace(GPU_CULL_NONE);
                C3D_DrawElements(GPU_TRIANGLES, s_zombie_index_count,
                                 C3D_UNSIGNED_SHORT, s_zombie_ibo);
                C3D_CullFace(GPU_CULL_BACK_CCW);
                s_draw_calls++;
                s_draw_verts += (unsigned)count;
                budget -= s_zombie_vert_count;
                slot++;
            }
            continue;
        }

        /* far / over-budget: shared pose, cheap */
        if (!far_ready) {
            if (!s_zombie_far_inited) {
                skin_init(&s_zombie_far_skin, &s_gpm_zombie);
                s_zombie_far_inited = true;
            }
            skin_set_clip(&s_zombie_far_skin, s_zombie_clip_walk,
                          s_zombie_far_clock * 1.25f);
            skin_update(&s_zombie_far_skin, s_draw_dt);
            skin_vertices(&s_zombie_far_skin, (SkinVertex *)s_zombie_far_vbo,
                          s_zombie_vert_cap);
            s_skin_verts_frame += s_gpm_zombie.vertex_count;
            s_skin_bones_frame += s_gpm_zombie.bone_count;
            s_skin_instances_frame++;
            far_ready = true;
        }
        zombie_gpm_draw_far(z, view);
    }
}

/* Draws a single titled-diorama zombie through the skinned path (near). */
static void zombie_gpm_draw_one(const Zombie *z, const C3D_Mtx *view, float clock)
{
    static SkinInstance inst;
    static bool inited;
    int clip = s_zombie_clip_walk;
    float time = clock;
    int count;
    C3D_Mtx model, mv;
    float r, g, b;

    if (!inited) {
        skin_init(&inst, &s_gpm_zombie);
        inited = true;
    }
    if (z->state == ZOMBIE_ATTACK && s_zombie_clip_attack >= 0) {
        clip = s_zombie_clip_attack;
    }
    skin_set_clip(&inst, clip, time * 1.25f);
    skin_update(&inst, 1.0f / 60.0f);
    count = skin_vertices(&inst, (SkinVertex *)s_zombie_vbo[0], s_zombie_vert_cap);
    if (count <= 0)
        return;
    s_skin_verts_frame += (unsigned)count;
    s_skin_bones_frame += s_gpm_zombie.bone_count;
    s_skin_instances_frame++;
    zombie_gpm_model(&model, z);
    Mtx_Multiply(&mv, view, &model);
    zombie_gpm_tint(z, &r, &g, &b);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_u_model_view, &mv);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u_tint, r, g, b, 0.0f);
    {
        C3D_BufInfo *buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, s_zombie_vbo[0], MESH_VERTEX_STRIDE, MESH_ATTR_COUNT,
                    MESH_ATTR_PERMUTATION);
    }
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DrawElements(GPU_TRIANGLES, s_zombie_index_count, C3D_UNSIGNED_SHORT,
                     s_zombie_ibo);
    C3D_CullFace(GPU_CULL_BACK_CCW);
    s_draw_calls++;
    s_draw_verts += (unsigned)count;
}

/* ------------------------------------------------------------------ */
/* imported guns (first-person viewmodel)                             */
/* ------------------------------------------------------------------ */

/* Per-weapon first-person placement, tuned for the imported static GPMs. */
/* First-person viewmodel tuning (camera space, metres, degrees):
 * pos = local offset, scale = viewmodel scale, pitch/yaw = camera-space aim. */
static const Vec3 s_gpm_fp_pos[WEAPON_COUNT] = {
    { 0.085f, -0.090f, -0.420f },   /* flintlock */
    { 0.085f, -0.086f, -0.400f },   /* revolver  */
    { 0.110f, -0.105f, -0.950f },   /* musket    */
};
static const float s_gpm_fp_scale[WEAPON_COUNT] = { 0.42f, 0.46f, 0.45f };
static const float s_gpm_fp_pitch[WEAPON_COUNT] = { 5.0f, 5.0f, 3.0f };
static const float s_gpm_fp_yaw[WEAPON_COUNT] = { 14.0f, 14.0f, 8.0f };

static void draw_first_person_gun_gpm(const GraveGame *game, WeaponId weapon,
                                      Vec3 *muzzle_out)
{
    const Player *p = &game->player;
    float t = game->zone_time;
    float sway_x = sinf(t * 1.9f) * 0.005f +
                   clampf(p->anim_speed, 0.0f, 1.0f) * sinf(p->anim_time * 2.0f) * 0.006f;
    float sway_y = cosf(t * 1.3f) * 0.004f;
    float recoil = clampf(p->recoil, 0.0f, 0.25f);
    float kick = clampf(p->muzzle_flash / R3D_MUZZLE_FLASH_TIME, 0.0f, 1.0f);
    C3D_Mtx model;

    Mtx_Identity(&model);
    Mtx_Translate(&model,
                  s_gpm_fp_pos[weapon].x + sway_x,
                  s_gpm_fp_pos[weapon].y + sway_y,
                  s_gpm_fp_pos[weapon].z + recoil * 0.5f + kick * 0.02f, true);
    /* camera-space aim first, then yaw the (muzzle +Z) model away from the
     * camera: T * Rx * Ry(180 + yaw) * Rz * S */
    Mtx_RotateX(&model, deg2rad(s_gpm_fp_pitch[weapon]) + recoil * 1.6f +
                         kick * 0.1f, true);
    Mtx_RotateY(&model, deg2rad(180.0f + s_gpm_fp_yaw[weapon]), true);
    Mtx_RotateZ(&model, sway_x * 0.6f, true);
    Mtx_Scale(&model, s_gpm_gun_scale[weapon] * s_gpm_fp_scale[weapon],
              s_gpm_gun_scale[weapon] * s_gpm_fp_scale[weapon],
              s_gpm_gun_scale[weapon] * s_gpm_fp_scale[weapon]);
    C3D_CullFace(GPU_CULL_NONE);
    draw_gpm_gun(&model, weapon, 0.18f);
    C3D_CullFace(GPU_CULL_BACK_CCW);

    if (muzzle_out != NULL) {
        Vec3 ml = s_gpm_gun_muzzle_local[weapon];
        C3D_FVec mp = Mtx_MultiplyFVecH(&model, FVec4_New(ml.x, ml.y, ml.z, 1.0f));
        *muzzle_out = v3(mp.x, mp.y, mp.z);  /* camera space for FP */
    }
}


/* ------------------------------------------------------------------ */
/* world drawing                                                      */
/* ------------------------------------------------------------------ */

static Vec3 weapon_muzzle_world(WeaponId weapon, const Vec3 *cam_pos,
                                const Vec3 *fwd, const Vec3 *right,
                                const Vec3 *up)
{
    WeaponId w = clamp_weapon(weapon);
    /* mirror the viewmodel transform: the gun is rotated 180 deg around Y,
     * so its local +Z barrel ends up `muzzle_z - vm_z` metres ahead. */
    float depth = s_muzzle_z[w] - s_vm_z[w];
    Vec3 p = v3_add(*cam_pos, v3_scale(*fwd, depth));

    p = v3_add(p, v3_scale(*right, s_vm_x[w]));
    p = v3_add(p, v3_scale(*up, s_vm_y[w] + 0.03f));
    return p;
}

static void draw_world_geometry(const GraveGame *game, const C3D_Mtx *view,
                                bool first_person, WeaponId weapon,
                                const Vec3 *cam_pos, const Vec3 *fwd,
                                const Vec3 *right, const Vec3 *up,
                                Vec3 *muzzle_out, bool *muzzle_valid)
{
    Vec3 muzzle;
    Vec3 fp_muzzle_cam = v3(0.0f, 0.0f, 0.0f);
    bool fp_muzzle_valid = false;
    bool hero_visible = !first_person;

    /* first person: viewmodel goes in before the world so it is never clipped
     * into a wall (depth is already correct from the near camera position) */
    if (first_person) {
        if (s_gpm_gun_ok[weapon] && s_gpm_gun_mesh[weapon].verts != NULL) {
            draw_first_person_gun_gpm(game, weapon, &fp_muzzle_cam);
            fp_muzzle_valid = true;
        } else {
            draw_first_person_gun(game, weapon);
        }
    }

    /* distant hills: plain silhouettes, no culling assumptions */
    C3D_CullFace(GPU_CULL_NONE);
    {
        const R3dZonePalette *pal = r3d_palette();
        draw_mesh_mv(view, &s_hills, pal->hills_r, pal->hills_g, pal->hills_b, 1.0f);
    }
    C3D_CullFace(GPU_CULL_BACK_CCW);

    /* ground */
    {
        const R3dZonePalette *pal = r3d_palette();
        draw_mesh_mv(view, &s_ground, pal->ground_r, pal->ground_g,
                     pal->ground_b, 0.0f);
    }

    /* blob shadows for props + characters */
    draw_shadows(game, view, first_person);

    /* props */
    draw_props(game, view);

    /* pickups */
    draw_pickups(game, view);

    /* weapon muzzle in the world, for flash + smoke */
    if (first_person) {
        if (fp_muzzle_valid) {
            /* camera-space muzzle -> world */
            muzzle = v3_add(*cam_pos, v3_scale(*right, fp_muzzle_cam.x));
            muzzle = v3_add(muzzle, v3_scale(*up, fp_muzzle_cam.y));
            muzzle = v3_add(muzzle, v3_scale(*fwd, -fp_muzzle_cam.z));
        } else {
            muzzle = weapon_muzzle_world(weapon, cam_pos, fwd, right, up);
        }
    } else if (s_gpm_hero_ready && !s_hero_rig_override) {
        muzzle = v3_add(game->player.transform.pos, v3(0.0f, 1.2f, 0.0f));
        hero_draw_gpm(&game->player, view, false, weapon, &muzzle);
    } else {
        muzzle = v3_add(game->player.transform.pos, v3(0.0f, 1.2f, 0.0f));
        hero_draw(&game->player, view, game->zone_time, false, weapon, &muzzle);
    }

    /* zombies */
    if (s_gpm_zombie_ok && s_zombie_vbo[0] != NULL) {
        draw_zombies_gpm(game, view, hero_visible);
    } else {
        draw_zombies(game, view);
    }

    *muzzle_out = muzzle;
    *muzzle_valid = true;

    /* effects: tracers, sparks, smoke, muzzle flash */
    {
        float flash = clampf(game->player.muzzle_flash / R3D_MUZZLE_FLASH_TIME,
                             0.0f, 1.0f);
        effects_build(game, cam_pos, fwd, right, up, muzzle,
                      flash > 0.0f, flash, first_person ? 0.30f : 1.0f);
    }
    zone_glow_build(game, view, right, up);
    begin_additive();
    dyn_flush(view, 1.0f, 1.0f, 1.0f, 1.0f);
    end_additive();
}

static void draw_eye(const GraveGame *game, const C3D_Mtx *projection,
                     const C3D_Mtx *view, const Vec3 *cam_pos,
                     const Vec3 *fwd, const Vec3 *right, const Vec3 *up,
                     bool first_person, bool crosshair, C3D_RenderTarget *target)
{
    WeaponId weapon;
    Vec3 muzzle = *cam_pos;

    C3D_RenderTargetClear(target, C3D_CLEAR_ALL, r3d_palette()->clear_color, 0);
    C3D_FrameDrawOn(target);
    bind_pass_state(projection, view);

    weapon = clamp_weapon(game != NULL ? game->player.weapon : WEAPON_FLINTLOCK);

    if (game == NULL) {
        s_last_muzzle = *cam_pos;
        s_have_last_muzzle = true;
        return;
    }

    draw_world_geometry(game, view, first_person, weapon, cam_pos, fwd, right,
                        up, &muzzle, &s_have_last_muzzle);
    s_last_muzzle = muzzle;

    if (first_person && crosshair)
        draw_crosshair();
}

/* ------------------------------------------------------------------ */
/* title screen diorama                                               */
/* ------------------------------------------------------------------ */

static void draw_title_diorama(const GraveGame *game, const Vec3 *cam_pos,
                               const Vec3 *fwd, const Vec3 *right,
                               const Vec3 *up, const C3D_Mtx *view)
{
    static const struct {
        PropKind kind;
        float x, z, yaw;
        float hx, hy, hz;
        unsigned char color;
    } set[] = {
        { PROP_GATE,       0.20f, -5.20f,  0.00f, 1.50f, 1.55f, 0.14f, PROP_GATE },
        { PROP_FENCE,     -2.90f, -4.40f,  0.35f, 1.20f, 0.95f, 0.10f, PROP_FENCE },
        { PROP_FENCE,      3.10f, -4.60f, -0.35f, 1.20f, 0.95f, 0.10f, PROP_FENCE },
        { PROP_WELL,      -3.40f, -2.40f,  0.40f, 0.95f, 0.60f, 0.95f, PROP_WELL },
        { PROP_DEADTREE,   4.30f, -2.00f,  0.00f, 0.34f, 2.20f, 0.34f, PROP_DEADTREE },
        { PROP_TOMBSTONE, -1.30f, -3.20f,  0.30f, 0.30f, 0.44f, 0.17f, PROP_TOMBSTONE },
        { PROP_TOMBSTONE,  2.00f, -3.50f, -0.20f, 0.28f, 0.40f, 0.16f, PROP_TOMBSTONE },
        { PROP_CRATE,     -2.40f, -1.00f,  0.20f, 0.36f, 0.32f, 0.36f, PROP_CRATE },
        { PROP_BARREL,     2.60f, -0.30f,  0.00f, 0.30f, 0.42f, 0.30f, PROP_BARREL },
        /* foreground framing pieces */
        { PROP_FENCE,     -3.30f,  1.70f,  1.10f, 1.10f, 0.90f, 0.10f, PROP_FENCE },
        { PROP_TOMBSTONE,  3.30f,  2.30f, -0.45f, 0.26f, 0.36f, 0.15f, PROP_TOMBSTONE },
    };
    static const struct {
        float x, z;
        ZombieType type;
        float phase;
    } zombies[] = {
        { -5.00f, -3.50f, ZOMBIE_SHAMBLER, 0.0f },
        {  6.30f, -4.00f, ZOMBIE_SHAMBLER, 1.7f },
        { -1.40f, -9.00f, ZOMBIE_RUNNER,   3.1f },
    };
    const HeroRig *rig = s_hero_rig;
    Player hero;
    int i;

    memset(&hero, 0, sizeof(hero));
    hero.transform.pos = v3(1.15f, 0.0f, 1.15f);
    hero.transform.yaw = atan2f(cam_pos->x - hero.transform.pos.x,
                                cam_pos->z - hero.transform.pos.z);
    hero.transform.pitch = 0.20f;   /* chin up: shows the muzzle under the hat */
    hero.anim_time = s_title_clock;
    hero.anim_speed = 0.0f;
    hero.weapon = WEAPON_REVOLVER;
    hero.camera_first_person = false;
    hero.alive = true;
    hero.max_hp = 100.0f;
    hero.hp = 100.0f;

    (void)game;

    /* distant hills + ground */
    C3D_CullFace(GPU_CULL_NONE);
    draw_mesh_mv(view, &s_hills, 1.0f, 1.0f, 1.0f, 1.0f);
    C3D_CullFace(GPU_CULL_BACK_CCW);
    draw_mesh_mv(view, &s_ground, 1.0f, 1.0f, 1.0f, 0.0f);

    /* shadows first, then the silhouettes */
    for (i = 0; i < (int)(sizeof(set) / sizeof(set[0])); i++) {
        draw_shadow(view, v3(set[i].x, 0.0f, set[i].z),
                    set[i].hx * 1.05f, set[i].hz * 1.05f);
    }
    draw_shadow(view, hero.transform.pos, 0.36f, 0.30f);
    for (i = 0; i < (int)(sizeof(zombies) / sizeof(zombies[0])); i++) {
        float s = zombie_scale(zombies[i].type);
        draw_shadow(view, v3(zombies[i].x, 0.0f, zombies[i].z),
                    0.42f * s, 0.36f * s);
    }

    for (i = 0; i < (int)(sizeof(set) / sizeof(set[0])); i++) {
        Prop p;
        memset(&p, 0, sizeof(p));
        p.pos = v3(set[i].x, 0.0f, set[i].z);
        p.half = v3(set[i].hx, set[i].hy, set[i].hz);
        p.yaw = set[i].yaw;
        p.kind = set[i].kind;
        p.color = set[i].color;
        draw_prop_one(&p, i, view);
    }

    if (s_gpm_hero_ready && !s_hero_rig_override) {
        hero_draw_gpm(&hero, view, true, WEAPON_REVOLVER, NULL);
    } else if (rig != NULL) {
        hero_draw(&hero, view, s_title_clock, true, WEAPON_REVOLVER, NULL);
    }

    /* shamblers drifting through the fog, walking towards the hero */
    for (i = 0; i < (int)(sizeof(zombies) / sizeof(zombies[0])); i++) {
        Zombie z;
        memset(&z, 0, sizeof(z));
        z.active = true;
        z.type = zombies[i].type;
        z.state = ZOMBIE_CHASE;
        z.pos = v3(zombies[i].x, 0.0f, zombies[i].z);
        z.yaw = atan2f(hero.transform.pos.x - z.pos.x,
                       hero.transform.pos.z - z.pos.z);
        z.anim_time = s_title_clock + zombies[i].phase;
        z.speed = 0.6f;
        if (s_gpm_zombie_ok && s_zombie_vbo[0] != NULL)
            zombie_gpm_draw_one(&z, view, z.anim_time);
        else
            draw_zombie(&z, i + 2, view);
    }

    /* drifting dust motes catch the light in the diorama */
    dyn_reset();
    dust_build(right, up);
    begin_additive();
    dyn_flush(view, 1.0f, 1.0f, 1.0f, 1.0f);
    end_additive();

    (void)fwd;
    (void)right;
    (void)up;
}

/* ------------------------------------------------------------------ */
/* init / fini                                                        */
/* ------------------------------------------------------------------ */

static void build_meshes(MeshVertex *scratch, int scratch_cap, bool skip_zombie_poses)
{
    int i, t, p;

    mesh_build_ground(&s_ground, &s_arena, scratch, scratch_cap,
                      R3D_GROUND_HALF_EXTENT, R3D_GROUND_CELLS);
    mesh_build_hills(&s_hills, &s_arena, scratch, scratch_cap, R3D_HILL_RADIUS);
    mesh_build_shadow_disc(&s_shadow, &s_arena, scratch, scratch_cap);
    mesh_build_props(s_props, &s_arena, scratch, scratch_cap);
    mesh_build_pickups(s_pickups, &s_arena, scratch, scratch_cap);
    mesh_build_weapons(s_weapons, &s_arena, scratch, scratch_cap);
    /* The 33 baked zombie pose meshes are only the fallback when no imported
     * zombie model could be loaded - skip them to save ~800 KB of arena when
     * the GPM skinning path is active. */
    if (!skip_zombie_poses)
        mesh_build_zombie_poses(s_zombie_poses, &s_arena, scratch, scratch_cap);
    mesh_build_hero_rig(&s_hero_builtin, &s_arena, scratch, scratch_cap);
    mesh_build_unit_cube(&s_unit_cube, &s_arena, scratch, scratch_cap);

    if (s_hero_rig == NULL)
        s_hero_rig = &s_hero_builtin;

    r3d_asset_log("props: timber=%u cart=%u ore=%u lantern=%u verts",
                  s_props[PROP_TIMBER].vertex_count,
                  s_props[PROP_CART].vertex_count,
                  s_props[PROP_ORE].vertex_count,
                  s_props[PROP_LANTERN].vertex_count);

    s_mesh_verts = s_ground.vertex_count + s_hills.vertex_count +
                   s_shadow.vertex_count + s_unit_cube.vertex_count;
    for (i = 0; i < PROP_KIND_COUNT; i++)
        s_mesh_verts += s_props[i].vertex_count;
    for (i = 0; i < DROP_COUNT; i++)
        s_mesh_verts += s_pickups[i].vertex_count;
    for (i = 0; i < WEAPON_COUNT; i++)
        s_mesh_verts += s_weapons[i].vertex_count;
    for (t = 0; t < ZOMBIE_COUNT; t++)
        for (p = 0; p < ZOMBIE_POSE_COUNT; p++)
            s_mesh_verts += s_zombie_poses[t][p].vertex_count;
    for (i = 0; i < s_hero_builtin.part_count; i++)
        s_mesh_verts += s_hero_builtin.part[i].mesh.vertex_count;

    s_mesh_bytes = (unsigned)s_arena.used;
}

void r3d_init(void)
{
    MeshVertex *scratch;
    int i;

    if (s_initialized)
        return;

    {
        int slot;
        for (slot = 0; slot < R3D_MODEL_COUNT; slot++)
            snprintf(s_model_source_text[slot], 64, "not loaded");
    }

    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    s_target_top = C3D_RenderTargetCreate(R3D_TOP_W, R3D_TOP_H,
                                          GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(s_target_top, GFX_TOP, GFX_LEFT,
                              DISPLAY_TRANSFER_FLAGS);
    s_target_bottom = C3D_RenderTargetCreate(R3D_BOT_W, R3D_BOT_H,
                                             GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(s_target_bottom, GFX_BOTTOM, GFX_LEFT,
                              DISPLAY_TRANSFER_FLAGS);

    /* shader */
    s_dvlb = DVLB_ParseFile((u32 *)vshader_shbin, vshader_shbin_size);
    if (s_dvlb == NULL)
        return;
    shaderProgramInit(&s_program);
    shaderProgramSetVsh(&s_program, &s_dvlb->DVLE[0]);

    s_u_projection = shaderInstanceGetUniformLocation(s_program.vertexShader, "projection");
    s_u_model_view = shaderInstanceGetUniformLocation(s_program.vertexShader, "modelView");
    s_u_tint = shaderInstanceGetUniformLocation(s_program.vertexShader, "tint");
    s_u_light_dir = shaderInstanceGetUniformLocation(s_program.vertexShader, "lightDir");
    s_u_light_color = shaderInstanceGetUniformLocation(s_program.vertexShader, "lightColor");
    s_u_ambient = shaderInstanceGetUniformLocation(s_program.vertexShader, "ambient");
    s_u_fill_color = shaderInstanceGetUniformLocation(s_program.vertexShader, "fillColor");
    s_u_fog_color = shaderInstanceGetUniformLocation(s_program.vertexShader, "fogColor");
    s_u_fog_params = shaderInstanceGetUniformLocation(s_program.vertexShader, "fogParams");

    AttrInfo_Init(&s_attr);
    AttrInfo_AddLoader(&s_attr, 0, GPU_FLOAT, 3);  /* position */
    AttrInfo_AddLoader(&s_attr, 1, GPU_FLOAT, 3);  /* normal   */
    AttrInfo_AddLoader(&s_attr, 2, GPU_FLOAT, 3);  /* albedo   */

    /* Imported models decide which procedural fallbacks (and how much arena)
     * are needed.  Loading them first lets the GPM path reserve a smaller mesh
     * arena, because the 33 baked zombie pose meshes are then unnecessary. */
    r3d_assets_init();

    /* meshes: one arena for all static geometry, one transient scratch */
    {
        unsigned arena_bytes = s_gpm_zombie_ok ? (1536u * 1024u)
                                              : R3D_MESH_ARENA_BYTES;
        s_arena_mem = linearAlloc(arena_bytes);
        if (s_arena_mem == NULL)
            return;
        mesh_arena_init(&s_arena, s_arena_mem, arena_bytes);
    }

    scratch = (MeshVertex *)linearAlloc((size_t)R3D_SCRATCH_VERTS * sizeof(MeshVertex));
    if (scratch == NULL)
        return;
    build_meshes(scratch, R3D_SCRATCH_VERTS, s_gpm_zombie_ok);
    if (s_gpm_hero_ok)
        r3d_build_wolf_attachments(scratch, R3D_SCRATCH_VERTS);

    linearFree(scratch);

    /* imported model runtime (indexed gun meshes + hero/zombie skin buffers) */
    (void)r3d_model_runtime_build();

    s_mesh_bytes = (unsigned)s_arena.used;

    s_dyn_mem = linearAlloc((size_t)R3D_DYN_VERTS * sizeof(MeshVertex));
    s_dyn_verts = (MeshVertex *)s_dyn_mem;

    Mtx_OrthoTilt(&s_ortho, 0.0f, 400.0f, 0.0f, 240.0f, 0.0f, 1.0f, true);

    for (i = 0; i < R3D_MAX_PUFFS; i++)
        s_puffs[i].active = false;
    s_title_clock = 0.0f;
    s_initialized = true;
}

void r3d_fini(void)
{
    if (!s_initialized)
        return;

    r3d_model_runtime_free();
    r3d_assets_free();

    if (s_dyn_mem != NULL)
        linearFree(s_dyn_mem);
    s_dyn_mem = NULL;
    s_dyn_verts = NULL;
    if (s_arena_mem != NULL)
        linearFree(s_arena_mem);
    s_arena_mem = NULL;
    memset(&s_arena, 0, sizeof(s_arena));

    if (s_dvlb != NULL) {
        shaderProgramFree(&s_program);
        DVLB_Free(s_dvlb);
        s_dvlb = NULL;
    }

    if (s_target_top_right != NULL) {
        C3D_RenderTargetDelete(s_target_top_right);
        s_target_top_right = NULL;
    }
    if (s_target_top != NULL) {
        C3D_RenderTargetDelete(s_target_top);
        s_target_top = NULL;
    }
    if (s_target_bottom != NULL) {
        C3D_RenderTargetDelete(s_target_bottom);
        s_target_bottom = NULL;
    }

    if (s_display_3d) {
        gfxSet3D(false);
        s_display_3d = false;
    }

    C3D_Fini();
    gfxExit();
    s_initialized = false;
}

C3D_RenderTarget *r3d_top_target(void)
{
    return s_target_top;
}

C3D_RenderTarget *r3d_bottom_target(void)
{
    return s_target_bottom;
}

/* ------------------------------------------------------------------ */
/* frame bracket                                                      */
/* ------------------------------------------------------------------ */

static void stereo_sync(void)
{
    bool want = s_stereo_enabled && s_stereo_iod > 0.005f;

    if (want && s_target_top_right == NULL) {
        s_target_top_right = C3D_RenderTargetCreate(R3D_TOP_W, R3D_TOP_H,
                                                    GPU_RB_RGBA8,
                                                    GPU_RB_DEPTH24_STENCIL8);
        if (s_target_top_right != NULL)
            C3D_RenderTargetSetOutput(s_target_top_right, GFX_TOP, GFX_RIGHT,
                                      DISPLAY_TRANSFER_FLAGS);
        else
            want = false;
    }
    if (want != s_display_3d) {
        gfxSet3D(want);
        s_display_3d = want;
    }
}

void r3d_begin_frame(void)
{
    if (!s_initialized)
        return;
    stereo_sync();
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
}

void r3d_end_frame(void)
{
    if (!s_initialized)
        return;
    C3D_FrameEnd(0);
}

void r3d_set_stereo(float iod, bool enabled)
{
    s_stereo_enabled = enabled;
    s_stereo_iod = iod;
    if (s_stereo_iod < 0.0f)
        s_stereo_iod = -s_stereo_iod;
}

/* ------------------------------------------------------------------ */
/* r3d_draw_world                                                     */
/* ------------------------------------------------------------------ */

static void build_projection(C3D_Mtx *projection, float fov_deg, float iod)
{
    float fovy = C3D_AngleFromDegrees(fov_deg);

    if (iod > 0.0001f || iod < -0.0001f)
        Mtx_PerspStereoTilt(projection, fovy, C3D_AspectRatioTop,
                            R3D_NEAR, R3D_FAR, iod, 2.0f, false);
    else
        Mtx_PerspTilt(projection, fovy, C3D_AspectRatioTop,
                      R3D_NEAR, R3D_FAR, false);
}

void r3d_draw_world(const GraveGame *game, float dt)
{
    Vec3 cam_pos, cam_look, fwd, right, up;
    C3D_Mtx view, projection;
    bool first_person;
    float fov;
    bool title;
    Vec3 title_pos, title_look;

    if (!s_initialized || s_target_top == NULL)
        return;

    if (dt < 0.0f)
        dt = 0.0f;
    if (dt > 0.1f)
        dt = 0.1f;

    s_draw_calls = 0;
    s_draw_verts = 0;
    s_skin_verts_frame = 0;
    s_skin_bones_frame = 0;
    s_skin_instances_frame = 0;
    s_draw_dt = dt;
    s_boss_head_valid = false;
    boss_effects_update(game, dt);

    title = game != NULL && game->phase == GAME_PHASE_TITLE;
    if (title)
        s_title_clock += dt;
    r3d_zone_select(title || game == NULL ? ZONE_ASHEN_MILL : game->zone);

    /* camera */
    if (title) {
        float a = 0.34f + 0.10f * sinf(s_title_clock * 0.085f);
        float radius = 4.5f;
        title_pos = v3(sinf(a) * radius - 0.60f,
                       1.42f + 0.05f * sinf(s_title_clock * 0.13f),
                       cosf(a) * radius);
        title_look = v3(0.30f, 1.12f, 0.30f);
        cam_pos = title_pos;
        cam_look = title_look;
        fov = 52.0f;
    } else {
        camera_from_game(game, &cam_pos, &cam_look, &fov);
    }

    build_view(&cam_pos, &cam_look, &view);
    camera_basis(&cam_pos, &cam_look, &fwd, &right, &up);
    s_elapsed += dt;
    dust_update(dt, cam_pos);

    first_person = !title && game != NULL && game->player.camera_first_person;

    /* smoke uses the previous frame's muzzle for spawning; update once */
    {
        Vec3 muzzle = s_have_last_muzzle ? s_last_muzzle
                                         : v3_add(cam_pos, v3_scale(fwd, 0.5f));
        update_smoke(game, dt, muzzle, s_have_last_muzzle);
    }

    if (s_display_3d && s_target_top_right != NULL && !title) {
        build_projection(&projection, fov, -s_stereo_iod);
        draw_eye(game, &projection, &view, &cam_pos, &fwd, &right, &up,
                 first_person, first_person, s_target_top);
        build_projection(&projection, fov, s_stereo_iod);
        draw_eye(game, &projection, &view, &cam_pos, &fwd, &right, &up,
                 first_person, first_person, s_target_top_right);
    } else {
        build_projection(&projection, fov, 0.0f);
        if (title) {
            C3D_RenderTargetClear(s_target_top, C3D_CLEAR_ALL, r3d_palette()->clear_color, 0);
            C3D_FrameDrawOn(s_target_top);
            bind_pass_state(&projection, &view);
            draw_title_diorama(game, &cam_pos, &fwd, &right, &up, &view);
        } else {
            draw_eye(game, &projection, &view, &cam_pos, &fwd, &right, &up,
                     first_person, first_person, s_target_top);
        }
    }

    /* Leave the GPU in a neutral state for citro2d: it does not reset the
     * cull face itself, so any 3D backface culling left on would make every
     * HUD triangle vanish. */
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
}

/* ------------------------------------------------------------------ */
/* debug helpers                                                      */
/* ------------------------------------------------------------------ */

void r3d_frame_stats(unsigned *draw_calls, unsigned *vertex_count)
{
    if (draw_calls != NULL)
        *draw_calls = s_draw_calls;
    if (vertex_count != NULL)
        *vertex_count = s_draw_verts;
}

unsigned r3d_mesh_bytes(void)
{
    return s_mesh_bytes;
}

unsigned r3d_mesh_vertices(void)
{
    return s_mesh_verts;
}

void r3d_frame_stats_ext(R3dFrameStats *out)
{
    if (out == NULL)
        return;
    out->draw_calls = s_draw_calls;
    out->draw_verts = s_draw_verts;
    out->skinned_verts = s_skin_verts_frame;
    out->skinned_bones = s_skin_bones_frame;
    out->skinned_instances = s_skin_instances_frame;
    out->mesh_arena_bytes = s_mesh_bytes;
    out->dynamic_bytes = s_dynamic_bytes;
}

void r3d_set_hero_rig(const struct HeroRig *rig)
{
    s_hero_rig = (rig == NULL) ? &s_hero_builtin : rig;
    s_hero_rig_override = (rig != NULL);
}

/* ------------------------------------------------------------------ */
/* imported-model hot swap (v0.2)                                     */
/* ------------------------------------------------------------------ */

static const GpmModel *s_hero_model_borrowed;

bool r3d_set_hero_model(const struct GpmModel *model)
{
    if (!s_initialized)
        return false;
    if (model == NULL)
        return r3d_reload_hero_model();
    if (!gpm_is_skinned(model) || model->vertex_count == 0 ||
        model->vertex_count > R3D_MAX_MODEL_VERTS)
        return false;
    if (!r3d_hero_buffers_alloc(model)) {
        s_gpm_hero_ready = false;
        return false;
    }
    s_hero_model_borrowed = model;
    s_src_hero = R3D_SRC_CUSTOM;
    r3d_hero_configure(model);
    s_gpm_hero_ready = true;
    r3d_update_model_sources();
    r3d_asset_log("hero: hot-swapped to caller model (%u verts)",
                  model->vertex_count);
    return true;
}

bool r3d_reload_hero_model(void)
{
    R3dEmbeddedModel emb;

    if (!s_initialized)
        return false;
    s_hero_model_borrowed = NULL;
    s_gpm_hero_ready = false;
    gpm_destroy(&s_gpm_hero);
    s_gpm_hero_ok = false;
    emb = r3d_hero_embedded_model();
    if (!r3d_load_one_model(&s_gpm_hero, &s_gpm_hero_ok, "hero", &emb, true,
                            &s_src_hero)) {
        r3d_update_model_sources();
        return false;
    }
    if (!r3d_hero_buffers_alloc(&s_gpm_hero)) {
        s_gpm_hero_ready = false;
        r3d_update_model_sources();
        return false;
    }
    r3d_hero_configure(&s_gpm_hero);
    s_gpm_hero_ready = true;
    r3d_dynamic_bytes_update();
    r3d_update_model_sources();
    return true;
}

const struct GpmModel *r3d_hero_model(void)
{
    if (!s_gpm_hero_ready)
        return NULL;
    if (s_hero_model_borrowed != NULL)
        return s_hero_model_borrowed;
    return s_hero_skin.model;
}
bool r3d_reload_all_models(void)
{
    MeshVertex *scratch;

    if (!s_initialized)
        return false;

    s_hero_model_borrowed = NULL;
    r3d_model_runtime_free();
    r3d_assets_free();
    r3d_assets_init();

    /* The procedural wolf head/tail are only built when an imported hero is
     * available; build them on demand if the reload just enabled the GPM path. */
    if (s_gpm_hero_ok && s_wolf_head.vbo == NULL) {
        scratch = (MeshVertex *)linearAlloc((size_t)R3D_SCRATCH_VERTS *
                                            sizeof(MeshVertex));
        if (scratch != NULL) {
            r3d_build_wolf_attachments(scratch, R3D_SCRATCH_VERTS);
            linearFree(scratch);
        } else {
            r3d_asset_log("attach: scratch alloc failed on reload");
        }
    }

    return r3d_model_runtime_build();
}

const char *r3d_model_source(R3dModelSlot slot)
{
    if ((int)slot < 0 || (int)slot >= R3D_MODEL_COUNT)
        return "not loaded";
    if (s_model_source_text[slot][0] == '\0')
        return "not loaded";
    return s_model_source_text[slot];
}

/* ------------------------------------------------------------------ */
/* single box helper (contract: debug / skeleton views)               */
/* ------------------------------------------------------------------ */

void r3d_draw_box(const C3D_Mtx *view, float x, float y, float z,
                  float sx, float sy, float sz,
                  float r, float g, float b)
{
    C3D_Mtx model;

    if (!s_initialized || view == NULL || s_unit_cube.vbo == NULL)
        return;

    bind_pass_state(&s_last_projection, view);

    Mtx_Identity(&model);
    Mtx_Translate(&model, x, y, z, true);
    Mtx_Scale(&model, sx, sy, sz);
    {
        C3D_Mtx mv;
        Mtx_Multiply(&mv, view, &model);
        draw_mesh_mv(&mv, &s_unit_cube, r, g, b, 0.0f);
    }
}
