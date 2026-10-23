---
id: 083
status: todo
branch:
depends: [081, 082]
---
# audio/script: process-wide SFX service + SfxRef.nplay/stop on the real path

## Goal
Give src one owner for the exe's static SFX state (notes/14 §2.6): the voice pool
(0x7a5b98, set up by C++ static init 0x569420 → `SfxPool__init` 0x568b60), the bus tables
(`SoftwareMixer`, card 082) and the per-sound-resource state the exe keeps in
`Resource_Sound_Data` (RI+0xb0: loaded bit RI+0x94, `CompatibleSound*` at RI+0xbc with bus
+0x60 and volume +0x6c). Then replace the no-service stubs of `SfxRef.nplay` (0x4e4610) and
`SfxRef.stop` (0x4e47d0) from card 064 with the exe path (`Game__postEvent` 0x4eb5c0,
`Handle__stopSfx` 0x4eba90).

Files (≤3 src files): `src/audio/sound.h`, `src/audio/sound.cpp`, `src/script/natives.cpp`
(**only the `sfxref_nplay` / `sfxref_stop` functions and one new helper right above them**;
other agents edit Text/RenderRef/Camera natives in the same file).

## Exe evidence (Claude-traced; asm checked where ECX matters)

### 0x4e4610 SfxRef.nplay(Vector3 pos, F radius, F pitch, F volume, I flags, I instance)I
    getArgs(ctx, &self, &pos, &radius, &pitch, &volume, &flags, &instance);
    h = Native__instanceToHandle(self);            // 0x4ecc70
    if (!h) { log "vm_sfxref_play: scripted resource %x not found in file ..."; return -1; }
    if (pos) { p = {field x, field y, field z} (0x4ec980, 0x5a34d8/f0/f8) }
    return Game__postEvent(ECX = h, pitch, DAT_005dada0 /*effects*/ * volume, flags,
                           pos ? p : 0, instance, radius, 0, 0);

### 0x4eb5c0 Game__postEvent(pitch, volume, flags, pos, instance, radius, vec1, vec2), ECX = node
    if (pos && SfxPool__inRange(pos, radius) < 0) return -1;     // 0x568940, 250 m
    RI = node+0xc; if (!RI) return -1;
    if (RI+0x7c != 1) RI->vf5(1.0);                               // not traced
    if (Resource__getTyped(ECX = RI, 0xa0000000, 0, 1.0) < 0) return -1;
    T = RI->vf3(1.0); C = T ? T+0xc : 0;
    if (!T || !C || C+0x60 < 0) return -1;
    ... event, SfxPool__post(C+0x60, instance, node+8, &event) ... return instance;
`audio::post_event` (audio.cpp) already implements everything after the typed lookup
(range cull, volume = asset volume * volume, `<= 0.01` drop, event build, pool post; card
082 adds the `bus < 0` drop).

### Typed lookup / load trigger: Resource__getTyped 0x51f4f0 → ResourceNode__requestLoad 0x518a80
- returns 0 when RI is loaded (RI+0x94 bit0), else 0x80000001.
- not loaded, args `(0, 0)` (stop) → synchronous: read record data, `+0x9c = 1000.0`, vf10
  `Resource_Sound__vf10` 0x525cf0 → `Resource_Sound_Data__parse` 0x525d90 →
  `Compatible__create(5, 0 /*sync*/, src, 0, owner, 0)` 0x455e80 (whole-file read; size < 1 →
  no CompatibleSound) → `Compatible__decodeSync` 0x526030 → `CompatibleSound__vf3` 0x5260b0
  (`bus = Sound__createBus(...)` when size > 0), then `setLoaded(1)`.
- not loaded, args `(0, 1.0)` (nplay) on a package record → a streaming request is queued
  and getTyped returns < 0: **this nplay returns -1**; the sound is loaded later.
