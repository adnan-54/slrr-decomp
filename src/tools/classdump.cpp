// classdump: dump TUFA .class constants/fields/methods, run parser+VM self-checks
// and structural conformance over a directory tree of shipped classes.
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "game/car.h"
#include "game/runtime.h"
#include "platform/fs.h"
#include "resource/manager.h"
#include "script/classfile.h"
#include "script/natives.h"
#include "script/vm.h"

static std::string flags_text(uint32_t f)
{
    std::string s;
    if (f & 0x01) s += "public ";
    if (f & 0x08) s += "static ";
    if (f & 0x20) s += "final ";
    if (f & 0x40) s += "native ";
    char hex[16];
    snprintf(hex, sizeof hex, "0x%x", f);
    return std::string("flags=") + hex + (s.empty() ? "" : " (" + s.substr(0, s.size() - 1) + ")");
}

static void dump_unit(const ClassUnit &u)
{
    std::string cls = u.class_name(u.self_class);
    std::string sup = u.super_class >= 0 ? u.class_name(u.super_class) : std::string();
    std::printf("unit @%u: version %u stamp 0x%08x\n", u.file_offset, u.version, u.stamp);
    std::printf("  class %s%s%s\n", cls.c_str(),
                sup.empty() ? "" : " extends ", sup.c_str());
    std::printf("  constants: %zu\n", u.consts.size());
    for (size_t i = 0; i < u.consts.size(); i++)
        std::printf("    #%zu %s\n", i, u.describe((int)i).c_str());
    std::printf("  fields: %zu\n", u.fields.size());
    for (const FieldInfo &f : u.fields) {
        std::printf("    %s %s %s %s init=%d\n", f.is_static ? "static" : "instance",
                    flags_text(f.flags).c_str(),
                    u.str(f.name) ? u.str(f.name) : "?",
                    u.str(f.type) ? u.str(f.type) : "?", f.init);
    }
    std::printf("  methods: %zu\n", u.methods.size());
    for (const MethodInfo &m : u.methods) {
        unsigned instrs = 0, bytes = 0;
        if (m.body >= 0 && (size_t)m.body < u.bodies.size()) {
            instrs = (unsigned)u.bodies[(size_t)m.body].instrs.size();
            bytes = u.bodies[(size_t)m.body].byte_length;
        }
        char body_len[64];
        if (m.body < 0)
            snprintf(body_len, sizeof body_len, "no body");
        else
            snprintf(body_len, sizeof body_len, "%u instrs, %u bytes", instrs, bytes);
        std::printf("    %s %s %s %s body=%d (%s) locals=%d\n",
                    m.is_static ? "static" : "instance", flags_text(m.flags).c_str(),
                    u.str(m.name) ? u.str(m.name) : "?",
                    u.str(m.sig) ? u.str(m.sig) : "?", m.body, body_len, m.locals);
    }
    std::printf("  bodies: %zu\n", u.bodies.size());
    for (size_t i = 0; i < u.bodies.size(); i++)
        std::printf("    body %zu: %zu instrs, %u bytes\n", i,
                    u.bodies[i].instrs.size(), u.bodies[i].byte_length);
}

static int dump_files(int argc, char **argv)
{
    int failures = 0;
    for (int i = 0; i < argc; i++) {
        ClassFile file;
        std::string err;
        if (!classfile_load(argv[i], &file, &err)) {
            std::fprintf(stderr, "%s: %s\n", argv[i], err.c_str());
            failures++;
            continue;
        }
        std::printf("file %s: %zu unit(s)\n", argv[i], file.units.size());
        for (const ClassUnit &u : file.units)
            dump_unit(u);
    }
    return failures ? 1 : 0;
}

// ---- self-check ------------------------------------------------------------

struct Checker {
    int failures = 0;
    void check(bool ok, const std::string &what)
    {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        if (!ok) failures++;
    }
};

static bool read_binary(const std::string &path, std::vector<uint8_t> *out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}

static bool read_text(const std::string &path, std::string *out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}

static bool ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$';
}

// Whole-identifier match, so "name" does not match inside "surname".
static bool has_ident(const std::string &src, const std::string &id)
{
    if (id.empty()) return false;
    size_t p = 0;
    while ((p = src.find(id, p)) != std::string::npos) {
        bool left = p == 0 || !ident_char(src[p - 1]);
        size_t end = p + id.size();
        bool right = end >= src.size() || !ident_char(src[end]);
        if (left && right) return true;
        p = end;
    }
    return false;
}

// Identifier followed (after spaces) by '(', i.e. a declaration or call site.
static bool has_call(const std::string &src, const std::string &id)
{
    if (id.empty()) return false;
    size_t p = 0;
    while ((p = src.find(id, p)) != std::string::npos) {
        bool left = p == 0 || !ident_char(src[p - 1]);
        size_t q = p + id.size();
        while (q < src.size() && (src[q] == ' ' || src[q] == '\t' || src[q] == '\n' ||
                                  src[q] == '\r'))
            q++;
        if (left && q < src.size() && src[q] == '(') return true;
        p += id.size();
    }
    return false;
}

static std::string simple_name(const std::string &qualified)
{
    size_t dot = qualified.rfind('.');
    return dot == std::string::npos ? qualified : qualified.substr(dot + 1);
}

