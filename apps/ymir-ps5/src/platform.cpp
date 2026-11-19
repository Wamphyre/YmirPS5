// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform.hpp"

#include <cstddef>
#include <cstdio>

namespace {

// Leading fields from the scePadReadState layout used by SwanStationPS5.
// Reserve room for the rest of the record, including extension and device data.
struct alignas(8) PadState {
    uint32_t buttons;
    uint8_t leftX;
    uint8_t leftY;
    uint8_t reserved[70];
    uint8_t connected;
    uint8_t tail[435];
};
static_assert(sizeof(PadState) == 512);
static_assert(offsetof(PadState, connected) == 76);

extern "C" {
int sceKernelDebugOutText(int channel, const char *text);
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int *user);
int sceUserServiceTerminate();
int scePadInit();
int scePadOpen(int user, int type, int index, const void *params);
int scePadReadState(int handle, PadState *data);
int scePadClose(int handle);
int sceAudioOutInit();
int sceAudioOutOpen(int user, int type, int index, unsigned length, unsigned rate, unsigned format);
int sceAudioOutOutput(int handle, const void *data);
int sceAudioOutClose(int handle);
}

constexpr unsigned kSourceRate = 44100;
constexpr unsigned kOutputRate = 48000;

} // namespace

namespace ps5 {

void Report(const char *message) {
    std::fprintf(stderr, "Ymir PS5: %s\n", message);
    char line[512];
    std::snprintf(line, sizeof(line), "Ymir PS5: %s\n", message);
    sceKernelDebugOutText(0, line);
}

Pad::~Pad() {
    if (m_handle >= 0) {
        scePadClose(m_handle);
    }
    if (m_userService) {
        sceUserServiceTerminate();
    }
}

bool Pad::Open() {
    m_userService = sceUserServiceInitialize(nullptr) >= 0;
    int user = -1;
    if (sceUserServiceGetInitialUser(&user) < 0) {
        return false;
    }
    scePadInit();
    m_handle = scePadOpen(user, 0, 0, nullptr);
    return m_handle >= 0;
}

uint32_t Pad::Read() {
    PadState state{};
    if (m_handle < 0 || scePadReadState(m_handle, &state) < 0 || !state.connected) {
        return 0;
    }

    uint32_t buttons = state.buttons;
    if (state.leftX < 64) {
        buttons |= Left;
    } else if (state.leftX > 192) {
        buttons |= Right;
    }
    if (state.leftY < 64) {
        buttons |= Up;
    } else if (state.leftY > 192) {
        buttons |= Down;
    }
    return buttons;
}

Audio::~Audio() {
    {
        const std::lock_guard lock{m_mutex};
        m_stopping = true;
    }
    m_ready.notify_one();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    if (m_handle >= 0) {
        sceAudioOutClose(m_handle);
    }
}

bool Audio::Open() {
    // Port type 0 is the main output. Format 1 is interleaved stereo PCM16.
    sceAudioOutInit();
    m_handle = sceAudioOutOpen(0xff, 0, 0, kGrainSize, kOutputRate, 1);
    if (m_handle < 0) {
        return false;
    }
    m_worker = std::thread{&Audio::Playback, this};
    return true;
}

void Audio::PushSample(int16_t left, int16_t right) {
    // Keep the resampler phase continuous across emulated frames.
    constexpr unsigned inputStep = kSourceRate / 300;
    constexpr unsigned outputStep = kOutputRate / 300;
    while (m_phase < outputStep) {
        const auto interpolate = [this](int from, int to) {
            return static_cast<int16_t>(from + (to - from) * static_cast<int>(m_phase) / int(outputStep));
        };
        QueueSample(interpolate(m_previousLeft, left), interpolate(m_previousRight, right));
        m_phase += inputStep;
    }
    m_phase -= outputStep;
    m_previousLeft = left;
    m_previousRight = right;
}

void Audio::Clear() {
    m_phase = 0;
    m_pendingSamples = 0;
    m_previousLeft = 0;
    m_previousRight = 0;
    const std::lock_guard lock{m_mutex};
    m_head = 0;
    m_count = 0;
}

void Audio::QueueSample(int16_t left, int16_t right) {
    m_pending[m_pendingSamples++] = left;
    m_pending[m_pendingSamples++] = right;
    if (m_pendingSamples != m_pending.size()) {
        return;
    }
    m_pendingSamples = 0;
    {
        const std::lock_guard lock{m_mutex};
        if (m_count == m_queue.size()) {
            m_head = (m_head + 1) % m_queue.size();
            --m_count;
        }
        m_queue[(m_head + m_count) % m_queue.size()] = m_pending;
        ++m_count;
    }
    m_ready.notify_one();
}

void Audio::Playback() {
    {
        std::unique_lock lock{m_mutex};
        m_ready.wait(lock, [this] { return m_count > 0 || m_stopping; });
    }

    for (;;) {
        Grain grain{};
        {
            const std::lock_guard lock{m_mutex};
            if (m_stopping) {
                return;
            }
            if (m_count > 0) {
                grain = m_queue[m_head];
                m_head = (m_head + 1) % m_queue.size();
                --m_count;
            }
        }
        if (sceAudioOutOutput(m_handle, grain.data()) < 0) {
            Report("AudioOut stopped accepting samples");
            return;
        }
    }
}

} // namespace ps5
