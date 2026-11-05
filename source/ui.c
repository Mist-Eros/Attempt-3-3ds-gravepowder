/*
 * Gravepowder - bottom screen UI (citro2d)
 *
 * The bottom screen is a paged info display:
 *   - a slim always-on status strip (health / weapon+ammo / pause)
 *   - a three-tab bar (AREA / WORLD / PACK)
 *   - one full page below it, with a 0.15 s slide transition
 * Pause, settings and the result pages overlay the whole screen.
 *
 * Everything is drawn with citro2d primitives (no textures): weathered paper,
 * brass trim, ink lines and blood-red accents.  The UI never mutates game
 * state beyond the app-owned tab; it returns UI_ACTION_* ids to app.c.
 */
#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "ui.h"

#include "render3d.h"

/* ------------------------------------------------------------------ */
/* palette: weathered paper / bone / brass / ink / blood              */
/* ------------------------------------------------------------------ */

#define CLR(r, g, b, a)                                                       \
    ((u32)(((u32)(r) & 0xFF) | (((u32)(g) & 0xFF) << 8) |                     \
           (((u32)(b) & 0xFF) << 16) | (((u32)(a) & 0xFF) << 24)))

static const u32 COL_BG        = CLR(0x16, 0x11, 0x0D, 0xFF);
static const u32 COL_BG2       = CLR(0x22, 0x1A, 0x12, 0xFF);
static const u32 COL_GRAIN     = CLR(0x0E, 0x0A, 0x07, 0xFF);
static const u32 COL_STAIN     = CLR(0x00, 0x00, 0x00, 0x40);

static const u32 COL_PAPER     = CLR(0xE3, 0xD2, 0xA8, 0xFF);
static const u32 COL_PAPER_HI  = CLR(0xF3, 0xE7, 0xC8, 0xFF);
static const u32 COL_PAPER_LO  = CLR(0xC2, 0xAB, 0x78, 0xFF);
static const u32 COL_INK       = CLR(0x2B, 0x1B, 0x10, 0xFF);
static const u32 COL_INK_SOFT  = CLR(0x4E, 0x39, 0x24, 0xFF);
static const u32 COL_BRASS     = CLR(0xA8, 0x82, 0x3A, 0xFF);
static const u32 COL_BRASS_HI  = CLR(0xE6, 0xC3, 0x6A, 0xFF);
static const u32 COL_BONE      = CLR(0xF4, 0xEA, 0xD0, 0xFF);
static const u32 COL_BONE_DIM  = CLR(0xC9, 0xBE, 0xA2, 0xFF);
static const u32 COL_BLOOD     = CLR(0x8E, 0x23, 0x18, 0xFF);
static const u32 COL_BLOOD_HI  = CLR(0xD6, 0x49, 0x2F, 0xFF);
static const u32 COL_ROT       = CLR(0x7C, 0x8C, 0x44, 0xFF);
static const u32 COL_STAM      = CLR(0x8A, 0x94, 0x52, 0xFF);
static const u32 COL_HILITE    = CLR(0xF7, 0xD9, 0x8A, 0xFF);
static const u32 COL_MAP_BG    = CLR(0x2A, 0x21, 0x16, 0xFF);
static const u32 COL_MAP_GRID  = CLR(0x3A, 0x2F, 0x20, 0xFF);

/* ------------------------------------------------------------------ */
/* layout constants                                                   */
/* ------------------------------------------------------------------ */

#define STRIP_H     33.0f
#define TAB_Y       36.0f
#define TAB_H       22.0f
#define TAB_W       76.0f
#define TAB_X0      6.0f
#define TAB_GAP     2.0f
#define CONTENT_Y   62.0f
#define CONTENT_BOT 236.0f

/* AREA page: a square top-down map scaled by grave_zone_half_extent(). */
#define AREA_MAP_X    104.0f
#define AREA_MAP_Y    64.0f
#define AREA_MAP_SIZE 112.0f

/* ------------------------------------------------------------------ */
/* depth / transition state                                           */
/* ------------------------------------------------------------------ */

/* Per-page depth layer: 0.5 = normal content, 1.0 = full-screen overlays. */
static float g_z = 0.5f;
/* Horizontal transition offset applied to every primitive (0 = none). */
static float g_dx = 0.0f;

/* ------------------------------------------------------------------ */
/* primitive helpers (all honour g_dx / g_z)                          */
/* ------------------------------------------------------------------ */

static void fill_rect(float x, float y, float w, float h, u32 clr)
{
    C2D_DrawRectSolid(x + g_dx, y, g_z, w, h, clr);
}

static void frame_rect(float x, float y, float w, float h, float t, u32 clr)
{
    fill_rect(x, y, w, t, clr);
    fill_rect(x, y + h - t, w, t, clr);
    fill_rect(x, y, t, h, clr);
    fill_rect(x + w - t, y, t, h, clr);
}

static void circle(float x, float y, float r, u32 clr)
{
    C2D_DrawCircleSolid(x + g_dx, y, g_z, r, clr);
}

static void line(float x0, float y0, float x1, float y1, float t, u32 clr)
{
    C2D_DrawLine(x0 + g_dx, y0, clr, x1 + g_dx, y1, clr, t, g_z);
}

static void tri(float x0, float y0, float x1, float y1, float x2, float y2,
                u32 clr)
{
    C2D_DrawTriangle(x0 + g_dx, y0, clr, x1 + g_dx, y1, clr, x2 + g_dx, y2, clr,
                     g_z);
}

static void text_left(const App *app, float x, float y, float scale, u32 clr,
                      const char *s)
{
    C2D_Text t;

    C2D_TextParse(&t, app->text_buf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x + g_dx, y, g_z, scale, scale, clr);
}

static void text_center(const App *app, float cx, float y, float scale, u32 clr,
                        const char *s)
{
    C2D_Text t;

    C2D_TextParse(&t, app->text_buf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_AlignCenter | C2D_WithColor, cx + g_dx, y, g_z, scale,
                 scale, clr);
}

static void text_right(const App *app, float rightX, float y, float scale,
                       u32 clr, const char *s)
{
    C2D_Text t;

    C2D_TextParse(&t, app->text_buf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_AlignRight | C2D_WithColor, rightX + g_dx, y, g_z,
                 scale, scale, clr);
}

static void text_shadow_center(const App *app, float cx, float y, float scale,
                               u32 fill, const char *s)
{
    text_center(app, cx + 1.5f, y + 1.5f, scale, COL_INK, s);
    text_center(app, cx, y, scale, fill, s);
}

static void text_woodcut_center(const App *app, float cx, float y, float scale,
                                u32 fill, const char *s)
{
    text_center(app, cx + 3.0f, y + 3.0f, scale, COL_INK, s);
    text_center(app, cx - 2.0f, y, scale, COL_INK, s);
    text_center(app, cx + 2.0f, y, scale, COL_INK, s);
    text_center(app, cx, y - 2.0f, scale, COL_INK, s);
    text_center(app, cx, y + 2.0f, scale, COL_INK, s);
    text_center(app, cx, y, scale, fill, s);
}

/* Parchment panel with an ink frame, a brass hairline and corner studs. */
static void paper_panel(float x, float y, float w, float h)
{
    fill_rect(x + 3.0f, y + 3.0f, w, h, COL_STAIN);
    fill_rect(x, y, w, h, COL_INK);
    fill_rect(x + 2.0f, y + 2.0f, w - 4.0f, h - 4.0f, COL_BRASS);
    fill_rect(x + 3.0f, y + 3.0f, w - 6.0f, h - 6.0f, COL_PAPER);
    fill_rect(x + 3.0f, y + 3.0f, w - 6.0f, 1.0f, COL_PAPER_HI);
    fill_rect(x + 3.0f, y + h - 4.0f, w - 6.0f, 1.0f, COL_PAPER_LO);
    fill_rect(x + 4.0f, y + 4.0f, 3.0f, 3.0f, COL_BRASS_HI);
    fill_rect(x + w - 7.0f, y + 4.0f, 3.0f, 3.0f, COL_BRASS_HI);
    fill_rect(x + 4.0f, y + h - 7.0f, 3.0f, 3.0f, COL_BRASS_HI);
    fill_rect(x + w - 7.0f, y + h - 7.0f, 3.0f, 3.0f, COL_BRASS_HI);
}

/* Dark timber panel (instrument housings, inactive tabs). */
static void dark_panel(float x, float y, float w, float h)
{
    fill_rect(x + 2.0f, y + 2.0f, w, h, COL_STAIN);
    fill_rect(x, y, w, h, COL_INK);
    fill_rect(x + 2.0f, y + 2.0f, w - 4.0f, h - 4.0f, COL_BG2);
    fill_rect(x + 2.0f, y + 2.0f, w - 4.0f, 1.0f, CLR(0x3A, 0x2C, 0x1C, 0xFF));
    fill_rect(x + 2.0f, y + h - 3.0f, w - 4.0f, 1.0f, COL_GRAIN);
    fill_rect(x + 3.0f, y + 3.0f, 2.0f, 2.0f, COL_BRASS);
    fill_rect(x + w - 5.0f, y + 3.0f, 2.0f, 2.0f, COL_BRASS);
}

static void divider(float x, float y, float w)
{
    fill_rect(x, y, w, 1.0f, COL_BRASS);
    fill_rect(x, y - 1.0f, 3.0f, 3.0f, COL_BRASS_HI);
    fill_rect(x + w - 3.0f, y - 1.0f, 3.0f, 3.0f, COL_BRASS_HI);
}

static void paper_button(float x, float y, float w, float h, u32 accent)
{
    paper_panel(x, y, w, h);
    fill_rect(x + 5.0f, y + 5.0f, 4.0f, h - 10.0f, accent);
    fill_rect(x + 5.0f, y + 5.0f, w - 10.0f, 1.0f,
              CLR(0xFF, 0xFF, 0xFF, 0x30));
}

static void bar(float x, float y, float w, float h, float frac, u32 fill,
                u32 back)
{
    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;
    fill_rect(x, y, w, h, COL_INK);
    fill_rect(x + 1.0f, y + 1.0f, w - 2.0f, h - 2.0f, back);
    if (frac > 0.0f)
        fill_rect(x + 1.0f, y + 1.0f, (w - 2.0f) * frac, h - 2.0f, fill);
    fill_rect(x + 1.0f, y + 1.0f, w - 2.0f, 1.0f,
              CLR(0xFF, 0xFF, 0xFF, 0x18));
}

/* Weathered wooden background shared by every page. */
static void draw_background(const App *app)
{
    int i;
    float save_dx = g_dx;

    g_dx = 0.0f;
    fill_rect(0.0f, 0.0f, 320.0f, 240.0f, COL_BG);
    for (i = 0; i < 320; i += 16) {
        fill_rect((float)i, 0.0f, 1.0f, 240.0f, COL_GRAIN);
        fill_rect((float)i + 8.0f, 0.0f, 1.0f, 240.0f,
                  CLR(0x2A, 0x20, 0x16, 0x60));
    }
    circle(40.0f, 34.0f, 22.0f, COL_STAIN);
    circle(282.0f, 96.0f, 16.0f, COL_STAIN);
    circle(238.0f, 196.0f, 30.0f, COL_STAIN);
    for (i = 0; i < 5; i++) {
        u32 a = CLR(0, 0, 0, 40 - (u32)i * 7);
        float t = 3.0f;
        float o = (float)i * t;
        fill_rect(0.0f, o, 320.0f, t, a);
        fill_rect(0.0f, 240.0f - o - t, 320.0f, t, a);
        fill_rect(o, 0.0f, t, 240.0f, a);
        fill_rect(320.0f - o - t, 0.0f, t, 240.0f, a);
    }
    g_dx = save_dx;
    (void)app;
}

/* ------------------------------------------------------------------ */
/* layout accessors                                                   */
/* ------------------------------------------------------------------ */

UiRect ui_pause_rect(void)
{
    UiRect r = { 264.0f, 5.0f, 50.0f, 24.0f };
    return r;
}

UiRect ui_tab_rect(int tab)
{
    UiRect r;

    if (tab < 0)
        tab = 0;
    if (tab >= APP_TAB_COUNT)
        tab = APP_TAB_COUNT - 1;
    r.x = TAB_X0 + (float)tab * (TAB_W + TAB_GAP);
    r.y = TAB_Y;
    r.w = TAB_W;
    r.h = TAB_H;
    return r;
}

UiRect ui_area_map_rect(void)
{
    UiRect r = { AREA_MAP_X, AREA_MAP_Y, AREA_MAP_SIZE, AREA_MAP_SIZE };
    return r;
}

