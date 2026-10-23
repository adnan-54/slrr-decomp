// Script VM implementation. See vm.h for the model and notes/05 for semantics.
#include "script/vm.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <utility>

#include "script/natives.h"

// ---------------------------------------------------------------------------
// Values

const char *Value::type() const
{
    switch (kind) {
    case Int: return "I";
    case Float: return "F";
    case Str: return "Ljava.lang.String;";
    case Obj: return "Lobject;";
    case Array: return "array";
    case Type: return s.c_str();
    case Resource: return "resource";
    case Ref: return "ref";
    default: return "null";
    }
}

Value Value::make_int(int32_t v)
{
    Value r;
    r.kind = Int;
    r.i = v;
    return r;
}

Value Value::make_float(float v)
{
    Value r;
    r.kind = Float;
    r.f = v;
    return r;
}

Value Value::make_str(const std::string &v)
{
    Value r;
    r.kind = Str;
    r.s = v;
    return r;
}

Value Value::make_obj(Object *o)
{
    Value r;
    r.kind = Obj;
    r.obj = o;
    return r;
}

Value Value::make_array(struct Array *a)
{
    Value r;
    r.kind = Array;
    r.arr = a;
    return r;
}

Value Value::make_type(const ClassUnit *u, const std::string &desc)
{
    Value r;
    r.kind = Type;
    r.type_unit = u;
    r.s = desc;
    return r;
}

Value Value::make_resource(const std::string &name, int32_t id)
{
    Value r;
    r.kind = Resource;
    r.s = name;
    r.i = id;
    return r;
}

Value Value::make_null()
{
    return Value();
}

Value Value::make_ref(const struct Ref &r)
{
    Value v;
    v.kind = Ref;
    v.ref = r;
    return v;
}

static float float_from_bits(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static bool is_truthy(const Value &v)
{
    switch (v.kind) {
    case Value::Null: return false;
    case Value::Int: return v.i != 0;
    case Value::Float: return v.f != 0.0f;
    case Value::Obj: return v.obj != nullptr;
    case Value::Array: return v.arr != nullptr;
    case Value::Str: return true;
    case Value::Type: return true;
    case Value::Resource: return true;
    default: return false;
    }
}

static Value default_for_desc(const std::string &desc)
{
    if (desc == "I") return Value::make_int(0);
    if (desc == "F") return Value::make_float(0.0f);
    return Value::make_null();
}

static std::string class_name_from_desc(const std::string &desc)
{
    if (desc.size() >= 2 && desc[0] == 'L' && desc[desc.size() - 1] == ';')
        return desc.substr(1, desc.size() - 2);
    return std::string();
}

// shortcut: float formatting is "%g"; the exact JVM::stringConversion format
// needs exe probing, upgrade when script output must match the game byte-for-byte.
static bool stringify(const Value &v, std::string *out, std::string *err)
{
    switch (v.kind) {
    case Value::Str:
        *out = v.s;
        return true;
    case Value::Int:
        *out = std::to_string(v.i);
        return true;
    case Value::Float: {
        char buf[64];
        snprintf(buf, sizeof buf, "%g", (double)v.f);
        *out = buf;
        return true;
    }
    case Value::Null:
        *out = std::string(); // notes/05: null concat prints nothing
        return true;
    default:
        *err = std::string("unsupported type (") + v.type() + ") in string concatenation";
        return false;
    }
}

// JVM::numericPromotion: int mixed with float becomes float; anything else fails.
static bool promote(Value *a, Value *b, std::string *err)
{
    if (a->kind == Value::Float && b->kind == Value::Int) {
        b->f = (float)b->i;
        b->kind = Value::Float;
    } else if (a->kind == Value::Int && b->kind == Value::Float) {
        a->f = (float)a->i;
        a->kind = Value::Float;
    }
    if ((a->kind == Value::Int && b->kind == Value::Int) ||
        (a->kind == Value::Float && b->kind == Value::Float))
        return true;
    *err = std::string("cannot promote ") + a->type() + " to " + b->type();
    return false;
}

static bool values_equal(const Value &a, const Value &b)
{
    if (a.kind == Value::Null || b.kind == Value::Null)
        return a.kind == Value::Null && b.kind == Value::Null;
    bool anum = a.kind == Value::Int || a.kind == Value::Float;
    bool bnum = b.kind == Value::Int || b.kind == Value::Float;
    if (anum && bnum) {
        if (a.kind == Value::Int && b.kind == Value::Int) return a.i == b.i;
        double x = a.kind == Value::Int ? (double)a.i : (double)a.f;
        double y = b.kind == Value::Int ? (double)b.i : (double)b.f;
        return x == y;
    }
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case Value::Str: return a.s == b.s;
    case Value::Obj: return a.obj == b.obj;
    case Value::Array: return a.arr == b.arr;
    case Value::Type: return a.s == b.s;
    case Value::Resource: return a.s == b.s && a.i == b.i;
    default: return false;
    }
}

// Arithmetic, bitwise and shift operators. `op` is the FORMAT.md node kind:
// 3 |, 4 ^, 5 &, 13 >>, 14 <<, 15 -, 16 +, 17 %, 18 /, 19 *.
static bool apply_binary(int op, Value a, Value b, Value *out, std::string *err)
{
    if (op == 0x10 && (a.kind == Value::Str || b.kind == Value::Str)) {
        std::string sa, sb;
        if (!stringify(a, &sa, err) || !stringify(b, &sb, err)) return false;
        *out = Value::make_str(sa + sb);
        return true;
    }
    if (op == 0x03 || op == 0x04 || op == 0x05 || op == 0x0d || op == 0x0e) {
        if (a.kind != Value::Int || b.kind != Value::Int) {
            *err = "non-integral operands";
            return false;
        }
        uint32_t shift = (uint32_t)b.i & 31u;
        switch (op) {
        case 0x03: a.i |= b.i; break;
        case 0x04: a.i ^= b.i; break;
        case 0x05: a.i &= b.i; break;
        case 0x0d: a.i = a.i >> shift; break; // arithmetic shift
        case 0x0e: a.i = (int32_t)((uint32_t)a.i << shift); break;
        }
        *out = a;
        return true;
    }
    if (!promote(&a, &b, err)) return false;
    if (a.kind == Value::Int) {
        switch (op) {
        case 0x0f: a.i -= b.i; break;
        case 0x10: a.i += b.i; break;
        case 0x11:
            if (b.i == 0) { *err = "division by zero"; return false; }
            a.i %= b.i;
            break;
        case 0x12:
            if (b.i == 0) { *err = "division by zero"; return false; }
            a.i /= b.i;
            break;
        case 0x13: a.i *= b.i; break;
        default: *err = "unsupported integral operator"; return false;
        }
    } else {
        switch (op) {
        case 0x0f: a.f -= b.f; break;
        case 0x10: a.f += b.f; break;
        case 0x11: a.f = fmodf(a.f, b.f); break;
        case 0x12: a.f /= b.f; break;
        case 0x13: a.f *= b.f; break;
        default: *err = "unsupported float operator"; return false;
        }
    }
    *out = a;
    return true;
}

// ---------------------------------------------------------------------------
// Classes, objects and arrays

struct FieldRef {
    const FieldInfo *info = nullptr;
    const ClassUnit *unit = nullptr;
};

