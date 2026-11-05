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

    /* v0.4 (append-only): aim / gyro.
     * aim is held; gyro rates are rad/s supplied by the app's motion read.
     * gyro_yaw:  positive = turn right on screen.
     * gyro_pitch: positive = look up. */
    bool  aim;
    float gyro_yaw;
    float gyro_pitch;
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
    ZOMBIE_FOREMAN,       /* v0.3 boss: huge, slow, hits very hard    */
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
    DROP_BONE_CHARM,/* v0.2 quest material: bone charms             */
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
    PROP_TIMBER,   /* v0.3 OLD MINE: mine support frame / timber stack  */
    PROP_CART,     /* v0.3 OLD MINE: mine cart                           */
    PROP_ORE,      /* v0.3 OLD MINE: glowing crystal/ore cluster         */
    PROP_LANTERN,  /* v0.3 OLD MINE: lantern post                        */
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
    ZONE_BONE_ORCHARD,     /* v0.2: the graveyard                     */
    ZONE_OLD_MINE,         /* v0.2 stub: spec exists, layout is a WIP */
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
/* world / trail progression (v0.2)                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    ZoneId zone;           /* node identity, same value as its index  */
    bool   unlocked;       /* appears as travelable on the WORLD map  */
    bool   cleared;        /* quest has been completed at least once  */
    int    clears;         /* times the zone quest was completed      */
    float  best_time;      /* fastest clear in seconds, 0 = none      */
} ZoneNode;

/* The world map: every zone is a node; the player is either on the trail
 * (hub/map screen, `in_zone == false`) or inside `current`. */
typedef struct {
    ZoneNode nodes[ZONE_COUNT];
    ZoneId   current;      /* loaded zone, or the map cursor on trail */
    bool     in_zone;      /* false = on the trail / world map        */
} World;

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

    /* internal (append-only, app can ignore): the weapon the active reload
     * timer belongs to.  Lets the core cancel a reload if the app swaps
     * weapons by writing player.weapon directly. */
    WeaponId reload_weapon;

    /* v0.2 (append-only): inventory model for the PACK page - which guns the
     * player owns.  Ammo lives in `ammo[]` above. */
    bool    weapon_owned[WEAPON_COUNT];

    /* v0.3 (append-only): trader upgrades, applied wherever damage / max hp /
     * reload time are computed. */
    int     weapon_tune[WEAPON_COUNT]; /* +15% damage per level (max 3)     */
    int     vitality_level;            /* +10 max hp per level (max 5)      */
    int     reload_tune;               /* -8% reload time per level (max 3) */
    float   reload_duration;           /* effective seconds of current reload */

    /* v0.4 (append-only): aiming + horizontal velocity state for the
     * acceleration model (the core integrates it; UI can ignore). */
    bool    aiming;                    /* held aim, set from GraveInput.aim  */
    float   vel_x;                     /* current horizontal velocity, m/s   */
    float   vel_z;
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
    /* v0.3 */
    GRAVE_EVT_BOSS_SPAWN     = 1u << 9,
    GRAVE_EVT_BOSS_DIE       = 1u << 10,
    GRAVE_EVT_WORLD_COMPLETE = 1u << 11,
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

    /* internal (append-only): seed the RNG restarts from, so a zone restart
     * through grave_game_start_zone is a clean deterministic replay. */
    uint32_t  seed;

    /* ---- v0.2 additions (append-only) ------------------------------- */
    World     world;              /* zone nodes / travel state           */
    int       material_bone_charms; /* BONE ORCHARD quest material total */
    int       run_materials[DROP_COUNT]; /* materials gained this run      */
    int       total_kills;        /* lifetime kills (saved)              */
    float     total_time;         /* lifetime play time in seconds       */
    int       zone_runs;          /* lifetime zone starts (saved)        */
    /* v0.2 spawner: the current wave is queued as a whole so that
     * "one brute per wave" stays one brute per wave even when the alive cap
     * only frees a single slot at a time. */
    int       spawn_wave_left;
    int       spawn_wave_brutes;

    /* ---- v0.3 additions (append-only) ------------------------------- */
    int       total_shots;    /* lifetime shots fired (saved)              */
    int       total_hits;     /* lifetime shots that hit a zombie (saved)  */
    int       boss_spawned;   /* the OLD MINE Foreman spawned this run     */
    int       boss_alive;     /* Foreman is on the field right now         */
    bool      boss_defeated;  /* Foreman killed - gates the mine quest     */
    float     boss_hp;        /* Foreman current hp (UI boss bar)          */
    float     boss_max_hp;    /* Foreman max hp                            */
    bool      world_complete; /* OLD MINE cleared - campaign finished      */
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

/* ================================================================== */
/* v0.2: world progression, inventory, save data                      */
/*                                                                    */
/* All of this is additive to the v0.1 contract; nothing was renamed  */
/* or removed.  The front end reads GraveGame.world for the WORLD     */
/* page, calls the grave_save/grave_load buffer pair through its own  */
/* SD I/O, and reads inventory through the helpers below (PACK page). */
/* ================================================================== */