static void crosscheck_java(const ClassUnit &u, const std::string &class_path, Checker &c)
{
    std::string java_path = class_path;
    if (java_path.size() > 6 && java_path.compare(java_path.size() - 6, 6, ".class") == 0)
        java_path.resize(java_path.size() - 6);
    java_path += ".java";
    std::string src;
    if (!read_text(java_path, &src)) {
        std::printf("  -- no sibling source %s (parse checks only)\n", java_path.c_str());
        return;
    }
    std::string cls = u.class_name(u.self_class);
    std::string simple = simple_name(cls);
    c.check(has_ident(src, simple), "class name '" + simple + "' appears in source");
    if (u.super_class >= 0) {
        std::string sup = simple_name(u.class_name(u.super_class));
        c.check(has_ident(src, sup), "superclass name '" + sup + "' appears in source");
    }
    bool names_ok = true;
    for (const FieldInfo &f : u.fields) {
        const char *n = u.str(f.name);
        if (!n || !has_ident(src, n)) {
            names_ok = false;
            std::printf("  FAIL field '%s' missing from source\n", n ? n : "?");
        }
    }
    c.check(names_ok, "all declared field names appear in source");
    names_ok = true;
    for (const MethodInfo &m : u.methods) {
        const char *n = u.str(m.name);
        std::string want = n && strcmp(n, "<init>") == 0 ? simple : (n ? n : "");
        if (!n || !has_call(src, want)) {
            names_ok = false;
            std::printf("  FAIL method '%s' missing from source\n", n ? n : "?");
        }
    }
    c.check(names_ok, "all declared method names appear as calls/declarations in source");

    // Members reached through the class itself (inherited fields used by the
    // constructor, self calls) must be visible in the source too.
    int refs = 0, bad = 0;
    for (size_t i = 0; i < u.consts.size(); i++) {
        const ConstEntry &e = u.consts[i];
        if (e.type != CONST_MEMBER || u.member_class((int)i) != cls)
            continue;
        refs++;
        std::string n = u.member_name((int)i);
        if (!has_ident(src, n)) {
            bad++;
            std::printf("  FAIL self member ref '%s' missing from source\n", n.c_str());
        }
    }
    c.check(bad == 0, "all " + std::to_string(refs) +
                          " self-class member refs appear in source");
}

// The parser's contract is that every chunk, body and instruction is consumed
// exactly. Prove it from the outside too: appending garbage or truncating the
// file must be rejected.
static void check_byte_accounting(const std::vector<uint8_t> &data, Checker &c)
{
    ClassFile tmp;
    std::string err;
    std::vector<uint8_t> bad = data;
    bad.push_back(0);
    c.check(!classfile_parse(bad.data(), bad.size(), &tmp, &err),
            "rejects one trailing garbage byte");
    bad = data;
    bad.pop_back();
    c.check(!classfile_parse(bad.data(), bad.size(), &tmp, &err),
            "rejects a truncated file");
}

static void selfcheck_file(const char *path, Checker &c)
{
    std::printf("selfcheck %s\n", path);
    std::vector<uint8_t> data;
    if (!read_binary(path, &data)) {
        c.check(false, "file readable");
        return;
    }
    c.check(true, "file readable (" + std::to_string(data.size()) + " bytes)");

    ClassFile file;
    std::string err;
    bool ok = classfile_parse(data.data(), data.size(), &file, &err);
    c.check(ok, "strict parse" + (ok ? std::string() : ": " + err));
    if (!ok) return;
    c.check(true, "all bytes consumed, no trailing garbage");
    check_byte_accounting(data, c);

    bool units_ok = !file.units.empty();
    c.check(units_ok, std::to_string(file.units.size()) + " class unit(s)");
    for (const ClassUnit &u : file.units) {
        std::string cls = u.class_name(u.self_class);
        c.check(!cls.empty(), "unit @" + std::to_string(u.file_offset) + " self class resolves: " + cls);

        // Every body in TREE must be reachable from a field initializer or method.
        int max_body = -1;
        for (const FieldInfo &f : u.fields)
            if (f.init > max_body) max_body = f.init;
        for (const MethodInfo &m : u.methods)
            if (m.body > max_body) max_body = m.body;
        c.check(max_body + 1 == (int)u.bodies.size(),
                "every TREE body referenced (max index " + std::to_string(max_body) +
                    ", " + std::to_string(u.bodies.size()) + " bodies)");

        crosscheck_java(u, path, c);

        // Real bytecode through the VM: Init's static field initializer is
        // `const-wrapper 100; store-to-init-slot`.
        if (cls == "java.game.Init") {
            const FieldInfo *target = nullptr;
            for (const FieldInfo &f : u.fields)
                if (f.is_static && u.str(f.name) &&
                    strcmp(u.str(f.name), "STEAM_THREAD_CBK_LATENCY") == 0)
                    target = &f;
            c.check(target != nullptr, "Init.STEAM_THREAD_CBK_LATENCY field found");
            if (target) {
                Vm vm;
                vm.add_class(&u);
                Value v;
                std::string verr;
                bool ran = vm.run_field_init(u, target->init, nullptr, &v, &verr);
                c.check(ran, "vm runs static field initializer" +
                                 (ran ? std::string() : ": " + verr));
                c.check(ran && v.kind == Value::Int && v.i == 100,
                        "vm field initializer yields 100");
            }
        }
    }
}

// ---- synthetic VM programs -------------------------------------------------

// TREE body builder. Jump operands are relative to the index of the jump token,
// so forward branches are written with a placeholder and patched once the target
// index is known.
struct Code {
    std::vector<Instr> ins;
    size_t mark() const { return ins.size(); }
    void op(uint8_t op, uint32_t operand = 0)
    {
        Instr i;
        i.op = op;
        i.line = 1;
        i.operand = operand;
        i.has_operand = opcode_operand_size(op) > 0;
        ins.push_back(i);
    }
    void patch(size_t at, size_t target)
    {
        ins[at].operand = (uint32_t)((int64_t)target - (int64_t)at);
    }
};

struct UnitBuilder {
    ClassUnit u;

    int add_str(const std::string &s)
    {
        ConstEntry e;
        e.type = CONST_STRING;
        e.str = s;
        u.consts.push_back(e);
        return (int)u.consts.size() - 1;
    }
    int add_class(const std::string &name)
    {
        ConstEntry e;
        e.type = CONST_CLASS;
        e.a = add_str(name);
        u.consts.push_back(e);
        return (int)u.consts.size() - 1;
    }
    int add_member(const std::string &cls, const std::string &name, const std::string &desc)
    {
        int c = add_class(cls);
        ConstEntry nt;
        nt.type = CONST_NAME_TYPE;
        nt.a = add_str(name);
        nt.b = add_str(desc);
        u.consts.push_back(nt);
        int n = (int)u.consts.size() - 1;
        ConstEntry m;
        m.type = CONST_MEMBER;
        m.a = c;
        m.b = n;
        u.consts.push_back(m);
        return (int)u.consts.size() - 1;
    }
    void add_field(bool is_static, const std::string &name, const std::string &desc,
                   int init = -1)
    {
        FieldInfo f;
        f.is_static = is_static;
        f.flags = 0x1;
        f.name = add_str(name);
        f.type = add_str(desc);
        f.init = init;
        u.fields.push_back(f);
    }
    void add_method(bool is_static, uint32_t flags, const std::string &name,
                    const std::string &sig, int body, int locals)
    {
        MethodInfo m;
        m.is_static = is_static;
        m.flags = flags;
        m.name = add_str(name);
        m.sig = add_str(sig);
        m.body = body;
        m.locals = locals;
        u.methods.push_back(m);
    }
    int add_body(const Code &c)
    {
        Body b;
        b.instrs = c.ins;
        b.byte_length = 0;
        for (const Instr &i : c.ins) {
            int sz = opcode_operand_size(i.op);
            b.byte_length += 3 + (sz > 0 ? (uint32_t)sz : 0);
        }
        u.bodies.push_back(b);
        return (int)u.bodies.size() - 1;
    }
    ClassUnit finish(const std::string &name, const std::string &super)
    {
        u.version = 4;
        u.stamp = 0;
        u.clss_unknown = -1;
        u.interface_flag = 0;
        u.self_class = add_class(name);
        u.super_class = super.empty() ? -1 : add_class(super);
        u.has_fields = !u.fields.empty();
        u.has_methods = !u.methods.empty();
        u.has_tree = !u.bodies.empty();
        return u;
    }
};

