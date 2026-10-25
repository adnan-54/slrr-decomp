---
id: 094
status: merged
branch: card/094
depends: []
---
# script: RenderRef lineCreate/lineAdd/plotRoute (service-less)

## Goal
Replace the three `java.util.resource.RenderRef` stub rows (`src/script/natives.cpp:6995-6997`)
with ports of the exe's guard chains. src has no render typed-data layer (0x51f4f0 /
0x524840 / 0x468b00 / 0x51bb30 line factory) and no process route state
(0x5fe450..0x5fe45c are written only by GroundRef.findRoute, whose route is not
modelled in src), so both service chains stop at their null guards. The exe return in
that case is: `lineCreate` -> `int 0`, `lineAdd` -> `void` (nothing appended),
`plotRoute` -> `int 0` (route checked before `getArgs`). Same no-service pattern as
cards 085/091; the whole chain is recorded below so a later render-line/route-service
card can fill in the bodies.

## Exe evidence (cleaned pseudocode; helpers abbreviated)
Mapping used by the neighbouring ResourceRef/RenderRef ports (notes/06, notes/12,
notes/13, card 077): `getArgs` 0x4b1af0 = `want()` + `get_*`; `instanceToHandle`
0x4ecc70 = `game_rt()->handle(receiver)`; `handle+0xc` = `Runtime::Handle::target`
(`Runtime::Resource`); `RI+0x7c != 1` -> `RI->vf5(1.0f)` makes the instance resident;
0x51f4f0 = getTyped(0xa0000001, ...); `RI->vf3(1.0f)` yields the render typed data `T`;
0x524840 = createInstance(parent, type, name, boneCount); 0x40cac0 = bind.

### 0x4e24f0 RenderRef.lineAdd(Vector3 p, Vector3 dir, I colour, F width)V
    getArgs(ctx, &self, &pArg, &dArg, &i, &f);        // i default -1, f default 1.0f
    h = instanceToHandle(self);
    if (h != 0 && *(int *)(h + 8) != 0) {              // h->id
        p   = { getF(pArg, "x"), getF(pArg, "y"), getF(pArg, "z") };
        dir = { getF(dArg, "x"), getF(dArg, "y"), getF(dArg, "z") };
        RenderRef__lineAdd(&p, &dir, i, f);            // 0x4ec6a0, ECX = h
    }

    Field descriptors: 0x5a0348 'x', 0x5a0378 'y', 0x5a0384 'z' (arg1);
                       0x5a03a4 'x', 0x5a03d0 'y', 0x5a040c 'z' (arg2).

### 0x4ec6a0 RenderRef__lineAdd(Vector3 *p, Vector3 *dir, int colour, float width) — ECX = h
    RI = h->target; if (!RI) return;
    if (RI+0x7c != 1) RI->vf5(1.0f);                   // 1.0f = 0x3f800000
    if (0x51f4f0(0xa0000001, 0, 0) < 0) return;
    T = RI->vf3(1.0f); if (!T) return;
    L = *(void **)(T + 0x1c); if (!L) return;          // line-strip object
    0x478690(L, p, dir, colour, width);                // append (ECX = L)

### 0x478690 line-strip append (not ported; summarised from the decompile)
    cdecl params p (position), dir (normal/up), colour, width; ECX = L.
    State seen: +0x24 capacity, +0x28 first index, +0x2c count, +0x30 accumulated
    length, +0x34 point array (stride 0x30: xyz at +0, colour at +0x10, width bits at
    +0x2c), +0x40 min width, +0x44 max width, +0x48 min segment length,
    +0x4c/+0x50/+0x54/+0x5c width/uv params, +0x74 flag, +0x78..+0xd4 duplicated
    quad, +0xfc flags (bit 1 end cap, bit 2 start cap, bit 4 side flip),
    +0x100..+0x158 last quad, +0x134..+0x158 bbox. It drops a point closer than
    +0x48 to the previous one, normalises `dir` to build the ribbon side vectors,
    clamps `width` between +0x40/+0x44, stores `colour` in the point and emits the
    previous quad. Only described here: a future line-service card ports it.
    UNRESOLVED: exact quad/uv layout and +0x74/+0xfc meanings.

