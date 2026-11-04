/*
 * Gravepowder - CPU skinning runtime for GPM models
 *
 * Pure C99 (no 3DS headers) so it can run in host unit tests.  Rigid skinning:
 * every vertex follows its dominant bone, which keeps the per-vertex cost to
 * one 4x4 transform + one 3x3 normal transform, exactly what the 3DS CPU likes.
 *
 * Per instance:
 *   - sample a clip (or two, crossfading) into local bone TRS
 *   - build model-space bone world matrices (parent-before-child)
 *   - pose matrix per bone = world * inverse_bind
 *   - skin the model's vertices into a caller-provided SkinVertex array
 *     (the renderer copies this straight into a dynamic VBO)
 *
 * Everything uses fixed-size buffers: no allocation after skin_init().
 */
#ifndef GRAVEPOWDER_SKIN_H
#define GRAVEPOWDER_SKIN_H

#include <stdbool.h>
#include <stdint.h>

#include "gpm.h"

#define SKIN_MAX_BONES GPM_MAX_BONES

/* Output vertex layout: identical to the renderer's MeshVertex (36 bytes),
 * kept separate so this module stays free of citro3d headers. */
typedef struct {
    float pos[3];
    float nrm[3];
    float col[3];
} SkinVertex;

typedef char skin_check_vertex[(sizeof(SkinVertex) == 36) ? 1 : -1];

/* Crossfade duration when the requested clip changes (seconds). */
#define SKIN_BLEND_TIME 0.15f

typedef struct {
    float translation[3];
    float rotation[4]; /* quaternion x,y,z,w */
    float scale[3];
} SkinTrs;

typedef struct {
    const GpmModel *model;

    int   clip_a;      /* current/fading-from clip index (-1 = rest pose)    */
    float time_a;      /* playhead for clip_a                                */
    int   clip_b;      /* target clip index (-1 = none)                      */
    float time_b;      /* playhead for clip_b                                */
    float blend;       /* 0 = clip_a, 1 = clip_b                             */

    SkinTrs local_a[SKIN_MAX_BONES];
    SkinTrs local_b[SKIN_MAX_BONES];
    float   world[SKIN_MAX_BONES][16]; /* model-space bone matrices            */
    float   pose[SKIN_MAX_BONES][16];  /* skinning matrices = world * inv_bind */
} SkinInstance;

/* Binds an instance to a model and resets it to the rest pose. */
void skin_init(SkinInstance *inst, const GpmModel *model);

/* Requests a clip (+ playhead time in seconds) for this frame.  Safe to call
 * every frame: changing the clip starts a SKIN_BLEND_TIME crossfade.  Use
 * clip = -1 for the rest pose. */
void skin_set_clip(SkinInstance *inst, int clip, float time);

/* Advances the crossfade and rebuilds all pose matrices.  dt in seconds. */
void skin_update(SkinInstance *inst, float dt);

/* Skins the whole model into `out` (capacity `max_verts`).  Returns the number
 * of vertices written (0 if the model is empty or the capacity is too small). */
int skin_vertices(const SkinInstance *inst, SkinVertex *out, int max_verts);

/* Model-space bone matrix (row-major 4x4) after the last skin_update, for
 * attaching procedural parts (wolf head/ears/tail, gun).  NULL when out of
 * range.  Static models return the identity for bone 0. */
const float *skin_bone_matrix(const SkinInstance *inst, int bone);

/* Bone index by name on the instance's model, or -1. */
int skin_bone_index(const SkinInstance *inst, const char *name);

/* True when the instance has a skeleton to animate (false: draw the static
 * model with the identity transform). */
bool skin_is_animated(const SkinInstance *inst);

#endif /* GRAVEPOWDER_SKIN_H */
