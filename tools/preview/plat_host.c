/*
 * Gravepowder - host preview backend (tools/preview/plat_host.c)
 *
 * NOT part of the 3DS build.  Two jobs:
 *   1. synthetic hid input for source/app.c (the driver writes the keyboard
 *      globals / touch globals directly, exactly like pressing a button);
 *   2. citro2d draw-call recorder for source/ui.c.  Every primitive becomes one
 *      line in a text draw list; tools/preview/render_ui.py replays the list
 *      into a 320x240 PNG mockup and runs the layout/legibility checks.
 *
 * The render3d API is stubbed to a dummy bottom target so app_draw_bottom()
 * works; the 3D world itself is out of scope (the harness verifies the UI).
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include "plat_host.h"
#include "render3d.h"

/* ------------------------------------------------------------------ */
/* synthetic hid                                                       */
/* ------------------------------------------------------------------ */

u32 host_keys_held;
u32 host_keys_down;
int host_touch_active;
int host_touch_px;
int host_touch_py;

void hidScanInput(void)
{
}

u32 hidKeysHeld(void)
{
    return host_keys_held;
}

u32 hidKeysDown(void)
{
    return host_keys_down;
}

void hidCircleRead(circlePosition *out)
{
    if (out != NULL) {
        out->dx = 0;
        out->dy = 0;
    }
}

void hidTouchRead(touchPosition *out)
{
    if (out != NULL) {
        out->px = (u16)host_touch_px;
        out->py = (u16)host_touch_py;
    }
}

bool aptMainLoop(void)
{
    return true;
}

void host_input_clear_edges(void)
{
    host_keys_down = 0;
}

/* v0.4 motion input: the host harness is a still rig.  Zero angular rates keep
 * the app's gyro aim path inert (the core consumes GraveInput.gyro_*). */
static angularRate g_host_gyro_rate;

void HIDUSER_EnableGyroscope(void)
{
}

void HIDUSER_DisableGyroscope(void)
{
}

float HIDUSER_GetGyroscopeRawToDpsCoefficient(void)
{
    return 1.0f;
}

void hidGyroRead(angularRate *rate)
{
    if (rate != NULL)
        *rate = g_host_gyro_rate;
}

/* ------------------------------------------------------------------ */
/* draw-list recorder                                                  */
/* ------------------------------------------------------------------ */

#define HOST_DRAW_MAX 32768
#define HOST_TEXT_MAX 112

enum {
    HOST_DRAW_RECT = 0,
    HOST_DRAW_LINE = 1,
    HOST_DRAW_TRI = 2,
    HOST_DRAW_CIRCLE = 3,
    HOST_DRAW_TEXT = 4
};

typedef struct {
    int type;
    float x, y, w, h;       /* rect x/y/w/h, circle x/y/r, text x/y/scale */
    float x1, y1, x2, y2;   /* line/triangle endpoints                    */
    float thick;
    u32 clr;
    int flags;
    char text[HOST_TEXT_MAX];
} HostDraw;

static HostDraw g_draws[HOST_DRAW_MAX];
static int g_draw_count;
static int g_draw_overflow;

int host_c2d_draw_calls;
int host_c2d_text_calls;
int host_c2d_textbuf_new_calls;
int host_text_overflow;

void host_drawlist_begin(void)
{
    g_draw_count = 0;
    g_draw_overflow = 0;
    host_c2d_draw_calls = 0;
    host_c2d_text_calls = 0;
    host_text_overflow = 0;
}

int host_drawlist_count(void)
{
    return g_draw_count;
}

int host_drawlist_capacity(void)
{
    return HOST_DRAW_MAX;
}

static HostDraw *next_draw(int type)
{
    HostDraw *d;

    if (g_draw_count >= HOST_DRAW_MAX) {
        ++g_draw_overflow;
        return NULL;
    }
    d = &g_draws[g_draw_count++];
    memset(d, 0, sizeof(*d));
    d->type = type;
    return d;
}

