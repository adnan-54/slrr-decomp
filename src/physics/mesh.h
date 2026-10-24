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
    std::vector<Vec3> face_normals;   // stored PHYS normal per triangle; empty for SCX
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

// Same tree for an already world-frame soup (metres, no scale), e.g. a map's
// PHYS triangles (notes/19 §7). Returns null when no usable triangle survives.
std::shared_ptr<const CollisionMesh> build_collision_mesh(
    const std::vector<Vec3> &verts, const std::vector<CollisionMesh::Triangle> &tris);

// Same tree plus the PHYS chunk's stored face normals, one per triangle in the
// same order as `tris` (0x519940 face.n). A size mismatch leaves face_normals
// empty and mesh_segment_cross refuses the mesh.
std::shared_ptr<const CollisionMesh> build_collision_mesh(
    const std::vector<Vec3> &verts, const std::vector<CollisionMesh::Triangle> &tris,
    const std::vector<Vec3> &normals);

struct MeshRayHit {
    Vec3 point{0.0f, 0.0f, 0.0f};  // world
    Vec3 normal{0.0f, 1.0f, 0.0f}; // world, unit, faces the ray
    float distance = 0.0f;         // along dir (dir must be unit)
    uint32_t triangle = 0;
};

// Closest hit along `dir` in [0, max_dist]; `pose` maps mesh-local to world.
bool mesh_raycast(const CollisionMesh &m, const Pose &pose, Vec3 origin, Vec3 dir,
                  float max_dist, MeshRayHit *hit);

// One crossing of the wheel sensor segment against a map PHYS face, as 0x4fd110
// reports it. The normal is the stored PHYS face normal, never recomputed.
struct SegmentCross {
    Vec3 point{0.0f, 0.0f, 0.0f};  // world
    Vec3 normal{0.0f, 1.0f, 0.0f}; // world, PHYS face normal
    float t = 0.0f;                // fraction along p0 -> p1 (0x4fd110 return)
    uint32_t triangle = 0;
};

// Every face 0x4fd110 accepts along p0 -> p1 (0x5043d0 picks the 2D axes).
// The exe tests every face of the resident map node; the segment's AABB query
// is the src broadphase (INVENTED: a crossing face always overlaps it, so the
// face set equals the exe's all-faces loop). Requires face_normals; returns 0
// otherwise. All hits are pushed in tree order.
int mesh_segment_cross(const CollisionMesh &m, const Pose &pose, Vec3 p0, Vec3 p1,
                       std::vector<SegmentCross> *out);

// Contacts between a mesh and a sphere/box/capsule. Normals are B -> A as in
// physics.h; `mesh_is_a` says which side the mesh is. Up to max_hits witnesses
// (deepest first, duplicates merged) with separation below `slop` are returned.
int mesh_contacts(const CollisionMesh &m, const Pose &mesh_pose, const Shape &prim,
                  const Pose &prim_pose, bool mesh_is_a, ShapeHit *hits, int max_hits,
                  float slop);