struct ClassState {
    const ClassUnit *unit = nullptr;
    std::string name;
    std::string super_name;
    ClassState *super = nullptr;
    std::vector<FieldRef> own_inst, own_statics;
    std::vector<FieldRef> ifields; // flattened super-first layout
    std::vector<Value> statics;
    std::vector<uint8_t> sinit, sbusy;
    size_t inst_base = 0;
    bool built = false, building = false;
};

static const char *field_name(const FieldRef &fr)
{
    return fr.unit->str(fr.info->name);
}

static const char *field_desc(const FieldRef &fr)
{
    return fr.unit->str(fr.info->type);
}

static bool parse_params(const std::string &sig, std::vector<char> *out, int *arity)
{
    out->clear();
    if (arity) *arity = 0;
    if (sig.size() < 2 || sig[0] != '(') return false;
    size_t i = 1;
    while (i < sig.size() && sig[i] != ')') {
        char c = sig[i];
        if (c == '[') {
            out->push_back('[');
            while (i < sig.size() && sig[i] == '[') i++;
            if (i >= sig.size()) return false;
            if (sig[i] == 'L') {
                size_t e = sig.find(';', i);
                if (e == std::string::npos) return false;
                i = e + 1;
            } else {
                i++;
            }
        } else if (c == 'L') {
            out->push_back('L');
            size_t e = sig.find(';', i);
            if (e == std::string::npos) return false;
            i = e + 1;
        } else {
            out->push_back(c);
            i++;
        }
    }
    if (i >= sig.size() || sig[i] != ')') return false;
    if (arity) *arity = (int)out->size();
    return true;
}

static std::string return_desc(const std::string &sig)
{
    size_t p = sig.rfind(')');
    if (p == std::string::npos || p + 1 >= sig.size()) return std::string();
    return sig.substr(p + 1);
}

static int arg_score(char param, const Value &v)
{
    switch (param) {
    case 'I': return v.kind == Value::Int ? 2 : 0;
    case 'F': return v.kind == Value::Float ? 2 : (v.kind == Value::Int ? 1 : 0);
    case 'L':
        return (v.kind == Value::Obj || v.kind == Value::Str || v.kind == Value::Array ||
                v.kind == Value::Type)
                   ? 2
                   : (v.kind == Value::Null ? 1 : 0);
    case '[': return v.kind == Value::Array ? 2 : (v.kind == Value::Null ? 1 : 0);
    default: return v.kind == Value::Int && param == 'Z' ? 2 : 0;
    }
}

static bool find_instance(ClassState *cs, const std::string *name, const std::string *desc,
                          size_t *idx)
{
    if (!cs) return false;
    for (size_t i = cs->ifields.size(); i-- > 0;) {
        const char *n = field_name(cs->ifields[i]);
        const char *d = field_desc(cs->ifields[i]);
        if (name && (!n || *name != n)) continue;
        if (desc && (!d || *desc != d)) continue;
        *idx = i;
        return true;
    }
    return false;
}

static bool find_static(ClassState *cs, const std::string *name, const std::string *desc,
                        ClassState **owner, size_t *slot)
{
    for (ClassState *s = cs; s; s = s->super) {
        for (size_t k = 0; k < s->own_statics.size(); k++) {
            const char *n = field_name(s->own_statics[k]);
            const char *d = field_desc(s->own_statics[k]);
            if (name && (!n || *name != n)) continue;
            if (desc && (!d || *desc != d)) continue;
            *owner = s;
            *slot = k;
            return true;
        }
    }
    return false;
}

static bool is_instance_of(const Value &v, const std::string &desc)
{
    if (v.kind == Value::Null) return false;
    if (!desc.empty() && desc[0] == '[') return v.kind == Value::Array;
    std::string want = class_name_from_desc(desc);
    if (want.empty()) return false;
    if (v.kind == Value::Str) return want == "java.lang.String";
    if (v.kind != Value::Obj || !v.obj || !v.obj->cls) return false;
    for (ClassState *s = v.obj->cls; s; s = s->super)
        if (s->name == want) return true;
    return false;
}

static void fill_frame(Frame *f, ClassState *decl, const MethodInfo *m, Object *self,
                       const std::vector<Value> &args)
{
    f->unit = decl->unit;
    f->state = decl;
    f->method = m;
    f->self = m->is_static ? nullptr : self;
    expand_body(decl->unit->bodies[(size_t)m->body], &f->code);

    const char *sg = decl->unit->str(m->sig);
    std::string sig = sg ? sg : std::string();
    std::vector<char> params;
    int arity = 0;
    parse_params(sig, &params, &arity);

    size_t base = m->is_static ? 0 : 1;
    size_t need = base + args.size();
    size_t nlocals = m->locals > 0 ? (size_t)m->locals : 0;
    if (nlocals < need) nlocals = need;
    f->locals.resize(nlocals);
    if (!m->is_static && self) f->locals[0] = Value::make_obj(self);
    for (size_t k = 0; k < args.size(); k++) {
        Value v = args[args.size() - 1 - k];
        if (k < params.size() && params[k] == 'F' && v.kind == Value::Int)
            v = Value::make_float((float)v.i);
        f->locals[base + k] = v;
    }
}

// ---------------------------------------------------------------------------
// Thread

bool Thread::fail(const std::string &msg)
{
    error_ = (line_ ? "line " + std::to_string(line_) + ": " : "") + msg;
    return false;
}

bool Thread::pop(Frame &f, Value *out)
{
    if (f.stack.empty()) return fail("operand stack underflow");
    *out = f.stack.back();
    f.stack.pop_back();
    return true;
}

void Thread::push(Frame &f, const Value &v)
{
    f.stack.push_back(v);
}

bool Thread::top_value(Frame &f, Value *out)
{
    if (f.stack.empty()) return fail("operand stack underflow");
    std::string e;
    if (!vm_->materialize(f.stack.back(), out, &e)) return fail(e);
    return true;
}

bool Thread::in_window(const Frame &f, size_t token) const
{
    return f.win_active && token >= f.win_start && token < f.win_end;
}

bool Thread::pop_args(Frame &f, int *argc, std::vector<Value> *args)
{
    Value v;
    if (!pop(f, &v)) return false;
    std::string e;
    if (!vm_->materialize(v, &v, &e)) return fail(e);
    if (v.kind != Value::Int || v.i < 0 || (size_t)v.i > f.stack.size())
        return fail("bad argument count");
    *argc = v.i;
    args->resize((size_t)*argc);
    for (int k = *argc - 1; k >= 0; k--) {
        Value a;
        if (!pop(f, &a)) return false;
        if (!vm_->materialize(a, &a, &e)) return fail(e);
        (*args)[(size_t)k] = a;
    }
    return true;
}

Thread::Status Thread::run_slice(size_t max_instructions)
{
    if (status_ == Done || status_ == Failed) return status_;
    size_t executed = 0;
    while (!frames_.empty()) {
        if (executed >= max_instructions) {
            status_ = Yielded;
            return status_;
        }
        executed++;
        if (!step()) {
            status_ = Failed;
            return status_;
        }
        if (stop_) {
            stop_ = false;
            status_ = frames_.empty() ? Done : Yielded;
            return status_;
        }
    }
    status_ = Done;
    return status_;
}

Thread::Status Thread::run()
{
    for (;;) {
        Status s = run_slice(256);
        if (s != Yielded) return s;
    }
}

