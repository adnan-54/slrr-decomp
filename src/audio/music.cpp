#include "audio/music.h"

#include <string.h>

namespace audio {

void MusicSet::set_tracks(const std::vector<std::string> &tracks)
{
    strings.clear();
    for (size_t i = 0; i < tracks.size(); i++) {
        strings += tracks[i];
        strings += '\0';
    }
    count = (int32_t)tracks.size();
}

std::string MusicSet::track_path(int index) const
{
    size_t pos = 0;
    for (int i = 0; i < index; i++) {
        if (pos >= strings.size())
            return std::string();
        pos += strlen(strings.c_str() + pos) + 1;
    }
    if (pos >= strings.size())
        return std::string();
    return strings.c_str() + pos;
}

MusicSet *MusicSystem::current()
{
    return current_ < 0 ? nullptr : &sets[current_];
}

const MusicSet *MusicSystem::current() const
{
    return current_ < 0 ? nullptr : &sets[current_];
}

void MusicSystem::change_set(int id)
{
    MusicSet *target =
        id >= 0 && id < kSetCount ? &sets[id] : nullptr;
    if (target == current())
        return;
    if (current())
        stop_stream();
    current_ = target ? id : -1;
    if (target) {
        start_set(*target);
        set_volume(channel_volume_);
    }
}

int MusicSystem::start()
{
    MusicSet *set = current();
    return set ? start_set(*set) : -1;
}

int MusicSystem::stop()
{
    MusicSet *set = current();
    if (!set)
        return -1;
    stop_stream();
    return set->track;
}

int MusicSystem::start_set(MusicSet &set)
{
    if (set.strings.empty() || set.count < 1 ||
        (set.volume <= 0.01f && set.volume != 0.01f))
        return -1;
    if (set.track < 0)
        set.track = set.count - 1;
    if (set.count <= set.track)
        set.track = 0;
    const bool loop = set.mode == 0;
    const MusicSet *context = loop ? nullptr : &set;
    open_stream(set.track_path(set.track), loop, context);
    if (set.volume < 0.01f) {
        set.volume = 0.0f;
        stop_stream();
        return set.track;
    }
    apply_volume(set.volume);
    return set.track;
}

int MusicSystem::play_track(int index)
{
    MusicSet *set = current();
    if (!set)
        return -1;
    stop_stream();
    if (set->mode == 5)
        set->mode = set->prev_mode;
    if (set->count < index)
        index = set->count;
    set->track = index;
    start_set(*set);
    return set->track;
}

void MusicSystem::play_track(const std::string &path)
{
    MusicSet *set = current();
    if (!set)
        return;
    stop_stream();
    set->prev_mode = set->mode;
    set->custom_track = path;
    set->mode = 5;
    open_stream(path, false, set);
    if (set->volume < 0.01f) {
        set->volume = 0.0f;
        stop_stream();
        return;
    }
    apply_volume(set->volume);
}

int MusicSystem::next()
{
    MusicSet *set = current();
    if (!set)
        return -1;
    stop_stream();
    if (set->mode == 5)
        set->mode = set->prev_mode;
    set->track++;
    start_set(*set);
    return set->track;
}

int MusicSystem::prev()
{
    MusicSet *set = current();
    if (!set)
        return -1;
    stop_stream();
    if (set->mode == 5)
        set->mode = set->prev_mode;
    set->track--;
    start_set(*set);
    return set->track;
}

int MusicSystem::random()
{
    MusicSet *set = current();
    if (!set)
        return -1;
    const int old = set->track;
    stop_stream();
    if (set->mode == 5)
        set->mode = set->prev_mode;
    if (set->count > 1) {
        do {
            uint32_t now = tick_ ? tick_() : ++fallback_tick_;
            int track = (int)(now % (uint32_t)set->count);
            if (track < 0)
                track = set->count - 1;
            if (set->count <= track)
                track = 0;
            set->track = track;
        } while (set->track == old);
    }
    start_set(*set);
    return set->track;
}

int MusicSystem::set_mode(int mode)
{
    MusicSet *set = current();
    if (!set)
        return mode;
    if ((uint32_t)mode < 6u)
        set->mode = mode;
    return set->mode;
}

int MusicSystem::mode() const
{
    const MusicSet *set = current();
    return set ? set->mode : -1;
}

int MusicSystem::track() const
{
    const MusicSet *set = current();
    return set ? set->track : -1;
}

int MusicSystem::count() const
{
    const MusicSet *set = current();
    return set ? set->count : -1;
}

float MusicSystem::set_volume(float volume)
{
    MusicSet *set = current();
    if (!set)
        return 0.0f;
    const float old = set->volume;
    set->volume = volume;
    if (volume < 0.01f) {
        set->volume = 0.0f;
        stop_stream();
        return old;
    }
    if (old < 0.01f)
        start_set(*set);
    apply_volume(set->volume);
    return old;
}

void MusicSystem::open_stream(const std::string &path, bool loop,
                              const MusicSet *context)
{
    if (!initialized_)
        return;
    if (!directshow_) {
        const size_t dot = path.rfind('.');
        if (dot != std::string::npos && path.compare(dot, std::string::npos,
                                                     ".wav") == 0) {
            stream_kind_ = 1;
            if (output_)
                output_->open(1, path, loop, context);
        } else if (dot != std::string::npos &&
                   path.compare(dot, std::string::npos, ".mp3") == 0) {
            stream_kind_ = 2;
            if (output_)
                output_->open(2, path, loop, context);
        }
        return;
    }
    if (output_ && output_->open(3, path, loop, context))
        stream_kind_ = 3;
}

void MusicSystem::apply_volume(float volume)
{
    if (!initialized_)
        return;
    if (stream_kind_ >= 1 && stream_kind_ <= 3 && output_)
        output_->set_volume(stream_kind_, volume);
}

void MusicSystem::stop_stream()
{
    if (!initialized_)
        return;
    if (stream_kind_ >= 1 && stream_kind_ <= 3) {
        if (output_)
            output_->stop(stream_kind_);
        stream_kind_ = 0;
    } else {
        stream_kind_ = 0;
    }
}

}  // namespace audio
