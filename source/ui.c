/*
 * Gravepowder - bottom screen UI (citro2d)
 *
 * Old-paper / weathered Western look: bone, brass and ink, blood-red accents
 * for danger.  Everything is drawn with citro2d primitives (no textures), all
 * layout lives in one place and the clickable rects are exposed through the
 * ui_*_rect() accessors in ui.h.
 *
 * The UI never mutates game state: it only reads App/GraveGame and returns
 * UI_ACTION_* ids to app.c, which applies them.
 */
#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "ui.h"

/* ------------------------------------------------------------------ */
/* palette: weathered paper / bone / brass / ink / blood              */
/* ------------------------------------------------------------------ */

#define CLR(r, g, b, a)                                                       \
    ((u32)(((u32)(r) & 0xFF) | (((u32)(g) & 0xFF) << 8) |                     \
           (((u32)(b) & 0xFF) << 16) | (((u32)(a) & 0xFF) << 24)))

static const u32 COL_BG        = CLR(0x16, 0x11, 0x0D, 0xFF); /* charred wood  */
static const u32 COL_BG2       = CLR(0x22, 0x1A, 0x12, 0xFF); /* raised timber */
static const u32 COL_GRAIN     = CLR(0x0E, 0x0A, 0x07, 0xFF);
static const u32 COL_STAIN     = CLR(0x00, 0x00, 0x00, 0x40);

static const u32 COL_PAPER     = CLR(0xE3, 0xD2, 0xA8, 0xFF); /* parchment     */
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

/* ------------------------------------------------------------------ */
/* basic drawing helpers                                              */
/* ------------------------------------------------------------------ */

/* Depth layer for the current page: 0.5 = normal HUD/menu content.  The
 * pause/settings overlays raise it to 1.0 so their panels and text draw above
 * everything the HUD already put down (citro2d depth test is per-primitive). */
static float g_z = 0.5f;

static void fill_rect(float x, float y, float w, float h, u32 clr)
{
    C2D_DrawRectSolid(x, y, g_z, w, h, clr);
}

static void frame_rect(float x, float y, float w, float h, float t, u32 clr)
{
    fill_rect(x, y, w, t, clr);
    fill_rect(x, y + h - t, w, t, clr);
    fill_rect(x, y, t, h, clr);
    fill_rect(x + w - t, y, t, h, clr);
}

static void text_left(const App *app, float x, float y, float scale, u32 clr,
                      const char *s)
{
    C2D_Text t;

    C2D_TextParse(&t, app->text_buf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, g_z, scale, scale, clr);
}

static void text_center(const App *app, float cx, float y, float scale, u32 clr,
                        const char *s)
{
    C2D_Text t;

    C2D_TextParse(&t, app->text_buf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_AlignCenter | C2D_WithColor, cx, y, g_z, scale, scale,
                 clr);
}

static void text_right(const App *app, float rightX, float y, float scale,
                       u32 clr, const char *s)
{
    C2D_Text t;

    C2D_TextParse(&t, app->text_buf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_AlignRight | C2D_WithColor, rightX, y, g_z, scale,
                 scale, clr);
}

/* Cheap one-pixel-ish drop shadow for headline text. */
static void text_shadow_center(const App *app, float cx, float y, float scale,
                               u32 fill, const char *s)
{
    text_center(app, cx + 1.5f, y + 1.5f, scale, COL_INK, s);
    text_center(app, cx, y, scale, fill, s);
}

/* Woodcut-style headline: shadow + 4-way ink outline + fill. */
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
    /* corner studs */
    fill_rect(x + 4.0f, y + 4.0f, 3.0f, 3.0f, COL_BRASS_HI);
    fill_rect(x + w - 7.0f, y + 4.0f, 3.0f, 3.0f, COL_BRASS_HI);
    fill_rect(x + 4.0f, y + h - 7.0f, 3.0f, 3.0f, COL_BRASS_HI);
    fill_rect(x + w - 7.0f, y + h - 7.0f, 3.0f, 3.0f, COL_BRASS_HI);
}

/* Dark timber panel (HUD instrument housing). */
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

