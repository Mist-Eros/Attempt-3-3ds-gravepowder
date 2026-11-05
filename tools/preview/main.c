/*
 * Gravepowder - bottom-screen UI/app page-tour preview driver (host only)
 *
 * Drives the REAL source/app.c + source/ui.c scene state machine, linked against
 * the REAL portable core (source/core/game.c + input.c + data.c), with only
 * libctru/citro2d/render3d replaced by the shims in tools/preview/stub and
 * tools/preview/plat_host.c.  It is NOT part of the 3DS build.
 *
 * For every UI page it captures the citro2d draw list that source/ui.c emitted
 * (tools/preview/out/<page>.txt).  tools/preview/render_ui.py replays those
 * lists into 320x240 PNG mockups and runs the layout/legibility checks plus the
 * comparison against the committed Azahar baselines in docs/screenshots/.
 *
 * v0.3 page coverage: title (no-save + has-save + NEW GAME confirm), HUD
 * AREA/WORLD/PACK/SHOP tabs + tab slide + strip states (reload, low HP, boss
 * bar), pause/settings/save/wardrobe overlays, zone-clear + campaign ending,
 * TRAVEL ON trail pages, travel confirmation dialog, per-zone AREA maps,
 * game-over.  All state setup is deterministic (fixed seed, fixed frame counts,
 * fixture values written to the real Game/App state).
 *
 * Usage:
 *   preview.exe [--scenario all|title|hud|pause|trail|zones|gameover] [--out DIR]
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define HOST_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define HOST_MKDIR(p) mkdir((p), 0777)
#endif

#include <3ds.h>

#include "app.h"      /* source/app.h: internal App state (3DS front end) */
#include "ui.h"       /* source/ui.h: UI actions + layout rects            */

#include "grave.h"
#include "plat_host.h"

#define FIXED_DT (1.0f / 60.0f)

static App *g;
static const char *g_outdir = "out";
static int g_captures;
static int g_failures;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void ensure_dir(const char *path)
{
    char buf[512];
    size_t i;
    size_t n;

    if (path == NULL || path[0] == '\0')
        return;
    n = strlen(path);
    if (n >= sizeof(buf))
        n = sizeof(buf) - 1u;
    memcpy(buf, path, n);
    buf[n] = '\0';
    for (i = 1; i < n; ++i) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char saved = buf[i];
            buf[i] = '\0';
            (void)HOST_MKDIR(buf);
            buf[i] = saved;
        }
    }
    (void)HOST_MKDIR(buf);
}

static void fail(const char *msg)
{
    fprintf(stderr, "[fail] %s\n", msg);
    ++g_failures;
}

static void frame(void)
{
    app_poll_input();
    app_step();
    host_input_clear_edges();
}

static void settle(int frames)
{
    int i;
    for (i = 0; i < frames; ++i)
        frame();
}

/* Touchscreen tap (down for one frame, release the next) exactly like a real
 * finger: app_poll_input classifies short, non-dragging touches as UI taps. */
static void tap(int x, int y)
{
    host_touch_active = 1;
    host_touch_px = x;
    host_touch_py = y;
    host_keys_held |= KEY_TOUCH;
    host_keys_down |= KEY_TOUCH;
    frame();
    host_touch_active = 0;
    host_keys_held &= ~KEY_TOUCH;
    frame();
}

static void tap_rect(UiRect r)
{
    tap((int)(r.x + r.w * 0.5f), (int)(r.y + r.h * 0.5f));
}

static void expect_page(AppPage page, const char *what)
{
    if (g->page != page) {
        fprintf(stderr, "[fail] expected page %d after %s, got %d\n",
                (int)page, what, (int)g->page);
        ++g_failures;
    }
}

static void expect_tab(AppTab tab, const char *what)
{
    if (g->tab != tab) {
        fprintf(stderr, "[fail] expected tab %d after %s, got %d\n",
                (int)tab, what, (int)g->tab);
        ++g_failures;
    }
}

