// Assert-based checks for the vehicle layer: engine curve, Pacejka sanity,
// drivetrain split, and a full car on a plane (acceleration, braking,
// suspension ride height). Build/run:
//   g++ -std=c++17 -Wall -Wextra -Wpedantic -Isrc -o src/build/vehiclecheck
//       src/tools/vehiclecheck.cpp src/vehicle/vehicle.cpp
//       src/physics/world.cpp src/physics/shape.cpp -lm
//   ./src/build/vehiclecheck
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "vehicle/vehicle.h"

static int failures = 0;

static void check(const char *name, bool ok, const char *detail)
{
    printf("[%s] %-54s %s\n", ok ? "PASS" : "FAIL", name, detail ? detail : "");
    if (!ok)
        failures++;
}

// notes/10 reference curve for the DynoData.java defaults, 250 rpm steps.
static void fill_curve(Engine &e)
{
    static const float nm[] = {
        0.0f,   26.2f,  95.2f,  131.3f, 151.5f, 163.0f, 169.1f,
        171.5f, 171.4f, 168.9f, 165.3f, 161.0f, 155.8f, 150.3f,
        144.2f, 137.9f, 131.3f, 124.6f, 117.7f, 110.8f, 103.9f,
        100.2f, 96.3f,  92.3f,  88.3f,  84.4f,  80.4f,  76.5f,
    };
    e.torque_curve.clear();
    for (int i = 0; i < 28; i++)
        e.torque_curve.push_back(TorquePoint{i * 250.0f, nm[i]});
}

static void test_engine_curve()
{
    Engine e;
    fill_curve(e);
    float peak = e.torque_at(1750.0f);
    float mid = e.torque_at(270.0f);
    float expected_mid = 26.2f + (20.0f / 250.0f) * (95.2f - 26.2f);
    char buf[160];

    snprintf(buf, sizeof buf, "1750 rpm %.3f Nm (table 171.5)", peak);
    check("engine curve: peak sample", fabsf(peak - 171.5f) < 0.05f, buf);
    snprintf(buf, sizeof buf, "4250 rpm %.3f Nm (table 124.6)", e.torque_at(4250.0f));
    check("engine curve: falling side", fabsf(e.torque_at(4250.0f) - 124.6f) < 0.05f, buf);
    snprintf(buf, sizeof buf, "270 rpm %.3f Nm (lerp %.3f)", mid, expected_mid);
    check("engine curve: linear interpolation", fabsf(mid - expected_mid) < 0.02f, buf);
    snprintf(buf, sizeof buf, "below/at-end: %.3f %.3f %.3f", e.torque_at(-10.0f),
             e.torque_at(6750.0f), e.torque_at(7000.0f));
    check("engine curve: zero outside [first, last)",
          e.torque_at(-10.0f) == 0.0f && e.torque_at(6750.0f) == 0.0f &&
              e.torque_at(7000.0f) == 0.0f,
          buf);
}

static float peak_long(float load, float mu)
{
    Pacejka pj;
    float best = 0.0f;
    for (float k = 0.005f; k <= 1.0f; k += 0.005f) {
        TyreSample t = tyre_magic(pj, load, 1.0f, k, 0.0f, mu);
        if (fabsf(t.f_long) > best)
            best = fabsf(t.f_long);
    }
    return best;
}

static float peak_lat(float load, float mu)
{
    Pacejka pj;
    float best = 0.0f;
    for (float a = 0.005f; a <= 1.0f; a += 0.005f) {
        TyreSample t = tyre_magic(pj, load, 1.0f, 0.0f, a, mu);
        if (t.f_lat > best)
            best = t.f_lat;
    }
    return best;
}

