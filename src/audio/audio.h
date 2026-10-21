// Audio core: DirectSound8-era event/voice model of the engine (notes/14).
//
// The exe posts 0x3c-byte SFX events (Game__postEvent 0x4eb5c0) into a static
// 64-slot pool (0x7a5b98, 0x5c bytes per slot, FUN_00568660). A per-frame
// service (FUN_005690a0) sorts the slots by attenuation and starts/updates/
// stops back-end voices; the low-level voice calls (FUN_00546a90/b60/b00/ab0)
// are abstracted as AudioBackend, implemented by the software mixer and by the
// DirectSound8 back end.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <map>

namespace audio {

constexpr int kSampleRate = 22050;   // primary buffer: PCM 22050 Hz
constexpr int kChannels = 2;         // stereo
constexpr int kBitsPerSample = 16;
constexpr int kVoices = 64;          // static pool, 64 x 0x5c
constexpr int kVoicesPerBus = 8;     // voice handle low 16 bits
constexpr int kBuses = 128;          // voice handle high 16 bits
constexpr float kCullDistance2 = 62500.0f;  // 250 m squared
constexpr float kMinDistance2 = 16.0f;      // radius cull floor
constexpr float kVolumeThreshold = 0.01f;

// java.util.resource.SfxRef flags.
constexpr uint32_t kSfxLoop = 0x00000001u;
constexpr uint32_t kSfx2D = 0x00000004u;        // added when pos == null
constexpr uint32_t kSfxNoAutoStop = 0x80000000u;

// Reset marker written into a freed slot's attenuation (FUN_005690a0/0x569020).
constexpr uint32_t kFreeAttenuationBits = 0x501502f9u;

inline float free_attenuation()
{
    float f;
    memcpy(&f, &kFreeAttenuationBits, sizeof f);
    return f;
}

// Event record at 0x6024c8 as built by Game__postEvent 0x4eb5c0.
struct SfxEvent {
    uint32_t flags;      // +0x00
    float volume;        // +0x04 (final: asset volume * caller volume)
    uint32_t reserved;   // +0x08 always 0
    float pitch;         // +0x0c
    uint32_t pad0;       // +0x10 unused
    float pos[3];        // +0x14
    float vec1[3];       // +0x20
    float vec2[3];       // +0x2c
    float radius;        // +0x38
};
static_assert(sizeof(SfxEvent) == 0x3c, "SfxEvent is the 0x3c event record");
static_assert(offsetof(SfxEvent, flags) == 0x00, "event flags");
static_assert(offsetof(SfxEvent, volume) == 0x04, "event volume");
static_assert(offsetof(SfxEvent, reserved) == 0x08, "event +0x08");
static_assert(offsetof(SfxEvent, pitch) == 0x0c, "event pitch");
static_assert(offsetof(SfxEvent, pos) == 0x14, "event pos");
static_assert(offsetof(SfxEvent, vec1) == 0x20, "event vec1");
static_assert(offsetof(SfxEvent, vec2) == 0x2c, "event vec2");
static_assert(offsetof(SfxEvent, radius) == 0x38, "event radius");

// Voice pool slot (0x7a5b98 + 0x5c*i). The gaps are real: the event is copied
// field by field (FUN_005689f0), skipping event+0x10 and slot+0x34/44/54.
struct Voice {
    int32_t state;      // +0x00 0 free, 1 new, 2 active, 3 loop update
    int32_t bus;        // +0x04 sound bank id (asset+0x60)
    int32_t instance;   // +0x08
    int32_t owner;      // +0x0c
    int32_t handle;     // +0x10 back-end voice handle, -1 when none
    float attenuation;  // +0x14 distance^2 / volume^2, 0 for 2D
    uint32_t flags;     // +0x18 event flags
    float volume;       // +0x1c
    int32_t reserved;   // +0x20 event+0x08
    float pitch;        // +0x24
    float pos[3];       // +0x28
    int32_t pad0;       // +0x34
    float vec1[3];      // +0x38
    int32_t pad1;       // +0x44
    float vec2[3];      // +0x48
    int32_t pad2;       // +0x54
    float radius;       // +0x58
};
static_assert(sizeof(Voice) == 0x5c, "Voice is the 0x5c pool slot");
static_assert(offsetof(Voice, state) == 0x00, "slot state");
static_assert(offsetof(Voice, bus) == 0x04, "slot bus");
static_assert(offsetof(Voice, instance) == 0x08, "slot instance");
static_assert(offsetof(Voice, owner) == 0x0c, "slot owner");
static_assert(offsetof(Voice, handle) == 0x10, "slot handle");
static_assert(offsetof(Voice, attenuation) == 0x14, "slot attenuation");
static_assert(offsetof(Voice, flags) == 0x18, "slot flags");
static_assert(offsetof(Voice, volume) == 0x1c, "slot volume");
static_assert(offsetof(Voice, pitch) == 0x24, "slot pitch");
static_assert(offsetof(Voice, pos) == 0x28, "slot pos");
static_assert(offsetof(Voice, vec1) == 0x38, "slot vec1");
static_assert(offsetof(Voice, vec2) == 0x48, "slot vec2");
static_assert(offsetof(Voice, radius) == 0x58, "slot radius");

// 65-entry hundredths-of-dB curve at 0x5e0648, indexed by (volume * 64).
extern const int32_t kVolumeDb[65];

// DirectSound SetVolume value for a linear volume: 0 when below 1/64, the
// curve value otherwise, 0 dB for >= 1.0 (FUN_005476e0/0x547010).
int volume_db(float volume);
// Linear gain corresponding to volume_db().
float volume_gain(float volume);

// Range test of FUN_00568940: cull beyond 250 m; radius>0 moves the reference
// point off the sphere and floors the squared distance at 16.
bool in_range(const float pos[3], float radius, const float listener[3]);
// FUN_00568660's 3D gate. Returns false when the event is culled; otherwise
// writes distance^2 / volume^2 (no floor when radius <= 0).
bool attenuation_of(const float pos[3], float radius, float volume,
                    const float listener[3], float *out);

// Low-level voice back end: FUN_00546a90 (play), 0x546b60 (update),
// 0x546ab0 (playing), 0x546b00 (stop), 0x546c00 (available voices),
// 0x546a70 (per-bus voice limit), 0x546b80 (bus available).
class AudioBackend {
  public:
    virtual ~AudioBackend() = default;
    // FUN_00546c00 returns at least 4.
    virtual int available_voices() const { return 4; }
    virtual int bus_limit(int) const { return kVoicesPerBus; }
    virtual bool bus_available(int bus) const { return bus >= 0 && bus < kBuses; }
    virtual int play(int bus, const Voice &voice) = 0;
    virtual void update(int handle, const Voice &voice) = 0;
    virtual bool playing(int handle) = 0;
    virtual void stop(int handle) = 0;
};

// Static event pool + per-frame service.
class VoicePool {
  public:
    Voice slots[kVoices];
    float listener[3] = {0.0f, 0.0f, 0.0f};

