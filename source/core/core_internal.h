/*
 * Gravepowder - internal to source/core/ (NOT part of the public contract)
 *
 * Tuning constants and the zombie/zone data tables shared by the portable
 * simulation (game.c), the host unit tests (tests/) and the headless balance
 * sim (tools/sim.c).  Nothing in here is visible to the 3DS front end: the
 * renderer and UI only ever see include/grave.h.
 *
 * Any gameplay number lives in data.c; this header just declares the shapes.
 */
#ifndef GRAVEPOWDER_CORE_INTERNAL_H
#define GRAVEPOWDER_CORE_INTERNAL_H

#include <stdint.h>

#include "grave.h"

#define GRAVE_PI      3.14159265358979323846f
#define GRAVE_TWO_PI  6.28318530717958647692f
#define GRAVE_DEG2RAD 0.01745329251994329577f

/* ---- player ------------------------------------------------------- */

#define GRAVE_PLAYER_MAX_HP         100.0f
#define GRAVE_PLAYER_RADIUS         0.35f
#define GRAVE_PLAYER_WALK_SPEED     4.2f
#define GRAVE_PLAYER_SPRINT_SPEED   6.6f
#define GRAVE_PLAYER_EYE_HEIGHT     1.68f  /* first-person camera          */
#define GRAVE_PLAYER_HIT_HEIGHT     1.30f  /* hitscan origin (chest)       */
#define GRAVE_PLAYER_CAM_PIVOT_Y    1.55f  /* third-person orbit pivot height       */

#define GRAVE_STAMINA_MAX           100.0f
#define GRAVE_STAMINA_DRAIN         25.0f  /* per sprinting second                  */
#define GRAVE_STAMINA_REGEN         16.0f  /* per resting second                    */
#define GRAVE_STAMINA_SPRINT_MIN    5.0f   /* can't start a sprint below this       */

/* Touch drag units: GraveInput.look_x/y are -1..1 deltas for one step.
 * 160 px of drag == 1.0 unit (see grave_input_touch). */
#define GRAVE_LOOK_YAW_PER_UNIT     2.2f   /* radians per full drag unit            */
#define GRAVE_LOOK_PITCH_PER_UNIT   1.6f
#define GRAVE_PITCH_LIMIT           1.30f  /* ~74 degrees up/down                   */

#define GRAVE_RECOIL_AIM_SHARE      0.5f   /* % of the camera kick that moves the shot */
#define GRAVE_RECOIL_RECOVERY       9.0f   /* exponential decay rate per second     */
#define GRAVE_ANIM_WALK_RATE        7.0f   /* walk-cycle radians per second at walk */

/* ---- weapons ------------------------------------------------------ */

#define GRAVE_MUZZLE_OFFSET         0.30f  /* shot origin forward of the player     */
#define GRAVE_TRACER_LIFE           0.06f
#define GRAVE_IMPACT_LIFE           0.22f
#define GRAVE_HEADSHOT_MULTIPLIER   1.5f
#define GRAVE_EMPTY_CLICK_COOLDOWN  0.28f

/* ---- pickups ------------------------------------------------------ */

#define GRAVE_PICKUP_RADIUS         1.55f  /* auto-pickup distance, metres          */
#define GRAVE_PICKUP_LIFE           60.0f  /* seconds on the ground before fading   */
#define GRAVE_PICKUP_LIFT           0.35f  /* base y for the renderer's bob         */
#define GRAVE_DROP_SCATTER_MIN      0.30f
#define GRAVE_DROP_SCATTER_MAX      0.65f
#define GRAVE_AMMO_PICKUP_ROUNDS    2

/* ---- zombies ------------------------------------------------------ */

#define GRAVE_IDLE_TIME_MIN         0.35f  /* spawn-to-chase notice delay            */
#define GRAVE_IDLE_TIME_MAX         1.15f
#define GRAVE_STAGGER_TIME          0.30f  /* hit reaction lock-up                  */
#define GRAVE_DYING_TIME            1.15f  /* fall-over + fade, then pooled          */
#define GRAVE_ZOMBIE_SEPARATION     0.90f  /* fraction of overlap resolved per pair  */
#define GRAVE_SPAWN_MIN_PLAYER_DIST 7.0f   /* don't materialise on the player        */
#define GRAVE_SPAWN_MIN_ZOMBIE_DIST 1.10f
#define GRAVE_WAVE_RETRY            0.60f  /* wait when the alive cap is reached     */

/* ---- camera ------------------------------------------------------- */

