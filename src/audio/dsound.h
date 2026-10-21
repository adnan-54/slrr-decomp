// DirectSound8 back end (Windows only): primary 22050 Hz/16-bit/stereo buffer,
// 3D listener and one secondary 3D buffer per voice. Sample data, bus distance
// spheres and every fallback path come from SoftwareMixer.
#pragma once
#include <stdint.h>

#include "audio/audio.h"
#include "audio/mixer.h"

#ifdef _WIN32
namespace audio {

class Ds8Backend : public AudioBackend {
  public:
    Ds8Backend();
    ~Ds8Backend() override;

    // Creates the device/primary buffer/listener. On any failure releases
    // what was created and stays on the software fallback.
    bool init(void *hwnd);
    void shutdown();
    // False when init() failed or was never called.
    bool using_software() const { return use_software_; }

    SoftwareMixer &software() { return mixer_; }
    void set_listener(const float pos[3]);
    // Software fallback output; the caller pumps it (DirectSound runs itself).
    int mix(int16_t *out, int frames) { return mixer_.mix(out, frames); }

    int available_voices() const override;
    int bus_limit(int bus) const override;
    bool bus_available(int bus) const override;
    int play(int bus, const Voice &voice) override;
    void update(int handle, const Voice &voice) override;
    bool playing(int handle) override;
    void stop(int handle) override;

  private:
    struct Impl;
    Impl *impl_;
    SoftwareMixer mixer_;
    bool use_software_ = true;
};

}  // namespace audio
#endif
