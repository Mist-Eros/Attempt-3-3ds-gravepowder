/*
 * Gravepowder - input helpers (portable)
 *
 * Converts raw platform input into the GraveInput struct the core consumes.
 * The 3DS backend reads hid state; the host tools can synthesise it.
 */
#include <math.h>
#include <string.h>

#include "grave.h"

void grave_input_reset(GraveInput *input)
{
    if (input == NULL)
        return;
    memset(input, 0, sizeof(*input));
}

static float clamp_unit(float v)
{
    if (v > 1.0f) return 1.0f;
    if (v < -1.0f) return -1.0f;
    return v;
}

void grave_input_stick(GraveInput *input, int32_t dx, int32_t dy)
{
    if (input == NULL)
        return;
    input->move_x = clamp_unit((float)dx / 156.0f);
    input->move_y = clamp_unit((float)dy / 156.0f);
}

void grave_input_touch(GraveInput *input, int32_t dx, int32_t dy)
{
    if (input == NULL)
        return;
    input->look_x += (float)dx / 160.0f;
    input->look_y += (float)dy / 160.0f;
}