/* SHOP page: two columns of item cards (ShopItem order). */
UiRect ui_shop_item_rect(int item)
{
    UiRect r;

    if (item < 0)
        item = 0;
    if (item >= SHOP_ITEM_COUNT)
        item = SHOP_ITEM_COUNT - 1;
    r.x = 8.0f + (float)(item % 2) * 156.0f;
    r.y = 84.0f + (float)(item / 2) * 37.0f;
    r.w = 148.0f;
    r.h = 35.0f;
    return r;
}

UiRect ui_title_continue_rect(void)
{
    UiRect r = { 56.0f, 124.0f, 208.0f, 28.0f };
    return r;
}

UiRect ui_title_newgame_rect(void)
{
    UiRect r = { 56.0f, 158.0f, 208.0f, 24.0f };
    return r;
}

UiRect ui_title_confirm_yes_rect(void)
{
    UiRect r = { 56.0f, 148.0f, 100.0f, 26.0f };
    return r;
}

UiRect ui_title_confirm_no_rect(void)
{
    UiRect r = { 164.0f, 148.0f, 100.0f, 26.0f };
    return r;
}

UiRect ui_pause_wardrobe_rect(void)
{
    UiRect r = { 56.0f, 146.0f, 208.0f, 24.0f };
    return r;
}

UiRect ui_settings_save_rect(void)
{
    UiRect r = { 48.0f, 156.0f, 224.0f, 24.0f };
    return r;
}

UiRect ui_save_erase_rect(void)
{
    UiRect r = { 48.0f, 110.0f, 224.0f, 26.0f };
    return r;
}

UiRect ui_save_back_rect(void)
{
    UiRect r = { 96.0f, 190.0f, 128.0f, 24.0f };
    return r;
}

UiRect ui_wardrobe_reload_rect(void)
{
    UiRect r = { 60.0f, 152.0f, 200.0f, 22.0f };
    return r;
}

UiRect ui_wardrobe_back_rect(void)
{
    UiRect r = { 96.0f, 194.0f, 128.0f, 22.0f };
    return r;
}

UiRect ui_travel_yes_rect(void)
{
    UiRect r = { 56.0f, 136.0f, 100.0f, 24.0f };
    return r;
}

UiRect ui_travel_no_rect(void)
{
    UiRect r = { 164.0f, 136.0f, 100.0f, 24.0f };
    return r;
}

UiRect ui_results_play_rect(void)
{
    UiRect r = { 56.0f, 146.0f, 208.0f, 30.0f };
    return r;
}

UiRect ui_world_node_rect(int node)
{
    if (node == 1) {
        UiRect b = { 178.0f, 78.0f, 118.0f, 42.0f };
        return b;
    }
    if (node == 2) {
        UiRect o = { 178.0f, 134.0f, 118.0f, 42.0f };
        return o;
    }
    {
        UiRect m = { 40.0f, 100.0f, 118.0f, 42.0f };
        return m;
    }
}

UiRect ui_pack_weapon_rect(int index)
{
    UiRect r;

    if (index < 0)
        index = 0;
    if (index >= WEAPON_COUNT)
        index = WEAPON_COUNT - 1;
    r.x = 6.0f;
    r.y = 62.0f + (float)index * 46.0f;
    r.w = 170.0f;
    r.h = 42.0f;
    return r;
}

UiRect ui_title_start_rect(void)
{
    UiRect r = { 62.0f, 166.0f, 196.0f, 30.0f };
    return r;
}

UiRect ui_pause_resume_rect(void)
{
    UiRect r = { 56.0f, 62.0f, 208.0f, 24.0f };
    return r;
}

UiRect ui_pause_restart_rect(void)
{
    UiRect r = { 56.0f, 90.0f, 208.0f, 24.0f };
    return r;
}

UiRect ui_pause_settings_rect(void)
{
    UiRect r = { 56.0f, 118.0f, 208.0f, 24.0f };
    return r;
}

UiRect ui_pause_quit_rect(void)
{
    UiRect r = { 56.0f, 174.0f, 208.0f, 24.0f };
    return r;
}

UiRect ui_settings_back_rect(void)
{
    UiRect r = { 76.0f, 186.0f, 168.0f, 24.0f };
    return r;
}

UiRect ui_settings_camera_rect(void)
{
    UiRect r = { 48.0f, 52.0f, 224.0f, 24.0f };
    return r;
}

UiRect ui_settings_sens_rect(void)
{
    UiRect r = { 48.0f, 78.0f, 224.0f, 24.0f };
    return r;
}

UiRect ui_settings_invert_rect(void)
{
    UiRect r = { 48.0f, 104.0f, 224.0f, 24.0f };
    return r;
}

UiRect ui_settings_mute_rect(void)
{
    UiRect r = { 48.0f, 130.0f, 224.0f, 24.0f };
    return r;
}

UiRect ui_results_continue_rect(void)
{
    UiRect r = { 76.0f, 186.0f, 168.0f, 24.0f };
    return r;
}

UiRect ui_results_travel_rect(void)
{
    UiRect r = { 76.0f, 146.0f, 168.0f, 32.0f };
    return r;
}

UiRect ui_gameover_retry_rect(void)
{
    UiRect r = { 76.0f, 160.0f, 168.0f, 32.0f };
    return r;
}

UiRect ui_weapon_slot_rect(int index)
{
    return ui_pack_weapon_rect(index);
}

static bool rect_hit(UiRect r, int px, int py)
{
    float fx = (float)px;
    float fy = (float)py;

    return fx >= r.x && fx < r.x + r.w && fy >= r.y && fy < r.y + r.h;
}

/* ------------------------------------------------------------------ */
/* status strip                                                       */
/* ------------------------------------------------------------------ */

static void draw_heart(float x, float y, float r, u32 clr)
{
    circle(x - r * 0.45f, y - r * 0.30f, r * 0.62f, clr);
    circle(x + r * 0.45f, y - r * 0.30f, r * 0.62f, clr);
    tri(x - r * 1.02f, y - r * 0.10f, x + r * 1.02f, y - r * 0.10f, x,
        y + r * 1.05f, clr);
}

static void draw_strip(const App *app)
{
    const GraveGame *g = &app->game;
    const Player *p = &g->player;
    const WeaponSpec *spec = grave_weapon_spec(p->weapon);
    float hp_frac = p->max_hp > 0.0f ? p->hp / p->max_hp : 0.0f;
    float stam_frac = p->stamina > 1.5f ? p->stamina / 100.0f : p->stamina;
    char buf[32];
    float save_dx = g_dx;
    int i;

    g_dx = 0.0f;

    /* housing */
    fill_rect(0.0f, 0.0f, 320.0f, STRIP_H, COL_BG2);
    for (i = 0; i < 320; i += 16)
        fill_rect((float)i, 0.0f, 1.0f, STRIP_H, COL_GRAIN);
    fill_rect(0.0f, STRIP_H - 2.0f, 320.0f, 1.0f,
              CLR(0x12, 0x10, 0x06, 0xEE));
    fill_rect(0.0f, STRIP_H - 1.0f, 320.0f, 2.0f, COL_BRASS);

    /* health + stamina */
    draw_heart(14.0f, 11.5f, 5.2f, COL_BLOOD_HI);
    bar(23.0f, 4.0f, 103.0f, 14.0f, hp_frac, COL_BLOOD_HI,
        CLR(0x3A, 0x20, 0x16, 0xFF));
    snprintf(buf, sizeof(buf), "%d / %d", (int)(p->hp + 0.5f),
             (int)(p->max_hp + 0.5f));
    text_center(app, 74.5f, 5.5f, 0.42f, COL_INK, buf);
    text_center(app, 74.0f, 5.0f, 0.42f, COL_BONE, buf);
    bar(23.0f, 20.0f, 103.0f, 7.0f, stam_frac, CLR(0xBC, 0xC6, 0x7A, 0xFF),
        CLR(0x2A, 0x25, 0x16, 0xFF));

    /* weapon + ammo */
    if (spec != NULL) {
        int ammo = p->ammo[p->weapon];
        int mag = spec->magazine > 0 ? spec->magazine : 1;

        text_left(app, 134.0f, 0.0f, 0.36f, COL_BONE_DIM, spec->name);
        snprintf(buf, sizeof(buf), "%d / %d", ammo, mag);
        text_left(app, 134.0f, 12.0f, 0.58f,
                  ammo > 0 ? COL_BRASS_HI : COL_BLOOD_HI, buf);
        if (p->reload_timer > 0.0f && p->reload_duration > 0.0f) {
            float frac = 1.0f - p->reload_timer / p->reload_duration;

            bar(134.0f, 27.0f, 110.0f, 4.0f, frac, COL_BRASS_HI,
                CLR(0x3C, 0x33, 0x20, 0xFF));
        } else if (ammo <= 0) {
            text_left(app, 196.0f, 12.0f, 0.34f, COL_BLOOD_HI, "RELOAD");
        }
    }

    /* pause button */
    {
        UiRect pr = ui_pause_rect();

        paper_button(pr.x, pr.y, pr.w, pr.h, COL_BRASS_HI);
        fill_rect(pr.x + 7.0f, pr.y + 7.0f, 4.0f, 10.0f, COL_INK);
        fill_rect(pr.x + 13.0f, pr.y + 7.0f, 4.0f, 10.0f, COL_INK);
        text_left(app, pr.x + 20.0f, pr.y + 7.0f, 0.28f, COL_INK, "PAUSE");
    }

    g_dx = save_dx;
}

/* ------------------------------------------------------------------ */
/* tab bar                                                            */
/* ------------------------------------------------------------------ */

static void draw_tab_bar(const App *app)
{
    static const char *const kTabNames[APP_TAB_COUNT] = {
        "AREA", "WORLD", "PACK", "SHOP"
    };
    float save_dx = g_dx;
    int i;

    g_dx = 0.0f;
    fill_rect(0.0f, TAB_Y - 2.0f, 320.0f, TAB_H + 4.0f, COL_INK);
    for (i = 0; i < APP_TAB_COUNT; i++) {
        UiRect r = ui_tab_rect(i);
        bool active = ((int)app->tab == i);

        if (active) {
            fill_rect(r.x, r.y, r.w, r.h, COL_PAPER);
            fill_rect(r.x + 2.0f, r.y + 2.0f, r.w - 4.0f, 1.0f, COL_PAPER_HI);
            fill_rect(r.x + 8.0f, r.y + r.h - 2.0f, r.w - 16.0f, 2.0f,
                      COL_BLOOD_HI);
            text_center(app, r.x + r.w * 0.5f, r.y + 5.0f, 0.42f, COL_INK,
                        kTabNames[i]);
        } else {
            fill_rect(r.x, r.y, r.w, r.h, COL_BG2);
            frame_rect(r.x, r.y, r.w, r.h, 1.0f, CLR(0x3A, 0x2C, 0x1C, 0xFF));
            text_center(app, r.x + r.w * 0.5f, r.y + 5.0f, 0.42f,
                        COL_BONE_DIM, kTabNames[i]);
        }
    }
    g_dx = save_dx;
}

/* ------------------------------------------------------------------ */
/* AREA page: top-down map of the current zone                        */
/* ------------------------------------------------------------------ */

/* Arena half extent for the current zone (core call), clamped so a bad value
 * can never divide by zero. */
static float area_half_extent(const GraveGame *g)
{
    float half = grave_zone_half_extent(g->zone);

    if (half < 4.0f)
        half = 4.0f;
    return half;
}

static void map_point(const GraveGame *g, float wx, float wz, float *mx,
                      float *my)
{
    float half = area_half_extent(g);

    *mx = AREA_MAP_X + (wx + half) / (2.0f * half) * AREA_MAP_SIZE;
    *my = AREA_MAP_Y + (wz + half) / (2.0f * half) * AREA_MAP_SIZE;
}

/* Top-down zone map drawn from the live prop data, scaled by the zone's arena
 * half extent.  Works for every zone as long as the core authors props: fence
 * walls, gate posts, the mill well, crates/barrels/stones/trees and the new
 * OLD MINE timber/cart/ore/lantern props. */
