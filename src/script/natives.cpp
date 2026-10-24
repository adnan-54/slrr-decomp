// Native methods for the script VM. Semantics follow the exe implementations
// documented in notes/06-native-methods.md and the shipped 2.2.1 java sources.
// Engine-dependent (or not yet reconstructed) methods are registered as explicit
// stubs that fail with their qualified name instead of silently doing nothing.
#include "script/natives.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "audio/audio.h"
#include "audio/music.h"
#include "audio/sfx.h"
#include "audio/sound.h"
#include "engine/math.h"
#include "game/car.h"
#include "game/runtime.h"
#include "input/controller.h"
#include "input/cursor.h"
#include "input/device.h"
#include "input/hotkeys.h"
#include "platform/fs.h"
#include "script/vm.h"
#include "vehicle/dyno.h"

namespace {

using Args = std::vector<Value>;

bool want(const char *who, const Args &a, size_t n, std::string *err)
{
    if (a.size() < n) {
        *err = std::string(who) + ": expected " + std::to_string(n) + " argument(s)";
        return false;
    }
    return true;
}

bool get_i(const char *who, const Value &v, int32_t *out, std::string *err)
{
    if (v.kind != Value::Int) {
        *err = std::string(who) + ": expected an int";
        return false;
    }
    *out = v.i;
    return true;
}

bool get_f(const char *who, const Value &v, float *out, std::string *err)
{
    if (v.kind == Value::Float) {
        *out = v.f;
        return true;
    }
    if (v.kind == Value::Int) {  // callMethod converts int args for F parameters
        *out = (float)v.i;
        return true;
    }
    *err = std::string(who) + ": expected a float";
    return false;
}

// 0x4ec980 Instance__getFloatField (defined with the DynoData natives below).
float dd_get_f(Vm &vm, Object *self, const char *name);

bool get_s(const char *who, const Value &v, const std::string **out, std::string *err)
{
    if (v.kind != Value::Str) {
        *err = std::string(who) + ": expected a String";
        return false;
    }
    *out = &v.s;
    return true;
}

bool get_o(const char *who, const Value &v, Object **out, std::string *err)
{
    if (v.kind != Value::Obj || !v.obj) {
        *err = std::string(who) + ": expected an object";
        return false;
    }
    *out = v.obj;
    return true;
}

// Object identity for natives that keep an opaque reference; a non-object
// argument becomes null, like the exe's unchecked field store would never run.
void *obj(const Value &v) { return v.kind == Value::Obj ? (void *)v.obj : nullptr; }

// ---- java.lang.Thread cooperative scheduler --------------------------------

// The exe binds every script Thread to an engine Thread (0x38 bytes: frame,
// state, priority, flags) and runs it in 256-token VM slices. Natives address
// the calling engine Thread through Scheduler::current and set its +0x2c flags;
// any flag in mask 0xfa stops the thread at the next instruction (0x563290).
struct ScriptThreadState {
    Object *script = nullptr;  // java.lang.Thread instance (identity)
    Object *target = nullptr;  // Runnable whose run() this thread executes
    std::unique_ptr<Thread> runner;
    std::string name;
    int32_t priority = 0;      // engine Thread ctor (0x5612f0) starts at 0
    bool daemon = false;
    bool suspended = false;    // +0x2c bit 0x20
    bool started = false;
    bool done = false;
    bool in_slice = false;  // its run_slice is on the stack (do not re-enter)
    bool waiting = false;   // engine thread+0x2c bit 0x10: on the monitor wait list
    bool sleeping = false;  // +0x2c bit 0x08; runnable again once wake_ms passes
    double wake_ms = 0;     // +0x30
    Object *wait_on = nullptr;  // +0x34 wait object (Thread__stop notifies it)
};

struct Scheduler {
    std::vector<std::unique_ptr<ScriptThreadState>> threads;
    // Level-2 sim clock; advanced by the sim step 0x47e050 (card 102).
    double sim_seconds = 0.0;
    // 0x569730 QueryPerformanceCounter milliseconds, read by Thread__sleep.
    // INVENTED: src has no QPC clock here; the host passes its frame clock.
    double now_ms = 0.0;
    // 0x4e5880 stores the scale through 0x47dfd0 into the global at 0x5fde90
    // (+0x27c); normal speed is 1.0 (playVideo saves and restores it).
    float warp = 1.0f;
    // Engine globals written by the java.lang.System natives below.
    int quit_request = 0;        // 0x5f0c74 quit request flag
    // INVENTED: the exe's initial measure was not traced; 0 until setMeasure.
    float measure = 0.0f;        // 0x5da524
    float measure_per_hour = 0.0f;  // 0x5fe378
    float measure_inv10 = 0.0f;  // 0x5fe374
    int day_seconds = 0;         // 0x6024c0
    int time_flag_5da080 = 0;    // 0x5da080
    float load_slice = 0.0f;     // 0x5da464
    int load_priority = 0;       // 0x5fe3bc
    bool pumping = false;
    // 0x54d430 gets the executing engine Thread as ctx+4; the equivalent here is the
    // ScriptThreadState whose slice is running (set by run_threads).
    ScriptThreadState *current = nullptr;
    // Receiver's wait list: the exe keeps a T_Container* at object+0x14. Object
    // identity is the key; entries are appended and popped at the tail (LIFO).
    std::map<Object *, std::vector<ScriptThreadState *>> monitors;
};

// shortcut: scheduler state is never pruned (Vm has no teardown hook); the game
// runs one long-lived Vm, so this only matters to tools that churn Vm instances.
std::map<Vm *, Scheduler> &schedulers()
{
    static std::map<Vm *, Scheduler> m;
    return m;
}

Scheduler &sched(Vm &vm) { return schedulers()[&vm]; }

ScriptThreadState *thread_state(Vm &vm, Object *o)
{
    for (auto &t : sched(vm).threads)
        if (t->script == o) return t.get();
    return nullptr;
}

ScriptThreadState *thread_state_for(Vm &vm, Object *o)
{
    ScriptThreadState *t = thread_state(vm, o);
    if (t) return t;
    std::unique_ptr<ScriptThreadState> n(new ScriptThreadState);
    n->script = o;
    t = n.get();
    sched(vm).threads.push_back(std::move(n));
    return t;
}

void monitor_notify_thread(ScriptThreadState *t);  // 0x5629a0, defined below

// vm.h keeps ClassState opaque. vm.cpp defines it with `unit` as its first
// member, so the declaring unit can be recovered via memcpy without widening
// the VM interface.
// shortcut: relies on ClassState's leading member in vm.cpp; upgrade when Vm
// exposes a ClassState -> ClassUnit accessor.
const ClassUnit *class_state_unit(ClassState *st)
{
    const ClassUnit *unit = nullptr;
    if (st) ::memcpy(&unit, st, sizeof unit);
    return unit;
}

bool thread_init(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("Thread.init", a, 2, err) || !get_o("Thread.init", a[0], &o, err))
        return false;
    ScriptThreadState *t = thread_state_for(vm, o);
    t->name = a[1].kind == Value::Str ? a[1].s : std::string();
    *out = Value::make_null();
    return true;
}

// 0x4e6170: clears flag 2, resolves the script `target` field and starts
// target.run() as a VM thread. A missing target or run() terminates it.
bool thread_start(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("Thread.start", a, 1, err) || !get_o("Thread.start", a[0], &o, err))
        return false;
    *out = Value::make_null();
    ScriptThreadState *t = thread_state_for(vm, o);
    if (t->started || t->done) return true;  // one start per script Thread
    Value tv;
    std::string e;
    if (!vm.get_instance_field(o, "target", &tv, &e) || tv.kind != Value::Obj ||
        !tv.obj) {
        t->done = true;
        return true;
    }
    ClassState *decl = nullptr;
    const MethodInfo *run = vm.find_method(tv.obj->cls, "run", "", 0, {}, &decl, &e);
    const ClassUnit *unit = class_state_unit(decl);
    std::unique_ptr<Thread> runner =
        run && unit ? vm.start(*unit, *run, tv.obj, {}, &e) : nullptr;
    if (!runner) {
        t->done = true;
        return true;
    }
    t->target = tv.obj;
    t->runner = std::move(runner);
    t->started = true;
    return true;
}

bool thread_stop(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("Thread.stop", a, 1, err) || !get_o("Thread.stop", a[0], &o, err))
        return false;
    if (ScriptThreadState *t = thread_state(vm, o)) {
        Scheduler &s = sched(vm);
        // 0x5648f0/0x54d290: a waiting thread leaves its monitor wait list.
        if (t->waiting) {
            bool found = false;
            auto it = s.monitors.find(t->wait_on);
            if (it != s.monitors.end()) {
                std::vector<ScriptThreadState *> &list = it->second;
                for (size_t i = list.size(); i-- > 0;) {
                    if (list[i] != t) continue;
                    list.erase(list.begin() + i);  // exe nulls the slot
                    monitor_notify_thread(t);
                    found = true;
                    break;
                }
                if (list.empty()) s.monitors.erase(it);
            }
            if (!found)
                std::fprintf(stderr,
                             "Instance::notify(Thread*): inconsistency\n");
        }
        t->started = false;
        t->done = true;
        t->target = nullptr;  // exe clears +0x18 and detaches the handle
        // 0x4e6200: the 0xfa flag test stops the caller at the next instruction.
        if (t == s.current && t->runner) t->runner->request_stop();
    }
    *out = Value::make_null();
    return true;
}

bool thread_is_alive(Vm &, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("Thread.isAlive", a, 1, err) || !get_o("Thread.isAlive", a[0], &o, err))
        return false;
    // 0x4e6040: the exe stub always returns 0 (assembly ends in xor eax,eax).
    (void)o;
    *out = Value::make_int(0);
    return true;
}

bool thread_get_priority(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("Thread.getPriority", a, 1, err) ||
        !get_o("Thread.getPriority", a[0], &o, err))
        return false;
    ScriptThreadState *t = thread_state(vm, o);
    *out = Value::make_int(t ? t->priority : 0);
    return true;
}

bool thread_set_priority(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    int32_t pri;
    if (!want("Thread.setPriority", a, 2, err) ||
        !get_o("Thread.setPriority", a[0], &o, err) ||
        !get_i("Thread.setPriority", a[1], &pri, err))
        return false;
    if (ScriptThreadState *t = thread_state(vm, o)) t->priority = pri;
    *out = Value::make_null();
    return true;
}

bool thread_sleep(Vm &vm, const Args &a, Value *out, std::string *err)
{
    float ms;
    // static native (Thread.class flags 0x49): no receiver, the float is a[0].
    if (!want("Thread.sleep", a, 1, err) || !get_f("Thread.sleep", a[0], &ms, err))
        return false;
    *out = Value::make_null();
    Scheduler &s = sched(vm);
    ScriptThreadState *t = s.current;  // 0x4e6130: ctx+4, the calling thread
    // shortcut: a native invoked synchronously (Vm::invoke / run_field_init) has
    // no executing script thread in src; the exe always has ctx+4. No-op.
    if (!t) return true;
    t->sleeping = true;                  // 0x564860: flags |= 0x08
    t->wake_ms = s.now_ms + (double)ms;  // +0x30 = now + ms
    t->runner->request_stop();           // 0x563290: leave the slice
    return true;
}

bool thread_suspend(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("Thread.suspend", a, 1, err) || !get_o("Thread.suspend", a[0], &o, err))
        return false;
    // 0x564890: set 0x20; the 0xfa test (0x563290) then stops the caller.
    if (ScriptThreadState *t = thread_state(vm, o)) {
        if (!t->suspended) {
            t->suspended = true;
            if (t == sched(vm).current) t->runner->request_stop();
        }
    }
    *out = Value::make_null();
    return true;
}

bool thread_resume(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("Thread.resume", a, 1, err) || !get_o("Thread.resume", a[0], &o, err))
        return false;
    // 0x562d10: only clear 0x20; the run-list relink is the pump's job.
    if (ScriptThreadState *t = thread_state(vm, o)) t->suspended = false;
    *out = Value::make_null();
    return true;
}

bool thread_set_daemon(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    int32_t on;
    if (!want("Thread.setDaemon", a, 2, err) ||
        !get_o("Thread.setDaemon", a[0], &o, err) ||
        !get_i("Thread.setDaemon", a[1], &on, err))
        return false;
    if (ScriptThreadState *t = thread_state(vm, o)) t->daemon = on != 0;
    *out = Value::make_null();
    return true;
}

// ---- java.lang.Math --------------------------------------------------------

// MSVC CRT rand/srand state (0x571110/0x571131): state = state*214013+2531011,
// result = (state >> 16) & 0x7fff. The CRT starts seeded with 1.
uint32_t g_rand_state = 1;

uint32_t msvc_rand()
{
    g_rand_state = g_rand_state * 214013u + 2531011u;
    return (g_rand_state >> 16) & 0x7fffu;
}

void msvc_srand(uint32_t seed)
{
    g_rand_state = seed;
}

// 0x569730: high-resolution clock in milliseconds. The exe subtracts a
// process-lifetime baseline; ours starts at the first call, which is equivalent
// for the monotonic differences scripts use.
double milliseconds_now()
{
    static const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start)
        .count();
}

bool math_random(Vm &, const Args &, Value *out, std::string *)
{
    // 0x4dee50: 0x7fff maps to 0x7ffe so the result stays below 1.
    uint32_t u = msvc_rand();
    if (u == 0x7fffu) u = 0x7ffeu;
    *out = Value::make_float((float)(int)u * (float)(1.0 / 32767.0));
    return true;
}

bool math_sqrt(Vm &, const Args &a, Value *out, std::string *err)
{
    float x;
    if (!want("Math.sqrt", a, 1, err) || !get_f("Math.sqrt", a[0], &x, err))
        return false;
    *out = Value::make_float(sqrtf(x));  // 0x4deee0: x87 fsqrt, no domain check
    return true;
}

bool math_randomize(Vm &, const Args &, Value *out, std::string *)
{
    // 0x4dee90: srand((int)milliseconds since process start).
    msvc_srand((uint32_t)(int)milliseconds_now());
    *out = Value::make_null();
    return true;
}

bool math_setrandseed(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t seed;
    if (!want("Math.setrandseed", a, 1, err) ||
        !get_i("Math.setrandseed", a[0], &seed, err))
        return false;
    msvc_srand((uint32_t)seed);
    *out = Value::make_null();
    return true;
}

bool math_ln(Vm &, const Args &a, Value *out, std::string *err)
{
    float x;
    if (!want("Math.ln", a, 1, err) || !get_f("Math.ln", a[0], &x, err))
        return false;
    // 0x4decf0: logs bad input but still returns logf. log10 is registered at
    // the same address, so it returns the natural log too (game bug).
    if (x <= 0.0f) fprintf(stderr, "vm_math_ln: incorrect argument %f\n", (double)x);
    *out = Value::make_float(logf(x));
    return true;
}

bool math_exp(Vm &, const Args &a, Value *out, std::string *err)
{
    float x;
    if (!want("Math.exp", a, 1, err) || !get_f("Math.exp", a[0], &x, err))
        return false;
    *out = Value::make_float(expf(x));  // 0x4decc0
    return true;
}

bool math_pow(Vm &, const Args &a, Value *out, std::string *err)
{
    float x, y;
    if (!want("Math.pow", a, 2, err) || !get_f("Math.pow", a[0], &x, err) ||
        !get_f("Math.pow", a[1], &y, err))
        return false;
    *out = Value::make_float(powf(x, y));  // 0x4dee10 -> SSE powf 0x571143
    return true;
}

bool math_atan2(Vm &, const Args &a, Value *out, std::string *err)
{
    float y, x;
    if (!want("Math.atan2", a, 2, err) || !get_f("Math.atan2", a[0], &y, err) ||
        !get_f("Math.atan2", a[1], &x, err))
        return false;
    *out = Value::make_float(atan2f(y, x));  // 0x4dec90: fpatan(arg0, arg1)
    return true;
}

// ---- java.lang.System ------------------------------------------------------

bool noop_void(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_null();
    return true;
}

bool zero_int(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_int(0);  // 0x461910 runFinalization returns 0
    return true;
}

// ---- java.net.NetworkEngine ----------------------------------------------
// 0x461910: xor eax,eax; ret. Shared with System.runFinalization; the exe's
// networking is disabled, so httpRequest's L return becomes a null reference.
bool network_http_request(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_null();
    return true;
}

bool system_arraycopy(Vm &, const Args &a, Value *out, std::string *err)
{
    const char *who = "System.arraycopy";
    if (!want(who, a, 5, err)) return false;
    if (a[0].kind != Value::Array || a[2].kind != Value::Array) {
        *err = std::string(who) + ": expected arrays";
        return false;
    }
    int32_t src_pos, dst_pos, len;
    if (!get_i(who, a[1], &src_pos, err) || !get_i(who, a[3], &dst_pos, err) ||
        !get_i(who, a[4], &len, err))
        return false;
    Array *src = a[0].arr;
    Array *dst = a[2].arr;
    // 0x4e5280 checks each upper bound and returns without copying. The exe
    // would index out of range for negative values; treat them as failed bounds.
    if (src_pos < 0 || dst_pos < 0 || len < 0 ||
        (int64_t)src->elems.size() < (int64_t)src_pos + len ||
        (int64_t)dst->elems.size() < (int64_t)dst_pos + len) {
        *out = Value::make_null();
        return true;
    }
    if (src == dst) {
        std::vector<Value> tmp(src->elems.begin() + src_pos,
                               src->elems.begin() + src_pos + len);
        for (int32_t i = 0; i < len; i++) dst->elems[dst_pos + i] = tmp[(size_t)i];
    } else {
        for (int32_t i = 0; i < len; i++)
            dst->elems[dst_pos + i] = src->elems[src_pos + i];
    }
    *out = Value::make_null();
    return true;
}

bool system_current_time(Vm &, const Args &, Value *out, std::string *)
{
    // 0x4e5500: milliseconds * 0.001, returned as float seconds.
    *out = Value::make_float((float)(milliseconds_now() * 0.001));
    return true;
}

bool system_build_number(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_int(940);  // 0x4e5470 returns 0x3ac
    return true;
}

// 0x4e5700: writes the text (or "<null>") to the engine log channel.
bool system_log(Vm &, const Args &a, Value *out, std::string *)
{
    const std::string &s = a.empty() || a[0].kind != Value::Str ? std::string("<null>")
                                                                : a[0].s;
    std::fprintf(stderr, "%s\n", s.c_str());
    *out = Value::make_null();
    return true;
}

// ---- java.util.Steam ------------------------------------------------------

// 0x7a75df: global "neutralized" flag, read by 0x49446b, 0x494646, 0x49a4f8.
bool steam_neutralized = false;

// INVENTED: the engine has no Steam service, so every steam_api.dll call takes
// its failure path and only the log line is observable.
void steam_failure(const char *message)
{
    std::fprintf(stderr, "%s\n", message);
}

// 0x4e4c80 Steam.initAPI()I
bool steam_init_api(Vm &, const Args &, Value *out, std::string *)
{
    // INVENTED: steam_init() (0x564af0) always fails without the Steam service.
    steam_failure("vm_steam_init_api: Failed to initialize Steam API");
    *out = Value::make_int(0);
    return true;
}

// 0x4e4d40 Steam.neutralize()V (raw bytes c6 05 df 75 7a 00 01 c3)
bool steam_neutralize(Vm &, const Args &, Value *out, std::string *)
{
    steam_neutralized = true;  // *(uint8_t*)0x7a75df = 1
    *out = Value::make_null();
    return true;
}

// 0x564ac0: pure DLC-id -> directory mapping.
const char *steam_package_dir(int32_t id)
{
    if (id < 0) return "Error! Invalid Steam package ID!";
    if (id == 0x7d5dc) return "dlc\\redline_cars_remake_pack\\";
    return "Invalid DLC ID!";
}

// 0x4e4c10 Steam.getPackage(I)Ljava.lang.String;
bool steam_get_package(Vm &, const Args &a, Value *out, std::string *err)
{
    const char *who = "Steam.getPackage";
    if (!want(who, a, 1, err)) return false;
    int32_t id;
    if (!get_i(who, a[0], &id, err)) return false;
    // 0x564b10 only gates a second call to the same mapping; no Steam call needed.
    // The exe also sets the remap flag DAT_007a75e4; ParseOptions::steam_version_remap
    // already defaults to true.
    *out = Value::make_str(steam_package_dir(id));
    return true;
}

// 0x4e4e00 Steam.setAchievement(Ljava.lang.String;)V
bool steam_set_achievement(Vm &, const Args &a, Value *out, std::string *err)
{
    const char *who = "Steam.setAchievement";
    if (!want(who, a, 1, err)) return false;
    const std::string *name;
    if (!get_s(who, a[0], &name, err)) return false;
    (void)name;
    // INVENTED: 0x564b50 (SetAchievement + StoreStats) always fails.
    steam_failure("vm_steam_set_achievement: Failed to set Steam achievement");
    *out = Value::make_null();
    return true;
}

// 0x4e4d50 Steam.resetAchievements()V
bool steam_reset_achievements(Vm &, const Args &, Value *out, std::string *)
{
    // INVENTED: 0x564b30 (ResetAllStats) always fails.
    steam_failure("vm_steam_reset_achievements: Failed to reset Steam achievements");
    *out = Value::make_null();
    return true;
}

// ---- java.lang.Debug / java.util.Replay -----------------------------------

bool file_path(const char *who, const std::string &in, std::string *out,
               std::string *err);

// 0x544fa0: append one line to error.log, CRLF-terminated. The first call in
// the process writes a "---\r\n" header.
// shortcut: exe never closes its 64 write handles, upgrade if exact slot exhaustion matters.
void error_log(const char *text)
{
    static bool first = true;
    FILE *f = std::fopen("error.log", "ab");
    if (!f) return;
    if (first) {
        first = false;
        std::fwrite("---\r\n", 1, 5, f);
    }
    std::fwrite(text, 1, strlen(text), f);
    std::fwrite("\r\n", 1, 2, f);
    std::fclose(f);
}

// 0x544e80: shared by errorEX (code 0) and giveWarning (code 9999). Texts other
// than a null one go to error.log; a message box follows unless code > 9999, and
// codes below 9999 exit the process.
void debug_sink(int code, const char *text)
{
    if (!text) text = "Fatal Error";
    else {
        error_log(text);
        if (code > 9999) return;
    }
    // INVENTED: no dialog service; the exe shows MessageBoxA(..., MB_ICONERROR).
    std::fprintf(stderr, "%s\n", text);
    if (code < 9999) std::exit(0);  // exe: _exit(code)
}

// 0x4d9350 Debug.errorEX(Ljava.lang.String;)V
bool debug_error_ex(Vm &, const Args &a, Value *out, std::string *err)
{
    const std::string *msg;
    if (!want("Debug.errorEX", a, 1, err) || !get_s("Debug.errorEX", a[0], &msg, err))
        return false;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", msg->c_str());
    debug_sink(0, buf);
    *out = Value::make_null();
    return true;
}

// 0x4d9380 Debug.giveWarning(Ljava.lang.String;)V
bool debug_give_warning(Vm &, const Args &a, Value *out, std::string *err)
{
    const std::string *msg;
    if (!want("Debug.giveWarning", a, 1, err) ||
        !get_s("Debug.giveWarning", a[0], &msg, err))
        return false;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", msg->c_str());
    debug_sink(9999, buf);
    *out = Value::make_null();
    return true;
}

// 0x4e30d0 Debug.writeFile / Replay.writeFile(String, String): arg0 is a printf
// format with arg1 as its single string vararg, then <buffer> + CRLF is appended
// to the named file (0x544ee0).
bool debug_write_file(Vm &, const Args &a, Value *out, std::string *err)
{
    const std::string *path, *text;
    if (!want("writeFile", a, 2, err) || !get_s("writeFile", a[0], &path, err) ||
        !get_s("writeFile", a[1], &text, err))
        return false;
    char buf[256];
    // shortcut: %d/%x/%f in the format string are not type-correct in C++, the exe reads the same stack slot as that type.
    snprintf(buf, sizeof buf, path->c_str(), text->c_str());
    FILE *f = std::fopen(buf, "ab");
    if (f) {
        std::fwrite(text->data(), 1, text->size(), f);
        std::fwrite("\r\n", 1, 2, f);
        std::fclose(f);
    }
    *out = Value::make_null();
    return true;
}

// ASCII case-insensitive equality, like CRTDLL _stricmp (0x57c167).
bool str_ieq(const std::string &a, const char *b)
{
    size_t i = 0;
    for (; i < a.size() && b[i]; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
    }
    return i == a.size() && !b[i];
}

// strtok(3) over one line with the exe's delimiter set (0x528350).
bool next_token(const std::string &line, size_t *pos, std::string *tok)
{
    static const char delims[] = " ,\t\n\r";
    size_t i = *pos;
    while (i < line.size() && strchr(delims, line[i])) i++;
    if (i >= line.size()) {
        *pos = i;
        return false;
    }
    size_t start = i;
    while (i < line.size() && !strchr(delims, line[i])) i++;
    tok->assign(line, start, i - start);
    *pos = i;
    return true;
}

// 0x4e2f00 Replay.readFile(int index, String key, String path): float stored
// under key for the frame whose "kf" is index + 1, read from a .rep file.
bool replay_read_file(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t index;
    const std::string *key, *path;
    if (!want("Replay.readFile", a, 3, err) ||
        !get_i("Replay.readFile", a[0], &index, err) ||
        !get_s("Replay.readFile", a[1], &key, err) ||
        !get_s("Replay.readFile", a[2], &path, err))
        return false;
    std::string p;
    if (!file_path("Replay.readFile", *path, &p, err)) return false;
    std::vector<uint8_t> data;
    std::string rerr;
    float result = 0.0f;
    if (!fs::read_file(p, &data, &rerr)) {
        *out = Value::make_float(result);  // exe returns 0.0 when open fails
        return true;
    }

    bool in_replay = false;
    int kf = 0;         // INVENTED: exe stack local is uninitialized
    float value = 0.0f; // INVENTED: exe stack local is uninitialized
    // shortcut: in-memory lines, exe reads with a fixed 1024-byte buffer and no bounds check.
    size_t pos = 0;
    while (pos < data.size()) {
        size_t nl = pos;
        while (nl < data.size() && data[nl] != '\n') nl++;
        std::string line(reinterpret_cast<const char *>(&data[pos]), nl - pos);
        pos = nl < data.size() ? nl + 1 : nl;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            if (!in_replay) continue;
        } else if (!in_replay) {
            if (line[0] == ';' || line[0] != '[' ||
                line.find("[replay]") == std::string::npos)
                continue;
            in_replay = true;
        }
        size_t tp = 0;
        std::string tok;
        // INVENTED: a line of only delimiters yields no token; the exe would
        // hand a null pointer to _stricmp.
        if (!next_token(line, &tp, &tok)) continue;
        if (str_ieq(tok, "kf")) {
            std::string v;
            if (next_token(line, &tp, &v)) sscanf(v.c_str(), "%d", &kf);
        } else if (str_ieq(tok, key->c_str()) && index == kf - 1) {
            std::string v;
            if (next_token(line, &tp, &v)) sscanf(v.c_str(), "%f", &value);
            // INVENTED: a missing value leaves the exe's uninitialized local;
            // keep the previous value instead.
            in_replay = false;
            result = value;
        }
    }
    *out = Value::make_float(result);
    return true;
}

// 0x4e55b0 runs the JVM collector; this VM has no managed heap to collect.
bool system_gc(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_int(0);
    return true;
}

bool system_sim_time(Vm &vm, const Args &, Value *out, std::string *)
{
    // 0x4e5810 returns the world simulation time; sleep advances the same clock.
    *out = Value::make_float((float)sched(vm).sim_seconds);
    return true;
}

// 0x4e5880: returns the previous warp factor and applies a non-negative one.
bool system_time_warp(Vm &vm, const Args &a, Value *out, std::string *err)
{
    float warp = 0.0f;
    if (!a.empty() && !get_f("System.timeWarp", a[0], &warp, err)) return false;
    Scheduler &s = sched(vm);
    float previous = s.warp;
    if (warp >= 0.0f) s.warp = warp;
    *out = Value::make_float(previous);
    return true;
}

// 0x4e5550: raises the engine quit request flag.
bool system_exit(Vm &vm, const Args &, Value *out, std::string *)
{
    sched(vm).quit_request = 1;
    *out = Value::make_null();
    return true;
}

// 0x4e57b0: metre scale and its per-hour / inverse-10 derivatives.
bool system_set_measure(Vm &vm, const Args &a, Value *out, std::string *err)
{
    const char *who = "System.setMeasure";
    float m = 1000.0f;  // exe default when the argument is missing
    if (!a.empty() && !get_f(who, a[0], &m, err)) return false;
    Scheduler &s = sched(vm);
    s.measure = m;                            // 0x5da524
    s.measure_per_hour = m * 0.00027777778f;  // 0x5fe378 (m / 3600)
    s.measure_inv10 = 10.0f / m;              // 0x5fe374
    *out = Value::make_null();
    return true;
}

// 0x4e5820: seconds within the day and the night flag at 0x5da080.
bool system_sync_game_time(Vm &vm, const Args &a, Value *out, std::string *err)
{
    const char *who = "System.syncGameTime";
    float t;
    if (!want(who, a, 1, err) || !get_f(who, a[0], &t, err)) return false;
    Scheduler &s = sched(vm);
    int32_t sec = (int32_t)t % 86400;  // C truncation and remainder
    s.day_seconds = sec;               // 0x6024c0
    uint32_t u = (uint32_t)sec;
    s.time_flag_5da080 = (u - 3600u) > 0x10b2fu || (u - 18000u) < 0x1c20u;
    *out = Value::make_null();
    return true;
}

// 0x4e56b0: priority 0 loads in 10% slices, anything else runs unthrottled.
bool system_set_ld_priority(Vm &vm, const Args &a, Value *out, std::string *err)
{
    const char *who = "System.setLdPriority";
    int32_t p;
    if (!want(who, a, 1, err) || !get_i(who, a[0], &p, err)) return false;
    Scheduler &s = sched(vm);
    if (p == 0) {
        s.load_slice = 0.1f;   // 0x5da464
        s.load_priority = 0;   // 0x5fe3bc
    } else {
        s.load_slice = -1.0f;  // 0x5da464
        s.load_priority = 1;   // 0x5fe3bc
    }
    *out = Value::make_null();
    return true;
}

// 0x4e5740: process working set in KB.
bool system_mem_usage(Vm &, const Args &, Value *out, std::string *)
{
    uint32_t kb = 0;
#ifdef _WIN32
    // psapi.dll is not in the portable link line; resolve it at runtime.
    HMODULE lib = LoadLibraryA("psapi.dll");
    if (lib) {
        typedef BOOL(WINAPI * GetInfo)(HANDLE, PROCESS_MEMORY_COUNTERS *, DWORD);
        GetInfo info = (GetInfo)GetProcAddress(lib, "GetProcessMemoryInfo");
        PROCESS_MEMORY_COUNTERS counters = {};
        counters.cb = sizeof counters;
        if (info && info(GetCurrentProcess(), &counters, sizeof counters))
            kb = (uint32_t)(counters.WorkingSetSize >> 10);
        FreeLibrary(lib);
    }
#else
    // VmRSS is the resident set in kB, the closest match to WorkingSetSize.
    FILE *f = std::fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            unsigned long pages = 0;
            if (std::sscanf(line, "VmRSS: %lu kB", &pages) == 1) {
                kb = (uint32_t)pages;
                break;
            }
        }
        std::fclose(f);
    }
#endif
    *out = Value::make_int((int32_t)kb);
    return true;
}

// 0x4e55c0: jumps to 0x47c240, which reloads the gameplay settings into engine globals.
bool system_get_config_options(Vm &, const Args &, Value *out, std::string *)
{
    // shortcut: no ConfigData store / renderer settings service in src; the exe
    // reloads gameplay settings through 0x47c240 (int/float/has-key reads into
    // ~40 globals), then re-creates two gfx resources at 0x422230.
    *out = Value::make_null();
    return true;
}

// 0x4e5690: return *(int *)(*(int **)0x5db008 + 0x16d854); (raw bytes a1 08 b0 5d 00
// 8b 80 54 d8 16 00 c3).
bool system_is_loading(Vm &, const Args &, Value *out, std::string *)
{
    // shortcut: no async resource loader in src; the exe reads the per-type loading
    // flag at [[0x5db008]+0x16d854], 0 until the streamer sets it.
    *out = Value::make_int(0);
    return true;
}

// 0x4e56a0: jmp 0x5191b0 with ecx = resource manager (unused by the target).
bool system_is_loading_reset(Vm &, const Args &, Value *out, std::string *)
{
    // 0x5191b0: rate = *(int *)0x5db02c; for (i = 0; i < 32; i++)
    //   *(int *)(0x773a20 + 4 * i) = rate;  *(int *)0x773a1c = rate << 5;
    // shortcut: no streamer in src; the exe resets its per-frame load budget.
    *out = Value::make_null();
    return true;
}

// 0x4e55d0: counts / VM fields formatted with "%d" into a 0x100 byte buffer.
bool system_info(Vm &, const Args &a, Value *out, std::string *err)
{
    const char *who = "System.info";
    int32_t n;
    if (!want(who, a, 1, err) || !get_i(who, a[0], &n, err)) return false;
    char buf[256];
    switch (n) {
    case 0:  // shortcut: no T_List<Thread> service in src; exe counts VM+0x18.
    case 1:  // shortcut: no T_List<Thread> service in src; exe counts VM+0x1c.
    case 2:  // shortcut: no T_List<Instance> service in src; exe counts VM+0x10.
    case 3:  // UNRESOLVED: VM+0x24 writer not traced (ctor 0x553980 only zeroes it)
             // — exe raw: uVar1 = *(undefined4 *)(*param_1 + 0x24);
    case 4:  // UNRESOLVED: VM+0x28 writer not traced (ctor 0x553980 only zeroes it)
             // — exe raw: uVar1 = *(undefined4 *)(*param_1 + 0x28);
    case 5:  // 0x7a383c is 0 in the image
        snprintf(buf, sizeof buf, "%d", (int)0);
        *out = Value::make_str(buf);
        return true;
    default:  // the exe switch is unsigned, so negatives also land here
        // INVENTED: exe builds a String from an uninitialised stack buffer; return "".
        *out = Value::make_str("");
        return true;
    }
}

// ---- java.lang.Integer / Float --------------------------------------------

bool int_tostring(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t v;
    if (!want("Integer.toString", a, 1, err) ||
        !get_i("Integer.toString", a[0], &v, err))
        return false;
    char buf[1024];
    snprintf(buf, sizeof buf, "%d", (int)v);  // 0x4deb80
    *out = Value::make_str(buf);
    return true;
}

bool int_tohexstring(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t v;
    if (!want("Integer.toHexString", a, 1, err) ||
        !get_i("Integer.toHexString", a[0], &v, err))
        return false;
    char buf[1024];
    snprintf(buf, sizeof buf, "0x%08x", (unsigned)v);  // 0x4deb30
    *out = Value::make_str(buf);
    return true;
}

bool float_tostring(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    const std::string *fmt;
    if (!want("Float.toString", a, 2, err) || !get_f("Float.toString", a[0], &v, err) ||
        !get_s("Float.toString", a[1], &fmt, err))
        return false;
    char buf[1024];
    // 0x4da0e0 passes the script format string straight to sprintf. Format
    // errors are the script's problem, exactly like the original.
    snprintf(buf, sizeof buf, fmt->c_str(), (double)v);
    *out = Value::make_str(buf);
    return true;
}

// ---- java.lang.String ------------------------------------------------------

// 0x4e4f70 drops the native string handle's reference. Rebuild strings are
// owned values without a handle or refcount, so there is nothing to release.
bool string_finalize(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_null();
    return true;
}

bool string_length(Vm &, const Args &a, Value *out, std::string *err)
{
    const std::string *s;
    if (!want("String.length", a, 1, err) ||
        !get_s("String.length", a[0], &s, err))
        return false;
    *out = Value::make_int((int32_t)s->size());  // 0x4e5110: strlen
    return true;
}

bool string_intvalue(Vm &, const Args &a, Value *out, std::string *err)
{
    const std::string *s;
    if (!want("String.intValue", a, 1, err) ||
        !get_s("String.intValue", a[0], &s, err))
        return false;
    int v = 0;
    // shortcut: the exe leaves the stack slot uninitialized on parse failure;
    // zero is deterministic here.
    sscanf(s->c_str(), "%d", &v);  // 0x4e50d0 via sscanf
    *out = Value::make_int(v);
    return true;
}

bool string_floatvalue(Vm &, const Args &a, Value *out, std::string *err)
{
    const std::string *s;
    if (!want("String.floatValue", a, 1, err) ||
        !get_s("String.floatValue", a[0], &s, err))
        return false;
    float v = 0.0f;
    sscanf(s->c_str(), "%f", &v);  // 0x4e4fc0 via sscanf
    *out = Value::make_float(v);
    return true;
}

bool string_append(Vm &, const Args &a, Value *out, std::string *err)
{
    const std::string *s;
    int32_t ascii;
    if (!want("String.append", a, 2, err) ||
        !get_s("String.append", a[0], &s, err) ||
        !get_i("String.append", a[1], &ascii, err))
        return false;
    std::string r = *s;
    if (r.size() > 255) r.resize(255);  // 0x4e4ed0 copies into char[256]
    if (ascii == 8) {
        if (!r.empty()) r.pop_back();
    } else if (ascii > 0x1f && r.size() < 255) {
        r.push_back((char)ascii);
    }
    *out = Value::make_str(r);
    return true;
}

bool string_token(Vm &, const Args &a, Value *out, std::string *err)
{
    const std::string *s, *delims;
    int32_t n;
    if (!want("String.token", a, 3, err) || !get_s("String.token", a[0], &s, err) ||
        !get_i("String.token", a[1], &n, err) || !get_s("String.token", a[2], &delims, err))
        return false;
    std::string copy = *s;
    char *save = nullptr;
    char *tok = nullptr;
    char *p = copy.data();
    if (n < 0) {
        // Negative counts exhaust the string in the exe's loop and return null.
        while (strtok_r(p, delims->c_str(), &save)) p = nullptr;
    } else {
        for (int32_t i = 0; i <= n; i++) {
            tok = strtok_r(p, delims->c_str(), &save);
            p = nullptr;
            if (!tok) break;
        }
    }
    *out = tok ? Value::make_str(tok) : Value::make_null();
    return true;
}

bool string_getascii(Vm &vm, const Args &a, Value *out, std::string *err)
{
    const std::string *s;
    if (!want("String.getASCII", a, 1, err) ||
        !get_s("String.getASCII", a[0], &s, err))
        return false;
    Array *arr = vm.new_array("I", s->size());
    for (size_t i = 0; i < s->size(); i++)
        arr->elems[i] = Value::make_int((int32_t)(signed char)(*s)[i]);  // char is signed
    *out = Value::make_array(arr);
    return true;
}

// ---- Vector3 / Ypr ---------------------------------------------------------

bool read_f(Vm &vm, Object *o, const char *name, float *out, std::string *err)
{
    Value v;
    if (!vm.get_instance_field(o, name, &v, err)) return false;
    return get_f(name, v, out, err);
}

bool write_f(Vm &vm, Object *o, const char *name, float value, std::string *err)
{
    return vm.set_instance_field(o, name, Value::make_float(value), err);
}

bool vec_read(Vm &vm, Object *o, float v[3], std::string *err)
{
    return read_f(vm, o, "x", &v[0], err) && read_f(vm, o, "y", &v[1], err) &&
           read_f(vm, o, "z", &v[2], err);
}

bool vec_write(Vm &vm, Object *o, const float v[3], std::string *err)
{
    return write_f(vm, o, "x", v[0], err) && write_f(vm, o, "y", v[1], err) &&
           write_f(vm, o, "z", v[2], err);
}

bool ypr_read(Vm &vm, Object *o, float v[3], std::string *err)
{
    return read_f(vm, o, "y", &v[0], err) && read_f(vm, o, "p", &v[1], err) &&
           read_f(vm, o, "r", &v[2], err);
}

bool ypr_write(Vm &vm, Object *o, const float v[3], std::string *err)
{
    return write_f(vm, o, "y", v[0], err) && write_f(vm, o, "p", v[1], err) &&
           write_f(vm, o, "r", v[2], err);
}

// 0x569970 Math__matToYpr (shared with the car damage strings).
void mat_to_ypr(const Mat34 &m, float out[3])
{
    game::mat_to_ypr(m, out);
}

bool vec_length(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    float v[3];
    if (!want("Vector3.length", a, 1, err) ||
        !get_o("Vector3.length", a[0], &o, err) || !vec_read(vm, o, v, err))
        return false;
    *out = Value::make_float(sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
    return true;
}

bool vec_distance(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o, *p;
    float v[3], w[3];
    if (!want("Vector3.distance", a, 2, err) ||
        !get_o("Vector3.distance", a[0], &o, err) ||
        !get_o("Vector3.distance", a[1], &p, err) || !vec_read(vm, o, v, err) ||
        !vec_read(vm, p, w, err))
        return false;
    float dx = w[0] - v[0], dy = w[1] - v[1], dz = w[2] - v[2];
    *out = Value::make_float(sqrtf(dx * dx + dy * dy + dz * dz));
    return true;
}

bool vec_normalize(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    float v[3];
    if (!want("Vector3.normalize", a, 1, err) ||
        !get_o("Vector3.normalize", a[0], &o, err) || !vec_read(vm, o, v, err))
        return false;
    float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n != 0.0f) {
        float k = 1.0f / n;
        v[0] *= k;
        v[1] *= k;
        v[2] *= k;
        if (!vec_write(vm, o, v, err)) return false;
    }
    *out = Value::make_null();
    return true;
}

bool vec_rotate_ypr(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o, *yo;
    float v[3], ang[3];
    if (!want("Vector3.rotate", a, 2, err) ||
        !get_o("Vector3.rotate", a[0], &o, err) ||
        !get_o("Vector3.rotate", a[1], &yo, err) || !vec_read(vm, o, v, err) ||
        !ypr_read(vm, yo, ang, err))
        return false;
    Mat34 m;
    euler_to_mat(&m, ang);
    float nx = m.m[0] * v[0] + m.m[1] * v[1] + m.m[2] * v[2];
    float ny = m.m[4] * v[0] + m.m[5] * v[1] + m.m[6] * v[2];
    float nz = m.m[8] * v[0] + m.m[9] * v[1] + m.m[10] * v[2];
    float nv[3] = {nx, ny, nz};
    if (!vec_write(vm, o, nv, err)) return false;
    *out = a[0];  // 0x4e67e0 returns the receiver handle
    return true;
}

bool vec_rotate_axis(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o, *ao;
    float v[3], axis[3], angle;
    if (!want("Vector3.rotate", a, 3, err) ||
        !get_o("Vector3.rotate", a[0], &o, err) ||
        !get_o("Vector3.rotate", a[1], &ao, err) ||
        !get_f("Vector3.rotate", a[2], &angle, err) || !vec_read(vm, o, v, err) ||
        !vec_read(vm, ao, axis, err))
        return false;
    float ax = axis[0], ay = axis[1], az = axis[2];
    float norm2 = ax * ax + ay * ay + az * az;
    if (norm2 < 1e-6f) {
        // shortcut: 0x4e6650 multiplies by an uninitialized matrix for a zero
        // axis; leave the vector unchanged instead.
        *out = a[0];
        return true;
    }
    if (fabsf(norm2 - 1.0f) > 1e-6f) {
        float k = 1.0f / sqrtf(norm2);
        ax *= k;
        ay *= k;
        az *= k;
    }
    float c = cosf(angle), s = sinf(angle), t = 1.0f - c;
    Mat34 m;
    m.m[0] = t * ax * ax + c;
    m.m[1] = t * ax * ay - s * az;
    m.m[2] = t * ax * az + s * ay;
    m.m[4] = t * ax * ay + s * az;
    m.m[5] = t * ay * ay + c;
    m.m[6] = t * ay * az - s * ax;
    m.m[8] = t * ax * az - s * ay;
    m.m[9] = t * ay * az + s * ax;
    m.m[10] = t * az * az + c;
    float nx = m.m[0] * v[0] + m.m[1] * v[1] + m.m[2] * v[2];
    float ny = m.m[4] * v[0] + m.m[5] * v[1] + m.m[6] * v[2];
    float nz = m.m[8] * v[0] + m.m[9] * v[1] + m.m[10] * v[2];
    float nv[3] = {nx, ny, nz};
    if (!vec_write(vm, o, nv, err)) return false;
    *out = a[0];
    return true;
}

bool vec_ctor_ypr(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o, *yo;
    float ang[3];
    if (!want("Vector3.<init>", a, 2, err) ||
        !get_o("Vector3.<init>", a[0], &o, err) ||
        !get_o("Vector3.<init>", a[1], &yo, err) || !ypr_read(vm, yo, ang, err))
        return false;
    Mat34 m;
    euler_to_mat(&m, ang);
    float v[3] = {-m.m[2], -m.m[6], -m.m[10]};  // 0x4e6380
    if (!vec_write(vm, o, v, err)) return false;
    *out = Value::make_null();
    return true;
}

bool ypr_ctor_vector(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o, *vo;
    float v[3], ypr[3];
    if (!want("Ypr.<init>", a, 2, err) ||
        !get_o("Ypr.<init>", a[0], &o, err) ||
        !get_o("Ypr.<init>", a[1], &vo, err) || !vec_read(vm, vo, v, err))
        return false;
    if (v[0] == 0.0f && v[2] == 0.0f) {
        // 0x4e7170 -> 0x569a60: y == 0 takes the negative branch (pitch -pi/2).
        ypr[0] = 0.0f;
        ypr[1] = v[1] > 0.0f ? 1.5707964f : -1.5707964f;
        ypr[2] = 0.0f;
    } else {
        ypr[0] = atan2f(-v[0], -v[2]);
        ypr[1] = atan2f(v[1], sqrtf(v[0] * v[0] + v[2] * v[2]));
        ypr[2] = 0.0f;
    }
    if (!ypr_write(vm, o, ypr, err)) return false;
    *out = Value::make_null();
    return true;
}

bool ypr_mul(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o, *po;
    float left[3], right[3];
    if (!want("Ypr.mul", a, 2, err) || !get_o("Ypr.mul", a[0], &o, err) ||
        !get_o("Ypr.mul", a[1], &po, err) || !ypr_read(vm, o, left, err) ||
        !ypr_read(vm, po, right, err))
        return false;
    Mat34 A, B;
    euler_to_mat(&A, left);
    euler_to_mat(&B, right);
    // 0x566a70 product, in-place on A (column-major 3x3 layout).
    const float *b = B.m;
    float r[12];
    r[0] = A.m[0] * b[0] + A.m[4] * b[1] + A.m[8] * b[2];
    r[1] = A.m[1] * b[0] + A.m[5] * b[1] + A.m[9] * b[2];
    r[2] = A.m[2] * b[0] + A.m[6] * b[1] + A.m[10] * b[2];
    r[4] = b[4] * A.m[0] + b[5] * A.m[4] + b[6] * A.m[8];
    r[5] = b[4] * A.m[1] + b[5] * A.m[5] + b[6] * A.m[9];
    r[6] = b[4] * A.m[2] + b[5] * A.m[6] + b[6] * A.m[10];
    r[8] = b[8] * A.m[0] + b[9] * A.m[4] + b[10] * A.m[8];
    r[9] = b[8] * A.m[1] + b[9] * A.m[5] + b[10] * A.m[9];
    r[10] = b[8] * A.m[2] + b[9] * A.m[6] + b[10] * A.m[10];
    r[3] = r[7] = r[11] = 0.0f;
    Mat34 M;
    memcpy(M.m, r, sizeof M.m);
    float ypr[3];
    mat_to_ypr(M, ypr);
    if (!ypr_write(vm, o, ypr, err)) return false;
    *out = Value::make_null();
    return true;
}

// ---- java.lang.Object ------------------------------------------------------

// 0x4dfb60/0x4dfb80 relink the instance between GC lists in the exe VM.
// Rebuild objects are arena-owned and GC-less; no script-visible values change.
bool object_gc_toggle(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_null();
    return true;
}

bool object_hashcode(Vm &, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("Object.hashCode", a, 1, err) ||
        !get_o("Object.hashCode", a[0], &o, err))
        return false;
    *out = Value::make_int((int32_t)(intptr_t)o);  // 0x4dfc50 returns the handle
    return true;
}

// 0x5629a0 Thread__notify (ECX = thread): clear the waiting flag and make the
// thread runnable again.
void monitor_notify_thread(ScriptThreadState *t)
{
    // INVENTED: exe flag 0x80 (dead) is modelled by our `done`.
    if (t->done) std::fprintf(stderr, "Thread::notify: thread already died\n");
    if (!t->waiting) std::fprintf(stderr, "Thread::notify: not waiting\n");
    t->waiting = false;
    // shortcut: exe skips the run-list relink when thread+0x2c bit 0x20 is set
    // (meaning UNRESOLVED); run_threads already gates suspended/done threads.
}

// 0x4e0440 Object.wait()V -> 0x54d430(monitor, ctx+4 thread) -> 0x564950.
bool object_wait(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("Object.wait", a, 1, err) ||
        !get_o("Object.wait", a[0], &o, err))
        return false;
    *out = Value::make_null();
    Scheduler &s = sched(vm);
    ScriptThreadState *t = s.current;
    // shortcut: a native invoked synchronously (Vm::invoke / run_field_init,
    // e.g. a static initializer or an engine callback) has no executing script
    // thread in src; the exe always has ctx+4. No-op, out stays null (V).
    if (!t) return true;
    // INVENTED: 0x80 (dead) maps to our `done`.
    if (t->done) std::fprintf(stderr, "Thread::wait: thread already died\n");
    if (t->waiting) std::fprintf(stderr, "Thread::wait: already waiting\n");
    s.monitors[o].push_back(t);  // monitor+0x14 tail append
    t->waiting = true;           // thread+0x2c |= 0x10, off the run list
    t->wait_on = o;              // +0x34: Thread__stop notifies this monitor
    t->runner->request_stop();   // 0x563290: leave the slice after this instruction
    return true;
}

// 0x5629a0 0x54d320 Object.notify()V: wake the last waiter of this object.
bool object_notify(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("Object.notify", a, 1, err) ||
        !get_o("Object.notify", a[0], &o, err))
        return false;
    *out = Value::make_null();
    Scheduler &s = sched(vm);
    auto it = s.monitors.find(o);
    if (it == s.monitors.end() || it->second.empty()) return true;
    ScriptThreadState *t = it->second.back();  // tail pop, LIFO
    it->second.pop_back();
    monitor_notify_thread(t);
    if (it->second.empty()) s.monitors.erase(it);
    return true;
}

// 0x54d3a0 Object.notifyAll()V: drain the object's wait list, tail to head.
bool object_notify_all(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("Object.notifyAll", a, 1, err) ||
        !get_o("Object.notifyAll", a[0], &o, err))
        return false;
    *out = Value::make_null();
    natives::monitor_notify_all(vm, o);
    return true;
}

// 0x549ff0 ClassRecord__name. The instance's ClassState is the exe's class
// record; its declaring unit holds the name constant.
std::string object_class_name(Object *o)
{
    const ClassUnit *unit = class_state_unit(o ? o->cls : nullptr);
    return unit ? unit->class_name(unit->self_class) : std::string();
}

// 0x4e03f0: "ClassName<hex handle>" in a 256-byte buffer; the handle is the
// same instance payload Object.hashCode returns.
bool object_tostring(Vm &, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("Object.toString", a, 1, err) ||
        !get_o("Object.toString", a[0], &o, err))
        return false;
    char buf[256];
    snprintf(buf, sizeof buf, "%s<%0x>", object_class_name(o).c_str(),
             (uint32_t)(uintptr_t)o);
    *out = Value::make_str(buf);
    return true;
}

// 0x4dfba0: one "!JVM ERROR:: <class> with message: <msg>" line through the
// same engine log sink as System.log (0x5695c0).
bool object_error_vm(Vm &, const Args &a, Value *out, std::string *err)
{
    const char *who = "Object.errorVM";
    Object *o;
    const std::string *message;
    if (!want(who, a, 2, err) || !get_o(who, a[0], &o, err) ||
        !get_s(who, a[1], &message, err))
        return false;
    std::fprintf(stderr, "!JVM ERROR:: %s with message: %s\n",
                 object_class_name(o).c_str(), message->c_str());
    *out = Value::make_null();
    return true;
}


// ---- java.util.resource.ResourceRef / GameRef / java.lang.GameType ---------

Runtime *game_rt()
{
    return Runtime::active();
}

bool need_rt(const char *who, std::string *err)
{
    if (game_rt()) return true;
    *err = std::string(who) + ": no active game runtime";
    return false;
}

Runtime::Handle *value_handle(const Value &v)
{
    Runtime *rt = game_rt();
    if (!rt || v.kind != Value::Obj || !v.obj) return nullptr;
    return rt->handle(v.obj);
}

Runtime::Resource *value_resource(const Value &v)
{
    Runtime::Handle *h = value_handle(v);
    return h ? h->target : nullptr;
}

// ---- java.lang.Object.runMethod --------------------------------------------

// The exe returns the method's raw 32-bit result slot.
static int32_t call_result_i(const Value &v)
{
    switch (v.kind) {
    case Value::Int: return v.i;
    case Value::Float: {
        int32_t b;
        memcpy(&b, &v.f, sizeof b);
        return b;
    }
    case Value::Obj: return (int32_t)(intptr_t)v.obj;  // 0x47dd70: piVar4[2]
    default: return 0;  // INVENTED: String/array slots have no rebuild equivalent
    }
}

// 0x4dff40 / 0x4dfda0 / 0x4e00d0 / 0x4e0260: run a script method on the
// receiver's resource instance. argi is the argument index, -1 for the void
// overload.
static bool object_run_method(const char *who, const Args &a, int argi, Value *out,
                              std::string *err)
{
    if (!want(who, a, argi >= 0 ? (size_t)argi + 1 : 2u, err)) return false;
    Object *o;
    const std::string *name;
    if (!get_o(who, a[0], &o, err) || !get_s(who, a[1], &name, err)) return false;
    Runtime *rt = game_rt();
    Runtime::Handle *h = rt ? rt->handle(o) : nullptr;  // 0x4ecc70
    *out = Value::make_int(0);
    if (!h) {
        // INVENTED: the exe appends the handle, the frame's file and line; a
        // NativeFn has no frame, so only the prefix is logged.
        std::fprintf(stderr, "%s: scripted resource not found\n", who);
        return true;
    }
    Object *self = rt->script_instance(h->target);  // typed +0x50
    if (!self) return true;
    std::vector<Value> args;
    if (argi >= 0) args.push_back(a[argi]);
    Value rv;
    std::string e;
    if (!rt->call_method(self, *name, args, &rv, &e)) return true;  // 0x47dd70
    *out = Value::make_int(call_result_i(rv));
    return true;
}

bool object_runmethod_int(Vm &, const Args &a, Value *out, std::string *err)
{
    return object_run_method("vm_object_runmethod_int", a, 2, out, err);  // 0x4dff40
}

bool object_runmethod_float(Vm &, const Args &a, Value *out, std::string *err)
{
    return object_run_method("vm_object_runmethod_float", a, 2, out, err);  // 0x4dfda0
}

bool object_runmethod_ref(Vm &, const Args &a, Value *out, std::string *err)
{
    return object_run_method("vm_object_runmethod_ref", a, 2, out, err);  // 0x4e00d0
}

bool object_runmethod_void(Vm &, const Args &a, Value *out, std::string *err)
{
    return object_run_method("vm_object_runmethod_void", a, -1, out, err);  // 0x4e0260
}

bool receiver_object(const char *who, const Args &a, Object **out, std::string *err)
{
    if (!want(who, a, 1, err)) return false;
    return get_o(who, a[0], out, err);
}

std::string value_string(const Args &a, size_t i)
{
    return i < a.size() && a[i].kind == Value::Str ? a[i].s : std::string();
}

// Wrapper objects for getParent/getFirstChild and friends. The exe returns
// null when the field is null and logs when the handle itself is missing.
bool wrap_resource(Runtime::Resource *r, Value *out, std::string *err)
{
    *out = Value::make_null();
    if (!r) return true;
    Object *o = game_rt()->wrap(r, err);
    if (!o) {
        if (err->empty()) *err = "resource wrapper creation failed";
        return false;
    }
    *out = Value::make_obj(o);
    return true;
}

Object *new_point(Vm &vm, const char *cls, const float v[3], bool ypr, std::string *err)
{
    ClassState *cs = vm.find_class(cls);
    if (!cs) {
        *err = std::string(cls) + " is not loaded";
        return nullptr;
    }
    Object *o = vm.new_object(cs, err);
    if (!o) return nullptr;
    bool ok = ypr ? ypr_write(vm, o, v, err) : vec_write(vm, o, v, err);
    return ok ? o : nullptr;
}

bool read_vec_arg(Vm &vm, const char *who, const Value &v, float out[3], std::string *err)
{
    out[0] = out[1] = out[2] = 0.0f;
    if (v.kind == Value::Null) return true;  // setMatrix/setState zero defaults
    Object *o;
    if (!get_o(who, v, &o, err)) return false;
    return vec_read(vm, o, out, err);
}

bool read_ypr_arg(Vm &vm, const char *who, const Value &v, float out[3], std::string *err)
{
    out[0] = out[1] = out[2] = 0.0f;
    if (v.kind == Value::Null) return true;
    Object *o;
    if (!get_o(who, v, &o, err)) return false;
    return ypr_read(vm, o, out, err);
}

// 0x4e4080: allocate the 0x10-byte ref node and attach it to the instance.
bool rr_new_native(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.newNative", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.newNative", a, &o, err)) return false;
    game_rt()->attach(o);
    *out = Value::make_null();
    return true;
}

// 0x4e3220: unlink from the RI handle list, free the node, detach.
bool rr_delete_native(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.deleteNative", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.deleteNative", a, &o, err)) return false;
    game_rt()->detach(o);
    *out = Value::make_null();
    return true;
}

// 0x4e3ff0 / 0x4e3f60 shared path. No log: the exe just returns.
static bool rr_make_resource(const char *who, uint32_t type, const char *name,
                             const Args &a, Value *out, std::string *err)
{
    if (!need_rt(who, err)) return false;
    Object *o;
    if (!receiver_object(who, a, &o, err) || !want(who, a, 3, err)) return false;
    Runtime::Handle *h = game_rt()->handle(o);        // 0x4ecc70
    Runtime::Handle *parent = value_handle(a[1]);     // getArgs L payload = ref node
    *out = Value::make_null();
    if (!h || !parent || a[2].kind != Value::Str) return true;   // null arg -> return
    const std::string text =
        "sourcefile " + a[2].s + "\r\nflags 0\r\n";               // 0x5a05c8 format
    Runtime::Resource *node =
        game_rt()->define_script_resource(type, name, text, parent->target);
    game_rt()->bind(h, node);                         // 0x40cac0
    return true;
}

bool rr_make_texture(Vm &, const Args &a, Value *out, std::string *err)   // 0x4e3ff0
{ return rr_make_resource("ResourceRef.makeTexture", 7, "_texture", a, out, err); }

bool rr_make_sound(Vm &, const Args &a, Value *out, std::string *err)     // 0x4e3f60
{ return rr_make_resource("ResourceRef.makeSound", 6, "_sfx", a, out, err); }

// 0x4e43a0 -> FUN_0051f9b0: unresolved ids clear the handle (the exe drops the
// old link, then only rebinds when FUN_005183a0 returns an instance).
bool rr_set_id(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.set", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.set", a, &o, err)) return false;
    int32_t id;
    if (!want("ResourceRef.set", a, 2, err) ||
        !get_i("ResourceRef.set", a[1], &id, err))
        return false;
    Runtime::Handle *h = game_rt()->handle(o);
    if (h) game_rt()->bind(h, (uint32_t)id);
    *out = Value::make_null();
    return true;
}

// 0x4e4350 -> FUN_0040cac0: copy the argument RI (id = RI+0x80); a null or
// unbound argument clears the receiver.
bool rr_set_ref(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.set", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.set", a, &o, err)) return false;
    if (!want("ResourceRef.set", a, 2, err)) return false;
    Runtime::Handle *h = game_rt()->handle(o);
    if (h) {
        Runtime::Handle *src = value_handle(a[1]);
        if (src)
            game_rt()->bind(h, src->target);
        else
            game_rt()->bind(h, (Runtime::Resource *)nullptr);
    }
    *out = Value::make_null();
    return true;
}

// 0x4e43e0: RI ? RI+0x7c : 0. A missing handle logs and returns 0.
bool rr_type(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.type", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.type", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    *out = Value::make_int(r ? (int32_t)r->type : 0);
    return true;
}

// 0x4e23b0: RI+0x7c type 3 (ins_render) -> render typed data +0x110, else 0.
bool rr_get_type_id(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("RenderRef.getTypeID", err)) return false;
    Object *o;
    if (!receiver_object("RenderRef.getTypeID", a, &o, err)) return false;
    // shortcut: for type 3 the exe returns render typed data +0x110
    // (FUN_00421610(0xa0000001)), not modeled; 0 is also its fetch-failure value.
    if (!value_handle(a[0])) {
        // INVENTED: the exe appends the id, file and line; a NativeFn has no frame.
        std::fprintf(stderr, "vm_renderref_gettype: scripted resource not found\n");
    }
    *out = Value::make_int(0);
    return true;
}

// 0x4e2830 -> 0x4ebd20: render typed data +0x17c (colour override).
bool rr_set_color(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("RenderRef.setColor", err)) return false;
    Object *o;
    if (!receiver_object("RenderRef.setColor", a, &o, err)) return false;
    int32_t color;
    if (!want("RenderRef.setColor", a, 2, err) ||
        !get_i("RenderRef.setColor", a[1], &color, err))
        return false;
    if (!value_handle(a[0])) {
        // INVENTED: the exe appends the id, file and line; a NativeFn has no frame.
        std::fprintf(stderr, "vm_renderref_setcolor: scripted resource not found\n");
    } else if (Runtime::Resource *r = value_resource(a[0])) {
        // 0x4ebd20: typed data +0x17c, the same slot Text__create 0x4e8f40 writes
        // (card 067's INVENTED mapping Resource::text_color).
        // shortcut: make-resident (vtbl+0x14) and the 0x51f4f0(0x80000001)
        // fetch are collapsed; src has no typed fetch that can fail.
        r->text_color = (uint32_t)color;
    }
    *out = Value::make_null();
    return true;
}

// 0x4e3cd0: ref node +8.
bool rr_id(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.id", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.id", a, &o, err)) return false;
    Runtime::Handle *h = game_rt()->handle(o);
    *out = Value::make_int(h ? (int32_t)h->id : 0);
    return true;
}

// 0x4e36b0: parent RI @ +0x24, id @ +0x80; the record super chain here.
bool rr_get_parent_id(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.getParentID", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.getParentID", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    *out = Value::make_int(r ? (int32_t)r->parent_id : 0);
    return true;
}

// 0x4e37c0/FUN_004ea740: wrap the parent RI; null when there is none.
bool rr_get_parent(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.getParent", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.getParent", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    return wrap_resource(r ? r->parent : nullptr, out, err);
}

// 0x4e3c00/FUN_004ea900: RI+0xfc world-tree root; null when the field is null.
bool rr_get_wt_root(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.getWTRoot", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.getWTRoot", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    return wrap_resource(r ? r->world_root : nullptr, out, err);
}

// 0x4e3510/FUN_004e9b30: first child RI @ +0x30.
bool rr_get_first_child(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.getFirstChild", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.getFirstChild", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    Runtime::Resource *c = r && !r->children.empty() ? r->children[0] : nullptr;
    return wrap_resource(c, out, err);
}

// 0x4e35e0/FUN_004ea610: next sibling RI @ +0x4.
bool rr_get_next_child(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("ResourceRef.getNextChild", err)) return false;
    Object *o;
    if (!receiver_object("ResourceRef.getNextChild", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    return wrap_resource(r ? r->next_sibling : nullptr, out, err);
}

// 0x4e3de0 ResourceRef.load()V
bool rr_load(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("ResourceRef.load", a, 1, err)) return false;
    *out = Value::make_null();
    if (!value_handle(a[0]))
        std::fprintf(stderr, "vm_resourceref_load: scripted resource not found\n");
    // shortcut: no ConfigData/streaming service in src. The exe ORs 0x80000 into
    // RI+0x84 only when GetConfigData("ground_precache") > 0 (0x4e3e0c), then
    // makes the instance resident (vtbl +0x14) and fetches typed data
    // (0x51f4f0 mask 0x80000001; on success vtbl +0xc). GetConfigData returns 0
    // when config data is absent (0x47c110), so the flag branch is not taken.
    return true;
}

// 0x4e4500 ResourceRef.unload()V
bool rr_unload(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("ResourceRef.unload", a, 1, err)) return false;
    *out = Value::make_null();
    Runtime::Handle *h = value_handle(a[0]);
    if (!h) {
        std::fprintf(stderr, "vm_resourceref_unload: scripted resource not found\n");
        return true;
    }
    // 0x4ec800: game instances go to the deferred unload queue (0x47e340);
    // every other kind unloads now (0x51a1b0 vf[+0x1c]).
    if (h->id && h->target) {
        if (h->target->type == 1)
            game_rt()->queue_unload(h);
        else
            game_rt()->unload(h->target);
    }
    return true;
}

// 0x4e3100 ResourceRef.cache()V
bool rr_cache(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("ResourceRef.cache", a, 1, err)) return false;
    *out = Value::make_null();
    if (!value_handle(a[0]))
        std::fprintf(stderr, "vm_resourceref_cache: scripted resource not found\n");
    // shortcut: 0x4e9830(h, 0, 1.0f) walks engine typed objects (0x468b00) and
    // its float result is discarded (FSTP ST0); no such service in src.
    return true;
}

// 0x4e40f0 ResourceRef.precache()V
bool rr_precache(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("ResourceRef.precache", a, 1, err)) return false;
    *out = Value::make_null();
    if (!value_handle(a[0]))
        std::fprintf(stderr, "vm_resourceref_precache: scripted resource not found\n");
    // shortcut: 0x4e9830(h, 0x3f800000 /*1.0f as int*/, 1.0f), result discarded.
    return true;
}

// 0x4e32b0 ResourceRef.destroy()V
bool rr_destroy(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("ResourceRef.destroy", a, 1, err)) return false;
    *out = Value::make_null();
    Runtime::Handle *h = value_handle(a[0]);
    // 0x4e9450 (ECX = h) requires both id (+0x8) and RI (+0xc); 0x47b970 then
    // appends the deferred delete node (drained by 0x47d070).
    if (h && h->id && h->target) game_rt()->queue_delete(h);
    return true;
}

// 0x4e32e0 ResourceRef.duplicate(ResourceRef source)V
bool rr_duplicate(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("ResourceRef.duplicate", a, 2, err)) return false;
    *out = Value::make_null();
    Runtime::Handle *h = value_handle(a[0]);
    if (!h || a[1].kind != Value::Obj || !a[1].obj) return true;
    Runtime::Resource *src = value_resource(a[1]);
    if (src) {
        uint32_t t = src->type;  // RI+0x7c
        if (t == 0xe || t == 0xd || t == 5 || t == 7) {
            // shortcut: 0x4e9470 clones these types through FUN_0051ee30;
            // src/ has no resource-clone service, so the receiver is unchanged.
            return true;
        }
    }
    // 0x4e9470 else branch -> 0x40cac0(src): null src clears the receiver.
    game_rt()->bind(h, src);
    return true;
}

// 0x4e4210 ResourceRef.scaleMesh(float x, float y, float z)V
bool rr_scale_mesh(Vm &, const Args &a, Value *out, std::string *err)
{
    float x, y, z;
    if (!want("ResourceRef.scaleMesh", a, 4, err) ||
        !get_f("ResourceRef.scaleMesh", a[1], &x, err) ||
        !get_f("ResourceRef.scaleMesh", a[2], &y, err) ||
        !get_f("ResourceRef.scaleMesh", a[3], &z, err))
        return false;
    (void)x;
    (void)y;
    (void)z;
    *out = Value::make_null();
    if (!value_handle(a[0])) {
        std::fprintf(stderr, "vm_resourceref_scalemesh: scripted resource not found\n");
        return true;
    }
    // shortcut: 0x4ebbf0 needs the instance typed data (vtbl +0xc / 0x51f4f0
    // mask 0x80000000) and applies the scale to render nodes via FUN_004d8710
    // (T+0x18, node vf +0x4c); no typed-data or node-list service in src/.
    return true;
}

// ---- java.util.resource.PhysicsRef ----------------------------------------

// 0x524620 creation info: null alias -> "_physinst".
static std::string alias_arg(const Value &v)
{
    return v.kind == Value::Str ? v.s : std::string("_physinst");
}

// 0x4e1470 PhysicsRef.create(ResourceRef parent, int typeRid, String alias)
bool ph_create(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("PhysicsRef.create", err)) return false;
    Object *o;
    if (!receiver_object("PhysicsRef.create", a, &o, err)) return false;
    int32_t type_rid;
    if (!want("PhysicsRef.create", a, 4, err) ||
        !get_i("PhysicsRef.create", a[2], &type_rid, err))
        return false;
    Runtime *rt = game_rt();
    Runtime::Handle *h = rt->handle(o);           // 0x4ecc70
    Runtime::Resource *parent = value_resource(a[1]);
    *out = Value::make_null();
    if (!h || !parent) return true;               // exe logs and returns
    Runtime::Resource *type = rt->resolve((uint32_t)type_rid);  // 0x5183a0
    // INVENTED: the exe faults on the null body (0x4f7b50 -> 0, 0x4e1563).
    if (!type) return true;
    const std::string alias = alias_arg(a[3]);
    Runtime::Resource *inst = rt->instantiate(type, parent, alias, "", false, err);
    if (!inst) return true;  // 0x518a40 no free slot: the exe faults (INVENTED)
    inst->type = 2;          // 0x524620 creation info: INSTANCE_PHYSICS
    inst->name = alias;
    rt->bind(h, inst);       // 0x4e8a50 first 0x40cac0
    // shortcut: rigid body, 0x218 physics object and typed+0xb4 link not modelled;
    // upgrade when the physics world exists (0x4f7830/0x4f7b50/0x4f7d60/0x524620).
    return true;
}

// 0x4e16a0 PhysicsRef.createBox(ResourceRef parent, F x, F y, F z, String alias)
bool ph_create_box(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("PhysicsRef.createBox", err)) return false;
    Object *o;
    if (!receiver_object("PhysicsRef.createBox", a, &o, err)) return false;
    float x, y, z;
    if (!want("PhysicsRef.createBox", a, 6, err) ||
        !get_f("PhysicsRef.createBox", a[2], &x, err) ||
        !get_f("PhysicsRef.createBox", a[3], &y, err) ||
        !get_f("PhysicsRef.createBox", a[4], &z, err))
        return false;
    (void)x;
    (void)y;
    (void)z;
    // 0x4f7d60: box half extents = x*0.5f, y*0.5f, z*0.5f; not stored (no physics).
    Runtime *rt = game_rt();
    Runtime::Handle *h = rt->handle(o);           // 0x4ecc70
    Runtime::Resource *parent = value_resource(a[1]);
    *out = Value::make_null();
    if (!h || !parent) return true;               // exe logs and returns
    const std::string alias = alias_arg(a[5]);
    Runtime::Resource *inst = rt->instantiate(nullptr, parent, alias, "", false, err);
    if (!inst) return true;  // 0x518a40 no free slot: the exe faults (INVENTED)
    inst->type = 2;          // 0x524620 creation info: INSTANCE_PHYSICS
    inst->name = alias;
    rt->bind(h, inst);       // 0x4e8a50 first 0x40cac0
    // shortcut: rigid body, 0x218 physics object and typed+0xb4 link not modelled;
    // upgrade when the physics world exists (0x4f7830/0x4f7b50/0x4f7d60/0x524620).
    return true;
}

// 0x4e1880 PhysicsRef.createSphere(ResourceRef parent, F r, String alias)
bool ph_create_sphere(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("PhysicsRef.createSphere", err)) return false;
    Object *o;
    if (!receiver_object("PhysicsRef.createSphere", a, &o, err)) return false;
    float r;
    if (!want("PhysicsRef.createSphere", a, 4, err) ||
        !get_f("PhysicsRef.createSphere", a[2], &r, err))
        return false;
    (void)r;
    // 0x4f7d60: sphere radius = r; not stored (no physics).
    Runtime *rt = game_rt();
    Runtime::Handle *h = rt->handle(o);           // 0x4ecc70
    Runtime::Resource *parent = value_resource(a[1]);
    *out = Value::make_null();
    if (!h || !parent) return true;               // exe logs and returns
    const std::string alias = alias_arg(a[3]);
    Runtime::Resource *inst = rt->instantiate(nullptr, parent, alias, "", false, err);
    if (!inst) return true;  // 0x518a40 no free slot: the exe faults (INVENTED)
    inst->type = 2;          // 0x524620 creation info: INSTANCE_PHYSICS
    inst->name = alias;
    rt->bind(h, inst);       // 0x4e8a50 first 0x40cac0
    // shortcut: rigid body, 0x218 physics object and typed+0xb4 link not modelled;
    // upgrade when the physics world exists (0x4f7830/0x4f7b50/0x4f7d60/0x524620).
    return true;
}

// INVENTED: the exe reads/writes the attached rigid body's pose (state = typed+0xb4,
// pos at state+0x64+idx*0xd4, ypr at state+0xa4+idx*0xd4, refreshed by 0x504180);
// src/ has no body service, so a physics instance keeps a stand-in pose here.
struct PhysicsPose {
    float pos[3] = {};
    float ypr[3] = {};
};
// shortcut: entries are never freed (no instance-destroy hook yet).
std::map<Runtime::Resource *, PhysicsPose> &physics_poses()
{
    static std::map<Runtime::Resource *, PhysicsPose> m;
    return m;
}

// 0x4e1c40 PhysicsRef.getPos: new Vector3 from the scripted resource pose.
bool ph_get_pos(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("PhysicsRef.getPos", err)) return false;
    Object *o;
    if (!receiver_object("PhysicsRef.getPos", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (!r) {  // exe: missing handle logs and returns null
        *out = Value::make_null();
        return true;
    }
    Object *v = new_point(vm, "java.lang.Vector3", physics_poses()[r].pos, false, err);
    if (!v) return false;
    *out = Value::make_obj(v);
    return true;
}

// 0x4e1a40 PhysicsRef.getOri: new Ypr from the scripted resource pose.
bool ph_get_ori(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("PhysicsRef.getOri", err)) return false;
    Object *o;
    if (!receiver_object("PhysicsRef.getOri", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (!r) {  // exe: missing handle logs and returns null
        *out = Value::make_null();
        return true;
    }
    Object *v = new_point(vm, "java.lang.Ypr", physics_poses()[r].ypr, true, err);
    if (!v) return false;
    *out = Value::make_obj(v);
    return true;
}

// 0x4e1e40 PhysicsRef.setMatrix(Vector3, Ypr); null arguments default to zeros.
bool ph_set_matrix(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("PhysicsRef.setMatrix", err)) return false;
    Object *o;
    if (!receiver_object("PhysicsRef.setMatrix", a, &o, err) ||
        !want("PhysicsRef.setMatrix", a, 3, err))
        return false;
    float pos[3], ypr[3];
    if (!read_vec_arg(vm, "PhysicsRef.setMatrix", a[1], pos, err) ||
        !read_ypr_arg(vm, "PhysicsRef.setMatrix", a[2], ypr, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (r) {
        PhysicsPose &p = physics_poses()[r];
        for (int i = 0; i < 3; i++) {
            p.pos[i] = pos[i];
            p.ypr[i] = ypr[i];
        }
    }
    // shortcut: the exe writes both pose buffers, matrix/quat and refreshes
    // (0x504180); only one pose is stored here (INVENTED).
    *out = Value::make_null();
    return true;
}

// ---- java.util.resource.GameRef -------------------------------------------

// 0x4daa40: RI+0x84. No missing-handle error in the exe; returns 0.
bool gr_get_flags(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getFlags", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.getFlags", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    *out = Value::make_int(r ? (int32_t)r->flags : 0);
    return true;
}

// 0x4db900: RI+0x84 |= flags; bit 0x10 also pokes the world-tree update.
// shortcut: only the stored mask is modeled (no world-tree dispatcher).
bool gr_set_flags(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.setFlags", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.setFlags", a, &o, err)) return false;
    int32_t flags;
    if (!want("GameRef.setFlags", a, 2, err) ||
        !get_i("GameRef.setFlags", a[1], &flags, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (r) r->flags |= (uint32_t)flags;
    *out = Value::make_null();
    return true;
}

// 0x4da5e0: RI+0x84 &= ~flags.
bool gr_clear_flags(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.clearFlags", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.clearFlags", a, &o, err)) return false;
    int32_t flags;
    if (!want("GameRef.clearFlags", a, 2, err) ||
        !get_i("GameRef.clearFlags", a[1], &flags, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (r) r->flags &= ~(uint32_t)flags;
    *out = Value::make_null();
    return true;
}

// 0x4e3890: missing handle/id logs and returns null; otherwise a new Vector3.
bool gr_get_pos(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getPos", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.getPos", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (!r) {
        *out = Value::make_null();
        return true;
    }
    Object *v = new_point(vm, "java.lang.Vector3", r->pos, false, err);
    if (!v) return false;
    *out = Value::make_obj(v);
    return true;
}

// 0x4dac40: FUN_004ea660 orientation, returned as Ypr.
bool gr_get_ori(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getOri", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.getOri", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (!r) {
        *out = Value::make_null();
        return true;
    }
    Object *v = new_point(vm, "java.lang.Ypr", r->ori, true, err);
    if (!v) return false;
    *out = Value::make_obj(v);
    return true;
}

// 0x4db050: FUN_0047c640(handle, 3, &v) (GII_VEL).
bool gr_get_vel(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getVel", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.getVel", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (!r) {
        *out = Value::make_null();
        return true;
    }
    Object *v = new_point(vm, "java.lang.Vector3", r->vel, false, err);
    if (!v) return false;
    *out = Value::make_obj(v);
    return true;
}

// 0x4e3a80/0x4dae20/0x4db240: fill the supplied object and return it; a missing
// handle logs and returns null (the argument is untouched).
bool gr_get_pos_out(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getPos", err)) return false;
    Object *o, *v;
    if (!receiver_object("GameRef.getPos", a, &o, err) ||
        !want("GameRef.getPos", a, 2, err) ||
        !get_o("GameRef.getPos", a[1], &v, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (!r) {
        *out = Value::make_null();
        return true;
    }
    if (!vec_write(vm, v, r->pos, err)) return false;
    *out = Value::make_obj(v);
    return true;
}

bool gr_get_ori_out(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getOri", err)) return false;
    Object *o, *v;
    if (!receiver_object("GameRef.getOri", a, &o, err) ||
        !want("GameRef.getOri", a, 2, err) ||
        !get_o("GameRef.getOri", a[1], &v, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (!r) {
        *out = Value::make_null();
        return true;
    }
    if (!ypr_write(vm, v, r->ori, err)) return false;
    *out = Value::make_obj(v);
    return true;
}

bool gr_get_vel_out(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getVel", err)) return false;
    Object *o, *v;
    if (!receiver_object("GameRef.getVel", a, &o, err) ||
        !want("GameRef.getVel", a, 2, err) ||
        !get_o("GameRef.getVel", a[1], &v, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (!r) {
        *out = Value::make_null();
        return true;
    }
    if (!vec_write(vm, v, r->vel, err)) return false;
    *out = Value::make_obj(v);
    return true;
}

// 0x4e3330: detail float at RI+0x9c for instance/render resources, else 0.
bool gr_get_detail(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getDetail", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.getDetail", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    float d = 0.0f;
    if (r && (r->type == 1 || r->type == 3)) d = r->detail;
    *out = Value::make_float(d);
    return true;
}

// 0x4db3b0: no RI or type != 8 -> 1. Type resources are empty when the record
// carries no data.
// shortcut: typed-data presence approximated by record/rsd data; upgrade when
// game-object typed data loading is modeled.
bool gr_is_empty(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.isEmpty", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.isEmpty", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    int32_t empty = 1;
    if (r && r->type == 8) {
        bool has_data = r->record && r->record->data_size > 0;
        empty = has_data ? 0 : 1;
    }
    *out = Value::make_int(empty);
    return true;
}

// 0x4db460: instance -> typed+0x50 script instance; type 8 -> type's script
// class, then the class chain check (FUN_00549d60). Null name skips the check.
bool gr_is_scripted(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.isScripted", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.isScripted", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    const std::string *name = nullptr;
    if (a.size() > 1 && a[1].kind == Value::Str) name = &a[1].s;
    std::string cls;
    int32_t scripted = 0;
    if (r) game_rt()->ensure_script_class(r);   // 0x468b00 -> getTyped path
    if (r && r->type == 1 && r->script) {
        scripted = 1;
        cls = r->script_class;  // the instance was made from the type's class
    } else if (r && r->type == 8 && !r->script_class.empty()) {
        scripted = 1;
        cls = r->script_class;
    }
    if (scripted && name) scripted = game_rt()->class_is_a(cls, *name) ? 1 : 0;
    *out = Value::make_int(scripted);
    return true;
}

// 0x4daf90: script instance of an instance, or one made for a type.
bool gr_get_script_instance(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getScriptInstance", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.getScriptInstance", a, &o, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    Object *s = game_rt()->script_instance(r);
    *out = s ? Value::make_obj(s) : Value::make_null();
    return true;
}

// 0x4da620: receiver handle required; a null parent defaults to the game root
// ref DAT_005fde50 (runtime game_root); a zero parent id short-circuits to null.
// createScripted binds the receiver handle to the new instance and returns its
// script instance.
bool gr_create(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.create", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.create", a, &o, err)) return false;
    if (!want("GameRef.create", a, 5, err)) return false;
    Runtime *rt = game_rt();
    Runtime::Handle *h = rt->handle(o);
    *out = Value::make_null();
    if (!h) return true;
    Runtime::Resource *parent = value_resource(a[1]);
    Runtime::Resource *type = value_resource(a[2]);
    if (!parent) parent = rt->game_root();
    if (!parent || parent->id == 0 || !type) return true;
    Runtime::Resource *inst = rt->instantiate(type, parent, value_string(a, 4),
                                              value_string(a, 3), true, err);
    if (!inst) return false;
    rt->bind(h, inst);
    if (inst->script) *out = Value::make_obj(inst->script);
    return true;
}

// 0x4da7e0: createNativeScripted, no script instance, receiver rebound.
bool gr_create_native(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.create_native", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.create_native", a, &o, err)) return false;
    if (!want("GameRef.create_native", a, 5, err)) return false;
    Runtime *rt = game_rt();
    Runtime::Handle *h = rt->handle(o);
    *out = Value::make_null();
    if (!h) return true;
    Runtime::Resource *parent = value_resource(a[1]);
    Runtime::Resource *type = value_resource(a[2]);
    if (!parent || parent->id == 0 || !type) return true;
    Runtime::Resource *inst = rt->instantiate(type, parent, value_string(a, 4),
                                              value_string(a, 3), false, err);
    if (!inst) return false;
    rt->bind(h, inst);
    return true;
}

// 0x4dbbe0: FUN_004ec210, resource-tree reparent.
bool gr_set_parent(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.setParent", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.setParent", a, &o, err)) return false;
    if (!want("GameRef.setParent", a, 2, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (r) game_rt()->reparent(r, value_resource(a[1]));
    *out = Value::make_null();
    return true;
}

// 0x4dbd00: position from a Vector3.
bool gr_set_pos(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.setPos", err)) return false;
    Object *o, *v;
    if (!receiver_object("GameRef.setPos", a, &o, err) ||
        !want("GameRef.setPos", a, 2, err) ||
        !get_o("GameRef.setPos", a[1], &v, err))
        return false;
    float pos[3];
    if (!vec_read(vm, v, pos, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (r)
        for (int i = 0; i < 3; i++) r->pos[i] = pos[i];
    *out = Value::make_null();
    return true;
}

// 0x4db950: pos + ori; null arguments default to zeros (FUN_004eb3f0).
bool gr_set_matrix(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.setMatrix", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.setMatrix", a, &o, err) ||
        !want("GameRef.setMatrix", a, 3, err))
        return false;
    float pos[3], ori[3];
    if (!read_vec_arg(vm, "GameRef.setMatrix", a[1], pos, err) ||
        !read_ypr_arg(vm, "GameRef.setMatrix", a[2], ori, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (r) {
        for (int i = 0; i < 3; i++) {
            r->pos[i] = pos[i];
            r->ori[i] = ori[i];
        }
    }
    *out = Value::make_null();
    return true;
}

// 0x4dbf20: position, orientation, linear and angular velocity (12 floats).
bool gr_set_state(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.setState", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.setState", a, &o, err) ||
        !want("GameRef.setState", a, 5, err))
        return false;
    float pos[3], ori[3], vel[3], ang[3];
    if (!read_vec_arg(vm, "GameRef.setState", a[1], pos, err) ||
        !read_ypr_arg(vm, "GameRef.setState", a[2], ori, err) ||
        !read_vec_arg(vm, "GameRef.setState", a[3], vel, err) ||
        !read_vec_arg(vm, "GameRef.setState", a[4], ang, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (r) {
        for (int i = 0; i < 3; i++) {
            r->pos[i] = pos[i];
            r->ori[i] = ori[i];
            r->vel[i] = vel[i];
            r->ang[i] = ang[i];
        }
    }
    *out = Value::make_null();
    return true;
}

// 0x4db590 GameRef.queueEvent(ro, type, param): dispatch through the receiver.
bool gr_queue_event(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.queueEvent", err)) return false;
    Object *o;
    if (!receiver_object("GameRef.queueEvent", a, &o, err)) return false;
    int32_t type;
    if (!want("GameRef.queueEvent", a, 4, err) ||
        !get_i("GameRef.queueEvent", a[2], &type, err))
        return false;
    Runtime::Resource *target = value_resource(a[0]);
    if (!target) {  // exe only logs the missing handle
        *out = Value::make_null();
        return true;
    }
    Runtime::Resource *sender = value_resource(a[1]);
    Value param = a[3].kind == Value::Str ? a[3] : Value::make_null();
    game_rt()->queue_event(target, sender, (uint32_t)type, param);
    *out = Value::make_null();
    return true;
}

// 0x4daa80 GameRef.getInfo(II)I / getInfo(ILjava.lang.String;)I
bool gr_get_info(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameRef.getInfo", err)) return false;
    if (!want("GameRef.getInfo", a, 3, err)) return false;
    *out = Value::make_int(0);
    Runtime::Handle *h = value_handle(a[0]);              // 0x4ecc70
    if (!h) {
        std::fprintf(stderr, "vm_gameref_getinfo: scripted resource not found\n");
        return true;
    }
    if (h->id == 0 || !h->target) return true;            // exe +8 / +0xc guards
    // shortcut: 0x4daa80 then makes the RI resident, requires typed data with a
    // native instance (typed+0x4c), resolves the type-registry entry 0x468b00
    // and calls its manager vtbl+0x3c(instance, query, sub). src has no typed-
    // data / native-type-manager layer (see the `shortcut:` in Runtime::queue_event);
    // exe failure value 0.
    return true;
}

// ---- java.lang.GameType events -------------------------------------------

// 0x4da3c0: GameType.addTimer(float deadline, int timerid). One-shot at
// g_simTime (0x60251c) + deadline.
bool gt_add_timer(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameType.addTimer", err)) return false;
    Object *o;
    float deadline;
    int32_t timerid;
    if (!receiver_object("GameType.addTimer", a, &o, err) ||
        !want("GameType.addTimer", a, 3, err) ||
        !get_f("GameType.addTimer", a[1], &deadline, err) ||
        !get_i("GameType.addTimer", a[2], &timerid, err))
        return false;
    *out = Value::make_null();
    Runtime::Handle *h = value_handle(a[0]);  // 0x4ecc70
    if (!h) {
        std::fprintf(stderr, "vm_gameref_addtimer: scripted resource not found\n");
        return true;
    }
    // asm 0x4da3f1..0x4da412: ECX = h, one-shot flag 0x80000080.
    game_rt()->add_timer(h, (float)sched(vm).sim_seconds + deadline, 0x80000080u,
                         timerid);
    return true;
}

// 0x4da930: GameType.removeAllTimers()V.
bool gt_remove_all_timers(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameType.removeAllTimers", err)) return false;
    Object *o;
    if (!receiver_object("GameType.removeAllTimers", a, &o, err)) return false;
    *out = Value::make_null();
    Runtime::Handle *h = value_handle(a[0]);  // 0x4ecc70
    if (!h) {
        std::fprintf(stderr, "vm_gameref_delalltimers: scripted resource not found\n");
        return true;
    }
    game_rt()->remove_timers(h);
    return true;
}

// 0x4dc740: GameType.registerCallback(I)V.
bool gt_register_callback(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameType.registerCallback", err)) return false;
    int32_t mode;
    if (!want("GameType.registerCallback", a, 2, err) ||
        !get_i("GameType.registerCallback", a[1], &mode, err))
        return false;
    *out = Value::make_null();
    Runtime::Handle *h = value_handle(a[0]);  // 0x4ecc70
    game_rt()->register_callback(h, mode);
    return true;
}

// 0x4dc7c0: GameType.unregisterCallback(I)V.
bool gt_unregister_callback(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameType.unregisterCallback", err)) return false;
    int32_t mode;
    if (!want("GameType.unregisterCallback", a, 2, err) ||
        !get_i("GameType.unregisterCallback", a[1], &mode, err))
        return false;
    *out = Value::make_null();
    Runtime::Handle *h = value_handle(a[0]);  // 0x4ecc70
    game_rt()->unregister_callback(h, mode);
    return true;
}

// 0x4dc800: GameType.unregisterCallbacks()V.
bool gt_unregister_callbacks(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameType.unregisterCallbacks", err)) return false;
    if (!want("GameType.unregisterCallbacks", a, 1, err)) return false;
    *out = Value::make_null();
    // 0x477320 is an empty function (bytes c2 04 00 = ret 4): no-op.
    return true;
}

// 0x4dc780: typed->event_mask |= m (typed+0xa0).
// A null receiver resource is ignored (0x4ebd90/0x4e8820 return 0 for it).
bool gt_set_event_mask(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameType.setEventMask", err)) return false;
    Object *o;
    int32_t m;
    if (!receiver_object("GameType.setEventMask", a, &o, err) ||
        !want("GameType.setEventMask", a, 2, err) ||
        !get_i("GameType.setEventMask", a[1], &m, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (r) r->event_mask |= (uint32_t)m;
    *out = Value::make_null();
    return true;
}

// 0x4dc4d0: typed->event_mask &= ~m.
// A null receiver resource is ignored (0x4ebd90/0x4e8820 return 0 for it).
bool gt_clear_event_mask(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameType.clearEventMask", err)) return false;
    Object *o;
    int32_t m;
    if (!receiver_object("GameType.clearEventMask", a, &o, err) ||
        !want("GameType.clearEventMask", a, 2, err) ||
        !get_i("GameType.clearEventMask", a[1], &m, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (r) r->event_mask &= ~(uint32_t)m;
    *out = Value::make_null();
    return true;
}

// 0x4e78e0: push a Notification onto the watched object's list (head first).
// The watcher is the receiver; the mask match replaces an earlier notification.
bool add_notification(const Args &a, Value *out, std::string *err,
                      const char *who, bool with_method)
{
    if (!need_rt(who, err)) return false;
    Object *o;
    int32_t etype, ealias;
    if (!receiver_object(who, a, &o, err) ||
        !want(who, a, with_method ? 6u : 5u, err) ||
        !get_i(who, a[2], &etype, err) ||
        !get_i(who, a[3], &ealias, err))
        return false;
    Runtime::Resource *me = value_resource(a[0]);
    *out = Value::make_null();
    if (!me) {
        // 0x4da280 / 0x4da140 log prefixes
        std::fprintf(stderr, "%s: scripted resource not found\n",
                     with_method ? "vm_gameref_addnotif2" : "vm_gameref_addnotif");
        return true;
    }
    Runtime::Resource *t = value_resource(a[1]);
    if (!t) return true;
    for (auto it = t->notifications.begin(); it != t->notifications.end(); ++it) {
        if (it->watcher == me && it->mask == (uint32_t)etype) {
            t->notifications.erase(it);
            break;
        }
    }
    Runtime::Resource::Notification n;
    n.watcher = me;
    n.mask = (uint32_t)etype;
    n.alias = (uint32_t)ealias;
    n.has_message = a[4].kind == Value::Str;
    if (n.has_message) n.message = a[4].s;
    n.has_method = with_method && a[5].kind == Value::Str;
    if (n.has_method) n.method = a[5].s;
    t->notifications.insert(t->notifications.begin(), n);
    return true;
}

// 0x4da280: GameType.addNotification(GameRef, int, int, String).
bool gt_add_notification(Vm &, const Args &a, Value *out, std::string *err)
{
    return add_notification(a, out, err, "GameType.addNotification", false);
}

// 0x4da140: GameType.addNotification(GameRef, int, int, String, String).
bool gt_add_notification_method(Vm &, const Args &a, Value *out, std::string *err)
{
    return add_notification(a, out, err, "GameType.addNotification", true);
}

// 0x4db6c0: remove the first notification watching from the receiver.
bool gt_rem_notification(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameType.remNotification", err)) return false;
    Object *o;
    int32_t etype;
    if (!receiver_object("GameType.remNotification", a, &o, err) ||
        !want("GameType.remNotification", a, 3, err) ||
        !get_i("GameType.remNotification", a[2], &etype, err))
        return false;
    Runtime::Resource *me = value_resource(a[0]);
    *out = Value::make_null();
    if (!me) {
        std::fprintf(stderr, "vm_gameref_remnotif: scripted resource not found\n");
        return true;
    }
    Runtime::Resource *t = value_resource(a[1]);
    if (!t) return true;
    for (auto it = t->notifications.begin(); it != t->notifications.end(); ++it) {
        if (it->watcher == me && it->mask == (uint32_t)etype) {
            t->notifications.erase(it);
            break;
        }
    }
    return true;
}

// 0x4dc510: typeID 0 uses the receiver's own type; otherwise the id is
// resolved as a game type. The receiver handle ends up on the new instance.
// shortcut: the exe's default ground/container (RI+0x1d4) is not modeled, so a
// null parent yields a parentless instance.
bool gt_create_native_instance(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GameType.createNativeInstance", err)) return false;
    Object *o;
    if (!receiver_object("GameType.createNativeInstance", a, &o, err)) return false;
    int32_t type_id;
    if (!want("GameType.createNativeInstance", a, 5, err) ||
        !get_i("GameType.createNativeInstance", a[2], &type_id, err))
        return false;
    Runtime *rt = game_rt();
    Runtime::Handle *h = rt->handle(o);
    *out = Value::make_null();
    if (!h) return true;
    // 0x4dc510: type 0 -> the receiver class's own type node, made once by 0x5243d0
    // under DAT_005fde80 and cached at class+0x1d4.
    // INVENTED: node type 8 and name = class name (0x5243d0/0x549ff0 not traced).
    static std::map<const ClassState *, Runtime::Resource *> class_types;
    Runtime::Resource *type = nullptr;
    if (type_id) {
        type = rt->resolve((uint32_t)type_id);
    } else if (o->cls) {
        Runtime::Resource *&t = class_types[o->cls];
        if (!t) {
            t = rt->define_script_resource(8, object_class_name(o), "", nullptr);
            t->script_class = object_class_name(o);
        }
        type = t;
    }
    if (!type) return true;
    // 0x4dc510: a null parent (or one with id 0) -> game root ref DAT_005fde50.
    Runtime::Resource *parent = value_resource(a[1]);
    if (!parent || parent->id == 0) parent = rt->game_root();
    Runtime::Resource *inst =
        rt->instantiate(type, parent, value_string(a, 4), value_string(a, 3), false, err);
    if (!inst) return false;
    rt->bind(h, inst);
    // 0x5240e0: typed+0x50 = the receiver script object, unconditionally
    inst->script = o;
    return true;
}

// ---- java.io.File: SDAT v2 datafiles --------------------------------------

// On-disk layout (confirmed against save files and 0x4d9830/0x4d93b0):
//   "SDAT\0\1\2\0", data bytes, then
//   u32 local_count, local_count*{u32 id, u32 type, i32 len, type-7 source bytes},
//   u32 lib_count, lib_count*{u32 index, u32 strlen+1, chars incl NUL},
//   u32 trailer_size = 4 + local_bytes + 4 + lib_bytes.
// The exe's local table remaps unsaved (0xffff????) resource ids; the original
// id is not serialized, so a reopened file cannot resolve it. Library ids are
// serialized as index<<16|low16 and classified by the index field.
// shortcut: no v1 trailer parsing (shipped saves are v2) and a local id read
// back from a foreign save maps to 0xffff0000|new_id instead of a recreated
// engine resource; upgrade when v1 saves or engine resource recreation matter.
struct FileLocal {
    uint32_t id = 0;    // id stored in the file
    uint32_t type = 0;  // resource type
    uint32_t orig = 0;  // original id, for files opened for writing
};

struct FileLib {
    uint32_t index = 0;  // per-file library slot stored in the id high half
    std::string name;    // library path as written (.rpk included)
    uint32_t key = 0;    // open-package index, resolved from name on open
};

struct FileState {
    FILE *f = nullptr;
    int mode = -1;
    std::string path;  // normalized path, for error messages
    std::vector<FileLocal> locals;
    std::vector<FileLib> libs;
    ~FileState()
    {
        if (f) std::fclose(f);
    }
};

std::map<Object *, std::unique_ptr<FileState>> &file_states()
{
    static std::map<Object *, std::unique_ptr<FileState>> m;
    return m;
}

FileState *file_state(Object *o)
{
    auto it = file_states().find(o);
    return it == file_states().end() ? nullptr : it->second.get();
}

void file_log(const char *what, const std::string &path)
{
    std::fprintf(stderr, "vm_file: %s: %s\n", what, path.c_str());
}

// The exe copies paths into a 256-byte buffer and canonicalizes separators.
// Rejecting an overlong path is safer than silently opening a truncated one.
bool file_path(const char *who, const std::string &in, std::string *out,
               std::string *err)
{
    if (in.size() > 255) {
        file_log("path too long", in);
        *err = std::string(who) + ": path longer than 255 bytes";
        return false;
    }
    *out = in;
    for (char &c : *out)
        if (c == '\\') c = '/';
    return true;
}

uint32_t package_index(const Package *pkg)
{
    Runtime *rt = game_rt();
    if (!rt || !pkg) return 0;
    return rt->resources().registry_index_of(*pkg);
}

uint32_t package_key(const std::string &name)
{
    Runtime *rt = game_rt();
    if (!rt) return 0;
    return rt->resources().registry_index(name);   // 0x518940
}

// 0x4e0470 java.lang.System.openLib(String)I
// exe: idx = 0x518940(name) [find-or-append in the global name registry];
// enc = idx << 16; pkg = 0x517b80(enc) [Package::load 0x51e310]; success when
// pkg state [pkg+0x50] >= 2 at 0x51e2e0. A missing path or a failed load
// returns -1. The failed registry entry is not removed by the exe.
bool system_open_lib(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("System.openLib", err)) return false;
    if (!want("System.openLib", a, 1, err)) return false;
    // INVENTED: the exe has no null check (0x4e0494 hands the raw pointer to
    // 0x518940).
    if (a[0].kind == Value::Null) {
        *out = Value::make_int(-1);
        return true;
    }
    const std::string *name;
    if (!get_s("System.openLib", a[0], &name, err)) return false;
    std::string path = *name;
    for (char &c : path)  // exe 0x565200 rewrites separators for Win32
        if (c == '\\') c = '/';
    Runtime *rt = game_rt();
    ResourceManager &rm = rt->resources();
    uint32_t idx = rm.registry_index(path);          // 0x518940
    const Package *pkg = rm.registry_package(idx);   // 0x517b80 -> 0x51e310
    if (!pkg) {
        // The failed registry entry stays, so a later openLib reuses its slot.
        *out = Value::make_int(-1);
        return true;
    }
    // 0x51e310: the load links every record into its super record's child list
    // (system.rpk and the rpkScan packages are opened here, not by resolve).
    rt->mount_package(pkg);
    *out = Value::make_int((int32_t)idx);
    return true;
}

// 0x555510: recursive scan over dir\*, returns the number of .java files.
// SERVICE MISSING: 0x5598f0 (the Java source compiler) has no src equivalent;
// only that call is skipped, each .java file is still counted.
int32_t compile_scan(const std::string &dir)
{
    std::fprintf(stderr, "CompileAll: begin file scan...\n");
    if (dir.size() + 2 > 0xff) {
        std::fprintf(stderr, "Path is too long! Can't compile %s\n", dir.c_str());
        return 0;
    }
    std::string base = dir;
    if (!base.empty() && base.back() != '/' && base.back() != '\\') base += '/';
    std::vector<fs::FindEntry> entries;
    int32_t n = 0;
    if (fs::find(base + "*", &entries)) {
        for (const fs::FindEntry &e : entries) {
            if (e.name == "." || e.name == "..") continue;
            if (e.is_dir) {
                n += compile_scan(base + e.name);
            } else {
                // 0x569370 = strrchr: the text after the LAST dot must be ".java".
                size_t dot = e.name.rfind('.');
                if (dot != std::string::npos &&
                    e.name.compare(dot, std::string::npos, ".java") == 0)
                    n++;  // 0x5598f0(dir, ...) skipped: no Java compiler
            }
        }
    }
    std::fprintf(stderr, "CompileAll: success!\n");
    return n;
}

// 0x4e54c0 java.lang.System.compileAll(String)I
bool system_compile_all(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("System.compileAll", a, 1, err)) return false;
    // INVENTED: the exe passes the raw pointer to strlen/FindFirstFileA without
    // a check; a null/non-String argument is the usual native error here.
    const std::string *arg;
    if (!get_s("System.compileAll", a[0], &arg, err)) return false;
    std::string path = *arg;
    for (char &c : path)
        if (c == '\\') c = '/';
    *out = Value::make_int(compile_scan(path));
    return true;
}

int file_put(FileState *s, const void *p, size_t n)
{
    if (!s || !s->f) return 0;
    if (std::fwrite(p, 1, n, s->f) != n) {
        file_log("write error", s->path);
        return -1;
    }
    return (int)n;
}

bool file_get(FileState *s, void *p, size_t n)
{
    return s && s->f && std::fread(p, 1, n, s->f) == n;
}

void file_parse_v2(FileState *s)
{
    std::fseek(s->f, 0, SEEK_END);
    long size = std::ftell(s->f);
    if (size < 12) return;
    uint32_t trailer = 0;
    std::fseek(s->f, size - 4, SEEK_SET);
    if (!file_get(s, &trailer, 4)) return;
    if (trailer < 8 || (long)trailer > size - 8) {
        file_log("invalid datafile", s->path);
        return;
    }
    long start = size - 4 - (long)trailer;
    std::fseek(s->f, start, SEEK_SET);
    uint32_t count = 0;
    if (!file_get(s, &count, 4)) return;
    for (uint32_t i = 0; i < count; i++) {
        FileLocal e;
        int32_t len = 0;
        if (!file_get(s, &e.id, 4) || !file_get(s, &e.type, 4) ||
            !file_get(s, &len, 4))
            return;
        e.orig = 0xffff0000u | (e.id & 0xffff);  // no engine resource to recreate
        if (e.type == 7 && len != 0) {
            long pos = std::ftell(s->f);
            if (len < 0 || pos < 0 || (long)len > size - 4 - pos) {
                file_log("invalid datafile", s->path);
                return;
            }
            std::fseek(s->f, len, SEEK_CUR);
        }
        s->locals.push_back(e);
    }
    if (!file_get(s, &count, 4)) return;
    for (uint32_t i = 0; i < count; i++) {
        FileLib e;
        uint32_t n = 0;
        if (!file_get(s, &e.index, 4) || !file_get(s, &n, 4) || n == 0) return;
        long pos = std::ftell(s->f);
        if (pos < 0 || (long)n > size - 4 - pos) {
            file_log("invalid datafile", s->path);
            return;
        }
        std::string raw(n, '\0');
        if (!file_get(s, &raw[0], n)) return;
        e.name = raw.c_str();  // written with its terminating NUL
        e.key = package_key(e.name);
        s->libs.push_back(e);
    }
}

uint32_t local_index(FileState *s, uint32_t orig, uint32_t type)
{
    for (const FileLocal &e : s->locals)
        if (e.orig == orig) return e.id;
    FileLocal e;
    e.id = (uint32_t)s->locals.size() + 1;  // the exe numbers from 1
    e.type = type;
    e.orig = orig;
    s->locals.push_back(e);
    return e.id;
}

uint32_t lib_index(FileState *s, const Package *pkg)
{
    uint32_t key = package_index(pkg);
    for (const FileLib &e : s->libs)
        if (e.key == key) return e.index;
    FileLib e;
    e.index = (uint32_t)s->libs.size() + 1;
    e.key = key;
    if (pkg) {
        e.name = pkg->mount.empty() ? pkg->rpk_path : pkg->mount + ".rpk";
        if (!e.name.empty() && e.name.back() == '/') e.name.pop_back();
    }
    s->libs.push_back(e);
    return e.index;
}

bool file_open(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    int32_t mode;
    if (!want("File.open", a, 2, err) || !get_o("File.open", a[0], &o, err) ||
        !get_i("File.open", a[1], &mode, err))
        return false;
    *out = Value::make_int(0);
    file_states().erase(o);  // reopen discards the previous handle, like 0x4d9830
    Value nv;
    std::string e;
    std::string path;
    if (!vm.get_instance_field(o, "name", &nv, &e) || nv.kind != Value::Str ||
        !file_path("File.open", nv.s, &path, err))
        return true;
    std::unique_ptr<FileState> s(new FileState);
    s->mode = mode;
    s->path = path;
    if (mode == 0) {
        s->f = std::fopen(path.c_str(), "rb");
        if (!s->f) {
            file_log("cannot open", path);
            return true;
        }
        uint8_t header[8] = {};
        if (file_get(s.get(), header, 8) && header[6] == 2) file_parse_v2(s.get());
        std::fseek(s->f, 8, SEEK_SET);
    } else if (mode == 1) {
        s->f = std::fopen(path.c_str(), "wb");
        if (!s->f) {
            file_log("cannot create", path);
            return true;
        }
        static const char header[8] = {'S', 'D', 'A', 'T', 0, 1, 2, 0};
        if (file_put(s.get(), header, 8) != 8) {
            std::fclose(s->f);
            s->f = nullptr;
            return true;
        }
    } else {
        file_log("vm_file_open: invalid file open mode", path);
        return true;
    }
    file_states()[o] = std::move(s);
    *out = Value::make_int(1);
    return true;
}

void file_write_trailer(FileState *s)
{
    uint32_t total = 4 + 4;
    uint32_t count = (uint32_t)s->locals.size();
    uint32_t scratch = 0;
    if (file_put(s, &count, 4) != 4) goto bad;
    for (const FileLocal &e : s->locals) {
        scratch = 0;  // no engine source is attached to a script-written entry
        if (file_put(s, &e.id, 4) != 4 || file_put(s, &e.type, 4) != 4 ||
            file_put(s, &scratch, 4) != 4)
            goto bad;
        total += 12;
    }
    count = (uint32_t)s->libs.size();
    if (file_put(s, &count, 4) != 4) goto bad;
    for (const FileLib &e : s->libs) {
        scratch = (uint32_t)e.name.size() + 1;
        if (file_put(s, &e.index, 4) != 4 || file_put(s, &scratch, 4) != 4 ||
            file_put(s, e.name.c_str(), scratch) != (int)scratch)
            goto bad;
        total += 8 + scratch;
    }
    if (file_put(s, &total, 4) != 4) goto bad;
    return;
bad:
    file_log("Invalid datafile saved", s->path);
}

bool file_close(Vm &, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("File.close", a, 1, err) || !get_o("File.close", a[0], &o, err))
        return false;
    auto it = file_states().find(o);
    if (it != file_states().end()) {
        FileState *s = it->second.get();
        if (s->mode == 1) file_write_trailer(s);
        if (s->f && std::fclose(s->f) != 0) file_log("close error", s->path);
        s->f = nullptr;
        file_states().erase(it);
    }
    *out = Value::make_null();
    return true;
}

bool file_write_int(Vm &, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("File.write", a, 2, err) || !get_o("File.write", a[0], &o, err))
        return false;
    uint32_t bits = 0;
    if (a[1].kind == Value::Float)
        ::memcpy(&bits, &a[1].f, 4);
    else if (!get_i("File.write", a[1], (int32_t *)&bits, err))
        return false;
    *out = Value::make_int(file_put(file_state(o), &bits, 4));
    return true;
}

bool file_write_string(Vm &, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("File.write", a, 2, err) || !get_o("File.write", a[0], &o, err))
        return false;
    FileState *s = file_state(o);
    if (!s) {
        *out = Value::make_int(0);
        return true;
    }
    if (a[1].kind != Value::Str) {  // null writes a zero length
        uint32_t zero = 0;
        file_put(s, &zero, 4);
        *out = Value::make_int(0);
        return true;
    }
    uint32_t n = (uint32_t)a[1].s.size() + 1;  // length includes the NUL
    if (file_put(s, &n, 4) != 4) {
        *out = Value::make_int(-1);
        return true;
    }
    *out = Value::make_int(file_put(s, a[1].s.c_str(), n));
    return true;
}

bool file_write_ref(Vm &, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("File.write", a, 2, err) || !get_o("File.write", a[0], &o, err))
        return false;
    FileState *s = file_state(o);
    uint32_t field = 0;
    Runtime::Handle *h = a[1].kind == Value::Obj ? value_handle(a[1]) : nullptr;
    if (h && h->id) {
        Runtime::Resource *r = h->target;
        if (h->id >> 16 == 0xffff || !r || !r->package) {
            // Unsaved resource: remap through the local table when saveable.
            if (r && (r->type == 5 || r->type == 7))
                field = local_index(s, h->id, r->type);
            else {
                file_log("ERROR! Unsaveable resource!", s ? s->path : std::string());
                field = h->id;
            }
        } else {
            field = lib_index(s, r->package) << 16 | (h->id & 0xffff);
        }
    }
    *out = Value::make_int(file_put(s, &field, 4));
    return true;
}

bool file_read_int(Vm &, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("File.readInt", a, 1, err) || !get_o("File.readInt", a[0], &o, err))
        return false;
    int32_t v = 0;
    file_get(file_state(o), &v, 4);
    *out = Value::make_int(v);
    return true;
}

bool file_read_float(Vm &, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("File.readFloat", a, 1, err) || !get_o("File.readFloat", a[0], &o, err))
        return false;
    float v = 0.0f;
    file_get(file_state(o), &v, 4);
    *out = Value::make_float(v);
    return true;
}

bool file_read_string(Vm &, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("File.readString", a, 1, err) ||
        !get_o("File.readString", a[0], &o, err))
        return false;
    *out = Value::make_null();
    FileState *s = file_state(o);
    int32_t n = 0;
    if (!file_get(s, &n, 4) || n <= 0) return true;
    if (n > (1 << 24)) {  // corrupt length; the exe would allocate it
        file_log("invalid string length", s->path);
        return true;
    }
    std::string buf((size_t)n, '\0');
    if (!file_get(s, &buf[0], (size_t)n)) {
        file_log("read error", s->path);  // exe would use uninitialized bytes
        return true;
    }
    *out = Value::make_str(buf.c_str());  // JVM__newString stops at the NUL
    return true;
}

bool file_read_res_id(Vm &, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("File.readResID", a, 1, err) ||
        !get_o("File.readResID", a[0], &o, err))
        return false;
    *out = Value::make_int(0);
    FileState *s = file_state(o);
    uint32_t id = 0;
    if (!file_get(s, &id, 4) || id == 0) return true;
    uint32_t high = id >> 16;
    if (high == 0 || high == 0xffff) {
        for (const FileLocal &e : s->locals)
            if ((e.id & 0xffff) == (id & 0xffff)) {
                *out = Value::make_int((int32_t)e.orig);
                return true;
            }
        file_log("ERROR! Unsaved resource!", s->path);
    } else {
        for (const FileLib &e : s->libs)
            if (e.index == high) {
                *out = Value::make_int((int32_t)(e.key << 16 | (id & 0xffff)));
                return true;
            }
    }
    return true;
}

// The exe static helpers return void (the native callers read stale EAX); this
// rebuild returns a meaningful 1/0 and logs failures instead of hiding them.
bool file_delete(Vm &, const Args &a, Value *out, std::string *err)
{
    const std::string *arg;
    std::string path;
    if (!want("File.delete", a, 1, err) || !get_s("File.delete", a[0], &arg, err))
        return false;
    if (!file_path("File.delete", *arg, &path, err)) return false;
    bool ok = ::remove(path.c_str()) == 0;
    if (!ok) file_log("delete failed", path);
    *out = Value::make_int(ok ? 1 : 0);
    return true;
}

bool file_copy(Vm &, const Args &a, Value *out, std::string *err)
{
    const std::string *from;
    const std::string *to;
    std::string src, dst;
    if (!want("File.copy", a, 2, err) ||
        !get_s("File.copy", a[0], &from, err) ||
        !get_s("File.copy", a[1], &to, err) ||
        !file_path("File.copy", *from, &src, err) ||
        !file_path("File.copy", *to, &dst, err))
        return false;
    *out = Value::make_int(0);
    std::vector<uint8_t> data;
    std::string e;
    if (!fs::read_file(src, &data, &e)) {
        file_log("copy read failed", src);
        return true;
    }
    FILE *f = std::fopen(dst.c_str(), "wb");
    if (!f) {
        file_log("copy open failed", dst);
        return true;
    }
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    if (std::fclose(f) != 0) ok = false;
    if (!ok) file_log("copy write failed", dst);
    *out = Value::make_int(ok ? 1 : 0);
    return true;
}

bool file_move(Vm &, const Args &a, Value *out, std::string *err)
{
    const std::string *from;
    const std::string *to;
    std::string src, dst;
    if (!want("File.move", a, 2, err) ||
        !get_s("File.move", a[0], &from, err) ||
        !get_s("File.move", a[1], &to, err) ||
        !file_path("File.move", *from, &src, err) ||
        !file_path("File.move", *to, &dst, err))
        return false;
    bool ok = ::rename(src.c_str(), dst.c_str()) == 0;
    if (!ok) file_log("move failed", src);
    *out = Value::make_int(ok ? 1 : 0);
    return true;
}

bool file_exists(Vm &, const Args &a, Value *out, std::string *err)
{
    const std::string *arg;
    std::string path;
    if (!want("File.exists", a, 1, err) || !get_s("File.exists", a[0], &arg, err))
        return false;
    if (!file_path("File.exists", *arg, &path, err)) return false;
    *out = Value::make_int(fs::exists(path) ? 1 : 0);
    return true;
}

// ---- java.io.FindFile ------------------------------------------------------

struct FindList {
    bool used = false;
    std::vector<fs::FindEntry> e;
    size_t cursor = 0;
};

// 0x7a59d0: 32 global match-list slots served by first/next/close.
FindList g_find[32];

// 0x5654c0 find_next: hand out the next entry of a slot.
int find_next(int slot, std::string *name, bool *is_dir)
{
    // INVENTED: slot range check; the exe indexes the table unchecked.
    if (slot < 0 || slot >= 32) return -1;
    FindList &l = g_find[slot];
    if (!l.used || l.cursor >= l.e.size()) return -1;
    *name = l.e[l.cursor].name;
    *is_dir = l.e[l.cursor].is_dir;
    l.cursor++;
    return 0;
}

// 0x5280c0 compare with fold=1: case-insensitive signed-char order (sort key).
int find_compare(const fs::FindEntry &a, const fs::FindEntry &b)
{
    size_t i = 0;
    for (;;) {
        int ca = i < a.name.size() ? (signed char)a.name[i] : 0;
        int cb = i < b.name.size() ? (signed char)b.name[i] : 0;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca < cb ? -1 : 1;
        if (ca == 0) return 0;
        i++;
    }
}

// 0x5652c0 find_open: snapshot the whole match list, sorted, into a free slot.
// Returns the slot, -2 when fs::find fails, -1 when all 32 slots are used.
int find_open(const std::string &path, std::string *name, bool *is_dir)
{
    std::vector<fs::FindEntry> entries;
    if (!fs::find(path, &entries)) return -2;
    int slot = -1;
    for (int i = 0; i < 32; i++)
        if (!g_find[i].used) {
            slot = i;
            break;
        }
    if (slot < 0) return -1;
    // INVENTED: stable order for names equal under folding (only possible on
    // case-sensitive hosts).
    std::stable_sort(entries.begin(), entries.end(),
                     [](const fs::FindEntry &a, const fs::FindEntry &b) {
                         return find_compare(a, b) < 0;
                     });
    g_find[slot].used = true;
    g_find[slot].e = std::move(entries);
    g_find[slot].cursor = 0;
    find_next(slot, name, is_dir);
    return slot;
}

// 0x565270 find_close: release a slot.
void find_close(int slot)
{
    if ((unsigned)slot < 32) {
        g_find[slot].used = false;
        g_find[slot].e.clear();
        g_find[slot].cursor = 0;
    }
}

// 0x4ece80: missing or non-int instance field reads as the caller's default.
int32_t find_field(Vm &vm, Object *self, const char *name, int32_t fallback)
{
    std::string e;
    Value v;
    if (!vm.get_instance_field(self, name, &v, &e) || v.kind != Value::Int)
        return fallback;
    return v.i;
}

// 0x4d9e90 FindFile.first
bool ff_first(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *self;
    const std::string *path;
    int32_t flags;
    if (!want("FindFile.first", a, 3, err) ||
        !receiver_object("FindFile.first", a, &self, err) ||
        !get_s("FindFile.first", a[1], &path, err) ||
        !get_i("FindFile.first", a[2], &flags, err))
        return false;
    std::string p;
    if (!file_path("FindFile.first", *path, &p, err)) return false;
    *out = Value::make_null();
    std::string name;
    bool is_dir = false;
    int h = find_open(p, &name, &is_dir);
    std::string e;
    vm.set_instance_field(self, "handle", Value::make_int(h), &e);
    vm.set_instance_field(self, "flags", Value::make_int(flags), &e);
    if (h < 0) return true;
    for (;;) {
        if (name != "." && name != ".." && (is_dir ? flags != 1 : flags < 2)) {
            *out = Value::make_str(name);
            return true;
        }
        if (find_next(h, &name, &is_dir) != 0) break;
    }
    return true;
}

// 0x4d9fb0 FindFile.next
bool ff_next(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *self;
    if (!receiver_object("FindFile.next", a, &self, err)) return false;
    *out = Value::make_null();
    int32_t h = find_field(vm, self, "handle", -1);
    int32_t flags = find_field(vm, self, "flags", 0);
    if (h < 0) return true;
    std::string name;
    bool is_dir = false;
    while (find_next(h, &name, &is_dir) == 0) {
        if (name != "." && name != ".." && (is_dir ? flags != 1 : flags < 2)) {
            *out = Value::make_str(name);
            break;
        }
    }
    return true;
}

// 0x4d9e40 FindFile.close
bool ff_close(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *self;
    if (!receiver_object("FindFile.close", a, &self, err)) return false;
    int32_t h = find_field(vm, self, "handle", -1);
    std::string e;
    vm.set_instance_field(self, "handle", Value::make_int(-1), &e);
    if (h >= 0) find_close(h);
    *out = Value::make_null();
    return true;
}

// ---- java.game.parts: Chassis / WheelRef / Part ---------------------------

// A script object's handle target is the instance or type record carrying the
// payload (Part/Chassis resolve typed data through the receiver).
game::PartData *part_of(const Args &a)
{
    Runtime *rt = game_rt();
    return rt ? game::part_data(*rt, value_resource(a[0])) : nullptr;
}

game::CarData *car_of(const Args &a)
{
    Runtime *rt = game_rt();
    return rt ? game::car_data(*rt, value_resource(a[0])) : nullptr;
}

game::WheelData *wheel_of(const Args &a)
{
    if (a.empty() || a[0].kind != Value::Obj || !a[0].obj) return nullptr;
    return game::wheel_data(a[0].obj);
}

game::Slot *slot_by_id(game::PartData *p, int32_t id)
{
    if (!p) return nullptr;
    for (game::Slot &s : p->slots)
        if (s.id == id) return &s;
    return nullptr;
}

float health_factor(float w)
{
    return w >= 1.0f ? 0.100000024f : 1.0f - w * w * 0.6f;
}

bool car_float_get(const char *who, const Args &a, float game::CarData::*f,
                   Value *out, std::string *err)
{
    if (!want(who, a, 1, err)) return false;
    game::CarData *c = car_of(a);
    *out = Value::make_float(c ? c->*f : 0.0f);
    return true;
}

bool car_float_set(const char *who, const Args &a, float game::CarData::*f,
                   Value *out, std::string *err)
{
    float v;
    if (!want(who, a, 2, err) || !get_f(who, a[1], &v, err)) return false;
    game::CarData *c = car_of(a);
    if (c) c->*f = v;
    *out = Value::make_null();
    return true;
}

bool wheel_float_get(const char *who, const Args &a, float game::WheelData::*f,
                     Value *out, std::string *err)
{
    if (!want(who, a, 1, err)) return false;
    game::WheelData *w = wheel_of(a);
    *out = Value::make_float(w ? w->*f : 0.0f);
    return true;
}

bool wheel_float_set(const char *who, const Args &a, float game::WheelData::*f,
                     Value *out, std::string *err)
{
    float v;
    if (!want(who, a, 2, err) || !get_f(who, a[1], &v, err)) return false;
    game::WheelData *w = wheel_of(a);
    if (w) w->*f = v;
    *out = Value::make_null();
    return true;
}

bool ch_get_material_index(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Chassis.getMaterialIndex", a, 1, err)) return false;
    game::CarData *c = car_of(a);
    *out = Value::make_int(c ? c->material_index : -1);
    return true;
}

bool ch_get_max_steer(Vm &, const Args &a, Value *out, std::string *err)
{
    return car_float_get("Chassis.getMaxSteer", a, &game::CarData::max_steer, out, err);
}

bool ch_set_max_steer(Vm &, const Args &a, Value *out, std::string *err)
{
    return car_float_set("Chassis.setMaxSteer", a, &game::CarData::max_steer, out, err);
}

bool ch_get_shifting(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Chassis.getShifting", a, 1, err)) return false;
    game::CarData *c = car_of(a);
    *out = Value::make_int(c ? c->shifting : 0);
    return true;
}

bool ch_get_nitroing(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Chassis.getNitroing", a, 1, err)) return false;
    game::CarData *c = car_of(a);
    *out = Value::make_int(c ? (int32_t)((c->flags >> 11) & 1) : 0);
    return true;
}

bool ch_get_engine_temp(Vm &, const Args &a, Value *out, std::string *err)
{
    return car_float_get("Chassis.getEngineTemp", a, &game::CarData::engine_temp, out, err);
}

bool ch_get_ai_float(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t id;
    if (!want("Chassis.getAIparam_float", a, 2, err) ||
        !get_i("Chassis.getAIparam_float", a[1], &id, err))
        return false;
    game::CarData *c = car_of(a);
    float v = 0.0f;  // shortcut: invalid ids read uninitialized exe stack
    if (c) {
        switch (id) {
        case 0: v = c->throttle; break;
        case 1: v = c->brake_input; break;
        case 2: v = c->handbrake_input; break;
        case 3: v = c->steer_input; break;
        case 4: v = c->clutch_input; break;
        case 5: v = c->nitro_input; break;
        default: break;
        }
    }
    *out = Value::make_float(v);
    return true;
}

bool ch_get_ai_int(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t id;
    if (!want("Chassis.getAIparam_int", a, 2, err) ||
        !get_i("Chassis.getAIparam_int", a[1], &id, err))
        return false;
    game::CarData *c = car_of(a);
    int32_t v = id;
    if (c && id == 6) v = c->gear;
    else if (c && id == 7) v = c->gear_copy;
    else if (c && id == 8) v = c->horn_id;
    *out = Value::make_int(v);
    return true;
}

bool ch_set_ai_float(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t id;
    float v;
    if (!want("Chassis.setAIparam_float", a, 3, err) ||
        !get_i("Chassis.setAIparam_float", a[1], &id, err) ||
        !get_f("Chassis.setAIparam_float", a[2], &v, err))
        return false;
    if (game::CarData *c = car_of(a)) {
        switch (id) {
        case 0: c->throttle = v; break;
        case 1: c->brake_input = v; break;
        case 2: c->handbrake_input = v; break;
        case 3: c->steer_input = v; break;
        case 4: c->clutch_input = v; break;
        case 5: c->nitro_input = v; break;
        default: break;
        }
    }
    *out = Value::make_null();
    return true;
}

bool ch_set_ai_int(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t id, v;
    if (!want("Chassis.setAIparam_int", a, 3, err) ||
        !get_i("Chassis.setAIparam_int", a[1], &id, err) ||
        !get_i("Chassis.setAIparam_int", a[2], &v, err))
        return false;
    if (game::CarData *c = car_of(a)) {
        if (id == 6) c->gear = v;
        else if (id == 7) c->gear_copy = v;
        else if (id == 8) c->horn_id = v;
    }
    *out = Value::make_null();
    return true;
}

bool ch_get_rpm(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Chassis.getRPM", a, 1, err)) return false;
    game::CarData *c = car_of(a);
    *out = Value::make_float(c ? c->rpm_smoothed * 9.549296f : 0.0f);
    return true;
}

bool ch_set_rpm(Vm &, const Args &a, Value *out, std::string *err)
{
    return car_float_set("Chassis.setRPM", a, &game::CarData::rpm_raw, out, err);
}

bool ch_get_torque(Vm &, const Args &a, Value *out, std::string *err)
{
    float rpm, nitro;
    if (!want("Chassis.getTorque", a, 3, err) ||
        !get_f("Chassis.getTorque", a[1], &rpm, err) ||
        !get_f("Chassis.getTorque", a[2], &nitro, err))
        return false;
    game::CarData *c = car_of(a);
    if (!c) {
        *out = Value::make_float(0.0f);
        return true;
    }
    float t = c->torque_at(rpm) * c->torque_health * (1.0f - nitro) +
              c->nitro_torque_at(rpm) * c->nitro_health * nitro;
    *out = Value::make_float(t);
    return true;
}

bool ch_set_torque(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    if (!want("Chassis.setTorque", a, 2, err) ||
        !get_f("Chassis.setTorque", a[1], &v, err))
        return false;
    if (game::CarData *c = car_of(a)) {
        c->torque_base = v;
        c->nitro_torque_base = v;
        float f = health_factor(c->engine_damage);
        c->torque_health = v * f;
        c->nitro_health = f * v;
    }
    *out = Value::make_null();
    return true;
}

// 0x4b5890 Chassis.forceUpdate -> 0x4aebf0 Vehicle__refreshParameters
bool ch_force_update(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!want("Chassis.forceUpdate", a, 1, err)) return false;
    game::CarData *c = car_of(a);
    Object *self = a[0].kind == Value::Obj ? a[0].obj : nullptr;
    Runtime *rt = game_rt();
    if (!c || !self || !rt) {
        *out = Value::make_null();
        return true;
    }
    c->flags &= ~0x800000u;
    if (find_field(vm, self, "suspend_update", 0) != 0) {
        // shortcut: Vehicle__applyTuningOverrides 0x498020 not ported.
        *out = Value::make_null();
        return true;
    }
    // shortcut: suspension wear (+0x16c/+0x170) and the +0x160/+0x168 rates are
    // not modelled.
    // INVENTED: WheelData() defaults stand in for the manager template M+0x2ec4.
    game::WheelData def;
    for (game::WheelData &w : c->wheels) {
        w.tyre_wear = 0.0f;
        w.brake_wear = 0.0f;
        std::copy(std::begin(def.pacejka), std::end(def.pacejka), w.pacejka);
    }
    std::string e;  // Chassis.java updatevariables() runs dyno.calcDyno()
    Value r;
    rt->call_method(self, "updatevariables", {}, &r, &e);
    c->inertia = dd_get_f(vm, self, "engine_inertia");
    // shortcut: installed parts' wear records (0x4a8060) not re-applied.
    c->torque_base = 1.0f;
    c->nitro_torque_base = 1.0f;
    c->starter_torque = dd_get_f(vm, self, "starter_torque");
    c->friction_fwd = dd_get_f(vm, self, "engine_friction_fwd");
    c->friction_rev = dd_get_f(vm, self, "engine_friction_rev");
    float idle_rpm = dd_get_f(vm, self, "engine_rpm_idle");
    float idle_rad = idle_rpm * 0.016666668f;
    c->idle_omega = idle_rad * 3.1415927f + idle_rad * 3.1415927f;
    float mass = dd_get_f(vm, self, "engine_mass");
    float old_heat = c->heat_capacity;
    c->heat_capacity = mass * 8000.0f;
    c->cooling_coeff = (old_heat * c->cooling_coeff) / (mass * 8000.0f);
    float f = health_factor(c->engine_damage);
    c->torque_health = c->torque_base * f;
    c->nitro_health = f * c->nitro_torque_base;
    // INVENTED: resolved at refresh time; the exe stores e+0xc30 in
    // Vehicle__onPartAttached 0x4942b0 (no part-attach events yet).
    game::Slot *s = slot_by_id(part_of(a), 401);
    Object *src = nullptr;
    if (!s) src = self;
    else if (s->attached) src = s->attached;
    if (src) {
        Value dv;
        if (vm.get_instance_field(src, "dynodata", &dv, &e) && dv.kind == Value::Obj &&
            dv.obj && vm.find_class("java.lang.Native"))
            c->dyno = game::dyno_data(dv.obj);
    }
    game::refresh_engine_curve(*c);
    Value ratio;
    if (vm.get_instance_field(self, "ratio", &ratio, &e) && ratio.kind == Value::Array &&
        ratio.arr) {
        size_t n = std::min(ratio.arr->elems.size(), (size_t)8);
        for (size_t i = 0; i < n; i++) {
            const Value &v = ratio.arr->elems[i];
            c->ratio[i] = v.kind == Value::Float ? v.f : v.kind == Value::Int ? (float)v.i : 0.0f;
        }
    }
    c->gears = find_field(vm, self, "gears", 0);
    c->rearend_ratio = dd_get_f(vm, self, "rearend_ratio");
    c->max_rpm = dd_get_f(vm, self, "maxRPM");
    c->rpm_limit = dd_get_f(vm, self, "RPM_limit");
    c->clutch_f = dd_get_f(vm, self, "ClutchF");
    // shortcut: SFX_trans_fwd/rev, SFX_ignition resource refs and wheel slot rebinding not modelled.
    c->rpm_trans_fwd = dd_get_f(vm, self, "rpm_trans_fwd");
    c->rpm_trans_rev = dd_get_f(vm, self, "rpm_trans_rev");
    c->sfx_starter_rpm = dd_get_f(vm, self, "sfx_starter_rpm");
    c->horn_id = 0;
    c->tank_nitro = dd_get_f(vm, self, "tank_nitro");
    c->consumption_nitro = dd_get_f(vm, self, "consumption_nitro");
    c->nitro = c->tank_nitro <= c->nitro ? c->tank_nitro : c->nitro;
    Value dc;
    Object *dco = vm.get_instance_field(self, "drag_center", &dc, &e) && dc.kind == Value::Obj
                      ? dc.obj
                      : nullptr;
    c->drag_center[0] = dco ? dd_get_f(vm, dco, "x") : 0.0f;
    c->drag_center[1] = dco ? dd_get_f(vm, dco, "y") : 0.0f;
    c->drag_center[2] = dco ? dd_get_f(vm, dco, "z") : 0.0f;
    c->c_drag = dd_get_f(vm, self, "C_drag");
    // shortcut: Vehicle__applyTuningOverrides 0x498020 not ported.
    *out = Value::make_null();
    return true;
}

bool ch_set_cooling(Vm &, const Args &a, Value *out, std::string *err)
{
    float lo, air, speed;
    if (!want("Chassis.setCooling", a, 4, err) ||
        !get_f("Chassis.setCooling", a[1], &lo, err) ||
        !get_f("Chassis.setCooling", a[2], &air, err) ||
        !get_f("Chassis.setCooling", a[3], &speed, err))
        return false;
    if (lo < 0.0f) lo = 10.0f;
    if (air < 0.0f) air = 50.0f;
    if (speed <= 0.0f) speed = 0.0f;
    if (game::CarData *c = car_of(a)) {
        c->cooling_low = lo;
        c->cooling_air = air;
        c->cooling_speed = speed;
    }
    *out = Value::make_null();
    return true;
}

bool ch_get_mileage(Vm &, const Args &a, Value *out, std::string *err)
{
    return car_float_get("Chassis.getMileage", a, &game::CarData::mileage, out, err);
}

bool ch_set_mileage(Vm &, const Args &a, Value *out, std::string *err)
{
    return car_float_set("Chassis.setMileage", a, &game::CarData::mileage, out, err);
}

bool ch_set_exhaust_min_vol(Vm &, const Args &a, Value *out, std::string *err)
{
    return car_float_set("Chassis.setSfxExhaustMinVol", a,
                         &game::CarData::exhaust_min_vol, out, err);
}

bool ch_set_steer_wheel_radius(Vm &, const Args &a, Value *out, std::string *err)
{
    return car_float_set("Chassis.setSteerWheelRadius", a,
                         &game::CarData::steer_wheel_radius, out, err);
}

bool ch_set_steer_wheel(Vm &, const Args &a, Value *out, std::string *err)
{
    float radius, angle;
    if (!want("Chassis.setSteerWheel", a, 3, err) ||
        !get_f("Chassis.setSteerWheel", a[1], &radius, err) ||
        !get_f("Chassis.setSteerWheel", a[2], &angle, err))
        return false;
    if (game::CarData *c = car_of(a)) {
        c->steer_wheel_radius = radius;
        c->steer_wheel_angle = angle;
    }
    *out = Value::make_null();
    return true;
}

bool ch_set_ackermann(Vm &, const Args &a, Value *out, std::string *err)
{
    return car_float_set("Chassis.setAckermann", a, &game::CarData::ackermann,
                         out, err);
}

bool ch_get_nitro(Vm &, const Args &a, Value *out, std::string *err)
{
    return car_float_get("Chassis.getNitro", a, &game::CarData::nitro, out, err);
}

bool ve_has_crime(Vm &, const Args &a, Value *out, std::string *err)
{
    return car_float_get("Vehicle.hasCrime", a, &game::CarData::crime, out, err);
}

// 0x4b2ae0 java.game.Vehicle.getHorn
bool veh_get_horn(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Vehicle.getHorn", a, 1, err)) return false;
    int32_t v = 0;
    if (game::CarData *c = car_of(a)) {
        // INVENTED: the exe indexes e+0x840+horn_id*0x10 unchecked; clamp to
        // the four horn records.
        int32_t id = c->horn_id;
        if (id >= 0 && id < 4)
            v = c->engine_sfx.horn_resource[id] != 0 ? 1 : 0;
    }
    *out = Value::make_int(v);
    return true;
}

// 0x4e6960 java.game.Vehicle.getSpeedSquare
bool veh_get_speed_square(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Vehicle.getSpeedSquare", a, 1, err)) return false;
    Runtime::Resource *r = value_resource(a[0]);
    float sq = 0.0f;
    if (r)
        sq = r->vel[0] * r->vel[0] + r->vel[1] * r->vel[1] +
             r->vel[2] * r->vel[2];
    *out = Value::make_float(sq);
    return true;
}

bool ch_get_wheels(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Chassis.getWheels", a, 1, err)) return false;
    game::CarData *c = car_of(a);
    *out = Value::make_int(c ? (int32_t)c->wheels.size() : 0);
    return true;
}

bool ch_get_wheel(Vm &vm, const Args &a, Value *out, std::string *err)
{
    int32_t i;
    if (!want("Chassis.getWheel", a, 2, err) ||
        !get_i("Chassis.getWheel", a[1], &i, err))
        return false;
    *out = Value::make_null();
    game::CarData *c = car_of(a);
    if (!c || i < 0 || (size_t)i >= c->wheels.size()) return true;
    ClassState *cs = vm.find_class("java.game.parts.WheelRef");
    if (!cs) {
        *err = "Chassis.getWheel: java.game.parts.WheelRef is not loaded";
        return false;
    }
    Object *o = vm.new_object(cs, err);
    if (!o) return false;
    game::bind_wheel(o, &c->wheels[(size_t)i]);
    *out = Value::make_obj(o);
    return true;
}

bool ch_get_wheel_damage(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t i;
    if (!want("Chassis.getWheelDamage", a, 2, err) ||
        !get_i("Chassis.getWheelDamage", a[1], &i, err))
        return false;
    *out = Value::make_null();
    game::CarData *c = car_of(a);
    // The exe guards `count < index` (off-by-one); use the safe bound.
    if (!c || i < 0 || (size_t)i >= c->wheels.size()) return true;
    const game::WheelData &w = c->wheels[(size_t)i];
    *out = Value::make_str(w.flags & 0x20u ? game::wheel_damage(w)
                                           : std::string());
    return true;
}

bool ch_set_wheel_damage(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t i;
    const std::string *s;
    if (!want("Chassis.setWheelDamage", a, 3, err) ||
        !get_i("Chassis.setWheelDamage", a[1], &i, err) ||
        !get_s("Chassis.setWheelDamage", a[2], &s, err))
        return false;
    game::CarData *c = car_of(a);
    if (c && i >= 0 && (size_t)i < c->wheels.size())
        game::set_wheel_damage(c->wheels[(size_t)i], *s);
    *out = Value::make_null();
    return true;
}

bool wr_get_pos(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!want("WheelRef.getPos", a, 1, err)) return false;
    *out = Value::make_null();
    game::WheelData *w = wheel_of(a);
    if (!w) return true;
    float v[3] = {w->template_pos[0],
                  w->hub[1] - w->template_pos[1] - w->base_len - w->travel,
                  w->template_pos[2]};
    Object *o = new_point(vm, "java.lang.Vector3", v, false, err);
    if (!o) return false;
    *out = Value::make_obj(o);
    return true;
}

bool wr_get_ypr(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!want("WheelRef.getYpr", a, 1, err)) return false;
    *out = Value::make_null();
    game::WheelData *w = wheel_of(a);
    if (!w) return true;
    float v[3];
    mat_to_ypr(w->rot, v);
    Object *o = new_point(vm, "java.lang.Ypr", v, true, err);
    if (!o) return false;
    *out = Value::make_obj(o);
    return true;
}

bool wr_get_drive(Vm &, const Args &a, Value *out, std::string *err)
{
    return wheel_float_get("WheelRef.getDrive", a, &game::WheelData::drive, out, err);
}

bool wr_get_steer(Vm &, const Args &a, Value *out, std::string *err)
{
    return wheel_float_get("WheelRef.getSteer", a, &game::WheelData::steer, out, err);
}

bool wr_get_radius(Vm &, const Args &a, Value *out, std::string *err)
{
    return wheel_float_get("WheelRef.getRadius", a, &game::WheelData::radius, out, err);
}

bool wr_get_brake(Vm &, const Args &a, Value *out, std::string *err)
{
    return wheel_float_get("WheelRef.getBrake", a, &game::WheelData::brake, out, err);
}

bool wr_get_hbrake(Vm &, const Args &a, Value *out, std::string *err)
{
    return wheel_float_get("WheelRef.getHBrake", a, &game::WheelData::hbrake, out, err);
}

bool wr_set_pos(Vm &vm, const Args &a, Value *out, std::string *err)
{
    float v[3];
    if (!want("WheelRef.setPos", a, 2, err) ||
        !read_vec_arg(vm, "WheelRef.setPos", a[1], v, err))
        return false;
    if (game::WheelData *w = wheel_of(a)) w->set_pos(v);
    *out = Value::make_null();
    return true;
}

bool wr_set_ypr(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!want("WheelRef.setYpr", a, 2, err)) return false;
    game::WheelData *w = wheel_of(a);
    if (a[1].kind == Value::Null) {
        if (w) w->set_ypr(nullptr);
        *out = Value::make_null();
        return true;
    }
    Object *o;
    float v[3];
    if (!get_o("WheelRef.setYpr", a[1], &o, err) || !ypr_read(vm, o, v, err))
        return false;
    if (w) w->set_ypr(v);
    *out = Value::make_null();
    return true;
}

bool wr_set_drive(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    if (!want("WheelRef.setDrive", a, 2, err) ||
        !get_f("WheelRef.setDrive", a[1], &v, err))
        return false;
    if (game::WheelData *w = wheel_of(a)) {
        w->drive = v;
        w->drive_eff = v;
    }
    *out = Value::make_null();
    return true;
}

bool wr_set_steer(Vm &, const Args &a, Value *out, std::string *err)
{
    return wheel_float_set("WheelRef.setSteer", a, &game::WheelData::steer, out, err);
}

bool wr_set_radius(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    if (!want("WheelRef.setRadius", a, 2, err) ||
        !get_f("WheelRef.setRadius", a[1], &v, err))
        return false;
    if (game::WheelData *w = wheel_of(a)) w->set_radius(v);
    *out = Value::make_null();
    return true;
}

bool wr_set_width(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    if (!want("WheelRef.setWidth", a, 2, err) ||
        !get_f("WheelRef.setWidth", a[1], &v, err))
        return false;
    if (game::WheelData *w = wheel_of(a)) w->half_width = v * 0.5f;
    *out = Value::make_null();
    return true;
}

bool wr_set_cpatch(Vm &, const Args &a, Value *out, std::string *err)
{
    float half, angle, offset;
    if (!want("WheelRef.setCPatch", a, 4, err) ||
        !get_f("WheelRef.setCPatch", a[1], &half, err) ||
        !get_f("WheelRef.setCPatch", a[2], &angle, err) ||
        !get_f("WheelRef.setCPatch", a[3], &offset, err))
        return false;
    if (game::WheelData *w = wheel_of(a)) {
        w->cpatch_halfwidth = fabsf(half);
        w->cpatch_sin = fabsf(half * sinf(angle));
        w->cpatch_offset = offset;
    }
    *out = Value::make_null();
    return true;
}

bool wr_set_friction(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    if (!want("WheelRef.setFriction", a, 2, err) ||
        !get_f("WheelRef.setFriction", a[1], &v, err))
        return false;
    if (game::WheelData *w = wheel_of(a)) w->set_friction(v);
    *out = Value::make_null();
    return true;
}

bool wr_set_frictn_x(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    if (!want("WheelRef.setFrictn_x", a, 2, err) ||
        !get_f("WheelRef.setFrictn_x", a[1], &v, err))
        return false;
    if (game::WheelData *w = wheel_of(a)) w->pacejka[12] = v;
    *out = Value::make_null();
    return true;
}

bool wr_set_sliction(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    if (!want("WheelRef.setSliction", a, 2, err) ||
        !get_f("WheelRef.setSliction", a[1], &v, err))
        return false;
    if (game::WheelData *w = wheel_of(a)) w->pacejka[0] = v;
    *out = Value::make_null();
    return true;
}

bool wr_set_stiffness(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    if (!want("WheelRef.setStiffness", a, 2, err) ||
        !get_f("WheelRef.setStiffness", a[1], &v, err))
        return false;
    if (game::WheelData *w = wheel_of(a)) w->pacejka[4] = v;
    *out = Value::make_null();
    return true;
}

bool wr_set_roll_res(Vm &, const Args &a, Value *out, std::string *err)
{
    return wheel_float_set("WheelRef.setRollRes", a, &game::WheelData::roll_res,
                           out, err);
}

bool wr_set_bearing(Vm &, const Args &a, Value *out, std::string *err)
{
    return wheel_float_set("WheelRef.setBearing", a,
                           &game::WheelData::damage_visual, out, err);
}

bool wr_set_max_load(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    if (!want("WheelRef.setMaxLoad", a, 2, err) ||
        !get_f("WheelRef.setMaxLoad", a[1], &v, err))
        return false;
    if (game::WheelData *w = wheel_of(a)) w->pacejka[11] = v;
    *out = Value::make_null();
    return true;
}

bool wr_set_load_smooth(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    if (!want("WheelRef.setLoadSmooth", a, 2, err) ||
        !get_f("WheelRef.setLoadSmooth", a[1], &v, err))
        return false;
    if (game::WheelData *w = wheel_of(a)) w->pacejka[14] = v;
    *out = Value::make_null();
    return true;
}

bool wr_set_pacejka(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t i;
    float v;
    if (!want("WheelRef.setPacejka", a, 3, err) ||
        !get_i("WheelRef.setPacejka", a[1], &i, err) ||
        !get_f("WheelRef.setPacejka", a[2], &v, err))
        return false;
    if (game::WheelData *w = wheel_of(a))
        if (i >= 0 && i < game::kPacejkaSlots) w->pacejka[i] = v;
    *out = Value::make_null();
    return true;
}

bool wr_set_force(Vm &, const Args &a, Value *out, std::string *err)
{
    return wheel_float_set("WheelRef.setForce", a, &game::WheelData::spring_k,
                           out, err);
}

bool wr_set_damping1(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    if (!want("WheelRef.setDamping", a, 2, err) ||
        !get_f("WheelRef.setDamping", a[1], &v, err))
        return false;
    if (game::WheelData *w = wheel_of(a)) {
        w->bump = v;
        w->rebound = v * 0.7f;
    }
    *out = Value::make_null();
    return true;
}

bool wr_set_damping2(Vm &, const Args &a, Value *out, std::string *err)
{
    float bump, rebound;
    if (!want("WheelRef.setDamping", a, 3, err) ||
        !get_f("WheelRef.setDamping", a[1], &bump, err) ||
        !get_f("WheelRef.setDamping", a[2], &rebound, err))
        return false;
    if (game::WheelData *w = wheel_of(a)) {
        w->bump = bump;
        w->rebound = rebound;
    }
    *out = Value::make_null();
    return true;
}

bool wr_set_rest_len(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    if (!want("WheelRef.setRestLen", a, 2, err) ||
        !get_f("WheelRef.setRestLen", a[1], &v, err))
        return false;
    if (game::WheelData *w = wheel_of(a)) w->set_rest_len(v);
    *out = Value::make_null();
    return true;
}

bool wr_set_min_len(Vm &, const Args &a, Value *out, std::string *err)
{
    return wheel_float_set("WheelRef.setMinLen", a, &game::WheelData::min_len,
                           out, err);
}

bool wr_set_max_len(Vm &, const Args &a, Value *out, std::string *err)
{
    return wheel_float_set("WheelRef.setMaxLen", a, &game::WheelData::max_len,
                           out, err);
}

bool wr_set_brake(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    if (!want("WheelRef.setBrake", a, 2, err) ||
        !get_f("WheelRef.setBrake", a[1], &v, err))
        return false;
    if (game::WheelData *w = wheel_of(a)) w->set_brake(v);
    *out = Value::make_null();
    return true;
}

bool wr_set_hbrake(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    if (!want("WheelRef.setHBrake", a, 2, err) ||
        !get_f("WheelRef.setHBrake", a[1], &v, err))
        return false;
    if (game::WheelData *w = wheel_of(a)) w->set_hbrake(v);
    *out = Value::make_null();
    return true;
}

bool wr_set_arm(Vm &, const Args &a, Value *out, std::string *err)
{
    float v[7];
    if (!want("WheelRef.setArm", a, 8, err)) return false;
    for (int i = 0; i < 7; i++)
        if (!get_f("WheelRef.setArm", a[(size_t)i + 1], &v[i], err)) return false;
    if (game::WheelData *w = wheel_of(a))
        w->set_arm(v[0], &v[1], &v[4]);
    *out = Value::make_null();
    return true;
}

bool wr_set_hub(Vm &, const Args &a, Value *out, std::string *err)
{
    float v[10];
    if (!want("WheelRef.setHub", a, 11, err)) return false;
    for (int i = 0; i < 10; i++)
        if (!get_f("WheelRef.setHub", a[(size_t)i + 1], &v[i], err)) return false;
    if (game::WheelData *w = wheel_of(a))
        w->set_hub(v[0], &v[1], &v[4], &v[7]);
    *out = Value::make_null();
    return true;
}

bool wr_set_instant_center(Vm &, const Args &a, Value *out, std::string *err)
{
    float v[6];
    if (!want("WheelRef.setInstantCenter", a, 7, err)) return false;
    for (int i = 0; i < 6; i++)
        if (!get_f("WheelRef.setInstantCenter", a[(size_t)i + 1], &v[i], err))
            return false;
    if (game::WheelData *w = wheel_of(a)) {
        for (int i = 0; i < 3; i++) {
            w->ic_h[i] = w->hub[i] + v[i];
            w->ic_l[i] = w->hub[i] + v[3 + i];
        }
    }
    *out = Value::make_null();
    return true;
}

bool wr_set_opp_wheel(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t i;
    if (!want("WheelRef.setOppWheel", a, 2, err) ||
        !get_i("WheelRef.setOppWheel", a[1], &i, err))
        return false;
    if (game::WheelData *w = wheel_of(a)) {
        // The exe indexes without a bound check; return null instead.
        w->opp = w->car && i >= 0 && (size_t)i < w->car->wheels.size()
                     ? &w->car->wheels[(size_t)i]
                     : nullptr;
    }
    *out = Value::make_null();
    return true;
}

// ---- java.game.parts.DynoData ----

// 0x4cc2b0 / 0x4cc1b0 table interpolation. rpm has already been checked >= 0.
float dyno_lerp(const EngineModel &e, const std::vector<float> &t, float rpm)
{
    float result = 0.0f;
    int32_t i = 1;
    if (1 < e.count) {
        int32_t x = e.step;
        while ((float)x <= rpm) {
            i++;
            x += e.step;
            if (e.count <= i) return result;
        }
        // INVENTED: the exe reads the table pointer blindly; guard short tables.
        if (t.size() <= (size_t)i) return result;
        result = ((rpm - (float)((i - 1) * e.step)) *
                      (t[(size_t)i] - t[(size_t)i - 1]) /
                      (float)e.step) +
                 t[(size_t)i - 1];
    }
    return result;
}

// Shared prologue of getTorque/getHP. *e is null when the exe returns 0.0f.
bool dyno_arg(const Vm &vm, const Args &a, const char *who, const EngineModel **e,
              float *rpm, std::string *err)
{
    *e = nullptr;
    if (!want(who, a, 3, err)) return false;
    Object *self;
    float nitro;
    if (!receiver_object(who, a, &self, err) ||
        !get_f(who, a[1], rpm, err) || !get_f(who, a[2], &nitro, err))
        return false;
    (void)nitro;
    if (!vm.find_class("java.lang.Native")) return true;
    const EngineModel *m = game::dyno_data(self);
    if (m && 0.0f <= *rpm) *e = m;
    return true;
}

// 0x4cc3b0 DynoData.newNative
bool dd_new_native(Vm &, const Args &a, Value *out, std::string *err)
{
    Object *self;
    if (!receiver_object("DynoData.newNative", a, &self, err)) return false;
    game::attach_dyno(self);
    *out = Value::make_null();
    return true;
}

// 0x4cc150 DynoData.deleteNative
bool dd_delete_native(Vm &, const Args &a, Value *out, std::string *err)
{
    Object *self;
    if (!receiver_object("DynoData.deleteNative", a, &self, err)) return false;
    game::detach_dyno(self);
    *out = Value::make_null();
    return true;
}

// 0x4cc2b0 DynoData.getTorque
bool dd_get_torque(Vm &vm, const Args &a, Value *out, std::string *err)
{
    const EngineModel *e;
    float rpm;
    if (!dyno_arg(vm, a, "DynoData.getTorque", &e, &rpm, err)) return false;
    *out = Value::make_float(e ? dyno_lerp(*e, e->torque_table[0], rpm) : 0.0f);
    return true;
}

// 0x4cc1b0 DynoData.getHP
bool dd_get_hp(Vm &vm, const Args &a, Value *out, std::string *err)
{
    const EngineModel *e;
    float rpm;
    if (!dyno_arg(vm, a, "DynoData.getHP", &e, &rpm, err)) return false;
    *out = Value::make_float(e ? dyno_lerp(*e, e->power_table[0], rpm) : 0.0f);
    return true;
}

// 0x4ec980 Instance__getFloatField: missing or non-numeric field reads as 0.
float dd_get_f(Vm &vm, Object *self, const char *name)
{
    std::string e;
    Value v;
    if (!vm.get_instance_field(self, name, &v, &e)) return 0.0f;
    if (v.kind == Value::Float) return v.f;
    if (v.kind == Value::Int) return (float)v.i;
    return 0.0f;
}

// 0x4ecff0: field lookup only.
bool dd_has(Vm &vm, Object *self, const char *name)
{
    std::string e;
    Value v;
    return vm.get_instance_field(self, name, &v, &e);
}

// 0x4ed080 Instance__setFloatField: failures (missing field) are ignored.
void dd_set_f(Vm &vm, Object *self, const char *name, float value)
{
    std::string e;
    vm.set_instance_field(self, name, Value::make_float(value), &e);
}

// 0x4ed060: arr[i] = max(table[i], 0), array length taken from the script side.
// INVENTED: i >= table.size() -> write 0 (the exe reads past the table when the
// script array is longer than count).
void dd_fill_array(Vm &vm, Object *self, const char *name, const std::vector<float> &table)
{
    std::string e;
    Value v;
    if (!vm.get_instance_field(self, name, &v, &e) || v.kind != Value::Array || !v.arr)
        return;
    for (size_t i = 0; i < v.arr->elems.size(); i++) {
        float x = i < table.size() ? table[i] : 0.0f;
        v.arr->elems[i] = Value::make_float(x <= 0.0f ? 0.0f : x);
    }
}

// 0x4cba80 DynoData.calcDyno
bool dd_calc_dyno(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *self;
    float tablesize;
    if (!want("DynoData.calcDyno", a, 2, err) ||
        !receiver_object("DynoData.calcDyno", a, &self, err) ||
        !get_f("DynoData.calcDyno", a[1], &tablesize, err))
        return false;

    bool temp = false;
    EngineModel *e = nullptr;
    if (vm.find_class("java.lang.Native")) e = game::dyno_data(self);
    std::unique_ptr<EngineModel> temp_model;
    if (!e) {
        temp = true;
        e = new EngineModel;
        temp_model.reset(e);
    }
    e->set_defaults();
    e->cylinders = dd_get_f(vm, self, "cylinders");
    e->bore_r = dd_get_f(vm, self, "bore") * 0.5f;
    e->crank_r = dd_get_f(vm, self, "stroke") * 0.5f;
    e->v_min = dd_get_f(vm, self, "Vmin");
    e->in_min = dd_get_f(vm, self, "in_min");
    e->in_max = dd_get_f(vm, self, "in_max");
    e->out_min = dd_get_f(vm, self, "out_min");
    e->out_max = dd_get_f(vm, self, "out_max");
    e->in_open = dd_get_f(vm, self, "time_in_open");
    e->in_close = dd_get_f(vm, self, "time_in_close");
    e->out_open = dd_get_f(vm, self, "time_out_open");
    float oc = dd_get_f(vm, self, "time_out_close");
    e->in_open_base = e->in_open;
    e->out_close = oc;
    e->in_duration = e->in_close - e->in_open;
    e->out_open_base = e->out_open;
    e->out_duration = oc - e->out_open;
    e->time_burn = dd_get_f(vm, self, "time_burn");
    e->rpm_limit = dd_get_f(vm, self, "RPM_limit");
    e->spark_min = dd_get_f(vm, self, "time_spark_min");
    e->spark_inc = dd_get_f(vm, self, "time_spark_inc");
    e->spark_rpm0 = dd_get_f(vm, self, "time_spark_RPM0");
    e->spark_rpm1 = dd_get_f(vm, self, "time_spark_RPM1");
    e->turbo_mul = dd_get_f(vm, self, "rpm_turbo_mul");
    e->turbo_opt = dd_get_f(vm, self, "rpm_turbo_opt");
    e->turbo_range = dd_get_f(vm, self, "rpm_turbo_range");
    e->turbo_band = 0.0f;
    e->p_turbo_max = dd_get_f(vm, self, "P_turbo_max");
    e->p_turbo_waste =
        dd_has(vm, self, "P_turbo_waste") ? dd_get_f(vm, self, "P_turbo_waste") : e->p_turbo_max;
    e->mixture_h = dd_get_f(vm, self, "mixture_H");
    e->afr = dd_get_f(vm, self, "mixture_ratio");
    e->max_air = dd_get_f(vm, self, "max_air_consumption");
    e->max_fuel = dd_get_f(vm, self, "max_fuel_consumption");
    e->t_intake = 293.15f;
    if (dd_has(vm, self, "intercooling"))
        e->t_intake = e->t_intake * dd_get_f(vm, self, "intercooling");
    e->update_mixture();
    e->update_geometry();
    float mr = dd_get_f(vm, self, "maxRPM");
    if (0.0f < mr)
        e->max_rpm = mr;
    else
        dd_set_f(vm, self, "maxRPM", e->max_rpm);
    float st = e->max_rpm / tablesize;
    e->table_set = 0;
    if (st <= 1.0f) st = 1.0f;
    e->build_tables((int)st);
    if (temp) {
        dd_set_f(vm, self, "table_stepsize", st);
        dd_set_f(vm, self, "torque", 1.0f);
        dd_fill_array(vm, self, "torquetable", e->torque_table[0]);
        dd_fill_array(vm, self, "HPtable", e->power_table[0]);
    }
    dd_set_f(vm, self, "Displacement", e->displacement);
    dd_set_f(vm, self, "Compression", e->compression);
    dd_set_f(vm, self, "maxTorque", e->max_torque);
    dd_set_f(vm, self, "RPM_maxTorque", e->rpm_max_torque);
    dd_set_f(vm, self, "maxHP", e->max_hp);
    dd_set_f(vm, self, "RPM_maxHP", e->rpm_max_hp);
    // nitro pass: intercooling is dropped
    e->t_intake = 293.15f;
    float nh = dd_get_f(vm, self, "nitro_H");
    float nc = dd_get_f(vm, self, "nitro_cooling");
    if (0.0001f < nh - 1.0f || 0.0001f < nc - 1.0f) {
        e->mixture_h = e->mixture_h * nh;
        e->t_intake = e->t_intake * nc;
        float keep = e->max_rpm;
        e->update_mixture();
        e->update_geometry();
        e->max_rpm = keep;
        e->table_set = 1;
        e->build_tables((int)(keep / tablesize));  // no >= 1 clamp here
        if (!temp) {
            *out = Value::make_float(0.0f);  // persistent model: done
            return true;
        }
        dd_set_f(vm, self, "torque2", 1.0f);
        dd_fill_array(vm, self, "torquetable2", e->torque_table[1]);
    }
    *out = Value::make_float(0.0f);
    return true;
}

bool pt_get_slots(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Part.getSlots", a, 1, err)) return false;
    game::PartData *p = part_of(a);
    *out = Value::make_int(p ? (int32_t)p->slots.size() : 0);
    return true;
}

bool pt_get_slot_id(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t i;
    if (!want("Part.getSlotID", a, 2, err) ||
        !get_i("Part.getSlotID", a[1], &i, err))
        return false;
    game::PartData *p = part_of(a);
    int32_t id = 0;
    if (p && i < 0) {
        for (const game::Slot &s : p->slots)
            if (s.flags & 1u) {
                id = s.id;
                break;
            }
    } else if (p && (size_t)i < p->slots.size()) {
        id = p->slots[(size_t)i].id;
    }
    *out = Value::make_int(id);
    return true;
}

bool pt_get_slot_index(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t id;
    if (!want("Part.getSlotIndex", a, 2, err) ||
        !get_i("Part.getSlotIndex", a[1], &id, err))
        return false;
    game::PartData *p = part_of(a);
    int32_t index = 0;
    if (p)
        for (size_t i = 0; i < p->slots.size(); i++)
            if (p->slots[i].id == id) {
                index = (int32_t)i;
                break;
            }
    *out = Value::make_int(index);
    return true;
}

bool pt_part_on_slot(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t id;
    if (!want("Part.partOnSlot", a, 2, err) ||
        !get_i("Part.partOnSlot", a[1], &id, err))
        return false;
    if (id == -1) {
        *out = a[0];  // this part's own script instance
        return true;
    }
    game::Slot *s = slot_by_id(part_of(a), id);
    *out = s && s->attached ? Value::make_obj(s->attached) : Value::make_null();
    return true;
}

bool pt_slot_id_on_slot(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t id;
    if (!want("Part.slotIDOnSlot", a, 2, err) ||
        !get_i("Part.slotIDOnSlot", a[1], &id, err))
        return false;
    game::Slot *s = slot_by_id(part_of(a), id);
    *out = Value::make_int(s ? s->mate_id : 0);
    return true;
}

bool pt_get_slot_damage(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t i;
    if (!want("Part.getSlotDamage", a, 2, err) ||
        !get_i("Part.getSlotDamage", a[1], &i, err))
        return false;
    game::PartData *p = part_of(a);
    *out = Value::make_str(p && i >= 0 && (size_t)i < p->slots.size() &&
                                   (p->slots[(size_t)i].flags & 0x20u)
                               ? game::slot_damage(p->slots[(size_t)i])
                               : std::string());
    return true;
}

bool pt_set_slot_damage(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t i;
    const std::string *s;
    if (!want("Part.setSlotDamage", a, 3, err) ||
        !get_i("Part.setSlotDamage", a[1], &i, err) ||
        !get_s("Part.setSlotDamage", a[2], &s, err))
        return false;
    game::PartData *p = part_of(a);
    if (p && i >= 0 && (size_t)i < p->slots.size())
        game::set_slot_damage(p->slots[(size_t)i], *s);
    *out = Value::make_null();
    return true;
}

bool pt_set_slot_pos(Vm &vm, const Args &a, Value *out, std::string *err)
{
    int32_t id;
    float pos[3] = {}, ypr[3] = {};
    if (!want("Part.setSlotPos", a, 4, err) ||
        !get_i("Part.setSlotPos", a[1], &id, err))
        return false;
    bool has_pos = a[2].kind != Value::Null;
    bool has_ypr = a[3].kind != Value::Null;
    if (has_pos && !read_vec_arg(vm, "Part.setSlotPos", a[2], pos, err))
        return false;
    if (has_ypr && !read_ypr_arg(vm, "Part.setSlotPos", a[3], ypr, err))
        return false;
    if (game::Slot *s = slot_by_id(part_of(a), id)) {
        for (int i = 0; i < 3; i++) s->pos[i] = s->type_pos[i];
        s->rot = s->type_rot;
        s->flags &= ~0x20u;
        if (has_pos) {
            for (int i = 0; i < 3; i++) s->pos[i] += pos[i];
            s->flags |= 0x20u;
        }
        if (has_ypr) {
            Mat34 e;
            euler_to_mat(&e, ypr);
            mat_mul(&s->rot, &e, &s->type_rot);
            s->flags |= 0x20u;
        }
    }
    *out = Value::make_null();
    return true;
}

bool pt_get_car_ref(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Part.getCarRef", a, 1, err)) return false;
    game::PartData *p = part_of(a);
    *out = p && p->car_ref && p->car_ref->script
               ? Value::make_obj(p->car_ref->script)
               : Value::make_null();
    return true;
}

bool pt_get_wear(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Part.getWear", a, 1, err)) return false;
    game::PartData *p = part_of(a);
    *out = Value::make_float(p ? 1.0f - p->wear : 1.0f);
    return true;
}

bool pt_set_wear(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    if (!want("Part.setWear", a, 2, err) ||
        !get_f("Part.setWear", a[1], &v, err))
        return false;
    if (game::PartData *p = part_of(a)) p->wear = 1.0f - v;
    *out = Value::make_float(v);
    return true;
}

bool pt_set_max_wear(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    if (!want("Part.setMaxWear", a, 2, err) ||
        !get_f("Part.setMaxWear", a[1], &v, err))
        return false;
    if (game::PartData *p = part_of(a)) p->max_wear_inv = 1.0f / v;
    *out = Value::make_null();
    return true;
}

bool pt_get_tear(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Part.getTear", a, 1, err)) return false;
    game::PartData *p = part_of(a);
    *out = Value::make_float(p ? p->tear : 1.0f);
    return true;
}

bool pt_set_tear(Vm &, const Args &a, Value *out, std::string *err)
{
    float v;
    if (!want("Part.setTear", a, 2, err) ||
        !get_f("Part.setTear", a[1], &v, err))
        return false;
    if (game::PartData *p = part_of(a)) p->tear = v;
    *out = Value::make_float(v);
    return true;
}

bool pt_disable_slot(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t id, status;
    if (!want("Part.disableSlot", a, 3, err) ||
        !get_i("Part.disableSlot", a[1], &id, err) ||
        !get_i("Part.disableSlot", a[2], &status, err))
        return false;
    // shortcut: mate-slot propagation on an installed part, upgrade when
    // install_OK creates attachments.
    if (game::Slot *s = slot_by_id(part_of(a), id)) {
        s->flags &= ~0x50u;
        if (status == 1) s->flags |= 0x10u;
        else if (status == 2) s->flags |= 0x50u;
    }
    *out = Value::make_null();
    return true;
}

bool pt_is_slot_disabled(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t id;
    if (!want("Part.isSlotDisabled", a, 2, err) ||
        !get_i("Part.isSlotDisabled", a[1], &id, err))
        return false;
    game::Slot *s = slot_by_id(part_of(a), id);
    int32_t v = 0;
    if (s) v = (s->flags & 0x50u) ? 2 : (int32_t)((s->flags >> 4) & 1u);
    *out = Value::make_int(v);
    return true;
}

bool pt_get_texture(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Part.getTexture", a, 1, err)) return false;
    game::PartData *p = part_of(a);
    *out = Value::make_int(p ? p->texture : 0);
    return true;
}

bool pt_set_texture(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t id;
    if (!want("Part.setTexture", a, 2, err) ||
        !get_i("Part.setTexture", a[1], &id, err))
        return false;
    (void)id;  // the exe requests the resource and returns 1; P+0x84 is not written
    *out = Value::make_int(1);
    return true;
}

bool pt_get_mesh(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Part.getMesh", a, 1, err)) return false;
    game::PartData *p = part_of(a);
    *out = Value::make_int(p ? p->mesh : 0);
    return true;
}

bool pt_set_mesh(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t id;
    if (!want("Part.setMesh", a, 2, err) ||
        !get_i("Part.setMesh", a[1], &id, err))
        return false;
    (void)id;
    *out = Value::make_int(1);
    return true;
}

bool pt_get_render_type(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Part.getRenderType", a, 1, err)) return false;
    game::PartData *p = part_of(a);
    *out = Value::make_int(p ? p->render_type : 0);
    return true;
}

bool pt_set_render_type(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t id;
    if (!want("Part.setRenderType", a, 2, err) ||
        !get_i("Part.setRenderType", a[1], &id, err))
        return false;
    (void)id;
    *out = Value::make_int(1);
    return true;
}

bool pt_get_car(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Part.getCar", a, 1, err)) return false;
    game::PartData *p = part_of(a);
    *out = Value::make_int(p ? p->car_id : 0);
    return true;
}

// 0x4cc6e0 Part.getLogo
bool pt_get_logo(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Part.getLogo", a, 1, err)) return false;
    // shortcut: no typed type manager (M+0x1120) in this rebuild; every 0x4cc6e0
    // failure path returns 0. Java Part.getLogo() (Part.java:730) shadows it.
    *out = Value::make_int(0);
    return true;
}

// 0x4cc7d0 Part.getMass
bool pt_get_mass(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Part.getMass", a, 1, err)) return false;
    game::PartData *p = part_of(a);
    float inv = p ? p->inv_mass : 0.0f;
    *out = Value::make_float(inv > 0.0f ? 1.0f / inv : 0.0f);  // JBE 0x4cc878
    return true;
}

// 0x4cc440 Part.flap
bool pt_flap(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t mode;
    if (!want("Part.flap", a, 2, err) || !get_i("Part.flap", a[1], &mode, err))
        return false;
    (void)mode;
    // shortcut: no per-slot bone/render object (slot+0x28, flags +0x88) nor the
    // 0x4b1030/0x4cb8d0/0x4b10c0/0x4b1770 trigger calls; the exe falls through the
    // 0x4c5ed0 slot loop and returns 0. Upgrade when part physics/render bones exist.
    *out = Value::make_int(0);
    return true;
}

bool pt_get_wheel_id(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Part.getWheelID", a, 1, err)) return false;
    game::PartData *p = part_of(a);
    int32_t v = p ? p->wheel_id : 0;
    *out = Value::make_int(v > 100 && v < 401 ? (v - 101) % 10 : -1);
    return true;
}

bool pt_set_sfx_loop_params(Vm &, const Args &a, Value *out, std::string *err)
{
    float x, y;
    if (!want("Part.setSfxLoopParams", a, 3, err) ||
        !get_f("Part.setSfxLoopParams", a[1], &x, err) ||
        !get_f("Part.setSfxLoopParams", a[2], &y, err))
        return false;
    game::PartData *p = part_of(a);
    if (!p) {
        *out = Value::make_int(0);
        return true;
    }
    p->sfx_loop[0] = x;
    p->sfx_loop[1] = y;
    *out = Value::make_int(1);
    return true;
}

// ---- java.game.parts.SfxTable ---------------------------------------------

// Native handle payload (wrapper object -> raw table pointer).
audio::SfxTable *sfx_table_of(const Args &a)
{
    if (a.empty() || a[0].kind != Value::Obj || !a[0].obj) return nullptr;
    return game::sfx_table(a[0].obj);
}

// 0x4b5a90 java.game.parts.SfxTable.getItems
bool sfx_get_items(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("SfxTable.getItems", a, 1, err)) return false;
    audio::SfxTable *t = sfx_table_of(a);
    *out = Value::make_int(t ? t->items_count() : 0);
    return true;
}

// 0x4b5a60 java.game.parts.SfxTable.clear (0x498280 helper)
bool sfx_clear(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("SfxTable.clear", a, 1, err)) return false;
    if (audio::SfxTable *t = sfx_table_of(a)) t->clear();
    *out = Value::make_null();
    return true;
}

// 0x4b5980 java.game.parts.SfxTable.addItem
bool sfx_add_item(Vm &, const Args &a, Value *out, std::string *err)
{
    float pitch, pmin, pmax, vmin, vmax;
    if (!want("SfxTable.addItem", a, 7, err) ||
        !get_f("SfxTable.addItem", a[2], &pitch, err) ||
        !get_f("SfxTable.addItem", a[3], &pmin, err) ||
        !get_f("SfxTable.addItem", a[4], &pmax, err) ||
        !get_f("SfxTable.addItem", a[5], &vmin, err) ||
        !get_f("SfxTable.addItem", a[6], &vmax, err))
        return false;
    if (audio::SfxTable *t = sfx_table_of(a)) {
        // INVENTED: the exe links the SfxRef RI (0x40cac0) and stores its
        // pointer; audio::SfxTable keeps an int32 key -> use the resource id.
        // shortcut: no addref/unlink of the resource; upgrade when SFX resource
        // lifetime is modelled.
        Runtime::Resource *r = value_resource(a[1]);
        t->add_item(r ? (int32_t)r->id : 0, pitch, pmin, pmax, vmin, vmax);
    }
    *out = Value::make_null();
    return true;
}

// ---- java.util.resource.SfxRef ---------------------------------------------

// Resource_Sound__vf10 0x525cf0 sync path for a type-6 (res_sound) record.
void sfx_load(Runtime::Resource *r)
{
    audio::SfxService &s = audio::sfx_service();
    audio::SoundParams params;
    const char *data = nullptr;
    size_t size = 0;
    if (r->record && r->package && r->record->data_size > 0) {
        data = (const char *)r->package->rpk.data(*r->record);
        size = r->record->data_size;
    } else if (!r->data.empty()) {
        data = r->data.data();   // 0x518410 descriptor data (makeSound)
        size = r->data.size();
    }
    const bool have = data && audio::parse_sound_params(data, size, &params);
    std::vector<uint8_t> file;
    bool read = false;
    if (have) {
        std::string path = game_rt()->resources().resolve_path(params.sourcefile);
        std::string err;
        read = !path.empty() && fs::read_file(path, &file, &err);
    }
    s.load(r, have ? &params : nullptr, read ? &file : nullptr);
}

// 0x4e4610 SfxRef.nplay(Vector3,float radius,float pitch,float volume,int flags,
// int instance)I
bool sfxref_nplay(Vm &vm, const Args &a, Value *out, std::string *err)
{
    float radius, pitch, volume;
    int32_t flags, instance;
    if (!want("SfxRef.nplay", a, 7, err) ||
        !get_f("SfxRef.nplay", a[2], &radius, err) ||
        !get_f("SfxRef.nplay", a[3], &pitch, err) ||
        !get_f("SfxRef.nplay", a[4], &volume, err) ||
        !get_i("SfxRef.nplay", a[5], &flags, err) ||
        !get_i("SfxRef.nplay", a[6], &instance, err))
        return false;
    Runtime::Handle *h = value_handle(a[0]);  // 0x4ecc70
    if (!h) {
        std::fprintf(stderr, "vm_sfxref_play: scripted resource not found\n");
        *out = Value::make_int(-1);
        return true;
    }
    float p[3];
    const float *pos = nullptr;
    if (a[1].kind == Value::Obj && a[1].obj) {   // read after the handle check (exe order)
        p[0] = dd_get_f(vm, a[1].obj, "x");
        p[1] = dd_get_f(vm, a[1].obj, "y");
        p[2] = dd_get_f(vm, a[1].obj, "z");
        pos = p;
    }
    audio::SfxService &s = audio::sfx_service();
    Runtime::Resource *r = h->target;
    // 0x4eb5c0 cull / no RI. INVENTED: the typed object of other resource kinds is
    // not a CompatibleSound; treat as no asset.
    if ((pos && !audio::in_range(pos, radius, s.pool.listener)) || !r || r->type != 6) {
        *out = Value::make_int(-1);
        return true;
    }
    if (!s.loaded(r)) {
        sfx_load(r);
        // INVENTED timing: 0x518a80 queues a streaming load and this post returns -1;
        // src has no streaming queue, so the load completes now and the next call plays.
        *out = Value::make_int(-1);
        return true;
    }
    *out = Value::make_int(audio::post_event(s.pool, s.assets, s.key(r), (int32_t)h->id,
        pitch, audio::channel_volumes().effects * volume, (uint32_t)flags, pos, instance,
        radius, nullptr, nullptr));
    return true;
}

// 0x4e47d0 SfxRef.stop(int instance)V
bool sfxref_stop(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t instance;
    if (!want("SfxRef.stop", a, 2, err) ||
        !get_i("SfxRef.stop", a[1], &instance, err))
        return false;
    Runtime::Handle *h = value_handle(a[0]);  // 0x4ecc70
    if (!h) {
        std::fprintf(stderr, "vm_sfxref_stop: scripted resource not found\n");
        *out = Value::make_null();
        return true;
    }
    Runtime::Resource *r = h->target;
    if (r && r->type == 6) {
        audio::SfxService &s = audio::sfx_service();
        if (!s.loaded(r)) sfx_load(r);   // 0x518a80 (0,0): synchronous load
        const audio::AudioAssets::Asset *as = s.assets.get(s.key(r));
        if (as && as->bus >= 0) s.pool.stop(instance, (int32_t)h->id, *s.backend);  // 0x569020
    }
    *out = Value::make_null();
    return true;
}

// ---- java.game.Painter -----------------------------------------------------

// 0x4e07c0 Painter.xPaint(GameRef part)V: queue the constant decal flush on
// the part. The exe has no null check and would fault in 0x47d550 on an
// unbound ref node; src's queue_event ignores a null target.
bool painter_x_paint(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Painter.xPaint", err) || !want("Painter.xPaint", a, 2, err))
        return false;
    *out = Value::make_null();
    game_rt()->queue_event(value_resource(a[1]), nullptr, 0x10,
                           Value::make_str("bigdecal 0,0 0,0,0 0,0,0 0 0 1 0"));
    return true;
}

// 0x4e0800 Painter.paintPart(GameRef cursor, int color)V
// SERVICE MISSING: 0x4eb000 (render-scene pick) does not exist in src. The
// exe returns here when the pick misses, then requires the hit surface's
// GII_CATEGORY (0x47c640 id 7) to be 9 (GIR_CAT_PART) or 5 (GIR_CAT_VEHICLE),
// and queues "paint 0 0 %d 0 -1" (0x5a527c) with `color` on that surface
// (0x47d550, code 0x10). Args are copied unchecked by getArgs (0x4b1af0).
bool painter_paint_part(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Painter.paintPart", a, 3, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4e0560 Painter.doPaint(GameRef cursor, int color, int brush, int temp,
//                          float rot, float size, int flip)V
// SERVICE MISSING: same 0x4eb000 gate. After a hit the exe queues
// "bigdecal %d,%d %.3f,%.3f,%.3f %.3f,%.3f,%.3f %.2f %.2f %d %d"
// (0x5a5570) with color, brush id, the two sampled 3D points, size, rot,
// temp and flip. brush id = `brush` if the id resolves (0x51f9b0 ->
// 0x5183a0) else 0; the temporary node has no other effect.
// doPaint is `native static` (shipped Painter.java:349), so src's
// run_native passes no receiver: 7 arguments (vm.cpp:1608).
bool painter_do_paint(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Painter.doPaint", a, 7, err)) return false;
    *out = Value::make_null();
    return true;
}

// ---- java.io.Controller ----------------------------------------------------

// INVENTED: the exe allocates the block with the typed "controller" object; we
// create it on first use with reset defaults (AxisRecord/Mapping initializers
// equal 0x566030's values).
input::Controller *controller_of(Runtime::Resource *r)
{
    if (!r) return nullptr;
    // shortcut: entries are never freed (no instance-destroy hook yet).
    static std::map<Runtime::Resource *, std::unique_ptr<input::Controller>> m;
    std::unique_ptr<input::Controller> &c = m[r];
    if (!c) c.reset(new input::Controller(input::devices()));
    return c.get();
}

// 0x4d3cb0 Controller.user_Add
bool ctl_user_add(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t vaxis, device, paxis;
    float in_min, in_max, out_min, out_max;
    if (!want("Controller.user_Add", a, 7, err) ||
        !get_i("Controller.user_Add", a[1], &vaxis, err) ||
        !get_i("Controller.user_Add", a[2], &device, err) ||
        !get_i("Controller.user_Add", a[3], &paxis, err) ||
        !get_f("Controller.user_Add", a[4], &in_min, err) ||
        !get_f("Controller.user_Add", a[5], &in_max, err) ||
        !get_f("Controller.user_Add", a[6], &out_min, err) ||
        !get_f("Controller.user_Add", a[7], &out_max, err))
        return false;
    input::Controller *c = controller_of(value_resource(a[0]));
    *out = Value::make_int(c ? c->add(vaxis, device, paxis, in_min, in_max,
                                      out_min, out_max)
                             : 0);
    return true;
}

// 0x4d3db0 Controller.user_Del
bool ctl_user_del(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t vaxis, device, paxis;
    if (!want("Controller.user_Del", a, 4, err) ||
        !get_i("Controller.user_Del", a[1], &vaxis, err) ||
        !get_i("Controller.user_Del", a[2], &device, err) ||
        !get_i("Controller.user_Del", a[3], &paxis, err))
        return false;
    if (input::Controller *c = controller_of(value_resource(a[0])))
        c->remove(vaxis, device, paxis);
    *out = Value::make_int(0);  // always 0, even when resolved
    return true;
}

// 0x4d3f10 Controller.user_Reset
bool ctl_user_reset(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Controller.user_Reset", a, 1, err)) return false;
    if (input::Controller *c = controller_of(value_resource(a[0]))) c->reset();
    *out = Value::make_null();
    return true;
}

// 0x4d3e60 Controller.user_GetAxisVal
bool ctl_user_get_axis_val(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t vaxis;
    if (!want("Controller.user_GetAxisVal", a, 2, err) ||
        !get_i("Controller.user_GetAxisVal", a[1], &vaxis, err))
        return false;
    input::Controller *c = controller_of(value_resource(a[0]));
    *out = Value::make_float(c ? c->get_axis_val(vaxis) : 0.0f);
    return true;
}

// 0x4d4080 Controller.user_SetAxisSmooth: returns 1 even when unresolved.
bool ctl_user_set_axis_smooth(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t vaxis;
    float center_range, center_factor, opposite, same, power;
    if (!want("Controller.user_SetAxisSmooth", a, 6, err) ||
        !get_i("Controller.user_SetAxisSmooth", a[1], &vaxis, err) ||
        !get_f("Controller.user_SetAxisSmooth", a[2], &center_range, err) ||
        !get_f("Controller.user_SetAxisSmooth", a[3], &center_factor, err) ||
        !get_f("Controller.user_SetAxisSmooth", a[4], &opposite, err) ||
        !get_f("Controller.user_SetAxisSmooth", a[5], &same, err) ||
        !get_f("Controller.user_SetAxisSmooth", a[6], &power, err))
        return false;
    if (input::Controller *c = controller_of(value_resource(a[0])))
        c->set_axis_smooth(vaxis, center_range, center_factor, opposite, same,
                           power);
    *out = Value::make_int(1);
    return true;
}

// 0x4d4180 Controller.user_SetAxisSpeed
bool ctl_user_set_axis_speed(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t vaxis;
    float speed;
    if (!want("Controller.user_SetAxisSpeed", a, 2, err) ||
        !get_i("Controller.user_SetAxisSpeed", a[1], &vaxis, err) ||
        !get_f("Controller.user_SetAxisSpeed", a[2], &speed, err))
        return false;
    if (input::Controller *c = controller_of(value_resource(a[0])))
        c->set_axis_speed(vaxis, speed);
    *out = Value::make_null();
    return true;
}

// 0x4d3fb0 Controller.user_SetAxisForce (native returns 1 in eax; Java sig V)
bool ctl_user_set_axis_force(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t vaxis;
    float force_c, force_f;
    if (!want("Controller.user_SetAxisForce", a, 3, err) ||
        !get_i("Controller.user_SetAxisForce", a[1], &vaxis, err) ||
        !get_f("Controller.user_SetAxisForce", a[2], &force_c, err) ||
        !get_f("Controller.user_SetAxisForce", a[3], &force_f, err))
        return false;
    if (input::Controller *c = controller_of(value_resource(a[0])))
        c->set_axis_force(vaxis, force_c, force_f);
    *out = Value::make_null();
    return true;
}

// ---- java.io.Input ---------------------------------------------------------

// FUN_004ed490: write the script Hotkey's int field "state"; a set error is
// ignored, like the exe's field setter.
void hotkey_set_state(void *hotkey, int state, void *ctx)
{
    if (hotkey == nullptr)
        return;
    std::string e;
    ((Vm *)ctx)->set_instance_field((Object *)hotkey, "state",
                                    Value::make_int(state), &e);
}

// 0x4de880 Input.createHotkey (static)
bool in_create_hotkey(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t axis, flags, events;
    if (!want("Input.createHotkey", a, 6, err) ||
        !get_i("Input.createHotkey", a[0], &axis, err) ||
        !get_i("Input.createHotkey", a[1], &flags, err) ||
        !get_i("Input.createHotkey", a[5], &events, err))
        return false;
    input::hotkeys().create(axis, flags, obj(a[2]), obj(a[3]), obj(a[4]),
                            (uint32_t)events);
    *out = Value::make_null();
    return true;
}

// 0x4de920 Input.deleteHotkey (static)
bool in_delete_hotkey(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Input.deleteHotkey", a, 1, err)) return false;
    input::hotkeys().remove(obj(a[0]));
    *out = Value::make_null();
    return true;
}

// 0x4de970 Input.flushHotkeys (static)
bool in_flush_hotkeys(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Input.flushHotkeys", a, 0, err)) return false;
    input::hotkeys().flush();
    *out = Value::make_null();
    return true;
}

// 0x4de750 Input.checkHotkeys (static): instance -> its controller block.
bool in_check_hotkeys(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!want("Input.checkHotkeys", a, 2, err)) return false;
    input::Controller *c = controller_of(value_resource(a[0]));
    input::hotkeys().check(c, input::devices(), obj(a[1]), hotkey_set_state, &vm);
    std::vector<input::QueuedEvent> q;
    q.swap(input::hotkeys().queue);
    for (const input::QueuedEvent &ev : q)   // 0x4de750 tail -> 0x47d550
        if (Runtime *rt = game_rt())
            rt->queue_event(value_resource(Value::make_obj((Object *)ev.handler)), nullptr,
                            0x100000, ev.hotkey ? Value::make_obj((Object *)ev.hotkey)
                                                : Value::make_null());
    *out = Value::make_null();
    return true;
}

// 0x4de990 Input.getAxis
bool in_get_axis(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t d, i;
    if (!want("Input.getAxis", a, 2, err) || !get_i("Input.getAxis", a[0], &d, err) ||
        !get_i("Input.getAxis", a[1], &i, err))
        return false;
    *out = Value::make_float(input::devices().get_axis(d, i));
    return true;
}

// 0x4de9c0 Input.getDeviceName
bool in_get_device_name(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t i;
    if (!want("Input.getDeviceName", a, 1, err) ||
        !get_i("Input.getDeviceName", a[0], &i, err))
        return false;
    const char *p = input::devices().device_name(i);
    *out = p ? Value::make_str(p) : Value::make_null();
    return true;
}

// 0x4dea20 Input.mapAxis (static): instance -> its controller block.
// INVENTED: unresolved instance is a no-op; the exe would dereference null.
bool in_map_axis(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t l_axis, device, i_axis;
    float i_from, i_to, l_from, l_to;
    if (!want("Input.mapAxis", a, 8, err) ||
        !get_i("Input.mapAxis", a[1], &l_axis, err) ||
        !get_i("Input.mapAxis", a[2], &device, err) ||
        !get_i("Input.mapAxis", a[3], &i_axis, err) ||
        !get_f("Input.mapAxis", a[4], &i_from, err) ||
        !get_f("Input.mapAxis", a[5], &i_to, err) ||
        !get_f("Input.mapAxis", a[6], &l_from, err) ||
        !get_f("Input.mapAxis", a[7], &l_to, err))
        return false;
    if (input::Controller *c = controller_of(value_resource(a[0])))
        c->add(l_axis, device, i_axis, i_from, i_to, l_from, l_to);
    *out = Value::make_null();
    return true;
}

// 0x4deab0 Input.setAxisSmooth (static): set_axis_smooth with power = 1.0.
// INVENTED: unresolved instance is a no-op; the exe would dereference null.
bool in_set_axis_smooth(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t l_axis;
    float center_range, center_factor, opposite, same;
    if (!want("Input.setAxisSmooth", a, 6, err) ||
        !get_i("Input.setAxisSmooth", a[1], &l_axis, err) ||
        !get_f("Input.setAxisSmooth", a[2], &center_range, err) ||
        !get_f("Input.setAxisSmooth", a[3], &center_factor, err) ||
        !get_f("Input.setAxisSmooth", a[4], &opposite, err) ||
        !get_f("Input.setAxisSmooth", a[5], &same, err))
        return false;
    if (input::Controller *c = controller_of(value_resource(a[0])))
        c->set_axis_smooth(l_axis, center_range, center_factor, opposite, same,
                           1.0f);
    *out = Value::make_null();
    return true;
}

// 0x4de6d0 Input.activeAxis
bool in_active_axis(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t d;
    if (!want("Input.activeAxis", a, 1, err) || !get_i("Input.activeAxis", a[0], &d, err))
        return false;
    *out = Value::make_int(input::devices().active_axis(d));
    return true;
}

// 0x4de700 Input.axisName
bool in_axis_name(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t d, i;
    if (!want("Input.axisName", a, 2, err) || !get_i("Input.axisName", a[0], &d, err) ||
        !get_i("Input.axisName", a[1], &i, err))
        return false;
    *out = Value::make_str(input::devices().axis_name(d, i));
    return true;
}

// 0x4de9f0 Input.lastKey (raw bytes; returns the packed (ansi << 16) | scan)
bool in_last_key(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Input.lastKey", a, 0, err)) return false;
    input::Backend *b = input::backend();
    // INVENTED: no keyboard backend means no key; the exe always has one.
    *out = Value::make_int(b ? (int32_t)input::hotkeys().last_key(*b) : 0);
    return true;
}

// ---- java.io.MouseCursor ---------------------------------------------------

// The exe reads the object's "cursor" field (a GameRef to the cursor resource);
// an unset field or a native-object read failure is its null path.
bool has_cursor(Vm &vm, Object *o)
{
    Value v;
    std::string e;
    return vm.get_instance_field(o, "cursor", &v, &e) && v.kind == Value::Obj &&
           v.obj;
}

// 0x4df270 MouseCursor.getPos
bool mc_get_pos(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("MouseCursor.getPos", a, 1, err) ||
        !get_o("MouseCursor.getPos", a[0], &o, err))
        return false;
    if (!has_cursor(vm, o)) {
        std::fprintf(stderr, "vm_mouse: Mighty Mouse ERROR 2\n");
        *out = Value::make_null();
        return true;
    }
    // INVENTED: no CursorSource is installed yet; the exe reads an uninitialized
    // info block when the cursor resource does not answer.
    float x = 0.0f, y = 0.0f;
    input::cursor_pos(&x, &y);
    float v[3] = {x, y, 0.0f};
    Object *p = new_point(vm, "java.lang.Vector3", v, false, err);
    if (!p) return false;
    *out = Value::make_obj(p);
    return true;
}

// 0x4df3b0 MouseCursor.getPos(Vector3 out): z is deliberately left untouched.
bool mc_get_pos_out(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o, *dst;
    if (!want("MouseCursor.getPos", a, 2, err) ||
        !get_o("MouseCursor.getPos", a[0], &o, err) ||
        !get_o("MouseCursor.getPos", a[1], &dst, err))
        return false;
    if (!has_cursor(vm, o)) {
        std::fprintf(stderr, "vm_mouse: Mighty Mouse ERROR 2\n");
        *out = Value::make_null();
        return true;
    }
    // INVENTED: zeros without a CursorSource (see mc_get_pos).
    float x = 0.0f, y = 0.0f;
    input::cursor_pos(&x, &y);
    if (!write_f(vm, dst, "x", x, err) || !write_f(vm, dst, "y", y, err))
        return false;
    *out = Value::make_obj(dst);
    return true;
}

// 0x4def10 MouseCursor.getPickedPos
bool mc_get_picked_pos(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o;
    if (!want("MouseCursor.getPickedPos", a, 1, err) ||
        !get_o("MouseCursor.getPickedPos", a[0], &o, err))
        return false;
    if (!has_cursor(vm, o)) {
        std::fprintf(stderr, "vm_mouse_getpickedpos: scripted resource not found\n");
        *out = Value::make_null();
        return true;
    }
    // INVENTED: zeros without a CursorSource (see mc_get_pos).
    float v[3] = {0.0f, 0.0f, 0.0f};
    input::cursor_picked_pos(v);
    Object *p = new_point(vm, "java.lang.Vector3", v, false, err);
    if (!p) return false;
    *out = Value::make_obj(p);
    return true;
}

// 0x4df100 MouseCursor.getPickedPos(Vector3 out)
bool mc_get_picked_pos_out(Vm &vm, const Args &a, Value *out, std::string *err)
{
    Object *o, *dst;
    if (!want("MouseCursor.getPickedPos", a, 2, err) ||
        !get_o("MouseCursor.getPickedPos", a[0], &o, err) ||
        !get_o("MouseCursor.getPickedPos", a[1], &dst, err))
        return false;
    if (!has_cursor(vm, o)) {
        std::fprintf(stderr, "vm_mouse_getpickedpos: scripted resource not found\n");
        *out = Value::make_null();
        return true;
    }
    // INVENTED: zeros without a CursorSource (see mc_get_pos).
    float v[3] = {0.0f, 0.0f, 0.0f};
    input::cursor_picked_pos(v);
    if (!write_f(vm, dst, "x", v[0], err) || !write_f(vm, dst, "y", v[1], err) ||
        !write_f(vm, dst, "z", v[2], err))
        return false;
    *out = Value::make_obj(dst);
    return true;
}

// ---- java.sound.Sound ------------------------------------------------------

// 0x4e48f0 Sound.changeMusicSet(int id)
bool snd_change_music_set(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t id;
    if (!want("Sound.changeMusicSet", a, 1, err) ||
        !get_i("Sound.changeMusicSet", a[0], &id, err))
        return false;
    audio::music_system().change_set(id);
    *out = Value::make_null();
    return true;
}

// 0x4e4b10 Sound.nextTrack() -> 0x568c70
bool snd_next_track(Vm &, const Args &, Value *out, std::string *)
{
    audio::music_system().next();
    *out = Value::make_null();
    return true;
}

// 0x4e4b20 Sound.prevTrack() -> 0x568ca0
bool snd_prev_track(Vm &, const Args &, Value *out, std::string *)
{
    audio::music_system().prev();
    *out = Value::make_null();
    return true;
}

// 0x4e4a30 Sound.firstTrack() -> 0x568b10
bool snd_first_track(Vm &, const Args &, Value *out, std::string *)
{
    audio::music_system().first();
    *out = Value::make_null();
    return true;
}

// 0x4e4b30 Sound.randomTrack() -> 0x568cd0
bool snd_random_track(Vm &, const Args &, Value *out, std::string *)
{
    audio::music_system().random();
    *out = Value::make_null();
    return true;
}

// 0x4e49b0 Sound.playTrack(int i) -> 0x568a50
bool snd_play_track_int(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t index;
    if (!want("Sound.playTrack", a, 1, err) ||
        !get_i("Sound.playTrack", a[0], &index, err))
        return false;
    audio::music_system().play_track(index);
    *out = Value::make_null();
    return true;
}

// 0x4e49e0 Sound.playTrack(String s) -> 0x568a90
bool snd_play_track_string(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Sound.playTrack", a, 1, err)) return false;
    if (a[0].kind == Value::Null) {
        // INVENTED: the exe passes a null C string to 0x568a90; use "" instead.
        audio::music_system().play_track(std::string());
    } else {
        const std::string *path;
        if (!get_s("Sound.playTrack", a[0], &path, err)) return false;
        audio::music_system().play_track(*path);
    }
    *out = Value::make_null();
    return true;
}

// 0x4e4a80 Sound.getTotalTracks() -> 0x568b50
bool snd_get_total_tracks(Vm &, const Args &, Value *out, std::string *)
{
    // INVENTED: exe returns an uninitialised value when no set is current.
    *out = Value::make_int(audio::music_system().count());
    return true;
}

// 0x4e4a60 Sound.getNowPlaying() -> 0x568b40
bool snd_get_now_playing(Vm &, const Args &, Value *out, std::string *)
{
    // INVENTED: exe returns an uninitialised value when no set is current.
    *out = Value::make_int(audio::music_system().track());
    return true;
}

// 0x4e4c00 Sound.stopMusic() -> 0x569090
bool snd_stop_music(Vm &, const Args &, Value *out, std::string *)
{
    audio::music_system().stop();
    *out = Value::make_null();
    return true;
}

// 0x4e4a40 Sound.getMode()
bool snd_get_mode(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_int(audio::music_system().mode());
    return true;
}

// 0x4e4b40 Sound.setMode(int m)
bool snd_set_mode(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t mode;
    if (!want("Sound.setMode", a, 1, err) ||
        !get_i("Sound.setMode", a[0], &mode, err))
        return false;
    *out = Value::make_int(audio::music_system().set_mode(mode));
    return true;
}

// 0x4e4b80 Sound.setVolume(int channel, float v)
bool snd_set_volume(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t channel;
    float v;
    if (!want("Sound.setVolume", a, 2, err) ||
        !get_i("Sound.setVolume", a[0], &channel, err) ||
        !get_f("Sound.setVolume", a[1], &v, err))
        return false;
    if ((uint32_t)channel >= 3) { // 0x4e4b80: other channels are ignored
        *out = Value::make_null();
        return true;
    }
    audio::channel_volumes()[channel] = v;
    if (channel == 1) {
        // DAT_005dada4 is read by change_set; keep both copies equal.
        audio::music_system().set_channel_volume(v);
        if (audio::music_system().current())
            audio::music_system().set_volume(v);
    }
    *out = Value::make_null();
    return true;
}

// 0x4e4aa0 Sound.getVolume(int channel) -> float
bool snd_get_volume(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t channel;
    if (!want("Sound.getVolume", a, 1, err) ||
        !get_i("Sound.getVolume", a[0], &channel, err))
        return false;
    const audio::ChannelVolumes &volumes = audio::channel_volumes();
    *out = Value::make_float(volumes[channel]);
    return true;
}

// 0x4e4af0 Sound.has3DHardware() -> 0x546be0
bool snd_has_3d_hardware(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_int(audio::has_3d_hardware() ? 1 : 0);
    return true;
}

// 0x4e4b00 Sound.hasMixHardware() -> 0x546bf0
bool snd_has_mix_hardware(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_int(audio::has_mix_hardware() ? 1 : 0);
    return true;
}

// 0x4e4a10 Sound.enableDebugDump(ResourceRef display) -> int
bool snd_enable_debug_dump(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Sound.enableDebugDump", a, 1, err)) return false;
    *out = Value::make_int(0);
    return true;
}

// DAT_005dadac, the exe's process-wide default text colour (same value as
// render::kDefaultTextColor in render/ui.h).
// shortcut: render::TextRenderer::default_color_ is not driven by it; no script
// text renderer is created yet.
uint32_t g_default_text_color = 0xffffffffu;

// java.render.GfxEngine display state (notes/12 §9): sorted mode list
// DAT_005eb450 (count DAT_005eb454, 0x14-byte entries) and C3DLow's current
// mode +0x1bc/+0x1be/+0x1c0 (i16), +0x1c2 windowed. Ctor 0x4223a0:
// 640x480x16, fullscreen.
struct GfxDisplayMode {
    int32_t width, height, depth;  // entry +0/+4/+8
};
// UNRESOLVED: display-mode list builder (DAT_005eb450/DAT_005eb454) not traced;
// notes/12 §9 names it "vf3" without a confirmed address. The list is empty,
// so num=0 / curr=0 / name="".
// UNRESOLVED: the last two dwords of the 0x14-byte entry are not read by these
// natives and were not identified.
// shortcut: no enumerator in src; the list stays empty until one exists.
std::vector<GfxDisplayMode> g_display_modes;
int16_t g_video_width = 640;    // +0x1bc
int16_t g_video_height = 480;   // +0x1be
int16_t g_video_depth = 16;     // +0x1c0
// shortcut: src never runs Game__initVideo (0x421280), so the current mode
// stays at the C3DLow ctor values and g_video_windowed stays false.
bool g_video_windowed = false;  // +0x1c2

// 0x4e5770 GfxEngine.numDisplayModes() -> 0x4264e0
bool gfx_num_display_modes(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_int((int32_t)g_display_modes.size());
    return true;
}

// 0x4e54f0 GfxEngine.currDisplayMode() -> 0x424110
bool gfx_curr_display_mode(Vm &, const Args &, Value *out, std::string *)
{
    int32_t i = 0;
    for (; i < (int32_t)g_display_modes.size(); i++) {
        const GfxDisplayMode &m = g_display_modes[(size_t)i];
        if (m.width == (int32_t)g_video_width &&
            m.height == (int32_t)g_video_height &&
            m.depth == (int32_t)g_video_depth)
            break;
    }
    if (i >= (int32_t)g_display_modes.size()) i = 0;  // no match -> 0
    *out = Value::make_int(i);
    return true;
}

// 0x4e5510 GfxEngine.displayModeName(int) -> 0x424210/0x55b980
bool gfx_display_mode_name(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t i;
    char buf[256];
    if (!want("GfxEngine.displayModeName", a, 1, err) ||
        !get_i("GfxEngine.displayModeName", a[0], &i, err))
        return false;
    if (i >= 0 && i < (int32_t)g_display_modes.size()) {
        const GfxDisplayMode &m = g_display_modes[(size_t)i];
        snprintf(buf, sizeof buf, "%d %d %d", m.width, m.height, m.depth);
    } else {
        buf[0] = 0;
    }
    *out = Value::make_str(buf);
    return true;
}

// 0x4e5480 GfxEngine.changeVideoMode(int,int,int) -> 0x422ec0
bool gfx_change_video_mode(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t w, h, depth;
    if (!want("GfxEngine.changeVideoMode", a, 3, err) ||
        !get_i("GfxEngine.changeVideoMode", a[0], &w, err) ||
        !get_i("GfxEngine.changeVideoMode", a[1], &h, err) ||
        !get_i("GfxEngine.changeVideoMode", a[2], &depth, err))
        return false;
    // 0x422ec0 early-outs: already current, then windowed is a no-op.
    if ((int32_t)g_video_width == w && (int32_t)g_video_height == h &&
        (int32_t)g_video_depth == depth) {
        *out = Value::make_null();
        return true;
    }
    if (g_video_windowed) {
        *out = Value::make_null();
        return true;
    }
    // shortcut: format selection (+0x17c/+0x19c/+0x1b5) and the viewport/window
    // writes (+0x1c4..+0x1e0, DAT_005e93c0..cc) are not modelled: src has no D3D
    // device/reset. The exe returns void, so the modelled effects are the two
    // guards plus the current width/height. +0x1c0 (depth) is not written by
    // the exe either.
    g_video_width = (int16_t)w;
    g_video_height = (int16_t)h;
    *out = Value::make_null();
    return true;
}

// 0x4ecfc0: receiver's "renderinst" field (RenderRef) -> its native handle.
Runtime::Handle *text_renderinst(Vm &vm, Object *self)
{
    Value v;
    std::string e;
    if (!vm.get_instance_field(self, "renderinst", &v, &e)) return nullptr;
    return value_handle(v);
}

// MSVC _stricmp in the C locale (0x57c11e): ASCII tolower, unsigned chars,
// difference at the first mismatch or at the terminating NUL.
int bone_stricmp(const std::string &a, const std::string &b)
{
    size_t i = 0;
    for (;; i++) {
        int ca = i < a.size() ? (unsigned char)a[i] : 0;
        int cb = i < b.size() ? (unsigned char)b[i] : 0;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
}

// 0x51b940 RenderInstData__boneId: find the bone by name or create one at the
// midpoint of the neighbouring ids. First bone of an instance = 0x8000.
uint32_t bone_id(Runtime::Resource *t, const std::string &name)
{
    uint32_t lo = 1, hi = 0xffff;
    std::vector<Runtime::Resource::Bone> &bones = t->bones;
    size_t at = bones.size();  // append unless an insert-before is found
    for (size_t i = 0; i < bones.size(); i++) {
        int c = bone_stricmp(bones[i].name, name);
        if (c == 0) return bones[i].id;
        if (c > 0) {
            hi = bones[i].id;
            at = i;
            break;
        }
        lo = bones[i].id;
    }
    uint32_t id = (lo + hi) >> 1;
    Runtime::Resource::Bone b;
    b.id = id;
    b.name = name.substr(0, 31);
    bones.insert(bones.begin() + at, b);
    t->text_flags |= 0x1800;  // T+0x16c
    return id;
}

// 0x51c450 RenderInstData__findBone: ids ascend in list order.
Runtime::Resource::Bone *find_bone(Runtime::Resource *t, uint32_t id)
{
    for (auto &b : t->bones) {
        if (b.id == id) return &b;
        if (b.id > id) return nullptr;
    }
    return nullptr;
}

// 0x4e77f0 RenderInst__getBoneId: no handle id (+8) check in the exe.
// INVENTED: null-handle safety; the exe dereferences it.
uint32_t ri_get_bone_id(Runtime::Handle *h, const std::string &name)
{
    return (h && h->target) ? bone_id(h->target, name) : 0;
}

// 0x4ec470 RenderInst__setBoneMatrix with an identity 3x3 rotation: the local
// matrix is identity plus translation pos*10 (Matrix4__fromRotPosX10 0x567570).
void ri_set_bone_pos(Runtime::Handle *h, uint32_t id, const float pos[3])
{
    if (!h || h->id == 0 || !h->target) return;
    if (auto *b = find_bone(h->target, id)) {
        float *m = b->local;
        m[0] = 1; m[1] = 0; m[2] = 0; m[3] = 0;
        m[4] = 0; m[5] = 1; m[6] = 0; m[7] = 0;
        m[8] = 0; m[9] = 0; m[10] = 1; m[11] = 0;
        m[12] = pos[0] * 10.0f;
        m[13] = pos[1] * 10.0f;
        m[14] = pos[2] * 10.0f;
        m[15] = 1;
    }
    // shortcut: bone_ref (0x4e84c0, always 0 from these callers), the dirty
    // list, n+0x4c (identity bit), n+0x100 and T+0x1c0 = DAT_005dadbc are not
    // modelled.
}

// 0x4ea7f0 RenderInst__getBoneMatrix.
bool ri_get_bone_matrix(Runtime::Handle *h, uint32_t id, float out[16])
{
    if (!h || h->id == 0) return false;
    // INVENTED: the exe returns 1 with out untouched when the typed object is
    // missing; the rebuild reports the failure instead.
    if (!h->target) return false;
    Runtime::Resource::Bone *b = find_bone(h->target, id);
    if (!b) return false;
    for (int i = 0; i < 16; i++) out[i] = b->local[i];
    return true;
}

// 0x4ec980 over a Vector3 {x,y,z} or Ypr {y,p,r}: a missing argument or field
// yields 0.0f; read through dd_get_f, which never fails.
static void get3(Vm &vm, const Value &v, const char *a, const char *b,
                 const char *c, float out[3])
{
    Object *o = v.kind == Value::Obj ? v.obj : nullptr;  // INVENTED: non-object -> null
    out[0] = o ? dd_get_f(vm, o, a) : 0.0f;
    out[1] = o ? dd_get_f(vm, o, b) : 0.0f;
    out[2] = o ? dd_get_f(vm, o, c) : 0.0f;
}

// 0x567570 Matrix4__fromRotPosX10 (ECX = m): transposed 3x3 of rot,
// translation pos*10, w = 1.
void mat_from_rot_pos_x10(float m[16], const Mat34 &rot, const float pos[3])
{
    m[0] = rot.m[0]; m[1] = rot.m[4]; m[2]  = rot.m[8];  m[3]  = 0.0f;
    m[4] = rot.m[1]; m[5] = rot.m[5]; m[6]  = rot.m[9];  m[7]  = 0.0f;
    m[8] = rot.m[2]; m[9] = rot.m[6]; m[10] = rot.m[10]; m[11] = 0.0f;
    m[12] = pos[0] * 10.0f; m[13] = pos[1] * 10.0f;
    m[14] = pos[2] * 10.0f; m[15] = 1.0f;
}

// 0x4ec3b0 RenderInst__setBoneMatrix: store the local matrix and mark the
// typed data dirty (0x4ec0f0 -> +0x16c |= 0x1800).
// shortcut: 0x4e84c0 bone_ref and the typed+0x1c0 frame counter are not
// modelled (same as ri_set_bone_matrix_ypr).
void ri_set_bone_matrix(Runtime::Handle *h, uint32_t id, const float m[16])
{
    if (!h || h->id == 0 || !h->target) return;
    if (auto *b = find_bone(h->target, id)) {
        for (int i = 0; i < 16; i++) b->local[i] = m[i];
        h->target->text_flags |= 0x1800;
    }
}

// 0x4ec540 RenderInst__setBoneMatrixYpr: bone `id`'s local matrix from ypr
// (y,p,r, same order as the exe's stack vector 0x569b40) and pos (x10).
// INVENTED: the typed data T is the bound Runtime::Resource.
void ri_set_bone_matrix_ypr(Runtime::Handle *h, uint32_t id,
                            const float pos[3], const float ypr[3])
{
    if (!h || h->id == 0 || !h->target) return;   // exe: handle+8 == 0 -> 0
    Runtime::Resource *t = h->target;
    Runtime::Resource::Bone *b = find_bone(t, id);
    if (!b) return;
    Mat34 rot = {};                    // 0x5666d0 (euler_to_mat)
    euler_to_mat(&rot, ypr);
    mat_from_rot_pos_x10(b->local, rot, pos);
    t->text_flags |= 0x1800;           // 0x4ec0f0, typed data +0x16c
    // shortcut: 0x4ec0f0's dirty-list move (T+0x260/T+0x270), n+0x4c
    // identity bit, n+0x100, the T+0x1c0 frame counter and the 0x4e84c0
    // bone_ref attach are unmodelled; src has no render side reading them.
}

// 0x4e5cd0 java.render.Text.setDefaultColor: DAT_005dadac = color. The exe does
// not read the receiver (FUN_004b1af0(ctx, 0, &color)).
bool tx_set_default_color(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t color;
    if (!want("Text.setDefaultColor", a, 2, err) ||
        !get_i("Text.setDefaultColor", a[1], &color, err))
        return false;
    g_default_text_color = (uint32_t)color;
    *out = Value::make_null();
    return true;
}

// 0x4e58d0 java.render.Text.getDefaultColor: MOV EAX,[DAT_005dadac]; RET.
bool tx_get_default_color(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_int((int32_t)g_default_text_color);
    return true;
}

// 0x4e5c40 java.render.Text.changeColor(I)V
// INVENTED: typed data T+0x17c mapped onto the bound Runtime::Resource.
bool tx_change_color(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Text.changeColor", err)) return false;
    Object *o;
    int32_t color;
    if (!receiver_object("Text.changeColor", a, &o, err) ||
        !want("Text.changeColor", a, 2, err) ||
        !get_i("Text.changeColor", a[1], &color, err))
        return false;
    Runtime::Handle *h = text_renderinst(vm, o);
    Runtime::Resource *r = h ? h->target : nullptr;
    if (r) r->text_color = (uint32_t)color;
    *out = Value::make_null();
    return true;
}

// 0x4e5bb0 java.render.Text.changeAlign(I)V
// INVENTED: typed data T+0x174 mapped onto the bound Runtime::Resource.
bool tx_change_align(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Text.changeAlign", err)) return false;
    Object *o;
    int32_t align;
    if (!receiver_object("Text.changeAlign", a, &o, err) ||
        !want("Text.changeAlign", a, 2, err) ||
        !get_i("Text.changeAlign", a[1], &align, err))
        return false;
    Runtime::Handle *h = text_renderinst(vm, o);
    Runtime::Resource *r = h ? h->target : nullptr;
    if (r) r->text_align = align;
    *out = Value::make_null();
    return true;
}

// 0x4e5d80 java.render.Text.setScale(FF)V
// shortcut: 0x4e5d80 scales the render nodes via T+0x114 (0x4d5f50/0x4d8710); src has no
// such link, so this is the exe's no-link path (no effect).
bool tx_set_scale(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Text.setScale", err)) return false;
    Object *o;
    float sx, sy;
    if (!receiver_object("Text.setScale", a, &o, err) ||
        !want("Text.setScale", a, 3, err) ||
        !get_f("Text.setScale", a[1], &sx, err) ||
        !get_f("Text.setScale", a[2], &sy, err))
        return false;
    (void)sx;
    (void)sy;
    *out = Value::make_null();
    return true;
}

// ---- java.render.GfxEngine: video, flush, envmap (notes/12 §2, §9) --------

// DAT_005ef0d4: 4-argument openVideo's extra option (0x44ef50 stores it).
// shortcut: only the DirectShow paths read it; kept to match 0x4dc8b0's write.
int32_t g_video_option = 0;

// 0x4dc910 GfxEngine.setGlobalEnvmap(ResourceRef)V
bool gfx_set_global_envmap(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GfxEngine.setGlobalEnvmap", err)) return false;
    if (!want("GfxEngine.setGlobalEnvmap", a, 1, err)) return false;
    // static native: the declared argument is a[0] (getArgs(ctx, 0, &arg) at 0x4dc91d)
    Runtime::Handle *src = value_handle(a[0]);   // arg ref node +0xc
    // INVENTED: a Java-null argument faults in the exe (no check at
    // 0x4dc92e); here it clears the slot like an unbound ref node.
    game_rt()->bind(&game_rt()->global_envmap, src ? src->target : nullptr);
    *out = Value::make_null();
    return true;
}

// 0x4dc840 GfxEngine.flush()V
// exe: forceRendering(0) [0x4e5560: 7x {0x517f60, 0x518f80, DAT_005e8ca0=0,
// Game__renderFrame 0x421920, DAT_005dadbc++}], resource-manager flush
// 0x5176a0(1) on DAT_005db008, then 0x422e40 (three per-frame subsystem
// updates on [0x5e938c]+0x33c/+0x320/+0x324).
// shortcut: src has no render frame or resource-manager flush host.
bool gfx_flush(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_null();
    return true;
}

// 0x4e5560 GfxEngine.forceRendering()V
// exe: 7x {0x517f60 streaming tick, 0x518f80 manager list tick,
// DAT_005e8ca0 = 0, Game__renderFrame 0x421920, DAT_005dadbc++}.
// shortcut: src has no render-frame/resource-streaming host; the 7x loop has
// nothing to observe (same no-service path as flush 0x4dc840).
bool gfx_force_rendering(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_null();
    return true;
}

// 0x4e5780 GfxEngine.printScreen(String)V -> 0x421b40 Game__printScreen
// SERVICE MISSING: 0x421b40 locks a copy of the backbuffer (C3DLow vf[0x50]),
// converts the 4-byte pixels to 3-byte RGB, compresses (0x527060) and writes; src has no
// backbuffer readback (render::Device) and no image writer. Void native, so the
// observable result is the exe's null/empty-name and lock-failure path:
// nothing is written.
bool gfx_print_screen(Vm &, const Args &a, Value *out, std::string *err)
{
    // static native: the declared argument is a[0] (getArgs(ctx, 0, &name) at 0x4e5780)
    if (!want("GfxEngine.printScreen", a, 1, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4dc900 GfxEngine.renderOK()I: movzx eax, byte [0x5fe0f8]; ret.
// DAT_005fe0f8 is the result of the last Game__renderFrame, stored by
// Game__renderAndMeasure at 0x47b5cd; 0 before the first frame.
// shortcut: src never runs a render frame, so this stays 0.
bool gfx_render_ok(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_int(0);
    return true;
}

// 0x4dc870 GfxEngine.openVideo(String,II)I
bool gfx_open_video(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GfxEngine.openVideo", a, 3, err)) return false;
    if (a[0].kind == Value::Null) {           // 0x4dc882: null name -> -1
        *out = Value::make_int(-1);
        return true;
    }
    // SERVICE MISSING: Video__open 0x44e9b0 returns -0x7fffbffb (0x80004005,
    // E_FAIL) when Video__init 0x44e910 has not set DAT_005ef0c8. The
    // rebuild never runs Video__init (no COM/DirectShow host).
    *out = Value::make_int((int32_t)0x80004005u);
    return true;
}

// 0x4dc8b0 GfxEngine.openVideo(String,III)I
bool gfx_open_video_ex(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GfxEngine.openVideo", a, 4, err)) return false;
    if (a[0].kind == Value::Null) {
        *out = Value::make_int(-1);
        return true;
    }
    int32_t opt;
    if (!get_i("GfxEngine.openVideo", a[3], &opt, err)) return false;
    g_video_option = opt;                     // 0x44ef50 Video__setLoop
    *out = Value::make_int((int32_t)0x80004005u);
    return true;
}

// 0x4dc830 GfxEngine.closeVideo()V: jmp 0x44e780 (Video__close).
// shortcut: Video__close's whole body is guarded by DAT_005ef0c8, which the
// rebuild never sets, so there is nothing to release.
bool gfx_close_video(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_null();
    return true;
}

// 0x4dc860 GfxEngine.isPlayingVideo()I: jmp 0x44e9a0 = mov eax,[0x5ef0cc]; ret.
// DAT_005ef0cc is set to 1 only by Video__start 0x44edc0 and cleared by
// Video__close; neither is reachable without a video backend.
bool gfx_is_playing_video(Vm &, const Args &, Value *out, std::string *)
{
    *out = Value::make_int(0);
    return true;
}

// 0x4e5b30 java.render.Text.create(ResourceRef parent, ResourceRef charset, float x, float y)
// INVENTED: the text state lives on the bound Runtime::Resource; the exe keeps it
// in the render instance's typed data block.
// shortcut: the bounds reset (+0xb0/+0xb4..+0xc8) is not modelled.
// UNRESOLVED: FUN_004e8f40's createInstance internals (0x5188e0/0x518880),
// FUN_0051f4f0 (typed-data mask).
bool tx_create(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Text.create", err)) return false;
    Object *o;
    if (!receiver_object("Text.create", a, &o, err)) return false;
    float x, y;
    if (!want("Text.create", a, 5, err) ||
        !get_f("Text.create", a[3], &x, err) ||
        !get_f("Text.create", a[4], &y, err))
        return false;
    Runtime::Handle *font = value_handle(a[2]);
    // 0x4e8f54: charset handle +8 (id) == 0 -> nothing done (exe faults on the
    // null handle too, safety here is INVENTED).
    if (!font || font->id == 0) {
        *out = Value::make_null();
        return true;
    }
    Runtime::Handle *h = text_renderinst(vm, o);
    if (!h) {  // INVENTED safety (exe would fault)
        *out = Value::make_null();
        return true;
    }
    Runtime::Resource *r = game_rt()->instantiate(nullptr, value_resource(a[1]), "r_text",
                                                  std::string(), false, err);
    if (!r) return false;
    r->type = 3;        // INSTANCE_RENDER (exe creates the render instance)
    r->flags |= 0x200;  // instance->+0x84
    game_rt()->bind(h, r);  // 0x40cac0
    r->text_flags |= 0x8000;
    r->text_data.clear();
    r->text_align = 2;
    r->text_scale = 1.0f;
    r->text_color = g_default_text_color;
    const float p[3] = {x, y, 0.0f};
    ri_set_bone_pos(h, ri_get_bone_id(h, "bone00"), p);  // 0x4e8f40 bone00
    *out = Value::make_null();
    return true;
}

// 0x4e5eb0 java.render.Text.update()V: copy the script's "text" String into the
// instance string slot (+0x170); "" when the field is missing/null/not a String.
// INVENTED: stored on the bound Runtime::Resource (text_data), not typed data.
bool tx_update(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Text.update", err)) return false;
    Object *o;
    if (!receiver_object("Text.update", a, &o, err) || !want("Text.update", a, 1, err))
        return false;
    Runtime::Handle *h = text_renderinst(vm, o);
    Runtime::Resource *r = h ? h->target : nullptr;
    if (r) {
        Value v;
        std::string e;
        if (vm.get_instance_field(o, "text", &v, &e) && v.kind == Value::Str)
            r->text_data = v.s;
        else
            r->text_data.clear();
    }
    *out = Value::make_null();
    return true;
}

// 0x4e5ae0 Text.getWidthPixels(String, ResourceRef)F
// SERVICE MISSING: the exe measures with the D3D text object (0x421100: vf131
// setFont, vf134 scale 1.0, vf133 textWidth(str, 1.0)). src has no font-atlas
// loader (notes/12 §4), so every path returns the exe's no-service 0.0f
// (null String, font id 0, no service, atlas flag +0x38 bit 3 clear).
bool tx_get_width_pixels(Vm &, const Args &a, Value *out, std::string *err)
{
    // static native: the declared arguments start at a[0] (getArgs(ctx, 0, &str, &font))
    if (!want("Text.getWidthPixels", a, 2, err)) return false;
    if (a[0].kind != Value::Str) {  // 0x4e5ae0: null String -> 0.0f
        // INVENTED: a non-Str slot stands for the exe's null String;
        // every other path also yields 0.0f while the text service is missing.
        *out = Value::make_float(0.0f);
        return true;
    }
    *out = Value::make_float(0.0f);
    return true;
}

// 0x4e5d00 java.render.Text.setPos(FF)V: bone00 translation, stored x10.
bool tx_set_pos(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Text.setPos", err)) return false;
    Object *o;
    if (!receiver_object("Text.setPos", a, &o, err)) return false;
    float x, y;
    if (!want("Text.setPos", a, 3, err) ||
        !get_f("Text.setPos", a[1], &x, err) ||
        !get_f("Text.setPos", a[2], &y, err))
        return false;
    Runtime::Handle *h = text_renderinst(vm, o);
    float p[3] = {x, y, 0.0f};
    ri_set_bone_pos(h, ri_get_bone_id(h, "bone00"), p);
    *out = Value::make_null();
    return true;
}

// 0x4e58e0 java.render.Text.getPos()Ljava.lang.Vector3;
bool tx_get_pos(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Text.getPos", err)) return false;
    Object *o;
    if (!receiver_object("Text.getPos", a, &o, err)) return false;
    Runtime::Handle *h = text_renderinst(vm, o);
    float m[16];
    if (!ri_get_bone_matrix(h, ri_get_bone_id(h, "bone00"), m)) {
        *out = Value::make_null();
        return true;
    }
    float v[3] = {m[12] * 0.1f, m[13] * 0.1f, m[14] * 0.1f};
    Object *p = new_point(vm, "java.lang.Vector3", v, false, err);
    if (!p) return false;
    *out = Value::make_obj(p);
    return true;
}

// 0x4e5a20 java.render.Text.getPos(Ljava.lang.Vector3;)Ljava.lang.Vector3;
bool tx_get_pos_into(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Text.getPos", err)) return false;
    Object *o;
    if (!receiver_object("Text.getPos", a, &o, err) || !want("Text.getPos", a, 2, err))
        return false;
    Runtime::Handle *h = text_renderinst(vm, o);
    float m[16];
    if (!ri_get_bone_matrix(h, ri_get_bone_id(h, "bone00"), m)) {
        *out = Value::make_null();
        return true;
    }
    float v[3] = {m[12] * 0.1f, m[13] * 0.1f, m[14] * 0.1f};
    // INVENTED: the exe writes the fields of a null Java object (faults).
    if (a[1].kind != Value::Obj || !a[1].obj) {
        *out = Value::make_null();
        return true;
    }
    if (!vec_write(vm, a[1].obj, v, err)) return false;
    *out = a[1];
    return true;
}

// 0x4e2290 RenderRef.getBoneId(Ljava.lang.String;)I
bool rr_get_bone_id(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("RenderRef.getBoneId", err)) return false;
    Object *o;
    const std::string *name;
    if (!want("RenderRef.getBoneId", a, 2, err) ||
        !get_o("RenderRef.getBoneId", a[0], &o, err) ||
        !get_s("RenderRef.getBoneId", a[1], &name, err))
        return false;
    Runtime::Handle *h = game_rt()->handle(o);  // 0x4ecc70
    if (!h) {
        // INVENTED: the exe appends the handle, file and line; a NativeFn has no frame.
        std::fprintf(stderr, "vm_renderref_getboneid: scripted resource not found\n");
        *out = Value::make_int(0);
        return true;
    }
    *out = Value::make_int((int32_t)ri_get_bone_id(h, *name));
    return true;
}

// 0x4e2140 RenderRef.create(ResourceRef parent, RenderRef type, String name)V
bool rr_create(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("RenderRef.create", err)) return false;
    Object *o;
    if (!receiver_object("RenderRef.create", a, &o, err) ||
        !want("RenderRef.create", a, 4, err))
        return false;
    Runtime::Handle *h = game_rt()->handle(o);
    Runtime::Handle *parent = value_handle(a[1]);
    Runtime::Handle *type = value_handle(a[2]);
    *out = Value::make_null();
    if (!h || !parent || !type) {
        // INVENTED: the exe appends the id, file and line (no frame here).
        std::fprintf(stderr, "vm_renderref_create: scripted resource not found\n");
        return true;
    }
    // parent handle unbound: the exe returns silently (no log). Type unbound
    // would fault in 0x51e090; INVENTED safety treats it the same as unbound.
    if (!parent->target || !type->target) return true;
    std::string name = a[3].kind == Value::Str ? a[3].s : std::string("_renderinst");
    Runtime::Resource *r = game_rt()->instantiate(type->target, parent->target,
                                                  name, "", false, err);
    if (!r) return false;
    r->type = 3;        // 0x524840 descriptor kind 3 (ins_render)
    r->flags |= 0x200;  // RI+0x84
    game_rt()->bind(h, r);           // 0x40cac0 (ECX = self handle)
    ri_get_bone_id(h, "bone00");     // 0x4e77f0 -> 0x51b940, id 0x8000
    return true;
}

// 0x4e2c80 RenderRef.setMatrix(Vector3 pos, Ypr ori)V
bool rr_set_matrix(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("RenderRef.setMatrix", err)) return false;
    Object *o;
    if (!receiver_object("RenderRef.setMatrix", a, &o, err) ||
        !want("RenderRef.setMatrix", a, 3, err))
        return false;
    Runtime::Handle *h = game_rt()->handle(o);
    *out = Value::make_null();
    if (!h) {
        // INVENTED: the exe appends the id, file and line.
        std::fprintf(stderr, "vm_renderref_setmatrix_d: scripted resource not found\n");
        return true;
    }
    float pos[3], ypr[3];
    get3(vm, a[1], "x", "y", "z", pos);
    get3(vm, a[2], "y", "p", "r", ypr);
    ri_set_bone_matrix_ypr(h, ri_get_bone_id(h, "bone00"), pos, ypr);
    return true;
}

// 0x4e2a50 RenderRef.setMatrix(int bone, ResourceRef boneRef, Vector3 pos, Ypr ori)V
bool rr_set_matrix_bone(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("RenderRef.setMatrix", err)) return false;
    Object *o;
    int32_t bone;
    if (!receiver_object("RenderRef.setMatrix", a, &o, err) ||
        !want("RenderRef.setMatrix", a, 5, err) ||
        !get_i("RenderRef.setMatrix", a[1], &bone, err))
        return false;
    Runtime::Handle *h = game_rt()->handle(o);
    *out = Value::make_null();
    if (!h) {
        // INVENTED: the exe appends the id, file and line.
        std::fprintf(stderr, "vm_renderref_setmatrix: scripted resource not found\n");
        return true;
    }
    float pos[3], ypr[3];
    get3(vm, a[3], "x", "y", "z", pos);
    get3(vm, a[4], "y", "p", "r", ypr);
    // a[2] (boneRef) only feeds 0x4e84c0, whose effects are unmodelled.
    ri_set_bone_matrix_ypr(h, (uint32_t)bone, pos, ypr);
    return true;
}

// 0x4e25d0 RenderRef.lineCreate(ResourceRef parent, ResourceRef type)I
// 0x4e8ca0 creates render instance "line" (0x524840/0x40cac0/0x468b00/0x51bb30)
// and stores the 0x15c-byte strip at typed+0x1c; the type ref's factory lives at
// typed+0x2c. The exe returns 0 whenever any of those guards fails, which is
// always here.
// SERVICE MISSING: no render typed-data layer, no line factory, no instance.
bool rr_line_create(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("RenderRef.lineCreate", a, 3, err)) return false;
    *out = Value::make_int(0);
    return true;
}

// 0x4e24f0 RenderRef.lineAdd(Vector3 p, Vector3 dir, I colour, F width)V
// 0x4ec6a0 needs the receiver's typed data (T+0x1c line object created by
// lineCreate) and calls 0x478690 to append a ribbon segment. Both are absent.
// SERVICE MISSING: T+0x1c / 0x478690.
bool rr_line_add(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("RenderRef.lineAdd", a, 5, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4e2640 RenderRef.plotRoute(ResourceRef a, ResourceRef b, I colour, F density,
// Vector3 scale)I
// Samples the process route (0x5fe450 path, 0x5fe454 len, 0x5fe458 s, 0x5fe45c e)
// and feeds each sample to 0x4ec6a0. src models no route (findRoute keeps no
// globals), so the exe's first check, DAT_005fe450 == 0, always returns 0.
// SERVICE MISSING: process route globals and the line service above.
bool rr_plot_route(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("RenderRef.plotRoute", a, 6, err)) return false;
    *out = Value::make_int(0);
    return true;
}

// 0x4e29f0 RenderRef.setLight(III)V
bool rr_set_light(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("RenderRef.setLight", err)) return false;
    Object *o;
    int32_t c[3];
    if (!receiver_object("RenderRef.setLight", a, &o, err) ||
        !want("RenderRef.setLight", a, 4, err) ||
        !get_i("RenderRef.setLight", a[1], &c[0], err) ||
        !get_i("RenderRef.setLight", a[2], &c[1], err) ||
        !get_i("RenderRef.setLight", a[3], &c[2], err))
        return false;
    *out = Value::make_null();
    Runtime::Resource *r = value_resource(a[0]);
    if (!r) return true;      // INVENTED: the exe dereferences a null handle
    static const int sh[3] = {16, 8, 0};
    float *dst[3] = {r->light_c0, r->light_c1, r->light_c2};
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < 3; ++i)
            dst[k][i] = (float)((c[k] >> sh[i]) & 0xff) * 0.00390625f;
    // shortcut: resident/typed fetch (0x51f4f0) and the dead extra-arg path
    // (+0x4c/+0x54, always 0 from 0x4e29f0) are collapsed.
    return true;
}

// 0x4e2950 RenderRef.setFlare(ResourceRef,IFFII)V
bool rr_set_flare(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("RenderRef.setFlare", err)) return false;
    Object *o;
    int32_t i1, i2, i3;
    float f1, f2;
    if (!receiver_object("RenderRef.setFlare", a, &o, err) ||
        !want("RenderRef.setFlare", a, 7, err) ||
        !get_i("RenderRef.setFlare", a[2], &i1, err) ||
        !get_f("RenderRef.setFlare", a[3], &f1, err) ||
        !get_f("RenderRef.setFlare", a[4], &f2, err) ||
        !get_i("RenderRef.setFlare", a[5], &i2, err) ||
        !get_i("RenderRef.setFlare", a[6], &i3, err))
        return false;
    *out = Value::make_null();
    Runtime::Resource *r = value_resource(a[0]);
    if (!r) return true;      // INVENTED: the exe dereferences a null handle
    Runtime::Handle *tex = value_handle(a[1]);
    r->flare_texture = tex ? tex->target : nullptr;   // 0x4ebe00 T+0xc4 link
    r->flare_f0 = f1;         // +0xd4
    r->flare_f1 = f2;         // +0xd8
    r->flare_i0 = i1;         // +0xe0
    r->flare_i1 = i2;         // +0x100
    r->flare_i2 = i3;         // +0xe4
    // shortcut: the stack RefNode used only as a lifetime anchor, the resident
    // call and the 0x51f4f0 fetch are collapsed.
    return true;
}

// 0x4e2eb0 RenderRef.setType(ResourceRef)V
bool rr_set_type(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("RenderRef.setType", err)) return false;
    Object *o;
    if (!receiver_object("RenderRef.setType", a, &o, err) ||
        !want("RenderRef.setType", a, 2, err))
        return false;
    *out = Value::make_null();
    Runtime::Resource *r = value_resource(a[0]);
    if (!r) return true;      // INVENTED: the exe dereferences a null handle
    Runtime::Handle *ref = value_handle(a[1]);
    uint32_t id = ref ? ref->id : 0;
    if (id != r->type_resource_id) {          // 0x51e090
        r->type_resource = ref ? ref->target : nullptr;
        r->type_resource_id = id;
        r->type_relinked = true;
    }
    // shortcut: the resident call and the 0x51f4f0 fetch are collapsed.
    return true;
}

// 0x4e2090 RenderRef.changeResource(ResourceRef,ResourceRef)V
bool rr_change_resource(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("RenderRef.changeResource", err)) return false;
    Object *o;
    if (!receiver_object("RenderRef.changeResource", a, &o, err) ||
        !want("RenderRef.changeResource", a, 3, err))
        return false;
    *out = Value::make_null();
    // 0x4e2090 -> T->vf[+8](a, b); the render-instance data vtable 0x5a7ec0
    // slot +8 is 0x46acd0 (XOR EAX,EAX; RET 8): both refs are ignored.
    // shortcut: resident/typed-fetch side effects collapse to nothing in src.
    return true;
}

// ---- java.render.Viewport lifecycle (0x4e6af0 / 0x4e6ab0 / 0x4e6c50 / 0x4e6c90)

// 0x4e6af0 Viewport.create(IFFFF)V: unwrap the receiver, build a type-0x12
// "_viewport" resource (0x525120 -> 0x5188e0) and rebind the same handle at
// 0x4e6b6b (FUN_0040cac0).
// INVENTED: depth and rect are stored on Runtime::Resource; the exe keeps them in
// the res_viewport typed data block.
// INVENTED: a missing handle returns null and creates nothing; the exe logs
// "vm_viewport_create: scripted resource %x not found" and returns.
// shortcut: the game viewport lists and the FUN_00420970 registration in 0x525120
// are not modelled; src has no game viewport list.
// UNRESOLVED: 0x5188e0 / 0x518a40 creation internals; the node is allocated from
// Runtime::instances_ instead.
bool vp_create(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Viewport.create", err)) return false;
    Object *o;
    if (!receiver_object("Viewport.create", a, &o, err)) return false;
    int32_t depth;
    float rect[4];
    if (!want("Viewport.create", a, 6, err) ||
        !get_i("Viewport.create", a[1], &depth, err) ||
        !get_f("Viewport.create", a[2], &rect[0], err) ||
        !get_f("Viewport.create", a[3], &rect[1], err) ||
        !get_f("Viewport.create", a[4], &rect[2], err) ||
        !get_f("Viewport.create", a[5], &rect[3], err))
        return false;
    Runtime::Handle *h = game_rt()->handle(o);  // 0x4ecc70
    if (!h) {
        *out = Value::make_null();
        return true;
    }
    Runtime::Resource *r = game_rt()->create_resource(0x12, "_viewport");
    r->viewport_depth = depth;
    for (int i = 0; i < 4; i++) r->viewport_rect[i] = rect[i];
    game_rt()->bind(h, r);  // 0x40cac0
    *out = Value::make_null();
    return true;
}

// 0x4e6ab0 Viewport.activate(I)V: the int argument is never read. 0x4200d0 checks
// res->type == 0x12 and moves the node to the drawn list game+0x14c.
// INVENTED: modelled as Resource::viewport_active; the game lists are not
// modelled. UNRESOLVED: FUN_00421e40 body.
bool vp_activate(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Viewport.activate", err)) return false;
    Object *o;
    if (!receiver_object("Viewport.activate", a, &o, err) ||
        !want("Viewport.activate", a, 2, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (r && r->type == 0x12) r->viewport_active = true;
    *out = Value::make_null();
    return true;
}

// 0x4e6c50 Viewport.deactivate()V -> 0x420b20 type-0x12 case (0x421f50 removes the
// node from game+0x14c).
// INVENTED: modelled as Resource::viewport_active; the game lists are not
// modelled. UNRESOLVED: FUN_00421f50 body.
bool vp_deactivate(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Viewport.deactivate", err)) return false;
    Object *o;
    if (!receiver_object("Viewport.deactivate", a, &o, err) ||
        !want("Viewport.deactivate", a, 1, err))
        return false;
    Runtime::Resource *r = value_resource(a[0]);
    if (r && r->type == 0x12) r->viewport_active = false;
    *out = Value::make_null();
    return true;
}

// 0x4e6c90 Viewport.destroy()V: 0x420b20 (deactivate) then 0x4e9450 queues the
// handle on the delete list 0x5fde90 (0x47b970).
// INVENTED: missing handle returns without logging (the exe logs "scripted
// resource %x not found" and does nothing).
bool vp_destroy(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Viewport.destroy", err)) return false;
    Object *o;
    if (!receiver_object("Viewport.destroy", a, &o, err) ||
        !want("Viewport.destroy", a, 1, err))
        return false;
    Runtime::Handle *h = game_rt()->handle(o);  // 0x4ecc70
    Runtime::Resource *r = h ? h->target : nullptr;
    if (r && r->type == 0x12) r->viewport_active = false;  // 0x420b20
    if (h && h->id && h->target) game_rt()->queue_delete(h);  // 0x4e9450
    *out = Value::make_null();
    return true;
}

// ---- java.render.Viewport accessors (0x4e6db0 / 0x4e6ee0 / 0x4e6df0 /
// 0x4e6e90 / 0x4e6e40 / 0x4e6f30)

// Returns the bound type-0x12 resource's rect, or nullptr; the exe's typed-data
// guard (FUN_00421610) yields no block for any other resource type.
static Runtime::Resource *viewport_resource(const Value &receiver)
{
    Runtime::Resource *r = value_resource(receiver);
    return r && r->type == 0x12 ? r : nullptr;
}

// 0x4e6db0 Viewport.getAspect: exe = backbuffer w / backbuffer h (FUN_004210f0,
// C3DLow vtable+0x64). shortcut: no backbuffer-size service in src/; 1.0f is the
// exe's own no-handle / no-typed-data value (0x4e6de9 / 0x4e9ab0 FLD1). INVENTED:
// the ratio is not computed even when a resource is bound.
bool vp_get_aspect(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Viewport.getAspect", err)) return false;
    Object *o;
    if (!receiver_object("Viewport.getAspect", a, &o, err)) return false;
    Runtime::Resource *r = viewport_resource(a[0]);
    (void)r;
    *out = Value::make_float(1.0f);
    return true;
}

// 0x4e6ee0 Viewport.getWidth: exe returns typed viewport data +0x18.
// INVENTED: missing handle/typed data returns 0.0f; the exe's fallback is the raw
// receiver slot (0x4e6f1f FLD [EBP-4]), which has no rebuild equivalent.
bool vp_get_width(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Viewport.getWidth", err)) return false;
    Object *o;
    if (!receiver_object("Viewport.getWidth", a, &o, err)) return false;
    Runtime::Resource *r = viewport_resource(a[0]);
    *out = Value::make_float(r ? r->viewport_rect[2] : 0.0f);
    return true;
}

// 0x4e6df0 Viewport.getHeight: exe returns typed viewport data +0x1c.
// INVENTED: missing handle/typed data returns 0.0f; the exe's fallback is the raw
// receiver slot, which has no rebuild equivalent.
bool vp_get_height(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Viewport.getHeight", err)) return false;
    Object *o;
    if (!receiver_object("Viewport.getHeight", a, &o, err)) return false;
    Runtime::Resource *r = viewport_resource(a[0]);
    *out = Value::make_float(r ? r->viewport_rect[3] : 0.0f);
    return true;
}

// 0x4e6e90 Viewport.getTop: exe returns typed viewport data +0x14.
// INVENTED: missing handle/typed data returns 0.0f; the exe's fallback is the raw
// receiver slot, which has no rebuild equivalent.
bool vp_get_top(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Viewport.getTop", err)) return false;
    Object *o;
    if (!receiver_object("Viewport.getTop", a, &o, err)) return false;
    Runtime::Resource *r = viewport_resource(a[0]);
    *out = Value::make_float(r ? r->viewport_rect[1] : 0.0f);
    return true;
}

// 0x4e6e40 Viewport.getLeft: exe returns typed viewport data +0x10.
// INVENTED: missing handle/typed data returns 0.0f; the exe's fallback is the raw
// receiver slot, which has no rebuild equivalent.
bool vp_get_left(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Viewport.getLeft", err)) return false;
    Object *o;
    if (!receiver_object("Viewport.getLeft", a, &o, err)) return false;
    Runtime::Resource *r = viewport_resource(a[0]);
    *out = Value::make_float(r ? r->viewport_rect[0] : 0.0f);
    return true;
}

// 0x4e6f30 Viewport.resize: writes typed viewport data +0x10..+0x1c; no-op without a handle.
bool vp_resize(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Viewport.resize", err)) return false;
    Object *o;
    if (!receiver_object("Viewport.resize", a, &o, err)) return false;
    float v[4];
    if (!want("Viewport.resize", a, 5, err) ||
        !get_f("Viewport.resize", a[1], &v[0], err) ||
        !get_f("Viewport.resize", a[2], &v[1], err) ||
        !get_f("Viewport.resize", a[3], &v[2], err) ||
        !get_f("Viewport.resize", a[4], &v[3], err))
        return false;
    if (Runtime::Resource *r = viewport_resource(a[0]))
        for (int i = 0; i < 4; ++i) r->viewport_rect[i] = v[i];
    *out = Value::make_null();
    return true;
}

// 0x4e6fb0 Viewport.unproject(Vector3,I)Vector3
// SERVICE MISSING: 0x422320 finds the viewport node in the game lists
// +0x198/+0x14c by node+0x34 == resource id, walks its children and unprojects
// through the child whose +0xa98 == camId with the camera matrices at +0x45c
// (inverse projection) and +0x39c (camera->world). src has no game node list,
// no viewport->camera link (cam_create, card 072) and no camera matrices, so
// the lookup never matches: the exe returns null there (0x4e6fb0 fallthrough).
// shortcut: the Vector3 x/y/z reads (0x4ec980) are skipped: their values are
// only consumed by the transform that never runs.
bool vp_unproject(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Viewport.unproject", err)) return false;
    if (!want("Viewport.unproject", a, 3, err)) return false;
    *out = Value::make_null();
    return true;
}

// ---- java.render.Camera (0x4d8de0 / 0x4d8fc0 / 0x4d9140) ----------------

// 0x4d8de0 Camera.create(ResourceRef,Viewport,IFFFFFII)V: builds a type-3
// "s_renderref_camera" instance and rebinds the receiver handle to it.
// INVENTED: creation via Runtime::create_resource stands in for 0x524dc0/0x524840;
// the camera typed-data words live on the bound Resource (see runtime.h).
// INVENTED: camera_aspect is the exe's no-data value 1.0f (0x4d8de0 passes
// 1.0f/[0x593598] when the viewport pixel size is unavailable; same as
// vp_get_aspect, card 061). The aspect itself is vp+0xec/vp+0xf0 from FUN_00422320.
// shortcut: the ResourceRef parent/package placement, the local type link and the
// viewport attach (0x420900 / 0x420080) are not modelled; a[1]/a[2] are unread.
// UNRESOLVED: 0x524dc0/0x524840 internals (DAT_005fde70+8/+0xc, bounds, +0x11c).
bool cam_create(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Camera.create", err)) return false;
    Object *o;
    if (!receiver_object("Camera.create", a, &o, err)) return false;
    int32_t id, i1, i2;
    float fov, near, far, f4, f5;
    if (!want("Camera.create", a, 11, err) ||
        !get_i("Camera.create", a[3], &id, err) ||
        !get_f("Camera.create", a[4], &fov, err) ||
        !get_f("Camera.create", a[5], &near, err) ||
        !get_f("Camera.create", a[6], &far, err) ||
        !get_f("Camera.create", a[7], &f4, err) ||
        !get_f("Camera.create", a[8], &f5, err) ||
        !get_i("Camera.create", a[9], &i1, err) ||
        !get_i("Camera.create", a[10], &i2, err))
        return false;
    Runtime::Handle *h = game_rt()->handle(o);  // 0x4ecc70
    // INVENTED: the exe has no guard and would fault at the 0x40cac0 bind.
    if (!h) {
        *out = Value::make_null();
        return true;
    }
    Runtime::Resource *r = game_rt()->create_resource(3, "s_renderref_camera");
    r->camera_aspect = 1.0f;        // 0x524dc0 data+0xc = 1.0f/param_2; INVENTED no-data
    r->camera_fov = fov;            // +0x10
    r->camera_near = near * 10.0f;  // +0x14
    r->camera_far = far * 10.0f;    // +0x18
    r->camera_f4 = f4;              // +0x34
    r->camera_f5 = f5;              // +0x38
    r->camera_i1 = i1;              // +0x30
    r->camera_i2 = i2;              // +0x2c
    r->camera_viewport_id = id;     // a[3] -> 0x420080 FUN_00464f60; INVENTED storage
    game_rt()->bind(h, r);          // 0x40cac0
    const float p[3] = {0.0f, 0.0f, 3.0f};
    ri_set_bone_pos(h, ri_get_bone_id(h, "bone00"), p);  // 0x4e77f0/0x4ec470
    *out = Value::make_null();
    return true;
}

// 0x4d8fc0 Camera.destroy()V: 0x4e9450 queues the receiver handle for the
// deferred delete (flushed by 0x47d070 -> D3 zeroes the handle).
// INVENTED: missing handle returns without logging (exe logs "vm_cam_destroy:
// scripted resource %x not found").
// shortcut: the embedded fog ref node (inst+0x108/+0x110/+0x114) is not modelled;
// src stores fog on the resource.
bool cam_destroy(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Camera.destroy", err)) return false;
    Object *o;
    if (!receiver_object("Camera.destroy", a, &o, err) ||
        !want("Camera.destroy", a, 1, err))
        return false;
    Runtime::Handle *h = game_rt()->handle(o);  // 0x4ecc70
    if (h && h->id && h->target) game_rt()->queue_delete(h);  // 0x4e9450
    *out = Value::make_null();
    return true;
}

// 0x4d9140 Camera.setFog(IFF)V: enable 1.0f, colour, start*10, end*10.
// INVENTED: values stored on the bound Resource (exe: a separate fog resource
// reached through instance data +0x114) and no log for the missing handle (exe
// logs "vm_cam_setfog: scripted resource %x not found").
bool cam_set_fog(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Camera.setFog", err)) return false;
    Object *o;
    if (!receiver_object("Camera.setFog", a, &o, err)) return false;
    int32_t colour;
    float start, end;
    if (!want("Camera.setFog", a, 4, err) ||
        !get_i("Camera.setFog", a[1], &colour, err) ||
        !get_f("Camera.setFog", a[2], &start, err) ||
        !get_f("Camera.setFog", a[3], &end, err))
        return false;
    if (Runtime::Resource *r = value_resource(a[0])) {
        r->fog_enabled = true;          // fd+0x24 = 1.0f
        r->fog_color = colour;          // fd+0x28
        r->fog_start = start * 10.0f;   // fd+0x1c
        r->fog_end = end * 10.0f;       // fd+0x20
    }
    *out = Value::make_null();
    return true;
}

// 0x4de640 GroundRef.setFog(IFF)V: same four words as Camera.setFog.
// INVENTED: values stored on the receiver's Resource; no log for a missing
// handle (same as the exe).
// shortcut: the 0x10-byte record and the 0x47c640/0x47e450 info post (id 0x4a)
// are not modelled; src has no info dispatcher.
bool gr_set_fog(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("GroundRef.setFog", err)) return false;
    Object *o;
    if (!receiver_object("GroundRef.setFog", a, &o, err)) return false;
    int32_t colour;
    float start, end;
    if (!want("GroundRef.setFog", a, 4, err) ||
        !get_i("GroundRef.setFog", a[1], &colour, err) ||
        !get_f("GroundRef.setFog", a[2], &start, err) ||
        !get_f("GroundRef.setFog", a[3], &end, err))
        return false;
    if (Runtime::Resource *r = value_resource(a[0])) {
        r->fog_enabled = true;          // rec+0 = 1
        r->fog_color = colour;          // rec+0xc
        r->fog_start = start * 10.0f;   // rec+4
        r->fog_end = end * 10.0f;       // rec+8
    }
    *out = Value::make_null();
    return true;
}

// ---- java.game.Navigator -------------------------------------------------

// 0x4df470 Navigator.updateNavigator(GameRef car, int mode)V
// mode 0 clamps the car position to the map rect and writes the offsets back;
// then the camera bone00 matrix is set (mode 0 translation, mode 1 yaw pose).
bool nav_update_navigator(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Navigator.updateNavigator", err)) return false;
    Object *self;
    int32_t mode;
    if (!receiver_object("Navigator.updateNavigator", a, &self, err) ||
        !want("Navigator.updateNavigator", a, 3, err) ||
        !get_i("Navigator.updateNavigator", a[2], &mode, err))
        return false;
    *out = Value::make_null();

    // 0x4ecae0 getObjectField(self, "cam", "java.render.Camera")
    std::string e;
    Value fv;
    Object *cam = vm.get_instance_field(self, "cam", &fv, &e) &&
                          fv.kind == Value::Obj ? fv.obj : nullptr;
    if (!cam || !game_rt()->class_is_a(object_class_name(cam), "java.render.Camera"))
        return true;                              // exe returns void
    Runtime::Handle *cam_h = game_rt()->handle(cam);       // 0x4ecc70
    Runtime::Handle *car = value_handle(a[1]);
    Runtime::Handle *vp = nullptr;
    if (vm.get_instance_field(self, "vp", &fv, &e) && fv.kind == Value::Obj)
        vp = value_handle(fv);                     // 0x4ecfc0
    if (!cam_h || cam_h->id == 0 || !car || car->id == 0 || !vp)
        return true;

    Runtime::Resource *cr = car->target;
    if (!cr) return true;   // INVENTED: unbound handle, exe reads zeros
    float pos[3] = {cr->pos[0], cr->pos[1], cr->pos[2]};    // 0x4ea7a0
    uint32_t bone = ri_get_bone_id(cam_h, "bone00");        // 0x4e77f0
    float bm[16] = {};      // INVENTED: exe reads uninitialized stack when
                            // the bone is missing; zeroed here
    ri_get_bone_matrix(cam_h, bone, bm);                    // 0x4ea7f0

    float off_x = dd_get_f(vm, self, "offsetX");            // 0x4ec980
    float off_z = dd_get_f(vm, self, "offsetZ");
    pos[0] += off_x;
    pos[2] += off_z;

    if (mode == 0) {                                        // clamp
        float top = dd_get_f(vm, self, "top") * 100.0f;
        float left = dd_get_f(vm, self, "left") * 100.0f;
        float size = dd_get_f(vm, self, "size") * 100.0f * 8.0f;
        float h = bm[13] * 5.0f;
        float k = h * 1.3333334f;
        float lo_z = top + h, hi_z = (size + top) - h;
        float lo_x = left + k, hi_x = (size + left) - k;
        if (lo_z > pos[2]) {
            dd_set_f(vm, self, "offsetZ", (lo_z - pos[2]) + off_z);
            pos[2] = lo_z;
        } else if (hi_z < pos[2]) {
            dd_set_f(vm, self, "offsetZ", (hi_z - pos[2]) + off_z);
            pos[2] = hi_z;
        }
        if (lo_x > pos[0]) {
            dd_set_f(vm, self, "offsetX", (lo_x - pos[0]) + off_x);
            pos[0] = lo_x;
        } else if (hi_x < pos[0]) {
            dd_set_f(vm, self, "offsetX", (hi_x - pos[0]) + off_x);
            pos[0] = hi_x;
        }
    }

    const float *m = bm;
    float m1[16];
    if (mode == 0) {
        bm[12] = pos[0] * 0.01f * 10.0f;
        bm[14] = pos[2] * 0.01f * 10.0f;
    } else if (mode == 1) {
        // 0x4ea660 -> 0x569970 yields {yaw,pitch,roll}; the exe replaces
        // pitch/roll with -1.5/0 before building the pose.
        // INVENTED mapping: the car yaw is the resource's ypr y
        // (same mapping as GameRef.getOri, natives.cpp:1881).
        float ang[3] = {cr->ori[0], -1.5f, 0.0f};
        Mat34 r = {};
        euler_to_mat(&r, ang);                              // 0x5666d0
        float p[3] = {r.m[2] * 4.5f + pos[0] * 0.01f,
                      r.m[6] * 4.5f,
                      r.m[10] * 4.5f + pos[2] * 0.01f};
        mat_from_rot_pos_x10(m1, r, p);                     // 0x567570
        m = m1;
    }
    ri_set_bone_matrix(cam_h, bone, m);                     // 0x4ec3b0

    // SERVICE MISSING: the "dynamarker"/"marker" loops after 0x4ec3b0 need
    // 0x422320 (find the viewport node in the GfxEngine lists +0x198/+0x14c
    // by node+0x34 == vp->id), 0x421260/0x465100 (viewport scale),
    // 0x4d88a0 (world->screen projected point via 0x421670/0x421e20, with
    // x clamped to [-0.9,0.9] and y to [-0.85,0.85]), 0x459290 and the
    // render typed data +0xb0..+0xc8. src has none of these; the exe's
    // effect there is moving marker symbol bones. The native returns void,
    // so skipping the loops keeps the camera pose above.
    return true;
}

// ---- GroundRef service-less natives (notes/19 §5) ------------------------
// Each exe implementation reads its arguments, then gates on Game__getInfo(self,
// 0x39) or (self, 0x3d). The gate resolves a native ground object through the
// typed layer; src has neither, so it is always 0 here.

// 0x4dc400 GroundRef.setWater(FFF)V: getArgs; if (h->RI) physics world set/clear.
// SERVICE MISSING: no process-wide physics world (PhysicsWorld__setWater 0x501e10).
bool gr_set_water(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.setWater", a, 4, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4dc2b0 GroundRef.setWater(Vector3,Vector3,FF)V: same, point/normal from the two Vector3.
// SERVICE MISSING: no process-wide physics world (PhysicsWorld__setWater 0x501e10).
bool gr_set_water_v(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.setWater", a, 5, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4da500 GroundRef.addWaterLimit(Vector3,Vector3)V: if (h->RI) PhysicsWorld__addWaterLimit.
// SERVICE MISSING: no process-wide physics world (PhysicsWorld__setWater 0x501e10).
bool gr_add_water_limit(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.addWaterLimit", a, 3, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4dd090 GroundRef.addTrafficCar(GameRef,Vector3)I: if (inst == 0) log, return 0;
// if (!getInfo(h, 0x39)) return 0.
// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x39) is 0 (notes/19).
// INVENTED: the exe appends in "<file>" at line <n>; a NativeFn has no frame.
bool gr_add_traffic_car(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.addTrafficCar", a, 3, err)) return false;
    if (a[1].kind == Value::Null || (a[1].kind == Value::Obj && !a[1].obj))
        std::fprintf(stderr, "vm_ground_addtrafficcar: Traffic car instance is null\n");
    *out = Value::make_int(0);
    return true;
}

// 0x4de520 GroundRef.remTrafficCar(I)V: if (car) TrafficCar__remove(car); ids are raw
// car pointers, none exist.
// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x39) is 0 (notes/19).
bool gr_rem_traffic_car(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.remTrafficCar", a, 2, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4de5f0 GroundRef.notifyTrafficCar(II)I: if (car == 0) return 0.
// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x39) is 0 (notes/19).
bool gr_notify_traffic_car(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.notifyTrafficCar", a, 3, err)) return false;
    *out = Value::make_int(0);
    return true;
}

// 0x4dc9e0 GroundRef.addTrafficN(GameRef,IFFF)I: if (n < 1 || !getInfo(h, 0x39)) return 0.
// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x39) is 0 (notes/19).
bool gr_add_traffic_n(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.addTrafficN", a, 6, err)) return false;
    *out = Value::make_int(0);
    return true;
}

// 0x4dcdc0 GroundRef.addTrafficP(GameRef,Vector3,IFFF)I: if (n < 1 || !getInfo || pos == 0)
// return 0.
// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x39) is 0 (notes/19).
bool gr_add_traffic_p(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.addTrafficP", a, 7, err)) return false;
    *out = Value::make_int(0);
    return true;
}

// 0x4dd840 GroundRef.delTraffic()V: if (getInfo(h, 0x39)) Traffic__clear().
// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x39) is 0 (notes/19).
bool gr_del_traffic(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.delTraffic", a, 1, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4e6280 GroundRef.setTrafficCarBehaviour(II)V: if (car && car+0x138) car+0x160 = mode.
// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x39) is 0 (notes/19).
bool gr_set_traffic_car_behaviour(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.setTrafficCarBehaviour", a, 3, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4de360 GroundRef.haltTrafficCross(Vector3,F)V: if (getInfo) { nearestCross(pos);
// cross->halt(time, 1) }.
// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x39) is 0 (notes/19).
bool gr_halt_traffic_cross(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.haltTrafficCross", a, 3, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4dd700 GroundRef.haltTrafficPath(Vector3,Vector3)V: if (p1 && p2 && getInfo)
// Traffic__haltPath(p1, p2).
// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x39) is 0 (notes/19).
bool gr_halt_traffic_path(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.haltTrafficPath", a, 3, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4de270 GroundRef.haltTrafficRoad(Vector3,Vector3)V: if (p1 && p2 && getInfo)
// Traffic__haltRoad(p1, p2).
// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x39) is 0 (notes/19).
bool gr_halt_traffic_road(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.haltTrafficRoad", a, 3, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4dd6c0 GroundRef.clearHalts()V: t = getInfo(h, 0x39); if (t) Traffic__clearHalts(t).
bool gr_clear_halts(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.clearHalts", a, 1, err)) return false;
    Runtime *rt = game_rt();
    traffic::Graph *t = rt ? rt->ground_traffic(value_handle(a[0])) : nullptr;
    if (t) t->clear_halts();
    *out = Value::make_null();
    return true;
}

// 0x4dd7f0 GroundRef.setTrafficCrossStopOffset(F)V: if (t = getInfo(h, 0x39))
// t+0x3140 = f.
// INVENTED: a null handle yields null; the exe dereferences it.
bool gr_set_traffic_cross_stop_offset(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.setTrafficCrossStopOffset", a, 2, err)) return false;
    float f;
    if (!get_f("GroundRef.setTrafficCrossStopOffset", a[1], &f, err)) return false;
    Runtime *rt = game_rt();
    traffic::Graph *t = rt ? rt->ground_traffic(value_handle(a[0])) : nullptr;
    if (t) t->cross_stop_offset = f;
    *out = Value::make_null();
    return true;
}

// 0x4dd880 GroundRef.getNearestCross(Vector3,F)V3: pos == 0 -> 0; h == 0 -> 0;
// !getInfo -> 0; c = Traffic__nearestCross(t, x, y, z, dist, 0) -> c+0x24.
// INVENTED: a null cross yields null; the exe dereferences it.
bool gr_get_nearest_cross(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.getNearestCross", a, 3, err)) return false;
    *out = Value::make_null();
    if (a[1].kind == Value::Null) return true;
    Object *o;
    float pos[3], dist;
    if (!get_o("GroundRef.getNearestCross", a[1], &o, err) ||
        !vec_read(vm, o, pos, err) ||
        !get_f("GroundRef.getNearestCross", a[2], &dist, err))
        return false;
    Runtime *rt = game_rt();
    traffic::Graph *t = rt ? rt->ground_traffic(value_handle(a[0])) : nullptr;
    if (!t) return true;
    const traffic::Cross *c = t->nearest_cross(pos[0], pos[1], pos[2], dist, false);
    if (!c) return true;   // INVENTED: the exe dereferences a null cross.
    Object *v = new_point(vm, "java.lang.Vector3", c->pos, false, err);
    if (!v) return false;
    *out = Value::make_obj(v);
    return true;
}

// 0x4ddfb0 GroundRef.getStartDirection(Vector3,Vector3)V3: if (!getInfo) return 0.
// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x39) is 0 (notes/19).
bool gr_get_start_direction(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.getStartDirection", a, 3, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4ddc50 GroundRef.getRouteLength(Vector3,Vector3)F / findRoute: r = -1.0f;
// if (p1 && p2 && getInfo) findRoute; return r.
// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x39) is 0 (notes/19).
bool gr_find_route(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.findRoute", a, 3, err)) return false;
    *out = Value::make_float(-1.0f);
    return true;
}

// 0x4ddc30 GroundRef.getRouteLength()F: return route(0x5fe450) ? routeLen(0x5fe454) : 0.0f.
// shortcut: no route state until findRoute (0x417e50) is ported; the route is never set.
bool gr_get_route_length(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.getRouteLength", a, 1, err)) return false;
    *out = Value::make_float(0.0f);
    return true;
}

// 0x4dde50 GroundRef.getRoutePos(F)V3: if (route == 0) return 0.
// shortcut: no route state until findRoute (0x417e50) is ported; the route is never set.
bool gr_get_route_pos(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.getRoutePos", a, 2, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4ddb50 GroundRef.getRouteDist(Vector3)F: r = -1.0f; if (route == 0 ||
// !(0 < routeLen)) return 0.0f; if (pos == 0) return r.
// shortcut: no route state until findRoute (0x417e50) is ported; the route is never set.
bool gr_get_route_dist(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.getRouteDist", a, 2, err)) return false;
    *out = Value::make_float(0.0f);
    return true;
}

// 0x4dd470 GroundRef.alignToRoad(Vector3)V3[]: pos == 0 -> 0; h == 0 -> 0; !getInfo -> 0.
// p = v.x/y/z; Traffic__alignToRoad(t, p, dir) (result ignored); return {new V3(p),
// new V3(dir)}.
// INVENTED: dir is zero-initialised (the exe leaves it uninitialised when no lane is
// found); a null handle or ground graph yields null instead of dereferencing.
bool gr_align_to_road(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.alignToRoad", a, 2, err)) return false;
    *out = Value::make_null();
    if (a[1].kind == Value::Null) return true;
    Object *o;
    float pos[3];
    if (!get_o("GroundRef.alignToRoad", a[1], &o, err) || !vec_read(vm, o, pos, err))
        return false;
    Runtime *rt = game_rt();
    traffic::Graph *t = rt ? rt->ground_traffic(value_handle(a[0])) : nullptr;
    if (!t) return true;
    float dir[3] = {0.0f, 0.0f, 0.0f};
    t->align_to_road(pos, dir);
    Object *a0 = new_point(vm, "java.lang.Vector3", pos, false, err);
    if (!a0) return false;
    Object *a1 = new_point(vm, "java.lang.Vector3", dir, false, err);
    if (!a1) return false;
    Array *arr = vm.new_array("Ljava.lang.Vector3;", 2);
    arr->elems[0] = Value::make_obj(a0);
    arr->elems[1] = Value::make_obj(a1);
    *out = Value::make_array(arr);
    return true;
}

// 0x4de580 GroundRef.setPedestrianDensityN(F)V: if (getInfo(h, 0x3d))
// Pedestrians__setDensity(d, d*1.1).
// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x3d) is 0 (notes/19).
bool gr_set_pedestrian_density_n(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.setPedestrianDensityN", a, 2, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4dc940 GroundRef.addPedestrianType(GameRef)V: if (getInfo(h, 0x3d)) { make type
// resident; Pedestrians__addType(type) }.
// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x3d) is 0 (notes/19).
bool gr_add_pedestrian_type(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.addPedestrianType", a, 2, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4de4d0 GroundRef.removePedestrianType(GameRef)V: if (getInfo(h, 0x3d))
// Pedestrians__removeType(type).
// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x3d) is 0 (notes/19).
bool gr_remove_pedestrian_type(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.removePedestrianType", a, 2, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4de420 GroundRef.pedestrianDistance(Vector3,I)F: if (!getInfo(h, 0x3d)) return -1.0f.
// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x3d) is 0 (notes/19).
bool gr_pedestrian_distance(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.pedestrianDistance", a, 3, err)) return false;
    *out = Value::make_float(-1.0f);
    return true;
}

// 0x4de1c0 GroundRef.getTrafficCarPos(I,Vector3)V: if (car) { pos = carPos(car);
// out.x/y/z = pos }.
// SERVICE MISSING: no native ground object, ids are raw car pointers and none exist.
bool gr_get_traffic_car_pos(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.getTrafficCarPos", a, 3, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4dda10 GroundRef.getPedestrianPos(Vector3,Vector3,I)V: getArgs(self, &pos, &out,
// &type); if (!getInfo(h, 0x3d)) { out.x = out.y = out.z = 0; return; }.
// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x3d) is 0 (notes/19).
// INVENTED: the exe writes through a null out object; the port skips a null argument.
bool gr_get_pedestrian_pos(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!want("GroundRef.getPedestrianPos", a, 4, err)) return false;
    if (a[2].kind != Value::Null) {
        Object *o;
        if (!get_o("GroundRef.getPedestrianPos", a[2], &o, err)) return false;
        if (!write_f(vm, o, "x", 0.0f, err) || !write_f(vm, o, "y", 0.0f, err) ||
            !write_f(vm, o, "z", 0.0f, err))
            return false;
    }
    *out = Value::make_null();
    return true;
}

// ---- java.util.resource.Animation (notes/12 §9) -------------------------
// All nine natives drive a render-side animation player: FUN_0051b8d0 creates
// the 0xc0-byte player under a RenderRef resource instance and the operations
// append {opcode, value} records to it (+0xb4 buf, +0xb8 count, +0xbc cap).
// SERVICE MISSING: src/ has no render instance, no RenderRef handle creation
// and no player, so no guard can pass and nothing is queued.

// 0x4d8a40 Animation.<init>(RenderRef,ResourceRef)V
// exe: builds the player when the RenderRef RI has typed data (0x4d8a40:
// Resource__getTyped(RI,0x80000000) -> RI->vf[+0xc] -> FUN_0051be40 ->
// FUN_0051b8d0), then stores it in the Animation's native-handle field and
// binds the "obj" RenderRef handle to the argument's RI (FUN_0040cac0).
// shortcut: no player can be created (native-handle field stays 0). The
// "obj" bind is unconditional in the exe and IS ported. INVENTED: the exe
// derefs a null RenderRef argument at 0x4d8b5b and a null obj handle in
// 0x40cac0; the port skips those instead of faulting.
bool anim_ctor(Vm &vm, const Args &a, Value *out, std::string *err)
{
    if (!need_rt("Animation.<init>", err)) return false;
    Object *o;
    if (!receiver_object("Animation.<init>", a, &o, err) ||
        !want("Animation.<init>", a, 3, err))
        return false;
    Value v;
    std::string e;
    Runtime::Handle *h = vm.get_instance_field(o, "obj", &v, &e) ? value_handle(v) : nullptr;
    Runtime::Handle *src = value_handle(a[1]);       // renderRef native handle
    if (h && src) game_rt()->bind(h, src->target);   // 0x40cac0(hObj, renderRef+0xc)
    *out = Value::make_null();
    return true;
}

// 0x4d8980 Animation.finalize()V
// exe: if the "obj" RenderRef handle's resource id (+8) != 0 and the stored
// player != 0, player->vf[0](1). shortcut: no player exists here.
bool anim_finalize(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Animation.finalize", a, 1, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4d8d70 Animation.setSpeed(F)V: queue opcode 5 (FUN_004cf350) when the
// "obj" RenderRef handle has a nonzero resource id.
// SERVICE MISSING: no player, so the queue helper is unreachable. INVENTED:
// once the obj bind makes the exe guard pass the exe derefs a null player;
// the port stays a no-op instead.
bool anim_set_speed(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Animation.setSpeed", a, 2, err)) return false;
    float v;
    if (!get_f("Animation.setSpeed", a[1], &v, err)) return false;
    (void)v;
    *out = Value::make_null();
    return true;
}

// 0x4d8d00 Animation.setFade(F)V: queue opcode 6 (FUN_004cf2e0).
// SERVICE MISSING: no player; INVENTED: no-op instead of the null-player deref.
bool anim_set_fade(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Animation.setFade", a, 2, err)) return false;
    float v;
    if (!get_f("Animation.setFade", a[1], &v, err)) return false;
    (void)v;
    *out = Value::make_null();
    return true;
}

// 0x4d89e0 Animation.getPos()F: 0.0f when the "obj" guard fails (0x4d8a26
// je -> 0x4d8a34 fldz), otherwise player+0x34 / clip length via FUN_0051dde0.
// SERVICE MISSING: no player, so the exe's no-data value 0.0f. INVENTED: with
// the obj bound the exe derefs the 0 player; the port still returns 0.0f.
bool anim_get_pos(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Animation.getPos", a, 1, err)) return false;
    *out = Value::make_float(0.0f);
    return true;
}

// 0x4d8c90 Animation.seek(F)V: queue opcode 4 (FUN_004cf270).
// SERVICE MISSING: no player; INVENTED: no-op instead of the null-player deref.
bool anim_seek(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Animation.seek", a, 2, err)) return false;
    float v;
    if (!get_f("Animation.seek", a[1], &v, err)) return false;
    (void)v;
    *out = Value::make_null();
    return true;
}

// 0x4d8c30 Animation.play()V: queue opcode 0, value 0 (FUN_004d8840).
// SERVICE MISSING: no player; INVENTED: no-op instead of the null-player deref.
bool anim_play(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Animation.play", a, 1, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4d8b70 Animation.loopPlay()V: queue opcode 1, value 0 (FUN_004d8780).
// SERVICE MISSING: no player; INVENTED: no-op instead of the null-player deref.
bool anim_loop_play(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Animation.loopPlay", a, 1, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4d8bd0 Animation.pause()V: queue opcode 3, value 0 (FUN_004d87e0).
// SERVICE MISSING: no player; INVENTED: no-op instead of the null-player deref.
bool anim_pause(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Animation.pause", a, 1, err)) return false;
    *out = Value::make_null();
    return true;
}

// ---- java.util.resource.ParticleSystem (no-service) ---------------------
// Every exe write needs the native particle object P (typed data T+0x18), and
// src has no typed-data layer, so the getTyped(0xa0000001) guard always fails
// and no state is touched. want() is the src equivalent of getArgs' arity
// check; the exe does no type checks there.

// 0x4e0ad0 ParticleSystem.init(ResourceRef,RenderRef,String)V: getArgs(self,&res,
// &render,&name); h = instanceToHandle(self); if (h && res && render) 0x4e8b50(...).
// 0x4e8b50 with ECX = h: make R = render+0xc resident, getTyped(R,0xa0000001,0,1.0f),
// T = R->vf3(1.0f), then 0x524840/0x40cac0/0x459290/0x468b00/0x51bae0 build P.
// SERVICE MISSING: 0x4e8b50 creates the particle system through
// 0x524840 / 0x40cac0 / 0x459290 / 0x468b00 / 0x51bae0 and stores it at typed
// +0x18 (typed+0x16c |= 0x8000); none of these services exist in src.
bool ps_init(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("ParticleSystem.init", a, 4, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4e1440 ParticleSystem.stop()V: getArgs(self); h = instanceToHandle(self);
// if (h) 0x4ec720(h): R = h+0xc; make resident (R+0x7c != 1 -> R->vf5(1.0f));
// if (getTyped(R,0xa0000001,0,0) < 0) return; T = R->vf3(1.0f); P = T+0x18;
// if (!P) return; P+0x9c |= 0x40000000.
// SERVICE MISSING: writes P+0x9c |= 0x40000000; P = typed+0x18 does not
// exist without the 0x4e8b50 service.
bool ps_stop(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("ParticleSystem.stop", a, 1, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4e10c0 ParticleSystem.setFreq(F)V: getArgs(self,&freq); h = instanceToHandle(self);
// if (!h) return; R = h+0xc; if (!R) return; make resident; if (getTyped(R,
// 0xa0000001,0,0) < 0) return; T = R->vf3(1.0f); P = T+0x18; if (!P) return;
// P+0xc = freq (raw float bits, movss).
// SERVICE MISSING: writes P+0xc = freq (P+0x18 typed slot absent).
bool ps_set_freq(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("ParticleSystem.setFreq", a, 2, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4e0b30 ParticleSystem.modePermanent(I)V: same guard chain as setFreq, then
// mode == 0 -> P+0x9c &= 0xdfffffff; mode == 1 -> P+0x9c |= 0x20000000;
// other values unchanged (asm: sub ecx,0 / je; sub ecx,1 / jne ret).
// SERVICE MISSING: P+0x9c bit 0x20000000 (0/1 only; other values unchanged).
// P absent.
bool ps_mode_permanent(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("ParticleSystem.modePermanent", a, 2, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4e0970 ParticleSystem.delAction(String)V: same guard chain as setFreq, then
// node = 0x4b1060(P, name) walks P+0x24; if (node) node->vf0(1).
// SERVICE MISSING: 0x4b1060 walks P+0x24 and destroys a name match
// (0x57c11e stricmp); P absent here.
bool ps_del_action(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("ParticleSystem.delAction", a, 2, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4e0de0 ParticleSystem.setDirectSource(String,Vector3,F,F,Vector3,F,F,F,String)V:
// getArgs(self,&name,&v1,&f1,&f2,&v2,&f3,&f4,&f5,&boneName); h =
// instanceToHandle(self); if (!h) return; R = h+0xc; if (!R) return; make resident
// (R+0x7c != 1 -> R->vf5(1.0f)); if (getTyped(R,0xa0000001,0,0) < 0) return;
// T = R->vf3(1.0f); if (!T) return; D = T+0x18; if (!D) return; x/y/z of v1 and v2
// via getF 0x4ec980; bone = boneName ? 0x4e77f0(h,boneName) : 0; act =
// list_find 0x4b1060(D,name) walks D+0x24; miss: alloc(0x7c), 0x46fa70
// create_direct_source, link at D+0x44/D+0x4c; hit: act->[0x28] == 0xb ->
// 0x4d5c20/0x4d5c50 set the two ends, act->[0x6c] = f5.
// SERVICE MISSING: D = typed+0x18 does not exist without the particle service
// (0x4e8b50, card 091); every exe write is behind D != 0.
// shortcut: type 0xb = direct source, offsets +0x28/+0x6c, list D+0x24..+0x4c;
// upgrade when the particle service exists.
bool ps_set_direct_source(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("ParticleSystem.setDirectSource", a, 10, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4e1160 ParticleSystem.setSource(String,Vector3,F,F,Vector3,F,F,F,String)V:
// same guard chain and slot/field layout as setDirectSource; miss: alloc(0x80),
// 0x46ffb0 create_source, link at D+0x44/D+0x4c; hit: act->[0x28] == 10 ->
// 0x4d5c20/0x4d5c50 set the two ends, act->[0x6c] = f5.
// SERVICE MISSING: D = typed+0x18 does not exist without the particle service
// (0x4e8b50, card 091); every exe write is behind D != 0.
// shortcut: type 10 = source, offsets +0x28/+0x6c, list D+0x24..+0x4c; upgrade
// when the particle service exists.
bool ps_set_source(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("ParticleSystem.setSource", a, 10, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4e0bf0 ParticleSystem.setCounter(String,Vector3,F)V: getArgs(self,&name,&pos,
// &f); h = instanceToHandle(self); if (!h) return; R = h+0xc; if (!R) return;
// make resident; if (getTyped(R,0xa0000001,0,0) < 0) return; T = R->vf3(1.0f);
// if (!T) return; D = T+0x18; if (!D) return; x = getF(pos,"x") (y,z unused);
// vec = 0x470410(&x,0.0f,f); act = list_find 0x4b1060(D,name); hit: act->[0x28] ==
// 0xd -> 0x4d5bf0(act,vec); miss: alloc(0x7c), 0x46f7e0 create, link at
// D+0x44/D+0x4c.
// SERVICE MISSING: D = typed+0x18 does not exist without the particle service
// (0x4e8b50, card 091); every exe write is behind D != 0.
// shortcut: type 0xd = counter, list D+0x24..+0x4c; upgrade when the particle
// service exists.
bool ps_set_counter(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("ParticleSystem.setCounter", a, 4, err)) return false;
    *out = Value::make_null();
    return true;
}

// 0x4e0a10 ParticleSystem.getCounter(String)I: getArgs(self,&name); h =
// instanceToHandle(self); if (!h) return 0; R = h+0xc; if (!R) return 0; make
// resident; if (getTyped(R,0xa0000001,0,0) < 0) return 0; T = R->vf3(1.0f); if
// (!T) return 0; D = T+0x18; if (!D) return 0; act = list_find 0x4b1060(D,name);
// if (!act || act->[0x28] != 0xd) return 0; v = act->[0x70]; act->[0x70] = 0;
// return v.
// SERVICE MISSING: D = typed+0x18 does not exist without the particle service
// (0x4e8b50, card 091); the exe returns 0 on every guard failure.
// shortcut: type 0xd = counter, counter at +0x70 is read-and-cleared; upgrade
// when the particle service exists.
bool ps_get_counter(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("ParticleSystem.getCounter", a, 2, err)) return false;
    *out = Value::make_int(0);
    return true;
}

}  // namespace

namespace natives {

// 0x60251c: the same clock System.simTime (0x4e5810) returns.
double sim_time(Vm &vm) { return sched(vm).sim_seconds; }

// 0x569730 clock, passed in by the host frame loop.
void set_clock(Vm &vm, double now_ms) { sched(vm).now_ms = now_ms; }

std::vector<ThreadInfo> thread_report(Vm &vm)
{
    std::vector<ThreadInfo> out;
    for (const auto &t : sched(vm).threads) {
        ThreadInfo info;
        info.name = t->name;
        info.started = t->started;
        info.done = t->done;
        info.sleeping = t->sleeping;
        info.waiting = t->waiting;
        info.suspended = t->suspended;
        if (t->runner && t->runner->status() == Thread::Failed)
            info.error = t->runner->error();
        out.push_back(std::move(info));
    }
    return out;
}

// 0x5612f0 Thread::Thread(vm, prio, flags, name): append at the tail of the VM
// run list. 0x562d70 Thread::run(0): budget 0 means no time check, so drain
// slices until the frames finish or a mask-0xfa flag stops the thread. A
// yielded thread stays registered for run_threads.
bool spawn_untimed(Vm &vm, std::unique_ptr<Thread> runner, int32_t priority,
                   const std::string &name)
{
    Scheduler &s = sched(vm);
    if (!runner) return false;
    std::unique_ptr<ScriptThreadState> up(new ScriptThreadState);
    ScriptThreadState *t = up.get();
    t->name = name;
    t->priority = priority;
    t->started = true;
    t->runner = std::move(runner);
    s.threads.push_back(std::move(up));
    ScriptThreadState *saved = s.current;  // the caller may be a script thread
    s.current = t;
    t->in_slice = true;
    Thread::Status st;
    do {
        st = t->runner->run_slice(256);
    } while (st == Thread::Yielded && !t->sleeping && !t->waiting &&
             !t->suspended && !t->done);
    t->in_slice = false;
    s.current = saved;
    if (st == Thread::Yielded) return true;  // continue later in run_threads
    if (st == Thread::Failed)
        std::fprintf(stderr, "vm thread failed: %s\n", t->runner->error().c_str());
    t->started = false;
    t->done = true;
    return st != Thread::Failed;
}

// 0x55caf0 JavaMachine::runThreads(float budget_ms, int min_prio).
// The run list is registration order (INVENTED: the exe re-appends woken and
// resumed threads at the tail; the vector order is stable). Runners are kept
// after completion (and after stop) so a script cannot destroy the Thread
// object that is currently on the pump's stack.
// shortcut: src has no per-slice wall clock, so one call makes ONE pass and each
// eligible thread gets run_slice(256). The exe gives it s*q ms with
// q = min(budget, 10) and s = prio < 0 ? (prio + 10) * 0.089999996f + 0.1f
// : prio * 9.9f + 1.0f.
void run_threads(Vm &vm, float budget_ms, int min_prio)
{
    (void)budget_ms;
    Scheduler &s = sched(vm);
    if (s.pumping) return;
    s.pumping = true;
    for (size_t i = 0; i < s.threads.size(); i++) {
        ScriptThreadState *t = s.threads[i].get();
        // 0x55caf0: dead (0x80) / waiting (0x10) / suspended (0x20) are off the
        // run list, and flags 0x44 block a slice.
        if (!t->runner || !t->started || t->done || t->waiting || t->suspended ||
            t->in_slice || min_prio > t->priority)
            continue;
        // 0x55caf0: sleep expires when wake(+0x30) < now (strict).
        if (t->sleeping && t->wake_ms < s.now_ms) t->sleeping = false;
        if (t->sleeping) continue;
        t->in_slice = true;
        s.current = t;  // ctx+4
        Thread::Status st = t->runner->run_slice(256);
        s.current = nullptr;
        t->in_slice = false;
        if (st == Thread::Yielded) continue;
        if (st == Thread::Failed)
            std::fprintf(stderr, "vm thread failed: %s\n", t->runner->error().c_str());
        // 0x55caf0: flags 0x40 (finished) -> 0x80 (dead).
        t->started = false;
        t->done = true;
    }
    s.pumping = false;
}

// 0x54d3a0: drain the object's wait list, tail to head.
void monitor_notify_all(Vm &vm, Object *o)
{
    Scheduler &s = sched(vm);
    auto it = s.monitors.find(o);
    if (it == s.monitors.end()) return;
    for (size_t i = it->second.size(); i-- > 0;)  // exe pops tail first
        monitor_notify_thread(it->second[i]);
    s.monitors.erase(it);
}

// 0x47e050 control step, clock part: step = min(warp * dt, 0.1f).
// shortcut: the exe advances the physics world's clock in world sub-steps
// (0x502060); src has no world in the frame loop, so the whole step is added.
void advance_sim(Vm &vm, float dt)
{
    Scheduler &s = sched(vm);
    if (s.quit_request) return;  // 0x5f0c74
    float step = std::min(s.warp * dt, 0.1f);  // 0x5da468
    if (step > 0) s.sim_seconds += step;
}

const std::vector<Entry> &table()
{
    static const std::vector<Entry> rows = {
    {"java.game.parts.bodypart.Chassis", "getAngvel", "()Ljava.lang.Vector3;", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getMaterialIndex", "()I", ch_get_material_index, false},
    {"java.game.parts.bodypart.Chassis", "getMaxSteer", "()F", ch_get_max_steer, false},
    {"java.game.parts.bodypart.Chassis", "setMaxSteer", "(F)V", ch_set_max_steer, false},
    {"java.game.parts.bodypart.Chassis", "getShifting", "()I", ch_get_shifting, false},
    {"java.game.parts.bodypart.Chassis", "getNitroing", "()I", ch_get_nitroing, false},
    {"java.game.parts.bodypart.Chassis", "getEngineTemp", "()F", ch_get_engine_temp, false},
    {"java.game.parts.bodypart.Chassis", "getAIparam_float", "(I)F", ch_get_ai_float, false},
    {"java.game.parts.bodypart.Chassis", "getAIparam_int", "(I)I", ch_get_ai_int, false},
    {"java.game.parts.bodypart.Chassis", "setAIparam_float", "(IF)V", ch_set_ai_float, false},
    {"java.game.parts.bodypart.Chassis", "setAIparam_int", "(II)V", ch_set_ai_int, false},
    {"java.game.parts.bodypart.Chassis", "getRPM", "()F", ch_get_rpm, false},
    {"java.game.parts.bodypart.Chassis", "setRPM", "(F)V", ch_set_rpm, false},
    {"java.game.parts.bodypart.Chassis", "getTorque", "(FF)F", ch_get_torque, false},
    {"java.game.parts.bodypart.Chassis", "setTorque", "(F)V", ch_set_torque, false},
    {"java.game.parts.bodypart.Chassis", "getMass", "()F", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getCM", "()Ljava.lang.Vector3;", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getMin", "()Ljava.lang.Vector3;", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getMax", "()Ljava.lang.Vector3;", nullptr, true},
    {"java.game.Vehicle", "getHorn", "()I", veh_get_horn, false},
    {"java.game.parts.bodypart.Chassis", "setHornSFX", "(Ljava.util.resource.ResourceRef;FI)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setNitroSFX", "(Ljava.util.resource.ResourceRef;F)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setCooling", "(FFF)V", ch_set_cooling, false},
    {"java.game.parts.bodypart.Chassis", "forceUpdate", "()V", ch_force_update, false},
    {"java.game.parts.bodypart.Chassis", "getMileage", "()F", ch_get_mileage, false},
    {"java.game.parts.bodypart.Chassis", "setMileage", "(F)V", ch_set_mileage, false},
    {"java.game.parts.bodypart.Chassis", "setBuck", "(IIFFFF)I", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "setSfxExhaustMinVol", "(F)V", ch_set_exhaust_min_vol, false},
    {"java.game.parts.bodypart.Chassis", "setSteerWheelRadius", "(F)V", ch_set_steer_wheel_radius, false},
    {"java.game.parts.bodypart.Chassis", "setSteerWheel", "(FF)V", ch_set_steer_wheel, false},
    {"java.game.parts.bodypart.Chassis", "setAckermann", "(F)V", ch_set_ackermann, false},
    {"java.game.Vehicle", "hasCrime", "()F", ve_has_crime, false},
    {"java.game.parts.bodypart.Chassis", "getSfxTable", "(I)Ljava.game.parts.SfxTable;", nullptr, true},
    {"java.game.parts.SfxTable", "getItems", "()I", sfx_get_items, false},
    {"java.game.parts.SfxTable", "clear", "()V", sfx_clear, false},
    {"java.game.parts.SfxTable", "addItem", "(Ljava.util.resource.ResourceRef;FFFFF)V", sfx_add_item, false},
    {"java.game.parts.bodypart.Chassis", "getWheels", "()I", ch_get_wheels, false},
    {"java.game.parts.bodypart.Chassis", "getWheel", "(I)Ljava.game.parts.WheelRef;", ch_get_wheel, false},
    {"java.game.parts.WheelRef", "getPos", "()Ljava.lang.Vector3;", wr_get_pos, false},
    {"java.game.parts.WheelRef", "getYpr", "()Ljava.lang.Ypr;", wr_get_ypr, false},
    {"java.game.parts.WheelRef", "getDrive", "()F", wr_get_drive, false},
    {"java.game.parts.WheelRef", "getSteer", "()F", wr_get_steer, false},
    {"java.game.parts.WheelRef", "getRadius", "()F", wr_get_radius, false},
    {"java.game.parts.WheelRef", "getBrake", "()F", wr_get_brake, false},
    {"java.game.parts.WheelRef", "getHBrake", "()F", wr_get_hbrake, false},
    {"java.game.parts.WheelRef", "setPos", "(Ljava.lang.Vector3;)V", wr_set_pos, false},
    {"java.game.parts.WheelRef", "setYpr", "(Ljava.lang.Ypr;)V", wr_set_ypr, false},
    {"java.game.parts.WheelRef", "setDrive", "(F)V", wr_set_drive, false},
    {"java.game.parts.WheelRef", "setSteer", "(F)V", wr_set_steer, false},
    {"java.game.parts.WheelRef", "setRadius", "(F)V", wr_set_radius, false},
    {"java.game.parts.WheelRef", "setWidth", "(F)V", wr_set_width, false},
    {"java.game.parts.WheelRef", "setCPatch", "(FFF)V", wr_set_cpatch, false},
    {"java.game.parts.WheelRef", "setFriction", "(F)V", wr_set_friction, false},
    {"java.game.parts.WheelRef", "setFrictn_x", "(F)V", wr_set_frictn_x, false},
    {"java.game.parts.WheelRef", "setSliction", "(F)V", wr_set_sliction, false},
    {"java.game.parts.WheelRef", "setStiffness", "(F)V", wr_set_stiffness, false},
    {"java.game.parts.WheelRef", "setRollRes", "(F)V", wr_set_roll_res, false},
    {"java.game.parts.WheelRef", "setBearing", "(F)V", wr_set_bearing, false},
    {"java.game.parts.WheelRef", "setMaxLoad", "(F)V", wr_set_max_load, false},
    {"java.game.parts.WheelRef", "setLoadSmooth", "(F)V", wr_set_load_smooth, false},
    {"java.game.parts.WheelRef", "setPacejka", "(IF)V", wr_set_pacejka, false},
    {"java.game.parts.WheelRef", "setForce", "(F)V", wr_set_force, false},
    {"java.game.parts.WheelRef", "setDamping", "(F)V", wr_set_damping1, false},
    {"java.game.parts.WheelRef", "setDamping", "(FF)V", wr_set_damping2, false},
    {"java.game.parts.WheelRef", "setRestLen", "(F)V", wr_set_rest_len, false},
    {"java.game.parts.WheelRef", "setMinLen", "(F)V", wr_set_min_len, false},
    {"java.game.parts.WheelRef", "setMaxLen", "(F)V", wr_set_max_len, false},
    {"java.game.parts.WheelRef", "setBrake", "(F)V", wr_set_brake, false},
    {"java.game.parts.WheelRef", "setHBrake", "(F)V", wr_set_hbrake, false},
    {"java.game.parts.WheelRef", "setArm", "(FFFFFFF)V", wr_set_arm, false},
    {"java.game.parts.WheelRef", "setHub", "(FFFFFFFFFF)V", wr_set_hub, false},
    {"java.game.parts.WheelRef", "setInstantCenter", "(FFFFFF)V", wr_set_instant_center, false},
    {"java.game.parts.WheelRef", "setOppWheel", "(I)V", wr_set_opp_wheel, false},
    {"java.game.parts.bodypart.Chassis", "getWheelPos", "(I)Ljava.lang.Vector3;", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getWheelDamage", "(I)Ljava.lang.String;", ch_get_wheel_damage, false},
    {"java.game.parts.bodypart.Chassis", "setWheelDamage", "(ILjava.lang.String;)V", ch_set_wheel_damage, false},
    {"java.game.parts.bodypart.Chassis", "setOSD", "(IF)V", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getOSD", "(I)F", nullptr, true},
    {"java.game.parts.bodypart.Chassis", "getNitro", "()F", ch_get_nitro, false},
    {"java.game.parts.Part", "getSlots", "()I", pt_get_slots, false},
    {"java.game.parts.Part", "getSlotID", "(I)I", pt_get_slot_id, false},
    {"java.game.parts.Part", "getSlotIndex", "(I)I", pt_get_slot_index, false},
    {"java.game.parts.Part", "partOnSlot", "(I)Ljava.game.parts.Part;", pt_part_on_slot, false},
    {"java.game.parts.Part", "slotIDOnSlot", "(I)I", pt_slot_id_on_slot, false},
    {"java.game.parts.Part", "getSlotDamage", "(I)Ljava.lang.String;", pt_get_slot_damage, false},
    {"java.game.parts.Part", "setSlotDamage", "(ILjava.lang.String;)V", pt_set_slot_damage, false},
    {"java.game.parts.Part", "setSlotPos", "(ILjava.lang.Vector3;Ljava.lang.Ypr;)V", pt_set_slot_pos, false},
    {"java.game.parts.Part", "getCarRef", "()Ljava.game.parts.Part;", pt_get_car_ref, false},
    {"java.game.parts.Part", "getWear", "()F", pt_get_wear, false},
    {"java.game.parts.Part", "setWear", "(F)F", pt_set_wear, false},
    {"java.game.parts.Part", "setMaxWear", "(F)V", pt_set_max_wear, false},
    {"java.game.parts.Part", "getTear", "()F", pt_get_tear, false},
    {"java.game.parts.Part", "setTear", "(F)F", pt_set_tear, false},
    {"java.game.parts.Part", "disableSlot", "(II)V", pt_disable_slot, false},
    {"java.game.parts.Part", "isSlotDisabled", "(I)I", pt_is_slot_disabled, false},
    {"java.game.parts.Part", "getTexture", "()I", pt_get_texture, false},
    {"java.game.parts.Part", "setTexture", "(I)I", pt_set_texture, false},
    {"java.game.parts.Part", "getMesh", "()I", pt_get_mesh, false},
    {"java.game.parts.Part", "setMesh", "(I)I", pt_set_mesh, false},
    {"java.game.parts.Part", "getRenderType", "()I", pt_get_render_type, false},
    {"java.game.parts.Part", "setRenderType", "(I)I", pt_set_render_type, false},
    {"java.game.parts.Part", "getLogo", "()I", pt_get_logo, false},
    {"java.game.parts.Part", "getCar", "()I", pt_get_car, false},
    {"java.game.parts.Part", "getWheelID", "()I", pt_get_wheel_id, false},
    {"java.game.parts.Part", "getMass", "()F", pt_get_mass, false},
    {"java.game.parts.Part", "setSfxLoopParams", "(FF)I", pt_set_sfx_loop_params, false},
    {"java.game.parts.Part", "flap", "(I)I", pt_flap, false},
    {"java.game.parts.Part", "install_OK", "(Ljava.util.resource.GameRef;ILjava.util.resource.GameRef;ILjava.lang.Vector3;)[I", nullptr, true},
    {"java.game.parts.DynoData", "newNative", "()V", dd_new_native, false},
    {"java.game.parts.DynoData", "deleteNative", "()V", dd_delete_native, false},
    {"java.game.parts.DynoData", "calcDyno", "(F)F", dd_calc_dyno, false},
    {"java.game.parts.DynoData", "getTorque", "(FF)F", dd_get_torque, false},
    {"java.game.parts.DynoData", "getHP", "(FF)F", dd_get_hp, false},
    {"java.io.Controller", "user_Add", "(IIIFFFF)I", ctl_user_add, false},
    {"java.io.Controller", "user_Del", "(III)I", ctl_user_del, false},
    {"java.io.Controller", "user_SetAxisSmooth", "(IFFFFF)I", ctl_user_set_axis_smooth, false},
    {"java.io.Controller", "user_SetAxisSpeed", "(IF)V", ctl_user_set_axis_speed, false},
    {"java.io.Controller", "user_Reset", "()V", ctl_user_reset, false},
    {"java.io.Controller", "user_GetAxisVal", "(I)F", ctl_user_get_axis_val, false},
    {"java.io.Controller", "user_SetAxisForce", "(IFF)V", ctl_user_set_axis_force, false},
    {"java.game.GameLogic", "kismajomCheck", "([Ljava.lang.String;)I", nullptr, true},
    {"java.lang.System", "exit", "()V", system_exit, false},
    {"java.lang.System", "exit", "(Ljava.lang.String;)V", noop_void, false},
    {"java.lang.System", "log", "(Ljava.lang.String;)V", system_log, false},
    {"java.lang.System", "gc", "()I", system_gc, false},
    {"java.lang.System", "runFinalization", "()I", zero_int, false},
    {"java.lang.System", "analyze", "(Ljava.lang.Object;)V", noop_void, false},
    {"java.lang.System", "analyze", "(I)V", noop_void, false},
    {"java.lang.System", "analyze", "(F)V", noop_void, false},
    {"java.lang.System", "arraycopy", "(Ljava.lang.Object;ILjava.lang.Object;II)V", system_arraycopy, false},
    {"java.lang.System", "currentTime", "()F", system_current_time, false},
    {"java.lang.System", "simTime", "()F", system_sim_time, false},
    {"java.lang.System", "timeWarp", "(F)F", system_time_warp, false},
    {"java.lang.System", "openLib", "(Ljava.lang.String;)I", system_open_lib, false},
    {"java.lang.System", "compileAll", "(Ljava.lang.String;)I", system_compile_all, false},
    {"java.lang.System", "info", "(I)Ljava.lang.String;", system_info, false},
    {"java.lang.System", "buildNumber", "()I", system_build_number, false},
    {"java.lang.System", "setLdPriority", "(I)V", system_set_ld_priority, false},
    {"java.lang.System", "syncGameTime", "(F)V", system_sync_game_time, false},
    {"java.lang.System", "setMeasure", "(F)V", system_set_measure, false},
    {"java.lang.System", "getConfigOptions", "()V", system_get_config_options, false},
    {"java.lang.System", "isLoadingReset", "()V", system_is_loading_reset, false},
    {"java.lang.System", "isLoading", "()I", system_is_loading, false},
    {"java.lang.System", "memUsage", "()I", system_mem_usage, false},
    {"java.lang.Float", "toString", "(FLjava.lang.String;)Ljava.lang.String;", float_tostring, false},
    {"java.lang.Integer", "toString", "(I)Ljava.lang.String;", int_tostring, false},
    {"java.lang.Integer", "toHexString", "(I)Ljava.lang.String;", int_tohexstring, false},
    {"java.lang.String", "finalize", "()V", string_finalize, false},
    {"java.lang.String", "token", "(ILjava.lang.String;)Ljava.lang.String;", string_token, false},
    {"java.lang.String", "intValue", "()I", string_intvalue, false},
    {"java.lang.String", "floatValue", "()F", string_floatvalue, false},
    {"java.lang.String", "append", "(I)Ljava.lang.String;", string_append, false},
    {"java.lang.String", "length", "()I", string_length, false},
    {"java.lang.String", "getASCII", "()[I", string_getascii, false},
    {"java.lang.Vector3", "rotate", "(Ljava.lang.Ypr;)Ljava.lang.Vector3;", vec_rotate_ypr, false},
    {"java.lang.Vector3", "length", "()F", vec_length, false},
    {"java.lang.Vector3", "distance", "(Ljava.lang.Vector3;)F", vec_distance, false},
    {"java.lang.Vector3", "normalize", "()V", vec_normalize, false},
    {"java.lang.Vector3", "rotate", "(Ljava.lang.Vector3;F)Ljava.lang.Vector3;", vec_rotate_axis, false},
    {"java.lang.Vector3", "<init>", "(Ljava.lang.Ypr;)", vec_ctor_ypr, false},
    {"java.lang.Ypr", "<init>", "(Ljava.lang.Vector3;)", ypr_ctor_vector, false},
    {"java.lang.Ypr", "mul", "(Ljava.lang.Ypr;)V", ypr_mul, false},
    {"java.lang.Thread", "init", "(Ljava.lang.String;)V", thread_init, false},
    {"java.lang.Thread", "start", "()V", thread_start, false},
    {"java.lang.Thread", "stop", "()V", thread_stop, false},
    {"java.lang.Thread", "isAlive", "()I", thread_is_alive, false},
    {"java.lang.Thread", "getPriority", "()I", thread_get_priority, false},
    {"java.lang.Thread", "setPriority", "(I)V", thread_set_priority, false},
    {"java.lang.Thread", "sleep", "(F)V", thread_sleep, false},
    {"java.lang.Thread", "suspend", "()V", thread_suspend, false},
    {"java.lang.Thread", "resume", "()V", thread_resume, false},
    {"java.lang.Thread", "setDaemon", "(I)V", thread_set_daemon, false},
    // 0x4e0440/0x4dfc70/0x4dfc90: monitor natives over Scheduler::monitors; wait
    // appends the pumped thread and marks it waiting (0x54d430/0x564950), notify
    // wakes the last waiter, notifyAll drains the list (0x54d320/0x54d3a0/0x5629a0).
    {"java.lang.Object", "wait", "()V", object_wait, false},
    {"java.lang.Object", "notify", "()V", object_notify, false},
    {"java.lang.Object", "notifyAll", "()V", object_notify_all, false},
    {"java.lang.Object", "toString", "()Ljava.lang.String;", object_tostring, false},
    {"java.lang.Object", "disableGC", "()V", object_gc_toggle, false},
    {"java.lang.Object", "enableGC", "()V", object_gc_toggle, false},
    {"java.lang.Object", "hashCode", "()I", object_hashcode, false},
    {"java.lang.Math", "random", "()F", math_random, false},
    {"java.lang.Math", "sqrt", "(F)F", math_sqrt, false},
    {"java.lang.Math", "randomize", "()V", math_randomize, false},
    {"java.lang.Math", "setrandseed", "(I)V", math_setrandseed, false},
    {"java.lang.Math", "ln", "(F)F", math_ln, false},
    {"java.lang.Math", "log10", "(F)F", math_ln, false},
    {"java.lang.Math", "exp", "(F)F", math_exp, false},
    {"java.lang.Math", "pow", "(FF)F", math_pow, false},
    {"java.lang.Math", "atan2", "(FF)F", math_atan2, false},
    {"java.io.Input", "getAxis", "(II)F", in_get_axis, false},
    {"java.io.Input", "getDeviceName", "(I)Ljava.lang.String;", in_get_device_name, false},
    {"java.io.Input", "mapAxis", "(Ljava.util.resource.GameRef;IIIFFFF)V", in_map_axis, false},
    {"java.io.Input", "setAxisSmooth", "(Ljava.util.resource.GameRef;IFFFF)V", in_set_axis_smooth, false},
    {"java.io.Input", "activeAxis", "(I)I", in_active_axis, false},
    {"java.io.Input", "axisName", "(II)Ljava.lang.String;", in_axis_name, false},
    {"java.io.Input", "lastKey", "()I", in_last_key, false},
    {"java.io.Input", "createHotkey", "(IILjava.io.Hotkey;Ljava.util.resource.GameRef;Ljava.render.Osd;I)V", in_create_hotkey, false},
    {"java.io.Input", "deleteHotkey", "(Ljava.io.Hotkey;)V", in_delete_hotkey, false},
    {"java.io.Input", "checkHotkeys", "(Ljava.util.resource.GameRef;Ljava.render.Osd;)V", in_check_hotkeys, false},
    {"java.io.Input", "flushHotkeys", "()V", in_flush_hotkeys, false},
    {"java.io.MouseCursor", "getPickedPos", "()Ljava.lang.Vector3;", mc_get_picked_pos, false},
    {"java.io.MouseCursor", "getPos", "()Ljava.lang.Vector3;", mc_get_pos, false},
    {"java.io.MouseCursor", "getPickedPos", "(Ljava.lang.Vector3;)Ljava.lang.Vector3;", mc_get_picked_pos_out, false},
    {"java.io.MouseCursor", "getPos", "(Ljava.lang.Vector3;)Ljava.lang.Vector3;", mc_get_pos_out, false},
    {"java.io.File", "open", "(I)I", file_open, false},
    {"java.io.File", "close", "()V", file_close, false},
    {"java.io.File", "delete", "(Ljava.lang.String;)I", file_delete, false},
    {"java.io.File", "copy", "(Ljava.lang.String;Ljava.lang.String;)I", file_copy, false},
    {"java.io.File", "move", "(Ljava.lang.String;Ljava.lang.String;)I", file_move, false},
    {"java.io.File", "exists", "(Ljava.lang.String;)I", file_exists, false},
    {"java.io.File", "write", "(I)I", file_write_int, false},
    {"java.io.File", "write", "(F)I", file_write_int, false},
    {"java.io.File", "write", "(Ljava.util.resource.ResourceRef;)I", file_write_ref, false},
    {"java.io.File", "write", "(Ljava.lang.String;)I", file_write_string, false},
    {"java.io.File", "readInt", "()I", file_read_int, false},
    {"java.io.File", "readFloat", "()F", file_read_float, false},
    {"java.io.File", "readResID", "()I", file_read_res_id, false},
    {"java.io.File", "readString", "()Ljava.lang.String;", file_read_string, false},
    {"java.io.FindFile", "first", "(Ljava.lang.String;I)Ljava.lang.String;", ff_first, false},
    {"java.io.FindFile", "next", "()Ljava.lang.String;", ff_next, false},
    {"java.io.FindFile", "close", "()V", ff_close, false},
    {"java.sound.Sound", "changeMusicSet", "(I)V", snd_change_music_set, false},
    {"java.sound.Sound", "nextTrack", "()V", snd_next_track, false},
    {"java.sound.Sound", "prevTrack", "()V", snd_prev_track, false},
    {"java.sound.Sound", "firstTrack", "()V", snd_first_track, false},
    {"java.sound.Sound", "randomTrack", "()V", snd_random_track, false},
    {"java.sound.Sound", "playTrack", "(I)V", snd_play_track_int, false},
    {"java.sound.Sound", "playTrack", "(Ljava.lang.String;)V", snd_play_track_string, false},
    {"java.sound.Sound", "getTotalTracks", "()I", snd_get_total_tracks, false},
    {"java.sound.Sound", "getNowPlaying", "()I", snd_get_now_playing, false},
    {"java.sound.Sound", "stopMusic", "()V", snd_stop_music, false},
    {"java.sound.Sound", "getMode", "()I", snd_get_mode, false},
    {"java.sound.Sound", "setMode", "(I)I", snd_set_mode, false},
    {"java.sound.Sound", "setVolume", "(IF)V", snd_set_volume, false},
    {"java.sound.Sound", "getVolume", "(I)F", snd_get_volume, false},
    {"java.sound.Sound", "has3DHardware", "()I", snd_has_3d_hardware, false},
    {"java.sound.Sound", "hasMixHardware", "()I", snd_has_mix_hardware, false},
    {"java.sound.Sound", "enableDebugDump", "(Ljava.util.resource.ResourceRef;)I", snd_enable_debug_dump, false},
    {"java.render.GfxEngine", "forceRendering", "()V", gfx_force_rendering, false},
    {"java.render.GfxEngine", "printScreen", "(Ljava.lang.String;)V", gfx_print_screen, false},
    {"java.render.GfxEngine", "numDisplayModes", "()I", gfx_num_display_modes, false},
    {"java.render.GfxEngine", "currDisplayMode", "()I", gfx_curr_display_mode, false},
    {"java.render.GfxEngine", "displayModeName", "(I)Ljava.lang.String;", gfx_display_mode_name, false},
    {"java.render.GfxEngine", "changeVideoMode", "(III)V", gfx_change_video_mode, false},
    {"java.render.GfxEngine", "setGlobalEnvmap", "(Ljava.util.resource.ResourceRef;)V", gfx_set_global_envmap, false},
    {"java.render.GfxEngine", "flush", "()V", gfx_flush, false},
    {"java.render.GfxEngine", "renderOK", "()I", gfx_render_ok, false},
    {"java.render.GfxEngine", "openVideo", "(Ljava.lang.String;II)I", gfx_open_video, false},
    {"java.render.GfxEngine", "openVideo", "(Ljava.lang.String;III)I", gfx_open_video_ex, false},
    {"java.render.GfxEngine", "closeVideo", "()V", gfx_close_video, false},
    {"java.render.GfxEngine", "isPlayingVideo", "()I", gfx_is_playing_video, false},
    {"java.render.Camera", "create", "(Ljava.util.resource.ResourceRef;Ljava.render.Viewport;IFFFFFII)V", cam_create, false},
    {"java.render.Camera", "destroy", "()V", cam_destroy, false},
    {"java.render.Camera", "setFog", "(IFF)V", cam_set_fog, false},
    {"java.render.Viewport", "create", "(IFFFF)V", vp_create, false},
    {"java.render.Viewport", "destroy", "()V", vp_destroy, false},
    {"java.render.Viewport", "activate", "(I)V", vp_activate, false},
    {"java.render.Viewport", "deactivate", "()V", vp_deactivate, false},
    {"java.render.Viewport", "getAspect", "()F", vp_get_aspect, false},
    {"java.render.Viewport", "getWidth", "()F", vp_get_width, false},
    {"java.render.Viewport", "getHeight", "()F", vp_get_height, false},
    {"java.render.Viewport", "getTop", "()F", vp_get_top, false},
    {"java.render.Viewport", "getLeft", "()F", vp_get_left, false},
    {"java.render.Viewport", "unproject", "(Ljava.lang.Vector3;I)Ljava.lang.Vector3;", vp_unproject, false},
    {"java.render.Viewport", "resize", "(FFFF)V", vp_resize, false},
    {"java.render.Text", "setScale", "(FF)V", tx_set_scale, false},
    {"java.render.Text", "setDefaultColor", "(I)V", tx_set_default_color, false},
    {"java.render.Text", "getDefaultColor", "()I", tx_get_default_color, false},
    {"java.render.Text", "create", "(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;FF)V", tx_create, false},
    {"java.render.Text", "changeColor", "(I)V", tx_change_color, false},
    {"java.render.Text", "changeAlign", "(I)V", tx_change_align, false},
    {"java.render.Text", "setPos", "(FF)V", tx_set_pos, false},
    {"java.render.Text", "getPos", "()Ljava.lang.Vector3;", tx_get_pos, false},
    {"java.render.Text", "update", "()V", tx_update, false},
    {"java.render.Text", "getWidthPixels", "(Ljava.lang.String;Ljava.util.resource.ResourceRef;)F", tx_get_width_pixels, false},
    {"java.render.Text", "getPos", "(Ljava.lang.Vector3;)Ljava.lang.Vector3;", tx_get_pos_into, false},
    {"java.util.resource.Animation", "<init>", "(Ljava.util.resource.RenderRef;Ljava.util.resource.ResourceRef;)", anim_ctor, false},
    {"java.util.resource.Animation", "finalize", "()V", anim_finalize, false},
    {"java.util.resource.Animation", "setSpeed", "(F)V", anim_set_speed, false},
    {"java.util.resource.Animation", "setFade", "(F)V", anim_set_fade, false},
    {"java.util.resource.Animation", "getPos", "()F", anim_get_pos, false},
    {"java.util.resource.Animation", "seek", "(F)V", anim_seek, false},
    {"java.util.resource.Animation", "play", "()V", anim_play, false},
    {"java.util.resource.Animation", "loopPlay", "()V", anim_loop_play, false},
    {"java.util.resource.Animation", "pause", "()V", anim_pause, false},
    {"java.util.resource.ParticleSystem", "init", "(Ljava.util.resource.ResourceRef;Ljava.util.resource.RenderRef;Ljava.lang.String;)V", ps_init, false},
    {"java.util.resource.ParticleSystem", "stop", "()V", ps_stop, false},
    {"java.util.resource.ParticleSystem", "setFreq", "(F)V", ps_set_freq, false},
    {"java.util.resource.ParticleSystem", "setDirectSource", "(Ljava.lang.String;Ljava.lang.Vector3;FFLjava.lang.Vector3;FFFLjava.lang.String;)V", ps_set_direct_source, false},
    {"java.util.resource.ParticleSystem", "setSource", "(Ljava.lang.String;Ljava.lang.Vector3;FFLjava.lang.Vector3;FFFLjava.lang.String;)V", ps_set_source, false},
    {"java.util.resource.ParticleSystem", "delAction", "(Ljava.lang.String;)V", ps_del_action, false},
    {"java.util.resource.ParticleSystem", "setCounter", "(Ljava.lang.String;Ljava.lang.Vector3;F)V", ps_set_counter, false},
    {"java.util.resource.ParticleSystem", "getCounter", "(Ljava.lang.String;)I", ps_get_counter, false},
    {"java.util.resource.ParticleSystem", "modePermanent", "(I)V", ps_mode_permanent, false},
    {"java.lang.GameType", "remNotification", "(Ljava.util.resource.GameRef;I)V", gt_rem_notification, false},
    {"java.lang.GameType", "addNotification", "(Ljava.util.resource.GameRef;IILjava.lang.String;)V", gt_add_notification, false},
    {"java.lang.GameType", "addNotification", "(Ljava.util.resource.GameRef;IILjava.lang.String;Ljava.lang.String;)V", gt_add_notification_method, false},
    {"java.lang.GameType", "addTimer", "(FI)V", gt_add_timer, false},
    {"java.lang.GameType", "removeAllTimers", "()V", gt_remove_all_timers, false},
    {"java.lang.GameType", "setEventMask", "(I)V", gt_set_event_mask, false},
    {"java.lang.GameType", "clearEventMask", "(I)V", gt_clear_event_mask, false},
    {"java.lang.GameType", "registerCallback", "(I)V", gt_register_callback, false},
    {"java.lang.GameType", "unregisterCallback", "(I)V", gt_unregister_callback, false},
    {"java.lang.GameType", "unregisterCallbacks", "()V", gt_unregister_callbacks, false},
    {"java.lang.GameType", "createNativeInstance", "(Ljava.util.resource.GameRef;ILjava.lang.String;Ljava.lang.String;)V", gt_create_native_instance, true},
    {"java.util.resource.ResourceRef", "deleteNative", "()V", rr_delete_native, true},
    {"java.util.resource.ResourceRef", "newNative", "()V", rr_new_native, true},
    {"java.util.resource.ResourceRef", "set", "(I)V", rr_set_id, true},
    {"java.util.resource.ResourceRef", "set", "(Ljava.util.resource.ResourceRef;)V", rr_set_ref, true},
    {"java.util.resource.ResourceRef", "load", "()V", rr_load, false},
    {"java.util.resource.ResourceRef", "unload", "()V", rr_unload, false},
    {"java.util.resource.ResourceRef", "destroy", "()V", rr_destroy, false},
    {"java.util.resource.ResourceRef", "type", "()I", rr_type, true},
    {"java.util.resource.ResourceRef", "id", "()I", rr_id, true},
    {"java.util.resource.ResourceRef", "getParentID", "()I", rr_get_parent_id, true},
    {"java.util.resource.ResourceRef", "getParent", "()Ljava.util.resource.ResourceRef;", rr_get_parent, true},
    {"java.util.resource.ResourceRef", "getWTRoot", "()Ljava.util.resource.ResourceRef;", rr_get_wt_root, true},
    {"java.util.resource.ResourceRef", "getFirstChild", "()Ljava.util.resource.ResourceRef;", rr_get_first_child, true},
    {"java.util.resource.ResourceRef", "getNextChild", "()Ljava.util.resource.ResourceRef;", rr_get_next_child, true},
    {"java.util.resource.ResourceRef", "makeTexture", "(Ljava.util.resource.ResourceRef;Ljava.lang.String;)V", rr_make_texture, true},
    {"java.util.resource.ResourceRef", "makeSound", "(Ljava.util.resource.ResourceRef;Ljava.lang.String;)V", rr_make_sound, true},
    {"java.util.resource.ResourceRef", "cache", "()V", rr_cache, false},
    {"java.util.resource.ResourceRef", "precache", "()V", rr_precache, false},
    {"java.util.resource.ResourceRef", "duplicate", "(Ljava.util.resource.ResourceRef;)V", rr_duplicate, false},
    {"java.util.resource.ResourceRef", "scaleMesh", "(FFF)V", rr_scale_mesh, false},
    {"java.util.resource.GameRef", "create", "(Ljava.util.resource.GameRef;Ljava.util.resource.GameRef;Ljava.lang.String;Ljava.lang.String;)Ljava.lang.GameType;", gr_create, true},
    {"java.util.resource.GameRef", "create_native", "(Ljava.util.resource.GameRef;Ljava.util.resource.GameRef;Ljava.lang.String;Ljava.lang.String;)V", gr_create_native, true},
    {"java.util.resource.GameRef", "getFlags", "()I", gr_get_flags, true},
    {"java.util.resource.GameRef", "setFlags", "(I)V", gr_set_flags, true},
    {"java.util.resource.GameRef", "clearFlags", "(I)V", gr_clear_flags, true},
    {"java.util.resource.GameRef", "getPos", "()Ljava.lang.Vector3;", gr_get_pos, true},
    {"java.util.resource.GameRef", "getOri", "()Ljava.lang.Ypr;", gr_get_ori, true},
    {"java.util.resource.GameRef", "getVel", "()Ljava.lang.Vector3;", gr_get_vel, true},
    {"java.util.resource.GameRef", "getInfo", "(II)I", gr_get_info, false},
    {"java.util.resource.GameRef", "getInfo", "(ILjava.lang.String;)I", gr_get_info, false},
    {"java.util.resource.GameRef", "queueEvent", "(Ljava.util.resource.ResourceRef;ILjava.lang.String;)V", gr_queue_event, false},
    {"java.util.resource.GameRef", "setParent", "(Ljava.util.resource.ResourceRef;)V", gr_set_parent, true},
    {"java.util.resource.GameRef", "setPos", "(Ljava.lang.Vector3;)V", gr_set_pos, true},
    {"java.util.resource.GameRef", "setMatrix", "(Ljava.lang.Vector3;Ljava.lang.Ypr;)V", gr_set_matrix, true},
    {"java.util.resource.GameRef", "setState", "(Ljava.lang.Vector3;Ljava.lang.Ypr;Ljava.lang.Vector3;Ljava.lang.Vector3;)V", gr_set_state, true},
    {"java.util.resource.GameRef", "isEmpty", "()I", gr_is_empty, true},
    {"java.util.resource.GameRef", "isScripted", "(Ljava.lang.String;)I", gr_is_scripted, true},
    {"java.util.resource.GameRef", "getScriptInstance", "()Ljava.lang.Object;", gr_get_script_instance, true},
    {"java.util.resource.GameRef", "setActiveCollision", "()V", nullptr, true},
    {"java.util.resource.GameRef", "getDetail", "()F", gr_get_detail, true},
    {"java.util.resource.GameRef", "getPos", "(Ljava.lang.Vector3;)Ljava.lang.Vector3;", gr_get_pos_out, true},
    {"java.util.resource.GameRef", "getOri", "(Ljava.lang.Ypr;)Ljava.lang.Ypr;", gr_get_ori_out, true},
    {"java.util.resource.GameRef", "getVel", "(Ljava.lang.Vector3;)Ljava.lang.Vector3;", gr_get_vel_out, true},
    {"java.util.resource.GroundRef", "setWater", "(FFF)V", gr_set_water, false},
    {"java.util.resource.GroundRef", "setWater", "(Ljava.lang.Vector3;Ljava.lang.Vector3;FF)V", gr_set_water_v, false},
    {"java.util.resource.GroundRef", "addWaterLimit", "(Ljava.lang.Vector3;Ljava.lang.Vector3;)V", gr_add_water_limit, false},
    {"java.util.resource.GroundRef", "setFog", "(IFF)V", gr_set_fog, false},
    {"java.util.resource.GroundRef", "addTrafficCar", "(Ljava.util.resource.GameRef;Ljava.lang.Vector3;)I", gr_add_traffic_car, false},
    {"java.util.resource.GroundRef", "remTrafficCar", "(I)V", gr_rem_traffic_car, false},
    {"java.util.resource.GroundRef", "notifyTrafficCar", "(II)I", gr_notify_traffic_car, false},
    {"java.util.resource.GroundRef", "addTrafficN", "(Ljava.util.resource.GameRef;IFFF)I", gr_add_traffic_n, false},
    {"java.util.resource.GroundRef", "addTrafficP", "(Ljava.util.resource.GameRef;Ljava.lang.Vector3;IFFF)I", gr_add_traffic_p, false},
    {"java.util.resource.GroundRef", "delTraffic", "()V", gr_del_traffic, false},
    {"java.util.resource.GroundRef", "setTrafficCarBehaviour", "(II)V", gr_set_traffic_car_behaviour, false},
    {"java.util.resource.GroundRef", "haltTrafficCross", "(Ljava.lang.Vector3;F)V", gr_halt_traffic_cross, false},
    {"java.util.resource.GroundRef", "haltTrafficPath", "(Ljava.lang.Vector3;Ljava.lang.Vector3;)V", gr_halt_traffic_path, false},
    {"java.util.resource.GroundRef", "haltTrafficRoad", "(Ljava.lang.Vector3;Ljava.lang.Vector3;)V", gr_halt_traffic_road, false},
    {"java.util.resource.GroundRef", "clearHalts", "()V", gr_clear_halts, false},
    {"java.util.resource.GroundRef", "setTrafficCrossStopOffset", "(F)V", gr_set_traffic_cross_stop_offset, false},
    {"java.util.resource.GroundRef", "getNearestCross", "(Ljava.lang.Vector3;F)Ljava.lang.Vector3;", gr_get_nearest_cross, false},
    {"java.util.resource.GroundRef", "getStartDirection", "(Ljava.lang.Vector3;Ljava.lang.Vector3;)Ljava.lang.Vector3;", gr_get_start_direction, false},
    {"java.util.resource.GroundRef", "getRouteLength", "(Ljava.lang.Vector3;Ljava.lang.Vector3;)F", gr_find_route, false},
    {"java.util.resource.GroundRef", "alignToRoad", "(Ljava.lang.Vector3;)[Ljava.lang.Vector3;", gr_align_to_road, false},
    {"java.util.resource.GroundRef", "findRoute", "(Ljava.lang.Vector3;Ljava.lang.Vector3;)F", gr_find_route, false},
    {"java.util.resource.GroundRef", "getRouteLength", "()F", gr_get_route_length, false},
    {"java.util.resource.GroundRef", "getRoutePos", "(F)Ljava.lang.Vector3;", gr_get_route_pos, false},
    {"java.util.resource.GroundRef", "getRouteDist", "(Ljava.lang.Vector3;)F", gr_get_route_dist, false},
    {"java.util.resource.GroundRef", "setPedestrianDensityN", "(F)V", gr_set_pedestrian_density_n, false},
    {"java.util.resource.GroundRef", "addPedestrianType", "(Ljava.util.resource.GameRef;)V", gr_add_pedestrian_type, false},
    {"java.util.resource.GroundRef", "removePedestrianType", "(Ljava.util.resource.GameRef;)V", gr_remove_pedestrian_type, false},
    {"java.util.resource.GroundRef", "pedestrianDistance", "(Ljava.lang.Vector3;I)F", gr_pedestrian_distance, false},
    {"java.util.resource.GroundRef", "getTrafficCarPos", "(ILjava.lang.Vector3;)V", gr_get_traffic_car_pos, false},
    {"java.util.resource.GroundRef", "getPedestrianPos", "(Ljava.lang.Vector3;Ljava.lang.Vector3;I)V", gr_get_pedestrian_pos, false},
    {"java.util.resource.RenderRef", "create", "(Ljava.util.resource.ResourceRef;Ljava.util.resource.RenderRef;Ljava.lang.String;)V", rr_create, false},
    {"java.util.resource.RenderRef", "getBoneId", "(Ljava.lang.String;)I", rr_get_bone_id, false},
    {"java.util.resource.RenderRef", "setMatrix", "(ILjava.util.resource.ResourceRef;Ljava.lang.Vector3;Ljava.lang.Ypr;)V", rr_set_matrix_bone, false},
    {"java.util.resource.RenderRef", "setMatrix", "(Ljava.lang.Vector3;Ljava.lang.Ypr;)V", rr_set_matrix, false},
    {"java.util.resource.RenderRef", "changeResource", "(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;)V", rr_change_resource, false},
    {"java.util.resource.RenderRef", "setColor", "(I)V", rr_set_color, false},
    {"java.util.resource.RenderRef", "setLight", "(III)V", rr_set_light, false},
    {"java.util.resource.RenderRef", "setFlare", "(Ljava.util.resource.ResourceRef;IFFII)V", rr_set_flare, false},
    {"java.util.resource.RenderRef", "setType", "(Ljava.util.resource.ResourceRef;)V", rr_set_type, false},
    {"java.util.resource.RenderRef", "getTypeID", "()I", rr_get_type_id, false},
    {"java.util.resource.RenderRef", "getDetail", "()F", gr_get_detail, false},
    {"java.util.resource.RenderRef", "lineCreate", "(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;)I", rr_line_create, false},
    {"java.util.resource.RenderRef", "lineAdd", "(Ljava.lang.Vector3;Ljava.lang.Vector3;IF)V", rr_line_add, false},
    {"java.util.resource.RenderRef", "plotRoute", "(Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;IFLjava.lang.Vector3;)I", rr_plot_route, false},
    {"java.util.resource.PhysicsRef", "create", "(Ljava.util.resource.ResourceRef;ILjava.lang.String;)V", ph_create, false},
    {"java.util.resource.PhysicsRef", "createBox", "(Ljava.util.resource.ResourceRef;FFFLjava.lang.String;)V", ph_create_box, false},
    {"java.util.resource.PhysicsRef", "createSphere", "(Ljava.util.resource.ResourceRef;FLjava.lang.String;)V", ph_create_sphere, false},
    {"java.util.resource.PhysicsRef", "setMatrix", "(Ljava.lang.Vector3;Ljava.lang.Ypr;)V", ph_set_matrix, false},
    {"java.util.resource.PhysicsRef", "getPos", "()Ljava.lang.Vector3;", ph_get_pos, false},
    {"java.util.resource.PhysicsRef", "getOri", "()Ljava.lang.Ypr;", ph_get_ori, false},
    {"java.util.resource.SfxRef", "nplay", "(Ljava.lang.Vector3;FFFII)I", sfxref_nplay, false},
    {"java.util.resource.SfxRef", "stop", "(I)V", sfxref_stop, false},
    {"java.game.Painter", "paintPart", "(Ljava.util.resource.GameRef;I)V", painter_paint_part, false},
    {"java.game.Painter", "doPaint", "(Ljava.util.resource.GameRef;IIIFFI)V", painter_do_paint, false},
    {"java.game.Painter", "xPaint", "(Ljava.util.resource.GameRef;)V", painter_x_paint, false},
    {"java.game.Navigator", "updateNavigator", "(Ljava.util.resource.GameRef;I)V", nav_update_navigator, false},
    {"java.game.Vehicle", "getSpeedSquare", "()F", veh_get_speed_square, false},
    {"java.util.Replay", "writeFile", "(Ljava.lang.String;Ljava.lang.String;)V", debug_write_file, false},
    {"java.util.Replay", "readFile", "(ILjava.lang.String;Ljava.lang.String;)F", replay_read_file, false},
    {"java.lang.Debug", "writeFile", "(Ljava.lang.String;Ljava.lang.String;)V", debug_write_file, false},
    {"java.lang.Object", "errorVM", "(Ljava.lang.String;)V", object_error_vm, false},
    {"java.lang.Debug", "errorEX", "(Ljava.lang.String;)V", debug_error_ex, false},
    {"java.lang.Debug", "giveWarning", "(Ljava.lang.String;)V", debug_give_warning, false},
    {"java.lang.Object", "runDLL", "(Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;[Ljava.lang.String;)Ljava.lang.String;", nullptr, true},
    {"java.lang.Object", "runMethod", "(Ljava.lang.String;I)I", object_runmethod_int, false},
    {"java.lang.Object", "runMethod", "(Ljava.lang.String;F)I", object_runmethod_float, false},
    {"java.lang.Object", "runMethod", "(Ljava.lang.String;Ljava.lang.Object;)I", object_runmethod_ref, false},
    {"java.lang.Object", "runMethod", "(Ljava.lang.String;)I", object_runmethod_void, false},
    {"java.net.NetworkEngine", "httpRequest", "(Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;)Ljava.lang.String;", network_http_request, false},
    {"java.net.NetworkEngine", "httpRequestAsync", "(Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;Ljava.util.resource.GameRef;Ljava.lang.String;)I", zero_int, false},
    {"java.util.Steam", "initAPI", "()I", steam_init_api, false},
    {"java.util.Steam", "neutralize", "()V", steam_neutralize, false},
    {"java.util.Steam", "getPackage", "(I)Ljava.lang.String;", steam_get_package, false},
    {"java.util.Steam", "setAchievement", "(Ljava.lang.String;)V", steam_set_achievement, false},
    {"java.util.Steam", "resetAchievements", "()V", steam_reset_achievements, false},
    };
    return rows;
}

void install(Vm &vm)
{
    for (const Entry &e : table()) {
        Vm::NativeFn fn = e.fn;
        if (!fn) {
            std::string cls = e.cls, method = e.method, sig = e.sig;
            fn = [cls, method, sig](Vm &, const std::vector<Value> &, Value *,
                                    std::string *err) {
                *err = "native not implemented (stub): " + cls + "." + method + sig;
                return false;
            };
        }
        vm.register_native(e.cls, e.method, e.sig, std::move(fn));
    }
}

}  // namespace natives
