#include "render/scene.h"

#include <math.h>
#include <string.h>

#include <algorithm>

namespace render {

namespace {

// notes/12 §6.1 + notes/17 §2.2: sequential record slots at material
// +0x2c/+0x30/+0x32/+0x34/+0x38 = base, no-specular, spheremap, second/mix,
// bump. A material is textured when it has a base texture.
bool has_base_texture(const mesh::Material &m)
{
    return m.texture_slots[0] >= 0;
}

bool is_bright(const mesh::Material &m)
{
    const float scale = m.ambient_scale;
    return fabsf(m.ambient.r * scale) > 0.1f || fabsf(m.ambient.g * scale) > 0.1f ||
           fabsf(m.ambient.b * scale) > 0.1f;
}

bool wants_specular(const mesh::Material &m)
{
    return is_bright(m) && m.specular_power > 8.0f && m.texture_slots[2] < 0;
}

// notes/12 §6.1: SCX record -> material class id. Chunked blobs carry the id
// directly; sequential records are selected from the flags/texture slots.
uint32_t select_material_type(const mesh::Material &m)
{
    if (m.type != 0)
        return m.type & 0xffffffu;
    if (!has_base_texture(m)) {
        uint32_t type = 0x30; // Color
        if (m.texture_slots[3] >= 0)
            type += 2; // ColorSpheremap / ColorSpecularSpheremap
        if (wants_specular(m))
            type += 1; // specular variant
        return type;
    }
    uint32_t type = 0; // Tex
    if (m.flags & 0x40u)
        type = 1; // MixTA
    else if (m.flags & 0x1000u)
        type = 2; // MixVCA
    else if ((m.flags & 0x101u) == 0x100u)
        type = 3; // ColorMixTex
    if (wants_specular(m))
        type += 4; // specular
    if (m.texture_slots[3] >= 0)
        type += 8; // spheremap
    return type;
}

// Texture parameter ids are 0x06xxxxNN: low byte 0 base, 1 second/mix,
// 3 spheremap, 7 bump (notes/17 §3.3).
int16_t record_index(const mesh::Material &m, uint32_t kind)
{
    for (const mesh::TextureRecord &t : m.textures)
        if ((t.subtype & 0xffu) == kind)
            return t.index > 0x7fff ? static_cast<int16_t>(-1)
                                    : static_cast<int16_t>(t.index);
    return -1;
}

const Material *material_of(const RenderMesh &mesh, uint32_t section)
{
    if (mesh.materials.empty() || section >= mesh.sections.size())
        return nullptr;
    uint32_t index = mesh.sections[section].material;
    if (index >= mesh.materials.size())
        index = 0;
    return &mesh.materials[index];
}

uint32_t blend_mode_for(uint32_t blend_type)
{
    switch (blend_type) {
    case 1: return 1;
    case 2: return 5;
    case 3: return 6;
    case 4: return 7;
    case 5: return 8;
    default: return 0;
    }
}

const char *group_marker(GroupKind kind)
{
    switch (kind) {
    case GroupKind::Horizon: return "group:horizon";
    case GroupKind::Ground: return "group:ground";
    case GroupKind::ObjectPrelit: return "group:object_prelit";
    case GroupKind::ObjectMerge: return "group:object_merge";
    case GroupKind::Object: return "group:object";
    case GroupKind::ObjectPrelit2: return "group:objectprelit";
    case GroupKind::Transparent: return "group:transparent";
    case GroupKind::PrelitTransparent: return "group:prelittransparent";
    case GroupKind::Particle: return "group:particle";
    case GroupKind::Skidmark1: return "group:skidmark1";
    case GroupKind::Skidmark2: return "group:skidmark2";
    case GroupKind::Text: return "group:text";
    default: return "group";
    }
}

uint32_t float_bits(float value)
{
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

} // namespace

RenderMesh build_render_mesh(const mesh::Mesh &source)
{
    RenderMesh out;
    out.materials.reserve(source.materials.size());
    for (const mesh::Material &m : source.materials) {
        Material r;
        r.type = select_material_type(m);
        r.flags = m.flags;
        r.colors.diffuse[0] = m.diffuse.r;
        r.colors.diffuse[1] = m.diffuse.g;
        r.colors.diffuse[2] = m.diffuse.b;
        r.colors.diffuse[3] = m.diffuse.a;
        r.colors.ambient[0] = m.ambient.r * m.ambient_scale;
        r.colors.ambient[1] = m.ambient.g * m.ambient_scale;
        r.colors.ambient[2] = m.ambient.b * m.ambient_scale;
        r.colors.ambient[3] = m.ambient.a;
        r.colors.specular[0] = m.specular.r;
        r.colors.specular[1] = m.specular.g;
        r.colors.specular[2] = m.specular.b;
        r.colors.specular[3] = m.specular.a;
        r.colors.power = m.specular_power;
        r.transparent = m.diffuse.a < 0.99f || (m.flags & 1u) != 0;
        r.alpha_test = (m.flags & 0x800u) != 0;
        r.alpha_ref = r.alpha_test ? 0x38u : 0u;
        r.texture_index[0] = m.texture_slots[0]; // base
        r.texture_index[1] = m.texture_slots[4]; // second/mix
        r.texture_index[2] = m.texture_slots[3]; // spheremap
        r.texture_index[3] = m.texture_slots[6]; // bump
        if (!m.textures.empty()) {
            if (r.texture_index[0] < 0)
                r.texture_index[0] = record_index(m, 0x00u);
            if (r.texture_index[1] < 0)
                r.texture_index[1] = record_index(m, 0x01u);
            if (r.texture_index[2] < 0)
                r.texture_index[2] = record_index(m, 0x03u);
            if (r.texture_index[3] < 0)
                r.texture_index[3] = record_index(m, 0x07u);
        }
        r.passes = (r.type & 0xffffffu) == 0xfffffcu ? 4u : 1u;
        out.materials.push_back(r);
    }

    out.sections.reserve(source.sections.size());
    for (const mesh::Section &s : source.sections) {
        RenderSection r;
        r.material = s.material == mesh::kNoMaterial ? 0u : s.material;
        if (r.material >= out.materials.size())
            r.material = 0;
        const bool lit = !out.materials.empty() &&
                         (out.materials[r.material].flags & 0x80000000u) != 0;
        const size_t count = s.positions.size();
        r.vertex_count = static_cast<uint32_t>(count);
        r.vertices.resize(count * kVertexStride);
        for (size_t i = 0; i < count; ++i) {
            float *v = reinterpret_cast<float *>(r.vertices.data() + i * kVertexStride);
            v[0] = s.positions[i].x * mesh::kRenderScale;
            v[1] = s.positions[i].y * mesh::kRenderScale;
            v[2] = s.positions[i].z * mesh::kRenderScale;
            if (i < s.normals.size()) {
                v[3] = s.normals[i].x;
                v[4] = s.normals[i].y;
                v[5] = s.normals[i].z;
            } else {
                v[3] = v[4] = v[5] = 0.0f;
            }
            const uint32_t vertex_color =
                i < s.diffuse.size() ? (s.diffuse[i] | 0xff000000u) : 0xffffffffu;
            uint32_t diffuse = 0xffffffffu;
            uint32_t specular = 0;
            if (lit) {
                diffuse = 0xff808080u;
                specular = vertex_color | 0xff000000u;
            } else {
                diffuse = vertex_color | 0xff000000u;
                specular = i < s.specular.size() ? s.specular[i] : 0u;
            }
            memcpy(&v[6], &diffuse, sizeof(diffuse));
            memcpy(&v[7], &specular, sizeof(specular));
            for (size_t set = 0; set < 3; ++set) {
                if (set < s.uv.size() && i < s.uv[set].size()) {
                    v[8 + set * 2] = s.uv[set][i].u;
                    v[9 + set * 2] = s.uv[set][i].v;
                } else {
                    v[8 + set * 2] = v[9 + set * 2] = 0.0f;
                }
            }
        }
        r.palette = s.palette;
        r.skinned = !s.palette.empty();
        r.bone_count = static_cast<uint32_t>(s.palette.size());
        r.visible = true;
        if (!s.indices16.empty()) {
            r.indices = s.indices16;
        } else {
            r.indices.reserve(s.indices.size());
            for (uint32_t index : s.indices)
                r.indices.push_back(static_cast<uint16_t>(index & 0xffffu));
        }
        r.primitive_count = static_cast<uint32_t>(r.indices.size() / 3);
        r.base_vertex = 0;
        r.start_index = 0;
        out.sections.push_back(std::move(r));
    }
    return out;
}

void RenderMesh::upload(Device &device)
{
    for (RenderSection &s : sections) {
        if (!s.vertices.empty() && s.vb == 0)
            s.vb = device.create_vertex_buffer(s.vertices.data(), s.vertices.size(),
                                               s.fvf);
        if (!s.indices.empty() && s.ib == 0)
            s.ib = device.create_index_buffer(s.indices.data(),
                                              s.indices.size() * sizeof(uint16_t),
                                              false);
    }
}

void RenderMesh::release(Device &device)
{
    for (RenderSection &s : sections) {
        if (s.vb) {
            device.release_vertex_buffer(s.vb);
            s.vb = 0;
        }
        if (s.ib) {
            device.release_index_buffer(s.ib);
            s.ib = 0;
        }
    }
}

bool MaterialBucket::matches(const RenderMesh *m, uint32_t key, uint32_t blend,
                             uint32_t var) const
{
    return mesh == m && mesh_key == key && blend_type == blend && variant == var;
}

void RenderGroup::add(const RenderInstance &instance, uint32_t section,
                      uint32_t blend_type, uint32_t variant)
{
    const RenderMesh *mesh = instance.mesh;
    if (!mesh || section >= mesh->sections.size())
        return;
    auto it = std::find_if(buckets_.begin(), buckets_.end(), [&](const MaterialBucket &b) {
        return b.matches(mesh, mesh->key, blend_type, variant);
    });
    if (it == buckets_.end()) {
        MaterialBucket bucket;
        bucket.mesh = mesh;
        bucket.mesh_key = mesh->key;
        bucket.blend_type = blend_type;
        bucket.variant = variant;
        buckets_.push_back(bucket);
        it = buckets_.end() - 1;
    }
    CacheEntry entry;
    entry.instance = &instance;
    entry.mesh = mesh;
    entry.section = section;
    entry.light_set = instance.light_set;
    it->entries.push_back(entry);
}

bool RenderGroup::empty() const
{
    for (const MaterialBucket &bucket : buckets_)
        if (!bucket.entries.empty())
            return false;
    return true;
}

void RenderGroup::render(Renderer &renderer)
{
    if (empty())
        return;
    Device &device = renderer.device();
    device.mark(group_marker(kind_));
    switch (kind_) {
    case GroupKind::Horizon:
        renderer.set_lighting(false, false);
        renderer.begin_sky_view();
        break;
    case GroupKind::Ground:
    case GroupKind::ObjectMerge:
    case GroupKind::Object:
        renderer.set_lighting(true, false);
        break;
    case GroupKind::ObjectPrelit:
    case GroupKind::ObjectPrelit2:
    case GroupKind::PrelitTransparent:
        renderer.set_lighting(false, false);
        break;
    default:
        break;
    }

    for (MaterialBucket &bucket : buckets_) {
        if (bucket.entries.empty())
            continue;
        const Material *material = material_of(*bucket.mesh, bucket.entries[0].section);
        if (!material)
            continue;
        // Cache entries are grouped by their light-instance key (notes/12 §4.3:
        // binary heap keyed on the light-set pointer); equal keys keep order.
        std::stable_sort(bucket.entries.begin(), bucket.entries.end(),
                         [](const CacheEntry &a, const CacheEntry &b) {
                             return a.light_set < b.light_set;
                         });
        const uint32_t mode = blend_mode_for(bucket.blend_type);
        uint32_t passes = renderer.begin_material(*material, mode);
        if (passes == 0)
            continue;
        uint32_t pass = 0;
        do {
            renderer.set_material_pass(*material, pass);
            for (const CacheEntry &entry : bucket.entries) {
                if (!entry.mesh || entry.section >= entry.mesh->sections.size())
                    continue;
                const RenderSection &section = entry.mesh->sections[entry.section];
                renderer.set_world(entry.instance->world);
                if (entry.instance->palette.size() > 1)
                    renderer.apply_palette(entry.instance->palette, section.palette);
                renderer.draw_section(section);
            }
        } while (++pass < passes);
        renderer.end_material(*material);
    }

    if (kind_ == GroupKind::Horizon)
        renderer.end_sky_view();
}

Renderer::Renderer(Device &device) : device_(&device) {}

void Renderer::present()
{
    device_->present();
}

void Renderer::set_default_states()
{
    for (uint32_t stage = 0; stage < 8; ++stage) {
        device_->set_sampler_state(stage, ss::MINFILTER, 2); // D3DTEXF_LINEAR
        device_->set_sampler_state(stage, ss::MAGFILTER, 2);
        device_->set_sampler_state(stage, ss::MIPFILTER, 2);
        device_->set_sampler_state(stage, ss::ADDRESSU, 1); // D3DTADDRESS_WRAP
        device_->set_sampler_state(stage, ss::ADDRESSV, 1);
    }
    device_->set_render_state(rs::SRCBLEND, blend::SRCALPHA);
    device_->set_render_state(rs::DESTBLEND, blend::INVSRCALPHA);
    device_->set_texture_stage_state(0, tss::COLOROP, d3dtop::MODULATE);
    device_->set_texture_stage_state(0, tss::COLORARG1, d3dta::TEXTURE);
    device_->set_texture_stage_state(0, tss::COLORARG2, d3dta::CURRENT);
    device_->set_texture_stage_state(0, tss::ALPHAOP, d3dtop::MODULATE);
    device_->set_texture_stage_state(0, tss::ALPHAARG1, d3dta::TEXTURE);
    device_->set_texture_stage_state(0, tss::ALPHAARG2, d3dta::TFACTOR);
    device_->set_render_state(rs::CULLMODE, cull_mode_);
    device_->set_render_state(rs::COLORVERTEX, 1);
    device_->set_render_state(rs::LOCALVIEWER, 1);
    device_->set_render_state(rs::NORMALIZENORMALS, 0);
    device_->set_render_state(rs::LIGHTING, 1);
    device_->set_render_state(rs::FOGVERTEXMODE, 3); // D3DFOG_LINEAR
    device_->set_render_state(rs::RANGEFOGENABLE, 1);
    device_->set_render_state(rs::AMBIENT, ambient_);
    MaterialColors defaults;
    device_->set_material(defaults);
    device_->set_render_state(rs::DIFFUSEMATERIALSOURCE, 0);
    device_->set_render_state(rs::SPECULARMATERIALSOURCE, 0);
    device_->set_render_state(rs::AMBIENTMATERIALSOURCE, 0);
    device_->set_render_state(rs::EMISSIVEMATERIALSOURCE, 0);
    device_->set_render_state(rs::SPECULARENABLE, 0);
    for (uint32_t i = 0; i < 8; ++i) {
        Light light;
        light.diffuse = light.specular = light.ambient = 0;
        device_->set_light(i, light);
        device_->light_enable(i, false);
    }
    lighting_ = true;
    device_->set_transform(xf::WORLD, world_);
    device_->set_transform(xf::VIEW, view_);
    device_->set_transform(xf::PROJECTION, projection_);
    if (neutral_texture_ == 0) {
        neutral_texture_ = device_->create_texture(4, 4, 1, 3);
        device_->fill_texture(neutral_texture_, 0x80808080u, 4, 4);
    }
}

bool Renderer::begin_frame()
{
    if (!device_->begin_scene())
        return false;
    // notes/12 §2.2 beginFrame.
    set_lighting(false, true);
    device_->set_render_state(rs::CULLMODE, cull_mode_);
    device_->set_render_state(rs::ZWRITEENABLE, 1);
    device_->set_render_state(rs::SRCBLEND, blend::ONE);
    device_->set_render_state(rs::DESTBLEND, blend::ONE);
    device_->set_render_state(rs::ALPHABLENDENABLE, 0);
    device_->set_render_state(rs::ZFUNC, cmp::LESSEQUAL);
    blend_enabled_ = false;
    alpha_test_enabled_ = false;
    return true;
}

void Renderer::end_frame()
{
    if (blend_enabled_)
        reset_blend_mode();
    if (offscreen_)
        end_offscreen();
    device_->end_scene();
}

void Renderer::render_frame(int backbuffer_width, int backbuffer_height,
                            Viewport *viewports, size_t viewport_count,
                            const std::vector<OverlayQuad> &overlays)
{
    present(); // previous frame, notes/12 §2.1 step 3
    device_->mark("frame:present");
    if (!begin_frame())
        return;
    device_->mark("frame:begin");
    for (size_t i = 0; i < viewport_count; ++i) {
        Viewport &viewport = viewports[i];
        viewport.update_rect(backbuffer_width, backbuffer_height);
        device_->set_viewport(viewport.pixel_x, viewport.pixel_y,
                              viewport.pixel_width, viewport.pixel_height, 0.0f, 1.0f);
        for (Camera *camera : viewport.cameras) {
            if (!camera)
                continue;
            camera->setup(*this, viewport);
            camera->render(*this, viewport);
        }
    }
    if (!overlays.empty())
        device_->mark("frame:overlays");
    for (const OverlayQuad &quad : overlays)
        draw_color_quad(quad.texture, quad.x, quad.y, quad.w, quad.h, quad.color);
    end_frame();
    device_->mark("frame:end");
}

void Renderer::set_projection(const Mat4 &projection)
{
    projection_ = projection;
    device_->set_transform(xf::PROJECTION, projection);
    dirty_ |= 1u;
}

void Renderer::set_view(const Mat4 &view, const Mat4 &inverse_view)
{
    view_ = view;
    inverse_view_ = inverse_view;
    device_->set_transform(xf::VIEW, view);
    dirty_ |= 1u;
}

void Renderer::set_world(const Mat4 &world)
{
    world_ = world;
    device_->set_transform(xf::WORLD, world);
    dirty_ |= 2u;
}

void Renderer::set_fog(bool enabled, float start, float end, uint32_t color)
{
    fog_enabled_ = enabled;
    fog_start_ = start;
    fog_end_ = end;
    fog_color_ = color;
    device_->set_render_state(rs::FOGENABLE, enabled ? 1u : 0u);
    if (enabled) {
        device_->set_render_state(rs::FOGSTART, float_bits(start));
        device_->set_render_state(rs::FOGEND, float_bits(end));
        device_->set_render_state(rs::FOGCOLOR, color);
    }
    dirty_ |= 4u;
}

void Renderer::set_ambient(uint32_t argb)
{
    ambient_ = argb;
    device_->set_render_state(rs::AMBIENT, argb);
}

void Renderer::set_sun_direction(float x, float y, float z)
{
    sun_[0] = x;
    sun_[1] = y;
    sun_[2] = z;
}

void Renderer::set_lighting(bool enabled, bool force)
{
    if (!force && lighting_ == enabled)
        return;
    lighting_ = enabled;
    device_->set_render_state(rs::LIGHTING, enabled ? 1u : 0u);
    if (!enabled) {
        for (uint32_t i = 0; i < 8; ++i)
            device_->light_enable(i, false);
        return;
    }
    const uint32_t count = std::min<uint32_t>(active_light_count_, 8);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t index = active_lights_[i];
        if (index < light_table_.size())
            set_light_slot(i, light_table_[index], true);
    }
    for (uint32_t i = count; i < 8; ++i)
        device_->light_enable(i, false);
}

void Renderer::set_light_table(const Light *lights, size_t count)
{
    light_table_.assign(lights, lights + count);
    if (light_table_.size() > 64)
        light_table_.resize(64);
    dirty_ |= 0x18u;
}

void Renderer::set_light_slot(uint32_t slot, const Light &light, bool enabled)
{
    device_->set_light(slot, light);
    device_->light_enable(slot, enabled);
}

void Renderer::set_active_lights(const uint32_t *indices, size_t count)
{
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(count, 8));
    for (uint32_t i = 0; i < 8; ++i) {
        if (i < n) {
            const uint32_t index = indices[i];
            if (i >= active_light_count_ || active_lights_[i] != index) {
                active_lights_[i] = index;
                if (lighting_ && index < light_table_.size())
                    set_light_slot(i, light_table_[index], true);
            }
        } else if (i < active_light_count_) {
            device_->light_enable(i, false);
        }
    }
    active_light_count_ = n;
    dirty_ |= 0x18u;
}

