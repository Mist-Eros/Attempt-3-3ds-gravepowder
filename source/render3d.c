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
#include <string.h>

#include "render3d.h"
#include "mesh.h"
#include "vshader_shbin.h"

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

#define R3D_MESH_ARENA_BYTES (2u * 1024u * 1024u)
#define R3D_SCRATCH_VERTS    16384
#define R3D_DYN_VERTS        2048
#define R3D_MAX_PUFFS        24
#define R3D_DUST_COUNT      16

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

static C3D_Mtx s_ortho;
static C3D_Mtx s_last_projection;

static bool s_stereo_enabled;
static float s_stereo_iod;

/* frame stats */
static unsigned s_draw_calls;
static unsigned s_draw_verts;
static unsigned s_mesh_verts;
static unsigned s_mesh_bytes;

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
    Vec3 light = v3_norm(v3(-0.42f, 0.86f, 0.30f));
    float lx = view->r[0].x * light.x + view->r[0].y * light.y + view->r[0].z * light.z;
    float ly = view->r[1].x * light.x + view->r[1].y * light.y + view->r[1].z * light.z;
    float lz = view->r[2].x * light.x + view->r[2].y * light.y + view->r[2].z * light.z;

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
                  R3D_LIGHT_R, R3D_LIGHT_G, R3D_LIGHT_B, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u_ambient,
                  R3D_AMBIENT_R, R3D_AMBIENT_G, R3D_AMBIENT_B, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u_fill_color,
                  R3D_FILL_R, R3D_FILL_G, R3D_FILL_B, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u_fog_color,
                  R3D_SKY_R, R3D_SKY_G, R3D_SKY_B, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_u_fog_params,
                  -R3D_FOG_NEAR * R3D_FOG_INV, -R3D_FOG_INV, 0.0f, 0.0f);
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
    if (depth > R3D_FOG_FAR + radius)
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
    case ZOMBIE_RUNNER: return 0.87f;
    case ZOMBIE_BRUTE:  return 1.26f;
    default:            return 0.93f;
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
                          Vec3 muzzle, bool muzzle_visible, float muzzle_flash)
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
        size = p->size + 0.20f * k;
        fade = (1.0f - k) * (1.0f - k) * 0.30f;
        dyn_quad(p->pos, *right, *up, size, size,
                 fade * 0.85f, fade * 0.82f, fade * 0.75f);
    }

    /* drifting dust */
    dust_build(right, up);

    /* muzzle flash: two crossed quads at the muzzle */
    if (muzzle_visible && muzzle_flash > 0.001f) {
        float f = clampf(muzzle_flash, 0.0f, 1.0f);
        float s1 = 0.055f + 0.13f * f;
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

    /* first person: viewmodel goes in before the world so it is never clipped
     * into a wall (depth is already correct from the near camera position) */
    if (first_person) {
        draw_first_person_gun(game, weapon);
    }

    /* distant hills: plain silhouettes, no culling assumptions */
    C3D_CullFace(GPU_CULL_NONE);
    draw_mesh_mv(view, &s_hills, 1.0f, 1.0f, 1.0f, 1.0f);
    C3D_CullFace(GPU_CULL_BACK_CCW);

    /* ground */
    draw_mesh_mv(view, &s_ground, 1.0f, 1.0f, 1.0f, 0.0f);

    /* blob shadows for props + characters */
    draw_shadows(game, view, first_person);

    /* props */
    draw_props(game, view);

    /* pickups */
    draw_pickups(game, view);

    /* weapon muzzle in the world, for flash + smoke */
    if (first_person) {
        muzzle = weapon_muzzle_world(weapon, cam_pos, fwd, right, up);
    } else {
        muzzle = v3_add(game->player.transform.pos, v3(0.0f, 1.2f, 0.0f));
        hero_draw(&game->player, view, game->zone_time, false, weapon, &muzzle);
    }

    /* zombies */
    draw_zombies(game, view);

    *muzzle_out = muzzle;
    *muzzle_valid = true;

    /* effects: tracers, sparks, smoke, muzzle flash */
    {
        float flash = clampf(game->player.muzzle_flash / R3D_MUZZLE_FLASH_TIME,
                             0.0f, 1.0f);
        effects_build(game, cam_pos, fwd, right, up, muzzle,
                      flash > 0.0f, flash);
    }
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

    C3D_RenderTargetClear(target, C3D_CLEAR_ALL, R3D_CLEAR_COLOR, 0);
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

    if (rig != NULL) {
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

static void build_meshes(MeshVertex *scratch, int scratch_cap)
{
    int i, t, p;

    mesh_build_ground(&s_ground, &s_arena, scratch, scratch_cap,
                      R3D_GROUND_HALF_EXTENT, R3D_GROUND_CELLS);
    mesh_build_hills(&s_hills, &s_arena, scratch, scratch_cap, R3D_HILL_RADIUS);
    mesh_build_shadow_disc(&s_shadow, &s_arena, scratch, scratch_cap);
    mesh_build_props(s_props, &s_arena, scratch, scratch_cap);
    mesh_build_pickups(s_pickups, &s_arena, scratch, scratch_cap);
    mesh_build_weapons(s_weapons, &s_arena, scratch, scratch_cap);
    mesh_build_zombie_poses(s_zombie_poses, &s_arena, scratch, scratch_cap);
    mesh_build_hero_rig(&s_hero_builtin, &s_arena, scratch, scratch_cap);
    mesh_build_unit_cube(&s_unit_cube, &s_arena, scratch, scratch_cap);

    s_hero_rig = &s_hero_builtin;

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

    /* meshes: one arena for all static geometry, one transient scratch */
    s_arena_mem = linearAlloc(R3D_MESH_ARENA_BYTES);
    if (s_arena_mem == NULL) {
        return;
    }
    mesh_arena_init(&s_arena, s_arena_mem, R3D_MESH_ARENA_BYTES);

    scratch = (MeshVertex *)linearAlloc((size_t)R3D_SCRATCH_VERTS * sizeof(MeshVertex));
    if (scratch == NULL)
        return;
    build_meshes(scratch, R3D_SCRATCH_VERTS);
    linearFree(scratch);

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

    if (s_dyn_mem != NULL)
        linearFree(s_dyn_mem);
    s_dyn_mem = NULL;
    s_dyn_verts = NULL;
    if (s_arena_mem != NULL)
        linearFree(s_arena_mem);
    s_arena_mem = NULL;

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

    title = game != NULL && game->phase == GAME_PHASE_TITLE;
    if (title)
        s_title_clock += dt;

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
            C3D_RenderTargetClear(s_target_top, C3D_CLEAR_ALL, R3D_CLEAR_COLOR, 0);
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

void r3d_set_hero_rig(const struct HeroRig *rig)
{
    s_hero_rig = (rig == NULL) ? &s_hero_builtin : rig;
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
