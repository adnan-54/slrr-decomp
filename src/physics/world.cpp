// Rigid-body integration, force model and contact solver (notes/08-physics.md,
// notes/11-collision-shapes.md §5). The exe keeps two state buffers and reruns
// the RK4 integration from the start-of-step state on every sub-iteration; the
// port reproduces that: solver impulses, forces and the position offset sit in
// the start state and act through every later re-integration.
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
        // volume integrals, 0x508d20); mesh bodies are static for now.
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
    body_count++; // w+0x54, every add
    if (b->def.flags & 4u) // 0x4f7830: flag 4 skips the active root list
        statics.insert(statics.begin(), b);
    else
        bodies.insert(bodies.begin(), b); // head insert into the root's child list
}

void World::unlink(RigidBody *b)
{
    if (!b) return;
    if (b->parent) { // 0x501d00: body is on its parent's child list
        std::vector<RigidBody *> &c = b->parent->children;
        for (size_t i = 0; i < c.size(); i++)
            if (c[i] == b) {
                c.erase(c.begin() + (long)i);
                break;
            }
        b->parent = nullptr;
        return;
    }
    for (size_t i = 0; i < bodies.size(); i++)
        if (bodies[i] == b) {
            bodies.erase(bodies.begin() + (long)i);
            return;
        }
    for (size_t i = 0; i < sleeping.size(); i++)
        if (sleeping[i] == b) {
            sleeping.erase(sleeping.begin() + (long)i);
            return;
        }
    for (size_t i = 0; i < statics.size(); i++)
        if (statics[i] == b) {
            statics.erase(statics.begin() + (long)i);
            return;
        }
}

void World::attach_child(RigidBody *child, RigidBody *parent)
{
    if (!child || !parent) return; // 0x4f7b20: head insert into the child list
    child->parent = parent;
    parent->children.insert(parent->children.begin(), child);
}

// 0x506d90 PhysBody__sleep. Root-level bodies move from the active list to the
// sleeping root (w+0x88); bodies that already have a parent just follow it.
void World::sleep(RigidBody *b)
{
    if (!b || b->asleep) return;
    if (!b->parent) {
        unlink(b);
        sleeping.insert(sleeping.begin(), b);
    }
    b->asleep = true;
    b->vel = Vec3{0.0f, 0.0f, 0.0f};
    b->angular_momentum = Vec3{0.0f, 0.0f, 0.0f};
    b->force = Vec3{0.0f, 0.0f, 0.0f};
    b->torque = Vec3{0.0f, 0.0f, 0.0f};
    b->pos_correction = Vec3{0.0f, 0.0f, 0.0f};
    // INVENTED: the exe also copies the active state onto the other state here;
    // src has one state, so only the flags bit (integrated) is cleared.
    b->integrated = false;
    for (RigidBody *c : b->children) sleep(c);
}

// 0x507060 PhysBody__wake. Root-level bodies move back to the active list.
void World::wake(RigidBody *b)
{
    if (!b || !b->asleep) return;
    b->asleep = false;
    // INVENTED: the exe resets body+0xc (0), +0x10 (100000.0f) and +0x14 (0)
    // when !(body+0x58 & 4); RigidBody has no matching fields.
    if (!b->parent) {
        unlink(b);
        bodies.insert(bodies.begin(), b);
    }
    for (RigidBody *c : b->children) wake(c);
}

World &process_world()
{
    // INVENTED: function-local static stands in for the exe's static world at
    // 0x60251c, built by PhysicsWorld__init 0x4fffb0 at engine startup.
    static World world;
    return world;
}

// ---- RK4 integration (notes/08 §Integrator, FUN_00506530) ----

struct BodyState {
    Vec3 pos, vel, momentum;
    Quat rot;
};

// Step-start state for one root body: the exe's active state buffer (index
// +0x20c). RK4 restarts from it on every sub-iteration (0x5001c0 integrates
// active -> other); PhysBody__applyImpulseAt 0x505450 adds solver impulses to
// it, so they reach the working (other) state at the next integration.
struct StepState {
    RigidBody *body;
    BodyState start;
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

    // 0x506530 adds the active state's position offset s[6..8] after the RK4
    // sum, so contact queries on the integrated state see the correction.
    b.pos = start.pos + k1.pos * (h / 6.0f) + k2.pos * (h / 3.0f) +
            k3.pos * (h / 3.0f) + k4.pos * (h / 6.0f) + b.pos_correction;
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