bool Thread::finish_frame(const Value &result)
{
    Value out;
    std::string e;
    if (!vm_->materialize(result, &out, &e)) return fail(e);
    std::unique_ptr<Frame> done = std::move(frames_.back());
    frames_.pop_back();
    if (frames_.empty()) {
        result_ = out;
        status_ = Done;
        return true;
    }
    if (!done->suppress_result) frames_.back()->stack.push_back(out);
    return true;
}

bool Thread::start_call(ClassState *decl, const MethodInfo *m, Object *self,
                        const Value *receiver, std::vector<Value> *args, bool suppress)
{
    if (!decl || !m) return fail("unresolved call target");
    const char *nm = decl->unit->str(m->name);
    const char *sg = decl->unit->str(m->sig);
    std::string name = nm ? nm : std::string();
    std::string sig = sg ? sg : std::string();

    if (m->flags & 0x40) {
        Value selfv;
        const Value *recv = receiver;
        if (!m->is_static && !recv) {
            if (!self) return fail("instance method " + name + " without receiver");
            selfv = Value::make_obj(self);
            recv = &selfv;
        }
        Value r;
        if (!vm_->invoke_native(*decl->unit, *m, m->is_static ? nullptr : recv, *args,
                                &r, &error_))
            return fail(error_);
        if (!suppress && !frames_.empty()) frames_.back()->stack.push_back(r);
        return true;
    }
    if (!m->is_static && !self) return fail("instance method " + name + " without receiver");
    if (m->body < 0 || (size_t)m->body >= decl->unit->bodies.size())
        return fail("method " + name + " has no body");
    const Body &body = decl->unit->bodies[(size_t)m->body];
    if (body.instrs.empty()) {
        if (!suppress && !frames_.empty()) {
            Value r = default_for_desc(return_desc(sig));
            frames_.back()->stack.push_back(r);
        }
        return true;
    }
    auto nf = std::make_unique<Frame>();
    fill_frame(nf.get(), decl, m, self, *args);
    nf->suppress_result = suppress;
    frames_.push_back(std::move(nf));
    return true;
}

bool Thread::ctor_chain(Frame &f)
{
    if (!f.self) return fail("constructor chain without this");
    int argc = 0;
    std::vector<Value> args;
    if (!pop_args(f, &argc, &args)) return false;
    ClassState *start = nullptr;
    if (f.pending_ctor_super)
        start = f.state ? f.state->super : nullptr;
    else if (f.pending_ctor_this)
        start = f.state;
    else
        return fail("constructor chain without this/super marker");
    f.pending_ctor_this = f.pending_ctor_super = false;
    if (!start) return fail("constructor chain has no target class");
    std::string e;
    ClassState *decl = nullptr;
    const MethodInfo *m = vm_->find_method(start, "<init>", std::string(), argc, args, &decl, &e);
    if (!m) return fail(e);
    return start_call(decl, m, f.self, nullptr, &args, true);
}

bool Thread::finalize_new(Frame &f)
{
    int argc = 0;
    std::vector<Value> args;
    if (!pop_args(f, &argc, &args)) return false;
    Value tv;
    if (!pop(f, &tv)) return false;
    std::string e;
    if (!vm_->materialize(tv, &tv, &e)) return fail(e);
    if (tv.kind != Value::Type) return fail("new without type");
    std::string name = class_name_from_desc(tv.s);
    if (name.empty()) return fail("new with bad descriptor " + tv.s);
    ClassState *cs = vm_->get_class(name);
    if (!cs) return fail("new of unknown class " + name);
    Object *obj = vm_->new_object(cs, &e);
    if (!obj) return fail(e);
    push(f, Value::make_obj(obj));

    std::string ferr;
    ClassState *decl = nullptr;
    const MethodInfo *m = vm_->find_method(cs, "<init>", std::string(), argc, args, &decl, &ferr);
    if (!m) {
        if (argc == 0) return true; // no explicit constructor
        return fail(ferr);
    }
    return start_call(decl, m, obj, nullptr, &args, true);
}

bool Thread::node(Frame &f, int kind, bool keep)
{
    auto popv = [&](Value *v) -> bool {
        Value x;
        if (!pop(f, &x)) return false;
        std::string e;
        if (!vm_->materialize(x, v, &e)) return fail(e);
        return true;
    };

    switch (kind) {
    case 0x00: { // cond ? a : b, both branches already evaluated
        Value c, tv, fv;
        if (!popv(&fv) || !popv(&tv) || !popv(&c)) return false;
        push(f, is_truthy(c) ? tv : fv);
        return true;
    }
    case 0x01: // ||
    case 0x02: { // &&
        Value b, a;
        if (!popv(&b) || !popv(&a)) return false;
        bool r = kind == 0x01 ? (is_truthy(a) || is_truthy(b))
                              : (is_truthy(a) && is_truthy(b));
        push(f, Value::make_int(r ? 1 : 0));
        return true;
    }
    case 0x03: case 0x04: case 0x05:
    case 0x0d: case 0x0e:
    case 0x0f: case 0x10: case 0x11: case 0x12: case 0x13: {
        Value b, a;
        if (!popv(&b) || !popv(&a)) return false;
        Value out;
        std::string e;
        if (!apply_binary(kind, a, b, &out, &e)) return fail(e);
        push(f, out);
        return true;
    }
    case 0x06: case 0x07: case 0x08: case 0x09: case 0x0a: case 0x0b: { // comparisons
        Value b, a;
        if (!popv(&b) || !popv(&a)) return false;
        if (kind == 0x06 || kind == 0x07) {
            bool eq = values_equal(a, b);
            push(f, Value::make_int(kind == 0x06 ? (eq ? 0 : 1) : (eq ? 1 : 0)));
            return true;
        }
        std::string e;
        if (!promote(&a, &b, &e)) return fail(e);
        bool r;
        if (a.kind == Value::Int) {
            switch (kind) {
            case 0x08: r = a.i >= b.i; break;
            case 0x09: r = a.i <= b.i; break;
            case 0x0a: r = a.i > b.i; break;
            default: r = a.i < b.i; break;
            }
        } else {
            switch (kind) {
            case 0x08: r = a.f >= b.f; break;
            case 0x09: r = a.f <= b.f; break;
            case 0x0a: r = a.f > b.f; break;
            default: r = a.f < b.f; break;
            }
        }
        push(f, Value::make_int(r ? 1 : 0));
        return true;
    }
    case 0x14: // ~
    {
        Value a;
        if (!popv(&a)) return false;
        if (a.kind != Value::Int) return fail("non-integral operand (~)");
        a.i = ~a.i;
        push(f, a);
        return true;
    }
    case 0x15: { // !
        Value a;
        if (!popv(&a)) return false;
        push(f, Value::make_int(is_truthy(a) ? 0 : 1));
        return true;
    }
    case 0x16: { // unary -
        Value a;
        if (!popv(&a)) return false;
        if (a.kind == Value::Int) a.i = -a.i;
        else if (a.kind == Value::Float) a.f = -a.f;
        else return fail("unary '-' on non-number");
        push(f, a);
        return true;
    }
    case 0x17: case 0x18: case 0x19: case 0x1a: { // ++/--
        Value target;
        if (!pop(f, &target)) return false;
        if (target.kind != Value::Ref || target.ref.kind == Ref::None)
            return fail("++/-- target is not a variable");
        std::string e;
        Value cur;
        if (!vm_->materialize(target, &cur, &e)) return fail(e);
        Value old = cur;
        bool decrement = kind == 0x18 || kind == 0x1a;
        if (cur.kind == Value::Int) cur.i += decrement ? -1 : 1;
        else if (cur.kind == Value::Float) cur.f += decrement ? -1.0f : 1.0f;
        else return fail("++/-- on non-number");
        if (!vm_->store(target.ref, cur, &e)) return fail(e);
        if (keep) push(f, (kind == 0x17 || kind == 0x18) ? old : cur);
        return true;
    }
    case 0x1b: // call statement marker
        if (keep) return true;
        {
            Value discard;
            return pop(f, &discard);
        }
    case 0x1c: // literal/const wrapper
        return true;
    case 0x1e: f.pending_new = true; return true;
    case 0x1f: f.pending_new_array = true; return true;
    case 0x20: f.pending_instanceof = true; return true;
    case 0x21: f.pending_cast = true; return true;
    case 0x22: return ctor_chain(f);
    case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28:
    case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: { // assignments
        Value target;
        if (!pop(f, &target)) return false;
        if (target.kind != Value::Ref || target.ref.kind == Ref::None)
            return fail("assignment target is not a variable");
        std::string e;
        Value val;
        if (!pop(f, &val)) return false;
        if (!vm_->materialize(val, &val, &e)) return fail(e);
        if (kind == 0x23) { // plain =
            if (!vm_->store(target.ref, val, &e)) return fail(e);
            if (keep) push(f, val);
            return true;
        }
        int bop;
        switch (kind) {
        case 0x24: bop = 0x13; break; // *=
        case 0x25: bop = 0x12; break; // /=
        case 0x26: bop = 0x11; break; // %=
        case 0x27: bop = 0x10; break; // +=
        case 0x28: bop = 0x0f; break; // -=
        case 0x29: bop = 0x0e; break; // <<=
        case 0x2a: bop = 0x0d; break; // >>=
        case 0x2b: bop = 0x0c; break; // >>>= (rejected by the original runtime)
        case 0x2c: bop = 0x05; break; // &=
        case 0x2d: bop = 0x04; break; // ^=
        default: bop = 0x03; break;   // |=
        }
        Value cur;
        if (!vm_->materialize(target, &cur, &e)) return fail(e);
        Value out;
        if (!apply_binary(bop, cur, val, &out, &e)) return fail(e);
        if (!vm_->store(target.ref, out, &e)) return fail(e);
        if (keep) push(f, out);
        return true;
    }
    default:
        return fail("node kind " + std::to_string(kind) + " not implemented");
    }
}

