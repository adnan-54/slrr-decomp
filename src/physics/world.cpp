// Rigid-body integration, force model and contact solver (notes/08-physics.md,
// notes/11-collision-shapes.md §5). The exe keeps two state buffers and reruns
// the RK4 integration from the start-of-step state on every sub-iteration; the
// port reproduces that: solver forces are consumed by the next integration and
// the last pass leaves the accepted velocities in place.
#include <math.h>

#include "physics/physics.h"

void compute_mass_props(BodyDef *def, float mass)
{
    def->inv_mass = mass > 0.0f ? 1.0f / mass : 0.0f;
    def->inv_inertia = Mat34{};
    def->linear_drag = mass > 0.0f ? -0.005f * mass : 0.0f;  // +0x98
    def->angular_drag = mass > 0.0f ? -0.01f * mass : 0.0f;  // +0x9c
    const Shape &s = def->shape;
    switch (s.type) {
    case SHAPE_SPHERE: {
        // RigidBody__computeMassProps 0x505760: invI = 2.5 / (r^2 m)
        float k = mass > 0.0f ? 2.5f / (s.radius * s.radius * mass) : 0.0f;
        def->inv_inertia.m[0] = def->inv_inertia.m[5] = def->inv_inertia.m[10] = k;
        break;
    }
    case SHAPE_CAPSULE_X:
    case SHAPE_CAPSULE_Y:
    case SHAPE_CAPSULE_Z: {
        int axis = s.type - SHAPE_CAPSULE_X;
        float length = 2.0f * s.half_length;
        float inv_axis = mass > 0.0f ? 2.0f / (s.radius * s.radius * mass) : 0.0f;
        float inv_perp = mass > 0.0f ? 1.0f / ((length * length / 12.0f +
                                               s.radius * s.radius / 4.0f) * mass)
                                     : 0.0f;
        for (int i = 0; i < 3; i++)
            def->inv_inertia.m[i * 5] = i == axis ? inv_axis : inv_perp;
        break;
    }
    case SHAPE_BOX: {
        float hx = s.half_extents.x, hy = s.half_extents.y, hz = s.half_extents.z;
        if (mass > 0.0f) {
            def->inv_inertia.m[0] = 3.0f / ((hy * hy + hz * hz) * mass);
            def->inv_inertia.m[5] = 3.0f / ((hx * hx + hz * hz) * mass);
            def->inv_inertia.m[10] = 3.0f / ((hx * hx + hy * hy) * mass);
        }
        break;
    }
    default:
        // TODO(notes/11 §1): mesh mass props need Poly__massProps (Mirtich
        // volume integrals, 0x508d20); no shape geometry is ported yet.
        break;
    }
}

Mat34 RigidBody::rotation() const
{
    return pose_rotation(Pose{pos, rot});
}

Mat34 RigidBody::world_inv_inertia() const
{
    Mat34 r = rotation();
    Mat34 rt;
    mat_transpose(&rt, &r);
    Mat34 tmp;
    mat_mul(&tmp, &r, &def.inv_inertia);
    Mat34 out;
    mat_mul(&out, &tmp, &rt);
    return out;
}

Vec3 RigidBody::angular_velocity() const
{
    return mat_vec(world_inv_inertia(), angular_momentum);
}

Vec3 RigidBody::point_velocity(const Vec3 &p_local) const
{
    Vec3 r = mat_vec(rotation(), p_local);
    return vel + vec_cross(angular_velocity(), r);
}

void RigidBody::apply_impulse_at(const Vec3 &impulse, const Vec3 &p_local)
{
    if (def.inv_mass <= 0.0f)
        return;
    vel += impulse * def.inv_mass;
    Vec3 r = mat_vec(rotation(), p_local);
    angular_momentum += vec_cross(r, impulse);
}

void RigidBody::apply_force_at(const Vec3 &f, const Vec3 &p_local)
{
    if (def.inv_mass <= 0.0f)
        return;
    force += f;
    Vec3 r = mat_vec(rotation(), p_local);
    torque += vec_cross(r, f);
}

