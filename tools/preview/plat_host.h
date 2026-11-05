/*
 * Gravepowder - host preview backend (tools/preview)
 *
 * Implements the citro2d/libctru shim surface declared in tools/preview/stub/
 * by recording every bottom-screen primitive the real source/ui.c emits into a
 * draw list, and by exposing scriptable hid input for source/app.c.
 *
 * The recorded list is the single source of truth for tools/preview/render_ui.py
 * (PNG mockups + layout/legibility checks + baseline comparison).  This file is
 * NOT part of the 3DS build.
 */
#ifndef GRAVEPOWDER_PREVIEW_PLAT_HOST_H
#define GRAVEPOWDER_PREVIEW_PLAT_HOST_H

#include <stdbool.h>
#include <stdint.h>

/* ---- draw-list capture ---------------------------------------------- */

/* Start a new capture (clears the draw list and the draw/text counters). */
void host_drawlist_begin(void);

/* Number of recorded primitives / text draws since host_drawlist_begin(). */
int host_drawlist_count(void);
int host_drawlist_capacity(void);

/* Write the draw list as text: one primitive per line, e.g.
 *   RECT x y w h abgr
 *   LINE x0 y0 x1 y1 thickness abgr
 *   TRI x0 y0 x1 y1 x2 y2 abgr
 *   CIRCLE x y radius abgr
 *   TEXT x y scale flags abgr text...
 * Returns 0 on success, -1 if the file could not be opened. */
int host_drawlist_save(const char *path);

/* Counters (valid between begin and the next begin). */
extern int host_c2d_draw_calls;
extern int host_c2d_text_calls;
extern int host_c2d_textbuf_new_calls;

/* True when a text draw was longer than the record buffer (truncated). */
extern int host_text_overflow;

/* ---- scripted input -------------------------------------------------- */
/* host_keys_held / host_keys_down / host_touch_* are extern in stub/3ds.h and
 * defined in plat_host.c; the driver writes them directly. */

/* Clear the "pressed this frame" edges (called once per simulated frame). */
void host_input_clear_edges(void);

#endif /* GRAVEPOWDER_PREVIEW_PLAT_HOST_H */