/* Brass divider with a pair of end hash marks. */
static void divider(float x, float y, float w)
{
    fill_rect(x, y, w, 1.0f, COL_BRASS);
    fill_rect(x, y - 1.0f, 3.0f, 3.0f, COL_BRASS_HI);
    fill_rect(x + w - 3.0f, y - 1.0f, 3.0f, 3.0f, COL_BRASS_HI);
}

/* Old-paper button.  `accent` picks the colour of the left tab. */
static void paper_button(float x, float y, float w, float h, u32 accent)
{
    paper_panel(x, y, w, h);
    fill_rect(x + 5.0f, y + 5.0f, 4.0f, h - 10.0f, accent);
    fill_rect(x + 5.0f, y + 5.0f, w - 10.0f, 1.0f,
              CLR(0xFF, 0xFF, 0xFF, 0x30));
}

/* Horizontal meter with an ink frame. */
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

/* Weathered background shared by every page. */
static void draw_background(const App *app)
{
    int i;

    fill_rect(0.0f, 0.0f, 320.0f, 240.0f, COL_BG);
    /* timber grain */
    for (i = 0; i < 320; i += 16) {
        fill_rect((float)i, 0.0f, 1.0f, 240.0f, COL_GRAIN);
        fill_rect((float)i + 8.0f, 0.0f, 1.0f, 240.0f,
                  CLR(0x2A, 0x20, 0x16, 0x60));
    }
    /* old stains */
    C2D_DrawCircleSolid(40.0f, 34.0f, g_z, 22.0f, COL_STAIN);
    C2D_DrawCircleSolid(282.0f, 96.0f, g_z, 16.0f, COL_STAIN);
    C2D_DrawCircleSolid(238.0f, 196.0f, g_z, 30.0f, COL_STAIN);
    /* vignette */
    for (i = 0; i < 5; i++) {
        u32 a = CLR(0, 0, 0, 40 - (u32)i * 7);
        float t = 3.0f;
        float o = (float)i * t;
        fill_rect(0.0f, o, 320.0f, t, a);
        fill_rect(0.0f, 240.0f - o - t, 320.0f, t, a);
        fill_rect(o, 0.0f, t, 240.0f, a);
        fill_rect(320.0f - o - t, 0.0f, t, 240.0f, a);
    }
    (void)app;
}

/* Small bone/brass caption row used on the title and results pages. */
static void caption_line(const App *app, float y, const char *left,
                         const char *right)
{
    if (left != NULL)
        text_left(app, 14.0f, y, 0.42f, COL_BONE_DIM, left);
    if (right != NULL)
        text_right(app, 306.0f, y, 0.42f, COL_BONE_DIM, right);
}

/* ------------------------------------------------------------------ */
/* layout accessors                                                   */
/* ------------------------------------------------------------------ */

UiRect ui_pause_rect(void)
{
    UiRect r = { 252.0f, 5.0f, 62.0f, 24.0f };
    return r;
}

UiRect ui_weapon_slot_rect(int index)
{
    UiRect r;

    if (index < 0)
        index = 0;
    if (index >= WEAPON_COUNT)
        index = WEAPON_COUNT - 1;
    r.x = 6.0f + (float)index * 104.0f;
    r.y = 176.0f;
    r.w = 100.0f;
    r.h = 32.0f;
    return r;
}

UiRect ui_title_start_rect(void)
{
    UiRect r = { 62.0f, 184.0f, 196.0f, 32.0f };
    return r;
}

UiRect ui_pause_resume_rect(void)
{
    UiRect r = { 56.0f, 72.0f, 208.0f, 30.0f };
    return r;
}

UiRect ui_pause_restart_rect(void)
{
    UiRect r = { 56.0f, 106.0f, 208.0f, 30.0f };
    return r;
}

UiRect ui_pause_settings_rect(void)
{
    UiRect r = { 56.0f, 140.0f, 208.0f, 30.0f };
    return r;
}

UiRect ui_pause_quit_rect(void)
{
    UiRect r = { 56.0f, 174.0f, 208.0f, 30.0f };
    return r;
}

UiRect ui_settings_back_rect(void)
{
    UiRect r = { 76.0f, 180.0f, 168.0f, 26.0f };
    return r;
}

