/*
 * Gravepowder - GPM v1 model loader (reference reader for docs/assets.md)
 *
 * Pure C99: no 3DS headers so it can be unit-tested on the host.
 * Every offset/count from the file is bounds-checked before use; a corrupt or
 * truncated file returns an error string and leaves the model zeroed, which
 * makes the renderer fall back to its procedural meshes.
 *
 * Layout note: the header carries counts for vertices/indices/bones/anims but
 * not for tracks/keys (the section table only gives offsets).  We derive the
 * track count from the animation records (max track_first+track_count) and the
 * key layout from the track key counts (keys are stored sequentially in track
 * order, as documented in docs/assets.md).  Both derivations are validated
 * against the section offsets/size, so a malformed file fails cleanly.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpm.h"

/* ------------------------------------------------------------------ */
/* layout checks (keep in sync with the on-disk structs)              */
/* ------------------------------------------------------------------ */

typedef char gpm_check_vertex[(sizeof(GpmVertex) == 32) ? 1 : -1];
typedef char gpm_check_bone[(sizeof(GpmBone) == 48) ? 1 : -1];
typedef char gpm_check_anim[(sizeof(GpmAnim) == 16) ? 1 : -1];
typedef char gpm_check_track[(sizeof(GpmTrack) == 8) ? 1 : -1];
typedef char gpm_check_key[(sizeof(GpmKey) == 20) ? 1 : -1];

/* 64-byte file header (see docs/assets.md) */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t bone_count;
    uint32_t anim_count;
    float    bbox_min[3];
    float    bbox_max[3];
    uint32_t off_strings;
    uint32_t off_sections;
    uint32_t file_size;
} GpmHeader;

typedef char gpm_check_header[(sizeof(GpmHeader) == 64) ? 1 : -1];

/* ------------------------------------------------------------------ */
/* small matrix helpers (row-major, column-vector: v' = M*v)          */
/* ------------------------------------------------------------------ */

static void mat4_identity(float m[16])
{
    int i;
    for (i = 0; i < 16; i++)
        m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_mul(float out[16], const float a[16], const float b[16])
{
    float t[16];
    int r, c, k;
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            float sum = 0.0f;
            for (k = 0; k < 4; k++)
                sum += a[r * 4 + k] * b[k * 4 + c];
            t[r * 4 + c] = sum;
        }
    }
    memcpy(out, t, sizeof(t));
}

/* local rest matrix = T * R * S */
static void mat4_from_trs(float m[16], const float t[3], const float q[4],
                          const float s[3])
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float len = sqrtf(x * x + y * y + z * z + w * w);
    float xx, yy, zz, xy, xz, yz, wx, wy, wz;

    if (len < 1e-8f) {
        x = 0.0f; y = 0.0f; z = 0.0f; w = 1.0f;
    } else {
        len = 1.0f / len;
        x *= len; y *= len; z *= len; w *= len;
    }

    xx = x * x; yy = y * y; zz = z * z;
    xy = x * y; xz = x * z; yz = y * z;
    wx = w * x; wy = w * y; wz = w * z;

    mat4_identity(m);
    m[0] = (1.0f - 2.0f * (yy + zz)) * s[0];
    m[1] = (2.0f * (xy - wz)) * s[1];
    m[2] = (2.0f * (xz + wy)) * s[2];
    m[3] = t[0];

    m[4] = (2.0f * (xy + wz)) * s[0];
    m[5] = (1.0f - 2.0f * (xx + zz)) * s[1];
    m[6] = (2.0f * (yz - wx)) * s[2];
    m[7] = t[1];

    m[8]  = (2.0f * (xz - wy)) * s[0];
    m[9]  = (2.0f * (yz + wx)) * s[1];
    m[10] = (1.0f - 2.0f * (xx + yy)) * s[2];
    m[11] = t[2];

    m[12] = 0.0f;
    m[13] = 0.0f;
    m[14] = 0.0f;
    m[15] = 1.0f;
}

/* General 4x4 inverse via Gauss-Jordan with partial pivoting (load time only,
 * used to build inverse-bind matrices). */