    // Solver forces stay in the active state for the whole step and are
    // re-applied by every sub-iteration's integration (0x5051e0 accumulates).
    b.integrated = true; // 0x5001c0 sets other-state flags bit0
}

// ---- Contact resolution (notes/11 §5, c_collision__resolve 0x4f0dd0) ----

static bool is_dynamic(const RigidBody &b)
{
    return b.def.inv_mass > 0.0f && (b.def.flags & 4u) == 0;
}

static StepState *find_step_state(std::vector<StepState> &states, RigidBody *body)
{
    for (StepState &s : states)
        if (s.body == body)
            return &s;
    return nullptr;
}

// One contact side in the active (start) state: 0x4f0dd0 takes r = R p from
// PhysBody__rotateToWorld 0x5061f0 and I^-1 from state +0xa4, both active.
struct ContactSide {
    bool dyn;
    Vec3 r;
    Mat34 inv_inertia; // world
    float inv_mass;
};

static ContactSide contact_side(const StepState &st, bool dyn, const Vec3 &p_local)
{
    ContactSide s{dyn, Vec3{0.0f, 0.0f, 0.0f}, Mat34{}, 0.0f};
    if (!dyn)
        return s; // a non-dynamic side adds nothing to K
    const RigidBody &b = *st.body;
    Quat q = st.start.rot;
    quat_normalize(&q);
    Mat34 r;
    quat_to_mat(&r, &q);
    Mat34 rt, tmp;
    mat_transpose(&rt, &r);
    mat_mul(&tmp, &r, &b.def.inv_inertia);
    mat_mul(&s.inv_inertia, &tmp, &rt);
    s.r = mat_vec(r, p_local);
    s.inv_mass = b.def.inv_mass;
    return s;
}

// K term of one side along d: d . ((I^-1 (r x d)) x r) + invMass.
static float side_k(const ContactSide &s, const Vec3 &d)
{
    if (!s.dyn)
        return 0.0f;
    Vec3 u = mat_vec(s.inv_inertia, vec_cross(s.r, d));
    return vec_dot(d, vec_cross(u, s.r)) + s.inv_mass;
}

// PhysBody__applyImpulseAt 0x505450: v += J invm, L += r x J on the active state.
static void side_impulse(StepState &st, const ContactSide &s, const Vec3 &j)
{
    st.start.vel += j * s.inv_mass;
    st.start.momentum += vec_cross(s.r, j);
}

// PhysBody__applyForceAt 0x5051e0: force += F, torque += r x F (active state).
static void side_force(RigidBody &b, const ContactSide &s, const Vec3 &f)
{
    b.force += f;
    b.torque += vec_cross(s.r, f);
}

