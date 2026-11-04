/*
 * Gravepowder - CPU skinning runtime (see include/skin.h)
 *
 * Rigid skinning: each vertex follows exactly its dominant bone (GPM v1 spec).
 * Fixed-size per-instance buffers, no allocation after skin_init().
 */
#include <math.h>
#include <string.h>

#include "skin.h"

/* ------------------------------------------------------------------ */
/* matrix / quaternion helpers (row-major, column-vector: v' = M*v)   */
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

static void quat_normalize(float q[4])
{
    float len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len < 1e-8f) {
        q[0] = 0.0f; q[1] = 0.0f; q[2] = 0.0f; q[3] = 1.0f;
    } else {
        len = 1.0f / len;
        q[0] *= len; q[1] *= len; q[2] *= len; q[3] *= len;
    }
}

static void quat_slerp(float out[4], const float a[4], const float b[4], float t)
{
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float sign = 1.0f;
    float ax = a[0], ay = a[1], az = a[2], aw = a[3];
    float bx = b[0], by = b[1], bz = b[2], bw = b[3];
    float s0, s1;

    if (dot < 0.0f) {
        dot = -dot;
        sign = -1.0f;
    }
    if (dot > 0.9995f) {
        /* nearly identical: nlerp is stable and cheap */
        out[0] = ax + (bx * sign - ax) * t;
        out[1] = ay + (by * sign - ay) * t;
        out[2] = az + (bz * sign - az) * t;
        out[3] = aw + (bw * sign - aw) * t;
        quat_normalize(out);
        return;
    }
    {
        float theta = acosf(dot);
        float sin_theta = sinf(theta);
        s0 = sinf((1.0f - t) * theta) / sin_theta;
        s1 = sinf(t * theta) / sin_theta * sign;
    }
    out[0] = ax * s0 + bx * s1;
    out[1] = ay * s0 + by * s1;
    out[2] = az * s0 + bz * s1;
    out[3] = aw * s0 + bw * s1;
}

static float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

static float clampf01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* ------------------------------------------------------------------ */
/* clip sampling                                                      */
/* ------------------------------------------------------------------ */

/* Fills `out` (bone_count entries) with the rest pose. */
static void sample_rest(const GpmModel *m, SkinTrs *out)
{
    uint32_t i;
    for (i = 0; i < m->bone_count; i++) {
        const GpmBone *b = &m->bones[i];
        out[i].translation[0] = b->translation[0];
        out[i].translation[1] = b->translation[1];
        out[i].translation[2] = b->translation[2];
        out[i].rotation[0] = b->rotation[0];
        out[i].rotation[1] = b->rotation[1];
        out[i].rotation[2] = b->rotation[2];
        out[i].rotation[3] = b->rotation[3];
        quat_normalize(out[i].rotation);
        out[i].scale[0] = b->scale[0];
        out[i].scale[1] = b->scale[1];
        out[i].scale[2] = b->scale[2];
    }
}

static void sample_track(const GpmModel *m, const GpmTrack *tr, uint32_t key_first,
                         float t, SkinTrs *out)
{
    uint32_t first = key_first;
    uint32_t count = tr->key_count;
    const GpmKey *keys = &m->keys[first];
    uint32_t lo = 0, hi = count - 1u;
    const GpmKey *k0, *k1;
    float span, f;
    SkinTrs *dst = &out[tr->bone];

    if (t <= keys[0].time) {
        k0 = &keys[0];
        k1 = k0;
        f = 0.0f;
    } else if (t >= keys[hi].time) {
        k0 = &keys[hi];
        k1 = k0;
        f = 0.0f;
    } else {
        /* binary search for the last key with time <= t */
        while (lo + 1u < hi) {
            uint32_t mid = (lo + hi) / 2u;
            if (keys[mid].time <= t)
                lo = mid;
            else
                hi = mid;
        }
        k0 = &keys[lo];
        k1 = &keys[lo + 1u];
        span = k1->time - k0->time;
        f = (span > 1e-8f) ? (t - k0->time) / span : 0.0f;
    }

    switch (tr->component) {
    case 0: /* translation */
        dst->translation[0] = lerpf(k0->value[0], k1->value[0], f);
        dst->translation[1] = lerpf(k0->value[1], k1->value[1], f);
        dst->translation[2] = lerpf(k0->value[2], k1->value[2], f);
        break;
    case 1: /* rotation */
        quat_slerp(dst->rotation, k0->value, k1->value, f);
        break;
    default: /* scale */
        dst->scale[0] = lerpf(k0->value[0], k1->value[0], f);
        dst->scale[1] = lerpf(k0->value[1], k1->value[1], f);
        dst->scale[2] = lerpf(k0->value[2], k1->value[2], f);
        break;
    }
}

