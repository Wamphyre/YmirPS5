// SPDX-License-Identifier: GPL-3.0-or-later

#include "menu.hpp"
#include "covers.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <cmrc/cmrc.hpp>
#include <stdexcept>
#include <stb_image.h>
#include <ymir/version.hpp>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

CMRC_DECLARE(ymir_ps5_assets);

namespace ps5 {

namespace {

    constexpr uint32_t RGB(unsigned r, unsigned g, unsigned b) {
        return 0xff000000u | (b << 16) | (g << 8) | r;
    }
    constexpr uint32_t kText = RGB(239, 246, 255);
    constexpr uint32_t kDetail = RGB(170, 189, 211);
    constexpr uint32_t kMuted = RGB(131, 151, 176);
    constexpr uint32_t kAccent = RGB(117, 226, 247);
    constexpr uint32_t kAmber = RGB(255, 197, 137);
    constexpr uint32_t kPanel = RGB(16, 21, 37);
    constexpr uint32_t kBorder = RGB(44, 60, 84);
    constexpr uint32_t kRow = RGB(22, 31, 49);
    constexpr uint32_t kSelection = RGB(29, 57, 77);

    uint32_t Blend(uint32_t destination, uint32_t source, unsigned opacity) {
        const auto channel = [=](unsigned shift) {
            return (((destination >> shift) & 255) * (255 - opacity) + ((source >> shift) & 255) * opacity + 127) /
                   255;
        };
        return RGB(channel(0), channel(8), channel(16));
    }

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
    : m_pixels(kMenuWidth * kMenuHeight)
    , m_background(m_pixels.size()) {
    const auto resources = cmrc::ymir_ps5_assets::get_filesystem();
    const auto font = resources.open("SplineSans-Medium.ttf");
    const auto *data = reinterpret_cast<const unsigned char *>(font.begin());
    m_title = std::make_unique<Font>(data, 53);
    m_body = std::make_unique<Font>(data, 31);
    m_small = std::make_unique<Font>(data, 22);

    for (unsigned y = 0; y < kMenuHeight; ++y) {
        for (unsigned x = 0; x < kMenuWidth; ++x) {
            const float dx = (float(x) - 1020) / 950;
            const float dy = float(y) / 650;
            const float glow = std::max(0.0f, 1 - dx * dx - dy * dy);
            m_pixels[y * kMenuWidth + x] = RGB(9 + 11 * glow, 13 + 19 * glow, 25 + 24 * glow);
        }
    }
    RoundedRect(60, 61, 304, 552, 21, kBorder);
    RoundedRect(61, 62, 302, 550, 20, kPanel);
    Fill(87, 418, 250, 1, kBorder);
    Fill(60, 640, 1160, 1, kBorder);

    const auto logo = resources.open("icon0.png");
    int width = 0, height = 0;
    const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels{
        stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(logo.begin()), int(logo.size()), &width, &height,
                              nullptr, STBI_rgb),
        stbi_image_free};
    if (!pixels) {
        throw std::runtime_error("Could not open the YmirPS5 logo");
    }
    constexpr int logoSize = 250;
    for (int y = 0; y < logoSize; ++y) {
        for (int x = 0; x < logoSize; ++x) {
            const auto *pixel = pixels.get() + (y * height / logoSize * width + x * width / logoSize) * 3;
            m_pixels[(87 + y) * kMenuWidth + 87 + x] = RGB(pixel[0], pixel[1], pixel[2]);
        }
    }
    Text(87, 372, "YmirPS5", *m_title, kText, 250);
    Text(89, 401, "Sega Saturn emulator", *m_small, kDetail, 246);
    Text(89, 584, "Version " Ymir_VERSION, *m_small, kMuted, 246);
    m_background.swap(m_pixels);
}

MenuCanvas::~MenuCanvas() = default;

void MenuCanvas::Fill(int x, int y, int width, int height, uint32_t color) {
    for (int row = y; row < y + height; ++row) {
        std::fill_n(m_pixels.data() + row * kMenuWidth + x, width, color);
    }
}

void MenuCanvas::RoundedRect(int x, int y, int width, int height, int radius, uint32_t color) {
    for (int row = 0; row < radius; ++row) {
        const int distance = radius - row - 1;
        const int inset = radius - int(std::sqrt(radius * radius - distance * distance));
        Fill(x + inset, y + row, width - 2 * inset, 1, color);
        Fill(x + inset, y + height - row - 1, width - 2 * inset, 1, color);
    }
    Fill(x, y + radius, width, height - 2 * radius, color);
}

void MenuCanvas::Line(float x0, float y0, float x1, float y1, uint32_t color) {
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float lengthSquared = dx * dx + dy * dy;
    for (int y = int(std::min(y0, y1)) - 2; y <= int(std::max(y0, y1)) + 2; ++y) {
        for (int x = int(std::min(x0, x1)) - 2; x <= int(std::max(x0, x1)) + 2; ++x) {
            const float px = x - x0;
            const float py = y - y0;
            const float t = std::clamp((px * dx + py * dy) / lengthSquared, 0.0f, 1.0f);
            const float alpha = std::clamp(1.8f - std::hypot(px - t * dx, py - t * dy), 0.0f, 1.0f);
            auto &pixel = m_pixels[y * kMenuWidth + x];
            pixel = Blend(pixel, color, unsigned(alpha * 255));
        }
    }
}

void MenuCanvas::Icon(MenuIcon icon, int x, int y, uint32_t color) {
    const auto line = [&](int x0, int y0, int x1, int y1) { Line(x + x0, y + y0, x + x1, y + y1, color); };
    const auto rect = [&](int left, int top, int right, int bottom) {
        line(left, top, right, top);
        line(right, top, right, bottom);
        line(right, bottom, left, bottom);
        line(left, bottom, left, top);
    };
    const auto circle = [&](int radius) {
        for (int dy = -radius - 1; dy <= radius + 1; ++dy) {
            for (int dx = -radius - 1; dx <= radius + 1; ++dx) {
                const float alpha = std::clamp(1.8f - std::abs(std::hypot(float(dx), float(dy)) - radius), 0.0f, 1.0f);
                auto &pixel = m_pixels[(y + dy) * kMenuWidth + x + dx];
                pixel = Blend(pixel, color, unsigned(alpha * 255));
            }
        }
    };

    switch (icon) {
    case MenuIcon::None: break;
    case MenuIcon::Cross: line(-6, -6, 6, 6); line(-6, 6, 6, -6); break;
    case MenuIcon::Circle: circle(7); break;
    case MenuIcon::Square: rect(-6, -6, 6, 6); break;
    case MenuIcon::Triangle: line(-8, 6, 0, -8); line(0, -8, 8, 6); line(8, 6, -8, 6); break;
    case MenuIcon::DPad:
        line(0, -7, 0, 7);
        line(-7, 0, 7, 0);
        break;
    case MenuIcon::Play: line(-5, -7, 7, 0); line(7, 0, -5, 7); line(-5, 7, -5, -7); break;
    case MenuIcon::Disc: circle(8); circle(2); break;
    case MenuIcon::Folder: rect(-9, -4, 9, 7); line(-9, -4, -9, -7); line(-9, -7, -2, -7); break;
    case MenuIcon::Video:
        rect(-9, -7, 9, 4);
        line(0, 4, 0, 8);
        line(-4, 8, 4, 8);
        break;
    case MenuIcon::Save: rect(-7, -8, 7, 8); line(-4, -3, 4, -3); line(-4, 3, 4, 3); break;
    case MenuIcon::Exit:
        line(0, -8, 0, -1);
        line(-4, -5, -7, -2);
        line(-7, -2, -7, 4);
        line(-7, 4, -3, 8);
        line(-3, 8, 3, 8);
        line(3, 8, 7, 4);
        line(7, 4, 7, -2);
        line(7, -2, 4, -5);
        break;
    }
}

int MenuCanvas::Text(int x, int baseline, std::string_view text, const Font &font, uint32_t color, int maxWidth,
                     bool centered) {
    const int end = x + maxWidth;
    const auto glyphFor = [&](uint32_t code) -> const Font::Glyph & {
        return font.glyphs[code >= 32 && code < font.glyphs.size() ? code : '?'];
    };
    int width = 0;
    for (size_t offset = 0; offset < text.size() && width <= maxWidth;) {
        width += glyphFor(NextCodePoint(text, offset)).advance;
    }
    if (centered) {
        x += std::max(0, maxWidth - width) / 2;
    }
    const int start = x;
    const auto draw = [&](const Font::Glyph &glyph) {
        if (glyph.opacity.empty()) {
            x += glyph.advance;
            return;
        }
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
                dst = Blend(dst, color, glyph.opacity[j * glyph.width + i]);
            }
        }
        x += glyph.advance;
    };
    const auto &dot = font.glyphs['.'];
    const bool truncate = width > maxWidth;
    const int cutoff = end - (truncate ? dot.advance * 3 : 0);
    for (size_t offset = 0; offset < text.size();) {
        const auto &glyph = glyphFor(NextCodePoint(text, offset));
        if (x + glyph.advance > cutoff) {
            break;
        }
        draw(glyph);
    }
    if (truncate && x + dot.advance * 3 <= end) {
        draw(dot);
        draw(dot);
        draw(dot);
    }
    return x - start;
}

