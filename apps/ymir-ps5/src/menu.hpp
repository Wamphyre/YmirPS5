// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ps5 {

inline constexpr unsigned kMenuWidth = 1280;
inline constexpr unsigned kMenuHeight = 720;
inline constexpr size_t kMenuVisibleRows = 7;

struct MenuRow {
    std::string label;
    std::string value;
    bool enabled = true;
};

class MenuCanvas {
public:
    MenuCanvas();
    ~MenuCanvas();

    void Draw(std::string_view page, std::string_view detail, std::span<const MenuRow> rows, size_t selected,
              std::string_view status, std::string_view footer);
    const std::vector<uint32_t> &Pixels() const {
        return m_pixels;
    }

private:
    struct Font;

    void Fill(int x, int y, int width, int height, uint32_t color);
    void Text(int x, int baseline, std::string_view text, const Font &font, uint32_t color, int maxWidth);

    std::vector<uint32_t> m_pixels;
    std::unique_ptr<Font> m_title;
    std::unique_ptr<Font> m_body;
    std::unique_ptr<Font> m_small;
};

} // namespace ps5