int host_drawlist_save(const char *path)
{
    FILE *f = fopen(path, "w");
    int i;

    if (f == NULL)
        return -1;

    for (i = 0; i < g_draw_count; ++i) {
        const HostDraw *d = &g_draws[i];

        switch (d->type) {
        case HOST_DRAW_RECT:
            fprintf(f, "RECT %.2f %.2f %.2f %.2f %u\n", (double)d->x,
                    (double)d->y, (double)d->w, (double)d->h, d->clr);
            break;
        case HOST_DRAW_LINE:
            fprintf(f, "LINE %.2f %.2f %.2f %.2f %.2f %u\n", (double)d->x,
                    (double)d->y, (double)d->x1, (double)d->y1,
                    (double)d->thick, d->clr);
            break;
        case HOST_DRAW_TRI:
            fprintf(f, "TRI %.2f %.2f %.2f %.2f %.2f %.2f %u\n", (double)d->x,
                    (double)d->y, (double)d->x1, (double)d->y1, (double)d->x2,
                    (double)d->y2, d->clr);
            break;
        case HOST_DRAW_CIRCLE:
            fprintf(f, "CIRCLE %.2f %.2f %.2f %u\n", (double)d->x, (double)d->y,
                    (double)d->w, d->clr);
            break;
        case HOST_DRAW_TEXT:
            fprintf(f, "TEXT %.2f %.2f %.3f %d %u %s\n", (double)d->x,
                    (double)d->y, (double)d->w, d->flags, d->clr, d->text);
            break;
        default:
            break;
        }
    }

    if (g_draw_overflow > 0)
        fprintf(f, "# WARNING: %d primitive(s) dropped (draw list full)\n",
                g_draw_overflow);

    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* citro2d shim                                                        */
/* ------------------------------------------------------------------ */

struct C2D_TextBuf_s {
    int dummy;
};

static struct C2D_TextBuf_s g_text_buf;
static C3D_RenderTarget *g_dummy_target;

bool C2D_Init(size_t maxObjects)
{
    (void)maxObjects;
    return true;
}

void C2D_Fini(void)
{
}

void C2D_Prepare(void)
{
}

void C2D_Flush(void)
{
}

void C2D_SceneBegin(C3D_RenderTarget *target)
{
    (void)target;
}

void C2D_TargetClear(C3D_RenderTarget *target, u32 color)
{
    HostDraw *d;

    (void)target;
    host_c2d_draw_calls++;
    d = next_draw(HOST_DRAW_RECT);
    if (d != NULL) {
        d->x = 0.0f;
        d->y = 0.0f;
        d->w = 320.0f;
        d->h = 240.0f;
        d->clr = color;
    }
}

C2D_TextBuf C2D_TextBufNew(size_t maxGlyphs)
{
    (void)maxGlyphs;
    host_c2d_textbuf_new_calls++;
    return &g_text_buf;
}

void C2D_TextBufDelete(C2D_TextBuf buf)
{
    (void)buf;
}

void C2D_TextBufClear(C2D_TextBuf buf)
{
    (void)buf;
}

void C2D_TextParse(C2D_Text *text, C2D_TextBuf buf, const char *str)
{
    if (text == NULL)
        return;
    text->buf = buf;
    text->str = (str != NULL) ? str : "";
    text->dummy = 0;
}

void C2D_TextOptimize(const C2D_Text *text)
{
    (void)text;
}

/* Approximate citro2d system-font metrics: the real font is proportional, so
 * this is only a layout estimate (render_ui.py uses a truetype approximation
 * for the same reason). */
void C2D_TextGetDimensions(const C2D_Text *text, float scaleX, float scaleY,
                           float *outWidth, float *outHeight)
{
    size_t len = (text != NULL && text->str != NULL) ? strlen(text->str) : 0u;

    if (outWidth != NULL)
        *outWidth = (float)len * 8.0f * scaleX;
    if (outHeight != NULL)
        *outHeight = 12.0f * scaleY;
}

#if defined(__clang__)
/* citro2d's real signature ends its named parameters in floats; reading the
 * colour vararg must still start the list after the last named parameter.
 * The language technically dislikes float default-promotion here, hence the
 * suppression (this is a host shim, not shipped code). */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wvarargs"
#endif
void C2D_DrawText(const C2D_Text *text, u32 flags, float x, float y, float z,
                  float scaleX, float scaleY, ...)
{
    HostDraw *d;
    va_list args;
    u32 clr = 0xFFFFFFFFu;

    (void)z;
    (void)scaleY;
    host_c2d_text_calls++;
    host_c2d_draw_calls++;

    if ((flags & C2D_WithColor) != 0u) {
        va_start(args, scaleY);
        clr = va_arg(args, u32);
        va_end(args);
    }

    d = next_draw(HOST_DRAW_TEXT);
    if (d != NULL) {
        const char *str = (text != NULL && text->str != NULL) ? text->str : "";

        d->x = x;
        d->y = y;
        d->w = scaleX;   /* render_ui.py reads the text scale from here */
        d->flags = (int)flags;
        d->clr = clr;
        if (strlen(str) >= (size_t)HOST_TEXT_MAX) {
            host_text_overflow++;
            strncpy(d->text, str, (size_t)HOST_TEXT_MAX - 1u);
            d->text[HOST_TEXT_MAX - 1] = '\0';
        } else {
            strcpy(d->text, str);
        }
    }
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

bool C2D_DrawRectSolid(float x, float y, float z, float w, float h, u32 clr)
{
    HostDraw *d;

    (void)z;
    host_c2d_draw_calls++;
    d = next_draw(HOST_DRAW_RECT);
    if (d != NULL) {
        d->x = x;
        d->y = y;
        d->w = w;
        d->h = h;
        d->clr = clr;
    }
    return true;
}

bool C2D_DrawTriangle(float x0, float y0, u32 clr0, float x1, float y1, u32 clr1,
                      float x2, float y2, u32 clr2, float depth)
{
    HostDraw *d;

    (void)clr1;
    (void)clr2;
    (void)depth;
    host_c2d_draw_calls++;
    d = next_draw(HOST_DRAW_TRI);
    if (d != NULL) {
        d->x = x0;
        d->y = y0;
        d->x1 = x1;
        d->y1 = y1;
        d->x2 = x2;
        d->y2 = y2;
        d->clr = clr0;
    }
    return true;
}

bool C2D_DrawLine(float x0, float y0, u32 clr0, float x1, float y1, u32 clr1,
                  float thickness, float depth)
{
    HostDraw *d;

    (void)clr1;
    (void)depth;
    host_c2d_draw_calls++;
    d = next_draw(HOST_DRAW_LINE);
    if (d != NULL) {
        d->x = x0;
        d->y = y0;
        d->x1 = x1;
        d->y1 = y1;
        d->thick = thickness;
        d->clr = clr0;
    }
    return true;
}

void C2D_DrawCircleSolid(float x, float y, float z, float radius, u32 clr)
{
    HostDraw *d;

    (void)z;
    host_c2d_draw_calls++;
    d = next_draw(HOST_DRAW_CIRCLE);
    if (d != NULL) {
        d->x = x;
        d->y = y;
        d->w = radius;
        d->clr = clr;
    }
}

/* ------------------------------------------------------------------ */
/* render3d shim: only the bottom target is needed by app_draw_bottom() */
/* ------------------------------------------------------------------ */

C3D_RenderTarget *r3d_top_target(void)
{
    static int dummy_top;
    return (C3D_RenderTarget *)&dummy_top;
}

C3D_RenderTarget *r3d_bottom_target(void)
{
    return (C3D_RenderTarget *)&g_dummy_target;
}

void r3d_init(void)
{
}

void r3d_fini(void)
{
}

void r3d_begin_frame(void)
{
}

void r3d_end_frame(void)
{
}

void r3d_draw_world(const GraveGame *game, float dt)
{
    (void)game;
    (void)dt;
}

void r3d_draw_box(const C3D_Mtx *view, float x, float y, float z, float sx,
                  float sy, float sz, float r, float g, float b)
{
    (void)view;
    (void)x;
    (void)y;
    (void)z;
    (void)sx;
    (void)sy;
    (void)sz;
    (void)r;
    (void)g;
    (void)b;
}

void r3d_set_stereo(float iod, bool enabled)
{
    (void)iod;
    (void)enabled;
}

void r3d_set_hero_rig(const struct HeroRig *rig)
{
    (void)rig;
}

void r3d_frame_stats(unsigned *draw_calls, unsigned *vertex_count)
{
    if (draw_calls != NULL)
        *draw_calls = 0u;
    if (vertex_count != NULL)
        *vertex_count = 0u;
}

void r3d_frame_stats_ext(R3dFrameStats *out)
{
    if (out != NULL)
        memset(out, 0, sizeof(*out));
}

unsigned r3d_mesh_bytes(void)
{
    return 0u;
}

unsigned r3d_mesh_vertices(void)
{
    return 0u;
}

bool r3d_set_hero_model(const struct GpmModel *model)
{
    (void)model;
    return false;
}

bool r3d_reload_hero_model(void)
{
    return false;
}

const struct GpmModel *r3d_hero_model(void)
{
    return NULL;
}

/* v0.3 model slots: the wardrobe page prints the provenance strings.  The
 * preview harness never loads real models, so every slot reports the
 * procedural fallback (the same strings the real renderer uses). */
bool r3d_reload_all_models(void)
{
    return true;
}

const char *r3d_model_source(R3dModelSlot slot)
{
    (void)slot;
    return "procedural fallback";
}

