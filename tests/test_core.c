/*
 * Gravepowder - host unit tests for the portable gameplay core.
 *
 * Build/run (from the repo root):
 *   make -C tests CC="D:/tools/zig/zig.exe cc" test
 *
 * The core is plain C99 with no <3ds.h>, so this links game.c + data.c +
 * input.c directly and exercises the game the way the headless sim does.
 */
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "grave.h"
#include "core_internal.h"

#define TEST_DT (1.0f / 60.0f)
#define TEST_SEED 0x00C0FFEEu

static int g_checks;
static int g_fails;

static void check_fail(const char *file, int line, const char *what)
{
    g_fails++;
    printf("FAIL %s:%d: %s\n", file, line, what);
}

#define CHECK(cond)                                                         \
    do {                                                                    \
        g_checks++;                                                         \
        if (!(cond))                                                        \
            check_fail(__FILE__, __LINE__, #cond);                          \
    } while (0)

#define CHECK_NEAR(actual, expected, tol, label)                            \
    do {                                                                    \
        float a_ = (float)(actual);                                         \
        float e_ = (float)(expected);                                       \
        g_checks++;                                                         \
        if (fabsf(a_ - e_) > (float)(tol)) {                                \
            g_fails++;                                                      \
            printf("FAIL %s:%d: %s (got %g, want %g +/- %g)\n",             \
                   __FILE__, __LINE__, label,                               \
                   (double)a_, (double)e_, (double)(float)(tol));           \
        }                                                                   \
    } while (0)

/* ================================================================== */
/* helpers                                                             */
/* ================================================================== */

static void clear_events(GraveGame *g)
{
    g->events = 0u;
}

static void step_idle(GraveGame *g)
{
    grave_game_step(g, NULL, TEST_DT);
}

static void step_n(GraveGame *g, int n)
{
    for (int i = 0; i < n; i++)
        step_idle(g);
}

static void step_with(GraveGame *g, const GraveInput *in)
{
    grave_game_step(g, in, TEST_DT);
}

static void fire_once(GraveGame *g)
{
    GraveInput in;
    grave_input_reset(&in);
    in.fire = true;
    step_with(g, &in);
}

static void reload_once(GraveGame *g)
{
    GraveInput in;
    grave_input_reset(&in);
    in.reload = true;
    step_with(g, &in);
}

static int count_zombies(const GraveGame *g)
{
    int n = 0;
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        if (g->zombies[i].active)
            n++;
    }
    return n;
}

static int count_pickups(const GraveGame *g)
{
    int n = 0;
    for (int i = 0; i < MAX_PICKUPS; i++) {
        if (g->pickups[i].active)
            n++;
    }
    return n;
}

static int count_traces(const GraveGame *g, int kind)
{
    int n = 0;
    for (int i = 0; i < MAX_SHOT_TRACES; i++) {
        if (g->shot_traces[i].life > 0.0f && (int)g->shot_traces[i].kind == kind)
            n++;
    }
    return n;
}

static int first_active_pickup(const GraveGame *g)
{
    for (int i = 0; i < MAX_PICKUPS; i++) {
        if (g->pickups[i].active)
            return i;
    }
    return -1;
}

static float dist_xz(Vec3 a, Vec3 b)
{
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return sqrtf(dx * dx + dz * dz);
}

static float aim_pitch_at(float target_y, float horizontal_dist)
{
    return atan2f(target_y - GRAVE_PLAYER_HIT_HEIGHT, horizontal_dist);
}

static void prop_extents(const Prop *p, float *minx, float *maxx, float *minz, float *maxz)
{
    *minx = p->pos.x - p->half.x;
    *maxx = p->pos.x + p->half.x;
    *minz = p->pos.z - p->half.z;
    *maxz = p->pos.z + p->half.z;
}

/* circle-vs-prop-footprint test, mirrors the core's collision */
static bool blocked_by_props_xz(const GraveGame *g, float x, float z, float radius)
{
    for (int i = 0; i < g->prop_count; i++) {
        float minx, maxx, minz, maxz;
        prop_extents(&g->props[i], &minx, &maxx, &minz, &maxz);
        float cx = grave_clampf(x, minx, maxx);
        float cz = grave_clampf(z, minz, maxz);
        float dx = x - cx;
        float dz = z - cz;
        if (dx * dx + dz * dz < radius * radius)
            return true;
    }
    return false;
}

static bool point_inside_prop(const GraveGame *g, Vec3 p, float pad)
{
    for (int i = 0; i < g->prop_count; i++) {
        float minx, maxx, minz, maxz;
        float top = g->props[i].pos.y + 2.0f * g->props[i].half.y;
        prop_extents(&g->props[i], &minx, &maxx, &minz, &maxz);
        if (p.x >= minx - pad && p.x <= maxx + pad &&
            p.y >= g->props[i].pos.y - pad && p.y <= top + pad &&
            p.z >= minz - pad && p.z <= maxz + pad)
            return true;
    }
    return false;
}

static void add_test_prop(GraveGame *g, Vec3 pos, Vec3 half, PropKind kind)
{
    if (g->prop_count >= MAX_PROPS)
        return;
    {
        Prop *p = &g->props[g->prop_count];
        p->pos = pos;
        p->half = half;
        p->yaw = 0.0f;
        p->kind = kind;
        p->color = (uint8_t)kind;
    }
    g->prop_count++;
}

static int add_test_pickup(GraveGame *g, GraveDrop kind, Vec3 pos, float life)
{
    for (int i = 0; i < MAX_PICKUPS; i++) {
        if (!g->pickups[i].active) {
            Pickup *q = &g->pickups[i];
            q->active = true;
            q->kind = kind;
            q->pos = pos;
            q->bob_time = 0.0f;
            q->life = life;
            return i;
        }
    }
    return -1;
}

static void place_zombie(GraveGame *g, int slot, ZombieType type, Vec3 pos,
                         ZombieState state)
{
    const ZombieSpec *spec = grave_zombie_spec(type);
    Zombie *z = &g->zombies[slot];
    memset(z, 0, sizeof(*z));
    z->active = true;
    z->type = type;
    z->state = state;
    z->pos = vec3_make(pos.x, 0.0f, pos.z);
    z->yaw = atan2f(g->player.transform.pos.x - pos.x,
                    g->player.transform.pos.z - pos.z);
    z->hp = spec->hp;
    z->max_hp = spec->hp;
    z->speed = spec->speed;
    z->attack_timer = state == ZOMBIE_IDLE ? 0.5f : 0.0f;
    z->anim_time = 0.0f;
    z->death_time = 0.0f;
    z->drop = DROP_NONE;
}

static void set_weapon(GraveGame *g, WeaponId id)
{
    g->player.weapon = id;
    g->player.reload_weapon = id;
    g->player.reload_timer = 0.0f;
    g->player.fire_cooldown = 0.0f;
}

/* Full clean arena: fresh init + zone start, no spawner, player placed exactly
 * where the test wants, pools empty.  The RNG restarts from TEST_SEED so every
 * test is reproducible. */
static void quiet_arena(GraveGame *g, float px, float pz, float yaw, float pitch)
{
    grave_game_init(g, TEST_SEED);
    grave_game_start_zone(g, ZONE_ASHEN_MILL);
    memset(g->zombies, 0, sizeof(g->zombies));
    memset(g->pickups, 0, sizeof(g->pickups));
    g->zombies_alive = 0;
    g->zombies_killed = 0;
    g->spawn_budget = 0;
    g->spawn_timer = 1.0e9f;
    g->material_coins = 0;
    g->material_sinew = 0;
    g->events = 0u;
    g->frame = 0u;
    g->zone_time = 0.0f;
    g->player.transform.pos = vec3_make(px, 0.0f, pz);
    g->player.transform.yaw = yaw;
    g->player.transform.pitch = pitch;
    g->player.hp = g->player.max_hp;
    g->player.alive = true;
    g->player.stamina = GRAVE_STAMINA_MAX;
    g->player.recoil = 0.0f;
    g->player.reload_timer = 0.0f;
    g->player.fire_cooldown = 0.0f;
    g->quest.kind = QUEST_CLEAR_ZONE;
    g->quest.target = g->spawn_kill_goal;
    g->quest.progress = 0;
    g->quest.material = DROP_NONE;
    grave_update_camera(g, 400.0f / 240.0f);
}

static void clear_props(GraveGame *g)
{
    g->prop_count = 0;
}

/* kill whatever is in slot 0 by setting its hp to 1 and firing once */
static void kill_slotted_zombie(GraveGame *g, ZombieType type, Vec3 pos)
{
    place_zombie(g, 0, type, pos, ZOMBIE_CHASE);
    g->zombies[0].hp = 1.0f;
    g->player.transform.yaw = 0.0f;
    g->player.transform.pitch = aim_pitch_at(0.8f, dist_xz(g->player.transform.pos, pos));
    set_weapon(g, WEAPON_FLINTLOCK);
    g->player.ammo[WEAPON_FLINTLOCK] = 1;
    fire_once(g);
}

