#include "render/device.h"

#include <math.h>
#include <string.h>

namespace render {

Mat4 mat4_identity()
{
    Mat4 o{};
    o.m[0] = o.m[5] = o.m[10] = o.m[15] = 1.0f;
    return o;
}

Mat4 mat4_mul(const Mat4 &a, const Mat4 &b)
{
    Mat4 o{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a.m[r * 4 + k] * b.m[k * 4 + c];
            o.m[r * 4 + c] = s;
        }
    return o;
}

Mat4 mat4_transpose(const Mat4 &a)
{
    Mat4 o{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            o.m[r * 4 + c] = a.m[c * 4 + r];
    return o;
}

Mat4 mat4_perspective_fov_rh(float fov_y, float aspect, float zn, float zf)
{
    Mat4 o{};
    const float y_scale = 1.0f / tanf(fov_y * 0.5f);
    const float x_scale = aspect != 0.0f ? y_scale / aspect : y_scale;
    o.m[0] = x_scale;
    o.m[5] = y_scale;
    o.m[10] = zf / (zn - zf);
    o.m[11] = -1.0f;
    o.m[14] = zn * zf / (zn - zf);
    return o;
}

Mat4 mat4_ortho_rh(float width, float height, float zn, float zf)
{
    Mat4 o{};
    o.m[0] = width != 0.0f ? 2.0f / width : 1.0f;
    o.m[5] = height != 0.0f ? 2.0f / height : 1.0f;
    o.m[10] = 1.0f / (zn - zf);
    o.m[14] = zn / (zn - zf);
    o.m[15] = 1.0f;
    return o;
}

// Row-vector affine matrix: p' = p * A + t with t in row 3.
Mat4 mat4_inverse_affine(const Mat4 &a)
{
    const float *p = a.m;
    const float det = p[0] * (p[5] * p[10] - p[6] * p[9]) -
                      p[1] * (p[4] * p[10] - p[6] * p[8]) +
                      p[2] * (p[4] * p[9] - p[5] * p[8]);
    if (det == 0.0f)
        return mat4_identity();
    const float k = 1.0f / det;
    Mat4 o{};
    o.m[0] = (p[5] * p[10] - p[6] * p[9]) * k;
    o.m[1] = (p[2] * p[9] - p[1] * p[10]) * k;
    o.m[2] = (p[1] * p[6] - p[2] * p[5]) * k;
    o.m[4] = (p[6] * p[8] - p[4] * p[10]) * k;
    o.m[5] = (p[0] * p[10] - p[2] * p[8]) * k;
    o.m[6] = (p[2] * p[4] - p[0] * p[6]) * k;
    o.m[8] = (p[4] * p[9] - p[5] * p[8]) * k;
    o.m[9] = (p[1] * p[8] - p[0] * p[9]) * k;
    o.m[10] = (p[0] * p[5] - p[1] * p[4]) * k;
    o.m[3] = o.m[7] = o.m[11] = 0.0f;
    o.m[12] = -(p[12] * o.m[0] + p[13] * o.m[4] + p[14] * o.m[8]);
    o.m[13] = -(p[12] * o.m[1] + p[13] * o.m[5] + p[14] * o.m[9]);
    o.m[14] = -(p[12] * o.m[2] + p[13] * o.m[6] + p[14] * o.m[10]);
    o.m[15] = 1.0f;
    return o;
}

bool mat4_equal(const Mat4 &a, const Mat4 &b)
{
    for (int i = 0; i < 16; ++i)
        if (a.m[i] != b.m[i])
            return false;
    return true;
}

Mat4 mat4_translation(float x, float y, float z)
{
    Mat4 o = mat4_identity();
    o.m[12] = x;
    o.m[13] = y;
    o.m[14] = z;
    return o;
}

void Device::set_render_state(uint32_t state, uint32_t value)
{
    auto it = render_states_.find(state);
    if (it != render_states_.end() && it->second == value)
        return;
    render_states_[state] = value;
    apply_render_state(state, value);
}

void Device::set_texture_stage_state(uint32_t stage, uint32_t state, uint32_t value)
{
    const uint32_t key = stage * 0x100u + state;
    auto it = stage_states_.find(key);
    if (it != stage_states_.end() && it->second == value)
        return;
    stage_states_[key] = value;
    apply_texture_stage_state(stage, state, value);
}

void Device::set_sampler_state(uint32_t stage, uint32_t state, uint32_t value)
{
    const uint32_t key = stage * 0x100u + state;
    auto it = sampler_states_.find(key);
    if (it != sampler_states_.end() && it->second == value)
        return;
    sampler_states_[key] = value;
    apply_sampler_state(stage, state, value);
}

void Device::set_texture(uint32_t stage, Texture texture)
{
    auto it = textures_.find(stage);
    if (it != textures_.end() && it->second == texture)
        return;
    textures_[stage] = texture;
    apply_texture(stage, texture);
}

void Device::set_transform(uint32_t state, const Mat4 &matrix)
{
    auto it = transforms_.find(state);
    if (it != transforms_.end() && mat4_equal(it->second, matrix))
        return;
    transforms_[state] = matrix;
    apply_transform(state, matrix);
}

void Device::set_fvf(uint32_t fvf)
{
    if (fvf_valid_ && fvf_ == fvf)
        return;
    fvf_ = fvf;
    fvf_valid_ = true;
    apply_fvf(fvf);
}

void Device::set_stream_source(uint32_t stream, Vbuf buffer, uint32_t stride)
{
    auto it = streams_.find(stream);
    if (it != streams_.end() && it->second.buffer == buffer &&
        it->second.stride == stride)
        return;
    StreamKey key;
    key.stream = stream;
    key.buffer = buffer;
    key.stride = stride;
    streams_[stream] = key;
    apply_stream_source(stream, buffer, stride);
}

void Device::set_indices(Ibuf buffer)
{
    if (indices_valid_ && indices_ == buffer)
        return;
    indices_ = buffer;
    indices_valid_ = true;
    apply_indices(buffer);
}

void Device::invalidate_state_cache()
{
    render_states_.clear();
    stage_states_.clear();
    sampler_states_.clear();
    textures_.clear();
    transforms_.clear();
    streams_.clear();
    fvf_valid_ = false;
    indices_valid_ = false;
}

bool Device::cached_render_state(uint32_t state, uint32_t *value) const
{
    auto it = render_states_.find(state);
    if (it == render_states_.end())
        return false;
    if (value)
        *value = it->second;
    return true;
}

bool Device::cached_transform(uint32_t state, Mat4 *matrix) const
{
    auto it = transforms_.find(state);
    if (it == transforms_.end())
        return false;
    if (matrix)
        *matrix = it->second;
    return true;
}

} // namespace render
