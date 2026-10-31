// Game runtime object/id layer. See game/runtime.h and notes/06, notes/13,
// notes/16. Semantics of handles follow the exe's 0x10-byte ref nodes and the
// natives in classdump --selfcheck exercise both layers.
#include "game/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/car.h"
#include "game/cursor.h"
#include "physics/mesh.h"
#include "physics/physics.h"
#include "platform/fs.h"
#include "render/device.h"
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
    // 0x20/0x40 params arrive as the String built by the trigger callback
    // (0x47c960: "%d" of the other side's id); other codes pass theirs through.
    rt.call_method(script, method, {ref, Value::make_int((int32_t)ev), param},
                   nullptr, &err);
}

// 0x527140 Str__vscan rules for type_ground's params format
// "%x,%f,%f,%f,%f,%f,%f,%s,%s": %x skips whitespace and reads hex (the 0x
// prefix is required, 0x527490), %f skips whitespace and reads a float, %s
// skips whitespace then copies until whitespace, NUL or the next format
// character (','); a literal must match and the scan stops at the first
// mismatch.
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
    // 0x527490: the 0x/0X prefix is required; bare "FF" and "0x" alone fail.
    if (p + 1 >= s.size() || s[p] != '0' || (s[p + 1] != 'x' && s[p + 1] != 'X'))
        return false;
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

// The record's gametype chain names the native `name` handler.
static bool native_type_is(Runtime &rt, uint32_t gametype, const Package *base,
                           const char *name)
{
    Runtime::Resource *ty = rt.resolve(gametype, base);
    if (!ty || !ty->package || !ty->record) return false;
    for (const DataRef &ref : rt.resources().refs(*ty->package, *ty->record))
        if (ref.kind == DataRef::Native && ref.native == name) return true;
    // shortcut: a scripted type's getInfo (0x47dd50) is not modelled.
    return false;
}

// 0x519940 / 0x4ff6b0: the map record and every embedded type-1 child carry a
// PHYS triangle soup in world-frame metres (notes/19 §7). The exe walks the
// resident nodes per body (Collide__groundTree 0x4fe6f0); src merges them but
// keeps each node's bounds box (0x51e6b0), face range and RSD object state, in
// walk order.
static void gather_ground_soup(const RpkArchive &rpk, const RpkRecord &rec,
                               std::vector<Vec3> *verts,
                               std::vector<CollisionMesh::Triangle> *tris,
                               std::vector<Vec3> *normals,
                               std::vector<CollisionMesh::MapNode> *nodes,
                               std::vector<Runtime::Resource::MapObject> *objs)
{
    RpkInstanceData inst;
    std::string err;
    if (!rpk.parse_instance(rec, &inst, &err)) return;
    uint32_t base = static_cast<uint32_t>(verts->size());
    for (size_t i = 0; i + 2 < inst.verts.size(); i += 3)
        verts->push_back(vec3(inst.verts[i], inst.verts[i + 1], inst.verts[i + 2]));
    CollisionMesh::MapNode node;
    node.first = static_cast<uint32_t>(tris->size());
    for (const RpkPhysFace &f : inst.faces) {
        tris->push_back(
            CollisionMesh::Triangle{{base + f.v[0], base + f.v[1], base + f.v[2]}});
        // 0x4fd110 tests the face against the stored PHYS normal, so keep it.
        normals->push_back(vec3(f.n[0], f.n[1], f.n[2]));
    }
    node.count = static_cast<uint32_t>(tris->size()) - node.first;
    if (rec.has_extra) {
        // 0x51e6b0: extra f32[0..2] centre, f32[3..5] half; file is little-endian.
        float box[6];
        memcpy(box, rec.extra, sizeof box);
        node.centre = vec3(box[0], box[1], box[2]);
        node.half = vec3(box[3], box[4], box[5]);
    } else {
        // INVENTED: half = 1e30 (never culled); no shipped PHYS node lacks the
        // block (notes/24).
        node.half = vec3(1e30f, 1e30f, 1e30f);
    }
    const size_t idx = nodes->size();
    // Same push site as the node: the two vectors stay index-aligned.
    Runtime::Resource::MapObject obj;
    obj.record = rec.id;
    rpk_rsd_fields(inst.rsd, &obj.gametype, &obj.params);
    objs->push_back(std::move(obj));
    nodes->push_back(node);
    for (const RpkRecord &child : inst.children)
        if (child.type == 1)
            gather_ground_soup(rpk, child, verts, tris, normals, nodes, objs);
    // Subtree range + union box (speed only: a sphere that misses the union
    // misses every node in it, so the 0x4feb20 per-node result is unchanged).
    CollisionMesh::MapNode &n = (*nodes)[idx];
    n.end = static_cast<uint32_t>(nodes->size());
    Vec3 lo = n.centre - n.half, hi = n.centre + n.half;
    for (size_t k = idx + 1; k < n.end; k++) {
        const CollisionMesh::MapNode &d = (*nodes)[k];
        lo = vec3(fminf(lo.x, d.centre.x - d.half.x), fminf(lo.y, d.centre.y - d.half.y),
                  fminf(lo.z, d.centre.z - d.half.z));
        hi = vec3(fmaxf(hi.x, d.centre.x + d.half.x), fmaxf(hi.y, d.centre.y + d.half.y),
                  fmaxf(hi.z, d.centre.z + d.half.z));
    }
    n.sub_centre = (lo + hi) * 0.5f;
    n.sub_half = (hi - lo) * 0.5f;
}

