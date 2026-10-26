// Vehicle simulation port. Notes/09-vehicle.md §1-§6 is the authoritative
// walk-through of Vehicle::update (0x49c8e0); notes/10-engine-dyno.md defines
// the torque-table lookup. All state is 32-bit float like the exe.
#include "vehicle/vehicle.h"

#include <math.h>

#include "physics/raycast.h"

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// 0x4b0ff0 Vehicle__healthFactor.
float health_factor(float w)
{
    return w >= 1.0f ? 0.100000024f : 1.0f - w * w * 0.6f;
}

// 0x4afb50 Vehicle__damageRoll: deterministic wear accumulation. Returns 0 when
// the amount is not positive, 1 while the wear stays below 1, 2 once it clamps.
int wear_add(float *wear, float rate, float amount)
{
    if (amount <= 0.0f) return 0;
    *wear = rate * amount + *wear;
    if (*wear < 0.0f) *wear = 0.0f; // exe logs "intdamage_data: part with invalid wear detected!"
    if (*wear < 1.0f) return 1;
    *wear = 1.0f;
    return 2;
}

// 0x4aa830/0x566a70: the wheel's sensor-body frame, R_car * W+0x90 * Ry(W+0xe4).
// The exe composes the same matrices with 0x566a70; 0x49c8e0 then reads rows
// (local_j = sum_r M[r][j] v_r), which is mat_tvec below.
static Mat34 wheel_frame(const Mat34 &r, const Wheel &w)
{
    float ang[3] = {w.steer_angle, 0.0f, 0.0f};
    Mat34 ry, rb, out;
    euler_to_mat(&ry, ang);
    mat_mul(&rb, &r, &w.base_rot);
    mat_mul(&out, &rb, &ry);
    return out;
}

// Engine__torqueCurve 0x4a2dd0: 0 below the first point or past the last,
// else lerp inside the first segment whose upper rpm is > q.
static float sample_curve(const std::vector<TorquePoint> &pts, float rpm)
{
    size_t n = pts.size();
    if (n <= 1 || rpm < pts[0].rpm)
        return 0.0f;
    size_t i = 1;
    while (pts[i].rpm <= rpm) {
        if (++i >= n)
            return 0.0f;
    }
    const TorquePoint &a = pts[i - 1];
    const TorquePoint &b = pts[i];
    return a.value + (b.value - a.value) * (rpm - a.rpm) / (b.rpm - a.rpm);
}

float Engine::torque_at(float rpm) const { return sample_curve(torque_curve, rpm); }
float Engine::power_at(float rpm) const { return sample_curve(power_curve, rpm); }

// Engine__frictionTorque 0x4a1510, with the rpm->rad/s conversion folded out.
float Engine::friction_torque(float omega_rad_s) const
{
    if (omega_rad_s > 0.0f) {
        float f = -omega_rad_s * friction_fwd * max_rpm * 0.05f;
        float cap = -0.5f * inertia * omega_rad_s;
        return f > cap ? f : cap;
    }
    if (omega_rad_s < 0.0f) {
        float f = omega_rad_s * omega_rad_s * friction_rev;
        float cap = -0.5f * inertia * omega_rad_s;
        return f < cap ? f : cap;
    }
    return 0.0f;
}

TyreSample tyre_magic(const Pacejka &pj, float normal_load, float camber, float kappa,
                      float alpha, float mu)
{
    TyreSample t;
    if (normal_load <= 0.0f)
        return t;

    const float pi = 3.14159265358979323846f;
    float p14 = pj.p[14], p11 = pj.p[11];
    float soft = normal_load;
    if (p11 > 1e-6f)
        soft = p14 * p11 * (2.0f / pi) * atanf((pi * 0.5f) * normal_load / p11) +
               (1.0f - p14) * (normal_load < p11 ? normal_load : p11);
    float fz = camber * soft;
    t.load = fz;

    float p4 = pj.p[4];
    if (pj.p[16] > 0.0f)
        p4 = p4 * (pj.p[15] + pj.p[16]) / (pj.p[16] + fz);

    float sx = sinf(alpha);
    float sz = cosf(alpha) * pj.p[12] * kappa;
    float sigma = sqrtf(sx * sx + sz * sz);
    t.sigma = sigma;
    if (sigma <= 1e-6f)
        return t;

    float D = pj.p[1] * fz + mu;
    float CD = pj.p[0] * D;
    if (CD == 0.0f)
        return t;

    float B = (pj.p[3] * fz + p4) * expf(-pj.p[5] * fz) / CD;
    float x = sigma + pj.p[9] * fz + pj.p[10];
    float E = pj.p[6] * fz * fz + pj.p[7] * fz + pj.p[8];
    float bx = B * x;
    float f = fz * D * sinf(pj.p[0] * atanf(bx + E * (atanf(bx) - bx)));
    if (f < 0.0f)
        f = 0.0f;
    t.force = f;
    t.f_lat = f * sx / sigma;
    t.f_long = f * sz / sigma;

    // Aligning torque, fixed constants 0x5da8f8..0x5da920 (notes/09).
    const float ba = 15.2f / (-2.3f * 1.49f);
    float ba_alpha = ba * alpha;
    t.mz = mu * fz * 1.49f * sinf(-2.3f * atanf(2.0f * ba_alpha - atanf(ba_alpha)));
    return t;
}