/* Capture the bottom screen draw list for the current app state. */
static void capture(const char *name)
{
    char path[512];
    int rc;

    host_drawlist_begin();
    app_draw_bottom();
    snprintf(path, sizeof(path), "%s/%s.txt", g_outdir, name);
    rc = host_drawlist_save(path);
    if (rc != 0) {
        fprintf(stderr, "[fail] could not write %s\n", path);
        ++g_failures;
        return;
    }
    printf("[capture] page=%-18s primitives=%4d text=%3d -> %s%s\n", name,
           host_c2d_draw_calls, host_c2d_text_calls, path,
           (host_text_overflow > 0) ? "  (TEXT TRUNCATED)" : "");
    if (host_text_overflow > 0)
        ++g_failures;
    ++g_captures;
}

static void boot(void)
{
    app_init();
    g = app_state();
    host_keys_held = 0;
    host_keys_down = 0;
    host_touch_active = 0;
    frame();
}

/* Enter the current zone from a no-save title screen (legacy prompt). */
static void start_zone_from_title(void)
{
    tap_rect(ui_title_start_rect());
    settle(4);
    expect_page(APP_PAGE_HUD, "tapping the title START prompt");
    if (!g->game.world.in_zone)
        fail("title START did not put the core back in a zone");
}

/* ------------------------------------------------------------------ */
/* deterministic page fixtures                                         */
/* ------------------------------------------------------------------ */

/* Light run-state fixture: player/strip/world-material values and a fixed set
 * of map blips, while leaving the core's zone and quest untouched (so per-zone
 * AREA maps keep their real zone layout and quest copy). */
