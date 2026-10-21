// Scene layer: SCX render meshes/sections, material buckets, render
// instances/cache entries, camera/viewport setup and the 13-step
// Camera__render pass order (notes/12-render.md §3.2, §4).
//
// Units are engine render units (SCX loader scale 0.1). Material state bodies
// follow notes/12 §6.2 where decoded; the per-class branches that the note
// marks partial are modelled at family level.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "mesh/scx.h"
#include "render/device.h"

namespace render {

class Renderer;
class Camera;

constexpr uint32_t kVertexFvf = 0x2a03d2u; // XYZ|NORMAL|DIFFUSE|SPECULAR|TEX3(2f)
constexpr uint32_t kVertexStride = 0x38u;
constexpr uint32_t kEngineFvf = 0x15003fu; // exe custom FVF (notes/17 §7)
constexpr uint32_t kQuadFvf = 0x144u;      // XYZRHW|DIFFUSE|TEX1 (text)
constexpr uint32_t kOverlayFvf = 4u;       // XYZRHW (overlay quads)
constexpr uint32_t kTextStride = 0x1cu;
constexpr uint32_t kGlyphBytes = 0x70u; // 4 vertices x 0x1c (text batches)
constexpr float kDefaultFov = 0.955f;
constexpr float kDefaultNear = 8.0f;
constexpr float kDefaultFar = 125000.0f;
constexpr uint32_t kDefaultCull = cull::CW;
constexpr uint32_t kClearColor = 0xff0000ffu;
constexpr float kClearZ = 0.99999f;

struct Material {
    uint32_t type = 0x30; // material class id, low 24 bits select
    uint32_t flags = 0;   // SCX record flags
    MaterialColors colors;
    // Resolved texture handles; -1 in texture_index means no texture record.
    int16_t texture_index[4] = {-1, -1, -1, -1}; // base, second/mix, sphere, bump
    Texture textures[4] = {};
    uint32_t alpha_ref = 0;
    bool alpha_test = false;
    bool transparent = false;
    bool two_sided = false;
    uint32_t passes = 1;
};

struct RenderSection {
    uint32_t material = 0;
    uint32_t engine_fvf = kEngineFvf;
    uint32_t fvf = kVertexFvf;
    uint32_t stride = kVertexStride;
    uint32_t vertex_count = 0;
    uint32_t primitive_count = 0;
    int32_t base_vertex = 0;
    uint32_t start_index = 0;
    std::vector<uint32_t> palette; // vertex-blend palette index array (vf49)
    bool skinned = false;
    uint32_t bone_count = 0;
    bool visible = true; // section+0x44 (projected-light receiver flag)
    std::vector<uint8_t> vertices;
    std::vector<uint16_t> indices;
    Vbuf vb = 0;
    Ibuf ib = 0;
};

struct RenderMesh {
    std::string name;
    uint32_t key = 0; // material-bucket key +0x78
    std::vector<Material> materials;
    std::vector<RenderSection> sections;

    void upload(Device &device);
    void release(Device &device);
};

// Converts the canonical SCX mesh into engine vertices (notes/17 §2.3/§7)
// and selects the notes/12 §6.1 material class for sequential records.
RenderMesh build_render_mesh(const mesh::Mesh &source);

struct RenderInstance {
    uint32_t id = 0;
    Mat4 world = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                  0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    const RenderMesh *mesh = nullptr;
    std::vector<Mat4> palette; // RI+0xd4 base, RI+0xd0 count
    uint32_t light_set = 0;    // per-camera light-instance key (0 = none)
    uint32_t color_override = 0;
    bool override_color = false;
};

struct CacheEntry {
    const RenderInstance *instance = nullptr;
    const RenderMesh *mesh = nullptr;
    uint32_t section = 0;
    uint32_t light_set = 0;
};

struct MaterialBucket {
    const RenderMesh *mesh = nullptr;
    uint32_t mesh_key = 0;
    uint32_t blend_type = 0;
    uint32_t variant = 0;
    std::vector<CacheEntry> entries;

    bool matches(const RenderMesh *m, uint32_t key, uint32_t blend,
                 uint32_t var) const;
    bool empty() const { return entries.empty(); }
};

enum class GroupKind : uint32_t {
    Horizon = 0,
    Ground,
    ObjectPrelit,
    ObjectMerge,
    Object,
    ObjectPrelit2,
    Transparent,
    PrelitTransparent,
    Particle,
    Skidmark1,
    Skidmark2,
    Text,
    Count
};

// A render group: begin -> {render; next} -> end over material buckets.
// Items are per-instance cache entries; entries for the same bucket are drawn
// grouped by light-set key (notes/12 §4.3 binary-heap order).
class RenderGroup {
  public:
    explicit RenderGroup(GroupKind kind = GroupKind::Object) : kind_(kind) {}
    GroupKind kind() const { return kind_; }
    void add(const RenderInstance &instance, uint32_t section,
             uint32_t blend_type = 0, uint32_t variant = 0);
    bool empty() const;
    void render(Renderer &renderer);
    const std::vector<MaterialBucket> &buckets() const { return buckets_; }
    std::vector<MaterialBucket> &buckets() { return buckets_; }