static void sample_clip(const GpmModel *m, int clip, float time, SkinTrs *out)
{
    const GpmAnim *an;
    float duration;
    uint32_t i;

    sample_rest(m, out);
    if (clip < 0 || (uint32_t)clip >= m->anim_count)
        return;

    an = &m->anims[clip];
    duration = an->duration;
    if (duration > 1e-6f) {
        time = fmodf(time, duration);
        if (time < 0.0f)
            time += duration;
    } else {
        time = 0.0f;
    }

    for (i = 0; i < an->track_count; i++) {
        uint32_t track_index = an->track_first + i;
        const GpmTrack *tr = &m->tracks[track_index];
        sample_track(m, tr, m->track_key_first[track_index], time, out);
    }
}

/* ------------------------------------------------------------------ */
/* pose construction                                                  */
/* ------------------------------------------------------------------ */

static void build_pose(SkinInstance *inst, const SkinTrs *local)
{
    const GpmModel *m = inst->model;
    uint32_t i;

    for (i = 0; i < m->bone_count; i++) {
        float lm[16];
        const GpmBone *b = &m->bones[i];

        mat4_from_trs(lm, local[i].translation, local[i].rotation,
                      local[i].scale);
        if (b->parent < 0) {
            memcpy(inst->world[i], lm, sizeof(lm));
        } else {
            mat4_mul(inst->world[i], inst->world[(uint32_t)b->parent], lm);
        }
        mat4_mul(inst->pose[i], inst->world[i], &m->inv_bind[i * 16]);
    }
}

