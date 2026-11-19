// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform.hpp"

#include "menu.hpp"

#include <ymir/media/host_cd.hpp>
#include <ymir/media/loader/loader.hpp>
#include <ymir/sys/clocks.hpp>
#include <ymir/sys/saturn.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using Button = ymir::peripheral::Button;
using ymir::media::DriveState;

constexpr std::string_view kDrivePrefix = "/dev/cd";
constexpr const char *kDefaultBIOS = "/data/YmirPS5/bios.bin";

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

bool MountUSB(ymir::Saturn &saturn, std::string_view source) {
    using ymir::media::host::HostDriveInfo;
    if (source.empty()) {
        const auto drives = ymir::media::host::EnumerateHostCDDrives();
        const auto ready = std::ranges::find(drives, DriveState::MediaPresent, &HostDriveInfo::driveState);
        if (ready != drives.end() && saturn.OpenHostCDDrive(ready->path)) {
            ps5::Report(("Connected to " + ready->path).c_str());
            return true;
        }
        for (const auto &drive : drives) {
            if (saturn.OpenHostCDDrive(drive.path)) {
                ps5::Report(("Connected to " + drive.path).c_str());
                return true;
            }
        }
    } else if (saturn.OpenHostCDDrive(std::string(source))) {
        ps5::Report(("Connected to " + std::string(source)).c_str());
        return true;
    }

    ps5::Report("Cannot open USB CD drive. Check the reader and access to /dev/xpt0 and /dev/pass*");
    return false;
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

enum class Page { Home, Images, Video, Saves, Slots, Slot, Memory, Confirm };

class Frontend {
public:
    explicit Frontend(fs::path bios)
        : m_bios(std::move(bios))
        , m_storage(m_bios)
        , m_settings(m_storage.ReadVideoSettings())
        , m_saturn(std::make_unique<ymir::Saturn>()) {}

    int Run(std::string_view source);

private:
    bool EnsureBIOS();
    bool Boot(std::string_view source);
    void Resume();
    void OpenMenu();
    void PollMedia();
    double CoreHz() const;

    void SetPage(Page page);
    void BuildRows();
    void Input(uint32_t held, uint32_t pressed);
    void Activate(int delta = 1);
    void Back();
    void Browse(const fs::path &directory);
    void ChangeVideo(int direction);
    void Ask(std::string question, std::function<std::string()> action);
    void Status(std::string text);

    fs::path m_bios;
    ps5::Storage m_storage;
    ps5::VideoSettings m_settings;
    ps5::MenuCanvas m_menu;
    ps5::Video m_video;
    ps5::Pad m_pad;
    ps5::Audio m_audio;
    uint32_t m_saturnButtons = 0;
    std::unique_ptr<ymir::Saturn> m_saturn;

    bool m_hasBIOS = false;
    bool m_started = false;
    bool m_menuOpen = true;
    bool m_running = true;
    bool m_dirty = true;
    bool m_waitForRelease = false;
    DriveState m_driveState = DriveState::Unknown;
    std::string m_game;
    Clock::time_point m_emuDeadline{};

    Page m_page = Page::Home;
    Page m_returnPage = Page::Home;
    size_t m_selected = 0;
    unsigned m_slot = 1;
    std::string m_title;
    std::string m_detail;
    std::string m_status;
    std::vector<ps5::MenuRow> m_rows;
    std::function<std::string()> m_confirm;
    fs::path m_directory;
    fs::path m_browserRoot;
    std::vector<ps5::ImageEntry> m_images;
    std::vector<ymir::bup::BackupFileInfo> m_memoryFiles;
    uint32_t m_repeatDirection = 0;
    Clock::time_point m_nextRepeat{};
};

bool Frontend::EnsureBIOS() {
    if (m_hasBIOS) {
        return true;
    }
    m_hasBIOS = LoadBIOS(*m_saturn, m_bios, m_storage.BackupPath());
    if (!m_hasBIOS) {
        Status("Se necesita una BIOS Saturn de 512 KiB en " + m_bios.string());
    }
    return m_hasBIOS;
}

bool Frontend::Boot(std::string_view source) {
    if (!EnsureBIOS()) {
        return false;
    }

    if (source.empty() || source.starts_with(kDrivePrefix)) {
        if (!MountUSB(*m_saturn, source)) {
            Status("No se pudo abrir el lector USB. Comprueba la conexión y el acceso CAM.");
            return false;
        }
        m_game = "Disco USB";
    } else {
        ymir::media::Disc disc;
        if (!ymir::media::LoadDisc(fs::path(source), disc, false,
                                   [](auto, const std::string &message) { ps5::Report(message.c_str()); })) {
            Status("No se pudo abrir la imagen. Comprueba la imagen y sus archivos asociados.");
            return false;
        }
        m_saturn->LoadDisc(std::move(disc));
        m_game = fs::path(source).filename().string();
    }

    m_saturn->Reset(true);
    m_video.ClearGameFrame();
    m_driveState = DriveState::Unknown;
    m_started = true;
    Resume();
    return true;
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
    SetPage(Page::Home);
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
        m_title = "Menú principal";
        m_detail = m_started ? m_game : "Elige un disco para arrancar Sega Saturn";
        if (m_started) {
            m_rows.push_back({"Continuar", ""});
        }
        m_rows.insert(m_rows.end(), {{"Arrancar disco USB", "Lector óptico"},
                                     {"Cargar una imagen", "CHD, CUE, CCD, MDS, ISO"},
                                     {"Vídeo", m_video.Description()},
                                     {"Partidas guardadas", ""},
                                     {"Salir de Ymir", ""}});
        break;
    case Page::Images:
        m_title = "Cargar una imagen";
        m_detail = m_directory.empty() ? "Elige una carpeta de juegos" : m_directory.string();
        for (const auto &entry : m_images) {
            m_rows.push_back({m_directory.empty() ? entry.path.string() : entry.path.filename().string(),
                              entry.folder ? "Carpeta" : entry.path.extension().string()});
        }
        if (m_images.empty()) {
            m_rows.push_back({"No hay imágenes ni carpetas disponibles", "", false});
            if (m_directory.empty()) {
                m_detail = "Copia las imágenes en games, dentro de la carpeta de Ymir";
            }
        }
        break;
    case Page::Video: {
        static constexpr const char *resolutions[] = {"1080p", "1440p", "2160p"};
        static constexpr const char *aspects[] = {"4:3", "16:9", "16:10", "Píxeles cuadrados"};
        m_title = "Vídeo";
        m_detail = m_video.Description();
        m_rows = {{"Resolución de salida", resolutions[unsigned(m_settings.resolution)]},
                  {"Relación de aspecto", aspects[unsigned(m_settings.aspect)]},
                  {"AMD FSR 1", m_video.HasGpu() ? (m_settings.fsr ? "Activado" : "Desactivado") : "Requiere Vulkan",
                   m_video.HasGpu()},
                  {"Generación de fotogramas FSR 3",
                   m_video.HasGpu() ? (m_settings.frameGen ? (m_settings.frameGen == 1 ? "x2" : "x4") : "Desactivada")
                                    : "Requiere Vulkan",
                   m_video.HasGpu()}};
        break;
    }
    case Page::Saves: {
        const bool hasStates = !m_storage.StatePath(*m_saturn, 1).empty();
        m_title = "Partidas guardadas";
        m_detail = m_started ? m_game : "Memoria de Saturn y estados del emulador";
        m_rows = {{"Estados rápidos", hasStates ? "5 ranuras por disco" : "Requiere un disco identificado", hasStates},
                  {"Memoria interna de Saturn", ""}};
        break;
    }
    case Page::Slots:
        m_title = "Estados rápidos";
        m_detail = m_game;
        for (unsigned slot = 1; slot <= 5; ++slot) {
            std::error_code error;
            const bool occupied = fs::is_regular_file(m_storage.StatePath(*m_saturn, slot), error);
            m_rows.push_back({"Ranura " + std::to_string(slot), occupied ? "Guardada" : "Vacía"});
        }
        break;
    case Page::Slot: {
        std::error_code error;
        const bool occupied = fs::is_regular_file(m_storage.StatePath(*m_saturn, m_slot), error);
        m_title = "Ranura " + std::to_string(m_slot);
        m_detail = m_game;
        m_rows = {{"Guardar estado", occupied ? "Sustituir" : "Vacía"},
                  {"Cargar estado", "", occupied},
                  {"Borrar estado", "", occupied}};
        break;
    }
    case Page::Memory: {
        auto &memory = m_saturn->mem.GetInternalBackupRAM();
        m_memoryFiles = memory.List();
        std::error_code error;
        const auto transferPath = m_storage.MemoryTransferPath();
        const bool hasTransfer = !transferPath.empty() && fs::is_regular_file(transferPath, error);
        m_title = "Memoria interna";
        m_detail = "Bloques usados: " + std::to_string(memory.GetUsedBlocks()) + " de " +
                   std::to_string(memory.GetTotalBlocks());
        m_rows = {{"Exportar memoria completa", "memory-transfer.bin", !transferPath.empty()},
                  {"Importar memoria completa", "memory-transfer.bin", hasTransfer}};
        for (const auto &file : m_memoryFiles) {
            m_rows.push_back({file.header.filename, std::to_string(file.numBlocks) + " bloques · Borrar"});
        }
        break;
    }
    case Page::Confirm: m_rows = {{"Cancelar", ""}, {"Confirmar", ""}}; break;
    }
    if (!m_rows.empty()) {
        m_selected = std::min(m_selected, m_rows.size() - 1);
    }
    m_dirty = true;
}

