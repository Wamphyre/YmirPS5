// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform.hpp"

#include "covers.hpp"
#include "menu.hpp"
#include "unlock.hpp"
#if Ymir_FF_HOST_CD_DRIVES
#include "disc_launcher.hpp"
#endif

#include <ymir/media/host_cd.hpp>
#include <ymir/media/loader/loader.hpp>
#include <ymir/sys/clocks.hpp>
#include <ymir/sys/saturn.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <string_view>

extern "C" int sceSystemServiceHideSplashScreen();
extern "C" int sceSystemServiceLoadExec(const char *path, const char **arguments);

namespace {

namespace fs = std::filesystem;
using Button = ymir::peripheral::Button;
using ymir::media::DriveState;
using ps5::MenuHint;
using ps5::MenuIcon;

constexpr char kUSBLogPath[] = "/data/YmirPS5/usb.log";

void ReportUSB(const char *message) {
    ps5::Report(message);
    if (auto *file = std::fopen(kUSBLogPath, "a")) {
        std::fprintf(file, "%s\n", message);
        std::fclose(file);
    }
}

bool LoadBIOS(ymir::Saturn &saturn, const fs::path &path, const fs::path &backup) {
    auto rom = std::make_unique<std::array<uint8, ymir::sys::kIPLSize>>();
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char *>(rom->data()), rom->size()) ||
        file.peek() != std::ifstream::traits_type::eof()) {
        ps5::Report(("Expected a 512 KiB Saturn BIOS at " + path.string()).c_str());
        return false;
    }
    saturn.LoadIPL(*rom);

    std::error_code error;
    saturn.LoadInternalBackupMemoryImage(backup, false, error);
    if (error) {
        ps5::Report(("Internal backup RAM will be volatile: " + error.message()).c_str());
    }
    return true;
}

std::string MountUSB(ymir::Saturn &saturn) {
    if (auto *file = std::fopen(kUSBLogPath, "w")) {
        std::fclose(file);
    }
    ReportUSB("Looking for an optical drive");
    const auto mount = [&saturn](const std::vector<ymir::media::host::HostDriveInfo> &drives) {
        for (bool discPresent : {true, false}) {
            for (const auto &drive : drives) {
                if ((drive.driveState == DriveState::MediaPresent) != discPresent) {
                    continue;
                }
                if (saturn.OpenHostCDDrive(drive.path, true)) {
                    const auto &disc = saturn.GetCDInterface();
                    ReportUSB(("Saturn CD ready on " + drive.path + ": " + disc.GetDiscHeader().gameTitle).c_str());
                    return true;
                }
                ReportUSB(("Could not read a Saturn CD from " + drive.path).c_str());
            }
        }
        return false;
    };

    auto drives = ymir::media::host::EnumerateHostCDDrives();
    if (mount(drives)) {
        return {};
    }

    // A title started from the dashboard can be sandboxed away from the CAM
    // devices. The service runs from the ELF loader and opens the reader with
    // its own process permissions, so retry through it when direct access fails.
#if Ymir_FF_HOST_CD_DRIVES
    if (ps5::EnsureDiscService(ReportUSB)) {
        drives = ymir::media::host::EnumerateHostCDDrives();
        if (mount(drives)) {
            return {};
        }
    }
#endif

    const std::string reason = drives.empty() ? "No accessible USB optical drive."
                                              : "Reader found, but no readable Saturn game disc.";
    ReportUSB(reason.c_str());
    return reason + " See /data/YmirPS5/usb.log.";
}

Button MapButtons(uint32_t buttons) {
    static constexpr std::pair<ps5::PadButton, Button> mapping[] = {
        {ps5::Up, Button::Up},         {ps5::Right, Button::Right}, {ps5::Down, Button::Down},
        {ps5::Left, Button::Left},     {ps5::Cross, Button::A},     {ps5::Circle, Button::B},
        {ps5::R1, Button::C},          {ps5::Square, Button::X},    {ps5::Triangle, Button::Y},
        {ps5::L1, Button::Z},          {ps5::L2, Button::L},        {ps5::R2, Button::R},
        {ps5::Options, Button::Start},
    };
    Button report = Button::Default;
    for (const auto &[physical, emulated] : mapping) {
        if ((buttons & physical) != 0) {
            report &= ~emulated;
        }
    }
    return report;
}

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

enum class Page { Home, Game, Images, Video, Audio, Saves, Slots, Slot, Memory, Confirm, Controls };
enum class HomeOption { BootUSB, Library, BootBIOS, Video, Audio, Saves, Exit };
enum class GameOption { Resume, Close, Restart, Save, Load, Exit, Video, Audio, Saves };
enum class StateAction { Manage, Save, Load };

constexpr std::array kResolutionLabels{"1080p", "1440p", "4K (2160p)", "Native"};
constexpr std::array kAspectLabels{"4:3", "16:9", "16:10", "Square pixels"};

std::string VideoProfile(const ps5::VideoSettings &settings) {
    return std::string(kResolutionLabels[unsigned(settings.resolution)]) + " · " +
           kAspectLabels[unsigned(settings.aspect)];
}

class Frontend {
public:
    Frontend()
        : m_settings(m_storage.ReadVideoSettings())
        , m_audioSettings(m_storage.ReadAudioSettings())
        , m_saturn(std::make_unique<ymir::Saturn>()) {}