- vf10 sets the loaded bit even when no sourcefile was read or the file is missing: the
  resource is "loaded" but has no CompatibleSound, so every later post returns -1.

### 0x4e47d0 SfxRef.stop(I)V → 0x4eba90 Handle__stopSfx(instance), ECX = h
    if (!h) { log "vm_sfxref_stop: scripted resource %x not found in file ..."; return; }
    RI = h+0xc; if (!RI) return;
    if (RI+0x7c != 1) RI->vf5(1.0);
    if (Resource__getTyped(ECX = RI, 0x80000000, 0, 0) < 0) return;
    T = RI->vf3(1.0);
    if (T && T+0xc && (T+0xc)+0x60 >= 0) SfxPool__stop(instance, h+8);   // 0x569020

### Unload (Resource_Sound__vf7 0x526070, CompatibleSound__vf0 0x525c40)
    if (loaded) { delete C (bus >= 0: Sound__releaseBus(bus)); RI+0x98 = 0; setLoaded(0); }

## Changes
1. `src/audio/sound.h` (add to card 081's header; include `audio/audio.h`, `audio/mixer.h`,
   `<map>`):
   ```cpp
   // Process-wide SFX state. Exe: static pool 0x7a5b98 (SfxPool__init 0x568b60 from the CRT
   // static initialiser 0x569420), bus tables 0x79a180.. (SoftwareMixer) and per-resource
   // Resource_Sound_Data (RI+0xb0). Keys are the caller's resource node pointers.
   class SfxService {
     public:
       VoicePool pool;
       AudioAssets assets;
       SoftwareMixer mixer;
       // shortcut: always the software mixer; a Windows host swaps in Ds8Backend later.
       AudioBackend *backend = &mixer;

       bool loaded(const void *res) const;          // RI+0x94 bit0
       // Resource_Sound__vf10 0x525cf0 (sync path). params == nullptr: no sourcefile;
       // file == nullptr or empty: File__readAll failed. Marks res loaded in every case.
       void load(const void *res, const SoundParams *params,
                 const std::vector<uint8_t> *file);
       // Resource_Sound__vf7 0x526070: release the bus, forget the asset, not loaded.
       void unload(const void *res);
       // AudioAssets key of res (0 when never loaded; the key space starts at 1).
       int32_t key(const void *res) const;
     private:
       struct Entry { int32_t key = 0; };
       std::map<const void *, Entry> entries_;   // present = loaded
       int32_t next_key_ = 1;   // INVENTED: AudioAssets is keyed by int32
   };
   SfxService &sfx_service();   // function-local static (exe: static storage)
   ```
2. `src/audio/sound.cpp`:
   - `load`: return if `loaded(res)`. Create `entries_[res]` with `key = next_key_++`.
     If `params && file && !file->empty()` (0x455e80: size < 1 → no CompatibleSound):
     `WavView w; std::vector<int16_t> pcm; int bus = -2;`
     `if (parse_wav(file->data(), file->size(), &w) == 0 && decode_pcm(w, &pcm))`
     `bus = mixer.create_bus(pcm.data(), pcm.size() / 2, (int)w.rate, params->instances,`
     `(~params->flags & 1) != 0, params->mindist, params->maxdist);` (0x5260b0 passes
     `~flags & 1` as is3d); then `assets.set(key, bus, params->volume)` — the asset exists
     even with a negative bus (exe: C+0x60 = -1/-2, post drops it).
   - `unload`: find entry; if `assets.get(key)` has `bus >= 0` → `mixer.release_bus(bus)`;
     `assets.assets.erase(key)`; erase the entry.
   - `key`: entry key or 0. `loaded`: entry present.
