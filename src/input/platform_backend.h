// The input backend used by the game: DirectInput8 (keyboard, mouse, attached
// joysticks) on Windows; an empty synthetic backend elsewhere (tests feed their
// own SyntheticBackend). window is the HWND for SetCooperativeLevel.
#pragma once

namespace input {

class Backend;

Backend *make_platform_backend(void *window);

}  // namespace input
