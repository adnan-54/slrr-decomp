---
id: 064
status: draft
branch:
depends: []
---
# audio: SfxRef.nplay / SfxRef.stop

## Goal
Replace the two `java.util.resource.SfxRef` stubs (registry rows 4787-4788 in
`src/script/natives.cpp`) with natives that decode the exe's arguments, take the
exe's missing-handle branch and reproduce the exe's return values on every path
that `src/` can reach.

`src/` has the SFX building blocks (`audio::post_event`, `VoicePool`,
`AudioAssets`, `ChannelVolumes`) but **no service that can run them**: there is no
process-wide voice pool or asset table instance, no sound-resource parser that
fills `AudioAssets` (exe: typed sound asset `id` at `+0x60`, base `volume` at
`+0x6c`), and `VoicePool::stop` needs an `AudioBackend` that nothing owns. Until
that service exists, `nplay` returns the exe's own failure value **-1** (the value
`Game__postEvent` returns in every lookup/cull/volume failure branch) and `stop`
does nothing (it is `void`); both still log the exe's missing-handle error text
when the receiver has no native handle. `SfxRef.nplay`'s successful return is the
`instance` argument itself (0 when the caller passes 0), not a new id.

## Exe evidence (cleaned pseudocode, renamed fields)
Helpers:
- `getArgs` = FUN_004b1af0 (0x4b1af0): out0 = receiver (script instance), then one
  out per signature char in Java order (`F`/`I` copy the 32-bit argument value;
  `L`/`[` copy the object pointer). Card 061/063, notes/06.
- `unwrap` = FUN_004ecc70 (0x4ecc70, Native__instanceToHandle): receiver →
  ScriptObj's native handle (`DAT_007a2620` is the shared class key), 0 when absent.
  Notes/13 §1. In src: `value_handle(a[0])` (`Runtime::Handle`).
  The hidden `ECX` of the shared event helpers is that same handle; its ref-node
  `+8` (resource id) and `+0xc` (Resource*) are the roles of our
  `Runtime::Handle::id` / `::target` (notes/13 §1; no byte-offset mapping claimed).

### 0x4e4610 SfxRef.nplay(Vector3 pos, F radius, F pitch, F volume, I flags, I instance)I
    getArgs(param_1, &self, &pos, &radius, &pitch, &volume, &flags, &instance);
    h = unwrap(self, DAT_007a2620);
    if (h == 0) {                                          // 0x4e4610 stack error path
        log("vm_sfxref_play: scripted resource " + hex(h) + " not found in file \"" +
            callinfo->file /* +0x74 */ + "\" at line " + callinfo->line /* +0x70 */);
        return -1;
    }
    if (pos != 0) {                                        // getFloatField 0x4ec980
        posv[0] = getFloatField(pos, "x");                 // 0x5a34d8 (notes/06 "x")
        posv[1] = getFloatField(pos, "y");                 // 0x5a34f0 (notes/06 "y")
        posv[2] = getFloatField(pos, "z");                 // 0x5a34f8 (notes/06 "z")
        posPtr = posv;
    } else posPtr = 0;
    return postEvent(h /* ECX */, pitch, DAT_005dada0 * volume, flags, posPtr,
                     instance, radius, 0, 0);

### 0x4eb5c0 Game__postEvent(pitch, volume, flags, pos, instance, radius, vec1, vec2)I  (ECX = handle)
    if (pos != 0 && FUN_00568940(pos, radius) < 0) return -1;   // listener range cull
    RI = *(h + 0xc);
    if (RI == 0) return -1;                                     // uVar4 initial value
    if (*(RI + 0x7c) != 1) RI->vf5(+0x14)(1.0f);                // raw: 0x3f800000
    if (FUN_0051f4f0(0xa0000000, 0, 1.0f) < 0) return -1;
    T = RI->vf3(+0xc)(1.0f);
    if (T == 0 || (A = *(T + 0xc)) == 0 || *(int*)(A + 0x60) < 0) return -1;
    final_volume = *(float*)(A + 0x6c) * volume;                // asset base volume
    if (!(final_volume > 0.01f)) return -1;
    event.flags /* +0x00 */ = flags;  if (pos == 0) event.flags |= 4;   // 2D
    event.volume /* +0x04 */ = final_volume;
    event.zero   /* +0x08 */ = 0;
    event.pitch  /* +0x0c */ = pitch;
    if (pos) { event.pos /* +0x14 */ = pos; }
    if (vec1) { event.vec1 /* +0x20 */ = vec1; }
    else if (!(event.flags & 4)) { *(u64*)(event + 0x20) = 0; } // quirk: +0x24 untouched
    if (vec2) { event.vec2 /* +0x2c */ = vec2; }
    else { *(u64*)(event + 0x2c) = 0; }
    event.radius /* +0x38 */ = radius;
    FUN_00568660(A ? *(int*)(A + 0x60) : -1, instance, *(u32*)(h + 8) /* owner */, &event);
    return instance;

