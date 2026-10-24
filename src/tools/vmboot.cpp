// vmboot: end-to-end script boot over the shipped system package.
//   vmboot --selfcheck
// Asserts the package pair opens, game.Init resolves, its static initializer
// returns 100 (the value from the earlier classdump VM check), the native
// registry holds 422 entries, a fresh runtime re-run matches, missing classes
// are reported as external, and one more real class (Vector3) runs through the
// VM with native dispatch.
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "app/boot.h"
#include "game/car.h"
#include "input/backend.h"
#include "input/device.h"
#include "mesh/scx.h"
#include "parts/cfg.h"
#include "platform/fs.h"
#include "physics/mesh.h"
#include "physics/physics.h"
#include "physics/vclip.h"
#include "resource/manager.h"
#include "resource/rpk.h"
#include "script/classfile.h"
#include "script/compiler/decl.h"
#include "script/natives.h"
#include "script/packages.h"

namespace {

const char *kGameDir = "bin/the game";

// INVENTED: the exe enumerates DirectInput at startup (0x565fc0); a headless
// boot has no devices, so this substitutes a state source that lives for the
// process. The names are not invented: the saved control set remaps by name
// through Input.getDeviceName, and platform_backend.cpp uses exactly these.
input::SyntheticBackend *g_input = nullptr;

void install_synthetic_input()
{
    g_input = new input::SyntheticBackend();
    input::SyntheticBackend::Device keyboard;
    keyboard.desc.type = input::kKeyboard;
    keyboard.desc.name = "SysKeyboard";
    g_input->devices.push_back(keyboard);

    input::SyntheticBackend::Device mouse;
    mouse.desc.type = input::kMouse;
    mouse.desc.name = "SysMouse";
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
        input::ControlDesc control;
        control.id = c.id;
        control.name = c.name;
        mouse.desc.controls.push_back(control);
    }
    g_input->devices.push_back(mouse);
    input::install_backend(g_input);  // 0x565fc0
}

bool actual_state(Boot &boot, Value *state, std::string *err)
{
    if (!boot.vm()->get_static_field("java.game.GameLogic", "actualState", state, err))
        return false;
    if (state->kind != Value::Obj || !state->obj) {
        *err = "GameLogic.actualState is not an object";
        return false;
    }
    return true;
}

// Class name of a runtime class state, through its declaring unit.
const ClassUnit *unit_of(Boot &boot, ClassState *st)
{
    for (const ClassUnit *u : boot.classes()->units())
        if (boot.vm()->find_class(u->class_name(u->self_class)) == st) return u;
    return nullptr;
}

std::string object_class(Boot &boot, Object *o)
{
    const ClassUnit *u = o ? unit_of(boot, o->cls) : nullptr;
    return u ? u->class_name(u->self_class) : "(unknown)";
}

// ClassState is opaque outside the VM, so the super chain is walked through the
// units: each unit names its own class and its super.
bool is_a(Boot &boot, ClassState *cls, const std::string &name)
{
    for (ClassState *s = cls; s;) {
        const ClassUnit *u = unit_of(boot, s);
        if (!u) return false;
        if (u->class_name(u->self_class) == name) return true;
        std::string super = u->class_name(u->super_class);
        s = super.empty() ? nullptr : boot.vm()->find_class(super);
    }
    return false;
}

// INVENTED instrumentation: reads the shipped script state.
// GameLogic.actualState is the active MainMenu (GameLogic.java:164,896);
// MainMenu.window is the shown Gates (MainMenu.java:14-20); showMenu is 0 while
// the splash waits and 2 after showPrimaryVisuals (MainMenu.java:69,238).
bool menu_probe(Boot &boot, int *show, int *seek, std::string *err)
{
    Value state;
    if (!actual_state(boot, &state, err)) return false;
    Value window;
    if (!boot.vm()->get_instance_field(state.obj, "window", &window, err)) return false;
    if (window.kind != Value::Obj || !window.obj) {
        *err = "MainMenu.window is not an object";
        return false;
    }
    Value v;
    if (!boot.vm()->get_instance_field(window.obj, "showMenu", &v, err)) return false;
    if (v.kind != Value::Int) {
        *err = "showMenu is not an int";
        return false;
    }
    *show = v.i;
    if (!boot.vm()->get_instance_field(window.obj, "messageSeek", &v, err)) return false;
    if (v.kind != Value::Int) {
        *err = "messageSeek is not an int";
        return false;
    }
    *seek = v.i;
    return true;
}

// Vehicle.isDriveable (Vehicle.java:279): null when the car can be driven, else
// the script's error String.
std::string is_driveable_text(Boot &boot, Object *vehicle)
{
    std::string err;
    Value rv;
    if (!vehicle || !boot.runtime()->call_method(vehicle, "isDriveable", {}, &rv, &err))
        return "(call failed" + (err.empty() ? "" : ": " + err) + ")";
    if (rv.kind == Value::Null) return "null";
    if (rv.kind == Value::Str) return rv.s;
    return "(not a String)";
}

// INVENTED: stands in for a player picking the car this check is written for.
// True when the dealer's shown car (CarMarket.cars[curcar]) has a Duhen_D18V_block
// in its chassis slot 401 (_main_200S.cfg:387) and Vehicle.isDriveable() is null.
// Chassis.addStockParts rolls each stock part at random (Chassis.java:931-937),
// so some used cars come without an engine and other models carry other blocks.
bool shown_car_has_engine(Boot &boot, Object *market)
{
    std::string e;
    Value cars, cur;
    if (!boot.vm()->get_instance_field(market, "cars", &cars, &e) || cars.kind != Value::Array ||
        !cars.arr)
        return false;
    if (!boot.vm()->get_instance_field(market, "curcar", &cur, &e) || cur.kind != Value::Int ||
        cur.i < 0 || (size_t)cur.i >= cars.arr->elems.size())
        return false;
    const Value &car = cars.arr->elems[(size_t)cur.i];
    if (car.kind != Value::Obj || !car.obj) return false;
    Runtime *rt = boot.runtime();
    // INVENTED harness filter: the shown car must also be driveable.
    const std::string drive = is_driveable_text(boot, car.obj);
    std::printf("  browse car %s: isDriveable %s\n", object_class(boot, car.obj).c_str(),
                drive.c_str());
    Runtime::Handle *h = rt->handle(car.obj);
    game::PartData *pd = h && h->target ? game::part_data(*rt, h->target) : nullptr;
    if (!pd) return false;
    bool engine = false;
    for (game::Slot &sl : pd->slots)
        if (sl.id == 401)
            engine = sl.attached &&
                     object_class(boot, sl.attached).find("Duhen_D18V_block") !=
                         std::string::npos;
    return engine && drive == "null";
}

// Spawned like the runtime's THRD-CREATE threads so a dialog that sleeps or waits
// inside the handler continues in the frame loop instead of blocking here.
bool osd_command(Boot &boot, Object *receiver, int cmd, const char *thread_name,
                 std::string *err)
{
    std::vector<Value> args{Value::make_int(cmd)};
    ClassState *decl = nullptr;
    const MethodInfo *m = boot.vm()->find_method(receiver->cls, "osdCommand", "(I)V", 1,
                                                 args, &decl, err);
    if (!m) return false;
    const ClassUnit *unit = unit_of(boot, decl);
    if (!unit) {
        *err = "osdCommand declaring unit not found";
        return false;
    }
    return natives::spawn_untimed(*boot.vm(), boot.vm()->start(*unit, *m, receiver, args, err),
                                  10, thread_name);
}

// MainMenu.java:144-145: the invisible ghost button sends CMD_MENU_SELECT
// (0xB007); Gates.osdCommand (MainMenu.java:607) runs slider.execute(CMD_MENU_SEL).
bool menu_select(Boot &boot, Object *window, std::string *err)
{
    // INVENTED: stands in for the Osd ghost-button click (no render hit test)
    return osd_command(boot, window, 0xB007, "THRD-MENU-SELECT", err);
}

struct Checker {
    int checks = 0;
    int failures = 0;
    void check(bool ok, const std::string &what)
    {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        checks++;
        if (!ok) failures++;
    }
};

