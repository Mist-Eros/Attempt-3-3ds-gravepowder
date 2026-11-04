/*
 * Gravepowder - audio contract
 *
 * Portable interface: source/audio.c is the host fallback (records events so
 * the preview tools/tests can verify what *would* play), while
 * source/audio_3ds.c implements it with libctru's ndsp mixer on hardware.
 */
#ifndef GRAVEPOWDER_AUDIO_H
#define GRAVEPOWDER_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    GRAVE_MUSIC_NONE = 0,
    GRAVE_MUSIC_MENU,
    GRAVE_MUSIC_ZONE,
    GRAVE_MUSIC_COUNT
} GraveMusic;

void audio_init(void);
void audio_exit(void);

/* Consumes the GRAVE_EVT_* bits the core accumulated this frame. */
void audio_play_event(uint32_t grave_events);

/* Idempotent track switching; only changes when the track differs. */
void audio_set_music(GraveMusic track);
GraveMusic audio_current_music(void);

/* Host/debug helpers (harmless zeros on 3DS). */
uint32_t audio_event_history(void);
uint32_t audio_event_count(void);

void audio_set_muted(bool muted);
bool audio_is_muted(void);

/*
 * Append-only extension: tell the mixer which weapon is equipped so SHOOT and
 * RELOAD can pick the matching report (REVOLVER has three shot variants).
 * Pass a WeaponId value from grave.h (0 = flintlock, 1 = revolver, 2 = musket).
 * The portable fallback only records it; the 3DS backend defaults to the
 * flintlock until the app layer calls this (once at startup / on switch).
 */
void audio_set_weapon(int weapon_id);

#endif /* GRAVEPOWDER_AUDIO_H */