    VoicePool();

    // FUN_00568660. instance == 0 allocates; otherwise updates (instance, owner).
    // Returns 0, or -1 when culled/dropped and the pool cannot take the event.
    int post(int32_t bus, int32_t instance, int32_t owner, const SfxEvent &event);
    // FUN_00569020: stop the voice matching (instance, owner).
    int stop(int32_t instance, int32_t owner, AudioBackend &backend);
    // FUN_005690a0: sort by attenuation, reserve up to available_voices, then
    // start/update/stop voices. Loop voices (flag bit 0) must be re-posted
    // every frame; the service stops them otherwise.
    void service(AudioBackend &backend);
    void free_slot(int index);

  private:
    static void copy_event(Voice &slot, const SfxEvent &event);
};

// Channel globals DAT_005dada0 (effects), DAT_005dada4 (music), DAT_005dada8
// (engine), all 1.0 in the shipped image.
struct ChannelVolumes {
    float effects = 1.0f;
    float music = 1.0f;
    float engine = 1.0f;
    float &operator[](int channel);
    float operator[](int channel) const;
};

// Sound assets as resolved by Resource__getTyped: bus at asset+0x60 and base
// volume at asset+0x6c, keyed here by SfxRef resource id.
struct AudioAssets {
    struct Asset {
        int32_t bus;
        float volume;
    };
    std::map<int32_t, Asset> assets;

    void set(int32_t resource, int32_t bus, float volume = 1.0f);
    const Asset *get(int32_t resource) const;
};

// Game__postEvent 0x4eb5c0: drop culled/quiet events, build the event record,
// post to the pool. Returns the instance id on success (the exe ignores the
// pool result) or -1.
int post_event(VoicePool &pool, const AudioAssets &assets, int32_t resource,
               int32_t owner, float pitch, float volume, uint32_t flags,
               const float *pos, int32_t instance, float radius,
               const float *vec1, const float *vec2);

}  // namespace audio
