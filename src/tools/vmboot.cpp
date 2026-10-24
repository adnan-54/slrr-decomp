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
#include <string>
#include <vector>

#include "app/boot.h"
#include "input/backend.h"
#include "input/device.h"
#include "platform/fs.h"
#include "script/classfile.h"
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
    const int kFrames = 18000, kPress = 3000, kRelease = 3060, kSelect = 3600;
    const int kGarageCmd = 6300, kBuyCmd = 12300, kBackCmd = 14000;
    FrameLoop loop(0);
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
    // frame-14000 AXIS_CANCEL returns to the Garage with player.car set.
    c.check(class_after == "java.game.Garage",
            "back from the used dealer: actualState is java.game.Garage (got " +
                class_after + ")");
    c.check(player_car != nullptr, "buying the shown used car sets GameLogic.player.car");

    std::vector<natives::ThreadInfo> threads = natives::thread_report(*boot.vm());
    if (dealer_at_garage) {
        bool dealer_thread = false;
        for (const natives::ThreadInfo &t : threads)
            if (t.name == "THRD-GARAGE-CMD") dealer_thread = true;
        c.check(dealer_thread, "Garage osdCommand(122) runs a THRD-GARAGE-CMD thread");
    }
    int failed = 0;
    for (const natives::ThreadInfo &t : threads)
        if (!t.error.empty()) failed++;
    // Finished threads are only counted; live and failed ones are listed.
    size_t done = 0;
    for (const natives::ThreadInfo &t : threads) {
        if (t.done && t.error.empty()) { done++; continue; }
        std::printf("  thread %s: %s\n", t.name.c_str(), thread_state(t).c_str());
    }
    std::printf("  threads done: %zu\n", done);
    std::printf("  frames: %d run, threads: %zu, failed: %d\n", kFrames, threads.size(),
                failed);
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