static void blend_pose(const SkinInstance *inst, SkinTrs *out)
{
    const GpmModel *m = inst->model;
    float t = clampf01(inst->blend);
    uint32_t i;

    /* smoothstep for a less mechanical crossfade */
    t = t * t * (3.0f - 2.0f * t);

    for (i = 0; i < m->bone_count; i++) {
        const SkinTrs *a = &inst->local_a[i];
        const SkinTrs *b = &inst->local_b[i];
        out[i].translation[0] = lerpf(a->translation[0], b->translation[0], t);
        out[i].translation[1] = lerpf(a->translation[1], b->translation[1], t);
        out[i].translation[2] = lerpf(a->translation[2], b->translation[2], t);
        quat_slerp(out[i].rotation, a->rotation, b->rotation, t);
        out[i].scale[0] = lerpf(a->scale[0], b->scale[0], t);
        out[i].scale[1] = lerpf(a->scale[1], b->scale[1], t);
        out[i].scale[2] = lerpf(a->scale[2], b->scale[2], t);
    }
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

void skin_init(SkinInstance *inst, const GpmModel *model)
{
    memset(inst, 0, sizeof(*inst));
    inst->model = model;
    inst->clip_a = -1;
    inst->clip_b = -1;
    inst->blend = 1.0f;
}

void skin_set_clip(SkinInstance *inst, int clip, float time)
{
    if (inst == NULL)
        return;

    if (inst->clip_a < 0 && inst->clip_b < 0) {
        /* first request: apply immediately instead of fading from rest */
        inst->clip_a = clip;
        inst->time_a = time;
        inst->clip_b = clip;
        inst->time_b = time;
        inst->blend = 1.0f;
        return;
    }
    if (inst->clip_a < 0 && inst->blend < 1.0f) {
        /* currently showing the rest pose: no meaningful fade source */
        inst->clip_a = clip;
        inst->time_a = time;
        inst->clip_b = clip;
        inst->time_b = time;
        inst->blend = 1.0f;
        return;
    }
    if (clip == inst->clip_b) {
        inst->time_b = time;
        if (inst->blend >= 1.0f)
            inst->time_a = time;
        return;
    }
    if (clip == inst->clip_a && inst->blend >= 1.0f) {
        inst->time_a = time;
        return;
    }

    /* switching target: fade from whatever is currently visible */
    if (inst->clip_b >= 0 && inst->blend < 1.0f) {
        /* restart the fade from the one-shot target to the new one */
        inst->clip_a = inst->clip_b;
        inst->time_a = inst->time_b;
    }
    inst->clip_b = clip;
    inst->time_b = time;
    inst->blend = 0.0f;
}

void skin_update(SkinInstance *inst, float dt)
{
    const GpmModel *m;
    static SkinTrs s_blended[SKIN_MAX_BONES]; /* single-threaded renderer */

    if (inst == NULL || inst->model == NULL)
        return;
    m = inst->model;
    if (m->bone_count == 0)
        return;

    if (inst->clip_a < 0 && inst->clip_b < 0) {
        sample_rest(m, inst->local_a);
        build_pose(inst, inst->local_a);
        return;
    }

    if (inst->blend < 1.0f) {
        inst->blend = clampf01(inst->blend + (dt / SKIN_BLEND_TIME));
        if (inst->blend >= 1.0f) {
            inst->clip_a = inst->clip_b;
            inst->time_a = inst->time_b;
        }
    }

    if (inst->clip_a >= 0)
        sample_clip(m, inst->clip_a, inst->time_a, inst->local_a);
    else
        sample_rest(m, inst->local_a);

    if (inst->blend < 1.0f && inst->clip_b >= 0 && inst->clip_b != inst->clip_a) {
        sample_clip(m, inst->clip_b, inst->time_b, inst->local_b);
        blend_pose(inst, s_blended);
        build_pose(inst, s_blended);
    } else {
        build_pose(inst, inst->local_a);
    }
}

int skin_vertices(const SkinInstance *inst, SkinVertex *out, int max_verts)
{
    const GpmModel *m;
    uint32_t i;
    int count;

    if (inst == NULL || inst->model == NULL || out == NULL)
        return 0;
    m = inst->model;
    if ((uint32_t)max_verts < m->vertex_count)
        return 0;
    count = (int)m->vertex_count;

    if (m->bone_count == 0) {
        for (i = 0; i < m->vertex_count; i++) {
            const GpmVertex *v = &m->vertices[i];
            SkinVertex *o = &out[i];
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
        return count;
    }

    for (i = 0; i < m->vertex_count; i++) {
        const GpmVertex *v = &m->vertices[i];
        const float *pm = inst->pose[v->bone];
        SkinVertex *o = &out[i];
        float px = v->pos[0], py = v->pos[1], pz = v->pos[2];
        float nx, ny, nz, nlen;

        o->pos[0] = pm[0] * px + pm[1] * py + pm[2] * pz + pm[3];
        o->pos[1] = pm[4] * px + pm[5] * py + pm[6] * pz + pm[7];
        o->pos[2] = pm[8] * px + pm[9] * py + pm[10] * pz + pm[11];

        nx = pm[0] * v->nrm[0] + pm[1] * v->nrm[1] + pm[2] * v->nrm[2];
        ny = pm[4] * v->nrm[0] + pm[5] * v->nrm[1] + pm[6] * v->nrm[2];
        nz = pm[8] * v->nrm[0] + pm[9] * v->nrm[1] + pm[10] * v->nrm[2];
        nlen = sqrtf(nx * nx + ny * ny + nz * nz);
        if (nlen > 1e-8f) {
            float inv = 1.0f / nlen;
            nx *= inv;
            ny *= inv;
            nz *= inv;
        } else {
            nx = 0.0f;
            ny = 1.0f;
            nz = 0.0f;
        }
        o->nrm[0] = nx;
        o->nrm[1] = ny;
        o->nrm[2] = nz;

        o->col[0] = (float)v->col[0] * (1.0f / 255.0f);
        o->col[1] = (float)v->col[1] * (1.0f / 255.0f);
        o->col[2] = (float)v->col[2] * (1.0f / 255.0f);
    }
    return count;
}

const float *skin_bone_matrix(const SkinInstance *inst, int bone)
{
    if (inst == NULL || inst->model == NULL || bone < 0 ||
        (uint32_t)bone >= inst->model->bone_count)
        return NULL;
    return inst->world[bone];
}

int skin_bone_index(const SkinInstance *inst, const char *name)
{
    if (inst == NULL || inst->model == NULL)
        return -1;
    return gpm_find_bone(inst->model, name);
}

bool skin_is_animated(const SkinInstance *inst)
{
    return inst != NULL && inst->model != NULL &&
           gpm_is_skinned(inst->model);
}
