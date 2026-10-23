// Game runtime object/id layer: resource handles over the package registry.
//
// The exe keeps a ResourceInstance (RI) per resolved resource and attaches a
// 0x10-byte ref node {prev,next,id,RI} to every script object (notes/06,
// notes/13). This rebuild keeps the same two layers: Resource nodes resolved
// through ResourceManager (id = package index << 16 | local id, super chains
// across packages) and Handle nodes owned by this runtime and keyed by script
// object. Engine subsystems (streaming, rendering, world, traffic) stay explicit
// not-implemented natives; the runtime only carries the state those natives
// expose (id/type/name/super chain, flags, pos/ori/vel, parent/children).
#pragma once
#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "resource/manager.h"

class Vm;
struct Object;
struct Value;

class Runtime {
  public:
    struct Resource;
    struct Handle;

    // A resolved resource or a created game instance. Record-backed nodes are
    // shared per open package; instance nodes are owned by the runtime.
    struct Resource {
        uint32_t id = 0;
        uint32_t type = 0;                 // resource type code (8 type, 1 instance)
        std::string name;                  // record name, or instance alias
        std::string script_class;          // `script` ref of the backing record
        const Package *package = nullptr;
        const RpkRecord *record = nullptr; // null for created instances
        const RdbResource *db = nullptr;
        Resource *parent = nullptr;        // super record or instance parent
        uint32_t parent_id = 0;            // raw super id / parent instance id
        Resource *world_root = nullptr;
        Resource *next_sibling = nullptr;
        std::vector<Resource *> children;
        uint32_t flags = 0;                // RI+0x84
        // Notification (0x4e7480, 0x44 bytes) on the watched instance's list.
        struct Notification {
            Resource *watcher = nullptr;   // +0x24 ref node
            uint32_t mask = 0;             // +0x34 etype
            uint32_t alias = 0;            // +0x38 ealias
            bool has_message = false;      // +0x3c non-null
            std::string message;
            bool has_method = false;       // +0x40 non-null
            std::string method;
        };
        // INVENTED: initial mask 0; the typed-object ctor value was not traced.
        uint32_t event_mask = 0;                  // typed+0xa0
        std::vector<Notification> notifications;  // typed+0x5c, index 0 = list head
        float pos[3] = {};
        float ori[3] = {};
        float vel[3] = {};
        float ang[3] = {};
        float detail = 0.0f;
        // java.render.Text state, render-instance typed data +0x16c..+0x17c
        // (Text__create 0x4e8f40). src has no typed-data/bone service yet, so it
        // lives on the bound Resource. INVENTED mapping; same as card 062.
        int32_t text_flags = 0;            // +0x16c (create: |= 0x8000)
        std::string text_data;             // +0x170 current string (create: "")
        int32_t text_align = 2;            // +0x174 (2 = left)
        float text_scale = 1.0f;           // +0x178
        uint32_t text_color = 0xffffffffu; // +0x17c (DAT_005dadac)
        float text_pos[3] = {};            // INVENTED: bone00 matrix translation, raw (x,y,0)
        // res_viewport typed data (0x525120, notes/12 §9): +0xc depth/order,
        // +0x10..+0x1c relative x,y,w,h.
        // INVENTED: the exe keeps these in the typed data block, not on the RI.
        int32_t viewport_depth = 0;    // +0xc
        float viewport_rect[4] = {};   // +0x10 x, +0x14 y, +0x18 w, +0x1c h
        bool viewport_active = false;  // INVENTED: game drawn-list (+0x14c) membership
        bool instance = false;
        Object *script = nullptr;          // script instance (typed +0x50)
        int refs = 0;                      // handles attached (RI+0x78 list)
        Handle *handles = nullptr;         // intrusive handle list
    };

    // The exe's 0x10-byte ref node. id is the raw id the handle was set to.
    struct Handle {
        Resource *target = nullptr;
        uint32_t id = 0;
        Handle *prev = nullptr;
        Handle *next = nullptr;
    };

    Runtime(ResourceManager &resources, Vm *vm = nullptr, ClassPath *classes = nullptr);
    ~Runtime();
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;

    // Process-wide runtime the natives dispatch through (the exe's engine
    // singleton). Tests and the boot create one and activate it.
    static Runtime *active();
    void activate();
    static void deactivate();

    ResourceManager &resources() const { return *resources_; }
    Vm *vm() const { return vm_; }
    ClassPath *classes() const { return classes_; }

    // Resolve id in base's namespace; base null tries every open package.
    // Null when no open package owns the id (external chain links included).
    Resource *resolve(uint32_t id, const Package *base = nullptr);
    // Super-id chain of a record-backed resource (first link is itself).
    bool chain(Resource *r, std::vector<ChainLink> *out, std::string *err) const;
    std::string type_name(uint32_t type) const;

    // Script instance -> handle payload.
    Handle *handle(Object *script) const;
    Handle *attach(Object *script);
    void detach(Object *script);
    // 0x525120 -> 0x5188e0: a created engine resource (no record); "_viewport" is type 0x12.
    Resource *create_resource(uint32_t type, const std::string &name);
    // FUN_0040cac0 / FUN_0051f9b0: rebind, ref-counting the target node.
    void bind(Handle *h, Resource *r);
    void bind(Handle *h, uint32_t id, const Package *base = nullptr);
    int refs(const Resource *r) const { return r ? r->refs : 0; }

    // Create a game instance of type under parent. scripted selects the
    // create/create_native pair; script objects are made but their constructor
    // thread is not spawned.
    Resource *instantiate(Resource *type, Resource *parent, const std::string &alias,
                          const std::string &params, bool scripted, std::string *err);
    void reparent(Resource *r, Resource *parent);
    // Script object for a node's script class, created once and cached.
    Object *script_instance(Resource *r);
    // 0x47dd70: run `name` on self with args to completion. False when the
    // method is missing or the thread did not finish (error text in *err).
    bool call_method(Object *self, const std::string &name,
                     const std::vector<Value> &args, Value *result, std::string *err);
    // 0x47d550 -> 0x47c760: synchronous event dispatch. param is a Value::Str,
    // Value::Int, Value::Obj or Value::Null; method null = default handler name.
    void queue_event(Resource *target, Resource *sender, uint32_t code,
                     const Value &param, const char *method = nullptr);
    // Wrapper object selected like FUN_004e7320 (GameRef/RenderRef/ResourceRef).
    Object *wrap(Resource *r, std::string *err);
    // Script class chain lookup used by GameRef.isScripted.
    bool class_is_a(const std::string &cls, const std::string &base);

  private:
    const Package *target_package(const Package &base, uint32_t id,
                                  uint32_t *local) const;
    std::string script_class_of(const Package &pkg, const RpkRecord &rec) const;
    void unlink(Handle *h);

    ResourceManager *resources_ = nullptr;
    Vm *vm_ = nullptr;
    ClassPath *classes_ = nullptr;
    std::map<std::pair<const Package *, uint32_t>, std::unique_ptr<Resource>> records_;
    std::vector<std::unique_ptr<Resource>> instances_;
    std::map<Object *, Handle *> handles_;
    uint32_t next_instance_id_ = 0x80000000u;
    static Runtime *active_;
};
