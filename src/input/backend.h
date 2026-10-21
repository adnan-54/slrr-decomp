// Input backend abstraction (notes/14 §1-2).
//
// The device table is fed by a backend: DirectInput8 on Windows
// (input/platform_backend.cpp) or SyntheticBackend here for tests/headless.
// A backend exposes the raw states the exe reads through IDirectInputDevice8
// and the buffered keyboard event stream used by Input.lastKey.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

namespace input {

// Device record type tags (record +0x30).
enum DeviceType { kKeyboard = 1, kMouse = 2, kJoystick = 3 };

// DirectInput control ids: axes 2/4/8/0x10/0x20/0x40, sliders 0x80/0x81,
// buttons 0x1000+n, POV pairs 0x2000+i (x) and 0x3000+i (y).
constexpr uint32_t kButtonBase = 0x1000;
constexpr uint32_t kPovBase = 0x2000;

// Raw device state union (record +0x644): keyboard 256 bytes, mouse
// DIMOUSESTATE (3 deltas + 4 buttons), joystick DIJOYSTATE2 (8 axes,
// 4 POVs, 128 buttons).
struct RawState {
    uint8_t keys[256] = {};
    int32_t mouse[3] = {};
    uint8_t mouse_buttons[4] = {};
    int32_t axes[8] = {};
    int32_t pov[4] = {};
    uint8_t buttons[128] = {};
};

// One control of a device: id in the exe's scheme plus its DirectInput name.
struct ControlDesc {
    uint32_t id = 0;
    std::string name;
};

struct DeviceDesc {
    int type = 0;
    std::string name;  // device_name (record +0x754), max 0x40 bytes
    std::vector<ControlDesc> controls;
};

class Backend {
  public:
    virtual ~Backend() {}
    // Fill one descriptor per device, in table order. Called once at startup.
    virtual void enumerate(std::vector<DeviceDesc> *devices) = 0;
    // Copy device's current raw state; false clears the state in the table.
    virtual bool poll(int device, RawState *raw) = 0;
    // Milliseconds since the previous poll (mouse velocity divisor).
    virtual float poll_interval_ms() = 0;
    // One buffered keyboard event as (ansi << 16) | scan; 0 when empty.
    virtual uint32_t read_key() { return 0; }
};

// Feeds states directly; the Linux/test backend.
class SyntheticBackend : public Backend {
  public:
    struct Device {
        DeviceDesc desc;
        RawState raw;
        bool available = true;
    };

    std::vector<Device> devices;
    std::vector<uint32_t> key_events;  // FIFO consumed by read_key
    float interval_ms = 16.0f;

    void enumerate(std::vector<DeviceDesc> *out) override;
    bool poll(int device, RawState *raw) override;
    float poll_interval_ms() override { return interval_ms; }
    uint32_t read_key() override;
};

// The exe's two-branch unit clamp: NaN falls into the +1 branch, exactly like
// the cmp/jcc sequences in the mouse integration and the axis filter.
inline float clamp_unit(float v)
{
    if (v <= 1.0f)
        return v <= -1.0f ? -1.0f : v;
    return 1.0f;
}

}  // namespace input