    int Run(std::ofstream &launchLog);

private:
    bool EnsureBIOS();
    void BootBIOS();
    bool Boot(std::string_view source);
    void FinishBoot(std::string game);
    void CloseGame();
    void Resume();
    void OpenMenu();
    void PollMedia();
    double CoreHz() const;

    void SetPage(Page page);
    void BuildRows();
    void Input(uint32_t held, uint32_t pressed);
    void Activate(int delta = 1);
    void Back();
    void OpenLibrary();
    void Browse(const fs::path &directory);
    void UpdateCovers();
    void ChangeVideo(int direction);
    void ChangeAudio(int direction);
    void SaveState();
    void LoadState();
    void Ask(std::string question, std::function<std::string()> action);
    void Status(std::string text);

    ps5::Storage m_storage;
    ps5::VideoSettings m_settings;
    ps5::AudioSettings m_audioSettings;
    ps5::MenuCanvas m_menu;
    ps5::Video m_video;
    ps5::Pad m_pad;
    ps5::Audio m_audio;
    uint32_t m_saturnButtons = 0;
    std::unique_ptr<ymir::Saturn> m_saturn;
    std::future<std::string> m_usbMount;
    Clock::time_point m_usbStarted{};
    bool m_usbFrameShown = false;

    bool m_hasBIOS = false;
    bool m_started = false;
    bool m_menuOpen = true;
    bool m_running = true;
    bool m_dirty = true;
    bool m_menuPresented = false;
    Clock::time_point m_lastMenuPresent{};
    bool m_waitForRelease = false;
    DriveState m_driveState = DriveState::Unknown;
    std::string m_game;
    Clock::time_point m_emuDeadline{};

    Page m_page = Page::Home;
    Page m_returnPage = Page::Home;
    Page m_controlsReturnPage = Page::Home;
    size_t m_controlsReturnSelection = 0;
    size_t m_selected = 0;
    unsigned m_slot = 1;
    StateAction m_stateAction = StateAction::Manage;
    std::string m_title;
    std::string m_detail;
    std::string m_status;
    std::vector<ps5::MenuRow> m_rows;
    std::function<std::string()> m_confirm;
    fs::path m_directory;
    fs::path m_browserRoot;
    std::vector<ps5::ImageEntry> m_images;
    struct LibraryCovers {
        size_t generation = 0;
        size_t first = 0;
        std::vector<ps5::CoverImage> images;
    };
    size_t m_libraryGeneration = 1;
    LibraryCovers m_libraryCovers;
    std::future<LibraryCovers> m_coverTask;
    std::vector<ymir::bup::BackupFileInfo> m_memoryFiles;
    uint32_t m_repeatDirection = 0;
    Clock::time_point m_nextRepeat{};
};

bool Frontend::EnsureBIOS() {
    if (m_hasBIOS) {
        return true;
    }
    const auto bios = ps5::FindBIOS();
    m_hasBIOS = !bios.empty() && LoadBIOS(*m_saturn, bios, m_storage.BackupPath());
    if (!m_hasBIOS) {
        Status("Expected a readable 512 KiB Saturn BIOS at " +
               (fs::path{ps5::kBIOSDirectory} / ps5::kBIOSFilename).string());
        return false;
    }
    ymir::smpc::PersistentSMPCData settings{};
    if (m_storage.ReadSystemSettings(settings)) {
        m_saturn->SMPC.LoadPersistentData(settings);
    }
    m_saturn->SMPC.SetPersistDataCallback({&m_storage,
        [](const ymir::smpc::PersistentSMPCData &data, void *context) {
            if (!static_cast<const ps5::Storage *>(context)->WriteSystemSettings(data)) {
                ps5::Report("Could not save the Saturn clock and system settings");
            }
        }});
    return true;
}

bool Frontend::Boot(std::string_view source) {
    if (!EnsureBIOS()) {
        return false;
    }

    if (source.empty()) {
        m_usbStarted = Clock::now();
        m_usbFrameShown = false;
        m_usbMount = std::async(std::launch::async, [this] { return MountUSB(*m_saturn); });
        return true;
    }

    ymir::media::Disc disc;
    if (!ymir::media::LoadDisc(fs::path(source), disc, false,
                               [](auto, const std::string &message) { ps5::Report(message.c_str()); })) {
        Status("Could not open the disc image. Check its format and companion files.");
        return false;
    }
    m_saturn->LoadDisc(std::move(disc));
    FinishBoot(fs::path(source).filename().string());
    return true;
}

void Frontend::BootBIOS() {
    if (EnsureBIOS()) {
        m_saturn->EjectDisc();
        m_saturn->UsePreferredRegion();
        FinishBoot("Saturn BIOS");
    }
}

void Frontend::FinishBoot(std::string game) {
    m_game = std::move(game);
    m_saturn->Reset(true);
    m_video.ClearGameFrame();
    m_driveState = DriveState::Unknown;
    m_started = true;
    Resume();
}

void Frontend::CloseGame() {
    m_saturn->EjectDisc();
    m_video.ClearGameFrame();
    m_driveState = DriveState::Unknown;
    m_game.clear();
    m_started = false;
    OpenMenu();
}

void Frontend::Resume() {
    m_audio.Clear();
    m_menuOpen = false;
    m_waitForRelease = true;
    m_emuDeadline = Clock::now();
}

