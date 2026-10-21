#include "render/ui.h"

#include <string.h>

namespace render {

namespace {

bool hex_digit(char c, uint32_t *value)
{
    if (c >= '0' && c <= '9')
        *value = static_cast<uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f')
        *value = static_cast<uint32_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
        *value = static_cast<uint32_t>(c - 'A' + 10);
    else
        return false;
    return true;
}

} // namespace

bool TextRenderer::begin_pass()
{
    uint32_t fog = 0;
    const bool fog_was_enabled = device_->cached_render_state(rs::FOGENABLE, &fog) && fog;
    device_->set_texture(0, font_ ? font_->texture : 0);
    device_->set_pixel_shader(0);
    device_->set_fvf(kQuadFvf);
    device_->set_sampler_state(0, ss::MIPFILTER, 0);
    device_->set_render_state(rs::ALPHABLENDENABLE, 1);
    device_->set_render_state(rs::SRCBLEND, blend::SRCALPHA);
    device_->set_render_state(rs::DESTBLEND, blend::INVSRCALPHA);
    device_->set_render_state(rs::FOGENABLE, 0);
    return fog_was_enabled;
}

void TextRenderer::end_pass(bool fog_was_enabled)
{
    if (fog_was_enabled)
        device_->set_render_state(rs::FOGENABLE, 1);
}

void TextRenderer::emit_glyph(float x, float y, const Glyph &glyph, uint32_t color)
{
    float vertices[28] = {
        x,               y,               0.0f, 1.0f, 0.0f, glyph.u0, glyph.v0,
        x + glyph.width, y,               0.0f, 1.0f, 0.0f, glyph.u1, glyph.v0,
        x,               y + glyph.height, 0.0f, 1.0f, 0.0f, glyph.u0, glyph.v1,
        x + glyph.width, y + glyph.height, 0.0f, 1.0f, 0.0f, glyph.u1, glyph.v1,
    };
    for (int i = 0; i < 4; ++i)
        memcpy(&vertices[i * 7 + 4], &color, sizeof(color));
    if (mode_ == Immediate)
        device_->draw_primitive_up(PrimitiveType::TriangleStrip, 2, vertices,
                                   kTextStride);
    else
        pending_.insert(pending_.end(), vertices, vertices + 28);
}

float TextRenderer::layout(float start_x, float start_y, const char *text, bool emit)
{
    if (!font_ || !text)
        return 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float max_x = 0.0f;
    uint32_t color = default_color_;
    const char *p = text;
    while (*p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == 0x1b) {
            if (p[1] == 'C') {
                uint32_t argb = 0;
                bool ok = true;
                for (int i = 0; i < 8; ++i) {
                    uint32_t digit = 0;
                    if (!hex_digit(p[2 + i], &digit)) {
                        ok = false;
                        break;
                    }
                    argb = (argb << 4) | digit;
                }
                if (ok) {
                    color = argb;
                    p += 10;
                    continue;
                }
            } else if (p[1] == 'c') {
                color = default_color_;
                p += 2;
                continue;
            }
            ++p;
            continue;
        }
        if (c == '\n') {
            if (x > max_x)
                max_x = x;
            x = 0.0f;
            y += font_->line_height > 0.0f ? font_->line_height
                                           : static_cast<float>(font_->height);
            ++p;
            continue;
        }
        if (c == '\r') {
            ++p;
            continue;
        }
        if (c == '\t') {
            const Glyph *space = font_->glyph(' ');
            x += space ? space->advance * 4.0f : font_->width;
            ++p;
            continue;
        }
        const Glyph *glyph = font_->glyph(static_cast<char>(c));
        if (glyph) {
            if (emit)
                emit_glyph(start_x + x, start_y + y, *glyph, color);
            x += glyph->advance > 0.0f ? glyph->advance : glyph->width;
        }
        ++p;
    }
    if (x > max_x)
        max_x = x;
    return max_x;
}

float TextRenderer::draw_text(float x, float y, const char *text)
{
    if (!font_ || !text)
        return 0.0f;
    if (mode_ == MeasureFirst)
        layout(x, y, text, false);
    const bool fog_was_enabled = begin_pass();
    const float width = layout(x, y, text, true);
    flush();
    end_pass(fog_was_enabled);
    return width;
}

float TextRenderer::measure_width(const char *text) const
{
    return const_cast<TextRenderer *>(this)->layout(0.0f, 0.0f, text, false);
}

void TextRenderer::flush()
{
    const size_t vertices = pending_.size() / 7;
    if (vertices == 0 || !font_)
        return;
    const size_t quads = vertices / 4;
    std::vector<uint16_t> indices;
    indices.reserve(quads * 6);
    for (size_t i = 0; i < quads; ++i) {
        const uint16_t base = static_cast<uint16_t>(i * 4);
        indices.push_back(base);
        indices.push_back(static_cast<uint16_t>(base + 1));
        indices.push_back(static_cast<uint16_t>(base + 2));
        indices.push_back(static_cast<uint16_t>(base + 2));
        indices.push_back(static_cast<uint16_t>(base + 1));
        indices.push_back(static_cast<uint16_t>(base + 3));
    }
    const Vbuf vb = device_->create_vertex_buffer(
        pending_.data(), pending_.size() * sizeof(float), kQuadFvf);
    const Ibuf ib = device_->create_index_buffer(
        indices.data(), indices.size() * sizeof(uint16_t), false);
    device_->set_fvf(kQuadFvf);
    device_->set_stream_source(0, vb, kTextStride);
    device_->set_indices(ib);
    device_->draw_indexed_primitive(PrimitiveType::TriangleList, 0, 0,
                                    static_cast<uint32_t>(vertices), 0,
                                    static_cast<uint32_t>(quads * 2));
    device_->release_vertex_buffer(vb);
    device_->release_index_buffer(ib);
    pending_.clear();
}

} // namespace render
