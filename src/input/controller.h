// Controller: 76 virtual axis records and 152 device mappings (notes/14 §5).
// Semantics mirror User__Add/Del/Reset/GetAxisVal/SetAxisForce/SetAxisSmooth/
// SetAxisSpeed (0x4d3cb0..0x4d4180) and the per-frame force pass 0x566160.
#pragma once
#include <stdint.h>

namespace input {

class Backend;
class DeviceTable;

// Frame clock (DAT_007a5b54 ms, DAT_007a5b60 stamp). Call once per frame
// before tick/getAxisVal; the exe increments the stamp in its per-frame pass.
void begin_frame(float dt_seconds);
float frame_ms();
uint32_t frame_stamp();

// 0x566160: frame clock, device poll, every controller's tick + FF pass.
void update(float dt_seconds);

// One virtual axis record (stride 0x30). Offsets: +0 value, +4 kind,
// +8 stamp, +0xc center_range, +0x10 center_factor, +0x14 opposite,
// +0x18 same, +0x1c speed, +0x20 power, +0x24 force_c, +0x28 force_f,
// +0x2c smooth.
struct AxisRecord {
    float value = 0.0f;
    int kind = 0;
    uint32_t stamp = 0;
    float center_range = 0.1f;
    float center_factor = 1.0f;
    float opposite = 1.0f;
    float same = 1.0f;
    float speed = 1.0f;
    float power = 1.0f;
    float force_c = 0.0f;
    float force_f = 0.0f;
    float smooth = 0.0f;
};

// One mapping (stride 0x20): vaxis 0 marks a free slot.
struct Mapping {
    int vaxis = 0;
    int device = 0;
    int index = 0;
    float last_raw = 0.0f;
    float in_min = 0.0f;
    float in_max = 0.0f;
    float out_min = 0.0f;
    float out_max = 0.0f;
};

class Controller {
  public:
    static constexpr int kAxisCount = 76;
    static constexpr int kMappingCount = 152;

    explicit Controller(DeviceTable &devices);
    ~Controller();
    Controller(const Controller &) = delete;
    Controller &operator=(const Controller &) = delete;

    // user_Add: first free mapping, returns slot+1 (0 when full, -1 invalid).
    int add(int vaxis, int device, int index, float in_min, float in_max,
            float out_min, float out_max);
    // user_Del: clears the first matching mapping; the native returns 0.
    int remove(int vaxis, int device, int index);
    void reset();
    float get_axis_val(int vaxis);
    void set_axis_force(int vaxis, float force_c, float force_f);
    void set_axis_smooth(int vaxis, float center_range, float center_factor,
                         float opposite, float same, float power);
    void set_axis_speed(int vaxis, float speed);
    // Per-frame force pass over analog (kind 0) records.
    void tick();
    void force_feedback_pass(Backend &backend);  // 0x566160 inner loop

    // 0x566160 tests and the native wrappers read these directly.
    const AxisRecord &axis(int vaxis) const { return axes_[vaxis]; }
    int mapping_count() const { return mapping_count_; }

  private:
    friend void update(float);

    DeviceTable &devices_;
    int mapping_count_ = 0;
    AxisRecord axes_[kAxisCount];
    Mapping mappings_[kMappingCount];
    Controller *next_ = nullptr;  // +0x2144
};

}  // namespace input
