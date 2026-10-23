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

#include <list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "game/traffic.h"
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
        std::string data;                  // 0x518410 descriptor data (created nodes)
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
        // Render-instance bone (RenderController, 0x51b050): typed-data bone list at
        // T+0x1f4, kept sorted by name (_stricmp). INVENTED: lives on the bound Resource.
        struct Bone {
            uint32_t id = 0;        // +0x44
            std::string name;       // +0x24, at most 31 chars
            float local[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // +0x64..+0xa0
        };
        std::vector<Bone> bones;    // list order = name order
        // Render-instance typed data (RI+0x108, vtable 0x5a7ec0; 0x4ebef0/0x4ebe00/
        // 0x51e090). INVENTED mapping: the exe keeps these in the typed data block,
        // src has no typed-data layer, so they live on the bound Resource.
        float light_c0[3] = {};        // +0x10,+0x14,+0x18 setLight arg 1 (RGB)
        float light_c1[3] = {};        // +0x30,+0x34,+0x38 setLight arg 2
        float light_c2[3] = {};        // +0x20,+0x24,+0x28 setLight arg 3
        Resource *flare_texture = nullptr; // +0xc4 ref node target (0x4ebe00)
        float flare_f0 = 0.0f;         // +0xd4 first F
        float flare_f1 = 0.0f;         // +0xd8 second F
        int32_t flare_i0 = 0;          // +0xe0 first I
        int32_t flare_i1 = 0;          // +0x100 second I
        int32_t flare_i2 = 0;          // +0xe4 third I
        Resource *type_resource = nullptr; // +0x108 ref node target (0x51e090)
        uint32_t type_resource_id = 0;     // +0x110 ref node id (compared by setType)
        bool type_relinked = false;        // +0x118 = 1 when the type link changed
        // res_viewport typed data (0x525120, notes/12 §9): +0xc depth/order,
        // +0x10..+0x1c relative x,y,w,h.
        // INVENTED: the exe keeps these in the typed data block, not on the RI.
        int32_t viewport_depth = 0;    // +0xc
        float viewport_rect[4] = {};   // +0x10 x, +0x14 y, +0x18 w, +0x1c h
        bool viewport_active = false;  // INVENTED: game drawn-list (+0x14c) membership
        // res_camera typed data (0x524dc0, notes/12 §9): +0xc aspect (viewport w/h),
        // +0x10 fov, +0x14 near*10, +0x18 far*10, +0x2c i2, +0x30 i1, +0x34 f4, +0x38 f5.
        // fog data (0x4d9140): +0x1c start*10, +0x20 end*10, +0x24 enable 1.0f, +0x28 colour.
        // INVENTED: the exe keeps the camera words in the type-0xc resource's typed data
        // and the fog words in a separate resource reached via instance data +0x114;
        // src stores both on the resource bound to the script handle.
        float camera_aspect = 1.0f;      // +0xc
        float camera_fov = 0.0f;         // +0x10
        float camera_near = 0.0f;        // +0x14
        float camera_far = 0.0f;         // +0x18
        int32_t camera_i2 = 0;           // +0x2c
        int32_t camera_i1 = 0;           // +0x30
        float camera_f4 = 0.0f;          // +0x34
        float camera_f5 = 0.0f;          // +0x38
        int32_t camera_viewport_id = 0;  // arg2 -> 0x420080/FUN_00464f60; INVENTED storage
        float fog_start = 0.0f;          // +0x1c
        float fog_end = 0.0f;            // +0x20
        bool fog_enabled = false;        // +0x24 (exe stores 1.0f); INVENTED bool
        int32_t fog_color = 0;           // +0x28
        // type_ground (vtable 0x59e464) typed data +0x88: the Traffic graph.
        // INVENTED: kept on the record node and created on first lookup; the exe
        // creates it in type_ground vf3 0x4bade0 when the instance is made resident.
        bool ground_checked = false;
        std::shared_ptr<traffic::Graph> traffic;
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

    // The game's embedded env-map ref node (game+0x98; its RI is game+0xa4).
    // 0x4dc910 GfxEngine.setGlobalEnvmap rebinds it via FUN_0040cac0;
    // Game__renderFrame 0x421920 step 2 resolves it to a texture. No reader yet.
    Handle global_envmap;

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

    // Resolve id: with base it is first made global (0x51e860); the high word
    // is then the registry slot of the owning package (0x517b80). Null when the
    // package cannot be mounted or has no such local record.
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
    // 0x525020 / 0x524f20 -> 0x518410: script-created type 6/7 node (no record).
    Resource *define_script_resource(uint32_t type, const std::string &name,
                                     const std::string &data, Resource *parent);
    // FUN_0040cac0 / FUN_0051f9b0: rebind, ref-counting the target node.
    void bind(Handle *h, Resource *r);
    void bind(Handle *h, uint32_t id, const Package *base = nullptr);
    int refs(const Resource *r) const { return r ? r->refs : 0; }

    // Create a game instance of type under parent. scripted selects the
    // create/create_native pair; a script object's <init>(I) runs in the
    // THRD-CREATE thread.
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
    // 0x47e340 / 0x47b970: append a ResourceRefDLNode (0x47b090) bound to h->target.
    // Callers check h->id && h->target first (0x4ec800 / 0x4e9450).
    void queue_unload(Handle *h);
    void queue_delete(Handle *h);
    // 0x47d070 Game__flushDeferred: drain both lists until a pass does nothing.
    void flush_deferred();
    // vf[+0x1c](1) for the kinds src models (0x51a1b0 type 1; others no-op).
    void unload(Resource *r);
    // TimedEvent (0x4e7530): Game timer list +0x180, appended by 0x4e7a60.
    struct Timer {
        Handle handle;          // +0x24 own ref, bound to the receiver's resource
        float due = 0.0f;       // +0x34
        float period = 0.0f;    // +0x38 (ctor 0)
        uint32_t flags = 0;     // +0x3c (addTimer: 0x80000080, sign bit = one-shot)
        int32_t param = 0;      // +0x40 timerid
    };
    // 0x4e7a60: false when h has no id.
    bool add_timer(Handle *h, float due, uint32_t flags, int32_t param);
    // 0x4e93c0: unbind every timer whose handle id equals h->id.
    void remove_timers(Handle *h);
    // 0x47b700: fire due timers at sim time now.
    void run_timers(float now);
    // Game callback lists (0x47d670): one 0x34-byte ResourceRefDLNode per
    // registerCallback, whose handle is bound to the receiver's resource
    // (0x47b090). Modes 8 = GII_CONTROL, 9 = GII_DRIVE, 0x1c = GII_ANIMATE.
    struct Callback {
        Handle handle;   // node+0x24
        int mode = 0;    // the exe keeps three lists; one mode-tagged list is equivalent
    };
    // 0x47d670: append; other modes are a no-op. Null handle: INVENTED guard.
    void register_callback(Handle *h, int mode);
    // 0x47e390: unlink the handle of the first node of `mode` with id == h->id.
    void unregister_callback(Handle *h, int mode);
    // Wrapper object selected like FUN_004e7320 (GameRef/RenderRef/ResourceRef).
    Object *wrap(Resource *r, std::string *err);
    // Script class chain lookup used by GameRef.isScripted.
    bool class_is_a(const std::string &cls, const std::string &base);
    // 0x47c640 Game__getInfo(h, 0x39) -> 0x4bb570: the ground's traffic graph or null.
    traffic::Graph *ground_traffic(Handle *h);

  private:
    std::string script_class_of(const Package &pkg, const RpkRecord &rec) const;
    void unlink(Handle *h);
    void destroy(Resource *r);   // vf0(1) common part, D1..D5

    ResourceManager *resources_ = nullptr;
    Vm *vm_ = nullptr;
    ClassPath *classes_ = nullptr;
    std::map<std::pair<const Package *, uint32_t>, std::unique_ptr<Resource>> records_;
    std::vector<std::unique_ptr<Resource>> instances_;
    std::map<Object *, Handle *> handles_;
    std::list<std::unique_ptr<Timer>> timers_;
    std::list<std::unique_ptr<Callback>> callbacks_;
    std::list<std::unique_ptr<Handle>> unload_q_, delete_q_;   // Game+0xe8 / +0x134
    uint32_t next_instance_id_ = 0x80000000u;
    uint32_t next_script_id_ = 1;  // INVENTED low word; 0x518a40 pool not traced
    static Runtime *active_;
};