void RigidBody::set_sphere(float radius, float mass)
{
    def = BodyDef{};
    def.shape.type = SHAPE_SPHERE;
    def.shape.radius = radius;
    compute_mass_props(&def, mass);
}

void RigidBody::set_box(const Vec3 &half_extents, float mass)
{
    def = BodyDef{};
    def.shape.type = SHAPE_BOX;
    def.shape.half_extents = half_extents;
    compute_mass_props(&def, mass);
}

void RigidBody::set_capsule(int axis, float half_length, float radius, float mass)
{
    def = BodyDef{};
    def.shape.type = SHAPE_CAPSULE_X + (axis < 0 ? 0 : (axis > 2 ? 2 : axis));
    def.shape.half_length = half_length;
    def.shape.radius = radius;
    compute_mass_props(&def, mass);
}

void RigidBody::set_static_shape(const Shape &s)
{
    def = BodyDef{};
    def.shape = s;
}

void World::add_body(RigidBody *b)
{
    bodies.push_back(b);
}

// ---- RK4 integration (notes/08 §Integrator, FUN_00506530) ----

struct BodyState {
    Vec3 pos, vel, momentum;
    Quat rot;
};

// Step-start state for one root body. RK4 restarts from it on every
// sub-iteration; it stays immutable for the whole step. Solver impulses are
// accumulated separately and replayed as an initial-velocity offset, so the
// last working state (start + impulses + gravity) is the true end-of-step
// state carried into the next step.
struct StepState {
    RigidBody *body;
    BodyState start;
    Vec3 impulse{0.0f, 0.0f, 0.0f};          // accumulated linear impulse
    Vec3 impulse_momentum{0.0f, 0.0f, 0.0f}; // accumulated r x impulse
};

static BodyState evaluate(const RigidBody &b, const BodyState &s, Vec3 force, Vec3 torque)
{
    Quat q = s.rot;
    quat_normalize(&q);
    Mat34 r;
    quat_to_mat(&r, &q);
    Mat34 rt;
    mat_transpose(&rt, &r);
    Mat34 tmp, iw;
    mat_mul(&tmp, &r, &b.def.inv_inertia);
    mat_mul(&iw, &tmp, &rt);
    Vec3 omega = mat_vec(iw, s.momentum);
    BodyState d;
    d.pos = s.vel;
    d.vel = force * b.def.inv_mass;
    d.momentum = torque;
    Quat wq{omega.x, omega.y, omega.z, 0.0f};
    Quat dq;
    quat_mul(&dq, &wq, &s.rot);
    d.rot = Quat{dq.x * 0.5f, dq.y * 0.5f, dq.z * 0.5f, dq.w * 0.5f};
    return d;
}

static BodyState stepped(const BodyState &s, const BodyState &k, float h)
{
    BodyState out;
    out.pos = s.pos + k.pos * h;
    out.vel = s.vel + k.vel * h;
    out.momentum = s.momentum + k.momentum * h;
    out.rot = Quat{s.rot.x + k.rot.x * h, s.rot.y + k.rot.y * h,
                   s.rot.z + k.rot.z * h, s.rot.w + k.rot.w * h};
    return out;
}

