/*
 * Gravepowder - portable audio fallback
 *
 * Deliberately contains no platform code: this is what host tools and tests
 * link against.  It records the GRAVE_EVT_* bits the app dispatches and the
 * requested music track/weapon so test harnesses can verify the wiring
 * without audio hardware.  The real mixer is source/audio_3ds.c (libctru
 * ndsp), compiled only into the 3DS build.
 *
 * Sound mapping (kept in sync with audio_3ds.c):
 *   GRAVE_EVT_SHOOT      -> weapon report (flintlock / 3 revolver variants / musket)
 *   GRAVE_EVT_RELOAD     -> per-weapon reload clacks
 *   GRAVE_EVT_HIT        -> zombie hit thud (occasionally a sinew squelch)
 *   GRAVE_EVT_ZOMBIE_DIE -> gurgle + collapse thud
 *   GRAVE_EVT_PICKUP     -> coin chime
 *   GRAVE_EVT_HURT       -> player grunt (sometimes a zombie swipe/groan)
 *   GRAVE_EVT_EMPTY_GUN  -> dry fire click
 *   GRAVE_EVT_QUEST_DONE -> quest-complete sting
 *   GRAVE_EVT_ZONE_DONE  -> zone-clear fanfare
 */
#include <stdbool.h>
#include <stdint.h>

#include "audio.h"

/*
 * The 3DS build compiles source/audio_3ds.c for the real functions; this file
 * is compiled there too but collapses to an empty translation unit.  Keeping
 * them in separate files means host builds never need libctru.
 */
#ifndef __3DS__

static bool s_initialized = false;
static bool s_muted = false;
static uint32_t s_history = 0;
static uint32_t s_calls = 0;
static GraveMusic s_music = GRAVE_MUSIC_NONE;
static int s_weapon = 0;

void audio_init(void)
{
    s_initialized = true;
    s_muted = false;
    s_history = 0;
    s_calls = 0;
    s_music = GRAVE_MUSIC_NONE;
    s_weapon = 0;
}

void audio_exit(void)
{
    s_initialized = false;
    s_music = GRAVE_MUSIC_NONE;
}

void audio_play_event(uint32_t grave_events)
{
    if (!s_initialized || s_muted || grave_events == 0)
        return;

    s_history |= grave_events;
    s_calls++;
}

void audio_set_music(GraveMusic track)
{
    if (!s_initialized)
        return;
    if (track != GRAVE_MUSIC_MENU && track != GRAVE_MUSIC_ZONE)
        track = GRAVE_MUSIC_NONE;
    s_music = track;
}

GraveMusic audio_current_music(void)
{
    return s_music;
}

uint32_t audio_event_history(void)
{
    return s_history;
}

uint32_t audio_event_count(void)
{
    return s_calls;
}

void audio_set_muted(bool muted)
{
    s_muted = muted;
}

bool audio_is_muted(void)
{
    return s_muted;
}

void audio_set_weapon(int weapon_id)
{
    if (weapon_id < 0)
        weapon_id = 0;
    if (weapon_id > 2)
        weapon_id = 2;
    s_weapon = weapon_id;
}

#endif /* !__3DS__ */
