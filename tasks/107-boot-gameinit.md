---
id: 107
status: todo
branch:
depends: [106]
---
# app: boot GameInit like Game__mainLoop's init (0x47d760 -> 0x47c110 "init" -> 0x523c80)

## Goal
Today `Boot::run` mounts system.rpk, loads Init.class and runs its static initialisers only.
The exe's main loop starts the game by reading the config value `init` (a type resource id,
`Config.init = sl:0x0040r`), creating a scripted instance of that type named "GameInit"
(0x523c80 -> THRD-CREATE runs `Init(int)` untimed, card 103) and keeping its handle at
game+0x238. Add that step (`Boot::game_init()`), plus a real-data check that reports how
far the shipped `java.game.Init` constructor gets.

## Exe evidence (Claude, asm of 0x47d760 init)
    id   = GetConfigData("init")    (0x47c110: config object [0x5fe0e4] -> its script
           instance (+0x50) -> int field "init" via 0x4ece80; no config -> 0 + log
           "GetConfigData: Config data not present")
    tref = typeRef(id)              (0x51fa40 on a local ref node)
    inst = 0x523c80(&tref, parent = &DAT_005fde50 /* game root ref */, params "", alias "GameInit")
    bind(game+0x238, inst)          (0x40cac0)
    fps_lock = GetConfigData("fps_lock"); res_lock = GetConfigData("res_lock")
    (`Config.init` is `static int init = sl:0x0040r;` in util/src/Config.java; the 2.3.1
    install's system/Scripts/util/Config.class is the shipped one. fps_lock/res_lock are not
    fields of that class: GetConfigData returns 0 for them -> not ported here.)

## Changes
1. `src/script/natives.h` / `natives.cpp`: add a read-only thread report for checks/tools:
   ```cpp
   struct ThreadInfo { std::string name; bool started, done, sleeping, waiting, suspended;
                       std::string error; };   // error = runner->error() when Failed
   std::vector<ThreadInfo> thread_report(Vm &vm);
   ```
   (record the Failed state: keep the runner, and expose `Thread::status()`/`error()`.)
2. `src/app/boot.h` / `boot.cpp`:
   - Boot owns `std::unique_ptr<Runtime> runtime_` (created in `run()` after the VM is built:
     `Runtime(*resources_, vm_.get(), classes_.get())`, `activate()`), and a
     `Runtime::Handle game_init_` (game+0x238). `reset()` destroys the runtime first.
     Keep the existing `run()` behaviour/results otherwise (static initialisers of Init).
   - `bool Boot::game_init(std::string *err)`: `Value v;` read the static int
     `java.util.Config.init` with `vm_->get_static_field("java.util.Config", "init", &v, &e)`
     (loads the class lazily, card 105; its initialiser yields the global id, card 106).
     INVENTED: src reads the static field directly instead of the config game object's script
     instance (0x5fe0e4 is created by engine startup, not ported); say so in a comment.
     Not an int / 0 -> error "GetConfigData: Config data not present".
     `type = runtime_->resolve((uint32_t)v.i)`; null -> error.
     `inst = runtime_->instantiate(type, nullptr, "GameInit", "", true, &e)` (INVENTED: the
     exe's parent is the game root ref DAT_005fde50, which src does not model -> null parent).
     `runtime_->bind(&game_init_, inst)`. Store in BootResult: `game_init_type` (record name),
     `game_init_class` (inst->script_class), `game_init_created` (bool).
3. `src/tools/vmboot.cpp` selfcheck: after the existing boot checks call `boot.game_init(&e)`.
   Hard checks (shipped facts only): Config.init resolves to the record named "Init" (type 8)
   in sl.rpk; the created instance's script class is "java.game.Init"; a THRD-CREATE thread
   exists in `thread_report`. Then PRINT (not check) the THRD-CREATE state: done / stopped
   (sleeping/waiting/suspended) / failed + its error text, and the first 10 lines of stderr-
   worthy loader failures if easy. A failing constructor is an expected finding at this stage
   (missing natives/services); it must be visible in the check output, not hidden or asserted.

## Files
- edit: src/app/boot.h, src/app/boot.cpp, src/script/natives.h, src/script/natives.cpp,
  src/tools/vmboot.cpp

## Done when
- `// 0x47d760`, `// 0x47c110`, `// 0x523c80` comments; INVENTED parts marked
- `make -C src clean check` green; the new vmboot lines show the Init constructor outcome
- card: status implemented, branch, and a `## Outcome` section quoting the printed THRD-CREATE
  state line
