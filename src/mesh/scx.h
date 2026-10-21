// SCX mesh format (decoded in notes/17-scx-mesh.md).
//
// Two container families share the "INVO" magic + u32 version header:
//   - sequential v1/v2/v3/v5: a chain of material+section records;
//   - chunked v4/v6: a directory followed by material/section/vertex/index chunks.
// The Steam version remap (DAT_007a75e4) rewrites 5 -> 3 and 6 -> 4 before
// dispatch, so a raw v5 file uses the sequential walker and a raw v6 file the
// chunked one. Raw v4 is chunked, raw v1/v2/v3 sequential, unconditionally.
//
// File positions stay in centimetres. Render calls multiply by kRenderScale
// (engine unit = 0.1 m), collision by kCollisionScale (metres).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <array>
#include <string>
#include <vector>

namespace mesh {

constexpr float kRenderScale = 0.1f;     // file cm -> render units
constexpr float kCollisionScale = 0.01f; // file cm -> metres
constexpr uint32_t kNoMaterial = 0xffffffffu;

struct Vec2 {
    float u = 0.0f, v = 0.0f;
};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// Colour components are 0..1. Packed 0xAARRGGBB sources are unpacked on load.
struct Color {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};

// One v4/v6 material texture/UV record (decoder 6 parameter).
struct TextureRecord {
    uint32_t subtype = 0;    // low 24 bits of the parameter id
    uint32_t index = 0;      // texture slot / resource index
    int32_t source_uv = 0;   // 0..nuv-1, -1 planar (notes/17 §4)
    uint32_t unknown = 3;    // dword 2, meaning unknown
    float scale_u = 0.0f, scale_v = 0.0f;
    float offset_u = 0.0f, offset_v = 0.0f;
};

struct Material {
    uint32_t type = 0;   // material class id (chunked blob)
    uint32_t flags = 0;
    Color diffuse, ambient, specular;
    float ambient_scale = 0.0f;
    float specular_power = 0.0f;
    float spheremap_strength = 0.0f;
    std::string name;
    std::array<int16_t, 8> texture_slots; // v1-v3/v5 record texture indices
    std::array<int16_t, 3> uv_channels;   // v1-v3/v5 UV channel indices
    int16_t base_uv = 0;                  // material +0x46, clamped to <= 2
    int16_t second_uv = 0;                // material +0x48
    uint32_t stride = 0;                  // sequential vertex stride
    std::vector<TextureRecord> textures;  // chunked blob records

    Material()
    {
        texture_slots.fill(-1);
        uv_channels.fill(-1);
    }
};

struct Section {
    uint32_t material = kNoMaterial; // index into Mesh::materials
    int32_t mode = -1;               // chunked mode; -1 until derived from flags
    uint32_t flags = 0;              // chunked vertex flag word
    uint32_t stride = 0;             // on-disk vertex stride
    uint32_t uv_set_count = 0;
    uint32_t weight_count = 0;
    std::vector<uint32_t> palette; // chunk 2/3 entries, not bound to a skeleton

    std::vector<Vec3> positions;
    std::vector<Vec3> normals;      // empty when the file has no normal field
    std::vector<uint32_t> diffuse;  // packed 0xAARRGGBB
    std::vector<uint32_t> specular; // packed override, empty when absent
    std::array<std::vector<float>, 3> weights;
    std::vector<float> beta;
    std::array<std::vector<Vec2>, 7> uv; // UV sets in file order
    std::vector<uint32_t> indices;       // sequential triangles (u32 on disk)
    std::vector<uint16_t> indices16;     // chunked triangles (u16 on disk)

    size_t vertex_count() const { return positions.size(); }
    size_t index_count() const
    {
        return indices.empty() ? indices16.size() : indices.size();
    }
    size_t triangle_count() const { return index_count() / 3; }
};

// Mesh bounds in file centimetres (notes/17 §6).
struct Bounds {
    Vec3 min, max;
    bool valid = false;

    Vec3 center() const;
    Vec3 half_extent() const;
    float radius() const; // sqrt(max(|min|^2, |max|^2))
};

struct Mesh {
    uint32_t version = 0; // raw file version (before the Steam remap)
    bool chunked = false; // container family after remap
    std::vector<Material> materials;
    std::vector<Section> sections;
    Bounds bounds;

    size_t vertex_count() const
    {
        size_t n = 0;
        for (const Section &s : sections)
            n += s.positions.size();
        return n;
    }
    size_t triangle_count() const
    {
        size_t n = 0;
        for (const Section &s : sections)
            n += s.triangle_count();
        return n;
    }
};

struct ParseOptions {
    // DAT_007a75e4: raw 5 is walked as 3, raw 6 as 4.
    bool steam_version_remap = true;
};

// Strict parse: every range is bounds-checked, material blobs must consume
// their chunk exactly, chunk chains and sequential records must end at EOF.
bool parse(const void *data, size_t size, Mesh *out, std::string *err,
           const ParseOptions &opt = ParseOptions());
bool load(const char *path, Mesh *out, std::string *err,
          const ParseOptions &opt = ParseOptions());

} // namespace mesh
