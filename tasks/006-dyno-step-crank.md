---
id: 006
status: todo
branch:
depends: [005]
---
# dyno: EngineModel::step_crank (0x4f50e0)

## Goal
Port the per-crank-step cylinder physics `EngineModel__stepCrank` (0x4f50e0) into
`EngineModel::step_crank(int n)` in `src/vehicle/dyno.cpp`. Field names are the
ones of `struct EngineModel` in `src/vehicle/dyno.h` (card 005). Port the
arithmetic in exactly this order with `float` math (`sqrtf`, `sinf`, `cosf`,
`powf`, `fabsf`) — the dyno tables come out of thousands of these steps.

## Exe evidence (cleaned pseudocode, renamed fields)
Locals are cached at entry and written back at the end (the exe really does
this; clamps on `vmin/vtot` therefore persist).
```c
void EngineModel::step_crank(int n) {               // 0x4f50e0
    if (n <= 0) return;
    const float w = omega, dt0 = dt, L = conrod, Tc = tc, r0 = crank_r;
    float V = v, tt = t, th = theta;
    float vmin = v_min, vtot = v_tot;
    float T = temp, b = burn, P = p;
    float X;                                         // written to x at the end
    do {
        const float b_prev = b, P0 = P;
        if (vmin < 0) vmin = 0;
        if (vtot < 0) vtot = 0;
        const float Vprev = V < 0 ? 0 : V;
        tt += dt;                                    // reads the member dt each step
        if (Tc <= tt) {                              // cycle boundary
            cycles += 1;
            tt -= Tc;
            const float vd = vtot - vmin;
            work_last = work;
            work = 0;
            imep = (vd <= 1e-8f) ? 0.0f : work_last / vd;
            torque = cylinders * imep * vd * 0.125f;
            float pi = rpm * imep * vd * 0.016666668f * cylinders;
            p_ind = pi;
            float pf = rpm * rpm * rpm * 2e-08f;
            p_fric = pf;
            p_brake = pi - pf;
            hp = (pi - pf) * 0.001341f;
        }
        th += w * dt0;
        if (12.566371f < th) th -= 12.566371f;       // 4*pi
        const float s = sinf(th), c = cosf(th);      // FUN_00570fcb / FUN_00570e7e
        X = sqrtf(L * L - s * crank_r * s * crank_r) + c * crank_r;
        V = ((r0 + L) - X) * area + vmin;
        if (V < 1e-7f) { V = 1e-7f; continue; }      // rest of the step skipped
        const float invV = 1.0f / V;
        const float q = invV * Vprev;
        P = powf(q, 1.3f) * P;                       // const 0x59ab6c
        T = powf(q, 0.3f) * T;                       // const 0x593704  (same q)

        // valve coefficients (window in fractions of the cycle)
        const float Tc2 = tc;
        float o = in_open * Tc2;
        bool open_in = (in_close <= in_open) ? (o < tt || tt < in_close * Tc2)
                                             : (o < tt && tt < in_close * Tc2);
        a_in = open_in ? in_max : in_min;
        o = out_open * Tc2;
        bool open_out = (out_close <= out_open) ? (o < tt || tt < out_close * Tc2)
                                                : (o < tt && tt < out_close * Tc2);
        a_out = open_out ? out_max : out_min;

        // combustion
        const float ts = spark * Tc2;
        spark_time = ts;
        float te = burn_time + ts;
        if (Tc2 < te) te -= Tc2;
        bool inside = (te <= ts) ? (ts < tt || tt <= te) : (ts < tt && tt <= te);
        bool heat;
        if (inside) { b = (tt - ts) / burn_time; heat = 0.0f < b; }   // a negative b is kept
        else if (0.0f < b_prev) { b = 1.0f; heat = true; }
        else { b = 0.0f; heat = false; }
        if (heat) {
            const float m_old = m;
            if (m < 1e-7f) m = 1e-7f;
            if (T < 1e-7f) T = 1e-7f;
            const float Tn = ((b - b_prev) * h * m_old) / (cv * m) + T;
            P = (Tn / T) * P;
            T = Tn;
            if (1.0f <= b) { b = 0.0f; h = 0.0f; }
        }

        // intake valve
        if (0.0f < a_in) {
            float k = a_in * dt * 1000.0f; if (1.0f <= k) k = 1.0f;
            const float pv = V * P;
            P = k * (p_in - P) + P;
            if (P <= 1e-6f) P = 1e-6f;
            const float veq = pv / P;
            if (V <= veq) {
                m = m - (veq - V) * invV * m;        // outflow
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
        // exhaust valve (no 1e-6 clamp on P, no air/fuel accounting)
        if (0.0f < a_out) {
            float k = a_out * dt * 1000.0f; if (1.0f <= k) k = 1.0f;
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
        float k = dt / t_loss; if (1.0f <= k) k = 1.0f;
        const float d = T - t_wall;
        const float sg = d == 0.0f ? 0.0f : (d < 0.0f ? -1.0f : 1.0f);
        float loss = d * d * sg * k;
        if (1.0f < loss / d) loss = loss / (loss / d);   // d == 0 -> NaN compare false -> loss 0
        const float Tn = T - loss;
        q_loss = q_loss + loss;
        if (T < 1e-7f) T = 1e-7f;
        P = (Tn / T) * P;
        work = (P + P0) * 0.5f * (V - Vprev) + work;
        T = Tn;
    } while (--n != 0);
    v_min = vmin; v_tot = vtot; burn = b; v = V; p = P; temp = T;
    t = tt; theta = th; x = X;
}
```
Notes on the original:
- `continue` in a do/while jumps to the `--n` test — that is the exe's control flow
  (`iVar7--` follows the skipped block).
- `X` is only assigned inside the loop; with `n >= 1` it is always set before the write-back.
- `b_prev` and `P0` are the values at the **start** of the step (`fVar12 = local_8`,
  `fVar6 = param_1` before any update).

## Files
- edit: `src/vehicle/dyno.cpp` — add `EngineModel::step_crank` (`// 0x4f50e0 EngineModel__stepCrank`),
  `#include <math.h>` if missing.
- read only: `src/vehicle/dyno.h`

## Done when
- the function has a `// 0x4f50e0` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
