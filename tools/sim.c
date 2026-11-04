/*
 * Gravepowder - headless gameplay sim (v0.2: valley campaign + balance)
 *
 * Runs the real portable core with a scripted "player bot": walk toward the
 * nearest zombie, aim, shoot when roughly on target, reload, collect drops.
 * Plays ASHEN MILL -> trail -> BONE ORCHARD as one campaign (exercising the
 * zone-unlock progression and the persistent inventory), then an idle run to
 * show how fast a player who does nothing dies.  No graphics, no audio, no
 * 3DS headers - this is how the balance gets tuned without the hardware.
 *
 *   make -C tools CC="D:/tools/zig/zig.exe cc" run
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "grave.h"
#include "core_internal.h"

#define SIM_DT        (1.0f / 60.0f)
#define SIM_MAX_STEPS (60 * 300) /* give up on a zone after 5 simulated minutes */
#define SIM_SEED      2024u

typedef struct {
    int steps;
    float time;
    int kills;
    int kill_goal;
    int coins;
    int sinew;
    int bone_charms;
    int shots;
    int hits;
    int pickups;
    int reloads;
    float hp;
    float t_kill_goal;
    float t_quest_done;
    int max_alive;
    bool quest_done;
    bool died;
} SimResult;

typedef struct {
    Vec3 last_pos;
    int  still_frames;
    int  strafe_frames;
    float strafe_dir;
} SimBrain;

static int active_zombies(const GraveGame *g)
{
    int n = 0;
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        if (g->zombies[i].active)
            n++;
    }
    return n;
}

static float dist_xz(Vec3 a, Vec3 b)
{
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return sqrtf(dx * dx + dz * dz);
}

/* Target selection: closest threat, but runners (the fast ones that actually
 * catch a kiting player) get priority.  Brutes are the slow wall, so they are
 * picked last - unless enough of them have piled up that clearing the wall is
 * the only way to make room for the rest of the wave. */
static int nearest_zombie(const GraveGame *g, float *dist_out)
{
    int best = -1;
    float best_score = 1.0e9f;
    float best_d = 1.0e9f;
    int brutes = 0;

    for (int i = 0; i < MAX_ZOMBIES; i++) {
        const Zombie *z = &g->zombies[i];
        if (z->active && z->state != ZOMBIE_DYING && z->state != ZOMBIE_DEAD &&
            z->type == ZOMBIE_BRUTE)
            brutes++;
    }
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        const Zombie *z = &g->zombies[i];
        float d;
        float score;
        if (!z->active || z->state == ZOMBIE_DYING || z->state == ZOMBIE_DEAD)
            continue;
        d = dist_xz(z->pos, g->player.transform.pos);
        score = d;
        if (z->type == ZOMBIE_RUNNER)
            score -= 2.5f;
        else if (z->type == ZOMBIE_BRUTE)
            score += (brutes >= 3) ? -1.0f : 4.0f;
        else
            score += 1.5f;
        if (score < best_score) {
            best_score = score;
            best_d = d;
            best = i;
        }
    }
    *dist_out = best_d;
    return best;
}