static void draw_area_map(const App *app)
{
    const GraveGame *g = &app->game;
    float half = area_half_extent(g);
    float prop_scale = AREA_MAP_SIZE / (2.0f * half);
    const Prop *gate = NULL;
    int i;

    /* map plate */
    fill_rect(AREA_MAP_X - 3.0f, AREA_MAP_Y - 3.0f, AREA_MAP_SIZE + 6.0f,
              AREA_MAP_SIZE + 6.0f, COL_INK);
    fill_rect(AREA_MAP_X - 1.0f, AREA_MAP_Y - 1.0f, AREA_MAP_SIZE + 2.0f,
              AREA_MAP_SIZE + 2.0f, COL_BRASS);
    fill_rect(AREA_MAP_X, AREA_MAP_Y, AREA_MAP_SIZE, AREA_MAP_SIZE, COL_MAP_BG);

    /* grid every 1/6 of the arena so distances read */
    for (i = 0; i <= 6; i++) {
        float tt = (float)i / 6.0f;

        fill_rect(AREA_MAP_X + tt * AREA_MAP_SIZE, AREA_MAP_Y, 1.0f,
                  AREA_MAP_SIZE, COL_MAP_GRID);
        fill_rect(AREA_MAP_X, AREA_MAP_Y + tt * AREA_MAP_SIZE, AREA_MAP_SIZE,
                  1.0f, COL_MAP_GRID);
    }

    for (i = 0; i < g->prop_count; i++) {
        const Prop *p = &g->props[i];
        float mx, my;

        map_point(g, p->pos.x, p->pos.z, &mx, &my);
        switch (p->kind) {
        case PROP_FENCE: {
            float w = p->half.x * 2.0f * prop_scale;
            float h = p->half.z * 2.0f * prop_scale;

            if (w < 2.0f)
                w = 2.0f;
            if (h < 2.0f)
                h = 2.0f;
            fill_rect(mx - w * 0.5f, my - h * 0.5f, w, h,
                      CLR(0x8A, 0x76, 0x52, 0xFF));
            break;
        }
        case PROP_GATE:
            fill_rect(mx - 2.0f, my - 2.0f, 4.0f, 4.0f, COL_BRASS_HI);
            gate = p;
            break;
        case PROP_WELL:
            circle(mx, my, 4.5f, COL_INK);
            circle(mx, my, 3.4f, CLR(0x4E, 0x44, 0x30, 0xFF));
            break;
        case PROP_CRATE:
            fill_rect(mx - 2.5f, my - 2.5f, 5.0f, 5.0f,
                      CLR(0x9A, 0x72, 0x42, 0xFF));
            fill_rect(mx - 2.5f, my - 2.5f, 5.0f, 1.0f,
                      CLR(0xC0, 0x96, 0x5C, 0xFF));
            break;
        case PROP_BARREL:
            circle(mx, my, 2.2f, CLR(0x86, 0x62, 0x3A, 0xFF));
            break;
        case PROP_TOMBSTONE:
            fill_rect(mx - 1.5f, my - 2.5f, 3.0f, 5.0f,
                      CLR(0xB8, 0xB0, 0x9C, 0xFF));
            break;
        case PROP_DEADTREE:
            fill_rect(mx - 1.0f, my - 1.0f, 2.0f, 2.0f,
                      CLR(0x53, 0x3F, 0x2A, 0xFF));
            fill_rect(mx - 3.0f, my - 1.0f, 6.0f, 1.0f,
                      CLR(0x6E, 0x5A, 0x3C, 0xFF));
            break;
        case PROP_TIMBER:
            fill_rect(mx - 4.0f, my - 1.0f, 8.0f, 2.0f,
                      CLR(0x8A, 0x62, 0x38, 0xFF));
            fill_rect(mx - 1.0f, my - 4.0f, 2.0f, 8.0f,
                      CLR(0x6E, 0x4C, 0x2A, 0xFF));
            break;
        case PROP_CART:
            fill_rect(mx - 4.0f, my - 3.0f, 8.0f, 5.0f,
                      CLR(0x5A, 0x46, 0x30, 0xFF));
            circle(mx - 2.6f, my + 3.0f, 1.3f, COL_INK);
            circle(mx + 2.6f, my + 3.0f, 1.3f, COL_INK);
            break;
        case PROP_ORE:
            circle(mx, my, 3.4f, CLR(0x2E, 0x4A, 0x44, 0xFF));
            circle(mx - 1.0f, my - 1.0f, 1.3f, CLR(0x8C, 0xE0, 0xC8, 0xFF));
            circle(mx + 1.2f, my + 0.8f, 1.0f, COL_BRASS_HI);
            break;
        case PROP_LANTERN:
            circle(mx, my, 3.0f, CLR(0x6A, 0x4A, 0x18, 0x80));
            circle(mx, my, 1.6f, COL_HILITE);
            break;
        default:
            break;
        }
    }

    /* gate / objective marker: a gold flag at the zone gate */
    if (gate != NULL) {
        float gx, gy;

        map_point(g, gate->pos.x, gate->pos.z, &gx, &gy);
        line(gx, gy - 9.0f, gx, gy + 1.0f, 1.0f, COL_INK);
        tri(gx, gy - 9.0f, gx + 7.0f, gy - 6.0f, gx, gy - 3.0f, COL_BRASS_HI);
    }

    /* pickups: loot dots */
    for (i = 0; i < MAX_PICKUPS; i++) {
        const Pickup *pk = &g->pickups[i];
        float mx, my;

        if (!pk->active)
            continue;
        map_point(g, pk->pos.x, pk->pos.z, &mx, &my);
        circle(mx, my, 1.6f, COL_HILITE);
    }

    /* zombies: live blips (the Foreman gets a bigger brass blip) */
    for (i = 0; i < MAX_ZOMBIES; i++) {
        const Zombie *z = &g->zombies[i];
        float mx, my;
        float r;
        u32 clr;

        if (!z->active || z->state == ZOMBIE_DEAD)
            continue;
        map_point(g, z->pos.x, z->pos.z, &mx, &my);
        if (z->type == ZOMBIE_RUNNER) {
            clr = COL_BLOOD_HI;
            r = 2.0f;
        } else if (z->type == ZOMBIE_BRUTE) {
            clr = COL_BRASS_HI;
            r = 3.2f;
        } else if (z->type == ZOMBIE_FOREMAN) {
            clr = COL_BLOOD_HI;
            r = 4.4f;
        } else {
            clr = COL_ROT;
            r = 2.0f;
        }
        circle(mx, my, r + 1.0f, COL_INK);
        circle(mx, my, r, clr);
    }

    /* player arrow (orientation on screen: +x right, +z down) */
    {
        float px, py;
        float fx = sinf(g->player.transform.yaw);
        float fz = cosf(g->player.transform.yaw);
        float nx = -fz;
        float nz = fx;

        map_point(g, g->player.transform.pos.x, g->player.transform.pos.z, &px,
                  &py);
        tri(px + fx * 8.0f, py + fz * 8.0f, px - fx * 4.0f + nx * 5.0f,
            py - fz * 4.0f + nz * 5.0f, px - fx * 4.0f - nx * 5.0f,
            py - fz * 4.0f - nz * 5.0f, COL_BRASS_HI);
        tri(px + fx * 5.0f, py + fz * 5.0f, px - fx * 2.5f + nx * 3.0f,
            py - fz * 2.5f + nz * 3.0f, px - fx * 2.5f - nx * 3.0f,
            py - fz * 2.5f - nz * 3.0f, COL_HILITE);
    }

    /* compass + frame */
    text_left(app, AREA_MAP_X + 4.0f, AREA_MAP_Y + 2.0f, 0.3f, COL_BONE_DIM,
              "N");
    tri(AREA_MAP_X + AREA_MAP_SIZE - 8.0f, AREA_MAP_Y + 14.0f,
        AREA_MAP_X + AREA_MAP_SIZE - 4.0f, AREA_MAP_Y + 5.0f,
        AREA_MAP_X + AREA_MAP_SIZE, AREA_MAP_Y + 14.0f, COL_BRASS_HI);
    frame_rect(AREA_MAP_X - 1.0f, AREA_MAP_Y - 1.0f, AREA_MAP_SIZE + 2.0f,
               AREA_MAP_SIZE + 2.0f, 1.0f, COL_BRASS);
}
/* Shown on the AREA tab when the player is on the trail (no zone loaded). */
static void draw_trail_page(const App *app)
{
    const GraveGame *g = &app->game;
    char buf[64];

    paper_panel(6.0f, CONTENT_Y, 308.0f, 130.0f);
    text_woodcut_center(app, 160.0f, 84.0f, 0.8f, COL_BRASS_HI,
                        "ON THE TRAIL");
    text_center(app, 160.0f, 112.0f, 0.4f, COL_INK_SOFT,
                "CHOOSE A ZONE ON THE WORLD MAP");
    text_center(app, 160.0f, 130.0f, 0.34f, COL_INK_SOFT,
                "TAP WORLD OR PRESS SELECT");
    snprintf(buf, sizeof(buf), "LAST CAMP  %s",
             grave_zone_name(g->world.current));
    text_center(app, 160.0f, 156.0f, 0.34f, COL_INK, buf);
}

static void draw_area_page(const App *app)
{
    const GraveGame *g = &app->game;
    char buf[64];
    float frac = 0.0f;
    UiRect qs = { 6.0f, 180.0f, 308.0f, 18.0f };
    int secs = (int)g->zone_time;
    int rounds = 0;
    int i;

    if (!g->world.in_zone) {
        draw_trail_page(app);
        return;
    }

    paper_panel(6.0f, CONTENT_Y, 308.0f, 116.0f);
    text_left(app, 14.0f, CONTENT_Y + 3.0f, 0.36f, COL_INK_SOFT,
              grave_zone_name(g->zone));
    text_right(app, 306.0f, CONTENT_Y + 3.0f, 0.36f, COL_INK_SOFT, "AREA MAP");
    /* left column: quick zone facts (three short rows, no wrapping) */
    text_left(app, 14.0f, 82.0f, 0.36f, COL_INK_SOFT, "TIME");
    snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
    text_left(app, 14.0f, 92.0f, 0.4f, COL_INK, buf);
    text_left(app, 14.0f, 108.0f, 0.36f, COL_INK_SOFT, "ROT DOWN");
    snprintf(buf, sizeof(buf), "%d", g->zombies_killed);
    text_left(app, 14.0f, 118.0f, 0.4f, COL_INK, buf);
    text_left(app, 14.0f, 134.0f, 0.36f, COL_INK_SOFT, "ROUNDS");
    for (i = 0; i < WEAPON_COUNT; i++)
        rounds += g->player.ammo[i];
    snprintf(buf, sizeof(buf), "%d", rounds);
    text_left(app, 14.0f, 144.0f, 0.4f, COL_INK, buf);

    /* v0.3: app-local NEW ZONE badge (bottom of the facts column so it is not
     * covered by the map plate) */
    if ((app->new_zone_mask & (1u << (unsigned)g->zone)) != 0u) {
        fill_rect(14.0f, 158.0f, 56.0f, 13.0f, COL_BLOOD);
        text_center(app, 42.0f, 160.5f, 0.22f, COL_BONE, "NEW ZONE");
    }

    draw_area_map(app);

    /* right column: map legend + objective (single short line) */
    circle(232.0f, 84.0f, 3.0f, COL_BRASS_HI);
    text_left(app, 240.0f, 78.0f, 0.36f, COL_INK_SOFT, "YOU");
    circle(232.0f, 98.0f, 3.0f, COL_ROT);
    text_left(app, 240.0f, 92.0f, 0.36f, COL_INK_SOFT, "ROT");
    circle(232.0f, 112.0f, 3.0f, COL_HILITE);
    text_left(app, 240.0f, 106.0f, 0.36f, COL_INK_SOFT, "LOOT");
    tri(229.0f, 134.0f, 235.0f, 134.0f, 232.0f, 126.0f, COL_BRASS_HI);
    text_left(app, 240.0f, 122.0f, 0.36f, COL_INK_SOFT, "GATE");
    text_left(app, 226.0f, 138.0f, 0.36f, COL_INK_SOFT, "OBJECTIVE");
    if (g->quest.kind == QUEST_COLLECT) {
        snprintf(buf, sizeof(buf), "GET %s",
                 grave_drop_name((GraveDrop)g->quest.material));
        text_left(app, 226.0f, 150.0f, 0.38f, COL_BLOOD, buf);
    } else {
        text_left(app, 226.0f, 150.0f, 0.4f, COL_BLOOD, "CLEAR ROT");
    }

    /* quest progress strip */
    paper_panel(qs.x, qs.y, qs.w, qs.h);
    if (g->quest.target > 0)
        frac = (float)g->quest.progress / (float)g->quest.target;
    /* Short labels only: at scale 0.40+ "CLEAR THE ROT" would collide with the
     * progress readout, and short words keep the glyph stems crisp. */
    if (g->quest.kind == QUEST_COLLECT) {
        if (g->quest.material == DROP_COIN)
            snprintf(buf, sizeof(buf), "GET COINS");
        else if (g->quest.material == DROP_SINEW)
            snprintf(buf, sizeof(buf), "GET SINEW");
        else
            snprintf(buf, sizeof(buf), "GET CHARMS");
        text_left(app, qs.x + 14.0f, qs.y + 2.0f, 0.42f, COL_INK, buf);
    } else {
        text_left(app, qs.x + 14.0f, qs.y + 2.0f, 0.42f, COL_INK,
                  "CLEAR ROT");
    }
    snprintf(buf, sizeof(buf), "%d/%d", g->quest.progress, g->quest.target);
    text_left(app, qs.x + 108.0f, qs.y + 2.0f, 0.4f, COL_INK, buf);
    bar(qs.x + 146.0f, qs.y + 5.0f, 44.0f, 7.0f, frac, COL_BRASS_HI,
        COL_PAPER_LO);
    snprintf(buf, sizeof(buf), "C%d S%d", g->material_coins,
             g->material_sinew);
    text_right(app, qs.x + qs.w - 6.0f, qs.y + 2.0f, 0.34f, COL_INK_SOFT, buf);

    /* footer hint: hard cap y <= 204 so the system font line box fits */
    text_center(app, 160.0f, 201.0f, 0.34f, COL_BONE_DIM,
                "TOUCH TABS OR PRESS SELECT");
}
/* ------------------------------------------------------------------ */
/* WORLD page: valley map with zone nodes                             */
/* ------------------------------------------------------------------ */

