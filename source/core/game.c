/*
 * Gravepowder - portable gameplay simulation
 *
 * This is the whole game minus the pixels and the sound: player movement and
 * collision, the three flintlock-era guns, hitscan, zombies and their states,
 * the drop table and pickups, the ASHEN MILL zone layout, the quest state
 * machine and the camera the renderer consumes.
 *
 * Rules this file lives by:
 *   - plain C99, no <3ds.h>, no malloc: the pool sizes come from include/grave.h
 *   - fixed-step and deterministic: one GraveGame.rng (xorshift32) touched in a
 *     fixed order, so the same seed + the same GraveInput stream replays exactly
 *   - the renderer/UI never call into anything here that mutates state except
 *     the public API below; they only read GraveGame and the camera outputs
 *
 * Numbers (damage, drop rates, zombie stats, zone tuning) live in data.c.
 */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "grave.h"
#include "core_internal.h"

/* hitscan hit volumes: body is an upright box, the head a small sphere sitting
 * just above it, so a high shot can land "head-ish" damage without any real
 * skeletal hitboxes.  The sphere's bottom edge touches the box's top edge, so
 * there is no dead "neck gap" between the two volumes. */
#define GRAVE_BODY_TOP_DROP 0.26f
#define GRAVE_HEAD_DROP     0.13f
#define GRAVE_HEAD_RADIUS   0.13f

#define GRAVE_SHOT_LIFT     0.07f   /* muzzle flash timer */
#define GRAVE_TRACER_KIND   0u
#define GRAVE_IMPACT_KIND   1u

/* ================================================================== */
/* small math                                                          */
/* ================================================================== */

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* yaw 0 = +z; positive yaw rotates the aim direction towards +x, which is
 * screen-LEFT with the renderer's basis (Lead's diagnosis: screen-right is
 * fwd x up = (-cos yaw, 0, sin yaw)).  All "on screen" input signs below are
 * documented against that basis. */
static Vec3 aim_dir(float yaw, float pitch)
{
    float cp = cosf(pitch);
    return vec3_make(sinf(yaw) * cp, sinf(pitch), cosf(yaw) * cp);
}

static Vec3 flat_forward(float yaw)
{
    return vec3_make(sinf(yaw), 0.0f, cosf(yaw));
}

/* Screen-right for the renderer camera: fwd x up (v0.4 axis fix). */
static Vec3 flat_right(float yaw)
{
    return vec3_make(-cosf(yaw), 0.0f, sinf(yaw));
}

static float dist_xz(Vec3 a, Vec3 b)
{
    float dx = b.x - a.x;
    float dz = b.z - a.z;
    return sqrtf(dx * dx + dz * dz);
}

static float yaw_towards(Vec3 from, Vec3 to)
{
    return grave_wrap_pi(atan2f(to.x - from.x, to.z - from.z));
}

/* ================================================================== */
/* deterministic rng (xorshift32)                                      */
/* ================================================================== */

static uint32_t rng_next(GraveGame *game)
{
    uint32_t x = game->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (x == 0u)
        x = 0x9E3779B9u; /* the state must never collapse to zero */
    game->rng = x;
    return x;
}

static float rng_unit(GraveGame *game) /* [0, 1) */
{
    return (float)(rng_next(game) >> 8) * (1.0f / 16777216.0f);
}

static float rng_sym(GraveGame *game) /* [-1, 1) */
{
    return rng_unit(game) * 2.0f - 1.0f;
}

static int rng_below(GraveGame *game, int n) /* 0 .. n-1 */
{
    if (n <= 1)
        return 0;
    return (int)(rng_next(game) % (uint32_t)n);
}

/* ================================================================== */
/* props: collision helpers                                            */
/* ================================================================== */

static void prop_aabb(const Prop *p, Vec3 *mn, Vec3 *mx)
{
    mn->x = p->pos.x - p->half.x;
    mn->y = p->pos.y;
    mn->z = p->pos.z - p->half.z;
    mx->x = p->pos.x + p->half.x;
    mx->y = p->pos.y + 2.0f * p->half.y;
    mx->z = p->pos.z + p->half.z;
}

/* Pushes a horizontal circle out of one prop's footprint.  The arena is flat
 * and the player never jumps in v0.1, so only the XZ footprint matters. */
static bool circle_vs_prop(float *x, float *z, float radius, const Prop *p)
{
    Vec3 mn, mx;
    prop_aabb(p, &mn, &mx);
    float cx = clampf(*x, mn.x, mx.x);
    float cz = clampf(*z, mn.z, mx.z);
    float dx = *x - cx;
    float dz = *z - cz;
    float d2 = dx * dx + dz * dz;
    if (d2 >= radius * radius)
        return false;

    if (d2 > 1.0e-8f) {
        float d = sqrtf(d2);
        *x = cx + (dx / d) * radius;
        *z = cz + (dz / d) * radius;
    } else {
        /* centre inside the box: leave along the shallowest side */
        float px0 = *x - mn.x;
        float px1 = mx.x - *x;
        float pz0 = *z - mn.z;
        float pz1 = mx.z - *z;
        float best = px0;
        int axis = 0; /* 0 = -x, 1 = +x, 2 = -z, 3 = +z */
        if (px1 < best) { best = px1; axis = 1; }
        if (pz0 < best) { best = pz0; axis = 2; }
        if (pz1 < best) { best = pz1; axis = 3; }
        switch (axis) {
        case 0: *x = mn.x - radius; break;
        case 1: *x = mx.x + radius; break;
        case 2: *z = mn.z - radius; break;
        default: *z = mx.z + radius; break;
        }
    }
    return true;
}

static void arena_clamp(const GraveGame *game, float *x, float *z)
{
    const ZoneSpec *zs = grave_zone_spec(game->zone);
    float limit = zs->half_extent - GRAVE_ZONE_BOUND_PAD;
    *x = clampf(*x, -limit, limit);
    *z = clampf(*z, -limit, limit);
}

/* Resolves a horizontal circle against the arena bounds and every prop. */
static void resolve_world(const GraveGame *game, float *x, float *z, float radius)
{
    arena_clamp(game, x, z);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < game->prop_count; i++)
            (void)circle_vs_prop(x, z, radius, &game->props[i]);
        arena_clamp(game, x, z);
    }
}

static bool world_blocked(const GraveGame *game, float x, float z, float radius)
{
    for (int i = 0; i < game->prop_count; i++) {
        Vec3 mn, mx;
        prop_aabb(&game->props[i], &mn, &mx);
        float cx = clampf(x, mn.x, mx.x);
        float cz = clampf(z, mn.z, mx.z);
        float dx = x - cx;
        float dz = z - cz;
        if (dx * dx + dz * dz < radius * radius)
            return true;
    }
    return false;
}

/* Point (3D) inside a prop box, expanded by pad - used to keep the camera out
 * of crates and fences. */
static bool point_blocked(const GraveGame *game, Vec3 p, float pad)
{
    for (int i = 0; i < game->prop_count; i++) {
        Vec3 mn, mx;
        prop_aabb(&game->props[i], &mn, &mx);
        if (p.x >= mn.x - pad && p.x <= mx.x + pad &&
            p.y >= mn.y - pad && p.y <= mx.y + pad &&
            p.z >= mn.z - pad && p.z <= mx.z + pad)
            return true;
    }
    return false;
}

/* ================================================================== */
/* ray tests                                                           */
/* ================================================================== */

static bool ray_slab(float origin, float dir, float mn, float mx,
                     float *t_enter, float *t_exit)
{
    if (fabsf(dir) < 1.0e-8f)
        return origin >= mn && origin <= mx;
    {
        float inv = 1.0f / dir;
        float ta = (mn - origin) * inv;
        float tb = (mx - origin) * inv;
        if (ta > tb) {
            float tmp = ta;
            ta = tb;
            tb = tmp;
        }
        if (ta > *t_enter) *t_enter = ta;
        if (tb < *t_exit)  *t_exit = tb;
        return *t_enter <= *t_exit;
    }
}

static bool ray_aabb(Vec3 origin, Vec3 dir, float tmax, Vec3 mn, Vec3 mx,
                     float *t_out)
{
    float t0 = 0.0f;
    float t1 = tmax;
    if (!ray_slab(origin.x, dir.x, mn.x, mx.x, &t0, &t1)) return false;
    if (!ray_slab(origin.y, dir.y, mn.y, mx.y, &t0, &t1)) return false;
    if (!ray_slab(origin.z, dir.z, mn.z, mx.z, &t0, &t1)) return false;
    if (t0 < 0.0f || t0 > tmax) return false;
    *t_out = t0;
    return true;
}

static bool ray_sphere(Vec3 origin, Vec3 dir, float tmax, Vec3 centre, float radius,
                       float *t_out)
{
    float ox = origin.x - centre.x;
    float oy = origin.y - centre.y;
    float oz = origin.z - centre.z;
    float b = ox * dir.x + oy * dir.y + oz * dir.z;
    float c = ox * ox + oy * oy + oz * oz - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0f) return false;
    {
        float t = -b - sqrtf(disc);
        if (t < 0.0f) return false;
        if (t > tmax) return false;
        *t_out = t;
    }
    return true;
}

/* ================================================================== */
/* shot traces (renderer-only ring of effects)                         */
/* ================================================================== */

static void push_trace(GraveGame *game, Vec3 start, Vec3 end, float life, uint8_t kind)
{
    int slot = -1;
    for (int i = 0; i < MAX_SHOT_TRACES; i++) {
        if (game->shot_traces[i].life <= 0.0f) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* pool full: recycle the shortest-lived entry */
        float weakest = game->shot_traces[0].life;
        slot = 0;
        for (int i = 1; i < MAX_SHOT_TRACES; i++) {
            if (game->shot_traces[i].life < weakest) {
                weakest = game->shot_traces[i].life;
                slot = i;
            }
        }
    }
    game->shot_traces[slot].start = start;
    game->shot_traces[slot].end = end;
    game->shot_traces[slot].life = life;
    game->shot_traces[slot].kind = kind;
}

static void update_shot_traces(GraveGame *game, float dt)
{
    for (int i = 0; i < MAX_SHOT_TRACES; i++) {
        if (game->shot_traces[i].life > 0.0f) {
            game->shot_traces[i].life -= dt;
            if (game->shot_traces[i].life < 0.0f)
                game->shot_traces[i].life = 0.0f;
        }
    }
}

/* ================================================================== */
/* zone layout: ASHEN MILL                                             */
/* ================================================================== */

static void add_prop(GraveGame *game, Vec3 pos, Vec3 half, float yaw, PropKind kind)
{
    if (game->prop_count >= MAX_PROPS)
        return;
    {
        Prop *p = &game->props[game->prop_count];
        p->pos = pos;
        p->half = half;
        p->yaw = yaw;
        p->kind = kind;
        /* The renderer picks a shade from its prop palette; by default the
         * index matches the PropKind so kind 0..7 is all we use. */
        p->color = (uint8_t)kind;
    }
    game->prop_count++;
}

/* Shared 40x40 perimeter wall with the south gate opening; every zone reuses
 * it so the arena bounds stay identical. */
static void build_perimeter(GraveGame *game)
{
    const float half = 20.0f;
    const float fence_half_len = 1.25f;
    const float fence_half_h = 1.05f;
    const float step = 2.5f;

    /* perimeter fence: a south opening (the gate) is the only way in/out */
    for (int i = 0; i < 16; i++) {
        float x = -18.75f + step * (float)i;
        add_prop(game, vec3_make(x, 0.0f, -half),
                 vec3_make(fence_half_len, fence_half_h, 0.10f), 0.0f, PROP_FENCE);
    }
    for (int i = 0; i < 16; i++) {
        float x = -18.75f + step * (float)i;
        if (fabsf(x) < 3.0f)
            continue; /* the gate opening */
        add_prop(game, vec3_make(x, 0.0f, half),
                 vec3_make(fence_half_len, fence_half_h, 0.10f), 0.0f, PROP_FENCE);
    }
    for (int i = 0; i < 16; i++) {
        float z = -18.75f + step * (float)i;
        add_prop(game, vec3_make(-half, 0.0f, z),
                 vec3_make(0.10f, fence_half_h, fence_half_len), 0.0f, PROP_FENCE);
        add_prop(game, vec3_make(half, 0.0f, z),
                 vec3_make(0.10f, fence_half_h, fence_half_len), 0.0f, PROP_FENCE);
    }

    /* gate: two heavy posts framing the opening (decorative; the arena bounds
     * keep the player in the zone in v0.1/v0.2) */
    add_prop(game, vec3_make(-3.0f, 0.0f, half),
             vec3_make(0.22f, 1.70f, 0.22f), 0.0f, PROP_GATE);
    add_prop(game, vec3_make(3.0f, 0.0f, half),
             vec3_make(0.22f, 1.70f, 0.22f), 0.0f, PROP_GATE);
}

