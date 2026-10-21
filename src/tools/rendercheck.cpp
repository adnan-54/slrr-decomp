// rendercheck: self-check for the render subsystem on the recording backend.
//
// Loads a real SCX part mesh (FL_door.scx), builds the engine render mesh and
// camera, renders one frame in the notes/12 pass order, and asserts pass
// markers, matrices, fog/sky state, material-bucket ordering, material state
// bodies, the palette pair/constants, draw-call structure, text batching and
// the device state cache. Prints PASS/FAIL.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "mesh/scx.h"
#include "render/recording.h"
#include "render/scene.h"
#include "render/ui.h"

using namespace render;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(condition)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

bool near_eq(float a, float b, float epsilon = 1e-4f)
{
    return fabsf(a - b) <= epsilon;
}

uint32_t float_bits(float value)
{
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

uint32_t u32_at(const std::vector<uint8_t> &data, size_t offset)
{
    uint32_t value = 0;
    if (offset + sizeof(value) <= data.size())
        memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

uint32_t color_at(const std::vector<float> &floats, size_t vertex)
{
    uint32_t value = 0;
    memcpy(&value, &floats[vertex * 7 + 4], sizeof(value));
    return value;
}

int marker_record_index(const RecordingDevice &device, const char *name,
                        int from = 0)
{
    const std::vector<Record> &records = device.records();
    for (int i = from; i < static_cast<int>(records.size()); ++i)
        if (records[i].type == RecordType::Marker && records[i].text == name)
            return i;
    return -1;
}

int find_record(const RecordingDevice &device, RecordType type, int from = 0,
                int to = 0x7fffffff)
{
    const std::vector<Record> &records = device.records();
    for (int i = from; i < static_cast<int>(records.size()) && i < to; ++i)
        if (records[i].type == type)
            return i;
    return -1;
}

int find_render_state(const RecordingDevice &device, uint32_t state,
                      uint32_t value, int from, int to)
{
    const std::vector<Record> &records = device.records();
    for (int i = from; i < static_cast<int>(records.size()) && i < to; ++i)
        if (records[i].type == RecordType::RenderState &&
            records[i].state == state && records[i].value == value)
            return i;
    return -1;
}

int find_stage_state(const RecordingDevice &device, uint32_t stage,
                     uint32_t state, uint32_t value, int from, int to)
{
    const std::vector<Record> &records = device.records();
    for (int i = from; i < static_cast<int>(records.size()) && i < to; ++i)
        if (records[i].type == RecordType::StageState &&
            records[i].stage == stage && records[i].state == state &&
            records[i].value == value)
            return i;
    return -1;
}

int find_sampler_state(const RecordingDevice &device, uint32_t stage,
                       uint32_t state, uint32_t value, int from, int to)
{
    const std::vector<Record> &records = device.records();
    for (int i = from; i < static_cast<int>(records.size()) && i < to; ++i)
        if (records[i].type == RecordType::SamplerState &&
            records[i].stage == stage && records[i].state == state &&
            records[i].value == value)
            return i;
    return -1;
}

int find_texture(const RecordingDevice &device, uint32_t stage, uint64_t handle,
                 int from, int to)
{
    const std::vector<Record> &records = device.records();
    for (int i = from; i < static_cast<int>(records.size()) && i < to; ++i)
        if (records[i].type == RecordType::Texture &&
            records[i].stage == stage && records[i].handle == handle)
            return i;
    return -1;
}

int last_before(const RecordingDevice &device, RecordType type, int before)
{
    const std::vector<Record> &records = device.records();
    for (int i = before - 1; i >= 0; --i)
        if (records[i].type == type)
            return i;
    return -1;
}

size_t count_between(const RecordingDevice &device, RecordType type, int from,
                     int to)
{
    size_t n = 0;
    const std::vector<Record> &records = device.records();
    for (int i = from; i < static_cast<int>(records.size()) && i < to; ++i)
        if (records[i].type == type)
            ++n;
    return n;
}

std::vector<int> draws_between(const RecordingDevice &device, int from, int to)
{
    std::vector<int> result;
    const std::vector<Record> &records = device.records();
    for (int i = from; i < static_cast<int>(records.size()) && i < to; ++i)
        if (records[i].type == RecordType::DrawIndexed)
            result.push_back(i);
    return result;
}

} // namespace

int main(int argc, char **argv)
{
    const char *path = argc > 1
                           ? argv[1]
                           : "bin/the game/cars/racers/Axis_data/meshes/"
                             "FL_door.scx";

    // 1. Real SCX mesh -> render mesh.
    mesh::Mesh source;
    std::string error;
    if (!mesh::load(path, &source, &error)) {
        std::printf("FAIL load %s: %s\n", path, error.c_str());
        return 1;
    }
    CHECK(source.sections.size() == 2);
    CHECK(source.vertex_count() == 195);
    CHECK(source.triangle_count() == 65);

    RenderMesh mesh = build_render_mesh(source);
    CHECK(mesh.materials.size() == 2);
    CHECK(mesh.materials[0].type == 0x0du); // MixTASpecularSpheremap
    CHECK(mesh.materials[0].texture_index[0] == 0);
    CHECK(mesh.materials[0].texture_index[1] == 1);
    CHECK(mesh.materials[0].texture_index[2] == 2);
    CHECK(mesh.materials[0].texture_index[3] == -1);
    CHECK(mesh.materials[0].alpha_ref == 0);
    CHECK(!mesh.materials[0].alpha_test);
    CHECK(!mesh.materials[0].transparent);
    CHECK(mesh.materials[0].passes == 1);
    CHECK(mesh.materials[1].type == 0x00u); // Tex
    CHECK(mesh.materials[1].texture_index[0] == 3);
    CHECK(mesh.materials[1].texture_index[1] == -1);
    CHECK(mesh.sections.size() == 2);
    CHECK(mesh.sections[0].vertex_count == 129);
    CHECK(mesh.sections[0].primitive_count == 43);
    CHECK(mesh.sections[1].vertex_count == 66);
    CHECK(mesh.sections[1].primitive_count == 22);
    CHECK(mesh.sections[0].stride == kVertexStride);
    CHECK(mesh.sections[0].fvf == kVertexFvf);
    CHECK(mesh.sections[0].engine_fvf == kEngineFvf);
    CHECK(mesh.sections[0].indices.size() == 129);

    const float *vertex =
        reinterpret_cast<const float *>(mesh.sections[0].vertices.data());
    CHECK(near_eq(vertex[0], source.sections[0].positions[0].x * 0.1f));
    CHECK(near_eq(vertex[1], source.sections[0].positions[0].y * 0.1f));
    CHECK(near_eq(vertex[3], source.sections[0].normals[0].x));
    CHECK(u32_at(mesh.sections[0].vertices, 24) ==
          (source.sections[0].diffuse[0] | 0xff000000u));
    CHECK(near_eq(vertex[8], source.sections[0].uv[0][0].u));
    CHECK(mesh.sections[0].indices[2] ==
          static_cast<uint16_t>(source.sections[0].indices[2]));

    RecordingDevice device;
    Renderer renderer(device);

    Texture tex_base = device.create_texture(8, 8, 1, 3);
    Texture tex_mix = device.create_texture(8, 8, 1, 3);
    Texture tex_sphere = device.create_texture(8, 8, 1, 3);
    Texture tex_inner = device.create_texture(8, 8, 1, 3);
    mesh.materials[0].textures[0] = tex_base;
    mesh.materials[0].textures[1] = tex_mix;
    mesh.materials[0].textures[2] = tex_sphere;
    mesh.materials[1].textures[0] = tex_inner;
    mesh.upload(device);
    CHECK(mesh.sections[0].vb != 0);
    CHECK(mesh.sections[0].ib != 0);
    CHECK(mesh.sections[1].vb != 0);
    CHECK(mesh.sections[1].ib != 0);

    // Second mesh copy with a two-bone palette on section 1 (vertex blend).
    RenderMesh palette_mesh = build_render_mesh(source);
    palette_mesh.materials[1].textures[0] = tex_inner;
    palette_mesh.sections[1].palette = {0, 1};
    palette_mesh.upload(device);

    // 2. Default states (notes/12 §1.4).
    device.reset();
    renderer.set_default_states();
    CHECK(find_render_state(device, rs::LIGHTING, 1, 0, 0x7fffffff) >= 0);
    CHECK(find_render_state(device, rs::CULLMODE, 2, 0, 0x7fffffff) >= 0);
    CHECK(find_render_state(device, rs::SRCBLEND, blend::SRCALPHA, 0,
                            0x7fffffff) >= 0);
    CHECK(find_render_state(device, rs::DESTBLEND, blend::INVSRCALPHA, 0,
                            0x7fffffff) >= 0);
    CHECK(find_render_state(device, rs::FOGVERTEXMODE, 3, 0, 0x7fffffff) >= 0);
    CHECK(find_render_state(device, rs::RANGEFOGENABLE, 1, 0, 0x7fffffff) >= 0);
    CHECK(find_render_state(device, rs::COLORVERTEX, 1, 0, 0x7fffffff) >= 0);
    CHECK(find_render_state(device, rs::LOCALVIEWER, 1, 0, 0x7fffffff) >= 0);
    CHECK(find_render_state(device, rs::NORMALIZENORMALS, 0, 0, 0x7fffffff) >= 0);
    CHECK(find_render_state(device, rs::SPECULARENABLE, 0, 0, 0x7fffffff) >= 0);
    CHECK(find_render_state(device, rs::DIFFUSEMATERIALSOURCE, 0, 0,
                            0x7fffffff) >= 0);
    CHECK(find_render_state(device, rs::SPECULARMATERIALSOURCE, 0, 0,
                            0x7fffffff) >= 0);
    CHECK(find_stage_state(device, 0, tss::COLOROP, d3dtop::MODULATE, 0,
                           0x7fffffff) >= 0);
    CHECK(find_stage_state(device, 0, tss::ALPHAOP, d3dtop::MODULATE, 0,
                           0x7fffffff) >= 0);
    CHECK(find_stage_state(device, 0, tss::ALPHAARG2, d3dta::TFACTOR, 0,
                           0x7fffffff) >= 0);
    const Record *material = device.last(RecordType::Material);
    CHECK(material != nullptr);
    if (material) {
        CHECK(near_eq(material->material.diffuse[0], 1.0f));
        CHECK(near_eq(material->material.ambient[0], 1.0f));
        CHECK(near_eq(material->material.power, 0.0f));
    }
    CHECK(device.count(RecordType::Light) == 8);
    CHECK(device.count(RecordType::LightEnable) == 8);
    for (const Record &record : device.records())
        if (record.type == RecordType::Light)
            CHECK(record.light.type == 3);
    CHECK(device.count(RecordType::SamplerState) == 40); // 8 stages x 5 states
    for (uint32_t stage = 0; stage < 8; ++stage) {
        CHECK(find_sampler_state(device, stage, ss::MINFILTER, 2, 0,
                                 0x7fffffff) >= 0);
        CHECK(find_sampler_state(device, stage, ss::MAGFILTER, 2, 0,
                                 0x7fffffff) >= 0);
        CHECK(find_sampler_state(device, stage, ss::MIPFILTER, 2, 0,
                                 0x7fffffff) >= 0);
        CHECK(find_sampler_state(device, stage, ss::ADDRESSU, 1, 0,
                                 0x7fffffff) >= 0);
        CHECK(find_sampler_state(device, stage, ss::ADDRESSV, 1, 0,
                                 0x7fffffff) >= 0);
    }
    const Record *neutral = device.last(RecordType::CreateTexture);
    CHECK(neutral != nullptr && neutral->width == 4 && neutral->height == 4 &&
          neutral->slot == 3);

    // 3. Camera / viewport / frame.
    device.reset();
    Camera camera;
    camera.view = mat4_translation(-10.0f, -20.0f, -30.0f);
    camera.inverse_view = mat4_translation(10.0f, 20.0f, 30.0f);
    camera.fov = kDefaultFov;
    camera.near_plane = kDefaultNear;
    camera.far_plane = kDefaultFar;
    camera.fog_enabled = true;
    camera.fog_start = 50.0f;
    camera.fog_end = 250.0f;
    camera.fog_color = 0xff102030u;
    Light sun;
    sun.type = 3;
    sun.diffuse = 0xffffffffu;
    sun.direction[0] = 0.0f;
    sun.direction[1] = -1.0f;
    sun.direction[2] = 0.0f;
    camera.light_table.push_back(sun);

    RenderInstance horizon;
    horizon.mesh = &mesh;
    horizon.world = mat4_translation(0.0f, 50.0f, 0.0f);
    camera.group(GroupKind::Horizon).add(horizon, 1);

    const Mat4 world0 = mat4_translation(100.0f, 0.0f, 0.0f);
    const Mat4 world1 = mat4_translation(0.0f, 100.0f, 0.0f);
    const Mat4 world2 = mat4_translation(0.0f, 0.0f, 100.0f);
    RenderInstance instance0; // light-set key 2 -> drawn last
    instance0.mesh = &mesh;
    instance0.world = world2;
    instance0.light_set = 2;
    RenderInstance instance1; // light-set key 0 -> drawn first
    instance1.mesh = &mesh;
    instance1.world = world0;
    instance1.light_set = 0;
    RenderInstance instance2; // light-set key 1
    instance2.mesh = &mesh;
    instance2.world = world1;
    instance2.light_set = 1;
    // Insert unsorted to exercise the per-bucket light-set ordering.
    camera.group(GroupKind::Object).add(instance0, 0);
    camera.group(GroupKind::Object).add(instance1, 0);
    camera.group(GroupKind::Object).add(instance2, 0);

    const Mat4 palette0 = mat4_translation(1.0f, 2.0f, 3.0f);
    const Mat4 palette1 = mat4_translation(4.0f, 5.0f, 6.0f);
    RenderInstance palette_instance;
    palette_instance.mesh = &palette_mesh;
    palette_instance.world = mat4_translation(7.0f, 8.0f, 9.0f);
    palette_instance.light_set = 0;
    palette_instance.palette = {palette0, palette1};
    camera.group(GroupKind::Object).add(palette_instance, 1);

    DebugLine line;
    line.p[0] = 0.0f;
    line.p[1] = 0.0f;
    line.p[2] = 0.0f;
    line.p[3] = 10.0f;
    line.p[4] = 0.0f;
    line.p[5] = 0.0f;
    line.color = 0xff00ff00u;
    camera.debug_lines.push_back(line);

    Viewport viewport;
    viewport.x = 0.25f;
    viewport.y = 0.5f;
    viewport.w = 0.5f;
    viewport.h = 0.5f;
    viewport.cameras.push_back(&camera);

    std::vector<OverlayQuad> overlays;
    OverlayQuad quad;
    quad.texture = tex_base;
    quad.x = 10.0f;
    quad.y = 20.0f;
    quad.w = 64.0f;
    quad.h = 32.0f;
    quad.color = 0x80ffffffu;
    overlays.push_back(quad);

    renderer.render_frame(640, 480, &viewport, 1, overlays);

    // Pass order (notes/12 §3.2): frame scheduling + 13 camera steps.
    const std::vector<std::string> expected_markers = {
        "frame:present",       "frame:begin",
        "pass:1 clear",        "pass:2 horizon",
        "group:horizon",       "pass:3 scene",
        "group:object",        "pass:4 skidmarks",
        "pass:5 pre_shadow_hooks", "pass:6 shadows",
        "pass:7 projected_lights", "pass:8 transparent",
        "pass:9 skidmarks2",   "pass:10 particles",
        "pass:11 flares",      "pass:12 text",
        "pass:13 debug_lines", "frame:overlays",
        "frame:end",
    };
    CHECK(device.marker_names() == expected_markers);
    const std::vector<Record> &records = device.records();
    CHECK(!records.empty());
    if (!records.empty()) {
        CHECK(records.front().type == RecordType::Present);
        CHECK(records.back().type == RecordType::Marker);
        CHECK(records.back().text == "frame:end");
    }
    const int begin_index = find_record(device, RecordType::BeginScene);
    const int end_index = find_record(device, RecordType::EndScene);
    CHECK(begin_index >= 0 && end_index > begin_index);
    CHECK(device.count(RecordType::Present) == 1);

    const int pass1 = marker_record_index(device, "pass:1 clear");
    const int pass2 = marker_record_index(device, "pass:2 horizon");
    const int pass3 = marker_record_index(device, "pass:3 scene");
    const int pass4 = marker_record_index(device, "pass:4 skidmarks");
    const int pass13 = marker_record_index(device, "pass:13 debug_lines");
    const int overlay_marker = marker_record_index(device, "frame:overlays");
    CHECK(pass1 >= 0 && pass2 > pass1 && pass3 > pass2 && pass4 > pass3);
    CHECK(pass13 > pass4 && overlay_marker > pass13);

    // Viewport and clear.
    const int viewport_index =
        find_record(device, RecordType::Viewport, begin_index, pass1);
    CHECK(viewport_index >= 0);
    if (viewport_index >= 0) {
        const Record &vp = records[viewport_index];
        CHECK(vp.vx == 160 && vp.vy == 240 && vp.vw == 320 && vp.vh == 240);
        CHECK(near_eq(vp.min_z, 0.0f) && near_eq(vp.max_z, 1.0f));
    }
    const int clear_index = find_record(device, RecordType::Clear, pass1, pass2);
    CHECK(clear_index >= 0);
    if (clear_index >= 0) {
        CHECK(records[clear_index].flags ==
              (clear_flags::TARGET | clear_flags::ZBUFFER));
        CHECK(records[clear_index].color == kClearColor);
        CHECK(near_eq(records[clear_index].z, kClearZ));
    }

    // Projection/view and fog from Camera__setup.
    int projection_index = -1;
    int view_index = -1;
    for (int i = begin_index; i < pass1; ++i) {
        if (records[i].type != RecordType::Transform)
            continue;
        if (records[i].state == xf::PROJECTION && projection_index < 0)
            projection_index = i;
        if (records[i].state == xf::VIEW && view_index < 0)
            view_index = i;
    }
    CHECK(projection_index >= 0);
    CHECK(view_index >= 0);
    const float aspect = 320.0f / 240.0f;
    const float y_scale = 1.0f / tanf(camera.fov * 0.5f);
    if (projection_index >= 0) {
        const Mat4 &p = records[projection_index].matrix;
        CHECK(near_eq(p.m[0], y_scale / aspect));
        CHECK(near_eq(p.m[5], y_scale));
        CHECK(near_eq(p.m[10], camera.far_plane /
                                    (camera.near_plane - camera.far_plane)));
        CHECK(near_eq(p.m[11], -1.0f));
        CHECK(near_eq(p.m[14], camera.near_plane * camera.far_plane /
                                    (camera.near_plane - camera.far_plane)));
    }
    if (view_index >= 0)
        CHECK(mat4_equal(records[view_index].matrix, camera.view));
    CHECK(find_render_state(device, rs::FOGENABLE, 1, begin_index, pass1) >= 0);
    CHECK(find_render_state(device, rs::FOGSTART, float_bits(50.0f), begin_index,
                            pass1) >= 0);
    CHECK(find_render_state(device, rs::FOGEND, float_bits(250.0f), begin_index,
                            pass1) >= 0);
    CHECK(find_render_state(device, rs::FOGCOLOR, 0xff102030u, begin_index,
                            pass1) >= 0);

    // Sky view (pass 2): camera-centred view, ALWAYS/no-Z, fog off, then restore.
    const int horizon_marker = marker_record_index(device, "group:horizon");
    CHECK(horizon_marker > pass2 && horizon_marker < pass3);
    if (horizon_marker >= 0) {
        int sky_view = -1;
        int restored_view = -1;
        for (int i = horizon_marker; i < pass3; ++i) {
            if (records[i].type == RecordType::Transform &&
                records[i].state == xf::VIEW) {
                if (sky_view < 0)
                    sky_view = i;
                restored_view = i;
            }
        }
        CHECK(sky_view >= 0);
        if (sky_view >= 0)
            CHECK(mat4_equal(records[sky_view].matrix, mat4_identity()));
        CHECK(restored_view > sky_view);
        if (restored_view >= 0)
            CHECK(mat4_equal(records[restored_view].matrix, camera.view));
        CHECK(find_render_state(device, rs::ZFUNC, cmp::ALWAYS, horizon_marker,
                                pass3) >= 0);
        CHECK(find_render_state(device, rs::ZWRITEENABLE, 0, horizon_marker,
                                pass3) >= 0);
        CHECK(find_render_state(device, rs::FOGENABLE, 0, horizon_marker,
                                pass3) >= 0);
        CHECK(find_render_state(device, rs::FOGENABLE, 1, horizon_marker,
                                pass3) >= 0);
    }

    // Object bucket: light-set ordering, material body, draw structure.
    const int object_marker = marker_record_index(device, "group:object");
    CHECK(object_marker > pass3 && object_marker < pass4);
    CHECK(find_render_state(device, rs::LIGHTING, 1, object_marker, pass4) >= 0);
    const std::vector<int> object_draws =
        draws_between(device, object_marker, pass4);
    CHECK(object_draws.size() == 4);
    CHECK(count_between(device, RecordType::Material, object_marker, pass4) == 2);
    // The horizon section already set the same FVF, so the object draws hit the
    // state cache instead of re-applying it.
    CHECK(count_between(device, RecordType::Fvf, object_marker, pass4) == 0);
    CHECK(count_between(device, RecordType::Stream, object_marker, pass4) == 2);
    CHECK(count_between(device, RecordType::Indices, object_marker, pass4) == 2);

    if (object_draws.size() == 4) {
        const int first = object_draws[0];
        const Record &first_draw = records[first];
        CHECK(first_draw.primitive_type ==
              static_cast<uint32_t>(PrimitiveType::TriangleList));
        CHECK(first_draw.base_vertex == 0);
        CHECK(first_draw.min_index == 0);
        CHECK(first_draw.num_vertices == 129);
        CHECK(first_draw.start_index == 0);
        CHECK(first_draw.primitive_count == 43);

        // Light-set keys 0,1,2 draw in ascending order.
        CHECK(last_before(device, RecordType::Material, first) >= 0);
        const int material_index = last_before(device, RecordType::Material, first);
        if (material_index >= 0) {
            const MaterialColors &colors = records[material_index].material;
            CHECK(near_eq(colors.diffuse[0], 0.314f, 0.01f));
            CHECK(near_eq(colors.ambient[0], 0.392f * 2.0f, 0.01f));
            CHECK(near_eq(colors.power, 16.2f, 0.1f));
        }
        CHECK(find_texture(device, 0, tex_base, object_marker, first) >= 0);
        CHECK(find_texture(device, 1, tex_mix, object_marker, first) >= 0);
        CHECK(find_stage_state(device, 0, tss::COLOROP, d3dtop::SELECTARG1,
                               object_marker, first) >= 0);
        CHECK(find_stage_state(device, 1, tss::COLOROP,
                               d3dtop::BLENDTEXTUREALPHA, object_marker,
                               first) >= 0);
        CHECK(find_stage_state(device, 1, tss::ALPHAOP,
                               d3dtop::BLENDTEXTUREALPHA, object_marker,
                               first) >= 0);
        // Material 0 diffuse alpha 1 -> TEXTUREFACTOR alpha 0xff (also set by
        // the horizon material, so search the whole frame).
        CHECK(find_render_state(device, rs::TEXTUREFACTOR, 0xff000000u, 0,
                                first) >= 0);
        CHECK(find_render_state(device, rs::SPECULARENABLE, 1, object_marker,
                                first) >= 0);
        const int stream_index =
            find_record(device, RecordType::Stream, object_marker, first);
        CHECK(stream_index >= 0);
        if (stream_index >= 0) {
            CHECK(records[stream_index].stride == kVertexStride);
            CHECK(records[stream_index].handle == mesh.sections[0].vb);
        }
        const int fvf_index = last_before(device, RecordType::Fvf, first);
        CHECK(fvf_index >= 0 && records[fvf_index].fvf == kVertexFvf);

        const Mat4 expected_worlds[3] = {world0, world1, world2};
        for (int i = 0; i < 3; ++i) {
            const int draw = object_draws[static_cast<size_t>(i)];
            int world_transform = -1;
            for (int j = draw - 1; j >= object_marker; --j) {
                if (records[j].type == RecordType::Transform &&
                    records[j].state == xf::WORLD) {
                    world_transform = j;
                    break;
                }
            }
            CHECK(world_transform >= 0);
            if (world_transform >= 0)
                CHECK(mat4_equal(records[world_transform].matrix,
                                 expected_worlds[i]));
        }

        // Palette bucket: WORLD/WORLD1 pair, VERTEXBLEND, software VP, bank 6.
        const int palette_draw = object_draws[3];
        const Record &palette_draw_record = records[palette_draw];
        CHECK(palette_draw_record.num_vertices == 66);
        CHECK(palette_draw_record.primitive_count == 22);
        int world_pair = -1;
        int world1_pair = -1;
        for (int j = palette_draw - 1; j >= object_marker; --j) {
            if (records[j].type == RecordType::Transform) {
                if (records[j].state == xf::WORLD && world_pair < 0)
                    world_pair = j;
                if (records[j].state == xf::WORLD1 && world1_pair < 0)
                    world1_pair = j;
            }
            if (world_pair >= 0 && world1_pair >= 0)
                break;
        }
        CHECK(world_pair >= 0 && world1_pair >= 0);
        if (world_pair >= 0)
            CHECK(mat4_equal(records[world_pair].matrix, palette0));
        if (world1_pair >= 0)
            CHECK(mat4_equal(records[world1_pair].matrix, palette1));
        const int blend_on =
            find_render_state(device, rs::VERTEXBLEND, 1, object_marker,
                              palette_draw);
        CHECK(blend_on >= 0);
        int software_vp = -1;
        for (int j = object_marker; j < palette_draw; ++j)
            if (records[j].type == RecordType::SoftwareVp && records[j].enabled) {
                software_vp = j;
                break;
            }
        CHECK(software_vp >= 0);
        int palette_constants = -1;
        for (int j = palette_draw - 1; j >= object_marker; --j)
            if (records[j].type == RecordType::VsConstants &&
                records[j].state == 0 && records[j].floats.size() == 24) {
                palette_constants = j;
                break;
            }
        CHECK(palette_constants >= 0);
        if (palette_constants >= 0) {
            CHECK(near_eq(records[palette_constants].floats[0], palette0.m[0]));
            CHECK(near_eq(records[palette_constants].floats[12], palette1.m[0]));
        }
        CHECK(find_stage_state(device, 0, tss::COLOROP, d3dtop::MODULATE,
                               object_marker, palette_draw) >= 0);
        CHECK(find_texture(device, 0, tex_inner, object_marker, palette_draw) >= 0);
        int material1_index = -1;
        for (int j = palette_draw - 1; j >= object_marker; --j)
            if (records[j].type == RecordType::Material) {
                material1_index = j;
                break;
            }
        CHECK(material1_index >= 0);
        if (material1_index >= 0) {
            CHECK(near_eq(records[material1_index].material.diffuse[0], 0.588f,
                          0.01f));
            CHECK(near_eq(records[material1_index].material.power, 5.4f, 0.1f));
        }
        const int blend_off =
            find_render_state(device, rs::VERTEXBLEND, 0, palette_draw, pass4);
        CHECK(blend_off >= 0);
        bool vp_restored = false;
        for (int j = palette_draw + 1; j < pass4; ++j)
            if (records[j].type == RecordType::SoftwareVp && !records[j].enabled)
                vp_restored = true;
        CHECK(vp_restored);
    }

    // 4. Overlay quad (notes/12 §3.3).
    const int overlay_draw =
        find_record(device, RecordType::DrawUp, overlay_marker, 0x7fffffff);
    CHECK(overlay_draw >= 0);
    if (overlay_draw >= 0) {
        const Record &draw = records[overlay_draw];
        CHECK(draw.primitive_type ==
              static_cast<uint32_t>(PrimitiveType::TriangleStrip));
        CHECK(draw.primitive_count == 2);
        CHECK(draw.stride == 0x10);
        CHECK(draw.floats.size() == 16);
        CHECK(near_eq(draw.floats[0], 10.0f) && near_eq(draw.floats[1], 20.0f));
        CHECK(find_render_state(device, rs::ZENABLE, 0, overlay_marker,
                                overlay_draw) >= 0);
        CHECK(find_render_state(device, rs::ZENABLE, 1, overlay_draw,
                                0x7fffffff) >= 0);
        CHECK(find_render_state(device, rs::ALPHABLENDENABLE, 1, overlay_marker,
                                overlay_draw) >= 0);
        // SRCBLEND/BLENDOP were already at these values (blend-mode 0), so the
        // cache drops the duplicate calls; check the effective device state.
        uint32_t cached_src = 0;
        uint32_t cached_op = 0;
        CHECK(device.cached_render_state(rs::SRCBLEND, &cached_src) &&
              cached_src == blend::SRCALPHA);
        CHECK(device.cached_render_state(rs::BLENDOP, &cached_op) &&
              cached_op == blend_op::ADD);
        CHECK(find_render_state(device, rs::DESTBLEND, blend::INVSRCALPHA,
                                overlay_marker, overlay_draw) >= 0);
        CHECK(find_render_state(device, rs::TEXTUREFACTOR, 0x80ffffffu,
                                overlay_marker, overlay_draw) >= 0);
        CHECK(find_stage_state(device, 0, tss::COLOROP, d3dtop::SELECTARG1,
                               overlay_marker, overlay_draw) >= 0);
        CHECK(find_stage_state(device, 0, tss::COLORARG1, d3dta::TFACTOR,
                               overlay_marker, overlay_draw) >= 0);
        CHECK(find_stage_state(device, 0, tss::ALPHAOP, d3dtop::SELECTARG1,
                               overlay_marker, overlay_draw) >= 0);
        CHECK(find_stage_state(device, 0, tss::ALPHAARG1, d3dta::TFACTOR,
                               overlay_marker, overlay_draw) >= 0);
        CHECK(find_texture(device, 0, tex_base, overlay_marker,
                           overlay_draw) >= 0);
        CHECK(find_record(device, RecordType::Fvf, overlay_marker,
                          overlay_draw) >= 0);
        CHECK(find_stage_state(device, 0, tss::COLOROP, d3dtop::MODULATE,
                               overlay_draw, 0x7fffffff) >= 0);
    }

    // Debug lines (pass 13): LineList, FVF 0x142, 0x10 stride.
    const int line_draw =
        find_record(device, RecordType::DrawUp, pass13, overlay_marker);
    CHECK(line_draw >= 0);
    if (line_draw >= 0) {
        CHECK(records[line_draw].primitive_type ==
              static_cast<uint32_t>(PrimitiveType::LineList));
        CHECK(records[line_draw].primitive_count == 1);
        CHECK(records[line_draw].stride == 0x10);
        CHECK(records[line_draw].floats.size() == 8);
        CHECK(find_record(device, RecordType::Fvf, pass13, line_draw) >= 0);
        int world_identity = -1;
        for (int j = line_draw - 1; j >= pass13; --j)
            if (records[j].type == RecordType::Transform &&
                records[j].state == xf::WORLD) {
                world_identity = j;
                break;
            }
        CHECK(world_identity >= 0);
        if (world_identity >= 0)
            CHECK(mat4_equal(records[world_identity].matrix, mat4_identity()));
    }

    // 5. Text path (fresh device so every text state is visible).
    RecordingDevice text_device;
    FontAtlas font;
    font.texture = text_device.create_texture(256, 256, 1, 3);
    font.width = 256;
    font.height = 256;
    font.line_height = 16.0f;
    font.has_glyphs = true;
    for (int i = 0; i < 96; ++i) {
        font.ascii[i].u0 = static_cast<float>(i) * 0.01f;
        font.ascii[i].u1 = font.ascii[i].u0 + 0.01f;
        font.ascii[i].v0 = 0.0f;
        font.ascii[i].v1 = 0.05f;
        font.ascii[i].width = 8.0f;
        font.ascii[i].height = 12.0f;
        font.ascii[i].advance = 10.0f;
    }
    TextRenderer text(text_device);
    text.set_font(&font);
    CHECK(text.color() == kDefaultTextColor);
    CHECK(kDefaultTextColor == 0xffffffffu);

    text_device.reset();
    const float width0 = text.draw_text(10.0f, 20.0f, "AB");
    CHECK(near_eq(width0, 20.0f));
    CHECK(text_device.count(RecordType::DrawUp) == 2);
    CHECK(text_device.count(RecordType::DrawIndexed) == 0);
    const Record *first_glyph = &text_device.records()[0];
    for (const Record &record : text_device.records())
        if (record.type == RecordType::DrawUp) {
            first_glyph = &record;
            break;
        }
    CHECK(first_glyph->primitive_type ==
          static_cast<uint32_t>(PrimitiveType::TriangleStrip));
    CHECK(first_glyph->primitive_count == 2);
    CHECK(first_glyph->stride == kTextStride);
    CHECK(first_glyph->floats.size() == 28);
    CHECK(near_eq(first_glyph->floats[0], 10.0f));
    CHECK(near_eq(first_glyph->floats[1], 20.0f));
    CHECK(color_at(first_glyph->floats, 0) == 0xffffffffu);
    CHECK(near_eq(first_glyph->floats[5], font.ascii['A' - 32].u0));
    CHECK(near_eq(first_glyph->floats[6], font.ascii['A' - 32].v0));
    CHECK(find_render_state(text_device, rs::ALPHABLENDENABLE, 1, 0,
                            0x7fffffff) >= 0);
    CHECK(find_render_state(text_device, rs::SRCBLEND, blend::SRCALPHA, 0,
                            0x7fffffff) >= 0);
    CHECK(find_render_state(text_device, rs::DESTBLEND, blend::INVSRCALPHA, 0,
                            0x7fffffff) >= 0);
    CHECK(find_render_state(text_device, rs::FOGENABLE, 0, 0, 0x7fffffff) >= 0);
    CHECK(find_sampler_state(text_device, 0, ss::MIPFILTER, 0, 0,
                             0x7fffffff) >= 0);
    CHECK(find_record(text_device, RecordType::Fvf, 0, 0x7fffffff) >= 0 &&
          text_device.last(RecordType::Fvf)->fvf == kQuadFvf);
    CHECK(find_texture(text_device, 0, font.texture, 0, 0x7fffffff) >= 0);
    const Record *pixel_shader = text_device.last(RecordType::Shader);
    CHECK(pixel_shader != nullptr && pixel_shader->stage == 1 &&
          pixel_shader->handle == 0);

    text_device.reset();
    text.set_mode(TextRenderer::Batched);
    text.draw_text(0.0f, 0.0f, "AB");
    CHECK(text_device.count(RecordType::DrawUp) == 0);
    CHECK(text_device.count(RecordType::DrawIndexed) == 1);
    const Record *batch_draw = text_device.last(RecordType::DrawIndexed);
    CHECK(batch_draw != nullptr);
    if (batch_draw) {
        CHECK(batch_draw->primitive_type ==
              static_cast<uint32_t>(PrimitiveType::TriangleList));
        CHECK(batch_draw->num_vertices == 8);
        CHECK(batch_draw->primitive_count == 4);
        CHECK(batch_draw->start_index == 0);
    }
    const Record *batch_vb = text_device.last(RecordType::CreateVertexBuffer);
    CHECK(batch_vb != nullptr && batch_vb->bytes == 2 * kGlyphBytes &&
          batch_vb->fvf == kQuadFvf);
    const Record *batch_ib = text_device.last(RecordType::CreateIndexBuffer);
    CHECK(batch_ib != nullptr && batch_ib->bytes == 24);
    if (batch_ib && batch_ib->data.size() == 24) {
        static const uint16_t expected_indices[12] = {0, 1, 2, 2, 1, 3,
                                                      4, 5, 6, 6, 5, 7};
        CHECK(memcmp(batch_ib->data.data(), expected_indices, 24) == 0);
    }
    CHECK(find_record(text_device, RecordType::Stream, 0, 0x7fffffff) >= 0 &&
          text_device.last(RecordType::Stream)->stride == kTextStride);

    // Escape codes: 0x1b 'C' + 8 hex sets colour, 0x1b 'c' restores.
    text_device.reset();
    const float width_escape =
        text.draw_text(0.0f, 0.0f, "\x1b" "Cffff0000A\x1b" "cB");
    CHECK(near_eq(width_escape, 20.0f));
    CHECK(text_device.count(RecordType::DrawIndexed) == 1);
    batch_vb = text_device.last(RecordType::CreateVertexBuffer);
    CHECK(batch_vb != nullptr && batch_vb->data.size() == 2 * kGlyphBytes);
    if (batch_vb && batch_vb->data.size() == 2 * kGlyphBytes) {
        CHECK(u32_at(batch_vb->data, 16) == 0xffff0000u);
        CHECK(u32_at(batch_vb->data, 4 * kTextStride + 16) == 0xffffffffu);
    }

    // Mode 2 measures first, then batches (no extra draw).
    text_device.reset();
    text.set_mode(TextRenderer::MeasureFirst);
    CHECK(near_eq(text.measure_width("AB"), 20.0f));
    const float width_measure = text.draw_text(0.0f, 0.0f, "AB");
    CHECK(near_eq(width_measure, 20.0f));
    CHECK(text_device.count(RecordType::DrawIndexed) == 1);
    CHECK(text_device.count(RecordType::DrawUp) == 0);

    // 6. State-cache dedup.
    text_device.reset();
    text_device.set_render_state(rs::COLORWRITEENABLE, 0x12345678u);
    text_device.set_render_state(rs::COLORWRITEENABLE, 0x12345678u);
    CHECK(text_device.count(RecordType::RenderState) == 1);
    text_device.set_render_state(rs::COLORWRITEENABLE, 0x12345679u);
    CHECK(text_device.count(RecordType::RenderState) == 2);
    uint32_t cached = 0;
    CHECK(text_device.cached_render_state(rs::COLORWRITEENABLE, &cached) &&
          cached == 0x12345679u);
    const Mat4 identity = mat4_identity();
    text_device.set_transform(xf::TEXTURE0, identity);
    text_device.set_transform(xf::TEXTURE0, identity);
    CHECK(text_device.count(RecordType::Transform) == 1);
    text_device.invalidate_state_cache();
    CHECK(!text_device.cached_render_state(rs::COLORWRITEENABLE, nullptr));

    if (g_failures == 0) {
        std::printf("PASS (%d checks)\n", g_checks);
        return 0;
    }
    std::printf("FAIL (%d of %d checks failed)\n", g_failures, g_checks);
    return 1;
}
