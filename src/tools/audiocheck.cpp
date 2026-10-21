// Audio subsystem checks: event/voice layouts and pool, postEvent, SfxTable
// evaluator, engine SFX formulas, music state machine, software mixer math.
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc
//        src/tools/audiocheck.cpp src/audio/*.cpp -o build/audiocheck
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "audio/audio.h"
#include "audio/mixer.h"
#include "audio/music.h"
#include "audio/sfx.h"

static int failures = 0;

static void check(const char *name, bool ok)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
    fflush(stdout);
    if (!ok)
        failures++;
}

static void checkf(const char *name, bool ok, const char *fmt = nullptr, ...)
{
    char detail[256] = "";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(detail, sizeof detail, fmt, ap);
        va_end(ap);
    }
    printf("%s  %-58s %s\n", ok ? "PASS" : "FAIL", name, detail);
    fflush(stdout);
    if (!ok)
        failures++;
}

static bool near(float a, float b, float eps = 1e-4f)
{
    return fabsf(a - b) <= eps * (1.0f + fabsf(b));
}

// ---------------------------------------------------------------- layouts

static void test_layouts()
{
    checkf("sizeof SfxEvent", sizeof(audio::SfxEvent) == 0x3c,
           "0x%zx", sizeof(audio::SfxEvent));
    checkf("sizeof Voice", sizeof(audio::Voice) == 0x5c, "0x%zx",
           sizeof(audio::Voice));
    checkf("sizeof SfxItem", sizeof(audio::SfxItem) == 0x24, "0x%zx",
           sizeof(audio::SfxItem));
    checkf("sizeof SfxTable", sizeof(audio::SfxTable) == 0x248, "0x%zx",
           sizeof(audio::SfxTable));
    check("SfxTable offsets",
          offsetof(audio::SfxTable, count) == 0x240 &&
              offsetof(audio::SfxTable, value_scale) == 0x244);
    check("Voice offsets",
          offsetof(audio::Voice, state) == 0x00 &&
              offsetof(audio::Voice, bus) == 0x04 &&
              offsetof(audio::Voice, instance) == 0x08 &&
              offsetof(audio::Voice, owner) == 0x0c &&
              offsetof(audio::Voice, handle) == 0x10 &&
              offsetof(audio::Voice, attenuation) == 0x14 &&
              offsetof(audio::Voice, flags) == 0x18 &&
              offsetof(audio::Voice, volume) == 0x1c &&
              offsetof(audio::Voice, pitch) == 0x24 &&
              offsetof(audio::Voice, pos) == 0x28 &&
              offsetof(audio::Voice, vec1) == 0x38 &&
              offsetof(audio::Voice, vec2) == 0x48 &&
              offsetof(audio::Voice, radius) == 0x58);
    check("SfxEvent offsets", offsetof(audio::SfxEvent, volume) == 0x04 &&
                                  offsetof(audio::SfxEvent, pitch) == 0x0c &&
                                  offsetof(audio::SfxEvent, pos) == 0x14);
    check("pool constants", audio::kVoices == 64 &&
                                audio::kVoicesPerBus == 8 &&
                                audio::kBuses == 128 &&
                                audio::kSampleRate == 22050);
}

// ------------------------------------------------------------ volume/cull

static void test_volume()
{
    checkf("volume_db(1.0) == 0", audio::volume_db(1.0f) == 0, "%d",
           audio::volume_db(1.0f));
    checkf("volume_db(0.5) == -600", audio::volume_db(0.5f) == -600, "%d",
           audio::volume_db(0.5f));
    checkf("volume_gain(0.5) ~ 10^-0.3",
           near(audio::volume_gain(0.5f), powf(10.0f, -0.3f), 1e-5f),
           "%.7f", audio::volume_gain(0.5f));
    checkf("volume 1/64 -> table[1]",
           audio::volume_db(1.0f / 64.0f) == -3610, "%d",
           audio::volume_db(1.0f / 64.0f));
    checkf("volume 0.01 (index 0) silent",
           audio::volume_db(0.01f) == -10000 &&
               audio::volume_gain(0.01f) == 0.0f,
           "db=%d gain=%.6f", audio::volume_db(0.01f),
           audio::volume_gain(0.01f));
    checkf("volume below 1/64 silent",
           audio::volume_db(0.015f) == -10000, "%d",
           audio::volume_db(0.015f));
    checkf("volume above 1 clamps to 0 dB", audio::volume_db(2.0f) == 0, "%d",
           audio::volume_db(2.0f));
    checkf("negative volume silent", audio::volume_db(-1.0f) == -10000, "%d",
           audio::volume_db(-1.0f));

    audio::ChannelVolumes channels;
    check("channel defaults 1.0", channels[0] == 1.0f && channels[1] == 1.0f &&
                                      channels[2] == 1.0f);
    check("channel out of range 0", channels[3] == 0.0f &&
                                       (const_cast<const audio::ChannelVolumes &>(
                                            channels))[3] == 0.0f);
}

static void test_cull()
{
    const float listener[3] = {0.0f, 0.0f, 0.0f};
    const float at250[3] = {250.0f, 0.0f, 0.0f};
    const float past250[3] = {250.001f, 0.0f, 0.0f};
    check("cull 250 m inclusive", audio::in_range(at250, 0.0f, listener));
    check("cull beyond 250 m", !audio::in_range(past250, 0.0f, listener));

    const float p300[3] = {300.0f, 0.0f, 0.0f};
    checkf("radius 166 keeps 300 m", audio::in_range(p300, 166.0f, listener));
    checkf("radius 165 culls 300 m", !audio::in_range(p300, 165.0f, listener));

    float att = -1.0f;
    const float p3[3] = {3.0f, 0.0f, 0.0f};
    check("attenuation 3 m, volume 0.5",
          audio::attenuation_of(p3, 0.0f, 0.5f, listener, &att) &&
              near(att, 36.0f));
    const float p2[3] = {2.0f, 0.0f, 0.0f};
    check("attenuation radius floor 16",
          audio::attenuation_of(p2, 2.0f, 0.5f, listener, &att) &&
              near(att, 64.0f));
    check("attenuation culls at 300 m",
          !audio::attenuation_of(p300, 0.0f, 1.0f, listener, &att));
}

// ------------------------------------------------------------ voice pool

struct MockBackend : audio::AudioBackend {
    struct PlayCall {
        int bus;
        audio::Voice voice;
    };
    std::vector<PlayCall> plays;
    std::vector<int> updates;
    std::vector<audio::Voice> update_voices;
    std::vector<int> stops;
    std::vector<int> live;
    bool bus_ok[audio::kBuses];
    bool fail_play = false;
    int available = 4;
    int per_bus = audio::kVoicesPerBus;
    int next_handle = 1;

    MockBackend()
    {
        for (int i = 0; i < audio::kBuses; i++)
            bus_ok[i] = true;
    }

