// EngineModel functions of the dyno cylinder model
// (notes/10-engine-dyno.md). 0x4f44a0 ctor, 0x4f44e0 freeTables,
// 0x4f4560 runCycle, 0x4f4640 buildTables, 0x4f49e0 setDefaults,
// 0x4f4ca0 setupRpmPoint, 0x4f4fc0 resetCycle, 0x4f50e0 stepCrank,
// 0x4f5980 updateMixture, 0x4f59f0 updateGeometry.
#include "vehicle/dyno.h"

#include <math.h>

// 0x4f44a0 EngineModel ctor
EngineModel::EngineModel()
{
    t = 0.0f;
    table_set = 0;
    free_tables();
}

// 0x4f44e0 EngineModel__freeTables
void EngineModel::free_tables()
{
    for (int k = 0; k < 2; k++) {
        torque_table[k].clear();
        torque_table[k].shrink_to_fit();
        power_table[k].clear();
        power_table[k].shrink_to_fit();
    }
}

// 0x4f49e0 EngineModel__setDefaults
void EngineModel::set_defaults()
{
    rho0 = 1.2929f;
    p_amb = 101325.0f;
    p_exh = 111457.5f;
    afr = 14.0f;
    crank_r = 0.053175f;
    bore_r = 0.049f;
    conrod = 0.15f;
    v_min = 1e-4f;
    cylinders = 4.0f;
    time_burn = 0.003f;
    burn_rpm_factor = 5e-5f;
    unused08 = 1.029f;
    unused18 = 1.0f;
    mixture_h = 1e6f;
    h_exh = 0.0f;
    cv = 712.0f;
    t_intake = 293.15f;
    t_exh = 293.15f;
    in_min = 0.005f;
    out_min = 0.005f;
    in_max = 0.8f;
    out_max = 0.8f;
    t_loss = 95.0f;
    t_wall = 353.15f;
    t_wall2 = 353.15f;
    spark = 0.48f;
    in_open = 0.001f;
    in_close = 0.25f;
    out_open = 0.75f;
    out_close = 0.99f;
    spark_min = 0.48f;
    spark_inc = -0.05f;
    spark_rpm0 = 2000.0f;
    spark_rpm1 = 5000.0f;
    in_open_base = 0.001f;
    in_duration = 0.249f;
    in_vvt_adv = 0.0f;
    in_vvt_rpm0 = 0.0f;
    in_vvt_rpm1 = 0.0f;
    out_open_base = 0.75f;
    out_duration = 0.24f;
    out_vvt_adv = 0.0f;
    out_vvt_rpm0 = 0.0f;
    out_vvt_rpm1 = 0.0f;
    t = 0.0f;
    theta = 0.0f;
    rpm = 1000.0f;
    turbo_mul = 20.0f;
    turbo_opt = 85000.0f;
    turbo_range = 30000.0f;
    turbo_band = 0.0f;
    p_turbo_max = 0.0f;
    p_turbo_waste = 3.0f;
    fuel_frac = 1.0f / (afr + 1.0f);
    air_frac = fuel_frac * afr;
    rho_amb = p_amb * 9.869233e-06f * rho0;
    rho_exh = p_exh * 9.869233e-06f * rho0;
    reset_cycle();
    max_p_ind = 0.0f;
    max_p_brake = 0.0f;
    max_imep = 0.0f;
    dt = tc * 0.005f; // tc is unset until the first rpm point (exe reads garbage)
    rpm_max_p_ind = 0.0f;
    rpm_max_p_brake = 0.0f;
    rpm_max_imep = 0.0f;
    max_air = 100.0f;
    max_fuel = 100.0f;
    fuel = 0.0f;
    air = 0.0f;
}

// 0x4f5980 EngineModel__updateMixture
void EngineModel::update_mixture()
{
    fuel_frac = 1.0f / (afr + 1.0f);
    air_frac = fuel_frac * afr;
    rho_amb = p_amb * 9.869233e-06f * rho0;
    rho_exh = p_exh * 9.869233e-06f * rho0;
}

// 0x4f59f0 EngineModel__updateGeometry
void EngineModel::update_geometry()
{
    float R = bore_r;
    float r = crank_r;
    h_in = mixture_h;
    p_in = p_amb;
    float A = R * R * 3.1415927f;
    bore = R + R;
    stroke = r + r;
    float vd = (r + r) * A;
    t_in = t_intake;
    area = A;
    v_disp = vd;
    piston_speed_max = 45.0f;
    rho_in = rho_amb;
    v_tot = vd + v_min;
    compression = (vd + v_min) / v_min;
    max_rpm = (45.0f / r) * 9.549296f;
    piston_speed = omega * r;
    displacement = cylinders * vd;
}

