// MouseCursor hooks (0x4df270/0x4df3b0/0x4def10/0x4df100). The exe resolves the
// script object's "cursor" resource and reads info id 2 (screen x,y) or 0x3b
// (picked world xyz). The render layer provides both; getPos leaves z alone.
#pragma once

namespace input {

class CursorSource {
  public:
    virtual ~CursorSource() {}
    virtual bool screen_pos(float *x, float *y) = 0;
    virtual bool picked_pos(float position[3]) = 0;
};

// Null when the renderer is absent, like the exe's missing resource error.
void set_cursor_source(CursorSource *source);
bool cursor_pos(float *x, float *y);
bool cursor_picked_pos(float position[3]);

}  // namespace input