static uint32_t hash_bytes(uint32_t h, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t hash_game(const GraveGame *g)
{
    GraveGame copy = *g;
    copy.seed = 0u; /* compare gameplay, not the restarted-seed marker */
    return hash_bytes(2166136261u, &copy, sizeof(copy));
}

static void script_input(int i, GraveInput *in)
{
    grave_input_reset(in);
    in->move_x = sinf((float)i * 0.070f) * 0.7f;
    in->move_y = cosf((float)i * 0.045f);
    in->look_x = sinf((float)i * 0.230f) * 0.12f;
    in->look_y = cosf((float)i * 0.170f) * 0.05f;
    in->fire = (i % 90) < 40;
    in->sprint = (i % 240) < 80;
    in->reload = (i % 137) == 0;
}

/* Runs the same deterministic input tape over a game, keeping it alive and in
 * the playing phase so the whole run gets exercised. */
static void run_script(GraveGame *g, int steps)
{
    for (int i = 0; i < steps; i++) {
        GraveInput in;
        script_input(i, &in);
        g->player.hp = 1000.0f;
        g->player.alive = true;
        grave_game_step(g, &in, TEST_DT);
        if (g->phase != GAME_PHASE_PLAYING)
            g->phase = GAME_PHASE_PLAYING;
        g->player.hp = g->player.max_hp;
        g->player.alive = true;
        g->zombies_killed = 0;
        g->material_coins = 0;
        g->material_sinew = 0;
        g->material_bone_charms = 0;
        g->quest.kind = QUEST_CLEAR_ZONE;
        g->quest.progress = 0;
    }
}

static uint32_t test_rng_next(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static float test_rng_unit(uint32_t *state)
{
    return (float)(test_rng_next(state) >> 8) * (1.0f / 16777216.0f);
}

static bool finite_f32(float v)
{
    return isfinite((double)v) != 0;
}

static bool valid_zone_id(ZoneId zone)
{
    return (int)zone >= 0 && (int)zone < ZONE_COUNT;
}

/* ================================================================== */
/* input helpers                                                       */
/* ================================================================== */

static void test_input_helpers(void)
{
    GraveInput in;

    memset(&in, 0x5A, sizeof(in));
    grave_input_reset(&in);
    CHECK(in.move_x == 0.0f && in.move_y == 0.0f);
    CHECK(in.look_x == 0.0f && in.look_y == 0.0f);
    CHECK(!in.fire && !in.sprint && !in.reload && !in.swap_weapon);
    CHECK(!in.toggle_camera && !in.pause && !in.interact);

    grave_input_reset(&in);
    grave_input_stick(&in, 156, 0);
    CHECK_NEAR(in.move_x, 1.0f, 1.0e-5f, "stick full right");
    CHECK_NEAR(in.move_y, 0.0f, 1.0e-5f, "stick full right y");
    grave_input_stick(&in, 0, -100);
    CHECK_NEAR(in.move_y, -100.0f / 156.0f, 1.0e-5f, "stick up");
    grave_input_stick(&in, 1000, 0);
    CHECK_NEAR(in.move_x, 1.0f, 1.0e-5f, "stick clamps high");
    grave_input_stick(&in, -1000, 0);
    CHECK_NEAR(in.move_x, -1.0f, 1.0e-5f, "stick clamps low");

    grave_input_reset(&in);
    grave_input_touch(&in, 160, 80);
    CHECK_NEAR(in.look_x, 1.0f, 1.0e-5f, "touch x full drag");
    CHECK_NEAR(in.look_y, 0.5f, 1.0e-5f, "touch y half drag");
    grave_input_touch(&in, -160, -80);
    CHECK_NEAR(in.look_x, 0.0f, 1.0e-5f, "touch accumulates");
    CHECK_NEAR(in.look_y, 0.0f, 1.0e-5f, "touch accumulates y");

    grave_input_reset(NULL);
    grave_input_stick(NULL, 10, 10);
    grave_input_touch(NULL, 10, 10);
    CHECK(true); /* merely reaching here is the check */
}

/* ================================================================== */
/* data tables                                                         */
/* ================================================================== */

static void test_spec_tables(void)
{
    for (int i = 0; i < WEAPON_COUNT; i++) {
        const WeaponSpec *spec = grave_weapon_spec((WeaponId)i);
        CHECK(spec != NULL);
        CHECK(spec->name != NULL && spec->name[0] != '\0');
        CHECK(spec->damage > 0);
        CHECK(spec->pellets >= 1);
        CHECK(spec->range > 5.0f);
        CHECK(spec->spread >= 0.0f);
        CHECK(spec->magazine >= 1);
        CHECK(spec->fire_interval > 0.0f);
        CHECK(spec->reload_time > 0.0f);
        CHECK(spec->recoil >= 0.0f);
    }
    CHECK(grave_weapon_spec(WEAPON_MUSKET)->damage >
          grave_weapon_spec(WEAPON_REVOLVER)->damage);
    CHECK(grave_weapon_spec(WEAPON_MUSKET)->range >
          grave_weapon_spec(WEAPON_FLINTLOCK)->range);
    CHECK(grave_weapon_spec(WEAPON_MUSKET)->reload_time >
          grave_weapon_spec(WEAPON_REVOLVER)->reload_time);
    CHECK(strcmp(grave_weapon_spec((WeaponId)99)->name,
                 grave_weapon_spec(WEAPON_FLINTLOCK)->name) == 0);

    for (int i = 0; i < ZOMBIE_COUNT; i++) {
        const ZombieSpec *spec = grave_zombie_spec((ZombieType)i);
        CHECK(spec != NULL);
        CHECK(spec->hp > 0.0f);
        CHECK(spec->speed > 0.0f);
        CHECK(spec->damage > 0.0f);
        CHECK(spec->body_height > 1.0f);
        CHECK(spec->radius > 0.1f && spec->radius < 1.0f);
        CHECK(spec->stagger_chance >= 0.0f && spec->stagger_chance <= 1.0f);
    }
    CHECK(grave_zombie_spec(ZOMBIE_BRUTE)->hp > grave_zombie_spec(ZOMBIE_SHAMBLER)->hp);
    CHECK(grave_zombie_spec(ZOMBIE_RUNNER)->speed >
          grave_zombie_spec(ZOMBIE_SHAMBLER)->speed);
    CHECK(grave_zombie_spec(ZOMBIE_BRUTE)->damage >
          grave_zombie_spec(ZOMBIE_RUNNER)->damage);
    CHECK(grave_zombie_spec((ZombieType)99) == grave_zombie_spec(ZOMBIE_SHAMBLER));

    for (int i = 0; i < ZONE_COUNT; i++) {
        const ZoneSpec *zs = grave_zone_spec((ZoneId)i);
        CHECK(zs->half_extent >= 10.0f);
        CHECK(zs->spawn_budget > 0);
        CHECK(zs->kill_goal > 0 && zs->kill_goal <= zs->spawn_budget);
        CHECK(zs->max_alive > 0 && zs->max_alive <= MAX_ZOMBIES);
        CHECK(zs->wave_interval > 0.0f);
        /* generic quest staging: kill -> material A -> material B */
        CHECK((int)zs->material_a > (int)DROP_NONE && (int)zs->material_a < DROP_COUNT);
        CHECK((int)zs->material_b > (int)DROP_NONE && (int)zs->material_b < DROP_COUNT);
        CHECK((int)zs->material_a != (int)DROP_AMMO);
        CHECK((int)zs->material_b != (int)DROP_AMMO);
        CHECK((int)zs->material_a != (int)zs->material_b);
        CHECK(zs->goal_a > 0 && zs->goal_b > 0);
        CHECK(zs->kill_goal >= zs->goal_a + zs->goal_b);
        CHECK(zs->brute_per_wave >= 0 && zs->brute_per_wave <= 2);
        CHECK(zs->runner_share >= 0.0f && zs->runner_share <= 1.0f);
        CHECK(zs->brute_chance >= 0.0f && zs->brute_chance < 0.5f);
        CHECK(zs->layout >= ZONE_LAYOUT_ASHEN_MILL && zs->layout <= ZONE_LAYOUT_OLD_MINE);
    }
    CHECK(grave_zone_spec(ZONE_BONE_ORCHARD)->kill_goal >= 50);
    CHECK(grave_zone_spec(ZONE_BONE_ORCHARD)->material_b == DROP_BONE_CHARM);
    CHECK(grave_zone_spec(ZONE_BONE_ORCHARD)->brute_per_wave >= 1);
    CHECK(grave_zone_spec(ZONE_BONE_ORCHARD)->runner_share >
          grave_zone_spec(ZONE_ASHEN_MILL)->runner_share);
    CHECK(strcmp(grave_zone_name(ZONE_ASHEN_MILL), "ASHEN MILL") == 0);
    CHECK(strcmp(grave_zone_name(ZONE_BONE_ORCHARD), "BONE ORCHARD") == 0);
    CHECK(strcmp(grave_zone_name(ZONE_OLD_MINE), "OLD MINE") == 0);
    CHECK(grave_zone_name((ZoneId)99) != NULL);
    CHECK(grave_drop_name(DROP_COIN)[0] != '\0');
    CHECK(grave_drop_name(DROP_SINEW)[0] != '\0');
    CHECK(grave_drop_name(DROP_AMMO)[0] != '\0');
    CHECK(grave_drop_name(DROP_BONE_CHARM)[0] != '\0');
    CHECK(grave_drop_name(DROP_NONE)[0] == '\0');
    CHECK(grave_save_size() == GRAVE_SAVE_SIZE);
}

/* ================================================================== */
/* init / zone start                                                   */
/* ================================================================== */

static void test_init_defaults(void)
{
    GraveGame g;
    grave_game_init(&g, 0u);

    CHECK(g.phase == GAME_PHASE_TITLE);
    CHECK(g.zone == ZONE_ASHEN_MILL);
    CHECK(g.frame == 0u);
    CHECK(g.zone_time == 0.0f);
    CHECK(g.rng != 0u); /* zero seed is replaced, xorshift must not collapse */
    CHECK(g.seed != 0u);
    CHECK(g.prop_count == 0);
    CHECK(g.zombies_alive == 0);
    CHECK(g.zombies_killed == 0);
    CHECK(g.material_coins == 0 && g.material_sinew == 0);
    CHECK(g.material_bone_charms == 0);
    CHECK(g.total_kills == 0);
    CHECK(g.total_time == 0.0f);
    CHECK(g.zone_runs == 0);
    CHECK(g.quest.kind == QUEST_NONE);
    CHECK(g.player.alive);
    CHECK_NEAR(g.player.hp, GRAVE_PLAYER_MAX_HP, 0.001f, "init hp");
    CHECK_NEAR(g.player.max_hp, GRAVE_PLAYER_MAX_HP, 0.001f, "init max hp");
    CHECK_NEAR(g.player.stamina, GRAVE_STAMINA_MAX, 0.001f, "init stamina");
    CHECK(g.player.weapon == WEAPON_FLINTLOCK);
    CHECK(g.player.ammo[WEAPON_FLINTLOCK] ==
          grave_weapon_spec(WEAPON_FLINTLOCK)->magazine);
    CHECK(g.player.ammo[WEAPON_REVOLVER] ==
          grave_weapon_spec(WEAPON_REVOLVER)->magazine);
    CHECK(g.player.ammo[WEAPON_MUSKET] ==
          grave_weapon_spec(WEAPON_MUSKET)->magazine);
    CHECK(!g.player.camera_first_person);
    CHECK(g.camera_fov > 0.0f);

    /* world: only the mill is unlocked at boot, and we are on the trail */
    CHECK(!g.world.in_zone);
    CHECK(g.world.current == ZONE_ASHEN_MILL);
    for (int i = 0; i < ZONE_COUNT; i++) {
        CHECK(g.world.nodes[i].zone == (ZoneId)i);
        CHECK(!g.world.nodes[i].cleared);
        CHECK(g.world.nodes[i].clears == 0);
        CHECK(g.world.nodes[i].best_time == 0.0f);
    }
    CHECK(g.world.nodes[ZONE_ASHEN_MILL].unlocked);
    CHECK(!g.world.nodes[ZONE_BONE_ORCHARD].unlocked);
    CHECK(!g.world.nodes[ZONE_OLD_MINE].unlocked);

    /* inventory helpers on a fresh save */
    CHECK(grave_inventory_weapon_count(&g) == WEAPON_COUNT);
    for (int i = 0; i < WEAPON_COUNT; i++) {
        CHECK(grave_inventory_owned(&g, (WeaponId)i));
        CHECK(grave_inventory_ammo(&g, (WeaponId)i) ==
              grave_weapon_spec((WeaponId)i)->magazine);
    }
    CHECK(grave_inventory_material(&g, DROP_COIN) == 0);
    CHECK(grave_inventory_material(&g, DROP_BONE_CHARM) == 0);
    CHECK(grave_inventory_material(&g, DROP_AMMO) == 0);
    CHECK(!grave_inventory_owned(&g, (WeaponId)99));
    CHECK(grave_inventory_ammo(&g, (WeaponId)99) == 0);
    CHECK(grave_inventory_weapon_count(NULL) == 0);

    /* stepping the title does nothing */
    step_n(&g, 10);
    CHECK(g.phase == GAME_PHASE_TITLE);
    CHECK(g.frame == 0u);
    CHECK(g.zone_time == 0.0f);

    grave_game_init(NULL, 1u);
    grave_game_start_zone(NULL, ZONE_ASHEN_MILL);
    grave_game_step(NULL, NULL, TEST_DT);
    grave_update_camera(NULL, 1.0f);
    CHECK(grave_hero_walk_phase(NULL) == 0.0f);
    grave_hero_root(NULL, NULL, NULL);
}

static void test_start_zone(void)
{
    GraveGame g;
    int fence = 0, gate = 0, well = 0, crate = 0, barrel = 0, tomb = 0, tree = 0;
    int gate_fence = 0;
    bool fence_at_gate_post = false;

    grave_game_init(&g, TEST_SEED);
    grave_game_start_zone(&g, ZONE_ASHEN_MILL);

    CHECK(g.phase == GAME_PHASE_PLAYING);
    CHECK(g.zone == ZONE_ASHEN_MILL);
    CHECK(g.world.in_zone);
    CHECK(g.world.current == ZONE_ASHEN_MILL);
    CHECK(g.zone_runs == 1);
    for (int i = 0; i < DROP_COUNT; i++)
        CHECK(g.run_materials[i] == 0);
    CHECK(g.prop_count >= 60 && g.prop_count <= MAX_PROPS);
    CHECK(g.frame == 0u);
    CHECK(g.zone_time == 0.0f);
    CHECK(g.events == 0u);
    CHECK(g.spawn_budget == grave_zone_spec(ZONE_ASHEN_MILL)->spawn_budget);
    CHECK(g.spawn_kill_goal == grave_zone_spec(ZONE_ASHEN_MILL)->kill_goal);
    CHECK(g.quest.kind == QUEST_CLEAR_ZONE);
    CHECK(g.quest.target == g.spawn_kill_goal);
    CHECK(g.quest.progress == 0);
    CHECK(g.quest.material == DROP_NONE);
    CHECK(g.player.alive);
    CHECK_NEAR(g.player.transform.pos.y, 0.0f, 0.0001f, "player on the ground");
    CHECK(fabsf(g.player.transform.pos.x) < 0.01f);
    CHECK(g.player.transform.pos.z > 12.0f && g.player.transform.pos.z < 19.2f);
    CHECK(g.player.transform.yaw == GRAVE_PI);
    CHECK_NEAR(g.player.hp, g.player.max_hp, 0.001f, "zone start full hp");
    CHECK(g.player.ammo[WEAPON_FLINTLOCK] ==
          grave_weapon_spec(WEAPON_FLINTLOCK)->magazine);
    CHECK(!blocked_by_props_xz(&g, g.player.transform.pos.x,
                               g.player.transform.pos.z, GRAVE_PLAYER_RADIUS));
    CHECK(g.rng == g.seed);
    CHECK(g.camera_fov == GRAVE_CAM_FOV_TP);

    for (int i = 0; i < g.prop_count; i++) {
        const Prop *p = &g.props[i];
        CHECK(p->half.x > 0.0f && p->half.y > 0.0f && p->half.z > 0.0f);
        CHECK((int)p->kind >= 0 && (int)p->kind < PROP_KIND_COUNT);
        CHECK(p->color <= (uint8_t)PROP_KIND_COUNT);
        CHECK(fabsf(p->pos.x) <= 20.5f && fabsf(p->pos.z) <= 20.5f);
        switch (p->kind) {
        case PROP_FENCE:    fence++; break;
        case PROP_GATE:     gate++; break;
        case PROP_WELL:     well++; break;
        case PROP_CRATE:    crate++; break;
        case PROP_BARREL:   barrel++; break;
        case PROP_TOMBSTONE: tomb++; break;
        case PROP_DEADTREE: tree++; break;
        default: break;
        }
        /* the only gap in the south fence is the gate opening */
        if (p->kind == PROP_FENCE && p->pos.z > 19.0f && fabsf(p->pos.x) < 2.0f)
            gate_fence++;
        if (p->kind == PROP_FENCE && fabsf(p->pos.z - 20.0f) < 0.3f &&
            fabsf(p->pos.x - 3.75f) < 0.3f)
            fence_at_gate_post = true;
    }
    CHECK(fence >= 40);
    CHECK(gate == 2);
    CHECK(well == 1);
    CHECK(crate >= 5);
    CHECK(barrel >= 3);
    CHECK(tomb >= 5);
    CHECK(tree >= 4);
    CHECK(gate_fence == 0); /* gate opening is clear of fence segments */
    CHECK(fence_at_gate_post);

    /* an idle step does not push the player out of the spawn */
    {
        Vec3 before = g.player.transform.pos;
        step_idle(&g);
        CHECK(dist_xz(before, g.player.transform.pos) < 0.001f);
    }
}

static void test_zone_restart(void)
{
    GraveGame g;
    uint32_t hash1;
    uint32_t hash2;
    int coins_before;
    int sinew_before;

    grave_game_init(&g, 424242u);
    grave_game_start_zone(&g, ZONE_ASHEN_MILL);
    CHECK(g.rng == g.seed);

    /* dirty the state: burn ammo, kills, materials, events and a pickup */
    kill_slotted_zombie(&g, ZOMBIE_RUNNER, vec3_make(0.0f, 0.0f, 4.0f));
    g.material_coins = 3;
    g.material_sinew = 2;
    g.events = 0xFFFFFFFFu;
    add_test_pickup(&g, DROP_COIN, vec3_make(1.0f, 0.3f, 1.0f), 30.0f);
    run_script(&g, 600);
    CHECK(count_zombies(&g) > 0);
    coins_before = g.material_coins;
    sinew_before = g.material_sinew;

    /* restart from the title / trail: run state is wiped, inventory persists */
    grave_game_start_zone(&g, ZONE_ASHEN_MILL);
    CHECK(g.phase == GAME_PHASE_PLAYING);
    CHECK(g.world.in_zone);
    CHECK(g.world.current == ZONE_ASHEN_MILL);
    CHECK(g.rng == g.seed);
    CHECK(g.frame == 0u);
    CHECK(g.zone_time == 0.0f);
    CHECK(g.events == 0u);
    CHECK(g.zombies_alive == 0);
    CHECK(g.zombies_killed == 0);
    CHECK(count_zombies(&g) == 0);
    CHECK(count_pickups(&g) == 0);
    CHECK(g.run_materials[DROP_COIN] == 0);
    CHECK(g.run_materials[DROP_SINEW] == 0);
    CHECK(g.run_materials[DROP_BONE_CHARM] == 0);
    CHECK(g.material_coins == coins_before); /* inventory carries over */
    CHECK(g.material_sinew == sinew_before);
    CHECK(g.quest.kind == QUEST_CLEAR_ZONE && g.quest.progress == 0);
    CHECK_NEAR(g.player.hp, g.player.max_hp, 0.001f, "restart full hp");
    CHECK(g.player.ammo[WEAPON_FLINTLOCK] ==
          grave_weapon_spec(WEAPON_FLINTLOCK)->magazine);

    /* two clean restarts replay identically (inventory/lifetime stats are
     * zeroed before each tape so the hash covers the same starting state) */
    g.material_coins = 0;
    g.material_sinew = 0;
    g.material_bone_charms = 0;
    g.total_kills = 0;
    g.total_time = 0.0f;
    g.zone_runs = 0;
    run_script(&g, 600);
    hash1 = hash_game(&g);
    grave_game_start_zone(&g, ZONE_ASHEN_MILL);
    g.material_coins = 0;
    g.material_sinew = 0;
    g.material_bone_charms = 0;
    g.total_kills = 0;
    g.total_time = 0.0f;
    g.zone_runs = 0;
    run_script(&g, 600);
    hash2 = hash_game(&g);
    CHECK(hash1 == hash2);
}

/* ================================================================== */
/* movement / collision / look                                         */
/* ================================================================== */

static void test_movement(void)
{
    GraveGame g;
    GraveInput in;

    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    CHECK_NEAR(g.player.transform.pos.z, 0.0f, 0.0001f, "arena origin z");

    grave_input_reset(&in);
    in.move_y = 1.0f;
    for (int i = 0; i < 30; i++)
        step_with(&g, &in);
    CHECK_NEAR(g.player.transform.pos.z, 2.1f, 0.02f, "walk forward distance");
    CHECK_NEAR(g.player.transform.pos.x, 0.0f, 0.001f, "walk forward x");
    CHECK(g.player.anim_speed > 0.9f);
    CHECK(g.player.anim_time > 0.1f);
    CHECK_NEAR(g.player.stamina, GRAVE_STAMINA_MAX, 0.001f, "walk stamina regen");

    /* camera-relative: yaw 90 degrees makes "forward" +x */
    quiet_arena(&g, 0.0f, 0.0f, GRAVE_PI * 0.5f, 0.0f);
    clear_props(&g);
    grave_input_reset(&in);
    in.move_y = 1.0f;
    for (int i = 0; i < 30; i++)
        step_with(&g, &in);
    CHECK_NEAR(g.player.transform.pos.x, 2.1f, 0.02f, "forward at yaw pi/2");
    CHECK_NEAR(g.player.transform.pos.z, 0.0f, 0.001f, "forward at yaw pi/2 z");

    /* strafe right is +x at yaw 0 */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    grave_input_reset(&in);
    in.move_x = 1.0f;
    for (int i = 0; i < 30; i++)
        step_with(&g, &in);
    CHECK_NEAR(g.player.transform.pos.x, 2.1f, 0.02f, "strafe right");
    CHECK_NEAR(g.player.transform.pos.z, 0.0f, 0.001f, "strafe right z");

    /* diagonal input is normalised: 30 steps at walk speed == 2.1 m total */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    grave_input_reset(&in);
    in.move_x = 1.0f;
    in.move_y = 1.0f;
    for (int i = 0; i < 30; i++)
        step_with(&g, &in);
    CHECK_NEAR(sqrtf(g.player.transform.pos.x * g.player.transform.pos.x +
                     g.player.transform.pos.z * g.player.transform.pos.z),
               2.1f, 0.02f, "diagonal normalised");

    /* sprint is faster and drains stamina */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    grave_input_reset(&in);
    in.move_y = 1.0f;
    in.sprint = true;
    for (int i = 0; i < 30; i++)
        step_with(&g, &in);
    CHECK_NEAR(g.player.transform.pos.z, GRAVE_PLAYER_SPRINT_SPEED * 0.5f, 0.03f,
               "sprint distance");
    CHECK(g.player.transform.pos.z > 2.5f);
    CHECK(g.player.stamina < 95.0f);
    CHECK(g.player.anim_speed > 1.2f);

    /* idle player doesn't move and the walk phase stays put */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    grave_input_reset(&in);
    for (int i = 0; i < 30; i++)
        step_with(&g, &in);
    CHECK_NEAR(g.player.transform.pos.z, 0.0f, 0.0001f, "idle no drift");
    CHECK_NEAR(g.player.anim_speed, 0.0f, 0.0001f, "idle anim speed");

    /* zone bounds clamp movement (arena is 40x40, 0.8 m of margin) */
    quiet_arena(&g, 0.0f, 19.0f, 0.0f, 0.0f);
    clear_props(&g);
    grave_input_reset(&in);
    in.move_y = 1.0f;
    for (int i = 0; i < 120; i++)
        step_with(&g, &in);
    CHECK_NEAR(g.player.transform.pos.z, 19.2f, 0.01f, "south bound clamp");

    quiet_arena(&g, -19.0f, 0.0f, GRAVE_PI * 0.5f, 0.0f);
    clear_props(&g);
    grave_input_reset(&in);
    in.move_y = -1.0f; /* backwards along +x -> -x */
    for (int i = 0; i < 120; i++)
        step_with(&g, &in);
    CHECK_NEAR(g.player.transform.pos.x, -19.2f, 0.01f, "west bound clamp");

    /* hero helpers consumed by the renderer */
    quiet_arena(&g, 1.0f, 2.0f, 0.4f, 0.0f);
    clear_props(&g);
    {
        Vec3 root;
        float root_yaw = 0.0f;
        grave_hero_root(&g, &root, &root_yaw);
        CHECK_NEAR(root.x, 1.0f, 0.0001f, "hero root x");
        CHECK_NEAR(root.z, 2.0f, 0.0001f, "hero root z");
        CHECK_NEAR(root_yaw, 0.4f, 0.0001f, "hero root yaw");
        CHECK(grave_hero_walk_phase(&g) >= 0.0f);
        grave_hero_root(&g, NULL, NULL);
        CHECK(true);
    }
}

static void test_collision(void)
{
    GraveGame g;
    GraveInput in;

    /* walk straight into a crate: circle vs AABB stops at 1.0 - 0.35 */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    add_test_prop(&g, vec3_make(0.0f, 0.0f, 2.0f), vec3_make(1.0f, 1.0f, 1.0f), PROP_CRATE);
    grave_input_reset(&in);
    in.move_y = 1.0f;
    for (int i = 0; i < 120; i++)
        step_with(&g, &in);
    CHECK_NEAR(g.player.transform.pos.z, 0.65f, 0.02f, "crate collision stop");
    CHECK(g.player.transform.pos.z < 1.0f);

    /* diagonal into a long wall slides along +x instead of stopping dead */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    add_test_prop(&g, vec3_make(0.0f, 0.0f, 2.0f), vec3_make(10.0f, 1.0f, 1.0f), PROP_FENCE);
    grave_input_reset(&in);
    in.move_x = 1.0f;
    in.move_y = 1.0f;
    for (int i = 0; i < 120; i++)
        step_with(&g, &in);
    CHECK(g.player.transform.pos.x > 2.0f);
    CHECK(g.player.transform.pos.z <= 0.7f);

    /* walking into the corner of a barrel */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    add_test_prop(&g, vec3_make(1.5f, 0.0f, 1.5f), vec3_make(0.4f, 0.6f, 0.4f), PROP_BARREL);
    grave_input_reset(&in);
    in.move_x = 0.0f;
    in.move_y = 1.0f;
    for (int i = 0; i < 120; i++)
        step_with(&g, &in);
    CHECK(!blocked_by_props_xz(&g, g.player.transform.pos.x, g.player.transform.pos.z,
                               GRAVE_PLAYER_RADIUS - 0.01f));

    /* arena props never actually contain the spawn point */
    quiet_arena(&g, 0.0f, 15.5f, GRAVE_PI, 0.0f);
    CHECK(!blocked_by_props_xz(&g, 0.0f, 15.5f, GRAVE_PLAYER_RADIUS));
}

static void test_look(void)
{
    GraveGame g;
    GraveInput in;

    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    grave_input_reset(&in);
    in.look_x = 1.0f;
    step_with(&g, &in);
    CHECK_NEAR(g.player.transform.yaw, GRAVE_LOOK_YAW_PER_UNIT, 0.001f, "touch yaw");

    grave_input_reset(&in);
    in.look_y = 1.0f; /* drag down = look down, clamped at the pitch limit */
    step_with(&g, &in);
    CHECK_NEAR(g.player.transform.pitch, -GRAVE_PITCH_LIMIT, 0.001f,
               "drag down clamps to pitch limit");

    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    grave_input_reset(&in);
    in.look_y = -1.0f;
    for (int i = 0; i < 30; i++)
        step_with(&g, &in);
    CHECK_NEAR(g.player.transform.pitch, GRAVE_PITCH_LIMIT, 0.001f, "pitch clamp up");

    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    grave_input_reset(&in);
    in.look_y = 1.0f;
    for (int i = 0; i < 30; i++)
        step_with(&g, &in);
    CHECK_NEAR(g.player.transform.pitch, -GRAVE_PITCH_LIMIT, 0.001f, "pitch clamp down");

    /* yaw stays wrapped to [-pi, pi] */
    quiet_arena(&g, 0.0f, 0.0f, 3.0f, 0.0f);
    clear_props(&g);
    grave_input_reset(&in);
    in.look_x = 1.0f;
    step_with(&g, &in);
    CHECK(g.player.transform.yaw >= -GRAVE_PI - 0.001f);
    CHECK(g.player.transform.yaw <= GRAVE_PI + 0.001f);
    CHECK_NEAR(g.player.transform.yaw, grave_wrap_pi(3.0f + GRAVE_LOOK_YAW_PER_UNIT),
               0.001f, "yaw wrapped");
}

/* ================================================================== */
/* weapons                                                             */
/* ================================================================== */

static int steps_until_ammo(GraveGame *g, WeaponId id, int max_steps)
{
    for (int i = 0; i < max_steps; i++) {
        step_idle(g);
        if (g->player.ammo[id] > 0)
            return i + 1;
    }
    return -1;
}

static void test_weapons(void)
{
    GraveGame g;
    GraveInput in;

    /* flintlock: one shot, then an automatic reload */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    clear_events(&g);
    fire_once(&g);
    CHECK_NEAR(g.player.ammo[WEAPON_FLINTLOCK], 0.0f, 0.001f, "flintlock spent");
    CHECK((g.events & GRAVE_EVT_SHOOT) != 0u);
    CHECK((g.events & GRAVE_EVT_RELOAD) != 0u);
    CHECK_NEAR(g.player.fire_cooldown, 0.9f, 0.001f, "flintlock interval");
    CHECK_NEAR(g.player.reload_timer, 1.3f, 0.001f, "flintlock auto reload");
    CHECK(g.player.reload_weapon == WEAPON_FLINTLOCK);
    CHECK(g.player.muzzle_flash > 0.0f);
    CHECK(g.player.firing);
    CHECK(g.player.recoil > 0.0f);
    CHECK(count_traces(&g, 0) == 1);
    CHECK(count_traces(&g, 1) == 0);
    CHECK(g.shot_traces[0].end.z > 10.0f); /* tracer runs downrange */

    /* trigger during a reload is ignored */
    clear_events(&g);
    fire_once(&g);
    CHECK_NEAR(g.player.ammo[WEAPON_FLINTLOCK], 0.0f, 0.001f, "no shot while reloading");
    CHECK((g.events & GRAVE_EVT_SHOOT) == 0u);
    CHECK((g.events & GRAVE_EVT_EMPTY_GUN) == 0u);

    /* reload finishes exactly at reload_time */
    {
        int steps = steps_until_ammo(&g, WEAPON_FLINTLOCK, 400);
        CHECK(steps >= 70 && steps <= 82); /* 1.3 s == 78 steps */
        CHECK_NEAR(g.player.ammo[WEAPON_FLINTLOCK], 1.0f, 0.001f, "reloaded round");
        CHECK(g.player.reload_timer == 0.0f);
    }

    /* manual reload on a full gun is a no-op */
    clear_events(&g);
    reload_once(&g);
    CHECK(g.player.reload_timer == 0.0f);
    CHECK((g.events & GRAVE_EVT_RELOAD) == 0u);

    /* revolver: six rounds at the spec interval */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    set_weapon(&g, WEAPON_REVOLVER);
    grave_input_reset(&in);
    in.fire = true;
    for (int i = 0; i < 70; i++)
        step_with(&g, &in);
    /* shots land on steps 1, 21, 41, 61 -> four rounds of six */
    CHECK_NEAR(g.player.ammo[WEAPON_REVOLVER], 2.0f, 0.001f, "revolver four shots");
    CHECK(g.player.fire_cooldown >= 0.0f);

    /* six rounds empty the cylinder and start the reload */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    set_weapon(&g, WEAPON_REVOLVER);
    grave_input_reset(&in);
    in.fire = true;
    for (int i = 0; i < 200; i++)
        step_with(&g, &in);
    CHECK_NEAR(g.player.ammo[WEAPON_REVOLVER], 0.0f, 0.001f, "revolver empty");
    CHECK(g.player.reload_timer > 0.0f);

    /* clicking an empty gun: event + auto reload */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    set_weapon(&g, WEAPON_REVOLVER);
    g.player.ammo[WEAPON_REVOLVER] = 0;
    clear_events(&g);
    fire_once(&g);
    CHECK((g.events & GRAVE_EVT_EMPTY_GUN) != 0u);
    CHECK((g.events & GRAVE_EVT_RELOAD) != 0u);
    CHECK_NEAR(g.player.reload_timer, 2.1f, 0.001f, "empty click reload timer");
    CHECK_NEAR(g.player.ammo[WEAPON_REVOLVER], 0.0f, 0.001f, "empty click no ammo");

    /* musket reload timing (2.6 s) */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    set_weapon(&g, WEAPON_MUSKET);
    g.player.ammo[WEAPON_MUSKET] = 0;
    clear_events(&g);
    reload_once(&g);
    CHECK_NEAR(g.player.reload_timer, 2.6f, 0.001f, "musket reload timer");
    CHECK((g.events & GRAVE_EVT_RELOAD) != 0u);
    {
        int steps = steps_until_ammo(&g, WEAPON_MUSKET, 400);
        CHECK(steps >= 150 && steps <= 162); /* 2.6 s == 156 steps */
    }

    /* the app owns weapon buttons: writing player.weapon is honoured by the
     * core, and swapping mid-reload cancels the reload (no free ammo) */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    fire_once(&g); /* flintlock now reloading */
    CHECK(g.player.reload_timer > 0.0f);
    g.player.weapon = WEAPON_REVOLVER; /* app-side swap */
    step_idle(&g);
    CHECK(g.player.reload_timer == 0.0f);
    CHECK_NEAR(g.player.ammo[WEAPON_REVOLVER],
               (float)grave_weapon_spec(WEAPON_REVOLVER)->magazine, 0.001f,
               "swap mid reload: new gun keeps its ammo");
    CHECK_NEAR(g.player.ammo[WEAPON_FLINTLOCK], 0.0f, 0.001f,
               "swap mid reload: old gun stays empty");

    /* a garbage weapon id is normalised, not trusted */
    g.player.weapon = (WeaponId)99;
    step_idle(&g);
    CHECK(g.player.weapon == WEAPON_FLINTLOCK);

    /* X button: GraveInput.swap_weapon is an edge tap that cycles the gun */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    set_weapon(&g, WEAPON_FLINTLOCK);
    {
        GraveInput sw;
        grave_input_reset(&sw);
        sw.swap_weapon = true;
        step_with(&g, &sw);
    }
    CHECK(g.player.weapon == WEAPON_REVOLVER);
    {
        GraveInput sw;
        grave_input_reset(&sw);
        sw.swap_weapon = true;
        step_with(&g, &sw);
    }
    CHECK(g.player.weapon == WEAPON_MUSKET);
    {
        GraveInput sw;
        grave_input_reset(&sw);
        sw.swap_weapon = true;
        step_with(&g, &sw);
    }
    CHECK(g.player.weapon == WEAPON_FLINTLOCK);

    /* swapping out of an in-flight reload cancels it, no free rounds */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    fire_once(&g); /* flintlock starts its auto reload */
    CHECK(g.player.reload_timer > 0.0f);
    {
        GraveInput sw;
        grave_input_reset(&sw);
        sw.swap_weapon = true;
        step_with(&g, &sw);
    }
    CHECK(g.player.weapon == WEAPON_REVOLVER);
    CHECK(g.player.reload_timer == 0.0f);
    CHECK_NEAR(g.player.ammo[WEAPON_FLINTLOCK], 0.0f, 0.001f, "cancel keeps it empty");
    CHECK_NEAR(g.player.ammo[WEAPON_REVOLVER],
               (float)grave_weapon_spec(WEAPON_REVOLVER)->magazine, 0.001f,
               "new gun keeps its ammo");

    /* Y button: GraveInput.toggle_camera is an edge tap */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    CHECK(!g.player.camera_first_person);
    {
        GraveInput tc;
        grave_input_reset(&tc);
        tc.toggle_camera = true;
        step_with(&g, &tc);
    }
    CHECK(g.player.camera_first_person);
    grave_update_camera(&g, 400.0f / 240.0f);
    CHECK(g.camera_fov == GRAVE_CAM_FOV_FP);
    {
        GraveInput tc;
        grave_input_reset(&tc);
        tc.toggle_camera = true;
        step_with(&g, &tc);
    }
    CHECK(!g.player.camera_first_person);
    grave_update_camera(&g, 400.0f / 240.0f);
    CHECK(g.camera_fov == GRAVE_CAM_FOV_TP);

    /* events are sticky until the app clears them (documented convention) */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    clear_events(&g);
    fire_once(&g);
    CHECK((g.events & GRAVE_EVT_SHOOT) != 0u);
    step_idle(&g); /* quiet frame: core does not clear the word */
    CHECK((g.events & GRAVE_EVT_SHOOT) != 0u);
    clear_events(&g);
    step_idle(&g);
    CHECK(g.events == 0u);
}

/* ================================================================== */
/* hitscan                                                             */
/* ================================================================== */

static void test_hitscan(void)
{
    GraveGame g;
    const ZombieSpec *shambler = grave_zombie_spec(ZOMBIE_SHAMBLER);
    const ZombieSpec *brute = grave_zombie_spec(ZOMBIE_BRUTE);

    /* body hit at range: flintlock 34 vs 44 hp shambler */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    place_zombie(&g, 0, ZOMBIE_SHAMBLER, vec3_make(0.0f, 0.0f, 5.0f), ZOMBIE_CHASE);
    g.player.transform.pitch = aim_pitch_at(0.8f, 5.0f);
    set_weapon(&g, WEAPON_FLINTLOCK);
    clear_events(&g);
    fire_once(&g);
    CHECK_NEAR(g.zombies[0].hp, 44.0f - 34.0f, 0.5f, "flintlock body damage");
    CHECK((g.events & GRAVE_EVT_HIT) != 0u);
    CHECK((g.events & GRAVE_EVT_SHOOT) != 0u);
    CHECK(count_traces(&g, 1) == 1); /* impact spark on a zombie hit */

    /* head-ish shot: flintlock headshot 51 > 44 one-shots a shambler */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    place_zombie(&g, 0, ZOMBIE_SHAMBLER, vec3_make(0.0f, 0.0f, 2.0f), ZOMBIE_CHASE);
    g.player.transform.pitch = aim_pitch_at(shambler->body_height - 0.13f, 2.0f);
    set_weapon(&g, WEAPON_FLINTLOCK);
    clear_events(&g);
    fire_once(&g);
    CHECK(g.zombies_killed == 1);
    CHECK(g.zombies[0].state == ZOMBIE_DYING);
    CHECK((g.events & GRAVE_EVT_ZOMBIE_DIE) != 0u);
    CHECK(count_traces(&g, 1) == 1);

    /* brute headshot does 55 * 1.5, not 55.  Aiming needs to clear the wide
     * body box, so this is a ranged shot slightly above the head centre. */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    place_zombie(&g, 0, ZOMBIE_BRUTE, vec3_make(0.0f, 0.0f, 5.0f), ZOMBIE_CHASE);
    {
        float head_y = brute->body_height - 0.13f;
        g.player.transform.pitch = aim_pitch_at(head_y + 0.07f, 5.0f);
    }
    set_weapon(&g, WEAPON_MUSKET);
    clear_events(&g);
    fire_once(&g);
    CHECK_NEAR(g.zombies[0].hp, brute->hp - 55.0f * 1.5f, 0.5f, "brute headshot damage");

    /* brute body shot does the flat 55 */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    place_zombie(&g, 0, ZOMBIE_BRUTE, vec3_make(0.0f, 0.0f, 5.0f), ZOMBIE_CHASE);
    g.player.transform.pitch = aim_pitch_at(1.1f, 5.0f);
    set_weapon(&g, WEAPON_MUSKET);
    fire_once(&g);
    CHECK_NEAR(g.zombies[0].hp, brute->hp - 55.0f, 0.5f, "brute body damage");

    /* miss: zombie behind the camera */
    quiet_arena(&g, 0.0f, 0.0f, GRAVE_PI * 0.5f, 0.0f);
    clear_props(&g);
    place_zombie(&g, 0, ZOMBIE_SHAMBLER, vec3_make(0.0f, 0.0f, 5.0f), ZOMBIE_CHASE);
    set_weapon(&g, WEAPON_FLINTLOCK);
    clear_events(&g);
    fire_once(&g);
    CHECK_NEAR(g.zombies[0].hp, shambler->hp, 0.001f, "miss leaves hp alone");
    CHECK((g.events & GRAVE_EVT_HIT) == 0u);

    /* out of range: flintlock reaches 26 m, target at 30 m */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    place_zombie(&g, 0, ZOMBIE_SHAMBLER, vec3_make(0.0f, 0.0f, 30.0f), ZOMBIE_CHASE);
    g.player.transform.pitch = aim_pitch_at(0.8f, 30.0f);
    set_weapon(&g, WEAPON_FLINTLOCK);
    clear_events(&g);
    fire_once(&g);
    CHECK_NEAR(g.zombies[0].hp, shambler->hp, 0.001f, "past max range");
    CHECK((g.events & GRAVE_EVT_HIT) == 0u);

    /* a prop between shooter and zombie eats the shot (tall crate) */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    add_test_prop(&g, vec3_make(0.0f, 0.0f, 3.0f), vec3_make(1.0f, 2.0f, 1.0f), PROP_CRATE);
    place_zombie(&g, 0, ZOMBIE_SHAMBLER, vec3_make(0.0f, 0.0f, 6.0f), ZOMBIE_CHASE);
    g.player.transform.pitch = aim_pitch_at(0.9f, 6.0f);
    set_weapon(&g, WEAPON_FLINTLOCK);
    clear_events(&g);
    fire_once(&g);
    CHECK_NEAR(g.zombies[0].hp, shambler->hp, 0.001f, "prop blocks shot");
    CHECK((g.events & GRAVE_EVT_HIT) == 0u);
    CHECK(count_traces(&g, 1) == 1); /* world impact spark */

    /* dying zombies are no longer hittable and cannot be killed twice */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    place_zombie(&g, 0, ZOMBIE_RUNNER, vec3_make(0.0f, 0.0f, 4.0f), ZOMBIE_CHASE);
    g.zombies[0].hp = 1.0f;
    set_weapon(&g, WEAPON_FLINTLOCK);
    fire_once(&g);
    CHECK(g.zombies_killed == 1);
    set_weapon(&g, WEAPON_FLINTLOCK);
    clear_events(&g);
    fire_once(&g);
    CHECK(g.zombies_killed == 1);
    CHECK((g.events & GRAVE_EVT_ZOMBIE_DIE) == 0u);
}

/* ================================================================== */
/* drops and pickups                                                   */
/* ================================================================== */

static void test_drops_pickups(void)
{
    GraveGame g;
    int pickup;

    /* kill 1 -> coin, kill 2 -> sinew (guaranteed quest-drop cycle) */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    kill_slotted_zombie(&g, ZOMBIE_RUNNER, vec3_make(0.0f, 0.0f, 4.0f));
    CHECK(g.zombies_killed == 1);
    CHECK(count_pickups(&g) == 1);
    pickup = first_active_pickup(&g);
    CHECK(pickup >= 0);
    CHECK(g.pickups[pickup].kind == DROP_COIN);
    CHECK(g.pickups[pickup].active);
    CHECK(g.pickups[pickup].life > 0.0f);

    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    g.zombies_killed = 1;
    kill_slotted_zombie(&g, ZOMBIE_RUNNER, vec3_make(0.0f, 0.0f, 4.0f));
    pickup = first_active_pickup(&g);
    CHECK(pickup >= 0);
    CHECK(g.pickups[pickup].kind == DROP_SINEW);

    /* pickups bob, expire, and auto-collect inside the radius */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    pickup = add_test_pickup(&g, DROP_COIN, vec3_make(5.0f, 0.35f, 5.0f), 0.05f);
    CHECK(pickup >= 0);
    {
        float bob = g.pickups[pickup].bob_time;
        float life = g.pickups[pickup].life;
        step_idle(&g);
        CHECK(g.pickups[pickup].bob_time > bob);
        CHECK(g.pickups[pickup].life < life);
    }
    step_n(&g, 5);
    CHECK(!g.pickups[pickup].active);
    CHECK(g.material_coins == 0);

    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    pickup = add_test_pickup(&g, DROP_COIN, vec3_make(0.0f, 0.35f, 2.5f), 30.0f);
    step_idle(&g);
    CHECK(g.pickups[pickup].active); /* outside the 1.55 m radius */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    pickup = add_test_pickup(&g, DROP_COIN, vec3_make(0.0f, 0.35f, 1.0f), 30.0f);
    clear_events(&g);
    step_idle(&g);
    CHECK(!g.pickups[pickup].active);
    CHECK(g.material_coins == 1);
    CHECK(g.run_materials[DROP_COIN] == 1);
    CHECK((g.events & GRAVE_EVT_PICKUP) != 0u);

    /* sinew pickup */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    add_test_pickup(&g, DROP_SINEW, vec3_make(0.0f, 0.35f, 0.5f), 30.0f);
    step_idle(&g);
    CHECK(g.material_sinew == 1);
    CHECK(g.run_materials[DROP_SINEW] == 1);

    /* bone charm pickup (BONE ORCHARD material, v0.2) */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    add_test_pickup(&g, DROP_BONE_CHARM, vec3_make(0.0f, 0.35f, 0.5f), 30.0f);
    step_idle(&g);
    CHECK(g.material_bone_charms == 1);
    CHECK(g.run_materials[DROP_BONE_CHARM] == 1);
    CHECK(grave_inventory_material(&g, DROP_BONE_CHARM) == 1);

    /* powder tops up the current weapon by 2, clamped to the magazine */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    set_weapon(&g, WEAPON_REVOLVER);
    g.player.ammo[WEAPON_REVOLVER] = 2;
    add_test_pickup(&g, DROP_AMMO, vec3_make(0.0f, 0.35f, 0.5f), 30.0f);
    step_idle(&g);
    CHECK(g.player.ammo[WEAPON_REVOLVER] == 4);

    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    set_weapon(&g, WEAPON_REVOLVER);
    g.player.ammo[WEAPON_REVOLVER] = 5;
    add_test_pickup(&g, DROP_AMMO, vec3_make(0.0f, 0.35f, 0.5f), 30.0f);
    step_idle(&g);
    CHECK(g.player.ammo[WEAPON_REVOLVER] ==
          grave_weapon_spec(WEAPON_REVOLVER)->magazine);

    /* full pickup pools never overflow when a zombie dies */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    for (int i = 0; i < MAX_PICKUPS; i++)
        add_test_pickup(&g, DROP_COIN, vec3_make(15.0f, 0.35f, 15.0f), 60.0f);
    CHECK(count_pickups(&g) == MAX_PICKUPS);
    kill_slotted_zombie(&g, ZOMBIE_RUNNER, vec3_make(0.0f, 0.0f, 4.0f));
    CHECK(g.zombies_killed == 1);
    CHECK(count_pickups(&g) == MAX_PICKUPS);

    /* death -> falling body -> dead + pooled, and the kill is counted once */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    kill_slotted_zombie(&g, ZOMBIE_RUNNER, vec3_make(0.0f, 0.0f, 4.0f));
    CHECK(g.zombies[0].active);
    CHECK(g.zombies[0].state == ZOMBIE_DYING);
    CHECK(g.zombies[0].death_time > 0.0f);
    CHECK(g.zombies[0].death_time < 0.05f); /* fall anim just started */
    step_n(&g, 40);
    CHECK(g.zombies[0].active);
    CHECK(g.zombies[0].death_time > 0.3f);
    CHECK(g.zombies[0].death_time < 1.0f);
    step_n(&g, 40);
    CHECK(!g.zombies[0].active);
    CHECK(g.zombies[0].state == ZOMBIE_DEAD);
    CHECK(g.zombies_alive == 0);
}

/* ================================================================== */
/* zombie AI                                                           */
/* ================================================================== */

static int steps_until_hp_change(GraveGame *g, int max_steps)
{
    float before = g->player.hp;
    for (int i = 0; i < max_steps; i++) {
        step_idle(g);
        if (g->player.hp != before)
            return i + 1;
    }
    return -1;
}

static void test_zombie_ai(void)
{
    GraveGame g;
    const ZombieSpec *shambler = grave_zombie_spec(ZOMBIE_SHAMBLER);

    /* IDLE -> CHASE */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    place_zombie(&g, 0, ZOMBIE_SHAMBLER, vec3_make(0.0f, 0.0f, 8.0f), ZOMBIE_IDLE);
    g.zombies[0].attack_timer = 0.1f;
    CHECK(g.zombies[0].state == ZOMBIE_IDLE);
    step_n(&g, 8);
    CHECK(g.zombies[0].state == ZOMBIE_CHASE);

    /* CHASE closes the distance */
    {
        float before = dist_xz(g.zombies[0].pos, g.player.transform.pos);
        step_n(&g, 60);
        float after = dist_xz(g.zombies[0].pos, g.player.transform.pos);
        CHECK(after < before - 0.5f);
        CHECK(after > before - 2.0f);
    }

    /* CHASE -> ATTACK windup -> damage -> cooldown -> second swipe */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    place_zombie(&g, 0, ZOMBIE_SHAMBLER, vec3_make(0.0f, 0.0f, 1.0f), ZOMBIE_CHASE);
    step_idle(&g);
    CHECK(g.zombies[0].state == ZOMBIE_ATTACK);
    CHECK_NEAR(g.zombies[0].attack_timer, shambler->windup, 0.001f, "windup timer");
    clear_events(&g);
    {
        int steps = steps_until_hp_change(&g, 60);
        CHECK(steps > 20 && steps < 40); /* ~0.45 s */
    }
    CHECK_NEAR(g.player.hp, GRAVE_PLAYER_MAX_HP - shambler->damage, 0.001f,
               "shambler swipe damage");
    CHECK((g.events & GRAVE_EVT_HURT) != 0u);
    {
        int steps2 = steps_until_hp_change(&g, 160);
        CHECK(steps2 > 75 && steps2 < 105); /* ~1.5 s cooldown */
    }
    CHECK_NEAR(g.player.hp, GRAVE_PLAYER_MAX_HP - 2.0f * shambler->damage, 0.001f,
               "second swipe damage");

    /* backing off aborts the attack */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    place_zombie(&g, 0, ZOMBIE_SHAMBLER, vec3_make(0.0f, 0.0f, 1.0f), ZOMBIE_CHASE);
    step_idle(&g);
    CHECK(g.zombies[0].state == ZOMBIE_ATTACK);
    g.player.transform.pos = vec3_make(0.0f, 0.0f, 10.0f);
    step_idle(&g);
    CHECK(g.zombies[0].state == ZOMBIE_CHASE);

    /* stagger interrupts the chase, then recovers */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    place_zombie(&g, 0, ZOMBIE_RUNNER, vec3_make(0.0f, 0.0f, 4.0f), ZOMBIE_CHASE);
    g.player.transform.pitch = aim_pitch_at(0.8f, 4.0f);
    set_weapon(&g, WEAPON_REVOLVER);
    fire_once(&g);
    CHECK(g.zombies[0].hp < grave_zombie_spec(ZOMBIE_RUNNER)->hp);
    CHECK(g.zombies[0].state == ZOMBIE_STAGGER);
    CHECK(g.zombies[0].attack_timer > 0.25f);
    CHECK(g.zombies[0].attack_timer <= GRAVE_STAGGER_TIME);
    step_n(&g, 20);
    CHECK(g.zombies[0].state == ZOMBIE_CHASE);

    /* separation: two zombies spawned on the same spot push apart */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    place_zombie(&g, 0, ZOMBIE_RUNNER, vec3_make(0.0f, 0.0f, 5.0f), ZOMBIE_CHASE);
    place_zombie(&g, 1, ZOMBIE_RUNNER, vec3_make(0.0f, 0.0f, 5.0f), ZOMBIE_CHASE);
    step_n(&g, 30);
    CHECK(dist_xz(g.zombies[0].pos, g.zombies[1].pos) > 0.4f);

    /* prop avoidance: a zombie walking at the player does not pass through a
     * crate - it is stopped/blocked outside the crate footprint */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    add_test_prop(&g, vec3_make(0.0f, 0.0f, 2.5f), vec3_make(1.0f, 1.0f, 1.0f), PROP_CRATE);
    place_zombie(&g, 0, ZOMBIE_SHAMBLER, vec3_make(0.0f, 0.0f, 5.0f), ZOMBIE_CHASE);
    step_n(&g, 120);
    CHECK(g.zombies[0].pos.z > 3.4f);
    CHECK(!blocked_by_props_xz(&g, g.zombies[0].pos.x, g.zombies[0].pos.z,
                               shambler->radius - 0.01f));

    /* zombies stay inside the arena bounds */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    place_zombie(&g, 0, ZOMBIE_SHAMBLER, vec3_make(19.0f, 0.0f, 0.0f), ZOMBIE_CHASE);
    step_n(&g, 600);
    CHECK(fabsf(g.zombies[0].pos.x) <= 19.25f);
    CHECK(fabsf(g.zombies[0].pos.z) <= 19.25f);
}

/* ================================================================== */
/* spawner                                                             */
/* ================================================================== */

static bool all_zombies_clear_of_props(const GraveGame *g)
{
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        const Zombie *z = &g->zombies[i];
        if (!z->active)
            continue;
        if (z->state == ZOMBIE_DYING || z->state == ZOMBIE_DEAD)
            continue;
        /* the core pushes circles out to exactly their radius, so a zombie
         * resting against a prop is legal; only real penetration is a bug */
        if (blocked_by_props_xz(g, z->pos.x, z->pos.z,
                                grave_zombie_spec(z->type)->radius - 0.01f))
            return false;
    }
    return true;
}

static bool all_zombies_far_from_player(const GraveGame *g)
{
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        const Zombie *z = &g->zombies[i];
        if (!z->active)
            continue;
        if (dist_xz(z->pos, g->player.transform.pos) < 6.0f)
            return false;
    }
    return true;
}

