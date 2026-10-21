#include "audio/mixer.h"

#include <math.h>
#include <string.h>

namespace audio {

SoftwareMixer::SoftwareMixer()
{
    for (int i = 0; i < kBuses; i++) {
        min_distance_[i] = 1.0f;
        max_distance_[i] = 100.0f;
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
    min_distance_[bus] = min_distance;
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

void SoftwareMixer::set_listener(const float pos[3])
{
    for (int i = 0; i < 3; i++)
        listener_[i] = pos[i];
}

float SoftwareMixer::distance_gain(int bus, const float pos[3],
                                   float radius) const
{
    if (bus < 0 || bus >= kBuses)
        return 0.0f;
    const float min_d = min_distance_[bus];
    const float max_d = max_distance_[bus];
    const float dx = pos[0] - listener_[0];
    const float dy = pos[1] - listener_[1];
    const float dz = pos[2] - listener_[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    float d = sqrtf(d2);
    if (radius > 0.0f)
        d -= radius;
    if (d < min_d)
        d = min_d;
    if (d > max_d)
        d = max_d;
    if (max_d <= min_d || d <= 0.0f)
        return 1.0f;
    const float gain = (max_d - d) / (max_d - min_d) * min_d / d;
    return gain > 1.0f ? 1.0f : gain;
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
        float gain = 1.0f;
        if (!(v.flags & kSfx2D))
            gain = distance_gain(v.bus, v.pos, v.radius);
        gain *= volume_gain(v.volume);
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
    return bus >= 0 && bus < kBuses ? kVoicesPerBus : 0;
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
        v.volume = voice_in.volume;
        v.flags = voice_in.flags;
        v.radius = voice_in.radius;
        for (int i = 0; i < 3; i++)
            v.pos[i] = voice_in.pos[i];
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
    v->volume = voice_in.volume;
    v->flags = voice_in.flags;
    v->radius = voice_in.radius;
    for (int i = 0; i < 3; i++)
        v->pos[i] = voice_in.pos[i];
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
