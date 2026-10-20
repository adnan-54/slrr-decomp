// Native method registry: one row per analysis/natives.tsv entry, keyed by
// (class, method, descriptor) like the exe's native table (notes/06).
#pragma once
#include <vector>

#include "script/vm.h"

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

}  // namespace natives
