// SPDX-License-Identifier: GPL-3.0-or-later
// VideoOut setup and tile mapping adapted from SwanStationPS5 src/platform/ps5_video.c.

#include "platform.hpp"

#include "menu.hpp"

#if YMIR_PS5_GPU
    #include "gpu.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <immintrin.h>
#include <sys/mman.h>

namespace {

struct VideoBuffer {
    void *data;
    void *metadata;
    void *reserved0;
    void *reserved1;
};

struct alignas(8) VideoAttribute {
    uint8_t data[80];
};

extern "C" {
size_t sceKernelGetDirectMemorySize();
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t size, size_t align, int type, int64_t *physical);
int sceKernelMapDirectMemory(void **address, size_t size, int prot, int flags, int64_t physical, size_t align);
int sceKernelReleaseDirectMemory(int64_t physical, size_t size);

int sceVideoOutOpen(int user, int bus, int index, const void *param);
int sceVideoOutClose(int handle);
int sceVideoOutSetFlipRate(int handle, int rate);
void sceVideoOutSetBufferAttribute2(VideoAttribute *attribute, uint64_t format, uint32_t tiling, uint32_t width,
                                    uint32_t height, uint64_t option, uint32_t dcc, uint64_t clearColor);
int sceVideoOutRegisterBuffers2(int handle, int setIndex, int firstBuffer, VideoBuffer *buffers, int count,
                                VideoAttribute *attribute, int category, void *option);
int sceVideoOutUnregisterBuffers(int handle, int setIndex);
int sceVideoOutSubmitFlip(int handle, int buffer, uint32_t mode, int64_t arg);
int sceVideoOutIsFlipPending(int handle);
int sceVideoOutWaitVblank(int handle);
}

struct Size {
    unsigned w, h;
};

constexpr Size OutputSize(ps5::Resolution resolution) {
    constexpr std::array<Size, 3> modes{{{1920, 1080}, {2560, 1440}, {3840, 2160}}};
    return modes[unsigned(resolution)];
}

struct Rect {
    unsigned x, y, w, h;
};

Rect GameRect(Size output, Size input, ps5::Aspect aspect) {
    Size ratio;
    switch (aspect) {
    case ps5::Aspect::SixteenNine: ratio = {16, 9}; break;
    case ps5::Aspect::SixteenTen: ratio = {16, 10}; break;
    case ps5::Aspect::Source: ratio = input.w && input.h ? input : Size{4, 3}; break;
    default: ratio = {4, 3}; break;
    }
    const unsigned width = std::min(output.w, output.h * ratio.w / ratio.h);
    const unsigned height = std::min(output.h, output.w * ratio.h / ratio.w);
    return {(output.w - width) / 2, (output.h - height) / 2, width, height};
}
constexpr size_t kDirectAlign = 0x200000;
constexpr int kWriteCombinedGarlic = 3;
constexpr int kCPUGPUReadWrite = 0x33;
constexpr uint64_t kRGBA8SRGB = 0x8000000022000000ull;

bool CheckVideo(int result, const char *operation) {
    if (result >= 0) {
        return true;
    }
    char message[128];
    std::snprintf(message, sizeof(message), "%s failed (0x%08x)", operation, static_cast<unsigned>(result));
    ps5::Report(message);
    return false;
}

} // namespace