static void build_ashen_mill(GraveGame *game)
{
    build_perimeter(game);

    /* the old well, west-ish of centre */
    add_prop(game, vec3_make(-6.5f, 0.0f, 1.5f),
             vec3_make(1.50f, 0.80f, 1.50f), 0.0f, PROP_WELL);

    /* crates: a stack by the east wall, a couple of loose ones */
    add_prop(game, vec3_make(7.5f, 0.0f, -2.0f),
             vec3_make(0.55f, 0.55f, 0.55f), 0.30f, PROP_CRATE);
    add_prop(game, vec3_make(8.8f, 0.0f, -2.8f),
             vec3_make(0.50f, 0.50f, 0.50f), -0.20f, PROP_CRATE);
    add_prop(game, vec3_make(8.2f, 0.0f, -4.2f),
             vec3_make(0.45f, 0.45f, 0.45f), 0.60f, PROP_CRATE);
    add_prop(game, vec3_make(12.5f, 0.0f, 7.5f),
             vec3_make(0.50f, 0.50f, 0.50f), 0.10f, PROP_CRATE);
    add_prop(game, vec3_make(13.4f, 0.0f, 8.3f),
             vec3_make(0.45f, 0.45f, 0.45f), -0.40f, PROP_CRATE);
    add_prop(game, vec3_make(-10.5f, 0.0f, 8.5f),
             vec3_make(0.55f, 0.55f, 0.55f), 0.20f, PROP_CRATE);
    add_prop(game, vec3_make(-9.4f, 0.0f, 9.4f),
             vec3_make(0.45f, 0.45f, 0.45f), 0.50f, PROP_CRATE);

    /* barrels */
    add_prop(game, vec3_make(-11.8f, 0.0f, 9.8f),
             vec3_make(0.42f, 0.60f, 0.42f), 0.0f, PROP_BARREL);
    add_prop(game, vec3_make(4.5f, 0.0f, 10.5f),
             vec3_make(0.42f, 0.60f, 0.42f), 0.0f, PROP_BARREL);
    add_prop(game, vec3_make(5.5f, 0.0f, 11.2f),
             vec3_make(0.38f, 0.55f, 0.38f), 0.0f, PROP_BARREL);
    add_prop(game, vec3_make(14.0f, 0.0f, -1.0f),
             vec3_make(0.42f, 0.60f, 0.42f), 0.0f, PROP_BARREL);
    add_prop(game, vec3_make(-15.5f, 0.0f, -4.0f),
             vec3_make(0.42f, 0.60f, 0.42f), 0.0f, PROP_BARREL);

    /* leaning tombstones along the north wall */
    add_prop(game, vec3_make(-15.0f, 0.0f, -14.0f),
             vec3_make(0.42f, 0.62f, 0.16f), 0.35f, PROP_TOMBSTONE);
    add_prop(game, vec3_make(-11.0f, 0.0f, -15.5f),
             vec3_make(0.38f, 0.55f, 0.15f), -0.25f, PROP_TOMBSTONE);
    add_prop(game, vec3_make(-6.5f, 0.0f, -14.2f),
             vec3_make(0.44f, 0.66f, 0.17f), 0.10f, PROP_TOMBSTONE);
    add_prop(game, vec3_make(-2.0f, 0.0f, -15.6f),
             vec3_make(0.36f, 0.52f, 0.15f), -0.45f, PROP_TOMBSTONE);
    add_prop(game, vec3_make(4.5f, 0.0f, -14.4f),
             vec3_make(0.42f, 0.60f, 0.16f), 0.30f, PROP_TOMBSTONE);
    add_prop(game, vec3_make(10.0f, 0.0f, -15.2f),
             vec3_make(0.38f, 0.56f, 0.15f), -0.15f, PROP_TOMBSTONE);
    add_prop(game, vec3_make(15.0f, 0.0f, -14.0f),
             vec3_make(0.44f, 0.64f, 0.17f), 0.40f, PROP_TOMBSTONE);

    /* dead trees, mostly around the edges */
    add_prop(game, vec3_make(-16.5f, 0.0f, 2.0f),
             vec3_make(0.38f, 2.40f, 0.38f), 0.0f, PROP_DEADTREE);
    add_prop(game, vec3_make(-13.5f, 0.0f, -7.0f),
             vec3_make(0.34f, 2.20f, 0.34f), 0.0f, PROP_DEADTREE);
    add_prop(game, vec3_make(16.0f, 0.0f, 11.0f),
             vec3_make(0.36f, 2.30f, 0.36f), 0.0f, PROP_DEADTREE);
    add_prop(game, vec3_make(17.0f, 0.0f, -9.0f),
             vec3_make(0.34f, 2.10f, 0.34f), 0.0f, PROP_DEADTREE);
    add_prop(game, vec3_make(2.5f, 0.0f, -9.5f),
             vec3_make(0.32f, 2.00f, 0.32f), 0.0f, PROP_DEADTREE);
    add_prop(game, vec3_make(-4.0f, 0.0f, 13.0f),
             vec3_make(0.30f, 1.90f, 0.30f), 0.0f, PROP_DEADTREE);
}

/* BONE ORCHARD: the same 40x40 walled yard, but given over to the dead.  Dense
 * rows of leaning gravestones with clear lanes between them, a ring of dead
 * orchard trees and only a little cover near the gate.  Same PropKinds as the
 * mill, so the renderer needs no new prop art. */
static void build_bone_orchard(GraveGame *game)
{
    build_perimeter(game);

    /* gravestone rows: 5 m apart in z, 3 m in x, centre/entrance kept clear */
    for (int row = 0; row < 6; row++) {
        float z = -15.0f + 5.0f * (float)row;
        for (int col = 0; col < 11; col++) {
            float x = -15.0f + 3.0f * (float)col;
            float tilt;
            if (z > 7.0f && fabsf(x) < 4.5f)
                continue; /* apron in front of the gate */
            /* deterministic scatter of leaning stones */
            tilt = (float)(((row * 7 + col * 5) % 9) - 4) * 0.06f;
            add_prop(game, vec3_make(x, 0.0f, z),
                     vec3_make(0.40f, 0.62f, 0.16f), tilt, PROP_TOMBSTONE);
        }
    }

    /* dead orchard trees, thickest around the walls; kept between the rows */
    add_prop(game, vec3_make(-13.4f, 0.0f, -12.5f),
             vec3_make(0.36f, 2.40f, 0.36f), 0.0f, PROP_DEADTREE);
    add_prop(game, vec3_make(13.4f, 0.0f, -7.5f),
             vec3_make(0.34f, 2.30f, 0.34f), 0.0f, PROP_DEADTREE);
    add_prop(game, vec3_make(-13.4f, 0.0f, 2.5f),
             vec3_make(0.35f, 2.20f, 0.35f), 0.0f, PROP_DEADTREE);
    add_prop(game, vec3_make(13.4f, 0.0f, 7.5f),
             vec3_make(0.33f, 2.25f, 0.33f), 0.0f, PROP_DEADTREE);
    add_prop(game, vec3_make(-4.5f, 0.0f, -12.5f),
             vec3_make(0.32f, 2.10f, 0.32f), 0.0f, PROP_DEADTREE);
    add_prop(game, vec3_make(8.0f, 0.0f, -2.5f),
             vec3_make(0.32f, 2.00f, 0.32f), 0.0f, PROP_DEADTREE);
    add_prop(game, vec3_make(-8.0f, 0.0f, 12.5f),
             vec3_make(0.31f, 2.05f, 0.31f), 0.0f, PROP_DEADTREE);
    add_prop(game, vec3_make(4.5f, 0.0f, 12.5f),
             vec3_make(0.30f, 1.95f, 0.30f), 0.0f, PROP_DEADTREE);
    add_prop(game, vec3_make(-17.0f, 0.0f, 5.0f),
             vec3_make(0.38f, 2.50f, 0.38f), 0.0f, PROP_DEADTREE);
    add_prop(game, vec3_make(17.0f, 0.0f, -12.0f),
             vec3_make(0.36f, 2.35f, 0.36f), 0.0f, PROP_DEADTREE);

    /* a little cover near the entrance, like the gate warden's post */
    add_prop(game, vec3_make(5.0f, 0.0f, 12.8f),
             vec3_make(0.50f, 0.50f, 0.50f), 0.25f, PROP_CRATE);
    add_prop(game, vec3_make(-5.0f, 0.0f, 12.2f),
             vec3_make(0.45f, 0.45f, 0.45f), -0.30f, PROP_CRATE);
    add_prop(game, vec3_make(7.0f, 0.0f, -13.5f),
             vec3_make(0.48f, 0.48f, 0.48f), 0.50f, PROP_CRATE);
    add_prop(game, vec3_make(-12.0f, 0.0f, 7.0f),
             vec3_make(0.42f, 0.60f, 0.42f), 0.0f, PROP_BARREL);
    add_prop(game, vec3_make(12.0f, 0.0f, -7.0f),
             vec3_make(0.40f, 0.58f, 0.40f), 0.0f, PROP_BARREL);
    add_prop(game, vec3_make(0.0f, 0.0f, -17.0f),
             vec3_make(0.38f, 0.56f, 0.38f), 0.0f, PROP_BARREL);
}

/* OLD MINE: a tight mine-works.  A narrow central adit runs from the gate to
 * the back wall, framed by timber supports and walled in by ore/rock stacks
 * with two side chambers.  Carts, lantern posts and ore seams dress the works.
 * The v0.3 PropKinds (timber/cart/ore/lantern) use the same half-extent box
 * collision as every other prop. */
