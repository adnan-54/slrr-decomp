// physcheck: assert-based self-checks for the physics core (src/physics).
// Covers the documented behaviours the port must preserve: analytic free fall,
// a body resting on a plane, sliding friction coming to rest, stacked bodies
// staying separated, unresolved pairs never inventing contacts, and mesh
// collision (SCX part ray, resting body on a triangle soup, normal orientation).
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include <memory>
#include <string>
#include <utility>

#include "mesh/scx.h"
#include "physics/mesh.h"
#include "physics/physics.h"
#include "physics/raycast.h"

static int failures = 0;

static void check(const char *name, bool ok, const char *detail)
{
    printf("%s  %-40s %s\n", ok ? "PASS" : "FAIL", name, detail);
    fflush(stdout);
    if (!ok)
        failures++;
}

static void add_ground(World &w, RigidBody *plane)
{
    Shape s;
    s.type = SHAPE_PLANE;
    plane->set_static_shape(s);
    plane->pos = vec3(0.0f, 0.0f, 0.0f);
    w.add_body(plane);
}

// Flat 4 m quad from two triangles, wound so the face normal points +Y.
// SCX positions are file centimetres; the builder converts to metres.
static std::shared_ptr<const CollisionMesh> flat_mesh()
{
    const float h = 2.0f / mesh::kCollisionScale;
    mesh::Section section;
    section.positions = {{-h, 0.0f, -h}, {h, 0.0f, -h}, {h, 0.0f, h}, {-h, 0.0f, h}};
    section.indices = {0, 2, 1, 0, 3, 2};
    mesh::Mesh parsed;
    parsed.sections.push_back(std::move(section));
    return build_collision_mesh(parsed);
}

static void add_mesh_ground(World &w, RigidBody *body,
                            const std::shared_ptr<const CollisionMesh> &cm)
{
    Shape s;
    s.type = SHAPE_MESH;
    s.mesh = cm;
    body->set_static_shape(s);
    body->pos = vec3(0.0f, 0.0f, 0.0f);
    w.add_body(body);
}

static Vec3 mesh_point(const mesh::Vec3 &p)
{
    return vec3(p.x * mesh::kCollisionScale, p.y * mesh::kCollisionScale,
                p.z * mesh::kCollisionScale);
}

static void test_free_fall()
{
    World w;
    RigidBody body;
    body.set_sphere(0.5f, 2.0f);
    body.def.linear_drag = 0.0f;
    body.def.angular_drag = 0.0f;
    body.pos = vec3(0.0f, 10.0f, 0.0f);
    w.add_body(&body);

    const float dt = 0.02f;
    const int n = 50;
    for (int i = 0; i < n; i++)
        w.step(dt);
    const float t = n * dt;
    const float want_y = 10.0f - 0.5f * 9.81f * t * t;
    const float want_v = -9.81f * t;
    bool ok = fabsf(body.pos.y - want_y) < 1e-3f && fabsf(body.vel.y - want_v) < 1e-3f &&
              fabsf(body.pos.x) < 1e-6f && fabsf(body.pos.z) < 1e-6f;
    char detail[160];
    snprintf(detail, sizeof detail, "y=%.6f want %.6f, vy=%.6f want %.6f", body.pos.y,
                  want_y, body.vel.y, want_v);
    check("free fall matches analytic solution", ok, detail);
}

static void test_resting_on_plane()
{
    World w;
    RigidBody plane;
    add_ground(w, &plane);
    RigidBody box;
    box.set_box(vec3(0.5f, 0.5f, 0.5f), 1.0f);
    box.def.linear_drag = 0.0f;
    box.def.angular_drag = 0.0f;
    box.pos = vec3(0.0f, 0.5f, 0.0f);
    w.add_body(&box);

    float min_y = 1e9f, max_y = -1e9f, max_speed = 0.0f;
    for (int i = 0; i < 2000; i++) {
        w.step(0.02f);
        if (i > 200) { // ignore the settling phase
            if (box.pos.y < min_y)
                min_y = box.pos.y;
            if (box.pos.y > max_y)
                max_y = box.pos.y;
            float speed = vec_length(box.vel);
            if (speed > max_speed)
                max_speed = speed;
        }
    }
    bool ok = min_y > 0.49f && max_y < 0.56f && max_speed < 0.05f && fabsf(box.pos.x) < 1e-3f &&
              fabsf(box.pos.z) < 1e-3f && w.last_iterations <= 20;
    char detail[192];
    snprintf(detail, sizeof detail, "y in [%.4f, %.4f], |v|max=%.4f, x=%.5f, z=%.5f",
                  min_y, max_y, max_speed, box.pos.x, box.pos.z);
    check("resting box stays on plane", ok, detail);
}