void MenuCanvas::DrawChrome(std::string_view page, std::string_view detail, std::string_view status, bool biosPresent,
                            std::span<const MenuHint> hints) {
    m_pixels = m_background;
    const uint32_t biosColor = biosPresent ? kAccent : kAmber;
    RoundedRect(89, 446, 9, 9, 4, biosColor);
    Text(109, 457, biosPresent ? "Saturn BIOS found" : "Saturn BIOS missing", *m_small, kText, 228);
    Text(89, 489, biosPresent ? "BIOS  /  512 KiB" : "/data/YmirPS5/bios/bios.bin", *m_small, kDetail, 250);
    Text(89, 526, "Games folder", *m_small, kText, 246);
    Text(89, 552, "/data/YmirPS5/games/", *m_small, kDetail, 246);

    constexpr int left = 400;
    constexpr int right = 1200;
    Text(left, 110, page, *m_title, kText, right - left);
    Text(left + 2, 148, detail, *m_small, kDetail, right - left - 2);
    if (!status.empty()) {
        RoundedRect(left, 573, right - left, 40, 8, kPanel);
        Fill(left + 2, 582, 2, 22, kAmber);
        Text(left + 17, 600, status, *m_small, kAmber, right - left - 30);
    }

    int hintX = 60;
    for (const auto &[icon, label] : hints) {
        if (icon != MenuIcon::None) {
            RoundedRect(hintX, 655, 34, 30, 10, kRow);
            Icon(icon, hintX + 17, 670, kAccent);
            hintX += 44;
        }
        hintX += Text(hintX, 678, label, *m_small, kDetail, 1160 - hintX) + 30;
    }
}

