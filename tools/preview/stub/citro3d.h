/*
 * Gravepowder - HOST PREVIEW SHIM for <citro3d.h>
 *
 * NOT part of the 3DS build.  Only the types that include/render3d.h and the
 * app/UI layer touch are provided; the actual 3D world is out of scope for the
 * preview harness (it verifies the bottom-screen UI, not PICA rendering).
 */
#ifndef GRAVEPOWDER_PREVIEW_SHIM_CITRO3D_H
#define GRAVEPOWDER_PREVIEW_SHIM_CITRO3D_H

typedef struct {
    float m[16];
} C3D_Mtx;

typedef struct C3D_RenderTarget C3D_RenderTarget;

#endif /* GRAVEPOWDER_PREVIEW_SHIM_CITRO3D_H */
