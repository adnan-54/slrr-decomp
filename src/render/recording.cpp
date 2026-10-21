#include "render/recording.h"

#include <string.h>

namespace render {

namespace {

uint32_t vertex_count_for(PrimitiveType type, uint32_t primitive_count)
{
    switch (type) {
    case PrimitiveType::PointList: return primitive_count;
    case PrimitiveType::LineList: return primitive_count * 2;
    case PrimitiveType::LineStrip: return primitive_count + 2;
    case PrimitiveType::TriangleList: return primitive_count * 3;
    case PrimitiveType::TriangleStrip: return primitive_count + 2;
    case PrimitiveType::TriangleFan: return primitive_count + 2;
    }
    return primitive_count;
}

} // namespace

Record &RecordingDevice::push(RecordType type)
{
    Record record;
    record.type = type;
    records_.push_back(record);
    return records_.back();
}

bool RecordingDevice::begin_scene()
{
    push(RecordType::BeginScene);
    return true;
}

void RecordingDevice::end_scene()
{
    push(RecordType::EndScene);
}

void RecordingDevice::present()
{
    push(RecordType::Present);
}

void RecordingDevice::clear(uint32_t flags, uint32_t color, float z,
                            uint32_t stencil)
{
    Record &r = push(RecordType::Clear);
    r.flags = flags;
    r.color = color;
    r.z = z;
    r.stencil = static_cast<float>(stencil);
}

void RecordingDevice::set_viewport(int x, int y, int width, int height,
                                   float min_z, float max_z)
{
    Record &r = push(RecordType::Viewport);
    r.vx = x;
    r.vy = y;
    r.vw = width;
    r.vh = height;
    r.min_z = min_z;
    r.max_z = max_z;
}

void RecordingDevice::set_material(const MaterialColors &material)
{
    push(RecordType::Material).material = material;
}

void RecordingDevice::set_light(uint32_t index, const Light &light)
{
    Record &r = push(RecordType::Light);
    r.index = index;
    r.light = light;
}

void RecordingDevice::light_enable(uint32_t index, bool enabled)
{
    Record &r = push(RecordType::LightEnable);
    r.index = index;
    r.enabled = enabled;
}

void RecordingDevice::set_software_vertex_processing(bool enabled)
{
    push(RecordType::SoftwareVp).enabled = enabled;
}

void RecordingDevice::draw_primitive(PrimitiveType type, uint32_t start_vertex,
                                     uint32_t primitive_count)
{
    Record &r = push(RecordType::Draw);
    r.primitive_type = static_cast<uint32_t>(type);
    r.start_vertex = start_vertex;
    r.primitive_count = primitive_count;
}

void RecordingDevice::draw_indexed_primitive(PrimitiveType type,
                                             int32_t base_vertex,
                                             uint32_t min_index,
                                             uint32_t num_vertices,
                                             uint32_t start_index,
                                             uint32_t primitive_count)
{
    Record &r = push(RecordType::DrawIndexed);
    r.primitive_type = static_cast<uint32_t>(type);
    r.base_vertex = static_cast<uint32_t>(base_vertex);
    r.min_index = min_index;
    r.num_vertices = num_vertices;
    r.start_index = start_index;
    r.primitive_count = primitive_count;
}

void RecordingDevice::draw_primitive_up(PrimitiveType type,
                                        uint32_t primitive_count,
                                        const void *vertices, uint32_t stride)
{
    Record &r = push(RecordType::DrawUp);
    r.primitive_type = static_cast<uint32_t>(type);
    r.primitive_count = primitive_count;
    r.stride = stride;
    const uint32_t floats =
        vertex_count_for(type, primitive_count) * stride / sizeof(float);
    const float *source = static_cast<const float *>(vertices);
    r.floats.assign(source, source + floats);
}

void RecordingDevice::set_vertex_shader_constant_f(uint32_t reg,
                                                   const float *data,
                                                   uint32_t vec4_count)
{
    Record &r = push(RecordType::VsConstants);
    r.state = reg;
    r.floats.assign(data, data + vec4_count * 4);
}

void RecordingDevice::set_pixel_shader_constant_f(uint32_t reg,
                                                  const float *data,
                                                  uint32_t vec4_count)
{
    Record &r = push(RecordType::PsConstants);
    r.state = reg;
    r.floats.assign(data, data + vec4_count * 4);
}

void RecordingDevice::set_vertex_shader(uint64_t shader)
{
    Record &r = push(RecordType::Shader);
    r.stage = 0;
    r.handle = shader;
}

void RecordingDevice::set_pixel_shader(uint64_t shader)
{
    Record &r = push(RecordType::Shader);
    r.stage = 1;
    r.handle = shader;
}

void RecordingDevice::mark(const char *name)
{
    push(RecordType::Marker).text = name ? name : "";
}

Vbuf RecordingDevice::create_vertex_buffer(const void *data, size_t bytes,
                                           uint32_t fvf)
{
    Record &r = push(RecordType::CreateVertexBuffer);
    r.handle = next_vertex_buffer_++;
    r.bytes = static_cast<uint32_t>(bytes);
    r.fvf = fvf;
    if (data && bytes) {
        const uint8_t *source = static_cast<const uint8_t *>(data);
        r.data.assign(source, source + bytes);
    }
    return r.handle;
}

Ibuf RecordingDevice::create_index_buffer(const void *data, size_t bytes,
                                          bool indices32)
{
    Record &r = push(RecordType::CreateIndexBuffer);
    r.handle = next_index_buffer_++;
    r.bytes = static_cast<uint32_t>(bytes);
    r.indices32 = indices32;
    if (data && bytes) {
        const uint8_t *source = static_cast<const uint8_t *>(data);
        r.data.assign(source, source + bytes);
    }
    return r.handle;
}

void RecordingDevice::release_vertex_buffer(Vbuf buffer)
{
    (void)buffer;
}

void RecordingDevice::release_index_buffer(Ibuf buffer)
{
    (void)buffer;
}

Texture RecordingDevice::new_texture(uint32_t width, uint32_t height,
                                     uint32_t levels, uint32_t slot)
{
    Record &r = push(RecordType::CreateTexture);
    r.handle = next_texture_++;
    r.width = width;
    r.height = height;
    r.levels = levels;
    r.slot = slot;
    return r.handle;
}

Texture RecordingDevice::create_texture(uint32_t width, uint32_t height,
                                        uint32_t levels, uint32_t slot)
{
    return new_texture(width, height, levels, slot);
}

Texture RecordingDevice::create_texture_from_memory(const void *data,
                                                    size_t bytes, uint32_t slot,
                                                    bool mipmaps)
{
    (void)data;
    (void)bytes;
    (void)mipmaps;
    return new_texture(0, 0, 0, slot);
}

Texture RecordingDevice::create_render_texture(uint32_t width, uint32_t height)
{
    return new_texture(width, height, 1, 0xffffffffu);
}

void RecordingDevice::fill_texture(Texture texture, uint32_t argb,
                                   uint32_t width, uint32_t height)
{
    Record &r = push(RecordType::FillTexture);
    r.handle = texture;
    r.color = argb;
    r.width = width;
    r.height = height;
}

void RecordingDevice::release_texture(Texture texture)
{
    (void)texture;
}

void RecordingDevice::begin_offscreen(Texture target)
{
    push(RecordType::OffscreenBegin).handle = target;
}

void RecordingDevice::end_offscreen()
{
    push(RecordType::OffscreenEnd);
}

void RecordingDevice::apply_render_state(uint32_t state, uint32_t value)
{
    Record &r = push(RecordType::RenderState);
    r.state = state;
    r.value = value;
}

void RecordingDevice::apply_texture_stage_state(uint32_t stage, uint32_t state,
                                                uint32_t value)
{
    Record &r = push(RecordType::StageState);
    r.stage = stage;
    r.state = state;
    r.value = value;
}

void RecordingDevice::apply_sampler_state(uint32_t stage, uint32_t state,
                                          uint32_t value)
{
    Record &r = push(RecordType::SamplerState);
    r.stage = stage;
    r.state = state;
    r.value = value;
}

void RecordingDevice::apply_texture(uint32_t stage, Texture texture)
{
    Record &r = push(RecordType::Texture);
    r.stage = stage;
    r.handle = texture;
}

void RecordingDevice::apply_transform(uint32_t state, const Mat4 &matrix)
{
    Record &r = push(RecordType::Transform);
    r.state = state;
    r.matrix = matrix;
}

void RecordingDevice::apply_fvf(uint32_t fvf)
{
    push(RecordType::Fvf).fvf = fvf;
}

void RecordingDevice::apply_stream_source(uint32_t stream, Vbuf buffer,
                                          uint32_t stride)
{
    Record &r = push(RecordType::Stream);
    r.stream = stream;
    r.handle = buffer;
    r.stride = stride;
}

void RecordingDevice::apply_indices(Ibuf buffer)
{
    push(RecordType::Indices).handle = buffer;
}

size_t RecordingDevice::count(RecordType type) const
{
    size_t n = 0;
    for (const Record &record : records_)
        if (record.type == type)
            ++n;
    return n;
}

const Record *RecordingDevice::last(RecordType type) const
{
    for (size_t i = records_.size(); i > 0; --i)
        if (records_[i - 1].type == type)
            return &records_[i - 1];
    return nullptr;
}

std::vector<std::string> RecordingDevice::marker_names() const
{
    std::vector<std::string> names;
    for (const Record &record : records_)
        if (record.type == RecordType::Marker)
            names.push_back(record.text);
    return names;
}

int RecordingDevice::marker_index(const char *name) const
{
    int index = 0;
    for (const Record &record : records_) {
        if (record.type == RecordType::Marker) {
            if (record.text == name)
                return index;
            ++index;
        }
    }
    return -1;
}

} // namespace render