static void test_spawner(void)
{
    GraveGame g;
    const ZoneSpec *zs = grave_zone_spec(ZONE_ASHEN_MILL);
    int max_seen = 0;
    int spawned_total;

    quiet_arena(&g, 0.0f, 15.5f, GRAVE_PI, 0.0f);
    g.spawn_budget = zs->spawn_budget;
    g.spawn_kill_goal = zs->kill_goal;
    g.spawn_timer = 0.0f;

    step_idle(&g); /* first wave */
    CHECK(count_zombies(&g) == 3);
    CHECK(g.zombies_alive == 3);
    CHECK(g.spawn_budget == zs->spawn_budget - 3);
    CHECK(all_zombies_clear_of_props(&g));
    CHECK(all_zombies_far_from_player(&g)); /* not spawn-camped */
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        if (g.zombies[i].active) {
            CHECK((int)g.zombies[i].type >= 0 && (int)g.zombies[i].type < ZOMBIE_COUNT);
            CHECK(g.zombies[i].state == ZOMBIE_IDLE);
            CHECK(g.zombies[i].hp > 0.0f);
            CHECK(g.zombies[i].yaw >= -GRAVE_PI - 0.01f);
            CHECK(g.zombies[i].yaw <= GRAVE_PI + 0.01f);
        }
    }

    /* idle (unkillable) player: the alive cap holds and nobody is stuck in a
     * prop; the wave budget keeps retrying instead of over-spawning */
    for (int i = 0; i < 1800; i++) {
        g.player.hp = 10000.0f;
        g.player.alive = true;
        step_idle(&g);
        if (count_zombies(&g) > max_seen)
            max_seen = count_zombies(&g);
        CHECK(g.zombies_alive == count_zombies(&g));
    }
    CHECK(max_seen <= zs->max_alive);
    CHECK(max_seen >= 8); /* waves actually fill the yard */
    CHECK(all_zombies_clear_of_props(&g));
    spawned_total = zs->spawn_budget - g.spawn_budget;
    CHECK(spawned_total <= zs->spawn_budget);
    CHECK(spawned_total >= 6);
}

