// DirectInput8 backend. Device creation and control enumeration mirror
// 0x544420/0x543920/0x543700 (notes/14 §1-3): keyboard first, mouse second,
// then attached game controllers with DIJOYSTATE2.
#include "input/platform_backend.h"

#include "input/backend.h"

#ifdef _WIN32
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <windows.h>
#include <dinput.h>
#include <string.h>

namespace input {
namespace {

struct ObjectCtx {
    DeviceDesc *desc = nullptr;
    int buttons = 0;
    int sliders = 0;
    int povs = 0;
};

BOOL CALLBACK axis_cb(LPCDIDEVICEOBJECTINSTANCEA obj, LPVOID param)
{
    ObjectCtx &ctx = *static_cast<ObjectCtx *>(param);
    if ((int)ctx.desc->controls.size() >= 64)
        return FALSE;
    uint32_t id;
    if (IsEqualGUID(obj->guidType, GUID_XAxis))
        id = 2;
    else if (IsEqualGUID(obj->guidType, GUID_YAxis))
        id = 4;
    else if (IsEqualGUID(obj->guidType, GUID_ZAxis))
        id = 8;
    else if (IsEqualGUID(obj->guidType, GUID_RxAxis))
        id = 0x10;
    else if (IsEqualGUID(obj->guidType, GUID_RyAxis))
        id = 0x20;
    else if (IsEqualGUID(obj->guidType, GUID_RzAxis))
        id = 0x40;
    else if (IsEqualGUID(obj->guidType, GUID_Slider)) {
        if (ctx.sliders >= 2)
            return TRUE;
        id = 0x80u + (uint32_t)ctx.sliders++;
    } else {
        return TRUE;
    }
    ControlDesc control;
    control.id = id;
    control.name = obj->tszName;
    ctx.desc->controls.push_back(control);
    return TRUE;
}

BOOL CALLBACK button_cb(LPCDIDEVICEOBJECTINSTANCEA obj, LPVOID param)
{
    ObjectCtx &ctx = *static_cast<ObjectCtx *>(param);
    if ((int)ctx.desc->controls.size() >= 64)
        return FALSE;
    ControlDesc control;
    control.id = kButtonBase + (uint32_t)ctx.buttons++;
    control.name = obj->tszName;
    ctx.desc->controls.push_back(control);
    return TRUE;
}

BOOL CALLBACK pov_cb(LPCDIDEVICEOBJECTINSTANCEA obj, LPVOID param)
{
    ObjectCtx &ctx = *static_cast<ObjectCtx *>(param);
    if ((int)ctx.desc->controls.size() >= 64)
        return FALSE;
    if (ctx.povs >= 4)
        return TRUE;
    int pov = ctx.povs++;
    ControlDesc x, y;
    x.id = kPovBase + (uint32_t)pov;
    x.name = obj->tszName;
    y.id = 0x3000u + (uint32_t)pov;
    y.name = obj->tszName;
    ctx.desc->controls.push_back(x);
    ctx.desc->controls.push_back(y);
    return TRUE;
}

class DInputBackend : public Backend {
  public:
    explicit DInputBackend(void *window) : window_((HWND)window)
    {
        if (FAILED(DirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION,
                                      IID_IDirectInput8A,
                                      reinterpret_cast<void **>(&di_), nullptr)))
            di_ = nullptr;
        if (di_ == nullptr)
            return;
        init_keyboard();
        init_mouse();
        init_joysticks();
    }

    ~DInputBackend() override
    {
        for (Device &d : devices_)
            d.dev->Release();
        if (di_ != nullptr)
            di_->Release();
    }

    bool valid() const { return di_ != nullptr && keyboard_ != nullptr; }

    void enumerate(std::vector<DeviceDesc> *out) override { *out = descs_; }

