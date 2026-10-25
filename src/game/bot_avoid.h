// Bot obstacle angle-interval solver (exe global DAT_005fe1f4, 0x38 bytes;
// notes/19 s8.2, card 181): reset 0x40ab10, add 0x409fa0, resolve 0x40a8a0.
#pragma once
#include <vector>

struct BotAvoid {
    struct Circle {          // 0x20 bytes
        int link = -1;       // +0 next member circle or -1
        float x, z, vx, vz, r, dist, ang;
    };
    struct Interval {        // 0x20 bytes
        int head;
        float a, b, gap, cx, cz, R, maxr;
    };
    struct HeapEnt { int index; float key; };

    std::vector<Circle> circles;      // [0..2]
    std::vector<Interval> intervals;  // [3..5]
    float F[2] = {0, 0}, dir[2] = {0, 0}, margin = 0, len = 0, range = 0;
    std::vector<HeapEnt> heap;        // global DAT_005e64e4

    void reset(float fx, float fz, float vx, float vz, float margin, float minRange);
    void add(float x, float z, float vx, float vz, float r);
    void resolve(float out[2]);

    static BotAvoid &shared();        // one solver shared by all bots
private:
    void heap_push(HeapEnt e);
    void heap_pop();
    void enclose(Interval &I, float px, float pz, float r);
};