static void format_mins(const char *prefix, float seconds, char *buf,
                        size_t size)
{
    int secs = (int)(seconds + 0.5f);

    snprintf(buf, size, "%s %d:%02d", prefix, secs / 60, secs % 60);
}

/* One WORLD-map node plate.  Reads the core's ZoneNode state: locked zones are
 * dimmed, unlocked ones are travelable, cleared ones show clears + best time;
 * the current zone/trailhead is highlighted in parchment. */
static void draw_world_node(const App *app, int zone, UiRect r)
{
    const GraveGame *g = &app->game;
    const ZoneNode *node = &g->world.nodes[zone];
    const char *name = grave_zone_name((ZoneId)zone);
    bool current = (g->world.current == (ZoneId)zone);
    char buf[64];
    char time_buf[40];
    float line2_y;
    float line3_y;

    if (current) {
        fill_rect(r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f, COL_INK);
        fill_rect(r.x - 1.0f, r.y - 1.0f, r.w + 2.0f, r.h + 2.0f,
                  COL_BRASS_HI);
        fill_rect(r.x, r.y, r.w, r.h, COL_PAPER);
        fill_rect(r.x, r.y, r.w, 3.0f, COL_BLOOD_HI);
        circle(r.x + 9.0f, r.y + r.h - 9.0f, 3.5f, COL_BLOOD_HI);
        text_shadow_center(app, r.x + r.w * 0.5f, r.y + 3.0f, 0.4f, COL_INK,
                           name);
        text_center(app, r.x + r.w * 0.5f, r.y + 18.0f, 0.34f, COL_BLOOD,
                    g->world.in_zone ? "CURRENT ZONE" : "TRAILHEAD");
        line2_y = r.y + 30.0f;
    } else {
        fill_rect(r.x, r.y, r.w, r.h, COL_BG2);
        frame_rect(r.x, r.y, r.w, r.h, 1.5f, CLR(0x4A, 0x3A, 0x26, 0xFF));
        text_center(app, r.x + r.w * 0.5f, r.y + 3.0f, 0.4f,
                    node->unlocked ? COL_BONE : CLR(0x8A, 0x7C, 0x62, 0xFF),
                    name);
        if (!node->unlocked)
            text_center(app, r.x + r.w * 0.5f, r.y + 18.0f, 0.34f,
                        CLR(0x8A, 0x7C, 0x62, 0xFF), "LOCKED");
        else if (node->cleared)
            text_center(app, r.x + r.w * 0.5f, r.y + 18.0f, 0.34f, COL_BRASS,
                        "CLEARED");
        else
            text_center(app, r.x + r.w * 0.5f, r.y + 18.0f, 0.34f, COL_BRASS_HI,
                        "TRAVEL");
        line2_y = r.y + 30.0f;
    }
    line3_y = line2_y;

    if (node->cleared && node->clears > 0 && line3_y + 8.0f <= r.y + r.h) {
        if (node->best_time > 0.0f) {
            format_mins("BEST", node->best_time, time_buf, sizeof(time_buf));
            snprintf(buf, sizeof(buf), "x%d  %s", node->clears, time_buf);
        } else {
            snprintf(buf, sizeof(buf), "x%d", node->clears);
        }
        text_center(app, r.x + r.w * 0.5f, line3_y, 0.34f,
                    current ? COL_INK_SOFT
                            : (node->unlocked ? COL_BONE_DIM
                                              : CLR(0x8A, 0x7C, 0x62, 0xFF)),
                    buf);
    }

    /* v0.3: app-local NEW badge, cleared once the player views WORLD/AREA */
    if (!current && (app->new_zone_mask & (1u << (unsigned)zone)) != 0u) {
        fill_rect(r.x + r.w - 33.0f, r.y - 6.0f, 30.0f, 13.0f, COL_BLOOD);
        text_center(app, r.x + r.w - 18.0f, r.y - 3.5f, 0.22f, COL_BONE, "NEW");
    }
}

static void draw_world_page(const App *app)
{
    const GraveGame *g = &app->game;
    char buf[64];
    int i;

    paper_panel(6.0f, CONTENT_Y, 308.0f, 116.0f);
    text_left(app, 14.0f, CONTENT_Y + 3.0f, 0.36f, COL_INK_SOFT,
              "THE VALLEY");
    text_right(app, 306.0f, CONTENT_Y + 3.0f, 0.36f, COL_INK_SOFT,
               "ZONE MAP");

    /* faint contour lines under the road/nodes so the parchment reads as a
     * valley map without competing with the node plates. */
    for (i = 0; i < 3; i++) {
        float ly = 86.0f + (float)i * 6.0f;

        line(20.0f, ly, 150.0f, ly + 1.0f, 1.0f,
             CLR(0x5A, 0x48, 0x30, 0x70));
        line(170.0f, ly + 1.0f, 300.0f, ly, 1.0f,
             CLR(0x5A, 0x48, 0x30, 0x70));
    }
    line(20.0f, 174.0f, 150.0f, 173.0f, 1.0f, CLR(0x5A, 0x48, 0x30, 0x70));
    line(170.0f, 173.0f, 300.0f, 174.0f, 1.0f, CLR(0x5A, 0x48, 0x30, 0x70));

    /* dashed roads from the trailhead/current node to the other zones */
    {
        UiRect cur = ui_world_node_rect(g->world.current);
        float ax = cur.x + cur.w * 0.5f;
        float ay = cur.y + cur.h * 0.5f;
        int z;

        for (z = 0; z < ZONE_COUNT; z++) {
            UiRect nr = ui_world_node_rect(z);
            float bx = nr.x + nr.w * 0.5f;
            float by = nr.y + nr.h * 0.5f;
            int k;

            if (z == (int)g->world.current)
                continue;
            for (k = 0; k < 10; k++) {
                float t0 = (float)k / 10.0f;
                float t1 = t0 + 0.05f;

                line(ax + (bx - ax) * t0, ay + (by - ay) * t0,
                     ax + (bx - ax) * t1, ay + (by - ay) * t1, 1.5f,
                     COL_INK_SOFT);
            }
        }
    }

    for (i = 0; i < ZONE_COUNT; i++)
        draw_world_node(app, i, ui_world_node_rect(i));

    /* quest line + run/lifetime summary */
    paper_panel(6.0f, 180.0f, 308.0f, 18.0f);
    if (g->quest.kind == QUEST_COLLECT) {
        snprintf(buf, sizeof(buf), "QUEST: GATHER %s",
                 grave_drop_name((GraveDrop)g->quest.material));
    } else {
        snprintf(buf, sizeof(buf), "QUEST: CLEAR THE ROT");
    }
    text_left(app, 12.0f, 183.0f, 0.4f, COL_INK, buf);
    snprintf(buf, sizeof(buf), "%d/%d", g->quest.progress, g->quest.target);
    text_right(app, 306.0f, 182.0f, 0.4f, COL_INK, buf);

    /* TODO(v0.3): accuracy needs a shots-fired counter in the core. */
    snprintf(buf, sizeof(buf),
             "COINS %d   SINEW %d   CHARMS %d   RUNS %d   ACC --",
             g->material_coins, g->material_sinew, g->material_bone_charms,
             g->zone_runs);
    text_center(app, 160.0f, 202.0f, 0.34f, COL_BONE_DIM, buf);
}
/* ------------------------------------------------------------------ */
/* PACK page: weapons, materials, character stats                     */
/* ------------------------------------------------------------------ */

static void draw_pack_weapon_card(const App *app, int index)
{
    const GraveGame *g = &app->game;
    UiRect r = ui_pack_weapon_rect(index);
    const WeaponSpec *spec = grave_weapon_spec((WeaponId)index);
    bool selected = (g->player.weapon == (WeaponId)index);
    bool owned = grave_inventory_owned(g, (WeaponId)index);
    char buf[64];
    int ammo = owned ? g->player.ammo[index] : 0;
    int mag = (spec != NULL && spec->magazine > 0) ? spec->magazine : 1;

    if (!owned) {
        fill_rect(r.x - 1.0f, r.y - 1.0f, r.w + 2.0f, r.h + 2.0f, COL_INK);
        fill_rect(r.x, r.y, r.w, r.h, CLR(0x6A, 0x5C, 0x44, 0xFF));
        fill_rect(r.x, r.y, 4.0f, r.h, CLR(0x44, 0x38, 0x28, 0xFF));
        if (spec != NULL && spec->name != NULL)
            text_left(app, r.x + 12.0f, r.y + 2.0f, 0.42f,
                      CLR(0x3A, 0x2E, 0x1E, 0xFF), spec->name);
        text_right(app, r.x + r.w - 8.0f, r.y + 2.0f, 0.36f,
                   CLR(0x3A, 0x2E, 0x1E, 0xFF), "NOT OWNED");
        if (spec != NULL) {
            snprintf(buf, sizeof(buf), "DMG %d   RNG %.0f   RELOAD %.1fs",
                     spec->damage, (double)spec->range,
                     (double)spec->reload_time);
            text_left(app, r.x + 12.0f, r.y + 16.0f, 0.30f,
                      CLR(0x3A, 0x2E, 0x1E, 0xFF), buf);
        }
        return;
    }

    if (selected) {
        fill_rect(r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f, COL_INK);
        fill_rect(r.x - 1.0f, r.y - 1.0f, r.w + 2.0f, r.h + 2.0f,
                  COL_BRASS_HI);
        fill_rect(r.x, r.y, r.w, r.h, COL_PAPER);
        fill_rect(r.x, r.y, 4.0f, r.h, COL_BLOOD_HI);
    } else {
        fill_rect(r.x - 1.0f, r.y - 1.0f, r.w + 2.0f, r.h + 2.0f, COL_INK);
        fill_rect(r.x, r.y, r.w, r.h, CLR(0xC9, 0xB6, 0x8C, 0xFF));
        fill_rect(r.x, r.y, 4.0f, r.h, CLR(0x7A, 0x60, 0x3C, 0xFF));
    }

    if (spec != NULL && spec->name != NULL)
        text_left(app, r.x + 12.0f, r.y + 2.0f, 0.42f, COL_INK, spec->name);

    snprintf(buf, sizeof(buf), "%d / %d", ammo, mag);
    text_right(app, r.x + r.w - 8.0f, r.y + 2.0f, 0.44f,
               ammo > 0 ? COL_INK : COL_BLOOD, buf);

    if (spec != NULL) {
        snprintf(buf, sizeof(buf), "DMG %d   RNG %.0f   RELOAD %.1fs",
                 spec->damage, (double)spec->range, (double)spec->reload_time);
        text_left(app, r.x + 12.0f, r.y + 16.0f, 0.30f, COL_INK_SOFT, buf);
        snprintf(buf, sizeof(buf), "MAG %d   %s", spec->magazine,
                 selected ? "EQUIPPED" : "TAP TO EQUIP");
        text_left(app, r.x + 12.0f, r.y + 27.0f, 0.30f,
                  selected ? COL_BLOOD : COL_INK_SOFT, buf);
    }
}