### 0x4e25d0 RenderRef.lineCreate(ResourceRef parent, ResourceRef type)I
    getArgs(ctx, &self, &parent, &type);
    h = instanceToHandle(self);
    if (h != 0 && parent != 0 && type != 0) {
        if (0x4e8ca0(parent, type, 0, 0, "line")) return 1;   // ECX = h
    }
    return 0;

### 0x4e8ca0 RenderRef__lineCreate(parent, type, a3, a4, name) — ECX = h, stdcall 5 args
    if (!parent || !type) return 0;
    RI = type->target; if (!RI) return 0;              // type+0xc
    if (RI+0x7c != 1) RI->vf5(1.0f);
    if (0x51f4f0(0xa0000001, 0, 1.0f) < 0) return 0;
    T = RI->vf3(1.0f); if (!T) return 0;
    factory = *(void **)(T + 0x2c); if (!factory) return 0;
    N = 0x524840(parent, type, name /* "line" */, 0);  // createInstance
    0x40cac0(N);                                       // bind(ECX = h, N)
    0x459290(N + 0xb0);
    L = 0x468b00(0xa0000001, 1.0f, 0, 1.0f);           // ECX = h
    if (!L) return 0;                                  // (asm: je to the xor-eax tail)
    0x51bb30(factory, a3, a4, 2);                      // ECX = L
    *(void **)(L + 0x1c) = result of 0x51bb30;         // 0x15c-byte line strip
    if (*(uint *)(N + 0x84) & 0x100) 0x518e90(N, 1.0f, 0, 1.0f);
    return 1;
    Name strings: 0x5a6738 (lineCreate) and 0x5a1d60 (plotRoute), both "line".
    0x51bb30 (ECX = typed data): frees the old +0x1c object (`+0x16c &= ~0x4000`,
    vf0(1)), allocates 0x15c bytes, ctor 0x477ed0(...), stores it at +0x1c and sets
    `+0x16c |= 0x4000`.

### 0x4e2640 RenderRef.plotRoute(ResourceRef a, ResourceRef b, I colour, F density, Vector3 scale)I
    if (DAT_005fe450 == 0) return 0;                   // route path pointer, BEFORE getArgs
    getArgs(ctx, &self, &a, &b, &i, &f, &scale);       // i default -1, f default 1.0f
    h = instanceToHandle(self);
    if (!h) return 0;
    if (0x4e8ca0(a, b, 0, 0, "line") == 0) return 0;   // ECX = h
    step = f / DAT_005fe454;                           // route length |e - s|
    sx = 1.0f; sz = 1.0f;
    if (scale) { sx = getF(scale, 0x5a1da4 "x");
                 (void)getF(scale, 0x5a1db8 "y");      // read and discarded
                 sz = getF(scale, 0x5a1dd0 "z"); }
    for (t = 0.0f; t < 1.0f; t += step) {              // do/while, t<1.0f
        d = (1.0f - t) * DAT_005fe458 + DAT_005fe45c * t;   // s*(1-t) + e*t
        pos = 0x4133f0(DAT_005fe450, d, 0);            // ctPath__posOffsetAt
        point = { pos.x * sx, -0.0005f, pos.z * sz };  // y = 0xba03126f (asm literal)
        up    = { 0.0f, 1.0f, 0.0f };
        width = 0x414a80(DAT_005fe450, d);             // segment +0x3c
        RenderRef__lineAdd(&point, &up, i, (float)width);   // 0x4ec6a0, ECX = h
    }
    return 1;
    Route globals (notes/19): 0x5fe450 Path*, 0x5fe454 length, 0x5fe458 s,
    0x5fe45c e. Segment loader 0x414d50 writes +0x30 len, +0x34, +0x38 curved
    byte and +0x40 = 1.0f; +0x3c (the plot width) is not loaded from the file.

