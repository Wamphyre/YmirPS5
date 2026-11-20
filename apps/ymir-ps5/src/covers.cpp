// SPDX-License-Identifier: GPL-3.0-or-later

#include "covers.hpp"

#include <ymir/media/loader/loader.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <memory>
#include <span>
#include <stb_image.h>
#include <string_view>
#include <unordered_map>

namespace ps5 {

namespace {

    namespace fs = std::filesystem;
    constexpr unsigned kMaxImageBytes = 6 * 1024 * 1024;
    constexpr unsigned kMaxCoverSize = 256;
    const fs::path kUserCovers{"/data/YmirPS5/covers"};

    std::string Key(std::string_view name) {
        std::string key;
        for (unsigned char c : name) {
            if (c >= 'A' && c <= 'Z') {
                key += char(c + 'a' - 'A');
            } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 128) {
                key += char(c);
            }
        }
        return key;
    }

    std::string_view Title(std::string_view name) {
        return name.substr(0, name.find(" ("));
    }

    CoverImage Decode(std::span<const uint8_t> bytes) {
        int width, height;
        if (bytes.empty() || !stbi_info_from_memory(bytes.data(), int(bytes.size()), &width, &height, nullptr) ||
            width <= 0 || height <= 0 || width > 4096 || height > 4096) {
            return {};
        }
        const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> source{
            stbi_load_from_memory(bytes.data(), int(bytes.size()), &width, &height, nullptr, STBI_rgb), stbi_image_free};
        if (!source) {
            return {};
        }
        const unsigned scale = std::max<unsigned>({kMaxCoverSize, unsigned(width), unsigned(height)});
        CoverImage image;
        image.width = std::max(1u, width * kMaxCoverSize / scale);
        image.height = std::max(1u, height * kMaxCoverSize / scale);
        image.rgb.resize(image.width * image.height * 3);
        for (unsigned y = 0; y < image.height; ++y) {
            for (unsigned x = 0; x < image.width; ++x) {
                const unsigned pixel = (y * height / image.height * width + x * width / image.width) * 3;
                std::copy_n(source.get() + pixel, 3, image.rgb.data() + (y * image.width + x) * 3);
            }
        }
        return image;
    }

    CoverImage ReadCover(const fs::path &file, uint64_t offset, uint64_t length) {
        if (length == 0 || length > kMaxImageBytes) {
            return {};
        }
        std::ifstream input(file, std::ios::binary);
        if (!input.seekg(offset)) {
            return {};
        }
        std::vector<uint8_t> bytes(length);
        if (!input.read(reinterpret_cast<char *>(bytes.data()), bytes.size())) {
            return {};
        }
        return Decode(bytes);
    }

    CoverImage Sidecar(const fs::path &image) {
        for (const auto &base : {image.parent_path() / image.stem(), kUserCovers / image.stem()}) {
            for (const char *extension : {".png", ".jpg", ".jpeg"}) {
                auto path = base;
                path += extension;
                std::error_code error;
                const auto length = fs::file_size(path, error);
                if (!error) {
                    auto cover = ReadCover(path, 0, length);
                    if (!cover.rgb.empty()) {
                        return cover;
                    }
                }
            }
        }
        return {};
    }

    struct Entry {
        std::string name;
        uint64_t offset = 0;
        uint64_t length = 0;
    };

    class Catalog {
    public:
        Catalog();
        CoverImage Get(const fs::path &image) const;

    private:
        void Add(std::string_view line, uint64_t fileSize);
        const Entry *Match(const fs::path &image) const;

        fs::path m_art;
        std::vector<Entry> m_entries;
        std::unordered_map<std::string, size_t> m_names;
        std::unordered_map<std::string, std::vector<size_t>> m_titles;
        std::unordered_map<std::string, std::vector<size_t>> m_serials;
    };