/* ================================================================== */
/* quest                                                               */
/* ================================================================== */

static void test_quest(void)
{
    GraveGame g;
    const ZoneSpec *zs = grave_zone_spec(ZONE_ASHEN_MILL);

    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    CHECK(g.quest.kind == QUEST_CLEAR_ZONE);
    CHECK(g.quest.target == zs->kill_goal);
    CHECK(g.quest.progress == 0);
    CHECK(g.quest.material == DROP_NONE);
    CHECK(g.phase == GAME_PHASE_PLAYING);

    g.zombies_killed = zs->kill_goal / 2;
    step_idle(&g);
    CHECK(g.quest.kind == QUEST_CLEAR_ZONE);
    CHECK(g.quest.progress == g.zombies_killed);

    /* kill step done -> collect material A (generic, zone-driven) */
    g.zombies_killed = zs->kill_goal;
    step_idle(&g);
    CHECK(g.quest.kind == QUEST_COLLECT);
    CHECK(g.quest.material == (int)zs->material_a);
    CHECK(g.quest.target == zs->goal_a);
    CHECK(g.quest.progress == 0);

    g.run_materials[zs->material_a] = zs->goal_a / 3;
    step_idle(&g);
    CHECK(g.quest.kind == QUEST_COLLECT);
    CHECK(g.quest.progress == g.run_materials[zs->material_a]);
    CHECK(g.phase == GAME_PHASE_PLAYING);

    /* material A done -> collect material B */
    g.run_materials[zs->material_a] = zs->goal_a;
    step_idle(&g);
    CHECK(g.quest.material == (int)zs->material_b);
    CHECK(g.quest.target == zs->goal_b);
    CHECK(g.quest.progress == 0);
    CHECK(g.phase == GAME_PHASE_PLAYING);

    g.run_materials[zs->material_b] = zs->goal_b / 2;
    step_idle(&g);
    CHECK(g.quest.progress == g.run_materials[zs->material_b]);
    CHECK(g.phase == GAME_PHASE_PLAYING);

    clear_events(&g);
    g.run_materials[zs->material_b] = zs->goal_b;
    step_idle(&g);
    CHECK(g.quest.kind == QUEST_DONE);
    CHECK(g.phase == GAME_PHASE_QUEST_COMPLETE);
    CHECK((g.events & GRAVE_EVT_QUEST_DONE) != 0u);
    CHECK((g.events & GRAVE_EVT_ZONE_DONE) != 0u);

    /* v0.2 progression: zone marked cleared, best time recorded, next zone
     * unlocked, the one after that still locked */
    CHECK(g.world.nodes[ZONE_ASHEN_MILL].cleared);
    CHECK(g.world.nodes[ZONE_ASHEN_MILL].clears == 1);
    CHECK(g.world.nodes[ZONE_ASHEN_MILL].best_time > 0.0f);
    CHECK(g.world.nodes[ZONE_BONE_ORCHARD].unlocked);
    CHECK(!g.world.nodes[ZONE_BONE_ORCHARD].cleared);
    CHECK(!g.world.nodes[ZONE_OLD_MINE].unlocked);

    /* results screen: stepping is frozen while QUEST_COMPLETE */
    {
        uint32_t frame = g.frame;
        float t = g.zone_time;
        step_n(&g, 10);
        CHECK(g.frame == frame);
        CHECK(g.zone_time == t);
        CHECK(g.phase == GAME_PHASE_QUEST_COMPLETE);
    }

    /* all conditions met in a single step hop through every stage */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    g.zombies_killed = zs->kill_goal;
    g.run_materials[zs->material_a] = zs->goal_a;
    g.run_materials[zs->material_b] = zs->goal_b;
    step_idle(&g);
    CHECK(g.quest.kind == QUEST_DONE);
    CHECK(g.phase == GAME_PHASE_QUEST_COMPLETE);

    /* not complete if the kill goal is short, even with the goods */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    g.zombies_killed = zs->kill_goal - 1;
    g.run_materials[zs->material_a] = zs->goal_a;
    g.run_materials[zs->material_b] = zs->goal_b;
    step_idle(&g);
    CHECK(g.phase == GAME_PHASE_PLAYING);
    CHECK(g.quest.progress == zs->kill_goal - 1);
}

