// Recording device backend: applies the same Device contract as D3D9 but
// stores every applied call in order, so the notes/12 pass order, material
// state bodies, draw-call structure and state-cache dedup can be asserted on
// Linux. Handles for buffers/textures are synthetic and stable per device.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "render/device.h"

namespace render {

enum class RecordType : uint32_t {
    Present,
    BeginScene,
    EndScene,
    Clear,
    Viewport,
    RenderState,
    StageState,
    SamplerState,
    Texture,
    Transform,
    Material,
    Light,
    LightEnable,
    Fvf,
    Stream,
    Indices,
    Draw,
    DrawIndexed,
    DrawUp,
    VsConstants,
    PsConstants,
    Shader,
    SoftwareVp,
    Marker,
    CreateTexture,
    CreateVertexBuffer,
    CreateIndexBuffer,
    FillTexture,
    OffscreenBegin,
    OffscreenEnd,
};

struct Record {
    RecordType type = RecordType::Marker;

    uint32_t state = 0;              // render/stage/sampler state id, constant reg
    uint32_t value = 0;              // render-state value
    uint32_t stage = 0;              // stage index; shader stage 0=VS 1=PS
    uint32_t flags = 0;              // Clear flags
    uint32_t color = 0;              // Clear colour / FillTexture colour
    uint32_t fvf = 0;
    uint32_t stream = 0;
    uint32_t stride = 0;
    uint32_t primitive_type = 0;
    uint32_t start_vertex = 0;
    uint32_t base_vertex = 0;
    uint32_t min_index = 0;
    uint32_t num_vertices = 0;
    uint32_t start_index = 0;
    uint32_t primitive_count = 0;
    uint32_t index = 0;              // light index
    uint32_t width = 0, height = 0, levels = 0, slot = 0;
    uint32_t bytes = 0;
    uint64_t handle = 0;             // texture/buffer handle
    int32_t vx = 0, vy = 0, vw = 0, vh = 0;
    float z = 0.0f, min_z = 0.0f, max_z = 0.0f, stencil = 0.0f;
    bool enabled = false;
    bool indices32 = false;
    Mat4 matrix{};
    MaterialColors material;
    Light light;
    std::string text;
    std::vector<float> floats; // DrawUp vertices / shader constants
    std::vector<uint8_t> data; // created buffer contents
};

class RecordingDevice : public Device {
  public:
    bool begin_scene() override;
    void end_scene() override;
    void present() override;
    void clear(uint32_t flags, uint32_t color, float z, uint32_t stencil) override;
    void set_viewport(int x, int y, int width, int height, float min_z,
                      float max_z) override;
    void set_material(const MaterialColors &material) override;
    void set_light(uint32_t index, const Light &light) override;
    void light_enable(uint32_t index, bool enabled) override;
    void set_software_vertex_processing(bool enabled) override;
    void draw_primitive(PrimitiveType type, uint32_t start_vertex,
                        uint32_t primitive_count) override;
    void draw_indexed_primitive(PrimitiveType type, int32_t base_vertex,
                                uint32_t min_index, uint32_t num_vertices,
                                uint32_t start_index,
                                uint32_t primitive_count) override;
    void draw_primitive_up(PrimitiveType type, uint32_t primitive_count,
                           const void *vertices, uint32_t stride) override;
    void set_vertex_shader_constant_f(uint32_t reg, const float *data,
                                      uint32_t vec4_count) override;
    void set_pixel_shader_constant_f(uint32_t reg, const float *data,
                                     uint32_t vec4_count) override;
    void set_vertex_shader(uint64_t shader) override;
    void set_pixel_shader(uint64_t shader) override;
    void mark(const char *name) override;

    Vbuf create_vertex_buffer(const void *data, size_t bytes, uint32_t fvf) override;
    Ibuf create_index_buffer(const void *data, size_t bytes, bool indices32) override;
    void release_vertex_buffer(Vbuf buffer) override;
    void release_index_buffer(Ibuf buffer) override;
    Texture create_texture(uint32_t width, uint32_t height, uint32_t levels,
                           uint32_t slot) override;
    Texture create_texture_from_memory(const void *data, size_t bytes,
                                       uint32_t slot, bool mipmaps) override;
    Texture create_render_texture(uint32_t width, uint32_t height) override;
    void fill_texture(Texture texture, uint32_t argb, uint32_t width,
                      uint32_t height) override;
    void release_texture(Texture texture) override;
    void begin_offscreen(Texture target) override;
    void end_offscreen() override;

    const std::vector<Record> &records() const { return records_; }
    void reset() { records_.clear(); }
    size_t count(RecordType type) const;
    const Record *last(RecordType type) const;
    const Record *last() const { return records_.empty() ? nullptr : &records_.back(); }
    std::vector<std::string> marker_names() const;
    int marker_index(const char *name) const; // first match, -1 when absent

  protected:
    void apply_render_state(uint32_t state, uint32_t value) override;
    void apply_texture_stage_state(uint32_t stage, uint32_t state,
                                   uint32_t value) override;
    void apply_sampler_state(uint32_t stage, uint32_t state,
                             uint32_t value) override;
    void apply_texture(uint32_t stage, Texture texture) override;
    void apply_transform(uint32_t state, const Mat4 &matrix) override;
    void apply_fvf(uint32_t fvf) override;
    void apply_stream_source(uint32_t stream, Vbuf buffer, uint32_t stride) override;
    void apply_indices(Ibuf buffer) override;

  private:
    Record &push(RecordType type);
    Texture new_texture(uint32_t width, uint32_t height, uint32_t levels,
                        uint32_t slot);

    std::vector<Record> records_;
    uint64_t next_texture_ = 0x1000;
    uint64_t next_vertex_buffer_ = 0x2000;
    uint64_t next_index_buffer_ = 0x3000;
};

} // namespace render