void Renderer::set_blend_mode(uint32_t mode, uint32_t zflag)
{
    static const struct {
        uint32_t src, dst, op;
        bool alpha_test;
    } modes[8] = {
        {blend::SRCALPHA, blend::ONE, blend_op::ADD, true},             // 0 additive
        {blend::SRCALPHA, blend::ONE, blend_op::SUBTRACT, true},        // 1 subt.
        {blend::SRCALPHA, blend::INVSRCALPHA, blend_op::ADD, true},     // 2 alpha
        {blend::ZERO, blend::SRCCOLOR, blend_op::ADD, false},           // 3 multiply
        {blend::ONE, blend::ONE, blend_op::MAX, false},                 // 4 max
        {blend::ONE, blend::ONE, blend_op::MIN, false},                 // 5 min
        {blend::BLENDFACTOR, blend::INVBLENDFACTOR, blend_op::ADD, false},
        {blend::ZERO, blend::ONE, blend_op::ADD, false},                // 7 mask
    };
    if (zflag == 0)
        device_->set_render_state(rs::ZENABLE, 0);
    else if (zflag == 1)
        device_->set_render_state(rs::ZWRITEENABLE, 0);
    const auto &m = modes[mode & 7u];
    blend_enabled_ = true;
    alpha_test_enabled_ = m.alpha_test;
    blend_src_ = m.src;
    blend_dst_ = m.dst;
    blend_op_ = m.op;
    device_->set_render_state(rs::ALPHATESTENABLE, m.alpha_test ? 1u : 0u);
    if (m.alpha_test) {
        device_->set_render_state(rs::ALPHAFUNC, cmp::GREATER);
        device_->set_render_state(rs::ALPHAREF, 4);
    }
    device_->set_render_state(rs::ALPHABLENDENABLE, 1);
    device_->set_render_state(rs::SRCBLEND, blend_src_);
    device_->set_render_state(rs::DESTBLEND, blend_dst_);
    device_->set_render_state(rs::BLENDOP, blend_op_);
}

