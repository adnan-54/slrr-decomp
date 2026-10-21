// Collision mesh built from SCX sections (notes/11-collision-shapes.md §1/§2,
// notes/17-scx-mesh.md §8). The exe turns the triangle soup into a V-Clip
// polyhedron (shape kind 6); this port keeps the soup with an AABB tree and
// answers the queries the physics core needs directly. Vertices are metres in
// the body frame (the SCX loader keeps file centimetres).
#pragma once
#include <memory>
#include <vector>

#include "physics/physics.h"

namespace mesh {
struct Mesh;
}

// Median-split AABB tree over a triangle soup. Leaves hold up to four
// triangles in `order`; inner nodes hold the union AABB and child indices.
struct CollisionMesh {
    struct Triangle {
        uint32_t v[3];
    };
    struct Node {
        Vec3 min{0.0f, 0.0f, 0.0f}, max{0.0f, 0.0f, 0.0f};
        uint32_t first = 0, count = 0; // triangle range in `order` (leaf)
        int32_t left = -1, right = -1;
    };

    std::vector<Vec3> vertices;       // local body frame, metres
    std::vector<Triangle> triangles;
    std::vector<Node> nodes;
    std::vector<uint32_t> order;      // triangle indices, leaf order
    Vec3 bounds_min{0.0f, 0.0f, 0.0f};
    Vec3 bounds_max{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;              // max |v|; the shape adds the 0.05 slack

    bool empty() const { return triangles.empty(); }
};

// Builds the soup from every SCX section (file centimetres -> metres).
// Returns null when no usable triangle survives.
std::shared_ptr<const CollisionMesh> build_collision_mesh(const mesh::Mesh &m);

struct MeshRayHit {
    Vec3 point{0.0f, 0.0f, 0.0f};  // world
    Vec3 normal{0.0f, 1.0f, 0.0f}; // world, unit, faces the ray
    float distance = 0.0f;         // along dir (dir must be unit)
    uint32_t triangle = 0;
};

// Closest hit along `dir` in [0, max_dist]; `pose` maps mesh-local to world.
bool mesh_raycast(const CollisionMesh &m, const Pose &pose, Vec3 origin, Vec3 dir,
                  float max_dist, MeshRayHit *hit);

// Contacts between a mesh and a sphere/box/capsule. Normals are B -> A as in
// physics.h; `mesh_is_a` says which side the mesh is. Up to max_hits witnesses
// (deepest first, duplicates merged) with separation below `slop` are returned.
int mesh_contacts(const CollisionMesh &m, const Pose &mesh_pose, const Shape &prim,
                  const Pose &prim_pose, bool mesh_is_a, ShapeHit *hits, int max_hits,
                  float slop);
