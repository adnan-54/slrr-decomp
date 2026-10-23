#include "audio/mixer.h"

#include <math.h>
#include <string.h>

namespace audio {

SoftwareMixer::SoftwareMixer()
{
    for (int i = 0; i < kBuses; i++) {
        // Sound resource defaults of FUN_00525d90 (mindist 5, maxdist 1000).
        min_distance_[i] = 5.0f;
        max_distance_[i] = 1000.0f;
        // set_sample users keep today's behaviour until create_bus overwrites.
        voice_limit_[i] = kVoicesPerBus;
        bus_3d_[i] = true;
    }
}

void SoftwareMixer::set_sample(int bus, const int16_t *pcm, size_t frames,
                               int rate)
{
    if (bus < 0 || bus >= kBuses)
        return;
    MixSample &s = samples_[bus];
    s.pcm.assign(pcm, pcm + frames * 2);
    s.rate = rate > 0 ? rate : kSampleRate;
}

void SoftwareMixer::clear_sample(int bus)
{
    if (bus < 0 || bus >= kBuses)
        return;
    samples_[bus] = MixSample();
}

const MixSample *SoftwareMixer::sample(int bus) const
{
    if (bus < 0 || bus >= kBuses || samples_[bus].frames() <= 0)
        return nullptr;
    return &samples_[bus];
}

void SoftwareMixer::set_bus_distances(int bus, float min_distance,
                                      float max_distance)
{
    if (bus < 0 || bus >= kBuses)
        return;
    min_distance_[bus] = min_distance <= 0.001f ? 0.001f : min_distance;
    max_distance_[bus] = max_distance;
}

float SoftwareMixer::bus_min_distance(int bus) const
{
    return bus >= 0 && bus < kBuses ? min_distance_[bus] : 0.0f;
}

float SoftwareMixer::bus_max_distance(int bus) const
{
    return bus >= 0 && bus < kBuses ? max_distance_[bus] : 0.0f;
}

// Sound__createBus 0x546860. INVENTED: the exe's "-1 when sound is not
// initialised" has no src counterpart (the software mixer always exists).
int SoftwareMixer::create_bus(const int16_t *pcm, size_t frames, int rate,
                              int instances, bool is3d, float min_distance,
                              float max_distance)
{
    int bus = 0;
    while (bus < kBuses && samples_[bus].frames() > 0)
        bus++;
    if (bus >= kBuses)
        return -1;
    if (frames == 0 || pcm == nullptr)
        return -2;
    set_sample(bus, pcm, frames, rate);
    set_bus_distances(bus, min_distance, max_distance);
    voice_limit_[bus] = instances < 9 ? instances : 8;
    bus_3d_[bus] = is3d;
    return bus;
}

// Sound__releaseBus 0x546b40 -> 0x5474f0. INVENTED: the exe leaves limit/3D
// until the bus is reused; resetting them keeps a stale bus_available() false.
void SoftwareMixer::release_bus(int bus)
{
    if (bus < 0 || bus >= kBuses)
        return;
    for (int slot = 0; slot < kVoicesPerBus; slot++)
        voices_[bus * kVoicesPerBus + slot].active = false;
    clear_sample(bus);
    voice_limit_[bus] = kVoicesPerBus;
    bus_3d_[bus] = true;
}

bool SoftwareMixer::bus_3d(int bus) const
{
    return bus >= 0 && bus < kBuses && bus_3d_[bus];
}

void SoftwareMixer::set_listener(const float pos[3])
{
    for (int i = 0; i < 3; i++)
        listener_[i] = pos[i];
}

// FUN_005476e0, software 3D branch (DAT_0079a0c0 == 0): the source point is
// taken off the radius sphere, its distance clamped to [min, max], and the gain
// is min/d * (max - d)/(max - min), capped at 1.
float SoftwareMixer::distance_gain(int bus, const float pos[3],
                                   float radius) const
{
    if (bus < 0 || bus >= kBuses)
        return 0.0f;
    const float min_d = min_distance_[bus];
    const float max_d = max_distance_[bus];
    float p[3];
    source_point(pos, radius, listener_, p);
    const float dx = p[0] - listener_[0];
    const float dy = p[1] - listener_[1];
    const float dz = p[2] - listener_[2];
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d <= min_d)
        d = min_d;
    if (max_d <= d)
        d = max_d;
    float linear = 1.0f;
    if (0.0f < max_d - min_d)
        linear = (max_d - d) / (max_d - min_d);
    const float gain = 1.0f / d * min_d * linear;
    return 1.0f <= gain ? 1.0f : gain;
}

