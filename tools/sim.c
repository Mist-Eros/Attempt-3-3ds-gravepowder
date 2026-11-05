/*
 * Gravepowder - headless gameplay sim (v0.4: three-zone campaign, trader, Foreman)
 *
 * Runs the real portable core with a scripted "player bot": walk toward the
 * nearest threat, aim, shoot when roughly on target, reload, collect drops.
 * Plays ASHEN MILL -> BONE ORCHARD -> OLD MINE as one campaign, shopping at the
 * trail between zones (tunes / vitality / reload / medkits / ammo), then beats
 * the Foreman and reports the whole run.  A `--plain` control campaign skips
 * the shop so the upgrade value is visible side by side.
 *
 *   make -C tools CC="D:/tools/zig/zig.exe cc" run
 *   ./sim --plain      (only the no-shopping control)
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "grave.h"
#include "core_internal.h"

#define SIM_DT        (1.0f / 60.0f)
#define SIM_MAX_STEPS (60 * 480) /* give up on a zone after 8 simulated minutes */
#define SIM_SEED      2024u

typedef struct {
    bool  ran;
    bool  done;
    bool  died;
    float time;
    int   kills;
    int   kill_goal;
    int   coins;
    int   sinew;
    int   bone_charms;
    int   shots;
    int   hits;
    int   pickups;
    int   reloads;
    int   waves;
    float hp;
    float max_hp;
    float hp_lost;
    float hp_lost_boss;
    float t_kill_goal;
    float t_quest_done;
    int   max_alive;
    bool  boss_spawned;
    bool  boss_dead;
    float boss_spawn_t;
    float boss_death_t;
    float boss_hp_lost;
} ZoneResult;

typedef struct {
    ZoneResult zone[ZONE_COUNT];
    bool  world_complete;
    int   shop_purchases;
    float total_time;
    int   final_coins;
    int   final_sinew;
    int   final_charms;
    int   final_vitality;
    int   final_tune[WEAPON_COUNT];
    int   final_reload;
    bool  all_zones_done;
} CampaignResult;

typedef struct {
    Vec3 last_pos;
    int  still_frames;
    int  strafe_frames;
    float strafe_dir;
} SimBrain;

/* ================================================================== */
/* small helpers                                                       */
/* ================================================================== */

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

/* Target selection: closest threat.  Runners (the fast ones that catch a kiting
 * player) get priority; the Foreman gets priority once it is on the field
 * because it gates the mine quest - but when the boss fight has adds crowding
 * the player, clear the adds first.  Brutes are the slow wall and go last
 * unless they have piled up. */