// 0x4f4fc0 EngineModel__resetCycle
void EngineModel::reset_cycle()
{
    float r = crank_r;
    float L = conrod;
    h = h_in;
    p = p_in;
    t = 0.0f;
    theta = 0.0f;
    a_in = 0.0f;
    a_out = 0.0f;
    burn = 0.0f;
    q_loss = 0.0f;
    unused11c = 0.0f;
    work = 0.0f;
    work_last = 0.0f;
    imep = 0.0f;
    p_ind = 0.0f;
    temp = t_in;
    fuel = 0.0f;
    air = 0.0f;
    float xx = sqrtf(L * L - r * 0.0f * r * 0.0f) + r; // sin(0) = 0: piston at TDC
    rho = rho_in;
    x = xx;
    v = ((xx - L) - r) * area + v_min;
    m = v * rho_in;
}

// 0x4f50e0 EngineModel__stepCrank
void EngineModel::step_crank(int n)
{
    if (n <= 0) return;
    // state cached on entry, written back at the end (clamped vmin/vtot persist)
    const float w = omega, dt0 = dt, L = conrod, Tc = tc, r0 = crank_r;
    float V = v, tt = t, th = theta;
    float vmin = v_min, vtot = v_tot;
    float T = temp, b = burn, P = p;
    float X;
    do {
        const float b_prev = b, P0 = P;
        if (vmin < 0.0f) vmin = 0.0f;
        if (vtot < 0.0f) vtot = 0.0f;
        const float Vprev = V < 0.0f ? 0.0f : V;
        tt += dt;
        if (Tc <= tt) { // cycle boundary
            cycles += 1;
            tt -= Tc;
            const float vd = vtot - vmin;
            work_last = work;
            work = 0.0f;
            imep = (vd <= 1e-8f) ? 0.0f : work_last / vd;
            torque = cylinders * ((imep * vd) * 0.125f); // exe product order
            const float pi = ((rpm * (imep * vd)) * 0.016666668f) * cylinders;
            p_ind = pi;
            const float pf = rpm * rpm * rpm * 2e-08f;
            p_fric = pf;
            p_brake = pi - pf;
            hp = (pi - pf) * 0.001341f;
        }
        th += w * dt0;
        if (12.566371f < th) th -= 12.566371f; // 4*pi
        const float s = sinf(th), c = cosf(th);
        X = sqrtf(L * L - (s * crank_r) * (s * crank_r)) + c * crank_r;
        V = ((r0 + L) - X) * area + vmin;
        if (V < 1e-7f) {
            V = 1e-7f;
            continue; // skips the rest of the step, jumps to --n
        }
        const float invV = 1.0f / V;
        const float q = invV * Vprev;
        P = powf(q, 1.3f) * P;
        T = powf(q, 0.3f) * T;

        // valve coefficients: window in fractions of the cycle
        const float Tc2 = tc;
        float o = in_open * Tc2;
        const bool open_in = (in_close <= in_open) ? (o < tt || tt < in_close * Tc2)
                                                   : (o < tt && tt < in_close * Tc2);
        a_in = open_in ? in_max : in_min;
        o = out_open * Tc2;
        const bool open_out = (out_close <= out_open) ? (o < tt || tt < out_close * Tc2)
                                                      : (o < tt && tt < out_close * Tc2);
        a_out = open_out ? out_max : out_min;

        // combustion
        const float ts = spark * Tc2;
        spark_time = ts;
        float te = burn_time + ts;
        if (Tc2 < te) te -= Tc2;
        const bool inside = (te <= ts) ? (ts < tt || tt <= te) : (ts < tt && tt <= te);
        bool heat;
        if (inside) {
            b = (tt - ts) / burn_time;
            heat = 0.0f < b; // a negative b is kept
        } else if (0.0f < b_prev) {
            b = 1.0f;
            heat = true;
        } else {
            b = 0.0f;
            heat = false;
        }
        if (heat) {
            const float m_old = m;
            if (m < 1e-7f) m = 1e-7f;
            if (T < 1e-7f) T = 1e-7f;
            const float Tn = ((b - b_prev) * h * m_old) / (cv * m) + T;
            P = (Tn / T) * P;
            T = Tn;
            if (1.0f <= b) {
                b = 0.0f;
                h = 0.0f;
            }
        }

        // intake valve
        if (0.0f < a_in) {
            float k = a_in * dt * 1000.0f;
            if (1.0f <= k) k = 1.0f;
            const float pv = V * P;
            P = k * (p_in - P) + P;
            if (P <= 1e-6f) P = 1e-6f;
            const float veq = pv / P;
            if (V <= veq) { // outflow
                m = m - (veq - V) * invV * m;
            } else {
                const float dm = (P / p_in) * rho_in * (V - veq);
                if (m < 1e-7f) m = 1e-7f;
                const float mo = m, inv = 1.0f / (mo + dm);
                m = mo + dm;
                T = (t_in * dm + T * mo) * inv;
                air = dm * air_frac + air;
                h = (h_in * dm + h * mo) * inv;
                fuel = fuel_frac * dm + fuel;
            }
            if (m < 0.0f) m = 0.0f;
        }
        // exhaust valve: no 1e-6 clamp on P, no air/fuel accounting
        if (0.0f < a_out) {
            float k = a_out * dt * 1000.0f;
            if (1.0f <= k) k = 1.0f;
            const float pv = V * P;
            P = k * (p_exh - P) + P;
            const float veq = pv / P;
            if (V <= veq) {
                m = m - (veq - V) * invV * m;
            } else {
                const float dm = (P / p_exh) * rho_exh * (V - veq);
                if (m < 1e-7f) m = 1e-7f;
                const float mo = m, inv = 1.0f / (mo + dm);
                m = mo + dm;
                T = (T * mo + dm * t_exh) * inv;
                h = (h_exh * dm + h * mo) * inv;
            }
        }
        if (m < 0.0f) m = 0.0f;

        // wall heat loss
        float k = dt / t_loss;
        if (1.0f <= k) k = 1.0f;
        const float d = T - t_wall;
        const float sg = d == 0.0f ? 0.0f : (d < 0.0f ? -1.0f : 1.0f);
        float loss = d * d * sg * k;
        if (1.0f < loss / d) loss = loss / (loss / d); // d == 0 -> 0/0 is NaN, compare false
        const float Tn = T - loss;
        q_loss = q_loss + loss;
        if (T < 1e-7f) T = 1e-7f;
        P = (Tn / T) * P;
        work = (P + P0) * 0.5f * (V - Vprev) + work;
        T = Tn;
    } while (--n != 0);
    v_min = vmin;
    v_tot = vtot;
    burn = b;
    v = V;
    p = P;
    temp = T;
    t = tt;
    theta = th;
    x = X;
}

