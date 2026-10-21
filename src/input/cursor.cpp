#include "input/cursor.h"

namespace input {
namespace {

CursorSource *g_source = nullptr;

}  // namespace

void set_cursor_source(CursorSource *source)
{
    g_source = source;
}

bool cursor_pos(float *x, float *y)
{
    return g_source != nullptr && g_source->screen_pos(x, y);
}

bool cursor_picked_pos(float position[3])
{
    return g_source != nullptr && g_source->picked_pos(position);
}

}  // namespace input