bool Thread::invoke(Frame &f, uint32_t op, uint32_t operand)
{
    size_t token = f.ip - 1;
    const ClassUnit *u = f.unit;
    std::string name, desc, mclass;
    if (op == 0x401a) {
        name = u->member_name((int)operand);
        desc = u->member_type((int)operand);
        mclass = u->member_class((int)operand);
    } else {
        const char *n = u->str((int)operand);
        if (!n) return fail("bad method name constant");
        name = n;
    }
    if (name.empty()) return fail("empty method name");

    // A member invoke (0x1a) carries an explicit receiver only when the token is
    // inside an 0x11 window; without one it is an implicit/static call resolved
    // through the member's class. 0x25 always names an explicit receiver.
    bool have_recv = false;
    if (op == 0x4025)
        have_recv = true;
    else if (op == 0x401a)
        have_recv = f.win_active && token >= f.win_start && token < f.win_end;
    else if (op == 0x4012)
        have_recv = f.win_active && token > f.win_start && token < f.win_end;

    Value recv;
    if (have_recv) {
        if (!pop(f, &recv)) return false;
        std::string e;
        if (!vm_->materialize(recv, &recv, &e)) return fail(e);
    }
    int argc = 0;
    std::vector<Value> args;
    if (!pop_args(f, &argc, &args)) return false;
    f.win_active = false;

    ClassState *start = nullptr;
    Object *self = nullptr;
    if (op == 0x4026) {
        if (!f.state || !f.state->super) return fail("super call without superclass");
        start = f.state->super;
        self = f.self;
    } else if (have_recv) {
        if (recv.kind == Value::Obj) {
            if (!recv.obj || !recv.obj->cls) return fail("receiver has no class");
            start = recv.obj->cls;
            self = recv.obj;
        } else if (recv.kind == Value::Str) {
            // Strings are Values, not Objects; natives still take them as `this`.
            start = vm_->get_class("java.lang.String");
            if (!start) return fail("unknown class java.lang.String");
        } else if (recv.kind == Value::Type) {
            std::string cn = mclass.empty() ? recv.s : mclass;
            start = vm_->get_class(cn);
            if (!start) start = vm_->get_class(recv.s);
            if (!start) return fail("unknown class " + cn);
        } else if (recv.kind == Value::Null) {
            return fail("null receiver");
        } else {
            return fail("receiver is not an object");
        }
    } else {
        // Implicit receiver, or a static invoke resolved through the member class.
        start = !mclass.empty() ? vm_->get_class(mclass) : nullptr;
        if (!start) start = f.state;
        if (!start) return fail("implicit receiver without a class");
        self = f.self;
    }

    std::string e;
    ClassState *decl = nullptr;
    const MethodInfo *m = vm_->find_method(start, name, desc, argc, args, &decl, &e);
    if (!m) return fail(e);
    if (m->is_static) {
        self = nullptr;
    } else if (!self) {
        if (!have_recv || recv.kind != Value::Obj || !recv.obj)
            return fail("instance method " + name + " without receiver");
        self = recv.obj;
    }
    return start_call(decl, m, self, have_recv ? &recv : nullptr, &args, false);
}

bool Thread::field_member(Frame &f, uint32_t operand, bool explicit_recv)
{
    const std::string mclass = f.unit->member_class((int)operand);
    const std::string fname = f.unit->member_name((int)operand);
    const std::string ftype = f.unit->member_type((int)operand);
    if (fname.empty()) return fail("bad field constant");

    Value recv;
    if (explicit_recv) {
        if (!pop(f, &recv)) return false;
        std::string e;
        if (!vm_->materialize(recv, &recv, &e)) return fail(e);
    }
    if (explicit_recv && recv.kind == Value::Array) {
        if (fname == "length") {
            push(f, Value::make_int((int32_t)recv.arr->elems.size()));
            return true;
        }
        return fail("unknown array member " + fname);
    }

    ClassState *cs = vm_->get_class(mclass);
    std::string e;
    if (explicit_recv && recv.kind == Value::Type) {
        ClassState *lookup = cs ? cs : vm_->get_class(recv.s);
        if (!lookup) return fail("unknown class " + mclass);
        ClassState *owner = nullptr;
        size_t slot = 0;
        if (!find_static(lookup, &fname, &ftype, &owner, &slot))
            return fail("static field not found: " + mclass + "." + fname);
        if (!vm_->ensure_static(owner, slot, &e)) return fail(e);
        Ref r;
        r.kind = Ref::Static;
        r.cstate = owner;
        r.slot = slot;
        push(f, Value::make_ref(r));
        return true;
    }

    if (explicit_recv) {
        if (recv.kind != Value::Obj || !recv.obj) return fail("field access on non-object");
        Object *obj = recv.obj;
        size_t idx = 0;
        if (find_instance(obj->cls, &fname, &ftype, &idx)) {
            Ref r;
            r.kind = Ref::Instance;
            r.obj = obj;
            r.slot = idx;
            push(f, Value::make_ref(r));
            return true;
        }
        // Java allows accessing a static field through an instance.
        ClassState *owner = nullptr;
        size_t slot = 0;
        if (cs && find_static(cs, &fname, &ftype, &owner, &slot)) {
            if (!vm_->ensure_static(owner, slot, &e)) return fail(e);
            Ref r;
            r.kind = Ref::Static;
            r.cstate = owner;
            r.slot = slot;
            push(f, Value::make_ref(r));
            return true;
        }
        return fail("field not found: " + fname + " " + ftype);
    }

    ClassState *owner = nullptr;
    size_t slot = 0;
    if (cs && find_static(cs, &fname, &ftype, &owner, &slot)) {
        if (!vm_->ensure_static(owner, slot, &e)) return fail(e);
        Ref r;
        r.kind = Ref::Static;
        r.cstate = owner;
        r.slot = slot;
        push(f, Value::make_ref(r));
        return true;
    }
    if (!f.self) return fail("no implicit this for field " + fname);
    size_t idx = 0;
    if (!find_instance(f.self->cls, &fname, &ftype, &idx)) return fail("field not found: " + fname);
    Ref r;
    r.kind = Ref::Instance;
    r.obj = f.self;
    r.slot = idx;
    push(f, Value::make_ref(r));
    return true;
}

