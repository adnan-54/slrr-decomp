// Native method registry: one row per analysis/natives.tsv entry, keyed by
// (class, method, descriptor) like the exe's native table (notes/06).
#pragma once
#include <string>
#include <vector>

#include "game/runtime.h"
#include "script/vm.h"

namespace input {
class Controller;
}

namespace natives {

struct Entry {
    const char *cls;
    const char *method;
    const char *sig;
    Vm::NativeFn fn;  // empty when the method is stubbed
    bool engine;      // explicit stub (engine-dependent or not yet reconstructed)
};

const std::vector<Entry> &table();
void install(Vm &vm);
// 0x60251c: process world time, the clock behind System.simTime (0x4e5810).
double sim_time(Vm &vm);
// 0x55caf0 JavaMachine::runThreads: one pass over the thread run list.
void run_threads(Vm &vm, float budget_ms, int min_prio);
// 0x5612f0 + 0x562d70(0): register a scheduler thread (prio, flags 0) and run it
// untimed until it finishes or a flag stops it; returns false if it failed.
bool spawn_untimed(Vm &vm, std::unique_ptr<Thread> runner, int32_t priority,
                   const std::string &name);
// 0x569730: the frame loop's millisecond clock, read by Thread.sleep.
void set_clock(Vm &vm, double now_ms);
// 0x54d3a0: drain an object's monitor wait list (Object.notifyAll body).
void monitor_notify_all(Vm &vm, Object *o);
// 0x47e050: sub-step the process world by min(warp * dt, 0.1f), timers + flush each sub-step.
void advance_sim(Vm &vm, float dt);
// The Controller block behind a `native player` resource (0x4d3cb0's object),
// created on first use; the control list reads its axes (0x498300 ax/ai).
input::Controller *controller_of(Runtime::Resource *r);
// Read-only snapshot of the VM's script threads for checks and tools.
struct ThreadInfo {
    std::string name;
    bool started = false, done = false, sleeping = false, waiting = false,
         suspended = false;
    std::string error;  // runner->error() when the thread Failed
    std::string where;  // innermost frame of a live thread
    Object *wait_on = nullptr;  // Object.wait monitor while waiting (thread+0x34)
};
std::vector<ThreadInfo> thread_report(Vm &vm);

}  // namespace natives
