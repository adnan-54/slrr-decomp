// D3D9 backend (notes/12-render.md §1.3): real IDirect3DDevice9 created on the
// HAL with the exe's behaviour-flag fallback chain, the cached Device setters
// forwarded straight to the D3D9 state API, and resource creation through
// managed vertex/index buffers and D3DX textures.
//
// This backend only compiles under _WIN32 (mingw i686); the Linux test build
// uses RecordingDevice instead.
#pragma once

#ifdef _WIN32

#include <stdint.h>

#include <unordered_map>

#include <windows.h>
#include <d3d9.h>

#include "render/device.h"

namespace render {

struct D3D9Config {
    uint32_t width = 640;
    uint32_t height = 480;
    uint32_t depth = 16;
    bool windowed = true;
    bool vsync = false;
    uint32_t msaa = 0; // D3DMULTISAMPLE_TYPE value
    uint32_t anisotropy = 0;
    HWND hwnd = nullptr;
};

class D3D9Device : public Device {
  public:
    explicit D3D9Device(const D3D9Config &config = D3D9Config());
    ~D3D9Device() override;

    bool valid() const { return device_ != nullptr; }
    const char *error() const { return error_; }
    IDirect3DDevice9 *device() const { return device_; }
    const D3D9Config &config() const { return config_; }

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
    void set_error(const char *message, long hr);
    Texture add_texture(IDirect3DTexture9 *texture);
    IDirect3DTexture9 *find_texture(Texture handle) const;

    D3D9Config config_;
    IDirect3D9 *d3d_ = nullptr;
    IDirect3DDevice9 *device_ = nullptr;
    D3DPRESENT_PARAMETERS present_ = {};
    char error_[256] = {};

    std::unordered_map<uint64_t, IDirect3DTexture9 *> textures_;
    std::unordered_map<uint64_t, IDirect3DVertexBuffer9 *> vertex_buffers_;
    std::unordered_map<uint64_t, IDirect3DIndexBuffer9 *> index_buffers_;
    uint64_t next_texture_ = 0x1000;
    uint64_t next_vertex_buffer_ = 0x2000;
    uint64_t next_index_buffer_ = 0x3000;

    IDirect3DSurface9 *offscreen_surface_ = nullptr;
    IDirect3DSurface9 *offscreen_previous_ = nullptr;
};

} // namespace render

#endif // _WIN32