void Renderer::reset_blend_mode()
{
    blend_enabled_ = false;
    alpha_test_enabled_ = false;
    device_->set_render_state(rs::ALPHABLENDENABLE, 0);
    device_->set_render_state(rs::ALPHATESTENABLE, 0);
    device_->set_render_state(rs::ZENABLE, 1);
    device_->set_render_state(rs::ZWRITEENABLE, 1);
}

void Renderer::reapply_blend()
{
    device_->set_render_state(rs::ALPHABLENDENABLE, blend_enabled_ ? 1u : 0u);
    device_->set_render_state(rs::SRCBLEND, blend_src_);
    device_->set_render_state(rs::DESTBLEND, blend_dst_);
    device_->set_render_state(rs::BLENDOP, blend_op_);
}

void Renderer::set_blend_factor(uint32_t argb)
{
    blend_factor_ = argb;
    device_->set_render_state(rs::BLENDFACTOR, argb);
}

void Renderer::begin_sky_view()
{
    Mat4 inv = inverse_view_;
    inv.m[12] = inv.m[13] = inv.m[14] = 0.0f;
    device_->set_transform(xf::VIEW, mat4_inverse_affine(inv));
    device_->set_render_state(rs::ZFUNC, cmp::ALWAYS);
    device_->set_render_state(rs::ZWRITEENABLE, 0);
    if (fog_enabled_)
        device_->set_render_state(rs::FOGENABLE, 0);
}