void Frontend::OpenMenu() {
    m_audio.Clear();
    m_saturnButtons = 0;
    m_menuOpen = true;
    SetPage(m_started ? Page::Game : Page::Home);
}

void Frontend::PollMedia() {
    if (!m_started) {
        return;
    }
    const auto &cd = m_saturn->GetCDInterface();
    const auto state = cd.PollDriveState();
    if (state == m_driveState) {
        return;
    }
    if (state == DriveState::MediaPresent) {
        if (cd.GetDiscHeader().IsValid()) {
            m_saturn->AutodetectRegion();
            m_game = cd.GetDiscHeader().gameTitle;
            ps5::Report(("Saturn disc: " + m_game).c_str());
        } else {
            ps5::Report("Disc present, but no Saturn header could be read");
        }
    } else if (m_driveState == DriveState::MediaPresent) {
        ps5::Report("Disc removed");
    }
    m_driveState = state;
}

double Frontend::CoreHz() const {
    const bool pal = m_saturn->GetVideoStandard() == ymir::core::config::sys::VideoStandard::PAL;
    return pal ? ymir::sys::kPALFrameRate : ymir::sys::kNTSCFrameRate;
}

void Frontend::Status(std::string text) {
    if (!text.empty()) {
        ps5::Report(text.c_str());
    }
    m_status = std::move(text);
    m_dirty = true;
}

void Frontend::SetPage(Page page) {
    m_page = page;
    m_selected = 0;
    m_status.clear();
    BuildRows();
}

void Frontend::BuildRows() {
    m_rows.clear();
    m_detail.clear();

    switch (m_page) {
    case Page::Home:
        m_title = "Home";
        m_detail = "Choose a disc or start the Saturn BIOS.";
        m_rows = {{"Boot USB disc", "Optical drive", true, MenuIcon::Disc},
                  {"Game library", "CHD, CUE, CCD, MDS, ISO", true, MenuIcon::Folder},
                  {"Boot Saturn BIOS", "Without a disc", true, MenuIcon::Play},
                  {"Video settings", VideoProfile(m_settings), true, MenuIcon::Video},
                  {"Audio settings", "", true, MenuIcon::Audio},
                  {"Saved games", "", true, MenuIcon::Save},
                  {"Exit YmirPS5", "", true, MenuIcon::Exit}};
        break;
    case Page::Game: {
        const bool hasStates = !m_storage.StatePath(*m_saturn, 1).empty();
        const char *stateDetail = hasStates ? "" : "Requires an identified disc";
        m_title = "Game menu";
        m_detail = m_game;
        m_rows = {{"Resume", "", true, MenuIcon::Play},
                  {"Close game", "Return to Home", true, MenuIcon::Folder},
                  {"Restart game", "", true, MenuIcon::Play},
                  {"Save state", stateDetail, hasStates, MenuIcon::Save},
                  {"Load state", stateDetail, hasStates, MenuIcon::Save},
                  {"Exit YmirPS5", "Return to the PS5 home screen", true, MenuIcon::Exit},
                  {"Video settings", VideoProfile(m_settings), true, MenuIcon::Video},
                  {"Audio settings", "", true, MenuIcon::Audio},
                  {"Saved games", "", true, MenuIcon::Save}};
        break;
    }
    case Page::Images:
        m_title = "Game library";
        m_detail = m_directory.empty() ? "Choose a storage location." : m_directory.string();
        for (const auto &entry : m_images) {
            m_rows.push_back({entry.folder ? entry.path.filename().string() : entry.path.stem().string(),
                              m_directory.empty() ? entry.path.parent_path().string() :
                              entry.folder ? "Folder" : entry.path.extension().string(), true,
                              entry.folder ? MenuIcon::Folder : MenuIcon::Disc});
        }
        break;
    case Page::Video: {
        m_title = "Video settings";
        m_detail = "Menu output  " + m_video.Description();
        const bool upscale = m_settings.resolution != ps5::Resolution::Native;
        const bool hasGpu = m_video.HasGpu();
        const char *fsrValue = !upscale ? "Choose an upscale mode" : m_settings.fsr ? "On" : "Off";
        m_rows = {{"Output resolution", kResolutionLabels[unsigned(m_settings.resolution)]},
                  {"Aspect ratio", kAspectLabels[unsigned(m_settings.aspect)]},
                  {"AMD FSR 1", hasGpu ? fsrValue : "Requires Vulkan", hasGpu && upscale},
                  {"FSR 3 frame generation",
                   hasGpu ? (m_settings.frameGen ? (m_settings.frameGen == 1 ? "x2" : "x4") : "Off") : "Requires Vulkan",
                   hasGpu}};
        break;
    }
    case Page::Audio:
        m_title = "Audio settings";
        m_rows = {{"Game volume", std::to_string(m_audioSettings.gameVolume) + "%"},
                  {"Menu sounds", std::to_string(m_audioSettings.menuVolume) + "%"},
                  {"Mute all", m_audioSettings.mute ? "On" : "Off"}};
        break;
    case Page::Saves: {
        const bool hasStates = !m_storage.StatePath(*m_saturn, 1).empty();
        m_title = "Saved games";
        m_detail = m_started ? m_game : "";
        m_rows = {{"Save states", hasStates ? "5 slots per disc" : "Requires an identified disc", hasStates},
                  {"Saturn backup memory", ""}};
        break;
    }
    case Page::Slots:
        switch (m_stateAction) {
        case StateAction::Manage: m_title = "Save states"; break;
        case StateAction::Save: m_title = "Save state"; break;
        case StateAction::Load: m_title = "Load state"; break;
        }
        m_detail = m_game;
        for (unsigned slot = 1; slot <= 5; ++slot) {
            std::error_code error;
            const bool occupied = fs::is_regular_file(m_storage.StatePath(*m_saturn, slot), error);
            m_rows.push_back({"Slot " + std::to_string(slot), occupied ? "Saved" : "Empty",
                              m_stateAction != StateAction::Load || occupied});
        }
        break;
    case Page::Slot: {
        std::error_code error;
        const bool occupied = fs::is_regular_file(m_storage.StatePath(*m_saturn, m_slot), error);
        m_title = "Slot " + std::to_string(m_slot);
        m_detail = m_game;
        m_rows = {{"Save state", occupied ? "Overwrite" : "Empty"},
                  {"Load state", "", occupied},
                  {"Delete state", "", occupied}};
        break;
    }
    case Page::Memory: {
        auto &memory = m_saturn->mem.GetInternalBackupRAM();
        m_memoryFiles = memory.List();
        std::error_code error;
        const auto transferPath = m_storage.MemoryTransferPath();
        const bool hasTransfer = !transferPath.empty() && fs::is_regular_file(transferPath, error);
        m_title = "Saturn backup memory";
        m_detail = "Blocks used  " + std::to_string(memory.GetUsedBlocks()) + " / " +
                   std::to_string(memory.GetTotalBlocks());
        m_rows = {{"Export backup memory", "memory-transfer.bin", !transferPath.empty()},
                  {"Import backup memory", "memory-transfer.bin", hasTransfer}};
        for (const auto &file : m_memoryFiles) {
            m_rows.push_back({file.header.filename, std::to_string(file.numBlocks) + " blocks  /  Delete"});
        }
        break;
    }
    case Page::Confirm: m_rows = {{"Cancel", ""}, {"Confirm", ""}}; break;
    case Page::Controls:
        m_title = "DualSense controls";
        m_detail = "Menu shortcuts and Saturn button mapping";
        m_rows = {{"Accept  /  Saturn A", "", false, MenuIcon::Cross},
                  {"Back  /  Saturn B", "", false, MenuIcon::Circle},
                  {"Game library  /  Saturn X", "", false, MenuIcon::Square},
                  {"Controls  /  Saturn Y", "", false, MenuIcon::Triangle},
                  {"Move  /  Saturn D-pad", "D-pad or left stick", false, MenuIcon::DPad},
                  {"Page  /  Saturn Z and C", "L1 and R1", false},
                  {"Saturn L, R and Start", "L2, R2 and Options", false}};
        break;
    }
    if (!m_rows.empty()) {
        m_selected = std::min(m_selected, m_rows.size() - 1);
    }
    m_dirty = true;
}