void MenuCanvas::Draw(std::string_view page, std::string_view detail, std::span<const MenuRow> rows, size_t selected,
                      std::string_view status, bool biosPresent, std::span<const MenuHint> hints, bool showFocus) {
    DrawChrome(page, detail, status, biosPresent, hints);

    constexpr int left = 400;
    constexpr int right = 1200;
    constexpr int rowHeight = 55;
    const size_t first = selected >= kMenuVisibleRows ? selected + 1 - kMenuVisibleRows : 0;
    for (size_t i = first; i < std::min(rows.size(), first + kMenuVisibleRows); ++i) {
        const int y = 174 + int(i - first) * rowHeight;
        const auto &row = rows[i];
        const bool focused = showFocus && i == selected;
        RoundedRect(left, y, right - left, 48, 9, focused ? kAccent : kBorder);
        RoundedRect(left + 1, y + 1, right - left - 2, 46, 8, focused ? kSelection : kRow);
        const int labelX = left + (row.icon == MenuIcon::None ? 17 : 56);
        const int valueX = right - 324;
        Icon(row.icon, left + 28, y + 24, focused ? kAccent : kDetail);
        Text(labelX, y + 34, row.label, *m_body, row.enabled || !showFocus ? kText : kMuted,
             (row.value.empty() ? right - 32 : valueX - 16) - labelX);
        Text(valueX, y + 32, row.value, *m_small, row.enabled || !showFocus ? kDetail : kMuted, 290);
        if (focused && row.enabled) {
            Line(right - 20, y + 18, right - 14, y + 24, kAccent);
            Line(right - 14, y + 24, right - 20, y + 30, kAccent);
        }
    }
    if (rows.size() > kMenuVisibleRows) {
        constexpr int height = rowHeight * kMenuVisibleRows - 7;
        Fill(right + 14, 174, 3, height, kBorder);
        const int thumb = std::max(12, int(height * kMenuVisibleRows / rows.size()));
        const int track = int((height - thumb) * first / (rows.size() - kMenuVisibleRows));
        Fill(right + 14, 174 + track, 3, thumb, kAccent);
    }
}