static void test_sliding_friction()
{
    World w;
    RigidBody plane;
    add_ground(w, &plane);
    RigidBody box;
    box.set_box(vec3(0.5f, 0.5f, 0.5f), 1.0f);
    box.def.linear_drag = 0.0f; // isolate solver friction from the drag force
    box.def.angular_drag = 0.0f;
    box.pos = vec3(0.0f, 0.5f, 0.0f);
    box.vel = vec3(2.0f, 0.0f, 0.0f);
    w.add_body(&box);

    float speed_early = 2.0f;
    for (int i = 1; i <= 300; i++) {
        w.step(0.02f);
        if (i == 20)
            speed_early = vec_length(box.vel);
    }
    const float x_stopped = box.pos.x;
    for (int i = 0; i < 100; i++)
        w.step(0.02f);
    float final_speed = vec_length(box.vel);
    bool ok = speed_early < 1.9f && final_speed < 0.05f && box.pos.x > 0.05f &&
              fabsf(box.pos.x - x_stopped) < 0.01f;
    char detail[192];
    snprintf(detail, sizeof detail,
                  "|v|@0.4s=%.3f, |v|final=%.4f, x=%.4f (drift %.5f)", speed_early, final_speed,
                  box.pos.x, box.pos.x - x_stopped);
    check("sliding with friction decelerates and stops", ok, detail);
}

static void test_stacked_boxes()
{
    World w;
    RigidBody plane;
    add_ground(w, &plane);
    RigidBody lower, upper;
    lower.set_box(vec3(0.5f, 0.5f, 0.5f), 1.0f);
    upper.set_box(vec3(0.5f, 0.5f, 0.5f), 1.0f);
    for (RigidBody *b : {&lower, &upper}) {
        b->def.linear_drag = 0.0f;
        b->def.angular_drag = 0.0f;
    }
    lower.pos = vec3(0.0f, 0.5f, 0.0f);
    upper.pos = vec3(0.0f, 1.5f, 0.0f);
    w.add_body(&lower);
    w.add_body(&upper);

    float worst_penetration = 0.0f;
    for (int i = 0; i < 2000; i++) {
        w.step(0.02f);
        float gap = upper.pos.y - lower.pos.y;
        if (1.0f - gap > worst_penetration)
            worst_penetration = 1.0f - gap;
    }
    bool ok = lower.pos.y > 0.49f && lower.pos.y < 0.57f && upper.pos.y > 1.49f &&
              upper.pos.y < 1.60f && worst_penetration < 0.02f &&
              fabsf(lower.pos.x) < 1e-3f && fabsf(upper.pos.x) < 1e-3f &&
              fabsf(lower.pos.z) < 1e-3f && fabsf(upper.pos.z) < 1e-3f &&
              vec_length(lower.vel) < 0.05f && vec_length(upper.vel) < 0.05f;
    char detail[192];
    snprintf(detail, sizeof detail, "lower y=%.4f, upper y=%.4f, gap=%.4f, worst pen=%.4f",
                  lower.pos.y, upper.pos.y, upper.pos.y - lower.pos.y, worst_penetration);
    check("stacked boxes stay separated", ok, detail);
}

static bool load_door_mesh(mesh::Mesh *out, std::string *err)
{
    const char *paths[] = {
        "../bin/the game/cars/racers/Axis_data/meshes/_phys_FL_door.scx",
        "bin/the game/cars/racers/Axis_data/meshes/_phys_FL_door.scx",
    };
    for (const char *path : paths)
        if (mesh::load(path, out, err))
            return true;
    return false;
}

