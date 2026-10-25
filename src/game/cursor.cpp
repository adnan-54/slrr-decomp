#include "game/cursor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "input/controller.h"
#include "input/cursor.h"
#include "script/natives.h"
#include "script/vm.h"

namespace game {
namespace {

// One button's link set: left +0xd4/+0xe0/+0xb0/+0xec(ref)/+0x10c(body)/+0x11c,
// right +0xd8/+0xe4/+0xbc/+0xfc/+0x114/+0x128.
struct Button {
    int down = 0;
    int dragged = 0;
    float start[3] = {};
    uint32_t ref = 0;   // Group resource id, 0 = none
    uint32_t body = 0;  // PhysicsRef instance id, 0 = none
    float local[3] = {};
};

// The 0x1bc typed object (ctor 0x4b72f0), fields src needs.
struct Cursor {
    uint32_t active = 0;      // +0x10 bit0 ("activate")
    uint32_t controller = 0;  // +0x1c link (INVENTED: from "activate <a>")
    uint32_t flags = 3;       // +0x50 bit0 pick, bit2 standby
    int32_t color = -1;       // +0x4c
    int32_t mode = 1;         // +0x1a8
    char mode_ch[2] = {};     // +0x1ac
    float pos[3] = {};        // +0xa4
    float picked[3] = {};     // +0x164
    float pick_t = 0.0f;      // +0x170
    uint32_t hover_ref = 0;   // +0x144
    uint32_t hover_body = 0;  // +0x14c
    Button left, right;
};

Runtime *g_rt = nullptr;
std::map<uint32_t, Cursor> g_cursors;  // by cursor resource id
float g_sens[2] = {800.0f, 800.0f};    // DAT_005daa78 / DAT_005daa7c
uint32_t g_last_click_body = 0;

// 0x4b9b80 type object +0x20: drag threshold.
const float kDragThreshold = 0.02f;

void reset_if_new(Runtime &rt)
{
    if (g_rt == &rt) return;
    g_rt = &rt;
    g_cursors.clear();
    g_last_click_body = 0;
}

// 0x47c080 Config("mouseSensitivity") * 1000.
void read_sensitivity(Runtime &rt)
{
    // INVENTED: Config.java's 0.8 default when the statics are not loaded.
    float s = 0.8f;
    if (rt.vm()) {
        Value v;
        std::string e;
        if (rt.vm()->get_static_field("java.util.Config", "mouseSensitivity", &v, &e) &&
            v.kind == Value::Float)
            s = v.f;
    }
    g_sens[0] = g_sens[1] = s * 1000.0f;
}

void clamp1(float *v)
{
    if (*v > 1.0f) *v = 1.0f;
    if (*v < -1.0f) *v = -1.0f;
}

// 0x47d550 queue: EVENT_CURSOR (0x10000) with the message string.
void send(Runtime &rt, uint32_t target, uint32_t sender, const char *msg)
{
    Runtime::Resource *t = target ? rt.resolve(target) : nullptr;
    if (!t) return;
    Runtime::Resource *s = sender ? rt.resolve(sender) : nullptr;
    rt.queue_event(t, s, 0x10000, Value::make_str(std::string(msg)));
}

// INVENTED: the exe has one cursor; MouseCursor natives read the first live one.
class Source : public input::CursorSource {
  public:
    bool screen_pos(float *x, float *y) override
    {
        if (g_cursors.empty()) return false;
        const Cursor &c = g_cursors.begin()->second;
        *x = c.pos[0];
        *y = c.pos[1];
        return true;
    }
    bool picked_pos(float p[3]) override
    {
        if (g_cursors.empty()) return false;
        const Cursor &c = g_cursors.begin()->second;
        for (int i = 0; i < 3; i++) p[i] = c.picked[i];
        return true;
    }
};

// 0x4b8430 one button. Codes: down/up/click/drag begin/drag end/drop.
// 0x4b98c4..0x4b9950: RCLICK getPhysBody + 0x4a2b40/0x4af9a0 transform a dead temp; no port.
void button(Runtime &rt, uint32_t cur, Cursor &c, Button &b, bool pressed, bool hit,
            const Runtime::PickHit &h, bool is_left)
{
    char m[256];
    const int down = is_left ? 1 : 3, up = is_left ? 2 : 4, click = is_left ? 5 : 6;
    const int dbeg = is_left ? 9 : 12, dend = is_left ? 10 : 13, drop = is_left ? 11 : 14;
    const uint32_t hit_ref = hit && h.ref ? h.ref->id : 0;
    const uint32_t hit_body = hit && h.body ? h.body->id : 0;
    if (!pressed) {
        if (!b.down) return;
        b.down = 0;
        snprintf(m, sizeof m, "%d %d %d %d", up, cur, b.ref, b.body);
        if (is_left) {  // left: cursor first, right: ref first
            send(rt, cur, 0, m);
            if (b.ref) send(rt, b.ref, 0, m);
        } else {
            if (b.ref) send(rt, b.ref, 0, m);
            send(rt, cur, 0, m);
        }
        if (!b.dragged) {
            if (b.ref) {
                // The ref's message is formatted by a call Ghidra shows without
                // arguments; Group.handleEvent reads LCLICK token(2) = body.
                snprintf(m, sizeof m, "%d %d %d %.3f %.3f %.3f", click, cur, b.body,
                         b.local[0], b.local[1], b.local[2]);
                send(rt, b.ref, 0, m);
                if (is_left) g_last_click_body = b.body;
                snprintf(m, sizeof m, "%d %d %d %.3f %.3f %.3f %.3f %.3f %.3f", click, cur,
                         b.ref, b.local[0], b.local[1], b.local[2], c.picked[0],
                         c.picked[1], c.picked[2]);
                send(rt, cur, b.ref, m);
            }
        } else {
            snprintf(m, sizeof m, "%d %d %d", dend, cur, b.body);
            send(rt, cur, 0, m);
            if (b.ref) {
                send(rt, b.ref, 0, m);
                if (hit) {
                    snprintf(m, sizeof m, "%d %d %d %d %d", drop, cur, hit_body, b.ref, b.body);
                    send(rt, hit_ref, 0, m);
                    if (is_left)
                        snprintf(m, sizeof m, "%d %d %d %d %.3f %.3f %.3f %d", drop, cur,
                                 hit_ref, b.ref, c.picked[0], c.picked[1], c.picked[2],
                                 b.body);
                    else
                        snprintf(m, sizeof m, "%d %d %d %d %.3f %.3f %.3f", drop, cur,
                                 hit_ref, b.ref, c.picked[0], c.picked[1], c.picked[2]);
                    send(rt, cur, hit_ref, m);
                }
            }
        }
        b.ref = b.body = 0;  // 0x488080 / 0x501ca0
        return;
    }
    if (!b.down) {
        b.down = 1;
        b.dragged = 0;
        for (int i = 0; i < 3; i++) b.start[i] = c.pos[i];
        if (hit) {
            b.ref = hit_ref;
            b.body = hit_body;
            for (int i = 0; i < 3; i++) b.local[i] = h.point[i];
        } else {
            b.ref = b.body = 0;  // local point keeps its last value
        }
        if (is_left)
            snprintf(m, sizeof m, "%d %d %d %d %.3f %.3f %.3f", down, cur, b.ref, b.body,
                     b.local[0], b.local[1], b.local[2]);
        else
            snprintf(m, sizeof m, "%d %d %d %d", down, cur, b.ref, b.body);
        if (b.ref) send(rt, b.ref, 0, m);
        send(rt, cur, 0, m);
        return;
    }
    if (b.dragged) return;
    float d2 = 0.0f;
    for (int i = 0; i < 3; i++) d2 += (b.start[i] - c.pos[i]) * (b.start[i] - c.pos[i]);
    if (d2 < kDragThreshold * kDragThreshold) return;
    b.dragged = 1;
    if (!is_left) c.left.dragged = 1;  // 0x4b8430 sets +0xe0 with +0xe4
    snprintf(m, sizeof m, "%d %d %d", dbeg, cur, b.body);
    send(rt, cur, 0, m);
    if (b.ref) send(rt, b.ref, 0, m);
}

// 0x4b8430
void tick_one(Runtime &rt, uint32_t cur, Cursor &c, float dt)
{
    Runtime::Resource *ctl = c.controller ? rt.resolve(c.controller) : nullptr;
    if (!ctl) return;  // +0x24 == 0
    input::Controller *p = natives::controller_of(ctl);
    // getInfo 0x43 = the axis float, 0x19 = int(axis * 127) (car.cpp ai()).
    float dx = p->get_axis_val(0x2a) * g_sens[0] * dt;
    float dy = p->get_axis_val(0x2b) * g_sens[1] * dt;
    int moved = 0;
    if (dx * dx + dy * dy > 1.0000001e-06f) {
        moved = 1;
        if (c.flags & 4) c.flags |= 1;
    }
    c.pos[0] += dx;
    c.pos[1] += dy;
    clamp1(&c.pos[0]);
    clamp1(&c.pos[1]);
    if (!(c.flags & 1)) return;

    Runtime::PickHit h;
    float far[3], near[3];
    bool hit = rt.screen_pick(c.pos[0], c.pos[1], &h, far, near);
    for (int i = 0; i < 3; i++) c.picked[i] = (far[i] - near[i]) * h.t + near[i];
    c.pick_t = h.t;

    char m[256];
    const uint32_t ref = hit && h.ref ? h.ref->id : 0;
    const uint32_t body = hit && h.body ? h.body->id : 0;
    if (c.hover_ref != ref || c.hover_body != body) {
        if (c.hover_ref) {
            snprintf(m, sizeof m, "%d %d %d %d %d", 16, cur, c.hover_ref, c.hover_body, moved);
            send(rt, c.hover_ref, 0, m);
            send(rt, cur, 0, m);
        }
        c.hover_body = body;
        c.hover_ref = ref;
        if (c.hover_ref) {
            snprintf(m, sizeof m, "%d %d %d %d %d", 15, cur, c.hover_ref, c.hover_body, moved);
            send(rt, c.hover_ref, 0, m);
            send(rt, cur, 0, m);
        }
    }
    // The event handlers above may have destroyed the cursor.
    if (!rt.resolve(cur)) return;
    auto pressed = [p](int v) {
        return (float)(int)(p->get_axis_val(v) * 127.0f) * 0.007874016f > 0.2f;
    };
    button(rt, cur, c, c.left, pressed(0x2d), hit, h, true);
    if (!rt.resolve(cur)) return;
    button(rt, cur, c, c.right, pressed(0x2e), hit, h, false);
}

}  // namespace

void cursor_create(Runtime &rt, Runtime::Resource *inst)
{
    reset_if_new(rt);
    inst->event_mask |= 0x10u;  // 0x4ebd90(C, 0x10, 0)
    g_cursors[inst->id] = Cursor();
    read_sensitivity(rt);
    static Source source;
    input::set_cursor_source(&source);
}

int cursor_command(Runtime &rt, Runtime::Resource *cur, uint32_t code, const Value &param)
{
    if (!cur || g_rt != &rt) return 0;
    auto it = g_cursors.find(cur->id);
    if (it == g_cursors.end()) return 0;
    Cursor &c = it->second;
    if (code != 0x10 || param.kind != Value::Str) return 1;
    const char *s = param.s.c_str();
    char word[64] = {};
    sscanf(s, "%63s", word);
    if (!strcmp(word, "cursor")) return 1;  // resource loads only (0x51fa40/0x51f9b0)
    if (!strcmp(word, "mode")) {            // pointer shape: render only
        int id = 0;
        sscanf(s, "%*s %d %1s %d", &c.mode, c.mode_ch, &id);
        return 1;
    }
    if (strcmp(word, "color") != 0) {
        if (!strcmp(word, "activate")) {
            // INVENTED: the controller link C+0x1c is not traced; MouseCursor.enable
            // sends "activate "+controller.id(), so `a` is taken as the controller.
            const char *a = s + strlen(word);
            c.controller = (uint32_t)strtoll(a, nullptr, 0);
            c.active |= 1;
            return 1;
        }
        if (!strcmp(word, "deactivate")) {
            c.controller = 0;  // 0x488080
            c.active &= ~1u;
            return 1;
        }
        if (!strcmp(word, "enable")) {
            c.flags |= 1;
            return 1;
        }
        if (!strcmp(word, "disable"))
            c.flags &= ~5u;
        else if (!strcmp(word, "standby"))
            c.flags = (c.flags & ~1u) | 4;
        else {
            float x = 0.0f, y = 0.0f;
            if (!strcmp(word, "sens")) {
                read_sensitivity(rt);
            } else if (!strcmp(word, "relmove")) {
                sscanf(s, "%*s %f,%f", &x, &y);
                c.pos[0] += x;
                c.pos[1] += y;
                clamp1(&c.pos[0]);
                clamp1(&c.pos[1]);
            } else if (!strcmp(word, "move")) {
                sscanf(s, "%*s %f,%f", &x, &y);
                c.pos[0] = x;
                c.pos[1] = y;
                clamp1(&c.pos[0]);
                clamp1(&c.pos[1]);
            }
            // lock / unlock / setpos: SysCursor only.
            return 1;
        }
        c.hover_ref = c.hover_body = 0;  // 0x488080 / 0x501ca0
        return 1;
    }
    sscanf(s, "%*s %d", &c.color);  // 0x4ebd20: render only
    return 1;
}

void cursor_tick(Runtime &rt, float dt)
{
    if (g_rt != &rt) return;
    // INVENTED: snapshot of ids; handlers may create or destroy cursors.
    std::vector<uint32_t> ids;
    for (auto &kv : g_cursors) ids.push_back(kv.first);
    for (uint32_t id : ids) {
        if (!rt.resolve(id)) {
            g_cursors.erase(id);
            continue;
        }
        auto it = g_cursors.find(id);
        if (it == g_cursors.end() || !(it->second.active & 1)) continue;  // 0x4b79e0
        tick_one(rt, id, it->second, dt);
    }
}

uint32_t cursor_last_click_body() { return g_last_click_body; }

}  // namespace game