static void build_old_mine(GraveGame *game)
{
    const float adit_x = 2.0f;

    build_perimeter(game); /* timber palisade + gate */

    /* central adit: timber support frames every 5 m, 3.6 m walkway between */
    for (int k = 0; k < 6; k++) {
        float z = 10.0f - 5.0f * (float)k; /* 10, 5, 0, -5, -10, -15 */
        add_prop(game, vec3_make(-adit_x, 0.0f, z),
                 vec3_make(0.22f, 1.80f, 0.22f), 0.0f, PROP_TIMBER);
        add_prop(game, vec3_make(adit_x, 0.0f, z),
                 vec3_make(0.22f, 1.80f, 0.22f), 0.0f, PROP_TIMBER);
    }

    /* ore/rock walls flanking the adit, with two side-chamber entrances */
    for (int k = 0; k < 9; k++) {
        float z = 13.0f - 3.6f * (float)k; /* 13.0 .. -15.8 */
        if (k == 2 || k == 5)
            continue; /* side chambers */
        add_prop(game, vec3_make(-4.6f, 0.0f, z),
                 vec3_make(1.10f, 1.15f, 1.30f), 0.0f, PROP_ORE);
        add_prop(game, vec3_make(4.6f, 0.0f, z),
                 vec3_make(1.10f, 1.15f, 1.30f), 0.0f, PROP_ORE);
    }

    /* back wall ore seam + flanking crystal clusters */
    add_prop(game, vec3_make(0.0f, 0.0f, -17.6f),
             vec3_make(1.60f, 1.30f, 0.90f), 0.0f, PROP_ORE);
    add_prop(game, vec3_make(-9.5f, 0.0f, -14.5f),
             vec3_make(1.30f, 1.05f, 1.10f), 0.4f, PROP_ORE);
    add_prop(game, vec3_make(9.5f, 0.0f, -14.5f),
             vec3_make(1.30f, 1.05f, 1.10f), -0.4f, PROP_ORE);

    /* side chambers: timber stacks and ore */
    add_prop(game, vec3_make(-10.5f, 0.0f, 6.0f),
             vec3_make(0.95f, 1.30f, 0.55f), 0.0f, PROP_TIMBER);
    add_prop(game, vec3_make(10.5f, 0.0f, 6.0f),
             vec3_make(0.95f, 1.30f, 0.55f), 0.0f, PROP_TIMBER);
    add_prop(game, vec3_make(-10.0f, 0.0f, -5.5f),
             vec3_make(0.90f, 1.20f, 0.60f), 0.20f, PROP_TIMBER);
    add_prop(game, vec3_make(10.0f, 0.0f, -5.5f),
             vec3_make(0.90f, 1.20f, 0.60f), -0.20f, PROP_TIMBER);
    add_prop(game, vec3_make(-13.5f, 0.0f, 1.0f),
             vec3_make(1.20f, 1.10f, 1.00f), 0.0f, PROP_ORE);
    add_prop(game, vec3_make(13.5f, 0.0f, 1.0f),
             vec3_make(1.20f, 1.10f, 1.00f), 0.0f, PROP_ORE);

    /* mine carts parked near the works */
    add_prop(game, vec3_make(-8.0f, 0.0f, 9.5f),
             vec3_make(1.00f, 0.70f, 0.80f), 0.3f, PROP_CART);
    add_prop(game, vec3_make(8.0f, 0.0f, 9.5f),
             vec3_make(1.00f, 0.70f, 0.80f), -0.3f, PROP_CART);
    add_prop(game, vec3_make(-11.5f, 0.0f, -10.5f),
             vec3_make(0.95f, 0.70f, 0.75f), 0.1f, PROP_CART);
    add_prop(game, vec3_make(11.5f, 0.0f, -10.5f),
             vec3_make(0.95f, 0.70f, 0.75f), -0.1f, PROP_CART);

    /* lantern posts lighting the adit and the chambers */
    add_prop(game, vec3_make(-3.4f, 0.0f, 12.5f),
             vec3_make(0.14f, 1.35f, 0.14f), 0.0f, PROP_LANTERN);
    add_prop(game, vec3_make(3.4f, 0.0f, 12.5f),
             vec3_make(0.14f, 1.35f, 0.14f), 0.0f, PROP_LANTERN);
    add_prop(game, vec3_make(-3.4f, 0.0f, 2.5f),
             vec3_make(0.14f, 1.35f, 0.14f), 0.0f, PROP_LANTERN);
    add_prop(game, vec3_make(3.4f, 0.0f, 2.5f),
             vec3_make(0.14f, 1.35f, 0.14f), 0.0f, PROP_LANTERN);
    add_prop(game, vec3_make(-3.4f, 0.0f, -7.5f),
             vec3_make(0.14f, 1.35f, 0.14f), 0.0f, PROP_LANTERN);
    add_prop(game, vec3_make(3.4f, 0.0f, -7.5f),
             vec3_make(0.14f, 1.35f, 0.14f), 0.0f, PROP_LANTERN);
    add_prop(game, vec3_make(-9.0f, 0.0f, 0.0f),
             vec3_make(0.14f, 1.35f, 0.14f), 0.0f, PROP_LANTERN);
    add_prop(game, vec3_make(9.0f, 0.0f, 0.0f),
             vec3_make(0.14f, 1.35f, 0.14f), 0.0f, PROP_LANTERN);
}

static void build_zone_layout(GraveGame *game)
{
    const ZoneSpec *zs = grave_zone_spec(game->zone);
    switch (zs->layout) {
    case ZONE_LAYOUT_BONE_ORCHARD:
        build_bone_orchard(game);
        break;
    case ZONE_LAYOUT_OLD_MINE:
        build_old_mine(game);
        break;
    case ZONE_LAYOUT_ASHEN_MILL:
    default:
        build_ashen_mill(game);
        break;
    }
}

/* ================================================================== */
/* pickups / drops                                                     */
/* ================================================================== */

static int find_free_pickup(const GraveGame *game)
{
    for (int i = 0; i < MAX_PICKUPS; i++) {
        if (!game->pickups[i].active)
            return i;
    }
    return -1;
}

static void drop_pickup(GraveGame *game, GraveDrop kind, Vec3 pos)
{
    int slot;
    if (kind == DROP_NONE)
        return;
    slot = find_free_pickup(game);
    if (slot < 0)
        return; /* pool full: the drop is simply lost */
    {
        Pickup *q = &game->pickups[slot];
        float angle = rng_unit(game) * GRAVE_TWO_PI;
        float radius = GRAVE_DROP_SCATTER_MIN +
                       rng_unit(game) * (GRAVE_DROP_SCATTER_MAX - GRAVE_DROP_SCATTER_MIN);
        q->active = true;
        q->kind = kind;
        q->pos.x = pos.x + cosf(angle) * radius;
        q->pos.z = pos.z + sinf(angle) * radius;
        q->pos.y = GRAVE_PICKUP_LIFT;
        arena_clamp(game, &q->pos.x, &q->pos.z);
        q->bob_time = rng_unit(game) * GRAVE_TWO_PI;
        q->life = GRAVE_PICKUP_LIFE;
    }
}

/* Zone-generic drop table: [material A, material B, material A, (powder|A)].
 * Deliberately not a pure random roll: the zone quest needs goal_a of A and
 * goal_b of B, and `kill_goal` kills always drop far more than that on this
 * cycle, so a collect step can never soft-lock on bad luck. */
static GraveDrop choose_drop(GraveGame *game)
{
    const ZoneSpec *zs = grave_zone_spec(game->zone);
    int slot = (game->zombies_killed - 1) & 3;
    switch (slot) {
    case 0:  return zs->material_a;
    case 1:  return zs->material_b;
    case 2:  return zs->material_a;
    default: return (rng_unit(game) < 0.60f) ? DROP_AMMO : zs->material_a;
    }
}

static void update_pickups(GraveGame *game, float dt)
{
    Player *p = &game->player;
    for (int i = 0; i < MAX_PICKUPS; i++) {
        Pickup *q = &game->pickups[i];
        if (!q->active)
            continue;

        q->bob_time += dt;
        q->life -= dt;
        if (q->life <= 0.0f) {
            q->active = false;
            continue;
        }

        if (dist_xz(q->pos, p->transform.pos) > GRAVE_PICKUP_RADIUS)
            continue;

        switch (q->kind) {
        case DROP_COIN:
            game->material_coins++;
            game->run_materials[DROP_COIN]++;
            break;
        case DROP_SINEW:
            game->material_sinew++;
            game->run_materials[DROP_SINEW]++;
            break;
        case DROP_BONE_CHARM:
            game->material_bone_charms++;
            game->run_materials[DROP_BONE_CHARM]++;
            break;
        case DROP_AMMO: {
            const WeaponSpec *spec = grave_weapon_spec(p->weapon);
            int ammo = p->ammo[p->weapon] + GRAVE_AMMO_PICKUP_ROUNDS;
            if (ammo > spec->magazine)
                ammo = spec->magazine;
            p->ammo[p->weapon] = ammo;
            break;
        }
        default:
            break;
        }
        q->active = false;
        game->events |= GRAVE_EVT_PICKUP;
    }
}

/* ================================================================== */
/* player                                                              */
/* ================================================================== */

static void player_look(GraveGame *game, const GraveInput *input, float dt)
{
    Player *p = &game->player;
    float look_x = clampf(input->look_x, -1.0f, 1.0f);
    float look_y = clampf(input->look_y, -1.0f, 1.0f);
    float gyro_yaw = input->gyro_yaw;
    float gyro_pitch = input->gyro_pitch;

    /* Screen conventions (v0.4 axis fix): screen-right is fwd x up, so with
     * yaw increasing towards +x (screen-left), positive look_x / gyro_yaw
     * (turn right on screen) must DECREASE yaw.  look_y positive = drag down =
     * look down; gyro_pitch positive = look up. */
    p->transform.yaw = grave_wrap_pi(p->transform.yaw
                                     - look_x * GRAVE_LOOK_YAW_PER_UNIT
                                     - gyro_yaw * dt);
    p->transform.pitch = clampf(p->transform.pitch
                                    - look_y * GRAVE_LOOK_PITCH_PER_UNIT
                                    + gyro_pitch * dt,
                                -GRAVE_PITCH_LIMIT, GRAVE_PITCH_LIMIT);
}

static void player_move(GraveGame *game, const GraveInput *input, float dt)
{
    Player *p = &game->player;
    float mx = clampf(input->move_x, -1.0f, 1.0f);
    float my = clampf(input->move_y, -1.0f, 1.0f);
    Vec3 fwd = flat_forward(p->transform.yaw);
    Vec3 right = flat_right(p->transform.yaw); /* screen-right */
    float wish_x = fwd.x * my + right.x * mx;
    float wish_z = fwd.z * my + right.z * mx;
    float wish_len = sqrtf(wish_x * wish_x + wish_z * wish_z);
    bool moving = wish_len > 0.001f;
    bool sprint =
        input->sprint && moving && !p->aiming && p->stamina > GRAVE_STAMINA_SPRINT_MIN;
    float target_speed;
    float target_x;
    float target_z;
    float target_len_sq;
    float cur_len_sq;
    float dvx;
    float dvz;
    float dlen;
    float max_dv;
    float accel;
    float old_x;
    float old_z;

    if (wish_len > 1.0f) {
        wish_x /= wish_len;
        wish_z /= wish_len;
    }
    target_speed = sprint ? GRAVE_PLAYER_SPRINT_SPEED : GRAVE_PLAYER_WALK_SPEED;
    if (p->aiming)
        target_speed *= GRAVE_AIM_MOVE_SCALE; /* aiming caps the walk, no sprint */
    target_x = wish_x * target_speed;
    target_z = wish_z * target_speed;

    if (sprint)
        p->stamina = clampf(p->stamina - GRAVE_STAMINA_DRAIN * dt, 0.0f, GRAVE_STAMINA_MAX);
    else
        p->stamina = clampf(p->stamina + GRAVE_STAMINA_REGEN * dt, 0.0f, GRAVE_STAMINA_MAX);

    /* approach the wish velocity instead of snapping to it (v0.4 feel pass) */
    target_len_sq = target_x * target_x + target_z * target_z;
    cur_len_sq = p->vel_x * p->vel_x + p->vel_z * p->vel_z;
    accel = (target_len_sq >= cur_len_sq) ? GRAVE_MOVE_ACCEL : GRAVE_MOVE_DECEL;
    dvx = target_x - p->vel_x;
    dvz = target_z - p->vel_z;
    dlen = sqrtf(dvx * dvx + dvz * dvz);
    max_dv = accel * dt;
    if (dlen > max_dv && dlen > 1.0e-6f) {
        float scale = max_dv / dlen;
        dvx *= scale;
        dvz *= scale;
    }
    p->vel_x += dvx;
    p->vel_z += dvz;
    if (!moving && fabsf(p->vel_x) < 0.01f)
        p->vel_x = 0.0f;
    if (!moving && fabsf(p->vel_z) < 0.01f)
        p->vel_z = 0.0f;

    old_x = p->transform.pos.x;
    old_z = p->transform.pos.z;
    {
        float nx = old_x + p->vel_x * dt;
        float nz = old_z + p->vel_z * dt;
        resolve_world(game, &nx, &nz, GRAVE_PLAYER_RADIUS);
        p->transform.pos.x = nx;
        p->transform.pos.z = nz;
        /* velocity follows the resolved motion so walls do not wind it up */
        p->vel_x = (nx - old_x) / dt;
        p->vel_z = (nz - old_z) / dt;
    }
    p->transform.pos.y = 0.0f;
    p->velocity_y = 0.0f;
    p->on_ground = true;

    {
        float speed = sqrtf(p->vel_x * p->vel_x + p->vel_z * p->vel_z);
        p->anim_speed = (moving && speed > 0.05f)
                            ? clampf(speed / GRAVE_PLAYER_WALK_SPEED, 0.0f, 1.4f)
                            : 0.0f;
    }
    p->anim_time += dt * GRAVE_ANIM_WALK_RATE * p->anim_speed;
    if (p->anim_time > 1000.0f)
        p->anim_time -= 1000.0f;
}

/* ================================================================== */
/* weapons                                                             */
/* ================================================================== */

static float player_reload_time(const Player *p, const WeaponSpec *spec)
{
    float duration = spec->reload_time *
                     (1.0f - GRAVE_RELOAD_TIME_STEP * (float)p->reload_tune);
    if (duration < GRAVE_RELOAD_MIN_TIME)
        duration = GRAVE_RELOAD_MIN_TIME;
    return duration;
}

static void weapon_start_reload(GraveGame *game)
{
    Player *p = &game->player;
    const WeaponSpec *spec = grave_weapon_spec(p->weapon);
    if (p->reload_timer > 0.0f)
        return;
    if (p->ammo[p->weapon] >= spec->magazine)
        return;
    p->reload_duration = player_reload_time(p, spec);
    p->reload_timer = p->reload_duration;
    p->reload_weapon = p->weapon;
    game->events |= GRAVE_EVT_RELOAD;
}