static const MethodInfo *find_method_sig(const ClassUnit &u, const char *name,
                                         const char *sig)
{
    for (const MethodInfo &m : u.methods) {
        const char *n = u.str(m.name);
        const char *s = u.str(m.sig);
        if (n && s && strcmp(n, name) == 0 && strcmp(s, sig) == 0) return &m;
    }
    return nullptr;
}

static Value vi(int32_t v)
{
    return Value::make_int(v);
}

static Value vf(float v)
{
    return Value::make_float(v);
}

static bool call_static(Vm &vm, const ClassUnit &u, const char *name, const char *sig,
                        const std::vector<Value> &args, Value *out, std::string *err)
{
    const MethodInfo *m = find_method_sig(u, name, sig);
    if (!m) {
        *err = std::string("missing method ") + name + sig;
        return false;
    }
    return vm.invoke_static(u, *m, args, out, err);
}

static bool call_instance(Vm &vm, const ClassUnit &u, const char *name, const char *sig,
                          Object *self, const std::vector<Value> &args, Value *out,
                          std::string *err)
{
    const MethodInfo *m = find_method_sig(u, name, sig);
    if (!m) {
        *err = std::string("missing method ") + name + sig;
        return false;
    }
    return vm.invoke(u, *m, self, args, out, err);
}

static bool call_native(Vm &vm, const ClassUnit &u, const char *name, const char *sig,
                        const Value *receiver, const std::vector<Value> &args, Value *out,
                        std::string *err)
{
    const MethodInfo *m = find_method_sig(u, name, sig);
    if (!m) {
        *err = std::string("missing native ") + name + sig;
        return false;
    }
    return vm.invoke_native(u, *m, receiver, args, out, err);
}

static bool obj_float(Vm &vm, Object *o, const char *name, float *out, std::string *err)
{
    Value v;
    if (!vm.get_instance_field(o, name, &v, err)) return false;
    if (v.kind != Value::Float) {
        *err = std::string(name) + " is not a float";
        return false;
    }
    *out = v.f;
    return true;
}

static void expect_int(Checker &c, bool ran, const Value &v, int32_t want,
                       const std::string &what, const std::string &err)
{
    c.check(ran, what + (ran ? std::string() : ": " + err));
    if (ran)
        c.check(v.kind == Value::Int && v.i == want,
                what + " == " + std::to_string(want));
}

static void expect_float(Checker &c, bool ran, const Value &v, float want, float tol,
                         const std::string &what, const std::string &err)
{
    c.check(ran, what + (ran ? std::string() : ": " + err));
    if (ran) {
        char buf[64];
        snprintf(buf, sizeof buf, "%.7g +/- %g", (double)want, (double)tol);
        c.check(v.kind == Value::Float && std::fabs(v.f - want) <= tol,
                what + " == " + buf);
    }
}

static void expect_str(Checker &c, bool ran, const Value &v, const std::string &want,
                       const std::string &what, const std::string &err)
{
    c.check(ran, what + (ran ? std::string() : ": " + err));
    if (ran)
        c.check(v.kind == Value::Str && v.s == want, what + " == \"" + want + "\"");
}

// ---- real corpus programs --------------------------------------------------

static const ClassUnit *find_unit(const std::vector<ClassFile> &files, const char *name)
{
    for (const ClassFile &f : files)
        for (const ClassUnit &u : f.units)
            if (u.class_name(u.self_class) == name) return &u;
    return nullptr;
}

static Object *make_vector(Vm &vm, const ClassUnit &u, float x, float y, float z,
                           std::string *err)
{
    ClassState *cs = vm.find_class("java.lang.Vector3");
    if (!cs) {
        *err = "java.lang.Vector3 is not registered";
        return nullptr;
    }
    const MethodInfo *ctor = find_method_sig(u, "<init>", "(FFF)");
    if (!ctor) {
        *err = "Vector3(float,float,float) constructor missing";
        return nullptr;
    }
    Object *obj = vm.new_object(cs, err);
    if (!obj) return nullptr;
    Value r;
    if (!vm.invoke(u, *ctor, obj, {vf(x), vf(y), vf(z)}, &r, err)) return nullptr;
    return obj;
}

static Object *make_ypr(Vm &vm, const ClassUnit &u, float y, float p, float r,
                        std::string *err)
{
    ClassState *cs = vm.find_class("java.lang.Ypr");
    if (!cs) {
        *err = "java.lang.Ypr is not registered";
        return nullptr;
    }
    const MethodInfo *ctor = find_method_sig(u, "<init>", "(FFF)");
    if (!ctor) {
        *err = "Ypr(float,float,float) constructor missing";
        return nullptr;
    }
    Object *obj = vm.new_object(cs, err);
    if (!obj) return nullptr;
    Value res;
    if (!vm.invoke(u, *ctor, obj, {vf(y), vf(p), vf(r)}, &res, err)) return nullptr;
    return obj;
}

