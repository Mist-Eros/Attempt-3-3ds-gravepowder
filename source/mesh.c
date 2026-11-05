/*
 * Gravepowder - procedural mesh builders (see mesh.h for the format)
 *
 * All geometry is flat-shaded: each triangle gets its face normal, so the
 * vertex-colour pipeline gives a crisp low-poly look without any texture.
 * Meshes are baked once at boot into linearAlloc'd VBOs.
 */
#include <math.h>
#include <string.h>

#include "mesh.h"

#define PI_F 3.14159265f

/* ------------------------------------------------------------------ */
/* arena                                                              */
/* ------------------------------------------------------------------ */

void mesh_arena_init(MeshArena *arena, void *mem, size_t size)
{
    arena->base = (uint8_t *)mem;
    arena->size = size;
    arena->used = 0;
    arena->overflow = false;
}

void *mesh_arena_alloc(MeshArena *arena, size_t size)
{
    size_t aligned;
    void *ptr;

    if (arena->base == NULL)
        return NULL;

    aligned = (arena->used + 15u) & ~(size_t)15u;
    if (size > arena->size || aligned + size > arena->size) {
        arena->overflow = true;
        return NULL;
    }
    ptr = arena->base + aligned;
    arena->used = aligned + size;
    return ptr;
}

/* ------------------------------------------------------------------ */
/* builder core                                                       */
/* ------------------------------------------------------------------ */

void mesh_builder_begin(MeshBuilder *b, MeshVertex *scratch, int capacity)
{
    b->verts = scratch;
    b->count = 0;
    b->capacity = capacity;
    b->overflow = false;
}

static void xf_normal(const C3D_Mtx *xf, float nx, float ny, float nz,
                      float *ox, float *oy, float *oz)
{
    float tx = xf->r[0].x * nx + xf->r[0].y * ny + xf->r[0].z * nz;
    float ty = xf->r[1].x * nx + xf->r[1].y * ny + xf->r[1].z * nz;
    float tz = xf->r[2].x * nx + xf->r[2].y * ny + xf->r[2].z * nz;
    float len = sqrtf(tx * tx + ty * ty + tz * tz);

    if (len < 1e-6f) {
        *ox = 0.0f;
        *oy = 1.0f;
        *oz = 0.0f;
        return;
    }
    len = 1.0f / len;
    *ox = tx * len;
    *oy = ty * len;
    *oz = tz * len;
}

void mesh_builder_vertex(MeshBuilder *b, const C3D_Mtx *xf,
                         float x, float y, float z,
                         float nx, float ny, float nz,
                         float r, float g, float bl)
{
    C3D_FVec tp;
    MeshVertex *v;
    float tnx, tny, tnz;

    if (b->count >= b->capacity) {
        b->overflow = true;
        return;
    }

    tp = Mtx_MultiplyFVecH(xf, FVec3_New(x, y, z));
    xf_normal(xf, nx, ny, nz, &tnx, &tny, &tnz);

    v = &b->verts[b->count++];
    v->pos[0] = tp.x;
    v->pos[1] = tp.y;
    v->pos[2] = tp.z;
    v->nrm[0] = tnx;
    v->nrm[1] = tny;
    v->nrm[2] = tnz;
    v->col[0] = r;
    v->col[1] = g;
    v->col[2] = bl;
}

/* Appends one flat triangle; the face normal comes from the local geometry. */
static void push_tri(MeshBuilder *b, const C3D_Mtx *xf,
                     float x0, float y0, float z0,
                     float x1, float y1, float z1,
                     float x2, float y2, float z2,
                     float r, float g, float bl)
{
    float ax = x1 - x0, ay = y1 - y0, az = z1 - z0;
    float bx = x2 - x0, by = y2 - y0, bz = z2 - z0;
    float nx = ay * bz - az * by;
    float ny = az * bx - ax * bz;
    float nz = ax * by - ay * bx;
    float len = sqrtf(nx * nx + ny * ny + nz * nz);

    if (len > 1e-9f) {
        len = 1.0f / len;
        nx *= len;
        ny *= len;
        nz *= len;
    } else {
        nx = 0.0f;
        ny = 1.0f;
        nz = 0.0f;
    }

    mesh_builder_vertex(b, xf, x0, y0, z0, nx, ny, nz, r, g, bl);
    mesh_builder_vertex(b, xf, x1, y1, z1, nx, ny, nz, r, g, bl);
    mesh_builder_vertex(b, xf, x2, y2, z2, nx, ny, nz, r, g, bl);
}

static void push_quad(MeshBuilder *b, const C3D_Mtx *xf,
                      float x0, float y0, float z0,
                      float x1, float y1, float z1,
                      float x2, float y2, float z2,
                      float x3, float y3, float z3,
                      float r, float g, float bl)
{
    push_tri(b, xf, x0, y0, z0, x1, y1, z1, x2, y2, z2, r, g, bl);
    push_tri(b, xf, x0, y0, z0, x2, y2, z2, x3, y3, z3, r, g, bl);
}

void mesh_builder_box(MeshBuilder *b, const C3D_Mtx *xf,
                      float cx, float cy, float cz,
                      float hx, float hy, float hz,
                      float r, float g, float bl)
{
    float x0 = cx - hx, x1 = cx + hx;
    float y0 = cy - hy, y1 = cy + hy;
    float z0 = cz - hz, z1 = cz + hz;

    push_quad(b, xf, x0, y0, z1, x1, y0, z1, x1, y1, z1, x0, y1, z1, r, g, bl);
    push_quad(b, xf, x1, y0, z0, x0, y0, z0, x0, y1, z0, x1, y1, z0, r, g, bl);
    push_quad(b, xf, x1, y0, z1, x1, y0, z0, x1, y1, z0, x1, y1, z1, r, g, bl);
    push_quad(b, xf, x0, y0, z0, x0, y0, z1, x0, y1, z1, x0, y1, z0, r, g, bl);
    push_quad(b, xf, x0, y1, z1, x1, y1, z1, x1, y1, z0, x0, y1, z0, r, g, bl);
    push_quad(b, xf, x0, y0, z0, x1, y0, z0, x1, y0, z1, x0, y0, z1, r, g, bl);
}

void mesh_builder_quad(MeshBuilder *b, const C3D_Mtx *xf,
                       float cx, float cy, float cz,
                       float half_w, float half_h,
                       float r, float g, float bl)
{
    float x0 = cx - half_w, x1 = cx + half_w;
    float y0 = cy - half_h, y1 = cy + half_h;
    push_quad(b, xf, x0, y0, cz, x1, y0, cz, x1, y1, cz, x0, y1, cz, r, g, bl);
}

void mesh_builder_cyl(MeshBuilder *b, const C3D_Mtx *xf,
                      float cx, float cy, float cz,
                      float radius, float height, int sides, bool caps,
                      float r, float g, float bl)
{
    mesh_builder_cone(b, xf, cx, cy, cz, radius, radius, height, sides, caps,
                      r, g, bl);
}

void mesh_builder_cone(MeshBuilder *b, const C3D_Mtx *xf,
                       float cx, float cy, float cz,
                       float bottom_radius, float top_radius, float height,
                       int sides, bool caps,
                       float r, float g, float bl)
{
    float y0 = cy - height * 0.5f;
    float y1 = cy + height * 0.5f;
    int i;

    if (sides < 3)
        sides = 3;

    for (i = 0; i < sides; i++) {
        float a0 = (float)i * 2.0f * PI_F / (float)sides;
        float a1 = (float)(i + 1) * 2.0f * PI_F / (float)sides;
        float c0 = cosf(a0), s0 = sinf(a0);
        float c1 = cosf(a1), s1 = sinf(a1);

        float bx0 = cx + bottom_radius * c0;
        float bz0 = cz + bottom_radius * s0;
        float bx1 = cx + bottom_radius * c1;
        float bz1 = cz + bottom_radius * s1;
        float tx0 = cx + top_radius * c0;
        float tz0 = cz + top_radius * s0;
        float tx1 = cx + top_radius * c1;
        float tz1 = cz + top_radius * s1;

        if (top_radius <= 1e-5f) {
            push_tri(b, xf, bx0, y0, bz0, bx1, y0, bz1, tx0, y1, tz0, r, g, bl);
        } else {
            push_quad(b, xf, bx0, y0, bz0, tx0, y1, tz0, tx1, y1, tz1,
                      bx1, y0, bz1, r, g, bl);
        }

        if (caps) {
            if (top_radius > 1e-5f)
                push_tri(b, xf, cx, y1, cz, tx1, y1, tz1, tx0, y1, tz0, r, g, bl);
            push_tri(b, xf, cx, y0, cz, bx0, y0, bz0, bx1, y0, bz1, r, g, bl);
        }
    }
}

bool mesh_commit(MeshBuilder *b, Mesh *out, MeshArena *arena)
{
    MeshVertex *dst;
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    float radius = 0.0f;
    int i;

    memset(out, 0, sizeof(*out));

    if (b->overflow || b->count <= 0)
        return false;

    for (i = 0; i < b->count; i++) {
        cx += b->verts[i].pos[0];
        cy += b->verts[i].pos[1];
        cz += b->verts[i].pos[2];
    }
    cx /= (float)b->count;
    cy /= (float)b->count;
    cz /= (float)b->count;
    for (i = 0; i < b->count; i++) {
        float dx = b->verts[i].pos[0] - cx;
        float dy = b->verts[i].pos[1] - cy;
        float dz = b->verts[i].pos[2] - cz;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > radius)
            radius = d2;
    }
    radius = sqrtf(radius);

    dst = (MeshVertex *)mesh_arena_alloc(arena,
                                         (size_t)b->count * sizeof(MeshVertex));
    if (dst == NULL)
        return false;

    memcpy(dst, b->verts, (size_t)b->count * sizeof(MeshVertex));
    out->vbo = dst;
    out->vertex_count = (uint32_t)b->count;
    out->center = vec3_make(cx, cy, cz);
    out->radius = radius;
    return true;
}

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static void xf_reset(C3D_Mtx *m)
{
    Mtx_Identity(m);
}

/* ---- albedo palettes ---------------------------------------------- */

/* Props are built in near-neutral greys and tinted by kind at draw time. */
static const float A_MAIN[3]   = { 0.95f, 0.93f, 0.90f };
static const float A_MID[3]    = { 0.76f, 0.74f, 0.71f };
static const float A_DARK[3]   = { 0.55f, 0.53f, 0.50f };
static const float A_ACCENT[3] = { 0.36f, 0.35f, 0.34f };
static const float A_METAL[3]  = { 0.64f, 0.64f, 0.66f };

/* ---- hash noise ---------------------------------------------------- */

static uint32_t hash_u32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static float hash2f(int ix, int iz)
{
    uint32_t h = hash_u32((uint32_t)ix * 0x9E3779B1u);
    h = hash_u32(h ^ ((uint32_t)iz * 0x85EBCA77u));
    return (float)(h & 0xFFFFu) / 65535.0f;
}

static float value_noise2(float x, float z)
{
    int ix = (int)floorf(x);
    int iz = (int)floorf(z);
    float fx = x - (float)ix;
    float fz = z - (float)iz;
    float sx = fx * fx * (3.0f - 2.0f * fx);
    float sz = fz * fz * (3.0f - 2.0f * fz);
    float n00 = hash2f(ix, iz);
    float n10 = hash2f(ix + 1, iz);
    float n01 = hash2f(ix, iz + 1);
    float n11 = hash2f(ix + 1, iz + 1);
    float a = n00 + (n10 - n00) * sx;
    float b2 = n01 + (n11 - n01) * sx;
    return a + (b2 - a) * sz;
}

