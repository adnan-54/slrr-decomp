// scxcheck: dump one SCX mesh or run the shipped-content selfcheck.
//
//   scxcheck <file.scx>          summary dump
//   scxcheck --selfcheck [root]  walk <root> (default "bin/the game"), parse
//                                every .scx and assert the known-good counts
//
// The selfcheck also runs the small assert-based _phys_FL_door.scx test.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "mesh/scx.h"

namespace fs = std::filesystem;

namespace {

struct VersionStat {
    size_t files = 0;
    size_t sections = 0;
    size_t vertices = 0;
    size_t triangles = 0;
    size_t materials = 0;
};

// The five structurally valid v4 assets whose UVs contain NaN; the loader
// never validates UV finiteness (notes/17 §9).
const char *const kKnownBadUv[] = {
    "misc/garage/g2/garazs2_targyak.scx",
    "misc/garage/g3/garazs3targyak.scx",
    "objects/meshes/area_41_egyedi_b/area_41_egyedi_b.scx",
    "objects/meshes/shop_01_egyedi/shop_01_egyedi.scx",
    "objects/meshes/area_41_egyedi/area_41_egyedi.scx",
};

std::string lower_ascii(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string relative_path(const fs::path &path, const fs::path &root)
{
    std::error_code ec;
    fs::path rel = fs::relative(path, root, ec);
    return ec ? path.generic_string() : rel.generic_string();
}

bool collect_scx(const fs::path &root, std::vector<fs::path> *out, std::string *err)
{
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                       ec);
    if (ec) {
        *err = "cannot walk " + root.string() + ": " + ec.message();
        return false;
    }
    const fs::recursive_directory_iterator end;
    while (it != end) {
        std::error_code fec;
        if (it->is_regular_file(fec)) {
            std::string ext = lower_ascii(it->path().extension().string());
            if (ext == ".scx")
                out->push_back(it->path());
        } else if (fec) {
            *err = "cannot stat " + it->path().string() + ": " + fec.message();
            return false;
        }
        it.increment(ec);
        if (ec) {
            *err = "walk failed under " + root.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

bool has_nan_uv(const mesh::Mesh &m)
{
    for (const mesh::Section &s : m.sections) {
        size_t sets = s.uv_set_count < s.uv.size() ? s.uv_set_count : s.uv.size();
        for (size_t k = 0; k < sets; k++)
            for (const mesh::Vec2 &uv : s.uv[k])
                if (!std::isfinite(uv.u) || !std::isfinite(uv.v))
                    return true;
    }
    return false;
}

uint32_t popcount32(uint32_t v)
{
    uint32_t n = 0;
    while (v) {
        n += v & 1u;
        v >>= 1;
    }
    return n;
}

// Checks the invariants the selfcheck promises: index width, per-field array
// sizes and the chunked stride identity fixed + 8*nuv.
std::string check_mesh(const mesh::Mesh &m)
{
    char buf[192];
    for (size_t si = 0; si < m.sections.size(); si++) {
        const mesh::Section &s = m.sections[si];
        const size_t nv = s.positions.size();
        auto bad = [&](const char *msg) {
            snprintf(buf, sizeof buf, "section %zu: %s", si, msg);
            return std::string(buf);
        };
        if (!s.normals.empty() && s.normals.size() != nv)
            return bad("normal count != vertex count");
        if (!s.diffuse.empty() && s.diffuse.size() != nv)
            return bad("diffuse count != vertex count");
        if (!s.specular.empty() && s.specular.size() != nv)
            return bad("specular count != vertex count");
        if (!s.beta.empty() && s.beta.size() != nv)
            return bad("beta count != vertex count");
        for (size_t k = 0; k < s.weights.size(); k++) {
            if (k < s.weight_count && s.weights[k].size() != nv)
                return bad("weight array size != vertex count");
            if (k >= s.weight_count && !s.weights[k].empty())
                return bad("unexpected weight array");
        }
        if (s.uv_set_count > s.uv.size())
            return bad("UV set count above 7");
        for (size_t k = 0; k < s.uv.size(); k++) {
            if (k < s.uv_set_count && s.uv[k].size() != nv)
                return bad("UV set size != vertex count");
            if (k >= s.uv_set_count && !s.uv[k].empty())
                return bad("unexpected UV set");
        }
        if (!s.indices.empty() && !s.indices16.empty())
            return bad("both index arrays populated");
        if (s.indices.size() % 3 != 0 || s.indices16.size() % 3 != 0)
            return bad("index count is not a triangle multiple");
        if (s.material != mesh::kNoMaterial && s.material >= m.materials.size())
            return bad("material index out of range");

        if (m.chunked) {
            uint32_t fixed = ((s.flags & 0x1u) ? 12u : 0u); // position
            fixed += (s.flags & 0x4u) ? 4u : ((s.flags & 0x8u) ? 8u : ((s.flags & 0x10u) ? 12u : 0u));
            fixed += (s.flags & 0x20u) ? 4u : 0u;  // extra beta
            fixed += (s.flags & 0x40u) ? 12u : 0u; // normal
            fixed += (s.flags & 0x80u) ? 4u : 0u;  // diffuse
            fixed += (s.flags & 0x100u) ? 4u : 0u; // specular
            if (s.uv_set_count != popcount32(s.flags & 0xfe00u))
                return bad("UV set count != popcount(flags & 0xfe00)");
            if (s.stride != fixed + 8 * s.uv_set_count)
                return bad("stride != fixed + 8*nuv");
        } else {
            if (s.stride < 0x2cu)
                return bad("sequential stride below 0x2c");
            uint32_t want_uv = s.stride >= 0x3c ? 3u : 2u;
            if (s.uv_set_count != want_uv)
                return bad("sequential UV set count does not match stride");
            uint32_t want_weights = m.version <= 2
                                        ? 0u
                                        : (s.stride > 0x2c ? 1u : 0u) +
                                              (s.stride > 0x30 ? 1u : 0u);
            if (s.weight_count != want_weights)
                return bad("sequential weight count does not match stride");
        }
    }
    if (m.bounds.valid != (m.vertex_count() != 0)) {
        snprintf(buf, sizeof buf, "bounds validity does not match vertex count");
        return std::string(buf);
    }
    return std::string();
}

void test_phys_fl_door(const fs::path &root)
{
    fs::path path = root / "cars/racers/Axis_data/meshes/_phys_FL_door.scx";
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        printf("SKIP  _phys_FL_door.scx (not present)\n");
        return;
    }
    mesh::Mesh m;
    std::string err;
    bool ok = mesh::load(path.string().c_str(), &m, &err);
    if (!ok) {
        printf("FAIL  _phys_FL_door.scx: %s\n", err.c_str());
        assert(ok);
        return;
    }
    // Expected counts and AABB come from the shipped file itself
    // (notes/17 §8: 16 vertices, 28 triangles; bounds in metres -> cm).
    assert(m.version == 3);
    assert(!m.chunked);
    assert(m.sections.size() == 1);
    assert(m.materials.size() == 1);
    assert(m.sections[0].vertex_count() == 16);
    assert(m.sections[0].triangle_count() == 28);
    assert(m.vertex_count() == 16);
    assert(m.triangle_count() == 28);
    assert(m.bounds.valid);
    const float eps = 1e-3f;
    assert(fabsf(m.bounds.min.x - -11.556355f) < eps);
    assert(fabsf(m.bounds.min.y - -32.133842f) < eps);
    assert(fabsf(m.bounds.min.z - -60.223564f) < eps);
    assert(fabsf(m.bounds.max.x - 10.455904f) < eps);
    assert(fabsf(m.bounds.max.y - 31.554514f) < eps);
    assert(fabsf(m.bounds.max.z - 54.891506f) < eps);
    assert(mesh::kRenderScale == 0.1f);
    assert(mesh::kCollisionScale == 0.01f);
    printf("PASS  _phys_FL_door.scx: 1 section, 16 vertices, 28 triangles, bounds match\n");
}

int selfcheck(const fs::path &root)
{
    std::vector<fs::path> files;
    std::string err;
    if (!collect_scx(root, &files, &err)) {
        fprintf(stderr, "FAIL  %s\n", err.c_str());
        return 1;
    }
    std::sort(files.begin(), files.end());

    std::map<uint32_t, VersionStat> stats;
    std::vector<std::string> errors;
    std::vector<std::string> bad_uv;
    size_t empty = 0;
    size_t parsed = 0;

    for (const fs::path &path : files) {
        const std::string rel = relative_path(path, root);
        std::error_code ec;
        uintmax_t size = fs::file_size(path, ec);
        if (ec) {
            errors.push_back(rel + ": cannot stat: " + ec.message());
            continue;
        }
        if (size == 0) {
            // modding_tools/.../sample.scx is 0 bytes; it is not a mesh and is
            // not counted (notes/17 §9).
            empty++;
            continue;
        }
        mesh::Mesh m;
        std::string perr;
        if (!mesh::load(path.string().c_str(), &m, &perr)) {
            errors.push_back(rel + ": " + perr);
            continue;
        }
        parsed++;
        VersionStat &st = stats[m.version];
        st.files++;
        st.sections += m.sections.size();
        st.vertices += m.vertex_count();
        st.triangles += m.triangle_count();
        st.materials += m.materials.size();

        std::string problem = check_mesh(m);
        if (!problem.empty())
            errors.push_back(rel + ": " + problem);
        if (m.version == 4 && has_nan_uv(m))
            bad_uv.push_back(lower_ascii(rel));
    }

    // Shipped-install known-good file counts (notes/17 §9). v1/v2 have no
    // samples; v4 is 2,495 clean assets plus the 5 known-bad NaN-UV files.
    static const struct {
        uint32_t version;
        size_t files;
    } kExpected[] = {{1, 0}, {2, 0}, {3, 3532}, {4, 2500}, {5, 775}, {6, 644}};
    for (const auto &e : kExpected) {
        size_t got = stats[e.version].files;
        if (got != e.files) {
            char buf[128];
            snprintf(buf, sizeof buf, "version %u: parsed %zu files, expected %zu",
                     e.version, got, e.files);
            errors.push_back(buf);
        }
    }

    std::sort(bad_uv.begin(), bad_uv.end());
    std::vector<std::string> known;
    for (const char *k : kKnownBadUv)
        known.push_back(lower_ascii(k));
    std::sort(known.begin(), known.end());
    if (bad_uv != known)
        errors.push_back("v4 NaN-UV set does not match the 5 known-bad assets");

    printf("SCX selfcheck: %s\n", root.string().c_str());
    printf("files: %zu .scx, skipped empty: %zu, parsed: %zu\n", files.size(), empty,
           parsed);
    printf("ver     files  known-bad   sections   vertices  triangles  materials\n");
    for (uint32_t v = 1; v <= 6; v++) {
        const VersionStat &st = stats[v];
        size_t bad = v == 4 ? bad_uv.size() : 0;
        printf("%3u  %8zu  %9zu  %10zu  %10zu  %10zu  %10zu\n", v, st.files, bad,
               st.sections, st.vertices, st.triangles, st.materials);
    }
    printf("known-bad (v4, NaN UV; structurally valid):\n");
    for (const char *k : kKnownBadUv)
        printf("  %s\n", k);

    test_phys_fl_door(root);

    if (!errors.empty()) {
        printf("FAIL  %zu problem(s):\n", errors.size());
        for (const std::string &e : errors)
            printf("  %s\n", e.c_str());
        return 1;
    }
    printf("PASS  all SCX files parsed; counts, blob consumption, strides and bounds match\n");
    return 0;
}

void print_color(const char *label, const mesh::Color &c)
{
    printf("    %s rgba(%.3f, %.3f, %.3f, %.3f)\n", label, c.r, c.g, c.b, c.a);
}

int dump_file(const char *path)
{
    mesh::Mesh m;
    std::string err;
    if (!mesh::load(path, &m, &err)) {
        fprintf(stderr, "%s: %s\n", path, err.c_str());
        return 1;
    }
    std::error_code ec;
    uintmax_t size = fs::file_size(path, ec);
    printf("%s\n", path);
    printf("  %zu bytes, version %u (%s)\n", ec ? static_cast<size_t>(0) : static_cast<size_t>(size),
           m.version, m.chunked ? "chunked" : "sequential");
    if (m.version == 5 || m.version == 6)
        printf("  Steam remap: walked as version %u\n", m.version == 5 ? 3u : 4u);
    printf("  sections %zu, vertices %zu, triangles %zu, materials %zu\n",
           m.sections.size(), m.vertex_count(), m.triangle_count(), m.materials.size());
    if (!m.bounds.valid) {
        printf("  bounds: none (no vertices)\n");
    } else {
        printf("  bounds cm: min (%.4f %.4f %.4f) max (%.4f %.4f %.4f)\n", m.bounds.min.x,
               m.bounds.min.y, m.bounds.min.z, m.bounds.max.x, m.bounds.max.y,
               m.bounds.max.z);
        mesh::Vec3 c = m.bounds.center();
        mesh::Vec3 h = m.bounds.half_extent();
        printf("  render x%.2f: center (%.4f %.4f %.4f) half (%.4f %.4f %.4f) radius %.4f\n",
               static_cast<double>(mesh::kRenderScale), c.x * mesh::kRenderScale,
               c.y * mesh::kRenderScale, c.z * mesh::kRenderScale, h.x * mesh::kRenderScale,
               h.y * mesh::kRenderScale, h.z * mesh::kRenderScale,
               m.bounds.radius() * mesh::kRenderScale);
        printf("  collision x%.2f: center (%.4f %.4f %.4f) half (%.4f %.4f %.4f) radius %.4f\n",
               static_cast<double>(mesh::kCollisionScale), c.x * mesh::kCollisionScale,
               c.y * mesh::kCollisionScale, c.z * mesh::kCollisionScale,
               h.x * mesh::kCollisionScale, h.y * mesh::kCollisionScale,
               h.z * mesh::kCollisionScale, m.bounds.radius() * mesh::kCollisionScale);
    }

    for (size_t i = 0; i < m.materials.size(); i++) {
        const mesh::Material &mat = m.materials[i];
        printf("  material %zu: type %u flags 0x%x name \"%s\"\n", i, mat.type, mat.flags,
               mat.name.c_str());
        print_color("diffuse", mat.diffuse);
        print_color("ambient", mat.ambient);
        print_color("specular", mat.specular);
        printf("    ambient scale %.4f, power %.4f, spheremap %.4f\n", mat.ambient_scale,
               mat.specular_power, mat.spheremap_strength);
        if (mat.stride != 0)
            printf("    sequential stride %u, slots [%d %d %d %d %d %d %d %d], UV channels "
                   "[%d %d %d], base UV %d, second UV %d\n",
                   mat.stride, mat.texture_slots[0], mat.texture_slots[1], mat.texture_slots[2],
                   mat.texture_slots[3], mat.texture_slots[4], mat.texture_slots[5],
                   mat.texture_slots[6], mat.texture_slots[7], mat.uv_channels[0],
                   mat.uv_channels[1], mat.uv_channels[2], mat.base_uv, mat.second_uv);
        for (size_t t = 0; t < mat.textures.size(); t++) {
            const mesh::TextureRecord &tex = mat.textures[t];
            printf("    texture %zu: subtype %u index %u sourceUV %d unknown %u scale "
                   "(%.4f %.4f) offset (%.4f %.4f)\n",
                   t, tex.subtype, tex.index, tex.source_uv, tex.unknown, tex.scale_u,
                   tex.scale_v, tex.offset_u, tex.offset_v);
        }
    }

    for (size_t i = 0; i < m.sections.size(); i++) {
        const mesh::Section &s = m.sections[i];
        printf("  section %zu: material %d mode %d flags 0x%x stride %u uv-sets %u weights "
               "%u beta %s palette %zu, vertices %zu, triangles %zu\n",
               i, s.material == mesh::kNoMaterial ? -1 : static_cast<int>(s.material),
               static_cast<int>(s.mode), s.flags, s.stride, s.uv_set_count, s.weight_count,
               s.beta.empty() ? "no" : "yes", s.palette.size(), s.vertex_count(),
               s.triangle_count());
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.scx>\n       %s --selfcheck [root]\n", argv[0],
                argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "--selfcheck") == 0) {
        if (argc > 3) {
            fprintf(stderr, "--selfcheck takes at most one root path\n");
            return 2;
        }
        fs::path root = argc == 3 ? fs::path(argv[2]) : fs::path("bin/the game");
        return selfcheck(root);
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        printf("usage: %s <file.scx>\n       %s --selfcheck [root]\n", argv[0], argv[0]);
        return 0;
    }
    if (argv[1][0] == '-') {
        fprintf(stderr, "unknown option %s\n", argv[1]);
        return 2;
    }
    if (argc != 2) {
        fprintf(stderr, "unexpected extra argument %s\n", argv[2]);
        return 2;
    }
    return dump_file(argv[1]);
}