// 0x4bade0 type_ground vf3, body part: mass-0 kind-0 box with flag 4 (static,
// no integration) added to the process world and kept on the resource.
// INVENTED timing: the exe builds it when the map node becomes resident
// (0x5195a0 load -> vf3); src builds it when a script handle first binds the
// record (Runtime::bind), not at mount: mounting every map package at once
// would stack all their soups at the origin.
static void create_ground_body(Runtime &rt, Runtime::Resource *r)
{
    if (r->ground_body) return;
    uint32_t gametype = 0;
    std::string params;
    if (!ground_rsd(r, &gametype, &params)) return;
    if (!native_type_is(rt, gametype, r->package, "ground")) return;

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
    b->owner = r;              // body+0x38 ResourceRef target, as the trigger body (0x4d4700)
    // INVENTED: one merged collider: faces merged in groundTree walk order
    // (0x4fe6f0) with the node table (cards 207/208). Streaming/unload not
    // modelled: for cars the exe loads nodes synchronously (0x518a80 prio 0,
    // notes/24), so only residency differs.
    // shortcut: no streaming/unload of map nodes, upgrade when 0x4feb20 /
    // 0x4fd110 / 0x4fc7e0 are traced.
    std::vector<Vec3> verts;
    std::vector<CollisionMesh::Triangle> tris;
    std::vector<Vec3> normals;
    std::vector<CollisionMesh::MapNode> nodes;
    r->map_objects.clear();
    if (r->package && r->record)
        gather_ground_soup(r->package->rpk, *r->record, &verts, &tris, &normals, &nodes,
                           &r->map_objects);
    b->def.shape.mesh = build_collision_mesh(verts, tris, normals, nodes);
    if (b->def.shape.mesh) b->def.shape.type = SHAPE_MESH;
    w.add_body(b.get());
    r->ground_body = std::move(b);
}

// 0x4fe6f0 residency + 0x5195a0 create (type_road__vf3 0x4d42a0), then the
// 0x4fe852 keep-alive (type_road__vf5 0x4d44d0 with f = 1.0 -> node vf1).
void Runtime::map_node_touched(Resource *ground, size_t ni)
{
    if (!ground || ni >= ground->map_objects.size()) return;
    Resource::MapObject &m = ground->map_objects[ni];
    if (!m.created) {
        if (!m.gametype) {
            m.created = true;                       // 0x5195a0: no gametype, no create
        } else if (!native_type_is(*this, m.gametype, ground->package, "road")) {
            m.created = true;   // shortcut: only native road objects are created; walk/scripted
                                // map gametypes (notes/31 §1) need their own create, port then.
        } else if (!ground->traffic) {
            return;             // INVENTED: src builds the graph lazily (load_ground);
                                // retry until it exists instead of binding 0 forever.
        } else {
            m.created = true;
            m.traffic_node = ground->traffic->bind_object(m.params, m.record);
        }
    }
    if (!m.traffic_node) return;
    traffic::Graph &g = *ground->traffic;
    uint32_t base = static_cast<uint32_t>(g.cross_eq.size() + g.cross_light.size());
    if (m.traffic_node > base) g.keep_road_live(m.traffic_node - 1 - base);  // ctRoad vf1 0x40f8e0
    // cross node: vf1 0x40e380 only reads the refcount -> nothing
}

