// 2-D UI: overlay quads (renderer) and the bitmap-font text path
// (notes/12-render.md §3.3).
//
// Text modes: 0 immediate per-glyph DrawPrimitiveUP, 1 batched glyph quads
// flushed as one indexed draw, 2 measure-first then batched. Escapes:
// 0x1b 'C' + 8 hex digits = colour, 0x1b 'c' = restore the default colour.
#pragma once
#include <stdint.h>

#include <vector>

#include "render/scene.h"

namespace render {

constexpr uint32_t kDefaultTextColor = 0xffffffffu; // DAT_005dadac

struct Glyph {
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f; // normalized atlas UVs
    float width = 0.0f, height = 0.0f;                // pixels
    float advance = 0.0f;                             // pixels
};

struct FontAtlas {
    Texture texture = 0;
    int width = 0, height = 0; // atlas pixel size (font+4)
    float line_height = 0.0f;
    bool has_glyphs = false;
    Glyph ascii[96]; // 32..127

    const Glyph *glyph(char c) const
    {
        const int index = static_cast<unsigned char>(c) - 32;
        if (!has_glyphs || index < 0 || index >= 96)
            return nullptr;
        return &ascii[index];
    }
};

class TextRenderer {
  public:
    enum Mode : uint32_t {
        Immediate = 0,   // one DrawPrimitiveUP per glyph
        Batched = 1,     // accumulate 0x70-byte glyph quads, one flush
        MeasureFirst = 2 // measuring pass, then batched
    };

    explicit TextRenderer(Device &device) : device_(&device) {}

    void set_font(const FontAtlas *font) { font_ = font; }
    const FontAtlas *font() const { return font_; }
    void set_mode(uint32_t mode) { mode_ = mode; }
    uint32_t mode() const { return mode_; }
    void set_color(uint32_t argb) { default_color_ = argb; }
    uint32_t color() const { return default_color_; }

    // Draws text at pixel position (x, y); returns the advance width.
    float draw_text(float x, float y, const char *text);
    float measure_width(const char *text) const;
    size_t pending_glyphs() const { return pending_.size() / 28; }
    void flush();

  private:
    float layout(float x, float y, const char *text, bool emit);
    bool begin_pass();
    void end_pass(bool fog_was_enabled);
    void emit_glyph(float x, float y, const Glyph &glyph, uint32_t color);

    Device *device_;
    const FontAtlas *font_ = nullptr;
    uint32_t mode_ = Immediate;
    uint32_t default_color_ = kDefaultTextColor;
    std::vector<float> pending_; // 7 floats (XYZRHW, colour, UV) per vertex
};

} // namespace render
