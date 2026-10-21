#include "render/d3d9.h"

#ifdef _WIN32

#include <stdio.h>
#include <string.h>

#include <d3dx9.h>

namespace render {

namespace {

D3DFORMAT format_for_slot(uint32_t slot)
{
    switch (slot) {
    case 6: return D3DFMT_DXT1;
    case 7: return D3DFMT_DXT5;
    case 13: return D3DFMT_DXT3;
    case 8: return D3DFMT_V8U8;
    default: return D3DFMT_A8R8G8B8; // 16/32-bit opaque and alpha fallbacks
    }
}

D3DCOLORVALUE color_value(uint32_t argb)
{
    D3DCOLORVALUE color;
    color.r = static_cast<float>((argb >> 16) & 0xffu) / 255.0f;
    color.g = static_cast<float>((argb >> 8) & 0xffu) / 255.0f;
    color.b = static_cast<float>(argb & 0xffu) / 255.0f;
    color.a = static_cast<float>((argb >> 24) & 0xffu) / 255.0f;
    return color;
}

} // namespace

D3D9Device::D3D9Device(const D3D9Config &config) : config_(config)
{
    d3d_ = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d_) {
        set_error("Direct3DCreate9 failed", 0);
        return;
    }

    HWND hwnd = config_.hwnd;
    if (!hwnd)
        hwnd = GetDesktopWindow(); // shortcut: no window is created here

    memset(&present_, 0, sizeof(present_));
    present_.BackBufferWidth = config_.width;
    present_.BackBufferHeight = config_.height;
    present_.BackBufferFormat = config_.windowed ? D3DFMT_UNKNOWN
                                                 : D3DFMT_X8R8G8B8;
    present_.BackBufferCount = 1;
    present_.MultiSampleType = static_cast<D3DMULTISAMPLE_TYPE>(config_.msaa);
    present_.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present_.hDeviceWindow = hwnd;
    present_.Windowed = config_.windowed ? TRUE : FALSE;
    present_.EnableAutoDepthStencil = TRUE;
    present_.PresentationInterval = config_.vsync ? D3DPRESENT_INTERVAL_ONE
                                                 : D3DPRESENT_INTERVAL_IMMEDIATE;

    // notes/12 §1.3: behaviour flags 0x44 hardware -> 0x84 software -> 0x24 mixed.
    static const DWORD behaviour_flags[] = {0x44u, 0x84u, 0x24u};
    static const D3DFORMAT depth_formats[] = {D3DFMT_D24S8, D3DFMT_D24X8,
                                              D3DFMT_D16, D3DFMT_D32};
    const D3DMULTISAMPLE_TYPE msaa_types[] = {
        static_cast<D3DMULTISAMPLE_TYPE>(config_.msaa),
        static_cast<D3DMULTISAMPLE_TYPE>(0),
    };

    long last_hr = 0;
    for (DWORD flags : behaviour_flags) {
        for (D3DFORMAT depth : depth_formats) {
            for (D3DMULTISAMPLE_TYPE msaa : msaa_types) {
                present_.AutoDepthStencilFormat = depth;
                present_.MultiSampleType = msaa;
                const HRESULT hr = d3d_->CreateDevice(
                    D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, flags, &present_,
                    &device_);
                if (SUCCEEDED(hr))
                    return;
                last_hr = static_cast<long>(hr);
            }
        }
    }
    set_error("CreateDevice failed", last_hr);
}

D3D9Device::~D3D9Device()
{
    end_offscreen();
    for (auto &entry : textures_)
        if (entry.second)
            entry.second->Release();
    for (auto &entry : vertex_buffers_)
        if (entry.second)
            entry.second->Release();
    for (auto &entry : index_buffers_)
        if (entry.second)
            entry.second->Release();
    if (device_)
        device_->Release();
    if (d3d_)
        d3d_->Release();
}

void D3D9Device::set_error(const char *message, long hr)
{
    snprintf(error_, sizeof(error_), "%s (hr=0x%08lx)", message,
             static_cast<unsigned long>(hr));
}

bool D3D9Device::begin_scene()
{
    return device_ && SUCCEEDED(device_->BeginScene());
}

void D3D9Device::end_scene()
{
    if (device_)
        device_->EndScene();
}

void D3D9Device::present()
{
    if (device_)
        device_->Present(nullptr, nullptr, nullptr, nullptr);
}