float Car::split_drive(float shaft_value, float *wheel_value) const
{
    float D = 0.0f;
    for (const Wheel &w : wheels)
        D += fabsf(w.drive_share);
    for (size_t i = 0; i < wheels.size(); i++) {
        const Wheel &w = wheels[i];
        float v = 0.0f;
        if (D > 1e-4f && w.drive_share > 1e-4f && fabsf(w.drive_ratio) > 1e-6f)
            v = (shaft_value / D) * w.drive_share * (-1.0f / w.drive_ratio);
        wheel_value[i] = v;
    }
    return D;
}

void Car::update(float dt)
{
    if (dt < 1e-4f)
        return;
    pending_.clear();
    update_suspension(dt);
    update_engine_and_drivetrain(dt);
    update_tyre_forces(dt);
    apply_contact_forces();
    update_drag(dt);
}

// §0-§5 suspension solver and the spring force it applies (notes/09).
void Car::update_suspension(float dt)
{
    for (Wheel &w : wheels) {
        w.contact = false;
        // ray_len keeps the previous tick's S+0xf0 (the 0x4f3d70 gate input).
        w.load = 0.0f;
        w.susp_velocity = 0.0f;
        w.normal_world = Vec3{0.0f, 1.0f, 0.0f};
        w.ground_velocity = Vec3{0.0f, 0.0f, 0.0f};
        w.ground_friction = 0.0f;
        w.ground_rolling_resistance = 0.0f;
        // 0x4aa830: max_steer * steer, then Ackermann from e+0xa30.
        float d0 = max_steer * steer;
        if (fabsf(d0) <= 0.001f) {
            w.steer_angle = 0.0f;
        } else if (ackermann == 0.0f) {
            w.steer_angle = w.steer_factor * d0;
        } else {
            float R = ackermann / (sinf(d0) / cosf(d0));
            if (R > 0.0f)
                w.steer_angle = atan2f(ackermann, w.mount.x + R) * w.steer_factor;
            else
                w.steer_angle = -atan2f(ackermann, -R - w.mount.x) * w.steer_factor;
        }
    }
    if (!ground)
        return;

    Mat34 r = body.rotation();
    Vec3 up = mat_vec(r, Vec3{0.0f, 1.0f, 0.0f});

    for (Wheel &w : wheels) {
        Vec3 origin = body.pos + mat_vec(r, w.mount);
        RayHit hit;
        float len = 0.0f;
        const CollisionMesh *cm = ground->def.shape.type == SHAPE_MESH
                                      ? ground->def.shape.mesh.get()
                                      : nullptr;
        if (cm && !cm->face_normals.empty()) {
            // 0x4fd110 crossings of the sensor segment 0x4f3f10 (mount - L .. mount)
            // -> 0x4f3d70: nearest crossing no more than r above the previous
            // length; its stored PHYS face normal is the contact normal.
            // Hit on the previous step's final pose = the pose 0x4fc610's collide
            // pass saw (notes/08 H); INVENTED: differs only after an in-loop
            // resolveImpacts, and equal d2 ties follow the AABB tree rather than
            // the exe's node face order.
            std::vector<SegmentCross> hits;
            mesh_segment_cross(*cm, ground->pose(), origin - up * w.sensor_len, origin,
                               &hits);
            float best = w.max_ray() * w.max_ray() + 0.1f;
            const SegmentCross *best_hit = nullptr;
            for (const SegmentCross &h : hits) {
                Vec3 d = origin - h.point;
                float d2 = vec_dot(d, d);
                if (w.ray_len - sqrtf(d2) > w.radius) {
                    w.gate_rejects++;
                    continue;
                }
                if (d2 < best) {
                    best = d2;
                    best_hit = &h;
                }
            }
            if (w.max_ray() * w.max_ray() > best) {
                len = fminf(sqrtf(best), w.max_ray());
                hit.point = best_hit->point;
                hit.normal = best_hit->normal;
            } else {
                w.ray_len = w.max_ray(); // miss: the next gate starts at max_ray
                continue;
            }
        } else if (!raycast_body(*ground, origin, -up, w.max_ray(), &hit)) {
            w.ray_len = w.max_ray();
            continue;
        } else {
            len = hit.distance;
        }

        w.contact = true;
        w.ray_len = len;
        w.normal_world = hit.normal;
        w.ground_friction = ground->def.shape.friction;
        w.ground_rolling_resistance = ground->def.shape.rolling_resistance;
        w.contact_local = Vec3{w.mount.x, w.mount.y - len, w.mount.z};

        Mat34 gr = ground->rotation();
        Vec3 gp_local = mat_tvec(gr, hit.point - ground->pos);
        w.ground_velocity = ground->point_velocity(gp_local);

        float v = vec_dot(body.point_velocity(w.contact_local) - w.ground_velocity, up);
        w.susp_velocity = v;

        if (vec_dot(up, hit.normal) < 0.0f)
            continue;

        float c = 2.0f * v >= 0.0f ? w.rebound : w.bump;
        float load = w.spring * (w.rest_ray() - len) - c * 2.0f * v;
        if (load < 0.0f)
            load = 0.0f;

        if (v < 0.0f && len < w.bump_ray()) {
            Vec3 rl = mat_vec(r, w.contact_local);
            Vec3 rn = vec_cross(rl, up);
            float k_eff = body.def.inv_mass +
                          vec_dot(rn, mat_vec(body.world_inv_inertia(), rn));
            if (k_eff > 1e-8f)
                load += (-v / k_eff) / dt;
        }
        w.load = load;
        if (load <= 0.0f)
            continue;

        Vec3 f = up * load;
        // Gram-Schmidt: no force along the ground-plane projection of forward.
        Vec3 fwd = mat_vec(wheel_frame(r, w), Vec3{0.0f, 0.0f, 1.0f});
        Vec3 fp = fwd - hit.normal * vec_dot(fwd, hit.normal);
        float fl = vec_length(fp);
        if (fl > 1e-8f) {
            fp = fp * (1.0f / fl);
            f -= fp * vec_dot(f, fp);
        }

        Vec3 point = w.contact_local;
        if (w.patch_angle > 1e-8f) {
            Vec3 n_local = mat_tvec(r, hit.normal);
            float t = clampf(n_local.x / w.patch_angle, -1.0f, 1.0f);
            point.x += w.patch_offset - w.patch_halfwidth * t;
        }
        pending_.push_back(PendingForce{f, point});
    }
}

