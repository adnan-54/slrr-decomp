---
id: 082
status: merged
branch: card/082
depends: [081]
---
# audio: bus allocation/release in SoftwareMixer (Sound__createBus) + post_event bus check

## Goal
A "bus" is one loaded sound sample with up to 8 playback voices (notes/14 §2.6). The exe
allocates it in `Sound__createBus` 0x546860 when a CompatibleSound uploads
(`CompatibleSound__vf3` 0x5260b0) and frees it in `Sound__releaseBus` 0x546b40 → 0x5474f0.
Today `SoftwareMixer` has a per-bus sample store and distances but nothing allocates a bus,
`bus_limit` is a constant 8 (exe: min(instances, 8)) and the bus 3D flag is not modelled.
Also `audio::post_event` misses the exe's `bus < 0` drop.

Files (≤3 src files): `src/audio/mixer.h`, `src/audio/mixer.cpp`, `src/audio/audio.cpp`.

## Exe evidence (Claude-traced)

### 0x546860 Sound__createBus(data, size, instances, is3d, min, max, name)
    if (!g_soundInit /*0x79a0be*/) return -1;
    bus = first b in 0..127 with buffers[b][0] /*0x79a180 + b*0x20*/ == 0;  none: return -1;
    b0 = DSBuffer__createFromWav(data, is3d);   // 0x5463a0: Wav__parse + CreateSoundBuffer + fill
    buffers[bus][0] = b0;
    if (b0 == 0) return -2;
    (3D interface + SetMin/MaxDistance only when is3d && 3D HW; Ds8Backend's business)
    if (min <= 0.001f) min = 0.001f;
    n = instances < 9 ? instances : 8;             // signed: 0 or negative stay as is
    limit[bus] /*0x79d380*/ = n; mind[bus] /*0x79d600*/ = min; flag3d[bus] /*0x79d580*/ = is3d;
    maxd[bus] /*0x79d800*/ = max; name[bus] /*0x7a8400*/ = name;
    for i in 1..n-1: buffers[bus][i] = DuplicateSoundBuffer(b0);   // fail -> 0
    b0->GetFrequency(&freq[bus] /*0x79d180*/);
    return bus;
Caller 0x5260b0 passes `is3d = ~CompatibleSound.flags & 1` and only calls when the file size
is > 0; it stores the result (also -1/-2) at CompatibleSound+0x60.

### 0x5474f0 (via Sound__releaseBus 0x546b40, which returns -1 when !g_soundInit)
    if (bus < 0x80) { for each of 8 buffers: Stop; release 3D iface if flag3d; Release; = 0;
                      0x79da00[bus] = 0; }
    return 0;

### 0x546a70 Sound__busVoiceLimit(bus) = bus < 0x80 ? limit[bus] : 0
### 0x546b80 Sound__busAvailable(bus) = bus < 0x80 && buffers[bus][0] != 0

### 2D bus (notes/14 §2.2, 0x5466d0)
`FUN_005466d0` ORs 4 (2D) into the voice flags when the bus 3D flag is 0, so a 2D bus never
gets distance gain.

### 0x4eb5c0 Game__postEvent (relevant lines)
    C = typed->+0xc;
    if (T != 0 && C != 0 && -1 < *(int*)(C + 0x60) /*bus*/) { ... post ... }
    else return -1;

## Changes
1. `src/audio/mixer.h`, public section of `SoftwareMixer`:
   ```cpp
   // Sound__createBus 0x546860: first bus without a sample; -1 when none is free, -2 when
   // the sample is empty (DSBuffer__createFromWav/0x5461e0 failure). frames of interleaved
   // stereo at rate. instances -> voice limit min(instances, 8); min floored at 0.001.
   int create_bus(const int16_t *pcm, size_t frames, int rate, int instances, bool is3d,
                  float min_distance, float max_distance);
   // Sound__releaseBus 0x546b40/0x5474f0: stop the bus voices and drop the sample.
   void release_bus(int bus);
   bool bus_3d(int bus) const;
   ```
   private: `int voice_limit_[kBuses]; bool bus_3d_[kBuses];` (ctor: limit 8 and 3D true for
   every bus, so `set_sample` users keep today's behaviour).
2. `src/audio/mixer.cpp`:
   - `create_bus`: loop `b = 0..kBuses-1`, take the first with `samples_[b].frames() <= 0`;
     none → -1. `frames == 0 || pcm == nullptr` → -2 (bus not taken). Else `set_sample(b, pcm,
     frames, rate)`, `set_bus_distances(b, min, max)` (already floors 0.001),
     `voice_limit_[b] = instances < 9 ? instances : 8`, `bus_3d_[b] = is3d`, return b.
     INVENTED: the exe's "-1 when sound is not initialised" has no src counterpart (the
     software mixer always exists).
   - `release_bus`: bounds check `0 <= bus < kBuses`; deactivate every `voices_` entry of
     that bus (exe: Stop on each buffer); `clear_sample(bus)`; `voice_limit_[bus] = 8`;
     `bus_3d_[bus] = true` (INVENTED reset values; the exe leaves the old ones until reuse).
   - `bus_limit(bus)`: return `voice_limit_[bus]` for valid buses (exe 0x546a70), else 0.
     `VoicePool::service` already compares per-bus counts with `bus_limit`.
   - `voice_gain`: apply `distance_gain` only when `!(voice.flags & kSfx2D) && bus_3d_[bus]`
     (0x5466d0); update the comment that says the 2D flag is not modelled.
3. `src/audio/audio.cpp` `post_event`: after `asset` lookup, `if (asset->bus < 0) return -1;`
   with comment `// 0x4eb5c0: -1 < C+0x60 required`.

Ds8Backend (dsound.cpp) reads the mixer's sample store and distances; its own buffer
creation follows `samples_`, so no change there in this card.

## Check
`make -C src check` stays green. No new test: the bus logic has no shipped-data oracle; it is
verified by comparing with 0x546860/0x5474f0 above.

## UNRESOLVED
- `0x79da00[bus]` (cleared on release): the writer that sets it was not traced; not modelled.
- Bus name pointer `0x7a8400[bus]` is only stored; no reader traced.