static void test_pacejka()
{
    Pacejka pj;
    TyreSample zero = tyre_magic(pj, 3000.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    char buf[192];
    snprintf(buf, sizeof buf, "F %.6f flat %.6f flong %.6f sigma %.6f", zero.force,
             zero.f_lat, zero.f_long, zero.sigma);
    check("Pacejka: zero force at zero slip", zero.force == 0.0f && zero.f_lat == 0.0f &&
                                                    zero.f_long == 0.0f,
          buf);

    float pl = peak_long(3000.0f, 1.0f);
    float pl6 = peak_long(6000.0f, 1.0f);
    snprintf(buf, sizeof buf, "peak %.1f N = %.3f*Fz (load 3000)", pl, pl / 3000.0f);
    check("Pacejka: longitudinal peak magnitude", pl > 0.6f * 3000.0f && pl < 1.0f * 3000.0f,
          buf);

    float pt = peak_lat(3000.0f, 1.0f);
    snprintf(buf, sizeof buf, "peak %.1f N = %.3f*Fz (load 3000)", pt, pt / 3000.0f);
    check("Pacejka: lateral peak magnitude", pt > 0.6f * 3000.0f && pt < 1.0f * 3000.0f, buf);

    snprintf(buf, sizeof buf, "mu3000 %.3f  mu6000 %.3f (higher load, lower coefficient)",
             pl / 3000.0f, pl6 / 6000.0f);
    check("Pacejka: load sensitivity", pl6 > pl && pl6 / 6000.0f < pl / 3000.0f - 0.01f,
          buf);
}

static void test_split()
{
    Car car;
    car.wheels.resize(4);
    const float share[4] = {0.4f, 0.1f, 0.3f, 0.2f};
    for (int i = 0; i < 4; i++) {
        car.wheels[i].set_drive(share[i]);
        car.wheels[i].drive_ratio = -1.0f;
    }
    float v[4];
    float D = car.split_drive(100.0f, v);
    float sum = v[0] + v[1] + v[2] + v[3];
    char buf[192];
    snprintf(buf, sizeof buf, "D %.3f  wheels %.1f %.1f %.1f %.1f  sum %.1f", D, v[0], v[1],
             v[2], v[3], sum);
    check("drivetrain: split sums to commanded torque",
          fabsf(D - 1.0f) < 1e-6f && fabsf(v[0] - 40.0f) < 1e-3f &&
              fabsf(v[1] - 10.0f) < 1e-3f && fabsf(v[2] - 30.0f) < 1e-3f &&
              fabsf(v[3] - 20.0f) < 1e-3f && fabsf(sum - 100.0f) < 1e-3f,
          buf);

    car.wheels[1].drive_ratio = -2.0f;
    car.split_drive(100.0f, v);
    sum = v[0] + v[1] + v[2] + v[3];
    snprintf(buf, sizeof buf, "ratio -2 wheel %.2f, sum %.2f (expected 5 / 95)", v[1], sum);
    check("drivetrain: per-wheel ratio sign", fabsf(v[1] - 5.0f) < 1e-3f &&
                                                  fabsf(sum - 95.0f) < 1e-3f,
          buf);
}

struct Rig {
    World world;
    RigidBody ground;
    Car car;

    Rig()
    {
        Shape gs;
        gs.type = SHAPE_PLANE;
        gs.plane_point = Vec3{0.0f, 0.0f, 0.0f};
        gs.plane_normal = Vec3{0.0f, 1.0f, 0.0f};
        gs.friction = 0.9f;
        ground.set_static_shape(gs);
        world.add_body(&ground);

        car.ground = &ground;
        car.body.set_box(Vec3{0.85f, 0.30f, 1.90f}, 1200.0f);
        car.body.pos = Vec3{0.0f, 0.48f, 0.0f};
        world.add_body(&car.body);

        car.wheels.resize(4);
        const float mx[4] = {-0.75f, 0.75f, -0.75f, 0.75f};
        const float mz[4] = {1.25f, 1.25f, -1.25f, -1.25f};
        for (int i = 0; i < 4; i++) {
            Wheel &w = car.wheels[i];
            w.mount = Vec3{mx[i], -0.10f, mz[i]};
            w.radius = 0.32f;
            w.rest_len = 0.15f;
            w.max_len = 0.30f;
            w.min_len = -1.0f;
            w.spring = 30000.0f;
            w.bump = 1500.0f;
            w.rebound = 1050.0f;
            w.friction = 1.0f;
            w.friction_eff = 1.0f;
            w.rolling_resistance = 0.015f;
        }
        car.wheels[2].set_drive(0.5f);
        car.wheels[3].set_drive(0.5f);
        fill_curve(car.engine);
        car.engine.omega = car.engine.idle;
        car.engine.smoothed_omega = car.engine.idle;
        car.gear = 1;
        car.clutch = 0.0f;
    }
};

static void test_acceleration()
{
    Rig rig;
    rig.car.throttle = 0.5f;
    bool finite = true;
    float max_speed = 0.0f;
    for (int i = 0; i < 300; i++) { // 3 s at 100 Hz
        rig.car.step(rig.world, 0.01f);
        float s = vec_length(rig.car.body.vel);
        if (!std::isfinite(s) || !std::isfinite(rig.car.body.pos.y) ||
            !std::isfinite(rig.car.wheels[2].spin) || !std::isfinite(rig.car.engine.omega))
            finite = false;
        if (s > max_speed)
            max_speed = s;
    }
    float speed = vec_length(rig.car.body.vel);
    char buf[224];
    snprintf(buf, sizeof buf, "v %.2f m/s (max %.2f), z %.2f m, y %.3f, rpm %.0f", speed,
             max_speed, rig.car.body.pos.z, rig.car.body.pos.y,
             rig.car.engine.omega * 9.549296f);
    check("car: accelerates from rest", finite && speed > 2.0f && speed < 80.0f, buf);
    snprintf(buf, sizeof buf, "z %.2f m, y %.3f m", rig.car.body.pos.z, rig.car.body.pos.y);
    check("car: stays on the plane and moves forward",
          rig.car.body.pos.z > 1.0f && rig.car.body.pos.y > 0.2f &&
              rig.car.body.pos.y < 1.0f,
          buf);
}

static void test_braking()
{
    Rig rig;
    rig.car.throttle = 0.5f;
    for (int i = 0; i < 200; i++)
        rig.car.step(rig.world, 0.01f);
    float v1 = vec_length(rig.car.body.vel);

    rig.car.throttle = 0.0f;
    rig.car.brake = 1.0f;
    for (Wheel &w : rig.car.wheels)
        w.set_brake(2000.0f);
    for (int i = 0; i < 100; i++)
        rig.car.step(rig.world, 0.01f);
    float v2 = vec_length(rig.car.body.vel);

    char buf[192];
    snprintf(buf, sizeof buf, "before %.2f m/s, after %.2f m/s", v1, v2);
    check("car: braking reduces speed",
          v1 > 1.0f && v2 < v1 * 0.7f && std::isfinite(v2), buf);
}

static void test_suspension_settle()
{
    Rig rig;
    rig.car.engine.running = false;
    float y_min = 1e9f, y_max = -1e9f;
    for (int i = 0; i < 400; i++) { // 4 s at 100 Hz
        rig.car.step(rig.world, 0.01f);
        if (i >= 350) {
            float y = rig.car.body.pos.y;
            if (y < y_min)
                y_min = y;
            if (y > y_max)
                y_max = y;
        }
    }
    float y_span = y_max - y_min;

    // len = body.y + mount.y (ground y = 0); static compression is mg/4k.
    float expected_y = -rig.car.wheels[0].mount.y + rig.car.wheels[0].rest_ray() -
                       1200.0f * 9.81f / (4.0f * rig.car.wheels[0].spring);
    float dy = fabsf(rig.car.body.pos.y - expected_y);
    bool loads_ok = true;
    float load_sum = 0.0f;
    for (const Wheel &w : rig.car.wheels) {
        if (fabsf(w.load - 1200.0f * 9.81f / 4.0f) > 0.15f * 1200.0f * 9.81f / 4.0f)
            loads_ok = false;
        load_sum += w.load;
    }
    char buf[224];
    snprintf(buf, sizeof buf,
             "y %.4f m (expected %.4f, dy %.4f), y span %.5f, sum F %.0f N",
             rig.car.body.pos.y, expected_y, dy, y_span, load_sum);
    check("suspension: settles at ride height",
          dy < 0.02f && y_span < 0.001f && loads_ok &&
              fabsf(load_sum - 1200.0f * 9.81f) < 0.15f * 1200.0f * 9.81f,
          buf);
}

int main()
{
    test_engine_curve();
    test_pacejka();
    test_split();
    test_acceleration();
    test_braking();
    test_suspension_settle();

    printf("\n%d check(s) failed\n", failures);
    assert(failures == 0);
    return 0;
}
