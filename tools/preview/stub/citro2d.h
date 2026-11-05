/*
 * Gravepowder - HOST PREVIEW SHIM for <citro2d.h>
 *
 * NOT part of the 3DS build.  The real source/app.c + source/ui.c draw the
 * bottom screen through this API; tools/preview/plat_host.c implements these
 * entry points by recording every primitive into a draw list, which
 * tools/preview/render_ui.py replays into a PNG layout mockup and checks.
 *
 * This mirrors the subset of citro2d that the UI actually uses, preserving the
 * real call signatures and flag values.
 */
#ifndef GRAVEPOWDER_PREVIEW_SHIM_CITRO2D_H
#define GRAVEPOWDER_PREVIEW_SHIM_CITRO2D_H

#include <stdbool.h>
#include <stddef.h>

#include <3ds.h>
#include <citro3d.h>

typedef struct C2D_TextBuf_s *C2D_TextBuf;

typedef struct {
    C2D_TextBuf buf;
    const char *str;
    int dummy;
} C2D_Text;

#define C2D_DEFAULT_MAX_OBJECTS 4096

/* Text flags. */
#define C2D_AtBaseline (1u << 0)
#define C2D_WithColor (1u << 1)
#define C2D_AlignRight (1u << 2)
#define C2D_AlignCenter (2u << 2)
#define C2D_WordWrap (1u << 4)

bool C2D_Init(size_t maxObjects);
void C2D_Fini(void);
void C2D_Prepare(void);
void C2D_Flush(void);
void C2D_SceneBegin(C3D_RenderTarget *target);
void C2D_TargetClear(C3D_RenderTarget *target, u32 color);

C2D_TextBuf C2D_TextBufNew(size_t maxGlyphs);
void C2D_TextBufDelete(C2D_TextBuf buf);
void C2D_TextBufClear(C2D_TextBuf buf);
void C2D_TextParse(C2D_Text *text, C2D_TextBuf buf, const char *str);
void C2D_TextOptimize(const C2D_Text *text);
void C2D_TextGetDimensions(const C2D_Text *text, float scaleX, float scaleY,
                           float *outWidth, float *outHeight);
void C2D_DrawText(const C2D_Text *text, u32 flags, float x, float y, float z,
                  float scaleX, float scaleY, ...);

bool C2D_DrawRectSolid(float x, float y, float z, float w, float h, u32 clr);
bool C2D_DrawTriangle(float x0, float y0, u32 clr0, float x1, float y1, u32 clr1,
                      float x2, float y2, u32 clr2, float depth);
bool C2D_DrawLine(float x0, float y0, u32 clr0, float x1, float y1, u32 clr1,
                  float thickness, float depth);
void C2D_DrawCircleSolid(float x, float y, float z, float radius, u32 clr);

static inline u32 C2D_Color32(u8 r, u8 g, u8 b, u8 a)
{
    return (u32)(r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24));
}

#endif /* GRAVEPOWDER_PREVIEW_SHIM_CITRO2D_H */