// c_collision__resolve 0x4f0dd0, reached through constraint__vf4_resolve
// 0x4f0db0 for modes 6/7. Returns 0 (nothing applied), 1, or 2 (friction
// clamped). Velocities come from the integrated (other) state, PhysBody__
// pointVelocity 0x505e80, so impulses of this pass show only after the next
// integration.
static int resolve_contact(Contact &c, float dt, std::vector<StepState> &states)
{
    if (c.mode != 6 && c.mode != 7)
        return 0; // 0x4f0db0
    if (c.separation > 0.05f)
        return 0;
    RigidBody &a = *c.a;
    RigidBody &b = *c.b;
    // INVENTED: the exe's dyn test is only !(def flags & 4); static bodies here
    // are built with inv_mass 0 and no flag, so inv_mass 0 also counts as static.
    // Body flag 0x80 (also excluded by the exe) is not modelled.
    bool dyn_a = is_dynamic(a);
    bool dyn_b = is_dynamic(b);
    if (!dyn_a && !dyn_b)
        return 0;
    // TODO: compound children (root-frame lift) and the 0x20000 special case are
    // not ported; this core has single, non-compound bodies only.
    StepState *sa = find_step_state(states, &a);
    StepState *sb = find_step_state(states, &b);
    if (!sa || !sb)
        return 0;

    const Vec3 n = c.normal;
    Vec3 vrel = a.point_velocity(c.local_a) - b.point_velocity(c.local_b);
    float vn = vec_dot(n, vrel);
    float imp = 0.0f;  // mode 6: velocity-level impulse
    float bias = 0.0f; // mode 7: applied as a force
    if (c.mode == 6) {
        if (vn < -0.2f)
            imp = -((c.restitution + 1.0f) * (vn + 0.2f));
    } else {
        // INVENTED: a static body's bit0 is never set by 0x5001c0; its initial
        // value is not traced and is taken as clear.
        if (!(a.integrated && b.integrated) && !(c.separation - 0.03f >= 0.0f))
            bias = (0.03f - c.separation) * 10.0f;
        if (vn < -0.2f)
            bias -= (c.restitution + 1.0f) * (vn + 0.2f);
    }

    int result = 0;
    float total = imp + bias;
    if (total > 0.0f) {
        ContactSide pa = contact_side(*sa, dyn_a, c.local_a);
        ContactSide pb = contact_side(*sb, dyn_b, c.local_b);
        float inv_k = 1.0f / (side_k(pa, n) + side_k(pb, n));
        if (imp > 0.0f) {
            float p = inv_k * imp;
            if (dyn_a)
                side_impulse(*sa, pa, n * p);
            if (dyn_b)
                side_impulse(*sb, pb, n * -p);
            c.normal_impulse += p;
        }
        if (bias > 0.0f) {
            float f = (inv_k * bias) / dt;
            if (dyn_a)
                side_force(a, pa, n * f);
            if (dyn_b)
                side_force(b, pb, n * -f);
            c.bias_force += f;
        }
        result = 1;
        // Friction cap from this call's normal response only; vt uses the
        // pre-response vrel.
        float normal_force = total * inv_k * (1.0f / dt);
        if (normal_force > 0.001f && c.friction > 0.0f) {
            Vec3 vt = vrel - n * vn;
            float vt2 = vec_dot(vt, vt);
            if (vt2 > 0.001f) {
                float speed = sqrtf(vt2);
                Vec3 t = vt * (1.0f / speed);
                float f = (speed / (side_k(pa, t) + side_k(pb, t))) * (1.0f / dt);
                float cap = c.friction * normal_force;
                if (f > cap) {
                    f = cap;
                    result = 2;
                }
                if (dyn_a)
                    side_force(a, pa, t * -f);
                if (dyn_b)
                    side_force(b, pb, t * f);
                c.friction_force += f;
            }
        }
    }

    // Position offset s[6..8] of the active state; returns 1 even after a
    // clamped friction.
    if (c.separation < 0.021f) {
        float correction = (0.021f - c.separation) -
                           (vec_dot(n, a.pos_correction) - vec_dot(n, b.pos_correction));
        if (correction > 0.0f) {
            if (!dyn_a) {
                b.pos_correction -= n * correction;
            } else if (!dyn_b) {
                a.pos_correction += n * correction;
            } else {
                a.pos_correction += n * (correction * 0.5f);
                b.pos_correction += n * (correction * -0.5f);
            }
            result = 1;
        }
    }

    if (result == 0)
        return 0;
    // Clear other-state flags bit0 of the dynamic sides (forces re-integration).
    // The world+0x14 counter is not kept.
    if (dyn_a)
        a.integrated = false;
    if (dyn_b)
        b.integrated = false;
    return result;
}

// ---- Contact generation / refresh (Collide__bodyPair 0x4fb370) ----

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

// Pairs the port intentionally leaves without contacts (mesh-mesh has no
// narrow phase; a kind-6 shape without geometry is not a collider).
static bool pair_unsupported_mesh(const Shape &a, const Shape &b)
{
    return (a.type == SHAPE_MESH && !a.mesh) || (b.type == SHAPE_MESH && !b.mesh) ||
           (a.type == SHAPE_MESH && b.type == SHAPE_MESH);
}

