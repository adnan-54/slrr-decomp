// Music state machine: six static sets and their stream commands (notes/14).
// Set 0x568f40/0x568a50/0x568a90/0x568c70/0x568ca0/0x568cd0/0x568ed0,
// current-set pointer 0x5fe3b8, stream layer 0x5470a0/0x547010/0x5473b0.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

namespace audio {

// One of the six sets at 0x5fe3d0/0x5fe418/0x5fe434/0x5fe3ec/0x602478/0x602494.
// set+0x10 points at consecutive NUL-terminated names: start_set walks it with
// strlen(name) + 1 per track. All fields are zero until the game fills them.
struct MusicSet {
    std::string custom_track;  // +0x00 playTrack(String) copy
    float volume = 0.0f;       // +0x04
    int32_t track = 0;         // +0x08
    int32_t count = 0;         // +0x0c
    std::string strings;       // +0x10 packed NUL-terminated names
    int32_t mode = 0;          // +0x14 0 loops; 5 = direct path
    int32_t prev_mode = 0;     // +0x18 saved by playTrack(String)

    // Packs the names the way start_set consumes them.
    void set_tracks(const std::vector<std::string> &tracks);
    std::string track_path(int index) const;
};

// Stream layer behind 0x5470a0 (open), 0x547010 (volume) and 0x5473b0 (stop).
// kind 1 = .wav direct, 2 = .mp3 direct, 3 = DirectShow graph. The exe ignores
// open's result in start_set; keep the same contract.
class MusicOutput {
  public:
    virtual ~MusicOutput() = default;
    // context is 0x5470a0's opaque third argument: null when the set mode is
    // 0, otherwise the set pointer (playTrack(String) always passes the set).
    virtual bool open(int kind, const std::string &path, bool loop,
                      const MusicSet *context) = 0;
    virtual void set_volume(int kind, float volume) = 0;
    virtual void stop(int kind) = 0;
};

struct MusicSystem {
    static constexpr int kSetCount = 6;

    MusicSet sets[kSetCount];

    void set_output(MusicOutput *output) { output_ = output; }
    // DAT_0079a0be (audio initialized) and DAT_0079a0bd (DirectShow usable).
    void set_initialized(bool initialized) { initialized_ = initialized; }
    bool initialized() const { return initialized_; }
    void set_directshow_available(bool available) { directshow_ = available; }
    bool directshow_available() const { return directshow_; }
    // GetTickCount source for random(); falls back to an incrementing counter.
    using TickFn = uint32_t (*)();
    void set_tick_source(TickFn tick) { tick_ = tick; }
    // DAT_005dada4; setVolume(channel 1) stores it and calls set_volume().
    void set_channel_volume(float volume) { channel_volume_ = volume; }
    float channel_volume() const { return channel_volume_; }

    MusicSet *current();
    const MusicSet *current() const;
    int current_index() const { return current_; }

    // Native Sound.changeMusicSet 0x4e48f0: invalid ids select null.
    void change_set(int id);
    // 0x568f40 on the current set. Returns the track, or -1 when the set has
    // no strings/count or its volume is below 0.01.
    int start();
    // 0x569010/0x5473b0: stop the stream and return the track.
    int stop();
    // 0x568a50; clamps count < index to count (start_set then wraps to 0).
    int play_track(int index);
    // 0x568a90: mode 5 with prev_mode = old mode, loop off, no start_set.
    void play_track(const std::string &path);
    int next();    // 0x568c70
    int prev();    // 0x568ca0
    int random();  // 0x568cd0
    // 0x4e4b40: accepted modes are 0..5; returns the resulting mode.
    int set_mode(int mode);
    int mode() const;
    int track() const;
    int count() const;
    // 0x568ed0: returns the previous volume.
    float set_volume(float volume);
    // 0x5470a0's global stream kind: 0 none, 1 .wav, 2 .mp3, 3 DirectShow.
    int stream_kind() const { return stream_kind_; }

  private:
    int start_set(MusicSet &set);
    void stop_stream();
    void apply_volume(float volume);
    void open_stream(const std::string &path, bool loop,
                     const MusicSet *context);

    MusicOutput *output_ = nullptr;
    bool initialized_ = false;  // BSS global, set by audio init
    bool directshow_ = false;
    float channel_volume_ = 1.0f;
    TickFn tick_ = nullptr;
    uint32_t fallback_tick_ = 0;
    int current_ = -1;
    int stream_kind_ = 0;
};

}  // namespace audio
