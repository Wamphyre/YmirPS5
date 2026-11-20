// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps5 {

struct CoverImage {
    unsigned width = 0;
    unsigned height = 0;
    std::vector<uint8_t> rgb;
    std::string title;
};

CoverImage FindCover(const std::filesystem::path &image);

} // namespace ps5