void MenuCanvas::DrawCover(const CoverImage &cover, int x, int y, int width, int height) {
    const double scale = std::min(double(width) / cover.width, double(height) / cover.height);
    const int targetWidth = std::max(1, int(cover.width * scale));
    const int targetHeight = std::max(1, int(cover.height * scale));
    x += (width - targetWidth) / 2;
    y += (height - targetHeight) / 2;
    for (int row = 0; row < targetHeight; ++row) {
        for (int column = 0; column < targetWidth; ++column) {
            const auto pixel = (row * cover.height / targetHeight * cover.width + column * cover.width / targetWidth) * 3;
            const auto *rgb = cover.rgb.data() + pixel;
            m_pixels[(y + row) * kMenuWidth + x + column] = RGB(rgb[0], rgb[1], rgb[2]);
        }
    }
}

void MenuCanvas::DrawLibrary(std::string_view detail, std::span<const MenuRow> cards, size_t selected,
                             std::span<const CoverImage> covers, std::string_view status, bool biosPresent,
                             std::span<const MenuHint> hints) {
    DrawChrome("Game library", detail, status, biosPresent, hints);
    if (cards.empty()) {
        Text(402, 215, "No game images found", *m_body, kDetail, 760);
        return;
    }

    constexpr int width = 190;
    constexpr int height = 182;
    constexpr int gap = 13;
    selected = std::min(selected, cards.size() - 1);
    const size_t first = selected / kLibraryPageSize * kLibraryPageSize;
    for (size_t cell = 0; cell < std::min(kLibraryPageSize, cards.size() - first); ++cell) {
        const size_t index = first + cell;
        const auto &card = cards[index];
        const int x = 400 + int(cell % kLibraryColumns) * (width + gap);
        const int y = 174 + int(cell / kLibraryColumns) * (height + gap);
        const bool focused = index == selected;
        RoundedRect(x, y, width, height, 12, focused ? kAccent : kBorder);
        RoundedRect(x + 1, y + 1, width - 2, height - 2, 11, focused ? kSelection : kRow);

        constexpr int artHeight = 129;
        RoundedRect(x + 7, y + 7, width - 14, artHeight, 7, kPanel);
        std::string_view label = card.label;
        if (cell < covers.size() && !covers[cell].rgb.empty()) {
            const auto &cover = covers[cell];
            DrawCover(cover, x + 10, y + 10, width - 20, artHeight - 6);
            if (!cover.title.empty()) {
                label = cover.title;
            }
        } else {
            RoundedRect(x + width / 2 - 22, y + 51, 44, 41, 12, kRow);
            Icon(card.icon, x + width / 2, y + 71, focused ? kAccent : kDetail);
        }
        Text(x + 12, y + 158, label, *m_small, kText, width - 24);
        Text(x + 12, y + 176, card.value, *m_small, kDetail, width - 24);
    }
    if (cards.size() > kLibraryPageSize) {
        const auto page = std::to_string(first / kLibraryPageSize + 1) + " / " +
                          std::to_string((cards.size() + kLibraryPageSize - 1) / kLibraryPageSize);
        Text(1130, 615, page, *m_small, kDetail, 70);
    }
}

