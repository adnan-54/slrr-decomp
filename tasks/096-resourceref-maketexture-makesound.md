---
id: 096
status: implemented
branch: card/096
depends: []
---
# script: ResourceRef.makeTexture/makeSound create a script-local node

## Goal
Replace the two `java.util.resource.ResourceRef` stub rows in `src/script/natives.cpp`
(`makeTexture` / `makeSound`, rules at lines 7192-7193) with ports of the exe natives
0x4e3ff0 and 0x4e3f60. The two bodies are identical except for the create helper and the
created kind: 0x525020 makes a type 7 `_texture` node, 0x524f20 a type 6 `_sfx` node. Both
build the descriptor text `sourcefile <file>\r\nflags 0\r\n`, register a fresh node with a
`0xffff????` id under the parent argument's resource, set `+0x84 |= 0x200`, and bind the
receiver's handle to it. `makeSound` does not touch the audio service: the type-6 node is
parsed later by `sfx_load` when `SfxRef.nplay` runs, so that helper must also read the
created node's text (`Resource::data`).

## Exe evidence (cleaned pseudocode; L payload of `getArgs` = 0x10-byte ref node)
Mapping as in the neighbouring ResourceRef ports: `getArgs` 0x4b1af0; ref node +8 = id,
+0xc = RI; 0x4ecc70 = `game_rt()->handle(receiver)`; 0x40cac0 = `Runtime::bind`.

### 0x4e3ff0 ResourceRef.makeTexture(ResourceRef parent, String filename)V
    getArgs(ctx, &self, &parent, &filename);
    h = instanceToHandle(self);                       // 0x4ecc70
    if (!h || !parent || !filename) return;           // no log, void
    n = strlen(filename);
    buf = Mem__alloc(n + 0x1d);                       // 0x567a20, strlen+29
    sprintf(buf, "sourcefile %s\r\nflags %d\r\n", filename, 0);   // 0x528030
    node = Resource_Texture__create(parent, buf, n + 0x1d);      // 0x525020
    Runtime__bind(h, node);                           // 0x40cac0

Format string at 0x5a05c8; the sound native uses the identical bytes at 0x5a078c.

### 0x4e3f60 ResourceRef.makeSound(...)V
Same body; create helper is 0x524f20 (type 6 `_sfx`).

### 0x525020 / 0x524f20 create(parent ref node, data, size)
    id = ResourceManager__nextLocalId();              // 0x518a40, returns 0xffff|n
    desc = { type /*7 or 6*/, id, 0, data, size, name };   // 0x18-byte stack record
    name = Mem__alloc(0x20);                          // 0x567a20
    strncpy(name, "_texture", 0x1f);                  // 0x569340; sound: DAT_005a8ec8 "_sfx"
    if (parent->RI (+0xc) == 0)
        node = 0x5188e0(id, parent->id (+8), &desc, 0, 0);
    else
        node = 0x518880(id, parent->id (+8), &desc, parent->RI, 0, 0);
    node->flags (+0x84) |= 0x200;
    free(name);
    return node;

### 0x5188e0 / 0x518880 wrappers
    if (id >> 16 != 0xffff && id >> 16 < manager->0x166740) 0x51e310();   // id-space bookkeeping
    0x5188e0 -> 0x518410(parent_id, &desc, 0, 0, 0)
    0x518880 -> 0x518410(parent_id, &desc, 0, 0, parent_RI)
    if (node) 0x51f790(0);                            // untraced post-create hook

### 0x518410 register(parent_id, desc, 0, 0, parent_RI)
    node = alloc by desc.type (6 -> 0x525b50 / 0xc0 bytes; 7 -> 0x5263a0 / 0xdc bytes);
    if (!node) log "Node registration error: ...";
    else {
        if (!parent_RI) parent_RI = parent_id ? find_by_id(parent_id, desc.type) : manager root;
        if (!parent_RI) log "Unregistrable node: ... (invalid parent ...)";
        node->parent (+0x24) = parent_RI;
        link node into the parent's child list (head parent+0x50, tail parent+0x58);
        if (DAT_005db2a8[desc.type]) 0x51f440(parent_RI);   // untraced
    }
    return node;

## Changes
1. `src/game/runtime.h`:
   - `struct Resource`: add `std::string data;  // 0x518410 descriptor data (created nodes)`
     next to `record` (null for created nodes).
   - public, after `create_resource`:
     ```cpp
     // 0x525020 / 0x524f20 -> 0x518410: script-created type 6/7 node (no record).
     Resource *define_script_resource(uint32_t type, const std::string &name,
                                      const std::string &data, Resource *parent);
     ```
   - private: `uint32_t next_script_id_ = 1;  // INVENTED low word; 0x518a40 pool not traced`
