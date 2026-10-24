// Script VM: values, objects, classes and the time-sliced Thread interpreter.
// The interpreter executes an 8-byte token stream (op:u16 line:u16 operand:u32)
// expanded from a TREE body. Semantics follow notes/05-thread-run-opcodes.md and
// the file format in FORMAT.md; deviations found in the shipped corpus are
// documented at the code that depends on them.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "script/classfile.h"

struct RuntimeInstr {
    uint16_t op = 0;      // runtime opcode: 0x10xx plain, 0x20xx float bits, 0x40xx const
    uint16_t line = 0;    // 1-based source line, for error messages
    uint32_t operand = 0;
};

struct ClassState;
struct Object;
struct Array;
struct Frame;

// Reference to an assignable location: a local slot, a static field, an instance
// field or an array element. Expression consumers materialize it; assignments
// write through it.
struct Ref {
    enum Kind { None, Local, Static, Instance, Element } kind = None;
    Frame *frame = nullptr;       // Local slot owner
    ClassState *cstate = nullptr; // Static field owner
    Object *obj = nullptr;        // Instance field owner
    Array *arr = nullptr;         // Element owner
    size_t slot = 0;
};

// Value model (notes/05): int, float, boolean (int), String, object, array, null.
// Resource is the 0x0e constant; Type is a class/descriptor token produced by
// 0x19/0x06 and consumed by invokes, new and casts.
struct Value {
    enum Kind { Null, Int, Float, Str, Obj, Array, Type, Resource, Ref } kind = Null;
    int32_t i = 0;
    float f = 0.0f;
    std::string s;
    Object *obj = nullptr;
    struct Array *arr = nullptr;
    const ClassUnit *type_unit = nullptr;
    struct Ref ref;

    static Value make_int(int32_t v);
    static Value make_float(float v);
    static Value make_str(const std::string &v);
    static Value make_obj(Object *o);
    static Value make_array(struct Array *a);
    static Value make_type(const ClassUnit *u, const std::string &desc);
    static Value make_resource(const std::string &name, int32_t id);
    static Value make_null();
    static Value make_ref(const struct Ref &r);
    const char *type() const;
};

// Runtime object and array. Script arrays hold Values directly; Object fields are
// the flattened instance layout of its class (super fields first).
struct Object {
    ClassState *cls = nullptr;
    std::vector<Value> fields;
};

struct Array {
    std::string elem;
    std::vector<Value> elems;
};

// TREE file instruction -> runtime token. Tag scheme from notes/05: 0x10xx plain
// operand, 0x20xx float bits, 0x40xx constant-pool index.
void expand_body(const Body &body, std::vector<RuntimeInstr> *out);

// One activation record. The thread owns a stack of frames, so a script call
// suspends and resumes naturally across time slices.
struct Frame {
    const ClassUnit *unit = nullptr;    // class unit owning the code
    ClassState *state = nullptr;        // resolved class state of that unit
    const MethodInfo *method = nullptr; // null for field initializers
    std::vector<RuntimeInstr> code;
    size_t ip = 0;
    std::vector<Value> locals;
    std::vector<Value> stack;
    Object *self = nullptr;             // `this`, null in static methods
    Value str_this;                     // `this` of a script String method (Strings are Values)
    std::vector<char> local_kind;       // declared 'I'/'F' per local slot (0 = other)
    bool suppress_result = false;       // constructor calls: do not push a result
    bool field_init = false;            // terminates on 0x10

    // 0x11 opens a window of tokens that form an explicit receiver expression.
    bool win_active = false;
    size_t win_start = 0, win_end = 0;

    int pending_decl = -1;              // 0x02 slot awaiting 0x2d
    bool pending_new = false;           // 0x07/0x08 kind 30
    bool pending_new_array = false;     // kind 31
    bool pending_instanceof = false;    // kind 32
    bool pending_cast = false;          // kind 33
    bool pending_ctor_this = false;     // 0x22
    bool pending_ctor_super = false;    // 0x23
};

class Vm;

// Cooperative interpreter. run_slice executes at most N tokens and can stop in
// the middle of a call; run() drains slices until Done or Failed.
class Thread {
  public:
    enum Status { Done, Yielded, Failed };

    Status run_slice(size_t max_instructions);
    Status run();
    // 0x563290: thread+0x2c & 0xfa checked after every instruction
    void request_stop() { stop_ = true; }
    const Value &result() const { return result_; }
    const std::string &error() const { return error_; }
    Status status() const { return status_; }
    // "Class.method line N" of the innermost frame (also the error prefix)
    std::string where() const;

  private:
    friend class Vm;
    explicit Thread(Vm *vm) : vm_(vm) {}
    bool step();
    bool fail(const std::string &msg);
    bool pop(Frame &f, Value *out);
    void push(Frame &f, const Value &v);
    bool top_value(Frame &f, Value *out);
    bool pop_args(Frame &f, int *argc, std::vector<Value> *args);
    bool in_window(const Frame &f, size_t token) const;
    bool node(Frame &f, int kind, bool keep);
    bool invoke(Frame &f, uint32_t op, uint32_t operand);
    bool field_member(Frame &f, uint32_t operand, bool explicit_recv);
    bool field_name(Frame &f, uint32_t operand);
    bool finish_frame(const Value &result);
    bool start_call(ClassState *decl, const MethodInfo *m, Object *self,
                    const Value *receiver, std::vector<Value> *args, bool suppress);
    bool ctor_chain(Frame &f);
    bool finalize_new(Frame &f);

