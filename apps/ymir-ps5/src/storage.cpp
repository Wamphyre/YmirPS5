// SPDX-License-Identifier: GPL-3.0-or-later

#include "storage.hpp"

#include <ymir/sys/backup_ram.hpp>
#include <ymir/sys/saturn.hpp>

#include <cereal/archives/portable_binary.hpp>
#include <serdes/cereal_savestate.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <fcntl.h>
#include <fstream>
#include <ranges>
#include <unistd.h>

namespace ps5 {

namespace fs = std::filesystem;

namespace {

    constexpr std::array<std::string_view, 5> kImageExtensions{".ccd", ".chd", ".cue", ".iso", ".mds"};
    constexpr uintmax_t kMaxStateSize = 32 * 1024 * 1024;

    std::string Lower(std::string text) {
        for (char &c : text) {
            if (c >= 'A' && c <= 'Z') {
                c += 'a' - 'A';
            }
        }
        return text;
    }

    bool IsImage(const fs::path &path) {
        return std::ranges::find(kImageExtensions, Lower(path.extension().string())) != kImageExtensions.end();
    }

    template <typename Writer>
    bool WriteAtomically(const fs::path &path, const Writer &write) {
        if (path.empty()) {
            return false;
        }
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        if (error) {
            return false;
        }

        static std::atomic_uint counter{};
        fs::path temp;
        int fd = -1;
        do {
            temp = path.string() + ".tmp." + std::to_string(getpid()) + "." + std::to_string(counter++);
            fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        } while (fd < 0 && errno == EEXIST);
        if (fd < 0) {
            return false;
        }

        bool ok = false;
        try {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (out) {
                write(out);
                out.close();
                ok = out.good() && fsync(fd) == 0;
            }
        } catch (const std::exception &) {
            ok = false;
        }
        close(fd);
        if (ok) {
            fs::rename(temp, path, error);
            ok = !error;
        }
        if (!ok) {
            fs::remove(temp, error);
        }
        return ok;
    }

    bool CanWriteDirectory(const fs::path &path) {
        static std::atomic_uint counter{};
        std::error_code error;
        fs::create_directories(path, error);
        if (error) {
            return false;
        }
        const auto probe = path / (".ymir-write-" + std::to_string(getpid()) + "-" + std::to_string(counter++));
        const int fd = open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            return false;
        }
        close(fd);
        unlink(probe.c_str());
        return true;
    }

} // namespace

std::vector<ImageEntry> ImageRoots(const fs::path &biosDirectory) {
    std::vector<ImageEntry> roots;
    std::error_code error;
    for (const auto &path : {fs::path{"/app0/games"}, fs::path{"/data/YmirPS5/games"}, fs::path{"/download0/games"},
                             biosDirectory / "games"}) {
        if (fs::is_directory(path, error) && std::ranges::find(roots, path, &ImageEntry::path) == roots.end()) {
            roots.push_back({path, true});
        }
    }
    for (unsigned i = 0; i < 8; ++i) {
        fs::path path = "/mnt/usb" + std::to_string(i);
        if (fs::is_directory(path, error)) {
            roots.push_back({std::move(path), true});
        }
    }
    return roots;
}

std::vector<ImageEntry> ListImages(const fs::path &path, std::error_code &error) {
    std::vector<ImageEntry> files;
    const fs::directory_iterator end;
    for (fs::directory_iterator it(path, error); !error && it != end; it.increment(error)) {
        std::error_code typeError;
        const bool folder = it->is_directory(typeError);
        if (folder || (IsImage(it->path()) && it->is_regular_file(typeError))) {
            files.push_back({it->path(), folder});
        }
    }
    std::ranges::sort(files, [](const auto &a, const auto &b) {
        if (a.folder != b.folder) {
            return a.folder;
        }
        return Lower(a.path.filename().string()) < Lower(b.path.filename().string());
    });
    return files;
}