/* ================================================================== */
/* world progression (v0.2)                                            */
/* ================================================================== */

static void test_world_progression(void)
{
    GraveGame g;
    const ZoneSpec *ashen = grave_zone_spec(ZONE_ASHEN_MILL);
    const ZoneSpec *orchard = grave_zone_spec(ZONE_BONE_ORCHARD);

    grave_game_init(&g, TEST_SEED);
    CHECK(grave_world_unlocked(&g, ZONE_ASHEN_MILL));
    CHECK(!grave_world_unlocked(&g, ZONE_BONE_ORCHARD));
    CHECK(!grave_world_unlocked(&g, ZONE_OLD_MINE));
    CHECK(!grave_world_cleared(&g, ZONE_ASHEN_MILL));
    CHECK(!g.world.in_zone);

    /* locked zones can't be entered and leave the game untouched */
    grave_game_start_zone(&g, ZONE_BONE_ORCHARD);
    CHECK(g.phase == GAME_PHASE_TITLE);
    CHECK(!g.world.in_zone);
    CHECK(g.prop_count == 0);
    CHECK(g.zone == ZONE_ASHEN_MILL);
    CHECK(!grave_game_travel_to(&g, ZONE_BONE_ORCHARD));
    CHECK(!grave_game_travel_to(&g, (ZoneId)99));

    /* the mill is the first stop and starts normally */
    grave_game_start_zone(&g, ZONE_ASHEN_MILL);
    CHECK(g.world.in_zone && g.world.current == ZONE_ASHEN_MILL);
    CHECK(g.zone_runs == 1);
    CHECK(g.quest.target == ashen->kill_goal);

    /* back to the trail/world map */
    grave_world_to_trail(&g);
    CHECK(!g.world.in_zone);
    CHECK(g.phase == GAME_PHASE_TITLE);
    CHECK(g.world.current == ZONE_ASHEN_MILL);

    /* finish the mill quest: it clears the node and unlocks the orchard */
    grave_game_start_zone(&g, ZONE_ASHEN_MILL);
    g.zombies_killed = ashen->kill_goal;
    g.run_materials[ashen->material_a] = ashen->goal_a;
    g.run_materials[ashen->material_b] = ashen->goal_b;
    step_idle(&g);
    CHECK(g.phase == GAME_PHASE_QUEST_COMPLETE);
    CHECK(grave_world_cleared(&g, ZONE_ASHEN_MILL));
    CHECK(grave_world_unlocked(&g, ZONE_BONE_ORCHARD));
    CHECK(!grave_world_unlocked(&g, ZONE_OLD_MINE));
    CHECK(g.world.nodes[ZONE_ASHEN_MILL].best_time > 0.0f);

    /* travel to the newly unlocked zone */
    grave_world_to_trail(&g);
    CHECK(grave_game_travel_to(&g, ZONE_BONE_ORCHARD));
    CHECK(g.zone == ZONE_BONE_ORCHARD);
    CHECK(g.world.in_zone && g.world.current == ZONE_BONE_ORCHARD);
    CHECK(g.zone_runs == 3);
    CHECK(g.quest.kind == QUEST_CLEAR_ZONE);
    CHECK(g.quest.target == orchard->kill_goal);
    CHECK(g.prop_count > 100);
    CHECK(g.world.nodes[ZONE_ASHEN_MILL].cleared); /* still remembered */

    /* the old mine is still locked; the unlock helper opens it */
    CHECK(!grave_game_travel_to(&g, ZONE_OLD_MINE));
    CHECK(grave_world_unlock(&g, ZONE_OLD_MINE));
    CHECK(grave_world_unlocked(&g, ZONE_OLD_MINE));
    CHECK(grave_game_travel_to(&g, ZONE_OLD_MINE));
    CHECK(g.zone == ZONE_OLD_MINE);
    CHECK(g.world.current == ZONE_OLD_MINE);

    CHECK(!grave_world_unlock(&g, (ZoneId)99));
    CHECK(!grave_world_unlocked(NULL, ZONE_ASHEN_MILL));
    CHECK(!grave_world_cleared(NULL, ZONE_ASHEN_MILL));
    CHECK(!grave_game_travel_to(NULL, ZONE_ASHEN_MILL));
    grave_world_to_trail(NULL);
}

