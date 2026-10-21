// Render device abstraction (notes/12-render.md).
//
// A D3D9-shaped immediate API: render state / texture-stage state / sampler
// state / texture / transform setters are cached (a repeated identical call is
// not forwarded), the draw and resource verbs are direct. Backends:
//   - RecordingDevice (render/recording.h): captures every applied call for
//     Linux tests in the notes/12 pass order;
//   - D3D9Device (render/d3d9.h): real IDirect3DDevice9, compiled on _WIN32.
//
// Enum values are the raw D3D9 numbers used by the exe (notes/12 keeps raw
// values in its state tables); the D3D9 backend casts them to the SDK enums.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <unordered_map>

namespace render {

struct Mat4 {
    float m[16];
};

Mat4 mat4_identity();
Mat4 mat4_mul(const Mat4 &a, const Mat4 &b); // row-major, D3DX row-vector convention
Mat4 mat4_transpose(const Mat4 &a);
Mat4 mat4_perspective_fov_rh(float fov_y, float aspect, float zn, float zf);
Mat4 mat4_ortho_rh(float width, float height, float zn, float zf);
Mat4 mat4_inverse_affine(const Mat4 &a); // identity when singular
bool mat4_equal(const Mat4 &a, const Mat4 &b);
Mat4 mat4_translation(float x, float y, float z);

using Texture = uint64_t;
using Vbuf = uint64_t;
using Ibuf = uint64_t;
constexpr Texture kNoTexture = 0;

enum class PrimitiveType : uint32_t {
    PointList = 1,
    LineList = 2,
    LineStrip = 3,
    TriangleList = 4,
    TriangleStrip = 5,
    TriangleFan = 6,
};

// D3DRENDERSTATETYPE values (d3d9types.h; notes/12 uses these raw ids).
namespace rs {
enum : uint32_t {
    ZENABLE = 7,
    ZWRITEENABLE = 14,
    ALPHATESTENABLE = 15,
    SRCBLEND = 19,
    DESTBLEND = 20,
    CULLMODE = 22,
    ZFUNC = 23,
    ALPHAREF = 24,
    ALPHAFUNC = 25,
    ALPHABLENDENABLE = 27,
    FOGENABLE = 28,
    SPECULARENABLE = 29,
    FOGCOLOR = 34,
    FOGTABLEMODE = 35,
    FOGSTART = 36,
    FOGEND = 37,
    RANGEFOGENABLE = 48,
    TEXTUREFACTOR = 60,
    LIGHTING = 137,
    AMBIENT = 139,
    FOGVERTEXMODE = 140,
    COLORVERTEX = 141,
    LOCALVIEWER = 142,
    NORMALIZENORMALS = 143,
    DIFFUSEMATERIALSOURCE = 145,
    SPECULARMATERIALSOURCE = 146,
    AMBIENTMATERIALSOURCE = 147,
    EMISSIVEMATERIALSOURCE = 148,
    VERTEXBLEND = 151,
    COLORWRITEENABLE = 168,
    BLENDOP = 171,
    SCISSORTESTENABLE = 174,
    SLOPESCALEDEPTHBIAS = 175,
    BLENDFACTOR = 193,
    DEPTHBIAS = 195,
};
} // namespace rs

// D3DTEXTURESTAGESTATETYPE values.
namespace tss {
enum : uint32_t {
    COLOROP = 1,
    COLORARG1 = 2,
    COLORARG2 = 3,
    ALPHAOP = 4,
    ALPHAARG1 = 5,
    ALPHAARG2 = 6,
    TEXCOORDINDEX = 11,
    TEXTURETRANSFORMFLAGS = 24,
    RESULTARG = 28,
};
} // namespace tss

// D3DSAMPLERSTATETYPE values.
namespace ss {
enum : uint32_t {
    ADDRESSU = 1,
    ADDRESSV = 2,
    MAGFILTER = 5,
    MINFILTER = 6,
    MIPFILTER = 7,
    MAXANISOTROPY = 10,
};
} // namespace ss

// D3DTRANSFORMSTATETYPE values.
namespace xf {
enum : uint32_t {
    VIEW = 2,
    PROJECTION = 3,
    TEXTURE0 = 16,
    WORLD = 256,
    WORLD1 = 257,
};
} // namespace xf

// D3DBLEND / D3DBLENDOP / D3DCMPFUNC values.
namespace blend {
enum : uint32_t {
    ZERO = 1,
    ONE = 2,
    SRCCOLOR = 3,
    INVSRCCOLOR = 4,
    SRCALPHA = 5,
    INVSRCALPHA = 6,
    BLENDFACTOR = 14,
    INVBLENDFACTOR = 15,
};
}
namespace blend_op {
enum : uint32_t { ADD = 1, SUBTRACT = 2, REVSUBTRACT = 3, MIN = 4, MAX = 5 };
}
namespace cmp {
enum : uint32_t { NEVER = 1, LESS = 2, EQUAL = 3, LESSEQUAL = 4, GREATER = 5, ALWAYS = 8 };
}
namespace cull {
enum : uint32_t { NONE = 1, CW = 2, CCW = 3 };
}
namespace clear_flags {
enum : uint32_t { TARGET = 1, ZBUFFER = 2, STENCIL = 4 };
}
namespace d3dtop {
enum : uint32_t {
    DISABLE = 1,
    SELECTARG1 = 2,
    SELECTARG2 = 3,
    MODULATE = 4,
    ADD = 7,
    BLENDTEXTUREALPHA = 13,
    MODULATEALPHA_ADDCOLOR = 18,
    MODULATEALPHAADDTOCOLOR = 20,
    MULTIPLYADD = 25,
};
}
namespace d3dta {
enum : uint32_t { DIFFUSE = 0, CURRENT = 1, TEXTURE = 2, TFACTOR = 3, TEMP = 5, COMPLEMENT = 0x10 };
}

struct MaterialColors {
    float diffuse[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float ambient[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // notes/12 §1.4
    float specular[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float emissive[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float power = 0.0f;
};

// Engine light record (notes/12 §5): type 1 point, 2 spot, 3 directional;
// types 4/5 are projected lights mapped to 1/2 for SetLight.
struct Light {
    uint32_t type = 3;
    uint32_t diffuse = 0;
    uint32_t specular = 0;
    uint32_t ambient = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float direction[3] = {0.0f, -1.0f, 0.0f};
    float range = 1.0f;
    float falloff = 0.0f;
    float attenuation[3] = {1.0f, 0.0f, 0.0f};
    float theta = 0.0f;
    float phi = 0.0f;
    float intensity = 1.0f;
};

class Device {
  public:
    virtual ~Device() = default;

    // Cached setters (notes/12 §1.3/§4.3): identical consecutive calls are
    // dropped, and the backends only record/apply changes.
    void set_render_state(uint32_t state, uint32_t value);
    void set_texture_stage_state(uint32_t stage, uint32_t state, uint32_t value);
    void set_sampler_state(uint32_t stage, uint32_t state, uint32_t value);
    void set_texture(uint32_t stage, Texture texture);
    void set_transform(uint32_t state, const Mat4 &matrix);
    void set_fvf(uint32_t fvf);
    void set_stream_source(uint32_t stream, Vbuf buffer, uint32_t stride);
    void set_indices(Ibuf buffer);
    // Device reset / render-target switch: the device state is unknown again.
    void invalidate_state_cache();
    bool cached_render_state(uint32_t state, uint32_t *value) const;
    bool cached_transform(uint32_t state, Mat4 *matrix) const;

    virtual bool begin_scene() = 0;
    virtual void end_scene() = 0;
    virtual void present() = 0;
    virtual void clear(uint32_t flags, uint32_t color, float z, uint32_t stencil) = 0;
    virtual void set_viewport(int x, int y, int width, int height, float min_z,
                              float max_z) = 0;
    virtual void set_material(const MaterialColors &material) = 0;
    virtual void set_light(uint32_t index, const Light &light) = 0;
    virtual void light_enable(uint32_t index, bool enabled) = 0;
    virtual void set_software_vertex_processing(bool enabled) = 0;
    virtual void draw_primitive(PrimitiveType type, uint32_t start_vertex,
                                uint32_t primitive_count) = 0;
    virtual void draw_indexed_primitive(PrimitiveType type, int32_t base_vertex,
                                        uint32_t min_index, uint32_t num_vertices,
                                        uint32_t start_index,
                                        uint32_t primitive_count) = 0;
    virtual void draw_primitive_up(PrimitiveType type, uint32_t primitive_count,
                                   const void *vertices, uint32_t stride) = 0;
    virtual void set_vertex_shader_constant_f(uint32_t reg, const float *data,
                                              uint32_t vec4_count) = 0;
    virtual void set_pixel_shader_constant_f(uint32_t reg, const float *data,
                                             uint32_t vec4_count) = 0;
    virtual void set_vertex_shader(uint64_t shader) { (void)shader; }
    virtual void set_pixel_shader(uint64_t shader) { (void)shader; }
    // Pass-order marker: the recording backend keeps it, D3D9 ignores it.
    virtual void mark(const char *name) { (void)name; }

    virtual Vbuf create_vertex_buffer(const void *data, size_t bytes, uint32_t fvf) = 0;
    virtual Ibuf create_index_buffer(const void *data, size_t bytes, bool indices32) = 0;
    virtual void release_vertex_buffer(Vbuf buffer) { (void)buffer; }
    virtual void release_index_buffer(Ibuf buffer) { (void)buffer; }
    virtual Texture create_texture(uint32_t width, uint32_t height, uint32_t levels,
                                   uint32_t slot) = 0;
    virtual Texture create_texture_from_memory(const void *data, size_t bytes,
                                               uint32_t slot, bool mipmaps) = 0;
    virtual Texture create_render_texture(uint32_t width, uint32_t height) = 0;
    virtual void fill_texture(Texture texture, uint32_t argb, uint32_t width,
                              uint32_t height) = 0;
    virtual void release_texture(Texture texture) { (void)texture; }
    virtual void begin_offscreen(Texture target) { (void)target; }
    virtual void end_offscreen() {}

  protected:
    virtual void apply_render_state(uint32_t state, uint32_t value) = 0;
    virtual void apply_texture_stage_state(uint32_t stage, uint32_t state,
                                           uint32_t value) = 0;
    virtual void apply_sampler_state(uint32_t stage, uint32_t state,
                                     uint32_t value) = 0;
    virtual void apply_texture(uint32_t stage, Texture texture) = 0;
    virtual void apply_transform(uint32_t state, const Mat4 &matrix) = 0;
    virtual void apply_fvf(uint32_t fvf) = 0;
    virtual void apply_stream_source(uint32_t stream, Vbuf buffer,
                                     uint32_t stride) = 0;
    virtual void apply_indices(Ibuf buffer) = 0;

  private:
    struct StreamKey {
        uint32_t stream = 0;
        Vbuf buffer = 0;
        uint32_t stride = 0;
        bool operator==(const StreamKey &o) const
        {
            return stream == o.stream && buffer == o.buffer && stride == o.stride;
        }
    };
    std::unordered_map<uint32_t, uint32_t> render_states_;
    std::unordered_map<uint32_t, uint32_t> stage_states_; // key stage*0x100+state
    std::unordered_map<uint32_t, uint32_t> sampler_states_;
    std::unordered_map<uint32_t, Texture> textures_;
    std::unordered_map<uint32_t, Mat4> transforms_;
    std::unordered_map<uint32_t, StreamKey> streams_;
    uint32_t fvf_ = 0;
    Ibuf indices_ = 0;
    bool fvf_valid_ = false;
    bool indices_valid_ = false;
};

} // namespace render
