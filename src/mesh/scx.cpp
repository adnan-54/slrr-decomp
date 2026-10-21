#include "mesh/scx.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <fstream>
#include <iterator>
#include <utility>

namespace mesh {
namespace {

constexpr uint32_t kMagic = 0x4F564E49u; // "INVO"
constexpr size_t kSequentialMaterialSize = 0x88;
constexpr uint32_t kSequentialMinStride = 0x2c;

bool fail(std::string *err, const char *msg, size_t off = static_cast<size_t>(-1))
{
    if (!err)
        return false;
    *err = msg;
    if (off != static_cast<size_t>(-1)) {
        char buf[32];
        snprintf(buf, sizeof buf, " @0x%zx", off);
        *err += buf;
    }
    return false;
}

bool have(size_t size, size_t off, size_t n)
{
    return off <= size && n <= size - off;
}

uint16_t rd16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

int16_t rd16s(const uint8_t *p)
{
    return static_cast<int16_t>(rd16(p));
}

uint32_t rd32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

float rdf32(const uint8_t *p)
{
    uint32_t bits = rd32(p);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

void wr16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void wr32(uint8_t *p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t popcount(uint32_t v)
{
    uint32_t n = 0;
    while (v) {
        n += v & 1u;
        v >>= 1;
    }
    return n;
}

Color color_argb(uint32_t packed)
{
    const float k = 1.0f / 255.0f;
    Color c;
    c.a = static_cast<float>((packed >> 24) & 0xffu) * k;
    c.r = static_cast<float>((packed >> 16) & 0xffu) * k;
    c.g = static_cast<float>((packed >> 8) & 0xffu) * k;
    c.b = static_cast<float>(packed & 0xffu) * k;
    return c;
}

Color color_f32(const uint8_t *p)
{
    Color c;
    c.r = rdf32(p);
    c.g = rdf32(p + 4);
    c.b = rdf32(p + 8);
    c.a = rdf32(p + 12);
    return c;
}

Color color_rgb_f32(const uint8_t *p)
{
    Color c;
    c.r = rdf32(p);
    c.g = rdf32(p + 4);
    c.b = rdf32(p + 8);
    return c;
}

std::string read_name(const uint8_t *p, size_t max)
{
    size_t n = 0;
    while (n < max && p[n] != 0)
        n++;
    return std::string(reinterpret_cast<const char *>(p), n);
}

// v1/v2 vertex RGB floats become packed ARGB through FUN_0056a740 (float*255,
// truncate); alpha is the material diffuse alpha.
uint8_t float_to_byte(float v)
{
    float f = v * 255.0f;
    if (!(f > 0.0f))
        return 0;
    if (f >= 255.0f)
        return 255;
    return static_cast<uint8_t>(static_cast<int>(f));
}

uint32_t pack_argb(float a, float r, float g, float b)
{
    return (static_cast<uint32_t>(float_to_byte(a)) << 24) |
           (static_cast<uint32_t>(float_to_byte(r)) << 16) |
           (static_cast<uint32_t>(float_to_byte(g)) << 8) |
           static_cast<uint32_t>(float_to_byte(b));
}

// Material record of a v3/v5 sequential section. The exe copies at most 0x88
// bytes over its default material (or the u16 at +0x42 when flags bit 3 is set).
Material decode_sequential_material(const uint8_t *buf)
{
    Material m;
    m.diffuse = color_f32(buf + 0x04);
    m.ambient = color_rgb_f32(buf + 0x14);
    m.ambient_scale = rdf32(buf + 0x20);
    m.specular_power = rdf32(buf + 0x24);
    m.flags = rd32(buf + 0x28);
    for (int i = 0; i < 8; i++)
        m.texture_slots[static_cast<size_t>(i)] = rd16s(buf + 0x2c + 2 * i);
    m.stride = rd32(buf + 0x3c);
    int16_t u0 = rd16s(buf + 0x40);
    int16_t u1 = rd16s(buf + 0x42);
    int16_t u2 = rd16s(buf + 0x44);
    if (u0 == 0 && u1 == 0 && u2 == 0)
        u0 = u1 = u2 = -1;
    m.uv_channels = {{u0, u1, u2}};
    int16_t base = rd16s(buf + 0x46);
    if (base > 2)
        base = 2;
    m.base_uv = base;
    m.second_uv = rd16s(buf + 0x48);
    m.specular = color_rgb_f32(buf + 0x5c);
    m.name = read_name(buf + 0x68, 32);
    return m;
}

// v1 header 0x38, v2 header 0x34 (notes/17 §2.4); fields inferred from the
// conversion code, no shipped samples exist.
Material decode_legacy_material(const uint8_t *rec, uint32_t version)
{
    Material m;
    for (int i = 0; i < 5; i++)
        m.texture_slots[static_cast<size_t>(i)] = rd16s(rec + 2 * i);
    if (version == 1) {
        m.diffuse = color_f32(rec + 0x14);
        m.ambient = color_rgb_f32(rec + 0x24);
        m.ambient_scale = rdf32(rec + 0x30);
        m.specular_power = rdf32(rec + 0x34);
    } else {
        m.diffuse = color_f32(rec + 0x0c);
        m.ambient = color_rgb_f32(rec + 0x1c);
        m.ambient_scale = rdf32(rec + 0x28);
        m.specular_power = rdf32(rec + 0x30);
    }
    m.stride = 0x34;
    return m;
}

bool decode_sequential_vertices(const uint8_t *data, size_t voff, uint32_t nv,
                                uint32_t version, const Material &mat, Section *s)
{
    s->stride = mat.stride;
    s->positions.resize(nv);
    s->normals.resize(nv);
    // The sequential engine FVF has three UV sets; +0x34 only holds a pair
    // when the stride reaches 0x3c.
    s->uv_set_count = (version >= 3 && mat.stride >= 0x3c) ? 3 : 2;
    for (uint32_t k = 0; k < s->uv_set_count; k++)
        s->uv[k].resize(nv);
    s->diffuse.resize(nv);

    if (version >= 3) {
        s->weight_count = (mat.stride > 0x2c ? 1u : 0u) + (mat.stride > 0x30 ? 1u : 0u);
        for (uint32_t k = 0; k < s->weight_count; k++)
            s->weights[k].resize(nv);
        // +0x3c is read as the specular override when material flags bit 2
        // is set (observed in 0x451b00; notes/17 §2.3 labels it blend weight A).
        bool has_specular = mat.stride > 0x3c && (mat.flags & 0x4u) != 0;
        if (has_specular)
            s->specular.resize(nv);

        for (uint32_t i = 0; i < nv; i++) {
            const uint8_t *p = data + voff + static_cast<size_t>(i) * mat.stride;
            Vec3 pos = {rdf32(p), rdf32(p + 4), rdf32(p + 8)};
            Vec3 nrm = {rdf32(p + 0xc), rdf32(p + 0x10), rdf32(p + 0x14)};
            Vec2 uv0 = {rdf32(p + 0x18), rdf32(p + 0x1c)};
            Vec2 uv1 = {rdf32(p + 0x20), rdf32(p + 0x24)};
            s->positions[i] = pos;
            s->normals[i] = nrm;
            s->uv[0][i] = uv0;
            s->uv[1][i] = uv1;
            s->diffuse[i] = rd32(p + 0x28);
            if (s->weight_count > 0)
                s->weights[0][i] = rdf32(p + 0x2c);
            if (s->weight_count > 1)
                s->weights[1][i] = rdf32(p + 0x30);
            if (s->uv_set_count >= 3)
                s->uv[2][i] = Vec2{rdf32(p + 0x34), rdf32(p + 0x38)};
            if (has_specular)
                s->specular[i] = rd32(p + 0x3c);
        }
    } else {
        // v1/v2: colour is three floats; alpha comes from the material diffuse.
        // The exe multiplies by the material diffuse when the material is lit;
        // lit status is not derivable from a v1/v2 header (no samples).
        for (uint32_t i = 0; i < nv; i++) {
            const uint8_t *p = data + voff + static_cast<size_t>(i) * mat.stride;
            Vec3 pos = {rdf32(p), rdf32(p + 4), rdf32(p + 8)};
            Vec3 nrm = {rdf32(p + 0xc), rdf32(p + 0x10), rdf32(p + 0x14)};
            Vec2 uv0 = {rdf32(p + 0x18), rdf32(p + 0x1c)};
            Vec2 uv1 = {rdf32(p + 0x20), rdf32(p + 0x24)};
            s->positions[i] = pos;
            s->normals[i] = nrm;
            s->uv[0][i] = uv0;
            s->uv[1][i] = uv1;
            s->diffuse[i] = pack_argb(mat.diffuse.a, rdf32(p + 0x28),
                                      rdf32(p + 0x2c), rdf32(p + 0x30));
        }
    }
    return true;
}

bool read_indices(const uint8_t *data, size_t ioff, uint32_t nf, Section *s)
{
    s->indices.resize(static_cast<size_t>(nf) * 3);
    for (size_t i = 0; i < s->indices.size(); i++)
        // The exe writes these through a u16 index buffer, truncating values
        // above 0xffff; three shipped files use index 0x10000.
        s->indices[i] = rd32(data + ioff + 4 * i);
    return true;
}

bool parse_sequential(const uint8_t *data, size_t size, uint32_t version, Mesh *m,
                      std::string *err)
{
    size_t off = 8;
    if (version == 1 || version == 2) {
        const size_t header = version == 1 ? 0x38 : 0x34;
        const size_t stride = 0x34;
        while (off < size) {
            if (!have(size, off, header))
                return fail(err, "truncated legacy material record", off);
            Material mat = decode_legacy_material(data + off, version);
            size_t nvoff = off + header;
            if (!have(size, nvoff, 4))
                return fail(err, "missing vertex count", nvoff);
            uint32_t nv = rd32(data + nvoff);
            size_t voff = nvoff + 4;
            if (nv > (size - voff) / stride)
                return fail(err, "vertex data overruns file", voff);
            size_t nend = voff + static_cast<size_t>(nv) * stride;
            if (!have(size, nend, 4))
                return fail(err, "missing face count", nend);
            uint32_t nf = rd32(data + nend);
            size_t ioff = nend + 4;
            if (nf > (size - ioff) / 12)
                return fail(err, "index data overruns file", ioff);

            Section s;
            s.material = static_cast<uint32_t>(m->materials.size());
            decode_sequential_vertices(data, voff, nv, version, mat, &s);
            read_indices(data, ioff, nf, &s);
            m->materials.push_back(std::move(mat));
            m->sections.push_back(std::move(s));
            off = ioff + static_cast<size_t>(nf) * 12;
        }
        return true;
    }

    // v3 (and v5 after the Steam remap).
    while (off < size) {
        if (size - off < 4)
            return fail(err, "trailing bytes after last record", off);
        uint32_t record_size = rd32(data + off);
        if (record_size == 0) {
            off += 4;
            if (off != size)
                return fail(err, "data after zero terminator", off);
            break;
        }
        if (record_size < 0x20)
            return fail(err, "record size below 0x20", off);
        if (record_size > size - off - 4)
            return fail(err, "material record overruns file", off);

        uint8_t buf[kSequentialMaterialSize];
        memset(buf, 0, sizeof buf);
        wr32(buf + 0x3c, kSequentialMinStride);
        for (int i = 0; i < 8; i++)
            wr16(buf + 0x2c + 2 * i, 0xffffu);
        wr16(buf + 0x40, 0xffffu);
        wr16(buf + 0x42, 0xffffu);
        wr16(buf + 0x44, 0xffffu);
        size_t copy = record_size < kSequentialMaterialSize ? record_size
                                                           : kSequentialMaterialSize;
        if (record_size >= 0x2c && (rd32(data + off + 0x28) & 0x8u) != 0 &&
            record_size >= 0x44) {
            size_t cap = rd16(data + off + 0x42);
            if (cap < copy)
                copy = cap;
        }
        memcpy(buf, data + off, copy);
        Material mat = decode_sequential_material(buf);
        if (mat.stride < kSequentialMinStride)
            return fail(err, "vertex stride below 0x2c", off);

        size_t nvoff = off + record_size;
        if (!have(size, nvoff, 4))
            return fail(err, "missing vertex count", nvoff);
        uint32_t nv = rd32(data + nvoff);
        size_t voff = nvoff + 4;
        if (nv > (size - voff) / mat.stride)
            return fail(err, "vertex data overruns file", voff);
        size_t nend = voff + static_cast<size_t>(nv) * mat.stride;
        if (!have(size, nend, 4))
            return fail(err, "missing face count", nend);
        uint32_t nf = rd32(data + nend);
        size_t ioff = nend + 4;
        if (nf > (size - ioff) / 12)
            return fail(err, "index data overruns file", ioff);

        Section s;
        s.material = static_cast<uint32_t>(m->materials.size());
        decode_sequential_vertices(data, voff, nv, version, mat, &s);
        read_indices(data, ioff, nf, &s);
        m->materials.push_back(std::move(mat));
        m->sections.push_back(std::move(s));
        off = ioff + static_cast<size_t>(nf) * 12;
    }
    return true;
}

size_t decoder_size(uint32_t decoder)
{
    switch (decoder) {
    case 0:
    case 1:
    case 5:
    case 7:
        return 8;
    case 2:
        return 12;
    case 3:
        return 16;
    case 4:
        return 20;
    case 6:
        return 32;
    case 8:
        return 36;
    default:
        return 4;
    }
}

bool parse_material_blob(const uint8_t *p, size_t n, Material *out, std::string *err,
                         size_t base)
{
    if (n < 12)
        return fail(err, "truncated material blob", base);
    out->type = rd32(p);
    out->flags = rd32(p + 4);
    uint32_t count = rd32(p + 8);
    size_t o = 12;
    for (uint32_t i = 0; i < count; i++) {
        if (n - o < 4)
            return fail(err, "material parameter overruns blob", base + o);
        uint32_t id = rd32(p + o);
        uint32_t decoder = id >> 24;
        uint32_t subtype = id & 0xffffffu;
        size_t sz = decoder_size(decoder);
        if (sz > n - o)
            return fail(err, "material parameter overruns blob", base + o);
        const uint8_t *q = p + o + 4;
        switch (decoder) {
        case 0: { // packed 0xAARRGGBB
            Color c = color_argb(rd32(q));
            if (subtype == 0)
                out->diffuse = c;
            else if (subtype == 1)
                out->ambient = c;
            else if (subtype == 2)
                out->specular = c;
            break;
        }
        case 1: { // float
            float f = rdf32(q);
            if (subtype == 0)
                out->specular_power = f;
            else if (subtype == 1)
                out->spheremap_strength = f;
            break;
        }
        case 6: { // texture / UV record
            TextureRecord t;
            t.subtype = subtype;
            t.index = rd32(q);
            t.source_uv = static_cast<int32_t>(rd32(q + 4));
            t.unknown = rd32(q + 8);
            t.scale_u = rdf32(q + 12);
            t.scale_v = rdf32(q + 16);
            t.offset_u = rdf32(q + 20);
            t.offset_v = rdf32(q + 24);
            out->textures.push_back(t);
            break;
        }
        case 8: // 32-byte NUL-padded name
            out->name = read_name(q, 32);
            break;
        default: // decoders 2/3/4/5/7 and unknown: consume, no in-memory decode
            break;
        }
        o += sz;
    }
    if (o != n)
        return fail(err, "material blob not consumed exactly", base + o);
    return true;
}

struct VertexLayout {
    uint32_t weight_count = 0;
    uint32_t uv_count = 0;
    bool position = false;
    bool beta = false;
    bool normal = false;
    bool diffuse = false;
    bool specular = false;
    size_t fixed = 0; // bytes without the UV pairs
};

bool decode_vertex_layout(uint32_t flags, VertexLayout *l, std::string *err, size_t base)
{
    uint32_t weight_bits = ((flags & 0x4u) ? 1u : 0u) + ((flags & 0x8u) ? 1u : 0u) +
                           ((flags & 0x10u) ? 1u : 0u);
    if (weight_bits > 1)
        return fail(err, "multiple blend-weight flags in vertex chunk", base);
    l->weight_count = (flags & 0x4u) ? 1u : ((flags & 0x8u) ? 2u : ((flags & 0x10u) ? 3u : 0u));
    l->uv_count = popcount(flags & 0xfe00u);
    l->position = (flags & 0x1u) != 0;
    l->beta = (flags & 0x20u) != 0;
    l->normal = (flags & 0x40u) != 0;
    l->diffuse = (flags & 0x80u) != 0;
    l->specular = (flags & 0x100u) != 0;
    l->fixed = (l->position ? 12u : 0u) + l->weight_count * 4u + (l->beta ? 4u : 0u) +
               (l->normal ? 12u : 0u) + (l->diffuse ? 4u : 0u) + (l->specular ? 4u : 0u);
    return true;
}

bool parse_vertex_chunk(const uint8_t *p, size_t n, Section *s, std::string *err,
                        size_t base)
{
    if (n < 8)
        return fail(err, "truncated vertex chunk", base);
    uint32_t nv = rd32(p);
    uint32_t flags = rd32(p + 4);
    size_t rest = n - 8;
    size_t stride = 0;
    if (nv == 0) {
        if (rest != 0)
            return fail(err, "empty vertex chunk with data", base);
    } else {
        if (rest % nv != 0)
            return fail(err, "vertex stride is not integral", base);
        stride = rest / nv;
    }

    VertexLayout layout;
    if (!decode_vertex_layout(flags, &layout, err, base))
        return false;
    size_t need = layout.fixed + 8 * layout.uv_count;
    if (stride < need)
        return fail(err, "vertex stride shorter than its flag-gated fields", base);

    s->flags = flags;
    s->stride = static_cast<uint32_t>(stride);
    s->uv_set_count = layout.uv_count;
    s->weight_count = layout.weight_count;
    s->positions.resize(nv);
    if (layout.normal)
        s->normals.resize(nv);
    if (layout.diffuse)
        s->diffuse.resize(nv);
    if (layout.specular)
        s->specular.resize(nv);
    for (uint32_t k = 0; k < layout.weight_count; k++)
        s->weights[k].resize(nv);
    if (layout.beta)
        s->beta.resize(nv);
    for (uint32_t k = 0; k < layout.uv_count; k++)
        s->uv[k].resize(nv);

    for (uint32_t i = 0; i < nv; i++) {
        const uint8_t *v = p + 8 + static_cast<size_t>(i) * stride;
        size_t o = 0;
        if (layout.position) {
            s->positions[i] = Vec3{rdf32(v + o), rdf32(v + o + 4), rdf32(v + o + 8)};
            o += 12;
        }
        for (uint32_t k = 0; k < layout.weight_count; k++) {
            s->weights[k][i] = rdf32(v + o);
            o += 4;
        }
        if (layout.beta) {
            s->beta[i] = rdf32(v + o);
            o += 4;
        }
        if (layout.normal) {
            s->normals[i] = Vec3{rdf32(v + o), rdf32(v + o + 4), rdf32(v + o + 8)};
            o += 12;
        }
        if (layout.diffuse) {
            s->diffuse[i] = rd32(v + o);
            o += 4;
        }
        if (layout.specular) {
            s->specular[i] = rd32(v + o);
            o += 4;
        }
        for (uint32_t k = 0; k < layout.uv_count; k++) {
            s->uv[k][i] = Vec2{rdf32(v + o), rdf32(v + o + 4)};
            o += 8;
        }
    }

    if (s->mode == -1) {
        if (flags & 0x2u)
            s->mode = 2;
        else if (flags & 0x4u)
            s->mode = 3;
        else if (flags & 0x8u)
            s->mode = 4;
        else if (flags & 0x10u)
            s->mode = 5;
    }
    return true;
}

bool parse_chunked(const uint8_t *data, size_t size, Mesh *m, std::string *err)
{
    if (size < 12)
        return fail(err, "truncated chunked header");
    uint32_t nchunks = rd32(data + 8);
    if (nchunks > (size - 12) / 8)
        return fail(err, "chunk directory overruns file", 8);
    size_t off = 12 + static_cast<size_t>(nchunks) * 8;
    int32_t current_material = -1;
    int32_t current_section = -1;

    while (off < size) {
        if (!have(size, off, 8))
            return fail(err, "truncated chunk header", off);
        uint32_t type = rd32(data + off);
        uint32_t chunk_size = rd32(data + off + 4);
        if (chunk_size < 8)
            return fail(err, "chunk size below 8", off);
        if (chunk_size > size - off)
            return fail(err, "chunk overruns file", off);
        const uint8_t *p = data + off + 8;
        size_t n = chunk_size - 8;

        switch (type) {
        case 0: { // material blob, starts a new group
            Material mat;
            if (!parse_material_blob(p, n, &mat, err, off + 8))
                return false;
            m->materials.push_back(std::move(mat));
            current_material = static_cast<int32_t>(m->materials.size()) - 1;
            break;
        }
        case 1: { // new section, mode 0
            if (n != 4)
                return fail(err, "bad section chunk size", off);
            Section s;
            s.mode = 0;
            s.material = current_material < 0 ? kNoMaterial
                                              : static_cast<uint32_t>(current_material);
            m->sections.push_back(std::move(s));
            current_section = static_cast<int32_t>(m->sections.size()) - 1;
            break;
        }
        case 2: { // new section, mode 1, one palette pair
            if (n != 12)
                return fail(err, "bad palette-pair chunk size", off);
            Section s;
            s.mode = 1;
            s.palette = {rd32(p + 4), rd32(p + 8)};
            s.material = current_material < 0 ? kNoMaterial
                                              : static_cast<uint32_t>(current_material);
            m->sections.push_back(std::move(s));
            current_section = static_cast<int32_t>(m->sections.size()) - 1;
            break;
        }
        case 3: { // new section, palette list, mode derived from vertex flags
            if (n < 8)
                return fail(err, "truncated palette chunk", off);
            uint32_t count = rd32(p + 4);
            if (count > (n - 8) / 4 || n != 8 + static_cast<size_t>(count) * 4)
                return fail(err, "bad palette chunk size", off);
            Section s;
            s.mode = -1;
            s.palette.resize(count);
            for (uint32_t i = 0; i < count; i++)
                s.palette[i] = rd32(p + 8 + static_cast<size_t>(i) * 4);
            s.material = current_material < 0 ? kNoMaterial
                                              : static_cast<uint32_t>(current_material);
            m->sections.push_back(std::move(s));
            current_section = static_cast<int32_t>(m->sections.size()) - 1;
            break;
        }
        case 4: { // vertices for the current section
            if (current_section < 0)
                return fail(err, "vertex chunk without a section", off);
            if (!parse_vertex_chunk(p, n, &m->sections[static_cast<size_t>(current_section)],
                                    err, off + 8))
                return false;
            break;
        }
        case 5: { // u16 triangle indices for the current section
            if (n < 4)
                return fail(err, "truncated index chunk", off);
            uint32_t count = rd32(p);
            if (count > (n - 4) / 2 || n != 4 + static_cast<size_t>(count) * 2)
                return fail(err, "bad index chunk size", off);
            if (count % 3 != 0)
                return fail(err, "index count is not a triangle multiple", off);
            if (current_section < 0)
                return fail(err, "index chunk without a section", off);
            Section &s = m->sections[static_cast<size_t>(current_section)];
            s.indices16.resize(count);
            for (uint32_t i = 0; i < count; i++) {
                uint16_t index = rd16(p + 4 + static_cast<size_t>(i) * 2);
                if (index >= s.positions.size())
                    return fail(err, "index outside the section vertex range",
                                off + 12 + static_cast<size_t>(i) * 2);
                s.indices16[i] = index;
            }
            break;
        }
        default:
            return fail(err, "unknown chunk type", off);
        }
        off += chunk_size;
    }
    return true;
}

void compute_bounds(Mesh *m)
{
    Bounds b;
    for (const Section &s : m->sections) {
        for (const Vec3 &p : s.positions) {
            if (!b.valid) {
                b.min = b.max = p;
                b.valid = true;
                continue;
            }
            if (p.x < b.min.x)
                b.min.x = p.x;
            if (p.y < b.min.y)
                b.min.y = p.y;
            if (p.z < b.min.z)
                b.min.z = p.z;
            if (p.x > b.max.x)
                b.max.x = p.x;
            if (p.y > b.max.y)
                b.max.y = p.y;
            if (p.z > b.max.z)
                b.max.z = p.z;
        }
    }
    if (!b.valid) {
        b.min = Vec3{};
        b.max = Vec3{};
    }
    m->bounds = b;
}

} // namespace

Vec3 Bounds::center() const
{
    return Vec3{(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f};
}

Vec3 Bounds::half_extent() const
{
    return Vec3{(max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f, (max.z - min.z) * 0.5f};
}

float Bounds::radius() const
{
    float lo = min.x * min.x + min.y * min.y + min.z * min.z;
    float hi = max.x * max.x + max.y * max.y + max.z * max.z;
    return sqrtf(lo > hi ? lo : hi);
}

bool parse(const void *data, size_t size, Mesh *out, std::string *err,
           const ParseOptions &opt)
{
    *out = Mesh();
    if (err)
        err->clear();
    if (!data || size < 8)
        return fail(err, "truncated mesh header");
    const uint8_t *p = static_cast<const uint8_t *>(data);
    if (rd32(p) != kMagic)
        return fail(err, "invalid mesh magic");
    uint32_t version = rd32(p + 4);
    if (version < 1 || version > 6)
        return fail(err, "unsupported mesh version");

    uint32_t effective = version;
    if (opt.steam_version_remap) {
        if (effective == 5)
            effective = 3;
        else if (effective == 6)
            effective = 4;
    }
    out->version = version;
    out->chunked = effective > 3;

    bool ok = effective <= 3 ? parse_sequential(p, size, effective, out, err)
                             : parse_chunked(p, size, out, err);
    if (!ok) {
        *out = Mesh();
        return false;
    }
    compute_bounds(out);
    return true;
}

bool load(const char *path, Mesh *out, std::string *err, const ParseOptions &opt)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err)
            *err = std::string("cannot open ") + path;
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    if (f.bad()) {
        if (err)
            *err = std::string("cannot read ") + path;
        return false;
    }
    return parse(data.data(), data.size(), out, err, opt);
}

} // namespace mesh
