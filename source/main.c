/*
 * Gravepowder - main frame loop
 *
 * Owned by the app/integration layer.  Everything game-specific lives below:
 *   include/app.h     scene state machine, input mapping, audio dispatch
 *   include/render3d.h  citro3d world renderer (top screen)
 *   include/grave.h   portable simulation core
 *
 * This file only provides the 3DS process lifetime: GPU init, fixed 60 Hz
 * accumulator, per-frame draw order, and clean shutdown.
 */
#include <3ds.h>

#include "app.h"
#include "render3d.h"

#define FIXED_DT (1.0f / 60.0f)
#define MAX_STEPS_PER_FRAME 4   /* never simulate more than 4 ticks per frame */
#define MAX_FRAME_DT 0.25f     /* hitch clamp (seconds)                    */
#define MAX_DRAW_DT 0.1f       /* clamp the renderer's animation delta     */

int main(int argc, char **argv)
{
    double previous_time;
    double accumulator = 0.0;

    (void)argc;
    (void)argv;

    /* The renderer brings up gfx + citro3d and creates both targets.  The app
     * layer owns citro2d (C2D_Init happens in app_init), so order matters. */
    r3d_init();
    app_init();

    previous_time = osGetTime() / 1000.0;

    while (aptMainLoop() && app_running()) {
        double now;
        double frame_time;
        int steps = 0;

        /* Input is latched exactly once per video frame so edge-triggered
         * buttons and touch taps cannot repeat when several 1/60 steps run. */
        hidScanInput();
        app_poll_input();

        now = osGetTime() / 1000.0;
        frame_time = now - previous_time;
        previous_time = now;

        if (frame_time > (double)MAX_FRAME_DT)
            frame_time = (double)MAX_FRAME_DT;
        if (frame_time < 0.0)
            frame_time = 0.0;

        accumulator += frame_time;
        while (accumulator >= (double)FIXED_DT && steps < MAX_STEPS_PER_FRAME) {
            app_step();
            accumulator -= (double)FIXED_DT;
            ++steps;
        }
        if (steps >= MAX_STEPS_PER_FRAME)
            accumulator = 0.0; /* dropped frames: do not spiral */

        /* Draw the world and the bottom screen inside one GPU frame.  The
         * renderer re-establishes its own 3D GPU state every frame, so 2D state
         * from the previous frame cannot leak into the world draw. */
        {
            float draw_dt = (float)frame_time;

            if (draw_dt > MAX_DRAW_DT)
                draw_dt = MAX_DRAW_DT;

            r3d_begin_frame();
            r3d_draw_world(app_game(), draw_dt);
            app_draw_bottom();
            r3d_end_frame();
        }
    }

    app_shutdown();
    r3d_fini();
    return 0;
}