bool contains(const std::vector<std::string> &v, const std::string &s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

// Type-1 blob tree totals (notes/19 §7); only type-1 children are parsed.
struct MapTally {
    size_t instances = 0, phys_chunks = 0, faces = 0, verts = 0;
};

void walk_map_instance(const RpkArchive &rpk, const RpkRecord &rec, MapTally &t,
                       std::string *err)
{
    RpkInstanceData inst;
    if (!rpk.parse_instance(rec, &inst, err)) return;
    t.instances++;
    t.phys_chunks += (size_t)inst.phys_chunks;
    t.faces += inst.faces.size();
    t.verts += inst.verts.size() / 3;
    for (const RpkRecord &child : inst.children)
        if (child.type == 1) walk_map_instance(rpk, child, t, err);
}

// Same walk and order as game/runtime.cpp gather_ground_soup, plus the PHYS
// stored face normals (card 156 check 1).
void gather_phys_faces(const RpkArchive &rpk, const RpkRecord &rec, std::vector<Vec3> *verts,
                       std::vector<CollisionMesh::Triangle> *tris, std::vector<Vec3> *normals)
{
    RpkInstanceData inst;
    std::string err;
    if (!rpk.parse_instance(rec, &inst, &err)) return;
    uint32_t base = (uint32_t)verts->size();
    for (size_t i = 0; i + 2 < inst.verts.size(); i += 3)
        verts->push_back(vec3(inst.verts[i], inst.verts[i + 1], inst.verts[i + 2]));
    for (const RpkPhysFace &f : inst.faces) {
        tris->push_back(CollisionMesh::Triangle{{base + f.v[0], base + f.v[1], base + f.v[2]}});
        normals->push_back(vec3(f.n[0], f.n[1], f.n[2]));
    }
    for (const RpkRecord &child : inst.children)
        if (child.type == 1) gather_phys_faces(rpk, child, verts, tris, normals);
}

std::string lower_ascii(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

uint32_t float_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

// Card 158: 0x47be30 (World::step C) runs once per executed 0x502060 step.
// The probe wraps the world's drive slot to see that call site; it forwards to
// game::drive_cars and checks the hook only accumulates force/torque
// (0x504e70/0x5051e0): car_root vel and angular_momentum stay bitwise equal.
struct DriveProbe {
    RigidBody *root = nullptr;
    unsigned calls_awake = 0;
    unsigned bad_velocity = 0;
    unsigned bad_momentum = 0;
};
DriveProbe g_drive_probe;

void probe_drive(float dt)
{
    DriveProbe &p = g_drive_probe;
    RigidBody *root = p.root;
    Vec3 vel = root ? root->vel : Vec3{0.0f, 0.0f, 0.0f};
    Vec3 mom = root ? root->angular_momentum : Vec3{0.0f, 0.0f, 0.0f};
    bool awake = root && !root->asleep;
    game::drive_cars(dt);
    if (!root) return;
    if (awake) p.calls_awake++;
    if (float_bits(root->vel.x) != float_bits(vel.x) ||
        float_bits(root->vel.y) != float_bits(vel.y) ||
        float_bits(root->vel.z) != float_bits(vel.z))
        p.bad_velocity++;
    if (float_bits(root->angular_momentum.x) != float_bits(mom.x) ||
        float_bits(root->angular_momentum.y) != float_bits(mom.y) ||
        float_bits(root->angular_momentum.z) != float_bits(mom.z))
        p.bad_momentum++;
}

const ClassUnit *load_extra(Boot &boot, const std::string &rel, const std::string &cls,
                            std::string *err)
{
    DataRef ref;
    ref.kind = DataRef::Script;
    ref.text = rel;
    ref.path = boot.resources()->resolve_path(rel);
    ref.file = !ref.path.empty();
    ref.is_class = true;
    if (!ref.file) {
        *err = "not found: " + rel;
        return nullptr;
    }
    if (!boot.resources()->load_class(ref, boot.classes(), err)) return nullptr;
    for (const ClassUnit *unit : boot.classes()->units()) boot.vm()->add_class(unit);
    ClassEntry *entry = boot.classes()->find(cls);
    if (!entry) {
        *err = cls + " unit not registered";
        return nullptr;
    }
    return entry->unit;
}

// One more real package class through the interpreter: load Vector3, run its
// bytecode constructor, then dispatch the native length().
void vector3_check(Boot &boot, Checker &c)
{
    std::string err;
    // Vector3's constructor chains to java.lang.Object.<init>, so the super the
    // boot reported external has to be provided for bytecode execution.
    const ClassUnit *object = load_extra(boot, "system\\scripts\\lang\\Object.class",
                                         "java.lang.Object", &err);
    c.check(object != nullptr, "Object.class loads through the package" +
                                   (object ? std::string() : ": " + err));
    const ClassUnit *unit =
        load_extra(boot, "system\\scripts\\lang\\Vector3.class", "java.lang.Vector3", &err);
    c.check(unit != nullptr, "Vector3.class loads through the package" +
                                 (unit ? std::string() : ": " + err));
    if (!unit) return;
    ClassState *vs = boot.vm()->find_class("java.lang.Vector3");
    c.check(vs != nullptr, "java.lang.Vector3 registered in the VM");
    if (!vs) return;

    const MethodInfo *ctor = nullptr, *length = nullptr;
    for (const MethodInfo &m : unit->methods) {
        const char *n = unit->str(m.name);
        const char *s = unit->str(m.sig);
        if (!n || !s) continue;
        if (strcmp(n, "<init>") == 0 && strcmp(s, "(FFF)") == 0) ctor = &m;
        if (strcmp(n, "length") == 0 && strcmp(s, "()F") == 0) length = &m;
    }
    c.check(ctor != nullptr && length != nullptr, "Vector3(FFF) and length()F found");
    if (!ctor || !length) return;

    Object *obj = boot.vm()->new_object(vs, &err);
    c.check(obj != nullptr, "Vector3 instance created" + (obj ? std::string() : ": " + err));
    if (!obj) return;

    Value result;
    // (3,4,0): no dependency on argument slot order for this assertion.
    bool ok = boot.vm()->invoke(*unit, *ctor, obj,
                                {Value::make_float(3.0f), Value::make_float(4.0f),
                                 Value::make_float(0.0f)},
                                &result, &err);
    c.check(ok, "Vector3(3,4,0) constructor executes" + (ok ? std::string() : ": " + err));
    if (!ok) return;

    err.clear();
    ok = boot.vm()->invoke(*unit, *length, obj, {}, &result, &err);
    char got[64];
    snprintf(got, sizeof got, "%g", ok && result.kind == Value::Float ? (double)result.f : 0.0);
    c.check(ok && result.kind == Value::Float && std::fabs(result.f - 5.0f) < 1e-4f,
            std::string("Vector3.length() native returns 5 (got ") + got + ")" +
                (ok ? std::string() : ": " + err));
}

// Thread state words used by the THRD-CREATE line and the per-thread report.
std::string thread_state(const natives::ThreadInfo &t)
{
    if (!t.error.empty()) return "failed: " + t.error;
    if (t.done) return "done";
    std::string at = t.where.empty() ? std::string() : " at " + t.where;
    if (t.sleeping) return "stopped (sleeping)" + at;
    if (t.waiting) return "stopped (waiting)" + at;
    if (t.suspended) return "stopped (suspended)" + at;
    return "running (yielded)" + at;
}

// Dialog.display() waits on the dialog object (Dialog.java:213-220) until its
// osdCommand(0) resolves the wait. StringRequesterDialog's OK path also needs
// its `input`: the shipped 2.3.1 class checks the gadget's osd.stringValue for
// SIF_NOEMPTY (2.2.1 source: input), so both get the name.
// INVENTED: stands in for clicking the dialog's Osd buttons (no render hit test).
void answer_dialogs(Boot &boot, std::vector<Object *> &answered, int frame)
{
    for (const natives::ThreadInfo &t : natives::thread_report(*boot.vm())) {
        Object *d = t.wait_on;
        if (!t.waiting || !d || !is_a(boot, d->cls, "java.render.osd.dialog.Dialog"))
            continue;
        // One OK per dialog: several threads may wait on the same object.
        if (std::find(answered.begin(), answered.end(), d) != answered.end()) continue;
        answered.push_back(d);
        const std::string cls = object_class(boot, d);
        std::string err;
        // TextDialog.text (Dialog.java:530): the buy dialog's body carries the price.
        if (is_a(boot, d->cls, "java.render.osd.dialog.NoYesDialog")) {
            Value text;
            if (boot.vm()->get_instance_field(d, "text", &text, &err) &&
                text.kind == Value::Str)
                std::printf("  NoYesDialog text: %s\n", text.s.c_str());
            else
                std::printf("  NoYesDialog text: (unread%s%s)\n",
                            err.empty() ? "" : ": ", err.c_str());
        }
        if (is_a(boot, d->cls, "java.render.osd.dialog.StringRequesterDialog")) {
            Value input;
            if (!boot.vm()->get_instance_field(d, "input", &input, &err) ||
                input.kind != Value::Str || input.s.empty()) {
                boot.vm()->set_instance_field(d, "input", Value::make_str("vmboot"), &err);
                Value osd;
                if (boot.vm()->get_instance_field(d, "osd", &osd, &err) &&
                    osd.kind == Value::Obj && osd.obj)
                    boot.vm()->set_instance_field(osd.obj, "stringValue",
                                                  Value::make_str("vmboot"), &err);
            }
        }
        bool ok = osd_command(boot, d, 0, "THRD-DIALOG-OK", &err);
        if (ok)
            std::printf("  dialog OK: %s (frame %d)\n", cls.c_str(), frame);
        else
            std::printf("  dialog %s: not answered%s\n", cls.c_str(),
                        err.empty() ? "" : (" (" + err + ")").c_str());
    }
}

// 0x47d760 init step: Config.init -> type record "Init" (sl.rpk, type 8) ->
// scripted GameInit instance; its Init(int) constructor runs in THRD-CREATE.
// A failing constructor is an expected finding here (missing engine services):
// it is printed, not asserted.
void game_init_check(Boot &boot, Checker &c)
{
    std::string err;
    bool ok = boot.game_init(&err);
    c.check(ok, "Config.init creates the GameInit instance" +
                    (ok ? std::string() : ": " + err));

    // Same stored field, looked up straight in the open rpk: the shipped
    // sl:0x40 is the type-8 record "Init" (resman check, notes/16).
    Value v;
    std::string e;
    const RpkRecord *rec = nullptr;
    if (boot.vm()->get_static_field("java.util.Config", "init", &v, &e) &&
        v.kind == Value::Int && v.i) {
        uint32_t id = (uint32_t)v.i;
        const Package *pkg = boot.resources()->registry_package(id >> 16);
        if (pkg) rec = pkg->rpk.find(id & 0xffffu);
    }
    c.check(rec && rec->name == "Init" && rec->type == 8,
            "Config.init resolves to type-8 record \"Init\" in sl.rpk" +
                (rec ? " (got \"" + rec->name + "\" type " +
                           std::to_string(rec->type) + ")"
                     : std::string()));
    const BootResult &r = boot.result();
    c.check(r.game_init_created && r.game_init_class == "java.game.Init",
            "GameInit instance uses script class java.game.Init (got " +
                r.game_init_class + ")");

    bool found = false;
    natives::ThreadInfo create;
    for (const natives::ThreadInfo &t : natives::thread_report(*boot.vm()))
        if (t.name == "THRD-CREATE") {
            create = t;
            found = true;
        }
    c.check(found, "Init(int) runs in a THRD-CREATE thread");
    if (!found) return;
    std::printf("  THRD-CREATE: %s\n", thread_state(create).c_str());
    c.check(create.done && create.error.empty(), "shipped Init(int) constructor runs to completion");

    // Drive the game like the exe's main loop after boot, on a simulated clock:
    // 16 ms steps, never the wall clock.
    // INVENTED: the press/release frames (the exe reads a real keyboard) and the
    // 1 s hold so the 50 ms Frontend HotkeyWatcher tick samples it. Scan 0x1c is
    // ControlSet's MENUSET AXIS_SELECT bind (RCDIK_ENTER); 0x544830 reads the
    // raw key byte's high bit.
    const int kFrames = 30000, kPress = 3000, kRelease = 3060, kSelect = 3600;
    const int kGarageCmd = 6300, kBuyCmd = 12300, kBackCmd = 14000;
    // Garage.java:99,1598 CMD_TESTDRIVE -> freeride EventList; EventList.java:52,537
    // CMD_RACE -> freerideSetup() + java.game.Track.
    const int kTestDriveCmd = 18300, kRaceCmd = 20000;
    FrameLoop loop(0);
    // Card 158: sit on the world's drive slot (0x502060 C); advance_sim only
    // installs the default when the slot is empty.
    process_world().drive = probe_drive;
    for (int i = 1; i < kPress; i++) loop.frame(i * 16);
    int show_before = -1, seek_before = -1;
    std::string perr;
    bool probed = menu_probe(boot, &show_before, &seek_before, &perr);
    c.check(probed && show_before == 0,
            "main menu waits for input before the press (showMenu == 0, got " +
                std::to_string(show_before) + (probed ? "" : ": " + perr) + ")");
    for (int i = kPress; i <= kSelect; i++) {
        g_input->devices[0].raw.keys[0x1c] = i < kRelease ? 0x80 : 0;
        loop.frame(i * 16);
    }

    // Click the current menu item, then let the handler and the menu animate.
    Value state_before;
    std::string serr;
    bool have_state = actual_state(boot, &state_before, &serr);
    const std::string class_before =
        have_state ? object_class(boot, state_before.obj) : "(none)";
    bool selected = false;
    if (have_state) {
        Value window;
        if (boot.vm()->get_instance_field(state_before.obj, "window", &window, &serr) &&
            window.kind == Value::Obj && window.obj)
            selected = menu_select(boot, window.obj, &serr);
        else if (serr.empty())
            serr = "MainMenu.window is not an object";
    }
    // The NEW CAREER dialog is up once the select handler has run; answer every
    // modal dialog that shows up from then on (loading screen, name, garage),
    // then let each handler continue.
    const int kDialog = 4200, kDialogStep = 300;
    std::vector<Object *> answered;
    // Garage section at the time of the dealer command (dealer_from) and whether
    // the CMD_BUYCARSUSED (122) handler was started.
    std::string dealer_from = "(none)";
    std::string buy_from = "(none)";
    bool dealer_at_garage = false, dealer_started = false, buy_at_market = false;
    std::string testdrive_from = "(none)", race_from = "(none)";
    bool testdrive_at_garage = false, race_at_eventlist = false;
    for (int i = kSelect + 1; i <= kFrames; i++) {
        g_input->devices[0].raw.keys[0x1c] = 0;
        loop.frame(i * 16);
        if (i >= kDialog && (i - kDialog) % kDialogStep == 0)
            answer_dialogs(boot, answered, i);
        if (i == kGarageCmd) {
            // Garage.java:1724,1739: CMD_BUYCARSUSED opens the used car dealer
            // between 7 and 17 h. getTime() is a plain getter for the static
            // float `time` (GameLogic.java:178,682-685).
            Value st;
            std::string gerr;
            if (actual_state(boot, &st, &gerr)) {
                dealer_from = object_class(boot, st.obj);
                dealer_at_garage = is_a(boot, st.obj->cls, "java.game.Garage");
            }
            if (dealer_at_garage) {
                Value t;
                if (boot.vm()->get_static_field("java.game.GameLogic", "time", &t, &gerr) &&
                    t.kind == Value::Float)
                    std::printf("  garage cmd 122 at game hour %g\n",
                                (double)(t.f / 3600.0f));
                // INVENTED: stands in for clicking the Used Car Dealer button
                dealer_started = osd_command(boot, st.obj, 122, "THRD-GARAGE-CMD", &gerr);
                // A handler failing inside its first slice also returns false but
                // keeps its thread in the report; only a start failure sets err.
                if (!dealer_started && !gerr.empty())
                    std::printf("  garage cmd 122: not started (%s)\n", gerr.c_str());
            } else {
                std::printf("  garage cmd skipped: actualState is %s\n",
                            dealer_from.c_str());
            }
        }
        if (i >= kBuyCmd - 2000 && i < kBuyCmd && (i - (kBuyCmd - 2000)) % 100 == 0) {
            // Browse right (CarMarket.java:824-827, Input.AXIS_MENU_RIGHT 58) until
            // the shown car has the D18V block; stops at the first car that has it.
            Value st;
            std::string berr;
            if (actual_state(boot, &st, &berr) &&
                object_class(boot, st.obj) == "java.game.CarMarket" &&
                !shown_car_has_engine(boot, st.obj))
                osd_command(boot, st.obj, 58, "THRD-MARKET-BROWSE", &berr);
        }
        if (i == kBuyCmd || i == kBackCmd) {
            // CarMarket.java:817-867,1013-1153: AXIS_SELECT buys the shown car
            // (AXIS_CANCEL in browse mode returns to the Garage).
            Value st;
            std::string merr;
            if (actual_state(boot, &st, &merr)) {
                const std::string cls = object_class(boot, st.obj);
                const int cmd = i == kBuyCmd ? 34 : 35;  // Input.AXIS_SELECT / AXIS_CANCEL
                if (i == kBuyCmd) {
                    buy_from = cls;
                    buy_at_market = cls == "java.game.CarMarket";
                }
                if (cls == "java.game.CarMarket") {
                    // INVENTED: stands in for the dealer's Osd menu button.
                    // NoYesDialog's display() returns osdCommand's argument
                    // (Dialog.java:204-220) and its YES button is index 0
                    // (buttonTexts "YES;NO", Dialog.java:138-153).
                    bool ok = osd_command(boot, st.obj, cmd,
                                          i == kBuyCmd ? "THRD-MARKET-BUY"
                                                       : "THRD-MARKET-BACK", &merr);
                    std::printf("  cmd %d -> %s (frame %d)\n", cmd, cls.c_str(), i);
                    if (!ok && !merr.empty())
                        std::printf("  cmd %d: not started (%s)\n", cmd, merr.c_str());
                } else {
                    std::printf("  cmd %d skipped: actualState is %s (frame %d)\n",
                                cmd, cls.c_str(), i);
                }
            }
        }
        if (i == kTestDriveCmd) {
            // Garage.java:99,1598: CMD_TESTDRIVE changes the section to a
            // freeride EventList.
            Value st;
            std::string terr;
            if (actual_state(boot, &st, &terr)) {
                testdrive_from = object_class(boot, st.obj);
                testdrive_at_garage = is_a(boot, st.obj->cls, "java.game.Garage");
                if (testdrive_at_garage) {
                    // INVENTED: stands in for clicking the Garage Test Drive button.
                    bool ok = osd_command(boot, st.obj, 134, "THRD-GARAGE-TESTDRIVE", &terr);
                    if (!ok && !terr.empty())
                        std::printf("  garage cmd 134: not started (%s)\n", terr.c_str());
                } else {
                    std::printf("  garage cmd 134 skipped: actualState is %s\n",
                                testdrive_from.c_str());
                }
            }
        }
        if (i == kRaceCmd) {
            // EventList.java:44,345: CMD_RACE belongs to the Generator dialog,
            // not the EventList (which only holds it in eGen, EventList.java:22);
            // the Generator's osdCommand freerideSetup()s and changes the section
            // to java.game.Track (EventList.java:52,537-545).
            Value st;
            std::string rerr;
            if (actual_state(boot, &st, &rerr)) {
                race_from = object_class(boot, st.obj);
                race_at_eventlist = is_a(boot, st.obj->cls, "java.game.EventList");
                Object *gen = nullptr;
                if (race_at_eventlist) {
                    Value gv;
                    if (boot.vm()->get_instance_field(st.obj, "eGen", &gv, &rerr) &&
                        gv.kind == Value::Obj)
                        gen = gv.obj;
                }
                if (gen) {
                    // INVENTED: stands in for clicking the freeride Race button.
                    bool ok = osd_command(boot, gen, 0x10E, "THRD-EVENT-RACE", &rerr);
                    if (!ok && !rerr.empty())
                        std::printf("  eventlist cmd 0x10E: not started (%s)\n",
                                    rerr.c_str());
                } else if (race_at_eventlist) {
                    std::printf("  eventlist cmd 0x10E: EventList.eGen is null\n");
                } else {
                    std::printf("  eventlist cmd 0x10E skipped: actualState is %s\n",
                                race_from.c_str());
                }
            }
        }
    }

    Value state_after;
    serr.clear();
    bool have_state_after = actual_state(boot, &state_after, &serr);
    const std::string class_after =
        have_state_after ? object_class(boot, state_after.obj) : "(none)";
    std::printf("  menu select: actualState %s -> %s%s\n", class_before.c_str(),
                class_after.c_str(),
                selected ? "" : (" (invoke failed: " + serr + ")").c_str());
    std::printf("  final actualState: %s\n", class_after.c_str());

    // 0x47e050 sub-steps the process world (0x502060); 0x4e5810 System.simTime
    // returns that world's time (float at 0x60251c+0), not a separate clock.
    c.check(process_world().time > 0.0f,
            "process world time advances over the run (got " +
                std::to_string((double)process_world().time) + ")");
    c.check((float)natives::sim_time(*boot.vm()) == process_world().time,
            "System.simTime equals the process world time");
    std::printf("  process world time after %d frames: %.3f s\n", kFrames,
                (double)process_world().time);

    // GameLogic.player (GameLogic.java:173); the owned vehicle is Racer.car
    // (Racer.java:31), assigned by buyCar before returning to the Garage
    // (CarMarket.java:1124).
    Object *player_car = nullptr;
    Value pv;
    if (boot.vm()->get_static_field("java.game.GameLogic", "player", &pv, &perr) &&
        pv.kind == Value::Obj && pv.obj) {
        Value cv;
        if (boot.vm()->get_instance_field(pv.obj, "car", &cv, &perr) && cv.kind == Value::Obj)
            player_car = cv.obj;
    }
    std::printf("  player car: %s\n",
                player_car ? object_class(boot, player_car).c_str() : "null");

    // MainMenu.java:436: display() == 0 sends the NEW CAREER handler on to
    // GameLogic.changeActiveSection(GameLogic.garage); the MainMenu fields are
    // gone once the section changed, so the menu probe no longer applies.
    const bool state_changed = have_state_after && class_after != class_before;
    int show_after = -1, seek_after = -1;
    perr.clear();
    bool probed_after = false;
    if (state_changed)
        perr = "actualState is no longer MainMenu";
    else
        probed_after = menu_probe(boot, &show_after, &seek_after, &perr);
    std::printf("  menu probe: showMenu %d -> %d, messageSeek %d -> %d%s\n",
                show_before, show_after, seek_before, seek_after,
                probed_after ? "" : (" (" + perr + ")").c_str());
    if (state_changed)
        c.check(dealer_at_garage,
                "name dialog OK moves actualState to java.game.Garage (got " +
                    dealer_from + " at the dealer command)");
    else
        c.check(probed_after && show_after >= 2,
                "Enter press runs AXIS_SELECT -> showPrimaryVisuals (showMenu >= 2, got " +
                    std::to_string(show_after) + (probed_after ? "" : ": " + perr) + ")");
    if (dealer_at_garage) {
        // Shipped: CMD_BUYCARSUSED within opening hours switches the section to
        // the used car market (Garage.java:1724-1788); read at the buy step.
        c.check(buy_at_market, "Used Car Dealer opens: actualState " + dealer_from +
                                   " -> " + buy_from);
    }
    // Shipped: the purchase's own AXIS_CANCEL (CarMarket.java:1139) or the
    // frame-14000 AXIS_CANCEL returns to the Garage with player.car set; read
    // there because CMD_TESTDRIVE changes the section again.
    c.check(testdrive_from == "java.game.Garage",
            "back from the used dealer: actualState is java.game.Garage at the testdrive "
            "cmd (got " + testdrive_from + ")");
    c.check(player_car != nullptr, "buying the shown used car sets GameLogic.player.car");

    // Vehicle.java:49 holds the Chassis script object; its handle target is the
    // instance create_car_bodies bound the payload to (0x4c1f90/0x4992e0).
    game::CarData *car_payload = nullptr;
    Runtime::Resource *car_res = nullptr;
    {
        Value chv;
        std::string cherr;
        Runtime::Handle *ch = nullptr;
        if (player_car &&
            boot.vm()->get_instance_field(player_car, "chassis", &chv, &cherr) &&
            chv.kind == Value::Obj && chv.obj)
            ch = boot.runtime()->handle(chv.obj);
        if (ch && ch->target) car_res = ch->target;
        if (car_res) car_payload = game::car_data(*boot.runtime(), car_res);
    }
    bool root_sleeping = false;
    if (car_payload && car_payload->root_body)
        for (RigidBody *b : process_world().sleeping)
            if (b == car_payload->root_body) root_sleeping = true;
    c.check(car_payload && car_payload->chassis_body && car_payload->root_body &&
                car_payload->chassis_body->parent == car_payload->root_body,
            "player car chassis body hangs under its car_root body (0x4992e0)");
    c.check(root_sleeping, "car_root body is in the world's sleeping list (0x506d90)");

    // Card 150: Vehicle's ctor ran Chassis.addStockParts -> Part.addPart, whose
    // "install 0 <car> 0 <this> 0" command attaches the part (0x4c4400 ->
    // 0x4c96c0 -> 0x4cb1d0). _main_200S.cfg:387: chassis slot 401 attaches part
    // 0x000A009B at its slot 99 (Duhen_D18V_block).
    Runtime *rt = boot.runtime();
    game::PartData *chassis_parts = car_res ? game::part_data(*rt, car_res) : nullptr;
    game::Slot *engine_slot = nullptr;
    if (chassis_parts)
        for (game::Slot &s : chassis_parts->slots)
            if (s.id == 401) engine_slot = &s;
    Object *engine_obj = engine_slot ? engine_slot->attached : nullptr;
    Runtime::Handle *eh = engine_obj ? rt->handle(engine_obj) : nullptr;
    Runtime::Resource *engine_res = eh ? eh->target : nullptr;
    game::PartData *engine_parts = engine_res ? game::part_data(*rt, engine_res) : nullptr;
    game::Slot *engine_own = nullptr;
    if (engine_parts)
        for (game::Slot &s : engine_parts->slots)
            if (s.id == 99) engine_own = &s;
    c.check(engine_obj && engine_slot->mate_id == 99,
            "chassis slot 401 attaches a part at its slot 99 (0x4cb1d0)");
    c.check(engine_obj && object_class(boot, engine_obj).find("Duhen_D18V_block") !=
                              std::string::npos,
            "chassis slot 401's part is a Duhen_D18V_block (got " +
                (engine_obj ? object_class(boot, engine_obj) : std::string("(none)")) + ")");
    c.check(engine_own && engine_own->attached == car_res->script &&
                engine_own->mate_id == 401,
            "the block's slot 99 points back at the chassis slot 401 (0x4cb1d0)");
    c.check(engine_res && car_res && engine_res->parent == car_res,
            "install reparents the part under the car (0x4ec210, Part.addPart's test)");
    c.check(engine_parts && car_res && engine_parts->car_ref == car_res &&
                engine_parts->car_id == (int32_t)car_res->id,
            "the installed part's Part.getCar() is the car (0x4c7d20 getInfo 0x2b)");
    game::Slot *crank_slot = nullptr;
    if (engine_parts)
        for (game::Slot &s : engine_parts->slots)
            if (s.id == 40) crank_slot = &s;
    c.check(crank_slot && crank_slot->attached,
            "the block's slot 40 holds the crankshaft (Duhen_D18V_block.java:99)");
    int installed_parts = 0;
    float installed_cfg_mass = 0.0f;
    {
        std::vector<Runtime::Resource *> stack;
        std::set<Runtime::Resource *> seen;
        if (car_res) stack.push_back(car_res);
        while (!stack.empty()) {
            Runtime::Resource *r = stack.back();
            stack.pop_back();
            game::PartData *p = game::part_data(*rt, r);
            if (!p) continue;
            for (game::Slot &s : p->slots) {
                if (!s.attached) continue;
                Runtime::Handle *sh = rt->handle(s.attached);
                Runtime::Resource *t = sh ? sh->target : nullptr;
                if (t && seen.insert(t).second) {
                    installed_parts++;
                    // 0x5032d0: every installed part's cfg `body` lines join the
                    // compound. Read them from the shipped files, not the payload.
                    CfgFile part_cfg;
                    if (game::load_part_cfg(*rt, t, &part_cfg))
                        for (const CfgBody &pb : part_cfg.bodies)
                            if (pb.mass > 0.0f) installed_cfg_mass += pb.mass;
                    stack.push_back(t);
                }
            }
        }
    }
    std::printf("  installed parts on the car (recursive slot walk): %d, "
                "cfg body mass %.3f kg\n",
                installed_parts, (double)installed_cfg_mass);

    // Card 163: 0x4ca390 reports every committed part through 0x4942b0, which
    // appends it to the car list (car+0xfc) and calls the Chassis script's
    // addpart(GameRef) -> Part.attachedParts (Part.java:263).
    int attached_count = -1;
    std::vector<Value> attached;
    Object *chassis_obj = car_res ? car_res->script : nullptr;
    {
        Value apv, cnt, data;
        std::string aerr;
        if (chassis_obj &&
            boot.vm()->get_instance_field(chassis_obj, "attachedParts", &apv, &aerr) &&
            apv.kind == Value::Obj && apv.obj &&
            boot.vm()->get_instance_field(apv.obj, "elementCount", &cnt, &aerr) &&
            cnt.kind == Value::Int && cnt.i >= 0) {
            attached_count = cnt.i;
            if (boot.vm()->get_instance_field(apv.obj, "elementData", &data, &aerr) &&
                data.kind == Value::Array && data.arr)
                for (int i = 0; i < attached_count && i < (int)data.arr->elems.size(); i++)
                    attached.push_back(data.arr->elems[(size_t)i]);
        }
    }
    int part_list_n = car_payload ? (int)car_payload->part_list.size() : 0;
    // The walk reaches the chassis again through the engine's back-pointer.
    const int walk_parts = installed_parts - (installed_parts > 0 ? 1 : 0);
    std::printf("  attachedParts count %d, recursive slot walk %d (%d without the chassis), "
                "car+0xfc list %d\n",
                attached_count, installed_parts, walk_parts, part_list_n);
    c.check(attached_count >= 0 && attached_count == walk_parts &&
                attached_count == part_list_n,
            "Part.attachedParts count = installed parts = car+0xfc list (0x4942b0)");
    std::set<Object *> distinct;
    bool all_parts = attached_count > 0 && (int)attached.size() == attached_count;
    bool battery = false, block = false;
    for (const Value &v : attached) {
        if (v.kind != Value::Obj || !v.obj || v.obj == chassis_obj) {
            all_parts = false;
            continue;
        }
        if (!is_a(boot, v.obj->cls, "java.game.parts.Part")) all_parts = false;
        if (is_a(boot, v.obj->cls, "java.game.parts.enginepart.Battery")) battery = true;
        if (is_a(boot, v.obj->cls, "java.game.parts.enginepart.Block")) block = true;
        distinct.insert(v.obj);
    }
    c.check(all_parts && (int)distinct.size() == attached_count,
            "every attachedParts element is a distinct Part, not the chassis");
    const std::string drive_text =
        player_car ? is_driveable_text(boot, player_car) : "(no player car)";
    c.check(drive_text == "null",
            "player.car.isDriveable() is null (Vehicle.java:279, got " + drive_text + ")");
    c.check(battery && block,
            "attachedParts has a Battery and a Block (got battery " +
                std::string(battery ? "yes" : "no") + ", block " +
                (block ? "yes" : "no") + ")");

    // 0x4992e0 wheel loop + 0x5032d0 compound mass. Re-read the shipped cfg so
    // the mass comes from the file, not from the payload under test.
    CfgFile car_cfg;
    bool have_car_cfg = car_res && game::load_car_cfg(*boot.runtime(), car_res, &car_cfg);
    float cfg_body_mass = 0.0f, cfg_wheel_mass = 0.0f;
    int cfg_wheels = 0;
    for (const CfgBody &b : car_cfg.bodies)
        if (b.mass > 0.0f) cfg_body_mass += b.mass;
    for (const CfgLine &ln : car_cfg.lines) {
        if (ln.key != "type") continue;
        cfg_wheels++;
        cfg_wheel_mass += 0.01f;  // trigger-sensor capsule (0x4f3f10)
        if (!ln.values.empty()) {
            float m = (float)std::strtod(ln.values[0].c_str(), nullptr);
            if (m > 0.0f) cfg_wheel_mass += m;  // wheelbone child
        }
    }
    float part_mass = 0.0f, root_mass = 0.0f;
    if (car_payload) {
        if (car_payload->chassis_body && car_payload->chassis_body->def.inv_mass > 0.0f)
            part_mass = 1.0f / car_payload->chassis_body->def.inv_mass;
        if (car_payload->root_body && car_payload->root_body->def.inv_mass > 0.0f)
            root_mass = 1.0f / car_payload->root_body->def.inv_mass;
    }
    int wheel_count = car_payload ? (int)car_payload->wheels.size() : 0;
    float expected_root = cfg_body_mass + cfg_wheel_mass + installed_cfg_mass;
    if (wheel_count > 0) {
        const game::WheelData &w0 = car_payload->wheels[0];
        std::printf("  car mass (0x5032d0): part_comp %.3f kg, car_root %.3f kg; "
                    "expected %.3f kg (cfg bodies %.3f + wheels %.3f + parts "
                    "%.3f); wheel 0 k %.2f bump %.2f rebound %.2f\n",
                    (double)part_mass, (double)root_mass, (double)expected_root,
                    (double)cfg_body_mass, (double)cfg_wheel_mass,
                    (double)installed_cfg_mass, (double)w0.spring_k,
                    (double)w0.bump, (double)w0.rebound);
    }
    // Card 153 done-when d: the compound inertia before the drive checks.
    if (car_payload && car_payload->root_body) {
        Mat34 inv = car_payload->root_body->def.inv_inertia;
        float det = inv.m[0] * (inv.m[5] * inv.m[10] - inv.m[6] * inv.m[9]) -
                    inv.m[1] * (inv.m[4] * inv.m[10] - inv.m[6] * inv.m[8]) +
                    inv.m[2] * (inv.m[4] * inv.m[9] - inv.m[5] * inv.m[8]);
        if (std::isfinite(det) && std::fabs(det) > 1e-12f) {
            Mat34 inertia{};
            mat_inverse(&inertia, &inv);
            std::printf("  car_root inertia diag (Ixx Iyy Izz): %.2f %.2f %.2f "
                        "kg m^2\n",
                        (double)inertia.m[0], (double)inertia.m[5],
                        (double)inertia.m[10]);
        }
    }
    c.check(have_car_cfg && std::fabs(part_mass - cfg_body_mass) < 1e-3f,
            "part_comp mass is the cfg body-line mass sum (0x5032d0)");
    c.check(have_car_cfg && std::fabs(root_mass - expected_root) < 1e-2f,
            "car_root mass = cfg bodies + wheelbones/sensors + installed part "
            "cfg bodies (0x5032d0)");
    c.check(have_car_cfg && wheel_count == cfg_wheels && cfg_wheels >= 4,
            "car wheel count equals the cfg type-line count (0x4992e0)");
    bool springs_ok = wheel_count > 0;
    for (int i = 0; i < wheel_count; i++) {
        const game::WheelData &w = car_payload->wheels[(size_t)i];
        if (!(w.spring_k > 0.0f && w.bump > 0.0f && w.rebound > 0.0f))
            springs_ok = false;
    }
    c.check(springs_ok, "every wheel has spring_k/bump/rebound > 0 (0x4992e0)");
    int cars_with_bodies = game::car_body_instances();
    std::printf("  car bodies: %d car instance(s) with bodies, world body_count %d\n",
                cars_with_bodies, process_world().body_count);
    c.check(process_world().body_count >= 2 &&
                process_world().body_count ==
                    2 * cars_with_bodies + (int)process_world().statics.size(),
            "world body_count counts both bodies of every car and the grounds (0x4f7830)");
    // Track.java:291-299: the freeride race builds Track(game = the event's
    // careerGM, map = new GroundRef(td.map_id)).
    c.check(class_after == "java.game.Track",
            "testdrive -> race reaches java.game.Track (got " + class_after + ")");
    std::string game_after = "(none)", map_after = "(none)";
    Object *map_object = nullptr;
    if (have_state_after && state_after.kind == Value::Obj && state_after.obj) {
        Value gv, mv;
        std::string ferr;
        if (boot.vm()->get_instance_field(state_after.obj, "game", &gv, &ferr) &&
            gv.kind == Value::Obj && gv.obj)
            game_after = object_class(boot, gv.obj);
        ferr.clear();
        if (boot.vm()->get_instance_field(state_after.obj, "map", &mv, &ferr) &&
            mv.kind == Value::Obj && mv.obj) {
            map_after = object_class(boot, mv.obj);
            map_object = mv.obj;
        }
    }
    c.check(game_after == "java.game.Freeride",
            "Track.game is the freeride gamemode (got " + game_after + ")");
    c.check(map_after == "java.util.resource.GroundRef",
            "Track.map is a non-null GroundRef (got " + map_after + ")");

    // 0x519940: Track.map's record carries the embedded type-1 tree whose PHYS
    // chunks are the map collision (notes/19 §7).
    Runtime::Resource *map_res = nullptr;
    if (map_object) {
        Runtime::Handle *mh = boot.runtime()->handle(map_object);
        if (mh) map_res = mh->target;
    }
    MapTally mt;
    std::string map_err, map_name = "(none)";
    if (map_res && map_res->record && map_res->package) {
        map_name = map_res->record->name;
        walk_map_instance(map_res->package->rpk, *map_res->record, mt, &map_err);
    } else {
        map_err = "Track.map has no record-backed handle target";
    }
    std::printf("  map %s (record 0x%08x): %zu type-1 instance(s), %zu PHYS chunk(s), "
                "%zu face(s), %zu vert(s)\n",
                map_name.c_str(), map_res && map_res->record ? map_res->record->id : 0u,
                mt.instances, mt.phys_chunks, mt.faces, mt.verts);
    c.check(map_err.empty(), "Track.map type-1 blob tree parses" +
                                 (map_err.empty() ? std::string() : ": " + map_err));
    c.check(mt.faces > 0, "freeride map has ground triangles (" +
                              std::to_string(mt.faces) + ")");
    // Corpus rows from notes/19 §7, keyed by the package leaf name.
    struct MapRow {
        const char *name;
        size_t instances, phys_chunks, faces, verts;
    };
    static const MapRow rows[] = {
        {"city", 58210, 23976, 197286, 236064},
        {"city2", 56689, 19040, 251902, 276107},
        {"test_track", 12224, 6420, 66825, 77132},
        {"roc_track", 10894, 5147, 55040, 61777},
    };
    std::string leaf = map_res && map_res->package ? map_res->package->rpk_path : "";
    size_t sep = leaf.find_last_of("\\/");
    if (sep != std::string::npos) leaf = leaf.substr(sep + 1);
    leaf = lower_ascii(leaf);
    if (leaf.size() >= 4 && leaf.compare(leaf.size() - 4, 4, ".rpk") == 0)
        leaf.resize(leaf.size() - 4);
    const MapRow *row = nullptr;
    for (const MapRow &r : rows)
        if (leaf == r.name || lower_ascii(map_name) == r.name) row = &r;
    if (row) {
        c.check(mt.instances == row->instances && mt.phys_chunks == row->phys_chunks &&
                    mt.faces == row->faces && mt.verts == row->verts,
                std::string("freeride map ") + row->name + " totals match the corpus table");
    } else {
        // INVENTED: the shipped freeride event picks multibot/maps/A1Ring.rpk,
        // outside the card's four-map corpus; use the record layout instead:
        // Track.map must be the package's top type-1 record (id 1, notes/01).
        c.check(map_res && map_res->record && map_res->record->id == 1 &&
                    map_res->record->type == 1,
                "freeride map record is the package's type-1 master (id 1)");
        std::printf("  note: %s has no corpus table row (multibot map)\n", leaf.c_str());
    }

    // 0x4bade0: the map's type-1 record built the static kind-0 groundbody at
    // mount (2.3.1 builds it when the node becomes resident).
    RigidBody *ground = map_res ? map_res->ground_body.get() : nullptr;
    bool ground_static = false, ground_active = false;
    for (RigidBody *b : process_world().statics)
        if (b == ground) ground_static = true;
    for (RigidBody *b : process_world().bodies)
        if (b == ground) ground_active = true;
    c.check(ground != nullptr, "Track.map resource has a ground body (0x4bade0)");
    c.check(ground_static && !ground_active,
            "ground body is in world.statics, not the active root list (0x4f7830)");
    c.check(ground && (ground->def.flags & 4u) != 0 && ground->def.inv_mass == 0.0f &&
                ground->def.shape.type == SHAPE_MESH && ground->def.shape.mesh &&
                ground->def.shape.friction == 1.0f,
            "ground body is a static mesh collider with ground_friction 1.0");
    c.check(ground && ground->pos.x == 0.0f && ground->pos.y == 0.0f && ground->pos.z == 0.0f,
            "ground body sits at the record's params xyz");

    // INVENTED: Gamemode.wakeUpCars (Gamemode.java:799) is not called -- its
    // Track.java:1056 call is commented out; in the exe the freeride car wakes
    // from player input (Vehicle__updateControls 0x498300 at 0x498599). Queue
    // the same command Vehicle.wakeUp (Vehicle.java:187) sends.
    // 0x4f3d70 gate rejections, per wheel cumulative; diff across phases.
    auto gate_rejects = [](const Car *c) {
        unsigned n = 0;
        if (c)
            for (const Wheel &w : c->wheels) n += w.gate_rejects;
        return n;
    };
    if (car_res && car_payload && car_payload->root_body) {
        RigidBody *root = car_payload->root_body;
        // Floor = highest soup surface under the car's start (x,z): a downward
        // ray against the merged map collider (card 144).
        float floor_y = 0.0f;
        bool floor_found = false;
        if (ground && ground->def.shape.mesh) {
            const CollisionMesh &cm = *ground->def.shape.mesh;
            const float top = cm.bounds_max.y + 1.0f;
            MeshRayHit hit;
            floor_found = mesh_raycast(cm, ground->pose(),
                                       Vec3{root->pos.x, top, root->pos.z},
                                       Vec3{0.0f, -1.0f, 0.0f},
                                       top - cm.bounds_min.y + 1.0f, &hit);
            if (floor_found) floor_y = hit.point.y;
        }
        // INVENTED (fixture): Vehicle.create is called with the 0,0,0 of
        // Vehicle.java:111, which is 16.9 m above the A1Ring soup at the map
        // origin. A drop from there reaches the floor above the suspension
        // ray's speed and tunnels past it (no capsule sensor bodies yet, card
        // 146 "Not this card"), so start the car on the soup with the wheel 0
        // ray just touching: the short part springs (rest 0.266, max_len =
        // rest_len) cannot catch a 1 m drop.
        if (floor_found && car_payload->sim && !car_payload->sim->wheels.empty()) {
            const Wheel &w0 = car_payload->sim->wheels[0];
            root->pos.y = floor_y + (w0.rest_ray() - w0.mount.y);
        }
        const float y_before = root->pos.y;
        unsigned rejects_before = gate_rejects(car_payload->sim.get());
        boot.runtime()->queue_event(car_res, nullptr, 0x10,
                                    Value::make_str("wakeup"));
        // Card 158 checks over the 300 settle frames: the drive hook fires once
        // per executed step with the car awake (0x47be30), and it leaves the
        // body velocities untouched (forces only, 0x504e70/0x5051e0).
        g_drive_probe.root = root;
        const unsigned awake_calls0 = g_drive_probe.calls_awake;
        const unsigned bad_vel0 = g_drive_probe.bad_velocity;
        const unsigned bad_mom0 = g_drive_probe.bad_momentum;
        unsigned awake_steps = 0;
        for (int i = kFrames + 1; i <= kFrames + 300; i++) {
            // One 16 ms frame executes exactly one world step here; the car is
            // woken by the queued event at the end of the first frame.
            bool awake = !root->asleep;
            float t0 = process_world().time;
            loop.frame(i * 16);
            if (awake && process_world().time > t0) awake_steps++;
        }
        unsigned awake_calls = g_drive_probe.calls_awake - awake_calls0;
        unsigned bad_vel = g_drive_probe.bad_velocity - bad_vel0;
        unsigned bad_mom = g_drive_probe.bad_momentum - bad_mom0;
        std::printf("  drive hook (0x47be30): %u call(s) with the car awake, "
                    "%u world step(s) with the car awake\n",
                    awake_calls, awake_steps);
        c.check(awake_calls == awake_steps,
                "the drive hook runs once per executed 0x502060 step (" +
                    std::to_string(awake_calls) + " call(s), " +
                    std::to_string(awake_steps) + " step(s))");
        c.check(bad_vel == 0 && bad_mom == 0,
                "the drive hook only accumulates force/torque (vel/momentum "
                "bitwise equal, " + std::to_string(bad_vel) + " vel / " +
                    std::to_string(bad_mom) + " momentum change(s))");
        unsigned settle_rejects =
            gate_rejects(car_payload->sim.get()) - rejects_before;
        bool awake = false;
        for (RigidBody *b : process_world().bodies)
            if (b == root) awake = true;
        c.check(awake,
                "queued \"wakeup\" puts car_root back in the active body list (0x507060)");
        c.check(floor_found, "freeride map soup lies under the car start");

        // 0x47be30 drive callback: the woken car is carried by its suspension
        // springs (Vehicle__update 0x49c8e0) instead of the car_root box.
        Car *sim = car_payload->sim.get();
        std::printf("  car drive (0x47be30): floor %.3f, car_root pos.y %.3f -> %.3f "
                    "after 300 frames, vel.y %.3f, %zu contact(s)\n",
                    (double)floor_y, (double)y_before, (double)root->pos.y,
                    (double)root->vel.y, process_world().contacts.size());
        float mass = root->def.inv_mass > 0.0f ? 1.0f / root->def.inv_mass : 0.0f;
        float weight = mass * 9.81f;
        // Card 153 done-when b: static share from the compound COM and the hub
        // z; front each = W*(zr-c)/(zr-zf)/2, rear = W*(c-zf)/(zr-zf)/2.
        float zf = 0.0f, zr = 0.0f, com_z = car_payload->com[2];
        for (size_t i = 0; i < car_payload->wheels.size(); i++) {
            float z = car_payload->wheels[i].hub[2];
            if (i == 0 || z < zf) zf = z;
            if (i == 0 || z > zr) zr = z;
        }
        bool shares_ok = sim && weight > 0.0f && zr - zf > 1e-3f;
        float load_sum = 0.0f;
        bool wheels_ok = sim != nullptr && !sim->wheels.empty();
        if (sim)
            for (size_t i = 0; i < sim->wheels.size(); i++) {
                const Wheel &w = sim->wheels[i];
                float compression = w.rest_ray() - w.ray_len;
                bool front = i < car_payload->wheels.size() &&
                             car_payload->wheels[i].hub[2] < 0.5f * (zf + zr);
                float share = 0.0f;
                if (weight > 0.0f && zr - zf > 1e-3f)
                    share = front ? weight * (zr - com_z) / (zr - zf) / 2.0f
                                  : weight * (com_z - zf) / (zr - zf) / 2.0f;
                std::printf("    wheel %zu: ray_len %.3f load %.1f compression %.3f "
                            "contact %d, static share %.1f N (%s)\n",
                            i, (double)w.ray_len, (double)w.load,
                            (double)compression, w.contact ? 1 : 0,
                            (double)share, front ? "front" : "rear");
                load_sum += w.load;
                if (!w.contact || !(w.load > 0.0f) || !(compression > 0.0f))
                    wheels_ok = false;
                if (!(share > 0.0f) || std::fabs(w.load - share) / share >= 0.10f)
                    shares_ok = false;
            }
        c.check(wheels_ok, "every wheel rests on its spring (contact, load > 0, "
                           "compression > 0)");
        c.check(shares_ok, "every wheel load is within 10% of its static share "
                           "from the compound COM and hub z (0x5032d0)");
        char lw[128];
        snprintf(lw, sizeof lw, "spring loads %.1f N vs weight %.1f N",
                      (double)load_sum, (double)weight);
        c.check(weight > 0.0f &&
                    std::fabs(load_sum - weight) / weight < 0.02f,
                std::string("the car is carried by its springs (") + lw + ")");
        bool box_contact = false;
        for (const Contact &ct : process_world().contacts)
            if (ct.a == root || ct.b == root ||
                ct.a == car_payload->chassis_body ||
                ct.b == car_payload->chassis_body)
                box_contact = true;
        c.check(!box_contact,
                "no world contact has car_root or part_comp as a side");
        // Card 160 done-when 3: the springs carry the car at rest, so no
        // collision child hull touches the soup (0x4feb20 not triggered).
        int hull_contacts_rest = 0;
        for (const Contact &ct : process_world().contacts)
            if (ct.child_index >= 0) hull_contacts_rest++;
        c.check(root->collision_children.size() > 0,
                "car_root has collision child hulls from the cfg body lines (0x4feb20, " +
                    std::to_string(root->collision_children.size()) + " hull(s))");
        c.check(hull_contacts_rest == 0,
                "no hull contact at rest (" + std::to_string(hull_contacts_rest) +
                    " contact(s), the springs carry the car)");
        c.check(std::fabs(root->vel.y) < 0.1f, "car settles (|vel.y| < 0.1)");

        // Card 159: every SunStrip cfg body-line hull as a V-Clip Poly
        // (Poly__buildFromFaces 0x50e6d0) against the soup face under its
        // lowest vertex (two-face Poly, 0x50f000). The returned V-F distance
        // must equal the directly computed minimum vertex-to-face distance.
        {
            CfgFile car_cfg;
            bool car_cfg_ok = car_res && game::load_car_cfg(*rt, car_res, &car_cfg, nullptr);
            size_t hulls = 0, evaluated = 0, vf = 0;
            bool terminate_ok = true, distance_ok = true;
            if (car_cfg_ok && ground && ground->def.shape.mesh) {
                const CollisionMesh &gm = *ground->def.shape.mesh;
                Pose gpose = ground->pose();
                Mat34 gr = pose_rotation(gpose);
                Pose rpose = root->pose();
                Mat34 rr = root->rotation();
                for (const CfgBody &b : car_cfg.bodies) {
                    std::string rel = b.shape;
                    if (rel.size() < 4 ||
                        lower_ascii(rel.substr(rel.size() - 4)) != ".scx")
                        continue;
                    for (char &ch : rel)
                        if (ch == '\\')
                            ch = '/';
                    // selfcheck() chdirs to the game dir, so cfg paths are
                    // already relative to the current directory.
                    mesh::Mesh hm;
                    std::string herr;
                    if (!mesh::load(rel.c_str(), &hm, &herr)) {
                        std::printf("    hull %s: skipped (%s)\n", rel.c_str(),
                                    herr.c_str());
                        continue;
                    }
                    std::shared_ptr<const CollisionMesh> hcm = build_collision_mesh(hm);
                    if (!hcm)
                        continue;
                    hulls++;
                    Poly hpoly = poly_from_triangles(*hcm);
                    // Same pose as the mass compound child (0x5032d0): the
                    // 0x508d20 centroid moves the child, not the mesh, and
                    // car_root's local frame is the compound COM.
                    float ang[3] = {b.xform.rx, b.xform.ry, b.xform.rz};
                    Mat34 lr;
                    euler_to_mat(&lr, ang);
                    Vec3 child{b.xform.px, b.xform.py, b.xform.pz};
                    {
                        BodyDef def;
                        def.shape.type = SHAPE_MESH;
                        def.shape.mesh = hcm;
                        Vec3 center{0.0f, 0.0f, 0.0f};
                        compute_mass_props(&def, b.mass, &center);
                        child += mat_vec(lr, center);
                    }
                    Vec3 local = child - Vec3{car_payload->com[0], car_payload->com[1],
                                              car_payload->com[2]};
                    Mat34 wr;
                    mat_mul(&wr, &rr, &lr);
                    Pose hpose;
                    hpose.pos = rpose.pos + mat_vec(rr, local);
                    mat_to_quat(&hpose.rot, &wr);

                    float lowest = 1e30f;
                    Vec3 low{0.0f, 0.0f, 0.0f};
                    for (const Vec3 &v : hpoly.verts) {
                        Vec3 w = hpose.pos + mat_vec(wr, v);
                        if (w.y < lowest) {
                            lowest = w.y;
                            low = w;
                        }
                    }
                    float maxd = low.y - (gm.bounds_min.y - 1.0f);
                    MeshRayHit hit;
                    if (maxd <= 0.0f ||
                        !mesh_raycast(gm, gpose, low, Vec3{0.0f, -1.0f, 0.0f}, maxd,
                                      &hit))
                        continue;
                    const CollisionMesh::Triangle &tri = gm.triangles[hit.triangle];
                    Vec3 tv[3];
                    for (int i = 0; i < 3; i++)
                        tv[i] = gpose.pos + mat_vec(gr, gm.vertices[tri.v[i]]);
                    Poly spoly = poly_triangle(tv[0], tv[1], tv[2], hit.normal);

                    int fa = 0, fb = 0;
                    Vec3 pa{0.0f, 0.0f, 0.0f}, pb{0.0f, 0.0f, 0.0f};
                    Vec3 nrm{0.0f, 1.0f, 0.0f};
                    Pose ipose;
                    float d = vclip_closest(hpose, hpoly, ipose, spoly, &fa, &fb, &pa, &pb,
                                            &nrm);
                    if (d >= 1e6f) {
                        terminate_ok = false;
                        std::printf("    hull %s: vclip_closest returned 1e6\n",
                                    rel.c_str());
                        continue;
                    }
                    evaluated++;
                    int ka = hpoly.features[fa].kind, kb = spoly.features[fb].kind;
                    float expected = 1e30f;
                    for (const Vec3 &v : hpoly.verts) {
                        Vec3 w = hpose.pos + mat_vec(wr, v);
                        float sd = vec_dot(hit.normal, w - tv[0]);
                        if (sd < expected)
                            expected = sd;
                    }
                    std::printf("    hull %-42s V-Clip d %+.6f, vertex-face min %+.6f, "
                                "kinds %d-%d\n",
                                rel.c_str(), (double)d, (double)expected, ka, kb);
                    if (ka == POLY_V && kb == POLY_F) {
                        vf++;
                        if (std::fabs(d - expected) > 1e-4f)
                            distance_ok = false;
                    }
                }
            }
            c.check(car_cfg_ok, "SunStrip car cfg loads for the hull V-Clip check");
            std::printf("  hull V-Clip (0x50d230): %zu hull(s), %zu evaluated, %zu V-F\n",
                        hulls, evaluated, vf);
            c.check(terminate_ok, "vclip_closest terminates on every cfg body-line hull");
            c.check(vf > 0 && distance_ok,
                    "V-Clip V-F distance equals the minimum vertex-face distance of the "
                    "soup face under the hull");
        }

        // Card 156: the wheel "ray" is the sensor segment (0x4f3f10) tested
        // one-sided against every PHYS face by 0x4fd110, using the stored
        // face normal. Re-read the map records here and compare.
        std::vector<Vec3> gverts;
        std::vector<CollisionMesh::Triangle> gtris;
        std::vector<Vec3> gnormals;
        if (map_res && map_res->package && map_res->record)
            gather_phys_faces(map_res->package->rpk, *map_res->record, &gverts, &gtris,
                              &gnormals);
        auto expected = build_collision_mesh(gverts, gtris, gnormals);
        bool normals_ok = ground && ground->def.shape.mesh && expected && sim;
        bool sensor_ok = normals_ok;
        bool wheel_normals_ok = normals_ok;
        if (sim && ground && ground->def.shape.mesh && expected) {
            const CollisionMesh &cm = *ground->def.shape.mesh;
            Mat34 rr = root->rotation();
            Vec3 up = mat_vec(rr, Vec3{0.0f, 1.0f, 0.0f});
            Pose gpose = ground->pose();
            for (size_t i = 0; i < sim->wheels.size(); i++) {
                const Wheel &w = sim->wheels[i];
                Vec3 top = root->pos + mat_vec(rr, w.mount);
                std::vector<SegmentCross> hits;
                int n = mesh_segment_cross(cm, gpose, top - up * w.max_ray(), top, &hits);
                float nearest = 1e30f;
                Vec3 nearest_normal{0.0f, 0.0f, 0.0f};
                uint32_t nearest_tri = 0;
                for (const SegmentCross &h : hits) {
                    float d = vec_length(top - h.point);
                    if (d < nearest) {
                        nearest = d;
                        nearest_normal = h.normal;
                        nearest_tri = h.triangle;
                    }
                    if (h.triangle >= expected->face_normals.size()) {
                        normals_ok = false;
                        continue;
                    }
                    Vec3 en = expected->face_normals[h.triangle];
                    if (float_bits(h.normal.x) != float_bits(en.x) ||
                        float_bits(h.normal.y) != float_bits(en.y) ||
                        float_bits(h.normal.z) != float_bits(en.z))
                        normals_ok = false;
                }
                std::printf("    wheel %zu sensor (0x4fd110): %d hit(s), nearest |top-X| "
                            "%.4f (ray_len %.4f), sensor_len %.4f max_ray %.4f, "
                            "normal (%.4f %.4f %.4f) tri %u\n",
                            i, n, (double)nearest, (double)w.ray_len,
                            (double)w.sensor_len, (double)w.max_ray(),
                            (double)nearest_normal.x, (double)nearest_normal.y,
                            (double)nearest_normal.z, nearest_tri);
                if (n < 1 || std::fabs(nearest - w.ray_len) > 1e-3f)
                    sensor_ok = false;
                // 0x4f3d70 S+0xc4: the sim wheel stores the PHYS face normal.
                if (n < 1 ||
                    float_bits(w.normal_world.x) != float_bits(nearest_normal.x) ||
                    float_bits(w.normal_world.y) != float_bits(nearest_normal.y) ||
                    float_bits(w.normal_world.z) != float_bits(nearest_normal.z))
                    wheel_normals_ok = false;
            }
        }
        c.check(normals_ok,
                "every sensor crossing uses the PHYS stored face normal (0x4fd110)");
        c.check(sensor_ok,
                "every wheel sensor segment crosses the map soup at its ray_len");
        c.check(wheel_normals_ok,
                "every sim wheel's normal_world is the PHYS face normal under it "
                "(0x4f3d70 S+0xc4)");
        std::printf("  sensor gate (0x4f3d70): %u crossing(s) rejected (prev - d > r) "
                    "over the 300 settle frames\n", settle_rejects);

        // Card 154 done-when a: the part on chassis slot 311+i is a Spring and
        // its updatevariables values reach the simulation wheel (0x4c8b10).
        bool wheel_parts_ok = sim != nullptr && chassis_parts != nullptr;
        for (int i = 0; chassis_parts && sim && i < (int)car_payload->wheels.size(); i++) {
            game::Slot *s = nullptr;
            for (game::Slot &cs : chassis_parts->slots)
                if (cs.id == 311 + i) s = &cs;
            Object *o = s ? s->attached : nullptr;
            Value fv, rv, mv;
            std::string ferr;
            bool have = o && boot.vm()->get_instance_field(o, "force", &fv, &ferr) &&
                        boot.vm()->get_instance_field(o, "restlength", &rv, &ferr) &&
                        boot.vm()->get_instance_field(o, "minlength", &mv, &ferr);
            bool spring = o && is_a(boot, o->cls, "java.game.parts.rgearpart.Spring");
            const Wheel &sw = sim->wheels[(size_t)i];
            std::printf("    wheel %d: slot %d part %s, force %.3f rest %.3f min %.3f; "
                        "sim spring %.3f rest %.3f min %.3f\n",
                        i, 311 + i, o ? object_class(boot, o).c_str() : "(none)",
                        have ? (double)fv.f : 0.0, have ? (double)rv.f : 0.0,
                        have ? (double)mv.f : 0.0, (double)sw.spring,
                        (double)sw.rest_len, (double)sw.min_len);
            if (!spring || !have || fv.kind != Value::Float || rv.kind != Value::Float ||
                mv.kind != Value::Float || std::fabs(sw.spring - fv.f) > 1e-4f ||
                std::fabs(sw.rest_len - rv.f) > 1e-4f ||
                std::fabs(sw.min_len - mv.f) > 1e-4f)
                wheel_parts_ok = false;
        }
        c.check(wheel_parts_ok, "the Spring on chassis slot 311+i drives sim wheel i "
                                "(force/restlength/minlength, 0x4c8b10)");

        // Card 154 done-when b: Part.getWheelID through the VM on every wheel
        // family slot returns the wheel index ((v-101)%10, 0x4c8b10).
        bool wheel_ids_ok = chassis_parts != nullptr;
        if (chassis_parts) {
            static const int families[5] = {101, 111, 121, 301, 311};
            for (int f = 0; f < 5; f++)
                for (int i = 0; i < 4; i++) {
                    game::Slot *s = nullptr;
                    for (game::Slot &cs : chassis_parts->slots)
                        if (cs.id == families[f] + i) s = &cs;
                    Object *o = s ? s->attached : nullptr;
                    Value id;
                    std::string ierr;
                    bool ok = o && rt->call_method(o, "getWheelID", {}, &id, &ierr) &&
                              id.kind == Value::Int && id.i == i;
                    std::printf("    slot %d: %s getWheelID -> %d\n", families[f] + i,
                                o ? object_class(boot, o).c_str() : "(none)",
                                id.kind == Value::Int ? id.i : -999);
                    if (!ok) wheel_ids_ok = false;
                }
        }
        c.check(wheel_ids_ok, "Part.getWheelID via the VM returns i for the parts on "
                              "chassis slots 101/111/121/301/311+i (0x4c8b10)");
    }

    // Card 147: Track.enter's "controllable" / "transmission" commands bind the
    // player's Controller to the car (notes/09 "Control path").
    Object *ctrl_obj = nullptr;
    {
        Value pv2;
        std::string perr2;
        if (boot.vm()->get_static_field("java.game.GameLogic", "player", &pv2, &perr2) &&
            pv2.kind == Value::Obj && pv2.obj) {
            Value cv;
            perr2.clear();
            if (boot.vm()->get_instance_field(pv2.obj, "controller", &cv, &perr2) &&
                cv.kind == Value::Obj)
                ctrl_obj = cv.obj;
        }
    }
    // 0x48dee0: Axis _main_*.cfg has one `controller` line -> 1 slot, driver 0.
    c.check(car_payload && car_payload->control_slots == 1 &&
                car_payload->driver_slot == 0,
            "Axis car cfg gives 1 control slot, driver slot 0 (0x48dee0)");
    // 0x4d2ab0 -> 0x4a3360: player "controllable <car id>" binds the controller.
    c.check(car_payload && ctrl_obj && car_payload->slot_player[0] &&
                car_payload->slot_player[0]->script == ctrl_obj,
            "controllable binds GameLogic.player.controller to slot 0 (0x4a3360)");
    // 0x49a4ef: engine on at create when M+0x17f0 >= 0 and Steam not neutralized.
    c.check(car_payload && (car_payload->flags & 0x4000u) != 0,
            "createCar sets the engine-on flag for a driven car (0x49a4ef)");
    // 0x4a37f0: 0x40 = auto clutch, 0x10 = auto gearbox, 0x20 = semi-auto.
    Value trv;
    std::string trerr;
    bool have_tr = boot.vm()->get_static_field("java.util.Config",
                                              "player_transmission", &trv, &trerr);
    int tr = have_tr && trv.kind == Value::Int ? trv.i : 0;
    uint32_t tr_bits =
        (tr & 1) ? (((tr & 2) ? 0u : 0x40u) | ((tr & 4) ? 0x20u : 0x10u)) : 0u;
    c.check(have_tr && car_payload && (car_payload->flags & 0x70u) == tr_bits,
            "transmission command sets the 0x4a37f0 flags (mode " +
                std::to_string(tr) + ")");
    // Card 154: the strut parts' updatevariables now reach WheelRef
    // setInstantCenter (the Chassis.java:346 value was 10000 x6 before).
    bool ic_ok = car_payload && !car_payload->wheels.empty();
    if (car_payload)
        for (size_t i = 0; i < car_payload->wheels.size(); i++) {
            const game::WheelData &w = car_payload->wheels[i];
            bool h = w.ic_h[0] != 0.0f || w.ic_h[1] != 0.0f || w.ic_h[2] != 0.0f;
            bool l = w.ic_l[0] != 0.0f || w.ic_l[1] != 0.0f || w.ic_l[2] != 0.0f;
            std::printf("    wheel %zu instant center: h %.3f %.3f %.3f, l %.3f %.3f "
                        "%.3f\n",
                        i, (double)w.ic_h[0], (double)w.ic_h[1], (double)w.ic_h[2],
                        (double)w.ic_l[0], (double)w.ic_l[1], (double)w.ic_l[2]);
            if (!h || !l) ic_ok = false;
        }
    c.check(ic_ok, "every wheel's instant center was written by its strut parts "
                   "(WheelRef.setInstantCenter, 0x4c8b10)");
    if (car_payload) {
        std::printf("  player bind: slots %d driver %d, controller %p slot0 script %p, "
                    "flags 0x%x, transmission %d -> 0x%x\n",
                    car_payload->control_slots, car_payload->driver_slot,
                    (void *)ctrl_obj,
                    (void *)(car_payload->slot_player[0]
                                 ? car_payload->slot_player[0]->script
                                 : nullptr),
                    car_payload->flags, tr, car_payload->flags & 0x70u);
    }

    // Card 149: drive_cars copies CarData's control inputs and engine block into
    // vehicle::Car before the sub-step (INVENTED glue, notes/09 "Control path").
    Car *sim = car_payload ? car_payload->sim.get() : nullptr;
    c.check(sim && sim->engine.running == ((car_payload->flags & 0x4000u) != 0),
            "drive bridge sets engine.running from the createCar flag");
    float curve_max = 0.0f;
    bool curve_ok = sim && sim->engine.torque_curve.size() == car_payload->torque_nm.size();
    if (sim && curve_ok)
        for (size_t i = 0; i < sim->engine.torque_curve.size(); i++) {
            const TorquePoint &p = sim->engine.torque_curve[i];
            if (p.value > curve_max) curve_max = p.value;
            if (p.value != car_payload->torque_nm[i] ||
                (i < car_payload->torque_rpm.size() &&
                 p.rpm != car_payload->torque_rpm[i]))
                curve_ok = false;
        }
    // Card 150: with the engine installed, Chassis.updatevariables reaches the
    // block's DynoData (Duhen_D18V_block, 0x4aebf0) and the curve is real.
    c.check(curve_ok && car_payload && car_payload->torque_nm.size() > 1 &&
                curve_max > 0.0f,
            "drive bridge copies the installed engine's torque curve (" +
                std::to_string(sim ? sim->engine.torque_curve.size() : 0) +
                " points, max " + std::to_string((double)curve_max) + " Nm)");
    // Card 151: Chassis.updatevariables copies dyno.RPM_limit and rounds
    // 1.25*RPM_limit down to 250 rpm steps (Chassis.java:688-692). Read the
    // installed block's script field instead of a hand-written number.
    float block_rpm_limit = 0.0f;
    {
        Value dv, lv;
        std::string derr;
        if (engine_obj &&
            boot.vm()->get_instance_field(engine_obj, "dynodata", &dv, &derr) &&
            dv.kind == Value::Obj && dv.obj &&
            boot.vm()->get_instance_field(dv.obj, "RPM_limit", &lv, &derr))
            block_rpm_limit = lv.kind == Value::Float ? lv.f
                              : lv.kind == Value::Int ? (float)lv.i : 0.0f;
    }
    c.check(car_payload && block_rpm_limit > 0.0f &&
                car_payload->rpm_limit == block_rpm_limit,
            "RPM_limit comes from the installed block's dynodata (got " +
                std::to_string(car_payload ? (double)car_payload->rpm_limit : 0.0) +
                ", block " + std::to_string((double)block_rpm_limit) + ")");
    c.check(car_payload && car_payload->max_rpm > car_payload->rpm_limit,
            "maxRPM is above RPM_limit (got " +
                std::to_string(car_payload ? (double)car_payload->max_rpm : 0.0) + " / " +
                std::to_string(car_payload ? (double)car_payload->rpm_limit : 0.0) + ")");
    c.check(car_payload && car_payload->rpm_max_torque > 0.0f &&
                car_payload->rpm_max_torque < car_payload->max_rpm,
            "rpm_max_torque lies inside the curve (got " +
                std::to_string(car_payload ? (double)car_payload->rpm_max_torque : 0.0) +
                ")");
    std::printf("  engine limits: rpm_max_torque %.0f, rpm_limit %.0f, max_rpm %.0f\n",
                car_payload ? (double)car_payload->rpm_max_torque : 0.0,
                car_payload ? (double)car_payload->rpm_limit : 0.0,
                car_payload ? (double)car_payload->max_rpm : 0.0);
    bool ratios_ok = sim && car_payload->ratio[7] < 0.0f;
    for (int i = 0; ratios_ok && i < kMaxGears; i++)
        if (sim->ratios[i] != car_payload->ratio[i]) ratios_ok = false;
    c.check(ratios_ok, "drive bridge copies ratios[] and reverse is negative");
    if (sim) {
        std::printf("  drive bridge: engine running %d, curve %zu points max %.1f Nm,"
                    " ratios",
                    (int)sim->engine.running, sim->engine.torque_curve.size(),
                    (double)curve_max);
        for (int i = 0; i < kMaxGears; i++)
            std::printf(" %.3f", (double)sim->ratios[i]);
        std::printf("\n");
    }

    // Card 148: the mode-8 control list (0x47bc20 -> Vehicle__updateControls
    // 0x498300) reads the driver Controller's axes. Shipped
    // save/controls/active_control_set group 1: A (DIK 0x1e) -> axis 50 gear up,
    // Up (0xc8) -> axis 28 throttle, Left/Right (0xcb/0xcd) -> axis 1 steer.
    if (car_payload && car_payload->root_body) {
        RigidBody *root = car_payload->root_body;
        // INVENTED harness: spawn pose, restored before the independent
        // semi-auto run (the earlier launches reach the map edge).
        const Vec3 semi_spawn = root->pos;
        const Quat semi_rot = root->rot;
        int fr = kFrames + 300;
        auto key = [](int dik, bool down) {
            g_input->devices[0].raw.keys[dik] = down ? 0x80 : 0;
        };
        auto frames = [&fr, &loop](int n) {
            for (int i = 0; i < n; i++) loop.frame(++fr * 16);
        };
        auto report = [car_payload, root](const char *phase) {
            // car+0x1e7c is rad/s; getRPM reports it * 9.549296.
            std::printf("  control %s: gear %d, throttle %.3f, steer %.3f, "
                        "speed %.2f m/s, rpm %.0f\n",
                        phase, car_payload->gear, (double)car_payload->throttle,
                        (double)car_payload->steer_input,
                        (double)vec_length(root->vel),
                        (double)(car_payload->rpm_raw * 9.549296f));
        };
        key(0x1e, true);
        frames(10);
        report("A held");
        // 0x498300's shift block shifts at once, but the 0x497930 auto box
        // returns while shift_end is live, so read the manual shift inside the
        // 0.3 s window (after it, neutral is allowed again).
        int shift_gear = car_payload->gear;
        float shift_ratio = car_payload->gear_ratio;
        key(0x1e, false);
        frames(10);
        report("A released");
        // 0x498300's shift block is guarded by `if (gears)`; gears come from the
        // installed transmission's cfg/script (0x4aebf0), so neutral stays when
        // the car has none.
        int want_gear = car_payload->gears > 0 ? 1 : 0;
        c.check(shift_gear == want_gear,
                "A (axis 50) shifts neutral -> 1st when the car has gears (got gear " +
                    std::to_string(shift_gear) + ", gears " +
                    std::to_string(car_payload->gears) + ")");
        c.check(std::fabs(shift_ratio - car_payload->ratio[shift_gear]) < 1e-6f,
                "gear_ratio follows ratio[gear] after the shift (got " +
                    std::to_string((double)shift_ratio) + ")");
        // INVENTED (fixture): a resting car sleeps (0x506d90); sleeping it here
        // exercises the input wake path 0x498599.
        process_world().sleep(root);
        key(200, true);
        frames(60);
        bool awake = false;
        for (RigidBody *b : process_world().bodies)
            if (b == root) awake = true;
        report("Up held");
        c.check(car_payload->throttle > 0.9f,
                "Up (axis 28) gives throttle > 0.9 (got " +
                    std::to_string((double)car_payload->throttle) + ")");
        c.check(awake, "throttle input wakes the sleeping car_root (0x498599)");
        key(203, true);
        frames(60);
        report("Up + Left");
        c.check(car_payload->steer_input > 0.9f,
                "Left (axis 1) gives steer > 0.9 (got " +
                    std::to_string((double)car_payload->steer_input) + ")");
        key(203, false);
        key(205, true);
        frames(60);
        report("Up + Right");
        c.check(car_payload->steer_input < -0.9f,
                "Right (axis 1) gives steer < -0.9 (got " +
                    std::to_string((double)car_payload->steer_input) + ")");
        key(205, false);
        key(200, false);
        frames(1);

        // Card 152: auto gearbox + auto clutch from standstill (0x497930 /
        // 0x4977a0). INVENTED fixture: 0x4a4067 "reset" clears the gear and
        // the shaft speed, like the command Track.enter sends.
        game::car_command(*rt, car_res, 0x10, Value::make_str("reset"));
        int settle = 0;
        while (std::fabs(car_payload->shaft_speed) > 1.0f && settle < 300) {
            frames(1);
            settle++;
        }
        c.check(std::fabs(car_payload->shaft_speed) <= 1.0f,
                "reset reaches standstill (|shaft speed| <= 1 after " +
                    std::to_string(settle) + " frame(s), got " +
                    std::to_string((double)car_payload->shaft_speed) + ")");
        struct DriveSample {
            int gear;
            float rpm, clutch, shaft, speed, ratio, thr, kappa;
        };
        std::vector<DriveSample> samples;
        float rpm_before = car_payload->rpm_smoothed;
        unsigned launch_rejects0 = gate_rejects(sim);
        // Card 160 done-when 1: a downward ray from 3 m over car_root must
        // never find the soup above the root.
        const CollisionMesh *soup = ground && ground->def.shape.mesh
                                        ? ground->def.shape.mesh.get()
                                        : nullptr;
        const Pose soup_pose = ground ? ground->pose() : Pose{};
        int soup_passed = 0;
        float min_margin = 1e30f;
        // Card 161 check 1: pacejka slot 13 is gm+0x54 + W+0x74 (0x49ef5a).
        float rr_ground = ground ? ground->def.shape.rolling_resistance : 0.0f;
        unsigned rr_bad = 0, rr_checked = 0;
        bool rr_printed = false;
        // Card 161 check 2: a zero-C_drag car leaves car+0x2248 at 0.
        int drag_nonzero = 0;
        key(200, true);
        for (int i = 0; i < 900; i++) {
            frames(1);
            if (sim)
                for (const Wheel &w : sim->wheels) {
                    if (!(w.contact && w.load > 0.0f)) continue;
                    float want = rr_ground + w.rolling_resistance;
                    if (w.pacejka.p[13] != want) rr_bad++;
                    rr_checked++;
                    if (!rr_printed) {
                        std::printf("  rolling resistance (0x49ef5a): gm+0x54 %.4f + "
                                    "W+0x74 %.4f = p13 %.4f\n",
                                    (double)rr_ground, (double)w.rolling_resistance,
                                    (double)w.pacejka.p[13]);
                        rr_printed = true;
                    }
                }
            if (sim && sim->drag_magnitude != 0.0f) drag_nonzero++;
            if (soup) {
                MeshRayHit rh;
                Vec3 origin = root->pos + Vec3{0.0f, 3.0f, 0.0f};
                if (mesh_raycast(*soup, soup_pose, origin, Vec3{0.0f, -1.0f, 0.0f},
                                 10.0f, &rh)) {
                    float margin = root->pos.y - rh.point.y;
                    if (margin < min_margin) min_margin = margin;
                    if (!(rh.point.y < root->pos.y)) soup_passed++;
                }
            }
            // 0x4b19a0 mean driven-wheel slip, one value per frame.
            float ksum = 0.0f;
            int kn = 0;
            if (car_payload->sim)
                for (const Wheel &w : car_payload->sim->wheels) {
                    if (w.drive == 0.0f || !(w.load > 0.0f)) continue;
                    ksum += w.kappa_prev;
                    kn++;
                }
            samples.push_back(DriveSample{car_payload->gear,
                                          car_payload->rpm_smoothed,
                                          car_payload->clutch_input,
                                          car_payload->shaft_speed,
                                          vec_length(root->vel),
                                          car_payload->gear_ratio,
                                          car_payload->throttle,
                                          kn ? ksum / (float)kn : 0.0f});
        }
        key(200, false);
        std::printf("  sensor gate (0x4f3d70): %u crossing(s) rejected (prev - d > r) "
                    "over the 900 launch frames\n",
                    gate_rejects(sim) - launch_rejects0);
        if (min_margin < 1e29f)
            std::printf("  hull vs soup (0x4feb20/0x4fc7e0): %d frame(s) with the soup "
                        "above car_root, min soup margin %.3f m\n",
                        soup_passed, (double)min_margin);
        else
            std::printf("  hull vs soup (0x4feb20/0x4fc7e0): no soup hit under car_root "
                        "over the 900 frames\n");
        c.check(soup && soup_passed == 0,
                "the car never crosses the soup (" + std::to_string(soup_passed) +
                    " frame(s) with the soup above car_root over 900 auto-drive frames)");
        // Card 161 check 1.
        c.check(rr_checked > 0 && rr_bad == 0,
                "pacejka p13 = ground rolling resistance + wheel rollres on every loaded "
                "wheel over 900 frames (" + std::to_string(rr_bad) + " bad of " +
                    std::to_string(rr_checked) + " checks)");
        // Card 161 check 2 (the SunStrip's parts reduce C_drag to 0).
        std::printf("  C_drag (e+0x91c): %.4f\n", (double)car_payload->c_drag);
        if (car_payload->c_drag == 0.0f)
            c.check(drag_nonzero == 0,
                    "zero C_drag leaves car+0x2248 = 0 on every auto-drive frame (" +
                        std::to_string(drag_nonzero) + " frame(s) nonzero)");
        // Card 161 check 3: the shipped C_drag corpus comes from
        // Chassis.updatevariables over the real installed parts; every CarData is
        // a Vehicle's Chassis (Vehicle.java always calls forceUpdate).
        game::CarData *x_car = nullptr;
        std::vector<game::CarData *> payloads = game::car_payloads();
        std::printf("  shipped C_drag:");
        for (game::CarData *p : payloads) {
            std::printf(" %.4f", (double)p->c_drag);
            if (!x_car || p->c_drag > x_car->c_drag) x_car = p;
        }
        std::printf(" (%zu car payload(s))\n", payloads.size());
        c.check(x_car && x_car->c_drag > 0.0f,
                "at least one shipped car has C_drag > 0 (got " +
                    std::to_string(x_car ? (double)x_car->c_drag : 0.0) + ")");
        int engage_i = -1;
        for (size_t i = 0; i < samples.size(); i++)
            if (samples[i].gear == 1) {
                engage_i = (int)i;
                break;
            }
        float before = engage_i > 0 ? samples[(size_t)engage_i - 1].rpm : rpm_before;
        c.check(engage_i >= 0 &&
                    (engage_i == 0 || samples[(size_t)engage_i - 1].gear == 0) &&
                    before > 3.0f * car_payload->idle_omega,
                "auto gearbox engages 1st from standstill (0x497930): rpm before " +
                    std::to_string((double)before) + " rad/s > 3*idle " +
                    std::to_string((double)(3.0f * car_payload->idle_omega)));
        float max_clutch = 0.0f;
        int n30 = 0;
        if (engage_i >= 0)
            for (int i = engage_i; i < engage_i + 30 && i < (int)samples.size(); i++) {
                max_clutch = std::max(max_clutch, samples[(size_t)i].clutch);
                n30++;
            }
        float launch = 0.0f;
        if (engage_i >= 0 && car_payload->ratio[1] != 0.0f)
            launch =
                (std::fabs(samples[(size_t)engage_i].ratio / car_payload->ratio[1]) *
                         samples[(size_t)engage_i].thr * 0.56f +
                     0.24000001f) *
                    (car_payload->rpm_max_torque * 0.10471976f -
                     car_payload->idle_omega) +
                car_payload->idle_omega;
        std::printf("  auto clutch: max %.3f over %d frame(s) after engage, "
                    "launch %.1f rad/s\n",
                    (double)max_clutch, n30, (double)launch);
        c.check(engage_i >= 0 && max_clutch > 0.0f,
                "auto clutch slips at launch (0x4977a0): max clutch_input " +
                    std::to_string((double)max_clutch) + " (key C never pressed)");
        int max_gear = 0;
        for (const DriveSample &s : samples)
            max_gear = std::max(max_gear, s.gear);
        // 0x4b19a0 mean driven-wheel kappa over the last 60 frames.
        float kappa_sum = 0.0f;
        int kappa_n = 0;
        for (size_t i = samples.size() > 60 ? samples.size() - 60 : 0;
             i < samples.size(); i++) {
            kappa_sum += samples[i].kappa;
            kappa_n++;
        }
        float final_speed = samples.empty() ? 0.0f : samples.back().speed;
        // 1st-gear top speed at the 0x4b06b0 upshift threshold: w * R.
        float v1 = 0.0f;
        if (!car_payload->wheels.empty() && car_payload->ratio[1] != 0.0f &&
            car_payload->rearend_ratio != 0.0f)
            v1 = 0.95f * std::min(car_payload->max_rpm, car_payload->rpm_limit) *
                 0.10471976f / (car_payload->ratio[1] * car_payload->rearend_ratio) *
                 car_payload->wheels[0].radius;
        std::printf("  auto drive: max gear %d, final speed %.2f m/s (v1 %.2f), mean "
                    "driven kappa %.3f over the last %d frames (0x4b19a0)\n",
                    max_gear, (double)final_speed, (double)v1,
                    (double)(kappa_n ? kappa_sum / (float)kappa_n : 0.0f), kappa_n);
        c.check(max_gear >= 2, "auto gearbox reaches 2nd gear without a gear key "
                               "(0x497930, got " + std::to_string(max_gear) + ")");
        c.check(v1 > 0.0f && final_speed > v1,
                "final speed beats the 1st-gear top speed at the 0x4b06b0 threshold "
                "(got " + std::to_string((double)final_speed) + " m/s, v1 " +
                    std::to_string((double)v1) + ")");
        // Card 160 done-when 2: the rev limiter (torque 0 above RPM_limit,
        // 0x49c8e0) and the top gear bound the car's speed.
        float v_top = 0.0f;
        if (!car_payload->wheels.empty() && car_payload->gears > 0 &&
            car_payload->gears < 8 && car_payload->ratio[car_payload->gears] > 0.0f &&
            car_payload->rearend_ratio > 0.0f)
            v_top = car_payload->rpm_limit * 0.10471976f /
                    (car_payload->ratio[car_payload->gears] *
                     car_payload->rearend_ratio) *
                    car_payload->wheels[0].radius;
        float h_speed = sqrtf(root->vel.x * root->vel.x + root->vel.z * root->vel.z);
        std::printf("  top-speed bound: final |vel| %.2f m/s, horizontal %.2f m/s, "
                    "v_top %.2f m/s (RPM_limit %.0f, gear %d)\n",
                    (double)final_speed, (double)h_speed, (double)v_top,
                    (double)car_payload->rpm_limit, car_payload->gears);
        c.check(v_top > 0.0f && final_speed <= v_top,
                "final speed is within the rev-limiter/top-gear bound (|vel| " +
                    std::to_string((double)final_speed) + " m/s <= v_top " +
                    std::to_string((double)v_top) + " m/s)");
        // Card 161 check 4. INVENTED harness: force the largest shipped C_drag /
        // drag_center onto the player car (the exe reaches it by removing the
        // parts that reduce C_drag) and keep driving 300 frames.
        if (x_car && x_car->c_drag > 0.0f && sim) {
            car_payload->c_drag = x_car->c_drag;
            for (int k = 0; k < 3; k++) car_payload->drag_center[k] = x_car->drag_center[k];
            int moving = 0, clamps = 0, bad = 0;
            Vec3 last_v{0.0f, 0.0f, 0.0f}, last_f{0.0f, 0.0f, 0.0f};
            key(200, true);
            for (int i = 0; i < 300; i++) {
                frames(1);
                const Vec3 v = sim->drag_velocity;
                const Vec3 f = sim->drag_force;
                float v2 = vec_dot(v, v);
                if (!(v2 > 1.0f)) continue;
                moving++;
                last_v = v;
                last_f = f;
                float fl = vec_length(f);
                if (!(sim->drag_magnitude > 0.0f)) bad++;
                if (sim->drag_magnitude != fl) bad++;
                if (!(vec_dot(f, v) < 0.0f)) bad++;
                if (sim->drag_clamped) {
                    clamps++;
                    Vec3 h = v * (sim->drag_m_eff * 0.5f);
                    Vec3 q = f * sim->drag_dt + h;
                    float hl = vec_length(h);
                    if (std::fabs(vec_length(q) - hl) > 1e-4f * hl) bad++;
                } else {
                    float s = sqrtf(v2);
                    Vec3 want = v * (1.0f / s) * -(v2 * sim->c_drag);
                    if (vec_length(f - want) > 1e-4f * fl) bad++;
                }
            }
            key(200, false);
            float final_v = vec_length(root->vel);
            std::printf("  drag run (0x49fb34): C_drag %.4f, last moving frame |v| %.2f m/s,"
                        " |F| %.2f N, %d clamp(s), %d moving frame(s), final |vel| %.2f"
                        " m/s\n",
                        (double)sim->c_drag, (double)vec_length(last_v),
                        (double)vec_length(last_f), clamps, moving, (double)final_v);
            c.check(moving >= 250,
                    "the forced-drag run keeps the car moving for >= 250 of 300 frames (" +
                        std::to_string(moving) + ")");
            c.check(bad == 0,
                    "drag force follows 0x49fb34 on every moving frame (" +
                        std::to_string(bad) + " mismatch(es))");
            c.check(final_v <= v_top,
                    "forced-drag final |vel| stays within v_top (" +
                        std::to_string((double)final_v) + " <= " +
                        std::to_string((double)v_top) + " m/s)");
        }
        // Card 162: semi-auto gearbox 0x4ad480 (transmission flag 0x20).
        // INVENTED harness: Track.enter sends "transmission " +
        // Config.player_transmission (default 1); 5 is the shipped
        // Vehicle.TRANSMISSION_SEMIAUTO.
        // INVENTED harness: the 1200 frames above have carried the car to the
        // edge of the test-track ground, so put it back at the spawn pose.
        root->pos = semi_spawn;
        root->rot = semi_rot;
        root->vel = Vec3{0.0f, 0.0f, 0.0f};
        root->angular_momentum = Vec3{0.0f, 0.0f, 0.0f};
        root->pos_correction = Vec3{0.0f, 0.0f, 0.0f};
        if (sim)
            for (Wheel &w : sim->wheels) {
                w.spin = 0.0f;
                w.kappa_prev = 0.0f;
            }
        game::car_command(*rt, car_res, 0x10, Value::make_str("transmission 5"));
        c.check((car_payload->flags & 0x70u) == 0x60u,
                "transmission 5 sets the semi-auto flags 0x20|0x40 (got " +
                    std::to_string(car_payload->flags & 0x70u) + ")");
        game::car_command(*rt, car_res, 0x10, Value::make_str("reset"));
        int semi_settle = 0;
        while (std::fabs(car_payload->shaft_speed) > 1.0f && semi_settle < 300) {
            frames(1);
            semi_settle++;
        }
        // Check 2: neutral stays while throttle is held and no shift key is
        // pressed (0x4ad480 returns at gear 0; the auto box 0x10 is off).
        key(200, true);
        bool semi_neutral = true;
        for (int i = 0; i < 60; i++) {
            frames(1);
            if (car_payload->gear != 0) semi_neutral = false;
        }
        key(200, false);
        c.check(semi_neutral,
                "transmission 5 stays in neutral under throttle with no shift key "
                "(0x4ad480 returns at gear 0)");
        // Check 3: the 0x498300 shift block engages 1st from neutral.
        key(0x1e, true);
        frames(10);
        key(0x1e, false);
        frames(10);
        int semi_gear = car_payload->gear;
        c.check(car_payload->gears > 0 && semi_gear == 1,
                "A shifts neutral -> 1st under transmission 5 (got gear " +
                    std::to_string(semi_gear) + ", gears " +
                    std::to_string(car_payload->gears) + ")");
        // Check 4: drive with no shift key; only 0x4ad480 may shift.
        if (car_payload->gears > 0 && semi_gear == 1) {
            car_payload->semi_shifts = 0;
            std::vector<int> semi_gears;
            key(200, true);
            for (int i = 0; i < 600; i++) {
                frames(1);
                semi_gears.push_back(car_payload->gear);
            }
            key(200, false);
            int semi_max = 0, semi_changes = 0, semi_bad_step = 0, semi_bad_gear = 0;
            for (size_t i = 0; i < semi_gears.size(); i++) {
                int g = semi_gears[i];
                if (g == 0 || g == 7) semi_bad_gear++;
                if (g > semi_max) semi_max = g;
                if (i != 0) {
                    int step = g - semi_gears[i - 1];
                    if (step != 0) {
                        semi_changes++;
                        if (step != 1 && step != -1) semi_bad_step++;
                    }
                }
            }
            float semi_vel = vec_length(root->vel);
            float semi_h = sqrtf(root->vel.x * root->vel.x + root->vel.z * root->vel.z);
            float semi_rpm = car_payload->rpm_raw * 9.549296f;
            std::printf("  semi-auto drive (0x4ad480): max gear %d, semi_shifts %d, "
                        "gear changes %d, final |vel| %.2f m/s (horizontal %.2f, "
                        "y %.2f), final rpm %.0f, pos %.1f %.1f %.1f\n",
                        semi_max, car_payload->semi_shifts, semi_changes,
                        (double)semi_vel, (double)semi_h, (double)root->vel.y,
                        (double)semi_rpm, (double)root->pos.x, (double)root->pos.y,
                        (double)root->pos.z);
            c.check(car_payload->semi_shifts > 0,
                    "semi-auto shifts while driving (0x4ad480, got " +
                        std::to_string(car_payload->semi_shifts) + " shift(s))");
            c.check(semi_max >= 2,
                    "semi-auto reaches 2nd gear without a gear key (0x4ad480, got " +
                        std::to_string(semi_max) + ")");
            c.check(semi_bad_step == 0 &&
                        semi_changes <= car_payload->semi_shifts,
                    "every gear change is +-1 and no more changes than shifts (" +
                        std::to_string(semi_changes) + " change(s), " +
                        std::to_string(car_payload->semi_shifts) + " shift(s), " +
                        std::to_string(semi_bad_step) + " bad step(s))");
            c.check(semi_bad_gear == 0,
                    "semi-auto never selects neutral or reverse while driving (got " +
                        std::to_string(semi_bad_gear) + " frame(s))");
            c.check(v_top > 0.0f && semi_vel <= v_top,
                    "semi-auto final |vel| within v_top (" +
                        std::to_string((double)semi_vel) + " <= " +
                        std::to_string((double)v_top) + " m/s)");
        }
        // Check 5: restore the shipped transmission mode and its flags.
        game::car_command(*rt, car_res, 0x10,
                          Value::make_str("transmission " + std::to_string(tr)));
        c.check((car_payload->flags & 0x70u) == tr_bits,
                "transmission restored to " + std::to_string(tr) + " (flags " +
                    std::to_string(car_payload->flags & 0x70u) + " == " +
                    std::to_string(tr_bits) + ")");
    }

    std::vector<natives::ThreadInfo> threads = natives::thread_report(*boot.vm());
    if (dealer_at_garage) {
        bool dealer_thread = false;
        for (const natives::ThreadInfo &t : threads)
            if (t.name == "THRD-GARAGE-CMD") dealer_thread = true;
        c.check(dealer_thread, "Garage osdCommand(122) runs a THRD-GARAGE-CMD thread");
    }
    int failed = 0, not_impl = 0;
    for (const natives::ThreadInfo &t : threads) {
        if (t.error.empty()) continue;
        failed++;
        if (t.error.find("native not implemented") != std::string::npos) not_impl++;
    }
    if (not_impl) std::printf("  native not implemented: %d failed thread(s)\n", not_impl);
    // Finished threads are only counted; live and failed ones are listed.
    size_t done = 0;
    for (const natives::ThreadInfo &t : threads) {
        if (t.done && t.error.empty()) { done++; continue; }
        std::printf("  thread %s: %s\n", t.name.c_str(), thread_state(t).c_str());
    }
    std::printf("  threads done: %zu\n", done);
    std::printf("  frames: %d run, threads: %zu, failed: %d\n", kFrames, threads.size(),
                failed);
    c.check(failed == 0,
            "no failed thread (got " + std::to_string(failed) + ")");
}

// 0x55ccc0 roots: dir on the install -> package root. The package tree maps a
// fully-qualified class name back to the directory that holds the class.
struct PackageRoot {
    const char *dir;
    const char *pkg;
};
const PackageRoot kPackageRoots[] = {
    {"system\\scripts\\lang", "java.lang"},
    {"system\\scripts\\net", "java.net"},
    {"system\\scripts\\io", "java.io"},
    {"system\\scripts\\util", "java.util"},
    {"system\\scripts\\render", "java.render"},
    {"system\\scripts\\sound", "java.sound"},
    {"sl\\scripts\\game", "java.game"},
    {"parts\\scripts", "java.game.parts"},
    {"parts\\accessories\\scripts", "java.game.parts.accessories"},
};

struct TreeTally {
    long long files = 0;      // .class files seen
    long long resolved = 0;   // file's class name maps back to its own dir
    long long pkg_mismatch = 0;  // shipped: class declares a PARENT package of its dir
    long long failures = 0;      // anything else
    long long extra_units = 0;   // further class units inside a file (not FQN-loadable)
    long long unreadable = 0;
    int shown = 0;
};

// 0x55bec0 loads "dir\\ClassName.class": only the unit named like the file is
// reachable through the package tree. Further units in the same file are
// auxiliary (nested) classes and are counted, not checked.
void walk_classes(const std::string &disk_dir, const std::string &pkg,
                  jvm::PackageTree &tree, TreeTally &t)
{
    std::vector<fs::FindEntry> entries;
    if (!fs::find(disk_dir + "/*", &entries)) return;
    for (const fs::FindEntry &e : entries) {
        if (e.name == "." || e.name == "..") continue;
        std::string full = disk_dir + "/" + e.name;
        if (e.is_dir) {
            // Subdirectories are sub-packages (0x55f550); src only holds .java.
            if (e.name != "src") walk_classes(full, pkg + "." + e.name, tree, t);
            continue;
        }
        if (e.name.size() < 6 || e.name.compare(e.name.size() - 6, 6, ".class") != 0)
            continue;
        ClassFile file;
        std::string err;
        if (!classfile_load(full.c_str(), &file, &err)) {
            t.unreadable++;
            continue;
        }
        std::string stem = e.name.substr(0, e.name.size() - 6);
        const ClassUnit *own = nullptr;
        for (const ClassUnit &u : file.units) {
            std::string name = u.class_name(u.self_class);
            size_t dot = name.rfind('.');
            if (name.substr(dot == std::string::npos ? 0 : dot + 1) == stem) {
                own = &u;
                break;
            }
        }
        if (!own) {
            t.failures++;
            if (t.shown < 5) {
                std::printf("  FAIL no unit named '%s' in %s\n", stem.c_str(), full.c_str());
                t.shown++;
            }
            continue;
        }
        t.files++;
        t.extra_units += (long long)file.units.size() - 1;
        std::string name = own->class_name(own->self_class);
        std::string dir = tree.find_dir(name);
        std::string hit = dir.empty() ? std::string() : tree.disk_path(dir);
        if (hit == disk_dir) {
            t.resolved++;
            continue;
        }
        // Shipped quirk: CareerEvents/ and Gamemodes/ classes declare
        // "package java.game" (a parent package), so the FQN tree cannot reach
        // them; the install loads those files from their directory instead.
        size_t dot = name.rfind('.');
        std::string declared = dot == std::string::npos ? std::string() : name.substr(0, dot);
        bool parent_pkg = !declared.empty() && pkg.size() > declared.size() &&
                          pkg.compare(0, declared.size(), declared) == 0 &&
                          pkg[declared.size()] == '.';
        if (parent_pkg) {
            t.pkg_mismatch++;
            continue;
        }
        t.failures++;
        if (t.shown < 5) {
            std::printf("  FAIL %s says '%s' -> %s\n", full.c_str(), name.c_str(),
                        hit.empty() ? "(not found)" : hit.c_str());
            t.shown++;
        }
    }
}

void package_tree_check(Checker &c)
{
    jvm::PackageTree tree(".");   // selfcheck() chdirs to the game dir
    tree.add_default_roots();

    TreeTally t;
    for (const PackageRoot &root : kPackageRoots) {
        std::string disk = tree.disk_path(root.dir);
        if (!disk.empty()) walk_classes(disk, root.pkg, tree, t);
    }
    std::printf("  package tree: %lld classes checked, %lld resolved, %lld package/dir "
                "mismatches (shipped), %lld failures, %lld extra units, %lld unreadable "
                "(%zu nodes)\n",
                t.files, t.resolved, t.pkg_mismatch, t.failures, t.extra_units, t.unreadable,
                tree.node_count());
    c.check(t.files > 0, "package tree checked at least one shipped class");
    c.check(t.failures == 0 && t.unreadable == 0,
            "every shipped class file resolves back to its own directory");

    std::string lang = tree.find_dir("java.util.resource.GameRef");
    std::string game = tree.find_dir("java.game.Init");
    c.check(!lang.empty(), "find_dir(java.util.resource.GameRef) -> " + lang);
    c.check(!game.empty(), "find_dir(java.game.Init) -> " + game);
}

bool iequals(const std::string &a, const std::string &b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = (char)(y + ('a' - 'A'));
        if (x != y) return false;
    }
    return true;
}

bool ends_with_ci(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    return s.size() >= n && iequals(s.substr(s.size() - n), suffix);
}

// One shipped script that exists only as <dir>\src\Name.java (0x5598f0).
struct JavaSource {
    std::string java_path;  // actual disk path, for reading the source
    std::string src_dir;    // actual disk path of the src directory
    std::string rel_dir;    // exe-style X:\... relative to the game dir
    std::string base;
};

// Every <dir>\src\X.java without a sibling <dir>\X.class below disk_dir.
// Windows path matching is case-insensitive, so `src`, `Src` and `SRC` all
// count and the class name is compared ignoring case.
void collect_java_only(const std::string &disk_dir, const std::string &rel_dir,
                       std::vector<JavaSource> *out)
{
    std::vector<fs::FindEntry> entries;
    if (!fs::find(disk_dir + "/*", &entries)) return;
    for (const fs::FindEntry &e : entries) {
        if (e.name == "." || e.name == ".." || !e.is_dir) continue;
        std::string full = disk_dir + "/" + e.name;
        std::string rel = rel_dir.empty() ? e.name : rel_dir + "\\" + e.name;
        if (!iequals(e.name, "src")) {
            collect_java_only(full, rel, out);
            continue;
        }
        std::vector<fs::FindEntry> javas;
        if (!fs::find(full + "/*.java", &javas)) continue;
        for (const fs::FindEntry &j : javas) {
            if (j.is_dir || !ends_with_ci(j.name, ".java")) continue;
            std::string base = j.name.substr(0, j.name.size() - 5);
            std::vector<fs::FindEntry> cls;
            bool have = fs::find(disk_dir + "/" + base + ".class", &cls) && !cls.empty();
            if (!have) out->push_back(JavaSource{full + "/" + j.name, full, rel_dir, base});
        }
    }
}

// 0x5598f0's missing-.class case over the 9 package roots: 16 shipped base
// classes exist only as parts\scripts[..]\src\Name.java. Resolving them by FQN
// goes through 0x55add0 -> 0x55c0c0 -> 0x55bec0 -> compileSource.
void package_root_compile_check(Boot &boot, Checker &c)
{
    std::vector<JavaSource> sources;
    for (const PackageRoot &root : kPackageRoots) {
        std::string disk = boot.tree()->disk_path(root.dir);
        if (!disk.empty()) collect_java_only(disk, root.dir, &sources);
    }
    int resolved = 0;
    std::vector<std::string> failed;
    for (const JavaSource &s : sources) {
        std::vector<uint8_t> bytes;
        std::string err, src;
        JavaUnit ju;
        std::vector<Token> tokens;
        if (!fs::read_file(s.java_path, &bytes, &err))
            src.clear();
        else
            src.assign(bytes.begin(), bytes.end());
        if (!bytes.empty() && parse_java(src, &ju, &err, &tokens)) {
            std::string fqn = ju.package.empty() ? s.base : ju.package + "." + s.base;
            if (boot.vm()->get_class(fqn)) {
                resolved++;
                continue;
            }
            failed.push_back(fqn);
        } else {
            failed.push_back(s.rel_dir + "\\" + s.base + ".java: " + err);
        }
    }
    std::printf("  package-root compile on load: %zu sources, %d resolved, %zu failed\n",
                sources.size(), resolved, failed.size());
    for (size_t i = 0; i < failed.size() && i < 10; i++)
        std::printf("    failed: %s\n", failed[i].c_str());
    c.check(sources.size() == 16 && resolved == 16,
            "the 16 package-root .java-only classes compile and resolve");
}

// resman --selfcheck finds 4 Axis.rpk `script` refs with only a .java source.
// The record path (0x51fcc0 -> 0x55aca0 -> 0x55bec0) loads each through
// load_class_at.
void axis_record_compile_check(Boot &boot, Checker &c)
{
    ResourceManager axis;
    std::string err;
    if (!axis.open("cars/racers/Axis.rpk", nullptr, &err)) {
        c.check(false, "Axis.rpk opens for the record-script check: " + err);
        return;
    }
    int found = 0, loaded = 0;
    std::vector<std::string> failed;
    for (size_t i = 0; i < axis.package_count(); i++) {
        const Package &pkg = axis.package(i);
        for (const RpkRecord &rec : pkg.rpk.records) {
            for (const DataRef &ref : axis.refs(pkg, rec)) {
                if (!ref.file || !ref.is_class || !ref.path.empty()) continue;
                found++;
                // The record stores `<dir>\Name.class`; compileSource looks for
                // `<dir>\src\Name.java` beside it.
                size_t sep = ref.text.find_last_of("\\/");
                if (sep == std::string::npos) {
                    failed.push_back(ref.text);
                    continue;
                }
                std::string dir = ref.text.substr(0, sep);
                std::string base = ref.text.substr(sep + 1);
                base.resize(base.size() - 6);   // ".class"
                std::string fqn;
                if (jvm::load_class_at(*boot.tree(), *boot.classes(), *boot.vm(), dir, base,
                                       &fqn))
                    loaded++;
                else
                    failed.push_back(ref.text);
            }
        }
    }
    std::printf("  Axis record scripts: %d java-only refs, %d load through "
                "load_class_at\n",
                found, loaded);
    for (size_t i = 0; i < failed.size() && i < 10; i++)
        std::printf("    failed: %s\n", failed[i].c_str());
    c.check(found == 4 && loaded == 4,
            "the 4 Axis record scripts missing their .class compile on load");
}

// Report-only sweep: every <dir>\src\X.java in the game dir without X.class.
void compile_on_load_sweep(Boot &boot)
{
    std::vector<JavaSource> sources;
    collect_java_only(".", "", &sources);
    int ok = 0, failed_count = 0;
    std::vector<std::string> failed;
    for (const JavaSource &s : sources) {
        std::string fqn;
        if (jvm::load_class_at(*boot.tree(), *boot.classes(), *boot.vm(), s.rel_dir, s.base,
                               &fqn))
            ok++;
        else {
            failed_count++;
            if (failed.size() < 10)
                failed.push_back(s.rel_dir + "\\" + s.base);
        }
    }
    std::printf("  compile on load: %zu sources, ok %d, failed %d\n", sources.size(), ok,
                failed_count);
    for (const std::string &f : failed) std::printf("    failed: %s\n", f.c_str());
}


// 0x51fcc0 -> 0x55aca0 -> 0x55bec0: every freeride map's record 0x102 points
// at <map>\data\track_data.class under the same FQN java.game.track_data, but a
// record-path load creates a fresh Class per record. Its init() must therefore
// set that map's own e_name.
void track_data_check(Boot &boot, Checker &c)
{
    std::vector<fs::FindEntry> dirs;
    fs::find("multibot/maps/*", &dirs);
    std::vector<std::string> maps;
    std::vector<size_t> pkg_index;
    for (const fs::FindEntry &e : dirs) {
        if (!e.is_dir || e.name == "." || e.name == "..") continue;
        std::string path = "multibot/maps/" + e.name + "/t_data.rpk";
        if (!fs::exists(path)) continue;
        std::string err;
        if (!boot.resources()->open(path.c_str(), nullptr, &err)) {
            std::printf("    %s: %s\n", e.name.c_str(), err.c_str());
            continue;
        }
        maps.push_back(e.name);
        pkg_index.push_back(boot.resources()->package_count() - 1);
    }
    c.check(maps.size() == 21,
            "all 21 freeride t_data packages open (" + std::to_string(maps.size()) + ")");

    std::set<ClassState *> states;
    int named = 0;
    std::vector<std::string> bad;
    for (size_t i = 0; i < maps.size(); i++) {
        const std::string &map = maps[i];
        // Re-fetch by index: resolving may lazily open more packages.
        const Package &pkg = boot.resources()->package(pkg_index[i]);
        if (!pkg.rpk.find(0x102)) {
            bad.push_back(map + ": no record 0x102");
            continue;
        }
        Runtime *rt = boot.runtime();
        Runtime::Resource *type = rt->resolve(0x102, &pkg);
        if (!type) {
            bad.push_back(map + ": record 0x102 does not resolve");
            continue;
        }
        // CareerEvent.init (CareerEvent.java:124-128): GameRef.create(null, type,
        // null, "track data") then track.init() and track.e_name.
        Runtime::Resource *inst =
            rt->instantiate(type, rt->game_root(), "track data", "", true, nullptr);
        Object *script = inst ? inst->script : nullptr;
        if (!script) {
            bad.push_back(map + ": no script instance");
            continue;
        }
        states.insert(script->cls);
        std::string err;
        if (!rt->call_method(script, "init", {}, nullptr, &err)) {
            bad.push_back(map + ": init(): " + err);
            continue;
        }
        Value v;
        if (!boot.vm()->get_instance_field(script, "e_name", &v, &err) ||
            v.kind != Value::Str) {
            bad.push_back(map + ": e_name: " + err);
            continue;
        }
        std::string java_path = "multibot/maps/" + map + "/data/track_data.java";
        std::vector<uint8_t> src_bytes;
        if (!fs::read_file(java_path, &src_bytes, &err)) {
            bad.push_back(map + ": " + err);
            continue;
        }
        std::string src(src_bytes.begin(), src_bytes.end());
        size_t p = src.find("e_name");
        size_t q = p == std::string::npos ? p : src.find('"', p);
        size_t r = q == std::string::npos ? q : src.find('"', q + 1);
        std::string want =
            r == std::string::npos ? std::string() : src.substr(q + 1, r - q - 1);
        if (want.empty() || v.s != want) {
            bad.push_back(map + ": e_name \"" + v.s + "\" != \"" + want + "\"");
            continue;
        }
        named++;
    }
    for (size_t i = 0; i < bad.size() && i < 10; i++)
        std::printf("    %s\n", bad[i].c_str());
    c.check(named == 21 && bad.empty(),
            "each map's track_data.init() sets its own e_name (" +
                std::to_string(named) + "/21)");
    c.check(states.size() == 21,
            "record-path loads bind one ClassState per map (" +
                std::to_string(states.size()) + ")");
}

int selfcheck()
{
    Checker c;
    // The exe runs with the game dir as cwd; script FindFile (System.rpkScan)
    // patterns are relative to it.
    char cwd[4096];
    std::string abs = std::string(getcwd(cwd, sizeof cwd)) + "/" + kGameDir;
    if (chdir(abs.c_str()) != 0) std::fprintf(stderr, "vmboot: chdir %s failed\n", abs.c_str());
    const std::string rpk = abs + "/system.rpk";
    std::printf("vmboot selfcheck: %s (rpk only)\n", rpk.c_str());

    // Before any script runs: ControlSet.load resolves saved device names
    // through the table during Init(int), so the backend must be installed.
    install_synthetic_input();

    // PhysicsWorld__init 0x4fffb0: the process world is created at engine
    // startup, before any script runs (0x60251c).
    World &world = process_world();
    c.check(float_bits(world.step_size) == 0x3ca3d70au,
            "process world step_size is 0.02f (0x4fffb0)");
    c.check(float_bits(world.gravity.y) == 0xc11cf5c3u,
            "process world gravity.y is -9.81f (0x4fffb0)");
    c.check(float_bits(world.air_density) == 0x3fa57a78u,
            "process world air_density is 1.2928f (0x4fffb0)");
    c.check(world.time == 0.0f && world.accumulator == 0.0f,
            "process world time and accumulator start at 0 (0x4fffb0)");

    Boot boot;
    BootResult first = boot.run(rpk);
    for (const std::string &e : first.errors) std::printf("  error: %s\n", e.c_str());
    c.check(first.packages > 0,
            "packages open (" + std::to_string(first.packages) + ")");
    c.check(first.entry_found, "game.Init resolves");
    c.check(first.clinit_ran, "game.Init static initializer executes");
    char value[64];
    snprintf(value, sizeof value, "%d",
             first.clinit_value.kind == Value::Int ? first.clinit_value.i : -1);
    c.check(first.clinit_ran && first.clinit_value.kind == Value::Int &&
                first.clinit_value.i == 100,
            std::string("static initializer returns 100 (got ") + value + ")");
    c.check(natives::table().size() == 422,
            "native registry has 422 entries (" +
                std::to_string(natives::table().size()) + ")");

    const BootResult &again = boot.run(rpk);
    c.check(again.clinit_runs == 1,
            "second run in the same runtime does not rerun the initializer (runs=" +
                std::to_string(again.clinit_runs) + ")");

    boot.reset();
    BootResult fresh = boot.run(rpk);
    for (const std::string &e : fresh.errors) std::printf("  error: %s\n", e.c_str());
    c.check(fresh.ok && fresh.clinit_runs == 1 && fresh.clinit_value.kind == Value::Int &&
                fresh.clinit_value.i == first.clinit_value.i,
            "fresh runtime rerun gives the same result");

    ClassEntry *entry = boot.classes() ? boot.classes()->find("java.game.Init") : nullptr;
    std::vector<std::string> missing;
    bool all_reported = true;
    if (entry) {
        for (size_t i = 0; i < entry->unit->consts.size(); i++) {
            if (entry->unit->consts[i].type != CONST_CLASS) continue;
            std::string name = entry->unit->class_name((int)i);
            if (name.empty() || boot.classes()->find(name)) continue;
            missing.push_back(name);
            if (!contains(fresh.external, name)) all_reported = false;
        }
    }
    c.check(!missing.empty(),
            "game.Init references classes outside the opened package (" +
                std::to_string(missing.size()) + ")");
    c.check(entry && all_reported,
            "every missing class referenced by game.Init is reported external");

    // 0x55add0/0x55c0c0/0x55bec0: resolve every class game.Init names through
    // the package tree. Names that still fail are listed, not counted as a
    // failure by themselves; a class file the tree finds but cannot parse is.
    jvm::PackageTree tree(kGameDir);
    tree.add_default_roots();
    int resolved = 0, parse_failures = 0;
    std::vector<std::string> unresolved, seen;
    if (entry) {
        for (size_t i = 0; i < entry->unit->consts.size(); i++) {
            if (entry->unit->consts[i].type != CONST_CLASS) continue;
            std::string name = entry->unit->class_name((int)i);
            if (name.empty() || name[0] == '[') continue;  // array descriptor
            if (contains(seen, name)) continue;
            seen.push_back(name);
            if (boot.vm()->get_class(name)) {
                resolved++;
                continue;
            }
            unresolved.push_back(name);
            std::string dir = tree.find_dir(name);
            if (dir.empty()) continue;
            size_t dot = name.rfind('.');
            std::string base = dot == std::string::npos ? name : name.substr(dot + 1);
            if (!tree.disk_path(dir + "\\" + base + ".class").empty()) parse_failures++;
        }
    }
    int other = resolved - (contains(seen, "java.lang.GameType") ? 1 : 0);
    std::printf("  get_class over game.Init CONST_CLASS: %zu names, %d resolved, %zu "
                "unresolved, %d parse failures\n",
                seen.size(), resolved, unresolved.size(), parse_failures);
    for (const std::string &n : unresolved) std::printf("    unresolved: %s\n", n.c_str());
    c.check(boot.vm()->get_class("java.lang.GameType") != nullptr,
            "superclass java.lang.GameType loads through the package tree");
    c.check(other >= 1,
            "at least one other game.Init class name loads on demand (" +
                std::to_string(other) + ")");
    c.check(parse_failures == 0,
            "no class file the package tree finds fails to parse");

    package_tree_check(c);
    vector3_check(boot, c);
    game_init_check(boot, c);
    track_data_check(boot, c);
    package_root_compile_check(boot, c);
    axis_record_compile_check(boot, c);
    compile_on_load_sweep(boot);   // report only

    boot.reset();
    std::printf("selfcheck: %s (%d checks, %d failure(s))\n",
                c.failures ? "FAILED" : "PASSED", c.checks, c.failures);
    return c.failures ? 1 : 0;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--selfcheck") == 0) {
        int rc = selfcheck();
        return rc;
    }
    std::fprintf(stderr, "usage: vmboot --selfcheck\n");
    return 2;
}