static void draw_pack_page(const App *app)
{
    const GraveGame *g = &app->game;
    UiRect panel = { 182.0f, CONTENT_Y, 132.0f, 156.0f };
    const Player *p = &g->player;
    char buf[64];
    int i;

    for (i = 0; i < WEAPON_COUNT; i++)
        draw_pack_weapon_card(app, i);

    /* right-hand character sheet */
    paper_panel(panel.x, panel.y, panel.w, panel.h);
    text_shadow_center(app, panel.x + panel.w * 0.5f, panel.y + 4.0f, 0.38f,
                       COL_INK, "MATERIALS");
    divider(panel.x + 8.0f, panel.y + 18.0f, panel.w - 16.0f);

    text_left(app, panel.x + 10.0f, panel.y + 22.0f, 0.34f, COL_INK_SOFT,
              "OLD COINS");
    snprintf(buf, sizeof(buf), "%d", g->material_coins);
    text_right(app, panel.x + panel.w - 8.0f, panel.y + 22.0f, 0.34f, COL_INK,
               buf);
    text_left(app, panel.x + 10.0f, panel.y + 33.0f, 0.34f, COL_INK_SOFT,
              "ROT SINEW");
    snprintf(buf, sizeof(buf), "%d", g->material_sinew);
    text_right(app, panel.x + panel.w - 8.0f, panel.y + 33.0f, 0.34f, COL_INK,
               buf);
    text_left(app, panel.x + 10.0f, panel.y + 44.0f, 0.34f, COL_INK_SOFT,
              "BONE CHARM");
    snprintf(buf, sizeof(buf), "%d", g->material_bone_charms);
    text_right(app, panel.x + panel.w - 8.0f, panel.y + 44.0f, 0.34f, COL_INK,
               buf);

    divider(panel.x + 8.0f, panel.y + 56.0f, panel.w - 16.0f);

    text_shadow_center(app, panel.x + panel.w * 0.5f, panel.y + 59.0f, 0.38f,
                       COL_INK, "CHARACTER");
    text_left(app, panel.x + 10.0f, panel.y + 72.0f, 0.34f, COL_INK_SOFT,
              "HEALTH");
    snprintf(buf, sizeof(buf), "%d/%d", (int)(p->hp + 0.5f),
             (int)(p->max_hp + 0.5f));
    text_right(app, panel.x + panel.w - 8.0f, panel.y + 72.0f, 0.34f, COL_INK,
               buf);
    bar(panel.x + 10.0f, panel.y + 81.0f, panel.w - 20.0f, 5.0f,
        p->max_hp > 0.0f ? p->hp / p->max_hp : 0.0f, COL_BLOOD_HI,
        COL_PAPER_LO);

    text_left(app, panel.x + 10.0f, panel.y + 89.0f, 0.34f, COL_INK_SOFT,
              "STAMINA");
    {
        float stam = p->stamina > 1.5f ? p->stamina / 100.0f : p->stamina;

        bar(panel.x + 10.0f, panel.y + 98.0f, panel.w - 20.0f, 5.0f, stam,
            COL_STAM, COL_PAPER_LO);
    }

    text_left(app, panel.x + 10.0f, panel.y + 106.0f, 0.34f, COL_INK_SOFT,
              "RUN KILLS");
    snprintf(buf, sizeof(buf), "%d", g->zombies_killed);
    text_right(app, panel.x + panel.w - 8.0f, panel.y + 106.0f, 0.34f, COL_INK,
               buf);

    text_left(app, panel.x + 10.0f, panel.y + 117.0f, 0.34f, COL_INK_SOFT,
              "LIFETIME");
    snprintf(buf, sizeof(buf), "%d", g->total_kills);
    text_right(app, panel.x + panel.w - 8.0f, panel.y + 117.0f, 0.34f, COL_INK,
               buf);

    text_left(app, panel.x + 10.0f, panel.y + 128.0f, 0.34f, COL_INK_SOFT,
              "ZONE RUNS");
    snprintf(buf, sizeof(buf), "%d", g->zone_runs);
    text_right(app, panel.x + panel.w - 8.0f, panel.y + 128.0f, 0.34f, COL_INK,
               buf);

    /* v0.3: lifetime accuracy from the core shot counters */
    {
        int acc = g->total_shots > 0
                      ? (g->total_hits * 100) / g->total_shots
                      : 0;

        text_left(app, panel.x + 10.0f, panel.y + 139.0f, 0.34f, COL_INK_SOFT,
                  "ACC");
        snprintf(buf, sizeof(buf), "%d%%", acc);
        text_right(app, panel.x + panel.w - 8.0f, panel.y + 139.0f, 0.34f,
                   COL_INK, buf);
    }
}
/* ------------------------------------------------------------------ */
/* v0.3 extras: boss bar, toast, travel dialog                        */
/* ------------------------------------------------------------------ */

/* Foreman boss bar: three thick layer segments along the bottom edge.  Only
 * drawn while the boss is alive; nothing else in the HUD is affected. */
static void draw_boss_bar(const App *app)
{
    const GraveGame *g = &app->game;
    float frac;
    int i;

    if (!g->world.in_zone || !g->boss_alive || g->boss_max_hp <= 0.0f)
        return;

    frac = g->boss_hp / g->boss_max_hp;
    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;

    dark_panel(6.0f, 220.0f, 308.0f, 18.0f);
    text_left(app, 12.0f, 224.0f, 0.32f, COL_BLOOD_HI, "FOREMAN");

    for (i = 0; i < 3; i++) {
        float x = 54.0f + (float)i * 85.0f;
        float seg = frac * 3.0f - (float)(2 - i);
        u32 fill;

        if (seg < 0.0f)
            seg = 0.0f;
        if (seg > 1.0f)
            seg = 1.0f;
        if (i == 0)
            fill = COL_BLOOD_HI;       /* outer HP layer   */
        else if (i == 1)
            fill = CLR(0x62, 0x80, 0xC8, 0xFF); /* shield layer */
        else
            fill = COL_HILITE;         /* armor layer      */
        bar(x, 224.0f, 82.0f, 10.0f, seg, fill, COL_BG2);
    }
}

/* Transient bottom-screen feedback ("PURCHASED: ..." / "MODELS RELOADED"). */
static void draw_toast(const App *app)
{
    char buf[52];
    float w;

    if (app->toast_timer <= 0.0f || app->toast[0] == '\0')
        return;

    snprintf(buf, sizeof(buf), "%.44s", app->toast);
    w = (float)strlen(buf) * 8.0f * 0.30f + 20.0f;
    if (w > 300.0f)
        w = 300.0f;
    if (w < 300.0f)
        w = 300.0f; /* full-width notification bar, never a half-overlap */
    dark_panel(10.0f, CONTENT_Y, w, 16.0f);
    text_center(app, 160.0f, CONTENT_Y + 4.0f, 0.34f, COL_HILITE, buf);
}

/* TRAVEL confirmation dialog for WORLD nodes (unlocked, not current). */
static void draw_travel_dialog(const App *app)
{
    const GraveGame *g = &app->game;
    int z = app->travel_confirm_zone;
    const ZoneNode *node;
    UiRect yes = ui_travel_yes_rect();
    UiRect no = ui_travel_no_rect();
    char buf[64];

    if (z < 0 || z >= ZONE_COUNT || app->page != APP_PAGE_HUD)
        return;

    node = &g->world.nodes[z];
    g_z = 1.0f;
    fill_rect(0.0f, 0.0f, 320.0f, 240.0f, CLR(0x08, 0x06, 0x04, 0xB4));
    paper_panel(46.0f, 72.0f, 228.0f, 92.0f);
    text_center(app, 160.0f, 82.0f, 0.44f, COL_INK, "TRAVEL TO");
    text_shadow_center(app, 160.0f, 100.0f, 0.46f, COL_BLOOD,
                       grave_zone_name((ZoneId)z));
    if (node->cleared) {
        snprintf(buf, sizeof(buf), "CLEARED x%d  -  BEST %d:%02d", node->clears,
                 (int)node->best_time / 60, (int)node->best_time % 60);
    } else {
        snprintf(buf, sizeof(buf), "NEW TERRITORY  -  THE ROT WAITS");
    }
    text_center(app, 160.0f, 120.0f, 0.34f, COL_INK_SOFT, buf);

    paper_button(yes.x, yes.y, yes.w, yes.h, COL_BRASS_HI);
    text_shadow_center(app, yes.x + yes.w * 0.5f, yes.y + 6.0f, 0.35f,
                       COL_INK, "TRAVEL");
    paper_button(no.x, no.y, no.w, no.h, COL_BLOOD);
    text_shadow_center(app, no.x + no.w * 0.5f, no.y + 6.0f, 0.35f, COL_INK,
                       "STAY");
    g_z = 0.5f;
}
/* ------------------------------------------------------------------ */
/* SHOP page: trailhead trader                                       */
/* ------------------------------------------------------------------ */

static void shop_item_price(const ShopItemSpec *spec, int level, char *buf,
                            size_t size)
{
    char part[3][16];
    int n = 0;

    if (level < 0)
        level = 0;
    if (level >= SHOP_MAX_LEVELS)
        level = SHOP_MAX_LEVELS - 1;
    if (spec->cost_coins[level] > 0) {
        snprintf(part[n], sizeof(part[n]), "C%d", spec->cost_coins[level]);
        n++;
    }
    if (spec->cost_sinew[level] > 0) {
        snprintf(part[n], sizeof(part[n]), "S%d", spec->cost_sinew[level]);
        n++;
    }
    if (spec->cost_charms[level] > 0) {
        snprintf(part[n], sizeof(part[n]), "K%d", spec->cost_charms[level]);
        n++;
    }
    if (n == 0) {
        snprintf(buf, size, "%s", "FREE");
    } else if (n == 1) {
        snprintf(buf, size, "%s", part[0]);
    } else if (n == 2) {
        snprintf(buf, size, "%s %s", part[0], part[1]);
    } else {
        snprintf(buf, size, "%s %s %s", part[0], part[1], part[2]);
    }
}

static bool shop_item_affordable(const GraveGame *g, const ShopItemSpec *spec,
                                 int level)
{
    if (level < 0)
        level = 0;
    if (level >= SHOP_MAX_LEVELS)
        level = SHOP_MAX_LEVELS - 1;
    return g->material_coins >= spec->cost_coins[level] &&
           g->material_sinew >= spec->cost_sinew[level] &&
           g->material_bone_charms >= spec->cost_charms[level];
}

/* Small stable glyphs keyed off ShopDesc (core-owned ids, no name parsing). */
static void shop_icon(float cx, float cy, const ShopItemSpec *spec)
{
    switch (spec->desc) {
    case SHOP_DESC_AMMO:
        fill_rect(cx - 5.0f, cy - 3.0f, 9.0f, 5.0f, COL_BRASS_HI);
        tri(cx + 4.0f, cy - 3.0f, cx + 4.0f, cy + 2.0f, cx + 9.0f, cy - 0.5f,
            COL_BRASS_HI);
        fill_rect(cx - 5.0f, cy + 2.0f, 9.0f, 2.0f, COL_INK_SOFT);
        break;
    case SHOP_DESC_TUNE:
        fill_rect(cx - 7.0f, cy - 2.0f, 12.0f, 4.0f, COL_BONE_DIM);
        fill_rect(cx + 3.0f, cy - 2.0f, 4.0f, 6.0f, COL_INK_SOFT);
        fill_rect(cx - 3.0f, cy + 2.0f, 3.0f, 4.0f, COL_INK_SOFT);
        break;
    case SHOP_DESC_VITALITY:
        draw_heart(cx, cy, 6.0f, COL_BLOOD_HI);
        break;
    case SHOP_DESC_RELOAD:
        circle(cx, cy, 6.0f, COL_INK);
        circle(cx, cy, 4.6f, COL_ROT);
        circle(cx, cy, 2.6f, COL_INK);
        tri(cx + 2.0f, cy - 6.0f, cx + 8.0f, cy - 4.0f, cx + 4.0f, cy + 1.0f,
            COL_INK);
        break;
    case SHOP_DESC_MEDKIT:
    default:
        fill_rect(cx - 6.0f, cy - 2.0f, 12.0f, 4.0f, COL_BONE);
        fill_rect(cx - 2.0f, cy - 6.0f, 4.0f, 12.0f, COL_BONE);
        break;
    }
}

static void draw_shop_item_card(const App *app, int item)
{
    const GraveGame *g = &app->game;
    const ShopItemSpec *spec = grave_shop_item((ShopItem)item);
    UiRect r = ui_shop_item_rect(item);
    int level;
    int max;
    bool can;
    bool affordable;
    bool maxed;
    char price[56];

    if (spec == NULL)
        return;

    level = grave_shop_level(g, (ShopItem)item);
    max = spec->max_level;
    can = grave_shop_can_buy(g, (ShopItem)item);
    maxed = max > 0 && level >= max;
    affordable = shop_item_affordable(g, spec, level);
    shop_item_price(spec, level, price, sizeof(price));

    paper_panel(r.x, r.y, r.w, r.h);
    shop_icon(r.x + 17.0f, r.y + 15.0f, spec);

    text_left(app, r.x + 30.0f, r.y + 2.0f, 0.34f, COL_INK, spec->name);

    /* Level is shown as pips (no tiny "LV 1/3" digits that lose stems);
     * repeatable consumables get a small plus glyph instead. */
    if (max > 0) {
        int pip;

        for (pip = 0; pip < max && pip < 5; pip++)
            circle(r.x + 9.0f + (float)pip * 5.0f, r.y + 27.0f, 1.8f,
                   pip < level ? COL_BRASS : COL_PAPER_LO);
    } else {
        fill_rect(r.x + 8.0f, r.y + 26.0f, 6.0f, 1.6f, COL_BRASS);
        fill_rect(r.x + 10.2f, r.y + 23.8f, 1.6f, 6.0f, COL_BRASS);
    }

    /* cost line, readable scale */
    text_left(app, r.x + 30.0f, r.y + 18.0f, 0.34f,
              maxed ? COL_BRASS : (affordable ? COL_INK_SOFT : COL_BLOOD),
              maxed ? "-" : price);

    {
        float bx = r.x + r.w - 48.0f;
        float by = r.y + 18.0f;
        u32 face;
        const char *label;

        if (maxed) {
            face = COL_PAPER_LO;
            label = "MAXED";
        } else if (!affordable) {
            face = CLR(0x8A, 0x3A, 0x2A, 0xFF);
            label = "NEED";
        } else if (!can) {
            face = COL_PAPER_LO;
            label = "FULL";
        } else {
            face = COL_BRASS_HI;
            label = "BUY";
        }
        fill_rect(bx, by, 42.0f, 13.0f, COL_INK);
        fill_rect(bx + 1.0f, by + 1.0f, 40.0f, 11.0f, face);
        text_center(app, bx + 21.0f, by + 2.5f, 0.32f,
                    maxed ? COL_INK_SOFT : COL_INK, label);
    }
}