/* ================================================================== */
/* BONE ORCHARD (v0.2 zone)                                            */
/* ================================================================== */

static void test_bone_orchard(void)
{
    GraveGame g;
    const ZoneSpec *zs = grave_zone_spec(ZONE_BONE_ORCHARD);
    int tombstones = 0;
    int trees = 0;
    int gate_fence = 0;

    grave_game_init(&g, TEST_SEED);
    CHECK(grave_world_unlock(&g, ZONE_BONE_ORCHARD));
    CHECK(grave_game_travel_to(&g, ZONE_BONE_ORCHARD));
    CHECK(g.zone == ZONE_BONE_ORCHARD);
    CHECK(g.phase == GAME_PHASE_PLAYING);
    CHECK(g.quest.kind == QUEST_CLEAR_ZONE);
    CHECK(g.quest.target == zs->kill_goal);
    CHECK(g.quest.target >= 50);
    CHECK(g.zone_runs == 1);
    CHECK(g.prop_count >= 100 && g.prop_count <= MAX_PROPS);
    CHECK(!blocked_by_props_xz(&g, g.player.transform.pos.x,
                               g.player.transform.pos.z, GRAVE_PLAYER_RADIUS));

    for (int i = 0; i < g.prop_count; i++) {
        const Prop *p = &g.props[i];
        CHECK(p->half.x > 0.0f && p->half.y > 0.0f && p->half.z > 0.0f);
        CHECK((int)p->kind >= 0 && (int)p->kind < PROP_KIND_COUNT);
        CHECK(fabsf(p->pos.x) <= 20.5f && fabsf(p->pos.z) <= 20.5f);
        if (p->kind == PROP_TOMBSTONE)
            tombstones++;
        if (p->kind == PROP_DEADTREE)
            trees++;
        if (p->kind == PROP_FENCE && p->pos.z > 19.0f && fabsf(p->pos.x) < 2.0f)
            gate_fence++;
    }
    CHECK(tombstones >= 40); /* much denser than the mill */
    CHECK(trees >= 8);
    CHECK(gate_fence == 0);
    CHECK(tombstones > 5);

    /* BONE ORCHARD is the runner-heavy, brute-per-wave zone */
    CHECK(zs->brute_per_wave == 1);
    CHECK(zs->runner_share > grave_zone_spec(ZONE_ASHEN_MILL)->runner_share);
    CHECK(zs->kill_goal != grave_zone_spec(ZONE_ASHEN_MILL)->kill_goal);

    /* first wave contains exactly one brute (the rest roll normally) */
    memset(g.zombies, 0, sizeof(g.zombies));
    memset(g.pickups, 0, sizeof(g.pickups));
    g.zombies_alive = 0;
    g.spawn_budget = zs->spawn_budget;
    g.spawn_timer = 0.0f;
    step_idle(&g);
    CHECK(count_zombies(&g) == 3);
    {
        int brutes = 0;
        for (int i = 0; i < MAX_ZOMBIES; i++) {
            if (g.zombies[i].active && g.zombies[i].type == ZOMBIE_BRUTE)
                brutes++;
        }
        CHECK(brutes == 1);
    }

    /* drops: kill 1 -> material A (coin), kill 2 -> material B (bone charm).
     * Player fires down the clear entrance lane between the gravestone rows. */
    memset(g.zombies, 0, sizeof(g.zombies));
    memset(g.pickups, 0, sizeof(g.pickups));
    g.zombies_alive = 0;
    g.zombies_killed = 0;
    g.spawn_budget = 0;
    g.spawn_timer = 1.0e9f;
    g.player.transform.pos = vec3_make(0.0f, 0.0f, 14.5f);
    g.player.transform.yaw = GRAVE_PI;
    g.player.transform.pitch = aim_pitch_at(0.8f, 4.0f);
    place_zombie(&g, 0, ZOMBIE_RUNNER, vec3_make(0.0f, 0.0f, 10.5f), ZOMBIE_CHASE);
    g.zombies[0].hp = 1.0f;
    set_weapon(&g, WEAPON_FLINTLOCK);
    g.player.ammo[WEAPON_FLINTLOCK] = 1;
    fire_once(&g);
    CHECK(g.zombies_killed == 1);
    CHECK(first_active_pickup(&g) >= 0);
    CHECK(g.pickups[first_active_pickup(&g)].kind == zs->material_a);

    memset(g.zombies, 0, sizeof(g.zombies));
    memset(g.pickups, 0, sizeof(g.pickups));
    g.zombies_alive = 0;
    g.zombies_killed = 1;
    g.player.transform.pitch = aim_pitch_at(0.8f, 4.0f);
    place_zombie(&g, 0, ZOMBIE_RUNNER, vec3_make(0.0f, 0.0f, 10.5f), ZOMBIE_CHASE);
    g.zombies[0].hp = 1.0f;
    set_weapon(&g, WEAPON_FLINTLOCK);
    g.player.ammo[WEAPON_FLINTLOCK] = 1;
    fire_once(&g);
    CHECK(first_active_pickup(&g) >= 0);
    CHECK(g.pickups[first_active_pickup(&g)].kind == DROP_BONE_CHARM);

    /* staged quest: kills -> coins -> bone charms -> cleared + old mine */
    g.zombies_killed = zs->kill_goal;
    step_idle(&g);
    CHECK(g.quest.kind == QUEST_COLLECT);
    CHECK(g.quest.material == (int)zs->material_a);
    CHECK(g.quest.target == zs->goal_a);
    g.run_materials[zs->material_a] = zs->goal_a;
    step_idle(&g);
    CHECK(g.quest.material == (int)zs->material_b);
    CHECK(g.quest.target == zs->goal_b);
    g.run_materials[zs->material_b] = zs->goal_b;
    g.zone_time = 42.0f;
    clear_events(&g);
    step_idle(&g);
    CHECK(g.quest.kind == QUEST_DONE);
    CHECK(g.phase == GAME_PHASE_QUEST_COMPLETE);
    CHECK((g.events & GRAVE_EVT_ZONE_DONE) != 0u);
    CHECK(grave_world_cleared(&g, ZONE_BONE_ORCHARD));
    CHECK(grave_world_unlocked(&g, ZONE_OLD_MINE));
    CHECK_NEAR(g.world.nodes[ZONE_BONE_ORCHARD].best_time, 42.0f + TEST_DT, 0.01f,
               "orchard best time");
}

/* ================================================================== */
/* save data (v0.2)                                                    */
/* ================================================================== */