void Renderer::end_sky_view()
{
    device_->set_transform(xf::VIEW, view_);
    device_->set_render_state(rs::ZFUNC, cmp::LESSEQUAL);
    device_->set_render_state(rs::ZWRITEENABLE, 1);
    device_->set_render_state(rs::FOGENABLE, fog_enabled_ ? 1u : 0u);
}

uint32_t Renderer::begin_material(const Material &material, uint32_t mode)
{
    const uint32_t passes = material.passes;
    material_mode_ = mode;
    if (passes == 0)
        return 0;
    device_->set_material(material.colors);
    if (mode == 3) {
        device_->set_render_state(rs::DIFFUSEMATERIALSOURCE, 1); // COLOR1
        device_->set_render_state(rs::SPECULARMATERIALSOURCE, 2); // COLOR2
    }
    if (material.alpha_ref != 0) {
        device_->set_render_state(rs::ALPHAREF, material.alpha_ref);
        device_->set_render_state(rs::ALPHATESTENABLE, 1);
    }
    if (material.two_sided)
        device_->set_render_state(rs::CULLMODE, cull::NONE);
    return passes;
}

void Renderer::bind_stage(uint32_t stage, Texture texture, bool clamp_address)
{
    if (texture == 0)
        texture = neutral_texture_;
    device_->set_texture(stage, texture);
    device_->set_sampler_state(stage, ss::ADDRESSU, clamp_address ? 3u : 1u);
    device_->set_sampler_state(stage, ss::ADDRESSV, clamp_address ? 3u : 1u);
}

void Renderer::unbind_stage(uint32_t stage)
{
    device_->set_texture(stage, 0);
    device_->set_sampler_state(stage, ss::ADDRESSU, 1);
    device_->set_sampler_state(stage, ss::ADDRESSV, 1);
}