  private:
    GroupKind kind_;
    std::vector<MaterialBucket> buckets_;
};

class Renderer;

struct CameraHook {
    virtual ~CameraHook() = default;
    virtual void pre_shadow(Camera &) {}
    virtual void post_render(Camera &) {}
    bool pre_shadow_enabled = true;
    bool post_render_enabled = true;
};

struct Flare {
    Texture texture = 0;
    bool visible = true;
    uint32_t count = 0;
    float params[7] = {}; // 0x1c-byte drawLensFlare record (notes/12 §3.5)
};

struct DebugLine {
    float p[6] = {};
    uint32_t color = 0xffffffffu; // 0x1c-byte line record
};

class Camera;

struct Viewport {
    float x = 0.0f, y = 0.0f, w = 1.0f, h = 1.0f; // relative rect
    uint32_t clear_flags = clear_flags::TARGET | clear_flags::ZBUFFER;
    int pixel_x = 0, pixel_y = 0, pixel_width = 0, pixel_height = 0;
    std::vector<Camera *> cameras;

    void update_rect(int backbuffer_width, int backbuffer_height);
};

struct OverlayQuad {
    Texture texture = 0;
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    uint32_t color = 0xffffffffu;
};

// Shader-constant banks (notes/12 §4.3 FUN_00441d20).
enum class ConstantBank : uint32_t {
    WorldViewProjection = 1,
    ViewProjection = 2,
    World = 3,
    InverseWorld = 4,
    TransposeInverseWorld = 5,
    Palette = 6,
    TransposeViewProjection = 7,
    TransposeWorldViewProjection = 8,
    Lights = 9,
    Fog = 0xa,
    ViewZ = 0xb,
    WorldView = 0xc,
    TransposeWorldView = 0xd,
    Projection = 0xe,
    TransposeProjection = 0xf,
    CameraPosition = 0x10,
};

class Renderer {
  public:
    explicit Renderer(Device &device);

    Device &device() { return *device_; }
    const Device &device() const { return *device_; }

    // Frame scheduling (notes/12 §2): present, BeginScene, viewports,
    // one-shot overlay quads, EndScene.
    void present();
    bool begin_frame();
    void end_frame();
    void render_frame(int backbuffer_width, int backbuffer_height,
                      Viewport *viewports, size_t viewport_count,
                      const std::vector<OverlayQuad> &overlays);
    void set_default_states();

    // Camera state.
    void set_projection(const Mat4 &projection);
    const Mat4 &projection() const { return projection_; }
    void set_view(const Mat4 &view, const Mat4 &inverse_view);
    const Mat4 &view_matrix() const { return view_; }
    void set_world(const Mat4 &world);
    const Mat4 &world_matrix() const { return world_; }
    void set_fog(bool enabled, float start, float end, uint32_t color);
    bool fog_enabled() const { return fog_enabled_; }
    void set_ambient(uint32_t argb);
    void set_sun_direction(float x, float y, float z);
    const float *sun_direction() const { return sun_; }
    void set_lighting(bool enabled, bool force = false);
    bool lighting() const { return lighting_; }
    void set_light_table(const Light *lights, size_t count);
    void set_active_lights(const uint32_t *indices, size_t count);
    void set_vertex_shader_version(uint32_t version) { vertex_shader_version_ = version; }

    // Blend modes (notes/12 §4.2).
    void set_blend_mode(uint32_t mode, uint32_t zflag);
    void reset_blend_mode();
    void reapply_blend();
    void set_blend_factor(uint32_t argb);

    // Sky view (notes/12 §5).
    void begin_sky_view();
    void end_sky_view();

    // Materials (notes/12 §4.1, §6.2).
    uint32_t begin_material(const Material &material, uint32_t mode);
    void set_material_pass(const Material &material, uint32_t pass);
    void end_material(const Material &material);

    // Vertex-blend palette (notes/12 §4.3).
    void apply_palette(const std::vector<Mat4> &base,
                       const std::vector<uint32_t> &indices);

    void draw_section(const RenderSection &section);
    void draw_color_quad(Texture texture, float x, float y, float w, float h,
                         uint32_t color);
    void draw_lens_flare(const Flare &flare);
    void draw_lines_3d(const DebugLine *lines, size_t count);
    void draw_line_2d(float x0, float y0, float x1, float y1, uint32_t color);

    // Shader constant queue/bank dispatcher (notes/12 §4.3).
    void queue_constant(uint32_t reg, ConstantBank bank);
    void flush_constants();
    void immediate_constant(uint32_t reg, float x, float y, float z, float w);