## UNRESOLVED
- 0x478690 ribbon/quad semantics, +0x74 and +0xfc flag bits; not needed for the
  no-op port but must be traced before a line-service card.
- Segment field +0x3c returned by 0x414a80 as the plot width: the loader 0x414d50
  never writes it (only +0x38 and +0x40); its initialisation is unknown.
- 0x51bb30 / 0x477ed0 line-object internals and 0x468b00's exact return (the asm
  uses ECX = h, the caller's handle); both are engine-side and unported.
- The field descriptors at 0x5a1da4/0x5a1db8/0x5a1dd0 read as 'x','y','z' from the
  raw bytes; the surrounding descriptor layout (nearby "writeFile" string) was not
  decoded, matching how 0x4ec980 is used elsewhere.

## Changes
1. `src/script/natives.cpp`, after `rr_set_matrix_bone` (line 5833) and before the
   Viewport block: add three functions. Exact bodies:
   ```cpp
   // 0x4e25d0 RenderRef.lineCreate(ResourceRef parent, ResourceRef type)I
   // 0x4e8ca0 creates render instance "line" (0x524840/0x40cac0/0x468b00/0x51bb30)
   // and stores the 0x15c-byte strip at typed+0x1c; the type ref's factory lives at
   // typed+0x2c. The exe returns 0 whenever any of those guards fails, which is
   // always here.
   // SERVICE MISSING: no render typed-data layer, no line factory, no instance.
   bool rr_line_create(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("RenderRef.lineCreate", a, 3, err)) return false;
       *out = Value::make_int(0);
       return true;
   }

   // 0x4e24f0 RenderRef.lineAdd(Vector3 p, Vector3 dir, I colour, F width)V
   // 0x4ec6a0 needs the receiver's typed data (T+0x1c line object created by
   // lineCreate) and calls 0x478690 to append a ribbon segment. Both are absent.
   // SERVICE MISSING: T+0x1c / 0x478690.
   bool rr_line_add(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("RenderRef.lineAdd", a, 5, err)) return false;
       *out = Value::make_null();
       return true;
   }

   // 0x4e2640 RenderRef.plotRoute(ResourceRef a, ResourceRef b, I colour, F density,
   // Vector3 scale)I
   // Samples the process route (0x5fe450 path, 0x5fe454 len, 0x5fe458 s, 0x5fe45c e)
   // and feeds each sample to 0x4ec6a0. src models no route (findRoute keeps no
   // globals), so the exe's first check, DAT_005fe450 == 0, always returns 0.
   // SERVICE MISSING: process route globals and the line service above.
   bool rr_plot_route(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("RenderRef.plotRoute", a, 6, err)) return false;
       *out = Value::make_int(0);
       return true;
   }
   ```
   `want(..., N, ...)` uses N = 1 + parameter count: 3 / 5 / 6. Do not add state
   fields, do not invent a route or a line service.
2. Registry rows 6995-6997: point them at the functions and set the last field
   `false`:
   `lineCreate (Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;)I`,
   `lineAdd (Ljava.lang.Vector3;Ljava.lang.Vector3;IF)V`,
   `plotRoute (Ljava.util.resource.ResourceRef;Ljava.util.resource.ResourceRef;IFLjava.lang.Vector3;)I`.

## Files
- edit: `src/script/natives.cpp` (three functions after `rr_set_matrix_bone`; three rows)
- read only: `src/script/natives.h` (Entry, `want`), `tasks/091-particlesystem-no-service.md` (style)

## Done when
- the three rows have functions and the last field `false`
- each function carries its `// 0xADDR` comment; missing pieces are `SERVICE MISSING:`
  with the addresses above; any deviation is `INVENTED:`
- return values: lineCreate `Value::make_int(0)`, lineAdd `Value::make_null()`,
  plotRoute `Value::make_int(0)`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