void Frontend::Browse(const fs::path &directory) {
    std::error_code error;
    auto entries = directory.empty() ? ps5::ImageRoots(m_bios.parent_path()) : ps5::ListImages(directory, error);
    if (error) {
        Status("No se puede abrir la carpeta: " + directory.string());
        return;
    }
    if (directory.empty()) {
        m_browserRoot.clear();
    } else if (m_directory.empty()) {
        m_browserRoot = directory;
    }
    m_directory = directory;
    m_images = std::move(entries);
    SetPage(Page::Images);
}

void Frontend::ChangeVideo(int direction) {
    auto settings = m_settings;
    switch (m_selected) {
    case 0: {
        constexpr int count = int(ps5::Resolution::Count);
        settings.resolution = ps5::Resolution((int(settings.resolution) + count + direction) % count);
        if (!m_video.SetOutput(settings.resolution, settings.frameGen != 0)) {
            m_settings.resolution = m_video.OutputResolution();
            if (!m_video.HasGpu()) {
                m_settings.fsr = false;
                m_settings.frameGen = 0;
            }
            BuildRows();
            Status("La pantalla no aceptó esa resolución.");
            return;
        }
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
        if (!m_video.SetOutput(settings.resolution, settings.frameGen != 0)) {
            m_settings.resolution = m_video.OutputResolution();
            if (!m_video.HasGpu()) {
                m_settings.fsr = false;
                m_settings.frameGen = 0;
            }
            BuildRows();
            Status("La pantalla no aceptó el cambio de modo.");
            return;
        }
        break;
    }
    }
    m_settings = settings;
    const bool saved = m_storage.WriteVideoSettings(m_settings);
    BuildRows();
    if (!saved) {
        Status("Cambio aplicado para esta sesión. No se pudo guardar la configuración.");
    } else if (m_selected == 3 && m_settings.frameGen) {
        Status("La interpolación depende de los Hz de la pantalla y de los FPS reales del juego.");
    } else {
        Status("");
    }
}