void Frontend::OpenLibrary() {
    const auto roots = ps5::ImageRoots();
    m_directory.clear();
    Browse(roots.empty() ? fs::path{} : roots.front().path);
}

void Frontend::Browse(const fs::path &directory) {
    std::error_code error;
    auto entries = directory.empty() ? ps5::ImageRoots() : ps5::ListImages(directory, error);
    if (error) {
        ps5::Report(("Library scan " + directory.string() + ": " + error.message()).c_str());
        Status("Could not open this folder  " + directory.string());
        return;
    }
    if (directory.empty()) {
        m_browserRoot.clear();
    } else if (m_directory.empty()) {
        m_browserRoot = directory;
    }
    m_directory = directory;
    m_images = std::move(entries);
    ++m_libraryGeneration;
    m_libraryCovers = {};
    SetPage(Page::Images);
}

void Frontend::UpdateCovers() {
    if (m_coverTask.valid() && m_coverTask.wait_for(0s) == std::future_status::ready) {
        auto covers = m_coverTask.get();
        if (covers.generation == m_libraryGeneration) {
            m_libraryCovers = std::move(covers);
            m_dirty = true;
        }
    }
    const size_t first = m_selected / ps5::kLibraryPageSize * ps5::kLibraryPageSize;
    if (m_page != Page::Images || m_coverTask.valid() || m_images.empty() || m_directory.empty() ||
        (m_libraryCovers.generation == m_libraryGeneration && m_libraryCovers.first == first)) {
        return;
    }

    const size_t end = std::min(first + ps5::kLibraryPageSize, m_images.size());
    std::vector<ps5::ImageEntry> entries(m_images.begin() + first, m_images.begin() + end);
    m_coverTask = std::async(std::launch::async, [entries = std::move(entries), first, generation = m_libraryGeneration] {
        LibraryCovers covers{generation, first, {}};
        for (const auto &entry : entries) {
            covers.images.push_back(entry.folder ? ps5::CoverImage{} : ps5::FindCover(entry.path));
        }
        return covers;
    });
}

