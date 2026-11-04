/*
 * Gravepowder - bottom screen UI (citro2d)
 *
 * Old-paper / weathered Western look: bone, brass and ink with red accents for
 * danger.  All clickable areas are exposed as ui_*_rect() accessors so the app
 * layer and tooling can hit-test without duplicating layout constants.
 */
#ifndef GRAVEPOWDER_UI_H
#define GRAVEPOWDER_UI_H

#include <stdbool.h>

#include "app.h"

#define UI_SCREEN_W 320
#define UI_SCREEN_H 240

/* UI actions returned by ui_hit_test(). */
typedef enum {
    UI_ACTION_NONE = 0,
    UI_ACTION_START,          /* title prompt / tap anywhere on title     */
    UI_ACTION_PAUSE,          /* HUD pause button                         */
    UI_ACTION_RESUME,         /* pause -> resume                          */
    UI_ACTION_RESTART,        /* pause -> restart zone                    */
    UI_ACTION_SETTINGS,       /* pause -> settings page                   */
    UI_ACTION_SETTINGS_BACK,  /* settings -> pause                        */
    UI_ACTION_QUIT_TITLE,     /* pause -> title                           */
    UI_ACTION_WEAPON,         /* arg = weapon index (0..WEAPON_COUNT-1)   */
    UI_ACTION_TOGGLE_CAMERA,  /* settings: first/third person             */
    UI_ACTION_CYCLE_SENS,     /* settings: touch sensitivity preset       */
    UI_ACTION_TOGGLE_INVERT,  /* settings: invert Y                       */
    UI_ACTION_TOGGLE_MUTE,    /* settings: mute audio                     */
    UI_ACTION_CONTINUE,       /* zone-clear -> title                      */
    UI_ACTION_RETRY           /* game-over -> restart zone                */
} UiAction;

/* Rect type for hit-testing (UI space: 0,0 = bottom screen top-left). */
typedef struct {
    float x, y, w, h;
} UiRect;

/* Draw the whole bottom screen for the app's current page. */
void ui_draw(const App *app);

/* Hit-test a touch point against the current page's clickable rects.
 * Returns true when an action was hit (action/arg filled in). */
bool ui_hit_test(const App *app, int px, int py, int *action, int *arg);

/* --- layout accessors (shared by app/ui; safe for preview tooling) ------- */
UiRect ui_pause_rect(void);
UiRect ui_weapon_slot_rect(int index);
UiRect ui_title_start_rect(void);
UiRect ui_pause_resume_rect(void);
UiRect ui_pause_restart_rect(void);
UiRect ui_pause_settings_rect(void);
UiRect ui_pause_quit_rect(void);
UiRect ui_settings_back_rect(void);
UiRect ui_settings_camera_rect(void);
UiRect ui_settings_sens_rect(void);
UiRect ui_settings_invert_rect(void);
UiRect ui_settings_mute_rect(void);
UiRect ui_results_continue_rect(void);
UiRect ui_gameover_retry_rect(void);

#endif /* GRAVEPOWDER_UI_H */