/* ------------------------------------------------------------------ */
/* ground                                                             */
/* ------------------------------------------------------------------ */

static void ground_color(float px, float pz, float out[3])
{
    float n1 = value_noise2(px * 0.16f + 3.1f, pz * 0.16f - 1.7f);
    float n2 = value_noise2(px * 0.85f - 11.0f, pz * 0.85f + 5.0f);
    float n3 = value_noise2(px * 0.05f + 40.0f, pz * 0.05f - 23.0f);
    float shade = 0.70f + 0.30f * n1 + 0.12f * (n2 - 0.5f) + 0.18f * (n3 - 0.5f);
    float r = 0.36f * shade;
    float g = 0.28f * shade;
    float b = 0.19f * shade;

    if (n1 > 0.62f) {
        float t = (n1 - 0.62f) * 1.9f;
        if (t > 1.0f)
            t = 1.0f;
        r = r * (1.0f - t) + 0.22f * t;
        g = g * (1.0f - t) + 0.24f * t;
        b = b * (1.0f - t) + 0.15f * t;
    }

    if (r > 0.48f) r = 0.48f;
    if (g > 0.38f) g = 0.38f;
    if (b > 0.26f) b = 0.26f;
    if (r < 0.12f) r = 0.12f;
    if (g < 0.10f) g = 0.10f;
    if (b < 0.07f) b = 0.07f;

    out[0] = r;
    out[1] = g;
    out[2] = b;
}

void mesh_build_ground(Mesh *out, MeshArena *arena,
                       MeshVertex *scratch, int scratch_cap,
                       float half_extent, int cells)
{
    MeshBuilder b;
    C3D_Mtx id;
    int gx, gz;

    if (cells < 1)
        cells = 1;

    mesh_builder_begin(&b, scratch, scratch_cap);
    xf_reset(&id);

    for (gz = 0; gz < cells; gz++) {
        for (gx = 0; gx < cells; gx++) {
            float x0 = -half_extent + (2.0f * half_extent) * (float)gx / (float)cells;
            float x1 = -half_extent + (2.0f * half_extent) * (float)(gx + 1) / (float)cells;
            float z0 = -half_extent + (2.0f * half_extent) * (float)gz / (float)cells;
            float z1 = -half_extent + (2.0f * half_extent) * (float)(gz + 1) / (float)cells;
            float c00[3], c10[3], c01[3], c11[3];

            ground_color(x0, z0, c00);
            ground_color(x1, z0, c10);
            ground_color(x0, z1, c01);
            ground_color(x1, z1, c11);

            /* two triangles, +Y facing, per-vertex mottled colour */
            mesh_builder_vertex(&b, &id, x0, 0.0f, z1, 0.0f, 1.0f, 0.0f,
                                c01[0], c01[1], c01[2]);
            mesh_builder_vertex(&b, &id, x1, 0.0f, z1, 0.0f, 1.0f, 0.0f,
                                c11[0], c11[1], c11[2]);
            mesh_builder_vertex(&b, &id, x1, 0.0f, z0, 0.0f, 1.0f, 0.0f,
                                c10[0], c10[1], c10[2]);

            mesh_builder_vertex(&b, &id, x0, 0.0f, z1, 0.0f, 1.0f, 0.0f,
                                c01[0], c01[1], c01[2]);
            mesh_builder_vertex(&b, &id, x1, 0.0f, z0, 0.0f, 1.0f, 0.0f,
                                c10[0], c10[1], c10[2]);
            mesh_builder_vertex(&b, &id, x0, 0.0f, z0, 0.0f, 1.0f, 0.0f,
                                c00[0], c00[1], c00[2]);
        }
    }

    (void)mesh_commit(&b, out, arena);
}

/* ------------------------------------------------------------------ */
/* distant hills                                                      */
/* ------------------------------------------------------------------ */

void mesh_build_hills(Mesh *out, MeshArena *arena,
                      MeshVertex *scratch, int scratch_cap, float radius)
{
    MeshBuilder b;
    C3D_Mtx id;
    const int segments = 20;
    int i;

    mesh_builder_begin(&b, scratch, scratch_cap);
    xf_reset(&id);

    for (i = 0; i < segments; i++) {
        float step = 2.0f * PI_F / (float)segments;
        float a0 = (float)i * step - step * 0.55f;
        float a1 = (float)i * step + step * 0.55f;
        float ac = (float)i * step;
        float h = 3.0f + 6.0f * hash2f(i, 3);
        float r0 = radius * (0.98f + 0.04f * hash2f(i, 7));
        float x0 = cosf(a0) * r0, z0 = sinf(a0) * r0;
        float x1 = cosf(a1) * r0, z1 = sinf(a1) * r0;
        float xc = cosf(ac) * (r0 * 0.96f), zc = sinf(ac) * (r0 * 0.96f);
        float base = 0.19f + 0.03f * hash2f(i, 11);

        push_tri(&b, &id, x0, 0.0f, z0, xc, h, zc, x1, 0.0f, z1,
                 base, base * 0.97f, base * 0.93f);
    }

    (void)mesh_commit(&b, out, arena);
}

/* ------------------------------------------------------------------ */
/* props                                                              */
/* ------------------------------------------------------------------ */

static void prop_crate(MeshBuilder *b, C3D_Mtx *id)
{
    int sx, sz;
    mesh_builder_box(b, id, 0.0f, 0.48f, 0.0f, 0.47f, 0.48f, 0.47f,
                     A_MAIN[0], A_MAIN[1], A_MAIN[2]);
    for (sx = -1; sx <= 1; sx += 2) {
        for (sz = -1; sz <= 1; sz += 2) {
            mesh_builder_box(b, id, 0.44f * (float)sx, 0.48f, 0.44f * (float)sz,
                             0.05f, 0.49f, 0.05f,
                             A_DARK[0], A_DARK[1], A_DARK[2]);
        }
    }
    mesh_builder_box(b, id, 0.0f, 0.16f, 0.0f, 0.49f, 0.045f, 0.49f,
                     A_ACCENT[0], A_ACCENT[1], A_ACCENT[2]);
    mesh_builder_box(b, id, 0.0f, 0.82f, 0.0f, 0.49f, 0.045f, 0.49f,
                     A_ACCENT[0], A_ACCENT[1], A_ACCENT[2]);
}

static void prop_barrel(MeshBuilder *b, C3D_Mtx *id)
{
    mesh_builder_cyl(b, id, 0.0f, 0.5f, 0.0f, 0.44f, 0.98f, 8, true,
                     A_MAIN[0], A_MAIN[1], A_MAIN[2]);
    mesh_builder_cyl(b, id, 0.0f, 0.18f, 0.0f, 0.465f, 0.07f, 8, false,
                     A_METAL[0], A_METAL[1], A_METAL[2]);
    mesh_builder_cyl(b, id, 0.0f, 0.52f, 0.0f, 0.468f, 0.07f, 8, false,
                     A_METAL[0], A_METAL[1], A_METAL[2]);
    mesh_builder_cyl(b, id, 0.0f, 0.84f, 0.0f, 0.465f, 0.07f, 8, false,
                     A_METAL[0], A_METAL[1], A_METAL[2]);
}

static void prop_fence(MeshBuilder *b, C3D_Mtx *id)
{
    mesh_builder_box(b, id, -0.44f, 0.5f, 0.0f, 0.055f, 0.5f, 0.055f,
                     A_MAIN[0], A_MAIN[1], A_MAIN[2]);
    mesh_builder_box(b, id, 0.44f, 0.5f, 0.0f, 0.055f, 0.5f, 0.055f,
                     A_MAIN[0], A_MAIN[1], A_MAIN[2]);
    mesh_builder_box(b, id, 0.0f, 0.26f, 0.0f, 0.47f, 0.075f, 0.035f,
                     A_MID[0], A_MID[1], A_MID[2]);
    mesh_builder_box(b, id, 0.0f, 0.58f, 0.0f, 0.47f, 0.065f, 0.035f,
                     A_MID[0], A_MID[1], A_MID[2]);
    mesh_builder_box(b, id, 0.0f, 0.88f, 0.0f, 0.47f, 0.06f, 0.032f,
                     A_MID[0], A_MID[1], A_MID[2]);
    {
        C3D_Mtx m;
        xf_reset(&m);
        Mtx_Translate(&m, 0.0f, 0.57f, 0.0f, true);
        Mtx_RotateZ(&m, 0.38f, true);
        mesh_builder_box(b, &m, 0.0f, 0.0f, 0.0f, 0.44f, 0.05f, 0.03f,
                         A_DARK[0], A_DARK[1], A_DARK[2]);
    }
}

static void prop_tombstone(MeshBuilder *b, C3D_Mtx *id)
{
    mesh_builder_box(b, id, 0.0f, 0.07f, 0.0f, 0.44f, 0.07f, 0.30f,
                     A_MID[0], A_MID[1], A_MID[2]);
    mesh_builder_box(b, id, 0.0f, 0.45f, -0.02f, 0.32f, 0.33f, 0.13f,
                     A_MAIN[0], A_MAIN[1], A_MAIN[2]);
    mesh_builder_box(b, id, 0.0f, 0.80f, -0.02f, 0.25f, 0.06f, 0.12f,
                     A_MAIN[0], A_MAIN[1], A_MAIN[2]);
    mesh_builder_box(b, id, 0.0f, 0.94f, -0.02f, 0.20f, 0.04f, 0.10f,
                     A_MID[0], A_MID[1], A_MID[2]);
    /* small cross */
    mesh_builder_box(b, id, 0.0f, 0.46f, 0.12f, 0.035f, 0.20f, 0.02f,
                     A_DARK[0], A_DARK[1], A_DARK[2]);
    mesh_builder_box(b, id, 0.0f, 0.52f, 0.12f, 0.13f, 0.035f, 0.02f,
                     A_DARK[0], A_DARK[1], A_DARK[2]);
}

static void tree_branch(MeshBuilder *b, float y, float dir, float len, float thick)
{
    C3D_Mtx m;
    xf_reset(&m);
    Mtx_Translate(&m, 0.0f, y, 0.0f, true);
    Mtx_RotateZ(&m, dir, true);
    Mtx_RotateX(&m, (dir > 0.0f ? 0.25f : -0.35f), true);
    mesh_builder_box(b, &m, 0.0f, len * 0.5f, 0.0f, thick, len, thick,
                     A_MID[0], A_MID[1], A_MID[2]);
}

static void prop_deadtree(MeshBuilder *b, C3D_Mtx *id)
{
    /* trunk */
    mesh_builder_cone(b, id, 0.0f, 0.45f, 0.0f, 0.16f, 0.09f, 0.90f, 6, true,
                      A_MAIN[0], A_MAIN[1], A_MAIN[2]);
    /* branches */
    tree_branch(b, 0.82f, 0.85f, 0.34f, 0.05f);
    tree_branch(b, 0.72f, -0.95f, 0.30f, 0.045f);
    tree_branch(b, 0.60f, 0.55f, 0.24f, 0.04f);
    tree_branch(b, 0.88f, -0.45f, 0.20f, 0.035f);
    /* roots */
    tree_branch(b, 0.06f, 0.55f, 0.16f, 0.045f);
    tree_branch(b, 0.06f, -0.60f, 0.15f, 0.04f);
    mesh_builder_box(b, id, 0.10f, 0.04f, 0.10f, 0.10f, 0.04f, 0.10f,
                     A_MID[0], A_MID[1], A_MID[2]);
}

