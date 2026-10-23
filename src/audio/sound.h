// Sound resources (res_sound, record type 6): .rsd text and WAV data (notes/14 §2.6).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "audio/audio.h"
#include "audio/mixer.h"

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

// Process-wide SFX state. Exe: static pool 0x7a5b98 (SfxPool__init 0x568b60 from the CRT
// static initialiser 0x569420), bus tables 0x79a180.. (SoftwareMixer) and per-resource
// Resource_Sound_Data (RI+0xb0). Keys are the caller's resource node pointers.
class SfxService {
  public:
    VoicePool pool;
    AudioAssets assets;
    SoftwareMixer mixer;
    // shortcut: always the software mixer; a Windows host swaps in Ds8Backend later.
    AudioBackend *backend = &mixer;

    bool loaded(const void *res) const;   // RI+0x94 bit0
    // Resource_Sound__vf10 0x525cf0 (sync path). params == nullptr: no sourcefile;
    // file == nullptr or empty: File__readAll failed. Marks res loaded in every case.
    void load(const void *res, const SoundParams *params,
              const std::vector<uint8_t> *file);
    // Resource_Sound__vf7 0x526070: release the bus, forget the asset, not loaded.
    void unload(const void *res);
    // AudioAssets key of res (0 when never loaded; the key space starts at 1).
    int32_t key(const void *res) const;

  private:
    struct Entry {
        int32_t key = 0;
    };
    std::map<const void *, Entry> entries_;   // present = loaded
    int32_t next_key_ = 1;   // INVENTED: AudioAssets is keyed by int32
};

SfxService &sfx_service();   // function-local static (exe: static storage)

}  // namespace audio
