// physcheck: assert-based self-checks for the physics core (src/physics).
// Covers the documented behaviours the port must preserve: analytic free fall,
// a body resting on a plane, sliding friction coming to rest, stacked bodies
// staying separated, and unresolved shape kinds never inventing contacts.
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "physics/physics.h"

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

static void test_unresolved_shape_kinds()
{
    // Mesh (type 6) is documented but its loader/V-Clip path is TODO: the core
    // must report the pair and emit no contacts rather than guessing.
    World w;
    RigidBody a, b;
    a.def = BodyDef{};
    a.def.inv_mass = 1.0f;
    a.def.shape.type = SHAPE_MESH;
    b.def = BodyDef{};
    b.def.inv_mass = 1.0f;
    b.def.shape.type = SHAPE_MESH;
    b.pos = vec3(0.1f, 0.0f, 0.0f);
    w.add_body(&a);
    w.add_body(&b);
    w.step(0.02f);
    bool ok = !shape_kind_resolved(SHAPE_MESH) && w.contacts.empty() && w.unsupported_pairs > 0;
    char detail[128];
    snprintf(detail, sizeof detail, "contacts=%zu unsupported_pairs=%u", w.contacts.size(),
                  w.unsupported_pairs);
    check("unresolved mesh kind emits no contacts", ok, detail);
}

int main()
{
    printf("physcheck: physics core checks\n");
    test_free_fall();
    test_resting_on_plane();
    test_sliding_friction();
    test_stacked_boxes();
    test_unresolved_shape_kinds();
    if (failures != 0) {
        fprintf(stderr, "physcheck: %d check(s) FAILED\n", failures);
        assert(failures == 0 && "physcheck failures");
        return 1;
    }
    printf("physcheck: all checks passed\n");
    return 0;
}