static void prop_well(MeshBuilder *b, C3D_Mtx *id)
{
    const int sides = 10;
    int i;
    float ring_h = 0.42f;
    float ro = 0.50f, ri = 0.36f;

    /* stone ring: outer + inner walls + top rim */
    for (i = 0; i < sides; i++) {
        float a0 = (float)i * 2.0f * PI_F / (float)sides;
        float a1 = (float)(i + 1) * 2.0f * PI_F / (float)sides;
        float c0 = cosf(a0), s0 = sinf(a0);
        float c1 = cosf(a1), s1 = sinf(a1);

        push_quad(b, id,
                  ro * c0, 0.0f, ro * s0,
                  ro * c0, ring_h, ro * s0,
                  ro * c1, ring_h, ro * s1,
                  ro * c1, 0.0f, ro * s1,
                  A_MAIN[0], A_MAIN[1], A_MAIN[2]);
        push_quad(b, id,
                  ri * c1, 0.0f, ri * s1,
                  ri * c1, ring_h, ri * s1,
                  ri * c0, ring_h, ri * s0,
                  ri * c0, 0.0f, ri * s0,
                  A_DARK[0], A_DARK[1], A_DARK[2]);
        push_quad(b, id,
                  ri * c0, ring_h, ri * s0,
                  ro * c0, ring_h, ro * s0,
                  ro * c1, ring_h, ro * s1,
                  ri * c1, ring_h, ri * s1,
                  A_MID[0], A_MID[1], A_MID[2]);
    }

    /* roof posts */
    mesh_builder_box(b, id, -0.40f, 0.68f, 0.0f, 0.045f, 0.42f, 0.045f,
                     A_MID[0], A_MID[1], A_MID[2]);
    mesh_builder_box(b, id, 0.40f, 0.68f, 0.0f, 0.045f, 0.42f, 0.045f,
                     A_MID[0], A_MID[1], A_MID[2]);
    mesh_builder_box(b, id, 0.0f, 1.06f, 0.0f, 0.48f, 0.035f, 0.05f,
                     A_DARK[0], A_DARK[1], A_DARK[2]);
    /* pitched roof */
    {
        C3D_Mtx m;
        xf_reset(&m);
        Mtx_Translate(&m, -0.22f, 1.14f, 0.0f, true);
        Mtx_RotateZ(&m, -0.42f, true);
        mesh_builder_box(b, &m, 0.0f, 0.0f, 0.0f, 0.24f, 0.035f, 0.30f,
                         A_MID[0], A_MID[1], A_MID[2]);
        xf_reset(&m);
        Mtx_Translate(&m, 0.22f, 1.14f, 0.0f, true);
        Mtx_RotateZ(&m, 0.42f, true);
        mesh_builder_box(b, &m, 0.0f, 0.0f, 0.0f, 0.24f, 0.035f, 0.30f,
                         A_MID[0], A_MID[1], A_MID[2]);
    }
    /* bucket on a rope */
    mesh_builder_box(b, id, 0.0f, 0.78f, 0.0f, 0.09f, 0.10f, 0.09f,
                     A_ACCENT[0], A_ACCENT[1], A_ACCENT[2]);
}

static void prop_gate(MeshBuilder *b, C3D_Mtx *id)
{
    int i;
    mesh_builder_box(b, id, -0.44f, 0.5f, 0.0f, 0.06f, 0.5f, 0.09f,
                     A_MAIN[0], A_MAIN[1], A_MAIN[2]);
    mesh_builder_box(b, id, 0.44f, 0.5f, 0.0f, 0.06f, 0.5f, 0.09f,
                     A_MAIN[0], A_MAIN[1], A_MAIN[2]);
    mesh_builder_box(b, id, 0.0f, 0.93f, 0.0f, 0.44f, 0.06f, 0.05f,
                     A_DARK[0], A_DARK[1], A_DARK[2]);
    for (i = 0; i < 5; i++) {
        float x = -0.32f + 0.16f * (float)i;
        mesh_builder_box(b, id, x, 0.55f, 0.0f, 0.025f, 0.36f, 0.025f,
                         A_MID[0], A_MID[1], A_MID[2]);
    }
    {
        C3D_Mtx m;
        xf_reset(&m);
        Mtx_Translate(&m, 0.0f, 0.55f, 0.0f, true);
        Mtx_RotateZ(&m, 0.62f, true);
        mesh_builder_box(b, &m, 0.0f, 0.0f, 0.0f, 0.44f, 0.035f, 0.03f,
                         A_DARK[0], A_DARK[1], A_DARK[2]);
    }
}

/* ------------------------------------------------------------------ */
/* v0.3 OLD MINE props                                                */
/* ------------------------------------------------------------------ */

static void prop_timber(MeshBuilder *b, C3D_Mtx *id)
{
    static const float timber[3]      = { 0.31f, 0.22f, 0.14f };
    static const float timber_dark[3] = { 0.21f, 0.15f, 0.09f };
    static const float timber_light[3]= { 0.40f, 0.29f, 0.18f };
    static const float rope[3]        = { 0.55f, 0.44f, 0.28f };
    C3D_Mtx m;
    int i;

    /* portal: two posts, crossbeam, corner braces */
    mesh_builder_box(b, id, -0.42f, 0.44f, 0.0f, 0.065f, 0.44f, 0.085f,
                     timber[0], timber[1], timber[2]);
    mesh_builder_box(b, id, 0.42f, 0.44f, 0.0f, 0.065f, 0.44f, 0.085f,
                     timber[0], timber[1], timber[2]);
    mesh_builder_box(b, id, 0.0f, 0.92f, 0.0f, 0.50f, 0.055f, 0.075f,
                     timber_light[0], timber_light[1], timber_light[2]);
    for (i = -1; i <= 1; i += 2) {
        xf_reset(&m);
        Mtx_Translate(&m, 0.30f * (float)i, 0.80f, 0.0f, true);
        Mtx_RotateZ(&m, 0.70f * (float)i, true);
        mesh_builder_box(b, &m, 0.0f, 0.0f, 0.0f, 0.17f, 0.030f, 0.055f,
                         timber_dark[0], timber_dark[1], timber_dark[2]);
    }
    /* plank roof (suggest the tunnel behind the portal) */
    for (i = 0; i < 4; i++) {
        float z = -0.33f + 0.22f * (float)i;
        mesh_builder_box(b, id, 0.0f, 0.985f, z, 0.50f, 0.022f, 0.105f,
                         (i & 1) ? timber_light[0] : timber[0],
                         (i & 1) ? timber_light[1] : timber[1],
                         (i & 1) ? timber_light[2] : timber[2]);
    }
    /* rope lashing on the right post */
    mesh_builder_box(b, id, 0.42f, 0.50f, 0.0f, 0.083f, 0.022f, 0.10f,
                     rope[0], rope[1], rope[2]);
    mesh_builder_box(b, id, 0.42f, 0.56f, 0.0f, 0.083f, 0.018f, 0.10f,
                     rope[0] * 0.8f, rope[1] * 0.8f, rope[2] * 0.8f);
}

static void prop_cart(MeshBuilder *b, C3D_Mtx *id)
{
    static const float wood[3]      = { 0.45f, 0.30f, 0.18f };
    static const float wood_dark[3] = { 0.30f, 0.20f, 0.12f };
    static const float metal[3]     = { 0.33f, 0.31f, 0.29f };
    static const float rust[3]      = { 0.44f, 0.29f, 0.18f };
    C3D_Mtx m;
    int side, along;

    /* open-topped plank body */
    mesh_builder_box(b, id, 0.0f, 0.33f, 0.0f, 0.34f, 0.055f, 0.46f,
                     wood_dark[0], wood_dark[1], wood_dark[2]);
    mesh_builder_box(b, id, 0.0f, 0.47f, 0.0f, 0.32f, 0.13f, 0.44f,
                     wood_dark[0] * 0.75f, wood_dark[1] * 0.75f, wood_dark[2] * 0.75f);
    mesh_builder_box(b, id, -0.315f, 0.50f, 0.0f, 0.035f, 0.15f, 0.46f,
                     wood[0], wood[1], wood[2]);
    mesh_builder_box(b, id, 0.315f, 0.50f, 0.0f, 0.035f, 0.15f, 0.46f,
                     wood[0], wood[1], wood[2]);
    mesh_builder_box(b, id, 0.0f, 0.50f, -0.435f, 0.34f, 0.15f, 0.035f,
                     wood[0], wood[1], wood[2]);
    mesh_builder_box(b, id, 0.0f, 0.50f, 0.435f, 0.34f, 0.15f, 0.035f,
                     wood[0], wood[1], wood[2]);
    /* iron rim + corner straps */
    mesh_builder_box(b, id, 0.0f, 0.655f, 0.0f, 0.355f, 0.018f, 0.475f,
                     metal[0], metal[1], metal[2]);
    for (side = -1; side <= 1; side += 2) {
        for (along = -1; along <= 1; along += 2) {
            mesh_builder_box(b, id, 0.315f * (float)side, 0.50f,
                             0.435f * (float)along,
                             0.045f, 0.155f, 0.045f,
                             metal[0], metal[1], metal[2]);
        }
    }
    /* wheels (cylinder axis rotated from +Y onto X) */
    for (side = -1; side <= 1; side += 2) {
        for (along = -1; along <= 1; along += 2) {
            xf_reset(&m);
            Mtx_Translate(&m, 0.375f * (float)side, 0.19f,
                          0.31f * (float)along, true);
            Mtx_RotateZ(&m, PI_F * 0.5f, true);
            mesh_builder_cyl(b, &m, 0.0f, 0.0f, 0.0f, 0.175f, 0.055f, 8, true,
                             wood_dark[0], wood_dark[1], wood_dark[2]);
            mesh_builder_cyl(b, &m, 0.0f, 0.030f * (float)side, 0.0f, 0.062f,
                             0.09f, 6, true, rust[0], rust[1], rust[2]);
        }
    }
    /* push handle at the back */
    xf_reset(&m);
    Mtx_Translate(&m, 0.0f, 0.44f, -0.56f, true);
    Mtx_RotateX(&m, -0.16f, true);
    mesh_builder_box(b, &m, -0.19f, 0.0f, 0.0f, 0.022f, 0.022f, 0.17f,
                     metal[0], metal[1], metal[2]);
    mesh_builder_box(b, &m, 0.19f, 0.0f, 0.0f, 0.022f, 0.022f, 0.17f,
                     metal[0], metal[1], metal[2]);
    mesh_builder_box(b, &m, 0.0f, 0.0f, -0.13f, 0.20f, 0.020f, 0.020f,
                     rust[0], rust[1], rust[2]);
}