Storage::Storage(const fs::path &bios)
    : m_biosDirectory(bios.has_parent_path() ? bios.parent_path() : fs::path{"."}) {
    for (const auto &candidate : {m_biosDirectory, fs::path{"/data/YmirPS5"}, fs::path{"/download0"}}) {
        if (CanWriteDirectory(candidate)) {
            m_root = candidate;
            break;
        }
    }
    m_backupPath = m_biosDirectory / "backup.bin";
    std::error_code error;
    if (!m_root.empty() && m_root != m_biosDirectory && !fs::exists(m_backupPath, error)) {
        m_backupPath = m_root / "backup.bin";
    }
}

VideoSettings Storage::ReadVideoSettings() const {
    VideoSettings settings;
    if (m_root.empty()) {
        return settings;
    }
    std::ifstream in(m_root / "video.cfg");
    std::string magic;
    unsigned resolution = 0, aspect = 0, fsr = 0, frameGen = 0;
    if (in >> magic >> resolution >> aspect >> fsr >> frameGen && magic == "YMIRPS5_VIDEO_1") {
        if (resolution < unsigned(Resolution::Count) && aspect < unsigned(Aspect::Count) && fsr < 2 &&
            (frameGen == 0 || frameGen == 1 || frameGen == 3)) {
            settings = {Resolution(resolution), Aspect(aspect), fsr != 0, frameGen};
        }
    }
    return settings;
}

bool Storage::WriteVideoSettings(const VideoSettings &settings) const {
    return !m_root.empty() && WriteAtomically(m_root / "video.cfg", [&](std::ostream &out) {
        out << "YMIRPS5_VIDEO_1\n"
            << unsigned(settings.resolution) << ' ' << unsigned(settings.aspect) << ' ' << settings.fsr << ' '
            << settings.frameGen << '\n';
    });
}

fs::path Storage::StatePath(const ymir::Saturn &saturn, unsigned slot) const {
    const auto hash = saturn.GetDiscHash();
    if (m_root.empty() || hash == ymir::XXH128Hash{} || slot == 0 || slot > 5 || !saturn.GetCDInterface().HasDisc()) {
        return {};
    }
    return m_root / "states" / ymir::ToString(hash) / ("slot-" + std::to_string(slot) + ".savestate");
}

bool Storage::SaveState(const ymir::Saturn &saturn, unsigned slot) const {
    return WriteAtomically(StatePath(saturn, slot), [&](std::ostream &out) {
        auto state = std::make_unique<ymir::savestate::SaveState>();
        saturn.SaveState(*state);
        cereal::PortableBinaryOutputArchive archive{out};
        archive(*state);
    });
}

bool Storage::LoadState(ymir::Saturn &saturn, unsigned slot) const {
    std::error_code error;
    const fs::path path = StatePath(saturn, slot);
    if (path.empty() || !fs::is_regular_file(path, error) || fs::file_size(path, error) > kMaxStateSize || error) {
        return false;
    }
    try {
        auto state = std::make_unique<ymir::savestate::SaveState>();
        std::ifstream in(path, std::ios::binary);
        cereal::PortableBinaryInputArchive archive{in};
        archive(*state);
        return saturn.LoadState(*state);
    } catch (const std::exception &) {
        return false;
    }
}

fs::path Storage::MemoryTransferPath() const {
    return m_root.empty() ? fs::path{} : m_root / "memory-transfer.bin";
}

bool Storage::ExportMemory(ymir::Saturn &saturn) const {
    const auto bytes = saturn.mem.GetInternalBackupRAM().ReadAll();
    return WriteAtomically(MemoryTransferPath(), [&](std::ostream &out) {
        out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    });
}

bool Storage::ImportMemory(ymir::Saturn &saturn) const {
    const auto path = MemoryTransferPath();
    auto &target = saturn.mem.GetInternalBackupRAM();
    std::error_code error;
    if (path.empty() || !fs::is_regular_file(path, error) || fs::file_size(path, error) != target.Size() || error) {
        return false;
    }
    ymir::bup::BackupMemory source;
    return source.LoadFrom(path, true, error) == ymir::bup::BackupMemoryImageLoadResult::Success &&
           source.IsHeaderValid() && target.CopyFrom(source);
}

} // namespace ps5
