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

#endif /* GRAVEPOWDER_RENDER3D_H */