namespace ps5 {

Video::~Video() {
    Close();
}

void Video::Close() {
#if YMIR_PS5_GPU
    if (m_gpu) {
        ymir_gpu_close();
        m_gpu = false;
        return;
    }
#endif
    if (m_bufferSet >= 0) {
        for (int i = 0; i < 4 && sceVideoOutIsFlipPending(m_handle) > 0; ++i) {
            sceVideoOutWaitVblank(m_handle);
        }
        sceVideoOutUnregisterBuffers(m_handle, m_bufferSet);
        m_bufferSet = -1;
    }
    if (m_handle >= 0) {
        sceVideoOutClose(m_handle);
        m_handle = -1;
    }
    if (m_memory != nullptr) {
        munmap(m_memory, m_frameBytes * kBufferCount);
        m_memory = nullptr;
    }
    if (m_physical >= 0) {
        sceKernelReleaseDirectMemory(m_physical, m_frameBytes * kBufferCount);
        m_physical = -1;
    }
}

bool Video::OpenGPU(Resolution resolution, bool prefer120Hz) {
#if YMIR_PS5_GPU
    const auto [width, height] = OutputSize(resolution);
    // The reference presenter can pick a different resolution to get 120 Hz.
    // Try the requested size again at the default refresh when that happens.
    for (int i = 0; i < (prefer120Hz ? 2 : 1); ++i) {
        char error[256]{};
        if (!ymir_gpu_open(width, height, prefer120Hz && i == 0, error, sizeof(error))) {
            Report(error);
            continue;
        }
        if (ymir_gpu_matches(width, height)) {
            m_gpu = true;
            m_uploadPending = true;
            m_resolution = resolution;
            m_prefer120Hz = prefer120Hz;
            return true;
        }
        ymir_gpu_close();
    }
#else
    (void)resolution;
    (void)prefer120Hz;
#endif
    return false;
}

bool Video::RecoverCPU() {
    const Resolution previous = m_resolution;
    Close();
    if (OpenCPU(previous)) {
        return true;
    }
    Close();
    return previous != Resolution::HD && OpenCPU(Resolution::HD);
}

bool Video::Open(VideoSettings &settings) {
    m_resolution = settings.resolution;
    if (OpenGPU(settings.resolution, settings.frameGen != 0)) {
        return true;
    }
    if (settings.resolution != Resolution::HD && OpenGPU(Resolution::HD, settings.frameGen != 0)) {
        settings.resolution = Resolution::HD;
        return true;
    }
    settings.fsr = false;
    settings.frameGen = 0;
    if (!RecoverCPU()) {
        return false;
    }
    settings.resolution = m_resolution;
    return true;
}

bool Video::SetOutput(Resolution resolution, bool prefer120Hz) {
    if (resolution == m_resolution && prefer120Hz == m_prefer120Hz) {
        return true;
    }

    const Resolution previousResolution = m_resolution;
    const bool previousRefresh = m_prefer120Hz;
    const bool wasGpu = m_gpu;
    Close();
    if (wasGpu) {
        if (OpenGPU(resolution, prefer120Hz)) {
            return true;
        }
        if (!OpenGPU(previousResolution, previousRefresh)) {
            RecoverCPU();
        }
        return false;
    }
    if (OpenCPU(resolution)) {
        return true;
    }
    RecoverCPU();
    return false;
}

double Video::DisplayHz() const {
#if YMIR_PS5_GPU
    if (m_gpu) {
        return ymir_gpu_refresh();
    }
#endif
    return 60.0;
}

std::string Video::Description() const {
#if YMIR_PS5_GPU
    if (m_gpu) {
        return ymir_gpu_description();
    }
#endif
    return std::to_string(OutputSize(m_resolution).h) + "p · VideoOut";
}

bool Video::OpenCPU(Resolution resolution) {
    const auto [width, height] = OutputSize(resolution);
    m_handle = sceVideoOutOpen(0xff, 0, 0, nullptr);
    if (!CheckVideo(m_handle, "sceVideoOutOpen")) {
        return false;
    }

    const size_t tiles = (width / kTileSize) * ((height + kTileSize - 1) / kTileSize);
    m_frameBytes = (tiles * kTileSize * kTileSize * sizeof(uint32_t) + kDirectAlign - 1) & ~(kDirectAlign - 1);
    const size_t bytes = m_frameBytes * kBufferCount;
    int64_t physical = 0;
    if (!CheckVideo(sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, kDirectAlign,
                                                  kWriteCombinedGarlic, &physical),
                    "Video memory allocation")) {
        return false;
    }
    m_physical = physical;

    void *memory = nullptr;
    if (!CheckVideo(sceKernelMapDirectMemory(&memory, bytes, kCPUGPUReadWrite, 0, m_physical, kDirectAlign),
                    "Video memory mapping")) {
        return false;
    }
    m_memory = static_cast<uint32_t *>(memory);
    std::memset(m_memory, 0, bytes);

    // Inverse of the 64 KiB tile address mapping. Visit destination memory in order for write combining.
    for (unsigned y = 0; y < kTileSize; ++y) {
        const unsigned row = ((y << 4) & 0x70) ^ ((y << 5) & 0xf00) ^ ((y << 9) & 0x1000) ^ ((y << 8) & 0x4000);
        for (unsigned x = 0; x < kTileSize; ++x) {
            const unsigned col =
                ((x << 2) & 0x0c) ^ ((x << 5) & 0x380) ^ ((x << 4) & 0x400) ^ ((x << 6) & 0x800) ^ ((x << 9) & 0xa000);
            m_swizzle[(row ^ col) / sizeof(uint32_t)] = y * kTileSize + x;
        }
    }

    std::array<VideoBuffer, kBufferCount> buffers{};
    for (size_t i = 0; i < buffers.size(); ++i) {
        buffers[i].data = m_memory + i * (m_frameBytes / sizeof(uint32_t));
    }
    VideoAttribute attr{};
    sceVideoOutSetBufferAttribute2(&attr, kRGBA8SRGB, 0, width, height, 0, 0, 0);
    if (!CheckVideo(sceVideoOutSetFlipRate(m_handle, 0), "sceVideoOutSetFlipRate")) {
        return false;
    }
    m_bufferSet = sceVideoOutRegisterBuffers2(m_handle, 0, 0, buffers.data(), buffers.size(), &attr, 0, nullptr);
    if (!CheckVideo(m_bufferSet, "sceVideoOutRegisterBuffers2")) {
        return false;
    }
    m_resolution = resolution;
    m_prefer120Hz = false;
    m_nextBuffer = 0;
    return true;
}

