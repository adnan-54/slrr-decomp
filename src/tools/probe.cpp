// PE32 probe: one exported C entry point that drives the engine (TUFA parse via
// the platform file layer). The DLL links the whole engine archive, so this also
// proves every library object cross-compiles.
#include <string>

#include "platform/fs.h"
#include "script/classfile.h"

extern "C" __declspec(dllexport) int __cdecl engine_probe(const char *class_path)
{
    if (!class_path || !*class_path) return -1;
    std::string err;
    ClassFile file;
    if (!classfile_load(class_path, &file, &err)) return -2;
    return (int)file.units.size();
}