static void vm_corpus_tests(int argc, char **argv, Checker &c)
{
    std::printf("shipped corpus VM programs\n");
    std::vector<ClassFile> files((size_t)argc);
    Vm vm;
    bool load_ok = true;
    for (int i = 0; i < argc; i++) {
        std::string err;
        bool ok = classfile_load(argv[i], &files[(size_t)i], &err);
        c.check(ok, std::string("load ") + argv[i] +
                        (ok ? std::string() : ": " + err));
        if (!ok) {
            load_ok = false;
            continue;
        }
        for (const ClassUnit &u : files[(size_t)i].units) vm.add_class(&u);
    }
    if (!load_ok) return;

    c.check(vm.find_class("java.lang.Object") != nullptr, "java.lang.Object registered");
    c.check(vm.find_class("java.lang.Math") != nullptr, "java.lang.Math registered");
    c.check(vm.find_class("java.lang.Vector3") != nullptr, "java.lang.Vector3 registered");
    const ClassUnit *math = find_unit(files, "java.lang.Math");
    const ClassUnit *vec = find_unit(files, "java.lang.Vector3");
    c.check(math != nullptr, "Math unit found");
    c.check(vec != nullptr, "Vector3 unit found");
    if (!math || !vec) return;

    std::string err;
    Value r;
    bool ran;

    ran = call_static(vm, *math, "abs", "(I)I", {vi(-5)}, &r, &err);
    expect_int(c, ran, r, 5, "Math.abs(-5)", err);
    ran = call_static(vm, *math, "abs", "(I)I", {vi(7)}, &r, &err);
    expect_int(c, ran, r, 7, "Math.abs(7)", err);
    ran = call_static(vm, *math, "abs", "(F)F", {vf(-2.5f)}, &r, &err);
    expect_float(c, ran, r, 2.5f, 0.0f, "Math.abs(-2.5f)", err);
    ran = call_static(vm, *math, "avg", "(FF)F", {vf(2.0f), vf(4.0f)}, &r, &err);
    expect_float(c, ran, r, 3.0f, 0.0f, "Math.avg(2,4)", err);
    ran = call_static(vm, *math, "interval", "(FFF)I",
                      {vf(0.0f), vf(10.0f), vf(5.0f)}, &r, &err);
    expect_int(c, ran, r, 1, "Math.interval(0,10,5)", err);
    ran = call_static(vm, *math, "interval", "(FFF)I",
                      {vf(0.0f), vf(10.0f), vf(0.0f)}, &r, &err);
    expect_int(c, ran, r, 0, "Math.interval(0,10,0)", err);
    ran = call_static(vm, *math, "interval", "(FFF)I",
                      {vf(0.0f), vf(10.0f), vf(10.0f)}, &r, &err);
    expect_int(c, ran, r, 0, "Math.interval(0,10,10)", err);
    ran = call_static(vm, *math, "interval", "(FFF)I",
                      {vf(0.0f), vf(10.0f), vf(11.0f)}, &r, &err);
    expect_int(c, ran, r, 0, "Math.interval(0,10,11)", err);
    ran = call_static(vm, *math, "deg2rad", "(I)F", {vi(180)}, &r, &err);
    expect_float(c, ran, r, 3.14159265f, 1e-6f, "Math.deg2rad(180) (lazy PI)", err);
    ran = call_static(vm, *math, "sin", "(F)F", {vf(0.5f)}, &r, &err);
    expect_float(c, ran, r, std::sin(0.5f), 1e-4f,
                 "Math.sin(0.5) (script call chain)", err);

    Object *a = make_vector(vm, *vec, 1.0f, 2.0f, 3.0f, &err);
    c.check(a != nullptr, "Vector3(1,2,3) constructed" +
                              (a ? std::string() : ": " + err));
    Object *b = make_vector(vm, *vec, 4.0f, 5.0f, 6.0f, &err);
    c.check(b != nullptr, "Vector3(4,5,6) constructed" +
                              (b ? std::string() : ": " + err));
    Object *mzero = make_vector(vm, *vec, 0.0f, 0.0f, 0.0f, &err);
    c.check(mzero != nullptr, "Vector3(0,0,0) constructed" +
                                  (mzero ? std::string() : ": " + err));
    Object *same = make_vector(vm, *vec, 5.0f, 7.0f, 9.0f, &err);
    c.check(same != nullptr, "Vector3(5,7,9) constructed" +
                                 (same ? std::string() : ": " + err));
    if (!a || !b || !mzero || !same) return;

    ran = call_instance(vm, *vec, "sqrlength", "()F", a, {}, &r, &err);
    expect_float(c, ran, r, 14.0f, 0.0f, "Vector3(1,2,3).sqrlength", err);

    ran = call_instance(vm, *vec, "diff",
                        "(Ljava.lang.Vector3;Ljava.lang.Vector3;)"
                        "Ljava.lang.Vector3;",
                        mzero, {Value::make_obj(b), Value::make_obj(a)}, &r, &err);
    c.check(ran, "Vector3.diff runs" + (ran ? std::string() : ": " + err));
    if (ran) c.check(r.kind == Value::Obj && r.obj == mzero, "Vector3.diff returns this");
    ran = call_instance(vm, *vec, "sqrlength", "()F", mzero, {}, &r, &err);
    expect_float(c, ran, r, 27.0f, 0.0f, "Vector3 diff (4,5,6)-(1,2,3)", err);

    ran = call_instance(vm, *vec, "add",
                        "(Ljava.lang.Vector3;)Ljava.lang.Vector3;", a,
                        {Value::make_obj(b)}, &r, &err);
    c.check(ran, "Vector3.add runs" + (ran ? std::string() : ": " + err));
    if (ran) c.check(r.kind == Value::Obj && r.obj == a, "Vector3.add returns this");
    ran = call_instance(vm, *vec, "sqrlength", "()F", a, {}, &r, &err);
    expect_float(c, ran, r, 155.0f, 0.0f, "Vector3 after add", err);

    ran = call_instance(vm, *vec, "mul", "(F)Ljava.lang.Vector3;", b, {vf(2.0f)},
                        &r, &err);
    c.check(ran, "Vector3.mul(float) runs" + (ran ? std::string() : ": " + err));
    ran = call_instance(vm, *vec, "sqrlength", "()F", b, {}, &r, &err);
    expect_float(c, ran, r, 308.0f, 0.0f, "Vector3(4,5,6)*2 sqrlength", err);

    ran = call_instance(vm, *vec, "equals", "(Ljava.lang.Vector3;)I", a,
                        {Value::make_obj(same)}, &r, &err);
    expect_int(c, ran, r, 1, "Vector3 equals same components", err);
    ran = call_instance(vm, *vec, "equals", "(Ljava.lang.Vector3;)I", a,
                        {Value::make_obj(b)}, &r, &err);
    expect_int(c, ran, r, 0, "Vector3 equals different components", err);

    // ---- native methods ----
    const ClassUnit *sys = find_unit(files, "java.lang.System");
    const ClassUnit *str = find_unit(files, "java.lang.String");
    const ClassUnit *itg = find_unit(files, "java.lang.Integer");
    const ClassUnit *flt = find_unit(files, "java.lang.Float");
    const ClassUnit *ypr = find_unit(files, "java.lang.Ypr");
    c.check(sys && str && itg && flt && ypr, "native test units found");

    if (sys) {
        ran = call_static(vm, *sys, "buildNumber", "()I", {}, &r, &err);
        expect_int(c, ran, r, 940, "System.buildNumber", err);
        ran = call_static(vm, *sys, "runFinalization", "()I", {}, &r, &err);
        expect_int(c, ran, r, 0, "System.runFinalization", err);
        ran = call_static(vm, *sys, "exit", "(Ljava.lang.String;)V",
                          {Value::make_str("bye")}, &r, &err);
        c.check(ran, "System.exit(String) is a no-op" +
                         (ran ? std::string() : ": " + err));
        err.clear();
        ran = call_static(vm, *sys, "exit", "()V", {}, &r, &err);
        c.check(ran, "System.exit()V sets the quit flag" +
                         (ran ? std::string() : ": " + err));

        float t1 = 0.0f, t2 = 0.0f;
        ran = call_static(vm, *sys, "currentTime", "()F", {}, &r, &err);
        c.check(ran && r.kind == Value::Float, "System.currentTime runs");
        if (ran && r.kind == Value::Float) t1 = r.f;
        ran = call_static(vm, *sys, "currentTime", "()F", {}, &r, &err);
        if (ran && r.kind == Value::Float) t2 = r.f;
        c.check(ran && t1 >= 0.0f && t2 >= t1, "System.currentTime monotonic");

        Array *asrc = vm.new_array("I", 4);
        for (int i = 0; i < 4; i++) asrc->elems[(size_t)i] = vi(i + 1);
        Array *adst = vm.new_array("I", 4);
        ran = call_static(vm, *sys, "arraycopy",
                          "(Ljava.lang.Object;ILjava.lang.Object;II)V",
                          {Value::make_array(asrc), vi(1), Value::make_array(adst), vi(2),
                           vi(2)},
                          &r, &err);
        c.check(ran, "System.arraycopy runs" + (ran ? std::string() : ": " + err));
        if (ran)
            c.check(adst->elems[0].i == 0 && adst->elems[1].i == 0 &&
                         adst->elems[2].i == 2 && adst->elems[3].i == 3,
                    "System.arraycopy 1..2 -> 2..3");

        Array *alias = vm.new_array("I", 4);
        for (int i = 0; i < 4; i++) alias->elems[(size_t)i] = vi(i + 1);
        ran = call_static(vm, *sys, "arraycopy",
                          "(Ljava.lang.Object;ILjava.lang.Object;II)V",
                          {Value::make_array(alias), vi(0), Value::make_array(alias), vi(1),
                           vi(3)},
                          &r, &err);
        c.check(ran && alias->elems[0].i == 1 && alias->elems[1].i == 1 &&
                    alias->elems[2].i == 2 && alias->elems[3].i == 3,
                "System.arraycopy overlapping same array");

        Array *small = vm.new_array("I", 2);
        small->elems[0] = vi(7);
        small->elems[1] = vi(8);
        ran = call_static(vm, *sys, "arraycopy",
                          "(Ljava.lang.Object;ILjava.lang.Object;II)V",
                          {Value::make_array(alias), vi(0), Value::make_array(small), vi(0),
                           vi(3)},
                          &r, &err);
        c.check(ran && small->elems[0].i == 7 && small->elems[1].i == 8,
                "System.arraycopy out of bounds is a no-op");

        err.clear();
        ran = call_static(vm, *sys, "gc", "()I", {}, &r, &err);
        expect_int(c, ran, r, 0, "System.gc", err);
    }

    if (itg) {
        ran = call_static(vm, *itg, "toString", "(I)Ljava.lang.String;", {vi(-42)}, &r,
                          &err);
        expect_str(c, ran, r, "-42", "Integer.toString(-42)", err);
        ran = call_static(vm, *itg, "toHexString", "(I)Ljava.lang.String;", {vi(0x1234)},
                          &r, &err);
        expect_str(c, ran, r, "0x00001234", "Integer.toHexString(0x1234)", err);
    }
    if (flt) {
        ran = call_static(vm, *flt, "toString", "(FLjava.lang.String;)Ljava.lang.String;",
                          {vf(1.5f), Value::make_str("%f")}, &r, &err);
        expect_str(c, ran, r, "1.500000", "Float.toString(1.5, \"%f\")", err);
    }

    ran = call_static(vm, *math, "sqrt", "(F)F", {vf(9.0f)}, &r, &err);
    expect_float(c, ran, r, 3.0f, 0.0f, "Math.sqrt(9)", err);
    ran = call_static(vm, *math, "pow", "(FF)F", {vf(2.0f), vf(10.0f)}, &r, &err);
    expect_float(c, ran, r, 1024.0f, 1e-2f, "Math.pow(2,10)", err);
    ran = call_static(vm, *math, "atan2", "(FF)F", {vf(1.0f), vf(1.0f)}, &r, &err);
    expect_float(c, ran, r, 0.785398163f, 1e-6f, "Math.atan2(1,1)", err);
    ran = call_static(vm, *math, "exp", "(F)F", {vf(0.0f)}, &r, &err);
    expect_float(c, ran, r, 1.0f, 0.0f, "Math.exp(0)", err);
    ran = call_static(vm, *math, "ln", "(F)F", {vf(1.0f)}, &r, &err);
    expect_float(c, ran, r, 0.0f, 0.0f, "Math.ln(1)", err);
    ran = call_static(vm, *math, "log10", "(F)F", {vf(1000.0f)}, &r, &err);
    // The exe registers log10 at Math.ln's native, so it returns ln(1000).
    expect_float(c, ran, r, std::log(1000.0f), 1e-4f,
                 "Math.log10 is ln (exe bug)", err);
    ran = call_static(vm, *math, "setrandseed", "(I)V", {vi(1)}, &r, &err);
    c.check(ran, "Math.setrandseed(1)" + (ran ? std::string() : ": " + err));
    ran = call_static(vm, *math, "random", "()F", {}, &r, &err);
    expect_float(c, ran, r, 41.0f / 32767.0f, 1e-8f,
                 "Math.random first MSVC value after srand(1)", err);
    ran = call_static(vm, *math, "random", "()F", {}, &r, &err);
    c.check(ran && r.kind == Value::Float && r.f >= 0.0f && r.f < 1.0f,
            "Math.random in [0,1)");
    ran = call_static(vm, *math, "randomize", "()V", {}, &r, &err);
    c.check(ran, "Math.randomize" + (ran ? std::string() : ": " + err));

    if (str) {
        Value srecv = Value::make_str("one two\tthree");
        ran = call_native(vm, *str, "length", "()I", &srecv, {}, &r, &err);
        expect_int(c, ran, r, 13, "String.length", err);
        ran = call_native(vm, *str, "token", "(ILjava.lang.String;)Ljava.lang.String;",
                          &srecv, {vi(1), Value::make_str(" \t\n")}, &r, &err);
        expect_str(c, ran, r, "two", "String.token(1)", err);
        ran = call_native(vm, *str, "token", "(ILjava.lang.String;)Ljava.lang.String;",
                          &srecv, {vi(3), Value::make_str(" \t\n")}, &r, &err);
        c.check(ran && r.kind == Value::Null, "String.token past end is null");
        Value istr = Value::make_str("  -123abc");
        ran = call_native(vm, *str, "intValue", "()I", &istr, {}, &r, &err);
        expect_int(c, ran, r, -123, "String.intValue", err);
        Value fstr = Value::make_str("2.5x");
        ran = call_native(vm, *str, "floatValue", "()F", &fstr, {}, &r, &err);
        expect_float(c, ran, r, 2.5f, 0.0f, "String.floatValue", err);
        Value arecv = Value::make_str("ab");
        ran = call_native(vm, *str, "append", "(I)Ljava.lang.String;", &arecv, {vi('c')},
                          &r, &err);
        expect_str(c, ran, r, "abc", "String.append('c')", err);
        ran = call_native(vm, *str, "append", "(I)Ljava.lang.String;", &arecv, {vi(8)},
                          &r, &err);
        expect_str(c, ran, r, "a", "String.append(backspace)", err);
        Value grecv = Value::make_str("Az");
        ran = call_native(vm, *str, "getASCII", "()[I", &grecv, {}, &r, &err);
        c.check(ran && r.kind == Value::Array && r.arr && r.arr->elems.size() == 2 &&
                    r.arr->elems[0].i == 'A' && r.arr->elems[1].i == 'z',
                "String.getASCII");
        ran = call_native(vm, *str, "finalize", "()V", &grecv, {}, &r, &err);
        c.check(ran, "String.finalize is a no-op" +
                         (ran ? std::string() : ": " + err));
    }

    if (ypr) {
        Object *zero = make_vector(vm, *vec, 1.0f, 2.0f, 3.0f, &err);
        Object *unitv = make_vector(vm, *vec, 3.0f, 4.0f, 0.0f, &err);
        Object *axisv = make_vector(vm, *vec, 0.0f, 0.0f, 1.0f, &err);
        Object *y0 = make_ypr(vm, *ypr, 0.0f, 0.0f, 0.0f, &err);
        Object *yaw90 = make_ypr(vm, *ypr, 1.5707964f, 0.0f, 0.0f, &err);
        c.check(zero && unitv && axisv && y0 && yaw90,
                "native Vector3/Ypr fixtures built" +
                    (zero && unitv && axisv && y0 && yaw90 ? std::string() : ": " + err));
        if (zero && unitv && axisv && y0 && yaw90) {
            Value zrecv = Value::make_obj(zero);
            Value urecv = Value::make_obj(unitv);
            ran = call_native(vm, *vec, "length", "()F", &zrecv, {}, &r,
                              &err);
            expect_float(c, ran, r, 3.7416575f, 1e-6f, "Vector3(1,2,3).length", err);
            ran = call_native(vm, *vec, "normalize", "()V", &urecv, {},
                              &r, &err);
            c.check(ran, "Vector3.normalize runs" +
                             (ran ? std::string() : ": " + err));
            ran = call_native(vm, *vec, "length", "()F", &urecv, {}, &r,
                              &err);
            expect_float(c, ran, r, 1.0f, 1e-6f, "Vector3.normalize unit length", err);

            Value recv = zrecv;
            ran = call_native(vm, *vec, "rotate",
                              "(Ljava.lang.Ypr;)Ljava.lang.Vector3;", &recv,
                              {Value::make_obj(y0)}, &r, &err);
            c.check(ran && r.kind == Value::Obj && r.obj == zero,
                    "Vector3.rotate(Ypr) returns receiver" +
                        (ran ? std::string() : ": " + err));
            float vx = 0.0f, vy = 0.0f, vz = 0.0f;
            if (ran) {
                bool got = obj_float(vm, zero, "x", &vx, &err) &&
                           obj_float(vm, zero, "y", &vy, &err) &&
                           obj_float(vm, zero, "z", &vz, &err);
                c.check(got && std::fabs(vx - 1.0f) <= 1e-6f &&
                            std::fabs(vy - 2.0f) <= 1e-6f &&
                            std::fabs(vz - 3.0f) <= 1e-6f,
                        "Vector3.rotate(zero Ypr) unchanged");
            }
            ran = call_native(vm, *vec, "rotate",
                              "(Ljava.lang.Ypr;)Ljava.lang.Vector3;", &recv,
                              {Value::make_obj(yaw90)}, &r, &err);
            if (ran) {
                bool got = obj_float(vm, zero, "x", &vx, &err) &&
                           obj_float(vm, zero, "y", &vy, &err) &&
                           obj_float(vm, zero, "z", &vz, &err);
                c.check(got && std::fabs(vx - 3.0f) <= 1e-5f &&
                            std::fabs(vy - 2.0f) <= 1e-5f &&
                            std::fabs(vz + 1.0f) <= 1e-5f,
                        "Vector3.rotate(yaw 90)");
            }
            Object *axis = make_vector(vm, *vec, 1.0f, 0.0f, 0.0f, &err);
            c.check(axis != nullptr, "axis rotation fixture built");
            if (axis) {
                Value arecv = Value::make_obj(axis);
                ran = call_native(vm, *vec, "rotate",
                                  "(Ljava.lang.Vector3;F)Ljava.lang.Vector3;", &arecv,
                                  {Value::make_obj(axisv), vf(1.5707964f)}, &r, &err);
                if (ran) {
                    bool got = obj_float(vm, axis, "x", &vx, &err) &&
                               obj_float(vm, axis, "y", &vy, &err) &&
                               obj_float(vm, axis, "z", &vz, &err);
                    c.check(got && std::fabs(vx) <= 1e-5f &&
                                std::fabs(vy - 1.0f) <= 1e-5f &&
                                std::fabs(vz) <= 1e-5f,
                            "Vector3.rotate(z axis, 90 deg)");
                }
            }

            ClassState *vcs = vm.find_class("java.lang.Vector3");
            Object *vfromy = vcs ? vm.new_object(vcs, &err) : nullptr;
            c.check(vfromy != nullptr, "native Vector3.<init>(Ypr) receiver");
            if (vfromy) {
                Value vrecv = Value::make_obj(vfromy);
                ran = call_native(vm, *vec, "<init>", "(Ljava.lang.Ypr;)", &vrecv,
                                  {Value::make_obj(y0)}, &r, &err);
                float fx = 0.0f, fy = 0.0f, fz = 0.0f;
                bool got = ran && obj_float(vm, vfromy, "x", &fx, &err) &&
                           obj_float(vm, vfromy, "y", &fy, &err) &&
                           obj_float(vm, vfromy, "z", &fz, &err);
                c.check(got && std::fabs(fx) <= 1e-6f && std::fabs(fy) <= 1e-6f &&
                            std::fabs(fz + 1.0f) <= 1e-6f,
                        "Vector3(Ypr 0) is (0,0,-1)");
            }

            ClassState *ycs = vm.find_class("java.lang.Ypr");
            Object *yfromv = ycs ? vm.new_object(ycs, &err) : nullptr;
            Object *yv = make_vector(vm, *vec, 0.0f, 0.0f, 0.0f, &err);
            c.check(yfromv && yv, "native Ypr.<init>(Vector3) fixture");
            if (yfromv && yv) {
                Value yrecv = Value::make_obj(yfromv);
                ran = call_native(vm, *ypr, "<init>", "(Ljava.lang.Vector3;)", &yrecv,
                                  {Value::make_obj(yv)}, &r, &err);
                float fy = 0.0f, fp = 0.0f, fr = 0.0f;
                bool got = ran && obj_float(vm, yfromv, "y", &fy, &err) &&
                           obj_float(vm, yfromv, "p", &fp, &err) &&
                           obj_float(vm, yfromv, "r", &fr, &err);
                c.check(got && fy == 0.0f && fp == -1.5707964f && fr == 0.0f,
                        "Ypr(Vector3 0) pitch -pi/2");
            }
            Object *ymul = make_ypr(vm, *ypr, 0.0f, 0.0f, 0.0f, &err);
            Object *yident = make_ypr(vm, *ypr, 0.0f, 0.0f, 0.0f, &err);
            c.check(ymul && yident, "Ypr.mul fixtures");
            if (ymul && yident) {
                Value mrecv = Value::make_obj(ymul);
                ran = call_native(vm, *ypr, "mul", "(Ljava.lang.Ypr;)V", &mrecv,
                                  {Value::make_obj(yident)}, &r, &err);
                float fy = 1.0f, fp = 1.0f, fr = 1.0f;
                bool got = ran && obj_float(vm, ymul, "y", &fy, &err) &&
                           obj_float(vm, ymul, "p", &fp, &err) &&
                           obj_float(vm, ymul, "r", &fr, &err);
                c.check(got && std::fabs(fy) <= 1e-6f && std::fabs(fp) <= 1e-6f &&
                            std::fabs(fr) <= 1e-6f,
                        "Ypr.mul(identity)");
            }
        }
    }
}