void Frontend::Ask(std::string question, std::function<std::string()> action) {
    m_returnPage = m_page;
    m_confirm = std::move(action);
    SetPage(Page::Confirm);
    m_title = std::move(question);
    m_detail = "El cambio se aplica al confirmar.";
}

void Frontend::Activate(int delta) {
    if (m_rows.empty() || !m_rows[m_selected].enabled) {
        return;
    }

    switch (m_page) {
    case Page::Home: {
        if (m_started && m_selected == 0) {
            Resume();
            return;
        }
        const size_t option = m_selected - (m_started ? 1 : 0);
        switch (option) {
        case 0: Boot({}); break;
        case 1:
            m_directory.clear();
            Browse({});
            break;
        case 2: SetPage(Page::Video); break;
        case 3: SetPage(Page::Saves); break;
        case 4: m_running = false; break;
        }
        break;
    }
    case Page::Images:
        if (m_images[m_selected].folder) {
            const auto path = m_images[m_selected].path;
            Browse(path);
        } else {
            Boot(m_images[m_selected].path.string());
        }
        break;
    case Page::Video: ChangeVideo(delta); break;
    case Page::Saves:
        if (m_selected == 0) {
            SetPage(Page::Slots);
        } else if (EnsureBIOS()) {
            SetPage(Page::Memory);
        }
        break;
    case Page::Slots:
        m_slot = unsigned(m_selected) + 1;
        SetPage(Page::Slot);
        break;
    case Page::Slot: {
        constexpr const char *failed = "No se pudo guardar el estado.";
        std::error_code error;
        const auto path = m_storage.StatePath(*m_saturn, m_slot);
        if (path.empty()) {
            Status("El disco actual no está disponible.");
            break;
        }
        const bool occupied = fs::is_regular_file(path, error);
        const auto save = [this]() -> std::string {
            return m_storage.SaveState(*m_saturn, m_slot) ? "Estado guardado." : failed;
        };
        if (m_selected == 0) {
            if (occupied) {
                Ask("¿Sustituir este estado?", save);
            } else {
                const auto message = save();
                BuildRows();
                Status(message);
            }
        } else if (m_selected == 1) {
            Ask("¿Cargar este estado?", [this] {
                if (!m_storage.LoadState(*m_saturn, m_slot)) {
                    return std::string("Estado ilegible o incompatible con el disco o la BIOS actual.");
                }
                m_video.ClearGameFrame();
                Resume();
                return std::string("Estado cargado.");
            });
        } else {
            Ask("¿Borrar este estado?", [path] {
                std::error_code removalError;
                return fs::remove(path, removalError) ? "Estado borrado." : "No se pudo borrar el estado.";
            });
        }
        break;
    }
    case Page::Memory:
        if (m_selected == 0) {
            const auto path = m_storage.MemoryTransferPath();
            const auto doExport = [this, path]() -> std::string {
                return m_storage.ExportMemory(*m_saturn) ? "Memoria exportada a " + path.string()
                                                         : "No se pudo exportar la memoria.";
            };
            std::error_code error;
            if (fs::exists(path, error)) {
                Ask("¿Sustituir el archivo exportado?", doExport);
            } else {
                const auto message = doExport();
                BuildRows();
                Status(message);
            }
        } else if (m_selected == 1) {
            Ask("¿Reemplazar la memoria de Saturn?", [this] {
                return m_storage.ImportMemory(*m_saturn) ? "Memoria importada."
                                                         : "Archivo de memoria inválido o inaccesible.";
            });
        } else {
            const auto file = m_memoryFiles[m_selected - 2].header.filename;
            Ask("¿Borrar " + file + "?", [this, file] {
                return m_saturn->mem.GetInternalBackupRAM().Delete(file) ? "Partida borrada."
                                                                         : "Partida no encontrada.";
            });
        }
        break;
    case Page::Confirm: {
        const auto message = m_selected == 1 ? m_confirm() : std::string{};
        m_confirm = {};
        SetPage(m_returnPage);
        Status(message);
        break;
    }
    }
}

