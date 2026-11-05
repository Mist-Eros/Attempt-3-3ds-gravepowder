/*
 * Gravepowder - data tables (portable)
 *
 * Weapon numbers, zone names and drop names live here so the core, the HUD and
 * the tests all read the same source of truth.  Tuning happens here only.
 */
#include <stddef.h>

#include "grave.h"
#include "core_internal.h"

static const WeaponSpec weapon_table[WEAPON_COUNT] = {
    [WEAPON_FLINTLOCK] = {
        "FLINTLOCK", 34, 1, 26.0f, 0.020f, 1, 0.90f, 1.30f, 1.2f,
    },
    [WEAPON_REVOLVER] = {
        "REVOLVER", 18, 1, 30.0f, 0.012f, 6, 0.32f, 2.10f, 0.7f,
    },
    [WEAPON_MUSKET] = {
        "MUSKET", 55, 1, 44.0f, 0.004f, 1, 1.60f, 2.60f, 2.0f,
    },
};

const WeaponSpec *grave_weapon_spec(WeaponId id)
{
    if (id < 0 || id >= WEAPON_COUNT)
        id = WEAPON_FLINTLOCK;
    return &weapon_table[id];
}

const char *grave_zone_name(ZoneId zone)
{
    switch (zone) {
    case ZONE_ASHEN_MILL:  return "ASHEN MILL";
    case ZONE_BONE_ORCHARD:return "BONE ORCHARD";
    case ZONE_OLD_MINE:    return "OLD MINE";
    default:               return "THE WILDS";
    }
}

const char *grave_drop_name(GraveDrop drop)
{
    switch (drop) {
    case DROP_COIN:       return "OLD COIN";
    case DROP_SINEW:      return "ROT SINEW";
    case DROP_AMMO:       return "POWDER";
    case DROP_BONE_CHARM: return "BONE CHARM";
    default:              return "";
    }
}

/* ------------------------------------------------------------------ */
/* zombie stats                                                       */
/*                                                                    */
/* v0.1 balance: a shambler dies to 2 flintlock body shots (or one    */
/* lucky head shot) and to 3 revolver rounds; a musket one-shots the  */
/* little ones.  Brutes are the "empty the cylinder" wall.  Damage is */
/* flat (no head/body split for the taking side).                     */
/* ------------------------------------------------------------------ */

static const ZombieSpec zombie_table[ZOMBIE_COUNT] = {
    /* hp    speed  dmg   interval windup reach radius height anim stagger */
    [ZOMBIE_SHAMBLER] = { 44.0f, 1.15f,  5.0f, 1.50f, 0.55f, 0.50f, 0.34f, 1.70f, 1.00f, 0.85f },
    [ZOMBIE_RUNNER]   = { 30.0f, 2.55f,  4.0f, 1.20f, 0.38f, 0.45f, 0.28f, 1.60f, 1.60f, 1.00f },
    [ZOMBIE_BRUTE]    = { 140.0f, 0.80f, 16.0f, 2.00f, 0.75f, 0.90f, 0.55f, 2.30f, 0.70f, 0.25f },
    /* v0.3 boss: 12x brute hp (three thick hp layers), very slow, giant reach,
     * never staggers.  Killed exactly once per OLD MINE run. */
    [ZOMBIE_FOREMAN]  = { 1680.0f, 0.50f, 30.0f, 2.80f, 1.20f, 1.70f, 1.20f, 3.40f, 0.50f, 0.00f },
};

const ZombieSpec *grave_zombie_spec(ZombieType type)
{
    if ((int)type < 0 || (int)type >= ZOMBIE_COUNT)
        type = ZOMBIE_SHAMBLER;
    return &zombie_table[type];
}

/* ------------------------------------------------------------------ */
/* zone tuning                                                        */
/*                                                                    */
/* Every zone runs the same generic quest ladder:                     */
/*   QUEST_CLEAR_ZONE  -> kill `kill_goal` zombies                    */
/*   QUEST_COLLECT     -> material_a, goal_a                          */
/*   QUEST_COLLECT     -> material_b, goal_b                          */
/* The drop cycle guarantees the quotas: per 4 kills a zombie drops    */
/* [A, B, A, (powder|A)], so `kill_goal` kills always drop far more A  */
/* and B than the collect steps ask for.                              */
/*                                                                    */
/* ASHEN MILL v0.1/v0.2   : 40 kills, 8 coins + 4 sinew               */
/* BONE ORCHARD v0.2      : 55 kills, 10 coins + 6 bone charms, a      */
/*                          brute in every wave, runner-heavy          */
/* OLD MINE v0.3          : 45 kills, 8 coins + 3 bone charms, tighter  */
/*                          corridors, a brute in every wave and the    */
/*                          Foreman boss as the final step              */
/* ------------------------------------------------------------------ */

