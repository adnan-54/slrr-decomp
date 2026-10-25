---
id: 206
status: todo
branch:
depends: []
---
# Part.install_OK on the existing install search (supersedes card 051)

## Goal
Replace the explicit stub `java.game.parts.Part.install_OK(Ljava.util.resource.GameRef;ILjava.util.resource.GameRef;ILjava.lang.Vector3;)[I`
(natives.cpp row ~7982, `nullptr, true`). Callers: sl/Scripts/game/Garage.java:1106 (drag a part
onto the car) and InventoryItem_Part.java:124 (install from inventory): `xpart = new GameRef();
int[] slotId = part.install_OK(car, 0, xpart, 0, pos); if (slotId) installCheck(xpart.getScriptInstance(), slotId) ...`.
Today null -> both paths silently skip the install. The search itself already exists in src
(`install_search`, src/game/car.cpp:672, used by `part_command` "install" ~1760). RE: notes/23 §3,
notes/13 §15.

## Exe evidence (cleaned)
### 0x4cd3a0 Part.install_OK
```
getArgs(self, dest, slot /*ignored*/, xpart, slot2, pos)     // order verified in the decompile
if (dest == 0) return null;
P = typed 0xa0000000 of self (else return null); require secondary 0x20000000 (0x468b00) (else null)
bind(P+0x60, dest.RI)              // 0x4cd4c7 LEA ECX,[P+0x60]: same field "install" sets (dest)
tref = {0,0,0,0}                   // no explicit target part
posp = pos ? &{pos.x, pos.y, pos.z} : null      // 0x4ec980 float fields "x","y","z"
r = Part__installSearch 0x4c96c0(P, &tref, &slot2, posp)   // tref <- target actually used
if (xpart != 0) bind(xpart, tref.RI)                       // out-arg = found target part
if (r == 0) return null;
arr = new int[2]; arr[0] = r; arr[1] = slot2; return arr;
```
0x4c96c0 with an empty tref goes straight to the dest fallback (getInfo(dest,7) 5/9, dest, then the
car's parts), exactly src `install_search(rt, P, nullptr, dest, &used, &slot2, pos)`.

## Files
- edit: src/game/car.h + car.cpp: `install_search` is in the anonymous namespace; add a public
  wrapper after `part_command` (~1790):
  `// 0x4cd3a0 Part.install_OK search step`
  `int32_t part_install_ok(Runtime &rt, Runtime::Resource *P, Runtime::Resource *dest, Runtime::Resource **used, int32_t *slot2, const float *pos)`
  `{ if (g_rt != &rt || !P || !g_parts.count(P)) return 0; return install_search(rt, P, nullptr, dest, used, slot2, pos); }`
  (the same guard part_command uses).
- edit: src/script/natives.cpp: `pt_install_ok` near `pt_get_car_ref` (~4891), registry row ->
  `pt_install_ok, false`. Args: a[0] self, a[1] dest (GameRef obj), a[2] slot (ignored), a[3] xpart
  (GameRef obj or null), a[4] slot2 (`get_i`), a[5] pos (Vector3 obj or null).
  - `Runtime::Resource *dest = value_resource(a[1])`; dest null -> `Value::make_null()`.
  - P = `value_resource(a[0])` (the part instance, as `part_of` uses).
  - pos: when `a[5]` is a non-null Obj, `p[k] = dd_get_f(vm, a[5].obj, "x"/"y"/"z")` (pattern at
    natives.cpp ~5206).
  - `int32_t r = game::part_install_ok(*rt, P, dest, &used, &slot2, posp)`.
  - xpart: `if (Runtime::Handle *xh = value_handle(a[3])) rt->bind(xh, used);` (Runtime::bind,
    runtime.h:213; `used` may be null: unbinds, like bind(xpart, 0)).
  - r == 0 -> null; else `Array *arr = vm.new_array("I", 2)` (pattern string_getascii ~1131),
    elems {r, slot2}, `Value::make_array(arr)`.
  - INVENTED: P+0x60 (stored dest) is not modelled; src passes dest directly (same as part_command).
- read only: src/game/runtime.h (Handle, bind).

## Done when
- `// 0x4cd3a0` comment; deviations `INVENTED:`/`shortcut:`
- `make -C src clean check` green (stub count drops by 1); no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