bool Thread::field_name(Frame &f, uint32_t operand)
{
    const char *np = f.unit->str((int)operand);
    if (!np) return fail("bad field name constant");
    std::string name = np;
    Value recv;
    if (!pop(f, &recv)) return false;
    std::string e;
    if (!vm_->materialize(recv, &recv, &e)) return fail(e);

    if (recv.kind == Value::Array) {
        if (name == "length") {
            push(f, Value::make_int((int32_t)recv.arr->elems.size()));
            return true;
        }
        return fail("unknown array member " + name);
    }
    if (recv.kind == Value::Type) {
        ClassState *cs = vm_->get_class(recv.s);
        ClassState *owner = nullptr;
        size_t slot = 0;
        if (!cs || !find_static(cs, &name, nullptr, &owner, &slot))
            return fail("static field not found: " + name);
        if (!vm_->ensure_static(owner, slot, &e)) return fail(e);
        Ref r;
        r.kind = Ref::Static;
        r.cstate = owner;
        r.slot = slot;
        push(f, Value::make_ref(r));
        return true;
    }
    if (recv.kind != Value::Obj || !recv.obj) return fail("field access on non-object");
    size_t idx = 0;
    if (find_instance(recv.obj->cls, &name, nullptr, &idx)) {
        Ref r;
        r.kind = Ref::Instance;
        r.obj = recv.obj;
        r.slot = idx;
        push(f, Value::make_ref(r));
        return true;
    }
    ClassState *owner = nullptr;
    size_t slot = 0;
    if (find_static(recv.obj->cls, &name, nullptr, &owner, &slot)) {
        if (!vm_->ensure_static(owner, slot, &e)) return fail(e);
        Ref r;
        r.kind = Ref::Static;
        r.cstate = owner;
        r.slot = slot;
        push(f, Value::make_ref(r));
        return true;
    }
    return fail("field not found: " + name);
}