static void rep_run_state(void)
{
    GraveGame *game = &g->game;
    int i;

    game->zone_time = 74.0f;
    game->zombies_killed = 27;
    game->zombies_alive = 5;
    game->material_coins = 8;
    game->material_sinew = 5;
    game->material_bone_charms = 3;
    game->total_kills = 47;
    game->total_time = 561.0f;
    game->zone_runs = 3;
    game->total_shots = 310;
    game->total_hits = 214;
    game->boss_alive = 0;
    game->boss_hp = 0.0f;
    game->boss_max_hp = 0.0f;
    game->boss_spawned = 0;

    game->player.max_hp = 100.0f;
    game->player.hp = 72.0f;
    game->player.stamina = 0.66f;
    game->player.alive = true;
    game->player.weapon = WEAPON_REVOLVER;
    game->player.weapon_owned[WEAPON_FLINTLOCK] = true;
    game->player.weapon_owned[WEAPON_REVOLVER] = true;
    game->player.weapon_owned[WEAPON_MUSKET] = true;
    game->player.ammo[WEAPON_FLINTLOCK] = 1;
    game->player.ammo[WEAPON_REVOLVER] = 4;
    game->player.ammo[WEAPON_MUSKET] = 1;
    game->player.reload_timer = 0.0f;
    game->player.reload_duration = 0.0f;
    game->player.fire_cooldown = 0.0f;
    game->player.camera_first_person = false;
    game->player.transform.pos = vec3_make(-4.0f, 0.0f, 6.0f);
    game->player.transform.yaw = 0.6f;
    game->player.transform.pitch = 0.0f;

    game->world.current = game->zone;
    game->world.in_zone = true;
    game->world.nodes[ZONE_ASHEN_MILL].unlocked = true;
    game->world.nodes[ZONE_ASHEN_MILL].cleared = true;
    game->world.nodes[ZONE_ASHEN_MILL].clears = 1;
    game->world.nodes[ZONE_ASHEN_MILL].best_time = 61.2f;
    game->world.nodes[ZONE_BONE_ORCHARD].unlocked = true;
    game->world.nodes[ZONE_BONE_ORCHARD].cleared = false;
    game->world.nodes[ZONE_BONE_ORCHARD].clears = 0;
    game->world.nodes[ZONE_BONE_ORCHARD].best_time = 0.0f;
    /* The late-run fixture has already reached the mine, so all nodes are
     * travelable (the per-zone tour relies on that). */
    game->world.nodes[ZONE_OLD_MINE].unlocked = true;
    game->world.nodes[ZONE_OLD_MINE].cleared = false;
    game->world.nodes[ZONE_OLD_MINE].clears = 0;
    game->world.nodes[ZONE_OLD_MINE].best_time = 0.0f;

    /* AREA-map fixture: five deterministic zombie blips. */
    memset(game->zombies, 0, sizeof(game->zombies));
    {
        static const struct { float x, z; int type; } blips[] = {
            { -8.0f, -6.0f, ZOMBIE_SHAMBLER },
            { 4.0f, -9.0f, ZOMBIE_RUNNER },
            { 12.0f, -4.0f, ZOMBIE_SHAMBLER },
            { -12.0f, 6.0f, ZOMBIE_BRUTE },
            { 9.0f, 10.0f, ZOMBIE_SHAMBLER }
        };
        int n = (int)(sizeof(blips) / sizeof(blips[0]));

        game->zombies_alive = n;
        for (i = 0; i < n; ++i) {
            Zombie *z = &game->zombies[i];

            z->active = true;
            z->type = (ZombieType)blips[i].type;
            z->state = ZOMBIE_CHASE;
            z->pos = vec3_make(blips[i].x, 0.0f, blips[i].z);
            z->max_hp = 40.0f;
            z->hp = 40.0f;
            z->speed = 1.0f;
            z->anim_time = (float)i * 0.7f;
        }
    }

    /* AREA-map fixture: a few ground pickups (loot dots). */
    memset(game->pickups, 0, sizeof(game->pickups));
    {
        static const struct { float x, z; int kind; } loot[] = {
            { -2.0f, -3.0f, DROP_COIN },
            { 6.0f, 1.0f, DROP_SINEW },
            { 11.0f, 8.0f, DROP_AMMO },
            { -9.0f, 11.0f, DROP_COIN }
        };
        int n = (int)(sizeof(loot) / sizeof(loot[0]));

        for (i = 0; i < n; ++i) {
            Pickup *p = &game->pickups[i];

            p->active = true;
            p->kind = (GraveDrop)loot[i].kind;
            p->pos = vec3_make(loot[i].x, 0.0f, loot[i].z);
            p->bob_time = (float)i * 0.9f;
            p->life = 30.0f;
        }
    }

    g->hit_flash = 0.0f;
    g->tab_anim = 0.0f;
    /* Clear transient notifications so page captures are reproducible. */
    g->toast[0] = '\0';
    g->toast_timer = 0.0f;
    g->wardrobe_flash = 0.0f;
    g->save_erase_armed = false;
}

/* Full Ashen Mill fixture used by the v0.2-era pages: also fixes the quest. */
static void rep_state(void)
{
    rep_run_state();
    g->game.quest.kind = QUEST_CLEAR_ZONE;
    g->game.quest.target = 40;
    g->game.quest.progress = 27;
}

/* ------------------------------------------------------------------ */
/* page tours                                                          */
/* ------------------------------------------------------------------ */

static void tour_title(void)
{
    boot();

    /* no save: legacy single "PRESS A OR TAP START" prompt */
    g->has_save = false;
    settle(24);
    expect_page(APP_PAGE_TITLE, "boot");
    capture("title");

    /* has save: CONTINUE / NEW GAME buttons */
    g->has_save = true;
    g->game.world.nodes[ZONE_ASHEN_MILL].cleared = true;
    g->game.world.nodes[ZONE_ASHEN_MILL].clears = 1;
    capture("title_save");

    tap_rect(ui_title_newgame_rect());
    settle(1);
    expect_page(APP_PAGE_TITLE_CONFIRM, "tapping NEW GAME");
    capture("title_confirm");

    tap_rect(ui_title_confirm_no_rect());
    settle(1);
    expect_page(APP_PAGE_TITLE, "cancelling the NEW GAME confirm");
}

