// Script-level boot path: mount the shipped system package, load the entry
// class (sl\Scripts\game\Init.class), register its units in a fresh VM and run
// the class's static initializers once. Mirrors the exe's script startup chain
// as far as the reimplemented runtime reaches; engine services are not started.
#pragma once

#include <stddef.h>

#include <memory>
#include <string>
#include <vector>

#include "resource/manager.h"
#include "script/vm.h"

struct BootResult {
    bool ok = false;
    std::string entry_class;              // FQN the boot looked for
    size_t packages = 0;                  // opened .rpk packages
    bool entry_found = false;             // class loaded and registered in the VM
    bool clinit_ran = false;              // every static initializer completed
    int clinit_runs = 0;                  // 1 per fresh runtime (exactly once)
    Value clinit_value;                   // result of the first static initializer
    size_t missing_links = 0;             // registry super/interface links left external
    std::vector<std::string> external;    // classes referenced by the entry but not loaded
    std::vector<std::string> errors;
};

class Boot {
  public:
    // Runs the sequence once per runtime. A second call without reset() returns
    // the cached result and does not re-run static initializers.
    const BootResult &run(const std::string &rpk);
    // Destroys the runtime so the next run() starts from scratch.
    void reset();

    const BootResult &result() const { return result_; }
    // Runtime handles; null before run() and after reset().
    ResourceManager *resources() { return resources_.get(); }
    ClassPath *classes() { return classes_.get(); }
    Vm *vm() { return vm_.get(); }

  private:
    std::unique_ptr<ResourceManager> resources_;
    std::unique_ptr<ClassPath> classes_;
    std::unique_ptr<Vm> vm_;
    BootResult result_;
    bool attempted_ = false;
};

// Main-loop control gate of 0x47d760: every frame with more than 15 ms since
// the last control step runs input::update (0x566160) with that interval.
class FrameLoop {
  public:
    explicit FrameLoop(int start_ms) : last_control_ms_(start_ms) {}
    // now_ms: the 0x569730 millisecond clock truncated to int.
    void frame(int now_ms);
    int last_control_ms() const { return last_control_ms_; }

  private:
    int last_control_ms_;
};