bool Thread::step()
{
    if (frames_.empty()) return true;
    Frame &f = *frames_.back();
    if (f.ip >= f.code.size()) return fail("fell off the end of the method body");
    RuntimeInstr ins = f.code[f.ip++];
    line_ = ins.line;

    switch (ins.op) {
    case 0x1001: { // load local
        size_t slot = ins.operand;
        if (slot >= f.locals.size())
            return fail("local slot " + std::to_string(slot) + " not declared");
        Ref r;
        r.kind = Ref::Local;
        r.frame = &f;
        r.slot = slot;
        push(f, Value::make_ref(r));
        return true;
    }
    case 0x1002: { // declare local
        size_t slot = ins.operand;
        if (slot > 65535) return fail("local slot out of range");
        f.pending_decl = (int)slot;
        if (f.locals.size() <= slot) f.locals.resize(slot + 1);
        return true;
    }
    case 0x1003: // epilogue marker
    case 0x1024: // call marker
    case 0x102b: // epilogue marker
    case 0x102c: // declaration type marker
        return true;

    case 0x1004: // && short-circuit: true skips the following jump
    case 0x1005: { // || short-circuit: false skips the following jump
        Value v;
        if (!top_value(f, &v)) return false;
        bool skip = ins.op == 0x1004 ? is_truthy(v) : !is_truthy(v);
        if (!skip) return true;
        if (f.ip >= f.code.size() || f.code[f.ip].op != 0x1014)
            return fail("short-circuit operator not followed by a jump");
        f.ip++;
        return true;
    }
    case 0x1007: // AST node, result kept
    case 0x1008: // AST node, statement result discarded
        return node(f, (int)ins.operand, ins.op == 0x1007);

    case 0x200a: // float literal: raw IEEE bits
        push(f, Value::make_float(float_from_bits(ins.operand)));
        return true;
    case 0x100b: // int literal
    case 0x100d: // boolean literal, represented as int
        push(f, Value::make_int((int32_t)ins.operand));
        return true;
    case 0x100c: // null
        push(f, Value::make_null());
        return true;

    case 0x1010: { // end of field-initializer body
        Value v;
        if (!pop(f, &v)) return false;
        return finish_frame(v);
    }
    case 0x1011: { // open explicit receiver window
        if (ins.operand == 0 || f.ip + ins.operand > f.code.size())
            return fail("receiver window out of range");
        f.win_active = true;
        f.win_start = f.ip;
        f.win_end = f.ip + ins.operand;
        return true;
    }
    case 0x1014: { // unconditional jump, operand relative to this token
        int64_t target = (int64_t)(f.ip - 1) + (int32_t)ins.operand;
        if (target < 0 || target > (int64_t)f.code.size()) return fail("jump out of range");
        f.ip = (size_t)target;
        return true;
    }
    case 0x1015: // branch if false
    case 0x1016: { // branch if true
        Value v;
        if (!pop(f, &v)) return false;
        std::string e;
        if (!vm_->materialize(v, &v, &e)) return fail(e);
        bool take = ins.op == 0x1016 ? is_truthy(v) : !is_truthy(v);
        if (take) {
            int64_t target = (int64_t)(f.ip - 1) + (int32_t)ins.operand;
            if (target < 0 || target > (int64_t)f.code.size()) return fail("jump out of range");
            f.ip = (size_t)target;
        }
        return true;
    }
    case 0x1017: { // switch case: compare the case value with the selector below it
        Value cv;
        if (!pop(f, &cv)) return false;
        std::string e;
        if (!vm_->materialize(cv, &cv, &e)) return fail(e);
        Value sel;
        if (!top_value(f, &sel)) return false;
        if (values_equal(cv, sel)) {
            Value discard;
            if (!pop(f, &discard)) return false;
            int64_t target = (int64_t)(f.ip - 1) + (int32_t)ins.operand;
            if (target < 0 || target > (int64_t)f.code.size()) return fail("jump out of range");
            f.ip = (size_t)target;
        }
        return true;
    }
    case 0x1018: { // switch default: drop the selector
        Value discard;
        return pop(f, &discard);
    }
    case 0x1020: { // array index: stack is [index, array]
        Value av, iv;
        if (!pop(f, &av)) return false;
        std::string e;
        if (!vm_->materialize(av, &av, &e)) return fail(e);
        if (!pop(f, &iv)) return false;
        if (!vm_->materialize(iv, &iv, &e)) return fail(e);
        if (av.kind != Value::Array || !av.arr) return fail("indexing a non-array");
        if (iv.kind != Value::Int) return fail("array index is not an integer");
        if (iv.i < 0 || (size_t)iv.i >= av.arr->elems.size()) return fail("array index out of bounds");
        Ref r;
        r.kind = Ref::Element;
        r.arr = av.arr;
        r.slot = (size_t)iv.i;
        push(f, Value::make_ref(r));
        return true;
    }
    case 0x1021: // finalize new: create instance, run field inits, call <init>
        return finalize_new(f);
    case 0x1022: f.pending_ctor_this = true; return true;
    case 0x1023: f.pending_ctor_super = true; return true;
    case 0x1027: push(f, Value::make_int((int32_t)ins.operand)); return true;
    case 0x1028: { // return value
        Value v;
        if (!pop(f, &v)) return false;
        return finish_frame(v);
    }
    case 0x1029: // return void
    case 0x102a: // constructor return
        return finish_frame(Value::make_null());
    case 0x102d: { // store initializer into the pending declaration slot
        if (f.pending_decl < 0) return fail("local store without declaration");
        Value v;
        if (!pop(f, &v)) return false;
        std::string e;
        if (!vm_->materialize(v, &v, &e)) return fail(e);
        Ref r;
        r.kind = Ref::Local;
        r.frame = &f;
        r.slot = (size_t)f.pending_decl;
        if (!vm_->store(r, v, &e)) return fail(e);
        f.pending_decl = -1;
        return true;
    }

    case 0x4009: { // string constant
        const char *s = f.unit->str((int)ins.operand);
        if (!s) return fail("bad string constant");
        push(f, Value::make_str(s));
        return true;
    }
    case 0x400e: { // resource constant (name, id)
        const ConstEntry *e = f.unit->entry((int)ins.operand);
        if (!e || e->type != CONST_RESOURCE) return fail("bad resource constant");
        const char *n = f.unit->str(e->a);
        push(f, Value::make_resource(n ? n : "", e->b));
        return true;
    }
    case 0x4006: { // type descriptor, meaning depends on the pending marker
        const char *dp = f.unit->str((int)ins.operand);
        if (!dp) return fail("bad type descriptor constant");
        std::string desc = dp;
        std::string e;
        if (f.pending_new) {
            f.pending_new = false;
            push(f, Value::make_type(nullptr, desc));
            return true;
        }
        if (f.pending_new_array) {
            f.pending_new_array = false;
            Value sv;
            if (!pop(f, &sv)) return false;
            if (!vm_->materialize(sv, &sv, &e)) return fail(e);
            if (sv.kind != Value::Int || sv.i < 0) return fail("bad array size");
            if (sv.i > 16 * 1024 * 1024) return fail("array size too large");
            if (desc.empty() || desc[0] != '[') return fail("new[] without array descriptor");
            auto arr = std::make_unique<Array>();
            arr->elem = desc.substr(1);
            arr->elems.resize((size_t)sv.i, default_for_desc(arr->elem));
            Array *raw = arr.get();
            vm_->arrays_.push_back(std::move(arr));
            push(f, Value::make_array(raw));
            return true;
        }
        if (f.pending_instanceof) {
            f.pending_instanceof = false;
            Value v;
            if (!pop(f, &v)) return false;
            if (!vm_->materialize(v, &v, &e)) return fail(e);
            push(f, Value::make_int(is_instance_of(v, desc) ? 1 : 0));
            return true;
        }
        if (f.pending_cast) {
            f.pending_cast = false;
            Value v;
            if (!pop(f, &v)) return false;
            if (!vm_->materialize(v, &v, &e)) return fail(e);
            push(f, v);
            return true;
        }
        if (f.pending_decl >= 0) return true; // type of a local declaration
        return fail("type token without a pending new/array/instanceof/cast/declaration");
    }
    case 0x4019: { // class constant
        if (f.pending_cast) {
            f.pending_cast = false;
            Value v;
            if (!pop(f, &v)) return false;
            std::string e;
            if (!vm_->materialize(v, &v, &e)) return fail(e);
            push(f, v);
            return true;
        }
        std::string name = f.unit->class_name((int)ins.operand);
        if (name.empty()) return fail("bad class constant");
        push(f, Value::make_type(nullptr, name));
        return true;
    }
    case 0x4012: // invoke by name (receiver window decides explicit/implicit)
    case 0x401a: // invoke member reference (explicit receiver on stack)
    case 0x4025: // invoke by name on an explicit receiver
    case 0x4026: // invoke by name on super
        return invoke(f, ins.op, ins.operand);
    case 0x401b: // member field (explicit receiver inside the 0x11 window)
        return field_member(f, ins.operand, in_window(f, f.ip - 1));
    case 0x401c: // field of the explicit receiver on top of the stack
        return field_name(f, ins.operand);
    default: {
        char buf[64];
        snprintf(buf, sizeof buf, "runtime opcode 0x%04x not implemented", ins.op);
        return fail(buf);
    }
    }
}

void expand_body(const Body &body, std::vector<RuntimeInstr> *out)
{
    out->clear();
    out->reserve(body.instrs.size());
    for (const Instr &in : body.instrs) {
        uint16_t op;
        if (!in.has_operand) {
            op = (uint16_t)(0x1000u | in.op);
        } else {
            switch (in.op) {
            case 0x0a: op = 0x200a; break; // float literal: operand = IEEE bits
            // Constant-pool references. Deviation from notes/05: the file format
            // (FORMAT.md and the whole corpus) has 0x09 = string and 0x0e =
            // resource(name,id), not the other way round; tagged 0x4009/0x400e.
            case 0x06: case 0x09: case 0x0e: case 0x12: case 0x19:
            case 0x1a: case 0x1b: case 0x1c: case 0x25: case 0x26:
                op = (uint16_t)(0x4000u | in.op);
                break;
            default: op = (uint16_t)(0x1000u | in.op); break;
            }
        }
        out->push_back(RuntimeInstr{op, in.line, in.operand});
    }
}

// ---------------------------------------------------------------------------
// Vm

Vm::Vm()
{
    natives::install(*this);
}
Vm::~Vm() = default;

void Vm::add_class(const ClassUnit *unit)
{
    if (!unit) return;
    std::string name = unit->class_name(unit->self_class);
    if (name.empty() || by_name_.count(name)) return;
    auto cs = std::make_unique<ClassState>();
    cs->unit = unit;
    cs->name = name;
    if (unit->super_class >= 0) cs->super_name = unit->class_name(unit->super_class);
    for (const FieldInfo &f : unit->fields) {
        FieldRef fr;
        fr.info = &f;
        fr.unit = unit;
        if (f.is_static) {
            cs->own_statics.push_back(fr);
            const char *d = unit->str(f.type);
            cs->statics.push_back(default_for_desc(d ? d : ""));
            cs->sinit.push_back(f.init < 0 ? 1 : 0);
            cs->sbusy.push_back(0);
        } else {
            cs->own_inst.push_back(fr);
        }
    }
    by_name_[name] = cs.get();
    states_.push_back(std::move(cs));
    linked_ = false;
}

ClassState *Vm::find_class(const std::string &name) const
{
    auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : it->second;
}

