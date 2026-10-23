// Sound resources: .rsd key/value text and RIFF/WAVE PCM (notes/14 §2.6).
#include "audio/sound.h"

#include <stdio.h>

namespace audio {

namespace {

char lower_ascii(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

// _stricmp, the comparison 0x525d90 uses for the keys.
bool ieq(const char *a, const char *b)
{
    while (*a && lower_ascii(*a) == lower_ascii(*b)) {
        a++;
        b++;
    }
    return *a == *b;
}

uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

}  // namespace

bool parse_sound_params(const char *data, size_t size, SoundParams *out)
{
    if (!data) return false;   // 0x525d90 returns 0.0

    float volume = 1.0f, mindist = 5.0f, maxdist = 1000.0f;
    int32_t instances = 4, flags = 0;
    char src[256] = "";
    char key[64] = "";   // 0x525d90 never resets it when "%s" fails

    size_t pos = 0;
    do {
        // Str__readLine 0x5281b0: keep the '\n', stop at it or at a NUL byte.
        std::string line;
        while (pos < size) {   // INVENTED: exe reads past the record end
            char c = data[pos++];
            if (c == '\0') break;
            line.push_back(c);
            if (c == '\n') break;
        }
        if (!line.empty()) {
            std::sscanf(line.c_str(), "%63s", key);   // INVENTED width
            if (ieq(key, "sourcefile"))
                std::sscanf(line.c_str(), "%*s %255s", src);
            else if (ieq(key, "volume"))
                std::sscanf(line.c_str(), "%*s %f", &volume);
            else if (ieq(key, "mindist"))
                std::sscanf(line.c_str(), "%*s %f", &mindist);
            else if (ieq(key, "maxdist"))
                std::sscanf(line.c_str(), "%*s %f", &maxdist);
            else if (ieq(key, "instances"))
                std::sscanf(line.c_str(), "%*s %d", &instances);
            else if (ieq(key, "flags"))
                std::sscanf(line.c_str(), "%*s %d", &flags);
        }
    } while (pos < size);

    if (src[0] == 0) return false;   // no CompatibleSound is created

    out->sourcefile = src;
    out->volume = volume;
    out->mindist = mindist;
    out->maxdist = maxdist;
    out->instances = instances;
    out->flags = flags;
    return true;
}

int parse_wav(const uint8_t *data, size_t size, WavView *out)
{
    if (!data) return -1;
    if (size < 20) return -3;   // INVENTED: 0x5462a0 has no bounds checks
    if (rd32(data) != 0x46464952 || rd32(data + 8) != 0x45564157) return -1;  // "RIFF" "WAVE"
    if (rd32(data + 12) != 0x20746d66) return -2;                             // "fmt "

    uint32_t fmt_size = rd32(data + 16);
    if ((size_t)fmt_size > size - 20) return -3;   // INVENTED
    if (size < 36) return -3;                      // INVENTED: WAVEFORMATEX fields

    const uint8_t *wfx = data + 20;
    const uint8_t *end = data + size;
    const uint8_t *q = wfx + fmt_size;
    for (;;) {   // "data": PAD/fact/LIST/smpl/DISP/JUNK seen before it
        if ((size_t)(end - q) < 8) return -3;   // INVENTED
        if (rd32(q) == 0x61746164) break;
        if ((size_t)rd32(q + 4) > (size_t)(end - q) - 8) return -3;   // INVENTED
        q += 8 + rd32(q + 4);
    }
    uint32_t bytes = rd32(q + 4);
    const uint8_t *pcm = q + 8;
    if ((size_t)(end - pcm) < bytes) return -3;   // INVENTED

    out->format_tag = rd16(wfx);
    out->channels = rd16(wfx + 2);
    out->rate = rd32(wfx + 4);
    out->bits = rd16(wfx + 14);
    out->pcm = pcm;
    out->bytes = bytes;
    return 0;
}

bool decode_pcm(const WavView &w, std::vector<int16_t> *stereo)
{
    if (w.format_tag != 1) return false;
    if (w.channels != 1 && w.channels != 2) return false;
    if (w.bits != 8 && w.bits != 16) return false;
    if (w.bytes == 0 || !w.pcm) return false;   // DSBuffer__fill 0x5461e0

    size_t sample_bytes = w.bits / 8;
    size_t frame_bytes = (size_t)w.channels * sample_bytes;
    size_t frames = w.bytes / frame_bytes;

    stereo->clear();
    stereo->reserve(frames * 2);
    for (size_t i = 0; i < frames; i++) {
        const uint8_t *f = w.pcm + i * frame_bytes;
        int16_t v[2];
        for (size_t c = 0; c < w.channels; c++) {
            const uint8_t *s = f + c * sample_bytes;
            if (w.bits == 8)
                v[c] = (int16_t)(((int)s[0] - 128) << 8);   // WAV 8-bit is unsigned
            else
                v[c] = (int16_t)(s[0] | (s[1] << 8));
        }
        stereo->push_back(v[0]);
        stereo->push_back(w.channels == 2 ? v[1] : v[0]);
    }
    return true;
}

bool SfxService::loaded(const void *res) const { return entries_.find(res) != entries_.end(); }

void SfxService::load(const void *res, const SoundParams *params,
                      const std::vector<uint8_t> *file)
{
    if (loaded(res)) return;   // 0x525cf0: only the first call reads
    Entry &e = entries_[res];
    e.key = next_key_++;
    if (!params || !file || file->empty()) return;   // 0x455e80: size < 1 -> no CompatibleSound

    WavView w;
    std::vector<int16_t> pcm;
    int bus = -2;
    if (parse_wav(file->data(), file->size(), &w) == 0 && decode_pcm(w, &pcm))
        bus = mixer.create_bus(pcm.data(), pcm.size() / 2, (int)w.rate, params->instances,
                               (~params->flags & 1) != 0, params->mindist, params->maxdist);
    assets.set(e.key, bus, params->volume);   // exists even with a negative bus
}

void SfxService::unload(const void *res)
{
    std::map<const void *, Entry>::iterator it = entries_.find(res);
    if (it == entries_.end()) return;
    const AudioAssets::Asset *as = assets.get(it->second.key);
    if (as && as->bus >= 0) mixer.release_bus(as->bus);   // 0x526070
    assets.assets.erase(it->second.key);
    entries_.erase(it);
}

int32_t SfxService::key(const void *res) const
{
    std::map<const void *, Entry>::const_iterator it = entries_.find(res);
    return it == entries_.end() ? 0 : it->second.key;
}

SfxService &sfx_service()
{
    static SfxService service;
    return service;
}

}  // namespace audio
