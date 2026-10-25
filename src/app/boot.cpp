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
    runtime_.reset();  // owns handles into resources_, so it goes first
    game_init_ = Runtime::Handle();
    vm_.reset();
    tree_.reset();
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

    // 0x47d760: the game runtime over the booted VM (resource ids/instances).
    // Natives dispatch through it once active.
    runtime_ = std::make_unique<Runtime>(*resources_, vm_.get(), classes_.get());
    runtime_->activate();

    // Package tree over the game directory (the .rpk's own directory). 0x55ccc0
    // installs the roots and resolves the two startup classes.
    size_t sep = rpk.find_last_of("\\/");
    std::string game_dir = sep == std::string::npos ? std::string(".") : rpk.substr(0, sep);
    tree_ = std::make_unique<jvm::PackageTree>(game_dir);
    tree_->add_default_roots();
    vm_->set_class_loader([this](Vm &vm, const std::string &fqn) {
        return jvm::load_class(*tree_, *classes_, vm, fqn);
    });
    // 0x51fcc0 -> 0x55aca0 -> 0x55bec0: a record's `script <dir>\Name.class`
    // loads that file and registers its package with dir; the Class is fresh
    // per record.
    runtime_->path_class_loader = [this](const std::string &dir, const std::string &base) {
        return jvm::load_class_by_path(*tree_, *classes_, *vm_, dir, base);
    };
    vm_->get_class("java.lang.String");
    vm_->get_class("java.util.resource.GameRef");

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

// 0x47d760 init step, after the entry class: GetConfigData("init") (0x47c110)
// yields the resource id of the game type; 0x523c80 creates a scripted instance
// with alias "GameInit"; 0x40cac0 binds it to game+0x238.
bool Boot::game_init(std::string *err)
{
    if (!runtime_ || !vm_) {
        *err = "game_init: boot not run";
        return false;
    }
    // INVENTED: the exe reads the field through the config game object's script
    // instance (0x5fe0e4, created by engine startup, not ported). src reads the
    // static field directly; its initializer (`static int init = sl:0x0040r`)
    // yields the global id through the resource resolver (cards 105/106).
    Value v;
    std::string e;
    if (!vm_->get_static_field("java.util.Config", "init", &v, &e)) {
        *err = "GetConfigData: Config data not present (" + e + ")";
        return false;
    }
    if (v.kind != Value::Int || v.i == 0) {
        *err = "GetConfigData: Config data not present";
        return false;
    }
    Runtime::Resource *type = runtime_->resolve((uint32_t)v.i);
    if (!type) {
        *err = "init: resource id " + std::to_string((uint32_t)v.i) + " does not resolve";
        return false;
    }
    Runtime::Resource *inst =
        runtime_->instantiate(type, runtime_->game_root(), "GameInit", "", true, &e);
    if (!inst) {
        *err = "init: instantiate GameInit: " + e;
        return false;
    }
    runtime_->bind(&game_init_, inst);
    runtime_->ensure_script_class(inst);
    result_.game_init_type = type->name;
    result_.game_init_class = inst->script_class;
    result_.game_init_created = true;
    return true;
}

// 0x47d760 control gate.
void FrameLoop::frame(int now_ms)
{
    Runtime *rt = Runtime::active();
    if (rt && rt->vm()) natives::set_clock(*rt->vm(), now_ms);

    if (now_ms - last_control_ms_ >= 16) {  // exe: cmp eax,0x10; jl
        float dt = (float)(now_ms - last_control_ms_) * 0.001f;
        input::update(dt);  // 0x566160
        // 0x47e050 sub-steps the world and runs timers + flush per sub-step.
        if (rt && rt->vm())
            natives::advance_sim(*rt->vm(), dt);
        else if (rt)
            rt->flush_deferred();  // exe's no-step path: exactly one flush
        last_control_ms_ = now_ms;
    }

    // 0x47d760: VM tick every >= 8 ms; last_vm_ms_ advances even without a VM.
    if (now_ms - last_vm_ms_ >= 8) {  // exe: cmp eax,8; jl
        // INVENTED: game+0x234 is treated as set whenever a VM exists;
        // 0x55cf20's GC work is not ported, only its runThreads call.
        if (rt && rt->vm()) natives::run_threads(*rt->vm(), 10.0f, 0);
        last_vm_ms_ = now_ms;
    }

    // 0x47d760: animate pass over the mode-0x1c list, then the per-frame
    // deferred flush, then listener + voice service, outside the 15 ms gate.
    // frame_dt = (now - previous frame) * 0.001, capped at 0.1 (0x5da468).
    if (rt) {
        const float frame_dt =
            std::min((float)(now_ms - last_frame_ms_) * 0.001f, 0.1f);
        last_frame_ms_ = now_ms;
        rt->run_animate_callbacks(frame_dt);  // 0x47ba10
        rt->flush_deferred();                 // 0x47d070
    } else {
        last_frame_ms_ = now_ms;
    }
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
