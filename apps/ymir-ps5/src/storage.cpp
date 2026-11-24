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
#include <set>
#include <unistd.h>

namespace ps5 {

namespace fs = std::filesystem;

namespace {

    constexpr std::array<std::string_view, 5> kImageExtensions{".ccd", ".chd", ".cue", ".iso", ".mds"};
    constexpr uintmax_t kMaxStateSize = 32 * 1024 * 1024;
    const fs::path kDataDirectory{"/data/YmirPS5"};
    const fs::path kGamesDirectory = kDataDirectory / "games";
    const fs::path kFallbackDirectory{"/download0"};

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

    bool ReadGameIndex(const fs::path &directory, std::vector<ImageEntry> &entries) {
        const auto relativeDirectory = directory.lexically_relative(kGamesDirectory);
        if (relativeDirectory.empty() || *relativeDirectory.begin() == "..") {
            return false;
        }

        std::ifstream index(kDataDirectory / "library.txt");
        std::string line;
        if (!std::getline(index, line) || line != "YMIRPS5_GAMES_1") {
            return false;
        }
        std::vector<ImageEntry> indexed;
        std::set<fs::path> added;
        while (std::getline(index, line)) {
            const fs::path game{line};
            if (game.empty() || game.is_absolute() || std::ranges::find(game, fs::path("..")) != game.end() ||
                !IsImage(game)) {
                continue;
            }
            const auto relative = game.lexically_relative(relativeDirectory);
            if (relative.empty() || *relative.begin() == ".." || relative == ".") {
                continue;
            }
            const auto entry = directory / *relative.begin();
            const bool folder = relative.has_parent_path();
            std::error_code error;
            const bool exists = folder ? fs::is_directory(entry, error) : fs::is_regular_file(entry, error);
            if (exists && added.insert(entry).second) {
                indexed.push_back({entry, folder});
            }
        }
        if (index.bad()) {
            return false;
        }
        entries = std::move(indexed);
        return true;
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

fs::path FindBIOS() {
    constexpr std::array names{kBIOSFilename, "bios.bin"};
    std::error_code error;
    for (const auto &directory : {fs::path{kBIOSDirectory}, kDataDirectory, kFallbackDirectory}) {
        for (const auto *name : names) {
            const auto path = directory / name;
            if (fs::is_regular_file(path, error) && fs::file_size(path, error) == ymir::sys::kIPLSize &&
                std::ifstream(path, std::ios::binary)) {
                return path;
            }
        }
    }
    return {};
}

std::vector<ImageEntry> ImageRoots() {
    std::vector<ImageEntry> roots;
    std::error_code error;
    for (const auto &path : {kGamesDirectory, fs::path{"/app0/games"}, kFallbackDirectory / "games"}) {
        if (fs::is_directory(path, error)) {
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
    if (error && ReadGameIndex(path, files)) {
        error.clear();
    }
    std::ranges::sort(files, [](const auto &a, const auto &b) {
        if (a.folder != b.folder) {
            return a.folder;
        }
        return Lower(a.path.filename().string()) < Lower(b.path.filename().string());
    });
    return files;
}

Storage::Storage() {
    std::error_code error;
    for (const auto *directory : {"games", "bios", "states", "covers"}) {
        const auto path = kDataDirectory / directory;
        fs::create_directories(path, error);
        if (error && m_setupError.empty()) {
            m_setupError = "Could not create " + path.string() + ". Check storage access.";
        }
    }

    const auto bios = FindBIOS();
    const auto biosDirectory = bios.empty() ? fs::path{kBIOSDirectory} : bios.parent_path();
    const bool dataWritable = CanWriteDirectory(kDataDirectory);
    if ((biosDirectory == kFallbackDirectory || !dataWritable) && CanWriteDirectory(kFallbackDirectory)) {
        m_root = kFallbackDirectory;
    } else if (dataWritable) {
        m_root = kDataDirectory;
    }
    if (!dataWritable && m_setupError.empty()) {
        m_setupError = "Could not write to " + kDataDirectory.string() + ". Check storage access.";
    }

    const auto backup = (m_root.empty() ? kDataDirectory : m_root) / "backup.bin";
    const auto legacyBackup = biosDirectory / "backup.bin";
    m_backupPath = !fs::exists(backup, error) && fs::is_regular_file(legacyBackup, error) ? legacyBackup : backup;
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

AudioSettings Storage::ReadAudioSettings() const {
    if (!m_root.empty()) {
        std::ifstream in(m_root / "audio.cfg");
        std::string magic;
        unsigned gameVolume, menuVolume, mute;
        if (in >> magic >> gameVolume >> menuVolume >> mute && magic == "YMIRPS5_AUDIO_1" &&
            gameVolume <= 100 && menuVolume <= 100 && mute <= 1) {
            return {gameVolume, menuVolume, mute != 0};
        }
    }
    return {};
}

bool Storage::WriteAudioSettings(const AudioSettings &settings) const {
    return !m_root.empty() && WriteAtomically(m_root / "audio.cfg", [&](std::ostream &out) {
        out << "YMIRPS5_AUDIO_1\n"
            << settings.gameVolume << ' ' << settings.menuVolume << ' ' << settings.mute << '\n';
    });
}

CartridgeMode Storage::ReadCartridgeMode() const {
    if (!m_root.empty()) {
        std::ifstream in(m_root / "cartridge.cfg");
        std::string magic;
        unsigned mode = 0;
        if (in >> magic >> mode && magic == "YMIRPS5_CART_1" && mode < unsigned(CartridgeMode::Count)) {
            return CartridgeMode(mode);
        }
    }
    return CartridgeMode::Automatic;
}

bool Storage::WriteCartridgeMode(CartridgeMode mode) const {
    return !m_root.empty() && WriteAtomically(m_root / "cartridge.cfg", [&](std::ostream &out) {
        out << "YMIRPS5_CART_1\n" << unsigned(mode) << '\n';
    });
}

bool Storage::ReadSystemSettings(ymir::smpc::PersistentSMPCData &settings) const {
    if (m_root.empty()) {
        return false;
    }
    std::ifstream in(m_root / "saturn.cfg");
    std::string magic;
    std::array<unsigned, 4> memory{};
    unsigned initialized;
    ymir::smpc::PersistentSMPCData saved{};
    if (!(in >> magic >> memory[0] >> memory[1] >> memory[2] >> memory[3] >> initialized >>
          saved.rtc.offset >> saved.rtc.timestamp) ||
        magic != "YMIRPS5_SATURN_1" || initialized > 1 ||
        std::ranges::any_of(memory, [](unsigned value) { return value > 255; })) {
        return false;
    }
    // Discard corrupted values outside the range of the Saturn's four-digit year.
    constexpr int64_t maxSeconds = 10'000LL * 366 * 24 * 60 * 60;
    if (saved.rtc.offset < -maxSeconds || saved.rtc.offset > maxSeconds ||
        saved.rtc.timestamp < -maxSeconds || saved.rtc.timestamp > maxSeconds) {
        return false;
    }
    std::copy(memory.begin(), memory.end(), saved.SMEM.begin());
    saved.STE = initialized != 0;
    settings = saved;
    return true;
}

bool Storage::WriteSystemSettings(const ymir::smpc::PersistentSMPCData &settings) const {
    return !m_root.empty() && WriteAtomically(m_root / "saturn.cfg", [&](std::ostream &out) {
        out << "YMIRPS5_SATURN_1\n";
        for (const auto value : settings.SMEM) {
            out << unsigned(value) << ' ';
        }
        out << settings.STE << ' ' << settings.rtc.offset << ' ' << settings.rtc.timestamp << '\n';
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
        if (!saturn.LoadState(*state)) {
            return false;
        }
        // Older PS5 save states represent a Saturn with an empty cartridge slot.
        if (state->scu.cartType == ymir::savestate::SCUSaveState::CartType::None &&
            saturn.GetCartridge().GetType() != ymir::cart::CartType::None) {
            saturn.RemoveCartridge();
        }
        return true;
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