// INVENTED HARNESS: the exe keeps a road's lanes live from the camera LOD walk
// (0x45f180 -> 0x461980 -> 0x4d44d0 -> lane vf7 0x40eed0); src has no render
// walk, so touch every resident `native road` map object once.
size_t Runtime::touch_road_nodes(Resource *ground)
{
    size_t n = 0;
    if (!ground) return 0;
    for (size_t i = 0; i < ground->map_objects.size(); i++) {
        const Resource::MapObject &m = ground->map_objects[i];
        if (!m.gametype || !native_type_is(*this, m.gametype, ground->package, "road"))
            continue;
        map_node_touched(ground, i);
        n++;
    }
    return n;
}

// The type record's refs name the `native trigger` handler (vtable 0x5a00e0).
static bool native_trigger_type(Runtime &rt, Runtime::Resource *ty)
{
    if (!ty || !ty->package || !ty->record) return false;
    for (const DataRef &ref : rt.resources().refs(*ty->package, *ty->record))
        if (ref.kind == DataRef::Native && ref.native == "trigger") return true;
    return false;
}

// 0x4d4700 type_trigger vf3 create: a mass-0 sensor body from the params
// "px,py,pz,yaw,pitch,roll,kind,sx,sy,sz"; the script's Trigger.java sends a
// sphere (3-arg ctor) or a box (4-arg ctor).
static void create_trigger_body(Runtime &rt, Runtime::Resource *r,
                                Runtime::Resource *type, const std::string &params)
{
    if (r->trigger_body || !native_trigger_type(rt, type)) return;
    // Defaults and scan order from 0x4d4700: sx 10, sy/sz 0, pos/ypr 0, kind
    // empty; the scan stops at the first mismatch, keeping what it parsed.
    float pos[3] = {0.0f, 0.0f, 0.0f}, ypr[3] = {0.0f, 0.0f, 0.0f};
    float sx = 10.0f, sy = 0.0f, sz = 0.0f;
    std::string kind;
    size_t i = 0;
    bool ok = true;
    for (int k = 0; k < 3 && ok; k++)
        ok = scan_float(params, &i, &pos[k]) && scan_char(params, &i, ',');
    for (int k = 0; k < 3 && ok; k++)
        ok = scan_float(params, &i, &ypr[k]) && scan_char(params, &i, ',');
    if (ok) ok = scan_str(params, &i, ',', &kind) && scan_char(params, &i, ',');
    if (ok) ok = scan_float(params, &i, &sx);
    if (ok) ok = scan_char(params, &i, ',') && scan_float(params, &i, &sy);
    if (ok) ok = scan_char(params, &i, ',') && scan_float(params, &i, &sz);
    (void)ok;

    auto b = std::make_unique<RigidBody>();
    if (kind.empty() || kind == "sphere") {
        b->set_sphere(sx, 0.0f);             // createBox(0, 1, sx, 0, 0, 0)
    } else if (kind == "box") {
        b->set_box(Vec3{sx, sy, sz}, 0.0f);  // createBox(0, 5, sx, sy, sz, 0)
    } else if (kind == "rod") {
        // createBox(0, 4, sx, sy, sz, 0): capsule along z: sx half length,
        // sy radius (notes/11 §1).
        b->set_capsule(2, sx, sy, 0.0f);
    } else {
        // shortcut: kind 6 named .scx hull (createBox(0, 6, 1, 1, 1, kind)) is
        // not ported; no shipped Track/Gamemode passes a named kind.
        std::fprintf(stderr, "trigger: named hull \"%s\" not supported\n", kind.c_str());
        return;
    }
    b->def.flags = (b->def.flags | 0x20u) & ~4u; // not the flag-4 static kind
    b->body_flags = 0x15040u;  // body+0x58: 0x40 sensor, 0x1000 keep records
    b->owner_id = r->id;       // body+0x40: id the EVENT_TRIGGER parameter reports
    b->owner = r;              // body+0x38 ResourceRef target
    // shortcut: the exe composes the full parent pose in 0x4e8a50; the shipped
    // map instance sits at the origin, so the params pos is the world pos here.
    if (r->parent) b->pos = Vec3{r->parent->pos[0], r->parent->pos[1], r->parent->pos[2]};
    b->pos += Vec3{pos[0], pos[1], pos[2]};
    Mat34 m;
    euler_to_mat(&m, ypr);  // Math__eulerToMat 0x5666d0
    mat_to_quat(&b->rot, &m);
    process_world().add_body(b.get());
    r->trigger_body = std::move(b);
}