static void draw_shop_page(const App *app)
{
    const GraveGame *g = &app->game;
    char buf[64];
    int i;

    if (g->world.in_zone) {
        paper_panel(6.0f, CONTENT_Y, 308.0f, 174.0f);
        text_left(app, 14.0f, CONTENT_Y + 4.0f, 0.34f, COL_INK_SOFT,
                  "THE TRADER");
        text_right(app, 306.0f, CONTENT_Y + 4.0f, 0.34f, COL_INK_SOFT, "SHOP");

        /* trader skull totem */
        circle(160.0f, 110.0f, 18.0f, COL_BONE);
        circle(153.0f, 104.0f, 3.5f, COL_INK);
        circle(167.0f, 104.0f, 3.5f, COL_INK);
        fill_rect(150.0f, 117.0f, 20.0f, 3.0f, COL_INK);
        fill_rect(140.0f, 88.0f, 40.0f, 5.0f, COL_BRASS);
        fill_rect(148.0f, 82.0f, 24.0f, 6.0f, COL_BRASS_HI);

        text_center(app, 160.0f, 140.0f, 0.5f, COL_BLOOD, "NO TRADER HERE");
        text_center(app, 160.0f, 156.0f, 0.34f, COL_INK_SOFT,
                    "THE TRADER WAITS AT THE TRAILHEAD");
        paper_panel(84.0f, 168.0f, 152.0f, 20.0f);
        text_center(app, 160.0f, 172.0f, 0.32f, COL_INK,
                    "RETURN TO THE TRAIL (WORLD)");
        text_center(app, 160.0f, 200.0f, 0.30f, COL_INK_SOFT,
                    "PURCHASES ARE ONLY MADE OUTSIDE A ZONE");
        return;
    }

    /* wallet header */
    dark_panel(6.0f, CONTENT_Y, 308.0f, 18.0f);
    text_left(app, 12.0f, CONTENT_Y + 3.0f, 0.32f, COL_BONE, "THE TRADER");
    snprintf(buf, sizeof(buf), "COINS %d  SINEW %d  CHARMS %d",
             g->material_coins, g->material_sinew, g->material_bone_charms);
    text_right(app, 306.0f, CONTENT_Y + 3.0f, 0.32f, COL_BRASS_HI, buf);

    for (i = 0; i < SHOP_ITEM_COUNT; i++)
        draw_shop_item_card(app, i);
}
/* ------------------------------------------------------------------ */
/* HUD page (strip + tabs + one info page)                            */
/* ------------------------------------------------------------------ */

static void draw_tab_page(const App *app, AppTab tab)
{
    switch (tab) {
    case APP_TAB_WORLD:
        draw_world_page(app);
        break;
    case APP_TAB_PACK:
        draw_pack_page(app);
        break;
    case APP_TAB_SHOP:
        draw_shop_page(app);
        break;
    case APP_TAB_AREA:
    default:
        draw_area_page(app);
        break;
    }
}

static void draw_hud_base(const App *app)
{
    float save_dx = g_dx;

    draw_background(app);

    if (app->tab_anim > 0.0f && app->tab_prev != app->tab) {
        float p = 1.0f - app->tab_anim / UI_TAB_TRANSITION_TIME;
        float dir;

        if (p < 0.0f)
            p = 0.0f;
        if (p > 1.0f)
            p = 1.0f;
        if (app->tab_prev == APP_TAB_COUNT - 1 && app->tab == APP_TAB_AREA)
            dir = 1.0f;
        else if (app->tab_prev == APP_TAB_AREA && app->tab == APP_TAB_COUNT - 1)
            dir = -1.0f;
        else
            dir = (app->tab > app->tab_prev) ? 1.0f : -1.0f;

        g_dx = -dir * 320.0f * p;
        draw_tab_page(app, app->tab_prev);
        g_dx = dir * 320.0f * (1.0f - p);
        draw_tab_page(app, app->tab);
        g_dx = 0.0f;
    } else {
        draw_tab_page(app, app->tab);
    }

    draw_tab_bar(app);
    draw_strip(app);
    draw_boss_bar(app);
    g_dx = save_dx;
}

/* ------------------------------------------------------------------ */
/* title page                                                         */
/* ------------------------------------------------------------------ */

static void caption_line(const App *app, float y, const char *left,
                         const char *right)
{
    if (left != NULL)
        text_left(app, 14.0f, y, 0.34f, COL_BONE_DIM, left);
    if (right != NULL)
        text_right(app, 306.0f, y, 0.34f, COL_BONE_DIM, right);
}

static void draw_title(const App *app)
{
    float pulse = 0.75f + 0.25f * sinf(app->time * 3.2f);
    int i;

    draw_background(app);

    /* hanging sign board behind the logo */
    fill_rect(20.0f, 22.0f, 280.0f, 58.0f, COL_INK);
    fill_rect(22.0f, 24.0f, 276.0f, 54.0f, CLR(0x3A, 0x2B, 0x1C, 0xFF));
    for (i = 0; i < 276; i += 20)
        fill_rect(22.0f + (float)i, 24.0f, 1.0f, 54.0f, COL_GRAIN);
    circle(28.0f, 30.0f, 3.0f, COL_BRASS);
    circle(292.0f, 30.0f, 3.0f, COL_BRASS);
    circle(28.0f, 74.0f, 3.0f, COL_BRASS);
    circle(292.0f, 74.0f, 3.0f, COL_BRASS);

    text_woodcut_center(app, 160.0f, 27.0f, 1.2f, COL_BRASS_HI,
                        "GRAVEPOWDER");
    divider(50.0f, 70.0f, 220.0f);
    text_center(app, 160.0f, 6.0f, 0.38f, COL_BONE_DIM,
                "A 3DS ZOMBIE SHOOTER");

    /* red zone ribbon */
    fill_rect(96.0f, 84.0f, 128.0f, 20.0f, COL_BLOOD);
    fill_rect(96.0f, 84.0f, 128.0f, 2.0f, COL_BLOOD_HI);
    fill_rect(96.0f, 102.0f, 128.0f, 2.0f, COL_INK);
    tri(96.0f, 84.0f, 88.0f, 94.0f, 96.0f, 104.0f, COL_BLOOD);
    tri(224.0f, 84.0f, 232.0f, 94.0f, 224.0f, 104.0f, COL_BLOOD);
    text_shadow_center(app, 160.0f, 86.0f, 0.48f, COL_BONE, "ASHEN MILL");

    if (app->has_save) {
        UiRect cont = ui_title_continue_rect();
        UiRect ng = ui_title_newgame_rect();
        char savebuf[80];
        int cleared = 0;
        int acc = app->game.total_shots > 0
                      ? (app->game.total_hits * 100) / app->game.total_shots
                      : 0;
        int secs = (int)app->game.total_time;

        for (i = 0; i < ZONE_COUNT; i++)
            if (app->game.world.nodes[i].cleared)
                cleared++;
        snprintf(savebuf, sizeof(savebuf), "SAVE: %d/%d ZONES  -  %d:%02d  -  ACC %d%%",
                 cleared, ZONE_COUNT, secs / 60, secs % 60, acc);
        text_center(app, 160.0f, 108.0f, 0.34f, COL_BONE_DIM, savebuf);

        char contbuf[64];

        paper_button(cont.x, cont.y, cont.w, cont.h,
                     pulse > 0.9f ? COL_HILITE : COL_BRASS_HI);
        snprintf(contbuf, sizeof(contbuf), "CONTINUE  -  %s",
                 grave_zone_name(app_title_target_zone(app)));
        text_shadow_center(app, 160.0f, cont.y + 7.0f, 0.4f, COL_INK, contbuf);

        paper_button(ng.x, ng.y, ng.w, ng.h, COL_BRASS);
        text_shadow_center(app, 160.0f, ng.y + 6.0f, 0.34f, COL_INK,
                           "NEW GAME");
        text_center(app, 160.0f, 190.0f, 0.34f, COL_BONE_DIM,
                    "A CONTINUE  -  SELECT NEW GAME");
    } else {
        /* control hints on a dark board */
        dark_panel(40.0f, 112.0f, 240.0f, 48.0f);
        text_center(app, 160.0f, 116.0f, 0.38f, COL_BONE_DIM,
                    "PAD MOVE    TOUCH DRAG TO AIM");
        text_center(app, 160.0f, 129.0f, 0.38f, COL_BONE_DIM,
                    "A FIRE    B RELOAD    X SWAP WEAPON");
        text_center(app, 160.0f, 142.0f, 0.38f, COL_BONE_DIM,
                    "Y CAMERA    SELECT TABS    START PAUSE");

        {
            UiRect r = ui_title_start_rect();
            u32 fill = pulse > 0.9f ? COL_HILITE : COL_BRASS_HI;

            paper_button(r.x, r.y, r.w, r.h, fill);
            text_shadow_center(app, 160.0f, r.y + 7.0f, 0.55f, COL_INK,
                               "PRESS A OR TAP START");
        }
    }

    divider(40.0f, 196.0f, 240.0f);
    {
        char footer[48];

        snprintf(footer, sizeof(footer), "%s  -  %s", GRAVEPOWDER_VERSION,
                 grave_zone_name(ZONE_ASHEN_MILL));
        caption_line(app, 202.0f, footer, "3DS HOMEBREW");
    }
}

/* ------------------------------------------------------------------ */
/* v0.3: WARDROBE, SAVE sub-page, NEW GAME confirmation               */
/* ------------------------------------------------------------------ */

static void draw_wardrobe_page(const App *app)
{
    static const char *const kSlotNames[R3D_MODEL_COUNT] = { "HERO", "ZOMBIES",
                                                             "GUNS" };
    UiRect panel = { 30.0f, 18.0f, 260.0f, 204.0f };
    UiRect rel = ui_wardrobe_reload_rect();
    UiRect back = ui_wardrobe_back_rect();
    char buf[96];
    int i;

    draw_hud_base(app);
    g_z = 1.0f;
    fill_rect(0.0f, 0.0f, 320.0f, 240.0f, CLR(0x08, 0x06, 0x04, 0xB4));

    paper_panel(panel.x, panel.y, panel.w, panel.h);
    text_woodcut_center(app, 160.0f, 22.0f, 0.62f, COL_BRASS_HI, "WARDROBE");
    text_center(app, 160.0f, 44.0f, 0.30f, COL_INK_SOFT, "MODEL SOURCES");
    divider(44.0f, 58.0f, 232.0f);

    for (i = 0; i < R3D_MODEL_COUNT; i++) {
        const char *src = r3d_model_source((R3dModelSlot)i);
        u32 dot = COL_BONE_DIM;

        if (src != NULL && strstr(src, "SD:") != NULL)
            dot = COL_ROT;
        else if (src != NULL && strstr(src, "procedural") != NULL)
            dot = COL_BLOOD_HI;
        else if (src != NULL && strstr(src, "embedded") != NULL)
            dot = COL_BRASS_HI;

        circle(46.0f, 72.0f + (float)i * 24.0f, 3.0f, COL_INK);
        circle(46.0f, 72.0f + (float)i * 24.0f, 2.2f, dot);
        text_left(app, 54.0f, 66.0f + (float)i * 24.0f, 0.3f, COL_INK,
                  kSlotNames[i]);
        text_left(app, 54.0f, 76.0f + (float)i * 24.0f, 0.34f, COL_INK_SOFT,
                  src != NULL ? src : "not loaded");
    }

    dark_panel(44.0f, 138.0f, 232.0f, 15.0f);
    text_center(app, 160.0f, 141.0f, 0.28f, COL_BONE_DIM,
                "sdmc:/3ds/Gravepowder/models/");

    paper_button(rel.x, rel.y, rel.w, rel.h, COL_BRASS_HI);
    text_shadow_center(app, rel.x + rel.w * 0.5f, rel.y + 4.0f, 0.34f, COL_INK,
                       "RELOAD MODELS");

    /* Status/hint line sits BELOW the reload button, never over the note. */
    text_center(app, 160.0f, 180.0f, 0.32f,
                app->wardrobe_flash > 0.0f
                    ? (app->wardrobe_reload_ok ? COL_ROT : COL_BLOOD_HI)
                    : COL_INK_SOFT,
                app->wardrobe_flash > 0.0f
                    ? (app->wardrobe_reload_ok
                           ? "LAST RELOAD: OK - MODELS RE-READ"
                           : "LAST RELOAD: FALLBACK USED (CHECK FILES)")
                    : "PLACE .GPM FILES IN THE FOLDER, THEN RELOAD");

    paper_button(back.x, back.y, back.w, back.h, COL_BRASS);
    text_shadow_center(app, back.x + back.w * 0.5f, back.y + 4.0f, 0.32f,
                       COL_INK, "BACK");

    snprintf(buf, sizeof(buf), "%s", "");
    (void)buf;
}