// 0x55add0: '[' is an array descriptor (arrays are Values, not classes here);
// a leading 'L' unwraps the descriptor ("Lfoo;" -> "foo"). A miss calls the
// installed loader once and looks the name up again.
ClassState *Vm::get_class(const std::string &name)
{
    std::string n = name;
    if (!n.empty() && n[0] == 'L')
        n = n.substr(1, n.size() >= 2 ? n.size() - 2 : 0);
    ClassState *c = find_class(n);
    if (!c && loader_ && !loading_.count(n)) {
        loading_.insert(n);
        loader_(*this, n);
        loading_.erase(n);
        c = find_class(n);
    }
    if (!c) return nullptr;
    std::string err;
    if (!ensure_linked(&err)) return nullptr;
    return c;
}

void Vm::register_native(const std::string &name, const std::string &sig, NativeFn fn)
{
    natives_[name + sig] = std::move(fn);
}

void Vm::register_native(const std::string &cls, const std::string &name,
                         const std::string &sig, NativeFn fn)
{
    natives_[cls + "." + name + sig] = std::move(fn);
}

ClassState *Vm::state_for(const ClassUnit *unit) const
{
    for (const auto &s : states_)
        if (s->unit == unit) return s.get();
    return nullptr;
}

bool Vm::ensure_linked(std::string *err)
{
    if (linked_) return true;
    // 0x549770: a class init resolves the superclass through getClass, so load
    // missing supers first. Collect the names before loading: add_class grows
    // states_, which would invalidate the iteration.
    std::vector<std::string> missing;
    for (auto &s : states_)
        if (!s->super_name.empty() && by_name_.find(s->super_name) == by_name_.end())
            missing.push_back(s->super_name);
    for (const std::string &n : missing) get_class(n);
    if (linked_) return true;
    for (auto &s : states_) {
        s->super = nullptr;
        if (!s->super_name.empty()) {
            auto it = by_name_.find(s->super_name);
            if (it != by_name_.end()) s->super = it->second;
        }
        s->built = false;
        s->building = false;
    }
    std::function<bool(ClassState *)> build = [&](ClassState *s) -> bool {
        if (s->built) return true;
        if (s->building) {
            *err = "class hierarchy cycle at " + s->name;
            return false;
        }
        s->building = true;
        if (s->super && !build(s->super)) return false;
        s->ifields = s->super ? s->super->ifields : std::vector<FieldRef>();
        s->inst_base = s->ifields.size();
        for (const FieldRef &fr : s->own_inst) s->ifields.push_back(fr);
        s->built = true;
        s->building = false;
        return true;
    };
    for (auto &s : states_)
        if (!build(s.get())) return false;
    linked_ = true;
    return true;
}

bool Vm::materialize(const Value &in, Value *out, std::string *err) const
{
    if (in.kind != Value::Ref) {
        *out = in;
        return true;
    }
    switch (in.ref.kind) {
    case Ref::Local:
        if (!in.ref.frame || in.ref.slot >= in.ref.frame->locals.size()) {
            *err = "dangling local reference";
            return false;
        }
        *out = in.ref.frame->locals[in.ref.slot];
        return true;
    case Ref::Static:
        if (!in.ref.cstate || in.ref.slot >= in.ref.cstate->statics.size()) {
            *err = "dangling static field reference";
            return false;
        }
        *out = in.ref.cstate->statics[in.ref.slot];
        return true;
    case Ref::Instance:
        if (!in.ref.obj || in.ref.slot >= in.ref.obj->fields.size()) {
            *err = "dangling instance field reference";
            return false;
        }
        *out = in.ref.obj->fields[in.ref.slot];
        return true;
    case Ref::Element:
        if (!in.ref.arr || in.ref.slot >= in.ref.arr->elems.size()) {
            *err = "dangling array element reference";
            return false;
        }
        *out = in.ref.arr->elems[in.ref.slot];
        return true;
    default:
        *err = "invalid reference";
        return false;
    }
}

bool Vm::store(const Ref &ref, const Value &value, std::string *err)
{
    Value v;
    if (!materialize(value, &v, err)) return false;
    switch (ref.kind) {
    case Ref::Local:
        if (!ref.frame || ref.slot >= ref.frame->locals.size()) {
            *err = "dangling local reference";
            return false;
        }
        ref.frame->locals[ref.slot] = v;
        return true;
    case Ref::Static:
        if (!ref.cstate || ref.slot >= ref.cstate->statics.size()) {
            *err = "dangling static field reference";
            return false;
        }
        ref.cstate->statics[ref.slot] = v;
        return true;
    case Ref::Instance:
        if (!ref.obj || ref.slot >= ref.obj->fields.size()) {
            *err = "dangling instance field reference";
            return false;
        }
        ref.obj->fields[ref.slot] = v;
        return true;
    case Ref::Element:
        if (!ref.arr || ref.slot >= ref.arr->elems.size()) {
            *err = "dangling array element reference";
            return false;
        }
        ref.arr->elems[ref.slot] = v;
        return true;
    default:
        *err = "invalid assignment target";
        return false;
    }
}

bool Vm::ensure_static(ClassState *state, size_t slot, std::string *err)
{
    if (!state || slot >= state->sinit.size()) {
        *err = "static field slot out of range";
        return false;
    }
    if (state->sinit[slot]) return true;
    if (state->sbusy[slot]) {
        *err = "recursive static initializer in " + state->name;
        return false;
    }
    state->sbusy[slot] = 1;
    Value v;
    bool ok = run_field_init(*state->unit, state->own_statics[slot].info->init, nullptr, &v, err);
    state->sbusy[slot] = 0;
    if (!ok) return false;
    state->statics[slot] = v;
    state->sinit[slot] = 1;
    return true;
}

bool Vm::init_object_fields(Object *obj, std::string *err)
{
    std::vector<ClassState *> chain;
    for (ClassState *s = obj->cls; s; s = s->super) chain.push_back(s);
    for (size_t ci = chain.size(); ci-- > 0;) {
        ClassState *s = chain[ci];
        for (size_t k = 0; k < s->own_inst.size(); k++) {
            const FieldRef &fr = s->own_inst[k];
            if (fr.info->init < 0) continue;
            Value v;
            if (!run_field_init(*fr.unit, fr.info->init, obj, &v, err)) return false;
            size_t idx = s->inst_base + k;
            if (idx < obj->fields.size()) obj->fields[idx] = v;
        }
    }
    return true;
}

Object *Vm::new_object(ClassState *state, std::string *err)
{
    if (!state) {
        *err = "new with unknown class";
        return nullptr;
    }
    if (!ensure_linked(err)) return nullptr;
    auto obj = std::make_unique<Object>();
    obj->cls = state;
    obj->fields.resize(state->ifields.size());
    for (size_t i = 0; i < state->ifields.size(); i++) {
        const char *d = field_desc(state->ifields[i]);
        obj->fields[i] = default_for_desc(d ? d : "");
    }
    Object *raw = obj.get();
    objects_.push_back(std::move(obj));
    if (!init_object_fields(raw, err)) return nullptr;
    return raw;
}

Array *Vm::new_array(const std::string &elem, size_t size)
{
    auto arr = std::make_unique<Array>();
    arr->elem = elem;
    arr->elems.assign(size, default_for_desc(elem));
    Array *raw = arr.get();
    arrays_.push_back(std::move(arr));
    return raw;
}

bool Vm::get_instance_field(Object *obj, const std::string &name, Value *out,
                            std::string *err) const
{
    size_t idx = 0;
    if (!obj || !find_instance(obj->cls, &name, nullptr, &idx)) {
        *err = "field not found: " + name;
        return false;
    }
    return materialize(obj->fields[idx], out, err);
}