static int nearest_zombie(const GraveGame *g, float *dist_out)
{
    int best = -1;
    float best_score = 1.0e9f;
    float best_d = 1.0e9f;
    int brutes = 0;
    int near_count = 0;
    bool boss_focus;

    for (int i = 0; i < MAX_ZOMBIES; i++) {
        const Zombie *z = &g->zombies[i];
        float d;
        if (!z->active || z->state == ZOMBIE_DYING || z->state == ZOMBIE_DEAD)
            continue;
        d = dist_xz(z->pos, g->player.transform.pos);
        if (d < 4.0f)
            near_count++;
        if (z->type == ZOMBIE_BRUTE)
            brutes++;
    }
    boss_focus = !g->boss_alive || near_count < 2;

    for (int i = 0; i < MAX_ZOMBIES; i++) {
        const Zombie *z = &g->zombies[i];
        float d;
        float score;
        if (!z->active || z->state == ZOMBIE_DYING || z->state == ZOMBIE_DEAD)
            continue;
        if (z->type == ZOMBIE_FOREMAN && !boss_focus)
            continue; /* clear the adds before resuming the boss */
        d = dist_xz(z->pos, g->player.transform.pos);
        score = d;
        if (z->type == ZOMBIE_RUNNER)
            score -= 2.5f;
        else if (z->type == ZOMBIE_BRUTE)
            score += (brutes >= 3) ? -1.0f : 4.0f;
        else if (z->type == ZOMBIE_FOREMAN)
            score -= 3.0f;
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

    in->look_x = grave_clampf(-yaw_err / GRAVE_LOOK_YAW_PER_UNIT, -1.0f, 1.0f);
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
        bool panic = (p->hp < 0.45f * p->max_hp);

        target.y = zspec->body_height * 0.55f;
        aim_at(g, target, in, &yaw_err, &pitch_err);

        /* Kite: hold a shooting band, sprint out when they crowd us.  The
         * Foreman is slow and has a big reach, so the bot keeps a much wider
         * berth against him (that is the whole boss fight).  At low hp the bot
         * widens the band and prioritises running, since a hit can kill it. */
        if (z->type == ZOMBIE_FOREMAN) {
            if (zdist < (panic ? 7.0f : 5.4f)) {
                in->move_y = -1.0f;
                in->sprint = true;
            } else if (zdist > 8.5f) {
                in->move_y = 1.0f;
            } else {
                in->move_x = (brain->strafe_dir >= 0.0f) ? 0.30f : -0.30f;
            }
        } else if (panic && zdist < 6.0f) {
            in->move_y = -1.0f;
            in->sprint = true;
        } else if (zdist < 4.6f || zombies_within(g, 3.5f) >= 2) {
            /* pre-emptive retreat: movement now ramps up, so leave a little
             * earlier than the raw melee reach to stay ahead of the pack */
            in->move_y = -1.0f;
            in->sprint = true;
        } else if (zdist > 7.0f) {
            in->move_y = 1.0f;
            in->sprint = zdist > 10.0f;
        } else {
            in->move_x = (brain->strafe_dir >= 0.0f) ? 0.55f : -0.55f;
        }

        /* v0.4: hold aim during the Foreman duel (while not sprinting out of
         * his reach) for the tighter spread and scoped camera.  Normal horde
         * fights stay hip-fire: the aim walk cap would let runners catch the
         * kiting bot. */
        in->aim = (z->type == ZOMBIE_FOREMAN && !in->sprint &&
                   zdist > 5.0f && zdist < range * 0.9f);

        if (fabsf(yaw_err) < 0.06f && fabsf(pitch_err) < 0.12f &&
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

/* ================================================================== */
/* trader bot                                                          */
/* ================================================================== */

static void sim_shop(GraveGame *g, int *purchases)
{
    printf("  [trader] wallet %dc %ds %dch | vit=%d tune=%d/%d/%d reload=%d\n",
           g->material_coins, g->material_sinew, g->material_bone_charms,
           g->player.vitality_level,
           g->player.weapon_tune[WEAPON_FLINTLOCK],
           g->player.weapon_tune[WEAPON_REVOLVER],
           g->player.weapon_tune[WEAPON_MUSKET],
           g->player.reload_tune);

    for (int guard = 0; guard < 40; guard++) {
        ShopItem item = SHOP_ITEM_COUNT;
        const ShopItemSpec *spec;
        int level_before;
        int row;

        if (g->player.hp < 0.65f * g->player.max_hp &&
            grave_shop_can_buy(g, SHOP_MEDKIT))
            item = SHOP_MEDKIT;
        else if (grave_shop_can_buy(g, SHOP_TUNE_REVOLVER))
            item = SHOP_TUNE_REVOLVER;
        else if (grave_shop_can_buy(g, SHOP_VITALITY))
            item = SHOP_VITALITY;
        else if (grave_shop_can_buy(g, SHOP_RELOAD))
            item = SHOP_RELOAD;
        else if (grave_shop_can_buy(g, SHOP_TUNE_FLINTLOCK))
            item = SHOP_TUNE_FLINTLOCK;
        else if (grave_shop_can_buy(g, SHOP_TUNE_MUSKET))
            item = SHOP_TUNE_MUSKET;
        else if (grave_shop_can_buy(g, SHOP_AMMO) && g->material_coins >= 80)
            item = SHOP_AMMO;
        else
            break;

        spec = grave_shop_item(item);
        level_before = grave_shop_level(g, item);
        row = (spec->max_level > 0) ? level_before : 0;
        if (!grave_shop_buy(g, item))
            break;
        (*purchases)++;
        printf("    bought %-16s L%d -> L%d (-%dc -%ds -%dch)\n",
               spec->name, level_before, grave_shop_level(g, item),
               spec->cost_coins[row], spec->cost_sinew[row],
               spec->cost_charms[row]);
    }
    printf("  [trader] after: wallet %dc %ds %dch | hp %.0f/%.0f\n",
           g->material_coins, g->material_sinew, g->material_bone_charms,
           (double)g->player.hp, (double)g->player.max_hp);
}

/* ================================================================== */
/* zone run                                                            */
/* ================================================================== */

static void fill_result(GraveGame *g, ZoneResult *res)
{
    res->time = g->zone_time;
    res->kills = g->zombies_killed;
    res->coins = g->run_materials[DROP_COIN];
    res->sinew = g->run_materials[DROP_SINEW];
    res->bone_charms = g->run_materials[DROP_BONE_CHARM];
    res->hp = g->player.hp;
    res->max_hp = g->player.max_hp;
}

static void run_zone(GraveGame *g, const char *label, ZoneResult *res)
{
    SimBrain brain;
    int kill_goal = g->spawn_kill_goal;
    int prev_wave_left = g->spawn_wave_left;
    float hp_start = g->player.hp;
    float boss_hp_at_spawn = 0.0f;
    bool boss_hp_seen = false;

    memset(&brain, 0, sizeof(brain));
    memset(res, 0, sizeof(*res));
    res->ran = true;
    res->kill_goal = kill_goal;
    res->t_kill_goal = -1.0f;
    res->t_quest_done = -1.0f;
    brain.strafe_dir = 1.0f;

    /* the app owns weapon buttons: the bot prefers the revolver */
    g->player.weapon = WEAPON_REVOLVER;
    g->player.reload_weapon = WEAPON_REVOLVER;

    printf("[%s] zone=%s kill_goal=%d hp=%.0f/%.0f\n", label,
           grave_zone_name(g->zone), kill_goal,
           (double)g->player.hp, (double)g->player.max_hp);

    for (int i = 0; i < SIM_MAX_STEPS; i++) {
        GraveInput in;
        Vec3 before = g->player.transform.pos;
        float moved;
        uint32_t events;
        float hp_before_step = g->player.hp;

        sim_think(g, &in, &brain);
        grave_game_step(g, &in, SIM_DT);
        grave_update_camera(g, 5.0f / 3.0f);

        events = g->events;
        g->events = 0u; /* what app.c does after dispatching to audio/UI */
        if (events & GRAVE_EVT_SHOOT) res->shots++;
        if (events & GRAVE_EVT_HIT) res->hits++;
        if (events & GRAVE_EVT_PICKUP) res->pickups++;
        if (events & GRAVE_EVT_RELOAD) res->reloads++;

        /* wave counting: the core queues a whole wave at once */
        if (g->spawn_wave_left > prev_wave_left)
            res->waves++;
        prev_wave_left = g->spawn_wave_left;

        if (g->player.hp < hp_before_step) {
            res->hp_lost += hp_before_step - g->player.hp;
            if (g->boss_alive && !g->boss_defeated)
                res->hp_lost_boss += hp_before_step - g->player.hp;
        }

        if (res->t_kill_goal < 0.0f && g->zombies_killed >= kill_goal)
            res->t_kill_goal = g->zone_time;

        if (!res->boss_spawned && g->boss_spawned) {
            res->boss_spawned = true;
            res->boss_spawn_t = g->zone_time;
            boss_hp_at_spawn = g->boss_hp;
            boss_hp_seen = true;
        }
        if (boss_hp_seen && !res->boss_dead && g->boss_hp < boss_hp_at_spawn) {
            res->boss_hp_lost += boss_hp_at_spawn - g->boss_hp;
            boss_hp_at_spawn = g->boss_hp;
        }
        if (res->boss_spawned && !res->boss_dead && g->boss_defeated) {
            res->boss_dead = true;
            res->boss_death_t = g->zone_time;
        }

        if (active_zombies(g) > res->max_alive)
            res->max_alive = active_zombies(g);

        /* unstick bookkeeping */
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

        /* every 15 simulated seconds: one balance log line */
        if ((i % 900) == 0) {
            printf("  t=%6.1fs kills=%2d/%-2d alive=%2d hp=%5.1f "
                   "coins=%2d sinew=%2d charms=%2d stage=%d%s\n",
                   (double)g->zone_time, g->zombies_killed, kill_goal,
                   active_zombies(g), (double)g->player.hp,
                   g->run_materials[DROP_COIN], g->run_materials[DROP_SINEW],
                   g->run_materials[DROP_BONE_CHARM], (int)g->quest.kind,
                   g->boss_alive ? " BOSS" : "");
        }

        if (g->phase == GAME_PHASE_QUEST_COMPLETE) {
            res->done = true;
            res->t_quest_done = g->zone_time;
            break;
        }
        if (g->phase == GAME_PHASE_GAME_OVER) {
            res->died = true;
            break;
        }
    }

    fill_result(g, res);
    if (hp_start > g->player.hp)
        res->hp_lost = hp_start - g->player.hp;
}

static void print_zone_result(const char *label, const ZoneResult *r)
{
    if (!r->ran) {
        printf("[%s] NOT REACHED\n", label);
        return;
    }
    printf("[%s] %s in %.1fs  kills=%d/%d waves=%d shots=%d hits=%d "
           "pickups=%d run-drops=%d (c=%d s=%d ch=%d) hp=%.0f/%.0f lost=%.0f\n",
           label, r->done ? "CLEAR" : (r->died ? "DIED" : "TIMEOUT"),
           (double)r->time, r->kills, r->kill_goal, r->waves, r->shots,
           r->hits, r->pickups, r->coins + r->sinew + r->bone_charms,
           r->coins, r->sinew, r->bone_charms, (double)r->hp,
           (double)r->max_hp, (double)r->hp_lost);
    if (r->t_kill_goal >= 0.0f)
        printf("      kill step done at %.1fs", (double)r->t_kill_goal);
    if (r->t_quest_done >= 0.0f)
        printf(", quest complete at %.1fs", (double)r->t_quest_done);
    printf(", peak zombies=%d\n", r->max_alive);
    if (r->boss_spawned) {
        printf("      FOREMAN: spawned %.1fs", (double)r->boss_spawn_t);
        if (r->boss_dead)
            printf(", killed %.1fs (fight %.1fs)",
                   (double)r->boss_death_t,
                   (double)(r->boss_death_t - r->boss_spawn_t));
        else
            printf(", still alive at end");
        printf(", hp lost during fight %.0f\n", (double)r->hp_lost_boss);
    }
}

/* ================================================================== */
/* campaign                                                            */
/* ================================================================== */

static void print_wallet(const GraveGame *g, const char *tag)
{
    printf("  [%s] inventory: %dc %ds %dch | vit=%d reload=%d "
           "tune=%d/%d/%d | hp %.0f/%.0f\n", tag,
           g->material_coins, g->material_sinew, g->material_bone_charms,
           g->player.vitality_level, g->player.reload_tune,
           g->player.weapon_tune[WEAPON_FLINTLOCK],
           g->player.weapon_tune[WEAPON_REVOLVER],
           g->player.weapon_tune[WEAPON_MUSKET],
           (double)g->player.hp, (double)g->player.max_hp);
}

static bool run_campaign(bool use_shop, CampaignResult *out)
{
    static const ZoneId order[ZONE_COUNT] = {
        ZONE_ASHEN_MILL, ZONE_BONE_ORCHARD, ZONE_OLD_MINE
    };
    static const char *const labels[ZONE_COUNT] = {
        "ashen-mill", "bone-orchard", "old-mine"
    };
    GraveGame g;
    bool ok = true;

    memset(out, 0, sizeof(*out));
    grave_game_init(&g, SIM_SEED);
    printf("== campaign (%s) ==\n", use_shop ? "with trader" : "plain, no shop");
    print_wallet(&g, "start");

    for (int z = 0; z < ZONE_COUNT; z++) {
        ZoneId zone = order[z];
        if (z > 0) {
            grave_world_to_trail(&g);
            if (use_shop)
                sim_shop(&g, &out->shop_purchases);
            print_wallet(&g, "trail");
        }
        if (!grave_game_travel_to(&g, zone)) {
            printf("FAIL: could not travel to %s (locked?)\n",
                   grave_zone_name(zone));
            ok = false;
            break;
        }
        printf("\n");
        run_zone(&g, labels[z], &out->zone[z]);
        print_zone_result(labels[z], &out->zone[z]);
        if (!out->zone[z].done) {
            ok = false;
            break;
        }
    }

    out->all_zones_done = ok;
    out->world_complete = g.world_complete;
    out->total_time = g.total_time;
    out->final_coins = g.material_coins;
    out->final_sinew = g.material_sinew;
    out->final_charms = g.material_bone_charms;
    out->final_vitality = g.player.vitality_level;
    out->final_reload = g.player.reload_tune;
    for (int i = 0; i < WEAPON_COUNT; i++)
        out->final_tune[i] = g.player.weapon_tune[i];

    printf("\n  world: cleared=[%d %d %d] unlocked=[%d %d %d] complete=%d\n",
           (int)g.world.nodes[ZONE_ASHEN_MILL].cleared,
           (int)g.world.nodes[ZONE_BONE_ORCHARD].cleared,
           (int)g.world.nodes[ZONE_OLD_MINE].cleared,
           (int)g.world.nodes[ZONE_ASHEN_MILL].unlocked,
           (int)g.world.nodes[ZONE_BONE_ORCHARD].unlocked,
           (int)g.world.nodes[ZONE_OLD_MINE].unlocked,
           (int)g.world_complete);
    print_wallet(&g, "end");
    printf("  lifetime: kills=%d shots=%d hits=%d (%.0f%% acc) "
           "playtime=%.1fs zone-runs=%d purchases=%d\n\n",
           g.total_kills, g.total_shots, g.total_hits,
           g.total_shots > 0 ? 100.0 * (double)g.total_hits / (double)g.total_shots : 0.0,
           (double)g.total_time, g.zone_runs, out->shop_purchases);
    return ok;
}

static void print_campaign_summary(const char *tag, const CampaignResult *c)
{
    printf("[%s] %s total=%.1fs zones=%d/%d world_complete=%d "
           "purchases=%d end-wallet=%dc/%ds/%dch vit=%d reload=%d "
           "tune=%d/%d/%d\n",
           tag, c->all_zones_done ? "CAMPAIGN COMPLETE" : "CAMPAIGN FAILED",
           (double)c->total_time,
           (int)((c->zone[0].done ? 1 : 0) + (c->zone[1].done ? 1 : 0) +
                 (c->zone[2].done ? 1 : 0)),
           ZONE_COUNT, (int)c->world_complete, c->shop_purchases,
           c->final_coins, c->final_sinew, c->final_charms,
           c->final_vitality, c->final_reload,
           c->final_tune[WEAPON_FLINTLOCK], c->final_tune[WEAPON_REVOLVER],
           c->final_tune[WEAPON_MUSKET]);
}

int main(int argc, char **argv)
{
    CampaignResult shopped;
    CampaignResult plain;
    bool plain_only = (argc > 1 && strcmp(argv[1], "--plain") == 0);
    bool shop_ok;
    bool plain_ok = false;

    printf("== Gravepowder headless sim (v0.4: steady hands) ==\n");
    printf("fixed 60 Hz steps, no rendering; seed=%u\n\n", (unsigned)SIM_SEED);

    shop_ok = run_campaign(true, &shopped);
    print_campaign_summary("shop", &shopped);

    if (!plain_only) {
        printf("\n");
        plain_ok = run_campaign(false, &plain);
        print_campaign_summary("plain", &plain);
    }

    printf("\n== gates ==\n");
    if (!shop_ok) {
        printf("FAIL: the shopping bot did not clear the valley\n");
    }
    if (!plain_only && !plain_ok) {
        printf("FAIL: the plain (no-shop) bot did not clear the valley\n");
    }
    if (!shopped.world_complete)
        printf("FAIL: world_complete was never set\n");
    if (shopped.zone[ZONE_OLD_MINE].boss_spawned &&
        !shopped.zone[ZONE_OLD_MINE].boss_dead)
        printf("FAIL: the Foreman survived the whole campaign\n");
    if (shopped.zone[2].done && shopped.zone[2].boss_dead)
        printf("foreman: spawned %.1fs, killed %.1fs (fight %.1fs), "
               "hp lost in fight %.0f\n",
               (double)shopped.zone[2].boss_spawn_t,
               (double)shopped.zone[2].boss_death_t,
               (double)(shopped.zone[2].boss_death_t -
                        shopped.zone[2].boss_spawn_t),
               (double)shopped.zone[2].hp_lost_boss);

    {
        bool ok = shop_ok && shopped.world_complete;
        if (!plain_only)
            ok = ok && plain_ok;
        printf("== %s ==\n", ok ? "SIM OK" : "SIM FAILED");
        return ok ? 0 : 1;
    }
}
