// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ymir {
struct Saturn;
}

namespace ps5 {

enum class Aspect { FourThree, SixteenNine, SixteenTen, Source, Count };
enum class Resolution { HD, QHD, UHD, Count };

struct VideoSettings {
    Resolution resolution = Resolution::HD;
    Aspect aspect = Aspect::FourThree;
    bool fsr = false;
    unsigned frameGen = 0; // 0, 1, or 3 frames inserted between originals
};

struct ImageEntry {
    std::filesystem::path path;
    bool folder;
};

std::filesystem::path FindBIOS();
std::vector<ImageEntry> ImageRoots();
std::vector<ImageEntry> ListImages(const std::filesystem::path &path, std::error_code &error);

class Storage {
public:
    Storage();

    const std::string &SetupError() const {
        return m_setupError;
    }

    VideoSettings ReadVideoSettings() const;
    bool WriteVideoSettings(const VideoSettings &settings) const;

    std::filesystem::path StatePath(const ymir::Saturn &saturn, unsigned slot) const;
    bool SaveState(const ymir::Saturn &saturn, unsigned slot) const;
    bool LoadState(ymir::Saturn &saturn, unsigned slot) const;

    const std::filesystem::path &BackupPath() const {
        return m_backupPath;
    }
    std::filesystem::path MemoryTransferPath() const;
    bool ExportMemory(ymir::Saturn &saturn) const;
    bool ImportMemory(ymir::Saturn &saturn) const;

private:
    std::string m_setupError;
    std::filesystem::path m_root;
    std::filesystem::path m_backupPath;
};

} // namespace ps5