// §1 engine + §2 drivetrain set-up + §3 clutch coupling + §4 brakes + §5 spin.
void Car::update_engine_and_drivetrain(float dt)
{
    float T_net = 0.0f;
    if (!engine.running) {
        engine.omega = 0.0f;
        engine.smoothed_omega = 0.0f;
        engine.cranking = false;
        combustion_torque = 0.0f;
    } else {
        float rpm = engine.omega * 9.549296f;
        float thr = fabsf(throttle);
        float T_max = 0.0f;
        if (fabsf(rpm) > 0.001f) {
            float T_f = engine.friction_torque(engine.omega);
            T_max = engine.torque_at(rpm) * engine.torque_health;
            T_net = T_f;
            if (engine.omega > 0.0f && engine.omega < engine.idle && T_max > 0.001f)
                thr = fminf(thr + fabsf(1.1f * T_f) / T_max, 1.0f);
        }

        if (thr > 0.1f && engine.omega <= 0.95f * engine.idle) {
            engine.cranking = true;
            T_net += engine.starter_torque;
            thr = 0.0f;
        } else {
            engine.cranking = false;
        }

        if (fabsf(rpm) > 0.001f) {
            float lim = engine.rpm_limit;
            float nxt = 2.0f * rpm - last_rpm;
            float L = 1.0f;
            if (nxt <= lim) {
                if (lim < rpm) {
                    float den = rpm - nxt;
                    L = fabsf(den) > 1e-6f ? (lim - nxt) / den : 0.0f;
                }
            } else if (rpm <= lim) {
                float den = nxt - rpm;
                L = fabsf(den) > 1e-6f ? (lim - rpm) / den : 0.0f;
            } else {
                L = 0.0f;
            }
            last_rpm = rpm;
            float T = L * T_max;
            combustion_torque = thr > 0.01f ? 2.0f * T * thr : 0.0f;
            T_net += combustion_torque;

            // 0x49cfd0..0x49d2a5 engine heat, radiator overheat and engine wear
            // (notes/34 §3). thr is the post-governor/post-starter value.
            // shortcut: nitro heat (heat += 2000*ΔT) has no sim path yet.
            if (!wear_off) {
                float heat = thr * 1000.0f * T_max;
                if (heat * dt > 0.0f)
                    engine_temp = heat * dt / heat_capacity + engine_temp;
                if (engine_temp <= 90.0f || radiator_dmg >= 1.0f) {
                    cooling_coeff = cool_low;                       // e+0xa5c
                } else {
                    Vec3 v = body.vel;                              // state +0x70
                    cooling_coeff = health_factor(radiator_dmg) *
                                        (vec_dot(v, v) * cool_speed + cool_air) + // e+0xa64/a60
                                    cool_low;
                }
                cooling_coeff = cooling_coeff / heat_capacity;      // +0x1ec8
                if (engine_temp > 100.0f) {                          // 0x49d071
                    float d = engine_temp - 100.0f;
                    wear_add(&radiator_dmg, radiator_rate, d * d * dt * k_dmg);
                }
                if (engine_dmg < 1.0f) {                             // 0x1e9c
                    float fric = rpm < 0.0f ? engine.friction_rev : engine.friction_fwd;
                    float r = fabsf(rpm) / engine.max_rpm;
                    r = r * r;
                    r = r * r;
                    r = r * r;
                    r = r * r;                                       // (|rpm|/maxRPM)^16
                    float t = fabsf(engine_temp - 90.0f) * 0.01f;
                    float p = ((dt + dt) / engine_rate) * r * fric * 100.0f * (t * t + 1.0f);
                    if (p > 0.0f &&
                        wear_add(&engine_dmg, engine_rate, k_dmg * p) != 0) { // 0x49d169
                        float h = health_factor(engine_dmg);
                        engine.torque_health = torque_base * h;      // +0x1e04
                        engine.nitro_health = nitro_base * h;        // +0x1e08
                        if (engine_dmg >= 1.0f) {
                            engine.torque_health *= 0.5f;
                            engine.nitro_health *= 0.5f;
                            engine.friction_fwd *= 50.0f;
                            engine.friction_rev *= 50.0f;
                            // shortcut: event M+0x1f4c (engine blown sound).
                        }
                    }
                }
            }
        } else {
            combustion_torque = 0.0f;
        }

        // Always while the engine runs, after the |rpm| block: Newton cooling.
        float d = engine_temp - engine_ambient;                      // 0x1ec4
        engine_temp = engine_temp - fabsf(d) * cooling_coeff * dt * d;
    }

    // §2: effective wheel inertia including the engaged drivetrain.
    float G = total_ratio();
    float D = 0.0f;
    for (const Wheel &w : wheels)
        D += fabsf(w.drive_share);
    float engagement = 1.0f - clampf(clutch, 0.0f, 1.0f);
    for (Wheel &w : wheels) {
        float I = w.spin_inertia;
        if (D > 1e-4f && w.drive_share > 1e-4f && fabsf(G) > 1e-6f)
            I += fabsf(G) * (w.drive_share / D) * engine.inertia * engagement;
        w.inv_inertia_eff = I > 1e-6f ? 0.1f / I : 0.0f;
    }

    // Wheel-average shaft speed before the differential step.
    float num = 0.0f, den = 0.0f;
    for (const Wheel &w : wheels) {
        if (w.drive_share > 0.001f && fabsf(w.drive_ratio) > 0.001f) {
            num += -w.spin * w.drive_ratio * w.drive_share;
            den += fabsf(w.drive_share);
        }
    }
    shaft_speed = den > 1e-4f ? num / den : 0.0f;

    // Viscous differential.
    float lock = diff_lock;
    for (Wheel &w : wheels) {
        if (w.drive_share <= 0.001f || fabsf(w.drive_ratio) <= 0.001f)
            continue;
        float target = (-1.0f / w.drive_ratio) * shaft_speed;
        float delta = target - w.spin;
        float step = fminf(fabsf(delta) * 5.0f * lock * dt, 1.0f) * delta;
        w.spin += step;
        w.slip_velocity.z += w.radius * step;
    }

    // Contact slip velocity (ground velocity minus chassis point velocity).
    Mat34 r = body.rotation();
    for (Wheel &w : wheels) {
        if (!w.contact) {
            w.slip_velocity = Vec3{0.0f, 0.0f, 0.0f};
            continue;
        }
        Vec3 rel = w.ground_velocity - body.point_velocity(w.contact_local);
        w.slip_velocity = mat_tvec(wheel_frame(r, w), rel);
        w.slip_velocity.z += w.spin * w.radius;
    }

    // §3 clutch / gearbox coupling.
    float shaft_old = shaft_speed;
    if (!engine.running || fabsf(G) < 1e-6f) {
        engine.smoothed_omega = engine.omega;
        clutch_slip = 0.0f;
        net_torque = T_net;
        if (engine.inertia > 1e-6f)
            engine.omega += T_net / engine.inertia;
    } else {
        float a = 1.0f / engine.inertia;
        float k = 1.0f - clampf(clutch, 0.0f, 1.0f);
        float g2 = G * G;
        float reflected = engine.shaft_inertia / g2;
        float w = reflected / (engine_inertia_factor * engine.inertia + reflected);
        engine.smoothed_omega +=
            ((engine.omega - (1.0f - w) * T_net * a) - engine.smoothed_omega) * 0.25f;
        float wd = shaft_old * G;
        float wfree = engine.omega + T_net * a;
        float delta = (wd - wfree) * w;
        float wsync = wfree + delta;
        float wd_new = wd + (wsync - wd) * k * 0.999f;
        float we_new = wfree + (wsync - wfree) * k * 0.999f;
        engine.omega = we_new;
        shaft_speed = wd_new / G;
        clutch_slip = w > 1e-6f ? 10.0f * k * delta / (w * w) : 0.0f;

        // §3 transmission/clutch wear, 0x49e1bf..0x49e2f6 (right after
        // clutch_slip, W+0x1ea4/0x1ea8 and W+0x1eb4/0x1eb8).
        if (!wear_off) {
            float x = fabsf((wd_new - we_new) * k * 0.1f) * k_dmg;
            if (x > 0.0f) wear_add(&trans_wear, trans_rate, x);
            float y = fabsf(clutch_slip) * 0.01f;
            y = y * y;
            y = y * y * dt * 0.0001f * k_dmg; // (|slip|*0.01)^4 * dt * 1e-4 * K
            if (y > 0.0f) wear_add(&clutch_wear, clutch_rate, y);
        }

        // Per-wheel shaft correction, ((new-old)/D)*share*(-1/ratio).
        if (D > 1e-4f) {
            float ds = shaft_speed - shaft_old;
            for (Wheel &w2 : wheels) {
                if (w2.drive_share <= 1e-4f || fabsf(w2.drive_ratio) <= 1e-6f)
                    continue;
                float d = (ds / D) * w2.drive_share * (-1.0f / w2.drive_ratio);
                w2.spin += d;
                w2.slip_velocity.z += w2.radius * d;
            }
        }
    }

    // §4 brakes (torque is an angular impulse per tick, no dt in the gain).
    // Heat/wear/failure and cooling per wheel: 0x49e5xx..0x49e8de.
    float brk = fabsf(brake);
    float hbrk = fabsf(handbrake);
    for (Wheel &w : wheels) {
        if (fabsf(w.spin) <= 0.001f)
            continue;
        float T_b = (w.extra_brake + brk) * brake_torque_factor * w.brake_eff +
                    handbrake_torque_factor * hbrk * w.handbrake_eff + w.bearing;
        if (T_b > 0.0f) {
            float dw = w.inv_inertia_eff * T_b * wheel_brake_factor * dt * 50.0f;
            float old = w.spin;
            if (fabsf(w.spin) <= dw) {
                w.spin = 0.0f;
                w.slip_velocity.z -= w.radius * old;
            } else {
                float d = w.spin > 0.0f ? dw : -dw;
                w.spin -= d;
                w.slip_velocity.z -= w.radius * d;
            }
            if (wear_off) continue; // goto next wheel: no heat/wear, no cooling
            float q = fabsf(w.spin) * (T_b - w.bearing) * 10.0f * 0.001f * dt * 50.0f;
            if (q > 0.0f) {
                w.brake_temp = q / w.brake_heat_cap + w.brake_temp; // W+0x188
                if (w.brake_wear < 1.0f && q * k_dmg > 0.0f) {
                    wear_add(&w.brake_wear, w.brake_rate, q * k_dmg); // W+0x164
                    float h = health_factor(w.brake_wear);
                    w.brake_eff = w.brake * h;          // W+0xd8
                    w.handbrake_eff = w.handbrake * h;  // W+0xdc
                    if (w.brake_wear >= 1.0f) {
                        w.bearing += 100.0f;
                        w.brake_eff *= 0.2f;
                        w.handbrake_eff *= 0.2f;
                        // shortcut: event M+0x1f6c (brake failure sound) not posted.
                    }
                }
            }
        }
        if (!wear_off) {
            float d = w.brake_temp - w.brake_ambient;               // W+0x18c
            w.brake_temp -= fabsf(d) * w.brake_cool * dt * d;        // W+0x190
        }
    }

    // §5 wheel spin angle, wrapped to [-pi, pi].
    const float pi = 3.14159265358979323846f;
    for (Wheel &w : wheels) {
        if (fabsf(w.spin) <= 0.001f)
            continue;
        w.spin_angle += w.spin * dt;
        w.spin_angle = fmodf(w.spin_angle, 2.0f * pi);
        if (w.spin_angle > pi)
            w.spin_angle -= 2.0f * pi;
        else if (w.spin_angle < -pi)
            w.spin_angle += 2.0f * pi;
    }
}