2. `src/game/runtime.cpp`:
   ```cpp
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
       r->instance = true;   // id prefix 0xffff; same stand-in as unload (runtime.cpp:626)
       if (parent) reparent(r, parent);
       return r;
   }
   ```
   `reparent` gives the exe's parent link (`+0x24`, child list, parent_id = parent RI id).
3. `src/script/natives.cpp`, in the ResourceRef section above `rr_set_id`:
   ```cpp
   // 0x4e3ff0 / 0x4e3f60 shared path. No log: the exe just returns.
   static bool rr_make_resource(const char *who, uint32_t type, const char *name,
                                const Args &a, Value *out, std::string *err)
   {
       if (!need_rt(who, err)) return false;
       Object *o;
       if (!receiver_object(who, a, &o, err) || !want(who, a, 3, err)) return false;
       Runtime::Handle *h = game_rt()->handle(o);        // 0x4ecc70
       Runtime::Handle *parent = value_handle(a[1]);     // getArgs L payload = ref node
       *out = Value::make_null();
       if (!h || !parent || a[2].kind != Value::Str) return true;   // null arg -> return
       const std::string text =
           "sourcefile " + a[2].s + "\r\nflags 0\r\n";               // 0x5a05c8 format
       Runtime::Resource *node =
           game_rt()->define_script_resource(type, name, text, parent->target);
       game_rt()->bind(h, node);                         // 0x40cac0
       return true;
   }

   bool rr_make_texture(Vm &, const Args &a, Value *out, std::string *err)   // 0x4e3ff0
   { return rr_make_resource("ResourceRef.makeTexture", 7, "_texture", a, out, err); }

   bool rr_make_sound(Vm &, const Args &a, Value *out, std::string *err)     // 0x4e3f60
   { return rr_make_resource("ResourceRef.makeSound", 6, "_sfx", a, out, err); }
   ```
   Registry rows 7192-7193: replace `nullptr` with `rr_make_texture` / `rr_make_sound`;
   leave the trailing `engine` bool unchanged (unused by `install`, natives.cpp:7300).
   Extend `sfx_load` (line ~4564) with the created-node text:
   ```cpp
   const char *data = nullptr;
   size_t size = 0;
   if (r->record && r->package && r->record->data_size > 0) {
       data = (const char *)r->package->rpk.data(*r->record);
       size = r->record->data_size;
   } else if (!r->data.empty()) {
       data = r->data.data();   // 0x518410 descriptor data (makeSound)
       size = r->data.size();
   }
   const bool have = data && audio::parse_sound_params(data, size, &params);
   ```
   (rest of `sfx_load` unchanged.)

## Files
- edit: src/game/runtime.h, src/game/runtime.cpp, src/script/natives.cpp

## Done when
- each ported function has a `// 0xADDR` comment; the id counter is `INVENTED:`
- `make -C src clean check` green; no new unit tests (no shipped script on the check
  path calls these natives, so the check only proves the build and registry)
- card frontmatter: status implemented, branch name

## UNRESOLVED
- 0x518a40 low 16-bit numbering (start, wrap, reuse) not traced. Only the `0xffff` high
  word is observed (0x5188e0's guard checks it); sequential from 1 is INVENTED.
- `Mem__alloc` size is `strlen+0x1d` but `sprintf` writes `strlen+24` bytes; the 5 extra
  bytes' content/purpose is not traced. src stores the exact text without padding.
- Post-create hooks 0x51f790(0) and 0x51f440(parent) not traced.
- Node ctors 0x525b50 (type 6) / 0x5263a0 (type 7) and how they copy desc data are not
  traced; src copies the text into `Resource::data` (the caller never frees `buf`).
- Parent ref node non-null but unbound (no RI): the exe resolves the parent by id and
  otherwise falls back to the manager root (0x518410); src has no root node, so the node
  is created unparented (INVENTED).
- Type-7 nodes have no src consumer (no runtime texture loader); creation/binding is only
  observable through the receiver handle. A later texture card should parse
  `Resource::data` like `sfx_load` does.
- 0x4ecc70 (instanceToHandle) is not re-traced here; its mapping is the one already used
  by `rr_id` / `rr_load` (notes/09).
