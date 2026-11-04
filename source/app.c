/*
 * Gravepowder - application layer (3DS build)
 *
 * Owns:
 *   - the scene state machine: TITLE/trail -> PLAYING/HUD -> PAUSE -> SETTINGS
 *     -> ZONE CLEAR / GAME OVER, plus WORLD-map travel between zones
 *   - 3DS input -> GraveInput mapping (circle pad, touch drag aim, buttons,
 *     bottom-screen taps and tabs)
 *   - audio event dispatch + music track switching
 *   - the v0.2 save file: the core hands us a 92-byte portable image, this
 *     layer owns the SD card I/O (write-temp-then-rename, corrupt-safe load)
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

#ifdef __3DS__
#include <sys/stat.h>
#endif

#include "app.h"
#include "ui.h"

#include "audio.h"
#include "grave.h"
#include "render3d.h"

#define FIXED_DT (1.0f / 60.0f)
#define BOTTOM_CLEAR C2D_Color32(0x17, 0x12, 0x0E, 0xFF)

/* Touch sensitivity presets: multiplier applied to the raw drag delta. */
static const float kSensScale[APP_SENS_COUNT] = { 0.55f, 1.0f, 1.55f };

/* v0.2 save file location (3DS only; the host harness has no SD card). */
#define SAVE_DIR  "sdmc:/3ds/Gravepowder"
#define SAVE_PATH SAVE_DIR "/save.bin"
#define SAVE_TMP  SAVE_PATH ".tmp"

static App g_app;

/* ------------------------------------------------------------------ */
/* save file (SD I/O lives here, serialisation lives in the core)     */
/* ------------------------------------------------------------------ */