static void test_ray_part_mesh()
{
    // _phys_FL_door.scx: v3, 16 vertices / 28 triangles, AABB in metres from
    // notes/11 §2. The first triangle's face centroid is the known hit point.
    mesh::Mesh parsed;
    std::string err;
    if (!load_door_mesh(&parsed, &err)) {
        check("ray hits decoded part mesh", false, err.c_str());
        return;
    }
    std::shared_ptr<const CollisionMesh> cm = build_collision_mesh(parsed);
    bool ok = cm && parsed.vertex_count() == 16 && parsed.triangle_count() == 28 &&
              cm->triangles.size() == 28;
    if (ok) {
        ok = fabsf(cm->bounds_min.x + 0.116f) < 1e-3f &&
             fabsf(cm->bounds_max.x - 0.105f) < 1e-3f &&
             fabsf(cm->bounds_min.y + 0.321f) < 1e-3f &&
             fabsf(cm->bounds_max.y - 0.316f) < 1e-3f &&
             fabsf(cm->bounds_min.z + 0.602f) < 1e-3f &&
             fabsf(cm->bounds_max.z - 0.549f) < 1e-3f;
    }
    Vec3 centroid{0.0f, 0.0f, 0.0f}, normal{0.0f, 1.0f, 0.0f};
    if (ok) {
        const mesh::Section &s = parsed.sections[0];
        uint32_t i0 = s.indices.empty() ? s.indices16[0] : s.indices[0];
        uint32_t i1 = s.indices.empty() ? s.indices16[1] : s.indices[1];
        uint32_t i2 = s.indices.empty() ? s.indices16[2] : s.indices[2];
        Vec3 a = mesh_point(s.positions[i0]), b = mesh_point(s.positions[i1]),
             c = mesh_point(s.positions[i2]);
        centroid = (a + b + c) * (1.0f / 3.0f);
        normal = vec_normalize(vec_cross(b - a, c - a), Vec3{0.0f, 1.0f, 0.0f});
        Vec3 origin = centroid + normal * 0.5f;
        MeshRayHit hit;
        ok = mesh_raycast(*cm, Pose{}, origin, -normal, 2.0f, &hit) &&
             fabsf(hit.distance - 0.5f) < 1e-3f &&
             vec_length(hit.point - centroid) < 1e-3f &&
             vec_dot(hit.normal, normal) > 0.999f;
        // raycast_body wiring for kind-6 bodies
        RigidBody body;
        Shape shape;
        shape.type = SHAPE_MESH;
        shape.mesh = cm;
        body.set_static_shape(shape);
        RayHit rh;
        ok = ok && raycast_body(body, origin, -normal, 2.0f, &rh) &&
             fabsf(rh.distance - 0.5f) < 1e-3f &&
             vec_length(rh.point - centroid) < 1e-3f;
    }
    char detail[192];
    snprintf(detail, sizeof detail, "verts=%zu tris=%zu hit=(%.4f, %.4f, %.4f)", cm ? cm->vertices.size() : 0,
                  cm ? cm->triangles.size() : 0, centroid.x, centroid.y, centroid.z);
    check("ray hits decoded part mesh", ok, detail);
}

static void test_resting_box_on_mesh()
{
    auto cm = flat_mesh();
    World w;
    RigidBody ground;
    add_mesh_ground(w, &ground, cm);
    RigidBody box;
    box.set_box(vec3(0.5f, 0.5f, 0.5f), 1.0f);
    box.def.linear_drag = 0.0f;
    box.def.angular_drag = 0.0f;
    box.pos = vec3(0.0f, 0.5f, 0.0f);
    w.add_body(&box);

    float min_y = 1e9f, max_y = -1e9f, max_speed = 0.0f;
    for (int i = 0; i < 2000; i++) {
        w.step(0.02f);
        if (i > 200) { // ignore the settling phase
            if (box.pos.y < min_y)
                min_y = box.pos.y;
            if (box.pos.y > max_y)
                max_y = box.pos.y;
            float speed = vec_length(box.vel);
            if (speed > max_speed)
                max_speed = speed;
        }
    }
    bool ok = cm && min_y > 0.49f && max_y < 0.56f && max_speed < 0.05f &&
              fabsf(box.pos.x) < 1e-3f && fabsf(box.pos.z) < 1e-3f &&
              w.last_iterations <= 20;
    char detail[192];
    snprintf(detail, sizeof detail, "y in [%.4f, %.4f], |v|max=%.4f, x=%.5f, z=%.5f", min_y,
                  max_y, max_speed, box.pos.x, box.pos.z);
    check("resting box stays on triangle mesh", ok, detail);
}