void Renderer::set_material_pass(const Material &material, uint32_t pass)
{
    const uint32_t type = material.type & 0xffffffu;
    // The palette transform pair is applied per cache entry by apply_palette
    // (notes/12 §4.3) before draw_section flushes the queued bank 6.
    if (type >= 0x30u && type <= 0x33u) {
        // Color / ColorSpecular / ColorSpheremap / ColorSpecularSpheremap.
        const bool specular = type == 0x31u || type == 0x33u;
        device_->set_render_state(rs::SPECULARENABLE, specular && pass == 0 ? 1u : 0u);
        if (pass == 0) {
            device_->set_texture_stage_state(0, tss::COLOROP, d3dtop::SELECTARG2);
            device_->set_texture_stage_state(0, tss::ALPHAOP, d3dtop::SELECTARG2);
            const uint32_t alpha =
                static_cast<uint32_t>(material.colors.diffuse[3] * 255.0f) & 0xffu;
            device_->set_render_state(rs::TEXTUREFACTOR, alpha << 24);
        } else {
            device_->set_texture_stage_state(0, tss::COLOROP, d3dtop::MODULATE);
            device_->set_texture_stage_state(0, tss::ALPHAOP, d3dtop::MODULATE);
        }
        return;
    }
    if (type == 0xfffffcu) {
        // ProjectedLight: 4 passes (notes/12 §6.2).
        device_->set_render_state(rs::ZENABLE, 0);
        device_->set_render_state(rs::TEXTUREFACTOR, 0xffffffffu);
        device_->set_texture_stage_state(0, tss::COLOROP, d3dtop::MODULATE);
        device_->set_texture_stage_state(0, tss::COLORARG1, d3dta::TEXTURE);
        return;
    }

    // Texture families 0..0x2f (+4 specular, +8 spheremap; low bits select
    // the Tex/MixTA/MixVCA/ColorMixTex combiner, notes/12 §6.2).
    const uint32_t base = type & 3u;
    const bool specular = (type & 4u) != 0;
    const bool sphere = (type & 8u) != 0;
    if (specular)
        device_->set_render_state(rs::SPECULARENABLE, pass == 0 ? 1u : 0u);
    if (pass == 0) {
        if (base == 2)
            set_lighting(false, false);
        bind_stage(0, material.textures[0], true);
        if (base >= 1u)
            bind_stage(1, material.textures[1], true);
        else if (sphere)
            bind_stage(1, material.textures[2], true);
        switch (base) {
        case 1: // MixTA
            device_->set_texture_stage_state(0, tss::COLOROP, d3dtop::SELECTARG1);
            device_->set_texture_stage_state(0, tss::ALPHAOP, d3dtop::SELECTARG1);
            device_->set_texture_stage_state(1, tss::COLOROP,
                                             d3dtop::BLENDTEXTUREALPHA);
            device_->set_texture_stage_state(1, tss::ALPHAOP,
                                             d3dtop::BLENDTEXTUREALPHA);
            break;
        case 2: // MixVCA
            device_->set_texture_stage_state(0, tss::COLOROP,
                                             d3dtop::MODULATEALPHAADDTOCOLOR);
            device_->set_texture_stage_state(1, tss::COLOROP, d3dtop::MULTIPLYADD);
            device_->set_texture_stage_state(1, tss::COLORARG2, 4);
            break;
        case 3: // ColorMixTex
            device_->set_texture_stage_state(0, tss::COLOROP, d3dtop::SELECTARG1);
            device_->set_texture_stage_state(0, tss::COLORARG1, d3dta::TEXTURE);
            device_->set_texture_stage_state(1, tss::COLOROP, d3dtop::MODULATE);
            break;
        default: // Tex
            device_->set_texture_stage_state(0, tss::COLOROP, d3dtop::MODULATE);
            device_->set_texture_stage_state(0, tss::COLORARG1, d3dta::TEXTURE);
            device_->set_texture_stage_state(0, tss::COLORARG2, d3dta::DIFFUSE);
            device_->set_texture_stage_state(0, tss::ALPHAOP, d3dtop::MODULATE);
            device_->set_texture_stage_state(0, tss::ALPHAARG1, d3dta::TEXTURE);
            device_->set_texture_stage_state(0, tss::ALPHAARG2, d3dta::DIFFUSE);
            break;
        }
        if (sphere && base == 0) {
            device_->set_texture_stage_state(1, tss::COLOROP, d3dtop::ADD);
            device_->set_texture_stage_state(1, tss::COLORARG1, d3dta::TEXTURE);
            device_->set_texture_stage_state(1, tss::COLORARG2, d3dta::CURRENT);
        }
        const uint32_t alpha =
            static_cast<uint32_t>(material.colors.diffuse[3] * 255.0f) & 0xffu;
        device_->set_render_state(rs::TEXTUREFACTOR, alpha << 24);
    } else {
        unbind_stage(0);
        if (base >= 1u || sphere)
            unbind_stage(1);
        device_->set_render_state(rs::SPECULARENABLE, 0);
        device_->set_texture_stage_state(0, tss::COLOROP, d3dtop::MODULATE);
        device_->set_texture_stage_state(1, tss::COLOROP, d3dtop::DISABLE);
    }
}

void Renderer::end_material(const Material &material)
{
    for (uint32_t stage = 0; stage < 4; ++stage)
        unbind_stage(stage);
    device_->set_texture_stage_state(0, tss::COLOROP, d3dtop::MODULATE);
    device_->set_texture_stage_state(0, tss::COLORARG1, d3dta::TEXTURE);
    device_->set_texture_stage_state(0, tss::COLORARG2, d3dta::CURRENT);
    device_->set_texture_stage_state(0, tss::ALPHAOP, d3dtop::MODULATE);
    device_->set_texture_stage_state(0, tss::ALPHAARG1, d3dta::TEXTURE);
    device_->set_texture_stage_state(0, tss::ALPHAARG2, d3dta::TFACTOR);
    if (material_mode_ == 3) {
        device_->set_render_state(rs::DIFFUSEMATERIALSOURCE, 0);
        device_->set_render_state(rs::SPECULARMATERIALSOURCE, 0);
    }
    if (material.alpha_ref != 0)
        device_->set_render_state(rs::ALPHATESTENABLE, 0);
    device_->set_render_state(rs::CULLMODE, cull_mode_);
    device_->set_render_state(rs::SPECULARENABLE, 0);
    // A palette may have been applied per entry regardless of the blend mode.
    device_->set_render_state(rs::VERTEXBLEND, 0);
    if (vertex_shader_version_ < 2)
        device_->set_software_vertex_processing(false);
    material_mode_ = 0xffffffffu;
}

void Renderer::apply_palette(const std::vector<Mat4> &base,
                             const std::vector<uint32_t> &indices)
{
    palette_base_ = base;
    palette_indices_ = indices;
    if (base.size() < 2 || indices.empty())
        return;
    if (vertex_shader_version_ < 2)
        device_->set_software_vertex_processing(true);
    device_->set_transform(xf::WORLD, base[indices[0] % base.size()]);
    if (base.size() == 2) {
        device_->set_transform(xf::WORLD1, base[indices[1] % base.size()]);
        device_->set_render_state(rs::VERTEXBLEND, 1);
    }
    queue_constant(0, ConstantBank::Palette);
}