void Frontend::Back() {
    switch (m_page) {
    case Page::Home:
        if (m_started) {
            Resume();
        }
        break;
    case Page::Images:
        if (m_directory.empty()) {
            SetPage(Page::Home);
        } else {
            Browse(m_directory == m_browserRoot ? fs::path{} : m_directory.parent_path());
        }
        break;
    case Page::Slot: SetPage(Page::Slots); break;
    case Page::Slots:
    case Page::Memory: SetPage(Page::Saves); break;
    case Page::Confirm:
        m_confirm = {};
        SetPage(m_returnPage);
        break;
    default: SetPage(Page::Home); break;
    }
}

void Frontend::Input(uint32_t held, uint32_t pressed) {
    if (pressed & (ps5::Circle | ps5::Touchpad)) {
        Back();
        return;
    }
    const auto now = Clock::now();
    const uint32_t direction = held & (ps5::Up | ps5::Down);
    if (direction != m_repeatDirection) {
        m_repeatDirection = direction;
        m_nextRepeat = now + 300ms;
    } else if (direction && now >= m_nextRepeat) {
        pressed |= direction;
        m_nextRepeat = now + 90ms;
    }

    if (!m_rows.empty()) {
        const size_t before = m_selected;
        if ((pressed & ps5::Up) && m_selected) {
            --m_selected;
        } else if ((pressed & ps5::Down) && m_selected + 1 < m_rows.size()) {
            ++m_selected;
        } else if (pressed & ps5::L1) {
            m_selected -= std::min(m_selected, ps5::kMenuVisibleRows);
        } else if (pressed & ps5::R1) {
            m_selected = std::min(m_selected + ps5::kMenuVisibleRows, m_rows.size() - 1);
        }
        m_dirty |= m_selected != before;
    }
    if (pressed & ps5::Cross) {
        Activate();
    } else if (m_page == Page::Video && (pressed & (ps5::Left | ps5::Right))) {
        Activate(pressed & ps5::Left ? -1 : 1);
    }
}

