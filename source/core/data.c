/*
 * Gravepowder - data tables (portable)
 *
 * Weapon numbers, zone names and drop names live here so the core, the HUD and
 * the tests all read the same source of truth.  Tuning happens here only.
 */
#include <stddef.h>

#include "grave.h"

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
