#include "input/hotkeys.h"

#include "input/backend.h"
#include "input/controller.h"
#include "input/device.h"

namespace input {

void HotkeyTable::create(int axis, int flags, HotkeyState *hotkey, void *handler,
                         void *osd, uint32_t events)
{
    for (HotkeySlot &s : slots_) {
        if (s.flags != 0)
            continue;
        s.flags = (uint32_t)flags;
        s.axis = axis;
        s.hotkey = hotkey;
        s.osd = osd;
        s.state = 0;
        s.handler = handler;
        s.events = events;
        s.latch = 0;
        return;
    }
}

void HotkeyTable::remove(HotkeyState *hotkey)
{
    for (HotkeySlot &s : slots_) {
        if (s.hotkey == hotkey) {
            s.flags = 0;
            return;
        }
    }
}

void HotkeyTable::flush()
{
    for (HotkeySlot &s : slots_) {
        if (s.flags != 0)
            s.latch = 0;
    }
}

void HotkeyTable::check(Controller &controller, DeviceTable &devices,
                        void *infocus_osd)
{
    int count = 0;
    int first = -1;
    for (int i = 0; i < kSlots; i++) {
        HotkeySlot &s = slots_[i];
        if (s.flags == 0)
            continue;
        if (s.osd != nullptr && s.osd != infocus_osd)
            continue;
        float value = 0.0f;
        if (s.flags & 1)
            value = controller.get_axis_val(s.axis);
        else if (s.flags & 2)
            value = devices.get_axis(0, s.axis) > 0.1f ? 1.0f : 0.0f;
        bool pressed = value > 0.2f;
        if (s.latch == 0) {
            s.latch = 1;  // first evaluation only arms the slot
        } else if (s.state != (pressed ? 1 : 0)) {
            uint32_t mask = pressed ? (s.events & 1u) : (s.events & 2u);
            if (mask != 0) {
                if (count == 0)
                    first = i;
                count++;
            }
        }
        s.state = pressed ? 1 : 0;
        if (s.hotkey != nullptr)
            s.hotkey->state = s.state;
    }
    if (count > 0)
        queue.push_back({slots_[first].handler, slots_[first].hotkey});
}

uint32_t HotkeyTable::last_key(Backend &keyboard)
{
    uint32_t packed = keyboard.read_key();
    int ansi = (int)(packed >> 16);
    if ((ansi & 0xff) != 0) {
        last_ring[last_pos] = (uint8_t)ansi;
        last_pos++;
        if (last_pos == 16)
            last_pos = 0;
    }
    return packed;
}

}  // namespace input