UiRect ui_settings_camera_rect(void)
{
    UiRect r = { 48.0f, 58.0f, 224.0f, 26.0f };
    return r;
}

UiRect ui_settings_sens_rect(void)
{
    UiRect r = { 48.0f, 88.0f, 224.0f, 26.0f };
    return r;
}

UiRect ui_settings_invert_rect(void)
{
    UiRect r = { 48.0f, 118.0f, 224.0f, 26.0f };
    return r;
}

UiRect ui_settings_mute_rect(void)
{
    UiRect r = { 48.0f, 148.0f, 224.0f, 26.0f };
    return r;
}

UiRect ui_results_continue_rect(void)
{
    UiRect r = { 76.0f, 164.0f, 168.0f, 30.0f };
    return r;
}

UiRect ui_gameover_retry_rect(void)
{
    UiRect r = { 76.0f, 160.0f, 168.0f, 32.0f };
    return r;
}

static bool rect_hit(UiRect r, int px, int py)
{
    float fx = (float)px;
    float fy = (float)py;

    return fx >= r.x && fx < r.x + r.w && fy >= r.y && fy < r.y + r.h;
}

/* ------------------------------------------------------------------ */
/* HUD                                                                */
/* ------------------------------------------------------------------ */

static void draw_radar(const App *app, float cx, float cy, float radius)
{
    const GraveGame *g = &app->game;
    const Player *p = &g->player;
    float yaw = p->transform.yaw;
    float cy_ = cosf(yaw);
    float sy_ = sinf(yaw);
    float range = 26.0f;
    float scale = (radius - 5.0f) / range;
    int i;

    /* housing */
    C2D_DrawCircleSolid(cx, cy, g_z, radius + 2.0f, COL_INK);
    C2D_DrawCircleSolid(cx, cy, g_z, radius, COL_BRASS);
    C2D_DrawCircleSolid(cx, cy, g_z, radius - 1.5f, CLR(0x14, 0x12, 0x0C, 0xFF));
    /* cross-hairs */
    C2D_DrawLine(cx - radius + 4.0f, cy, CLR(0x3A, 0x33, 0x22, 0xFF), cx + radius - 4.0f,
                 cy, CLR(0x3A, 0x33, 0x22, 0xFF), 1.0f, g_z);
    C2D_DrawLine(cx, cy - radius + 4.0f, CLR(0x3A, 0x33, 0x22, 0xFF), cx,
                 cy + radius - 4.0f, CLR(0x3A, 0x33, 0x22, 0xFF), 1.0f, g_z);
    C2D_DrawCircleSolid(cx, cy, g_z, 2.0f, COL_BRASS_HI);

    /* pickups: tiny brass flecks */
    for (i = 0; i < MAX_PICKUPS; i++) {
        const Pickup *pk = &g->pickups[i];
        float dx, dz, sx, sz, px, py, len;

        if (!pk->active)
            continue;
        dx = pk->pos.x - p->transform.pos.x;
        dz = pk->pos.z - p->transform.pos.z;
        sx = dx * cy_ - dz * sy_;
        sz = dx * sy_ + dz * cy_;
        px = cx + sx * scale;
        py = cy - sz * scale;
        len = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
        if (len > radius - 4.0f) {
            px = cx + (px - cx) * (radius - 4.0f) / len;
            py = cy + (py - cy) * (radius - 4.0f) / len;
        }
        C2D_DrawCircleSolid(px, py, g_z, 1.5f, COL_BRASS_HI);
    }

    /* zombies: blips, colour by type */
    for (i = 0; i < MAX_ZOMBIES; i++) {
        const Zombie *z = &g->zombies[i];
        float dx, dz, sx, sz, px, py, len;
        u32 color;
        float dot;

        if (!z->active || z->state == ZOMBIE_DEAD)
            continue;
        dx = z->pos.x - p->transform.pos.x;
        dz = z->pos.z - p->transform.pos.z;
        sx = dx * cy_ - dz * sy_;
        sz = dx * sy_ + dz * cy_;
        px = cx + sx * scale;
        py = cy - sz * scale;
        len = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
        if (len > radius - 4.0f) {
            px = cx + (px - cx) * (radius - 4.0f) / len;
            py = cy + (py - cy) * (radius - 4.0f) / len;
        }
        if (z->type == ZOMBIE_RUNNER) {
            color = COL_BLOOD_HI;
            dot = 2.0f;
        } else if (z->type == ZOMBIE_BRUTE) {
            color = COL_BRASS_HI;
            dot = 3.0f;
        } else {
            color = COL_ROT;
            dot = 2.0f;
        }
        C2D_DrawCircleSolid(px, py, g_z, dot + 1.0f, COL_INK);
        C2D_DrawCircleSolid(px, py, g_z, dot, color);
    }

    /* player: brass arrow pointing up (forward) */
    C2D_DrawTriangle(cx, cy - 6.0f, COL_BRASS_HI, cx - 4.0f, cy + 4.0f, COL_BRASS,
                     cx + 4.0f, cy + 4.0f, COL_BRASS, g_z);
    C2D_DrawTriangle(cx, cy - 3.0f, COL_HILITE, cx - 2.0f, cy + 3.0f, COL_BRASS,
                     cx + 2.0f, cy + 3.0f, COL_BRASS, g_z);
}

