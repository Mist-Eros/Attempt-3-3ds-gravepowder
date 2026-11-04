/*
 * Gravepowder - procedural mesh builders and animation rig format
 *
 * Everything the 3D renderer draws is generated in C and uploaded once into
 * linearAlloc'd VBOs at boot: no textures, no external model files, no
 * per-frame allocation.  This file is the "art kit":
 *
 *   MeshArena    bump allocator over one linearAlloc block (GPU visible)
 *   MeshBuilder  writes vertices into a scratch array, then mesh_commit()
 *                copies them into the arena as one drawable Mesh
 *   primitives   flat-shaded boxes / cylinders / tapered cylinders / quads
 *   HeroRig      a small part skeleton used for the anthro wolf gunslinger;
 *                a replacement hero model can fill the same struct (meshes +
 *                parents + joint pivots + animation channels) and be handed to
 *                r3d_set_hero_rig() - see include/render3d.h.
 *   zombie poses zombie bodies are baked into a handful of static pose meshes
 *                (one draw call per zombie) instead of drawing 18 body parts
 *                per zombie every frame.
 *
 * Vertex layout (36 bytes, interleaved):
 *   v0 position (3 floats)
 *   v1 normal   (3 floats)
 *   v2 albedo   (3 floats)   multiplied by the per-draw tint uniform
 */
#ifndef GRAVEPOWDER_MESH_H
#define GRAVEPOWDER_MESH_H

#include <citro3d.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "grave.h"

/* ------------------------------------------------------------------ */
/* vertex format                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    float pos[3];
    float nrm[3];
    float col[3];
} MeshVertex;

#define MESH_VERTEX_STRIDE   ((int)sizeof(MeshVertex))
#define MESH_ATTR_COUNT      3
#define MESH_ATTR_PERMUTATION 0x210  /* v0,v1,v2 from one interleaved VBO */

/* ------------------------------------------------------------------ */
/* GPU meshes + allocation arena                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    void    *vbo;          /* linearAlloc'd vertex data (GPU visible)   */
    uint32_t vertex_count;
    Vec3     center;       /* local-space bounding-sphere centre        */
    float    radius;       /* local-space bounding-sphere radius        */
} Mesh;

typedef struct {
    uint8_t *base;
    size_t   size;
    size_t   used;
    bool     overflow;
} MeshArena;

/* Initialises an arena over caller-provided GPU memory. */
void mesh_arena_init(MeshArena *arena, void *mem, size_t size);

/* 16-byte aligned arena allocation; NULL when full. */
void *mesh_arena_alloc(MeshArena *arena, size_t size);

/* ------------------------------------------------------------------ */
/* mesh builder                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    MeshVertex *verts;
    int         count;
    int         capacity;
    bool        overflow;
} MeshBuilder;

/* Scratch must hold at least `capacity` vertices; the builder never
 * allocates.  Every build_* helper below appends to a builder. */
void mesh_builder_begin(MeshBuilder *b, MeshVertex *scratch, int capacity);

/* Appends one transformed vertex (public so custom models can be loaded). */
void mesh_builder_vertex(MeshBuilder *b, const C3D_Mtx *xf,
                         float x, float y, float z,
                         float nx, float ny, float nz,
                         float r, float g, float bl);

/* Axis-aligned-in-local-space box: centre (cx,cy,cz), half extents. */
void mesh_builder_box(MeshBuilder *b, const C3D_Mtx *xf,
                      float cx, float cy, float cz,
                      float hx, float hy, float hz,
                      float r, float g, float bl);

/* Cylinder along local +Y, centred at (cx,cy,cz). */
void mesh_builder_cyl(MeshBuilder *b, const C3D_Mtx *xf,
                      float cx, float cy, float cz,
                      float radius, float height, int sides, bool caps,
                      float r, float g, float bl);

/* Tapered cylinder (cone when top_radius == 0) along local +Y. */
void mesh_builder_cone(MeshBuilder *b, const C3D_Mtx *xf,
                       float cx, float cy, float cz,
                       float bottom_radius, float top_radius, float height,
                       int sides, bool caps,
                       float r, float g, float bl);

/* Flat quad in the local XY plane (normal +Z), centred at (cx,cy,cz). */
void mesh_builder_quad(MeshBuilder *b, const C3D_Mtx *xf,
                       float cx, float cy, float cz,
                       float half_w, float half_h,
                       float r, float g, float bl);

/* Copies the built vertices into arena memory and fills `out`.
 * Returns false (and leaves `out` empty) if the builder overflowed or the
 * arena could not satisfy the request. */
bool mesh_commit(MeshBuilder *b, Mesh *out, MeshArena *arena);