static void draw_save_page(const App *app)
{
    const GraveGame *g = &app->game;
    UiRect panel = { 30.0f, 18.0f, 260.0f, 204.0f };
    UiRect erase = ui_save_erase_rect();
    UiRect back = ui_save_back_rect();
    char buf[80];
    int cleared = 0;
    int i;
    int acc = g->total_shots > 0 ? (g->total_hits * 100) / g->total_shots : 0;
    int secs = (int)g->total_time;

    for (i = 0; i < ZONE_COUNT; i++)
        if (g->world.nodes[i].cleared)
            cleared++;

    draw_hud_base(app);
    g_z = 1.0f;
    fill_rect(0.0f, 0.0f, 320.0f, 240.0f, CLR(0x08, 0x06, 0x04, 0xB4));

    paper_panel(panel.x, panel.y, panel.w, panel.h);
    text_woodcut_center(app, 160.0f, 22.0f, 0.6f, COL_BRASS_HI,
                        "SAVE & PROGRESS");
    divider(44.0f, 46.0f, 232.0f);

    text_left(app, 44.0f, 54.0f, 0.34f, COL_INK_SOFT, "SAVE FILE");
    text_right(app, 276.0f, 54.0f, 0.34f, COL_INK,
               app->has_save ? "save.bin  -  VALID" : "NO SAVE YET");
    snprintf(buf, sizeof(buf), "ZONES %d/%d   %d:%02d   ACC %d%%", cleared,
             ZONE_COUNT, secs / 60, secs % 60, acc);
    text_left(app, 44.0f, 70.0f, 0.34f, COL_INK_SOFT, "PROGRESS");
    text_right(app, 276.0f, 70.0f, 0.34f, COL_INK, buf);
    text_left(app, 44.0f, 86.0f, 0.34f, COL_INK_SOFT, "LAST CAMP");
    text_right(app, 276.0f, 86.0f, 0.34f, COL_INK,
               grave_zone_name(g->world.current));

    paper_button(erase.x, erase.y, erase.w, erase.h,
                 app->save_erase_armed ? COL_BLOOD_HI : COL_BLOOD);
    text_shadow_center(app, erase.x + erase.w * 0.5f, erase.y + 7.0f, 0.34f,
                       COL_INK,
                       app->save_erase_armed
                           ? "TAP AGAIN TO ERASE SAVE"
                           : "ERASE SAVE FILE");
    text_center(app, 160.0f, 146.0f, 0.32f, COL_INK_SOFT,
                "THIS CANNOT BE UNDONE");

    paper_button(back.x, back.y, back.w, back.h, COL_BRASS_HI);
    text_shadow_center(app, back.x + back.w * 0.5f, back.y + 5.0f, 0.32f,
                       COL_INK, "BACK");

    text_center(app, 160.0f, 214.0f, 0.32f, COL_INK_SOFT,
                "SAVE HAPPENS ON ZONE CLEAR / QUIT / SHOP / EQUIP");
}

static void draw_title_confirm_page(const App *app)
{
    UiRect yes = ui_title_confirm_yes_rect();
    UiRect no = ui_title_confirm_no_rect();

    draw_title(app);
    g_z = 1.0f;
    fill_rect(0.0f, 0.0f, 320.0f, 240.0f, CLR(0x08, 0x06, 0x04, 0xC0));

    paper_panel(30.0f, 40.0f, 260.0f, 160.0f);
    text_center(app, 160.0f, 50.0f, 0.62f, COL_INK,
                app->has_save ? "START A NEW GAME?" : "START A NEW GAME");
    text_center(app, 160.0f, 74.0f, 0.4f, COL_BLOOD,
                "YOUR SAVE WILL BE ERASED");
    text_center(app, 160.0f, 96.0f, 0.32f, COL_INK_SOFT,
                app->has_save ? "PROGRESS WILL BE LOST"
                              : "NO SAVE FILE - YOU CAN'T LOSE ANYTHING YET");
    text_center(app, 160.0f, 114.0f, 0.30f, COL_INK_SOFT, "CAMP");
    text_center(app, 160.0f, 126.0f, 0.32f, COL_INK,
                grave_zone_name(app->game.world.current));

    paper_button(yes.x, yes.y, yes.w, yes.h, COL_BLOOD_HI);
    text_shadow_center(app, yes.x + yes.w * 0.5f, yes.y + 6.0f, 0.34f,
                       COL_INK, "ERASE + START");
    paper_button(no.x, no.y, no.w, no.h, COL_BRASS_HI);
    text_shadow_center(app, no.x + no.w * 0.5f, no.y + 6.0f, 0.34f, COL_INK,
                       "CANCEL");
}
/* ------------------------------------------------------------------ */
/* pause + settings pages                                             */
/* ------------------------------------------------------------------ */

static void draw_pause(const App *app)
{
    const GraveGame *g = &app->game;
    UiRect panel = { 30.0f, 18.0f, 260.0f, 204.0f };
    UiRect rr = ui_pause_resume_rect();
    UiRect ra = ui_pause_restart_rect();
    UiRect se = ui_pause_settings_rect();
    UiRect wr = ui_pause_wardrobe_rect();
    UiRect qu = ui_pause_quit_rect();
    char buf[48];

    draw_hud_base(app);
    g_z = 1.0f;
    fill_rect(0.0f, 0.0f, 320.0f, 240.0f, CLR(0x08, 0x06, 0x04, 0xB4));

    paper_panel(panel.x, panel.y, panel.w, panel.h);
    text_woodcut_center(app, 160.0f, 22.0f, 0.68f, COL_BRASS_HI, "PAUSED");
    text_center(app, 160.0f, 50.0f, 0.32f, COL_INK_SOFT,
                "THE DEAD ARE PATIENT");

    paper_button(rr.x, rr.y, rr.w, rr.h, COL_BRASS_HI);
    text_shadow_center(app, 160.0f, rr.y + 5.0f, 0.42f, COL_INK, "RESUME");

    paper_button(ra.x, ra.y, ra.w, ra.h, COL_BRASS);
    text_shadow_center(app, 160.0f, ra.y + 5.0f, 0.38f, COL_INK,
                       "RESTART ZONE");

    paper_button(se.x, se.y, se.w, se.h, COL_BRASS);
    text_shadow_center(app, 160.0f, se.y + 5.0f, 0.38f, COL_INK, "SETTINGS");

    paper_button(wr.x, wr.y, wr.w, wr.h, COL_BRASS);
    text_shadow_center(app, 160.0f, wr.y + 5.0f, 0.38f, COL_INK, "WARDROBE");

    paper_button(qu.x, qu.y, qu.w, qu.h, COL_BLOOD);
    text_shadow_center(app, 160.0f, qu.y + 5.0f, 0.38f, COL_INK,
                       "QUIT TO TITLE");

    snprintf(buf, sizeof(buf), "ZONE %s", grave_zone_name(g->zone));
    text_center(app, 160.0f, 202.0f, 0.32f, COL_INK_SOFT, buf);
}

static void draw_settings(const App *app)
{
    static const char *const kSensName[APP_SENS_COUNT] = { "LOW", "MEDIUM",
                                                           "HIGH" };
    UiRect panel = { 30.0f, 18.0f, 260.0f, 204.0f };
    UiRect cam = ui_settings_camera_rect();
    UiRect sen = ui_settings_sens_rect();
    UiRect inv = ui_settings_invert_rect();
    UiRect mut = ui_settings_mute_rect();
    UiRect sv = ui_settings_save_rect();
    UiRect back = ui_settings_back_rect();
    int sens = app->settings.touch_sens;

    if (sens < 0 || sens >= APP_SENS_COUNT)
        sens = APP_SENS_MED;

    draw_hud_base(app);
    g_z = 1.0f;
    fill_rect(0.0f, 0.0f, 320.0f, 240.0f, CLR(0x08, 0x06, 0x04, 0xB4));

    paper_panel(panel.x, panel.y, panel.w, panel.h);
    text_woodcut_center(app, 160.0f, 28.0f, 0.75f, COL_BRASS_HI, "SETTINGS");

    paper_button(cam.x, cam.y, cam.w, cam.h, COL_BRASS);
    text_left(app, cam.x + 12.0f, cam.y + 6.0f, 0.4f, COL_INK, "CAMERA");
    text_right(app, cam.x + cam.w - 10.0f, cam.y + 6.0f, 0.4f, COL_BRASS,
               app->game.player.camera_first_person ? "FIRST PERSON"
                                                    : "THIRD PERSON");

    paper_button(sen.x, sen.y, sen.w, sen.h, COL_BRASS);
    text_left(app, sen.x + 12.0f, sen.y + 6.0f, 0.4f, COL_INK, "TOUCH SENS");
    text_right(app, sen.x + sen.w - 10.0f, sen.y + 6.0f, 0.4f, COL_BRASS,
               kSensName[sens]);

    paper_button(inv.x, inv.y, inv.w, inv.h, COL_BRASS);
    text_left(app, inv.x + 12.0f, inv.y + 6.0f, 0.4f, COL_INK, "INVERT Y");
    text_right(app, inv.x + inv.w - 10.0f, inv.y + 6.0f, 0.4f, COL_BRASS,
               app->settings.invert_y ? "ON" : "OFF");

    paper_button(mut.x, mut.y, mut.w, mut.h, COL_BRASS);
    text_left(app, mut.x + 12.0f, mut.y + 5.0f, 0.38f, COL_INK, "MUTE AUDIO");
    text_right(app, mut.x + mut.w - 10.0f, mut.y + 5.0f, 0.38f, COL_BRASS,
               app->settings.muted ? "ON" : "OFF");

    paper_button(sv.x, sv.y, sv.w, sv.h, COL_BRASS);
    text_left(app, sv.x + 12.0f, sv.y + 5.0f, 0.38f, COL_INK,
              "SAVE & PROGRESS");
    text_right(app, sv.x + sv.w - 10.0f, sv.y + 5.0f, 0.38f, COL_BRASS, ">");

    paper_button(back.x, back.y, back.w, back.h, COL_BRASS_HI);
    text_shadow_center(app, back.x + back.w * 0.5f, back.y + 4.0f, 0.4f,
                       COL_INK, "BACK");
}

/* ------------------------------------------------------------------ */
/* results pages                                                      */
/* ------------------------------------------------------------------ */

