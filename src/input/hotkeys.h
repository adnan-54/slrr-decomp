// Hotkey table (512 slots, stride 0x20) and the 16-byte lastKey ring
// (notes/14 §6). check() queues exactly one Game__queueEvent per call for the
// first slot that changed state, exactly like 0x4de750.
#pragma once
#include <stdint.h>

#include <vector>

namespace input {

class Backend;
class Controller;
class DeviceTable;

// Script Hotkey object stand-in; the native writes its +0x10 state field.
struct HotkeyState {
    int state = 0;
};

// Slot offsets: +0 flags, +4 axis, +8 Hotkey, +0xc Osd, +0x10 state,
// +0x14 handler, +0x18 event mask (1 press, 2 release), +0x1c latch.
struct HotkeySlot {
    uint32_t flags = 0;  // 1 virtual axis, 2 key
    int axis = 0;
    HotkeyState *hotkey = nullptr;
    void *osd = nullptr;
    int state = 0;
    void *handler = nullptr;
    uint32_t events = 0;
    uint32_t latch = 0;
};

// Game__queueEvent(handler, 0, 0x100000, hotkey, 0).
struct QueuedEvent {
    void *handler = nullptr;
    HotkeyState *hotkey = nullptr;
};

class HotkeyTable {
  public:
    static constexpr int kSlots = 512;

    void create(int axis, int flags, HotkeyState *hotkey, void *handler, void *osd,
                uint32_t events);
    void remove(HotkeyState *hotkey);
    void flush();
    // infocus_osd null accepts every slot; otherwise slots owned by another OSD
    // are skipped.
    void check(Controller &controller, DeviceTable &devices, void *infocus_osd);
    // Buffered keyboard read plus the ANSI ring; returns the packed
    // (ansi << 16) | scan value unchanged.
    uint32_t last_key(Backend &keyboard);

    const HotkeySlot &slot(int index) const { return slots_[index]; }
    std::vector<QueuedEvent> queue;  // one entry per check() that fired
    uint8_t last_ring[16] = {};
    int last_pos = 0;

  private:
    HotkeySlot slots_[kSlots];
};

}  // namespace input