void Frontend::ChangeVideo(int direction) {
    auto settings = m_settings;
    switch (m_selected) {
    case 0: {
        constexpr int count = int(ps5::Resolution::Count);
        settings.resolution = ps5::Resolution((int(settings.resolution) + count + direction) % count);
        break;
    }
    case 1: {
        constexpr int count = int(ps5::Aspect::Count);
        settings.aspect = ps5::Aspect((int(settings.aspect) + count + direction) % count);
        break;
    }
    case 2: settings.fsr = !settings.fsr; break;
    case 3: {
        constexpr std::array<unsigned, 3> modes{0, 1, 3};
        const auto index = std::ranges::find(modes, settings.frameGen) - modes.begin();
        settings.frameGen = modes[(index + 3 + direction) % 3];
        break;
    }
    }
    m_settings = settings;
    const bool saved = m_storage.WriteVideoSettings(m_settings);
    BuildRows();
    if (!saved) {
        Status("Settings applied for this session. Could not save them to storage.");
    } else if (m_selected == 3 && m_settings.frameGen) {
        Status("Frame generation depends on the refresh rate and the game's frame rate.");
    } else {
        Status("");
    }
}

void Frontend::ChangeAudio(int direction) {
    const auto adjust = [direction](unsigned volume) {
        return static_cast<unsigned>(std::clamp(int(volume) + 10 * direction, 0, 100));
    };
    switch (m_selected) {
    case 0: m_audioSettings.gameVolume = adjust(m_audioSettings.gameVolume); break;
    case 1: m_audioSettings.menuVolume = adjust(m_audioSettings.menuVolume); break;
    case 2: m_audioSettings.mute = !m_audioSettings.mute; break;
    }
    m_audio.SetSettings(m_audioSettings);
    m_audio.PlayMenuSound(ps5::MenuSound::Confirm);
    const bool saved = m_storage.WriteAudioSettings(m_audioSettings);
    BuildRows();
    Status(saved ? "" : "Settings applied for this session. Could not save them to storage.");
}

void Frontend::SaveState() {
    const auto path = m_storage.StatePath(*m_saturn, m_slot);
    if (path.empty()) {
        Status("The current disc is unavailable.");
        return;
    }

    const auto save = [this]() -> std::string {
        return m_storage.SaveState(*m_saturn, m_slot) ? "State saved." : "Could not save the state.";
    };
    std::error_code error;
    if (fs::is_regular_file(path, error)) {
        Ask("Overwrite this save state?", save);
    } else {
        const auto message = save();
        BuildRows();
        Status(message);
    }
}

void Frontend::LoadState() {
    Ask("Load this save state?", [this] {
        if (!m_storage.LoadState(*m_saturn, m_slot)) {
            return std::string("This state could not be read or is incompatible with the current disc or BIOS.");
        }
        m_video.ClearGameFrame();
        Resume();
        return std::string("State loaded.");
    });
}

void Frontend::Ask(std::string question, std::function<std::string()> action) {
    m_returnPage = m_page;
    m_confirm = std::move(action);
    SetPage(Page::Confirm);
    m_title = std::move(question);
}

void Frontend::Activate(int delta) {
    if (m_rows.empty() || !m_rows[m_selected].enabled) {
        return;
    }
    if (m_page != Page::Audio) {
        m_audio.PlayMenuSound(ps5::MenuSound::Confirm);
    }

    switch (m_page) {
    case Page::Home: {
        switch (static_cast<HomeOption>(m_selected)) {
        case HomeOption::BootUSB: Boot({}); break;
        case HomeOption::Library: OpenLibrary(); break;
        case HomeOption::BootBIOS: BootBIOS(); break;
        case HomeOption::Video: SetPage(Page::Video); break;
        case HomeOption::Audio: SetPage(Page::Audio); break;
        case HomeOption::Saves: SetPage(Page::Saves); break;
        case HomeOption::Exit: m_running = false; break;
        }
        break;
    }
    case Page::Game:
        switch (static_cast<GameOption>(m_selected)) {
        case GameOption::Resume: Resume(); break;
        case GameOption::Close:
            Ask("Close the game and return to Home?", [this] {
                CloseGame();
                return std::string{};
            });
            break;
        case GameOption::Restart:
            Ask("Restart the current game?", [this] {
                FinishBoot(m_game);
                return std::string{};
            });
            break;
        case GameOption::Save:
        case GameOption::Load:
            m_stateAction = static_cast<GameOption>(m_selected) == GameOption::Save ? StateAction::Save : StateAction::Load;
            SetPage(Page::Slots);
            break;
        case GameOption::Exit:
            Ask("Exit YmirPS5?", [this] {
                m_running = false;
                return std::string{};
            });
            break;
        case GameOption::Video: SetPage(Page::Video); break;
        case GameOption::Audio: SetPage(Page::Audio); break;
        case GameOption::Saves: SetPage(Page::Saves); break;
        }
        break;
    case Page::Images:
        if (m_images[m_selected].folder) {
            const auto path = m_images[m_selected].path;
            Browse(path);
        } else {
            Boot(m_images[m_selected].path.string());
        }
        break;
    case Page::Video: ChangeVideo(delta); break;
    case Page::Audio: ChangeAudio(delta); break;
    case Page::Saves:
        if (m_selected == 0) {
            m_stateAction = StateAction::Manage;
            SetPage(Page::Slots);
        } else if (EnsureBIOS()) {
            SetPage(Page::Memory);
        }
        break;
    case Page::Slots:
        m_slot = unsigned(m_selected) + 1;
        switch (m_stateAction) {
        case StateAction::Manage: SetPage(Page::Slot); break;
        case StateAction::Save: SaveState(); break;
        case StateAction::Load: LoadState(); break;
        }
        break;
    case Page::Slot:
        if (m_selected == 0) {
            SaveState();
        } else if (m_selected == 1) {
            LoadState();
        } else {
            const auto path = m_storage.StatePath(*m_saturn, m_slot);
            Ask("Delete this save state?", [path] {
                std::error_code removalError;
                return fs::remove(path, removalError) ? "State deleted." : "Could not delete the state.";
            });
        }
        break;
    case Page::Memory:
        if (m_selected == 0) {
            const auto path = m_storage.MemoryTransferPath();
            const auto doExport = [this, path]() -> std::string {
                return m_storage.ExportMemory(*m_saturn) ? "Backup memory exported to " + path.string()
                                                         : "Could not export backup memory.";
            };
            std::error_code error;
            if (fs::exists(path, error)) {
                Ask("Overwrite the export?", doExport);
            } else {
                const auto message = doExport();
                BuildRows();
                Status(message);
            }
        } else if (m_selected == 1) {
            Ask("Replace Saturn backup memory?", [this] {
                return m_storage.ImportMemory(*m_saturn) ? "Backup memory imported."
                                                         : "The backup memory file is invalid or inaccessible.";
            });
        } else {
            const auto file = m_memoryFiles[m_selected - 2].header.filename;
            Ask("Delete " + file + "?", [this, file] {
                return m_saturn->mem.GetInternalBackupRAM().Delete(file) ? "Save deleted."
                                                                         : "Save not found.";
            });
        }
        break;
    case Page::Confirm: {
        // Return before running the action so it can change pages or resume gameplay.
        const bool accepted = m_selected == 1;
        auto action = std::move(m_confirm);
        m_confirm = {};
        SetPage(m_returnPage);
        if (accepted && action) {
            auto message = action();
            if (m_menuOpen && m_running) {
                BuildRows();
                Status(std::move(message));
            }
        }
        break;
    }
    case Page::Controls: break;
    }
}

