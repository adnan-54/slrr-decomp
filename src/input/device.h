// 16-record device table (notes/14 §1-4). Record offsets are the exe's:
// B+0x30 type, +0x34 button count, +0x38 control count, +0x44 last values,
// +0x144 control ids, +0x244 control names[16], +0x644 raw state,
// +0x754 device name.
#pragma once
#include <stdint.h>

#include <string>

#include "input/backend.h"

namespace input {

class Backend;
class HotkeyTable;

struct DeviceRecord {
    int type = 0;
    int button_count = 0;
    int control_count = 0;
    float last[64] = {};           // absolute axis cache / activeAxis history
    uint32_t ids[64] = {};
    char names[64][16] = {};
    RawState raw;
    char device_name[64] = {};
};

class DeviceTable {
  public:
    static constexpr int kMaxDevices = 16;
    static constexpr int kMaxControls = 64;

    // Enumerate descriptors into records (the exe does this once at startup).
    void init(Backend &backend);
    // One frame: poll interval, raw states, mouse absolute axis integration.
    void poll(Backend &backend);

    int count() const { return count_; }
    // 0x544830 dispatch: keyboard / mouse / joystick reader.
    float get_axis(int device, int index);
    // 0x5447e0: 1 for analog controls on mouse/joystick, 0 for buttons/keys.
    int kind(int device, int index) const;
    int type(int device) const;
    // 0x544670: index of the single active control, -1 for none/multiple.
    int active_axis(int device);
    // 0x544750: empty for an invalid device/index.
    std::string axis_name(int device, int index) const;
    // 0x5448a0: null for an invalid device.
    const char *device_name(int device) const;

  private:
    float mouse_axis(int device, int index);
    float joystick_axis(int device, int index) const;

    DeviceRecord devices_[kMaxDevices];
    int count_ = 0;
    float mouse_sensitivity_ = 0.5f;  // DAT_00794b70
    float poll_ms_ = 0.0f;            // DAT_00794b74 (mouse velocity divisor)
};

// Process-wide tables, like the exe globals 0x78d0f0 (devices) / 0x5fe478 (hotkeys).
DeviceTable &devices();
HotkeyTable &hotkeys();
// Null until installed; install_backend runs devices().init(*b) (exe: 0x565fc0 at startup).
Backend *backend();
void install_backend(Backend *b);

}  // namespace input
