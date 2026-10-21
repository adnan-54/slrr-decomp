// Assert-based checks for the input layer: synthetic keyboard/mouse/joystick
// states through the backend, device table readers/kind/activeAxis/names,
// controller mapping math (lerp, clamp, kind switch, smoothing, power, stamp
// cache), hotkey fire/queue semantics, the lastKey ring, and cursor hooks.
// Build/run:
//   g++ -std=c++17 -Wall -Wextra -Wpedantic -Isrc -o src/build/inputcheck
//       src/tools/inputcheck.cpp src/input/backend.cpp src/input/device.cpp
//       src/input/controller.cpp src/input/hotkeys.cpp src/input/cursor.cpp
//       src/input/platform_backend.cpp -lm
//   ./src/build/inputcheck
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "input/backend.h"
#include "input/controller.h"
#include "input/cursor.h"
#include "input/device.h"
#include "input/hotkeys.h"

using namespace input;

static int failures = 0;

static void check(const char *name, bool ok, const char *detail)
{
    printf("[%s] %-54s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
    if (!ok)
        failures++;
}

static bool near(float a, float b, float eps = 1e-5f)
{
    return fabsf(a - b) <= eps;
}

static DeviceDesc mouse_desc()
{
    static const struct {
        uint32_t id;
        const char *name;
    } controls[] = {
        {0x8002, "Mouse X"}, {0x8004, "Mouse Y"}, {0x8008, "Mouse Z"},
        {0x1000, "M Button 1"}, {0x1001, "M Button 2"}, {0x1002, "M Button 3"},
        {0x1003, "M Button 4"}, {2, "Mouse X"}, {4, "Mouse Y"}, {8, "Mouse Z"},
    };
    DeviceDesc d;
    d.type = kMouse;
    d.name = "SysMouse";
    for (const auto &c : controls)
        d.controls.push_back(ControlDesc{c.id, c.name});
    return d;
}

static DeviceDesc stick_desc()
{
    static const struct {
        uint32_t id;
        const char *name;
    } controls[] = {
        {2, "X Axis"}, {4, "Y Axis"}, {8, "Z Axis"}, {0x10, "Rx"}, {0x20, "Ry"},
        {0x40, "Rz"}, {0x80, "Slider 0"}, {0x81, "Slider 1"},
        {0x1000, "Button 0"}, {0x1001, "Button 1"}, {0x1002, "Button 2"},
        {0x1003, "Button 3"}, {0x2000, "POV 0"}, {0x3000, "POV 0"},
    };
    DeviceDesc d;
    d.type = kJoystick;
    d.name = "Test Stick";
    for (const auto &c : controls)
        d.controls.push_back(ControlDesc{c.id, c.name});
    return d;
}

// Keyboard at 0, mouse at 1, joystick at 2, like the exe's enumeration.
struct Fixture {
    SyntheticBackend backend;
    DeviceTable table;

    Fixture()
    {
        SyntheticBackend::Device kb;
        kb.desc.type = kKeyboard;
        kb.desc.name = "SysKeyboard";
        backend.devices.push_back(kb);
        SyntheticBackend::Device mouse;
        mouse.desc = mouse_desc();
        backend.devices.push_back(mouse);
        SyntheticBackend::Device stick;
        stick.desc = stick_desc();
        backend.devices.push_back(stick);
        table.init(backend);
    }
};

static void test_devices()
{
    Fixture f;
    char buf[160];

    snprintf(buf, sizeof buf, "count %d, types %d/%d/%d", f.table.count(),
             f.table.type(0), f.table.type(1), f.table.type(2));
    check("device table: 3 records", f.table.count() == 3 &&
                                          f.table.type(0) == kKeyboard &&
                                          f.table.type(1) == kMouse &&
                                          f.table.type(2) == kJoystick,
          buf);
    check("device table: device names",
          strcmp(f.table.device_name(0), "SysKeyboard") == 0 &&
              strcmp(f.table.device_name(1), "SysMouse") == 0 &&
              strcmp(f.table.device_name(2), "Test Stick") == 0,
          "");
    check("device table: invalid indices",
          f.table.device_name(-1) == nullptr && f.table.device_name(3) == nullptr &&
              f.table.type(3) == 0 && f.table.get_axis(3, 0) == 0.0f,
          "");

    f.backend.devices[0].raw.keys[30] = 0x80;
    f.table.poll(f.backend);
    check("keyboard: 0x80 is pressed", f.table.get_axis(0, 30) == 1.0f &&
                                           f.table.get_axis(0, 31) == 0.0f,
          "");
    check("keyboard: index bounds", f.table.get_axis(0, -1) == 0.0f &&
                                        f.table.get_axis(0, 256) == 0.0f,
          "");
    check("keyboard: kind is always 0", f.table.kind(0, 30) == 0, "");
    check("keyboard: scan code names",
          f.table.axis_name(0, 1) == "Escape" &&
              f.table.axis_name(0, 30) == "A" &&
              f.table.axis_name(0, 87) == "F11" &&
              f.table.axis_name(0, 0) == "?" &&
              f.table.axis_name(0, 256).empty(),
          "");
    snprintf(buf, sizeof buf, "active %d (expected 30)", f.table.active_axis(0));
    check("keyboard: activeAxis finds the one key", f.table.active_axis(0) == 30,
          buf);
}

static void test_mouse()
{
    Fixture f;
    SyntheticBackend::Device &mouse = f.backend.devices[1];
    char buf[160];

    f.backend.interval_ms = 16.0f;
    mouse.raw.mouse[0] = 160;
    mouse.raw.mouse[1] = 32;
    mouse.raw.mouse_buttons[0] = 0x80;
    f.table.poll(f.backend);

    snprintf(buf, sizeof buf, "relative %.5f absolute %.5f",
             f.table.get_axis(1, 0), f.table.get_axis(1, 7));
    check("mouse: relative delta / dt * 0.005",
          near(f.table.get_axis(1, 0), 0.05f, 1e-6f), buf);
    check("mouse: absolute axis integrates and clamps",
          f.table.get_axis(1, 7) == 1.0f, "");
    check("mouse: buttons", f.table.get_axis(1, 3) == 1.0f &&
                                f.table.get_axis(1, 4) == 0.0f,
          "");
    check("mouse: kinds", f.table.kind(1, 0) == 1 && f.table.kind(1, 3) == 0 &&
                              f.table.kind(1, 7) == 1,
          "");
    check("mouse: control names", f.table.axis_name(1, 0) == "Mouse X" &&
                                      f.table.axis_name(1, 9) == "Mouse Z",
          "");

    mouse.raw.mouse[0] = 16000;
    f.table.poll(f.backend);
    check("mouse: relative value clamps to 1", f.table.get_axis(1, 0) == 1.0f,
          "");

    check("mouse: index -3 sets sensitivity 0.02",
          f.table.get_axis(1, -3) == 0.0f && f.table.get_axis(1, 7) == 0.0f, "");
    mouse.raw.mouse[0] = 64;
    f.table.poll(f.backend);
    snprintf(buf, sizeof buf, "absolute %.5f relative %.5f",
             f.table.get_axis(1, 7), f.table.get_axis(1, 0));
    check("mouse: sensitivity scales the integration",
          near(f.table.get_axis(1, 7), 0.02f, 1e-6f) &&
              near(f.table.get_axis(1, 0), 0.02f, 1e-6f),
          buf);

    f.backend.interval_ms = 0.0f;
    mouse.raw.mouse[0] = 100;
    f.table.poll(f.backend);
    check("mouse: zero poll interval gives zero velocity",
          f.table.get_axis(1, 0) == 0.0f, "");
}

static void test_joystick()
{
    Fixture f;
    SyntheticBackend::Device &stick = f.backend.devices[2];
    RawState &raw = stick.raw;
    raw.pov[0] = -1;
    raw.axes[0] = 0x8000;
    raw.axes[1] = 0;
    raw.axes[2] = 0xffff;
    raw.buttons[1] = 0x80;
    f.table.poll(f.backend);

    check("joystick: axis is (0x8000-raw)/0x8000",
          near(f.table.get_axis(2, 0), 0.0f, 1e-6f) &&
              f.table.get_axis(2, 1) == 1.0f &&
              near(f.table.get_axis(2, 2), -0.9999695f, 1e-5f),
          "");
    check("joystick: buttons", f.table.get_axis(2, 9) == 1.0f &&
                                   f.table.get_axis(2, 8) == 0.0f,
          "");
    check("joystick: centered POV is 0",
          f.table.get_axis(2, 12) == 0.0f && f.table.get_axis(2, 13) == 0.0f,
          "");

    raw.pov[0] = 0;
    f.table.poll(f.backend);
    check("joystick: POV up is +Y", near(f.table.get_axis(2, 12), 0.0f, 1e-6f) &&
                                        near(f.table.get_axis(2, 13), 1.0f, 1e-6f),
          "");
    raw.pov[0] = 9000;
    f.table.poll(f.backend);
    check("joystick: POV right is -X",
          near(f.table.get_axis(2, 12), -1.0f, 1e-4f) &&
              fabsf(f.table.get_axis(2, 13)) < 1e-4f,
          "");

    check("joystick: kinds",
          f.table.kind(2, 0) == 1 && f.table.kind(2, 8) == 0 &&
              f.table.kind(2, 12) == 1,
          "");
    check("joystick: control and device names",
          f.table.axis_name(2, 0) == "X Axis" &&
              f.table.axis_name(2, 13) == "POV 0" &&
              strcmp(f.table.device_name(2), "Test Stick") == 0,
          "");
    check("joystick: invalid indices", f.table.axis_name(2, 14).empty() &&
                                           f.table.get_axis(2, -1) == 0.0f &&
                                           f.table.get_axis(2, 14) == 0.0f,
          "");

    raw = RawState();
    raw.pov[0] = -1;
    for (int i = 0; i < 8; i++)
        raw.axes[i] = 0x8000;
    raw.axes[0] = 0x8000 - 16384;  // X = 0.5, all others centered
    f.table.poll(f.backend);
    check("joystick: activeAxis finds one axis", f.table.active_axis(2) == 0, "");
    raw.axes[1] = 0x8000 - 16384;
    f.table.poll(f.backend);
    check("joystick: activeAxis rejects two axes", f.table.active_axis(2) == -1,
          "");
}

static void test_controller_keys()
{
    Fixture f;
    Controller c(f.table);
    uint8_t &key = f.backend.devices[0].raw.keys[30];

    key = 0x80;
    f.table.poll(f.backend);
    begin_frame(0.016f);
    int slot = c.add(1, 0, 30, 0.0f, 1.0f, 0.0f, 1.0f);
    check("controller: add returns slot+1", slot == 1 && c.mapping_count() == 1,
          "");
    c.set_axis_speed(1, 0.01f);
    float v1 = c.get_axis_val(1);
    check("controller: step = dt * speed * rate", near(v1, 0.16f, 1e-6f), "");

    key = 0;
    f.table.poll(f.backend);
    float v2 = c.get_axis_val(1);
    check("controller: stamp caches within a frame", v2 == v1, "");
    begin_frame(0.016f);
    float v3 = c.get_axis_val(1);
    check("controller: centered input decays to 0", near(v3, 0.0f, 1e-6f), "");

    key = 0x80;
    f.table.poll(f.backend);
    begin_frame(0.016f);
    c.add(2, 0, 30, 0.0f, 1.0f, 0.0f, 4.0f);
    check("controller: mapping sum clamps to 1", c.get_axis_val(2) == 1.0f, "");

    begin_frame(0.016f);
    c.add(3, 0, 30, 0.0f, 0.5f, 0.0f, 1.0f);
    check("controller: raw outside the input range adds 0",
          c.get_axis_val(3) == 0.0f, "");

    begin_frame(0.016f);
    c.add(4, 0, 30, 0.0f, 1.0f, -1.0f, 1.0f);
    c.set_axis_speed(4, 0.03125f);
    float v6 = c.get_axis_val(4);
    check("controller: half-step reaches the middle", near(v6, 0.5f, 1e-6f), "");

    c.set_axis_smooth(4, 0.1f, 1.0f, 0.5f, 0.25f, 1.0f);
    begin_frame(0.016f);
    float v7 = c.get_axis_val(4);
    check("controller: 'same' factor scales the step", near(v7, 0.625f, 1e-6f),
          "");

    key = 0;
    f.table.poll(f.backend);
    begin_frame(0.016f);
    float v8 = c.get_axis_val(4);
    check("controller: 'opposite' factor scales the step",
          near(v8, 0.375f, 1e-6f), "");

    c.set_axis_smooth(4, 0.1f, 1.0f, 0.5f, 0.25f, 2.0f);
    float v9 = c.get_axis_val(4);
    check("controller: power curve squares the value",
          near(v9, 0.140625f, 1e-6f), "");

    c.add(5, 0, 30, 0.0f, 1.0f, -1.0f, 1.0f);
    c.set_axis_speed(5, 0.03125f);
    begin_frame(0.016f);
    float v10 = c.get_axis_val(5);
    check("controller: negative input half-step", near(v10, -0.5f, 1e-6f), "");
    c.set_axis_smooth(5, 0.1f, 1.0f, 1.0f, 1.0f, 2.0f);
    float v11 = c.get_axis_val(5);
    check("controller: power keeps the sign", near(v11, -0.25f, 1e-6f), "");

    c.reset();
    check("controller: reset clears mappings and restores defaults",
          c.mapping_count() == 0 && c.axis(4).power == 1.0f &&
              c.axis(4).speed == 1.0f && c.axis(4).center_range == 0.1f &&
              c.axis(4).kind == 0,
          "");
    begin_frame(0.016f);
    check("controller: reset axis evaluates to 0", c.get_axis_val(4) == 0.0f,
          "");
    check("controller: add rejects negative arguments",
          c.add(-1, 0, 0, 0, 1, 0, 1) == -1 &&
              c.add(1, -1, 0, 0, 1, 0, 1) == -1 &&
              c.add(1, 0, -1, 0, 1, 0, 1) == -1,
          "");

    int first = 0, last = 0;
    for (int i = 0; i < 152; i++) {
        int s = c.add(1, 0, 30, 0.0f, 1.0f, 0.0f, 1.0f);
        if (i == 0)
            first = s;
        if (i == 151)
            last = s;
    }
    check("controller: 152 mapping slots, then full",
          first == 1 && last == 152 && c.mapping_count() == 152 &&
              c.add(1, 0, 30, 0, 1, 0, 1) == 0,
          "");
    c.remove(1, 0, 30);
    check("controller: user_Del frees one match", c.mapping_count() == 151, "");
}

static void test_controller_kind_switch()
{
    Fixture f;
    Controller c(f.table);
    SyntheticBackend::Device &stick = f.backend.devices[2];
    stick.raw.pov[0] = -1;
    for (int i = 0; i < 8; i++)
        stick.raw.axes[i] = 0x8000;
    stick.raw.axes[0] = 0x8000 - 16384;  // 0.5
    f.table.poll(f.backend);
    begin_frame(0.016f);

    c.add(1, 2, 0, 0.0f, 1.0f, -1.0f, 1.0f);  // analog, kind 1
    c.add(1, 0, 30, 0.0f, 1.0f, 0.0f, 1.0f);  // key, kind 0
    check("controller: last add selects the axis kind", c.axis(1).kind == 0, "");
    float v1 = c.get_axis_val(1);
    check("controller: different-kind mapping is skipped",
          near(v1, 0.0f, 1e-6f) && c.axis(1).kind == 0, "");

    stick.raw.axes[0] = 0x8000;  // moved 0.5 = 50% of the 1.0 input range
    f.table.poll(f.backend);
    begin_frame(0.016f);
    float v2 = c.get_axis_val(1);
    check("controller: analog takeover after a >10% move",
          c.axis(1).kind == 1 && near(v2, -0.98f, 1e-5f), "");

    f.backend.devices[0].raw.keys[30] = 0x80;
    f.table.poll(f.backend);
    begin_frame(0.016f);
    float v3 = c.get_axis_val(1);
    // The switch happens mid-loop: the analog mapping already contributed -1
    // before the key mapping switched the kind and added +1.
    check("controller: key takeover switches kind back",
          c.axis(1).kind == 0 && near(v3, 0.0f, 1e-6f), "");
}

static void test_controller_mouse()
{
    Fixture f;
    Controller c(f.table);
    f.backend.interval_ms = 16.0f;
    f.backend.devices[1].raw.mouse[0] = 1600;  // 1600 / 16 * 0.005 = 0.5
    f.table.poll(f.backend);
    begin_frame(0.016f);
    c.add(1, 1, 0, -1.0f, 1.0f, -1.0f, 1.0f);
    check("controller: mouse mapping is analog", c.axis(1).kind == 1, "");
    float v = c.get_axis_val(1);
    check("controller: mouse axis passes through", near(v, 0.5f, 1e-6f), "");
}

static void test_controller_stick()
{
    Fixture f;
    Controller c(f.table);
    SyntheticBackend::Device &stick = f.backend.devices[2];
    stick.raw.pov[0] = -1;
    for (int i = 0; i < 8; i++)
        stick.raw.axes[i] = 0x8000;
    stick.raw.axes[0] = 0x8000 - 16384;  // 0.5
    f.table.poll(f.backend);
    begin_frame(0.016f);
    c.add(1, 2, 0, -1.0f, 1.0f, -1.0f, 1.0f);
    float v1 = c.get_axis_val(1);
    check("controller: joystick jumps to within 0.02", near(v1, 0.48f, 1e-5f),
          "");
    begin_frame(0.016f);
    float v2 = c.get_axis_val(1);
    check("controller: joystick blends by 0.3 inside 0.021",
          near(v2, 0.486f, 1e-5f), "");
}

static void test_controller_tick()
{
    Fixture f;
    Controller c(f.table);
    SyntheticBackend::Device &stick = f.backend.devices[2];
    stick.raw.pov[0] = -1;
    for (int i = 0; i < 8; i++)
        stick.raw.axes[i] = 0x8000;
    f.table.poll(f.backend);
    begin_frame(0.016f);
    c.add(2, 2, 0, -1.0f, 1.0f, -1.0f, 1.0f);  // kind 1
    c.set_axis_force(1, 1.0f, 1.0f);
    c.set_axis_force(2, 1.0f, 1.0f);
    c.tick();
    float expected = 1.0f * 1.0f * 1.5e-6f * 16.0f;
    check("controller: tick applies the force step",
          near(c.axis(1).value, expected, 1e-9f), "");
    check("controller: tick skips analog axes", c.axis(2).value == 0.0f, "");
}

static void test_hotkeys()
{
    Fixture f;
    Controller c(f.table);
    HotkeyTable ht;
    HotkeyState a, b, hk;
    int ha = 0, hb = 0, ho = 0;
    int osd1 = 0, osd2 = 0;

    ht.create(30, 2, &a, &ha, nullptr, 3);
    ht.create(31, 2, &b, &hb, nullptr, 3);
    f.table.poll(f.backend);
    ht.check(c, f.table, nullptr);
    check("hotkey: first check only arms", ht.queue.empty(), "");

    f.backend.devices[0].raw.keys[30] = 0x80;
    f.table.poll(f.backend);
    ht.check(c, f.table, nullptr);
    check("hotkey: press queues one event",
          ht.queue.size() == 1 && ht.queue[0].handler == &ha &&
              ht.queue[0].hotkey == &a && a.state == 1,
          "");

    f.backend.devices[0].raw.keys[31] = 0x80;
    f.table.poll(f.backend);
    ht.check(c, f.table, nullptr);
    check("hotkey: one event per call, first changed slot",
          ht.queue.size() == 2 && ht.queue[1].handler == &hb &&
              b.state == 1,
          "");

    f.backend.devices[0].raw.keys[30] = 0;
    f.backend.devices[0].raw.keys[31] = 0;
    f.table.poll(f.backend);
    ht.check(c, f.table, nullptr);
    check("hotkey: release fires the release mask",
          ht.queue.size() == 3 && ht.queue[2].handler == &ha &&
              a.state == 0 && b.state == 0,
          "");

    ht.remove(&b);
    check("hotkey: delete frees the slot", ht.slot(1).flags == 0, "");

    ht.create(32, 2, &hk, &ho, &osd1, 3);
    f.table.poll(f.backend);
    size_t n = ht.queue.size();
    ht.check(c, f.table, &osd2);
    check("hotkey: another OSD filters the slot out",
          ht.slot(1).latch == 0 && hk.state == 0, "");
    ht.check(c, f.table, &osd1);
    check("hotkey: matching OSD arms without firing",
          ht.slot(1).latch == 1 && ht.queue.size() == n, "");
    f.backend.devices[0].raw.keys[32] = 0x80;
    f.table.poll(f.backend);
    ht.check(c, f.table, &osd1);
    check("hotkey: matching OSD fires", ht.queue.size() == n + 1 &&
                                            ht.queue.back().handler == &ho,
          "");

    f.backend.devices[0].raw.keys[30] = 0x80;
    f.table.poll(f.backend);
    ht.check(c, f.table, &osd1);
    size_t m = ht.queue.size();
    check("hotkey: global slot fires alongside the OSD slot",
          m == n + 2 && ht.queue.back().hotkey == &a, "");

    ht.flush();
    f.backend.devices[0].raw.keys[30] = 0;
    f.table.poll(f.backend);
    ht.check(c, f.table, &osd1);
    check("hotkey: flush re-arms without firing", ht.queue.size() == m, "");
    f.backend.devices[0].raw.keys[30] = 0x80;
    f.table.poll(f.backend);
    ht.check(c, f.table, &osd1);
    check("hotkey: fires again after the flush",
          ht.queue.size() == m + 1 && ht.queue.back().hotkey == &a, "");
}

static void test_last_key()
{
    SyntheticBackend be;
    HotkeyTable ht;
    be.key_events.push_back(((uint32_t)'A' << 16) | 30);
    be.key_events.push_back(((uint32_t)'B' << 16) | 48);
    be.key_events.push_back(0x0000002au);  // ansi 0: not stored in the ring
    be.key_events.push_back(((uint32_t)'C' << 16) | 46);

    check("lastKey: returns the packed (ansi<<16)|scan",
          ht.last_key(be) == (((uint32_t)'A' << 16) | 30) &&
              ht.last_key(be) == (((uint32_t)'B' << 16) | 48),
          "");
    check("lastKey: ansi 0 keeps the ring unchanged",
          ht.last_key(be) == 0x2au && ht.last_pos == 2, "");
    check("lastKey: ring stores ANSI bytes in order",
          ht.last_key(be) == (((uint32_t)'C' << 16) | 46) &&
              ht.last_ring[0] == 'A' && ht.last_ring[1] == 'B' &&
              ht.last_ring[2] == 'C' && ht.last_pos == 3,
          "");

    SyntheticBackend be2;
    HotkeyTable ht2;
    for (int i = 0; i < 17; i++)
        be2.key_events.push_back(((uint32_t)('a' + i) << 16) | (uint32_t)(1 + i));
    for (int i = 0; i < 17; i++)
        ht2.last_key(be2);
    check("lastKey: 16-byte ring wraps",
          ht2.last_pos == 1 && ht2.last_ring[0] == 'q' &&
              ht2.last_ring[1] == 'b' && ht2.last_ring[15] == 'p',
          "");
}

static void test_cursor()
{
    class Source : public CursorSource {
      public:
        bool screen_pos(float *x, float *y) override
        {
            *x = 123.0f;
            *y = 456.0f;
            return true;
        }
        bool picked_pos(float position[3]) override
        {
            position[0] = 1.0f;
            position[1] = 2.0f;
            position[2] = 3.0f;
            return true;
        }
    } source;

    float x = 0.0f, y = 0.0f;
    float position[3] = {0.0f, 0.0f, 0.0f};
    check("cursor: no source behaves like the missing resource",
          !cursor_pos(&x, &y) && !cursor_picked_pos(position), "");
    set_cursor_source(&source);
    check("cursor: screen and picked hooks",
          cursor_pos(&x, &y) && x == 123.0f && y == 456.0f &&
              cursor_picked_pos(position) && position[0] == 1.0f &&
              position[1] == 2.0f && position[2] == 3.0f,
          "");
    set_cursor_source(nullptr);
}

int main()
{
    test_devices();
    test_mouse();
    test_joystick();
    test_controller_keys();
    test_controller_kind_switch();
    test_controller_mouse();
    test_controller_stick();
    test_controller_tick();
    test_hotkeys();
    test_last_key();
    test_cursor();

    printf("\n%d check(s) failed\n", failures);
    assert(failures == 0);
    return 0;
}
