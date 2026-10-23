#include "audio/sfx.h"

#include <math.h>
#include <stddef.h>

namespace audio {

SfxTable::SfxTable()
{
    for (int i = 0; i < 16; i++) {
        items[i].next = -1;
        items[i].prev = -1;
        items[i].owner = 0;
        items[i].resource = 0;
        items[i].inv_pitch = 0.0f;
        items[i].pmin = 0.0f;
        items[i].pmax = 0.0f;
        items[i].vmin = 0.0f;
        items[i].vmax = 0.0f;
    }
    count = 0;
    value_scale = 1.0f;
}

bool SfxTable::add_item(int32_t resource, float pitch, float pmin, float pmax,
                        float vmin, float vmax)
{
    if (count >= 16)
        return false;
    SfxItem &item = items[count];
    item.prev = count > 0 ? count - 1 : -1;
    item.next = -1;
    if (count > 0)
        items[count - 1].next = count;
    item.owner = resource;  // 0x40cac0: node+8 = *(RI+0x80), the id word
    item.resource = resource;
    item.inv_pitch = 1.0f / pitch;
    item.pmin = pmin;
    item.pmax = pmax;
    item.vmin = vmin;
    item.vmax = vmax;
    count++;
    return true;
}

void SfxTable::clear()
{
    for (int i = 0; i < 16; i++) {
        items[i].next = -1;
        items[i].prev = -1;
        items[i].owner = 0;
        items[i].resource = 0;
    }
    count = 0;
}

int SfxTable::evaluate(float value, float volume, uint32_t flags,
                       const float *pos, int32_t instance, float radius,
                       const float *vec1, const float *vec2,
                       const float listener[3], SfxEmitFn emit,
                       void *ctx) const
{
    if (!(value > 0.0f))
        return 0;
    if (pos && !in_range(pos, radius, listener))
        return 0;

    value *= value_scale;
    bool no_match = true;
    bool seen = false;
    int posted = 0;

    for (int i = 0; i < count; i++) {
        const SfxItem &item = items[i];
        if (item.owner == 0)
            break;
        seen = true;
        const float pmin = item.pmin;
        const float pmax = item.pmax;
        if (pmin <= value && value <= pmax) {
            const float t = (value - pmin) / (pmax - pmin);
            float blend = 1.0f;
            if (i > 0) {
                const float prev_pmax = items[i - 1].pmax;
                if (prev_pmax < value) {
                    no_match = false;
                } else {
                    const float overlap = prev_pmax - pmin;
                    if (0.0f < overlap) {
                        const float x = (value - pmin) / overlap;
                        if (x > 0.5f) {
                            const float y = 1.0f - ((x - 0.5f) * 2.0f);
                            blend = ((1.0f - y * y) + 1.0f) * 0.5f;
                        } else {
                            blend = (x + x) * (x + x) * 0.5f;
                        }
                    }
                }
            }
            if (i < count - 1) {
                const float next_pmin = items[i + 1].pmin;
                if (next_pmin <= value && 0.0f < pmax - next_pmin) {
                    float x = 1.0f - (value - next_pmin) / (pmax - next_pmin);
                    if (x > 0.5f) {
                        const float y = 1.0f - ((x - 0.5f) * 2.0f);
                        blend = (1.0f - y * y) + 1.0f;
                    } else {
                        blend = (x + x) * (x + x);
                    }
                    blend *= 0.5f;
                }
            }
            if (blend > 0.0f) {
                SfxEmit out;
                out.resource = item.resource;
                out.owner = item.owner;
                out.pitch = item.inv_pitch * value;
                out.volume = blend * volume *
                             (item.vmax * t + (1.0f - t) * item.vmin);
                out.flags = flags;
                out.pos = pos;
                out.instance = instance;
                out.radius = radius;
                out.vec1 = vec1;
                out.vec2 = vec2;
                emit(ctx, out);
                posted++;
                no_match = false;
            }
        }
    }
    if (!seen)
        return posted;

    if (no_match && 0.0f < value && value <= 2500.0f) {
        int best = 0;
        float best_pmin = items[0].pmin;
        for (int i = 1; i < count && items[i].owner != 0; i++) {
            if (items[i].pmin < best_pmin) {
                best = i;
                best_pmin = items[i].pmin;
            }
        }
        if (best_pmin <= 2500.0f) {
            const SfxItem &item = items[best];
            SfxEmit out;
            out.resource = item.resource;
            out.owner = item.owner;
            out.pitch = item.inv_pitch * value;
            out.volume = (1.0f - (item.pmin - value) / item.pmin) * volume *
                         item.vmin;
            out.flags = flags;
            out.pos = pos;
            out.instance = instance;
            out.radius = radius;
            out.vec1 = vec1;
            out.vec2 = vec2;
            emit(ctx, out);
            posted++;
        }
    }
    return posted;
}

void EngineSfx::emit_node(int32_t resource, float pitch, float volume,
                          uint32_t flags, const float *pos, int32_t instance,
                          const float *vec1, const float *vec2, SfxEmitFn emit,
                          void *ctx) const
{
    SfxEmit out;
    out.resource = resource;
    out.owner = resource;  // 0x40cac0: node+8 = *(RI+0x80), the id word
    out.pitch = pitch;
    out.volume = volume;
    out.flags = flags;
    out.pos = pos;
    out.instance = instance;
    out.radius = 0.0f;
    out.vec1 = vec1;
    out.vec2 = vec2;
    emit(ctx, out);
}

int EngineSfx::emit_table(int table, float value, float volume,
                          const float *pos, const float listener[3],
                          int32_t instance, const float *vec1,
                          const float *vec2, SfxEmitFn emit, void *ctx) const
{
    if (table < 0 || table > 2)
        return 0;
    return tables[table].evaluate(value, volume, 3u, pos, instance, 0.0f,
                                  vec1, vec2, listener, emit, ctx);
}

void EngineSfx::update(const EngineState &state, float engine_volume,
                       float effects_volume, const float *pos,
                       const float listener[3], int32_t instance,
                       SfxEmitFn emit, void *ctx)
{
    if (!(pitch_scale > 0.0f))
        return;

    const float throttle = fabsf(state.throttle);
    if (state.on_ground) {
        const float value = fabsf(state.rpm * 9.549296f) + 1.0f;
        const float scaled = value * pitch_scale;
        if (throttle <= 0.1f) {
            emit_table(tables[1].count > 0 ? 1 : 0, scaled, engine_volume, pos,
                       listener, instance, state.vec1, state.vec2, emit, ctx);
        } else if (!state.shifting) {
            if (throttle > 0.5f || tables[1].count == 0) {
                emit_table(0, scaled, engine_volume, pos, listener, instance,
                           state.vec1, state.vec2, emit, ctx);
            } else {
                const float x = (throttle - 0.1f) * 2.5f;
                emit_table(1, scaled, engine_volume * (1.0f - x), pos,
                           listener, instance, state.vec1, state.vec2, emit,
                           ctx);
                emit_table(0, scaled, engine_volume * x, pos, listener,
                           instance, state.vec1, state.vec2, emit, ctx);
            }
        } else if (state.starter && starter_resource != 0) {
            emit_node(starter_resource, pitch_scale, engine_volume, 3u, pos,
                      instance, state.vec1, state.vec2, emit, ctx);
        }
        if (value > 1.0f && tables[2].count > 0) {
            const float volume =
                ((1.0f - exhaust_min_vol) * throttle + exhaust_min_vol) *
                engine_volume;
            emit_table(2, scaled, volume, pos, listener, instance, state.vec1,
                       state.vec2, emit, ctx);
        }
        if (value > 1.0f && state.nitro_firing && nitro_resource != 0 &&
            state.nitro_amount > 0.01f) {
            emit_node(nitro_resource, scaled / nitro_pitch_divisor,
                      state.nitro_amount * engine_volume, 3u, pos, instance,
                      state.vec1, state.vec2, emit, ctx);
        }
        if (state.nitro_start && nitro_start_resource != 0) {
            emit_node(nitro_start_resource, pitch_scale, engine_volume, 0u,
                      pos, instance, state.vec1, state.vec2, emit, ctx);
        }
    } else {
        const float value =
            (fabsf(state.air_speed) * 9.549296f + 1.0f) * pitch_scale;
        emit_table(tables[2].count > 0 ? 2 : 0, value, engine_volume, pos,
                   listener, instance, state.vec1, state.vec2, emit, ctx);
    }

    if (exhaust_timer[0] > 0.0f) {
        if (exhaust_resource[0] != 0) {
            float v = exhaust_timer[0] * 0.0005f;
            if (v >= 4.0f)
                v = 4.0f;
            const float rise = v >= 1.0f ? 1.0f : v;
            emit_node(exhaust_resource[0], (rise + 1.0f) * pitch_scale,
                      v * effects_volume, 3u, pos, instance, state.vec1,
                      state.vec2, emit, ctx);
        }
        exhaust_timer[0] = 0.0f;
    }
    if (exhaust_timer[1] > 0.0f && exhaust_resource[1] != 0) {
        float v = exhaust_timer[1] * 0.0001f;
        if (v >= 4.0f)
            v = 4.0f;
        const float rise = v >= 1.0f ? 1.0f : v;
        emit_node(exhaust_resource[1], (rise + 1.0f) * pitch_scale,
                  v * effects_volume, 3u, pos, instance, state.vec1,
                  state.vec2, emit, ctx);
    }
    if (state.horn && state.horn_id >= 0 && state.horn_id < 4 &&
        horn_resource[state.horn_id] != 0) {
        emit_node(horn_resource[state.horn_id],
                  horn_pitch[state.horn_id] * pitch_scale, effects_volume, 3u,
                  pos, instance, state.vec1, state.vec2, emit, ctx);
    }
}

}  // namespace audio
