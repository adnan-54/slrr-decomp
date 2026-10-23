// Boot path. See app/boot.h; registry formats in notes/16-package-registry.md,
// startup chain in notes/03-script-vm.md.
#include "app/boot.h"

#include <algorithm>
#include <set>

#include "audio/sound.h"
#include "input/controller.h"
#include "game/runtime.h"
#include "script/natives.h"

namespace {

const char *kEntryClass = "java.game.Init";
// The entry class is not named by any system.rpk record, so the boot resolves
// the shipped path the same way the loader does on disk.
const char *kEntryPath = "sl\\Scripts\\game\\Init.class";

}  // namespace

void Boot::reset()
{
    vm_.reset();
    classes_.reset();
    resources_.reset();
    result_ = BootResult();
    attempted_ = false;
}

const BootResult &Boot::run(const std::string &rpk)
{
    if (attempted_) return result_;
    attempted_ = true;
    result_.entry_class = kEntryClass;

    resources_ = std::make_unique<ResourceManager>();
    std::string err;
    if (!resources_->open(rpk.c_str(), nullptr, &err)) {
        result_.errors.push_back("open " + rpk + ": " + err);
        return result_;
    }
    result_.packages = resources_->package_count();

    classes_ = std::make_unique<ClassPath>();
    DataRef ref;
    ref.kind = DataRef::Script;
    ref.text = kEntryPath;
    ref.path = resources_->resolve_path(kEntryPath);
    ref.file = !ref.path.empty();
    ref.is_class = true;
    if (!ref.file) {
        result_.errors.push_back(std::string("entry class not found: ") + kEntryPath);
        return result_;
    }
    if (!resources_->load_class(ref, classes_.get(), &err)) {
        result_.errors.push_back("load " + ref.path + ": " + err);
        return result_;
    }
    classes_->link();

    vm_ = std::make_unique<Vm>();
    for (const ClassUnit *unit : classes_->units()) vm_->add_class(unit);

    ClassEntry *entry = classes_->find(kEntryClass);
    result_.entry_found = entry != nullptr && vm_->find_class(kEntryClass) != nullptr;
    if (!result_.entry_found) {
        result_.errors.push_back(std::string("entry class does not resolve: ") + kEntryClass);
        return result_;
    }

    // This class format has no merged <clinit>: each static field carries its
    // own initializer body. Run them once, in declaration order; the attempted
    // guard above makes that exactly once per runtime.
    // shortcut: run_field_init returns the value but does not mark the VM's
    // lazy static slots; a later static read would re-run the body. Upgrade
    // when the boot must hand off live statics, not just their value.
    result_.clinit_ran = true;
    bool first = true;
    for (const FieldInfo &f : entry->unit->fields) {
        if (!f.is_static || f.init < 0) continue;
        Value v;
        if (!vm_->run_field_init(*entry->unit, f.init, nullptr, &v, &err)) {
            const char *name = entry->unit->str(f.name);
            result_.errors.push_back(std::string("static initializer ") +
                                     (name ? name : "?") + ": " + err);
            result_.clinit_ran = false;
            break;
        }
        if (first) {
            result_.clinit_value = v;
            first = false;
        }
    }
    if (result_.clinit_ran) result_.clinit_runs++;

    // External-package gaps: classes the entry names but the opened packages do
    // not provide. Reported, never fatal (notes/16: most engine supers are
    // expected-missing here).
    result_.missing_links = classes_->missing_links();
    std::set<std::string> external;
    for (size_t i = 0; i < entry->unit->consts.size(); i++) {
        if (entry->unit->consts[i].type != CONST_CLASS) continue;
        std::string name = entry->unit->class_name((int)i);
        if (!name.empty() && !classes_->find(name)) external.insert(name);
    }
    if (!entry->super_name.empty() && !entry->super) external.insert(entry->super_name);
    for (const std::string &name : entry->interface_names)
        if (!classes_->find(name)) external.insert(name);
    result_.external.assign(external.begin(), external.end());

    result_.ok = result_.packages > 0 && result_.entry_found && result_.clinit_ran &&
                 result_.errors.empty();
    return result_;
}

// 0x47d760 control gate.
void FrameLoop::frame(int now_ms)
{
    Runtime *rt = Runtime::active();
    if (rt && rt->vm()) natives::set_clock(*rt->vm(), now_ms);

    if (now_ms - last_control_ms_ >= 16) {  // exe: cmp eax,0x10; jl
        float dt = (float)(now_ms - last_control_ms_) * 0.001f;
        input::update(dt);  // 0x566160
        if (rt) {
            rt->flush_deferred();  // 0x47d070 (end of sim step 0x47e050)
            if (rt->vm()) {
                natives::advance_sim(*rt->vm(), dt);  // 0x47e050 clock part
                rt->run_timers((float)natives::sim_time(*rt->vm()));  // 0x47b700
            }
        }
        last_control_ms_ = now_ms;
    }

    // 0x47d760: VM tick every >= 8 ms; last_vm_ms_ advances even without a VM.
    if (now_ms - last_vm_ms_ >= 8) {  // exe: cmp eax,8; jl
        // INVENTED: game+0x234 is treated as set whenever a VM exists;
        // 0x55cf20's GC work is not ported, only its runThreads call.
        if (rt && rt->vm()) natives::run_threads(*rt->vm(), 10.0f, 0);
        last_vm_ms_ = now_ms;
    }

    // 0x47d760: per-frame deferred flush, then listener + voice service,
    // both outside the 15 ms control gate.
    if (rt) rt->flush_deferred();  // 0x47d070
    // No camera in src = the exe's DAT_005f1e44 <= 0 branch: listener at the origin.
    audio::SfxService &sfx = audio::sfx_service();
    const float listener[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 3; i++) sfx.pool.listener[i] = listener[i];  // 0x568bb0
    sfx.mixer.set_listener(listener);                                 // 0x546cd0
    sfx.pool.service(*sfx.backend);                                   // 0x5690a0

    // 0x47dc80: wake scripts waiting on java.render.Frontend.render.
    if (rt && rt->vm() && rt->vm()->find_class("java.render.Frontend")) {
        Value v;
        std::string e;
        if (rt->vm()->get_static_field("java.render.Frontend", "render", &v, &e) &&
            v.kind == Value::Obj && v.obj)
            natives::monitor_notify_all(*rt->vm(), v.obj);  // 0x54d3a0
    }
}