static void weapon_finish_reload(GraveGame *game)
{
    Player *p = &game->player;
    const WeaponSpec *spec = grave_weapon_spec(p->reload_weapon);
    p->ammo[p->reload_weapon] = spec->magazine;
    p->reload_timer = 0.0f;
}

static void hurt_player(GraveGame *game, float damage);

static void damage_zombie(GraveGame *game, int index, float damage)
{
    Zombie *z = &game->zombies[index];
    const ZombieSpec *spec = grave_zombie_spec(z->type);

    z->hp -= damage;
    if (z->type == ZOMBIE_FOREMAN)
        game->boss_hp = z->hp > 0.0f ? z->hp : 0.0f;

    if (z->hp <= 0.0f) {
        z->hp = 0.0f;
        z->state = ZOMBIE_DYING;
        z->death_time = 0.0f;
        z->attack_timer = 0.0f;
        z->speed = 0.0f;
        game->total_kills++;

        if (z->type == ZOMBIE_FOREMAN) {
            /* the Foreman is the mine's final step: fat loot pile, a special
             * event pair, and it does NOT count towards the normal kill goal */
            game->boss_defeated = true;
            game->boss_alive = 0;
            game->boss_hp = 0.0f;
            game->events |= GRAVE_EVT_BOSS_DIE | GRAVE_EVT_ZOMBIE_DIE;
            for (int i = 0; i < GRAVE_BOSS_COIN_DROPS; i++)
                drop_pickup(game, DROP_COIN, z->pos);
            for (int i = 0; i < GRAVE_BOSS_CHARM_DROPS; i++)
                drop_pickup(game, DROP_BONE_CHARM, z->pos);
            return;
        }

        game->zombies_killed++;
        game->events |= GRAVE_EVT_ZOMBIE_DIE;
        drop_pickup(game, choose_drop(game), z->pos);
        return;
    }
    if (rng_unit(game) < spec->stagger_chance) {
        z->state = ZOMBIE_STAGGER;
        z->attack_timer = GRAVE_STAGGER_TIME;
        z->speed = 0.0f;
    }
}

typedef struct {
    bool   hit_zombie;
    int    zombie_index;
    bool   headshot;
    bool   hit_world;
    Vec3   end;
} ShotResult;

static void hitscan(GraveGame *game, Vec3 origin, Vec3 dir, float range, ShotResult *out)
{
    float best = range;
    int best_zombie = -1;
    bool best_head = false;

    out->hit_zombie = false;
    out->zombie_index = -1;
    out->headshot = false;
    out->hit_world = false;

    /* props stop a shot (the fence is waist-high cover) */
    for (int i = 0; i < game->prop_count; i++) {
        Vec3 mn, mx;
        float t;
        prop_aabb(&game->props[i], &mn, &mx);
        if (ray_aabb(origin, dir, best, mn, mx, &t)) {
            best = t;
            best_zombie = -1;
            best_head = false;
            out->hit_world = true;
        }
    }

    /* zombies: body box first, then a small head sphere above it */
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        const Zombie *z = &game->zombies[i];
        const ZombieSpec *spec;
        Vec3 mn, mx, head;
        float t_body;
        float t_head;
        bool hit_body;
        bool hit_head;

        if (!z->active || z->state == ZOMBIE_DYING || z->state == ZOMBIE_DEAD)
            continue;
        spec = grave_zombie_spec(z->type);

        mn.x = z->pos.x - spec->radius;
        mn.y = z->pos.y;
        mn.z = z->pos.z - spec->radius;
        mx.x = z->pos.x + spec->radius;
        mx.y = z->pos.y + spec->body_height - GRAVE_BODY_TOP_DROP;
        mx.z = z->pos.z + spec->radius;
        hit_body = ray_aabb(origin, dir, best, mn, mx, &t_body);
        if (!hit_body)
            t_body = best;

        head = vec3_make(z->pos.x,
                         z->pos.y + spec->body_height - GRAVE_HEAD_DROP,
                         z->pos.z);
        hit_head = ray_sphere(origin, dir, best, head, GRAVE_HEAD_RADIUS, &t_head);

        if (!hit_body && !hit_head)
            continue;
        if (hit_head && (!hit_body || t_head <= t_body + 0.02f)) {
            best = t_head;
            best_zombie = i;
            best_head = true;
        } else {
            best = t_body;
            best_zombie = i;
            best_head = false;
        }
    }

    out->end.x = origin.x + dir.x * best;
    out->end.y = origin.y + dir.y * best;
    out->end.z = origin.z + dir.z * best;
    out->hit_zombie = best_zombie >= 0;
    out->zombie_index = best_zombie;
    out->headshot = best_head;
}

/* Aim assist (v0.4): a held-aim shot that missed gets one forgiving cone test
 * against the nearest zombie inside GRAVE_AIM_ASSIST_RAD of the aim ray, with
 * a clear line of fire and inside weapon range.  Hip-fire never assists. */
static bool aim_assist(GraveGame *game, Vec3 origin, Vec3 dir, float range,
                       ShotResult *shot)
{
    int best = -1;
    float best_dist = range;
    float min_dot = cosf(GRAVE_AIM_ASSIST_RAD);

    for (int i = 0; i < MAX_ZOMBIES; i++) {
        const Zombie *z = &game->zombies[i];
        const ZombieSpec *spec;
        Vec3 to;
        float dist;
        float dot;
        bool blocked = false;

        if (!z->active || z->state == ZOMBIE_DYING || z->state == ZOMBIE_DEAD)
            continue;
        spec = grave_zombie_spec(z->type);
        to.x = z->pos.x - origin.x;
        to.y = (z->pos.y + spec->body_height * 0.55f) - origin.y;
        to.z = z->pos.z - origin.z;
        dist = sqrtf(to.x * to.x + to.y * to.y + to.z * to.z);
        if (dist < 1.0e-4f || dist > range)
            continue;
        dot = (to.x * dir.x + to.y * dir.y + to.z * dir.z) / dist;
        if (dot < min_dot)
            continue;
        {
            Vec3 ray;
            ray.x = to.x / dist;
            ray.y = to.y / dist;
            ray.z = to.z / dist;
            for (int k = 0; k < game->prop_count; k++) {
                Vec3 mn, mx;
                float t;
                prop_aabb(&game->props[k], &mn, &mx);
                if (ray_aabb(origin, ray, dist, mn, mx, &t)) {
                    blocked = true;
                    break;
                }
            }
        }
        if (blocked)
            continue;
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }

    if (best < 0)
        return false;
    shot->hit_zombie = true;
    shot->zombie_index = best;
    shot->headshot = false;
    shot->hit_world = false;
    shot->end.x = origin.x + dir.x * best_dist;
    shot->end.y = origin.y + dir.y * best_dist;
    shot->end.z = origin.z + dir.z * best_dist;
    return true;
}

static void fire_weapon(GraveGame *game)
{
    Player *p = &game->player;
    const WeaponSpec *spec = grave_weapon_spec(p->weapon);
    int pellets = spec->pellets > 0 ? spec->pellets : 1;
    /* the kick from *this* shot must not move this shot's aim; the player
     * carries it into the next one */
    float aim_recoil = p->recoil;
    float spread = spec->spread * (p->aiming ? GRAVE_AIM_SPREAD_SCALE : 1.0f);
    float kick = spec->recoil * (p->aiming ? GRAVE_AIM_RECOIL_SCALE : 1.0f);

    p->ammo[p->weapon] -= 1;
    p->fire_cooldown = spec->fire_interval;
    p->muzzle_flash = GRAVE_SHOT_LIFT;
    p->firing = true;
    game->events |= GRAVE_EVT_SHOOT;

    for (int pellet = 0; pellet < pellets; pellet++) {
        ShotResult shot;
        Vec3 origin = p->transform.pos;
        float yaw = p->transform.yaw + rng_sym(game) * spread;
        float pitch = clampf(p->transform.pitch
                                 + aim_recoil * GRAVE_RECOIL_AIM_SHARE
                                 + rng_sym(game) * spread,
                             -1.55f, 1.55f);
        Vec3 dir = aim_dir(yaw, pitch);

        game->total_shots++;
        origin.y += GRAVE_PLAYER_HIT_HEIGHT;
        origin.x += dir.x * GRAVE_MUZZLE_OFFSET;
        origin.y += dir.y * GRAVE_MUZZLE_OFFSET;
        origin.z += dir.z * GRAVE_MUZZLE_OFFSET;

        hitscan(game, origin, dir, spec->range, &shot);
        if (!shot.hit_zombie && p->aiming)
            (void)aim_assist(game, origin, dir, spec->range, &shot);

        push_trace(game, origin, shot.end, GRAVE_TRACER_LIFE, GRAVE_TRACER_KIND);
        if (shot.hit_zombie) {
            float damage = (float)spec->damage *
                           (1.0f + GRAVE_TUNE_DAMAGE_STEP *
                                       (float)p->weapon_tune[p->weapon]);
            if (shot.headshot)
                damage *= GRAVE_HEADSHOT_MULTIPLIER;
            damage_zombie(game, shot.zombie_index, damage);
            game->total_hits++;
            game->events |= GRAVE_EVT_HIT;
            push_trace(game, shot.end, shot.end, GRAVE_IMPACT_LIFE, GRAVE_IMPACT_KIND);
        } else if (shot.hit_world) {
            push_trace(game, shot.end, shot.end, GRAVE_IMPACT_LIFE, GRAVE_IMPACT_KIND);
        }
    }

    p->recoil = clampf(p->recoil + kick * GRAVE_DEG2RAD, -0.5f, 0.5f);

    if (p->ammo[p->weapon] <= 0)
        weapon_start_reload(game);
}

static void update_weapons(GraveGame *game, const GraveInput *input, float dt)
{
    Player *p = &game->player;

    /* Weapon/camera buttons arrive either as edge taps in GraveInput (the 3DS
     * app does this) or as direct writes to player.weapon /
     * player.camera_first_person (the pause menu does this).  Both are
     * supported; the core never resets the weapon outside these two paths. */
    if ((int)p->weapon < 0 || (int)p->weapon >= WEAPON_COUNT) {
        p->weapon = WEAPON_FLINTLOCK;
        p->reload_timer = 0.0f;
    }
    if (input->swap_weapon) {
        p->weapon = (WeaponId)(((int)p->weapon + 1) % WEAPON_COUNT);
        p->reload_timer = 0.0f; /* swapping deliberately cancels a reload */
        p->reload_weapon = p->weapon;
    }
    if (p->reload_timer > 0.0f && p->weapon != p->reload_weapon)
        p->reload_timer = 0.0f; /* app swapped weapons: the reload is lost */

    /* timers tick first, then this step's trigger pulls */
    if (p->reload_timer > 0.0f) {
        p->reload_timer -= dt;
        if (p->reload_timer <= 0.0f)
            weapon_finish_reload(game);
    }
    if (p->fire_cooldown > 0.0f) {
        p->fire_cooldown -= dt;
        if (p->fire_cooldown < 0.0f)
            p->fire_cooldown = 0.0f;
    }

    if (input->reload)
        weapon_start_reload(game);

    if (input->fire) {
        if (p->reload_timer > 0.0f) {
            /* still reloading: the trigger does nothing */
        } else if (p->ammo[p->weapon] <= 0) {
            game->events |= GRAVE_EVT_EMPTY_GUN;
            p->fire_cooldown = GRAVE_EMPTY_CLICK_COOLDOWN;
            weapon_start_reload(game);
        } else if (p->fire_cooldown <= 0.0f) {
            fire_weapon(game);
        }
    }

    if (p->muzzle_flash > 0.0f) {
        p->muzzle_flash -= dt;
        if (p->muzzle_flash < 0.0f)
            p->muzzle_flash = 0.0f;
    }
    p->firing = p->muzzle_flash > 0.0f;

    /* recoil settles exponentially; snap the last crumb to zero */
    p->recoil -= p->recoil * clampf(dt * GRAVE_RECOIL_RECOVERY, 0.0f, 1.0f);
    if (fabsf(p->recoil) < 0.0005f)
        p->recoil = 0.0f;
}

/* ================================================================== */
/* zombies                                                             */
/* ================================================================== */

