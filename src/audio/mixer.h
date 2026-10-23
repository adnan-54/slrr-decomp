// Software mixing back end: 22050 Hz/16-bit/stereo, 128 buses x 8 voices.
// Voice gain is the exe's no-3D-hardware path of FUN_005476e0: distance gain
// from the per-bus min/max distances times the event volume, quantized by the
// hundredths-dB table (notes/14). The DirectSound8 back end reuses its sample
// store and bus distances.
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
    // Per-bus min/max distance (sound resource "mindist"/"maxdist", defaults
    // 5/1000, FUN_00525d90); min is floored at 0.001 (FUN_00546860).
    void set_bus_distances(int bus, float min_distance, float max_distance);
    float bus_min_distance(int bus) const;
    float bus_max_distance(int bus) const;
    // Sound__createBus 0x546860: first bus without a sample; -1 when none is free, -2 when
    // the sample is empty (DSBuffer__createFromWav/0x5461e0 failure). frames of interleaved
    // stereo at rate. instances -> voice limit min(instances, 8); min floored at 0.001.
    int create_bus(const int16_t *pcm, size_t frames, int rate, int instances, bool is3d,
                   float min_distance, float max_distance);
    // Sound__releaseBus 0x546b40/0x5474f0: stop the bus voices and drop the sample.
    void release_bus(int bus);
    bool bus_3d(int bus) const;
    void set_listener(const float pos[3]);
    const float *listener() const { return listener_; }
    // Software distance gain of a 3D voice at pos/radius for its bus
    // (FUN_005476e0 with DAT_0079a0c0 == 0).
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
        float gain = 0.0f;  // linear, set at play/update like FUN_005476e0
        uint32_t flags = 0;
    };

    static int handle_of(int bus, int slot) { return (bus << 16) | slot; }
    static int bus_of(int handle) { return (handle >> 16) & 0xffff; }
    static int slot_of(int handle) { return handle & 0xffff; }
    MixVoice *voice(int handle);
    float voice_gain(int bus, const Voice &voice) const;

    MixSample samples_[kBuses];
    float min_distance_[kBuses];
    float max_distance_[kBuses];
    int voice_limit_[kBuses];
    bool bus_3d_[kBuses];
    MixVoice voices_[kBuses * kVoicesPerBus];
    float listener_[3] = {0.0f, 0.0f, 0.0f};
    int available_ = 4;
};

}  // namespace audio