// One candidate pair: broad-phase filters and per-point contact generation.
static void consider_pair(World &w, RigidBody &a, RigidBody &b, bool add_new,
                          std::vector<Contact> &next)
{
    if (!is_dynamic(a) && !is_dynamic(b))
        return;
    bool existing = pair_has_contacts(w, &a, &b);
    if (!add_new && !existing)
        return;
    int ta = a.def.shape.type, tb = b.def.shape.type;
    // Collide__bodyPair skips shape 0 and 7 on either side.
    if (ta == SHAPE_NONE || tb == SHAPE_NONE || ta == SHAPE_MESH_NO_POLY ||
        tb == SHAPE_MESH_NO_POLY)
        return;
    if (!shape_kind_resolved(ta) || !shape_kind_resolved(tb) ||
        (ta == SHAPE_PLANE && tb == SHAPE_PLANE) ||
        pair_unsupported_mesh(a.def.shape, b.def.shape)) {
        // Unsupported kind/pair, no contacts are invented.
        if (add_new)
            w.unsupported_pairs++;
        return;
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

// PhysicsWorld__updateContacts 0x4f9af0 plus Collide__bodyPair 0x4fb370.
// add_new: the broad phase pass at the start of a step may create pairs; later
// sub-iteration refreshes only update pairs that already exist.
static void update_contacts(World &w, bool add_new)
{
    std::vector<Contact> next;
    for (size_t i = 0; i < w.bodies.size(); i++) {
        RigidBody &a = *w.bodies[i];
        for (size_t j = i + 1; j < w.bodies.size(); j++)
            consider_pair(w, a, *w.bodies[j], add_new, next);
        // Flag-4 bodies sit outside the active root list (0x4f7830); pair them
        // with every active root. The shape 0/7 filter drops the ground itself.
        for (size_t j = 0; j < w.statics.size(); j++)
            consider_pair(w, a, *w.statics[j], add_new, next);
    }
    w.contacts.swap(next);
}

int World::step(float dt)
{
    // PhysicsWorld__step 0x502060 prologue: dt joins the accumulator; a sum
    // still below the threshold is kept there for the next call.
    const float t = dt + accumulator;
    accumulator = t;
    if (t < 0.0001f)
        return 0;
    accumulator -= t;
    last_dt = t;
    const float h = t;

    // Force model (FUN_004f8460): gravity + linear drag, once per step, per root.
    // shortcut: child bodies are carried but not integrated as a compound (0x4f7990/0x5032d0), upgrade with the compound card.
    // Contact resolution looks a body's step state up by identity, so the
    // statics paired by update_contacts need an entry too: they are side B of
    // a root-vs-ground contact (PhysicsWorld__addContact 0x502830) and
    // integrate_body leaves them untouched.
    std::vector<StepState> states(bodies.size() + statics.size());
    for (size_t i = 0; i < bodies.size(); i++) {
        RigidBody &b = *bodies[i];
        b.force = Vec3{0.0f, 0.0f, 0.0f};
        b.torque = Vec3{0.0f, 0.0f, 0.0f};
        b.ext_force = Vec3{0.0f, 0.0f, 0.0f};
        b.ext_torque = Vec3{0.0f, 0.0f, 0.0f};
        // INVENTED: where the exe resets s[6..8] between steps is not traced;
        // the integrated pose already contains it (0x506530).
        b.pos_correction = Vec3{0.0f, 0.0f, 0.0f};
        if (is_dynamic(b)) {
            b.integrated = false; // 0x4f8460 clears other-state flags bit0
            float mass = 1.0f / b.def.inv_mass;
            b.ext_force = gravity * mass + b.vel * b.def.linear_drag;
            // TODO: buoyancy (body def flag 0x80) and the ground/water plane
            // path are documented but not part of this core.
        }
        states[i].body = &b;
        states[i].start = BodyState{b.pos, b.vel, b.angular_momentum, b.rot};
    }
    for (size_t i = 0; i < statics.size(); i++) {
        RigidBody &b = *statics[i];
        StepState &s = states[bodies.size() + i];
        s.body = &b;
        s.start = BodyState{b.pos, b.vel, b.angular_momentum, b.rot};
    }

    update_contacts(*this, true);

    // 0x502060: integrate every root once, then sub-iterate. The exe skips
    // bodies whose flags bit0 is still set; re-integrating them from unchanged
    // inputs gives the same state.
    for (StepState &s : states)
        integrate_body(*s.body, s.start, h);
    int iterations = 0;
    for (int it = 0; it < 20; it++) {
        iterations = it + 1;
        update_contacts(*this, false); // contact refresh, no new pairs mid-step
        // 0x4f9af0 clears the converged flag (world+0x39c) when a contact is
        // within 0.02 m; only then are the contacts resolved.
        bool converged = true;
        for (const Contact &c : contacts)
            if (c.separation < 0.02f)
                converged = false;
        if (!converged) {
            int sum = 0;
            for (Contact &c : contacts)
                sum += resolve_contact(c, h, states);
            if (sum == 0)
                converged = true;
        }
        for (StepState &s : states)
            integrate_body(*s.body, s.start, h);
        if (converged)
            break;
    }
    last_iterations = iterations;

    // Impact pass (PhysicsWorld__resolveImpacts 0x4ffd20): impacts become
    // resting contacts. Script event dispatch is not part of this core.
    // TODO: the exe runs it inside the loop's event phase, only when
    // 0x4fc610 dispatched events, and resolves each mode-6 contact first.
    for (Contact &c : contacts)
        if (c.mode == 6)
            c.mode = 7;
    time += t; // 0x502060 epilogue: simulated time
    return 1;
}