static void app_save(void)
{
#ifdef __3DS__
    App *a = &g_app;
    unsigned char buf[GRAVE_SAVE_SIZE];
    int size = grave_save_size();
    FILE *f;

    if (size <= 0 || size > (int)sizeof(buf))
        return;
    if (grave_save_to_buffer(&a->game, &a->settings, buf, size) != size)
        return;

    mkdir("sdmc:/3ds", 0777);   /* harmless if it already exists */
    mkdir(SAVE_DIR, 0777);
    f = fopen(SAVE_TMP, "wb");
    if (f == NULL)
        return;
    if (fwrite(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        remove(SAVE_TMP);
        return;
    }
    fclose(f);
    /* atomic-ish replace: a crash mid-write leaves the previous save intact */
    remove(SAVE_PATH);
    rename(SAVE_TMP, SAVE_PATH);
#else
    /* host harness: no SD card, nothing to persist */
#endif
}

static void app_load(void)
{
#ifdef __3DS__
    App *a = &g_app;
    unsigned char buf[GRAVE_SAVE_SIZE + 1u];
    int size = grave_save_size();
    FILE *f;
    size_t got;

    if (size <= 0 || size > GRAVE_SAVE_SIZE)
        return;
    f = fopen(SAVE_PATH, "rb");
    if (f == NULL)
        return;
    got = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if ((int)got != size)
        return; /* truncated/oversized: keep the fresh init */
    if (!grave_load_from_buffer(&a->game, &a->settings, buf, size)) {
        /* corrupt/old save: keep the fresh init, do not overwrite it here */
        a->settings.invert_y = 0;
        a->settings.touch_sens = (uint8_t)APP_SENS_MED;
        a->settings.muted = 0;
        a->settings.reserved = 0;
        return;
    }
    audio_set_muted(a->settings.muted != 0);
#else
    /* host harness: always start fresh */
#endif
}

/* ------------------------------------------------------------------ */
/* scene transitions                                                  */
/* ------------------------------------------------------------------ */

static void show_tab(App *a, AppTab tab)
{
    if (tab < 0 || tab >= APP_TAB_COUNT)
        tab = APP_TAB_AREA;
    if (tab == a->tab)
        return;
    a->tab_prev = a->tab;
    a->tab = tab;
    a->tab_anim = UI_TAB_TRANSITION_TIME;
}

static void cycle_tab(App *a)
{
    show_tab(a, (AppTab)(((int)a->tab + 1) % APP_TAB_COUNT));
}

/* Which zone the title screen's START should enter. */
static ZoneId title_target_zone(const App *a)
{
    ZoneId z = a->game.world.current;

    if ((int)z < 0 || (int)z >= ZONE_COUNT)
        z = ZONE_ASHEN_MILL;
    if (!a->game.world.nodes[z].unlocked)
        z = ZONE_ASHEN_MILL;
    return z;
}

/* Enters a zone through the core's unlock-respecting travel helper. */
static bool enter_zone(App *a, ZoneId zone)
{
    if (!grave_game_travel_to(&a->game, zone))
        return false;
    a->last_weapon = (int)a->game.player.weapon;
    audio_set_weapon(a->last_weapon);
    a->page = APP_PAGE_HUD;
    a->paused = false;
    a->hit_flash = 0.0f;
    a->prev_hp = a->game.player.hp;
    a->look_accum_x = 0.0f;
    a->look_accum_y = 0.0f;
    a->tab = APP_TAB_AREA;
    a->tab_prev = APP_TAB_AREA;
    a->tab_anim = 0.0f;
    return true;
}

/* Leaves the loaded zone and shows the title screen. */
static void back_to_title(App *a)
{
    grave_world_to_trail(&a->game);
    a->page = APP_PAGE_TITLE;
    a->paused = false;
    a->hit_flash = 0.0f;
    a->prev_hp = a->game.player.hp;
    a->tab = APP_TAB_AREA;
    a->tab_prev = APP_TAB_AREA;
    a->tab_anim = 0.0f;
    app_save();
}

/* Zone-clear "TRAVEL ON": leave the zone but stay on the world map page. */
static void go_to_world_map(App *a)
{
    grave_world_to_trail(&a->game);
    a->page = APP_PAGE_HUD;
    a->paused = false;
    a->hit_flash = 0.0f;
    a->prev_hp = a->game.player.hp;
    a->tab = APP_TAB_WORLD;
    a->tab_prev = APP_TAB_WORLD;
    a->tab_anim = 0.0f;
    app_save();
}

static void capture_results(App *a, AppPage page)
{
    a->result_time = a->game.zone_time;
    a->result_kills = a->game.zombies_killed;
    a->result_coins = a->game.material_coins;
    a->result_sinew = a->game.material_sinew;
    a->page = page;
    a->paused = false;
    app_save();
}

static void apply_action(App *a, int action, int arg)
{
    switch (action) {
    case UI_ACTION_START:
        if (a->page == APP_PAGE_TITLE)
            enter_zone(a, title_target_zone(a));
        break;
    case UI_ACTION_PAUSE:
        if (a->page == APP_PAGE_HUD) {
            if (!a->game.world.in_zone) {
                back_to_title(a); /* on the trail: pause button = leave */
            } else {
                a->paused = true;
                a->page = APP_PAGE_PAUSE;
            }
        }
        break;
    case UI_ACTION_RESUME:
        if (a->page == APP_PAGE_PAUSE) {
            a->paused = false;
            a->page = APP_PAGE_HUD;
        }
        break;
    case UI_ACTION_RESTART:
        enter_zone(a, a->game.world.current);
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
        if (arg >= 0 && arg < WEAPON_COUNT &&
            grave_inventory_owned(&a->game, (WeaponId)arg)) {
            a->game.player.weapon = (WeaponId)arg;
            a->last_weapon = arg;
            audio_set_weapon(arg);
            app_save();
        }
        break;
    case UI_ACTION_TAB:
        if (arg >= 0 && arg < APP_TAB_COUNT)
            show_tab(a, (AppTab)arg);
        break;
    case UI_ACTION_WORLD_NODE:
        if (arg >= 0 && arg < ZONE_COUNT) {
            ZoneId zone = (ZoneId)arg;

            if (zone == a->game.world.current && a->game.world.in_zone) {
                /* tapping where you already are = resume the zone view */
                a->page = APP_PAGE_HUD;
                a->paused = false;
                break;
            }
            if (grave_world_unlocked(&a->game, zone))
                enter_zone(a, zone);
        }
        break;
    case UI_ACTION_TRAVEL_ON:
        go_to_world_map(a);
        break;
    case UI_ACTION_TOGGLE_CAMERA:
        a->game.player.camera_first_person = !a->game.player.camera_first_person;
        app_save();
        break;
    case UI_ACTION_CYCLE_SENS:
        a->settings.touch_sens = (uint8_t)((a->settings.touch_sens + 1u) %
                                           (uint8_t)APP_SENS_COUNT);
        app_save();
        break;
    case UI_ACTION_TOGGLE_INVERT:
        a->settings.invert_y = a->settings.invert_y ? 0u : 1u;
        app_save();
        break;
    case UI_ACTION_TOGGLE_MUTE:
        a->settings.muted = a->settings.muted ? 0u : 1u;
        audio_set_muted(a->settings.muted != 0);
        app_save();
        break;
    case UI_ACTION_CONTINUE:
        back_to_title(a);
        break;
    case UI_ACTION_RETRY:
        enter_zone(a, a->game.zone);
        break;
    default:
        break;
    }
}

static void update_music(App *a)
{
    GraveMusic want;

    if (a->page == APP_PAGE_TITLE ||
        (a->page == APP_PAGE_HUD && !a->game.world.in_zone))
        want = GRAVE_MUSIC_MENU;
    else if (a->page == APP_PAGE_ZONE_CLEAR || a->page == APP_PAGE_GAME_OVER)
        want = GRAVE_MUSIC_ZONE; /* results keep the zone track; stings via events */
    else
        want = GRAVE_MUSIC_ZONE;

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
    a->tab = APP_TAB_AREA;
    a->tab_prev = APP_TAB_AREA;
    a->tab_anim = 0.0f;
    a->settings.invert_y = 0;
    a->settings.touch_sens = (uint8_t)APP_SENS_MED;
    a->settings.muted = 0;
    a->settings.reserved = 0;
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

    /* fresh core state (TITLE phase, trail mode), then overlay the save */
    grave_game_init(&a->game, a->seed);
    a->seed++;
    app_load();
    a->last_weapon = (int)a->game.player.weapon;
    grave_input_reset(&a->input);
    a->prev_hp = a->game.player.hp;
    audio_set_muted(a->settings.muted != 0);
    audio_set_weapon(a->last_weapon);
}

void app_shutdown(void)
{
    App *a = &g_app;

    app_save();
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
    sens = a->settings.touch_sens;
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
            if (a->touch_dragging && a->page == APP_PAGE_HUD &&
                a->game.world.in_zone) {
                a->look_accum_x += (float)ddx * sensScale / 160.0f;
                a->look_accum_y += (a->settings.invert_y ? -(float)ddy : (float)ddy) *
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
    if ((down & KEY_SELECT) != 0 && a->page == APP_PAGE_HUD)
        cycle_tab(a);

    if ((down & KEY_START) != 0) {
        switch (a->page) {
        case APP_PAGE_HUD:
            if (!a->game.world.in_zone) {
                back_to_title(a);
            } else {
                a->paused = true;
                a->page = APP_PAGE_PAUSE;
            }
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
            enter_zone(a, title_target_zone(a));
            break;
        case APP_PAGE_ZONE_CLEAR:
            back_to_title(a);
            break;
        case APP_PAGE_GAME_OVER:
            enter_zone(a, a->game.zone);
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
    if (a->tab_anim > 0.0f) {
        a->tab_anim -= FIXED_DT;
        if (a->tab_anim < 0.0f)
            a->tab_anim = 0.0f;
    }

    if (a->page == APP_PAGE_HUD && a->game.world.in_zone) {
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