void Renderer::draw_section(const RenderSection &section)
{
    flush_constants(); // notes/12 §4.3 step 1 (FUN_00442400)
    device_->set_fvf(section.fvf);
    device_->set_stream_source(0, section.vb, section.stride);
    if (section.primitive_count == 0)
        return;
    if (section.ib != 0) {
        device_->set_indices(section.ib);
        device_->draw_indexed_primitive(PrimitiveType::TriangleList,
                                        section.base_vertex, 0,
                                        section.vertex_count, section.start_index,
                                        section.primitive_count);
    } else {
        device_->draw_primitive(PrimitiveType::TriangleList, section.base_vertex,
                                section.primitive_count);
    }
}

void Renderer::draw_color_quad(Texture texture, float x, float y, float w, float h,
                               uint32_t color)
{
    const float quad[16] = {
        x,     y,     0.0f, 1.0f,
        x + w, y,     0.0f, 1.0f,
        x,     y + h, 0.0f, 1.0f,
        x + w, y + h, 0.0f, 1.0f,
    };
    const bool old_blend = blend_enabled_;
    const bool old_alpha = alpha_test_enabled_;
    const uint32_t old_src = blend_src_, old_dst = blend_dst_, old_op = blend_op_;

    device_->set_texture(0, texture);
    device_->set_render_state(rs::ZENABLE, 0);
    device_->set_render_state(rs::ALPHABLENDENABLE, 1);
    device_->set_render_state(rs::SRCBLEND, blend::SRCALPHA);
    device_->set_render_state(rs::DESTBLEND, blend::INVSRCALPHA);
    device_->set_render_state(rs::BLENDOP, blend_op::ADD);
    device_->set_render_state(rs::TEXTUREFACTOR, color);
    // notes/12 §3.3: SELECTARG1 with raw arg 3 (TFACTOR) on colour and alpha;
    // the XYZRHW quad carries no diffuse/texcoords.
    device_->set_texture_stage_state(0, tss::COLOROP, d3dtop::SELECTARG1);
    device_->set_texture_stage_state(0, tss::COLORARG1, d3dta::TFACTOR);
    device_->set_texture_stage_state(0, tss::COLORARG2, d3dta::TFACTOR);
    device_->set_texture_stage_state(0, tss::ALPHAOP, d3dtop::SELECTARG1);
    device_->set_texture_stage_state(0, tss::ALPHAARG1, d3dta::TFACTOR);
    device_->set_texture_stage_state(0, tss::ALPHAARG2, d3dta::TFACTOR);
    device_->set_fvf(kOverlayFvf);
    device_->draw_primitive_up(PrimitiveType::TriangleStrip, 2, quad, 0x10);

    device_->set_render_state(rs::ZENABLE, 1);
    device_->set_texture_stage_state(0, tss::COLOROP, d3dtop::MODULATE);
    device_->set_texture_stage_state(0, tss::COLORARG1, d3dta::TEXTURE);
    device_->set_texture_stage_state(0, tss::COLORARG2, d3dta::CURRENT);
    device_->set_texture_stage_state(0, tss::ALPHAOP, d3dtop::MODULATE);
    device_->set_texture_stage_state(0, tss::ALPHAARG1, d3dta::TEXTURE);
    device_->set_texture_stage_state(0, tss::ALPHAARG2, d3dta::TFACTOR);
    blend_enabled_ = old_blend;
    alpha_test_enabled_ = old_alpha;
    blend_src_ = old_src;
    blend_dst_ = old_dst;
    blend_op_ = old_op;
    reapply_blend();
}

void Renderer::draw_lens_flare(const Flare &flare)
{
    // Layout partial: notes/12 §3.5 marks the ghost-quad layout inferred.
    if (!flare.visible || flare.count == 0)
        return;
    device_->set_texture(0, flare.texture);
    device_->set_render_state(rs::ZENABLE, 0);
    device_->set_render_state(rs::ALPHABLENDENABLE, 1);
    device_->set_render_state(rs::SRCBLEND, blend::SRCALPHA);
    device_->set_render_state(rs::DESTBLEND, blend::ONE);
    device_->set_render_state(rs::BLENDOP, blend_op::ADD);
    device_->set_fvf(0x142u); // XYZ|DIFFUSE|TEX1
    device_->draw_primitive_up(PrimitiveType::TriangleList, flare.count * 2,
                               flare.params, 0x1cu);
    device_->set_render_state(rs::ZENABLE, 1);
}

void Renderer::draw_lines_3d(const DebugLine *lines, size_t count)
{
    if (!lines || count == 0)
        return;
    // DebugLine is a 0x1c CPU record; D3D needs XYZ|DIFFUSE (0x142) vertices.
    std::vector<float> verts;
    verts.reserve(count * 8);
    for (size_t i = 0; i < count; ++i) {
        for (int end = 0; end < 2; ++end) {
            verts.push_back(lines[i].p[end * 3 + 0]);
            verts.push_back(lines[i].p[end * 3 + 1]);
            verts.push_back(lines[i].p[end * 3 + 2]);
            verts.push_back(static_cast<float>(lines[i].color));
        }
    }
    set_world(mat4_identity());
    device_->set_fvf(0x142u);
    device_->draw_primitive_up(PrimitiveType::LineList,
                               static_cast<uint32_t>(count), verts.data(), 0x10);
}

void Renderer::draw_line_2d(float x0, float y0, float x1, float y1, uint32_t color)
{
    const float verts[8] = {x0, y0, 0.0f, 1.0f, x1, y1, 0.0f, 1.0f};
    (void)color;
    device_->set_fvf(kOverlayFvf);
    device_->draw_primitive_up(PrimitiveType::LineList, 1, verts, 0x10);
}

void Renderer::queue_constant(uint32_t reg, ConstantBank bank)
{
    PendingConstant pending;
    pending.reg = reg;
    pending.bank = bank;
    pending_.push_back(pending);
}

uint32_t Renderer::bank_group(ConstantBank bank)
{
    switch (bank) {
    case ConstantBank::WorldViewProjection:
    case ConstantBank::TransposeWorldViewProjection:
    case ConstantBank::WorldView:
    case ConstantBank::TransposeWorldView: return 3u;
    case ConstantBank::ViewProjection:
    case ConstantBank::TransposeViewProjection:
    case ConstantBank::ViewZ:
    case ConstantBank::Projection:
    case ConstantBank::TransposeProjection: return 1u;
    case ConstantBank::World:
    case ConstantBank::InverseWorld:
    case ConstantBank::TransposeInverseWorld:
    case ConstantBank::Palette: return 2u;
    case ConstantBank::Lights: return 0x18u;
    case ConstantBank::Fog: return 4u;
    default: return 0u; // CameraPosition: no dirty group (notes/12 §4.3)
    }
}

