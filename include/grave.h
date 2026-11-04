/*
 * Gravepowder - portable game contract
 *
 * Everything under source/core/ is plain C99 with no <3ds.h> in sight, so the
 * gameplay can be unit tested on a PC and tuned with a headless sim.  The 3DS
 * front end (source/render3d.c, source/ui.c, source/app.c) only reads the state
 * declared here and draws it.
 *
 * This header is the contract for the first playable milestone (v0.1):
 *   - one anthro hero, third-person camera by default, first-person toggle
 *   - flintlock-era weapons: flintlock pistol, revolver, musket
 *   - shambling zombies that drop old coins / sinew for the zone quest
 *   - one walled zone with props, a clear-the-yard quest, materials counter
 *
 * The world is fixed-size: no malloc anywhere in the game loop.
 */
#ifndef GRAVEPOWDER_H
#define GRAVEPOWDER_H

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* basic types                                                        */
/* ------------------------------------------------------------------ */

typedef struct { float x, y, z; } Vec3;

typedef struct {
    Vec3  pos;      /* feet position, y = 0 is the ground                */
    float yaw;      /* radians, 0 = +z, positive turns right             */
    float pitch;    /* radians, positive looks up                        */
} ActorTransform;

static inline Vec3 vec3_make(float x, float y, float z)
{
    Vec3 v = { x, y, z };
    return v;
}

static inline float vec3_len(Vec3 v)
{
    return __builtin_sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

/* ------------------------------------------------------------------ */
/* input (filled in by the 3DS front end / host tools)                */
/* ------------------------------------------------------------------ */

typedef struct {
    float move_x;    /* -1 .. 1 strafe (circle pad / wasd)               */
    float move_y;    /* -1 .. 1 forward  (circle pad / wasd)             */
    float look_x;    /* touch drag delta this frame, -1 .. 1             */
    float look_y;
    bool  fire;      /* held                                             */
    bool  sprint;
    bool  reload;
    bool  swap_weapon;   /* tap: next weapon                          */
    bool  toggle_camera; /* tap: first <-> third person               */
    bool  pause;         /* tap: start / touch pause button           */
    bool  interact;      /* tap: use / pick up                        */
} GraveInput;

/* ------------------------------------------------------------------ */
/* weapons                                                            */
/* ------------------------------------------------------------------ */

typedef enum {
    WEAPON_FLINTLOCK = 0,   /* one shot, big damage, slow reload       */
    WEAPON_REVOLVER,        /* six shots, medium damage, quick-ish     */
    WEAPON_MUSKET,          /* one shot, huge damage, very slow        */
    WEAPON_COUNT
} WeaponId;

typedef struct {
    const char *name;
    int   damage;
    int   pellets;        /* musket = 1 for now                       */
    float range;          /* metres                                   */
    float spread;         /* radians                                  */
    int   magazine;       /* rounds before reload                     */
    float fire_interval;  /* seconds between shots                    */
    float reload_time;    /* seconds to reload                        */
    float recoil;         /* degrees of kick                          */
} WeaponSpec;

const WeaponSpec *grave_weapon_spec(WeaponId id);

/* ------------------------------------------------------------------ */
/* zombies                                                            */
/* ------------------------------------------------------------------ */

typedef enum {
    ZOMBIE_SHAMBLER = 0,  /* slow, weak, common                        */
    ZOMBIE_RUNNER,        /* faster, fragile                          */
    ZOMBIE_BRUTE,         /* slow, tanky, hits hard                   */
    ZOMBIE_COUNT
} ZombieType;

typedef enum {
    ZOMBIE_IDLE = 0,
    ZOMBIE_CHASE,
    ZOMBIE_ATTACK,
    ZOMBIE_STAGGER,
    ZOMBIE_DYING,
    ZOMBIE_DEAD
} ZombieState;

#define MAX_ZOMBIES 64

typedef struct {
    bool         active;
    ZombieType   type;
    ZombieState  state;
    Vec3         pos;
    float        yaw;
    float        hp;
    float        max_hp;
    float        speed;       /* current movement speed, m/s          */
    float        attack_timer;
    float        anim_time;   /* drives the walk/death animation      */
    float        death_time;  /* 0..1 fade-out when dying             */
    uint8_t      drop;        /* GraveDrop, spawned on death          */
} Zombie;

/* ------------------------------------------------------------------ */
/* pickups & materials                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    DROP_NONE = 0,
    DROP_COIN,      /* quest material: old coins                    */
    DROP_SINEW,     /* quest material: rot sinew                    */
    DROP_AMMO,      /* flintlock ammo refill                        */
    DROP_COUNT
} GraveDrop;

#define MAX_PICKUPS 64

typedef struct {
    bool    active;
    GraveDrop kind;
    Vec3    pos;
    float   bob_time;
    float   life;
} Pickup;

/* ------------------------------------------------------------------ */
/* world props (collision + drawing boxes)                            */
/* ------------------------------------------------------------------ */

typedef enum {
    PROP_CRATE = 0,
    PROP_BARREL,
    PROP_FENCE,
    PROP_TOMBSTONE,
    PROP_DEADTREE,
    PROP_WELL,
    PROP_GATE,
    PROP_KIND_COUNT
} PropKind;

