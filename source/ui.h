/*
 * Gravepowder - bottom screen UI (citro2d)
 *
 * v0.3: the bottom screen is a paged info display:
 *   - a slim always-on status strip (health / stamina / weapon+ammo+reload /
 *     pause, plus a 3-layer boss bar while the Foreman is alive)
 *   - a four-tab bar (AREA / WORLD / PACK / SHOP) with a 0.15 s slide
 *   - one full page below it: per-zone AREA map, valley WORLD map, PACK
 *     inventory and the trailhead trader SHOP
 * Pause, settings, the SAVE sub-page, WARDROBE and the result pages overlay the
 * whole screen.
 *
 * All clickable areas are exposed as ui_*_rect() accessors so app.c and the
 * scratch preview harness can hit-test without duplicating layout constants.
 */
#ifndef GRAVEPOWDER_UI_H
#define GRAVEPOWDER_UI_H

/* Release version shown in the UI footer.  Single source of truth: bump this
 * one macro per release (no other file hard-codes the version string). */
#define GRAVEPOWDER_VERSION "v0.5"

#include <stdbool.h>

#include "app.h"

#define UI_SCREEN_W 320
#define UI_SCREEN_H 240

/* Slide transition when switching tabs, in seconds. */
#define UI_TAB_TRANSITION_TIME 0.15f

/* UI actions returned by ui_hit_test(). */
typedef enum {
    UI_ACTION_NONE = 0,
    UI_ACTION_START,          /* title: continue / new game (save-aware)  */
    UI_ACTION_PAUSE,          /* status strip pause button                */
    UI_ACTION_RESUME,         /* pause -> resume                          */
    UI_ACTION_RESTART,        /* pause -> restart zone                    */
    UI_ACTION_SETTINGS,       /* pause -> settings page                   */
    UI_ACTION_SETTINGS_BACK,  /* settings -> pause                        */
    UI_ACTION_WARDROBE,       /* pause -> wardrobe page                   */
    UI_ACTION_WARDROBE_RELOAD,/* wardrobe: r3d_reload_all_models()        */
    UI_ACTION_WARDROBE_BACK,  /* wardrobe -> pause                        */
    UI_ACTION_SETTINGS_SAVE,  /* settings -> save sub-page                */
    UI_ACTION_SAVE_BACK,      /* save sub-page -> settings                */
    UI_ACTION_SAVE_ERASE,     /* save sub-page: arm/confirm erase         */
    UI_ACTION_QUIT_TITLE,     /* pause -> title                           */
    UI_ACTION_TAB,            /* arg = AppTab to show                     */
    UI_ACTION_WEAPON,         /* arg = weapon index: equip (PACK cards)   */
    UI_ACTION_WORLD_NODE,     /* arg = ZoneId node tapped on WORLD map    */
    UI_ACTION_TRAVEL_YES,     /* travel confirmation dialog: travel       */
    UI_ACTION_TRAVEL_NO,      /* travel confirmation dialog: stay         */
    UI_ACTION_SHOP_ITEM,      /* arg = ShopItem to buy                    */
    UI_ACTION_TITLE_CONTINUE, /* title: load save and enter last camp     */
    UI_ACTION_TITLE_NEW_GAME, /* title: ask to confirm a fresh run        */
    UI_ACTION_NEW_GAME_YES,   /* confirm dialog: erase save and start     */
    UI_ACTION_CANCEL,         /* confirm dialog / armed state: back out   */
    UI_ACTION_TOGGLE_CAMERA,  /* settings: first/third person             */
    UI_ACTION_CYCLE_SENS,     /* settings: touch sensitivity preset       */
    UI_ACTION_TOGGLE_INVERT,  /* settings: invert Y                       */
    UI_ACTION_TOGGLE_MUTE,    /* settings: mute audio                     */
    UI_ACTION_CONTINUE,       /* zone-clear / ending -> title screen      */
    UI_ACTION_TRAVEL_ON,      /* zone-clear -> stay on the WORLD map page */
    UI_ACTION_ENDING_PLAY,    /* valley-cleared ending -> play again      */
    UI_ACTION_RETRY,          /* game-over -> restart zone                */
    /* v0.4 appended actions. */
    UI_ACTION_TOGGLE_GYRO,    /* settings: cycle GYRO ON/OFF/INVERT       */
    UI_ACTION_CYCLE_GYRO_SENS,/* settings: cycle GYRO SPEED LOW/MED/HIGH  */
    UI_ACTION_CYCLE_AIM_ASSIST/* settings: cycle AIM ASSIST OFF/NORMAL/STRONG */
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

/* --- layout accessors (shared by app/ui; safe for tooling) --------------- */
/* Always-on status strip + four-tab bar. */
UiRect ui_pause_rect(void);
UiRect ui_tab_rect(int tab);
/* AREA page. */
UiRect ui_area_map_rect(void);
/* WORLD page node hitboxes: index == ZoneId (0 Ashen Mill, 1 Bone Orchard,
 * 2 Old Mine).  All nodes are returned; locked nodes are still hit-tested so
 * the map geometry is stable and the app can ignore locked taps. */
#define UI_WORLD_NODE_COUNT (ZONE_COUNT)
UiRect ui_world_node_rect(int node);
/* PACK page: weapon cards (same order as WeaponId). */
UiRect ui_pack_weapon_rect(int index);
/* SHOP page: one item card per ShopItem. */
UiRect ui_shop_item_rect(int item);

/* Title page. */
UiRect ui_title_start_rect(void);       /* legacy single-prompt start       */
UiRect ui_title_continue_rect(void);    /* has-save title: CONTINUE         */
UiRect ui_title_newgame_rect(void);     /* has-save title: NEW GAME         */
UiRect ui_title_confirm_yes_rect(void); /* NEW GAME confirm: erase + start  */
UiRect ui_title_confirm_no_rect(void);  /* NEW GAME confirm: back out       */

/* Pause menu. */
UiRect ui_pause_resume_rect(void);
UiRect ui_pause_restart_rect(void);
UiRect ui_pause_settings_rect(void);
UiRect ui_pause_wardrobe_rect(void);
UiRect ui_pause_quit_rect(void);
/* Settings rows. */
UiRect ui_settings_back_rect(void);
UiRect ui_settings_camera_rect(void);
UiRect ui_settings_sens_rect(void);
UiRect ui_settings_gyro_rect(void);
UiRect ui_settings_gyro_sens_rect(void);
UiRect ui_settings_aim_rect(void);
UiRect ui_settings_invert_rect(void);
UiRect ui_settings_mute_rect(void);
UiRect ui_settings_save_rect(void);
/* Save sub-page. */
UiRect ui_save_erase_rect(void);
UiRect ui_save_back_rect(void);
/* WARDROBE page. */
UiRect ui_wardrobe_reload_rect(void);
UiRect ui_wardrobe_back_rect(void);
/* WORLD travel confirmation dialog. */
UiRect ui_travel_yes_rect(void);
UiRect ui_travel_no_rect(void);
/* Result pages. */
UiRect ui_results_continue_rect(void);   /* "TITLE" button                    */
UiRect ui_results_travel_rect(void);     /* "TRAVEL ON" -> WORLD map page     */
UiRect ui_results_play_rect(void);       /* ending: PLAY AGAIN                */
UiRect ui_gameover_retry_rect(void);

/* Legacy alias: the old HUD weapon slots are now the PACK weapon cards. */
UiRect ui_weapon_slot_rect(int index);

#endif /* GRAVEPOWDER_UI_H */
