// Rigid-body integration, force model and contact solver (notes/08-physics.md,
// notes/11-collision-shapes.md §5). The exe keeps two state buffers and reruns
// the RK4 integration from the start-of-step state on every sub-iteration; the
// port reproduces that: solver impulses, forces and the position offset sit in
// the start state and act through every later re-integration.
#include <math.h>

#include "physics/mesh.h"
#include "physics/physics.h"
#include "physics/vclip.h"

namespace {

// 0x508820 Poly__massProps: Mirtich volume integrals of the triangle hull in
// doubles. Each triangle plus the origin forms a signed tetrahedron:
//   T0 = int dV, T1[k] = int x_k dV, T2[k] = int x_k^2 dV,
//   TP = int (xy, yz, zx) dV.
bool poly_integrals(const CollisionMesh &m, double *volume, double t1[3],
                    double t2[3], double tp[3], double centroid[3])
{
    double T0 = 0.0;
    double T1[3] = {0.0, 0.0, 0.0};
    double T2[3] = {0.0, 0.0, 0.0};
    double TP[3] = {0.0, 0.0, 0.0};
    for (const CollisionMesh::Triangle &t : m.triangles) {
        const Vec3 &a = m.vertices[t.v[0]];
        const Vec3 &b = m.vertices[t.v[1]];
        const Vec3 &c = m.vertices[t.v[2]];
        double v = (a.x * ((double)b.y * c.z - (double)b.z * c.y) +
                    a.y * ((double)b.z * c.x - (double)b.x * c.z) +
                    a.z * ((double)b.x * c.y - (double)b.y * c.x)) / 6.0;
        if (v == 0.0) continue;
        T0 += v;
        T1[0] += v * (a.x + b.x + c.x) / 4.0;
        T1[1] += v * (a.y + b.y + c.y) / 4.0;
        T1[2] += v * (a.z + b.z + c.z) / 4.0;
        T2[0] += v * ((double)a.x * a.x + (double)b.x * b.x + (double)c.x * c.x +
                      (double)a.x * b.x + (double)a.x * c.x + (double)b.x * c.x) / 10.0;
        T2[1] += v * ((double)a.y * a.y + (double)b.y * b.y + (double)c.y * c.y +
                      (double)a.y * b.y + (double)a.y * c.y + (double)b.y * c.y) / 10.0;
        T2[2] += v * ((double)a.z * a.z + (double)b.z * b.z + (double)c.z * c.z +
                      (double)a.z * b.z + (double)a.z * c.z + (double)b.z * c.z) / 10.0;
        TP[0] += v * (((double)a.x * a.y + (double)b.x * b.y + (double)c.x * c.y) / 10.0 +
                      ((double)a.x * b.y + (double)b.x * a.y + (double)a.x * c.y +
                       (double)c.x * a.y + (double)b.x * c.y + (double)c.x * b.y) / 20.0);
        TP[1] += v * (((double)a.y * a.z + (double)b.y * b.z + (double)c.y * c.z) / 10.0 +
                      ((double)a.y * b.z + (double)b.y * a.z + (double)a.y * c.z +
                       (double)c.y * a.z + (double)b.y * c.z + (double)c.y * b.z) / 20.0);
        TP[2] += v * (((double)a.z * a.x + (double)b.z * b.x + (double)c.z * c.x) / 10.0 +
                      ((double)a.z * b.x + (double)b.z * a.x + (double)a.z * c.x +
                       (double)c.z * a.x + (double)b.z * c.x + (double)c.z * b.x) / 20.0);
    }
    if (fabs(T0) < 1e-12) return false;
    *volume = T0;
    for (int k = 0; k < 3; k++) {
        t1[k] = T1[k];
        t2[k] = T2[k];
        tp[k] = TP[k];
        centroid[k] = T1[k] / T0;
    }
    return true;
}

}  // namespace

void compute_mass_props(BodyDef *def, float mass, Vec3 *center)
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
    case SHAPE_MESH: {
        // Poly__massProps 0x508d20: unit-density tensor about the centroid, then
        // inverse and scale by invMass (the caller does the 1/m).
        if (!s.mesh || !(mass > 0.0f)) break;
        double vol, I1[3], I2[3], IP[3], c[3];
        if (!poly_integrals(*s.mesh, &vol, I1, I2, IP, c)) break;
        if (center) *center = Vec3{(float)c[0], (float)c[1], (float)c[2]};
        double cx = c[0], cy = c[1], cz = c[2];
        double i00 = (I2[1] + I2[2]) / vol - (cy * cy + cz * cz);
        double i11 = (I2[0] + I2[2]) / vol - (cx * cx + cz * cz);
        double i22 = (I2[0] + I2[1]) / vol - (cx * cx + cy * cy);
        double i01 = -IP[0] / vol + cx * cy;
        double i12 = -IP[1] / vol + cy * cz;
        double i02 = -IP[2] / vol + cz * cx;
        Mat34 inertia{};
        inertia.m[0] = (float)i00;
        inertia.m[1] = inertia.m[4] = (float)i01;
        inertia.m[2] = inertia.m[8] = (float)i02;
        inertia.m[5] = (float)i11;
        inertia.m[6] = inertia.m[9] = (float)i12;
        inertia.m[10] = (float)i22;
        // mat_inverse 0x566830 has no singular guard; a flat hull stays static.
        double det = i00 * (i11 * i22 - i12 * i12) - i01 * (i01 * i22 - i12 * i02) +
                     i02 * (i01 * i12 - i11 * i02);
        if (fabs(det) < 1e-9) break;
        Mat34 inv{};
        mat_inverse(&inv, &inertia);
        float k = 1.0f / mass;
        for (int i = 0; i < 12; i++) def->inv_inertia.m[i] = inv.m[i] * k;
        break;
    }
    default:
        break;
    }
}

Mat34 RigidBody::rotation() const
{
    return pose_rotation(Pose{pos, rot});
}