// 0x4d4af0 type_trigger vf7 (params "fffffff" = x,y,z,yaw,pitch,roll,r, scan
// 0x56a1a0): place the sensor, resize a sphere when r > 0, clear velocities.
void move_trigger(Runtime::Resource *r, const std::string &params)
{
    if (!r || !r->trigger_body) return;
    RigidBody &b = *r->trigger_body;
    float pos[3] = {0.0f, 0.0f, 0.0f}, ypr[3] = {0.0f, 0.0f, 0.0f}, rad = -1.0f;
    size_t i = 0;
    bool ok = true;
    for (int k = 0; k < 3 && ok; k++)
        ok = scan_float(params, &i, &pos[k]) && scan_char(params, &i, ',');
    for (int k = 0; k < 3 && ok; k++)
        ok = scan_float(params, &i, &ypr[k]) && scan_char(params, &i, ',');
    if (ok) scan_float(params, &i, &rad);
    Mat34 m;
    euler_to_mat(&m, ypr);  // Math__eulerToMat 0x5666d0
    if (rad > 0.0f && b.def.shape.type == SHAPE_SPHERE)
        b.def.shape.radius = rad;  // shape+0xb4 = r, +0x10 = r; bound +0xc = r+0.05 is bounding_radius()
    // 0x4eac50 places the body at pos with rotation R.
    // shortcut: pos is world-space here, as in create_trigger_body (no parent pose).
    b.pos = Vec3{pos[0], pos[1], pos[2]};
    mat_to_quat(&b.rot, &m);
    b.vel = Vec3{0.0f, 0.0f, 0.0f};              // state vel = 0
    b.angular_momentum = Vec3{0.0f, 0.0f, 0.0f}; // 0x506ce0(&zero)
    // INVENTED: the +0xa0 &= ~2 "moved" bit has no src field; nothing to clear.
}

// 0x4bade0 type_ground vf3 create: check the type is a native ground, then
// load the named .trc (File__readAll 0x565610, Traffic__load 0x41a000).
// INVENTED: lazy creation at the first lookup and the failure log; the exe
// creates the graph when the instance is made resident and has no failure log.
static void load_ground(Runtime &rt, Runtime::Resource *r, Runtime::Handle *h)
{
    uint32_t gametype = 0;
    std::string params;
    if (!ground_rsd(r, &gametype, &params)) return;
    if (!native_type_is(rt, gametype, r->package, "ground")) return;

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
    // 0x4bb2b8: Traffic__init(T, *0x5dadf0) right after the load.
    g->init(process_world().time);
    // 0x416000 Traffic__ctor tail: createScripted(game root, null type, "",
    // "CarContainer"), then +0x84 |= 0x10 and 0x51f7f0 makes it its own
    // world-tree root. INVENTED: the exe creates it in the ctor before the
    // load; src builds the graph lazily, so it goes here.
    Runtime::Resource *cc =
        rt.instantiate(nullptr, rt.game_root(), "CarContainer", "", true, nullptr);  // 0x523c80
    if (cc) {
        cc->flags |= 0x10;
        cc->world_root = cc;    // 0x51f7f0: obj+0xf0 link = obj
    }
    g->car_container = cc ? cc->id : 0;    // T+0x311c
    g->reparent = [&rt, r, cc](uint32_t inst, uint32_t parent) {
        Runtime::Resource *i = rt.resolve(inst);   // 0x4ec210 body: GameRef.setParent
        // INVENTED: road/cross objects are map record ids in src (no map node
        // instance, 0x5195a0 not ported); the ground instance r stands in.
        Runtime::Resource *p = cc && parent == cc->id ? cc : parent ? r : nullptr;
        if (i && p) rt.reparent(i, p);
    };
    r->traffic = g;
    // 0x4bb2e8 Game__registerCallback(ground, 0x1c): the exe does this at
    // instance create, src at the first handle lookup (see ground_traffic).
    rt.register_callback(h, 0x1c);
}