// ---- conformance -----------------------------------------------------------

static bool ends_with(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

static std::string path_join(const std::string &dir, const std::string &name)
{
    if (dir.empty()) return name;
    if (dir[dir.size() - 1] == '/') return dir + name;
    return dir + "/" + name;
}

static void collect_class_files(const std::string &dir, std::vector<std::string> *out)
{
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    while (struct dirent *e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string path = path_join(dir, name);
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode))
            collect_class_files(path, out);
        else if (S_ISREG(st.st_mode) && ends_with(name, ".class"))
            out->push_back(path);
    }
    closedir(d);
}

// Constant operand type required by each opcode that references CONS.
static int expected_const_type(uint8_t op)
{
    switch (op) {
    case 0x06: case 0x09: case 0x12: case 0x1c: case 0x25: case 0x26:
        return CONST_STRING;
    case 0x0e:
        return CONST_RESOURCE;
    case 0x19:
        return CONST_CLASS;
    case 0x1a: case 0x1b:
        return CONST_MEMBER;
    default:
        return -1;
    }
}

static int conformance(const char *dir)
{
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        std::fprintf(stderr, "conformance: not a directory: %s\n", dir);
        return 2;
    }
    std::vector<std::string> paths;
    collect_class_files(dir, &paths);

    size_t scanned = paths.size();
    size_t empty = 0, parsed = 0, units = 0, bodies = 0, instrs = 0, failures = 0;
    auto fail = [&](const std::string &msg) {
        failures++;
        if (failures <= 100) std::fprintf(stderr, "FAIL %s\n", msg.c_str());
    };

    for (const std::string &path : paths) {
        std::vector<uint8_t> data;
        if (!read_binary(path, &data)) {
            fail(path + ": unreadable");
            continue;
        }
        if (data.empty()) {
            empty++;
            continue;
        }
        ClassFile file;
        std::string err;
        if (!classfile_parse(data.data(), data.size(), &file, &err)) {
            fail(path + ": " + err);
            continue;
        }
        parsed++;
        if (file.units.empty()) {
            fail(path + ": no class units");
            continue;
        }
        for (const ClassUnit &u : file.units) {
            units++;
            std::string cls = u.class_name(u.self_class);
            if (cls.empty()) fail(path + ": unit with unresolved class name");

            int max_body = -1;
            for (const FieldInfo &f : u.fields)
                if (f.init > max_body) max_body = f.init;
            for (const MethodInfo &m : u.methods)
                if (m.body > max_body) max_body = m.body;
            if (max_body + 1 != (int)u.bodies.size())
                fail(path + ": " + cls + ": body reachability " +
                     std::to_string(max_body + 1) + " != " +
                     std::to_string(u.bodies.size()));

            for (const Body &b : u.bodies) {
                bodies++;
                instrs += b.instrs.size();
                for (const Instr &in : b.instrs) {
                    if (!in.has_operand) continue;
                    int want = expected_const_type(in.op);
                    if (want < 0) continue;
                    const ConstEntry *e = u.entry((int)in.operand);
                    if (!e || (int)e->type != want)
                        fail(path + ": " + cls + ": op 0x" +
                             std::to_string(in.op) + " bad constant " +
                             std::to_string(in.operand));
                }
            }
        }
    }

    std::printf("conformance: %zu files scanned, %zu empty skipped, %zu parsed, "
                "%zu units, %zu bodies, %zu instructions, %zu failure(s)\n",
                scanned, empty, parsed, units, bodies, instrs, failures);
    if (parsed == 0 && failures == 0) {
        std::fprintf(stderr, "conformance: no class files under %s\n", dir);
        return 1;
    }
    return failures ? 1 : 0;
}