// World inverse inertia R I^-1 R^T from an arbitrary rotation. q is normalised
// first, as the exe does before matrix(q) 0x5665c0.
static Mat34 world_inv_inertia_from(const BodyDef &def, Quat q)
{
    quat_normalize(&q);
    Mat34 r;
    quat_to_mat(&r, &q);
    Mat34 rt;
    mat_transpose(&rt, &r);
    Mat34 tmp;
    mat_mul(&tmp, &r, &def.inv_inertia);
    Mat34 out;
    mat_mul(&out, &tmp, &rt);
    return out;
}

Mat34 RigidBody::world_inv_inertia() const
{
    return world_inv_inertia_from(def, rot);
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

void World::remove_body(RigidBody *b)
{
    if (!b) return;
    unlink(b);
    // 0x4f6850 (body+0x30 contact list): the body's contacts go with it.
    for (size_t i = contacts.size(); i-- > 0;)
        if (contacts[i].a == b || contacts[i].b == b)
            contacts.erase(contacts.begin() + (long)i);
    // INVENTED: the exe's sweep 0x4fa900 sees a deleted body as a null record
    // side; src bodies die with their owner, so drop the record here instead of
    // dereferencing it (no EVENT_TRIGGER_OFF: the owner is being destroyed).
    for (size_t i = triggers.size(); i-- > 0;)
        if (triggers[i].a == b || triggers[i].b == b)
            triggers.erase(triggers.begin() + (long)i);
    body_count--;
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

// 0x505c40 derivative(y, h, k): the quaternion term uses the step-start omega
// (k.omega is never written by the exe, so every stage keeps y.omega).
static BodyState evaluate(const RigidBody &b, const BodyState &s, Vec3 force, Vec3 torque,
                          Vec3 omega)
{
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
    // 0x505c40: k.omega is never written, every stage uses the step-start omega.
    Vec3 omega0 = mat_vec(world_inv_inertia_from(b.def, start.rot), start.momentum);
    BodyState k1 = evaluate(b, start, force, torque, omega0);
    BodyState k2 = evaluate(b, stepped(start, k1, h * 0.5f), force, torque, omega0);
    BodyState k3 = evaluate(b, stepped(start, k2, h * 0.5f), force, torque, omega0);
    BodyState k4 = evaluate(b, stepped(start, k3, h), force, torque, omega0);

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

    // 0x506530: angular damping runs before the spin clamp, and its factor is
    // not clamped at 0.
    // notes/08: optional angular damping `1 + mass*coef*dt*100` when +0x9c < 0.
    // "mass" is read as inverse mass: the default -0.01*m then gives a
    // mass-independent 0.98 per 0.02 s step (the literal product is unbounded).
    if (b.def.angular_drag < 0.0f) {
        float factor = 1.0f + b.def.inv_mass * b.def.angular_drag * h * 100.0f;
        b.angular_momentum = b.angular_momentum * factor;
    }
    // 0x506530: L *= 1/(|w|^2*0.0025) when that exceeds 1 (|w| > 20); |v| <= 250.
    Vec3 omega = b.angular_velocity();
    float e = vec_dot(omega, omega) * 0.0025f;
    if (1.0f < e)
        b.angular_momentum = b.angular_momentum * (1.0f / e);
    float v2 = vec_dot(b.vel, b.vel);
    if (v2 > 62500.0f)
        b.vel = b.vel * (250.0f / sqrtf(v2));

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
    // Collide__bodyPair 0x4fb370 step 4: a body flagged 0x40 is a sensor and
    // makes no physical contact (0x4fd910 -> addTriggerContact, card 168).
    if ((a.body_flags | b.body_flags) & 0x40u)
        return;
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

// ---- Car hulls vs the map soup (0x4feb20 / 0x4fc7e0, card 160) ----

static float vec_axis(const Vec3 &v, int k) { return k == 0 ? v.x : k == 1 ? v.y : v.z; }

// 0x4f9860: swept sphere (p0 -> p1, radius r) vs one triangle; true = may touch.
static bool swept_sphere_face(const Vec3 &p0, const Vec3 &p1, float r, const Vec3 &v0,
                              const Vec3 &v1, const Vec3 &v2, const Vec3 &n)
{
    float da = vec_dot(n, p0 - v0), db = vec_dot(n, p1 - v0);
    if (!(da * db <= 0.0f || fabsf(da) <= r || fabsf(db) <= r))
        return false;
    const float r2 = r * r;
    const Vec3 e0 = v1 - v0, e1 = v2 - v1, e2 = v0 - v2;
    struct EdgeSide {
        Vec3 m, origin;
        float len2;
    };
    const EdgeSide sides[3] = {
        {vec_cross(e0, n), v0, vec_dot(e0, e0)},
        {vec_cross(e2, n), v0, vec_dot(e2, e2)},
        {vec_cross(e1, n), v1, vec_dot(e1, e1)},
    };
    for (const EdgeSide &s : sides) {
        float sa = vec_dot(s.m, p0 - s.origin);
        float sb = vec_dot(s.m, p1 - s.origin);
        if (sa > 0.0f && sb > 0.0f && sa * sa > s.len2 * r2 && sb * sb > s.len2 * r2)
            return false;
    }
    return true;
}

// 0x4fc450: body-frame box (half extents h) vs one triangle, 4 axes; true = keep.
static bool box_triangle(const Vec3 &h, const Vec3 &nb, const Vec3 tv[3])
{
    if (fabsf(vec_dot(nb, tv[0])) >
        h.x * fabsf(nb.x) + h.y * fabsf(nb.y) + h.z * fabsf(nb.z))
        return false;
    for (int k = 0; k < 3; k++) {
        float lo = vec_axis(tv[0], k), hi = lo;
        for (int i = 1; i < 3; i++) {
            float x = vec_axis(tv[i], k);
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        float hk = vec_axis(h, k);
        if (hi < -hk || lo > hk)
            return false;
    }
    return true;
}

// One child's world pose in the root frame (child -> parent -> world).
static Pose child_world_pose(const RigidBody &a, const RigidBody::CollisionChild &c)
{
    Pose p;
    p.pos = a.pos + mat_vec(a.rotation(), c.pose.pos);
    quat_mul(&p.rot, &a.rot, &c.pose.rot);
    return p;
}

// 0x506c00: body pose at t on the step-start -> integrated sweep (t = 1 is
// the integrated state, t = 0 the step start). The exe slerps the rotation
// (0x5681c0); src has no slerp helper, so shortest-path lerp + normalize.
static Pose pose_at(const RigidBody &b, const BodyState &start, float t)
{
    Pose p;
    p.pos = b.pos * t + start.pos * (1.0f - t);
    Quat a = b.rot, q = start.rot;
    if (a.x * q.x + a.y * q.y + a.z * q.z + a.w * q.w < 0.0f)
        a = Quat{-a.x, -a.y, -a.z, -a.w};
    p.rot = Quat{q.x + (a.x - q.x) * t, q.y + (a.y - q.y) * t,
                 q.z + (a.z - q.z) * t, q.w + (a.w - q.w) * t};
    quat_normalize(&p.rot);
    return p;
}

// Child pose in the root frame at sweep position t.
static Pose child_pose_at(const RigidBody &a, const RigidBody::CollisionChild &c,
                          const BodyState &start, float t)
{
    Pose root = pose_at(a, start, t);
    Pose p;
    p.pos = root.pos + mat_vec(pose_rotation(root), c.pose.pos);
    quat_mul(&p.rot, &root.rot, &c.pose.rot);
    return p;
}

// Signed distance of the lowest hull vertex along the face normal. This is the
// 0x4fc7e0 deep-branch witness (min vertex of kind 1 along the face normal);
// it keeps the sign a two-face triangle Poly (0x50f000) cannot.
static float hull_plane_distance(const Poly &poly, const Mat34 &rot, const Vec3 &pos,
                                 const Vec3 &n, const Vec3 &v0, Vec3 *support,
                                 int *vertex_index = nullptr)
{
    float lowest = 1e30f;
    Vec3 point{0.0f, 0.0f, 0.0f};
    int index = 0;
    for (size_t i = 0; i < poly.verts.size(); i++) {
        Vec3 world = pos + mat_vec(rot, poly.verts[i]);
        float d = vec_dot(n, world - v0);
        if (d < lowest) {
            lowest = d;
            point = world;
            index = (int)i;
        }
    }
    if (support) *support = point;
    if (vertex_index) *vertex_index = index;
    return poly.verts.empty() ? 1e30f : lowest;
}

// Kind-1 feature of a hull vertex (the deep branch reports it as fA).
static int hull_vertex_feature(const Poly &poly, int vertex)
{
    for (size_t i = 0; i < poly.features.size(); i++)
        if (poly.features[i].kind == POLY_V && poly.features[i].vertex == vertex)
            return (int)i;
    return 0;
}

// One (body child, soup triangle) candidate of 0x4feb20. The triangle and its
// two-face Poly (0x50f000) are fixed; 0x4fc7e0 re-evaluates the distance at
// poses along the step sweep.
struct HullCandidate {
    int child = -1;
    int tri = -1;
    Vec3 vw[3]{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    Vec3 n{0.0f, 1.0f, 0.0f};  // stored PHYS face normal (world)
    Poly face;
    float d = 0.0f;            // last distance - margin (0 for a mesh)
    bool deep = false;         // last evaluation took the deep branch
    bool marked = false;       // 0x4fc7e0 re-evaluation bookkeeping
    int fa = 0, fb = 0;
    Vec3 pa{0.0f, 0.0f, 0.0f};  // hull witness, child-local
    Vec3 pb{0.0f, 0.0f, 0.0f};  // face witness, world
    Vec3 nn{0.0f, 1.0f, 0.0f};  // face -> hull normal, world
};

// 0x50d230 at the sweep position t (margin 0 for a mesh).
static void evaluate_candidate(HullCandidate &c, const RigidBody &a,
                               const BodyState &start, float t)
{
    const RigidBody::CollisionChild &cc = a.collision_children[c.child];
    const Pose cp = child_pose_at(a, cc, start, t);
    c.deep = false;
    Pose bpose;  // the triangle poly holds world vertices
    c.d = vclip_closest(cp, *cc.poly, bpose, c.face, &c.fa, &c.fb, &c.pa, &c.pb, &c.nn);
}

// 0x4fc7e0 deep branch (best.d <= -margin): the lowest hull vertex along the
// face normal replaces the V-Clip result.
static void deep_candidate(HullCandidate &c, const RigidBody &a,
                           const BodyState &start, float t)
{
    const RigidBody::CollisionChild &cc = a.collision_children[c.child];
    const Pose cp = child_pose_at(a, cc, start, t);
    const Mat34 cr = pose_rotation(cp);
    Vec3 support{0.0f, 0.0f, 0.0f};
    int vi = 0;
    c.d = hull_plane_distance(*cc.poly, cr, cp.pos, c.n, c.vw[0], &support, &vi);
    c.deep = true;
    c.nn = c.n;
    c.fa = hull_vertex_feature(*cc.poly, vi);
    c.pa = mat_tvec(cr, support - cp.pos);
    c.pb = support - c.n * c.d;
}

// 0x4fc7e0's "still approaching" test: n . (body world point - pB) < 0.
static bool hull_approaching(const HullCandidate &c, const RigidBody &a,
                             const BodyState &start, float t)
{
    const RigidBody::CollisionChild &cc = a.collision_children[c.child];
    const Pose cp = child_pose_at(a, cc, start, t);
    return vec_dot(c.nn, cp.pos - c.pb) < 0.0f;
}

// One soup face: world vertices and normal, then the 0x4f9860 swept-sphere
// and 0x4fc450 OBB filters. Shared by the map-node and BVH walks.
static void collect_face_candidate(const CollisionMesh &gm, const Pose &gp,
                                   const Mat34 &gr, bool stored_normals, int child,
                                   uint32_t ti, const Pose &cp0, const Pose &cp1,
                                   const Mat34 &cr1, float br, const Vec3 &half,
                                   std::vector<HullCandidate> &out)
{
    const CollisionMesh::Triangle &tri = gm.triangles[ti];
    HullCandidate c;
    for (int i = 0; i < 3; i++)
        c.vw[i] = gp.pos + mat_vec(gr, gm.vertices[tri.v[i]]);
    Vec3 n = stored_normals ? mat_vec(gr, gm.face_normals[ti])
                            : vec_cross(c.vw[1] - c.vw[0], c.vw[2] - c.vw[0]);
    c.n = vec_normalize(n, Vec3{0.0f, 1.0f, 0.0f});
    if (!swept_sphere_face(cp0.pos, cp1.pos, br, c.vw[0], c.vw[1], c.vw[2], c.n))
        return;
    Vec3 tv[3];
    for (int i = 0; i < 3; i++)
        tv[i] = mat_tvec(cr1, c.vw[i] - cp1.pos);
    if (!box_triangle(half, mat_tvec(cr1, c.n), tv))
        return;
    // INVENTED side gate (card 164): the merged soup stores road
    // faces twice (normals up and down); keep the face the hull
    // started in front of.
    if (!(vec_dot(c.n, cp0.pos - c.vw[0]) > 0.0f))
        return;
    c.child = child;
    c.tri = (int)ti;
    c.face = poly_triangle(c.vw[0], c.vw[1], c.vw[2], c.n);
    out.push_back(c);
}

// 0x4feb20 broad phase: map nodes (0x4fe6f0 walk order) gate the step-start
// sphere against the node's record box; meshes without a node table (SCX)
// keep the BVH walk with the 0x4f9860/0x4fc450 filters on the step sweep.
// INVENTED: map nodes per 0x4feb20; streaming not modelled (no collision
// effect for cars, notes/24).
static void collect_hull_candidates(const RigidBody &a, const BodyState &start,
                                    const RigidBody &g, std::vector<HullCandidate> &out)
{
    const CollisionMesh &gm = *g.def.shape.mesh;
    if (gm.empty())
        return;
    const Pose gp = g.pose();
    const Mat34 gr = g.rotation();
    const bool stored_normals = gm.face_normals.size() == gm.triangles.size();
    for (size_t ci = 0; ci < a.collision_children.size(); ci++) {
        const RigidBody::CollisionChild &cc = a.collision_children[ci];
        if (cc.shape.type != SHAPE_MESH || !cc.shape.mesh || !cc.poly)
            continue;
        const Pose cp0 = child_world_pose(a, cc);            // start pose
        const Pose cp1 = child_pose_at(a, cc, start, 1.0f);  // integrated pose
        const Mat34 cr1 = pose_rotation(cp1);
        const float br = cc.shape.bounding_radius();
        // Face-cache AABB half extents (shape+0x14): raw mesh bounds + 0.05,
        // plus |R^T motion| * 1.5 for the swept OBB (0x4feb20).
        const CollisionMesh &cm = *cc.shape.mesh;
        Vec3 lm = mat_tvec(cr1, cp1.pos - cp0.pos);
        const Vec3 half{fmaxf(fabsf(cm.bounds_min.x), fabsf(cm.bounds_max.x)) + 0.05f +
                            fabsf(lm.x) * 1.5f,
                        fmaxf(fabsf(cm.bounds_min.y), fabsf(cm.bounds_max.y)) + 0.05f +
                            fabsf(lm.y) * 1.5f,
                        fmaxf(fabsf(cm.bounds_min.z), fabsf(cm.bounds_max.z)) + 0.05f +
                            fabsf(lm.z) * 1.5f};
        if (!gm.map_nodes.empty()) {
            // 0x4feb20 node gate: step-start sphere vs the map node's record
            // box (no sweep).
            // INVENTED: boxes are not rotated (shipped grounds have identity
            // rotation).
            auto misses = [&](const Vec3 &centre, const Vec3 &h) {
                const Vec3 c = gp.pos + mat_vec(gr, centre);
                return fabsf(cp0.pos.y - c.y) > h.y + br ||
                       fabsf(cp0.pos.x - c.x) > h.x + br ||
                       fabsf(cp0.pos.z - c.z) > h.z + br;
            };
            for (size_t ni = 0; ni < gm.map_nodes.size();) {
                const CollisionMesh::MapNode &mn = gm.map_nodes[ni];
                // Speed only: the subtree's union box encloses every node box
                // in it, so a miss here is a miss for each of them.
                if (mn.end > ni && misses(mn.sub_centre, mn.sub_half)) {
                    ni = mn.end;
                    continue;
                }
                ni++;
                if (misses(mn.centre, mn.half))
                    continue;
                for (uint32_t ti = mn.first; ti < mn.first + mn.count; ti++)
                    collect_face_candidate(gm, gp, gr, stored_normals, (int)ci, ti,
                                           cp0, cp1, cr1, br, half, out);
            }
            continue;
        }
        uint32_t stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const CollisionMesh::Node &node = gm.nodes[stack[--sp]];
            // 0x4feb20 node test, swept over the step.
            bool miss = false;
            for (int k = 0; k < 3; k++) {
                float lo = fminf(vec_axis(cp0.pos, k), vec_axis(cp1.pos, k));
                float hi = fmaxf(vec_axis(cp0.pos, k), vec_axis(cp1.pos, k));
                if (hi < vec_axis(node.min, k) - br || lo > vec_axis(node.max, k) + br)
                    miss = true;
            }
            if (miss) continue;
            if (node.count == 0) {
                if (node.left >= 0 && sp < 64) stack[sp++] = (uint32_t)node.left;
                if (node.right >= 0 && sp < 64) stack[sp++] = (uint32_t)node.right;
                continue;
            }
            for (uint32_t k = 0; k < node.count; k++)
                collect_face_candidate(gm, gp, gr, stored_normals, (int)ci,
                                       gm.order[node.first + k], cp0, cp1, cr1, br,
                                       half, out);
        }
    }
}

static bool hull_contact_exists(const World &w, const std::vector<Contact> &next,
                                const RigidBody *a, const RigidBody *g, int child, int tri)
{
    for (const Contact &c : w.contacts)
        if (c.a == a && c.b == g && c.child_index == child && c.soup_triangle == tri)
            return true;
    for (const Contact &c : next)
        if (c.a == a && c.b == g && c.child_index == child && c.soup_triangle == tri)
            return true;
    return false;
}

// INVENTED: harness counter read only by vmboot (card 165): contacts created
// through the 0x4fc7e0 deep branch. The exe has no such counter.
static unsigned g_hull_deep_contacts = 0;
unsigned hull_deep_contacts() { return g_hull_deep_contacts; }

// 0x4fc7e0 Collide__candidatesTOI: 10-step bisection of the sweep from the
// integrated pose (t = 1) back toward the step start (t = 0), warm-started from
// the cached features; creates at most one contact per body.
static void consider_hull_pair(const World &w, RigidBody &a, RigidBody &g,
                               const BodyState &start, std::vector<Contact> &next)
{
    std::vector<HullCandidate> cands;
    collect_hull_candidates(a, start, g, cands);
    if (cands.empty())
        return;
    // PairCache 0x4f6ea0: features of an existing contact for this pair.
    for (HullCandidate &c : cands)
        for (const Contact &old : w.contacts)
            if (old.a == &a && old.b == &g && old.child_index == c.child &&
                old.soup_triangle == c.tri) {
                c.fa = old.feature_a;
                c.fb = old.feature_b;
                break;
            }

    float t = 1.0f, step = 0.5f;
    int iter = 0;
    HullCandidate *best = nullptr;
    for (HullCandidate &c : cands) {
        evaluate_candidate(c, a, start, t);
        if (!best || c.d < best->d) best = &c;
        if (best->d <= 0.0f) break;
    }
    while (best) {
        if (best->d > 0.0f) {
            if (best->d < kAddSlop &&
                (iter == 0 || hull_approaching(*best, a, start, t)))
                break;
            if (iter == 0 && best->d >= kAddSlop)
                break;  // no contact
            t += step;
        } else {
            t -= step;
        }
        step *= 0.5f;
        if (++iter >= 10) break;
        if (best->d <= 0.0f) {
            evaluate_candidate(*best, a, start, t);
            if (best->d <= 0.0f) continue;  // next loop turn
            best->marked = true;
        }
        for (HullCandidate &c : cands) {
            if (c.marked) {  // keep its d, clear the mark
                c.marked = false;
                continue;
            }
            evaluate_candidate(c, a, start, t);
        }
        best = nullptr;
        for (HullCandidate &c : cands) {
            if (!best || c.d < best->d) best = &c;
            if (best->d <= 0.0f) break;
        }
    }
    if (!best || best->d > kAddSlop)
        return;
    if (best->d <= 0.0f)
        deep_candidate(*best, a, start, t);
    const HullCandidate &c = *best;
    if (hull_contact_exists(w, next, &a, &g, c.child, c.tri))
        return;  // 0x502830 rejects duplicates (same child, triangle)
    const RigidBody::CollisionChild &cc = a.collision_children[c.child];
    const Pose cp = child_pose_at(a, cc, start, t);
    const Mat34 gr = g.rotation();
    const Pose gp = g.pose();
    Vec3 paw = cp.pos + mat_vec(pose_rotation(cp), c.pa);
    Contact ct;
    ct.a = &a;
    ct.b = &g;
    ct.normal = c.nn;
    ct.separation = c.d;
    ct.world_point = (paw + c.pb) * 0.5f;
    // Lift the child witness into the root frame (notes/11 §4, child+0x1c).
    ct.local_a = cc.pose.pos + mat_vec(pose_rotation(cc.pose), c.pa);
    ct.local_b = mat_tvec(gr, c.pb - gp.pos);
    ct.friction = cc.shape.friction < g.def.shape.friction ? cc.shape.friction
                                                           : g.def.shape.friction;
    ct.restitution = 0.5f * (cc.shape.restitution + g.def.shape.restitution);
    ct.feature_a = c.fa;
    ct.feature_b = c.fb;
    ct.child_index = c.child;
    ct.soup_triangle = c.tri;
    if (c.deep) g_hull_deep_contacts++;
    next.push_back(ct);
}

// Event phase H of 0x502060 (0x4fc610 -> 0x4feb20 -> 0x4fc7e0): create new
// hull-vs-soup contacts on the integrated pose. Returns how many were added.
static size_t add_hull_contacts(World &w, const std::vector<StepState> &states)
{
    std::vector<Contact> next;
    for (RigidBody *a : w.bodies) {
        if (!is_dynamic(*a) || a->collision_children.empty())
            continue;
        const StepState *st = nullptr;
        for (const StepState &s : states)
            if (s.body == a) {
                st = &s;
                break;
            }
        if (!st) continue;
        for (RigidBody *g : w.statics) {
            if (g->def.shape.type != SHAPE_MESH || !g->def.shape.mesh)
                continue;
            consider_hull_pair(w, *a, *g, st->start, next);
        }
    }
    size_t added = next.size();
    w.contacts.insert(w.contacts.end(), next.begin(), next.end());
    return added;
}

// Phys__shapeDistance 0x50d9d0 refresh of an existing hull contact as
// PhysicsWorld__updateContacts 0x4f9af0 runs it (notes/11 §"Contact refresh"):
// recompute features/points/normal at the integrated states every time, drop
// the pair on a 0.0 result or beyond the keep slop, bisect the step sweep when
// it is penetrating now but was not deep last refresh, and store the world
// point from A's witness. No plane fallback: the two-face triangle Poly is
// two-sided, so a hull that crossed it still gets a signed distance.
static void refresh_hull_contacts(World &w, RigidBody &a, RigidBody &g,
                                  const BodyState &start_a, std::vector<Contact> &next)
{
    const CollisionMesh &gm = *g.def.shape.mesh;
    const Pose gp = g.pose();
    const Mat34 gr = g.rotation();
    // rA = rB = 0 for mesh-vs-soup (no shape margin); the exe's margin terms
    // ptA -= rA*R_A^T n and ptB += rB*R_B^T n are therefore no-ops here.
    const float r = 0.0f;
    for (const Contact &old : w.contacts) {
        if (old.a != &a || old.b != &g || old.child_index < 0)
            continue;
        if (old.child_index >= (int)a.collision_children.size() ||
            old.soup_triangle < 0 || old.soup_triangle >= (int)gm.triangles.size())
            continue;
        const RigidBody::CollisionChild &cc = a.collision_children[old.child_index];
        if (cc.shape.type != SHAPE_MESH || !cc.shape.mesh || !cc.poly)
            continue;
        const CollisionMesh::Triangle &tri = gm.triangles[old.soup_triangle];
        Vec3 vw[3];
        for (int i = 0; i < 3; i++)
            vw[i] = gp.pos + mat_vec(gr, gm.vertices[tri.v[i]]);
        Vec3 n = gm.face_normals.size() == gm.triangles.size()
                     ? mat_vec(gr, gm.face_normals[old.soup_triangle])
                     : vec_cross(vw[1] - vw[0], vw[2] - vw[0]);
        n = vec_normalize(n, Vec3{0.0f, 1.0f, 0.0f});
        Poly face = poly_triangle(vw[0], vw[1], vw[2], n);
        Pose bpose;  // the triangle Poly holds world vertices
        const int fa = old.feature_a, fb = old.feature_b;
        Pose cpose = child_world_pose(a, cc);
        Vec3 pa{0.0f, 0.0f, 0.0f}, pb{0.0f, 0.0f, 0.0f}, cn{0.0f, 1.0f, 0.0f};
        float d = vclip_check(cpose, *cc.poly, bpose, face, fa, fb, &pa, &pb, &cn);
        if (d == 0.0f || d > r + kKeepSlop)
            continue;  // invalid cached features, or apart beyond 0.05 m
        if (d < r + kAddSlop) {
            w.wake(&a);
            w.wake(&g);
            // w+0x39c (converged) is recomputed from the separations in step().
        }
        Contact c = old;
        // Else: deep now and deep at the last refresh -> keep the world point.
        if (d > r || d > -r || old.separation >= r) {
            if (d <= r) {
                // Same 10-step bisection as 0x4fc7e0 (t = 0.5, step = 0.25),
                // interpolating the hull child from the step start to the
                // integrated pose; keep the last check that left the face.
                float t = 0.5f, step = 0.25f;
                for (int iter = 0; iter < 10; iter++) {
                    cpose = child_pose_at(a, cc, start_a, t);
                    d = vclip_check(cpose, *cc.poly, bpose, face, fa, fb, &pa, &pb, &cn);
                    if (d > r) {
                        if (d < r + kAddSlop &&
                            (iter == 0 || vec_dot(cn, cpose.pos - pb) < 0.0f))
                            break;
                        t += step;
                    } else {
                        t -= step;
                    }
                    step *= 0.5f;
                }
                if (d <= r) {
                    // Still penetrating along the sweep: use the integrated states.
                    cpose = child_world_pose(a, cc);
                    d = vclip_check(cpose, *cc.poly, bpose, face, fa, fb, &pa, &pb, &cn);
                }
            }
            c.world_point = cpose.pos + mat_vec(pose_rotation(cpose), pa);
        }
        // Lift the child witness into the root frame (notes/11 §4, child+0x1c).
        c.local_a = cc.pose.pos + mat_vec(pose_rotation(cc.pose), pa);
        c.local_b = mat_tvec(gr, pb - gp.pos);
        c.normal = cn;
        c.separation = d - r;
        next.push_back(c);
    }
}

// PhysicsWorld__updateContacts 0x4f9af0 plus Collide__bodyPair 0x4fb370.
// add_new: the broad phase pass at the start of a step may create pairs; later
// sub-iteration refreshes only update pairs that already exist. `states` holds
// each root's step-start state for the 0x506c00 sweep interpolation.
static void update_contacts(World &w, bool add_new, const std::vector<StepState> &states)
{
    std::vector<Contact> next;
    for (size_t i = 0; i < w.bodies.size(); i++) {
        RigidBody &a = *w.bodies[i];
        BodyState start_a{a.pos, a.vel, a.angular_momentum, a.rot};
        for (const StepState &s : states)
            if (s.body == &a) {
                start_a = s.start;
                break;
            }
        for (size_t j = i + 1; j < w.bodies.size(); j++)
            consider_pair(w, a, *w.bodies[j], add_new, next);
        // Flag-4 bodies sit outside the active root list (0x4f7830); pair them
        // with every active root. The shape 0/7 filter drops the ground itself.
        for (size_t j = 0; j < w.statics.size(); j++)
            consider_pair(w, a, *w.statics[j], add_new, next);
        // Collide__bodyVsTriangles 0x4feb20: a compound root's child hulls
        // collide with a static triangle soup. Existing pairs are refreshed
        // here (0x4f9af0); new ones are created in the step's event phase.
        // INVENTED: merged soup built in groundTree walk order with the node
        // table; streaming not modelled (no collision effect for cars,
        // notes/24).
        if (is_dynamic(a) && !a.collision_children.empty())
            for (RigidBody *g : w.statics) {
                if (g->def.shape.type != SHAPE_MESH || !g->def.shape.mesh)
                    continue;
                refresh_hull_contacts(w, a, *g, start_a, next);
            }
    }
    w.contacts.swap(next);
}

// ---- Trigger contacts (0x502d60 / 0x4fd910 / 0x4fa900 / 0x4fb070, card 168) ----

// INVENTED/shortcut: the exe's Poly__buildBox yields 26 features (8 V, 12 E,
// 6 F quads); src builds the same box from 12 triangles through
// poly_from_triangles 0x50e6d0 (36 features). Upgrade if trigger V-Clip shows
// overruns.
static std::shared_ptr<const Poly> box_poly(const Vec3 &h)
{
    CollisionMesh m;
    m.vertices = {Vec3{-h.x, -h.y, -h.z}, Vec3{h.x, -h.y, -h.z},
                  Vec3{h.x, h.y, -h.z},  Vec3{-h.x, h.y, -h.z},
                  Vec3{-h.x, -h.y, h.z}, Vec3{h.x, -h.y, h.z},
                  Vec3{h.x, h.y, h.z},   Vec3{-h.x, h.y, h.z}};
    static const uint32_t kTris[12][3] = {
        {0, 2, 1}, {0, 3, 2},  // -z
        {4, 5, 6}, {4, 6, 7},  // +z
        {0, 1, 5}, {0, 5, 4},  // -y
        {3, 7, 6}, {3, 6, 2},  // +y
        {0, 4, 7}, {0, 7, 3},  // -x
        {1, 2, 6}, {1, 6, 5},  // +x
    };
    for (const auto &t : kTris) {
        CollisionMesh::Triangle tri;
        for (int i = 0; i < 3; i++) tri.v[i] = t[i];
        m.triangles.push_back(tri);
    }
    return std::make_shared<const Poly>(poly_from_triangles(m));
}

// 0x502d60 PhysicsWorld__addTriggerContact(A = sensor, B = other): one record
// per (sensor, other root); an existing record only refreshes its stamp.
static void add_trigger_contact(World &w, RigidBody &sensor, RigidBody &other)
{
    if (!(sensor.body_flags & 0x1000u)) {
        // shortcut: the no-record path queues EVENT_TRIGGER_ON with a
        // c_collision param; only wheel sensor capsules take it and no shipped
        // race script reads such an event.
        return;
    }
    RigidBody *root = &other;
    while ((root->body_flags & 4u) && root->parent) root = root->parent;
    for (World::TriggerRecord &rec : w.triggers)
        if (rec.a == &sensor && rec.b == root && rec.owner_id == root->owner_id) {
            rec.stamp = w.time;
            return;
        }
    w.triggers.push_back(World::TriggerRecord{&sensor, root, root->owner_id, w.time});
    if (w.trigger_event) w.trigger_event(&sensor, root, 0x20);
    if (root->owner_id && w.trigger_event) w.trigger_event(root, &sensor, 0x20);
}

// 0x4fb070: EVENT_TRIGGER_OFF (0x40) to both owners, then unlink and delete.
static void remove_trigger(World &w, size_t index)
{
    World::TriggerRecord rec = w.triggers[index];
    if (w.trigger_event) {
        if (rec.a) w.trigger_event(rec.a, rec.b, 0x40);
        if (rec.b && rec.owner_id) w.trigger_event(rec.b, rec.a, 0x40);
    }
    w.triggers.erase(w.triggers.begin() + (long)index);
}

// 0x4fa900 trigger sweep (PhysicsWorld__step finish pass): a record whose last
// touch is older than two steps survives only while the bodies' bounding
// spheres still overlap.
static void sweep_triggers(World &w)
{
    for (size_t i = 0; i < w.triggers.size();) {
        const World::TriggerRecord &rec = w.triggers[i];
        if (!rec.a || !rec.b) {  // 0x4fa900: a missing side removes the record
            remove_trigger(w, i);
            continue;
        }
        if (rec.stamp < w.time - 2.0f * w.last_dt) {
            Vec3 dp = rec.a->pos - rec.b->pos;
            float reach = rec.a->def.shape.bounding_radius() +
                          rec.b->def.shape.bounding_radius() + 0.05f;
            if (vec_dot(dp, dp) >= reach * reach) {
                remove_trigger(w, i);
                continue;
            }
        }
        i++;
    }
}

// Collide__bodyPair 0x4fb370 step 4 -> 0x4fd910: the sensor shape against
// every collision child of a dynamic root, d < 0 (no margins for a box or a
// kind-6 hull) -> 0x502d60.
// shortcut: the exe runs this inside the contact generation of every
// sub-iteration; src tests the final pose once per step after the event phase.
// Sphere sensors (0x4fd910 sphere): contact when d < r (rB = 0 for hulls);
// rod sensors are skipped.
static void trigger_pass(World &w)
{
    for (RigidBody *b : w.bodies) {
        if (b->asleep || !is_dynamic(*b) || b->collision_children.empty()) continue;
        for (RigidBody *a : w.bodies) {
            if (!(a->body_flags & 0x40u) || a == b) continue;
            const bool sphere = a->def.shape.type == SHAPE_SPHERE;
            if (!sphere && a->def.shape.type != SHAPE_BOX) continue;
            if (!a->sensor_poly) {
                if (sphere) {
                    // Poly__buildPoint: one vertex at the body origin.
                    Poly pt;
                    pt.verts = {Vec3{0.0f, 0.0f, 0.0f}};
                    pt.features.emplace_back();  // POLY_V, vertex 0, no planes
                    a->sensor_poly = std::make_shared<const Poly>(std::move(pt));
                } else {
                    a->sensor_poly = box_poly(a->def.shape.half_extents);
                }
            }
            const Pose ap = a->pose();
            for (const RigidBody::CollisionChild &cc : b->collision_children) {
                if (!cc.poly) continue;
                const Pose cp = child_world_pose(*b, cc);
                Vec3 dp = cp.pos - ap.pos;
                float reach = a->def.shape.bounding_radius() +
                              cc.shape.bounding_radius() + 0.05f;
                if (vec_dot(dp, dp) > reach * reach) continue;
                int fa = 0, fb = 0;
                Vec3 pa{0.0f, 0.0f, 0.0f}, pb{0.0f, 0.0f, 0.0f};
                Vec3 n{0.0f, 1.0f, 0.0f};
                if (vclip_closest(ap, *a->sensor_poly, cp, *cc.poly, &fa, &fb, &pa, &pb,
                                  &n) < (sphere ? a->def.shape.radius : 0.0f))
                    add_trigger_contact(w, *a, *b);
            }
        }
    }
    sweep_triggers(w);
}

// 0x504180 PhysBody__updateBounds per record, then 0x503c20
// GameObject__boundsFromInstances unions their swept boxes into the root's
// object box (O+0xb0..+0xc8). Nothing reads the box yet.
static void update_object_boxes(const std::vector<StepState> &states)
{
    for (const StepState &s : states) {
        RigidBody &b = *s.body;
        if (b.inst_records.empty() || b.asleep || !is_dynamic(b)) continue;
        const Mat34 r0 = pose_rotation(Pose{s.start.pos, s.start.rot});
        const Mat34 r1 = b.rotation();
        bool first = true;
        Vec3 lo{0.0f, 0.0f, 0.0f}, hi{0.0f, 0.0f, 0.0f};
        for (const RigidBody::InstRecord &r : b.inst_records) {
            const Vec3 p0 = s.start.pos + mat_vec(r0, r.offset);
            const Vec3 p1 = b.pos + mat_vec(r1, r.offset);
            const Vec3 c = (p0 + p1) * 0.5f;
            const Vec3 h{fabsf(p1.x - p0.x) * 0.5f + r.br,
                         fabsf(p1.y - p0.y) * 0.5f + r.br,
                         fabsf(p1.z - p0.z) * 0.5f + r.br};
            const Vec3 rlo = c - h, rhi = c + h;
            if (first) {
                lo = rlo;
                hi = rhi;
                first = false;
            } else {
                lo = Vec3{fminf(lo.x, rlo.x), fminf(lo.y, rlo.y), fminf(lo.z, rlo.z)};
                hi = Vec3{fmaxf(hi.x, rhi.x), fmaxf(hi.y, rhi.y), fmaxf(hi.z, rhi.z)};
            }
        }
        if (!first) { // no records: O keeps its old box
            b.obj_kind = 1;
            b.obj_half = (hi - lo) * 0.5f;
            b.obj_centre = lo + b.obj_half;
        }
    }
}

// 0x4fc610 dispatch -> 0x4ee460 collidePass -> 0x4fe6f0 groundTree, map node part.
// INVENTED order: DFS over map_nodes instead of the interval manager's SAP order
// (same overlapped set, notes/36 §1).
// shortcut: Collide__groundPrefetch (+5 m box, async residency) not ported.
// shortcut: node unload is not ported, and the awake type-2 child half of 0x4f9750 is
// approximated by !asleep (notes/36).
static void walk_map_nodes(World &w, const std::vector<StepState> &states)
{
    if (!w.map_node_touch) return;
    for (const StepState &s : states) {
        const RigidBody &b = *s.body;
        if (b.inst_records.empty() || b.asleep || !is_dynamic(b) || b.obj_kind != 1) continue;
        if (vec_dot(b.obj_half, b.obj_half) > 10000.0f) continue;  // 0x4f9750 size part
        for (RigidBody *g : w.statics) {
            if (!g->def.shape.mesh || g->def.shape.mesh->map_nodes.empty()) continue;
            const std::vector<CollisionMesh::MapNode> &nodes = g->def.shape.mesh->map_nodes;
            const Vec3 gp = g->pose().pos;
            const Mat34 gr = g->rotation();
            for (size_t ni = 0; ni < nodes.size();) {
                const Vec3 c = gp + mat_vec(gr, nodes[ni].centre);
                const Vec3 h = nodes[ni].half;
                if (b.obj_half.x + h.x < fabsf(c.x - b.obj_centre.x) ||
                    b.obj_half.z + h.z < fabsf(c.z - b.obj_centre.z) ||
                    b.obj_half.y + h.y < fabsf(c.y - b.obj_centre.y)) {
                    ni = nodes[ni].end > ni ? nodes[ni].end : ni + 1;  // children skipped
                    continue;
                }
                w.map_node_touch(g, ni);
                ni++;
            }
        }
    }
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

    // 0x502060 C: drive callbacks run inside the step, after the force pass
    // (A) and the controllers (B), before the first integrate (D). Their
    // forces land in the accumulators RK4 holds constant for the whole step.
    if (drive) drive(h);

    update_contacts(*this, true, states);

    // 0x502060: integrate every root once, then sub-iterate. The exe skips
    // bodies whose flags bit0 is still set; re-integrating them from unchanged
    // inputs gives the same state.
    for (StepState &s : states)
        integrate_body(*s.body, s.start, h);
    int iterations = 0;
    for (int it = 0; it < 20; it++) {
        iterations = it + 1;
        update_contacts(*this, false, states); // contact refresh, no new pairs mid-step
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
        // 0x502060 H: the event phase creates new collision pairs when the
        // iteration converged (or at iteration 19). New hull-vs-soup contacts
        // re-open the loop like PhysicsWorld__resolveImpacts 0x4ffd20 does.
        // shortcut: resolveImpacts (mode 6 -> 7 before resolve) is not ported;
        // the added contacts set converged = false directly.
        if (converged || it == 19) {
            // 0x502060 H: purgeEvents/boundsFromInstances before dispatch.
            update_object_boxes(states);
            walk_map_nodes(*this, states);
            if (add_hull_contacts(*this, states) > 0)
                converged = false;
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
    // 0x502060 tail: 0x4fa900 sweeps the trigger records (0x502d60 creates
    // them, 0x4fb070 removes them).
    trigger_pass(*this);
    time += t; // 0x502060 epilogue: simulated time
    return 1;
}