Raw failing/limit lines (Ghidra):
    iVar2 = FUN_004ecc70(local_c,DAT_007a2620);
    uVar4 = FUN_004eb5c0(local_20,DAT_005dada0 * local_1c,local_18,pfVar3,local_14,local_10,0,0);
    iVar3 = FUN_0051f4f0(0xa0000000,0,0x3f800000);
    _DAT_006024cc = *(float *)(iVar2 + 0x6c);
    uVar4 = param_5;                     // success: returns the instance argument

### 0x4e47d0 SfxRef.stop(I instance)V
    getArgs(param_1, &self, &instance);
    h = unwrap(self, DAT_007a2620);
    if (h == 0) {
        log("vm_sfxref_stop: scripted resource " + hex(h) + " not found in file \"" +
            callinfo->file + "\" at line " + callinfo->line);
        return;
    }
    FUN_004eba90(instance);               // ECX = h

### 0x4eba90 Sound__stopInstance(instance)  (ECX = handle)
    RI = *(h + 0xc);
    if (RI == 0) return;
    if (*(RI + 0x7c) != 1) RI->vf5(+0x14)(1.0f);                // raw: 0x3f800000
    if (FUN_0051f4f0(0x80000000, 0, 0) < 0) return;
    T = RI->vf3(+0xc)(1.0f);
    if (T == 0 || *(T + 0xc) == 0 || *(int*)(*(T + 0xc) + 0x60) < 0) return;
    FUN_00569020(instance, *(u32*)(h + 8) /* owner */);         // VoicePool::stop
    // raw: FUN_00569020(param_1,*(undefined4 *)(in_ECX + 8));

Constants: `0x3f800000` = 1.0f; `0.01f` = `audio::kVolumeThreshold`;
`DAT_005dada0` = effects channel volume = `audio::channel_volumes().effects` (1.0f
in the shipped image; notes/14 §1). `0xa0000000` / `0x80000000` are the typed-data
masks; nplay and stop use different ones.

## Files
- edit: `src/script/natives.cpp` only — add two functions before
  `}  // namespace` (line 4383), next to the Sound/SfxTable natives; change
  registry rows 4787-4788 from `nullptr, true` to the new functions with `false`.

## Changes
1. `sfxref_nplay(Vm &vm, const Args &a, Value *out, std::string *err)`
   ```cpp
   // 0x4e4610 SfxRef.nplay(Vector3,float radius,float pitch,float volume,int flags,int instance)
   float radius, pitch, volume; int32_t flags, instance;
   if (!want("SfxRef.nplay", a, 7, err) ||
       !get_f("SfxRef.nplay", a[2], &radius, err) ||
       !get_f("SfxRef.nplay", a[3], &pitch, err) ||
       !get_f("SfxRef.nplay", a[4], &volume, err) ||
       !get_i("SfxRef.nplay", a[5], &flags, err) ||
       !get_i("SfxRef.nplay", a[6], &instance, err))
       return false;
   (void)radius; (void)pitch; (void)volume; (void)flags; (void)instance;
   Runtime::Handle *h = value_handle(a[0]);
   if (!h) {
       std::fprintf(stderr, "vm_sfxref_play: scripted resource not found\n");
       *out = Value::make_int(-1);
       return true;
   }
   // Service gap: no process-wide VoicePool/AudioAssets instance exists and no
   // sound-resource parser fills AudioAssets, so Game__postEvent 0x4eb5c0 cannot
   // find the typed sound asset. The exe returns -1 in that case.
   *out = Value::make_int(-1);
   return true;
   ```
   The receiver-handle check must come **before** reading `pos` (exe order). Do not
   read the Vector3 fields here: the exe reads them only after the handle check, and
   the posting path is unreachable until the service exists. When the service lands,
   read `a[1]` with `dd_get_f(vm, pos, "x"/"y"/"z")` (0x4ec980 semantics: missing →
   0, notes/06 line 420) when `a[1].kind == Value::Obj`, else a null `pos` pointer.
