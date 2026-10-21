// Game runtime object/id layer. See game/runtime.h and notes/06, notes/13,
// notes/16. Semantics of handles follow the exe's 0x10-byte ref nodes and the
// natives in classdump --selfcheck exercise both layers.
#include "game/runtime.h"

#include <string.h>

#include "script/vm.h"

Runtime *Runtime::active_ = nullptr;

// vm.h keeps ClassState opaque. vm.cpp defines it with `unit` as its first
// member, so the declaring unit can be recovered via memcpy (same helper as
// native dispatch in script/natives.cpp).
// shortcut: relies on ClassState's leading member in vm.cpp; upgrade when Vm
// exposes a ClassState -> ClassUnit accessor.
static const ClassUnit *class_state_unit(ClassState *st)
{
    const ClassUnit *unit = nullptr;
    if (st) ::memcpy(&unit, st, sizeof unit);
    return unit;
}

// 0x47c960 dispatchScript: EVENT_HOTKEY passes the Hotkey object positionally;
// every other code wraps sender (or target) as a ref and calls the handler with
// [ref, ev, param].
static void dispatch_script(Runtime &rt, Runtime::Resource *target,
                            Runtime::Resource *sender, uint32_t code,
                            const Value &param, const char *method)
{
    Object *script = target->script;
    uint32_t ev = code & 0x0fffffffu;
    std::string err;
    if (code == 0x100000) {
        rt.call_method(script, "handleEvent", {param}, nullptr, &err);
        return;
    }
    Value ref = Value::make_null();
    if (Object *o = rt.wrap(sender ? sender : target, &err))
        ref = Value::make_obj(o);
    if (!method) {
        int32_t async = 0;
        Value v;
        Vm *vm = rt.vm();
        if (vm && vm->get_instance_field(script, "asyncMode", &v, &err) &&
            v.kind == Value::Int)
            async = v.i;
        method = async ? "handleEventAsync" : "handleEvent";
    }
    // INVENTED: param passes through for ev 1/4/0x20/0x40; the exe formats a
    // contact record into a String (no caller of those codes exists yet).
    rt.call_method(script, method, {ref, Value::make_int((int32_t)ev), param},
                   nullptr, &err);
}

Runtime::Runtime(ResourceManager &resources, Vm *vm, ClassPath *classes)
    : resources_(&resources), vm_(vm), classes_(classes)
{
}

Runtime::~Runtime()
{
    if (active_ == this) active_ = nullptr;
    for (auto &kv : handles_) delete kv.second;
    handles_.clear();
}

Runtime *Runtime::active()
{
    return active_;
}

void Runtime::activate()
{
    active_ = this;
}

void Runtime::deactivate()
{
    active_ = nullptr;
}

const Package *Runtime::target_package(const Package &base, uint32_t id,
                                       uint32_t *local) const
{
    uint32_t hi = id >> 16;
    *local = id & 0xffffu;
    if (hi == 0) return &base;
    if ((size_t)hi > base.rpk.subpackages.size()) return nullptr;
    return resources_->find_open_package(base.rpk.subpackages[hi - 1].name);
}

std::string Runtime::script_class_of(const Package &pkg, const RpkRecord &rec) const
{
    for (const DataRef &ref : resources_->refs(pkg, rec)) {
        if (ref.kind != DataRef::Script) continue;
        if (classes_) {
            for (const auto &kv : classes_->entries())
                if (!ref.path.empty() && kv.second.path == ref.path) return kv.first;
        }
        if (!ref.file) return ref.text;  // class named directly by the record
    }
    return std::string();
}

Runtime::Resource *Runtime::resolve(uint32_t id, const Package *base)
{
    if (!id) return nullptr;
    const Package *target = nullptr;
    const RpkRecord *rec = nullptr;
    uint32_t local = 0;
    if (base) {
        target = target_package(*base, id, &local);
        if (target) rec = target->rpk.find(local);
    } else {
        // Same order as ResourceManager::lookup: every open package as base,
        // first owning package wins.
        for (size_t i = 0; i < resources_->package_count() && !rec; i++) {
            const Package *p = target_package(resources_->package(i), id, &local);
            if (!p) continue;
            rec = p->rpk.find(local);
            if (rec) target = p;
        }
    }
    if (!target || !rec) return nullptr;

    auto key = std::make_pair(target, rec->id);
    auto it = records_.find(key);
    if (it != records_.end()) return it->second.get();

    auto up = std::make_unique<Resource>();
    Resource *r = up.get();
    records_[key] = std::move(up);  // cache first: super chains may revisit
    r->id = rec->id;
    r->type = rec->type;
    r->name = rec->name;
    r->package = target;
    r->record = rec;
    r->db = target->rdb.find(rec->id);
    r->script_class = script_class_of(*target, *rec);
    r->parent_id = rec->super_id;
    if (!r->parent_id && r->db) r->parent_id = r->db->super_id;
    if (r->parent_id) r->parent = resolve(r->parent_id, target);
    return r;
}