static void prop_ore(MeshBuilder *b, C3D_Mtx *id)
{
    static const float rock[3]       = { 0.27f, 0.26f, 0.26f };
    static const float rock_dark[3]  = { 0.17f, 0.16f, 0.17f };
    static const float amber[3]      = { 1.00f, 0.70f, 0.26f };
    static const float amber_deep[3] = { 0.62f, 0.38f, 0.12f };
    static const float teal[3]       = { 0.34f, 0.92f, 0.82f };
    C3D_Mtx m;
    int i;

    /* broken rock base */
    mesh_builder_box(b, id, 0.0f, 0.10f, 0.0f, 0.40f, 0.10f, 0.33f,
                     rock[0], rock[1], rock[2]);
    xf_reset(&m);
    Mtx_Translate(&m, 0.16f, 0.15f, 0.08f, true);
    Mtx_RotateZ(&m, 0.34f, true);
    mesh_builder_box(b, &m, 0.0f, 0.0f, 0.0f, 0.20f, 0.075f, 0.16f,
                     rock_dark[0], rock_dark[1], rock_dark[2]);
    xf_reset(&m);
    Mtx_Translate(&m, -0.18f, 0.14f, -0.10f, true);
    Mtx_RotateZ(&m, -0.26f, true);
    mesh_builder_box(b, &m, 0.0f, 0.0f, 0.0f, 0.17f, 0.065f, 0.14f,
                     rock[0] * 0.85f, rock[1] * 0.85f, rock[2] * 0.85f);

    /* crystal shards: one tall amber anchor + amber/teal satellites */
    xf_reset(&m);
    Mtx_Translate(&m, 0.0f, 0.16f, 0.0f, true);
    mesh_builder_cone(b, &m, 0.0f, 0.0f, 0.0f, 0.105f, 0.0f, 0.66f, 3, true,
                      amber[0], amber[1], amber[2]);
    {
        static const float shard_amber[4][3] = {
            { 0.16f, 0.18f, 0.10f, },
            { -0.14f, 0.20f, 0.12f, },
            { 0.10f, 0.22f, -0.14f, },
            { -0.08f, 0.17f, -0.10f, },
        };
        for (i = 0; i < 4; i++) {
            xf_reset(&m);
            Mtx_Translate(&m, shard_amber[i][0], shard_amber[i][1],
                          shard_amber[i][2], true);
            Mtx_RotateZ(&m, 0.30f * (float)(i - 1), true);
            Mtx_RotateX(&m, 0.24f * (float)((i & 1) ? 1 : -1), true);
            mesh_builder_cone(b, &m, 0.0f, 0.0f, 0.0f, 0.062f, 0.0f,
                              0.40f - 0.05f * (float)(i & 1), 3, true,
                              (i == 2) ? teal[0] : amber_deep[0],
                              (i == 2) ? teal[1] : amber_deep[1],
                              (i == 2) ? teal[2] : amber_deep[2]);
        }
    }
    /* little glowing chips at the base */
    for (i = 0; i < 3; i++) {
        float a = 0.8f + 2.1f * (float)i;
        mesh_builder_box(b, id, cosf(a) * 0.26f, 0.10f, sinf(a) * 0.22f,
                         0.035f, 0.028f, 0.030f,
                         (i == 1) ? teal[0] : amber[0],
                         (i == 1) ? teal[1] : amber[1],
                         (i == 1) ? teal[2] : amber[2]);
    }
}

static void prop_lantern(MeshBuilder *b, C3D_Mtx *id)
{
    static const float post[3]  = { 0.29f, 0.21f, 0.14f };
    static const float metal[3] = { 0.24f, 0.22f, 0.20f };
    static const float glow[3]  = { 1.00f, 0.74f, 0.32f };
    static const float dim[3]   = { 0.55f, 0.38f, 0.16f };
    int side;

    /* post + cross arm */
    mesh_builder_box(b, id, 0.0f, 0.46f, 0.0f, 0.052f, 0.46f, 0.052f,
                     post[0], post[1], post[2]);
    mesh_builder_box(b, id, 0.0f, 0.045f, 0.0f, 0.13f, 0.045f, 0.13f,
                     metal[0], metal[1], metal[2]);
    mesh_builder_box(b, id, 0.155f, 0.90f, 0.0f, 0.16f, 0.026f, 0.026f,
                     post[0], post[1], post[2]);
    mesh_builder_box(b, id, 0.305f, 0.865f, 0.0f, 0.012f, 0.030f, 0.012f,
                     metal[0], metal[1], metal[2]);
    /* lantern cage: plates, four corner posts and a glowing core */
    mesh_builder_box(b, id, 0.305f, 0.825f, 0.0f, 0.072f, 0.014f, 0.072f,
                     metal[0], metal[1], metal[2]);
    mesh_builder_box(b, id, 0.305f, 0.625f, 0.0f, 0.072f, 0.014f, 0.072f,
                     metal[0], metal[1], metal[2]);
    for (side = -1; side <= 1; side += 2) {
        mesh_builder_box(b, id, 0.305f, 0.725f, 0.062f * (float)side,
                         0.060f, 0.088f, 0.012f,
                         metal[0], metal[1], metal[2]);
        mesh_builder_box(b, id, 0.305f, 0.725f, 0.062f * (float)side,
                         0.052f, 0.070f, 0.004f,
                         glow[0] * 0.7f, glow[1] * 0.7f, glow[2] * 0.7f);
    }
    mesh_builder_box(b, id, 0.305f, 0.725f, 0.0f, 0.040f, 0.078f, 0.040f,
                     glow[0], glow[1], glow[2]);
    /* cap + hanger ring */
    mesh_builder_cone(b, id, 0.305f, 0.842f, 0.0f, 0.085f, 0.012f, 0.075f, 4,
                      true, metal[0], metal[1], metal[2]);
    mesh_builder_cyl(b, id, 0.305f, 0.856f, 0.0f, 0.045f, 0.012f, 6, true,
                     dim[0], dim[1], dim[2]);
}

void mesh_build_props(Mesh out[PROP_KIND_COUNT], MeshArena *arena,
                      MeshVertex *scratch, int scratch_cap)
{
    MeshBuilder b;
    C3D_Mtx id;
    int kind;

    for (kind = 0; kind < PROP_KIND_COUNT; kind++) {
        mesh_builder_begin(&b, scratch, scratch_cap);
        xf_reset(&id);
        switch (kind) {
        case PROP_CRATE:      prop_crate(&b, &id); break;
        case PROP_BARREL:     prop_barrel(&b, &id); break;
        case PROP_FENCE:      prop_fence(&b, &id); break;
        case PROP_TOMBSTONE:  prop_tombstone(&b, &id); break;
        case PROP_DEADTREE:   prop_deadtree(&b, &id); break;
        case PROP_WELL:       prop_well(&b, &id); break;
        case PROP_GATE:       prop_gate(&b, &id); break;
        case PROP_TIMBER:     prop_timber(&b, &id); break;
        case PROP_CART:       prop_cart(&b, &id); break;
        case PROP_ORE:        prop_ore(&b, &id); break;
        case PROP_LANTERN:    prop_lantern(&b, &id); break;
        default:              prop_crate(&b, &id); break;
        }
        (void)mesh_commit(&b, &out[kind], arena);
    }
}

/* ------------------------------------------------------------------ */
/* pickups                                                            */
/* ------------------------------------------------------------------ */

void mesh_build_pickups(Mesh out[DROP_COUNT], MeshArena *arena,
                        MeshVertex *scratch, int scratch_cap)
{
    MeshBuilder b;
    C3D_Mtx id;
    const float coin_gold[3] = { 0.95f, 0.76f, 0.30f };
    const float coin_rim[3]  = { 0.58f, 0.44f, 0.16f };
    const float sinew_red[3] = { 0.55f, 0.19f, 0.14f };
    const float sinew_bone[3]= { 0.72f, 0.62f, 0.48f };
    const float pouch[3]     = { 0.30f, 0.26f, 0.23f };
    const float pouch_brass[3] = { 0.72f, 0.56f, 0.24f };
    int i;

    /* DROP_NONE: empty mesh */
    memset(&out[DROP_NONE], 0, sizeof(out[DROP_NONE]));

    /* DROP_COIN: vertical disc (spins around Y at draw time) */
    mesh_builder_begin(&b, scratch, scratch_cap);
    xf_reset(&id);
    Mtx_Translate(&id, 0.0f, 0.11f, 0.0f, true);
    Mtx_RotateX(&id, PI_F * 0.5f, true);
    mesh_builder_cyl(&b, &id, 0.0f, 0.0f, 0.0f, 0.085f, 0.022f, 10, true,
                     coin_gold[0], coin_gold[1], coin_gold[2]);
    mesh_builder_cyl(&b, &id, 0.0f, -0.03f, 0.0f, 0.055f, 0.024f, 10, true,
                     coin_rim[0], coin_rim[1], coin_rim[2]);
    Mtx_RotateX(&id, PI_F * 0.5f, true);
    mesh_builder_cyl(&b, &id, 0.0f, 0.0f, 0.0f, 0.055f, 0.026f, 10, true,
                     coin_rim[0], coin_rim[1], coin_rim[2]);
    (void)mesh_commit(&b, &out[DROP_COIN], arena);

    /* DROP_SINEW: a wad of red sinew and bone */
    mesh_builder_begin(&b, scratch, scratch_cap);
    xf_reset(&id);
    for (i = 0; i < 3; i++) {
        C3D_Mtx m;
        xf_reset(&m);
        Mtx_Translate(&m, 0.0f, 0.055f + 0.025f * (float)i, 0.0f, true);
        Mtx_RotateY(&m, 0.7f * (float)i, true);
        Mtx_RotateZ(&m, 0.5f * (float)(i - 1), true);
        mesh_builder_box(&b, &m, 0.0f, 0.0f, 0.0f,
                         0.075f, 0.022f, 0.045f,
                         sinew_red[0], sinew_red[1], sinew_red[2]);
    }
    mesh_builder_cyl(&b, &id, 0.0f, 0.035f, 0.0f, 0.03f, 0.07f, 5, true,
                     sinew_bone[0], sinew_bone[1], sinew_bone[2]);
    (void)mesh_commit(&b, &out[DROP_SINEW], arena);

    /* DROP_AMMO: powder pouch */
    mesh_builder_begin(&b, scratch, scratch_cap);
    xf_reset(&id);
    mesh_builder_box(&b, &id, 0.0f, 0.075f, 0.0f, 0.075f, 0.075f, 0.06f,
                     pouch[0], pouch[1], pouch[2]);
    mesh_builder_cyl(&b, &id, 0.0f, 0.155f, 0.0f, 0.034f, 0.03f, 6, true,
                     pouch_brass[0], pouch_brass[1], pouch_brass[2]);
    mesh_builder_cyl(&b, &id, 0.0f, 0.175f, 0.0f, 0.012f, 0.03f, 5, true,
                     pouch_brass[0], pouch_brass[1], pouch_brass[2]);
    (void)mesh_commit(&b, &out[DROP_AMMO], arena);

    /* DROP_BONE_CHARM: bone-white charm with a red glint (Bone Orchard quest) */
    {
        const float bone_white[3] = { 0.90f, 0.86f, 0.74f };
        const float bone_shadow[3] = { 0.62f, 0.58f, 0.48f };
        const float charm_red[3] = { 1.00f, 0.18f, 0.14f };

        mesh_builder_begin(&b, scratch, scratch_cap);
        xf_reset(&id);
        /* little end-bone with two knobs, standing upright */
        mesh_builder_box(&b, &id, 0.0f, 0.105f, 0.0f, 0.020f, 0.070f, 0.020f,
                         bone_white[0], bone_white[1], bone_white[2]);
        mesh_builder_box(&b, &id, -0.032f, 0.168f, 0.0f, 0.030f, 0.024f, 0.024f,
                         bone_white[0], bone_white[1], bone_white[2]);
        mesh_builder_box(&b, &id, 0.032f, 0.168f, 0.0f, 0.030f, 0.024f, 0.024f,
                         bone_white[0], bone_white[1], bone_white[2]);
        mesh_builder_box(&b, &id, 0.0f, 0.045f, 0.0f, 0.052f, 0.018f, 0.020f,
                         bone_shadow[0], bone_shadow[1], bone_shadow[2]);
        /* rawhide wrap + glowing red charm gem */
        mesh_builder_box(&b, &id, 0.0f, 0.092f, 0.0f, 0.026f, 0.014f, 0.026f,
                         charm_red[0] * 0.5f, charm_red[1] * 0.5f, charm_red[2] * 0.5f);
        mesh_builder_quad(&b, &id, 0.0f, 0.100f, 0.024f, 0.020f, 0.020f,
                          charm_red[0], charm_red[1], charm_red[2]);
        mesh_builder_quad(&b, &id, 0.0f, 0.100f, -0.024f, 0.020f, 0.020f,
                          charm_red[0], charm_red[1], charm_red[2]);
        (void)mesh_commit(&b, &out[DROP_BONE_CHARM], arena);
    }
}