static void test_resting_sphere_on_mesh()
{
    auto cm = flat_mesh();
    World w;
    RigidBody ground;
    add_mesh_ground(w, &ground, cm);
    RigidBody sphere;
    sphere.set_sphere(0.5f, 1.0f);
    sphere.def.linear_drag = 0.0f;
    sphere.def.angular_drag = 0.0f;
    sphere.pos = vec3(0.0f, 0.5f, 0.0f);
    w.add_body(&sphere);

    float min_y = 1e9f, max_y = -1e9f, max_speed = 0.0f;
    for (int i = 0; i < 2000; i++) {
        w.step(0.02f);
        if (i > 200) {
            if (sphere.pos.y < min_y)
                min_y = sphere.pos.y;
            if (sphere.pos.y > max_y)
                max_y = sphere.pos.y;
            float speed = vec_length(sphere.vel);
            if (speed > max_speed)
                max_speed = speed;
        }
    }
    bool ok = cm && min_y > 0.49f && max_y < 0.56f && max_speed < 0.05f &&
              fabsf(sphere.pos.x) < 1e-3f && fabsf(sphere.pos.z) < 1e-3f &&
              w.last_iterations <= 20;
    char detail[192];
    snprintf(detail, sizeof detail, "y in [%.4f, %.4f], |v|max=%.4f, x=%.5f, z=%.5f", min_y,
                  max_y, max_speed, sphere.pos.x, sphere.pos.z);
    check("resting sphere stays on triangle mesh", ok, detail);
}

static void test_mesh_normals()
{
    auto cm = flat_mesh();
    Shape mesh_shape;
    mesh_shape.type = SHAPE_MESH;
    mesh_shape.mesh = cm;
    Shape sphere;
    sphere.type = SHAPE_SPHERE;
    sphere.radius = 0.5f;
    Pose mesh_pose, sphere_pose;
    sphere_pose.pos = vec3(0.0f, 0.4f, 0.0f); // 0.1 m penetration

    ShapeHit hits[4];
    int n = collide_shapes(sphere, sphere_pose, mesh_shape, mesh_pose, hits, 4);
    bool ok = cm && n > 0 && hits[0].normal.y > 0.9f && hits[0].separation < 0.0f;
    float mesh_to_sphere_y = hits[0].normal.y;
    // Reversed order: the normal is still B -> A, i.e. downward here.
    n = collide_shapes(mesh_shape, mesh_pose, sphere, sphere_pose, hits, 4);
    ok = ok && n > 0 && hits[0].normal.y < -0.9f;
    // A downward ray's hit normal faces the ray (outward from the face).
    MeshRayHit rh;
    ok = ok && mesh_raycast(*cm, mesh_pose, vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, -1.0f, 0.0f),
                            2.0f, &rh) &&
         rh.normal.y > 0.9f;
    char detail[160];
    snprintf(detail, sizeof detail, "contacts=%d, mesh->sphere ny=%.3f, ray ny=%.3f", n,
                  mesh_to_sphere_y, rh.normal.y);
    check("mesh contact normals point outward", ok, detail);
}

static void test_unresolved_shape_kinds()
{
    // Mesh-mesh has no ported narrow phase: the pair must be counted and emit
    // no contacts (kind 6 against primitives is resolved).
    auto cm = flat_mesh();
    World w;
    RigidBody a, b;
    a.def = BodyDef{};
    a.def.inv_mass = 1.0f;
    a.def.shape.type = SHAPE_MESH;
    a.def.shape.mesh = cm;
    b.def = BodyDef{};
    b.def.inv_mass = 1.0f;
    b.def.shape.type = SHAPE_MESH;
    b.def.shape.mesh = cm;
    b.pos = vec3(0.1f, 0.0f, 0.0f);
    w.add_body(&a);
    w.add_body(&b);
    w.step(0.02f);
    bool ok = shape_kind_resolved(SHAPE_MESH) && w.contacts.empty() &&
              w.unsupported_pairs > 0;
    char detail[128];
    snprintf(detail, sizeof detail, "contacts=%zu unsupported_pairs=%u", w.contacts.size(),
                  w.unsupported_pairs);
    check("mesh-mesh pair emits no contacts", ok, detail);
}

int main()
{
    printf("physcheck: physics core checks\n");
    test_free_fall();
    test_resting_on_plane();
    test_sliding_friction();
    test_stacked_boxes();
    test_ray_part_mesh();
    test_resting_box_on_mesh();
    test_resting_sphere_on_mesh();
    test_mesh_normals();
    test_unresolved_shape_kinds();
    if (failures != 0) {
        fprintf(stderr, "physcheck: %d check(s) FAILED\n", failures);
        assert(failures == 0 && "physcheck failures");
        return 1;
    }
    printf("physcheck: all checks passed\n");
    return 0;
}