void Renderer::compute_bank(ConstantBank bank, std::vector<float> *data) const
{
    data->clear();
    const Mat4 wvp = mat4_mul(world_, mat4_mul(view_, projection_));
    const Mat4 vp = mat4_mul(view_, projection_);
    auto push_matrix = [&](const Mat4 &m) {
        data->insert(data->end(), m.m, m.m + 16);
    };
    switch (bank) {
    case ConstantBank::WorldViewProjection: push_matrix(wvp); break;
    case ConstantBank::ViewProjection: push_matrix(vp); break;
    case ConstantBank::World: push_matrix(world_); break;
    case ConstantBank::InverseWorld: push_matrix(mat4_inverse_affine(world_)); break;
    case ConstantBank::TransposeInverseWorld:
        push_matrix(mat4_transpose(mat4_inverse_affine(world_)));
        break;
    case ConstantBank::Palette: {
        const size_t count = std::min<size_t>(palette_base_.size(), 16);
        for (size_t i = 0; i < count; ++i)
            data->insert(data->end(), palette_base_[i].m, palette_base_[i].m + 12);
        break;
    }
    case ConstantBank::TransposeViewProjection: push_matrix(mat4_transpose(vp)); break;
    case ConstantBank::TransposeWorldViewProjection:
        push_matrix(mat4_transpose(wvp));
        break;
    case ConstantBank::Lights: {
        const uint32_t count = std::min<uint32_t>(active_light_count_, 4);
        for (uint32_t i = 0; i < count; ++i) {
            Light light;
            const uint32_t index = active_lights_[i];
            if (index < light_table_.size())
                light = light_table_[index];
            data->push_back(light.direction[0]);
            data->push_back(light.direction[1]);
            data->push_back(light.direction[2]);
            data->push_back(0.0f);
            data->push_back(static_cast<float>((light.diffuse >> 16) & 0xffu) / 255.0f);
            data->push_back(static_cast<float>((light.diffuse >> 8) & 0xffu) / 255.0f);
            data->push_back(static_cast<float>(light.diffuse & 0xffu) / 255.0f);
            data->push_back(light.intensity);
            data->push_back(light.position[0]);
            data->push_back(light.position[1]);
            data->push_back(light.position[2]);
            data->push_back(light.range);
        }
        break;
    }
    case ConstantBank::Fog: {
        const float span = fog_end_ - fog_start_;
        data->push_back(span != 0.0f ? 1.0f / span : 0.0f);
        data->push_back(span != 0.0f ? fog_end_ / span : 0.0f);
        data->push_back(0.0f);
        data->push_back(0.0f);
        break;
    }
    case ConstantBank::ViewZ: {
        data->push_back(view_.m[2]);
        data->push_back(view_.m[6]);
        data->push_back(view_.m[10]);
        data->push_back(view_.m[14]);
        break;
    }
    case ConstantBank::WorldView: push_matrix(mat4_mul(world_, view_)); break;
    case ConstantBank::TransposeWorldView:
        push_matrix(mat4_transpose(mat4_mul(world_, view_)));
        break;
    case ConstantBank::Projection: push_matrix(projection_); break;
    case ConstantBank::TransposeProjection: push_matrix(mat4_transpose(projection_)); break;
    case ConstantBank::CameraPosition:
        data->push_back(inverse_view_.m[12]);
        data->push_back(inverse_view_.m[13]);
        data->push_back(inverse_view_.m[14]);
        data->push_back(1.0f);
        break;
    }
}

void Renderer::flush_constants()
{
    uint32_t cleared = 0;
    std::vector<float> data;
    for (const PendingConstant &pending : pending_) {
        const uint32_t group = bank_group(pending.bank);
        if (group != 0 && (dirty_ & group) == 0)
            continue;
        compute_bank(pending.bank, &data);
        if (data.empty())
            continue;
        device_->set_vertex_shader_constant_f(
            pending.reg, data.data(), static_cast<uint32_t>(data.size() / 4));
        cleared |= group;
    }
    pending_.clear();
    dirty_ &= ~cleared;
}

void Renderer::immediate_constant(uint32_t reg, float x, float y, float z, float w)
{
    const float data[4] = {x, y, z, w};
    device_->set_vertex_shader_constant_f(reg, data, 1);
}

void Renderer::begin_offscreen(Texture target)
{
    device_->begin_offscreen(target);
    offscreen_ = true;
}

void Renderer::end_offscreen()
{
    device_->end_offscreen();
    offscreen_ = false;
}

void Renderer::begin_shadow_map(Texture target, const Mat4 &light_view,
                                const Mat4 &light_projection)
{
    // notes/12 §3.4 B: vf83 shadow-map render target begin.
    begin_offscreen(target);
    device_->set_transform(xf::VIEW, light_view);
    device_->set_transform(xf::PROJECTION, light_projection);
    device_->set_render_state(rs::ZENABLE, 0);
    device_->set_render_state(rs::LIGHTING, 0);
    device_->set_render_state(rs::ZFUNC, cmp::ALWAYS);
    device_->set_render_state(rs::ALPHABLENDENABLE, 0);
    device_->set_render_state(rs::TEXTUREFACTOR, 0xffffffffu);
}

void Renderer::end_shadow_map()
{
    // notes/12 §3.4 B: vf84 restore.
    device_->set_render_state(rs::ZENABLE, 1);
    device_->set_render_state(rs::LIGHTING, lighting_ ? 1u : 0u);
    device_->set_render_state(rs::ZFUNC, cmp::LESSEQUAL);
    device_->set_texture_stage_state(0, tss::COLOROP, d3dtop::MODULATE);
    device_->set_texture_stage_state(0, tss::COLORARG2, d3dta::TEXTURE);
    device_->set_transform(xf::VIEW, view_);
    device_->set_transform(xf::PROJECTION, projection_);
    end_offscreen();
}

void Viewport::update_rect(int backbuffer_width, int backbuffer_height)
{
    pixel_x = static_cast<int>(x * static_cast<float>(backbuffer_width));
    pixel_y = static_cast<int>(y * static_cast<float>(backbuffer_height));
    pixel_width = static_cast<int>(w * static_cast<float>(backbuffer_width));
    pixel_height = static_cast<int>(h * static_cast<float>(backbuffer_height));
    if (pixel_width < 1)
        pixel_width = 1;
    if (pixel_height < 1)
        pixel_height = 1;
}

Camera::Camera()
    : groups_{RenderGroup(GroupKind::Horizon), RenderGroup(GroupKind::Ground),
              RenderGroup(GroupKind::ObjectPrelit), RenderGroup(GroupKind::ObjectMerge),
              RenderGroup(GroupKind::Object), RenderGroup(GroupKind::ObjectPrelit2),
              RenderGroup(GroupKind::Transparent),
              RenderGroup(GroupKind::PrelitTransparent), RenderGroup(GroupKind::Particle),
              RenderGroup(GroupKind::Skidmark1), RenderGroup(GroupKind::Skidmark2),
              RenderGroup(GroupKind::Text)}
{
    projected_light_material.type = 0xfffffcu;
    projected_light_material.passes = 4;
}

