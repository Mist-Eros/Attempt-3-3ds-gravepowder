/*
 * Gravepowder - GPM v1 model loader ("Gravepowder Model")
 *
 * Pure C99, no 3DS headers, so the same code runs on the console and in host
 * unit tests.  The binary format is frozen in docs/assets.md; this file is the
 * reference reader (tools/import_model.py is the reference writer).
 *
 * A GPM file is one self-contained model: up to N rigid-skinned vertices
 * (dominant bone per vertex, PS1-style), bones with local rest TRS, and
 * standard-role animation clips resampled by the importer.
 *
 * Loading:
 *   - embedded in the .3dsx:  gpm_parse_memory(&model, hero_gpm, hero_gpm_size)
 *   - SD card override:       gpm_load_file(&model, "sdmc:/3ds/Gravepowder/models/hero.gpm")
 * Both validate the whole file up front (magic/version/section bounds/indices/
 * names) and fail with a human-readable string instead of crashing.
 *
 * Runtime animation/skinning lives in source/skin.c (include/skin.h).
 */
#ifndef GRAVEPOWDER_GPM_H
#define GRAVEPOWDER_GPM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "grave.h"

#define GPM_MAGIC   0x31504D47u /* "GPM1" */
#define GPM_VERSION 1u

#define GPM_FLAG_SKINNED   (1u << 0)
#define GPM_FLAG_ANIMATED  (1u << 1)

/* Fixed runtime limits.  The importer/spec has no hard caps, but the console
 * renderer uses fixed pools: a model beyond these is rejected cleanly (the
 * procedural fallback stays active).  Real assets: hero 24 bones, zombie 49. */
#define GPM_MAX_BONES  96
#define GPM_MAX_ANIMS  32
#define GPM_MAX_TRACKS 1536

typedef struct {
    float   pos[3];    /* rest-pose local position                       */
    float   nrm[3];    /* rest-pose local normal                         */
    uint8_t col[4];    /* RGBA albedo baked from source material/texture */
    uint16_t bone;     /* dominant bone index (0 for static models)      */
    uint16_t pad;
} GpmVertex;           /* 32 bytes */

typedef struct {
    int16_t  parent;   /* -1 = root, parent-before-child ordering */
    uint16_t pad;
    float    translation[3];
    float    rotation[4]; /* quaternion x,y,z,w */
    float    scale[3];
    uint32_t name_off;
} GpmBone;             /* 48 bytes */

typedef struct {
    uint32_t name_off;   /* standard clip role name: idle/walk/run/... */
    float    duration;   /* seconds */
    uint32_t track_first;
    uint32_t track_count;
} GpmAnim;             /* 16 bytes */

typedef struct {
    uint16_t bone;
    uint8_t  component;  /* 0 = translation, 1 = rotation (quant), 2 = scale */
    uint8_t  pad;
    uint16_t key_count;
    uint16_t pad2;
} GpmTrack;            /* 8 bytes */

typedef struct {
    float time;          /* seconds */
    float value[4];      /* quaternion for rotation, xyz for T/S */
} GpmKey;              /* 20 bytes */

/* Kept in sync with the file layout at compile time (see gpm.c). */
typedef struct GpmModel {
    const uint8_t *data;   /* backing storage (owned or borrowed)         */
    size_t         data_size;
    bool           owns_data;   /* gpm_destroy() frees it                  */

    uint32_t flags;
    uint32_t vertex_count;
    const GpmVertex *vertices;
    uint32_t index_count;
    const uint16_t  *indices;
    uint32_t bone_count;
    const GpmBone   *bones;
    uint32_t anim_count;
    const GpmAnim   *anims;
    uint32_t track_count;
    const GpmTrack  *tracks;
    uint32_t key_count;
    const GpmKey    *keys;
    const char      *strings;  /* NUL-separated name table (borrowed)         */
    Vec3     bbox_min;
    Vec3     bbox_max;

    /* derived rig data (malloc'd at load, NULL for static models).
     * Row-major 4x4 matrices, column-vector convention: v' = M*v. */
    float    *rest_world;      /* bone_count * 16                          */
    float    *inv_bind;        /* bone_count * 16                          */
    uint32_t *track_key_first; /* track_count entries into `keys`          */
} GpmModel;

/* ---------------------------------------------------------------- */
/* loading                                                          */
/* ---------------------------------------------------------------- */

/* Parses `size` bytes at `data`.  On success the model references the buffer
 * (if it is not 4-byte aligned a private aligned copy is made).  Returns NULL
 * on success, or a static human-readable error string on failure.  On failure
 * the model is left zeroed and needs no destroy. */
const char *gpm_parse_memory(GpmModel *model, const void *data, size_t size);

/* Reads `path` and parses it.  The file buffer is malloc'd and owned by the
 * model.  Missing/broken files return an error string (caller falls back). */
const char *gpm_load_file(GpmModel *model, const char *path);

/* Frees owned buffers and derived rig arrays.  Safe on zeroed/partial models. */
void gpm_destroy(GpmModel *model);

/* ---------------------------------------------------------------- */
/* queries                                                          */
/* ---------------------------------------------------------------- */

/* NUL-terminated string at `off` inside the model's string table, or NULL. */
const char *gpm_name(const GpmModel *model, uint32_t off);

/* Bone index by exact name, or -1. */
int gpm_find_bone(const GpmModel *model, const char *name);

/* Clip index by exact standard-role name ("idle"/"walk"/...), or -1. */
int gpm_find_anim(const GpmModel *model, const char *role);

/* Returns the first role present from `roles[]` (in priority order) or -1.
 * This is how the renderer handles models that only supply a subset of clips
 * (e.g. shoot -> idle, run -> walk, hit -> idle). */
int gpm_find_anim_chain(const GpmModel *model, const char *const *roles, int role_count);

const char *gpm_anim_name(const GpmModel *model, int anim_index);
float gpm_anim_duration(const GpmModel *model, int anim_index);

/* True when the model has a usable skeleton (bones + skinned flag). */
bool gpm_is_skinned(const GpmModel *model);

#endif /* GRAVEPOWDER_GPM_H */