static void integrate_body(RigidBody &b, const BodyState &start, float h)
{
    if (b.def.inv_mass <= 0.0f || (b.def.flags & 4u) != 0) {
        b.force = Vec3{0.0f, 0.0f, 0.0f};
        b.torque = Vec3{0.0f, 0.0f, 0.0f};
        return; // static / no-integration
    }
    Vec3 force = b.ext_force + b.force;
    Vec3 torque = b.ext_torque + b.torque;

    // Classic RK4; stage weights 0.5/0.5/1 and final 1/6,1/3,1/3,1/6.
    BodyState k1 = evaluate(b, start, force, torque);
    BodyState k2 = evaluate(b, stepped(start, k1, h * 0.5f), force, torque);
    BodyState k3 = evaluate(b, stepped(start, k2, h * 0.5f), force, torque);
    BodyState k4 = evaluate(b, stepped(start, k3, h), force, torque);

    // The position-correction offset (state s[6..8]) is kept out of pos for
    // contact queries and folded in at the end of the step, so `d` stays the
    // uncorrected separation the resolve formula expects (notes/11 §5).
    b.pos = start.pos + k1.pos * (h / 6.0f) + k2.pos * (h / 3.0f) +
            k3.pos * (h / 3.0f) + k4.pos * (h / 6.0f);
    b.vel = start.vel + k1.vel * (h / 6.0f) + k2.vel * (h / 3.0f) +
            k3.vel * (h / 3.0f) + k4.vel * (h / 6.0f);
    b.angular_momentum = start.momentum + k1.momentum * (h / 6.0f) +
                         k2.momentum * (h / 3.0f) + k3.momentum * (h / 3.0f) +
                         k4.momentum * (h / 6.0f);
    b.rot = Quat{start.rot.x + k1.rot.x * (h / 6.0f) + k2.rot.x * (h / 3.0f) +
                     k3.rot.x * (h / 3.0f) + k4.rot.x * (h / 6.0f),
                 start.rot.y + k1.rot.y * (h / 6.0f) + k2.rot.y * (h / 3.0f) +
                     k3.rot.y * (h / 3.0f) + k4.rot.y * (h / 6.0f),
                 start.rot.z + k1.rot.z * (h / 6.0f) + k2.rot.z * (h / 3.0f) +
                     k3.rot.z * (h / 3.0f) + k4.rot.z * (h / 6.0f),
                 start.rot.w + k1.rot.w * (h / 6.0f) + k2.rot.w * (h / 3.0f) +
                     k3.rot.w * (h / 3.0f) + k4.rot.w * (h / 6.0f)};
    quat_normalize(&b.rot);

    // Safety clamps (must be reproduced): |omega| <= 20, |v| <= 250.
    Vec3 omega = b.angular_velocity();
    float w2 = vec_dot(omega, omega);
    if (w2 > 0.0f && w2 * 0.0025f > 1.0f)
        b.angular_momentum = b.angular_momentum * (20.0f / sqrtf(w2));
    float v2 = vec_dot(b.vel, b.vel);
    if (v2 > 62500.0f)
        b.vel = b.vel * (250.0f / sqrtf(v2));

    // notes/08: optional angular damping `1 + mass*coef*dt*100` when +0x9c < 0.
    // "mass" is read as inverse mass: the default -0.01*m then gives a
    // mass-independent 0.98 per 0.02 s step (the literal product is unbounded).
    if (b.def.angular_drag < 0.0f) {
        float factor = 1.0f + b.def.inv_mass * b.def.angular_drag * h * 100.0f;
        if (factor < 0.0f)
            factor = 0.0f;
        b.angular_momentum = b.angular_momentum * factor;
    }

    // Solver forces are consumed by this integration (applyForceAt: "consumed
    // by the next integration").
    b.force = Vec3{0.0f, 0.0f, 0.0f};
    b.torque = Vec3{0.0f, 0.0f, 0.0f};
}

// ---- Contact resolution (notes/11 §5, c_collision__resolve 0x4f0dd0) ----

static float contact_k(const RigidBody &a, const RigidBody &b, Vec3 n, Vec3 pa, Vec3 pb)
{
    // K = n . ((I^-1 (r x n)) x r) + invMass, the documented form; the scalar
    // triple product reduces it to (r x n) . I^-1 (r x n).
    Mat34 ra = a.rotation();
    Vec3 ca = vec_cross(mat_vec(ra, pa), n);
    float k = vec_dot(ca, mat_vec(a.world_inv_inertia(), ca));
    Mat34 rb = b.rotation();
    Vec3 cb = vec_cross(mat_vec(rb, pb), n);
    k += vec_dot(cb, mat_vec(b.world_inv_inertia(), cb));
    return k + a.def.inv_mass + b.def.inv_mass;
}

static StepState *find_step_state(std::vector<StepState> &states, RigidBody *body)
{
    for (StepState &s : states)
        if (s.body == body)
            return &s;
    return nullptr;
}