static void tour_hud_pages(void)
{
    rep_state();
    capture("hud_area");

    tap_rect(ui_tab_rect(APP_TAB_WORLD));
    settle(12);
    rep_state();
    capture("hud_world");

    tap_rect(ui_tab_rect(APP_TAB_PACK));
    settle(12);
    rep_state();
    capture("hud_pack");

    /* mid-slide tab transition (PACK -> WORLD, half-way through the 0.15 s) */
    g->tab_prev = APP_TAB_PACK;
    g->tab = APP_TAB_WORLD;
    g->tab_anim = UI_TAB_TRANSITION_TIME * 0.45f;
    capture("hud_transition");
    g->tab = APP_TAB_PACK;
    g->tab_prev = APP_TAB_PACK;
    g->tab_anim = 0.0f;

    /* status-strip states: reloading + low health damage flash */
    rep_state();
    g->game.player.reload_timer = 1.10f;
    g->game.player.reload_duration = 2.00f;
    g->game.player.reload_weapon = WEAPON_REVOLVER;
    capture("hud_reload");

    rep_state();
    g->game.player.hp = 16.0f;
    g->hit_flash = 0.85f;
    g->prev_hp = 16.0f;
    capture("hud_low_hp");
    rep_state();
    g->hit_flash = 0.0f;
}

static void tour_pause_pages(void)
{
    rep_state();
    tap_rect(ui_pause_rect());
    settle(1);
    expect_page(APP_PAGE_PAUSE, "tapping the status-strip pause button");
    capture("pause");

    /* settings + save sub-page */
    tap_rect(ui_pause_settings_rect());
    settle(1);
    expect_page(APP_PAGE_SETTINGS, "pause -> settings");
    capture("settings");

    tap_rect(ui_settings_save_rect());
    settle(1);
    expect_page(APP_PAGE_SAVE, "settings -> save & progress");
    capture("save");

    tap_rect(ui_save_erase_rect());
    settle(1);
    expect_page(APP_PAGE_SAVE, "save erase armed");
    capture("save_armed");

    tap_rect(ui_save_back_rect());
    settle(1);
    expect_page(APP_PAGE_SETTINGS, "save -> settings");
    tap_rect(ui_settings_back_rect());
    settle(1);
    expect_page(APP_PAGE_PAUSE, "settings -> pause");

    /* wardrobe overlay + model-reload status line */
    tap_rect(ui_pause_wardrobe_rect());
    settle(1);
    expect_page(APP_PAGE_WARDROBE, "pause -> wardrobe");
    capture("wardrobe");

    tap_rect(ui_wardrobe_reload_rect());
    settle(2);
    expect_page(APP_PAGE_WARDROBE, "wardrobe reload");
    capture("wardrobe_reloaded");

    tap_rect(ui_wardrobe_back_rect());
    settle(1);
    expect_page(APP_PAGE_PAUSE, "wardrobe -> pause");
    tap_rect(ui_pause_resume_rect());
    settle(2);
    expect_page(APP_PAGE_HUD, "resume from pause");
}

/* Zone clear -> TRAVEL ON trail pages -> SHOP page + purchase toast. */
static void tour_trail_and_shop(void)
{
    GraveGame *game = &g->game;

    rep_state();
    game->zone_time = 131.4f;
    game->zombies_killed = 40;
    game->material_coins = 8;
    game->material_sinew = 4;
    game->quest.kind = QUEST_DONE;
    game->events |= GRAVE_EVT_ZONE_DONE;
    game->phase = GAME_PHASE_QUEST_COMPLETE;
    frame();
    expect_page(APP_PAGE_ZONE_CLEAR, "forcing quest completion");
    capture("zone_clear");

    tap_rect(ui_results_travel_rect());
    settle(4);
    expect_page(APP_PAGE_HUD, "TRAVEL ON");
    capture("world_trail");

    tap_rect(ui_tab_rect(APP_TAB_AREA));
    settle(12);
    capture("hud_trail_area");

    /* SHOP page (trailhead trader) with plenty of materials to spend */
    game->material_coins = 240;
    game->material_sinew = 120;
    game->material_bone_charms = 60;
    tap_rect(ui_tab_rect(APP_TAB_SHOP));
    settle(12);
    expect_tab(APP_TAB_SHOP, "SHOP tab");
    capture("shop");

    /* Buy the first affordable item so the purchase toast is on screen. */
    if (grave_shop_can_buy(game, (ShopItem)0)) {
        tap_rect(ui_shop_item_rect(0));
        settle(1);
        capture("shop_toast");
    } else {
        fail("SHOP item 0 is not purchasable with the fixture materials");
    }
}

