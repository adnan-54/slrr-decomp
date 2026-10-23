// Sound resources (res_sound, record type 6): .rsd text and WAV data (notes/14 §2.6).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace audio {

// Resource_Sound_Data__parse 0x525d90 fields; defaults are CompatibleSound__ctor 0x525b00.
struct SoundParams {
    std::string sourcefile;     // "%*s %s"
    float volume = 1.0f;        // CompatibleSound +0x6c
    float mindist = 5.0f;       // +0x70
    float maxdist = 1000.0f;    // +0x74
    int32_t instances = 4;      // +0x64
    int32_t flags = 0;          // +0x68 (bit0 set = 2D bus)
};

// 0x525d90 key loop. False when no sourcefile was read (exe returns 0.0).
bool parse_sound_params(const char *data, size_t size, SoundParams *out);

// Wav__parse 0x5462a0 result.
struct WavView {
    uint16_t format_tag = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *pcm = nullptr;
    uint32_t bytes = 0;
};

// 0: ok, -1: not RIFF/WAVE, -2: first chunk is not "fmt ".
// INVENTED: -3 when a chunk header or the data range leaves the buffer (exe reads on).
int parse_wav(const uint8_t *data, size_t size, WavView *out);

// INVENTED representation of the DirectSound buffer for SoftwareMixer::set_sample:
// interleaved stereo int16 at the file rate. PCM tag 1 only; 8-bit is unsigned
// ((x - 128) << 8), 16-bit as is; mono is duplicated to both channels. Other formats
// (one shipped 32-bit file) return false, like a failed CreateSoundBuffer (-2 bus).
bool decode_pcm(const WavView &w, std::vector<int16_t> *stereo);

}  // namespace audio