static int count_active_zombies(const GraveGame *game)
{
    int count = 0;
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        if (game->zombies[i].active)
            count++;
    }
    return count;
}

static float zombie_attack_reach(const ZombieSpec *spec)
{
    return GRAVE_PLAYER_RADIUS + spec->radius + spec->reach;
}

static void hurt_player(GraveGame *game, float damage)
{
    Player *p = &game->player;
    if (!p->alive)
        return;
    p->hp -= damage;
    game->events |= GRAVE_EVT_HURT;
    if (p->hp <= 0.0f) {
        p->hp = 0.0f;
        p->alive = false;
    }
}

static void update_zombie(GraveGame *game, Zombie *z, float dt)
{
    Player *p = &game->player;
    const ZombieSpec *spec = grave_zombie_spec(z->type);
    float dx = p->transform.pos.x - z->pos.x;
    float dz = p->transform.pos.z - z->pos.z;
    float dist = sqrtf(dx * dx + dz * dz);

    z->yaw = yaw_towards(z->pos, p->transform.pos);

    switch (z->state) {
    case ZOMBIE_IDLE:
        z->speed = 0.0f;
        z->attack_timer -= dt;
        z->anim_time += dt * 0.8f;
        if (z->attack_timer <= 0.0f)
            z->state = ZOMBIE_CHASE;
        break;

    case ZOMBIE_CHASE: {
        float reach = zombie_attack_reach(spec);
        if (dist <= reach) {
            z->state = ZOMBIE_ATTACK;
            z->attack_timer = spec->windup;
            z->speed = 0.0f;
        } else {
            float nx = z->pos.x + (dx / dist) * spec->speed * dt;
            float nz = z->pos.z + (dz / dist) * spec->speed * dt;
            z->speed = spec->speed;
            resolve_world(game, &nx, &nz, spec->radius);
            z->pos.x = nx;
            z->pos.z = nz;
            z->anim_time += dt * spec->anim_rate * (2.2f + spec->speed * 0.8f);
        }
        break;
    }

    case ZOMBIE_ATTACK: {
        float reach = zombie_attack_reach(spec);
        z->speed = 0.0f;
        if (dist > reach + 0.9f) {
            z->state = ZOMBIE_CHASE; /* the player backed off */
            break;
        }
        z->attack_timer -= dt;
        if (z->attack_timer <= 0.0f) {
            if (dist <= reach + 0.35f) {
                hurt_player(game, spec->damage);
                z->anim_time += 1.5f;
            }
            z->attack_timer = spec->attack_interval;
        } else {
            z->anim_time += dt * 1.6f;
        }
        break;
    }

    case ZOMBIE_STAGGER:
        z->speed = 0.0f;
        z->attack_timer -= dt;
        z->anim_time += dt * 1.1f;
        if (z->attack_timer <= 0.0f)
            z->state = ZOMBIE_CHASE;
        break;

    case ZOMBIE_DYING:
        z->speed = 0.0f;
        z->death_time += dt / GRAVE_DYING_TIME;
        z->anim_time += dt;
        if (z->death_time >= 1.0f) {
            z->death_time = 1.0f;
            z->state = ZOMBIE_DEAD;
            z->active = false;
        }
        break;

    case ZOMBIE_DEAD:
    default:
        break;
    }

    if (z->anim_time > 1000.0f)
        z->anim_time -= 1000.0f;
}

static void separate_zombies(GraveGame *game)
{
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *a = &game->zombies[i];
        if (!a->active || a->state == ZOMBIE_DYING || a->state == ZOMBIE_DEAD)
            continue;
        for (int j = i + 1; j < MAX_ZOMBIES; j++) {
            Zombie *b = &game->zombies[j];
            float radius_a, radius_b, min_dist, dx, dz, d2;
            if (!b->active || b->state == ZOMBIE_DYING || b->state == ZOMBIE_DEAD)
                continue;
            radius_a = grave_zombie_spec(a->type)->radius;
            radius_b = grave_zombie_spec(b->type)->radius;
            min_dist = radius_a + radius_b;
            dx = b->pos.x - a->pos.x;
            dz = b->pos.z - a->pos.z;
            d2 = dx * dx + dz * dz;
            if (d2 >= min_dist * min_dist)
                continue;
            {
                float d = sqrtf(d2);
                float overlap = min_dist - d;
                float ux, uz;
                if (d < 1.0e-4f) {
                    /* exactly stacked: fan them out deterministically on x */
                    ux = (j > i) ? 1.0f : -1.0f;
                    uz = 0.0f;
                    d = 1.0f;
                    overlap = min_dist;
                } else {
                    ux = dx / d;
                    uz = dz / d;
                }
                a->pos.x -= ux * overlap * 0.5f * GRAVE_ZOMBIE_SEPARATION;
                a->pos.z -= uz * overlap * 0.5f * GRAVE_ZOMBIE_SEPARATION;
                b->pos.x += ux * overlap * 0.5f * GRAVE_ZOMBIE_SEPARATION;
                b->pos.z += uz * overlap * 0.5f * GRAVE_ZOMBIE_SEPARATION;
            }
        }
    }

    for (int i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *z = &game->zombies[i];
        if (!z->active || z->state == ZOMBIE_DYING || z->state == ZOMBIE_DEAD)
            continue;
        resolve_world(game, &z->pos.x, &z->pos.z, grave_zombie_spec(z->type)->radius);
    }
}

static void update_zombies(GraveGame *game, float dt)
{
    Player *p = &game->player;
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *z = &game->zombies[i];
        if (!z->active)
            continue;
        update_zombie(game, z, dt);
        if (!p->alive)
            break;
    }
    separate_zombies(game);
    game->zombies_alive = count_active_zombies(game);
}

/* ================================================================== */
/* spawner                                                             */
/* ================================================================== */

static int find_free_zombie(const GraveGame *game)
{
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        if (!game->zombies[i].active)
            return i;
    }
    return -1;
}

static bool valid_spawn_spot(GraveGame *game, Vec3 pos, float radius)
{
    Player *p = &game->player;
    if (dist_xz(pos, p->transform.pos) < GRAVE_SPAWN_MIN_PLAYER_DIST)
        return false;
    if (world_blocked(game, pos.x, pos.z, radius + 0.15f))
        return false;
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        const Zombie *z = &game->zombies[i];
        if (!z->active)
            continue;
        if (dist_xz(pos, z->pos) < GRAVE_SPAWN_MIN_ZOMBIE_DIST)
            return false;
    }
    return true;
}

/* Rejection-samples a spawn point on the yard edge / gate that is clear of
 * props, not on top of the player and not stacked on another zombie.  Falls
 * back to a deterministic perimeter sweep, and reports failure instead of ever
 * materialising a zombie inside geometry. */