/* Per-zone AREA maps, travel confirmation dialog, boss bar and campaign ending. */
static void tour_zones_boss_ending(void)
{
    GraveGame *game = &g->game;

    grave_world_unlock(game, ZONE_BONE_ORCHARD);
    grave_world_unlock(game, ZONE_OLD_MINE);

    /* WORLD tab -> tap the orchard node: confirmation dialog. */
    tap_rect(ui_tab_rect(APP_TAB_WORLD));
    settle(12);
    tap_rect(ui_world_node_rect(ZONE_BONE_ORCHARD));
    settle(1);
    if (g->travel_confirm_zone < 0)
        fail("tapping an unlocked world node did not open the travel confirm");
    capture("travel_confirm");

    tap_rect(ui_travel_yes_rect());
    settle(4);
    expect_page(APP_PAGE_HUD, "travel confirmation -> HUD");
    if (game->zone != ZONE_BONE_ORCHARD)
        fail("travel confirm did not enter BONE ORCHARD");
    rep_run_state();
    capture("hud_area_orchard");

    /* OLD MINE + live Foreman boss bar in the status strip. */
    tap_rect(ui_tab_rect(APP_TAB_WORLD));
    settle(12);
    tap_rect(ui_world_node_rect(ZONE_OLD_MINE));
    settle(1);
    tap_rect(ui_travel_yes_rect());
    settle(4);
    if (game->zone != ZONE_OLD_MINE)
        fail("travel confirm did not enter OLD MINE");
    rep_run_state();
    game->boss_spawned = 1;
    game->boss_alive = 1;
    game->boss_max_hp = 1200.0f;
    game->boss_hp = 780.0f;
    capture("hud_area_mine");

    /* Campaign ending: world complete -> VALLEY CLEARED results page. */
    game->world.nodes[ZONE_ASHEN_MILL].cleared = true;
    game->world.nodes[ZONE_ASHEN_MILL].clears = 3;
    game->world.nodes[ZONE_BONE_ORCHARD].cleared = true;
    game->world.nodes[ZONE_BONE_ORCHARD].clears = 2;
    game->world.nodes[ZONE_OLD_MINE].cleared = true;
    game->world.nodes[ZONE_OLD_MINE].clears = 1;
    game->world.nodes[ZONE_BONE_ORCHARD].best_time = 83.4f;
    game->world.nodes[ZONE_OLD_MINE].best_time = 121.7f;
    game->world_complete = true;
    game->quest.kind = QUEST_DONE;
    game->events |= GRAVE_EVT_ZONE_DONE;
    game->phase = GAME_PHASE_QUEST_COMPLETE;
    frame();
    expect_page(APP_PAGE_ZONE_CLEAR, "campaign ending");
    capture("ending");
}

static void tour_game_over(void)
{
    /* If we are still on a result page (e.g. right after the ending), go back
     * into a zone through its own buttons first. */
    if (g->page == APP_PAGE_ZONE_CLEAR) {
        if (g->game.world_complete)
            tap_rect(ui_results_play_rect());
        else
            tap_rect(ui_results_continue_rect());
        settle(4);
    }
    if (g->page == APP_PAGE_TITLE) {
        if (g->has_save)
            tap_rect(ui_title_continue_rect());
        else
            tap_rect(ui_title_start_rect());
        settle(4);
    }

    rep_state();
    /* Re-enter the zone when the last state left us on the trail. */
    if (!g->game.world.in_zone) {
        tap_rect(ui_tab_rect(APP_TAB_WORLD));
        settle(12);
        tap_rect(ui_world_node_rect(ZONE_ASHEN_MILL));
        settle(1);
        if (g->travel_confirm_zone >= 0)
            tap_rect(ui_travel_yes_rect());
        settle(4);
    }
    rep_state();
    g->game.player.hp = 0.0f;
    g->game.player.alive = false;
    frame();
    expect_page(APP_PAGE_GAME_OVER, "player death");
    capture("game_over");
}