static void draw_weapon_slots(const App *app)
{
    const GraveGame *g = &app->game;
    int i;

    for (i = 0; i < WEAPON_COUNT; i++) {
        UiRect r = ui_weapon_slot_rect(i);
        const WeaponSpec *spec = grave_weapon_spec((WeaponId)i);
        char buf[24];
        int ammo = g->player.ammo[i];
        int mag = spec != NULL ? spec->magazine : 1;
        bool selected = (g->player.weapon == (WeaponId)i);
        u32 accent = selected ? COL_BRASS_HI : COL_BRASS;
        u32 label = COL_INK;

        paper_panel(r.x, r.y, r.w, r.h);

        if (selected) {
            frame_rect(r.x - 1.0f, r.y - 1.0f, r.w + 2.0f, r.h + 2.0f, 2.0f,
                       COL_BLOOD_HI);
            fill_rect(r.x + 6.0f, r.y + 4.0f, r.w - 12.0f, 2.0f, COL_BRASS_HI);
        }
        fill_rect(r.x + 8.0f, r.y + 6.0f, 3.0f, 3.0f, accent);

        if (spec != NULL && spec->name != NULL) {
            text_shadow_center(app, r.x + r.w * 0.5f + 1.0f, r.y + 2.0f, 0.42f,
                               label, spec->name);
        }
        snprintf(buf, sizeof(buf), "%d / %d", ammo, mag);
        text_left(app, r.x + 10.0f, r.y + 15.0f, 0.44f,
                  ammo > 0 ? COL_INK : COL_BLOOD, buf);
        snprintf(buf, sizeof(buf), "%d", i + 1);
        text_right(app, r.x + r.w - 8.0f, r.y + 15.0f, 0.4f, COL_INK_SOFT, buf);
    }
}