/* ------------------------------------------------------------------ */
/* weapons (grip at origin, barrel towards +Z)                        */
/* ------------------------------------------------------------------ */

static const float W_BRASS[3] = { 0.74f, 0.58f, 0.24f };
static const float W_WOOD[3]  = { 0.33f, 0.20f, 0.11f };
static const float W_WOOD2[3] = { 0.43f, 0.28f, 0.15f };
static const float W_IRON[3]  = { 0.36f, 0.36f, 0.39f };
static const float W_STEEL[3] = { 0.54f, 0.54f, 0.57f };

/* Cylinder along local +Z. */
static void zcyl(MeshBuilder *b, float cx, float cy, float cz,
                 float radius, float length, int sides, bool caps,
                 const float col[3])
{
    C3D_Mtx m;
    xf_reset(&m);
    Mtx_Translate(&m, cx, cy, cz, true);
    Mtx_RotateX(&m, PI_F * 0.5f, true);
    mesh_builder_cyl(b, &m, 0.0f, 0.0f, 0.0f, radius, length, sides, caps,
                     col[0], col[1], col[2]);
}

static void gun_flintlock(MeshBuilder *b)
{
    C3D_Mtx id, m;
    xf_reset(&id);

    /* brass barrel */
    zcyl(b, 0.0f, 0.025f, 0.105f, 0.023f, 0.20f, 6, true, W_BRASS);
    /* wooden stock / grip angled down-back */
    xf_reset(&m);
    Mtx_Translate(&m, 0.0f, -0.035f, -0.035f, true);
    Mtx_RotateX(&m, -0.42f, true);
    mesh_builder_box(b, &m, 0.0f, -0.03f, 0.0f, 0.023f, 0.065f, 0.032f,
                     W_WOOD[0], W_WOOD[1], W_WOOD[2]);
    /* forestock under the barrel */
    mesh_builder_box(b, &id, 0.0f, 0.005f, 0.075f, 0.024f, 0.022f, 0.075f,
                     W_WOOD2[0], W_WOOD2[1], W_WOOD2[2]);
    /* lock / flint hammer */
    mesh_builder_box(b, &id, 0.026f, 0.028f, 0.0f, 0.012f, 0.028f, 0.036f,
                     W_IRON[0], W_IRON[1], W_IRON[2]);
    mesh_builder_box(b, &id, 0.036f, 0.052f, -0.008f, 0.008f, 0.018f, 0.012f,
                     W_STEEL[0], W_STEEL[1], W_STEEL[2]);
    /* trigger guard */
    mesh_builder_box(b, &id, 0.0f, -0.028f, 0.012f, 0.016f, 0.008f, 0.030f,
                     W_BRASS[0], W_BRASS[1], W_BRASS[2]);
    /* brass butt cap */
    mesh_builder_box(b, &id, 0.0f, -0.078f, -0.072f, 0.026f, 0.014f, 0.02f,
                     W_BRASS[0], W_BRASS[1], W_BRASS[2]);
}

static void gun_revolver(MeshBuilder *b)
{
    C3D_Mtx id, m;
    xf_reset(&id);

    /* octagonal barrel */
    zcyl(b, 0.0f, 0.025f, 0.11f, 0.018f, 0.17f, 8, true, W_STEEL);
    /* cylinder drum */
    zcyl(b, 0.0f, 0.018f, 0.045f, 0.036f, 0.06f, 8, true, W_IRON);
    /* frame */
    mesh_builder_box(b, &id, 0.0f, 0.005f, 0.03f, 0.022f, 0.030f, 0.055f,
                     W_IRON[0], W_IRON[1], W_IRON[2]);
    /* grip */
    xf_reset(&m);
    Mtx_Translate(&m, 0.0f, -0.04f, -0.012f, true);
    Mtx_RotateX(&m, -0.62f, true);
    mesh_builder_box(b, &m, 0.0f, -0.045f, 0.0f, 0.021f, 0.062f, 0.028f,
                     W_WOOD[0], W_WOOD[1], W_WOOD[2]);
    /* hammer */
    mesh_builder_box(b, &id, 0.0f, 0.045f, -0.012f, 0.010f, 0.018f, 0.012f,
                     W_STEEL[0], W_STEEL[1], W_STEEL[2]);
    /* trigger guard */
    mesh_builder_box(b, &id, 0.0f, -0.028f, 0.022f, 0.014f, 0.007f, 0.026f,
                     W_BRASS[0], W_BRASS[1], W_BRASS[2]);
}

static void gun_musket(MeshBuilder *b)
{
    C3D_Mtx id, m;
    xf_reset(&id);

    /* long barrel */
    zcyl(b, 0.0f, 0.028f, 0.32f, 0.0145f, 0.46f, 6, true, W_IRON);
    /* wooden stock: butt -> forestock */
    xf_reset(&m);
    Mtx_Translate(&m, 0.0f, 0.0f, 0.20f, true);
    mesh_builder_box(b, &m, 0.0f, 0.008f, 0.0f, 0.027f, 0.036f, 0.20f,
                     W_WOOD[0], W_WOOD[1], W_WOOD[2]);
    mesh_builder_box(b, &id, 0.0f, 0.008f, 0.40f, 0.020f, 0.026f, 0.14f,
                     W_WOOD2[0], W_WOOD2[1], W_WOOD2[2]);
    /* butt stock angled down */
    xf_reset(&m);
    Mtx_Translate(&m, 0.0f, -0.045f, -0.02f, true);
    Mtx_RotateX(&m, -0.35f, true);
    mesh_builder_box(b, &m, 0.0f, -0.025f, 0.0f, 0.026f, 0.075f, 0.035f,
                     W_WOOD[0], W_WOOD[1], W_WOOD[2]);
    /* lock plate + hammer */
    mesh_builder_box(b, &id, 0.030f, 0.030f, 0.055f, 0.008f, 0.030f, 0.040f,
                     W_IRON[0], W_IRON[1], W_IRON[2]);
    mesh_builder_box(b, &id, 0.030f, 0.058f, 0.040f, 0.007f, 0.018f, 0.012f,
                     W_STEEL[0], W_STEEL[1], W_STEEL[2]);
    /* trigger guard + ramrod */
    mesh_builder_box(b, &id, 0.0f, -0.020f, 0.055f, 0.016f, 0.007f, 0.036f,
                     W_BRASS[0], W_BRASS[1], W_BRASS[2]);
    zcyl(b, 0.0f, -0.020f, 0.34f, 0.005f, 0.36f, 5, true, W_STEEL);
    /* barrel bands */
    zcyl(b, 0.0f, 0.028f, 0.18f, 0.018f, 0.022f, 6, true, W_STEEL);
    zcyl(b, 0.0f, 0.028f, 0.44f, 0.017f, 0.022f, 6, true, W_STEEL);
}

void mesh_build_weapons(Mesh out[WEAPON_COUNT], MeshArena *arena,
                        MeshVertex *scratch, int scratch_cap)
{
    MeshBuilder b;
    int w;

    for (w = 0; w < WEAPON_COUNT; w++) {
        mesh_builder_begin(&b, scratch, scratch_cap);
        switch (w) {
        case WEAPON_FLINTLOCK: gun_flintlock(&b); break;
        case WEAPON_REVOLVER:  gun_revolver(&b); break;
        case WEAPON_MUSKET:    gun_musket(&b); break;
        default:               gun_flintlock(&b); break;
        }
        (void)mesh_commit(&b, &out[w], arena);
    }
}

/* ------------------------------------------------------------------ */
/* generic meshes                                                     */
/* ------------------------------------------------------------------ */

void mesh_build_quad(Mesh *out, MeshArena *arena,
                     MeshVertex *scratch, int scratch_cap)
{
    MeshBuilder b;
    C3D_Mtx id;

    mesh_builder_begin(&b, scratch, scratch_cap);
    xf_reset(&id);
    mesh_builder_quad(&b, &id, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f,
                      1.0f, 1.0f, 1.0f);
    (void)mesh_commit(&b, out, arena);
}

void mesh_build_shadow_disc(Mesh *out, MeshArena *arena,
                            MeshVertex *scratch, int scratch_cap)
{
    MeshBuilder b;
    C3D_Mtx id;
    const int sides = 12;
    int i;

    mesh_builder_begin(&b, scratch, scratch_cap);
    xf_reset(&id);
    for (i = 0; i < sides; i++) {
        float a0 = (float)i * 2.0f * PI_F / (float)sides;
        float a1 = (float)(i + 1) * 2.0f * PI_F / (float)sides;
        /* normal +Y: wind centre, p(a1), p(a0) */
        push_tri(&b, &id,
                 0.0f, 0.0f, 0.0f,
                 cosf(a1) * 0.5f, 0.0f, sinf(a1) * 0.5f,
                 cosf(a0) * 0.5f, 0.0f, sinf(a0) * 0.5f,
                 1.0f, 1.0f, 1.0f);
    }
    (void)mesh_commit(&b, out, arena);
}

void mesh_build_unit_cube(Mesh *out, MeshArena *arena,
                          MeshVertex *scratch, int scratch_cap)
{
    MeshBuilder b;
    C3D_Mtx id;

    mesh_builder_begin(&b, scratch, scratch_cap);
    xf_reset(&id);
    mesh_builder_box(&b, &id, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 0.5f,
                     0.9f, 0.9f, 0.9f);
    (void)mesh_commit(&b, out, arena);
}

/* ------------------------------------------------------------------ */
/* hero rig                                                           */
/* ------------------------------------------------------------------ */