// ---- native registry check -------------------------------------------------

static int natives_check(const char *path)
{
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "natives-check: cannot read %s\n", path);
        return 2;
    }
    Checker c;
    std::map<std::string, const natives::Entry *> reg;
    size_t bad = 0, implemented = 0, stubs = 0;
    for (const natives::Entry &e : natives::table()) {
        std::string key = std::string(e.cls) + "." + e.method + e.sig;
        if (!reg.emplace(key, &e).second) {
            std::fprintf(stderr, "natives-check: duplicate registry row %s\n", key.c_str());
            bad++;
        }
        if (!e.fn && !e.engine) {
            std::fprintf(stderr, "natives-check: silent gap %s\n", key.c_str());
            bad++;
        }
        if (e.fn)
            implemented++;
        else
            stubs++;
    }
    c.check(bad == 0, "registry rows unique and implemented-or-explicit-stub");

    std::string line;
    std::getline(f, line);  // fn\tclass\tmethod\tsignature
    size_t rows = 0, missing = 0, malformed = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        size_t t1 = line.find('\t');
        size_t t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        size_t t3 = t2 == std::string::npos ? t2 : line.find('\t', t2 + 1);
        rows++;
        if (t3 == std::string::npos) {
            malformed++;
            continue;
        }
        std::string cls = line.substr(t1 + 1, t2 - t1 - 1);
        std::string method = line.substr(t2 + 1, t3 - t2 - 1);
        std::string key = cls + "." + method + line.substr(t3 + 1);
        auto it = reg.find(key);
        if (it == reg.end()) {
            if (missing++ < 5)
                std::fprintf(stderr, "natives-check: missing %s\n", key.c_str());
        } else {
            reg.erase(it);
        }
    }
    c.check(malformed == 0, "natives.tsv rows have four columns");
    c.check(rows > 0, "natives.tsv parsed");
    c.check(missing == 0, "every natives.tsv entry is registered");
    c.check(reg.empty(), "no registry rows absent from natives.tsv");
    std::printf("natives: %zu rows (%zu implemented, %zu explicit stubs)\n", rows,
                implemented, stubs);
    std::printf("natives-check: %s (%d failure(s))\n",
                c.failures ? "FAILED" : "PASSED", c.failures);
    return c.failures ? 1 : 0;
}