void D3D9Device::clear(uint32_t flags, uint32_t color, float z, uint32_t stencil)
{
    if (device_)
        device_->Clear(0, nullptr, static_cast<DWORD>(flags),
                       static_cast<D3DCOLOR>(color), z, stencil);
}

void D3D9Device::set_viewport(int x, int y, int width, int height, float min_z,
                              float max_z)
{
    if (!device_)
        return;
    D3DVIEWPORT9 viewport;
    viewport.X = static_cast<DWORD>(x);
    viewport.Y = static_cast<DWORD>(y);
    viewport.Width = static_cast<DWORD>(width);
    viewport.Height = static_cast<DWORD>(height);
    viewport.MinZ = min_z;
    viewport.MaxZ = max_z;
    device_->SetViewport(&viewport);
}

void D3D9Device::set_material(const MaterialColors &material)
{
    if (!device_)
        return;
    D3DMATERIAL9 out;
    out.Diffuse.r = material.diffuse[0];
    out.Ambient.r = material.ambient[0];
    out.Specular.r = material.specular[0];
    out.Emissive.r = material.emissive[0];
    out.Diffuse.g = material.diffuse[1];
    out.Diffuse.b = material.diffuse[2];
    out.Diffuse.a = material.diffuse[3];
    out.Ambient.g = material.ambient[1];
    out.Ambient.b = material.ambient[2];
    out.Ambient.a = material.ambient[3];
    out.Specular.g = material.specular[1];
    out.Specular.b = material.specular[2];
    out.Specular.a = material.specular[3];
    out.Emissive.g = material.emissive[1];
    out.Emissive.b = material.emissive[2];
    out.Emissive.a = material.emissive[3];
    out.Power = material.power;
    device_->SetMaterial(&out);
}

void D3D9Device::set_light(uint32_t index, const Light &light)
{
    if (!device_)
        return;
    D3DLIGHT9 out;
    memset(&out, 0, sizeof(out));
    uint32_t type = light.type;
    if (type == 4)
        type = 1; // projected point light (notes/12 §5)
    else if (type == 5)
        type = 2; // projected spot light
    out.Type = type == 1 ? D3DLIGHT_POINT
                         : type == 2 ? D3DLIGHT_SPOT : D3DLIGHT_DIRECTIONAL;
    out.Diffuse = color_value(light.diffuse);
    out.Specular = color_value(light.specular);
    out.Ambient = color_value(light.ambient);
    out.Position.x = light.position[0];
    out.Position.y = light.position[1];
    out.Position.z = light.position[2];
    out.Direction.x = light.direction[0];
    out.Direction.y = light.direction[1];
    out.Direction.z = light.direction[2];
    out.Range = light.range;
    out.Falloff = light.falloff;
    out.Attenuation0 = light.attenuation[0];
    out.Attenuation1 = light.attenuation[1];
    out.Attenuation2 = light.attenuation[2];
    out.Theta = light.theta;
    out.Phi = light.phi;
    device_->SetLight(index, &out);
}

void D3D9Device::light_enable(uint32_t index, bool enabled)
{
    if (device_)
        device_->LightEnable(index, enabled ? TRUE : FALSE);
}

void D3D9Device::set_software_vertex_processing(bool enabled)
{
    if (device_)
        device_->SetSoftwareVertexProcessing(enabled ? TRUE : FALSE);
}

void D3D9Device::draw_primitive(PrimitiveType type, uint32_t start_vertex,
                                uint32_t primitive_count)
{
    if (device_)
        device_->DrawPrimitive(static_cast<D3DPRIMITIVETYPE>(type), start_vertex,
                               primitive_count);
}

void D3D9Device::draw_indexed_primitive(PrimitiveType type, int32_t base_vertex,
                                        uint32_t min_index,
                                        uint32_t num_vertices,
                                        uint32_t start_index,
                                        uint32_t primitive_count)
{
    if (device_)
        device_->DrawIndexedPrimitive(static_cast<D3DPRIMITIVETYPE>(type),
                                      base_vertex, min_index, num_vertices,
                                      start_index, primitive_count);
}

void D3D9Device::draw_primitive_up(PrimitiveType type, uint32_t primitive_count,
                                   const void *vertices, uint32_t stride)
{
    if (device_)
        device_->DrawPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(type),
                                 primitive_count, vertices, stride);
}

void D3D9Device::set_vertex_shader_constant_f(uint32_t reg, const float *data,
                                              uint32_t vec4_count)
{
    if (device_ && data)
        device_->SetVertexShaderConstantF(reg, data, vec4_count);
}