2. `sfxref_stop(Vm &, const Args &a, Value *out, std::string *err)`
   ```cpp
   // 0x4e47d0 SfxRef.stop(int instance)
   int32_t instance;
   if (!want("SfxRef.stop", a, 2, err) ||
       !get_i("SfxRef.stop", a[1], &instance, err))
       return false;
   (void)instance;
   Runtime::Handle *h = value_handle(a[0]);
   if (!h)
       std::fprintf(stderr, "vm_sfxref_stop: scripted resource not found\n");
   // Service gap: no voice pool/backend instance; the exe stop path is a no-op
   // when the typed sound asset cannot be obtained (0x4eba90 guards).
   *out = Value::make_null();
   return true;
   ```
3. Registry:
   ```cpp
   {"java.util.resource.SfxRef", "nplay", "(Ljava.lang.Vector3;FFFII)I", sfxref_nplay, false},
   {"java.util.resource.SfxRef", "stop", "(I)V", sfxref_stop, false},
   ```

## Done when
- each function has its `// 0xADDR SfxRef.<name>` comment; the service gap and the
  -1 return are explained in a `shortcut:` comment with the exe branch cited
- missing handle: `nplay` returns -1, `stop` returns null, both log the
  `vm_sfxref_*: scripted resource not found` text to stderr (src convention, e.g.
  natives.cpp:1655)
- present handle: `nplay` returns -1 and `stop` returns null without touching the
  audio subsystem (exe `Game__postEvent` lookup-failure value ``-1``; `stop` is void)
- registry rows 4787-4788 point at the new functions with `engine = false`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name

## UNRESOLVED
- **Missing service (blocking the real path)**: `src/` declares `audio::post_event`
  (audio.cpp:334), `VoicePool` and `AudioAssets`, but nothing owns a process-wide
  instance, nothing parses sound resources into `AudioAssets`, and
  `VoicePool::stop` requires an `AudioBackend&` that is not reachable. Exe evidence
  for the asset data: `T = RI->vf3(+0xc)(1.0f); A = *(T + 0xc); id = *(int*)(A + 0x60);
  base = *(float*)(A + 0x6c)` with masks `0xa0000000` (nplay) / `0x80000000` (stop).
  Re-open this card (or a follow-up) when the SFX service exists; the posting path
  is then `audio::post_event(pool, assets, h->target->id, h->id, pitch,
  channel_volumes().effects * volume, flags, pos, instance, radius, nullptr, nullptr)`.
- `FUN_0051f4f0` / `FUN_00568940` / `FUN_00568660` / `FUN_00569020` are not
  re-traced here; semantics are taken from notes/14 §2.2 (typed lookup, 250 m cull,
  voice pool post/stop). Both typed masks being different between play and stop is
  observed, purpose unknown.
- The hidden `ECX = handle` in 0x4eb5c0/0x4eba90 is not shown by the decompiler (the
  call sites pass no explicit argument); the mapping `+8 = owner id`, `+0xc = RI` is
  from the ref-node layout in notes/13 §1 and is used only in the documented
  future-posting form above.
- Field-name data `0x5a34d8 / 0x5a34f0 / 0x5a34f8` were not re-read byte-wise in
  this session (Ghidra prints bare `DAT_` labels; raw line
  `FUN_004ec980(local_8,&DAT_005a34d8)`); they are `x`/`y`/`z` per notes/06 line 420.
  The port therefore reads `"x"`, `"y"`, `"z"` by name.