    int available_voices() const override { return available; }
    int bus_limit(int) const override { return per_bus; }
    bool bus_available(int bus) const override
    {
        return bus >= 0 && bus < audio::kBuses && bus_ok[bus];
    }
    int play(int bus, const audio::Voice &voice) override
    {
        PlayCall call;
        call.bus = bus;
        call.voice = voice;
        plays.push_back(call);
        if (fail_play)
            return -1;
        const int handle = next_handle++;
        live.push_back(handle);
        return handle;
    }
    void update(int handle, const audio::Voice &voice) override
    {
        updates.push_back(handle);
        update_voices.push_back(voice);
    }
    bool playing(int handle) override
    {
        for (size_t i = 0; i < live.size(); i++)
            if (live[i] == handle)
                return true;
        return false;
    }
    void stop(int handle) override
    {
        stops.push_back(handle);
        for (size_t i = 0; i < live.size(); i++) {
            if (live[i] == handle) {
                live.erase(live.begin() + (long)i);
                return;
            }
        }
    }
};

static audio::SfxEvent make_event(uint32_t flags, float volume, float x = 0.0f,
                                  float y = 0.0f, float z = 0.0f)
{
    audio::SfxEvent event = {};
    event.flags = flags;
    event.volume = volume;
    event.pitch = 1.0f;
    event.pos[0] = x;
    event.pos[1] = y;
    event.pos[2] = z;
    return event;
}

static bool slot_free(const audio::Voice &slot)
{
    return slot.state == 0 && slot.bus == 0 && slot.handle == -1 &&
           memcmp(&slot.attenuation, &audio::kFreeAttenuationBits,
                  sizeof(float)) == 0;
}

static int find_slot(const audio::VoicePool &pool, int instance, int owner)
{
    for (int i = 0; i < audio::kVoices; i++) {
        const audio::Voice &slot = pool.slots[i];
        if (slot.state != 0 && slot.instance == instance &&
            slot.owner == owner)
            return i;
    }
    return -1;
}

static void test_pool()
{
    audio::VoicePool pool;
    bool all_free = true;
    for (int i = 0; i < audio::kVoices; i++)
        all_free = all_free && slot_free(pool.slots[i]);
    check("pool starts free with sentinel", all_free);

    check("3D event at 0.01 volume dropped",
          pool.post(0, 0, 0x10, make_event(0, 0.01f, 10.0f)) == -1 &&
              slot_free(pool.slots[0]));
    check("3D event beyond 250 m dropped",
          pool.post(0, 0, 0x10, make_event(0, 1.0f, 300.0f)) == -1);
    check("2D event skips volume gate",
          pool.post(0, 0, 0x10, make_event(audio::kSfx2D, 0.0f, 1000.0f)) ==
                  0 &&
              pool.slots[0].state == 1 && pool.slots[0].attenuation == 0.0f);
    pool = audio::VoicePool();

    check("3D event allocates slot 0",
          pool.post(3, 0, 0x10, make_event(0, 0.5f, 10.0f)) == 0 &&
              pool.slots[0].state == 1 && pool.slots[0].bus == 3 &&
              pool.slots[0].owner == 0x10 &&
              near(pool.slots[0].attenuation, 400.0f));

    audio::SfxEvent update = make_event(0, 0.25f, 20.0f);
    update.pitch = 2.0f;
    check("instance post allocates",
          pool.post(3, 7, 0x10, update) == 0 &&
              find_slot(pool, 7, 0x10) >= 0);
    int inst = find_slot(pool, 7, 0x10);
    check("instance update reuses slot",
          pool.post(3, 7, 0x10, update) == 0 &&
              find_slot(pool, 7, 0x10) == inst &&
              near(pool.slots[inst].volume, 0.25f) &&
              near(pool.slots[inst].pitch, 2.0f) &&
              near(pool.slots[inst].attenuation, 6400.0f) &&
              pool.slots[0].state == 1);
    check("instance owner mismatch allocates",
          pool.post(3, 7, 0x11, update) == 0 &&
              find_slot(pool, 7, 0x11) >= 0 &&
              find_slot(pool, 7, 0x11) != inst);

    MockBackend backend;
    audio::VoicePool service_pool;
    service_pool.post(0, 5, 0x80, make_event(audio::kSfxLoop | audio::kSfx2D,
                                             1.0f));
    service_pool.service(backend);
    const int loop_slot = find_slot(service_pool, 5, 0x80);
    check("service starts loop voice",
          loop_slot >= 0 && service_pool.slots[loop_slot].state == 2 &&
              service_pool.slots[loop_slot].handle >= 0);
    service_pool.post(0, 5, 0x80, make_event(audio::kSfxLoop | audio::kSfx2D,
                                             0.5f));
    check("loop re-post enters state 3",
          loop_slot >= 0 && service_pool.slots[loop_slot].state == 3);
    service_pool.service(backend);
    check("service updates loop voice",
          loop_slot >= 0 && service_pool.slots[loop_slot].state == 2 &&
              backend.updates.size() == 1 &&
              near(backend.update_voices[0].volume, 0.5f));
    service_pool.service(backend);
    check("unrefreshed loop voice stopped",
          loop_slot >= 0 && slot_free(service_pool.slots[loop_slot]) &&
              backend.stops.size() == 1);

    MockBackend priority;
    priority.available = 1;
    audio::VoicePool prio_pool;
    prio_pool.post(0, 0, 0x10, make_event(0, 0.5f, 30.0f));
    prio_pool.post(0, 0, 0x20, make_event(0, 0.5f, 10.0f));
    prio_pool.post(0, 0, 0x30, make_event(0, 0.5f, 20.0f));
    prio_pool.service(priority);
    checkf("budget reserves nearest attenuation",
           priority.plays.size() == 1 && prio_pool.slots[1].state == 2 &&
               prio_pool.slots[0].state == 0 && prio_pool.slots[2].state == 0,
           "plays=%zu", priority.plays.size());

    MockBackend limited;
    limited.per_bus = 2;
    limited.available = 100;
    audio::VoicePool limit_pool;
    for (int i = 0; i < 3; i++)
        limit_pool.post(0, 0, 0x40 + i, make_event(0, 1.0f, 5.0f));
    limit_pool.service(limited);
    checkf("per-bus limit 8/2 blocks the third", limited.plays.size() == 2,
           "plays=%zu", limited.plays.size());

    MockBackend no_bus;
    no_bus.bus_ok[0] = false;
    audio::VoicePool no_bus_pool;
    no_bus_pool.post(0, 0, 0x10, make_event(0, 1.0f, 5.0f));
    no_bus_pool.service(no_bus);
    check("unavailable bus frees the voice",
          no_bus.plays.empty() && slot_free(no_bus_pool.slots[0]));

    MockBackend failing;
    failing.fail_play = true;
    audio::VoicePool fail_pool;
    fail_pool.post(0, 0, 0x10, make_event(0, 1.0f, 5.0f));
    fail_pool.service(failing);
    check("failed play frees the slot",
          failing.plays.size() == 1 && slot_free(fail_pool.slots[0]));

    audio::VoicePool full_pool;
    for (int i = 0; i < audio::kVoices; i++)
        full_pool.post(0, 0, 1, make_event(audio::kSfx2D, 1.0f));
    check("65th uninstanced post fails without stealing",
          full_pool.post(0, 0, 2, make_event(audio::kSfx2D, 1.0f)) == -1 &&
              full_pool.slots[0].owner == 1);

    MockBackend stop_backend;
    audio::VoicePool stop_pool;
    stop_pool.post(2, 9, 0x99, make_event(audio::kSfx2D, 1.0f));
    stop_pool.service(stop_backend);
    const int stop_slot = find_slot(stop_pool, 9, 0x99);
    const int handle =
        stop_slot >= 0 ? stop_pool.slots[stop_slot].handle : -1;
    check("stop matches instance/owner",
          stop_slot >= 0 && stop_pool.stop(9, 0x99, stop_backend) == 0 &&
              stop_backend.stops.size() == 1 && stop_backend.stops[0] == handle &&
              slot_free(stop_pool.slots[stop_slot]));
    check("stop unknown instance returns -1",
          stop_pool.stop(9, 0x9a, stop_backend) == -1);
}

