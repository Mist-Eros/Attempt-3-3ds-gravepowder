/*
 * Gravepowder - application layer (3DS build)
 *
 * Owns:
 *   - the scene state machine: TITLE -> PLAYING/HUD -> PAUSE -> SETTINGS ->
 *     ZONE CLEAR / GAME OVER
 *   - 3DS input -> GraveInput mapping (circle pad, touch drag aim, buttons,
 *     bottom-screen taps)
 *   - audio event dispatch + music track switching
 *   - citro2d ownership (C2D_Init, text buffer) and the bottom-screen frame
 *
 * The core sim is fixed-step and portable; this file only feeds it and reads
 * it back.  See include/app.h for the public contract.
 */
#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "ui.h"

#include "audio.h"
#include "grave.h"
#include "render3d.h"

#define FIXED_DT (1.0f / 60.0f)
#define BOTTOM_CLEAR C2D_Color32(0x17, 0x12, 0x0E, 0xFF)

/* Touch sensitivity presets: multiplier applied to the raw drag delta. */
static const float kSensScale[APP_SENS_COUNT] = { 0.55f, 1.0f, 1.55f };

static App g_app;

/* ------------------------------------------------------------------ */
/* scene transitions                                                  */
/* ------------------------------------------------------------------ */

static void start_zone(App *a)
{
    grave_game_start_zone(&a->game, ZONE_ASHEN_MILL);
    a->last_weapon = (int)a->game.player.weapon;
    audio_set_weapon(a->last_weapon);
    a->page = APP_PAGE_HUD;
    a->paused = false;
    a->hit_flash = 0.0f;
    a->prev_hp = a->game.player.hp;
    a->look_accum_x = 0.0f;
    a->look_accum_y = 0.0f;
}

static void back_to_title(App *a)
{
    grave_game_init(&a->game, a->seed);
    a->seed++;
    a->last_weapon = (int)a->game.player.weapon;
    audio_set_weapon(a->last_weapon);
    a->page = APP_PAGE_TITLE;
    a->paused = false;
    a->hit_flash = 0.0f;
    a->prev_hp = a->game.player.hp;
}

static void capture_results(App *a, AppPage page)
{
    a->result_time = a->game.zone_time;
    a->result_kills = a->game.zombies_killed;
    a->result_coins = a->game.material_coins;
    a->result_sinew = a->game.material_sinew;
    a->page = page;
    a->paused = false;
}

static void apply_action(App *a, int action, int arg)
{
    switch (action) {
    case UI_ACTION_START:
        if (a->page == APP_PAGE_TITLE)
            start_zone(a);
        break;
    case UI_ACTION_PAUSE:
        if (a->page == APP_PAGE_HUD) {
            a->paused = true;
            a->page = APP_PAGE_PAUSE;
        }
        break;
    case UI_ACTION_RESUME:
        if (a->page == APP_PAGE_PAUSE) {
            a->paused = false;
            a->page = APP_PAGE_HUD;
        }
        break;
    case UI_ACTION_RESTART:
        start_zone(a);
        break;
    case UI_ACTION_SETTINGS:
        if (a->page == APP_PAGE_PAUSE)
            a->page = APP_PAGE_SETTINGS;
        break;
    case UI_ACTION_SETTINGS_BACK:
        if (a->page == APP_PAGE_SETTINGS)
            a->page = APP_PAGE_PAUSE;
        break;
    case UI_ACTION_QUIT_TITLE:
        back_to_title(a);
        break;
    case UI_ACTION_WEAPON:
        if (arg >= 0 && arg < WEAPON_COUNT)
            a->game.player.weapon = (WeaponId)arg;
        break;
    case UI_ACTION_TOGGLE_CAMERA:
        a->game.player.camera_first_person = !a->game.player.camera_first_person;
        break;
    case UI_ACTION_CYCLE_SENS:
        a->touch_sens = (a->touch_sens + 1) % APP_SENS_COUNT;
        break;
    case UI_ACTION_TOGGLE_INVERT:
        a->invert_y = !a->invert_y;
        break;
    case UI_ACTION_TOGGLE_MUTE:
        a->muted = !a->muted;
        audio_set_muted(a->muted);
        break;
    case UI_ACTION_CONTINUE:
        back_to_title(a);
        break;
    case UI_ACTION_RETRY:
        start_zone(a);
        break;
    default:
        break;
    }
}

