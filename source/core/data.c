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
    default:               return "THE WILDS";
    }
}

const char *grave_drop_name(GraveDrop drop)
{
    switch (drop) {
    case DROP_COIN:  return "OLD COIN";
    case DROP_SINEW: return "ROT SINEW";
    case DROP_AMMO:  return "POWDER";
    default:         return "";
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
/* ASHEN MILL is the v0.1 yard: ~3 minutes of shooting to clear the   */
/* kill step plus the coin/sinew collection step.  BONE ORCHARD is a  */
/* v0.2 placeholder using the same arena builder for now.             */
/* ------------------------------------------------------------------ */

static const ZoneSpec zone_table[ZONE_COUNT] = {
    /* half  budget kills alive wave  coin sinew */
    [ZONE_ASHEN_MILL]   = { 20.0f, 48, 40, 8, 2.8f, 8, 4 },
    [ZONE_BONE_ORCHARD] = { 20.0f, 70, 58, 8, 2.8f, 10, 5 },
};

const ZoneSpec *grave_zone_spec(ZoneId zone)
{
    if ((int)zone < 0 || (int)zone >= ZONE_COUNT)
        zone = ZONE_ASHEN_MILL;
    return &zone_table[zone];
}

