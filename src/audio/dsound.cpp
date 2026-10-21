#include "audio/dsound.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>

#include <math.h>
#include <string.h>

#include <vector>

namespace audio {

struct Ds8Backend::Impl {
    struct Entry {
        IDirectSoundBuffer *buffer = nullptr;
        IDirectSound3DBuffer *buffer3d = nullptr;
        bool used = false;
    };
    IDirectSound8 *device = nullptr;
    IDirectSoundBuffer *primary = nullptr;
    IDirectSound3DListener *listener = nullptr;
    std::vector<Entry> entries;
};

namespace {

LONG ds_volume(float volume, float gain)
{
    int db = volume_db(volume);
    if (db <= -10000 || gain <= 0.0f)
        return DSBVOLUME_MIN;
    db += (int)lrintf(2000.0f * log10f(gain));
    if (db < DSBVOLUME_MIN)
        db = DSBVOLUME_MIN;
    if (db > DSBVOLUME_MAX)
        db = DSBVOLUME_MAX;
    return (LONG)db;
}

void apply_voice(SoftwareMixer &mixer, int bus, IDirectSoundBuffer *buffer,
                 IDirectSound3DBuffer *buffer3d, const Voice &voice)
{
    float gain = 1.0f;
    DWORD mode = DS3DMODE_DISABLE;
    if (!(voice.flags & kSfx2D)) {
        gain = mixer.distance_gain(bus, voice.pos, voice.radius);
        mode = DS3DMODE_NORMAL;
        buffer3d->SetPosition(voice.pos[0], voice.pos[1], voice.pos[2],
                              DS3D_IMMEDIATE);
        buffer3d->SetMinDistance(mixer.bus_min_distance(bus), DS3D_IMMEDIATE);
        buffer3d->SetMaxDistance(mixer.bus_max_distance(bus), DS3D_IMMEDIATE);
    }
    buffer3d->SetMode(mode, DS3D_IMMEDIATE);
    buffer->SetVolume(ds_volume(voice.volume, gain));
    const MixSample *sample = mixer.sample(bus);
    DWORD rate = (DWORD)(sample ? sample->rate : kSampleRate);
    DWORD freq = (DWORD)lrintf((float)rate * voice.pitch);
    if (freq < DSBFREQUENCY_MIN)
        freq = DSBFREQUENCY_MIN;
    if (freq > DSBFREQUENCY_MAX)
        freq = DSBFREQUENCY_MAX;
    buffer->SetFrequency(freq);
}

}  // namespace

Ds8Backend::Ds8Backend() : impl_(new Impl)
{
    impl_->entries.resize(kVoices);
}

Ds8Backend::~Ds8Backend()
{
    shutdown();
    delete impl_;
}

bool Ds8Backend::init(void *hwnd)
{
    shutdown();
    if (FAILED(DirectSoundCreate8(nullptr, &impl_->device, nullptr)) ||
        !impl_->device)
        return false;
    if (FAILED(impl_->device->SetCooperativeLevel((HWND)hwnd,
                                                  DSSCL_PRIORITY))) {
        shutdown();
        return false;
    }

    DSBUFFERDESC primary_desc = {};
    primary_desc.dwSize = sizeof primary_desc;
    primary_desc.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D;
    if (FAILED(impl_->device->CreateSoundBuffer(&primary_desc,
                                                &impl_->primary, nullptr)) ||
        !impl_->primary) {
        shutdown();
        return false;
    }
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kChannels;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = kBitsPerSample;
    format.nBlockAlign = (WORD)(kChannels * kBitsPerSample / 8);
    format.nAvgBytesPerSec = kSampleRate * format.nBlockAlign;
    if (FAILED(impl_->primary->SetFormat(&format))) {
        shutdown();
        return false;
    }
    if (FAILED(impl_->primary->QueryInterface(
            IID_IDirectSound3DListener, (void **)&impl_->listener)) ||
        !impl_->listener) {
        shutdown();
        return false;
    }
    // Distance gain is computed by SoftwareMixer; the 3D listener only pans.
    impl_->listener->SetRolloffFactor(0.0f, DS3D_IMMEDIATE);
    impl_->listener->SetPosition(0.0f, 0.0f, 0.0f, DS3D_IMMEDIATE);
    use_software_ = false;
    return true;
}

void Ds8Backend::shutdown()
{
    for (size_t i = 0; i < impl_->entries.size(); i++) {
        Impl::Entry &entry = impl_->entries[i];
        if (entry.buffer) {
            entry.buffer->Stop();
            entry.buffer->Release();
        }
        if (entry.buffer3d)
            entry.buffer3d->Release();
        entry = Impl::Entry();
    }
    if (impl_->listener) {
        impl_->listener->Release();
        impl_->listener = nullptr;
    }
    if (impl_->primary) {
        impl_->primary->Release();
        impl_->primary = nullptr;
    }
    if (impl_->device) {
        impl_->device->Release();
        impl_->device = nullptr;
    }
    use_software_ = true;
}

void Ds8Backend::set_listener(const float pos[3])
{
    mixer_.set_listener(pos);
    if (impl_->listener)
        impl_->listener->SetPosition(pos[0], pos[1], pos[2], DS3D_IMMEDIATE);
}

int Ds8Backend::available_voices() const
{
    return use_software_ ? mixer_.available_voices() : 4;
}

int Ds8Backend::bus_limit(int bus) const
{
    return mixer_.bus_limit(bus);
}

bool Ds8Backend::bus_available(int bus) const
{
    return mixer_.sample(bus) != nullptr;
}

int Ds8Backend::play(int bus, const Voice &voice)
{
    if (use_software_)
        return mixer_.play(bus, voice);
    const MixSample *sample = mixer_.sample(bus);
    if (!sample)
        return -1;
    int index = -1;
    for (size_t i = 0; i < impl_->entries.size(); i++) {
        if (!impl_->entries[i].used) {
            index = (int)i;
            break;
        }
    }
    if (index < 0)
        return -1;
    Impl::Entry &entry = impl_->entries[index];

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kChannels;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = kBitsPerSample;
    format.nBlockAlign = (WORD)(kChannels * kBitsPerSample / 8);
    format.nAvgBytesPerSec = kSampleRate * format.nBlockAlign;

    const DWORD bytes = (DWORD)(sample->pcm.size() * sizeof(int16_t));
    DSBUFFERDESC desc = {};
    desc.dwSize = sizeof desc;
    desc.dwFlags = DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME |
                   DSBCAPS_CTRLFREQUENCY | DSBCAPS_GETCURRENTPOSITION2;
    desc.dwBufferBytes = bytes;
    desc.lpwfxFormat = &format;
    if (FAILED(impl_->device->CreateSoundBuffer(&desc, &entry.buffer,
                                                nullptr)) ||
        !entry.buffer)
        return -1;

    void *data1 = nullptr;
    void *data2 = nullptr;
    DWORD size1 = 0;
    DWORD size2 = 0;
    if (FAILED(entry.buffer->Lock(0, bytes, &data1, &size1, &data2, &size2,
                                  0))) {
        entry.buffer->Release();
        entry.buffer = nullptr;
        return -1;
    }
    const char *source = (const char *)sample->pcm.data();
    memcpy(data1, source, size1);
    if (data2 && size2)
        memcpy(data2, source + size1, size2);
    entry.buffer->Unlock(data1, size1, data2, size2);

    if (FAILED(entry.buffer->QueryInterface(IID_IDirectSound3DBuffer,
                                            (void **)&entry.buffer3d)) ||
        !entry.buffer3d) {
        entry.buffer->Release();
        entry.buffer = nullptr;
        return -1;
    }
    apply_voice(mixer_, bus, entry.buffer, entry.buffer3d, voice);
    const DWORD flags = (voice.flags & kSfxLoop) ? DSBPLAY_LOOPING : 0;
    if (FAILED(entry.buffer->Play(0, 0, flags))) {
        entry.buffer3d->Release();
        entry.buffer->Release();
        entry = Impl::Entry();
        return -1;
    }
    entry.used = true;
    return index;
}

void Ds8Backend::update(int handle, const Voice &voice)
{
    if (use_software_) {
        mixer_.update(handle, voice);
        return;
    }
    if (handle < 0 || (size_t)handle >= impl_->entries.size())
        return;
    Impl::Entry &entry = impl_->entries[handle];
    if (!entry.used)
        return;
    apply_voice(mixer_, voice.bus, entry.buffer, entry.buffer3d, voice);
}

bool Ds8Backend::playing(int handle)
{
    if (use_software_)
        return mixer_.playing(handle);
    if (handle < 0 || (size_t)handle >= impl_->entries.size())
        return false;
    Impl::Entry &entry = impl_->entries[handle];
    if (!entry.used)
        return false;
    DWORD status = 0;
    if (FAILED(entry.buffer->GetStatus(&status)))
        return false;
    return (status & DSBSTATUS_PLAYING) != 0;
}

void Ds8Backend::stop(int handle)
{
    if (use_software_) {
        mixer_.stop(handle);
        return;
    }
    if (handle < 0 || (size_t)handle >= impl_->entries.size())
        return;
    Impl::Entry &entry = impl_->entries[handle];
    if (!entry.used)
        return;
    entry.buffer->Stop();
    entry.buffer3d->Release();
    entry.buffer->Release();
    entry = Impl::Entry();
}

}  // namespace audio
#endif
