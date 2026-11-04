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

/* yaw 0 = +z, positive yaw turns right (towards +x); pitch positive looks up */
static Vec3 aim_dir(float yaw, float pitch)
{
    float cp = cosf(pitch);
    return vec3_make(sinf(yaw) * cp, sinf(pitch), cosf(yaw) * cp);
}

static Vec3 flat_forward(float yaw)
{
    return vec3_make(sinf(yaw), 0.0f, cosf(yaw));
}

static Vec3 flat_right(float yaw)
{
    return vec3_make(cosf(yaw), 0.0f, -sinf(yaw));
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

static void build_ashen_mill(GraveGame *game)
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
     * keep the player in the yard in v0.1) */
    add_prop(game, vec3_make(-3.0f, 0.0f, half),
             vec3_make(0.22f, 1.70f, 0.22f), 0.0f, PROP_GATE);
    add_prop(game, vec3_make(3.0f, 0.0f, half),
             vec3_make(0.22f, 1.70f, 0.22f), 0.0f, PROP_GATE);

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

static void build_zone_layout(GraveGame *game)
{
    /* v0.1 only ships ASHEN MILL; the other zones reuse the yard as a
     * placeholder until their props get authored. */
    build_ashen_mill(game);
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

/* v0.1 drop table: a rotating coin / sinew / coin / (powder or coin) cycle.
 * It is deliberately not a pure random roll: the quest needs 8 coins + 4
 * sinew, and 22 kills on this cycle always *drop* at least 12 coins and 5
 * sinew, so the collect step can never be soft-locked by bad luck. */
static GraveDrop choose_drop(GraveGame *game)
{
    int slot = (game->zombies_killed - 1) & 3;
    switch (slot) {
    case 0:  return DROP_COIN;
    case 1:  return DROP_SINEW;
    case 2:  return DROP_COIN;
    default: return (rng_unit(game) < 0.60f) ? DROP_AMMO : DROP_COIN;
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
            break;
        case DROP_SINEW:
            game->material_sinew++;
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

static void player_look(GraveGame *game, const GraveInput *input)
{
    Player *p = &game->player;
    float look_x = clampf(input->look_x, -1.0f, 1.0f);
    float look_y = clampf(input->look_y, -1.0f, 1.0f);
    p->transform.yaw = grave_wrap_pi(p->transform.yaw + look_x * GRAVE_LOOK_YAW_PER_UNIT);
    p->transform.pitch = clampf(p->transform.pitch - look_y * GRAVE_LOOK_PITCH_PER_UNIT,
                                -GRAVE_PITCH_LIMIT, GRAVE_PITCH_LIMIT);
}

static void player_move(GraveGame *game, const GraveInput *input, float dt)
{
    Player *p = &game->player;
    float mx = clampf(input->move_x, -1.0f, 1.0f);
    float my = clampf(input->move_y, -1.0f, 1.0f);
    Vec3 fwd = flat_forward(p->transform.yaw);
    Vec3 right = flat_right(p->transform.yaw);
    float wish_x = fwd.x * my + right.x * mx;
    float wish_z = fwd.z * my + right.z * mx;
    float wish_len = sqrtf(wish_x * wish_x + wish_z * wish_z);
    bool moving = wish_len > 0.001f;
    bool sprint = input->sprint && moving && p->stamina > GRAVE_STAMINA_SPRINT_MIN;
    float speed;

    if (wish_len > 1.0f) {
        wish_x /= wish_len;
        wish_z /= wish_len;
    }
    speed = sprint ? GRAVE_PLAYER_SPRINT_SPEED : GRAVE_PLAYER_WALK_SPEED;

    if (sprint)
        p->stamina = clampf(p->stamina - GRAVE_STAMINA_DRAIN * dt, 0.0f, GRAVE_STAMINA_MAX);
    else
        p->stamina = clampf(p->stamina + GRAVE_STAMINA_REGEN * dt, 0.0f, GRAVE_STAMINA_MAX);

    {
        float nx = p->transform.pos.x + wish_x * speed * dt;
        float nz = p->transform.pos.z + wish_z * speed * dt;
        resolve_world(game, &nx, &nz, GRAVE_PLAYER_RADIUS);
        p->transform.pos.x = nx;
        p->transform.pos.z = nz;
    }
    p->transform.pos.y = 0.0f;
    p->velocity_y = 0.0f;
    p->on_ground = true;

    p->anim_speed = moving ? (sprint ? 1.35f : 1.0f) : 0.0f;
    p->anim_time += dt * GRAVE_ANIM_WALK_RATE * p->anim_speed;
    if (p->anim_time > 1000.0f)
        p->anim_time -= 1000.0f;
}

/* ================================================================== */
/* weapons                                                             */
/* ================================================================== */

static void weapon_start_reload(GraveGame *game)
{
    Player *p = &game->player;
    const WeaponSpec *spec = grave_weapon_spec(p->weapon);
    if (p->reload_timer > 0.0f)
        return;
    if (p->ammo[p->weapon] >= spec->magazine)
        return;
    p->reload_timer = spec->reload_time;
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
    if (z->hp <= 0.0f) {
        z->hp = 0.0f;
        z->state = ZOMBIE_DYING;
        z->death_time = 0.0f;
        z->attack_timer = 0.0f;
        z->speed = 0.0f;
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

static void fire_weapon(GraveGame *game)
{
    Player *p = &game->player;
    const WeaponSpec *spec = grave_weapon_spec(p->weapon);
    int pellets = spec->pellets > 0 ? spec->pellets : 1;
    /* the kick from *this* shot must not move this shot's aim; the player
     * carries it into the next one */
    float aim_recoil = p->recoil;

    p->ammo[p->weapon] -= 1;
    p->fire_cooldown = spec->fire_interval;
    p->muzzle_flash = GRAVE_SHOT_LIFT;
    p->firing = true;
    game->events |= GRAVE_EVT_SHOOT;

    for (int pellet = 0; pellet < pellets; pellet++) {
        ShotResult shot;
        Vec3 origin = p->transform.pos;
        float yaw = p->transform.yaw + rng_sym(game) * spec->spread;
        float pitch = clampf(p->transform.pitch
                                 + aim_recoil * GRAVE_RECOIL_AIM_SHARE
                                 + rng_sym(game) * spec->spread,
                             -1.55f, 1.55f);
        Vec3 dir = aim_dir(yaw, pitch);

        origin.y += GRAVE_PLAYER_HIT_HEIGHT;
        origin.x += dir.x * GRAVE_MUZZLE_OFFSET;
        origin.y += dir.y * GRAVE_MUZZLE_OFFSET;
        origin.z += dir.z * GRAVE_MUZZLE_OFFSET;

        hitscan(game, origin, dir, spec->range, &shot);

        push_trace(game, origin, shot.end, GRAVE_TRACER_LIFE, GRAVE_TRACER_KIND);
        if (shot.hit_zombie) {
            float damage = (float)spec->damage;
            if (shot.headshot)
                damage *= GRAVE_HEADSHOT_MULTIPLIER;
            damage_zombie(game, shot.zombie_index, damage);
            game->events |= GRAVE_EVT_HIT;
            push_trace(game, shot.end, shot.end, GRAVE_IMPACT_LIFE, GRAVE_IMPACT_KIND);
        } else if (shot.hit_world) {
            push_trace(game, shot.end, shot.end, GRAVE_IMPACT_LIFE, GRAVE_IMPACT_KIND);
        }
    }

    p->recoil = clampf(p->recoil + spec->recoil * GRAVE_DEG2RAD, -0.5f, 0.5f);

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
    float roll;
    /* guaranteed flavour beats: an early runner and two brutes per zone */
    if (spawn_index == 3)
        return ZOMBIE_RUNNER;
    if (spawn_index == 12 || spawn_index == 25)
        return ZOMBIE_BRUTE;

    roll = rng_unit(game);
    if (spawn_index >= 18) {
        if (roll < 0.18f) return ZOMBIE_BRUTE;
        if (roll < 0.55f) return ZOMBIE_RUNNER;
        return ZOMBIE_SHAMBLER;
    }
    if (spawn_index >= 8) {
        if (roll < 0.36f) return ZOMBIE_RUNNER;
        if (roll < 0.46f) return ZOMBIE_BRUTE;
        return ZOMBIE_SHAMBLER;
    }
    if (roll < 0.25f)
        return ZOMBIE_RUNNER;
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

static void update_spawner(GraveGame *game, float dt)
{
    const ZoneSpec *zs = grave_zone_spec(game->zone);
    int spawned;
    int wave_size;
    int room_alive;
    int room_pool;
    int count;

    if (game->spawn_budget <= 0)
        return;

    game->spawn_timer -= dt;
    if (game->spawn_timer > 0.0f)
        return;

    spawned = zs->spawn_budget - game->spawn_budget;
    room_alive = zs->max_alive - game->zombies_alive;
    if (room_alive <= 0) {
        game->spawn_timer = GRAVE_WAVE_RETRY;
        return;
    }
    room_pool = MAX_ZOMBIES - count_active_zombies(game);
    if (room_pool <= 0) {
        game->spawn_timer = GRAVE_WAVE_RETRY;
        return;
    }

    wave_size = 3;
    if (spawned >= 10)
        wave_size = 4;
    if (spawned >= 20)
        wave_size = 5;

    count = wave_size;
    if (count > game->spawn_budget)
        count = game->spawn_budget;
    if (count > room_alive)
        count = room_alive;
    if (count > room_pool)
        count = room_pool;

    for (int i = 0; i < count; i++) {
        ZombieType type = choose_zombie_type(game, spawned + i);
        Vec3 pos;
        if (!find_spawn_point(game, grave_zombie_spec(type)->radius, &pos)) {
            /* no clean spot this wave; retry shortly without spending budget */
            game->spawn_timer = GRAVE_WAVE_RETRY;
            break;
        }
        spawn_zombie(game, type, pos);
        game->spawn_budget--;
    }
    game->zombies_alive = count_active_zombies(game);
    game->spawn_timer = zs->wave_interval;
}

/* ================================================================== */
/* quest                                                               */
/* ================================================================== */

static void update_quest(GraveGame *game)
{
    const ZoneSpec *zs = grave_zone_spec(game->zone);

    if (game->quest.kind == QUEST_NONE || game->quest.kind == QUEST_DONE)
        return;

    for (int guard = 0; guard < 4; guard++) {
        Quest *q = &game->quest;

        if (q->kind == QUEST_CLEAR_ZONE) {
            int target = game->spawn_kill_goal > 0 ? game->spawn_kill_goal : 1;
            q->target = target;
            q->progress = game->zombies_killed < target ? game->zombies_killed : target;
            if (q->progress < target)
                return;
            /* kill step done: now bring back the goods */
            q->kind = QUEST_COLLECT;
            q->material = DROP_COIN;
            q->target = zs->coin_goal;
            q->progress = game->material_coins < q->target ? game->material_coins : q->target;
            continue;
        }

        if (q->kind == QUEST_COLLECT) {
            int have = 0;
            if (q->material == DROP_COIN)
                have = game->material_coins;
            else if (q->material == DROP_SINEW)
                have = game->material_sinew;
            q->progress = have < q->target ? have : q->target;
            if (q->progress < q->target)
                return;
            if (q->material != DROP_SINEW) {
                q->material = DROP_SINEW;
                q->target = zs->sinew_goal;
                q->progress = game->material_sinew < q->target ? game->material_sinew : q->target;
                continue;
            }
            q->kind = QUEST_DONE;
            game->events |= GRAVE_EVT_QUEST_DONE | GRAVE_EVT_ZONE_DONE;
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
    for (int i = 0; i < WEAPON_COUNT; i++)
        game->player.ammo[i] = grave_weapon_spec((WeaponId)i)->magazine;
    game->quest.kind = QUEST_NONE;
    grave_update_camera(game, 400.0f / 240.0f);
}

void grave_game_start_zone(GraveGame *game, ZoneId zone)
{
    const ZoneSpec *zs;
    Player *p;

    if (game == NULL)
        return;
    if ((int)zone < 0 || (int)zone >= ZONE_COUNT)
        zone = ZONE_ASHEN_MILL;
    zs = grave_zone_spec(zone);

    memset(game->zombies, 0, sizeof(game->zombies));
    memset(game->pickups, 0, sizeof(game->pickups));
    memset(game->props, 0, sizeof(game->props));
    memset(game->shot_traces, 0, sizeof(game->shot_traces));
    game->prop_count = 0;

    p = &game->player;
    memset(p, 0, sizeof(*p));
    p->max_hp = GRAVE_PLAYER_MAX_HP;
    p->hp = GRAVE_PLAYER_MAX_HP;
    p->alive = true;
    p->stamina = GRAVE_STAMINA_MAX;
    p->weapon = WEAPON_FLINTLOCK;
    p->reload_weapon = WEAPON_FLINTLOCK;
    p->transform.pos = vec3_make(0.0f, 0.0f, zs->half_extent - 4.5f);
    p->transform.yaw = GRAVE_PI; /* facing into the yard, gate at our back */
    p->transform.pitch = 0.0f;
    p->on_ground = true;
    p->camera_first_person = false;
    for (int i = 0; i < WEAPON_COUNT; i++)
        p->ammo[i] = grave_weapon_spec((WeaponId)i)->magazine;

    game->zone = zone;
    game->phase = GAME_PHASE_PLAYING;
    game->rng = game->seed; /* restart = clean deterministic replay */
    game->quest.kind = QUEST_CLEAR_ZONE;
    game->quest.target = zs->kill_goal;
    game->quest.progress = 0;
    game->quest.material = DROP_NONE;
    game->material_coins = 0;
    game->material_sinew = 0;
    game->zombies_alive = 0;
    game->zombies_killed = 0;
    game->zone_time = 0.0f;
    game->frame = 0;
    game->events = 0;
    game->spawn_timer = 1.1f;
    game->spawn_budget = zs->spawn_budget;
    game->spawn_kill_goal = zs->kill_goal;

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

    player_look(game, input);
    if (input->toggle_camera)
        p->camera_first_person = !p->camera_first_person;

    player_move(game, input, dt);
    update_weapons(game, input, dt);
    update_spawner(game, dt);
    update_zombies(game, dt);
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
        game->camera_fov = GRAVE_CAM_FOV_FP;
        return;
    }

    {
        Vec3 pivot;
        Vec3 cam;
        float distance = GRAVE_CAM_TP_DISTANCE;
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
        game->camera_fov = GRAVE_CAM_FOV_TP;
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
