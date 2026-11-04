/*
 * Gravepowder - application/UI contract
 *
 * source/app.c owns the scene state machine (title -> zone -> pause -> results),
 * translates 3DS input into GraveInput, steps the core and dispatches audio
 * events.  source/ui.c draws the bottom-screen HUD with citro2d; main.c just
 * runs the frame loop.
 */
#ifndef GRAVEPOWDER_APP_H
#define GRAVEPOWDER_APP_H

#include "grave.h"

typedef struct App App;

void app_init(void);
void app_shutdown(void);

/* Processes input and advances the simulation by one fixed 1/60 step. */
void app_step(void);

/* Draws the 2D bottom screen (HUD / menus).  Call inside the C3D frame. */
void app_draw_bottom(void);

/* Read-only handles so the renderer can draw the current world. */
const GraveGame *app_game(void);
App *app_state(void);

/* Convenience for the frame loop: true while the app wants to keep running. */
bool app_running(void);
void app_request_exit(void);

/* Polls hid state exactly once per video frame before the fixed-step loop:
 * latches edge-triggered buttons (A/B/X/Y/START) and the touchscreen drag/tap
 * so several 1/60 steps in one frame do not repeat them. */
void app_poll_input(void);

#endif /* GRAVEPOWDER_APP_H */