bool Vm::get_static_field(const std::string &cls, const std::string &name, Value *out,
                          std::string *err)
{
    ClassState *cs = get_class(cls);
    ClassState *owner = nullptr;
    size_t slot = 0;
    if (!cs || !find_static(cs, &name, nullptr, &owner, &slot)) return false;
    // 0x549750 runs the class static init before reading the slot.
    if (!ensure_static(owner, slot, err)) return false;
    *out = owner->statics[slot];
    return true;
}

bool Vm::set_instance_field(Object *obj, const std::string &name, const Value &value,
                            std::string *err)
{
    size_t idx = 0;
    if (!obj || !find_instance(obj->cls, &name, nullptr, &idx)) {
        *err = "field not found: " + name;
        return false;
    }
    obj->fields[idx] = value;
    return true;
}

const MethodInfo *Vm::find_method(ClassState *start, const std::string &name,
                                  const std::string &desc, int argc,
                                  const std::vector<Value> &args, ClassState **decl,
                                  std::string *err)
{
    if (!start) {
        *err = "no class to resolve " + name;
        return nullptr;
    }
    if (!desc.empty()) {
        for (ClassState *s = start; s; s = s->super) {
            for (const MethodInfo &m : s->unit->methods) {
                const char *n = s->unit->str(m.name);
                const char *d = s->unit->str(m.sig);
                if (n && d && name == n && desc == d) {
                    if (decl) *decl = s;
                    return &m;
                }
            }
        }
        *err = "method not found: " + start->name + "." + name + desc;
        return nullptr;
    }
    const MethodInfo *best = nullptr;
    ClassState *bestdecl = nullptr;
    int bestscore = -1;
    for (ClassState *s = start; s; s = s->super) {
        for (const MethodInfo &m : s->unit->methods) {
            const char *n = s->unit->str(m.name);
            if (!n || name != n) continue;
            const char *sg = s->unit->str(m.sig);
            if (!sg) continue;
            std::vector<char> params;
            int arity = 0;
            if (!parse_params(sg, &params, &arity) || arity != argc) continue;
            int score = 0;
            bool ok = true;
            for (int i = 0; i < argc; i++) {
                int sc = arg_score(params[(size_t)i], args[(size_t)i]);
                if (!sc) {
                    ok = false;
                    break;
                }
                score += sc;
            }
            if (!ok) continue;
            if (score > bestscore) {
                best = &m;
                bestdecl = s;
                bestscore = score;
                if (score == 2 * argc) break;
            }
        }
        if (best) break; // a derived class hides same-name overloads below it
    }
    if (!best) {
        *err = "method not found: " + start->name + "." + name + " (" +
               std::to_string(argc) + " args)";
        return nullptr;
    }
    if (decl) *decl = bestdecl;
    return best;
}

bool Vm::run_native(const std::string &cls, const std::string &name,
                    const std::string &sig, const Value *receiver,
                    const std::vector<Value> &args, Value *result, std::string *err)
{
    // Natives see the receiver at argument 0 and arguments converted to their
    // declared descriptor, matching the exe's callMethod/native convention.
    std::vector<Value> full;
    if (receiver) full.push_back(*receiver);
    std::vector<char> params;
    int arity = 0;
    parse_params(sig, &params, &arity);
    for (size_t i = 0; i < args.size(); i++) {
        Value v = args[i];
        if (i < params.size() && params[i] == 'F' && v.kind == Value::Int)
            v = Value::make_float((float)v.i);
        full.push_back(v);
    }
    auto it = natives_.find(cls + "." + name + sig);
    if (it == natives_.end()) it = natives_.find(name + sig);
    if (it == natives_.end()) it = natives_.find(name);
    if (it == natives_.end()) {
        *err = "native method not implemented: " + name + sig;
        return false;
    }
    return it->second(*this, full, result, err);
}

bool Vm::invoke_native(const ClassUnit &unit, const MethodInfo &method,
                       const Value *receiver, const std::vector<Value> &args,
                       Value *result, std::string *err)
{
    const char *n = unit.str(method.name);
    const char *s = unit.str(method.sig);
    return run_native(unit.class_name(unit.self_class), n ? n : "", s ? s : "",
                      receiver, args, result, err);
}

bool Vm::run_field_init(const ClassUnit &unit, int body_index, Object *self, Value *result,
                        std::string *err)
{
    if (!ensure_linked(err)) return false;
    if (body_index < 0 || (size_t)body_index >= unit.bodies.size()) {
        *err = "field initializer body index out of range";
        return false;
    }
    const Body &body = unit.bodies[(size_t)body_index];
    if (body.instrs.empty()) {
        *result = Value::make_null();
        return true;
    }
    auto t = std::unique_ptr<Thread>(new Thread(this));
    auto f = std::make_unique<Frame>();
    f->unit = &unit;
    f->state = state_for(&unit);
    f->self = self;
    f->field_init = true;
    if (self) f->locals.push_back(Value::make_obj(self));
    expand_body(body, &f->code);
    t->frames_.push_back(std::move(f));
    if (t->run() != Thread::Done) {
        *err = t->error();
        return false;
    }
    *result = t->result();
    return true;
}

std::unique_ptr<Thread> Vm::start(const ClassUnit &unit, const MethodInfo &method, Object *self,
                                  const std::vector<Value> &args, std::string *err)
{
    if (!ensure_linked(err)) return nullptr;
    ClassState *st = state_for(&unit);
    if (!st) {
        *err = "class not registered: " + unit.class_name(unit.self_class);
        return nullptr;
    }
    if (method.flags & 0x40) {
        *err = "native method cannot be started as a thread";
        return nullptr;
    }
    if (!method.is_static && !self) {
        *err = "instance method needs a receiver";
        return nullptr;
    }
    if (method.body < 0 || (size_t)method.body >= unit.bodies.size()) {
        *err = "method has no body";
        return nullptr;
    }
    auto t = std::unique_ptr<Thread>(new Thread(this));
    const Body &body = unit.bodies[(size_t)method.body];
    if (body.instrs.empty()) {
        const char *sg = unit.str(method.sig);
        t->result_ = default_for_desc(return_desc(sg ? sg : std::string()));
        t->status_ = Thread::Done;
        return t;
    }
    auto f = std::make_unique<Frame>();
    fill_frame(f.get(), st, &method, self, args);
    t->frames_.push_back(std::move(f));
    return t;
}

bool Vm::invoke(const ClassUnit &unit, const MethodInfo &method, Object *self,
                const std::vector<Value> &args, Value *result, std::string *err)
{
    if (method.flags & 0x40) {
        Value selfv;
        const Value *recv = nullptr;
        if (!method.is_static) {
            if (!self) {
                *err = "instance native called without receiver";
                return false;
            }
            selfv = Value::make_obj(self);
            recv = &selfv;
        }
        return invoke_native(unit, method, recv, args, result, err);
    }
    std::unique_ptr<Thread> t = start(unit, method, self, args, err);
    if (!t) return false;
    if (t->run() != Thread::Done) {
        *err = t->error();
        return false;
    }
    *result = t->result();
    return true;
}

bool Vm::invoke_static(const ClassUnit &unit, const MethodInfo &method,
                       const std::vector<Value> &args, Value *result, std::string *err)
{
    return invoke(unit, method, nullptr, args, result, err);
}