static const float H_FUR[3]       = { 0.58f, 0.32f, 0.16f };
static const float H_FUR_DARK[3]  = { 0.42f, 0.22f, 0.10f };
static const float H_CREAM[3]     = { 0.90f, 0.84f, 0.68f };
static const float H_CREAM_DARK[3]= { 0.70f, 0.62f, 0.48f };
static const float H_COAT[3]      = { 0.33f, 0.26f, 0.20f };
static const float H_COAT_DARK[3] = { 0.20f, 0.16f, 0.13f };
static const float H_TROUSER[3]   = { 0.36f, 0.31f, 0.25f };
static const float H_TROUSER_D[3] = { 0.27f, 0.24f, 0.19f };
static const float H_BOOT[3]      = { 0.22f, 0.18f, 0.15f };
static const float H_BRASS[3]     = { 0.80f, 0.62f, 0.26f };
static const float H_HAT[3]       = { 0.32f, 0.26f, 0.20f };
static const float H_HATBAND[3]   = { 0.46f, 0.18f, 0.13f };
static const float H_EYE[3]       = { 1.10f, 0.82f, 0.30f };
static const float H_NOSE[3]      = { 0.05f, 0.045f, 0.045f };

static void part_commit(HeroRig *rig, int idx, MeshBuilder *b, MeshArena *arena,
                        int parent, int channel, float px, float py, float pz)
{
    (void)mesh_commit(b, &rig->part[idx].mesh, arena);
    rig->part[idx].parent = (int8_t)parent;
    rig->part[idx].channel = (uint8_t)channel;
    rig->part[idx].pivot = vec3_make(px, py, pz);
}

static void hero_hips(MeshBuilder *b, C3D_Mtx *id)
{
    mesh_builder_box(b, id, 0.0f, 0.0f, 0.0f, 0.15f, 0.11f, 0.10f,
                     H_TROUSER[0], H_TROUSER[1], H_TROUSER[2]);
    mesh_builder_box(b, id, 0.0f, 0.09f, 0.0f, 0.155f, 0.03f, 0.105f,
                     H_COAT_DARK[0], H_COAT_DARK[1], H_COAT_DARK[2]);
    mesh_builder_box(b, id, 0.0f, 0.09f, 0.104f, 0.03f, 0.022f, 0.008f,
                     H_BRASS[0], H_BRASS[1], H_BRASS[2]);
    /* left hip holster */
    mesh_builder_box(b, id, -0.165f, -0.055f, 0.01f, 0.035f, 0.075f, 0.045f,
                     H_COAT_DARK[0], H_COAT_DARK[1], H_COAT_DARK[2]);
}

static void hero_torso(MeshBuilder *b, C3D_Mtx *id)
{
    int i;
    mesh_builder_box(b, id, 0.0f, 0.15f, 0.0f, 0.18f, 0.16f, 0.115f,
                     H_COAT[0], H_COAT[1], H_COAT[2]);
    mesh_builder_box(b, id, 0.0f, 0.17f, 0.108f, 0.075f, 0.13f, 0.02f,
                     H_CREAM[0], H_CREAM[1], H_CREAM[2]);
    for (i = 0; i < 3; i++) {
        mesh_builder_box(b, id, 0.0f, 0.075f + 0.07f * (float)i, 0.132f,
                         0.012f, 0.012f, 0.006f,
                         H_BRASS[0], H_BRASS[1], H_BRASS[2]);
    }
    mesh_builder_box(b, id, 0.19f, 0.26f, 0.0f, 0.055f, 0.05f, 0.10f,
                     H_COAT_DARK[0], H_COAT_DARK[1], H_COAT_DARK[2]);
    mesh_builder_box(b, id, -0.19f, 0.26f, 0.0f, 0.055f, 0.05f, 0.10f,
                     H_COAT_DARK[0], H_COAT_DARK[1], H_COAT_DARK[2]);
    mesh_builder_box(b, id, 0.0f, 0.30f, -0.01f, 0.12f, 0.045f, 0.10f,
                     H_COAT_DARK[0], H_COAT_DARK[1], H_COAT_DARK[2]);
    /* pale neck ruff + russet shoulder tufts so the back reads at 240p */
    mesh_builder_box(b, id, 0.0f, 0.335f, -0.01f, 0.135f, 0.035f, 0.095f,
                     H_CREAM[0], H_CREAM[1], H_CREAM[2]);
    mesh_builder_box(b, id, 0.185f, 0.305f, 0.0f, 0.06f, 0.035f, 0.085f,
                     H_FUR[0], H_FUR[1], H_FUR[2]);
    mesh_builder_box(b, id, -0.185f, 0.305f, 0.0f, 0.06f, 0.035f, 0.085f,
                     H_FUR[0], H_FUR[1], H_FUR[2]);
    mesh_builder_box(b, id, 0.0f, 0.02f, -0.10f, 0.16f, 0.13f, 0.025f,
                     H_COAT[0], H_COAT[1], H_COAT[2]);
    {
        /* leather bandolier strap across the chest */
        C3D_Mtx m;
        xf_reset(&m);
        Mtx_Translate(&m, 0.0f, 0.17f, 0.118f, true);
        Mtx_RotateZ(&m, 0.55f, true);
        mesh_builder_box(b, &m, 0.0f, 0.0f, 0.0f, 0.030f, 0.20f, 0.012f,
                         H_COAT_DARK[0], H_COAT_DARK[1], H_COAT_DARK[2]);
        mesh_builder_box(b, &m, 0.0f, 0.055f, 0.010f, 0.014f, 0.014f, 0.010f,
                         H_BRASS[0], H_BRASS[1], H_BRASS[2]);
    }
}

static void hero_coat_tail(MeshBuilder *b, C3D_Mtx *id)
{
    mesh_builder_box(b, id, 0.0f, -0.24f, -0.02f, 0.085f, 0.25f, 0.028f,
                     H_COAT[0], H_COAT[1], H_COAT[2]);
    mesh_builder_box(b, id, 0.0f, -0.475f, -0.02f, 0.085f, 0.02f, 0.03f,
                     H_COAT_DARK[0], H_COAT_DARK[1], H_COAT_DARK[2]);
}

static void hero_head(MeshBuilder *b, C3D_Mtx *id)
{
    C3D_Mtx m;
    int side;

    mesh_builder_box(b, id, 0.0f, 0.08f, 0.0f, 0.11f, 0.095f, 0.10f,
                     H_FUR[0], H_FUR[1], H_FUR[2]);
    mesh_builder_box(b, id, 0.0f, 0.045f, 0.135f, 0.06f, 0.045f, 0.09f,
                     H_CREAM[0], H_CREAM[1], H_CREAM[2]);
    mesh_builder_box(b, id, 0.0f, 0.065f, 0.222f, 0.026f, 0.02f, 0.018f,
                     H_NOSE[0], H_NOSE[1], H_NOSE[2]);
    mesh_builder_box(b, id, 0.0f, -0.012f, 0.10f, 0.045f, 0.02f, 0.06f,
                     H_CREAM_DARK[0], H_CREAM_DARK[1], H_CREAM_DARK[2]);

    for (side = -1; side <= 1; side += 2) {
        /* pointed ears: tall enough to break the hat silhouette */
        xf_reset(&m);
        Mtx_Translate(&m, 0.082f * (float)side, 0.185f, -0.02f, true);
        Mtx_RotateZ(&m, -0.22f * (float)side, true);
        mesh_builder_cone(b, &m, 0.0f, 0.0f, 0.0f, 0.052f, 0.0f, 0.17f, 3, true,
                          H_FUR[0], H_FUR[1], H_FUR[2]);
        mesh_builder_cone(b, &m, 0.0f, 0.012f, 0.014f, 0.030f, 0.0f, 0.11f, 3, true,
                          H_CREAM_DARK[0], H_CREAM_DARK[1], H_CREAM_DARK[2]);
        /* eye + brow */
        mesh_builder_quad(b, id, 0.052f * (float)side, 0.10f, 0.101f,
                          0.020f, 0.013f,
                          H_EYE[0], H_EYE[1], H_EYE[2]);
        mesh_builder_box(b, id, 0.052f * (float)side, 0.126f, 0.10f,
                         0.028f, 0.011f, 0.012f,
                         H_FUR_DARK[0], H_FUR_DARK[1], H_FUR_DARK[2]);
    }

    /* battered hat (small brim so the muzzle stays visible from above) */
    mesh_builder_cyl(b, id, 0.0f, 0.19f, -0.005f, 0.168f, 0.022f, 10, true,
                     H_HAT[0], H_HAT[1], H_HAT[2]);
    mesh_builder_cyl(b, id, 0.0f, 0.205f, -0.005f, 0.108f, 0.03f, 8, false,
                     H_HATBAND[0], H_HATBAND[1], H_HATBAND[2]);
    mesh_builder_cone(b, id, 0.0f, 0.265f, -0.005f, 0.107f, 0.09f, 0.115f, 8,
                      true, H_HAT[0], H_HAT[1], H_HAT[2]);
}

static void hero_arm_upper(MeshBuilder *b, C3D_Mtx *id)
{
    mesh_builder_box(b, id, 0.0f, -0.12f, 0.0f, 0.05f, 0.13f, 0.06f,
                     H_COAT[0], H_COAT[1], H_COAT[2]);
    mesh_builder_box(b, id, 0.0f, -0.235f, 0.004f, 0.056f, 0.025f, 0.066f,
                     H_CREAM_DARK[0], H_CREAM_DARK[1], H_CREAM_DARK[2]);
}

static void hero_arm_fore(MeshBuilder *b, C3D_Mtx *id)
{
    mesh_builder_box(b, id, 0.0f, -0.09f, 0.004f, 0.042f, 0.115f, 0.05f,
                     H_COAT_DARK[0], H_COAT_DARK[1], H_COAT_DARK[2]);
}

static void hero_hand(MeshBuilder *b, C3D_Mtx *id)
{
    mesh_builder_box(b, id, 0.0f, -0.03f, 0.02f, 0.04f, 0.04f, 0.045f,
                     H_FUR[0], H_FUR[1], H_FUR[2]);
    mesh_builder_box(b, id, 0.0f, -0.07f, 0.045f, 0.035f, 0.028f, 0.035f,
                     H_FUR_DARK[0], H_FUR_DARK[1], H_FUR_DARK[2]);
}

static void hero_thigh(MeshBuilder *b, C3D_Mtx *id)
{
    mesh_builder_box(b, id, 0.0f, -0.16f, 0.005f, 0.065f, 0.15f, 0.075f,
                     H_TROUSER[0], H_TROUSER[1], H_TROUSER[2]);
}

static void hero_shin(MeshBuilder *b, C3D_Mtx *id)
{
    mesh_builder_box(b, id, 0.0f, -0.15f, 0.01f, 0.05f, 0.15f, 0.06f,
                     H_TROUSER_D[0], H_TROUSER_D[1], H_TROUSER_D[2]);
    mesh_builder_box(b, id, 0.0f, -0.29f, 0.01f, 0.06f, 0.025f, 0.07f,
                     H_COAT_DARK[0], H_COAT_DARK[1], H_COAT_DARK[2]);
}

static void hero_foot(MeshBuilder *b, C3D_Mtx *id)
{
    mesh_builder_box(b, id, 0.0f, -0.03f, 0.035f, 0.055f, 0.04f, 0.095f,
                     H_BOOT[0], H_BOOT[1], H_BOOT[2]);
    mesh_builder_box(b, id, 0.0f, -0.045f, 0.125f, 0.05f, 0.032f, 0.045f,
                     H_FUR_DARK[0], H_FUR_DARK[1], H_FUR_DARK[2]);
}