// ------------------------------------------------------------ post_event

static void test_post_event()
{
    audio::VoicePool pool;
    audio::AudioAssets assets;
    assets.set(500, 3, 0.5f);
    const float pos[3] = {10.0f, 0.0f, 0.0f};
    const float vec1[3] = {2.0f, 3.0f, 4.0f};
    const float vec2[3] = {5.0f, 6.0f, 7.0f};
    int result = audio::post_event(pool, assets, 500, 0x80, 2.0f, 0.5f, 0u,
                                   pos, 1, 0.0f, vec1, vec2);
    const int posted_slot = find_slot(pool, 1, 0x80);
    const audio::Voice &slot = pool.slots[posted_slot >= 0 ? posted_slot : 0];
    checkf("post_event volume/fields",
           result == 1 && slot.state == 1 && slot.bus == 3 &&
               slot.owner == 0x80 && slot.instance == 1 &&
               near(slot.volume, 0.25f) && near(slot.pitch, 2.0f) &&
               near(slot.pos[0], 10.0f) && near(slot.vec1[1], 3.0f) &&
               near(slot.vec2[2], 7.0f) && slot.radius == 0.0f,
           "ret=%d vol=%.4f", result, slot.volume);
    check("post_event unknown asset drops",
          audio::post_event(pool, assets, 501, 1, 1.0f, 1.0f, 0u, pos, 0,
                            0.0f, nullptr, nullptr) == -1);

    assets.set(501, 0, 0.02f);
    checkf("post_event final volume 0.01 drops",
           audio::post_event(pool, assets, 501, 1, 1.0f, 0.5f, 0u, nullptr, 0,
                             0.0f, nullptr, nullptr) == -1);
    assets.set(501, 0, 0.5f);
    audio::VoicePool twod_pool;
    check("post_event null pos adds 2D flag",
          audio::post_event(twod_pool, assets, 501, 1, 1.0f, 0.04f, 0u,
                            nullptr, 0, 0.0f, nullptr, nullptr) == 0 &&
              twod_pool.slots[0].state == 1 &&
              twod_pool.slots[0].flags == audio::kSfx2D);
    const float far[3] = {300.0f, 0.0f, 0.0f};
    check("post_event culls 3D before pool",
          audio::post_event(pool, assets, 500, 1, 1.0f, 1.0f, 0u, far, 0,
                            0.0f, nullptr, nullptr) == -1);

    audio::VoicePool full_pool;
    for (int i = 0; i < audio::kVoices; i++)
        audio::post_event(full_pool, assets, 501, 1, 1.0f, 1.0f, 0u, nullptr,
                          0, 0.0f, nullptr, nullptr);
    const int full_result = audio::post_event(full_pool, assets, 501, 0x99,
                                              1.0f, 1.0f, 0u, nullptr, 9,
                                              0.0f, nullptr, nullptr);
    checkf("post_event returns instance when pool full",
           full_result == 9 && full_pool.slots[0].owner == 1, "ret=%d",
           full_result);
}

// ------------------------------------------------------------ SfxTable

struct EmitLog {
    int count = 0;
    audio::SfxEmit items[64];
    void reset() { count = 0; }
};

static void capture(void *ctx, const audio::SfxEmit &emit)
{
    EmitLog *log = (EmitLog *)ctx;
    if (log->count < 64)
        log->items[log->count] = emit;
    log->count++;
}