static void draw_hud(const App *app)
{
    const GraveGame *g = &app->game;
    const Player *p = &g->player;
    char buf[64];
    const WeaponSpec *spec = grave_weapon_spec(p->weapon);
    float hp_frac = p->max_hp > 0.0f ? p->hp / p->max_hp : 0.0f;
    float stam_frac = p->stamina > 1.5f ? p->stamina / 100.0f : p->stamina;
    int mag = spec != NULL ? spec->magazine : 1;
    int ammo = p->ammo[p->weapon];
    int i;

    /* ---------------- top bar: zone / quest / pause ---------------- */
    fill_rect(0.0f, 0.0f, 320.0f, 34.0f, COL_BG2);
    fill_rect(0.0f, 32.0f, 320.0f, 2.0f, COL_BRASS);
    fill_rect(0.0f, 34.0f, 320.0f, 1.0f, COL_INK);
    for (i = 0; i < 320; i += 16)
        fill_rect((float)i, 0.0f, 1.0f, 34.0f, COL_GRAIN);

    text_left(app, 10.0f, 3.0f, 0.46f, COL_BRASS_HI, grave_zone_name(g->zone));
    text_left(app, 10.0f, 19.0f, 0.4f, COL_BONE_DIM, "CLEAR THE ROT");

    {
        UiRect pr = ui_pause_rect();

        paper_button(pr.x, pr.y, pr.w, pr.h, COL_BRASS_HI);
        fill_rect(pr.x + 10.0f, pr.y + 8.0f, 5.0f, 9.0f, COL_INK);
        fill_rect(pr.x + 18.0f, pr.y + 8.0f, 5.0f, 9.0f, COL_INK);
        text_left(app, pr.x + 27.0f, pr.y + 6.0f, 0.38f, COL_INK, "PAUSE");
    }

    /* ---------------- health / stamina ---------------- */
    paper_panel(6.0f, 40.0f, 150.0f, 56.0f);
    fill_rect(14.0f, 46.0f, 3.0f, 3.0f, COL_BLOOD);
    text_left(app, 20.0f, 43.0f, 0.4f, COL_INK_SOFT, "HEALTH");
    bar(14.0f, 55.0f, 134.0f, 15.0f, hp_frac,
        hp_frac > 0.35f ? COL_BLOOD_HI : COL_BLOOD,
        CLR(0x6A, 0x59, 0x3E, 0xFF));
    snprintf(buf, sizeof(buf), "%d / %d", (int)(p->hp + 0.5f),
             (int)(p->max_hp + 0.5f));
    text_center(app, 81.0f, 56.0f, 0.45f, COL_BONE, buf);

    text_left(app, 14.0f, 73.0f, 0.34f, COL_INK_SOFT, "STAMINA");
    bar(14.0f, 83.0f, 134.0f, 7.0f, stam_frac, COL_STAM,
        CLR(0x6A, 0x59, 0x3E, 0xFF));

    /* damage flash over the health panel */
    if (app->hit_flash > 0.01f) {
        u32 a = CLR(0xD6, 0x30, 0x20,
                    (u32)(90.0f * app->hit_flash));
        fill_rect(6.0f, 40.0f, 150.0f, 3.0f, a);
        fill_rect(6.0f, 93.0f, 150.0f, 3.0f, a);
        fill_rect(6.0f, 40.0f, 3.0f, 56.0f, a);
        fill_rect(153.0f, 40.0f, 3.0f, 56.0f, a);
    }

    /* ---------------- ammo ---------------- */
    paper_panel(162.0f, 40.0f, 152.0f, 56.0f);
    text_shadow_center(app, 238.0f, 42.0f, 0.38f, COL_INK,
                       spec != NULL ? spec->name : "GUN");
    snprintf(buf, sizeof(buf), "%d / %d", ammo, mag);
    text_shadow_center(app, 238.0f, 54.0f, 0.72f,
                       ammo > 0 ? COL_INK : COL_BLOOD, buf);

    if (p->reload_timer > 0.0f && spec != NULL && spec->reload_time > 0.0f) {
        float frac = 1.0f - p->reload_timer / spec->reload_time;

        bar(170.0f, 78.0f, 136.0f, 6.0f, frac, COL_BRASS_HI,
            CLR(0x6A, 0x59, 0x3E, 0xFF));
        text_center(app, 238.0f, 82.0f, 0.34f, COL_INK_SOFT, "RELOADING");
    } else if (ammo <= 0) {
        text_center(app, 238.0f, 82.0f, 0.34f, COL_BLOOD, "EMPTY - B TO RELOAD");
    } else {
        text_center(app, 238.0f, 82.0f, 0.34f, COL_INK_SOFT, "B: RELOAD");
    }

    /* ---------------- radar ---------------- */
    dark_panel(6.0f, 100.0f, 104.0f, 72.0f);
    text_center(app, 58.0f, 102.0f, 0.36f, COL_BRASS_HI, "SCANNER");
    draw_radar(app, 58.0f, 138.0f, 24.0f);

    /* ---------------- quest tracker ---------------- */
    paper_panel(114.0f, 100.0f, 200.0f, 72.0f);
    text_left(app, 122.0f, 105.0f, 0.44f, COL_INK, "CLEAR THE ROT");
    snprintf(buf, sizeof(buf), "%d/%d", g->quest.progress, g->quest.target);
    text_right(app, 306.0f, 105.0f, 0.44f,
               g->quest.progress >= g->quest.target ? COL_BLOOD : COL_INK, buf);
    bar(122.0f, 128.0f, 184.0f, 8.0f,
        g->quest.target > 0 ? (float)g->quest.progress / (float)g->quest.target
                            : 0.0f,
        COL_BRASS_HI, CLR(0x6A, 0x59, 0x3E, 0xFF));

    C2D_DrawCircleSolid(126.0f, 146.0f, g_z, 4.0f, COL_BRASS);
    C2D_DrawCircleSolid(126.0f, 146.0f, g_z, 2.0f, COL_BRASS_HI);
    text_left(app, 134.0f, 140.0f, 0.4f, COL_INK_SOFT, "OLD COINS");
    snprintf(buf, sizeof(buf), "%d", g->material_coins);
    text_right(app, 240.0f, 140.0f, 0.4f, COL_INK, buf);

    C2D_DrawCircleSolid(126.0f, 160.0f, g_z, 4.0f, COL_BLOOD);
    C2D_DrawCircleSolid(126.0f, 160.0f, g_z, 2.0f, COL_BLOOD_HI);
    text_left(app, 134.0f, 154.0f, 0.4f, COL_INK_SOFT, "ROT SINEW");
    snprintf(buf, sizeof(buf), "%d", g->material_sinew);
    text_right(app, 240.0f, 154.0f, 0.4f, COL_INK, buf);

    {
        int secs = (int)g->zone_time;

        snprintf(buf, sizeof(buf), "TIME %d:%02d", secs / 60, secs % 60);
        text_right(app, 306.0f, 140.0f, 0.4f, COL_INK_SOFT, buf);
        snprintf(buf, sizeof(buf), "KILLS %d", g->zombies_killed);
        text_right(app, 306.0f, 154.0f, 0.4f, COL_INK_SOFT, buf);
    }

    /* ---------------- weapon slots + hints ---------------- */
    draw_weapon_slots(app);

    text_center(app, 160.0f, 211.0f, 0.42f, COL_BONE_DIM,
                "PAD MOVE   DRAG AIM   A FIRE   B RELOAD");
    text_center(app, 160.0f, 222.0f, 0.42f, COL_BONE_DIM,
                "X SWAP   Y CAMERA   L SPRINT   START PAUSE");

    /* ---------------- pause overlay is a separate page ---------------- */
}