bool Runtime::chain(Resource *r, std::vector<ChainLink> *out, std::string *err) const
{
    if (!r || !r->package || !r->record) {
        *err = "chain: resource is not record-backed";
        return false;
    }
    return resources_->chain(*r->package, *r->record, out, err);
}

std::string Runtime::type_name(uint32_t type) const
{
    return resources_->type_name(type);
}

Runtime::Handle *Runtime::handle(Object *script) const
{
    auto it = handles_.find(script);
    return it == handles_.end() ? nullptr : it->second;
}

Runtime::Handle *Runtime::attach(Object *script)
{
    if (!script) return nullptr;
    auto it = handles_.find(script);
    if (it != handles_.end()) return it->second;
    Handle *h = new Handle;
    handles_[script] = h;
    return h;
}

void Runtime::detach(Object *script)
{
    auto it = handles_.find(script);
    if (it == handles_.end()) return;
    unlink(it->second);
    delete it->second;
    handles_.erase(it);
}

void Runtime::unlink(Handle *h)
{
    if (!h) return;
    if (h->target) {
        if (h->prev)
            h->prev->next = h->next;
        else
            h->target->handles = h->next;
        if (h->next) h->next->prev = h->prev;
        if (h->target->refs > 0) h->target->refs--;
    }
    h->target = nullptr;
    h->id = 0;
    h->prev = h->next = nullptr;
}

void Runtime::bind(Handle *h, Resource *r)
{
    if (!h || h->target == r) return;
    unlink(h);
    if (!r) return;
    h->target = r;
    h->id = r->id;
    h->next = r->handles;
    if (r->handles) r->handles->prev = h;
    r->handles = h;
    r->refs++;
}

void Runtime::bind(Handle *h, uint32_t id, const Package *base)
{
    if (!h || h->id == id) return;
    unlink(h);
    if (!id) return;
    Resource *r = resolve(id, base);
    if (!r) return;
    h->target = r;
    h->id = id;  // the exe stores the id it was set to, not the record's local id
    h->next = r->handles;
    if (r->handles) r->handles->prev = h;
    r->handles = h;
    r->refs++;
}

Runtime::Resource *Runtime::instantiate(Resource *type, Resource *parent,
                                        const std::string &alias,
                                        const std::string &params, bool scripted,
                                        std::string *err)
{
    (void)params;  // constructor params are script data; no ctor thread is run
    auto up = std::make_unique<Resource>();
    Resource *r = up.get();
    instances_.push_back(std::move(up));
    r->id = next_instance_id_++;
    r->type = 1;  // ins_game
    r->name = alias.empty() ? "_gameinst" : alias;
    r->instance = true;
    if (type) r->script_class = type->script_class;
    r->parent = parent;
    r->parent_id = parent ? parent->id : 0;
    if (parent) {
        parent->children.push_back(r);
        for (size_t i = 0; i < parent->children.size(); i++)
            parent->children[i]->next_sibling =
                i + 1 < parent->children.size() ? parent->children[i + 1] : nullptr;
        if (parent->instance) r->world_root = parent->world_root ? parent->world_root : parent;
    }
    if (scripted) r->script = script_instance(r);
    (void)err;
    return r;
}

void Runtime::reparent(Resource *r, Resource *parent)
{
    if (!r || r->parent == parent) return;
    Resource *old = r->parent;
    if (old) {
        for (size_t i = 0; i < old->children.size(); i++) {
            if (old->children[i] != r) continue;
            old->children.erase(old->children.begin() + (long)i);
            break;
        }
        for (size_t i = 0; i < old->children.size(); i++)
            old->children[i]->next_sibling =
                i + 1 < old->children.size() ? old->children[i + 1] : nullptr;
    }
    r->parent = parent;
    r->parent_id = parent ? parent->id : 0;
    r->world_root = parent && parent->instance
                        ? (parent->world_root ? parent->world_root : parent)
                        : nullptr;
    if (parent) {
        parent->children.push_back(r);
        for (size_t i = 0; i < parent->children.size(); i++)
            parent->children[i]->next_sibling =
                i + 1 < parent->children.size() ? parent->children[i + 1] : nullptr;
    }
}