// 0x4f4ca0 EngineModel__setupRpmPoint
void EngineModel::setup_rpm_point()
{
    update_mixture();
    // spark: spark_min + spark_inc * min(f, 1), f from the rpm window
    float d = spark_rpm1 - spark_rpm0;
    float f;
    if (d <= 0.0f || (f = (rpm - spark_rpm0) / d) < 0.0f) {
        spark = spark_min;
    } else {
        spark = (f <= 1.0f ? spark_inc * f : spark_inc) + spark_min;
    }
    // intake VVT (constant with the defaults; ported because calcDyno never sets it)
    d = in_vvt_rpm1 - in_vvt_rpm0;
    if (d <= 0.0f || (f = (rpm - in_vvt_rpm0) / d) < 0.0f) {
        in_open = in_open_base;
    } else {
        in_open = (f <= 1.0f ? in_vvt_adv * f : in_vvt_adv) + in_open_base;
    }
    in_close = in_duration + in_open;
    // exhaust VVT
    d = out_vvt_rpm1 - out_vvt_rpm0;
    if (d <= 0.0f || (f = (rpm - out_vvt_rpm0) / d) < 0.0f) {
        out_open = out_open_base;
    } else {
        out_open = (f <= 1.0f ? out_vvt_adv * f : out_vvt_adv) + out_open_base;
    }
    p_in = p_amb;
    out_close = out_duration + out_open;
    h_in = mixture_h;
    t_in = t_intake;
    rho_in = rho_amb;

    // turbo: g falls off outside the full-boost band, boost is a pressure ratio
    if (0.0f < turbo_mul && 0.0f < turbo_range) {
        float g;
        const float x = fabsf(rpm * turbo_mul - turbo_opt);
        if (turbo_band <= x) {
            g = 1.0f - (x - turbo_band) / (turbo_range - turbo_band);
            if (g <= 0.0f) g = 0.0f;
        } else {
            g = 1.0f;
        }
        float bst = p_turbo_max * g;
        if (p_turbo_waste <= bst) bst = p_turbo_waste;
        const float s = sqrtf(bst + 1.0f);
        t_in = t_intake * s;
        rho_in = rho_amb * s;
        p_in = s * s * p_amb;
    }

    // consumption limits use the flow rates of the previous runCycle
    if (0.0f < max_fuel && max_fuel < fuel_rate) h_in = mixture_h / (fuel_rate / max_fuel);
    if (0.0f < max_air && max_air < air_rate) p_in = p_in / (air_rate / max_air);

    // burn duration shrinks with rpm, capped at 0.9
    float k = rpm * burn_rpm_factor;
    if (0.9f <= k) k = 0.9f;
    burn_time = (1.0f - k) * time_burn;
}

