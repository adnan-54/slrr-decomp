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

static void build_synthetic_units(std::vector<ClassUnit> *units)
{
    units->reserve(4);

    // Base: one instance field, a constructor, a getter and a mutator.
    UnitBuilder b;
    b.add_field(false, "v", "I");
    int m_v = b.add_member("synth.Base", "v", "I");
    Code bc;
    bc.op(0x01, 1);       // value (first argument, local slot 1)
    bc.op(0x1b, (uint32_t)m_v); // this.v, implicit receiver
    bc.op(0x08, 35);      // store
    bc.op(0x2a);          // constructor return
    int bc_body = b.add_body(bc);

    Code bg;
    bg.op(0x01, 0);
    bg.op(0x1b, (uint32_t)m_v);
    bg.op(0x28);
    int bg_body = b.add_body(bg);

    Code bb;
    bb.op(0x1b, (uint32_t)m_v); // v + 1
    bb.op(0x07, 28);
    bb.op(0x0b, 1);
    bb.op(0x07, 16);
    bb.op(0x1b, (uint32_t)m_v); // assignment target
    bb.op(0x07, 35);            // assignment keeps its value
    bb.op(0x28);
    int bb_body = b.add_body(bb);

    b.add_method(false, 0x1, "<init>", "(I)V", bc_body, 2);
    b.add_method(false, 0x1, "get", "()I", bg_body, 1);
    b.add_method(false, 0x1, "bump", "()I", bb_body, 1);
    units->push_back(b.finish("synth.Base", ""));

    // Derived: super constructor, super method call, implicit (0x12) call.
    UnitBuilder d;
    int s_get = d.add_str("get");
    int s_bump = d.add_str("bump");

    Code dc;
    dc.op(0x01, 1);
    dc.op(0x27, 1);
    dc.op(0x23);        // super(...)
    dc.op(0x08, 34);    // constructor chain
    dc.op(0x2a);
    int dc_body = d.add_body(dc);

    Code dg;
    dg.op(0x07, 28);
    dg.op(0x0b, 100);
    dg.op(0x27, 0);
    dg.op(0x26, (uint32_t)s_get); // super.get()
    dg.op(0x07, 27);
    dg.op(0x07, 16);
    dg.op(0x28);
    int dg_body = d.add_body(dg);

    Code di;
    di.op(0x27, 0);
    di.op(0x24);
    di.op(0x12, (uint32_t)s_bump); // implicit this.bump()
    di.op(0x07, 27);
    di.op(0x28);
    int di_body = d.add_body(di);

    d.add_method(false, 0x1, "<init>", "(I)V", dc_body, 2);
    d.add_method(false, 0x1, "get", "()I", dg_body, 1);
    d.add_method(false, 0x1, "bumpAndGet", "()I", di_body, 1);
    units->push_back(d.finish("synth.Derived", "synth.Base"));

    // Maker: static programs covering the remaining opcodes.
    UnitBuilder m;
    int s_i = m.add_str("I");
    int s_arr = m.add_str("[I");
    int s_derived = m.add_str("Lsynth.Derived;");
    int s_base = m.add_str("Lsynth.Base;");
    int s_v = m.add_str("v");
    int ms_get = m.add_str("get");
    int ms_bump_and_get = m.add_str("bumpAndGet");
    int s_a = m.add_str("a");
    int m_length = m.add_member("[I", "length", "I");
    int m_dbl = m.add_member("synth.Maker", "dbl", "(I)I");
    int c_maker = m.add_class("synth.Maker");

    // for (int i=0,sum=0; i<100; i++) sum += i*2; return sum;
    Code loop;
    loop.op(0x02, 0);
    loop.op(0x06, (uint32_t)s_i);
    loop.op(0x2c);
    loop.op(0x0b, 0);
    loop.op(0x2d);
    loop.op(0x02, 1);
    loop.op(0x06, (uint32_t)s_i);
    loop.op(0x2c);
    loop.op(0x0b, 0);
    loop.op(0x2d);
    size_t head = loop.mark();
    loop.op(0x01, 1);
    loop.op(0x0b, 100);
    loop.op(0x07, 11);            // i < 100
    size_t exit_jump = loop.mark();
    loop.op(0x15);
    loop.op(0x01, 1);
    loop.op(0x0b, 2);
    loop.op(0x07, 19);            // i * 2
    loop.op(0x01, 0);
    loop.op(0x08, 39);            // sum += ...
    loop.op(0x01, 1);
    loop.op(0x08, 23);            // i++
    size_t back_jump = loop.mark();
    loop.op(0x14);
    loop.patch(back_jump, head);
    loop.patch(exit_jump, loop.mark());
    loop.op(0x01, 0);
    loop.op(0x28);
    int loop_body = m.add_body(loop);

    // if/else, switch and ternary.
    Code br;
    br.op(0x02, 1);
    br.op(0x06, (uint32_t)s_i);
    br.op(0x2c);
    br.op(0x0b, 0);
    br.op(0x2d);
    br.op(0x01, 0);
    br.op(0x0b, 0);
    br.op(0x07, 10);              // x > 0
    size_t jf = br.mark();
    br.op(0x15);
    br.op(0x0b, 1);
    br.op(0x01, 1);
    br.op(0x08, 35);              // r = 1
    size_t jend = br.mark();
    br.op(0x14);
    size_t els = br.mark();
    br.patch(jf, els);
    br.op(0x0b, 2);
    br.op(0x01, 1);
    br.op(0x08, 35);              // r = 2
    br.patch(jend, br.mark());

    br.op(0x01, 0);               // switch selector
    br.op(0x07, 28);
    br.op(0x0b, 0);
    size_t case0 = br.mark();
    br.op(0x17);
    br.op(0x07, 28);
    br.op(0x0b, 1);
    size_t case1 = br.mark();
    br.op(0x17);
    br.op(0x18);
    br.op(0x07, 28);
    br.op(0x0b, 30);
    br.op(0x01, 1);
    br.op(0x08, 39);              // default: r += 30
    size_t jdef = br.mark();
    br.op(0x14);
    size_t label0 = br.mark();
    br.patch(case0, label0);
    br.op(0x07, 28);
    br.op(0x0b, 10);
    br.op(0x01, 1);
    br.op(0x08, 39);              // case 0: r += 10
    size_t j0 = br.mark();
    br.op(0x14);
    size_t label1 = br.mark();
    br.patch(case1, label1);
    br.op(0x07, 28);
    br.op(0x0b, 20);
    br.op(0x01, 1);
    br.op(0x08, 39);              // case 1: r += 20
    br.patch(jdef, br.mark());
    br.patch(j0, br.mark());

    br.op(0x01, 1);               // r + (x == 0 ? 100 : 0)
    br.op(0x01, 0);
    br.op(0x0b, 0);
    br.op(0x07, 7);
    br.op(0x07, 28);
    br.op(0x0b, 100);
    br.op(0x07, 28);
    br.op(0x0b, 0);
    br.op(0x07, 0);
    br.op(0x07, 16);
    br.op(0x28);
    int br_body = m.add_body(br);

    // int[] a = new int[3]; a[0]=7; a[1]=a[0]+1; return a.length*10 + a[1];
    Code ar;
    ar.op(0x02, 0);
    ar.op(0x06, (uint32_t)s_arr);
    ar.op(0x2c);
    ar.op(0x0b, 3);
    ar.op(0x07, 31);              // new int[3]
    ar.op(0x06, (uint32_t)s_arr);
    ar.op(0x2d);
    ar.op(0x0b, 7);
    ar.op(0x0b, 0);
    ar.op(0x01, 0);
    ar.op(0x20);
    ar.op(0x08, 35);              // a[0] = 7
    ar.op(0x0b, 0);
    ar.op(0x01, 0);
    ar.op(0x20);
    ar.op(0x07, 28);
    ar.op(0x0b, 1);
    ar.op(0x07, 16);
    ar.op(0x0b, 1);
    ar.op(0x01, 0);
    ar.op(0x20);
    ar.op(0x08, 35);              // a[1] = a[0] + 1
    ar.op(0x11, 2);
    ar.op(0x01, 0);
    ar.op(0x1b, (uint32_t)m_length);
    ar.op(0x0b, 10);
    ar.op(0x07, 19);
    ar.op(0x0b, 1);
    ar.op(0x01, 0);
    ar.op(0x20);
    ar.op(0x07, 16);
    ar.op(0x28);
    int ar_body = m.add_body(ar);

    // new Derived(7).get() through an explicit 0x25 receiver.
    Code run;
    run.op(0x27, 0);              // argc for get()
    run.op(0x07, 30);
    run.op(0x06, (uint32_t)s_derived);
    run.op(0x07, 28);
    run.op(0x0b, 7);
    run.op(0x27, 1);
    run.op(0x21);
    run.op(0x25, (uint32_t)ms_get);
    run.op(0x28);
    int run_body = m.add_body(run);

    // new Derived(7).bumpAndGet() (implicit call happens inside the callee).
    Code implicit;
    implicit.op(0x27, 0);
    implicit.op(0x07, 30);
    implicit.op(0x06, (uint32_t)s_derived);
    implicit.op(0x07, 28);
    implicit.op(0x0b, 7);
    implicit.op(0x27, 1);
    implicit.op(0x21);
    implicit.op(0x25, (uint32_t)ms_bump_and_get);
    implicit.op(0x28);
    int implicit_body = m.add_body(implicit);

    // Explicit field access by name on a new object (0x1c), inherited field.
    Code field;
    field.op(0x07, 30);
    field.op(0x06, (uint32_t)s_derived);
    field.op(0x07, 28);
    field.op(0x0b, 7);
    field.op(0x27, 1);
    field.op(0x21);
    field.op(0x1c, (uint32_t)s_v);
    field.op(0x28);
    int field_body = m.add_body(field);

    // instanceof Derived, starting from a Derived / a Base object.
    Code inst_d;
    inst_d.op(0x07, 30);
    inst_d.op(0x06, (uint32_t)s_derived);
    inst_d.op(0x07, 28);
    inst_d.op(0x0b, 7);
    inst_d.op(0x27, 1);
    inst_d.op(0x21);
    inst_d.op(0x07, 32);
    inst_d.op(0x06, (uint32_t)s_base);
    inst_d.op(0x28);
    int inst_d_body = m.add_body(inst_d);

    Code inst_b;
    inst_b.op(0x07, 30);
    inst_b.op(0x06, (uint32_t)s_base);
    inst_b.op(0x07, 28);
    inst_b.op(0x0b, 3);
    inst_b.op(0x27, 1);
    inst_b.op(0x21);
    inst_b.op(0x07, 32);
    inst_b.op(0x06, (uint32_t)s_derived);
    inst_b.op(0x28);
    int inst_b_body = m.add_body(inst_b);

    // Cast to Base through a descriptor token, then read the inherited field.
    Code cast;
    cast.op(0x07, 30);
    cast.op(0x06, (uint32_t)s_derived);
    cast.op(0x07, 28);
    cast.op(0x0b, 7);
    cast.op(0x27, 1);
    cast.op(0x21);
    cast.op(0x07, 33);
    cast.op(0x06, (uint32_t)s_base);
    cast.op(0x1c, (uint32_t)s_v);
    cast.op(0x28);
    int cast_body = m.add_body(cast);

    Code strings;
    strings.op(0x07, 28);
    strings.op(0x09, (uint32_t)s_a);
    strings.op(0x07, 28);
    strings.op(0x0b, 42);
    strings.op(0x07, 16);
    strings.op(0x28);
    int strings_body = m.add_body(strings);

    // Native dispatch through an explicit class receiver inside a 0x11 window.
    Code native;
    native.op(0x01, 0);
    native.op(0x27, 1);
    native.op(0x24);
    native.op(0x11, 2);
    native.op(0x19, (uint32_t)c_maker);
    native.op(0x1a, (uint32_t)m_dbl);
    native.op(0x07, 27);
    native.op(0x28);
    int native_body = m.add_body(native);

    m.add_method(true, 0x9, "loop", "()I", loop_body, 2);
    m.add_method(true, 0x9, "branches", "(I)I", br_body, 2);
    m.add_method(true, 0x9, "arrays", "()I", ar_body, 1);
    m.add_method(true, 0x9, "run", "()I", run_body, 0);
    m.add_method(true, 0x9, "implicitCall", "()I", implicit_body, 0);
    m.add_method(true, 0x9, "fieldName", "()I", field_body, 0);
    m.add_method(true, 0x9, "instanceOfDerived", "()I", inst_d_body, 0);
    m.add_method(true, 0x9, "instanceOfBase", "()I", inst_b_body, 0);
    m.add_method(true, 0x9, "castTest", "()I", cast_body, 0);
    m.add_method(true, 0x9, "strings", "()Ljava.lang.String;", strings_body, 0);
    m.add_method(true, 0x9, "nativeCall", "(I)I", native_body, 1);
    m.add_method(true, 0x49, "dbl", "(I)I", -1, 0);
    units->push_back(m.finish("synth.Maker", ""));
}

