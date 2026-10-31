---
id: 260
status: implemented
branch: card/260
review:
depends: []
---
# traffic: path height probes 0x413b00 / 0x4136e0 / 0x40da20 + natives::load_priority accessor

## Goal
Port the three height probes TrafficCar__materialize 0x40d4e0 uses to put the front point on
the road surface (card 258 consumes them), and expose the engine global `DAT_005fe3bc`
(src `Scheduler::load_priority`) so the traffic flush call site can read it. Notes:
`notes/42-traffic-410680-services.md` §12, §9. No caller in this card besides the accessor
(the probes are used by card 258); keep them in `namespace traffic` next to `segment_project`.

## Exe evidence (src names: Segment p0/p1/m0/m1/len(+0x30)/curved(+0x38), traffic.h:19)
### 0x413b00 ctSegment__heightAt(ECX = seg, x, z, float *y, float *lat) -> bool (asm read in full)
```
if (!seg.curved) {                                          // 0x413b09 -> 0x413de6
    n = sqrtf(m0[0]*m0[0] + m0[2]*m0[2]);                   // NO 1e-5 guard here
    inv = 1.0f / n; dx = m0[0]*inv; dz = m0[2]*inv;
    t = dz*(z - p0[2]) + dx*(x - p0[0]);
    L = seg.len * n;                                        // len (+0x30), NOT flat_len
    if (t < 0.0f || L < t) return false;                    // COMISS/JC (NaN -> false)
    t = t / L;
    *y = (1.0f - t) * p0[1] + p1[1] * t;
    *lat = fabsf(dx*(z - p0[2]) + (-dz)*(x - p0[0]));
    return true;
}
// curved: the SAME code as src segment_project's curved branch (traffic.cpp, after
// `float nrm[2] = {-s.m1[0], -s.m1[2]};` through the side evaluation): same double
// 1e-5 (0x593588) guards on both normalizations, a/b >= 0 test, d = len*a/(b+a),
// Newton loop with double 0.01 (0x593560), 0.9f (0x59370c), double 0.1 (0x593568),
// k = 1.0f, prev_err = 0, the d < 0 / d > len clamps that break when old == clamp.
// Outputs differ:
*lat = (x - pos[0]) * side[0] + (z - pos[2]) * side[2];    // signed, pos from the last evaluation
*y   = pos[1];                                             // 0x413dd5: [EBP-0x14] = pos+4
return true;
```
Implement by sharing code with segment_project (e.g. a static helper that returns the final d,
pos and side) or by a copy; either way segment_project's behaviour must not change.

### 0x4136e0 ctPath__heightAt(ECX = path, x, z, float *y, float *lat) -> bool (decompile)
```
best = -1;
for (i = 0; i < path.segs.size(); i++) {                    // stride 0x54
    // body = 0x413b00 inlined on segs[i]
    if (segment_height(segs[i], x, z, &yi, &lati) && (best == -1 || fabsf(lati) < fabsf(*lat))) {
        *lat = lati; *y = yi; best = i;
    }
}
return best != -1;                                          // no endpoint fallback (unlike path_project)
```
### 0x40da20 CarPathRef__heightAt(ECX = ref {owner +0, path +4, dir +8}, x, z, float *y) -> bool (asm)
```
if (!ref.path) return false;                                // 0x40da24
float lat;
ok = ref.dir < 0 ? 0x4136e0(*path, x, z, y, &lat)           // CMP [ECX+8],0; JL
                 : 0x413b00(*(const Segment *)path, x, z, y, &lat);
return ok && fabsf(lat) < 10.0f;                            // 0x59373c = 0x41200000 = 10.0f, COMISS/SETA
```
src signature like `car_ref_pos_offset(const Path *path, int32_t dir, ...)` (traffic.cpp, the
`reinterpret_cast<const Segment *>(path)` cross case). Note `*y` is written whenever the
segment/path probe succeeds, even when the `< 10` test then fails — keep that.

### natives::load_priority
`DAT_005fe3bc` is `Scheduler::load_priority` (natives.cpp: `int load_priority = 0; // 0x5fe3bc`,
set by System.setLdPriority 0x4e56b0). Add `int load_priority(Vm &vm);` to natives.h next to
`double sim_time(Vm &vm);`, returning `sched(vm).load_priority` (same pattern as sim_time).

## Files
- edit: src/game/traffic.cpp only (segment_height 0x413b00 + path_height 0x4136e0 next to
  segment_project; car_ref_height 0x40da20 next to car_ref_pos_offset). Card 258 calls them from
  the same file, so no header declarations are needed.
- edit: src/script/natives.h, src/script/natives.cpp (load_priority accessor)

## Done when
- each ported function has a `// 0xADDR Name` comment; no logic beyond the evidence (else `INVENTED:`)
- segment_project unchanged in behaviour; `make -C src check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