/* ------------------------------------------------------------------ */
/* hero rig (swappable)                                               */
/* ------------------------------------------------------------------ */

enum {
    RIG_CH_NONE = 0,
    RIG_CH_TORSO,
    RIG_CH_HEAD,
    RIG_CH_LEG_A,
    RIG_CH_LEG_B,
    RIG_CH_KNEE_A,
    RIG_CH_KNEE_B,
    RIG_CH_ARM_A,
    RIG_CH_ARM_B,
    RIG_CH_ELBOW_A,
    RIG_CH_ELBOW_B,
    RIG_CH_COAT_A,
    RIG_CH_COAT_B,
    RIG_CH_TAIL_A,
    RIG_CH_TAIL_B,
    RIG_CH_COUNT
};

/* One articulated part.  Geometry lives in joint-local space: the joint is
 * the origin, the mesh hangs/extends from it.  Parts must be ordered so a
 * parent (lower index, -1 = root) always appears before its children.
 * Animation applies, in order: translate(pivot) * rotateX * rotateY * rotateZ
 * where the euler angles come from the renderer's animation for `channel`. */
typedef struct {
    Mesh     mesh;
    int8_t   parent;
    uint8_t  channel;     /* RIG_CH_* */
    Vec3     pivot;       /* joint offset from the parent joint, metres */
} RigPart;

#define HERO_PART_COUNT 20

typedef struct HeroRig {
    RigPart part[HERO_PART_COUNT];
    int     part_count;
    int8_t  gun_hand;     /* part index the weapon attaches to (-1 none) */
    float   scale;        /* overall model scale (1.0 = 1.8 m tall wolf) */
    float   eye_height;   /* metres above the feet, for first person      */
} HeroRig;

/* Builds the built-in procedural wolf gunslinger rig. */
void mesh_build_hero_rig(HeroRig *rig, MeshArena *arena,
                         MeshVertex *scratch, int scratch_cap);

/* ------------------------------------------------------------------ */
/* baked zombie pose meshes                                           */
/* ------------------------------------------------------------------ */

#define ZOMBIE_WALK_FRAMES 8
#define ZOMBIE_POSE_LUNGE  8
#define ZOMBIE_POSE_STAGGER 9
#define ZOMBIE_POSE_DEATH  10
#define ZOMBIE_POSE_COUNT  11

/* out[type][pose]; one draw call per zombie at runtime. */
void mesh_build_zombie_poses(Mesh out[ZOMBIE_COUNT][ZOMBIE_POSE_COUNT],
                             MeshArena *arena,
                             MeshVertex *scratch, int scratch_cap);

/* ------------------------------------------------------------------ */
/* world art                                                          */
/* ------------------------------------------------------------------ */

/* Mottled dirt plane centred on the origin, y = 0. */
void mesh_build_ground(Mesh *out, MeshArena *arena,
                       MeshVertex *scratch, int scratch_cap,
                       float half_extent, int cells);

/* Ring of distant low-poly hills to hide the horizon (unlit-ish). */
void mesh_build_hills(Mesh *out, MeshArena *arena,
                      MeshVertex *scratch, int scratch_cap, float radius);

/* One mesh per PropKind, built to fill the Prop.half extents:
 * x,z in [-0.5,0.5], y in [0,1] so the renderer can scale by 2*half. */
void mesh_build_props(Mesh out[PROP_KIND_COUNT], MeshArena *arena,
                      MeshVertex *scratch, int scratch_cap);

/* One mesh per GraveDrop (coin / sinew / powder pouch). */
void mesh_build_pickups(Mesh out[DROP_COUNT], MeshArena *arena,
                        MeshVertex *scratch, int scratch_cap);

/* One mesh per WeaponId, grip at the origin, barrel pointing along +Z. */
void mesh_build_weapons(Mesh out[WEAPON_COUNT], MeshArena *arena,
                        MeshVertex *scratch, int scratch_cap);

/* Unit quad in the XY plane (normal +Z), 2 triangles. */
void mesh_build_quad(Mesh *out, MeshArena *arena,
                     MeshVertex *scratch, int scratch_cap);

/* Flat unit-radius disc at y = 0 (normal +Y); used as a blob shadow. */
void mesh_build_shadow_disc(Mesh *out, MeshArena *arena,
                            MeshVertex *scratch, int scratch_cap);

/* Unit cube (36 verts, flat normals), used by r3d_draw_box(). */
void mesh_build_unit_cube(Mesh *out, MeshArena *arena,
                          MeshVertex *scratch, int scratch_cap);

#endif /* GRAVEPOWDER_MESH_H */