// §6 Pacejka force pass and force application points.
void Car::update_tyre_forces(float dt)
{
    Mat34 r = body.rotation();
    for (Wheel &w : wheels) {
        if (!w.contact || w.load <= 0.0f || w.ground_friction == 0.0f) {
            w.kappa_prev = 0.0f;
            w.alpha_prev = 0.0f;
            w.sigma = 0.0f;
            w.mz = 0.0f;
            continue;
        }

        Mat34 rw = wheel_frame(r, w);
        Vec3 rel = w.ground_velocity - body.point_velocity(w.contact_local);
        Vec3 v = mat_tvec(rw, rel);
        float vz = v.z;
        float vx = w.slip_velocity.x;
        float avz = fabsf(vz);

        float kappa = 0.0f, alpha = 0.0f;
        if (avz > 0.1f) {
            kappa = (-w.spin * w.radius - vz) / avz;
            alpha = atanf(vx / avz);
        }

        float b = clampf((avz - 0.1f) * 0.1f, 0.0f, 1.0f);
        float kprev = w.kappa_prev;
        if (b < 1.0f) {
            float kl = ((-w.spin * w.radius - vz) * dt + 0.3f * kprev) /
                       ((dt * 50.0f * 0.1f + 1.0f) * 0.3f);
            float al = (0.5f * w.alpha_prev + vx * dt) /
                       ((dt * 50.0f * 0.2f + 1.0f) * 0.5f);
            kl = clampf(kl, -1.0f, 1.0f);
            al = clampf(al, -1.0f, 1.0f);
            kappa = b * kappa + (1.0f - b) * kl;
            alpha = b * alpha + (1.0f - b) * al;
        }

        float sx = sinf(alpha);
        float sz = cosf(alpha) * w.pacejka.p[12] * kappa;
        float sigma = sqrtf(sx * sx + sz * sz);
        if (sigma <= 1e-4f) {
            w.sigma = sigma;
            w.kappa_prev = kappa;
            w.alpha_prev = alpha;
            w.mz = 0.0f;
            continue;
        }

        Vec3 n_body = mat_tvec(rw, w.normal_world);
        float p2 = w.ground_friction * w.friction_eff;
        float p13 = w.ground_rolling_resistance + w.rolling_resistance;
        w.pacejka.p[2] = p2;
        w.pacejka.p[13] = p13;

        float camber = (sqrtf(n_body.y * n_body.y + n_body.z * n_body.z) + 1.0f) * 0.5f;
        TyreSample ts = tyre_magic(w.pacejka, w.load, camber, kappa, alpha, p2);

        w.sigma = ts.sigma;
        w.alpha_prev = alpha;
        w.kappa_prev = kappa;

        float dw = ts.f_long * w.radius * w.inv_inertia_eff * dt * 50.0f *
                   wheel_gndfeedback_factor;

        // Overshoot guard: do not let the feedback flip the slip-ratio sign.
        float kappa2 = 0.0f;
        if (avz > 0.1f) {
            float vs2 = -(w.spin + dw) * w.radius;
            float kraw = (vs2 - vz) / avz;
            kappa2 = kraw;
            if (b < 1.0f) {
                float kl = ((vs2 - vz) * dt + 0.3f * kprev) /
                           ((dt * 50.0f * 0.1f + 1.0f) * 0.3f);
                kappa2 = b * kraw + (1.0f - b) * clampf(kl, -1.0f, 1.0f);
            }
        }
        if (kappa2 * kappa < 0.0f) {
            float den = kappa2 - kappa;
            float s = fabsf(den) > 1e-9f ? fabsf(kappa / den) : 0.0f;
            dw *= s;
            ts.f_long *= s;
        }
        w.spin += dw;
        w.slip_velocity.z += dw * w.radius;
        w.mz = ts.mz;

        Vec3 a = vec_normalize(vec_cross(n_body, Vec3{0.0f, 0.0f, 1.0f}),
                               Vec3{1.0f, 0.0f, 0.0f});
        Vec3 bb = vec_normalize(vec_cross(a, n_body), Vec3{0.0f, 0.0f, 1.0f});
        Vec3 f_body = a * ts.f_lat + bb * (vz * ts.load * p13 * 0.01f - ts.f_long);
        Vec3 f_world = mat_vec(rw, f_body);

        Vec3 point = w.contact_local;
        if (w.patch_angle > 1e-8f) {
            float t = clampf(n_body.x / w.patch_angle, -1.0f, 1.0f);
            point.x += w.patch_offset - w.patch_halfwidth * t;
        }
        pending_.push_back(PendingForce{f_world, point});
    }
}