// ---- entry point -----------------------------------------------------------

static int selfcheck(int argc, char **argv)
{
    if (argc < 1) {
        std::fprintf(stderr, "usage: classdump --selfcheck <file.class>...\n");
        return 2;
    }
    Checker c;
    for (int i = 0; i < argc; i++)
        selfcheck_file(argv[i], c);
    std::printf("selfcheck: %s (%d failure(s))\n", c.failures ? "FAILED" : "PASSED",
                c.failures);
    return c.failures ? 1 : 0;
}

static int corpus(int argc, char **argv)
{
    if (argc < 1) {
        std::fprintf(stderr, "usage: classdump --corpus <file.class>...\n");
        return 2;
    }
    Checker c;
    vm_corpus_tests(argc, argv, c);
    std::printf("corpus: %s (%d failure(s))\n", c.failures ? "FAILED" : "PASSED",
                c.failures);
    return c.failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: classdump <file.class>...\n"
                     "       classdump --selfcheck <file.class>...\n"
                     "       classdump --corpus <file.class>...\n"
                     "       classdump --natives-check <natives.tsv>\n"
                     "       classdump --conformance <directory>\n");
        return 2;
    }
    if (strcmp(argv[1], "--selfcheck") == 0)
        return selfcheck(argc - 2, argv + 2);
    if (strcmp(argv[1], "--corpus") == 0)
        return corpus(argc - 2, argv + 2);
    if (strcmp(argv[1], "--disasm") == 0) {
        for (int i = 2; i < argc; i++) {
            ClassFile file;
            std::string err;
            if (!classfile_load(argv[i], &file, &err)) {
                std::fprintf(stderr, "%s: %s\n", argv[i], err.c_str());
                continue;
            }
            for (const ClassUnit &u : file.units) {
                std::printf("unit %s\n", u.class_name(u.self_class).c_str());
                for (size_t bi = 0; bi < u.bodies.size(); bi++) {
                    std::printf(" body %zu\n", bi);
                    for (size_t ii = 0; ii < u.bodies[bi].instrs.size(); ii++) {
                        const Instr &in = u.bodies[bi].instrs[ii];
                        std::printf("  %zu: op 0x%02x operand %u line %u\n", ii, in.op, in.operand, in.line);
                    }
                }
            }
        }
        return 0;
    }
    if (strcmp(argv[1], "--natives-check") == 0) {
        if (argc != 3) {
            std::fprintf(stderr, "usage: classdump --natives-check <natives.tsv>\n");
            return 2;
        }
        return natives_check(argv[2]);
    }
    if (strcmp(argv[1], "--conformance") == 0) {
        if (argc != 3) {
            std::fprintf(stderr, "usage: classdump --conformance <directory>\n");
            return 2;
        }
        return conformance(argv[2]);
    }
    return dump_files(argc - 1, argv + 1);
}
