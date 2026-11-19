// SPDX-License-Identifier: GPL-3.0-or-later

#include "menu.hpp"

#include <algorithm>
#include <array>
#include <cmrc/cmrc.hpp>
#include <stdexcept>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

CMRC_DECLARE(ymir_ps5_font);

namespace ps5 {

namespace {

    constexpr uint32_t RGB(unsigned r, unsigned g, unsigned b) {
        return 0xff000000u | (b << 16) | (g << 8) | r;
    }
    constexpr uint32_t kText = RGB(244, 243, 248);
    constexpr uint32_t kDetail = RGB(174, 173, 192);
    constexpr uint32_t kMuted = RGB(143, 142, 160);
    constexpr uint32_t kAccent = RGB(236, 179, 124);
    constexpr uint32_t kSelection = RGB(49, 43, 62);
    constexpr uint32_t kLine = RGB(54, 51, 70);
    constexpr int kMargin = 76;

    uint32_t NextCodePoint(std::string_view text, size_t &offset) {
        const uint8_t first = text[offset++];
        if (first < 0x80) {
            return first;
        }
        const unsigned following = first >= 0xf0 ? 3 : first >= 0xe0 ? 2 : 1;
        uint32_t code = first & (0x7f >> (following + 1));
        for (unsigned i = 0; i < following; ++i) {
            if (offset == text.size() || (uint8_t(text[offset]) & 0xc0) != 0x80) {
                return '?';
            }
            code = (code << 6) | (uint8_t(text[offset++]) & 0x3f);
        }
        return code;
    }

} // namespace

struct MenuCanvas::Font {
    struct Glyph {
        int width;
        int height;
        int x;
        int y;
        int advance;
        std::vector<uint8_t> opacity;
    };

    explicit Font(const unsigned char *data, float pixels) {
        stbtt_fontinfo font{};
        if (!stbtt_InitFont(&font, data, 0)) {
            throw std::runtime_error("Could not open the PS5 menu font");
        }
        const float scale = stbtt_ScaleForPixelHeight(&font, pixels);
        for (unsigned code = 32; code < glyphs.size(); ++code) {
            auto &glyph = glyphs[code];
            int advance;
            stbtt_GetCodepointHMetrics(&font, code, &advance, nullptr);
            glyph.advance = std::max(1, int(advance * scale + 0.5f));
            uint8_t *bitmap =
                stbtt_GetCodepointBitmap(&font, scale, scale, code, &glyph.width, &glyph.height, &glyph.x, &glyph.y);
            if (bitmap) {
                glyph.opacity.assign(bitmap, bitmap + glyph.width * glyph.height);
                stbtt_FreeBitmap(bitmap, font.userdata);
            }
        }
    }

    std::array<Glyph, 256> glyphs{};
};

MenuCanvas::MenuCanvas()
    : m_pixels(kMenuWidth * kMenuHeight) {
    auto font = cmrc::ymir_ps5_font::get_filesystem().open("SplineSans-Medium.ttf");
    const auto *data = reinterpret_cast<const unsigned char *>(font.begin());
    m_title = std::make_unique<Font>(data, 54);
    m_body = std::make_unique<Font>(data, 29);
    m_small = std::make_unique<Font>(data, 23);
}

MenuCanvas::~MenuCanvas() = default;

void MenuCanvas::Fill(int x, int y, int width, int height, uint32_t color) {
    for (int row = y; row < y + height; ++row) {
        std::fill_n(m_pixels.data() + row * kMenuWidth + x, width, color);
    }
}

void MenuCanvas::Text(int x, int baseline, std::string_view text, const Font &font, uint32_t color, int maxWidth) {
    const int end = x + maxWidth;
    for (size_t offset = 0; offset < text.size();) {
        const uint32_t code = NextCodePoint(text, offset);
        const auto &glyph = font.glyphs[code >= 32 && code < font.glyphs.size() ? code : '?'];
        if (x + glyph.advance > end) {
            break;
        }
        if (!glyph.opacity.empty()) {
            for (int j = 0; j < glyph.height; ++j) {
                const int y = baseline + glyph.y + j;
                if (y < 0 || y >= int(kMenuHeight)) {
                    continue;
                }
                for (int i = 0; i < glyph.width; ++i) {
                    const int column = x + glyph.x + i;
                    if (column < 0 || column >= end || column >= int(kMenuWidth)) {
                        continue;
                    }
                    auto &dst = m_pixels[y * kMenuWidth + column];
                    const unsigned a = glyph.opacity[j * glyph.width + i];
                    const auto mix = [a, dst, color](int shift) {
                        return (((dst >> shift) & 255) * (255 - a) + ((color >> shift) & 255) * a + 127) / 255;
                    };
                    dst = RGB(mix(0), mix(8), mix(16));
                }
            }
        }
        x += glyph.advance;
    }
}

void MenuCanvas::Draw(std::string_view page, std::string_view detail, std::span<const MenuRow> rows, size_t selected,
                      std::string_view status, std::string_view footer) {
    for (unsigned row = 0; row < kMenuHeight; ++row) {
        Fill(0, row, kMenuWidth, 1, RGB(22 + row / 100, 20 + row / 135, 33 + row / 70));
    }

    Text(kMargin, 70, "Ymir PS5", *m_small, kAccent, 640);
    Text(kMargin, 129, page, *m_title, kText, 1100);
    Text(kMargin, 176, detail, *m_small, kDetail, 1128);
    constexpr int rowHeight = 53;
    const size_t first = selected >= kMenuVisibleRows ? selected + 1 - kMenuVisibleRows : 0;
    for (size_t i = first; i < std::min(rows.size(), first + kMenuVisibleRows); ++i) {
        const int y = 205 + int(i - first) * rowHeight;
        const auto &row = rows[i];
        if (i == selected) {
            Fill(kMargin, y, 1128, 48, kSelection);
            Fill(kMargin, y, 3, 48, kAccent);
        }
        Text(kMargin + 20, y + 33, row.label, *m_body, row.enabled ? kText : kMuted, row.value.empty() ? 1080 : 700);
        Text(846, y + 32, row.value, *m_small, row.enabled ? kDetail : kMuted, 336);
    }
    if (rows.size() > kMenuVisibleRows) {
        Fill(1209, 208, 3, 364, kLine);
        const int thumb = std::max(10, int(364 * kMenuVisibleRows / rows.size()));
        const int track = int((364 - thumb) * first / (rows.size() - kMenuVisibleRows));
        Fill(1209, 208 + track, 3, thumb, kAccent);
    }

    Text(kMargin, 616, status, *m_small, kAccent, 1128);
    Fill(kMargin, 642, 1128, 1, kLine);
    Text(kMargin, 677, footer, *m_small, kDetail, 1128);
}

} // namespace ps5
