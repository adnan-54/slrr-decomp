// Rigid-body physics core, ported from notes/08-physics.md and
// notes/11-collision-shapes.md. Original addresses are quoted per block.
// Generic engine naming: no game prefix; math types come from engine/math.h.
#pragma once
#include <stdint.h>

#include <memory>
#include <vector>

#include "engine/math.h"

struct Vec3 {
    float x, y, z;
};

static inline Vec3 vec3(float x, float y, float z) { return Vec3{x, y, z}; }
static inline Vec3 operator+(Vec3 a, Vec3 b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline Vec3 operator-(Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline Vec3 operator-(Vec3 a) { return Vec3{-a.x, -a.y, -a.z}; }
static inline Vec3 operator*(Vec3 a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }
static inline Vec3 operator*(float s, Vec3 a) { return Vec3{a.x * s, a.y * s, a.z * s}; }
static inline Vec3 &operator+=(Vec3 &a, Vec3 b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
static inline Vec3 &operator-=(Vec3 &a, Vec3 b) { a.x -= b.x; a.y -= b.y; a.z -= b.z; return a; }
static inline float vec_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline Vec3 vec_cross(Vec3 a, Vec3 b)
{
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static inline float vec_length(Vec3 a) { return sqrtf(vec_dot(a, a)); }
static inline Vec3 vec_normalize(Vec3 a, Vec3 fallback)
{
    float n = vec_length(a);
    return n > 1e-8f ? a * (1.0f / n) : fallback;
}

// Row vector times matrix: world = R * local (rows of R are world axes).
static inline Vec3 mat_vec(const Mat34 &m, Vec3 v)
{
    return Vec3{m.m[0] * v.x + m.m[1] * v.y + m.m[2] * v.z,
                m.m[4] * v.x + m.m[5] * v.y + m.m[6] * v.z,
                m.m[8] * v.x + m.m[9] * v.y + m.m[10] * v.z};
}
static inline Vec3 mat_tvec(const Mat34 &m, Vec3 v)
{
    return Vec3{m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z,
                m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z,
                m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z};
}

struct Pose {
    Vec3 pos{0.0f, 0.0f, 0.0f};
    Quat rot{0.0f, 0.0f, 0.0f, 1.0f};
};

static inline Mat34 pose_rotation(const Pose &p)
{
    Quat q = p.rot;
    quat_normalize(&q);
    Mat34 m;
    quat_to_mat(&m, &q);
    return m;
}

struct CollisionMesh;
struct Poly;

// Shape kinds (notes/11 §1, shape+0x08).
// Kind 6 carries a triangle-soup CollisionMesh (physics/mesh.h); kind 7 keeps
// the loaded mesh without the collision polyhedron and is skipped by
// Collide__bodyPair. Shape 8 is an engine extension standing in for the world
// ground/water plane used by the documented body-flag 0x800 path (notes/08
// §Force model); the exe builds a temporary collider for it, not a scene shape.
enum ShapeKind {
    SHAPE_NONE = 0,
    SHAPE_SPHERE = 1,
    SHAPE_CAPSULE_X = 2,
    SHAPE_CAPSULE_Y = 3,
    SHAPE_CAPSULE_Z = 4,
    SHAPE_BOX = 5,
    SHAPE_MESH = 6,          // convex .scx collision mesh (build_collision_mesh)
    SHAPE_MESH_NO_POLY = 7,  // skipped by Collide__bodyPair like shape 0
    SHAPE_PLANE = 8,         // engine extension (see above)
};

struct Shape {
    int type = SHAPE_NONE;
    float radius = 0.0f;              // +0x10: sphere radius / capsule radius
    float half_length = 0.0f;         // +0xb4: capsule half length
    Vec3 half_extents{0.0f, 0.0f, 0.0f}; // +0xb4..+0xbc: box
    Vec3 plane_point{0.0f, 0.0f, 0.0f};  // SHAPE_PLANE, shape-local
    Vec3 plane_normal{0.0f, 1.0f, 0.0f}; // SHAPE_PLANE, shape-local, unit
    std::shared_ptr<const CollisionMesh> mesh; // +0xb8..: SHAPE_MESH geometry
    float friction = 0.6f;            // +0x50
    float rolling_resistance = 0.0f;  // +0x54
    float restitution = 0.2f;         // +0x58

    // notes/11 §1: broad-phase radius; primitives already carry the +0.05 slack.
    float bounding_radius() const;
};

// True for kinds whose distance/contact query is ported below.
bool shape_kind_resolved(int type);

// Body definition (rigidbody_type, notes/11 §1).
struct BodyDef {
    float inv_mass = 0.0f;       // +0x2c; 0 = static
    Mat34 inv_inertia{};         // +0x30: body-frame inverse inertia (3x3)
    Shape shape;                 // +0x90
    float linear_drag = 0.0f;    // +0x98 = -0.005 * mass
    float angular_drag = 0.0f;   // +0x9c = -0.01 * mass
    uint32_t flags = 0;          // +0xb4; bit2 = no integration
};

// Fills inv_mass/inv_inertia/drag from a mass and the shape (notes/11 §1 mass
// formulas). SHAPE_MESH runs Poly__massProps 0x508d20 (Mirtich integrals
// 0x508820) over the hull; the exe shifts the hull vertices to the centroid and
// returns it, so *center receives that offset (callers keep the mesh shared and
// move the body instead). Unresolved shapes stay a point mass (zero inertia).
void compute_mass_props(BodyDef *def, float mass, Vec3 *center = nullptr);

struct RigidBody {
    // INVENTED storage: the exe hangs real child bodies under part_comp and
    // descends them in Collide__bodyVsTriangles 0x4feb20; the port keeps the
    // child shapes and their parent-frame poses on the root instead.
    struct CollisionChild {
        Shape shape;
        Pose pose;                         // child -> parent frame
        std::shared_ptr<const Poly> poly;  // kind-6 V-Clip hull
    };
    std::vector<CollisionChild> collision_children;
    BodyDef def;
    Vec3 pos{0.0f, 0.0f, 0.0f};
    Quat rot{0.0f, 0.0f, 0.0f, 1.0f};  // state s[0x25..0x28]
    Vec3 vel{0.0f, 0.0f, 0.0f};        // state s[3..5]
    Vec3 angular_momentum{0.0f, 0.0f, 0.0f}; // state s[0x1f..0x21]; omega = I^-1 L
    Vec3 pos_correction{0.0f, 0.0f, 0.0f};   // state s[6..8], applied after integration
    Vec3 ext_force{0.0f, 0.0f, 0.0f};  // gravity/drag, applied every integration
    Vec3 ext_torque{0.0f, 0.0f, 0.0f};
    Vec3 force{0.0f, 0.0f, 0.0f};      // solver force (active state +0x8c), kept for the whole step
    Vec3 torque{0.0f, 0.0f, 0.0f};     // solver torque (active state +0xec), kept for the whole step
    // Other-state flags bit0: set by the integration (0x5001c0), cleared at step
    // start (0x4f8460) and by a contact that touches the body (0x4f0dd0).
    bool integrated = false;
    // Compound link (+0x20 parent, +0x24/+0x28 child list). A body with a parent
    // is carried by its root: it lives on the parent's children list, not in
    // World::bodies, and is not integrated on its own.
    RigidBody *parent = nullptr;
    std::vector<RigidBody *> children;
    // PhysBody sleep flag (+0x58 bit 0x80, 0x506d90/0x507060).
    bool asleep = false;

    Pose pose() const { return Pose{pos, rot}; }
    Mat34 rotation() const;
    Mat34 world_inv_inertia() const;
    Vec3 angular_velocity() const;
    Vec3 point_velocity(const Vec3 &p_local) const; // PhysBody__pointVelocity 0x5051c0-ish
    void apply_impulse_at(const Vec3 &impulse, const Vec3 &p_local); // 0x505450
    void apply_force_at(const Vec3 &f, const Vec3 &p_local);        // 0x5051e0

    void set_sphere(float radius, float mass);
    void set_box(const Vec3 &half_extents, float mass);
    void set_capsule(int axis, float half_length, float radius, float mass);
    void set_static_shape(const Shape &s);
};

// Contact query result in world space. separation is the surface distance
// (negative = penetrating), normal points from B to A.
struct ShapeHit {
    Vec3 point_a{0.0f, 0.0f, 0.0f};
    Vec3 point_b{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    float separation = 0.0f;
    int feature_a = 0;
    int feature_b = 0;
};

// Signed distance for the deepest feature pair of a resolved pair. Returns
// false when the pair is unresolved or when the shapes are separated by more
// than the contact slop (only overlap/near-contact witnesses are generated).
bool shape_distance(const Shape &a, const Pose &pa, const Shape &b, const Pose &pb,
                    ShapeHit *out);

// Contact manifold for one pair, up to max_hits points (a box-box or box-plane
// pair can yield four). Only features within 0.02 m of contact are emitted
// (notes/11 §4). Unresolved kinds return 0.
int collide_shapes(const Shape &a, const Pose &pa, const Shape &b, const Pose &pb,
                   ShapeHit *hits, int max_hits);
// Same query with a caller-chosen contact threshold: the exe adds pairs below
// 0.02 m but keeps existing contacts alive until 0.05 m (notes/11 §4/§5).
int collide_shapes_within(const Shape &a, const Pose &pa, const Shape &b, const Pose &pb,
                          ShapeHit *hits, int max_hits, float slop);

// c_collision (notes/11 §5). Modes 6 = new impact, 7 = resting.
struct Contact {
    RigidBody *a = nullptr;
    RigidBody *b = nullptr;
    Vec3 local_a{0.0f, 0.0f, 0.0f};   // +0x74
    Vec3 local_b{0.0f, 0.0f, 0.0f};   // +0x80
    Vec3 normal{0.0f, 1.0f, 0.0f};    // +0x8c, world, B -> A
    Vec3 world_point{0.0f, 0.0f, 0.0f}; // +0x98
    float separation = 0.0f;          // +0xa4, margins already subtracted
    float friction = 0.6f;            // +0xac = min(muA, muB)
    float restitution = 0.2f;         // +0xb0 = (eA + eB) / 2
    int mode = 6;                     // +0x2c
    float normal_impulse = 0.0f;      // +0xb8
    float bias_force = 0.0f;          // +0xbc
    float friction_force = 0.0f;      // +0xc0
    int feature_a = 0;
    int feature_b = 0;
    // Hull-vs-soup contact (Collide__bodyVsTriangles 0x4feb20 /
    // Collide__candidatesTOI 0x4fc7e0): the root's collision child and the
    // soup triangle, kept for the Phys__shapeDistance 0x50d9d0 refresh.
    int child_index = -1;
    int soup_triangle = -1;
};

// PhysicsWorld (notes/08, World::step 0x502060). Fixed iteration order:
//   apply forces per root body, integrate every root -> for each sub-iteration
//   (max 20): refresh contacts; if one is within 0.02 m, resolve every contact
//   in list order (c_collision__resolve 0x4f0dd0); re-integrate every root from
//   the start-of-step state, which carries the solver impulses, forces and
//   position offset; stop when no contact was near or the pass returned 0.
struct World {
    // World+0x00..0x0c, PhysicsWorld__init 0x4fffb0.
    float time = 0.0f;         // +0x00: simulated seconds
    float accumulator = 0.0f;  // +0x04
    float step_size = 0.02f;   // +0x08
    float last_dt = 0.02f;     // +0x0c
    Vec3 gravity{0.0f, -9.81f, 0.0f}; // notes/11 §6 (PhysicsWorld__init 0x4fffb0)
    float air_density = 1.2928f;      // +0x364 = 0x3fa57a78, buoyancy path (body def flag 0x80) TODO
    std::vector<RigidBody *> bodies;  // root bodies (active root, w+0x84)
    std::vector<RigidBody *> sleeping; // sleeping root bodies (w+0x88)
    // INVENTED: flag-4 (static / no-integration) bodies are never linked under
    // the exe's active root (0x4f7830); src keeps them here to own and pair them.
    std::vector<RigidBody *> statics;
    int body_count = 0;                // w+0x54, incremented by add_body
    std::vector<Contact> contacts;    // world+0x1d8
    // 0x502060 order A-D: apply forces, controllers, drive callbacks, integrate.
    void (*drive)(float dt) = nullptr; // 0x47be30 Game__runDriveCallbacks
    int last_iterations = 0;
    unsigned unsupported_pairs = 0;   // pairs with a TODO shape kind, no contacts

    void add_body(RigidBody *b);
    // 0x501d00: remove from the list/parent that holds it. 0x4f7b20: head
    // insert into a parent's child list.
    void unlink(RigidBody *b);
    void attach_child(RigidBody *child, RigidBody *parent);
    // 0x506d90 / 0x507060: only root-level bodies change lists; children are
    // recursed.
    void sleep(RigidBody *b);
    void wake(RigidBody *b);
    int step(float dt);
};

// The process-wide world (g_physWorld, 0x60251c): the exe creates it at engine
// startup, before any script runs (PhysicsWorld__init 0x4fffb0).
World &process_world();