#define MAX_PROPS 192

typedef struct {
    Vec3    pos;       /* centre of the base                        */
    Vec3    half;      /* half extents for collision                */
    float   yaw;
    PropKind kind;
    uint8_t color;     /* palette index, renderer picks the shade   */
} Prop;

/* ------------------------------------------------------------------ */
/* quests & zones                                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    ZONE_ASHEN_MILL = 0,   /* v0.1: the walled yard                   */
    ZONE_BONE_ORCHARD,     /* v0.2                                    */
    ZONE_COUNT
} ZoneId;

typedef enum {
    QUEST_NONE = 0,
    QUEST_CLEAR_ZONE,      /* kill N zombies in the zone              */
    QUEST_COLLECT,         /* bring back N of a material              */
    QUEST_DONE
} QuestKind;

typedef struct {
    QuestKind kind;
    int       target;
    int       progress;
    int       material;    /* GraveDrop for collect quests            */
} Quest;

/* ------------------------------------------------------------------ */
/* player                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    ActorTransform transform;
    float   hp;
    float   max_hp;
    float   stamina;
    float   velocity_y;    /* simple gravity, kept for jump later      */
    bool    on_ground;
    WeaponId weapon;
    int     ammo[WEAPON_COUNT];
    float   reload_timer;
    float   fire_cooldown;
    float   recoil;        /* camera kick, radians                     */
    bool    camera_first_person;
    bool    alive;
    float   anim_time;     /* walk cycle                               */
    float   anim_speed;    /* 0 = idle, 1 = walk                       */
    bool    firing;        /* muzzle flash / shot flash timer          */
    float   muzzle_flash;
} Player;

/* ------------------------------------------------------------------ */
/* events: consumed by the renderer and audio layer                   */
/* ------------------------------------------------------------------ */

enum {
    GRAVE_EVT_SHOOT     = 1u << 0,
    GRAVE_EVT_RELOAD    = 1u << 1,
    GRAVE_EVT_HIT       = 1u << 2,
    GRAVE_EVT_ZOMBIE_DIE= 1u << 3,
    GRAVE_EVT_PICKUP    = 1u << 4,
    GRAVE_EVT_HURT      = 1u << 5,
    GRAVE_EVT_QUEST_DONE= 1u << 6,
    GRAVE_EVT_ZONE_DONE = 1u << 7,
    GRAVE_EVT_EMPTY_GUN = 1u << 8,
};

/* a shot tracer / impact for the renderer */
typedef struct {
    Vec3 start;
    Vec3 end;
    float life;
    uint8_t kind;      /* 0 = tracer, 1 = impact spark             */
} ShotTrace;

#define MAX_SHOT_TRACES 32

/* ------------------------------------------------------------------ */
/* game state                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    GAME_PHASE_TITLE = 0,
    GAME_PHASE_PLAYING,
    GAME_PHASE_PAUSED,
    GAME_PHASE_QUEST_COMPLETE,
    GAME_PHASE_GAME_OVER
} GamePhase;

typedef struct {
    GamePhase phase;
    ZoneId    zone;
    Quest     quest;
    Player    player;

    Zombie    zombies[MAX_ZOMBIES];
    Pickup    pickups[MAX_PICKUPS];
    Prop      props[MAX_PROPS];
    int       prop_count;

    ShotTrace shot_traces[MAX_SHOT_TRACES];

    int       material_coins;
    int       material_sinew;

    int       zombies_alive;
    int       zombies_killed;
    float     zone_time;

    uint32_t  events;          /* GRAVE_EVT_* accumulated this step  */
    uint32_t  frame;           /* fixed 60 Hz step counter           */
    uint32_t  rng;

    /* spawner */
    float     spawn_timer;
    int       spawn_budget;
    int       spawn_kill_goal;

    /* camera product for the renderer (filled by grave_update_camera) */
    Vec3      camera_pos;
    Vec3      camera_look;
    float     camera_fov;
} GraveGame;

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

void grave_game_init(GraveGame *game, uint32_t seed);
void grave_game_start_zone(GraveGame *game, ZoneId zone);
void grave_game_step(GraveGame *game, const GraveInput *input, float dt);

/* Fills game->camera_* from the player/transform.  The 3DS front end calls
 * this after stepping so the renderer just consumes the result. */
void grave_update_camera(GraveGame *game, float aspect);

/* Helpers used by the renderer for the hero/zombie pose. */
float grave_hero_walk_phase(const GraveGame *game);
void  grave_hero_root(const GraveGame *game, Vec3 *pos, float *yaw);

/* Input helpers: the 3DS backend converts hid state into GraveInput. */
void grave_input_reset(GraveInput *input);
void grave_input_stick(GraveInput *input, int32_t dx, int32_t dy);
void grave_input_touch(GraveInput *input, int32_t dx, int32_t dy);

/* Zone/material helpers. */
const char *grave_zone_name(ZoneId zone);
const char *grave_drop_name(GraveDrop drop);

#endif /* GRAVEPOWDER_H */