/* Unlocks the first zone (ASHEN MILL); called by grave_game_init. */
void grave_world_init(GraveGame *game);

/* Unlocks a zone node.  Returns true when the node is unlocked afterwards
 * (idempotent); false for an invalid ZoneId. */
bool grave_world_unlock(GraveGame *game, ZoneId zone);

bool grave_world_unlocked(const GraveGame *game, ZoneId zone);
bool grave_world_cleared(const GraveGame *game, ZoneId zone);

/* Leaves the loaded zone and puts the core back on the trail / world map
 * (phase becomes GAME_PHASE_TITLE; pools are left alone, the app drives the
 * menu pages). */
void grave_world_to_trail(GraveGame *game);

/* Unlock-respecting zone start: returns false and changes nothing when the
 * zone is still locked.  Equivalent to grave_game_start_zone + a check. */
bool grave_game_travel_to(GraveGame *game, ZoneId zone);

/* ---- inventory (PACK page) --------------------------------------- */

bool grave_inventory_owned(const GraveGame *game, WeaponId id);
int  grave_inventory_weapon_count(const GraveGame *game); /* owned guns */
int  grave_inventory_ammo(const GraveGame *game, WeaponId id); /* loaded rounds */
/* Materials only: coins / sinew / bone charms; 0 for other drop kinds. */
int  grave_inventory_material(const GraveGame *game, GraveDrop drop);

/* ---- save data (v0.3) -------------------------------------------- */

#define GRAVE_SAVE_MAGIC   0x47505633u /* "GPV3" */
#define GRAVE_SAVE_VERSION 3u
/* Fixed on-disk size in bytes; verified at compile time in game.c.
 * v2 images are rejected cleanly by the version field (app keeps fresh init). */
#define GRAVE_SAVE_SIZE    108

/* Portable fixed-size save image.  Serialised little-endian-by-layout (C
 * struct layout, same header on 3DS and host), checksummed, versioned.  The
 * buffer pair is pure so host tests can round-trip it; the 3DS front end owns
 * the actual SD card file I/O (see source/core/NOTES.md for the exact path
 * convention and the recommended write-temp-then-rename flow). */
typedef struct {
    uint32_t magic;             /* GRAVE_SAVE_MAGIC                     */
    uint16_t version;           /* GRAVE_SAVE_VERSION                   */
    uint16_t bytes;             /* sizeof(SaveData) == GRAVE_SAVE_SIZE  */
    uint32_t checksum;          /* FNV-1a over every byte after this    */
    uint32_t unlocked_mask;     /* bit (1u << (int)ZoneId)              */
    uint32_t cleared_mask;
    uint8_t  zone_clears[ZONE_COUNT];
    float    zone_best_time[ZONE_COUNT]; /* 0 = never cleared           */
    uint8_t  current_zone;      /* WORLD page cursor                    */
    uint8_t  camera_first_person;
    uint8_t  invert_y;
    uint8_t  touch_sens;
    uint8_t  muted;
    uint8_t  weapon_owned[WEAPON_COUNT];
    int32_t  material_coins;
    int32_t  material_sinew;
    int32_t  material_bone_charms;
    int32_t  ammo[WEAPON_COUNT]; /* loaded rounds at save time           */
    uint32_t kills_total;
    uint32_t runs;
    float    play_time;
    uint8_t  reserved[12];      /* keep the v0.2 part's layout stable   */

    /* ---- v0.3 appended fields --------------------------------------- */
    uint8_t  weapon_tune[WEAPON_COUNT];
    uint8_t  vitality_level;
    uint8_t  reload_tune;
    uint8_t  world_complete;
    uint8_t  reserved_v3[2];
    uint32_t shots_fired;
    uint32_t shots_hit;
} SaveData;

/* Front-end settings that the core does not simulate but should persist.  All
 * fields are optional; pass NULL to save/load without them.
 *
 * v0.4 (append-only): gyro settings.  They are stored inside SaveData.reserved
 * so the save format version/size are unchanged; a pre-v0.4 (zeroed reserved)
 * image loads as ON / MED. */
typedef struct {
    uint8_t invert_y;
    uint8_t touch_sens;
    uint8_t muted;
    uint8_t reserved;
    uint8_t gyro_mode;   /* GraveGyroMode: 0 ON (default), 1 OFF, 2 INVERT */
    uint8_t gyro_sens;   /* GraveGyroSens: 0 MED (default), 1 LOW, 2 HIGH */
} GraveSettings;

/* sizeof(SaveData); also the exact number of bytes grave_save_to_buffer
 * writes.  Returns 0 if the build's layout does not match GRAVE_SAVE_SIZE. */
int  grave_save_size(void);