void Video::CopyFrame(const uint32_t *pixels, uint32_t width, uint32_t height) {
    const std::lock_guard lock{m_frameMutex};
    m_pendingFrame.pixels.assign(pixels, pixels + size_t(width) * height);
    m_pendingFrame.width = width;
    m_pendingFrame.height = height;
    m_frameReady = true;
}

void Video::ClearGameFrame() {
    const std::lock_guard lock{m_frameMutex};
    m_pendingFrame = {};
    m_displayFrame = {};
    m_frameReady = false;
    m_uploadPending = true;
}

bool Video::PresentGame(const VideoSettings &settings, bool emulatedFrame, double coreHz) {
    bool changed = false;
    {
        const std::lock_guard lock{m_frameMutex};
        if (m_frameReady) {
            changed = !m_gpu || m_pendingFrame.width != m_displayFrame.width ||
                      m_pendingFrame.height != m_displayFrame.height || m_pendingFrame.pixels != m_displayFrame.pixels;
            std::swap(m_pendingFrame, m_displayFrame);
            m_frameReady = false;
        }
    }
    const auto &[pixels, width, height] = m_displayFrame;
#if YMIR_PS5_GPU
    if (m_gpu) {
        const auto rect = GameRect({kMenuWidth, kMenuHeight}, {width, height}, settings.aspect);
        const bool upload = !pixels.empty() && (changed || m_uploadPending);
        m_uploadPending = false;
        if (ymir_gpu_game(pixels.data(), width, height, upload, emulatedFrame, rect.x, rect.y, rect.w, rect.h,
                          settings.fsr, settings.frameGen, coreHz)) {
            return true;
        }
        Report("Vulkan could not present the game; switching to VideoOut");
        if (!RecoverCPU()) {
            return false;
        }
    }
#else
    (void)emulatedFrame;
    (void)coreHz;
#endif
    return pixels.empty() || PresentCPU(pixels, width, height, settings.aspect);
}

bool Video::PresentMenu(std::span<const uint32_t> pixels) {
#if YMIR_PS5_GPU
    if (m_gpu) {
        if (ymir_gpu_menu(pixels.data())) {
            return true;
        }
        Report("Vulkan could not present the menu; switching to VideoOut");
        if (!RecoverCPU()) {
            return false;
        }
    }
#endif
    return PresentCPU(pixels, kMenuWidth, kMenuHeight, Aspect::SixteenNine);
}

bool Video::PresentCPU(std::span<const uint32_t> pixels, unsigned width, unsigned height, Aspect aspect) {
    // Keep at most one submitted flip in the queue.
    const int pending = sceVideoOutIsFlipPending(m_handle);
    if (!CheckVideo(pending, "sceVideoOutIsFlipPending")) {
        return false;
    }
    if (pending > 0) {
        return true;
    }

    auto *destination = m_memory + size_t(m_nextBuffer) * (m_frameBytes / sizeof(uint32_t));
    const auto output = OutputSize(m_resolution);
    const Rect rect = GameRect(output, {width, height}, aspect);
    std::array<unsigned, 3840> sourceX{};
    std::array<size_t, 2160> sourceRow{};
    for (unsigned x = 0; x < rect.w; ++x) {
        sourceX[x] = x * width / rect.w;
    }
    for (unsigned y = 0; y < rect.h; ++y) {
        sourceRow[y] = size_t(y * height / rect.h) * width;
    }

    for (unsigned y = 0; y < output.h; y += kTileSize) {
        for (unsigned x = 0; x < output.w; x += kTileSize) {
            for (const unsigned pixel : m_swizzle) {
                const unsigned screenX = x + pixel % kTileSize - rect.x;
                const unsigned screenY = y + pixel / kTileSize - rect.y;
                uint32_t color = 0xff000000;
                if (screenX < rect.w && screenY < rect.h) {
                    color |= pixels[sourceRow[screenY] + sourceX[screenX]];
                }
                *destination++ = color;
            }
        }
    }

    _mm_sfence();
    if (!CheckVideo(sceVideoOutSubmitFlip(m_handle, m_nextBuffer, 1, m_flip++), "sceVideoOutSubmitFlip")) {
        return false;
    }
    m_nextBuffer = (m_nextBuffer + 1) % kBufferCount;
    return true;
}

} // namespace ps5
