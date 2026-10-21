#include "input/device.h"

#include <math.h>
#include <string.h>

#include <cmath>

namespace input {

// 256-entry keyboard control name table (PTR_DAT_005e0098). Unused scan codes
// are "?" in the exe; printed scancode names match the shipped strings.
static const char *key_name(int scan)
{
    switch (scan) {
    case 1: return "Escape";
    case 2: return "1";
    case 3: return "2";
    case 4: return "3";
    case 5: return "4";
    case 6: return "5";
    case 7: return "6";
    case 8: return "7";
    case 9: return "8";
    case 10: return "9";
    case 11: return "0";
    case 12: return "-";
    case 13: return "=";
    case 14: return "Backspace";
    case 15: return "Tab";
    case 16: return "Q";
    case 17: return "W";
    case 18: return "E";
    case 19: return "R";
    case 20: return "T";
    case 21: return "Y";
    case 22: return "U";
    case 23: return "I";
    case 24: return "O";
    case 25: return "P";
    case 26: return "[";
    case 27: return "]";
    case 28: return "Return";
    case 29: return "LCtrl";
    case 30: return "A";
    case 31: return "S";
    case 32: return "D";
    case 33: return "F";
    case 34: return "G";
    case 35: return "H";
    case 36: return "J";
    case 37: return "K";
    case 38: return "L";
    case 39: return ";";
    case 40: return "'";
    case 41: return "`";
    case 42: return "LShift";
    case 43: return "\\";
    case 44: return "Z";
    case 45: return "X";
    case 46: return "C";
    case 47: return "V";
    case 48: return "B";
    case 49: return "N";
    case 50: return "M";
    case 51: return ",";
    case 52: return ".";
    case 53: return "/";
    case 54: return "RShift";
    case 55: return "Num*";
    case 56: return "LAlt";
    case 57: return "Space";
    case 58: return "CapsLock";
    case 59: return "F1";
    case 60: return "F2";
    case 61: return "F3";
    case 62: return "F4";
    case 63: return "F5";
    case 64: return "F6";
    case 65: return "F7";
    case 66: return "F8";
    case 67: return "F9";
    case 68: return "F10";
    case 69: return "NumLock";
    case 70: return "ScrollLock";
    case 71: return "Num7";
    case 72: return "Num8";
    case 73: return "Num9";
    case 74: return "Num-";
    case 75: return "Num4";
    case 76: return "Num5";
    case 77: return "Num6";
    case 78: return "Num+";
    case 79: return "Num1";
    case 80: return "Num2";
    case 81: return "Num3";
    case 82: return "Num0";
    case 83: return "Num.";
    case 87: return "F11";
    case 88: return "F12";
    case 156: return "NumEnter";
    case 157: return "RCtrl";
    case 181: return "Num/";
    case 183: return "SysRq";
    case 184: return "RAlt";
    case 197: return "Pause";
    case 199: return "Home";
    case 200: return "Up";
    case 201: return "PageUp";
    case 203: return "Left";
    case 205: return "Right";
    case 207: return "End";
    case 208: return "Down";
    case 209: return "PageDown";
    case 210: return "Insert";
    case 211: return "Delete";
    case 219: return "LWin";
    case 220: return "RWin";
    case 221: return "AppMenu";
    default: return "?";
    }
}

static void copy_name(char *dst, size_t cap, const std::string &src)
{
    size_t n = src.size();
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

// POV angle: 100ths of a degree, -1 centered. 0x2000 ids return -sin,
// 0x3000 ids +cos; a negative raw other than -1 wraps as unsigned.
static float pov_value(int32_t raw, bool x_axis)
{
    if (raw == -1)
        return 0.0f;
    float angle = (float)((double)(uint32_t)raw * 0.00017452778);
    return x_axis ? -std::sin(angle) : std::cos(angle);
}

void DeviceTable::init(Backend &backend)
{
    std::vector<DeviceDesc> descs;
    backend.enumerate(&descs);
    count_ = 0;
    for (const DeviceDesc &desc : descs) {
        if (count_ >= kMaxDevices)
            break;
        DeviceRecord &d = devices_[count_];
        d = DeviceRecord();
        d.type = desc.type;
        copy_name(d.device_name, sizeof d.device_name, desc.name);
        if (desc.type == kKeyboard) {
            d.control_count = 256;  // exe forces 0x100; ids stay zero
        } else {
            size_t n = desc.controls.size();
            if (n > (size_t)kMaxControls)
                n = kMaxControls;
            d.control_count = (int)n;
            for (size_t i = 0; i < n; i++) {
                d.ids[i] = desc.controls[i].id;
                copy_name(d.names[i], sizeof d.names[i], desc.controls[i].name);
                if (d.ids[i] >= kButtonBase && d.ids[i] < kPovBase)
                    d.button_count++;
            }
        }
        count_++;
    }
}

void DeviceTable::poll(Backend &backend)
{
    poll_ms_ = backend.poll_interval_ms();
    for (int i = 0; i < count_; i++) {
        DeviceRecord &d = devices_[i];
        RawState state;
        if (backend.poll(i, &state))
            d.raw = state;
        else
            d.raw = RawState();  // exe memsets the raw state on read failure
        if (d.type != kMouse)
            continue;
        // 0x544d90: absolute axes accumulate the raw deltas.
        for (int k = 0; k < d.control_count; k++) {
            uint32_t id = d.ids[k];
            int axis = id == 2 ? 0 : id == 4 ? 1 : id == 8 ? 2 : -1;
            if (axis < 0)
                continue;
            d.last[k] += (float)d.raw.mouse[axis] * 0.015625f * mouse_sensitivity_;
            d.last[k] = clamp_unit(d.last[k]);
        }
    }
}

float DeviceTable::mouse_axis(int device, int index)
{
    DeviceRecord &d = devices_[device];
    // Negative index -1..-101 sets the mouse sensitivity ((~i)*0.01) and
    // clears the absolute axis cache.
    if (index >= -101 && index <= -1) {
        mouse_sensitivity_ = (float)(~index) * 0.01f;
        for (int i = 0; i < d.control_count; i++)
            d.last[i] = 0.0f;
        return 0.0f;
    }
    if (index < 0 || index >= d.control_count)
        return 0.0f;
    uint32_t id = d.ids[index];
    float value = 0.0f;
    if (id >= kButtonBase && id < kButtonBase + (uint32_t)d.button_count) {
        int b = (int)(id - kButtonBase);
        if (b < 4 && (int8_t)d.raw.mouse_buttons[b] < 0)
            value = 1.0f;
    } else if (id == 2 || id == 4 || id == 8) {
        value = d.last[index];
    } else if (id == 0x8002 || id == 0x8004 || id == 0x8008) {
        if (poll_ms_ > 0.0f) {
            int axis = id == 0x8002 ? 0 : id == 0x8004 ? 1 : 2;
            value = (float)d.raw.mouse[axis] / poll_ms_ * 0.005f;
        }
    }
    return clamp_unit(value);
}

float DeviceTable::joystick_axis(int device, int index) const
{
    const DeviceRecord &d = devices_[device];
    if (index < 0 || index >= d.control_count)
        return 0.0f;
    uint32_t id = d.ids[index];
    if (id >= kButtonBase && id < kButtonBase + (uint32_t)d.button_count) {
        int b = (int)(id - kButtonBase);
        if (b < 128 && (int8_t)d.raw.buttons[b] < 0)
            return 1.0f;
        return 0.0f;
    }
    int axis = -1;
    switch (id) {
    case 2: axis = 0; break;
    case 4: axis = 1; break;
    case 8: axis = 2; break;
    case 0x10: axis = 3; break;
    case 0x20: axis = 4; break;
    case 0x40: axis = 5; break;
    case 0x80: axis = 6; break;
    case 0x81: axis = 7; break;
    default: break;
    }
    if (axis >= 0)
        return (float)(0x8000 - d.raw.axes[axis]) * 3.0517578125e-5f;
    if (id >= 0x2000 && id <= 0x2003)
        return pov_value(d.raw.pov[id - 0x2000], true);
    if (id >= 0x3000 && id <= 0x3003)
        return pov_value(d.raw.pov[id - 0x3000], false);
    return 0.0f;
}

float DeviceTable::get_axis(int device, int index)
{
    if (device < 0 || device >= count_)
        return 0.0f;
    DeviceRecord &d = devices_[device];
    if (d.type == kKeyboard) {
        if (index < 0 || index >= d.control_count)
            return 0.0f;
        return (int8_t)d.raw.keys[index & 0xff] < 0 ? 1.0f : 0.0f;
    }
    if (d.type == kMouse)
        return mouse_axis(device, index);
    if (d.type == kJoystick)
        return joystick_axis(device, index);
    return 0.0f;
}

int DeviceTable::kind(int device, int index) const
{
    if (device < 0 || device >= count_)
        return 0;
    const DeviceRecord &d = devices_[device];
    if (index < 0 || index >= d.control_count)
        return 0;
    if (d.type == kMouse || d.type == kJoystick) {
        uint32_t id = d.ids[index];
        if (id < kButtonBase || id >= kButtonBase + (uint32_t)d.button_count)
            return 1;
    }
    return 0;
}

int DeviceTable::type(int device) const
{
    if (device < 0 || device >= count_)
        return 0;
    return devices_[device].type;
}

int DeviceTable::active_axis(int device)
{
    if (device < 0 || device >= count_)
        return -1;
    DeviceRecord &d = devices_[device];
    if (d.control_count <= 0)
        return -1;
    int hits = 0;
    int hit = -1;
    int last = -1;
    for (int i = 0; i < d.control_count; i++) {
        if (d.ids[i] & 0x8000u)
            continue;
        float v = get_axis(device, i);
        if (v >= 0.25f) {
            hits++;
            hit = i;
        }
        if (v <= -0.25f && v >= -0.75f) {
            hits++;
            hit = i;
        }
        if (v <= -0.75f && d.last[i] >= -0.25f) {
            hits++;
            hit = i;
        }
        d.last[i] = v;
        last = hit;
    }
    return hits == 1 ? last : -1;
}

std::string DeviceTable::axis_name(int device, int index) const
{
    if (device < 0 || device >= count_)
        return std::string();
    const DeviceRecord &d = devices_[device];
    if (index < 0 || index >= d.control_count)
        return std::string();
    if (d.type == kKeyboard)
        return key_name(index);
    return std::string(d.names[index]);
}

const char *DeviceTable::device_name(int device) const
{
    if (device < 0 || device >= count_)
        return nullptr;
    return devices_[device].device_name;
}

}  // namespace input