int Frontend::Run(std::string_view source) {
    if (!m_video.Open(m_settings)) {
        return 1;
    }
    if (!m_pad.Open()) {
        ps5::Report("Could not open the active user's controller");
    }
    m_saturn->VDP.UseSoftwareRenderer();
    m_saturn->VDP.SetSoftwareRenderCallback({&m_video, [](uint32 *data, uint32 w, uint32 h, void *context) {
                                                 static_cast<ps5::Video *>(context)->CopyFrame(data, w, h);
                                             }});
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
    if (!source.empty()) {
        Boot(source);
    }
    ps5::Report("Running. Touchpad opens the menu; L3+R3 exits");

    uint32_t previousButtons = 0;
    auto videoDeadline = Clock::now();
    bool menuPreviouslyOpen = m_menuOpen;
    while (m_running) {
        const uint32_t buttons = m_pad.Read();
        const uint32_t pressed = buttons & ~previousButtons;
        previousButtons = buttons;
        constexpr uint32_t exitButtons = ps5::L3 | ps5::R3;
        if ((buttons & exitButtons) == exitButtons) {
            return 0;
        }

        if (m_menuOpen) {
            const auto previousState = m_driveState;
            PollMedia();
            if (previousState != m_driveState &&
                (m_page == Page::Saves || m_page == Page::Slots || m_page == Page::Slot ||
                 (m_page == Page::Confirm && m_returnPage == Page::Slot))) {
                m_confirm = {};
                SetPage(Page::Saves);
            }
            Input(buttons, pressed);
        } else if (pressed & ps5::Touchpad) {
            OpenMenu();
        }
        if (!m_running) {
            break;
        }

        const auto now = Clock::now();
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
            if (m_dirty) {
                m_menu.Draw(m_title, m_detail, m_rows, m_selected, m_status,
                            "Cruz  Aceptar    Círculo  Volver    Cruceta / stick  Mover    L1 / R1  Página");
                m_dirty = false;
            }
            drawable = m_video.PresentMenu(m_menu.Pixels());
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
            return 1;
        }
        if (hadGpu && !m_video.HasGpu()) {
            m_settings.resolution = m_video.OutputResolution();
            m_settings.fsr = false;
            m_settings.frameGen = 0;
            if (m_menuOpen && m_page != Page::Confirm) {
                BuildRows();
                Status("Se ha activado VideoOut tras un error de Vulkan.");
            }
        }
        videoDeadline += period;
        std::this_thread::sleep_until(videoDeadline);
    }
    return 0;
}

int Run(int argc, char **argv) {
    if (argc > 3) {
        ps5::Report("Usage: ymir-ps5.elf [saturn-bios.bin] [/dev/cdN or disc image]");
        return 1;
    }
    fs::path bios = argc >= 2 ? argv[1] : kDefaultBIOS;
    std::error_code error;
    if (argc < 2 && !fs::exists(bios, error) && fs::exists("/download0/bios.bin", error)) {
        bios = "/download0/bios.bin";
    }
    auto frontend = std::make_unique<Frontend>(std::move(bios));
    return frontend->Run(argc >= 3 ? argv[2] : "");
}

} // namespace

int main(int argc, char **argv) {
    try {
#if YMIR_PS5_NATIVE_TITLE
        (void)argc;
        (void)argv;
        return Run(0, nullptr);
#else
        return Run(argc, argv);
#endif
    } catch (const std::exception &error) {
        ps5::Report(error.what());
        return 1;
    }
}
