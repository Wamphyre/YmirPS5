// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "storage.hpp"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace ps5 {

void Report(const char *message);

class Video {
public:
    ~Video();

    bool Open(VideoSettings &settings);
    bool SetOutput(Resolution resolution, bool prefer120Hz);
    bool HasGpu() const {
        return m_gpu;
    }
    Resolution OutputResolution() const {
        return m_resolution;
    }
    double DisplayHz() const;
    std::string Description() const;

    void CopyFrame(const uint32_t *pixels, uint32_t width, uint32_t height);
    void ClearGameFrame();
    bool PresentGame(const VideoSettings &settings, bool emulatedFrame, double coreHz);
    bool PresentMenu(std::span<const uint32_t> pixels);

private:
    bool OpenCPU(Resolution resolution);
    bool OpenGPU(Resolution resolution, bool prefer120Hz);
    bool RecoverCPU();
    void Close();
    bool PresentCPU(std::span<const uint32_t> pixels, unsigned width, unsigned height, Aspect aspect);
    static constexpr int kBufferCount = 3;
    static constexpr unsigned kTileSize = 128;

    struct Frame {
        std::vector<uint32_t> pixels;
        unsigned width = 0;
        unsigned height = 0;
    };

    bool m_gpu = false;
    bool m_prefer120Hz = false;
    Resolution m_resolution = Resolution::HD;
    size_t m_frameBytes = 0;
    bool m_uploadPending = false;
    int m_handle = -1;
    int m_bufferSet = -1;
    int m_nextBuffer = 0;
    int64_t m_flip = 1;
    int64_t m_physical = -1;
    uint32_t *m_memory = nullptr;
    std::array<uint16_t, kTileSize * kTileSize> m_swizzle{};

    std::mutex m_frameMutex;
    bool m_frameReady = false;
    Frame m_pendingFrame;
    Frame m_displayFrame;
};

enum PadButton : uint32_t {
    L3 = 0x00000002,
    R3 = 0x00000004,
    Options = 0x00000008,
    Up = 0x00000010,
    Right = 0x00000020,
    Down = 0x00000040,
    Left = 0x00000080,
    L2 = 0x00000100,
    R2 = 0x00000200,
    L1 = 0x00000400,
    R1 = 0x00000800,
    Triangle = 0x00001000,
    Circle = 0x00002000,
    Cross = 0x00004000,
    Square = 0x00008000,
    Touchpad = 0x00100000,
};

class Pad {
public:
    ~Pad();

    bool Open();
    uint32_t Read();

private:
    int m_handle = -1;
    bool m_userService = false;
};

class Audio {
public:
    ~Audio();

    bool Open();
    void PushSample(int16_t left, int16_t right);
    void Clear();

private:
    static constexpr unsigned kGrainSize = 256;
    using Grain = std::array<int16_t, kGrainSize * 2>;

    void Playback();
    void QueueSample(int16_t left, int16_t right);

    int m_handle = -1;
    unsigned m_phase = 0;
    unsigned m_pendingSamples = 0;
    int16_t m_previousLeft = 0;
    int16_t m_previousRight = 0;
    Grain m_pending{};

    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::array<Grain, 8> m_queue{};
    unsigned m_head = 0;
    unsigned m_count = 0;
    bool m_stopping = false;
    std::thread m_worker;
};

} // namespace ps5