Runtime::Runtime(ResourceManager &resources, Vm *vm, ClassPath *classes)
    : resources_(&resources), vm_(vm), classes_(classes)
{
    // 0x502d60/0x4fb070 queue the trigger events to the bodies' owners; 0x47c960
    // formats the other side's id with "%d" into the param.
    process_world().trigger_event = [this](RigidBody *target, RigidBody *other,
                                           uint32_t code) {
        Resource *r = static_cast<Resource *>(target->owner);
        // INVENTED: routing of the bot_trigger contacts to the parent bot (the
        // exe's forwarding was not traced); the script queue below is unchanged.
        if (r)
            if (Resource *bot = game::bot_of_trigger(r))
                game::bot_trigger_event(bot, other, code);
        if (r)
            queue_event(r, nullptr, code,
                        Value::make_str(std::to_string((int32_t)other->owner_id)));
    };
    // 0x4fe6f0 residency + 0x4fe852 keep-alive on the ground body's resource.
    process_world().map_node_touch = [this](RigidBody *g, size_t ni) {
        map_node_touched(static_cast<Resource *>(g->owner), ni);
    };
}

Runtime::~Runtime()
{
    // The trigger callback captures this; the process world outlives the runtime
    // (the exe's engine singleton 0x60251c).
    process_world().trigger_event = nullptr;
    process_world().map_node_touch = nullptr;
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
        load_ground(*this, r, h);
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
    if (r->record && r->type == 1) create_ground_body(*this, r);
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
    if (r->record && r->type == 1) create_ground_body(*this, r);
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
    // 0x523c80 also runs the type record's native manager vf3; type_trigger
    // 0x4d4700 builds the sensor body from the same params string.
    create_trigger_body(*this, r, type, params);
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
    // 0x51fcc0 -> 0x55add0: loads the class on demand.
    if (!cs && !r->script_class.empty()) cs = vm_->get_class(r->script_class);
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
            !game::player_command(*this, target, ev, param) &&
            !game::bot_command(*this, target, ev, param) &&
            !game::cursor_command(*this, target, ev, param) && target->script)
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
    // +0x154 native instance dtor, type_ground vf4 0x4bb3d0: its groundbody
    // leaves the world (0x4fadf0).
    if (r->ground_body) {
        process_world().remove_body(r->ground_body.get());
        r->ground_body.reset();
    }
    // type_trigger vf4 0x4d4a60: detach and release the sensor body.
    if (r->trigger_body) {
        process_world().remove_body(r->trigger_body.get());
        r->trigger_body.reset();
    }
    // type_car vf4 0x49beb0 / type_BOT vf4 0x4805a0: the car's world bodies and
    // the bot payload go.
    game::instance_dtor(*this, r);
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
    // shortcut: other native instance dtors, the "Undefined GameType
    // Destructor" log, arrays +0x11c/+0x124/+0x13c/+0x150, handles
    // +0x128/+0x140 and the loaded bit (0x51f790) have no src counterpart.
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