void D3D9Device::set_pixel_shader_constant_f(uint32_t reg, const float *data,
                                             uint32_t vec4_count)
{
    if (device_ && data)
        device_->SetPixelShaderConstantF(reg, data, vec4_count);
}

void D3D9Device::set_vertex_shader(uint64_t shader)
{
    if (device_)
        device_->SetVertexShader(reinterpret_cast<IDirect3DVertexShader9 *>(
            static_cast<uintptr_t>(shader)));
}

void D3D9Device::set_pixel_shader(uint64_t shader)
{
    if (device_)
        device_->SetPixelShader(reinterpret_cast<IDirect3DPixelShader9 *>(
            static_cast<uintptr_t>(shader)));
}

Vbuf D3D9Device::create_vertex_buffer(const void *data, size_t bytes, uint32_t fvf)
{
    if (!device_ || bytes == 0)
        return 0;
    IDirect3DVertexBuffer9 *buffer = nullptr;
    if (FAILED(device_->CreateVertexBuffer(static_cast<UINT>(bytes),
                                           D3DUSAGE_WRITEONLY, fvf,
                                           D3DPOOL_MANAGED, &buffer, nullptr)))
        return 0;
    if (data) {
        void *locked = nullptr;
        if (SUCCEEDED(buffer->Lock(0, 0, &locked, 0))) {
            memcpy(locked, data, bytes);
            buffer->Unlock();
        }
    }
    const Vbuf handle = next_vertex_buffer_++;
    vertex_buffers_[handle] = buffer;
    return handle;
}

Ibuf D3D9Device::create_index_buffer(const void *data, size_t bytes, bool indices32)
{
    if (!device_ || bytes == 0)
        return 0;
    IDirect3DIndexBuffer9 *buffer = nullptr;
    if (FAILED(device_->CreateIndexBuffer(static_cast<UINT>(bytes),
                                          D3DUSAGE_WRITEONLY,
                                          indices32 ? D3DFMT_INDEX32
                                                    : D3DFMT_INDEX16,
                                          D3DPOOL_MANAGED, &buffer, nullptr)))
        return 0;
    if (data) {
        void *locked = nullptr;
        if (SUCCEEDED(buffer->Lock(0, 0, &locked, 0))) {
            memcpy(locked, data, bytes);
            buffer->Unlock();
        }
    }
    const Ibuf handle = next_index_buffer_++;
    index_buffers_[handle] = buffer;
    return handle;
}

void D3D9Device::release_vertex_buffer(Vbuf buffer)
{
    auto it = vertex_buffers_.find(buffer);
    if (it == vertex_buffers_.end())
        return;
    if (it->second)
        it->second->Release();
    vertex_buffers_.erase(it);
}

void D3D9Device::release_index_buffer(Ibuf buffer)
{
    auto it = index_buffers_.find(buffer);
    if (it == index_buffers_.end())
        return;
    if (it->second)
        it->second->Release();
    index_buffers_.erase(it);
}

Texture D3D9Device::add_texture(IDirect3DTexture9 *texture)
{
    if (!texture)
        return 0;
    const Texture handle = next_texture_++;
    textures_[handle] = texture;
    return handle;
}

IDirect3DTexture9 *D3D9Device::find_texture(Texture handle) const
{
    auto it = textures_.find(handle);
    return it == textures_.end() ? nullptr : it->second;
}

Texture D3D9Device::create_texture(uint32_t width, uint32_t height,
                                   uint32_t levels, uint32_t slot)
{
    if (!device_ || width == 0 || height == 0)
        return 0;
    IDirect3DTexture9 *texture = nullptr;
    const UINT mip_levels = levels == 0 ? D3DX_DEFAULT : levels;
    if (FAILED(device_->CreateTexture(width, height, mip_levels, 0,
                                      format_for_slot(slot), D3DPOOL_MANAGED,
                                      &texture, nullptr)))
        return 0;
    return add_texture(texture);
}

Texture D3D9Device::create_texture_from_memory(const void *data, size_t bytes,
                                               uint32_t slot, bool mipmaps)
{
    if (!device_ || !data || bytes == 0)
        return 0;
    IDirect3DTexture9 *texture = nullptr;
    const HRESULT hr = D3DXCreateTextureFromFileInMemoryEx(
        device_, data, static_cast<UINT>(bytes), D3DX_DEFAULT, D3DX_DEFAULT,
        mipmaps ? D3DX_DEFAULT : 1, 0, format_for_slot(slot), D3DPOOL_MANAGED,
        D3DX_DEFAULT, D3DX_DEFAULT, 0, nullptr, nullptr, &texture);
    if (FAILED(hr))
        return 0;
    return add_texture(texture);
}