static bool mat4_invert(float out[16], const float in[16])
{
    float a[4][8];
    int r, c, i;

    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++)
            a[r][c] = in[r * 4 + c];
        for (c = 0; c < 4; c++)
            a[r][4 + c] = (r == c) ? 1.0f : 0.0f;
    }

    for (c = 0; c < 4; c++) {
        int pivot = c;
        float best = fabsf(a[c][c]);
        for (r = c + 1; r < 4; r++) {
            float v = fabsf(a[r][c]);
            if (v > best) {
                best = v;
                pivot = r;
            }
        }
        if (best < 1e-12f)
            return false;
        if (pivot != c) {
            for (i = 0; i < 8; i++) {
                float tmp = a[c][i];
                a[c][i] = a[pivot][i];
                a[pivot][i] = tmp;
            }
        }
        {
            float inv = 1.0f / a[c][c];
            for (i = 0; i < 8; i++)
                a[c][i] *= inv;
        }
        for (r = 0; r < 4; r++) {
            float f;
            if (r == c)
                continue;
            f = a[r][c];
            if (f == 0.0f)
                continue;
            for (i = 0; i < 8; i++)
                a[r][i] -= f * a[c][i];
        }
    }

    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++)
            out[r * 4 + c] = a[r][4 + c];
    return true;
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static bool aligned4(const void *p)
{
    return ((uintptr_t)p & 3u) == 0u;
}

static uint32_t rd_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static bool string_in_table(const uint8_t *buf, size_t size,
                            uint32_t off_strings, uint32_t name_off)
{
    uint32_t abs_off;
    if (off_strings >= size)
        return false;
    if (name_off >= size - off_strings)
        return false;
    abs_off = off_strings + name_off;
    return memchr(buf + abs_off, 0, size - abs_off) != NULL;
}

/* ------------------------------------------------------------------ */
/* rest-pose / inverse-bind construction                              */
/* ------------------------------------------------------------------ */