/* ------------------------------------------------------------------ */
/* title page                                                         */
/* ------------------------------------------------------------------ */

static void draw_title(const App *app)
{
    float pulse = 0.75f + 0.25f * sinf(app->time * 3.2f);
    int i;

    draw_background(app);

    /* hanging sign board behind the logo */
    fill_rect(20.0f, 26.0f, 280.0f, 62.0f, COL_INK);
    fill_rect(22.0f, 28.0f, 276.0f, 58.0f, CLR(0x3A, 0x2B, 0x1C, 0xFF));
    for (i = 0; i < 276; i += 20)
        fill_rect(22.0f + (float)i, 28.0f, 1.0f, 58.0f, COL_GRAIN);
    C2D_DrawCircleSolid(28.0f, 34.0f, g_z, 3.0f, COL_BRASS);
    C2D_DrawCircleSolid(292.0f, 34.0f, g_z, 3.0f, COL_BRASS);
    C2D_DrawCircleSolid(28.0f, 80.0f, g_z, 3.0f, COL_BRASS);
    C2D_DrawCircleSolid(292.0f, 80.0f, g_z, 3.0f, COL_BRASS);

    text_woodcut_center(app, 160.0f, 32.0f, 1.3f, COL_BRASS_HI,
                        "GRAVEPOWDER");
    divider(50.0f, 76.0f, 220.0f);

    text_center(app, 160.0f, 10.0f, 0.4f, COL_BONE_DIM,
                "A 3DS ZOMBIE SHOOTER");

    /* red zone ribbon */
    fill_rect(96.0f, 94.0f, 128.0f, 20.0f, COL_BLOOD);
    fill_rect(96.0f, 94.0f, 128.0f, 2.0f, COL_BLOOD_HI);
    fill_rect(96.0f, 112.0f, 128.0f, 2.0f, COL_INK);
    C2D_DrawTriangle(96.0f, 94.0f, COL_BLOOD, 88.0f, 104.0f, COL_BLOOD,
                     96.0f, 114.0f, COL_BLOOD, g_z);
    C2D_DrawTriangle(224.0f, 94.0f, COL_BLOOD, 232.0f, 104.0f, COL_BLOOD,
                     224.0f, 114.0f, COL_BLOOD, g_z);
    text_shadow_center(app, 160.0f, 96.0f, 0.5f, COL_BONE, "ASHEN MILL");

    /* control hints on a dark board */
    dark_panel(40.0f, 124.0f, 240.0f, 50.0f);
    text_center(app, 160.0f, 129.0f, 0.42f, COL_BONE_DIM,
                "PAD MOVE    TOUCH DRAG TO AIM");
    text_center(app, 160.0f, 142.0f, 0.42f, COL_BONE_DIM,
                "A FIRE    B RELOAD    X SWAP WEAPON");
    text_center(app, 160.0f, 155.0f, 0.42f, COL_BONE_DIM,
                "Y CAMERA    L SPRINT    START PAUSE");

    /* start prompt */
    {
        UiRect r = ui_title_start_rect();
        u32 fill = pulse > 0.9f ? COL_HILITE : COL_BRASS_HI;

        paper_button(r.x, r.y, r.w, r.h, fill);
        text_shadow_center(app, 160.0f, r.y + 7.0f, 0.55f, COL_INK,
                           "PRESS A OR TAP START");
    }

    divider(40.0f, 222.0f, 240.0f);
    caption_line(app, 224.0f, "v0.1  -  ASHEN MILL", "3DS HOMEBREW");
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
    UiRect qu = ui_pause_quit_rect();
    char buf[48];

    /* keep the HUD visible behind a dark wash so the player keeps context */
    draw_hud(app);
    g_z = 1.0f;  /* overlay layer: stays above every HUD primitive */
    fill_rect(0.0f, 0.0f, 320.0f, 240.0f, CLR(0x08, 0x06, 0x04, 0xB4));

    paper_panel(panel.x, panel.y, panel.w, panel.h);
    text_woodcut_center(app, 160.0f, 28.0f, 0.8f, COL_BRASS_HI, "PAUSED");
    text_center(app, 160.0f, 56.0f, 0.4f, COL_INK_SOFT,
                "THE DEAD ARE PATIENT");

    paper_button(rr.x, rr.y, rr.w, rr.h, COL_BRASS_HI);
    text_shadow_center(app, 160.0f, rr.y + 6.0f, 0.55f, COL_INK, "RESUME");

    paper_button(ra.x, ra.y, ra.w, ra.h, COL_BRASS);
    text_shadow_center(app, 160.0f, ra.y + 6.0f, 0.5f, COL_INK, "RESTART ZONE");

    paper_button(se.x, se.y, se.w, se.h, COL_BRASS);
    text_shadow_center(app, 160.0f, se.y + 6.0f, 0.5f, COL_INK, "SETTINGS");

    paper_button(qu.x, qu.y, qu.w, qu.h, COL_BLOOD);
    text_shadow_center(app, 160.0f, qu.y + 6.0f, 0.5f, COL_INK, "QUIT TO TITLE");

    snprintf(buf, sizeof(buf), "ZONE %s", grave_zone_name(g->zone));
    text_center(app, 160.0f, 208.0f, 0.36f, COL_INK_SOFT, buf);
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
    UiRect back = ui_settings_back_rect();
    int sens = app->touch_sens;

    if (sens < 0 || sens >= APP_SENS_COUNT)
        sens = APP_SENS_MED;

    draw_hud(app);
    g_z = 1.0f;  /* overlay layer */
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
               app->invert_y ? "ON" : "OFF");

    paper_button(mut.x, mut.y, mut.w, mut.h, COL_BRASS);
    text_left(app, mut.x + 12.0f, mut.y + 6.0f, 0.4f, COL_INK, "MUTE AUDIO");
    text_right(app, mut.x + mut.w - 10.0f, mut.y + 6.0f, 0.4f, COL_BRASS,
               app->muted ? "ON" : "OFF");

    paper_button(back.x, back.y, back.w, back.h, COL_BRASS_HI);
    text_shadow_center(app, back.x + back.w * 0.5f, back.y + 5.0f, 0.45f,
                       COL_INK, "BACK");

    text_center(app, 160.0f, 208.0f, 0.32f, COL_INK_SOFT,
                "TAP A ROW TO CHANGE - START RESUMES");
}