static void update_music(App *a)
{
    GraveMusic want;

    if (a->page == APP_PAGE_TITLE)
        want = GRAVE_MUSIC_MENU;
    else
        want = GRAVE_MUSIC_ZONE; /* playing, paused and the results pages keep
                                    the zone track; stings come via events   */

    if (want != a->music) {
        audio_set_music(want);
        a->music = want;
    }
}

/* ------------------------------------------------------------------ */
/* contract: init / shutdown                                          */
/* ------------------------------------------------------------------ */

void app_init(void)
{
    App *a = &g_app;

    memset(a, 0, sizeof(*a));
    a->running = true;
    a->seed = 0xC0FFEEu;
    a->page = APP_PAGE_TITLE;
    a->touch_sens = APP_SENS_MED;
    a->invert_y = false;
    a->muted = false;
    a->music = GRAVE_MUSIC_NONE;
    a->prev_hp = 0.0f;
    a->time = 0.0f;

    /* C3D is already up (r3d_init runs first in main.c).  citro2d and its text
     * buffer are owned by the app layer. */
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();  /* once: citro2d owns the GPU state from here on */
    a->text_buf = C2D_TextBufNew(4096);

    audio_init();
    audio_set_muted(false);

    grave_game_init(&a->game, a->seed);
    a->seed++;
    a->prev_hp = a->game.player.hp;
    a->last_weapon = (int)a->game.player.weapon;
    grave_input_reset(&a->input);
    audio_set_weapon(a->last_weapon);
}

void app_shutdown(void)
{
    App *a = &g_app;

    audio_exit();
    if (a->text_buf != NULL)
        C2D_TextBufDelete(a->text_buf);
    a->text_buf = NULL;
    C2D_Fini();
}

/* ------------------------------------------------------------------ */
/* input                                                              */
/* ------------------------------------------------------------------ */