static bool build_rest_matrices(GpmModel *model)
{
    uint32_t i;
    float local[16];

    model->rest_world = (float *)malloc((size_t)model->bone_count * 16u * sizeof(float));
    model->inv_bind = (float *)malloc((size_t)model->bone_count * 16u * sizeof(float));
    if (model->rest_world == NULL || model->inv_bind == NULL)
        return false;

    for (i = 0; i < model->bone_count; i++) {
        const GpmBone *b = &model->bones[i];
        float *world = &model->rest_world[i * 16];

        mat4_from_trs(local, b->translation, b->rotation, b->scale);
        if (b->parent < 0) {
            memcpy(world, local, sizeof(local));
        } else {
            const float *pw = &model->rest_world[(uint32_t)b->parent * 16u];
            mat4_mul(world, pw, local);
        }
        if (!mat4_invert(&model->inv_bind[i * 16], world))
            return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* destroy / parse                                                    */
/* ------------------------------------------------------------------ */

void gpm_destroy(GpmModel *model)
{
    if (model == NULL)
        return;
    free(model->rest_world);
    free(model->inv_bind);
    free(model->track_key_first);
    if (model->owns_data)
        free((void *)model->data);
    memset(model, 0, sizeof(*model));
}

const char *gpm_parse_memory(GpmModel *model, const void *data, size_t size)
{
    GpmHeader h;
    uint8_t *owned = NULL;
    const uint8_t *buf;
    uint32_t off[6];
    uint32_t i;
    uint32_t track_count;
    uint32_t key_count;
    uint64_t key_sum = 0;
    const char *err = NULL;

    if (model == NULL)
        return "null model";
    memset(model, 0, sizeof(*model));

    if (data == NULL || size < sizeof(GpmHeader))
        return "file too small";

    /* The on-disk structs are naturally aligned; guarantee that for the typed
     * array pointers below by copying a misaligned buffer once at load. */
    if (!aligned4(data)) {
        owned = (uint8_t *)malloc(size);
        if (owned == NULL)
            return "out of memory (align copy)";
        memcpy(owned, data, size);
        buf = owned;
    } else {
        buf = (const uint8_t *)data;
    }
    model->data = buf;
    model->data_size = size;
    model->owns_data = (owned != NULL);

    memcpy(&h, buf, sizeof(h));
    if (h.magic != GPM_MAGIC) { err = "not a GPM file (bad magic)"; goto fail; }
    if (h.version != GPM_VERSION) { err = "unsupported GPM version"; goto fail; }
    if (h.file_size != size) { err = "GPM file_size mismatch (truncated?)"; goto fail; }
    if (h.vertex_count == 0u || h.index_count == 0u || (h.index_count % 3u) != 0u) {
        err = "bad vertex/index counts";
        goto fail;
    }
    if (h.bone_count > GPM_MAX_BONES || h.anim_count > GPM_MAX_ANIMS ||
        h.vertex_count > 65535u) {
        err = "model exceeds runtime limits (bones/anims/verts)";
        goto fail;
    }
    if (h.off_sections + 24u > size || h.off_sections + 24u < h.off_sections ||
        h.off_strings >= size) {
        err = "bad section/string table offset";
        goto fail;
    }

    for (i = 0; i < 6; i++)
        off[i] = rd_u32(buf + h.off_sections + i * 4u);

    if ((uint64_t)off[0] + (uint64_t)h.vertex_count * sizeof(GpmVertex) > size ||
        (uint64_t)off[1] + (uint64_t)h.index_count * 2u > size ||
        (uint64_t)off[2] + (uint64_t)h.bone_count * sizeof(GpmBone) > size ||
        (uint64_t)off[3] + (uint64_t)h.anim_count * sizeof(GpmAnim) > size ||
        off[4] > size || off[5] > size) {
        err = "section out of file bounds";
        goto fail;
    }
    if (!aligned4(buf + off[0]) || !aligned4(buf + off[2]) ||
        !aligned4(buf + off[3]) || !aligned4(buf + off[4]) ||
        !aligned4(buf + off[5]) || (off[1] & 1u) != 0u) {
        err = "misaligned GPM section";
        goto fail;
    }

    model->flags = h.flags;
    model->vertex_count = h.vertex_count;
    model->vertices = (const GpmVertex *)(const void *)(buf + off[0]);
    model->index_count = h.index_count;
    model->indices = (const uint16_t *)(const void *)(buf + off[1]);
    model->bone_count = h.bone_count;
    model->bones = (const GpmBone *)(const void *)(buf + off[2]);
    model->anim_count = h.anim_count;
    model->anims = (const GpmAnim *)(const void *)(buf + off[3]);
    model->bbox_min = vec3_make(h.bbox_min[0], h.bbox_min[1], h.bbox_min[2]);
    model->bbox_max = vec3_make(h.bbox_max[0], h.bbox_max[1], h.bbox_max[2]);
    model->strings = (const char *)(const void *)(buf + h.off_strings);

    /* name tables */
    for (i = 0; i < h.bone_count; i++) {
        if (!string_in_table(buf, size, h.off_strings, model->bones[i].name_off)) {
            err = "bad bone name offset";
            goto fail;
        }
    }
    for (i = 0; i < h.anim_count; i++) {
        if (!string_in_table(buf, size, h.off_strings, model->anims[i].name_off)) {
            err = "bad animation name offset";
            goto fail;
        }
    }

    /* vertices: bone indices valid (0 when there is no skeleton) */
    for (i = 0; i < h.vertex_count; i++) {
        if (h.bone_count == 0u) {
            if (model->vertices[i].bone != 0u) { err = "static vertex has bone"; goto fail; }
        } else {
            if (model->vertices[i].bone >= h.bone_count) {
                err = "vertex references invalid bone";
                goto fail;
            }
        }
    }

    /* indices in range */
    for (i = 0; i < h.index_count; i++) {
        if (model->indices[i] >= h.vertex_count) { err = "index out of range"; goto fail; }
    }

    /* skeleton: parents valid and stored parent-before-child */
    for (i = 0; i < h.bone_count; i++) {
        int16_t parent = model->bones[i].parent;
        if (parent < -1 || parent >= (int16_t)h.bone_count || parent >= (int16_t)i) {
            err = "bad bone hierarchy";
            goto fail;
        }
    }
    if (h.bone_count == 0u && (h.flags & GPM_FLAG_SKINNED) != 0u) {
        err = "static model marked skinned";
        goto fail;
    }

    /* animation records reference a contiguous track range */
    track_count = 0;
    for (i = 0; i < h.anim_count; i++) {
        const GpmAnim *a = &model->anims[i];
        uint32_t end;
        if (a->duration < 0.0f || !isfinite(a->duration)) {
            err = "bad animation duration";
            goto fail;
        }
        if (a->track_count == 0u) {
            err = "animation has no tracks";
            goto fail;
        }
        if (a->track_first > 0xFFFFu || a->track_count > 0xFFFFu ||
            a->track_first + a->track_count < a->track_first) {
            err = "bad animation track range";
            goto fail;
        }
        end = a->track_first + a->track_count;
        if (end > track_count)
            track_count = end;
    }
    if (track_count > GPM_MAX_TRACKS) {
        err = "model exceeds runtime limits (tracks)";
        goto fail;
    }
    model->track_count = track_count;
    if (track_count > 0u) {
        if ((uint64_t)off[4] + (uint64_t)track_count * sizeof(GpmTrack) > size) {
            err = "tracks section out of bounds";
            goto fail;
        }
        model->tracks = (const GpmTrack *)(const void *)(buf + off[4]);
    }

    /* keys: stored sequentially in track order, sized by the track key counts */
    model->track_key_first = (uint32_t *)malloc((size_t)track_count * sizeof(uint32_t));
    if (model->track_key_first == NULL && track_count > 0u) {
        err = "out of memory building tracks";
        goto fail;
    }
    for (i = 0; i < track_count; i++) {
        const GpmTrack *tr = &model->tracks[i];
        if (tr->bone >= h.bone_count) { err = "track references invalid bone"; goto fail; }
        if (tr->component > 2u) { err = "bad track component"; goto fail; }
        if (tr->key_count == 0u) { err = "track has no keys"; goto fail; }
        model->track_key_first[i] = (uint32_t)key_sum;
        key_sum += tr->key_count;
        if (key_sum > 0xFFFFFFFFu) { err = "too many keys"; goto fail; }
    }
    key_count = (uint32_t)key_sum;
    model->key_count = key_count;
    if ((uint64_t)off[5] + (uint64_t)key_count * sizeof(GpmKey) > size) {
        err = "keys section out of bounds";
        goto fail;
    }
    if (key_count > 0u)
        model->keys = (const GpmKey *)(const void *)(buf + off[5]);

    /* keys sorted by time within each track */
    for (i = 0; i < track_count; i++) {
        const GpmTrack *tr = &model->tracks[i];
        uint32_t first = model->track_key_first[i];
        uint32_t k;
        for (k = 1; k < tr->key_count; k++) {
            float t0 = model->keys[first + k - 1u].time;
            float t1 = model->keys[first + k].time;
            if (!isfinite(t0) || !isfinite(t1) || t1 < t0) {
                err = "keys not sorted by time";
                goto fail;
            }
        }
    }

    /* derived rig matrices */
    if (h.bone_count > 0u && !build_rest_matrices(model)) {
        err = "out of memory building rig";
        goto fail;
    }

    return NULL;

fail:
    gpm_destroy(model);
    return err;
}

const char *gpm_load_file(GpmModel *model, const char *path)
{
    FILE *f;
    long len;
    uint8_t *buf;
    size_t got;
    const char *err;

    if (model == NULL || path == NULL)
        return "null argument";
    memset(model, 0, sizeof(*model));

    f = fopen(path, "rb");
    if (f == NULL)
        return "file not found";
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return "seek failed";
    }
    len = ftell(f);
    if (len < 0 || len > (long)(64l * 1024l * 1024l) || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return "bad file size";
    }
    buf = (uint8_t *)malloc((size_t)len);
    if (buf == NULL) {
        fclose(f);
        return "out of memory";
    }
    got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        free(buf);
        return "read failed";
    }

    err = gpm_parse_memory(model, buf, (size_t)len);
    if (err != NULL) {
        free(buf);
        return err;
    }
    /* parse borrowed our aligned buffer, or made its own copy when needed */
    if (!model->owns_data) {
        model->data = buf;
        model->owns_data = true;
        model->data_size = (size_t)len;
    } else {
        free(buf);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* queries                                                            */
/* ------------------------------------------------------------------ */

const char *gpm_name(const GpmModel *model, uint32_t off)
{
    uint32_t base;
    if (model == NULL || model->strings == NULL || model->data == NULL)
        return NULL;
    base = (uint32_t)((const uint8_t *)model->strings - model->data);
    if (base >= model->data_size || off >= model->data_size - base)
        return NULL;
    return model->strings + off;
}

int gpm_find_bone(const GpmModel *model, const char *name)
{
    uint32_t i;
    if (model == NULL || name == NULL || model->bones == NULL)
        return -1;
    for (i = 0; i < model->bone_count; i++) {
        const char *bn = gpm_name(model, model->bones[i].name_off);
        if (bn != NULL && strcmp(bn, name) == 0)
            return (int)i;
    }
    return -1;
}

const char *gpm_anim_name(const GpmModel *model, int anim_index)
{
    if (model == NULL || model->anims == NULL)
        return NULL;
    if (anim_index < 0 || (uint32_t)anim_index >= model->anim_count)
        return NULL;
    return gpm_name(model, model->anims[anim_index].name_off);
}

float gpm_anim_duration(const GpmModel *model, int anim_index)
{
    if (model == NULL || model->anims == NULL)
        return 0.0f;
    if (anim_index < 0 || (uint32_t)anim_index >= model->anim_count)
        return 0.0f;
    return model->anims[anim_index].duration;
}

int gpm_find_anim(const GpmModel *model, const char *role)
{
    uint32_t i;
    if (model == NULL || role == NULL || model->anims == NULL)
        return -1;
    for (i = 0; i < model->anim_count; i++) {
        const char *an = gpm_name(model, model->anims[i].name_off);
        if (an != NULL && strcmp(an, role) == 0)
            return (int)i;
    }
    return -1;
}

int gpm_find_anim_chain(const GpmModel *model, const char *const *roles, int role_count)
{
    int i;
    if (model == NULL || roles == NULL)
        return -1;
    for (i = 0; i < role_count; i++) {
        int idx = gpm_find_anim(model, roles[i]);
        if (idx >= 0)
            return idx;
    }
    return -1;
}

bool gpm_is_skinned(const GpmModel *model)
{
    if (model == NULL || model->bones == NULL)
        return false;
    return model->bone_count > 0u && (model->flags & GPM_FLAG_SKINNED) != 0u;
}