Texture D3D9Device::create_render_texture(uint32_t width, uint32_t height)
{
    if (!device_ || width == 0 || height == 0)
        return 0;
    IDirect3DTexture9 *texture = nullptr;
    if (FAILED(device_->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                      D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture,
                                      nullptr)))
        return 0;
    return add_texture(texture);
}

void D3D9Device::fill_texture(Texture texture, uint32_t argb, uint32_t width,
                              uint32_t height)
{
    IDirect3DTexture9 *texture_object = find_texture(texture);
    if (!texture_object)
        return;
    D3DSURFACE_DESC desc;
    if (FAILED(texture_object->GetLevelDesc(0, &desc)))
        return;
    const UINT w = width != 0 && width < desc.Width ? width : desc.Width;
    const UINT h = height != 0 && height < desc.Height ? height : desc.Height;
    D3DLOCKED_RECT locked;
    if (FAILED(texture_object->LockRect(0, &locked, nullptr, 0)))
        return;
    for (UINT y = 0; y < h; ++y) {
        uint32_t *row = reinterpret_cast<uint32_t *>(
            static_cast<uint8_t *>(locked.pBits) + y * locked.Pitch);
        for (UINT x = 0; x < w; ++x)
            row[x] = argb;
    }
    texture_object->UnlockRect(0);
}

void D3D9Device::release_texture(Texture texture)
{
    auto it = textures_.find(texture);
    if (it == textures_.end())
        return;
    if (it->second)
        it->second->Release();
    textures_.erase(it);
}

void D3D9Device::begin_offscreen(Texture target)
{
    if (!device_)
        return;
    IDirect3DTexture9 *texture = find_texture(target);
    if (!texture)
        return;
    end_offscreen();
    device_->GetRenderTarget(0, &offscreen_previous_);
    IDirect3DSurface9 *surface = nullptr;
    if (SUCCEEDED(texture->GetSurfaceLevel(0, &surface))) {
        device_->SetRenderTarget(0, surface);
        offscreen_surface_ = surface;
    }
}

void D3D9Device::end_offscreen()
{
    if (!device_)
        return;
    if (offscreen_previous_) {
        device_->SetRenderTarget(0, offscreen_previous_);
        offscreen_previous_->Release();
        offscreen_previous_ = nullptr;
    }
    if (offscreen_surface_) {
        offscreen_surface_->Release();
        offscreen_surface_ = nullptr;
    }
}

void D3D9Device::apply_render_state(uint32_t state, uint32_t value)
{
    if (device_)
        device_->SetRenderState(static_cast<D3DRENDERSTATETYPE>(state), value);
}

void D3D9Device::apply_texture_stage_state(uint32_t stage, uint32_t state,
                                           uint32_t value)
{
    if (device_)
        device_->SetTextureStageState(
            stage, static_cast<D3DTEXTURESTAGESTATETYPE>(state), value);
}

void D3D9Device::apply_sampler_state(uint32_t stage, uint32_t state,
                                     uint32_t value)
{
    if (device_)
        device_->SetSamplerState(stage, static_cast<D3DSAMPLERSTATETYPE>(state),
                                 value);
}

void D3D9Device::apply_texture(uint32_t stage, Texture texture)
{
    if (device_)
        device_->SetTexture(stage, find_texture(texture));
}

void D3D9Device::apply_transform(uint32_t state, const Mat4 &matrix)
{
    if (device_)
        device_->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(state),
                              reinterpret_cast<const D3DMATRIX *>(matrix.m));
}

void D3D9Device::apply_fvf(uint32_t fvf)
{
    if (device_)
        device_->SetFVF(fvf);
}

void D3D9Device::apply_stream_source(uint32_t stream, Vbuf buffer,
                                     uint32_t stride)
{
    if (!device_)
        return;
    auto it = vertex_buffers_.find(buffer);
    device_->SetStreamSource(stream,
                             it == vertex_buffers_.end() ? nullptr : it->second,
                             0, stride);
}

void D3D9Device::apply_indices(Ibuf buffer)
{
    if (!device_)
        return;
    auto it = index_buffers_.find(buffer);
    device_->SetIndices(it == index_buffers_.end() ? nullptr : it->second);
}

} // namespace render

#endif // _WIN32