static bool find_spawn_point(GraveGame *game, float radius, Vec3 *out)
{
    const ZoneSpec *zs = grave_zone_spec(game->zone);
    float half = zs->half_extent;
    float edge = half - GRAVE_ZONE_BOUND_PAD - 1.6f;

    for (int attempt = 0; attempt < 16; attempt++) {
        Vec3 pos;
        if (rng_unit(game) < 0.30f) {
            /* through the gate */
            pos.x = (rng_unit(game) * 2.0f - 1.0f) * 1.6f;
            pos.z = half - 1.6f;
        } else {
            int side = rng_below(game, 4);
            float along = (rng_unit(game) * 2.0f - 1.0f) * edge;
            switch (side) {
            case 0: pos.x = along;  pos.z = -edge; break;
            case 1: pos.x = along;  pos.z = edge;  break;
            case 2: pos.x = -edge;  pos.z = along; break;
            default: pos.x = edge;  pos.z = along; break;
            }
        }
        pos.y = 0.0f;
        if (valid_spawn_spot(game, pos, radius)) {
            *out = pos;
            return true;
        }
    }

    /* deterministic fallback: walk the perimeter, gate first */
    {
        float t;
        for (int i = 0; i <= 16; i++) {
            t = -edge + 2.0f * edge * (float)i / 16.0f;
            {
                Vec3 candidates[4];
                int k;
                candidates[0] = vec3_make(t, 0.0f, edge);
                candidates[1] = vec3_make(t, 0.0f, -edge);
                candidates[2] = vec3_make(-edge, 0.0f, t);
                candidates[3] = vec3_make(edge, 0.0f, t);
                for (k = 0; k < 4; k++) {
                    if (valid_spawn_spot(game, candidates[k], radius)) {
                        *out = candidates[k];
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static ZombieType choose_zombie_type(GraveGame *game, int spawn_index)
{
    const ZoneSpec *zs = grave_zone_spec(game->zone);
    float roll = rng_unit(game);
    float runner = clampf(zs->runner_share, 0.0f, 0.9f);
    float brute_late = clampf(zs->brute_chance, 0.0f, 0.9f);
    float brute_mid = brute_late * 0.55f;

    /* one early runner guarantees the fast threat shows up in wave one */
    if (spawn_index == 3)
        return ZOMBIE_RUNNER;
    if (spawn_index < 8)
        return (roll < 0.25f) ? ZOMBIE_RUNNER : ZOMBIE_SHAMBLER;
    if (spawn_index >= 18) {
        if (roll < brute_late) return ZOMBIE_BRUTE;
        if (roll < brute_late + runner) return ZOMBIE_RUNNER;
        return ZOMBIE_SHAMBLER;
    }
    if (roll < brute_mid) return ZOMBIE_BRUTE;
    if (roll < brute_mid + runner) return ZOMBIE_RUNNER;
    return ZOMBIE_SHAMBLER;
}

static void spawn_zombie(GraveGame *game, ZombieType type, Vec3 pos)
{
    int slot = find_free_zombie(game);
    const ZombieSpec *spec;
    Zombie *z;
    if (slot < 0)
        return;
    spec = grave_zombie_spec(type);
    z = &game->zombies[slot];
    memset(z, 0, sizeof(*z));
    z->active = true;
    z->type = type;
    z->state = ZOMBIE_IDLE;
    z->pos = pos;
    z->pos.y = 0.0f;
    z->yaw = yaw_towards(pos, game->player.transform.pos);
    z->hp = spec->hp;
    z->max_hp = spec->hp;
    z->speed = 0.0f;
    z->attack_timer = GRAVE_IDLE_TIME_MIN +
                      rng_unit(game) * (GRAVE_IDLE_TIME_MAX - GRAVE_IDLE_TIME_MIN);
    z->anim_time = rng_unit(game) * GRAVE_TWO_PI;
    z->death_time = 0.0f;
    z->drop = DROP_NONE;
}

/* Spawns the OLD MINE boss exactly once per run, after the kill threshold.
 * The Foreman uses the normal zombie pool/model, just scaled up by its spec.
 * When the Foreman arrives the rabble scatters: any remaining normal zombies
 * fall (DYING) so the finale is a boss duel instead of a boss + horde pile-up. */
static void spawn_boss(GraveGame *game)
{
    const ZombieSpec *spec = grave_zombie_spec(ZOMBIE_FOREMAN);
    Vec3 pos;

    if (game->boss_spawned)
        return;
    if (find_free_zombie(game) < 0)
        return;
    if (!find_spawn_point(game, spec->radius, &pos)) {
        /* deterministic fallback: back of the adit, clear of the walls */
        pos = vec3_make(0.0f, 0.0f, -14.0f);
        resolve_world(game, &pos.x, &pos.z, spec->radius);
    }
    spawn_zombie(game, ZOMBIE_FOREMAN, pos);
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        Zombie *z = &game->zombies[i];
        if (!z->active || z->type == ZOMBIE_FOREMAN)
            continue;
        if (z->state == ZOMBIE_DYING || z->state == ZOMBIE_DEAD)
            continue;
        z->state = ZOMBIE_DYING;
        z->death_time = 0.0f;
        z->attack_timer = 0.0f;
        z->speed = 0.0f;
    }
    game->zombies_alive = count_active_zombies(game);
    game->boss_spawned = 1;
    game->boss_alive = 1;
    game->boss_max_hp = spec->hp;
    game->boss_hp = spec->hp;
    game->events |= GRAVE_EVT_BOSS_SPAWN;
}

static void update_spawner(GraveGame *game, float dt)
{
    const ZoneSpec *zs = grave_zone_spec(game->zone);
    int spawned;
    int room_alive;
    int room_pool;
    int count;

    /* the Foreman IS the last wave: while he is on the field the mine stops
     * feeding fresh zombies in, so the boss fight is a boss fight */
    if (game->boss_alive)
        return;

    if (game->spawn_budget <= 0 && game->spawn_wave_left <= 0)
        return;

    /* A wave is queued as a whole (with its guaranteed brutes), then the
     * members are released as the alive cap frees slots.  That keeps
     * "one brute per wave" honest even when only one slot opens at a time. */
    if (game->spawn_wave_left <= 0) {
        game->spawn_timer -= dt;
        if (game->spawn_timer > 0.0f)
            return;
        if (game->spawn_budget <= 0)
            return;
        spawned = zs->spawn_budget - game->spawn_budget;
        count = 3;
        if (spawned >= 10)
            count = 4;
        if (spawned >= 20)
            count = 5;
        if (count > game->spawn_budget)
            count = game->spawn_budget;
        game->spawn_wave_left = count;
        game->spawn_wave_brutes = zs->brute_per_wave;
        if (game->spawn_wave_brutes > count)
            game->spawn_wave_brutes = count;
    }

    room_alive = zs->max_alive - game->zombies_alive;
    room_pool = MAX_ZOMBIES - count_active_zombies(game);
    if (room_alive <= 0 || room_pool <= 0) {
        game->spawn_timer = GRAVE_WAVE_RETRY;
        return;
    }

    count = game->spawn_wave_left;
    if (count > room_alive)
        count = room_alive;
    if (count > room_pool)
        count = room_pool;

    for (int i = 0; i < count; i++) {
        ZombieType type;
        Vec3 pos;
        if (game->spawn_wave_brutes > 0) {
            type = ZOMBIE_BRUTE;
            game->spawn_wave_brutes--;
        } else {
            type = choose_zombie_type(game, zs->spawn_budget - game->spawn_budget);
        }
        if (!find_spawn_point(game, grave_zombie_spec(type)->radius, &pos)) {
            /* no clean spot right now; keep the wave queued and retry shortly */
            game->spawn_timer = GRAVE_WAVE_RETRY;
            break;
        }
        spawn_zombie(game, type, pos);
        game->spawn_budget--;
        game->spawn_wave_left--;
    }
    game->zombies_alive = count_active_zombies(game);
    if (game->spawn_wave_left <= 0)
        game->spawn_timer = zs->wave_interval;
}

/* ================================================================== */
/* world progression                                                   */
/* ================================================================== */

static bool valid_zone(ZoneId zone)
{
    return (int)zone >= 0 && (int)zone < ZONE_COUNT;
}

static void world_mark_complete(GraveGame *game)
{
    World *w = &game->world;
    int zi = (int)game->zone;
    ZoneNode *node;

    if (!valid_zone(game->zone))
        return;
    node = &w->nodes[zi];
    node->unlocked = true;
    node->cleared = true;
    node->clears++;
    if (node->best_time <= 0.0f || game->zone_time < node->best_time)
        node->best_time = game->zone_time;

    /* the valley is a chain: finishing a zone opens the next one */
    if (zi + 1 < ZONE_COUNT)
        w->nodes[zi + 1].unlocked = true;
}

/* ================================================================== */
/* quest                                                               */
/* ================================================================== */

static void update_quest(GraveGame *game)
{
    const ZoneSpec *zs = grave_zone_spec(game->zone);

    if (game->quest.kind == QUEST_NONE || game->quest.kind == QUEST_DONE)
        return;

    for (int guard = 0; guard < 6; guard++) {
        Quest *q = &game->quest;

        if (q->kind == QUEST_CLEAR_ZONE) {
            int target = game->spawn_kill_goal > 0 ? game->spawn_kill_goal : 1;
            q->target = target;
            q->progress = game->zombies_killed < target ? game->zombies_killed : target;
            if (q->progress < target)
                return;
            /* kill step done: now bring back the zone's first material */
            q->kind = QUEST_COLLECT;
            q->material = (int)zs->material_a;
            q->target = zs->goal_a;
            q->progress = game->run_materials[q->material] < q->target
                              ? game->run_materials[q->material] : q->target;
            continue;
        }

        if (q->kind == QUEST_COLLECT) {
            int have = 0;
            if ((int)q->material >= 0 && (int)q->material < DROP_COUNT)
                have = game->run_materials[q->material];
            q->progress = have < q->target ? have : q->target;
            if (q->progress < q->target)
                return;
            if (q->material != (int)zs->material_b) {
                q->material = (int)zs->material_b;
                q->target = zs->goal_b;
                q->progress = game->run_materials[q->material] < q->target
                                  ? game->run_materials[q->material] : q->target;
                continue;
            }
            /* OLD MINE's final stage is the Foreman: the collected goods are
             * not enough until the boss is dead. */
            if (game->zone == ZONE_OLD_MINE && !game->boss_defeated)
                return;
            q->kind = QUEST_DONE;
            /* "clear the job, get paid": the quest payout feeds the trader */
            game->material_coins += zs->reward_coins;
            game->material_sinew += zs->reward_sinew;
            game->material_bone_charms += zs->reward_charms;
            world_mark_complete(game);
            game->events |= GRAVE_EVT_QUEST_DONE | GRAVE_EVT_ZONE_DONE;
            if (game->zone == ZONE_OLD_MINE) {
                game->world_complete = true;
                game->events |= GRAVE_EVT_WORLD_COMPLETE;
            }
            if (game->phase == GAME_PHASE_PLAYING)
                game->phase = GAME_PHASE_QUEST_COMPLETE;
            return;
        }

        return;
    }
}

/* ================================================================== */
/* public API                                                          */
/* ================================================================== */

void grave_game_init(GraveGame *game, uint32_t seed)
{
    if (game == NULL)
        return;
    memset(game, 0, sizeof(*game));
    game->phase = GAME_PHASE_TITLE;
    game->zone = ZONE_ASHEN_MILL;
    game->seed = seed != 0u ? seed : 0x9E3779B9u;
    game->rng = game->seed;
    game->player.max_hp = GRAVE_PLAYER_MAX_HP;
    game->player.hp = GRAVE_PLAYER_MAX_HP;
    game->player.alive = true;
    game->player.stamina = GRAVE_STAMINA_MAX;
    game->player.weapon = WEAPON_FLINTLOCK;
    game->player.reload_weapon = WEAPON_FLINTLOCK;
    game->player.camera_first_person = false;
    for (int i = 0; i < WEAPON_COUNT; i++) {
        game->player.ammo[i] = grave_weapon_spec((WeaponId)i)->magazine;
        game->player.weapon_owned[i] = true; /* all v0.1/v0.2 guns are owned */
    }
    game->quest.kind = QUEST_NONE;
    grave_world_init(game);
    grave_update_camera(game, 400.0f / 240.0f);
}

static float player_max_hp_for(int vitality_level)
{
    int level = vitality_level;
    if (level < 0)
        level = 0;
    if (level > GRAVE_VITALITY_MAX_LEVEL)
        level = GRAVE_VITALITY_MAX_LEVEL;
    return GRAVE_PLAYER_MAX_HP + GRAVE_VITALITY_HP_STEP * (float)level;
}

void grave_game_start_zone(GraveGame *game, ZoneId zone)
{
    const ZoneSpec *zs;
    Player *p;
    bool keep_first_person;
    WeaponId keep_weapon;
    bool keep_owned[WEAPON_COUNT];
    int keep_tune[WEAPON_COUNT];
    int keep_ammo[WEAPON_COUNT];
    int keep_vitality;
    int keep_reload;
    float keep_hp;

    if (game == NULL)
        return;
    if (!valid_zone(zone))
        return;
    if (!game->world.nodes[zone].unlocked)
        return; /* progression gate: locked zones cannot be entered */
    zs = grave_zone_spec(zone);

    /* inventory/upgrades carry across the trail: guns, ammo, trader levels,
     * camera preference and material totals survive a zone start.  Only
     * run-local state resets. */
    keep_first_person = game->player.camera_first_person;
    keep_weapon = game->player.weapon;
    if ((int)keep_weapon < 0 || (int)keep_weapon >= WEAPON_COUNT)
        keep_weapon = WEAPON_FLINTLOCK;
    keep_vitality = game->player.vitality_level;
    keep_reload = game->player.reload_tune;
    keep_hp = game->player.hp;
    for (int i = 0; i < WEAPON_COUNT; i++) {
        keep_owned[i] = game->player.weapon_owned[i];
        keep_tune[i] = game->player.weapon_tune[i];
        keep_ammo[i] = game->player.ammo[i];
    }
    if (!keep_owned[keep_weapon])
        keep_weapon = WEAPON_FLINTLOCK;

    memset(game->zombies, 0, sizeof(game->zombies));
    memset(game->pickups, 0, sizeof(game->pickups));
    memset(game->props, 0, sizeof(game->props));
    memset(game->shot_traces, 0, sizeof(game->shot_traces));
    game->prop_count = 0;

    p = &game->player;
    memset(p, 0, sizeof(*p));
    p->vitality_level = keep_vitality;
    p->reload_tune = keep_reload;
    p->max_hp = player_max_hp_for(keep_vitality);
    /* HP carries between zones (the trader's medkit is the heal), but a dead
     * player retrying a zone starts back at full. */
    p->hp = (keep_hp > 0.0f && keep_hp <= p->max_hp) ? keep_hp : p->max_hp;
    p->alive = true;
    p->stamina = GRAVE_STAMINA_MAX;
    p->weapon = keep_weapon;
    p->reload_weapon = keep_weapon;
    p->camera_first_person = keep_first_person;
    for (int i = 0; i < WEAPON_COUNT; i++) {
        const WeaponSpec *spec = grave_weapon_spec((WeaponId)i);
        p->weapon_owned[i] = keep_owned[i];
        p->weapon_tune[i] = keep_tune[i];
        p->ammo[i] = keep_ammo[i];
        if (p->ammo[i] < 0)
            p->ammo[i] = 0;
        if (p->ammo[i] > spec->magazine)
            p->ammo[i] = spec->magazine;
    }
    if (!p->weapon_owned[WEAPON_FLINTLOCK])
        p->weapon_owned[WEAPON_FLINTLOCK] = true;
    p->transform.pos = vec3_make(0.0f, 0.0f, zs->half_extent - 4.5f);
    p->transform.yaw = GRAVE_PI; /* facing into the zone, gate at our back */
    p->transform.pitch = 0.0f;
    p->on_ground = true;

    game->zone = zone;
    game->phase = GAME_PHASE_PLAYING;
    game->rng = game->seed; /* restart = clean deterministic replay */
    game->quest.kind = QUEST_CLEAR_ZONE;
    game->quest.target = zs->kill_goal;
    game->quest.progress = 0;
    game->quest.material = DROP_NONE;
    memset(game->run_materials, 0, sizeof(game->run_materials));
    game->zombies_alive = 0;
    game->zombies_killed = 0;
    game->zone_time = 0.0f;
    game->frame = 0;
    game->events = 0;
    game->spawn_timer = 1.1f;
    game->spawn_budget = zs->spawn_budget;
    game->spawn_kill_goal = zs->kill_goal;
    game->spawn_wave_left = 0;
    game->spawn_wave_brutes = 0;
    game->boss_spawned = 0;
    game->boss_alive = 0;
    game->boss_defeated = false;
    game->boss_hp = 0.0f;
    game->boss_max_hp = 0.0f;
    game->zone_runs++;

    game->world.current = zone;
    game->world.in_zone = true;

    build_zone_layout(game);
    grave_update_camera(game, 400.0f / 240.0f);
}

void grave_game_step(GraveGame *game, const GraveInput *input, float dt)
{
    GraveInput empty;
    Player *p;

    if (game == NULL)
        return;
    if (dt <= 0.0f)
        return;
    if (dt > 0.25f)
        dt = 0.25f;

    /* Pause / menu / weapon / camera-mode buttons are owned by the app layer
     * (see NOTES.md).  The core only advances the simulation while PLAYING. */
    if (game->phase != GAME_PHASE_PLAYING)
        return;

    if (input == NULL) {
        grave_input_reset(&empty);
        input = &empty;
    }

    p = &game->player;
    game->frame++;
    game->zone_time += dt;
    game->total_time += dt;

    p->aiming = input->aim;
    player_look(game, input, dt);
    if (input->toggle_camera)
        p->camera_first_person = !p->camera_first_person;

    player_move(game, input, dt);
    update_weapons(game, input, dt);
    update_spawner(game, dt);
    update_zombies(game, dt);

    /* the Foreman walks in once the mine's normal dead are thinned out */
    if (game->zone == ZONE_OLD_MINE && !game->boss_spawned &&
        game->zombies_killed >= GRAVE_BOSS_SPAWN_KILLS)
        spawn_boss(game);

    update_shot_traces(game, dt);

    if (!p->alive) {
        if (game->phase == GAME_PHASE_PLAYING)
            game->phase = GAME_PHASE_GAME_OVER;
        return;
    }

    update_pickups(game, dt);
    update_quest(game);
}

void grave_update_camera(GraveGame *game, float aspect)
{
    Player *p;
    float yaw;
    float pitch;
    Vec3 fwd;
    Vec3 right;
    Vec3 eye;
    float limit;

    if (game == NULL)
        return;
    (void)aspect; /* vertical FOV is fixed; the renderer handles the aspect */

    p = &game->player;
    yaw = p->transform.yaw;
    pitch = clampf(p->transform.pitch + p->recoil, -GRAVE_PITCH_LIMIT, GRAVE_PITCH_LIMIT);
    fwd = aim_dir(yaw, pitch);
    right = flat_right(yaw);
    eye = p->transform.pos;

    if (p->camera_first_person) {
        eye.y += GRAVE_PLAYER_EYE_HEIGHT;
        game->camera_pos = eye;
        game->camera_look = vec3_make(eye.x + fwd.x * 10.0f,
                                      eye.y + fwd.y * 10.0f,
                                      eye.z + fwd.z * 10.0f);
        game->camera_fov = p->aiming ? GRAVE_AIM_FOV : GRAVE_CAM_FOV_FP;
        return;
    }

    {
        Vec3 pivot;
        Vec3 cam;
        float distance = p->aiming ? GRAVE_AIM_TP_DISTANCE : GRAVE_CAM_TP_DISTANCE;
        const ZoneSpec *zs = grave_zone_spec(game->zone);

        pivot = p->transform.pos;
        pivot.y += GRAVE_PLAYER_CAM_PIVOT_Y;
        pivot.x += right.x * GRAVE_CAM_TP_SHOULDER;
        pivot.z += right.z * GRAVE_CAM_TP_SHOULDER;

        cam = vec3_make(pivot.x - fwd.x * distance,
                        pivot.y - fwd.y * distance,
                        pivot.z - fwd.z * distance);

        limit = zs->half_extent - 0.25f;
        for (int iter = 0; iter < 12; iter++) {
            bool bad = cam.y < GRAVE_CAM_NEAR_FLOOR ||
                       fabsf(cam.x) > limit || fabsf(cam.z) > limit ||
                       point_blocked(game, cam, 0.15f);
            if (!bad)
                break;
            distance -= 0.25f;
            if (distance < GRAVE_CAM_MIN_DISTANCE) {
                distance = GRAVE_CAM_MIN_DISTANCE;
                break;
            }
            cam.x = pivot.x - fwd.x * distance;
            cam.y = pivot.y - fwd.y * distance;
            cam.z = pivot.z - fwd.z * distance;
        }
        if (cam.y < GRAVE_CAM_NEAR_FLOOR)
            cam.y = GRAVE_CAM_NEAR_FLOOR;

        game->camera_pos = cam;
        game->camera_look = vec3_make(pivot.x + fwd.x * 12.0f,
                                      pivot.y + fwd.y * 12.0f,
                                      pivot.z + fwd.z * 12.0f);
        game->camera_fov = p->aiming ? GRAVE_AIM_FOV : GRAVE_CAM_FOV_TP;
    }
}

float grave_hero_walk_phase(const GraveGame *game)
{
    if (game == NULL)
        return 0.0f;
    return game->player.anim_time;
}

void grave_hero_root(const GraveGame *game, Vec3 *pos, float *yaw)
{
    if (game == NULL)
        return;
    if (pos != NULL)
        *pos = game->player.transform.pos;
    if (yaw != NULL)
        *yaw = game->player.transform.yaw;
}

/* ================================================================== */
/* v0.2: world progression                                             */
/* ================================================================== */

void grave_world_init(GraveGame *game)
{
    if (game == NULL)
        return;
    memset(&game->world, 0, sizeof(game->world));
    for (int i = 0; i < ZONE_COUNT; i++)
        game->world.nodes[i].zone = (ZoneId)i;
    game->world.nodes[ZONE_ASHEN_MILL].unlocked = true;
    game->world.current = ZONE_ASHEN_MILL;
    game->world.in_zone = false;
}

bool grave_world_unlock(GraveGame *game, ZoneId zone)
{
    if (game == NULL || !valid_zone(zone))
        return false;
    game->world.nodes[zone].unlocked = true;
    return true;
}

bool grave_world_unlocked(const GraveGame *game, ZoneId zone)
{
    if (game == NULL || !valid_zone(zone))
        return false;
    return game->world.nodes[zone].unlocked;
}

bool grave_world_cleared(const GraveGame *game, ZoneId zone)
{
    if (game == NULL || !valid_zone(zone))
        return false;
    return game->world.nodes[zone].cleared;
}

void grave_world_to_trail(GraveGame *game)
{
    if (game == NULL)
        return;
    game->world.in_zone = false;
    game->phase = GAME_PHASE_TITLE;
    game->events = 0u;
}

bool grave_game_travel_to(GraveGame *game, ZoneId zone)
{
    if (game == NULL || !valid_zone(zone))
        return false;
    if (!game->world.nodes[zone].unlocked)
        return false;
    grave_game_start_zone(game, zone);
    return game->world.in_zone && game->zone == zone;
}

/* ================================================================== */
/* v0.2: inventory helpers (PACK page)                                 */
/* ================================================================== */

bool grave_inventory_owned(const GraveGame *game, WeaponId id)
{
    if (game == NULL || (int)id < 0 || (int)id >= WEAPON_COUNT)
        return false;
    return game->player.weapon_owned[id];
}

int grave_inventory_weapon_count(const GraveGame *game)
{
    int count = 0;
    if (game == NULL)
        return 0;
    for (int i = 0; i < WEAPON_COUNT; i++) {
        if (game->player.weapon_owned[i])
            count++;
    }
    return count;
}

int grave_inventory_ammo(const GraveGame *game, WeaponId id)
{
    if (game == NULL || (int)id < 0 || (int)id >= WEAPON_COUNT)
        return 0;
    return game->player.ammo[id];
}

int grave_inventory_material(const GraveGame *game, GraveDrop drop)
{
    if (game == NULL)
        return 0;
    switch (drop) {
    case DROP_COIN:       return game->material_coins;
    case DROP_SINEW:      return game->material_sinew;
    case DROP_BONE_CHARM: return game->material_bone_charms;
    default:              return 0;
    }
}

/* ================================================================== */
/* v0.2: save data                                                     */
/* ================================================================== */

/* If a future field changes the struct size, this stops compiling instead of
 * silently writing a different number of bytes than GRAVE_SAVE_SIZE says. */
typedef char grave_save_size_check[(sizeof(SaveData) == GRAVE_SAVE_SIZE) ? 1 : -1];

static uint32_t save_checksum(const SaveData *sd)
{
    const unsigned char *bytes = (const unsigned char *)sd;
    uint32_t hash = 2166136261u;

    /* the checksum field itself is skipped; everything after it is covered */
    for (size_t i = offsetof(SaveData, unlocked_mask); i < sizeof(SaveData); i++) {
        hash ^= (uint32_t)bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

int grave_save_size(void)
{
    return (int)sizeof(SaveData);
}

int grave_save_to_buffer(const GraveGame *game, const GraveSettings *settings,
                         void *buf, int buf_size)
{
    SaveData sd;

    if (game == NULL || buf == NULL)
        return 0;
    if (buf_size < (int)sizeof(SaveData))
        return 0;

    memset(&sd, 0, sizeof(sd)); /* deterministic image: padding stays zero */
    sd.magic = GRAVE_SAVE_MAGIC;
    sd.version = (uint16_t)GRAVE_SAVE_VERSION;
    sd.bytes = (uint16_t)sizeof(SaveData);

    for (int i = 0; i < ZONE_COUNT; i++) {
        const ZoneNode *node = &game->world.nodes[i];
        if (node->unlocked)
            sd.unlocked_mask |= 1u << i;
        if (node->cleared) {
            sd.cleared_mask |= 1u << i;
            sd.unlocked_mask |= 1u << i;
        }
        sd.zone_clears[i] = (uint8_t)(node->clears < 0 ? 0
                                      : (node->clears > 255 ? 255 : node->clears));
        sd.zone_best_time[i] = node->best_time > 0.0f ? node->best_time : 0.0f;
    }
    sd.current_zone = valid_zone(game->world.current) ? (uint8_t)game->world.current
                                                     : (uint8_t)ZONE_ASHEN_MILL;
    sd.camera_first_person = game->player.camera_first_person ? 1u : 0u;
    if (settings != NULL) {
        sd.invert_y = settings->invert_y ? 1u : 0u;
        sd.touch_sens = settings->touch_sens;
        sd.muted = settings->muted ? 1u : 0u;
        /* v0.4 gyro settings live inside SaveData.reserved[0..1] so the save
         * version/layout stay unchanged (old images = ON / MED defaults). */
        sd.reserved[0] = settings->gyro_mode <= (uint8_t)GRAVE_GYRO_INVERT
                             ? settings->gyro_mode : (uint8_t)GRAVE_GYRO_ON;
        sd.reserved[1] = settings->gyro_sens <= (uint8_t)GRAVE_GYRO_SENS_HIGH
                             ? settings->gyro_sens : (uint8_t)GRAVE_GYRO_SENS_MED;
    }

    for (int i = 0; i < WEAPON_COUNT; i++) {
        const WeaponSpec *spec = grave_weapon_spec((WeaponId)i);
        int ammo = game->player.ammo[i];
        if (ammo < 0) ammo = 0;
        if (ammo > spec->magazine) ammo = spec->magazine;
        sd.weapon_owned[i] = game->player.weapon_owned[i] ? 1u : 0u;
        sd.ammo[i] = ammo;
    }

    sd.material_coins = game->material_coins > 0 ? game->material_coins : 0;
    sd.material_sinew = game->material_sinew > 0 ? game->material_sinew : 0;
    sd.material_bone_charms = game->material_bone_charms > 0
                                  ? game->material_bone_charms : 0;
    sd.kills_total = game->total_kills > 0 ? (uint32_t)game->total_kills : 0u;
    sd.runs = game->zone_runs > 0 ? (uint32_t)game->zone_runs : 0u;
    sd.play_time = game->total_time > 0.0f ? game->total_time : 0.0f;

    /* v0.3: trader upgrades, world completion and accuracy */
    for (int i = 0; i < WEAPON_COUNT; i++) {
        int tune = game->player.weapon_tune[i];
        if (tune < 0) tune = 0;
        if (tune > GRAVE_TUNE_MAX_LEVEL) tune = GRAVE_TUNE_MAX_LEVEL;
        sd.weapon_tune[i] = (uint8_t)tune;
    }
    {
        int vitality = game->player.vitality_level;
        int reload = game->player.reload_tune;
        if (vitality < 0) vitality = 0;
        if (vitality > GRAVE_VITALITY_MAX_LEVEL) vitality = GRAVE_VITALITY_MAX_LEVEL;
        if (reload < 0) reload = 0;
        if (reload > GRAVE_RELOAD_MAX_LEVEL) reload = GRAVE_RELOAD_MAX_LEVEL;
        sd.vitality_level = (uint8_t)vitality;
        sd.reload_tune = (uint8_t)reload;
    }
    sd.world_complete = game->world_complete ? 1u : 0u;
    sd.shots_fired = game->total_shots > 0 ? (uint32_t)game->total_shots : 0u;
    sd.shots_hit = game->total_hits > 0 ? (uint32_t)game->total_hits : 0u;

    sd.checksum = save_checksum(&sd);
    memcpy(buf, &sd, sizeof(sd));
    return (int)sizeof(sd);
}

bool grave_load_from_buffer(GraveGame *game, GraveSettings *settings_out,
                            const void *buf, int buf_size)
{
    SaveData sd;
    World world;
    bool owned[WEAPON_COUNT];
    int ammo[WEAPON_COUNT];
    int owned_count = 0;
    uint32_t valid_mask;
    int weapon;

    if (game == NULL || buf == NULL)
        return false;
    if (buf_size < (int)sizeof(SaveData))
        return false;
    memcpy(&sd, buf, sizeof(sd)); /* buf may be unaligned */

    if (sd.magic != GRAVE_SAVE_MAGIC)
        return false;
    if (sd.version != (uint16_t)GRAVE_SAVE_VERSION)
        return false;
    if (sd.bytes != (uint16_t)sizeof(SaveData))
        return false;
    if (sd.checksum != save_checksum(&sd))
        return false;

    valid_mask = (1u << ZONE_COUNT) - 1u;
    if ((sd.unlocked_mask & ~valid_mask) != 0u ||
        (sd.cleared_mask & ~valid_mask) != 0u)
        return false;

    /* build the world image first; nothing below this point can fail */
    memset(&world, 0, sizeof(world));
    for (int i = 0; i < ZONE_COUNT; i++) {
        ZoneNode *node = &world.nodes[i];
        node->zone = (ZoneId)i;
        node->unlocked = (sd.unlocked_mask & (1u << i)) != 0u;
        node->cleared = (sd.cleared_mask & (1u << i)) != 0u;
        if (node->cleared)
            node->unlocked = true; /* a cleared zone is always travelable */
        node->clears = (int)sd.zone_clears[i];
        node->best_time = sd.zone_best_time[i] > 0.0f ? sd.zone_best_time[i] : 0.0f;
    }
    if (!world.nodes[ZONE_ASHEN_MILL].unlocked) {
        /* keep the valley playable even if a writer forgot the first stop */
        world.nodes[ZONE_ASHEN_MILL].unlocked = true;
        sd.unlocked_mask |= 1u;
    }
    world.current = (sd.current_zone < (uint8_t)ZONE_COUNT &&
                     world.nodes[sd.current_zone].unlocked)
                        ? (ZoneId)sd.current_zone
                        : ZONE_ASHEN_MILL;
    world.in_zone = false;

    for (int i = 0; i < WEAPON_COUNT; i++) {
        const WeaponSpec *spec = grave_weapon_spec((WeaponId)i);
        owned[i] = sd.weapon_owned[i] != 0u;
        if (owned[i])
            owned_count++;
        ammo[i] = (int)sd.ammo[i];
        if (ammo[i] < 0) ammo[i] = 0;
        if (ammo[i] > spec->magazine) ammo[i] = spec->magazine;
    }
    if (owned_count == 0)
        owned[WEAPON_FLINTLOCK] = true;
    weapon = owned[WEAPON_FLINTLOCK] ? (int)WEAPON_FLINTLOCK : -1;
    for (int i = 0; i < WEAPON_COUNT && weapon < 0; i++) {
        if (owned[i])
            weapon = i;
    }
    if (weapon < 0)
        weapon = (int)WEAPON_FLINTLOCK;

    if (settings_out != NULL) {
        memset(settings_out, 0, sizeof(*settings_out));
        settings_out->invert_y = sd.invert_y ? 1u : 0u;
        settings_out->touch_sens = sd.touch_sens;
        settings_out->muted = sd.muted ? 1u : 0u;
        /* v0.4: gyro settings ride in SaveData.reserved; sanitise junk.
         * A pre-v0.4 image has zeroed reserved -> ON / MED defaults. */
        settings_out->gyro_mode = sd.reserved[0] <= (uint8_t)GRAVE_GYRO_INVERT
                                      ? sd.reserved[0] : (uint8_t)GRAVE_GYRO_ON;
        settings_out->gyro_sens = sd.reserved[1] <= (uint8_t)GRAVE_GYRO_SENS_HIGH
                                      ? sd.reserved[1] : (uint8_t)GRAVE_GYRO_SENS_MED;
    }

    /* ---- apply ---- */
    game->world = world;
    game->material_coins = sd.material_coins > 0 ? (int)sd.material_coins : 0;
    game->material_sinew = sd.material_sinew > 0 ? (int)sd.material_sinew : 0;
    game->material_bone_charms = sd.material_bone_charms > 0
                                    ? (int)sd.material_bone_charms : 0;
    game->total_kills = sd.kills_total > 0x7FFFFFFFu
                            ? 0x7FFFFFFF : (int)sd.kills_total;
    game->zone_runs = sd.runs > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)sd.runs;
    game->total_time = sd.play_time > 0.0f ? sd.play_time : 0.0f;

    game->player.camera_first_person = sd.camera_first_person != 0u;
    for (int i = 0; i < WEAPON_COUNT; i++) {
        int tune = (int)sd.weapon_tune[i];
        if (tune < 0) tune = 0;
        if (tune > GRAVE_TUNE_MAX_LEVEL) tune = GRAVE_TUNE_MAX_LEVEL;
        game->player.weapon_owned[i] = owned[i];
        game->player.ammo[i] = ammo[i];
        game->player.weapon_tune[i] = tune;
    }
    game->player.vitality_level = sd.vitality_level > 0
                                      ? (sd.vitality_level > (uint8_t)GRAVE_VITALITY_MAX_LEVEL
                                             ? GRAVE_VITALITY_MAX_LEVEL
                                             : (int)sd.vitality_level)
                                      : 0;
    game->player.reload_tune = sd.reload_tune > 0
                                   ? (sd.reload_tune > (uint8_t)GRAVE_RELOAD_MAX_LEVEL
                                          ? GRAVE_RELOAD_MAX_LEVEL
                                          : (int)sd.reload_tune)
                                   : 0;
    game->player.weapon = (WeaponId)weapon;
    game->player.reload_weapon = (WeaponId)weapon;

    game->total_shots = sd.shots_fired > 0x7FFFFFFFu
                            ? 0x7FFFFFFF : (int)sd.shots_fired;
    game->total_hits = sd.shots_hit > 0x7FFFFFFFu
                           ? 0x7FFFFFFF : (int)sd.shots_hit;
    game->world_complete = sd.world_complete != 0u;

    /* a save is loaded at boot / on the trail, so drop any live run state */
    memset(game->zombies, 0, sizeof(game->zombies));
    memset(game->pickups, 0, sizeof(game->pickups));
    memset(game->props, 0, sizeof(game->props));
    memset(game->shot_traces, 0, sizeof(game->shot_traces));
    memset(game->run_materials, 0, sizeof(game->run_materials));
    game->prop_count = 0;
    game->zombies_alive = 0;
    game->zombies_killed = 0;
    game->zone_time = 0.0f;
    game->frame = 0;
    game->events = 0u;
    game->spawn_timer = 0.0f;
    game->spawn_budget = 0;
    game->spawn_kill_goal = 0;
    game->spawn_wave_left = 0;
    game->spawn_wave_brutes = 0;
    game->boss_spawned = 0;
    game->boss_alive = 0;
    game->boss_defeated = false;
    game->boss_hp = 0.0f;
    game->boss_max_hp = 0.0f;
    game->quest.kind = QUEST_NONE;
    game->quest.target = 0;
    game->quest.progress = 0;
    game->quest.material = DROP_NONE;

    game->player.max_hp = player_max_hp_for(game->player.vitality_level);
    game->player.hp = game->player.max_hp;
    game->player.alive = true;
    game->player.stamina = GRAVE_STAMINA_MAX;
    game->player.velocity_y = 0.0f;
    game->player.on_ground = true;
    game->player.transform.pos = vec3_make(0.0f, 0.0f, 0.0f);
    game->player.transform.yaw = 0.0f;
    game->player.transform.pitch = 0.0f;
    game->player.reload_timer = 0.0f;
    game->player.fire_cooldown = 0.0f;
    game->player.recoil = 0.0f;
    game->player.anim_time = 0.0f;
    game->player.anim_speed = 0.0f;
    game->player.firing = false;
    game->player.muzzle_flash = 0.0f;
    game->player.aiming = false;
    game->player.vel_x = 0.0f;
    game->player.vel_z = 0.0f;

    game->phase = GAME_PHASE_TITLE;
    game->zone = world.current;
    grave_update_camera(game, 400.0f / 240.0f);
    return true;
}

/* ================================================================== */
/* v0.3: trader / shop                                                 */
/* ================================================================== */

int grave_shop_level(const GraveGame *game, ShopItem item)
{
    const ShopItemSpec *spec;

    if (game == NULL || (int)item < 0 || (int)item >= SHOP_ITEM_COUNT)
        return 0;
    spec = grave_shop_item(item);
    switch (spec->level_field) {
    case SHOP_LEVEL_WEAPON_TUNE:
        if (spec->weapon < 0 || spec->weapon >= WEAPON_COUNT)
            return 0;
        return game->player.weapon_tune[spec->weapon];
    case SHOP_LEVEL_VITALITY:
        return game->player.vitality_level;
    case SHOP_LEVEL_RELOAD:
        return game->player.reload_tune;
    case SHOP_LEVEL_NONE:
    default:
        return 0;
    }
}

/* cost-row index for the next purchase: the current level (or row 0 for a
 * repeatable consumable) */
static int shop_cost_row(const GraveGame *game, ShopItem item, const ShopItemSpec *spec)
{
    int level;
    if (spec->max_level <= 0)
        return 0;
    level = grave_shop_level(game, item);
    if (level < 0)
        level = 0;
    if (level >= SHOP_MAX_LEVELS)
        level = SHOP_MAX_LEVELS - 1;
    return level;
}

static bool shop_consumable_has_effect(const GraveGame *game, ShopItem item)
{
    if (item == SHOP_AMMO) {
        for (int i = 0; i < WEAPON_COUNT; i++) {
            if (game->player.weapon_owned[i] &&
                game->player.ammo[i] < grave_weapon_spec((WeaponId)i)->magazine)
                return true;
        }
        return false;
    }
    if (item == SHOP_MEDKIT)
        return game->player.hp < game->player.max_hp;
    return true;
}

bool grave_shop_can_buy(const GraveGame *game, ShopItem item)
{
    const ShopItemSpec *spec;
    int level;
    int row;

    if (game == NULL || (int)item < 0 || (int)item >= SHOP_ITEM_COUNT)
        return false;
    if (game->world.in_zone)
        return false; /* the trader only sets up on the trail */
    spec = grave_shop_item(item);
    level = grave_shop_level(game, item);
    if (spec->max_level > 0) {
        if (level >= spec->max_level)
            return false;
    } else if (!shop_consumable_has_effect(game, item)) {
        return false;
    }
    row = shop_cost_row(game, item, spec);
    if (row < 0 || row >= SHOP_MAX_LEVELS)
        return false;
    if (game->material_coins < spec->cost_coins[row])
        return false;
    if (game->material_sinew < spec->cost_sinew[row])
        return false;
    if (game->material_bone_charms < spec->cost_charms[row])
        return false;
    return true;
}

bool grave_shop_buy(GraveGame *game, ShopItem item)
{
    const ShopItemSpec *spec;
    int level;
    int row;

    if (game == NULL || (int)item < 0 || (int)item >= SHOP_ITEM_COUNT)
        return false;
    if (!grave_shop_can_buy(game, item))
        return false;

    spec = grave_shop_item(item);
    level = grave_shop_level(game, item);
    row = shop_cost_row(game, item, spec);

    game->material_coins -= spec->cost_coins[row];
    game->material_sinew -= spec->cost_sinew[row];
    game->material_bone_charms -= spec->cost_charms[row];

    switch (item) {
    case SHOP_AMMO:
        for (int i = 0; i < WEAPON_COUNT; i++) {
            if (game->player.weapon_owned[i])
                game->player.ammo[i] = grave_weapon_spec((WeaponId)i)->magazine;
        }
        break;

    case SHOP_TUNE_FLINTLOCK:
    case SHOP_TUNE_REVOLVER:
    case SHOP_TUNE_MUSKET:
        if (spec->weapon >= 0 && spec->weapon < WEAPON_COUNT)
            game->player.weapon_tune[spec->weapon] = level + 1;
        break;

    case SHOP_VITALITY: {
        float old_max = game->player.max_hp;
        game->player.vitality_level = level + 1;
        game->player.max_hp = player_max_hp_for(game->player.vitality_level);
        /* heal by exactly the gained amount, as the spec asks */
        game->player.hp = clampf(game->player.hp + (game->player.max_hp - old_max),
                                 0.0f, game->player.max_hp);
        break;
    }

    case SHOP_RELOAD:
        game->player.reload_tune = level + 1;
        break;

    case SHOP_MEDKIT:
        game->player.hp = clampf(game->player.hp + 0.5f * game->player.max_hp,
                                 0.0f, game->player.max_hp);
        break;

    default:
        break;
    }
    return true;
}
