/*
 * Gravepowder - HOST PREVIEW SHIM for <3ds.h>
 *
 * This header is NOT part of the 3DS build.  It exists so the platform-coupled
 * front end (source/app.c, source/ui.c) can be compiled and run on a PC by
 * tools/preview: it provides the fixed-width types, the synthetic hid input
 * state and the button bits that the real libctru would provide.
 *
 * The preview driver (tools/preview/main.c) writes host_keys_held /
 * host_keys_down / host_touch_* directly to script input.
 */
#ifndef GRAVEPOWDER_PREVIEW_SHIM_3DS_H
#define GRAVEPOWDER_PREVIEW_SHIM_3DS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

/* libctru input structs (subset). */
typedef struct {
    s16 dx;
    s16 dy;
} circlePosition;

typedef struct {
    u16 px;
    u16 py;
} touchPosition;

/* Synthetic hid state, filled in by the preview driver. */
extern u32 host_keys_held;
extern u32 host_keys_down;
extern int host_touch_active;
extern int host_touch_px;
extern int host_touch_py;

void hidScanInput(void);
u32 hidKeysHeld(void);
u32 hidKeysDown(void);
void hidCircleRead(circlePosition *out);
void hidTouchRead(touchPosition *out);
bool aptMainLoop(void);

/* Button bits (same values as libctru for the keys the game uses). */
#define KEY_A (1u << 0)
#define KEY_B (1u << 1)
#define KEY_SELECT (1u << 2)
#define KEY_START (1u << 3)
#define KEY_RIGHT (1u << 4)
#define KEY_LEFT (1u << 5)
#define KEY_UP (1u << 6)
#define KEY_DOWN (1u << 7)
#define KEY_R (1u << 8)
#define KEY_L (1u << 9)
#define KEY_X (1u << 10)
#define KEY_Y (1u << 11)
#define KEY_TOUCH (1u << 12)

#endif /* GRAVEPOWDER_PREVIEW_SHIM_3DS_H */