// 0x47ba10 Game__runAnimateCallbacks(dt), ECX = Game: walk the mode-0x1c list,
// next read before the body runs (a body may unregister its node).
void Runtime::run_animate_callbacks(float)
{
    for (auto it = callbacks_.begin(); it != callbacks_.end();) {
        auto nx = std::next(it);
        Callback *cb = it->get();
        if (cb->mode != 0x1c) {
            it = nx;
            continue;
        }
        if (cb->handle.id == 0) {
            callbacks_.erase(it);      // delete node; count--
            it = nx;
            continue;
        }
        Resource *inst = cb->handle.target;
        // shortcut: no resident bit (inst+0x7c); a bound target is live.
        if (!inst) {
            it = nx;
            continue;
        }
        if (inst->traffic) {
            // 0x4bac10 type_ground vf13: 0x41b290 with the process world time,
            // then 0x417c90 flushSpawns(T, 1, cam) on the same object.
            inst->traffic->step(process_world().time);
            // cam: src has no camera snapshots (boot.cpp:222), so the exe's
            // `*0x5f1e44 > 0 ? &0x5fe110 : null` is always null (notes/42 §13).
            inst->traffic->flush_spawns(
                1, nullptr, vm() && natives::load_priority(*vm()) != 0,
                // shortcut: instance creation is card 261, so no instance.
                [](const traffic::Car &, uint32_t, const std::string &) {
                    return 0u;
                });
        } else {
            // shortcut: script "animate" and other native vf13 not dispatched;
            // no shipped script registers mode 0x1c (only Chassis.java calls
            // unregisterCallbacks).
        }
        it = nx;
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

// ---- pick raycast (0x4fe1a0) ----------------------------------------------

// shortcut: entries are never freed (no instance-destroy hook yet).
std::map<Runtime::Resource *, PhysicsPose> &physics_poses()
{
    static std::map<Runtime::Resource *, PhysicsPose> m;
    return m;
}

// 0x4b1ef0: 0 -> 0, negative -> -1, positive -> 1.
static float pick_sign(float v)
{
    return v < 0.0f ? -1.0f : (v > 0.0f ? 1.0f : 0.0f);
}

// 0x5007c0: ray o + dir*s against a sphere; returns the segment parameter
// measured from the `to` end (dir runs from -> to, len = |to - from|) in
// [0,1], or -1 on a miss.
static float ray_sphere(Vec3 o, Vec3 dir, float len, Vec3 c, float r)
{
    Vec3 v = c - o;
    float p = vec_dot(dir, v);
    if (p < -r || p > len + r) return -1.0f;
    float r2 = r * r;
    float q = vec_dot(v, v) - p * p;
    if (q <= r2 && (p >= 0.0f || p * p + q <= r2) &&
        (p <= len || (p - len) * (p - len) + q <= r2)) {
        float t = ((len - p) - sqrtf(r2 - q)) / len;
        return t > 0.0f ? t : 0.0f;
    }
    return -1.0f;
}

// 0x500580: segment a->b in box-local space against half extents h; parameter
// measured from b, -1 on a miss. Keeps the exe's seed and min/max order
// (y seeded from its own axis, then x and z folded in).
static float slab(Vec3 a, Vec3 b, float hx, float hy, float hz)
{
    const float h[3] = {hx, hy, hz};
    const float av[3] = {a.x, a.y, a.z};
    const float bv[3] = {b.x, b.y, b.z};
    for (int k = 0; k < 3; k++)
        if (!(av[k] <= h[k] || bv[k] <= h[k]) ||
            !(-h[k] <= av[k] || -h[k] <= bv[k]))
            return -1.0f;
    const float d[3] = {a.x - b.x, a.y - b.y, a.z - b.z};
    float lo[3], hi[3];
    for (int k = 0; k < 3; k++) {
        if (d[k] == 0.0f) {
            lo[k] = 0.0f;
            hi[k] = 1.0f;
            continue;
        }
        float s = pick_sign(d[k]);
        lo[k] = (-s * h[k] - bv[k]) / d[k];
        hi[k] = (s * h[k] - bv[k]) / d[k];
    }
    float lo_t = d[1] == 0.0f ? 0.0f : lo[1];
    float hi_t = d[1] == 0.0f ? 1.0f : hi[1];
    hi_t = fminf(hi_t, hi[0]);
    hi_t = fminf(hi_t, hi[2]);
    lo_t = fmaxf(lo_t, lo[0]);
    lo_t = fmaxf(lo_t, lo[2]);
    if (!(lo_t <= hi_t)) return -1.0f;
    return lo_t > 0.0f ? lo_t : 0.0f;
}

struct PickRay {
    Vec3 from, to, dir;
    float len = 0.0f;
    float best_t = 1.0f;
    bool first_only = true;
    Runtime::PickHit *out = nullptr;
};

static bool body_hit(PickRay &ray, Runtime::Resource *body);

// 0x4fddf0: walk the resource tree, testing kind-1 children and kind-2 bodies.
static bool walk(PickRay &ray, Runtime::Resource *node)
{
    if (node->type != 2) {
        // 0x4fd110 triangle test of the node's own mesh: not ported (INVENTED).
        if ((node->flags & 0x40u) != 0) return false;
        for (Runtime::Resource *child : node->children) {
            if (child->type != 1 && child->type != 2) continue;
            // INVENTED: the child+0xb0 bounds cull is skipped, src has no node bounds.
            if (walk(ray, child) && ray.first_only) return true;
        }
        return false;
    }
    // body+0x58 flags 0x4020 are not modelled: treated as 0.
    if (node->pick_shape == 0) return false;
    return body_hit(ray, node);
}

// 0x4fc140: one physics body: bound-sphere cull, child bodies, then the shape.
static bool body_hit(PickRay &ray, Runtime::Resource *body)
{
    // INVENTED: the exe's child bodies always carry a shape; src children may not.
    if (body->pick_shape == 0) return false;
    PhysicsPose &pose = physics_poses()[body];
    Vec3 pos{pose.pos[0], pose.pos[1], pose.pos[2]};
    float hx = body->pick_half[0], hy = body->pick_half[1], hz = body->pick_half[2];
    // INVENTED bound radius (shape+0xc not traced): |half extents| for a box,
    // the radius for a sphere.
    float bound = body->pick_shape == 5
                      ? sqrtf(hx * hx + hy * hy + hz * hz)
                      : body->pick_radius;
    float t0 = ray_sphere(ray.from, ray.dir, ray.len, pos, bound);
    if (!(0.0f <= t0 && t0 <= ray.best_t)) return false;
    for (Runtime::Resource *child : body->children)
        if (body_hit(ray, child)) return true;
    float t;
    if (body->pick_shape == 5) {
        // 0x4af9a0 body-local: R^T*(p - pos); box test on the segment.
        Mat34 rot;
        euler_to_mat(&rot, pose.ypr);
        Vec3 a = mat_tvec(rot, ray.from - pos);
        Vec3 b = mat_tvec(rot, ray.to - pos);
        t = slab(a, b, hx, hy, hz);
        if (t <= 0.0f) return false;
    } else {
        // shape kind 1 sphere: the bound-sphere parameter is the hit.
        // Kind 6/7 hulls are not stored (INVENTED skip).
        t = t0;
    }
    if (t < ray.best_t) {
        ray.best_t = t;
        if (ray.out) {
            ray.out->ref = body->parent;  // body+0x44 owner link
            ray.out->body = body;
            ray.out->t = t;
            // hit_index/hit_sub/normal are not part of PickHit.
        }
    }
    return false;
}

// 0x4fe1a0: hit == NULL selects first-only (the exe's t_out == NULL).
bool Runtime::raycast(Resource *root, const float from[3], const float to[3],
                      PickHit *hit)
{
    if (!root) return false;
    PickRay ray;
    ray.from = Vec3{from[0], from[1], from[2]};
    ray.to = Vec3{to[0], to[1], to[2]};
    Vec3 d = ray.to - ray.from;
    ray.len = vec_length(d);
    ray.dir = d * (1.0f / (ray.len > 1e-6f ? ray.len : 1e-6f));
    ray.first_only = hit == nullptr;
    ray.out = hit;
    if (hit) *hit = PickHit{};
    bool r = walk(ray, root);
    if (ray.first_only) return r;
    if (ray.best_t < 1.0f) {
        Vec3 p = (ray.from - ray.to) * ray.best_t + ray.to;
        if (hit->body) {  // 0x4fe1a0 tail: world -> body-local
            PhysicsPose &pose = physics_poses()[hit->body];
            Mat34 rot;
            euler_to_mat(&rot, pose.ypr);
            p = mat_tvec(rot, p - Vec3{pose.pos[0], pose.pos[1], pose.pos[2]});
        }
        hit->t = ray.best_t;
        hit->point[0] = p.x;
        hit->point[1] = p.y;
        hit->point[2] = p.z;
        return true;
    }
    return false;
}

// ---- screen pick (0x4eb000) -----------------------------------------------

// 0x567630: general 4x4 inverse (row-major); no singular test (det 0 -> inf/NaN).
// INVENTED: 1/det instead of rcpps + one Newton step (rcpps is CPU-dependent).
static render::Mat4 mat4_inverse(const render::Mat4 &a)
{
    const float *m = a.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    det = 1.0f / det;
    render::Mat4 o;
    for (int i = 0; i < 16; i++) o.m[i] = inv[i] * det;
    return o;
}

// 0x461c20: unproject a clip-space point through IP (cam+0x45c) and CM (cam+0x39c).
static void camera_unproject(const render::Mat4 &ip, const render::Mat4 &cm,
                             const float p[3], float out[3])
{
    const float *m = ip.m;
    float w = 1.0f / (p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15]);
    float q[3];
    for (int c = 0; c < 3; c++)
        q[c] = (p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + m[12 + c]) * w;
    for (int c = 0; c < 3; c++)
        out[c] = q[0] * cm.m[c] + q[1] * cm.m[4 + c] + q[2] * cm.m[8 + c] +
                 cm.m[12 + c];
}

// 0x461dd0 tail: cam+0x39c = the instance's first bone matrix, identity if none.
// INVENTED CM source: a pose written by GameRef.setMatrix lives on the Resource
// in src (not in the bone), so it replaces the bone matrix when present.
static render::Mat4 camera_matrix(const Runtime::Resource *cam)
{
    render::Mat4 cm = render::mat4_identity();
    if (!cam->matrix_pose) {
        if (!cam->bones.empty())
            for (int i = 0; i < 16; i++) cm.m[i] = cam->bones.front().local[i];
        return cm;
    }
    Mat34 r;
    euler_to_mat(&r, cam->ori);  // Ypr, same mapping as GameRef.getOri users
    // 0x567570: transposed 3x3 rotation, translation pos*10.
    cm.m[0] = r.m[0]; cm.m[1] = r.m[4]; cm.m[2] = r.m[8];
    cm.m[4] = r.m[1]; cm.m[5] = r.m[5]; cm.m[6] = r.m[9];
    cm.m[8] = r.m[2]; cm.m[9] = r.m[6]; cm.m[10] = r.m[10];
    cm.m[12] = cam->pos[0] * 10.0f;
    cm.m[13] = cam->pos[1] * 10.0f;
    cm.m[14] = cam->pos[2] * 10.0f;
    cm.m[15] = 1.0f;
    return cm;
}

// 0x4eb000: the exe clears all outputs first (index = -1, t = 0); PickHit's
// defaults are null/0, and raycast resets it again.
bool Runtime::screen_pick(float cx, float cy, PickHit *hit, float far_out[3],
                          float near_out[3])
{
    if (hit) *hit = PickHit{};
    if (far_out) far_out[0] = far_out[1] = far_out[2] = 0.0f;
    if (near_out) near_out[0] = near_out[1] = near_out[2] = 0.0f;
    // INVENTED order: drawn viewports by creation id ascending (the exe walks
    // game+0x174 in list order), a viewport's cameras in creation order.
    for (const auto &vup : instances_) {
        Resource *vp = vup.get();
        if (vp->type != 0x12 || !vp->viewport_active) continue;
        const float *rc = vp->viewport_rect;                    // +0x10..+0x1c
        float u = (cx - (2.0f * rc[0] - 1.0f)) / rc[2] - 1.0f;  // 0x4eb000
        float v = (cy - (2.0f * rc[1] - 1.0f)) / rc[3] - 1.0f;
        if (u < -1.0f || u > 1.0f || v < -1.0f || v > 1.0f) continue;
        v = -v;
        for (const auto &cup : instances_) {
            Resource *cam = cup.get();
            if (cam->type != 3 || cam->camera_viewport != vp) continue;
            // 0x461dd0: P from fov/near/far, IP = P^-1, CM from the bone.
            // cos/sin order verified (asm 0x461f4c cosf, 0x461f5b sinf, DIVSS cos/sin).
            const float deg = cam->camera_fov * 0.017453292f;
            const float rad = cam->camera_fov * (3.14159265f / 360.0f);
            const float k = sqrtf(sqrtf(cosf(rad) / sinf(rad)));
            const float zn = cam->camera_near;             // type+0x14 (already x10)
            const float zf = cam->camera_far * k;          // type+0x18 * k
            // type+0xc, else the viewport aspect vp_get_aspect returns (1.0f).
            const float aspect =
                cam->camera_aspect != 0.0f ? cam->camera_aspect : 1.0f;
            const render::Mat4 ip = mat4_inverse(
                render::mat4_perspective_fov_rh(deg, aspect, zn, zf));
            const render::Mat4 cm = camera_matrix(cam);
            const float pf[3] = {u, v, 1.0f};  // 0x421e20 far point
            const float pn[3] = {u, v, 0.0f};  // near point
            float far[3], near[3];
            camera_unproject(ip, cm, pf, far);
            camera_unproject(ip, cm, pn, near);
            for (int i = 0; i < 3; i++) {
                far[i] *= 0.1f;
                near[i] *= 0.1f;
            }
            // 0x4fe1a0 root = cam+0xac0 (the instance's parent node), from=far.
            PickHit h;
            if (raycast(cam->parent, far, near, &h)) {
                if (hit) *hit = h;
                if (far_out)
                    for (int i = 0; i < 3; i++) far_out[i] = far[i];
                if (near_out)
                    for (int i = 0; i < 3; i++) near_out[i] = near[i];
                return true;
            }
        }
    }
    return false;
}