// Replay the step's accumulated impulses after the gravity/force integration.
// Applying them here (velocity level, not as an initial offset) keeps a resting
// body from riding the support impulse across the whole step.
static void state_apply_accumulated(RigidBody &b, const StepState &st)
{
    if (b.def.inv_mass <= 0.0f)
        return;
    b.vel += st.impulse * b.def.inv_mass;
    b.angular_momentum += st.impulse_momentum;
}

// applyImpulseAt: the working body keeps the impulse for the final state; the
// step accumulator replays it when the next sub-iteration re-integrates.
static void state_apply_impulse(RigidBody &b, StepState *st, const Vec3 &impulse,
                                const Vec3 &p_local)
{
    b.apply_impulse_at(impulse, p_local);
    if (!st || b.def.inv_mass <= 0.0f)
        return;
    st->impulse += impulse;
    Vec3 r = mat_vec(b.rotation(), p_local);
    st->impulse_momentum += vec_cross(r, impulse);
}

// Gauss-Jordan with partial pivoting for the <=4 point manifold LCP. The
// augmented column n holds the right-hand side.
static bool solve_linear(float A[4][5], int n)
{
    for (int col = 0; col < n; col++) {
        int piv = col;
        for (int r = col + 1; r < n; r++)
            if (fabsf(A[r][col]) > fabsf(A[piv][col]))
                piv = r;
        if (fabsf(A[piv][col]) < 1e-9f)
            return false;
        if (piv != col)
            for (int k = col; k <= n; k++) {
                float t = A[col][k];
                A[col][k] = A[piv][k];
                A[piv][k] = t;
            }
        float inv = 1.0f / A[col][col];
        for (int k = col; k <= n; k++)
            A[col][k] *= inv;
        for (int r = 0; r < n; r++) {
            if (r == col || A[r][col] == 0.0f)
                continue;
            float f = A[r][col];
            for (int k = col; k <= n; k++)
                A[r][k] -= f * A[col][k];
        }
    }
    return true;
}