/* ------------------------------------------------------------------ */
/* results pages                                                      */
/* ------------------------------------------------------------------ */

static void draw_zone_clear(const App *app)
{
    UiRect panel = { 26.0f, 24.0f, 268.0f, 192.0f };
    UiRect con = ui_results_continue_rect();
    char buf[64];
    int secs = (int)app->result_time;

    draw_background(app);
    paper_panel(panel.x, panel.y, panel.w, panel.h);
    text_woodcut_center(app, 160.0f, 34.0f, 0.9f, COL_BRASS_HI, "ZONE CLEARED");
    text_center(app, 160.0f, 66.0f, 0.45f, COL_INK, "ASHEN MILL");
    divider(60.0f, 82.0f, 200.0f);

    snprintf(buf, sizeof(buf), "TIME        %d:%02d", secs / 60, secs % 60);
    text_center(app, 160.0f, 92.0f, 0.42f, COL_INK_SOFT, buf);
    snprintf(buf, sizeof(buf), "ROT SLAIN   %d", app->result_kills);
    text_center(app, 160.0f, 106.0f, 0.42f, COL_INK_SOFT, buf);
    snprintf(buf, sizeof(buf), "OLD COINS   %d", app->result_coins);
    text_center(app, 160.0f, 120.0f, 0.42f, COL_INK_SOFT, buf);
    snprintf(buf, sizeof(buf), "ROT SINEW   %d", app->result_sinew);
    text_center(app, 160.0f, 134.0f, 0.42f, COL_INK_SOFT, buf);

    paper_button(con.x, con.y, con.w, con.h, COL_BRASS_HI);
    text_shadow_center(app, 160.0f, con.y + 6.0f, 0.45f, COL_INK,
                       "TAP TO CONTINUE");
    text_center(app, 160.0f, 198.0f, 0.34f, COL_INK_SOFT, "BACK TO TITLE");
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
    C2D_TextBufClear(app->text_buf);

    switch (app->page) {
    case APP_PAGE_TITLE:
        draw_title(app);
        break;
    case APP_PAGE_PAUSE:
        draw_pause(app);
        break;
    case APP_PAGE_SETTINGS:
        draw_settings(app);
        break;
    case APP_PAGE_ZONE_CLEAR:
        draw_zone_clear(app);
        break;
    case APP_PAGE_GAME_OVER:
        draw_game_over(app);
        break;
    case APP_PAGE_HUD:
    default:
        draw_background(app);
        draw_hud(app);
        break;
    }
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
        /* forgiving: the whole title screen starts the game */
        local_action = UI_ACTION_START;
        hit = true;
        break;

    case APP_PAGE_HUD:
        if (rect_hit(ui_pause_rect(), px, py)) {
            local_action = UI_ACTION_PAUSE;
            hit = true;
            break;
        }
        for (i = 0; i < WEAPON_COUNT; i++) {
            if (rect_hit(ui_weapon_slot_rect(i), px, py)) {
                local_action = UI_ACTION_WEAPON;
                local_arg = i;
                hit = true;
                break;
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
        } else if (rect_hit(ui_settings_back_rect(), px, py)) {
            local_action = UI_ACTION_SETTINGS_BACK;
        }
        hit = (local_action != UI_ACTION_NONE);
        break;

    case APP_PAGE_ZONE_CLEAR:
        if (rect_hit(ui_results_continue_rect(), px, py)) {
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