static void vm_synthetic_tests(Checker &c)
{
    std::printf("synthetic VM programs\n");
    std::vector<ClassUnit> units;
    build_synthetic_units(&units);
    for (size_t i = 0; i < units.size(); i++)
        c.check(!units[i].class_name(units[i].self_class).empty(),
                "synthetic unit " + std::to_string(i) + " has a class name");
    Vm vm;
    for (ClassUnit &u : units) vm.add_class(&u);
    vm.register_native("dbl", "(I)I",
                       [](Vm &, const std::vector<Value> &args, Value *out,
                          std::string *err) {
                           if (args.size() != 1 || args[0].kind != Value::Int) {
                               *err = "dbl expects one int";
                               return false;
                           }
                           *out = Value::make_int(args[0].i * 2);
                           return true;
                       });

    const ClassUnit &maker = units[2];
    std::string err;
    Value r;

    const MethodInfo *loop = find_method_sig(maker, "loop", "()I");
    c.check(loop != nullptr, "synthetic loop method present");
    if (loop) {
        std::unique_ptr<Thread> t = vm.start(maker, *loop, nullptr, {}, &err);
        c.check(t != nullptr, "scheduler starts a thread" +
                                   (t ? std::string() : ": " + err));
        if (t) {
            Thread::Status s = t->run_slice(256);
            c.check(s == Thread::Yielded, "first 256-token slice yields");
            s = t->run();
            c.check(s == Thread::Done, "thread finishes on a later slice" +
                                           (s == Thread::Done ? std::string()
                                                              : ": " + t->error()));
            expect_int(c, s == Thread::Done, t->result(), 9900,
                       "sum 0..99 doubled", t->error());
        }
    }

    bool ran;
    ran = call_static(vm, maker, "branches", "(I)I", {vi(0)}, &r, &err);
    expect_int(c, ran, r, 112, "branches(0)", err);
    ran = call_static(vm, maker, "branches", "(I)I", {vi(1)}, &r, &err);
    expect_int(c, ran, r, 21, "branches(1)", err);
    ran = call_static(vm, maker, "branches", "(I)I", {vi(5)}, &r, &err);
    expect_int(c, ran, r, 31, "branches(5)", err);
    ran = call_static(vm, maker, "branches", "(I)I", {vi(-5)}, &r, &err);
    expect_int(c, ran, r, 32, "branches(-5)", err);

    ran = call_static(vm, maker, "arrays", "()I", {}, &r, &err);
    expect_int(c, ran, r, 38, "array new/index/length", err);

    ran = call_static(vm, maker, "run", "()I", {}, &r, &err);
    expect_int(c, ran, r, 107, "new+ctor chain+virtual call", err);
    ran = call_static(vm, maker, "implicitCall", "()I", {}, &r, &err);
    expect_int(c, ran, r, 8, "implicit this call", err);
    ran = call_static(vm, maker, "fieldName", "()I", {}, &r, &err);
    expect_int(c, ran, r, 7, "field access by name (0x1c)", err);
    ran = call_static(vm, maker, "instanceOfDerived", "()I", {}, &r, &err);
    expect_int(c, ran, r, 1, "instanceof derived object", err);
    ran = call_static(vm, maker, "instanceOfBase", "()I", {}, &r, &err);
    expect_int(c, ran, r, 0, "instanceof base object", err);
    ran = call_static(vm, maker, "castTest", "()I", {}, &r, &err);
    expect_int(c, ran, r, 7, "cast + inherited field", err);
    ran = call_static(vm, maker, "strings", "()Ljava.lang.String;", {}, &r, &err);
    expect_str(c, ran, r, "a42", "string concat", err);
    ran = call_static(vm, maker, "nativeCall", "(I)I", {vi(21)}, &r, &err);
    expect_int(c, ran, r, 42, "native dispatch stub", err);
}

// ---- VM service natives: Thread / System / File ----------------------------

static void build_service_units(std::vector<ClassUnit> *units)
{
    // synth.Worker: run() counts to 1000 in one-instruction steps so a 256-token
    // slice only makes partial progress.
    UnitBuilder w;
    w.add_field(false, "n", "I");
    int m_n = w.add_member("synth.Worker", "n", "I");
    Code wc;
    wc.op(0x01, 1);
    wc.op(0x1b, (uint32_t)m_n);
    wc.op(0x08, 35);
    wc.op(0x2a);
    int wc_body = w.add_body(wc);
    Code run;
    size_t head = run.mark();
    run.op(0x1b, (uint32_t)m_n);
    run.op(0x0b, 1000);
    run.op(0x07, 11);  // n < 1000
    size_t exit_jump = run.mark();
    run.op(0x15);
    run.op(0x1b, (uint32_t)m_n);
    run.op(0x07, 28);
    run.op(0x0b, 1);
    run.op(0x07, 16);  // n + 1
    run.op(0x1b, (uint32_t)m_n);
    run.op(0x07, 35);  // n = ...
    size_t back = run.mark();
    run.op(0x14);
    run.patch(back, head);
    run.patch(exit_jump, run.mark());
    run.op(0x29);
    int run_body = w.add_body(run);
    Code gn;
    gn.op(0x01, 0);
    gn.op(0x1b, (uint32_t)m_n);
    gn.op(0x28);
    int gn_body = w.add_body(gn);
    w.add_method(false, 0x1, "<init>", "(I)V", wc_body, 2);
    w.add_method(false, 0x1, "run", "()V", run_body, 1);
    w.add_method(false, 0x1, "getN", "()I", gn_body, 1);
    units->push_back(w.finish("synth.Worker", ""));

    UnitBuilder t;
    t.add_field(false, "target", "Ljava.lang.Runnable;");
    t.add_method(false, 0x41, "init", "(Ljava.lang.String;)V", -1, 0);
    t.add_method(false, 0x41, "start", "()V", -1, 0);
    t.add_method(false, 0x41, "stop", "()V", -1, 0);
    t.add_method(false, 0x41, "isAlive", "()I", -1, 0);
    t.add_method(false, 0x41, "getPriority", "()I", -1, 0);
    t.add_method(false, 0x41, "setPriority", "(I)V", -1, 0);
    t.add_method(false, 0x41, "sleep", "(F)V", -1, 0);
    t.add_method(false, 0x41, "suspend", "()V", -1, 0);
    t.add_method(false, 0x41, "resume", "()V", -1, 0);
    t.add_method(false, 0x41, "setDaemon", "(I)V", -1, 0);
    units->push_back(t.finish("java.lang.Thread", ""));

    UnitBuilder s;
    s.add_method(true, 0x49, "log", "(Ljava.lang.String;)V", -1, 0);
    s.add_method(true, 0x49, "gc", "()I", -1, 0);
    s.add_method(true, 0x49, "simTime", "()F", -1, 0);
    s.add_method(true, 0x49, "timeWarp", "(F)F", -1, 0);
    s.add_method(true, 0x49, "memUsage", "()I", -1, 0);
    s.add_method(true, 0x49, "currentTime", "()F", -1, 0);
    units->push_back(s.finish("java.lang.System", ""));

    UnitBuilder f;
    f.add_field(false, "name", "Ljava.lang.String;");
    f.add_method(false, 0x41, "open", "(I)I", -1, 0);
    f.add_method(false, 0x41, "close", "()V", -1, 0);
    f.add_method(false, 0x41, "write", "(I)I", -1, 0);
    f.add_method(false, 0x41, "write", "(F)I", -1, 0);
    f.add_method(false, 0x41, "write", "(Ljava.lang.String;)I", -1, 0);
    f.add_method(false, 0x41, "readInt", "()I", -1, 0);
    f.add_method(false, 0x41, "readFloat", "()F", -1, 0);
    f.add_method(false, 0x41, "readString", "()Ljava.lang.String;", -1, 0);
    f.add_method(false, 0x41, "readResID", "()I", -1, 0);
    f.add_method(true, 0x49, "delete", "(Ljava.lang.String;)I", -1, 0);
    f.add_method(true, 0x49, "copy",
                 "(Ljava.lang.String;Ljava.lang.String;)I", -1, 0);
    f.add_method(true, 0x49, "move",
                 "(Ljava.lang.String;Ljava.lang.String;)I", -1, 0);
    f.add_method(true, 0x49, "exists", "(Ljava.lang.String;)I", -1, 0);
    units->push_back(f.finish("java.io.File", ""));
}

static const ClassUnit *unit_by_name(const std::vector<ClassUnit> &units,
                                     const char *name)
{
    for (const ClassUnit &u : units)
        if (u.class_name(u.self_class) == name) return &u;
    return nullptr;
}

static uint32_t le32(const std::vector<uint8_t> &b, size_t at)
{
    return (uint32_t)b[at] | (uint32_t)b[at + 1] << 8 | (uint32_t)b[at + 2] << 16 |
           (uint32_t)b[at + 3] << 24;
}