// Resolve one body pair as a block: solve the normal impulses of its manifold
// together (notes/11 §5 is per-contact PGS; the block keeps a face manifold
// from oscillating and lets the normal resultant shift to counter friction
// torque like a real contact patch). Friction and position correction stay
// per-contact and in list order.
static int resolve_pair(Contact *cs, int n, float dt, std::vector<StepState> &states)
{
    RigidBody &a = *cs[0].a;
    RigidBody &b = *cs[0].b;
    bool dyn_a = a.def.inv_mass > 0.0f && (a.def.flags & 4u) == 0;
    bool dyn_b = b.def.inv_mass > 0.0f && (b.def.flags & 4u) == 0;
    if (!dyn_a && !dyn_b)
        return 0;
    // TODO: compound children (root-frame lift) and the 0x20000 special case are
    // not ported; this core has single, non-compound bodies only.
    StepState *sa = find_step_state(states, &a);
    StepState *sb = find_step_state(states, &b);
    if (!sa || !sb)
        return 0;

    // Collect the still-live contacts of the manifold (c_collision d <= 0.05).
    int idx[4], m = 0;
    for (int i = 0; i < n && m < 4; i++)
        if (cs[i].separation <= 0.05f)
            idx[m++] = i;
    if (m == 0)
        return 0;

    Mat34 rotA = a.rotation(), rotB = b.rotation();
    Mat34 ia = a.world_inv_inertia(), ib = b.world_inv_inertia();
    Vec3 nrm[4], ra[4], rb[4];
    float e[4], vn[4], target[4], lambda0[4];
    for (int i = 0; i < m; i++) {
        Contact &c = cs[idx[i]];
        nrm[i] = c.normal;
        ra[i] = mat_vec(rotA, c.local_a);
        rb[i] = mat_vec(rotB, c.local_b);
        e[i] = c.restitution;
        lambda0[i] = c.normal_impulse;
        Vec3 vrel = a.point_velocity(c.local_a) - b.point_velocity(c.local_b);
        vn[i] = vec_dot(c.normal, vrel);
        // notes/11 §5 mode 6 restitution rhs. Mode 7 targets zero approach:
        // the documented resting spring bias is a force, and replayed as a
        // velocity-level impulse it would leave a permanent 0.09 m/s separation
        // at d = 0.021, so the documented position correction replaces it.
        target[i] = (c.mode == 6 && vn[i] < -0.2f)
                        ? -(1.0f + e[i]) * (vn[i] + 0.2f)
                        : 0.0f;
    }

    // Manifold effective mass: K[i][j] is the vn change at point i from a unit
    // normal impulse at point j (contact_k is the diagonal).
    float K[4][4];
    for (int i = 0; i < m; i++)
        for (int j = 0; j < m; j++) {
            Vec3 ca = vec_cross(ra[j], nrm[j]);
            Vec3 cb = vec_cross(rb[j], nrm[j]);
            float k = vec_dot(nrm[i], nrm[j]) * (a.def.inv_mass + b.def.inv_mass);
            k += vec_dot(nrm[i], vec_cross(mat_vec(ia, ca), ra[i]));
            k += vec_dot(nrm[i], vec_cross(mat_vec(ib, cb), rb[i]));
            K[i][j] = k;
        }

    // Small LCP: lambda >= 0, vn_after >= target, equal where lambda > 0.
    // Enumerate active sets; the true one has non-negative total impulses and
    // no untouched point still approaching. 2^4 at most.
    float delta[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool solved = false;
    for (int mask = (1 << m) - 1; mask >= 0 && !solved; mask--) {
        int act[4], na = 0;
        for (int i = 0; i < m; i++)
            if (mask & (1 << i))
                act[na++] = i;
        float A[4][5] = {};
        for (int r = 0; r < na; r++) {
            for (int c = 0; c < na; c++)
                A[r][c] = K[act[r]][act[c]];
            A[r][na] = target[act[r]] - vn[act[r]];
        }
        if (na > 0 && !solve_linear(A, na))
            continue;
        float d[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        bool valid = true;
        for (int r = 0; r < na; r++) {
            d[act[r]] = A[r][na];
            if (lambda0[act[r]] + d[act[r]] < -1e-6f)
                valid = false;
        }
        for (int i = 0; i < m && valid; i++) {
            if (mask & (1 << i))
                continue;
            float v = vn[i];
            for (int j = 0; j < m; j++)
                v += K[i][j] * d[j];
            if (v < target[i] - 1e-5f)
                valid = false;
        }
        if (valid) {
            for (int i = 0; i < m; i++)
                delta[i] = d[i];
            solved = true;
        }
    }
    if (!solved) {
        // Numerical fallback: full solve with the total impulse clamped at zero.
        float A[4][5] = {};
        for (int r = 0; r < m; r++) {
            for (int c = 0; c < m; c++)
                A[r][c] = K[r][c];
            A[r][m] = target[r] - vn[r];
        }
        if (!solve_linear(A, m))
            return 0;
        for (int i = 0; i < m; i++) {
            delta[i] = A[i][m];
            if (lambda0[i] + delta[i] < 0.0f)
                delta[i] = -lambda0[i];
        }
    }

    int result = 0;
    for (int i = 0; i < m; i++) {
        if (delta[i] == 0.0f)
            continue;
        Contact &c = cs[idx[i]];
        state_apply_impulse(a, sa, c.normal * delta[i], c.local_a);
        state_apply_impulse(b, sb, -c.normal * delta[i], c.local_b);
        c.normal_impulse += delta[i];
        result = 1;
    }

    for (int i = 0; i < m; i++) {
        Contact &c = cs[idx[i]];
        Vec3 vrel = a.point_velocity(c.local_a) - b.point_velocity(c.local_b);
        float vn2 = vec_dot(c.normal, vrel);
        // Friction cap uses the step's accumulated normal impulse; the exe's
        // friction is a force, so it is applied to next pass's integration.
        float normal_force = c.normal_impulse / dt;
        if (normal_force > 0.001f && c.friction > 0.0f) {
            Vec3 vt = vrel - c.normal * vn2;
            float vt2 = vec_dot(vt, vt);
            if (vt2 > 0.001f) {
                float speed = sqrtf(vt2);
                Vec3 t = vt * (1.0f / speed);
                float kt = contact_k(a, b, t, c.local_a, c.local_b);
                if (kt > 0.0f) {
                    float f = (speed / kt) / dt;
                    if (f > c.friction * normal_force) {
                        f = c.friction * normal_force;
                        result = 2;
                    } else {
                        result = 1;
                    }
                    a.apply_force_at(-t * f, c.local_a);
                    b.apply_force_at(t * f, c.local_b);
                    c.friction_force += f;
                }
            }
        }

        if (c.separation < 0.021f) {
            float off_a = vec_dot(c.normal, a.pos_correction);
            float off_b = vec_dot(c.normal, b.pos_correction);
            float correction = (0.021f - c.separation) - (off_a - off_b);
            if (correction > 0.0f) {
                if (!dyn_a) {
                    b.pos_correction -= c.normal * correction;
                } else if (!dyn_b) {
                    a.pos_correction += c.normal * correction;
                } else {
                    a.pos_correction += c.normal * (correction * 0.5f);
                    b.pos_correction -= c.normal * (correction * 0.5f);
                }
                result = 1;
            }
        }
    }

    if (result != 0) {
        // Wake both roots (clear other-state flags bit0). Sleeping is not decoded.
        a.settled = false;
        b.settled = false;
    }
    return result;
}

// ---- Contact generation / refresh (Collide__bodyPair 0x4fb370) ----

static bool is_dynamic(const RigidBody &b)
{
    return b.def.inv_mass > 0.0f && (b.def.flags & 4u) == 0;
}

// notes/11 §4/§5: a pair is added below 0.02 m but an existing contact is kept
// until it separates beyond 0.05 m (0x4f9af0 drops it above rA+rB+0.05).
static const float kAddSlop = 0.02f;
static const float kKeepSlop = 0.05f;

static bool pair_has_contacts(const World &w, const RigidBody *a, const RigidBody *b)
{
    for (const Contact &c : w.contacts)
        if (c.a == a && c.b == b)
            return true;
    return false;
}

static int existing_mode(const World &w, const RigidBody *a, const RigidBody *b)
{
    for (const Contact &c : w.contacts)
        if (c.a == a && c.b == b)
            return c.mode;
    return 6;
}

// PhysicsWorld__updateContacts 0x4f9af0 plus Collide__bodyPair 0x4fb370.
// add_new: the broad phase pass at the start of a step may create pairs; later
// sub-iteration refreshes only update pairs that already exist.
static void update_contacts(World &w, bool add_new)
{
    std::vector<Contact> next;
    for (size_t i = 0; i < w.bodies.size(); i++) {
        RigidBody &a = *w.bodies[i];
        for (size_t j = i + 1; j < w.bodies.size(); j++) {
            RigidBody &b = *w.bodies[j];
            if (!is_dynamic(a) && !is_dynamic(b))
                continue;
            bool existing = pair_has_contacts(w, &a, &b);
            if (!add_new && !existing)
                continue;
            int ta = a.def.shape.type, tb = b.def.shape.type;
            // Collide__bodyPair skips shape 0 and 7 on either side.
            if (ta == SHAPE_NONE || tb == SHAPE_NONE || ta == SHAPE_MESH_NO_POLY ||
                tb == SHAPE_MESH_NO_POLY)
                continue;
            if (!shape_kind_resolved(ta) || !shape_kind_resolved(tb) ||
                (ta == SHAPE_PLANE && tb == SHAPE_PLANE)) {
                // TODO(notes/11 §1/§4): unresolved kind, no contacts are invented.
                if (add_new)
                    w.unsupported_pairs++;
                continue;
            }
            ShapeHit hits[4];
            int count = collide_shapes_within(a.def.shape, a.pose(), b.def.shape, b.pose(), hits, 4,
                                              existing ? kKeepSlop : kAddSlop);
            int mode = existing ? existing_mode(w, &a, &b) : 6;
            Mat34 ra = a.rotation(), rb = b.rotation();
            for (int k = 0; k < count; k++) {
                Contact c;
                c.a = &a;
                c.b = &b;
                c.normal = hits[k].normal;
                c.separation = hits[k].separation;
                c.world_point = (hits[k].point_a + hits[k].point_b) * 0.5f;
                c.local_a = mat_tvec(ra, hits[k].point_a - a.pos);
                c.local_b = mat_tvec(rb, hits[k].point_b - b.pos);
                c.friction = a.def.shape.friction < b.def.shape.friction
                                 ? a.def.shape.friction
                                 : b.def.shape.friction;
                c.restitution = 0.5f * (a.def.shape.restitution + b.def.shape.restitution);
                c.mode = mode;
                c.feature_a = hits[k].feature_a;
                c.feature_b = hits[k].feature_b;
                if (!add_new) {
                    // Sub-iteration refresh: keep the accumulated normal impulse
                    // (+0xb8) of the nearest matching point. Witness points move
                    // and re-sort between passes, so match by A-local position.
                    const Contact *match = nullptr;
                    float best = 1e-4f;
                    for (const Contact &old : w.contacts) {
                        if (old.a != &a || old.b != &b)
                            continue;
                        Vec3 diff = old.local_a - c.local_a;
                        float d2 = vec_dot(diff, diff);
                        if (d2 < best) {
                            best = d2;
                            match = &old;
                        }
                    }
                    if (match) {
                        c.normal_impulse = match->normal_impulse;
                        c.bias_force = match->bias_force;
                        c.friction_force = match->friction_force;
                    }
                }
                next.push_back(c);
            }
        }
    }
    w.contacts.swap(next);
}

int World::step(float dt)
{
    if (dt < 0.0001f)
        return 0;
    const float h = dt;

    // Force model (FUN_004f8460): gravity + linear drag, once per step, per root.
    std::vector<StepState> states(bodies.size());
    for (size_t i = 0; i < bodies.size(); i++) {
        RigidBody &b = *bodies[i];
        b.force = Vec3{0.0f, 0.0f, 0.0f};
        b.torque = Vec3{0.0f, 0.0f, 0.0f};
        b.ext_force = Vec3{0.0f, 0.0f, 0.0f};
        b.ext_torque = Vec3{0.0f, 0.0f, 0.0f};
        b.pos_correction = Vec3{0.0f, 0.0f, 0.0f};
        if (is_dynamic(b)) {
            float mass = 1.0f / b.def.inv_mass;
            b.ext_force = gravity * mass + b.vel * b.def.linear_drag;
            // TODO: buoyancy (body def flag 0x80) and the ground/water plane
            // path are documented but not part of this core.
        }
        states[i].body = &b;
        states[i].start = BodyState{b.pos, b.vel, b.angular_momentum, b.rot};
    }

    update_contacts(*this, true);

    int iterations = 0;
    for (int it = 0; it < 20; it++) {
        // Every sub-iteration re-integrates all roots from the immutable
        // start-of-step state (gravity/forces only), then replays the impulses
        // accumulated so far as a velocity-level change.
        for (StepState &s : states) {
            integrate_body(*s.body, s.start, h);
            state_apply_accumulated(*s.body, s);
        }
        update_contacts(*this, false); // contact refresh, no new pairs mid-step
        int corrections = 0;
        // Manifold points of a pair are contiguous; resolve each pair as a block.
        size_t first = 0;
        while (first < contacts.size()) {
            size_t last = first + 1;
            while (last < contacts.size() && contacts[last].a == contacts[first].a &&
                   contacts[last].b == contacts[first].b)
                last++;
            corrections += resolve_pair(&contacts[first], int(last - first), h, states);
            first = last;
        }
        iterations = it + 1;
        if (corrections == 0)
            break;
    }
    last_iterations = iterations;

    // Impact pass (PhysicsWorld__resolveImpacts 0x4ffd20): impacts become
    // resting contacts. Script event dispatch is not part of this core.
    for (Contact &c : contacts)
        if (c.mode == 6)
            c.mode = 7;

    // Fold the accumulated state s[6..8] offset into the pose; it only spans
    // this step (the next step's contact query starts from the corrected pose).
    for (RigidBody *b : bodies) {
        b->pos += b->pos_correction;
        b->pos_correction = Vec3{0.0f, 0.0f, 0.0f};
    }
    return 1;
}
