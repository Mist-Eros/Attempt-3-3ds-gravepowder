/*
 * Gravepowder - app internals (3DS build only)
 *
 * The public contract lives in include/app.h; this header defines the concrete
 * App state that app.c and ui.c share and that main.c may inspect.  It is
 * deliberately not part of the portable contract: everything here is either
 * 3DS/citro2d specific or private to the front end.
 */
#ifndef GRAVEPOWDER_APP_INTERNAL_H
#define GRAVEPOWDER_APP_INTERNAL_H

#include <citro2d.h>

#include "../include/app.h"
#include "../include/audio.h"

/* Which bottom-screen page the UI should draw.  The world keeps rendering
 * behind the pause/result pages (the renderer is phase-agnostic). */
typedef enum {
    APP_PAGE_TITLE = 0,
    APP_PAGE_TITLE_CONFIRM, /* "start a new game? this erases your save"  */
    APP_PAGE_HUD,
    APP_PAGE_PAUSE,
    APP_PAGE_SETTINGS,
    APP_PAGE_SAVE,          /* settings -> save & progress sub-page       */
    APP_PAGE_WARDROBE,      /* pause -> model sources / reload models     */
    APP_PAGE_ZONE_CLEAR,
    APP_PAGE_GAME_OVER
} AppPage;

/* Touch sensitivity presets (the middle one is the default). */
enum {
    APP_SENS_LOW = 0,
    APP_SENS_MED,
    APP_SENS_HIGH,
    APP_SENS_COUNT
};

/* v0.5: procedural mascot moods (driven by events / HP in app_step). */
typedef enum {
    APP_MASCOT_IDLE = 0,
    APP_MASCOT_HAPPY,
    APP_MASCOT_WORRIED,
    APP_MASCOT_PALE
} AppMascotMood;

/* Bottom-screen info tabs (the HUD page is now a paged info display). */
typedef enum {
    APP_TAB_AREA = 0,   /* top-down map of the current zone            */
    APP_TAB_WORLD,      /* valley map / zone nodes                     */
    APP_TAB_PACK,       /* weapons, materials, character stats         */
    APP_TAB_SHOP,       /* trailhead trader (only on the trail)        */
    APP_TAB_COUNT
} AppTab;

struct App {
    /* Portable simulation state. */
    GraveGame game;
    GraveInput input;        /* held/edge flags latched by app_poll_input */
    float look_accum_x;      /* touch drag since the last fixed step        */
    float look_accum_y;
    uint32_t seed;

    /* Scene / UI. */
    AppPage page;
    bool paused;             /* app-owned pause (the core is not stepped)  */
    bool quit_requested;
    float time;              /* wall clock for pulses/particles            */
    float hit_flash;         /* damage feedback timer (0..1 fade)          */
    float prev_hp;

    /* Bottom-screen paging. */
    AppTab tab;              /* current info tab                            */
    AppTab tab_prev;         /* tab that is sliding out (transition)        */
    float tab_anim;          /* remaining transition time (0 = idle)        */

    /* Touch state machine (drag = aim, short tap = UI). */
    bool touch_active;
    bool touch_dragging;
    int touch_prev_x;
    int touch_prev_y;
    int touch_start_x;
    int touch_start_y;
    float touch_travel;      /* total movement in px, distinguishes tap    */

    /* Front-end settings.  This is the persisted GraveSettings image the
     * core save buffer serialises, so it is the single source of truth. */
    GraveSettings settings;  /* invert_y, touch_sens, muted, reserved   */

    /* Result snapshot so the zone-clear page keeps its numbers.  The gain
     * fields are (materials at clear - materials at zone entry), so the card
     * can show the quest payout line without duplicating core math. */
    float result_time;
    int result_kills;
    int result_coins;
    int result_sinew;
    int result_gain_coins;
    int result_gain_sinew;
    int result_gain_charms;
    int zone_start_coins;
    int zone_start_sinew;
    int zone_start_charms;

    /* v0.3 front-end state. */
    bool has_save;           /* a valid save file was loaded at boot       */
    bool save_erase_armed;   /* save sub-page: first erase tap done        */
    uint32_t new_zone_mask;  /* app-local, non-persisted WORLD NEW badges  */
    uint32_t known_unlock_mask; /* previous frame unlock state (edge detect) */
    int travel_confirm_zone; /* >= 0: travel dialog is open for that zone  */
    char toast[48];          /* transient bottom-screen feedback line      */
    float toast_timer;
    bool wardrobe_reload_ok; /* result of the last r3d_reload_all_models() */
    float wardrobe_flash;    /* timer for the reload status line           */

    /* v0.5: 3DS motion pipeline (gyro code compiles only on device; the host
     * preview harness keeps these fields at 0/false). */
    bool gyro_attempted;     /* lazy enable tried on first poll frame      */
    bool gyro_ok;            /* HIDUSER enables + coefficient succeeded    */
    bool gyro_calibrated;    /* rest-frame bias measured                   */
    float gyro_coef;         /* raw gyro units -> degrees/second           */
    float gyro_bias;         /* averaged rest rate subtracted from raw     */
    float gyro_bias_acc;     /* calibration accumulator                    */
    int gyro_calib_left;     /* calibration frames remaining               */
    float gyro_filtered;     /* low-pass filtered yaw rate (dps)           */

    /* v0.5 juice / animation timers (all deterministic, dt-driven). */
    int mascot_mood;         /* AppMascotMood                              */
    float mascot_timer;      /* remaining time in the current mood         */
    float press_timer;       /* touch-press ring timer                     */
    float press_x;
    float press_y;
    float display_coins;     /* count-up values shown in PACK/SHOP         */
    float display_sinew;
    float display_charms;
    float banner_timer;      /* wave/boss pop-in banner                    */
    char banner[32];

    /* Audio bookkeeping. */
    GraveMusic music;
    int last_weapon;         /* WeaponId last reported to audio_set_weapon()  */

    /* citro2d resources owned by the app (ui.c draws through these). */
    C2D_TextBuf text_buf;
    C2D_Text text_cache;     /* scratch handle; ui.c parses into text_buf   */

    bool running;
};

/* Bottom-screen helper implemented in ui.c.  ui_hit_test returns true and
 * writes the UI action id + argument when (px,py) hits a clickable rect of the
 * page currently shown. */
void ui_draw(const App *app);
bool ui_hit_test(const App *app, int px, int py, int *action, int *arg);

/* Zone the title screen CONTINUE will actually enter (same helper the app
 * action uses; ui.c prints it so the button label can never lie). */
ZoneId app_title_target_zone(const App *app);

#endif /* GRAVEPOWDER_APP_INTERNAL_H */
