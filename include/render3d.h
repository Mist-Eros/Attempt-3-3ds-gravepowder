/*
 * Gravepowder - 3D front end contract
 *
 * source/render3d.c owns the citro3d side of the 3DS build: the GPU surface
 * (two render targets, top 3D + bottom 2D), the shader programs, the meshes and
 * all the world drawing.  main.c / app.c only call into this header.
 *
 * The renderer never mutates the game state - it reads GraveGame (including the
 * camera output fields filled by grave_update_camera) and draws it.
 */
#ifndef GRAVEPOWDER_RENDER3D_H
#define GRAVEPOWDER_RENDER3D_H

#include <citro3d.h>

#include "grave.h"

/* Initialises gfx + C3D and creates both render targets. */
void r3d_init(void);
void r3d_fini(void);

/* Render targets owned by the 3D layer (the HUD draws into the bottom one). */
C3D_RenderTarget *r3d_top_target(void);
C3D_RenderTarget *r3d_bottom_target(void);

/* Frame bracket: C3D_FrameBegin / C3D_FrameEnd. */
void r3d_begin_frame(void);
void r3d_end_frame(void);

/* Draws the 3D world into the top target (clears it first).  dt lets the
 * renderer animate particle effects that are not core state. */
void r3d_draw_world(const GraveGame *game, float dt);

/* Draws a single coloured box (used by the skeleton / debug views). */
void r3d_draw_box(const C3D_Mtx *view, float x, float y, float z,
                  float sx, float sy, float sz,
                  float r, float g, float b);

/* Optional: stereoscopic 3D support (slider).  When valid_iod is false the
 * renderer draws a normal mono frame. */
void r3d_set_stereo(float iod, bool enabled);

/* ------------------------------------------------------------------ */
/* addenda (shooter-render, additive only)                            */
/* ------------------------------------------------------------------ */

/* Replaces the built-in procedural wolf with a user-supplied rig (see
 * source/mesh.h for the HeroRig format: part meshes + parents + joint pivots
 * + animation channels).  Pass NULL to restore the built-in hero.  The rig is
 * borrowed, not copied: keep it alive for as long as it is installed. */
struct HeroRig;
void r3d_set_hero_rig(const struct HeroRig *rig);

/* Debug/telemetry helpers: stats for the last r3d_draw_world() call and the
 * total size of the baked mesh data. */
void r3d_frame_stats(unsigned *draw_calls, unsigned *vertex_count);
unsigned r3d_mesh_bytes(void);
unsigned r3d_mesh_vertices(void);

/* Extended per-frame stats (v0.2): skinned vertex/bone counts and memory use.
 * Filled after r3d_draw_world(); all counts are per frame. */
typedef struct {
    unsigned draw_calls;        /* draw calls issued this frame               */
    unsigned draw_verts;        /* vertices submitted (all paths)             */
    unsigned skinned_verts;     /* vertices transformed by the CPU skinner    */
    unsigned skinned_bones;     /* bone poses evaluated for skinned instances */
    unsigned skinned_instances; /* instances skinned this frame               */
    unsigned mesh_arena_bytes;  /* static mesh arena in use                   */
    unsigned dynamic_bytes;     /* skinned dynamic VBO/IBO memory (linearAlloc) */
} R3dFrameStats;

void r3d_frame_stats_ext(R3dFrameStats *out);

/* ------------------------------------------------------------------ */
/* imported model swapping (GPM v1, additive)                          */
/* ------------------------------------------------------------------ */

/* Imported models (see docs/assets.md) are loaded at boot: SD-card override
 * first (sdmc:/3ds/Gravepowder/models/<name>.gpm), then the embedded copy,
 * then the procedural fallback.  These helpers let a menu hot-swap the hero
 * without a rebuild.
 *
 * r3d_set_hero_model(): install a caller-owned GpmModel (borrowed: keep it
 * alive).  Returns false if it is not skinnable or too large for the fixed
 * runtime vertex buffers.  Passing NULL calls r3d_reload_hero_model().
 * r3d_reload_hero_model(): re-read the SD override / embedded hero model.
 * r3d_hero_model(): the model currently in use, or NULL for the procedural
 * fallback.  Include "gpm.h" for the GpmModel layout. */
struct GpmModel;
bool r3d_set_hero_model(const struct GpmModel *model);
bool r3d_reload_hero_model(void);
const struct GpmModel *r3d_hero_model(void);

#endif /* GRAVEPOWDER_RENDER3D_H */
