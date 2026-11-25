// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ps5 {

inline constexpr unsigned kMenuWidth = 1920;
inline constexpr unsigned kMenuHeight = 1080;
inline constexpr size_t kMenuVisibleRows = 7;
inline constexpr size_t kLibraryColumns = 4;
inline constexpr size_t kLibraryPageSize = kLibraryColumns * 2;

struct CoverImage;

enum class MenuIcon {
    None, Play, Disc, Folder, Cartridge, Cheats, Video, Audio, Save, Exit, Cross, Circle, Square, Triangle, DPad
};

struct MenuRow {
    std::string label;
    std::string value;
    bool enabled = true;
    MenuIcon icon = MenuIcon::None;
};

struct MenuHint {
    MenuIcon icon;
    std::string_view label;
};

class MenuCanvas {
public:
    MenuCanvas();
    ~MenuCanvas();

    void Draw(std::string_view page, std::string_view detail, std::span<const MenuRow> rows, size_t selected,
              std::string_view status, bool biosPresent, std::span<const MenuHint> hints, bool showFocus);
    void DrawLibrary(std::string_view detail, std::span<const MenuRow> cards, size_t selected,
                     std::span<const CoverImage> covers, std::string_view status, bool biosPresent,
                     std::span<const MenuHint> hints);
    void DrawDiscLoading(double seconds);
    const std::vector<uint32_t> &Pixels() const {
        return m_pixels;
    }

private:
    struct Font;

    void Fill(int x, int y, int width, int height, uint32_t color);
    void FillPixels(int x, int y, int width, int height, uint32_t color);
    void RoundedRect(int x, int y, int width, int height, int radius, uint32_t color);
    void Line(float x0, float y0, float x1, float y1, uint32_t color);
    void Icon(MenuIcon icon, int x, int y, uint32_t color);
    int Text(int x, int baseline, std::string_view text, const Font &font, uint32_t color, int maxWidth,
             bool centered = false);
    void DrawChrome(std::string_view page, std::string_view detail, std::string_view status, bool biosPresent,
                    std::span<const MenuHint> hints);
    void DrawCover(const CoverImage &cover, int x, int y, int width, int height);
    void BlitRGB(const uint8_t *rgb, int width, int height, int x, int y, int targetWidth, int targetHeight);

    std::vector<uint32_t> m_pixels;
    std::vector<uint32_t> m_background;
    std::unique_ptr<Font> m_title;
    std::unique_ptr<Font> m_body;
    std::unique_ptr<Font> m_small;
};

} // namespace ps5