    bool poll(int device, RawState *raw) override
    {
        if (device < 0 || (size_t)device >= devices_.size())
            return false;
        Device &d = devices_[device];
        *raw = RawState();
        union {
            uint8_t keys[256];
            DIMOUSESTATE mouse;
            DIJOYSTATE2 joy;
        } state;
        memset(&state, 0, sizeof state);
        HRESULT hr = d.dev->GetDeviceState(d.state_size, &state);
        if (FAILED(hr)) {
            if (FAILED(d.dev->Acquire()))
                return false;
            hr = d.dev->GetDeviceState(d.state_size, &state);
            if (FAILED(hr))
                return false;
        }
        if (d.type == kKeyboard) {
            memcpy(raw->keys, state.keys, sizeof raw->keys);
        } else if (d.type == kMouse) {
            raw->mouse[0] = state.mouse.lX;
            raw->mouse[1] = state.mouse.lY;
            raw->mouse[2] = state.mouse.lZ;
            for (int i = 0; i < 4; i++)
                raw->mouse_buttons[i] = state.mouse.rgbButtons[i];
        } else {
            raw->axes[0] = state.joy.lX;
            raw->axes[1] = state.joy.lY;
            raw->axes[2] = state.joy.lZ;
            raw->axes[3] = state.joy.lRx;
            raw->axes[4] = state.joy.lRy;
            raw->axes[5] = state.joy.lRz;
            raw->axes[6] = state.joy.rglSlider[0];
            raw->axes[7] = state.joy.rglSlider[1];
            for (int i = 0; i < 4; i++)
                raw->pov[i] = (int32_t)state.joy.rgdwPOV[i];
            memcpy(raw->buttons, state.joy.rgbButtons, sizeof raw->buttons);
        }
        return true;
    }

    float poll_interval_ms() override
    {
        LARGE_INTEGER now;
        if (!QueryPerformanceCounter(&now))
            return 0.0f;
        if (!have_time_) {
            QueryPerformanceFrequency(&freq_);
            have_time_ = true;
            prev_ = now;
            return 0.0f;
        }
        float ms = 0.0f;
        if (freq_.QuadPart != 0)
            ms = (float)((double)(now.QuadPart - prev_.QuadPart) * 1000.0 /
                         (double)freq_.QuadPart);
        prev_ = now;
        return ms;
    }

    uint32_t read_key() override
    {
        if (keyboard_ == nullptr || FAILED(keyboard_->Acquire()))
            return 0;
        DIDEVICEOBJECTDATA event;
        DWORD count = 1;
        HRESULT hr = keyboard_->GetDeviceData(sizeof event, &event, &count, 0);
        if (FAILED(hr) || count != 1 || event.dwOfs >= 256 || (event.dwData & 0x80) == 0)
            return 0;
        UINT vk = MapVirtualKeyExA((UINT)event.dwOfs, 1, layout_);
        BYTE keys[256];
        WORD ansi = 0;
        if (GetKeyboardState(keys))
            ToAsciiEx(vk, (UINT)event.dwOfs, keys, &ansi, 0, layout_);
        return ((uint32_t)ansi << 16) | event.dwOfs;
    }

  private:
    struct Device {
        IDirectInputDevice8A *dev = nullptr;
        DWORD state_size = 0;
        int type = 0;
    };

    static void set_buffer(IDirectInputDevice8A *dev)
    {
        DIPROPDWORD prop;
        memset(&prop, 0, sizeof prop);
        prop.diph.dwSize = sizeof prop;
        prop.diph.dwHeaderSize = sizeof prop.diph;
        prop.diph.dwHow = DIPH_DEVICE;
        prop.dwData = 16;
        dev->SetProperty(DIPROP_BUFFERSIZE, &prop.diph);
    }