static const ZoneSpec zone_table[ZONE_COUNT] = {
    /* half  budget kills alive wave  matA        goalA matB         goalB */
    [ZONE_ASHEN_MILL] = {
        20.0f, 48, 40, 8, 2.8f, DROP_COIN, 8, DROP_SINEW, 4,
        /* brute/wave runner% brute%  payout c/s/ch          layout */
        0, 0.37f, 0.18f, 70, 12, 0, ZONE_LAYOUT_ASHEN_MILL,
    },
    [ZONE_BONE_ORCHARD] = {
        20.0f, 64, 55, 8, 3.0f, DROP_COIN, 10, DROP_BONE_CHARM, 6,
        1, 0.55f, 0.20f, 140, 10, 10, ZONE_LAYOUT_BONE_ORCHARD,
    },
    [ZONE_OLD_MINE] = {
        20.0f, 55, 45, 7, 2.6f, DROP_COIN, 8, DROP_BONE_CHARM, 3,
        1, 0.50f, 0.24f, 200, 0, 14, ZONE_LAYOUT_OLD_MINE,
    },
};

const ZoneSpec *grave_zone_spec(ZoneId zone)
{
    if ((int)zone < 0 || (int)zone >= ZONE_COUNT)
        zone = ZONE_ASHEN_MILL;
    return &zone_table[zone];
}

float grave_zone_half_extent(ZoneId zone)
{
    return grave_zone_spec(zone)->half_extent;
}

/* ------------------------------------------------------------------ */
/* trader / shop (v0.3)                                               */
/*                                                                    */
/* Materials finally have a sink.  Prices are tuned so a mid-campaign  */
/* player can afford the first tune after the mill and keep up through */
/* the orchard; the --plain sim control skips shopping to compare.     */
/* ------------------------------------------------------------------ */

static const ShopItemSpec shop_table[SHOP_ITEM_COUNT] = {
    [SHOP_AMMO] = {
        "POWDER CHARGE", SHOP_DESC_AMMO,
        { 25, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 },
        0, SHOP_LEVEL_NONE, -1,
    },
    [SHOP_TUNE_FLINTLOCK] = {
        "FLINTLOCK TUNE", SHOP_DESC_TUNE,
        { 60, 120, 220, 0, 0 }, { 0, 0, 0, 0, 0 }, { 2, 4, 7, 0, 0 },
        GRAVE_TUNE_MAX_LEVEL, SHOP_LEVEL_WEAPON_TUNE, WEAPON_FLINTLOCK,
    },
    [SHOP_TUNE_REVOLVER] = {
        "REVOLVER TUNE", SHOP_DESC_TUNE,
        { 60, 120, 220, 0, 0 }, { 0, 0, 0, 0, 0 }, { 2, 4, 7, 0, 0 },
        GRAVE_TUNE_MAX_LEVEL, SHOP_LEVEL_WEAPON_TUNE, WEAPON_REVOLVER,
    },
    [SHOP_TUNE_MUSKET] = {
        "MUSKET TUNE", SHOP_DESC_TUNE,
        { 60, 120, 220, 0, 0 }, { 0, 0, 0, 0, 0 }, { 2, 4, 7, 0, 0 },
        GRAVE_TUNE_MAX_LEVEL, SHOP_LEVEL_WEAPON_TUNE, WEAPON_MUSKET,
    },
    [SHOP_VITALITY] = {
        "ROOT TONIC", SHOP_DESC_VITALITY,
        { 40, 40, 40, 40, 40 }, { 1, 1, 1, 1, 1 }, { 0, 0, 0, 0, 0 },
        GRAVE_VITALITY_MAX_LEVEL, SHOP_LEVEL_VITALITY, -1,
    },
    [SHOP_RELOAD] = {
        "QUICK HANDS", SHOP_DESC_RELOAD,
        { 50, 50, 50, 0, 0 }, { 2, 2, 2, 0, 0 }, { 0, 0, 0, 0, 0 },
        GRAVE_RELOAD_MAX_LEVEL, SHOP_LEVEL_RELOAD, -1,
    },
    [SHOP_MEDKIT] = {
        "FIELD MEDKIT", SHOP_DESC_MEDKIT,
        { 30, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 },
        0, SHOP_LEVEL_NONE, -1,
    },
};

const ShopItemSpec *grave_shop_item(ShopItem item)
{
    if ((int)item < 0 || (int)item >= SHOP_ITEM_COUNT)
        item = SHOP_AMMO;
    return &shop_table[item];
}