    Vm *vm_ = nullptr;
    std::vector<std::unique_ptr<Frame>> frames_;
    Value result_;
    std::string error_;
    Status status_ = Yielded;
    bool stop_ = false;
    uint16_t line_ = 0;
};

// Runtime state: class registry, object/array arena, static fields and the
// native dispatch table.
class Vm {
  public:
    Vm();
    ~Vm();
    // fn(vm, args in declared order, result, error); returns false on failure.
    using NativeFn = std::function<bool(Vm &, const std::vector<Value> &, Value *,
                                        std::string *)>;
    // Installed by the host to load a class by FQN on a find_class miss.
    using ClassLoader = std::function<bool(Vm &, const std::string &fqn)>;
    // Installed by a Runtime: turns a script resource constant (name, local)
    // into its global id (0x5189e0 / 0x55cad0).
    using ResourceResolver = std::function<int32_t(const std::string &pkg, int32_t local)>;

    // 0x554ee0: always creates a ClassState; a name already in by_name_ keeps
    // the first entry (0x55af50) but the new unit still gets its own state.
    ClassState *add_class(const ClassUnit *unit);
    // Keeps a ClassFile parsed outside the ClassPath alive for its units.
    void keep_file(std::unique_ptr<ClassFile> file);
    ClassState *find_class(const std::string &name) const;
    void set_class_loader(ClassLoader f) { loader_ = std::move(f); }
    void set_resource_resolver(ResourceResolver f) { resource_resolver_ = std::move(f); }
    // 0x55add0: descriptor unwrap, then the loaded table, then the loader once.
    ClassState *get_class(const std::string &name);
    void register_native(const std::string &name, const std::string &sig, NativeFn fn);
    // Class-qualified registration: (class, method, descriptor), like the exe's
    // native table. run_native prefers these keys and falls back to name+sig.
    void register_native(const std::string &cls, const std::string &name,
                         const std::string &sig, NativeFn fn);

    bool invoke(const ClassUnit &unit, const MethodInfo &method, Object *self,
                const std::vector<Value> &args, Value *result, std::string *err);
    // Native dispatch with an explicit receiver Value, so String receivers (not
    // Objects in this VM) can reach their natives. receiver is prepended to args.
    bool invoke_native(const ClassUnit &unit, const MethodInfo &method,
                       const Value *receiver, const std::vector<Value> &args,
                       Value *result, std::string *err);
    bool invoke_static(const ClassUnit &unit, const MethodInfo &method,
                       const std::vector<Value> &args, Value *result, std::string *err);
    bool run_field_init(const ClassUnit &unit, int body_index, Object *self, Value *result,
                        std::string *err);

    // Starts a script method as a fresh thread (used by invoke and the scheduler
    // self-check). Returns null and sets err when the method cannot run.
    std::unique_ptr<Thread> start(const ClassUnit &unit, const MethodInfo &method,
                                  Object *self, const std::vector<Value> &args,
                                  std::string *err);
    Object *new_object(ClassState *state, std::string *err);
    Array *new_array(const std::string &elem, size_t size);

    // Named instance field access for natives that manipulate script objects.
    bool get_instance_field(Object *obj, const std::string &name, Value *out,
                            std::string *err) const;
    // 0x549750: static field read; runs the field's initializer first.
    bool get_static_field(const std::string &cls, const std::string &name, Value *out,
                          std::string *err);
    bool set_instance_field(Object *obj, const std::string &name, const Value &value,
                            std::string *err);

    // Materialize a Ref to its current value.
    bool materialize(const Value &in, Value *out, std::string *err) const;
    bool store(const Ref &ref, const Value &value, std::string *err);

    bool ensure_static(ClassState *state, size_t slot, std::string *err);
    const MethodInfo *find_method(ClassState *start, const std::string &name,
                                  const std::string &desc, int argc,
                                  const std::vector<Value> &args, ClassState **decl,
                                  std::string *err);

  private:
    friend class Thread;
    bool ensure_linked(std::string *err);
    ClassState *state_for(const ClassUnit *unit) const;
    bool run_native(const std::string &cls, const std::string &name,
                    const std::string &sig, const Value *receiver,
                    const std::vector<Value> &args, Value *result, std::string *err);
    bool init_object_fields(Object *obj, std::string *err);

    std::vector<std::unique_ptr<ClassState>> states_;
    std::vector<std::unique_ptr<ClassFile>> files_;   // Vm::keep_file
    std::map<std::string, ClassState *> by_name_;
    std::vector<std::unique_ptr<Object>> objects_;
    std::vector<std::unique_ptr<Array>> arrays_;
    std::map<std::string, NativeFn> natives_;
    ClassLoader loader_;
    ResourceResolver resource_resolver_;
    std::set<std::string> loading_;  // re-entry guard for one get_class miss
    bool linked_ = false;
};