    Catalog::Catalog() {
        for (const auto &directory :
             {kUserCovers, fs::path{"/app0/covers"}, fs::path{"/data/homebrew/PPSA98567/covers"}}) {
            std::error_code error;
            const auto art = directory / "art.bin";
            const auto artSize = fs::file_size(art, error);
            if (error || artSize == 0) {
                continue;
            }
            const auto index = directory / "index.tsv";
            const auto indexSize = fs::file_size(index, error);
            if (error || indexSize > 1024 * 1024) {
                continue;
            }
            std::ifstream input(index);
            std::string line;
            if (!std::getline(input, line) || line != "YMIRPS5_COVERS_1") {
                continue;
            }
            while (std::getline(input, line)) {
                Add(line, artSize);
            }
            if (!m_entries.empty()) {
                m_art = art;
                return;
            }
        }
    }

    void Catalog::Add(std::string_view line, uint64_t fileSize) {
        std::array<std::string_view, 3> fields;
        for (auto &field : fields) {
            const auto separator = line.find('\t');
            if (separator == std::string_view::npos) {
                return;
            }
            field = line.substr(0, separator);
            line.remove_prefix(separator + 1);
        }

        Entry entry{std::string(fields[0])};
        const auto parse = [](std::string_view text, uint64_t &value) {
            const auto end = text.data() + text.size();
            const auto result = std::from_chars(text.data(), end, value);
            return result.ec == std::errc{} && result.ptr == end;
        };
        const auto key = Key(entry.name);
        if (key.empty() || !parse(fields[1], entry.offset) || !parse(fields[2], entry.length) ||
            entry.offset >= fileSize || entry.length == 0 || entry.length > fileSize - entry.offset) {
            return;
        }
        const size_t id = m_entries.size();
        m_names.try_emplace(key, id);
        m_titles[Key(Title(entry.name))].push_back(id);
        while (!line.empty()) {
            const auto separator = line.find('|');
            const auto serial = Key(line.substr(0, separator));
            if (!serial.empty()) {
                m_serials[serial].push_back(id);
            }
            line.remove_prefix(separator == std::string_view::npos ? line.size() : separator + 1);
        }
        m_entries.push_back(std::move(entry));
    }

    const Entry *Catalog::Match(const fs::path &image) const {
        if (m_entries.empty()) {
            return nullptr;
        }
        const auto name = image.stem().string();
        if (const auto match = m_names.find(Key(name)); match != m_names.end()) {
            return &m_entries[match->second];
        }

        ymir::media::Disc disc;
        if (!m_serials.empty() && ymir::media::LoadDisc(image, disc, false, [](auto, const auto &) {}) &&
            disc.header.IsValid()) {
            if (const auto match = m_serials.find(Key(disc.header.productNumber)); match != m_serials.end()) {
                const auto &ids = match->second;
                const auto &regions = disc.header.rawCompatAreaCode;
                const auto preferred = std::find_if(ids.begin(), ids.end(), [&](size_t id) {
                    const auto &name = m_entries[id].name;
                    return (regions.find('J') != std::string::npos && name.find("(Japan)") != std::string::npos) ||
                           (regions.find('U') != std::string::npos && name.find("(USA)") != std::string::npos) ||
                           (regions.find('E') != std::string::npos && name.find("(Europe)") != std::string::npos);
                });
                return &m_entries[preferred == ids.end() ? ids.front() : *preferred];
            }
        }

        const auto match = m_titles.find(Key(Title(name)));
        if (match == m_titles.end()) {
            return nullptr;
        }
        // A plain filename can match multiple releases of the same game.
        const auto &ids = match->second;
        return &m_entries[ids.front()];
    }

    CoverImage Catalog::Get(const fs::path &image) const {
        if (const auto *entry = Match(image)) {
            auto cover = ReadCover(m_art, entry->offset, entry->length);
            if (!cover.rgb.empty()) {
                cover.title = Title(entry->name);
            }
            return cover;
        }
        return {};
    }

} // namespace

CoverImage FindCover(const fs::path &image) {
    if (auto cover = Sidecar(image); !cover.rgb.empty()) {
        return cover;
    }
    static const Catalog catalog;
    try {
        return catalog.Get(image);
    } catch (const std::exception &) {
        return {};
    }
}

} // namespace ps5