static void hero_tail_seg(MeshBuilder *b, C3D_Mtx *id,
                          float cy, float cz, float hx, float hy, float hz,
                          const float col[3])
{
    mesh_builder_box(b, id, 0.0f, cy, cz, hx, hy, hz, col[0], col[1], col[2]);
}

/* ------------------------------------------------------------------ */
/* wolf head / tail attachments (imported-body hero)                  */
/* ------------------------------------------------------------------ */

void mesh_build_wolf_head(Mesh *out, MeshArena *arena,
                          MeshVertex *scratch, int scratch_cap)
{
    MeshBuilder b;
    C3D_Mtx id, m;
    int side;

    mesh_builder_begin(&b, scratch, scratch_cap);
    xf_reset(&id);

    /* cranium */
    mesh_builder_box(&b, &id, 0.0f, 0.010f, 0.0f, 0.108f, 0.098f, 0.105f,
                     H_FUR[0], H_FUR[1], H_FUR[2]);
    /* brow ridge */
    mesh_builder_box(&b, &id, 0.0f, 0.058f, 0.086f, 0.100f, 0.030f, 0.040f,
                     H_FUR[0], H_FUR[1], H_FUR[2]);
    /* long snout + nose + jaw */
    mesh_builder_box(&b, &id, 0.0f, -0.030f, 0.150f, 0.060f, 0.046f, 0.098f,
                     H_CREAM[0], H_CREAM[1], H_CREAM[2]);
    mesh_builder_box(&b, &id, 0.0f, -0.004f, 0.250f, 0.030f, 0.024f, 0.022f,
                     H_NOSE[0], H_NOSE[1], H_NOSE[2]);
    mesh_builder_box(&b, &id, 0.0f, -0.086f, 0.118f, 0.048f, 0.022f, 0.082f,
                     H_CREAM_DARK[0], H_CREAM_DARK[1], H_CREAM_DARK[2]);

    for (side = -1; side <= 1; side += 2) {
        /* cheek ruff: big angled fur wedge that breaks up the cube silhouette */
        xf_reset(&m);
        Mtx_Translate(&m, 0.095f * (float)side, -0.045f, 0.015f, true);
        Mtx_RotateZ(&m, -0.34f * (float)side, true);
        mesh_builder_box(&b, &m, 0.0f, 0.0f, 0.0f, 0.055f, 0.085f, 0.080f,
                         H_FUR[0], H_FUR[1], H_FUR[2]);
        mesh_builder_box(&b, &m, 0.020f * (float)side, -0.050f, 0.045f,
                         0.045f, 0.045f, 0.050f,
                         H_CREAM_DARK[0], H_CREAM_DARK[1], H_CREAM_DARK[2]);

        /* tall ears, splayed outward well past the hat brim */
        xf_reset(&m);
        Mtx_Translate(&m, 0.112f * (float)side, 0.112f, -0.010f, true);
        Mtx_RotateZ(&m, -0.40f * (float)side, true);
        Mtx_RotateX(&m, -0.10f, true);
        mesh_builder_cone(&b, &m, 0.0f, 0.0f, 0.0f, 0.075f, 0.0f, 0.30f, 3, true,
                          H_FUR[0], H_FUR[1], H_FUR[2]);
        mesh_builder_cone(&b, &m, 0.0f, 0.014f, 0.020f, 0.044f, 0.0f, 0.20f, 3, true,
                          H_CREAM_DARK[0], H_CREAM_DARK[1], H_CREAM_DARK[2]);

        /* amber eyes + dark brows */
        mesh_builder_quad(&b, &id, 0.055f * (float)side, 0.022f, 0.110f,
                          0.024f, 0.016f,
                          H_EYE[0], H_EYE[1], H_EYE[2]);
        mesh_builder_box(&b, &id, 0.055f * (float)side, 0.052f, 0.104f,
                         0.032f, 0.010f, 0.005f,
                         H_FUR_DARK[0], H_FUR_DARK[1], H_FUR_DARK[2]);
    }

    /* battered narrow-brim hat, sitting back so the ears stay clear */
    mesh_builder_cyl(&b, &id, 0.0f, 0.138f, -0.012f, 0.133f, 0.018f, 10, true,
                     H_HAT[0], H_HAT[1], H_HAT[2]);
    mesh_builder_cyl(&b, &id, 0.0f, 0.152f, -0.012f, 0.094f, 0.026f, 8, false,
                     H_HATBAND[0], H_HATBAND[1], H_HATBAND[2]);
    mesh_builder_cone(&b, &id, 0.0f, 0.212f, -0.012f, 0.092f, 0.078f, 0.105f, 8,
                      true, H_HAT[0], H_HAT[1], H_HAT[2]);

    (void)mesh_commit(&b, out, arena);
}

void mesh_build_wolf_tail(Mesh out[3], MeshArena *arena,
                          MeshVertex *scratch, int scratch_cap)
{
    MeshBuilder b;
    C3D_Mtx id;
    int i;

    /* full lengths 0.20 / 0.18 / 0.15 m, widening at the middle and tapering
     * to the cream tip: a ~0.5 m bushy tail. */
    static const float seg_half[3][3] = {
        { 0.058f, 0.062f, 0.100f },
        { 0.078f, 0.083f, 0.090f },
        { 0.048f, 0.052f, 0.075f },
    };

    for (i = 0; i < 3; i++) {
        mesh_builder_begin(&b, scratch, scratch_cap);
        xf_reset(&id);
        mesh_builder_box(&b, &id, 0.0f, 0.0f, -seg_half[i][2],
                         seg_half[i][0], seg_half[i][1], seg_half[i][2],
                         (i == 2) ? H_CREAM[0] : H_FUR[0],
                         (i == 2) ? H_CREAM[1] : H_FUR[1],
                         (i == 2) ? H_CREAM[2] : H_FUR[2]);
        (void)mesh_commit(&b, &out[i], arena);
    }
}
void mesh_build_hero_rig(HeroRig *rig, MeshArena *arena,
                         MeshVertex *scratch, int scratch_cap)
{
    MeshBuilder b;
    C3D_Mtx id;

    memset(rig, 0, sizeof(*rig));
    rig->part_count = HERO_PART_COUNT;
    rig->gun_hand = 10;   /* right hand */
    rig->scale = 1.0f;
    rig->eye_height = 1.56f;

    xf_reset(&id);

    /* 0: hips (root) */
    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_hips(&b, &id);
    part_commit(rig, 0, &b, arena, -1, RIG_CH_NONE, 0.0f, 0.96f, 0.0f);

    /* 1: torso */
    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_torso(&b, &id);
    part_commit(rig, 1, &b, arena, 0, RIG_CH_TORSO, 0.0f, 0.17f, 0.0f);

    /* 2/3: long coat tails */
    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_coat_tail(&b, &id);
    part_commit(rig, 2, &b, arena, 0, RIG_CH_COAT_A, -0.12f, 0.12f, -0.03f);

    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_coat_tail(&b, &id);
    part_commit(rig, 3, &b, arena, 0, RIG_CH_COAT_B, 0.12f, 0.12f, -0.03f);

    /* 4: head */
    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_head(&b, &id);
    part_commit(rig, 4, &b, arena, 1, RIG_CH_HEAD, 0.0f, 0.34f, 0.01f);

    /* 5/6/7: left arm */
    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_arm_upper(&b, &id);
    part_commit(rig, 5, &b, arena, 1, RIG_CH_ARM_A, -0.20f, 0.25f, 0.0f);

    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_arm_fore(&b, &id);
    part_commit(rig, 6, &b, arena, 5, RIG_CH_ELBOW_A, 0.0f, -0.26f, 0.0f);

    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_hand(&b, &id);
    part_commit(rig, 7, &b, arena, 6, RIG_CH_NONE, 0.0f, -0.20f, 0.005f);

    /* 8/9/10: right arm (gun hand) */
    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_arm_upper(&b, &id);
    part_commit(rig, 8, &b, arena, 1, RIG_CH_ARM_B, 0.20f, 0.25f, 0.0f);

    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_arm_fore(&b, &id);
    part_commit(rig, 9, &b, arena, 8, RIG_CH_ELBOW_B, 0.0f, -0.26f, 0.0f);

    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_hand(&b, &id);
    part_commit(rig, 10, &b, arena, 9, RIG_CH_NONE, 0.0f, -0.20f, 0.005f);

    /* 11/12/13: left leg */
    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_thigh(&b, &id);
    part_commit(rig, 11, &b, arena, 0, RIG_CH_LEG_A, -0.09f, -0.04f, 0.0f);

    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_shin(&b, &id);
    part_commit(rig, 12, &b, arena, 11, RIG_CH_KNEE_A, 0.0f, -0.33f, 0.01f);

    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_foot(&b, &id);
    part_commit(rig, 13, &b, arena, 12, RIG_CH_NONE, 0.0f, -0.30f, 0.0f);

    /* 14/15/16: right leg */
    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_thigh(&b, &id);
    part_commit(rig, 14, &b, arena, 0, RIG_CH_LEG_B, 0.09f, -0.04f, 0.0f);

    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_shin(&b, &id);
    part_commit(rig, 15, &b, arena, 14, RIG_CH_KNEE_B, 0.0f, -0.33f, 0.01f);

    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_foot(&b, &id);
    part_commit(rig, 16, &b, arena, 15, RIG_CH_NONE, 0.0f, -0.30f, 0.0f);

    /* 17/18/19: bushy tail */
    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_tail_seg(&b, &id, 0.0f, -0.12f, 0.075f, 0.075f, 0.12f, H_FUR);
    part_commit(rig, 17, &b, arena, 0, RIG_CH_TAIL_A, 0.0f, 0.02f, -0.11f);

    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_tail_seg(&b, &id, -0.02f, -0.11f, 0.115f, 0.115f, 0.11f, H_FUR);
    part_commit(rig, 18, &b, arena, 17, RIG_CH_TAIL_B, 0.0f, 0.0f, -0.19f);

    mesh_builder_begin(&b, scratch, scratch_cap);
    hero_tail_seg(&b, &id, -0.02f, -0.085f, 0.078f, 0.078f, 0.095f, H_CREAM);
    part_commit(rig, 19, &b, arena, 18, RIG_CH_TAIL_B, 0.0f, -0.01f, -0.17f);
}

/* ------------------------------------------------------------------ */
/* zombies: baked pose meshes                                         */
/* ------------------------------------------------------------------ */

#define ZOMBIE_BOX_COUNT 20

typedef struct {
    int8_t  parent;
    uint8_t slot;      /* colour slot: 0 coat, 1 skin, 2 skin dark, 3 rags, 4 boot */
    uint8_t quad;      /* 1 = glowing eye quad, 0 = box */
    Vec3    pivot;
    Vec3    center;
    Vec3    half;
} ZombieBox;