void Frontend::Back() {
    switch (m_page) {
    case Page::Home: break;
    case Page::Game: Resume(); break;
    case Page::Images:
        if (m_directory.empty()) {
            SetPage(Page::Home);
        } else {
            Browse(m_directory == m_browserRoot ? fs::path{} : m_directory.parent_path());
        }
        break;
    case Page::Slot: SetPage(Page::Slots); break;
    case Page::Slots: SetPage(m_stateAction == StateAction::Manage ? Page::Saves : Page::Game); break;
    case Page::Memory: SetPage(Page::Saves); break;
    case Page::Confirm:
        m_confirm = {};
        SetPage(m_returnPage);
        break;
    case Page::Controls:
        SetPage(m_controlsReturnPage);
        m_selected = std::min(m_controlsReturnSelection, m_rows.empty() ? size_t{0} : m_rows.size() - 1);
        break;
    default: SetPage(m_started ? Page::Game : Page::Home); break;
    }
}

void Frontend::Input(uint32_t held, uint32_t pressed) {
    if (pressed & (ps5::Circle | ps5::Touchpad)) {
        if (m_page != Page::Home) {
            m_audio.PlayMenuSound(ps5::MenuSound::Back);
        }
        Back();
        return;
    }
    if (pressed & ps5::Triangle) {
        if (m_page == Page::Controls) {
            m_audio.PlayMenuSound(ps5::MenuSound::Back);
            Back();
        } else if (m_page != Page::Confirm) {
            m_audio.PlayMenuSound(ps5::MenuSound::Confirm);
            m_controlsReturnPage = m_page;
            m_controlsReturnSelection = m_selected;
            SetPage(Page::Controls);
        }
        return;
    }
    if (m_page == Page::Controls) {
        return;
    }
    if (m_page == Page::Home && (pressed & ps5::Square)) {
        m_audio.PlayMenuSound(ps5::MenuSound::Confirm);
        OpenLibrary();
        return;
    }
    const auto now = Clock::now();
    const bool repeatHorizontal = m_page == Page::Images || (m_page == Page::Audio && m_selected < 2);
    const uint32_t directions = ps5::Up | ps5::Down | (repeatHorizontal ? ps5::Left | ps5::Right : 0);
    const uint32_t direction = held & directions;
    if (direction != m_repeatDirection) {
        m_repeatDirection = direction;
        m_nextRepeat = now + 300ms;
    } else if (direction && now >= m_nextRepeat) {
        pressed |= direction;
        m_nextRepeat = now + 90ms;
    }

    if (!m_rows.empty()) {
        const size_t before = m_selected;
        if (m_page == Page::Images) {
            const size_t columns = ps5::kLibraryColumns;
            const size_t last = m_rows.size() - 1;
            const size_t pageSize = ps5::kLibraryPageSize;
            if ((pressed & ps5::Up) && m_selected >= columns) {
                m_selected -= columns;
            } else if ((pressed & ps5::Down) && m_selected / columns < last / columns) {
                m_selected = std::min(m_selected + columns, last);
            } else if ((pressed & ps5::Left) && m_selected % columns != 0) {
                --m_selected;
            } else if ((pressed & ps5::Right) && m_selected % columns + 1 < columns && m_selected < last) {
                ++m_selected;
            } else if ((pressed & ps5::L1) && m_selected >= pageSize) {
                m_selected -= pageSize;
            } else if ((pressed & ps5::R1) && m_selected / pageSize < last / pageSize) {
                m_selected = std::min(m_selected + pageSize, last);
            }
        } else if ((pressed & ps5::Up) && m_selected) {
            --m_selected;
        } else if ((pressed & ps5::Down) && m_selected + 1 < m_rows.size()) {
            ++m_selected;
        } else if (pressed & ps5::L1) {
            m_selected -= std::min(m_selected, ps5::kMenuVisibleRows);
        } else if (pressed & ps5::R1) {
            m_selected = std::min(m_selected + ps5::kMenuVisibleRows, m_rows.size() - 1);
        }
        if (m_selected != before) {
            m_audio.PlayMenuSound(ps5::MenuSound::Navigate);
            m_dirty = true;
        }
    }
    if (pressed & ps5::Cross) {
        Activate();
    } else if ((m_page == Page::Video || m_page == Page::Audio) && (pressed & (ps5::Left | ps5::Right))) {
        Activate(pressed & ps5::Left ? -1 : 1);
    }
}