static int nearest_pickup(const GraveGame *g, float *dist_out)
{
    int best = -1;
    float best_d = 1.0e9f;
    for (int i = 0; i < MAX_PICKUPS; i++) {
        const Pickup *q = &g->pickups[i];
        float d;
        if (!q->active)
            continue;
        d = dist_xz(q->pos, g->player.transform.pos);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    *dist_out = best_d;
    return best;
}

static int zombies_within(const GraveGame *g, float radius)
{
    int n = 0;
    for (int i = 0; i < MAX_ZOMBIES; i++) {
        const Zombie *z = &g->zombies[i];
        if (!z->active || z->state == ZOMBIE_DYING || z->state == ZOMBIE_DEAD)
            continue;
        if (dist_xz(z->pos, g->player.transform.pos) < radius)
            n++;
    }
    return n;
}

/* Turn the touch-look input toward a world target; reports how far off the
 * aim still is so the bot knows when to pull the trigger. */
static void aim_at(const GraveGame *g, Vec3 target, GraveInput *in,
                   float *yaw_err_out, float *pitch_err_out)
{
    const Player *p = &g->player;
    float dx = target.x - p->transform.pos.x;
    float dz = target.z - p->transform.pos.z;
    float horiz = sqrtf(dx * dx + dz * dz);
    float desired_yaw = atan2f(dx, dz);
    float yaw_err = grave_wrap_pi(desired_yaw - p->transform.yaw);
    float desired_pitch;
    float pitch_err;

    if (horiz < 0.001f)
        horiz = 0.001f;
    desired_pitch = atan2f(target.y - GRAVE_PLAYER_HIT_HEIGHT, horiz);
    desired_pitch = grave_clampf(desired_pitch, -GRAVE_PITCH_LIMIT, GRAVE_PITCH_LIMIT);
    pitch_err = desired_pitch - p->transform.pitch;

    in->look_x = grave_clampf(yaw_err / GRAVE_LOOK_YAW_PER_UNIT, -1.0f, 1.0f);
    in->look_y = grave_clampf(-pitch_err / GRAVE_LOOK_PITCH_PER_UNIT, -1.0f, 1.0f);

    *yaw_err_out = yaw_err;
    *pitch_err_out = pitch_err;
}

/* Small sidestep when the bot has been pushing against a prop/wall. */
static void apply_unstick(SimBrain *brain, GraveInput *in)
{
    if (brain->strafe_frames > 0) {
        in->move_x = brain->strafe_dir * 0.9f;
        in->move_y = 0.25f;
    }
}

static void sim_think(const GraveGame *g, GraveInput *in, SimBrain *brain)
{
    const Player *p = &g->player;
    int zi;
    int pi;
    float zdist = 1.0e9f;
    float pdist = 1.0e9f;
    bool collecting;
    float yaw_err = 0.0f;
    float pitch_err = 0.0f;

    grave_input_reset(in);
    zi = nearest_zombie(g, &zdist);
    pi = nearest_pickup(g, &pdist);

    /* after the kill step, the job is to go pick the materials up */
    collecting = (g->quest.kind == QUEST_COLLECT) && (zi < 0 || zdist > 7.0f);
    if (!collecting && pi >= 0 && pdist < 3.0f)
        collecting = true;

    if (collecting && pi >= 0) {
        Vec3 target = g->pickups[pi].pos;
        target.y = 0.3f;
        aim_at(g, target, in, &yaw_err, &pitch_err);
        if (pdist > 0.8f) {
            in->move_y = 1.0f;
            in->sprint = pdist > 9.0f;
        }
        apply_unstick(brain, in);
        return;
    }

    if (zi >= 0) {
        const Zombie *z = &g->zombies[zi];
        const ZombieSpec *zspec = grave_zombie_spec(z->type);
        float range = grave_weapon_spec(p->weapon)->range;
        Vec3 target = z->pos;

        target.y = zspec->body_height * 0.55f;
        aim_at(g, target, in, &yaw_err, &pitch_err);

        /* kite: hold the 3.4..6.5 m band, sprint out when they crowd us */
        if (zdist > 6.5f) {
            in->move_y = 1.0f;
            in->sprint = zdist > 10.0f;
        } else if (zdist < 3.4f || zombies_within(g, 3.0f) >= 2) {
            in->move_y = -1.0f;
            in->sprint = true;
        } else {
            in->move_x = (brain->strafe_dir >= 0.0f) ? 0.55f : -0.55f;
        }

        if (fabsf(yaw_err) < 0.12f && fabsf(pitch_err) < 0.18f &&
            zdist < range * 0.85f && zdist > 0.9f &&
            p->ammo[p->weapon] > 0 && p->reload_timer <= 0.0f &&
            p->fire_cooldown <= 0.0f)
            in->fire = true;

        apply_unstick(brain, in);
        return;
    }

    /* nothing left to shoot: reload and grab any loot left on the ground */
    if (p->ammo[p->weapon] < grave_weapon_spec(p->weapon)->magazine &&
        p->reload_timer <= 0.0f)
        in->reload = true;
    if (pi >= 0) {
        Vec3 target = g->pickups[pi].pos;
        target.y = 0.3f;
        aim_at(g, target, in, &yaw_err, &pitch_err);
        if (pdist > 0.8f)
            in->move_y = 1.0f;
    }
    apply_unstick(brain, in);
}

/* Plays the currently-started zone.  The app layer owns weapon/menu buttons, so
 * the bot "presses" them by writing the authoritative player fields, just like
 * app.c does. */
static SimResult run_zone(GraveGame *g, const char *label, bool idle)
{
    SimBrain brain;
    SimResult res;
    int kill_goal = g->spawn_kill_goal;

    memset(&res, 0, sizeof(res));
    memset(&brain, 0, sizeof(brain));
    brain.strafe_dir = 1.0f;
    res.kill_goal = kill_goal;
    res.t_kill_goal = -1.0f;
    res.t_quest_done = -1.0f;

    g->player.weapon = WEAPON_REVOLVER;
    g->player.reload_weapon = WEAPON_REVOLVER;

    printf("[%s] zone=%s goal=%d kills\n", label,
           grave_zone_name(g->zone), kill_goal);

    for (int i = 0; i < SIM_MAX_STEPS; i++) {
        GraveInput in;
        Vec3 before = g->player.transform.pos;
        float moved;
        uint32_t events;

        if (idle)
            grave_input_reset(&in);
        else
            sim_think(g, &in, &brain);

        grave_game_step(g, &in, SIM_DT);
        grave_update_camera(g, 5.0f / 3.0f);

        events = g->events;
        g->events = 0u; /* what app.c does after dispatching to audio/UI */
        if (events & GRAVE_EVT_SHOOT) res.shots++;
        if (events & GRAVE_EVT_HIT) res.hits++;
        if (events & GRAVE_EVT_PICKUP) res.pickups++;
        if (events & GRAVE_EVT_RELOAD) res.reloads++;

        if (res.t_kill_goal < 0.0f && g->zombies_killed >= kill_goal)
            res.t_kill_goal = g->zone_time;

        if (active_zombies(g) > res.max_alive)
            res.max_alive = active_zombies(g);

        /* unstick bookkeeping (bot only) */
        if (!idle && i > 0) {
            moved = dist_xz(before, g->player.transform.pos);
            if (brain.strafe_frames > 0) {
                brain.strafe_frames--;
            } else if ((in.move_x != 0.0f || in.move_y != 0.0f) && moved < 0.015f) {
                brain.still_frames++;
            } else {
                brain.still_frames = 0;
            }
            if (brain.strafe_frames == 0 && brain.still_frames > 25) {
                brain.strafe_frames = 40;
                brain.strafe_dir = -brain.strafe_dir;
                brain.still_frames = 0;
            }
        }

        /* every 10 simulated seconds: one balance log line */
        if (!idle && (i % 600) == 0) {
            printf("  t=%5.1fs kills=%2d/%-2d alive=%2d hp=%5.1f "
                   "coins=%2d sinew=%2d charms=%2d stage=%d\n",
                   (double)g->zone_time, g->zombies_killed, kill_goal,
                   active_zombies(g), (double)g->player.hp,
                   g->run_materials[DROP_COIN], g->run_materials[DROP_SINEW],
                   g->run_materials[DROP_BONE_CHARM], (int)g->quest.kind);
        }

        if (g->phase == GAME_PHASE_QUEST_COMPLETE) {
            res.quest_done = true;
            res.t_quest_done = g->zone_time;
            break;
        }
        if (g->phase == GAME_PHASE_GAME_OVER) {
            res.died = true;
            break;
        }
    }

    res.steps = (int)(g->zone_time * 60.0f + 0.5f);
    res.time = g->zone_time;
    res.kills = g->zombies_killed;
    res.coins = g->run_materials[DROP_COIN];
    res.sinew = g->run_materials[DROP_SINEW];
    res.bone_charms = g->run_materials[DROP_BONE_CHARM];
    res.hp = g->player.hp;
    return res;
}

static void print_result(const char *label, const SimResult *r, bool idle)
{
    if (idle) {
        printf("[%s] %s after %.1fs  hp=%.0f kills=%d\n",
               label, r->died ? "DIED" : "SURVIVED",
               (double)r->time, (double)r->hp, r->kills);
        return;
    }
    printf("[%s] %s in %.1fs  kills=%d/%d shots=%d hits=%d reloads=%d "
           "run drops=%d (coins=%d sinew=%d charms=%d) pickups=%d hp=%.0f/%.0f\n",
           label, r->quest_done ? "QUEST COMPLETE" : (r->died ? "DIED" : "TIMEOUT"),
           (double)r->time, r->kills, r->kill_goal, r->shots, r->hits,
           r->reloads, r->coins + r->sinew + r->bone_charms,
           r->coins, r->sinew, r->bone_charms, r->pickups,
           (double)r->hp, (double)GRAVE_PLAYER_MAX_HP);
    if (r->t_kill_goal >= 0.0f)
        printf("      kill step done at %.1fs", (double)r->t_kill_goal);
    if (r->t_quest_done >= 0.0f)
        printf(", quest complete at %.1fs", (double)r->t_quest_done);
    printf(", peak zombies on field=%d\n", r->max_alive);
}

static void print_world(const GraveGame *g)
{
    printf("      world: in_zone=%d current=%s cleared=[%d %d %d] "
           "unlocked=[%d %d %d]\n",
           (int)g->world.in_zone, grave_zone_name(g->world.current),
           (int)g->world.nodes[ZONE_ASHEN_MILL].cleared,
           (int)g->world.nodes[ZONE_BONE_ORCHARD].cleared,
           (int)g->world.nodes[ZONE_OLD_MINE].cleared,
           (int)g->world.nodes[ZONE_ASHEN_MILL].unlocked,
           (int)g->world.nodes[ZONE_BONE_ORCHARD].unlocked,
           (int)g->world.nodes[ZONE_OLD_MINE].unlocked);
    printf("      inventory: coins=%d sinew=%d bone-charms=%d kills-total=%d "
           "zones-run=%d playtime=%.1fs\n",
           g->material_coins, g->material_sinew, g->material_bone_charms,
           g->total_kills, g->zone_runs, (double)g->total_time);
}

int main(void)
{
    GraveGame g;
    SimResult ashen;
    SimResult orchard;
    SimResult idle;
    int failures = 0;

    printf("== Gravepowder headless gameplay sim (v0.2 valley campaign) ==\n");
    printf("fixed 60 Hz steps, no rendering; bot plays ASHEN MILL -> BONE ORCHARD\n\n");

    /* ---- campaign: the mill unlocks the orchard through the quest --------- */
    grave_game_init(&g, SIM_SEED);
    if (!grave_game_travel_to(&g, ZONE_ASHEN_MILL)) {
        printf("FAIL: could not enter ASHEN MILL\n");
        return 1;
    }
    ashen = run_zone(&g, "campaign/ashen-mill", false);
    print_result("campaign/ashen-mill", &ashen, false);
    print_world(&g);
    printf("\n");

    grave_world_to_trail(&g);
    if (!grave_game_travel_to(&g, ZONE_BONE_ORCHARD)) {
        printf("FAIL: BONE ORCHARD was not unlocked by clearing ASHEN MILL\n");
        failures++;
    } else {
        orchard = run_zone(&g, "campaign/bone-orchard", false);
        print_result("campaign/bone-orchard", &orchard, false);
        print_world(&g);
        printf("\n");
        if (!orchard.quest_done)
            failures++;
    }

    /* ---- idle player: balance sanity (should die) ------------------------ */
    grave_game_init(&g, SIM_SEED);
    if (grave_game_travel_to(&g, ZONE_ASHEN_MILL))
        idle = run_zone(&g, "idle/ashen-mill", true);
    else
        memset(&idle, 0, sizeof(idle));
    print_result("idle/ashen-mill", &idle, true);
    printf("\n");

    /* ---- gates ----------------------------------------------------------- */
    if (!ashen.quest_done) {
        printf("FAIL: bot did not complete ASHEN MILL\n");
        failures++;
    }
    if (!idle.died)
        printf("WARN: a player who does nothing never dies - too easy\n");
    if (ashen.quest_done &&
        (ashen.t_quest_done < 60.0f || ashen.t_quest_done > 300.0f))
        printf("WARN: ASHEN MILL clear time %.1fs outside the 1-5 min range\n",
               (double)ashen.t_quest_done);
    if (orchard.quest_done &&
        (orchard.t_quest_done < 90.0f || orchard.t_quest_done > 300.0f))
        printf("WARN: BONE ORCHARD clear time %.1fs outside the 1.5-5 min range\n",
               (double)orchard.t_quest_done);

    printf("== %s ==\n", failures == 0 ? "SIM OK" : "SIM FAILED");
    return failures == 0 ? 0 : 1;
}