static void test_sfx_table()
{
    audio::SfxTable table;
    check("SfxTable defaults",
          table.count == 0 && table.value_scale == 1.0f &&
              table.items[0].owner == 0 && table.items[0].next == -1);

    checkf("add_item stores fields",
           table.add_item(0x100, 2.0f, 10.0f, 20.0f, 0.25f, 1.0f) &&
               table.count == 1 && table.items[0].resource == 0x100 &&
               table.items[0].owner == 0x180 &&
               near(table.items[0].inv_pitch, 0.5f) &&
               near(table.items[0].pmin, 10.0f) &&
               near(table.items[0].pmax, 20.0f) &&
               near(table.items[0].vmin, 0.25f) &&
               near(table.items[0].vmax, 1.0f),
           "owner=0x%x inv=%.3f", table.items[0].owner,
           table.items[0].inv_pitch);
    check("link next/prev", table.add_item(0x101, 1.0f, 20.0f, 30.0f, 0.0f,
                                           1.0f) &&
                                table.items[0].next == 1 &&
                                table.items[1].prev == 0 &&
                                table.items[1].owner == 0x181);
    for (int i = 2; i < 16; i++)
        table.add_item(0x200 + i, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    checkf("table full at 16",
           table.count == 16 &&
               !table.add_item(0x999, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f),
           "count=%d", table.count);
    table.clear();
    check("clear unlinks", table.count == 0 && table.items[0].owner == 0 &&
                              table.items[15].owner == 0);

    const float listener[3] = {0.0f, 0.0f, 0.0f};
    EmitLog log;
    table.add_item(0x100, 2.0f, 0.0f, 100.0f, 0.2f, 1.0f);
    table.add_item(0x101, 4.0f, 50.0f, 150.0f, 0.3f, 1.5f);
    table.evaluate(25.0f, 1.0f, 3u, nullptr, 0, 0.0f, nullptr, nullptr,
                   listener, capture, &log);
    checkf("evaluator below overlap",
           log.count == 1 && log.items[0].resource == 0x100 &&
               near(log.items[0].pitch, 12.5f) &&
               near(log.items[0].volume, 0.4f),
           "count=%d pitch=%.4f vol=%.4f", log.count,
           log.items[0].pitch, log.items[0].volume);

    log.reset();
    table.evaluate(75.0f, 1.0f, 3u, nullptr, 7, 0.0f, nullptr, nullptr,
                   listener, capture, &log);
    checkf("evaluator crossfade 75",
           log.count == 2 && near(log.items[0].volume, 0.4f) &&
               near(log.items[0].pitch, 37.5f) &&
               near(log.items[1].volume, 0.3f) &&
               near(log.items[1].pitch, 18.75f) &&
               log.items[1].instance == 7,
           "count=%d v0=%.4f v1=%.4f", log.count, log.items[0].volume,
           log.items[1].volume);

    log.reset();
    table.evaluate(0.0f, 1.0f, 3u, nullptr, 0, 0.0f, nullptr, nullptr,
                   listener, capture, &log);
    check("evaluator rejects value <= 0", log.count == 0);

    const float far[3] = {300.0f, 0.0f, 0.0f};
    log.reset();
    table.evaluate(25.0f, 1.0f, 3u, far, 0, 0.0f, nullptr, nullptr, listener,
                   capture, &log);
    check("evaluator culls 3D", log.count == 0);

    table.value_scale = 2.0f;
    log.reset();
    table.evaluate(25.0f, 1.0f, 3u, nullptr, 0, 0.0f, nullptr, nullptr,
                   listener, capture, &log);
    checkf("value_scale applies before match",
           log.count == 1 && near(log.items[0].pitch, 25.0f) &&
               near(log.items[0].volume, 0.6f),
           "count=%d pitch=%.4f vol=%.4f", log.count,
           log.items[0].pitch, log.items[0].volume);
    table.value_scale = 1.0f;

    audio::SfxTable fallback;
    fallback.add_item(0x300, 2.0f, 10.0f, 20.0f, 0.4f, 1.0f);
    fallback.add_item(0x301, 1.0f, 100.0f, 200.0f, 1.0f, 1.0f);
    log.reset();
    fallback.evaluate(5.0f, 1.0f, 0u, nullptr, 0, 0.0f, nullptr, nullptr,
                      listener, capture, &log);
    checkf("fallback smallest pmin",
           log.count == 1 && log.items[0].resource == 0x300 &&
               near(log.items[0].pitch, 2.5f) &&
               near(log.items[0].volume, 0.2f),
           "count=%d pitch=%.4f vol=%.4f", log.count,
           log.items[0].pitch, log.items[0].volume);
    log.reset();
    fallback.evaluate(3000.0f, 1.0f, 0u, nullptr, 0, 0.0f, nullptr, nullptr,
                      listener, capture, &log);
    check("fallback limited to <= 2500", log.count == 0);

    fallback.items[0].owner = 0;
    log.reset();
    fallback.evaluate(5.0f, 1.0f, 0u, nullptr, 0, 0.0f, nullptr, nullptr,
                      listener, capture, &log);
    check("owner 0 breaks the scan without fallback", log.count == 0);
}

// ------------------------------------------------------------ engine SFX

static void add_broad(audio::SfxTable &table, int32_t resource, float pitch = 1.0f)
{
    table.add_item(resource, pitch, 0.0f, 20000.0f, 1.0f, 1.0f);
}

static void test_engine_sfx()
{
    const float listener[3] = {0.0f, 0.0f, 0.0f};
    const float rpm_value = fabsf(1000.0f * 9.549296f) + 1.0f;

    {
        audio::EngineSfx engine;
        add_broad(engine.tables[0], 0x100);
        engine.pitch_scale = 0.0f;
        audio::EngineState state;
        state.rpm = 1000.0f;
        state.throttle = 0.8f;
        EmitLog log;
        engine.update(state, 1.0f, 1.0f, nullptr, listener, 0, capture, &log);
        check("pitch_scale 0 disables engine update", log.count == 0);
    }

    {
        audio::EngineSfx engine;
        add_broad(engine.tables[0], 0x100);
        add_broad(engine.tables[1], 0x200);
        audio::EngineState state;
        state.rpm = 1000.0f;
        state.throttle = 0.05f;
        EmitLog log;
        engine.update(state, 2.0f, 1.0f, nullptr, listener, 0, capture, &log);
        checkf("low throttle picks table1",
               log.count == 1 && log.items[0].resource == 0x200 &&
                   log.items[0].owner == 0x280 &&
                   near(log.items[0].pitch, rpm_value) &&
                   near(log.items[0].volume, 2.0f) &&
                   log.items[0].flags == 3u,
               "count=%d res=0x%x pitch=%.3f vol=%.3f", log.count,
               log.items[0].resource, log.items[0].pitch,
               log.items[0].volume);
    }

    {
        audio::EngineSfx engine;
        add_broad(engine.tables[0], 0x100);
        add_broad(engine.tables[1], 0x200);
        audio::EngineState state;
        state.rpm = 1000.0f;
        state.throttle = 0.3f;
        EmitLog log;
        engine.update(state, 1.0f, 1.0f, nullptr, listener, 0, capture, &log);
        checkf("throttle crossfade table1/table0",
               log.count == 2 && log.items[0].resource == 0x200 &&
                   log.items[1].resource == 0x100 &&
                   near(log.items[0].volume, 0.5f) &&
                   near(log.items[1].volume, 0.5f),
               "count=%d v0=%.4f v1=%.4f", log.count,
               log.items[0].volume, log.items[1].volume);
    }

    {
        audio::EngineSfx engine;
        add_broad(engine.tables[0], 0x100);
        add_broad(engine.tables[2], 0x300);
        engine.exhaust_min_vol = 0.25f;
        audio::EngineState state;
        state.rpm = 1000.0f;
        state.throttle = 0.8f;
        EmitLog log;
        engine.update(state, 1.0f, 1.0f, nullptr, listener, 0, capture, &log);
        checkf("high throttle table0 plus table2",
               log.count == 2 && log.items[0].resource == 0x100 &&
                   log.items[1].resource == 0x300 &&
                   near(log.items[0].volume, 1.0f) &&
                   near(log.items[1].volume, 0.85f),
               "count=%d v0=%.4f v1=%.4f", log.count,
               log.items[0].volume, log.items[1].volume);
    }

    {
        audio::EngineSfx engine;
        add_broad(engine.tables[0], 0x100);
        add_broad(engine.tables[2], 0x300);
        engine.starter_resource = 0x400;
        audio::EngineState state;
        state.rpm = 1000.0f;
        state.throttle = 0.8f;
        state.shifting = true;
        state.starter = true;
        EmitLog log;
        engine.update(state, 1.0f, 1.0f, nullptr, listener, 0, capture, &log);
        checkf("starter node during shift",
               log.count == 2 && log.items[0].resource == 0x400 &&
                   near(log.items[0].pitch, 1.0f) &&
                   near(log.items[0].volume, 1.0f) &&
                   log.items[0].flags == 3u,
               "count=%d res=0x%x", log.count, log.items[0].resource);
    }

    {
        audio::EngineSfx engine;
        add_broad(engine.tables[0], 0x100);
        add_broad(engine.tables[2], 0x300);
        engine.nitro_resource = 0x500;
        engine.nitro_pitch_divisor = 2.0f;
        audio::EngineState state;
        state.rpm = 1000.0f;
        state.throttle = 0.8f;
        state.nitro_firing = true;
        state.nitro_amount = 0.5f;
        EmitLog log;
        engine.update(state, 2.0f, 1.0f, nullptr, listener, 0, capture, &log);
        checkf("nitro node pitch/volume",
               log.count == 3 && log.items[2].resource == 0x500 &&
                   near(log.items[2].pitch, rpm_value / 2.0f) &&
                   near(log.items[2].volume, 1.0f) &&
                   log.items[2].flags == 3u,
               "count=%d res=0x%x pitch=%.3f vol=%.3f", log.count,
               log.items[2].resource, log.items[2].pitch,
               log.items[2].volume);
    }

    {
        audio::EngineSfx engine;
        engine.nitro_start_resource = 0x600;
        audio::EngineState state;
        state.rpm = 1000.0f;
        state.throttle = 0.05f;
        state.nitro_start = true;
        EmitLog log;
        engine.update(state, 1.0f, 1.0f, nullptr, listener, 0, capture, &log);
        checkf("nitro start node flags 0",
               log.count == 1 && log.items[0].resource == 0x600 &&
                   near(log.items[0].pitch, 1.0f) &&
                   near(log.items[0].volume, 1.0f) &&
                   log.items[0].flags == 0u,
               "count=%d res=0x%x flags=%u", log.count,
               log.items[0].resource, log.items[0].flags);
    }

    {
        audio::EngineSfx engine;
        add_broad(engine.tables[0], 0x100);
        add_broad(engine.tables[2], 0x300);
        audio::EngineState state;
        state.on_ground = false;
        state.air_speed = 100.0f;
        EmitLog log;
        engine.update(state, 1.0f, 1.0f, nullptr, listener, 0, capture, &log);
        const float air_value = fabsf(100.0f * 9.549296f) + 1.0f;
        checkf("air branch uses table2",
               log.count == 1 && log.items[0].resource == 0x300 &&
                   near(log.items[0].pitch, air_value),
               "count=%d res=0x%x pitch=%.3f", log.count,
               log.items[0].resource, log.items[0].pitch);
        engine.tables[2].clear();
        log.reset();
        engine.update(state, 1.0f, 1.0f, nullptr, listener, 0, capture, &log);
        checkf("air branch falls back to table0",
               log.count == 1 && log.items[0].resource == 0x100,
               "count=%d res=0x%x", log.count, log.items[0].resource);
    }

    {
        audio::EngineSfx engine;
        engine.exhaust_resource[0] = 0x700;
        engine.exhaust_resource[1] = 0x800;
        engine.exhaust_timer[0] = 1000.0f;
        engine.exhaust_timer[1] = 10000.0f;
        audio::EngineState state;
        EmitLog log;
        engine.update(state, 1.0f, 2.0f, nullptr, listener, 0, capture, &log);
        checkf("exhaust timers pitch/volume/reset",
               log.count == 2 && log.items[0].resource == 0x700 &&
                   near(log.items[0].pitch, 1.5f) &&
                   near(log.items[0].volume, 1.0f) &&
                   log.items[1].resource == 0x800 &&
                   near(log.items[1].pitch, 2.0f) &&
                   near(log.items[1].volume, 2.0f) &&
                   engine.exhaust_timer[0] == 0.0f &&
                   engine.exhaust_timer[1] == 10000.0f,
               "count=%d t0=%.1f t1=%.1f", log.count,
               engine.exhaust_timer[0], engine.exhaust_timer[1]);
        engine.exhaust_timer[0] = 10000.0f;
        engine.exhaust_timer[1] = 0.0f;
        log.reset();
        engine.update(state, 1.0f, 2.0f, nullptr, listener, 0, capture, &log);
        checkf("exhaust volume clamps at 4",
               log.count == 1 && near(log.items[0].volume, 8.0f) &&
                   near(log.items[0].pitch, 2.0f),
               "count=%d vol=%.3f pitch=%.3f", log.count,
               log.items[0].volume, log.items[0].pitch);
        engine.exhaust_resource[0] = 0;
        engine.exhaust_timer[0] = 1000.0f;
        engine.exhaust_timer[1] = 0.0f;
        log.reset();
        engine.update(state, 1.0f, 2.0f, nullptr, listener, 0, capture, &log);
        check("exhaust timer resets with null node",
              log.count == 0 && engine.exhaust_timer[0] == 0.0f);
    }

    {
        audio::EngineSfx engine;
        engine.horn_resource[2] = 0x900;
        engine.horn_pitch[2] = 1.5f;
        engine.pitch_scale = 1.25f;
        audio::EngineState state;
        state.horn = true;
        state.horn_id = 2;
        EmitLog log;
        engine.update(state, 1.0f, 2.0f, nullptr, listener, 0, capture, &log);
        checkf("horn node pitch scaled",
               log.count == 1 && log.items[0].resource == 0x900 &&
                   log.items[0].owner == 0x980 &&
                   near(log.items[0].pitch, 1.875f) &&
                   near(log.items[0].volume, 2.0f) &&
                   log.items[0].flags == 3u,
               "count=%d pitch=%.4f vol=%.3f", log.count,
               log.items[0].pitch, log.items[0].volume);
        state.horn_id = 4;
        log.reset();
        engine.update(state, 1.0f, 2.0f, nullptr, listener, 0, capture, &log);
        check("horn id out of range ignored", log.count == 0);
        state.horn_id = 2;
        state.horn = false;
        log.reset();
        engine.update(state, 1.0f, 2.0f, nullptr, listener, 0, capture, &log);
        check("horn off ignored", log.count == 0);
        engine.set_part_loop_params(0.3f, 0.7f);
        check("part loop params",
              engine.part_loop_volume == 0.3f &&
                  engine.part_loop_pitch == 0.7f);
    }

    {
        audio::EngineSfx engine;
        add_broad(engine.tables[0], 0x100);
        audio::EngineState state;
        state.rpm = 1000.0f;
        state.throttle = 0.05f;
        EmitLog log;
        engine.update(state, 1.0f, 1.0f, nullptr, listener, 0, capture, &log);
        checkf("empty table1 falls back to table0",
               log.count == 1 && log.items[0].resource == 0x100,
               "count=%d res=0x%x", log.count, log.items[0].resource);
    }
}

// ------------------------------------------------------------ music

struct MockOutput : audio::MusicOutput {
    struct Open {
        int kind;
        std::string path;
        bool loop;
        const audio::MusicSet *context;
    };
    struct Volume {
        int kind;
        float volume;
    };
    std::vector<Open> opens;
    std::vector<Volume> volumes;
    std::vector<int> stops;
    bool open_result = true;

    bool open(int kind, const std::string &path, bool loop,
              const audio::MusicSet *context) override
    {
        opens.push_back(Open{kind, path, loop, context});
        return open_result;
    }
    void set_volume(int kind, float volume) override
    {
        volumes.push_back(Volume{kind, volume});
    }
    void stop(int kind) override { stops.push_back(kind); }
};

static uint32_t tick_sequence[16];
static int tick_index = 0;

static uint32_t fake_tick()
{
    return tick_sequence[tick_index++ % 16];
}

static void test_music()
{
    audio::MusicSystem music;
    MockOutput out;
    music.set_output(&out);
    music.set_initialized(true);

    check("music starts with no current set",
          music.current() == nullptr && music.current_index() == -1 &&
              music.start() == -1 && music.stop() == -1 &&
              music.play_track(0) == -1);
    music.play_track(std::string("none.wav"));
    check("music no current: no stream calls", out.opens.empty() &&
                                                   out.volumes.empty() &&
                                                   out.stops.empty());
    checkf("getters without current",
           music.mode() == -1 && music.track() == -1 &&
               music.count() == -1 && music.set_volume(0.5f) == 0.0f &&
               music.set_mode(3) == 3,
           "mode=%d", music.mode());

    audio::MusicSet &set0 = music.sets[0];
    set0.set_tracks({"a.wav", "b.mp3", "c.ogg"});
    checkf("set_tracks packs NUL names",
           set0.count == 3 &&
               set0.strings == std::string("a.wav\0b.mp3\0c.ogg\0", 18) &&
               set0.track_path(0) == "a.wav" &&
               set0.track_path(1) == "b.mp3" &&
               set0.track_path(2) == "c.ogg" &&
               set0.track_path(3).empty(),
           "count=%d len=%zu", set0.count, set0.strings.size());
    check("music set defaults are zero",
          set0.volume == 0.0f && set0.track == 0 && set0.mode == 0 &&
              set0.prev_mode == 0 && set0.custom_track.empty());

    music.change_set(0);
    checkf("change_set starts via set_volume at channel 1",
           music.current_index() == 0 && music.track() == 0 &&
               music.stream_kind() == 1 && out.opens.size() == 1 &&
               out.opens[0].kind == 1 && out.opens[0].path == "a.wav" &&
               out.opens[0].loop && out.opens[0].context == nullptr &&
               !out.volumes.empty() && out.volumes.back().kind == 1 &&
               near(out.volumes.back().volume, 1.0f),
           "opens=%zu kind=%d", out.opens.size(), music.stream_kind());

    music.play_track(1);
    checkf("play_track(int) opens mp3",
           music.track() == 1 && out.opens.size() == 2 &&
               out.opens[1].kind == 2 && out.opens[1].path == "b.mp3" &&
               out.opens[1].loop && out.stops.back() == 1,
           "track=%d opens=%zu", music.track(), out.opens.size());
    const size_t opens_mp3 = out.opens.size();
    music.play_track(2);
    checkf("play_track unsupported extension",
           music.track() == 2 && out.opens.size() == opens_mp3 &&
               music.stream_kind() == 0 && out.stops.back() == 2,
           "track=%d kind=%d opens=%zu", music.track(),
           music.stream_kind(), out.opens.size());
    music.play_track(3);
    checkf("play_track(count) wraps to 0",
           music.track() == 0 && out.opens.back().path == "a.wav" &&
               out.opens.back().kind == 1,
           "track=%d", music.track());

    const size_t opens_before = out.opens.size();
    music.play_track(-1);
    checkf("play_track(-1) selects last and .ogg fails open",
           music.track() == 2 && out.opens.size() == opens_before &&
               music.stream_kind() == 0,
           "track=%d kind=%d opens=%zu", music.track(), music.stream_kind(),
           out.opens.size());

    music.next();
    checkf("next wraps to first", music.track() == 0 &&
                                     out.opens.back().path == "a.wav",
           "track=%d", music.track());
    music.prev();
    checkf("prev wraps to last and open fails",
           music.track() == 2 && out.opens.back().path == "a.wav",
           "track=%d", music.track());

    tick_sequence[0] = 0;
    tick_index = 0;
    music.set_tick_source(fake_tick);
    music.random();
    checkf("random picks a different track", music.track() == 0 &&
                                                 out.opens.back().path ==
                                                     "a.wav",
           "track=%d", music.track());
    tick_sequence[0] = 0;
    tick_sequence[1] = 1;
    tick_index = 0;
    music.random();
    checkf("random retries on equal tick", music.track() == 1 &&
                                               out.opens.back().path ==
                                                   "b.mp3",
           "track=%d", music.track());
    music.set_tick_source(nullptr);
    music.random();
    checkf("random fallback tick terminates", music.track() >= 0 &&
                                                 music.track() < 3,
           "track=%d", music.track());

    check("set_mode accepts 0..5", music.set_mode(5) == 5 &&
                                       music.mode() == 5);
    check("set_mode rejects 6", music.set_mode(6) == 5 && music.mode() == 5);
    check("set_mode rejects negative",
          music.set_mode(static_cast<int>(0xffffffffu)) == 5 &&
              music.mode() == 5);

    music.set_mode(5);
    music.play_track(1);
    checkf("play_track restores mode 5 to prev",
           music.mode() == 0 && out.opens.back().path == "b.mp3",
           "mode=%d track=%d", music.mode(), music.track());

    set0.mode = 0;
    set0.prev_mode = 0;
    music.play_track(std::string("custom.mp3"));
    checkf("play_track(String) mode 5 direct open",
           set0.mode == 5 && set0.prev_mode == 0 &&
               set0.custom_track == "custom.mp3" &&
               out.opens.back().kind == 2 &&
               out.opens.back().path == "custom.mp3" &&
               !out.opens.back().loop &&
               out.opens.back().context == &set0,
           "mode=%d kind=%d loop=%d", set0.mode, out.opens.back().kind,
           out.opens.back().loop);
    music.next();
    checkf("next restores direct mode",
           music.mode() == 0 && music.track() == 2,
           "mode=%d track=%d", music.mode(), music.track());

    music.set_mode(5);
    set0.prev_mode = 5;
    music.play_track(std::string("x.wav"));
    music.next();
    checkf("direct mode 5 restored to 5",
           music.mode() == 5 && out.opens.back().kind == 1,
           "mode=%d kind=%d", music.mode(), out.opens.back().kind);
    set0.mode = 0;
    set0.prev_mode = 0;

    music.play_track(0);
    const float old1 = music.set_volume(0.5f);
    checkf("volume 0.5 applied without restart",
           old1 == 1.0f && near(set0.volume, 0.5f) &&
               near(out.volumes.back().volume, 0.5f),
           "old=%.3f vol=%.3f", old1, set0.volume);
    const float old2 = music.set_volume(0.005f);
    checkf("volume 0.005 stops", near(old2, 0.5f) && set0.volume == 0.0f &&
                                     music.stream_kind() == 0 &&
                                     out.stops.back() == 1,
           "old=%.4f kind=%d", old2, music.stream_kind());
    const float old3 = music.set_volume(0.02f);
    checkf("volume above threshold restarts from 0",
           old3 == 0.0f && music.stream_kind() == 1 &&
               out.opens.back().path == "a.wav" &&
               near(out.volumes.back().volume, 0.02f),
           "old=%.4f kind=%d vol=%.4f", old3, music.stream_kind(),
           out.volumes.back().volume);
    const float old4 = music.set_volume(0.01f);
    checkf("volume exactly 0.01 is accepted",
           near(old4, 0.02f) && set0.volume == 0.01f &&
               music.stream_kind() == 1 &&
               near(out.volumes.back().volume, 0.01f),
           "old=%.4f vol=%.3f kind=%d", old4, set0.volume,
           music.stream_kind());

    set0.volume = 0.009f;
    const size_t opens_low = out.opens.size();
    check("start rejects volume below 0.01",
          music.start() == -1 && out.opens.size() == opens_low);
    set0.volume = 0.01f;
    check("start accepts volume 0.01", music.start() == music.track() &&
                                          out.opens.size() ==
                                              opens_low + 1);
    set0.volume = 1.0f;

    audio::MusicSet &set1 = music.sets[1];
    set1.set_tracks({"d.wav"});
    const size_t opens_change = out.opens.size();
    music.change_set(1);
    checkf("change_set opens new set",
           music.current_index() == 1 && music.track() == 0 &&
               out.opens.size() == opens_change + 1 &&
               out.opens.back().path == "d.wav" &&
               out.opens.back().kind == 1,
           "opens=%zu path=%s", out.opens.size(),
           out.opens.back().path.c_str());
    const size_t opens_same = out.opens.size();
    const size_t stops_same = out.stops.size();
    music.change_set(1);
    check("change_set same id is a no-op",
          out.opens.size() == opens_same && out.stops.size() == stops_same);

    music.change_set(6);
    checkf("change_set invalid stops and clears current",
           music.current() == nullptr && music.stream_kind() == 0,
           "kind=%d current=%d", music.stream_kind(), music.current_index());

    music.set_directshow_available(true);
    set0.set_tracks({"e.bin"});
    set0.volume = 1.0f;
    set0.track = 0;
    music.change_set(0);
    checkf("DirectShow path ignores extension",
           music.stream_kind() == 3 && out.opens.back().kind == 3 &&
               out.opens.back().path == "e.bin" && out.opens.back().loop &&
               out.opens.back().context == nullptr &&
               out.volumes.back().kind == 3,
           "kind=%d path=%s", music.stream_kind(),
           out.opens.back().path.c_str());
    music.stop();
    check("stop stream resets kind", music.stream_kind() == 0 &&
                                         out.stops.back() == 3);
    out.open_result = false;
    const size_t volumes_before = out.volumes.size();
    music.start();
    checkf("DirectShow failed open leaves kind 0",
           music.stream_kind() == 0 && out.volumes.size() == volumes_before,
           "kind=%d", music.stream_kind());
    out.open_result = true;

    music.start();
    check("DirectShow reopens", music.stream_kind() == 3);
    music.set_initialized(false);
    music.play_track(0);
    checkf("uninitialized audio leaves stream kind untouched",
           music.stream_kind() == 3, "kind=%d", music.stream_kind());
    music.set_initialized(true);
    music.stop();
    check("stop after re-init clears kind", music.stream_kind() == 0);

    music.set_channel_volume(0.7f);
    music.sets[2].set_tracks({"f.wav"});
    music.sets[2].volume = 0.0f;
    music.change_set(2);
    checkf("change_set uses music channel volume",
           music.current_index() == 2 && music.stream_kind() == 3 &&
               near(music.sets[2].volume, 0.7f) &&
               near(out.volumes.back().volume, 0.7f),
           "vol=%.3f kind=%d", music.sets[2].volume, music.stream_kind());

    music.set_directshow_available(false);
    music.sets[3].set_tracks({"g.wav"});
    music.sets[3].volume = 1.0f;
    music.change_set(3);
    checkf("direct backend uses .wav after toggling",
           music.stream_kind() == 1 && out.opens.back().path == "g.wav",
           "kind=%d path=%s", music.stream_kind(),
           out.opens.back().path.c_str());
    music.sets[4].set_tracks({"upper.WAV"});
    music.sets[4].volume = 1.0f;
    music.change_set(4);
    checkf("extension compare is case-sensitive",
           music.stream_kind() == 0, "kind=%d", music.stream_kind());
}

// ------------------------------------------------------------ mixer

static void fill_sample(audio::SoftwareMixer &mixer, int bus, int16_t l,
                        int16_t r, int frames, int rate = audio::kSampleRate)
{
    std::vector<int16_t> pcm((size_t)frames * 2);
    for (int i = 0; i < frames; i++) {
        pcm[(size_t)i * 2] = l;
        pcm[(size_t)i * 2 + 1] = r;
    }
    mixer.set_sample(bus, pcm.data(), (size_t)frames, rate);
}

static audio::Voice make_voice(float volume = 1.0f, float pitch = 1.0f,
                               uint32_t flags = audio::kSfx2D)
{
    audio::Voice voice = {};
    voice.volume = volume;
    voice.pitch = pitch;
    voice.flags = flags;
    voice.handle = -1;
    return voice;
}

static void test_mixer()
{
    audio::SoftwareMixer mixer;
    check("mixer defaults", mixer.available_voices() == 4 &&
                                !mixer.sample(5) &&
                                mixer.bus_min_distance(5) == 1.0f &&
                                mixer.bus_max_distance(5) == 100.0f);

    const int16_t stereo[2] = {1000, -1000};
    mixer.set_sample(3, stereo, 1);
    audio::Voice voice = make_voice();
    const int handle = mixer.play(3, voice);
    checkf("mixer play handle", handle == (3 << 16), "handle=0x%x", handle);
    int16_t out[8] = {};
    check("mixer mixes stereo sample", mixer.mix(out, 1) == 1 &&
                                           out[0] == 1000 &&
                                           out[1] == -1000);
    check("mixer non-loop ends", !mixer.playing(handle) &&
                                     mixer.mix(out, 1) == 0);

    fill_sample(mixer, 3, 1000, -1000, 4);
    audio::Voice half = make_voice(0.5f);
    const int half_handle = mixer.play(3, half);
    mixer.mix(out, 1);
    checkf("mixer volume 0.5 uses dB scale",
           out[0] >= 500 && out[0] <= 502,
           "out=%d", out[0]);
    (void)half_handle;

    audio::Voice mute = make_voice(0.0f);
    mixer.stop(half_handle);
    const int mute_handle = mixer.play(3, mute);
    mixer.mix(out, 1);
    check("mixer volume index 0 silent", out[0] == 0 && out[1] == 0);
    mixer.stop(mute_handle);

    fill_sample(mixer, 4, 20000, 20000, 2);
    const int a = mixer.play(4, make_voice());
    const int b = mixer.play(4, make_voice());
    check("mixer two voices", mixer.mix(out, 1) == 2 && out[0] == 32767 &&
                                  out[1] == 32767);
    mixer.stop(a);
    mixer.stop(b);

    fill_sample(mixer, 5, 0, 0, 4, 44100);
    std::vector<int16_t> ramp = {0, 0, 1000, 1000, 2000, 2000, 3000, 3000};
    mixer.set_sample(5, ramp.data(), 4, 44100);
    audio::Voice resample = make_voice(1.0f, 1.0f);
    mixer.play(5, resample);
    int16_t res_out[4] = {};
    mixer.mix(res_out, 2);
    checkf("mixer 44.1k -> 22.05k step 2",
           res_out[0] == 0 && res_out[2] == 2000,
           "%d %d", res_out[0], res_out[2]);

    fill_sample(mixer, 6, 0, 0, 2, 11025);
    mixer.set_sample(6, ramp.data(), 2, 11025);
    audio::Voice interp = make_voice(1.0f, 1.0f);
    const int interp_handle = mixer.play(6, interp);
    int16_t interp_out[4] = {};
    mixer.mix(interp_out, 2);
    checkf("mixer linear interpolation", interp_out[0] == 0 &&
                                             interp_out[2] == 500,
           "%d %d", interp_out[0], interp_out[2]);
    mixer.stop(interp_handle);

    fill_sample(mixer, 7, 100, 200, 2);
    audio::Voice loop = make_voice(1.0f, 1.0f, audio::kSfx2D | audio::kSfxLoop);
    const int loop_handle = mixer.play(7, loop);
    int16_t loop_out[12] = {};
    mixer.mix(loop_out, 6);
    checkf("mixer loop wraps",
           loop_out[0] == 100 && loop_out[1] == 200 &&
               loop_out[2] == 100 && loop_out[3] == 200 &&
               loop_out[4] == 100 && loop_out[5] == 200,
           "%d %d %d %d", loop_out[0], loop_out[1], loop_out[4],
           loop_out[5]);
    check("mixer loop still playing", mixer.playing(loop_handle));
    mixer.stop(loop_handle);

    fill_sample(mixer, 8, 10000, 10000, 1);
    mixer.set_bus_distances(8, 1.0f, 100.0f);
    audio::Voice near_voice = make_voice(1.0f, 1.0f, 0u);
    near_voice.pos[0] = 50.0f;
    const int near_handle = mixer.play(8, near_voice);
    mixer.mix(out, 1);
    const float expected = (100.0f - 50.0f) / (100.0f - 1.0f) * 1.0f / 50.0f;
    checkf("mixer 3D distance gain",
           out[0] >= 100 && out[0] <= 102,
           "out=%d expected=%.4f", out[0], expected * 10000.0f);
    mixer.stop(near_handle);

    audio::Voice radius_voice = make_voice(1.0f, 1.0f, 0u);
    radius_voice.pos[0] = 50.0f;
    radius_voice.radius = 20.0f;
    const int radius_handle = mixer.play(8, radius_voice);
    mixer.mix(out, 1);
    checkf("mixer radius moves reference 20 m",
           out[0] >= 235 && out[0] <= 237, "out=%d", out[0]);
    mixer.stop(radius_handle);

    audio::Voice inside = make_voice(1.0f, 1.0f, 0u);
    inside.pos[0] = 10.0f;
    inside.radius = 20.0f;
    const int inside_handle = mixer.play(8, inside);
    mixer.mix(out, 1);
    checkf("mixer listener inside radius full gain", out[0] == 10000,
           "out=%d", out[0]);
    mixer.stop(inside_handle);

    audio::Voice two_d = make_voice(1.0f, 1.0f, audio::kSfx2D);
    two_d.pos[0] = 1000.0f;
    const int two_d_handle = mixer.play(8, two_d);
    mixer.mix(out, 1);
    check("mixer 2D bypasses distance", out[0] == 10000);
    mixer.stop(two_d_handle);

    const float max_pos[3] = {100.0f, 0.0f, 0.0f};
    check("mixer distance gain at max is 0",
          mixer.distance_gain(8, max_pos, 0.0f) == 0.0f);

    fill_sample(mixer, 9, 1, 1, 1);
    int played = 0;
    for (int i = 0; i < audio::kVoicesPerBus; i++)
        if (mixer.play(9, make_voice()) >= 0)
            played++;
    checkf("mixer 8 voices per bus", played == 8 &&
                                         mixer.play(9, make_voice()) == -1,
           "played=%d", played);
    check("mixer bus_limit", mixer.bus_limit(9) == 8 &&
                                 mixer.bus_limit(-1) == 0);
    check("mixer unavailable bus", !mixer.bus_available(10) &&
                                       mixer.play(10, make_voice()) == -1);

    mixer.set_available_voices(2);
    check("mixer available voices override",
          mixer.available_voices() == 2);

    audio::SoftwareMixer update_mixer;
    fill_sample(update_mixer, 1, 1000, 1000, 4);
    audio::Voice upd = make_voice(1.0f);
    const int upd_handle = update_mixer.play(1, upd);
    upd.volume = 0.5f;
    update_mixer.update(upd_handle, upd);
    update_mixer.mix(out, 1);
    checkf("mixer update changes volume", out[0] >= 500 && out[0] <= 502,
           "out=%d", out[0]);
    update_mixer.stop(upd_handle);
    check("mixer stop deactivates", !update_mixer.playing(upd_handle) &&
                                        update_mixer.mix(out, 1) == 0);
}

static void test_integration()
{
    audio::VoicePool pool;
    audio::SoftwareMixer mixer;
    audio::AudioAssets assets;
    assets.set(700, 2, 1.0f);
    fill_sample(mixer, 2, 1234, -1234, 3);
    const int instance = audio::post_event(pool, assets, 700, 0x80, 1.0f, 0.5f,
                                           0u, nullptr, 4, 0.0f, nullptr,
                                           nullptr);
    pool.service(mixer);
    const int slot_index = find_slot(pool, 4, 0x80);
    int16_t out[6] = {};
    const int mixed = mixer.mix(out, 1);
    checkf("post -> service -> software voice",
           instance == 4 && slot_index >= 0 &&
               pool.slots[slot_index].state == 2 && mixed == 1 && out[0] > 0 &&
               out[0] < 1234,
           "state=%d out=%d", slot_index >= 0 ? pool.slots[slot_index].state : -1,
           out[0]);
    mixer.mix(out, 2);
    pool.service(mixer);
    check("ended voice freed on next service",
          slot_index >= 0 && slot_free(pool.slots[slot_index]));
}

int main()
{
    test_layouts();
    test_volume();
    test_cull();
    test_pool();
    test_post_event();
    test_sfx_table();
    test_engine_sfx();
    test_music();
    test_mixer();
    test_integration();
    printf("\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