// FUN_005476e0 scales the event volume by the distance gain and only then
// quantizes it through the dB table (0x5e0648); index 0 makes it return 0, so
// the voice is not started (FUN_005466d0) or is stopped (FUN_00548630).
// The bus 3D flag (sound resource flags bit0, DAT_0079d580): a 2D bus never
// gets distance gain (0x5466d0 ORs 4 into the voice flags).
float SoftwareMixer::voice_gain(int bus, const Voice &voice) const
{
    float volume = voice.volume;
    if (!(voice.flags & kSfx2D) && bus_3d(bus))
        volume *= distance_gain(bus, voice.pos, voice.radius);
    return volume_gain(volume);
}

int SoftwareMixer::mix(int16_t *out, int frames)
{
    if (frames <= 0)
        return 0;
    std::vector<int32_t> acc((size_t)frames * 2, 0);
    int mixed = 0;
    for (MixVoice &v : voices_) {
        if (!v.active)
            continue;
        const MixSample &s = samples_[v.bus];
        const int n = s.frames();
        if (n <= 0)
            continue;
        const double step =
            (double)s.rate * (double)v.pitch / (double)kSampleRate;
        if (!(step > 0.0))
            continue;
        // INVENTED: no pan or doppler; FUN_005476e0 also sets SetPan from the
        // listener basis and scales pitch by relative velocity (not ported).
        const float gain = v.gain;
        const bool loop = (v.flags & kSfxLoop) != 0;
        bool produced = false;
        for (int f = 0; f < frames; f++) {
            if (v.position >= (double)n) {
                if (!loop) {
                    v.active = false;
                    break;
                }
                while (v.position >= (double)n)
                    v.position -= (double)n;
            }
            if (v.position < 0.0)
                v.position = 0.0;
            const size_t i = (size_t)v.position;
            const size_t i1 = i + 1 < (size_t)n ? i + 1 : i;
            const float frac = (float)(v.position - (double)i);
            const float l = (float)s.pcm[2 * i] +
                            ((float)s.pcm[2 * i1] - (float)s.pcm[2 * i]) * frac;
            const float r =
                (float)s.pcm[2 * i + 1] +
                ((float)s.pcm[2 * i1 + 1] - (float)s.pcm[2 * i + 1]) * frac;
            acc[(size_t)f * 2] += (int32_t)lrintf(l * gain);
            acc[(size_t)f * 2 + 1] += (int32_t)lrintf(r * gain);
            produced = true;
            v.position += step;
        }
        if (produced)
            mixed++;
    }
    const size_t total = (size_t)frames * 2;
    for (size_t i = 0; i < total; i++) {
        int32_t x = acc[i];
        if (x > 32767)
            x = 32767;
        else if (x < -32768)
            x = -32768;
        out[i] = (int16_t)x;
    }
    return mixed;
}

int SoftwareMixer::bus_limit(int bus) const
{
    return bus >= 0 && bus < kBuses ? voice_limit_[bus] : 0;
}

bool SoftwareMixer::bus_available(int bus) const
{
    return sample(bus) != nullptr;
}

SoftwareMixer::MixVoice *SoftwareMixer::voice(int handle)
{
    if (handle < 0)
        return nullptr;
    const int bus = bus_of(handle);
    const int slot = slot_of(handle);
    if (bus >= kBuses || slot >= kVoicesPerBus)
        return nullptr;
    return &voices_[bus * kVoicesPerBus + slot];
}

int SoftwareMixer::play(int bus, const Voice &voice_in)
{
    if (!bus_available(bus))
        return -1;
    const int base = bus * kVoicesPerBus;
    for (int slot = 0; slot < kVoicesPerBus; slot++) {
        MixVoice &v = voices_[base + slot];
        if (v.active)
            continue;
        v = MixVoice();
        v.active = true;
        v.bus = bus;
        v.slot = slot;
        v.pitch = voice_in.pitch;
        v.flags = voice_in.flags;
        v.gain = voice_gain(bus, voice_in);
        v.active = v.gain > 0.0f;
        return handle_of(bus, slot);
    }
    return -1;
}

void SoftwareMixer::update(int handle, const Voice &voice_in)
{
    MixVoice *v = voice(handle);
    if (!v || !v->active)
        return;
    v->pitch = voice_in.pitch;
    v->flags = voice_in.flags;
    v->gain = voice_gain(v->bus, voice_in);
    if (v->gain <= 0.0f)
        v->active = false;
}

bool SoftwareMixer::playing(int handle)
{
    MixVoice *v = voice(handle);
    if (!v || !v->active)
        return false;
    if (v->flags & kSfxLoop)
        return true;
    return v->position < (double)samples_[v->bus].frames();
}

void SoftwareMixer::stop(int handle)
{
    MixVoice *v = voice(handle);
    if (v)
        v->active = false;
}

}  // namespace audio
