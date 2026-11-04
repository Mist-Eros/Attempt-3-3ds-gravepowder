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
    APP_PAGE_HUD,
    APP_PAGE_PAUSE,
    APP_PAGE_SETTINGS,
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

    /* Touch state machine (drag = aim, short tap = UI). */
    bool touch_active;
    bool touch_dragging;
    int touch_prev_x;
    int touch_prev_y;
    int touch_start_x;
    int touch_start_y;
    float touch_travel;      /* total movement in px, distinguishes tap    */

    /* Settings (RAM only for v0.1). */
    int touch_sens;          /* APP_SENS_*                               */
    bool invert_y;
    bool muted;

    /* Result snapshot so the zone-clear page keeps its numbers. */
    float result_time;
    int result_kills;
    int result_coins;
    int result_sinew;

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

#endif /* GRAVEPOWDER_APP_INTERNAL_H */
