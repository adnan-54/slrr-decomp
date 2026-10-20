// Ray query against a rigid body's shape, used by the vehicle suspension
// (notes/09-vehicle.md: the wheel spring is a downward ray from the chassis
// mount; S+0xb4/S+0xd0..cc are filled by the physics ray pass).
//
// The exe's ray pass is not decoded; this is the minimal helper the vehicle
// layer needs. Plane/sphere/box are covered (the shipped ground bodies are
// primitives). Capsule and mesh kinds return no hit.
#pragma once
#include <math.h>

#include "physics/physics.h"

struct RayHit {
    Vec3 point{0.0f, 0.0f, 0.0f};     // world
    Vec3 normal{0.0f, 1.0f, 0.0f};    // world, unit
    float distance = 0.0f;            // along dir
};

// dir must be unit length. Returns false when the ray misses or the first hit
// is farther than max_dist (origin inside a box counts as a hit at t=0).
inline bool raycast_body(const RigidBody &body, Vec3 origin, Vec3 dir, float max_dist,
                         RayHit *hit)
{
    if (max_dist < 0.0f)
        return false;
    const Shape &s = body.def.shape;
    Mat34 r = body.rotation();

    switch (s.type) {
    case SHAPE_PLANE: {
        Vec3 lo = mat_tvec(r, origin - body.pos);
        Vec3 ld = mat_tvec(r, dir);
        float denom = vec_dot(ld, s.plane_normal);
        if (fabsf(denom) < 1e-8f)
            return false;
        float t = vec_dot(s.plane_point - lo, s.plane_normal) / denom;
        if (t < 0.0f || t > max_dist)
            return false;
        hit->distance = t;
        hit->point = body.pos + mat_vec(r, lo + ld * t);
        hit->normal = vec_normalize(mat_vec(r, s.plane_normal), Vec3{0.0f, 1.0f, 0.0f});
        return true;
    }
    case SHAPE_SPHERE: {
        Vec3 oc = origin - body.pos;
        float b = vec_dot(oc, dir);
        float c = vec_dot(oc, oc) - s.radius * s.radius;
        float disc = b * b - c;
        if (disc < 0.0f)
            return false;
        float root = sqrtf(disc);
        float t = -b - root;
        if (t < 0.0f)
            t = -b + root;
        if (t < 0.0f || t > max_dist)
            return false;
        hit->distance = t;
        hit->point = origin + dir * t;
        hit->normal = vec_normalize(hit->point - body.pos, Vec3{0.0f, 1.0f, 0.0f});
        return true;
    }
    case SHAPE_BOX: {
        Vec3 lo = mat_tvec(r, origin - body.pos);
        Vec3 ld = mat_tvec(r, dir);
        float o[3] = {lo.x, lo.y, lo.z};
        float d[3] = {ld.x, ld.y, ld.z};
        float h[3] = {s.half_extents.x, s.half_extents.y, s.half_extents.z};
        float t_enter = -1e30f, t_exit = 1e30f;
        int enter_axis = 0;
        float enter_sign = 1.0f;
        for (int i = 0; i < 3; i++) {
            if (fabsf(d[i]) < 1e-8f) {
                if (fabsf(o[i]) > h[i])
                    return false;
                continue;
            }
            float inv = 1.0f / d[i];
            float t1 = (-h[i] - o[i]) * inv;
            float t2 = (h[i] - o[i]) * inv;
            float sign = -1.0f; // entering through the -axis face
            if (t1 > t2) {
                float tmp = t1;
                t1 = t2;
                t2 = tmp;
                sign = 1.0f;
            }
            if (t1 > t_enter) {
                t_enter = t1;
                enter_axis = i;
                enter_sign = sign;
            }
            if (t2 < t_exit)
                t_exit = t2;
        }
        if (t_enter > t_exit || t_exit < 0.0f)
            return false;
        float t = t_enter >= 0.0f ? t_enter : t_exit;
        if (t > max_dist)
            return false;
        float ln[3] = {0.0f, 0.0f, 0.0f};
        if (t_enter >= 0.0f)
            ln[enter_axis] = enter_sign;
        else
            ln[1] = 1.0f; // origin inside: push out along +Y, unused by the car
        hit->distance = t;
        hit->point = origin + dir * t;
        hit->normal = vec_normalize(mat_vec(r, Vec3{ln[0], ln[1], ln[2]}),
                                    Vec3{0.0f, 1.0f, 0.0f});
        return true;
    }
    default:
        // TODO: capsule/mesh ground bodies. The suspension only needs planes
        // in the shipped data and tests.
        return false;
    }
}