static const ZombieBox zombie_boxes[ZOMBIE_BOX_COUNT] = {
    /* 0 hips */
    { -1, 0, 0, { 0.0f, 0.92f, 0.0f }, { 0.0f, 0.02f, 0.0f }, { 0.16f, 0.10f, 0.12f } },
    /* 1 torso */
    {  0, 0, 0, { 0.0f, 0.06f, 0.0f }, { 0.0f, 0.16f, 0.01f }, { 0.19f, 0.20f, 0.13f } },
    /* 2 head */
    {  1, 1, 0, { 0.0f, 0.32f, 0.02f }, { 0.0f, 0.09f, 0.02f }, { 0.11f, 0.10f, 0.11f } },
    /* 3 jaw */
    {  2, 2, 0, { 0.0f, -0.02f, 0.03f }, { 0.0f, 0.0f, 0.08f }, { 0.065f, 0.03f, 0.075f } },
    /* 4 eye L */
    {  2, 1, 1, { 0.0f, 0.0f, 0.0f }, { -0.050f, 0.105f, 0.138f }, { 0.026f, 0.018f, 0.0f } },
    /* 5 eye R */
    {  2, 1, 1, { 0.0f, 0.0f, 0.0f }, { 0.050f, 0.105f, 0.138f }, { 0.026f, 0.018f, 0.0f } },
    /* 6 arm L upper */
    {  1, 0, 0, { 0.21f, 0.25f, 0.01f }, { 0.0f, -0.16f, 0.0f }, { 0.06f, 0.17f, 0.07f } },
    /* 7 forearm L */
    {  6, 1, 0, { 0.0f, -0.30f, 0.0f }, { 0.0f, -0.14f, 0.02f }, { 0.05f, 0.14f, 0.058f } },
    /* 8 hand L */
    {  7, 2, 0, { 0.0f, -0.27f, 0.0f }, { 0.0f, -0.03f, 0.03f }, { 0.055f, 0.045f, 0.06f } },
    /* 9 arm R upper */
    {  1, 0, 0, { -0.21f, 0.25f, 0.01f }, { 0.0f, -0.16f, 0.0f }, { 0.06f, 0.17f, 0.07f } },
    /* 10 forearm R */
    {  9, 1, 0, { 0.0f, -0.30f, 0.0f }, { 0.0f, -0.14f, 0.02f }, { 0.05f, 0.14f, 0.058f } },
    /* 11 hand R */
    { 10, 2, 0, { 0.0f, -0.27f, 0.0f }, { 0.0f, -0.03f, 0.03f }, { 0.055f, 0.045f, 0.06f } },
    /* 12 thigh L */
    {  0, 3, 0, { 0.10f, -0.05f, 0.0f }, { 0.0f, -0.19f, 0.0f }, { 0.075f, 0.19f, 0.08f } },
    /* 13 shin L */
    { 12, 3, 0, { 0.0f, -0.37f, 0.01f }, { 0.0f, -0.17f, 0.02f }, { 0.06f, 0.17f, 0.065f } },
    /* 14 foot L */
    { 13, 4, 0, { 0.0f, -0.33f, 0.0f }, { 0.0f, -0.03f, 0.06f }, { 0.06f, 0.04f, 0.10f } },
    /* 15 thigh R */
    {  0, 3, 0, { -0.10f, -0.05f, 0.0f }, { 0.0f, -0.19f, 0.0f }, { 0.075f, 0.19f, 0.08f } },
    /* 16 shin R */
    { 15, 3, 0, { 0.0f, -0.37f, 0.01f }, { 0.0f, -0.17f, 0.02f }, { 0.06f, 0.17f, 0.065f } },
    /* 17 foot R */
    { 16, 4, 0, { 0.0f, -0.33f, 0.0f }, { 0.0f, -0.03f, 0.06f }, { 0.06f, 0.04f, 0.10f } },
    /* 18 coat tail L */
    {  0, 3, 0, { 0.09f, -0.06f, 0.0f }, { 0.0f, -0.15f, 0.02f }, { 0.05f, 0.15f, 0.02f } },
    /* 19 coat tail R */
    {  0, 3, 0, { -0.09f, -0.06f, 0.0f }, { 0.0f, -0.15f, 0.02f }, { 0.05f, 0.15f, 0.02f } },
};

static const float zombie_palette[ZOMBIE_COUNT][5][3] = {
    {   /* shambler: rotten browns, sickly green skin */
        { 0.35f, 0.31f, 0.23f }, { 0.60f, 0.68f, 0.46f }, { 0.46f, 0.52f, 0.36f },
        { 0.28f, 0.26f, 0.21f }, { 0.19f, 0.17f, 0.14f }
    },
    {   /* runner: leaner, colder */
        { 0.30f, 0.32f, 0.26f }, { 0.62f, 0.60f, 0.46f }, { 0.50f, 0.48f, 0.38f },
        { 0.25f, 0.25f, 0.22f }, { 0.17f, 0.16f, 0.15f }
    },
    {   /* brute: bulk, dried blood */
        { 0.34f, 0.29f, 0.26f }, { 0.55f, 0.54f, 0.46f }, { 0.44f, 0.43f, 0.37f },
        { 0.26f, 0.23f, 0.21f }, { 0.18f, 0.15f, 0.13f }
    },
};

/* Glowing eye albedo (>1 so it saturates the lighting and reads as glow). */
static const float zombie_eye[ZOMBIE_COUNT][3] = {
    { 1.50f, 2.20f, 1.40f },
    { 2.10f, 2.00f, 1.00f },
    { 2.30f, 1.20f, 0.65f },
};

/* Fills per-part euler rotations + root vertical offset for a pose. */
static void zombie_pose_angles(int type, int pose,
                               Vec3 ang[ZOMBIE_BOX_COUNT], float *bob)
{
    float ph;
    float s, c;
    float hunch = 0.34f;
    float splay = 0.20f;
    int i;

    for (i = 0; i < ZOMBIE_BOX_COUNT; i++)
        ang[i] = vec3_make(0.0f, 0.0f, 0.0f);
    *bob = 0.0f;

    if (type == ZOMBIE_BRUTE) {
        hunch = 0.46f;
        splay = 0.34f;
    } else if (type == ZOMBIE_RUNNER) {
        hunch = 0.28f;
        splay = 0.24f;
    }

    if (pose < ZOMBIE_WALK_FRAMES) {
        ph = (float)pose * 2.0f * PI_F / (float)ZOMBIE_WALK_FRAMES;
        s = sinf(ph);
        c = cosf(ph);
        (void)c;
        *bob = 0.022f * fabsf(s);

        ang[0].y = 0.10f * s;
        ang[1].x = hunch + 0.05f * s;
        ang[1].z = 0.05f * s;
        ang[2].x = 0.20f + 0.09f * sinf(ph + 1.1f);
        ang[2].z = 0.15f * sinf(ph + 0.7f);
        ang[2].y = 0.07f * s;

        /* arms stretched out in front, swaying */
        ang[6].x = -1.02f - 0.06f * s;
        ang[6].z = splay;
        ang[9].x = -1.02f + 0.06f * s;
        ang[9].z = -splay;
        ang[7].x = -0.30f;
        ang[10].x = -0.30f;

        /* legs */
        ang[12].x = -0.34f * s;
        ang[15].x = 0.34f * s;
        ang[13].x = (s > 0.0f) ? 0.48f * s : 0.0f;
        ang[16].x = (s < 0.0f) ? -0.48f * s : 0.0f;
        ang[14].x = -0.10f;
        ang[17].x = -0.10f;
        ang[18].x = 0.18f + 0.22f * sinf(ph + PI_F);
        ang[19].x = 0.18f + 0.22f * sinf(ph + PI_F + 0.5f);
    } else if (pose == ZOMBIE_POSE_LUNGE) {
        ang[1].x = hunch + 0.24f;
        ang[0].y = 0.14f;
        ang[2].x = -0.16f;
        ang[6].x = -1.55f;
        ang[9].x = -1.55f;
        ang[6].z = splay * 1.2f;
        ang[9].z = -splay * 1.2f;
        ang[7].x = -0.45f;
        ang[10].x = -0.45f;
        ang[12].x = -0.52f;
        ang[15].x = 0.34f;
        ang[13].x = 0.30f;
        ang[16].x = 0.18f;
        ang[18].x = 0.45f;
        ang[19].x = 0.45f;
        *bob = 0.02f;
    } else if (pose == ZOMBIE_POSE_STAGGER) {
        ang[1].x = hunch - 0.36f;
        ang[2].x = -0.24f;
        ang[2].z = 0.22f;
        ang[6].x = -0.35f;
        ang[9].x = -0.35f;
        ang[6].z = splay + 0.85f;
        ang[9].z = -(splay + 0.85f);
        ang[7].x = -0.45f;
        ang[10].x = -0.45f;
        ang[12].x = -0.16f;
        ang[15].x = 0.16f;
        ang[13].x = 0.34f;
        ang[16].x = 0.34f;
        ang[18].x = -0.15f;
        ang[19].x = -0.15f;
        *bob = -0.02f;
    } else { /* ZOMBIE_POSE_DEATH: sprawled, the renderer tips the whole body */
        ang[1].x = hunch - 0.20f;
        ang[2].x = 0.34f;
        ang[2].z = 0.30f;
        ang[6].x = -0.25f;
        ang[9].x = -0.25f;
        ang[6].z = splay + 1.05f;
        ang[9].z = -(splay + 1.05f);
        ang[7].x = -0.15f;
        ang[10].x = -0.15f;
        ang[12].x = -0.34f;
        ang[15].x = 0.30f;
        ang[13].x = 0.22f;
        ang[16].x = 0.12f;
        ang[18].x = 0.20f;
        ang[19].x = 0.20f;
    }
}

static void zombie_pose_build(MeshBuilder *b, int type, int pose)
{
    Vec3 ang[ZOMBIE_BOX_COUNT];
    C3D_Mtx world[ZOMBIE_BOX_COUNT];
    float bob;
    int i;

    zombie_pose_angles(type, pose, ang, &bob);

    for (i = 0; i < ZOMBIE_BOX_COUNT; i++) {
        const ZombieBox *def = &zombie_boxes[i];
        C3D_Mtx local;

        xf_reset(&local);
        Mtx_Translate(&local, def->pivot.x, def->pivot.y + (i == 0 ? bob : 0.0f),
                      def->pivot.z, true);
        Mtx_RotateX(&local, ang[i].x, true);
        Mtx_RotateY(&local, ang[i].y, true);
        Mtx_RotateZ(&local, ang[i].z, true);

        if (def->parent >= 0) {
            C3D_Mtx tmp;
            Mtx_Multiply(&tmp, &world[def->parent], &local);
            world[i] = tmp;
        } else {
            world[i] = local;
        }
    }

    for (i = 0; i < ZOMBIE_BOX_COUNT; i++) {
        const ZombieBox *def = &zombie_boxes[i];
        const float *col;

        if (def->quad) {
            col = zombie_eye[type];
            mesh_builder_quad(b, &world[i],
                              def->center.x, def->center.y, def->center.z,
                              def->half.x, def->half.y,
                              col[0], col[1], col[2]);
        } else {
            col = zombie_palette[type][def->slot];
            mesh_builder_box(b, &world[i],
                             def->center.x, def->center.y, def->center.z,
                             def->half.x, def->half.y, def->half.z,
                             col[0], col[1], col[2]);
        }
    }
}

void mesh_build_zombie_poses(Mesh out[ZOMBIE_COUNT][ZOMBIE_POSE_COUNT],
                             MeshArena *arena,
                             MeshVertex *scratch, int scratch_cap)
{
    MeshBuilder b;
    int type, pose;

    for (type = 0; type < ZOMBIE_COUNT; type++) {
        for (pose = 0; pose < ZOMBIE_POSE_COUNT; pose++) {
            mesh_builder_begin(&b, scratch, scratch_cap);
            zombie_pose_build(&b, type, pose);
            (void)mesh_commit(&b, &out[type][pose], arena);
        }
    }
}