int Frontend::Run(std::ofstream &launchLog) {
    launchLog << "Opening video" << std::endl;
    if (!m_video.Open(m_settings)) {
        launchLog << "Video initialization failed" << std::endl;
        return 1;
    }
    launchLog << m_video.Description() << std::endl;
    if (!m_pad.Open()) {
        ps5::Report("Could not open the active user's controller");
    }
    m_saturn->VDP.UseSoftwareRenderer();
    m_saturn->VDP.SetSoftwareRenderCallback({&m_video, [](uint32 *data, uint32 w, uint32 h, void *context) {
                                                 static_cast<ps5::Video *>(context)->CopyFrame(data, w, h);
                                             }});
    m_audio.SetSettings(m_audioSettings);
    if (m_audio.Open()) {
        m_saturn->SCSP.SetSampleCallback({&m_audio, [](sint16 left, sint16 right, void *context) {
                                              static_cast<ps5::Audio *>(context)->PushSample(left, right);
                                          }});
    } else {
        ps5::Report("Could not open AudioOut; continuing without audio");
    }

    auto &port = m_saturn->SMPC.GetPeripheralPort1();
    port.ConnectControlPad();
    port.SetPeripheralReportCallback({&m_saturnButtons, [](ymir::peripheral::PeripheralReport &report, void *ctx) {
                                          report.report.controlPad.buttons = MapButtons(*static_cast<uint32_t *>(ctx));
                                      }});
    OpenMenu();
    Status(m_storage.SetupError());
    ps5::Report("Running. L3+R3 or Touchpad opens the game menu");
    launchLog << "Menu initialized" << std::endl;

    uint32_t previousButtons = 0;
    auto videoDeadline = Clock::now();
    bool menuPreviouslyOpen = m_menuOpen;
    while (m_running) {
        const uint32_t buttons = m_pad.Read();
        const uint32_t pressed = buttons & ~previousButtons;
        constexpr uint32_t gameMenuButtons = ps5::L3 | ps5::R3;
        const bool gameMenuPressed = (buttons & gameMenuButtons) == gameMenuButtons &&
                                     (previousButtons & gameMenuButtons) != gameMenuButtons;
        previousButtons = buttons;

        if (m_menuOpen && !m_usbMount.valid()) {
            const auto previousState = m_driveState;
            PollMedia();
            if (previousState != m_driveState) {
                const Page activePage = m_page == Page::Confirm ? m_returnPage : m_page;
                if (activePage == Page::Slots || activePage == Page::Slot) {
                    m_confirm = {};
                    SetPage(m_stateAction == StateAction::Manage ? Page::Saves : Page::Game);
                } else if (m_page == Page::Game || m_page == Page::Saves) {
                    BuildRows();
                }
            }
            Input(buttons, pressed);
        } else if (!m_menuOpen && (gameMenuPressed || (pressed & ps5::Touchpad))) {
            OpenMenu();
            m_audio.PlayMenuSound(ps5::MenuSound::Back);
        }
        if (!m_running) {
            break;
        }

        const auto now = Clock::now();
        if (m_usbMount.valid()) {
            // The mount worker owns the Saturn core until the reader is ready.
            if (!m_usbFrameShown || m_usbMount.wait_for(0s) != std::future_status::ready) {
                m_menu.DrawDiscLoading(std::chrono::duration<double>(now - m_usbStarted).count());
                if (!m_video.PresentMenu(m_menu.Pixels(), m_settings.frameGen != 0)) {
                    ps5::Report("Display presentation failed while reading a USB disc");
                    return 1;
                }
                m_usbFrameShown = true;
                std::this_thread::sleep_until(now + 33ms);
                continue;
            }
            const auto error = m_usbMount.get();
            if (!m_video.HasGpu()) {
                BuildRows();
            }
            if (error.empty()) {
                FinishBoot(m_saturn->GetCDInterface().GetDiscHeader().gameTitle);
            } else {
                Status(error);
            }
        }

        const bool modeChanged = menuPreviouslyOpen != m_menuOpen;
        menuPreviouslyOpen = m_menuOpen;
        const double coreHz = CoreHz();
        const bool interpolate = !m_menuOpen && m_settings.frameGen && m_video.HasGpu();
        const double refresh = m_video.DisplayHz();
        const double presentationHz = (m_menuOpen || interpolate) ? std::max(refresh, 30.0) : coreHz;
        const auto period =
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1 / presentationHz));
        if (modeChanged || now - videoDeadline >= period) {
            videoDeadline = now;
        }

        const bool hadGpu = m_video.HasGpu();
        bool drawable = true;
        if (m_menuOpen) {
            UpdateCovers();
            bool menuRepainted = false;
            if (m_dirty) {
                std::vector<MenuHint> hints;
                if (m_page != Page::Controls && !m_rows.empty()) {
                    hints.push_back(
                        {MenuIcon::Cross, m_page == Page::Video || m_page == Page::Audio ? "Change" : "Select"});
                }
                if (m_page != Page::Home) {
                    hints.push_back({MenuIcon::Circle, m_page == Page::Game ? "Resume" : "Back"});
                }
                if (m_page == Page::Home) {
                    hints.push_back({MenuIcon::Square, "Game library"});
                }
                if (m_page != Page::Controls && m_page != Page::Confirm) {
                    hints.push_back({MenuIcon::Triangle, "Controls"});
                }
                if (m_page == Page::Controls) {
                    hints.push_back({MenuIcon::None, "L3 + R3 / Touchpad  Game menu"});
                } else {
                    hints.push_back({MenuIcon::DPad, "Navigate"});
                    const size_t visible = m_page == Page::Images ? ps5::kLibraryPageSize : ps5::kMenuVisibleRows;
                    if (m_rows.size() > visible) {
                        hints.push_back({MenuIcon::None, "L1 / R1  Page"});
                    }
                }
                const bool biosPresent = m_hasBIOS || !ps5::FindBIOS().empty();
                if (m_page == Page::Images) {
                    std::span<const ps5::CoverImage> covers;
                    const size_t first = m_selected / ps5::kLibraryPageSize * ps5::kLibraryPageSize;
                    if (m_libraryCovers.first == first) {
                        covers = m_libraryCovers.images;
                    }
                    m_menu.DrawLibrary(m_detail, m_rows, m_selected, covers, m_status, biosPresent, hints);
                } else {
                    m_menu.Draw(m_title, m_detail, m_rows, m_selected, m_status, biosPresent, hints,
                                m_page != Page::Controls);
                }
                m_dirty = false;
                menuRepainted = true;
            }
            // The menu is a static 1080p image. Uploading it every frame wastes
            // CPU/GPU bandwidth, so present it when it changes and at a slow
            // heartbeat to recover from compositor/device events.
            if (menuRepainted || !m_menuPresented || now - m_lastMenuPresent >= 1s) {
                drawable = m_video.PresentMenu(m_menu.Pixels(), m_settings.frameGen != 0);
                m_menuPresented = drawable;
                m_lastMenuPresent = now;
            }
        } else {
            if (buttons == 0) {
                m_waitForRelease = false;
            }
            m_saturnButtons = m_waitForRelease ? 0 : buttons;
            const auto framePeriod =
                std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1 / coreHz));
            const bool due = now + 1ms >= m_emuDeadline;
            if (due) {
                PollMedia();
                m_saturn->RunFrame();
                m_emuDeadline += framePeriod;
                if (now - m_emuDeadline >= framePeriod) {
                    m_emuDeadline = now + framePeriod;
                }
            }
            drawable = m_video.PresentGame(m_settings, due, coreHz);
        }
        if (!drawable) {
            ps5::Report("Display presentation failed");
            launchLog << "First frame presentation failed" << std::endl;
            return 1;
        }
        if (launchLog.is_open()) {
            launchLog << "First menu frame submitted" << std::endl;
            launchLog.close();
        }
        if (hadGpu && !m_video.HasGpu()) {
            if (m_menuOpen && m_page != Page::Confirm) {
                BuildRows();
                Status("Switched to VideoOut after a Vulkan error.");
            }
        }
        videoDeadline += period;
        std::this_thread::sleep_until(videoDeadline);
    }
    ps5::Report("Exit requested");
    return 0;
}

} // namespace