#define GRAVE_CAM_TP_DISTANCE       3.2f   /* metres behind the pivot                */
#define GRAVE_CAM_TP_SHOULDER       0.45f  /* over-the-shoulder right offset         */
#define GRAVE_CAM_MIN_DISTANCE      0.60f  /* pull-in floor when hugging a wall      */
#define GRAVE_CAM_FOV_TP            62.0f  /* vertical field of view, degrees        */
#define GRAVE_CAM_FOV_FP            68.0f
#define GRAVE_CAM_NEAR_FLOOR        0.35f

/* ---- arena -------------------------------------------------------- */

#define GRAVE_ZONE_BOUND_PAD        0.8f   /* player keeps this far off the fence     */

/* ---- data tables (implemented in data.c) -------------------------- */

typedef struct {
    float hp;              /* max hit points                                */
    float speed;           /* chase speed, m/s                              */
    float damage;          /* damage per landed swipe                       */
    float attack_interval; /* seconds between swipes (after one lands)      */
    float windup;          /* seconds of telegraph before the swipe lands   */
    float reach;           /* extra attack reach beyond the two radii       */
    float radius;          /* collision/hitbox radius, metres               */
    float body_height;     /* full height for hit boxes + renderer          */
    float anim_rate;       /* walk-cycle rate multiplier for the renderer   */
    float stagger_chance;  /* 0..1 chance a non-lethal hit staggers         */
} ZombieSpec;

const ZombieSpec *grave_zombie_spec(ZombieType type);

typedef struct {
    float half_extent;     /* arena is 2*half_extent square, metres         */
    int   spawn_budget;    /* total zombies that may spawn in the zone      */
    int   kill_goal;       /* quest kill step target                        */
    int   max_alive;       /* concurrent zombie cap (sim-side, < MAX_ZOMBIES) */
    float wave_interval;   /* seconds between spawn waves                   */

    /* Generic quest staging (v0.2): kill goal -> collect material A ->
     * collect material B -> done.  Ashen Mill = coins + sinew, Bone Orchard =
     * coins + bone charms. */
    GraveDrop material_a;
    int       goal_a;
    GraveDrop material_b;
    int       goal_b;

    /* Spawner flavour: brutes forced at the head of each wave and how the
     * rest of the wave rolls. */
    int   brute_per_wave;  /* guaranteed brutes per wave (0 = random only)  */
    float runner_share;    /* mid/late-wave runner ratio, 0..1              */
    float brute_chance;    /* random extra brute chance, 0..1               */

    /* Quest payout (v0.3): "clear the job, get paid" - added to the material
     * inventory when the zone quest completes, so the trader has a sink. */
    int   reward_coins;
    int   reward_sinew;
    int   reward_charms;

    /* Which hand-authored prop layout to build (ZONE_LAYOUT_*). */
    int   layout;
} ZoneSpec;

/* zone layout ids, internal (the renderer just draws props) */
enum {
    ZONE_LAYOUT_ASHEN_MILL = 0,
    ZONE_LAYOUT_BONE_ORCHARD,
    ZONE_LAYOUT_OLD_MINE
};

const ZoneSpec *grave_zone_spec(ZoneId zone);

/* ---- v0.3: trader upgrades + OLD MINE boss ------------------------- */

#define GRAVE_TUNE_MAX_LEVEL      3
#define GRAVE_TUNE_DAMAGE_STEP    0.15f  /* +15% weapon damage per level    */
#define GRAVE_VITALITY_MAX_LEVEL  5
#define GRAVE_VITALITY_HP_STEP    10.0f  /* +10 max hp per level            */
#define GRAVE_RELOAD_MAX_LEVEL    3
#define GRAVE_RELOAD_TIME_STEP    0.08f  /* -8% reload time per level        */
#define GRAVE_RELOAD_MIN_TIME     0.20f

/* The Foreman spawns once per OLD MINE run once this many normal zombies have
 * been killed; its death gates the zone's final quest completion. */
#define GRAVE_BOSS_SPAWN_KILLS    40
#define GRAVE_BOSS_COIN_DROPS     5
#define GRAVE_BOSS_CHARM_DROPS    2

/* ---- shared math helpers (inline, no link dependency) -------------- */

static inline float grave_clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline float grave_wrap_pi(float a)
{
    a = a + GRAVE_PI;
    a = a - GRAVE_TWO_PI * (float)(int)(a / GRAVE_TWO_PI);
    if (a < 0.0f)
        a += GRAVE_TWO_PI;
    return a - GRAVE_PI;
}

#endif /* GRAVEPOWDER_CORE_INTERNAL_H */
