// Software mixing back end: 22050 Hz/16-bit/stereo, 128 buses x 8 voices,
// volume quantized by the exe's hundredths-dB table and distance-attenuated
// with the per-bus min/max distances (notes/14). The DirectSound8 back end
// reuses its sample store and distance/gain helpers.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "audio/audio.h"

namespace audio {

struct MixSample {
    std::vector<int16_t> pcm;  // interleaved stereo
    int rate = kSampleRate;    // source frames per second
    int frames() const { return (int)(pcm.size() / 2); }
};

class SoftwareMixer : public AudioBackend {
  public:
    SoftwareMixer();

    // Bus sample store: an SFX resource decoded to interleaved stereo.
    void set_sample(int bus, const int16_t *pcm, size_t frames,
                    int rate = kSampleRate);
    void clear_sample(int bus);
    const MixSample *sample(int bus) const;
    // Per-bus rolloff sphere, defaults 1..100 m.
    void set_bus_distances(int bus, float min_distance, float max_distance);
    float bus_min_distance(int bus) const;
    float bus_max_distance(int bus) const;
    void set_listener(const float pos[3]);
    // Gain of a 3D voice at pos/radius for its bus (FUN_00547... rolloff).
    float distance_gain(int bus, const float pos[3], float radius) const;

    // Mixes all active voices into frames of interleaved stereo, clipping to
    // int16. Loop voices wrap; others end. Returns voices that produced audio.
    int mix(int16_t *out, int frames);

    // AudioBackend. Handles are (bus << 16) | slot.
    int available_voices() const override { return available_; }
    void set_available_voices(int count) { available_ = count; }
    int bus_limit(int bus) const override;
    bool bus_available(int bus) const override;
    int play(int bus, const Voice &voice) override;
    void update(int handle, const Voice &voice) override;
    bool playing(int handle) override;
    void stop(int handle) override;

  private:
    struct MixVoice {
        bool active = false;
        int bus = 0;
        int slot = 0;
        double position = 0.0;  // source frames
        float pitch = 1.0f;
        float volume = 0.0f;
        uint32_t flags = 0;
        float pos[3] = {0.0f, 0.0f, 0.0f};
        float radius = 0.0f;
    };

    static int handle_of(int bus, int slot) { return (bus << 16) | slot; }
    static int bus_of(int handle) { return (handle >> 16) & 0xffff; }
    static int slot_of(int handle) { return handle & 0xffff; }
    MixVoice *voice(int handle);

    MixSample samples_[kBuses];
    float min_distance_[kBuses];
    float max_distance_[kBuses];
    MixVoice voices_[kBuses * kVoicesPerBus];
    float listener_[3] = {0.0f, 0.0f, 0.0f};
    int available_ = 4;
};

}  // namespace audio