void Car::apply_contact_forces()
{
    for (const PendingForce &p : pending_)
        body.apply_force_at(p.force, p.point); // 0x504e70 / 0x5051e0
    pending_.clear();
}

// Vehicle::update §7, 0x49fb34..0x49ff40. The force is the plain coefficient
// form F = -C_drag*|v|^2*v_hat at drag_center (no frontal area, no air
// density), capped so the drag impulse cannot reverse the point's velocity.
// drag_magnitude (car+0x2248) is zeroed on every call, before the gates.
void Car::update_drag(float dt)
{
    drag_magnitude = 0.0f;
    drag_velocity = Vec3{0.0f, 0.0f, 0.0f};
    drag_force = Vec3{0.0f, 0.0f, 0.0f};
    drag_m_eff = 0.0f;
    drag_dt = dt;
    drag_clamped = false;
    if (!(c_drag > 0.0f)) return;

    Vec3 v = body.point_velocity(drag_center); // 0x5062e0, root branch
    float s2 = vec_dot(v, v);
    if (!(s2 > 1.0f)) return;
    float s = sqrtf(s2);
    Vec3 vh = v * (1.0f / s);
    Vec3 f = vh * -(s2 * c_drag);
    Vec3 d = mat_vec(body.rotation(), drag_center); // 0x5061f0
    Vec3 w = mat_vec(body.world_inv_inertia(), vec_cross(d, vh));
    float m_eff = 1.0f / (vec_dot(vh, vec_cross(w, d)) + body.def.inv_mass);
    Vec3 p_imp = v * m_eff;
    Vec3 h = p_imp * 0.5f;
    Vec3 q = f * dt + h;
    if (vec_dot(q, q) > vec_dot(p_imp, p_imp) * 0.25f) {
        float ql = sqrtf(vec_dot(q, q));
        float hl = sqrtf(vec_dot(p_imp, p_imp) * 0.25f);
        f = (q * (hl / ql) - h) * (1.0f / dt);
        drag_clamped = true;
    }
    drag_velocity = v;
    drag_force = f;
    drag_m_eff = m_eff;
    drag_magnitude = vec_length(f);
    body.apply_force_at(f, drag_center); // 0x5051e0
    body.integrated = false;             // 0x49ff39: other state flags bit0
}
