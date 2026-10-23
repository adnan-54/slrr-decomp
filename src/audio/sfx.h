// Looping SFX tables and the engine/vehicle SFX evaluator (notes/14):
// SfxTable at e+0xfc/0x344/0x58c (0x248 bytes, 16 entries of 0x24) and the
// type_car__vf13 0x494910 consumer.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "audio/audio.h"

namespace audio {

// Entry: intrusive list node (next/prev are table indices here, -1 = none),
// owner word = resource id (0x40cac0: *(RI+0x80)), resource, 1/pitch and
// pitch/volume ranges.
struct SfxItem {
    int32_t next;     // +0x00
    int32_t prev;     // +0x04
    int32_t owner;    // +0x08 = resource id (0x40cac0: *(RI+0x80))
    int32_t resource; // +0x0c
    float inv_pitch;  // +0x10 = 1.0 / pitch
    float pmin;       // +0x14
    float pmax;       // +0x18
    float vmin;       // +0x1c
    float vmax;       // +0x20
};
static_assert(sizeof(SfxItem) == 0x24, "SfxItem is 0x24");
static_assert(offsetof(SfxItem, owner) == 0x08, "entry owner");
static_assert(offsetof(SfxItem, resource) == 0x0c, "entry resource");
static_assert(offsetof(SfxItem, inv_pitch) == 0x10, "entry 1/pitch");
static_assert(offsetof(SfxItem, pmin) == 0x14, "entry pmin");
static_assert(offsetof(SfxItem, vmax) == 0x20, "entry vmax");

// One Game__postEvent argument set produced by the evaluator. pos/vec1/vec2
// point at caller storage and are valid only during the callback.
struct SfxEmit {
    int32_t resource;
    int32_t owner;
    float pitch;
    float volume;
    uint32_t flags;
    const float *pos;
    int32_t instance;
    float radius;
    const float *vec1;
    const float *vec2;
};
using SfxEmitFn = void (*)(void *ctx, const SfxEmit &emit);

struct SfxTable {
    SfxItem items[16];   // +0x000
    int32_t count;       // +0x240
    float value_scale;   // +0x244, multiplies the input value

    SfxTable();
    // 0x4b5980: rejects a full table, links the entry and stores 1.0/pitch.
    bool add_item(int32_t resource, float pitch, float pmin, float pmax,
                  float vmin, float vmax);
    // 0x4b5a60: unlink/clear, count = 0.
    void clear();
    int items_count() const { return count; }

    // FUN_004eb7a0. value is scaled by value_scale before the range match.
    // Returns the number of events emitted.
    int evaluate(float value, float volume, uint32_t flags, const float *pos,
                 int32_t instance, float radius, const float *vec1,
                 const float *vec2, const float listener[3],
                 SfxEmitFn emit, void *ctx) const;
};

// Per-frame vehicle state consumed by type_car__vf13. The caller applies the
// exe's outer gating (part hook, speed/flags) and sets on_ground.
struct EngineState {
    float rpm = 0.0f;            // car+0x1e80, rad/s (smoothed)
    float throttle = 0.0f;       // car+0x1de4
    bool shifting = false;       // car+0x1dfc != 0
    bool on_ground = true;
    float air_speed = 0.0f;      // wheel surface speed, rad/s equivalent
    bool starter = false;        // e+0xd0 > 0
    bool horn = false;           // horn currently linked/active
    int32_t horn_id = 0;         // car+0x1e00
    bool nitro_firing = false;   // e+0xc30 != 0
    float nitro_amount = 0.0f;   // FUN_004f5ab0 result
    bool nitro_start = false;    // rising edge of the nitro-start flag
    const float *vec1 = nullptr;
    const float *vec2 = nullptr;
};

// Engine block e + 0x2050: three tables, direct nodes and timers.
struct EngineSfx {
    SfxTable tables[3];
    float pitch_scale = 1.0f;  // DAT_005fe10c (0 disables the update)
    float exhaust_min_vol = 0.0f;  // e+0x7d4
    int32_t starter_resource = 0;      // node e+0x7f8
    int32_t nitro_resource = 0;        // node e+0x828
    int32_t nitro_start_resource = 0;  // node e+0x818
    int32_t horn_resource[4] = {};     // nodes e+0x838 + id*0x10
    float horn_pitch[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // e+0x878 + id*4
    int32_t exhaust_resource[2] = {};  // car+0x1f84 / car+0x1f94
    float exhaust_timer[2] = {0.0f, 0.0f};  // car+0x224c / car+0x2248
    float nitro_pitch_divisor = 1.0f;  // e+0x814
    float part_loop_volume = 1.0f;  // Part +0x144 (consumer unresolved)
    float part_loop_pitch = 1.0f;   // Part +0x148

    // Part.setSfxLoopParams 0x4cdb80.
    void set_part_loop_params(float volume, float pitch)
    {
        part_loop_volume = volume;
        part_loop_pitch = pitch;
    }

    // type_car__vf13. engine_volume/effects_volume are DAT_005dada8/0x5dada0.
    void update(const EngineState &state, float engine_volume,
                float effects_volume, const float *pos,
                const float listener[3], int32_t instance,
                SfxEmitFn emit, void *ctx);

  private:
    int emit_table(int table, float value, float volume, const float *pos,
                   const float listener[3], int32_t instance,
                   const float *vec1, const float *vec2, SfxEmitFn emit,
                   void *ctx) const;
    void emit_node(int32_t resource, float pitch, float volume, uint32_t flags,
                   const float *pos, int32_t instance, const float *vec1,
                   const float *vec2, SfxEmitFn emit, void *ctx) const;
};

}  // namespace audio