3. `src/script/natives.cpp`, add `#include "audio/sound.h"` (`platform/fs.h` is already
   included) and above `sfxref_nplay`:
   ```cpp
   // Resource_Sound__vf10 0x525cf0 sync path for a type-6 (res_sound) record.
   void sfx_load(Runtime::Resource *r)
   {
       audio::SfxService &s = audio::sfx_service();
       audio::SoundParams params;
       const bool have = r->record && r->package && r->record->data_size > 0 &&
           audio::parse_sound_params((const char *)r->package->rpk.data(*r->record),
                                     r->record->data_size, &params);
       std::vector<uint8_t> file;
       bool read = false;
       if (have) {
           std::string path = game_rt()->resources().resolve_path(params.sourcefile);
           std::string err;
           read = !path.empty() && fs::read_file(path, &file, &err);
       }
       s.load(r, have ? &params : nullptr, read ? &file : nullptr);
   }
   ```
   `sfxref_nplay` (keep the arg decoding and the not-found log/-1 as they are):
   ```cpp
   float p[3];
   const float *pos = nullptr;
   if (a[1].kind == Value::Obj && a[1].obj) {      // read after the handle check (exe order)
       p[0] = dd_get_f(vm, a[1].obj, "x"); p[1] = dd_get_f(vm, a[1].obj, "y");
       p[2] = dd_get_f(vm, a[1].obj, "z"); pos = p;
   }
   audio::SfxService &s = audio::sfx_service();
   Runtime::Resource *r = h->target;
   if ((pos && !audio::in_range(pos, radius, s.pool.listener)) || !r || r->type != 6) {
       *out = Value::make_int(-1); return true;   // 0x4eb5c0 cull / no RI
   }
   if (!s.loaded(r)) {
       sfx_load(r);
       // INVENTED timing: 0x518a80 queues a streaming load and this post returns -1; src
       // has no streaming queue, so the load completes now and the next call plays.
       *out = Value::make_int(-1); return true;
   }
   *out = Value::make_int(audio::post_event(s.pool, s.assets, s.key(r), (int32_t)h->id,
       pitch, audio::channel_volumes().effects * volume, (uint32_t)flags, pos, instance,
       radius, nullptr, nullptr));
   return true;
   ```
   (`dd_get_f` takes the `Vm &`; name the first parameter `vm`. `r->type != 6`: INVENTED —
   the typed object of other resource kinds is not a CompatibleSound; treat as no asset.)
   `sfxref_stop` (keep the decoding and the not-found log):
   ```cpp
   Runtime::Resource *r = h ? h->target : nullptr;
   if (r && r->type == 6) {
       audio::SfxService &s = audio::sfx_service();
       if (!s.loaded(r)) sfx_load(r);             // 0x518a80 (0,0): synchronous load
       const audio::AudioAssets::Asset *as = s.assets.get(s.key(r));
       if (as && as->bus >= 0) s.pool.stop(instance, (int32_t)h->id, *s.backend);  // 0x569020
   }
   *out = Value::make_null();
   ```
   Remove the two `shortcut:` comments of card 064 that this replaces.

## Check
`make -C src check` green (vmboot boot path runs `game.Init`, whose constructor precaches
three menu SfxRefs; nothing in the check calls nplay with an asset yet). No synthetic test.

## UNRESOLVED
- `RI->vf5(1.0)` (called when RI+0x7c != 1) and `ResourceRef.precache` →
  `Handle__precache` 0x4e9830 → `0x468b00(0xa0000001, 1.0, 0, 1.0)` were not traced, so
  precache does not load sounds here (exe may load them ahead; then the first nplay plays).
- When the streaming queue (0x51e5c0, drained by 0x517f60/0x518f80 in the main loop)
  completes; the src "load now, return -1 once" is the closest exe-consistent behaviour.
- ResourceRef.unload of a sound (0x51a1b0 vf[+0x1c] = `Resource_Sound__vf7`) should call
  `sfx_service().unload(r)`; that row belongs to the ResourceRef natives (not this card).
- Size-0 type-6 records (folders/definitions): `data` pointer passed to 0x525d90 not checked;
  src treats them as "no sourcefile".