RenderGroup &Camera::group(GroupKind kind)
{
    return groups_[static_cast<size_t>(kind)];
}

const RenderGroup &Camera::group(GroupKind kind) const
{
    return groups_[static_cast<size_t>(kind)];
}

void Camera::setup(Renderer &renderer, const Viewport &viewport)
{
    aspect = viewport.pixel_height != 0
                 ? static_cast<float>(viewport.pixel_width) /
                       static_cast<float>(viewport.pixel_height)
                 : 1.0f;
    renderer.set_projection(
        mat4_perspective_fov_rh(fov, aspect, near_plane, far_plane));
    renderer.set_view(view, inverse_view);
    renderer.set_fog(fog_enabled, fog_start, fog_end, fog_color);
    renderer.set_sun_direction(sun_direction[0], sun_direction[1], sun_direction[2]);
    renderer.set_light_table(light_table.data(), light_table.size());
    renderer.set_active_lights(nullptr, 0);
    renderer.queue_constant(0, ConstantBank::WorldViewProjection);
}

void Camera::render(Renderer &renderer, const Viewport &viewport)
{
    if (skip_render)
        return;
    Device &device = renderer.device();
    const uint32_t mask = clear_mask & viewport.clear_flags;

    // 1. clear.
    device.mark("pass:1 clear");
    if (mask != 0) {
        uint32_t flags = 0;
        if (mask & clear_flags::ZBUFFER)
            flags |= clear_flags::ZBUFFER;
        if (mask & clear_flags::TARGET)
            flags |= clear_flags::TARGET;
        device.clear(flags, kClearColor, kClearZ, 0);
    }

    // 2. horizon/sky.
    device.mark("pass:2 horizon");
    if (render_sky)
        group(GroupKind::Horizon).render(renderer);

    // 3. ground, prelit, merge, object, objectprelit.
    device.mark("pass:3 scene");
    if (ground_enabled)
        group(GroupKind::Ground).render(renderer);
    if (prelit_enabled)
        group(GroupKind::ObjectPrelit).render(renderer);
    if (merge_enabled)
        group(GroupKind::ObjectMerge).render(renderer);
    if (object_enabled) {
        group(GroupKind::Object).render(renderer);
        group(GroupKind::ObjectPrelit2).render(renderer);
    }

    // 4. first skidmark half.
    device.mark("pass:4 skidmarks");
    renderer.set_lighting(false, false);
    if (skidmarks_enabled) {
        group(GroupKind::Skidmark1).render(renderer);
        group(GroupKind::Skidmark2).render(renderer);
    }

    // 5. pre-shadow hooks.
    device.mark("pass:5 pre_shadow_hooks");
    for (CameraHook *hook : hooks)
        if (hook && hook->pre_shadow_enabled)
            hook->pre_shadow(*this);

    // 6. shadows (projected ground quads are a separate CPU path; the
    // optional shadow-map group is driven here).
    device.mark("pass:6 shadows");
    if (shadows_enabled) {
        renderer.set_blend_mode(2, 1);
        if (shadow_size > 0.0f && renderer.shadow_target() != 0) {
            renderer.begin_shadow_map(renderer.shadow_target(), shadow_view,
                                      shadow_projection);
            const uint32_t old_cull = renderer.cull_mode();
            device.set_render_state(rs::CULLMODE, cull::NONE);
            for (RenderInstance *caster : shadow_casters) {
                if (!caster || !caster->mesh)
                    continue;
                renderer.set_world(caster->world);
                for (const RenderSection &section : caster->mesh->sections)
                    renderer.draw_section(section);
            }
            device.set_render_state(rs::CULLMODE, old_cull);
            renderer.end_shadow_map();
        }
        renderer.reset_blend_mode();
    }

    // 7. projected dynamic lights.
    device.mark("pass:7 projected_lights");
    if (projected_lights_enabled && !projected_receivers.empty()) {
        renderer.set_lighting(false, false);
        if (!projected_light_indices.empty())
            renderer.set_active_lights(projected_light_indices.data(),
                                       projected_light_indices.size());
        renderer.set_world(mat4_identity());
        uint32_t passes = renderer.begin_material(projected_light_material, 0);
        if (passes != 0) {
            uint32_t pass = 0;
            do {
                renderer.set_material_pass(projected_light_material, pass);
                for (RenderInstance *receiver : projected_receivers) {
                    if (!receiver || !receiver->mesh)
                        continue;
                    renderer.set_world(receiver->world);
                    for (const RenderSection &section : receiver->mesh->sections)
                        if (section.visible)
                            renderer.draw_section(section);
                }
            } while (++pass < passes);
            renderer.end_material(projected_light_material);
        }
    }

    // 8. prelit transparent, transparent.
    device.mark("pass:8 transparent");
    if (prelit_enabled)
        group(GroupKind::PrelitTransparent).render(renderer);
    if (object_enabled)
        group(GroupKind::Transparent).render(renderer);

    // 9. second skidmark half.
    device.mark("pass:9 skidmarks2");
    renderer.set_lighting(false, false);
    if (skidmarks_enabled)
        group(GroupKind::Skidmark2).render(renderer);

    // 10. directional lights + particles.
    device.mark("pass:10 particles");
    if (!directional_lights.empty())
        renderer.set_active_lights(directional_lights.data(), directional_lights.size());
    renderer.set_lighting(false, false);
    group(GroupKind::Particle).render(renderer);

    // 11. lens flares.
    device.mark("pass:11 flares");
    if (flares_enabled) {
        renderer.begin_offscreen(renderer.flare_target());
        renderer.end_offscreen();
        renderer.set_blend_mode(0, 1);
        for (const Flare &flare : flares) {
            if (!flare.visible || flare.count == 0)
                continue;
            renderer.draw_lens_flare(flare);
        }
        renderer.reset_blend_mode();
        renderer.set_active_lights(nullptr, 0);
    }

    // 12. 3-D text and post hooks.
    device.mark("pass:12 text");
    group(GroupKind::Text).render(renderer);
    for (CameraHook *hook : hooks)
        if (hook && hook->post_render_enabled)
            hook->post_render(*this);

    // 13. identity world and debug lines.
    device.mark("pass:13 debug_lines");
    renderer.set_world(mat4_identity());
    if (debug_lines_enabled && !debug_lines.empty())
        renderer.draw_lines_3d(debug_lines.data(), debug_lines.size());
}

} // namespace render
