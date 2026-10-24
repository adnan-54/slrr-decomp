// Game runtime object/id layer. See game/runtime.h and notes/06, notes/13,
// notes/16. Semantics of handles follow the exe's 0x10-byte ref nodes and the
// natives in classdump --selfcheck exercise both layers.
#include "game/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/car.h"
#include "physics/mesh.h"
#include "physics/physics.h"
#include "platform/fs.h"
#include "script/natives.h"
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

// 0x527140 Str__vscan rules for type_ground's params format
// "%x,%f,%f,%f,%f,%f,%f,%s,%s": %x skips whitespace and reads hex (0x prefix
// accepted), %f skips whitespace and reads a float, %s skips whitespace then
// copies until whitespace, NUL or the next format character (','); a literal
// must match and the scan stops at the first mismatch.
// INVENTED: strtof stands in for the untraced 0x527530 float reader.
static bool scan_ws(char c)
{
    return c == ' ' || (c >= 9 && c <= 13);
}

static void scan_skip_ws(const std::string &s, size_t *i)
{
    while (*i < s.size() && scan_ws(s[*i])) (*i)++;
}

static bool scan_hex(const std::string &s, size_t *i, uint32_t *out)
{
    scan_skip_ws(s, i);
    size_t p = *i;
    if (p + 1 < s.size() && s[p] == '0' && (s[p + 1] == 'x' || s[p + 1] == 'X'))
        p += 2;
    size_t start = p;
    uint32_t v = 0;
    for (; p < s.size(); p++) {
        char c = s[p];
        if (c >= '0' && c <= '9') v = v * 16 + (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (uint32_t)(c - 'A' + 10);
        else break;
    }
    if (p == start) return false;
    *i = p;
    *out = v;
    return true;
}

static bool scan_float(const std::string &s, size_t *i, float *out)
{
    scan_skip_ws(s, i);
    const char *p = s.c_str() + *i;
    char *end = nullptr;
    float v = std::strtof(p, &end);
    if (end == p) return false;
    *out = v;
    *i += (size_t)(end - p);
    return true;
}

static bool scan_str(const std::string &s, size_t *i, char stop, std::string *out)
{
    scan_skip_ws(s, i);
    size_t start = *i;
    while (*i < s.size() && *i - start < 0x400) {
        char c = s[*i];
        if (scan_ws(c) || c == '\0' || c == stop) break;
        (*i)++;
    }
    if (*i == start) return false;
    *out = s.substr(start, *i - start);
    return true;
}

static bool scan_char(const std::string &s, size_t *i, char c)
{
    if (*i >= s.size() || s[*i] != c) return false;
    (*i)++;
    return true;
}

// 0x4bade0 type_ground vf3 create: same format; only the last %s (the .trc
// path) is kept. A failed conversion returns early and leaves trc empty.
static bool scan_ground_params(const std::string &s, std::string *trc)
{
    size_t i = 0;
    uint32_t id = 0;
    float f = 0;
    if (!scan_hex(s, &i, &id)) return false;
    for (int k = 0; k < 6; k++)
        if (!scan_char(s, &i, ',') || !scan_float(s, &i, &f)) return false;
    std::string scm;
    if (!scan_char(s, &i, ',') || !scan_str(s, &i, ',', &scm)) return false;
    if (!scan_char(s, &i, ',') || !scan_str(s, &i, '\0', trc)) return false;
    return true;
}

// The type-1 record's data is an instance blob; its RSD chunk carries the
// descriptor lines `gametype <id>` and `params <text>` (0x519940).
static bool ground_rsd(Runtime::Resource *r, uint32_t *gametype, std::string *params)
{
    if (!r->package || !r->record) return false;
    RpkInstanceData inst;
    std::string err;
    if (!r->package->rpk.parse_instance(*r->record, &inst, &err)) return false;
    bool has_type = false;
    const std::string &data = inst.rsd;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = pos;
        while (end < data.size() && data[end] != '\n') end++;
        std::string line(data, pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t k = 0;
        while (k < line.size() && !scan_ws(line[k])) k++;
        std::string key = line.substr(0, k);
        while (k < line.size() && scan_ws(line[k])) k++;
        if (key == "gametype") {
            *gametype = (uint32_t)std::strtoul(line.c_str() + k, nullptr, 16);
            has_type = true;
        } else if (key == "params") {
            *params = line.substr(k);
        }
        pos = end + 1;
    }
    return has_type;
}

// The record's gametype chain names the `native ground` type.
static bool native_ground_type(Runtime &rt, uint32_t gametype, const Package *base)
{
    Runtime::Resource *ty = rt.resolve(gametype, base);
    if (!ty || !ty->package || !ty->record) return false;
    for (const DataRef &ref : rt.resources().refs(*ty->package, *ty->record))
        if (ref.kind == DataRef::Native && ref.native == "ground") return true;
    // shortcut: a scripted type's getInfo (0x47dd50) is not modelled.
    return false;
}

// 0x519940 / 0x4ff6b0: the map record and every embedded type-1 child carry a
// PHYS triangle soup in world-frame metres (notes/19 §7). The exe walks the
// resident nodes per body (Collide__groundTree 0x4fe6f0); src merges them.
static void gather_ground_soup(const RpkArchive &rpk, const RpkRecord &rec,
                               std::vector<Vec3> *verts,
                               std::vector<CollisionMesh::Triangle> *tris)
{
    RpkInstanceData inst;
    std::string err;
    if (!rpk.parse_instance(rec, &inst, &err)) return;
    uint32_t base = static_cast<uint32_t>(verts->size());
    for (size_t i = 0; i + 2 < inst.verts.size(); i += 3)
        verts->push_back(vec3(inst.verts[i], inst.verts[i + 1], inst.verts[i + 2]));
    for (const RpkPhysFace &f : inst.faces)
        tris->push_back(
            CollisionMesh::Triangle{{base + f.v[0], base + f.v[1], base + f.v[2]}});
    for (const RpkRecord &child : inst.children)
        if (child.type == 1) gather_ground_soup(rpk, child, verts, tris);
}

// 0x4bade0 type_ground vf3, body part: mass-0 kind-0 box with flag 4 (static,
// no integration) added to the process world and kept on the resource.
// INVENTED timing: the exe builds it when the map node becomes resident
// (Collide__groundTree 0x4fe6f0 requestLoad); src builds it once at mount.
static void create_ground_body(Runtime &rt, Runtime::Resource *r)
{
    if (r->ground_body) return;
    uint32_t gametype = 0;
    std::string params;
    if (!ground_rsd(r, &gametype, &params)) return;
    if (!native_ground_type(rt, gametype, r->package)) return;

    // params "%x,%f,%f,%f,...": floats 1..3 are the body position.
    // INVENTED: a failed scan leaves the remaining fields at 0.
    size_t i = 0;
    uint32_t id = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    scan_hex(params, &i, &id);
    scan_char(params, &i, ',');
    scan_float(params, &i, &x);
    scan_char(params, &i, ',');
    scan_float(params, &i, &y);
    scan_char(params, &i, ',');
    scan_float(params, &i, &z);

    World &w = process_world();
    // 0x47c080 Config("gravity") / Config("ground_friction").
    // INVENTED: Config.java's 9.81 / 1.0 when the statics are not loaded.
    float gravity = 9.81f, friction = 1.0f;
    if (rt.vm()) {
        Value v;
        std::string e;
        if (rt.vm()->get_static_field("java.util.Config", "gravity", &v, &e) &&
            v.kind == Value::Float)
            gravity = v.f;
        if (rt.vm()->get_static_field("java.util.Config", "ground_friction", &v, &e) &&
            v.kind == Value::Float)
            friction = v.f;
    }
    if (-gravity <= 0.0f) w.gravity = vec3(0.0f, -gravity, 0.0f);
    if (friction <= 0.0f) friction = 1.0f;

    auto b = std::make_unique<RigidBody>();
    b->def.flags |= 4u;                      // static, no integration
    b->def.shape.type = SHAPE_NONE;          // Physics__createBox(0, 0, ...) base
    b->def.shape.friction = friction;        // shape +0x50
    b->def.shape.rolling_resistance = 0.01f; // shape +0x54
    b->def.shape.restitution = 0.2f;         // shape +0x58
    b->pos = vec3(x, y, z);
    // INVENTED: one merged collider built at create instead of the exe's
    // per-node streaming walk (Collide__groundTree 0x4fe6f0) and its kind-0
    // groundbody + 0x4feb20/0x4fc7e0 TOI narrow phase.
    // shortcut: no streaming/unload of map nodes, upgrade when 0x4feb20 /
    // 0x4fd110 / 0x4fc7e0 are traced.
    std::vector<Vec3> verts;
    std::vector<CollisionMesh::Triangle> tris;
    if (r->package && r->record)
        gather_ground_soup(r->package->rpk, *r->record, &verts, &tris);
    b->def.shape.mesh = build_collision_mesh(verts, tris);
    if (b->def.shape.mesh) b->def.shape.type = SHAPE_MESH;
    w.add_body(b.get());
    r->ground_body = std::move(b);
}

// 0x4bade0 type_ground vf3 create: check the type is a native ground, then
// load the named .trc (File__readAll 0x565610, Traffic__load 0x41a000).
// INVENTED: lazy creation at the first lookup and the failure log; the exe
// creates the graph when the instance is made resident and has no failure log.
static void load_ground(Runtime &rt, Runtime::Resource *r)
{
    uint32_t gametype = 0;
    std::string params;
    if (!ground_rsd(r, &gametype, &params)) return;
    if (!native_ground_type(rt, gametype, r->package)) return;

    std::string trc;
    if (!scan_ground_params(params, &trc) || trc.empty()) return;
    std::string path = rt.resources().resolve_path(trc);
    if (path.empty()) return;
    std::vector<uint8_t> buf;
    std::string err;
    if (!fs::read_file(path, &buf, &err)) return;   // File__readAll < 0
    auto g = std::make_shared<traffic::Graph>();
    size_t used = 0;
    if (!g->load(buf.data(), buf.size(), &used, &err)) {
        std::fprintf(stderr, "ground: %s: %s\n", trc.c_str(), err.c_str());
        return;
    }
    // shortcut: Traffic__init 0x419da0 and the 0x47d670 registration are not
    // ported.
    r->traffic = g;
}

Runtime::Runtime(ResourceManager &resources, Vm *vm, ClassPath *classes)
    : resources_(&resources), vm_(vm), classes_(classes)
{
}

Runtime::~Runtime()
{
    if (active_ == this) {
        if (vm_) vm_->set_resource_resolver(nullptr);
        active_ = nullptr;
    }
    // INVENTED: the exe's Game dtor was not traced; unbind the timer handles so
    // the resources' ref lists do not outlive them.
    for (auto &t : timers_) unlink(&t->handle);
    timers_.clear();
    // INVENTED: Game dtor not traced; unbind callback handles like the timers.
    for (auto &c : callbacks_) unlink(&c->handle);
    callbacks_.clear();
    // INVENTED: same cleanup for the deferred queues (dtor not traced).
    for (auto &h : unload_q_) unlink(h.get());
    for (auto &h : delete_q_) unlink(h.get());
    unload_q_.clear();
    delete_q_.clear();
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
    // 0x5189e0 / 0x55cad0: a script resource constant becomes an int global id.
    // The exe rewrites the opcode at class load (0x54b310); resolving at
    // execution gives the same value unless registry order matters.
    if (vm_) {
        ResourceManager *rm = resources_;
        vm_->set_resource_resolver([rm](const std::string &pkg, int32_t local) -> int32_t {
            const Package *p = rm->registry_package(rm->registry_index(pkg));
            return p ? (int32_t)rm->global_id(*p, (uint32_t)local) : 0;
        });
    }
}

void Runtime::deactivate()
{
    if (active_ && active_->vm_) active_->vm_->set_resource_resolver(nullptr);
    active_ = nullptr;
}

// 0x51fcc0: a file ref (<dir>\Name.class) goes through 0x55aca0 -> 0x55bec0
// every time (the 0x55aca0 key is the directory path, which never matches a
// shipped class name), so the returned ClassState is the record's own.
// A ref that names the class directly stays a name lookup (name_out).
ClassState *Runtime::script_class_of(const Package &pkg, const RpkRecord &rec,
                                    std::string *name) const
{
    for (const DataRef &ref : resources_->refs(pkg, rec)) {
        if (ref.kind != DataRef::Script) continue;
        if (!ref.file) {
            *name = ref.text;  // class named directly by the record
            return nullptr;
        }
        // 0x51fcc0 -> 0x55aca0(dir, Name): the record text is <dir>\Name.class.
        size_t sep = ref.text.find_last_of("\\/");
        if (!path_class_loader || sep == std::string::npos) continue;
        std::string base = ref.text.substr(sep + 1);
        if (base.size() > 6 && base.compare(base.size() - 6, 6, ".class") == 0)
            base.resize(base.size() - 6);
        ClassState *cs = path_class_loader(ref.text.substr(0, sep), base);
        if (!cs) continue;
        const ClassUnit *unit = class_state_unit(cs);
        *name = unit ? unit->class_name(unit->self_class) : std::string();
        return cs;
    }
    return nullptr;
}

// 0x51fc50 (vf10 +0x28) / 0x51fcc0: the record's `script ...` ref is resolved
// once, on the first typed-data request (0x51f4f0 -> 0x518a80), never at
// package mount. INVENTED: src has no typed-data layer, so the deferred
// product is just the class; script_class_checked is the node+0x94 bit0 flag.
void Runtime::ensure_script_class(Resource *r)
{
    if (!r || r->script_class_checked) return;
    r->script_class_checked = true;   // mark first: script_class_of may load classes
    if (r->package && r->record)
        r->script_cls = script_class_of(*r->package, *r->record, &r->script_class);
}

// 0x51e310 Package::load / 0x518410 Resource__createInstance at mount: the
// exe mounts every record of a package in file order and tail-inserts each
// node into its parent's child list.
void Runtime::mount_package(const Package *p)
{
    if (!p) return;
    for (const Package *m : mounted_) if (m == p) return;
    mounted_.push_back(p);   // mark first: parent resolution re-enters resolve
    // 0x518410: every record of a package is mounted at load.
    // INVENTED: create all nodes first. The exe resolves the super id only
    // against already-registered nodes and logs "Unregistrable node" when it
    // is missing; src's resolve() creates a forward-referenced parent on
    // demand. Shipped packages list parents before children, so the result
    // is the same; only a malformed forward reference would differ.
    for (const RpkRecord &rec : p->rpk.records) resolve(rec.id, p);
    // 0x518410 tail insert: child order = record order in the .rpk.
    // shortcut: order across packages is mount-completion order, not the
    // exe's per-record interleaving inside 0x51e6b0 (not fully traced).
    for (const RpkRecord &rec : p->rpk.records) {
        Resource *r = resolve(rec.id, p);    // cached by the loop above
        if (!r) continue;
        if (r->parent) {
            std::vector<Resource *> &ch = r->parent->children;
            if (!ch.empty()) ch.back()->next_sibling = r;
            r->next_sibling = nullptr;
            ch.push_back(r);
        }
        if (rec.type == 1) create_ground_body(*this, r);
    }
}

Runtime::Resource *Runtime::resolve(uint32_t id, const Package *base)
{
    if (!id) return nullptr;
    if (base) {
        id = resources_->global_id(*base, id);   // 0x51e860
        if (!id) return nullptr;
    }
    // 0x51fa40 resolves through the RI registry, which also holds created
    // instances (0x8000... game instances, 0xffff... script resources).
    if ((id >> 16) == 0x8000u || (id >> 16) == 0xffffu)
        for (auto &up : instances_)
            if (up->id == id) return up.get();
    // 0x517b80: the high word is a registry slot, mounted on first reference.
    // 0x51e310: the mount links the package's records into their parents.
    const Package *target = resources_->registry_package(id >> 16);
    if (target) mount_package(target);
    const RpkRecord *rec = target ? target->rpk.find(id & 0xffffu) : nullptr;
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
    // 0x51e310 mount only creates/link nodes (0x518880 does 0x51f790(0)); the
    // record text is parsed on the first typed-data request (0x51f4f0 -> 0x518a80
    // -> vf10 0x51fc50 -> 0x51fcc0). See ensure_script_class().
    r->parent_id = rec->super_id;
    if (r->parent_id) r->parent = resolve(r->parent_id, target);
    return r;
}

// 0x47c640 Game__getInfo(h, 0x39) -> 0x47e450 -> 0x4bb570 vf15: the traffic
// graph (+0x88) of the ground's typed data; null when it has none.
// shortcut: the typeof-8 branch of 0x47c640 is not recovered; the graph is built
// lazily at the first lookup instead of at instance create (see load_ground).
traffic::Graph *Runtime::ground_traffic(Handle *h)
{
    if (!h || h->id == 0 || !h->target) return nullptr;
    Resource *r = h->target;
    if (r->type != 1) return nullptr;
    if (!r->ground_checked) {
        r->ground_checked = true;
        load_ground(*this, r);
    }
    return r->traffic.get();
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

// 0x525120 -> 0x5188e0: created engine resource, no record. Same owned-node
// allocation as instantiate (which keeps its ins_game / script / parent logic).
Runtime::Resource *Runtime::create_resource(uint32_t type, const std::string &name)
{
    auto up = std::make_unique<Resource>();
    Resource *r = up.get();
    instances_.push_back(std::move(up));
    r->id = next_instance_id_++;
    r->type = type;
    r->name = name.empty() ? "_gameinst" : name;
    r->instance = true;
    return r;
}

// 0x518410 -> 0x525b50 / 0x5263a0: node from a script descriptor. The exe keeps the
// buffer; src copies the text. +0x84 |= 0x200 is done by the create helpers.
Runtime::Resource *Runtime::define_script_resource(uint32_t type, const std::string &name,
                                                   const std::string &data, Resource *parent)
{
    auto up = std::make_unique<Resource>();
    Resource *r = up.get();
    instances_.push_back(std::move(up));
    r->id = 0xffff0000u | next_script_id_++;   // 0x518a40 high word (see UNRESOLVED)
    r->type = type;
    r->name = name;
    r->data = data;
    r->flags |= 0x200;
    r->instance = true;   // id prefix 0xffff; same stand-in as unload (runtime.cpp:630)
    if (parent) reparent(r, parent);
    return r;
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
    // A record node's runtime id (+0x80) is the global registry<<16 | local id
    // (0x51e860, notes/16); src keeps record ids local, so convert here.
    h->id = r->record && r->package ? resources_->global_id(*r->package, r->id) : r->id;
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
    auto up = std::make_unique<Resource>();
    Resource *r = up.get();
    instances_.push_back(std::move(up));
    r->id = next_instance_id_++;
    r->type = 1;  // ins_game
    r->name = alias.empty() ? "_gameinst" : alias;
    r->instance = true;
    // 0x523c80: create fetches the type's typed data (classes) before the ctor.
    if (type) {
        ensure_script_class(type);
        r->script_class = type->script_class;
        r->script_cls = type->script_cls;
    }
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
    // 0x523c80: the type record's native manager vf3 runs before the script
    // <init>. Only `native car` (Vehicle__createCar 0x4992e0) builds bodies in
    // src; create_car_bodies is a no-op for every other native kind.
    // shortcut: other native type-manager kinds (part, camera, ...) are
    // not dispatched (0x523c80/0x5240e0).
    game::create_car_bodies(*this, r, type, params);
    // 0x523c80: a script class runs its <init> (signature "(I)" in this class
    // format, no return type) in a THRD-CREATE thread (prio 10,
    // flags 0) with the new instance id as the single int argument, untimed.
    // INVENTED: the exe binds the script object's native handle first (0x4ed320);
    // src resolves it lazily via handle(obj), so there is nothing to bind here.
    if (scripted && r->script && vm_) {
        std::vector<Value> args{Value::make_int((int32_t)r->id)};
        ClassState *decl = nullptr;
        std::string e;
        const MethodInfo *m =
            vm_->find_method(r->script->cls, "<init>", "(I)", 1, args, &decl, &e);
        const ClassUnit *unit = class_state_unit(decl);
        if (m && unit) {
            natives::spawn_untimed(*vm_,
                                   vm_->start(*unit, *m, r->script, args, &e), 10,
                                   "THRD-CREATE");
        } else {
            // 0x561700 Thread::callMethod: missing method -> log, no run.
            std::fprintf(stderr, "Thread::callMethod: \"<init>(I)\" not found\n");
        }
    }
    (void)err;
    return r;
}

// DAT_005fde50 game root ref: one node for the process. Name/type INVENTED.
Runtime::Resource *Runtime::game_root()
{
    if (!game_root_) game_root_ = instantiate(nullptr, nullptr, "GameRoot", "", false, nullptr);
    return game_root_;
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
    // 0x4daf90 getScriptInstance hits the same typed-data path before the class.
    ensure_script_class(r);
    if (!vm_) return nullptr;
    ClassState *cs = r->script_cls;
    if (!cs && !r->script_class.empty()) cs = vm_->find_class(r->script_class);
    if (!cs) return nullptr;
    std::string err;
    Object *o = vm_->new_object(cs, &err);
    if (!o) return nullptr;
    r->script = o;
    // INVENTED call site: the exe's typed+0x50 script instance refers back to
    // its node (ResourceRef natives resolve through it); C++-side creation
    // binds it here because script constructors do not always call set(id).
    bind(attach(o), r);
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
    // 0x47c760 step 2: the native type handler (typed+0x4c vf14) runs before
    // the script and a non-zero return suppresses handleEvent. The car and
    // native-player command handlers are ported.
    // shortcut: other native kinds and the streaming check (typed+0x4c == 0
    // path) are not modelled; upgrade when engine types exist.
    if (target->event_mask & code) {
        uint32_t ev = code & 0x0fffffffu;  // vf14 gets the masked code
        if (!game::car_command(*this, target, ev, param) &&
            !game::part_command(*this, target, ev, param) &&
            !game::player_command(*this, target, ev, param) && target->script)
            dispatch_script(*this, target, sender, code, param, method);
    }

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
        // INVENTED: a null watcher never happens but the exe drops such entries;
        // zeroed by destroy's handle clear (0x51f970). Keep the check.
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

// 0x47e340 Game__queueUnload / 0x47b970 Game__queueDelete: a ResourceRefDLNode
// (0x47b090) owns a handle bound to the would-be-unloaded resource's id.
void Runtime::queue_unload(Handle *h)
{
    auto n = std::make_unique<Handle>();
    bind(n.get(), h->target);
    unload_q_.push_back(std::move(n));
}

void Runtime::queue_delete(Handle *h)
{
    // 0x47b970 fetches typed data when the resource is a game instance; the
    // result is unused, so there is no src effect.
    auto n = std::make_unique<Handle>();
    bind(n.get(), h->target);
    delete_q_.push_back(std::move(n));
}

// 0x47d070 Game__flushDeferred: drain both lists until a pass does nothing.
void Runtime::flush_deferred()
{
    bool work;
    do {
        work = false;
        while (!unload_q_.empty()) {
            // Process before pop_front: script code run by queue_event may append.
            Handle *h = unload_q_.front().get();
            if (h->id) {
                Resource *r = h->target;
                if (r) {
                    if (r->type == 1) queue_event(r, nullptr, 2, Value::make_null());
                    unload(r);
                }
                unlink(h);
            }
            unload_q_.pop_front();
            work = true;
        }
        while (!delete_q_.empty()) {
            Handle *h = delete_q_.front().get();
            if (h->id) {
                Resource *r = h->target;
                if (r) {
                    if (r->type == 1) queue_event(r, nullptr, 2, Value::make_null());
                    unload(r);
                    destroy(r);  // vf0(1); D3 zeroes h, so unlink is a no-op
                }
                unlink(h);
            }
            delete_q_.pop_front();
            work = true;
        }
        // shortcut: generic list Game+0x1cc has no producer traced in src.
    } while (work);
    // shortcut: Game+0x234 -> 0x559f60 not traced; nothing to run.
}

// 0x51a1b0 unload, param 1 (type 1 game instance; other kinds carry no src state).
void Runtime::unload(Resource *r)
{
    if (!r || r->type != 1) return;
    // U1: game-instance children first (exe passes param|2).
    std::vector<Resource *> children = r->children;
    for (Resource *c : children)
        if (c->type == 1) unload(c);
    // U2: 0x54c600 unroots the script object then the slot is zeroed.
    // shortcut: the src VM has no root list; dropping the pointer is the whole effect.
    r->script = nullptr;
    // U3: delete every notification on the instance.
    r->notifications.clear();
    // U4: destroy children of the same package or runtime-created ones. The exe
    // tests id>>16 equal or 0xffff; src instance ids are 0x80000000+, so
    // `instance` stands in for the runtime-created 0xffff prefix.
    for (Resource *c : children)
        if (c->instance || (c->package && c->package == r->package)) destroy(c);
    // shortcut: +0x154 native instance dtor, "Undefined GameType Destructor"
    // log, arrays +0x11c/+0x124/+0x13c/+0x150, handles +0x128/+0x140 and the
    // loaded bit (0x51f790) have no src counterpart.
}

// vf0(1) common part: 0x519500 / 0x51b5a0 / 0x526bd0 -> 0x51ebe0 -> 0x569890.
void Runtime::destroy(Resource *r)
{
    if (!r) return;
    // D1: the class dtor runs its unload again (idempotent).
    unload(r);
    // D3: 0x51f970 ResourceNode__clearRefs; every bound handle is zeroed.
    for (Handle *h = r->handles; h;) {
        Handle *nx = h->next;
        h->target = nullptr;
        h->id = 0;
        h->prev = h->next = nullptr;
        h = nx;
    }
    r->handles = nullptr;
    r->refs = 0;
    // src keeps the watcher as a raw Resource* where the exe holds a handle that
    // D3 just zeroed; drop those references here.
    for (auto &kv : records_)
        for (auto &n : kv.second->notifications)
            if (n.watcher == r) n.watcher = nullptr;
    for (auto &up : instances_)
        for (auto &n : up->notifications)
            if (n.watcher == r) n.watcher = nullptr;
    // D4: 0x569890 TreeNode__dtor destroys every child recursively.
    while (!r->children.empty()) destroy(r->children.front());
    // D5: 0x569b70 TListNode__dtor leaves the parent's child list.
    reparent(r, nullptr);
    // shortcut: D2 (0x5173e0 id recycling) and D6 (memory free) are not
    // modelled; the node stays in instances_/records_ because side tables keyed
    // by Resource* (g_parts/g_cars, the controller map) would dangle. Upgrade
    // when those tables get a destroy hook.
}

// 0x4e7a60 Handle::addTimer(due, flags, param, method), ECX = handle.
// 0x47b090 binds the new node's handle to the given handle's resource.
bool Runtime::add_timer(Handle *h, float due, uint32_t flags, int32_t param)
{
    if (!h || h->id == 0) return false;
    auto up = std::make_unique<Timer>();
    Timer *t = up.get();
    bind(&t->handle, h->target);
    t->due = due;
    t->flags = flags;
    t->param = param;
    timers_.push_back(std::move(up));
    return true;
}

// 0x4e93c0: unbind the timers watching h's id. Nodes stay in the list; the
// timer pass deletes them.
void Runtime::remove_timers(Handle *h)
{
    if (!h || h->id == 0) return;
    for (auto &t : timers_)
        if (t->handle.id == h->id) unlink(&t->handle);
}

// 0x47b700 Game::runTimers: fire due timers as events. next is read before
// dispatch, so a timer appended while the last node fires waits a pass.
void Runtime::run_timers(float now)
{
    auto it = timers_.begin();
    while (it != timers_.end()) {
        auto nx = std::next(it);
        bool last = nx == timers_.end();
        Timer *t = it->get();
        if (t->handle.id == 0) {
            timers_.erase(it);
        } else if (t->due <= now) {
            queue_event(t->handle.target, nullptr, t->flags & 0x0fffffffu,
                        Value::make_int(t->param));
            if ((int32_t)t->flags < 0) {  // sign bit = one-shot
                unlink(&t->handle);  // the node's handle must leave the target's ref list
                timers_.erase(it);
            }
            else
                t->due = t->period + t->due;
        }
        if (last) break;
        it = nx;
    }
}

// 0x47d670 Game__registerCallback (ECX = Game; ret 8): append a
// ResourceRefDLNode bound to h->target to the mode's list.
void Runtime::register_callback(Handle *h, int mode)
{
    // INVENTED: 0x47b090 dereferences h (+0xc); a null handle crashes the exe.
    if (!h) return;
    if (mode != 8 && mode != 9 && mode != 0x1c) return;   // exe default
    auto cb = std::make_unique<Callback>();
    cb->mode = mode;
    bind(&cb->handle, h->target);                          // 0x47b090 -> 0x40cac0
    callbacks_.push_back(std::move(cb));                   // append at the tail
}

// 0x47e390 Game__unregisterCallback (ECX = Game; ret 8): first node of that
// mode whose handle id equals h->id; 0x488080 unlinks its handle only.
void Runtime::unregister_callback(Handle *h, int mode)
{
    if (!h) return;                                        // INVENTED (exe reads h+8)
    for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
        Callback *cb = it->get();
        if (cb->mode != mode || cb->handle.id != h->id) continue;
        unlink(&cb->handle);                               // 0x488080
        // shortcut: the consumers 0x47ba10/0x47bc20/0x47be30 that reap id-0
        // nodes are not ported; drop the dead node here to bound the list.
        callbacks_.erase(it);
        return;
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