Object *Runtime::script_instance(Resource *r)
{
    if (!r) return nullptr;
    if (r->script) return r->script;
    if (!vm_ || r->script_class.empty()) return nullptr;
    ClassState *cs = vm_->find_class(r->script_class);
    if (!cs) return nullptr;
    std::string err;
    // The exe spawns the script constructor in a THRD-CREATE thread; the
    // rebuild creates the instance with default fields and does not run it.
    // shortcut: no constructor thread, upgrade when the script scheduler owns
    // game-object creation.
    Object *o = vm_->new_object(cs, &err);
    if (!o) return nullptr;
    r->script = o;
    return o;
}

// 0x47dd70: resolve `name` on self and run it to completion (the exe spawns a
// THRD-RUN thread with a zero time slice). Missing method / unfinished thread
// means failure.
bool Runtime::call_method(Object *self, const std::string &name,
                          const std::vector<Value> &args, Value *result,
                          std::string *err)
{
    if (!vm_ || !self) return false;
    ClassState *decl = nullptr;
    const MethodInfo *m = vm_->find_method(self->cls, name, "", (int)args.size(),
                                           args, &decl, err);
    const ClassUnit *unit = class_state_unit(decl);
    if (!m || !unit) return false;
    Value tmp;
    return vm_->invoke(*unit, *m, self, args, result ? result : &tmp, err);
}

// 0x47c760 Game::queueEvent: synchronous dispatch. The target's script handler
// runs first, then matching notifications forward to their watcher; one-shot
// entries are removed after forwarding.
void Runtime::queue_event(Resource *target, Resource *sender, uint32_t code,
                          const Value &param, const char *method)
{
    if (!target) return;
    // shortcut: no native type handlers (typed+0x4c) or streaming checks;
    // upgrade when engine types exist.
    if ((target->event_mask & code) && target->script)
        dispatch_script(*this, target, sender, code, param, method);

    // INVENTED: snapshot iteration; the exe walks the live list.
    std::vector<Resource::Notification> list = target->notifications;
    auto drop = [target](const Resource::Notification &n) {
        for (size_t k = 0; k < target->notifications.size(); k++) {
            const Resource::Notification &x = target->notifications[k];
            if (x.watcher == n.watcher && x.mask == n.mask && x.alias == n.alias) {
                target->notifications.erase(target->notifications.begin() + (long)k);
                break;
            }
        }
    };
    for (size_t i = 0; i < list.size(); i++) {
        const Resource::Notification &n = list[i];
        // INVENTED: a null watcher never happens (no destroy hook) but the exe
        // drops such entries; keep the check.
        if (!n.watcher) {
            drop(n);
            continue;
        }
        if ((code & n.mask) == 0) continue;
        uint32_t c = (n.alias & 0x0fffffffu) ? n.alias : code;
        Value p = (n.alias & 0x10000000u)
                      ? (n.has_message ? Value::make_str(n.message) : Value::make_null())
                      : param;
        if (!(sender && (n.mask & 0x40000000u)))
            queue_event(n.watcher, sender ? sender : target, c & 0x0fffffffu, p,
                        n.has_method ? n.method.c_str() : nullptr);
        if (n.mask & 0x80000000u) drop(n);
    }
}

Object *Runtime::wrap(Resource *r, std::string *err)
{
    if (!r) return nullptr;
    if (!vm_) {
        *err = "wrapper: no script VM";
        return nullptr;
    }
    const char *cls = "java.util.resource.ResourceRef";
    if (r->type == 1 || r->type == 8)
        cls = "java.util.resource.GameRef";
    else if (r->type == 3 || r->type == 0x0e)
        cls = "java.util.resource.RenderRef";
    ClassState *cs = vm_->find_class(cls);
    if (!cs) {
        *err = std::string("wrapper: ") + cls + " is not loaded";
        return nullptr;
    }
    Object *o = vm_->new_object(cs, err);
    if (!o) return nullptr;
    bind(attach(o), r);
    return o;
}

bool Runtime::class_is_a(const std::string &cls, const std::string &base)
{
    std::string c = cls;
    for (int guard = 0; guard < 64 && !c.empty(); guard++) {
        if (c == base) return true;
        if (!classes_) return false;
        ClassEntry *e = classes_->find(c);
        if (!e || e->super_name.empty()) return false;
        c = e->super_name;
    }
    return false;
}