static void draw_zone_clear(const App *app)
{
    UiRect panel = { 26.0f, 24.0f, 268.0f, 192.0f };
    UiRect travel = ui_results_travel_rect();
    UiRect title = ui_results_continue_rect();
    char buf[64];
    int secs = (int)app->result_time;
    int i;

    /* v0.3 campaign ending: all three zones cleared */
    if (app->game.world_complete) {
        UiRect play = ui_results_play_rect();
        int cleared = 0;
        int acc = app->game.total_shots > 0
                      ? (app->game.total_hits * 100) / app->game.total_shots
                      : 0;
        int total_secs = (int)app->game.total_time;

        for (i = 0; i < ZONE_COUNT; i++)
            if (app->game.world.nodes[i].cleared)
                cleared++;

        draw_background(app);
        paper_panel(panel.x, panel.y, panel.w, panel.h);
        text_woodcut_center(app, 160.0f, 26.0f, 0.75f, COL_BRASS_HI,
                            "VALLEY CLEARED");
        text_center(app, 160.0f, 56.0f, 0.36f, COL_INK,
                    "ALL THREE ZONES DELIVERED");
        divider(60.0f, 72.0f, 200.0f);

        text_left(app, 62.0f, 82.0f, 0.36f, COL_INK_SOFT, "ROT SLAIN");
        snprintf(buf, sizeof(buf), "%d", app->game.total_kills);
        text_right(app, 258.0f, 82.0f, 0.36f, COL_INK, buf);
        text_left(app, 62.0f, 96.0f, 0.36f, COL_INK_SOFT, "TIME");
        snprintf(buf, sizeof(buf), "%d:%02d", total_secs / 60,
                 total_secs % 60);
        text_right(app, 258.0f, 96.0f, 0.36f, COL_INK, buf);
        text_left(app, 62.0f, 108.0f, 0.36f, COL_INK_SOFT, "ZONES");
        snprintf(buf, sizeof(buf), "%d/%d", cleared, ZONE_COUNT);
        text_right(app, 258.0f, 108.0f, 0.36f, COL_INK, buf);
        text_left(app, 62.0f, 122.0f, 0.36f, COL_INK_SOFT, "ACCURACY");
        snprintf(buf, sizeof(buf), "%d%%", acc);
        text_right(app, 258.0f, 122.0f, 0.36f, COL_INK, buf);

        paper_button(play.x, play.y, play.w, play.h, COL_BRASS_HI);
        text_shadow_center(app, 160.0f, play.y + 7.0f, 0.42f, COL_INK,
                           "PLAY AGAIN");
        paper_button(title.x, title.y, title.w, title.h, COL_BRASS);
        text_shadow_center(app, 160.0f, title.y + 4.0f, 0.36f, COL_INK,
                           "TITLE");
        return;
    }

    draw_background(app);
    paper_panel(panel.x, panel.y, panel.w, panel.h);
    text_woodcut_center(app, 160.0f, 32.0f, 0.85f, COL_BRASS_HI,
                        "ZONE CLEARED");
    text_center(app, 160.0f, 62.0f, 0.42f, COL_INK,
                grave_zone_name(app->game.zone));
    divider(60.0f, 78.0f, 200.0f);

    snprintf(buf, sizeof(buf), "TIME        %d:%02d", secs / 60, secs % 60);
    text_center(app, 160.0f, 88.0f, 0.38f, COL_INK_SOFT, buf);
    snprintf(buf, sizeof(buf), "ROT SLAIN   %d", app->result_kills);
    text_center(app, 160.0f, 102.0f, 0.38f, COL_INK_SOFT, buf);
    snprintf(buf, sizeof(buf), "PICKINGS   +%dC  +%dS  +%dK",
             app->result_gain_coins, app->result_gain_sinew,
             app->result_gain_charms);
    text_center(app, 160.0f, 116.0f, 0.38f, COL_BRASS, buf);
    snprintf(buf, sizeof(buf), "BANK       %dC  %dS  %dK",
             app->result_coins, app->result_sinew,
             app->game.material_bone_charms);
    text_center(app, 160.0f, 130.0f, 0.38f, COL_INK_SOFT, buf);

    paper_button(travel.x, travel.y, travel.w, travel.h, COL_BRASS_HI);
    text_shadow_center(app, travel.x + travel.w * 0.5f, travel.y + 8.0f, 0.5f,
                       COL_INK, "TRAVEL ON");

    paper_button(title.x, title.y, title.w, title.h, COL_BRASS);
    text_shadow_center(app, title.x + title.w * 0.5f, title.y + 4.0f, 0.4f,
                       COL_INK, "TITLE");
}
static void draw_game_over(const App *app)
{
    UiRect panel = { 26.0f, 24.0f, 268.0f, 192.0f };
    UiRect retry = ui_gameover_retry_rect();
    char buf[64];
    int secs = (int)app->result_time;

    draw_background(app);
    paper_panel(panel.x, panel.y, panel.w, panel.h);
    text_woodcut_center(app, 160.0f, 32.0f, 0.68f, COL_BLOOD_HI,
                        "YOU JOINED THE ROT");
    text_center(app, 160.0f, 66.0f, 0.42f, COL_INK,
                "ASHEN MILL CLAIMS ANOTHER");
    divider(60.0f, 80.0f, 200.0f);

    snprintf(buf, sizeof(buf), "ROT SLAIN   %d", app->result_kills);
    text_center(app, 160.0f, 90.0f, 0.42f, COL_INK_SOFT, buf);
    snprintf(buf, sizeof(buf), "SURVIVED    %d:%02d", secs / 60, secs % 60);
    text_center(app, 160.0f, 104.0f, 0.42f, COL_INK_SOFT, buf);
    snprintf(buf, sizeof(buf), "OLD COINS %d    ROT SINEW %d",
             app->result_coins, app->result_sinew);
    text_center(app, 160.0f, 118.0f, 0.4f, COL_INK_SOFT, buf);

    paper_button(retry.x, retry.y, retry.w, retry.h, COL_BLOOD);
    text_shadow_center(app, 160.0f, retry.y + 7.0f, 0.5f, COL_INK,
                       "TAP TO RETRY");

    text_center(app, 160.0f, 198.0f, 0.34f, COL_INK_SOFT, "START = TITLE");
}

/* ------------------------------------------------------------------ */
/* public entry points                                                */
/* ------------------------------------------------------------------ */

void ui_draw(const App *app)
{
    if (app == NULL || app->text_buf == NULL)
        return;

    g_z = 0.5f;
    g_dx = 0.0f;
    C2D_TextBufClear(app->text_buf);

    switch (app->page) {
    case APP_PAGE_TITLE:
        draw_title(app);
        break;
    case APP_PAGE_TITLE_CONFIRM:
        draw_title_confirm_page(app);
        break;
    case APP_PAGE_PAUSE:
        draw_pause(app);
        break;
    case APP_PAGE_SETTINGS:
        draw_settings(app);
        break;
    case APP_PAGE_SAVE:
        draw_save_page(app);
        break;
    case APP_PAGE_WARDROBE:
        draw_wardrobe_page(app);
        break;
    case APP_PAGE_ZONE_CLEAR:
        draw_zone_clear(app);
        break;
    case APP_PAGE_GAME_OVER:
        draw_game_over(app);
        break;
    case APP_PAGE_HUD:
    default:
        draw_hud_base(app);
        if (app->travel_confirm_zone >= 0)
            draw_travel_dialog(app);
        draw_toast(app);
        break;
    }

    g_z = 0.5f;
    g_dx = 0.0f;
}

bool ui_hit_test(const App *app, int px, int py, int *action, int *arg)
{
    int local_arg = 0;
    int local_action = UI_ACTION_NONE;
    bool hit = false;
    int i;

    if (app == NULL)
        return false;
    if (px < 0 || px >= UI_SCREEN_W || py < 0 || py >= UI_SCREEN_H)
        return false;

    switch (app->page) {
    case APP_PAGE_TITLE:
        if (app->has_save) {
            if (rect_hit(ui_title_continue_rect(), px, py))
                local_action = UI_ACTION_TITLE_CONTINUE;
            else if (rect_hit(ui_title_newgame_rect(), px, py))
                local_action = UI_ACTION_TITLE_NEW_GAME;
            hit = (local_action != UI_ACTION_NONE);
        } else {
            local_action = UI_ACTION_START; /* tap anywhere starts a run */
            hit = true;
        }
        break;

    case APP_PAGE_TITLE_CONFIRM:
        if (rect_hit(ui_title_confirm_yes_rect(), px, py))
            local_action = UI_ACTION_NEW_GAME_YES;
        else if (rect_hit(ui_title_confirm_no_rect(), px, py))
            local_action = UI_ACTION_CANCEL;
        hit = (local_action != UI_ACTION_NONE);
        break;

    case APP_PAGE_HUD:
        /* travel confirmation dialog owns the whole screen while open */
        if (app->travel_confirm_zone >= 0) {
            if (rect_hit(ui_travel_yes_rect(), px, py))
                local_action = UI_ACTION_TRAVEL_YES;
            else if (rect_hit(ui_travel_no_rect(), px, py))
                local_action = UI_ACTION_TRAVEL_NO;
            hit = (local_action != UI_ACTION_NONE);
            break;
        }
        /* the status strip and tab bar are always live */
        if (rect_hit(ui_pause_rect(), px, py)) {
            local_action = UI_ACTION_PAUSE;
            hit = true;
            break;
        }
        for (i = 0; i < APP_TAB_COUNT; i++) {
            if (rect_hit(ui_tab_rect(i), px, py)) {
                local_action = UI_ACTION_TAB;
                local_arg = i;
                hit = true;
                break;
            }
        }
        if (hit)
            break;
        /* page content is only interactive once the slide has finished */
        if (app->tab_anim > 0.0f)
            break;
        if (app->tab == APP_TAB_PACK) {
            for (i = 0; i < WEAPON_COUNT; i++) {
                if (rect_hit(ui_pack_weapon_rect(i), px, py)) {
                    local_action = UI_ACTION_WEAPON;
                    local_arg = i;
                    hit = true;
                    break;
                }
            }
        } else if (app->tab == APP_TAB_WORLD) {
            for (i = 0; i < UI_WORLD_NODE_COUNT; i++) {
                if (rect_hit(ui_world_node_rect(i), px, py)) {
                    local_action = UI_ACTION_WORLD_NODE;
                    local_arg = i;
                    hit = true;
                    break;
                }
            }
        } else if (app->tab == APP_TAB_SHOP) {
            if (!app->game.world.in_zone) {
                for (i = 0; i < SHOP_ITEM_COUNT; i++) {
                    if (rect_hit(ui_shop_item_rect(i), px, py)) {
                        local_action = UI_ACTION_SHOP_ITEM;
                        local_arg = i;
                        hit = true;
                        break;
                    }
                }
            }
        }
        break;

    case APP_PAGE_PAUSE:
        if (rect_hit(ui_pause_resume_rect(), px, py)) {
            local_action = UI_ACTION_RESUME;
        } else if (rect_hit(ui_pause_restart_rect(), px, py)) {
            local_action = UI_ACTION_RESTART;
        } else if (rect_hit(ui_pause_settings_rect(), px, py)) {
            local_action = UI_ACTION_SETTINGS;
        } else if (rect_hit(ui_pause_wardrobe_rect(), px, py)) {
            local_action = UI_ACTION_WARDROBE;
        } else if (rect_hit(ui_pause_quit_rect(), px, py)) {
            local_action = UI_ACTION_QUIT_TITLE;
        }
        hit = (local_action != UI_ACTION_NONE);
        break;

    case APP_PAGE_SETTINGS:
        if (rect_hit(ui_settings_camera_rect(), px, py)) {
            local_action = UI_ACTION_TOGGLE_CAMERA;
        } else if (rect_hit(ui_settings_sens_rect(), px, py)) {
            local_action = UI_ACTION_CYCLE_SENS;
        } else if (rect_hit(ui_settings_invert_rect(), px, py)) {
            local_action = UI_ACTION_TOGGLE_INVERT;
        } else if (rect_hit(ui_settings_mute_rect(), px, py)) {
            local_action = UI_ACTION_TOGGLE_MUTE;
        } else if (rect_hit(ui_settings_save_rect(), px, py)) {
            local_action = UI_ACTION_SETTINGS_SAVE;
        } else if (rect_hit(ui_settings_back_rect(), px, py)) {
            local_action = UI_ACTION_SETTINGS_BACK;
        }
        hit = (local_action != UI_ACTION_NONE);
        break;

    case APP_PAGE_SAVE:
        if (rect_hit(ui_save_erase_rect(), px, py)) {
            local_action = UI_ACTION_SAVE_ERASE;
        } else if (rect_hit(ui_save_back_rect(), px, py)) {
            local_action = UI_ACTION_SAVE_BACK;
        }
        hit = (local_action != UI_ACTION_NONE);
        break;

    case APP_PAGE_WARDROBE:
        if (rect_hit(ui_wardrobe_reload_rect(), px, py)) {
            local_action = UI_ACTION_WARDROBE_RELOAD;
        } else if (rect_hit(ui_wardrobe_back_rect(), px, py)) {
            local_action = UI_ACTION_WARDROBE_BACK;
        }
        hit = (local_action != UI_ACTION_NONE);
        break;

    case APP_PAGE_ZONE_CLEAR:
        if (app->game.world_complete) {
            if (rect_hit(ui_results_play_rect(), px, py)) {
                local_action = UI_ACTION_ENDING_PLAY;
                hit = true;
            } else if (rect_hit(ui_results_continue_rect(), px, py)) {
                local_action = UI_ACTION_CONTINUE;
                hit = true;
            }
        } else if (rect_hit(ui_results_travel_rect(), px, py)) {
            local_action = UI_ACTION_TRAVEL_ON;
            hit = true;
        } else if (rect_hit(ui_results_continue_rect(), px, py)) {
            local_action = UI_ACTION_CONTINUE;
            hit = true;
        }
        break;

    case APP_PAGE_GAME_OVER:
        if (rect_hit(ui_gameover_retry_rect(), px, py)) {
            local_action = UI_ACTION_RETRY;
            hit = true;
        }
        break;

    default:
        break;
    }

    if (action != NULL)
        *action = local_action;
    if (arg != NULL)
        *arg = local_arg;
    return hit;
}