// 0x4f5ab0 EngineModel__boostGauge: the turbo g of setupRpmPoint, normalised by
// the wastegate pressure. Caller guarantees turbo_mul, turbo_range, p_turbo_max > 0.
// Denominator is sqrt(P), not sqrt(P+1): the exe quirk.
float EngineModel::boost_gauge(float rpm) const
{
    float g;
    const float x = fabsf(rpm * turbo_mul - turbo_opt);
    if (turbo_band <= x) {
        g = 1.0f - (x - turbo_band) / (turbo_range - turbo_band);
        if (g <= 0.0f) g = 0.0f;
    } else {
        g = 1.0f;
    }
    float bst = p_turbo_max * g;
    if (p_turbo_waste <= bst) bst = p_turbo_waste;
    const float p = p_turbo_max < p_turbo_waste ? p_turbo_max : p_turbo_waste;
    return (sqrtf(bst + 1.0f) - 1.0f) / (sqrtf(p) - 1.0f);
}

// 0x4f4560 EngineModel__runCycle
void EngineModel::run_cycle()
{
    fuel = 0.0f;
    air = 0.0f;
    q_loss = 0.0f;
    unused11c = 0.0f;
    cycles = 0;
    do {
        step_crank(1);
    } while (cycles == 0);

    const float inv = 1.0f / tc;
    const float fr = fuel * inv * cylinders;
    air_rate = air * inv * cylinders;
    fuel_rate = fr;
    bsfc = (fr * 7920.0f) / hp;
    ve = (air + fuel) / (v_disp * rho0);
    q_loss = inv * q_loss;
}

// 0x4f4640 EngineModel__buildTables
void EngineModel::build_tables(int step)
{
    max_p_ind = 0.0f;
    max_p_brake = 0.0f;
    max_imep = 0.0f;
    max_torque = 0.0f;
    max_hp = 0.0f;
    rpm_max_p_ind = 0.0f;
    rpm_max_p_brake = 0.0f;
    rpm_max_imep = 0.0f;
    rpm_max_torque = 0.0f;
    // INVENTED: step == 0 is an integer-divide fault in the exe (nitro pass).
    if (step <= 0) {
        count = 0;
        this->step = step;
        torque_table[table_set].clear();
        power_table[table_set].clear();
        return;
    }
    count = (int)max_rpm / step; // float->int truncation, then int division
    rpm_max_hp = 0.0f;
    this->step = step;
    // exe: Mem__alloc(count*4), uninitialised; zeroed here
    if (0 < count) {
        torque_table[table_set].assign(count, 0.0f);
        power_table[table_set].assign(count, 0.0f);
        torque_table[table_set][0] = 0.0f;
        power_table[table_set][0] = 0.0f;
    } else {
        // INVENTED: exe Mem__alloc(count*4) then writes element 0 out of bounds.
        torque_table[table_set].clear();
        power_table[table_set].clear();
    }
    int rpm_i = step;
    for (int i = 1; i < count; i++, rpm_i += step) {
        const float fr = (float)rpm_i;
        theta = 0.0f;
        rpm = fr;
        tc = 120.0f / fr;
        omega = fr * 0.10471976f;
        t = (120.0f / fr) * 0.0f;
        piston_speed = fr * 0.10471976f * crank_r;
        setup_rpm_point();
        dt = tc * 0.0043290043f; // 1/231 crank steps per cycle
        reset_cycle();
        run_cycle();
        // settle: up to 10 more cycles until imep stops moving by 5 %
        float ref = imep;
        int left = 10;
        do {
            run_cycle();
            if (fabsf(ref / imep - 1.0f) < 0.05f) break;
            left--;
            ref = ref * 0.4f + imep * 0.6f;
        } while (0 < left);
        if (max_p_ind < p_ind) {
            max_p_ind = p_ind;
            rpm_max_p_ind = rpm;
        }
        if (max_p_brake < p_brake) {
            max_p_brake = p_brake;
            rpm_max_p_brake = rpm;
            rpm_max_hp = rpm;
            max_hp = p_brake * 0.001341f;
        }
        if (max_imep < imep) {
            max_imep = imep;
            rpm_max_imep = rpm;
            rpm_max_torque = rpm;
            max_torque = torque;
        }
        torque_table[table_set][i] = torque;
        power_table[table_set][i] = p_brake; // watts
    }
    torque = max_torque;
    p_ind = max_p_ind;
    p_brake = max_p_brake;
}