static void test_save(void)
{
    GraveGame a;
    GraveGame b;
    uint8_t buf[GRAVE_SAVE_SIZE];
    uint8_t buf2[GRAVE_SAVE_SIZE];
    uint8_t bad[GRAVE_SAVE_SIZE];
    GraveSettings settings;
    GraveSettings loaded;
    SaveData sd;
    int written;

    memset(&settings, 0, sizeof(settings));
    settings.invert_y = 1;
    settings.touch_sens = 3;
    settings.muted = 1;

    grave_game_init(&a, 777u);
    CHECK(grave_world_unlock(&a, ZONE_BONE_ORCHARD));
    a.world.nodes[ZONE_ASHEN_MILL].cleared = true;
    a.world.nodes[ZONE_ASHEN_MILL].clears = 2;
    a.world.nodes[ZONE_ASHEN_MILL].best_time = 95.5f;
    a.world.current = ZONE_BONE_ORCHARD;
    a.material_coins = 17;
    a.material_sinew = 9;
    a.material_bone_charms = 4;
    a.player.weapon_owned[WEAPON_MUSKET] = false;
    a.player.ammo[WEAPON_REVOLVER] = 3;
    a.player.camera_first_person = true;
    a.total_kills = 123;
    a.total_time = 456.5f;
    a.zone_runs = 7;

    written = grave_save_to_buffer(&a, &settings, buf, sizeof(buf));
    CHECK(written == GRAVE_SAVE_SIZE);
    CHECK(written == grave_save_size());
    memcpy(&sd, buf, sizeof(sd));
    CHECK(sd.magic == GRAVE_SAVE_MAGIC);
    CHECK(sd.version == GRAVE_SAVE_VERSION);
    CHECK(sd.bytes == (uint16_t)GRAVE_SAVE_SIZE);
    CHECK(sd.checksum != 0u);

    /* byte-identical images for the same state */
    CHECK(grave_save_to_buffer(&a, &settings, buf2, sizeof(buf2)) == written);
    CHECK(memcmp(buf, buf2, sizeof(buf)) == 0);

    /* a state change changes the bytes */
    a.material_coins++;
    CHECK(grave_save_to_buffer(&a, &settings, buf2, sizeof(buf2)) == written);
    CHECK(memcmp(buf, buf2, sizeof(buf)) != 0);
    a.material_coins--;

    /* bad save arguments */
    CHECK(grave_save_to_buffer(&a, NULL, buf2, 8) == 0);
    CHECK(grave_save_to_buffer(NULL, NULL, buf2, sizeof(buf2)) == 0);

    /* round-trip into a fresh game */
    grave_game_init(&b, 999u);
    memset(&loaded, 0xFF, sizeof(loaded));
    CHECK(grave_load_from_buffer(&b, &loaded, buf, sizeof(buf)));
    CHECK(b.world.current == ZONE_BONE_ORCHARD);
    CHECK(grave_world_unlocked(&b, ZONE_BONE_ORCHARD));
    CHECK(grave_world_cleared(&b, ZONE_ASHEN_MILL));
    CHECK(b.world.nodes[ZONE_ASHEN_MILL].clears == 2);
    CHECK_NEAR(b.world.nodes[ZONE_ASHEN_MILL].best_time, 95.5f, 0.01f,
               "save best time");
    CHECK(b.phase == GAME_PHASE_TITLE);
    CHECK(!b.world.in_zone);
    CHECK(b.material_coins == 17);
    CHECK(b.material_sinew == 9);
    CHECK(b.material_bone_charms == 4);
    CHECK(!b.player.weapon_owned[WEAPON_MUSKET]);
    CHECK(b.player.weapon_owned[WEAPON_FLINTLOCK]);
    CHECK(b.player.weapon_owned[WEAPON_REVOLVER]);
    CHECK(b.player.ammo[WEAPON_REVOLVER] == 3);
    CHECK(b.player.camera_first_person);
    CHECK(b.total_kills == 123);
    CHECK_NEAR(b.total_time, 456.5f, 0.01f, "save play time");
    CHECK(b.zone_runs == 7);
    CHECK(loaded.invert_y == 1);
    CHECK(loaded.touch_sens == 3);
    CHECK(loaded.muted == 1);

    /* a loaded save can be travelled from immediately */
    CHECK(grave_game_travel_to(&b, ZONE_BONE_ORCHARD));
    CHECK(b.zone == ZONE_BONE_ORCHARD);
    CHECK(b.phase == GAME_PHASE_PLAYING);
    CHECK(b.material_coins == 17); /* inventory persists across start_zone */

    /* bad images are rejected and leave the game untouched */
    grave_game_init(&b, 999u);
    {
        uint32_t before = hash_game(&b);

        memcpy(bad, buf, sizeof(bad));
        bad[0] ^= 0xFFu; /* magic */
        CHECK(!grave_load_from_buffer(&b, NULL, bad, sizeof(bad)));
        CHECK(hash_game(&b) == before);

        memcpy(bad, buf, sizeof(bad));
        bad[4] = (uint8_t)(GRAVE_SAVE_VERSION + 1u); /* version */
        CHECK(!grave_load_from_buffer(&b, NULL, bad, sizeof(bad)));
        CHECK(hash_game(&b) == before);

        memcpy(bad, buf, sizeof(bad));
        bad[6] = (uint8_t)(GRAVE_SAVE_SIZE - 1); /* size field */
        CHECK(!grave_load_from_buffer(&b, NULL, bad, sizeof(bad)));
        CHECK(hash_game(&b) == before);

        memcpy(bad, buf, sizeof(bad));
        bad[offsetof(SaveData, material_coins)] ^= 0x01u; /* corrupt payload */
        CHECK(!grave_load_from_buffer(&b, NULL, bad, sizeof(bad)));
        CHECK(hash_game(&b) == before);

        memcpy(bad, buf, sizeof(bad));
        bad[offsetof(SaveData, unlocked_mask)] |= 0x80u; /* unknown zone bit */
        CHECK(!grave_load_from_buffer(&b, NULL, bad, sizeof(bad)));
        CHECK(hash_game(&b) == before);

        CHECK(!grave_load_from_buffer(&b, NULL, buf, 16)); /* truncated */
        CHECK(!grave_load_from_buffer(NULL, NULL, buf, sizeof(buf)));
        CHECK(!grave_load_from_buffer(&b, NULL, NULL, sizeof(buf)));
        CHECK(hash_game(&b) == before);
    }

    /* a save written at zone start replays the run deterministically */
    grave_game_init(&a, 2024u);
    grave_game_start_zone(&a, ZONE_ASHEN_MILL);
    CHECK(grave_save_to_buffer(&a, NULL, buf, sizeof(buf)) == written);
    grave_game_init(&b, 2024u);
    grave_game_start_zone(&b, ZONE_ASHEN_MILL);
    run_script(&a, 240);
    run_script(&b, 240);
    CHECK(a.rng == b.rng);
    CHECK(a.zombies_killed == b.zombies_killed);
    CHECK(a.prop_count == b.prop_count);
}

/* ================================================================== */
/* camera                                                              */
/* ================================================================== */

static void test_camera(void)
{
    GraveGame g;
    Vec3 view;

    /* third person: 3.2 m behind the pivot, over-shoulder offset, level aim */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    grave_update_camera(&g, 400.0f / 240.0f);
    CHECK_NEAR(g.camera_fov, GRAVE_CAM_FOV_TP, 0.001f, "third person fov");
    CHECK_NEAR(g.camera_pos.y, GRAVE_PLAYER_CAM_PIVOT_Y, 0.001f, "tp height");
    CHECK_NEAR(g.camera_pos.x, GRAVE_CAM_TP_SHOULDER, 0.001f, "tp shoulder");
    CHECK_NEAR(g.camera_pos.z, -GRAVE_CAM_TP_DISTANCE, 0.001f, "tp distance");
    CHECK_NEAR(g.camera_look.y, GRAVE_PLAYER_CAM_PIVOT_Y, 0.001f, "tp look height");
    view.x = g.camera_look.x - g.camera_pos.x;
    view.y = g.camera_look.y - g.camera_pos.y;
    view.z = g.camera_look.z - g.camera_pos.z;
    {
        float len = sqrtf(view.x * view.x + view.y * view.y + view.z * view.z);
        CHECK_NEAR(view.z / len, 1.0f, 0.001f, "tp view dir z");
        CHECK_NEAR(view.x / len, 0.0f, 0.001f, "tp view dir x");
    }
    CHECK(isfinite(g.camera_pos.x) && isfinite(g.camera_look.z));

    /* first person: eye height 1.68, 10 m look-ahead, wider fov */
    g.player.camera_first_person = true;
    grave_update_camera(&g, 400.0f / 240.0f);
    CHECK_NEAR(g.camera_pos.y, GRAVE_PLAYER_EYE_HEIGHT, 0.001f, "fp eye height");
    CHECK_NEAR(g.camera_pos.x, 0.0f, 0.001f, "fp x");
    CHECK_NEAR(g.camera_pos.z, 0.0f, 0.001f, "fp z");
    CHECK_NEAR(g.camera_fov, GRAVE_CAM_FOV_FP, 0.001f, "first person fov");
    CHECK(g.camera_fov != GRAVE_CAM_FOV_TP);
    {
        float len = sqrtf((g.camera_look.x - g.camera_pos.x) *
                              (g.camera_look.x - g.camera_pos.x) +
                          (g.camera_look.y - g.camera_pos.y) *
                              (g.camera_look.y - g.camera_pos.y) +
                          (g.camera_look.z - g.camera_pos.z) *
                              (g.camera_look.z - g.camera_pos.z));
        CHECK_NEAR(len, 10.0f, 0.01f, "fp look distance");
    }

    /* yaw 90: camera ends up on -x, looking +x */
    quiet_arena(&g, 0.0f, 0.0f, GRAVE_PI * 0.5f, 0.0f);
    clear_props(&g);
    grave_update_camera(&g, 400.0f / 240.0f);
    CHECK_NEAR(g.camera_pos.x, -GRAVE_CAM_TP_DISTANCE, 0.01f, "tp yaw camera x");
    CHECK(g.camera_look.x > 5.0f);

    /* looking up keeps the camera above the floor by pulling it in */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.5f);
    clear_props(&g);
    grave_update_camera(&g, 400.0f / 240.0f);
    CHECK(g.camera_pos.y >= GRAVE_CAM_NEAR_FLOOR - 0.001f);
    CHECK(g.camera_pos.y < 1.0f);
    {
        float len = sqrtf((g.camera_look.x - g.camera_pos.x) *
                              (g.camera_look.x - g.camera_pos.x) +
                          (g.camera_look.y - g.camera_pos.y) *
                              (g.camera_look.y - g.camera_pos.y) +
                          (g.camera_look.z - g.camera_pos.z) *
                              (g.camera_look.z - g.camera_pos.z));
        CHECK_NEAR((g.camera_look.y - g.camera_pos.y) / len, sinf(0.5f), 0.001f,
                   "tp view pitch");
    }

    /* a prop directly behind the player pulls the camera in (no clipping) */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    add_test_prop(&g, vec3_make(0.0f, 0.0f, -2.2f), vec3_make(1.2f, 2.0f, 1.2f), PROP_CRATE);
    grave_update_camera(&g, 400.0f / 240.0f);
    CHECK(!point_inside_prop(&g, g.camera_pos, 0.1f));
    CHECK(g.camera_pos.z > -1.0f);

    /* camera stays inside the arena when the player hugs the wall */
    quiet_arena(&g, 0.0f, 19.0f, GRAVE_PI, 0.0f);
    clear_props(&g);
    grave_update_camera(&g, 400.0f / 240.0f);
    CHECK(g.camera_pos.z <= grave_zone_spec(ZONE_ASHEN_MILL)->half_extent - 0.2f);

    /* recoil kicks the camera up, but not the player's base pitch */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    set_weapon(&g, WEAPON_MUSKET);
    {
        float base_side = g.player.transform.pitch;
        float look_y_before;
        grave_update_camera(&g, 400.0f / 240.0f);
        look_y_before = g.camera_look.y;
        fire_once(&g);
        CHECK(g.player.recoil > 0.0f);
        grave_update_camera(&g, 400.0f / 240.0f);
        CHECK(g.camera_look.y > look_y_before);
        CHECK_NEAR(g.player.transform.pitch, base_side, 0.0001f, "recoil is camera-only");
    }
}

/* ================================================================== */
/* phases                                                              */
/* ================================================================== */

static void test_phases(void)
{
    GraveGame g;

    /* pause is app-owned: a PAUSED phase freezes stepping */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    g.phase = GAME_PHASE_PAUSED;
    {
        uint32_t frame = g.frame;
        float t = g.zone_time;
        step_n(&g, 10);
        CHECK(g.phase == GAME_PHASE_PAUSED);
        CHECK(g.frame == frame);
        CHECK(g.zone_time == t);
    }
    g.phase = GAME_PHASE_PLAYING;
    step_idle(&g);
    CHECK(g.frame == 1u);

    /* death: hp <= 0 -> GAME_OVER, world frozen */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    g.player.hp = 5.0f;
    place_zombie(&g, 0, ZOMBIE_BRUTE, vec3_make(0.0f, 0.0f, 0.9f), ZOMBIE_ATTACK);
    g.zombies[0].attack_timer = 0.01f;
    {
        int steps = 0;
        while (g.phase == GAME_PHASE_PLAYING && steps < 30) {
            step_idle(&g);
            steps++;
        }
        CHECK(g.phase == GAME_PHASE_GAME_OVER);
        CHECK(!g.player.alive);
        CHECK(g.player.hp == 0.0f);
    }
    {
        uint32_t frame = g.frame;
        step_n(&g, 10);
        CHECK(g.phase == GAME_PHASE_GAME_OVER);
        CHECK(g.frame == frame);
    }

    /* no events are manufactured on a frozen frame */
    clear_events(&g);
    step_n(&g, 5);
    CHECK(g.events == 0u);
}

