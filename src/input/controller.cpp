#include "input/controller.h"

#include <cmath>
#include <math.h>

#include "input/backend.h"
#include "input/device.h"

namespace input {
namespace {

float g_frame_ms = 0.0f;
// The exe increments BSS stamp 0 -> 1 before the first evaluation; starting at
// 1 keeps a controller usable without begin_frame (reset records have stamp 0).
uint32_t g_stamp = 1;
const float kFrameScale = 1.0f;  // DAT_007a5b5c
// 0x566010 registry (DAT_007a5b68 / DAT_007a5b64), newest first.
Controller *g_controllers = nullptr;
int g_controller_count = 0;

}  // namespace

void begin_frame(float dt_seconds)
{
    g_frame_ms = dt_seconds * 1000.0f;
    g_stamp++;
}

float frame_ms()
{
    return g_frame_ms;
}

uint32_t frame_stamp()
{
    return g_stamp;
}

Controller::Controller(DeviceTable &devices) : devices_(devices)
{
    // 0x566010 register (newest first; the exe never unregisters)
    next_ = g_controllers;
    g_controllers = this;
    g_controller_count++;
}

Controller::~Controller()
{
    // INVENTED: the exe never destroys controller blocks.
    Controller **link = &g_controllers;
    while (*link && *link != this)
        link = &(*link)->next_;
    if (*link) {
        *link = next_;
        g_controller_count--;
    }
}

int Controller::add(int vaxis, int device, int index, float in_min, float in_max,
                    float out_min, float out_max)
{
    if (vaxis < 0 || vaxis >= kAxisCount || device < 0 || index < 0)
        return -1;
    for (int k = 0; k < kMappingCount; k++) {
        Mapping &m = mappings_[k];
        if (m.vaxis != 0)
            continue;
        m.vaxis = vaxis;
        m.device = device;
        m.index = index;
        m.in_min = in_min;
        m.in_max = in_max;
        m.out_min = out_min;
        m.out_max = out_max;
        m.last_raw = devices_.get_axis(device, index);
        axes_[vaxis].kind = devices_.kind(device, index);
        mapping_count_++;
        return k + 1;
    }
    return 0;
}

int Controller::remove(int vaxis, int device, int index)
{
    for (Mapping &m : mappings_) {
        if (m.vaxis == vaxis && m.device == device && m.index == index) {
            m.vaxis = 0;  // other fields stay stale, like the exe
            mapping_count_--;
            break;
        }
    }
    return 0;
}

void Controller::reset()
{
    for (Mapping &m : mappings_)
        m.vaxis = 0;
    mapping_count_ = 0;
    for (AxisRecord &a : axes_)
        a = AxisRecord();
}

void Controller::set_axis_force(int vaxis, float force_c, float force_f)
{
    if (vaxis < 0 || vaxis >= kAxisCount)
        return;
    axes_[vaxis].force_c = force_c;
    axes_[vaxis].force_f = force_f;
}

void Controller::set_axis_smooth(int vaxis, float center_range, float center_factor,
                                 float opposite, float same, float power)
{
    if (vaxis < 0 || vaxis >= kAxisCount)
        return;
    AxisRecord &a = axes_[vaxis];
    a.center_range = center_range;
    a.center_factor = center_factor;
    a.opposite = opposite;
    a.same = same;
    a.power = power;
}

void Controller::set_axis_speed(int vaxis, float speed)
{
    if (vaxis < 0 || vaxis >= kAxisCount)
        return;
    axes_[vaxis].speed = speed;
}

float Controller::get_axis_val(int vaxis)
{
    if (vaxis < 0 || vaxis >= kAxisCount)
        return 0.0f;
    AxisRecord &a = axes_[vaxis];
    if (a.stamp != g_stamp) {
        float sum = 0.0f;
        int last_type = 0;
        for (Mapping &m : mappings_) {
            if (m.vaxis != vaxis)
                continue;
            float raw = devices_.get_axis(m.device, m.index);
            int kind = devices_.kind(m.device, m.index);
            if (kind != a.kind) {
                // An analog control only takes over the virtual axis when it
                // moved more than 10% of its input range since the last read.
                if (std::fabs(m.in_max - m.in_min) * 0.1f <
                    std::fabs(raw - m.last_raw)) {
                    a.kind = kind;
                    m.last_raw = raw;
                }
                if (kind != a.kind)
                    continue;
            }
            last_type = devices_.type(m.device);
            if ((m.in_min <= raw && raw <= m.in_max) ||
                (raw <= m.in_min && m.in_max <= raw)) {
                sum += (m.out_max - m.out_min) * (raw - m.in_min) /
                           (m.in_max - m.in_min) +
                       m.out_min;
            }
            m.last_raw = raw;
        }
        sum = clamp_unit(sum);
        if (a.kind == 0) {
            float rate;
            if (a.center_range <= sum || sum <= -a.center_range) {
                rate = 0.0f <= a.value * sum ? a.same : a.opposite;
            } else {
                rate = a.center_factor;
                if (a.force_f != 0.0f)
                    rate *= 0.5f;
            }
            float old = a.value;
            float step = kFrameScale * g_frame_ms * a.speed * rate;
            float delta = sum - old;
            if (step >= 1.0f)
                step = 1.0f;
            float mag = std::fabs(delta);
            if (step < mag)
                delta *= step / mag;
            if (a.smooth <= 0.0f) {
                a.value = old + delta;
            } else {
                float f = a.smooth * 0.01f * g_frame_ms * kFrameScale;
                if (f >= 1.0f)
                    f = 1.0f;
                a.value = old + delta * (1.0f - f);
            }
        } else if (last_type == 2) {
            a.value = sum;  // relative mouse axis passes through
        } else {
            float old = a.value;
            if (std::fabs(old - sum) > 0.021f) {
                float step = sum <= old ? -0.02f : 0.02f;
                a.value = sum - step;
            } else {
                a.value = (sum - old) * 0.3f + old;
            }
        }
        a.value = clamp_unit(a.value);
        a.stamp = g_stamp;
    }
    float p = a.power;
    if (p <= 1.0f)
        return a.value;
    float sign = a.value < 0.0f ? -1.0f : 1.0f;
    float mag = std::fabs(a.value);
    if (mag < 1e-5f)
        return 0.0f;
    float root = std::sqrt(mag);
    if (p >= 5.0f)
        p = 5.0f;
    while (p > 1.125f) {
        mag *= std::sqrt(root);
        p -= 0.25f;
    }
    return sign * mag;
}

void Controller::tick()
{
    for (AxisRecord &a : axes_) {
        if (a.kind != 0)
            continue;
        float delta = 0.0f;
        if (a.force_f != 0.0f) {
            delta = (a.force_c - a.value) * a.force_f * 1.5e-6f * g_frame_ms *
                    kFrameScale;
            if ((delta + a.value - a.force_c) * (a.value - a.force_c) < 0.0f)
                delta = a.force_c - a.value;
        }
        if (a.smooth > 0.0f) {
            float f = a.smooth * 0.01f * g_frame_ms * kFrameScale;
            if (f >= 1.0f)
                f = 1.0f;
            delta *= 1.0f - f;
        }
        a.value = clamp_unit(a.value + delta);
    }
}

void Controller::force_feedback_pass(Backend &backend)
{
    // 0x566160 inner loop
    for (int i = 0; i < kMappingCount; i++) {
        const Mapping &m = mappings_[i];
        if (m.index >= 1)
            continue;
        if (m.vaxis < 0 || m.vaxis >= kAxisCount)  // INVENTED: bounds guard (exe indexes unchecked)
            continue;
        const AxisRecord &a = axes_[m.vaxis];
        if (a.kind == 0 || devices_.type(m.device) != kJoystick)
            continue;
        if (m.device < 0 || m.device >= devices_.count())
            continue;
        backend.force_feedback(m.device, -a.force_c, a.force_f);
    }
}

void update(float dt_seconds)
{
    // 0x566160
    begin_frame(dt_seconds);
    Backend *b = backend();
    if (b)
        devices().poll(*b);
    for (Controller *c = g_controllers; c; c = c->next_) {
        c->tick();
        if (b)
            c->force_feedback_pass(*b);
    }
}

}  // namespace input