void app_poll_input(void)
{
    App *a = &g_app;
    u32 held;
    u32 down;
    circlePosition stick;
    touchPosition touch;
    int sens;
    float sensScale;

    held = hidKeysHeld();
    down = hidKeysDown();

    /* ---------------- circle pad ---------------- */
    hidCircleRead(&stick);
    grave_input_reset(&a->input);
    grave_input_stick(&a->input, stick.dx, stick.dy);

    /* R = steady walk (precision strafe): slows the camera-relative movement
     * so it is easy to strafe past a zombie without overshooting.  grave.h has
     * no dedicated strafe flag in v0.1, so this is applied here. */
    if (held & KEY_R) {
        a->input.move_x *= 0.45f;
        a->input.move_y *= 0.45f;
    }

    a->input.fire = (held & KEY_A) != 0;
    a->input.sprint = (held & KEY_L) != 0;
    a->input.reload = (down & KEY_B) != 0;
    a->input.swap_weapon = (down & KEY_X) != 0;
    a->input.toggle_camera = (down & KEY_Y) != 0;

    /* ---------------- touch: drag = aim, short tap = UI ---------------- */
    sens = a->touch_sens;
    if (sens < 0 || sens >= APP_SENS_COUNT)
        sens = APP_SENS_MED;
    sensScale = kSensScale[sens];

    hidTouchRead(&touch);
    if ((held & KEY_TOUCH) != 0) {
        if (!a->touch_active) {
            a->touch_active = true;
            a->touch_dragging = false;
            a->touch_start_x = touch.px;
            a->touch_start_y = touch.py;
            a->touch_prev_x = touch.px;
            a->touch_prev_y = touch.py;
            a->touch_travel = 0.0f;
        } else {
            int ddx = touch.px - a->touch_prev_x;
            int ddy = touch.py - a->touch_prev_y;

            a->touch_prev_x = touch.px;
            a->touch_prev_y = touch.py;
            a->touch_travel += fabsf((float)ddx) + fabsf((float)ddy);
            if (!a->touch_dragging && a->touch_travel > 10.0f)
                a->touch_dragging = true;
            if (a->touch_dragging && a->page == APP_PAGE_HUD) {
                a->look_accum_x += (float)ddx * sensScale / 160.0f;
                a->look_accum_y += (a->invert_y ? -(float)ddy : (float)ddy) *
                                   sensScale / 160.0f;
            }
        }
    } else if (a->touch_active) {
        int tx = a->touch_prev_x;
        int ty = a->touch_prev_y;

        a->touch_active = false;
        if (!a->touch_dragging) {
            int action = UI_ACTION_NONE;
            int arg = 0;

            if (ui_hit_test(a, tx, ty, &action, &arg))
                apply_action(a, action, arg);
        }
        a->touch_dragging = false;
    }

    /* ---------------- menu / pause button shortcuts ---------------- */
    if ((down & KEY_START) != 0) {
        switch (a->page) {
        case APP_PAGE_HUD:
            a->paused = true;
            a->page = APP_PAGE_PAUSE;
            break;
        case APP_PAGE_PAUSE:
            a->paused = false;
            a->page = APP_PAGE_HUD;
            break;
        case APP_PAGE_SETTINGS:
            a->page = APP_PAGE_PAUSE;
            break;
        case APP_PAGE_ZONE_CLEAR:
        case APP_PAGE_GAME_OVER:
            back_to_title(a);
            break;
        default:
            break;
        }
    }

    if ((down & KEY_A) != 0) {
        switch (a->page) {
        case APP_PAGE_TITLE:
            start_zone(a);
            break;
        case APP_PAGE_ZONE_CLEAR:
            back_to_title(a);
            break;
        case APP_PAGE_GAME_OVER:
            start_zone(a);
            break;
        case APP_PAGE_PAUSE:
            a->paused = false;
            a->page = APP_PAGE_HUD;
            break;
        case APP_PAGE_SETTINGS:
            a->page = APP_PAGE_PAUSE;
            break;
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* fixed step                                                         */
/* ------------------------------------------------------------------ */

void app_step(void)
{
    App *a = &g_app;

    a->time += FIXED_DT;
    if (a->hit_flash > 0.0f) {
        a->hit_flash -= FIXED_DT * 2.2f;
        if (a->hit_flash < 0.0f)
            a->hit_flash = 0.0f;
    }

    if (a->page == APP_PAGE_HUD) {
        GraveGame *g = &a->game;
        GraveInput in = a->input;
        u32 events;

        in.look_x = a->look_accum_x;
        in.look_y = a->look_accum_y;
        a->look_accum_x = 0.0f;
        a->look_accum_y = 0.0f;

        grave_game_step(g, &in, FIXED_DT);
        /* Fill game->camera_* for the renderer (5:3 = 400x240 top screen). */
        grave_update_camera(g, 400.0f / 240.0f);

        /* edge-triggered flags are consumed on the first step of the frame */
        a->input.reload = false;
        a->input.swap_weapon = false;
        a->input.toggle_camera = false;
        a->input.pause = false;

        events = g->events;
        g->events = 0;
        if ((int)g->player.weapon != a->last_weapon) {
            a->last_weapon = (int)g->player.weapon;
            audio_set_weapon(a->last_weapon);
        }
        audio_play_event(events);

        /* damage feedback */
        if (g->player.hp < a->prev_hp - 0.01f)
            a->hit_flash = 1.0f;
        a->prev_hp = g->player.hp;

        /* results: the core phase is authoritative, with the events and the
         * quest/HP state as a safety net */
        if (g->phase == GAME_PHASE_GAME_OVER || !g->player.alive ||
            g->player.hp <= 0.0f) {
            capture_results(a, APP_PAGE_GAME_OVER);
        } else if (g->phase == GAME_PHASE_QUEST_COMPLETE ||
                   (events & GRAVE_EVT_ZONE_DONE) != 0 ||
                   g->quest.kind == QUEST_DONE ||
                   (g->quest.kind == QUEST_CLEAR_ZONE && g->quest.target > 0 &&
                    g->quest.progress >= g->quest.target)) {
            capture_results(a, APP_PAGE_ZONE_CLEAR);
        }
    }

    update_music(a);
}

/* ------------------------------------------------------------------ */
/* drawing / frame support                                            */
/* ------------------------------------------------------------------ */

void app_draw_bottom(void)
{
    App *a = &g_app;
    C3D_RenderTarget *bottom = r3d_bottom_target();

    if (bottom == NULL)
        return;

    C2D_TargetClear(bottom, BOTTOM_CLEAR);
    C2D_SceneBegin(bottom);
    C2D_Prepare();  /* re-establish 2D GPU state after the 3D world pass */
    ui_draw(a);
    C2D_Flush();
}

/* ------------------------------------------------------------------ */
/* contract: accessors                                                */
/* ------------------------------------------------------------------ */

const GraveGame *app_game(void)
{
    return &g_app.game;
}

App *app_state(void)
{
    return &g_app;
}

bool app_running(void)
{
    return g_app.running;
}

void app_request_exit(void)
{
    g_app.running = false;
}