/* ================================================================== */
/* pool capacity                                                       */
/* ================================================================== */

static void test_capacity(void)
{
    GraveGame g;

    /* every zombie slot in use: no overflow, counts stay consistent */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        ZombieType type = (i % 3 == 0) ? ZOMBIE_BRUTE
                        : (i % 3 == 1) ? ZOMBIE_RUNNER : ZOMBIE_SHAMBLER;
        float x = -18.0f + (float)(i % 8) * 5.0f;
        float z = -18.0f + (float)(i / 8) * 5.0f;
        place_zombie(&g, i, type, vec3_make(x, 0.0f, z), ZOMBIE_CHASE);
    }
    CHECK(count_zombies(&g) == MAX_ZOMBIES);
    g.player.hp = 100000.0f;
    step_idle(&g);
    CHECK(count_zombies(&g) == MAX_ZOMBIES);
    CHECK(g.zombies_alive == MAX_ZOMBIES);
    g.spawn_budget = 5;
    g.spawn_timer = 0.0f;
    step_idle(&g);
    CHECK(count_zombies(&g) <= MAX_ZOMBIES);
    CHECK(g.spawn_budget == 5); /* no room: budget untouched */

    /* shot trace ring buffer never overflows */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    set_weapon(&g, WEAPON_REVOLVER);
    for (int i = 0; i < MAX_SHOT_TRACES; i++) {
        g.shot_traces[i].life = 10.0f;
        g.shot_traces[i].kind = 0;
        g.shot_traces[i].start = vec3_make(0.0f, 1.0f, 0.0f);
        g.shot_traces[i].end = vec3_make(0.0f, 1.0f, 10.0f);
    }
    fire_once(&g);
    CHECK(count_traces(&g, 0) + count_traces(&g, 1) == MAX_SHOT_TRACES);
    step_n(&g, 8);
    CHECK(count_traces(&g, 0) + count_traces(&g, 1) < MAX_SHOT_TRACES);

    /* pickups: the pool is a hard cap */
    quiet_arena(&g, 0.0f, 0.0f, 0.0f, 0.0f);
    clear_props(&g);
    for (int i = 0; i < MAX_PICKUPS; i++)
        add_test_pickup(&g, DROP_COIN, vec3_make(17.0f, 0.35f, 17.0f), 60.0f);
    CHECK(count_pickups(&g) == MAX_PICKUPS);
    CHECK(add_test_pickup(&g, DROP_SINEW, vec3_make(0.0f, 0.35f, 0.0f), 30.0f) == -1);
}

/* ================================================================== */
/* determinism                                                         */
/* ================================================================== */

static void test_determinism(void)
{
    GraveGame a;
    GraveGame b;
    uint32_t ha;
    uint32_t hb;

    grave_game_init(&a, 1234u);
    grave_game_start_zone(&a, ZONE_ASHEN_MILL);
    grave_game_init(&b, 1234u);
    grave_game_start_zone(&b, ZONE_ASHEN_MILL);
    run_script(&a, 900);
    run_script(&b, 900);
    grave_update_camera(&a, 400.0f / 240.0f);
    grave_update_camera(&b, 400.0f / 240.0f);
    ha = hash_game(&a);
    hb = hash_game(&b);
    CHECK(ha == hb); /* same seed + same inputs == same state */

    grave_game_init(&b, 987654u);
    grave_game_start_zone(&b, ZONE_ASHEN_MILL);
    run_script(&b, 900);
    grave_update_camera(&b, 400.0f / 240.0f);
    hb = hash_game(&b);
    CHECK(ha != hb); /* different seed => different run */

    /* per-step determinism of the RNG itself */
    grave_game_init(&a, 42u);
    grave_game_start_zone(&a, ZONE_ASHEN_MILL);
    grave_game_init(&b, 42u);
    grave_game_start_zone(&b, ZONE_ASHEN_MILL);
    for (int i = 0; i < 300; i++) {
        grave_game_step(&a, NULL, TEST_DT);
        grave_game_step(&b, NULL, TEST_DT);
    }
    CHECK(hash_game(&a) == hash_game(&b));
    CHECK(a.rng == b.rng);
}

/* ================================================================== */
/* 10k random steps: no NaN, nothing out of bounds                     */
/* ================================================================== */

static int stress_invariant_violations(const GraveGame *g)
{
    int bad = 0;
    const Player *p = &g->player;
    const ZoneSpec *zs = grave_zone_spec(g->zone);
    float limit = zs->half_extent - GRAVE_ZONE_BOUND_PAD + 0.75f;

    if (!finite_f32(p->transform.pos.x) || !finite_f32(p->transform.pos.y) ||
        !finite_f32(p->transform.pos.z) || !finite_f32(p->transform.yaw) ||
        !finite_f32(p->transform.pitch) || !finite_f32(p->hp) ||
        !finite_f32(p->stamina) || !finite_f32(p->recoil) ||
        !finite_f32(p->anim_time) || !finite_f32(p->reload_timer) ||
        !finite_f32(p->fire_cooldown))
        bad++;
    if (fabsf(p->transform.pos.x) > limit || fabsf(p->transform.pos.z) > limit)
        bad++;
    if (p->stamina < -0.001f || p->stamina > GRAVE_STAMINA_MAX + 0.001f)
        bad++;
    for (int i = 0; i < WEAPON_COUNT; i++) {
        int mag = grave_weapon_spec((WeaponId)i)->magazine;
        if (p->ammo[i] < 0 || p->ammo[i] > mag)
            bad++;
    }

    if (g->prop_count < 0 || g->prop_count > MAX_PROPS)
        bad++;
    if (g->events & ~(uint32_t)0x1FFu)
        bad++;
    if (g->zombies_alive != count_zombies(g))
        bad++;

    for (int i = 0; i < MAX_ZOMBIES; i++) {
        const Zombie *z = &g->zombies[i];
        if (!z->active)
            continue;
        if (!finite_f32(z->pos.x) || !finite_f32(z->pos.z) ||
            !finite_f32(z->hp) || !finite_f32(z->anim_time) ||
            !finite_f32(z->death_time) || !finite_f32(z->yaw) ||
            !finite_f32(z->attack_timer) || !finite_f32(z->speed))
            bad++;
        if (fabsf(z->pos.x) > limit || fabsf(z->pos.z) > limit)
            bad++;
        if (z->hp < 0.0f || z->hp > z->max_hp + 0.001f)
            bad++;
        if ((int)z->type < 0 || (int)z->type >= ZOMBIE_COUNT)
            bad++;
        if ((int)z->state < ZOMBIE_IDLE || (int)z->state > ZOMBIE_DEAD)
            bad++;
    }

    for (int i = 0; i < MAX_PICKUPS; i++) {
        const Pickup *q = &g->pickups[i];
        if (!q->active)
            continue;
        if (!finite_f32(q->pos.x) || !finite_f32(q->pos.z) ||
            !finite_f32(q->bob_time) || !finite_f32(q->life))
            bad++;
        if (q->life < -0.001f)
            bad++;
    }

    for (int i = 0; i < MAX_SHOT_TRACES; i++) {
        const ShotTrace *t = &g->shot_traces[i];
        if (!finite_f32(t->life) || !finite_f32(t->start.x) ||
            !finite_f32(t->start.y) || !finite_f32(t->start.z) ||
            !finite_f32(t->end.x) || !finite_f32(t->end.y) ||
            !finite_f32(t->end.z))
            bad++;
        if (t->life < -0.001f)
            bad++;
        if (t->kind > 1u)
            bad++;
    }

    {
        float yaw = grave_wrap_pi(p->transform.yaw);
        if (!finite_f32(yaw))
            bad++;
        if (g->quest.kind < QUEST_NONE || g->quest.kind > QUEST_DONE)
            bad++;
        if (g->quest.progress < 0)
            bad++;
        if (g->material_coins < 0 || g->material_sinew < 0 ||
            g->material_bone_charms < 0)
            bad++;
        if (g->spawn_budget < 0)
            bad++;
        for (int i = 0; i < DROP_COUNT; i++) {
            if (g->run_materials[i] < 0)
                bad++;
        }
        if (!finite_f32(g->total_time) || g->total_time < 0.0f)
            bad++;
        if (g->total_kills < 0 || g->zone_runs < 0)
            bad++;
        if (!valid_zone_id(g->world.current))
            bad++;
        for (int i = 0; i < ZONE_COUNT; i++) {
            const ZoneNode *node = &g->world.nodes[i];
            if (!valid_zone_id(node->zone) || node->zone != (ZoneId)i)
                bad++;
            if (node->clears < 0)
                bad++;
            if (!finite_f32(node->best_time) || node->best_time < 0.0f)
                bad++;
        }
        if (!g->world.nodes[ZONE_ASHEN_MILL].unlocked)
            bad++;
        if ((int)g->player.weapon < 0 || (int)g->player.weapon >= WEAPON_COUNT)
            bad++;
    }
    return bad;
}

static void test_stress(void)
{
    GraveGame g;
    uint32_t seed = 0xBADF00Du;
    int bad = 0;

    grave_game_init(&g, 555u);
    grave_game_start_zone(&g, ZONE_ASHEN_MILL);

    for (int step = 0; step < 10000; step++) {
        GraveInput in;
        grave_input_reset(&in);
        in.move_x = test_rng_unit(&seed) * 2.0f - 1.0f;
        in.move_y = test_rng_unit(&seed) * 2.0f - 1.0f;
        in.look_x = (test_rng_unit(&seed) * 2.0f - 1.0f) * 0.15f;
        in.look_y = (test_rng_unit(&seed) * 2.0f - 1.0f) * 0.10f;
        in.fire = test_rng_unit(&seed) < 0.35f;
        in.reload = test_rng_unit(&seed) < 0.02f;
        in.sprint = test_rng_unit(&seed) < 0.30f;
        in.swap_weapon = test_rng_unit(&seed) < 0.01f;
        in.toggle_camera = test_rng_unit(&seed) < 0.01f;

        /* keep the run going and don't let the quest finish early */
        g.player.hp = 100000.0f;
        g.player.alive = true;
        g.zombies_killed = 0;
        g.material_coins = 0;
        g.material_sinew = 0;
        g.material_bone_charms = 0;
        memset(g.run_materials, 0, sizeof(g.run_materials));
        g.quest.kind = QUEST_CLEAR_ZONE;
        g.quest.progress = 0;

        grave_game_step(&g, &in, TEST_DT);

        if (g.phase != GAME_PHASE_PLAYING)
            g.phase = GAME_PHASE_PLAYING;
        g.player.hp = g.player.max_hp;
        g.player.alive = true;

        if ((step % 500) == 0)
            bad += stress_invariant_violations(&g);
    }
    bad += stress_invariant_violations(&g);
    CHECK(bad == 0);
    CHECK(g.frame == 10000u);
    CHECK(finite_f32(g.zone_time));
    CHECK(g.zone_time > 160.0f);

    grave_update_camera(&g, 400.0f / 240.0f);
    CHECK(finite_f32(g.camera_pos.x) && finite_f32(g.camera_pos.y) &&
          finite_f32(g.camera_pos.z));
    CHECK(finite_f32(g.camera_look.x) && finite_f32(g.camera_look.y) &&
          finite_f32(g.camera_look.z));
    CHECK(finite_f32(g.camera_fov) && g.camera_fov > 0.0f);

    /* the same 10k idle steps with a zeroed input are also safe (title test) */
    grave_game_init(&g, 31337u);
    grave_game_start_zone(&g, ZONE_ASHEN_MILL);
    g.player.hp = 100000.0f;
    for (int step = 0; step < 2000; step++) {
        g.player.hp = 100000.0f;
        step_idle(&g);
    }
    CHECK(stress_invariant_violations(&g) == 0);
}

/* ================================================================== */
/* main                                                               */
/* ================================================================== */

int main(void)
{
    printf("Gravepowder core tests\n");

    printf("  input helpers ... ");   test_input_helpers();   printf("done\n");
    printf("  data tables ... ");     test_spec_tables();     printf("done\n");
    printf("  init defaults ... ");   test_init_defaults();   printf("done\n");
    printf("  zone start ... ");      test_start_zone();      printf("done\n");
    printf("  zone restart ... ");    test_zone_restart();    printf("done\n");
    printf("  movement ... ");        test_movement();        printf("done\n");
    printf("  collision ... ");       test_collision();       printf("done\n");
    printf("  look ... ");            test_look();            printf("done\n");
    printf("  weapons ... ");         test_weapons();         printf("done\n");
    printf("  hitscan ... ");         test_hitscan();         printf("done\n");
    printf("  drops/pickups ... ");   test_drops_pickups();   printf("done\n");
    printf("  zombie AI ... ");       test_zombie_ai();       printf("done\n");
    printf("  spawner ... ");         test_spawner();         printf("done\n");
    printf("  quest ... ");           test_quest();           printf("done\n");
    printf("  world progression ... "); test_world_progression(); printf("done\n");
    printf("  bone orchard ... ");    test_bone_orchard();    printf("done\n");
    printf("  save data ... ");       test_save();            printf("done\n");
    printf("  camera ... ");          test_camera();          printf("done\n");
    printf("  phases ... ");          test_phases();          printf("done\n");
    printf("  capacity ... ");        test_capacity();        printf("done\n");
    printf("  determinism ... ");     test_determinism();     printf("done\n");
    printf("  10k step stress ... "); test_stress();          printf("done\n");

    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