static void vm_service_tests(Checker &c)
{
    std::printf("VM service natives (Thread/System/File)\n");
    std::vector<ClassUnit> units;
    build_service_units(&units);
    // Deliberately leaked: scheduler and file state in natives.cpp are keyed by
    // Vm*/Object* and the VM has no teardown hook, so the VM must outlive them.
    Vm &vm = *new Vm();
    for (ClassUnit &u : units) vm.add_class(&u);
    const ClassUnit *threadu = unit_by_name(units, "java.lang.Thread");
    const ClassUnit *workeru = unit_by_name(units, "synth.Worker");
    const ClassUnit *sysu = unit_by_name(units, "java.lang.System");
    const ClassUnit *fileu = unit_by_name(units, "java.io.File");
    c.check(threadu && workeru && sysu && fileu && vm.find_class("java.lang.Thread") &&
                vm.find_class("synth.Worker") && vm.find_class("java.lang.System") &&
                vm.find_class("java.io.File"),
            "service fixtures registered");
    if (!threadu || !workeru || !sysu || !fileu) return;

    std::string err;
    Value r;
    bool ran;

    // System: clock, collector, memory, logging.
    ran = call_static(vm, *sysu, "simTime", "()F", {}, &r, &err);
    expect_float(c, ran, r, 0.0f, 0.0f, "System.simTime starts at 0", err);
    ran = call_static(vm, *sysu, "gc", "()I", {}, &r, &err);
    expect_int(c, ran, r, 0, "System.gc", err);
    ran = call_static(vm, *sysu, "memUsage", "()I", {}, &r, &err);
    c.check(ran, "System.memUsage runs" + (ran ? std::string() : ": " + err));
    if (ran) c.check(r.kind == Value::Int && r.i > 0, "System.memUsage > 0 kB");
    ran = call_static(vm, *sysu, "log", "(Ljava.lang.String;)V",
                      {Value::make_null()}, &r, &err);
    c.check(ran, "System.log(null)" + (ran ? std::string() : ": " + err));
    ran = call_static(vm, *sysu, "log", "(Ljava.lang.String;)V",
                      {Value::make_str("vm_service: log line")}, &r, &err);
    c.check(ran, "System.log(String)" + (ran ? std::string() : ": " + err));
    float t1 = 0.0f, t2 = 0.0f;
    ran = call_static(vm, *sysu, "currentTime", "()F", {}, &r, &err);
    if (ran && r.kind == Value::Float) t1 = r.f;
    ran = call_static(vm, *sysu, "currentTime", "()F", {}, &r, &err);
    if (ran && r.kind == Value::Float) t2 = r.f;
    c.check(ran && t1 >= 0.0f && t2 >= t1, "System.currentTime monotonic");

    // Thread: state transitions, priorities and cooperative sleep.
    ClassState *tcs = vm.find_class("java.lang.Thread");
    ClassState *wcs = vm.find_class("synth.Worker");
    Object *worker = vm.new_object(wcs, &err);
    c.check(worker != nullptr, "worker object" + (worker ? std::string() : ": " + err));
    if (!worker) return;
    ran = call_instance(vm, *workeru, "<init>", "(I)V", worker, {vi(0)}, &r, &err);
    c.check(ran, "worker constructor" + (ran ? std::string() : ": " + err));
    Object *th = vm.new_object(tcs, &err);
    c.check(th != nullptr, "thread object" + (th ? std::string() : ": " + err));
    if (!th) return;
    ran = vm.set_instance_field(th, "target", Value::make_obj(worker), &err);
    c.check(ran, "thread target" + (ran ? std::string() : ": " + err));
    ran = call_instance(vm, *threadu, "init", "(Ljava.lang.String;)V", th,
                        {Value::make_str("worker")}, &r, &err);
    c.check(ran, "Thread.init" + (ran ? std::string() : ": " + err));
    ran = call_instance(vm, *threadu, "isAlive", "()I", th, {}, &r, &err);
    expect_int(c, ran, r, 0, "Thread.isAlive before start", err);
    ran = call_instance(vm, *threadu, "getPriority", "()I", th, {}, &r, &err);
    expect_int(c, ran, r, 0, "Thread.getPriority default", err);
    ran = call_instance(vm, *threadu, "setPriority", "(I)V", th, {vi(7)}, &r, &err);
    c.check(ran, "Thread.setPriority" + (ran ? std::string() : ": " + err));
    ran = call_instance(vm, *threadu, "getPriority", "()I", th, {}, &r, &err);
    expect_int(c, ran, r, 7, "Thread.getPriority after set", err);
    ran = call_instance(vm, *threadu, "setDaemon", "(I)V", th, {vi(1)}, &r, &err);
    c.check(ran, "Thread.setDaemon" + (ran ? std::string() : ": " + err));
    ran = call_instance(vm, *threadu, "start", "()V", th, {}, &r, &err);
    c.check(ran, "Thread.start" + (ran ? std::string() : ": " + err));
    ran = call_instance(vm, *threadu, "isAlive", "()I", th, {}, &r, &err);
    expect_int(c, ran, r, 1, "Thread.isAlive after start", err);

    // Thread.sleep takes milliseconds; 250 ms advances the sim clock 0.25 s.
    ran = call_instance(vm, *threadu, "sleep", "(F)V", th, {vf(250.0f)}, &r, &err);
    c.check(ran, "Thread.sleep" + (ran ? std::string() : ": " + err));
    ran = call_static(vm, *sysu, "simTime", "()F", {}, &r, &err);
    expect_float(c, ran, r, 0.25f, 1e-6f, "sleep advances simTime", err);
    int32_t n1 = 0, n2 = 0;
    ran = call_instance(vm, *workeru, "getN", "()I", worker, {}, &r, &err);
    if (ran && r.kind == Value::Int) n1 = r.i;
    c.check(ran && n1 > 0 && n1 < 1000, "sleep pumps one slice of run()");

    ran = call_instance(vm, *threadu, "suspend", "()V", th, {}, &r, &err);
    c.check(ran, "Thread.suspend" + (ran ? std::string() : ": " + err));
    ran = call_instance(vm, *threadu, "sleep", "(F)V", th, {vf(250.0f)}, &r, &err);
    c.check(ran, "sleep while suspended" + (ran ? std::string() : ": " + err));
    ran = call_instance(vm, *workeru, "getN", "()I", worker, {}, &r, &err);
    c.check(ran && r.kind == Value::Int && r.i == n1, "suspended thread frozen");
    ran = call_static(vm, *sysu, "simTime", "()F", {}, &r, &err);
    expect_float(c, ran, r, 0.5f, 1e-6f, "clock advances while suspended", err);
    ran = call_instance(vm, *threadu, "resume", "()V", th, {}, &r, &err);
    c.check(ran, "Thread.resume" + (ran ? std::string() : ": " + err));
    ran = call_instance(vm, *threadu, "sleep", "(F)V", th, {vf(250.0f)}, &r, &err);
    c.check(ran, "sleep after resume" + (ran ? std::string() : ": " + err));
    ran = call_instance(vm, *workeru, "getN", "()I", worker, {}, &r, &err);
    if (ran && r.kind == Value::Int) n2 = r.i;
    c.check(ran && n2 > n1 && n2 < 1000, "resume advances the worker");

    ran = call_static(vm, *sysu, "timeWarp", "(F)F", {vf(0.0f)}, &r, &err);
    expect_float(c, ran, r, 1.0f, 0.0f, "timeWarp(0) returns previous 1.0", err);
    ran = call_instance(vm, *threadu, "sleep", "(F)V", th, {vf(250.0f)}, &r, &err);
    c.check(ran, "sleep during timeWarp(0)" + (ran ? std::string() : ": " + err));
    ran = call_static(vm, *sysu, "simTime", "()F", {}, &r, &err);
    expect_float(c, ran, r, 0.75f, 1e-6f, "timeWarp(0) freezes simTime", err);
    ran = call_static(vm, *sysu, "timeWarp", "(F)F", {vf(2.0f)}, &r, &err);
    expect_float(c, ran, r, 0.0f, 0.0f, "timeWarp(2) returns previous 0.0", err);
    ran = call_instance(vm, *threadu, "sleep", "(F)V", th, {vf(100.0f)}, &r, &err);
    c.check(ran, "sleep at 2x warp" + (ran ? std::string() : ": " + err));
    ran = call_static(vm, *sysu, "simTime", "()F", {}, &r, &err);
    expect_float(c, ran, r, 0.95f, 1e-5f, "timeWarp(2) doubles sleep time", err);
    ran = call_static(vm, *sysu, "timeWarp", "(F)F", {vf(1.0f)}, &r, &err);
    expect_float(c, ran, r, 2.0f, 0.0f, "timeWarp(1) returns previous 2.0", err);

    bool finished = false;
    for (int i = 0; i < 200 && !finished; i++) {
        ran = call_instance(vm, *threadu, "isAlive", "()I", th, {}, &r, &err);
        finished = ran && r.kind == Value::Int && r.i == 0;
        if (!finished) {
            ran = call_instance(vm, *threadu, "sleep", "(F)V", th, {vf(250.0f)}, &r,
                                &err);
            c.check(ran, "pump to completion" + (ran ? std::string() : ": " + err));
        }
    }
    c.check(finished, "worker reaches completion");
    ran = call_instance(vm, *workeru, "getN", "()I", worker, {}, &r, &err);
    expect_int(c, ran, r, 1000, "run() ran to completion", err);
    ran = call_instance(vm, *threadu, "isAlive", "()I", th, {}, &r, &err);
    expect_int(c, ran, r, 0, "Thread.isAlive after completion", err);

    // stop freezes a running thread; a missing target never starts.
    Object *worker2 = vm.new_object(wcs, &err);
    Object *th2 = vm.new_object(tcs, &err);
    if (worker2 && th2) {
        call_instance(vm, *workeru, "<init>", "(I)V", worker2, {vi(0)}, &r, &err);
        vm.set_instance_field(th2, "target", Value::make_obj(worker2), &err);
        call_instance(vm, *threadu, "init", "(Ljava.lang.String;)V", th2,
                      {Value::make_str("stopper")}, &r, &err);
        call_instance(vm, *threadu, "start", "()V", th2, {}, &r, &err);
        call_instance(vm, *threadu, "sleep", "(F)V", th2, {vf(250.0f)}, &r, &err);
        int32_t before = 0, after = 0;
        ran = call_instance(vm, *workeru, "getN", "()I", worker2, {}, &r, &err);
        if (ran && r.kind == Value::Int) before = r.i;
        c.check(ran && before > 0, "second worker progressed");
        ran = call_instance(vm, *threadu, "stop", "()V", th2, {}, &r, &err);
        c.check(ran, "Thread.stop" + (ran ? std::string() : ": " + err));
        ran = call_instance(vm, *threadu, "isAlive", "()I", th2, {}, &r, &err);
        expect_int(c, ran, r, 0, "Thread.isAlive after stop", err);
        call_instance(vm, *threadu, "sleep", "(F)V", th2, {vf(250.0f)}, &r, &err);
        ran = call_instance(vm, *workeru, "getN", "()I", worker2, {}, &r, &err);
        if (ran && r.kind == Value::Int) after = r.i;
        c.check(ran && after == before, "stopped thread no longer runs");
    } else {
        c.check(false, "second thread fixture");
    }
    Object *th3 = vm.new_object(tcs, &err);
    if (th3) {
        ran = call_instance(vm, *threadu, "start", "()V", th3, {}, &r, &err);
        c.check(ran, "Thread.start without target" +
                         (ran ? std::string() : ": " + err));
        ran = call_instance(vm, *threadu, "isAlive", "()I", th3, {}, &r, &err);
        expect_int(c, ran, r, 0, "targetless thread is not alive", err);
    }

    // File: SDAT v2 round-trip in a temp dir, binary bytes and error paths.
    char tmpl[] = "/tmp/slrr-vm-services-XXXXXX";
    char *dir = mkdtemp(tmpl);
    c.check(dir != nullptr, "temp dir created");
    if (!dir) return;
    std::string base(dir);
    auto make_file = [&](const std::string &path) -> Object * {
        ClassState *fcs = vm.find_class("java.io.File");
        Object *o = vm.new_object(fcs, &err);
        if (o && !vm.set_instance_field(o, "name", Value::make_str(path), &err))
            return nullptr;
        return o;
    };
    std::string path = base + "/round.sdat";
    Object *f = make_file(path);
    c.check(f != nullptr, "file object");
    if (!f) return;
    ran = call_instance(vm, *fileu, "open", "(I)I", f, {vi(1)}, &r, &err);
    expect_int(c, ran, r, 1, "File.open(write)", err);
    ran = call_instance(vm, *fileu, "write", "(I)I", f, {vi(0x11223344)}, &r, &err);
    expect_int(c, ran, r, 4, "File.write(int)", err);
    ran = call_instance(vm, *fileu, "write", "(F)I", f, {vf(1.5f)}, &r, &err);
    expect_int(c, ran, r, 4, "File.write(float)", err);
    ran = call_instance(vm, *fileu, "write", "(Ljava.lang.String;)I", f,
                        {Value::make_str("hello")}, &r, &err);
    expect_int(c, ran, r, 6, "File.write(String)", err);
    ran = call_instance(vm, *fileu, "close", "()V", f, {}, &r, &err);
    c.check(ran, "File.close(write)" + (ran ? std::string() : ": " + err));

    size_t size = 0;
    c.check(fs::is_file(path, &size) && size == 38, "SDAT file is 38 bytes");
    std::vector<uint8_t> bytes;
    std::string ferr;
    c.check(fs::read_file(path, &bytes, &ferr) && bytes.size() == 38, "SDAT bytes read");
    if (bytes.size() == 38) {
        c.check(memcmp(&bytes[0], "SDAT\0\1\2\0", 8) == 0, "SDAT header");
        c.check(le32(bytes, 8) == 0x11223344 && le32(bytes, 12) == 0x3fc00000 &&
                    le32(bytes, 16) == 6 && memcmp(&bytes[20], "hello\0", 6) == 0,
                "SDAT data section is binary exact");
        c.check(le32(bytes, 26) == 0 && le32(bytes, 30) == 0 &&
                    le32(bytes, 34) == 8,
                "empty trailer written on close");
    }
    std::string mixed = path;
    for (char &ch : mixed)
        if (ch == '/') ch = '\\';
    ran = call_static(vm, *fileu, "exists", "(Ljava.lang.String;)I",
                      {Value::make_str(mixed)}, &r, &err);
    expect_int(c, ran, r, 1, "File.exists normalizes backslashes", err);

    Object *rf = make_file(path);
    ran = call_instance(vm, *fileu, "open", "(I)I", rf, {vi(0)}, &r, &err);
    expect_int(c, ran, r, 1, "File.open(read)", err);
    ran = call_instance(vm, *fileu, "readInt", "()I", rf, {}, &r, &err);
    expect_int(c, ran, r, 0x11223344, "File.readInt", err);
    ran = call_instance(vm, *fileu, "readFloat", "()F", rf, {}, &r, &err);
    expect_float(c, ran, r, 1.5f, 0.0f, "File.readFloat", err);
    ran = call_instance(vm, *fileu, "readString", "()Ljava.lang.String;", rf, {}, &r,
                        &err);
    expect_str(c, ran, r, "hello", "File.readString", err);
    ran = call_instance(vm, *fileu, "readString", "()Ljava.lang.String;", rf, {}, &r,
                        &err);
    c.check(ran && r.kind == Value::Null, "File.readString zero length is null");
    ran = call_instance(vm, *fileu, "readInt", "()I", rf, {}, &r, &err);
    expect_int(c, ran, r, 0, "File.readInt at EOF", err);
    ran = call_instance(vm, *fileu, "close", "()V", rf, {}, &r, &err);
    c.check(ran, "File.close(read)");

    std::string cpy = base + "/copy.sdat";
    ran = call_static(vm, *fileu, "copy",
                      "(Ljava.lang.String;Ljava.lang.String;)I",
                      {Value::make_str(mixed), Value::make_str(cpy)}, &r, &err);
    expect_int(c, ran, r, 1, "File.copy", err);
    std::vector<uint8_t> copied;
    c.check(fs::read_file(cpy, &copied, &ferr) && copied == bytes, "copy is binary exact");
    std::string moved = base + "/moved.sdat";
    ran = call_static(vm, *fileu, "move",
                      "(Ljava.lang.String;Ljava.lang.String;)I",
                      {Value::make_str(cpy), Value::make_str(moved)}, &r, &err);
    expect_int(c, ran, r, 1, "File.move", err);
    ran = call_static(vm, *fileu, "exists", "(Ljava.lang.String;)I",
                      {Value::make_str(cpy)}, &r, &err);
    expect_int(c, ran, r, 0, "move removed the source", err);
    ran = call_static(vm, *fileu, "exists", "(Ljava.lang.String;)I",
                      {Value::make_str(moved)}, &r, &err);
    expect_int(c, ran, r, 1, "move created the target", err);
    ran = call_static(vm, *fileu, "delete", "(Ljava.lang.String;)I",
                      {Value::make_str(moved)}, &r, &err);
    expect_int(c, ran, r, 1, "File.delete", err);
    ran = call_static(vm, *fileu, "delete", "(Ljava.lang.String;)I",
                      {Value::make_str(moved)}, &r, &err);
    expect_int(c, ran, r, 0, "File.delete missing file", err);

    Object *missing = make_file(base + "/absent.sdat");
    ran = call_instance(vm, *fileu, "open", "(I)I", missing, {vi(0)}, &r, &err);
    expect_int(c, ran, r, 0, "open missing file fails", err);
    ran = call_instance(vm, *fileu, "write", "(I)I", missing, {vi(1)}, &r, &err);
    expect_int(c, ran, r, 0, "write without a handle", err);
    ran = call_instance(vm, *fileu, "readInt", "()I", missing, {}, &r, &err);
    expect_int(c, ran, r, 0, "read without a handle", err);
    ran = call_instance(vm, *fileu, "close", "()V", missing, {}, &r, &err);
    c.check(ran, "close without a handle");
    Object *badmode = make_file(base + "/badmode.sdat");
    ran = call_instance(vm, *fileu, "open", "(I)I", badmode, {vi(2)}, &r, &err);
    expect_int(c, ran, r, 0, "open invalid mode fails", err);
    Object *nodir = make_file(base + "/nodir/x.sdat");
    ran = call_instance(vm, *fileu, "open", "(I)I", nodir, {vi(1)}, &r, &err);
    expect_int(c, ran, r, 0, "open in missing directory fails", err);

    remove(path.c_str());
    rmdir(base.c_str());
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
        c.check(!ran && err == "native not implemented (stub): java.lang.System.exit()V",
                "engine shutdown flag is an explicit stub");

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

        err.clear();
        ran = call_static(vm, *sys, "exit", "()V", {}, &r, &err);
        c.check(!ran && err == "native not implemented (stub): java.lang.System.exit()V",
                "engine native is an explicit stub");
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

// ---- game runtime object/id layer -----------------------------------------

// The selfcheck samples live under <game>/sl/...; derive the game root so the
// runtime check can open the shipped packages next to them.
static std::string game_root(int argc, char **argv)
{
    const std::string needle = "the game";
    for (int i = 0; i < argc; i++) {
        std::string p = argv[i];
        size_t at = p.find(needle);
        if (at != std::string::npos) return p.substr(0, at + needle.size());
    }
    return std::string();
}

static void runtime_tests(const std::string &game, Checker &c)
{
    std::printf("game runtime object/id layer\n");
    static const char *kCreateSig =
        "(Ljava.util.resource.GameRef;Ljava.util.resource.GameRef;"
        "Ljava.lang.String;Ljava.lang.String;)Ljava.lang.GameType;";
    static const char *kCreateNativeSig =
        "(Ljava.util.resource.GameRef;Ljava.util.resource.GameRef;"
        "Ljava.lang.String;Ljava.lang.String;)V";
    static const char *kGTISig =
        "(Ljava.util.resource.GameRef;ILjava.lang.String;Ljava.lang.String;)V";

    ResourceManager rm;
    std::string err;
    bool ok = rm.open((game + "/system.rpk").c_str(), (game + "/system.rdb").c_str(), &err);
    c.check(ok, "runtime: open system package" + (ok ? std::string() : ": " + err));
    if (!ok) return;
    ok = rm.open((game + "/cars/racers/Axis.rpk").c_str(),
                 (game + "/cars/racers/Axis.rdb").c_str(), &err);
    c.check(ok, "runtime: open Axis package" + (ok ? std::string() : ": " + err));
    if (!ok) return;

    // Only the classes the runtime check needs (the record `script` refs alone
    // do not pull in the engine-side resource classes).
    static const char *kFiles[] = {
        "system\\scripts\\lang\\Object.class",
        "system\\scripts\\lang\\Native.class",
        "system\\scripts\\lang\\GameType.class",
        "system\\scripts\\lang\\Vector3.class",
        "system\\scripts\\lang\\Ypr.class",
        "system\\scripts\\util\\resource\\ResourceRef.class",
        "system\\scripts\\util\\resource\\GameRef.class",
        "system\\scripts\\util\\resource\\GroundRef.class",
        "cars\\racers\\Axis_data\\scripts\\Axis_FL_door.class",
        "cars\\racers\\Axis_data\\scripts\\Axis_200S.class",
        "parts\\scripts\\Part.class",
        "parts\\scripts\\BodyPart.class",
        "parts\\scripts\\WheelRef.class",
        "parts\\scripts\\bodypart\\Chassis.class",
    };
    ClassPath cp;
    for (const char *rel : kFiles) {
        std::string path = rm.resolve_path(rel);
        bool loaded = !path.empty() && cp.add_file(path, &err);
        c.check(loaded, std::string("runtime: load ") + rel +
                            (loaded ? std::string()
                                    : ": " + (path.empty() ? "not found" : err)));
    }
    cp.link();

    Vm vm;
    for (const ClassUnit *u : cp.units()) vm.add_class(u);
    natives::install(vm);
    Runtime rt(rm, &vm, &cp);
    rt.activate();

    ClassEntry *rr_entry = cp.find("java.util.resource.ResourceRef");
    ClassEntry *gr_entry = cp.find("java.util.resource.GameRef");
    ClassEntry *gt_entry = cp.find("java.lang.GameType");
    const ClassUnit *rr_unit = rr_entry ? rr_entry->unit : nullptr;
    const ClassUnit *gr_unit = gr_entry ? gr_entry->unit : nullptr;
    const ClassUnit *gt_unit = gt_entry ? gt_entry->unit : nullptr;
    c.check(rr_unit && gr_unit && gt_unit, "runtime: resource classes loaded");
    if (!rr_unit || !gr_unit || !gt_unit) {
        rt.deactivate();
        return;
    }

    // ---- package id resolution and super chain ----
    std::vector<ResourceHit> hits = rm.lookup(0xD2);
    c.check(hits.size() == 1 && hits[0].package == &rm.package(1),
            "runtime: 0xD2 resolves only in the Axis package");
    Runtime::Resource *fl = rt.resolve(0xD2);
    c.check(fl != nullptr, "runtime: resolve 0xD2");
    if (!fl) {
        rt.deactivate();
        return;
    }
    c.check(fl->type == 8, "runtime: 0xD2 is RESTYPE_GAME (8)");
    c.check(rt.type_name(fl->type) == "typ_game_object", "runtime: type name typ_game_object");
    c.check(fl->name == "FL", "runtime: 0xD2 record name FL");
    c.check(fl->script_class == "java.game.cars.Axis_FL_door",
            "runtime: 0xD2 script class");
    c.check(fl->parent_id == 7 && fl->parent && fl->parent->id == 7,
            "runtime: 0xD2 super id 0x7");
    c.check(fl->parent && fl->parent->name == ">Stock", "runtime: super is >Stock");
    c.check(fl->parent && fl->parent->parent_id == 0x0004f242 &&
                fl->parent->parent == nullptr,
            "runtime: external super stays unresolved");
    std::vector<ChainLink> links;
    ok = rt.chain(fl, &links, &err);
    c.check(ok && links.size() == 3, "runtime: chain has three links" +
                                         (ok ? std::string() : ": " + err));
    c.check(ok && links.size() == 3 && links[0].id == 0xD2 && links[1].id == 7 &&
                links[2].id == 0x0004f242 && links[2].external,
            "runtime: chain FL -> >Stock -> external parts.rpk");
    std::string chain_err;
    c.check(!rt.chain(nullptr, &links, &chain_err) && !chain_err.empty(),
            "runtime: chain of a missing resource fails");
    Runtime::Resource *sys7 = rt.resolve(7);
    c.check(sys7 && sys7->name == "res_mesh" && sys7->type == 5,
            "runtime: id 7 without a base resolves in the first package");
    c.check(rt.resolve(7, &rm.package(1)) == fl->parent,
            "runtime: id 7 with the Axis base resolves >Stock");
    c.check(rt.resolve(0x0004f242) == nullptr,
            "runtime: id in an unopened sub-package does not resolve");
    c.check(rt.resolve(0) == nullptr, "runtime: id 0 does not resolve");

    // ---- ResourceRef natives, driven through the VM ----
    ClassState *rr_state = vm.find_class("java.util.resource.ResourceRef");
    const MethodInfo *rr_ctor = find_method_sig(*rr_unit, "<init>", "()");
    c.check(rr_state && rr_ctor, "runtime: ResourceRef() constructor present");
    auto new_ref = [&]() -> Object * {
        if (!rr_state || !rr_ctor) return nullptr;
        Object *o = vm.new_object(rr_state, &err);
        Value r;
        if (!o || !vm.invoke(*rr_unit, *rr_ctor, o, {}, &r, &err)) return nullptr;
        return o;
    };
    Value out;
    bool ran;
    Object *ref = new_ref();
    c.check(ref != nullptr, "runtime: ResourceRef() runs newNative" +
                                (ref ? std::string() : ": " + err));
    c.check(ref && rt.handle(ref) && rt.handle(ref)->target == nullptr &&
                rt.handle(ref)->id == 0,
            "runtime: a fresh ref has an empty handle");
    if (ref) {
        ran = call_instance(vm, *rr_unit, "set", "(I)V", ref, {vi(0xD2)}, &out, &err);
        c.check(ran, "runtime: ref.set(0xD2)" + (ran ? std::string() : ": " + err));
        c.check(rt.handle(ref) && rt.handle(ref)->target == fl &&
                    rt.handle(ref)->id == 0xD2,
                "runtime: set(int) binds the handle");
        c.check(fl->refs == 1, "runtime: 0xD2 refcount 1");
        ran = call_instance(vm, *rr_unit, "type", "()I", ref, {}, &out, &err);
        expect_int(c, ran, out, 8, "runtime: ref.type()", err);
        ran = call_instance(vm, *rr_unit, "id", "()I", ref, {}, &out, &err);
        expect_int(c, ran, out, 0xD2, "runtime: ref.id()", err);
        ran = call_instance(vm, *rr_unit, "getParentID", "()I", ref, {}, &out, &err);
        expect_int(c, ran, out, 7, "runtime: ref.getParentID()", err);
        ran = call_instance(vm, *rr_unit, "getParent",
                            "()Ljava.util.resource.ResourceRef;", ref, {}, &out, &err);
        Object *parent_obj = out.kind == Value::Obj ? out.obj : nullptr;
        c.check(ran, "runtime: ref.getParent()" + (ran ? std::string() : ": " + err));
        c.check(parent_obj && rt.handle(parent_obj) &&
                    rt.handle(parent_obj)->target == fl->parent &&
                    rt.handle(parent_obj)->id == 7,
                "runtime: getParent wraps the super");
        c.check(fl->parent->refs == 1, "runtime: >Stock refcount 1");
        c.check(ran && call_instance(vm, *rr_unit, "getWTRoot",
                                     "()Ljava.util.resource.ResourceRef;", ref, {}, &out,
                                     &err) &&
                    out.kind == Value::Null,
                "runtime: getWTRoot of a plain record is null");
        c.check(call_instance(vm, *rr_unit, "getFirstChild",
                              "()Ljava.util.resource.ResourceRef;", ref, {}, &out, &err) &&
                    out.kind == Value::Null,
                "runtime: getFirstChild of a plain record is null");
    }

    Object *ref2 = new_ref();
    if (ref && ref2) {
        ran = call_instance(vm, *rr_unit, "set", "(Ljava.util.resource.ResourceRef;)V",
                            ref2, {Value::make_obj(ref)}, &out, &err);
        c.check(ran, "runtime: ref2.set(ref)" + (ran ? std::string() : ": " + err));
        c.check(rt.handle(ref2) && rt.handle(ref2)->target == fl &&
                    rt.handle(ref2)->id == 0xD2,
                "runtime: set(ResourceRef) copies the target");
        c.check(fl->refs == 2, "runtime: 0xD2 refcount 2");
        ran = call_instance(vm, *rr_unit, "deleteNative", "()V", ref, {}, &out, &err);
        c.check(ran, "runtime: ref.deleteNative()" +
                         (ran ? std::string() : ": " + err));
        c.check(rt.handle(ref) == nullptr, "runtime: deleteNative detaches the handle");
        c.check(fl->refs == 1, "runtime: 0xD2 refcount back to 1");
    }

    // ---- GameRef natives ----
    Object *gr = rt.wrap(fl, &err);
    c.check(gr != nullptr, "runtime: wrap 0xD2 as GameRef" +
                                (gr ? std::string() : ": " + err));
    if (!gr) {
        rt.deactivate();
        return;
    }
    c.check(rt.handle(gr) && rt.handle(gr)->target == fl &&
                rt.handle(gr)->id == 0xD2,
            "runtime: GameRef wrapper binds 0xD2");
    ran = call_instance(vm, *gr_unit, "getFlags", "()I", gr, {}, &out, &err);
    expect_int(c, ran, out, 0, "runtime: getFlags", err);
    ran = call_instance(vm, *gr_unit, "setFlags", "(I)V", gr, {vi(0x10)}, &out, &err);
    c.check(ran, "runtime: setFlags(0x10)" + (ran ? std::string() : ": " + err));
    c.check(fl->flags == 0x10, "runtime: flags stored");
    ran = call_instance(vm, *gr_unit, "clearFlags", "(I)V", gr, {vi(0x10)}, &out, &err);
    c.check(ran, "runtime: clearFlags(0x10)" + (ran ? std::string() : ": " + err));
    c.check(fl->flags == 0, "runtime: flags cleared");

    auto make_pt = [&](const char *cls, float a, float b, float cc, const char *fa,
                       const char *fb, const char *fc) -> Object * {
        ClassState *cs = vm.find_class(cls);
        if (!cs) {
            err = std::string(cls) + " is not registered";
            return nullptr;
        }
        Object *o = vm.new_object(cs, &err);
        if (!o || !vm.set_instance_field(o, fa, vf(a), &err) ||
            !vm.set_instance_field(o, fb, vf(b), &err) ||
            !vm.set_instance_field(o, fc, vf(cc), &err))
            return nullptr;
        return o;
    };
    Object *pos = make_pt("java.lang.Vector3", 1.0f, 2.0f, 3.0f, "x", "y", "z");
    Object *vel = make_pt("java.lang.Vector3", 4.0f, 5.0f, 6.0f, "x", "y", "z");
    Object *ang = make_pt("java.lang.Vector3", 0.1f, 0.2f, 0.3f, "x", "y", "z");
    Object *ori = make_pt("java.lang.Ypr", 0.5f, 0.25f, 0.125f, "y", "p", "r");
    c.check(pos && vel && ang && ori, "runtime: Vector3/Ypr fixtures" +
                                          (pos && vel && ang && ori
                                               ? std::string()
                                               : ": " + err));
    if (pos && vel && ang && ori) {
        ran = call_instance(vm, *gr_unit, "setPos", "(Ljava.lang.Vector3;)V", gr,
                            {Value::make_obj(pos)}, &out, &err);
        c.check(ran && fl->pos[0] == 1.0f && fl->pos[1] == 2.0f && fl->pos[2] == 3.0f,
                "runtime: setPos updates the instance position");
        ran = call_instance(vm, *gr_unit, "getPos", "()Ljava.lang.Vector3;", gr, {},
                            &out, &err);
        float x = 0.0f, y = 0.0f, z = 0.0f;
        bool got = ran && out.kind == Value::Obj &&
                   obj_float(vm, out.obj, "x", &x, &err) &&
                   obj_float(vm, out.obj, "y", &y, &err) &&
                   obj_float(vm, out.obj, "z", &z, &err);
        c.check(got && x == 1.0f && y == 2.0f && z == 3.0f,
                "runtime: getPos returns the position");

        ran = call_instance(vm, *gr_unit, "setMatrix",
                            "(Ljava.lang.Vector3;Ljava.lang.Ypr;)V", gr,
                            {Value::make_obj(pos), Value::make_obj(ori)}, &out, &err);
        c.check(ran && fl->ori[0] == 0.5f && fl->ori[1] == 0.25f && fl->ori[2] == 0.125f,
                "runtime: setMatrix updates the orientation");
        c.check(call_instance(vm, *gr_unit, "getOri", "()Ljava.lang.Ypr;", gr, {}, &out,
                            &err) &&
                    out.kind == Value::Obj &&
                    obj_float(vm, out.obj, "y", &y, &err) && y == 0.5f,
                "runtime: getOri returns the orientation");

        ran = call_instance(vm, *gr_unit, "setState",
                            "(Ljava.lang.Vector3;Ljava.lang.Ypr;Ljava.lang.Vector3;"
                            "Ljava.lang.Vector3;)V",
                            gr,
                            {Value::make_obj(pos), Value::make_obj(ori),
                             Value::make_obj(vel), Value::make_obj(ang)},
                            &out, &err);
        c.check(ran && fl->vel[0] == 4.0f && fl->vel[2] == 6.0f && fl->ang[1] == 0.2f,
                "runtime: setState stores velocities");
        ran = call_instance(vm, *gr_unit, "getVel", "()Ljava.lang.Vector3;", gr, {},
                            &out, &err);
        got = ran && out.kind == Value::Obj &&
              obj_float(vm, out.obj, "x", &x, &err) &&
              obj_float(vm, out.obj, "z", &z, &err);
        c.check(got && x == 4.0f && z == 6.0f, "runtime: getVel returns the velocity");

        Object *fill = make_pt("java.lang.Vector3", 0.0f, 0.0f, 0.0f, "x", "y", "z");
        ran = call_instance(vm, *gr_unit, "getPos",
                            "(Ljava.lang.Vector3;)Ljava.lang.Vector3;", gr,
                            {Value::make_obj(fill)}, &out, &err);
        c.check(ran && out.kind == Value::Obj && out.obj == fill &&
                    obj_float(vm, fill, "y", &y, &err) && y == 2.0f,
                "runtime: getPos(out) fills and returns the argument");
        Object *fill_ori = make_pt("java.lang.Ypr", 0.0f, 0.0f, 0.0f, "y", "p", "r");
        ran = call_instance(vm, *gr_unit, "getOri",
                            "(Ljava.lang.Ypr;)Ljava.lang.Ypr;", gr,
                            {Value::make_obj(fill_ori)}, &out, &err);
        c.check(ran && out.kind == Value::Obj && out.obj == fill_ori &&
                    obj_float(vm, fill_ori, "r", &z, &err) && z == 0.125f,
                "runtime: getOri(out) fills and returns the argument");
        Object *fill_vel = make_pt("java.lang.Vector3", 0.0f, 0.0f, 0.0f, "x", "y", "z");
        ran = call_instance(vm, *gr_unit, "getVel",
                            "(Ljava.lang.Vector3;)Ljava.lang.Vector3;", gr,
                            {Value::make_obj(fill_vel)}, &out, &err);
        c.check(ran && out.kind == Value::Obj && out.obj == fill_vel &&
                    obj_float(vm, fill_vel, "y", &y, &err) && y == 5.0f,
                "runtime: getVel(out) fills and returns the argument");
    }

    // GameRef.class does not declare getDetail (natives.tsv lists the shared
    // 0x4e3330 address under both GameRef and RenderRef), so it cannot be
    // reached through a GameRef unit; the row stays implemented for parity.
    ran = call_instance(vm, *gr_unit, "isEmpty", "()I", gr, {}, &out, &err);
    expect_int(c, ran, out, 0, "runtime: a data-bearing type is not empty", err);
    ran = call_instance(vm, *gr_unit, "isScripted", "(Ljava.lang.String;)I", gr,
                        {Value::make_str("java.game.cars.Axis_FL_door")}, &out, &err);
    expect_int(c, ran, out, 1, "runtime: isScripted(exact class)", err);
    ran = call_instance(vm, *gr_unit, "isScripted", "(Ljava.lang.String;)I", gr,
                        {Value::make_str("java.game.parts.bodypart.FrontDoor")}, &out,
                        &err);
    expect_int(c, ran, out, 1, "runtime: isScripted(super class)", err);
    ran = call_instance(vm, *gr_unit, "isScripted", "(Ljava.lang.String;)I", gr,
                        {Value::make_str("java.lang.Native")}, &out, &err);
    expect_int(c, ran, out, 0, "runtime: isScripted(unrelated class)", err);
    ran = call_instance(vm, *gr_unit, "isScripted", "(Ljava.lang.String;)I", gr,
                        {Value::make_null()}, &out, &err);
    expect_int(c, ran, out, 1, "runtime: isScripted(null) skips the class check", err);
    ran = call_instance(vm, *gr_unit, "getScriptInstance", "()Ljava.lang.Object;", gr,
                        {}, &out, &err);
    c.check(ran && out.kind == Value::Obj && fl->script && out.obj == fl->script,
            "runtime: getScriptInstance of a type creates its script object");

    // ---- instances: create / create_native / world tree ----
    Object *recv_a = rt.wrap(fl, &err);
    ran = recv_a && call_instance(vm, *gr_unit, "create", kCreateSig, recv_a,
                                  {Value::make_obj(gr), Value::make_obj(gr),
                                   Value::make_str(""), Value::make_str("A")},
                                  &out, &err);
    c.check(ran, "runtime: create A" + (ran ? std::string() : ": " + err));
    Runtime::Resource *inst_a = rt.handle(recv_a) ? rt.handle(recv_a)->target : nullptr;
    c.check(inst_a && inst_a->instance && inst_a->type == 1 && inst_a->name == "A",
            "runtime: create binds a game instance");
    c.check(inst_a && inst_a->parent == fl && fl->children.size() == 1 &&
                fl->children[0] == inst_a,
            "runtime: instance joins the parent children");
    c.check(ran && out.kind == Value::Obj && inst_a && inst_a->script &&
                out.obj == inst_a->script && out.obj != recv_a,
            "runtime: create returns the script instance");
    c.check(inst_a && inst_a->script_class == "java.game.cars.Axis_FL_door",
            "runtime: instance keeps the type script class");
    c.check(inst_a && inst_a->refs == 1, "runtime: instance refcount 1");
    std::string inst_chain_err;
    c.check(inst_a && !rt.chain(inst_a, &links, &inst_chain_err) &&
                !inst_chain_err.empty(),
            "runtime: chain of an instance fails");
    if (inst_a) {
        ran = call_instance(vm, *gr_unit, "getScriptInstance", "()Ljava.lang.Object;",
                            recv_a, {}, &out, &err);
        c.check(ran && out.kind == Value::Obj && out.obj == inst_a->script,
                "runtime: getScriptInstance returns the created script");
        ran = call_instance(vm, *gr_unit, "isEmpty", "()I", recv_a, {}, &out, &err);
        expect_int(c, ran, out, 1, "runtime: an instance is empty by the type test", err);
    }

    Object *recv_b = rt.wrap(fl, &err);
    ran = recv_b && call_instance(vm, *gr_unit, "create", kCreateSig, recv_b,
                                  {Value::make_obj(gr), Value::make_obj(gr),
                                   Value::make_str(""), Value::make_str("B")},
                                  &out, &err);
    c.check(ran, "runtime: create B" + (ran ? std::string() : ": " + err));
    Runtime::Resource *inst_b = rt.handle(recv_b) ? rt.handle(recv_b)->target : nullptr;
    c.check(fl->children.size() == 2 && fl->children[1] == inst_b,
            "runtime: second instance appends to the siblings");
    c.check(rt.handle(recv_b) && rt.handle(recv_b)->id == inst_b->id,
            "runtime: instance id is exposed by the handle");

    ran = call_instance(vm, *rr_unit, "getFirstChild",
                        "()Ljava.util.resource.ResourceRef;", gr, {}, &out, &err);
    Object *child_a = out.kind == Value::Obj ? out.obj : nullptr;
    c.check(ran && child_a && rt.handle(child_a) &&
                rt.handle(child_a)->target == inst_a,
            "runtime: getFirstChild returns A");
    ran = child_a && call_instance(vm, *rr_unit, "getNextChild",
                                   "()Ljava.util.resource.ResourceRef;", child_a, {},
                                   &out, &err);
    Object *child_b = out.kind == Value::Obj ? out.obj : nullptr;
    c.check(ran && child_b && rt.handle(child_b) &&
                rt.handle(child_b)->target == inst_b,
            "runtime: getNextChild returns B");
    c.check(inst_a && inst_a->refs == 2, "runtime: A refcount 2 after wrappers");

    Object *recv_d = rt.wrap(fl, &err);
    ran = recv_d && child_a &&
          call_instance(vm, *gr_unit, "create", kCreateSig, recv_d,
                        {Value::make_obj(child_a), Value::make_obj(gr),
                         Value::make_str(""), Value::make_str("D")},
                        &out, &err);
    c.check(ran, "runtime: create D under A" + (ran ? std::string() : ": " + err));
    Runtime::Resource *inst_d = rt.handle(recv_d) ? rt.handle(recv_d)->target : nullptr;
    c.check(inst_d && inst_d->parent == inst_a && inst_d->world_root == inst_a,
            "runtime: world-tree root of a nested instance");
    ran = call_instance(vm, *rr_unit, "getWTRoot",
                        "()Ljava.util.resource.ResourceRef;", recv_d, {}, &out, &err);
    Object *root = out.kind == Value::Obj ? out.obj : nullptr;
    c.check(ran && root && rt.handle(root) && rt.handle(root)->target == inst_a,
            "runtime: getWTRoot returns the top instance");

    Object *recv_n = rt.wrap(fl, &err);
    ran = recv_n && call_instance(vm, *gr_unit, "create_native", kCreateNativeSig,
                                  recv_n,
                                  {Value::make_obj(gr), Value::make_obj(gr),
                                   Value::make_str(""), Value::make_str("N")},
                                  &out, &err);
    Runtime::Resource *inst_n = rt.handle(recv_n) ? rt.handle(recv_n)->target : nullptr;
    c.check(ran && inst_n && inst_n->instance && inst_n->script == nullptr,
            "runtime: create_native makes no script instance");
    ran = call_instance(vm, *gr_unit, "isScripted", "(Ljava.lang.String;)I", recv_n,
                        {Value::make_null()}, &out, &err);
    expect_int(c, ran, out, 0, "runtime: a native instance is not scripted", err);

    // ---- GameType.createNativeInstance ----
    Object *recv_g = rt.wrap(fl, &err);
    ran = recv_g && call_instance(vm, *gt_unit, "createNativeInstance", kGTISig,
                                  recv_g,
                                  {Value::make_null(), vi(0), Value::make_str(""),
                                   Value::make_str("T")},
                                  &out, &err);
    Runtime::Resource *inst_t = rt.handle(recv_g) ? rt.handle(recv_g)->target : nullptr;
    c.check(ran && inst_t && inst_t->instance && inst_t->type == 1 &&
                inst_t->parent == nullptr,
            "runtime: createNativeInstance(0) uses the receiver type");
    Object *recv_g2 = rt.wrap(fl, &err);
    ran = recv_g2 && call_instance(vm, *gt_unit, "createNativeInstance", kGTISig,
                                   recv_g2,
                                   {Value::make_obj(gr), vi(0xD2), Value::make_str(""),
                                    Value::make_str("T2")},
                                   &out, &err);
    Runtime::Resource *inst_t2 = rt.handle(recv_g2) ? rt.handle(recv_g2)->target : nullptr;
    c.check(ran && inst_t2 && inst_t2->parent == fl &&
                inst_t2->script_class == "java.game.cars.Axis_FL_door",
            "runtime: createNativeInstance(typeID) resolves and parents");

    // ---- car payloads: Chassis / WheelRef / Part ----
    ClassEntry *part_entry = cp.find("java.game.parts.Part");
    ClassEntry *chassis_entry = cp.find("java.game.parts.bodypart.Chassis");
    ClassEntry *wheel_entry = cp.find("java.game.parts.WheelRef");
    const ClassUnit *part_unit = part_entry ? part_entry->unit : nullptr;
    const ClassUnit *chassis_unit = chassis_entry ? chassis_entry->unit : nullptr;
    const ClassUnit *wheel_unit = wheel_entry ? wheel_entry->unit : nullptr;
    c.check(part_unit && chassis_unit && wheel_unit,
            "runtime: car classes loaded (Part/Chassis/WheelRef)");

    if (part_unit && chassis_unit && wheel_unit) {
        auto cint = [&](const ClassUnit *u, Object *self, const char *name,
                        const char *sig, const std::vector<Value> &args,
                        int32_t want, const std::string &what) {
            Value v;
            std::string e;
            bool ran = call_instance(vm, *u, name, sig, self, args, &v, &e);
            expect_int(c, ran, v, want, what, e);
        };
        auto cfloat = [&](const ClassUnit *u, Object *self, const char *name,
                          const char *sig, const std::vector<Value> &args,
                          float want, float tol, const std::string &what) {
            Value v;
            std::string e;
            bool ran = call_instance(vm, *u, name, sig, self, args, &v, &e);
            expect_float(c, ran, v, want, tol, what, e);
        };
        auto cstr = [&](const ClassUnit *u, Object *self, const char *name,
                        const char *sig, const std::vector<Value> &args,
                        const std::string &want, const std::string &what) {
            Value v;
            std::string e;
            bool ran = call_instance(vm, *u, name, sig, self, args, &v, &e);
            expect_str(c, ran, v, want, what, e);
        };
        auto pos_of = [&](Object *w, float p[3]) {
            Value v;
            std::string e;
            bool ran = call_instance(vm, *wheel_unit, "getPos",
                                     "()Ljava.lang.Vector3;", w, {}, &v, &e);
            return ran && v.kind == Value::Obj &&
                   obj_float(vm, v.obj, "x", &p[0], &e) &&
                   obj_float(vm, v.obj, "y", &p[1], &e) &&
                   obj_float(vm, v.obj, "z", &p[2], &e);
        };
        auto ypr_of = [&](Object *w, float p[3]) {
            Value v;
            std::string e;
            bool ran = call_instance(vm, *wheel_unit, "getYpr",
                                     "()Ljava.lang.Ypr;", w, {}, &v, &e);
            return ran && v.kind == Value::Obj &&
                   obj_float(vm, v.obj, "y", &p[0], &e) &&
                   obj_float(vm, v.obj, "p", &p[1], &e) &&
                   obj_float(vm, v.obj, "r", &p[2], &e);
        };

        // Record 6 is the Axis car (`_main_200S.cfg` supplies the four wheels).
        Runtime::Resource *car_type = rt.resolve(6, &rm.package(1));
        c.check(car_type && car_type->name == "Axis_200S" &&
                    car_type->script_class == "java.game.cars.Axis_200S",
                "car: id 6 resolves to Axis_200S");
        Runtime::Resource *car_inst =
            car_type ? rt.instantiate(car_type, car_type, "test_car", "", true, &err)
                     : nullptr;
        Object *car_obj = car_inst ? car_inst->script : nullptr;
        c.check(car_inst && car_obj && car_inst->script_class ==
                                              "java.game.cars.Axis_200S",
                "car: instantiate Axis_200S script");
        if (!car_inst || !car_obj) {
            rt.deactivate();
            return;
        }
        rt.bind(rt.attach(car_obj), car_inst);

        cint(chassis_unit, car_obj, "getWheels", "()I", {}, 4, "car: getWheels");
        Object *wobj[4] = {};
        for (int i = 0; i < 4; i++) {
            Value v;
            std::string e;
            bool ran = call_instance(vm, *chassis_unit, "getWheel",
                                     "(I)Ljava.game.parts.WheelRef;", car_obj,
                                     {vi(i)}, &v, &e);
            wobj[i] = v.kind == Value::Obj ? v.obj : nullptr;
            c.check(ran && wobj[i] != nullptr,
                    "car: getWheel(" + std::to_string(i) + ")");
        }
        {
            Value v;
            std::string e;
            bool ran = call_instance(vm, *chassis_unit, "getWheel",
                                     "(I)Ljava.game.parts.WheelRef;", car_obj,
                                     {vi(4)}, &v, &e);
            c.check(ran && v.kind == Value::Null, "car: getWheel(4) is null");
            ran = call_instance(vm, *chassis_unit, "getWheel",
                                "(I)Ljava.game.parts.WheelRef;", car_obj,
                                {vi(-1)}, &v, &e);
            c.check(ran && v.kind == Value::Null, "car: getWheel(-1) is null");
        }

        game::CarData *car = car_inst ? game::car_data(rt, car_inst) : nullptr;
        game::WheelData *wheels[4] = {};
        for (int i = 0; i < 4; i++)
            wheels[i] = wobj[i] ? game::wheel_data(wobj[i]) : nullptr;
        c.check(car && car->wheels.size() == 4, "car: four wheel records");
        c.check(car && wheels[0] == &car->wheels[0] &&
                    wheels[3] == &car->wheels[3],
                "car: WheelRef payloads point at the wheel array");

        if (car && wheels[0] && wheels[1] && wheels[2] && wheels[3]) {
            game::WheelData *w0 = wheels[0], *w2 = wheels[2];

            // createCar defaults from _main_200S.cfg.
            c.check(w0->base_len == 0.29f && w0->travel == 0.1f &&
                        w0->rest_len == 0.39f && w0->max_len == 0.39f &&
                        w0->min_len == -1.0f && w0->free_len == 0.705f &&
                        w0->radius == 0.315f,
                    "car: front wheel lengths from the cfg");
            c.check(std::fabs(w2->travel - 0.07f) < 1e-6f &&
                        std::fabs(w2->rest_len - 0.36f) < 1e-6f &&
                        std::fabs(w2->free_len - 0.675f) < 1e-6f,
                    "car: rear wheel lengths from the cfg");
            c.check(w0->friction == 1.0f && w0->friction_eff == 1.0f &&
                        w0->pacejka[0] == 1.4f && w0->pacejka[2] == 1.49f &&
                        w0->pacejka[4] == 15.2f && w0->pacejka[6] == 0.0f &&
                        w0->pacejka[8] == -1.0f && w0->pacejka[11] == 8000.0f &&
                        w0->pacejka[12] == 1.0f && w0->pacejka[13] == 0.015f &&
                        w0->pacejka[14] == 0.4f && w0->pacejka[16] == 0.0f,
                    "car: Pacejka defaults and friction");
            c.check(w0->drive == 1.0f && w0->drive_eff == 1.0f &&
                        w0->steer == 1.0f && w0->brake == 1.0f &&
                        w0->brake_eff == 1.0f && w0->hbrake == 0.0f &&
                        w2->steer == 0.0f && w2->hbrake == 1.0f &&
                        w2->hbrake_eff == 1.0f,
                    "car: drive/steer/brake flags from the wheel lines");
            c.check(w0->damage_visual == 27.0f && w0->template_damage == 27.0f &&
                        w0->opp == &car->wheels[1] &&
                        car->wheels[1].opp == &car->wheels[0] &&
                        car->wheels[2].opp == &car->wheels[3],
                    "car: damage base and opposite wheels");
            c.check(w0->spin == 0.0f && w0->spin_angle == 0.0f &&
                        w0->half_width == 0.1f && w0->roll_res == 0.015f &&
                        std::fabs(w0->hub[1] + 0.182f) < 1e-6f,
                    "car: createCar wheel state (front hub y = template+.39)");

            float p[3];
            c.check(pos_of(wobj[0], p) && std::fabs(p[0] + 0.68f) < 1e-5f &&
                        std::fabs(p[1]) < 1e-5f && std::fabs(p[2] + 1.3f) < 1e-5f,
                    "car: FL getPos returns the template pose");
            c.check(ypr_of(wobj[0], p) && std::fabs(p[0]) < 1e-6f &&
                        std::fabs(p[1]) < 1e-6f && std::fabs(p[2]) < 1e-6f,
                    "car: FL getYpr starts at zero");
            c.check(pos_of(wobj[2], p) && std::fabs(p[0] + 0.68f) < 1e-5f &&
                        std::fabs(p[2] - 1.125f) < 1e-5f,
                    "car: RL getPos from the cfg");
            c.check(pos_of(wobj[3], p) && std::fabs(p[0] - 0.73f) < 1e-5f,
                    "car: RR getPos from the cfg");
            cfloat(wheel_unit, wobj[0], "getDrive", "()F", {}, 1.0f, 1e-6f,
                   "car: getDrive");
            cfloat(wheel_unit, wobj[0], "getSteer", "()F", {}, 1.0f, 1e-6f,
                   "car: getSteer");
            cfloat(wheel_unit, wobj[0], "getRadius", "()F", {}, 0.315f, 1e-6f,
                   "car: getRadius");

            // Pacejka aliases.
            {
                Value v;
                std::string e;
                bool ran =
                    call_instance(vm, *wheel_unit, "setPacejka", "(IF)V",
                                  wobj[0], {vi(0), vf(2.5f)}, &v, &e) &&
                    call_instance(vm, *wheel_unit, "setPacejka", "(IF)V",
                                  wobj[0], {vi(16), vf(3.5f)}, &v, &e) &&
                    call_instance(vm, *wheel_unit, "setPacejka", "(IF)V",
                                  wobj[0], {vi(17), vf(9.0f)}, &v, &e) &&
                    call_instance(vm, *wheel_unit, "setPacejka", "(IF)V",
                                  wobj[0], {vi(-1), vf(9.0f)}, &v, &e);
                c.check(ran && w0->pacejka[0] == 2.5f &&
                            w0->pacejka[16] == 3.5f,
                        "car: setPacejka bounds 0..16");
                ran = call_instance(vm, *wheel_unit, "setSliction", "(F)V",
                                    wobj[0], {vf(0.7f)}, &v, &e) &&
                      call_instance(vm, *wheel_unit, "setStiffness", "(F)V",
                                    wobj[0], {vf(12.0f)}, &v, &e) &&
                      call_instance(vm, *wheel_unit, "setFrictn_x", "(F)V",
                                    wobj[0], {vf(0.9f)}, &v, &e) &&
                      call_instance(vm, *wheel_unit, "setMaxLoad", "(F)V",
                                    wobj[0], {vf(5000.0f)}, &v, &e) &&
                      call_instance(vm, *wheel_unit, "setLoadSmooth", "(F)V",
                                    wobj[0], {vf(0.5f)}, &v, &e);
                c.check(ran && w0->pacejka[0] == 0.7f &&
                            w0->pacejka[4] == 12.0f &&
                            w0->pacejka[12] == 0.9f &&
                            w0->pacejka[11] == 5000.0f &&
                            w0->pacejka[14] == 0.5f,
                        "car: Pacejka alias setters");
            }

            // setFriction writes base + p2 and applies tyre wear to the effective value.
            {
                Value v;
                std::string e;
                bool ran = call_instance(vm, *wheel_unit, "setFriction", "(F)V",
                                         wobj[0], {vf(2.0f)}, &v, &e);
                c.check(ran && w0->friction == 2.0f && w0->pacejka[2] == 2.0f &&
                            w0->friction_eff == 2.0f,
                        "car: setFriction writes p2 and effective");
                w0->tyre_wear = 0.5f;
                ran = call_instance(vm, *wheel_unit, "setFriction", "(F)V",
                                    wobj[0], {vf(2.0f)}, &v, &e);
                c.check(ran && std::fabs(w0->friction_eff - 1.7f) < 1e-6f,
                        "car: setFriction applies tyre wear (2*(1-0.25*0.6))");
                w0->tyre_wear = 0.0f;
                w0->brake_wear = 0.5f;
                ran = call_instance(vm, *wheel_unit, "setBrake", "(F)V",
                                    wobj[0], {vf(3.0f)}, &v, &e);
                c.check(ran && w0->brake == 3.0f &&
                            std::fabs(w0->brake_eff - 2.55f) < 1e-6f,
                        "car: setBrake effective uses brake wear");
                w0->brake_wear = 0.0f;
            }

            // Drive, lengths, spring and geometry.
            {
                Value v;
                std::string e;
                bool ran = call_instance(vm, *wheel_unit, "setDrive", "(F)V",
                                         wobj[0], {vf(0.25f)}, &v, &e);
                c.check(ran && w0->drive == 0.25f && w0->drive_eff == 0.25f,
                        "car: setDrive updates drive and effective");
                cfloat(wheel_unit, wobj[0], "getDrive", "()F", {}, 0.25f, 1e-6f,
                       "car: getDrive after setDrive");
                ran = call_instance(vm, *wheel_unit, "setMaxLen", "(F)V",
                                    wobj[0], {vf(0.8f)}, &v, &e) &&
                      call_instance(vm, *wheel_unit, "setMinLen", "(F)V",
                                    wobj[0], {vf(-0.4f)}, &v, &e) &&
                      call_instance(vm, *wheel_unit, "setRestLen", "(F)V",
                                    wobj[0], {vf(0.5f)}, &v, &e) &&
                      call_instance(vm, *wheel_unit, "setForce", "(F)V",
                                    wobj[0], {vf(1000.0f)}, &v, &e) &&
                      call_instance(vm, *wheel_unit, "setDamping", "(F)V",
                                    wobj[0], {vf(50.0f)}, &v, &e);
                c.check(ran && w0->max_len == 0.8f && w0->min_len == -0.4f &&
                            w0->rest_len == 0.5f &&
                            std::fabs(w0->free_len - 0.815f) < 1e-6f &&
                            w0->spring_k == 1000.0f && w0->bump == 50.0f &&
                            std::fabs(w0->rebound - 35.0f) < 1e-6f,
                        "car: length/spring setters (setMaxLen vs setMinLen)");
                ran = call_instance(vm, *wheel_unit, "setDamping", "(FF)V",
                                    wobj[0], {vf(60.0f), vf(20.0f)}, &v, &e) &&
                      call_instance(vm, *wheel_unit, "setRadius", "(F)V",
                                    wobj[0], {vf(0.4f)}, &v, &e) &&
                      call_instance(vm, *wheel_unit, "setWidth", "(F)V",
                                    wobj[0], {vf(0.5f)}, &v, &e) &&
                      call_instance(vm, *wheel_unit, "setRollRes", "(F)V",
                                    wobj[0], {vf(0.03f)}, &v, &e) &&
                      call_instance(vm, *wheel_unit, "setBearing", "(F)V",
                                    wobj[0], {vf(5.0f)}, &v, &e) &&
                      call_instance(vm, *wheel_unit, "setCPatch", "(FFF)V",
                                    wobj[0], {vf(-0.2f), vf(0.5f), vf(0.3f)},
                                    &v, &e);
                c.check(ran && w0->bump == 60.0f && w0->rebound == 20.0f &&
                            w0->radius == 0.4f &&
                            std::fabs(w0->free_len - 0.9f) < 1e-6f &&
                            w0->half_width == 0.25f && w0->roll_res == 0.03f &&
                            w0->damage_visual == 5.0f &&
                            std::fabs(w0->cpatch_halfwidth - 0.2f) < 1e-6f &&
                            std::fabs(w0->cpatch_sin - 0.0958851f) < 1e-5f &&
                            w0->cpatch_offset == 0.3f,
                        "car: damping/width/cpatch/bearing setters");
                w0->damage_visual = 27.0f;  // undo setBearing before the damage test
            }

            // Pose: getPos inverts the template offset; instant centres shift.
            {
                Object *p123 = make_pt("java.lang.Vector3", 1.0f, 2.0f, 3.0f,
                                       "x", "y", "z");
                Value v;
                std::string e;
                bool ran = p123 && call_instance(vm, *wheel_unit, "setPos",
                                                 "(Ljava.lang.Vector3;)V",
                                                 wobj[0], {Value::make_obj(p123)},
                                                 &v, &e);
                c.check(ran && std::fabs(w0->hub[0] - 0.32f) < 1e-5f &&
                            std::fabs(w0->hub[1] - 1.818f) < 1e-5f &&
                            std::fabs(w0->hub[2] - 1.7f) < 1e-5f &&
                            std::fabs(w0->ic_h[0] - 1.0f) < 1e-5f &&
                            std::fabs(w0->ic_l[2] - 3.0f) < 1e-5f,
                        "car: setPos writes hub and shifts instant centres");
                float pp[3];
                c.check(pos_of(wobj[0], pp) &&
                            std::fabs(pp[0] + 0.68f) < 1e-5f &&
                            std::fabs(pp[1] - 2.0f) < 1e-5f &&
                            std::fabs(pp[2] + 1.3f) < 1e-5f,
                        "car: getPos after setPos keeps the template x/z");

                Object *yo = make_pt("java.lang.Ypr", 0.1f, 0.2f, 0.3f, "y", "p",
                                     "r");
                ran = yo && call_instance(vm, *wheel_unit, "setYpr",
                                          "(Ljava.lang.Ypr;)V", wobj[0],
                                          {Value::make_obj(yo)}, &v, &e);
                float yp[3];
                c.check(ran && w0->pose.m[0] == w0->rot.m[0] &&
                            ypr_of(wobj[0], yp) &&
                            std::fabs(yp[0] - 0.1f) < 1e-5f &&
                            std::fabs(yp[1] - 0.2f) < 1e-5f &&
                            std::fabs(yp[2] - 0.3f) < 1e-5f,
                        "car: setYpr/getYpr round-trip and pose copy");
                ran = call_instance(vm, *wheel_unit, "setYpr",
                                    "(Ljava.lang.Ypr;)V", wobj[0],
                                    {Value::make_null()}, &v, &e);
                c.check(ran && ypr_of(wobj[0], yp) &&
                            std::fabs(yp[0]) < 1e-6f &&
                            std::fabs(yp[1]) < 1e-6f &&
                            std::fabs(yp[2]) < 1e-6f,
                        "car: setYpr(null) restores the template Ypr");
            }

            // Damage strings: long form sets the override, short form resets.
            {
                cstr(chassis_unit, car_obj, "getWheelDamage",
                     "(I)Ljava.lang.String;", {vi(0)}, "",
                     "car: getWheelDamage without override");
                const std::string dmg =
                    "1.000,2.000,3.000,0.000,0.000,0.000,7.000,1.000,1.000,9.000";
                Value v;
                std::string e;
                bool ran = call_instance(vm, *chassis_unit, "setWheelDamage",
                                         "(ILjava.lang.String;)V", car_obj,
                                         {vi(0), Value::make_str(dmg)}, &v, &e);
                cstr(chassis_unit, car_obj, "getWheelDamage",
                     "(I)Ljava.lang.String;", {vi(0)}, dmg,
                     "car: setWheelDamage round-trip");
                c.check(ran && w0->hub[0] == 1.0f && w0->hub[1] == 2.0f &&
                            w0->hub[2] == 3.0f && w0->wobble == 7.0f &&
                            w0->wobble_phase == 9.0f &&
                            w0->damage_visual == 1827.0f &&
                            (w0->flags & 0x20u),
                        "car: setWheelDamage writes hub/wobble/visual");
                ran = call_instance(vm, *chassis_unit, "setWheelDamage",
                                    "(ILjava.lang.String;)V", car_obj,
                                    {vi(0), Value::make_str("")}, &v, &e);
                cstr(chassis_unit, car_obj, "getWheelDamage",
                     "(I)Ljava.lang.String;", {vi(0)}, "",
                     "car: short setWheelDamage clears the override");
                c.check(ran && !(w0->flags & 0x20u) &&
                            std::fabs(w0->hub[0] + 0.68f) < 1e-6f &&
                            w0->damage_visual == 27.0f && w0->wobble == 0.0f &&
                            w0->brake_eff == w0->brake &&
                            w0->drive_eff == w0->drive,
                        "car: short form restores template pose/effects");
            }

            // Arm, hub and instant-centre geometry on the rear wheel.
            {
                float hub0[3] = {w2->hub[0], w2->hub[1], w2->hub[2]};
                Value v;
                std::string e;
                bool ran = call_instance(
                    vm, *wheel_unit, "setArm", "(FFFFFFF)V", wobj[2],
                    {vf(1.5f), vf(0.0f), vf(2.0f), vf(0.0f), vf(1.0f),
                     vf(2.0f), vf(3.0f)}, &v, &e);
                c.check(ran && w2->arm_len == 1.5f && w2->arm_axis[1] == 1.0f &&
                            std::fabs(w2->arm_pivot[0] - (hub0[0] + 1.0f)) <
                                1e-5f &&
                            std::fabs(w2->arm_pivot[1] -
                                      (hub0[1] + 2.0f - 0.36f)) < 1e-5f,
                        "car: setArm normalises the axis and offsets the pivot");
                ran = call_instance(
                    vm, *wheel_unit, "setHub", "(FFFFFFFFFF)V", wobj[2],
                    {vf(0.0f), vf(0.1f), vf(0.2f), vf(0.3f), vf(0.0f),
                     vf(2.0f), vf(0.0f), vf(0.4f), vf(0.5f), vf(0.6f)}, &v, &e);
                c.check(ran && w2->hub_len == 0.0f && w2->hub_p1[0] == 0.1f &&
                            w2->hub_p2[0] == 0.0f && w2->hub_p2[1] == 1.0f &&
                            std::fabs(w2->hub_pc[0] - (hub0[0] + 0.3f)) <
                                1e-5f &&
                            std::fabs(w2->hub_pc[1] - (hub0[1] - 0.06f)) <
                                1e-5f,
                        "car: setHub normalises p2 and offsets pc");
                ran = call_instance(vm, *wheel_unit, "setInstantCenter",
                                    "(FFFFFF)V", wobj[2],
                                    {vf(1.0f), vf(2.0f), vf(3.0f), vf(4.0f),
                                     vf(5.0f), vf(6.0f)}, &v, &e);
                c.check(ran && std::fabs(w2->ic_h[1] - (hub0[1] + 2.0f)) <
                                    1e-5f &&
                            std::fabs(w2->ic_l[2] - (hub0[2] + 6.0f)) < 1e-5f,
                        "car: setInstantCenter adds hub-relative vectors");
                // setOppWheel/getSlotIndex/isSlotDisabled/getBrake/getHBrake are
                // in the exe native table but not declared by the shipped Axis
                // class shell, so they cannot be reached through this VM.
            }

            // Chassis engine block.
            cint(chassis_unit, car_obj, "getMaterialIndex", "()I", {}, -1,
                 "car: getMaterialIndex default");
            cfloat(chassis_unit, car_obj, "getMaxSteer", "()F", {}, 0.7f, 1e-6f,
                   "car: getMaxSteer default");
            cint(chassis_unit, car_obj, "getShifting", "()I", {}, 0,
                 "car: getShifting default");
            cfloat(chassis_unit, car_obj, "getEngineTemp", "()F", {}, 20.0f,
                   1e-6f, "car: getEngineTemp default");
            {
                Value v;
                std::string e;
                bool ran = call_instance(vm, *chassis_unit, "setMaxSteer",
                                         "(F)V", car_obj, {vf(0.6f)}, &v, &e);
                cfloat(chassis_unit, car_obj, "getMaxSteer", "()F", {}, 0.6f,
                       1e-6f, "car: setMaxSteer");
                c.check(ran && car->max_steer == 0.6f, "car: max_steer stored");
                ran = call_instance(vm, *chassis_unit, "setAIparam_float",
                                    "(IF)V", car_obj, {vi(3), vf(0.4f)}, &v,
                                    &e);
                cfloat(chassis_unit, car_obj, "getAIparam_float", "(I)F",
                       {vi(3)}, 0.4f, 1e-6f, "car: AI steer parameter");
                cfloat(chassis_unit, car_obj, "getAIparam_float", "(I)F",
                       {vi(9)}, 0.0f, 0.0f, "car: invalid AI float id");
                ran = call_instance(vm, *chassis_unit, "setAIparam_int",
                                    "(II)V", car_obj, {vi(6), vi(2)}, &v, &e) &&
                      call_instance(vm, *chassis_unit, "setAIparam_int",
                                    "(II)V", car_obj, {vi(8), vi(5)}, &v, &e);
                c.check(ran && car->gear == 2 && car->horn_id == 5,
                        "car: setAIparam_int writes gear/horn");
                cint(chassis_unit, car_obj, "getAIparam_int", "(I)I", {vi(6)},
                     2, "car: getAIparam_int gear");
                cint(chassis_unit, car_obj, "getAIparam_int", "(I)I", {vi(3)},
                     3, "car: invalid AI int id returns the id");
            }

            car->torque_rpm = {1000.0f, 2000.0f};
            car->torque_nm = {100.0f, 200.0f};
            car->nitro_rpm = {1000.0f, 2000.0f};
            car->nitro_nm = {50.0f, 100.0f};
            cfloat(chassis_unit, car_obj, "getTorque", "(FF)F",
                   {vf(1500.0f), vf(0.0f)}, 150.0f, 1e-4f,
                   "car: getTorque interpolates the curve");
            cfloat(chassis_unit, car_obj, "getTorque", "(FF)F",
                   {vf(1500.0f), vf(1.0f)}, 75.0f, 1e-4f,
                   "car: getTorque blends the nitro curve");
            cfloat(chassis_unit, car_obj, "getTorque", "(FF)F",
                   {vf(500.0f), vf(0.0f)}, 0.0f, 0.0f,
                   "car: getTorque below the first rpm");
            cfloat(chassis_unit, car_obj, "getTorque", "(FF)F",
                   {vf(2000.0f), vf(0.0f)}, 0.0f, 0.0f,
                   "car: getTorque at the last rpm is 0 (exe loop)");
            {
                Value v;
                std::string e;
                bool ran = call_instance(vm, *chassis_unit, "setTorque", "(F)V",
                                         car_obj, {vf(200.0f)}, &v, &e);
                c.check(ran && car->torque_base == 200.0f &&
                            car->nitro_torque_base == 200.0f &&
                            car->torque_health == 200.0f &&
                            car->nitro_health == 200.0f,
                        "car: setTorque writes base and health");
                cfloat(chassis_unit, car_obj, "getTorque", "(FF)F",
                       {vf(1500.0f), vf(0.0f)}, 30000.0f, 0.01f,
                       "car: torque scales with health");
                car->engine_damage = 0.5f;
                ran = call_instance(vm, *chassis_unit, "setTorque", "(F)V",
                                    car_obj, {vf(100.0f)}, &v, &e);
                c.check(ran && std::fabs(car->torque_health - 85.0f) < 1e-5f &&
                            std::fabs(car->nitro_health - 85.0f) < 1e-5f,
                        "car: setTorque applies the engine damage factor");
                cfloat(chassis_unit, car_obj, "getTorque", "(FF)F",
                       {vf(1500.0f), vf(0.0f)}, 12750.0f, 0.01f,
                       "car: damaged engine torque");
                cfloat(chassis_unit, car_obj, "getTorque", "(FF)F",
                       {vf(1500.0f), vf(1.0f)}, 6375.0f, 0.01f,
                       "car: damaged engine nitro torque");
                car->engine_damage = 0.0f;
            }

            car->rpm_smoothed = 100.0f;
            cfloat(chassis_unit, car_obj, "getRPM", "()F", {}, 954.9296f, 1e-3f,
                   "car: getRPM uses 9.549296");
            {
                Value v;
                std::string e;
                bool ran = call_instance(vm, *chassis_unit, "setRPM", "(F)V",
                                         car_obj, {vf(30.0f)}, &v, &e);
                c.check(ran && car->rpm_raw == 30.0f, "car: setRPM stores rad/s");
                ran = call_instance(vm, *chassis_unit, "setCooling", "(FFF)V",
                                    car_obj, {vf(-1.0f), vf(-1.0f), vf(0.0f)},
                                    &v, &e);
                c.check(ran && car->cooling_low == 10.0f &&
                            car->cooling_air == 50.0f &&
                            car->cooling_speed == 0.0f,
                        "car: setCooling clamps negatives");
                ran = call_instance(vm, *chassis_unit, "setCooling", "(FFF)V",
                                    car_obj, {vf(5.0f), vf(6.0f), vf(7.0f)},
                                    &v, &e);
                c.check(ran && car->cooling_low == 5.0f &&
                            car->cooling_air == 6.0f &&
                            car->cooling_speed == 7.0f,
                        "car: setCooling stores values");
                ran = call_instance(vm, *chassis_unit, "setMileage", "(F)V",
                                    car_obj, {vf(123.5f)}, &v, &e);
                cfloat(chassis_unit, car_obj, "getMileage", "()F", {}, 123.5f,
                       1e-6f, "car: mileage round-trip");
                ran = call_instance(vm, *chassis_unit, "setSfxExhaustMinVol",
                                    "(F)V", car_obj, {vf(1.5f)}, &v, &e) &&
                      call_instance(vm, *chassis_unit, "setSteerWheelRadius",
                                    "(F)V", car_obj, {vf(0.33f)}, &v, &e) &&
                      call_instance(vm, *chassis_unit, "setSteerWheel", "(FF)V",
                                    car_obj, {vf(0.3f), vf(0.2f)}, &v, &e) &&
                      call_instance(vm, *chassis_unit, "setAckermann", "(F)V",
                                    car_obj, {vf(0.4f)}, &v, &e);
                c.check(ran && car->exhaust_min_vol == 1.5f &&
                            car->steer_wheel_radius == 0.3f &&
                            car->steer_wheel_angle == 0.2f &&
                            car->ackermann == 0.4f,
                        "car: exhaust/steer/ackermann setters");
                car->nitro = 12.5f;
                cfloat(chassis_unit, car_obj, "getNitro", "()F", {}, 12.5f,
                       1e-6f, "car: getNitro");
                car->flags = 0x800u;
                cint(chassis_unit, car_obj, "getNitroing", "()I", {}, 1,
                     "car: getNitroing reads flag 0x800");
                car->flags = 0;
            }

            // Subsystem-dependent natives stay explicit stubs with their name.
            {
                Value v;
                std::string e;
                bool ran = call_instance(vm, *chassis_unit, "getMass", "()F",
                                         car_obj, {}, &v, &e);
                c.check(!ran && e.find("not implemented (stub)") !=
                                     std::string::npos &&
                            e.find("Chassis.getMass") != std::string::npos,
                        "car: Chassis.getMass remains an explicit stub");
                ran = call_instance(vm, *chassis_unit, "getOSD", "(I)F", car_obj,
                                    {vi(0)}, &v, &e);
                c.check(!ran && e.find("not implemented (stub)") !=
                                     std::string::npos,
                        "car: Chassis.getOSD remains an explicit stub");
            }
        } else {
            c.check(false, "car: car payload and wheel array available");
        }

        // ---- Part payload from FL_door.cfg ----
        Runtime::Resource *part_inst =
            rt.instantiate(fl, fl, "test_part", "", true, &err);
        Object *part_obj = part_inst ? part_inst->script : nullptr;
        c.check(part_inst && part_obj &&
                    part_inst->script_class == "java.game.cars.Axis_FL_door",
                "car: instantiate part 0xD2 as Axis_FL_door");
        game::PartData *part = part_inst ? game::part_data(rt, part_inst) : nullptr;
        if (part_obj && part) {
            rt.bind(rt.attach(part_obj), part_inst);
            cint(part_unit, part_obj, "getSlots", "()I", {}, 3,
                 "car: Part.getSlots");
            cint(part_unit, part_obj, "getSlotID", "(I)I", {vi(0)}, 4,
                 "car: slot 0 id 4");
            cint(part_unit, part_obj, "getSlotID", "(I)I", {vi(1)}, 6,
                 "car: slot 1 id 6");
            cint(part_unit, part_obj, "getSlotID", "(I)I", {vi(2)}, 11,
                 "car: slot 2 id 11");
            cint(part_unit, part_obj, "getSlotID", "(I)I", {vi(-1)}, 0,
                 "car: getSlotID(-1) without an installed slot");
            cint(part_unit, part_obj, "getSlotID", "(I)I", {vi(3)}, 0,
                 "car: getSlotID out of range");
            cint(part_unit, part_obj, "slotIDOnSlot", "(I)I", {vi(4)}, 0,
                 "car: slotIDOnSlot without an attachment");
            cint(part_unit, part_obj, "getTexture", "()I", {}, 0x00020025,
                 "car: texture from FL_door.cfg");
            cint(part_unit, part_obj, "getMesh", "()I", {}, 0x5A,
                 "car: mesh from FL_door.cfg");
            cint(part_unit, part_obj, "getRenderType", "()I", {}, 0x59,
                 "car: render type from FL_door.cfg");
            cfloat(part_unit, part_obj, "getWear", "()F", {}, 1.0f, 1e-6f,
                   "car: getWear default");
            cfloat(part_unit, part_obj, "getTear", "()F", {}, 1.0f, 1e-6f,
                   "car: getTear default");
            cint(part_unit, part_obj, "getCar", "()I", {}, 0,
                 "car: getCar default");
            cint(part_unit, part_obj, "getWheelID", "()I", {}, -1,
                 "car: getWheelID out of range");
            {
                Value v;
                std::string e;
                bool ran = call_instance(vm, *part_unit, "partOnSlot",
                                         "(I)Ljava.game.parts.Part;", part_obj,
                                         {vi(-1)}, &v, &e);
                c.check(ran && v.kind == Value::Obj && v.obj == part_obj,
                        "car: partOnSlot(-1) returns the receiver");
                ran = call_instance(vm, *part_unit, "partOnSlot",
                                    "(I)Ljava.game.parts.Part;", part_obj,
                                    {vi(4)}, &v, &e);
                c.check(ran && v.kind == Value::Null,
                        "car: partOnSlot(empty) is null");
                ran = call_instance(vm, *part_unit, "getCarRef",
                                    "()Ljava.game.parts.Part;", part_obj, {},
                                    &v, &e);
                c.check(ran && v.kind == Value::Null,
                        "car: getCarRef without a car is null");
                ran = call_instance(vm, *part_unit, "getSlotDamage",
                                    "(I)Ljava.lang.String;", part_obj, {vi(0)},
                                    &v, &e);
                c.check(ran && v.kind == Value::Str && v.s.empty(),
                        "car: getSlotDamage without override is empty");
                ran = call_instance(vm, *part_unit, "getMass", "()F", part_obj,
                                    {}, &v, &e);
                c.check(!ran && e.find("not implemented (stub)") !=
                                     std::string::npos,
                        "car: Part.getMass remains an explicit stub");
            }

            // Wear/tear and model bookkeeping.
            {
                Value v;
                std::string e;
                bool ran = call_instance(vm, *part_unit, "setWear", "(F)F",
                                         part_obj, {vf(0.8f)}, &v, &e);
                c.check(ran && v.kind == Value::Float &&
                            std::fabs(v.f - 0.8f) < 1e-6f &&
                            std::fabs(part->wear - 0.2f) < 1e-6f,
                        "car: setWear stores 1-v and returns v");
                cfloat(part_unit, part_obj, "getWear", "()F", {}, 0.8f, 1e-6f,
                       "car: getWear after setWear");
                ran = call_instance(vm, *part_unit, "setTear", "(F)F", part_obj,
                                    {vf(0.5f)}, &v, &e);
                c.check(ran && v.kind == Value::Float && v.f == 0.5f &&
                            part->tear == 0.5f,
                        "car: setTear stores and returns v");
                ran = call_instance(vm, *part_unit, "setMaxWear", "(F)V",
                                    part_obj, {vf(0.25f)}, &v, &e);
                c.check(ran && std::fabs(part->max_wear_inv - 4.0f) < 1e-6f,
                        "car: setMaxWear stores 1/v");
                ran = call_instance(vm, *part_unit, "setTexture", "(I)I",
                                    part_obj, {vi(7)}, &v, &e);
                c.check(ran && v.kind == Value::Int && v.i == 1 &&
                            part->texture == 0x00020025,
                        "car: setTexture returns 1 without changing the field");
                ran = call_instance(vm, *part_unit, "setMesh", "(I)I", part_obj,
                                    {vi(7)}, &v, &e);
                c.check(ran && v.kind == Value::Int && v.i == 1 &&
                            part->mesh == 0x5A,
                        "car: setMesh returns 1 without changing the field");
                ran = call_instance(vm, *part_unit, "setRenderType", "(I)I",
                                    part_obj, {vi(7)}, &v, &e);
                c.check(ran && v.kind == Value::Int && v.i == 1 &&
                            part->render_type == 0x59,
                        "car: setRenderType returns 1 without changing the field");
                // Hidden native: the shipped class marks it non-native, so call
                // the registered handler directly (still the native path).
                Value recv = Value::make_obj(part_obj);
                ran = call_native(vm, *part_unit, "setSfxLoopParams", "(FF)I",
                                  &recv, {vf(0.5f), vf(0.25f)}, &v, &e);
                c.check(ran && v.kind == Value::Int && v.i == 1 &&
                            part->sfx_loop[0] == 0.5f &&
                            part->sfx_loop[1] == 0.25f,
                        "car: setSfxLoopParams writes and returns 1");
                part->wheel_id = 105;
                cint(part_unit, part_obj, "getWheelID", "()I", {}, 4,
                     "car: getWheelID ((105-101)%10)");
                part->wheel_id = 0;
            }

            // Slot disable/pose/damage.
            {
                Value v;
                std::string e;
                bool ran = call_instance(vm, *part_unit, "disableSlot", "(II)V",
                                         part_obj, {vi(4), vi(1)}, &v, &e);
                c.check(ran && (part->slots[0].flags & 0x10u) &&
                            !(part->slots[0].flags & 0x40u),
                        "car: disableSlot(1) sets 0x10");
                ran = call_instance(vm, *part_unit, "disableSlot", "(II)V",
                                    part_obj, {vi(4), vi(2)}, &v, &e);
                c.check(ran && (part->slots[0].flags & 0x50u) == 0x50u,
                        "car: disableSlot(2) sets 0x40|0x10");
                ran = call_instance(vm, *part_unit, "disableSlot", "(II)V",
                                    part_obj, {vi(4), vi(0)}, &v, &e);
                c.check(ran && !(part->slots[0].flags & 0x50u),
                        "car: disableSlot(0) clears the flags");

                Object *dp = make_pt("java.lang.Vector3", 0.1f, 0.2f, 0.3f,
                                     "x", "y", "z");
                Object *dy = make_pt("java.lang.Ypr", 0.1f, 0.2f, 0.3f, "y",
                                     "p", "r");
                ran = dp && dy && call_instance(
                    vm, *part_unit, "setSlotPos",
                    "(ILjava.lang.Vector3;Ljava.lang.Ypr;)V", part_obj,
                    {vi(4), Value::make_obj(dp), Value::make_obj(dy)}, &v, &e);
                c.check(ran && (part->slots[0].flags & 0x20u) &&
                            std::fabs(part->slots[0].pos[0] - 0.006f) < 1e-5f &&
                            std::fabs(part->slots[0].pos[1] - 0.19f) < 1e-5f &&
                            std::fabs(part->slots[0].pos[2] + 0.245f) < 1e-5f,
                        "car: setSlotPos applies position deltas");
                ran = call_instance(vm, *part_unit, "setSlotPos",
                                    "(ILjava.lang.Vector3;Ljava.lang.Ypr;)V",
                                    part_obj,
                                    {vi(4), Value::make_null(),
                                     Value::make_null()}, &v, &e);
                c.check(ran && !(part->slots[0].flags & 0x20u) &&
                            std::fabs(part->slots[0].pos[0] + 0.094f) < 1e-6f &&
                            std::fabs(part->slots[0].pos[1] + 0.010f) < 1e-6f &&
                            std::fabs(part->slots[0].pos[2] + 0.545f) < 1e-6f,
                        "car: setSlotPos(null,null) restores the type pose");
                const std::string sd = "1.000,2.000,3.000,0.000,0.000,0.000";
                ran = call_instance(vm, *part_unit, "setSlotDamage",
                                    "(ILjava.lang.String;)V", part_obj,
                                    {vi(0), Value::make_str(sd)}, &v, &e);
                cstr(part_unit, part_obj, "getSlotDamage",
                     "(I)Ljava.lang.String;", {vi(0)}, sd,
                     "car: setSlotDamage round-trip");
                c.check(ran && part->slots[0].pos[0] == 1.0f &&
                            part->slots[0].pos[1] == 2.0f &&
                            part->slots[0].pos[2] == 3.0f &&
                            (part->slots[0].flags & 0x20u),
                        "car: setSlotDamage writes the pose");
                ran = call_instance(vm, *part_unit, "setSlotDamage",
                                    "(ILjava.lang.String;)V", part_obj,
                                    {vi(0), Value::make_str("")}, &v, &e);
                cstr(part_unit, part_obj, "getSlotDamage",
                     "(I)Ljava.lang.String;", {vi(0)}, "",
                     "car: short setSlotDamage clears the override");
                c.check(ran && !(part->slots[0].flags & 0x20u) &&
                            part->slots[0].pos[0] == 1.0f,
                        "car: short form keeps the last pose (exe semantics)");
                ran = call_instance(vm, *part_unit, "setSlotPos",
                                    "(ILjava.lang.Vector3;Ljava.lang.Ypr;)V",
                                    part_obj,
                                    {vi(4), Value::make_null(),
                                     Value::make_null()}, &v, &e);
                c.check(ran && part->slots[0].pos[0] == -0.094f,
                        "car: setSlotPos restores the type pose again");
            }
        } else {
            c.check(false, "car: part payload from FL_door.cfg available");
        }
    }

    // ---- subsystem natives stay explicit stubs ----
    ran = call_instance(vm, *gr_unit, "getInfo", "(II)I", gr, {vi(0), vi(0)}, &out,
                        &err);
    c.check(!ran && err.find("not implemented (stub)") != std::string::npos,
            "runtime: engine subsystem native remains an explicit stub");

    // 0xD2 is still held by ref2 and gr; all other wrappers moved to instances.
    c.check(fl->refs == 2, "runtime: final 0xD2 refcount 2");
    c.check(fl->parent && fl->parent->refs == 1, "runtime: final >Stock refcount 1");

    rt.deactivate();
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
    vm_synthetic_tests(c);
    vm_service_tests(c);
    std::string game = game_root(argc, argv);
    if (!game.empty()) runtime_tests(game, c);
    else std::printf("game runtime: skipped (no game root in the sample paths)\n");
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