    void set_neutral_texture(Texture texture) { neutral_texture_ = texture; }
    Texture neutral_texture() const { return neutral_texture_; }
    void set_envmap(Texture texture) { envmap_ = texture; }
    Texture envmap() const { return envmap_; }
    void set_shadow_target(Texture texture) { shadow_target_ = texture; }
    Texture shadow_target() const { return shadow_target_; }
    void set_flare_target(Texture texture) { flare_target_ = texture; }
    Texture flare_target() const { return flare_target_; }
    uint32_t cull_mode() const { return cull_mode_; }
    uint32_t vertex_shader_version() const { return vertex_shader_version_; }

    void begin_offscreen(Texture target);
    void end_offscreen();
    void begin_shadow_map(Texture target, const Mat4 &light_view,
                          const Mat4 &light_projection);
    void end_shadow_map();

  private:
    struct PendingConstant {
        uint32_t reg;
        ConstantBank bank;
    };
    void bind_stage(uint32_t stage, Texture texture, bool clamp_address);
    void unbind_stage(uint32_t stage);
    void set_light_slot(uint32_t slot, const Light &light, bool enabled);
    void compute_bank(ConstantBank bank, std::vector<float> *data) const;
    static uint32_t bank_group(ConstantBank bank);

    Device *device_ = nullptr;
    Mat4 world_ = mat4_identity();
    Mat4 view_ = mat4_identity();
    Mat4 inverse_view_ = mat4_identity();
    Mat4 projection_ = mat4_identity();
    bool fog_enabled_ = false;
    float fog_start_ = 50.0f;
    float fog_end_ = 250.0f;
    uint32_t fog_color_ = 0;
    uint32_t ambient_ = 0;
    float sun_[3] = {0.0f, -1.0f, 0.0f};
    bool lighting_ = false;
    uint32_t cull_mode_ = kDefaultCull;
    uint32_t blend_src_ = blend::SRCALPHA;
    uint32_t blend_dst_ = blend::INVSRCALPHA;
    uint32_t blend_op_ = blend_op::ADD;
    uint32_t blend_factor_ = 0xffffffffu;
    bool blend_enabled_ = false;
    bool alpha_test_enabled_ = false;
    uint32_t material_mode_ = 0xffffffffu;
    uint32_t vertex_shader_version_ = 0; // < 2 selects software vertex processing
    std::vector<Mat4> palette_base_;
    std::vector<uint32_t> palette_indices_;
    std::vector<Light> light_table_;
    uint32_t active_lights_[8] = {};
    uint32_t active_light_count_ = 0;
    uint32_t dirty_ = 0x1fu; // bit0 view/proj, bit1 world, bit2 fog, bit3/4 lights
    std::vector<PendingConstant> pending_;
    Texture neutral_texture_ = 0;
    Texture envmap_ = 0;
    Texture shadow_target_ = 0;
    Texture flare_target_ = 0;
    bool offscreen_ = false;
};

// A camera: view/projection/fog/sun plus the twelve render groups of
// notes/12 §3.1 and the exact 13-step Camera__render order (§3.2).
class Camera {
  public:
    Camera();
    Mat4 view = mat4_identity();
    Mat4 inverse_view = mat4_identity();
    float fov = kDefaultFov;
    float near_plane = kDefaultNear;
    float far_plane = kDefaultFar;
    float aspect = 1.0f;
    bool fog_enabled = false;
    float fog_start = 50.0f;
    float fog_end = 250.0f;
    uint32_t fog_color = 0;
    float sun_direction[3] = {0.0f, -1.0f, 0.0f};
    uint32_t clear_mask = clear_flags::TARGET | clear_flags::ZBUFFER;
    bool skip_render = false; // +0x739c/+0x73a0/+0x73b0
    bool render_sky = true;
    bool ground_enabled = true;
    bool prelit_enabled = true;
    bool merge_enabled = true;
    bool object_enabled = true;
    bool skidmarks_enabled = true;
    bool shadows_enabled = true;
    bool projected_lights_enabled = true;
    bool flares_enabled = true;
    bool debug_lines_enabled = true;

    Mat4 shadow_view = mat4_identity();
    Mat4 shadow_projection = mat4_identity();
    float shadow_size = 0.0f; // 0 -> no shadow-map pass

    std::vector<RenderInstance *> shadow_casters;
    std::vector<RenderInstance *> projected_receivers;
    std::vector<uint32_t> projected_light_indices;
    std::vector<CameraHook *> hooks;
    std::vector<Light> light_table;
    std::vector<uint32_t> directional_lights;
    std::vector<Flare> flares;
    std::vector<DebugLine> debug_lines;
    Material projected_light_material;

    RenderGroup &group(GroupKind kind);
    const RenderGroup &group(GroupKind kind) const;

    void setup(Renderer &renderer, const Viewport &viewport);
    void render(Renderer &renderer, const Viewport &viewport);

  private:
    RenderGroup groups_[static_cast<size_t>(GroupKind::Count)];
};

} // namespace render
