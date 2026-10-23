#include "audio/audio.h"

#include <math.h>

#include <algorithm>

namespace audio {

// Copied verbatim from 0x5e0648 (hundredths of a dB, index = volume * 64).
const int32_t kVolumeDb[65] = {
    -10000, -3610, -3010, -2660, -2410, -2210, -2060, -1920, -1810, -1700,
    -1610,  -1530, -1450, -1380, -1320, -1260, -1200, -1150, -1100, -1050,
    -1010,  -970,  -930,  -890,  -850,  -820,  -780,  -750,  -720,  -690,
    -660,   -630,  -600,  -580,  -550,  -520,  -500,  -480,  -450,  -430,
    -410,   -390,  -370,  -350,  -330,  -310,  -290,  -270,  -250,  -230,
    -210,   -200,  -180,  -160,  -150,  -130,  -120,  -100,  -90,   -70,
    -60,    -40,   -30,   -10,   0,
};

int volume_db(float volume)
{
    // The exe truncates volume*64 and returns early when the index is 0.
    if (!(volume >= 1.0f / 64.0f))
        return -10000;
    float scaled = volume * 64.0f;
    if (scaled > 64.0f)
        return 0;  // indices above the table use 0 dB
    return kVolumeDb[(int)scaled];
}

float volume_gain(float volume)
{
    int db = volume_db(volume);
    if (db == -10000)
        return 0.0f;  // exe: index 0 leaves the voice silent
    return powf(10.0f, (float)db / 2000.0f);
}

bool in_range(const float pos[3], float radius, const float listener[3])
{
    float dx = pos[0] - listener[0];
    float dy = pos[1] - listener[1];
    float dz = pos[2] - listener[2];
    float d2 = dx * dx + dy * dy + dz * dz;
    if (radius > 0.0f) {
        d2 -= radius * radius;
        if (d2 <= kMinDistance2)
            d2 = kMinDistance2;
    }
    return d2 <= kCullDistance2;
}

bool attenuation_of(const float pos[3], float radius, float volume,
                    const float listener[3], float *out)
{
    float dx = pos[0] - listener[0];
    float dy = pos[1] - listener[1];
    float dz = pos[2] - listener[2];
    float d2 = dx * dx + dy * dy + dz * dz;
    if (radius > 0.0f) {
        d2 -= radius * radius;
        if (d2 <= kMinDistance2)
            d2 = kMinDistance2;
    }
    if (d2 > kCullDistance2)
        return false;
    *out = d2 / (volume * volume);
    return true;
}

void source_point(const float pos[3], float radius, const float listener[3],
                  float out[3])
{
    for (int i = 0; i < 3; i++)
        out[i] = pos[i];
    if (!(radius > 0.0f))
        return;
    const float v[3] = {listener[0] - pos[0], listener[1] - pos[1],
                        listener[2] - pos[2]};
    const float d2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (radius * radius <= d2) {
        const float s = radius / sqrtf(d2);
        for (int i = 0; i < 3; i++)
            out[i] = s * v[i] + pos[i];
    } else {
        for (int i = 0; i < 3; i++)
            out[i] = listener[i];
    }
}

VoicePool::VoicePool()
{
    // FUN_00568b60 zeroes state/bus and marks every slot free by attenuation.
    for (int i = 0; i < kVoices; i++) {
        memset(&slots[i], 0, sizeof slots[i]);
        slots[i].handle = -1;
        slots[i].attenuation = free_attenuation();
    }
}

void VoicePool::copy_event(Voice &slot, const SfxEvent &event)
{
    slot.flags = event.flags;
    slot.volume = event.volume;
    slot.reserved = (int32_t)event.reserved;
    slot.pitch = event.pitch;
    for (int i = 0; i < 3; i++) {
        slot.pos[i] = event.pos[i];
        slot.vec1[i] = event.vec1[i];
        slot.vec2[i] = event.vec2[i];
    }
    slot.radius = event.radius;
}

void VoicePool::free_slot(int index)
{
    slots[index].state = 0;
    slots[index].bus = 0;
    slots[index].attenuation = free_attenuation();
    slots[index].handle = -1;
}

int VoicePool::post(int32_t bus, int32_t instance, int32_t owner, const SfxEvent &event)
{
    float attenuation = 0.0f;
    if (!(event.flags & kSfx2D)) {
        if (!(event.volume > kVolumeThreshold))
            return -1;
        if (!attenuation_of(event.pos, event.radius, event.volume, listener, &attenuation))
            return -1;
    }
    if (instance == 0) {
        for (int i = 0; i < kVoices; i++) {
            if (slots[i].state != 0)
                continue;
            slots[i].state = 1;
            slots[i].bus = bus;
            slots[i].instance = 0;
            slots[i].owner = owner;
            slots[i].handle = -1;
            slots[i].attenuation = attenuation;
            copy_event(slots[i], event);
            return 0;
        }
        return -1;
    }
    int free_index = -1;
    for (int i = 0; i < kVoices; i++) {
        Voice &slot = slots[i];
        if (slot.state == 0) {
            free_index = i;
            continue;
        }
        if (slot.instance != instance || slot.owner != owner)
            continue;
        if (slot.state == 2 || slot.state == 3)
            slot.state = 3;
        slot.attenuation = attenuation;
        copy_event(slot, event);
        return 0;
    }
    if (free_index >= 0) {
        slots[free_index].state = 1;
        slots[free_index].bus = bus;
        slots[free_index].instance = instance;
        slots[free_index].owner = owner;
        slots[free_index].handle = -1;
        slots[free_index].attenuation = attenuation;
        copy_event(slots[free_index], event);
        return 0;
    }
    return -1;
}

int VoicePool::stop(int32_t instance, int32_t owner, AudioBackend &backend)
{
    for (int i = 0; i < kVoices; i++) {
        Voice &slot = slots[i];
        if (slot.state < 1 || slot.state > 3)
            continue;
        if (slot.instance != instance || slot.owner != owner)
            continue;
        if (slot.handle >= 0)
            backend.stop(slot.handle);
        free_slot(i);
        return 0;
    }
    return -1;
}

void VoicePool::service(AudioBackend &backend)
{
    int order[kVoices];
    for (int i = 0; i < kVoices; i++)
        order[i] = i;
    // Ascending attenuation; qsort's tie order in the exe is unspecified.
    std::sort(order, order + kVoices, [this](int a, int b) {
        if (slots[a].attenuation != slots[b].attenuation)
            return slots[a].attenuation < slots[b].attenuation;
        return a < b;
    });

    int bus_counts[kBuses] = {0};
    const int available = backend.available_voices();
    int reserved = 0;

    auto reserve = [&](Voice &slot) {
        if (reserved >= available)
            return false;
        if (slot.bus < 0 || slot.bus >= kBuses)
            return false;
        if (bus_counts[slot.bus] >= backend.bus_limit(slot.bus))
            return false;
        if (!backend.bus_available(slot.bus))
            return false;
        bus_counts[slot.bus]++;
        reserved++;
        return true;
    };

    for (int k = 0; k < kVoices; k++) {
        const int index = order[k];
        Voice &slot = slots[index];
        switch (slot.state) {
        case 1:
            if (!reserve(slot))
                free_slot(index);
            break;
        case 2:
            if (backend.playing(slot.handle)) {
                if (!reserve(slot)) {
                    backend.stop(slot.handle);
                    free_slot(index);
                }
            } else {
                free_slot(index);
            }
            break;
        case 3:
            if (!reserve(slot)) {
                backend.stop(slot.handle);
                free_slot(index);
            }
            break;
        default:
            break;
        }
    }

    for (int k = 0; k < kVoices; k++) {
        const int index = order[k];
        Voice &slot = slots[index];
        if (slot.state == 1) {
            int handle = backend.play(slot.bus, slot);
            if (handle < 0) {
                free_slot(index);
            } else {
                slot.handle = handle;
                slot.state = 2;
            }
        } else if (slot.state == 2) {
            // A loop voice is re-posted as state 3 each frame; if it was not,
            // this frame ends it.
            if (slot.flags & kSfxLoop) {
                if (slot.handle >= 0)
                    backend.stop(slot.handle);
                free_slot(index);
            }
        } else if (slot.state == 3) {
            backend.update(slot.handle, slot);
            slot.state = 2;
        }
    }
}

float &ChannelVolumes::operator[](int channel)
{
    switch (channel) {
    case 0: return effects;
    case 1: return music;
    case 2: return engine;
    default: break;
    }
    static float zero = 0.0f;
    return zero;
}

float ChannelVolumes::operator[](int channel) const
{
    switch (channel) {
    case 0: return effects;
    case 1: return music;
    case 2: return engine;
    default: break;
    }
    return 0.0f;
}

ChannelVolumes &channel_volumes()
{
    static ChannelVolumes volumes;  // DAT_005dada0/a4/a8
    return volumes;
}

HardwareCaps &hardware_caps()
{
    static HardwareCaps caps;  // BSS at 0x79a0d0 before GetCaps
    return caps;
}

// 0x546be0
bool has_3d_hardware()
{
    return hardware_caps().free_hw_3d_all > 3;
}

// 0x546bf0
bool has_mix_hardware()
{
    return hardware_caps().free_hw_mixing_all > 3;
}

void AudioAssets::set(int32_t resource, int32_t bus, float volume)
{
    assets[resource] = Asset{bus, volume};
}

const AudioAssets::Asset *AudioAssets::get(int32_t resource) const
{
    auto it = assets.find(resource);
    return it == assets.end() ? nullptr : &it->second;
}

int post_event(VoicePool &pool, const AudioAssets &assets, int32_t resource,
               int32_t owner, float pitch, float volume, uint32_t flags,
               const float *pos, int32_t instance, float radius,
               const float *vec1, const float *vec2)
{
    if (pos && !in_range(pos, radius, pool.listener))
        return -1;
    const AudioAssets::Asset *asset = assets.get(resource);
    if (!asset)
        return -1;
    if (asset->bus < 0)
        return -1;  // 0x4eb5c0: -1 < C+0x60 required
    float final_volume = asset->volume * volume;
    if (!(final_volume > kVolumeThreshold))
        return -1;

    SfxEvent event = {};
    event.flags = pos ? flags : (flags | kSfx2D);
    event.volume = final_volume;
    event.pitch = pitch;
    event.radius = radius;
    if (pos) {
        for (int i = 0; i < 3; i++)
            event.pos[i] = pos[i];
    }
    if (vec1) {
        for (int i = 0; i < 3; i++)
            event.vec1[i] = vec1[i];
    }
    if (vec2) {
        for (int i = 0; i < 3; i++)
            event.vec2[i] = vec2[i];
    }
    pool.post(asset->bus, instance, owner, event);
    return instance;
}

}  // namespace audio