int main(int, char **) {
    // The shell keeps pic1.dds visible until the title releases its boot splash.
    std::ofstream launchLog("/data/YmirPS5/launch.log");
    launchLog << "YmirPS5 started" << std::endl;
    const int splashResult = sceSystemServiceHideSplashScreen();
    launchLog << "HideSplashScreen returned " << splashResult << std::endl;

    // The sandbox hides /dev, including the CAM nodes of a USB optical reader.
    // Ask the console's unlock service before this process starts any thread.
    const auto unlock = ps5::UnlockSandbox();
    launchLog << "Sandbox unlock: " << ps5::DescribeUnlock(unlock) << std::endl;
    ymir::media::host::SetCDDriveLogCallback(ReportUSB);
    try {
        return std::make_unique<Frontend>()->Run(launchLog);
    } catch (const std::exception &error) {
        ps5::Report(error.what());
        launchLog << error.what() << std::endl;
        return 1;
    }
}

// The native CRT calls this after main and would otherwise terminate with libc exit().
extern "C" [[noreturn]] void catchReturnFromMain(int) {
    ps5::Report("Returning to the PS5 home screen");
    for (;;) {
        sceSystemServiceLoadExec("exit", nullptr);
        std::this_thread::sleep_for(std::chrono::seconds{1});
    }
}