/* Serialises the persistent slice of the game state into buf.  Returns the
 * number of bytes written (== grave_save_size()), or 0 on bad arguments.
 * Two calls on the same state write byte-identical images. */
int  grave_save_to_buffer(const GraveGame *game, const GraveSettings *settings,
                          void *buf, int buf_size);

/* Validates (magic, version, size, checksum) and applies a save image.
 * On failure returns false and leaves `game` and `settings_out` untouched. */
bool grave_load_from_buffer(GraveGame *game, GraveSettings *settings_out,
                            const void *buf, int buf_size);

/* ================================================================== */
/* v0.3: OLD MINE, the Foreman, trader, accuracy, world completion    */
/*                                                                    */
/* Everything below is additive; nothing above was renamed/removed.   */
/* ================================================================== */

/* Arena half extent per zone (metres) so the AREA tab can scale its map. */
float grave_zone_half_extent(ZoneId zone);

/* ---- trader / shop ----------------------------------------------- */
/* The shop is only usable on the trail / world map (!world.in_zone).  All
 * costs are in materials (coins / sinew / bone charms); the core applies the
 * upgrade immediately on a successful buy. */

typedef enum {
    SHOP_AMMO = 0,           /* refill every owned gun to full magazine   */
    SHOP_TUNE_FLINTLOCK,     /* +15% flintlock damage per level (max 3)   */
    SHOP_TUNE_REVOLVER,      /* +15% revolver damage per level (max 3)    */
    SHOP_TUNE_MUSKET,        /* +15% musket damage per level (max 3)      */
    SHOP_VITALITY,           /* +10 max hp per level (max 5)              */
    SHOP_RELOAD,             /* -8% reload time per level (max 3)         */
    SHOP_MEDKIT,             /* heal 50% of max hp instantly              */
    SHOP_ITEM_COUNT
} ShopItem;

/* Which persistent player level a shop item indexes (NONE = consumable). */
typedef enum {
    SHOP_LEVEL_NONE = 0,
    SHOP_LEVEL_WEAPON_TUNE,  /* levels: player.weapon_tune[spec->weapon]  */
    SHOP_LEVEL_VITALITY,     /* level: player.vitality_level              */
    SHOP_LEVEL_RELOAD        /* level: player.reload_tune                 */
} ShopLevelField;

/* Stable description ids so the UI can style/translate without parsing the
 * name strings. */
typedef enum {
    SHOP_DESC_AMMO = 0,
    SHOP_DESC_TUNE,
    SHOP_DESC_VITALITY,
    SHOP_DESC_RELOAD,
    SHOP_DESC_MEDKIT,
    SHOP_DESC_COUNT
} ShopDesc;

#define SHOP_MAX_LEVELS 5

typedef struct {
    const char    *name;
    ShopDesc       desc;
    /* cost tables indexed by the *current* level (0-based): the price of
     * buying level+1.  Unused entries are 0. */
    int            cost_coins[SHOP_MAX_LEVELS];
    int            cost_sinew[SHOP_MAX_LEVELS];
    int            cost_charms[SHOP_MAX_LEVELS];
    int            max_level;      /* 0 = repeatable consumable             */
    ShopLevelField level_field;
    int            weapon;         /* WeaponId for tune items, else -1      */
} ShopItemSpec;

const ShopItemSpec *grave_shop_item(ShopItem item);

/* Current level of the item (0 for consumables / unknown items). */
int  grave_shop_level(const GraveGame *game, ShopItem item);

/* True when the buy would succeed: on the trail, below max level (or a
 * repeatable consumable that would actually do something), and affordable. */
bool grave_shop_can_buy(const GraveGame *game, ShopItem item);

/* Buys + applies the item.  Returns false and changes nothing if
 * grave_shop_can_buy is false. */
bool grave_shop_buy(GraveGame *game, ShopItem item);

/* ================================================================== */
/* v0.4: aiming / gyro settings (append-only)                         */
/* ================================================================== */

/* Gyro mode stored in GraveSettings.gyro_mode (0 = ON is the default). */
typedef enum {
    GRAVE_GYRO_ON = 0,      /* gyro turn enabled (default)               */
    GRAVE_GYRO_OFF = 1,     /* ignore gyro rates                          */
    GRAVE_GYRO_INVERT = 2,  /* enabled, yaw rate sign flipped             */
    GRAVE_GYRO_MODE_COUNT
} GraveGyroMode;

/* Gyro sensitivity step stored in GraveSettings.gyro_sens (0 = MED default).
 * The app applies the actual multiplier when it converts sensor rates into
 * GraveInput.gyro_yaw / gyro_pitch; the core always consumes rad/s. */
typedef enum {
    GRAVE_GYRO_SENS_MED = 0,
    GRAVE_GYRO_SENS_LOW = 1,
    GRAVE_GYRO_SENS_HIGH = 2,
    GRAVE_GYRO_SENS_COUNT
} GraveGyroSens;

#endif /* GRAVEPOWDER_H */