void MenuCanvas::DrawDiscLoading(double seconds) {
    constexpr float pi = std::numbers::pi_v<float>;
    constexpr int centerX = 640;
    constexpr int centerY = 305;
    constexpr int radius = 98;
    const float phase = float(std::fmod(seconds / 3.0, 1.0)) * 2 * pi;

    for (unsigned y = 0; y < kMenuHeight; ++y) {
        const float glow = std::max(0.0f, 1 - std::abs(float(y) - centerY) / 390);
        Fill(0, y, kMenuWidth, 1, RGB(10 + 6 * glow, 15 + 9 * glow, 26 + 16 * glow));
    }

    const auto orbit = [&](float angle, float size) -> std::array<float, 2> {
        const float x = size * 210 * std::cos(angle);
        const float y = size * 63 * std::sin(angle);
        return {centerX + 0.951f * x + 0.309f * y, centerY - 0.309f * x + 0.951f * y};
    };
    const auto ring = [&](int first, int last) {
        for (int step = first; step < last; ++step) {
            const float t = step * (2 * pi / 128);
            const float next = (step + 1) * (2 * pi / 128);
            const auto [x, y] = orbit(t, 1);
            const auto [nx, ny] = orbit(next, 1);
            const float distance = std::fmod(phase - t + 2 * pi, 2 * pi);
            const float trail = std::max(0.0f, 1 - distance / 1.65f);
            Line(x, y, nx, ny, Blend(kBorder, kAccent, unsigned(64 + 191 * trail)));
            const auto [rX, rY] = orbit(t, 1.10f);
            const auto [nX, nY] = orbit(next, 1.10f);
            Line(rX, rY, nX, nY, kBorder);
        }
    };
    const auto satellite = [&](bool front) {
        if ((std::sin(phase) >= 0) != front) {
            return;
        }
        const auto [x, y] = orbit(phase, 1);
        RoundedRect(int(x) - 5, int(y) - 5, 11, 11, 5, kAmber);
    };

    ring(64, 128);
    satellite(false);
    for (int y = -radius; y <= radius; ++y) {
        const int extent = int(std::sqrt(radius * radius - y * y));
        for (int x = -extent; x <= extent; ++x) {
            const float surface = std::sqrt(float(std::max(0, radius * radius - x * x - y * y))) / radius;
            const float light = std::clamp(0.13f + 0.7f * surface - 0.45f * y / radius, 0.0f, 1.0f);
            const float band = 0.5f + 0.5f * std::cos((y + 0.29f * x) * 0.135f);
            const auto color = Blend(RGB(14, 27, 53), RGB(81, 187, 227), unsigned(255 * light * (0.7f + 0.3f * band)));
            m_pixels[(centerY + y) * kMenuWidth + centerX + x] = color;
        }
    }
    ring(0, 64);
    satellite(true);

    Text(340, 509, "Reading USB disc", *m_title, kText, 600, true);
    Text(340, 550, "Detecting Sega Saturn media", *m_small, kDetail, 600, true);
}

} // namespace ps5