/* ------------------------------------------------------------------ */
/* scenarios                                                           */
/* ------------------------------------------------------------------ */

static void run_title(void)
{
    tour_title();
}

static void run_hud(void)
{
    boot();
    start_zone_from_title();
    settle(90);
    tour_hud_pages();
}

static void run_pause(void)
{
    boot();
    start_zone_from_title();
    settle(30);
    tour_pause_pages();
}

static void run_trail(void)
{
    boot();
    start_zone_from_title();
    settle(30);
    tour_trail_and_shop();
}

static void run_zones(void)
{
    boot();
    start_zone_from_title();
    settle(30);
    tour_zones_boss_ending();
    tour_game_over();
}

static void run_gameover(void)
{
    boot();
    start_zone_from_title();
    settle(30);
    tour_game_over();
}

static void run_all(void)
{
    /* One continuous session: title variants -> continue -> HUD pages ->
     * pause/settings/save/wardrobe -> zone clear -> trail + shop -> per-zone
     * maps + travel dialog + boss bar + ending -> game over. */
    tour_title();

    tap_rect(ui_title_continue_rect());
    settle(4);
    expect_page(APP_PAGE_HUD, "CONTINUE from the has-save title");
    settle(90);

    tour_hud_pages();
    tour_pause_pages();
    tour_trail_and_shop();
    tour_zones_boss_ending();
    tour_game_over();
}

/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
    printf("Gravepowder - host UI preview harness (bottom screen only)\n"
           "  Drives the real app/UI layer against the real portable core and\n"
           "  records the citro2d draw lists for tools/preview/render_ui.py.\n"
           "\n"
           "Usage: %s [--scenario all|title|hud|pause|trail|zones|gameover]"
           " [--out DIR]\n"
           "\n"
           "  --scenario all       full page tour (default)\n"
           "  --scenario title     title: no-save prompt, has-save buttons, NEW GAME confirm\n"
           "  --scenario hud       AREA/WORLD/PACK pages + tab slide + strip states\n"
           "  --scenario pause     pause/settings/save/wardrobe overlays\n"
           "  --scenario trail     zone-clear, TRAVEL ON trail pages, SHOP + toast\n"
           "  --scenario zones     travel confirm, per-zone AREA maps, boss bar, ending\n"
           "  --scenario gameover  death -> game-over page\n"
           "  --out DIR            draw-list output directory (default: out)\n",
           argv0);
}

int main(int argc, char **argv)
{
    const char *scenario = "all";
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            scenario = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            g_outdir = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option '%s' (try --help)\n", argv[i]);
            return 2;
        }
    }

    ensure_dir(g_outdir);

    printf("Gravepowder UI preview harness (host shim; not the 3DS build)\n");
    printf("  scenario='%s' out='%s'\n", scenario, g_outdir);

    if (strcmp(scenario, "title") == 0) {
        run_title();
    } else if (strcmp(scenario, "hud") == 0) {
        run_hud();
    } else if (strcmp(scenario, "pause") == 0) {
        run_pause();
    } else if (strcmp(scenario, "trail") == 0) {
        run_trail();
    } else if (strcmp(scenario, "zones") == 0) {
        run_zones();
    } else if (strcmp(scenario, "gameover") == 0) {
        run_gameover();
    } else if (strcmp(scenario, "all") == 0) {
        run_all();
    } else {
        fprintf(stderr, "unknown scenario '%s' (try --help)\n", scenario);
        return 2;
    }

    printf("\npreview: %d capture(s), %d failure(s)\n", g_captures, g_failures);
    return (g_failures == 0 && g_captures > 0) ? 0 : 1;
}