    void init_keyboard()
    {
        IDirectInputDevice8A *dev = nullptr;
        if (FAILED(di_->CreateDevice(GUID_SysKeyboard, &dev, nullptr)))
            return;
        dev->SetDataFormat(&c_dfDIKeyboard);
        dev->SetCooperativeLevel(window_, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
        set_buffer(dev);
        dev->Acquire();
        keyboard_ = dev;
        layout_ = GetKeyboardLayout(0);
        Device d;
        d.dev = dev;
        d.state_size = 256;
        d.type = kKeyboard;
        devices_.push_back(d);
        DeviceDesc desc;
        desc.type = kKeyboard;
        desc.name = "SysKeyboard";
        descs_.push_back(desc);
    }

    void init_mouse()
    {
        IDirectInputDevice8A *dev = nullptr;
        if (FAILED(di_->CreateDevice(GUID_SysMouse, &dev, nullptr)))
            return;
        dev->SetDataFormat(&c_dfDIMouse);
        dev->SetCooperativeLevel(window_, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND);
        dev->Acquire();
        Device d;
        d.dev = dev;
        d.state_size = sizeof(DIMOUSESTATE);
        d.type = kMouse;
        devices_.push_back(d);
        DeviceDesc desc;
        desc.type = kMouse;
        desc.name = "SysMouse";
        static const struct {
            uint32_t id;
            const char *name;
        } controls[] = {
            {0x8002, "Mouse X"}, {0x8004, "Mouse Y"}, {0x8008, "Mouse Z"},
            {0x1000, "M Button 1"}, {0x1001, "M Button 2"},
            {0x1002, "M Button 3"}, {0x1003, "M Button 4"},
            {2, "Mouse X"}, {4, "Mouse Y"}, {8, "Mouse Z"},
        };
        for (const auto &c : controls) {
            ControlDesc control;
            control.id = c.id;
            control.name = c.name;
            desc.controls.push_back(control);
        }
        descs_.push_back(desc);
    }

    static BOOL CALLBACK device_cb(LPCDIDEVICEINSTANCEA inst, LPVOID param)
    {
        return static_cast<DInputBackend *>(param)->add_joystick(inst);
    }

    BOOL add_joystick(LPCDIDEVICEINSTANCEA inst)
    {
        if (devices_.size() >= 16)
            return FALSE;
        IDirectInputDevice8A *dev = nullptr;
        if (FAILED(di_->CreateDevice(inst->guidInstance, &dev, nullptr)))
            return TRUE;
        dev->SetDataFormat(&c_dfDIJoystick2);
        dev->SetCooperativeLevel(window_, DISCL_EXCLUSIVE | DISCL_FOREGROUND);
        dev->Unacquire();
        set_buffer(dev);
        dev->Acquire();
        DeviceDesc desc;
        desc.type = kJoystick;
        desc.name = inst->tszInstanceName;
        ObjectCtx ctx;
        ctx.desc = &desc;
        dev->EnumObjects(axis_cb, &ctx, DIDFT_AXIS);
        dev->EnumObjects(button_cb, &ctx, DIDFT_BUTTON);
        dev->EnumObjects(pov_cb, &ctx, DIDFT_POV);
        Device d;
        d.dev = dev;
        d.state_size = sizeof(DIJOYSTATE2);
        d.type = kJoystick;
        devices_.push_back(d);
        descs_.push_back(desc);
        return TRUE;
    }

    void init_joysticks()
    {
        di_->EnumDevices(DI8DEVCLASS_GAMECTRL, device_cb, this, DIEDFL_ATTACHEDONLY);
    }

    IDirectInput8A *di_ = nullptr;
    IDirectInputDevice8A *keyboard_ = nullptr;
    HKL layout_ = nullptr;
    HWND window_ = nullptr;
    std::vector<Device> devices_;
    std::vector<DeviceDesc> descs_;
    LARGE_INTEGER freq_ = {};
    LARGE_INTEGER prev_ = {};
    bool have_time_ = false;
};

}  // namespace
}  // namespace input

#endif  // _WIN32

namespace input {

Backend *make_platform_backend(void *window)
{
#ifdef _WIN32
    DInputBackend *backend = new DInputBackend(window);
    if (!backend->valid()) {
        delete backend;
        return nullptr;
    }
    return backend;
#else
    (void)window;
    return new SyntheticBackend;
#endif
}

}  // namespace input
